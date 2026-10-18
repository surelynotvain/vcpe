/* VCPE cells: open-world streaming around a moving focus (see vcpe_cells.h). */

/* HOW THIS FILE WORKS
 *
 * Every cell has a state: OFF (on the memory stick), LOADING, LOADED (in memory, asleep), ACTIVATING (being turned
 * into a live cell over several frames) and ACTIVE. Three distances from the focus cell (Chebyshev: a square ring)
 * drive the changes, each with `keep` cells of slack before going back, so a player walking along a border does not
 * load and drop the same cells every few steps:
 *
 *   active  radius                     wake up (activate) inside, sleep beyond radius + keep
 *   loaded  radius + preload           load inside, drop beyond radius + preload + keep
 *
 * Loads are aimed at where the player is going: the "loaded" distance is the smaller of the distance to the focus
 * cell and to the cell the focus will reach in lookahead seconds, and among the candidates the nearest to that
 * predicted point goes first (cells inside the active square always before the rest). A cell that failed to load
 * waits retry_frames before it is tried again.
 *
 * Activation is the expensive, frame-bound part (building terrain vertices, spawning objects), so it gets a time
 * budget per frame; a callback that needs longer returns 0 and continues next frame. An unfinished cell is finished
 * before another is started, nearest first.
 *
 * Floating origin: positions on the GE are relative to `origin`, a cell corner. When the focus moves more than
 * `rebase` metres from it, the origin jumps to the focus cell's corner and update() says so, so the game can move
 * whatever it caches in origin-relative space (static model matrices, particles).
 *
 * The def index is a cell's identity; an open-addressing hash maps (cx, cz) to it. Resident cells (anything not OFF)
 * are kept in a list, so per-frame work depends on the loaded area, not the world's size. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vcpe_cells.h"

/* vcpe_pack, weak: only vcpe_cells_pack_io uses it */
int vcpe_pack_load_async(uint32_t off, uint32_t size, int priority, void (*ready)(int, int, void *), void *user)
    __attribute__((weak));
void vcpe_pack_unload(int handle) __attribute__((weak));
void vcpe_pack_pin(int handle, int on) __attribute__((weak));

static uint32_t hash_size(int count)
{
    uint32_t n = 16;
    while (n < (uint32_t)count * 2u) n <<= 1;
    return n;
}
static uint32_t cell_hash(int cx, int cz) { return (uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u; }

uint32_t vcpe_cells_bytes(int count)
{
    if (count < 1 || count > 1 << 20)
        return 0;
    uint32_t bytes = ((uint32_t)count * 2u + 3u) & ~3u;
    return bytes + 4u * (hash_size(count) + 3u * (uint32_t)count);
}

void vcpe_cells_defaults(VcpeCellsConfig *c, const VcpeCellDef *defs, int count, float cell_size)
{
    memset(c, 0, sizeof *c);
    c->defs = defs;
    c->count = count;
    c->cell_size = cell_size;
    c->radius = 2;
    c->preload = 1;
    c->keep = 1;
    c->max_inflight = 2;
    c->max_resident = 81;
    c->activate_budget_us = 2000;
    c->lookahead_s = 1.0f;
    c->rebase = cell_size * 4;
    c->retry_frames = 60;
    c->pack_priority = 50;
}

int vcpe_cells_find(const VcpeCells *w, int cx, int cz)
{
    for (uint32_t i = cell_hash(cx, cz);; i++) {
        int32_t e = w->hash[i & (uint32_t)w->hash_mask];
        if (e < 0)
            return -1;
        if (w->cfg.defs[e].cx == cx && w->cfg.defs[e].cz == cz)
            return e;
    }
}

static VcpeCells *pack_world;
static int pack_load(void *user, int cell);
static void pack_unload(void *user, int cell);

int vcpe_cells_init(VcpeCells *w, const VcpeCellsConfig *c, const VcpeCellIO *io, void *mem)
{
    if (!w || !c || !c->defs || c->count < 1 || !(c->cell_size > 0) || c->radius < 0 || c->preload < 0 || c->keep < 0 ||
        c->max_inflight < 1 || c->max_resident < 1 || !mem || ((uintptr_t)mem & 3))
        return -1;
    memset(w, 0, sizeof *w);
    w->cfg = *c;
    if (io) w->io = *io;
    uint8_t *p = mem;
    uint32_t hs = hash_size(c->count);
    w->state = p;
    w->cooldown = p + c->count;
    p += ((uint32_t)c->count * 2u + 3u) & ~3u;
    w->hash = (int32_t *)p;
    w->resident = w->hash + hs;
    w->pack = w->resident + c->count;
    w->hash_mask = (int)(hs - 1);
    memset(w->state, 0, (size_t)c->count * 2);
    for (uint32_t i = 0; i < hs; i++) w->hash[i] = -1;
    for (int i = 0; i < c->count; i++) {
        w->pack[i] = -1;
        if (vcpe_cells_find(w, c->defs[i].cx, c->defs[i].cz) >= 0)
            return -1;
        uint32_t k = cell_hash(c->defs[i].cx, c->defs[i].cz);
        while (w->hash[k & (hs - 1)] >= 0) k++;
        w->hash[k & (hs - 1)] = i;
    }
    w->fx = w->fz = w->px = w->pz = INT32_MIN / 2;
    if (w->io.load == pack_load)
        pack_world = w;
    return 0;
}

static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }
static int cheb(int ax, int az, int bx, int bz) { return imax(iabs(ax - bx), iabs(az - bz)); }
static int dist_focus(const VcpeCells *w, int i) { return cheb(w->cfg.defs[i].cx, w->cfg.defs[i].cz, w->fx, w->fz); }
static int dist_load(const VcpeCells *w, int i)
{
    int a = dist_focus(w, i), b = cheb(w->cfg.defs[i].cx, w->cfg.defs[i].cz, w->px, w->pz);
    return a < b ? a : b;
}

