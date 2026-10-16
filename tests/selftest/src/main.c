/* VCPE engine self-test: every built-in in one EBOOT, driven by tests/selftest/run.sh in PPSSPP headless.
 * Writes SELFTEST.LOG (PASS / FAIL lines) and snapshots. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vcpe.h"
#include "pakinfo.h"

PSP_MODULE_INFO("vcpeselftest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

enum { B_LEFT = 1, B_RIGHT = 2, B_UP = 4, B_DOWN = 8, B_OK = 16, B_BACK = 32, B_MENU = 64 };
static const VcpePadBind binds[] = {{PSP_CTRL_LEFT, B_LEFT}, {PSP_CTRL_RIGHT, B_RIGHT}, {PSP_CTRL_UP, B_UP},
    {PSP_CTRL_DOWN, B_DOWN}, {PSP_CTRL_CROSS, B_OK}, {PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE, B_BACK},
    {PSP_CTRL_START, B_MENU}};
static int fails, frame, presses, scene;
static int m_clicks, m_dclicks, m_drags, m_drops, m_rpress, m_wheel, m_warps;
static float m_drop_x, m_drop_y;
static VcpeLodMesh lod_m;
static int lod_lvl[5] = {-1, -1, -1, -1, -1}, lod_full;   /* lod_full: autotest "lodfull 1" draws level 0 only */
#define CHECK(c, ...) do { if (c) vcpe_log("PASS " __VA_ARGS__); else { fails++; vcpe_log("FAIL " __VA_ARGS__); } } while (0)

static int cmd(const char *c, const char *arg, int num, int num2, const char *line, unsigned *held)
{
    if (!strcmp(c, "scene")) { scene = atoi(arg); return VCPE_CMD_FRAME; }
    if (!strcmp(c, "lodfull")) { lod_full = atoi(arg); return VCPE_CMD_FRAME; }
    if (!strcmp(c, "loading")) {   /* a slow load behind the loading screen, snapshot halfway */
        vcpe_loading_show_after(0);
        vcpe_loading_begin(1000);
        /* boot log lines: the engine's own (a PAK read, a bundle) and the game's */
        static const VcpeBundle lb[1] = {{PAK_HELLO_OFF, PAK_HELLO_SIZE, 0, 0}};
        vcpe_gfx_tables(0, lb, 1);
        vcpe_bundle_load(0);
        vcpe_bundle_unload(0);
        int st = vcpe_step_begin("Creating test world (%d objects)", 42);
        int nested = vcpe_step_begin("hidden: inside another step");
        vcpe_step_end(nested, 1);
        vcpe_step_end(st, 1);
        vcpe_step_end(vcpe_step_begin("Opening a file that is not there"), 0);
        vcpe_step_note(VCPE_STEP_INFO, "Memory: heap %d KB free", vcpe_heap_kb());
        CHECK(nested == -1 && st > 0, "steps: nested step not reported (%d), outer step %d", nested, st);
        int running = vcpe_step_begin("Waiting for the slow part");
        for (int i = 0; i < 10; i++) {
            vcpe_loading_add(100);
            sceKernelDelayThread(40000);
            if (i == 5)
                vcpe_test_snap("loading");
            if (i == 9)
                vcpe_test_snap("loading2");   /* the animation moved on */
        }
        vcpe_step_end(running, 1);
        vcpe_loading_end();
        CHECK(vcpe_loading_shown() >= 5 && !vcpe_loading_active(), "loading screen drew %d frames", vcpe_loading_shown());
        vcpe_loading_show_after(150);
        vcpe_loading_begin(0);
        vcpe_loading_add(10);
        vcpe_loading_end();
        CHECK(vcpe_loading_shown() == 0, "a quick load shows no loading screen");
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(c, "batch")) { vcpe_gfx_batch(atoi(arg)); return VCPE_CMD_NEXT; }
    if (!strcmp(c, "pipeline")) { vcpe_gfx_pipeline(atoi(arg)); return VCPE_CMD_NEXT; }
    if (!strcmp(c, "mreset")) { m_clicks = m_dclicks = m_drags = m_drops = m_rpress = m_wheel = m_warps = 0; return VCPE_CMD_NEXT; }
    if (!strcmp(c, "mat")) {   /* mat X Y: the pointer is there */
        float x = atoi(arg), y = num;
        CHECK(vcpe_absf(vcpe_mouse_x - x) < 0.01f && vcpe_absf(vcpe_mouse_y - y) < 0.01f, "mouse at %.1f %.1f (expected %.0f %.0f)", vcpe_mouse_x, vcpe_mouse_y, x, y);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(c, "mcheck")) {   /* mcheck clicks dclicks drags drops rpress wheel warps */
        int e[7] = {0};
        sscanf(line, "%*s %d %d %d %d %d %d %d", &e[0], &e[1], &e[2], &e[3], &e[4], &e[5], &e[6]);
        CHECK(m_clicks == e[0] && m_dclicks == e[1] && m_drags == e[2] && m_drops == e[3] && m_rpress == e[4] && m_wheel == e[5] && m_warps == e[6],
              "mouse events clicks %d dclicks %d drags %d drops %d rpress %d wheel %d warps %d (expected %d %d %d %d %d %d %d)",
              m_clicks, m_dclicks, m_drags, m_drops, m_rpress, m_wheel, m_warps, e[0], e[1], e[2], e[3], e[4], e[5], e[6]);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(c, "mdrop")) {   /* mdrop X Y: the last drop happened there */
        CHECK(vcpe_absf(m_drop_x - atoi(arg)) < 0.01f && vcpe_absf(m_drop_y - num) < 0.01f, "drop at %.1f %.1f", m_drop_x, m_drop_y);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(c, "report")) {
        vcpe_log("presses counted %d", presses);
        CHECK(presses == num, "pad presses %d (expected %d)", presses, num);
        vcpe_log("RESULT %s (%d failures)", fails ? "FAIL" : "PASS", fails);
        vcpe_log_flush();
        return VCPE_CMD_NEXT;
    }
    return VCPE_CMD_UNKNOWN;
}

