/* Host test for vcpe_ultra: event bus (order, filtering, re-posting, thread ring, overflow), blackboard, live
 * tuning from a file (parse, clamp, change detection, save), toolkit (ring, pool, fixed point, timers, profiler),
 * autotest commands and the engine events.
 * From the repository root: cc -O2 -Isrc tests/host/ultra_test.c src/vcpe_ultra.c -o /tmp/ultra_test */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "vcpe_ultra.h"
#include "vcpe_test.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { fails++; printf("FAIL " __VA_ARGS__); } printf("\n"); } while (0)

/* the engine state vcpe_ultra watches (normally from vcpe_sys / vcpe_gfx / vcpe_load) */
volatile int vcpe_resume_gen;
int vcpe_stat_vblank_misses, vcpe_stat_vblanks = 1;
static int loading;
int vcpe_loading_active(void) { return loading; }
int vcpe_loading_shown(void) { return 7; }

static int seen[16], order[64], norder;
static void on_any(const VcpeEvent *e, void *u) { (void)u; if (e->id < 16) seen[e->id]++; if (norder < 64) order[norder++] = e->id; }
static int reposts;
static void on_game(const VcpeEvent *e, void *u)
{
    *(int *)u += e->i;
    if (reposts++ < 3)
        vcpe_ev_post(VCPE_EV_GAME, 1, 0, 0);   /* posting from a listener: delivered next dispatch */
}

