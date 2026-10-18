/* VCPE animator: layers, crossfades, blend spaces, events, root motion and foot IK over vcpe_rig (see
 * vcpe_animator.h). */

/* HOW THIS FILE WORKS
 *
 * A layer is a small stack of motions, oldest first. Playing something pushes it on top with weight 0; update()
 * raises the top weight by dt / fade, and once it reaches 1 everything below it is dropped. Evaluating samples the
 * oldest motion as a whole pose and blends each newer one over it by its eased weight, so a crossfade started in
 * the middle of another crossfade continues smoothly instead of popping. Layer 0 fades in from the bind pose (a
 * "bind" motion: clip -1 without a blend space) the first time and when stopped. When requests come faster than
 * fades finish and the stack is full, layer 0's current blend is sampled once into a frozen pose that becomes the new
 * bottom of the stack, so even a stream of interruptions never pops (higher layers drop their oldest motion).
 *
 * Higher layers are blended over the result with their own weight and bone mask (override: an upper-body attack
 * over the legs' walk) or added (additive: deltas from each clip's first frame, e.g. breathing, flinches).
 *
 * Blend spaces keep a phase in cycles instead of seconds; each clip of the pair the parameter falls between is
 * sampled at that phase x its own length, and the phase advances at the weighted mix of their cycle rates. That is
 * what keeps feet in step while a walk turns into a run.
 *
 * Root motion is gathered during update(), not evaluate(), so a character's position never depends on whether it
 * was drawn: every motion in layer 0 contributes in proportion to how much of it the evaluated pose would show.
 *
 * Contributions in a stack: the top motion shows e = ease(weight); each motion below shows what is left over by the
 * ones above it, and the oldest takes all that remains. */
#include <math.h>
#include <string.h>
#include "vcpe_animator.h"

static VcpeXform scratch_layer[VCPE_RIG_MAX_BONES], scratch_motion[VCPE_RIG_MAX_BONES], scratch_pair[VCPE_RIG_MAX_BONES];

static float ease(float w)
{
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    return w * w * (3 - 2 * w);
}

uint32_t vcpe_animator_bytes(const VcpeRig *rig)
{
    return rig && rig->nbones > 0 ? (uint32_t)rig->nbones * (48u + 48u + 2u * (uint32_t)sizeof(VcpeXform)) : 0;
}

int vcpe_animator_init(VcpeAnimator *a, const VcpeRig *rig, void *mem)
{
    if (!a || !rig || rig->nbones < 1 || !mem || ((uintptr_t)mem & 15))
        return -1;
    memset(a, 0, sizeof *a);
    a->rig = rig;
    uint8_t *p = mem;
    a->skin = (float (*)[12])p;
    a->model = (float (*)[12])(p + 48 * rig->nbones);
    a->local = (VcpeXform *)(p + 96 * rig->nbones);
    a->frozen = a->local + rig->nbones;
    for (int l = 0; l < VCPE_ANIM_LAYERS; l++) {
        a->layer[l].weight = a->layer[l].target = l == 0 ? 1.0f : 0.0f;
        a->layer[l].queue_clip = -1;
    }
    vcpe_rig_bind_pose(rig, a->local);
    vcpe_rig_model(rig, a->local, a->model);
    vcpe_rig_skin(rig, (const float (*)[12])a->model, a->skin);
    return 0;
}

/* ------------------------------------------------------------------ blend spaces */
static void space_pair(const VcpeBlendSpace *s, float p, int *i, int *j, float *k)
{
    int n = s->count < 1 ? 1 : s->count > VCPE_ANIM_SPACE ? VCPE_ANIM_SPACE : s->count;
    *k = 0;
    if (n == 1 || p <= s->at[0]) { *i = *j = 0; return; }
    if (p >= s->at[n - 1]) { *i = *j = n - 1; return; }
    for (int m = 0; m < n - 1; m++)
        if (p <= s->at[m + 1]) {
            float span = s->at[m + 1] - s->at[m];
            *i = m;
            *j = m + 1;
            *k = span > 0 ? (p - s->at[m]) / span : 0;
            return;
        }
    *i = *j = n - 1;
}
static float space_rate(const VcpeRig *rig, const VcpeBlendSpace *s, float p)   /* cycles per second */
{
    int i, j;
    float k, li, lj;
    space_pair(s, p, &i, &j, &k);
    li = vcpe_rig_length(rig, s->clip[i]);
    lj = vcpe_rig_length(rig, s->clip[j]);
    return (li > 0 ? (1 - k) / li : 0) + (lj > 0 ? k / lj : 0);
}

