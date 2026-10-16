#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../../src/vcpe_3d.c"
#include "../../src/vcpe_font.c"
#include "../../src/vcpe_node3d.c"
static int binds,draws,counts[32];static const uint16_t *index_ptr[32];
void vcpe_gfx_flush(void){} int vcpe_bind_page(int page){(void)page;binds++;return 1;}
void vcpe_unbind(void){} void vcpe_filter(int v){(void)v;}void vcpe_blend(int v){(void)v;}void vcpe_tex_white(int v){(void)v;}
void vcpe_page_size(int p,int *w,int *h){(void)p;*w=*h=128;}
void *vcpe_gfx_mem(int n){static VcpeVtx storage[256];assert(n<=(int)sizeof storage);return storage;}
void sceGuEnable(int x){(void)x;}void sceGuDisable(int x){(void)x;}
void sceGuDepthBuffer(void *p,int x){(void)p;(void)x;}void sceGuDepthRange(int a,int b){(void)a;(void)b;}
void sceGuDepthFunc(int a){(void)a;}void sceGuClearDepth(int a){(void)a;}void sceGuClear(int a){(void)a;}void sceGuDepthMask(int a){(void)a;}
void sceGuTexWrap(int a,int b){(void)a;(void)b;}void sceGuAlphaFunc(int a,int b,int c){(void)a;(void)b;(void)c;}
void sceGuTexFilter(int a,int b){(void)a;(void)b;}void sceGuTexLevelMode(int a,float b){(void)a;(void)b;}
void sceGuFog(float a,float b,unsigned int c){(void)a;(void)b;(void)c;}
void sceGuBlendFunc(int a,int b,int c,unsigned int d,unsigned int e){(void)a;(void)b;(void)c;(void)d;(void)e;}
void sceGumMatrixMode(int a){(void)a;}void sceGumLoadIdentity(void){}
void sceGumPerspective(float a,float b,float c,float d){(void)a;(void)b;(void)c;(void)d;}
void sceGumRotateX(float a){(void)a;}void sceGumRotateY(float a){(void)a;}void sceGumRotateZ(float a){(void)a;}
void sceGumTranslate(const ScePspFVector3 *a){(void)a;}void sceGumLoadMatrix(const ScePspFMatrix4 *a){(void)a;}
void sceGumDrawArray(int mode,int flags,int n,const void *idx,const void *v)
{(void)mode;(void)flags;(void)v;assert(draws<32);index_ptr[draws]=idx;counts[draws++]=n;}
static void clear_calls(void){draws=binds=0;}
int main(void)
{
    float pos[3]={0},rot[3]={0};
    VcpeVtx v[15]={0};uint16_t idx[120];
    /* Five chunks, eight triangles each. Two adjacent visible chunks coalesce; outside chunks never reach GE. */
    for(int group=0;group<5;group++)for(int k=0;k<3;k++) {
        int i=group*3+k;v[i].x=group==0||group==3?100.0f:(k==0?-.1f:.1f);v[i].y=k==2?.1f:-.1f;v[i].z=-5;
    }
    for(int group=0;group<5;group++)for(int i=0;i<24;i++)idx[group*24+i]=(uint16_t)(group*3+i%3);
#ifdef NO_CULL_MODULE
    assert(vcpe_3d_prepare(v,15,idx,120)==-1);
    vcpe_3d_begin(pos,rot,90,1,100);vcpe_3d_draw(3,v,idx,120);
    assert(draws==1 && counts[0]==120 && vcpe_stat_tris==40);
    puts("PASS 3D compatibility: no culling module links and preserves the complete draw");return 0;
#else
    vcpe_cull_limits(65536,8);assert(!vcpe_3d_prepare(v,15,idx,120));
    vcpe_3d_begin(pos,rot,90,1,100);clear_calls();vcpe_3d_draw(3,v,idx,120);
    float sphere[3]={0,0,-5};
    assert(vcpe_3d_visible(sphere,1));
    sphere[0]=100;assert(!vcpe_3d_visible(sphere,1));
    sphere[0]=0;sphere[2]=0;assert(vcpe_3d_visible(sphere,1)); /* tangent to near plane */
    uint32_t nan_bits=0x7FC00000u;memcpy(&sphere[2],&nan_bits,sizeof nan_bits);
    assert(vcpe_3d_visible(sphere,1)); /* inconclusive: retain, including fast-math callers */
    assert(draws==2 && binds==1 && counts[0]==48 && index_ptr[0]==idx+24 && counts[1]==24 && index_ptr[1]==idx+96);
    assert(vcpe_stat_tris==24 && vcpe_stat_cull_tris==16);
    for(int group=0;group<5;group++)for(int k=0;k<3;k++)v[group*3+k].x=group<2?-100:100;
    vcpe_3d_geometry_changed(v);clear_calls();vcpe_3d_draw(3,v,idx,120);assert(!draws && !binds);
    /* Camera rotation, model movement, no origin-only test, and no texture bind for rejected whole mesh. */
    float model[16]={1,0,0,200,0,1,0,0,0,0,1,0,0,0,0,1};
    vcpe_3d_model(model);clear_calls();vcpe_3d_draw(3,v,idx,120);assert(!draws && !binds);
    vcpe_3d_model(0);rot[1]=180;vcpe_3d_begin(pos,rot,90,1,100);clear_calls();vcpe_3d_draw(3,v,idx,120);assert(!draws && !binds);
    rot[1]=0;vcpe_3d_begin(pos,rot,90,1,100);
    for(int i=0;i<15;i++)v[i].x=200;
    vcpe_3d_geometry_changed(v);clear_calls();vcpe_3d_draw(3,v,idx,120);assert(!draws && !binds);
    vcpe_3d_autocull(0);clear_calls();vcpe_3d_draw(3,v,idx,120);assert(draws==1 && counts[0]==120);
    vcpe_3d_autocull(1);vcpe_3d_cull_reset();clear_calls();vcpe_3d_draw(3,v,idx,120);assert(draws==1 && counts[0]==120);
    static const VcpeGlyph glyphs[]={{'A',0,0,8,10,-3,-2,9}};
    VcpeFont font={3,12,1,glyphs};
    model[3]=200;model[11]=-5;clear_calls();vcpe_font_draw3d(&font,model,"AA\nA",.1f,0xFFFFFFFF,1);
    assert(!draws && !binds);
    model[3]=0;clear_calls();vcpe_font_draw3d(&font,model,"AA\nA",.1f,0xFFFFFFFF,1);assert(draws==1 && binds==1 && counts[0]==18);
    struct {
        VcpeVtx vertices[3];uint16_t indices[3] __attribute__((aligned(16)));
    } geo={0};
    for(int i=0;i<3;i++){geo.vertices[i].x=120+(i==0?-.1f:.1f);geo.vertices[i].y=i==2?.1f:-.1f;geo.indices[i]=(uint16_t)i;}
    VcpeNodeDef node={.parent=-1,.mesh=0,.base=-1,.visible=1,.layers=1,.pos={-120,0,-5},.scale={1,1,1}};
    VcpeMeshDef mesh={0,1,{120,0,0,1}};
    VcpeBatch batch={.page=3,.voff=0,.nverts=3,.ioff=offsetof(__typeof__(geo),indices),.nidx=3};
    VcpeScene3D scene={.nodes=&node,.nnodes=1,.meshes=&mesh,.batches=&batch,.geo=(const uint8_t *)&geo};
    vcpe_node3d_load(&scene);vcpe_node3d_update(0);clear_calls();vcpe_node3d_draw(1);
    assert(draws==1 && counts[0]==3); /* origin is offscreen; actual mesh is inside */
    vcpe_node3d_pos(0)[0]=120;vcpe_node3d_update(0);clear_calls();vcpe_node3d_draw(1);assert(!draws);
    vcpe_node3d_pos(0)[0]=-120;vcpe_node3d_update(0);
    for(int i=0;i<3;i++)geo.vertices[i].x+=200;
    vcpe_3d_geometry_changed(geo.vertices);clear_calls();vcpe_node3d_draw(1);assert(!draws);
    for(int i=0;i<3;i++)geo.vertices[i].x-=200;
    vcpe_3d_geometry_changed(geo.vertices);clear_calls();vcpe_node3d_draw(1);assert(draws==1);
    puts("PASS scene/labels: automatic batch registration, offscreen origins, moved nodes, deformed geometry, current multiline labels");
    vcpe_3d_cull_reset();
    puts("PASS 3D submission: hidden ranges omitted, adjacent ranges coalesced in order, no hidden bind, camera/model changes, dynamic refit, bypass and uncached fallback");
#endif
}
