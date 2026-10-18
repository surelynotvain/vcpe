/* vcpe_animator host test: crossfades, blend spaces, layers, queues, events, root motion and foot IK. */
#include "vcpe_animator.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static VcpeRig rig;
static int IDLE, WALK, RUN, SWING, BREATHE, TURN;
static float qangle(const float a[4], const float b[4])
{
    double x = (double)a[3] * b[0] - (double)a[0] * b[3] - (double)a[1] * b[2] + (double)a[2] * b[1];
    double y = (double)a[3] * b[1] + (double)a[0] * b[2] - (double)a[1] * b[3] - (double)a[2] * b[0];
    double z = (double)a[3] * b[2] - (double)a[0] * b[1] + (double)a[1] * b[0] - (double)a[2] * b[3];
    double w = (double)a[3] * b[3] + (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
    return (float)(2 * atan2(sqrt(x * x + y * y + z * z), fabs(w)));
}
static float max_pose_diff(const VcpeXform *a, const VcpeXform *b)
{
    float worst = 0;
    for (int i = 0; i < rig.nbones; i++) {
        float d = qangle(a[i].r, b[i].r);
        for (int k = 0; k < 3; k++) d = fmaxf(d, fabsf(a[i].t[k] - b[i].t[k]));
        worst = fmaxf(worst, d);
    }
    return worst;
}

static int events[64], nev;
static uint32_t ev_id[64];
static void on_event(void *user, int layer, int clip, uint32_t id, int32_t arg)
{
    (void)user;
    if (nev < 64) { events[nev] = layer * 1000 + clip * 10 + arg; ev_id[nev] = id; }
    nev++;
}

static uint8_t mem[2][4096] __attribute__((aligned(16)));
static VcpeXform ref[VCPE_RIG_MAX_BONES], ref2[VCPE_RIG_MAX_BONES], prev[VCPE_RIG_MAX_BONES];

static float slope_h(float x) { return x > 5 ? 0.0f : -0.15f; }   /* world +X of the character at x = 5 (left foot) on the shelf, right foot lower */
static int ground(void *user, float x, float z, float *h, float n[3])
{
    (void)user; (void)z;
    *h = slope_h(x);
    n[0] = 0; n[1] = 1; n[2] = 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    static uint8_t blob[65536] __attribute__((aligned(16)));
    size_t n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    CHECK(!vcpe_rig_open(&rig, blob, (uint32_t)n), "rig opens");
    IDLE = vcpe_rig_clip_named(&rig, "idle"); WALK = vcpe_rig_clip_named(&rig, "walk"); RUN = vcpe_rig_clip_named(&rig, "run");
    SWING = vcpe_rig_clip_named(&rig, "swing"); BREATHE = vcpe_rig_clip_named(&rig, "breathe"); TURN = vcpe_rig_clip_named(&rig, "turn");
    int spine = vcpe_rig_bone_named(&rig, "spine"), arm = vcpe_rig_bone_named(&rig, "arm"), thigh = vcpe_rig_bone_named(&rig, "thigh.L");
    CHECK(vcpe_animator_bytes(&rig) == 160u * (uint32_t)rig.nbones && vcpe_animator_bytes(&rig) <= sizeof mem[0], "memory per bone");

    VcpeAnimator a;
    CHECK(vcpe_animator_init(&a, &rig, mem[0] + 4) == -1, "misaligned memory refused");
    CHECK(!vcpe_animator_init(&a, &rig, mem[0]), "init");
    CHECK(fabsf(a.skin[3][0] - 1) < 1e-5f && fabsf(a.skin[3][3]) < 1e-5f, "starts in the bind pose");
    vcpe_animator_events(&a, on_event, 0);

    /* a clip with no fade is exactly the clip */
    vcpe_animator_play(&a, 0, IDLE, 0, 1, -1);
    for (int i = 0; i < 10; i++) vcpe_animator_update(&a, 1 / 60.0f);
    vcpe_animator_evaluate(&a, 1);
    vcpe_rig_sample(&rig, IDLE, 10 / 60.0f, 1, ref, 0);
    CHECK(max_pose_diff(a.local, ref) < 1e-5f, "single clip = vcpe_rig_sample");

    /* crossfade idle -> walk over 0.2 s: halfway the weight is ease(0.5) = 0.5 */
    vcpe_animator_play(&a, 0, WALK, 0.2f, 1, -1);
    for (int i = 0; i < 6; i++) vcpe_animator_update(&a, 1 / 60.0f);
    vcpe_animator_evaluate(&a, 1);
    vcpe_rig_sample(&rig, IDLE, 16 / 60.0f, 1, ref, 0);
    vcpe_rig_sample(&rig, WALK, 6 / 60.0f, 1, ref2, 0);
    vcpe_rig_blend(&rig, ref, ref2, 0.5f, 0);
    CHECK(max_pose_diff(a.local, ref) < 1e-4f, "mid-crossfade pose is the 50 %% blend (%g)", max_pose_diff(a.local, ref));
    CHECK(a.layer[0].count == 2, "two motions while fading");
    for (int i = 0; i < 7; i++) vcpe_animator_update(&a, 1 / 60.0f);
    CHECK(a.layer[0].count == 1 && vcpe_animator_current(&a, 0) == WALK, "faded in: idle dropped");

    /* interrupting crossfades never pops: run, then idle again mid-fade, then walk */
    vcpe_animator_evaluate(&a, 1);
    memcpy(prev, a.local, sizeof(VcpeXform) * rig.nbones);
    float worst = 0;
    for (int i = 0; i < 90; i++) {
        if (i == 0) vcpe_animator_play(&a, 0, RUN, 0.3f, 1, -1);
        if (i == 8) vcpe_animator_play(&a, 0, IDLE, 0.3f, 1, -1);
        if (i == 12) vcpe_animator_play(&a, 0, WALK, 0.25f, 1, -1);
        if (i == 13) vcpe_animator_play(&a, 0, RUN, 0.25f, 1, -1);
        if (i == 14) vcpe_animator_play(&a, 0, IDLE, 0.25f, 1, -1);   /* a fifth motion: the oldest is dropped */
        vcpe_animator_update(&a, 1 / 60.0f);
        vcpe_animator_evaluate(&a, 1);
        worst = fmaxf(worst, max_pose_diff(a.local, prev));
        memcpy(prev, a.local, sizeof(VcpeXform) * rig.nbones);
    }
    CHECK(worst < 0.12f, "largest per-frame change through interrupted crossfades: %g", worst);
    CHECK(a.layer[0].count == 1 && vcpe_animator_current(&a, 0) == IDLE, "ends on the last request");

    /* blend space: idle 0, walk 1.4, run 3.0 m/s; root motion speed follows the parameter */
    static const VcpeBlendSpace move = {{0, 1, 2}, {0, 1.4f, 5.0f}, 3};   /* run: 3 m per 0.6 s cycle */
    VcpeBlendSpace sp = move;
    sp.clip[0] = (int16_t)IDLE; sp.clip[1] = (int16_t)WALK; sp.clip[2] = (int16_t)RUN;
    float dp[3], dyaw, speeds[4], params[4] = {0, 1.4f, 3.2f, 5.0f};
    for (int k = 0; k < 4; k++) {
        vcpe_animator_init(&a, &rig, mem[0]);
        vcpe_animator_play_space(&a, 0, &sp, params[k], 0, 1);
        for (int i = 0; i < 120; i++) vcpe_animator_update(&a, 1 / 60.0f);
        vcpe_animator_take_motion(&a, dp, &dyaw);
        speeds[k] = -dp[2] / 2.0f;
        CHECK(fabsf(dp[0]) < 1e-3f && fabsf(dyaw) < 1e-3f, "straight travel");
    }
    CHECK(speeds[0] < 1e-4f && fabsf(speeds[1] - 1.4f) < 0.01f && fabsf(speeds[3] - 5.0f) < 0.01f, "walk 1.4 and run 5.0 m/s (%g %g %g)", speeds[0], speeds[1], speeds[3]);
    CHECK(speeds[2] > speeds[1] && speeds[2] < speeds[3], "in-between speed (%g)", speeds[2]);
    vcpe_animator_take_motion(&a, dp, &dyaw);
    CHECK(dp[2] == 0 && dyaw == 0, "take_motion clears");

    /* phase sync: footsteps of the dominant clip, 2 per cycle */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_events(&a, on_event, 0);
    vcpe_animator_play_space(&a, 0, &sp, 1.4f, 0, 1);
    nev = 0;
    for (int i = 0; i < 119; i++) vcpe_animator_update(&a, 1 / 60.0f);
    CHECK(nev == 4 && ev_id[0] == vcpe_rig_hash("foot") && events[0] == WALK * 10 && events[1] == WALK * 10 + 1, "walk footsteps in 2 s: %d", nev);
    vcpe_animator_param(&a, 0, 4.8f);
    nev = 0;
    for (int i = 0; i < 60; i++) vcpe_animator_update(&a, 1 / 60.0f);
    CHECK(nev >= 3 && events[0] / 10 == RUN, "near-run parameter: run footsteps, faster (%d in 1 s)", nev);
    /* blending walk and run at one phase keeps the legs in step: thigh angles share a sign */
    vcpe_animator_param(&a, 0, 3.2f);
    int same = 0, total = 0;
    for (int i = 0; i < 40; i++) {
        vcpe_animator_update(&a, 1 / 60.0f);
        vcpe_animator_evaluate(&a, 1);
        float ph = vcpe_animator_time(&a, 0);
        vcpe_rig_sample(&rig, WALK, ph * vcpe_rig_length(&rig, WALK), 1, ref, 0);
        vcpe_rig_sample(&rig, RUN, ph * vcpe_rig_length(&rig, RUN), 1, ref2, 0);
        if (fabsf(ref[thigh].r[0]) > 0.02f) { total++; same += (ref[thigh].r[0] > 0) == (ref2[thigh].r[0] > 0) && (a.local[thigh].r[0] > 0) == (ref[thigh].r[0] > 0); }
    }
    CHECK(total > 10 && same == total, "phase-synced legs (%d/%d)", same, total);

    /* upper-body one-shot on a masked layer: the legs keep walking, the hit fires once, the layer fades out */
    static uint8_t upper[VCPE_RIG_MAX_BONES];
    vcpe_rig_mask(&rig, upper, spine, 255);
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_events(&a, on_event, 0);
    vcpe_animator_play(&a, 0, WALK, 0, 1, -1);
    vcpe_animator_layer(&a, 1, upper, 0);
    vcpe_animator_play(&a, 1, SWING, 0.05f, 1, -1);
    nev = 0;
    float peak = 0, base_err = 0;
    for (int i = 1; i <= 45; i++) {
        vcpe_animator_update(&a, 1 / 60.0f);
        vcpe_animator_evaluate(&a, VCPE_ANIM_LAYERS);
        vcpe_rig_sample(&rig, WALK, i / 60.0f, 1, ref, 0);
        base_err = fmaxf(base_err, qangle(a.local[thigh].r, ref[thigh].r));
        peak = fmaxf(peak, qangle(a.local[arm].r, ref[arm].r));
    }
    int hits = 0, hit_ok = 1;
    for (int i = 0; i < nev && i < 64; i++)
        if (events[i] >= 1000) { hits++; hit_ok &= events[i] == 1000 + SWING * 10 + 42 && ev_id[i] == vcpe_rig_hash("hit"); }
    CHECK(hits == 1 && hit_ok, "one hit event on layer 1 (%d)", hits);
    CHECK(base_err < 1e-5f && peak > 1.2f, "legs untouched (%g), arm swings (%g rad)", base_err, peak);
    CHECK(vcpe_animator_layer_weight_now(&a, 1) == 0 && a.layer[1].count == 0, "one-shot layer faded out and emptied");
    vcpe_animator_evaluate(&a, VCPE_ANIM_LAYERS);
    vcpe_rig_sample(&rig, WALK, 45 / 60.0f, 1, ref, 0);
    CHECK(max_pose_diff(a.local, ref) < 1e-5f, "after the swing: walk only");

    /* evaluate(1): far characters blend only the base layer */
    vcpe_animator_play(&a, 1, SWING, 0, 1, -1);
    vcpe_animator_update(&a, 0.2f);
    vcpe_animator_evaluate(&a, 1);
    vcpe_rig_sample(&rig, WALK, 45 / 60.0f + 0.2f, 1, ref, 0);
    CHECK(max_pose_diff(a.local, ref) < 1e-5f, "base layer only");

    /* additive breathing on top */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_play(&a, 0, IDLE, 0, 1, -1);
    vcpe_animator_layer(&a, 2, 0, 1);
    vcpe_animator_play(&a, 2, BREATHE, 0, 1, -1);
    vcpe_animator_update(&a, 0.5f);
    vcpe_animator_evaluate(&a, VCPE_ANIM_LAYERS);
    vcpe_rig_sample(&rig, IDLE, 0.5f, 1, ref, 0);
    vcpe_rig_sample_delta(&rig, BREATHE, 0.5f, 1, ref2, 0);
    vcpe_rig_add(&rig, ref, ref2, 1, 0);
    CHECK(max_pose_diff(a.local, ref) < 1e-5f && qangle(a.local[spine].r, rig.bones[spine].r) > 0.05f, "additive layer = idle + breathe delta");

    /* queue: a base-layer one-shot hands over to idle, finishing the fade at its last frame */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_play(&a, 0, SWING, 0, 1, 0);
    vcpe_animator_queue(&a, 0, IDLE, 0.1f, 1, -1);
    for (int i = 0; i < 22; i++) vcpe_animator_update(&a, 1 / 60.0f);
    CHECK(vcpe_animator_current(&a, 0) == SWING && !vcpe_animator_finished(&a, 0), "still swinging at 0.37 s");
    for (int i = 0; i < 8; i++) vcpe_animator_update(&a, 1 / 60.0f);
    CHECK(vcpe_animator_current(&a, 0) == IDLE && a.layer[0].count == 1, "idle took over by the end of the swing");

    /* finished / hold last frame without a queue */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_play(&a, 0, SWING, 0, 1, 0);
    vcpe_animator_update(&a, 1.0f);
    CHECK(vcpe_animator_finished(&a, 0) && fabsf(vcpe_animator_time(&a, 0) - 0.5f) < 1e-6f, "one-shot holds its last frame");

    /* turning root motion, independent of evaluate */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_play(&a, 0, TURN, 0, 1, -1);
    for (int i = 0; i < 60; i++) vcpe_animator_update(&a, 1 / 60.0f);
    vcpe_animator_take_motion(&a, dp, &dyaw);
    CHECK(fabsf(dyaw - 1.5707963f) < 2e-3f, "a quarter turn (%g)", dyaw);
    vcpe_animator_evaluate(&a, 1);
    CHECK(fabsf(vcpe_quat_yaw(a.local[0].r)) < 1e-3f, "the turn stays out of the pose");
    /* walking after turning: travel composes in the turned frame */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_play(&a, 0, TURN, 0, 1, -1);
    vcpe_animator_queue(&a, 0, WALK, 0, 1, -1);
    for (int i = 0; i < 120; i++) vcpe_animator_update(&a, 1 / 60.0f);
    vcpe_animator_take_motion(&a, dp, &dyaw);
    CHECK(fabsf(dyaw - 1.5707963f) < 5e-3f && dp[0] < -1.2f && fabsf(dp[2]) < 0.1f, "walked along -X after turning left (%g %g)", dp[0], dp[2]);

    /* fading in from the bind pose and stopping back to it */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_play(&a, 0, SWING, 0.2f, 1, 0);
    CHECK(a.layer[0].count == 2, "first play with a fade starts from the bind pose");
    vcpe_animator_update(&a, 0.3f);
    vcpe_animator_stop(&a, 0, 0.1f);
    vcpe_animator_update(&a, 0.2f);
    vcpe_animator_evaluate(&a, 1);
    vcpe_rig_bind_pose(&rig, ref);
    CHECK(max_pose_diff(a.local, ref) < 1e-6f, "stopped: bind pose");

    /* foot IK on a step: right foot 15 cm lower */
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_evaluate(&a, 1);
    VcpeFootIK ik;
    memset(&ik, 0, sizeof ik);
    ik.thigh[0] = thigh; ik.calf[0] = thigh + 1; ik.foot[0] = thigh + 2;
    ik.thigh[1] = vcpe_rig_bone_named(&rig, "thigh.R"); ik.calf[1] = ik.thigh[1] + 1; ik.foot[1] = ik.thigh[1] + 2;
    ik.pelvis = vcpe_rig_bone_named(&rig, "pelvis");
    ik.max_drop = 0.3f; ik.max_raise = 0.3f;
    float world[16] = {1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 2, 0, 0, 0, 1};
    CHECK(vcpe_animator_feet(&a, &ik, world, ground, 0, 1 / 60.0f) == 2, "both feet");
    CHECK(vcpe_animator_feet(&a, &ik, world, ground, 0, 1 / 60.0f) == 0, "not twice on one evaluated pose");
    float fl = a.model[ik.foot[0]][7], fr = a.model[ik.foot[1]][7], pel = a.model[ik.pelvis][7];
    CHECK(fabsf(fl - 0.1f) < 1e-3f && fabsf(fr - (0.1f - 0.15f)) < 1e-3f, "feet on the ground: %g %g", fl, fr);
    CHECK(fabsf(pel - 0.85f) < 1e-3f, "pelvis dropped 15 cm (%g)", pel);
    CHECK(a.model[ik.calf[0]][11] < a.model[ik.thigh[0]][11] - 0.01f, "the raised leg's knee points forward");
    /* skin matrices follow the IK: the right foot's vertex moves with it */
    float y = a.skin[ik.foot[1]][4] * 0 + a.skin[ik.foot[1]][5] * 0.1f + a.skin[ik.foot[1]][7];
    CHECK(fabsf(y - (0.1f - 0.15f)) < 1e-3f, "skinned right ankle at %g", y);

    /* smoothing: offsets approach the ground over time */
    vcpe_animator_init(&a, &rig, mem[0]);
    memset(&ik.offset, 0, sizeof ik.offset);
    ik.pelvis_offset = 0; ik.applied = 0; ik.smooth = 10;
    vcpe_animator_evaluate(&a, 1);
    vcpe_animator_feet(&a, &ik, world, ground, 0, 1 / 60.0f);
    CHECK(ik.pelvis_offset < 0 && ik.pelvis_offset > -0.15f, "smoothed pelvis on the first frame (%g)", ik.pelvis_offset);

    /* two animators share the scratch but not their poses */
    VcpeAnimator b;
    vcpe_animator_init(&a, &rig, mem[0]);
    vcpe_animator_init(&b, &rig, mem[1]);
    vcpe_animator_play(&a, 0, WALK, 0, 1, -1);
    vcpe_animator_play(&b, 0, RUN, 0, 1, -1);
    vcpe_animator_update(&a, 0.25f);
    vcpe_animator_update(&b, 0.25f);
    vcpe_animator_evaluate(&a, 1);
    vcpe_animator_evaluate(&b, 1);
    vcpe_rig_sample(&rig, WALK, 0.25f, 1, ref, 0);
    CHECK(max_pose_diff(a.local, ref) < 1e-5f && a.evaluations == 1, "independent animators");
    CHECK(vcpe_animator_play(&a, 0, 99, 0, 1, 0) == -1 && vcpe_animator_play(&a, 9, WALK, 0, 1, 0) == -1, "bad clip/layer refused");

    printf("PASS crossfades, interrupted fades, blend spaces with phase sync, masked one-shot layer, additive, queue, root motion, foot IK\n");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
