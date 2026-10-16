/* VCPE Blank Engine: the engine built with no game at all. It boots on any PSP and shows a small UI over the
 * engine's own picture (engineassets/staring.jpg, baked by tools/mkblankimg.py) with what the engine sees: version,
 * console model, memory, clocks, battery, firmware, frame timing. It needs no data files, so it is the quickest
 * way to check that a toolchain, an engine change or a console works at all.
 *
 * Pages (d-pad left/right):
 *   SYSTEM  technical information, live
 *   JOBS    the reservation demo: 2 clients (ai 70, loader 30), 2 servers, 3 reservations booked at once with X;
 *           every reservation's state, server and priority is shown as it runs
 *   ULTRA   the event bus and the blackboard: events as they arrive, values changed with the d-pad
 *   PERF    the engine's perf overlay and statistics
 * HOME > Exit quits. Runs autotest scripts like any VCPE game (AUTOTEST.TXT next to the EBOOT). */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe.h"
#include "vcpe_jobs.h"
#include "vcpe_pack.h"
#include "vcpe_spatial.h"
#include "blankimg.h"

PSP_MODULE_INFO("VCPE Blank Engine", PSP_MODULE_USER, VCPE_VERSION_MAJOR, VCPE_VERSION_MINOR);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

enum { PAGE_SYSTEM, PAGE_JOBS, PAGE_ULTRA, PAGE_PERF, PAGE_ADAPT, PAGE_BENCH, PAGE_SAVES, NPAGES };
static const char *page_names[NPAGES] = {"SYSTEM", "JOBS", "ULTRA", "PERF", "ADAPT", "BENCH", "SAVES"};
static int page, frames;

#define WHITE 0xFFFFFFFF
#define GREY 0xFFB0B0B0
#define ACCENT 0xFF40C8FF   /* 0xAABBGGRR: orange */
#define PANEL 0xB0101018

/* ------------------------------------------------------------------ background picture */
static void draw_background(void)
{
    vcpe_blend(0);
    vcpe_tex_white(0);
    vcpe_filter(1);
    if (!vcpe_tex_bound(blank_img)) {
        vcpe_tex_claim(blank_img);
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, blank_clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, 512, 512, 512, blank_img);   /* 272 rows stored; rows below are never sampled */
        vcpe_tex_flush();
    }
    vcpe_quad(0, 0, VCPE_SCR_W, VCPE_SCR_H, 0, 0, VCPE_SCR_W, VCPE_SCR_H, WHITE);
    vcpe_gfx_flush();
}

static void panel(float x, float y, float w, float h) { vcpe_fill(x, y, w, h, PANEL); }

/* ------------------------------------------------------------------ SYSTEM */
static int heap_kb, heap_age;
static const char *modules = "sys pak gfx test hud math pad mem load log ultra jobs pack adapt";

static void page_system(void)
{
    if (heap_age-- <= 0) { heap_kb = vcpe_heap_kb(); heap_age = 60; }   /* the probe allocates: once a second */
    int fw = sceKernelDevkitVersion();
    int batt = scePowerIsBatteryExist() ? scePowerGetBatteryLifePercent() : -1;
    char t[96];
    float x = 14, y = 40;
    panel(8, 34, 300, 196);
    vcpe_hud_text_shadow(x, y, "TECHNICAL INFORMATION", ACCENT, 1); y += 14;
    snprintf(t, sizeof t, "Engine      VCPE %s (blank build)", VCPE_VERSION); vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Built       %s %s", __DATE__, __TIME__); vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Console     %s", vcpe_mem_32mb() ? "PSP-1000 (32 MB)" : "PSP-2000/3000/Go (64 MB)");
    vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Firmware    %x.%02x", (fw >> 24) & 0xFF, (fw >> 16) & 0xFF); vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "CPU / bus   %d / %d MHz", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
    vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    if (batt >= 0) snprintf(t, sizeof t, "Battery     %d%%%s", batt, scePowerIsPowerOnline() ? " (charging)" : "");
    else snprintf(t, sizeof t, "Battery     none (AC only)");
    vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Heap        %d KB largest free, %d KB used", heap_kb, vcpe_mem_used_kb());
    vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Volatile    %s", vcpe_volatile_mem(0) ? "4096 KB locked" : "not available");
    vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Frame       %d fps, %d vblanks late, DL %d KB", vcpe_stat_fps, vcpe_stat_vblank_misses, vcpe_stat_dlist_kb);
    vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 11;
    snprintf(t, sizeof t, "Uptime      %d s", frames / 60); vcpe_hud_text_shadow(x, y, t, WHITE, 1); y += 15;
    vcpe_hud_text_shadow(x, y, "Modules", ACCENT, 1); y += 11;
    vcpe_hud_text_shadow(x, y, modules, GREY, 1);
}

