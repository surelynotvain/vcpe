/* VCPE RPG windows: Window_Base / Window_Selectable behaviour without bitmaps.
 *
 * A window is a frame plus contents; instead of drawing the contents into a bitmap once (RGSS's refresh), the game
 * draws them every frame through a draw callback between rpg_win_contents_begin / _end, which translate to the
 * contents' origin (x + padding, y + padding - oy) and clip to the inside. Selection, scrolling, columns, cursor
 * repeat, paging and the ok / cancel decisions follow Window_Selectable; rpg_win_update returns what happened. */
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"
#include <string.h>

void rpg_win_init(RpgWin *w, float x, float y, float width, float height)
{
    memset(w, 0, sizeof *w);
    w->x = x;
    w->y = y;
    w->w = width;
    w->h = height;
    w->padding = 12;
    w->opacity = 255;
    w->back_opacity = 192;
    w->contents_opacity = 255;
    w->openness = 255;
    w->visible = 1;
    w->index = -1;
    w->col_max = 1;
    w->spacing = 32;
    w->item_h = 24;
    w->ok = w->cancel = 1;
}

float rpg_fitting_height(int lines) { return lines * 24 + 12 * 2; }

float rpg_win_item_w(const RpgWin *w) { return (w->w - w->padding * 2 + w->spacing) / w->col_max - w->spacing; }
int rpg_win_row_max(const RpgWin *w)
{
    int r = (w->item_max + w->col_max - 1) / w->col_max;
    return r > 1 ? r : 1;
}
int rpg_win_page_rows(const RpgWin *w)
{
    float inner = w->h - w->padding * 2;
    int r = (int)(inner / w->item_h);
    return r > 0 ? r : 1;
}
int rpg_win_top_row(const RpgWin *w) { return (int)(w->oy / w->item_h); }
void rpg_win_set_top_row(RpgWin *w, int row)
{
    if (row > rpg_win_row_max(w) - 1)
        row = rpg_win_row_max(w) - 1;
    if (row < 0)
        row = 0;
    w->oy = row * w->item_h;
}

RpgRect rpg_win_item_rect(const RpgWin *w, int i)
{
    RpgRect r;
    r.w = rpg_win_item_w(w);
    r.h = w->item_h;
    r.x = i % w->col_max * (r.w + w->spacing);
    r.y = i / w->col_max * w->item_h;
    return r;
}

RpgRect rpg_win_item_text_rect(const RpgWin *w, int i)
{
    RpgRect r = rpg_win_item_rect(w, i);
    r.x += 4;
    r.w -= 8;
    return r;
}

static void ensure_visible(RpgWin *w)
{
    if (w->index < 0)
        return;
    int row = w->index / w->col_max, top = rpg_win_top_row(w), pr = rpg_win_page_rows(w);
    if (row < top)
        rpg_win_set_top_row(w, row);
    if (row > top + pr - 1)
        rpg_win_set_top_row(w, row - (pr - 1));
}

void rpg_win_select(RpgWin *w, int i)
{
    w->index = i;
    ensure_visible(w);
}

void rpg_win_open(RpgWin *w)
{
    if (w->openness < 255)
        w->opening = 1;
    w->closing = 0;
}

void rpg_win_close(RpgWin *w)
{
    if (w->openness > 0)
        w->closing = 1;
    w->opening = 0;
}

int rpg_win_is_open(const RpgWin *w) { return w->openness >= 255; }
int rpg_win_is_closed(const RpgWin *w) { return w->openness <= 0; }

void rpg_input_consume(void) { rpg_in.trigger = rpg_in.repeat = 0; }

static void cursor_move(RpgWin *w, int d, int wrap)
{
    int n = w->item_max, c = w->col_max, i = w->index;
    int horizontal = rpg_win_page_rows(w) == 1;
    switch (d) {
    case 2: if (i < n - c || (wrap && c == 1)) rpg_win_select(w, (i + c) % n); break;
    case 8: if (i >= c || (wrap && c == 1)) rpg_win_select(w, (i - c + n) % n); break;
    case 6: if (c >= 2 && (i < n - 1 || (wrap && horizontal))) rpg_win_select(w, (i + 1) % n); break;
    case 4: if (c >= 2 && (i > 0 || (wrap && horizontal))) rpg_win_select(w, (i - 1 + n) % n); break;
    }
}

