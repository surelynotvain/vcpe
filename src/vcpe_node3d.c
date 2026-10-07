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
    int16_t mesh;
    uint8_t visible, transparency;
    uint32_t layers;
    float world[16];
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
        nodes[i].transparency = d->transparency;
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
    /* the converter writes parents before their children */
    for (int i = 0; i < S->nnodes; i++) {
        float l[16];
        local_matrix(&nodes[i], l);
        int p = S->nodes[i].parent;
        if (p >= 0)
            mat_mul(nodes[p].world, l, nodes[i].world);
        else if (S->nodes[i].base >= 0)
            mat_mul(S->bases[S->nodes[i].base], l, nodes[i].world);
        else
            memcpy(nodes[i].world, l, sizeof l);
    }
}

static int shown(int i)
{
    for (; i >= 0; i = S->nodes[i].parent)
        if (!nodes[i].visible)
            return 0;
    return 1;
}

int vcpe_node3d_shown(int n) { return n >= 0 && shown(n); }
uint32_t vcpe_node3d_layers(int n) { return n >= 0 ? nodes[n].layers : 0; }
float vcpe_node3d_opacity(int n) { return n >= 0 ? 1.0f - nodes[n].transparency / 255.0f : 0; }

void vcpe_node3d_draw(uint32_t cull)
{
    for (int i = 0; i < S->nnodes; i++) {
        int m = nodes[i].mesh;
        /* fully transparent (GeometryInstance3D.transparency 1): not drawn; partial transparency comes later */
        if (m < 0 || !(nodes[i].layers & cull) || nodes[i].transparency >= 250 || !shown(i))
            continue;
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
