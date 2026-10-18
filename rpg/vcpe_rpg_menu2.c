/* VCPE RPG menu scenes (2): Scene_Item, Scene_Skill (with Scene_ItemBase's target window), Scene_Equip,
 * Scene_Status, Scene_Name. Layouts follow VX Ace's, fitted to the project's screen height where VX Ace's would not
 * fit (the status page drops its separator lines, the name input lets its windows touch). */
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"
#include "vcpe_rpg_menu.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void rpg_scene_fade(int frames, int dir, void (*then)(void));
int rpg_scene_fade_step(void);

static RpgActorState *menu_actor(void)
{
    RpgActorState *a = rpg_party_menu_actor();
    return a ? a : rpg_actor(1);
}

/* ------------------------------------------------------------------ Scene_ItemBase: the target actor window */
static struct {
    RpgWin w;
    int kind, id;        /* the item being used */
    int left;            /* the window shows on the left */
} TA;

static void actor_window_show(int kind, int id, int item_index_left_column)
{
    const RpgUsable *u = rpg_usable(kind, id);
    TA.kind = kind;
    TA.id = id;
    rpg_win_init(&TA.w, 0, 0, rpg_screen_w - 160, rpg_screen_h);
    TA.w.item_max = rpg.party.nmembers;
    TA.w.item_h = (rpg_screen_h - 24) / 4;
    TA.left = !item_index_left_column;
    TA.w.x = item_index_left_column ? rpg_screen_w - TA.w.w : 0;
    TA.w.cursor_fix = rpg_for_user(u);
    TA.w.cursor_all = rpg_for_all(u);
    int idx = 0;
    RpgActorState *m = menu_actor();
    for (int i = 0; i < rpg.party.nmembers; i++)
        if (rpg.party.members[i] == (TA.w.cursor_fix ? m->actor_id : (rpg.party.target_actor ? rpg.party.target_actor : m->actor_id)))
            idx = i;
    rpg_win_select(&TA.w, TA.w.cursor_all ? 0 : idx);
    TA.w.active = 1;
    TA.w.visible = 1;
}

static void actor_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    RpgActorState *a = rpg_actor(rpg.party.members[i]);
    if (rpg_hooks.menu_actor_draw) {
        rpg_hooks.menu_actor_draw(a, r.x, r.y, r.w, r.h);
        return;
    }
    float fs = w->item_h - 2 < 96 ? w->item_h - 2 : 96;
    rpg_draw_actor_face(a, r.x + 1, r.y + 1, fs, rpg_actor_battle_member(a));
    rpg_draw_actor_simple_status(a, r.x + 108, r.y + (w->item_h - rpg_line_height() * 3) / 2);
}

/* the user of an item: the movable member with the best pharmacology */
static RpgActorState *item_user(int kind, RpgActorState *skill_user)
{
    if (kind == RPG_KIND_SKILL)
        return skill_user;
    RpgActorState *best = 0;
    double bp = -1;
    for (int i = 0; i < rpg.party.nmembers; i++) {
        RpgActorState *a = rpg_actor(rpg.party.members[i]);
        if (!rpg_battler_movable(&a->b))
            continue;
        double p = rpg_battler_sparam(&a->b, 3);
        if (p > bp) {
            bp = p;
            best = a;
        }
    }
    return best ? best : menu_actor();
}

static int targets(const RpgUsable *u, int *ids)
{
    int n = 0;
    if (!rpg_for_friend(u))
        return 0;
    if (rpg_for_all(u)) {
        for (int i = 0; i < rpg.party.nmembers; i++)
            ids[n++] = rpg.party.members[i];
    } else if (TA.w.index >= 0 && TA.w.index < rpg.party.nmembers) {
        ids[n++] = rpg.party.members[TA.w.index];
    }
    return n;
}

static int item_usable(int kind, int id, RpgActorState *user)
{
    if (!user || !rpg_battler_usable(&user->b, kind, id))
        return 0;
    const RpgUsable *u = rpg_usable(kind, id);
    int ids[RPG_MAX_PARTY], n = targets(u, ids);
    if (!rpg_for_friend(u))
        return 1;
    for (int i = 0; i < n; i++)
        if (rpg_battler_item_test(&rpg_actor(ids[i])->b, &user->b, kind, id))
            return 1;
    return 0;
}

static void use_item(int kind, int id, RpgActorState *user)
{
    rpg_play_system_se(kind == RPG_KIND_SKILL ? RPG_SE_USE_SKILL : RPG_SE_USE_ITEM);
    rpg_battler_use_item(&user->b, kind, id);
    const RpgUsable *u = rpg_usable(kind, id);
    int ids[RPG_MAX_PARTY], n = targets(u, ids);
    for (int i = 0; i < n; i++)
        for (int r = 0; r < u->repeats; r++)
            rpg_battler_item_apply(&rpg_actor(ids[i])->b, &user->b, kind, id);
    if (rpg.reserved_common > 0)
        rpg_scene_goto(RPG_SCENE_MAP);
    else if (rpg_party_all_dead())
        rpg_scene_goto(RPG_SCENE_GAMEOVER);
}

/* ------------------------------------------------------------------ item lists */
typedef struct {
    int kind[RPG_MAX_ITEMS * 3 / 2], id[RPG_MAX_ITEMS * 3 / 2];
    int n;
} ItemList;

