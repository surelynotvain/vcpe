/* VCPE RPG characters: Game_CharacterBase, Game_Character (move routes), Game_Event's self movement and
 * Game_Player (input, scrolling, transfers, encounters, event triggers), plus Sprite_Character drawing.
 * The arithmetic follows VX Ace exactly (real positions as floats, distance_per_frame = 2^speed / 256, animation
 * counters in 1.5 steps while walking). */
#include "vcpe_rpg.h"
#include "vcpe_spr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static RpgChar *routine_char;   /* the character whose route script is being evaluated */

/* ------------------------------------------------------------------ base */
int rpg_char_moving(const RpgChar *c) { return c->real_x != c->x || c->real_y != c->y; }
int rpg_char_jumping(const RpgChar *c) { return c->jump_count > 0; }
int rpg_char_stopping(const RpgChar *c) { return !rpg_char_moving(c) && !rpg_char_jumping(c); }

int rpg_player_dash(void);
int rpg_char_real_speed(const RpgChar *c) { return c->move_speed + ((c->kind == 1 && rpg_player_dash()) ? 1 : 0); }
float rpg_char_distance_per_frame(const RpgChar *c) { return (float)(1 << rpg_char_real_speed(c)) / 256.f; }

void rpg_char_straighten(RpgChar *c)
{
    if (c->walk_anime || c->step_anime)
        c->pattern = 1;
    c->anime_count = 0;
}

int rpg_char_object(const RpgChar *c) { return c->tile_id > 0 || c->character_name[0] == '!'; }

static int jump_height(const RpgChar *c)
{
    int d = c->jump_count - c->jump_peak;
    return (c->jump_peak * c->jump_peak - d * d) / 2;
}

int rpg_char_screen_x(const RpgChar *c) { return (int)floorf(rpg_adjust_x(c->real_x) * 32 + 16 + 0.0001f); }
int rpg_char_screen_y(const RpgChar *c)
{
    return (int)floorf(rpg_adjust_y(c->real_y) * 32 + 32 + 0.0001f) - (rpg_char_object(c) ? 0 : 4) - jump_height(c);
}
int rpg_char_screen_z(const RpgChar *c) { return c->priority_type * 100; }

void rpg_char_update_bush(RpgChar *c)
{
    if (c->priority_type == 1 && !rpg_char_object(c) && rpg_map_bush(c->x, c->y) && !rpg_char_jumping(c)) {
        if (!rpg_char_moving(c))
            c->bush_depth = 8;
    } else {
        c->bush_depth = 0;
    }
}

void rpg_char_set_direction(RpgChar *c, int d)
{
    if (!c->direction_fix && d != 0)
        c->direction = d;
    c->stop_count = 0;
}

void rpg_char_moveto(RpgChar *c, int x, int y)
{
    int w = rpg_map_width(), h = rpg_map_height();
    c->x = w ? ((x % w) + w) % w : x;
    c->y = h ? ((y % h) + h) % h : y;
    c->real_x = c->x;
    c->real_y = c->y;
    c->prelock_direction = 0;
    rpg_char_straighten(c);
    rpg_char_update_bush(c);
}

void rpg_char_set_graphic(RpgChar *c, const char *name, int index)
{
    c->tile_id = 0;
    snprintf(c->character_name, sizeof c->character_name, "%s", name);
    c->character_index = index;
    c->original_pattern = 1;
}

static int collide_events(RpgChar *c, int x, int y)
{
    RpgChar *list[16];
    int n = rpg_map_events_xy(x, y, list, 16, 1);
    for (int i = 0; i < n; i++)
        if (list[i] != c && (list[i]->priority_type == 1 || c->kind == 0))
            return 1;
    return 0;
}

static int collide_chars(RpgChar *c, int x, int y)
{
    if (collide_events(c, x, y))
        return 1;
    /* Game_Event: also the player when the event is on the same layer */
    if (c->kind == 0 && c->priority_type == 1 && rpg_player_collide(x, y))
        return 1;
    return 0;
}

