/* VCPE animator: what a character plays, built on vcpe_rig. Layers of motions with crossfades, phase-synced
 * locomotion blend spaces, masked upper-body layers, additive layers, queued one-shots, clip events, root motion and
 * foot IK on uneven ground. update() and evaluate() are separate on purpose:
 *
 *   update(dt)     every simulation tick, for every character: clocks, fades, events, root motion. Cheap, and what
 *                  gameplay depends on (a hit lands on its event frame even for a character nobody is looking at).
 *   evaluate(n)    only when the pose will be seen: samples and blends n layers into local/model/skin matrices.
 *                  Skip it off screen, run it every 2nd or 4th frame far away (vcpe_crowd decides), or blend only
 *                  the base layer at a distance. The last pose stays valid in between.
 *
 *   static uint8_t mem[...] __attribute__((aligned(16)));          // vcpe_animator_bytes(&rig)
 *   VcpeAnimator a;  vcpe_animator_init(&a, &rig, mem);
 *   static const VcpeBlendSpace move = {{idle, walk, run}, {0, 1.6f, 5.5f}, 3};   // clips along a speed axis
 *   vcpe_animator_play_space(&a, 0, &move, speed, 0.25f, 1);         // base layer
 *   vcpe_animator_layer(&a, 1, upper_mask, 0);                       // vcpe_rig_mask from the spine
 *   vcpe_animator_play(&a, 1, swing, 0.1f, 1, 0);                    // one-shot: the layer fades out after it
 *   each tick:  vcpe_animator_param(&a, 0, speed); vcpe_animator_update(&a, dt);
 *               vcpe_animator_take_motion(&a, dp, &dyaw);            // root-motion clips move the character
 *   when drawn: vcpe_animator_evaluate(&a, VCPE_ANIM_LAYERS); vcpe_animator_feet(&a, &feet, world, ground, 0, dt);
 *               vcpe_skinmesh_skin(&mesh, lvl, (const float (*)[12])a.skin, rig.nbones, out);
 *
 * Plain C, no PSP headers. Evaluation uses static scratch poses: run all animators on one thread. */
#ifndef VCPE_ANIMATOR_H
#define VCPE_ANIMATOR_H
#include <stdint.h>
#include "vcpe_rig.h"

#define VCPE_ANIM_LAYERS 4
#define VCPE_ANIM_STACK 4          /* motions crossfading in one layer at once; a fifth freezes the blend so far */
#define VCPE_ANIM_SPACE 8          /* clips in a blend space */

/* 1D blend space: clips placed along a parameter, at[] ascending (speed in m/s, lean, aim pitch...). Cycles are
 * phase-synced: a walk of 1.1 s and a run of 0.7 s blend at the same point of the stride. Loop clips only. */
typedef struct { int16_t clip[VCPE_ANIM_SPACE]; float at[VCPE_ANIM_SPACE]; int count; } VcpeBlendSpace;

typedef struct {
    int16_t clip;                  /* clip index; -1 bind pose or blend space; -2 the frozen pose */
    uint8_t loop, fresh;
    const VcpeBlendSpace *space;
    float time;                    /* clip: seconds; blend space: phase in cycles (both unwrapped) */
    float speed, weight, fade, param;
} VcpeMotion;

typedef struct {
    VcpeMotion stack[VCPE_ANIM_STACK];   /* oldest first; the last one is the current motion */
    int count;
    const uint8_t *mask;           /* per-bone weights (vcpe_rig_mask); 0: every bone */
    uint8_t additive, has_queue, queue_loop, auto_out;
    float weight, target, rate;    /* layer weight moving toward target at rate per second */
    int16_t queue_clip;
    float queue_fade, queue_speed;
} VcpeAnimLayer;

typedef void (*VcpeAnimEventFn)(void *user, int layer, int clip, uint32_t id, int32_t arg);

typedef struct {
    const VcpeRig *rig;
    float (*skin)[12];             /* skin matrices for vcpe_skin_apply / vcpe_skinmesh_skin (16-byte aligned) */
    float (*model)[12];            /* character-space bone matrices (attachments: vcpe_rig_attach) */
    VcpeXform *local;              /* the evaluated pose, before IK */
    VcpeXform *frozen;             /* layer 0 snapshot when more crossfades pile up than the stack holds */
    VcpeAnimLayer layer[VCPE_ANIM_LAYERS];
    VcpeAnimEventFn on_event;
    void *user;
    float motion[3], motion_yaw;   /* root motion gathered since vcpe_animator_take_motion */
    uint32_t evaluations;          /* evaluate calls, for LOD statistics */
} VcpeAnimator;

