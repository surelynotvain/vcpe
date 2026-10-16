/* VCPE loading screen (see vcpe_load.h). */

/* HOW THIS FILE WORKS
 *
 * Loading normally blocks the main thread: while a 10 MB level is read, nothing draws and the screen freezes. This
 * module draws frames *from inside* the load instead. Between vcpe_loading_begin and vcpe_loading_end it installs two
 * hooks:
 * - vcpe_pak_progress: the PAK reader calls it after every 256 KB chunk;
 * - vcpe_step_hook: engine modules (and the game) report what they are doing, one line per step. Each hook calls
 *   vcpe_loading_tick(), which draws a complete frame (begin, screen, end, present) if enough time passed since the
 *   last one. So the screen animates as long as the load makes progress, with no extra thread.
 *
 * The screen appears only after show_after_us, so quick loads never flash a loading screen. */
#include <pspkernel.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>
#include "vcpe_sys.h"
#include "vcpe_test.h"
#include "vcpe_gfx.h"
#include "vcpe_pak.h"
#include "vcpe_hud.h"
#include "vcpe_load.h"
#include "vcpe_version.h"
#include "vcpe_loadimg.h"   /* tools/mkloadimg.py: engineassets/hetalia-czechia.gif */

static int show_after_us = 150000;
/* Each redraw waits for a vblank (up to 16.7 ms), so redrawing less often lets the load finish sooner. */
static int redraw_us = 50000;   /* 4.0: was 33 ms; each redraw waits for a vblank, so fewer redraws load faster */

static int active, shown;
void vcpe_adapt_loading(int active) __attribute__((weak));
void vcpe_diag_loading(int active) __attribute__((weak));
void vcpe_adapt_loading_tick(void) __attribute__((weak));
static uint32_t total, done;
static SceInt64 t_begin, t_last;
static SceInt64 t_adapt;
static void (*hook)(float progress, int ms);
static const char *word = "LOADING";

/* the boot log: the last steps reported (vcpe_step_*), newest at the bottom */
/* Retain history independently of the visible height. */
#define LOG_LINES 24
#define LOG_CHARS 58
typedef struct { int id, status; char text[LOG_CHARS + 1]; } LogLine;
static LogLine log_lines[LOG_LINES];
static int log_n;   /* lines used */
static int show_log = 1;
static int visible_lines = 8;

void vcpe_loading_show_log(int on) { show_log = on; }
void vcpe_loading_log_lines(int lines) { visible_lines = lines < 1 ? 1 : lines > LOG_LINES ? LOG_LINES : lines; }
int vcpe_loading_log_height(void) { return visible_lines; }

/* In autotest runs, finished steps also go to stdout with their result, for test logs. */
static void log_print(const LogLine *l)
{
    static const char *tag[] = {"[ .... ]", "[  OK  ]", "[FAILED]", "[ INFO ]"};
    if (vcpe_autotest)
        printf("%s %s\n", tag[l->status], l->text);
}

/* A new step (text given) adds a line; a finished step (text 0) only updates the status of its line. */
static void step_hook(int id, int status, const char *text)
{
    if (text) {
        if (log_n == LOG_LINES) {
            memmove(log_lines, log_lines + 1, sizeof(LogLine) * (LOG_LINES - 1));
            log_n--;
        }
        LogLine *l = &log_lines[log_n++];
        l->id = id;
        l->status = status;
        snprintf(l->text, sizeof l->text, "%s", text);
        if (status != VCPE_STEP_RUN)
            log_print(l);
    } else {
        for (int i = log_n - 1; i >= 0; i--)
            if (log_lines[i].id == id) {
                log_lines[i].status = status;
                log_print(&log_lines[i]);
                break;
            }
    }
    vcpe_loading_tick();
}

/* what the engine is doing, one line per step ("Loading textures 3: 28 pages, 7196 KB"), top-left like a PC
 * booting. Plain text: the results only go to stdout in autotest runs */
/* The log is drawn in the 8x8 HUD font, top-left, one line every 9 pixels. */
void vcpe_loading_draw_log(int ms)
{
    (void)ms;
    static const char *tag[] = {"[ .... ]", "[  OK  ]", "[FAILED]", "[ INFO ]"};
    static const uint32_t color[] = {0xFFC0C0C0, 0xFF80D080, 0xFF8080FF, 0xFFC0C0C0};
    int first = log_n > visible_lines ? log_n - visible_lines : 0;
    for (int i = first; i < log_n; i++) {
        char line[59];
        int status = log_lines[i].status;
        if (status < 0 || status > 3) status = VCPE_STEP_INFO;
        snprintf(line, sizeof line, "%s %.49s", tag[status], log_lines[i].text);
        vcpe_hud_text(8, 8 + (i - first) * 9, line, color[status], 1);
    }
}

void vcpe_loading_draw_hook(void (*draw)(float progress, int ms)) { hook = draw; }
void vcpe_loading_text(const char *text) { word = text ? text : "LOADING"; }
int vcpe_loading_active(void) { return active; }
void vcpe_loading_show_after(int ms) { show_after_us = ms * 1000; }
void vcpe_loading_redraw_ms(int ms) { redraw_us = (ms < 16 ? 16 : ms) * 1000; }
int vcpe_loading_shown(void) { return shown; }

