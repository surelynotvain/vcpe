/* VCPE RPG map: Game_Map (setup, scrolling, passability, events, common events, the main interpreter's schedule)
 * and Spriteset_Map (parallax, tile layers, characters, animations, pictures, screen effects).
 *
 * Events live in rpg.events[] in the order of the map document (sorted by id); ev_index[] finds them by id. Parallel
 * interpreters: parallel common events take the first slots of rpg.parallel (one per such common event, in database
 * order), parallel map events the following ones (assigned when their page becomes parallel). */
#include "vcpe_rpg.h"
#include "vcpe_tiles.h"
#include "vcpe_spr.h"
#include "vcpe_gfx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

void rpg_tile_draw(int id, float x, float y, uint32_t color);
void rpg_tilemap_init(void);
void rpg_char_draw_sorted(int layer_min, int layer_max);

static const RpgMap *map;
static const RpgTileset *tileset;
static int tile_images[9];
static int16_t ev_index[RPG_MAX_EVENTS + 1];
static int parallax_img = -1;
static char parallax_loaded[RPG_NAME];
static int ce_parallel[RPG_MAX_EVENTS / 4];   /* common event ids with a parallel interpreter slot */
static int nce_parallel;

const RpgMap *rpg_map(void) { return map; }
int rpg_map_width(void) { return map ? map->width : 0; }
int rpg_map_height(void) { return map ? map->height : 0; }
int rpg_map_tile_image(int slot) { return slot >= 0 && slot < 9 ? tile_images[slot] : -1; }
int rpg_screen_tile_x(void) { return rpg_screen_w / 32; }
int rpg_screen_tile_y(void) { return rpg_screen_h / 32; }

int rpg_map_tile(int x, int y, int z)
{
    if (!map)
        return 0;
    return rpg_t16(&rpg_mapdoc, &map->data, x, y, z);
}

int rpg_map_flag(int tile)
{
    if (!tileset || tile < 0)
        return 0;
    return (uint16_t)rpg_t16(&rpg_db, &tileset->flags, tile, 0, 0);
}

int rpg_map_valid(int x, int y) { return map && x >= 0 && y >= 0 && x < map->width && y < map->height; }
int rpg_map_loop_h(void) { return map && (map->scroll_type == 2 || map->scroll_type == 3); }
int rpg_map_loop_v(void) { return map && (map->scroll_type == 1 || map->scroll_type == 3); }
int rpg_round_x(int x) { return rpg_map_loop_h() ? (x + map->width) % map->width : x; }
int rpg_round_y(int y) { return rpg_map_loop_v() ? (y + map->height) % map->height : y; }
int rpg_round_x_dir(int x, int d) { return rpg_round_x(x + (d == 6 ? 1 : d == 4 ? -1 : 0)); }
int rpg_round_y_dir(int y, int d) { return rpg_round_y(y + (d == 2 ? 1 : d == 8 ? -1 : 0)); }

float rpg_adjust_x(float x)
{
    if (rpg_map_loop_h() && x < rpg.display_x - (map->width - rpg_screen_tile_x()) / 2.f)
        return x - rpg.display_x + map->width;
    return x - rpg.display_x;
}

float rpg_adjust_y(float y)
{
    if (rpg_map_loop_v() && y < rpg.display_y - (map->height - rpg_screen_tile_y()) / 2.f)
        return y - rpg.display_y + map->height;
    return y - rpg.display_y;
}

/* ------------------------------------------------------------------ passability */
/* tile events (events showing a tile below characters) count as the topmost tiles of their cell */
static int tile_events_xy(int x, int y, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < rpg.nevents && n < max; i++) {
        RpgChar *e = &rpg.events[i];
        if (e->tile_id > 0 && e->priority_type == 0 && e->x == x && e->y == y && !e->through)
            out[n++] = e->tile_id;
    }
    return n;
}

int rpg_map_check_passage(int x, int y, int bit)
{
    int tiles[16], n = tile_events_xy(x, y, tiles, 12);
    for (int z = 2; z >= 0; z--)
        tiles[n++] = rpg_map_tile(x, y, z);
    for (int i = 0; i < n; i++) {
        int f = rpg_map_flag(tiles[i]);
        if (f & 0x10)
            continue;                 /* star: no effect on passage */
        if (!(f & bit))
            return 1;
        if ((f & bit) == bit)
            return 0;
    }
    return 0;
}

