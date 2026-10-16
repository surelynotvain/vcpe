/* Native PSP savedata. Links vcpe_savedata vcpe_save vcpe_gfx vcpe_sys and -lpsputility.
 * Call between frames, after gfx init, on the main thread. Operations block while the firmware utility runs;
 * HOME/power callbacks remain active. No local save is changed or removed. */
#ifndef VCPE_SAVEDATA_H
#define VCPE_SAVEDATA_H
#include <stdint.h>
typedef struct {
    const char *game_id;       /* unique nine-character DISC_ID, e.g. UCJS19050; never change after release */
    const char *slot;          /* 1..19 uppercase letters/digits/underscore, e.g. SLOT00 */
    const char *title;         /* game title shown in XMB */
    const char *save_title;    /* slot title shown in XMB */
    const char *detail;
    const char *icon0;         /* PNG next to EBOOT; NULL: extract embedded ICON0 from EBOOT.PBP */
    int dialog;               /* 0: automatic; 1: firmware save/load confirmation UI */
} VcpeSavedata;
enum { VCPE_SAVEDATA_INVALID = -1, VCPE_SAVEDATA_MEMORY = -2, VCPE_SAVEDATA_CORRUPT = -3,
       VCPE_SAVEDATA_BUSY = -4 };
/* 0: success. Negative engine or firmware error (including cancellation); output untouched on failure.
 * Envelope checks payload size, game-provided format version and CRC. Files appear under
 * ms0:/PSP/SAVEDATA/<game_id><slot>/ with PARAM.SFO, ICON0.PNG (if available), DATA.BIN. */
int vcpe_savedata_write(const VcpeSavedata *save, const void *data, uint32_t size, uint32_t version);
int vcpe_savedata_read(const VcpeSavedata *save, void *data, uint32_t cap, uint32_t *size, uint32_t version);
/* Explicit import from existing vcpe_save two-slot files, preserving both originals. */
int vcpe_savedata_import(const VcpeSavedata *save, const char *local_name, uint32_t cap, uint32_t version);
#endif
