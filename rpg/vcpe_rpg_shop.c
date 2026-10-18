/* VCPE RPG shop: Scene_Shop with its command, buy, sell (category + list), number and status windows, as VX Ace.
 * The goods come from the shop event command (rpg_temp.shop_list: kind, id, price or -1 for the item's own). */
#include "vcpe_rpg.h"
#include "vcpe_rpg_win.h"
#include "vcpe_rpg_menu.h"
#include <string.h>
#include <stdio.h>

static struct {
    RpgWin help, gold, cmd, dummy, buy, number, status, cat, sell;
    int mode;   /* 0 command, 1 buy list, 2 sell category, 3 sell list, 4 number */
    int from;   /* the number window came from: 1 buy, 3 sell */
    int kind, id, price, num, max;
    int goods_kind[256], goods_id[256], goods_price[256], ngoods;
    int sell_kind[RPG_MAX_ITEMS * 3 / 2], sell_id[RPG_MAX_ITEMS * 3 / 2], nsell;
    int page;
} SH;

static int buy_enabled(RpgWin *w, int i)
{
    (void)w;
    if (i < 0 || i >= SH.ngoods)
        return 0;
    int k = SH.goods_kind[i], id = SH.goods_id[i];
    return SH.goods_price[i] <= rpg.party.gold && rpg_party_item_number(k, id) < rpg_party_max_item_number(k, id);
}

static int sell_enabled(RpgWin *w, int i)
{
    (void)w;
    return i >= 0 && i < SH.nsell && rpg_item_price(SH.sell_kind[i], SH.sell_id[i]) > 0;
}

static int cmd_enabled(RpgWin *w, int i)
{
    (void)w;
    return i != 1 || !rpg_temp.shop_purchase_only;
}

static void sell_refresh(void)
{
    int cat = SH.cat.index;
    int kind = cat == 1 ? RPG_KIND_WEAPON : cat == 2 ? RPG_KIND_ARMOR : RPG_KIND_ITEM;
    int count = rpg_count(&rpg_db, kind == RPG_KIND_WEAPON ? RPG_T_WEAPON : kind == RPG_KIND_ARMOR ? RPG_T_ARMOR : RPG_T_ITEM);
    SH.nsell = 0;
    for (int id = 1; id < count && id < RPG_MAX_ITEMS; id++) {
        if (rpg_party_item_number(kind, id) <= 0)
            continue;
        if (kind == RPG_KIND_ITEM && (rpg_item_data(id)->itype_id == 2) != (cat == 3))
            continue;
        SH.sell_kind[SH.nsell] = kind;
        SH.sell_id[SH.nsell++] = id;
    }
    SH.sell.item_max = SH.nsell;
}

static void shop_start(void)
{
    rpg_win_init(&SH.help, 0, 0, rpg_screen_w, rpg_fitting_height(2));
    rpg_win_init(&SH.gold, rpg_screen_w - 160, SH.help.h, 160, rpg_fitting_height(1));
    rpg_win_init(&SH.cmd, 0, SH.help.h, SH.gold.x, rpg_fitting_height(1));
    SH.cmd.col_max = 3;
    SH.cmd.spacing = 8;
    SH.cmd.item_max = 3;
    SH.cmd.enabled = cmd_enabled;
    SH.cmd.active = 1;
    rpg_win_select(&SH.cmd, 0);
    float y = SH.cmd.y + SH.cmd.h, h = rpg_screen_h - y;
    rpg_win_init(&SH.dummy, 0, y, rpg_screen_w, h);
    rpg_win_init(&SH.number, 0, y, 304, h);
    rpg_win_init(&SH.status, 304, y, rpg_screen_w - 304, h);
    rpg_win_init(&SH.buy, 0, y, 304, h);
    SH.buy.enabled = buy_enabled;
    rpg_win_init(&SH.cat, 0, y, rpg_screen_w, rpg_fitting_height(1));
    SH.cat.col_max = 4;
    SH.cat.spacing = 8;
    SH.cat.item_max = 4;
    rpg_win_init(&SH.sell, 0, y + SH.cat.h, rpg_screen_w, rpg_screen_h - y - SH.cat.h);
    SH.sell.col_max = 2;
    SH.sell.enabled = sell_enabled;
    SH.ngoods = 0;
    for (int i = 0; i < rpg_temp.shop_goods_n && SH.ngoods < 256; i++) {
        int k = rpg_temp.shop_list[i][0], id = rpg_temp.shop_list[i][1];
        int kind = k == 0 ? RPG_KIND_ITEM : k == 1 ? RPG_KIND_WEAPON : RPG_KIND_ARMOR;
        if (!((const int32_t *)(kind == RPG_KIND_ITEM ? (const void *)rpg_item_data(id) : (const void *)rpg_equip(kind, id)))[0])
            continue;
        SH.goods_kind[SH.ngoods] = kind;
        SH.goods_id[SH.ngoods] = id;
        SH.goods_price[SH.ngoods] = rpg_temp.shop_list[i][2] >= 0 ? rpg_temp.shop_list[i][2] : rpg_item_price(kind, id);
        SH.ngoods++;
    }
    SH.buy.item_max = SH.ngoods;
    SH.mode = 0;
    SH.page = 0;
}