static void list_party_items(ItemList *l, int category)
{
    /* 0 items, 1 weapons, 2 armors, 3 key items */
    l->n = 0;
    int kind = category == 1 ? RPG_KIND_WEAPON : category == 2 ? RPG_KIND_ARMOR : RPG_KIND_ITEM;
    int count = rpg_count(&rpg_db, kind == RPG_KIND_WEAPON ? RPG_T_WEAPON : kind == RPG_KIND_ARMOR ? RPG_T_ARMOR : RPG_T_ITEM);
    for (int id = 1; id < count && id < RPG_MAX_ITEMS; id++) {
        if (rpg_party_item_number(kind, id) <= 0)
            continue;
        if (kind == RPG_KIND_ITEM) {
            int key = rpg_item_data(id)->itype_id == 2;
            if ((category == 3) != key)
                continue;
        }
        l->kind[l->n] = kind;
        l->id[l->n] = id;
        l->n++;
    }
}

/* ------------------------------------------------------------------ Scene_Item */
static struct {
    RpgWin help, cat, list;
    ItemList items;
    int mode;   /* 0 category, 1 list, 2 actor */
    int last_cat;
} IT;

static int item_enabled(RpgWin *w, int i)
{
    (void)w;
    if (i < 0 || i >= IT.items.n)
        return 0;
    return IT.items.kind[i] == RPG_KIND_ITEM && rpg_party_usable(RPG_KIND_ITEM, IT.items.id[i]);
}

static void item_refresh(void)
{
    list_party_items(&IT.items, IT.cat.index);
    IT.list.item_max = IT.items.n;
    if (IT.list.index >= IT.items.n)
        IT.list.index = IT.items.n - 1;
}

static void item_start(void)
{
    rpg_win_init(&IT.help, 0, 0, rpg_screen_w, rpg_fitting_height(2));
    rpg_win_init(&IT.cat, 0, IT.help.h, rpg_screen_w, rpg_fitting_height(1));
    IT.cat.col_max = 4;
    IT.cat.spacing = 8;
    IT.cat.item_max = 4;
    IT.cat.active = 1;
    rpg_win_select(&IT.cat, IT.last_cat);
    float y = IT.cat.y + IT.cat.h;
    rpg_win_init(&IT.list, 0, y, rpg_screen_w, rpg_screen_h - y);
    IT.list.col_max = 2;
    IT.list.enabled = item_enabled;
    IT.mode = 0;
    TA.w.visible = 0;
    item_refresh();
}

static void item_update(void)
{
    if (IT.mode == 0) {
        int last = IT.cat.index;
        int r = rpg_win_update(&IT.cat);
        if (IT.cat.index != last) {
            item_refresh();
            IT.list.oy = 0;
        }
        IT.last_cat = IT.cat.index;
        if (r == RPG_WIN_CANCEL) {
            rpg_scene_return();
        } else if (r == RPG_WIN_OK) {
            IT.mode = 1;
            IT.list.active = 1;
            int sel = 0;
            for (int i = 0; i < IT.items.n; i++)
                if (IT.items.kind[i] == rpg.party.last_item_kind && IT.items.id[i] == rpg.party.last_item_id)
                    sel = i;
            rpg_win_select(&IT.list, sel);
        }
    } else if (IT.mode == 1) {
        int r = rpg_win_update(&IT.list);
        if (r == RPG_WIN_CANCEL) {
            IT.list.index = -1;
            IT.cat.active = 1;
            IT.mode = 0;
        } else if (r == RPG_WIN_OK) {
            int kind = IT.items.kind[IT.list.index], id = IT.items.id[IT.list.index];
            rpg.party.last_item_kind = kind;
            rpg.party.last_item_id = id;
            const RpgUsable *u = rpg_usable(kind, id);
            if (rpg_for_friend(u)) {
                actor_window_show(kind, id, IT.list.index % 2 == 0);
                IT.mode = 2;
            } else {
                RpgActorState *user = item_user(kind, 0);
                if (item_usable(kind, id, user))
                    use_item(kind, id, user);
                else
                    rpg_play_system_se(RPG_SE_BUZZER);
                item_refresh();
                IT.list.active = 1;
            }
        }
    } else {
        int r = rpg_win_update(&TA.w);
        if (r == RPG_WIN_CANCEL) {
            TA.w.visible = 0;
            item_refresh();
            IT.list.active = 1;
            IT.mode = 1;
        } else if (r == RPG_WIN_OK) {
            if (!TA.w.cursor_all && TA.w.index >= 0)
                rpg.party.target_actor = rpg.party.members[TA.w.index];
            RpgActorState *user = item_user(TA.kind, 0);
            if (item_usable(TA.kind, TA.id, user))
                use_item(TA.kind, TA.id, user);
            else
                rpg_play_system_se(RPG_SE_BUZZER);
            TA.w.active = 1;
            item_refresh();
            if (rpg_party_item_number(TA.kind, TA.id) <= 0) {
                TA.w.visible = 0;
                IT.list.active = 1;
                IT.mode = 1;
            }
        }
    }
}

