/* Arc length is cached once; lookups are a binary search plus one Catmull-Rom segment. */
#include "vcpe_path.h"
#include "vcpe_math.h"

int vcpe_path_init(VcpePath *p, const float (*pts)[3], int n)
{
    if (!p || !pts || n < 2 || n > VCPE_PATH_MAX)
        return -1;
    p->points = pts;
    p->count = n;
    p->cumulative[0] = 0;
    for (int i = 1; i < n; i++) {
        float dx = pts[i][0] - pts[i - 1][0], dz = pts[i][2] - pts[i - 1][2];
        p->cumulative[i] = p->cumulative[i - 1] + vcpe_sqrt(dx * dx + dz * dz);
    }
    return p->cumulative[n - 1] > .001f ? 0 : -1;
}

float vcpe_path_length(const VcpePath *p) { return p && p->count ? p->cumulative[p->count - 1] : 0; }

void vcpe_path_point(const VcpePath *p, float distance, float out[3])
{
    int count = p->count;
    if (count < 2) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    float total = p->cumulative[count - 1], d = vcpe_clampf(distance, 0, total);
    int lo = 0, hi = count - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (p->cumulative[mid] <= d) lo = mid; else hi = mid;
    }
    float len = p->cumulative[hi] - p->cumulative[lo], u = len > .00001f ? (d - p->cumulative[lo]) / len : 0;
    float u2 = u * u, u3 = u2 * u;
    const float *a = p->points[lo ? lo - 1 : lo], *b = p->points[lo], *c = p->points[hi];
    const float *e = p->points[hi + 1 < count ? hi + 1 : hi];
    for (int k = 0; k < 3; k++)
        out[k] = .5f * (2 * b[k] + (-a[k] + c[k]) * u + (2 * a[k] - 5 * b[k] + 4 * c[k] - e[k]) * u2 +
                        (-a[k] + 3 * b[k] - 3 * c[k] + e[k]) * u3);
    if (distance > total) {
        const float *q0 = p->points[count - 2], *q1 = p->points[count - 1];
        float extra = (distance - total) / (len > .00001f ? len : 1);
        out[0] += (q1[0] - q0[0]) * extra;
        out[2] += (q1[2] - q0[2]) * extra;
    }
}

void vcpe_path_sample(const VcpePath *p, float d, float half_base, float out[3], float *yaw)
{
    float a[3], b[3];
    vcpe_path_point(p, d, out);
    vcpe_path_point(p, d - half_base, a);
    vcpe_path_point(p, d + half_base, b);
    *yaw = vcpe_atan2(a[0] - b[0], a[2] - b[2]) * 180 / VCPE_PI;
}
