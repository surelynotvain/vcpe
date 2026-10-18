/* VCPE RPG battle: see vcpe_rpg_battle.h. Follows VX Ace's BattleManager, Scene_Battle, Window_BattleLog,
 * Spriteset_Battle and Sprite_Battler; Game_Enemy / Game_Troop / Game_Unit / Game_Action. */
#include "vcpe_rpg.h"
#include "vcpe_rpg_battle.h"
#include "vcpe_rpg_menu.h"
#include "vcpe_co.h"
#include "vcpe_spr.h"
#include "vcpe_gfx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

RpgBattleState rpg_b;
RpgBattleWindows rpg_bw;
RpgBattleVT rpg_bvt;
static RpgBattlerSprite enemy_sprites[RPG_MAX_ENEMIES], actor_sprites[RPG_MAX_ACTORS];
int rpg_log_message_speed = 20;
int (*rpg_hooks_enemy_turn)(const RpgEnemyState *e);
void (*rpg_hooks_enemy_actions)(RpgEnemyState *e);
int rpg_hooks_force_action_nowait;
const char *(*rpg_battle_command_name_hook)(int cmd, int ext);
void rpg_scene_fade(int frames, int dir, void (*then)(void));
int rpg_scene_fade_step(void);
int rpg_scene_changing(void);

/* ------------------------------------------------------------------ units */
int rpg_unit_members(int troop, RpgBattler **out)
{
    int n = 0;
    if (troop) {
        for (int i = 0; i < rpg_b.nenemies; i++)
            out[n++] = &rpg_b.enemies[i].b;
    } else {
        int ids[RPG_MAX_PARTY], k = rpg.party.in_battle ? rpg_party_battle_members(ids) : rpg.party.nmembers;
        if (!rpg.party.in_battle)
            memcpy(ids, rpg.party.members, sizeof(int) * k);
        for (int i = 0; i < k; i++)
            out[n++] = &rpg_actor(ids[i])->b;
    }
    return n;
}

