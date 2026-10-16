#ifndef TEST_PSPGUM_H
#define TEST_PSPGUM_H
#include <stdint.h>
typedef struct { float x,y,z; } ScePspFVector3;
typedef struct { float x,y,z,w; } ScePspFVector4;
typedef struct { ScePspFVector4 x,y,z,w; } ScePspFMatrix4;
void sceGumMatrixMode(int); void sceGumLoadIdentity(void);
void sceGumPerspective(float,float,float,float);
void sceGumRotateX(float); void sceGumRotateY(float); void sceGumRotateZ(float);
void sceGumTranslate(const ScePspFVector3 *); void sceGumLoadMatrix(const ScePspFMatrix4 *);
void sceGumDrawArray(int,int,int,const void *,const void *);
#endif
