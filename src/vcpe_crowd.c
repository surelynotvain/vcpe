/* VCPE crowd: tiers, caps and staggered schedules for many characters (see vcpe_crowd.h). */

/* HOW THIS FILE WORKS
 *
 * Apparent size: an agent's bounding sphere seen from the camera covers about 2 r x px_k / d pixels, with d the
 * distance to the sphere's near side (the vcpe_lod measure). Hysteresis: an agent moves to a finer tier only when it
 * is hysteresis x threshold bigger than the threshold, and back only when that much smaller, so one walking at a
 * boundary does not flip every frame.
 *
 * Caps: candidates for FULL are ranked by size (important agents count four times as big) and only the first
 * max_full keep it; the rest drop to REDUCED, which is capped the same way. The ranking is redone every frame, so
 * the closest characters always get the detail.
 *
 * Staggering: an agent's phase comes from its index; one that poses every 4 frames poses when (frame + phase) % 4 is
 * 0, so a quarter of them pose on each frame instead of all of them every fourth frame. Clocks and AI that do not
 * tick keep the elapsed time and get it all on their next tick, so nothing slows down, it only gets coarser. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_crowd.h"

float vcpe_adapt_lod_factor(void) __attribute__((weak));

void vcpe_crowd_defaults(VcpeCrowdConfig *c)
{
    memset(c, 0, sizeof *c);
    c->full_px = 120;
    c->reduced_px = 50;
    c->max_full = 6;
    c->max_reduced = 16;
    c->reduced_every = 2;
    c->far_every = 4;
    c->ai_far_every = 4;
    c->ai_dormant_every = 30;
    c->dormant_dist = 150;
    c->hysteresis = 0.15f;
}

void vcpe_crowd_init(VcpeCrowd *c, const VcpeCrowdConfig *cfg)
{
    memset(c, 0, sizeof *c);
    if (cfg) c->cfg = *cfg;
    else vcpe_crowd_defaults(&c->cfg);
    if (c->cfg.reduced_every < 1) c->cfg.reduced_every = 1;
    if (c->cfg.far_every < 1) c->cfg.far_every = 1;
    if (c->cfg.ai_far_every < 1) c->cfg.ai_far_every = 1;
    if (c->cfg.ai_dormant_every < 1) c->cfg.ai_dormant_every = 1;
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

/* finer tier when clearly above a threshold, coarser when clearly below */
static int size_tier(const VcpeCrowdConfig *c, float px, int prev)
{
    float up = 1 + c->hysteresis, down = 1 - c->hysteresis;
    int t = VCPE_TIER_FAR;
    if (px >= c->reduced_px * (prev <= VCPE_TIER_REDUCED ? down : up)) t = VCPE_TIER_REDUCED;
    if (px >= c->full_px * (prev == VCPE_TIER_FULL ? down : up)) t = VCPE_TIER_FULL;
    return t;
}

static const float *sort_key;
static int by_key(const void *a, const void *b)
{
    float x = sort_key[*(const int *)a], y = sort_key[*(const int *)b];
    return x < y ? 1 : x > y ? -1 : *(const int *)a - *(const int *)b;   /* biggest first; index breaks ties */
}

static void cap(VcpeCrowdAgent *ag, int n, int tier, int max, const float *key)
{
    static int idx[VCPE_CROWD_MAX];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (ag[i].tier == tier) idx[m++] = i;
    if (m <= max)
        return;
    sort_key = key;
    qsort(idx, (size_t)m, sizeof idx[0], by_key);
    for (int k = max < 0 ? 0 : max; k < m; k++)
        ag[idx[k]].tier = (uint8_t)(tier + 1);
}

