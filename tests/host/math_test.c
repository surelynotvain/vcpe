/* host test for vcpe_math: table trig, atan2, rng ranges */
#include <math.h>
#include <stdio.h>
#include "vcpe_math.h"
int main(void)
{
    vcpe_math_init();
    double es = 0, ea = 0;
    for (int i = -200000; i <= 200000; i++) {
        float r = i * 0.0001f;
        double e = fabs(vcpe_sin(r) - sin(r)); if (e > es) es = e;
        e = fabs(vcpe_cos(r) - cos(r)); if (e > es) es = e;
        float y = sinf(i * 0.37f) * (1 + i % 7), x = cosf(i * 0.11f) * (1 + i % 5);
        if (x != 0 || y != 0) { e = fabs(remainder(vcpe_atan2(y, x) - atan2(y, x), 2 * M_PI)); if (e > ea) ea = e; }
    }
    VcpeRng g; vcpe_rng_seed(&g, 42);
    int hist[10] = {0}, bad = 0;
    for (int i = 0; i < 1000000; i++) { int k = vcpe_rng_int(&g, 10); if (k < 0 || k > 9) bad++; else hist[k]++; float f = vcpe_rng_float(&g); if (f < 0 || f >= 1) bad++; }
    int lo = hist[0], hi = hist[0]; for (int i = 1; i < 10; i++) { if (hist[i] < lo) lo = hist[i]; if (hist[i] > hi) hi = hist[i]; }
    printf("sin/cos max err %.2e  atan2 max err %.2e  rng out-of-range %d  bucket spread %d..%d\n", es, ea, bad, lo, hi);
    printf("angle_diff(350,10)=%.1f diff(10,350)=%.1f diff(720,0)=%.1f\n", vcpe_angle_diff_deg(350, 10), vcpe_angle_diff_deg(10, 350), vcpe_angle_diff_deg(720, 0));
    return !(es < 2e-6 && ea < 2e-5 && !bad);
}
