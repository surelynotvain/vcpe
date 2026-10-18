/* VCPE cells: open-world streaming. The world is a grid of cells (terrain, objects, NPCs per cell); the cells near
 * the player are active, a ring around them is loaded but asleep, everything else is on the memory stick. Each
 * frame vcpe_cells_update decides what to load next (nearest to where the player will be, not where they are),
 * what to wake, what to put to sleep and what to drop, with hysteresis so walking back and forth over a border never
 * thrashes. It also keeps a floating origin: rendering subtracts it, so coordinates on the GE stay small however
 * far the player walks.
 *
 *   static const VcpeCellDef defs[] = {{0, 0, PAK_C0_0_OFF, PAK_C0_0_SIZE, 0}, ...};   // converter output
 *   VcpeCellsConfig cfg;  vcpe_cells_defaults(&cfg, defs, ndefs, 64.0f);              // 64 m cells
 *   VcpeCellIO io = {0};  io.activate = build_cell; io.deactivate = drop_cell;
 *   vcpe_cells_pack_io(&io);                                  // cell data as vcpe_pack packs (or your own load/unload)
 *   static uint8_t mem[...];                                  // vcpe_cells_bytes(ndefs)
 *   VcpeCells w;  vcpe_cells_init(&w, &cfg, &io, mem);
 *   each frame:  if (vcpe_cells_update(&w, player_pos, player_vel) & VCPE_CELLS_REBASED) ...origin moved...
 *                vcpe_cells_origin(&w, o);  camera = player - o;  draw cells at their position - o
 *   fast travel: vcpe_cells_update(&w, dest, 0) each frame behind a loading screen until vcpe_cells_ready(&w)
 *
 * The game's callbacks do the real work: load starts reading a cell (it may finish later: vcpe_cells_loaded),
 * activate turns loaded data into a live cell (vcpe_terrain_cell, spawning, collision) within a time budget and may
 * ask to be called again, deactivate puts it to sleep, unload frees its data. All callbacks run on the main thread
 * inside vcpe_cells_update (load completions wherever the game reports them). Plain C, no PSP headers. */
#ifndef VCPE_CELLS_H
#define VCPE_CELLS_H
#include <stdint.h>

typedef struct { int16_t cx, cz; uint32_t off, size, user; } VcpeCellDef;   /* off/size: PAK location of its pack */

enum { VCPE_CELL_OFF, VCPE_CELL_LOADING, VCPE_CELL_LOADED, VCPE_CELL_ACTIVATING, VCPE_CELL_ACTIVE };
enum { VCPE_CELLS_REBASED = 1, VCPE_CELLS_MOVED = 2, VCPE_CELLS_CHANGED = 4 };   /* vcpe_cells_update results */

typedef struct {
    int (*load)(void *user, int cell);                       /* 0 started (report with vcpe_cells_loaded), -1 failed */
    int (*activate)(void *user, int cell, uint32_t budget_us);   /* 1 done, 0 call again next frame, -1 failed */
    void (*deactivate)(void *user, int cell);
    void (*unload)(void *user, int cell);
    void *user;
} VcpeCellIO;

typedef struct {
    const VcpeCellDef *defs;
    int count;
    float cell_size;               /* metres; cell (cx, cz) spans [cx, cx+1) x [cz, cz+1) cells */
    int radius;                    /* active: Chebyshev cell distance <= radius */
    int preload;                   /* loaded but asleep: up to radius + preload */
    int keep;                      /* extra distance before sleeping/dropping again (hysteresis) */
    int max_inflight;              /* loads running at once */
    int max_resident;              /* loaded + active cells at most (e.g. the pack pool's slots) */
    uint32_t activate_budget_us;   /* activation time per frame (with a clock), passed to activate */
    uint32_t (*clock_us)(void);    /* microsecond clock; 0: one activate call per frame */
    float lookahead_s;             /* load toward where the focus will be in this many seconds */
    float rebase;                  /* metres from the origin that trigger an origin shift (0: never) */
    int retry_frames;              /* wait after a failed load before trying that cell again */
    int pack_priority;             /* vcpe_cells_pack_io: vcpe_pack priority of cell packs */
} VcpeCellsConfig;

typedef struct {
    VcpeCellsConfig cfg;
    VcpeCellIO io;
    uint8_t *state, *cooldown;
    int32_t *hash, *resident, *pack;
    int hash_mask, nresident, inflight;
    int fx, fz, px, pz;            /* focus cell and predicted cell */
    int ox, oz;                    /* origin, in cells */
    float origin[3];
    uint32_t loads, activations, unloads, failures, rebases;
} VcpeCells;

uint32_t vcpe_cells_bytes(int count);
/* radius 2, preload 1, keep 1, 2 loads at once, resident (2 x 4 + 1)^2, 2 ms activation, 1 s lookahead, rebase at
 * 4 cells, retry after 60 frames, pack priority 50 */
void vcpe_cells_defaults(VcpeCellsConfig *c, const VcpeCellDef *defs, int count, float cell_size);
/* mem: vcpe_cells_bytes(count), 4-byte aligned. -1 bad config or two defs for one cell */
int vcpe_cells_init(VcpeCells *w, const VcpeCellsConfig *c, const VcpeCellIO *io, void *mem);
/* focus: world position (the player or camera); vel: world velocity in m/s, or 0. Returns VCPE_CELLS_* flags */
int vcpe_cells_update(VcpeCells *w, const float focus[3], const float vel[3]);
void vcpe_cells_loaded(VcpeCells *w, int cell, int ok);    /* a load started by io.load finished */
int vcpe_cells_find(const VcpeCells *w, int cx, int cz);   /* def index or -1 */
int vcpe_cells_state(const VcpeCells *w, int cell);
void vcpe_cells_origin(const VcpeCells *w, float out[3]);  /* subtract from world positions for rendering */
int vcpe_cells_list(const VcpeCells *w, int state, int *out, int max);   /* cells in that state */
int vcpe_cells_ready(const VcpeCells *w);                  /* every existing cell within radius is active */
void vcpe_cells_flush(VcpeCells *w);                       /* sleep and drop everything (before a fast travel) */
int vcpe_cells_line(const VcpeCells *w, char *out, int n); /* "cells: 9 active, 16 asleep, 2 loading ..." */

/* Ready-made load/unload through vcpe_pack (link vcpe_pack, and vcpe_jobs for background reads): each cell's pack
 * from defs[i].off/size, pinned while resident so the pool never evicts live cell data (keep max_resident <= the
 * pool's slots). In activate/deactivate, vcpe_cells_pack(w, cell) is its pack handle. One world at a time. */
void vcpe_cells_pack_io(VcpeCellIO *io);
int vcpe_cells_pack(const VcpeCells *w, int cell);

#endif
