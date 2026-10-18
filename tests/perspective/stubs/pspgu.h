#ifndef VCPE_PERSPECTIVE_GU_STUB
#define VCPE_PERSPECTIVE_GU_STUB
enum {GU_DEPTH_TEST=100,GU_CULL_FACE,GU_FOG,GU_ALPHA_TEST,GU_GREATER,GU_CLAMP,GU_TEXTURE_2D,GU_TRIANGLES};
#define GU_TEXTURE_32BITF 1
#define GU_COLOR_8888 2
#define GU_VERTEX_32BITF 4
#define GU_TRANSFORM_2D 8
void sceGuDisable(int);void sceGuEnable(int);void sceGuAlphaFunc(int,int,int);
void sceGuTexWrap(int,int);void sceGuDrawArray(int,int,int,const void *,const void *);
#endif
