/* VCPE RPG messages: Game_Message + Window_Message (with the choice list, number input, key item list and gold
 * window) + Window_ScrollText.
 *
 * Window_Message runs a Fiber in VX Ace; here it is a small state machine with the same steps: open, type the text
 * one character per frame (\. \| waits, \! pause, \> \< instant lines, \^ no pause, \$ gold), wait for input, clear,
 * and if the interpreter queued more text in the same frame with the same placement, continue without closing.
 * What has been typed so far is kept as a list of drawing operations (text runs and icons) that is drawn each frame,
 * instead of a contents bitmap. */
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

RpgMessage rpg_msg;
int rpg_msg_skip;
static int8_t *choice_slot;

enum { M_IDLE, M_OPEN, M_TEXT, M_WAIT, M_PAUSE, M_CHOICE, M_NUMBER, M_ITEM, M_NEXT, M_CLOSE };
typedef struct { float x, y, size; uint32_t color; int icon; int len; char text[48]; } Op;
#define MAX_OPS 512
static struct {
    int state, ret_state;
    int wait;
    char text[4096];
    const char *p;
    float x, y, new_x, line_h, size;
    uint32_t color;
    int show_fast, line_fast, pause_skip;
    int background, position;
    Op ops[MAX_OPS];
    int nops;
    RpgWin win, choice, number, item, gold;
    int gold_open;
    int num_value, num_index;
    int item_ids[256], nitems;
    int pause_frame;
    /* scrolling text */
    int scroll_active;
    float scroll_pos, scroll_h;
    char scroll_text[4096];
} M;

void rpg_msg_clear(void)
{
    rpg_msg.texts[0] = 0;
    rpg_msg.ntexts = 0;
    rpg_msg.nchoices = 0;
    rpg_msg.choice_cancel_type = 0;
    rpg_msg.num_var = 0;
    rpg_msg.num_digits = 0;
    rpg_msg.item_var = 0;
    rpg_msg.scroll_mode = 0;
    rpg_msg.face_name[0] = 0;
    rpg_msg.face_index = 0;
    rpg_msg.background = 0;
    rpg_msg.position = 2;
}

void rpg_msg_add(const char *line)
{
    int n = strlen(rpg_msg.texts);
    snprintf(rpg_msg.texts + n, sizeof rpg_msg.texts - n, "%s%s", rpg_msg.ntexts ? "\n" : "", line);
    rpg_msg.ntexts++;
}

static int has_text(void) { return rpg_msg.ntexts > 0; }
int rpg_msg_choice(void) { return rpg_msg.nchoices > 0; }
static int num_input(void) { return rpg_msg.num_var > 0; }
static int item_choice(void) { return rpg_msg.item_var > 0; }
int rpg_msg_busy(void) { return has_text() || rpg_msg_choice() || num_input() || item_choice(); }

void rpg_msg_setup_choices(RpgList l, const RpgCmd *c, RpgFrame *f, int indent)
{
    int n = rpg_p(l, c, 0);   /* the choices array: count, then strings */
    rpg_msg.nchoices = 0;
    for (int i = 0; i < n && i < 8; i++)
        snprintf(rpg_msg.choices[rpg_msg.nchoices++], sizeof rpg_msg.choices[0], "%s", rpg_ps(l, c, 1 + i));
    rpg_msg.choice_cancel_type = rpg_p(l, c, 1 + n);
    choice_slot = &f->branch[indent];
}

/* ------------------------------------------------------------------ layout */
static float win_h(void) { return rpg_fitting_height(4); }

static void placement(void)
{
    M.position = rpg_msg.position;
    M.background = rpg_msg.background;
    float h = win_h();
    M.win.y = M.position * (rpg_screen_h - h) / 2;
    M.win.opacity = M.background == 0 ? 255 : 0;
    M.gold.y = M.win.y > 0 ? 0 : rpg_screen_h - M.gold.h;
}

