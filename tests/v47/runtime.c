#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../runtime/w_world.c"
#include "../../runtime/w_phys.c"
#include "../../src/vcpe_store.h"
const BlobDef g_worlds[4]={{0}};
const ACtrl g_actrls[1]={{0}};
int g_autotest,g_prof_hier;
long long sceKernelGetSystemTimeWide(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000ll+t.tv_nsec/1000; }
uint32_t sceKernelGetSystemTimeLow(void) { return (uint32_t)sceKernelGetSystemTimeWide(); }
int vcpe_step_begin(const char *fmt,...) { (void)fmt; return -1; }
void vcpe_step_end(int id,int ok) { (void)id;(void)ok; }
void hooks_unload(void) {} void hooks_instantiate(int o) { (void)o; }
int hook_anim_new(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
void anim_recapture(int o) { (void)o; }
const char *lang_static(const char *s) { return s; }
static uint64_t digest;
static void hash(const void *p,size_t n) { const uint8_t *b=p; while(n--) { digest^=*b++; digest*=1099511628211ull; } }
void g_trigger_enter(int a,int b,int c) { hash(&a,4);hash(&b,4);hash(&c,4); }
void g_collision_enter(int a,int b,float c) { hash(&a,4);hash(&b,4);hash(&c,4); }
static void make_obj(int i)
{
    Obj *o=&W.obj[i]; memset(o,0,sizeof *o);
    o->alive=o->active=o->ah=1; o->parent=-1; o->rect=o->canvas=o->rb=-1; o->lsx=o->lsy=1;
}
static uint64_t simulation(int grid,uint64_t *checks)
{
    phys_world_free(); free_world(); memset(&W,0,sizeof W);
    enum { N=256 };
    W.nobj=W.ncol=W.capcol=N; W.nrb=W.caprb=64; W.nwall=1;
    W.col=calloc(N,sizeof *W.col); W.rbd=calloc(64,sizeof *W.rbd); W.wall=calloc(1,sizeof *W.wall);
    W.wall[0]=(WWall){.go=-1,.layer=0,.x0=-10,.x1=30,.y0=-1,.y1=0};
    for(int i=0;i<N;i++) {
        make_obj(i); W.obj[i].lx=(i%16)*1.4f; W.obj[i].ly=(i/16)*1.3f;
        W.col[i].d=(WCol){.go=i,.kind=i%3?COL_BOX:COL_CIRCLE,.flags=WCOL_ENABLED|((i%7==0)?WCOL_TRIGGER:0),.a=.9f,.b=.9f};
        if(i<64) { W.obj[i].rb=i; W.rbd[i]=(WRb){.go=i,.type=RB_DYNAMIC,.sim=1,.mass=1,.grav=.2f}; }
    }
    w_build_children(); phys_world_init(); phys_broadphase(grid); digest=1469598103934665603ull; *checks=0;
    for(int i=0;i<64;i++) phys_set_velocity(i,((i%3)-1)*.7f,-.2f);
    for(int frame=0;frame<120;frame++) {
        if(frame==35) { W.obj[70].active=W.obj[70].ah=0; phys_dirty(); }
        if(frame==60) { W.obj[70].active=W.obj[70].ah=1; phys_dirty(); }
        w_transforms(); phys_step(1.0f/60); *checks+=(unsigned)phys_stat_candidates;
        for(int i=0;i<64;i++) { hash(&W.obj[i].lx,8); hash(&bodies[i],sizeof bodies[i]); }
    }
    uint64_t result=digest; phys_world_free(); free_world(); memset(&W,0,sizeof W); return result;
}
int main(void)
{
    uint64_t achecks,bchecks; uint64_t a=simulation(0,&achecks), b=simulation(1,&bchecks);
    assert(a==b && bchecks<achecks/2);
    printf("physics: identical 120-step state/contact hash %llx; candidate checks %llu -> %llu\n",
        (unsigned long long)a,(unsigned long long)achecks,(unsigned long long)bchecks);
    W.nobj=3; for(int i=0;i<3;i++) make_obj(i);
    W.obj[1].parent=0; W.obj[1].lx=2; W.obj[2].parent=1; W.obj[2].lx=3;
    w_build_children(); w_transforms(); int rebuilt=w_transform_rebuilds;
    for(int i=0;i<100;i++) w_transforms();
    assert(w_transform_rebuilds==rebuilt && W.obj[2].m[4]==5);
    W.obj[0].lx=7; w_transforms(); assert(W.obj[2].m[4]==12 && w_transform_rebuilds==rebuilt+3);
    obj_set_pos(1,20,0); obj_matrix(2,W.obj[2].m); assert(W.obj[2].m[4]==23);
    W.obj[2].parent=0; w_transforms(); assert(W.obj[2].m[4]==10);
    printf("transforms: 100 static frames rebuilt zero matrices (%d cache hits); direct writes, setters and reparenting passed\n",w_transform_hits);
    free_world(); memset(&W,0,sizeof W);
    assert(reserve_for(WSEC('P','A','R','T'),360)==0);
    assert(w_component_reserve(WSEC('P','A','R','T'),7)==0 && reserve_for(WSEC('P','A','R','T'),360)==7);
    nbudgets=0;
    struct { WSection s; WGo go[2]; } objects={.s={.tag=WSEC('G','O','B','J'),.count=2}};
    struct { WSection s; WScript scripts[2]; } scripts={.s={.tag=WSEC('S','C','R','P'),.count=2}};
    for(int i=0;i<2;i++) objects.go[i]=(WGo){.parent=i?0:-1,.rect=-1,.sr=-1,.img=-1,.text=-1,.anim=-1,.rb=-1,
        .sgrp=-1,.canvas=-1,.tilemap=-1,.trail=-1,.line=-1,.part=-1,.audio=-1,.col_first=-1,.script_first=0,
        .nscript=i?0:2,.active=1,.lsx=1,.lsy=1};
    prefab_blob[0].mem=(void *)1; prefab_blob[0].nsec=2;
    prefab_blob[0].tags[0]=objects.s.tag; prefab_blob[0].sec[0]=&objects.s;
    prefab_blob[0].tags[1]=scripts.s.tag; prefab_blob[0].sec[1]=&scripts.s;
    W.script=calloc(2,sizeof *W.script); W.capscript=2;
    assert(w_instantiate(0,1,2)==0 && W.nobj==2 && W.nscript==2);
    assert(W.script[0].remap==W.script[1].remap && W.script[0].remap[1]==1);
    assert(w_instantiate(0,3,4)==-1 && W.nobj==2);
    free_world(); memset(&W,0,sizeof W); memset(prefab_blob,0,sizeof prefab_blob);
    VcpeStore store; assert(vcpe_store_init(&store,sizeof(uint64_t),4)==0);
    uint64_t *records[100]; uint32_t ids[100];
    for(int i=0;i<100;i++) { void *r; assert(vcpe_store_add(&store,&ids[i],&r)==0); records[i]=r; *records[i]=(uint64_t)i+5; }
    for(int i=0;i<100;i++) assert(vcpe_store_get(&store,ids[i])==records[i] && *records[i]==(uint64_t)i+5);
    for(int i=4;i<100;i++) vcpe_store_remove(&store,ids[i]);
    assert(vcpe_store_trim(&store)>0 && *records[0]==5 && *records[3]==8);
    vcpe_store_free(&store);
    puts("component budgets, shared prefab maps, atomic capacity failure and stable-address paged growth passed");
    free(cg); free(pairs_cur); free(pairs_prev);
}
