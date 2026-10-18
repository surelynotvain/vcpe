/* vcpe_rig host test: sampling against tests/rig/make.py references (synthetic rig and a converted glTF file),
 * plus masks, blending, additive layers, events, root motion, IK, skin matrices, attachment and blob validation. */
#include "vcpe_rig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *dir;
static struct { char name[32]; void *mem; uint32_t size; } blobs[4];

static void *load(const char *name, uint32_t *size)
{
    for (int i = 0; i < 4; i++)
        if (blobs[i].mem && !strcmp(blobs[i].name, name)) { *size = blobs[i].size; return blobs[i].mem; }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *m = aligned_alloc(16, (size_t)(n + 15) & ~(size_t)15);
    if (fread(m, 1, (size_t)n, f) != (size_t)n) exit(1);
    fclose(f);
    for (int i = 0; i < 4; i++)
        if (!blobs[i].mem) { snprintf(blobs[i].name, sizeof blobs[i].name, "%s", name); blobs[i].mem = m; blobs[i].size = (uint32_t)n; break; }
    *size = (uint32_t)n;
    return m;
}
static VcpeRig open_rig(const char *name)
{
    uint32_t n;
    void *m = load(name, &n);
    VcpeRig r;
    if (vcpe_rig_open(&r, m, n)) { printf("cannot open %s\n", name); exit(1); }
    return r;
}
static float qangle(const float a[4], const float b[4])
{
    /* the relative rotation's vector part: precise for small angles, unlike acos of a float dot product */
    double x = (double)a[3] * b[0] - (double)a[0] * b[3] - (double)a[1] * b[2] + (double)a[2] * b[1];
    double y = (double)a[3] * b[1] + (double)a[0] * b[2] - (double)a[1] * b[3] - (double)a[2] * b[0];
    double z = (double)a[3] * b[2] - (double)a[0] * b[1] + (double)a[1] * b[0] - (double)a[2] * b[3];
    double w = (double)a[3] * b[3] + (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
    return (float)(2 * atan2(sqrt(x * x + y * y + z * z), fabs(w)));
}
static float dist3(const float a[3], const float b[3])
{
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

static VcpeXform pose[VCPE_RIG_MAX_BONES], other[VCPE_RIG_MAX_BONES];
static float model[VCPE_RIG_MAX_BONES][12], skin[VCPE_RIG_MAX_BONES][12] __attribute__((aligned(16)));

static void references(void)
{
    char path[512], line[512];
    snprintf(path, sizeof path, "%s/expect.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { printf("missing expect.txt\n"); exit(1); }
    int poses = 0, models = 0, verts = 0, motions = 0;
    while (fgets(line, sizeof line, f)) {
        char kind[16], blob[32], mesh[32];
        int clip, loop, bone;
        float t, v[8];
        if (sscanf(line, "%15s", kind) != 1) continue;
        if (!strcmp(kind, "pose") && sscanf(line, "%*s %31s %d %f %d %d %f %f %f %f %f %f %f %f", blob, &clip, &t, &loop, &bone,
                                            v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7) == 13) {
            VcpeRig r = open_rig(blob);
            vcpe_rig_sample(&r, clip, t, loop, pose, 0);
            CHECK(qangle(pose[bone].r, v) < 2e-4f, "pose %s clip %d t %g bone %d rotation off by %g rad", blob, clip, t, bone, qangle(pose[bone].r, v));
            CHECK(dist3(pose[bone].t, v + 4) < 1e-3f, "pose %s clip %d t %g bone %d translation %g %g %g", blob, clip, t, bone, pose[bone].t[0], pose[bone].t[1], pose[bone].t[2]);
            CHECK(fabsf(pose[bone].s - v[7]) < 1e-4f, "pose scale %g vs %g", pose[bone].s, v[7]);
            poses++;
        } else if (!strcmp(kind, "model") && sscanf(line, "%*s %31s %d %f %d %d %f %f %f", blob, &clip, &t, &loop, &bone, v, v + 1, v + 2) == 8) {
            VcpeRig r = open_rig(blob);
            vcpe_rig_sample(&r, clip, t, loop, pose, 0);
            vcpe_rig_model(&r, pose, model);
            float p[3] = {model[bone][3], model[bone][7], model[bone][11]};
            CHECK(dist3(p, v) < 2e-3f, "model %s clip %d t %g bone %d at %g %g %g, expected %g %g %g", blob, clip, t, bone, p[0], p[1], p[2], v[0], v[1], v[2]);
            VcpeXform x;
            vcpe_rig_model_xform(&r, pose, bone, &x);
            CHECK(dist3(x.t, p) < 1e-4f, "model_xform agrees with model matrices");
            models++;
        } else if (!strcmp(kind, "skinv") && sscanf(line, "%*s %31s %31s %d %f %d %f %f %f", blob, mesh, &clip, &t, &bone, v, v + 1, v + 2) == 8) {
            VcpeRig r = open_rig(blob);
            uint32_t n;
            void *mm = load(mesh, &n);
            VcpeSkinMesh sm;
            CHECK(!vcpe_skinmesh_open(&sm, mm, n), "skinned mesh opens");
            VcpeVtx out[64];
            vcpe_skinmesh_init(&sm, out, 0xffffffffu);
            vcpe_rig_sample(&r, clip, t, 1, pose, 0);
            vcpe_rig_model(&r, pose, model);
            vcpe_rig_skin(&r, (const float (*)[12])model, skin);
            CHECK(!vcpe_skinmesh_skin(&sm, 0, (const float (*)[12])skin, r.nbones, out), "skinning succeeds");
            float p[3] = {out[bone].x, out[bone].y, out[bone].z};
            CHECK(dist3(p, v) < 3e-3f, "skinned vertex %d at t %g: %g %g %g, glTF says %g %g %g", bone, t, p[0], p[1], p[2], v[0], v[1], v[2]);
            verts++;
        } else if (!strcmp(kind, "motion") && sscanf(line, "%*s %31s %d %f %f %d %f %f %f %f", blob, &clip, &t, v + 7, &loop, v, v + 1, v + 2, v + 3) == 9) {
            VcpeRig r = open_rig(blob);
            float d[3], yaw;
            vcpe_rig_motion(&r, clip, t, v[7], loop, d, &yaw);
            CHECK(dist3(d, v) < 1e-3f && fabsf(yaw - v[3]) < 1e-3f, "motion %g %g %g yaw %g, expected %g %g %g yaw %g", d[0], d[1], d[2], yaw, v[0], v[1], v[2], v[3]);
            motions++;
        }
    }
    fclose(f);
    CHECK(poses > 90 && models > 90 && verts == 12 && motions == 1, "reference counts %d %d %d %d", poses, models, verts, motions);
    printf("PASS %d poses, %d bone positions, %d skinned glTF vertices and root motion match the Python references\n", poses, models, verts);
}

static int ev_count;
static uint32_t ev_ids[16];
static void on_event(void *user, int clip, const VcpeRigEvent *e)
{
    (void)user; (void)clip;
    if (ev_count < 16) ev_ids[ev_count] = e->id;
    ev_count++;
}

static void compose_motion(float p[3], float *yaw, const float q[3], float qyaw)
{
    float c = cosf(*yaw), s = sinf(*yaw);
    p[0] += q[0] * c + q[2] * s;
    p[2] += -q[0] * s + q[2] * c;
    *yaw += qyaw;
}

static void units(void)
{
    VcpeRig r = open_rig("synth.vrg");
    uint32_t n;
    uint8_t *raw = load("synth.vrg", &n);
    CHECK(r.nbones == 7 && r.nclips == 4 && r.motion_bone == 0, "rig header");
    CHECK(vcpe_rig_bone_named(&r, "calf") == 3 && vcpe_rig_bone(&r, vcpe_rig_hash("head")) == 6 && vcpe_rig_bone_named(&r, "tail") < 0, "bone lookup");
    CHECK(vcpe_rig_clip_named(&r, "wave") == 1 && vcpe_rig_clip_named(&r, "nope") < 0, "clip lookup");
    CHECK(fabsf(vcpe_rig_length(&r, 1) - 2) < 1e-6f && vcpe_rig_flags(&r, 0) == (VCPE_RIG_LOOP | VCPE_RIG_MOTION_XZ | VCPE_RIG_MOTION_YAW), "clip info");

    /* validation */
    VcpeRig bad;
    uint8_t *copy = aligned_alloc(16, (n + 15) & ~15u);
    memcpy(copy, raw, n);
    CHECK(vcpe_rig_open(&bad, copy, n - 1) == -2, "truncated blob refused");
    copy[0] = 'X';
    CHECK(vcpe_rig_open(&bad, copy, n) == -1, "wrong magic refused");
    copy[0] = 'V';
    VcpeRigBone *bb = (VcpeRigBone *)(copy + 32);
    bb[2].parent = 5;
    CHECK(vcpe_rig_open(&bad, copy, n) == -2, "child before parent refused");
    bb[2].parent = 1;
    VcpeRigClip *cc = (VcpeRigClip *)(copy + 32 + ((88 * 7 + 15) & ~15));
    cc[0].tracks = n;
    CHECK(vcpe_rig_open(&bad, copy, n) == -2, "track table out of bounds refused");
    cc[0].tracks = ((VcpeRigClip *)(raw + 32 + ((88 * 7 + 15) & ~15)))[0].tracks;
    CHECK(vcpe_rig_open(&bad, copy, n) == 0, "repaired copy opens");
    CHECK(vcpe_rig_open(&bad, copy + 1, n - 1) == -2 || vcpe_rig_open(&bad, copy + 1, n - 1) == -1, "misaligned blob refused");
    free(copy);

    /* masks */
    uint8_t mask[VCPE_RIG_MAX_BONES] = {0};
    vcpe_rig_mask(&r, mask, 5, 255);
    CHECK(mask[5] == 255 && mask[6] == 255 && !mask[0] && !mask[1] && !mask[2] && !mask[4], "spine mask holds spine and head only");
    vcpe_rig_mask(&r, mask, 2, 128);
    CHECK(mask[2] == 128 && mask[3] == 128 && mask[4] == 128 && mask[5] == 255, "second mask adds the leg");
    for (int i = 0; i < r.nbones; i++) pose[i].s = -7;
    vcpe_rig_sample(&r, 1, 1.0f, 0, pose, mask);
    CHECK(pose[0].s == -7 && pose[1].s == -7 && pose[5].s > 0 && pose[2].s > 0, "masked sampling leaves other bones alone");

    /* bind pose skins to identity */
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_model(&r, pose, model);
    vcpe_rig_skin(&r, (const float (*)[12])model, skin);
    float worst = 0;
    for (int b = 0; b < r.nbones; b++)
        for (int k = 0; k < 12; k++) {
            float e = fabsf(skin[b][k] - ((k == 0 || k == 5 || k == 10) ? 1.0f : 0.0f));
            if (e > worst) worst = e;
        }
    CHECK(worst < 1e-5f, "bind pose gives identity skin matrices (%g)", worst);

    /* blending */
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_sample(&r, 1, 1.0f, 0, other, 0);     /* spine at its 40 degree peak */
    float spine_full = qangle(other[5].r, r.bones[5].r);
    vcpe_rig_blend(&r, pose, other, 0.5f, 0);
    CHECK(fabsf(qangle(pose[5].r, r.bones[5].r) - spine_full * 0.5f) < 2e-3f, "half blend is half the angle (%g of %g)", qangle(pose[5].r, r.bones[5].r), spine_full);
    CHECK(fabsf(pose[1].t[0] - 0.1f) < 1e-4f, "translations blend linearly");
    memset(mask, 0, sizeof mask);
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_blend(&r, pose, other, 1.0f, mask);
    CHECK(qangle(pose[5].r, r.bones[5].r) < 1e-6f, "zero mask blends nothing");

    /* additive */
    vcpe_rig_sample_delta(&r, 3, 1.0f, 0, other, 0);
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_add(&r, pose, other, 1.0f, 0);
    CHECK(fabsf(qangle(pose[5].r, r.bones[5].r) - 20 * 3.14159265f / 180) < 1e-3f && fabsf(pose[5].t[2] - 0.1f) < 1e-3f, "additive lean applies its delta");
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_add(&r, pose, other, 0.5f, 0);
    CHECK(fabsf(qangle(pose[5].r, r.bones[5].r) - 10 * 3.14159265f / 180) < 2e-3f, "half additive weight, half the lean");
    CHECK(qangle(pose[2].r, r.bones[2].r) < 1e-6f, "additive leaves unanimated bones");

    /* events */
    ev_count = 0;
    CHECK(vcpe_rig_events(&r, 1, -1e-6f, 2.0f, 0, on_event, 0) == 3 && ev_ids[0] == vcpe_rig_hash("start") && ev_ids[2] == vcpe_rig_hash("end"), "one-shot events fire in order");
    ev_count = 0;
    CHECK(vcpe_rig_events(&r, 1, 0.5f, 1.0f, 0, on_event, 0) == 1 && ev_ids[0] == vcpe_rig_hash("mid"), "event window is (from, to]");
    VcpeRig g = open_rig("model.vrg");
    ev_count = 0;
    float gl = vcpe_rig_length(&g, 0);
    CHECK(vcpe_rig_events(&g, 0, 0, 3 * gl - 0.01f, 1, on_event, 0) == 3, "looped events repeat every cycle (%d)", ev_count);
    CHECK(vcpe_rig_flags(&g, 0) & VCPE_RIG_LOOP, "converter --loop flag");

    /* root motion composes across splits and wraps */
    float a[3], b[3], whole[3], ya, yb, yw;
    vcpe_rig_motion(&r, 0, 0.2f, 0.6f, 1, a, &ya);
    vcpe_rig_motion(&r, 0, 0.6f, 1.7f, 1, b, &yb);
    vcpe_rig_motion(&r, 0, 0.2f, 1.7f, 1, whole, &yw);
    compose_motion(a, &ya, b, yb);
    CHECK(dist3(a, whole) < 2e-3f && fabsf(ya - yw) < 2e-3f, "motion over 0.2..1.7 equals 0.2..0.6 then 0.6..1.7");
    vcpe_rig_motion(&r, 1, 0, 1, 0, a, &ya);
    CHECK(a[0] == 0 && a[2] == 0 && ya == 0, "clips without root motion report none");
    vcpe_rig_sample(&r, 0, 0.5f, 1, pose, 0);
    CHECK(fabsf(pose[0].t[2]) < 1e-4f && fabsf(vcpe_quat_yaw(pose[0].r)) < 1e-3f, "root motion is taken out of the pose");
    CHECK(fabsf(pose[0].t[1]) < 1e-3f + 0.05f, "vertical root movement stays in the pose");

    /* IK on the leg */
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_model(&r, pose, model);
    float hip[3] = {model[2][3], model[2][7], model[2][11]};
    float target[3] = {hip[0] + 0.1f, hip[1] - 0.7f, hip[2] + 0.2f}, pole[3] = {hip[0], hip[1] - 0.4f, hip[2] - 1.0f};
    VcpeXform foot_before;
    vcpe_rig_model_xform(&r, pose, 4, &foot_before);
    CHECK(vcpe_rig_ik2(&r, pose, 2, 3, 4, target, pole, 1, 1) == 1, "reachable target");
    vcpe_rig_model(&r, pose, model);
    float foot[3] = {model[4][3], model[4][7], model[4][11]}, knee[3] = {model[3][3], model[3][7], model[3][11]};
    CHECK(dist3(foot, target) < 1e-4f, "foot on target (%g away)", dist3(foot, target));
    CHECK(knee[2] < (hip[2] + target[2]) * 0.5f, "knee bends toward the pole (z %g)", knee[2]);
    CHECK(fabsf(dist3(knee, hip) - 0.45f) < 1e-4f && fabsf(dist3(knee, foot) - 0.45f) < 1e-4f, "bone lengths kept");
    VcpeXform foot_after;
    vcpe_rig_model_xform(&r, pose, 4, &foot_after);
    CHECK(qangle(foot_after.r, foot_before.r) < 1e-4f, "planted foot keeps its rotation");
    float far_target[3] = {hip[0], hip[1] - 3, hip[2]};
    vcpe_rig_bind_pose(&r, pose);
    CHECK(vcpe_rig_ik2(&r, pose, 2, 3, 4, far_target, pole, 1, 0) == 0, "out of reach reported");
    vcpe_rig_model(&r, pose, model);
    CHECK(fabsf(model[4][7] - (hip[1] - 0.9f)) < 1e-3f, "chain straightens toward an unreachable target");
    CHECK(vcpe_rig_ik2(&r, pose, 2, 4, 3, target, pole, 1, 0) == -1, "broken chain refused");
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_ik2(&r, pose, 2, 3, 4, target, pole, 0.5f, 0);
    vcpe_rig_model(&r, pose, model);
    float half[3] = {model[4][3], model[4][7], model[4][11]};
    CHECK(dist3(half, target) > 0.01f && dist3(half, target) < dist3(hip, target), "half weight moves part way");

    /* attachment */
    float world[16] = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, -5, 0, 0, 0, 1}, att[16];
    vcpe_rig_bind_pose(&r, pose);
    vcpe_rig_model(&r, pose, model);
    vcpe_rig_attach(world, model[6], 0, att);
    CHECK(fabsf(att[3] - (10 + model[6][3])) < 1e-5f && fabsf(att[7] - model[6][7]) < 1e-5f && fabsf(att[11] - (model[6][11] - 5)) < 1e-5f && att[15] == 1, "attach matrix");

    /* quaternion helpers */
    float qy[4] = {0, sinf(0.25f), 0, cosf(0.25f)}, v[3] = {1, 0, 0}, rv[3];
    CHECK(fabsf(vcpe_quat_yaw(qy) - 0.5f) < 1e-6f, "yaw");
    vcpe_quat_rotate(qy, v, rv);
    CHECK(fabsf(rv[0] - cosf(0.5f)) < 1e-6f && fabsf(rv[2] + sinf(0.5f)) < 1e-6f, "positive yaw turns +X toward -Z");
    float from[3] = {0, 1, 0}, to[3] = {0, -1, 0}, qb[4];
    vcpe_quat_between(qb, from, to);
    vcpe_quat_rotate(qb, from, rv);
    CHECK(dist3(rv, to) < 1e-5f, "opposite vectors");

    /* skinned mesh */
    void *mm = load("model.vsm", &n);
    VcpeSkinMesh sm;
    CHECK(!vcpe_skinmesh_open(&sm, mm, n) && sm.nverts == 4 && sm.nsubs == 1 && sm.subs[0].page == 5 && sm.nlevels == 1 && sm.nbones == 2, "mesh header, --page");
    VcpeVtx out[4];
    vcpe_skinmesh_init(&sm, out, 0xff808080u);
    CHECK((out[0].color & 0xff) == 0x80 && ((out[0].color >> 24) & 0xff) == 0xff && out[1].u == 1, "template copy with tint");
    CHECK(vcpe_skinmesh_skin(&sm, 0, (const float (*)[12])skin, 1, out) == -1, "rig too small for the mesh refused");
    uint8_t *mcopy = aligned_alloc(16, (n + 15) & ~15u);
    memcpy(mcopy, mm, n);
    uint16_t *ix = (uint16_t *)(mcopy + sm.subs[0].ioff[0]);
    ix[1] = 9;
    CHECK(vcpe_skinmesh_open(&sm, mcopy, n) == -2, "index beyond the vertices refused");
    free(mcopy);
    VcpeLodView view = {{0, 0, 0}, 300, 1, 0.75f};
    CHECK(!vcpe_skinmesh_open(&sm, mm, n) && vcpe_skinmesh_lod(&sm, &view, sm.centre, 1, -1) == 0, "single-level mesh picks level 0");
    /* LOD levels: prefixes of the vertex array, fewer triangles, submeshes kept apart */
    mm = load("tube.vsm", &n);
    CHECK(!vcpe_skinmesh_open(&sm, mm, n) && sm.nlevels >= 2 && sm.nsubs == 2, "tube with %d levels", sm.nlevels);
    for (int l = 1; l < sm.nlevels; l++) {
        uint32_t tris = 0, prev = 0;
        for (int s = 0; s < sm.nsubs; s++) { tris += sm.subs[s].nidx[l]; prev += sm.subs[s].nidx[l - 1]; }
        CHECK(sm.levels[l].nverts < sm.levels[l - 1].nverts && tris < prev, "level %d coarser (%u < %u indices)", l, tris, prev);
    }
    CHECK(sm.subs[0].page == 1 && sm.subs[1].page == 2, "submesh pages");
    VcpeLodView near_view = {{0, 0, 2}, 300, 1, 0.75f}, far_view = {{0, 0, 400}, 300, 1, 0.75f};
    int ln = vcpe_skinmesh_lod(&sm, &near_view, sm.centre, 1, -1), lf = vcpe_skinmesh_lod(&sm, &far_view, sm.centre, 1, -1);
    CHECK(ln == 0 && lf == sm.nlevels - 1, "near picks full detail (%d), far the coarsest (%d)", ln, lf);
    float id[2][12] __attribute__((aligned(16))) = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
    static VcpeVtx tube[400];
    for (int i = 0; i < sm.nverts; i++) tube[i].x = tube[i].y = tube[i].z = -99;
    vcpe_skinmesh_init(&sm, tube, 0xffffffffu);
    for (int i = 0; i < sm.nverts; i++) tube[i].x = -99;
    CHECK(!vcpe_skinmesh_skin(&sm, sm.nlevels - 1, (const float (*)[12])id, 2, tube), "skin the coarsest level");
    int written = 0;
    for (int i = 0; i < sm.nverts; i++) written += tube[i].x != -99;
    CHECK(written == sm.levels[sm.nlevels - 1].nverts, "only the level's prefix is skinned (%d of %d)", written, sm.nverts);
    printf("PASS masks, blending, additive, events, root motion, IK, skin matrices, attachment, validation, mesh LOD\n");
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : ".";
    references();
    units();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
