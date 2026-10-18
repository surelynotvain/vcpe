/* VCPE collide: static collision for 3D worlds and a character controller. Triangle meshes (houses, cave walls,
 * rocks: placed many times with their own transform), vertical cylinders (trees, pillars, posts) and the ground
 * (vcpe_terrain through a callback), with raycasts (line of sight, arrows, picking, camera collision) and a capsule
 * that walks: slides along walls, climbs steps up to step_up, stays on slopes up to a limit and slides off steeper
 * ones, snaps down stairs and slopes instead of bouncing, and lands from jumps and falls.
 *
 *   VcpeColMesh house;  vcpe_colmesh_build(&house, &verts[0].x, sizeof verts[0], idx, ntris, 2.0f, mem, cap);
 *   VcpeColWorld w;  vcpe_col_world(&w, inst, 256, trees, 512);
 *   vcpe_col_ground(&w, terrain_height, 0, cells);                  // e.g. vcpe_terrain_height_any behind a wrapper
 *   vcpe_col_add(&w, &house, house_matrix, HOUSE_ID);
 *   VcpeKcc me;  vcpe_kcc_init(&me, 0.3f, 1.8f);  me.pos = spawn;
 *   each tick:  vcpe_kcc_step(&w, &me, wish_velocity, jump_pressed, 4.5f, dt);
 *
 * Space: whatever the game uses consistently (world, or world minus the vcpe_cells origin); Y up. Meshes are
 * borrowed (vertex positions as floats every `stride` bytes, u16 triangle indices: a VcpeVtx array works directly).
 * Plain C, no PSP headers, no allocation. Queries are single-threaded (meshes keep a query stamp). */
#ifndef VCPE_COLLIDE_H
#define VCPE_COLLIDE_H
#include <stdint.h>

typedef struct {
    const float *pos;              /* x of vertex 0; y, z follow */
    int stride;                    /* bytes from one vertex to the next */
    const uint16_t *idx;
    int ntris;
    float (*plane)[4];             /* per triangle: unit normal and n . p */
    uint32_t *stamp;               /* per triangle: last query that tested it */
    uint32_t *start, *refs;        /* grid cell c holds refs[start[c] .. start[c + 1]) */
    int gw, gd;                    /* grid cells across x and z */
    float gx0, gz0, cell;
    float lo[3], hi[3];            /* bounds, mesh space */
    uint32_t query;
} VcpeColMesh;

/* Bytes vcpe_colmesh_build needs (grid cell size `cell` metres, enlarged to keep at most 64 x 64 cells). 0 bad input */
uint32_t vcpe_colmesh_bytes(const float *pos, int stride, const uint16_t *idx, int ntris, float cell);
/* mem 4-byte aligned. Degenerate triangles are kept out. 0 ok, -1 bad input / too small */
int vcpe_colmesh_build(VcpeColMesh *m, const float *pos, int stride, const uint16_t *idx, int ntris, float cell,
                       void *mem, uint32_t cap);

typedef struct {
    VcpeColMesh *mesh;
    float m[12], inv[12];          /* mesh to world and back: 3x4 row-major, rotation x uniform scale + translation */
    float scale;
    float lo[3], hi[3];            /* world bounds */
    uint32_t user;
    uint8_t used;
} VcpeColInstance;
typedef struct { float x, z, y0, y1, r; uint32_t user; uint8_t used; } VcpeColCylinder;

typedef int (*VcpeColGround)(void *user, float x, float z, float *y, float normal[3]);   /* 0: no ground there */
typedef int (*VcpeColGroundRay)(void *user, const float o[3], const float d[3], float max, float *t);

typedef struct {
    VcpeColInstance *inst;
    int cap_inst, ninst;
    VcpeColCylinder *cyl;
    int cap_cyl, ncyl;
    VcpeColGround ground;
    VcpeColGroundRay ground_ray;   /* optional: without it rays march the ground() heights */
    void *ground_user;
} VcpeColWorld;

void vcpe_col_world(VcpeColWorld *w, VcpeColInstance *inst, int cap_inst, VcpeColCylinder *cyl, int cap_cyl);
void vcpe_col_ground(VcpeColWorld *w, VcpeColGround ground, VcpeColGroundRay ray, void *user);
/* m: mesh to world, 3x4 row-major with uniform scale (0: identity). Returns the instance id, -1 when full/bad */
int vcpe_col_add(VcpeColWorld *w, VcpeColMesh *mesh, const float *m, uint32_t user);
void vcpe_col_move(VcpeColWorld *w, int id, const float *m);      /* doors, lifts (no velocity is imparted) */
void vcpe_col_remove(VcpeColWorld *w, int id);
int vcpe_col_add_cylinder(VcpeColWorld *w, float x, float z, float y0, float y1, float r, uint32_t user);
void vcpe_col_remove_cylinder(VcpeColWorld *w, int id);

enum { VCPE_COL_NONE, VCPE_COL_GROUND, VCPE_COL_MESH, VCPE_COL_CYLINDER };
typedef struct { float t, point[3], normal[3]; int kind, id, tri; uint32_t user; } VcpeColHit;
/* First hit from o along d (normalised here, so t is a distance) up to max: ground, meshes and cylinders */
int vcpe_col_ray(VcpeColWorld *w, const float o[3], const float d[3], float max, VcpeColHit *hit);

/* Character: a capsule floating step_up above the feet (so steps and pebbles never stop it; the ground probe lifts
 * the feet onto them), from the feet up to height. */
typedef struct {
    float pos[3];                  /* feet */
    float vel[3];                  /* m/s, kept by vcpe_kcc_step */
    float radius, height, step_up, step_down, slope_cos, gravity;
    int grounded, ground_kind, ground_id;
    float ground_normal[3];
    int contacts;                  /* wall/ceiling contacts in the last move */
    float wall_normal[3];          /* the last one */
} VcpeKcc;
/* step_up 0.35, step_down 0.45, slope 46 degrees, gravity 9.81 m/s2 */
void vcpe_kcc_init(VcpeKcc *k, float radius, float height);
/* Kinematic move by delta (substeps of half the radius, so it never tunnels). Returns contacts. */
int vcpe_kcc_move(VcpeColWorld *w, VcpeKcc *k, const float delta[3]);
/* Walking: horizontal wish velocity (m/s; y ignored), gravity, a jump when grounded, sliding off steep ground. */
void vcpe_kcc_step(VcpeColWorld *w, VcpeKcc *k, const float wish[3], int jump, float jump_speed, float dt);

#endif
