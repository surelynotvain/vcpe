/* VCPE 3D scene: node tree, world matrices, mesh drawing and Godot-style AnimationPlayers (see vcpe_node3d.h). */
#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "vcpe_3d.h"
#include "vcpe_node3d.h"

typedef struct {
    float pos[3], rot[3], scale[3];
    int16_t mesh, parent;
    uint8_t visible, transparency;
    uint32_t layers;
    uint8_t dirty, moved;   /* dirty: recompute the local matrix; moved: world matrix changed in this update */
    float key[9];           /* pos, rot, scale the cached local matrix was built from */
    float local[16], world[16];
} Node;

typedef struct {
    int clip;          /* playing clip (index into the scene's clips), or -1 */
    float t;
    int playing;
} Player;

static const VcpeScene3D *S;
static Node *nodes;
static Player *players;
float vcpe_anim_fov;

uint32_t vcpe_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ nodes */
void vcpe_node3d_load(const VcpeScene3D *s)
{
    S = s;
    free(nodes);
    free(players);
    nodes = calloc(s->nnodes ? s->nnodes : 1, sizeof(Node));
    players = calloc(s->nplayers ? s->nplayers : 1, sizeof(Player));
    for (int i = 0; i < s->nnodes; i++) {
        const VcpeNodeDef *d = &s->nodes[i];
        memcpy(nodes[i].pos, d->pos, sizeof d->pos);
        memcpy(nodes[i].rot, d->rot, sizeof d->rot);
        memcpy(nodes[i].scale, d->scale, sizeof d->scale);
        nodes[i].visible = d->visible;
        nodes[i].layers = d->layers;
        nodes[i].mesh = d->mesh;
        nodes[i].parent = d->parent;
        nodes[i].transparency = d->transparency;
        nodes[i].dirty = 1;
    }
    for (int p = 0; p < s->nplayers; p++) {
        players[p].clip = -1;
        if (s->players[p].autoplay >= 0) {
            players[p].clip = s->players[p].first + s->players[p].autoplay;
            players[p].playing = 1;
        }
    }
    vcpe_anim_fov = 0;
}

int vcpe_node3d_find(uint32_t h)
{
    for (int i = 0; S && i < S->nnodes; i++)
        if (S->nodes[i].name == h)
            return i;
    return -1;
}

void vcpe_node3d_set_visible(int n, int on) { if (n >= 0) nodes[n].visible = on != 0; }
int vcpe_node3d_visible(int n) { return n >= 0 && nodes[n].visible; }
void vcpe_node3d_set_layers(int n, uint32_t l) { if (n >= 0) nodes[n].layers = l; }
void vcpe_node3d_set_mesh(int n, int m) { if (n >= 0) nodes[n].mesh = m; }
void vcpe_node3d_set_transparency(int n, float t) { if (n >= 0) nodes[n].transparency = (uint8_t)(t < 0 ? 0 : t > 1 ? 255 : t * 255); }
float *vcpe_node3d_pos(int n) { return nodes[n].pos; }
float *vcpe_node3d_rot(int n) { return nodes[n].rot; }
float *vcpe_node3d_scale(int n) { return nodes[n].scale; }
void vcpe_node3d_world(int n, float out[16]) { memcpy(out, nodes[n].world, sizeof nodes[n].world); }

static void mat_mul(const float *a, const float *b, float *o)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, sizeof r);
}

/* T * Ry * Rx * Rz * S (Godot's default YXZ euler order), row-major */
static void local_matrix(const Node *n, float *m)
{
    float cx = cosf(n->rot[0]), sx = sinf(n->rot[0]), cy = cosf(n->rot[1]), sy = sinf(n->rot[1]);
    float cz = cosf(n->rot[2]), sz = sinf(n->rot[2]);
    float r[9] = {
        cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx,
        cx * sz, cx * cz, -sx,
        -sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx,
    };
    for (int i = 0; i < 3; i++) {
        m[i * 4 + 0] = r[i * 3 + 0] * n->scale[0];
        m[i * 4 + 1] = r[i * 3 + 1] * n->scale[1];
        m[i * 4 + 2] = r[i * 3 + 2] * n->scale[2];
        m[i * 4 + 3] = n->pos[i];
    }
    m[12] = m[13] = m[14] = 0;
    m[15] = 1;
}

