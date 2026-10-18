/* vcpe_cells host test: rings, priorities, hysteresis, budgets, failures, origin shifts, teleports and the
 * vcpe_pack loader. */
#include "vcpe_cells.h"
#include "vcpe_pack.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W 20
static VcpeCellDef defs[W * W];
static int ndefs;
static VcpeCells world;

/* fake asynchronous loader: completes `delay` updates after starting */
static struct { int cell, due; } pending[64];
static int npending, frame, delay = 2, fail_cell = -1, fail_times, loading_now, max_loading, order[64], norder;
static int activate_calls[W * W], activate_cost = 1, unloads[W * W], loads[W * W];
static uint32_t clock_now;
static uint32_t fake_clock(void) { return clock_now; }

static int fake_load(void *user, int cell)
{
    (void)user;
    if (cell == fail_cell && fail_times > 0) { fail_times--; return -1; }
    pending[npending].cell = cell;
    pending[npending++].due = frame + delay;
    loads[cell]++;
    if (norder < 64) order[norder++] = cell;
    loading_now++;
    if (loading_now > max_loading) max_loading = loading_now;
    return 0;
}
static int fake_activate(void *user, int cell, uint32_t budget)
{
    (void)user; (void)budget;
    clock_now += 1500;
    return ++activate_calls[cell] >= activate_cost;
}
static int deactivated;
static void fake_deactivate(void *user, int cell) { (void)user; (void)cell; deactivated++; }
static void fake_unload(void *user, int cell) { (void)user; unloads[cell]++; }

static void tick(const float pos[3], const float vel[3])
{
    frame++;
    for (int i = 0; i < npending; i++)
        if (pending[i].due <= frame) {
            int c = pending[i].cell;
            pending[i--] = pending[--npending];
            loading_now--;
            vcpe_cells_loaded(&world, c, 1);
        }
    vcpe_cells_update(&world, pos, vel);
}

static int count_state(int st) { int n = 0; for (int i = 0; i < ndefs; i++) n += world.state[i] == st; return n; }

/* pak stand-in for vcpe_pack */
static const char *pak;
int vcpe_pak_read(uint32_t off, void *dst, uint32_t size)
{
    FILE *f = fopen(pak, "rb");
    int ok = f && !fseek(f, off, SEEK_SET) && fread(dst, 1, size, f) == size;
    if (f) fclose(f);
    return ok ? 0 : -1;
}
const char *vcpe_pak_path(void) { return pak; }

static int pack_found;
static int pack_activate(void *user, int cell, uint32_t budget)
{
    (void)user; (void)budget;
    uint32_t n;
    const char *txt = vcpe_pack_find(vcpe_cells_pack(&world, cell), "cell.txt", &n);
    char want[32];
    snprintf(want, sizeof want, "cell %d %d", defs[cell].cx, defs[cell].cz);
    pack_found += txt && n == strlen(want) && !memcmp(txt, want, n);
    return 1;
}