int rpg_map_passable(int x, int y, int d) { return rpg_map_check_passage(x, y, (1 << (d / 2 - 1)) & 0x0f); }

static int layered_flag(int x, int y, int bit)
{
    if (!rpg_map_valid(x, y))
        return 0;
    for (int z = 2; z >= 0; z--)
        if (rpg_map_flag(rpg_map_tile(x, y, z)) & bit)
            return 1;
    return 0;
}
int rpg_map_ladder(int x, int y) { return layered_flag(x, y, 0x20); }
int rpg_map_bush(int x, int y) { return layered_flag(x, y, 0x40); }
int rpg_map_counter(int x, int y) { return layered_flag(x, y, 0x80); }
int rpg_map_damage_floor(int x, int y) { return layered_flag(x, y, 0x100); }

int rpg_map_terrain_tag(int x, int y)
{
    if (!rpg_map_valid(x, y))
        return 0;
    for (int z = 2; z >= 0; z--) {
        int tag = rpg_map_flag(rpg_map_tile(x, y, z)) >> 12;
        if (tag > 0)
            return tag;
    }
    return 0;
}

int rpg_map_region(int x, int y) { return rpg_map_valid(x, y) ? (rpg_map_tile(x, y, 3) >> 8) & 255 : 0; }

/* ------------------------------------------------------------------ scrolling */
void rpg_map_set_display(float x, float y)
{
    if (!rpg_map_loop_h()) {
        float mx = map->width - rpg_screen_w / 32.f;
        x = x > mx ? mx : x;
        x = x < 0 ? 0 : x;
    }
    if (!rpg_map_loop_v()) {
        float my = map->height - rpg_screen_h / 32.f;
        y = y > my ? my : y;
        y = y < 0 ? 0 : y;
    }
    rpg.display_x = fmodf(x + map->width, map->width);
    rpg.display_y = fmodf(y + map->height, map->height);
    rpg.parallax_x = x;
    rpg.parallax_y = y;
}

/* the scroll limits use the real visible size (a screen that is not a whole number of tiles high still reaches the
 * map's bottom row) */
static float max_dx(void) { return map->width - rpg_screen_w / 32.f; }
static float max_dy(void) { return map->height - rpg_screen_h / 32.f; }

void rpg_map_scroll(int d, float dist)
{
    if (!map)
        return;
    if (d == 2) {
        if (rpg_map_loop_v()) {
            rpg.display_y = fmodf(rpg.display_y + dist, map->height);
            if (rpg.parallax_loop_y)
                rpg.parallax_y += dist;
        } else {
            float last = rpg.display_y, m = max_dy();
            rpg.display_y = rpg.display_y + dist < m ? rpg.display_y + dist : (m > rpg.display_y ? m : rpg.display_y);
            rpg.parallax_y += rpg.display_y - last;
        }
    } else if (d == 4) {
        if (rpg_map_loop_h()) {
            rpg.display_x = fmodf(rpg.display_x + map->width - dist, map->width);
            if (rpg.parallax_loop_x)
                rpg.parallax_x -= dist;
        } else {
            float last = rpg.display_x;
            rpg.display_x = rpg.display_x - dist > 0 ? rpg.display_x - dist : 0;
            rpg.parallax_x += rpg.display_x - last;
        }
    } else if (d == 6) {
        if (rpg_map_loop_h()) {
            rpg.display_x = fmodf(rpg.display_x + dist, map->width);
            if (rpg.parallax_loop_x)
                rpg.parallax_x += dist;
        } else {
            float last = rpg.display_x, m = max_dx();
            rpg.display_x = rpg.display_x + dist < m ? rpg.display_x + dist : (m > rpg.display_x ? m : rpg.display_x);
            rpg.parallax_x += rpg.display_x - last;
        }
    } else if (d == 8) {
        if (rpg_map_loop_v()) {
            rpg.display_y = fmodf(rpg.display_y + map->height - dist, map->height);
            if (rpg.parallax_loop_y)
                rpg.parallax_y -= dist;
        } else {
            float last = rpg.display_y;
            rpg.display_y = rpg.display_y - dist > 0 ? rpg.display_y - dist : 0;
            rpg.parallax_y += rpg.display_y - last;
        }
    }
}