int rpg_char_passable(RpgChar *c, int x, int y, int d)
{
    int x2 = rpg_round_x_dir(x, d), y2 = rpg_round_y_dir(y, d);
    int r;
    if (!rpg_map_valid(x2, y2))
        r = 0;
    else if (c->through)
        r = 1;
    else if (!rpg_map_passable(x, y, d) || !rpg_map_passable(x2, y2, 10 - d))
        r = 0;
    else
        r = !collide_chars(c, x2, y2);
    if (rpg_hooks.passable)
        r = rpg_hooks.passable(c, x, y, d, r);
    return r;
}

int rpg_char_diagonal_passable(RpgChar *c, int x, int y, int horz, int vert)
{
    int x2 = rpg_round_x_dir(x, horz), y2 = rpg_round_y_dir(y, vert);
    return (rpg_char_passable(c, x, y, vert) && rpg_char_passable(c, x, y2, horz)) ||
           (rpg_char_passable(c, x, y, horz) && rpg_char_passable(c, x2, y, vert));
}

static void increase_steps(RpgChar *c);
static void check_touch(RpgChar *c, int x, int y);

void rpg_char_move_straight(RpgChar *c, int d, int turn_ok)
{
    c->move_succeed = rpg_char_passable(c, c->x, c->y, d);
    if (c->move_succeed) {
        rpg_char_set_direction(c, d);
        c->x = rpg_round_x_dir(c->x, d);
        c->y = rpg_round_y_dir(c->y, d);
        c->real_x = c->x + (d == 4 ? 1 : d == 6 ? -1 : 0);
        c->real_y = c->y + (d == 8 ? 1 : d == 2 ? -1 : 0);
        increase_steps(c);
    } else if (turn_ok) {
        rpg_char_set_direction(c, d);
        check_touch(c, rpg_round_x_dir(c->x, c->direction), rpg_round_y_dir(c->y, c->direction));
    }
}

void rpg_char_move_diagonal(RpgChar *c, int horz, int vert)
{
    c->move_succeed = rpg_char_diagonal_passable(c, c->x, c->y, horz, vert);
    if (c->move_succeed) {
        c->x = rpg_round_x_dir(c->x, horz);
        c->y = rpg_round_y_dir(c->y, vert);
        c->real_x = c->x + (horz == 4 ? 1 : -1);
        c->real_y = c->y + (vert == 8 ? 1 : -1);
        increase_steps(c);
    }
    if (c->direction == 10 - horz)
        rpg_char_set_direction(c, horz);
    if (c->direction == 10 - vert)
        rpg_char_set_direction(c, vert);
}

void rpg_char_jump(RpgChar *c, int xp, int yp)
{
    if (abs(xp) > abs(yp)) {
        if (xp != 0)
            rpg_char_set_direction(c, xp < 0 ? 4 : 6);
    } else if (yp != 0) {
        rpg_char_set_direction(c, yp < 0 ? 8 : 2);
    }
    c->x += xp;
    c->y += yp;
    int dist = (int)floorf(sqrtf((float)(xp * xp + yp * yp)) + 0.5f);
    c->jump_peak = 10 + dist - c->move_speed;
    c->jump_count = c->jump_peak * 2;
    c->stop_count = 0;
    rpg_char_straighten(c);
}

int rpg_char_distance_x(const RpgChar *c, int x)
{
    int r = c->x - x, w = rpg_map_width();
    if (rpg_map_loop_h() && abs(r) > w / 2)
        r += r < 0 ? w : -w;
    return r;
}

int rpg_char_distance_y(const RpgChar *c, int y)
{
    int r = c->y - y, h = rpg_map_height();
    if (rpg_map_loop_v() && abs(r) > h / 2)
        r += r < 0 ? h : -h;
    return r;
}

void rpg_char_move_random(RpgChar *c) { rpg_char_move_straight(c, 2 + rpg_rand(4) * 2, 0); }
void rpg_char_move_forward(RpgChar *c) { rpg_char_move_straight(c, c->direction, 1); }

