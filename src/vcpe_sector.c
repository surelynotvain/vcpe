#include "vcpe_sector.h"
#include <string.h>
#include <math.h>
static int finite_value(float f)
{ uint32_t bits;memcpy(&bits,&f,4);return (bits&0x7f800000u)!=0x7f800000u && f>-100000 && f<100000; }
static int inside(const VcpeSectorCell *c,float x,float z)
{ return x>=c->x0 && x<=c->x1 && z>=c->z0 && z<=c->z1; }
static int side(const VcpeSectorCell *c,const VcpeSectorPortal *p)
{
    if(p->x0==p->x1 && p->z0>=c->z0 && p->z1<=c->z1) {
        if(p->x0==c->x0)return 0;
        if(p->x0==c->x1)return 1;
    }
    if(p->z0==p->z1 && p->x0>=c->x0 && p->x1<=c->x1) {
        if(p->z0==c->z0)return 2;
        if(p->z0==c->z1)return 3;
    }
    return -1;
}
int vcpe_sector_validate(const VcpeSectors *s)
{
    if(!s || !s->cells || s->count<1 || s->count>64 || s->portal_count<0 ||
       s->portal_count>256 || (s->portal_count && !s->portals))return -1;
    for(int i=0;i<s->count;i++) {
        const VcpeSectorCell *c=&s->cells[i];
        if(!finite_value(c->x0)||!finite_value(c->x1)||!finite_value(c->z0)||!finite_value(c->z1)||
           !finite_value(c->floor)||!finite_value(c->ceiling)||c->x0>=c->x1||c->z0>=c->z1||c->floor>=c->ceiling)return -1;
        for(int j=0;j<i;j++) {
            const VcpeSectorCell *d=&s->cells[j];
            if(c->x0<d->x1 && c->x1>d->x0 && c->z0<d->z1 && c->z1>d->z0)return -1;
        }
    }
    for(int i=0;i<s->portal_count;i++) {
        const VcpeSectorPortal *p=&s->portals[i];
        if(p->a<0||p->b<0||p->a>=s->count||p->b>=s->count||p->a==p->b ||
           !finite_value(p->x0)||!finite_value(p->x1)||!finite_value(p->z0)||!finite_value(p->z1)||
           p->x0>p->x1||p->z0>p->z1||(p->x0==p->x1 && p->z0==p->z1))return -1;
        int a=side(&s->cells[p->a],p),b=side(&s->cells[p->b],p);
        if(a<0||b!=(a^1))return -1;
    }
    return 0;
}
uint64_t vcpe_sector_connected(const VcpeSectors *s,int cell)
{
    if(!s || s->count<1 || s->count>64 || cell<0 || cell>=s->count || s->portal_count<0 ||
       s->portal_count>256 || (s->portal_count && !s->portals))return 0;
    uint64_t mask=UINT64_C(1)<<cell,old;
    do {
        old=mask;
        for(int i=0;i<s->portal_count;i++) {
            const VcpeSectorPortal *p=&s->portals[i];
            if(!p->open || p->a<0 || p->b<0 || p->a>=s->count || p->b>=s->count)continue;
            uint64_t bits=(UINT64_C(1)<<p->a)|(UINT64_C(1)<<p->b);
            if(mask&bits)mask|=bits;
        }
    } while(mask!=old);
    return mask;
}
static int passable(const VcpeSectors *s,const VcpeSectorBody *b,int other,int crossing)
{
    const VcpeSectorCell *c=&s->cells[b->cell],*d=&s->cells[other];
    float floor=c->floor>d->floor?c->floor:d->floor;
    float ceiling=c->ceiling<d->ceiling?c->ceiling:d->ceiling;
    return (!crossing || (d->floor-c->floor<=b->step_up && c->floor-d->floor<=b->step_down)) && ceiling-floor>=b->height;
}
static int opening(const VcpeSectors *s,const VcpeSectorBody *b,int wall,float x,float z,int crossing,int *dest)
{
    for(int i=0;i<s->portal_count;i++) {
        const VcpeSectorPortal *p=&s->portals[i];
        int other=p->a==b->cell?p->b:p->b==b->cell?p->a:-1;
        if(!p->open || other<0 || side(&s->cells[b->cell],p)!=wall || !passable(s,b,other,crossing))continue;
        float at=wall<2?z:x,lo=wall<2?p->z0:p->x0,hi=wall<2?p->z1:p->x1;
        if(at>=lo+b->radius && at<=hi-b->radius) {*dest=other;return 1;}
    }
    return 0;
}
static int attempt(const VcpeSectors *s,VcpeSectorBody *b,float x,float z)
{
    const VcpeSectorCell *c=&s->cells[b->cell];
    float distances[4]={x-c->x0,c->x1-x,z-c->z0,c->z1-z};
    int dest=b->cell;
    for(int wall=0;wall<4;wall++) if(distances[wall]<b->radius) {
        int other;
        if(!opening(s,b,wall,x,z,distances[wall]<0,&other))return 0;
        if(distances[wall]<0)dest=other;
    }
    if(!inside(&s->cells[dest],x,z))return 0;
    /* A short adjoining cell must not let the body's radius protrude through its
     * opposite wall. Check destination boundaries too, in both movement directions. */
    if(dest!=b->cell) {
        VcpeSectorBody next=*b;next.cell=dest;c=&s->cells[dest];
        float d[4]={x-c->x0,c->x1-x,z-c->z0,c->z1-z};
        for(int wall=0;wall<4;wall++)if(d[wall]<b->radius) {
            int ignored;if(!opening(s,&next,wall,x,z,0,&ignored))return 0;
        }
    }
    b->x=x;b->z=z;b->cell=dest;b->y=s->cells[dest].floor;return 1;
}
int vcpe_sector_move(const VcpeSectors *s,VcpeSectorBody *b,float dx,float dz)
{
    if(!b || vcpe_sector_validate(s)<0 || b->cell<0 || b->cell>=s->count ||
       !finite_value(dx)||!finite_value(dz)||!finite_value(b->x)||!finite_value(b->z)||
       !finite_value(b->radius)||!finite_value(b->height)||!finite_value(b->step_up)||!finite_value(b->step_down)||
       b->radius<.01f||b->height<=0||b->step_up<0||b->step_down<0 ||
       !inside(&s->cells[b->cell],b->x,b->z) ||
       s->cells[b->cell].ceiling-s->cells[b->cell].floor<b->height)return -1;
    float distance=fabsf(dx)+fabsf(dz),step=b->radius*.4f;
    if(distance>step*128)return -1;
    int n=(int)(distance/step)+1,moved=0;
    dx/=n;dz/=n;
    for(int i=0;i<n;i++) {
        if(dx) moved|=attempt(s,b,b->x+dx,b->z);
        if(dz) moved|=attempt(s,b,b->x,b->z+dz);
    }
    return moved;
}
