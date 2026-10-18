/* VCPE RPG menu helpers (vcpe_rpg_menu.c): Window_Base drawing and menu customisation. */
#ifndef VCPE_RPG_MENU_H
#define VCPE_RPG_MENU_H
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"

uint32_t rpg_color_normal(void);
uint32_t rpg_color_system(void);
uint32_t rpg_color_crisis(void);
uint32_t rpg_color_knockout(void);
uint32_t rpg_color_power_up(void);
uint32_t rpg_color_power_down(void);
uint32_t rpg_color_mp_cost(void);
uint32_t rpg_color_tp_cost(void);
uint32_t rpg_color_enabled(uint32_t c, int enabled);
uint32_t rpg_hp_color(const RpgBattler *b);
uint32_t rpg_mp_color(const RpgBattler *b);
void rpg_draw_text_c(float x, float y, float w, const char *s, uint32_t c, int align);
void rpg_draw_current_max(float x, float y, float w, int cur, int max, uint32_t c1, uint32_t c2);
void rpg_draw_actor_name(const RpgActorState *a, float x, float y, float w);
void rpg_draw_actor_class(const RpgActorState *a, float x, float y, float w);
void rpg_draw_actor_nickname(const RpgActorState *a, float x, float y, float w);
void rpg_draw_actor_level(const RpgActorState *a, float x, float y);
void rpg_draw_actor_icons(const RpgBattler *b, float x, float y, float w);
void rpg_draw_actor_hp(const RpgBattler *b, float x, float y, float w);
void rpg_draw_actor_mp(const RpgBattler *b, float x, float y, float w);
void rpg_draw_actor_tp(const RpgBattler *b, float x, float y, float w);
void rpg_draw_actor_face(const RpgActorState *a, float x, float y, float size, int enabled);
void rpg_draw_actor_simple_status(const RpgActorState *a, float x, float y);
void rpg_draw_actor_param(const RpgActorState *a, float x, float y, int p);
void rpg_draw_item_name(int kind, int id, float x, float y, int enabled, float w);
void rpg_draw_currency(int value, const char *unit, float x, float y, float w);
void rpg_draw_character(const char *name, int index, float x, float y);
void rpg_menu_background(void);
void rpg_help_draw(RpgWin *w, const char *text);

/* the game's additions to the main menu: command ids >= 100 */
typedef struct {
    int (*commands)(int *out, int max);      /* the command list (built-in ids 0..6, the game's >= 100) */
    const char *(*command_name)(int id);
    void (*command_ok)(int id);
    void (*draw)(void);
} RpgMenuHooks;
extern RpgMenuHooks rpg_menu_hooks;

#endif