static void item_list_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int kind = IT.items.kind[i], id = IT.items.id[i];
    int en = w->enabled ? w->enabled(w, i) : 1;
    r.w -= 4;
    rpg_draw_item_name(kind, id, r.x, r.y, en, r.w - 40);
    char n[16];
    snprintf(n, sizeof n, ":%2d", rpg_party_item_number(kind, id));
    rpg_draw_text_c(r.x, r.y, r.w, n, rpg_color_enabled(rpg_color_normal(), en), 2);
}

static void cat_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    static const int t[4] = {4, 12, 13, 14};
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, rpg_term_command(t[i]), rpg_color_normal(), 1);
}

static void item_draw(void)
{
    rpg_menu_background();
    const char *desc = "";
    if (IT.mode >= 1 && IT.list.index >= 0 && IT.list.index < IT.items.n)
        desc = RPG_STR(((const int32_t *)(IT.items.kind[IT.list.index] == RPG_KIND_ITEM
                                              ? (const void *)rpg_item_data(IT.items.id[IT.list.index])
                                              : (const void *)rpg_equip(IT.items.kind[IT.list.index], IT.items.id[IT.list.index])))[3]);
    rpg_help_draw(&IT.help, desc);
    rpg_win_draw(&IT.cat, cat_item, 0);
    rpg_win_draw(&IT.list, item_list_item, 0);
    if (TA.w.visible && IT.mode == 2) {
        TA.w.item_max = rpg.party.nmembers;
        rpg_win_draw(&TA.w, actor_item, 0);
    }
}

/* ------------------------------------------------------------------ Scene_Skill */
static struct {
    RpgWin help, cmd, status, list;
    int stypes[16], nstypes;
    int skills[RPG_MAX_SKILLS / 2], nskills;
    int mode;
} SK;

static RpgActorState *sk_actor;

static int skill_enabled(RpgWin *w, int i)
{
    (void)w;
    return i >= 0 && i < SK.nskills && rpg_battler_usable(&sk_actor->b, RPG_KIND_SKILL, SK.skills[i]);
}

static void skill_refresh(void)
{
    /* added skill types, sorted */
    SK.nstypes = 0;
    int nst = rpg_system()->skill_types.count;
    for (int t = 1; t < nst && SK.nstypes < 16; t++)
        if (rpg_features_set_has(&sk_actor->b, F_STYPE_ADD, t))
            SK.stypes[SK.nstypes++] = t;
    SK.cmd.item_max = SK.nstypes;
    if (SK.cmd.index >= SK.nstypes)
        SK.cmd.index = SK.nstypes - 1;
    int st = SK.cmd.index >= 0 ? SK.stypes[SK.cmd.index] : -1;
    int all[RPG_MAX_SKILLS / 2], n = rpg_actor_skills(sk_actor, all, RPG_MAX_SKILLS / 2);
    SK.nskills = 0;
    for (int i = 0; i < n; i++)
        if (rpg_skill_data(all[i])->stype_id == st)
            SK.skills[SK.nskills++] = all[i];
    SK.list.item_max = SK.nskills;
}

static void skill_start(void)
{
    sk_actor = menu_actor();
    rpg_win_init(&SK.help, 0, 0, rpg_screen_w, rpg_fitting_height(2));
    rpg_win_init(&SK.cmd, 0, SK.help.h, 160, rpg_fitting_height(4));
    SK.cmd.pageup = SK.cmd.pagedown = 1;
    SK.cmd.active = 1;
    rpg_win_init(&SK.status, 160, SK.help.h, rpg_screen_w - 160, rpg_fitting_height(4));
    float y = SK.status.y + SK.status.h;
    rpg_win_init(&SK.list, 0, y, rpg_screen_w, rpg_screen_h - y);
    SK.list.col_max = 2;
    SK.list.enabled = skill_enabled;
    SK.cmd.index = 0;
    skill_refresh();
    /* select_last: the last skill's type */
    if (sk_actor->last_skill > 0)
        for (int i = 0; i < SK.nstypes; i++)
            if (SK.stypes[i] == rpg_skill_data(sk_actor->last_skill)->stype_id)
                rpg_win_select(&SK.cmd, i);
    skill_refresh();
    SK.mode = 0;
    TA.w.visible = 0;
}