/* The animation: 31 frames of 8-bit indexed pixels sharing one palette (src/vcpe_loadimg.h, built from
 * engineassets/hetalia-czechia.gif by tools/mkloadimg.py). Size is the drawn width; height keeps its aspect ratio.
 * Find the frame for this moment from the per-frame durations, then
 * point the GE at that frame's texels; the palette load is skipped while the same frame is still bound. */
void vcpe_loading_anim(float x, float y, float size, int ms)
{
    static int loop_ms;
    if (!loop_ms)
        for (int i = 0; i < VCPE_LOADIMG_FRAMES; i++)
            loop_ms += vcpe_loadimg_ms[i];
    int t = (ms < 0 ? 0 : ms) % loop_ms, f = 0;
    while (f < VCPE_LOADIMG_FRAMES - 1 && t >= vcpe_loadimg_ms[f])
        t -= vcpe_loadimg_ms[f++];
    const uint8_t *img = vcpe_loadimg + f * VCPE_LOADIMG_FRAME_BYTES;
    vcpe_blend(0);
    vcpe_tex_white(0);
    vcpe_filter(1);
    if (!vcpe_tex_bound(img)) {
        vcpe_tex_claim(img);
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, vcpe_loadimg_clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, VCPE_LOADIMG_TEX_W, VCPE_LOADIMG_TEX_H, VCPE_LOADIMG_TEX_W, img);
        vcpe_tex_flush();
    }
    vcpe_quad(x, y, x + size, y + size * VCPE_LOADIMG_HEIGHT / VCPE_LOADIMG_WIDTH,
              0, 0, VCPE_LOADIMG_WIDTH, VCPE_LOADIMG_HEIGHT, 0xFFFFFFFF);
    vcpe_gfx_flush();
}

/* the standard screen: the boot log top-left; the animation in the bottom-right corner with the word and the
 * progress bar just above it */
/* The standard screen: log top-left, animation bottom-right, the word with 0-3 animated dots and the bar above it. */
void vcpe_loading_draw_standard(float p, int ms)
{
    const float m = 12, sz = VCPE_LOADIMG_WIDTH;
    const char *credit = "made with VCPE " VCPE_VERSION;
    float ax = VCPE_SCR_W - m - sz;
    float ay = VCPE_SCR_H - m - 12 - VCPE_LOADIMG_HEIGHT;
    if (show_log)
        vcpe_loading_draw_log(ms);
    vcpe_loading_anim(ax, ay, sz, ms);
    vcpe_hud_text(VCPE_SCR_W - m - vcpe_hud_text_width(credit, 1), VCPE_SCR_H - m - 8,
                   credit, 0xFFC0C0C0, 1);
    char s[40];
    int dots = (ms / 300) % 4;
    snprintf(s, sizeof s, "%s%.*s", word, dots, "...");
    float y = ay - (p >= 0 ? 16 : 12);
    vcpe_hud_text(ax, y, s, 0xFFC0C0C0, 1);
    if (p >= 0) {
        vcpe_fill(ax, ay - 6, sz, 2, 0xFF303030);
        vcpe_fill(ax, ay - 6, sz * (p > 1 ? 1 : p), 2, 0xFFC0C0C0);
    }
}

/* One complete frame of the loading screen, shown immediately. */
static void draw(SceInt64 now)
{
    float p = total ? (float)done / (float)total : -1.0f;
    int ms = (int)((now - t_begin) / 1000);
    vcpe_gfx_begin(0xFF000000);
    if (hook)
        hook(p, ms);
    else
        vcpe_loading_draw_standard(p, ms);
    vcpe_gfx_end();
    vcpe_gfx_present();
    shown++;
    t_last = now;
}

/* Draw if one is due. Never inside a game frame: the display list is shared, and a load started mid-frame would
 * otherwise begin a second frame inside the first. */
void vcpe_loading_tick(void)
{
    if (!active || vcpe_gfx_in_frame())
        return;
    SceInt64 now = sceKernelGetSystemTimeWide();
    if (vcpe_adapt_loading_tick && now - t_adapt >= 50000) {
        t_adapt = now;
        vcpe_adapt_loading_tick();
    }
    if (now - t_begin >= show_after_us && (!shown || now - t_last >= redraw_us))
        draw(now);
}

static void pak_progress(uint32_t bytes)
{
    done += bytes;
    vcpe_loading_tick();
}

void vcpe_loading_add(uint32_t bytes)
{
    done += bytes;
    vcpe_loading_tick();
}

/* Start: remember the expected bytes for the bar, clear the log and install the hooks. */
void vcpe_loading_begin(uint32_t total_bytes)
{
    active = 1;
    if (vcpe_adapt_loading) vcpe_adapt_loading(1);
    if (vcpe_diag_loading) vcpe_diag_loading(1);
    shown = 0;
    total = total_bytes;
    done = 0;
    t_begin = sceKernelGetSystemTimeWide();
    t_adapt = t_begin;
    log_n = 0;
    vcpe_pak_progress = pak_progress;
    vcpe_step_hook = step_hook;
}

/* Finish: if the screen was shown, draw it once more in its final state (full bar), then remove the hooks. */
void vcpe_loading_end(void)
{
    if (!active)
        return;
    /* a screen that was shown ends on its last state: a full bar, every step's result */
    if (shown) {
        if (total)
            done = total;
        draw(sceKernelGetSystemTimeWide());
    }
    active = 0;
    vcpe_pak_progress = 0;
    vcpe_step_hook = 0;
    if (vcpe_adapt_loading) vcpe_adapt_loading(0);
    if (vcpe_diag_loading) vcpe_diag_loading(0);
}