static int filter(int troop, RpgBattler **out, int (*f)(const RpgBattler *))
{
    RpgBattler *all[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(troop, all), k = 0;
    for (int i = 0; i < n; i++)
        if (f(all[i]))
            out[k++] = all[i];
    return k;
}

int rpg_unit_alive(int troop, RpgBattler **out) { return filter(troop, out, rpg_battler_alive); }
int rpg_unit_dead(int troop, RpgBattler **out) { return filter(troop, out, rpg_battler_dead); }
int rpg_unit_movable(int troop, RpgBattler **out) { return filter(troop, out, rpg_battler_movable); }
int rpg_unit_all_dead(int troop)
{
    RpgBattler *a[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    if (troop)
        return rpg_unit_alive(1, a) == 0;
    return rpg_party_all_dead();
}

int rpg_unit_agi(int troop)
{
    RpgBattler *a[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(troop, a), s = 0;
    if (!n)
        return 1;
    for (int i = 0; i < n; i++)
        s += rpg_battler_param(a[i], 6);
    return s / n;
}

RpgBattler *rpg_unit_random_target(int troop)
{
    RpgBattler *a[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_alive(troop, a);
    if (!n)
        return 0;
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += rpg_battler_sparam(a[i], 0);
    double r = rpg_randf() * sum;
    for (int i = 0; i < n; i++) {
        r -= rpg_battler_sparam(a[i], 0);
        if (r < 0)
            return a[i];
    }
    return a[0];
}

RpgBattler *rpg_unit_random_dead_target(int troop)
{
    RpgBattler *a[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_dead(troop, a);
    return n ? a[rpg_rand(n)] : 0;
}

RpgBattler *rpg_unit_smooth_target(int troop, int index)
{
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY], *a[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(troop, m);
    if (index >= 0 && index < n && rpg_battler_alive(m[index]))
        return m[index];
    return rpg_unit_alive(troop, a) ? a[0] : 0;
}

RpgBattler *rpg_unit_smooth_dead_target(int troop, int index)
{
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY], *a[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(troop, m);
    if (index >= 0 && index < n && rpg_battler_dead(m[index]))
        return m[index];
    return rpg_unit_dead(troop, a) ? a[0] : 0;
}

RpgBattler *rpg_unit_substitute(int troop)
{
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(troop, m);
    for (int i = 0; i < n; i++)
        if (rpg_special_flag(m[i], 2) && rpg_battler_movable(m[i]))
            return m[i];
    return 0;
}

int rpg_battler_index(const RpgBattler *b)
{
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(!b->is_actor, m);
    for (int i = 0; i < n; i++)
        if (m[i] == b)
            return i;
    return -1;
}

int rpg_battler_opposite_troop(const RpgBattler *b) { return b->is_actor; }

/* ------------------------------------------------------------------ Game_Enemy / Game_Troop */
RpgEnemyState *rpg_troop_member(int i) { return i >= 0 && i < rpg_b.nenemies ? &rpg_b.enemies[i] : 0; }
int rpg_troop_count(void) { return rpg_b.nenemies; }

void rpg_enemy_setup(RpgEnemyState *e, int index, int enemy_id)
{
    memset(e, 0, sizeof *e);
    e->b.is_actor = 0;
    e->b.index = index;
    e->enemy_id = enemy_id;
    const RpgEnemy *d = rpg_enemy_data(enemy_id);
    snprintf(e->name, RPG_NAME, "%s", RPG_STR(d->name));
    e->b.hp = rpg_battler_mhp(&e->b);
    e->b.mp = rpg_battler_mmp(&e->b);
}

static const char *original_name(const RpgEnemyState *e) { return RPG_STR(rpg_enemy_data(e->enemy_id)->name); }

static void enemy_name(RpgEnemyState *e)
{
    static const char *half[26] = {" A", " B", " C", " D", " E", " F", " G", " H", " I", " J", " K", " L", " M",
                                   " N", " O", " P", " Q", " R", " S", " T", " U", " V", " W", " X", " Y", " Z"};
    static const char *full[26] = {"\xef\xbc\xa1", "\xef\xbc\xa2", "\xef\xbc\xa3", "\xef\xbc\xa4", "\xef\xbc\xa5",
                                   "\xef\xbc\xa6", "\xef\xbc\xa7", "\xef\xbc\xa8", "\xef\xbc\xa9", "\xef\xbc\xaa",
                                   "\xef\xbc\xab", "\xef\xbc\xac", "\xef\xbc\xad", "\xef\xbc\xae", "\xef\xbc\xaf",
                                   "\xef\xbc\xb0", "\xef\xbc\xb1", "\xef\xbc\xb2", "\xef\xbc\xb3", "\xef\xbc\xb4",
                                   "\xef\xbc\xb5", "\xef\xbc\xb6", "\xef\xbc\xb7", "\xef\xbc\xb8", "\xef\xbc\xb9",
                                   "\xef\xbc\xba"};
    const char *l = e->letter > 0 ? (rpg_system()->japanese ? full : half)[(e->letter - 1) % 26] : "";
    snprintf(e->name, RPG_NAME, "%s%s", original_name(e), e->plural ? l : "");
}

void rpg_troop_make_unique_names(void)
{
    for (int i = 0; i < rpg_b.nenemies; i++) {
        RpgEnemyState *e = &rpg_b.enemies[i];
        if (!rpg_battler_alive(&e->b) || e->letter)
            continue;
        int k;
        for (k = 0; k < rpg_b.nnames; k++)
            if (!strcmp(rpg_b.names_seen[k], original_name(e)))
                break;
        if (k == rpg_b.nnames && k < RPG_MAX_ENEMIES) {
            snprintf(rpg_b.names_seen[k], RPG_NAME, "%s", original_name(e));
            rpg_b.names_count[k] = 0;
            rpg_b.nnames++;
        }
        e->letter = rpg_b.names_count[k] % 26 + 1;
        rpg_b.names_count[k]++;
    }
    for (int i = 0; i < rpg_b.nenemies; i++) {
        RpgEnemyState *e = &rpg_b.enemies[i];
        for (int k = 0; k < rpg_b.nnames; k++)
            if (!strcmp(rpg_b.names_seen[k], original_name(e)) && rpg_b.names_count[k] >= 2)
                e->plural = 1;
        enemy_name(e);
    }
}

void rpg_enemy_transform(RpgEnemyState *e, int enemy_id)
{
    const char *old = original_name(e);
    char was[RPG_NAME];
    snprintf(was, sizeof was, "%s", old);
    e->enemy_id = enemy_id;
    if (strcmp(original_name(e), was)) {
        e->letter = 0;
        e->plural = 0;
    }
    enemy_name(e);
    rpg_battler_refresh(&e->b);
    if (e->b.nactions)
        rpg_battler_make_actions(&e->b);
}

static void troop_setup(int troop_id)
{
    memset(rpg_b.enemies, 0, sizeof rpg_b.enemies);
    rpg_b.nenemies = 0;
    rpg_b.turn_count = 0;
    rpg_b.nnames = 0;
    memset(rpg_b.event_flags, 0, sizeof rpg_b.event_flags);
    rpg_interp_clear(&rpg_b.interp);
    rpg_b.interp.battle = 1;
    rpg_screen_clear(&rpg_battle_screen);
    rpg_b.troop_id = troop_id;
    const RpgTroop *t = rpg_troop_data(troop_id);
    for (int i = 0; i < t->members.count && rpg_b.nenemies < RPG_MAX_ENEMIES; i++) {
        const RpgMember *m = rpg_at(&rpg_db, RPG_T_MEMBER, t->members, i);
        if (!rpg_enemy_data(m->enemy_id)->id)
            continue;
        RpgEnemyState *e = &rpg_b.enemies[rpg_b.nenemies];
        rpg_enemy_setup(e, rpg_b.nenemies, m->enemy_id);
        e->b.hidden = m->hidden;
        e->screen_x = m->x;
        e->screen_y = m->y;
        rpg_b.nenemies++;
    }
    /* init_screen_tone: the map's tone carries into the battle */
    memcpy(rpg_battle_screen.tone, rpg.screen.tone, sizeof rpg.screen.tone);
    rpg_troop_make_unique_names();
}

int rpg_troop_conditions_met(int page)
{
    const RpgTroop *t = rpg_troop_data(rpg_b.troop_id);
    const RpgTroopPage *p = rpg_at(&rpg_db, RPG_T_TROOPPAGE, t->pages, page);
    if (!p)
        return 0;
    const RpgTroopCond *c = &p->condition;
    if (!c->turn_ending && !c->turn_valid && !c->enemy_valid && !c->actor_valid && !c->switch_valid)
        return 0;
    if (page < 64 && rpg_b.event_flags[page])
        return 0;
    if (c->turn_ending && rpg_b.phase != BP_TURN_END)
        return 0;
    if (c->turn_valid) {
        int n = rpg_b.turn_count, a = c->turn_a, b = c->turn_b;
        if (b == 0 && n != a)
            return 0;
        if (b > 0 && (n < 1 || n < a || n % b != a % b))
            return 0;
    }
    if (c->enemy_valid) {
        RpgEnemyState *e = rpg_troop_member(c->enemy_index);
        if (!e)
            return 0;
        int mhp = rpg_battler_mhp(&e->b);
        if ((double)e->b.hp / (mhp ? mhp : 1) * 100 > c->enemy_hp)
            return 0;
    }
    if (c->actor_valid) {
        if (c->actor_id <= 0 || c->actor_id >= RPG_MAX_ACTORS)
            return 0;
        RpgActorState *a = rpg_actor(c->actor_id);
        int mhp = rpg_battler_mhp(&a->b);
        if ((double)a->b.hp / (mhp ? mhp : 1) * 100 > c->actor_hp)
            return 0;
    }
    if (c->switch_valid && !rpg_sw(c->switch_id))
        return 0;
    return 1;
}

void rpg_troop_setup_battle_event(void)
{
    if (rpg_interp_running(&rpg_b.interp))
        return;
    if (rpg.reserved_common > 0) {
        rpg_interp_setup(&rpg_b.interp, 1, rpg.reserved_common, 0, 0);
        rpg.reserved_common = 0;
        return;
    }
    const RpgTroop *t = rpg_troop_data(rpg_b.troop_id);
    for (int i = 0; i < t->pages.count; i++) {
        if (!rpg_troop_conditions_met(i))
            continue;
        rpg_interp_setup(&rpg_b.interp, 2, rpg_b.troop_id, i, 0);
        const RpgTroopPage *p = rpg_at(&rpg_db, RPG_T_TROOPPAGE, t->pages, i);
        if (p->span <= 1 && i < 64)
            rpg_b.event_flags[i] = 1;
        return;
    }
}

void rpg_troop_increase_turn(void)
{
    const RpgTroop *t = rpg_troop_data(rpg_b.troop_id);
    for (int i = 0; i < t->pages.count && i < 64; i++) {
        const RpgTroopPage *p = rpg_at(&rpg_db, RPG_T_TROOPPAGE, t->pages, i);
        if (p->span == 1)
            rpg_b.event_flags[i] = 0;
    }
    rpg_b.turn_count++;
}

int rpg_troop_exp_total(void)
{
    int s = 0;
    for (int i = 0; i < rpg_b.nenemies; i++)
        if (rpg_battler_dead(&rpg_b.enemies[i].b)) {
            int e = rpg_enemy_data(rpg_b.enemies[i].enemy_id)->exp;
            s += rpg_bh.enemy_exp ? rpg_bh.enemy_exp(&rpg_b.enemies[i], e) : e;
        }
    return s;
}

int rpg_troop_gold_total(void)
{
    int s = 0;
    for (int i = 0; i < rpg_b.nenemies; i++)
        if (rpg_battler_dead(&rpg_b.enemies[i].b)) {
            int g = rpg_enemy_data(rpg_b.enemies[i].enemy_id)->gold;
            s += rpg_bh.enemy_gold ? rpg_bh.enemy_gold(&rpg_b.enemies[i], g) : g;
        }
    return s * (rpg_party_ability(4) ? 2 : 1);
}

int rpg_troop_drop_items(int *kinds, int *ids, int max)
{
    int n = 0;
    for (int i = 0; i < rpg_b.nenemies; i++) {
        if (!rpg_battler_dead(&rpg_b.enemies[i].b))
            continue;
        double rate = rpg_party_ability(5) ? 2 : 1;
        if (rpg_bh.drop_rate)
            rate = rpg_bh.drop_rate(&rpg_b.enemies[i], rate);
        const RpgEnemy *d = rpg_enemy_data(rpg_b.enemies[i].enemy_id);
        for (int k = 0; k < d->drop_items.count && n < max; k++) {
            const RpgDropItem *di = rpg_at(&rpg_db, RPG_T_DROPITEM, d->drop_items, k);
            if (di->kind > 0 && rpg_randf() * di->denominator < rate) {
                kinds[n] = di->kind == 1 ? RPG_KIND_ITEM : di->kind == 2 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR;
                ids[n++] = di->data_id;
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ Game_Action */
void rpg_action_set_skill(RpgBattler *b, int i, int skill)
{
    b->actions[i].kind = 0;
    b->actions[i].id = skill;
    b->actions[i].valid = rpg_skill_data(skill)->id != 0;
}

void rpg_action_set_item(RpgBattler *b, int i, int item)
{
    b->actions[i].kind = 1;
    b->actions[i].id = item;
    b->actions[i].valid = rpg_item_data(item)->id != 0;
}

int rpg_action_kind(const RpgBattler *b, int i) { return b->actions[i].kind ? RPG_KIND_ITEM : RPG_KIND_SKILL; }
int rpg_action_item_id(const RpgBattler *b, int i) { return b->actions[i].valid ? b->actions[i].id : 0; }
int rpg_action_attack(const RpgBattler *b, int i)
{
    return b->actions[i].valid && b->actions[i].kind == 0 && b->actions[i].id == rpg_battler_attack_skill(b);
}

static void action_clear(RpgBattler *b, int i)
{
    b->actions[i].valid = 0;
    b->actions[i].id = 0;
    b->actions[i].target = -1;
}

void rpg_action_decide_random_target(RpgBattler *b, int i)
{
    const RpgUsable *u = rpg_usable(rpg_action_kind(b, i), b->actions[i].id);
    RpgBattler *t;
    if (rpg_for_dead_friend(u))
        t = rpg_unit_random_dead_target(!b->is_actor);
    else if (rpg_for_friend(u))
        t = rpg_unit_random_target(!b->is_actor);
    else
        t = rpg_unit_random_target(b->is_actor);
    if (t)
        b->actions[i].target = rpg_battler_index(t);
    else
        action_clear(b, i);
}

void rpg_action_prepare(RpgBattler *b, int i)
{
    if (rpg_battler_confusion(b) && !b->actions[i].forcing)
        rpg_action_set_skill(b, i, rpg_battler_attack_skill(b));
}

int rpg_action_valid(const RpgBattler *b, int i)
{
    if (!b->actions[i].valid)
        return 0;
    return b->actions[i].forcing || rpg_battler_usable(b, rpg_action_kind(b, i), b->actions[i].id);
}

int rpg_action_speed(const RpgBattler *b, int i)
{
    int agi = rpg_battler_param(b, 6);
    int s = agi + rpg_rand(5 + agi / 4);
    if (b->actions[i].valid)
        s += rpg_usable(rpg_action_kind(b, i), b->actions[i].id)->speed;
    if (rpg_action_attack(b, i))
        s += (int)rpg_battler_atk_speed(b);
    return s;
}

static RpgBattler *confusion_target(RpgBattler *b)
{
    switch (rpg_battler_confusion(b)) {
    case 1: return rpg_unit_random_target(b->is_actor);
    case 2: return rpg_rand(2) == 0 ? rpg_unit_random_target(b->is_actor) : rpg_unit_random_target(!b->is_actor);
    default: return rpg_unit_random_target(!b->is_actor);
    }
}

int rpg_action_make_targets(RpgBattler *b, int i, RpgBattler **out, int max)
{
    int n = 0;
    const RpgUsable *u = rpg_usable(rpg_action_kind(b, i), b->actions[i].id);
    int ti = b->actions[i].target;
    if (!b->actions[i].forcing && rpg_battler_confusion(b)) {
        RpgBattler *t = confusion_target(b);
        if (t)
            out[n++] = t;
    } else if (rpg_for_opponent(u)) {
        int opp = b->is_actor;
        if (rpg_for_random(u)) {
            int k = rpg_for_random(u) ? u->scope - 2 : 0;
            for (int j = 0; j < k && n < max; j++) {
                RpgBattler *t = rpg_unit_random_target(opp);
                if (t) out[n++] = t;
            }
        } else if (rpg_for_one(u)) {
            int num = 1 + (rpg_action_attack(b, i) ? rpg_battler_atk_times_add(b) : 0);
            RpgBattler *t = ti < 0 ? rpg_unit_random_target(opp) : rpg_unit_smooth_target(opp, ti);
            for (int j = 0; j < num && n < max; j++)
                if (t) out[n++] = t;
        } else {
            n = rpg_unit_alive(opp, out);
        }
    } else if (rpg_for_friend(u)) {
        int fr = !b->is_actor;
        if (rpg_for_user(u)) {
            out[n++] = b;
        } else if (rpg_for_dead_friend(u)) {
            if (rpg_for_one(u)) {
                RpgBattler *t = rpg_unit_smooth_dead_target(fr, ti);
                if (t) out[n++] = t;
            } else {
                n = rpg_unit_dead(fr, out);
            }
        } else if (rpg_for_one(u)) {
            RpgBattler *t = rpg_unit_smooth_target(fr, ti);
            if (t) out[n++] = t;
        } else {
            n = rpg_unit_alive(fr, out);
        }
    }
    return n;
}

static double evaluate_with_target(RpgBattler *b, int i, RpgBattler *t)
{
    const RpgUsable *u = rpg_usable(rpg_action_kind(b, i), b->actions[i].id);
    rpg_battler_result_clear(t);
    rpg_battler_make_damage_value(t, b, rpg_action_kind(b, i), b->actions[i].id);
    if (rpg_for_opponent(u))
        return (double)t->result.hp_damage / (t->hp > 1 ? t->hp : 1);
    int rec = -t->result.hp_damage, room = rpg_battler_mhp(t) - t->hp;
    return (double)(rec < room ? rec : room) / rpg_battler_mhp(t);
}

double rpg_action_evaluate(RpgBattler *b, int i)
{
    double v = 0;
    if (rpg_action_valid(b, i)) {
        const RpgUsable *u = rpg_usable(rpg_action_kind(b, i), b->actions[i].id);
        RpgBattler *c[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
        int n;
        if (rpg_for_opponent(u)) n = rpg_unit_alive(b->is_actor, c);
        else if (rpg_for_user(u)) { c[0] = b; n = 1; }
        else if (rpg_for_dead_friend(u)) n = rpg_unit_dead(!b->is_actor, c);
        else n = rpg_unit_alive(!b->is_actor, c);
        for (int k = 0; k < n; k++) {
            double x = evaluate_with_target(b, i, c[k]);
            if (rpg_for_all(u)) {
                v += x;
            } else if (x > v) {
                v = x;
                b->actions[i].target = rpg_battler_index(c[k]);
            }
        }
    }
    if (v > 0)
        v += rpg_randf();
    return v;
}

void rpg_battler_clear_actions(RpgBattler *b)
{
    b->nactions = 0;
    if (b->is_actor)
        ((RpgActorState *)b)->action_input_index = 0;
}

static int make_action_times(RpgBattler *b)
{
    int r = 1;
    const RpgFeature *fs[256];
    int n = rpg_features_list(b, fs, 256);
    for (int i = 0; i < n; i++)
        if (fs[i]->code == F_ACTION_PLUS && rpg_randf() < fs[i]->value)
            r++;
    return r > 8 ? 8 : r;
}

static int enemy_action_ok(RpgEnemyState *e, const RpgAction *a)
{
    double p1 = a->condition_param1, p2 = a->condition_param2;
    int ok = 1;
    switch (a->condition_type) {
    case 1: {
        int n = rpg_b.turn_count;
        if (rpg_hooks_enemy_turn)
            n = rpg_hooks_enemy_turn(e);
        ok = p2 == 0 ? n == (int)p1 : (n > 0 && n >= (int)p1 && n % (int)p2 == (int)p1 % (int)p2);
        break;
    }
    case 2: {
        double r = (double)e->b.hp / rpg_battler_mhp(&e->b);
        ok = r >= p1 && r <= p2;
        break;
    }
    case 3: {
        int mmp = rpg_battler_mmp(&e->b);
        double r = mmp > 0 ? (double)e->b.mp / mmp : 0;
        ok = r >= p1 && r <= p2;
        break;
    }
    case 4: ok = rpg_battler_state(&e->b, (int)p1); break;
    case 5: ok = rpg_party_highest_level() >= p1; break;
    case 6: ok = rpg_sw((int)p1); break;
    }
    return ok && rpg_battler_usable(&e->b, RPG_KIND_SKILL, a->skill_id);
}

void rpg_battler_make_actions(RpgBattler *b)
{
    rpg_battler_clear_actions(b);
    if (!rpg_battler_movable(b))
        return;
    b->nactions = make_action_times(b);
    for (int i = 0; i < b->nactions; i++) {
        action_clear(b, i);
        b->actions[i].forcing = 0;
    }
    if (b->is_actor) {
        RpgActorState *a = (RpgActorState *)b;
        if (rpg_special_flag(b, 0)) {
            /* auto battle: the best evaluated action */
            for (int i = 0; i < b->nactions; i++) {
                double best = -1;
                int bk = 0, bid = 1, bt = -1;
                int skills[RPG_MAX_SKILLS / 4], ns = rpg_actor_skills(a, skills, RPG_MAX_SKILLS / 4);
                for (int k = -1; k < ns; k++) {
                    int sid = k < 0 ? rpg_battler_attack_skill(b) : skills[k];
                    if (k >= 0 && !rpg_battler_usable(b, RPG_KIND_SKILL, sid))
                        continue;
                    rpg_action_set_skill(b, i, sid);
                    b->actions[i].target = -1;
                    double v = rpg_action_evaluate(b, i);
                    if (v > best) {   /* max keeps the first maximum */
                        best = v;
                        bk = 0;
                        bid = sid;
                        bt = b->actions[i].target;
                    }
                }
                (void)bk;
                rpg_action_set_skill(b, i, bid);
                b->actions[i].target = bt;
            }
        } else if (rpg_battler_confusion(b)) {
            for (int i = 0; i < b->nactions; i++)
                rpg_action_set_skill(b, i, rpg_battler_attack_skill(b));
        }
        return;
    }
    RpgEnemyState *e = (RpgEnemyState *)b;
    if (rpg_hooks_enemy_actions) {
        rpg_hooks_enemy_actions(e);
        return;
    }
    const RpgEnemy *d = rpg_enemy_data(e->enemy_id);
    const RpgAction *list[64];
    int n = 0, rmax = 0;
    for (int k = 0; k < d->actions.count && n < 64; k++) {
        const RpgAction *a = rpg_at(&rpg_db, RPG_T_ACTION, d->actions, k);
        if (enemy_action_ok(e, a)) {
            list[n++] = a;
            if (a->rating > rmax) rmax = a->rating;
        }
    }
    if (!n)
        return;
    int zero = rmax - 3, m = 0;
    for (int k = 0; k < n; k++)
        if (list[k]->rating > zero)
            list[m++] = list[k];
    for (int i = 0; i < b->nactions; i++) {
        int sum = 0;
        for (int k = 0; k < m; k++)
            sum += list[k]->rating - zero;
        if (sum <= 0) {
            action_clear(b, i);
            continue;
        }
        int v = rpg_rand(sum);
        for (int k = 0; k < m; k++) {
            if (v < list[k]->rating - zero) {
                rpg_action_set_skill(b, i, list[k]->skill_id);
                break;
            }
            v -= list[k]->rating - zero;
        }
    }
}

void rpg_battler_make_speed(RpgBattler *b)
{
    int s = 0, any = 0;
    for (int i = 0; i < b->nactions; i++) {
        int x = rpg_action_speed(b, i);
        if (!any || x < s) s = x;
        any = 1;
    }
    b->speed = any ? s : 0;
}

void rpg_battler_force_action(RpgBattler *b, int skill, int target)
{
    rpg_battler_clear_actions(b);
    b->nactions = 1;
    action_clear(b, 0);
    b->actions[0].forcing = 1;
    rpg_action_set_skill(b, 0, skill);
    if (target == -2)
        b->actions[0].target = b->last_target_index;
    else if (target == -1)
        rpg_action_decide_random_target(b, 0);
    else
        b->actions[0].target = target;
}

int rpg_battler_current_action(const RpgBattler *b) { return b->nactions > 0; }

void rpg_battler_remove_current_action(RpgBattler *b)
{
    if (b->nactions > 0) {
        memmove(&b->actions[0], &b->actions[1], sizeof b->actions[0] * (b->nactions - 1));
        b->nactions--;
    }
}

int rpg_actor_next_command(RpgActorState *a)
{
    if (a->action_input_index >= a->b.nactions - 1)
        return 0;
    a->action_input_index++;
    return 1;
}

int rpg_actor_prior_command(RpgActorState *a)
{
    if (a->action_input_index <= 0)
        return 0;
    a->action_input_index--;
    return 1;
}

double rpg_item_cnt(RpgBattler *t, RpgBattler *user, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    if (u->hit_type != 1 || t->is_actor == user->is_actor)
        return 0;
    return rpg_battler_xparam(t, 6);
}

double rpg_item_mrf(RpgBattler *t, RpgBattler *user, int kind, int id)
{
    (void)user;
    return rpg_usable(kind, id)->hit_type == 2 ? rpg_battler_xparam(t, 5) : 0;
}

/* ------------------------------------------------------------------ waits (coroutine) */
static int show_fast(void) { return (rpg_in.press & (RPG_K_A | RPG_K_C)) != 0; }

void rpg_bupdate_for_wait(void) { rpg_bvt.update_basic(); }

static void def_wait(int d)
{
    for (int i = 0; i < d; i++)
        if (i < d / 2 || !show_fast())
            rpg_bupdate_for_wait();
}

static void def_abs_wait(int d)
{
    for (int i = 0; i < d; i++)
        rpg_bupdate_for_wait();
}
void rpg_bwait(int d) { rpg_bvt.wait(d); }
void rpg_babs_wait(int d) { rpg_bvt.abs_wait(d); }
int rpg_battle_anim_omit, rpg_battle_effect_omit;
int rpg_battle_effect_remaining(const RpgBattlerSprite *s) { return s->effect != BE_NONE ? s->duration : 0; }

void rpg_bwait_for_message(void)
{
    rpg_msg_update();
    while (rpg_msg.visible)
        rpg_bupdate_for_wait();
}

int rpg_battle_animation_playing(void)
{
    for (int i = 0; i < rpg_b.nenemies; i++)
        if (rpg_anim_remaining(&rpg_b.enemies[i].b) > rpg_battle_anim_omit)
            return 1;
    for (int i = 1; i < RPG_MAX_ACTORS; i++)
        if (rpg.actors[i].exists && rpg_anim_remaining(&rpg.actors[i].b) > rpg_battle_anim_omit)
            return 1;
    return rpg_anim_remaining(0) > rpg_battle_anim_omit;
}

int rpg_battle_effect_playing(void)
{
    for (int i = 0; i < rpg_b.nenemies; i++)
        if (rpg_battle_effect_remaining(&enemy_sprites[i]) > rpg_battle_effect_omit)
            return 1;
    return 0;
}

void rpg_bwait_for_animation(void)
{
    rpg_bupdate_for_wait();
    while (rpg_battle_animation_playing())
        rpg_bupdate_for_wait();
}

void rpg_bwait_for_effect(void)
{
    rpg_bupdate_for_wait();
    while (rpg_battle_effect_playing())
        rpg_bupdate_for_wait();
}

/* ------------------------------------------------------------------ Window_BattleLog */
static char log_lines[RPG_LOG_LINES + 2][256];
static int log_n, log_num_wait;

void rpg_log_clear(void)
{
    log_num_wait = 0;
    log_n = 0;
}
int rpg_log_lines(void) { return log_n; }
const char *rpg_log_last(void) { return log_n ? log_lines[log_n - 1] : 0; }
void rpg_log_add(const char *t)
{
    if (log_n >= RPG_LOG_LINES + 2) {
        memmove(log_lines[0], log_lines[1], sizeof log_lines[0] * (RPG_LOG_LINES + 1));
        log_n--;
    }
    snprintf(log_lines[log_n++], sizeof log_lines[0], "%s", t);
}
void rpg_log_replace(const char *t)
{
    if (log_n)
        log_n--;
    rpg_log_add(t);
}
void rpg_log_back_one(void)
{
    if (log_n)
        log_n--;
}
void rpg_log_back_to(int n)
{
    if (log_n > n)
        log_n = n;
}
void rpg_log_wait(void)
{
    log_num_wait++;
    rpg_bwait(rpg_log_message_speed);
}
void rpg_log_wait_and_clear(void)
{
    if (log_n > 0)
        while (log_num_wait < 2)
            rpg_log_wait();
    rpg_log_clear();
}

static void fmt1(char *o, int n, const char *f, const char *a) { snprintf(o, n, f, a); }

static void display_use_item(RpgBattler *s, int kind, int id)
{
    char t[256];
    if (kind == RPG_KIND_SKILL) {
        const RpgSkill *sk = rpg_skill_data(id);
        snprintf(t, sizeof t, "%s%s", rpg_battler_name(s), RPG_STR(sk->message1));
        rpg_log_add(t);
        if (*RPG_STR(sk->message2)) {
            rpg_log_wait();
            rpg_log_add(RPG_STR(sk->message2));
        }
    } else {
        snprintf(t, sizeof t, rpg_vocab.use_item, rpg_battler_name(s), rpg_item_name(RPG_KIND_ITEM, id));
        rpg_log_add(t);
    }
}

static void display_counter(RpgBattler *t, int kind, int id)
{
    (void)kind; (void)id;
    char s[256];
    rpg_play_system_se(RPG_SE_EVASION);
    fmt1(s, sizeof s, rpg_vocab.counter_attack, rpg_battler_name(t));
    rpg_log_add(s);
    rpg_log_wait();
    rpg_log_back_one();
}

static void display_reflection(RpgBattler *t, int kind, int id)
{
    (void)kind; (void)id;
    char s[256];
    rpg_play_system_se(RPG_SE_REFLECTION);
    fmt1(s, sizeof s, rpg_vocab.magic_reflection, rpg_battler_name(t));
    rpg_log_add(s);
    rpg_log_wait();
    rpg_log_back_one();
}

static void display_substitute(RpgBattler *sub, RpgBattler *t)
{
    char s[256];
    snprintf(s, sizeof s, rpg_vocab.substitute, rpg_battler_name(sub), rpg_battler_name(t));
    rpg_log_add(s);
    rpg_log_wait();
    rpg_log_back_one();
}

static void hp_text(RpgBattler *t, char *o, int n)
{
    const char *nm = rpg_battler_name(t);
    char v[32];
    if (t->result.hp_drain > 0) {
        snprintf(v, sizeof v, "%d", t->result.hp_drain);
        if (t->is_actor) snprintf(o, n, rpg_vocab.actor_drain, nm, rpg_term_basic(2), v);
        else snprintf(o, n, rpg_vocab.enemy_drain, nm, rpg_term_basic(2), v);
    } else if (t->result.hp_damage > 0) {
        snprintf(v, sizeof v, "%d", t->result.hp_damage);
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_damage : rpg_vocab.enemy_damage, nm, v);
    } else if (t->result.hp_damage < 0) {
        snprintf(v, sizeof v, "%d", -t->result.hp_damage);
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_recovery : rpg_vocab.enemy_recovery, nm, rpg_term_basic(2), v);
    } else {
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_no_damage : rpg_vocab.enemy_no_damage, nm);
    }
}

static void mp_text(RpgBattler *t, char *o, int n)
{
    const char *nm = rpg_battler_name(t);
    char v[32];
    o[0] = 0;
    if (t->result.mp_drain > 0) {
        snprintf(v, sizeof v, "%d", t->result.mp_drain);
        if (t->is_actor) snprintf(o, n, rpg_vocab.actor_drain, nm, rpg_term_basic(4), v);
        else snprintf(o, n, rpg_vocab.enemy_drain, nm, rpg_term_basic(4), v);
    } else if (t->result.mp_damage > 0) {
        snprintf(v, sizeof v, "%d", t->result.mp_damage);
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_loss : rpg_vocab.enemy_loss, nm, rpg_term_basic(4), v);
    } else if (t->result.mp_damage < 0) {
        snprintf(v, sizeof v, "%d", -t->result.mp_damage);
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_recovery : rpg_vocab.enemy_recovery, nm, rpg_term_basic(4), v);
    }
}

static void tp_text(RpgBattler *t, char *o, int n)
{
    const char *nm = rpg_battler_name(t);
    char v[32];
    o[0] = 0;
    if (t->result.tp_damage > 0) {
        snprintf(v, sizeof v, "%d", t->result.tp_damage);
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_loss : rpg_vocab.enemy_loss, nm, rpg_term_basic(6), v);
    } else if (t->result.tp_damage < 0) {
        snprintf(v, sizeof v, "%d", -t->result.tp_damage);
        snprintf(o, n, t->is_actor ? rpg_vocab.actor_gain : rpg_vocab.enemy_gain, nm, rpg_term_basic(6), v);
    }
}

static void display_damage(RpgBattler *t, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    char s[256];
    if (t->result.missed) {
        if (!id || u->hit_type == 1) {
            snprintf(s, sizeof s, t->is_actor ? rpg_vocab.actor_no_hit : rpg_vocab.enemy_no_hit, rpg_battler_name(t));
            rpg_play_system_se(RPG_SE_MISS);
        } else {
            snprintf(s, sizeof s, rpg_vocab.action_failure, rpg_battler_name(t));
        }
        rpg_log_add(s);
        rpg_log_wait();
    } else if (t->result.evaded) {
        if (!id || u->hit_type == 1) {
            snprintf(s, sizeof s, rpg_vocab.evasion, rpg_battler_name(t));
            rpg_play_system_se(RPG_SE_EVASION);
        } else {
            snprintf(s, sizeof s, rpg_vocab.magic_evasion, rpg_battler_name(t));
            rpg_play_system_se(RPG_SE_MAGIC_EVASION);
        }
        rpg_log_add(s);
        rpg_log_wait();
    } else {
        int to_hp = u->damage.type == 1 || u->damage.type == 3 || u->damage.type == 5;
        if (!(t->result.hp_damage == 0 && id && !to_hp)) {
            if (t->result.hp_damage > 0 && t->result.hp_drain == 0)
                rpg_battler_damage_effect(t);
            if (t->result.hp_damage < 0)
                rpg_play_system_se(RPG_SE_RECOVERY);
            hp_text(t, s, sizeof s);
            rpg_log_add(s);
            rpg_log_wait();
        }
        if (!rpg_battler_dead(t) && t->result.mp_damage != 0) {
            if (t->result.mp_damage < 0)
                rpg_play_system_se(RPG_SE_RECOVERY);
            mp_text(t, s, sizeof s);
            rpg_log_add(s);
            rpg_log_wait();
        }
        if (!rpg_battler_dead(t) && t->result.tp_damage != 0) {
            if (t->result.tp_damage < 0)
                rpg_play_system_se(RPG_SE_RECOVERY);
            tp_text(t, s, sizeof s);
            rpg_log_add(s);
            rpg_log_wait();
        }
    }
}

static int status_affected(const RpgBattler *t)
{
    return t->result.nadded || t->result.nremoved || t->result.nadded_buffs || t->result.nadded_debuffs ||
           t->result.nremoved_buffs;
}

static void display_affected_status(RpgBattler *t, int kind, int id)
{
    (void)kind; (void)id;
    if (!status_affected(t))
        return;
    char s[256];
    if (rpg_log_lines() < RPG_LOG_LINES)
        rpg_log_add("");
    for (int i = 0; i < t->result.nadded; i++) {
        const RpgState *st = rpg_state_data(t->result.added_states[i]);
        const char *m = RPG_STR(t->is_actor ? st->message1 : st->message2);
        if (st->id == 1)
            rpg_battler_collapse_effect(t);
        if (!*m)
            continue;
        snprintf(s, sizeof s, "%s%s", rpg_battler_name(t), m);
        rpg_log_replace(s);
        rpg_log_wait();
        rpg_bwait_for_effect();
    }
    for (int i = 0; i < t->result.nremoved; i++) {
        const RpgState *st = rpg_state_data(t->result.removed_states[i]);
        const char *m = RPG_STR(st->message4);
        if (!*m)
            continue;
        snprintf(s, sizeof s, "%s%s", rpg_battler_name(t), m);
        rpg_log_replace(s);
        rpg_log_wait();
    }
    const int *lists[3] = {t->result.added_buffs, t->result.added_debuffs, t->result.removed_buffs};
    const int ns[3] = {t->result.nadded_buffs, t->result.nadded_debuffs, t->result.nremoved_buffs};
    const char *f[3] = {rpg_vocab.buff_add, rpg_vocab.debuff_add, rpg_vocab.buff_remove};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < ns[k]; i++) {
            snprintf(s, sizeof s, f[k], rpg_battler_name(t), rpg_term_param(lists[k][i]));
            rpg_log_replace(s);
            rpg_log_wait();
        }
    const char *last = rpg_log_last();
    if (last && !*last)
        rpg_log_back_one();
}

static void display_auto_affected_status(RpgBattler *t)
{
    if (status_affected(t)) {
        rpg_bvt.display_affected_status(t, 0, 0);
        if (rpg_log_lines() > 0)
            rpg_log_wait();
    }
}

static void display_current_state(RpgBattler *s)
{
    for (int i = 0; i < s->nstates; i++) {
        const char *m = RPG_STR(rpg_state_data(s->states[i].id)->message3);
        if (*m) {
            char t[256];
            snprintf(t, sizeof t, "%s%s", rpg_battler_name(s), m);
            rpg_log_add(t);
            rpg_log_wait();
            return;
        }
    }
}

static void display_critical(RpgBattler *t, int kind, int id)
{
    (void)kind; (void)id;
    if (t->result.critical) {
        rpg_log_add(t->is_actor ? rpg_vocab.critical_to_actor : rpg_vocab.critical_to_enemy);
        rpg_log_wait();
    }
}

static void display_action_results(RpgBattler *t, int kind, int id)
{
    if (!t->result.used)
        return;
    int last = rpg_log_lines();
    rpg_bvt.display_critical(t, kind, id);
    rpg_bvt.display_damage(t, kind, id);
    rpg_bvt.display_affected_status(t, kind, id);
    if (rpg_battler_result_hit(t) && !t->result.success) {
        char s[256];
        snprintf(s, sizeof s, rpg_vocab.action_failure, rpg_battler_name(t));
        rpg_log_add(s);
        rpg_log_wait();
    }
    if (rpg_log_lines() > last)
        rpg_log_wait();
    rpg_log_back_to(last);
}

/* ------------------------------------------------------------------ sprites */
RpgBattlerSprite *rpg_battler_sprite(const RpgBattler *b)
{
    if (b->is_actor)
        return &actor_sprites[((const RpgActorState *)b)->actor_id % RPG_MAX_ACTORS];
    return &enemy_sprites[((const RpgEnemyState *)b)->b.index % RPG_MAX_ENEMIES];
}

static int battler_image(const RpgBattler *b)
{
    if (b->is_actor)
        return -1;
    const RpgEnemy *d = rpg_enemy_data(((const RpgEnemyState *)b)->enemy_id);
    return rpg_image_hue("battlers", RPG_STR(d->battler_name), d->battler_hue);
}

static void start_effect(RpgBattler *b, RpgBattlerSprite *s, int e)
{
    s->effect = e;
    switch (e) {
    case BE_APPEAR: s->duration = 16; s->visible = 1; break;
    case BE_DISAPPEAR: s->duration = 32; s->visible = 0; break;
    case BE_WHITEN: s->duration = 16; s->visible = 1; break;
    case BE_BLINK: s->duration = 20; s->visible = 1; break;
    case BE_COLLAPSE: s->duration = 48; s->visible = 0; break;
    case BE_BOSS_COLLAPSE: s->duration = rpg_image_h(battler_image(b)); s->visible = 0; break;
    case BE_INSTANT_COLLAPSE: s->duration = 16; s->visible = 0; break;
    }
    s->blend = 0;
    memset(s->color, 0, sizeof s->color);
    s->opacity = 255;
    s->ox_shift = 0;
    s->src_y = 0;
}

static void sprite_update(RpgBattler *b, RpgBattlerSprite *s)
{
    /* setup_new_effect */
    if (!s->visible && rpg_battler_alive(b))
        start_effect(b, s, BE_APPEAR);
    else if (s->visible && b->hidden)
        start_effect(b, s, BE_DISAPPEAR);
    if (s->visible && b->sprite_effect) {
        static const int map[] = {BE_NONE, BE_BLINK, BE_COLLAPSE, BE_BOSS_COLLAPSE, BE_INSTANT_COLLAPSE, BE_WHITEN};
        if (b->sprite_effect > 0 && b->sprite_effect < 6)
            start_effect(b, s, map[b->sprite_effect]);
        b->sprite_effect = 0;
    }
    /* setup_new_animation */
    if (b->animation_id > 0) {
        float x, y, h;
        if (b->is_actor) {
            /* front view: actors have no sprite; their animations play over the status window position */
            x = rpg_screen_w / 2;
            y = rpg_screen_h;
            h = 0;
        } else {
            RpgEnemyState *e = (RpgEnemyState *)b;
            x = rpg_bvt.enemy_screen_x(e);
            y = rpg_bvt.enemy_screen_y(e);
            h = rpg_image_h(battler_image(b));
        }
        rpg_anim_start(b->animation_id, x, y, h, b->animation_mirror, b);
        b->animation_id = 0;
    }
    if (s->duration > 0) {
        s->duration--;
        int d = s->duration;
        switch (s->effect) {
        case BE_WHITEN:
            s->color[0] = s->color[1] = s->color[2] = 255;
            s->color[3] = 128 - (16 - d) * 10;
            break;
        case BE_BLINK: s->opacity = (d % 10 < 5) ? 255 : 0; break;
        case BE_APPEAR: s->opacity = (16 - d) * 16; break;
        case BE_DISAPPEAR: s->opacity = 256 - (32 - d) * 10; break;
        case BE_COLLAPSE:
            s->blend = 1;
            s->color[0] = 255; s->color[1] = 128; s->color[2] = 128; s->color[3] = 128;
            s->opacity = 256 - (48 - d) * 6;
            break;
        case BE_BOSS_COLLAPSE: {
            int bh = rpg_image_h(battler_image(b));
            int alpha = bh ? d * 120 / bh : 0;
            s->ox_shift = d % 2 * 4 - 2;
            s->blend = 1;
            s->color[0] = s->color[1] = s->color[2] = 255;
            s->color[3] = 255 - alpha;
            s->opacity = alpha;
            s->src_y--;
            if (d % 20 == 19)
                rpg_play_system_se(RPG_SE_BOSS_COLLAPSE2);
            break;
        }
        case BE_INSTANT_COLLAPSE: s->opacity = 0; break;
        }
        if (s->duration == 0)
            s->effect = BE_NONE;
    }
}

static void init_sprites(void)
{
    memset(enemy_sprites, 0, sizeof enemy_sprites);
    memset(actor_sprites, 0, sizeof actor_sprites);
    for (int i = 0; i < rpg_b.nenemies; i++) {
        RpgBattlerSprite *s = &enemy_sprites[i];
        s->visible = rpg_battler_alive(&rpg_b.enemies[i].b);
        s->opacity = s->visible ? 255 : 0;
        s->zoom_x = s->zoom_y = 1;
    }
}

float rpg_battleback_oy(void) { return (rpg_screen_h - 480) / 2.f; }
static float enemy_screen_y(const RpgEnemyState *e) { return e->screen_y + rpg_battleback_oy(); }
static float enemy_screen_x(const RpgEnemyState *e) { return e->screen_x; }

static void draw_battler(RpgBattler *b, RpgBattlerSprite *s)
{
    int img = battler_image(b);
    if (img < 0 || s->opacity <= 0)
        return;
    if (!s->visible && s->effect == BE_NONE)
        return;
    RpgEnemyState *e = (RpgEnemyState *)b;
    int w = rpg_image_w(img), h = rpg_image_h(img);
    float zx = s->zoom_x ? s->zoom_x : 1, zy = s->zoom_y ? s->zoom_y : 1;
    float x = rpg_bvt.enemy_screen_x(e) + rpg_battle_screen.shake, y = rpg_bvt.enemy_screen_y(e);
    int sy = 0, sh = h;
    if (s->src_y < 0) {   /* boss collapse: the picture sinks */
        sh = h + s->src_y;
        if (sh <= 0)
            return;
    }
    float dx = x - (w / 2.f + s->ox_shift) * zx, dy = y - h * zy;
    float zxx = s->mirror ? -zx : zx;
    if (s->mirror)
        dx += w * zx;
    uint32_t col = rpg_rgba(255, 255, 255, s->opacity);
    rpg_draw_part(img, 0, sy, w, sh, dx, dy - s->src_y * zy, zxx, zy, 0, col, s->blend);
    float fl[4];
    rpg_anim_target_flash(b, fl);
    float a = s->color[3];
    float c[3] = {s->color[0], s->color[1], s->color[2]};
    if (fl[3] > a) {
        a = fl[3];
        c[0] = fl[0];
        c[1] = fl[1];
        c[2] = fl[2];
    }
    if (a > 0) {
        /* Sprite#color: the picture blended towards a colour, drawn as a tinted silhouette over it */
        vcpe_spr_flash(1);
        rpg_draw_part(img, 0, sy, w, sh, dx, dy - s->src_y * zy, zxx, zy, 0,
                      rpg_rgba((int)c[0], (int)c[1], (int)c[2], (int)(a * s->opacity / 255)), s->blend);
        vcpe_spr_flash(0);
    }
}

static void draw_back(void)
{
    /* the battlebacks (map's or specified): centred, at the project's 676x516 stretch is the game's choice */
    const char *b1 = rpg.battleback1[0] ? rpg.battleback1 : 0, *b2 = rpg.battleback2[0] ? rpg.battleback2 : 0;
    float oy = rpg_battleback_oy();
    int i1 = b1 ? rpg_image("battlebacks1", b1) : -1, i2 = b2 ? rpg_image("battlebacks2", b2) : -1;
    float sh = rpg_battle_screen.shake;
    if (i1 >= 0) {
        int w = rpg_image_w(i1), h = rpg_image_h(i1);
        rpg_draw_part(i1, 0, 0, w, h, 320 - w / 2.f + sh, 240 - h / 2.f + oy, 1, 1, 0, 0xffffffff, 0);
    } else {
        rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, 0xff000000);
    }
    if (i2 >= 0) {
        int w = rpg_image_w(i2), h = rpg_image_h(i2);
        rpg_draw_part(i2, 0, 0, w, h, 320 - w / 2.f + sh, 240 - h / 2.f + oy, 1, 1, 0, 0xffffffff, 0);
    }
}

static void draw_battlers(void)
{
    /* by z (Sprite_Battler: screen_z 100 for enemies; later ones drawn later) */
    for (int i = rpg_b.nenemies - 1; i >= 0; i--)
        rpg_bvt.draw_battler(&rpg_b.enemies[i].b, &enemy_sprites[i]);
}

/* ------------------------------------------------------------------ windows */
static void make_actor_commands(RpgActorState *a)
{
    RpgBattleWindows *w = &rpg_bw;
    w->ncmds = 0;
    w->cmds[w->ncmds] = RPG_BCMD_ATTACK;
    w->cmd_ext[w->ncmds++] = 0;
    int nst = rpg_system()->skill_types.count;
    for (int t = 1; t < nst && w->ncmds < 12; t++)
        if (rpg_features_set_has(&a->b, F_STYPE_ADD, t)) {
            w->cmds[w->ncmds] = RPG_BCMD_SKILL;
            w->cmd_ext[w->ncmds++] = t;
        }
    w->cmds[w->ncmds] = RPG_BCMD_GUARD;
    w->cmd_ext[w->ncmds++] = 0;
    w->cmds[w->ncmds] = RPG_BCMD_ITEM;
    w->cmd_ext[w->ncmds++] = 0;
}

static int actor_cmd_enabled(RpgWin *win, int i)
{
    (void)win;
    RpgActorState *a = rpg_bw.cmd_actor;
    switch (rpg_bw.cmds[i]) {
    case RPG_BCMD_ATTACK: return a && rpg_battler_usable(&a->b, RPG_KIND_SKILL, rpg_battler_attack_skill(&a->b));
    case RPG_BCMD_GUARD: return a && rpg_battler_usable(&a->b, RPG_KIND_SKILL, rpg_battler_guard_skill(&a->b));
    case RPG_BCMD_ESCAPE: return rpg_b.can_escape;
    }
    return 1;
}

static const char *cmd_name(int cmd, int ext)
{
    if (rpg_battle_command_name_hook) {
        const char *n = rpg_battle_command_name_hook(cmd, ext);
        if (n)
            return n;
    }
    switch (cmd) {
    case RPG_BCMD_ATTACK: return rpg_term_command(2);
    case RPG_BCMD_SKILL: {
        const int32_t *s = rpg_at(&rpg_db, RPG_T_STR, rpg_system()->skill_types, ext);
        return s ? RPG_STR(*s) : "";
    }
    case RPG_BCMD_GUARD: return rpg_term_command(3);
    case RPG_BCMD_ITEM: return rpg_term_command(4);
    case RPG_BCMD_ESCAPE: return rpg_term_command(1);
    }
    return "";
}

static int skill_enabled(RpgWin *w, int i)
{
    (void)w;
    RpgActorState *a = rpg_bw.cmd_actor;
    return a && i < rpg_bw.nskills && rpg_battler_usable(&a->b, RPG_KIND_SKILL, rpg_bw.skills[i]);
}

static int item_enabled(RpgWin *w, int i)
{
    (void)w;
    return i < rpg_bw.nitems && rpg_party_usable(RPG_KIND_ITEM, rpg_bw.items[i]);
}

static void refresh_skills(void)
{
    RpgActorState *a = rpg_bw.cmd_actor;
    rpg_bw.nskills = 0;
    if (a) {
        int all[RPG_MAX_SKILLS / 4], n = rpg_actor_skills(a, all, RPG_MAX_SKILLS / 4);
        for (int i = 0; i < n && rpg_bw.nskills < 512; i++)
            if (rpg_skill_data(all[i])->stype_id == rpg_bw.skill_stype)
                rpg_bw.skills[rpg_bw.nskills++] = all[i];
    }
    rpg_bw.skill.item_max = rpg_bw.nskills;
}

static void refresh_items(void)
{
    rpg_bw.nitems = 0;
    for (int id = 1; id < rpg_count(&rpg_db, RPG_T_ITEM) && id < RPG_MAX_ITEMS && rpg_bw.nitems < 512; id++)
        if (rpg_party_item_number(RPG_KIND_ITEM, id) > 0 && rpg_party_usable(RPG_KIND_ITEM, id))
            rpg_bw.items[rpg_bw.nitems++] = id;
    rpg_bw.item.item_max = rpg_bw.nitems;
}

static void create_windows(void)
{
    RpgBattleWindows *w = &rpg_bw;
    memset(w, 0, sizeof *w);
    float sh = rpg_screen_h, sw = rpg_screen_w;
    rpg_win_init(&w->log, 0, 0, sw, rpg_fitting_height(RPG_LOG_LINES));
    w->log.opacity = 0;
    float st_h = rpg_fitting_height(4);
    w->info_y = sh - st_h;
    rpg_win_init(&w->status, 128, w->info_y, sw - 128, st_h);
    w->status.openness = 0;
    rpg_win_init(&w->party_cmd, 0, w->info_y, 128, st_h);
    w->party_cmd.openness = 0;
    w->party_cmd.item_max = 2;
    rpg_win_init(&w->actor_cmd, sw, w->info_y, 128, st_h);
    w->actor_cmd.openness = 0;
    w->actor_cmd.enabled = actor_cmd_enabled;
    rpg_win_init(&w->help, 0, 0, sw, rpg_fitting_height(2));
    w->help.visible = 0;
    rpg_win_init(&w->skill, 0, w->help.h, sw, w->info_y - w->help.h);
    w->skill.col_max = 2;
    w->skill.visible = 0;
    w->skill.enabled = skill_enabled;
    rpg_win_init(&w->item, 0, w->help.h, sw, w->info_y - w->help.h);
    w->item.col_max = 2;
    w->item.visible = 0;
    w->item.enabled = item_enabled;
    rpg_win_init(&w->actor_sel, 0, w->info_y, sw - 128, st_h);
    w->actor_sel.visible = 0;
    rpg_win_init(&w->enemy_sel, 0, w->info_y, sw - 128, st_h);
    w->enemy_sel.col_max = 2;
    w->enemy_sel.visible = 0;
    w->info_ox = 64;
}

static void refresh_status_default(void)
{
    int ids[RPG_MAX_PARTY];
    rpg_bw.status.item_max = rpg_party_battle_members(ids);
}
void rpg_battle_refresh_status(void) { rpg_bvt.refresh_status(); }

/* the info viewport slides between the party command (0), the actor command (128) and the turn (64) */
static void update_info_viewport(void)
{
    float target = rpg_bw.info_ox;
    if (rpg_bw.party_cmd.active) target = 0;
    if (rpg_bw.actor_cmd.active) target = 128;
    if (rpg_b.phase == BP_TURN) target = 64;
    if (rpg_bw.info_ox < target) rpg_bw.info_ox = rpg_bw.info_ox + 16 < target ? rpg_bw.info_ox + 16 : target;
    if (rpg_bw.info_ox > target) rpg_bw.info_ox = rpg_bw.info_ox - 16 > target ? rpg_bw.info_ox - 16 : target;
}

/* ------------------------------------------------------------------ BattleManager */
void rpg_battle_setup(int troop, int can_escape, int can_lose)
{
    memset(&rpg_b, 0, sizeof rpg_b);
    rpg_b.phase = BP_INIT;
    rpg_b.actor_index = -1;
    troop_setup(troop);
    rpg_b.can_escape = can_escape;
    rpg_b.can_lose = can_lose;
    rpg_b.escape_ratio = 1.5f - 1.0f * rpg_unit_agi(1) / (rpg_party_agi() ? rpg_party_agi() : 1);
}

void rpg_battle_event_branch(int8_t *slot) { rpg_b.event_branch = slot; }

static void on_encounter(void)
{
    int tagi = rpg_unit_agi(1), pagi = rpg_party_agi();
    double pre = (pagi >= tagi ? 0.05 : 0.03) * (rpg_party_ability(3) ? 4 : 1);
    double sur = rpg_party_ability(2) ? 0 : (pagi >= tagi ? 0.03 : 0.05);
    rpg_b.preemptive = rpg_randf() < pre;
    rpg_b.surprise = rpg_randf() < sur && !rpg_b.preemptive;
}

/* Scene_Map#update_encounter + Game_Player#encounter */
int rpg_battle_encounter_start(void)
{
    if (rpg_interp_running(&rpg.interp) || rpg.encounter_disabled || rpg.player.encounter_count > 0)
        return 0;
    rpg_player_make_encounter_count();
    int troop = rpg_player_encounter_troop();
    if (troop <= 0 || !rpg_troop_data(troop)->id)
        return 0;
    rpg_battle_setup(troop, 1, 0);
    on_encounter();
    rpg_temp.battle_from_event = 0;
    rpg_scene_call(RPG_SCENE_BATTLE);
    return 1;
}

static const char *party_name(char *o, int n)
{
    int ids[RPG_MAX_PARTY], k = rpg_party_battle_members(ids);
    if (k == 0) o[0] = 0;
    else if (k == 1) snprintf(o, n, "%s", rpg_actor(ids[0])->name);
    else snprintf(o, n, rpg_vocab.party_name, rpg_actor(ids[0])->name);
    return o;
}

static void bm_battle_start(void)
{
    rpg.battle_count++;
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(0, m);
    for (int i = 0; i < n; i++)
        rpg_battler_on_battle_start(m[i]);
    rpg.party.in_battle = 1;
    for (int i = 0; i < rpg_b.nenemies; i++)
        rpg_battler_on_battle_start(&rpg_b.enemies[i].b);
    /* enemy names that appear (alive, each once) */
    char seen[RPG_MAX_ENEMIES][RPG_NAME];
    int ns = 0;
    for (int i = 0; i < rpg_b.nenemies; i++) {
        RpgEnemyState *e = &rpg_b.enemies[i];
        if (!rpg_battler_alive(&e->b))
            continue;
        int dup = 0;
        for (int k = 0; k < ns; k++)
            dup |= !strcmp(seen[k], original_name(e));
        if (dup)
            continue;
        snprintf(seen[ns++], RPG_NAME, "%s", original_name(e));
        char t[256];
        snprintf(t, sizeof t, rpg_vocab.emerge, original_name(e));
        rpg_msg_add(t);
    }
    char pn[RPG_NAME * 2], t[256];
    if (rpg_b.preemptive) {
        snprintf(t, sizeof t, rpg_vocab.preemptive, party_name(pn, sizeof pn));
        rpg_msg_add(t);
    } else if (rpg_b.surprise) {
        snprintf(t, sizeof t, rpg_vocab.surprise, party_name(pn, sizeof pn));
        rpg_msg_add(t);
    }
    rpg_bwait_for_message();
}

void rpg_battle_abort(void) { rpg_b.phase = BP_ABORTING; }

static void replay_bgm_and_bgs(void)
{
    rpg_bgm_play_at(rpg_b.map_bgm.name, rpg_b.map_bgm.volume, rpg_b.map_bgm.pitch, rpg_b.map_bgm_pos);
    rpg_bgs_play(rpg_b.map_bgs.name, rpg_b.map_bgs.volume, rpg_b.map_bgs.pitch);
}

static void battle_end_me(void)
{
    const RpgSystem *s = rpg_system();
    if (rpg.has_battle_end_me)
        rpg_me_play(rpg.battle_end_me.name, rpg.battle_end_me.volume, rpg.battle_end_me.pitch);
    else
        rpg_me_play(RPG_STR(s->battle_end_me.name), s->battle_end_me.volume, s->battle_end_me.pitch);
}

static void bm_display_exp(void)
{
    int e = rpg_troop_exp_total();
    if (e > 0) {
        char t[256], v[32];
        snprintf(v, sizeof v, "%d", e);
        snprintf(t, sizeof t, "\\.%s", "");
        char u[256];
        snprintf(u, sizeof u, rpg_vocab.obtain_exp, v);
        snprintf(t, sizeof t, "\\.%s", u);
        rpg_msg_add(t);
    }
}

static void bm_gain_gold(void)
{
    int g = rpg_troop_gold_total();
    if (g > 0) {
        char t[256], v[32], u[256];
        snprintf(v, sizeof v, "%d", g);
        snprintf(u, sizeof u, rpg_vocab.obtain_gold, v);
        snprintf(t, sizeof t, "\\.%s", u);
        rpg_msg_add(t);
        rpg_party_gain_gold(g);
    }
    rpg_bwait_for_message();
}

static void bm_gain_drop_items(void)
{
    int kinds[64], ids[64], n = rpg_troop_drop_items(kinds, ids, 64);
    for (int i = 0; i < n; i++) {
        rpg_party_gain_item(kinds[i], ids[i], 1, 0);
        char t[256];
        snprintf(t, sizeof t, rpg_vocab.obtain_item, rpg_item_name(kinds[i], ids[i]));
        rpg_msg_add(t);
    }
    rpg_bwait_for_message();
}

static void level_up_message(RpgActorState *a, const int *news, int n)
{
    char t[256], lv[16];
    snprintf(lv, sizeof lv, "%d", a->level);
    snprintf(t, sizeof t, rpg_vocab.level_up, a->name, rpg_term_basic(0), lv);
    /* $game_message.new_page: a form feed in the text */
    if (rpg_msg.ntexts) {
        char ff[260];
        snprintf(ff, sizeof ff, "\f%s", t);
        rpg_msg_add(ff);
    } else {
        rpg_msg_add(t);
    }
    for (int i = 0; i < n; i++) {
        snprintf(t, sizeof t, rpg_vocab.obtain_skill, RPG_STR(rpg_skill_data(news[i])->name));
        rpg_msg_add(t);
    }
}

static void bm_gain_exp(void)
{
    int e = rpg_troop_exp_total();
    for (int i = 0; i < rpg.party.nmembers; i++)
        rpg_actor_gain_exp(rpg_actor(rpg.party.members[i]), e);
    rpg_bwait_for_message();
}

static void bm_battle_end(int result)
{
    rpg_log("RPG battle %d ended: %s after %d turns", rpg_b.troop_id, result == 0 ? "victory" : result == 1 ? "escape" : "defeat",
            rpg_b.turn_count);
    rpg_b.phase = BP_NONE;
    rpg_b.result = result;
    if (rpg_b.event_branch)
        *rpg_b.event_branch = result;
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(0, m);
    rpg.party.in_battle = 0;
    for (int i = 0; i < n; i++)
        rpg_battler_on_battle_end(m[i]);
    for (int i = 0; i < rpg_b.nenemies; i++)
        rpg_battler_on_battle_end(&rpg_b.enemies[i].b);
}

static int bm_process_victory(void)
{
    battle_end_me();
    replay_bgm_and_bgs();
    char pn[RPG_NAME * 2], t[256];
    snprintf(t, sizeof t, rpg_vocab.victory, party_name(pn, sizeof pn));
    rpg_msg_add(t);
    rpg_bvt.display_exp();
    rpg_bvt.gain_gold();
    rpg_bvt.gain_drop_items();
    rpg_bvt.gain_exp();
    rpg_scene_return();
    rpg_bvt.battle_end(0);
    return 1;
}

static int bm_process_abort(void)
{
    replay_bgm_and_bgs();
    rpg_scene_return();
    rpg_bvt.battle_end(1);
    return 1;
}

static int bm_process_escape(void)
{
    char pn[RPG_NAME * 2], t[256];
    snprintf(t, sizeof t, rpg_vocab.escape_start, party_name(pn, sizeof pn));
    rpg_msg_add(t);
    int ok = rpg_b.preemptive ? 1 : rpg_randf() < rpg_b.escape_ratio;
    rpg_play_system_se(RPG_SE_ESCAPE);
    if (ok) {
        rpg_bvt.process_abort();
    } else {
        rpg_b.escape_ratio += 0.1f;
        snprintf(t, sizeof t, "\\.%s", rpg_vocab.escape_failure);
        rpg_msg_add(t);
        RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
        int n = rpg_unit_members(0, m);
        for (int i = 0; i < n; i++)
            rpg_battler_clear_actions(m[i]);
    }
    rpg_bwait_for_message();
    return ok;
}

static int bm_process_defeat(void)
{
    char pn[RPG_NAME * 2], t[256];
    snprintf(t, sizeof t, rpg_vocab.defeat, party_name(pn, sizeof pn));
    rpg_msg_add(t);
    rpg_bwait_for_message();
    if (rpg_b.can_lose) {
        int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
        for (int i = 0; i < n; i++)
            if (rpg_battler_dead(&rpg_actor(ids[i])->b))
                rpg_battler_set_hp(&rpg_actor(ids[i])->b, 1);
        replay_bgm_and_bgs();
        rpg_scene_return();
    } else {
        rpg_scene_goto(RPG_SCENE_GAMEOVER);
    }
    rpg_bvt.battle_end(2);
    return 1;
}

static int bm_judge_win_loss(void)
{
    if (rpg_b.phase != BP_NONE) {
        if (rpg.party.nmembers == 0)
            return rpg_bvt.process_abort();
        if (rpg_party_all_dead())
            return rpg_bvt.process_defeat();
        if (rpg_unit_all_dead(1))
            return rpg_bvt.process_victory();
        if (rpg_b.phase == BP_ABORTING)
            return rpg_bvt.process_abort();
    }
    return 0;
}

static RpgActorState *bm_actor(void)
{
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
    return rpg_b.actor_index >= 0 && rpg_b.actor_index < n ? rpg_actor(ids[rpg_b.actor_index]) : 0;
}
RpgActorState *rpg_battle_actor(void) { return bm_actor(); }

static int bm_input_start(void)
{
    if (rpg_b.phase != BP_INPUT) {
        rpg_b.phase = BP_INPUT;
        RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
        int n = rpg_unit_members(0, m);
        for (int i = 0; i < n; i++)
            rpg_battler_make_actions(m[i]);
        for (int i = 0; i < rpg_b.nenemies; i++)
            rpg_battler_make_actions(&rpg_b.enemies[i].b);
        rpg_b.actor_index = -1;
    }
    return !rpg_b.surprise && rpg_party_inputable();
}

static int bm_next_command(void)
{
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
    do {
        RpgActorState *a = bm_actor();
        if (!a || !rpg_actor_next_command(a)) {
            rpg_b.actor_index++;
            if (rpg_b.actor_index >= n)
                return 0;
        }
    } while (!rpg_battler_inputable(&bm_actor()->b));
    return 1;
}

static int bm_prior_command(void)
{
    do {
        RpgActorState *a = bm_actor();
        if (!a || !rpg_actor_prior_command(a)) {
            rpg_b.actor_index--;
            if (rpg_b.actor_index < 0)
                return 0;
        }
    } while (!rpg_battler_inputable(&bm_actor()->b));
    return 1;
}

static void bm_make_action_orders(void)
{
    rpg_b.naction_battlers = 0;
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    if (!rpg_b.surprise) {
        int n = rpg_unit_members(0, m);
        for (int i = 0; i < n; i++)
            rpg_b.action_battlers[rpg_b.naction_battlers++] = m[i];
    }
    if (!rpg_b.preemptive)
        for (int i = 0; i < rpg_b.nenemies; i++)
            rpg_b.action_battlers[rpg_b.naction_battlers++] = &rpg_b.enemies[i].b;
    for (int i = 0; i < rpg_b.naction_battlers; i++)
        rpg_battler_make_speed(rpg_b.action_battlers[i]);
    /* sort by speed, fastest first (Ruby's sort! is not stable; insertion keeps equal speeds in order) */
    for (int i = 1; i < rpg_b.naction_battlers; i++) {
        RpgBattler *x = rpg_b.action_battlers[i];
        int j = i - 1;
        while (j >= 0 && rpg_b.action_battlers[j]->speed < x->speed) {
            rpg_b.action_battlers[j + 1] = rpg_b.action_battlers[j];
            j--;
        }
        rpg_b.action_battlers[j + 1] = x;
    }
}

static void bm_turn_start(void)
{
    rpg_b.phase = BP_TURN;
    rpg_b.actor_index = -1;
    rpg_troop_increase_turn();
    rpg_bvt.make_action_orders();
}

static void bm_turn_end(void)
{
    rpg_b.phase = BP_TURN_END;
    rpg_b.preemptive = 0;
    rpg_b.surprise = 0;
}

static RpgBattler *bm_next_subject(void)
{
    while (rpg_b.naction_battlers > 0) {
        RpgBattler *b = rpg_b.action_battlers[0];
        memmove(&rpg_b.action_battlers[0], &rpg_b.action_battlers[1], sizeof(RpgBattler *) * (rpg_b.naction_battlers - 1));
        rpg_b.naction_battlers--;
        if (rpg_battler_index(b) < 0 || !rpg_battler_alive(b))
            continue;
        return b;
    }
    return 0;
}

int rpg_battle_force_action(int kind, int index, int skill, int target)
{
    RpgBattler *list[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = 0;
    if (!rpg.party.in_battle)
        return 0;
    if (kind == 0) {
        if (index < 0) {
            for (int i = 0; i < rpg_b.nenemies; i++)
                list[n++] = &rpg_b.enemies[i].b;
        } else if (index < rpg_b.nenemies) {
            list[n++] = &rpg_b.enemies[index].b;
        }
    } else {
        if (index == 0) {
            n = rpg_unit_members(0, list);
        } else if (index > 0 && index < RPG_MAX_ACTORS && rpg_actor_data(index)->id) {
            list[n++] = &rpg_actor(index)->b;
        }
    }
    for (int i = 0; i < n; i++) {
        if (rpg_battler_state(list[i], 1))
            continue;
        rpg_battler_force_action(list[i], skill, target);
        /* BattleManager.force_action */
        rpg_b.action_forced = list[i];
        for (int k = 0; k < rpg_b.naction_battlers; k++)
            if (rpg_b.action_battlers[k] == list[i]) {
                memmove(&rpg_b.action_battlers[k], &rpg_b.action_battlers[k + 1],
                        sizeof(RpgBattler *) * (rpg_b.naction_battlers - k - 1));
                rpg_b.naction_battlers--;
                break;
            }
        if (rpg_hooks_force_action_nowait)
            continue;
        return 1;   /* the interpreter waits until the forced action ran, then the next battler */
    }
    return 0;
}
int rpg_battle_action_forced(void) { return rpg_b.action_forced != 0; }

/* ------------------------------------------------------------------ Scene_Battle */
static RpgWin *selecting;   /* the window select_loop updates itself */
int rpg_battle_input_ended(void) { return rpg_scene_changing() || rpg_b.input_abort; }
static void sb_update_basic(void)
{
    vcpe_co_wait_frame();   /* Graphics.update: this frame is shown, input is read for the next */
    if (rpg.timer_working && rpg.timer_count > 0 && --rpg.timer_count == 0)
        rpg.timer_working = 0;
    rpg_screen_update(&rpg_battle_screen);
    rpg_msg_update();
    rpg_win_update(&rpg_bw.log);
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(0, m);
    for (int i = 0; i < n; i++)
        rpg_bvt.sprite_update(m[i], rpg_battler_sprite(m[i]));
    for (int i = 0; i < rpg_b.nenemies; i++)
        rpg_bvt.sprite_update(&rpg_b.enemies[i].b, &enemy_sprites[i]);
    rpg_anims_update();
    update_info_viewport();
    /* update_message_open: windows close while a message shows */
    if (rpg_msg_busy() && !rpg_win_is_closed(&rpg_bw.status)) {
        rpg_win_close(&rpg_bw.status);
        rpg_win_close(&rpg_bw.party_cmd);
        rpg_win_close(&rpg_bw.actor_cmd);
    }
    /* the windows' own open / close (input is handled by the selection loops) */
    RpgWin *ws[] = {&rpg_bw.status, &rpg_bw.party_cmd, &rpg_bw.actor_cmd, &rpg_bw.help, &rpg_bw.skill, &rpg_bw.item,
                    &rpg_bw.actor_sel, &rpg_bw.enemy_sel};
    for (unsigned i = 0; i < sizeof ws / sizeof *ws; i++) {
        if (ws[i] == selecting)
            continue;
        int act = ws[i]->active;
        ws[i]->active = 0;
        rpg_win_update(ws[i]);
        ws[i]->active = act;
    }
}

/* run a selectable window until it is decided: returns RPG_WIN_OK / CANCEL / PAGEUP / PAGEDOWN. The window is
 * updated here (with its input), the others by update_basic */
static int select_loop(RpgWin *w)
{
    if (rpg_battle_input_ended())
        return RPG_WIN_CANCEL;
    w->active = 1;
    for (;;) {
        selecting = w;
        rpg_bupdate_for_wait();
        int r = rpg_win_update(w);
        selecting = 0;
        if (rpg_battle_input_ended())
            return RPG_WIN_CANCEL;
        if (r != RPG_WIN_NONE)
            return r;
    }
}
int rpg_battle_select(RpgWin *w) { return select_loop(w); }

static void sb_start_actor_command_selection(void);
static void sb_turn_start(void);

static void select_enemy(int *ok)
{
    RpgWin *w = &rpg_bw.enemy_sel;
    RpgBattler *alive[RPG_MAX_ENEMIES];
    int n = rpg_unit_alive(1, alive);
    w->item_max = n;
    w->x = rpg_screen_w - w->w;
    rpg_win_select(w, 0);
    w->visible = 1;
    int r = select_loop(w);
    w->visible = 0;
    if (r == RPG_WIN_OK && w->index >= 0 && w->index < n) {
        RpgActorState *a = bm_actor();
        a->b.actions[a->action_input_index].target = rpg_battler_index(alive[w->index]);
        *ok = 1;
    } else {
        *ok = 0;
    }
}

static void select_actor(int *ok)
{
    RpgWin *w = &rpg_bw.actor_sel;
    int ids[RPG_MAX_PARTY];
    w->item_max = rpg_party_battle_members(ids);
    w->x = rpg_screen_w - w->w;
    rpg_win_select(w, 0);
    w->visible = 1;
    int r = select_loop(w);
    w->visible = 0;
    if (r == RPG_WIN_OK) {
        RpgActorState *a = bm_actor();
        a->b.actions[a->action_input_index].target = w->index;
        *ok = 1;
    } else {
        *ok = 0;
    }
}

static void sb_scene_next_command(void)
{
    if (rpg_battle_input_ended())
        return;
    if (rpg_bvt.next_command())
        rpg_bvt.start_actor_command_selection();
    else
        rpg_bvt.turn_start();
}

static void sb_scene_prior_command(void)
{
    if (rpg_battle_input_ended())
        return;
    if (rpg_bvt.prior_command())
        rpg_bvt.start_actor_command_selection();
    else
        rpg_bvt.start_party_command_selection();
}

static void sb_command_attack(void)
{
    RpgActorState *a = bm_actor();
    rpg_action_set_skill(&a->b, a->action_input_index, rpg_battler_attack_skill(&a->b));
    for (;;) {
        int ok;
        select_enemy(&ok);
        if (ok) {
            rpg_bvt.scene_next_command();
            return;
        }
        /* on_enemy_cancel: back to the actor commands */
        if (select_loop(&rpg_bw.actor_cmd) != RPG_WIN_OK) {
            rpg_bvt.scene_prior_command();
            return;
        }
        int c = rpg_bw.cmds[rpg_bw.actor_cmd.index];
        if (c != RPG_BCMD_ATTACK) {
            rpg_bw.actor_cmd.active = 0;
            switch (c) {
            case RPG_BCMD_SKILL: rpg_bvt.command_skill(); return;
            case RPG_BCMD_GUARD: rpg_bvt.command_guard(); return;
            case RPG_BCMD_ITEM: rpg_bvt.command_item(); return;
            case RPG_BCMD_ESCAPE: rpg_bvt.command_escape(); return;
            default: rpg_bvt.command_game(c); return;
            }
        }
        rpg_action_set_skill(&a->b, a->action_input_index, rpg_battler_attack_skill(&a->b));
    }
}

static void choose_from_list(RpgWin *w, int kind)
{
    RpgActorState *a = bm_actor();
    for (;;) {
        w->visible = 1;
        rpg_bw.help.visible = 1;
        int r = select_loop(w);
        if (r != RPG_WIN_OK) {
            w->visible = 0;
            rpg_bw.help.visible = 0;
            /* back to the actor command window */
            if (select_loop(&rpg_bw.actor_cmd) != RPG_WIN_OK) {
                rpg_bvt.scene_prior_command();
                return;
            }
            int c = rpg_bw.cmds[rpg_bw.actor_cmd.index];
            switch (c) {
            case RPG_BCMD_ATTACK: rpg_bvt.command_attack(); return;
            case RPG_BCMD_SKILL: rpg_bvt.command_skill(); return;
            case RPG_BCMD_GUARD: rpg_bvt.command_guard(); return;
            case RPG_BCMD_ITEM: rpg_bvt.command_item(); return;
            case RPG_BCMD_ESCAPE: rpg_bvt.command_escape(); return;
            default: rpg_bvt.command_game(c); return;
            }
        }
        int id = kind == RPG_KIND_SKILL ? rpg_bw.skills[w->index] : rpg_bw.items[w->index];
        if (kind == RPG_KIND_SKILL) {
            rpg_action_set_skill(&a->b, a->action_input_index, id);
            a->last_skill = id;
        } else {
            rpg_action_set_item(&a->b, a->action_input_index, id);
            rpg.party.last_item_kind = RPG_KIND_ITEM;
            rpg.party.last_item_id = id;
        }
        const RpgUsable *u = rpg_usable(kind, id);
        int ok = 1;
        if (rpg_need_selection(u)) {
            if (rpg_for_opponent(u))
                select_enemy(&ok);
            else
                select_actor(&ok);
        }
        if (ok) {
            w->visible = 0;
            rpg_bw.help.visible = 0;
            rpg_bvt.scene_next_command();
            return;
        }
    }
}

static void sb_command_skill(void)
{
    rpg_bw.cmd_actor = bm_actor();
    rpg_bw.skill_stype = rpg_bw.cmd_ext[rpg_bw.actor_cmd.index];
    refresh_skills();
    int sel = 0;
    for (int i = 0; i < rpg_bw.nskills; i++)
        if (rpg_bw.skills[i] == rpg_bw.cmd_actor->last_skill)
            sel = i;
    rpg_win_select(&rpg_bw.skill, sel);
    choose_from_list(&rpg_bw.skill, RPG_KIND_SKILL);
}

static void sb_command_item(void)
{
    refresh_items();
    int sel = 0;
    for (int i = 0; i < rpg_bw.nitems; i++)
        if (rpg.party.last_item_kind == RPG_KIND_ITEM && rpg_bw.items[i] == rpg.party.last_item_id)
            sel = i;
    rpg_win_select(&rpg_bw.item, sel);
    choose_from_list(&rpg_bw.item, RPG_KIND_ITEM);
}

static void sb_command_guard(void)
{
    RpgActorState *a = bm_actor();
    rpg_action_set_skill(&a->b, a->action_input_index, rpg_battler_guard_skill(&a->b));
    rpg_bvt.scene_next_command();
}

static void sb_command_game(int cmd) { (void)cmd; rpg_bvt.scene_next_command(); }

static void sb_start_actor_command_selection(void)
{
    if (rpg_battle_input_ended())
        return;
    RpgActorState *a = bm_actor();
    if (!a) {
        rpg_bvt.scene_next_command();
        return;
    }
    int idx = rpg_actor_index(a);
    rpg_win_select(&rpg_bw.status, idx);
    rpg_win_close(&rpg_bw.party_cmd);
    rpg_bw.party_cmd.active = 0;
    rpg_bw.cmd_actor = a;
    rpg_bvt.make_actor_commands(a);
    RpgWin *w = &rpg_bw.actor_cmd;
    w->item_max = rpg_bw.ncmds;
    rpg_win_select(w, 0);
    rpg_win_open(w);
    w->cancel = 1;
    int r = select_loop(w);
    if (rpg_battle_input_ended())
        return;   /* not a cancel: back out to Scene_Battle#update, which judges */
    if (r == RPG_WIN_CANCEL) {
        rpg_bvt.scene_prior_command();
        return;
    }
    if (r != RPG_WIN_OK)
        return;
    switch (rpg_bw.cmds[w->index]) {
    case RPG_BCMD_ATTACK: rpg_bvt.command_attack(); break;
    case RPG_BCMD_SKILL: rpg_bvt.command_skill(); break;
    case RPG_BCMD_GUARD: rpg_bvt.command_guard(); break;
    case RPG_BCMD_ITEM: rpg_bvt.command_item(); break;
    case RPG_BCMD_ESCAPE: rpg_bvt.command_escape(); break;
    default: rpg_bvt.command_game(rpg_bw.cmds[w->index]); break;
    }
}

static void sb_command_fight(void) { rpg_bvt.scene_next_command(); }

static void sb_command_escape(void)
{
    if (!rpg_bvt.process_escape())
        rpg_bvt.turn_start();
}

static void sb_start_party_command_selection(void)
{
    if (rpg_battle_input_ended())
        return;
    rpg_battle_refresh_status();
    rpg_bw.status.index = -1;
    rpg_win_open(&rpg_bw.status);
    if (rpg_bvt.input_start()) {
        rpg_win_close(&rpg_bw.actor_cmd);
        RpgWin *w = &rpg_bw.party_cmd;
        w->item_max = 2;
        rpg_win_select(w, 0);
        rpg_win_open(w);
        w->cancel = 0;
        for (;;) {
            int r = select_loop(w);
            if (rpg_battle_input_ended())
                return;
            if (r != RPG_WIN_OK)
                continue;
            if (w->index == 0) {
                rpg_bvt.command_fight();
                return;
            }
            if (!rpg_b.can_escape) {
                rpg_play_system_se(RPG_SE_BUZZER);
                continue;
            }
            rpg_bvt.command_escape();
            return;
        }
    } else {
        rpg_bw.party_cmd.active = 0;
        rpg_bvt.turn_start();
    }
}

static void sb_turn_start(void)
{
    rpg_win_close(&rpg_bw.party_cmd);
    rpg_win_close(&rpg_bw.actor_cmd);
    rpg_bw.party_cmd.active = rpg_bw.actor_cmd.active = 0;
    rpg_bw.status.index = -1;
    rpg_b.subject = 0;
    rpg_bvt.turn_start_bm();
    rpg_log_wait();
    rpg_log_clear();
}

static void sb_turn_end(void)
{
    RpgBattler *m[RPG_MAX_ENEMIES + RPG_MAX_PARTY];
    int n = rpg_unit_members(0, m);
    for (int i = 0; i < rpg_b.nenemies; i++)
        m[n++] = &rpg_b.enemies[i].b;
    for (int i = 0; i < n; i++) {
        rpg_battler_on_turn_end(m[i]);
        rpg_battle_refresh_status();
        rpg_bvt.display_auto_affected_status(m[i]);
        rpg_log_wait_and_clear();
    }
    rpg_bvt.turn_end_bm();
    rpg_bvt.process_event();
    rpg_bvt.start_party_command_selection();
}

static int sb_process_forced_action(void)
{
    if (rpg_b.action_forced) {
        RpgBattler *last = rpg_b.subject;
        rpg_b.subject = rpg_b.action_forced;
        rpg_b.action_forced = 0;
        rpg_bvt.process_action();
        rpg_b.subject = last;
        return 1;
    }
    return 0;
}

static void sb_process_event(void)
{
    while (!rpg_scene_changing()) {
        rpg_interp_update(&rpg_b.interp);
        rpg_troop_setup_battle_event();
        rpg_bwait_for_message();
        if (rpg_unit_all_dead(1))
            rpg_bwait_for_effect();
        rpg_bvt.process_forced_action();
        rpg_bvt.judge_win_loss();
        if (!rpg_interp_running(&rpg_b.interp))
            break;
        rpg_bupdate_for_wait();
    }
}

static void sb_show_normal_animation(RpgBattler **t, int n, int id, int mirror)
{
    const RpgAnimation *a = rpg_anim_data(id);
    if (!a->id)
        return;
    for (int i = 0; i < n; i++) {
        t[i]->animation_id = id;
        t[i]->animation_mirror = mirror;
        if (a->position != 3)
            rpg_babs_wait(15);
    }
    if (a->position == 3)
        rpg_babs_wait(15);
}

static void sb_show_attack_animation(RpgBattler **t, int n)
{
    RpgBattler *s = rpg_b.subject;
    if (s->is_actor) {
        rpg_bvt.show_normal_animation(t, n, rpg_actor_atk_animation((RpgActorState *)s, 0), 0);
        rpg_bvt.show_normal_animation(t, n, rpg_actor_atk_animation((RpgActorState *)s, 1), 1);
    } else {
        rpg_play_system_se(RPG_SE_ENEMY_ATTACK);
        rpg_babs_wait(15);
    }
}

static void sb_show_animation(RpgBattler **t, int n, int id)
{
    if (id < 0)
        rpg_bvt.show_attack_animation(t, n);
    else
        rpg_bvt.show_normal_animation(t, n, id, 0);
    rpg_log_wait();
    rpg_bwait_for_animation();
}

static void sb_apply_item_effects(RpgBattler *t, int kind, int id)
{
    rpg_battler_item_apply(t, rpg_b.subject, kind, id);
    rpg_battle_refresh_status();
    rpg_bvt.display_action_results(t, kind, id);
}

static void sb_invoke_counter_attack(RpgBattler *t, int kind, int id)
{
    display_counter(t, kind, id);
    int atk = rpg_battler_attack_skill(t);
    rpg_battler_item_apply(rpg_b.subject, t, RPG_KIND_SKILL, atk);
    rpg_battle_refresh_status();
    rpg_bvt.display_action_results(rpg_b.subject, RPG_KIND_SKILL, atk);
}

static void sb_invoke_magic_reflection(RpgBattler *t, int kind, int id)
{
    display_reflection(t, kind, id);
    rpg_bvt.apply_item_effects(rpg_b.subject, kind, id);
}

static RpgBattler *sb_apply_substitute(RpgBattler *t, int kind, int id)
{
    const RpgUsable *u = rpg_usable(kind, id);
    if (t->hp < rpg_battler_mhp(t) / 4 && (!id || u->hit_type != 0)) {
        RpgBattler *s = rpg_unit_substitute(!t->is_actor);
        if (s && s != t) {
            display_substitute(s, t);
            return s;
        }
    }
    return t;
}

static void sb_invoke_item(RpgBattler *t, int kind, int id)
{
    if (rpg_randf() < rpg_item_cnt(t, rpg_b.subject, kind, id))
        rpg_bvt.invoke_counter_attack(t, kind, id);
    else if (rpg_randf() < rpg_item_mrf(t, rpg_b.subject, kind, id))
        rpg_bvt.invoke_magic_reflection(t, kind, id);
    else
        rpg_bvt.apply_item_effects(rpg_bvt.apply_substitute(t, kind, id), kind, id);
    rpg_b.subject->last_target_index = rpg_battler_index(t);
}

static void sb_use_item(void)
{
    RpgBattler *s = rpg_b.subject;
    int kind = rpg_action_kind(s, 0), id = rpg_action_item_id(s, 0);
    rpg_bvt.display_use_item(s, kind, id);
    rpg_battler_use_item(s, kind, id);
    rpg_battle_refresh_status();
    RpgBattler *t[32];
    int n = rpg_action_make_targets(s, 0, t, 32);
    const RpgUsable *u = rpg_usable(kind, id);
    rpg_bvt.show_animation(t, n, u->animation_id);
    for (int i = 0; i < n; i++)
        for (int r = 0; r < u->repeats; r++)
            rpg_bvt.invoke_item(t[i], kind, id);
}

static void sb_execute_action(void)
{
    rpg_b.subject->sprite_effect = 5;   /* whiten */
    rpg_bvt.use_item();
    rpg_log_wait_and_clear();
}

static void sb_process_action_end(void)
{
    RpgBattler *s = rpg_b.subject;
    rpg_battler_on_action_end(s);
    rpg_battle_refresh_status();
    rpg_bvt.display_auto_affected_status(s);
    rpg_log_wait_and_clear();
    rpg_bvt.display_current_state(s);
    rpg_log_wait_and_clear();
    rpg_bvt.judge_win_loss();
}

static void sb_process_action(void)
{
    if (rpg_scene_changing())
        return;
    if (!rpg_b.subject || !rpg_battler_current_action(rpg_b.subject))
        rpg_b.subject = rpg_bvt.next_subject();
    if (!rpg_b.subject) {
        rpg_bvt.turn_end();
        return;
    }
    if (rpg_battler_current_action(rpg_b.subject)) {
        rpg_action_prepare(rpg_b.subject, 0);
        if (rpg_action_valid(rpg_b.subject, 0)) {
            rpg_win_open(&rpg_bw.status);
            rpg_bvt.execute_action();
        }
        rpg_battler_remove_current_action(rpg_b.subject);
    }
    if (!rpg_battler_current_action(rpg_b.subject))
        rpg_bvt.process_action_end();
}

static void sb_scene_battle_start(void)
{
    rpg_bvt.battle_start();
    rpg_bvt.process_event();
    rpg_bvt.start_party_command_selection();
}

static void sb_update(void)
{
    rpg_bvt.update_basic();
    if (rpg_b.phase == BP_TURN) {
        rpg_bvt.process_event();
        rpg_bvt.process_action();
    }
    rpg_bvt.judge_win_loss();
    if (rpg_b.input_abort) {
        /* the command input was given up and the battle goes on: choose again */
        rpg_b.input_abort = 0;
        if (rpg_b.phase != BP_NONE && !rpg_scene_changing())
            rpg_bvt.start_party_command_selection();
    }
}

static void sb_start(void)
{
    create_windows();
    init_sprites();
    rpg_anims_clear();
    rpg_log_clear();
}

/* the coroutine: Scene_Base#main */
static int battle_co = -1, battle_done;

static void battle_main(void *arg)
{
    (void)arg;
    rpg_scene_hold = 1;
    rpg_bvt.scene_battle_start();
    while (!rpg_scene_changing())
        rpg_bvt.update();
    /* pre_terminate: fade out (the scene manager switches after this) */
    for (int i = 0; i < 30; i++) {
        rpg_brightness = 255 - 255 * (i + 1) / 30;
        vcpe_co_wait_frame();
    }
    battle_done = 1;
    rpg_scene_hold = 0;
}

/* ------------------------------------------------------------------ drawing */
static void status_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    rpg_bvt.draw_status_item(w, i, r);
}

void rpg_battle_draw_status_default(RpgWin *w, int i, RpgRect r)
{
    (void)w;
    int ids[RPG_MAX_PARTY], n = rpg_party_battle_members(ids);
    if (i >= n)
        return;
    RpgActorState *a = rpg_actor(ids[i]);
    float gw = 220, rx = r.x + 4, rw = r.w - 8;
    float bw = rw - gw - 10;
    rpg_draw_actor_name(a, rx, r.y, 100);
    rpg_draw_actor_icons(&a->b, rx + 104, r.y, bw - 104);
    float gx = rx + rw - gw;
    if (rpg_system()->opt_display_tp) {
        rpg_draw_actor_hp(&a->b, gx, r.y, 72);
        rpg_draw_actor_mp(&a->b, gx + 82, r.y, 64);
        rpg_draw_actor_tp(&a->b, gx + 156, r.y, 64);
    } else {
        rpg_draw_actor_hp(&a->b, gx, r.y, 134);
        rpg_draw_actor_mp(&a->b, gx + 144, r.y, 76);
    }
}

static void party_cmd_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int en = i == 0 || rpg_b.can_escape;
    (void)w;
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, rpg_term_command(i == 0 ? 0 : 1), rpg_color_enabled(rpg_color_normal(), en), 0);
}

