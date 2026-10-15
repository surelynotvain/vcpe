/* VCPE mouse simulation (see vcpe_mouse.h). */
#include <pspctrl.h>
#include <pspkernel.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_gfx.h"
#include "vcpe_pad.h"
#include "vcpe_test.h"
#include "vcpe_mouse.h"

/* vcpe_log is optional: "mpos" logs only when the game links vcpe_log */
extern void vcpe_log(const char *fmt, ...) __attribute__((weak));

#define MAX_POI 512

float vcpe_mouse_x = VCPE_SCR_W / 2, vcpe_mouse_y = VCPE_SCR_H / 2;
static VcpeMouseConf cf;
static int inited;
static float bx0, by0, bx1 = VCPE_SCR_W - 1, by1 = VCPE_SCR_H - 1;
static uint32_t held, latch_pressed, latch_released;
static int moved, idle, wheel_latch;
static int dragging, drag_cancelled, latch_drag_started, latch_dropped, latch_clicked;
static float press_x, press_y;
static int frames, last_click_frame = -1000;
static float last_click_x, last_click_y;
static SceInt64 last_time;
static float poi_x[MAX_POI], poi_y[MAX_POI];
static int npoi;
static void (*poi_collect)(void);
static uint32_t warp_dir_prev;
static int warp_next, warp_count, latch_warped;
static float warp_from_x, warp_from_y;
static int wheel_frames[2];

/* autotest */
enum { ACT_NONE, ACT_CLICK, ACT_DCLICK, ACT_GLIDE, ACT_DRAG };
static uint32_t script_held;          /* buttons held by the script */
static struct { int type, frame, n; uint32_t btn; float x0, y0, x1, y1; } act;
static uint32_t script_warp;          /* a d-pad direction for one warp */
static int script_wheel;

void vcpe_mouse_defaults(VcpeMouseConf *c)
{
    memset(c, 0, sizeof *c);
    c->left = PSP_CTRL_CROSS;
    c->right = PSP_CTRL_CIRCLE;
    c->middle = PSP_CTRL_TRIANGLE;
    c->slow = PSP_CTRL_RTRIGGER;
    c->speed = 150;          /* 600 px/s on a 1920-wide screen */
    c->rim_boost = 1.5f;
    c->slow_factor = 0.35f;
    c->curve = 1;
    c->deadzone = 0.2f;
    c->dpad_warp = 1;
    c->warp_angle = 50;
    c->warp_min_dist = 4;    /* 15 px on a 1920-wide screen */
    c->warp_first = 20;
    c->warp_rapid = 10;
    c->drag_px = 3;
    c->double_frames = 20;
}

void vcpe_mouse_init(const VcpeMouseConf *c)
{
    if (c)
        cf = *c;
    else
        vcpe_mouse_defaults(&cf);
    inited = 1;
    vcpe_mouse_x = (bx0 + bx1) / 2;
    vcpe_mouse_y = (by0 + by1) / 2;
    held = latch_pressed = latch_released = 0;
    dragging = drag_cancelled = latch_drag_started = latch_dropped = latch_clicked = 0;
    last_time = 0;
}