void rpg_char_move_toward(RpgChar *c, const RpgChar *t)
{
    int sx = rpg_char_distance_x(c, t->x), sy = rpg_char_distance_y(c, t->y);
    if (abs(sx) > abs(sy)) {
        rpg_char_move_straight(c, sx > 0 ? 4 : 6, 1);
        if (!c->move_succeed && sy != 0)
            rpg_char_move_straight(c, sy > 0 ? 8 : 2, 1);
    } else if (sy != 0) {
        rpg_char_move_straight(c, sy > 0 ? 8 : 2, 1);
        if (!c->move_succeed && sx != 0)
            rpg_char_move_straight(c, sx > 0 ? 4 : 6, 1);
    }
}

void rpg_char_move_away(RpgChar *c, const RpgChar *t)
{
    int sx = rpg_char_distance_x(c, t->x), sy = rpg_char_distance_y(c, t->y);
    if (abs(sx) > abs(sy)) {
        rpg_char_move_straight(c, sx > 0 ? 6 : 4, 1);
        if (!c->move_succeed && sy != 0)
            rpg_char_move_straight(c, sy > 0 ? 2 : 8, 1);
    } else if (sy != 0) {
        rpg_char_move_straight(c, sy > 0 ? 2 : 8, 1);
        if (!c->move_succeed && sx != 0)
            rpg_char_move_straight(c, sx > 0 ? 6 : 4, 1);
    }
}

void rpg_char_turn_toward(RpgChar *c, const RpgChar *t)
{
    int sx = rpg_char_distance_x(c, t->x), sy = rpg_char_distance_y(c, t->y);
    if (abs(sx) > abs(sy))
        rpg_char_set_direction(c, sx > 0 ? 4 : 6);
    else if (sy != 0)
        rpg_char_set_direction(c, sy > 0 ? 8 : 2);
}

static void turn_away(RpgChar *c, const RpgChar *t)
{
    int sx = rpg_char_distance_x(c, t->x), sy = rpg_char_distance_y(c, t->y);
    if (abs(sx) > abs(sy))
        rpg_char_set_direction(c, sx > 0 ? 6 : 4);
    else if (sy != 0)
        rpg_char_set_direction(c, sy > 0 ? 2 : 8);
}

static void turn_right_90(RpgChar *c)
{
    static const int t[10] = {0, 0, 4, 0, 8, 0, 2, 0, 6, 0};
    rpg_char_set_direction(c, t[c->direction % 10]);
}
static void turn_left_90(RpgChar *c)
{
    static const int t[10] = {0, 0, 6, 0, 2, 0, 8, 0, 4, 0};
    rpg_char_set_direction(c, t[c->direction % 10]);
}

/* ------------------------------------------------------------------ move routes */
static const RpgRoute *route_of(const RpgChar *c)
{
    if (c->route < 0 || !c->route_doc)
        return 0;
    return (const RpgRoute *)rpg_rec(rpg_doc_by_id(c->route_doc), RPG_T_ROUTE, c->route);
}

void rpg_char_force_route(RpgChar *c, const RpgDoc *doc, int route)
{
    if (!c->has_orig_route) {
        c->orig_route_doc = c->route_doc;
        c->orig_route = c->route;
        c->orig_route_index = c->route_index;
        c->has_orig_route = 1;
    }
    c->route_doc = rpg_doc_id(doc);
    c->route = route;
    c->route_index = 0;
    c->move_route_forcing = 1;
    c->prelock_direction = 0;
    c->wait_count = 0;
}

static void restore_route(RpgChar *c)
{
    c->route_doc = c->orig_route_doc;
    c->route = c->orig_route;
    c->route_index = c->orig_route_index;
    c->has_orig_route = 0;
}

static int mp(const RpgChar *c, const RpgMoveCmd *m, int k)
{
    return (k < m->params.count) ? *(const int32_t *)rpg_rec(rpg_doc_by_id(c->route_doc), RPG_T_PARAM, m->params.first + k) : 0;
}