uint32_t vcpe_animator_bytes(const VcpeRig *rig);                       /* 160 bytes per bone */
/* mem: vcpe_animator_bytes(rig) bytes, 16-byte aligned, kept by the game. Starts in the bind pose. -1 bad input */
int vcpe_animator_init(VcpeAnimator *a, const VcpeRig *rig, void *mem);

/* Start a clip on a layer, crossfading from what plays there over fade seconds. loop: 1, 0, or -1 for the clip's own
 * flag. On layers above 0 a one-shot without a queued follow-up fades the layer out when it ends. -1 bad clip/layer */
int vcpe_animator_play(VcpeAnimator *a, int layer, int clip, float fade, float speed, int loop);
int vcpe_animator_play_space(VcpeAnimator *a, int layer, const VcpeBlendSpace *s, float param, float fade, float speed);
/* play this when the current one-shot ends, crossfading so the fade completes at its last frame (attack -> idle) */
void vcpe_animator_queue(VcpeAnimator *a, int layer, int clip, float fade, float speed, int loop);
void vcpe_animator_param(VcpeAnimator *a, int layer, float param);     /* the current blend space's parameter */
void vcpe_animator_speed(VcpeAnimator *a, int layer, float speed);     /* >= 0 */
/* mask (kept by the game) and mode of a layer; layer 0 is always full-body override */
void vcpe_animator_layer(VcpeAnimator *a, int layer, const uint8_t *mask, int additive);
void vcpe_animator_layer_weight(VcpeAnimator *a, int layer, float weight, float fade);
void vcpe_animator_stop(VcpeAnimator *a, int layer, float fade);       /* layer 0: crossfade back to the bind pose */
void vcpe_animator_events(VcpeAnimator *a, VcpeAnimEventFn fn, void *user);

void vcpe_animator_update(VcpeAnimator *a, float dt);
void vcpe_animator_evaluate(VcpeAnimator *a, int layers);
/* root motion since the last call, in the character's frame (pos += Ry(yaw) * dpos; yaw += dyaw); then cleared */
void vcpe_animator_take_motion(VcpeAnimator *a, float dpos[3], float *dyaw);

int vcpe_animator_current(const VcpeAnimator *a, int layer);    /* clip of the current motion (-1: none / space) */
float vcpe_animator_time(const VcpeAnimator *a, int layer);     /* seconds into it; blend space: phase 0..1 */
int vcpe_animator_finished(const VcpeAnimator *a, int layer);   /* the current one-shot reached its end */
float vcpe_animator_layer_weight_now(const VcpeAnimator *a, int layer);

/* Foot IK: legs follow the ground instead of the flat plane the clips were made on. After evaluate. The pelvis drops
 * so the lower foot can reach, each foot rises or falls by the ground under it (smoothed) and optionally tilts to its
 * normal. world: the character's model matrix (yaw, uniform scale, origin at the feet). ground: height and normal at
 * a world x, z (return 0 where there is none: that foot is left alone). Recomputes model and skin. Returns feet
 * adjusted (0 when called again before the next evaluate). Zero the struct and fill in the bones to start. */
typedef int (*VcpeGroundFn)(void *user, float x, float z, float *height, float normal[3]);
typedef struct {
    int thigh[2], calf[2], foot[2];    /* two hip-knee-ankle chains (the calf's parent is the thigh, and so on) */
    int pelvis;                        /* moved down with the lower foot; -1: none */
    float max_drop, max_raise;         /* metres (world) the pelvis may drop and a foot may rise */
    float align;                       /* 0..1 of the ground tilt given to the feet (capped at 35 degrees) */
    float smooth;                      /* 1/s; 0: follow instantly */
    float offset[2], pelvis_offset;    /* state */
    uint32_t applied;                  /* state: the evaluation it was applied to (+1) */
} VcpeFootIK;
int vcpe_animator_feet(VcpeAnimator *a, VcpeFootIK *ik, const float world[16], VcpeGroundFn ground, void *user, float dt);

#endif
