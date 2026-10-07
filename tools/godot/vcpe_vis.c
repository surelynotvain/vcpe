/* vcpe_vis: which triangles are ever seen from a set of camera poses (for dropping never-visible static scenery).
 * in:  u32 ntris, u32 nposes, nposes x 16 floats (view-projection, row-major), ntris x 9 floats (world xyz x3)
 * out: ntris bytes (1 = some pixel of it was in front at some pose)
 * Software z-buffer at W x H, clipping against the near plane (w > eps). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define W 640
#define H 364

static float zb[W * H];
static int ib[W * H];

typedef struct { float x, y, z, w; } V4;

static V4 mul(const float *m, const float *p)
{
    V4 r;
    r.x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    r.y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    r.z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    r.w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    return r;
}

static void raster(const V4 *v, int id)
{
    float sx[3], sy[3], sz[3];
    for (int i = 0; i < 3; i++) {
        sx[i] = (v[i].x / v[i].w * 0.5f + 0.5f) * W;
        sy[i] = (1.0f - (v[i].y / v[i].w * 0.5f + 0.5f)) * H;
        sz[i] = v[i].z / v[i].w;
    }
    float minx = sx[0], maxx = sx[0], miny = sy[0], maxy = sy[0];
    for (int i = 1; i < 3; i++) {
        if (sx[i] < minx) minx = sx[i];
        if (sx[i] > maxx) maxx = sx[i];
        if (sy[i] < miny) miny = sy[i];
        if (sy[i] > maxy) maxy = sy[i];
    }
    int x0 = (int)minx, x1 = (int)maxx + 1, y0 = (int)miny, y1 = (int)maxy + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (area == 0 || x0 >= x1 || y0 >= y1)
        return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
            if (z < -1 || z > 1)
                continue;
            int k = y * W + x;
            if (z < zb[k]) {
                zb[k] = z;
                ib[k] = id;
            }
        }
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    unsigned nt, np;
    if (!f || fread(&nt, 4, 1, f) != 1 || fread(&np, 4, 1, f) != 1) return 1;
    float *vp = malloc(np * 16 * 4), *tri = malloc((size_t)nt * 9 * 4);
    if (fread(vp, 4, np * 16, f) != np * 16 || fread(tri, 4, (size_t)nt * 9, f) != (size_t)nt * 9) return 1;
    fclose(f);
    unsigned char *vis = calloc(nt, 1);
    const float eps = 1e-3f;
    for (unsigned p = 0; p < np; p++) {
        for (int k = 0; k < W * H; k++) { zb[k] = FLT_MAX; ib[k] = -1; }
        const float *m = vp + p * 16;
        for (unsigned t = 0; t < nt; t++) {
            V4 in[3], poly[9], tmp[9];
            int n = 3;
            for (int i = 0; i < 3; i++) in[i] = mul(m, tri + (size_t)t * 9 + i * 3);
            /* clip against w > eps (Sutherland-Hodgman, one plane) */
            int m2 = 0;
            for (int i = 0; i < n; i++) {
                V4 a = in[i], b = in[(i + 1) % n];
                int ia = a.w > eps, ibn = b.w > eps;
                if (ia) tmp[m2++] = a;
                if (ia != ibn) {
                    float s = (eps - a.w) / (b.w - a.w);
                    V4 c = {a.x + s * (b.x - a.x), a.y + s * (b.y - a.y), a.z + s * (b.z - a.z), eps};
                    tmp[m2++] = c;
                }
            }
            if (m2 < 3) continue;
            memcpy(poly, tmp, sizeof(V4) * m2);
            for (int i = 1; i + 1 < m2; i++) {
                V4 tr[3] = {poly[0], poly[i], poly[i + 1]};
                raster(tr, (int)t);
            }
        }
        for (int k = 0; k < W * H; k++)
            if (ib[k] >= 0) vis[ib[k]] = 1;
    }
    f = fopen(argv[2], "wb");
    fwrite(vis, 1, nt, f);
    fclose(f);
    unsigned c = 0;
    for (unsigned t = 0; t < nt; t++) c += vis[t];
    printf("visible %u of %u triangles over %u poses\n", c, nt, np);
    return 0;
}