static void process_move(RpgChar *c, const RpgRoute *r, const RpgMoveCmd *m)
{
    RpgChar *pl = &rpg.player.ch;
    switch (m->code) {
    case 0:
        if (r->repeat)
            c->route_index = -1;
        else if (c->move_route_forcing) {
            c->move_route_forcing = 0;
            restore_route(c);   /* advance_move_route_index then steps the restored route on, as VX Ace */
        }
        break;
    case 1: rpg_char_move_straight(c, 2, 1); break;
    case 2: rpg_char_move_straight(c, 4, 1); break;
    case 3: rpg_char_move_straight(c, 6, 1); break;
    case 4: rpg_char_move_straight(c, 8, 1); break;
    case 5: rpg_char_move_diagonal(c, 4, 2); break;
    case 6: rpg_char_move_diagonal(c, 6, 2); break;
    case 7: rpg_char_move_diagonal(c, 4, 8); break;
    case 8: rpg_char_move_diagonal(c, 6, 8); break;
    case 9: rpg_char_move_random(c); break;
    case 10: rpg_char_move_toward(c, pl); break;
    case 11: rpg_char_move_away(c, pl); break;
    case 12: rpg_char_move_forward(c); break;
    case 13: {
        int fix = c->direction_fix;
        c->direction_fix = 1;
        rpg_char_move_straight(c, 10 - c->direction, 0);
        c->direction_fix = fix;
        break;
    }
    case 14: rpg_char_jump(c, mp(c, m, 0), mp(c, m, 1)); break;
    case 15: c->wait_count = mp(c, m, 0) - 1; break;
    case 16: rpg_char_set_direction(c, 2); break;
    case 17: rpg_char_set_direction(c, 4); break;
    case 18: rpg_char_set_direction(c, 6); break;
    case 19: rpg_char_set_direction(c, 8); break;
    case 20: turn_right_90(c); break;
    case 21: turn_left_90(c); break;
    case 22: rpg_char_set_direction(c, 10 - c->direction); break;
    case 23: if (rpg_rand(2)) turn_left_90(c); else turn_right_90(c); break;
    case 24: rpg_char_set_direction(c, 2 + rpg_rand(4) * 2); break;
    case 25: rpg_char_turn_toward(c, pl); break;
    case 26: turn_away(c, pl); break;
    case 27: rpg_sw_set(mp(c, m, 0), 1); break;
    case 28: rpg_sw_set(mp(c, m, 0), 0); break;
    case 29: c->move_speed = mp(c, m, 0); break;
    case 30: c->move_frequency = mp(c, m, 0); break;
    case 31: c->walk_anime = 1; break;
    case 32: c->walk_anime = 0; break;
    case 33: c->step_anime = 1; break;
    case 34: c->step_anime = 0; break;
    case 35: c->direction_fix = 1; break;
    case 36: c->direction_fix = 0; break;
    case 37: c->through = 1; break;
    case 38: c->through = 0; break;
    case 39: c->transparent = 1; break;
    case 40: c->transparent = 0; break;
    case 41: rpg_char_set_graphic(c, rpg_s(rpg_doc_by_id(c->route_doc), mp(c, m, 0)), mp(c, m, 1)); break;
    case 42: c->opacity = mp(c, m, 0); break;
    case 43: c->blend_type = mp(c, m, 0); break;
    case 44: rpg_se_play(rpg_s(rpg_doc_by_id(c->route_doc), mp(c, m, 0)), mp(c, m, 1), mp(c, m, 2)); break;
    case 45:
        if (rpg_hooks.script) {
            routine_char = c;
            rpg_hooks.script(0, rpg_s(rpg_doc_by_id(c->route_doc), mp(c, m, 0)), 2);
            routine_char = 0;
        }
        break;
    }
}

RpgChar *rpg_route_character(void) { return routine_char; }