static void update_scroll(void)
{
    if (rpg.scroll_rest <= 0)
        return;
    float lx = rpg.display_x, ly = rpg.display_y, dist = (float)(1 << rpg.scroll_speed) / 256.f;
    rpg_map_scroll(rpg.scroll_dir, dist);
    if (rpg.display_x == lx && rpg.display_y == ly)
        rpg.scroll_rest = 0;
    else
        rpg.scroll_rest -= dist;
}

/* ------------------------------------------------------------------ events */
const RpgEvent *rpg_event_data(int index)
{
    return (const RpgEvent *)rpg_at(&rpg_mapdoc, RPG_T_EVENT, map->events, index);
}

int rpg_map_event_index(int id) { return (id > 0 && id <= RPG_MAX_EVENTS) ? ev_index[id] : -1; }

RpgChar *rpg_map_event(int id)
{
    int i = rpg_map_event_index(id);
    return i >= 0 ? &rpg.events[i] : 0;
}

const RpgPage *rpg_event_page(const RpgChar *e)
{
    if (e->page < 0)
        return 0;
    const RpgEvent *ev = rpg_event_data(rpg_map_event_index(e->id));
    return (const RpgPage *)rpg_at(&rpg_mapdoc, RPG_T_PAGE, ev->pages, e->page);
}

RpgList rpg_event_list(const RpgChar *e)
{
    const RpgPage *p = rpg_event_page(e);
    RpgList l = {&rpg_mapdoc, {0, 0}};
    if (p)
        l.r = p->list;
    return l;
}

int rpg_party_has_item(int kind, int id, int include_equip);
int rpg_party_member(int actor_id);

static int page_ok(const RpgChar *e, const RpgPage *p)
{
    const RpgPageCond *c = &p->condition;
    if (c->switch1_valid && !rpg_sw(c->switch1_id))
        return 0;
    if (c->switch2_valid && !rpg_sw(c->switch2_id))
        return 0;
    if (c->variable_valid && rpg_var(c->variable_id) < c->variable_value)
        return 0;
    if (c->self_switch_valid) {
        const char *ch = rpg_s(&rpg_mapdoc, c->self_switch_ch);
        if (!rpg_self_sw(rpg.map_id, e->id, ch[0] - 'A'))
            return 0;
    }
    if (c->item_valid && !rpg_party_has_item(0, c->item_id, 0))
        return 0;
    if (c->actor_valid && !rpg_party_member(c->actor_id))
        return 0;
    return 1;
}

static int alloc_parallel(void)
{
    for (int i = nce_parallel; i < (int)(sizeof rpg.parallel / sizeof *rpg.parallel); i++) {
        int used = 0;
        for (int k = 0; k < rpg.nevents && !used; k++)
            used = rpg.events[k].interp == i;
        if (!used) {
            rpg_interp_clear(&rpg.parallel[i]);
            if (i >= rpg.nparallel)
                rpg.nparallel = i + 1;
            return i;
        }
    }
    return -1;
}

static void setup_page(RpgChar *e, int page)
{
    const RpgEvent *ev = rpg_event_data(rpg_map_event_index(e->id));
    const RpgPage *p = page >= 0 ? (const RpgPage *)rpg_at(&rpg_mapdoc, RPG_T_PAGE, ev->pages, page) : 0;
    e->page = page;
    e->has_page = p != 0;
    if (p) {
        const RpgPageGraphic *g = &p->graphic;
        e->tile_id = g->tile_id;
        snprintf(e->character_name, sizeof e->character_name, "%s", rpg_s(&rpg_mapdoc, g->character_name));
        e->character_index = g->character_index;
        if (e->original_direction != g->direction) {
            e->direction = g->direction;
            e->original_direction = e->direction;
            e->prelock_direction = 0;
        }
        if (e->original_pattern != g->pattern) {
            e->pattern = g->pattern;
            e->original_pattern = e->pattern;
        }
        e->move_type = p->move_type;
        e->move_speed = p->move_speed;
        e->move_frequency = p->move_frequency;
        e->route_doc = RPG_DOC_MAP + rpg.map_id;
        e->route = p->move_route;
        e->route_index = 0;
        e->move_route_forcing = 0;
        e->walk_anime = p->walk_anime;
        e->step_anime = p->step_anime;
        e->direction_fix = p->direction_fix;
        e->through = p->through;
        e->priority_type = p->priority_type;
        e->trigger = p->trigger;
        if (e->trigger == 4) {
            if (e->interp < 0)
                e->interp = alloc_parallel();
            else
                rpg_interp_clear(&rpg.parallel[e->interp]);
        } else {
            e->interp = -1;
        }
    } else {
        e->tile_id = 0;
        e->character_name[0] = 0;
        e->character_index = 0;
        e->move_type = 0;
        e->through = 1;
        e->trigger = -1;
        e->interp = -1;
    }
    rpg_char_update_bush(e);
    e->starting = 0;
    if (e->trigger == 3)
        rpg_event_start(e);
}

