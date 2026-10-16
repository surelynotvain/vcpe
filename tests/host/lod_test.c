/* Host test for vcpe_lod (plain C, no PSP headers): reads a VLD1 blob made by tools/vcpe_lod.py and checks the
 * layout, the vertex prefixes, picking by distance and the hysteresis. Usage: lod_test FILE.vld */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_lod.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) { passes++; printf("PASS " __VA_ARGS__); } else { fails++; printf("FAIL " __VA_ARGS__); } putchar('\n'); } while (0)

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint32_t buf[1 << 18];
    int size = (int)fread(buf, 1, sizeof buf, f);
    fclose(f);
    VcpeLodMesh m;
    CHECK(vcpe_lod_open(&m, buf, size) == 0, "open %d bytes: %d vertices, %d levels", size, m.nverts, m.nlevels);
    CHECK(m.nlevels >= 4 && m.lv[0].err == 0 && m.lv[0].nverts == m.nverts, "level 0 is the full mesh");
    int prefix_ok = 1, shrink_ok = 1;
    for (int l = 0; l < m.nlevels; l++) {
        const uint16_t *ix = vcpe_lod_idx(&m, l);
        for (int i = 0; i < vcpe_lod_nidx(&m, l); i++)
            if (ix[i] >= m.lv[l].nverts) prefix_ok = 0;
        if (l && (m.lv[l].nidx >= m.lv[l - 1].nidx || m.lv[l].nverts > m.lv[l - 1].nverts || m.lv[l].err <= 0)) shrink_ok = 0;
        printf("     level %d: %5u tris, %5u verts, error %.4f\n", l, m.lv[l].nidx / 3, m.lv[l].nverts, m.lv[l].err);
    }
    CHECK(prefix_ok, "every level only uses its vertex prefix");
    CHECK(shrink_ok, "levels get coarser with growing error");
    CHECK(fabsf(m.radius - 1) < 1e-3f && fabsf(m.centre[0]) < 1e-3f, "bounding sphere r %.4f", m.radius);

    VcpeLodView v;
    float cam[3] = {0, 0, 0}, c[3] = {0, 0, -3};
    vcpe_lod_view(&v, cam, 60, 272, 1);
    CHECK(fabsf(v.px_k - 136 / tanf(30 * 3.14159265f / 180)) < 0.01f, "pixels per unit %.2f", v.px_k);
    int near = vcpe_lod_pick(&m, &v, c, 1, -1);
    CHECK(near <= 1 && vcpe_lod_error_px(&m, &v, c, 1, near) <= 1, "3 units away: level %d (%.2f px)", near, vcpe_lod_error_px(&m, &v, c, 1, near));
    c[2] = -0.5f;
    CHECK(vcpe_lod_pick(&m, &v, c, 1, -1) == 0, "camera inside the sphere: full detail");
    int last = 0, mono = 1, picks[64];
    for (int d = 0; d < 64; d++) {
        c[2] = -(2 + d * 2.0f);
        picks[d] = vcpe_lod_pick(&m, &v, c, 1, -1);
        if (picks[d] < last) mono = 0;
        if (vcpe_lod_error_px(&m, &v, c, 1, picks[d]) > 1) mono = 0;
        last = picks[d];
    }
    CHECK(mono && picks[63] == m.nlevels - 1, "farther never picks finer, error always <= 1 px, far = coarsest");
    c[2] = -6;
    int a = vcpe_lod_pick(&m, &v, c, 1, -1);
    c[2] = -12;
    CHECK(vcpe_lod_pick(&m, &v, c, 2, -1) == a, "scale 2 at twice the distance-ish picks like scale 1 (%d)", a);

    /* hysteresis: find the distance where level 1 starts, sit just past it, and wiggle */
    float d0 = 0;
    for (float d = 1.5f; d < 400; d += 0.01f) {
        c[2] = -d;
        if (vcpe_lod_pick(&m, &v, c, 1, -1) >= 1) { d0 = d; break; }
    }
    int lvl = 0, switches = 0;
    for (int i = 0; i < 200; i++) {
        c[2] = -(d0 + ((i & 1) ? 0.02f : -0.02f));
        int n = vcpe_lod_pick(&m, &v, c, 1, lvl);
        switches += n != lvl;
        lvl = n;
    }
    CHECK(d0 > 0 && switches == 0, "no flicker at the level-1 boundary (%.2f units, %d switches)", d0, switches);
    c[2] = -(d0 * 2);
    int up = vcpe_lod_pick(&m, &v, c, 1, 0);
    c[2] = -1.5f;
    CHECK(up >= 1 && vcpe_lod_pick(&m, &v, c, 1, up) == 0, "moving away goes coarser, coming close goes back to full");

    v.thresh_px = 4;
    c[2] = -6;
    CHECK(vcpe_lod_pick(&m, &v, c, 1, -1) >= a, "a looser threshold never picks finer");

    /* damaged blobs */
    static uint32_t bad[1 << 18];
    memcpy(bad, buf, size);
    ((uint8_t *)bad)[0] = 'X';
    CHECK(vcpe_lod_open(&m, bad, size) == -1, "wrong magic refused");
    memcpy(bad, buf, size);
    CHECK(vcpe_lod_open(&m, bad, 100) == -2, "truncated blob refused");
    VcpeLodLevel *lv = (VcpeLodLevel *)((uint8_t *)bad + 32);
    lv[1].idx_off = size;
    CHECK(vcpe_lod_open(&m, bad, size) == -2, "index range past the end refused");
    CHECK(vcpe_lod_open(&m, (uint8_t *)buf + 1, size - 1) != 0, "unaligned blob refused");
    printf("%d PASS, %d FAIL\n", passes, fails);
    return fails != 0;
}
