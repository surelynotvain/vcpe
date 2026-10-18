/* VCPE rigs: reading VRG1 blobs, sampling, blending, root motion and IK (see vcpe_rig.h). */

/* HOW THIS FILE WORKS
 *
 * A clip is a table of tracks, one per bone. A track says for rotation, translation and scale whether the bone keeps
 * its bind value (nothing stored), holds one value for the whole clip, or has a key on every frame. Most bones of a
 * real character only rotate, so the common case is one 6-byte key per bone per frame:
 *
 *   rotation     "smallest three": the largest quaternion component is dropped (it is rebuilt from the other three,
 *                the quaternion being unit length) and the other three, each within +-1/sqrt(2), get 15 bits (32766
 *                steps, an even count, so 0 is exact and rest poses stay rest poses). The two bits that say which
 *                component was dropped are the top bits of the first two words. Error < 5e-5.
 *   translation  three 16-bit fractions of the track's own box (min + q x step), so hips and IK targets keep
 *                sub-millimetre precision without storing floats.
 *   scale        floats (rare in practice).
 *
 * Sampling picks the two frames around t and interpolates: normalised lerp for rotations (on the short arc), linear
 * for the rest. Frames are evenly spaced, so finding them is one multiply; no key search.
 *
 * Root motion: a walk authored with the hips really moving forward would drift away from the character's capsule.
 * Clips flagged VCPE_RIG_MOTION_* keep the motion bone at its first-frame ground position and heading in the pose,
 * and vcpe_rig_motion hands the travel to the game instead, which moves the character (and so the capsule).
 *
 * IK works in character space: the chain's bone positions come from walking the parents, the two rotations that
 * put the end on the target (law of cosines for the knee angle, the pole for the bend plane) are found there and
 * converted back to local rotations, so the rest of the pipeline (vcpe_rig_model) is unchanged.
 *
 * The blob is validated once in vcpe_rig_open; sampling then trusts the offsets. No allocation anywhere. */
#include <math.h>
#include <string.h>
#include "vcpe_rig.h"

#define PI_F 3.14159265f

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

/* bytes each part of a track takes, in this order: rotation, translation, scale (each 4-byte aligned) */
static uint32_t rot_bytes(int mode, int frames)
{
    return mode == VCPE_TRACK_BIND ? 0 : align4(6u * (mode == VCPE_TRACK_CONST ? 1u : (uint32_t)frames));
}
static uint32_t pos_bytes(int mode, int frames)
{
    return mode == VCPE_TRACK_BIND ? 0 : mode == VCPE_TRACK_CONST ? 12u : align4(24u + 6u * (uint32_t)frames);
}
static uint32_t scl_bytes(int mode, int frames)
{
    return mode == VCPE_TRACK_BIND ? 0 : 4u * (mode == VCPE_TRACK_CONST ? 1u : (uint32_t)frames);
}

uint32_t vcpe_rig_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int finite_f(float f) { return f == f && f < 3.0e38f && f > -3.0e38f; }

int vcpe_rig_open(VcpeRig *rig, const void *blob, uint32_t size)
{
    const uint8_t *b = blob;
    memset(rig, 0, sizeof *rig);
    if (!b || size < 32 || memcmp(b, "VRG1", 4))
        return -1;
    uint32_t nb = rd16(b + 4), nc = rd16(b + 6), total = rd32(b + 8), boff = rd32(b + 12), coff = rd32(b + 16);
    int motion = (int16_t)rd16(b + 20);
    if (((uintptr_t)b & 3) || total > size || total < 32 || nb < 1 || nb > VCPE_RIG_MAX_BONES || (boff & 3) ||
        (coff & 3) || (uint64_t)boff + nb * sizeof(VcpeRigBone) > total || (uint64_t)coff + nc * sizeof(VcpeRigClip) > total ||
        motion < -1 || motion >= (int)nb)
        return -2;
    const VcpeRigBone *bones = (const VcpeRigBone *)(b + boff);
    for (uint32_t i = 0; i < nb; i++) {
        if (bones[i].parent < -1 || bones[i].parent >= (int)i || !(bones[i].s > 0) || !finite_f(bones[i].s))
            return -2;
        for (int k = 0; k < 4; k++)
            if (!finite_f(bones[i].r[k]) || (k < 3 && !finite_f(bones[i].t[k])))
                return -2;
    }
    const VcpeRigClip *clips = (const VcpeRigClip *)(b + coff);
    for (uint32_t c = 0; c < nc; c++) {
        const VcpeRigClip *cl = &clips[c];
        if (cl->frames < 1 || !(cl->duration > 0) || !finite_f(cl->duration) || (cl->tracks & 3) || (cl->events & 3) ||
            (uint64_t)cl->tracks + nb * sizeof(VcpeRigTrack) > total ||
            (uint64_t)cl->events + cl->nevents * sizeof(VcpeRigEvent) > total)
            return -2;
        const VcpeRigTrack *tr = (const VcpeRigTrack *)(b + cl->tracks);
        for (uint32_t i = 0; i < nb; i++) {
            if (tr[i].rot > VCPE_TRACK_KEYS || tr[i].pos > VCPE_TRACK_KEYS || tr[i].scl > VCPE_TRACK_KEYS || (tr[i].off & 3))
                return -2;
            uint64_t need = (uint64_t)rot_bytes(tr[i].rot, cl->frames) + pos_bytes(tr[i].pos, cl->frames) +
                            scl_bytes(tr[i].scl, cl->frames);
            if (tr[i].off + need > total)
                return -2;
        }
        const VcpeRigEvent *ev = (const VcpeRigEvent *)(b + cl->events);
        for (int e = 0; e < cl->nevents; e++)
            if (!finite_f(ev[e].t) || (e && ev[e].t < ev[e - 1].t))
                return -2;
    }
    rig->base = b;
    rig->size = total;
    rig->bones = bones;
    rig->clips = clips;
    rig->nbones = (int)nb;
    rig->nclips = (int)nc;
    rig->motion_bone = motion;
    return 0;
}