static void drop_resident(VcpeCells *w, int cell)
{
    for (int k = 0; k < w->nresident; k++)
        if (w->resident[k] == cell) {
            w->resident[k] = w->resident[--w->nresident];
            return;
        }
}

void vcpe_cells_loaded(VcpeCells *w, int cell, int ok)
{
    if (!w || cell < 0 || cell >= w->cfg.count || w->state[cell] != VCPE_CELL_LOADING)
        return;
    w->inflight--;
    if (ok) {
        w->state[cell] = VCPE_CELL_LOADED;
        w->loads++;
    } else {
        w->state[cell] = VCPE_CELL_OFF;
        w->cooldown[cell] = (uint8_t)(w->cfg.retry_frames > 255 ? 255 : w->cfg.retry_frames < 0 ? 0 : w->cfg.retry_frames);
        w->failures++;
        drop_resident(w, cell);
    }
}

static void unload(VcpeCells *w, int cell)
{
    if (w->io.unload)
        w->io.unload(w->io.user, cell);
    w->state[cell] = VCPE_CELL_OFF;
    w->unloads++;
    drop_resident(w, cell);
}

static void start_load(VcpeCells *w, int cell)
{
    w->state[cell] = VCPE_CELL_LOADING;
    w->resident[w->nresident++] = cell;
    w->inflight++;
    if (!w->io.load) {
        vcpe_cells_loaded(w, cell, 1);   /* data always present */
        return;
    }
    /* the callback may report completion (or failure) before it returns */
    if (w->io.load(w->io.user, cell) < 0 && w->state[cell] == VCPE_CELL_LOADING)
        vcpe_cells_loaded(w, cell, 0);
}

