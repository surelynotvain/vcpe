/* Synthetic generated-data interface for runtime tests; no game data. */
#ifndef AWDATA_H
#define AWDATA_H
#include <stdint.h>
typedef struct { int16_t ox, oy; uint16_t w, h; int16_t rx, ry, rw, rh; } SprDef;
extern const SprDef g_sprites[];
#define SPR_NONE 0xFFFF
#endif