void vcpe_mouse_bounds(float x, float y, float w, float h)
{
    bx0 = x;
    by0 = y;
    bx1 = x + w - 1;
    by1 = y + h - 1;
    vcpe_mouse_set(vcpe_mouse_x, vcpe_mouse_y);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void vcpe_mouse_set(float x, float y)
{
    float nx = clampf(x, bx0, bx1), ny = clampf(y, by0, by1);
    if (nx != vcpe_mouse_x || ny != vcpe_mouse_y)
        moved = 1;
    vcpe_mouse_x = nx;
    vcpe_mouse_y = ny;
}

/* ---- points of interest and warping ---- */

void vcpe_mouse_poi_source(void (*collect)(void)) { poi_collect = collect; }
void vcpe_mouse_poi_clear(void) { npoi = 0; }

void vcpe_mouse_poi(float x, float y)
{
    if (npoi < MAX_POI) {
        poi_x[npoi] = x;
        poi_y[npoi] = y;
        npoi++;
    }
}

void vcpe_mouse_poi_rect(float x, float y, float w, float h) { vcpe_mouse_poi(x + w / 2, y + h / 2); }

int vcpe_mouse_warp(float dx, float dy)
{
    float l = sqrtf(dx * dx + dy * dy);
    if (l <= 0)
        return 0;
    dx /= l;
    dy /= l;
    if (poi_collect) {
        npoi = 0;
        poi_collect();
    }
    const float max_angle = cf.warp_angle * (float)M_PI / 180.0f;
    float best = -1e30f;
    int bi = -1;
    for (int i = 0; i < npoi; i++) {
        float ox = poi_x[i] - vcpe_mouse_x, oy = poi_y[i] - vcpe_mouse_y;
        float dist = sqrtf(ox * ox + oy * oy);
        if (dist < cf.warp_min_dist)
            continue;
        float c = (ox * dx + oy * dy) / dist;
        float angle = acosf(clampf(c, -1, 1));
        if (angle > max_angle)
            continue;
        /* closer and straighter wins: score = -dist * (1 + 2 * angle / max_angle) */
        float score = -(1 + angle * 2 / max_angle) * dist;
        if (score > best) {
            best = score;
            bi = i;
        }
    }
    if (bi < 0)
        return 0;
    warp_from_x = vcpe_mouse_x;
    warp_from_y = vcpe_mouse_y;
    vcpe_mouse_set(poi_x[bi], poi_y[bi]);
    latch_warped = 1;
    return 1;
}

int vcpe_mouse_warped(void) { int r = latch_warped; latch_warped = 0; return r; }
void vcpe_mouse_warp_from(float *x, float *y) { if (x) *x = warp_from_x; if (y) *y = warp_from_y; }

static void dpad_warp(uint32_t psp)
{
    uint32_t d = psp & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    if (!d) {
        warp_dir_prev = 0;
        warp_count = 0;
        return;
    }
    if (d != warp_dir_prev) {   /* a new direction warps at once */
        warp_dir_prev = d;
        warp_count = 0;
        warp_next = frames;
    }
    if (frames < warp_next)
        return;
    float dx = ((d & PSP_CTRL_RIGHT) ? 1 : 0) - ((d & PSP_CTRL_LEFT) ? 1 : 0);
    float dy = ((d & PSP_CTRL_DOWN) ? 1 : 0) - ((d & PSP_CTRL_UP) ? 1 : 0);
    if (vcpe_mouse_warp(dx, dy)) {
        warp_next = frames + (warp_count == 0 ? cf.warp_first : cf.warp_rapid);
        warp_count++;
    }
}

/* ---- the frame ---- */

static void wheel_button(int i, uint32_t on, int step)
{
    if (!on) {
        wheel_frames[i] = 0;
        return;
    }
    int f = ++wheel_frames[i];
    if (f == 1 || (f > 20 && (f - 21) % 6 == 0))
        wheel_latch += step;
}

void vcpe_mouse_update(void)
{
    if (!inited)
        vcpe_mouse_init(0);
    frames++;
    moved = 0;
    uint32_t psp = vcpe_pad_psp();

    /* stick: deadzone, curve, rim boost; time-based so a slow frame does not slow the pointer */
    SceInt64 now = sceKernelGetSystemTimeWide();
    float dt = last_time ? (float)(now - last_time) / 1e6f : 1 / 60.0f;
    last_time = now;
    if (dt > 0.05f) dt = 0.05f;
    if (vcpe_autotest) dt = 1 / 60.0f;
    int sx, sy;
    vcpe_pad_stick(&sx, &sy);
    float vx = sx / 127.5f, vy = sy / 127.5f;
    float len = sqrtf(vx * vx + vy * vy);
    if (len > cf.deadzone && len > 0) {
        float t = (len - cf.deadzone) / (1 - cf.deadzone);
        if (t > 1) t = 1;
        if (cf.curve != 1) t = powf(t, cf.curve);
        float k = t / len * cf.speed * dt;
        if (len > 0.99f) k *= cf.rim_boost;
        if (psp & cf.slow) k *= cf.slow_factor;
        vcpe_mouse_set(vcpe_mouse_x + vx * k, vcpe_mouse_y + vy * k);
    }
    if (cf.dpad_warp)
        dpad_warp(psp | script_warp);
    script_warp = 0;
    if (script_wheel) {
        wheel_latch += script_wheel;
        script_wheel = 0;
    }
    wheel_button(0, psp & cf.wheel_up, 1);
    wheel_button(1, psp & cf.wheel_down, -1);

    /* buttons */
    uint32_t h = script_held;
    if (psp & cf.left) h |= VCPE_MB_LEFT;
    if (psp & cf.right) h |= VCPE_MB_RIGHT;
    if (psp & cf.middle) h |= VCPE_MB_MIDDLE;
    uint32_t down = h & ~held, up = held & ~h;
    held = h;
    latch_pressed |= down;
    latch_released |= up;

    /* left: click / drag / drop */
    if (down & VCPE_MB_LEFT) {
        press_x = vcpe_mouse_x;
        press_y = vcpe_mouse_y;
        dragging = drag_cancelled = 0;
    }
    if ((held & VCPE_MB_LEFT) && !dragging && !drag_cancelled) {
        float ox = vcpe_mouse_x - press_x, oy = vcpe_mouse_y - press_y;
        if (ox * ox + oy * oy > (float)(cf.drag_px * cf.drag_px)) {
            dragging = 1;
            latch_drag_started = 1;
        }
    }
    if (up & VCPE_MB_LEFT) {
        if (dragging) {
            latch_dropped = 1;
        } else if (!drag_cancelled) {
            float ox = vcpe_mouse_x - last_click_x, oy = vcpe_mouse_y - last_click_y;
            int dbl = frames - last_click_frame <= cf.double_frames && ox * ox + oy * oy <= (float)(cf.drag_px * cf.drag_px);
            latch_clicked = dbl ? 2 : 1;
            last_click_frame = dbl ? -1000 : frames;   /* a third click starts a new pair */
            last_click_x = vcpe_mouse_x;
            last_click_y = vcpe_mouse_y;
        }
        dragging = drag_cancelled = 0;
    }
    if (moved || down || up)
        idle = 0;
    else
        idle++;
}

int vcpe_mouse_moved(void) { return moved; }
int vcpe_mouse_idle(void) { return idle; }
int vcpe_mouse_visible(void) { return cf.hide_idle_frames <= 0 || idle < cf.hide_idle_frames; }
uint32_t vcpe_mouse_held(void) { return held; }
uint32_t vcpe_mouse_pressed(void) { uint32_t r = latch_pressed; latch_pressed = 0; return r; }
uint32_t vcpe_mouse_released(void) { uint32_t r = latch_released; latch_released = 0; return r; }
int vcpe_mouse_wheel(void) { int r = wheel_latch; wheel_latch = 0; return r; }
int vcpe_mouse_clicked(void) { int r = latch_clicked; latch_clicked = 0; return r; }
int vcpe_mouse_drag_started(void) { int r = latch_drag_started; latch_drag_started = 0; return r; }
int vcpe_mouse_dragging(void) { return dragging; }
int vcpe_mouse_dropped(void) { int r = latch_dropped; latch_dropped = 0; return r; }
void vcpe_mouse_press_pos(float *x, float *y) { if (x) *x = press_x; if (y) *y = press_y; }

void vcpe_mouse_cancel_drag(void)
{
    dragging = 0;
    drag_cancelled = 1;
    latch_drag_started = latch_dropped = 0;
}

int vcpe_mouse_in(float x, float y, float w, float h)
{
    return vcpe_mouse_x >= x && vcpe_mouse_x < x + w && vcpe_mouse_y >= y && vcpe_mouse_y < y + h;
}

/* ---- built-in pointers: 'X' outline, '.' fill, hot spot per shape ---- */

static const char *const arrow[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X",
    "       XX", 0};
static const char *const hand[] = {
    "     XX", "    X..X", "    X..X", "    X..X", "    X..XXX", "    X..X..XX", "XX  X..X..X.XX",
    "X.X X.......X.X", " X.XX.........X", "  X..........X", "  X..........X", "   X........X", "   X........X",
    "    X......X", "    XXXXXXXX", 0};
static const char *const grab[] = {
    "    XX XX XX", "   X..X..X..XX", "   X.........X.X", "  XX...........X", " X.X...........X",
    " X.............X", "  X...........X", "   X..........X", "    X........X", "    XXXXXXXXXX", 0};
static const char *const cross[] = {
    "    XXX", "    X.X", "    X.X", "    X.X", "XXXXX.XXXXX", "X.........X", "XXXXX.XXXXX", "    X.X",
    "    X.X", "    X.X", "    XXX", 0};
static const struct { const char *const *rows; int hx, hy; } shapes[] = {
    {arrow, 0, 0}, {hand, 5, 0}, {grab, 8, 4}, {cross, 5, 5}};

void vcpe_mouse_draw(int shape, float scale)
{
    if (shape < 0 || shape > VCPE_CURSOR_CROSS)
        shape = VCPE_CURSOR_ARROW;
    if (scale <= 0)
        scale = 1;
    float ox = floorf(vcpe_mouse_x) - shapes[shape].hx * scale, oy = floorf(vcpe_mouse_y) - shapes[shape].hy * scale;
    const char *const *rows = shapes[shape].rows;
    for (int r = 0; rows[r]; r++) {
        const char *s = rows[r];
        for (int c = 0; s[c];) {
            char k = s[c];
            int e = c;
            while (s[e] == k)
                e++;
            if (k != ' ')
                vcpe_fill(ox + c * scale, oy + r * scale, (e - c) * scale, scale, k == 'X' ? 0xFF000000u : 0xFFFFFFFFu);
            c = e;
        }
    }
}

/* ---- autotest ---- */

static uint32_t btn_named(const char *s)
{
    if (!strcmp(s, "right")) return VCPE_MB_RIGHT;
    if (!strcmp(s, "middle")) return VCPE_MB_MIDDLE;
    return VCPE_MB_LEFT;
}

int vcpe_mouse_command(const char *cmd, const char *arg, int num, int num2, const char *line, unsigned *held_psp)
{
    float a[5] = {0};
    int n = sscanf(line, "%*s %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4]);
    static const char *const acts[] = {"glide", "click", "rclick", "dclick", "drag"};
    for (unsigned i = 0; i < sizeof acts / sizeof acts[0]; i++)
        if (!strcmp(cmd, acts[i]))
            memset(&act, 0, sizeof act);
    if (!strcmp(cmd, "mouse")) {
        vcpe_mouse_set(a[0], a[1]);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(cmd, "glide")) {
        act.type = ACT_GLIDE;
        act.x0 = vcpe_mouse_x; act.y0 = vcpe_mouse_y; act.x1 = a[0]; act.y1 = a[1];
        act.n = n >= 3 && a[2] > 0 ? (int)a[2] : 12;
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick") || !strcmp(cmd, "dclick")) {
        if (n >= 2)
            vcpe_mouse_set(a[0], a[1]);
        act.type = cmd[0] == 'd' ? ACT_DCLICK : ACT_CLICK;
        act.btn = cmd[0] == 'r' ? VCPE_MB_RIGHT : VCPE_MB_LEFT;
        script_held |= act.btn;
        act.frame = 1;
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "mdown")) {
        script_held |= btn_named(arg);
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "mup")) {
        script_held &= ~(arg[0] ? btn_named(arg) : VCPE_MB_LEFT);
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "drag")) {
        act.type = ACT_DRAG;
        act.x0 = a[0]; act.y0 = a[1]; act.x1 = a[2]; act.y1 = a[3];
        act.n = n >= 5 && a[4] > 0 ? (int)a[4] : 12;
        vcpe_mouse_set(act.x0, act.y0);
        script_held |= VCPE_MB_LEFT;
        act.frame = 1;
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "warp")) {
        script_warp = vcpe_pad_button(arg) & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
        warp_dir_prev = 0;   /* each scripted warp is a fresh press */
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "wheel")) {
        script_wheel += atoi(arg);
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "mpos")) {
        if (vcpe_log)
            vcpe_log("mouse %.1f %.1f held %u drag %d", vcpe_mouse_x, vcpe_mouse_y, (unsigned)held, dragging);
        return VCPE_CMD_NEXT;
    }
    return VCPE_CMD_UNKNOWN;
}

