/* VCPE RPG scenes: SceneManager (goto / call / return), Graphics.brightness, and the title, map and game over scenes.
 *
 * RGSS scenes block inside loops (fades, closing windows); here each scene is a state machine updated once per frame
 * and the blocking loops are states: a fade runs N frames of "update_for_fade" (Scene_Map: windows + the map without
 * the main interpreter), then continues with what came after it. */
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"
#include "vcpe_spr.h"
#include <string.h>
#include <stdio.h>

int rpg_transition;
int rpg_scene_hold;
int rpg_quit_request;
int rpg_brightness = 255;            /* Graphics.brightness */
static float white_alpha;            /* Scene_Map's white fade (viewport colour) */
static RpgSceneDef scenes[RPG_SCENE_GAME + 32];
static int cur = RPG_SCENE_NONE, stack[16], sp;
static int pending = -1, pending_kind;   /* 0 goto, 1 call, 2 return */
int rpg_map_reopen(void);
void rpg_balloons_update(void);
void rpg_balloons_reset(void);
void rpg_screen_draw_flash(const RpgScreen *s);
void rpg_menu_define(void);
void rpg_battle_define(void);
void rpg_save_define(void);
int rpg_saves_exist(void);

void rpg_scene_define(int id, const RpgSceneDef *def)
{
    if (id >= 0 && id < (int)(sizeof scenes / sizeof *scenes))
        scenes[id] = *def;
}

void rpg_scene_goto(int s)
{
    pending = s;
    pending_kind = 0;
}

void rpg_scene_call(int s)
{
    pending = s;
    pending_kind = 1;
}

void rpg_scene_return(void)
{
    pending = sp > 0 ? stack[sp - 1] : RPG_SCENE_TITLE;
    pending_kind = 2;
}

int rpg_scene_current(void) { return cur; }
int rpg_scene_changing(void) { return pending >= 0; }

static void switch_scene(void)
{
    int s = pending, kind = pending_kind;
    pending = -1;
    if (cur != RPG_SCENE_NONE && scenes[cur].terminate)
        scenes[cur].terminate();
    if (kind == 1 && sp < 16)
        stack[sp++] = cur;
    else if (kind == 2 && sp > 0)
        sp--;
    cur = s;
    if (scenes[cur].start)
        scenes[cur].start();
}

void rpg_frame(uint32_t held)
{
    rpg_input_set(held);
    if (pending >= 0 && !rpg_scene_hold)
        switch_scene();
    rpg_audio_update();
    if (scenes[cur].update)
        scenes[cur].update();
    if (pending >= 0 && pending == RPG_SCENE_NONE)
        pending = -1;
    rpg.frame_count++;
}

void rpg_draw(void)
{
    if (scenes[cur].draw)
        scenes[cur].draw();
    if (white_alpha > 0)
        rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, rpg_rgba(255, 255, 255, (int)white_alpha));
    if (rpg_brightness < 255)
        rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, rpg_rgba(0, 0, 0, 255 - rpg_brightness));
}

/* a blocking fade as a state: frames left, direction, and whether the map keeps updating */
static struct {
    int frames, total, dir, white;   /* dir: -1 out, +1 in */
    void (*then)(void);
} fade;

static void fade_start(int frames, int dir, int white, void (*then)(void))
{
    fade.frames = frames;
    fade.total = frames;
    fade.dir = dir;
    fade.white = white;
    fade.then = then;
    if (frames <= 0) {
        if (dir < 0) rpg_brightness = 0;
        else rpg_brightness = 255;
        if (then) then();
    }
}

/* one frame of the fade; 1 while it runs */
static int fade_step(void)
{
    if (fade.frames <= 0)
        return 0;
    int i = fade.total - fade.frames;
    int v = 255 * (i + 1) / fade.total;
    if (fade.white)
        white_alpha = fade.dir < 0 ? v : 255 - v;
    else
        rpg_brightness = fade.dir < 0 ? 255 - v : v;
    fade.frames--;
    if (fade.frames == 0) {
        if (fade.white && fade.dir > 0)
            white_alpha = 0;
        void (*t)(void) = fade.then;
        fade.then = 0;
        if (t)
            t();
    }
    return 1;
}

