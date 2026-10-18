/* VCPE RPG menu scenes, VX Ace layout: Scene_Menu (commands, gold, party status), Scene_Save / Scene_Load,
 * Scene_End, and the Window_Base drawing helpers they share (actor name / level / class / icons / gauges, item
 * names, currency). Scene_MenuBase's blurred snapshot background is the map drawn again under a dark veil.
 * Item, Skill, Equip, Status, Shop and Name input are in vcpe_rpg_menu2.c. */
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"
#include "vcpe_rpg_menu.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ Window_Base helpers */
uint32_t rpg_color_normal(void) { return rpg_text_color(0); }
uint32_t rpg_color_system(void) { return rpg_text_color(16); }
uint32_t rpg_color_crisis(void) { return rpg_text_color(17); }
uint32_t rpg_color_knockout(void) { return rpg_text_color(18); }
uint32_t rpg_color_power_up(void) { return rpg_text_color(24); }
uint32_t rpg_color_power_down(void) { return rpg_text_color(25); }
uint32_t rpg_color_mp_cost(void) { return rpg_text_color(23); }
uint32_t rpg_color_tp_cost(void) { return rpg_text_color(29); }
uint32_t rpg_color_enabled(uint32_t c, int enabled) { return enabled ? c : (c & 0x00ffffff) | (((c >> 24) * 160 / 255) << 24); }

uint32_t rpg_hp_color(const RpgBattler *b)
{
    if (rpg_battler_dead(b)) return rpg_color_knockout();
    if (b->hp < rpg_battler_mhp(b) / 4) return rpg_color_crisis();
    return rpg_color_normal();
}

uint32_t rpg_mp_color(const RpgBattler *b)
{
    return b->mp < rpg_battler_mmp(b) / 4 ? rpg_color_crisis() : rpg_color_normal();
}

void rpg_draw_text_c(float x, float y, float w, const char *s, uint32_t c, int align)
{
    rpg_text_box(x, y, w, rpg_line_height(), s, c, align);
}

void rpg_draw_current_max(float x, float y, float w, int cur, int max, uint32_t c1, uint32_t c2)
{
    char a[16], b[16];
    snprintf(a, sizeof a, "%d", cur);
    snprintf(b, sizeof b, "%d", max);
    float xr = x + w;
    if (w < 96) {
        rpg_draw_text_c(xr - 40, y, 42, a, c1, 2);
    } else {
        rpg_draw_text_c(xr - 92, y, 42, a, c1, 2);
        rpg_draw_text_c(xr - 52, y, 12, "/", c2, 2);
        rpg_draw_text_c(xr - 42, y, 42, b, c2, 2);
    }
}

void rpg_draw_actor_name(const RpgActorState *a, float x, float y, float w)
{
    rpg_draw_text_c(x, y, w, a->name, rpg_hp_color(&a->b), 0);
}

void rpg_draw_actor_class(const RpgActorState *a, float x, float y, float w)
{
    rpg_draw_text_c(x, y, w, RPG_STR(rpg_class_data(a->class_id)->name), rpg_color_normal(), 0);
}

void rpg_draw_actor_nickname(const RpgActorState *a, float x, float y, float w)
{
    rpg_draw_text_c(x, y, w, a->nickname, rpg_color_normal(), 0);
}

void rpg_draw_actor_level(const RpgActorState *a, float x, float y)
{
    char lv[16];
    snprintf(lv, sizeof lv, "%d", a->level);
    rpg_draw_text_c(x, y, 32, rpg_term_basic(1), rpg_color_system(), 0);
    rpg_draw_text_c(x + 32, y, 24, lv, rpg_color_normal(), 2);
}

void rpg_draw_actor_icons(const RpgBattler *b, float x, float y, float w)
{
    int icons[40], n = 0;
    for (int i = 0; i < b->nstates && n < 40; i++) {
        int ic = rpg_state_data(b->states[i].id)->icon_index;
        if (ic)
            icons[n++] = ic;
    }
    for (int p = 0; p < 8 && n < 40; p++) {
        int lv = b->buffs[p];
        if (lv > 0) icons[n++] = 64 + (lv - 1) * 8 + p;
        else if (lv < 0) icons[n++] = 80 + (-lv - 1) * 8 + p;
    }
    int max = (int)(w / 24);
    for (int i = 0; i < n && i < max; i++)
        rpg_icon(icons[i], x + 24 * i, y, 1);
}

