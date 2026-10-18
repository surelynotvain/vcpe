#include "vcpe_skin.h"
static float half_float(uint16_t h)
{
 unsigned sign=(unsigned)(h&0x8000)<<16,exponent=(h>>10)&31,mantissa=h&1023;
 union {uint32_t u;float f;} v;
 if(!exponent){
  if(!mantissa){v.u=sign;return v.f;}
  int shift=0;while(!(mantissa&1024)){mantissa<<=1;shift++;}
  v.u=sign|((unsigned)(113-shift)<<23)|((mantissa&1023)<<13);return v.f;
 }
 v.u=sign|((exponent==31?255:exponent+112)<<23)|(mantissa<<13);return v.f;
}
int vcpe_skin_pose(const VcpeSkinClip *c,float seconds,int loop,float out[VCPE_SKIN_MAX_BONES][12])
{
 if(!c||!c->keys||c->frames<1||c->bones<1||c->bones>VCPE_SKIN_MAX_BONES||c->duration<=0)return -1;
 float t=seconds;
 if(loop){t-=c->duration*(int)(t/c->duration);if(t<0)t+=c->duration;}
 else {if(t<0)t=0;if(t>c->duration)t=c->duration;}
 float f=t*(c->frames-1)/c->duration;int a=(int)f,b=a+1<c->frames?a+1:a;float k=f-a;
 int first=a*c->bones*12,last=b*c->bones*12;
 if(c->half){
  const uint16_t *keys=c->keys;
  for(int i=0;i<c->bones;i++)for(int j=0;j<12;j++){
   int p=i*12+j;float x=half_float(keys[first+p]),y=half_float(keys[last+p]);out[i][j]=x+(y-x)*k;
  }
 }else{
  const float *x=(const float *)c->keys+first,*y=(const float *)c->keys+last;
  for(int i=0;i<c->bones;i++)for(int j=0;j<12;j++)out[i][j]=x[i*12+j]+(y[i*12+j]-x[i*12+j])*k;
 }
 return 0;
}
#ifdef __PSP__
static int accelerate;
void vcpe_skin_accelerate(int on){accelerate=on!=0;}
static int skin_vfpu(VcpeVtx *out,const VcpeSkinVertex *bind,int count,const float (*pose)[12],int bones)
{
 /* C000: homogeneous bind position. C300: weighted output. Matrix rows are
  * 16-byte aligned; bind vertices and output need only 4-byte alignment.
  * All source weights/bone checks match the scalar path. */
 for(int i=0;i<count;i++){
  __asm__ volatile("lv.s S000,0(%0)\n lv.s S001,4(%0)\n lv.s S002,8(%0)\n vone.s S003\n vzero.t C300\n"::"r"(bind[i].xyz):"memory");
  for(int j=0;j<4;j++){
   float w=bind[i].weight[j];if(w<=0)continue;
   unsigned b=bind[i].bone[j];if(b>=(unsigned)bones)return -1;
   __asm__ volatile(
    "lv.q C100,0(%0)\n lv.q C110,16(%0)\n lv.q C120,32(%0)\n lv.s S210,0(%1)\n"
    "vdot.q S200,C100,C000\n vdot.q S201,C110,C000\n vdot.q S202,C120,C000\n"
    "vscl.t C200,C200,S210\n vadd.t C300,C300,C200\n"
    ::"r"(pose[b]),"r"(&bind[i].weight[j]):"memory");
  }
  __asm__ volatile("sv.s S300,0(%0)\n sv.s S301,4(%0)\n sv.s S302,8(%0)\n"::"r"(&out[i].x):"memory");
 }
 return 0;
}
#else
void vcpe_skin_accelerate(int on){(void)on;}
#endif
int vcpe_skin_apply(VcpeVtx *out,const VcpeSkinVertex *bind,int count,const float (*pose)[12],int bones)
{
 if(!out||!bind||!pose||count<0||bones<1||bones>VCPE_SKIN_MAX_BONES)return -1;
#ifdef __PSP__
 if(accelerate&&!((uintptr_t)pose&15))return skin_vfpu(out,bind,count,pose,bones);
#endif
 for(int i=0;i<count;i++){
  float result[3]={0,0,0};
  for(int j=0;j<4;j++){
   float w=bind[i].weight[j];if(w<=0)continue;
   unsigned b=bind[i].bone[j];if(b>=(unsigned)bones)return -1;
   const float *m=pose[b],*v=bind[i].xyz;
   for(int k=0;k<3;k++)result[k]+=(m[4*k]*v[0]+m[4*k+1]*v[1]+m[4*k+2]*v[2]+m[4*k+3])*w;
  }
  out[i].x=result[0];out[i].y=result[1];out[i].z=result[2];
 }
 return 0;
}

#include <math.h>
/* Add a source-targeted head look on top of the source clip, preserving the
 * head pivot and applying the same rotation to its descendant bones. */
int vcpe_skin_aim(float (*pose)[12],int count,int head,const unsigned char *mask,
                  const float bind[12],const float target[3],float max_radians)
{
 if(!pose||!mask||!bind||!target||head<0||head>=count||count>VCPE_SKIN_MAX_BONES)return -1;
 float h[12],pivot[3],from[3],to[3],axis[3];
 for(int i=0;i<3;i++)for(int j=0;j<4;j++){
  float v=j==3?pose[head][i*4+3]:0;
  for(int k=0;k<3;k++)v+=pose[head][i*4+k]*bind[k*4+j];
  h[i*4+j]=v;
 }
 float f2=0,t2=0;
 for(int i=0;i<3;i++){pivot[i]=h[4*i+3];from[i]=-h[4*i+2];to[i]=target[i]-pivot[i];f2+=from[i]*from[i];t2+=to[i]*to[i];}
 if(f2<1e-12f||t2<1e-12f)return 0;
 float fi=1/sqrtf(f2),ti=1/sqrtf(t2),dot=0;
 for(int i=0;i<3;i++){from[i]*=fi;to[i]*=ti;dot+=from[i]*to[i];}
 axis[0]=from[1]*to[2]-from[2]*to[1];axis[1]=from[2]*to[0]-from[0]*to[2];axis[2]=from[0]*to[1]-from[1]*to[0];
 float a2=axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2];if(a2<1e-12f)return 0;
 float sine=sqrtf(a2),angle=atan2f(sine,dot);if(angle>max_radians)angle=max_radians;
 for(int i=0;i<3;i++)axis[i]/=sine;
 float c=cosf(angle),s=sinf(angle),v=1-c,x=axis[0],y=axis[1],z=axis[2];
 float r[9]={c+x*x*v,x*y*v-z*s,x*z*v+y*s,y*x*v+z*s,c+y*y*v,y*z*v-x*s,z*x*v-y*s,z*y*v+x*s,c+z*z*v};
 for(int b=0;b<count;b++)if(mask[b]){
  float out[12];
  for(int i=0;i<3;i++)for(int j=0;j<4;j++){
   float sum=0;for(int k=0;k<3;k++)sum+=r[i*3+k]*(pose[b][k*4+j]-(j==3?pivot[k]:0));
   out[i*4+j]=sum+(j==3?pivot[i]:0);
  }
  for(int j=0;j<12;j++)pose[b][j]=out[j];
 }
 return 0;
}