static void skill_update(void)
{
    if (SK.mode == 0) {
        int last = SK.cmd.index;
        int r = rpg_win_update(&SK.cmd);
        if (SK.cmd.index != last) {
            skill_refresh();
            SK.list.oy = 0;
        }
        if (r == RPG_WIN_CANCEL) {
            rpg_scene_return();
        } else if (r == RPG_WIN_PAGEDOWN || r == RPG_WIN_PAGEUP) {
            rpg_party_menu_actor_step(r == RPG_WIN_PAGEDOWN ? 1 : -1);
            sk_actor = menu_actor();
            skill_refresh();
            SK.cmd.active = 1;
        } else if (r == RPG_WIN_OK) {
            SK.mode = 1;
            SK.list.active = 1;
            int sel = 0;
            for (int i = 0; i < SK.nskills; i++)
                if (SK.skills[i] == sk_actor->last_skill)
                    sel = i;
            rpg_win_select(&SK.list, sel);
        }
    } else if (SK.mode == 1) {
        int r = rpg_win_update(&SK.list);
        if (r == RPG_WIN_CANCEL) {
            SK.list.index = -1;
            SK.cmd.active = 1;
            SK.mode = 0;
        } else if (r == RPG_WIN_OK) {
            int id = SK.skills[SK.list.index];
            sk_actor->last_skill = id;
            const RpgUsable *u = rpg_usable(RPG_KIND_SKILL, id);
            if (rpg_for_friend(u)) {
                actor_window_show(RPG_KIND_SKILL, id, SK.list.index % 2 == 0);
                SK.mode = 2;
            } else {
                if (item_usable(RPG_KIND_SKILL, id, sk_actor))
                    use_item(RPG_KIND_SKILL, id, sk_actor);
                else
                    rpg_play_system_se(RPG_SE_BUZZER);
                skill_refresh();
                SK.list.active = 1;
            }
        }
    } else {
        int r = rpg_win_update(&TA.w);
        if (r == RPG_WIN_CANCEL) {
            TA.w.visible = 0;
            SK.list.active = 1;
            SK.mode = 1;
        } else if (r == RPG_WIN_OK) {
            if (!TA.w.cursor_all && TA.w.index >= 0)
                rpg.party.target_actor = rpg.party.members[TA.w.index];
            if (item_usable(TA.kind, TA.id, sk_actor))
                use_item(TA.kind, TA.id, sk_actor);
            else
                rpg_play_system_se(RPG_SE_BUZZER);
            TA.w.active = 1;
            skill_refresh();
        }
    }
}

static void stype_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    const int32_t *s = rpg_at(&rpg_db, RPG_T_STR, rpg_system()->skill_types, SK.stypes[i]);
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, s ? RPG_STR(*s) : "", rpg_color_normal(), 0);
}

static void skill_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int id = SK.skills[i], en = w->enabled(w, i);
    r.w -= 4;
    rpg_draw_item_name(RPG_KIND_SKILL, id, r.x, r.y, en, r.w - 40);
    char c[16];
    int tp = rpg_battler_skill_tp_cost(&sk_actor->b, id), mp = rpg_battler_skill_mp_cost(&sk_actor->b, id);
    if (tp > 0) {
        snprintf(c, sizeof c, "%d", tp);
        rpg_draw_text_c(r.x, r.y, r.w, c, rpg_color_enabled(rpg_color_tp_cost(), en), 2);
    } else if (mp > 0) {
        snprintf(c, sizeof c, "%d", mp);
        rpg_draw_text_c(r.x, r.y, r.w, c, rpg_color_enabled(rpg_color_mp_cost(), en), 2);
    }
}

static void skill_draw(void)
{
    rpg_menu_background();
    const char *desc = "";
    if (SK.mode >= 1 && SK.list.index >= 0 && SK.list.index < SK.nskills)
        desc = RPG_STR(rpg_skill_data(SK.skills[SK.list.index])->description);
    rpg_help_draw(&SK.help, desc);
    rpg_win_draw(&SK.cmd, stype_item, 0);
    rpg_win_draw_frame(&SK.status);
    if (rpg_win_contents_begin(&SK.status)) {
        rpg_draw_actor_face(sk_actor, 0, 0, 96, 1);
        rpg_draw_actor_simple_status(sk_actor, 108, rpg_line_height() / 2);
        rpg_win_contents_end(&SK.status);
    }
    rpg_win_draw(&SK.list, skill_item, 0);
    if (TA.w.visible && SK.mode == 2) {
        TA.w.item_max = rpg.party.nmembers;
        rpg_win_draw(&TA.w, actor_item, 0);
    }
}

/* ------------------------------------------------------------------ Scene_Equip */
static struct {
    RpgWin help, status, cmd, slot, list;
    int etypes[RPG_MAX_SLOTS], nslots;
    int ids[RPG_MAX_ITEMS + 1], kinds[RPG_MAX_ITEMS + 1], n;
    int mode;   /* 0 command, 1 slot, 2 item */
    RpgActorState temp;
    int has_temp;
} EQ;
static RpgActorState *eq_actor;

static int slot_enabled(RpgWin *w, int i)
{
    (void)w;
    return rpg_actor_equip_change_ok(eq_actor, i);
}

static void equip_items_refresh(void)
{
    /* Window_EquipItem: equippable items of the slot's type, then "nothing" */
    EQ.n = 0;
    int s = EQ.slot.index;
    if (s >= 0 && s < EQ.nslots) {
        int kind = EQ.etypes[s] == 0 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR;
        int count = rpg_count(&rpg_db, kind == RPG_KIND_WEAPON ? RPG_T_WEAPON : RPG_T_ARMOR);
        for (int id = 1; id < count && id < RPG_MAX_ITEMS; id++)
            if (rpg_party_item_number(kind, id) > 0 && rpg_equip(kind, id)->etype_id == EQ.etypes[s] &&
                rpg_actor_equippable(eq_actor, kind == RPG_KIND_ARMOR, id)) {
                EQ.kinds[EQ.n] = kind;
                EQ.ids[EQ.n++] = id;
            }
    }
    EQ.kinds[EQ.n] = 0;
    EQ.ids[EQ.n++] = 0;
    EQ.list.item_max = EQ.n;
}

static void equip_refresh(void)
{
    EQ.nslots = rpg_actor_slots(eq_actor, EQ.etypes);
    EQ.slot.item_max = EQ.nslots;
    equip_items_refresh();
}