void rpg_draw_actor_hp(const RpgBattler *b, float x, float y, float w)
{
    int mhp = rpg_battler_mhp(b);
    rpg_gauge(x, y, w, mhp ? (float)b->hp / mhp : 0, rpg_text_color(20), rpg_text_color(21));
    rpg_draw_text_c(x, y, 30, rpg_term_basic(3), rpg_color_system(), 0);
    rpg_draw_current_max(x, y, w, b->hp, mhp, rpg_hp_color(b), rpg_color_normal());
}

void rpg_draw_actor_mp(const RpgBattler *b, float x, float y, float w)
{
    int mmp = rpg_battler_mmp(b);
    rpg_gauge(x, y, w, mmp ? (float)b->mp / mmp : 0, rpg_text_color(22), rpg_text_color(23));
    rpg_draw_text_c(x, y, 30, rpg_term_basic(5), rpg_color_system(), 0);
    rpg_draw_current_max(x, y, w, b->mp, mmp, rpg_mp_color(b), rpg_color_normal());
}

void rpg_draw_actor_tp(const RpgBattler *b, float x, float y, float w)
{
    char t[16];
    snprintf(t, sizeof t, "%d", (int)b->tp);
    rpg_gauge(x, y, w, b->tp / 100.f, rpg_text_color(28), rpg_text_color(29));
    rpg_draw_text_c(x, y, 30, rpg_term_basic(7), rpg_color_system(), 0);
    rpg_draw_text_c(x + w - 42, y, 42, t, rpg_color_normal(), 2);
}

void rpg_draw_actor_face(const RpgActorState *a, float x, float y, float size, int enabled)
{
    rpg_face(a->face_name, a->face_index, x, y, size, enabled);
}

void rpg_draw_actor_simple_status(const RpgActorState *a, float x, float y)
{
    float lh = rpg_line_height();
    rpg_draw_actor_name(a, x, y, 112);
    rpg_draw_actor_level(a, x, y + lh);
    rpg_draw_actor_icons(&a->b, x, y + lh * 2, 96);
    rpg_draw_actor_class(a, x + 120, y, 112);
    rpg_draw_actor_hp(&a->b, x + 120, y + lh, 124);
    rpg_draw_actor_mp(&a->b, x + 120, y + lh * 2, 124);
}

void rpg_draw_actor_param(const RpgActorState *a, float x, float y, int p)
{
    char v[16];
    snprintf(v, sizeof v, "%d", rpg_battler_param(&a->b, p));
    rpg_draw_text_c(x, y, 120, rpg_term_param(p), rpg_color_system(), 0);
    rpg_draw_text_c(x + 120, y, 36, v, rpg_color_normal(), 2);
}

void rpg_draw_item_name(int kind, int id, float x, float y, int enabled, float w)
{
    if (id <= 0)
        return;
    rpg_icon(rpg_item_icon(kind, id), x, y, enabled);
    rpg_draw_text_c(x + 24, y, w - 24, rpg_item_name(kind, id), rpg_color_enabled(rpg_color_normal(), enabled), 0);
}

void rpg_draw_currency(int value, const char *unit, float x, float y, float w)
{
    char v[24];
    snprintf(v, sizeof v, "%d", value);
    float cx = rpg_text_width(unit);
    rpg_draw_text_c(x, y, w - cx - 2, v, rpg_color_normal(), 2);
    rpg_draw_text_c(x, y, w, unit, rpg_color_system(), 2);
}