static float new_line_x(void) { return rpg_msg.face_name[0] ? 112 : 0; }

static float calc_line_height(const char *t)
{
    /* the line's height: the largest font size it reaches through \{ */
    float size = rpg_font_size, best = size;
    for (const char *s = t; *s && *s != '\n'; s++) {
        if (s[0] == 0x1b && s[1] == '{') { size += 8; if (size > best) best = size; }
        if (s[0] == 0x1b && s[1] == '}') size -= 8;
    }
    float lh = rpg_line_height();
    return best > lh ? best : lh;
}

static void new_page(void)
{
    M.nops = 0;
    M.size = rpg_font_size;
    M.color = rpg_text_color(0);
    M.x = M.new_x = new_line_x();
    M.y = 0;
    M.line_h = calc_line_height(M.p);
    M.show_fast = M.line_fast = M.pause_skip = 0;
}

static void add_char(const char *c, int len)
{
    Op *o = M.nops ? &M.ops[M.nops - 1] : 0;
    float saved = rpg_font_size;
    rpg_font_size = M.size;
    char tmp[8];
    memcpy(tmp, c, len);
    tmp[len] = 0;
    float w = rpg_text_width(tmp);
    rpg_font_size = saved;
    if (!o || o->icon || o->color != M.color || o->size != M.size || o->y != M.y || o->len + len >= (int)sizeof o->text) {
        if (M.nops >= MAX_OPS)
            return;
        o = &M.ops[M.nops++];
        o->x = M.x;
        o->y = M.y;
        o->size = M.size;
        o->color = M.color;
        o->icon = 0;
        o->len = 0;
    }
    memcpy(o->text + o->len, c, len);
    o->len += len;
    o->text[o->len] = 0;
    M.x += w;
}

static int bracket(const char **p)
{
    const char *s = *p;
    if (*s != '[')
        return 0;
    int v = atoi(s + 1);
    while (*s && *s != ']')
        s++;
    if (*s)
        s++;
    *p = s;
    return v;
}

static void start_input(void);

/* process one step of text; returns 1 when a character was shown (the typing rhythm waits after it) */
static int step(void)
{
    const char *s = M.p;
    if (!*s)
        return -1;
    if (*s == '\n') {
        M.p++;
        M.line_fast = 0;
        M.x = M.new_x;
        M.y += M.line_h;
        M.line_h = calc_line_height(M.p);
        if (M.y + M.line_h > win_h() - 24 && *M.p) {
            M.ret_state = M_TEXT;
            M.state = M_PAUSE;
            M.wait = 10;
            M.pause_frame = 1;   /* then a new page */
        }
        return 0;
    }
    if (*s == '\f') {   /* \x0c: new page */
        M.p++;
        if (*M.p == '\n')
            M.p++;
        M.ret_state = M_TEXT;
        M.state = M_PAUSE;
        M.wait = 10;
        M.pause_frame = 1;
        return 0;
    }
    if (*s == 0x1b) {
        char code = s[1];
        M.p = s + 2;
        switch (code) {
        case 'C': case 'c': M.color = rpg_text_color(bracket(&M.p)); return 0;
        case 'I': case 'i': {
            int icon = bracket(&M.p);
            if (M.nops < MAX_OPS) {
                Op *o = &M.ops[M.nops++];
                o->x = M.x;
                o->y = M.y;
                o->icon = icon;
                o->len = 0;
            }
            M.x += 24;
            return 1;
        }
        case '{': if (M.size <= 64) M.size += 8; return 0;
        case '}': if (M.size >= 16) M.size -= 8; return 0;
        case '$': rpg_win_open(&M.gold); M.gold_open = 1; return 0;
        case '.': M.wait = 15; return 0;
        case '|': M.wait = 60; return 0;
        case '!':
            M.ret_state = M_TEXT;
            M.state = M_PAUSE;
            M.wait = 10;
            M.pause_frame = 0;
            return 0;
        case '>': M.line_fast = 1; return 0;
        case '<': M.line_fast = 0; return 0;
        case '^': M.pause_skip = 1; return 0;
        default: bracket(&M.p); return 0;
        }
    }
    int len = 1;
    unsigned char c = *s;
    if (c >= 0xf0) len = 4;
    else if (c >= 0xe0) len = 3;
    else if (c >= 0xc0) len = 2;
    add_char(s, len);
    M.p += len;
    return 1;
}

