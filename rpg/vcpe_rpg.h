/* VCPE RPG (4.5): a native C runtime for tile-based RPGs authored the RPG Maker VX Ace way: a database of actors,
 * classes, skills, items, equipment, enemies, troops, states and animations; tile maps with autotiles; events with
 * pages, move routes and command lists; messages, menus, saves and turn battles. The data comes from
 * tools/rpgmaker/vrpg.py ("VRPG" documents, records in vcpe_rpg_db.h); nothing runs a script interpreter. Event
 * commands are executed by a C interpreter that follows VX Ace's Game_Interpreter command for command, and the rules
 * (damage, states, movement, passability, encounter counting) follow its default scripts.
 *
 * Games build on it through RpgHooks: anything a project changed with its own scripts (custom commands, script
 * calls, battle systems, extra windows, notetags) is C in the game, called from the hooks. No game names here.
 *
 * Layers:   vcpe_rpg_data   documents, records, strings, notes
 *           vcpe_rpg_res    images / sounds by RPG Maker name, BGM / BGS / ME / SE playback
 *           vcpe_rpg_gfx    the virtual screen, sprites, window skins, text with escape codes
 *           vcpe_rpg_map    Game_Map + tilemap + parallax
 *           vcpe_rpg_char   Game_CharacterBase / Character / Event / Player
 *           vcpe_rpg_interp Game_Interpreter
 *           vcpe_rpg_msg    Window_Message, choices, number input, item choice, scrolling text
 *           vcpe_rpg_screen Game_Screen: tone, flash, shake, weather, pictures; map animations and balloons
 *           vcpe_rpg_party  Game_Actor / Party, items, equipment, parameters, features
 *           vcpe_rpg_battle Game_Battler, Enemy / Troop, BattleManager, the battle scene
 *           vcpe_rpg_scene  scenes: title, map, menu, item, skill, equip, status, file, shop, name, game over
 *           vcpe_rpg_save   saves (vcpe_save underneath)
 * Platform: only vcpe_* calls (no PSPSDK headers), so the runtime moves with the engine's backends. */
#ifndef VCPE_RPG_H
#define VCPE_RPG_H
#include <stdint.h>
#include "vcpe_rpg_db.h"

/* ------------------------------------------------------------------ limits */
#ifndef RPG_MAX_SWITCHES
#define RPG_MAX_SWITCHES 5001
#endif
#ifndef RPG_MAX_VARIABLES
#define RPG_MAX_VARIABLES 5001
#endif
#define RPG_SELF_SW_CAP 16384      /* self switches that are ON (hash set) */
#define RPG_MAX_EVENTS 1000        /* per map */
#define RPG_MAX_ACTORS 64
#define RPG_MAX_PARTY 8
#define RPG_MAX_STATES 512
#define RPG_MAX_ITEMS 2048         /* per kind (items, weapons, armors) */
#define RPG_MAX_SKILLS 2048
#define RPG_MAX_SLOTS 8            /* equipment slots */
#define RPG_MAX_PICTURES 100
#define RPG_INTERP_DEPTH 24        /* nested common events */
#define RPG_INDENTS 32
#define RPG_NAME 64                /* bytes of a changeable name (UTF-8) */

/* ------------------------------------------------------------------ documents (vcpe_rpg_data.c) */
typedef struct {
    const uint8_t *base;
    uint32_t size;
    const int32_t *tab[RPG_NTABLES];
    int count[RPG_NTABLES];
    const char *str;
    uint32_t nstr;
    const int16_t *i16;
    uint32_t n16;
} RpgDoc;

int rpg_doc_open(RpgDoc *d, const void *bytes, uint32_t size);   /* 0 ok */
const void *rpg_rec(const RpgDoc *d, int table, int i);          /* a zero record when out of range */
const char *rpg_s(const RpgDoc *d, int32_t off);                 /* "" when out of range */
int rpg_t16(const RpgDoc *d, const RpgTable *t, int x, int y, int z);
static inline int rpg_count(const RpgDoc *d, int table) { return d->count[table]; }

extern RpgDoc rpg_db;      /* the database document */
extern RpgDoc rpg_mapdoc;  /* the current map's document */
const RpgActor *rpg_actor_data(int id);
const RpgClass *rpg_class_data(int id);
const RpgSkill *rpg_skill_data(int id);
const RpgItem *rpg_item_data(int id);
const RpgWeapon *rpg_weapon_data(int id);
const RpgArmor *rpg_armor_data(int id);
const RpgEnemy *rpg_enemy_data(int id);
const RpgState *rpg_state_data(int id);
const RpgTroop *rpg_troop_data(int id);
const RpgAnimation *rpg_anim_data(int id);
const RpgTileset *rpg_tileset_data(int id);
const RpgCommonEvent *rpg_common_data(int id);
const RpgMapInfo *rpg_mapinfo(int id);
const RpgSystem *rpg_system(void);
#define RPG_STR(off) rpg_s(&rpg_db, (off))
/* records of a range: rpg_at(&doc, RPG_T_FEATURE, range, i) */
static inline const void *rpg_at(const RpgDoc *d, int table, RpgRange r, int i)
{
    return (i >= 0 && i < r.count) ? rpg_rec(d, table, r.first + i) : 0;
}
/* note tags: rpg_note_find(note, "<tag:") returns the text after the tag start or 0 */
const char *rpg_note_find(const char *note, const char *tag);
int rpg_note_int(const char *note, const char *tag, int dflt);

/* an event command list: commands live in a document */
typedef struct {
    const RpgDoc *doc;
    RpgRange r;
} RpgList;
static inline const RpgCmd *rpg_cmd(RpgList l, int i)
{
    return (i >= 0 && i < l.r.count) ? (const RpgCmd *)rpg_rec(l.doc, RPG_T_CMD, l.r.first + i) : 0;
}
/* command parameter k (flattened slots, vrpg.py) */
static inline int32_t rpg_p(RpgList l, const RpgCmd *c, int k)
{
    return (k >= 0 && k < c->params.count) ? *(const int32_t *)rpg_rec(l.doc, RPG_T_PARAM, c->params.first + k) : 0;
}
static inline const char *rpg_ps(RpgList l, const RpgCmd *c, int k) { return rpg_s(l.doc, rpg_p(l, c, k)); }