void rpg_event_refresh(RpgChar *e)
{
    int np = -1;
    if (!e->erased) {
        const RpgEvent *ev = rpg_event_data(rpg_map_event_index(e->id));
        for (int i = ev->pages.count - 1; i >= 0; i--) {
            const RpgPage *p = (const RpgPage *)rpg_at(&rpg_mapdoc, RPG_T_PAGE, ev->pages, i);
            if (page_ok(e, p)) {
                np = i;
                break;
            }
        }
    }
    if (np < 0 || np != e->page)
        setup_page(e, np);
}

void rpg_event_start(RpgChar *e)
{
    RpgList l = rpg_event_list(e);
    if (l.r.count <= 1)
        return;   /* empty? */
    e->starting = 1;
    if (e->trigger >= 0 && e->trigger <= 2 && !e->locked) {
        e->prelock_direction = e->direction;
        rpg_char_turn_toward(e, &rpg.player.ch);
        e->locked = 1;
    }
}

void rpg_event_erase(RpgChar *e)
{
    e->erased = 1;
    rpg_event_refresh(e);
}

static void init_event(RpgChar *e, const RpgEvent *ev)
{
    memset(e, 0, sizeof *e);
    e->id = ev->id;
    e->kind = 0;
    e->map_id = rpg.map_id;
    e->move_speed = 4;
    e->move_frequency = 6;
    e->walk_anime = 1;
    e->opacity = 255;
    e->direction = 2;
    e->pattern = 1;
    e->priority_type = 1;
    e->original_direction = 2;
    e->original_pattern = 1;
    e->move_succeed = 1;
    e->page = -1;
    e->interp = -1;
    e->route = -1;
    e->orig_route = -1;
    rpg_char_moveto(e, ev->x, ev->y);
}

static void refresh_common(void)
{
    /* Game_CommonEvent#refresh: the interpreter exists while the switch is on, and is dropped (stopped) when off */
    for (int i = 0; i < nce_parallel; i++) {
        const RpgCommonEvent *ce = rpg_common_data(ce_parallel[i]);
        int active = ce->trigger == 2 && rpg_sw(ce->switch_id);
        RpgInterp *in = &rpg.parallel[i];
        if (!active && (in->depth || in->main >= 0)) {
            rpg_interp_clear(in);
            in->main = -1;   /* -1: not active */
        } else if (active && in->main < 0) {
            in->main = 0;
        }
    }
}

void rpg_map_refresh(void)
{
    for (int i = 0; i < rpg.nevents; i++)
        rpg_event_refresh(&rpg.events[i]);
    refresh_common();
    rpg.need_refresh = 0;
}

static int load_tiles(void)
{
    tileset = rpg_tileset_data(rpg.tileset_id);
    for (int i = 0; i < 9; i++)
        tile_images[i] = rpg_image("tilesets", rpg_s(&rpg_db, tileset->tileset_names[i]));
    return 0;
}

void rpg_map_change_tileset(int id)
{
    rpg.tileset_id = id;
    load_tiles();
    rpg_map_refresh();
}

void rpg_map_change_parallax(const char *name, int lx, int ly, int sx, int sy)
{
    snprintf(rpg.parallax_name, sizeof rpg.parallax_name, "%s", name);
    if (rpg.parallax_loop_x && !lx)
        rpg.parallax_x = 0;
    if (rpg.parallax_loop_y && !ly)
        rpg.parallax_y = 0;
    rpg.parallax_loop_x = lx;
    rpg.parallax_loop_y = ly;
    rpg.parallax_sx = sx;
    rpg.parallax_sy = sy;
}