static void update_world(void)
{
    /* the converter writes parents before their children. Local matrices are cached and only rebuilt when
     * pos/rot/scale changed (they are writable through vcpe_node3d_pos/rot/scale, so compare a snapshot);
     * world matrices only when the local matrix or the parent's world moved. */
    for (int i = 0; i < S->nnodes; i++) {
        Node *n = &nodes[i];
        int p = n->parent;
        n->moved = 0;
        if (n->dirty || memcmp(n->key, n->pos, sizeof n->key)) {
            memcpy(n->key, n->pos, sizeof n->key);
            local_matrix(n, n->local);
            n->dirty = 0;
            n->moved = 1;
        } else if (!(p >= 0 && nodes[p].moved)) {
            continue;
        }
        n->moved = 1;
        if (p >= 0)
            mat_mul(nodes[p].world, n->local, n->world);
        else if (S->nodes[i].base >= 0)
            mat_mul(S->bases[S->nodes[i].base], n->local, n->world);
        else
            memcpy(n->world, n->local, sizeof n->local);
    }
}

void vcpe_node3d_refresh(void) { update_world(); }

/* inverse of an affine row-major 4x4 */
static void mat_inv_affine(const float *m, float *o)
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    float id = det != 0 ? 1.0f / det : 0;
    o[0] = (e * k - f * h) * id; o[1] = (c * h - b * k) * id; o[2] = (b * f - c * e) * id;
    o[4] = (f * g - d * k) * id; o[5] = (a * k - c * g) * id; o[6] = (c * d - a * f) * id;
    o[8] = (d * h - e * g) * id; o[9] = (b * g - a * h) * id; o[10] = (a * e - b * d) * id;
    for (int r = 0; r < 3; r++)
        o[r * 4 + 3] = -(o[r * 4] * m[3] + o[r * 4 + 1] * m[7] + o[r * 4 + 2] * m[11]);
    o[12] = o[13] = o[14] = 0;
    o[15] = 1;
}

void vcpe_node3d_set_parent(int n, int parent)
{
    if (n < 0) return;
    update_world();
    float pinv[16], l[16];
    if (parent >= 0) {
        mat_inv_affine(nodes[parent].world, pinv);
        mat_mul(pinv, nodes[n].world, l);
    } else {
        memcpy(l, nodes[n].world, sizeof l);
    }
    Node *d = &nodes[n];
    for (int k = 0; k < 3; k++) d->pos[k] = l[k * 4 + 3];
    float sx = sqrtf(l[0] * l[0] + l[4] * l[4] + l[8] * l[8]);
    float sy = sqrtf(l[1] * l[1] + l[5] * l[5] + l[9] * l[9]);
    float sz = sqrtf(l[2] * l[2] + l[6] * l[6] + l[10] * l[10]);
    float det = l[0] * (l[5] * l[10] - l[6] * l[9]) - l[1] * (l[4] * l[10] - l[6] * l[8]) + l[2] * (l[4] * l[9] - l[5] * l[8]);
    if (det < 0) sx = -sx;
    d->scale[0] = sx; d->scale[1] = sy; d->scale[2] = sz;
    float r12 = l[6] / sz, r02 = l[2] / sz, r22 = l[10] / sz, r10 = l[4] / sx, r11 = l[5] / sy;
    if (r12 > 1) r12 = 1;
    if (r12 < -1) r12 = -1;
    d->rot[0] = asinf(-r12);                       /* YXZ euler, as Godot's get_euler() */
    if (fabsf(r12) < 0.99999f) {
        d->rot[1] = atan2f(r02, r22);
        d->rot[2] = atan2f(r10, r11);
    } else {
        d->rot[1] = atan2f(-l[8] / sx, l[0] / sx);
        d->rot[2] = 0;
    }
    d->parent = parent;
    d->dirty = 1;
}

static int shown(int i)
{
    for (; i >= 0; i = nodes[i].parent)
        if (!nodes[i].visible)
            return 0;
    return 1;
}

