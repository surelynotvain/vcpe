/* VCPE RPG interpreter: Game_Interpreter as a resumable state machine instead of a Ruby Fiber.
 *
 * Fiber.yield points become returns from rpg_interp_update: a command that has to wait either asks to be run again
 * next frame before it does anything ("pre-wait": nothing changed yet, so running it again is the same as resuming)
 * or does its work, moves on and leaves a wait condition for the following frames ("post-wait"). The frame timing
 * matches VX Ace's, including the one frame a finished list (and a finished common-event call) still yields: a
 * parallel event without waits runs every second frame there too.
 *
 * Common-event calls (117) push a frame with its own branch table, like VX Ace's child interpreter. */
#include "vcpe_rpg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static RpgInterp *current;
RpgInterp *rpg_interp_current(void) { return current; }

/* ------------------------------------------------------------------ lists */
RpgList rpg_frame_list(const RpgFrame *f)
{
    RpgList l = {&rpg_db, {0, 0}};
    switch (f->src) {
    case 0: {
        /* a map event's page, of the map the event started on (it keeps running after a transfer, as in VX Ace) */
        const RpgDoc *d = f->map == rpg.map_id ? &rpg_mapdoc : rpg_map_doc(f->map);
        const RpgEvent *ev = d ? rpg_doc_event(d, f->a) : 0;
        if (ev) {
            const RpgPage *p = (const RpgPage *)rpg_at(d, RPG_T_PAGE, ev->pages, f->b);
            if (p) {
                l.doc = d;
                l.r = p->list;
            }
        }
        break;
    }
    case 1: {
        const RpgCommonEvent *ce = rpg_common_data(f->a);
        l.r = ce->list;
        break;
    }
    case 2: {
        const RpgTroop *t = rpg_troop_data(f->a);
        const RpgTroopPage *p = (const RpgTroopPage *)rpg_at(&rpg_db, RPG_T_TROOPPAGE, t->pages, f->b);
        if (p)
            l.r = p->list;
        break;
    }
    default:
        if (rpg_game_list)
            l = rpg_game_list(f->a);
        break;
    }
    return l;
}

RpgList (*rpg_game_list)(int id);

void rpg_interp_clear(RpgInterp *in)
{
    int main = in->main, battle = in->battle;
    memset(in, 0, sizeof *in);
    in->main = main;
    in->battle = battle;
}

static void push(RpgInterp *in, int src, int a, int b, int event_id)
{
    if (in->depth >= RPG_INTERP_DEPTH) {
        rpg_log("RPG event calls nested too deep");
        return;
    }
    RpgFrame *f = &in->f[in->depth++];
    memset(f, 0, sizeof *f);
    f->src = src;
    f->a = a;
    f->b = b;
    f->event_id = event_id;
    f->map = rpg.map_id;
    memset(f->branch, -1, sizeof f->branch);
    f->phase = 1;   /* run starts with wait_for_message */
}

void rpg_interp_setup(RpgInterp *in, int src, int a, int b, int event_id)
{
    rpg_interp_clear(in);
    in->map_id = rpg.map_id;
    push(in, src, a, b, event_id);
}

int rpg_interp_running(const RpgInterp *in) { return in->depth > 0; }

/* ------------------------------------------------------------------ helpers */
int rpg_operate_value(int op, int type, int operand)
{
    int v = type == 0 ? operand : rpg_var(operand);
    return op == 0 ? v : -v;
}

void rpg_operate_variable(int id, int op, int value)
{
    int32_t cur = rpg_var(id), r = cur;
    switch (op) {
    case 0: r = value; break;
    case 1: r = cur + value; break;
    case 2: r = cur - value; break;
    case 3: r = cur * value; break;
    case 4:   /* Ruby: floor division; by zero -> rescue -> 0 */
        if (value == 0) r = 0;
        else { r = cur / value; if ((cur % value != 0) && ((cur < 0) != (value < 0))) r--; }
        break;
    case 5:
        if (value == 0) r = 0;
        else { r = cur % value; if (r != 0 && ((r < 0) != (value < 0))) r += value; }
        break;
    }
    rpg_var_set(id, r);
}

static int same_map(const RpgInterp *in) { return in->map_id == rpg.map_id; }

RpgChar *rpg_get_character(RpgInterp *in, int param)
{
    if (rpg.party.in_battle)
        return 0;
    if (param < 0)
        return &rpg.player.ch;
    if (!same_map(in))
        return 0;
    int id = param > 0 ? param : in->f[in->depth > 0 ? in->depth - 1 : 0].event_id;
    return rpg_map_event(id);
}

static RpgScreen *screen(void) { return rpg.party.in_battle ? &rpg_battle_screen : &rpg.screen; }