/* ------------------------------------------------------------------ resources (vcpe_rpg_res.c) */
/* the game registers sorted name tables from its converter ("Characters/Actor1" -> vcpe_spr image) */
typedef struct { const char *name; int id; } RpgName;
void rpg_res_images(const RpgName *names, int n);
void rpg_res_sounds(const RpgName *se, int nse, const RpgName *streams, int nstreams);
int rpg_image(const char *folder, const char *name);           /* vcpe_spr image or -1 */
int rpg_image_hue(const char *folder, const char *name, int hue);
typedef struct { int32_t volume, pitch; char name[RPG_NAME]; } RpgSound;   /* a BGM/BGS/ME/SE as played */
void rpg_bgm_play(const char *name, int volume, int pitch);   /* name "" stops */
void rpg_bgm_play_at(const char *name, int volume, int pitch, uint32_t pos);
void rpg_bgm_stop(void);
void rpg_bgm_fade(int ms);
void rpg_bgs_play(const char *name, int volume, int pitch);
void rpg_bgs_stop(void);
void rpg_bgs_fade(int ms);
void rpg_me_play(const char *name, int volume, int pitch);
void rpg_me_stop(void);
void rpg_se_play(const char *name, int volume, int pitch);
void rpg_se_stop(void);
void rpg_audio_update(void);        /* once per frame: the ME's end resumes the BGM */
int rpg_me_playing(void);
RpgSound rpg_bgm_current(void);     /* with its position */
RpgSound rpg_bgs_current(void);
uint32_t rpg_bgm_pos(void);
void rpg_audio_volumes(int bgm, int bgs, int me, int se);   /* 0..100 options */
void rpg_play_system_se(int which);   /* RPG::System sounds: 0 cursor, 1 ok, 2 cancel, 3 buzzer, 4 equip ... */
enum { RPG_SE_CURSOR, RPG_SE_OK, RPG_SE_CANCEL, RPG_SE_BUZZER, RPG_SE_EQUIP, RPG_SE_SAVE, RPG_SE_LOAD,
       RPG_SE_BATTLE_START, RPG_SE_ESCAPE, RPG_SE_ENEMY_ATTACK, RPG_SE_ENEMY_DAMAGE, RPG_SE_ENEMY_COLLAPSE,
       RPG_SE_BOSS_COLLAPSE1, RPG_SE_BOSS_COLLAPSE2, RPG_SE_ACTOR_DAMAGE, RPG_SE_ACTOR_COLLAPSE, RPG_SE_RECOVERY,
       RPG_SE_MISS, RPG_SE_EVASION, RPG_SE_MAGIC_EVASION, RPG_SE_REFLECTION, RPG_SE_SHOP, RPG_SE_USE_ITEM,
       RPG_SE_USE_SKILL };

/* ------------------------------------------------------------------ input (game supplies the pad) */
enum { RPG_K_DOWN = 1, RPG_K_LEFT = 2, RPG_K_RIGHT = 4, RPG_K_UP = 8, RPG_K_A = 16, RPG_K_B = 32, RPG_K_C = 64,
       RPG_K_X = 128, RPG_K_Y = 256, RPG_K_Z = 512, RPG_K_L = 1024, RPG_K_R = 2048 };
/* C = confirm, B = cancel / menu, A = dash, X Y Z L R as the project uses them */
typedef struct { uint32_t press, trigger, repeat; } RpgInput;
extern RpgInput rpg_in;
void rpg_input_set(uint32_t held);   /* once per update: edges and RPG Maker's repeat (24 frames, then every 6) */
int rpg_dir4(void);
int rpg_dir8(void);

/* ------------------------------------------------------------------ the virtual screen (vcpe_rpg_gfx.c) */
/* Game coordinates are the project's pixels. The game says how they map to the display: rpg_view(scale, ox, oy). */
extern float rpg_scale, rpg_ox, rpg_oy;
extern int rpg_screen_w, rpg_screen_h;   /* the project's visible screen in its pixels (Graphics.width/height) */
void rpg_view(float scale, float ox, float oy, int screen_w, int screen_h);
typedef struct { float scale, ox, oy; } RpgXform;
extern RpgXform rpg_picture_xform;      /* pictures: their own mapping (default = the view) */
void rpg_draw_part(int img, int sx, int sy, int sw, int sh, float x, float y, float zx, float zy, float angle,
                   uint32_t abgr, int blend);      /* game coordinates, top-left at (x, y) unless angle != 0 */
void rpg_draw_part_x(const RpgXform *xf, int img, int sx, int sy, int sw, int sh, float x, float y, float zx,
                     float zy, float angle, uint32_t abgr, int blend);
void rpg_fill(float x, float y, float w, float h, uint32_t abgr);
void rpg_clip(float x, float y, float w, float h);
void rpg_clip_off(void);
int rpg_image_w(int img);
int rpg_image_h(int img);
/* a tone (RPG::Tone -255..255 rgb, gray 0..255) over everything drawn after rpg_tone_begin, as a colour overlay */
void rpg_screen_tone(const float tone[4], float x, float y, float w, float h);
uint32_t rpg_rgba(int r, int g, int b, int a);
/* fonts and text */
typedef struct VcpeTtf VcpeTtf;
void rpg_font(VcpeTtf *f, float size, int outline);   /* size in game pixels (RPG Maker's Font.default_size) */
extern float rpg_font_size;
extern int rpg_font_outline;
float rpg_text_width(const char *s);
float rpg_text(float x, float y, const char *s, uint32_t abgr);   /* y = top of the line */
void rpg_text_box(float x, float y, float w, float h, const char *s, uint32_t abgr, int align);   /* 0 1 2 */
float rpg_line_height(void);
/* window skin */
void rpg_windowskin(int img);                /* System/Window */
uint32_t rpg_text_color(int n);              /* the skin's 32 text colours */
void rpg_window(float x, float y, float w, float h, int opacity, int back_opacity, float openness, const float tone[4]);
void rpg_cursor(float x, float y, float w, float h, int frame, int active);
void rpg_pause_sign(float x, float y, int frame);
void rpg_icon(int index, float x, float y, int enabled);
void rpg_face(const char *name, int index, float x, float y, float size, int enabled);
void rpg_gauge(float x, float y, float w, float rate, uint32_t c1, uint32_t c2);
/* escape-coded text (\C[n] \I[n] \V[n] \N[n] \P[n] \G \{ \} \\) on one window's contents */
typedef struct {
    float x, y, new_x, height;
    float size;
    uint32_t color;
} RpgTextPos;
void rpg_convert_escapes(char *out, int cap, const char *in);   /* \V \N \P \G and \\ resolved */
float rpg_draw_text_ex(float x, float y, const char *text);    /* the whole converted text; returns the last x */

