/* Compile-only GU declarations. Cache tests never render; unused functions are garbage-collected by the linker. */
#define GU_PSM_8888 0
#define GU_PSM_T4 1
#define GU_PSM_T8 2
#define GU_SPRITES 3
#define GU_TRIANGLE_STRIP 4
#define GU_TEXTURE_32BITF 8
#define GU_COLOR_8888 16
#define GU_VERTEX_32BITF 32
#define GU_TRANSFORM_2D 64
void sceGuClutMode(int, int, int, int);
void sceGuClutLoad(int, const void *);
void sceGuTexMode(int, int, int, int);
void sceGuTexImage(int, int, int, int, const void *);
void sceGuDrawArray(int, int, int, const void *, const void *);