/* src over dst by w on the selected bones (sel != 0), or all bones */
static void blend_sel(const VcpeRig *rig, VcpeXform *dst, const VcpeXform *src, float w, const uint8_t *sel)
{
    if (w <= 0)
        return;
    for (int b = 0; b < rig->nbones; b++) {
        if (sel && !sel[b])
            continue;
        if (w >= 1) {
            dst[b] = src[b];
            continue;
        }
        vcpe_quat_nlerp(dst[b].r, dst[b].r, src[b].r, w);
        for (int k = 0; k < 3; k++)
            dst[b].t[k] += (src[b].t[k] - dst[b].t[k]) * w;
        dst[b].s += (src[b].s - dst[b].s) * w;
    }
}

static void sample_clip(const VcpeRig *rig, int clip, float t, int loop, VcpeXform *out, const uint8_t *sel, int delta)
{
    if (delta)
        vcpe_rig_sample_delta(rig, clip, t, loop, out, sel);
    else
        vcpe_rig_sample(rig, clip, t, loop, out, sel);
}

static void motion_sample(const VcpeAnimator *a, const VcpeMotion *m, VcpeXform *out, const uint8_t *sel, int delta)
{
    static const VcpeXform none = {{0, 0, 0, 1}, {0, 0, 0}, 1};
    const VcpeRig *rig = a->rig;
    if (m->clip == -2) {
        for (int b = 0; b < rig->nbones; b++)
            if (!sel || sel[b]) out[b] = delta ? none : a->frozen[b];
    } else if (m->space) {
        int i, j;
        float k, phase = m->time - floorf(m->time);
        space_pair(m->space, m->param, &i, &j, &k);
        sample_clip(rig, m->space->clip[i], phase * vcpe_rig_length(rig, m->space->clip[i]), 1, out, sel, delta);
        if (j != i && k > 0) {
            sample_clip(rig, m->space->clip[j], phase * vcpe_rig_length(rig, m->space->clip[j]), 1, scratch_pair, sel, delta);
            blend_sel(rig, out, scratch_pair, k, sel);
        }
    } else if (m->clip >= 0)
        sample_clip(rig, m->clip, m->time, m->loop, out, sel, delta);
    else if (delta) {
        for (int b = 0; b < rig->nbones; b++)
            if (!sel || sel[b]) out[b] = none;
    } else {
        /* the bind pose, for the bones selected */
        VcpeXform *o = out;
        for (int b = 0; b < rig->nbones; b++)
            if (!sel || sel[b]) {
                memcpy(o[b].r, rig->bones[b].r, sizeof o[b].r);
                memcpy(o[b].t, rig->bones[b].t, sizeof o[b].t);
                o[b].s = rig->bones[b].s;
            }
    }
}

static void layer_pose(const VcpeAnimator *a, const VcpeAnimLayer *ly, VcpeXform *out, const uint8_t *sel, int delta)
{
    motion_sample(a, &ly->stack[0], out, sel, delta);
    for (int i = 1; i < ly->count; i++) {
        motion_sample(a, &ly->stack[i], scratch_motion, sel, delta);
        blend_sel(a->rig, out, scratch_motion, ease(ly->stack[i].weight), sel);
    }
}