/* ------------------------------------------------------------------ characters (vcpe_rpg_char.c) */
typedef struct RpgChar {
    int id;                        /* event id, 0 player */
    int kind;                      /* 0 event, 1 player, 2 follower, 3 vehicle */
    int x, y;
    float real_x, real_y;
    int tile_id;
    char character_name[RPG_NAME];
    int character_index;
    int move_speed, move_frequency;
    uint8_t walk_anime, step_anime, direction_fix, through, transparent, locked, move_succeed, starting;
    uint8_t erased, move_route_forcing, priority_type, has_page;
    int opacity, blend_type, direction, pattern, bush_depth, animation_id, balloon_id;
    int balloon_duration, anim_playing;
    int original_direction, original_pattern, prelock_direction;
    float anime_count;
    int stop_count, jump_count, jump_peak, wait_count;
    /* move route: a route of a document (rpg_doc_id: the database or a map's; saved as ids, not pointers) */
    int route_doc;
    int route, route_index;
    int orig_route_doc;
    int orig_route, orig_route_index, has_orig_route;
    /* events */
    int page;                      /* index into the event's pages, -1 none */
    int trigger, move_type;
    int interp;                    /* parallel interpreter slot, -1 none */
    int map_id;
    int user_int[8];               /* the game's per-character values (saved) */
} RpgChar;

const RpgEvent *rpg_event_data(int index);       /* events of the current map, by index */
int rpg_char_moving(const RpgChar *c);
int rpg_char_jumping(const RpgChar *c);
int rpg_char_stopping(const RpgChar *c);
int rpg_char_passable(RpgChar *c, int x, int y, int d);
int rpg_char_diagonal_passable(RpgChar *c, int x, int y, int horz, int vert);
void rpg_char_moveto(RpgChar *c, int x, int y);
void rpg_char_set_direction(RpgChar *c, int d);
void rpg_char_move_straight(RpgChar *c, int d, int turn_ok);
void rpg_char_move_diagonal(RpgChar *c, int horz, int vert);
void rpg_char_jump(RpgChar *c, int dx, int dy);
void rpg_char_set_graphic(RpgChar *c, const char *name, int index);
/* document ids (saved): the database, or a map's document (RPG_DOC_MAP + map id). Map documents an interpreter or a
 * move route still uses stay loaded after the player left that map (a cutscene running across a transfer). */
enum { RPG_DOC_NONE, RPG_DOC_DB, RPG_DOC_MAP = 1000 };
const RpgDoc *rpg_doc_by_id(int id);
int rpg_doc_id(const RpgDoc *d);
const RpgDoc *rpg_map_doc(int map_id);          /* loads it when needed; 0 if it cannot */
const RpgEvent *rpg_doc_event(const RpgDoc *d, int event_id);
void rpg_char_force_route(RpgChar *c, const RpgDoc *doc, int route);
void rpg_char_update(RpgChar *c);
void rpg_char_straighten(RpgChar *c);
int rpg_char_screen_x(const RpgChar *c);
int rpg_char_screen_y(const RpgChar *c);
int rpg_char_screen_z(const RpgChar *c);
int rpg_char_object(const RpgChar *c);           /* '!' graphics and tiles: no 4-pixel shift */
void rpg_char_turn_toward(RpgChar *c, const RpgChar *t);
void rpg_char_move_toward(RpgChar *c, const RpgChar *t);
void rpg_char_move_away(RpgChar *c, const RpgChar *t);
void rpg_char_move_random(RpgChar *c);
void rpg_char_move_forward(RpgChar *c);
int rpg_char_distance_x(const RpgChar *c, int x);
int rpg_char_distance_y(const RpgChar *c, int y);
float rpg_char_distance_per_frame(const RpgChar *c);
int rpg_char_real_speed(const RpgChar *c);
void rpg_char_update_bush(RpgChar *c);
void rpg_char_draw(const RpgChar *c);            /* the map sprite (Sprite_Character) */

/* the player (Game_Player) */
typedef struct {
    RpgChar ch;
    int transferring, new_map, new_x, new_y, new_dir;
    float encounter_count;
    int vehicle;                    /* -1 walking, 0 boat, 1 ship, 2 airship */
    int getting_on, getting_off;
    int followers_visible;
} RpgPlayer;
void rpg_player_refresh(void);
void rpg_player_reserve_transfer(int map, int x, int y, int d);
void rpg_player_update(void);
void rpg_player_center(int x, int y);
void rpg_player_make_encounter_count(void);
int rpg_player_encounter_troop(void);
void rpg_player_moveto(int x, int y);
int rpg_player_movable(void);
int rpg_player_dash(void);
int rpg_player_check_action(void);
int rpg_player_collide(int x, int y);

/* ------------------------------------------------------------------ interpreter (vcpe_rpg_interp.c) */
typedef struct {
    int src;              /* where the list lives: 0 map event page, 1 common event, 2 troop page, 3 game list */
    int a, b;             /* event id + page index / common event id / troop id + page / game list id */
    int index;
    int event_id;
    int map;              /* src 0: the map whose event this is */
    int phase;            /* 1: waiting for the message before the first command; 3: the list ended (one yield) */
    int8_t branch[RPG_INDENTS];   /* @branch: -1 = nil */
} RpgFrame;
typedef struct RpgInterp {
    int depth;            /* frames used (0: not running) */
    RpgFrame f[RPG_INTERP_DEPTH];
    int map_id;
    int wait_count;
    int wait_mode;        /* what it is waiting for (RPG_W_*) */
    int wait_arg;
    int main;             /* the map's main interpreter (unlocks its event) */
    int battle;           /* the troop interpreter */
} RpgInterp;
enum { RPG_W_NONE, RPG_W_MESSAGE, RPG_W_CHOICE, RPG_W_NUMBER, RPG_W_ITEM, RPG_W_SCROLL, RPG_W_ROUTE, RPG_W_ANIM,
       RPG_W_BALLOON, RPG_W_TRANSFER, RPG_W_SCROLLMAP, RPG_W_SCENE, RPG_W_MSG_HIDDEN, RPG_W_GATHER, RPG_W_GAME,
       RPG_W_FORCED_ACTION, RPG_W_FRAME };
enum { RPG_CMD_NEXT = 1, RPG_CMD_YIELD_SAME, RPG_CMD_YIELD_NEXT, RPG_CMD_JUMPED };
extern RpgList (*rpg_game_list)(int id);       /* frames with src 3: lists the game makes */
extern int (*rpg_hooks_wait)(RpgInterp *in, int arg);   /* RPG_W_GAME: the game's own wait condition */
void rpg_interp_clear(RpgInterp *in);
void rpg_interp_setup(RpgInterp *in, int src, int a, int b, int event_id);
int rpg_interp_running(const RpgInterp *in);
void rpg_interp_update(RpgInterp *in);
RpgList rpg_frame_list(const RpgFrame *f);
RpgInterp *rpg_interp_current(void);            /* the one executing now (hooks) */
/* parameter helpers for hooks */
int rpg_operate_value(int op, int type, int operand);
void rpg_operate_variable(int id, int op, int value);
RpgChar *rpg_get_character(RpgInterp *in, int param);

