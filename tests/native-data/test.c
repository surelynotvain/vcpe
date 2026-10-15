#include "vcpe_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
int main(int argc,char **argv){assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);unsigned n=ftell(f);rewind(f);unsigned char *b=malloc(n);assert(fread(b,1,n,f)==n);fclose(f);VcpeData d;assert(vcpe_data_open(&d,b,n)==0);VcpeValue r=vcpe_data_root(&d);assert(vcpe_data_int(vcpe_data_get(r,"negative"))==-123);assert(!strcmp(vcpe_data_string(vcpe_data_get(r,"unicode")),"A \xe3\x81\x82"));VcpeValue a=vcpe_data_get(r,"array");assert(vcpe_data_count(a)==3);assert(vcpe_data_number(vcpe_data_at(a,1))==1.25);assert(vcpe_data_kind(vcpe_data_at(a,-1))==VCPE_D_NULL);assert(vcpe_data_kind(vcpe_data_at(a,3))==VCPE_D_NULL);assert(vcpe_data_kind(vcpe_data_get(r,"missing"))==VCPE_D_NULL);uint32_t z;assert(!memcmp(vcpe_data_bytes(vcpe_data_get(r,"blob"),&z),"\0\1\2",3)&&z==3);for(unsigned len=0;len<n;len++)assert(vcpe_data_open(&d,b,len)<0);b[12]=0xff;b[13]=0xff;b[14]=0xff;b[15]=0x7f;assert(vcpe_data_open(&d,b,n)<0);free(b);puts("VDAT bounds, types, Unicode and truncation passed");}
