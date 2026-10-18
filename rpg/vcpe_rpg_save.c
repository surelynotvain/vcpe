/* VCPE RPG saves: the whole RpgGame (it holds no pointers) run-length compressed behind a small header that the file
 * list shows (play time, the party's walking graphics, the map's name), written through vcpe_save's two-slot files.
 * After loading, the map document is opened again and the derived tables rebuilt (rpg_map_reopen). */
#include "vcpe_rpg.h"
#include "vcpe_save.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SAVE_VERSION 0x52504701u   /* "RPG" 1 */
void (*rpg_save_name_hook)(int slot, char *out, int n);
int rpg_map_reopen(void);
static int last_slot = -1;

static void name(int slot, char *out, int n)
{
    if (rpg_save_name_hook)
        rpg_save_name_hook(slot, out, n);
    else
        snprintf(out, n, "SAVE%02d.DAT", slot + 1);
}

/* zero runs: 0x00 then a 16-bit count; other bytes as they are */
static uint32_t pack(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n;) {
        if (in[i] == 0) {
            uint32_t r = 0;
            while (i + r < n && in[i + r] == 0 && r < 65535)
                r++;
            out[o++] = 0;
            out[o++] = r & 255;
            out[o++] = r >> 8;
            i += r;
        } else {
            out[o++] = in[i++];
        }
    }
    return o;
}

static int unpack(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n;) {
        if (in[i] == 0) {
            if (i + 2 >= n)
                return -1;
            uint32_t r = in[i + 1] | in[i + 2] << 8;
            if (o + r > cap)
                return -1;
            memset(out + o, 0, r);
            o += r;
            i += 3;
        } else {
            if (o >= cap)
                return -1;
            out[o++] = in[i++];
        }
    }
    return o == cap ? 0 : -1;
}

static void make_header(RpgSaveHeader *h)
{
    memset(h, 0, sizeof *h);
    h->magic = SAVE_VERSION;
    h->frame_count = rpg.frame_count;
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
    for (int i = 0; i < n && i < 4; i++) {
        RpgActorState *a = rpg_actor(ids[i]);
        snprintf(h->chars[i], RPG_NAME, "%s", a->character_name);
        h->char_index[i] = a->character_index;
    }
    h->nchars = n < 4 ? n : 4;
    const RpgMap *m = rpg_map();
    snprintf(h->map_name, RPG_NAME, "%s", m ? rpg_s(&rpg_mapdoc, m->display_name) : "");
}

int rpg_save_write(int slot)
{
    if (rpg_hooks.save_extra)
        rpg_hooks.save_extra();
    rpg.save_count++;
    rpg.bgm_on_save = rpg_bgm_current();
    rpg.bgm_on_save_pos = rpg_bgm_pos();
    rpg.bgs_on_save = rpg_bgs_current();
    uint32_t cap = sizeof(RpgSaveHeader) + sizeof rpg * 3 / 2 + 64;
    uint8_t *buf = malloc(cap);
    if (!buf)
        return -1;
    RpgSaveHeader h;
    make_header(&h);
    memcpy(buf, &h, sizeof h);
    uint32_t n = sizeof h + pack((const uint8_t *)&rpg, sizeof rpg, buf + sizeof h);
    char f[64];
    name(slot, f, sizeof f);
    int r = vcpe_save_write_var(f, buf, n, SAVE_VERSION ^ (uint32_t)sizeof rpg);
    free(buf);
    if (!r)
        last_slot = slot;
    rpg_log("RPG save %d: %u bytes, %s", slot, (unsigned)n, r ? "failed" : "ok");
    return r;
}

int rpg_save_header(int slot, RpgSaveHeader *h)
{
    char f[64];
    name(slot, f, sizeof f);
    uint32_t cap = sizeof(RpgSaveHeader) + sizeof rpg * 3 / 2 + 64, size = 0;
    uint8_t *buf = malloc(cap);
    if (!buf)
        return -1;
    int r = vcpe_save_read_var(f, buf, cap, &size, SAVE_VERSION ^ (uint32_t)sizeof rpg);
    if (!r && size >= sizeof *h)
        memcpy(h, buf, sizeof *h);
    free(buf);
    return r || size < sizeof *h ? -1 : 0;
}

int rpg_save_read(int slot)
{
    char f[64];
    name(slot, f, sizeof f);
    uint32_t cap = sizeof(RpgSaveHeader) + sizeof rpg * 3 / 2 + 64, size = 0;
    uint8_t *buf = malloc(cap);
    RpgGame *tmp = malloc(sizeof *tmp);
    if (!buf || !tmp) {
        free(buf);
        free(tmp);
        return -1;
    }
    int r = vcpe_save_read_var(f, buf, cap, &size, SAVE_VERSION ^ (uint32_t)sizeof rpg);
    if (!r)
        r = unpack(buf + sizeof(RpgSaveHeader), size - sizeof(RpgSaveHeader), (uint8_t *)tmp, sizeof *tmp);
    if (!r) {
        memcpy(&rpg, tmp, sizeof rpg);
        rpg_map_reopen();
        if (rpg_hooks.load_extra)
            rpg_hooks.load_extra();
        last_slot = slot;
    }
    free(buf);
    free(tmp);
    rpg_log("RPG load %d: %s", slot, r ? "failed" : "ok");
    return r;
}

int rpg_saves_exist(void)
{
    char f[64];
    for (int i = 0; i < RPG_SAVE_FILES; i++) {
        name(i, f, sizeof f);
        if (vcpe_save_exists_var(f, SAVE_VERSION ^ (uint32_t)sizeof rpg))
            return 1;
    }
    return 0;
}

int rpg_save_last_slot(void) { return last_slot; }
