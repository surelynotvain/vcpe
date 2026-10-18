/* VCPE RPG battlers, actors and the party: Game_BattlerBase, Game_Battler, Game_Actor, Game_Party, following VX Ace's
 * default scripts. Parameters are computed in double precision with the same order of operations as Ruby, so
 * values truncate the same way (e.g. 34 * 1.1 * 1.25 -> 46).
 *
 * A battler's features come from its states and, for actors, the actor, its class and its equipment, for enemies
 * the enemy (feature ranges in the database document). */
#include "vcpe_rpg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

_Static_assert(sizeof(RpgUsable) <= sizeof(RpgSkill) && sizeof(RpgUsable) <= sizeof(RpgItem), "usable prefix");
_Static_assert(__builtin_offsetof(RpgSkill, effects) == __builtin_offsetof(RpgUsable, effects), "skill layout");
_Static_assert(__builtin_offsetof(RpgItem, effects) == __builtin_offsetof(RpgUsable, effects), "item layout");
_Static_assert(__builtin_offsetof(RpgWeapon, params) == __builtin_offsetof(RpgEquip, params), "weapon layout");
_Static_assert(__builtin_offsetof(RpgArmor, params) == __builtin_offsetof(RpgEquip, params), "armor layout");

void (*rpg_battler_damage_hook)(RpgBattler *target, RpgBattler *user, int kind, int id, double *value);
RpgBattlerHooks rpg_bh;
RpgEnemyState *rpg_troop_member(int i);

/* ------------------------------------------------------------------ items */
const RpgUsable *rpg_usable(int kind, int id)
{
    if (kind == RPG_KIND_SKILL)
        return (const RpgUsable *)rpg_skill_data(id);
    return (const RpgUsable *)rpg_item_data(id);
}

const RpgEquip *rpg_equip(int kind, int id)
{
    if (kind == RPG_KIND_WEAPON)
        return (const RpgEquip *)rpg_weapon_data(id);
    return (const RpgEquip *)rpg_armor_data(id);
}

static const int32_t *head(int kind, int id)
{
    switch (kind) {
    case RPG_KIND_ITEM: return (const int32_t *)rpg_item_data(id);
    case RPG_KIND_WEAPON: return (const int32_t *)rpg_weapon_data(id);
    case RPG_KIND_ARMOR: return (const int32_t *)rpg_armor_data(id);
    default: return (const int32_t *)rpg_skill_data(id);
    }
}
const char *rpg_item_name(int kind, int id) { return RPG_STR(head(kind, id)[1]); }
int rpg_item_icon(int kind, int id) { return head(kind, id)[2]; }
int rpg_item_price(int kind, int id)
{
    if (kind == RPG_KIND_ITEM) return rpg_item_data(id)->price;
    if (kind == RPG_KIND_WEAPON || kind == RPG_KIND_ARMOR) return rpg_equip(kind, id)->price;
    return 0;
}

int rpg_for_opponent(const RpgUsable *u) { return u->scope >= 1 && u->scope <= 6; }
int rpg_for_friend(const RpgUsable *u) { return u->scope >= 7 && u->scope <= 11; }
int rpg_for_dead_friend(const RpgUsable *u) { return u->scope == 9 || u->scope == 10; }
int rpg_for_user(const RpgUsable *u) { return u->scope == 11; }
int rpg_for_one(const RpgUsable *u) { int s = u->scope; return s == 1 || s == 3 || s == 7 || s == 9 || s == 11; }
int rpg_for_random(const RpgUsable *u) { return u->scope >= 3 && u->scope <= 6; }
int rpg_for_all(const RpgUsable *u) { int s = u->scope; return s == 2 || s == 8 || s == 10; }
int rpg_need_selection(const RpgUsable *u) { int s = u->scope; return s == 1 || s == 7 || s == 9; }
int rpg_battle_ok(const RpgUsable *u) { return u->occasion == 0 || u->occasion == 1; }
int rpg_menu_ok(const RpgUsable *u) { return u->occasion == 0 || u->occasion == 2; }
static int dmg_none(const RpgUsable *u) { return u->damage.type == 0; }
static int dmg_to_hp(const RpgUsable *u) { int t = u->damage.type; return t == 1 || t == 3 || t == 5; }
static int dmg_to_mp(const RpgUsable *u) { int t = u->damage.type; return t == 2 || t == 4 || t == 6; }
static int dmg_recover(const RpgUsable *u) { int t = u->damage.type; return t == 3 || t == 4; }
static int dmg_drain(const RpgUsable *u) { int t = u->damage.type; return t == 5 || t == 6; }

/* ------------------------------------------------------------------ battler identity */
RpgActorState *rpg_actor(int id)
{
    if (id <= 0 || id >= RPG_MAX_ACTORS)
        id = 0;
    RpgActorState *a = &rpg.actors[id];
    if (!a->exists && id > 0 && rpg_actor_data(id)->id)
        rpg_actor_setup(id);
    return a;
}

RpgActorState *rpg_battler_actor(const RpgBattler *b) { return b->is_actor ? (RpgActorState *)b : 0; }
RpgEnemyState *rpg_battler_enemy(const RpgBattler *b) { return b->is_actor ? 0 : (RpgEnemyState *)b; }

const char *rpg_battler_name(const RpgBattler *b)
{
    if (b->is_actor)
        return ((const RpgActorState *)b)->name;
    return ((const RpgEnemyState *)b)->name;
}

/* ------------------------------------------------------------------ features */
static int ranges(const RpgBattler *b, RpgRange *r, int max)
{
    int n = 0;
    for (int i = 0; i < b->nstates && n < max; i++)
        r[n++] = rpg_state_data(b->states[i].id)->features;
    if (b->is_actor) {
        const RpgActorState *a = (const RpgActorState *)b;
        if (n < max) r[n++] = rpg_actor_data(a->actor_id)->features;
        if (n < max) r[n++] = rpg_class_data(a->class_id)->features;
        for (int s = 0; s < RPG_MAX_SLOTS && n < max; s++)
            if (a->equip_id[s] > 0)
                r[n++] = rpg_equip(a->equip_kind[s] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, a->equip_id[s])->features;
    } else {
        const RpgEnemyState *e = (const RpgEnemyState *)b;
        if (n < max) r[n++] = rpg_enemy_data(e->enemy_id)->features;
    }
    return n;
}

#define FOR_FEATURES(b, ft)                                                                         \
    RpgRange rr_[40];                                                                               \
    int nr_ = ranges((b), rr_, 40);                                                                 \
    for (int ri_ = 0; ri_ < nr_; ri_++)                                                             \
        for (int fi_ = 0; fi_ < rr_[ri_].count; fi_++)                                              \
            for (const RpgFeature *ft = rpg_rec(&rpg_db, RPG_T_FEATURE, rr_[ri_].first + fi_); ft; ft = 0)

int rpg_features_list(const RpgBattler *b, const RpgFeature **out, int max)
{
    int n = 0;
    FOR_FEATURES(b, ft) if (n < max) out[n++] = ft;
    return n;
}

double rpg_features_pi(const RpgBattler *b, int code, int id)
{
    double r = 1.0;
    FOR_FEATURES(b, ft) if (ft->code == code && ft->data_id == id) r *= ft->value;
    return r;
}

double rpg_features_sum(const RpgBattler *b, int code, int id)
{
    double r = 0.0;
    FOR_FEATURES(b, ft) if (ft->code == code && ft->data_id == id) r += ft->value;
    return r;
}

double rpg_features_sum_all(const RpgBattler *b, int code)
{
    double r = 0.0;
    FOR_FEATURES(b, ft) if (ft->code == code) r += ft->value;
    return r;
}

int rpg_features_set_has(const RpgBattler *b, int code, int id)
{
    FOR_FEATURES(b, ft) if (ft->code == code && ft->data_id == id) return 1;
    return 0;
}

int rpg_features_set_max(const RpgBattler *b, int code)
{
    int m = 0, any = 0;
    FOR_FEATURES(b, ft) if (ft->code == code) {
        if (!any || ft->data_id > m) m = ft->data_id;
        any = 1;
    }
    return m;
}

int rpg_features_any(const RpgBattler *b, int code)
{
    FOR_FEATURES(b, ft) if (ft->code == code) return 1;
    return 0;
}

