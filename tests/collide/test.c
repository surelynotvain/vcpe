/* vcpe_collide host test: mesh grid, rays (meshes, transformed instances, cylinders, ground), and the capsule
 * controller on walls, stairs, ramps, jumps, ceilings, falls, trees, moving instances and terrain. */
#include "vcpe_collide.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a mesh builder: boxes and quads into one triangle soup */
typedef struct { float v[4096][3]; uint16_t i[8192]; int nv, ni; } Soup;
static void quad(Soup *s, const float a[3], const float b[3], const float c[3], const float d[3])
{
    int k = s->nv;
    memcpy(s->v[k], a, 12); memcpy(s->v[k + 1], b, 12); memcpy(s->v[k + 2], c, 12); memcpy(s->v[k + 3], d, 12);
    s->nv += 4;
    uint16_t t[6] = {(uint16_t)k, (uint16_t)(k + 1), (uint16_t)(k + 2), (uint16_t)k, (uint16_t)(k + 2), (uint16_t)(k + 3)};
    memcpy(s->i + s->ni, t, sizeof t);
    s->ni += 6;
}
static void box(Soup *s, float x0, float y0, float z0, float x1, float y1, float z1)
{
    float p[8][3];
    for (int c = 0; c < 8; c++) { p[c][0] = c & 1 ? x1 : x0; p[c][1] = c & 2 ? y1 : y0; p[c][2] = c & 4 ? z1 : z0; }
    quad(s, p[0], p[1], p[3], p[2]); quad(s, p[4], p[5], p[7], p[6]);   /* z faces */
    quad(s, p[0], p[2], p[6], p[4]); quad(s, p[1], p[3], p[7], p[5]);   /* x faces */
    quad(s, p[0], p[1], p[5], p[4]); quad(s, p[2], p[3], p[7], p[6]);   /* y faces */
}
static VcpeColMesh build(Soup *s)
{
    VcpeColMesh m;
    uint32_t n = vcpe_colmesh_bytes(&s->v[0][0], 12, s->i, s->ni / 3, 1.0f);
    void *mem = malloc(n);
    if (vcpe_colmesh_build(&m, &s->v[0][0], 12, s->i, s->ni / 3, 1.0f, mem, n)) { printf("build failed\n"); exit(1); }
    return m;
}

static int flat_ground(void *u, float x, float z, float *y, float n[3]) { (void)u; (void)x; (void)z; *y = 0; n[0] = 0; n[1] = 1; n[2] = 0; return 1; }
/* rolling hills plus a 70-degree cliff band at 30 < x < 32 */
static float hills(float x, float z) { float h = 0.5f * sinf(x * 0.3f) * cosf(z * 0.25f); if (x > 30) h += (x < 32 ? (x - 30) * 2.75f : 5.5f); return h; }
static int hill_ground(void *u, float x, float z, float *y, float n[3])
{
    (void)u;
    float e = 0.01f, h = hills(x, z), dx = (hills(x + e, z) - hills(x - e, z)) / (2 * e), dz = (hills(x, z + e) - hills(x, z - e)) / (2 * e);
    float l = 1 / sqrtf(dx * dx + 1 + dz * dz);
    *y = h; n[0] = -dx * l; n[1] = l; n[2] = -dz * l;
    return 1;
}

static VcpeColInstance inst[16];
static VcpeColCylinder cyl[8];

static void walk(VcpeColWorld *w, VcpeKcc *k, float vx, float vz, int frames, int *air)
{
    float wish[3] = {vx, 0, vz};
    for (int i = 0; i < frames; i++) {
        vcpe_kcc_step(w, k, wish, 0, 0, 1 / 60.0f);
        if (air && !k->grounded) (*air)++;
    }
}