/* ------------------------------------------------------------------ JOBS: 3 reservations, 2 clients, 2 servers */
static int cl_ai, cl_loader, res_id[3], res_client[3], res_urg[3], booked, finished;
static volatile int work_ms[3];
static const char *res_label[3] = {"loader: unpack level", "ai: plan path 1", "ai: plan path 2 (urgent)"};

/* runs on a server thread: busy work that sleeps in small steps, so the main thread keeps its 60 fps */
static void demo_job(void *arg)
{
    volatile int *ms = arg;
    for (*ms = 0; *ms < 600; *ms += 20)
        sceKernelDelayThread(20000);
}

static void demo_done(int id, void *arg) { (void)id; (void)arg; finished++; vcpe_ev_post(VCPE_EV_GAME, finished, 0, 0); }

static void jobs_book(void)
{
    if (booked && vcpe_jobs_pending())
        return;
    int cl[3] = {cl_loader, cl_ai, cl_ai}, urg[3] = {0, 0, 10};
    for (int i = 0; i < 3; i++) {   /* all three at the same time */
        res_client[i] = cl[i];
        res_urg[i] = urg[i];
        res_id[i] = vcpe_reserve(cl[i], demo_job, (void *)&work_ms[i], demo_done, urg[i]);
    }
    booked = 1;
}

static void page_jobs(void)
{
    static const char *st[] = {"-", "PENDING", "RUNNING", "DONE"};
    static const uint32_t col[] = {GREY, 0xFF60A0FF, 0xFF40FF80, WHITE};
    char t[96];
    panel(8, 34, 464, 196);
    vcpe_hud_text_shadow(14, 40, "RESERVATIONS: 3 booked at once, 2 clients, 2 servers", ACCENT, 1);
    snprintf(t, sizeof t, "clients  ai = %d   loader = %d     servers = %d", 70, 30, vcpe_servers_count());
    vcpe_hud_text_shadow(14, 56, t, WHITE, 1);
    for (int i = 0; i < 3; i++) {
        float y = 76 + i * 30;
        int s = booked ? vcpe_reserve_state(res_id[i]) : 0, sv = booked ? vcpe_reserve_server(res_id[i]) : -1;
        int done = booked && (s == 0 || s == 3);
        snprintf(t, sizeof t, "%-26s prio %d+%d  %s%s", res_label[i], res_client[i] == cl_ai ? 70 : 30, res_urg[i],
                 done ? "DONE" : st[s], s == 2 ? (sv == 0 ? " on server 0" : " on server 1") : "");
        vcpe_hud_text_shadow(14, y, t, done ? WHITE : col[s], 1);
        float p = done ? 1 : work_ms[i] / 600.0f;
        vcpe_fill(14, y + 11, 300, 4, 0xFF303030);
        vcpe_fill(14, y + 11, 300 * p, 4, done ? 0xFF40FF80 : 0xFF40C8FF);
    }
    vcpe_jobs_line(t, sizeof t);
    t[56] = 0;   /* the panel holds 56 characters */
    vcpe_hud_text_shadow(14, 172, t, GREY, 1);
    vcpe_hud_text_shadow(14, 186, "Both AI reservations outrank the loader, so the two", GREY, 1);
    vcpe_hud_text_shadow(14, 196, "servers take them first; the loader gets the next one.", GREY, 1);
    vcpe_hud_prompt(14, 212, VCPE_BTN_CROSS, "BOOK 3 RESERVATIONS", WHITE);
}