int rpg_special_flag(const RpgBattler *b, int flag) { return rpg_features_set_has(b, F_SPECIAL_FLAG, flag); }
double rpg_element_rate(const RpgBattler *b, int e) { return rpg_features_pi(b, F_ELEMENT_RATE, e); }
double rpg_state_rate(const RpgBattler *b, int s) { return rpg_features_pi(b, F_STATE_RATE, s); }
int rpg_state_resist(const RpgBattler *b, int s) { return rpg_features_set_has(b, F_STATE_RESIST, s); }
double rpg_battler_atk_speed(const RpgBattler *b) { return rpg_features_sum_all(b, F_ATK_SPEED); }
int rpg_battler_atk_times_add(const RpgBattler *b)
{
    double v = rpg_features_sum_all(b, F_ATK_TIMES);
    return v > 0 ? (int)v : 0;
}

int rpg_battler_atk_elements(const RpgBattler *b, int *out, int max)
{
    int n = 0;
    FOR_FEATURES(b, ft) if (ft->code == F_ATK_ELEMENT) {
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= out[i] == ft->data_id;
        if (!dup && n < max)
            out[n++] = ft->data_id;
    }
    if (b->is_actor) {
        /* bare hands: the physical element */
        const RpgActorState *a = (const RpgActorState *)b;
        int armed = 0;
        for (int s = 0; s < RPG_MAX_SLOTS; s++)
            armed |= a->equip_id[s] > 0 && a->equip_kind[s] == 0;
        if (!armed) {
            int dup = 0;
            for (int i = 0; i < n; i++)
                dup |= out[i] == 1;
            if (!dup && n < max)
                out[n++] = 1;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ parameters */
int rpg_battler_param_base(const RpgBattler *b, int p)
{
    int v;
    if (b->is_actor) {
        const RpgActorState *a = (const RpgActorState *)b;
        const RpgClass *c = rpg_class_data(a->class_id);
        v = rpg_t16(&rpg_db, &c->params, p, a->level, 0);
    } else {
        v = rpg_enemy_data(((const RpgEnemyState *)b)->enemy_id)->params[p & 7];
    }
    return rpg_bh.param_base ? rpg_bh.param_base(b, p, v) : v;
}

int rpg_battler_param_plus(const RpgBattler *b, int p)
{
    if (b->is_actor) {
        const RpgActorState *a = (const RpgActorState *)b;
        int v = a->param_plus[p];
        for (int s = 0; s < RPG_MAX_SLOTS; s++)
            if (a->equip_id[s] > 0)
                v += rpg_equip(a->equip_kind[s] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, a->equip_id[s])->params[p];
        return v;
    }
    return 0;
}

int rpg_battler_param_min(const RpgBattler *b, int p) { (void)b; return p == 1 ? 0 : 1; }
int rpg_battler_param_max(const RpgBattler *b, int p)
{
    int v = p == 0 ? (b->is_actor ? 9999 : 999999) : p == 1 ? 9999 : 999;
    return rpg_bh.param_max ? rpg_bh.param_max(b, p, v) : v;
}

double rpg_battler_param_d(const RpgBattler *b, int p)
{
    double v = rpg_battler_param_base(b, p) + rpg_battler_param_plus(b, p);
    v *= rpg_features_pi(b, F_PARAM, p) * (b->buffs[p] * 0.25 + 1.0) * (rpg_bh.param_rate ? rpg_bh.param_rate(b, p) : 1.0);
    return v;
}

int rpg_battler_param(const RpgBattler *b, int p)
{
    if (p < 0 || p > 7)
        return 0;
    double v = rpg_battler_param_d(b, p);
    double mx = rpg_battler_param_max(b, p), mn = rpg_battler_param_min(b, p);
    v = v < mx ? v : mx;
    v = v > mn ? v : mn;
    int r = (int)v;
    if (rpg_hooks.param && b->is_actor)
        r = rpg_hooks.param((const RpgActorState *)b, p, r);
    return r;
}

double rpg_battler_xparam(const RpgBattler *b, int p) { return rpg_features_sum(b, F_XPARAM, p); }
double rpg_battler_sparam(const RpgBattler *b, int p) { return rpg_features_pi(b, F_SPARAM, p); }
int rpg_battler_mhp(const RpgBattler *b) { return rpg_battler_param(b, 0); }
int rpg_battler_mmp(const RpgBattler *b) { return rpg_battler_param(b, 1); }

/* ------------------------------------------------------------------ states */
int rpg_battler_state(const RpgBattler *b, int s)
{
    for (int i = 0; i < b->nstates; i++)
        if (b->states[i].id == s)
            return 1;
    return 0;
}

int rpg_battler_exists(const RpgBattler *b) { return !b->hidden; }
int rpg_battler_dead(const RpgBattler *b) { return rpg_battler_exists(b) && rpg_battler_state(b, 1); }
int rpg_battler_alive(const RpgBattler *b) { return rpg_battler_exists(b) && !rpg_battler_state(b, 1); }

int rpg_battler_restriction(const RpgBattler *b)
{
    int r = 0;
    for (int i = 0; i < b->nstates; i++) {
        int x = rpg_state_data(b->states[i].id)->restriction;
        if (x > r)
            r = x;
    }
    return r;
}

int rpg_battler_normal(const RpgBattler *b) { return rpg_battler_exists(b) && rpg_battler_restriction(b) == 0; }
int rpg_battler_inputable(const RpgBattler *b) { return rpg_battler_normal(b) && !rpg_special_flag(b, 0); }
int rpg_battler_movable(const RpgBattler *b) { return rpg_battler_exists(b) && rpg_battler_restriction(b) < 4; }
int rpg_battler_confusion(const RpgBattler *b)
{
    int r = rpg_battler_restriction(b);
    return rpg_battler_exists(b) && r >= 1 && r <= 3 ? r : 0;
}
int rpg_battler_guard(const RpgBattler *b) { return rpg_special_flag(b, 1) && rpg_battler_movable(b); }

void rpg_battler_erase_state(RpgBattler *b, int s)
{
    if (rpg_bh.erase_state)
        rpg_bh.erase_state(b, s);
    for (int i = 0; i < b->nstates; i++)
        if (b->states[i].id == s) {
            memmove(&b->states[i], &b->states[i + 1], (b->nstates - i - 1) * sizeof b->states[0]);
            b->nstates--;
            return;
        }
}

static void result_clear_status(RpgBattler *b)
{
    b->result.nadded = b->result.nremoved = 0;
    b->result.nadded_buffs = b->result.nadded_debuffs = b->result.nremoved_buffs = 0;
}

void rpg_battler_clear_states(RpgBattler *b)
{
    if (rpg_bh.clear_states)
        rpg_bh.clear_states(b);
    b->nstates = 0;
    result_clear_status(b);
}

static void sort_states(RpgBattler *b)
{
    /* by priority (high first), then id */
    for (int i = 1; i < b->nstates; i++) {
        RpgStateSlot s = b->states[i];
        int pi = rpg_state_data(s.id)->priority;
        int j = i - 1;
        while (j >= 0) {
            int pj = rpg_state_data(b->states[j].id)->priority;
            if (pj > pi || (pj == pi && b->states[j].id < s.id))
                break;
            b->states[j + 1] = b->states[j];
            j--;
        }
        b->states[j + 1] = s;
    }
}

static void push_unique(int *arr, int *n, int max, int v)
{
    for (int i = 0; i < *n; i++)
        if (arr[i] == v)
            return;
    if (*n < max)
        arr[(*n)++] = v;
}

static int contains(const int *arr, int n, int v)
{
    for (int i = 0; i < n; i++)
        if (arr[i] == v)
            return 1;
    return 0;
}

void rpg_actor_release_unequippable(RpgActorState *a, int item_gain);

void rpg_battler_refresh(RpgBattler *b)
{
    if (b->is_actor)
        rpg_actor_release_unequippable((RpgActorState *)b, 1);
    for (int i = b->nstates - 1; i >= 0; i--)
        if (rpg_state_resist(b, b->states[i].id))
            rpg_battler_erase_state(b, b->states[i].id);
    int mhp = rpg_battler_mhp(b), mmp = rpg_battler_mmp(b);
    b->hp = b->hp > mhp ? mhp : (b->hp < 0 ? 0 : b->hp);
    b->mp = b->mp > mmp ? mmp : (b->mp < 0 ? 0 : b->mp);
    if (b->hp == 0)
        rpg_battler_add_state(b, 1);
    else
        rpg_battler_remove_state(b, 1);
}

void rpg_battler_set_hp(RpgBattler *b, int hp)
{
    b->hp = hp;
    rpg_battler_refresh(b);
}

void rpg_battler_set_mp(RpgBattler *b, int mp)
{
    b->mp = mp;
    rpg_battler_refresh(b);
}

void rpg_battler_set_tp(RpgBattler *b, float tp) { b->tp = tp > 100 ? 100 : (tp < 0 ? 0 : tp); }

void rpg_battler_change_hp(RpgBattler *b, int v, int enable_death)
{
    if (!enable_death && b->hp + v <= 0)
        rpg_battler_set_hp(b, 1);
    else
        rpg_battler_set_hp(b, b->hp + v);
}

void rpg_battler_recover_all(RpgBattler *b)
{
    rpg_battler_clear_states(b);
    b->hp = rpg_battler_mhp(b);
    b->mp = rpg_battler_mmp(b);
}

void rpg_battler_die(RpgBattler *b)
{
    b->hp = 0;
    rpg_battler_clear_states(b);
    memset(b->buffs, 0, sizeof b->buffs);
    memset(b->buff_turns, 0, sizeof b->buff_turns);
}

void rpg_battler_revive(RpgBattler *b)
{
    if (b->hp == 0)
        b->hp = 1;
}

static void clear_actions(RpgBattler *b)
{
    b->nactions = 0;
    if (b->is_actor)
        ((RpgActorState *)b)->action_input_index = 0;
}

static void reset_state_counts(RpgBattler *b, int s)
{
    const RpgState *st = rpg_state_data(s);
    int variance = 1 + (st->max_turns - st->min_turns > 0 ? st->max_turns - st->min_turns : 0);
    for (int i = 0; i < b->nstates; i++)
        if (b->states[i].id == s) {
            b->states[i].turns = st->min_turns + rpg_rand(variance);
            b->states[i].steps = st->steps_to_remove;
        }
    if (rpg_bh.reset_state_counts)
        rpg_bh.reset_state_counts(b, s);
}

static void add_state_base(RpgBattler *b, int s);
void rpg_battler_add_state(RpgBattler *b, int s)
{
    if (rpg_bh.add_state_pre)
        rpg_bh.add_state_pre(b, s);
    add_state_base(b, s);
    if (rpg_bh.add_state_post)
        rpg_bh.add_state_post(b, s);
}

static void add_state_base(RpgBattler *b, int s)
{
    const RpgState *st = rpg_state_data(s);
    /* state_addable? */
    if (!rpg_battler_alive(b) || !st->id || rpg_state_resist(b, s) || contains(b->result.removed_states, b->result.nremoved, s))
        return;
    if (st->remove_by_restriction && rpg_battler_restriction(b) > 0)
        return;
    if (!rpg_battler_state(b, s)) {
        /* add_new_state */
        if (s == 1)
            rpg_battler_die(b);
        if (b->nstates < (int)(sizeof b->states / sizeof b->states[0]))
            b->states[b->nstates++] = (RpgStateSlot){(int16_t)s, 0, 0, 0};
        if (rpg_battler_restriction(b) > 0) {
            /* on_restrict */
            clear_actions(b);
            for (int i = b->nstates - 1; i >= 0; i--)
                if (i < b->nstates && rpg_state_data(b->states[i].id)->remove_by_restriction)
                    rpg_battler_remove_state(b, b->states[i].id);
        }
        sort_states(b);
        rpg_battler_refresh(b);
    }
    reset_state_counts(b, s);
    push_unique(b->result.added_states, &b->result.nadded, 24, s);
}

void rpg_battler_remove_state(RpgBattler *b, int s)
{
    if (!rpg_battler_state(b, s))
        return;
    if (s == 1)
        rpg_battler_revive(b);
    rpg_battler_erase_state(b, s);
    rpg_battler_refresh(b);
    push_unique(b->result.removed_states, &b->result.nremoved, 24, s);
}

int rpg_battler_state_turns(const RpgBattler *b, int s)
{
    for (int i = 0; i < b->nstates; i++)
        if (b->states[i].id == s)
            return b->states[i].turns;
    return 0;
}

void rpg_battler_set_state_turns(RpgBattler *b, int s, int t)
{
    for (int i = 0; i < b->nstates; i++)
        if (b->states[i].id == s)
            b->states[i].turns = t;
}

static void overwrite_buff_turns(RpgBattler *b, int p, int turns)
{
    if (b->buff_turns[p] < turns)
        b->buff_turns[p] = turns;
}

void rpg_battler_add_buff(RpgBattler *b, int p, int turns)
{
    if (!rpg_battler_alive(b))
        return;
    if (b->buffs[p] != 2)
        b->buffs[p]++;
    if (b->buffs[p] < 0) {
        b->buffs[p] = 0;
        b->buff_turns[p] = 0;
    }
    overwrite_buff_turns(b, p, turns);
    push_unique(b->result.added_buffs, &b->result.nadded_buffs, 8, p);
    rpg_battler_refresh(b);
}

void rpg_battler_add_debuff(RpgBattler *b, int p, int turns)
{
    if (!rpg_battler_alive(b))
        return;
    if (b->buffs[p] != -2)
        b->buffs[p]--;
    if (b->buffs[p] > 0) {
        b->buffs[p] = 0;
        b->buff_turns[p] = 0;
    }
    overwrite_buff_turns(b, p, turns);
    push_unique(b->result.added_debuffs, &b->result.nadded_debuffs, 8, p);
    rpg_battler_refresh(b);
}

void rpg_battler_remove_buff(RpgBattler *b, int p)
{
    if (!rpg_battler_alive(b) || b->buffs[p] == 0)
        return;
    b->buffs[p] = 0;
    b->buff_turns[p] = 0;
    push_unique(b->result.removed_buffs, &b->result.nremoved_buffs, 8, p);
    rpg_battler_refresh(b);
}

void rpg_battler_escape(RpgBattler *b)
{
    if (rpg.party.in_battle)
        b->hidden = 1;
    clear_actions(b);
    rpg_battler_clear_states(b);
    rpg_play_system_se(RPG_SE_ESCAPE);
}

void rpg_battler_update_state_turns(RpgBattler *b)
{
    for (int i = 0; i < b->nstates; i++)
        if (b->states[i].turns > 0)
            b->states[i].turns--;
}

void rpg_battler_update_buff_turns(RpgBattler *b)
{
    for (int p = 0; p < 8; p++)
        if (b->buff_turns[p] > 0)
            b->buff_turns[p]--;
}

void rpg_battler_remove_states_auto(RpgBattler *b, int timing)
{
    int ids[24], n = b->nstates;
    for (int i = 0; i < n; i++)
        ids[i] = b->states[i].id;
    for (int i = 0; i < n; i++) {
        const RpgState *st = rpg_state_data(ids[i]);
        if (rpg_battler_state(b, ids[i]) && rpg_battler_state_turns(b, ids[i]) == 0 && st->auto_removal_timing == timing)
            rpg_battler_remove_state(b, ids[i]);
    }
}

void rpg_battler_remove_buffs_auto(RpgBattler *b)
{
    for (int p = 0; p < 8; p++)
        if (b->buffs[p] != 0 && b->buff_turns[p] <= 0)
            rpg_battler_remove_buff(b, p);
}

static void remove_states_by_damage(RpgBattler *b)
{
    int ids[24], n = b->nstates;
    for (int i = 0; i < n; i++)
        ids[i] = b->states[i].id;
    for (int i = 0; i < n; i++) {
        const RpgState *st = rpg_state_data(ids[i]);
        if (st->remove_by_damage && rpg_rand(100) < st->chance_by_damage)
            rpg_battler_remove_state(b, ids[i]);
    }
}

void rpg_battler_result_clear(RpgBattler *b)
{
    b->result.used = b->result.missed = b->result.evaded = b->result.critical = b->result.success = 0;
    b->result.hp_damage = b->result.mp_damage = b->result.tp_damage = b->result.hp_drain = b->result.mp_drain = 0;
    result_clear_status(b);
}

int rpg_battler_result_hit(const RpgBattler *b) { return b->result.used && !b->result.missed && !b->result.evaded; }

void rpg_battler_collapse_effect(RpgBattler *b)
{
    if (b->is_actor) {
        if (rpg.party.in_battle) {
            b->sprite_effect = 2;   /* collapse */
            rpg_play_system_se(RPG_SE_ACTOR_COLLAPSE);
        }
        return;
    }
    switch (rpg_features_set_max(b, F_COLLAPSE_TYPE)) {
    case 0: b->sprite_effect = 2; rpg_play_system_se(RPG_SE_ENEMY_COLLAPSE); break;
    case 1: b->sprite_effect = 3; rpg_play_system_se(RPG_SE_BOSS_COLLAPSE1); break;
    case 2: b->sprite_effect = 4; break;
    }
}

void rpg_battler_damage_effect(RpgBattler *b)
{
    b->sprite_effect = 1;   /* blink */
    if (b->is_actor) {
        rpg_battle_screen.shake_power = 5;
        rpg_battle_screen.shake_speed = 5;
        rpg_battle_screen.shake_duration = 10;
        rpg_play_system_se(RPG_SE_ACTOR_DAMAGE);
    } else {
        rpg_play_system_se(RPG_SE_ENEMY_DAMAGE);
    }
}

/* ------------------------------------------------------------------ usability and costs */
int rpg_battler_attack_skill(const RpgBattler *b) { return rpg_bh.attack_skill ? rpg_bh.attack_skill(b, 1) : 1; }
int rpg_battler_guard_skill(const RpgBattler *b) { (void)b; return 2; }

int rpg_battler_skill_mp_cost(const RpgBattler *b, int skill)
{
    return (int)(rpg_skill_data(skill)->mp_cost * rpg_battler_sparam(b, 4));
}
int rpg_battler_skill_tp_cost(const RpgBattler *b, int skill) { (void)b; return rpg_skill_data(skill)->tp_cost; }

static int skill_wtype_ok(const RpgBattler *b, const RpgSkill *s)
{
    if (!b->is_actor)
        return 1;
    const RpgActorState *a = (const RpgActorState *)b;
    int w1 = s->required_wtype_id1, w2 = s->required_wtype_id2;
    if (w1 == 0 && w2 == 0)
        return 1;
    if (w1 > 0 && rpg_actor_wtype_equipped(a, w1))
        return 1;
    if (w2 > 0 && rpg_actor_wtype_equipped(a, w2))
        return 1;
    return 0;
}

int rpg_battler_usable(const RpgBattler *b, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    if (!u->id)
        return 0;
    if (rpg_bh.usable_extra && !rpg_bh.usable_extra(b, kind, id))
        return 0;
    int occ = rpg.party.in_battle ? rpg_battle_ok(u) : rpg_menu_ok(u);
    if (!rpg_battler_movable(b) || !occ)
        return 0;
    if (kind == RPG_KIND_SKILL) {
        const RpgSkill *s = rpg_skill_data(id);
        if (!skill_wtype_ok(b, s))
            return 0;
        if (b->tp < rpg_battler_skill_tp_cost(b, id) || b->mp < rpg_battler_skill_mp_cost(b, id))
            return 0;
        if (rpg_features_set_has(b, F_SKILL_SEAL, id) || rpg_features_set_has(b, F_STYPE_SEAL, s->stype_id))
            return 0;
        return 1;
    }
    return rpg_party_has_item(RPG_KIND_ITEM, id, 0);
}

void rpg_battler_use_item(RpgBattler *b, int kind, int id)
{
    if (rpg_bh.use_item_pre)
        rpg_bh.use_item_pre(b, kind, id);
    if (kind == RPG_KIND_SKILL) {
        rpg_battler_set_mp(b, b->mp - rpg_battler_skill_mp_cost(b, id));
        rpg_battler_set_tp(b, b->tp - rpg_battler_skill_tp_cost(b, id));
    } else {
        rpg_party_consume_item(RPG_KIND_ITEM, id);
    }
    const RpgUsable *u = rpg_usable(kind, id);
    for (int i = 0; i < u->effects.count; i++) {
        const RpgEffect *e = rpg_at(&rpg_db, RPG_T_EFFECT, u->effects, i);
        if (e->code == 44)
            rpg.reserved_common = e->data_id;
    }
}

/* ------------------------------------------------------------------ damage */
double rpg_damage_formula(int kind, int id, const RpgBattler *a, const RpgBattler *b)
{
    if (rpg_hooks.formula)
        return rpg_hooks.formula(kind, id, a, b);
    return 0;
}

static double luk_effect_rate(const RpgBattler *target, const RpgBattler *user)
{
    double r = 1.0 + (rpg_battler_param(user, 7) - rpg_battler_param(target, 7)) * 0.001;
    return r > 0 ? r : 0;
}

static int opposite(const RpgBattler *a, const RpgBattler *b) { return a->is_actor != b->is_actor; }

void rpg_battler_make_damage_value(RpgBattler *t, RpgBattler *user, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    /* RPG::UsableItem::Damage#eval: [formula, 0].max * sign (the hook returns the formula's value) */
    double v = rpg_damage_formula(kind, id, user, t);
    if (v < 0)
        v = 0;
    if (dmg_recover(u))
        v = -v;
    /* element rate */
    if (u->damage.element_id < 0) {
        int el[16], n = rpg_battler_atk_elements(user, el, 16);
        if (n) {
            double m = 0.0;
            for (int i = 0; i < n; i++) {
                double r = rpg_element_rate(t, el[i]);
                if (r > m) m = r;
            }
            v *= m;
        }
    } else {
        v *= rpg_element_rate(t, u->damage.element_id);
    }
    if (u->hit_type == 1) v *= rpg_battler_sparam(t, 6);   /* pdr */
    if (u->hit_type == 2) v *= rpg_battler_sparam(t, 7);   /* mdr */
    if (dmg_recover(u)) v *= rpg_battler_sparam(t, 2);     /* rec */
    if (t->result.critical) v *= rpg_bh.critical_rate ? rpg_bh.critical_rate(t) : 3;
    /* variance */
    int amp = (int)(fabs(v) * u->damage.variance / 100);
    if (amp < 0) amp = 0;
    int var = rpg_rand(amp + 1) + rpg_rand(amp + 1) - amp;
    v = v >= 0 ? v + var : v - var;
    /* guard */
    if (v > 0 && rpg_battler_guard(t))
        v /= 2 * rpg_battler_sparam(t, 1);
    if (rpg_battler_damage_hook)
        rpg_battler_damage_hook(t, user, kind, id, &v);
    int value = (int)v;
    /* Game_ActionResult#make_damage */
    if (value == 0)
        t->result.critical = 0;
    if (dmg_to_hp(u))
        t->result.hp_damage = value;
    if (dmg_to_mp(u))
        t->result.mp_damage = value;
    if (t->result.mp_damage > t->mp)
        t->result.mp_damage = t->mp;
    if (dmg_drain(u)) {
        t->result.hp_drain = t->result.hp_damage;
        t->result.mp_drain = t->result.mp_damage;
    }
    if (t->result.hp_drain > t->hp)
        t->result.hp_drain = t->hp;
    if (dmg_to_hp(u) || t->result.mp_damage != 0)
        t->result.success = 1;
    if (rpg_bh.make_damage_post)
        rpg_bh.make_damage_post(t, user, kind, id);
}

static void execute_damage(RpgBattler *t, RpgBattler *user)
{
    if (rpg_bh.execute_damage_pre)
        rpg_bh.execute_damage_pre(t, user);
    if (t->result.hp_damage > 0) {
        /* on_damage */
        remove_states_by_damage(t);
        rpg_battler_set_tp(t, t->tp + 50.0 * ((double)t->result.hp_damage / rpg_battler_mhp(t)) * rpg_battler_sparam(t, 5));
    }
    rpg_battler_set_hp(t, t->hp - t->result.hp_damage);
    rpg_battler_set_mp(t, t->mp - t->result.mp_damage);
    rpg_battler_set_hp(user, user->hp + t->result.hp_drain);
    rpg_battler_set_mp(user, user->mp + t->result.mp_drain);
}

static int effect_test(RpgBattler *t, RpgBattler *user, const RpgEffect *e)
{
    (void)user;
    switch (e->code) {
    case 11: return t->hp < rpg_battler_mhp(t) || e->value1 < 0 || e->value2 < 0;
    case 12: return t->mp < rpg_battler_mmp(t) || e->value1 < 0 || e->value2 < 0;
    case 21: return !rpg_battler_state(t, e->data_id);
    case 22: return rpg_battler_state(t, e->data_id);
    case 31: return t->buffs[e->data_id & 7] != 2;
    case 32: return t->buffs[e->data_id & 7] != -2;
    case 33: return t->buffs[e->data_id & 7] > 0;
    case 34: return t->buffs[e->data_id & 7] < 0;
    case 43: return t->is_actor && !rpg_actor_skill_learned((RpgActorState *)t, e->data_id);
    }
    return 1;
}

int rpg_battler_item_test(RpgBattler *t, RpgBattler *user, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    if (rpg_for_dead_friend(u) != rpg_battler_dead(t))
        return 0;
    if (rpg.party.in_battle || rpg_for_opponent(u))
        return 1;
    if (dmg_recover(u) && dmg_to_hp(u) && t->hp < rpg_battler_mhp(t))
        return 1;
    if (dmg_recover(u) && dmg_to_mp(u) && t->mp < rpg_battler_mmp(t))
        return 1;
    for (int i = 0; i < u->effects.count; i++)
        if (effect_test(t, user, rpg_at(&rpg_db, RPG_T_EFFECT, u->effects, i)))
            return 1;
    return 0;
}

static void effect_apply(RpgBattler *t, RpgBattler *user, int kind, const RpgEffect *e)
{
    switch (e->code) {
    case 11: {
        double v = (rpg_battler_mhp(t) * (double)e->value1 + e->value2) * rpg_battler_sparam(t, 2);
        if (kind == RPG_KIND_ITEM)
            v *= rpg_battler_sparam(user, 3);
        int iv = (int)v;
        t->result.hp_damage -= iv;
        t->result.success = 1;
        rpg_battler_set_hp(t, t->hp + iv);
        break;
    }
    case 12: {
        double v = (rpg_battler_mmp(t) * (double)e->value1 + e->value2) * rpg_battler_sparam(t, 2);
        if (kind == RPG_KIND_ITEM)
            v *= rpg_battler_sparam(user, 3);
        int iv = (int)v;
        t->result.mp_damage -= iv;
        if (iv != 0)
            t->result.success = 1;
        rpg_battler_set_mp(t, t->mp + iv);
        break;
    }
    case 13: {
        int iv = (int)e->value1;
        t->result.tp_damage -= iv;
        if (iv != 0)
            t->result.success = 1;
        rpg_battler_set_tp(t, t->tp + iv);
        break;
    }
    case 21:
        if (e->data_id == 0) {
            /* the attacker's attack states */
            const RpgFeature *fs[256];
            int n = rpg_features_list(user, fs, 256), done[64], nd = 0;
            for (int i = 0; i < n; i++) {
                if (fs[i]->code != F_ATK_STATE || contains(done, nd, fs[i]->data_id))
                    continue;
                int s = fs[i]->data_id;
                push_unique(done, &nd, 64, s);
                double chance = e->value1;
                chance *= rpg_state_rate(t, s);
                chance *= rpg_features_sum(user, F_ATK_STATE, s);
                chance *= luk_effect_rate(t, user);
                if (rpg_randf() < chance) {
                    rpg_battler_add_state(t, s);
                    t->result.success = 1;
                }
            }
        } else {
            double chance = e->value1;
            if (opposite(t, user)) {
                chance *= rpg_state_rate(t, e->data_id);
                chance *= luk_effect_rate(t, user);
            }
            if (rpg_randf() < chance) {
                rpg_battler_add_state(t, e->data_id);
                t->result.success = 1;
            }
        }
        break;
    case 22:
        if (rpg_randf() < e->value1) {
            rpg_battler_remove_state(t, e->data_id);
            t->result.success = 1;
        }
        break;
    case 31:
        rpg_battler_add_buff(t, e->data_id & 7, (int)e->value1);
        t->result.success = 1;
        break;
    case 32: {
        double chance = rpg_features_pi(t, F_DEBUFF_RATE, e->data_id) * luk_effect_rate(t, user);
        if (rpg_randf() < chance) {
            rpg_battler_add_debuff(t, e->data_id & 7, (int)e->value1);
            t->result.success = 1;
        }
        break;
    }
    case 33:
        if (t->buffs[e->data_id & 7] > 0)
            rpg_battler_remove_buff(t, e->data_id & 7);
        t->result.success = 1;
        break;
    case 34:
        if (t->buffs[e->data_id & 7] < 0)
            rpg_battler_remove_buff(t, e->data_id & 7);
        t->result.success = 1;
        break;
    case 41:
        if (e->data_id == 0)
            rpg_battler_escape(t);
        t->result.success = 1;
        break;
    case 42:
        if (t->is_actor)
            rpg_actor_add_param((RpgActorState *)t, e->data_id & 7, (int)e->value1);
        t->result.success = 1;
        break;
    case 43:
        if (t->is_actor)
            rpg_actor_learn_skill((RpgActorState *)t, e->data_id);
        t->result.success = 1;
        break;
    }
}

void (*rpg_item_effect_hook)(RpgBattler *t, RpgBattler *user, int kind, int id, const RpgEffect *e);

void rpg_battler_item_apply(RpgBattler *t, RpgBattler *user, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    rpg_battler_result_clear(t);
    t->result.used = rpg_battler_item_test(t, user, kind, id);
    /* item_hit / item_eva */
    double hit = u->success_rate * 0.01;
    if (u->hit_type == 1)
        hit *= rpg_battler_xparam(user, 0);
    t->result.missed = t->result.used && rpg_randf() >= hit;
    double eva = u->hit_type == 1 ? rpg_battler_xparam(t, 1) : u->hit_type == 2 ? rpg_battler_xparam(t, 4) : 0;
    t->result.evaded = !t->result.missed && rpg_randf() < eva;
    if (rpg_battler_result_hit(t)) {
        if (!dmg_none(u)) {
            double cri = u->damage.critical ? rpg_battler_xparam(user, 2) * (1 - rpg_battler_xparam(t, 3)) : 0;
            t->result.critical = rpg_randf() < cri;
            rpg_battler_make_damage_value(t, user, kind, id);
            execute_damage(t, user);
        }
        for (int i = 0; i < u->effects.count; i++) {
            RpgEffect ec = *(const RpgEffect *)rpg_at(&rpg_db, RPG_T_EFFECT, u->effects, i);
            const RpgEffect *e = &ec;
            if (rpg_bh.effect_pre && !rpg_bh.effect_pre(t, user, kind, id, &ec))
                continue;
            effect_apply(t, user, kind, e);
            if (rpg_item_effect_hook)
                rpg_item_effect_hook(t, user, kind, id, e);
        }
        /* item_user_effect */
        rpg_battler_set_tp(user, user->tp + u->tp_gain * rpg_battler_sparam(user, 5));
    }
}

/* ------------------------------------------------------------------ turns */
void rpg_battler_regenerate_all(RpgBattler *b)
{
    if (!rpg_battler_alive(b))
        return;
    if (rpg_bh.regenerate_hp_pre)
        rpg_bh.regenerate_hp_pre(b);
    int dmg = -(int)(rpg_battler_mhp(b) * rpg_battler_xparam(b, 7));
    int max_slip = rpg_system()->opt_slip_death ? b->hp : (b->hp - 1 > 0 ? b->hp - 1 : 0);
    b->result.hp_damage = dmg < max_slip ? dmg : max_slip;
    rpg_battler_set_hp(b, b->hp - b->result.hp_damage);
    if (rpg_bh.regenerate_hp_post)
        rpg_bh.regenerate_hp_post(b);
    b->result.mp_damage = -(int)(rpg_battler_mmp(b) * rpg_battler_xparam(b, 8));
    rpg_battler_set_mp(b, b->mp - b->result.mp_damage);
    rpg_battler_set_tp(b, b->tp + 100 * rpg_battler_xparam(b, 9));
}

void rpg_battler_on_battle_start(RpgBattler *b)
{
    if (!rpg_special_flag(b, 3))
        rpg_battler_set_tp(b, rpg_randf() * 25);
    if (rpg_bh.on_battle_start)
        rpg_bh.on_battle_start(b);
}

void rpg_battler_on_action_end(RpgBattler *b)
{
    rpg_battler_result_clear(b);
    rpg_battler_remove_states_auto(b, 1);
    rpg_battler_remove_buffs_auto(b);
}

void rpg_battler_on_turn_end(RpgBattler *b)
{
    rpg_battler_result_clear(b);
    rpg_battler_regenerate_all(b);
    rpg_battler_update_state_turns(b);
    rpg_battler_update_buff_turns(b);
    rpg_battler_remove_states_auto(b, 2);
}

void rpg_battler_on_battle_end(RpgBattler *b)
{
    if (rpg_bh.on_battle_end)
        rpg_bh.on_battle_end(b);
    rpg_battler_result_clear(b);
    int ids[24], n = b->nstates;
    for (int i = 0; i < n; i++)
        ids[i] = b->states[i].id;
    for (int i = 0; i < n; i++)
        if (rpg_state_data(ids[i])->remove_at_battle_end)
            rpg_battler_remove_state(b, ids[i]);
    if (rpg_bh.remove_battle_states_post)
        rpg_bh.remove_battle_states_post(b);
    for (int p = 0; p < 8; p++)
        rpg_battler_remove_buff(b, p);
    clear_actions(b);
    if (!rpg_special_flag(b, 3))
        b->tp = 0;
    b->hidden = 0;
}

/* ------------------------------------------------------------------ Game_Actor */
static int exp_formula(const RpgClass *c, int level)
{
    double lv = level, basis = c->exp_params[0], extra = c->exp_params[1], acc_a = c->exp_params[2],
           acc_b = c->exp_params[3];
    double v = basis * pow(lv - 1, 0.9 + acc_a / 250) * lv * (lv + 1) / (6 + lv * lv / 50 / acc_b) + (lv - 1) * extra;
    return (int)floor(v + 0.5);
}

int rpg_actor_exp_for_level(const RpgActorState *a, int level)
{
    int v = exp_formula(rpg_class_data(a->class_id), level);
    if (rpg_hooks.exp_for_level)
        v = rpg_hooks.exp_for_level(a, level, v);
    return v;
}

int rpg_actor_max_level(const RpgActorState *a)
{
    int v = rpg_actor_data(a->actor_id)->max_level;
    if (rpg_hooks.max_level)
        v = rpg_hooks.max_level(a, v);
    return v;
}

int rpg_actor_current_level_exp(const RpgActorState *a) { return rpg_actor_exp_for_level(a, a->level); }
int rpg_actor_next_level_exp(const RpgActorState *a) { return rpg_actor_exp_for_level(a, a->level + 1); }

int rpg_actor_skill_learned(const RpgActorState *a, int s)
{
    return s > 0 && s < RPG_MAX_SKILLS && (a->skills[s >> 3] & (1 << (s & 7)));
}

void rpg_actor_learn_skill(RpgActorState *a, int s)
{
    if (s > 0 && s < RPG_MAX_SKILLS)
        a->skills[s >> 3] |= 1 << (s & 7);
}

void rpg_actor_forget_skill(RpgActorState *a, int s)
{
    if (s > 0 && s < RPG_MAX_SKILLS)
        a->skills[s >> 3] &= ~(1 << (s & 7));
}

int rpg_actor_skills(const RpgActorState *a, int *out, int max)
{
    int n = 0;
    int ns = rpg_count(&rpg_db, RPG_T_SKILL);
    for (int s = 1; s < ns && s < RPG_MAX_SKILLS && n < max; s++)
        if (rpg_actor_skill_learned(a, s) || rpg_features_set_has(&a->b, F_SKILL_ADD, s))
            out[n++] = s;
    return n;
}

int rpg_actor_dual_wield(const RpgActorState *a) { return rpg_features_set_max(&a->b, F_SLOT_TYPE) == 1; }

int rpg_actor_slots(const RpgActorState *a, int *etypes)
{
    if (rpg_hooks.equip_slots) {
        int n = rpg_hooks.equip_slots(a, etypes, RPG_MAX_SLOTS);
        if (n > 0)
            return n;
    }
    static const int normal[5] = {0, 1, 2, 3, 4}, dual[5] = {0, 0, 2, 3, 4};
    memcpy(etypes, rpg_actor_dual_wield(a) ? dual : normal, sizeof normal);
    return 5;
}

static int equip_etype(int kind, int id)
{
    if (id <= 0)
        return -1;
    return rpg_equip(kind ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, id)->etype_id;
}

int rpg_actor_equippable(const RpgActorState *a, int kind, int id)
{
    if (id <= 0)
        return 0;
    const RpgEquip *e = rpg_equip(kind ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, id);
    if (!e->id)
        return 0;
    if (rpg_features_set_has(&a->b, F_EQUIP_SEAL, e->etype_id))
        return 0;
    if (kind == 0)
        return rpg_features_set_has(&a->b, F_EQUIP_WTYPE, rpg_weapon_data(id)->wtype_id);
    return rpg_features_set_has(&a->b, F_EQUIP_ATYPE, rpg_armor_data(id)->atype_id);
}

void rpg_actor_release_unequippable(RpgActorState *a, int item_gain)
{
    int et[RPG_MAX_SLOTS], n = rpg_actor_slots(a, et);
    /* releasing changes the features (and so what is equippable): repeat until stable, as each refresh does */
    for (int s = 0; s < RPG_MAX_SLOTS; s++) {
        if (a->equip_id[s] <= 0)
            continue;
        int bad = s >= n || !rpg_actor_equippable(a, a->equip_kind[s], a->equip_id[s]) ||
                  equip_etype(a->equip_kind[s], a->equip_id[s]) != et[s];
        if (bad) {
            if (item_gain)
                rpg_party_gain_item(a->equip_kind[s] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, a->equip_id[s], 1, 0);
            a->equip_id[s] = 0;
        }
    }
}

int rpg_actor_equip_change_ok(const RpgActorState *a, int slot)
{
    int et[RPG_MAX_SLOTS];
    rpg_actor_slots(a, et);
    return !rpg_features_set_has(&a->b, F_EQUIP_FIX, et[slot]) && !rpg_features_set_has(&a->b, F_EQUIP_SEAL, et[slot]);
}

static int trade(int new_kind, int new_id, int old_kind, int old_id)
{
    if (new_id > 0 && !rpg_party_has_item(new_kind, new_id, 0))
        return 0;
    if (old_id > 0)
        rpg_party_gain_item(old_kind, old_id, 1, 0);
    if (new_id > 0)
        rpg_party_lose_item(new_kind, new_id, 1, 0);
    return 1;
}

int rpg_actor_change_equip(RpgActorState *a, int slot, int kind, int id)
{
    /* kind: RPG_KIND_WEAPON / RPG_KIND_ARMOR; id 0 = remove */
    int et[RPG_MAX_SLOTS], n = rpg_actor_slots(a, et);
    if (slot < 0 || slot >= n)
        return 0;
    int ok = a->equip_kind[slot] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON;
    if (!trade(kind, id, ok, a->equip_id[slot]))
        return 0;
    if (id > 0 && et[slot] != equip_etype(kind == RPG_KIND_ARMOR, id))
        return 0;
    a->equip_kind[slot] = kind == RPG_KIND_ARMOR;
    a->equip_id[slot] = id;
    rpg_battler_refresh(&a->b);
    return 1;
}

void rpg_actor_force_change_equip(RpgActorState *a, int slot, int kind, int id)
{
    a->equip_kind[slot] = kind == RPG_KIND_ARMOR;
    a->equip_id[slot] = id;
    rpg_actor_release_unequippable(a, 0);
    rpg_battler_refresh(&a->b);
}

void rpg_actor_change_equip_by_id(RpgActorState *a, int slot, int id)
{
    int et[RPG_MAX_SLOTS];
    rpg_actor_slots(a, et);
    rpg_actor_change_equip(a, slot, et[slot] == 0 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR, id);
}

void rpg_actor_discard_equip(RpgActorState *a, int kind, int id)
{
    for (int s = 0; s < RPG_MAX_SLOTS; s++)
        if (a->equip_id[s] == id && a->equip_kind[s] == (kind == RPG_KIND_ARMOR)) {
            a->equip_id[s] = 0;
            return;
        }
}

int rpg_actor_has_equip(const RpgActorState *a, int is_armor, int id)
{
    for (int s = 0; s < RPG_MAX_SLOTS; s++)
        if (a->equip_id[s] == id && a->equip_kind[s] == is_armor && id > 0)
            return 1;
    return 0;
}

int rpg_actor_wtype_equipped(const RpgActorState *a, int wtype)
{
    for (int s = 0; s < RPG_MAX_SLOTS; s++)
        if (a->equip_id[s] > 0 && a->equip_kind[s] == 0 && rpg_weapon_data(a->equip_id[s])->wtype_id == wtype)
            return 1;
    return 0;
}

void rpg_actor_clear_equipments(RpgActorState *a)
{
    int et[RPG_MAX_SLOTS], n = rpg_actor_slots(a, et);
    for (int s = 0; s < n; s++)
        if (rpg_actor_equip_change_ok(a, s))
            rpg_actor_change_equip(a, s, et[s] == 0 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR, 0);
}

static int performance(int kind, int id)
{
    const RpgEquip *e = rpg_equip(kind, id);
    int p = 0;
    for (int i = 0; i < 8; i++)
        p += e->params[i];
    return p;
}

void rpg_actor_optimize_equipments(RpgActorState *a)
{
    rpg_actor_clear_equipments(a);
    int et[RPG_MAX_SLOTS], n = rpg_actor_slots(a, et);
    for (int s = 0; s < n; s++) {
        if (!rpg_actor_equip_change_ok(a, s))
            continue;
        int kind = et[s] == 0 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR, best = 0, bestp = -1;
        int count = rpg_count(&rpg_db, kind == RPG_KIND_WEAPON ? RPG_T_WEAPON : RPG_T_ARMOR);
        for (int id = 1; id < count; id++) {
            if (!rpg_party_has_item(kind, id, 0) || rpg_equip(kind, id)->etype_id != et[s] ||
                !rpg_actor_equippable(a, kind == RPG_KIND_ARMOR, id))
                continue;
            int p = performance(kind, id);
            if (p >= 0 && p > bestp) {   /* max_by keeps the first maximum */
                bestp = p;
                best = id;
            }
        }
        rpg_actor_change_equip(a, s, kind, best);
    }
}

int rpg_actor_atk_animation(const RpgActorState *a, int which)
{
    int w[2] = {0, 0}, n = 0;
    for (int s = 0; s < RPG_MAX_SLOTS && n < 2; s++)
        if (a->equip_id[s] > 0 && a->equip_kind[s] == 0)
            w[n++] = a->equip_id[s];
    if (rpg_actor_dual_wield(a)) {
        if (which == 0)
            return w[0] ? rpg_weapon_data(w[0])->animation_id : (w[1] ? 0 : 1);
        return w[1] ? rpg_weapon_data(w[1])->animation_id : 0;
    }
    if (which == 0)
        return w[0] ? rpg_weapon_data(w[0])->animation_id : 1;
    return 0;
}

void rpg_actor_add_param(RpgActorState *a, int p, int v)
{
    a->param_plus[p & 7] += v;
    rpg_battler_refresh(&a->b);
}

static void level_up(RpgActorState *a)
{
    a->level++;
    const RpgClass *c = rpg_class_data(a->class_id);
    for (int i = 0; i < c->learnings.count; i++) {
        const RpgLearning *l = rpg_at(&rpg_db, RPG_T_LEARNING, c->learnings, i);
        if ((rpg_bh.learning_level ? rpg_bh.learning_level(l) : l->level) == a->level)
            rpg_actor_learn_skill(a, l->skill_id);
    }
}

void (*rpg_level_up_message)(RpgActorState *a, const int *new_skills, int n);

void rpg_actor_change_exp(RpgActorState *a, int exp, int show)
{
    a->exp = exp > 0 ? exp : 0;
    int last = a->level, before[RPG_MAX_SKILLS / 4], nb = rpg_actor_skills(a, before, RPG_MAX_SKILLS / 4);
    while (a->level < rpg_actor_max_level(a) && a->exp >= rpg_actor_next_level_exp(a))
        level_up(a);
    while (a->level > 1 && a->exp < rpg_actor_current_level_exp(a))
        a->level--;
    if (show && a->level > last) {
        int now[RPG_MAX_SKILLS / 4], nn = rpg_actor_skills(a, now, RPG_MAX_SKILLS / 4), news[64], nw = 0;
        for (int i = 0; i < nn; i++)
            if (!contains(before, nb, now[i]) && nw < 64)
                news[nw++] = now[i];
        if (rpg_level_up_message)
            rpg_level_up_message(a, news, nw);
    }
    rpg_battler_refresh(&a->b);
}

void rpg_actor_gain_exp(RpgActorState *a, int exp)
{
    double rate = rpg_battler_sparam(&a->b, 9) * (rpg_actor_battle_member(a) ? 1 : (rpg_system()->opt_extra_exp ? 1 : 0));
    rpg_actor_change_exp(a, a->exp + (int)(exp * rate), 1);
}

void rpg_actor_change_level(RpgActorState *a, int level, int show)
{
    int mx = rpg_actor_max_level(a);
    level = level > mx ? mx : (level < 1 ? 1 : level);
    rpg_actor_change_exp(a, rpg_actor_exp_for_level(a, level), show);
}

void rpg_actor_change_class(RpgActorState *a, int class_id, int keep_exp)
{
    /* VX Ace keeps an exp per class; this keeps the current one when asked, else starts the class at its level 1
     * exp (the per-class table is rarely relied on) */
    a->class_id = class_id;
    rpg_actor_change_exp(a, keep_exp ? a->exp : 0, 0);
    rpg_battler_refresh(&a->b);
}

void rpg_actor_setup(int id)
{
    RpgActorState *a = &rpg.actors[id];
    const RpgActor *d = rpg_actor_data(id);
    memset(a, 0, sizeof *a);
    a->exists = 1;
    a->b.is_actor = 1;
    a->b.index = id;
    a->actor_id = id;
    snprintf(a->name, sizeof a->name, "%s", RPG_STR(d->name));
    snprintf(a->nickname, sizeof a->nickname, "%s", RPG_STR(d->nickname));
    snprintf(a->character_name, sizeof a->character_name, "%s", RPG_STR(d->character_name));
    a->character_index = d->character_index;
    snprintf(a->face_name, sizeof a->face_name, "%s", RPG_STR(d->face_name));
    a->face_index = d->face_index;
    a->class_id = d->class_id;
    a->level = d->initial_level;
    a->exp = rpg_actor_exp_for_level(a, a->level);
    const RpgClass *c = rpg_class_data(a->class_id);
    for (int i = 0; i < c->learnings.count; i++) {
        const RpgLearning *l = rpg_at(&rpg_db, RPG_T_LEARNING, c->learnings, i);
        if ((rpg_bh.learning_level ? rpg_bh.learning_level(l) : l->level) <= a->level)
            rpg_actor_learn_skill(a, l->skill_id);
    }
    /* init_equips: initial equipment by etype into the first empty slot of that type */
    int et[RPG_MAX_SLOTS], n = rpg_actor_slots(a, et);
    for (int i = 0; i < 5; i++) {
        int item = d->equips[i];
        int etype = (i == 1 && rpg_actor_dual_wield(a)) ? 0 : i;
        int slot = -1, first = -1;
        for (int s = 0; s < n; s++)
            if (et[s] == etype) {
                if (first < 0) first = s;
                if (a->equip_id[s] == 0 && slot < 0) slot = s;
            }
        if (slot < 0)
            slot = first;
        if (slot >= 0) {
            a->equip_kind[slot] = etype != 0;
            a->equip_id[slot] = item;
        }
    }
    rpg_battler_refresh(&a->b);
    rpg_battler_recover_all(&a->b);
}

int rpg_actor_battle_member(const RpgActorState *a)
{
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
    for (int i = 0; i < n; i++)
        if (ids[i] == a->actor_id)
            return 1;
    return 0;
}

int rpg_actor_index(const RpgActorState *a)
{
    int ids[RPG_MAX_PARTY], n = rpg.party.in_battle ? rpg_party_battle_members(ids) : rpg.party.nmembers;
    if (!rpg.party.in_battle)
        memcpy(ids, rpg.party.members, sizeof(int) * n);
    for (int i = 0; i < n; i++)
        if (ids[i] == a->actor_id)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ Game_Party */
int rpg_party_member(int id)
{
    for (int i = 0; i < rpg.party.nmembers; i++)
        if (rpg.party.members[i] == id)
            return 1;
    return 0;
}

int rpg_party_battle_members(int *out)
{
    int n = 0, mx = rpg.party.max_battle_members ? rpg.party.max_battle_members : 4;
    for (int i = 0; i < rpg.party.nmembers && i < mx; i++)
        if (rpg_battler_exists(&rpg_actor(rpg.party.members[i])->b))
            out[n++] = rpg.party.members[i];
    return n;
}

/* members: battle members in battle, else all */
static int members(int *out)
{
    if (rpg.party.in_battle)
        return rpg_party_battle_members(out);
    memcpy(out, rpg.party.members, sizeof(int) * rpg.party.nmembers);
    return rpg.party.nmembers;
}

RpgActorState *rpg_party_leader(void)
{
    int ids[RPG_MAX_PARTY];
    return rpg_party_battle_members(ids) ? rpg_actor(ids[0]) : 0;
}

const char *rpg_party_leader_graphic(int *index)
{
    RpgActorState *a = rpg_party_leader();
    if (!a) {
        *index = 0;
        return "";
    }
    *index = a->character_index;
    return a->character_name;
}

static int16_t *container(int kind)
{
    if (kind == RPG_KIND_ITEM) return rpg.party.items;
    if (kind == RPG_KIND_WEAPON) return rpg.party.weapons;
    if (kind == RPG_KIND_ARMOR) return rpg.party.armors;
    return 0;
}

int rpg_party_item_number(int kind, int id)
{
    int16_t *c = container(kind);
    return (c && id > 0 && id < RPG_MAX_ITEMS) ? c[id] : 0;
}

int (*rpg_max_item_hook)(int kind, int id);
int rpg_party_max_item_number(int kind, int id)
{
    return rpg_max_item_hook ? rpg_max_item_hook(kind, id) : 99;
}

int rpg_party_has_item(int kind, int id, int include_equip)
{
    if (rpg_party_item_number(kind, id) > 0)
        return 1;
    if (!include_equip || kind == RPG_KIND_ITEM)
        return 0;
    int ids[RPG_MAX_PARTY], n = members(ids);
    for (int i = 0; i < n; i++)
        if (rpg_actor_has_equip(rpg_actor(ids[i]), kind == RPG_KIND_ARMOR, id))
            return 1;
    return 0;
}

void rpg_party_gain_item(int kind, int id, int amount, int include_equip)
{
    int16_t *c = container(kind);
    if (!c || id <= 0 || id >= RPG_MAX_ITEMS)
        return;
    int last = c[id], nn = last + amount, mx = rpg_party_max_item_number(kind, id);
    c[id] = nn < 0 ? 0 : (nn > mx ? mx : nn);
    if (include_equip && nn < 0) {
        int left = -nn, ids[RPG_MAX_PARTY], n = members(ids);
        for (int i = 0; i < n && left > 0; i++) {
            RpgActorState *a = rpg_actor(ids[i]);
            while (left > 0 && rpg_actor_has_equip(a, kind == RPG_KIND_ARMOR, id)) {
                rpg_actor_discard_equip(a, kind, id);
                left--;
            }
        }
    }
    rpg.need_refresh = 1;
}

void rpg_party_lose_item(int kind, int id, int amount, int include_equip)
{
    rpg_party_gain_item(kind, id, -amount, include_equip);
}

void rpg_party_consume_item(int kind, int id)
{
    if (kind == RPG_KIND_ITEM && rpg_item_data(id)->consumable)
        rpg_party_lose_item(kind, id, 1, 0);
}

void rpg_party_gain_gold(int amount)
{
    long long g = (long long)rpg.party.gold + amount;
    rpg.party.gold = g < 0 ? 0 : (g > 99999999 ? 99999999 : (int)g);
}

void rpg_party_add_actor(int id)
{
    if (!rpg_party_member(id) && rpg.party.nmembers < RPG_MAX_PARTY)
        rpg.party.members[rpg.party.nmembers++] = id;
    rpg_player_refresh();
    rpg.need_refresh = 1;
}

void rpg_party_remove_actor(int id)
{
    for (int i = 0; i < rpg.party.nmembers; i++)
        if (rpg.party.members[i] == id) {
            memmove(&rpg.party.members[i], &rpg.party.members[i + 1], (rpg.party.nmembers - i - 1) * sizeof(int));
            rpg.party.nmembers--;
            break;
        }
    rpg_player_refresh();
    rpg.need_refresh = 1;
}

void rpg_party_swap_order(int a, int b)
{
    int t = rpg.party.members[a];
    rpg.party.members[a] = rpg.party.members[b];
    rpg.party.members[b] = t;
    rpg_player_refresh();
}

int rpg_party_all_dead(void)
{
    int ids[RPG_MAX_PARTY], n = members(ids);
    for (int i = 0; i < n; i++)
        if (rpg_battler_alive(&rpg_actor(ids[i])->b))
            return 0;
    return rpg.party.in_battle || n > 0;
}

int rpg_party_highest_level(void)
{
    int ids[RPG_MAX_PARTY], n = members(ids), m = 0;
    for (int i = 0; i < n; i++)
        if (rpg_actor(ids[i])->level > m)
            m = rpg_actor(ids[i])->level;
    return m;
}

int rpg_party_usable(int kind, int id)
{
    int ids[RPG_MAX_PARTY], n = members(ids);
    for (int i = 0; i < n; i++)
        if (rpg_battler_usable(&rpg_actor(ids[i])->b, kind, id))
            return 1;
    return 0;
}

int rpg_party_inputable(void)
{
    int ids[RPG_MAX_PARTY], n = members(ids);
    for (int i = 0; i < n; i++)
        if (rpg_battler_inputable(&rpg_actor(ids[i])->b))
            return 1;
    return 0;
}

int rpg_party_agi(void)
{
    int ids[RPG_MAX_PARTY], n = members(ids), s = 0;
    if (!n)
        return 1;
    for (int i = 0; i < n; i++)
        s += rpg_battler_param(&rpg_actor(ids[i])->b, 6);
    return s / n;
}

int rpg_party_ability(int ability)
{
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
    for (int i = 0; i < n; i++)
        if (rpg_features_set_has(&rpg_actor(ids[i])->b, F_PARTY_ABILITY, ability))
            return 1;
    return 0;
}
int rpg_party_encounter_half(void) { return rpg_party_ability(0); }
int rpg_party_encounter_none(void) { return rpg_party_ability(1); }

void rpg_party_increase_steps(void) { rpg.party.steps++; }

static void show_states(RpgActorState *a)
{
    for (int i = 0; i < a->b.result.nadded; i++) {
        const char *m = RPG_STR(rpg_state_data(a->b.result.added_states[i])->message1);
        if (*m) {
            char line[256];
            snprintf(line, sizeof line, "%s%s", a->name, m);
            rpg_msg_add(line);
        }
    }
    for (int i = 0; i < a->b.result.nremoved; i++) {
        const char *m = RPG_STR(rpg_state_data(a->b.result.removed_states[i])->message4);
        if (*m) {
            char line[256];
            snprintf(line, sizeof line, "%s%s", a->name, m);
            rpg_msg_add(line);
        }
    }
}

void rpg_party_on_player_walk(void)
{
    int ids[RPG_MAX_PARTY], n = members(ids);
    for (int i = 0; i < n; i++) {
        RpgActorState *a = rpg_actor(ids[i]);
        rpg_battler_result_clear(&a->b);
        /* floor damage */
        if (rpg_map_damage_floor(rpg.player.ch.x, rpg.player.ch.y)) {
            int dmg = (int)(10 * rpg_battler_sparam(&a->b, 8));
            int mx = rpg_system()->opt_floor_death ? a->b.hp : (a->b.hp - 1 > 0 ? a->b.hp - 1 : 0);
            rpg_battler_set_hp(&a->b, a->b.hp - (dmg < mx ? dmg : mx));
            if (dmg > 0)
                rpg_screen_flash_damage(&rpg.screen);
        }
        if (rpg.player.vehicle < 0 && !rpg.player.ch.move_route_forcing) {
            if (rpg.party.steps % 20 == 0) {
                rpg_battler_on_turn_end(&a->b);
                if (a->b.result.hp_damage > 0)
                    rpg_screen_flash_damage(&rpg.screen);
            }
            int ids2[24], n2 = a->b.nstates;
            for (int k = 0; k < n2; k++)
                ids2[k] = a->b.states[k].id;
            for (int k = 0; k < n2; k++) {
                const RpgState *st = rpg_state_data(ids2[k]);
                if (!st->remove_by_walking || !rpg_battler_state(&a->b, ids2[k]))
                    continue;
                for (int s = 0; s < a->b.nstates; s++)
                    if (a->b.states[s].id == ids2[k] && a->b.states[s].steps > 0)
                        a->b.states[s].steps--;
                for (int s = 0; s < a->b.nstates; s++)
                    if (a->b.states[s].id == ids2[k] && a->b.states[s].steps == 0) {
                        rpg_battler_remove_state(&a->b, ids2[k]);
                        break;
                    }
            }
            show_states(a);
        }
    }
}

RpgActorState *rpg_party_menu_actor(void)
{
    if (rpg.party.menu_actor > 0 && rpg_party_member(rpg.party.menu_actor))
        return rpg_actor(rpg.party.menu_actor);
    return rpg.party.nmembers ? rpg_actor(rpg.party.members[0]) : 0;
}

void rpg_party_set_menu_actor(int id) { rpg.party.menu_actor = id; }

void rpg_party_menu_actor_step(int d)
{
    int n = rpg.party.nmembers, cur = 0;
    if (!n)
        return;
    RpgActorState *m = rpg_party_menu_actor();
    for (int i = 0; i < n; i++)
        if (m && rpg.party.members[i] == m->actor_id)
            cur = i;
    cur = (cur + d + n) % n;
    rpg.party.menu_actor = rpg.party.members[cur];
}

void rpg_party_setup_starting(void)
{
    const RpgSystem *s = rpg_system();
    rpg.party.nmembers = 0;
    for (int i = 0; i < s->party_members.count && i < RPG_MAX_PARTY; i++)
        rpg.party.members[rpg.party.nmembers++] = *(const int32_t *)rpg_at(&rpg_db, RPG_T_INT, s->party_members, i);
    for (int i = 0; i < rpg.party.nmembers; i++)
        rpg_actor(rpg.party.members[i]);
}
