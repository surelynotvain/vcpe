#include "vcpe_data.h"
#include <string.h>
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static const uint8_t *range(const VcpeData *d,uint32_t off,uint32_t n){return d&&d->base&&off<=d->size&&n<=d->size-off?d->base+off:0;}
static const uint8_t *node(VcpeValue v){return v.off>=16?range(v.doc,v.off,12):0;}
static VcpeValue nil(const VcpeData *d){VcpeValue v={d,0};return v;}
int vcpe_data_open(VcpeData *d,const void *bytes,uint32_t size){memset(d,0,sizeof(*d));if(!bytes||size<28)return -1;const uint8_t *p=bytes;if(memcmp(p,"VDAT",4)||u32(p+4)!=1||u32(p+8)!=size)return -1;d->base=p;d->size=size;d->root=u32(p+12);if(!range(d,d->root,12)){memset(d,0,sizeof(*d));return -1;}return 0;}
VcpeValue vcpe_data_root(const VcpeData *d){VcpeValue v={d,d?d->root:0};return v;}
int vcpe_data_kind(VcpeValue v){const uint8_t *p=node(v);return p?(int)u32(p):VCPE_D_NULL;}
int vcpe_data_count(VcpeValue v){const uint8_t *p=node(v);return p?(int)u32(p+4):0;}
int32_t vcpe_data_int(VcpeValue v){const uint8_t *p=node(v);return p&&u32(p)==VCPE_D_INT?(int32_t)u32(p+8):0;}
double vcpe_data_number(VcpeValue v){const uint8_t *p=node(v);if(!p)return 0;if(u32(p)==VCPE_D_INT)return (int32_t)u32(p+8);const uint8_t *q=range(v.doc,u32(p+8),8);double d=0;if(u32(p)==VCPE_D_FLOAT&&q)memcpy(&d,q,8);return d;}
const char *vcpe_data_string(VcpeValue v){const uint8_t *p=node(v);if(!p||u32(p)!=VCPE_D_STRING)return "";uint32_t n=u32(p+4);if(n==UINT32_MAX)return "";const uint8_t *s=range(v.doc,u32(p+8),n+1);return s&&s[n]==0?(const char*)s:"";}
const void *vcpe_data_bytes(VcpeValue v,uint32_t *size){const uint8_t *p=node(v);if(size)*size=0;if(!p||u32(p)!=VCPE_D_BYTES)return 0;const uint8_t *q=range(v.doc,u32(p+8),u32(p+4));if(q&&size)*size=u32(p+4);return q;}
VcpeValue vcpe_data_at(VcpeValue v,int i){const uint8_t *p=node(v);if(!p||u32(p)!=VCPE_D_ARRAY||i<0||(uint32_t)i>=u32(p+4))return nil(v.doc);uint32_t off=u32(p+8),count=u32(p+4);if(count>UINT32_MAX/4)return nil(v.doc);const uint8_t *q=range(v.doc,off,count*4);if(!q)return nil(v.doc);VcpeValue r={v.doc,u32(q+i*4)};return node(r)?r:nil(v.doc);}
VcpeValue vcpe_data_get(VcpeValue v,const char *key){const uint8_t *p=node(v);if(!p||!key||u32(p)!=VCPE_D_OBJECT)return nil(v.doc);uint32_t count=u32(p+4);if(count>UINT32_MAX/8)return nil(v.doc);const uint8_t *q=range(v.doc,u32(p+8),count*8);if(!q)return nil(v.doc);uint32_t lo=0,hi=count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;VcpeValue k={v.doc,u32(q+m*8)};if(vcpe_data_kind(k)!=VCPE_D_STRING)return nil(v.doc);int c=strcmp(key,vcpe_data_string(k));if(c==0){VcpeValue r={v.doc,u32(q+m*8+4)};return node(r)?r:nil(v.doc);}if(c<0)hi=m;else lo=m+1;}return nil(v.doc);}

VcpeValue vcpe_data_member(VcpeValue v,int i){const uint8_t *p=node(v);if(!p||u32(p)!=VCPE_D_OBJECT||i<0||(uint32_t)i>=u32(p+4))return nil(v.doc);uint32_t count=u32(p+4);if(count>UINT32_MAX/8)return nil(v.doc);const uint8_t *q=range(v.doc,u32(p+8),count*8);if(!q)return nil(v.doc);VcpeValue r={v.doc,u32(q+i*8+4)};return node(r)?r:nil(v.doc);}