/* Scene_Base#fadeout_all: music and screen out over time ms */
static int fadeout_all_frames;
static void (*fadeout_all_then)(void);
void rpg_fadeout_all(int ms)
{
    rpg_bgm_fade(ms);
    rpg_bgs_fade(ms);
    rpg_me_stop();
    fadeout_all_frames = ms * 60 / 1000;
    fade_start(fadeout_all_frames, -1, 0, fadeout_all_then);
}

/* ------------------------------------------------------------------ new game */
void rpg_new_game(void)
{
    memset(&rpg, 0, sizeof rpg);
    memset(&rpg_temp, 0, sizeof rpg_temp);
    rpg_msg_clear();
    rpg.party.max_battle_members = 4;
    rpg.name_display = 1;
    rpg.player.vehicle = -1;
    rpg.player.followers_visible = 1;
    const RpgSystem *s = rpg_system();
    memcpy(rpg.window_tone, s->window_tone, sizeof rpg.window_tone);
    rpg_screen_clear(&rpg.screen);
    rpg_interp_clear(&rpg.interp);
    rpg.interp.main = 1;
    RpgChar *p = &rpg.player.ch;
    p->kind = 1;
    p->move_speed = 4;
    p->move_frequency = 6;
    p->walk_anime = 1;
    p->opacity = 255;
    p->direction = 2;
    p->pattern = 1;
    p->priority_type = 1;
    p->original_direction = 2;
    p->original_pattern = 1;
    p->move_succeed = 1;
    p->route = -1;
    p->transparent = s->opt_transparent;
    rpg_party_setup_starting();
    if (rpg_hooks.new_game)
        rpg_hooks.new_game();
    rpg_map_load(s->start_map_id);
    rpg_player_moveto(s->start_x, s->start_y);
    rpg_player_refresh();
    rpg.frame_count = 0;
}

/* ------------------------------------------------------------------ title */
static struct {
    RpgWin cmd;
    int state;   /* 0 choosing, 1 closing for new game, 2 closing for continue, 3 fading out */
    int bg1, bg2;
    int saves;   /* checked once: it opens every save file */
} T;

static int title_enabled(RpgWin *w, int i)
{
    (void)w;
    return i != 1 || T.saves;
}

static void title_start(void)
{
    const RpgSystem *s = rpg_system();
    T.bg1 = rpg_image("titles1", RPG_STR(s->title1_name));
    T.bg2 = rpg_image("titles2", RPG_STR(s->title2_name));
    /* Window_TitleCommand: 160 wide, centred, y = (Graphics.height * 1.6 - height) / 2 */
    float w = 160, h = rpg_fitting_height(3);
    rpg_win_init(&T.cmd, (int)(rpg_screen_w - w) / 2, (int)(rpg_screen_h * 1.6f - h) / 2, w, h);
    T.cmd.item_max = 3;
    T.cmd.cancel = 0;
    T.saves = rpg_saves_exist();
    T.cmd.enabled = title_enabled;
    T.cmd.openness = 0;
    rpg_win_open(&T.cmd);
    T.cmd.active = 1;
    rpg_win_select(&T.cmd, T.saves ? 1 : 0);
    T.state = 0;
    rpg_bgm_play(RPG_STR(s->title_bgm.name), s->title_bgm.volume, s->title_bgm.pitch);
    rpg_bgs_stop();
    rpg_me_stop();
    rpg_brightness = 0;
    fade_start(20, 1, 0, 0);   /* transition_speed 20 (from black) */
}

static void title_new_game_go(void)
{
    rpg_bgm_stop();
    rpg_bgs_stop();
    rpg_map_autoplay();
    rpg_scene_goto(RPG_SCENE_MAP);
}

static void title_update(void)
{
    if (fade_step())
        return;
    int r = rpg_win_update(&T.cmd);
    if (T.state == 0 && r == RPG_WIN_OK) {
        if (T.cmd.index == 0) {
            rpg_new_game();
            T.state = 1;
        } else if (T.cmd.index == 1) {
            T.state = 2;
        } else {
            T.state = 4;
        }
        rpg_win_close(&T.cmd);
    }
    if ((T.state == 1 || T.state == 2 || T.state == 4) && rpg_win_is_closed(&T.cmd)) {
        if (T.state == 1) {
            T.state = 3;
            fadeout_all_then = title_new_game_go;
            rpg_fadeout_all(1000);
        } else if (T.state == 2) {
            T.state = 5;
            rpg_scene_call(RPG_SCENE_LOAD);
        } else {
            T.state = 3;
            fadeout_all_then = 0;
            rpg_fadeout_all(1000);
            rpg_quit_request = 1;
        }
    }
}