int vcpe_rig_bone(const VcpeRig *rig, uint32_t name)
{
    for (int i = 0; i < rig->nbones; i++)
        if (rig->bones[i].name == name)
            return i;
    return -1;
}
int vcpe_rig_bone_named(const VcpeRig *rig, const char *name) { return vcpe_rig_bone(rig, vcpe_rig_hash(name)); }
int vcpe_rig_clip(const VcpeRig *rig, uint32_t name)
{
    for (int i = 0; i < rig->nclips; i++)
        if (rig->clips[i].name == name)
            return i;
    return -1;
}
int vcpe_rig_clip_named(const VcpeRig *rig, const char *name) { return vcpe_rig_clip(rig, vcpe_rig_hash(name)); }
float vcpe_rig_length(const VcpeRig *rig, int clip)
{
    return clip >= 0 && clip < rig->nclips ? rig->clips[clip].duration : 0;
}
int vcpe_rig_flags(const VcpeRig *rig, int clip) { return clip >= 0 && clip < rig->nclips ? rig->clips[clip].flags : 0; }

void vcpe_rig_mask(const VcpeRig *rig, uint8_t *mask, int bone, uint8_t weight)
{
    if (bone < 0 || bone >= rig->nbones)
        return;
    /* parents come first, so one pass from the bone down finds every descendant: a bone is inside when its parent
     * is. A scratch bit per bone marks "below `bone`" independently of the weights already in the mask. */
    uint32_t below[(VCPE_RIG_MAX_BONES + 31) / 32] = {0};
    below[bone >> 5] |= 1u << (bone & 31);
    mask[bone] = weight;
    for (int i = bone + 1; i < rig->nbones; i++) {
        int p = rig->bones[i].parent;
        if (p >= 0 && (below[p >> 5] >> (p & 31) & 1)) {
            below[i >> 5] |= 1u << (i & 31);
            mask[i] = weight;
        }
    }
}