/* ------------------------------------------------------------------ messages (vcpe_rpg_msg.c) */
typedef struct {
    char texts[2048];         /* lines joined with '\n' */
    int ntexts;
    char face_name[RPG_NAME];
    int face_index, background, position;
    char choices[8][256];
    int nchoices, choice_cancel_type, choice_result;
    int num_var, num_digits;
    int item_var;
    int scroll_mode, scroll_speed, scroll_no_fast;
    int visible;
    int busy_until;           /* the window is still closing */
} RpgMessage;
extern RpgMessage rpg_msg;
void rpg_msg_clear(void);
void rpg_msg_add(const char *line);
int rpg_msg_busy(void);         /* has text / choices / input */
int rpg_msg_choice(void);
void rpg_msg_update(void);
void rpg_msg_draw(void);
extern int rpg_msg_skip;        /* the game's fast-forward */

/* ------------------------------------------------------------------ screen (vcpe_rpg_screen.c) */
typedef struct {
    char name[RPG_NAME];
    int number, origin;
    float x, y, zoom_x, zoom_y, opacity;
    int blend_type;
    float target_x, target_y, target_zoom_x, target_zoom_y, target_opacity;
    int duration;
    float tone[4], tone_target[4];
    int tone_duration;
    float angle, rotate_speed;
} RpgPicture;
typedef struct {
    float brightness;
    int fadeout_duration, fadein_duration;
    float tone[4], tone_target[4];
    int tone_duration;
    float flash_color[4];
    int flash_duration;
    int shake_power, shake_speed, shake_duration, shake_direction;
    float shake;
    int weather_type;
    float weather_power, weather_power_target;
    int weather_duration;
    RpgPicture pictures[RPG_MAX_PICTURES + 1];
} RpgScreen;
void rpg_screen_clear(RpgScreen *s);
void rpg_screen_update(RpgScreen *s);
void rpg_screen_draw_pictures(const RpgScreen *s);
void rpg_screen_draw_over(const RpgScreen *s);   /* tone, flash, weather, brightness */
void rpg_screen_draw_weather(const RpgScreen *s);
void rpg_screen_draw_tone_flash(const RpgScreen *s);
void rpg_screen_draw_brightness(const RpgScreen *s);
void rpg_picture_show(RpgPicture *p, const char *name, int origin, float x, float y, float zx, float zy, float op,
                      int blend);
void rpg_picture_move(RpgPicture *p, int origin, float x, float y, float zx, float zy, float op, int blend, int dur);
void rpg_picture_erase(RpgPicture *p);
/* animations (RPG::Animation) and balloons on characters / battlers */
typedef struct {
    int id, frame, rate_count, mirror;
    float x, y;            /* target position (game pixels), y = the sprite's foot */
    float target_h;        /* target height (head / centre / foot positions) */
    int screen;            /* position type 3: the whole screen */
    void *target;
    float flash[4];
    int flash_duration, hide_duration;
} RpgAnimPlay;
#define RPG_MAX_ANIMS 16
int rpg_anim_start(int id, float x, float y, float h, int mirror, void *target);
void rpg_anims_update(void);
void rpg_anims_draw(void);
int rpg_anim_playing(void *target);
void rpg_anims_clear(void);
void rpg_anim_target_flash(void *target, float out[4]);   /* the flash a target shows now (alpha 0: none) */

/* ------------------------------------------------------------------ party and actors (vcpe_rpg_party.c) */
typedef struct {
    int16_t id, turns, steps, frames;   /* frames: a project's per-state timer (time-based states) */
} RpgStateSlot;
typedef struct RpgBattler {
    int is_actor, index;                    /* index: actor id / troop member index */
    int hp, mp;
    float tp;
    RpgStateSlot states[24];
    int nstates;
    int buffs[8], buff_turns[8];
    /* battle */
    int hidden, action_count, action_times;
    struct { int kind, id, target, forcing, valid; } actions[8];   /* kind 0 skill, 1 item */
    int nactions;
    int speed, sprite_effect, animation_id, animation_mirror, guarding;
    int last_target_index;
    /* result (Game_ActionResult) */
    struct {
        int used, missed, evaded, critical, success, hp_damage, mp_damage, tp_damage, hp_drain, mp_drain;
        int added_states[24], nadded, removed_states[24], nremoved, added_buffs[8], nadded_buffs, added_debuffs[8],
            nadded_debuffs, removed_buffs[8], nremoved_buffs;
    } result;
    int user[16];                           /* the game's battler values */
} RpgBattler;
typedef struct {
    RpgBattler b;
    char name[RPG_NAME], nickname[RPG_NAME];
    int actor_id, class_id, level, exp;
    int param_plus[8];
    int equip_kind[RPG_MAX_SLOTS], equip_id[RPG_MAX_SLOTS];     /* kind 0 weapon, 1 armor; id 0 empty */
    uint8_t skills[RPG_MAX_SKILLS / 8];
    char character_name[RPG_NAME], face_name[RPG_NAME];
    int character_index, face_index;
    int last_skill, last_item;
    int action_input_index;
    int exists;
} RpgActorState;
typedef struct {
    RpgBattler b;
    int enemy_id, letter, plural;
    char name[RPG_NAME];
    int screen_x, screen_y;
} RpgEnemyState;

/* feature codes (RPG::BaseItem::Feature) */
enum { F_ELEMENT_RATE = 11, F_DEBUFF_RATE = 12, F_STATE_RATE = 13, F_STATE_RESIST = 14, F_PARAM = 21, F_XPARAM = 22,
       F_SPARAM = 23, F_ATK_ELEMENT = 31, F_ATK_STATE = 32, F_ATK_SPEED = 33, F_ATK_TIMES = 34, F_STYPE_ADD = 41,
       F_STYPE_SEAL = 42, F_SKILL_ADD = 43, F_SKILL_SEAL = 44, F_EQUIP_WTYPE = 51, F_EQUIP_ATYPE = 52,
       F_EQUIP_FIX = 53, F_EQUIP_SEAL = 54, F_SLOT_TYPE = 55, F_ACTION_PLUS = 61, F_SPECIAL_FLAG = 62,
       F_COLLAPSE_TYPE = 63, F_PARTY_ABILITY = 64 };