static void start_message(void)
{
    rpg_convert_escapes(M.text, sizeof M.text, rpg_msg.texts);
    M.p = M.text;
    new_page();
}

static void start_input(void)
{
    if (rpg_msg_choice()) {
        RpgWin *c = &M.choice;
        float mw = 0;
        for (int i = 0; i < rpg_msg.nchoices; i++) {
            char conv[512];
            rpg_convert_escapes(conv, sizeof conv, rpg_msg.choices[i]);
            float w = rpg_text_width(conv);
            if (w > mw) mw = w;
        }
        float w = (mw + 12 > 96 ? mw + 12 : 96) + 24;
        if (w > rpg_screen_w) w = rpg_screen_w;
        rpg_win_init(c, rpg_screen_w - w, 0, w, rpg_fitting_height(rpg_msg.nchoices));
        c->y = M.win.y >= rpg_screen_h / 2 ? M.win.y - c->h : M.win.y + M.win.h;
        c->item_max = rpg_msg.nchoices;
        c->cancel = rpg_msg.choice_cancel_type > 0;
        c->openness = 0;
        rpg_win_open(c);
        c->active = 1;
        rpg_win_select(c, 0);
        M.state = M_CHOICE;
    } else if (num_input()) {
        RpgWin *n = &M.number;
        int d = rpg_msg.num_digits;
        int maxv = 1;
        for (int i = 0; i < d; i++)
            maxv *= 10;
        int v = rpg_var(rpg_msg.num_var);
        M.num_value = v < 0 ? 0 : (v > maxv - 1 ? maxv - 1 : v);
        M.num_index = 0;
        float w = d * 20 + 24;
        rpg_win_init(n, (rpg_screen_w - w) / 2, 0, w, rpg_fitting_height(1));
        n->y = M.win.y >= rpg_screen_h / 2 ? M.win.y - n->h - 8 : M.win.y + M.win.h + 8;
        n->openness = 0;
        rpg_win_open(n);
        n->active = 1;
        M.state = M_NUMBER;
    } else if (item_choice()) {
        RpgWin *it = &M.item;
        rpg_win_init(it, 0, 0, rpg_screen_w, rpg_fitting_height(4));
        it->y = M.win.y >= rpg_screen_h / 2 ? 0 : rpg_screen_h - it->h;
        it->col_max = 2;
        M.nitems = 0;
        for (int id = 1; id < rpg_count(&rpg_db, RPG_T_ITEM) && M.nitems < 256; id++)
            if (rpg_item_data(id)->itype_id == 2 && rpg_party_item_number(RPG_KIND_ITEM, id) > 0)
                M.item_ids[M.nitems++] = id;
        it->item_max = M.nitems;
        it->openness = 0;
        rpg_win_open(it);
        it->active = 1;
        rpg_win_select(it, 0);
        M.state = M_ITEM;
    } else if (!M.pause_skip) {
        M.ret_state = M_NEXT;
        M.state = M_PAUSE;
        M.wait = 10;
        M.pause_frame = 0;
    } else {
        M.state = M_NEXT;
    }
}

static void finish_input(void)
{
    rpg_msg_clear();
    rpg_win_close(&M.gold);
    M.state = M_NEXT;   /* one frame later: continue or close */
}