/* ------------------------------------------------------------------ quaternions */
void vcpe_quat_mul(float o[4], const float a[4], const float b[4])
{
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
/* v' = v + 2w (u x v) + 2 u x (u x v), u = the vector part */
void vcpe_quat_rotate(const float q[4], const float v[3], float o[3])
{
    float cx = q[1] * v[2] - q[2] * v[1], cy = q[2] * v[0] - q[0] * v[2], cz = q[0] * v[1] - q[1] * v[0];
    float x = v[0] + 2 * (q[3] * cx + q[1] * cz - q[2] * cy);
    float y = v[1] + 2 * (q[3] * cy + q[2] * cx - q[0] * cz);
    float z = v[2] + 2 * (q[3] * cz + q[0] * cy - q[1] * cx);
    o[0] = x; o[1] = y; o[2] = z;
}
static void quat_norm(float q[4])
{
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (n < 1e-20f) {
        q[0] = q[1] = q[2] = 0; q[3] = 1;
        return;
    }
    n = 1.0f / sqrtf(n);
    for (int i = 0; i < 4; i++)
        q[i] *= n;
}
void vcpe_quat_nlerp(float o[4], const float a[4], const float b[4], float t)
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -t : t;
    for (int i = 0; i < 4; i++)
        o[i] = a[i] * (1 - t) + b[i] * s;
    quat_norm(o);
}
void vcpe_quat_between(float o[4], const float f[3], const float t[3])
{
    float d = f[0] * t[0] + f[1] * t[1] + f[2] * t[2];
    if (d < -0.999999f) {
        /* opposite: half a turn about any axis perpendicular to f */
        float ax[3] = {0, -f[2], f[1]};
        if (ax[1] * ax[1] + ax[2] * ax[2] < 1e-6f) { ax[0] = f[2]; ax[1] = 0; ax[2] = -f[0]; }
        o[0] = ax[0]; o[1] = ax[1]; o[2] = ax[2]; o[3] = 0;
    } else {
        o[0] = f[1] * t[2] - f[2] * t[1];
        o[1] = f[2] * t[0] - f[0] * t[2];
        o[2] = f[0] * t[1] - f[1] * t[0];
        o[3] = 1 + d;
    }
    quat_norm(o);
}
float vcpe_quat_yaw(const float q[4]) { return 2.0f * atan2f(q[1], q[3]); }
static void quat_conj(float o[4], const float q[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
static float wrap_pi(float a)
{
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

/* ------------------------------------------------------------------ keys */
static void key_rot(const uint8_t *p, float q[4])
{
    const float k = 1.41421356f / 32766.0f, o = 0.70710678f;
    unsigned a = rd16(p), b = rd16(p + 2), c = rd16(p + 4), largest = (a >> 15) << 1 | (b >> 15);
    float v[3] = {(a & 0x7fff) * k - o, (b & 0x7fff) * k - o, (c & 0x7fff) * k - o};
    float w = 1 - v[0] * v[0] - v[1] * v[1] - v[2] * v[2];
    w = w > 0 ? sqrtf(w) : 0;
    for (unsigned i = 0, j = 0; i < 4; i++)
        q[i] = i == largest ? w : v[j++];
}
static void key_pos(const uint8_t *p, int frame, float out[3])
{
    const float *box = (const float *)p;      /* min[3], step[3] */
    const uint8_t *k = p + 24 + 6 * frame;
    for (int i = 0; i < 3; i++)
        out[i] = box[i] + box[3 + i] * rd16(k + 2 * i);
}

/* one bone of one clip between frames fa and fb (k: 0..1 from fa) */
static void track_sample(const VcpeRig *rig, const VcpeRigClip *c, int bone, int fa, int fb, float k, VcpeXform *x)
{
    const VcpeRigTrack *tr = (const VcpeRigTrack *)(rig->base + c->tracks) + bone;
    const VcpeRigBone *bb = &rig->bones[bone];
    const uint8_t *p = rig->base + tr->off;
    if (tr->rot == VCPE_TRACK_BIND)
        memcpy(x->r, bb->r, sizeof x->r);
    else if (tr->rot == VCPE_TRACK_CONST || fa == fb)
        key_rot(p + (tr->rot == VCPE_TRACK_CONST ? 0 : 6 * fa), x->r);
    else {
        float qa[4], qb[4];
        key_rot(p + 6 * fa, qa);
        key_rot(p + 6 * fb, qb);
        vcpe_quat_nlerp(x->r, qa, qb, k);
    }
    p += rot_bytes(tr->rot, c->frames);
    if (tr->pos == VCPE_TRACK_BIND)
        memcpy(x->t, bb->t, sizeof x->t);
    else if (tr->pos == VCPE_TRACK_CONST)
        memcpy(x->t, p, sizeof x->t);
    else {
        float ta[3], tb[3];
        key_pos(p, fa, ta);
        key_pos(p, fb, tb);
        for (int i = 0; i < 3; i++)
            x->t[i] = ta[i] + (tb[i] - ta[i]) * k;
    }
    p += pos_bytes(tr->pos, c->frames);
    if (tr->scl == VCPE_TRACK_BIND)
        x->s = bb->s;
    else if (tr->scl == VCPE_TRACK_CONST)
        x->s = *(const float *)p;
    else {
        const float *s = (const float *)p;
        x->s = s[fa] + (s[fb] - s[fa]) * k;
    }
}

static float clip_time(const VcpeRigClip *c, float t, int loop)
{
    if (!(t == t))
        return 0;
    if (loop) {
        t = fmodf(t, c->duration);
        if (t < 0)
            t += c->duration;
        return t;
    }
    return t < 0 ? 0 : t > c->duration ? c->duration : t;
}
static void frames_at(const VcpeRigClip *c, float t, int *fa, int *fb, float *k)
{
    float f = t * (float)(c->frames - 1) / c->duration;
    int a = (int)f;
    if (a < 0) a = 0;
    if (a >= c->frames - 1) {
        *fa = *fb = c->frames - 1;
        *k = 0;
        return;
    }
    *fa = a;
    *fb = a + 1;
    *k = f - (float)a;
}

/* the motion bone as authored (with its travel), at clip time t */
static void motion_raw(const VcpeRig *rig, const VcpeRigClip *c, float t, VcpeXform *x)
{
    int fa, fb;
    float k;
    frames_at(c, t, &fa, &fb, &k);
    track_sample(rig, c, rig->motion_bone, fa, fb, k, x);
}

/* take the travel/turn since frame 0 out of the motion bone's pose */
static void strip_motion(const VcpeRig *rig, const VcpeRigClip *c, VcpeXform *x)
{
    VcpeXform first;
    track_sample(rig, c, rig->motion_bone, 0, 0, 0, &first);
    if (c->flags & VCPE_RIG_MOTION_XZ) {
        x->t[0] = first.t[0];
        x->t[2] = first.t[2];
    }
    if (c->flags & VCPE_RIG_MOTION_YAW) {
        float h = -0.5f * wrap_pi(vcpe_quat_yaw(x->r) - vcpe_quat_yaw(first.r));
        float undo[4] = {0, sinf(h), 0, cosf(h)};
        vcpe_quat_mul(x->r, undo, x->r);
    }
}

void vcpe_rig_bind_pose(const VcpeRig *rig, VcpeXform *out)
{
    for (int i = 0; i < rig->nbones; i++) {
        memcpy(out[i].r, rig->bones[i].r, sizeof out[i].r);
        memcpy(out[i].t, rig->bones[i].t, sizeof out[i].t);
        out[i].s = rig->bones[i].s;
    }
}

int vcpe_rig_sample(const VcpeRig *rig, int clip, float t, int loop, VcpeXform *out, const uint8_t *mask)
{
    if (clip < 0 || clip >= rig->nclips || !out)
        return -1;
    const VcpeRigClip *c = &rig->clips[clip];
    int fa, fb;
    float k;
    frames_at(c, clip_time(c, t, loop), &fa, &fb, &k);
    for (int i = 0; i < rig->nbones; i++)
        if (!mask || mask[i])
            track_sample(rig, c, i, fa, fb, k, &out[i]);
    int m = rig->motion_bone;
    if (m >= 0 && (c->flags & (VCPE_RIG_MOTION_XZ | VCPE_RIG_MOTION_YAW)) && (!mask || mask[m]))
        strip_motion(rig, c, &out[m]);
    return 0;
}

int vcpe_rig_sample_delta(const VcpeRig *rig, int clip, float t, int loop, VcpeXform *out, const uint8_t *mask)
{
    if (vcpe_rig_sample(rig, clip, t, loop, out, mask))
        return -1;
    const VcpeRigClip *c = &rig->clips[clip];
    for (int i = 0; i < rig->nbones; i++) {
        if (mask && !mask[i])
            continue;
        VcpeXform ref;
        float inv[4];
        track_sample(rig, c, i, 0, 0, 0, &ref);
        quat_conj(inv, ref.r);
        vcpe_quat_mul(out[i].r, inv, out[i].r);
        for (int k = 0; k < 3; k++)
            out[i].t[k] -= ref.t[k];
        out[i].s = ref.s > 0 ? out[i].s / ref.s : 1;
    }
    return 0;
}

void vcpe_rig_blend(const VcpeRig *rig, VcpeXform *dst, const VcpeXform *src, float w, const uint8_t *mask)
{
    if (w <= 0)
        return;
    if (w > 1)
        w = 1;
    for (int i = 0; i < rig->nbones; i++) {
        float k = mask ? w * mask[i] * (1.0f / 255.0f) : w;
        if (k <= 0)
            continue;
        vcpe_quat_nlerp(dst[i].r, dst[i].r, src[i].r, k);
        for (int j = 0; j < 3; j++)
            dst[i].t[j] += (src[i].t[j] - dst[i].t[j]) * k;
        dst[i].s += (src[i].s - dst[i].s) * k;
    }
}

void vcpe_rig_add(const VcpeRig *rig, VcpeXform *dst, const VcpeXform *delta, float w, const uint8_t *mask)
{
    static const float one[4] = {0, 0, 0, 1};
    if (w <= 0)
        return;
    for (int i = 0; i < rig->nbones; i++) {
        float k = mask ? w * mask[i] * (1.0f / 255.0f) : w, q[4];
        if (k <= 0)
            continue;
        vcpe_quat_nlerp(q, one, delta[i].r, k);
        vcpe_quat_mul(dst[i].r, dst[i].r, q);
        quat_norm(dst[i].r);
        for (int j = 0; j < 3; j++)
            dst[i].t[j] += delta[i].t[j] * k;
        dst[i].s *= 1 + (delta[i].s - 1) * k;
    }
}

/* ------------------------------------------------------------------ matrices */
static void xform_matrix(const VcpeXform *x, float m[12])
{
    float X = x->r[0], Y = x->r[1], Z = x->r[2], W = x->r[3], s = x->s;
    float xx = X * X, yy = Y * Y, zz = Z * Z, xy = X * Y, xz = X * Z, yz = Y * Z, wx = W * X, wy = W * Y, wz = W * Z;
    m[0] = (1 - 2 * (yy + zz)) * s; m[1] = 2 * (xy - wz) * s;       m[2] = 2 * (xz + wy) * s;        m[3] = x->t[0];
    m[4] = 2 * (xy + wz) * s;       m[5] = (1 - 2 * (xx + zz)) * s; m[6] = 2 * (yz - wx) * s;        m[7] = x->t[1];
    m[8] = 2 * (xz - wy) * s;       m[9] = 2 * (yz + wx) * s;       m[10] = (1 - 2 * (xx + yy)) * s; m[11] = x->t[2];
}
static void affine_mul(float o[12], const float a[12], const float b[12])
{
    float r[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (j == 3 ? a[i * 4 + 3] : 0);
    memcpy(o, r, sizeof r);
}

void vcpe_rig_model(const VcpeRig *rig, const VcpeXform *local, float (*model)[12])
{
    for (int i = 0; i < rig->nbones; i++) {
        int p = rig->bones[i].parent;
        if (p < 0)
            xform_matrix(&local[i], model[i]);
        else {
            float m[12];
            xform_matrix(&local[i], m);
            affine_mul(model[i], model[p], m);
        }
    }
}

void vcpe_rig_skin(const VcpeRig *rig, const float (*model)[12], float (*skin)[12])
{
    for (int i = 0; i < rig->nbones; i++)
        affine_mul(skin[i], model[i], rig->bones[i].inv_bind);
}

static void compose(VcpeXform *o, const VcpeXform *parent, const VcpeXform *child)
{
    float t[3], r[4];
    for (int i = 0; i < 3; i++)
        t[i] = child->t[i] * parent->s;
    vcpe_quat_rotate(parent->r, t, t);
    vcpe_quat_mul(r, parent->r, child->r);
    for (int i = 0; i < 3; i++)
        o->t[i] = parent->t[i] + t[i];
    memcpy(o->r, r, sizeof r);
    o->s = parent->s * child->s;
}

void vcpe_rig_model_xform(const VcpeRig *rig, const VcpeXform *local, int bone, VcpeXform *out)
{
    static const VcpeXform identity = {{0, 0, 0, 1}, {0, 0, 0}, 1};
    int chain[VCPE_RIG_MAX_BONES], n = 0;
    *out = identity;
    for (int b = bone; b >= 0 && b < rig->nbones && n < VCPE_RIG_MAX_BONES; b = rig->bones[b].parent)
        chain[n++] = b;
    while (n--)
        compose(out, out, &local[chain[n]]);
}

void vcpe_rig_attach(const float w[16], const float m[12], const float *offset, float out[16])
{
    float a[16], r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            /* w x (bone as 4x4) */
            float v = 0;
            for (int k = 0; k < 3; k++)
                v += w[i * 4 + k] * m[k * 4 + j];
            a[i * 4 + j] = v + (j == 3 ? w[i * 4 + 3] : 0);
        }
    if (!offset) {
        memcpy(out, a, sizeof a);
        return;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * offset[j] + a[i * 4 + 1] * offset[4 + j] + a[i * 4 + 2] * offset[8 + j] +
                           a[i * 4 + 3] * offset[12 + j];
    memcpy(out, r, sizeof r);
}

/* ------------------------------------------------------------------ root motion */
typedef struct { float p[3], yaw; } Motion;

/* travel within one cycle, a <= b in 0..duration */
static void motion_segment(const VcpeRig *rig, const VcpeRigClip *c, float a, float b, Motion *m)
{
    VcpeXform xa, xb, first;
    m->p[0] = m->p[1] = m->p[2] = m->yaw = 0;
    if (b <= a)
        return;
    motion_raw(rig, c, a, &xa);
    motion_raw(rig, c, b, &xb);
    track_sample(rig, c, rig->motion_bone, 0, 0, 0, &first);
    if (c->flags & VCPE_RIG_MOTION_YAW) {
        /* add up the turn frame by frame so a 360-degree turn clip is not folded back to zero */
        float step = c->duration / (float)(c->frames > 1 ? c->frames - 1 : 1), prev = vcpe_quat_yaw(xa.r);
        int f0 = (int)ceilf(a / step), f1 = (int)floorf(b / step);
        if (f1 > c->frames - 1) f1 = c->frames - 1;
        for (int f = f0 < 0 ? 0 : f0; f <= f1; f++) {
            if (f * step <= a || f * step >= b)
                continue;
            VcpeXform x;
            track_sample(rig, c, rig->motion_bone, f, f, 0, &x);
            float y = vcpe_quat_yaw(x.r);
            m->yaw += wrap_pi(y - prev);
            prev = y;
        }
        m->yaw += wrap_pi(vcpe_quat_yaw(xb.r) - prev);
    }
    if (c->flags & VCPE_RIG_MOTION_XZ) {
        float dx = xb.t[0] - xa.t[0], dz = xb.t[2] - xa.t[2];
        if (c->flags & VCPE_RIG_MOTION_YAW) {
            /* into the frame of the character at a: undo the heading the clip had reached there */
            float h = wrap_pi(vcpe_quat_yaw(xa.r) - vcpe_quat_yaw(first.r)), cs = cosf(h), sn = sinf(h);
            float x = dx * cs - dz * sn, z = dx * sn + dz * cs;
            dx = x;
            dz = z;
        }
        m->p[0] = dx;
        m->p[2] = dz;
    }
}
/* m then n: n's travel happens after m's turn */
static void motion_then(Motion *m, const Motion *n)
{
    float cs = cosf(m->yaw), sn = sinf(m->yaw);
    m->p[0] += n->p[0] * cs + n->p[2] * sn;
    m->p[2] += -n->p[0] * sn + n->p[2] * cs;
    m->yaw += n->yaw;
}

void vcpe_rig_motion(const VcpeRig *rig, int clip, float from, float to, int loop, float dpos[3], float *dyaw)
{
    Motion total = {{0, 0, 0}, 0};
    dpos[0] = dpos[1] = dpos[2] = 0;
    if (dyaw) *dyaw = 0;
    if (clip < 0 || clip >= rig->nclips || rig->motion_bone < 0 || !(to > from))
        return;
    const VcpeRigClip *c = &rig->clips[clip];
    if (!(c->flags & (VCPE_RIG_MOTION_XZ | VCPE_RIG_MOTION_YAW)))
        return;
    float d = c->duration;
    if (!loop) {
        motion_segment(rig, c, clip_time(c, from, 0), clip_time(c, to, 0), &total);
    } else {
        float cf = floorf(from / d), ct = floorf(to / d), a = from - cf * d, b = to - ct * d;
        float cycles = ct - cf;
        if (cycles <= 0)
            motion_segment(rig, c, a, b, &total);
        else {
            Motion part, full;
            motion_segment(rig, c, a, d, &total);
            if (cycles > 1) {
                motion_segment(rig, c, 0, d, &full);
                for (int i = 1; i < cycles && i <= 8; i++)
                    motion_then(&total, &full);
            }
            motion_segment(rig, c, 0, b, &part);
            motion_then(&total, &part);
        }
    }
    memcpy(dpos, total.p, sizeof total.p);
    if (dyaw) *dyaw = total.yaw;
}

int vcpe_rig_events(const VcpeRig *rig, int clip, float from, float to, int loop, VcpeRigEventFn fn, void *user)
{
    if (clip < 0 || clip >= rig->nclips || !(to > from) || !fn)
        return 0;
    const VcpeRigClip *c = &rig->clips[clip];
    const VcpeRigEvent *ev = (const VcpeRigEvent *)(rig->base + c->events);
    int fired = 0;
    if (!loop) {
        for (int e = 0; e < c->nevents; e++)
            if (ev[e].t > from && ev[e].t <= to) {
                fn(user, clip, &ev[e]);
                fired++;
            }
        return fired;
    }
    float d = c->duration, cf = floorf(from / d), ct = floorf(to / d);
    if (ct - cf > 8)
        cf = ct - 8;
    for (float cyc = cf; cyc <= ct; cyc++)
        for (int e = 0; e < c->nevents; e++) {
            float t = cyc * d + ev[e].t;
            if (t > from && t <= to) {
                fn(user, clip, &ev[e]);
                fired++;
            }
        }
    return fired;
}

/* ------------------------------------------------------------------ skinned meshes */
int vcpe_skinmesh_open(VcpeSkinMesh *m, const void *blob, uint32_t size)
{
    const uint8_t *b = blob;
    memset(m, 0, sizeof *m);
    if (!b || size < 48 || memcmp(b, "VSM1", 4))
        return -1;
    uint32_t nv = rd16(b + 4), ns = rd16(b + 6), nl = rd16(b + 8), nbones = rd16(b + 10), total = rd32(b + 12);
    uint32_t vtx = rd32(b + 16), bind = rd32(b + 20), sub = rd32(b + 24), lvl = rd32(b + 28);
    if (((uintptr_t)b & 15) || total > size || nv < 1 || nl < 1 || nl > VCPE_SKINMESH_LOD || nbones > VCPE_RIG_MAX_BONES ||
        (vtx & 15) || (bind & 3) || (sub & 3) || (lvl & 3) || (uint64_t)vtx + nv * sizeof(VcpeVtx) > total ||
        (uint64_t)bind + nv * sizeof(VcpeSkinVertex) > total || (uint64_t)sub + ns * sizeof(VcpeSkinSub) > total ||
        (uint64_t)lvl + nl * sizeof(VcpeSkinLevel) > total)
        return -2;
    const VcpeSkinLevel *levels = (const VcpeSkinLevel *)(b + lvl);
    const VcpeSkinSub *subs = (const VcpeSkinSub *)(b + sub);
    const VcpeSkinVertex *sv = (const VcpeSkinVertex *)(b + bind);
    for (uint32_t l = 0; l < nl; l++)
        if (levels[l].nverts < 1 || levels[l].nverts > nv || (l && levels[l].nverts > levels[l - 1].nverts))
            return -2;
    for (uint32_t s = 0; s < ns; s++)
        for (uint32_t l = 0; l < nl; l++) {
            uint32_t off = subs[s].ioff[l], n = subs[s].nidx[l];
            if (n % 3 || (n && (off & 1)) || (uint64_t)off + n * 2u > total)
                return -2;
            const uint16_t *ix = (const uint16_t *)(b + off);
            for (uint32_t i = 0; i < n; i++)
                if (ix[i] >= levels[l].nverts)
                    return -2;   /* a level only reaches the vertices it skins */
        }
    for (uint32_t i = 0; i < nv; i++)
        for (int k = 0; k < 4; k++)
            if (sv[i].weight[k] > 0 && sv[i].bone[k] >= nbones)
                return -2;
    m->base = b;
    m->verts = (const VcpeVtx *)(b + vtx);
    m->bind = sv;
    m->subs = subs;
    m->levels = levels;
    m->nverts = (int)nv;
    m->nsubs = (int)ns;
    m->nlevels = (int)nl;
    m->nbones = (int)nbones;
    for (int i = 0; i < 4; i++) {
        float f;
        memcpy(&f, b + 32 + 4 * i, 4);
        if (i < 3) m->centre[i] = f;
        else m->radius = f;
    }
    return 0;
}

void vcpe_skinmesh_init(const VcpeSkinMesh *m, VcpeVtx *out, uint32_t tint)
{
    memcpy(out, m->verts, (size_t)m->nverts * sizeof *out);
    if (tint == 0xffffffffu)
        return;
    for (int i = 0; i < m->nverts; i++) {
        uint32_t c = out[i].color, r = 0;
        for (int k = 0; k < 32; k += 8)
            r |= ((((c >> k) & 255) * ((tint >> k) & 255) + 127) / 255) << k;
        out[i].color = r;
    }
}

int vcpe_skinmesh_skin(const VcpeSkinMesh *m, int level, const float (*skin)[12], int bones, VcpeVtx *out)
{
    if (bones < m->nbones)
        return -1;
    if (level < 0) level = 0;
    if (level >= m->nlevels) level = m->nlevels - 1;
    return vcpe_skin_apply(out, m->bind, m->levels[level].nverts, skin, bones);
}

/* the vcpe_lod rule: the coarsest level whose error, seen from the nearest point of the bounding sphere, stays
 * under the threshold; going coarser needs the error below threshold x down_k (hysteresis) */
int vcpe_skinmesh_lod(const VcpeSkinMesh *m, const VcpeLodView *v, const float c[3], float scale, int prev)
{
    float d[3] = {c[0] - v->pos[0], c[1] - v->pos[1], c[2] - v->pos[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - m->radius * scale;
    if (dist < 0.01f)
        return 0;
    int best = 0;
    for (int l = 1; l < m->nlevels; l++) {
        float px = m->levels[l].err * scale * v->px_k / dist, lim = v->thresh_px * (l > prev ? v->down_k : 1.0f);
        if (px <= lim)
            best = l;
        else
            break;
    }
    return best;
}

/* ------------------------------------------------------------------ IK */
static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int unit3(float v[3])
{
    float l = len3(v);
    if (l < 1e-8f)
        return 0;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return 1;
}

int vcpe_rig_ik2(const VcpeRig *rig, VcpeXform *local, int a, int b, int c, const float target[3],
                 const float pole[3], float weight, int keep_end)
{
    if (a < 0 || b < 0 || c < 0 || a >= rig->nbones || b >= rig->nbones || c >= rig->nbones ||
        rig->bones[b].parent != a || rig->bones[c].parent != b)
        return -1;
    if (weight <= 0)
        return 1;
    if (weight > 1)
        weight = 1;
    VcpeXform mp, ma, mb, mc;
    int parent = rig->bones[a].parent;
    vcpe_rig_model_xform(rig, local, parent, &mp);   /* identity for a root */
    compose(&ma, &mp, &local[a]);
    compose(&mb, &ma, &local[b]);
    compose(&mc, &mb, &local[c]);
    float ab[3], bc[3], to[3];
    for (int i = 0; i < 3; i++) {
        ab[i] = mb.t[i] - ma.t[i];
        bc[i] = mc.t[i] - mb.t[i];
        to[i] = target[i] - ma.t[i];
    }
    float l1 = len3(ab), l2 = len3(bc), d = len3(to), lo = fabsf(l1 - l2) + 1e-4f, hi = l1 + l2 - 1e-4f;
    if (l1 < 1e-6f || l2 < 1e-6f || d < 1e-6f || hi <= lo)
        return 0;
    int reached = 1;
    if (d > hi) { d = hi; reached = 0; }
    if (d < lo) { d = lo; reached = 0; }
    float n[3] = {to[0], to[1], to[2]}, m[3];
    unit3(n);
    /* bend plane: the pole's direction with its part along n removed; the current knee if the pole is on the line */
    for (int i = 0; i < 3; i++)
        m[i] = pole[i] - ma.t[i];
    float k = dot3(m, n);
    for (int i = 0; i < 3; i++)
        m[i] -= n[i] * k;
    if (!unit3(m)) {
        k = dot3(ab, n);
        for (int i = 0; i < 3; i++)
            m[i] = ab[i] - n[i] * k;
        if (!unit3(m)) {
            float any[3] = {n[1], -n[0], 0};
            if (!unit3(any)) { any[0] = 1; any[1] = any[2] = 0; }
            memcpy(m, any, sizeof m);
        }
    }
    float ca = (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d);
    ca = ca < -1 ? -1 : ca > 1 ? 1 : ca;
    float sa = sqrtf(1 - ca * ca), knee[3], end[3];
    for (int i = 0; i < 3; i++) {
        knee[i] = ma.t[i] + n[i] * l1 * ca + m[i] * l1 * sa;
        end[i] = ma.t[i] + n[i] * d;
    }
    /* swing a so its bone points at the knee, then b so it points at the end */
    float u[3] = {ab[0], ab[1], ab[2]}, v[3], qa[4], qb[4], pc[3], na[4], nb[4];
    for (int i = 0; i < 3; i++)
        v[i] = knee[i] - ma.t[i];
    unit3(u);
    unit3(v);
    vcpe_quat_between(qa, u, v);
    for (int i = 0; i < 3; i++)
        pc[i] = mc.t[i] - ma.t[i];
    vcpe_quat_rotate(qa, pc, pc);
    for (int i = 0; i < 3; i++) {
        u[i] = ma.t[i] + pc[i] - knee[i];
        v[i] = end[i] - knee[i];
    }
    unit3(u);
    unit3(v);
    vcpe_quat_between(qb, u, v);
    vcpe_quat_mul(na, qa, ma.r);
    vcpe_quat_mul(nb, qb, qa);
    vcpe_quat_mul(nb, nb, mb.r);
    quat_norm(na);
    quat_norm(nb);
    /* back to local rotations: model = parent model x local */
    float inv[4], la[4], lb[4];
    quat_conj(inv, mp.r);
    vcpe_quat_mul(la, inv, na);
    quat_conj(inv, na);
    vcpe_quat_mul(lb, inv, nb);
    vcpe_quat_nlerp(local[a].r, local[a].r, la, weight);
    vcpe_quat_nlerp(local[b].r, local[b].r, lb, weight);
    if (keep_end) {
        /* the end keeps the rotation it had in character space before the chain moved */
        float lc[4], nbw[4];
        VcpeXform tmp;
        compose(&tmp, &mp, &local[a]);
        compose(&tmp, &tmp, &local[b]);
        memcpy(nbw, tmp.r, sizeof nbw);
        quat_conj(inv, nbw);
        vcpe_quat_mul(lc, inv, mc.r);
        quat_norm(lc);
        memcpy(local[c].r, lc, sizeof lc);
    }
    return reached;
}