/* ------------------------------------------------------------------ map documents
 * A small pool: the current map, and maps still referenced by an interpreter frame or a move route. */
#define POOL 6
static struct {
    int map;
    void *mem;
    RpgDoc doc;
    uint32_t used;
} pool[POOL];
static uint32_t pool_clock;

static int referenced(int m)
{
    if (m == rpg.map_id)
        return 1;
    RpgInterp *ins[1 + sizeof rpg.parallel / sizeof *rpg.parallel];
    int n = 0;
    ins[n++] = &rpg.interp;
    for (int i = 0; i < rpg.nparallel; i++)
        ins[n++] = &rpg.parallel[i];
    for (int i = 0; i < n; i++)
        for (int k = 0; k < ins[i]->depth; k++)
            if (ins[i]->f[k].src == 0 && ins[i]->f[k].map == m)
                return 1;
    if (rpg.player.ch.route_doc == RPG_DOC_MAP + m || rpg.player.ch.orig_route_doc == RPG_DOC_MAP + m)
        return 1;
    return 0;
}

const RpgDoc *rpg_map_doc(int m)
{
    for (int i = 0; i < POOL; i++)
        if (pool[i].mem && pool[i].map == m) {
            pool[i].used = ++pool_clock;
            return &pool[i].doc;
        }
    /* a free slot, else the least recently used one that nothing references */
    int slot = -1;
    for (int i = 0; i < POOL && slot < 0; i++)
        if (!pool[i].mem)
            slot = i;
    for (int i = 0; i < POOL && slot < 0; i++) {
        int best = -1;
        for (int k = 0; k < POOL; k++)
            if (pool[k].mem && !referenced(pool[k].map) && (best < 0 || pool[k].used < pool[best].used))
                best = k;
        slot = best;
    }
    if (slot < 0) {
        rpg_log("RPG map pool full (map %d)", m);
        return 0;
    }
    if (pool[slot].mem) {
        if (rpg_hooks.free_map) rpg_hooks.free_map(pool[slot].mem);
        else free(pool[slot].mem);
        pool[slot].mem = 0;
    }
    const void *bytes;
    uint32_t size;
    if (!rpg_hooks.load_map || rpg_hooks.load_map(m, &bytes, &size)) {
        rpg_log("RPG map %d could not be loaded", m);
        return 0;
    }
    if (rpg_doc_open(&pool[slot].doc, bytes, size)) {
        rpg_log("RPG map %d: bad document", m);
        if (rpg_hooks.free_map) rpg_hooks.free_map((void *)bytes);
        else free((void *)bytes);
        return 0;
    }
    pool[slot].mem = (void *)bytes;
    pool[slot].map = m;
    pool[slot].used = ++pool_clock;
    return &pool[slot].doc;
}

int rpg_doc_id(const RpgDoc *d)
{
    if (!d)
        return RPG_DOC_NONE;
    if (d == &rpg_db || d->base == rpg_db.base)
        return RPG_DOC_DB;
    if (d == &rpg_mapdoc || d->base == rpg_mapdoc.base)
        return RPG_DOC_MAP + rpg.map_id;
    for (int i = 0; i < POOL; i++)
        if (pool[i].mem && pool[i].doc.base == d->base)
            return RPG_DOC_MAP + pool[i].map;
    return RPG_DOC_NONE;
}

