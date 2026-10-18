/* VCPE rigs: skeletons and compressed animation clips for skinned characters (tools/vcpe_rig.py writes them from
 * glTF). Clips store each bone's rotation as a 6-byte quaternion and its translation as 6 bytes inside the track's
 * own bounds; bones that never move in a clip store one key or nothing. Sampling gives local bone transforms that
 * can be blended (crossfades, blend spaces, masked layers, additive layers), bent by IK and turned into the
 * delta-from-bind matrices vcpe_skin_apply takes:
 *
 *   VcpeRig rig;  vcpe_rig_open(&rig, blob, size);              // blob: VRG1 from the PAK or a pack, kept by the game
 *   vcpe_rig_sample(&rig, walk, t, 1, pose, 0);                 // local transforms of every bone at t seconds
 *   vcpe_rig_sample(&rig, run, t, 1, other, 0);
 *   vcpe_rig_blend(&rig, pose, other, 0.3f, 0);                 // 30 % of the way to the run
 *   vcpe_rig_model(&rig, pose, model);                          // character-space bone matrices
 *   vcpe_rig_skin(&rig, model, skin);                           // model x inverse bind
 *   vcpe_skin_apply(out, bind, nverts, skin, rig.nbones);       // vcpe_skin.h
 *
 * vcpe_animator (vcpe_animator.h) drives all of this per character; these functions stay public for tools and
 * special cases. Plain C without PSP headers: everything here runs in host tests. Nothing is shared between calls,
 * so different threads may work on different characters.
 *
 * Conventions: Y up, forward -Z (the vcpe_3d camera and model convention); matrices are 3x4 row-major affine
 * (rotation-scale columns, then translation), the vcpe_skin layout. Scale is uniform per bone. Parents come
 * before their children in the bone table. Quaternions are x, y, z, w. */
#ifndef VCPE_RIG_H
#define VCPE_RIG_H
#include <stdint.h>

#define VCPE_RIG_MAX_BONES 192     /* the vcpe_skin limit */

typedef struct { float r[4], t[3], s; } VcpeXform;                      /* a bone relative to its parent */

/* blob layout (little endian; every table 16-byte aligned; tools/vcpe_rig.py documents the byte format) */
typedef struct {
    uint32_t name;                 /* FNV-1a of the bone name (vcpe_hash) */
    int16_t parent;                /* -1: a root */
    uint16_t flags;
    float t[3], r[4], s;           /* bind pose, local */
    float inv_bind[12];            /* inverse of the bind pose in character space */
} VcpeRigBone;                     /* 88 bytes */
typedef struct { float t; uint32_t id; int32_t arg; } VcpeRigEvent;    /* id: FNV-1a of the event name */
typedef struct {
    uint32_t name;
    uint16_t frames, flags;        /* VCPE_RIG_* */
    float duration;                /* seconds from the first to the last frame */
    uint32_t tracks;               /* blob offset of nbones VcpeRigTrack */
    uint32_t events;               /* blob offset of nevents VcpeRigEvent, sorted by time */
    uint16_t nevents, pad;
    uint32_t reserved[2];
} VcpeRigClip;                     /* 32 bytes */
typedef struct { uint8_t rot, pos, scl, pad; uint32_t off; } VcpeRigTrack;
enum { VCPE_TRACK_BIND, VCPE_TRACK_CONST, VCPE_TRACK_KEYS };
enum {
    VCPE_RIG_LOOP = 1,             /* authored as a cycle; vcpe_animator plays it looped unless told otherwise */
    VCPE_RIG_MOTION_XZ = 2,        /* root motion: the motion bone's ground travel is taken out of the pose */
    VCPE_RIG_MOTION_YAW = 4,       /* root motion: the motion bone's turn about Y is taken out of the pose */
    VCPE_RIG_ADDITIVE = 8          /* meant as an additive layer (relative to its first frame) */
};

typedef struct {
    const uint8_t *base;
    uint32_t size;
    const VcpeRigBone *bones;
    const VcpeRigClip *clips;
    int nbones, nclips;
    int motion_bone;               /* root-motion bone (a root, or under unanimated parents) */
} VcpeRig;

