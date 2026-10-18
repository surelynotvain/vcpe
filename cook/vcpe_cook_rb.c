/* VCPE cook: Ruby Marshal 4.8 reader (tools/rpgmaker/marshal.py in C). Every node lives in one arena freed with the
 * document; object links ('@') point at the shared node like Ruby's. */
#include "vcpe_cook.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct Block { struct Block *next; size_t used, cap; } Block;

struct RbDoc {
    const uint8_t *p, *end;
    Block *blocks;
    const char **syms;
    int nsyms, capsyms;
    RbNode **objs;
    int nobjs, capobjs;
    RbNode *root;
    char *err;
    int errcap, failed;
};

static void *arena(RbDoc *d, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    Block *b = d->blocks;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 256 * 1024 ? n : 256 * 1024;
        b = malloc(sizeof(Block) + cap);
        if (!b)
            return 0;
        b->cap = cap;
        b->used = 0;
        b->next = d->blocks;
        d->blocks = b;
    }
    void *r = (char *)(b + 1) + b->used;
    b->used += n;
    return r;
}

static void fail(RbDoc *d, const char *m)
{
    if (!d->failed && d->err)
        snprintf(d->err, d->errcap, "marshal: %s at %d", m, (int)(d->p - (d->end - 0)));
    d->failed = 1;
}

static int byte(RbDoc *d)
{
    if (d->p >= d->end) {
        fail(d, "truncated");
        return 0;
    }
    return *d->p++;
}

static int64_t integer(RbDoc *d)
{
    int c = (int8_t)byte(d);
    if (c == 0)
        return 0;
    if (c > 4)
        return c - 5;
    if (c < -4)
        return c + 5;
    int n = c < 0 ? -c : c;
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint64_t)byte(d) << (8 * i);
    if (c < 0)
        return (int64_t)v - (int64_t)(1ULL << (8 * n));
    return (int64_t)v;
}

static const char *bytes(RbDoc *d, uint32_t *len)
{
    long n = integer(d);
    if (n < 0 || d->p + n > d->end) {
        fail(d, "bad length");
        *len = 0;
        return "";
    }
    char *s = arena(d, n + 1);
    memcpy(s, d->p, n);
    s[n] = 0;
    d->p += n;
    *len = (uint32_t)n;
    return s;
}

