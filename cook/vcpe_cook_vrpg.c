/* VCPE cook: RPG Maker VX Ace data -> VRPG documents, tools/rpgmaker/vrpg.py in C and byte-identical to it. The
 * record layouts come from rpg/vcpe_rpg_schema.h (generated from the same schema as the Python writer). */
#include "vcpe_cook.h"
#include "vcpe_rpg_schema.h"
#include <stdlib.h>
#include <string.h>

#define NT ((int)(sizeof rpg_schema / sizeof rpg_schema[0]))
#define VRPG_VERSION 1

typedef struct { uint32_t *v; uint32_t rows, cap; } Table;   /* rows x slots words */

typedef struct {
    Table t[NT];
    uint8_t *str;
    uint32_t nstr, capstr;
    uint32_t *sh;          /* string hash: offset + 1 */
    uint32_t shcap, shn;
    int16_t *i16;
    uint32_t ni16, capi16;
    int failed;
    char *err;
    int errcap;
} Doc;

static int table_index(const char *name)
{
    for (int i = 0; i < NT; i++)
        if (!strcmp(rpg_schema[i].name, name))
            return i;
    return -1;
}

static void doc_fail(Doc *d, const char *m, const char *x)
{
    if (!d->failed && d->err)
        snprintf(d->err, d->errcap, "%s %s", m, x ? x : "");
    d->failed = 1;
}