static int both(const char *c, const char *arg, int num, int num2, const char *line, unsigned *held)
{
    int r = vcpe_mouse_command(c, arg, num, num2, line, held);
    return r != VCPE_CMD_UNKNOWN ? r : cmd(c, arg, num, num2, line, held);
}

/* points of interest: a 4x3 grid of slots on the right half */
static void slots(void)
{
    for (int i = 0; i < 12; i++)
        vcpe_mouse_poi_rect(260 + (i % 4) * 50, 60 + (i / 4) * 50, 40, 40);
}

static void mouse_scene_draw(void)
{
    for (int i = 0; i < 12; i++) {
        float x = 260 + (i % 4) * 50, y = 60 + (i / 4) * 50;
        vcpe_fill(x, y, 40, 40, vcpe_mouse_in(x, y, 40, 40) ? 0xFF60C0FF : 0xFF604030);
    }
    vcpe_hud_printf(10, 10, 0xFFFFFFFF, "mouse %.0f %.0f  clicks %d drags %d drops %d", vcpe_mouse_x, vcpe_mouse_y, m_clicks, m_drags, m_drops);
    float x = vcpe_mouse_x, y = vcpe_mouse_y;
    for (int sh = VCPE_CURSOR_ARROW; sh <= VCPE_CURSOR_CROSS; sh++) {   /* every built-in shape, then the pointer */
        vcpe_mouse_set(20 + sh * 40, 230);
        vcpe_mouse_draw(sh, 1);
        vcpe_mouse_set(20 + sh * 40, 180);
        vcpe_mouse_draw(sh, 2);
    }
    vcpe_mouse_set(x, y);
    vcpe_mouse_draw(vcpe_mouse_dragging() ? VCPE_CURSOR_GRAB : VCPE_CURSOR_ARROW, 1);
}

static void ev_count(const VcpeEvent *e, void *u) { if (e->id < 8) ((int *)u)[e->id]++; }
static void ev_add(const VcpeEvent *e, void *u) { *(int *)u += e->i; }
static int st_done_n;
static void st_job(void *a) { (void)a; sceKernelDelayThread(20000); }
static void st_done(int id, void *a) { (void)id; (void)a; st_done_n++; }
static int co_runs;
static void co_fn(void *arg) { co_runs++; vcpe_co_wait_frame(); co_runs++; }