static void title_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    const RpgSystem *s = rpg_system();
    /* Vocab: new_game, continue, shutdown are terms.commands 18, 19, 20 */
    static const int idx[3] = {18, 19, 20};
    const char *t = RPG_STR(s->terms.commands[idx[i]]);
    int en = !w->enabled || w->enabled(w, i);
    rpg_text_box(r.x + 4, r.y, r.w - 8, r.h, t, en ? rpg_text_color(0) : (rpg_text_color(0) & 0x00ffffff) | 0x80000000, 0);
}

static void draw_centered(int img)
{
    if (img < 0)
        return;
    /* Scene_Title (and this project's resize): the picture stretched to the screen */
    int w = rpg_image_w(img), h = rpg_image_h(img);
    vcpe_spr_filter(0);
    rpg_draw_part_x(&rpg_picture_xform, img, 0, 0, w, h, 0, 0, 640.f / w, 480.f / h, 0, 0xffffffff, 0);
    vcpe_spr_filter(1);
}

int rpg_debug_skip;
static void title_draw(void)
{
    if (rpg_hooks.title_draw) {
        rpg_hooks.title_draw();
    } else {
        if (!(rpg_debug_skip & 1)) draw_centered(T.bg1);
        if (!(rpg_debug_skip & 2)) draw_centered(T.bg2);
        const RpgSystem *s = rpg_system();
        if (s->opt_draw_title && !(rpg_debug_skip & 4)) {
            float saved = rpg_font_size;
            rpg_font_size = 48;
            rpg_text_box(0, 0, rpg_screen_w, rpg_screen_h / 2, RPG_STR(s->game_title), 0xffffffff, 1);
            rpg_font_size = saved;
        }
    }
    if (!(rpg_debug_skip & 8)) rpg_win_draw(&T.cmd, title_item, 0);
}

/* ------------------------------------------------------------------ map */
static struct {
    int menu_calling;
    int transfer_state;   /* 0 none, 1 fading out, 2 waiting before the fade in, 3 fading in */
    int wait;
    int name_frames;      /* the map name window */
    char name[RPG_NAME];
} S;

static void map_post_transfer(void);

static void map_start(void)
{
    rpg_player_refresh();
    rpg_char_straighten(&rpg.player.ch);
    rpg_map_refresh();
    rpg_msg.visible = 0;
    S.menu_calling = 0;
    S.transfer_state = 0;
    rpg_balloons_reset();
    rpg_anims_clear();
    /* perform_transition: from black after a fade, else the normal 15-frame transition */
    if (rpg_brightness == 0)
        fade_start(30, 1, 0, 0);
}

static void update_for_fade(void)
{
    rpg_msg_update();
    rpg_map_update(0);
    rpg_balloons_update();
    rpg_anims_update();
}

static void do_transfer(void)
{
    RpgPlayer *p = &rpg.player;
    rpg_char_set_direction(&p->ch, p->new_dir);
    if (p->new_map != rpg.map_id) {
        rpg_map_load(p->new_map);
        rpg_map_autoplay();
    }
    rpg_player_moveto(p->new_x, p->new_y);
    p->transferring = 0;
    rpg_balloons_reset();
    rpg_anims_clear();
    map_post_transfer();
}

static void fade_in_now(void) { fade_start(30, 1, rpg_temp.fade_type == 1, 0); }

static void map_post_transfer(void)
{
    if (rpg_temp.fade_type == 0 || rpg_temp.fade_type == 1) {
        S.transfer_state = 2;
        S.wait = 15;   /* Graphics.wait(fadein_speed / 2) */
    } else {
        S.transfer_state = 0;
    }
    if (rpg.name_display) {
        const char *n = rpg_s(&rpg_mapdoc, rpg_map()->display_name);
        snprintf(S.name, sizeof S.name, "%s", n);
        S.name_frames = n[0] ? 150 : 0;
    }
}

int rpg_battle_encounter_start(void);