int vcpe_cells_update(VcpeCells *w, const float focus[3], const float vel[3])
{
    if (!w || !focus)
        return 0;
    const VcpeCellsConfig *c = &w->cfg;
    int flags = 0;
    float s = c->cell_size, la = vel ? c->lookahead_s : 0;
    float qx = focus[0] + (vel ? vel[0] * la : 0), qz = focus[2] + (vel ? vel[2] * la : 0);
    int fx = (int)floorf(focus[0] / s), fz = (int)floorf(focus[2] / s);
    if (fx != w->fx || fz != w->fz)
        flags |= VCPE_CELLS_MOVED;
    w->fx = fx;
    w->fz = fz;
    w->px = (int)floorf(qx / s);
    w->pz = (int)floorf(qz / s);
    if (c->rebase > 0 && (fabsf(focus[0] - w->origin[0]) > c->rebase || fabsf(focus[2] - w->origin[2]) > c->rebase)) {
        w->ox = fx;
        w->oz = fz;
        w->origin[0] = fx * s;
        w->origin[2] = fz * s;
        w->rebases++;
        flags |= VCPE_CELLS_REBASED;
    }
    int active_r = c->radius, load_r = c->radius + c->preload;

    /* sleep and drop what is now too far (the list changes while walking it: walk it backwards) */
    for (int k = w->nresident - 1; k >= 0; k--) {
        if (k >= w->nresident) continue;
        int i = w->resident[k], st = w->state[i];
        if ((st == VCPE_CELL_ACTIVE || st == VCPE_CELL_ACTIVATING) && dist_focus(w, i) > active_r + c->keep) {
            if (w->io.deactivate) w->io.deactivate(w->io.user, i);
            w->state[i] = st = VCPE_CELL_LOADED;
            flags |= VCPE_CELLS_CHANGED;
        }
        if (st == VCPE_CELL_LOADED && dist_load(w, i) > load_r + c->keep) {
            unload(w, i);
            flags |= VCPE_CELLS_CHANGED;
        }
    }
    /* over the cap: drop sleeping cells, furthest first */
    while (w->nresident > c->max_resident) {
        int far = -1, fd = -1;
        for (int k = 0; k < w->nresident; k++) {
            int i = w->resident[k];
            if (w->state[i] == VCPE_CELL_LOADED && dist_load(w, i) > fd) { fd = dist_load(w, i); far = i; }
        }
        if (far < 0 || fd <= active_r)
            break;
        unload(w, far);
        flags |= VCPE_CELLS_CHANGED;
    }

    /* start loads: the best few candidates by distance to the predicted point */
    int free_slots = c->max_inflight - w->inflight;
    if (free_slots > 0 && w->nresident < c->max_resident) {
        enum { BEST = 8 };
        int best[BEST], tier[BEST], nb = 0;
        float score[BEST];
        int x0 = (fx < w->px ? fx : w->px) - load_r, x1 = (fx > w->px ? fx : w->px) + load_r;
        int z0 = (fz < w->pz ? fz : w->pz) - load_r, z1 = (fz > w->pz ? fz : w->pz) + load_r;
        int want = free_slots < BEST ? free_slots : BEST;
        if (want > c->max_resident - w->nresident) want = c->max_resident - w->nresident;
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) {
                int i = vcpe_cells_find(w, x, z);
                if (i < 0 || w->state[i] != VCPE_CELL_OFF || dist_load(w, i) > load_r)
                    continue;
                if (w->cooldown[i]) { w->cooldown[i]--; continue; }
                float dx = (x + 0.5f) * s - qx, dz = (z + 0.5f) * s - qz, sc = dx * dx + dz * dz;
                int t = dist_focus(w, i) <= active_r ? 0 : 1;     /* the active square first */
#define WORSE(k) (tier[k] > t || (tier[k] == t && score[k] > sc))
                if (nb == want && !WORSE(nb - 1))
                    continue;                                      /* no better than the worst kept */
                int k = nb < want ? nb++ : want - 1;               /* append, or replace the worst */
                while (k > 0 && WORSE(k - 1)) { score[k] = score[k - 1]; tier[k] = tier[k - 1]; best[k] = best[k - 1]; k--; }
#undef WORSE
                score[k] = sc;
                tier[k] = t;
                best[k] = i;
            }
        for (int k = 0; k < nb; k++) {
            start_load(w, best[k]);
            flags |= VCPE_CELLS_CHANGED;
        }
    }

    /* activation within the budget: unfinished cells first, then the nearest loaded one inside the active square */
    uint32_t t0 = c->clock_us ? c->clock_us() : 0;
    for (int calls = 0;; calls++) {
        int pick = -1, pd = 1 << 30;
        for (int k = 0; k < w->nresident; k++) {
            int i = w->resident[k], st = w->state[i], d = dist_focus(w, i);
            if (st == VCPE_CELL_ACTIVATING) d -= 1 << 20;
            else if (st != VCPE_CELL_LOADED || d > active_r) continue;
            if (d < pd) { pd = d; pick = i; }
        }
        if (pick < 0)
            break;
        uint32_t used = c->clock_us ? c->clock_us() - t0 : 0;
        if (calls && (!c->clock_us || used >= c->activate_budget_us))
            break;
        uint32_t budget = c->activate_budget_us > used ? c->activate_budget_us - used : 0;
        int r = w->io.activate ? w->io.activate(w->io.user, pick, budget) : 1;
        flags |= VCPE_CELLS_CHANGED;
        if (r > 0) {
            w->state[pick] = VCPE_CELL_ACTIVE;
            w->activations++;
        } else if (r == 0) {
            w->state[pick] = VCPE_CELL_ACTIVATING;
            break;      /* it used its time */
        } else {
            w->state[pick] = VCPE_CELL_LOADED;
            unload(w, pick);
            w->cooldown[pick] = (uint8_t)(c->retry_frames > 255 ? 255 : c->retry_frames < 0 ? 0 : c->retry_frames);
            w->failures++;
        }
    }
    return flags;
}