/* ------------------------------------------------------------------ playing */
static VcpeMotion *push(VcpeAnimator *a, int layer, float fade)
{
    VcpeAnimLayer *ly = &a->layer[layer];
    if (layer == 0 && ly->count == 0 && fade > 0) {
        /* fade in from the bind pose */
        memset(&ly->stack[0], 0, sizeof ly->stack[0]);
        ly->stack[0].clip = -1;
        ly->stack[0].weight = 1;
        ly->count = 1;
    }
    if (ly->count == VCPE_ANIM_STACK) {
        if (layer == 0) {
            layer_pose(a, ly, a->frozen, 0, 0);
            memset(&ly->stack[0], 0, sizeof ly->stack[0]);
            ly->stack[0].clip = -2;
            ly->stack[0].weight = 1;
            ly->count = 1;
        } else {
            memmove(&ly->stack[0], &ly->stack[1], sizeof ly->stack[0] * (VCPE_ANIM_STACK - 1));
            ly->count--;
        }
    }
    VcpeMotion *m = &ly->stack[ly->count++];
    memset(m, 0, sizeof *m);
    m->fade = fade > 0 ? fade : 0;
    m->weight = (fade > 0 && ly->count > 1) ? 0 : 1;
    m->speed = 1;
    m->fresh = 1;
    if (layer > 0) {
        ly->target = 1;
        ly->rate = fade > 0 ? 1.0f / fade : 0;
        if (fade <= 0) ly->weight = 1;
    }
    ly->has_queue = 0;
    return m;
}

int vcpe_animator_play(VcpeAnimator *a, int layer, int clip, float fade, float speed, int loop)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || clip < 0 || clip >= a->rig->nclips)
        return -1;
    VcpeMotion *m = push(a, layer, fade);
    m->clip = (int16_t)clip;
    m->loop = loop < 0 ? (vcpe_rig_flags(a->rig, clip) & VCPE_RIG_LOOP) != 0 : loop != 0;
    m->speed = speed > 0 ? speed : 0;
    a->layer[layer].auto_out = layer > 0 && !m->loop;
    return 0;
}

int vcpe_animator_play_space(VcpeAnimator *a, int layer, const VcpeBlendSpace *s, float param, float fade, float speed)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || !s || s->count < 1 || s->count > VCPE_ANIM_SPACE)
        return -1;
    for (int i = 0; i < s->count; i++)
        if (s->clip[i] < 0 || s->clip[i] >= a->rig->nclips || (i && s->at[i] < s->at[i - 1]))
            return -1;
    VcpeMotion *m = push(a, layer, fade);
    m->clip = -1;
    m->space = s;
    m->loop = 1;
    m->param = param;
    m->speed = speed > 0 ? speed : 0;
    a->layer[layer].auto_out = 0;
    return 0;
}

void vcpe_animator_queue(VcpeAnimator *a, int layer, int clip, float fade, float speed, int loop)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || clip < 0 || clip >= a->rig->nclips)
        return;
    VcpeAnimLayer *ly = &a->layer[layer];
    ly->has_queue = 1;
    ly->queue_clip = (int16_t)clip;
    ly->queue_fade = fade > 0 ? fade : 0;
    ly->queue_speed = speed > 0 ? speed : 0;
    ly->queue_loop = (uint8_t)(loop < 0 ? (vcpe_rig_flags(a->rig, clip) & VCPE_RIG_LOOP) != 0 : loop != 0);
}