/* ------------------------------------------------------------------ pools */
static uint32_t shash(const uint8_t *s, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

static void str_grow_hash(Doc *d)
{
    uint32_t cap = d->shcap ? d->shcap * 2 : 8192;
    uint32_t *nh = calloc(cap, 4);
    for (uint32_t i = 0; i < d->shcap; i++)
        if (d->sh[i]) {
            const uint8_t *s = d->str + d->sh[i] - 1;
            uint32_t h = shash(s, (uint32_t)strlen((const char *)s)) & (cap - 1);
            while (nh[h])
                h = (h + 1) & (cap - 1);
            nh[h] = d->sh[i];
        }
    free(d->sh);
    d->sh = nh;
    d->shcap = cap;
}

/* Doc.string: NUL bytes removed, deduplicated, offset 0 = "" */
static uint32_t dstring(Doc *d, const char *s0, uint32_t n0)
{
    if (!s0)
        return 0;
    char tmp[4096], *s = n0 < sizeof tmp ? tmp : malloc(n0 + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < n0; i++)
        if (s0[i])
            s[n++] = s0[i];
    s[n] = 0;
    uint32_t r = 0;
    if (n) {
        if (d->shn * 2 >= d->shcap)
            str_grow_hash(d);
        uint32_t h = shash((uint8_t *)s, n) & (d->shcap - 1);
        while (d->sh[h]) {
            const char *o = (const char *)d->str + d->sh[h] - 1;
            if (!strcmp(o, s)) {
                r = d->sh[h] - 1;
                goto done;
            }
            h = (h + 1) & (d->shcap - 1);
        }
        if (d->nstr + n + 1 > d->capstr) {
            d->capstr = (d->nstr + n + 1) * 2;
            d->str = realloc(d->str, d->capstr);
        }
        r = d->nstr;
        memcpy(d->str + d->nstr, s, n + 1);
        d->nstr += n + 1;
        d->sh[h] = r + 1;
        d->shn++;
    }
done:
    if (s != tmp)
        free(s);
    return r;
}

static uint32_t node_string(Doc *d, const RbNode *v)
{
    if (!v || v->type == RB_NIL)
        return 0;
    if (v->type == RB_STR || v->type == RB_SYM)
        return dstring(d, v->s, v->len);
    char b[32];
    if (v->type == RB_INT)
        snprintf(b, sizeof b, "%lld", (long long)v->i);
    else if (v->type == RB_TRUE)
        strcpy(b, "True");
    else if (v->type == RB_FALSE)
        strcpy(b, "False");
    else {
        doc_fail(d, "string expected", 0);
        return 0;
    }
    return dstring(d, b, (uint32_t)strlen(b));
}

static uint32_t reserve(Doc *d, int t, uint32_t n)
{
    Table *tb = &d->t[t];
    uint32_t slots = rpg_schema[t].slots;
    if (tb->rows + n > tb->cap) {
        tb->cap = (tb->rows + n) * 2 + 16;
        tb->v = realloc(tb->v, (size_t)tb->cap * slots * 4);
    }
    memset(tb->v + (size_t)tb->rows * slots, 0, (size_t)n * slots * 4);
    uint32_t first = tb->rows;
    tb->rows += n;
    return first;
}

static void set_row(Doc *d, int t, uint32_t row, const uint32_t *v)
{
    memcpy(d->t[t].v + (size_t)row * rpg_schema[t].slots, v, rpg_schema[t].slots * 4);
}

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint32_t as_int(const RbNode *v) { return (uint32_t)(int32_t)rb_int(v); }

static double rd_double(const uint8_t *p)
{
    double x;
    memcpy(&x, p, 8);
    return x;
}

/* ------------------------------------------------------------------ records */
static void encode(Doc *d, int t, const RbNode *obj, uint32_t *out);
static uint32_t route(Doc *d, const RbNode *r);

static void params(Doc *d, const RbNode *list, uint32_t *out2);

static void flatten(Doc *d, const RbNode *v, uint32_t **flat, uint32_t *n, uint32_t *cap)
{
#define PUSH(x) do { if (*n == *cap) { *cap = *cap * 2 + 16; *flat = realloc(*flat, *cap * 4); } (*flat)[(*n)++] = (x); } while (0)
    if (!v || v->type == RB_NIL || v->type == RB_FALSE) {
        PUSH(0);
    } else if (v->type == RB_TRUE) {
        PUSH(1);
    } else if (v->type == RB_INT || v->type == RB_FLOAT) {
        PUSH(as_int(v));
    } else if (v->type == RB_STR || v->type == RB_SYM) {
        PUSH(dstring(d, v->s, v->len));
    } else if (v->type == RB_ARRAY) {
        PUSH(v->len);
        for (uint32_t i = 0; i < v->len; i++)
            flatten(d, v->items[i], flat, n, cap);
    } else if (v->type == RB_OBJECT || v->type == RB_USER) {
        const char *c = v->s;
        if (!strcmp(c, "RPG::AudioFile") || !strcmp(c, "RPG::BGM") || !strcmp(c, "RPG::BGS") ||
            !strcmp(c, "RPG::ME") || !strcmp(c, "RPG::SE")) {
            PUSH(node_string(d, rb_get(v, "name")));
            PUSH(as_int(rb_get(v, "volume")));
            PUSH(as_int(rb_get(v, "pitch")));
        } else if ((!strcmp(c, "Tone") || !strcmp(c, "Color")) && v->type == RB_USER && v->len >= 32) {
            for (int i = 0; i < 4; i++)
                PUSH((uint32_t)(int32_t)rd_double((const uint8_t *)v->items + 8 * i));
        } else if (!strcmp(c, "RPG::MoveRoute")) {
            uint32_t r = route(d, v);
            PUSH(r);
        } else if (!strcmp(c, "RPG::MoveCommand")) {
            PUSH(as_int(rb_get(v, "code")));
        } else {
            doc_fail(d, "unknown parameter object", c);
        }
    } else {
        doc_fail(d, "unknown parameter", 0);
    }
#undef PUSH
}

static void params(Doc *d, const RbNode *list, uint32_t *out2)
{
    uint32_t *flat = 0, n = 0, cap = 0;
    if (list && list->type == RB_ARRAY)
        for (uint32_t i = 0; i < list->len; i++)
            flatten(d, list->items[i], &flat, &n, &cap);
    int tp = table_index("Param");
    uint32_t first = reserve(d, tp, n);
    for (uint32_t i = 0; i < n; i++)
        d->t[tp].v[first + i] = flat[i];
    free(flat);
    out2[0] = first;
    out2[1] = n;
}

static uint32_t list_len(const RbNode *l) { return (l && l->type == RB_ARRAY) ? l->len : 0; }

static void cmd_list(Doc *d, const RbNode *lst, uint32_t *out2)
{
    int tc = table_index("Cmd");
    uint32_t n = list_len(lst);
    uint32_t first = reserve(d, tc, n);
    for (uint32_t i = 0; i < n; i++) {
        const RbNode *c = lst->items[i];
        uint32_t row[4];
        params(d, rb_get(c, "parameters"), row + 2);
        row[0] = as_int(rb_get(c, "code"));
        row[1] = as_int(rb_get(c, "indent"));
        set_row(d, tc, first + i, row);
    }
    out2[0] = first;
    out2[1] = n;
}

static uint32_t route(Doc *d, const RbNode *r)
{
    if (!r || r->type == RB_NIL)
        return (uint32_t)-1;
    const RbNode *cmds = rb_get(r, "list");
    uint32_t n = list_len(cmds);
    int tr = table_index("Route"), tm = table_index("MoveCmd");
    uint32_t idx = reserve(d, tr, 1);
    uint32_t first = reserve(d, tm, n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t row[3];
        row[0] = as_int(rb_get(cmds->items[i], "code"));
        params(d, rb_get(cmds->items[i], "parameters"), row + 1);
        set_row(d, tm, first + i, row);
    }
    uint32_t row[5] = {as_int(rb_get(r, "repeat")), as_int(rb_get(r, "skippable")), as_int(rb_get(r, "wait")), first, n};
    set_row(d, tr, idx, row);
    return idx;
}

static void table16(Doc *d, const RbNode *t, uint32_t *out5)
{
    memset(out5, 0, 20);
    if (!t || t->type != RB_USER || t->len < 20)
        return;
    const uint8_t *b = (const uint8_t *)t->items;
    int32_t hd[5];
    memcpy(hd, b, 20);
    uint32_t n = (uint32_t)hd[4];
    if (20 + (uint64_t)n * 2 > t->len) {
        doc_fail(d, "bad Table", 0);
        return;
    }
    if (d->ni16 + n > d->capi16) {
        d->capi16 = (d->ni16 + n) * 2;
        d->i16 = realloc(d->i16, d->capi16 * 2);
    }
    uint32_t first = d->ni16;
    memcpy(d->i16 + first, b + 20, n * 2);
    d->ni16 += n;
    out5[0] = first;
    out5[1] = n;
    out5[2] = hd[1];
    out5[3] = hd[2];
    out5[4] = hd[3];
}

/* one field of kind f from value v into out; returns slots written */
static int value(Doc *d, const RpgSchemaField *f, const RbNode *v, uint32_t *out)
{
    int nil = !v || v->type == RB_NIL;
    switch (f->kind) {
    case RS_I: out[0] = as_int(v); return 1;
    case RS_F: out[0] = f2u((float)rb_num(v)); return 1;
    case RS_S: out[0] = node_string(d, v); return 1;
    case RS_A:
        if (nil) {
            out[0] = 0, out[1] = 100, out[2] = 100;
        } else {
            out[0] = node_string(d, rb_get(v, "name"));
            out[1] = as_int(rb_get(v, "volume"));
            out[2] = as_int(rb_get(v, "pitch"));
        }
        return 3;
    case RS_TONE:
        for (int i = 0; i < 4; i++)
            out[i] = (!nil && v->type == RB_USER && v->len >= 32) ? f2u((float)rd_double((const uint8_t *)v->items + 8 * i)) : 0;
        return 4;
    case RS_T16: table16(d, nil ? 0 : v, out); return 5;
    case RS_ROUTE: out[0] = route(d, v); return 1;
    case RS_LIST: cmd_list(d, v, out); return 2;
    case RS_SUB: encode(d, f->arg, v, out); return rpg_schema[f->arg].slots;
    case RS_RANGE: {
        int t = f->arg;
        uint32_t n = list_len(v);
        uint32_t first = reserve(d, t, n);
        uint32_t *row = malloc(rpg_schema[t].slots * 4 + 4);
        for (uint32_t i = 0; i < n; i++) {
            const RbNode *it = v->items[i];
            if (!strcmp(rpg_schema[t].name, "Int"))
                row[0] = as_int(it);
            else if (!strcmp(rpg_schema[t].name, "Str"))
                row[0] = node_string(d, it);
            else
                encode(d, t, it, row);
            set_row(d, t, first + i, row);
        }
        free(row);
        out[0] = first;
        out[1] = n;
        return 2;
    }
    case RS_IARR:
    case RS_SARR: {
        uint32_t n = list_len(v);
        for (int i = 0; i < f->arg; i++) {
            const RbNode *it = (uint32_t)i < n ? v->items[i] : 0;
            out[i] = !it ? 0 : f->kind == RS_SARR ? node_string(d, it) : as_int(it);
        }
        return f->arg;
    }
    }
    doc_fail(d, "bad schema kind", 0);
    return 1;
}

/* the object's value of field f; Map documents pass a stand-in for @events */
static const RbNode *events_override;

static void encode(Doc *d, int t, const RbNode *obj, uint32_t *out)
{
    const RpgSchemaTable *s = &rpg_schema[t];
    int k = 0;
    for (int i = 0; i < s->nfields; i++) {
        const RbNode *v = 0;
        if (events_override && !strcmp(s->name, "Map") && !strcmp(s->fields[i].name, "@events"))
            v = events_override;
        else if (obj && obj->type == RB_OBJECT)
            v = rb_attr(obj, s->fields[i].name);
        k += value(d, &s->fields[i], v, out + k);
    }
}

static uint32_t add(Doc *d, int t, const RbNode *obj)
{
    uint32_t *row = malloc(rpg_schema[t].slots * 4 + 4);
    encode(d, t, obj, row);
    uint32_t r = reserve(d, t, 1);
    set_row(d, t, r, row);
    free(row);
    return r;
}

/* ------------------------------------------------------------------ output */
static void put32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

static int doc_bytes(Doc *d, CookBlob *out)
{
    uint32_t head = 32 + 12 * NT, body = 0;
    for (int t = 0; t < NT; t++)
        body += d->t[t].rows * rpg_schema[t].slots * 4;
    uint32_t so = head + ((body + 3) & ~3u);
    uint32_t io = so + ((d->nstr + 3) & ~3u);
    uint32_t total = io + d->ni16 * 2;
    uint8_t *b = calloc(1, total + 4);
    if (!b)
        return -1;
    memcpy(b, "VRPG", 4);
    put32(b + 4, VRPG_VERSION);
    put32(b + 8, NT);
    put32(b + 12, so);
    put32(b + 16, d->nstr);
    put32(b + 20, io);
    put32(b + 24, d->ni16);
    uint32_t off = head;
    for (int t = 0; t < NT; t++) {
        put32(b + 32 + 12 * t, off);
        put32(b + 36 + 12 * t, d->t[t].rows);
        put32(b + 40 + 12 * t, rpg_schema[t].slots);
        uint32_t n = d->t[t].rows * rpg_schema[t].slots;
        for (uint32_t i = 0; i < n; i++)
            put32(b + off + 4 * i, d->t[t].v[i]);
        off += n * 4;
    }
    memcpy(b + so, d->str, d->nstr);
    for (uint32_t i = 0; i < d->ni16; i++) {
        uint16_t v = (uint16_t)d->i16[i];
        b[io + 2 * i] = v, b[io + 2 * i + 1] = v >> 8;
    }
    out->data = b;
    out->size = total;
    return 0;
}

static void doc_init(Doc *d, char *err, int errcap)
{
    memset(d, 0, sizeof *d);
    d->str = malloc(1024);
    d->capstr = 1024;
    d->str[0] = 0;
    d->nstr = 1;
    d->err = err;
    d->errcap = errcap;
}

static void doc_free(Doc *d)
{
    for (int t = 0; t < NT; t++)
        free(d->t[t].v);
    free(d->str);
    free(d->sh);
    free(d->i16);
}

static RbDoc *load(CookFs *fs, const char *key, uint8_t **buf, char *err, int errcap)
{
    CookFile *f = cook_fs_find(fs, key);
    uint32_t n;
    *buf = f ? cook_fs_read(fs, f, &n) : 0;
    COOK_FEED("convert", key);
    if (!*buf) {
        snprintf(err, errcap, "missing %s", key);
        return 0;
    }
    RbDoc *r = rb_load(*buf, n, err, errcap);
    if (!r) {
        free(*buf);
        *buf = 0;
    }
    return r;
}

int cook_vrpg_db(CookFs *fs, CookBlob *out, char *err, int errcap)
{
    static const char *files[][2] = {{"Actor", "actors"}, {"Class", "classes"}, {"Skill", "skills"}, {"Item", "items"},
                                     {"Weapon", "weapons"}, {"Armor", "armors"}, {"Enemy", "enemies"}, {"State", "states"},
                                     {"Troop", "troops"}, {"Animation", "animations"}, {"Tileset", "tilesets"},
                                     {"CommonEvent", "commonevents"}};
    Doc d;
    doc_init(&d, err, errcap);
    events_override = 0;
    int nroots = (int)(sizeof rpg_schema_db_roots / sizeof rpg_schema_db_roots[0]);
    for (int r = 0; r < nroots && !d.failed; r++) {
        const char *tn = rpg_schema_db_roots[r];
        int t = table_index(tn);
        char key[64];
        const char *file = 0;
        for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
            if (!strcmp(files[i][0], tn))
                file = files[i][1];
        if (!file)
            file = !strcmp(tn, "System") ? "system" : "mapinfos";
        snprintf(key, sizeof key, "data/%s.rvdata2", file);
        uint8_t *buf;
        RbDoc *rb = load(fs, key, &buf, err, errcap);
        if (!rb) {
            doc_free(&d);
            return -1;
        }
        RbNode *root = rb_root(rb);
        if (!strcmp(tn, "System")) {
            add(&d, t, root);
        } else if (!strcmp(tn, "MapInfo")) {
            int64_t top = 0;
            for (uint32_t i = 0; root->type == RB_HASH && i < root->len; i++)
                if (root->items[i]->i > top)
                    top = root->items[i]->i;
            uint32_t first = reserve(&d, t, root->len ? (uint32_t)top + 1 : 1);
            uint32_t *row = malloc(rpg_schema[t].slots * 4);
            for (uint32_t i = 0; root->type == RB_HASH && i < root->len; i++) {
                encode(&d, t, root->items[root->len + i], row);
                row[0] = (uint32_t)root->items[i]->i;
                set_row(&d, t, first + (uint32_t)root->items[i]->i, row);
            }
            free(row);
        } else {
            uint32_t n = list_len(root);
            uint32_t first = reserve(&d, t, n);
            uint32_t *row = malloc(rpg_schema[t].slots * 4);
            for (uint32_t i = 0; i < n; i++) {
                encode(&d, t, root->items[i], row);
                set_row(&d, t, first + i, row);
            }
            free(row);
        }
        rb_free(rb);
        free(buf);
        if (cook_yield && cook_yield()) {
            doc_fail(&d, "stopped", 0);
        }
    }
    int rc = d.failed ? -1 : doc_bytes(&d, out);
    doc_free(&d);
    return rc;
}

static int evcmp(const void *a, const void *b)
{
    int64_t x = (*(RbNode *const *)a)->i, y = (*(RbNode *const *)b)->i;
    return x < y ? -1 : x > y;
}

int cook_vrpg_maps(CookFs *fs, CookBlob **maps, int *max_map, char *err, int errcap)
{
    CookFile **files;
    int n = cook_fs_list(fs, "data/map", &files);
    int top = 0;
    for (int i = 0; i < n; i++) {
        const char *k = files[i]->key + 8;   /* "data/map" */
        if (strlen(k) == 3 + 8 && k[0] >= '0' && k[0] <= '9' && !strcmp(k + 3, ".rvdata2")) {
            int id = atoi(k);
            if (id > top)
                top = id;
        }
    }
    *max_map = top;
    *maps = calloc(top + 1, sizeof(CookBlob));
    for (int i = 0; i < n; i++) {
        const char *k = files[i]->key + 8;
        if (!(strlen(k) == 11 && k[0] >= '0' && k[0] <= '9' && !strcmp(k + 3, ".rvdata2")))
            continue;
        int id = atoi(k);
        uint8_t *buf;
        RbDoc *rb = load(fs, files[i]->key, &buf, err, errcap);
        if (!rb) {
            free(files);
            return -1;
        }
        RbNode *m = rb_root(rb);
        RbNode *evs = rb_get(m, "events");
        /* the events sorted by id, as an array */
        RbNode arr = {RB_ARRAY};
        RbNode **keys = 0;
        if (evs && evs->type == RB_HASH && evs->len) {
            keys = malloc(evs->len * sizeof *keys);
            RbNode *pairs = malloc(evs->len * sizeof(RbNode));
            for (uint32_t j = 0; j < evs->len; j++) {
                pairs[j] = *evs->items[j];
                pairs[j].items = (RbNode **)(void *)evs->items[evs->len + j];   /* carry the value */
                keys[j] = &pairs[j];
            }
            qsort(keys, evs->len, sizeof *keys, evcmp);
            arr.len = evs->len;
            arr.items = malloc(evs->len * sizeof(RbNode *));
            for (uint32_t j = 0; j < evs->len; j++)
                arr.items[j] = (RbNode *)(void *)keys[j]->items;
            free(pairs);
        }
        Doc d;
        doc_init(&d, err, errcap);
        events_override = &arr;
        add(&d, table_index("Map"), m);
        events_override = 0;
        int rc = d.failed ? -1 : doc_bytes(&d, &(*maps)[id]);
        doc_free(&d);
        free(arr.items);
        free(keys);
        rb_free(rb);
        free(buf);
        if (rc) {
            free(files);
            return -1;
        }
        if (cook_yield && cook_yield())
            break;
    }
    free(files);
    return 0;
}