static void update_routine_move(RpgChar *c)
{
    if (c->wait_count > 0) {
        c->wait_count--;
        return;
    }
    c->move_succeed = 1;
    const RpgRoute *r = route_of(c);
    if (!r)
        return;
    const RpgMoveCmd *m = c->route_index >= 0 && c->route_index < r->list.count
                              ? (const RpgMoveCmd *)rpg_rec(rpg_doc_by_id(c->route_doc), RPG_T_MOVECMD, r->list.first + c->route_index)
                              : 0;
    if (m) {
        int doc = c->route_doc;
        int route = c->route;
        process_move(c, r, m);
        /* advance_move_route_index: on the route that was current (a route end may have restored another) */
        if (c->route_doc == doc && c->route == route) {
            if (c->move_succeed || r->skippable)
                c->route_index++;
        } else {
            c->route_index++;
        }
    }
}

/* ------------------------------------------------------------------ update */
static void update_animation(RpgChar *c)
{
    if (rpg_char_moving(c) && c->walk_anime)
        c->anime_count += 1.5f;
    else if (c->step_anime || c->pattern != c->original_pattern)
        c->anime_count += 1;
    if (c->anime_count > 18 - rpg_char_real_speed(c) * 2) {
        if (!c->step_anime && c->stop_count > 0)
            c->pattern = c->original_pattern;
        else
            c->pattern = (c->pattern + 1) % 4;
        c->anime_count = 0;
    }
}

static void update_jump(RpgChar *c)
{
    c->jump_count--;
    c->real_x = (c->real_x * c->jump_count + c->x) / (c->jump_count + 1.0f);
    c->real_y = (c->real_y * c->jump_count + c->y) / (c->jump_count + 1.0f);
    rpg_char_update_bush(c);
    if (c->jump_count == 0) {
        c->real_x = c->x = rpg_round_x(c->x);
        c->real_y = c->y = rpg_round_y(c->y);
    }
}

static void update_move(RpgChar *c)
{
    float d = rpg_char_distance_per_frame(c);
    if (c->x < c->real_x) c->real_x = c->real_x - d > c->x ? c->real_x - d : c->x;
    if (c->x > c->real_x) c->real_x = c->real_x + d < c->x ? c->real_x + d : c->x;
    if (c->y < c->real_y) c->real_y = c->real_y - d > c->y ? c->real_y - d : c->y;
    if (c->y > c->real_y) c->real_y = c->real_y + d < c->y ? c->real_y + d : c->y;
    if (!rpg_char_moving(c))
        rpg_char_update_bush(c);
}

static int near_screen(const RpgChar *c)
{
    float ax = rpg_adjust_x(c->real_x) - rpg_screen_w / 2 / 32;
    float ay = rpg_adjust_y(c->real_y) - rpg_screen_h / 2 / 32;
    int r = ax >= -12 && ax <= 12 && ay >= -8 && ay <= 8;
    if (rpg_hooks.near_screen)
        r = rpg_hooks.near_screen(c, r);
    return r;
}

static void self_movement(RpgChar *c)
{
    if (!near_screen(c) || c->stop_count <= 30 * (5 - c->move_frequency))
        return;
    switch (c->move_type) {
    case 1:
        switch (rpg_rand(6)) {
        case 0: case 1: rpg_char_move_random(c); break;
        case 2: case 3: case 4: rpg_char_move_forward(c); break;
        default: c->stop_count = 0;
        }
        break;
    case 2: {
        int sx = abs(rpg_char_distance_x(c, rpg.player.ch.x)), sy = abs(rpg_char_distance_y(c, rpg.player.ch.y));
        if (sx + sy < 20) {
            int r = rpg_rand(6);
            if (r <= 3) rpg_char_move_toward(c, &rpg.player.ch);
            else if (r == 4) rpg_char_move_random(c);
            else rpg_char_move_forward(c);
        } else {
            rpg_char_move_random(c);
        }
        break;
    }
    case 3: update_routine_move(c); break;
    }
}

static void update_stop(RpgChar *c)
{
    if (!c->locked)
        c->stop_count++;
    if (c->move_route_forcing)
        update_routine_move(c);
    else if (c->kind == 0)
        self_movement(c);
}