int vcpe_node3d_shown(int n) { return n >= 0 && shown(n); }
int vcpe_node3d_parent(int n) { return n >= 0 ? nodes[n].parent : -1; }
uint32_t vcpe_node3d_layers(int n) { return n >= 0 ? nodes[n].layers : 0; }
float vcpe_node3d_opacity(int n) { return n >= 0 ? 1.0f - nodes[n].transparency / 255.0f : 0; }

void vcpe_node3d_draw(uint32_t cull)
{
    for (int i = 0; i < S->nnodes; i++) {
        int m = nodes[i].mesh;
        /* fully transparent (GeometryInstance3D.transparency 1): not drawn; partial transparency comes later */
        if (m < 0 || !(nodes[i].layers & cull) || nodes[i].transparency >= 250 || !shown(i))
            continue;
        const VcpeMeshDef *mdf = &S->meshes[m];
        if (mdf->bound[3] > 0) {   /* off-screen: skip (sphere moved by the world matrix) */
            const float *w = nodes[i].world, *b = mdf->bound;
            float c[3];
            for (int k = 0; k < 3; k++) c[k] = w[k * 4] * b[0] + w[k * 4 + 1] * b[1] + w[k * 4 + 2] * b[2] + w[k * 4 + 3];
            float s0 = w[0] * w[0] + w[4] * w[4] + w[8] * w[8], s1 = w[1] * w[1] + w[5] * w[5] + w[9] * w[9];
            float s2 = w[2] * w[2] + w[6] * w[6] + w[10] * w[10], sm = s0 > s1 ? s0 : s1;
            if (s2 > sm) sm = s2;
            if (!vcpe_3d_visible(c, b[3] * sqrtf(sm)))
                continue;
        }
        vcpe_3d_model(nodes[i].world);
        if (nodes[i].transparency)
            vcpe_3d_alpha(1.0f - nodes[i].transparency / 255.0f);
        const VcpeMeshDef *md = &S->meshes[m];
        for (int b = md->first; b < md->first + md->n; b++) {
            const VcpeBatch *bt = &S->batches[b];
            vcpe_3d_draw(bt->page, S->geo + (bt->voff - S->geo_off), (const uint16_t *)(S->geo + (bt->ioff - S->geo_off)), bt->nidx);
        }
        if (nodes[i].transparency)
            vcpe_3d_alpha(1);
    }
    vcpe_3d_model(0);
}

/* ------------------------------------------------------------------ animation */
static float ease(float x, float c)   /* Godot's ease() */
{
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    if (c > 0) {
        if (c < 1) return 1.0f - powf(1.0f - x, 1.0f / c);
        return powf(x, c);
    }
    if (c < 0) {
        if (x < 0.5f) return powf(x * 2.0f, -c) * 0.5f;
        return (1.0f - powf(1.0f - (x - 0.5f) * 2.0f, -c)) * 0.5f + 0.5f;
    }
    return 0;
}

static float cubic(float a, float b, float c, float d, float t)   /* Godot cubic_interpolate (Catmull-Rom) */
{
    return 0.5f * ((b * 2.0f) + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t * t + (-a + 3.0f * b - 3.0f * c + d) * t * t * t);
}

static void sample(const VcpeTrackDef *tr, float t, float out[3])
{
    const VcpeKeyDef *k = S->keys + tr->first;
    int n = tr->nkeys;
    if (t <= k[0].t || n == 1) { memcpy(out, k[0].v, 12); return; }
    if (t >= k[n - 1].t) { memcpy(out, k[n - 1].v, 12); return; }
    int i = 0;
    while (i + 1 < n && k[i + 1].t <= t)
        i++;
    if (tr->discrete || tr->interp == VCPE_INTERP_NEAREST || tr->kind == VCPE_TR_VISIBLE) {
        memcpy(out, k[i].v, 12);
        return;
    }
    float w = ease((t - k[i].t) / (k[i + 1].t - k[i].t), k[i].trans);
    const float *a = k[i > 0 ? i - 1 : i].v, *b = k[i].v, *c = k[i + 1].v, *d = k[i + 2 < n ? i + 2 : i + 1].v;
    for (int j = 0; j < 3; j++)
        out[j] = tr->interp == VCPE_INTERP_CUBIC ? cubic(a[j], b[j], c[j], d[j], w) : b[j] + (c[j] - b[j]) * w;
}