static VcpeMotion *current(VcpeAnimator *a, int layer)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || !a->layer[layer].count)
        return 0;
    return &a->layer[layer].stack[a->layer[layer].count - 1];
}
void vcpe_animator_param(VcpeAnimator *a, int layer, float param)
{
    VcpeMotion *m = current(a, layer);
    if (m) m->param = param;
}
void vcpe_animator_speed(VcpeAnimator *a, int layer, float speed)
{
    VcpeMotion *m = current(a, layer);
    if (m) m->speed = speed > 0 ? speed : 0;
}
void vcpe_animator_layer(VcpeAnimator *a, int layer, const uint8_t *mask, int additive)
{
    if (!a || layer < 1 || layer >= VCPE_ANIM_LAYERS)
        return;
    a->layer[layer].mask = mask;
    a->layer[layer].additive = additive != 0;
}
void vcpe_animator_layer_weight(VcpeAnimator *a, int layer, float weight, float fade)
{
    if (!a || layer < 1 || layer >= VCPE_ANIM_LAYERS)
        return;
    VcpeAnimLayer *ly = &a->layer[layer];
    ly->target = weight < 0 ? 0 : weight > 1 ? 1 : weight;
    ly->rate = fade > 0 ? 1.0f / fade : 0;
    if (fade <= 0) ly->weight = ly->target;
    ly->auto_out = 0;
}
void vcpe_animator_stop(VcpeAnimator *a, int layer, float fade)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS)
        return;
    if (layer == 0) {
        if (a->layer[0].count) {
            VcpeMotion *m = push(a, 0, fade);
            m->clip = -1;
        }
        return;
    }
    vcpe_animator_layer_weight(a, layer, 0, fade);
    a->layer[layer].has_queue = 0;
}
void vcpe_animator_events(VcpeAnimator *a, VcpeAnimEventFn fn, void *user)
{
    a->on_event = fn;
    a->user = user;
}

/* ------------------------------------------------------------------ update */
typedef struct { VcpeAnimator *a; int layer; } EventCtx;
static void relay(void *user, int clip, const VcpeRigEvent *e)
{
    EventCtx *c = user;
    c->a->on_event(c->a->user, c->layer, clip, e->id, e->arg);
}

static void motion_events(VcpeAnimator *a, int layer, const VcpeMotion *m, float t0, float t1)
{
    if (!a->on_event)
        return;
    EventCtx ctx = {a, layer};
    float lead = m->fresh ? 1e-6f : 0;
    if (m->space) {
        int i, j;
        float k;
        space_pair(m->space, m->param, &i, &j, &k);
        int c = m->space->clip[k < 0.5f ? i : j];
        float len = vcpe_rig_length(a->rig, c);
        vcpe_rig_events(a->rig, c, t0 * len - lead, t1 * len, 1, relay, &ctx);
    } else if (m->clip >= 0)
        vcpe_rig_events(a->rig, m->clip, t0 - lead, t1, m->loop, relay, &ctx);
}

static void add_motion(const VcpeRig *rig, int clip, float from, float to, float w, float acc[4])
{
    float d[3], yaw;
    vcpe_rig_motion(rig, clip, from, to, 1, d, &yaw);
    acc[0] += d[0] * w;
    acc[2] += d[2] * w;
    acc[3] += yaw * w;
}
static void motion_travel(const VcpeRig *rig, const VcpeMotion *m, float t0, float t1, float w, float acc[4])
{
    if (w <= 0 || !(t1 > t0))
        return;
    if (m->space) {
        int i, j;
        float k;
        space_pair(m->space, m->param, &i, &j, &k);
        float li = vcpe_rig_length(rig, m->space->clip[i]), lj = vcpe_rig_length(rig, m->space->clip[j]);
        add_motion(rig, m->space->clip[i], t0 * li, t1 * li, w * (j == i ? 1 : 1 - k), acc);
        if (j != i && k > 0)
            add_motion(rig, m->space->clip[j], t0 * lj, t1 * lj, w * k, acc);
    } else if (m->clip >= 0) {
        float d[3], yaw;
        vcpe_rig_motion(rig, m->clip, t0, t1, m->loop, d, &yaw);
        acc[0] += d[0] * w;
        acc[2] += d[2] * w;
        acc[3] += yaw * w;
    }
}

