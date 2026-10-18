/* VCPE RPG battle (vcpe_rpg_battle.c): Game_Enemy, Game_Troop, Game_Action, BattleManager, Window_BattleLog and the
 * other battle windows, Spriteset_Battle / Sprite_Battler, and Scene_Battle - VX Ace's front-view turn battle.
 *
 * Scene_Battle runs in a coroutine (vcpe_co): its code waits the way the Ruby does ("wait for the message", "wait
 * 20 frames for the log", "wait for the animation") and every wait is a frame handed back to the main loop, which
 * draws. Every Scene_Battle / BattleManager step is a function pointer in rpg_bvt with the default in rpg_bdef, so
 * a project's own battle system (an active time battle, other commands, other windows) replaces just the steps it
 * changes and calls the defaults for the rest, the way RGSS scripts alias methods. */
#ifndef VCPE_RPG_BATTLE_H
#define VCPE_RPG_BATTLE_H
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"

#define RPG_MAX_ENEMIES 8
#define RPG_LOG_LINES 6

/* the battle state (not saved: battles are not saved in VX Ace) */
typedef enum { BP_NONE, BP_INIT, BP_INPUT, BP_TURN, BP_TURN_END, BP_ABORTING } RpgBattlePhase;
typedef struct {
    int troop_id;
    RpgEnemyState enemies[RPG_MAX_ENEMIES];
    int nenemies;
    int turn_count;
    uint8_t event_flags[64];
    char names_seen[RPG_MAX_ENEMIES][RPG_NAME];
    int names_count[RPG_MAX_ENEMIES], nnames;
    RpgInterp interp;
    RpgBattlePhase phase;
    int can_escape, can_lose, preemptive, surprise;
    int actor_index;
    RpgBattler *action_forced;
    RpgBattler *action_battlers[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int naction_battlers;
    RpgBattler *subject;
    float escape_ratio;
    RpgSound map_bgm, map_bgs;
    uint32_t map_bgm_pos;
    int8_t *event_branch;
    int result;            /* 0 win, 1 escape / abort, 2 lose */
    int input_abort;       /* set mid-input (a cheat, a project hook): the command windows give up and the update judges */
    int user[64];          /* the project's battle values */
} RpgBattleState;
extern RpgBattleState rpg_b;

/* battle sprite effects (Sprite_Battler) */
enum { BE_NONE, BE_BLINK, BE_COLLAPSE, BE_BOSS_COLLAPSE, BE_INSTANT_COLLAPSE, BE_WHITEN, BE_APPEAR, BE_DISAPPEAR };
typedef struct {
    int effect, duration, visible, opacity, blend, src_y, ox_shift;
    float color[4];
    int mirror;
    float zoom_x, zoom_y;
    int user[8];
} RpgBattlerSprite;
RpgBattlerSprite *rpg_battler_sprite(const RpgBattler *b);

/* units: the party's battle members (in battle) and the troop */
int rpg_unit_members(int troop, RpgBattler **out);
int rpg_unit_alive(int troop, RpgBattler **out);
int rpg_unit_dead(int troop, RpgBattler **out);
int rpg_unit_movable(int troop, RpgBattler **out);
RpgBattler *rpg_unit_random_target(int troop);
RpgBattler *rpg_unit_random_dead_target(int troop);
RpgBattler *rpg_unit_smooth_target(int troop, int index);
RpgBattler *rpg_unit_smooth_dead_target(int troop, int index);
RpgBattler *rpg_unit_substitute(int troop);
int rpg_unit_all_dead(int troop);
int rpg_unit_agi(int troop);
int rpg_battler_index(const RpgBattler *b);   /* in its unit, -1 */
int rpg_battler_opposite_troop(const RpgBattler *b);   /* 1 when its opponents are the troop */

/* Game_Action (the battler's actions[]) */
typedef struct { int kind, id, target, forcing, valid; } RpgActionRef;
void rpg_action_set_skill(RpgBattler *b, int i, int skill);
void rpg_action_set_item(RpgBattler *b, int i, int item);
int rpg_action_kind(const RpgBattler *b, int i);
int rpg_action_item_id(const RpgBattler *b, int i);
int rpg_action_attack(const RpgBattler *b, int i);
int rpg_action_valid(const RpgBattler *b, int i);
void rpg_action_prepare(RpgBattler *b, int i);
void rpg_action_decide_random_target(RpgBattler *b, int i);
int rpg_action_speed(const RpgBattler *b, int i);
int rpg_action_make_targets(RpgBattler *b, int i, RpgBattler **out, int max);
double rpg_action_evaluate(RpgBattler *b, int i);
void rpg_battler_make_actions(RpgBattler *b);
void rpg_battler_make_speed(RpgBattler *b);
void rpg_battler_clear_actions(RpgBattler *b);
void rpg_battler_force_action(RpgBattler *b, int skill, int target);
int rpg_battler_current_action(const RpgBattler *b);   /* 1 when it has one (actions[0]) */
void rpg_battler_remove_current_action(RpgBattler *b);
int rpg_actor_next_command(RpgActorState *a);
int rpg_actor_prior_command(RpgActorState *a);
double rpg_item_cnt(RpgBattler *t, RpgBattler *user, int kind, int id);
double rpg_item_mrf(RpgBattler *t, RpgBattler *user, int kind, int id);

/* Game_Enemy / Game_Troop */
void rpg_enemy_setup(RpgEnemyState *e, int index, int enemy_id);
int rpg_troop_exp_total(void);
int rpg_troop_gold_total(void);
int rpg_troop_drop_items(int *kinds, int *ids, int max);
int rpg_troop_conditions_met(int page);
void rpg_troop_setup_battle_event(void);
void rpg_troop_increase_turn(void);

/* the log window */
void rpg_log_clear(void);
void rpg_log_add(const char *text);
void rpg_log_replace(const char *text);
void rpg_log_back_one(void);
void rpg_log_back_to(int n);
int rpg_log_lines(void);
const char *rpg_log_last(void);
void rpg_log_wait(void);
void rpg_log_wait_and_clear(void);
extern int rpg_log_message_speed;   /* 20 */

/* waits inside the battle coroutine */
void rpg_bwait(int frames);          /* Scene_Battle#wait: shortened while C / A is held */
void rpg_babs_wait(int frames);
void rpg_bwait_for_message(void);
void rpg_bwait_for_animation(void);
void rpg_bwait_for_effect(void);
void rpg_bupdate_for_wait(void);     /* one frame */
int rpg_battle_animation_playing(void);
int rpg_battle_effect_playing(void);
void rpg_battle_refresh_status(void);

/* the battle windows (the game may move / resize them in its start hook) */
typedef struct {
    RpgWin log, status, party_cmd, actor_cmd, help, skill, item, actor_sel, enemy_sel;
    float info_ox, info_y;           /* the info viewport (status + command windows) and its slide */
    int cmds[16], ncmds, cmd_ext[16];        /* the actor command list: RPG_BCMD_* and the skill type */
    int skills[512], nskills, items[512], nitems;
    int skill_stype;
    RpgActorState *cmd_actor;
} RpgBattleWindows;
extern RpgBattleWindows rpg_bw;
enum { RPG_BCMD_ATTACK, RPG_BCMD_SKILL, RPG_BCMD_GUARD, RPG_BCMD_ITEM, RPG_BCMD_ESCAPE, RPG_BCMD_GAME = 100 };

/* the steps of the battle: rpg_bvt is what runs, rpg_bdef the defaults (call them from overrides) */
typedef struct {
    /* BattleManager */
    void (*battle_start)(void);
    int (*judge_win_loss)(void);
    int (*process_victory)(void);
    int (*process_escape)(void);
    int (*process_abort)(void);
    int (*process_defeat)(void);
    void (*battle_end)(int result);
    int (*input_start)(void);
    void (*turn_start_bm)(void);
    void (*turn_end_bm)(void);
    void (*make_action_orders)(void);
    RpgBattler *(*next_subject)(void);
    int (*next_command)(void);
    int (*prior_command)(void);
    void (*display_exp)(void);
    void (*gain_gold)(void);
    void (*gain_drop_items)(void);
    void (*gain_exp)(void);
    /* Scene_Battle */
    void (*start)(void);                       /* windows and sprites */
    void (*scene_battle_start)(void);          /* Scene_Battle#battle_start */
    void (*update)(void);                      /* one Scene_Battle#update */
    void (*update_basic)(void);                /* windows, troop screen, sprites (after the frame's yield) */
    void (*start_party_command_selection)(void);
    void (*start_actor_command_selection)(void);
    void (*command_fight)(void);
    void (*command_escape)(void);
    void (*command_attack)(void);
    void (*command_skill)(void);
    void (*command_guard)(void);
    void (*command_item)(void);
    void (*command_game)(int cmd);             /* RPG_BCMD_GAME + n */
    void (*scene_next_command)(void);
    void (*scene_prior_command)(void);
    void (*turn_start)(void);
    void (*turn_end)(void);
    void (*process_event)(void);
    int (*process_forced_action)(void);
    void (*process_action)(void);
    void (*process_action_end)(void);
    void (*execute_action)(void);
    void (*use_item)(void);
    void (*invoke_item)(RpgBattler *target, int kind, int id);
    void (*apply_item_effects)(RpgBattler *target, int kind, int id);
    void (*invoke_counter_attack)(RpgBattler *target, int kind, int id);
    void (*invoke_magic_reflection)(RpgBattler *target, int kind, int id);
    RpgBattler *(*apply_substitute)(RpgBattler *target, int kind, int id);
    void (*show_animation)(RpgBattler **targets, int n, int animation_id);
    void (*show_attack_animation)(RpgBattler **targets, int n);
    void (*show_normal_animation)(RpgBattler **targets, int n, int animation_id, int mirror);
    void (*make_actor_commands)(RpgActorState *a);   /* fills rpg_bw.cmds */
    /* the log's messages */
    void (*display_use_item)(RpgBattler *subject, int kind, int id);
    void (*display_action_results)(RpgBattler *target, int kind, int id);
    void (*display_damage)(RpgBattler *target, int kind, int id);
    void (*display_affected_status)(RpgBattler *target, int kind, int id);
    void (*display_auto_affected_status)(RpgBattler *target);
    void (*display_current_state)(RpgBattler *subject);
    /* drawing */
    void (*draw)(void);
    void (*draw_back)(void);
    void (*draw_battlers)(void);
    void (*draw_windows)(void);
    void (*draw_status_item)(RpgWin *w, int i, RpgRect r);
    void (*sprite_update)(RpgBattler *b, RpgBattlerSprite *s);
    void (*draw_battler)(RpgBattler *b, RpgBattlerSprite *s);
    float (*enemy_screen_y)(const RpgEnemyState *e);   /* troop y -> screen y (the project's height) */
    float (*enemy_screen_x)(const RpgEnemyState *e);
    void (*wait)(int frames);                  /* Scene_Battle#wait / #abs_wait */
    void (*abs_wait)(int frames);
    void (*refresh_status)(void);
    void (*display_critical)(RpgBattler *target, int kind, int id);
} RpgBattleVT;
extern int rpg_battle_anim_omit, rpg_battle_effect_omit;   /* waits end this many frames early (0) */
int rpg_anim_remaining(void *target);                      /* frames left of a target's animation, 0 none */
int rpg_battle_effect_remaining(const RpgBattlerSprite *s);
extern RpgBattleVT rpg_bvt;
extern const RpgBattleVT rpg_bdef;

/* hooks for the project's rules */
extern int (*rpg_hooks_enemy_turn)(const RpgEnemyState *e);    /* the turn count enemy action conditions use */
extern void (*rpg_hooks_enemy_actions)(RpgEnemyState *e);       /* replace Game_Enemy#make_actions' choice */
extern int rpg_hooks_force_action_nowait;                        /* 339 forces all battlers without waiting */
extern const char *(*rpg_battle_command_name_hook)(int cmd, int ext);
RpgActorState *rpg_battle_actor(void);                           /* BattleManager.actor */
int rpg_battle_select(RpgWin *w);                                /* run a window until decided (coroutine) */
int rpg_battle_input_ended(void);   /* the scene is changing or rpg_b.input_abort: command selection returns at once */

/* drawing helpers */
float rpg_battleback_oy(void);   /* the vertical shift of the battle backgrounds (screen shorter than 480) */
void rpg_battle_draw_status_default(RpgWin *w, int i, RpgRect r);

#endif