/* ------------------------------------------------------------------ waits */
static int still_waiting(RpgInterp *in)
{
    switch (in->wait_mode) {
    case RPG_W_MESSAGE: return rpg_msg_busy();
    case RPG_W_MSG_HIDDEN: return rpg_msg.visible;
    case RPG_W_ROUTE: {
        RpgChar *c = in->wait_arg < 0 ? &rpg.player.ch : rpg_map_event(in->wait_arg);
        return c && c->move_route_forcing;
    }
    case RPG_W_ANIM: {
        RpgChar *c = in->wait_arg < 0 ? &rpg.player.ch : rpg_map_event(in->wait_arg);
        return c && c->animation_id > 0;
    }
    case RPG_W_BALLOON: {
        RpgChar *c = in->wait_arg < 0 ? &rpg.player.ch : rpg_map_event(in->wait_arg);
        return c && c->balloon_id > 0;
    }
    case RPG_W_TRANSFER: return rpg.player.transferring;
    case RPG_W_SCROLLMAP: return rpg.scroll_rest > 0;
    case RPG_W_FORCED_ACTION: return rpg_battle_action_forced();
    case RPG_W_GAME: return rpg_hooks_wait ? rpg_hooks_wait(in, in->wait_arg) : 0;
    }
    return 0;
}

static void wait_frames(RpgInterp *in, int n) { in->wait_count = n; }

/* ------------------------------------------------------------------ commands */
#define P(k) rpg_p(l, c, (k))
#define PS(k) rpg_ps(l, c, (k))
enum { NEXT, YIELD_SAME, YIELD_NEXT, JUMPED };

static void skip_branch(RpgList l, RpgFrame *f)
{
    const RpgCmd *c = rpg_cmd(l, f->index);
    int indent = c ? c->indent : 0;
    while (f->index + 1 < l.r.count && rpg_cmd(l, f->index + 1)->indent > indent)
        f->index++;
}

static int next_code(RpgList l, RpgFrame *f)
{
    const RpgCmd *n = rpg_cmd(l, f->index + 1);
    return n ? n->code : 0;
}

static int cond_111(RpgInterp *in, RpgList l, const RpgCmd *c, RpgFrame *f)
{
    int r = 0;
    switch (P(0)) {
    case 0: r = rpg_sw(P(1)) == (P(2) == 0); break;
    case 1: {
        int v1 = rpg_var(P(1)), v2 = P(2) == 0 ? P(3) : rpg_var(P(3));
        switch (P(4)) {
        case 0: r = v1 == v2; break;
        case 1: r = v1 >= v2; break;
        case 2: r = v1 <= v2; break;
        case 3: r = v1 > v2; break;
        case 4: r = v1 < v2; break;
        case 5: r = v1 != v2; break;
        }
        break;
    }
    case 2:
        if (f->event_id > 0) {
            const char *ch = PS(1);
            r = rpg_self_sw(in->map_id, f->event_id, ch[0] - 'A') == (P(2) == 0);
        }
        break;
    case 3:
        if (rpg.timer_working) {
            int sec = rpg.timer_count / 60;
            r = P(2) == 0 ? sec >= P(1) : sec <= P(1);
        }
        break;
    case 4: {
        int id = P(1);
        if (id <= 0 || id >= RPG_MAX_ACTORS || !rpg_actor_data(id)->id)
            break;
        RpgActorState *a = rpg_actor(id);
        switch (P(2)) {
        case 0: r = rpg_party_member(id); break;
        case 1: r = !strcmp(a->name, PS(3)); break;
        case 2: r = a->class_id == P(3); break;
        case 3: r = rpg_actor_skill_learned(a, P(3)); break;
        case 4: r = rpg_actor_has_equip(a, 0, P(3)); break;
        case 5: r = rpg_actor_has_equip(a, 1, P(3)); break;
        case 6: r = rpg_battler_state(&a->b, P(3)); break;
        }
        break;
    }
    case 5: {
        RpgEnemyState *e = rpg_troop_member(P(1));
        if (e) {
            if (P(2) == 0) r = rpg_battler_alive(&e->b);
            else r = rpg_battler_state(&e->b, P(3));
        }
        break;
    }
    case 6: {
        RpgChar *ch = rpg_get_character(in, P(1));
        if (ch) r = ch->direction == P(2);
        break;
    }
    case 7:
        switch (P(2)) {
        case 0: r = rpg.party.gold >= P(1); break;
        case 1: r = rpg.party.gold <= P(1); break;
        case 2: r = rpg.party.gold < P(1); break;
        }
        break;
    case 8: r = rpg_party_has_item(0, P(1), 0); break;
    case 9: r = rpg_party_has_item(1, P(1), P(2)); break;
    case 10: r = rpg_party_has_item(2, P(1), P(2)); break;
    case 11: r = rpg_input_press_code(P(1)); break;
    case 12: r = rpg_hooks.script ? rpg_hooks.script(in, PS(1), 1) : 0; break;
    case 13: r = rpg.player.vehicle == P(1); break;
    }
    return r;
}

