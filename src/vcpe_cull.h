/* CPU camera culling for indexed VcpeVtx triangle lists. No PSP/GPU dependencies; main thread only.
 * Bounds come from the referenced vertices, never an object's origin. Cached data is immutable until dirty().
 * Keep geometry alive until forget/reset. Mutable vertices/indices MUST be marked dirty before the next draw.
 * Failure, invalid coordinates/camera/model, and exhausted cache budgets all fail open (keep drawing). */
#ifndef VCPE_CULL_H
#define VCPE_CULL_H
#include <stdint.h>
#ifdef VCPE_CULL_WEAK
#define VCPE_CULL_API __attribute__((weak))
#else
#define VCPE_CULL_API
#endif
typedef struct { float center[3], extent[3]; int valid; } VcpeCullBox;
typedef struct { float p[6][4]; int valid; } VcpeCullPlanes;
/* Prepared planes are immutable snapshots; rebuild after camera/model changes. */
typedef struct { VcpeCullPlanes planes; float normal_length[6]; int valid; } VcpeCullPrepared;
typedef struct { uint32_t first, count; VcpeCullBox box; } VcpeCullChunk;
typedef struct VcpeCullMesh VcpeCullMesh;
typedef struct { uint32_t bytes, peak_bytes, entries, fallbacks, refits; } VcpeCullCacheStats;

/* All axes world-space, forward points into the view; tan_h/tan_v are half-FOV tangents.
 * Planes are inward-facing. local_planes transforms them by the affine model transpose, preserving shear. */
void vcpe_cull_camera(VcpeCullPlanes *out, const float pos[3], const float right[3], const float up[3],
                      const float forward[3], float tan_h, float tan_v, float near_z, float far_z) VCPE_CULL_API;
void vcpe_cull_local_planes(VcpeCullPlanes *out, const VcpeCullPlanes *view, const float *model16) VCPE_CULL_API;
/* 0 outside, 1 intersects/inconclusive, 2 entirely inside. Touching a plane is retained, with roundoff margin. */
int vcpe_cull_box_test(const VcpeCullPlanes *planes, const VcpeCullBox *box) VCPE_CULL_API;
int vcpe_cull_sphere_test(const VcpeCullPlanes *planes, const float center[3], float radius) VCPE_CULL_API;
void vcpe_cull_prepare_planes(VcpeCullPrepared *out,const VcpeCullPlanes *planes) VCPE_CULL_API;
int vcpe_cull_sphere_prepared(const VcpeCullPrepared *planes,const float center[3],float radius) VCPE_CULL_API;
void vcpe_cull_box_points(VcpeCullBox *out, const void *verts, uint32_t count) VCPE_CULL_API;

/* Defaults: 512 KiB heap cap, 128 triangles per contiguous chunk. Chunk size rounds up to a multiple of 8
 * (8..65536) so GU index starts retain 16-byte alignment. limits() clears the old cache.
 * No triangles are removed/reordered in the source. Rejected contiguous ranges simply aren't submitted. */
void vcpe_cull_limits(uint32_t bytes, uint32_t chunk_triangles) VCPE_CULL_API;
int vcpe_cull_prepare(const void *verts, uint32_t nverts, const uint16_t *indices, uint32_t nidx) VCPE_CULL_API;
const VcpeCullMesh *vcpe_cull_find(const void *verts, const uint16_t *indices, uint32_t nidx) VCPE_CULL_API;
const VcpeCullBox *vcpe_cull_bound(const VcpeCullMesh *mesh) VCPE_CULL_API;
const VcpeCullChunk *vcpe_cull_chunks(const VcpeCullMesh *mesh, uint32_t *count) VCPE_CULL_API;
void vcpe_cull_dirty(const void *verts) VCPE_CULL_API; /* all LOD index lists sharing this vertex base */
void vcpe_cull_forget(const void *verts) VCPE_CULL_API;
void vcpe_cull_reset(void) VCPE_CULL_API;
const VcpeCullCacheStats *vcpe_cull_cache_stats(void) VCPE_CULL_API;
#undef VCPE_CULL_API
#endif
