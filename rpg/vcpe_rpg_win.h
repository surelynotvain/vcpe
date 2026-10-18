/* VCPE RPG windows (vcpe_rpg_win.c): Window_Base / Window_Selectable state without bitmaps. */
#ifndef VCPE_RPG_WIN_H
#define VCPE_RPG_WIN_H

typedef struct { float x, y, w, h; } RpgRect;
typedef struct RpgWin {
    float x, y, w, h, padding, oy;
    int opacity, back_opacity, contents_opacity, openness, opening, closing, visible, active;
    int index, item_max, col_max;
    float spacing, item_h;
    int cursor_fix, cursor_all;
    int ok, cancel, pageup, pagedown;            /* which handlers exist */
    int (*enabled)(struct RpgWin *w, int index);   /* current_item_enabled? */
    void (*on_change)(struct RpgWin *w);           /* the index moved (help text) */
    int frame;
    int user;
} RpgWin;
enum { RPG_WIN_NONE, RPG_WIN_OK, RPG_WIN_CANCEL, RPG_WIN_PAGEUP, RPG_WIN_PAGEDOWN };

void rpg_win_init(RpgWin *w, float x, float y, float width, float height);
float rpg_fitting_height(int lines);
float rpg_win_item_w(const RpgWin *w);
int rpg_win_row_max(const RpgWin *w);
int rpg_win_page_rows(const RpgWin *w);
int rpg_win_top_row(const RpgWin *w);
void rpg_win_set_top_row(RpgWin *w, int row);
RpgRect rpg_win_item_rect(const RpgWin *w, int i);
RpgRect rpg_win_item_text_rect(const RpgWin *w, int i);
void rpg_win_select(RpgWin *w, int i);
void rpg_win_open(RpgWin *w);
void rpg_win_close(RpgWin *w);
int rpg_win_is_open(const RpgWin *w);
int rpg_win_is_closed(const RpgWin *w);
int rpg_win_update(RpgWin *w);                 /* RPG_WIN_* */
void rpg_win_draw_frame(const RpgWin *w);
int rpg_win_contents_begin(const RpgWin *w);   /* 0: not open (draw nothing) */
void rpg_win_contents_end(const RpgWin *w);
void rpg_win_draw_cursor(const RpgWin *w);
/* frame + visible items (in contents coordinates) + cursor */
void rpg_win_draw(RpgWin *w, void (*item)(RpgWin *w, int i, RpgRect r, void *user), void *user);
void rpg_push_origin(float dx, float dy);
void rpg_pop_origin(void);
void rpg_input_consume(void);

#endif
