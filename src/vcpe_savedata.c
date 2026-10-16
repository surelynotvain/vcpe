/* Firmware owns PARAM.SFO and presentation; the engine owns the checked payload envelope.
 * Source: PSPSDK src/utility/psputility_savedata.h and psputility.h.
 * https://github.com/pspdev/pspsdk/tree/master/src/utility */
#include <pspkernel.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_sys.h"
#include "vcpe_gfx.h"
#include "vcpe_save.h"
#include "vcpe_savedata.h"
#include "vcpe_mem.h"

#define MAX_PAYLOAD (16u * 1024 * 1024)
typedef struct { char magic[4]; uint32_t version, size, crc; } Envelope;
static int busy;

static int valid_name(const char *s, int min, int max)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < (size_t)min || n > (size_t)max) return 0;
    for (; *s; s++)
        if (!(*s >= 'A' && *s <= 'Z') && !(*s >= '0' && *s <= '9') && *s != '_') return 0;
    return 1;
}

/* ICON0 is optional. Bound the allocation and never read outside a malformed PBP section. */
static void *load_icon(const char *name, uint32_t *size)
{
    char path[512];
    vcpe_path(path, sizeof path, name ? name : "EBOOT.PBP");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t off = 0, n = 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return 0; }
    rewind(f);
    if (name) n = end <= 256 * 1024 ? (uint32_t)end : 0;
    else {
        uint32_t h[10];
        if (fread(h, 1, sizeof h, f) == sizeof h && h[0] == 0x50425000 &&
            h[3] >= sizeof h && h[4] > h[3] && h[4] <= (uint32_t)end) {
            off = h[3]; n = h[4] - off;
        }
    }
    void *buf = n && n <= 256 * 1024 ? malloc(n) : 0;
    if (buf && (fseek(f, off, SEEK_SET) || fread(buf, 1, n, f) != n)) { free(buf); buf = 0; }
    fclose(f);
    if (buf) *size = n;
    return buf;
}

static void utility_update(void) { sceUtilitySavedataUpdate(1); }

static int transfer(const VcpeSavedata *s, void *buf, uint32_t cap, uint32_t *size, int writing)
{
    SceUtilitySavedataParam p;
    memset(&p, 0, sizeof p);
    p.base.size = sizeof p;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &p.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &p.base.buttonSwap);
    p.base.graphicsThread = 17; p.base.accessThread = 19;
    p.base.fontThread = 18; p.base.soundThread = 16;
    p.mode = writing ? (s->dialog ? PSP_UTILITY_SAVEDATA_SAVE : PSP_UTILITY_SAVEDATA_AUTOSAVE) :
                       (s->dialog ? PSP_UTILITY_SAVEDATA_LOAD : PSP_UTILITY_SAVEDATA_AUTOLOAD);
    p.overwrite = 1;
    snprintf(p.gameName, sizeof p.gameName, "%s", s->game_id);
    snprintf(p.saveName, sizeof p.saveName, "%s", s->slot);
    snprintf(p.fileName, sizeof p.fileName, "DATA.BIN");
    p.dataBuf = buf; p.dataBufSize = cap; p.dataSize = writing ? *size : 0;
    /* Firmware 2+ full parameter layout requires a nonzero key. This is a stable format key, not a secret;
     * do not tie it to the engine release or existing saves would stop loading after an upgrade. */
    static const uint8_t format_key[16] = {0x56,0x43,0x50,0x45,0x53,0x41,0x56,0x45,0x31,0x37,0x29,0x51,0x63,0x2A,0x7C,0x19};
    memcpy(p.key, format_key, sizeof p.key);
    p.secureVersion = 1;
    snprintf(p.sfoParam.title, sizeof p.sfoParam.title, "%s", s->title ? s->title : s->game_id);
    snprintf(p.sfoParam.savedataTitle, sizeof p.sfoParam.savedataTitle, "%s", s->save_title ? s->save_title : s->slot);
    snprintf(p.sfoParam.detail, sizeof p.sfoParam.detail, "%s", s->detail ? s->detail : "");
    uint32_t icon_size = 0;
    void *icon = writing ? load_icon(s->icon0, &icon_size) : 0;
    p.icon0FileData.buf = icon; p.icon0FileData.bufSize = p.icon0FileData.size = icon_size;
    vcpe_gfx_present();
    int yielded = vcpe_volatile_suspend();
    if (yielded < 0) { free(icon); return VCPE_SAVEDATA_BUSY; }
    int result = sceUtilitySavedataInitStart(&p);
    if (result >= 0) {
        int shutting = 0, status_error = 0;
        /* The request keeps pointers into this stack. Always finish its shutdown before returning. */
        for (;;) {
            int status = sceUtilitySavedataGetStatus();
            if (status < 0) { status_error = status; break; }
            if (status == PSP_UTILITY_DIALOG_NONE || status == PSP_UTILITY_DIALOG_FINISHED) break;
            if (status == PSP_UTILITY_DIALOG_QUIT && !shutting) {
                sceUtilitySavedataShutdownStart(); shutting = 1;
            }
            vcpe_gfx_begin(0xFF000000);
            vcpe_gfx_end_overlay(status == PSP_UTILITY_DIALOG_VISIBLE ? utility_update : 0);
            vcpe_gfx_present();
            sceKernelDelayThread(1000);
        }
        result = status_error ? status_error : p.base.result;
        if (!result && !writing) *size = p.dataSize;
    }
    if (yielded && vcpe_volatile_resume() < 0)
        vcpe_fatal("Savedata memory", "Unable to reclaim volatile memory after the PSP savedata utility.");
    free(icon);
    return result;
}