static void open_number(int kind, int id, int price, int max)
{
    SH.kind = kind;
    SH.id = id;
    SH.price = price;
    SH.max = max;
    SH.num = 1;
    SH.number.active = 1;
    SH.mode = 4;
}

static void shop_update(void)
{
    rpg_win_update(&SH.gold);
    if ((rpg_in.trigger & RPG_K_A) && (SH.mode == 1 || SH.mode == 4)) {
        int pm = (rpg.party.nmembers + 3) / 4;
        if (pm > 1)
            SH.page = (SH.page + 1) % pm;
    }
    switch (SH.mode) {
    case 0: {
        int r = rpg_win_update(&SH.cmd);
        if (r == RPG_WIN_CANCEL || (r == RPG_WIN_OK && SH.cmd.index == 2)) {
            rpg_scene_return();
        } else if (r == RPG_WIN_OK && SH.cmd.index == 0) {
            SH.mode = 1;
            SH.buy.active = 1;
            rpg_win_select(&SH.buy, SH.buy.index < 0 ? 0 : SH.buy.index);
        } else if (r == RPG_WIN_OK) {
            SH.mode = 2;
            SH.cat.active = 1;
            if (SH.cat.index < 0)
                rpg_win_select(&SH.cat, 0);
            SH.sell.index = -1;
            sell_refresh();
        }
        break;
    }
    case 1: {
        int r = rpg_win_update(&SH.buy);
        if (r == RPG_WIN_CANCEL) {
            SH.mode = 0;
            SH.cmd.active = 1;
        } else if (r == RPG_WIN_OK) {
            int i = SH.buy.index, k = SH.goods_kind[i], id = SH.goods_id[i], pr = SH.goods_price[i];
            int max = rpg_party_max_item_number(k, id) - rpg_party_item_number(k, id);
            if (pr > 0 && rpg.party.gold / pr < max)
                max = rpg.party.gold / pr;
            SH.from = 1;
            open_number(k, id, pr, max);
        }
        break;
    }
    case 2: {
        int last = SH.cat.index;
        int r = rpg_win_update(&SH.cat);
        if (SH.cat.index != last)
            sell_refresh();
        if (r == RPG_WIN_CANCEL) {
            SH.mode = 0;
            SH.cmd.active = 1;
        } else if (r == RPG_WIN_OK) {
            SH.mode = 3;
            sell_refresh();
            SH.sell.active = 1;
            rpg_win_select(&SH.sell, 0);
        }
        break;
    }
    case 3: {
        int r = rpg_win_update(&SH.sell);
        if (r == RPG_WIN_CANCEL) {
            SH.sell.index = -1;
            SH.mode = 2;
            SH.cat.active = 1;
        } else if (r == RPG_WIN_OK) {
            int k = SH.sell_kind[SH.sell.index], id = SH.sell_id[SH.sell.index];
            SH.from = 3;
            open_number(k, id, rpg_item_price(k, id) / 2, rpg_party_item_number(k, id));
        }
        break;
    }
    case 4: {
        rpg_win_update(&SH.number);
        int last = SH.num;
        if (rpg_in.repeat & RPG_K_RIGHT) SH.num++;
        if (rpg_in.repeat & RPG_K_LEFT) SH.num--;
        if (rpg_in.repeat & RPG_K_UP) SH.num += 10;
        if (rpg_in.repeat & RPG_K_DOWN) SH.num -= 10;
        if (SH.num > SH.max) SH.num = SH.max;
        if (SH.num < 1) SH.num = 1;
        if (SH.num != last)
            rpg_play_system_se(RPG_SE_CURSOR);
        if (rpg_in.trigger & RPG_K_C) {
            rpg_play_system_se(RPG_SE_SHOP);
            if (SH.from == 1) {
                rpg_party_gain_gold(-SH.num * SH.price);
                rpg_party_gain_item(SH.kind, SH.id, SH.num, 0);
            } else {
                rpg_party_gain_gold(SH.num * SH.price);
                rpg_party_lose_item(SH.kind, SH.id, SH.num, 0);
            }
            rpg_input_consume();
            SH.mode = SH.from;
        } else if (rpg_in.trigger & RPG_K_B) {
            rpg_play_system_se(RPG_SE_CANCEL);
            rpg_input_consume();
            SH.mode = SH.from;
        }
        if (SH.mode == 1) {
            SH.buy.active = 1;
        } else if (SH.mode == 3) {
            sell_refresh();
            SH.sell.active = 1;
            if (SH.sell.index >= SH.nsell)
                SH.sell.index = SH.nsell - 1;
        }
        break;
    }
    }
}

