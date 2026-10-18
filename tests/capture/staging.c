#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../src/vcpe_test.c"
static uint8_t framebuffer[512*272*4],output[480*272*4];
static unsigned offset,calls,partial,fail;
int sceIoWrite(int fd,const void *data,unsigned size)
{
 assert(fd==3);const uint8_t *p=data;
 assert((uintptr_t)p<(uintptr_t)framebuffer || (uintptr_t)p>=(uintptr_t)framebuffer+sizeof framebuffer);
 calls++;if(fail&&calls==3)return 0;
 if(partial&&size>997)size=997;
 assert(offset+size<=sizeof output);memcpy(output+offset,data,size);offset+=size;return (int)size;
}
static void compare(void){
 assert(offset==sizeof output);
 for(int y=0;y<272;y++)assert(!memcmp(output+y*480*4,framebuffer+y*512*4,480*4));
}
int main(void){
 for(unsigned i=0;i<sizeof framebuffer;i++)framebuffer[i]=(uint8_t)(i*37+(i>>10));
 assert(write_frame(3,framebuffer,512)==0);assert(calls==34);compare();
 offset=calls=0;partial=1;assert(write_frame(3,framebuffer,512)==0);compare();
 offset=calls=0;partial=0;fail=1;assert(write_frame(3,framebuffer,512)==-1&&calls==3);
 puts("PASS capture staging: no I/O from framebuffer memory, exact row cropping, short-write retry, error exit");
}
