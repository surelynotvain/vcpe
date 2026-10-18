/* All rasterization is GU_TRANSFORM_2D. No 3D module or depth storage needed. */
#include "vcpe_perspective.h"
#include <pspgu.h>
int vcpe_perspective_draw_ex(VcpePerspective *p,unsigned flags)
{
    if(!p || !p->valid || !vcpe_gfx_in_frame()) return 0;
    vcpe_perspective_sort(p); vcpe_gfx_flush();
    sceGuDisable(GU_DEPTH_TEST); sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FOG); vcpe_opaque((flags & VCPE_PERSPECTIVE_OPAQUE)!=0);
    sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER,0x20,0xff);
    sceGuTexWrap(GU_CLAMP,GU_CLAMP);
    vcpe_blend(0); vcpe_filter(1); vcpe_tex_white(0);
    unsigned drawn=0;
    for(unsigned first=0;first<p->count;) {
        int page=p->triangles[first].page;
        unsigned end=first+1;
        while(end<p->count && p->triangles[end].page==page && end-first<64) end++;
        int w=1,h=1;
        if(page>=0) {
            if(!vcpe_bind_page(page)) { p->dropped+=end-first;first=end;continue; }
            vcpe_page_size(page,&w,&h); sceGuEnable(GU_TEXTURE_2D);
        } else { vcpe_unbind();sceGuDisable(GU_TEXTURE_2D); }
        VcpeVtx *v=vcpe_gfx_mem((int)((end-first)*3*sizeof *v));
        if(!v) {p->dropped+=p->count-first;break;}
        for(unsigned i=first;i<end;i++) for(int j=0;j<3;j++) {
            *v=p->triangles[i].v[j];v->u*=w;v->v*=h;v++;
        }
        v-=(end-first)*3;
        sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D,
                       (int)((end-first)*3),0,v);
        vcpe_stat_draws++;
        drawn+=end-first; first=end;
    }
    sceGuDisable(GU_ALPHA_TEST);sceGuEnable(GU_TEXTURE_2D);vcpe_unbind();vcpe_opaque(0);
    p->submitted=drawn; return (int)drawn;
}
int vcpe_perspective_draw(VcpePerspective *p) {return vcpe_perspective_draw_ex(p,0);}