void rpg_draw_character(const char *name, int index, float x, float y)
{
    /* Window_Base#draw_character: the standing frame, bottom-centre at (x, y) */
    int img = rpg_image("characters", name);
    if (img < 0)
        return;
    int single = name[0] == '$' || (name[0] == '!' && name[1] == '$');
    int w = rpg_image_w(img), h = rpg_image_h(img);
    int cw = single ? w / 3 : w / 12, ch = single ? h / 4 : h / 8;
    int n = single ? 0 : index;
    rpg_draw_part(img, (n % 4 * 3 + 1) * cw, (n / 4 * 4) * ch, cw, ch, x - cw / 2.f, y - ch, 1, 1, 0, 0xffffffff, 0);
}

/* Scene_MenuBase background: the map under a 16,16,16,128 veil */
void rpg_menu_background(void)
{
    rpg_map_draw();
    rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, rpg_rgba(16, 16, 16, 128));
}

void rpg_help_draw(RpgWin *w, const char *text)
{
    rpg_win_draw_frame(w);
    if (rpg_win_contents_begin(w)) {
        char conv[1024];
        rpg_convert_escapes(conv, sizeof conv, text ? text : "");
        rpg_draw_text_ex(4, 0, conv);
        rpg_win_contents_end(w);
    }
}

/* ------------------------------------------------------------------ Scene_Menu */
enum { C_ITEM, C_SKILL, C_EQUIP, C_STATUS, C_FORMATION, C_SAVE, C_END };
static struct {
    RpgWin cmd, status, gold;
    int cmds[16], ncmds;
    int pending;
    int mode;   /* 0 commands, 1 personal, 2 formation */
} MN;
static int last_command = -1;
RpgMenuHooks rpg_menu_hooks;

static int cmd_enabled(int c)
{
    switch (c) {
    case C_ITEM: case C_SKILL: case C_EQUIP: case C_STATUS: return rpg.party.nmembers > 0;
    case C_FORMATION: return rpg.party.nmembers >= 2 && !rpg.formation_disabled;
    case C_SAVE: return !rpg.save_disabled;
    }
    return 1;
}

static int menu_cmd_enabled(RpgWin *w, int i)
{
    (void)w;
    return cmd_enabled(MN.cmds[i]);
}

static const char *cmd_name(int c)
{
    static const int term[] = {4, 5, 6, 7, 8, 9, 10};
    if (c >= 100 && rpg_menu_hooks.command_name)
        return rpg_menu_hooks.command_name(c);
    return rpg_term_command(term[c]);
}

static void menu_start(void)
{
    MN.ncmds = 0;
    if (rpg_menu_hooks.commands) {
        MN.ncmds = rpg_menu_hooks.commands(MN.cmds, 16);
    } else {
        for (int c = C_ITEM; c <= C_END; c++)
            MN.cmds[MN.ncmds++] = c;
    }
    float cw = 160;
    rpg_win_init(&MN.cmd, 0, 0, cw, rpg_fitting_height(MN.ncmds));
    MN.cmd.item_max = MN.ncmds;
    MN.cmd.enabled = menu_cmd_enabled;
    MN.cmd.active = 1;
    rpg_win_select(&MN.cmd, 0);
    for (int i = 0; i < MN.ncmds; i++)
        if (MN.cmds[i] == last_command)
            rpg_win_select(&MN.cmd, i);
    rpg_win_init(&MN.gold, 0, rpg_screen_h - rpg_fitting_height(1), 160, rpg_fitting_height(1));
    rpg_win_init(&MN.status, cw, 0, rpg_screen_w - cw, rpg_screen_h);
    MN.status.item_max = rpg.party.nmembers;
    MN.status.item_h = (rpg_screen_h - 24) / 4;
    MN.pending = -1;
    MN.mode = 0;
}

static void status_select_last(void)
{
    RpgActorState *a = rpg_party_menu_actor();
    int idx = 0;
    for (int i = 0; i < rpg.party.nmembers; i++)
        if (a && rpg.party.members[i] == a->actor_id)
            idx = i;
    rpg_win_select(&MN.status, idx);
    MN.status.active = 1;
}