/* ------------------------------------------------------------------ ULTRA: events and blackboard */
static char ev_log[6][48];
static int ev_n;
static void log_event(const VcpeEvent *e, void *u)
{
    (void)u;
    static const char *names[] = {"any", "RESUME", "LOAD_BEGIN", "LOAD_END", "LATE_FRAME", "LOW_MEMORY", "BB_CHANGED",
                                  "TUNE_CHANGED", "JOB_DONE"};
    char t[48];
    if (e->id == VCPE_EV_BB_CHANGED || e->id == VCPE_EV_TUNE_CHANGED)
        snprintf(t, sizeof t, "%-12s %s", names[e->id], e->id == VCPE_EV_BB_CHANGED ? (const char *)e->p : "");
    else if (e->id < 9) snprintf(t, sizeof t, "%-12s i=%d", names[e->id], (int)e->i);
    else snprintf(t, sizeof t, "GAME+%-7d i=%d", e->id - VCPE_EV_GAME, (int)e->i);
    if (ev_n == 6) { memmove(ev_log, ev_log + 1, sizeof ev_log[0] * 5); ev_n = 5; }
    snprintf(ev_log[ev_n++], sizeof ev_log[0], "%s", t);
}

static void bb_line(const char *text)
{
    static float y;
    if (!text) { y = 0; return; }
    if (y < 60) vcpe_hud_text_shadow(250, 150 + y, text, WHITE, 1);
    y += 10;
}

static void page_ultra(void)
{
    panel(8, 34, 464, 196);
    vcpe_hud_text_shadow(14, 40, "EVENT BUS (newest at the bottom)", ACCENT, 1);
    for (int i = 0; i < ev_n; i++)
        vcpe_hud_text_shadow(14, 56 + i * 11, ev_log[i], WHITE, 1);
    vcpe_hud_text_shadow(250, 136, "BLACKBOARD", ACCENT, 1);
    bb_line(0);
    vcpe_bb_dump(bb_line);
    vcpe_hud_prompt(14, 212, VCPE_BTN_CROSS, "POST EVENT", WHITE);
    vcpe_hud_prompt(160, 212, VCPE_BTN_UP, "COUNTER +1", WHITE);
}

