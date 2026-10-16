#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_diag.h"
static uint32_t ring[256], cost[VCPE_COST_COUNT], last[VCPE_COST_COUNT], cursor, count, worst, failures;
static int loading;
static struct { char name[24]; uint32_t count, bytes; } failed[16];
void vcpe_diag_cost(int d,uint32_t n)
{
    if (d>=0 && d<VCPE_COST_COUNT) cost[d]=UINT32_MAX-cost[d]<n?UINT32_MAX:cost[d]+n;
}
void vcpe_diag_failure(const char *name,uint32_t bytes)
{
    failures++;
    if (!name) name="unknown";
    for (int i=0;i<16;i++) if (!failed[i].name[0] || !strncmp(failed[i].name,name,23)) {
        snprintf(failed[i].name,sizeof failed[i].name,"%s",name); failed[i].count++; failed[i].bytes=bytes; return;
    }
}
void vcpe_diag_frame(uint32_t us)
{
    memcpy(last,cost,sizeof last); memset(cost,0,sizeof cost);
    if (loading || !us) return;
    ring[cursor++&255]=us; if (count<256) count++;
    if (us>worst) worst=us;
}
void vcpe_diag_loading(int active) { loading=active!=0; memset(cost,0,sizeof cost); }
void vcpe_diag_reset(void)
{
    memset(ring,0,sizeof ring); memset(cost,0,sizeof cost); memset(last,0,sizeof last); memset(failed,0,sizeof failed);
    cursor=count=worst=failures=0; loading=0;
}
static int cmp(const void *a,const void *b) { uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b; return x<y?-1:x>y; }
void vcpe_diag_stats(VcpeDiagStats *out)
{
    uint32_t sorted[256]; uint64_t sum=0;
    for (uint32_t i=0;i<count;i++) { sorted[i]=ring[i]; sum+=ring[i]; }
    if (count>1) qsort(sorted,count,sizeof *sorted,cmp);
    memset(out,0,sizeof *out); out->samples=count; out->worst_us=worst; out->failures=failures;
    if (count) {
        out->average_us=(uint32_t)(sum/count); out->p95_us=sorted[(count*95+99)/100-1];
        out->p99_us=sorted[(count*99+99)/100-1];
    }
    memcpy(out->last_cost,last,sizeof last);
}
int vcpe_diag_line(char *out,int n)
{
    VcpeDiagStats s; vcpe_diag_stats(&s);
    return snprintf(out,n,"frames avg %u p95 %u p99 %u worst %u us, CPU %u GPU-wait %u IO %u us, audio %u, alloc fails %u",
        (unsigned)s.average_us,(unsigned)s.p95_us,(unsigned)s.p99_us,(unsigned)s.worst_us,(unsigned)s.last_cost[0],(unsigned)s.last_cost[1],(unsigned)s.last_cost[2],(unsigned)s.last_cost[3],(unsigned)s.failures);
}
int vcpe_diag_failure_line(int i,char *out,int n)
{
    return i>=0 && i<16 && failed[i].name[0] ? snprintf(out,n,"%s: %u allocation failures, last %u bytes",
        failed[i].name,(unsigned)failed[i].count,(unsigned)failed[i].bytes) : 0;
}
