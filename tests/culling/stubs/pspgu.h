#ifndef TEST_PSPGU_H
#define TEST_PSPGU_H
#define GU_ADD 1
#define GU_ALPHA_TEST 2
#define GU_BLEND 3
#define GU_CLAMP 4
#define GU_CLIP_PLANES 5
#define GU_COLOR_8888 6
#define GU_CULL_FACE 7
#define GU_DEPTH_BUFFER_BIT 8
#define GU_DEPTH_TEST 9
#define GU_FALSE 10
#define GU_FIX 11
#define GU_FOG 12
#define GU_GEQUAL 13
#define GU_GREATER 14
#define GU_INDEX_16BIT 15
#define GU_MODEL 16
#define GU_NEAREST 17
#define GU_NEAREST_MIPMAP_NEAREST 18
#define GU_PI 3.14159265358979323846f
#define GU_PROJECTION 20
#define GU_REPEAT 21
#define GU_TEXTURE_2D 22
#define GU_TEXTURE_32BITF 23
#define GU_TEXTURE_AUTO 24
#define GU_TRANSFORM_3D 25
#define GU_TRIANGLES 26
#define GU_TRUE 27
#define GU_VERTEX_32BITF 28
#define GU_VIEW 29
void sceGuEnable(int);
void sceGuDisable(int);
void sceGuDepthBuffer(void *, int);
void sceGuDepthRange(int, int);
void sceGuDepthFunc(int);
void sceGuClearDepth(int);
void sceGuClear(int);
void sceGuDepthMask(int);
void sceGuTexWrap(int,int);
void sceGuAlphaFunc(int,int,int);
void sceGuTexFilter(int,int);
void sceGuTexLevelMode(int,float);
void sceGuFog(float,float,unsigned int);
void sceGuBlendFunc(int,int,int,unsigned int,unsigned int);
#define GU_SRC_ALPHA 99
#define GU_ONE_MINUS_SRC_ALPHA 99
#endif