static int game_data_operand(RpgInterp *in, int type, int p1, int p2)
{
    switch (type) {
    case 0: return rpg_party_item_number(0, p1);
    case 1: return rpg_party_item_number(1, p1);
    case 2: return rpg_party_item_number(2, p1);
    case 3: {
        if (p1 <= 0 || p1 >= RPG_MAX_ACTORS || !rpg_actor_data(p1)->id)
            return 0;
        RpgActorState *a = rpg_actor(p1);
        switch (p2) {
        case 0: return a->level;
        case 1: return a->exp;
        case 2: return a->b.hp;
        case 3: return a->b.mp;
        default: if (p2 >= 4 && p2 <= 11) return rpg_battler_param(&a->b, p2 - 4);
        }
        return 0;
    }
    case 4: {
        RpgEnemyState *e = rpg_troop_member(p1);
        if (!e) return 0;
        if (p2 == 0) return e->b.hp;
        if (p2 == 1) return e->b.mp;
        if (p2 >= 2 && p2 <= 9) return rpg_battler_param(&e->b, p2 - 2);
        return 0;
    }
    case 5: {
        RpgChar *ch = rpg_get_character(in, p1);
        if (!ch) return 0;
        switch (p2) {
        case 0: return ch->x;
        case 1: return ch->y;
        case 2: return ch->direction;
        case 3: return rpg_char_screen_x(ch);
        case 4: return rpg_char_screen_y(ch);
        }
        return 0;
    }
    case 6: return p1 >= 0 && p1 < rpg.party.nmembers ? rpg.party.members[p1] : 0;
    case 7:
        switch (p1) {
        case 0: return rpg.map_id;
        case 1: return rpg.party.nmembers;
        case 2: return rpg.party.gold;
        case 3: return rpg.party.steps;
        case 4: return rpg.frame_count / 60;
        case 5: return rpg.timer_count / 60;
        case 6: return rpg.save_count;
        case 7: return rpg.battle_count;
        }
    }
    return 0;
}

/* iterate actors: fixed (0 = all party members, else the actor id) or by variable */
static int actor_targets(RpgList l, const RpgCmd *c, int fixed_or_var, int param, int *out)
{
    int id = fixed_or_var == 0 ? param : rpg_var(param);
    int n = 0;
    if (id == 0) {
        for (int i = 0; i < rpg.party.nmembers; i++)
            out[n++] = rpg.party.members[i];
    } else if (id > 0 && id < RPG_MAX_ACTORS && rpg_actor_data(id)->id) {
        out[n++] = id;
    }
    (void)l; (void)c;
    return n;
}

static int enemy_targets(int index, RpgEnemyState **out)
{
    int n = 0;
    if (index < 0) {
        for (int i = 0; i < rpg_troop_count(); i++)
            out[n++] = rpg_troop_member(i);
    } else if (rpg_troop_member(index)) {
        out[n++] = rpg_troop_member(index);
    }
    return n;
}