void vcpe_animator_update(VcpeAnimator *a, float dt)
{
    if (!a || !(dt > 0))
        return;
    const VcpeRig *rig = a->rig;
    float travel[4] = {0, 0, 0, 0};
    for (int l = 0; l < VCPE_ANIM_LAYERS; l++) {
        VcpeAnimLayer *ly = &a->layer[l];
        if (ly->weight != ly->target) {
            float step = ly->rate > 0 ? ly->rate * dt : 1;
            ly->weight = ly->weight < ly->target ? fminf(ly->target, ly->weight + step) : fmaxf(ly->target, ly->weight - step);
        }
        if (l > 0 && ly->weight <= 0 && ly->target <= 0) {
            ly->count = 0;            /* faded out: nothing left to sample */
            continue;
        }
        if (!ly->count)
            continue;
        VcpeMotion *cur = &ly->stack[ly->count - 1];
        if (!cur->space && cur->clip >= 0 && !cur->loop) {
            float len = vcpe_rig_length(rig, cur->clip), next = cur->time + dt * cur->speed;
            if (ly->has_queue && next >= len - ly->queue_fade) {
                int clip = ly->queue_clip, loop = ly->queue_loop;
                float fade = ly->queue_fade, speed = ly->queue_speed;
                VcpeMotion *m = push(a, l, fade);   /* clears the queue */
                m->clip = (int16_t)clip;
                m->loop = (uint8_t)loop;
                m->speed = speed;
                ly->auto_out = l > 0 && !loop;
            } else if (l > 0 && ly->auto_out && ly->target > 0) {
                float out = cur->fade > 0 ? cur->fade : 0.15f;
                if (next >= len - out) {
                    ly->target = 0;
                    ly->rate = 1.0f / out;
                }
            }
        }
        float left = 1;
        for (int i = ly->count - 1; i >= 0; i--) {
            VcpeMotion *m = &ly->stack[i];
            float e = i == 0 ? 1 : ease(m->weight), share = e * left;
            left *= 1 - e;
            float t0 = m->time;
            if (m->space)
                m->time += dt * m->speed * space_rate(rig, m->space, m->param);
            else if (m->clip >= 0) {
                m->time += dt * m->speed;
                if (!m->loop) {
                    float len = vcpe_rig_length(rig, m->clip);
                    if (m->time > len) m->time = len;
                }
            }
            if (l == 0)
                motion_travel(rig, m, t0, m->time, share, travel);
            if (i == ly->count - 1) {
                motion_events(a, l, m, t0, m->time);
                if (m->weight < 1)
                    m->weight = m->fade > 0 ? fminf(1, m->weight + dt / m->fade) : 1;
            }
            m->fresh = 0;
        }
        if (ly->count > 1 && ly->stack[ly->count - 1].weight >= 1) {
            ly->stack[0] = ly->stack[ly->count - 1];
            ly->count = 1;
        }
    }
    /* compose this tick's travel after what was gathered before (it starts at the heading reached so far) */
    float c = cosf(a->motion_yaw), s = sinf(a->motion_yaw);
    a->motion[0] += travel[0] * c + travel[2] * s;
    a->motion[2] += -travel[0] * s + travel[2] * c;
    a->motion_yaw += travel[3];
}

void vcpe_animator_take_motion(VcpeAnimator *a, float dpos[3], float *dyaw)
{
    memcpy(dpos, a->motion, sizeof a->motion);
    if (dyaw) *dyaw = a->motion_yaw;
    a->motion[0] = a->motion[1] = a->motion[2] = a->motion_yaw = 0;
}

/* ------------------------------------------------------------------ evaluate */

void vcpe_animator_evaluate(VcpeAnimator *a, int layers)
{
    if (!a)
        return;
    const VcpeRig *rig = a->rig;
    if (layers < 1) layers = 1;
    if (layers > VCPE_ANIM_LAYERS) layers = VCPE_ANIM_LAYERS;
    if (a->layer[0].count)
        layer_pose(a, &a->layer[0], a->local, 0, 0);
    else
        vcpe_rig_bind_pose(rig, a->local);
    for (int l = 1; l < layers; l++) {
        const VcpeAnimLayer *ly = &a->layer[l];
        float w = ease(ly->weight);
        if (!ly->count || w <= 0)
            continue;
        layer_pose(a, ly, scratch_layer, ly->mask, ly->additive);
        if (ly->additive)
            vcpe_rig_add(rig, a->local, scratch_layer, w, ly->mask);
        else
            vcpe_rig_blend(rig, a->local, scratch_layer, w, ly->mask);
    }
    vcpe_rig_model(rig, a->local, a->model);
    vcpe_rig_skin(rig, (const float (*)[12])a->model, a->skin);
    a->evaluations++;
}