typedef struct {
    int members[RPG_MAX_PARTY];
    int nmembers;
    int gold, steps;
    int16_t items[RPG_MAX_ITEMS], weapons[RPG_MAX_ITEMS], armors[RPG_MAX_ITEMS];
    int menu_actor, target_actor;
    int last_item_kind, last_item_id;
    int in_battle;
    int max_battle_members;
} RpgParty;


/* the fields skills and items share (the schema keeps their order), and those weapons and armors share */
typedef struct {
    int32_t id, name, icon_index, description;
    RpgRange features;
    int32_t note, scope, occasion, speed, success_rate, repeats, tp_gain, hit_type, animation_id;
    RpgDamage damage;
    RpgRange effects;
} RpgUsable;
typedef struct {
    int32_t id, name, icon_index, description;
    RpgRange features;
    int32_t note, price, etype_id, params[8];
} RpgEquip;
enum { RPG_KIND_ITEM, RPG_KIND_WEAPON, RPG_KIND_ARMOR, RPG_KIND_SKILL };
const RpgUsable *rpg_usable(int kind, int id);   /* RPG_KIND_ITEM / RPG_KIND_SKILL */
const RpgEquip *rpg_equip(int kind, int id);     /* RPG_KIND_WEAPON / RPG_KIND_ARMOR */
const char *rpg_item_name(int kind, int id);
int rpg_item_icon(int kind, int id);
int rpg_item_price(int kind, int id);
/* RPG::UsableItem scope tests */
int rpg_for_opponent(const RpgUsable *u);
int rpg_for_friend(const RpgUsable *u);
int rpg_for_dead_friend(const RpgUsable *u);
int rpg_for_user(const RpgUsable *u);
int rpg_for_one(const RpgUsable *u);
int rpg_for_random(const RpgUsable *u);
int rpg_for_all(const RpgUsable *u);
int rpg_need_selection(const RpgUsable *u);
int rpg_battle_ok(const RpgUsable *u);
int rpg_menu_ok(const RpgUsable *u);

/* Game_BattlerBase / Game_Battler */
RpgActorState *rpg_actor(int id);
RpgActorState *rpg_battler_actor(const RpgBattler *b);       /* 0 for enemies */
RpgEnemyState *rpg_battler_enemy(const RpgBattler *b);       /* 0 for actors */
const char *rpg_battler_name(const RpgBattler *b);
int rpg_battler_param(const RpgBattler *b, int p);
double rpg_battler_param_d(const RpgBattler *b, int p);     /* before the clamp and truncation (hooks) */
int rpg_battler_param_base(const RpgBattler *b, int p);
int rpg_battler_param_plus(const RpgBattler *b, int p);
int rpg_battler_param_min(const RpgBattler *b, int p);
int rpg_battler_param_max(const RpgBattler *b, int p);
double rpg_battler_xparam(const RpgBattler *b, int p);
double rpg_battler_sparam(const RpgBattler *b, int p);
double rpg_features_pi(const RpgBattler *b, int code, int id);
double rpg_features_sum(const RpgBattler *b, int code, int id);
double rpg_features_sum_all(const RpgBattler *b, int code);
int rpg_features_set_has(const RpgBattler *b, int code, int id);
int rpg_features_set_max(const RpgBattler *b, int code);
int rpg_features_any(const RpgBattler *b, int code);
int rpg_features_list(const RpgBattler *b, const RpgFeature **out, int max);   /* every feature of every object */
int rpg_special_flag(const RpgBattler *b, int flag);
int rpg_party_ability(int ability);
double rpg_element_rate(const RpgBattler *b, int element);
double rpg_state_rate(const RpgBattler *b, int state);
int rpg_state_resist(const RpgBattler *b, int state);
int rpg_battler_state(const RpgBattler *b, int state);
int rpg_battler_exists(const RpgBattler *b);
int rpg_battler_alive(const RpgBattler *b);
int rpg_battler_dead(const RpgBattler *b);
int rpg_battler_restriction(const RpgBattler *b);
int rpg_battler_movable(const RpgBattler *b);
int rpg_battler_normal(const RpgBattler *b);
int rpg_battler_inputable(const RpgBattler *b);
int rpg_battler_confusion(const RpgBattler *b);
int rpg_battler_guard(const RpgBattler *b);
int rpg_battler_mhp(const RpgBattler *b);
int rpg_battler_mmp(const RpgBattler *b);
void rpg_battler_set_hp(RpgBattler *b, int hp);
void rpg_battler_set_mp(RpgBattler *b, int mp);
void rpg_battler_set_tp(RpgBattler *b, float tp);
void rpg_battler_change_hp(RpgBattler *b, int value, int enable_death);
void rpg_battler_refresh(RpgBattler *b);
void rpg_battler_recover_all(RpgBattler *b);
void rpg_battler_clear_states(RpgBattler *b);
void rpg_battler_add_state(RpgBattler *b, int state);
void rpg_battler_remove_state(RpgBattler *b, int state);
void rpg_battler_erase_state(RpgBattler *b, int state);
int rpg_battler_state_turns(const RpgBattler *b, int state);
void rpg_battler_set_state_turns(RpgBattler *b, int state, int turns);
void rpg_battler_add_buff(RpgBattler *b, int param, int turns);
void rpg_battler_add_debuff(RpgBattler *b, int param, int turns);
void rpg_battler_remove_buff(RpgBattler *b, int param);
void rpg_battler_die(RpgBattler *b);
void rpg_battler_revive(RpgBattler *b);
void rpg_battler_escape(RpgBattler *b);
void rpg_battler_collapse_effect(RpgBattler *b);
void rpg_battler_damage_effect(RpgBattler *b);
int rpg_battler_usable(const RpgBattler *b, int kind, int id);
int rpg_battler_skill_mp_cost(const RpgBattler *b, int skill);
int rpg_battler_skill_tp_cost(const RpgBattler *b, int skill);
void rpg_battler_use_item(RpgBattler *b, int kind, int id);
int rpg_battler_item_test(RpgBattler *target, RpgBattler *user, int kind, int id);
void rpg_battler_item_apply(RpgBattler *target, RpgBattler *user, int kind, int id);
void rpg_battler_make_damage_value(RpgBattler *target, RpgBattler *user, int kind, int id);
void rpg_battler_result_clear(RpgBattler *b);
int rpg_battler_result_hit(const RpgBattler *b);
void rpg_battler_on_battle_start(RpgBattler *b);
void rpg_battler_on_battle_end(RpgBattler *b);
void rpg_battler_on_action_end(RpgBattler *b);
void rpg_battler_on_turn_end(RpgBattler *b);
void rpg_battler_regenerate_all(RpgBattler *b);
void rpg_battler_update_state_turns(RpgBattler *b);
void rpg_battler_update_buff_turns(RpgBattler *b);
void rpg_battler_remove_states_auto(RpgBattler *b, int timing);
void rpg_battler_remove_buffs_auto(RpgBattler *b);
int rpg_battler_attack_skill(const RpgBattler *b);
int rpg_battler_guard_skill(const RpgBattler *b);
double rpg_battler_atk_speed(const RpgBattler *b);
int rpg_battler_atk_times_add(const RpgBattler *b);
int rpg_battler_atk_elements(const RpgBattler *b, int *out, int max);
double rpg_damage_formula(int kind, int id, const RpgBattler *a, const RpgBattler *b);
double rpg_formula_c(int kind, int id, const RpgBattler *a, const RpgBattler *b);
double rpg_formula_eval(int kind, int id, const RpgBattler *a, const RpgBattler *b);   /* run-time interpreter (vcpe_rpg_formula.c) */
extern void (*rpg_battler_damage_hook)(RpgBattler *target, RpgBattler *user, int kind, int id, double *value);