static void menu_update(void)
{
    int r = rpg_win_update(&MN.cmd);
    if (r == RPG_WIN_CANCEL) {
        rpg_scene_return();
        return;
    }
    if (r == RPG_WIN_OK) {
        int c = MN.cmds[MN.cmd.index];
        last_command = c;
        switch (c) {
        case C_ITEM: rpg_scene_call(RPG_SCENE_ITEM); break;
        case C_SKILL: case C_EQUIP: case C_STATUS: status_select_last(); MN.mode = 1; break;
        case C_FORMATION: status_select_last(); MN.mode = 2; break;
        case C_SAVE: rpg_scene_call(RPG_SCENE_SAVE); break;
        case C_END: rpg_scene_call(RPG_SCENE_END); break;
        default:
            if (rpg_menu_hooks.command_ok)
                rpg_menu_hooks.command_ok(c);
            MN.cmd.active = 1;
        }
        return;
    }
    r = rpg_win_update(&MN.status);
    if (MN.mode == 1) {
        if (r == RPG_WIN_OK) {
            rpg_party_set_menu_actor(rpg.party.members[MN.status.index]);
            int c = MN.cmds[MN.cmd.index];
            rpg_scene_call(c == C_SKILL ? RPG_SCENE_SKILL : c == C_EQUIP ? RPG_SCENE_EQUIP : RPG_SCENE_STATUS);
        } else if (r == RPG_WIN_CANCEL) {
            MN.status.index = -1;
            MN.cmd.active = 1;
            MN.mode = 0;
        }
    } else if (MN.mode == 2) {
        if (r == RPG_WIN_OK) {
            if (MN.pending >= 0) {
                rpg_party_swap_order(MN.status.index, MN.pending);
                MN.pending = -1;
            } else {
                MN.pending = MN.status.index;
            }
            MN.status.active = 1;
        } else if (r == RPG_WIN_CANCEL) {
            if (MN.pending >= 0) {
                MN.pending = -1;
                MN.status.active = 1;
            } else {
                MN.status.index = -1;
                MN.cmd.active = 1;
                MN.mode = 0;
            }
        }
    }
}

static void cmd_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    int c = MN.cmds[i];
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, cmd_name(c), rpg_color_enabled(rpg_color_normal(), cmd_enabled(c)), 0);
}

static void status_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    RpgActorState *a = rpg_actor(rpg.party.members[i]);
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids), en = 0;
    for (int k = 0; k < n; k++)
        en |= ids[k] == a->actor_id;
    if (i == MN.pending)
        rpg_fill(r.x, r.y, r.w, r.h, rpg_text_color(19) & 0x80ffffff);
    float fs = w->item_h - 2 < 96 ? w->item_h - 2 : 96;
    rpg_draw_actor_face(a, r.x + 1, r.y + 1, fs, en);
    rpg_draw_actor_simple_status(a, r.x + 108, r.y + (w->item_h - rpg_line_height() * 3) / 2);
}

static void menu_draw(void)
{
    rpg_menu_background();
    rpg_win_draw(&MN.cmd, cmd_item, 0);
    rpg_win_draw_frame(&MN.gold);
    if (rpg_win_contents_begin(&MN.gold)) {
        rpg_draw_currency(rpg.party.gold, RPG_STR(rpg_system()->currency_unit), 4, 0, MN.gold.w - 24 - 8);
        rpg_win_contents_end(&MN.gold);
    }
    MN.status.item_max = rpg.party.nmembers;
    rpg_win_draw(&MN.status, status_item, 0);
    if (rpg_menu_hooks.draw)
        rpg_menu_hooks.draw();
}

/* ------------------------------------------------------------------ Scene_File (save / load) */
static struct {
    int save, index, top;
    RpgWin help;
    RpgSaveHeader hdr[RPG_SAVE_FILES];
    int has[RPG_SAVE_FILES];
    int state;   /* 0 choosing, 1 fading out (load) */
} F;