int main(void)
{
    /* event bus */
    int sum = 0;
    int h_any = vcpe_ev_on(VCPE_EV_ANY, on_any, 0), h_game = vcpe_ev_on(VCPE_EV_GAME, on_game, &sum);
    vcpe_ev_post(VCPE_EV_GAME, 5, 0, 0);
    vcpe_ev_post(VCPE_EV_GAME + 1, 9, 0, 0);
    vcpe_ev_post_thread(VCPE_EV_GAME, 2, 0, 0);
    int n1 = vcpe_ev_dispatch();
    CHECK(n1 == 3 && sum == 7 && norder == 3, "dispatch delivers queued + thread events, filtered by id (%d, sum %d)", n1, sum);
    int n2 = vcpe_ev_dispatch();
    CHECK(n2 == 2 && sum == 9, "events posted by listeners wait for the next dispatch (%d)", n2);
    vcpe_ev_dispatch(); vcpe_ev_dispatch();
    vcpe_ev_off(h_game);
    vcpe_ev_post(VCPE_EV_GAME, 100, 0, 0);
    vcpe_ev_dispatch();
    CHECK(sum == 10, "vcpe_ev_off stops delivery (sum %d)", sum);
    for (int i = 0; i < 300; i++) vcpe_ev_post(VCPE_EV_GAME + 2, i, 0, 0);
    CHECK(vcpe_ev_dropped == 44 && vcpe_ev_dispatch() == 256, "full queue drops and counts (%d dropped)", vcpe_ev_dropped);
    (void)h_any;

    /* blackboard */
    memset(seen, 0, sizeof seen);
    vcpe_bb_set_int("player.hp", 3);
    vcpe_bb_set_int("player.hp", 3);   /* same value: no event */
    vcpe_bb_set_float("speed", 1.5f);
    vcpe_bb_set_str("level", "forest");
    vcpe_ev_dispatch();
    CHECK(seen[VCPE_EV_BB_CHANGED] == 3, "blackboard changes post events, unchanged sets don't (%d)", seen[VCPE_EV_BB_CHANGED]);
    CHECK(vcpe_bb_int("player.hp", 0) == 3 && vcpe_bb_float("speed", 0) == 1.5f && !strcmp(vcpe_bb_str("level", ""), "forest") &&
          vcpe_bb_int("missing", -1) == -1 && vcpe_bb_type("speed") == VCPE_BB_FLOAT && vcpe_bb_count() == 3, "blackboard values and defaults");
    char nm[32];
    for (int i = 0; i < 200; i++) { snprintf(nm, sizeof nm, "k%d", i); vcpe_bb_set_int(nm, i); }
    int ok = 1;
    for (int i = 0; i < 200; i++) { snprintf(nm, sizeof nm, "k%d", i); ok &= vcpe_bb_int(nm, -1) == i; }
    CHECK(ok && vcpe_bb_count() == 203, "200 more keys all found (%d)", vcpe_bb_count());

    /* live tuning */
    float speed = 1; int lives = 3;
    vcpe_tune_float("player.speed", &speed, 0, 10);
    vcpe_tune_int("player.lives", &lives, 1, 9);
    vcpe_tune_file("/tmp/vcpe_ultra_tune.txt", 1);
    FILE *f = fopen("/tmp/vcpe_ultra_tune.txt", "w");
    fprintf(f, "# test\nplayer.speed = 4.25   # faster\nplayer.lives=12\nunknown = 3\n");
    fclose(f);
    memset(seen, 0, sizeof seen);
    vcpe_ultra_update();
    CHECK(speed == 4.25f && lives == 9 && seen[VCPE_EV_TUNE_CHANGED] == 2, "tune file read on change, clamped (%g, %d)", speed, lives);
    speed = 2;
    vcpe_ultra_update();
    CHECK(speed == 2, "unchanged file is not re-read");
    CHECK(vcpe_tune_set("player.speed", "7") == 1 && speed == 7 && vcpe_tune_set("nope", "1") == -1, "tune set by name");
    CHECK(vcpe_tune_save() == 0, "tune save");
    speed = 0; lives = 0;
    CHECK(vcpe_tune_reload() == 2 && speed == 7 && lives == 9, "saved file reads back (%g %d)", speed, lives);

    /* engine events */
    memset(seen, 0, sizeof seen);
    vcpe_ultra_update();
    vcpe_resume_gen++;
    vcpe_stat_vblank_misses += 2; vcpe_stat_vblanks = 2;
    loading = 1;
    vcpe_ultra_update();
    loading = 0;
    vcpe_ultra_update();
    CHECK(seen[VCPE_EV_RESUME] == 1 && seen[VCPE_EV_LATE_FRAME] == 1 && seen[VCPE_EV_LOAD_BEGIN] == 1 && seen[VCPE_EV_LOAD_END] == 1,
          "engine events: resume, late frame, load begin/end");

    /* autotest commands */
    unsigned held = 0;
    int r1 = vcpe_ultra_command("tune", "player.lives", 2, 0, "tune player.lives 2", &held);
    int r2 = vcpe_ultra_command("bb", "gold", 0, 0, "bb gold 250", &held);
    int r3 = vcpe_ultra_command("bb", "name", 0, 0, "bb name vain", &held);
    int r4 = vcpe_ultra_command("jump", "", 0, 0, "jump", &held);
    CHECK(r1 == VCPE_CMD_NEXT && lives == 2 && vcpe_bb_int("gold", 0) == 250 && !strcmp(vcpe_bb_str("name", ""), "vain") &&
          r2 == VCPE_CMD_NEXT && r3 == VCPE_CMD_NEXT && r4 == VCPE_CMD_UNKNOWN, "autotest commands");

    /* toolkit */
    int mem[8], v, got = 0;
    VcpeRing ring;
    vcpe_ring_init(&ring, mem, sizeof(int), 8);
    for (v = 0; v < 10; v++) got += vcpe_ring_push(&ring, &v) == 0;
    int first; vcpe_ring_pop(&ring, &first);
    CHECK(got == 8 && first == 0 && vcpe_ring_count(&ring) == 7, "ring: capacity, FIFO order");
    static char pool_mem[16 * 10];
    VcpePool pool;
    vcpe_pool_init(&pool, pool_mem, 13, 10);
    void *blocks[11]; int nb = 0;
    while (nb < 11 && (blocks[nb] = vcpe_pool_alloc(&pool))) nb++;
    vcpe_pool_free(&pool, blocks[3]);
    CHECK(nb == 10 && pool.used == 9 && vcpe_pool_alloc(&pool) == blocks[3] && blocks[0] == (void *)pool_mem, "pool: 10 blocks, O(1) reuse");
    CHECK(vcpe_fx_to(vcpe_fx_mul(vcpe_fx_from(1.5f), vcpe_fx_from(-2))) == -3.0f &&
          vcpe_fx_div(VCPE_FX_ONE, 4 * VCPE_FX_ONE) == VCPE_FX_ONE / 4, "fixed point");
    VcpeTimer t; vcpe_timer_start(&t, 0.1f, 1);
    int fires = 0; for (int i = 0; i < 60; i++) fires += vcpe_timer_tick(&t, 1 / 60.0f);
    CHECK(fires == 10, "repeating timer without drift (%d fires in 1 s)", fires);
    int pid = vcpe_prof_id("work");
    for (int fr = 0; fr < 200; fr++) { vcpe_prof_begin(pid); usleep(1000); vcpe_prof_end(pid); vcpe_ultra_update(); }
    char line[96]; vcpe_prof_line(line, sizeof line);
    CHECK(vcpe_prof_us(pid) > 800 && vcpe_prof_us(pid) < 3000 && strstr(line, "work"), "profiler: %s", line);
    CHECK(vcpe_fnv("") == 2166136261u && vcpe_fnv("a") == 0xE40C292Cu, "FNV-1a reference values");

    printf("RESULT %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