static void actor_cmd_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int en = w->enabled(w, i);
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, cmd_name(rpg_bw.cmds[i], rpg_bw.cmd_ext[i]),
                    rpg_color_enabled(rpg_color_normal(), en), 0);
}

static void skill_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    RpgActorState *a = rpg_bw.cmd_actor;
    int id = rpg_bw.skills[i], en = w->enabled(w, i);
    r.w -= 4;
    rpg_draw_item_name(RPG_KIND_SKILL, id, r.x, r.y, en, r.w - 40);
    int tp = rpg_battler_skill_tp_cost(&a->b, id), mp = rpg_battler_skill_mp_cost(&a->b, id);
    char c[16];
    if (tp > 0) {
        snprintf(c, sizeof c, "%d", tp);
        rpg_draw_text_c(r.x, r.y, r.w, c, rpg_color_enabled(rpg_color_tp_cost(), en), 2);
    } else if (mp > 0) {
        snprintf(c, sizeof c, "%d", mp);
        rpg_draw_text_c(r.x, r.y, r.w, c, rpg_color_enabled(rpg_color_mp_cost(), en), 2);
    }
}

static void item_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int id = rpg_bw.items[i], en = w->enabled(w, i);
    r.w -= 4;
    rpg_draw_item_name(RPG_KIND_ITEM, id, r.x, r.y, en, r.w - 40);
    char n[16];
    snprintf(n, sizeof n, ":%2d", rpg_party_item_number(RPG_KIND_ITEM, id));
    rpg_draw_text_c(r.x, r.y, r.w, n, rpg_color_enabled(rpg_color_normal(), en), 2);
}