static void cmd_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    const char *t[3] = {rpg_vocab.shop_buy, rpg_vocab.shop_sell, rpg_vocab.shop_cancel};
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, t[i], rpg_color_enabled(rpg_color_normal(), w->enabled(w, i)), 1);
}

static void buy_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int en = w->enabled(w, i);
    rpg_draw_item_name(SH.goods_kind[i], SH.goods_id[i], r.x, r.y, en, r.w - 60);
    char p[16];
    snprintf(p, sizeof p, "%d", SH.goods_price[i]);
    rpg_draw_text_c(r.x, r.y, r.w - 4, p, rpg_color_enabled(rpg_color_normal(), en), 2);
}

static void sell_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)u;
    int en = w->enabled(w, i);
    r.w -= 4;
    rpg_draw_item_name(SH.sell_kind[i], SH.sell_id[i], r.x, r.y, en, r.w - 40);
    char n[16];
    snprintf(n, sizeof n, ":%2d", rpg_party_item_number(SH.sell_kind[i], SH.sell_id[i]));
    rpg_draw_text_c(r.x, r.y, r.w, n, rpg_color_enabled(rpg_color_normal(), en), 2);
}

static void cat_item(RpgWin *w, int i, RpgRect r, void *u)
{
    (void)w; (void)u;
    static const int t[4] = {4, 12, 13, 14};
    rpg_draw_text_c(r.x + 4, r.y, r.w - 8, rpg_term_command(t[i]), rpg_color_normal(), 1);
}

static void draw_status(int kind, int id)
{
    RpgWin *w = &SH.status;
    rpg_win_draw_frame(w);
    if (!rpg_win_contents_begin(w))
        return;
    float lh = rpg_line_height(), cw = w->w - 24;
    char n[16];
    snprintf(n, sizeof n, "%d", id > 0 ? rpg_party_item_number(kind, id) : 0);
    rpg_draw_text_c(4, 0, cw - 8, rpg_vocab.possession, rpg_color_system(), 0);
    rpg_draw_text_c(4, 0, cw - 8, n, rpg_color_normal(), 2);
    if (id > 0 && kind != RPG_KIND_ITEM) {
        const RpgEquip *e = rpg_equip(kind, id);
        int pid = kind == RPG_KIND_WEAPON ? 2 : 3;
        for (int k = 0; k < 4; k++) {
            int mi = SH.page * 4 + k;
            if (mi >= rpg.party.nmembers)
                break;
            RpgActorState *a = rpg_actor(rpg.party.members[mi]);
            float y = lh * 2 + lh * (k * 2.4f);
            int en = rpg_actor_equippable(a, kind == RPG_KIND_ARMOR, id);
            rpg_draw_text_c(4, y, 112, a->name, rpg_color_enabled(rpg_color_normal(), en), 0);
            /* the weakest item of that type now equipped */
            int et[RPG_MAX_SLOTS], ns = rpg_actor_slots(a, et), best = -1, bv = 0;
            for (int s = 0; s < ns; s++)
                if (et[s] == e->etype_id) {
                    int v = a->equip_id[s] > 0 ? rpg_equip(a->equip_kind[s] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, a->equip_id[s])->params[pid] : 0;
                    if (best < 0 || v < bv) {
                        best = s;
                        bv = v;
                    }
                }
            if (en) {
                int ch = e->params[pid] - (best >= 0 ? bv : 0);
                char t[16];
                snprintf(t, sizeof t, "%+d", ch);
                uint32_t c = ch > 0 ? rpg_color_power_up() : ch < 0 ? rpg_color_power_down() : rpg_color_normal();
                rpg_draw_text_c(4, y, cw - 8, t, c, 2);
            }
            if (best >= 0 && a->equip_id[best] > 0)
                rpg_draw_item_name(a->equip_kind[best] ? RPG_KIND_ARMOR : RPG_KIND_WEAPON, a->equip_id[best], 4, y + lh,
                                   en, cw - 8);
        }
    }
    rpg_win_contents_end(w);
}