static int validate(const VcpeSavedata *s, const void *data, uint32_t n)
{
    if (!s || !data || n > MAX_PAYLOAD || !valid_name(s->game_id, 9, 9) || !valid_name(s->slot, 1, 19))
        return VCPE_SAVEDATA_INVALID;
    return busy || vcpe_gfx_in_frame() ? VCPE_SAVEDATA_BUSY : 0;
}

int vcpe_savedata_write(const VcpeSavedata *s, const void *data, uint32_t n, uint32_t version)
{
    int r = validate(s, data, n);
    if (r) return r;
    uint32_t bytes = sizeof(Envelope) + n;
    Envelope *e = malloc(bytes);
    if (!e) return VCPE_SAVEDATA_MEMORY;
    *e = (Envelope){{'V','S','D','5'}, version, n, vcpe_crc32(data, n, 0)};
    memcpy(e + 1, data, n);
    busy = 1; r = transfer(s, e, bytes, &bytes, 1); busy = 0;
    free(e);
    return r;
}

int vcpe_savedata_read(const VcpeSavedata *s, void *data, uint32_t cap, uint32_t *size, uint32_t version)
{
    int r = validate(s, data, cap);
    if (r || !size) return r ? r : VCPE_SAVEDATA_INVALID;
    uint32_t bytes = 0, capacity = sizeof(Envelope) + cap;
    Envelope *e = calloc(1, capacity);
    if (!e) return VCPE_SAVEDATA_MEMORY;
    busy = 1; r = transfer(s, e, capacity, &bytes, 0); busy = 0;
    if (!r) {
        if (bytes < sizeof *e || bytes > capacity || memcmp(e->magic, "VSD5", 4) || e->version != version ||
            e->size > cap || e->size != bytes - sizeof *e || e->crc != vcpe_crc32(e + 1, e->size, 0))
            r = VCPE_SAVEDATA_CORRUPT;
        else { memcpy(data, e + 1, e->size); *size = e->size; }
    }
    free(e);
    return r;
}

int vcpe_savedata_import(const VcpeSavedata *s, const char *name, uint32_t cap, uint32_t version)
{
    if (!name || cap > MAX_PAYLOAD) return VCPE_SAVEDATA_INVALID;
    void *data = malloc(cap ? cap : 1);
    if (!data) return VCPE_SAVEDATA_MEMORY;
    uint32_t size = 0;
    int r = vcpe_save_read_var(name, data, cap, &size, version);
    if (!r) r = vcpe_savedata_write(s, data, size, version);
    free(data);
    return r;
}