static void equip_preview(void)
{
    EQ.has_temp = 0;
    if (EQ.mode == 2 && EQ.list.index >= 0 && EQ.list.index < EQ.n) {
        EQ.temp = *eq_actor;
        rpg_actor_force_change_equip(&EQ.temp, EQ.slot.index, EQ.kinds[EQ.list.index], EQ.ids[EQ.list.index]);
        EQ.has_temp = 1;
    }
}

static void equip_start(void)
{
    eq_actor = menu_actor();
    rpg_win_init(&EQ.help, 0, 0, rpg_screen_w, rpg_fitting_height(2));
    rpg_win_init(&EQ.status, 0, EQ.help.h, 208, rpg_fitting_height(7));
    rpg_win_init(&EQ.cmd, 208, EQ.help.h, rpg_screen_w - 208, rpg_fitting_height(1));
    EQ.cmd.col_max = 3;
    EQ.cmd.spacing = 8;
    EQ.cmd.item_max = 3;
    EQ.cmd.pageup = EQ.cmd.pagedown = 1;
    EQ.cmd.active = 1;
    rpg_win_select(&EQ.cmd, 0);
    rpg_win_init(&EQ.slot, 208, EQ.cmd.y + EQ.cmd.h, rpg_screen_w - 208, rpg_fitting_height(5));
    EQ.slot.enabled = slot_enabled;
    float y = EQ.slot.y + EQ.slot.h;
    rpg_win_init(&EQ.list, 0, y, rpg_screen_w, rpg_screen_h - y);
    EQ.list.col_max = 2;
    EQ.mode = 0;
    EQ.slot.index = -1;
    equip_refresh();
}

static void equip_update(void)
{
    if (EQ.mode == 0) {
        int r = rpg_win_update(&EQ.cmd);
        if (r == RPG_WIN_CANCEL) {
            rpg_scene_return();
        } else if (r == RPG_WIN_PAGEDOWN || r == RPG_WIN_PAGEUP) {
            rpg_party_menu_actor_step(r == RPG_WIN_PAGEDOWN ? 1 : -1);
            eq_actor = menu_actor();
            equip_refresh();
            EQ.cmd.active = 1;
        } else if (r == RPG_WIN_OK) {
            if (EQ.cmd.index == 0) {
                EQ.mode = 1;
                EQ.slot.active = 1;
                rpg_win_select(&EQ.slot, 0);
                equip_items_refresh();
            } else {
                rpg_play_system_se(RPG_SE_EQUIP);
                if (EQ.cmd.index == 1)
                    rpg_actor_optimize_equipments(eq_actor);
                else
                    rpg_actor_clear_equipments(eq_actor);
                equip_refresh();
                EQ.cmd.active = 1;
            }
        }
    } else if (EQ.mode == 1) {
        int last = EQ.slot.index;
        int r = rpg_win_update(&EQ.slot);
        if (EQ.slot.index != last) {
            equip_items_refresh();
            EQ.list.oy = 0;
        }
        if (r == RPG_WIN_CANCEL) {
            EQ.slot.index = -1;
            EQ.cmd.active = 1;
            EQ.mode = 0;
        } else if (r == RPG_WIN_OK) {
            EQ.mode = 2;
            EQ.list.active = 1;
            rpg_win_select(&EQ.list, 0);
        }
    } else {
        int r = rpg_win_update(&EQ.list);
        if (r == RPG_WIN_CANCEL) {
            EQ.list.index = -1;
            EQ.slot.active = 1;
            EQ.mode = 1;
        } else if (r == RPG_WIN_OK) {
            rpg_play_system_se(RPG_SE_EQUIP);
            int s = EQ.slot.index;
            rpg_actor_change_equip(eq_actor, s, EQ.etypes[s] == 0 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR,
                                   EQ.ids[EQ.list.index]);
            EQ.list.index = -1;
            EQ.slot.active = 1;
            EQ.mode = 1;
            equip_refresh();
        }
    }
    equip_preview();
}

static void eqcmd_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    static const int t[3] = {15, 16, 17};
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, rpg_term_command(t[i]), rpg_color_normal(), 1);
}

static void slot_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int en = w->enabled(w, i);
    rpg_draw_text_c(r.x + 4, r.y, 92, rpg_term_etype(EQ.etypes[i]), rpg_color_enabled(rpg_color_system(), en), 0);
    if (eq_actor->equip_id[i] > 0)
        rpg_draw_item_name(eq_actor->equip_kind[i] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, eq_actor->equip_id[i],
                           r.x + 4 + 92, r.y, en, r.w - 100);
}

static void eqitem_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    if (!EQ.ids[i])
        return;
    r.w -= 4;
    rpg_draw_item_name(EQ.kinds[i], EQ.ids[i], r.x, r.y, 1, r.w - 40);
    char n[16];
    snprintf(n, sizeof n, ":%2d", rpg_party_item_number(EQ.kinds[i], EQ.ids[i]));
    rpg_draw_text_c(r.x, r.y, r.w, n, rpg_color_normal(), 2);
}

