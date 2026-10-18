/* VCPE RPG documents: see vcpe_rpg.h and tools/rpgmaker/vrpg.py for the layout. Opening a document only checks the
 * header and every table against the size, then keeps pointers into the caller's bytes: nothing is copied. Any record
 * index out of range reads as an all-zero record, the way a missing database entry reads as nil in RPG Maker. */
#include "vcpe_rpg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

RpgDoc rpg_db, rpg_mapdoc;
static const int slots[RPG_NTABLES] = RPG_DB_SLOTS;
static const int32_t zero_rec[256];
void (*rpg_log_hook)(const char *line);

int rpg_doc_open(RpgDoc *d, const void *bytes, uint32_t size)
{
    const uint8_t *b = bytes;
    memset(d, 0, sizeof *d);
    if (size < 32 || memcmp(b, "VRPG", 4))
        return -1;
    const uint32_t *h = (const uint32_t *)(b + 4);
    uint32_t ntables = h[1], so = h[2], ss = h[3], io = h[4], ic = h[5];
    if (h[0] != 1 || ntables != RPG_NTABLES || 32 + 12 * ntables > size)
        return -2;
    if (so + ss > size || io + ic * 2 > size || (io & 1))
        return -3;
    const uint32_t *dir = (const uint32_t *)(b + 32);
    for (int t = 0; t < RPG_NTABLES; t++) {
        uint32_t off = dir[t * 3], count = dir[t * 3 + 1], sl = dir[t * 3 + 2];
        if ((int)sl != slots[t] || (off & 3) || off + (uint64_t)count * sl * 4 > size)
            return -4;
        d->tab[t] = (const int32_t *)(b + off);
        d->count[t] = count;
    }
    d->base = b;
    d->size = size;
    d->str = (const char *)(b + so);
    d->nstr = ss;
    d->i16 = (const int16_t *)(b + io);
    d->n16 = ic;
    return 0;
}

const void *rpg_rec(const RpgDoc *d, int t, int i)
{
    if (!d || t < 0 || t >= RPG_NTABLES || i < 0 || i >= d->count[t])
        return zero_rec;
    return d->tab[t] + (size_t)i * slots[t];
}

const char *rpg_s(const RpgDoc *d, int32_t off)
{
    if (!d || off <= 0 || (uint32_t)off >= d->nstr)
        return "";
    return d->str + off;
}

int rpg_t16(const RpgDoc *d, const RpgTable *t, int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= t->xsize || (t->ysize && y >= t->ysize) || (t->zsize && z >= t->zsize))
        return 0;
    int ys = t->ysize ? t->ysize : 1;
    uint32_t i = (uint32_t)x + (uint32_t)t->xsize * ((uint32_t)y + (uint32_t)ys * (uint32_t)z);
    if ((int)i >= t->count || (uint32_t)(t->first + i) >= d->n16)
        return 0;
    return d->i16[t->first + i];
}

#define ACCESS(fn, type, table) \
    const type *fn(int id) { return (const type *)rpg_rec(&rpg_db, table, id); }
ACCESS(rpg_actor_data, RpgActor, RPG_T_ACTOR)
ACCESS(rpg_class_data, RpgClass, RPG_T_CLASS)
ACCESS(rpg_skill_data, RpgSkill, RPG_T_SKILL)
ACCESS(rpg_item_data, RpgItem, RPG_T_ITEM)
ACCESS(rpg_weapon_data, RpgWeapon, RPG_T_WEAPON)
ACCESS(rpg_armor_data, RpgArmor, RPG_T_ARMOR)
ACCESS(rpg_enemy_data, RpgEnemy, RPG_T_ENEMY)
ACCESS(rpg_state_data, RpgState, RPG_T_STATE)
ACCESS(rpg_troop_data, RpgTroop, RPG_T_TROOP)
ACCESS(rpg_anim_data, RpgAnimation, RPG_T_ANIMATION)
ACCESS(rpg_tileset_data, RpgTileset, RPG_T_TILESET)
ACCESS(rpg_common_data, RpgCommonEvent, RPG_T_COMMONEVENT)
ACCESS(rpg_mapinfo, RpgMapInfo, RPG_T_MAPINFO)
const RpgSystem *rpg_system(void) { return (const RpgSystem *)rpg_rec(&rpg_db, RPG_T_SYSTEM, 0); }

const char *rpg_note_find(const char *note, const char *tag)
{
    if (!note || !tag)
        return 0;
    const char *p = strstr(note, tag);
    return p ? p + strlen(tag) : 0;
}

int rpg_note_int(const char *note, const char *tag, int dflt)
{
    const char *p = rpg_note_find(note, tag);
    return p ? atoi(p) : dflt;
}

void rpg_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (rpg_log_hook)
        rpg_log_hook(line);
}

const RpgDoc *rpg_doc_by_id(int id)
{
    if (id == RPG_DOC_DB)
        return &rpg_db;
    if (id > RPG_DOC_MAP)
        return rpg_map_doc(id - RPG_DOC_MAP);
    return 0;
}
