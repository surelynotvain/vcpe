#include <assert.h>
#include <stdio.h>
#include "vcpe_diag.h"
#include "vcpe_adapt.h"
int main(void)
{
    for(int i=1;i<=100;i++)vcpe_diag_frame((uint32_t)i*1000);
    VcpeDiagStats s;vcpe_diag_stats(&s);
    assert(s.samples==100&&s.average_us==50500&&s.p95_us==95000&&s.p99_us==99000&&s.worst_us==100000);
    vcpe_diag_loading(1);vcpe_diag_frame(900000);vcpe_diag_loading(0);vcpe_diag_stats(&s);assert(s.samples==100);
    vcpe_diag_failure("physics",4096);vcpe_diag_stats(&s);assert(s.failures==1);
    VcpeAdaptConfig c;vcpe_adapt_defaults(&c);c.target_us=10000;
    assert(!vcpe_adapt_config(&c));vcpe_adapt_enable(1);
    for(int i=0;i<500;i++){vcpe_adapt_cost(VCPE_COST_CPU,20000);vcpe_adapt_sample(30000,8192);}
    assert(vcpe_adapt_stats()->scale256==256&&vcpe_adapt_stats()->bottleneck==VCPE_BOTTLENECK_CPU);
    vcpe_adapt_enable(1);
    for(int i=0;i<500;i++){vcpe_adapt_cost(VCPE_COST_IO,20000);vcpe_adapt_sample(30000,8192);}
    assert(vcpe_adapt_stats()->scale256==256&&vcpe_adapt_stats()->bottleneck==VCPE_BOTTLENECK_IO);
    vcpe_adapt_enable(1);
    for(int i=0;i<500;i++){vcpe_adapt_cost(VCPE_COST_GPU,20000);vcpe_adapt_sample(30000,8192);}
    assert(vcpe_adapt_stats()->scale256==128&&vcpe_adapt_stats()->bottleneck==VCPE_BOTTLENECK_RENDER);
    vcpe_adapt_enable(1);
    for(int i=0;i<500;i++){vcpe_adapt_cost(VCPE_COST_AUDIO,1);vcpe_adapt_sample(30000,8192);}
    assert(vcpe_adapt_stats()->scale256==256&&vcpe_adapt_stats()->bottleneck==VCPE_BOTTLENECK_AUDIO);
    puts("diagnostics: exact p95/p99/worst/failure reporting; CPU, I/O and audio pressure preserve quality; rendering pressure scales detail");
}
