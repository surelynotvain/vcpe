/* vcpe_shadow: which (point, light) pairs see each other, for baking shadowed lighting into vertex colours.
 * in:  u32 ntris, u32 nlights, u32 nqueries, ntris x 9 floats (world xyz x3), nlights x 3 floats (position),
 *      nqueries x (3 floats point, 3 floats normal, u32 light index)
 * out: nqueries bytes (1 = lit: no triangle between the point and the light)
 * A BVH over the triangles (median split on the longest axis), Moller-Trumbore any-hit rays.
 * Like Godot's shadow maps (casters drawn with back faces culled) a triangle only blocks a light it faces:
 * the converter orders each triangle's corners so (v1 - v0) x (v2 - v0) is its front normal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef struct { float lo[3], hi[3]; int left, count; } Node;   /* leaf: count > 0, triangles from left */

static float *T;          /* triangles, 9 floats each */
static int *ord;          /* triangle order */
static float *cen;        /* centroids */
static Node *nodes;
static int nnodes;

static void bounds(int first, int count, float *lo, float *hi)
{
    for (int k = 0; k < 3; k++) { lo[k] = 1e30f; hi[k] = -1e30f; }
    for (int i = first; i < first + count; i++) {
        const float *t = T + ord[i] * 9;
        for (int v = 0; v < 3; v++)
            for (int k = 0; k < 3; k++) {
                if (t[v * 3 + k] < lo[k]) lo[k] = t[v * 3 + k];
                if (t[v * 3 + k] > hi[k]) hi[k] = t[v * 3 + k];
            }
    }
}

static int axis_sort;
static int cmp(const void *a, const void *b)
{
    float x = cen[*(const int *)a * 3 + axis_sort], y = cen[*(const int *)b * 3 + axis_sort];
    return x < y ? -1 : x > y;
}

static int build(int first, int count)
{
    int n = nnodes++;
    bounds(first, count, nodes[n].lo, nodes[n].hi);
    if (count <= 4) {
        nodes[n].left = first;
        nodes[n].count = count;
        return n;
    }
    float ext[3];
    for (int k = 0; k < 3; k++) ext[k] = nodes[n].hi[k] - nodes[n].lo[k];
    axis_sort = ext[0] > ext[1] ? (ext[0] > ext[2] ? 0 : 2) : (ext[1] > ext[2] ? 1 : 2);
    qsort(ord + first, count, sizeof(int), cmp);
    int half = count / 2;
    nodes[n].count = 0;
    int l = build(first, half);
    int r = build(first + half, count - half);
    (void)l;
    nodes[n].left = r;            /* left child is n + 1; right child stored */
    return n;
}

static int box_hit(const Node *nd, const float *o, const float *inv, float tmax)
{
    float t0 = 0, t1 = tmax;
    for (int k = 0; k < 3; k++) {
        float a = (nd->lo[k] - o[k]) * inv[k], b = (nd->hi[k] - o[k]) * inv[k];
        if (a > b) { float t = a; a = b; b = t; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return 0;
    }
    return 1;
}

static int tri_hit(const float *t, const float *o, const float *d, float tmax)
{
    float e1[3], e2[3], p[3], q[3], s[3];
    for (int k = 0; k < 3; k++) { e1[k] = t[3 + k] - t[k]; e2[k] = t[6 + k] - t[k]; s[k] = o[k] - t[k]; }
    p[0] = d[1] * e2[2] - d[2] * e2[1]; p[1] = d[2] * e2[0] - d[0] * e2[2]; p[2] = d[0] * e2[1] - d[1] * e2[0];
    float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (fabsf(det) < 1e-12f) return 0;
    float id = 1.0f / det;
    float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * id;
    if (u < 0 || u > 1) return 0;
    q[0] = s[1] * e1[2] - s[2] * e1[1]; q[1] = s[2] * e1[0] - s[0] * e1[2]; q[2] = s[0] * e1[1] - s[1] * e1[0];
    float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * id;
    if (v < 0 || u + v > 1) return 0;
    float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * id;
    if (!(tt > 1e-4f && tt < tmax)) return 0;
    /* front normal . direction to the light > 0: the light sees the front face */
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    return n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0;
}

static int occluded(const float *o, const float *d, float tmax)
{
    float inv[3];
    for (int k = 0; k < 3; k++) inv[k] = 1.0f / (fabsf(d[k]) > 1e-12f ? d[k] : (d[k] < 0 ? -1e-12f : 1e-12f));
    int stack[128], sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node *nd = &nodes[stack[--sp]];
        if (!box_hit(nd, o, inv, tmax)) continue;
        if (nd->count) {
            for (int i = nd->left; i < nd->left + nd->count; i++)
                if (tri_hit(T + ord[i] * 9, o, d, tmax)) return 1;
        } else {
            int self = (int)(nd - nodes);
            if (sp < 126) { stack[sp++] = self + 1; stack[sp++] = nd->left; }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: vcpe_shadow in.bin out.bin\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    uint32_t nt, nl, nq;
    if (fread(&nt, 4, 1, f) != 1 || fread(&nl, 4, 1, f) != 1 || fread(&nq, 4, 1, f) != 1) return 1;
    T = malloc((size_t)nt * 36);
    float *L = malloc((size_t)nl * 12 + 12);
    float *Q = malloc((size_t)nq * 28 + 28);
    if (fread(T, 36, nt, f) != nt || fread(L, 12, nl, f) != nl || fread(Q, 28, nq, f) != nq) return 1;
    fclose(f);
    ord = malloc(sizeof(int) * (nt + 1));
    cen = malloc(sizeof(float) * 3 * (nt + 1));
    for (uint32_t i = 0; i < nt; i++) {
        ord[i] = (int)i;
        for (int k = 0; k < 3; k++) cen[i * 3 + k] = (T[i * 9 + k] + T[i * 9 + 3 + k] + T[i * 9 + 6 + k]) / 3;
    }
    nodes = malloc(sizeof(Node) * (2 * nt + 2));
    if (nt) build(0, (int)nt);
    uint8_t *out = calloc(nq + 1, 1);
    long lit = 0;
    for (uint32_t q = 0; q < nq; q++) {
        const float *p = Q + q * 7, *n = p + 3;
        uint32_t li;
        memcpy(&li, p + 6, 4);
        const float *lp = L + li * 3;
        float o[3], d[3];
        for (int k = 0; k < 3; k++) o[k] = p[k] + n[k] * 0.01f;   /* off the surface */
        for (int k = 0; k < 3; k++) d[k] = lp[k] - o[k];
        out[q] = !nt || !occluded(o, d, 0.999f);
        lit += out[q];
    }
    f = fopen(argv[2], "wb");
    fwrite(out, 1, nq, f);
    fclose(f);
    printf("shadow rays %u, lit %ld, triangles %u, bvh nodes %d\n", nq, lit, nt, nnodes);
    return 0;
}