static void file_start(int save)
{
    F.save = save;
    rpg_win_init(&F.help, 0, 0, rpg_screen_w, rpg_fitting_height(1));
    uint32_t newest = 0;
    int newest_i = 0;
    for (int i = 0; i < RPG_SAVE_FILES; i++) {
        F.has[i] = !rpg_save_header(i, &F.hdr[i]);
        if (F.has[i] && F.hdr[i].frame_count >= newest) {
            newest = F.hdr[i].frame_count;
            newest_i = i;
        }
    }
    int last = rpg_save_last_slot();
    F.index = save ? (last >= 0 ? last : 0) : (last >= 0 ? last : newest_i);
    F.top = F.index - 2;
    if (F.top > RPG_SAVE_FILES - 4) F.top = RPG_SAVE_FILES - 4;
    if (F.top < 0) F.top = 0;
    F.state = 0;
}
static void save_start(void) { file_start(1); }
static void load_start(void) { file_start(0); }

static void load_done(void)
{
    /* Game_System#on_after_load: the music of the save */
    rpg_bgm_play_at(rpg.bgm_on_save.name, rpg.bgm_on_save.volume, rpg.bgm_on_save.pitch, rpg.bgm_on_save_pos);
    rpg_bgs_play(rpg.bgs_on_save.name, rpg.bgs_on_save.volume, rpg.bgs_on_save.pitch);
    rpg_scene_goto(RPG_SCENE_MAP);
}

static void ensure(void)
{
    if (F.index < F.top) F.top = F.index;
    if (F.index > F.top + 3) F.top = F.index - 3;
}

void rpg_scene_fade(int frames, int dir, void (*then)(void));
int rpg_scene_fade_step(void);
extern int rpg_brightness;

static void file_update(void)
{
    if (rpg_scene_fade_step())
        return;
    if (F.state)
        return;
    if (rpg_in.trigger & RPG_K_C) {
        if (F.save) {
            if (!rpg_save_write(F.index)) {
                rpg_play_system_se(RPG_SE_SAVE);
                rpg_scene_return();
            } else {
                rpg_play_system_se(RPG_SE_BUZZER);
            }
        } else {
            if (F.has[F.index] && !rpg_save_read(F.index)) {
                rpg_play_system_se(RPG_SE_LOAD);
                F.state = 1;
                rpg_bgm_fade(1000);
                rpg_bgs_fade(1000);
                rpg_scene_fade(60, -1, load_done);
            } else {
                rpg_play_system_se(RPG_SE_BUZZER);
            }
        }
        return;
    }
    if (rpg_in.trigger & RPG_K_B) {
        rpg_play_system_se(RPG_SE_CANCEL);
        rpg_scene_return();
        return;
    }
    int last = F.index;
    if (rpg_in.repeat & RPG_K_DOWN)
        if (F.index < RPG_SAVE_FILES - 1 || (rpg_in.trigger & RPG_K_DOWN)) F.index = (F.index + 1) % RPG_SAVE_FILES;
    if (rpg_in.repeat & RPG_K_UP)
        if (F.index > 0 || (rpg_in.trigger & RPG_K_UP)) F.index = (F.index - 1 + RPG_SAVE_FILES) % RPG_SAVE_FILES;
    if (rpg_in.trigger & RPG_K_R && F.top + 4 < RPG_SAVE_FILES) {
        F.top += 4;
        if (F.top > RPG_SAVE_FILES - 4) F.top = RPG_SAVE_FILES - 4;
        F.index = F.index + 4 < RPG_SAVE_FILES - 1 ? F.index + 4 : RPG_SAVE_FILES - 1;
    }
    if (rpg_in.trigger & RPG_K_L && F.top > 0) {
        F.top -= 4;
        if (F.top < 0) F.top = 0;
        F.index = F.index - 4 > 0 ? F.index - 4 : 0;
    }
    ensure();
    if (F.index != last)
        rpg_play_system_se(RPG_SE_CURSOR);
}