/* 4.1 LOD meshes: the same sphere at 5 distances, each at the level the picker chose (colour = UV) */
static void lod_scene_draw(void)
{
    static const float cam[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
    vcpe_3d_begin(cam, rot, 60, 0.1f, 200);
    VcpeLodView view;
    vcpe_3d_lod_view(&view, 1);
    for (int k = 0; k < 5; k++) {
        float d = 2.5f * (1 << k), c[3] = {(k - 2) * 0.32f * d, 0, -d};
        float m[16] = {1, 0, 0, c[0], 0, 1, 0, c[1], 0, 0, 1, c[2], 0, 0, 0, 1};
        lod_lvl[k] = vcpe_lod_pick(&lod_m, &view, c, 1, lod_lvl[k]);
        vcpe_3d_model(m);
        vcpe_3d_draw_lod(-1, &lod_m, lod_full ? 0 : lod_lvl[k]);
    }
    vcpe_3d_model(0);
    vcpe_3d_end();
    vcpe_hud_printf(10, 10, 0xFFFFFFFF, "LOD levels %d %d %d %d %d  tris %d of %d", lod_lvl[0], lod_lvl[1], lod_lvl[2],
                    lod_lvl[3], lod_lvl[4], vcpe_lod_stat_drawn, vcpe_lod_stat_full);
    static int checked;
    if (!checked++) {
        int mono = 1;
        for (int k = 1; k < 5; k++) mono &= lod_lvl[k] >= lod_lvl[k - 1];
        CHECK(mono && lod_lvl[0] < lod_lvl[4] && vcpe_lod_stat_drawn < vcpe_lod_stat_full && vcpe_stat_tris == vcpe_lod_stat_drawn,
              "lod: levels %d..%d by distance, %d of %d triangles drawn", lod_lvl[0], lod_lvl[4], vcpe_lod_stat_drawn, vcpe_lod_stat_full);
    }
}

static void scene_draw(void)
{
    int ep = vcpe_bind_epoch;
    /* a busy frame: fills, sprites of the debug font, button icons, a strip - the same with and without batching */
    for (int i = 0; i < 40; i++)
        vcpe_fill((i * 37) % 460, (i * 53) % 250, 20, 12, 0xFF000000u | (uint32_t)(i * 2654435761u));
    vcpe_hud_text_shadow(10, 10, "VCPE " VCPE_VERSION " SELFTEST\nbuilt-in 8x8 font", 0xFFFFFFFF, 1);
    vcpe_hud_text(10, 34, "SCALE 2", 0xFF40D0FF, 2);
    float x = 10;
    for (int b = VCPE_BTN_CROSS; b <= VCPE_BTN_RIGHT; b++) {
        float w = vcpe_hud_button_width(b, 9);
        vcpe_hud_button(b, x + w / 2, 80, 9, 255);
        x += w + 6;
    }
    vcpe_hud_prompt(10, 100, VCPE_BTN_CROSS, "JUMP", 0xFFFFFFFF);
    vcpe_hud_prompt(110, 100, VCPE_BTN_CIRCLE, "BACK", 0xFFFFFFFF);
    vcpe_hud_prompt(210, 100, VCPE_BTN_START, "PAUSE", 0xFFFFFFFF);
    for (int i = 0; i < 300; i++) {
        char ch[2] = {(char)('A' + i % 26), 0};
        vcpe_hud_text(10 + (i % 50) * 9, 130 + (i / 50) * 10, ch, 0xFF000000u | (uint32_t)(i * 40503u), 1);
    }
    vcpe_hud_printf(10, 200, 0xFFFFFF80, "sin(30)=%.4f  %s", vcpe_sin_deg(30), "static line");
    static int checked;
    if (!checked++)
        CHECK(vcpe_bind_epoch - ep == 1, "4.0: 300 HUD strings bound the font %d time(s)", vcpe_bind_epoch - ep);
}

int main(int argc, char **argv)
{
    vcpe_sys_init(argc, argv);
    vcpe_log_open("SELFTEST.LOG");
    vcpe_log("VCPE %s selftest, heap %dKB", VCPE_VERSION, vcpe_heap_kb());
    vcpe_require_64mb("VCPE selftest");
    vcpe_require_file("GAME.PAK", "VCPE selftest", "Run tests/selftest/run.sh.");
    vcpe_pak_open_checked("GAME.PAK", "VCPE selftest", PAK_MAGIC, PAK_SIZE, PAK_STAMP);
    char hello[16] = {0};
    CHECK(vcpe_pak_read(PAK_HELLO_OFF, hello, PAK_HELLO_SIZE) == 0 && !strcmp(hello, "hello vcpe"), "checked PAK read '%s'", hello);

    /* math */
    CHECK(vcpe_absf(vcpe_sin_deg(30) - 0.5f) < 1e-5f && vcpe_absf(vcpe_cos_deg(60) - 0.5f) < 1e-5f, "sin/cos table");
    CHECK(vcpe_absf(vcpe_direction_deg(0, 0, 10, -10) - 45) < 0.01f, "direction 45 = %.4f", vcpe_direction_deg(0, 0, 10, -10));
    CHECK(vcpe_absf(vcpe_sqrt(2) - 1.41421356f) < 1e-6f, "sqrt.s");
    vcpe_seed(7);
    int r1 = vcpe_irand(1000);
    vcpe_seed(7);
    CHECK(vcpe_irand(1000) == r1, "rng repeats from a seed");

    /* saves: write, read back, newest wins, version mismatch */
    struct { int a, b; char s[12]; } sv = {1, 2, "first"}, ld;
    vcpe_save_erase("TEST.SAV");
    CHECK(vcpe_save_read("TEST.SAV", &ld, sizeof ld, 3) == -1, "no save yet");
    CHECK(vcpe_save_write("TEST.SAV", &sv, sizeof sv, 3) == 0, "save written");
    sv.a = 5; strcpy(sv.s, "second");
    CHECK(vcpe_save_write("TEST.SAV", &sv, sizeof sv, 3) == 0, "second save written");
    memset(&ld, 0, sizeof ld);
    CHECK(vcpe_save_read("TEST.SAV", &ld, sizeof ld, 3) == 0 && ld.a == 5 && !strcmp(ld.s, "second"), "newest save read (%d %s)", ld.a, ld.s);
    CHECK(vcpe_save_read("TEST.SAV", &ld, sizeof ld, 4) == -2, "other version refused");

    /* audio: voice allocation */
    static const VcpeSound none[1] = {{"none", 0, 0, 0}};
    int a = vcpe_audio_init("MUSIC.PAK", 8, PAK_HELLO_OFF, 64, none, 1, none);
    CHECK(a == 0, "audio init %d", a);

    vcpe_gfx_init();

    /* memory (3.4): the model, the volatile partition, bundles sharing a slab and placed in given memory */
    int hk = vcpe_heap_kb();
    CHECK(!vcpe_mem_32mb() && hk > 36000 && hk < 64000, "64 MB model, heap %d KB", hk);
    uint32_t vs;
    uint8_t *vm = vcpe_volatile_mem(&vs);
    CHECK(vm && vs == 4u << 20 && vcpe_volatile_mem(0) == vm, "volatile partition %p, %u bytes", vm, (unsigned)vs);
    static const VcpeBundle bt[2] = {{PAK_HELLO_OFF, PAK_HELLO_SIZE, 0, 0}, {PAK_HELLO_OFF + 6, 16, 0, 0}};
    vcpe_gfx_tables(0, bt, 2);
    CHECK(vcpe_bundle_slab(0, 1) == 0 && vcpe_bundle_load(1) == 0 && !memcmp(vcpe_bundle_mem(1), "vcpe", 4) &&
          vcpe_bundle_load(0) == 0 && !vcpe_bundle_loaded(1) && vcpe_bundle_mem(0) == vcpe_bundle_mem(1 - 1) &&
          !memcmp(vcpe_bundle_mem(0), "hello vcpe", 10), "slab: bundles take turns in one block");
    vcpe_bundle_unload(0);
    CHECK(vcpe_bundle_load_at(0, vm, vs, 1) == 0 && vcpe_bundle_mem(0) == vm && !memcmp(vm, "hello vcpe", 10) &&
          vcpe_bundle_load_at(1, vm, 8, 0) == -1, "bundle placed in the volatile partition (and a too small block refused)");
    vcpe_bundle_unload(0);
    CHECK(vcpe_mem_used_kb() >= 0 && vcpe_mem_peak_kb() >= vcpe_mem_used_kb(), "heap use %d KB, peak %d KB", vcpe_mem_used_kb(), vcpe_mem_peak_kb());

    /* 4.0: slab kept when it fits, arena, memory report, texture identity, coroutine pool, resume */
    CHECK(vcpe_bundle_slab(0, 1) == 0 && vcpe_bundle_load(1) == 0, "slab again");
    uint8_t *slab1 = vcpe_bundle_mem(1);
    vcpe_bundle_unload(1);
    CHECK(vcpe_bundle_slab(1, 1) == 0 && vcpe_bundle_load(1) == 0 && vcpe_bundle_mem(1) == slab1, "slab block reused when the new range fits");
    vcpe_bundle_unload(1);
    VcpeArena ar;
    CHECK(vcpe_arena_init(&ar, 0, 4096) == 0, "arena init");
    void *a1 = vcpe_arena_alloc(&ar, 100, 4), *a2 = vcpe_arena_alloc(&ar, 10, 64);
    uint32_t mk = vcpe_arena_mark(&ar);
    void *a3 = vcpe_arena_alloc(&ar, 4000, 4);
    vcpe_arena_release(&ar, mk);
    void *a4 = vcpe_arena_alloc(&ar, 8, 4);
    vcpe_arena_reset(&ar);
    void *a5 = vcpe_arena_alloc(&ar, 8, 4);
    CHECK(a1 && a2 && ((uintptr_t)a2 & 63) == 0 && !a3 && a4 && a5 == a1 && ar.peak >= 128, "arena: bump, align, overflow refused, mark/release, reset");
    vcpe_arena_free_all(&ar);
    char mr[256];
    int ml = vcpe_mem_report(mr, sizeof mr, "boot");
    vcpe_log("%s", mr);
    CHECK(ml > 20 && strstr(mr, "largest free") && vcpe_mem_largest_free_kb() > 30000, "memory report line");
    static int key_a, key_b;
    vcpe_gfx_begin(0);
    CHECK(!vcpe_tex_bound(&key_a), "new frame: nothing claimed");
    vcpe_tex_claim(&key_a);
    int tb1 = vcpe_tex_bound(&key_a) && !vcpe_tex_bound(&key_b);
    vcpe_unbind();
    int tb2 = !vcpe_tex_bound(&key_a);
    vcpe_tex_claim(&key_b);
    vcpe_gfx_state_reset();
    CHECK(tb1 && tb2 && !vcpe_tex_bound(&key_b), "texture identity: claim, unbind and state reset forget it");
    vcpe_gfx_end();
    CHECK(vcpe_resume_pending() == 0 && vcpe_bundles_reload() == 0, "no reload pending without a sleep");
    CHECK(vcpe_absf(vcpe_dt() - 1 / 59.94f) < 1e-6f || !vcpe_autotest, "vcpe_dt is one vblank in autotest");
    co_runs = 0;
    int c1 = vcpe_co_start(co_fn, 0);
    vcpe_co_update(0.016f);
    vcpe_co_update(0.016f);
    int c2 = vcpe_co_start(co_fn, 0);
    vcpe_co_update(0.016f);
    vcpe_co_update(0.016f);
    CHECK(c1 >= 0 && c2 >= 0 && (c1 & 0xFF) == (c2 & 0xFF) && c1 != c2 && co_runs == 4 && !vcpe_co_running(),
          "coroutine slot and its parked thread reused (%d runs)", co_runs);
    vcpe_co_shutdown();

    /* 4.1 ultra tools on the PSP: tuning file on the memory stick, events from the engine, blackboard */
    static float tspeed = 1;
    static int ev_seen[8], ev_sum;
    vcpe_tune_float("test.speed", &tspeed, 0, 5);
    vcpe_tune_file("TUNE.TXT", 1);
    char tp[300];
    vcpe_path(tp, sizeof tp, "TUNE.TXT");
    FILE *tf = fopen(tp, "w");
    if (tf) { fputs("test.speed = 9   # clamped to 5\n", tf); fclose(tf); }
    vcpe_ev_on(VCPE_EV_ANY, ev_count, ev_seen);
    vcpe_ev_on(VCPE_EV_GAME, ev_add, &ev_sum);
    vcpe_ev_post(VCPE_EV_GAME, 40, 0, 0);
    vcpe_ev_post_thread(VCPE_EV_GAME, 2, 0, 0);
    vcpe_bb_set_int("lives", 3);
    vcpe_ultra_update();
    CHECK(tspeed == 5 && ev_seen[VCPE_EV_TUNE_CHANGED] == 1, "ultra: tune file on the stick read and clamped (%g)", tspeed);
    CHECK(ev_sum == 42 && ev_seen[VCPE_EV_BB_CHANGED] == 1 && vcpe_bb_int("lives", 0) == 3, "ultra: events and blackboard (%d)", ev_sum);
    int pid = vcpe_prof_id("busy");
    for (int k = 0; k < 40; k++) { vcpe_prof_begin(pid); sceKernelDelayThread(500); vcpe_prof_end(pid); vcpe_ultra_update(); }
    char pl[80]; vcpe_prof_line(pl, sizeof pl); vcpe_log("%s", pl);
    CHECK(vcpe_prof_us(pid) > 200, "ultra: profiler on the PSP clock (%d us)", vcpe_prof_us(pid));
    remove(tp);

    /* 4.1 reservations on the PSP: 3 at once for 2 clients, 2 server threads */
    int c_ai = vcpe_client_add("ai", 70), c_ld = vcpe_client_add("loader", 30);
    vcpe_servers_start(2, 0);
    int r[3] = {vcpe_reserve(c_ld, st_job, 0, st_done, 0), vcpe_reserve(c_ai, st_job, 0, st_done, 0),
                vcpe_reserve(c_ai, st_job, 0, st_done, 10)};
    sceKernelDelayThread(5000);   /* the main thread sleeps: the servers take the two best reservations */
    int ld_waiting = vcpe_reserve_state(r[0]) == VCPE_RES_PENDING;
    vcpe_jobs_wait_all();
    vcpe_jobs_update();
    CHECK(ld_waiting && st_done_n == 3 && vcpe_reserve_server(r[1]) != vcpe_reserve_server(r[2]),
          "jobs: 3 reservations, 2 clients, 2 servers (loader waited, %d done)", st_done_n);
    vcpe_jobs_shutdown();
    /* 4.1 compact loading: the selftest PAK's "hello" section is not a pack: the header check refuses it */
    CHECK(vcpe_pack_pool(0, 64 * 1024, 2) > 0 && vcpe_pack_load(PAK_HELLO_OFF, PAK_HELLO_SIZE, 50) == -1,
          "pack: pool made, a non-pack block refused");
    vcpe_pack_pool_free();
    /* 4.1 LOD meshes: the sphere from the PAK (tools/vcpe_lod.py), kept for scene 3 */
    static uint8_t lod_blob[PAK_LODSPHERE_SIZE] __attribute__((aligned(16)));
    CHECK(vcpe_pak_read(PAK_LODSPHERE_OFF, lod_blob, PAK_LODSPHERE_SIZE) == 0 && vcpe_lod_open(&lod_m, lod_blob, PAK_LODSPHERE_SIZE) == 0 &&
          lod_m.nlevels >= 4, "lod: mesh opened, %d vertices, %d levels", lod_m.nverts, lod_m.nlevels);

    vcpe_pad_init(binds, sizeof binds / sizeof binds[0], VCPE_STICK_DPAD);
    vcpe_mouse_init(0);
    vcpe_mouse_poi_source(slots);
    VcpeTestHooks hooks = {vcpe_pad_button, both, 0, vcpe_mouse_busy, 0};
    if (vcpe_test_load(&hooks))
        vcpe_seed(1);
    while (vcpe_running) {
        vcpe_pad_update();
        vcpe_mouse_update();
        vcpe_perf_update_begin();
        int ck = vcpe_mouse_clicked();
        m_clicks += ck == 1;
        m_dclicks += ck == 2;
        m_drags += vcpe_mouse_drag_started();
        if (vcpe_mouse_dropped()) { m_drops++; m_drop_x = vcpe_mouse_x; m_drop_y = vcpe_mouse_y; }
        m_rpress += !!(vcpe_mouse_pressed() & VCPE_MB_RIGHT);
        m_wheel += vcpe_mouse_wheel();
        m_warps += vcpe_mouse_warped();
        presses += __builtin_popcount(vcpe_pad_pressed() & (B_OK | B_BACK));
        frame++;
        vcpe_perf_update_end();
        vcpe_gfx_begin(0xFF302018);
        if (scene == 0)
            scene_draw();
        else if (scene == 2)
            mouse_scene_draw();
        else if (scene == 3)
            lod_scene_draw();
        else {
            vcpe_hud_printf(10, 10, 0xFFFFFFFF, "frame %d presses %d", frame, presses);
            vcpe_hud_perf();
        }
        vcpe_gfx_end();
        vcpe_test_frame_end();
    }
    vcpe_gfx_shutdown();
    vcpe_log_close();
    sceKernelExitGame();
    return 0;
}
