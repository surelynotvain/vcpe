/* vcpe_terrain host test: cells cut by tools/vcpe_terrain.py, heights, seams, level errors, index lists, skirts,
 * LOD picking, relighting, rays and the far grid. */
#include "vcpe_terrain.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
_Static_assert(sizeof(VcpeTerrainBlob) == 48, "VTR1 header");

static void *slurp(const char *dir, const char *name, uint32_t *n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("missing %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *m = aligned_alloc(16, ((size_t)len + 15) & ~(size_t)15);
    if (fread(m, 1, (size_t)len, f) != (size_t)len) exit(1);
    fclose(f);
    *n = (uint32_t)len;
    return m;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static VcpeTerrainCell cell[2][2];
    VcpeTerrainCell *list[4];
    void *blob[2][2];
    uint32_t bsize[2][2];
    for (int cz = 0; cz < 2; cz++)
        for (int cx = 0; cx < 2; cx++) {
            char name[64];
            snprintf(name, sizeof name, "cell_%d_%d.vtr", cx, cz);
            blob[cz][cx] = slurp(dir, name, &bsize[cz][cx]);
            uint32_t need = vcpe_terrain_cell_bytes(blob[cz][cx], bsize[cz][cx]);
            CHECK(need == (17u * 17 + 4 * 17) * 24 + 17u * 17 * 4, "cell bytes %u", need);
            void *mem = aligned_alloc(16, (need + 15) & ~15u);
            CHECK(vcpe_terrain_cell(&cell[cz][cx], blob[cz][cx], bsize[cz][cx], mem, need - 16) == -2, "too little memory refused");
            CHECK(!vcpe_terrain_cell(&cell[cz][cx], blob[cz][cx], bsize[cz][cx], mem, need), "cell %d %d builds", cx, cz);
            list[cz * 2 + cx] = &cell[cz][cx];
        }
    VcpeTerrainCell *c = &cell[0][0];
    CHECK(c->n == 16 && c->levels == 5 && fabsf(c->quad - 2) < 1e-6f && c->blob->page == 3 && c->has_overlay, "header, overlay present");
    CHECK(!cell[1][1].has_overlay, "a cell without overlay alpha draws one pass");

    /* heights against the reference triangles, from any cell */
    char path[512], line[256];
    snprintf(path, sizeof path, "%s/expect.txt", dir);
    FILE *f = fopen(path, "r");
    int heights = 0, errs = 0, fw = 0, fd = 0;
    float worst = 0;
    while (fgets(line, sizeof line, f)) {
        float x, z, y, e;
        int cx, cz, l;
        if (sscanf(line, "h %f %f %f", &x, &z, &y) == 3) {
            float got = -1;
            CHECK(vcpe_terrain_height_any(list, 4, x, z, &got, 0), "point %g %g on the terrain", x, z);
            worst = fmaxf(worst, fabsf(got - y));
            heights++;
        } else if (sscanf(line, "err %d %d %d %f", &cx, &cz, &l, &e) == 4) {
            float mine = cell[cz][cx].err[l];
            /* the runtime keeps errors monotonic (a coarser level never claims less), so compare to the running max */
            CHECK(mine >= e - 1e-3f, "cell %d %d level %d error %g below the measured %g", cx, cz, l, mine, e);
            if (l == 1) CHECK(fabsf(mine - e) < 1e-3f, "level 1 error %g vs %g", mine, e);
            errs++;
        } else
            sscanf(line, "far %d %d", &fw, &fd);
    }
    fclose(f);
    CHECK(heights == 200 && worst < 1e-3f, "%d heights within %g m", heights, worst);
    CHECK(errs == 16 && c->err[0] == 0, "level errors");
    CHECK(cell[0][0].err[1] < 1e-3f || cell[0][0].err[4] > cell[0][0].err[1], "errors grow with the level");

    /* seams: both cells give the same height on their shared edge; edge vertices coincide */
    float worst_seam = 0;
    for (int k = 0; k <= 64; k++) {
        float z = k * 0.5f, a, b;
        int ha = vcpe_terrain_height(&cell[0][0], 32.0f, z, &a, 0), hb = vcpe_terrain_height(&cell[0][1], 32.0f, z, &b, 0);
        if (z <= 32) { CHECK(ha && hb, "edge in both cells"); worst_seam = fmaxf(worst_seam, fabsf(a - b)); }
    }
    for (int j = 0; j <= 16; j++) {
        const VcpeVtx *l0 = &cell[0][0].verts[j * 17 + 16], *r0 = &cell[0][1].verts[j * 17];
        worst_seam = fmaxf(worst_seam, fabsf(l0->y - r0->y) + fabsf(l0->x + 0 - (r0->x + 32)) + fabsf(l0->z - r0->z));
    }
    CHECK(worst_seam < 1e-5f, "cell edges match exactly (%g)", worst_seam);
    float y, nrm[3];
    CHECK(!vcpe_terrain_height(&cell[0][0], 40, 5, &y, 0) && !vcpe_terrain_height(&cell[0][0], -0.1f, 5, &y, 0), "outside the cell");
    CHECK(vcpe_terrain_height(&cell[0][0], 3, 3, &y, nrm) && fabsf(y - 3) < 1e-3f && fabsf(nrm[1] - 1) < 1e-5f, "flat corner: height 3, normal up");
    vcpe_terrain_height(&cell[0][1], 40.5f, 20, &y, nrm);
    CHECK(fabsf(sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]) - 1) < 1e-5f && nrm[0] < -0.5f, "cliff normal faces -X (%g %g %g)", nrm[0], nrm[1], nrm[2]);

    /* index lists: in range, cover the cell exactly once from above, skirts appended */
    for (int l = 0; l < c->levels; l++) {
        int count;
        const uint16_t *idx = vcpe_terrain_indices(16, l, &count);
        int m = 16 >> l, grid = m * m * 6, bad = 0;
        CHECK(idx && count == grid + 4 * m * 6, "level %d count %d", l, count);
        double area = 0;
        for (int i = 0; i < count; i++) bad += idx[i] >= c->nverts;
        for (int t = 0; t < grid; t += 3) {
            const VcpeVtx *a = &c->verts[idx[t]], *b = &c->verts[idx[t + 1]], *d = &c->verts[idx[t + 2]];
            area += fabs((b->x - a->x) * (d->z - a->z) - (d->x - a->x) * (b->z - a->z)) * 0.5;
        }
        CHECK(!bad && fabs(area - 32.0 * 32.0) < 1e-3, "level %d covers %g m2", l, area);
    }
    int dummy;
    CHECK(!vcpe_terrain_indices(12, 0, &dummy) && !vcpe_terrain_indices(16, 5, &dummy), "bad n / level refused");
    for (int t = 0; t <= 16; t++) {
        const VcpeVtx *e = &c->verts[t], *s = &c->verts[17 * 17 + t];
        CHECK(e->x == s->x && e->z == s->z && fabsf(e->y - s->y - c->skirt) < 1e-5f, "north skirt hangs below its edge");
    }
    CHECK(c->skirt > 2 * c->err[c->levels - 1], "skirt deeper than any crack");

    /* LOD: near full detail, far coarse, hysteresis keeps a level at the boundary */
    float origin[3] = {0, 0, 0};
    VcpeLodView near_v, far_v;
    vcpe_lod_view(&near_v, (float[3]){16, 20, 16}, 60, 272, 1.5f);
    vcpe_lod_view(&far_v, (float[3]){16, 20, 3000}, 60, 272, 1.5f);
    int ln = vcpe_terrain_pick(&cell[0][1], &near_v, origin, -1), lf = vcpe_terrain_pick(&cell[0][1], &far_v, origin, -1);
    CHECK(ln == 0 && lf == cell[0][1].levels - 1, "near %d far %d", ln, lf);
    int steps = 0, prev = 0, flips = 0;
    for (float zz = 40; zz < 2000; zz *= 1.05f, steps++) {
        VcpeLodView v;
        vcpe_lod_view(&v, (float[3]){48, 20, zz}, 60, 272, 1.5f);
        int l = vcpe_terrain_pick(&cell[0][1], &v, origin, prev);
        flips += l < prev;
        prev = l;
    }
    CHECK(flips == 0 && prev > 0, "moving away only coarsens (%d steps)", steps);
    VcpeLodView shifted;
    vcpe_lod_view(&shifted, (float[3]){16 + 1000, 20, 16 + 1000}, 60, 272, 1.5f);   /* camera relative to the origin */
    CHECK(vcpe_terrain_pick(&cell[0][1], &shifted, (float[3]){-1000, 0, -1000}, -1) == ln, "origin shift changes nothing");

    /* lighting */
    VcpeTerrainLight light = {{0, 1, 0}, 0xff808080u, 0xff404040u, 7};
    vcpe_terrain_relight(c, &light);
    uint32_t col = c->verts[2 * 17 + 2].color;      /* flat corner, base colour (100, 160, 200), alpha 0 */
    CHECK((col & 0xff) == (200 * 192 + 127) / 255 && ((col >> 8) & 0xff) == (160 * 192 + 127) / 255 && !(col >> 24), "lit flat ground 0x%08x", col);
    CHECK(c->verts[17 * 17 + 2].color == c->verts[2].color, "skirts take their edge's colour");
    CHECK(((c->verts[5 * 17 + 11].color) >> 24) == 255, "road overlay alpha survives relighting");
    VcpeTerrainLight below = {{0, -1, 0}, 0xffffffffu, 0xff202020u, 8};
    vcpe_terrain_relight(c, &below);
    col = c->verts[2 * 17 + 2].color;
    CHECK((col & 0xff) == (200 * 32 + 127) / 255, "sun below the ground: ambient only");
    for (int i = 0; i < 4; i++) list[i]->light_serial = 0;
    int relit = vcpe_terrain_relight_some(list, 4, &light, 400);
    CHECK(relit == 1, "vertex budget: one cell per call (%d)", relit);
    relit = vcpe_terrain_relight_some(list, 4, &light, 100000);
    CHECK(relit == 3 && vcpe_terrain_relight_some(list, 4, &light, 100000) == 0, "the rest, then nothing to do");

    /* rays */
    float t;
    CHECK(vcpe_terrain_ray(c, (float[3]){3, 50, 3}, (float[3]){0, -1, 0}, 100, &t) && fabsf(t - 47) < 1e-2f, "straight down hits at 47 m (%g)", t);
    float o[3] = {1, 30, 1}, d[3] = {0.6f, -0.5f, 0.62f};
    CHECK(vcpe_terrain_ray(&cell[0][0], o, d, 200, &t), "slanted ray hits");
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), p[3] = {o[0] + d[0] / l * t, o[1] + d[1] / l * t, o[2] + d[2] / l * t};
    vcpe_terrain_height(&cell[0][0], p[0], p[2], &y, 0);
    CHECK(fabsf(p[1] - y) < 0.02f, "hit point on the ground (%g vs %g)", p[1], y);
    CHECK(!vcpe_terrain_ray(c, (float[3]){3, 50, 3}, (float[3]){0, 1, 0}, 100, &t), "upward ray misses");

    /* far grid with a hole under the loaded cells */
    uint32_t fn;
    uint16_t *fh = slurp(dir, "far.u16", &fn);
    CHECK(fw == 4 && fd == 4 && fn == 25 * 2, "far grid %dx%d", fw, fd);
    VcpeTerrainFar far;
    uint32_t fb = vcpe_terrain_far_bytes(fw, fd);
    void *fm = aligned_alloc(16, (fb + 15) & ~15u);
    CHECK(!vcpe_terrain_far_init(&far, fh, fw, fd, 0, 0, 16, c->blob->base, c->blob->step, 9, 0xff808080u, fm), "far init");
    CHECK(far.nidx == 4 * 4 * 6, "everything kept");
    CHECK(vcpe_terrain_far_cut(&far, 0, 0, 32, 32) == 32 - 8, "the loaded cell's 4 quads left out");
    vcpe_terrain_height(&cell[0][1], 48, 16, &y, 0);
    CHECK(fabsf(far.verts[1 * 5 + 3].y - y) < 1e-3f, "far samples sit on the cell heights");
    vcpe_terrain_far_relight(&far, &light);
    CHECK(far.light_serial == 7 && far.dirty, "far relit");
    CHECK(vcpe_terrain_far_bytes(300, 300) == 0, "far grid beyond 16-bit indices refused");

    /* damaged blobs */
    uint8_t *bad = aligned_alloc(16, (bsize[0][0] + 15) & ~15u);
    memcpy(bad, blob[0][0], bsize[0][0]);
    ((VcpeTerrainBlob *)bad)->n = 12;
    CHECK(!vcpe_terrain_cell_bytes(bad, bsize[0][0]), "n not a power of two refused");
    ((VcpeTerrainBlob *)bad)->n = 16;
    ((VcpeTerrainBlob *)bad)->heights = bsize[0][0] - 10;
    CHECK(!vcpe_terrain_cell_bytes(bad, bsize[0][0]), "heights out of bounds refused");
    bad[0] = 'X';
    VcpeTerrainCell tmp;
    CHECK(vcpe_terrain_cell(&tmp, bad, bsize[0][0], fm, 1 << 20) == -1, "not VTR1");

    free(bad);
    free(fm);
    free(fh);
    for (int cz = 0; cz < 2; cz++)
        for (int cx = 0; cx < 2; cx++) { free(cell[cz][cx].verts); free(blob[cz][cx]); }
    printf("PASS terrain: heights, seams, level errors, index coverage, skirts, LOD hysteresis, relighting, rays, far grid\n");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