/* ------------------------------------------------------------------ scrolling text */
static void scroll_update(void)
{
    if (!M.scroll_active) {
        if (!rpg_msg.scroll_mode || !has_text())
            return;
        rpg_convert_escapes(M.scroll_text, sizeof M.scroll_text, rpg_msg.texts);
        int lines = 1;
        for (const char *s = M.scroll_text; *s; s++)
            lines += *s == '\n';
        M.scroll_h = 1 + lines * rpg_line_height();
        M.scroll_pos = -rpg_screen_h;
        M.scroll_active = 1;
        return;
    }
    int fast = !rpg_msg.scroll_no_fast && (rpg_in.press & (RPG_K_A | RPG_K_C));
    float speed = rpg_msg.scroll_speed * (fast ? 1.0f : 0.5f);
    if (rpg_msg_skip && !rpg_msg.scroll_no_fast)
        speed = 25;
    M.scroll_pos += speed;
    if (M.scroll_pos >= M.scroll_h) {
        M.scroll_active = 0;
        rpg_msg_clear();
    }
}

/* ------------------------------------------------------------------ update */
static int closed_all(void)
{
    return rpg_win_is_closed(&M.win) && rpg_win_is_closed(&M.choice) && rpg_win_is_closed(&M.number) &&
           rpg_win_is_closed(&M.item);
}

static void update_children(void)
{
    rpg_win_update(&M.gold);
    if (M.state == M_CHOICE) {
        int r = rpg_win_update(&M.choice);
        if (r == RPG_WIN_OK || r == RPG_WIN_CANCEL) {
            int v = r == RPG_WIN_OK ? M.choice.index : rpg_msg.choice_cancel_type - 1;
            if (choice_slot)
                *choice_slot = v;
            rpg_win_close(&M.choice);
            finish_input();
        }
    } else {
        rpg_win_update(&M.choice);
    }
    if (M.state == M_NUMBER) {
        RpgWin *n = &M.number;
        rpg_win_update(n);
        if (rpg_win_is_open(n)) {
            int d = rpg_msg.num_digits, last = M.num_index;
            if (rpg_in.repeat & RPG_K_RIGHT)
                if (M.num_index < d - 1 || (rpg_in.trigger & RPG_K_RIGHT)) M.num_index = (M.num_index + 1) % d;
            if (rpg_in.repeat & RPG_K_LEFT)
                if (M.num_index > 0 || (rpg_in.trigger & RPG_K_LEFT)) M.num_index = (M.num_index + d - 1) % d;
            if (M.num_index != last)
                rpg_play_system_se(RPG_SE_CURSOR);
            if (rpg_in.repeat & (RPG_K_UP | RPG_K_DOWN)) {
                rpg_play_system_se(RPG_SE_CURSOR);
                int place = 1;
                for (int i = 0; i < d - 1 - M.num_index; i++)
                    place *= 10;
                int dig = M.num_value / place % 10;
                M.num_value -= dig * place;
                if (rpg_in.repeat & RPG_K_UP) dig = (dig + 1) % 10;
                if (rpg_in.repeat & RPG_K_DOWN) dig = (dig + 9) % 10;
                M.num_value += dig * place;
            }
            if (rpg_in.trigger & RPG_K_C) {
                rpg_play_system_se(RPG_SE_OK);
                rpg_var_set(rpg_msg.num_var, M.num_value);
                rpg_win_close(n);
                finish_input();
            }
        }
    } else {
        rpg_win_update(&M.number);
    }
    if (M.state == M_ITEM) {
        int r = rpg_win_update(&M.item);
        if (r == RPG_WIN_OK || r == RPG_WIN_CANCEL) {
            int v = (r == RPG_WIN_OK && M.item.index >= 0 && M.item.index < M.nitems) ? M.item_ids[M.item.index] : 0;
            rpg_var_set(rpg_msg.item_var, v);
            rpg_win_close(&M.item);
            finish_input();
        }
    } else {
        rpg_win_update(&M.item);
    }
}