/* the project's battler rules (RGSS scripts that alias Game_Battler methods); every pointer is optional */
typedef struct {
    double (*param_rate)(const RpgBattler *b, int p);                 /* an extra factor (1.0) */
    int (*param_max)(const RpgBattler *b, int p, int dflt);
    int (*param_base)(const RpgBattler *b, int p, int dflt);
    int (*attack_skill)(const RpgBattler *b, int dflt);
    int (*learning_level)(const RpgLearning *l);
    void (*make_damage_post)(RpgBattler *t, RpgBattler *user, int kind, int id);   /* after make_damage */
    void (*execute_damage_pre)(RpgBattler *t, RpgBattler *user);
    int (*effect_pre)(RpgBattler *t, RpgBattler *user, int kind, int id, RpgEffect *e);   /* 0: skip it */
    void (*add_state_pre)(RpgBattler *b, int state);
    void (*add_state_post)(RpgBattler *b, int state);
    void (*reset_state_counts)(RpgBattler *b, int state);
    void (*erase_state)(RpgBattler *b, int state);
    void (*remove_battle_states_post)(RpgBattler *b);
    void (*regenerate_hp_pre)(RpgBattler *b);
    void (*regenerate_hp_post)(RpgBattler *b);
    double (*critical_rate)(const RpgBattler *t);                      /* the critical multiplier (3) */
    int (*enemy_exp)(const RpgEnemyState *e, int dflt);
    int (*enemy_gold)(const RpgEnemyState *e, int dflt);
    double (*drop_rate)(const RpgEnemyState *e, double dflt);
    int (*usable_extra)(const RpgBattler *b, int kind, int id);         /* 0: not usable */
    void (*use_item_pre)(RpgBattler *b, int kind, int id);
    void (*on_battle_start)(RpgBattler *b);
    void (*on_battle_end)(RpgBattler *b);
    void (*clear_states)(RpgBattler *b);
} RpgBattlerHooks;
extern RpgBattlerHooks rpg_bh;

/* Game_Actor */
void rpg_actor_setup(int id);
int rpg_actor_exp_for_level(const RpgActorState *a, int level);
int rpg_actor_max_level(const RpgActorState *a);
int rpg_actor_next_level_exp(const RpgActorState *a);
int rpg_actor_current_level_exp(const RpgActorState *a);
void rpg_actor_change_exp(RpgActorState *a, int exp, int show);
void rpg_actor_gain_exp(RpgActorState *a, int exp);
void rpg_actor_change_level(RpgActorState *a, int level, int show);
void rpg_actor_learn_skill(RpgActorState *a, int skill);
void rpg_actor_forget_skill(RpgActorState *a, int skill);
int rpg_actor_skill_learned(const RpgActorState *a, int skill);
int rpg_actor_skills(const RpgActorState *a, int *out, int max);   /* learned + added by features, sorted */
void rpg_actor_change_class(RpgActorState *a, int class_id, int keep_exp);
void rpg_actor_add_param(RpgActorState *a, int param, int value);
int rpg_actor_slots(const RpgActorState *a, int *etypes);           /* the equipment slots' etype ids */
int rpg_actor_equippable(const RpgActorState *a, int kind, int id);
int rpg_actor_equip_change_ok(const RpgActorState *a, int slot);
int rpg_actor_change_equip(RpgActorState *a, int slot, int kind, int id);   /* trades with the party */
void rpg_actor_force_change_equip(RpgActorState *a, int slot, int kind, int id);
void rpg_actor_change_equip_by_id(RpgActorState *a, int slot, int id);
void rpg_actor_discard_equip(RpgActorState *a, int kind, int id);
int rpg_actor_has_equip(const RpgActorState *a, int is_armor, int id);
void rpg_actor_release_unequippable(RpgActorState *a, int item_gain);
void rpg_actor_clear_equipments(RpgActorState *a);
void rpg_actor_optimize_equipments(RpgActorState *a);
int rpg_actor_wtype_equipped(const RpgActorState *a, int wtype);
int rpg_actor_atk_animation(const RpgActorState *a, int which);
int rpg_actor_dual_wield(const RpgActorState *a);
int rpg_actor_battle_member(const RpgActorState *a);
int rpg_actor_index(const RpgActorState *a);

/* Game_Party */
int rpg_party_member(int actor_id);
int rpg_party_has_item(int kind, int id, int include_equip);
int rpg_party_item_number(int kind, int id);
int rpg_party_max_item_number(int kind, int id);
void rpg_party_gain_item(int kind, int id, int amount, int include_equip);
void rpg_party_lose_item(int kind, int id, int amount, int include_equip);
void rpg_party_consume_item(int kind, int id);
void rpg_party_gain_gold(int amount);
void rpg_party_add_actor(int id);
void rpg_party_remove_actor(int id);
void rpg_party_swap_order(int a, int b);
int rpg_party_battle_members(int *out);           /* actor ids */
int rpg_party_all_dead(void);
int rpg_party_highest_level(void);
int rpg_party_usable(int kind, int id);
int rpg_party_inputable(void);
int rpg_party_agi(void);
RpgActorState *rpg_party_leader(void);
RpgActorState *rpg_party_menu_actor(void);
void rpg_party_set_menu_actor(int id);
void rpg_party_menu_actor_step(int d);
void rpg_party_setup_starting(void);