static int exec(RpgInterp *in, RpgFrame *f, RpgList l, const RpgCmd *c)
{
    int code = c->code;
    switch (code) {
    case 101:
        if (rpg_msg_busy())
            return YIELD_SAME;
        snprintf(rpg_msg.face_name, sizeof rpg_msg.face_name, "%s", PS(0));
        rpg_msg.face_index = P(1);
        rpg_msg.background = P(2);
        rpg_msg.position = P(3);
        if (rpg_hooks.message_start)
            rpg_hooks.message_start();
        while (next_code(l, f) == 401) {
            f->index++;
            rpg_msg_add(rpg_ps(l, rpg_cmd(l, f->index), 0));
        }
        switch (next_code(l, f)) {
        case 102: {
            f->index++;
            const RpgCmd *cc = rpg_cmd(l, f->index);
            rpg_msg_setup_choices(l, cc, f, cc->indent);
            break;
        }
        case 103: {
            f->index++;
            const RpgCmd *cc = rpg_cmd(l, f->index);
            rpg_msg.num_var = rpg_p(l, cc, 0);
            rpg_msg.num_digits = rpg_p(l, cc, 1);
            break;
        }
        case 104:
            f->index++;
            rpg_msg.item_var = rpg_p(l, rpg_cmd(l, f->index), 0);
            break;
        }
        in->wait_mode = RPG_W_MESSAGE;
        return NEXT;
    case 102:
        if (rpg_msg_busy())
            return YIELD_SAME;
        rpg_msg_setup_choices(l, c, f, c->indent);
        in->wait_mode = RPG_W_MESSAGE;
        return NEXT;
    case 402:
        if (f->branch[c->indent] != P(0))
            skip_branch(l, f);
        return NEXT;
    case 403:
        if (f->branch[c->indent] != 4)
            skip_branch(l, f);
        return NEXT;
    case 103:
        if (rpg_msg_busy())
            return YIELD_SAME;
        rpg_msg.num_var = P(0);
        rpg_msg.num_digits = P(1);
        in->wait_mode = RPG_W_MESSAGE;
        return NEXT;
    case 104:
        if (rpg_msg_busy())
            return YIELD_SAME;
        rpg_msg.item_var = P(0);
        in->wait_mode = RPG_W_MESSAGE;
        return NEXT;
    case 105:
        if (rpg_msg.visible)
            return YIELD_SAME;
        rpg_msg.scroll_mode = 1;
        rpg_msg.scroll_speed = P(0);
        rpg_msg.scroll_no_fast = P(1);
        while (next_code(l, f) == 405) {
            f->index++;
            rpg_msg_add(rpg_ps(l, rpg_cmd(l, f->index), 0));
        }
        in->wait_mode = RPG_W_MESSAGE;
        return NEXT;
    case 108:
        while (next_code(l, f) == 408)
            f->index++;
        return NEXT;
    case 111: {
        int r = cond_111(in, l, c, f);
        f->branch[c->indent] = r;
        if (!r)
            skip_branch(l, f);
        return NEXT;
    }
    case 411:
        if (f->branch[c->indent] > 0)
            skip_branch(l, f);
        return NEXT;
    case 112:
        return NEXT;
    case 413:
        do {
            f->index--;
        } while (f->index > 0 && rpg_cmd(l, f->index)->indent != c->indent);
        return NEXT;
    case 113:
        for (;;) {
            f->index++;
            if (f->index >= l.r.count - 1)
                return NEXT;
            const RpgCmd *n = rpg_cmd(l, f->index);
            if (n->code == 413 && n->indent < c->indent)
                return NEXT;
        }
    case 115:
        f->index = l.r.count;
        return JUMPED;
    case 117: {
        int id = P(0);
        if (id > 0 && id < rpg_count(&rpg_db, RPG_T_COMMONEVENT)) {
            f->index++;   /* continue after the call when the child is done */
            push(in, 1, id, 0, same_map(in) ? f->event_id : 0);
            return JUMPED;
        }
        return NEXT;
    }
    case 118:
        return NEXT;
    case 119: {
        const char *label = PS(0);
        for (int i = 0; i < l.r.count; i++) {
            const RpgCmd *t = rpg_cmd(l, i);
            if (t->code == 118 && !strcmp(rpg_ps(l, t, 0), label)) {
                f->index = i;
                return NEXT;
            }
        }
        return NEXT;
    }
    case 121:
        for (int i = P(0); i <= P(1); i++)
            rpg_sw_set(i, P(2) == 0);
        return NEXT;
    case 122: {
        int v = 0;
        switch (P(3)) {
        case 0: v = P(4); break;
        case 1: v = rpg_var(P(4)); break;
        case 2: v = P(4) + rpg_rand(P(5) - P(4) + 1); break;
        case 3: v = game_data_operand(in, P(4), P(5), P(6)); break;
        case 4: v = rpg_hooks.script ? rpg_hooks.script(in, PS(4), 3) : 0; break;
        }
        for (int i = P(0); i <= P(1); i++)
            rpg_operate_variable(i, P(2), v);
        return NEXT;
    }
    case 123:
        if (f->event_id > 0)
            rpg_self_sw_set(in->map_id, f->event_id, PS(0)[0] - 'A', P(1) == 0);
        return NEXT;
    case 124:
        if (P(0) == 0) {
            rpg.timer_count = P(1) * 60;
            rpg.timer_working = 1;
        } else {
            rpg.timer_working = 0;
        }
        return NEXT;
    case 125: rpg_party_gain_gold(rpg_operate_value(P(0), P(1), P(2))); return NEXT;
    case 126: rpg_party_gain_item(0, P(0), rpg_operate_value(P(1), P(2), P(3)), 0); return NEXT;
    case 127: rpg_party_gain_item(1, P(0), rpg_operate_value(P(1), P(2), P(3)), P(4)); return NEXT;
    case 128: rpg_party_gain_item(2, P(0), rpg_operate_value(P(1), P(2), P(3)), P(4)); return NEXT;
    case 129: {
        int id = P(0);
        if (id > 0 && id < RPG_MAX_ACTORS && rpg_actor_data(id)->id) {
            if (P(1) == 0) {
                if (P(2) == 1)
                    rpg_actor_setup(id);
                rpg_party_add_actor(id);
            } else {
                rpg_party_remove_actor(id);
            }
        }
        return NEXT;
    }
    case 132:
        rpg.has_battle_bgm = 1;
        snprintf(rpg.battle_bgm.name, sizeof rpg.battle_bgm.name, "%s", PS(0));
        rpg.battle_bgm.volume = P(1);
        rpg.battle_bgm.pitch = P(2);
        return NEXT;
    case 133:
        rpg.has_battle_end_me = 1;
        snprintf(rpg.battle_end_me.name, sizeof rpg.battle_end_me.name, "%s", PS(0));
        rpg.battle_end_me.volume = P(1);
        rpg.battle_end_me.pitch = P(2);
        return NEXT;
    case 134: rpg.save_disabled = P(0) == 0; return NEXT;
    case 135: rpg.menu_disabled = P(0) == 0; return NEXT;
    case 136:
        rpg.encounter_disabled = P(0) == 0;
        rpg_player_make_encounter_count();
        return NEXT;
    case 137: rpg.formation_disabled = P(0) == 0; return NEXT;
    case 138:
        for (int i = 0; i < 4; i++)
            rpg.window_tone[i] = P(i);
        return NEXT;
    case 201: {
        if (rpg.party.in_battle)
            return NEXT;
        if (rpg.player.transferring || rpg_msg.visible)
            return YIELD_SAME;
        int m = P(1), x = P(2), y = P(3);
        if (P(0) != 0) {
            m = rpg_var(m);
            x = rpg_var(x);
            y = rpg_var(y);
        }
        rpg_player_reserve_transfer(m, x, y, P(4));
        rpg_temp.fade_type = P(5);
        in->wait_mode = RPG_W_TRANSFER;
        return NEXT;
    }
    case 202: {
        int m = P(2), x = P(3), y = P(4);
        if (P(1) != 0) {
            m = rpg_var(m);
            x = rpg_var(x);
            y = rpg_var(y);
        }
        rpg_vehicle_set_location(P(0), m, x, y);
        return NEXT;
    }
    case 203: {
        RpgChar *ch = rpg_get_character(in, P(0));
        if (ch) {
            if (P(1) == 0) {
                rpg_char_moveto(ch, P(2), P(3));
            } else if (P(1) == 1) {
                rpg_char_moveto(ch, rpg_var(P(2)), rpg_var(P(3)));
            } else {
                RpgChar *o = rpg_get_character(in, P(2));
                if (o) {
                    int nx = o->x, ny = o->y;
                    rpg_char_moveto(o, ch->x, ch->y);
                    rpg_char_moveto(ch, nx, ny);
                }
            }
            if (P(4) > 0)
                rpg_char_set_direction(ch, P(4));
            if (ch == &rpg.player.ch)
                rpg_player_center(ch->x, ch->y);
        }
        return NEXT;
    }
    case 204:
        if (rpg.party.in_battle)
            return NEXT;
        if (rpg.scroll_rest > 0)
            return YIELD_SAME;
        rpg.scroll_dir = P(0);
        rpg.scroll_rest = P(1);
        rpg.scroll_speed = P(2);
        return NEXT;
    case 205: {
        if (rpg.need_refresh)
            rpg_map_refresh();
        RpgChar *ch = rpg_get_character(in, P(0));
        if (ch) {
            int route = P(1);
            rpg_char_force_route(ch, l.doc, route);
            const RpgRoute *r = (const RpgRoute *)rpg_rec(l.doc, RPG_T_ROUTE, route);
            if (r->wait) {
                in->wait_mode = RPG_W_ROUTE;
                in->wait_arg = ch == &rpg.player.ch ? -1 : ch->id;
            }
        }
        return NEXT;
    }
    case 206: rpg_vehicle_get_on_off(); return NEXT;
    case 211: rpg.player.ch.transparent = P(0) == 0; return NEXT;
    case 212: {
        RpgChar *ch = rpg_get_character(in, P(0));
        if (ch) {
            ch->animation_id = P(1);
            if (P(2)) {
                in->wait_mode = RPG_W_ANIM;
                in->wait_arg = ch == &rpg.player.ch ? -1 : ch->id;
            }
        }
        return NEXT;
    }
    case 213: {
        RpgChar *ch = rpg_get_character(in, P(0));
        if (ch) {
            ch->balloon_id = P(1);
            if (P(2)) {
                in->wait_mode = RPG_W_BALLOON;
                in->wait_arg = ch == &rpg.player.ch ? -1 : ch->id;
            }
        }
        return NEXT;
    }
    case 214:
        if (same_map(in) && f->event_id > 0) {
            RpgChar *e = rpg_map_event(f->event_id);
            if (e)
                rpg_event_erase(e);
        }
        return NEXT;
    case 216: rpg.player.followers_visible = P(0) == 0; rpg_player_refresh(); return NEXT;
    case 217: return NEXT;   /* followers gather: no followers drawn */
    case 221:
        if (rpg_msg.visible)
            return YIELD_SAME;
        screen()->fadeout_duration = 30;
        screen()->fadein_duration = 0;
        wait_frames(in, 30);
        return NEXT;
    case 222:
        if (rpg_msg.visible)
            return YIELD_SAME;
        screen()->fadein_duration = 30;
        screen()->fadeout_duration = 0;
        wait_frames(in, 30);
        return NEXT;
    case 223: {
        RpgScreen *s = screen();
        for (int i = 0; i < 4; i++)
            s->tone_target[i] = P(i);
        s->tone_duration = P(4);
        if (s->tone_duration == 0)
            memcpy(s->tone, s->tone_target, sizeof s->tone);
        if (P(5))
            wait_frames(in, P(4));
        return NEXT;
    }
    case 224: {
        RpgScreen *s = screen();
        for (int i = 0; i < 4; i++)
            s->flash_color[i] = P(i);
        s->flash_duration = P(4);
        rpg_screen_flash_start(s);
        if (P(5))
            wait_frames(in, P(4));
        return NEXT;
    }
    case 225: {
        RpgScreen *s = screen();
        s->shake_power = P(0);
        s->shake_speed = P(1);
        s->shake_duration = P(2);
        if (P(3))
            wait_frames(in, P(2));
        return NEXT;
    }
    case 230:
        wait_frames(in, P(0));
        return NEXT;
    case 231: {
        float x = P(4), y = P(5);
        if (P(3) != 0) {
            x = rpg_var(P(4));
            y = rpg_var(P(5));
        }
        int n = P(0);
        if (n >= 1 && n <= RPG_MAX_PICTURES)
            rpg_picture_show(&screen()->pictures[n], PS(1), P(2), x, y, P(6), P(7), P(8), P(9));
        return NEXT;
    }
    case 232: {
        /* params: number, nil, origin, designation, x, y, zoom x, zoom y, opacity, blend, duration, wait */
        float x = P(4), y = P(5);
        if (P(3) != 0) {
            x = rpg_var(P(4));
            y = rpg_var(P(5));
        }
        int n = P(0);
        if (n >= 1 && n <= RPG_MAX_PICTURES)
            rpg_picture_move(&screen()->pictures[n], P(2), x, y, P(6), P(7), P(8), P(9), P(10));
        if (P(11))
            wait_frames(in, P(10));
        return NEXT;
    }
    case 233: {
        int n = P(0);
        if (n >= 1 && n <= RPG_MAX_PICTURES)
            screen()->pictures[n].rotate_speed = P(1);
        return NEXT;
    }
    case 234: {
        int n = P(0);
        if (n >= 1 && n <= RPG_MAX_PICTURES) {
            RpgPicture *p = &screen()->pictures[n];
            for (int i = 0; i < 4; i++)
                p->tone_target[i] = P(1 + i);
            p->tone_duration = P(5);
            if (!p->tone_duration)
                memcpy(p->tone, p->tone_target, sizeof p->tone);
        }
        if (P(6))
            wait_frames(in, P(5));
        return NEXT;
    }
    case 235: {
        int n = P(0);
        if (n >= 1 && n <= RPG_MAX_PICTURES)
            rpg_picture_erase(&screen()->pictures[n]);
        return NEXT;
    }
    case 236: {
        if (rpg.party.in_battle)
            return NEXT;
        RpgScreen *s = screen();
        s->weather_type = P(0);
        s->weather_power_target = P(0) == 0 ? 0 : P(1);
        s->weather_duration = P(2);
        if (P(2) == 0)
            s->weather_power = s->weather_power_target;
        if (P(3))
            wait_frames(in, P(2));
        return NEXT;
    }
    case 241: rpg_bgm_play(PS(0), P(1), P(2)); return NEXT;
    case 242: rpg_bgm_fade(P(0) * 1000); return NEXT;
    case 243:
        rpg.saved_bgm = rpg_bgm_current();
        rpg.saved_bgm_pos = rpg_bgm_pos();
        return NEXT;
    case 244: rpg_bgm_play_at(rpg.saved_bgm.name, rpg.saved_bgm.volume, rpg.saved_bgm.pitch, rpg.saved_bgm_pos); return NEXT;
    case 245: rpg_bgs_play(PS(0), P(1), P(2)); return NEXT;
    case 246: rpg_bgs_fade(P(0) * 1000); return NEXT;
    case 249: rpg_me_play(PS(0), P(1), P(2)); return NEXT;
    case 250: rpg_se_play(PS(0), P(1), P(2)); return NEXT;
    case 251: rpg_se_stop(); return NEXT;
    case 261:
        if (rpg_msg.visible)
            return YIELD_SAME;
        return YIELD_NEXT;   /* movies are not part of the runtime */
    case 281: rpg.name_display = P(0) == 0; return NEXT;
    case 282: rpg_map_change_tileset(P(0)); return NEXT;
    case 283:
        snprintf(rpg.battleback1, sizeof rpg.battleback1, "%s", PS(0));
        snprintf(rpg.battleback2, sizeof rpg.battleback2, "%s", PS(1));
        return NEXT;
    case 284: rpg_map_change_parallax(PS(0), P(1), P(2), P(3), P(4)); return NEXT;
    case 285: {
        int x = P(3), y = P(4), v = 0;
        if (P(2) != 0) {
            x = rpg_var(x);
            y = rpg_var(y);
        }
        switch (P(1)) {
        case 0: v = rpg_map_terrain_tag(x, y); break;
        case 1: {
            RpgChar *list[4];
            v = rpg_map_events_xy(x, y, list, 1, 0) ? list[0]->id : 0;
            break;
        }
        case 2: case 3: case 4: v = rpg_map_tile(x, y, P(1) - 2); break;
        default: v = rpg_map_region(x, y);
        }
        rpg_var_set(P(0), v);
        return NEXT;
    }
    case 301: {
        if (rpg.party.in_battle)
            return NEXT;
        int troop = P(0) == 0 ? P(1) : P(0) == 1 ? rpg_var(P(1)) : rpg_player_encounter_troop();
        if (troop > 0 && troop < rpg_count(&rpg_db, RPG_T_TROOP)) {
            rpg_battle_setup(troop, P(2), P(3));
            rpg_temp.battle_from_event = 1;
            rpg_battle_event_branch(&f->branch[c->indent]);
            rpg_player_make_encounter_count();
            rpg_scene_call(RPG_SCENE_BATTLE);
        }
        return YIELD_NEXT;
    }
    case 601:
        if (f->branch[c->indent] != 0)
            skip_branch(l, f);
        return NEXT;
    case 602:
        if (f->branch[c->indent] != 1)
            skip_branch(l, f);
        return NEXT;
    case 603:
        if (f->branch[c->indent] != 2)
            skip_branch(l, f);
        return NEXT;
    case 302: {
        if (rpg.party.in_battle)
            return NEXT;
        int n = 0;
        rpg_temp.shop_list[n][0] = P(0);
        rpg_temp.shop_list[n][1] = P(1);
        rpg_temp.shop_list[n][2] = P(2) == 0 ? -1 : P(3);
        n++;
        rpg_temp.shop_purchase_only = P(4);
        while (next_code(l, f) == 605 && n < 256) {
            f->index++;
            const RpgCmd *g = rpg_cmd(l, f->index);
            rpg_temp.shop_list[n][0] = rpg_p(l, g, 0);
            rpg_temp.shop_list[n][1] = rpg_p(l, g, 1);
            rpg_temp.shop_list[n][2] = rpg_p(l, g, 2) == 0 ? -1 : rpg_p(l, g, 3);
            n++;
        }
        rpg_temp.shop_goods_n = n;
        rpg_scene_call(RPG_SCENE_SHOP);
        return YIELD_NEXT;
    }
    case 303:
        if (rpg.party.in_battle)
            return NEXT;
        if (P(0) > 0 && P(0) < RPG_MAX_ACTORS && rpg_actor_data(P(0))->id) {
            rpg_temp.name_actor = P(0);
            rpg_temp.name_max = P(1);
            rpg_scene_call(RPG_SCENE_NAME);
            return YIELD_NEXT;
        }
        return NEXT;
    case 311: {
        int v = rpg_operate_value(P(2), P(3), P(4)), ids[RPG_MAX_ACTORS];
        int n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++) {
            RpgActorState *a = rpg_actor(ids[i]);
            if (rpg_battler_dead(&a->b))
                continue;
            rpg_battler_change_hp(&a->b, v, P(5));
            if (rpg_battler_dead(&a->b))
                rpg_battler_collapse_effect(&a->b);
        }
        if (rpg_party_all_dead())
            rpg_scene_goto(RPG_SCENE_GAMEOVER);
        return NEXT;
    }
    case 312: {
        int v = rpg_operate_value(P(2), P(3), P(4)), ids[RPG_MAX_ACTORS];
        int n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++)
            rpg_battler_set_mp(&rpg_actor(ids[i])->b, rpg_actor(ids[i])->b.mp + v);
        return NEXT;
    }
    case 313: {
        int ids[RPG_MAX_ACTORS], n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++) {
            RpgBattler *b = &rpg_actor(ids[i])->b;
            int dead = rpg_battler_dead(b);
            if (P(2) == 0)
                rpg_battler_add_state(b, P(3));
            else
                rpg_battler_remove_state(b, P(3));
            if (rpg_battler_dead(b) && !dead)
                rpg_battler_collapse_effect(b);
        }
        return NEXT;
    }
    case 314: {
        int ids[RPG_MAX_ACTORS], n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++)
            rpg_battler_recover_all(&rpg_actor(ids[i])->b);
        return NEXT;
    }
    case 315: {
        int v = rpg_operate_value(P(2), P(3), P(4)), ids[RPG_MAX_ACTORS];
        int n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++)
            rpg_actor_change_exp(rpg_actor(ids[i]), rpg_actor(ids[i])->exp + v, P(5));
        return NEXT;
    }
    case 316: {
        int v = rpg_operate_value(P(2), P(3), P(4)), ids[RPG_MAX_ACTORS];
        int n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++)
            rpg_actor_change_level(rpg_actor(ids[i]), rpg_actor(ids[i])->level + v, P(5));
        return NEXT;
    }
    case 317: {
        int v = rpg_operate_value(P(3), P(4), P(5)), ids[RPG_MAX_ACTORS];
        int n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++)
            rpg_actor_add_param(rpg_actor(ids[i]), P(2), v);
        return NEXT;
    }
    case 318: {
        int ids[RPG_MAX_ACTORS], n = actor_targets(l, c, P(0), P(1), ids);
        for (int i = 0; i < n; i++) {
            if (P(2) == 0)
                rpg_actor_learn_skill(rpg_actor(ids[i]), P(3));
            else
                rpg_actor_forget_skill(rpg_actor(ids[i]), P(3));
        }
        return NEXT;
    }
    case 319:
        if (P(0) > 0 && P(0) < RPG_MAX_ACTORS)
            rpg_actor_change_equip_by_id(rpg_actor(P(0)), P(1), P(2));
        return NEXT;
    case 320:
        if (P(0) > 0 && P(0) < RPG_MAX_ACTORS)
            snprintf(rpg_actor(P(0))->name, RPG_NAME, "%s", PS(1));
        return NEXT;
    case 321:
        if (P(0) > 0 && P(0) < RPG_MAX_ACTORS && rpg_class_data(P(1))->id)
            rpg_actor_change_class(rpg_actor(P(0)), P(1), 0);
        return NEXT;
    case 322:
        if (P(0) > 0 && P(0) < RPG_MAX_ACTORS) {
            RpgActorState *a = rpg_actor(P(0));
            snprintf(a->character_name, RPG_NAME, "%s", PS(1));
            a->character_index = P(2);
            snprintf(a->face_name, RPG_NAME, "%s", PS(3));
            a->face_index = P(4);
        }
        rpg_player_refresh();
        return NEXT;
    case 323: return NEXT;
    case 324:
        if (P(0) > 0 && P(0) < RPG_MAX_ACTORS)
            snprintf(rpg_actor(P(0))->nickname, RPG_NAME, "%s", PS(1));
        return NEXT;
    case 331: {
        int v = rpg_operate_value(P(1), P(2), P(3));
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++) {
            if (rpg_battler_dead(&es[i]->b))
                return NEXT;   /* VX Ace returns out of the whole command here */
            rpg_battler_change_hp(&es[i]->b, v, P(4));
            if (rpg_battler_dead(&es[i]->b))
                rpg_battler_collapse_effect(&es[i]->b);
        }
        return NEXT;
    }
    case 332: {
        int v = rpg_operate_value(P(1), P(2), P(3));
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++)
            rpg_battler_set_mp(&es[i]->b, es[i]->b.mp + v);
        return NEXT;
    }
    case 333: {
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++) {
            int dead = rpg_battler_dead(&es[i]->b);
            if (P(1) == 0)
                rpg_battler_add_state(&es[i]->b, P(2));
            else
                rpg_battler_remove_state(&es[i]->b, P(2));
            if (rpg_battler_dead(&es[i]->b) && !dead)
                rpg_battler_collapse_effect(&es[i]->b);
        }
        return NEXT;
    }
    case 334: {
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++)
            rpg_battler_recover_all(&es[i]->b);
        return NEXT;
    }
    case 335: {
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++) {
            es[i]->b.hidden = 0;
            rpg_troop_make_unique_names();
        }
        return NEXT;
    }
    case 336: {
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++) {
            rpg_enemy_transform(es[i], P(1));
            rpg_troop_make_unique_names();
        }
        return NEXT;
    }
    case 337: {
        RpgEnemyState *es[32];
        int n = enemy_targets(P(0), es);
        for (int i = 0; i < n; i++)
            if (rpg_battler_alive(&es[i]->b))
                es[i]->b.animation_id = P(1);
        return NEXT;
    }
    case 339: {
        if (rpg_battle_force_action(P(0), P(1), P(2), P(3))) {
            in->wait_mode = RPG_W_FORCED_ACTION;
        }
        return NEXT;
    }
    case 340:
        rpg_battle_abort();
        return YIELD_NEXT;
    case 351:
        if (rpg.party.in_battle)
            return NEXT;
        rpg_scene_call(RPG_SCENE_MENU);
        return YIELD_NEXT;
    case 352:
        if (rpg.party.in_battle)
            return NEXT;
        rpg_scene_call(RPG_SCENE_SAVE);
        return YIELD_NEXT;
    case 353:
        rpg_scene_goto(RPG_SCENE_GAMEOVER);
        return YIELD_NEXT;
    case 354:
        rpg_scene_goto(RPG_SCENE_TITLE);
        return YIELD_NEXT;
    case 355: {
        static char script[4096];
        int n = snprintf(script, sizeof script, "%s\n", PS(0));
        while (next_code(l, f) == 655) {
            f->index++;
            if (n < (int)sizeof script - 1)
                n += snprintf(script + n, sizeof script - n, "%s\n", rpg_ps(l, rpg_cmd(l, f->index), 0));
        }
        if (rpg_hooks.script)
            rpg_hooks.script(in, script, 0);
        return NEXT;
    }
    }
    return NEXT;
}