int vcpe_crowd_update(VcpeCrowd *c, VcpeCrowdAgent *ag, int n, const VcpeCrowdView *v, float dt)
{
    static float key[VCPE_CROWD_MAX];
    static uint8_t before[VCPE_CROWD_MAX];
    const VcpeCrowdConfig *cf = &c->cfg;
    if (n > VCPE_CROWD_MAX) n = VCPE_CROWD_MAX;
    if (n < 0) n = 0;
    if (!(dt >= 0)) dt = 0;
    float factor = vcpe_adapt_lod_factor ? vcpe_adapt_lod_factor() : 1;
    if (!(factor >= 1)) factor = 1;
    c->frame++;
    for (int i = 0; i < n; i++) {
        VcpeCrowdAgent *a = &ag[i];
        if (!a->seen) {
            a->phase = mix((uint32_t)i + 1);
            a->tier = VCPE_TIER_HIDDEN;
        }
        before[i] = a->seen ? a->tier : VCPE_TIER_COUNT;
        float d[3] = {a->pos[0] - v->pos[0], a->pos[1] - v->pos[1], a->pos[2] - v->pos[2]};
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), near = dist - a->radius;
        if (near < 0.01f) near = 0.01f;
        a->px = 2 * a->radius * v->px_k / near / factor;
        a->visible = !v->visible || v->visible(v->user, a->pos, a->radius);
        if (cf->dormant_dist > 0 && dist > cf->dormant_dist && !a->important) {
            a->tier = VCPE_TIER_DORMANT;
            a->visible = 0;
        } else if (!a->visible)
            a->tier = VCPE_TIER_HIDDEN;
        else
            a->tier = (uint8_t)size_tier(cf, a->px, a->seen ? before[i] : VCPE_TIER_FAR);
        key[i] = a->px * (a->important ? 4 : 1);
        a->seen = 1;
    }
    cap(ag, n, VCPE_TIER_FULL, cf->max_full, key);
    cap(ag, n, VCPE_TIER_REDUCED, cf->max_reduced, key);
    memset(c->count, 0, sizeof c->count);
    c->evaluations = c->ai_ticks = 0;
    for (int i = 0; i < n; i++) {
        VcpeCrowdAgent *a = &ag[i];
        if (a->important && a->visible && a->tier == VCPE_TIER_FAR)
            a->tier = VCPE_TIER_REDUCED;
        int t = a->tier, woke = before[i] >= VCPE_TIER_HIDDEN && t <= VCPE_TIER_FAR;
        uint32_t f = c->frame + a->phase;
        switch (t) {
        case VCPE_TIER_FULL: a->evaluate = 1; a->layers = 4; a->mesh_lod = 0; break;
        case VCPE_TIER_REDUCED: a->evaluate = woke || f % (uint32_t)cf->reduced_every == 0; a->layers = 2; a->mesh_lod = 1; break;
        case VCPE_TIER_FAR: a->evaluate = woke || f % (uint32_t)cf->far_every == 0; a->layers = 1; a->mesh_lod = 2; break;
        default: a->evaluate = 0; a->layers = 0; a->mesh_lod = 2; break;
        }
        a->tick_ai = t <= VCPE_TIER_REDUCED ? 1 : t == VCPE_TIER_DORMANT ? f % (uint32_t)cf->ai_dormant_every == 0
                                                                       : f % (uint32_t)cf->ai_far_every == 0;
        a->tick_anim = t != VCPE_TIER_DORMANT || a->tick_ai;
        a->anim_acc += dt;
        a->ai_acc += dt;
        a->anim_dt = a->ai_dt = 0;
        if (a->tick_anim) { a->anim_dt = a->anim_acc; a->anim_acc = 0; }
        if (a->tick_ai) { a->ai_dt = a->ai_acc; a->ai_acc = 0; }
        c->count[t]++;
        c->evaluations += a->evaluate;
        c->ai_ticks += a->tick_ai;
    }
    return c->evaluations;
}

int vcpe_crowd_line(const VcpeCrowd *c, char *out, int n)
{
    return snprintf(out, (size_t)n, "crowd: %d full, %d reduced, %d far, %d hidden, %d dormant; %d poses, %d AI",
                    c->count[0], c->count[1], c->count[2], c->count[3], c->count[4], c->evaluations, c->ai_ticks);
}