static void map_update(void)
{
    if (fade_step()) {
        update_for_fade();
        return;
    }
    if (S.transfer_state == 2) {
        if (S.wait-- > 0)
            return;
        S.transfer_state = 3;
        fade_in_now();
        return;
    }
    S.transfer_state = 0;
    /* Scene_Map#update: windows first, then the map with its main interpreter, the player, the timer */
    rpg_msg_update();
    rpg_map_update(1);
    rpg_player_update();
    if (rpg.timer_working && rpg.timer_count > 0) {
        rpg.timer_count--;
        if (rpg.timer_count == 0)
            rpg.timer_working = 0;
    }
    rpg_balloons_update();
    rpg_anims_update();
    if (S.name_frames > 0)
        S.name_frames--;
    /* update_scene if scene_change_ok? */
    if (rpg_msg_busy() || rpg_msg.visible)
        return;
    if (rpg_party_all_dead()) {
        rpg_scene_goto(RPG_SCENE_GAMEOVER);
        return;
    }
    if (rpg_scene_changing())
        return;
    if (rpg.player.transferring) {
        S.name_frames = 0;
        if (rpg_temp.fade_type == 0 || rpg_temp.fade_type == 1)
            fade_start(30, -1, rpg_temp.fade_type == 1, do_transfer);
        else
            do_transfer();
        return;
    }
    /* encounters */
    if (rpg_hooks.encounter && rpg_hooks.encounter()) {
        return;
    } else if (!rpg_hooks.encounter && rpg_battle_encounter_start()) {
        return;
    }
    if (rpg_scene_changing())
        return;
    if (rpg.menu_disabled || rpg_interp_running(&rpg.interp)) {
        S.menu_calling = 0;
    } else {
        if (rpg_in.trigger & RPG_K_B)
            S.menu_calling = 1;
        if (S.menu_calling && !rpg_char_moving(&rpg.player.ch)) {
            rpg_play_system_se(RPG_SE_OK);
            rpg_scene_call(RPG_SCENE_MENU);
        }
    }
}

void rpg_map_name_draw(const char *name, int frames);
void (*rpg_map_name_hook)(const char *name, int frames);

static void map_draw(void)
{
    rpg_map_draw();
    rpg_screen_draw_flash(&rpg.screen);
    if (rpg_hooks.hud_draw)
        rpg_hooks.hud_draw();
    if (S.name_frames > 0) {
        if (rpg_map_name_hook) {
            rpg_map_name_hook(S.name, S.name_frames);
        } else {
            /* Window_MapName: a dim band top-left that fades in, stays, then fades out */
            int a = S.name_frames > 134 ? (150 - S.name_frames) * 16 : S.name_frames < 16 ? S.name_frames * 16 : 255;
            a = a > 255 ? 255 : a;
            rpg_fill(0, 12, 240, 24, rpg_rgba(0, 0, 0, a * 160 / 255));
            rpg_text_box(12, 12, 216, 24, S.name, rpg_rgba(255, 255, 255, a), 1);
        }
    }
    rpg_screen_draw_brightness(&rpg.screen);   /* viewport 3 (z 100): below the windows (z 200) */
    rpg_msg_draw();
}

/* ------------------------------------------------------------------ game over */
static int go_img, go_state;
static void gameover_start(void)
{
    go_img = rpg_image("system", "GameOver");
    rpg_bgm_stop();
    rpg_bgs_stop();
    const RpgSystem *s = rpg_system();
    rpg_me_play(RPG_STR(s->gameover_me.name), s->gameover_me.volume, s->gameover_me.pitch);
    rpg_brightness = 0;
    fade_start(60, 1, 0, 0);
    go_state = 0;
}

static void go_title(void) { rpg_scene_goto(RPG_SCENE_TITLE); }

static void gameover_update(void)
{
    if (fade_step())
        return;
    if (go_state == 0 && (rpg_in.trigger & RPG_K_C)) {
        go_state = 1;
        fade_start(120, -1, 0, go_title);
    }
}

static void gameover_draw(void)
{
    draw_centered(go_img);
}

void rpg_init(void)
{
    static const RpgSceneDef title = {title_start, title_update, title_draw, 0};
    static const RpgSceneDef map = {map_start, map_update, map_draw, 0};
    static const RpgSceneDef over = {gameover_start, gameover_update, gameover_draw, 0};
    rpg_tilemap_init();
    rpg_scene_define(RPG_SCENE_TITLE, &title);
    rpg_scene_define(RPG_SCENE_MAP, &map);
    rpg_scene_define(RPG_SCENE_GAMEOVER, &over);
    rpg_menu_define();
    rpg_battle_define();
    rpg_save_define();
}

void rpg_scene_fade(int frames, int dir, void (*then)(void)) { fade_start(frames, dir, 0, then); }
int rpg_scene_fade_step(void) { return fade_step(); }