static void enemy_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    RpgBattler *alive[RPG_MAX_ENEMIES];
    int n = rpg_unit_alive(1, alive);
    if (i < n)
        rpg_draw_text_c(r.x + 4, r.y, r.w - 8, rpg_battler_name(alive[i]), rpg_color_normal(), 0);
}

static void actor_sel_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    rpg_bvt.draw_status_item(w, i, r);
}

static void draw_windows(void)
{
    RpgBattleWindows *w = &rpg_bw;
    /* the log: a dim band behind its lines */
    if (log_n > 0) {
        rpg_fill(0, w->log.y + 12, rpg_screen_w, log_n * 24, rpg_rgba(0, 0, 0, 64));
        for (int i = 0; i < log_n; i++) {
            char conv[512];
            rpg_convert_escapes(conv, sizeof conv, log_lines[i]);
            rpg_draw_text_ex(w->log.x + 12 + 4, w->log.y + 12 + i * 24, conv);
        }
    }
    /* the info viewport: status, party command, actor command, sliding by info_ox */
    float ox = w->info_ox;
    rpg_push_origin(-ox, 0);
    refresh_status_default();
    rpg_win_draw(&w->status, status_item, 0);
    rpg_win_draw(&w->party_cmd, party_cmd_item, 0);
    rpg_win_draw(&w->actor_cmd, actor_cmd_item, 0);
    rpg_pop_origin();
    if (w->help.visible) {
        const char *d = "";
        if (w->skill.visible && w->skill.index >= 0 && w->skill.index < w->nskills)
            d = RPG_STR(rpg_skill_data(w->skills[w->skill.index])->description);
        if (w->item.visible && w->item.index >= 0 && w->item.index < w->nitems)
            d = RPG_STR(rpg_item_data(w->items[w->item.index])->description);
        rpg_help_draw(&w->help, d);
    }
    if (w->skill.visible)
        rpg_win_draw(&w->skill, skill_item, 0);
    if (w->item.visible)
        rpg_win_draw(&w->item, item_item, 0);
    if (w->actor_sel.visible)
        rpg_win_draw(&w->actor_sel, actor_sel_item, 0);
    if (w->enemy_sel.visible)
        rpg_win_draw(&w->enemy_sel, enemy_item, 0);
}