/* ------------------------------------------------------------------ adaptive budgets */
static void *demo_cache;
static uint32_t demo_maintenance(int phase, uint32_t budget_us, void *user)
{
    (void)user;
    if (budget_us && phase != VCPE_ADAPT_PLAY && demo_cache) {
        free(demo_cache); demo_cache = 0; return 64 * 1024;
    }
    return 0;
}
static void demo_feed(uint32_t us, int kb, int n)
{
    if (n > 3600) n = 3600;
    while (n-- > 0) vcpe_adapt_sample(us, kb);
    char line[256]; vcpe_adapt_line(line, sizeof line); printf("%s\n", line);
}
static void demo_loading(void)
{
    if (!demo_cache) demo_cache = malloc(64 * 1024);
    uint32_t passes = vcpe_adapt_stats()->maintenance_runs;
    vcpe_loading_begin(0);
    /* A short synthetic load exercises periodic work as well as begin/end, without flashing the loading screen. */
    for (int i=0; i<12; i++) { sceKernelDelayThread(10000); vcpe_loading_tick(); }
    vcpe_loading_end();
    printf("%s adaptive loading progress work (%u passes)\n",
        vcpe_adapt_stats()->maintenance_runs - passes > 2 ? "PASS" : "FAIL",
        (unsigned)(vcpe_adapt_stats()->maintenance_runs - passes));
    char line[256]; vcpe_adapt_line(line, sizeof line); printf("%s\n", line);
}
static uint32_t bench_candidates,bench_full,bench_us,bench_freed;
static int bench_ok;
static void bench_run(void)
{
    uint32_t start=sceKernelGetSystemTimeLow();
    VcpeGrid grid={0};
    bench_ok=vcpe_grid_begin(&grid,512)==0; bench_candidates=0; bench_full=512*511/2;
    if (bench_ok) {
        for (int i=0;i<512;i++) vcpe_grid_set(&grid,i,(i%32)*1.5f,(i/32)*1.5f,.4f,.4f);
        for (int i=0;i<512;i++) {
            vcpe_grid_query(&grid,i);
            for (int j=vcpe_grid_next(&grid,i+1);j<512;j=vcpe_grid_next(&grid,j+1)) bench_candidates++;
        }
        vcpe_grid_free(&grid);
    }
    VcpeStore store;
    bench_ok=bench_ok && vcpe_store_init(&store,32,32)==0;
    uint32_t ids[128]; void *records[128]; int made=0;
    if (bench_ok) {
        for (;made<128;made++) {
            if (vcpe_store_add(&store,&ids[made],&records[made])<0) { bench_ok=0; break; }
            *(uint32_t *)records[made]=(uint32_t)made;
        }
        for (int i=0;i<made;i++) if (vcpe_store_get(&store,ids[i])!=records[i] || *(uint32_t *)records[i]!=(uint32_t)i) bench_ok=0;
        for (int i=8;i<made;i++) vcpe_store_remove(&store,ids[i]);
        bench_freed=vcpe_store_trim(&store); vcpe_store_free(&store);
    }
    bench_us=sceKernelGetSystemTimeLow()-start;
    bench_ok=bench_ok && bench_candidates<bench_full && bench_freed>0;
    printf("%s VCPE %s engine bench: candidates %u/%u, stable store trim %u bytes, %u us\n",
        bench_ok?"PASS":"FAIL",VCPE_VERSION,(unsigned)bench_candidates,(unsigned)bench_full,(unsigned)bench_freed,(unsigned)bench_us);
}
static void page_bench(void)
{
    static VcpeDiagStats stats;
    if (frames%30==0 || !stats.samples) vcpe_diag_stats(&stats);
    panel(8,34,464,206);
    vcpe_hud_text(14,42,"VCPE " VCPE_VERSION " ENGINE DIAGNOSTICS",ACCENT,1);
    vcpe_hud_printf(14,62,WHITE,"Frame avg / p95  %u / %u us",(unsigned)stats.average_us,(unsigned)stats.p95_us);
    vcpe_hud_printf(14,74,WHITE,"Frame p99 / max  %u / %u us",(unsigned)stats.p99_us,(unsigned)stats.worst_us);
    vcpe_hud_printf(14,86,WHITE,"CPU / GPU wait   %u / %u us",(unsigned)stats.last_cost[0],(unsigned)stats.last_cost[1]);
    vcpe_hud_printf(14,98,WHITE,"Asset reads      %u us",(unsigned)stats.last_cost[2]);
    vcpe_hud_printf(14,110,WHITE,"Allocation fails %u",(unsigned)stats.failures);
    vcpe_hud_printf(14,134,WHITE,"Grid candidates  %u / %u",(unsigned)bench_candidates,(unsigned)bench_full);
    vcpe_hud_printf(14,146,WHITE,"Stable pool trim %u bytes",(unsigned)bench_freed);
    vcpe_hud_printf(14,158,WHITE,"Benchmark time   %u us",(unsigned)bench_us);
    vcpe_hud_text(14,182,bench_full?(bench_ok?"ENGINE MICROBENCH PASSED":"ENGINE MICROBENCH FAILED"):"X: run engine microbench",ACCENT,1);
    vcpe_hud_text(14,206,"Includes CPU, GPU-wait and I/O counters.",GREY,1);
    vcpe_hud_text(14,218,"Microbench timing is specific to this device.",GREY,1);
}
/* Native save demo uses a dedicated DISC_ID, independent of any demake. */
static VcpeSavedata save_demo = {"UCJS19050", "DEMO00", "VCPE 5.0 Save Demo", "Engine test slot",
    "Made with VCPE 5.0. Native savedata utility demonstration.", 0, 1};