void rpg_msg_update(void)
{
    static int inited;
    if (!inited) {
        inited = 1;
        rpg_win_init(&M.win, 0, 0, rpg_screen_w, win_h());
        M.win.openness = 0;
        rpg_win_init(&M.gold, rpg_screen_w - 160, 0, 160, rpg_fitting_height(1));
        M.gold.openness = 0;
        M.choice.openness = M.number.openness = M.item.openness = 0;
    }
    scroll_update();
    rpg_win_update(&M.win);
    update_children();
    if (rpg_msg_skip) {
        M.pause_skip = 1;
        M.show_fast = 1;
    }
    for (int guard = 0; guard < 4096; guard++) {
        switch (M.state) {
        case M_IDLE:
            if (has_text() && !rpg_msg.scroll_mode) {
                rpg_msg.visible = 1;
                placement();
                start_message();
                rpg_win_open(&M.win);
                M.state = M_OPEN;
                continue;
            }
            if (rpg_msg_busy() && !rpg_msg.scroll_mode) {
                /* input without text (a lone choice / number input) */
                rpg_msg.visible = 1;
                placement();
                M.nops = 0;
                M.p = "";
                M.state = M_TEXT;
                continue;
            }
            if (!M.scroll_active)
                rpg_msg.visible = 0;
            return;
        case M_OPEN:
            if (!rpg_win_is_open(&M.win))
                return;
            M.state = M_TEXT;
            continue;
        case M_TEXT: {
            if (M.wait > 0) {
                M.wait--;
                return;
            }
            if (rpg_in.trigger & RPG_K_C)
                M.show_fast = 1;
            int r = step();
            if (r < 0) {
                start_input();
                continue;
            }
            if (M.state != M_TEXT)
                continue;
            if (r == 1 && !M.show_fast && !M.line_fast)
                return;
            continue;
        }
        case M_PAUSE:
            if (rpg_msg_skip) {
                M.wait = 0;
            } else if (M.wait > 0) {
                M.wait--;
                M.pause_frame = M.pause_frame;
                return;
            } else if (!(rpg_in.trigger & (RPG_K_B | RPG_K_C))) {
                return;
            }
            rpg_input_consume();
            if (M.ret_state == M_TEXT && M.pause_frame) {
                new_page();
            }
            M.pause_frame = 0;
            if (M.ret_state == M_NEXT)
                finish_input();
            else
                M.state = M.ret_state;
            if (M.state == M_NEXT)
                return;   /* Fiber.yield after the clear */
            continue;
        case M_CHOICE: case M_NUMBER: case M_ITEM:
            return;
        case M_NEXT:
            if (has_text() && M.background == rpg_msg.background && M.position == rpg_msg.position &&
                !rpg_msg.scroll_mode) {
                start_message();
                M.state = M_TEXT;
                continue;
            }
            if (rpg_msg_busy() && !has_text() && !rpg_msg.scroll_mode) {
                M.state = M_TEXT;
                M.p = "";
                continue;
            }
            rpg_win_close(&M.win);
            M.state = M_CLOSE;
            return;
        case M_CLOSE:
            if (!closed_all())
                return;
            rpg_msg.visible = 0;
            M.state = M_IDLE;
            if (has_text())
                continue;
            return;
        }
    }
}

/* ------------------------------------------------------------------ draw */
static void draw_choice_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    char conv[512];
    rpg_convert_escapes(conv, sizeof conv, rpg_msg.choices[i]);
    rpg_draw_text_ex(r.x + 4, r.y, conv);
}

static void draw_item_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    int id = M.item_ids[i];
    rpg_icon(rpg_item_icon(RPG_KIND_ITEM, id), r.x + 4, r.y, 1);
    rpg_text(r.x + 28, r.y, rpg_item_name(RPG_KIND_ITEM, id), rpg_text_color(0));
    char n[16];
    snprintf(n, sizeof n, ":%2d", rpg_party_item_number(RPG_KIND_ITEM, id));
    rpg_text_box(r.x, r.y, r.w - 4, r.h, n, rpg_text_color(0), 2);
}