int main(void)
{
    static Soup scene;
    /* a wall along x = 5, stairs going up toward -z, a high block, a 30 and a 60 degree ramp, a low ceiling */
    quad(&scene, (float[3]){5, 0, -40}, (float[3]){5, 0, 40}, (float[3]){5, 3, 40}, (float[3]){5, 3, -40});
    for (int st = 0; st < 6; st++) box(&scene, -2, 0, -10 - 0.4f * st - 0.4f, 2, 0.25f * (st + 1), -10 - 0.4f * st);
    box(&scene, -2, 0, 10, 2, 0.6f, 12);
    float t30 = tanf(30 * 3.14159265f / 180), t60 = tanf(60 * 3.14159265f / 180);
    quad(&scene, (float[3]){-12, 0, 0}, (float[3]){-8, 0, 0}, (float[3]){-8, 4 * t30, -4}, (float[3]){-12, 4 * t30, -4});
    quad(&scene, (float[3]){-20, 0, 0}, (float[3]){-16, 0, 0}, (float[3]){-16, 3 * t60, -3}, (float[3]){-20, 3 * t60, -3});
    box(&scene, 10, 2.2f, -3, 14, 2.5f, 3);
    /* a degenerate triangle must be ignored */
    scene.v[scene.nv][0] = scene.v[scene.nv + 1][0] = scene.v[scene.nv + 2][0] = 1;
    scene.i[scene.ni++] = (uint16_t)scene.nv; scene.i[scene.ni++] = (uint16_t)(scene.nv + 1); scene.i[scene.ni++] = (uint16_t)(scene.nv + 2);
    scene.nv += 3;
    VcpeColMesh m = build(&scene);
    CHECK(m.gw <= 65 && m.gd <= 65 && m.ntris == scene.ni / 3, "grid %dx%d over %d triangles", m.gw, m.gd, m.ntris);
    CHECK(vcpe_colmesh_bytes(&scene.v[0][0], 8, scene.i, 2, 1) == 0, "bad stride refused");

    static Soup cube;
    box(&cube, -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
    VcpeColMesh cm = build(&cube);

    VcpeColWorld w;
    vcpe_col_world(&w, inst, 16, cyl, 8);
    vcpe_col_ground(&w, flat_ground, 0, 0);
    int sid = vcpe_col_add(&w, &m, 0, 77);
    CHECK(sid == 0, "scene instance");
    /* the cube rotated 45 degrees about Y, scaled 2, at (20, 1, 0): its corner points along +-x at 1.414 */
    float c45 = cosf(0.785398f) * 2, s45 = sinf(0.785398f) * 2;
    float cmat[12] = {c45, 0, s45, 20, 0, 2, 0, 1, -s45, 0, c45, 0};
    int cid = vcpe_col_add(&w, &cm, cmat, 5);
    int tree = vcpe_col_add_cylinder(&w, 0, -5, 0, 6, 0.4f, 9);

    /* rays */
    VcpeColHit h;
    CHECK(vcpe_col_ray(&w, (float[3]){0, 10, 3}, (float[3]){0, -1, 0}, 100, &h) && h.kind == VCPE_COL_GROUND && fabsf(h.t - 10) < 1e-3f, "down onto the ground");
    CHECK(vcpe_col_ray(&w, (float[3]){0, 1, 3}, (float[3]){1, 0, 0}, 100, &h) && h.kind == VCPE_COL_MESH && fabsf(h.t - 5) < 1e-4f && h.normal[0] < -0.99f && h.user == 77, "into the wall, normal toward the ray (t %g)", h.t);
    CHECK(!vcpe_col_ray(&w, (float[3]){0, 4, 3}, (float[3]){1, 0, 0}, 4.9f, &h), "stops short of the wall");
    CHECK(vcpe_col_ray(&w, (float[3]){15, 1, 0}, (float[3]){1, 0, 0}, 100, &h) && h.kind == VCPE_COL_MESH && h.id == cid && fabsf(h.t - (5 - 1.41421f)) < 1e-3f, "rotated, scaled cube's corner (t %g)", h.t);
    CHECK(vcpe_col_ray(&w, (float[3]){20, 10, 0}, (float[3]){0, -1, 0}, 100, &h) && h.id == cid && fabsf(h.point[1] - 2) < 1e-4f && h.normal[1] > 0.99f, "top of the scaled cube at y 2");
    CHECK(vcpe_col_ray(&w, (float[3]){0, 1, 0}, (float[3]){0, 0, -1}, 100, &h) && h.kind == VCPE_COL_CYLINDER && fabsf(h.t - 4.6f) < 1e-4f && h.normal[2] > 0.99f, "tree trunk (t %g)", h.t);
    CHECK(vcpe_col_ray(&w, (float[3]){0, 9, -5}, (float[3]){0, -1, 0}, 100, &h) && h.kind == VCPE_COL_CYLINDER && fabsf(h.point[1] - 6) < 1e-4f, "tree top");
    CHECK(vcpe_col_ray(&w, (float[3]){0, 0.1f, -8}, (float[3]){0, 0, -1}, 100, &h) && h.kind == VCPE_COL_MESH && fabsf(h.t - 2) < 1e-4f, "first stair riser");
    CHECK(vcpe_col_ray(&w, (float[3]){0, 1.1f, -8}, (float[3]){0, 0, -1}, 100, &h) && fabsf(h.t - 3.6f) < 1e-4f, "over three steps to the fifth riser (t %g)", h.t);
    /* a long ray across many grid cells: the 60 degree ramp where its surface reaches y 1 */
    CHECK(vcpe_col_ray(&w, (float[3]){-18, 1, 30}, (float[3]){0, 0, -1}, 200, &h) && h.kind == VCPE_COL_MESH && fabsf(h.point[2] + 1 / t60) < 1e-3f, "long ray onto the ramp (z %g)", h.point[2]);
    CHECK(vcpe_col_ray(&w, (float[3]){-30, 0.1f, -0.2f}, (float[3]){1, 0, 0}, 200, &h) && fabsf(h.point[0] - 5) < 1e-3f, "under the open ramps to the wall");

    VcpeKcc k;
    vcpe_kcc_init(&k, 0.3f, 1.8f);
    /* standing still on flat ground */
    k.pos[0] = 0; k.pos[1] = 0.5f; k.pos[2] = 3;
    walk(&w, &k, 0, 0, 120, 0);
    CHECK(k.grounded && fabsf(k.pos[1]) < 1e-4f && fabsf(k.pos[0]) < 1e-4f && fabsf(k.pos[2] - 3) < 1e-4f, "falls, lands, stays put (%g %g %g)", k.pos[0], k.pos[1], k.pos[2]);

    /* into the wall head-on, then sliding along it */
    walk(&w, &k, 4, 0, 120, 0);
    CHECK(fabsf(k.pos[0] - (5 - 0.3f)) < 0.01f && k.contacts > 0 && k.wall_normal[0] < -0.9f, "stopped by the wall at %g", k.pos[0]);
    float z0 = k.pos[2];
    walk(&w, &k, 3, 3, 60, 0);
    CHECK(fabsf(k.pos[0] - (5 - 0.3f)) < 0.01f && k.pos[2] - z0 > 2.9f, "slides along it (dz %g)", k.pos[2] - z0);
    /* a big step in one move does not tunnel */
    k.pos[0] = 0; k.pos[1] = 0; k.pos[2] = 3;
    vcpe_kcc_move(&w, &k, (float[3]){12, 0, 0});
    CHECK(k.pos[0] < 5, "a 12 m move stops at the wall (%g)", k.pos[0]);

    /* stairs up (0.25 m steps), then down again without leaving the ground */
    k.pos[0] = 0; k.pos[1] = 0; k.pos[2] = -8;
    vcpe_kcc_move(&w, &k, (float[3]){0, -0.01f, 0});
    int air = 0;
    walk(&w, &k, 0, -2, 120, &air);
    CHECK(k.grounded && fabsf(k.pos[1] - 1.5f) < 1e-3f, "up the stairs to 1.5 m (%g)", k.pos[1]);
    air = 0;
    walk(&w, &k, 0, 2, 100, &air);
    CHECK(air == 0 && fabsf(k.pos[1]) < 1e-3f, "down again, %d frames in the air", air);
    /* a 0.6 m block cannot be stepped on */
    k.pos[0] = 0; k.pos[1] = 0; k.pos[2] = 8;
    walk(&w, &k, 0, 2, 120, 0);
    CHECK(k.pos[2] < 10 - 0.29f && k.pos[1] < 0.01f, "blocked by the high step (z %g, y %g)", k.pos[2], k.pos[1]);

    /* standing on a ledge with the centre just past its edge: the edge probes hold the feet up */
    k.pos[0] = 0; k.pos[1] = 0.6f; k.pos[2] = 9.92f; k.grounded = 1;
    walk(&w, &k, 0, 0, 30, 0);
    CHECK(k.grounded && fabsf(k.pos[1] - 0.6f) < 1e-3f, "on the ledge's edge (y %g)", k.pos[1]);

    /* ramps: 30 degrees walkable, 60 degrees not */
    k.pos[0] = -10; k.pos[1] = 0; k.pos[2] = 1;
    walk(&w, &k, 0, -2, 150, 0);
    CHECK(k.grounded && k.pos[1] > 4 * t30 - 0.05f, "up the 30 degree ramp (y %g)", k.pos[1]);
    k.pos[0] = -18; k.pos[1] = 0; k.pos[2] = 1;
    walk(&w, &k, 0, -2, 150, 0);
    CHECK(k.pos[1] < 0.8f, "the 60 degree ramp is too steep (y %g)", k.pos[1]);

    /* jump: apex v^2 / 2g, lands grounded */
    k.pos[0] = 0; k.pos[1] = 0; k.pos[2] = 3;
    walk(&w, &k, 0, 0, 10, 0);
    vcpe_kcc_step(&w, &k, (float[3]){0, 0, 0}, 1, 4.5f, 1 / 60.0f);
    float apex = 0;
    int landed = -1;
    for (int i = 0; i < 120; i++) {
        vcpe_kcc_step(&w, &k, (float[3]){0, 0, 0}, 0, 4.5f, 1 / 60.0f);
        if (k.pos[1] > apex) apex = k.pos[1];
        if (k.grounded && landed < 0) landed = i;
    }
    CHECK(fabsf(apex - 4.5f * 4.5f / (2 * 9.81f)) < 0.06f && landed > 40 && k.grounded, "jump apex %g, landed after %d frames", apex, landed);
    /* the low ceiling (bottom at 2.2) stops the head */
    k.pos[0] = 12; k.pos[1] = 0; k.pos[2] = 0;
    walk(&w, &k, 0, 0, 5, 0);
    vcpe_kcc_step(&w, &k, (float[3]){0, 0, 0}, 1, 6.0f, 1 / 60.0f);
    float top = 0;
    for (int i = 0; i < 90; i++) { vcpe_kcc_step(&w, &k, (float[3]){0, 0, 0}, 0, 6, 1 / 60.0f); if (k.pos[1] > top) top = k.pos[1]; }
    CHECK(top + 1.8f < 2.2f + 0.02f && k.grounded, "head stops at the ceiling (feet %g)", top);

    /* a long fall onto the ground and onto a mesh top: no tunnelling */
    k.pos[0] = 0; k.pos[1] = 60; k.pos[2] = 3; k.grounded = 0; k.vel[1] = 0;
    for (int i = 0; i < 400; i++) vcpe_kcc_step(&w, &k, (float[3]){0, 0, 0}, 0, 0, 1 / 60.0f);
    CHECK(k.grounded && fabsf(k.pos[1]) < 1e-3f, "landed from 60 m (%g)", k.pos[1]);
    k.pos[0] = 12; k.pos[1] = 40; k.pos[2] = 0; k.grounded = 0; k.vel[1] = -30;
    for (int i = 0; i < 400; i++) vcpe_kcc_step(&w, &k, (float[3]){0, 0, 0}, 0, 0, 1 / 60.0f);
    CHECK(k.grounded && fabsf(k.pos[1] - 2.5f) < 1e-3f && k.ground_kind == VCPE_COL_MESH, "landed on the slab top (%g)", k.pos[1]);

    /* trees: never closer than both radii */
    k.pos[0] = 0.05f; k.pos[1] = 0; k.pos[2] = -2; k.grounded = 1;
    float closest = 99;
    for (int i = 0; i < 180; i++) {
        vcpe_kcc_step(&w, &k, (float[3]){0, 0, -2}, 0, 0, 1 / 60.0f);
        float d = sqrtf(k.pos[0] * k.pos[0] + (k.pos[2] + 5) * (k.pos[2] + 5));
        if (d < closest) closest = d;
    }
    CHECK(closest > 0.7f - 0.01f && k.pos[2] < -5.5f, "walked around the tree (closest %g, z %g)", closest, k.pos[2]);
    vcpe_col_remove_cylinder(&w, tree);
    CHECK(!vcpe_col_ray(&w, (float[3]){0, 1, 0}, (float[3]){0, 0, -1}, 4.9f, &h), "removed tree");

    /* a moved instance moves its collision */
    float moved[12] = {2, 0, 0, 40, 0, 2, 0, 1, 0, 0, 2, 0};
    vcpe_col_move(&w, cid, moved);
    CHECK(vcpe_col_ray(&w, (float[3]){35, 1, 0}, (float[3]){1, 0, 0}, 100, &h) && h.id == cid && fabsf(h.t - 4) < 1e-3f, "moved cube (t %g)", h.t);
    vcpe_col_remove(&w, cid);
    CHECK(!vcpe_col_ray(&w, (float[3]){35, 1, 0}, (float[3]){1, 0, 0}, 20, &h), "removed cube");

    /* terrain through the callback: follow the hills, slide off the cliff band */
    VcpeColWorld hw;
    vcpe_col_world(&hw, 0, 0, 0, 0);
    vcpe_col_ground(&hw, hill_ground, 0, 0);
    k.pos[0] = 0; k.pos[1] = 2; k.pos[2] = 0; k.grounded = 0; k.vel[1] = 0;
    float worst = 0, y;
    float n[3];
    int ground_frames = 0;
    for (int i = 0; i < 600; i++) {
        vcpe_kcc_step(&hw, &k, (float[3]){3, 0, 1}, 0, 0, 1 / 60.0f);
        if (k.grounded && k.pos[0] < 29) { hill_ground(0, k.pos[0], k.pos[2], &y, n); worst = fmaxf(worst, fabsf(k.pos[1] - y)); ground_frames++; }
    }
    CHECK(worst < 0.005f && ground_frames > 400, "on the hills within %g m for %d frames", worst, ground_frames);
    CHECK(k.pos[0] < 31.5f && k.pos[1] < 3, "the cliff band is too steep to climb (x %g y %g)", k.pos[0], k.pos[1]);
    CHECK(vcpe_col_ray(&hw, (float[3]){0, 10, 0}, (float[3]){0.3f, -1, 0.2f}, 50, &h) && h.kind == VCPE_COL_GROUND, "ray onto the hills");
    hill_ground(0, h.point[0], h.point[2], &y, n);
    CHECK(fabsf(h.point[1] - y) < 2e-3f, "hit point on the surface (%g vs %g)", h.point[1], y);

    free(m.plane);
    free(cm.plane);
    printf("PASS collide: rays, instances, cylinders, ground; walls, sliding, no tunnelling, stairs, ramps, jumps, ceiling, falls, trees, terrain\n");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