void rpg_char_update(RpgChar *c)
{
    update_animation(c);
    if (rpg_char_jumping(c))
        update_jump(c);
    else if (rpg_char_moving(c))
        update_move(c);
    else
        update_stop(c);
}

/* ------------------------------------------------------------------ steps and touch triggers */
void rpg_party_increase_steps(void);

static void increase_steps(RpgChar *c)
{
    if (rpg_map_ladder(c->x, c->y))
        rpg_char_set_direction(c, 8);
    c->stop_count = 0;
    rpg_char_update_bush(c);
    if (c->kind == 1 && !c->move_route_forcing && rpg.player.vehicle < 0)
        rpg_party_increase_steps();
}

static void start_map_event(int x, int y, int trig_mask, int normal)
{
    RpgChar *list[16];
    int n = rpg_map_events_xy(x, y, list, 16, 0);
    for (int i = 0; i < n; i++) {
        RpgChar *e = list[i];
        if (e->trigger >= 0 && (trig_mask & (1 << e->trigger)) && (e->priority_type == 1) == normal)
            rpg_event_start(e);
    }
}

static void check_touch(RpgChar *c, int x, int y)
{
    if (c->kind == 1) {
        if (!rpg_interp_running(&rpg.interp))
            start_map_event(x, y, 2 | 4, 1);
    } else if (c->kind == 0) {
        /* Game_Event#check_event_trigger_touch */
        if (rpg_interp_running(&rpg.interp))
            return;
        if (c->trigger == 2 && rpg.player.ch.x == x && rpg.player.ch.y == y && !rpg_char_jumping(c) &&
            c->priority_type == 1)
            rpg_event_start(c);
    }
}

/* ------------------------------------------------------------------ the player */
int rpg_player_collide(int x, int y)
{
    RpgChar *p = &rpg.player.ch;
    return !p->through && p->x == x && p->y == y;
}

static float center_x(void) { return (rpg_screen_w / 32 - 1) / 2.0f; }
static float center_y(void) { return (rpg_screen_h / 32 - 1) / 2.0f; }

void rpg_player_center(int x, int y) { rpg_map_set_display(x - center_x(), y - center_y()); }

void rpg_player_make_encounter_count(void)
{
    const RpgMap *m = rpg_map();
    int n = m ? m->encounter_step : 30;
    rpg.player.encounter_count = (n > 0 ? rpg_rand(n) + rpg_rand(n) : 0) + 1;
}

void rpg_player_moveto(int x, int y)
{
    rpg_char_moveto(&rpg.player.ch, x, y);
    rpg_player_center(x, y);
    rpg_player_make_encounter_count();
}

const char *rpg_party_leader_graphic(int *index);

void rpg_player_refresh(void)
{
    int idx = 0;
    const char *name = rpg_party_leader_graphic(&idx);
    snprintf(rpg.player.ch.character_name, sizeof rpg.player.ch.character_name, "%s", name ? name : "");
    rpg.player.ch.character_index = idx;
}

void rpg_player_reserve_transfer(int map_id, int x, int y, int d)
{
    rpg.player.transferring = 1;
    rpg.player.new_map = map_id;
    rpg.player.new_x = x;
    rpg.player.new_y = y;
    rpg.player.new_dir = d;
}

int rpg_player_dash(void)
{
    RpgPlayer *p = &rpg.player;
    if (p->ch.move_route_forcing)
        return 0;
    const RpgMap *m = rpg_map();
    if (m && m->disable_dashing)
        return 0;
    if (p->vehicle >= 0)
        return 0;
    return (rpg_in.press & RPG_K_A) != 0;
}

int rpg_player_movable(void)
{
    RpgPlayer *p = &rpg.player;
    if (rpg_char_moving(&p->ch))
        return 0;
    if (p->ch.move_route_forcing)
        return 0;
    if (p->getting_on || p->getting_off)
        return 0;
    if (rpg_msg_busy() || rpg_msg.visible)
        return 0;
    return 1;
}