static void shop_draw(void)
{
    rpg_menu_background();
    const char *desc = "";
    int sk = 0, sid = 0;
    if (SH.mode == 1 && SH.buy.index >= 0 && SH.buy.index < SH.ngoods) {
        sk = SH.goods_kind[SH.buy.index];
        sid = SH.goods_id[SH.buy.index];
    } else if (SH.mode == 3 && SH.sell.index >= 0 && SH.sell.index < SH.nsell) {
        sk = SH.sell_kind[SH.sell.index];
        sid = SH.sell_id[SH.sell.index];
    } else if (SH.mode == 4) {
        sk = SH.kind;
        sid = SH.id;
    }
    if (sid > 0)
        desc = RPG_STR(((const int32_t *)(sk == RPG_KIND_ITEM ? (const void *)rpg_item_data(sid) : (const void *)rpg_equip(sk, sid)))[3]);
    rpg_help_draw(&SH.help, desc);
    rpg_win_draw_frame(&SH.gold);
    if (rpg_win_contents_begin(&SH.gold)) {
        rpg_draw_currency(rpg.party.gold, RPG_STR(rpg_system()->currency_unit), 4, 0, SH.gold.w - 32);
        rpg_win_contents_end(&SH.gold);
    }
    rpg_win_draw(&SH.cmd, cmd_item, 0);
    if (SH.mode == 0)
        rpg_win_draw_frame(&SH.dummy);
    if (SH.mode == 1) {
        rpg_win_draw(&SH.buy, buy_item, 0);
        draw_status(sk, sid);
    }
    if (SH.mode == 2 || SH.mode == 3) {
        rpg_win_draw(&SH.cat, cat_item, 0);
        rpg_win_draw(&SH.sell, sell_item, 0);
    }
    if (SH.mode == 4) {
        RpgWin *w = &SH.number;
        rpg_win_draw_frame(w);
        if (rpg_win_contents_begin(w)) {
            float ch = w->h - 24, lh = rpg_line_height(), iy = ch / 2 - lh * 3 / 2, py = ch / 2 + lh / 2;
            float cwid = 2 * 10 + 12, cx = (w->w - 24) - cwid - 4;
            rpg_draw_item_name(SH.kind, SH.id, 0, iy, 1, cx - 32);
            char n[16];
            snprintf(n, sizeof n, "%d", SH.num);
            rpg_draw_text_c(cx - 28, iy, 22, "\xc3\x97", rpg_color_normal(), 0);
            rpg_draw_text_c(cx, iy, cwid - 4, n, rpg_color_normal(), 2);
            rpg_draw_currency(SH.price * SH.num, RPG_STR(rpg_system()->currency_unit), 4, py, w->w - 24 - 8);
            rpg_win_contents_end(w);
            rpg_cursor(w->x + 12 + cx, w->y + 12 + iy, cwid, lh, w->frame, 1);
        }
        draw_status(SH.kind, SH.id);
    }
}

void rpg_shop_define(void)
{
    static const RpgSceneDef shop = {shop_start, shop_update, shop_draw, 0};
    rpg_scene_define(RPG_SCENE_SHOP, &shop);
}