/* Game_Enemy / Game_Troop / battle (vcpe_rpg_battle.c) */
extern RpgScreen rpg_battle_screen;
int rpg_troop_count(void);
RpgEnemyState *rpg_troop_member(int i);
void rpg_troop_make_unique_names(void);
void rpg_enemy_transform(RpgEnemyState *e, int enemy_id);
void rpg_battle_setup(int troop, int can_escape, int can_lose);
void rpg_battle_event_branch(int8_t *slot);
int rpg_battle_force_action(int kind, int index, int skill, int target);
int rpg_battle_action_forced(void);
void rpg_battle_abort(void);
void rpg_vehicle_set_location(int vehicle, int map, int x, int y);
void rpg_vehicle_get_on_off(void);
int rpg_input_press_code(int code);
void rpg_msg_setup_choices(RpgList l, const RpgCmd *c, RpgFrame *f, int indent);
void rpg_screen_flash_start(RpgScreen *s);
void rpg_screen_flash_damage(RpgScreen *s);
void rpg_screen_draw_flash(const RpgScreen *s);
void rpg_tilemap_init(void);
void rpg_tile_draw(int id, float x, float y, uint32_t color);
void rpg_balloons_update(void);
void rpg_balloons_reset(void);
void rpg_balloon_draw(const RpgChar *c);
int rpg_anim_target_hidden(void *target);
extern int rpg_anim_rate;              /* frames per animation frame (4) */
/* a converted image's scale against the original (pictures stored smaller): the game supplies it */
extern float (*rpg_image_scale)(int img);
void rpg_windowskin_colors(const uint32_t *c32);
int rpg_map_reopen(void);
int rpg_scene_changing(void);
extern int rpg_brightness;
extern int rpg_quit_request;
extern int rpg_scene_hold;          /* a scene that must finish (battle fade-out) before the switch */
extern void (*rpg_level_up_message)(RpgActorState *a, const int *new_skills, int n);
RpgChar *rpg_route_character(void);

/* ------------------------------------------------------------------ game state (saved) */
typedef struct {
    uint8_t switches[RPG_MAX_SWITCHES];
    int32_t variables[RPG_MAX_VARIABLES];
    uint32_t self_sw[RPG_SELF_SW_CAP];      /* keys (map << 14 | event << 2 | ch) + 1, 0 = empty */
    int nself;
    RpgActorState actors[RPG_MAX_ACTORS];
    RpgParty party;
    RpgPlayer player;
    int map_id;
    float display_x, display_y;
    int scroll_dir, scroll_speed;
    float scroll_rest;
    int tileset_id;
    char parallax_name[RPG_NAME];
    int parallax_loop_x, parallax_loop_y, parallax_sx, parallax_sy;
    float parallax_x, parallax_y;
    char battleback1[RPG_NAME], battleback2[RPG_NAME];
    int name_display;
    RpgChar events[RPG_MAX_EVENTS];         /* by index into the map's event list */
    int nevents;
    RpgInterp interp;                       /* the map's main interpreter */
    RpgInterp parallel[RPG_MAX_EVENTS / 4];  /* parallel events + parallel common events */
    int nparallel;
    RpgScreen screen;
    /* Game_System */
    int save_disabled, menu_disabled, encounter_disabled, formation_disabled;
    int save_count, battle_count, win_count, escape_count;
    float window_tone[4];
    RpgSound battle_bgm, battle_end_me, saved_bgm, bgm_on_save, bgs_on_save;
    uint32_t bgm_on_save_pos;
    uint32_t saved_bgm_pos;
    int has_battle_bgm, has_battle_end_me;
    uint32_t frame_count;
    /* Game_Timer */
    int timer_count, timer_working;
    /* reserved common event (Game_Temp) */
    int reserved_common;
    int need_refresh;
    /* game data (the hooks' save area) */
    uint8_t user[16384];
} RpgGame;
extern RpgGame rpg;

/* Game_Temp (not saved) */
typedef struct {
    int fade_type;
    int battle_troop, battle_can_escape, battle_can_lose, battle_result, battle_from_event;
    int shop_goods_first, shop_goods_n, shop_purchase_only;
    const RpgDoc *shop_doc;
    int shop_list[256][3];
    int name_actor, name_max;
    int menu_calling, save_calling;
    int scene_request;      /* a scene the interpreter asked for (RPG_SCENE_*) */
    int destination_valid, destination_x, destination_y;
} RpgTemp;
extern RpgTemp rpg_temp;

/* ------------------------------------------------------------------ map (vcpe_rpg_map.c) */
int rpg_map_load(int id);       /* reads Map document id (through the game's hook) and sets up the events */
int rpg_map_width(void);
int rpg_map_height(void);
const RpgMap *rpg_map(void);
int rpg_map_tile(int x, int y, int z);
int rpg_map_valid(int x, int y);
int rpg_map_passable(int x, int y, int d);
int rpg_map_check_passage(int x, int y, int bit);
int rpg_map_flag(int tile);
int rpg_map_bush(int x, int y);
int rpg_map_ladder(int x, int y);
int rpg_map_counter(int x, int y);
int rpg_map_damage_floor(int x, int y);
int rpg_map_terrain_tag(int x, int y);
int rpg_map_region(int x, int y);
int rpg_map_loop_h(void);
int rpg_map_loop_v(void);
int rpg_round_x(int x);
int rpg_round_y(int y);
int rpg_round_x_dir(int x, int d);
int rpg_round_y_dir(int y, int d);
float rpg_adjust_x(float x);
float rpg_adjust_y(float y);
void rpg_map_set_display(float x, float y);
void rpg_map_scroll(int d, float dist);
int rpg_screen_tile_x(void);
int rpg_screen_tile_y(void);
void rpg_map_update(int main);
void rpg_map_refresh(void);
void rpg_map_autoplay(void);
void rpg_map_draw(void);        /* parallax, tiles, characters, animations, pictures, screen effects */
int rpg_map_events_xy(int x, int y, RpgChar **out, int max, int nt);
int rpg_map_event_index(int id);
RpgChar *rpg_map_event(int id);
int rpg_map_setup_starting_event(void);
int rpg_map_any_starting(void);
void rpg_event_refresh(RpgChar *e);
void rpg_event_start(RpgChar *e);
void rpg_event_erase(RpgChar *e);
const RpgPage *rpg_event_page(const RpgChar *e);
RpgList rpg_event_list(const RpgChar *e);
void rpg_map_change_tileset(int id);
void rpg_map_change_parallax(const char *name, int lx, int ly, int sx, int sy);
int rpg_map_tile_image(int slot);   /* vcpe_spr image of tileset slot 0..8 */
extern int rpg_tile_anim;           /* autotile animation counter */