int rpg_player_encounter_troop(void)
{
    const RpgMap *m = rpg_map();
    if (!m)
        return 0;
    int sum = 0, region = rpg_map_region(rpg.player.ch.x, rpg.player.ch.y);
    int ok[64], n = 0;
    for (int i = 0; i < m->encounter_list.count && n < 64; i++) {
        const RpgEncounter *e = rpg_at(&rpg_mapdoc, RPG_T_ENCOUNTER, m->encounter_list, i);
        int good = e->region_set.count == 0;
        for (int k = 0; k < e->region_set.count && !good; k++)
            good = *(const int32_t *)rpg_at(&rpg_mapdoc, RPG_T_INT, e->region_set, k) == region;
        if (good) {
            ok[n++] = i;
            sum += e->weight;
        }
    }
    if (sum <= 0)
        return 0;
    int v = rpg_rand(sum);
    for (int i = 0; i < n; i++) {
        const RpgEncounter *e = rpg_at(&rpg_mapdoc, RPG_T_ENCOUNTER, m->encounter_list, ok[i]);
        v -= e->weight;
        if (v < 0)
            return e->troop_id;
    }
    return 0;
}

static void move_by_input(void)
{
    if (!rpg_player_movable() || rpg_interp_running(&rpg.interp))
        return;
    if (rpg_hooks.player_input) {
        rpg_hooks.player_input();
        return;
    }
    int d = rpg_dir4();
    if (d > 0)
        rpg_char_move_straight(&rpg.player.ch, d, 1);
}

static void update_scroll(float lx, float ly)
{
    RpgChar *p = &rpg.player.ch;
    float ax1 = rpg_adjust_x(lx), ay1 = rpg_adjust_y(ly), ax2 = rpg_adjust_x(p->real_x), ay2 = rpg_adjust_y(p->real_y);
    if (ay2 > ay1 && ay2 > center_y())
        rpg_map_scroll(2, ay2 - ay1);
    if (ax2 < ax1 && ax2 < center_x())
        rpg_map_scroll(4, ax1 - ax2);
    if (ax2 > ax1 && ax2 > center_x())
        rpg_map_scroll(6, ax2 - ax1);
    if (ay2 < ay1 && ay2 < center_y())
        rpg_map_scroll(8, ay1 - ay2);
}

int rpg_party_encounter_none(void);
int rpg_party_encounter_half(void);
void rpg_party_on_player_walk(void);

static int check_event_here(int mask)
{
    start_map_event(rpg.player.ch.x, rpg.player.ch.y, mask, 0);
    return rpg_map_setup_starting_event();
}

int rpg_player_check_action(void)
{
    RpgChar *p = &rpg.player.ch;
    if (check_event_here(1))
        return 1;
    int x2 = rpg_round_x_dir(p->x, p->direction), y2 = rpg_round_y_dir(p->y, p->direction);
    start_map_event(x2, y2, 1 | 2 | 4, 1);
    if (!rpg_map_any_starting() && rpg_map_counter(x2, y2)) {
        int x3 = rpg_round_x_dir(x2, p->direction), y3 = rpg_round_y_dir(y2, p->direction);
        start_map_event(x3, y3, 1 | 2 | 4, 1);
    }
    return rpg_map_setup_starting_event();
}

static void update_nonmoving(int last_moving)
{
    if (rpg_interp_running(&rpg.interp))
        return;
    if (last_moving) {
        rpg_party_on_player_walk();
        if (check_event_here(2 | 4))
            return;
    }
    if (rpg_player_movable() && (rpg_in.trigger & RPG_K_C)) {
        if (rpg_player_check_action())
            return;
    }
    if (last_moving && !rpg_party_encounter_none() && !rpg.player.ch.move_route_forcing) {
        float v = rpg_map_bush(rpg.player.ch.x, rpg.player.ch.y) ? 2 : 1;
        if (rpg_party_encounter_half())
            v *= 0.5f;
        if (rpg.player.vehicle == 1)
            v *= 0.5f;
        rpg.player.encounter_count -= v;
    }
}