int vcpe_animator_current(const VcpeAnimator *a, int layer)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || !a->layer[layer].count)
        return -1;
    const VcpeMotion *m = &a->layer[layer].stack[a->layer[layer].count - 1];
    return m->space ? -1 : m->clip;
}
float vcpe_animator_time(const VcpeAnimator *a, int layer)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || !a->layer[layer].count)
        return 0;
    const VcpeMotion *m = &a->layer[layer].stack[a->layer[layer].count - 1];
    if (m->space)
        return m->time - floorf(m->time);
    if (m->clip >= 0 && m->loop) {
        float len = vcpe_rig_length(a->rig, m->clip);
        return len > 0 ? m->time - len * floorf(m->time / len) : 0;
    }
    return m->time;
}
int vcpe_animator_finished(const VcpeAnimator *a, int layer)
{
    if (!a || layer < 0 || layer >= VCPE_ANIM_LAYERS || !a->layer[layer].count)
        return 1;
    const VcpeMotion *m = &a->layer[layer].stack[a->layer[layer].count - 1];
    return !m->space && m->clip >= 0 && !m->loop && m->time >= vcpe_rig_length(a->rig, m->clip);
}
float vcpe_animator_layer_weight_now(const VcpeAnimator *a, int layer)
{
    return a && layer >= 0 && layer < VCPE_ANIM_LAYERS ? a->layer[layer].weight : 0;
}