/* ------------------------------------------------------------------ update */
void rpg_interp_update(RpgInterp *in)
{
    if (!in->depth)
        return;
    RpgInterp *saved = current;
    current = in;
    for (int guard = 0; guard < 10000; guard++) {
        if (in->wait_count > 0) {
            in->wait_count--;
            break;
        }
        if (in->wait_mode) {
            if (still_waiting(in))
                break;
            in->wait_mode = 0;
        }
        RpgFrame *f = &in->f[in->depth - 1];
        if (f->phase == 1) {   /* run: wait_for_message first */
            if (rpg_msg_busy())
                break;
            f->phase = 0;
        }
        if (f->phase == 3) {   /* the yield after the list's end has happened: the frame is done */
            in->depth--;
            if (!in->depth)
                break;
            continue;
        }
        RpgList l = rpg_frame_list(f);
        if (f->index >= l.r.count) {
            f->phase = 3;      /* Fiber.yield once after the last command */
            break;
        }
        const RpgCmd *c = rpg_cmd(l, f->index);
        int r = rpg_hooks.command ? rpg_hooks.command(in, l, c, f) : 0;
        if (!r)
            r = exec(in, f, l, c);
        else
            r--;   /* RPG_CMD_* = the internal codes + 1 */
        if (r == YIELD_SAME)
            break;
        if (r == JUMPED)
            continue;
        f->index++;
        if (r == YIELD_NEXT)
            break;
    }
    current = saved;
}