static uint32_t save_counter;
static int save_result;
static void save_write(void) { save_counter++; save_result = vcpe_savedata_write(&save_demo, &save_counter, sizeof save_counter, 1); }
static void save_read(void) { uint32_t n; save_result = vcpe_savedata_read(&save_demo, &save_counter, sizeof save_counter, &n, 1); }
static void save_check(void)
{
    VcpeSavedata s = save_demo; s.dialog = 0;
    uint8_t a[513], b[513]; uint32_t n = 99; int ok = 1;
    for (unsigned i=0; i<sizeof a; i++) a[i] = (uint8_t)(i * 37);
    /* A registered volatile bundle must reload after the firmware borrows the partition. */
    char pakpath[512]; vcpe_path(pakpath, sizeof pakpath, "SAVETEST.PAK");
    FILE *pak = fopen(pakpath, "wb");
    int pak_ok = pak && fwrite(a, 1, 64, pak) == 64;
    if (pak) pak_ok &= fclose(pak) == 0;
    uint32_t volatile_size = 0;
    uint8_t *vol = vcpe_volatile_mem(&volatile_size);
    static const VcpeBundle bundle = {0, 64, 0, 1};
    static const VcpePage texture = {8,8,GU_PSM_T8,0,0,0,0,0};
    vcpe_gfx_tables(&texture, &bundle, 1);
    int registered = pak_ok && vol && volatile_size >= 64 && vcpe_pak_open(pakpath) >= 0 &&
        vcpe_bundle_load_at(0, vol, volatile_size, 1) == 0;
    ok &= registered;
    int r = vcpe_savedata_write(&s, a, sizeof a, 7); ok &= r == 0;
    memset(b, 0, sizeof b);
    r = vcpe_savedata_read(&s, b, sizeof b, &n, 7); ok &= r == 0 && n == sizeof a && !memcmp(a,b,sizeof a);
    memset(b, 0xA5, sizeof b); n = 99;
    r = vcpe_savedata_read(&s, b, sizeof b, &n, 8); ok &= r == VCPE_SAVEDATA_CORRUPT && n == 99 && b[0] == 0xA5;
    r = vcpe_savedata_read(&s, b, 4, &n, 7); ok &= r != 0 && n == 99 && b[0] == 0xA5;
    s.slot = "../BAD"; ok &= vcpe_savedata_write(&s, a, sizeof a, 7) == VCPE_SAVEDATA_INVALID;
    s.slot = "IMPORT";
    ok &= vcpe_save_write("MIGRATE.DAT", a, sizeof a, 7) == 0;
    ok &= vcpe_savedata_import(&s, "MIGRATE.DAT", sizeof a, 7) == 0;
    ok &= vcpe_savedata_read(&s, b, sizeof b, &n, 7) == 0 && !memcmp(a,b,sizeof a);
    ok &= vcpe_save_read("MIGRATE.DAT", b, sizeof b, 7) == 0 && !memcmp(a,b,sizeof a);
    if (registered) {
        memset(vol,0,64); /* prove the next frame re-reads, independent of what this firmware overwrote */
        vcpe_gfx_begin(0xFF000000); vcpe_gfx_end(); vcpe_gfx_present();
        ok &= !memcmp(vol,a,64) && !vcpe_resume_pending();
        vcpe_bundle_unload(0);
    }
    vcpe_gfx_tables(0,0,0); vcpe_pak_close();
    save_result = ok ? 0 : -1;
    printf("%s native savedata: roundtrip, version/capacity rejection, import, originals and volatile bundle reload\n",ok?"PASS":"FAIL");
    page = PAGE_SAVES;
}
static void loading_preview(void)
{
    vcpe_loading_show_after(0); vcpe_loading_redraw_ms(16); vcpe_loading_begin(120);
    for (int i=0; i<12; i++) {
        int st = vcpe_step_begin("Preparing engine resource %02d", i + 1);
        sceKernelDelayThread(25000); vcpe_step_end(st, 1); vcpe_loading_add(10);
    }
    vcpe_test_snap("loading0");
    for (int i=0; i<20; i++) { sceKernelDelayThread(25000); vcpe_loading_tick(); }
    vcpe_test_snap("loading1");
    vcpe_loading_end(); vcpe_loading_show_after(150); vcpe_loading_redraw_ms(50);
    printf("%s loader default: %d rows; animation captures saved\n",vcpe_loading_log_height()==8?"PASS":"FAIL",vcpe_loading_log_height());
}
static void page_saves(void)
{
    panel(8,34,464,206);
    vcpe_hud_text(14,42,"NATIVE PSP SAVEDATA",ACCENT,1);
    vcpe_hud_printf(14,64,WHITE,"Counter %u   Last result 0x%08X",(unsigned)save_counter,(unsigned)save_result);
    vcpe_hud_text(14,86,"PSP/SAVEDATA/UCJS19050DEMO00",WHITE,1);
    vcpe_hud_text(14,106,"Title, slot name and icon appear in the XMB",GREY,1);
    vcpe_hud_text(14,118,"Saved Data Utility after leaving the app.",GREY,1);
    vcpe_hud_prompt(14,146,VCPE_BTN_CROSS,"SAVE (native dialog)",WHITE);
    vcpe_hud_prompt(14,164,VCPE_BTN_CIRCLE,"LOAD (native dialog)",WHITE);
    vcpe_hud_prompt(14,182,VCPE_BTN_TRIANGLE,"PREVIEW LOADING SCREEN",WHITE);
    vcpe_hud_text(14,210,"Result 0 means success; firmware errors stay visible.",GREY,1);
}
static int blank_command(const char *cmd, const char *arg, int num, int num2, const char *line, unsigned *held)
{
    if (!strcmp(cmd,"savecheck")) { save_check(); return VCPE_CMD_NEXT; }
    if (!strcmp(cmd,"loadpreview")) { loading_preview(); return VCPE_CMD_NEXT; }
    if (!strcmp(cmd,"saveui")) { page=PAGE_SAVES; save_write(); return VCPE_CMD_NEXT; }
    if (!strcmp(cmd,"bench")) { page=PAGE_BENCH; bench_run(); return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "adapt")) { vcpe_adapt_enable(atoi(arg)); page = PAGE_ADAPT; return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "adaptfeed")) {
        unsigned us; int kb, n;
        if (sscanf(line, "%*s %u %d %d", &us, &kb, &n) == 3) demo_feed(us, kb, n);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(cmd, "adaptcheck")) {
        int scale = vcpe_adapt_stats()->scale256;
        int expected = atoi(arg);
        printf("%s adapt scale %d expected %d\n", scale == expected ? "PASS" : "FAIL", scale, expected);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(cmd, "adaptload")) { demo_loading(); return VCPE_CMD_NEXT; }
    return vcpe_ultra_command(cmd, arg, num, num2, line, held);
}
static void page_adapt(void)
{
    const VcpeAdaptStats *s = vcpe_adapt_stats();
    static const char *why[] = {"stable", "slow frames", "low memory", "recovering", "CPU bound", "I/O bound", "audio pressure"};
    panel(8, 34, 464, 206);
    vcpe_hud_text_shadow(14, 42, "AUTOMATIC ENGINE BUDGETS", ACCENT, 1);
    vcpe_hud_printf(14, 60, WHITE, "Controller     %s", s->enabled ? "ON" : "OFF");
    vcpe_hud_printf(14, 72, WHITE, "Draw quality   %d%%", s->scale256 * 100 / 256);
    vcpe_hud_printf(14, 84, WHITE, "Reason         %s", why[s->reason]);
    vcpe_hud_printf(14, 96, WHITE, "Frame average  %u us", (unsigned)s->ema_us);
    vcpe_hud_printf(14, 108, WHITE, "Free estimate  %d KB", s->available_kb);
    vcpe_hud_printf(14, 120, WHITE, "Reclaimed      %u KB", (unsigned)(s->reclaimed_bytes / 1024));
    vcpe_hud_printf(14, 132, WHITE, "Micro work     %u us", (unsigned)s->last_work_us);
    vcpe_hud_printf(14, 144, WHITE, "Budget overruns %u", (unsigned)s->budget_overruns);
    vcpe_hud_text(310, 60, "DRAW BUDGET", GREY, 1);
    int scale = vcpe_adapt_effect_scale();
    for (int i=0; i<64; i++)
        if ((i+1)*scale/256 != i*scale/256)
            vcpe_fill(310+(i%8)*17, 78+(i/8)*10, 4, 4, ACCENT);
    vcpe_hud_text(14, 168, "X: toggle   Up: simulate overload", GREY, 1);
    vcpe_hud_text(14, 180, "Down: simulate recovery", GREY, 1);
    vcpe_hud_text(14, 192, "Triangle: reclaim 64 KB at loading", GREY, 1);
    vcpe_hud_text(14, 214, "Scales cosmetic drawing and mesh LOD.", WHITE, 1);
    vcpe_hud_text(14, 226, "Simulation and game RNG stay unchanged.", WHITE, 1);
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    vcpe_sys_init(argc, argv);
    vcpe_gfx_init();
    vcpe_pad_init(0, 0, VCPE_STICK_DPAD);
    vcpe_adapt_enable(1);
    vcpe_adapt_work_add(demo_maintenance, 0);
    VcpeTestHooks hooks = {vcpe_pad_button, blank_command, 0, 0, 0};
    if (vcpe_test_load(&hooks))
        vcpe_seed(1);
    vcpe_ev_on(VCPE_EV_ANY, log_event, 0);
    cl_ai = vcpe_client_add("ai", 70);
    cl_loader = vcpe_client_add("loader", 30);
    vcpe_servers_start(2, 0);
    vcpe_bb_set_int("counter", 0);
    vcpe_bb_set_str("build", "blank");
    while (vcpe_running) {
        vcpe_pad_update();
        uint32_t pr = vcpe_pad_pressed();
        if (pr & PSP_CTRL_RIGHT) page = (page + 1) % NPAGES;
        if (pr & PSP_CTRL_LEFT) page = (page + NPAGES - 1) % NPAGES;
        if (pr & PSP_CTRL_CROSS) {
            if (page == PAGE_JOBS) jobs_book();
            if (page == PAGE_ULTRA) vcpe_ev_post(VCPE_EV_GAME + 1, frames, 0, 0);
            if (page == PAGE_ADAPT) vcpe_adapt_enable(!vcpe_adapt_stats()->enabled);
            if (page == PAGE_BENCH) bench_run();
            if (page == PAGE_SAVES) save_write();
        }
        if (page == PAGE_ULTRA && (pr & PSP_CTRL_UP)) vcpe_bb_set_int("counter", vcpe_bb_int("counter", 0) + 1);
        if (page == PAGE_ADAPT && (pr & PSP_CTRL_UP)) demo_feed(35000, 8192, 500);
        if (page == PAGE_ADAPT && (pr & PSP_CTRL_DOWN)) demo_feed(16683, 8192, 1600);
        if (page == PAGE_ADAPT && (pr & PSP_CTRL_TRIANGLE)) demo_loading();
        if (page == PAGE_SAVES && (pr & PSP_CTRL_CIRCLE)) save_read();
        if (page == PAGE_SAVES && (pr & PSP_CTRL_TRIANGLE)) loading_preview();
        frames++;
        vcpe_jobs_update();

        vcpe_gfx_begin(0xFF000000);
        draw_background();
        /* top bar: title and the page tabs */
        vcpe_fill(0, 0, VCPE_SCR_W, 26, 0xC0000000);
        vcpe_hud_text_shadow(8, 5, "VCPE " VCPE_VERSION " BLANK ENGINE", ACCENT, 1);
        for (int p = 0; p < NPAGES; p++) {
            float x = 8 + p * 66;
            if (p == page) vcpe_fill(x - 3, 15, vcpe_hud_text_width(page_names[p], 1) + 6, 10, 0xFF40C8FF);
            vcpe_hud_text(x, 16, page_names[p], p == page ? 0xFF000000 : GREY, 1);
        }
        vcpe_hud_text_shadow(VCPE_SCR_W - 8 - vcpe_hud_text_width("< > PAGES  HOME: QUIT", 1), 5, "< > PAGES  HOME: QUIT", GREY, 1);
        if (page == PAGE_SYSTEM) page_system();
        else if (page == PAGE_JOBS) page_jobs();
        else if (page == PAGE_ULTRA) page_ultra();
        else if (page == PAGE_ADAPT) page_adapt();
        else if (page == PAGE_BENCH) page_bench();
        else if (page == PAGE_SAVES) page_saves();
        else {
            vcpe_hud_perf_detail(1);
            vcpe_hud_perf();
        }
        vcpe_gfx_end();
        vcpe_ultra_update();
        vcpe_test_frame_end();
    }
    vcpe_jobs_shutdown();
    free(demo_cache);
    vcpe_gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