static void file_draw(void)
{
    if (F.save)
        rpg_menu_background();
    else
        rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, 0xff000000);
    rpg_help_draw(&F.help, F.save ? rpg_vocab.save_message : rpg_vocab.load_message);
    float y0 = F.help.h, fh = (rpg_screen_h - F.help.h) / 4;
    for (int k = 0; k < 4; k++) {
        int i = F.top + k;
        RpgWin w;
        rpg_win_init(&w, 0, y0 + k * fh, rpg_screen_w, fh);
        rpg_win_draw_frame(&w);
        if (!rpg_win_contents_begin(&w))
            continue;
        char name[32];
        snprintf(name, sizeof name, "%s %d", rpg_vocab.file, i + 1);
        rpg_draw_text_c(4, 0, 200, name, rpg_color_normal(), 0);
        float nw = rpg_text_width(name);
        if (F.has[i]) {
            for (int c = 0; c < F.hdr[i].nchars; c++)
                rpg_draw_character(F.hdr[i].chars[c], F.hdr[i].char_index[c], 152 + c * 48, fh - 24 - 2);
            uint32_t sec = F.hdr[i].frame_count / 60;
            char t[32];
            snprintf(t, sizeof t, "%02u:%02u:%02u", sec / 3600, sec / 60 % 60, sec % 60);
            rpg_draw_text_c(0, fh - 24 - rpg_line_height(), w.w - 24 - 4, t, rpg_color_normal(), 2);
            if (F.hdr[i].map_name[0])
                rpg_draw_text_c(240, 0, w.w - 24 - 244, F.hdr[i].map_name, rpg_color_normal(), 2);
        }
        rpg_win_contents_end(&w);
        if (i == F.index)
            rpg_cursor(w.x + 12, w.y + 12, nw + 8, rpg_line_height(), w.frame + rpg.frame_count, 1);
    }
}


/* ------------------------------------------------------------------ Scene_End */
static struct { RpgWin cmd; int state; } E;
void rpg_fadeout_all(int ms);

static void end_start(void)
{
    float w = 160, h = rpg_fitting_height(3);
    rpg_win_init(&E.cmd, (rpg_screen_w - w) / 2, (rpg_screen_h - h) / 2, w, h);
    E.cmd.item_max = 3;
    E.cmd.openness = 0;
    rpg_win_open(&E.cmd);
    E.cmd.active = 1;
    rpg_win_select(&E.cmd, 0);
    E.state = 0;
}

static void end_to_title(void) { rpg_scene_goto(RPG_SCENE_TITLE); }
extern int rpg_quit_request;

static void end_update(void)
{
    if (rpg_scene_fade_step())
        return;
    int r = rpg_win_update(&E.cmd);
    if (E.state == 0) {
        if (r == RPG_WIN_CANCEL || (r == RPG_WIN_OK && E.cmd.index == 2)) {
            rpg_scene_return();
        } else if (r == RPG_WIN_OK) {
            E.state = E.cmd.index == 0 ? 1 : 2;
            rpg_win_close(&E.cmd);
        }
    } else if (E.state <= 2 && rpg_win_is_closed(&E.cmd)) {
        if (E.state == 2)
            rpg_quit_request = 1;
        E.state = 3;
        rpg_bgm_fade(1000);
        rpg_bgs_fade(1000);
        rpg_me_stop();
        rpg_scene_fade(60, -1, end_to_title);
    }
}

static void end_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    static const int t[3] = {21, 20, 22};
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, rpg_term_command(t[i]), rpg_color_normal(), 0);
}

static void end_draw(void)
{
    rpg_menu_background();
    rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, rpg_rgba(0, 0, 0, 64));
    rpg_win_draw(&E.cmd, end_item, 0);
}

void rpg_menu2_define(void);

void rpg_menu_define(void)
{
    static const RpgSceneDef menu = {menu_start, menu_update, menu_draw, 0};
    static const RpgSceneDef save = {save_start, file_update, file_draw, 0};
    static const RpgSceneDef load = {load_start, file_update, file_draw, 0};
    static const RpgSceneDef end = {end_start, end_update, end_draw, 0};
    rpg_scene_define(RPG_SCENE_MENU, &menu);
    rpg_scene_define(RPG_SCENE_SAVE, &save);
    rpg_scene_define(RPG_SCENE_LOAD, &load);
    rpg_scene_define(RPG_SCENE_END, &end);
    rpg_menu2_define();
}

void rpg_save_define(void) {}