const RpgEvent *rpg_doc_event(const RpgDoc *d, int id)
{
    const RpgMap *m = (const RpgMap *)rpg_rec(d, RPG_T_MAP, 0);
    int lo = 0, hi = m->events.count;
    while (lo < hi) {   /* events are sorted by id */
        int mid = (lo + hi) / 2;
        const RpgEvent *e = (const RpgEvent *)rpg_rec(d, RPG_T_EVENT, m->events.first + mid);
        if (e->id == id)
            return e;
        if (e->id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static int open_map(int id)
{
    const RpgDoc *d = rpg_map_doc(id);
    if (!d)
        return -1;
    rpg_mapdoc = *d;
    map = (const RpgMap *)rpg_rec(&rpg_mapdoc, RPG_T_MAP, 0);
    return 0;
}

/* Game_Map#setup */
int rpg_map_load(int id)
{
    if (open_map(id))
        return -1;
    rpg.map_id = id;
    rpg.tileset_id = map->tileset_id;
    rpg.display_x = rpg.display_y = 0;
    load_tiles();
    /* events */
    memset(ev_index, -1, sizeof ev_index);
    rpg.nevents = map->events.count < RPG_MAX_EVENTS ? map->events.count : RPG_MAX_EVENTS;
    for (int i = 0; i < rpg.nevents; i++) {
        const RpgEvent *ev = rpg_event_data(i);
        if (ev->id > 0 && ev->id <= RPG_MAX_EVENTS)
            ev_index[ev->id] = i;
    }
    /* parallel common events: one interpreter each */
    nce_parallel = 0;
    for (int i = 1; i < rpg_count(&rpg_db, RPG_T_COMMONEVENT) && nce_parallel < (int)(sizeof ce_parallel / sizeof *ce_parallel); i++)
        if (rpg_common_data(i)->trigger == 2)
            ce_parallel[nce_parallel++] = i;
    rpg.nparallel = nce_parallel;
    for (int i = 0; i < nce_parallel; i++) {
        rpg_interp_clear(&rpg.parallel[i]);
        rpg.parallel[i].main = -1;
    }
    for (int i = 0; i < rpg.nevents; i++)
        init_event(&rpg.events[i], rpg_event_data(i));
    for (int i = 0; i < rpg.nevents; i++)
        rpg_event_refresh(&rpg.events[i]);
    refresh_common();
    /* scroll, parallax, battleback */
    rpg.scroll_dir = 2;
    rpg.scroll_rest = 0;
    rpg.scroll_speed = 4;
    snprintf(rpg.parallax_name, sizeof rpg.parallax_name, "%s", rpg_s(&rpg_mapdoc, map->parallax_name));
    rpg.parallax_loop_x = map->parallax_loop_x;
    rpg.parallax_loop_y = map->parallax_loop_y;
    rpg.parallax_sx = map->parallax_sx;
    rpg.parallax_sy = map->parallax_sy;
    rpg.parallax_x = rpg.parallax_y = 0;
    if (map->specify_battleback) {
        snprintf(rpg.battleback1, sizeof rpg.battleback1, "%s", rpg_s(&rpg_mapdoc, map->battleback1_name));
        snprintf(rpg.battleback2, sizeof rpg.battleback2, "%s", rpg_s(&rpg_mapdoc, map->battleback2_name));
    } else {
        rpg.battleback1[0] = rpg.battleback2[0] = 0;
    }
    rpg.need_refresh = 0;
    if (rpg_hooks.map_setup)
        rpg_hooks.map_setup(id);
    return 0;
}

/* after loading a save: the map document again, the derived tables, nothing reset */
int rpg_map_reopen(void)
{
    if (open_map(rpg.map_id))
        return -1;
    load_tiles();
    memset(ev_index, -1, sizeof ev_index);
    for (int i = 0; i < rpg.nevents; i++) {
        const RpgEvent *ev = rpg_event_data(i);
        if (ev->id > 0 && ev->id <= RPG_MAX_EVENTS)
            ev_index[ev->id] = i;
    }
    nce_parallel = 0;
    for (int i = 1; i < rpg_count(&rpg_db, RPG_T_COMMONEVENT) && nce_parallel < (int)(sizeof ce_parallel / sizeof *ce_parallel); i++)
        if (rpg_common_data(i)->trigger == 2)
            ce_parallel[nce_parallel++] = i;
    return 0;
}

void rpg_map_autoplay(void)
{
    if (map->autoplay_bgm)
        rpg_bgm_play(rpg_s(&rpg_mapdoc, map->bgm.name), map->bgm.volume, map->bgm.pitch);
    if (map->autoplay_bgs)
        rpg_bgs_play(rpg_s(&rpg_mapdoc, map->bgs.name), map->bgs.volume, map->bgs.pitch);
}

int rpg_map_events_xy(int x, int y, RpgChar **out, int max, int nt)
{
    int n = 0;
    for (int i = 0; i < rpg.nevents && n < max; i++) {
        RpgChar *e = &rpg.events[i];
        if (e->x == x && e->y == y && (!nt || !e->through))
            out[n++] = e;
    }
    return n;
}

int rpg_map_any_starting(void)
{
    for (int i = 0; i < rpg.nevents; i++)
        if (rpg.events[i].starting)
            return 1;
    return 0;
}

/* Game_Map#setup_starting_event: reserved common event, then a starting map event, then an autorun common event */
int rpg_map_setup_starting_event(void)
{
    if (rpg.need_refresh)
        rpg_map_refresh();
    if (rpg.reserved_common > 0) {
        rpg_interp_setup(&rpg.interp, 1, rpg.reserved_common, 0, 0);
        rpg.reserved_common = 0;
        return 1;
    }
    for (int i = 0; i < rpg.nevents; i++) {
        RpgChar *e = &rpg.events[i];
        if (e->starting) {
            e->starting = 0;
            rpg_interp_setup(&rpg.interp, 0, e->id, e->page, e->id);
            return 1;
        }
    }
    for (int i = 1; i < rpg_count(&rpg_db, RPG_T_COMMONEVENT); i++) {
        const RpgCommonEvent *ce = rpg_common_data(i);
        if (ce->trigger == 1 && rpg_sw(ce->switch_id)) {
            rpg_interp_setup(&rpg.interp, 1, i, 0, 0);
            return 1;
        }
    }
    return 0;
}

static void update_interpreter(void)
{
    for (int guard = 0; guard < 100; guard++) {
        rpg_interp_update(&rpg.interp);
        if (rpg_interp_running(&rpg.interp))
            return;
        if (rpg.interp.f[0].event_id > 0) {
            RpgChar *e = rpg_map_event(rpg.interp.f[0].event_id);
            if (e && e->locked) {
                e->locked = 0;
                rpg_char_set_direction(e, e->prelock_direction);
            }
            rpg_interp_clear(&rpg.interp);
        }
        if (!rpg_map_setup_starting_event())
            return;
    }
}

static void update_event(RpgChar *e)
{
    rpg_char_update(e);
    if (e->trigger == 3)
        rpg_event_start(e);
    if (e->interp >= 0 && e->trigger == 4) {
        RpgInterp *in = &rpg.parallel[e->interp];
        if (!rpg_interp_running(in))
            rpg_interp_setup(in, 0, e->id, e->page, e->id);
        rpg_interp_update(in);
    }
}

/* Game_Map#update */
void rpg_map_update(int main)
{
    if (!map)
        return;
    if (rpg.need_refresh)
        rpg_map_refresh();
    if (main)
        update_interpreter();
    update_scroll();
    for (int i = 0; i < rpg.nevents; i++)
        update_event(&rpg.events[i]);
    for (int i = 0; i < nce_parallel; i++) {
        RpgInterp *in = &rpg.parallel[i];
        if (in->main < 0)
            continue;
        if (!rpg_interp_running(in))
            rpg_interp_setup(in, 1, ce_parallel[i], 0, 0);
        rpg_interp_update(in);
    }
    if (rpg.parallax_loop_x)
        rpg.parallax_x += rpg.parallax_sx / 64.f;
    if (rpg.parallax_loop_y)
        rpg.parallax_y += rpg.parallax_sy / 64.f;
    rpg_screen_update(&rpg.screen);
    if (rpg_hooks.map_update)
        rpg_hooks.map_update();
}

/* ------------------------------------------------------------------ drawing */
static void draw_parallax(void)
{
    if (!rpg.parallax_name[0])
        return;
    if (strcmp(parallax_loaded, rpg.parallax_name)) {
        snprintf(parallax_loaded, sizeof parallax_loaded, "%s", rpg.parallax_name);
        parallax_img = rpg_image("parallaxes", rpg.parallax_name);
    }
    if (parallax_img < 0)
        return;
    int bw = rpg_image_w(parallax_img), bh = rpg_image_h(parallax_img);
    float ox, oy;
    if (rpg.parallax_loop_x) {
        ox = rpg.parallax_x * 16;
    } else {
        float w1 = bw - rpg_screen_w > 0 ? bw - rpg_screen_w : 0, w2 = map->width * 32 - rpg_screen_w;
        ox = rpg.parallax_x * 16 * w1 / (w2 > 1 ? w2 : 1);
    }
    if (rpg.parallax_loop_y) {
        oy = rpg.parallax_y * 16;
    } else {
        float h1 = bh - rpg_screen_h > 0 ? bh - rpg_screen_h : 0, h2 = map->height * 32 - rpg_screen_h;
        oy = rpg.parallax_y * 16 * h1 / (h2 > 1 ? h2 : 1);
    }
    if (rpg_hooks.parallax_origin)
        rpg_hooks.parallax_origin(&ox, &oy);
    /* a Plane: the image tiled over the screen, shifted by (ox, oy) */
    float sx = -fmodf(ox, bw), sy = -fmodf(oy, bh);
    if (sx > 0) sx -= bw;
    if (sy > 0) sy -= bh;
    for (float y = sy; y < rpg_screen_h; y += bh)
        for (float x = sx; x < rpg_screen_w; x += bw)
            rpg_draw_part(parallax_img, 0, 0, bw, bh, x, y, 1, 1, 0, 0xffffffff, 0);
}

/* the visible cells of one layer pass; above = 0: tiles under characters, 1: star tiles */
static void draw_tiles(int above, float shx)
{
    float dx = rpg.display_x * 32 - shx, dy = rpg.display_y * 32;
    int x0 = (int)floorf(rpg.display_x) - 1, y0 = (int)floorf(rpg.display_y) - 1;
    int x1 = x0 + rpg_screen_w / 32 + 3, y1 = y0 + rpg_screen_h / 32 + 3;
    float s = rpg_scale;
    for (int z = 0; z < 3; z++) {
        /* tables overhang into the next row: keep painter order there, else group by page */
        vcpe_tiles_begin(dx - rpg_ox / s, dy - rpg_oy / s, s, s, z < 2 ? 0 : 1);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                int mx = x, my = y;
                if (rpg_map_loop_h()) mx = ((x % map->width) + map->width) % map->width;
                if (rpg_map_loop_v()) my = ((y % map->height) + map->height) % map->height;
                if (!rpg_map_valid(mx, my))
                    continue;
                int id = rpg_map_tile(mx, my, z);
                if (!id)
                    continue;
                int star = (rpg_map_flag(id) & 0x10) != 0;
                if (star != above)
                    continue;
                rpg_tile_draw(id, x * 32.f, y * 32.f, 0xffffffff);
            }
        }
        vcpe_tiles_end();
        if (z == 1 && !above) {
            /* shadows: the low 4 bits of layer 3, one 16x16 half-black square per quarter */
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    int mx = x, my = y;
                    if (rpg_map_loop_h()) mx = ((x % map->width) + map->width) % map->width;
                    if (rpg_map_loop_v()) my = ((y % map->height) + map->height) % map->height;
                    if (!rpg_map_valid(mx, my))
                        continue;
                    int sh = rpg_map_tile(mx, my, 3) & 15;
                    if (!sh)
                        continue;
                    for (int q = 0; q < 4; q++)
                        if (sh & (1 << q))
                            rpg_fill(x * 32 - dx + (q & 1) * 16, y * 32 - dy + (q >> 1) * 16, 16, 16, 0x80000000);
                }
        }
    }
}

float rpg_shake_x(void) { return rpg.screen.shake; }

void rpg_map_draw(void)
{
    if (!map)
        return;
    rpg_tile_anim = rpg.frame_count;
    float shx = rpg.screen.shake;
    rpg_clip(0, 0, rpg_screen_w, rpg_screen_h);
    draw_parallax();
    vcpe_spr_filter(1);
    draw_tiles(0, shx);
    rpg_char_draw_sorted(0, 1);     /* below + same as characters */
    draw_tiles(1, shx);
    rpg_char_draw_sorted(2, 2);     /* above characters */
    rpg_anims_draw();
    rpg_screen_draw_tone_flash(&rpg.screen);   /* the tone is viewport 1's: map, characters, animations */
    if (rpg_hooks.map_draw_fog)
        rpg_hooks.map_draw_fog();
    rpg_screen_draw_weather(&rpg.screen);
    rpg_screen_draw_pictures(&rpg.screen);
    if (rpg_hooks.map_draw_over)
        rpg_hooks.map_draw_over();
    rpg_clip_off();
}