static RbNode *node(RbDoc *d, int type)
{
    RbNode *n = arena(d, sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type;
    return n;
}

static RbNode *track(RbDoc *d, RbNode *n)
{
    if (d->nobjs == d->capobjs) {
        d->capobjs = d->capobjs ? d->capobjs * 2 : 4096;
        d->objs = realloc(d->objs, d->capobjs * sizeof *d->objs);
    }
    d->objs[d->nobjs++] = n;
    return n;
}

static const char *symbol(RbDoc *d, const char *s)
{
    if (d->nsyms == d->capsyms) {
        d->capsyms = d->capsyms ? d->capsyms * 2 : 256;
        d->syms = realloc(d->syms, d->capsyms * sizeof *d->syms);
    }
    d->syms[d->nsyms++] = s;
    return s;
}

static RbNode *rd(RbDoc *d, int depth);

/* a symbol in a position that must be one (class names, attribute names) */
static const char *rd_sym(RbDoc *d)
{
    int t = byte(d);
    uint32_t len;
    if (t == ':')
        return symbol(d, bytes(d, &len));
    if (t == ';') {
        long i = integer(d);
        if (i >= 0 && i < d->nsyms)
            return d->syms[i];
    }
    fail(d, "symbol expected");
    return "";
}

static RbNode *rd(RbDoc *d, int depth)
{
    static RbNode nil = {RB_NIL};
    if (d->failed || depth > 200) {
        fail(d, "too deep");
        return &nil;
    }
    int t = byte(d);
    RbNode *n;
    uint32_t len;
    switch (t) {
    case '0': return &nil;
    case 'T': return node(d, RB_TRUE);
    case 'F': return node(d, RB_FALSE);
    case 'i':
        n = node(d, RB_INT);
        n->i = integer(d);
        return n;
    case ':':
        n = node(d, RB_SYM);
        n->s = symbol(d, bytes(d, &n->len));
        return n;
    case ';': {
        long i = integer(d);
        n = node(d, RB_SYM);
        n->s = (i >= 0 && i < d->nsyms) ? d->syms[i] : "";
        n->len = (uint32_t)strlen(n->s);
        return n;
    }
    case '@': {
        long i = integer(d);
        if (i < 0 || i >= d->nobjs) {
            fail(d, "bad link");
            return &nil;
        }
        return d->objs[i];
    }
    case '"':
        n = track(d, node(d, RB_STR));
        n->s = bytes(d, &n->len);
        return n;
    case '[': {
        n = track(d, node(d, RB_ARRAY));
        long c = integer(d);
        if (c < 0 || c > d->end - d->p) {
            fail(d, "bad array");
            return n;
        }
        n->len = (uint32_t)c;
        n->items = arena(d, (c ? c : 1) * sizeof(RbNode *));
        for (long i = 0; i < c; i++)
            n->items[i] = rd(d, depth + 1);
        return n;
    }
    case '{':
    case '}': {
        n = track(d, node(d, RB_HASH));
        long c = integer(d);
        if (c < 0 || c > d->end - d->p) {
            fail(d, "bad hash");
            return n;
        }
        n->len = (uint32_t)c;
        n->items = arena(d, (c ? 2 * c : 1) * sizeof(RbNode *));
        for (long i = 0; i < c; i++) {
            n->items[i] = rd(d, depth + 1);
            n->items[c + i] = rd(d, depth + 1);
        }
        if (t == '}')
            rd(d, depth + 1);
        return n;
    }
    case 'o': {
        n = track(d, node(d, RB_OBJECT));
        n->s = rd_sym(d);
        long c = integer(d);
        if (c < 0 || c > d->end - d->p) {
            fail(d, "bad object");
            return n;
        }
        n->len = (uint32_t)c;
        n->items = arena(d, (c ? c : 1) * sizeof(RbNode *));
        n->names = arena(d, (c ? c : 1) * sizeof(char *));
        for (long i = 0; i < c; i++) {
            n->names[i] = rd_sym(d);
            n->items[i] = rd(d, depth + 1);
        }
        return n;
    }
    case 'u': {
        const char *cls = rd_sym(d);
        n = track(d, node(d, RB_USER));
        n->s = cls;
        const char *b = bytes(d, &n->len);
        n->items = (RbNode **)(void *)b;   /* raw bytes */
        return n;
    }
    case 'U': {
        n = track(d, node(d, RB_OBJECT));
        n->s = rd_sym(d);
        rd(d, depth + 1);
        return n;
    }
    case 'I': {
        RbNode *v = rd(d, depth + 1);
        long c = integer(d);
        for (long i = 0; i < c && !d->failed; i++) {
            rd(d, depth + 1);
            rd(d, depth + 1);
        }
        return v;
    }
    case 'f': {
        n = track(d, node(d, RB_FLOAT));
        const char *s = bytes(d, &len);
        if (!strcmp(s, "inf"))
            n->f = INFINITY;
        else if (!strcmp(s, "-inf"))
            n->f = -INFINITY;
        else if (!strcmp(s, "nan"))
            n->f = NAN;
        else
            n->f = strtod(s, 0);
        return n;
    }
    case 'l': {
        int sign = byte(d);
        long words = integer(d);
        n = track(d, node(d, RB_INT));
        unsigned long long v = 0;
        for (long i = 0; i < words * 2; i++) {
            int b = byte(d);
            if (i < 8)
                v |= (unsigned long long)b << (8 * i);
        }
        n->i = (v > (unsigned long long)INT64_MAX) ? INT64_MAX : (int64_t)v;
        if (sign == '-')
            n->i = -n->i;
        return n;
    }
    case 'C':
        rd_sym(d);
        return rd(d, depth + 1);
    case 'e':
        rd_sym(d);
        return rd(d, depth + 1);
    }
    fail(d, "unsupported type");
    return &nil;
}

RbDoc *rb_load(const uint8_t *data, uint32_t size, char *err, int errcap)
{
    if (size < 2 || data[0] != 4 || data[1] != 8) {
        if (err)
            snprintf(err, errcap, "not a Ruby Marshal 4.8 file");
        return 0;
    }
    RbDoc *d = calloc(1, sizeof *d);
    d->p = data + 2;
    d->end = data + size;
    d->err = err;
    d->errcap = errcap;
    d->root = rd(d, 0);
    if (d->failed) {
        rb_free(d);
        return 0;
    }
    free(d->objs);
    d->objs = 0;
    free(d->syms);
    d->syms = 0;
    return d;
}

RbNode *rb_root(RbDoc *d) { return d->root; }

void rb_free(RbDoc *d)
{
    if (!d)
        return;
    while (d->blocks) {
        Block *n = d->blocks->next;
        free(d->blocks);
        d->blocks = n;
    }
    free(d->objs);
    free(d->syms);
    free(d);
}

RbNode *rb_attr(const RbNode *o, const char *name)
{
    if (!o || o->type != RB_OBJECT)
        return 0;
    for (uint32_t i = 0; i < o->len; i++)
        if (!strcmp(o->names[i], name))
            return o->items[i];
    return 0;
}

RbNode *rb_get(const RbNode *o, const char *field)
{
    char k[64];
    snprintf(k, sizeof k, "@%s", field);
    return rb_attr(o, k);
}

int64_t rb_int(const RbNode *n)
{
    if (!n)
        return 0;
    switch (n->type) {
    case RB_TRUE: return 1;
    case RB_INT: return n->i > 0x7fffffff ? 0x7fffffff : n->i < -0x7fffffffLL - 1 ? -0x7fffffffLL - 1 : n->i;
    case RB_FLOAT: return (int64_t)n->f;
    }
    return 0;
}

double rb_num(const RbNode *n)
{
    if (!n)
        return 0;
    if (n->type == RB_FLOAT)
        return n->f;
    if (n->type == RB_INT)
        return (double)n->i;
    if (n->type == RB_TRUE)
        return 1;
    return 0;
}