static void apply(int p, const VcpeClipDef *cl, float t0, float t1, int first)
{
    for (int ti = cl->first; ti < cl->first + cl->n; ti++) {
        const VcpeTrackDef *tr = &S->tracks[ti];
        if (tr->kind == VCPE_TR_METHOD || tr->kind == VCPE_TR_AUDIO) {
            const VcpeKeyDef *k = S->keys + tr->first;
            for (int i = 0; i < tr->nkeys; i++) {
                if ((k[i].t > t0 || (first && k[i].t >= t0)) && k[i].t <= t1) {
                    if (tr->kind == VCPE_TR_METHOD && S->on_method)
                        S->on_method(p, tr->node, (int)k[i].v[0]);
                    if (tr->kind == VCPE_TR_AUDIO && S->on_audio)
                        S->on_audio(p, (int)k[i].v[0], k[i].v[1]);
                }
            }
            continue;
        }
        if (tr->node < 0)
            continue;
        float v[3];
        sample(tr, t1, v);
        Node *n = &nodes[tr->node];
        switch (tr->kind) {
        case VCPE_TR_POS: memcpy(n->pos, v, 12); break;
        case VCPE_TR_ROT: memcpy(n->rot, v, 12); break;
        case VCPE_TR_SCALE: memcpy(n->scale, v, 12); break;
        case VCPE_TR_VISIBLE: n->visible = v[0] > 0.5f; break;
        case VCPE_TR_FOV: vcpe_anim_fov = v[0]; break;
        case VCPE_TR_TRANSPARENCY: n->transparency = (uint8_t)(v[0] < 0 ? 0 : v[0] > 1 ? 255 : v[0] * 255); break;
        }
    }
}

static int started[256];   /* a clip that has just been started fires its keys at time 0 too */

int vcpe_anim_find(uint32_t h)
{
    for (int i = 0; S && i < S->nplayers; i++)
        if (S->players[i].name == h)
            return i;
    return -1;
}

void vcpe_anim_play(int p, uint32_t clip)
{
    if (p < 0)
        return;
    const VcpePlayerDef *pd = &S->players[p];
    for (int c = pd->first; c < pd->first + pd->n; c++) {
        if (S->clips[c].name == clip) {
            players[p].clip = c;
            players[p].t = 0;
            players[p].playing = 1;
            if (p < 256)
                started[p] = 1;
            return;
        }
    }
}

void vcpe_anim_stop(int p) { if (p >= 0) players[p].playing = 0; }
int vcpe_anim_playing(int p) { return p >= 0 && players[p].playing; }
float vcpe_anim_time(int p) { return p >= 0 ? players[p].t : 0; }
uint32_t vcpe_anim_clip(int p) { return p >= 0 && players[p].clip >= 0 ? S->clips[players[p].clip].name : 0; }
float vcpe_anim_length(int p) { return p >= 0 && players[p].clip >= 0 ? S->clips[players[p].clip].length : 0; }

void vcpe_node3d_update(float dt)
{
    for (int p = 0; p < S->nplayers; p++) {
        Player *pl = &players[p];
        if (!pl->playing || pl->clip < 0)
            continue;
        const VcpeClipDef *cl = &S->clips[pl->clip];
        int first = p < 256 && started[p];
        if (p < 256)
            started[p] = 0;
        float t0 = pl->t, t1 = first ? 0 : t0 + dt;
        if (t1 >= cl->length) {
            if (cl->loop && cl->length > 0) {
                apply(p, cl, t0, cl->length, first);
                t1 = fmodf(t1, cl->length);
                t0 = 0;
                first = 1;
            } else {
                t1 = cl->length;
                pl->playing = 0;
            }
        }
        apply(p, cl, t0, t1, first);
        pl->t = t1;
    }
    update_world();
}