int main(int argc, char **argv)
{
    /* a 20 x 20 world with an ocean hole at (10..11, 10..11) */
    for (int z = 0; z < W; z++)
        for (int x = 0; x < W; x++)
            if (!(x >= 10 && x <= 11 && z >= 10 && z <= 11))
                defs[ndefs++] = (VcpeCellDef){(int16_t)x, (int16_t)z, 0, 0, 0};
    VcpeCellsConfig cfg;
    vcpe_cells_defaults(&cfg, defs, ndefs, 64);
    cfg.clock_us = fake_clock;
    VcpeCellIO io = {fake_load, fake_activate, fake_deactivate, fake_unload, 0};
    static uint8_t mem[65536] __attribute__((aligned(16)));
    CHECK(vcpe_cells_bytes(ndefs) <= sizeof mem, "memory %u", vcpe_cells_bytes(ndefs));
    VcpeCellDef dup[2] = {{1, 1, 0, 0, 0}, {1, 1, 0, 0, 0}};
    VcpeCellsConfig bad = cfg;
    bad.defs = dup; bad.count = 2;
    CHECK(vcpe_cells_init(&world, &bad, &io, mem) == -1, "duplicate cell refused");
    CHECK(!vcpe_cells_init(&world, &cfg, &io, mem), "init");
    CHECK(vcpe_cells_find(&world, 3, 4) == 4 * W + 3 && vcpe_cells_find(&world, 10, 10) < 0 && vcpe_cells_find(&world, -1, 0) < 0, "lookup");

    /* standing still: 5 x 5 active, a ring of 24 asleep, nearest first, two loads at a time */
    float pos[3] = {5.5f * 64, 0, 5.5f * 64};
    for (int i = 0; i < 80; i++) tick(pos, 0);
    CHECK(count_state(VCPE_CELL_ACTIVE) == 25 && count_state(VCPE_CELL_LOADED) == 24 && vcpe_cells_ready(&world), "25 active, 24 asleep (%d %d)", count_state(VCPE_CELL_ACTIVE), count_state(VCPE_CELL_LOADED));
    CHECK(max_loading == 2, "two loads in flight at most (%d)", max_loading);
    CHECK(order[0] == vcpe_cells_find(&world, 5, 5), "the focus cell loads first");
    int ring1 = 1;
    for (int k = 0; k < 9; k++) ring1 &= abs(defs[order[k]].cx - 5) <= 1 && abs(defs[order[k]].cz - 5) <= 1;
    CHECK(ring1, "then its neighbours");
    for (int k = 0; k < 25; k++) CHECK(abs(defs[order[k]].cx - 5) <= 2 && abs(defs[order[k]].cz - 5) <= 2, "the active square before the asleep ring");
    char line[256];
    vcpe_cells_line(&world, line, sizeof line);
    CHECK(strstr(line, "25 active, 24 asleep") != 0, "%s", line);

    /* pacing across a border never thrashes */
    int before = 0;
    for (int i = 0; i < ndefs; i++) before += unloads[i];
    for (int i = 0; i < 300; i++) {
        float p[3] = {(i & 16 ? 6.05f : 5.95f) * 64, 0, 5.5f * 64};
        tick(p, 0);
    }
    int after = 0;
    for (int i = 0; i < ndefs; i++) after += unloads[i];
    CHECK(after - before == 0, "no drops while pacing over a border (%d)", after - before);

    /* walking east at 12 m/s: cells ahead are loaded before the focus reaches them */
    int late = 0, steps = 0;
    for (float x = 6.0f * 64; x < 14.5f * 64; x += 12.0f / 60, steps++) {
        float p[3] = {x, 0, 5.5f * 64}, v[3] = {12, 0, 0};
        int fx_before = world.fx;
        tick(p, v);
        if (world.fx != fx_before) {
            int ahead = vcpe_cells_find(&world, world.fx + 2, 5);
            late += ahead >= 0 && vcpe_cells_state(&world, ahead) < VCPE_CELL_LOADED;
        }
    }
    CHECK(late == 0, "new active column already loaded when entered (%d late)", late);
    CHECK(world.nresident <= cfg.max_resident, "resident cap");
    for (int i = 0; i < 120; i++) { float p[3] = {14.5f * 64, 0, 5.5f * 64}; tick(p, 0); }
    CHECK(vcpe_cells_ready(&world) && vcpe_cells_state(&world, vcpe_cells_find(&world, 5, 5)) == VCPE_CELL_OFF, "settled at the destination, origin cells dropped");

    /* floating origin */
    float o[3];
    vcpe_cells_origin(&world, o);
    CHECK(fabsf(o[0] - 64.0f * world.ox) < 1e-3f && fabsf(o[0] - 14.5f * 64) <= cfg.rebase + 64 && world.rebases >= 1, "origin follows in cell steps (%g)", o[0]);
    int flags = vcpe_cells_update(&world, (float[3]){19.9f * 64, 0, 5.5f * 64}, 0);
    CHECK(flags & VCPE_CELLS_REBASED, "rebase reported");

    /* the ocean hole is skipped, ready() ignores missing cells */
    vcpe_cells_flush(&world);
    CHECK(count_state(VCPE_CELL_ACTIVE) == 0 && count_state(VCPE_CELL_LOADED) == 0, "flush drops everything");
    for (int i = 0; i < 120; i++) { float p[3] = {10.5f * 64, 0, 10.5f * 64}; tick(p, 0); }
    CHECK(vcpe_cells_ready(&world) && count_state(VCPE_CELL_ACTIVE) == 25 - 4, "around the hole: 21 active");

    /* multi-frame activation finishes one cell before starting the next; the clock budget allows 2 calls a frame */
    vcpe_cells_flush(&world);
    for (int i = 0; i < 300; i++) tick((float[3]){2.5f * 64, 0, 2.5f * 64}, 0);
    memset(activate_calls, 0, sizeof activate_calls);
    activate_cost = 3;
    vcpe_cells_flush(&world);
    int building_max = 0;
    for (int i = 0; i < 200; i++) {
        tick((float[3]){2.5f * 64, 0, 2.5f * 64}, 0);
        int b = count_state(VCPE_CELL_ACTIVATING);
        if (b > building_max) building_max = b;
    }
    CHECK(building_max == 1 && vcpe_cells_ready(&world), "one cell under construction at a time");
    activate_cost = 1;

    /* failures retry after the cooldown */
    vcpe_cells_flush(&world);
    for (int i = 0; i < 100; i++) tick((float[3]){17.5f * 64, 0, 17.5f * 64}, 0);
    fail_cell = vcpe_cells_find(&world, 3, 17);
    fail_times = 2;
    uint32_t failures = world.failures;
    vcpe_cells_flush(&world);
    for (int i = 0; i < 30; i++) tick((float[3]){3.5f * 64, 0, 17.5f * 64}, 0);
    CHECK(world.failures == failures + 1 && vcpe_cells_state(&world, fail_cell) == VCPE_CELL_OFF, "failed once, waiting");
    for (int i = 0; i < 200; i++) tick((float[3]){3.5f * 64, 0, 17.5f * 64}, 0);
    CHECK(world.failures == failures + 2 && vcpe_cells_state(&world, fail_cell) == VCPE_CELL_ACTIVE, "retried after the cooldown and loaded");

    /* synchronous loads (completion inside io.load) */
    VcpeCellIO sync = {0, fake_activate, 0, 0, 0};
    cfg.clock_us = 0;
    CHECK(!vcpe_cells_init(&world, &cfg, &sync, mem), "init without a loader");
    for (int i = 0; i < 60; i++) vcpe_cells_update(&world, pos, 0);
    CHECK(vcpe_cells_ready(&world) && world.activations == 25, "always-present data activates one cell per frame without a clock");

    /* vcpe_pack loader: packs pinned while resident, found in activate, unpinned and freed when dropped */
    if (argc > 2) {
        pak = argv[1];
        int n = 0;
        for (int i = 2; i + 3 < argc; i += 4, n++)
            defs[n] = (VcpeCellDef){(int16_t)atoi(argv[i]), (int16_t)atoi(argv[i + 1]), (uint32_t)atoi(argv[i + 2]), (uint32_t)atoi(argv[i + 3]), 0};
        CHECK(vcpe_pack_pool(0, VCPE_PACK_MAX_SLOTS * 4096, VCPE_PACK_MAX_SLOTS) == 4096, "pack pool");
        vcpe_cells_defaults(&cfg, defs, n, 64);
        cfg.radius = 1;
        cfg.max_resident = VCPE_PACK_MAX_SLOTS;   /* never more cells than pool slots */
        VcpeCellIO pio = {0};
        pio.activate = pack_activate;
        vcpe_cells_pack_io(&pio);
        CHECK(!vcpe_cells_init(&world, &cfg, &pio, mem), "pack world");
        for (int i = 0; i < 60; i++) vcpe_cells_update(&world, (float[3]){2.5f * 64, 0, 2.5f * 64}, 0);
        vcpe_cells_line(&world, line, sizeof line);
        CHECK(vcpe_cells_ready(&world) && pack_found == 9, "9 active cells found their pack contents (%d): %s", pack_found, line);
        int c0 = vcpe_cells_find(&world, 2, 2), h0 = vcpe_cells_pack(&world, c0);
        CHECK(h0 >= 0 && vcpe_pack_state(h0) == VCPE_PACK_READY, "pack ready");
        for (int i = 0; i < 120; i++) vcpe_cells_update(&world, (float[3]){6.5f * 64, 0, 6.5f * 64}, 0);   /* 4 cells away: beyond the keep distance */
        CHECK(vcpe_cells_pack(&world, c0) == -1 && vcpe_pack_state(h0) == VCPE_PACK_FREE, "dropped cell's pack freed");
        CHECK(vcpe_cells_ready(&world) && world.nresident <= VCPE_PACK_MAX_SLOTS && world.failures == 0, "moved; resident %d", world.nresident);
        vcpe_pack_pool_free();
    }

    printf("PASS cells: rings, nearest-first loads, inflight/resident caps, no thrash, lookahead, origin shifts, holes, budgets, retries, packs\n");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