static void sb_draw(void)
{
    rpg_bvt.draw_back();
    rpg_bvt.draw_battlers();
    rpg_anims_draw();
    rpg_screen_draw_tone_flash(&rpg_battle_screen);
    rpg_screen_draw_pictures(&rpg_battle_screen);
    rpg_screen_draw_flash(&rpg_battle_screen);
    rpg_screen_draw_brightness(&rpg_battle_screen);
    rpg_bvt.draw_windows();
    rpg_msg_draw();
}

/* ------------------------------------------------------------------ the scene */
static void scene_start(void)
{
    /* Scene_Map#pre_battle_scene: map music saved, battle music, the start sound */
    rpg_b.map_bgm = rpg_bgm_current();
    rpg_b.map_bgm_pos = rpg_bgm_pos();
    rpg_b.map_bgs = rpg_bgs_current();
    const RpgSystem *s = rpg_system();
    if (rpg.has_battle_bgm)
        rpg_bgm_play(rpg.battle_bgm.name, rpg.battle_bgm.volume, rpg.battle_bgm.pitch);
    else
        rpg_bgm_play(RPG_STR(s->battle_bgm.name), s->battle_bgm.volume, s->battle_bgm.pitch);
    rpg_bgs_stop();
    rpg_play_system_se(RPG_SE_BATTLE_START);
    rpg_msg.visible = 0;
    rpg_level_up_message = level_up_message;
    rpg_bvt.start();
    rpg_log("RPG battle %d starts: %d enemies", rpg_b.troop_id, rpg_b.nenemies);
    battle_done = 0;
    rpg_brightness = 255;
    battle_co = vcpe_co_start(battle_main, 0);
    if (battle_co < 0) {
        rpg_log("RPG battle: no coroutine free");
        rpg_scene_return();
    }
}