void rpg_msg_draw(void)
{
    if (M.scroll_active) {
        float y = -M.scroll_pos;
        rpg_draw_text_ex(4 + 12, y + 12, M.scroll_text);
    }
    if (M.win.openness > 0 && (rpg_msg.visible || M.state != M_IDLE)) {
        if (M.background == 1) {
            /* the dim band: black at 160 alpha with 12 px gradient edges */
            float y = M.win.y, h = M.win.h, a = M.win.openness / 255.f;
            for (int i = 0; i < 12; i++) {
                int al = (int)(160 * (i + 0.5f) / 12 * a);
                rpg_fill(0, y + i, rpg_screen_w, 1, rpg_rgba(0, 0, 0, al));
                rpg_fill(0, y + h - 1 - i, rpg_screen_w, 1, rpg_rgba(0, 0, 0, al));
            }
            rpg_fill(0, y + 12, rpg_screen_w, h - 24, rpg_rgba(0, 0, 0, (int)(160 * a)));
        }
        rpg_win_draw_frame(&M.win);
        if (rpg_win_contents_begin(&M.win)) {
            if (rpg_msg.face_name[0])
                rpg_face(rpg_msg.face_name, rpg_msg.face_index, 0, 0, 96, 1);
            for (int i = 0; i < M.nops; i++) {
                Op *o = &M.ops[i];
                if (o->icon) {
                    rpg_icon(o->icon, o->x, o->y, 1);
                } else {
                    float saved = rpg_font_size;
                    rpg_font_size = o->size;
                    rpg_text(o->x, o->y, o->text, o->color);
                    rpg_font_size = saved;
                }
            }
            if (rpg_hooks.message_draw)
                rpg_hooks.message_draw(M.win.x, M.win.y, M.win.w, M.win.h);
            rpg_win_contents_end(&M.win);
            if (M.state == M_PAUSE && M.wait <= 0)
                rpg_pause_sign(M.win.x + M.win.w / 2 - 8, M.win.y + M.win.h - 16, M.win.frame);
        }
    }
    if (M.gold.openness > 0) {
        rpg_win_draw_frame(&M.gold);
        if (rpg_win_contents_begin(&M.gold)) {
            char g[32];
            snprintf(g, sizeof g, "%d", rpg.party.gold);
            const char *unit = RPG_STR(rpg_system()->currency_unit);
            float uw = rpg_text_width(unit);
            rpg_text_box(0, 0, M.gold.w - 24 - uw - 2, 24, g, rpg_text_color(0), 2);
            rpg_text_box(0, 0, M.gold.w - 24, 24, unit, rpg_text_color(16), 2);
            rpg_win_contents_end(&M.gold);
        }
    }
    if (M.choice.openness > 0)
        rpg_win_draw(&M.choice, draw_choice_item, 0);
    if (M.number.openness > 0) {
        rpg_win_draw_frame(&M.number);
        if (rpg_win_contents_begin(&M.number)) {
            int d = rpg_msg.num_digits;
            char s[16];
            snprintf(s, sizeof s, "%0*d", d, M.num_value);
            for (int i = 0; i < d; i++) {
                char c[2] = {s[i], 0};
                rpg_text_box(i * 20 + 1, 0, 20, 24, c, rpg_text_color(0), 1);
            }
            rpg_win_contents_end(&M.number);
            rpg_cursor(M.number.x + 12 + M.num_index * 20, M.number.y + 12, 20, 24, M.number.frame, 1);
        }
    }
    if (M.item.openness > 0)
        rpg_win_draw(&M.item, draw_item_item, 0);
}

int rpg_msg_window_y(void) { return (int)M.win.y; }
int rpg_msg_open(void) { return M.win.openness > 0; }