/* ------------------------------------------------------------------ foot IK */
int vcpe_animator_feet(VcpeAnimator *a, VcpeFootIK *ik, const float w[16], VcpeGroundFn ground, void *user, float dt)
{
    if (!a || !ik || !w || !ground)
        return 0;
    const VcpeRig *rig = a->rig;
    for (int f = 0; f < 2; f++) {
        int t = ik->thigh[f], c = ik->calf[f], o = ik->foot[f];
        if (t < 0 || c < 0 || o < 0 || t >= rig->nbones || c >= rig->nbones || o >= rig->nbones ||
            rig->bones[c].parent != t || rig->bones[o].parent != c)
            return 0;
    }
    /* once per evaluated pose: the pelvis offset is added to the evaluated pose, so a second call would add it twice */
    if (ik->applied == a->evaluations + 1)
        return 0;
    ik->applied = a->evaluations + 1;
    float ws2 = w[0] * w[0] + w[4] * w[4] + w[8] * w[8];
    if (!(ws2 > 1e-12f))
        return 0;
    /* world up in character space: R^T (0, 1, 0) / scale = second row of the matrix / scale^2 */
    float up[3] = {w[4] / ws2, w[5] / ws2, w[6] / ws2};
    float k = ik->smooth > 0 ? fminf(1, ik->smooth * (dt > 0 ? dt : 0)) : 1;
    float n[2][3];
    int have[2];
    for (int f = 0; f < 2; f++) {
        const float *m = a->model[ik->foot[f]];
        float px = w[0] * m[3] + w[1] * m[7] + w[2] * m[11] + w[3];
        float pz = w[8] * m[3] + w[9] * m[7] + w[10] * m[11] + w[11];
        float h = 0, want = 0;
        n[f][0] = 0; n[f][1] = 1; n[f][2] = 0;
        have[f] = ground(user, px, pz, &h, n[f]) != 0;
        if (have[f]) {
            want = h - w[7];
            want = want < -ik->max_drop ? -ik->max_drop : want > ik->max_raise ? ik->max_raise : want;
        }
        ik->offset[f] += (want - ik->offset[f]) * k;
    }
    float pel = fminf(0, fminf(ik->offset[0], ik->offset[1]));
    if (pel < -ik->max_drop) pel = -ik->max_drop;
    ik->pelvis_offset += (pel - ik->pelvis_offset) * k;
    if (ik->pelvis >= 0 && ik->pelvis < rig->nbones && ik->pelvis_offset != 0) {
        VcpeXform parent;
        float v[3] = {up[0] * ik->pelvis_offset, up[1] * ik->pelvis_offset, up[2] * ik->pelvis_offset}, inv[4];
        vcpe_rig_model_xform(rig, a->local, rig->bones[ik->pelvis].parent, &parent);
        inv[0] = -parent.r[0]; inv[1] = -parent.r[1]; inv[2] = -parent.r[2]; inv[3] = parent.r[3];
        vcpe_quat_rotate(inv, v, v);
        for (int i = 0; i < 3; i++)
            a->local[ik->pelvis].t[i] += v[i] / parent.s;
        vcpe_rig_model(rig, a->local, a->model);
    }
    int done = 0;
    float upn[3] = {up[0], up[1], up[2]}, ul = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    for (int i = 0; i < 3; i++) upn[i] /= ul;
    for (int f = 0; f < 2; f++) {
        if (!have[f])
            continue;
        const float *hip = a->model[ik->thigh[f]], *knee = a->model[ik->calf[f]], *ank = a->model[ik->foot[f]];
        float rise = ik->offset[f] - ik->pelvis_offset;
        float target[3] = {ank[3] + up[0] * rise, ank[7] + up[1] * rise, ank[11] + up[2] * rise};
        float bend[3] = {knee[3] - (hip[3] + ank[3]) * 0.5f, knee[7] - (hip[7] + ank[7]) * 0.5f, knee[11] - (hip[11] + ank[11]) * 0.5f};
        if (bend[0] * bend[0] + bend[1] * bend[1] + bend[2] * bend[2] < 1e-8f) { bend[0] = 0; bend[1] = 0; bend[2] = -1; }
        float pole[3] = {knee[3] + bend[0], knee[7] + bend[1], knee[11] + bend[2]};
        vcpe_rig_ik2(rig, a->local, ik->thigh[f], ik->calf[f], ik->foot[f], target, pole, 1, 1);
        if (ik->align > 0) {
            /* tilt the foot toward the ground normal, in character space, capped */
            float gn[3] = {w[0] * n[f][0] + w[4] * n[f][1] + w[8] * n[f][2], w[1] * n[f][0] + w[5] * n[f][1] + w[9] * n[f][2],
                           w[2] * n[f][0] + w[6] * n[f][1] + w[10] * n[f][2]};
            float gl = sqrtf(gn[0] * gn[0] + gn[1] * gn[1] + gn[2] * gn[2]);
            if (gl > 1e-6f) {
                float q[4], one[4] = {0, 0, 0, 1}, t = ik->align > 1 ? 1 : ik->align;
                for (int i = 0; i < 3; i++) gn[i] /= gl;
                vcpe_quat_between(q, upn, gn);
                float half = 0.30543f;   /* 35 degrees / 2 */
                float s = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (s > sinf(half)) {
                    for (int i = 0; i < 3; i++) q[i] *= sinf(half) / s;
                    q[3] = cosf(half);
                }
                vcpe_quat_nlerp(q, one, q, t);
                VcpeXform calf, foot;
                vcpe_rig_model_xform(rig, a->local, ik->calf[f], &calf);
                vcpe_quat_mul(foot.r, calf.r, a->local[ik->foot[f]].r);
                vcpe_quat_mul(foot.r, q, foot.r);
                float inv[4] = {-calf.r[0], -calf.r[1], -calf.r[2], calf.r[3]};
                vcpe_quat_mul(a->local[ik->foot[f]].r, inv, foot.r);
            }
        }
        done++;
    }
    vcpe_rig_model(rig, a->local, a->model);
    vcpe_rig_skin(rig, (const float (*)[12])a->model, a->skin);
    return done;
}