/* ------------------------------------------------------------------ scenes (vcpe_rpg_scene.c) */
enum { RPG_SCENE_NONE, RPG_SCENE_TITLE, RPG_SCENE_MAP, RPG_SCENE_MENU, RPG_SCENE_ITEM, RPG_SCENE_SKILL,
       RPG_SCENE_EQUIP, RPG_SCENE_STATUS, RPG_SCENE_SAVE, RPG_SCENE_LOAD, RPG_SCENE_END, RPG_SCENE_SHOP,
       RPG_SCENE_NAME, RPG_SCENE_BATTLE, RPG_SCENE_GAMEOVER, RPG_SCENE_DEBUG, RPG_SCENE_GAME = 100 };
typedef struct {
    void (*start)(void);
    void (*update)(void);
    void (*draw)(void);
    void (*terminate)(void);
} RpgSceneDef;
void rpg_scene_goto(int scene);
void rpg_scene_call(int scene);   /* returns to the caller with rpg_scene_return */
void rpg_scene_return(void);
int rpg_scene_current(void);
void rpg_scene_define(int id, const RpgSceneDef *def);   /* the game replaces or adds scenes (ids >= 100) */
void rpg_frame(uint32_t held);    /* one update (60 Hz) */
void rpg_draw(void);              /* the current scene */
void rpg_new_game(void);
void rpg_fadeout_all(int ms);
extern int rpg_transition;        /* frames of the current fade-in (scene changes) */

/* ------------------------------------------------------------------ vocabulary (Vocab: in scripts, not data) */
typedef struct {
    const char *shop_buy, *shop_sell, *shop_cancel, *possession, *exp_total, *exp_next, *save_message,
        *load_message, *file, *party_name, *emerge, *preemptive, *surprise, *escape_start, *escape_failure,
        *victory, *defeat, *obtain_exp, *obtain_gold, *obtain_item, *level_up, *obtain_skill, *use_item,
        *critical_to_enemy, *critical_to_actor, *actor_damage, *actor_recovery, *actor_gain, *actor_loss,
        *actor_drain, *actor_no_damage, *actor_no_hit, *enemy_damage, *enemy_recovery, *enemy_gain, *enemy_loss,
        *enemy_drain, *enemy_no_damage, *enemy_no_hit, *evasion, *magic_evasion, *magic_reflection,
        *counter_attack, *substitute, *buff_add, *debuff_add, *buff_remove, *action_failure;
} RpgVocab;
extern RpgVocab rpg_vocab;
const char *rpg_term_basic(int i);     /* level, level_a, hp, hp_a, mp, mp_a, tp, tp_a */
const char *rpg_term_param(int i);
const char *rpg_term_etype(int i);
const char *rpg_term_command(int i);

/* ------------------------------------------------------------------ saves (vcpe_rpg_save.c) */
#define RPG_SAVE_FILES 16
typedef struct {
    uint32_t magic, frame_count;
    char chars[4][RPG_NAME];
    int char_index[4], nchars;
    char map_name[RPG_NAME];
    int user[8];
} RpgSaveHeader;
int rpg_save_write(int slot);                /* 0 ok */
int rpg_save_read(int slot);                 /* 0 ok: the game is that save now */
int rpg_save_header(int slot, RpgSaveHeader *h);   /* 0: the slot holds a save */
int rpg_saves_exist(void);
int rpg_save_last_slot(void);
extern void (*rpg_save_name_hook)(int slot, char *out, int n);

/* ------------------------------------------------------------------ hooks */
typedef struct {
    /* load the map document id into new memory (released with free_map, or free() when 0); 0 ok */
    int (*load_map)(int id, const void **bytes, uint32_t *size);
    void (*free_map)(void *bytes);
    /* an event command the game handles itself (custom scripts): 0 = run the default, else an RPG_CMD_* code; the
     * hook may step f->index over lines it consumed (RPG_CMD_NEXT then moves past the last one) */
    int (*command)(RpgInterp *in, RpgList l, const RpgCmd *c, RpgFrame *f);
    /* a script call (355 + 655 lines joined with '\n', 111 type 12, route 45): return the value (111: truth) */
    int (*script)(RpgInterp *in, const char *code, int kind);
    /* after the map is set up / before it is left */
    void (*map_setup)(int id);
    void (*map_update)(void);
    void (*map_draw_fog)(void);        /* above the map's characters and tone, below the pictures */
    void (*map_draw_over)(void);       /* drawn above the map and pictures, below messages */
    void (*hud_draw)(void);            /* above everything on the map */
    void (*player_input)(void);        /* replace Game_Player#move_by_input (8 directions, ...) */
    int (*passable)(RpgChar *c, int x, int y, int d, int dflt);   /* passability rules on top of the map's */
    int (*near_screen)(const RpgChar *c, int dflt);   /* events moving off screen */
    void (*new_game)(void);
    void (*message_start)(void);       /* a message is about to show (name windows, ...) */
    void (*message_draw)(float x, float y, float w, float h);
    int (*encounter)(void);            /* 1: the game handles encounters (symbols); 0 default */
    void (*battle_start)(void);
    void (*save_extra)(void);          /* fill rpg.user before a save */
    void (*load_extra)(void);
    int (*param)(const RpgActorState *a, int param, int value);   /* final parameter value */
    int (*max_level)(const RpgActorState *a, int dflt);
    int (*exp_for_level)(const RpgActorState *a, int level, int dflt);
    int (*equip_slots)(const RpgActorState *a, int *etypes, int max);   /* slot etypes; 0 = default */
    double (*formula)(int kind, int id, const RpgBattler *a, const RpgBattler *b);   /* damage formula (vrpg_formula.py) */
    void (*title_draw)(void);
    int (*parallax_origin)(float *ox, float *oy);   /* 1: the game placed the parallax itself */
    void (*menu_actor_draw)(const RpgActorState *a, float x, float y, float w, float h);   /* item/skill target row */
} RpgHooks;
extern RpgHooks rpg_hooks;

void rpg_init(void);              /* after the database document is open */
void rpg_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void (*rpg_log_hook)(const char *line);
int rpg_rand(int n);              /* 0..n-1 (vcpe_math's generator) */
float rpg_randf(void);

/* switches / variables / self switches */
static inline int rpg_sw(int id) { return id > 0 && id < RPG_MAX_SWITCHES ? rpg.switches[id] : 0; }
void rpg_sw_set(int id, int on);
static inline int32_t rpg_var(int id) { return id > 0 && id < RPG_MAX_VARIABLES ? rpg.variables[id] : 0; }
void rpg_var_set(int id, int32_t v);
int rpg_self_sw(int map, int event, int ch);   /* ch 0..3 = 'A'..'D' */
void rpg_self_sw_set(int map, int event, int ch, int on);
void rpg_self_sw_clear_all(void);

#endif
