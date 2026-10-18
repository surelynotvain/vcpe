/* vcpe_crowd host test: tiers by size and distance, caps, important agents, visibility, waking, staggering, dt
 * bookkeeping, hysteresis and the vcpe_adapt factor. */
#include "vcpe_crowd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float factor = 1;
float vcpe_adapt_lod_factor(void) { return factor; }    /* stands in for vcpe_adapt */
static int right_half(void *u, const float c[3], float r) { (void)u; return c[0] + r > 0; }

int main(void)
{
    static VcpeCrowdAgent ag[VCPE_CROWD_MAX];
    VcpeCrowd crowd;
    VcpeCrowdConfig cfg;
    vcpe_crowd_defaults(&cfg);
    vcpe_crowd_init(&crowd, &cfg);
    float pxk = 136.0f / tanf(30 * 3.14159265f / 180);
    VcpeCrowdView view = {{0, 0, 0}, pxk, 0, 0};

    /* 100 agents along +x from 2 to 200 m */
    for (int i = 0; i < 100; i++) { ag[i].pos[0] = 2 + i * 2.0f; ag[i].radius = 0.9f; }
    vcpe_crowd_update(&crowd, ag, 100, &view, 1 / 60.0f);
    int mono = 1, dormant_ok = 1;
    for (int i = 1; i < 100; i++) {
        if (ag[i].pos[0] <= 150 && ag[i].tier < ag[i - 1].tier) mono = 0;
        if ((ag[i].pos[0] > 150) != (ag[i].tier == VCPE_TIER_DORMANT)) dormant_ok = 0;
    }
    CHECK(mono && dormant_ok, "tiers coarsen with distance; dormant exactly beyond 150 m");
    CHECK(ag[0].tier == VCPE_TIER_FULL && ag[3].tier == VCPE_TIER_REDUCED && ag[10].tier == VCPE_TIER_FAR, "2 m full, 8 m reduced, 22 m far (%d %d %d)", ag[0].tier, ag[3].tier, ag[10].tier);
    CHECK(ag[0].evaluate && ag[0].layers == 4 && ag[0].mesh_lod == 0 && ag[0].tick_ai, "full: everything every frame");
    char line[160];
    vcpe_crowd_line(&crowd, line, sizeof line);
    CHECK(strstr(line, "dormant") != 0, "%s", line);

    /* caps: 30 agents within 3 m: the 6 closest full, the next 16 reduced, the rest far */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    for (int i = 0; i < 30; i++) { ag[i].pos[0] = 1.5f + (29 - i) * 0.05f; ag[i].pos[2] = (float)(i % 3) * 0.1f; ag[i].radius = 0.9f; }
    vcpe_crowd_update(&crowd, ag, 30, &view, 1 / 60.0f);
    CHECK(crowd.count[VCPE_TIER_FULL] == 6 && crowd.count[VCPE_TIER_REDUCED] == 16 && crowd.count[VCPE_TIER_FAR] == 8, "caps 6/16/8 (%d %d %d)", crowd.count[0], crowd.count[1], crowd.count[2]);
    int nearest_full = 1;
    for (int i = 24; i < 30; i++) nearest_full &= ag[i].tier == VCPE_TIER_FULL;
    CHECK(nearest_full, "the closest six are full");

    /* important agents: reduced at least, never dormant */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    ag[0].pos[0] = 40; ag[0].radius = 0.9f; ag[0].important = 1;
    ag[1].pos[0] = 400; ag[1].radius = 0.9f; ag[1].important = 1;
    ag[2].pos[0] = 40; ag[2].radius = 0.9f;
    vcpe_crowd_update(&crowd, ag, 3, &view, 1 / 60.0f);
    CHECK(ag[0].tier == VCPE_TIER_REDUCED && ag[1].tier != VCPE_TIER_DORMANT && ag[2].tier == VCPE_TIER_FAR, "follower stays reduced (%d), never sleeps (%d)", ag[0].tier, ag[1].tier);

    /* visibility and waking up */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    view.visible = right_half;
    for (int i = 0; i < 8; i++) { ag[i].pos[0] = i < 4 ? -10.0f : 10.0f; ag[i].pos[2] = (float)i; ag[i].radius = 0.9f; }
    for (int f = 0; f < 3; f++) vcpe_crowd_update(&crowd, ag, 8, &view, 1 / 60.0f);
    CHECK(crowd.count[VCPE_TIER_HIDDEN] == 4 && !ag[0].evaluate && !ag[0].visible && ag[0].tick_anim, "behind the camera: hidden, no pose, clocks run");
    int woke = 1;
    for (int i = 0; i < 4; i++) ag[i].pos[0] = 30;
    vcpe_crowd_update(&crowd, ag, 8, &view, 1 / 60.0f);
    for (int i = 0; i < 4; i++) woke &= ag[i].visible && ag[i].evaluate && ag[i].tier == VCPE_TIER_FAR;
    CHECK(woke, "turning toward them gives a fresh pose at once");
    view.visible = 0;

    /* staggering: 64 far agents pose every 4th frame, a quarter on each frame */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    for (int i = 0; i < 64; i++) { ag[i].pos[0] = 30 + (float)(i % 8); ag[i].pos[2] = (float)(i / 8); ag[i].radius = 0.9f; }
    vcpe_crowd_update(&crowd, ag, 64, &view, 1 / 60.0f);
    int per[4] = {0}, each[64] = {0}, lo = 99, hi = 0;
    float ai_sum[64] = {0}, anim_sum[64] = {0};
    for (int f = 0; f < 400; f++) {
        int e = vcpe_crowd_update(&crowd, ag, 64, &view, 1 / 60.0f);
        per[f & 3] += e;
        if (e < lo) lo = e;
        if (e > hi) hi = e;
        for (int i = 0; i < 64; i++) { each[i] += ag[i].evaluate; ai_sum[i] += ag[i].ai_dt; anim_sum[i] += ag[i].anim_dt; }
    }
    int exact = 1;
    for (int i = 0; i < 64; i++) exact &= each[i] == 100;
    CHECK(exact && lo >= 6 && hi <= 26, "each poses every 4th frame; %d..%d poses per frame", lo, hi);
    int dt_ok = 1;
    for (int i = 0; i < 64; i++) dt_ok &= fabsf(anim_sum[i] - 400 / 60.0f) < 1e-3f && ai_sum[i] > 400 / 60.0f - 4 / 60.0f - 1e-3f;
    CHECK(dt_ok, "clocks and AI receive all the elapsed time");

    /* dormant agents tick every 30 frames with the time that piled up */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    ag[0].pos[0] = 300; ag[0].radius = 0.9f;
    int ticks = 0;
    float last = 0;
    for (int f = 0; f < 300; f++) {
        vcpe_crowd_update(&crowd, ag, 1, &view, 1 / 60.0f);
        if (ag[0].tick_anim) { ticks++; last = ag[0].anim_dt; }
    }
    CHECK(ticks == 10 && fabsf(last - 0.5f) < 1e-4f, "dormant: %d ticks of %g s", ticks, last);

    /* hysteresis: pacing around the reduced/far boundary flips the tier rarely */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    ag[0].radius = 0.9f;
    float boundary = 2 * 0.9f * pxk / 50 + 0.9f;
    int flips = 0, prev = -1;
    for (int f = 0; f < 600; f++) {
        ag[0].pos[0] = boundary * (1 + 0.05f * sinf(f * 0.2f));
        vcpe_crowd_update(&crowd, ag, 1, &view, 1 / 60.0f);
        if (prev >= 0 && ag[0].tier != prev) flips++;
        prev = ag[0].tier;
    }
    CHECK(flips <= 1, "%d tier changes pacing 5 %% around a threshold", flips);

    /* vcpe_adapt under load: a factor of 2 halves apparent sizes */
    vcpe_crowd_init(&crowd, &cfg);
    memset(ag, 0, sizeof ag);
    for (int i = 0; i < 20; i++) { ag[i].pos[0] = 2 + i * 0.6f; ag[i].radius = 0.9f; }
    vcpe_crowd_update(&crowd, ag, 20, &view, 1 / 60.0f);
    int full_fast = crowd.count[VCPE_TIER_FULL], reduced_fast = crowd.count[VCPE_TIER_REDUCED];
    factor = 2;
    for (int f = 0; f < 3; f++) vcpe_crowd_update(&crowd, ag, 20, &view, 1 / 60.0f);
    CHECK(crowd.count[VCPE_TIER_FULL] < full_fast && crowd.count[VCPE_TIER_FULL] + crowd.count[VCPE_TIER_REDUCED] < full_fast + reduced_fast, "load coarsens tiers (%d -> %d full)", full_fast, crowd.count[VCPE_TIER_FULL]);
    factor = 1;

    printf("PASS crowd: tiers, caps, important agents, visibility, waking, staggering, elapsed time, dormancy, hysteresis, adapt\n");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