/* blob: 4-byte aligned, kept in memory while the rig is used. 0 ok, -1 not a VRG1 blob, -2 damaged/out of bounds */
int vcpe_rig_open(VcpeRig *rig, const void *blob, uint32_t size);
int vcpe_rig_bone(const VcpeRig *rig, uint32_t name_hash);              /* bone index, -1 */
int vcpe_rig_bone_named(const VcpeRig *rig, const char *name);
int vcpe_rig_clip(const VcpeRig *rig, uint32_t name_hash);              /* clip index, -1 */
int vcpe_rig_clip_named(const VcpeRig *rig, const char *name);
float vcpe_rig_length(const VcpeRig *rig, int clip);                    /* seconds, 0 for a bad index */
int vcpe_rig_flags(const VcpeRig *rig, int clip);
uint32_t vcpe_rig_hash(const char *s);                                  /* FNV-1a, = vcpe_hash */

/* Masks: one weight 0..255 per bone. vcpe_rig_mask sets `bone` and everything below it (e.g. the spine for an
 * upper-body layer); start from a zeroed array. */
void vcpe_rig_mask(const VcpeRig *rig, uint8_t *mask, int bone, uint8_t weight);

/* Local transforms. mask (optional): only bones with a non-zero weight are written, the rest stay as they are.
 * loop: wrap t into the clip, else clamp. Root-motion clips leave the motion bone's travel/turn out of the pose.
 * 0 ok, -1 bad clip. */
void vcpe_rig_bind_pose(const VcpeRig *rig, VcpeXform *out);
int vcpe_rig_sample(const VcpeRig *rig, int clip, float t, int loop, VcpeXform *out, const uint8_t *mask);
/* The change from the clip's first frame to t, for vcpe_rig_add (breathing, flinches, leaning on a slope) */
int vcpe_rig_sample_delta(const VcpeRig *rig, int clip, float t, int loop, VcpeXform *out, const uint8_t *mask);

/* dst moves toward src by w (x mask/255 per bone): normalised quaternion lerp on the short arc, linear translation
 * and scale. Blending several poses: blend each next one with w_i / (w_0 + .. + w_i) for a weighted average. */
void vcpe_rig_blend(const VcpeRig *rig, VcpeXform *dst, const VcpeXform *src, float w, const uint8_t *mask);
/* dst gets w (x mask/255) of a delta from vcpe_rig_sample_delta on top */
void vcpe_rig_add(const VcpeRig *rig, VcpeXform *dst, const VcpeXform *delta, float w, const uint8_t *mask);

/* Character-space bone matrices (parent x local), then skinning matrices (model x inverse bind) for vcpe_skin_apply.
 * Keep `skin` 16-byte aligned for the VFPU path of vcpe_skin. */
void vcpe_rig_model(const VcpeRig *rig, const VcpeXform *local, float (*model)[12]);
void vcpe_rig_skin(const VcpeRig *rig, const float (*model)[12], float (*skin)[12]);
/* One bone's character-space transform without computing the others (walks its parents) */
void vcpe_rig_model_xform(const VcpeRig *rig, const VcpeXform *local, int bone, VcpeXform *out);
/* Row-major 4x4 for vcpe_3d_model: object (a sword, a helmet) held by a bone. world: the character's model matrix;
 * offset: the object relative to the bone (row-major 4x4, 0 = none). */
void vcpe_rig_attach(const float world[16], const float model[12], const float *offset, float out[16]);

/* Root motion between two unwrapped clip times (from <= to, seconds): ground travel of the motion bone expressed in
 * the character's frame at `from` (dpos[1] stays 0) and the turn in radians (positive: counter-clockwise seen from
 * above, the vcpe_3d yaw direction). Loops count whole cycles. Apply as pos += Ry(yaw) * dpos; yaw += dyaw. */
void vcpe_rig_motion(const VcpeRig *rig, int clip, float from, float to, int loop, float dpos[3], float *dyaw);

/* Events with from < time <= to (unwrapped seconds; loops repeat them every cycle, at most 8 cycles per call).
 * Returns how many fired. */