int vcpe_mouse_busy(unsigned *held_psp)
{
    switch (act.type) {
    case ACT_GLIDE: {
        act.frame++;
        float t = (float)act.frame / act.n;
        vcpe_mouse_set(act.x0 + (act.x1 - act.x0) * t, act.y0 + (act.y1 - act.y0) * t);
        if (act.frame >= act.n)
            act.type = ACT_NONE;
        return 1;
    }
    case ACT_CLICK:
        script_held &= ~act.btn;   /* frame 2: release */
        act.type = ACT_NONE;
        return 1;
    case ACT_DCLICK:
        /* down, up, down, up on four frames */
        act.frame++;
        if (act.frame == 2 || act.frame == 4)
            script_held &= ~act.btn;
        else
            script_held |= act.btn;
        if (act.frame >= 4)
            act.type = ACT_NONE;
        return 1;
    case ACT_DRAG: {
        /* frames 2..n+1 move, frame n+2 releases */
        act.frame++;
        if (act.frame <= act.n + 1) {
            float t = (float)(act.frame - 1) / act.n;
            vcpe_mouse_set(act.x0 + (act.x1 - act.x0) * t, act.y0 + (act.y1 - act.y0) * t);
        } else {
            script_held &= ~VCPE_MB_LEFT;
            act.type = ACT_NONE;
        }
        return 1;
    }
    }
    return 0;
}
