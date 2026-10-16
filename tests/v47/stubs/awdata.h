#ifndef AWDATA_H
#define AWDATA_H
#include <stdint.h>
#define NUM_PREFABS 2
#define NUM_SCENES 2
#define PREFAB_BASE 2
#define NUM_TEXTFILES 0
#define NUM_PARAMS 4
#define NUM_SCRIPT_TYPES 2
#define NUM_SPRITES 4
#define STR_OFF 0
#define STR_SIZE 4
#define SPR_NONE 0xFFFF
typedef struct { int16_t ox,oy; uint16_t w,h; int16_t rx,ry,rw,rh; } SprDef;
typedef struct { uint32_t off,size; } BlobDef;
typedef struct { uint16_t npath; } ACtrl;
extern const SprDef g_sprites[];
extern const BlobDef g_worlds[];
extern const ACtrl g_actrls[];
#endif