void rpg_player_update(void)
{
    RpgChar *p = &rpg.player.ch;
    float lx = p->real_x, ly = p->real_y;
    int last_moving = rpg_char_moving(p);
    move_by_input();
    rpg_char_update(p);
    update_scroll(lx, ly);
    if (!rpg_char_moving(p))
        update_nonmoving(last_moving);
}

/* ------------------------------------------------------------------ drawing */
static void draw_char(const RpgChar *c)
{
    if (c->transparent || (!c->character_name[0] && c->tile_id <= 0) || c->opacity <= 0)
        return;
    float x = rpg_char_screen_x(c) + rpg.screen.shake, y = rpg_char_screen_y(c);
    uint32_t col = rpg_rgba(255, 255, 255, c->opacity);
    if (c->tile_id > 0) {
        int id = c->tile_id;
        int img = rpg_map_tile_image(5 + id / 256);
        int sx = (id / 128 % 2 * 8 + id % 8) * 32, sy = id % 256 / 8 % 16 * 32;
        rpg_draw_part(img, sx, sy, 32, 32, x - 16, y - 32, 1, 1, 0, col, c->blend_type);
        return;
    }
    int img = rpg_image("characters", c->character_name);
    if (img < 0)
        return;
    int w = rpg_image_w(img), h = rpg_image_h(img);
    const char *n = c->character_name;
    int single = (n[0] == '$') || (n[0] == '!' && n[1] == '$');
    int cw = single ? w / 3 : w / 12, ch = single ? h / 4 : h / 8;
    int index = single ? 0 : c->character_index;
    int pattern = c->pattern < 3 ? c->pattern : 1;
    int sx = (index % 4 * 3 + pattern) * cw, sy = (index / 4 * 4 + (c->direction - 2) / 2) * ch;
    int bush = c->bush_depth;
    if (bush > 0 && bush < ch) {
        rpg_draw_part(img, sx, sy, cw, ch - bush, x - cw / 2.f, y - ch, 1, 1, 0, col, c->blend_type);
        rpg_draw_part(img, sx, sy + ch - bush, cw, bush, x - cw / 2.f, y - bush, 1, 1, 0,
                      rpg_rgba(255, 255, 255, c->opacity * 128 / 255), c->blend_type);
    } else {
        rpg_draw_part(img, sx, sy, cw, ch, x - cw / 2.f, y - ch, 1, 1, 0, col, c->blend_type);
    }
}

void rpg_char_draw(const RpgChar *c) { draw_char(c); }

void rpg_balloon_draw(const RpgChar *c);

/* every character on layers [lo, hi] (priority types), sorted like sprites: by z, then y, then creation order */
void rpg_char_draw_sorted(int lo, int hi)
{
    static int16_t order[RPG_MAX_EVENTS + 4];
    static int32_t keys[RPG_MAX_EVENTS + 4];
    int n = 0;
    for (int i = 0; i <= rpg.nevents; i++) {
        const RpgChar *c = i < rpg.nevents ? &rpg.events[i] : &rpg.player.ch;
        if (c->priority_type < lo || c->priority_type > hi)
            continue;
        order[n] = i;
        keys[n] = rpg_char_screen_z(c) * 100000 + rpg_char_screen_y(c) * 16 + 0;
        n++;
    }
    /* insertion sort: stable, and the list is nearly sorted from frame to frame */
    for (int i = 1; i < n; i++) {
        int16_t o = order[i];
        int32_t k = keys[i];
        int j = i - 1;
        while (j >= 0 && keys[j] > k) {
            keys[j + 1] = keys[j];
            order[j + 1] = order[j];
            j--;
        }
        keys[j + 1] = k;
        order[j + 1] = o;
    }
    for (int i = 0; i < n; i++) {
        const RpgChar *c = order[i] < rpg.nevents ? &rpg.events[order[i]] : &rpg.player.ch;
        draw_char(c);
    }
    for (int i = 0; i < n; i++) {
        const RpgChar *c = order[i] < rpg.nevents ? &rpg.events[order[i]] : &rpg.player.ch;
        if (c->balloon_id > 0)
            rpg_balloon_draw(c);
    }
}