int rpg_win_update(RpgWin *w)
{
    /* Window#update: opening / closing at 48 per frame */
    if (w->opening) {
        w->openness += 48;
        if (w->openness >= 255) {
            w->openness = 255;
            w->opening = 0;
        }
    } else if (w->closing) {
        w->openness -= 48;
        if (w->openness <= 0) {
            w->openness = 0;
            w->closing = 0;
        }
    }
    w->frame++;
    if (!w->active || !rpg_win_is_open(w))
        return RPG_WIN_NONE;
    /* process_cursor_move */
    if (!w->cursor_fix && !w->cursor_all && w->item_max > 0) {
        int last = w->index;
        if (rpg_in.repeat & RPG_K_DOWN) cursor_move(w, 2, (rpg_in.trigger & RPG_K_DOWN) != 0);
        if (rpg_in.repeat & RPG_K_UP) cursor_move(w, 8, (rpg_in.trigger & RPG_K_UP) != 0);
        if (rpg_in.repeat & RPG_K_RIGHT) cursor_move(w, 6, (rpg_in.trigger & RPG_K_RIGHT) != 0);
        if (rpg_in.repeat & RPG_K_LEFT) cursor_move(w, 4, (rpg_in.trigger & RPG_K_LEFT) != 0);
        if (!w->pagedown && (rpg_in.trigger & RPG_K_R)) {
            int pr = rpg_win_page_rows(w);
            if (rpg_win_top_row(w) + pr < rpg_win_row_max(w)) {
                rpg_win_set_top_row(w, rpg_win_top_row(w) + pr);
                int ni = w->index + pr * w->col_max;
                w->index = ni < w->item_max - 1 ? ni : w->item_max - 1;
            }
        }
        if (!w->pageup && (rpg_in.trigger & RPG_K_L)) {
            int pr = rpg_win_page_rows(w);
            if (rpg_win_top_row(w) > 0) {
                rpg_win_set_top_row(w, rpg_win_top_row(w) - pr);
                int ni = w->index - pr * w->col_max;
                w->index = ni > 0 ? ni : 0;
            }
        }
        if (w->index != last) {
            rpg_play_system_se(RPG_SE_CURSOR);
            if (w->on_change)
                w->on_change(w);
        }
    }
    /* process_handling */
    if (w->ok && (rpg_in.trigger & RPG_K_C)) {
        if (!w->enabled || w->enabled(w, w->index)) {
            rpg_play_system_se(RPG_SE_OK);
            rpg_input_consume();
            w->active = 0;
            return RPG_WIN_OK;
        }
        rpg_play_system_se(RPG_SE_BUZZER);
        return RPG_WIN_NONE;
    }
    if (w->cancel && (rpg_in.trigger & RPG_K_B)) {
        rpg_play_system_se(RPG_SE_CANCEL);
        rpg_input_consume();
        w->active = 0;
        return RPG_WIN_CANCEL;
    }
    if (w->pagedown && (rpg_in.trigger & RPG_K_R)) {
        rpg_play_system_se(RPG_SE_CURSOR);
        rpg_input_consume();
        w->active = 0;
        return RPG_WIN_PAGEDOWN;
    }
    if (w->pageup && (rpg_in.trigger & RPG_K_L)) {
        rpg_play_system_se(RPG_SE_CURSOR);
        rpg_input_consume();
        w->active = 0;
        return RPG_WIN_PAGEUP;
    }
    return RPG_WIN_NONE;
}

static float saved_ox[8], saved_oy[8];
static int nsaved;

void rpg_push_origin(float dx, float dy)
{
    if (nsaved < 8) {
        saved_ox[nsaved] = rpg_ox;
        saved_oy[nsaved] = rpg_oy;
        nsaved++;
    }
    rpg_ox += dx * rpg_scale;
    rpg_oy += dy * rpg_scale;
}

void rpg_pop_origin(void)
{
    if (nsaved > 0) {
        nsaved--;
        rpg_ox = saved_ox[nsaved];
        rpg_oy = saved_oy[nsaved];
    }
}

void rpg_win_draw_frame(const RpgWin *w)
{
    if (!w->visible || w->openness <= 0)
        return;
    rpg_window(w->x, w->y, w->w, w->h, w->opacity, w->back_opacity, w->openness / 255.f, rpg.window_tone);
}

int rpg_win_contents_begin(const RpgWin *w)
{
    if (!w->visible || w->openness < 255)
        return 0;
    rpg_clip(w->x + w->padding, w->y + w->padding, w->w - w->padding * 2, w->h - w->padding * 2);
    rpg_push_origin(w->x + w->padding, w->y + w->padding - w->oy);
    return 1;
}

void rpg_win_contents_end(const RpgWin *w)
{
    (void)w;
    rpg_pop_origin();
    rpg_clip_off();
}

void rpg_win_draw_cursor(const RpgWin *w)
{
    if (w->index < 0 || !w->visible || w->openness < 255)
        return;
    RpgRect r;
    if (w->cursor_all) {
        r.x = 0;
        r.y = 0;
        r.w = w->w - w->padding * 2;
        r.h = rpg_win_row_max(w) * w->item_h;
    } else {
        r = rpg_win_item_rect(w, w->index);
        r.y -= w->oy;
    }
    float cx = w->x + w->padding + r.x, cy = w->y + w->padding + r.y;
    float top = w->y + w->padding, bottom = w->y + w->h - w->padding;
    if (cy + r.h <= top || cy >= bottom)
        return;
    rpg_cursor(cx, cy, r.w, r.h, w->frame, w->active);
}

extern int rpg_debug_skip;
void rpg_win_draw(RpgWin *w, void (*item)(RpgWin *w, int i, RpgRect r, void *user), void *user)
{
    if (!(rpg_debug_skip & 16)) rpg_win_draw_frame(w);
    if (!rpg_win_contents_begin(w))
        return;
    int first = rpg_win_top_row(w) * w->col_max, last = first + (rpg_win_page_rows(w) + 1) * w->col_max;
    for (int i = first; i < w->item_max && i < last; i++)
        if (item && !(rpg_debug_skip & 32))
            item(w, i, rpg_win_item_rect(w, i), user);
    rpg_win_contents_end(w);
    if (!(rpg_debug_skip & 64)) rpg_win_draw_cursor(w);
}