static void equip_draw(void)
{
    rpg_menu_background();
    const char *desc = "";
    if (EQ.mode == 1 && EQ.slot.index >= 0 && eq_actor->equip_id[EQ.slot.index] > 0)
        desc = RPG_STR(rpg_equip(eq_actor->equip_kind[EQ.slot.index] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON,
                                 eq_actor->equip_id[EQ.slot.index])->description);
    if (EQ.mode == 2 && EQ.list.index >= 0 && EQ.ids[EQ.list.index] > 0)
        desc = RPG_STR(rpg_equip(EQ.kinds[EQ.list.index], EQ.ids[EQ.list.index])->description);
    rpg_help_draw(&EQ.help, desc);
    rpg_win_draw_frame(&EQ.status);
    if (rpg_win_contents_begin(&EQ.status)) {
        float lh = rpg_line_height();
        rpg_draw_actor_name(eq_actor, 4, 0, 160);
        for (int i = 0; i < 6; i++) {
            int p = 2 + i;
            float y = lh * (1 + i);
            char v[16];
            rpg_draw_text_c(4, y, 80, rpg_term_param(p), rpg_color_system(), 0);
            int cur = rpg_battler_param(&eq_actor->b, p);
            snprintf(v, sizeof v, "%d", cur);
            rpg_draw_text_c(94, y, 32, v, rpg_color_normal(), 2);
            rpg_draw_text_c(126, y, 22, "\xe2\x86\x92", rpg_color_system(), 1);
            if (EQ.has_temp) {
                int nv = rpg_battler_param(&EQ.temp.b, p);
                snprintf(v, sizeof v, "%d", nv);
                uint32_t c = nv > cur ? rpg_color_power_up() : nv < cur ? rpg_color_power_down() : rpg_color_normal();
                rpg_draw_text_c(150, y, 32, v, c, 2);
            }
        }
        rpg_win_contents_end(&EQ.status);
    }
    rpg_win_draw(&EQ.cmd, eqcmd_item, 0);
    rpg_win_draw(&EQ.slot, slot_item, 0);
    rpg_win_draw(&EQ.list, eqitem_item, 0);
}

/* ------------------------------------------------------------------ Scene_Status */
static RpgWin ST;
static RpgActorState *st_actor;

static void status_start(void)
{
    st_actor = menu_actor();
    rpg_win_init(&ST, 0, 0, rpg_screen_w, rpg_screen_h);
    ST.pageup = ST.pagedown = 1;
    ST.ok = 0;
    ST.active = 1;
}

static void status_update(void)
{
    int r = rpg_win_update(&ST);
    if (r == RPG_WIN_CANCEL) {
        rpg_scene_return();
    } else if (r == RPG_WIN_PAGEDOWN || r == RPG_WIN_PAGEUP) {
        rpg_party_menu_actor_step(r == RPG_WIN_PAGEDOWN ? 1 : -1);
        st_actor = menu_actor();
        ST.active = 1;
    }
}

void (*rpg_status_draw_hook)(const RpgActorState *a, float lh);

static void status_draw(void)
{
    rpg_menu_background();
    rpg_win_draw_frame(&ST);
    if (!rpg_win_contents_begin(&ST))
        return;
    float lh = rpg_line_height();
    RpgActorState *a = st_actor;
    if (rpg_status_draw_hook) {
        rpg_status_draw_hook(a, lh);
    } else {
        rpg_draw_actor_name(a, 4, 0, 112);
        rpg_draw_actor_class(a, 128, 0, 160);
        rpg_draw_actor_nickname(a, 288, 0, 200);
        float y = lh;
        rpg_draw_actor_face(a, 8, y, 96, 1);
        rpg_draw_actor_level(a, 136, y);
        rpg_draw_actor_icons(&a->b, 136, y + lh, 96);
        rpg_draw_actor_hp(&a->b, 136, y + lh * 2, 124);
        rpg_draw_actor_mp(&a->b, 136, y + lh * 3, 124);
        int maxed = a->level >= rpg_actor_max_level(a);
        char s1[24], s2[24], nx[64];
        snprintf(s1, sizeof s1, maxed ? "-------" : "%d", a->exp);
        snprintf(s2, sizeof s2, maxed ? "-------" : "%d", rpg_actor_next_level_exp(a) - a->exp);
        snprintf(nx, sizeof nx, rpg_vocab.exp_next, rpg_term_basic(0));
        rpg_draw_text_c(304, y, 180, rpg_vocab.exp_total, rpg_color_system(), 0);
        rpg_draw_text_c(304, y + lh * 2, 180, nx, rpg_color_system(), 0);
        rpg_draw_text_c(304, y + lh, 180, s1, rpg_color_normal(), 2);
        rpg_draw_text_c(304, y + lh * 3, 180, s2, rpg_color_normal(), 2);
        y = lh * 5 + 8;
        for (int i = 0; i < 6; i++)
            rpg_draw_actor_param(a, 32, y + lh * i, i + 2);
        int et[RPG_MAX_SLOTS], n = rpg_actor_slots(a, et);
        for (int i = 0; i < n && i < 6; i++)
            if (a->equip_id[i] > 0)
                rpg_draw_item_name(a->equip_kind[i] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, a->equip_id[i], 288, y + lh * i,
                                   1, 300);
        rpg_fill(0, y + lh * 6 + 2, ST.w - 24, 2, rpg_color_normal() & 0x30ffffff);
        char d[512];
        rpg_convert_escapes(d, sizeof d, RPG_STR(rpg_actor_data(a->actor_id)->description));
        rpg_draw_text_ex(4, y + lh * 6 + 6, d);
    }
    rpg_win_contents_end(&ST);
}