typedef void (*VcpeRigEventFn)(void *user, int clip, const VcpeRigEvent *e);
int vcpe_rig_events(const VcpeRig *rig, int clip, float from, float to, int loop, VcpeRigEventFn fn, void *user);

/* Two-bone IK (thigh-calf-foot, upper arm-forearm-hand): rotate a and b so c reaches target (character space), the
 * knee/elbow bending toward pole. keep_end: c keeps its character-space rotation (planted feet, gripping hands).
 * weight 0..1. Returns 1 reached, 0 out of reach (the chain straightens toward it), -1 when b's parent is not a or
 * c's parent is not b. Call vcpe_rig_model afterwards. */
int vcpe_rig_ik2(const VcpeRig *rig, VcpeXform *local, int a, int b, int c, const float target[3],
                 const float pole[3], float weight, int keep_end);

/* Skinned meshes (VSM1 from tools/vcpe_rig.py): a colour/UV template, bind vertices with up to four bone weights
 * (bone indices of the rig they were converted against), and per material a submesh, optionally with coarser levels
 * whose vertices are a prefix of the array (skin and draw only those far away).
 *
 *   VcpeSkinMesh m;  vcpe_skinmesh_open(&m, blob, size);
 *   vcpe_skinmesh_init(&m, out, 0xffffffff);                       // once per output buffer: UVs, colours
 *   lvl = vcpe_skinmesh_lod(&m, &view, centre, 1, lvl);              // view: vcpe_3d_lod_view
 *   vcpe_skinmesh_skin(&m, lvl, skin, rig.nbones, out);             // positions only
 *   vcpe_3d_model(world); vcpe_3d_draw_skinned(&m, lvl, out);
 *
 * Pipelined frames (vcpe_gfx_pipeline): the GE may still read last frame's vertices, so alternate two output
 * buffers per character. */
#include "vcpe_skin.h"
#include "vcpe_lod.h"
#define VCPE_SKINMESH_LOD 4
typedef struct { int16_t page; uint16_t pad; uint32_t ioff[VCPE_SKINMESH_LOD], nidx[VCPE_SKINMESH_LOD]; } VcpeSkinSub;
typedef struct { uint16_t nverts, pad; float err; } VcpeSkinLevel;
typedef struct VcpeSkinMesh {
    const uint8_t *base;
    const VcpeVtx *verts;          /* template: u, v, colour (positions are the bind pose) */
    const VcpeSkinVertex *bind;
    const VcpeSkinSub *subs;
    const VcpeSkinLevel *levels;
    int nverts, nsubs, nlevels, nbones;   /* nbones: highest bone index used + 1 */
    float centre[3], radius;              /* bind-pose bounding sphere, character space */
} VcpeSkinMesh;
/* blob 16-byte aligned (the GE reads indices from it). 0 ok, -1 not VSM1, -2 damaged */
int vcpe_skinmesh_open(VcpeSkinMesh *m, const void *blob, uint32_t size);
/* copy the template into an output buffer, colours multiplied by tint (ABGR; 0xffffffff keeps them) */
void vcpe_skinmesh_init(const VcpeSkinMesh *m, VcpeVtx *out, uint32_t tint);
/* positions of the vertices level uses; -1 when the rig has fewer bones than the mesh needs */
int vcpe_skinmesh_skin(const VcpeSkinMesh *m, int level, const float (*skin)[12], int bones, VcpeVtx *out);
int vcpe_skinmesh_lod(const VcpeSkinMesh *m, const VcpeLodView *v, const float centre[3], float scale, int prev);
static inline const uint16_t *vcpe_skinmesh_idx(const VcpeSkinMesh *m, int sub, int level)
{ return (const uint16_t *)(m->base + m->subs[sub].ioff[level]); }

/* quaternion helpers used by the animator and IK (x y z w) */
void vcpe_quat_mul(float out[4], const float a[4], const float b[4]);
void vcpe_quat_rotate(const float q[4], const float v[3], float out[3]);
void vcpe_quat_between(float out[4], const float from[3], const float to[3]);   /* shortest arc, unit vectors */
void vcpe_quat_nlerp(float out[4], const float a[4], const float b[4], float t);
float vcpe_quat_yaw(const float q[4]);                                           /* twist about +Y, radians */

#endif
