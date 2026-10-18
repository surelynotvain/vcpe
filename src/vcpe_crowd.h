/* VCPE crowd: level of detail for many characters (a town, a battle, wildlife). Each frame it sorts every agent into
 * a tier by how big it looks and whether the camera sees it, caps how many get the expensive tiers (the biggest on
 * screen win), and spreads the work of the cheaper tiers over frames with staggered phases so no frame pays for
 * everyone at once:
 *
 *   tier      when                         pose (vcpe_animator_evaluate)   layers   mesh LOD   AI
 *   FULL      >= full_px tall (max_full)   every frame                     all      0          every frame
 *   REDUCED   >= reduced_px (max_reduced)  every reduced_every frames      2        1          every frame
 *   FAR       smaller                      every far_every frames          1        2          every ai_far_every
 *   HIDDEN    off screen                   never (fresh pose on return)    0        -          every ai_far_every
 *   DORMANT   beyond dormant_dist          never; clocks every ai_dormant_every frames (with the dt that piled up)
 *
 *   VcpeCrowd crowd;  VcpeCrowdConfig cfg;  vcpe_crowd_defaults(&cfg);  vcpe_crowd_init(&crowd, &cfg);
 *   each frame:  VcpeCrowdView v = {{cx, cy, cz}, px_k, visible_fn, 0};   // px_k: VcpeLodView.px_k
 *                vcpe_crowd_update(&crowd, agents, n, &v, dt);
 *                for each agent a:  if (a->tick_anim) vcpe_animator_update(&anim[i], a->anim_dt);
 *                                   if (a->tick_ai) think(i, a->ai_dt);
 *                                   if (a->evaluate) { vcpe_animator_evaluate(&anim[i], a->layers); skin; }
 *                                   if (a->visible) draw at a->mesh_lod
 *
 * Important agents (the player's follower, an enemy in combat, a quest giver talking) never go below REDUCED while
 * visible and never sleep. With vcpe_adapt linked and enabled, its LOD factor shrinks every agent's apparent size
 * under load, so tiers coarsen automatically and recover when frames are fast again.
 * Plain C, no PSP headers; a visibility callback (vcpe_3d_visible) keeps it independent of the renderer. */
#ifndef VCPE_CROWD_H
#define VCPE_CROWD_H
#include <stdint.h>

#define VCPE_CROWD_MAX 512
enum { VCPE_TIER_FULL, VCPE_TIER_REDUCED, VCPE_TIER_FAR, VCPE_TIER_HIDDEN, VCPE_TIER_DORMANT, VCPE_TIER_COUNT };

typedef struct {
    float pos[3], radius;          /* bounding sphere, in the camera's space; set by the game */
    uint8_t important;             /* set by the game */
    /* results */
    uint8_t tier, visible, evaluate, layers, mesh_lod, tick_anim, tick_ai;
    float px;                      /* apparent height in pixels */
    float anim_dt, ai_dt;          /* time to pass to the animator / AI when they tick */
    uint32_t phase;                /* stagger, from the agent's index */
    float anim_acc, ai_acc;        /* time piled up since they last ticked */
    uint8_t seen;                  /* had a tier before */
} VcpeCrowdAgent;

typedef struct {
    float full_px, reduced_px;           /* apparent heights (pixels) for FULL and REDUCED; FAR below */
    int max_full, max_reduced;
    int reduced_every, far_every;        /* pose every n frames */
    int ai_far_every, ai_dormant_every;  /* AI (and, dormant, animation clocks) every n frames */
    float dormant_dist;                  /* metres; 0: never dormant */
    float hysteresis;                    /* fraction past a threshold before a tier changes back */
} VcpeCrowdConfig;

typedef int (*VcpeCrowdVisible)(void *user, const float center[3], float radius);
typedef struct { float pos[3]; float px_k; VcpeCrowdVisible visible; void *user; } VcpeCrowdView;

typedef struct {
    VcpeCrowdConfig cfg;
    uint32_t frame;
    int count[VCPE_TIER_COUNT];
    int evaluations, ai_ticks;
} VcpeCrowd;

/* 120 / 50 px; 6 full, 16 reduced; poses every 2 / 4 frames; AI every 4 / 30 frames; dormant beyond 150 m; 15 % */
void vcpe_crowd_defaults(VcpeCrowdConfig *c);
void vcpe_crowd_init(VcpeCrowd *c, const VcpeCrowdConfig *cfg);
/* n <= VCPE_CROWD_MAX agents. Returns poses to evaluate this frame. */
int vcpe_crowd_update(VcpeCrowd *c, VcpeCrowdAgent *agents, int n, const VcpeCrowdView *view, float dt);
int vcpe_crowd_line(const VcpeCrowd *c, char *out, int n);   /* "crowd: 6 full, 9 reduced, ... 11 poses, 30 AI" */

#endif