static void scene_update(void)
{
    if (battle_co >= 0)
        vcpe_co_update(1 / 60.f);
}

static void scene_terminate(void)
{
    rpg_scene_hold = 0;
    if (battle_co >= 0 && vcpe_co_alive(battle_co))
        vcpe_co_reset();
    battle_co = -1;
    rpg_me_stop();
    rpg_anims_clear();
    rpg.party.in_battle = 0;
    /* the map fades back in (Scene_Map#perform_transition after a fade out) */
    rpg_brightness = 0;
}

static void scene_draw(void) { rpg_bvt.draw(); }

const RpgBattleVT rpg_bdef = {
    bm_battle_start, bm_judge_win_loss, bm_process_victory, bm_process_escape, bm_process_abort,
    bm_process_defeat, bm_battle_end, bm_input_start, bm_turn_start, bm_turn_end, bm_make_action_orders,
    bm_next_subject, bm_next_command, bm_prior_command, bm_display_exp, bm_gain_gold, bm_gain_drop_items,
    bm_gain_exp,
    sb_start, sb_scene_battle_start, sb_update, sb_update_basic, sb_start_party_command_selection,
    sb_start_actor_command_selection, sb_command_fight, sb_command_escape, sb_command_attack, sb_command_skill,
    sb_command_guard, sb_command_item, sb_command_game, sb_scene_next_command, sb_scene_prior_command,
    sb_turn_start, sb_turn_end, sb_process_event, sb_process_forced_action, sb_process_action,
    sb_process_action_end, sb_execute_action, sb_use_item, sb_invoke_item, sb_apply_item_effects,
    sb_invoke_counter_attack, sb_invoke_magic_reflection, sb_apply_substitute, sb_show_animation,
    sb_show_attack_animation, sb_show_normal_animation, make_actor_commands,
    display_use_item, display_action_results, display_damage, display_affected_status,
    display_auto_affected_status, display_current_state,
    sb_draw, draw_back, draw_battlers, draw_windows, rpg_battle_draw_status_default, sprite_update, draw_battler,
    enemy_screen_y, enemy_screen_x, def_wait, def_abs_wait, refresh_status_default, display_critical,
};

void rpg_vehicle_set_location(int v, int m, int x, int y) { (void)v; (void)m; (void)x; (void)y; }
void rpg_vehicle_get_on_off(void) {}

void rpg_battle_define(void)
{
    static const RpgSceneDef b = {scene_start, scene_update, scene_draw, scene_terminate};
    if (!rpg_bvt.start)
        rpg_bvt = rpg_bdef;
    rpg_scene_define(RPG_SCENE_BATTLE, &b);
}