int vcpe_cells_state(const VcpeCells *w, int cell)
{
    return w && cell >= 0 && cell < w->cfg.count ? w->state[cell] : VCPE_CELL_OFF;
}

void vcpe_cells_origin(const VcpeCells *w, float out[3])
{
    out[0] = w->origin[0];
    out[1] = w->origin[1];
    out[2] = w->origin[2];
}

int vcpe_cells_list(const VcpeCells *w, int state, int *out, int max)
{
    int n = 0;
    for (int k = 0; k < w->nresident; k++)
        if (w->state[w->resident[k]] == state) {
            if (n < max) out[n] = w->resident[k];
            n++;
        }
    return n;
}

int vcpe_cells_ready(const VcpeCells *w)
{
    int r = w->cfg.radius;
    for (int z = w->fz - r; z <= w->fz + r; z++)
        for (int x = w->fx - r; x <= w->fx + r; x++) {
            int i = vcpe_cells_find(w, x, z);
            if (i >= 0 && w->state[i] != VCPE_CELL_ACTIVE)
                return 0;
        }
    return 1;
}

void vcpe_cells_flush(VcpeCells *w)
{
    for (int k = w->nresident - 1; k >= 0; k--) {
        if (k >= w->nresident) continue;
        int i = w->resident[k];
        if (w->state[i] == VCPE_CELL_ACTIVE || w->state[i] == VCPE_CELL_ACTIVATING) {
            if (w->io.deactivate) w->io.deactivate(w->io.user, i);
            w->state[i] = VCPE_CELL_LOADED;
        }
        if (w->state[i] == VCPE_CELL_LOADED)
            unload(w, i);
    }
}

int vcpe_cells_line(const VcpeCells *w, char *out, int n)
{
    int count[5] = {0};
    for (int k = 0; k < w->nresident; k++)
        count[w->state[w->resident[k]]]++;
    return snprintf(out, (size_t)n, "cells: %d active, %d asleep, %d loading, %d building; focus %d,%d origin %d,%d; "
                    "%u loads %u drops %u failed %u rebases", count[VCPE_CELL_ACTIVE], count[VCPE_CELL_LOADED],
                    count[VCPE_CELL_LOADING], count[VCPE_CELL_ACTIVATING], w->fx, w->fz, w->ox, w->oz,
                    (unsigned)w->loads, (unsigned)w->unloads, (unsigned)w->failures, (unsigned)w->rebases);
}

/* ------------------------------------------------------------------ vcpe_pack loader */
static void pack_ready(int handle, int ok, void *user)
{
    VcpeCells *w = pack_world;
    int cell = (int)(intptr_t)user;
    if (!w || cell < 0 || cell >= w->cfg.count)
        return;
    if (ok && handle >= 0) {
        w->pack[cell] = handle;
        if (vcpe_pack_pin) vcpe_pack_pin(handle, 1);
    }
    vcpe_cells_loaded(w, cell, ok && handle >= 0);
}

static int pack_load(void *user, int cell)
{
    (void)user;
    VcpeCells *w = pack_world;
    if (!w || !vcpe_pack_load_async)
        return -1;
    const VcpeCellDef *d = &w->cfg.defs[cell];
    int h = vcpe_pack_load_async(d->off, d->size, w->cfg.pack_priority, pack_ready, (void *)(intptr_t)cell);
    return h < 0 ? -1 : 0;
}

static void pack_unload(void *user, int cell)
{
    (void)user;
    VcpeCells *w = pack_world;
    if (!w || w->pack[cell] < 0)
        return;
    if (vcpe_pack_pin) vcpe_pack_pin(w->pack[cell], 0);
    if (vcpe_pack_unload) vcpe_pack_unload(w->pack[cell]);
    w->pack[cell] = -1;
}

void vcpe_cells_pack_io(VcpeCellIO *io)
{
    io->load = pack_load;
    io->unload = pack_unload;
}

int vcpe_cells_pack(const VcpeCells *w, int cell)
{
    return w && cell >= 0 && cell < w->cfg.count ? w->pack[cell] : -1;
}