/* ------------------------------------------------------------------ Scene_Name */
static const char *latin[2][90] = {
    {"A", "B", "C", "D", "E", "a", "b", "c", "d", "e", "F", "G", "H", "I", "J", "f", "g", "h", "i", "j",
     "K", "L", "M", "N", "O", "k", "l", "m", "n", "o", "P", "Q", "R", "S", "T", "p", "q", "r", "s", "t",
     "U", "V", "W", "X", "Y", "u", "v", "w", "x", "y", "Z", "[", "]", "^", "_", "z", "{", "}", "|", "~",
     "0", "1", "2", "3", "4", "!", "#", "$", "%", "&", "5", "6", "7", "8", "9", "(", ")", "*", "+", "-",
     "/", "=", "@", "<", ">", ":", ";", " ", "Page", "OK"},
    {"\xc3\x81", "\xc3\x89", "\xc3\x8d", "\xc3\x93", "\xc3\x9a", "\xc3\xa1", "\xc3\xa9", "\xc3\xad", "\xc3\xb3",
     "\xc3\xba", "\xc3\x80", "\xc3\x88", "\xc3\x8c", "\xc3\x92", "\xc3\x99", "\xc3\xa0", "\xc3\xa8", "\xc3\xac",
     "\xc3\xb2", "\xc3\xb9", "\xc3\x82", "\xc3\x8a", "\xc3\x8e", "\xc3\x94", "\xc3\x9b", "\xc3\xa2", "\xc3\xaa",
     "\xc3\xae", "\xc3\xb4", "\xc3\xbb", "\xc3\x84", "\xc3\x8b", "\xc3\x8f", "\xc3\x96", "\xc3\x9c", "\xc3\xa4",
     "\xc3\xab", "\xc3\xaf", "\xc3\xb6", "\xc3\xbc", "\xc4\x80", "\xc4\x92", "\xc4\xaa", "\xc5\x8c", "\xc5\xaa",
     "\xc4\x81", "\xc4\x93", "\xc4\xab", "\xc5\x8d", "\xc5\xab", "\xc3\x83", "\xc3\x85", "\xc3\x86", "\xc3\x87",
     "\xc3\x90", "\xc3\xa3", "\xc3\xa5", "\xc3\xa6", "\xc3\xa7", "\xc3\xb0", "\xc3\x91", "\xc3\x95", "\xc3\x98",
     "\xc5\xa0", "\xc5\xb4", "\xc3\xb1", "\xc3\xb5", "\xc3\xb8", "\xc5\xa1", "\xc5\xb5", "\xc3\x9d", "\xc5\xb6",
     "\xc5\xb8", "\xc5\xbd", "\xc3\x9e", "\xc3\xbd", "\xc3\xbf", "\xc5\xb7", "\xc5\xbe", "\xc3\xbe", "\xc4\xb2",
     "\xc5\x92", "\xc4\xb3", "\xc5\x93", "\xc3\x9f", "\xc2\xab", "\xc2\xbb", " ", "Page", "OK"}};

static struct {
    RpgWin edit, input;
    RpgActorState *actor;
    int max;
    char chars[16][8];
    int n;
    char deflt[RPG_NAME];
    int page;
} NM;

static void name_set(const char *s)
{
    NM.n = 0;
    while (*s && NM.n < NM.max) {
        int len = 1;
        unsigned char c = *s;
        if (c >= 0xf0) len = 4;
        else if (c >= 0xe0) len = 3;
        else if (c >= 0xc0) len = 2;
        memcpy(NM.chars[NM.n], s, len);
        NM.chars[NM.n][len] = 0;
        NM.n++;
        s += len;
    }
}

static void name_get(char *out, int cap)
{
    int k = 0;
    for (int i = 0; i < NM.n; i++)
        k += snprintf(out + k, cap - k, "%s", NM.chars[i]);
    out[k] = 0;
}

static void name_start(void)
{
    NM.actor = rpg_actor(rpg_temp.name_actor);
    NM.max = rpg_temp.name_max > 0 && rpg_temp.name_max <= 16 ? rpg_temp.name_max : 8;
    float eh = rpg_fitting_height(4), ih = rpg_fitting_height(9);
    float y = (rpg_screen_h - (eh + ih + 8)) / 2;
    if (y < 0)
        y = 0;
    float gap = rpg_screen_h - eh - ih >= 8 ? 8 : rpg_screen_h - eh - ih;
    rpg_win_init(&NM.edit, (rpg_screen_w - 360) / 2, y, 360, eh);
    rpg_win_init(&NM.input, NM.edit.x, NM.edit.y + eh + (gap > 0 ? gap : 0), 360, ih);
    if (NM.input.y + ih > rpg_screen_h)
        NM.input.y = rpg_screen_h - ih;
    NM.input.item_max = 90;
    NM.input.index = 0;
    NM.input.active = 0;   /* its cursor moves in name_update (Window_NameInput), not as a plain list */
    NM.input.ok = NM.input.cancel = 0;
    snprintf(NM.deflt, sizeof NM.deflt, "%s", NM.actor->name);
    name_set(NM.deflt);
    NM.page = 0;
}

static void name_update(void)
{
    RpgWin *w = &NM.input;
    rpg_win_update(&NM.edit);
    rpg_win_update(w);
    int last = w->index, lastp = NM.page;
    int i = w->index;
    if (rpg_in.repeat & RPG_K_DOWN) { if (i < 80 || (rpg_in.trigger & RPG_K_DOWN)) i = (i + 10) % 90; }
    if (rpg_in.repeat & RPG_K_UP) { if (i >= 10 || (rpg_in.trigger & RPG_K_UP)) i = (i + 80) % 90; }
    if (rpg_in.repeat & RPG_K_RIGHT) { if (i % 10 < 9) i++; else if (rpg_in.trigger & RPG_K_RIGHT) i -= 9; }
    if (rpg_in.repeat & RPG_K_LEFT) { if (i % 10 > 0) i--; else if (rpg_in.trigger & RPG_K_LEFT) i += 9; }
    if (rpg_in.trigger & RPG_K_R) NM.page = (NM.page + 1) % 2;
    if (rpg_in.trigger & RPG_K_L) NM.page = (NM.page + 1) % 2;
    w->index = i;
    if (w->index != last || NM.page != lastp)
        rpg_play_system_se(RPG_SE_CURSOR);
    if ((rpg_in.trigger & RPG_K_A) && w->index != 89) {
        w->index = 89;
        rpg_play_system_se(RPG_SE_CURSOR);
    }
    if (rpg_in.repeat & RPG_K_B) {
        if (NM.n > 0) {
            NM.n--;
            rpg_play_system_se(RPG_SE_CANCEL);
        }
    }
    if (rpg_in.trigger & RPG_K_C) {
        if (w->index < 88) {
            if (NM.n < NM.max) {
                snprintf(NM.chars[NM.n++], 8, "%s", latin[NM.page][w->index]);
                rpg_play_system_se(RPG_SE_OK);
            } else {
                rpg_play_system_se(RPG_SE_BUZZER);
            }
        } else if (w->index == 88) {
            rpg_play_system_se(RPG_SE_OK);
            NM.page = (NM.page + 1) % 2;
        } else {
            if (NM.n == 0) {
                name_set(NM.deflt);
                rpg_play_system_se(NM.n ? RPG_SE_OK : RPG_SE_BUZZER);
            } else {
                rpg_play_system_se(RPG_SE_OK);
                name_get(NM.actor->name, RPG_NAME);
                rpg_input_consume();
                rpg_scene_return();
            }
        }
    }
}

static void name_draw(void)
{
    rpg_menu_background();
    /* Window_NameEdit */
    rpg_win_draw_frame(&NM.edit);
    float cw = rpg_text_width("A");
    float left = ((NM.edit.w - 24) + 96) / 2 - (NM.max + 1) * cw / 2;
    if (left > (NM.edit.w - 24) - (NM.max + 1) * cw)
        left = (NM.edit.w - 24) - (NM.max + 1) * cw;
    if (rpg_win_contents_begin(&NM.edit)) {
        rpg_draw_actor_face(NM.actor, 0, 0, 96, 1);
        for (int i = 0; i < NM.max; i++)
            rpg_fill(left + i * cw + 1, 36 + 24 - 4, cw - 2, 2, rpg_color_normal() & 0x30ffffff);
        for (int i = 0; i < NM.n; i++)
            rpg_draw_text_c(left + i * cw - 1, 36, cw + 4, NM.chars[i], rpg_color_normal(), 0);
        rpg_win_contents_end(&NM.edit);
        int ci = NM.n < NM.max ? NM.n : NM.max - 1;
        rpg_cursor(NM.edit.x + 12 + left + ci * cw, NM.edit.y + 12 + 36, cw, 24, NM.edit.frame, 0);
    }
    /* Window_NameInput */
    rpg_win_draw_frame(&NM.input);
    if (rpg_win_contents_begin(&NM.input)) {
        for (int i = 0; i < 90; i++) {
            float x = i % 10 * 32 + i % 10 / 5 * 16, y = i / 10 * 24;
            rpg_draw_text_c(x, y, 32, latin[NM.page][i], rpg_color_normal(), 1);
        }
        rpg_win_contents_end(&NM.input);
        int i = NM.input.index;
        rpg_cursor(NM.input.x + 12 + i % 10 * 32 + i % 10 / 5 * 16, NM.input.y + 12 + i / 10 * 24, 32, 24,
                   NM.input.frame, 1);
    }
}

void rpg_shop_define(void);

void rpg_menu2_define(void)
{
    static const RpgSceneDef item = {item_start, item_update, item_draw, 0};
    static const RpgSceneDef skill = {skill_start, skill_update, skill_draw, 0};
    static const RpgSceneDef equip = {equip_start, equip_update, equip_draw, 0};
    static const RpgSceneDef status = {status_start, status_update, status_draw, 0};
    static const RpgSceneDef name = {name_start, name_update, name_draw, 0};
    rpg_scene_define(RPG_SCENE_ITEM, &item);
    rpg_scene_define(RPG_SCENE_SKILL, &skill);
    rpg_scene_define(RPG_SCENE_EQUIP, &equip);
    rpg_scene_define(RPG_SCENE_STATUS, &status);
    rpg_scene_define(RPG_SCENE_NAME, &name);
    rpg_shop_define();
}
