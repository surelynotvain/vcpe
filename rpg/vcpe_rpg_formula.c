/* VCPE rpg: damage formulas at run time. The VX Ace Damage#formula strings (Ruby expressions) are read from the
 * database and evaluated by a small interpreter, so a game built on the player's own data on the device carries no
 * generated formula code. Same language and rules as tools/rpgmaker/vrpg_formula.py: integer and float literals,
 * a.STAT / b.STAT, v[n], + - * / %, parentheses, unary minus, comparisons, && ||, ternary, b.state?(n),
 * [x, y].min / .max, .abs .to_i .to_f. Ruby's numeric rules are kept (integer division floors, a float anywhere makes
 * the operation a float). A formula the interpreter cannot read evaluates to 0 and is logged once.
 *
 * Install with rpg_hooks.formula = rpg_formula_eval. */
#include "vcpe_rpg.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int isf; int64_t i; double f; } V;

enum { N_NUM, N_PARAM, N_HP, N_MP, N_TP, N_LEVEL, N_XPARAM, N_SPARAM, N_STATE, N_VAR, N_NEG, N_NOT, N_ADD, N_SUB, N_MUL,
       N_DIV, N_MOD, N_LT, N_GT, N_LE, N_GE, N_EQ, N_NE, N_AND, N_OR, N_COND, N_MIN, N_MAX, N_ABS, N_TOI, N_TOF };

typedef struct Node {
    uint8_t op, isf, who;   /* who: 0 a, 1 b */
    int8_t arg;             /* stat index */
    V val;
    int a, b, c;            /* children (indices), -1 none */
    int list, nlist;        /* min / max: first item index into ilist */
} Node;

typedef struct {
    Node *n;
    int nn, cap;
    int *list;
    int nl, capl;
    int root;
    int ok;
} Prog;

typedef struct { const char *p; Prog *g; int fail; } Lx;

static int mk(Prog *g, int op, int isf, int a, int b, int c)
{
    Node n;
    memset(&n, 0, sizeof n);
    n.op = (uint8_t)op;
    n.isf = (uint8_t)isf;
    n.a = a;
    n.b = b;
    n.c = c;
    if (g->nn == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 16;
        g->n = realloc(g->n, sizeof(Node) * g->cap);
    }
    g->n[g->nn] = n;
    return g->nn++;
}

static void skip(Lx *x) { while (*x->p == ' ' || *x->p == '\t' || *x->p == '\r' || *x->p == '\n') x->p++; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int op_is(Lx *x, const char *s)
{
    skip(x);
    size_t n = strlen(s);
    if (strncmp(x->p, s, n))
        return 0;
    /* a one-character operator must not be the start of a two-character one */
    if (n == 1) {
        char c = x->p[1];
        if ((s[0] == '<' || s[0] == '>' ) && c == '=') return 0;
        if ((s[0] == '=' || s[0] == '!') && c == '=') return 0;
        if (s[0] == '&' || s[0] == '|') return 0;
    }
    return 1;
}

static int take(Lx *x, const char *s)
{
    if (!op_is(x, s)) {
        x->fail = 1;
        return 0;
    }
    x->p += strlen(s);
    return 1;
}

static int ternary(Lx *x);

static int name_tok(Lx *x, char *buf, int cap)
{
    skip(x);
    if (!is_alpha(*x->p)) {
        x->fail = 1;
        return 0;
    }
    int n = 0;
    while (is_alpha(*x->p) || is_digit(*x->p)) {
        if (n < cap - 1) buf[n++] = *x->p;
        x->p++;
    }
    if (*x->p == '?' ) {   /* state? */
        if (n < cap - 1) buf[n++] = '?';
        x->p++;
    }
    buf[n] = 0;
    return 1;
}

static int stat_index(const char *s, int *kind)
{
    static const char *P[] = {"mhp", "mmp", "atk", "def", "mat", "mdf", "agi", "luk"};
    static const char *X[] = {"hit", "eva", "cri", "cev", "mev", "mrf", "cnt", "hrg", "mrg", "trg"};
    static const char *S[] = {"tgr", "grd", "rec", "pha", "mcr", "tcr", "pdr", "mdr", "fdr", "exr"};
    for (int i = 0; i < 8; i++) if (!strcmp(s, P[i])) { *kind = N_PARAM; return i; }
    for (int i = 0; i < 10; i++) if (!strcmp(s, X[i])) { *kind = N_XPARAM; return i; }
    for (int i = 0; i < 10; i++) if (!strcmp(s, S[i])) { *kind = N_SPARAM; return i; }
    if (!strcmp(s, "hp")) { *kind = N_HP; return 0; }
    if (!strcmp(s, "mp")) { *kind = N_MP; return 0; }
    if (!strcmp(s, "tp")) { *kind = N_TP; return 0; }
    if (!strcmp(s, "level")) { *kind = N_LEVEL; return 0; }
    *kind = -1;
    return 0;
}

static int primary(Lx *x)
{
    Prog *g = x->g;
    skip(x);
    if (is_digit(*x->p)) {
        const char *s = x->p;
        while (is_digit(*x->p)) x->p++;
        int isf = 0;
        if (*x->p == '.' && is_digit(x->p[1])) {
            isf = 1;
            x->p++;
            while (is_digit(*x->p)) x->p++;
        }
        int n = mk(g, N_NUM, isf, -1, -1, -1);
        if (isf) { g->n[n].val.isf = 1; g->n[n].val.f = strtod(s, 0); }
        else g->n[n].val.i = strtoll(s, 0, 10);
        return n;
    }
    if (*x->p == '(') {
        x->p++;
        int e = ternary(x);
        take(x, ")");
        return e;
    }
    if (*x->p == '[') {
        x->p++;
        int items[32], ni = 0;
        items[ni++] = ternary(x);
        while (op_is(x, ",") && ni < 32) {
            x->p++;
            items[ni++] = ternary(x);
        }
        take(x, "]");
        /* the method is applied in postfix(); keep the list as a pending node */
        int n = mk(g, N_MIN, 0, -1, -1, -1);
        g->n[n].op = 255;   /* LIST marker */
        g->n[n].list = g->nl;
        g->n[n].nlist = ni;
        if (g->nl + ni > g->capl) {
            g->capl = (g->nl + ni) * 2;
            g->list = realloc(g->list, sizeof(int) * g->capl);
        }
        for (int i = 0; i < ni; i++)
            g->list[g->nl++] = items[i];
        return n;
    }
    char nm[24];
    if (!name_tok(x, nm, sizeof nm))
        return mk(g, N_NUM, 0, -1, -1, -1);
    if (!strcmp(nm, "a") || !strcmp(nm, "b")) {
        int who = nm[0] == 'b';
        take(x, ".");
        char st[24];
        if (!name_tok(x, st, sizeof st))
            return mk(g, N_NUM, 0, -1, -1, -1);
        if (!strcmp(st, "state?")) {
            take(x, "(");
            int e = ternary(x);
            take(x, ")");
            int n = mk(g, N_STATE, 0, e, -1, -1);
            g->n[n].who = (uint8_t)who;
            return n;
        }
        int kind;
        int idx = stat_index(st, &kind);
        if (kind < 0) {
            x->fail = 1;
            return mk(g, N_NUM, 0, -1, -1, -1);
        }
        int n = mk(g, kind, kind == N_XPARAM || kind == N_SPARAM || kind == N_TP, -1, -1, -1);
        g->n[n].who = (uint8_t)who;
        g->n[n].arg = (int8_t)idx;
        return n;
    }
    if (!strcmp(nm, "v")) {
        take(x, "[");
        int e = ternary(x);
        take(x, "]");
        return mk(g, N_VAR, 0, e, -1, -1);
    }
    if (!strcmp(nm, "true") || !strcmp(nm, "false")) {
        int n = mk(g, N_NUM, 0, -1, -1, -1);
        g->n[n].val.i = nm[0] == 't';
        return n;
    }
    x->fail = 1;
    return mk(g, N_NUM, 0, -1, -1, -1);
}

static int postfix(Lx *x)
{
    Prog *g = x->g;
    int a = primary(x);
    while (!x->fail && op_is(x, ".")) {
        x->p++;
        char m[24];
        if (!name_tok(x, m, sizeof m))
            break;
        if ((!strcmp(m, "min") || !strcmp(m, "max")) && g->n[a].op == 255) {
            int fl = 0;
            for (int i = 0; i < g->n[a].nlist; i++)
                fl |= g->n[g->list[g->n[a].list + i]].isf;
            g->n[a].op = !strcmp(m, "min") ? N_MIN : N_MAX;
            g->n[a].isf = (uint8_t)fl;
        } else if (!strcmp(m, "to_i"))
            a = mk(g, N_TOI, 0, a, -1, -1);
        else if (!strcmp(m, "to_f"))
            a = mk(g, N_TOF, 1, a, -1, -1);
        else if (!strcmp(m, "abs"))
            a = mk(g, N_ABS, g->n[a].isf, a, -1, -1);
        else
            x->fail = 1;
    }
    if (g->n[a].op == 255)
        x->fail = 1;
    return a;
}

static int unary(Lx *x)
{
    Prog *g = x->g;
    if (op_is(x, "-")) {
        x->p++;
        int a = unary(x);
        return mk(g, N_NEG, g->n[a].isf, a, -1, -1);
    }
    if (op_is(x, "+")) {
        x->p++;
        return unary(x);
    }
    if (op_is(x, "!")) {
        x->p++;
        int a = unary(x);
        return mk(g, N_NOT, 0, a, -1, -1);
    }
    return postfix(x);
}

static int mul(Lx *x)
{
    Prog *g = x->g;
    int a = unary(x);
    for (;;) {
        int op = op_is(x, "*") ? N_MUL : op_is(x, "/") ? N_DIV : op_is(x, "%") ? N_MOD : 0;
        if (!op)
            return a;
        x->p++;
        int b = unary(x);
        a = mk(g, op, g->n[a].isf | g->n[b].isf, a, b, -1);
    }
}

static int add(Lx *x)
{
    Prog *g = x->g;
    int a = mul(x);
    for (;;) {
        int op = op_is(x, "+") ? N_ADD : op_is(x, "-") ? N_SUB : 0;
        if (!op)
            return a;
        x->p++;
        int b = mul(x);
        a = mk(g, op, g->n[a].isf | g->n[b].isf, a, b, -1);
    }
}

static int compare(Lx *x)
{
    Prog *g = x->g;
    int a = add(x);
    for (;;) {
        int op = 0, len = 1;
        if (op_is(x, "==")) { op = N_EQ; len = 2; }
        else if (op_is(x, "!=")) { op = N_NE; len = 2; }
        else if (op_is(x, "<=")) { op = N_LE; len = 2; }
        else if (op_is(x, ">=")) { op = N_GE; len = 2; }
        else if (op_is(x, "<")) op = N_LT;
        else if (op_is(x, ">")) op = N_GT;
        if (!op)
            return a;
        x->p += len;
        int b = add(x);
        a = mk(g, op, 0, a, b, -1);
    }
}

static int logic(Lx *x)
{
    Prog *g = x->g;
    int a = compare(x);
    for (;;) {
        int op = 0;
        skip(x);
        if (x->p[0] == '&' && x->p[1] == '&') op = N_AND;
        else if (x->p[0] == '|' && x->p[1] == '|') op = N_OR;
        if (!op)
            return a;
        x->p += 2;
        int b = compare(x);
        a = mk(g, op, 0, a, b, -1);
    }
}

static int ternary(Lx *x)
{
    Prog *g = x->g;
    int c = logic(x);
    if (op_is(x, "?")) {
        x->p++;
        int a = ternary(x);
        take(x, ":");
        int b = ternary(x);
        return mk(g, N_COND, g->n[a].isf | g->n[b].isf, c, a, b);
    }
    return c;
}

/* ------------------------------------------------------------------ evaluation */
static V vi(int64_t i) { V v = {0, i, 0}; return v; }
static V vf(double f) { V v = {1, 0, f}; return v; }
static double asf(V v) { return v.isf ? v.f : (double)v.i; }
static V cast(V v, int isf) { return isf && !v.isf ? vf((double)v.i) : v; }
static int truth(V v) { return v.isf ? v.f != 0 : v.i != 0; }

static int64_t i_div(int64_t a, int64_t b)
{
    if (!b) return 0;
    int64_t q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}
static int64_t i_mod(int64_t a, int64_t b)
{
    if (!b) return 0;
    int64_t r = a % b;
    if (r && ((r < 0) != (b < 0))) r += b;
    return r;
}
static double f_div(double a, double b) { return b != 0 ? a / b : (a == 0 ? NAN : (a > 0 ? INFINITY : -INFINITY)); }
static double f_mod(double a, double b)
{
    double r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

static V eval(const Prog *g, int ni, const RpgBattler *ab[2])
{
    const Node *n = &g->n[ni];
    switch (n->op) {
    case N_NUM: return n->val;
    case N_PARAM: return vi(rpg_battler_param(ab[n->who], n->arg));
    case N_HP: return vi(ab[n->who]->hp);
    case N_MP: return vi(ab[n->who]->mp);
    case N_TP: return vf((double)ab[n->who]->tp);
    case N_LEVEL: { const RpgActorState *s = rpg_battler_actor(ab[n->who]); return vi(s ? s->level : 0); }
    case N_XPARAM: return vf(rpg_battler_xparam(ab[n->who], n->arg));
    case N_SPARAM: return vf(rpg_battler_sparam(ab[n->who], n->arg));
    case N_STATE: { V s = eval(g, n->a, ab); return vi(rpg_battler_state(ab[n->who], (int)(s.isf ? (int64_t)s.f : s.i))); }
    case N_VAR: { V s = eval(g, n->a, ab); return vi(rpg_var((int)(s.isf ? (int64_t)s.f : s.i))); }
    case N_NEG: { V a = eval(g, n->a, ab); return a.isf ? vf(-a.f) : vi(-a.i); }
    case N_NOT: return vi(!truth(eval(g, n->a, ab)));
    case N_ADD: case N_SUB: case N_MUL: {
        V a = eval(g, n->a, ab), b = eval(g, n->b, ab);
        if (n->isf) {
            double x = asf(a), y = asf(b);
            return vf(n->op == N_ADD ? x + y : n->op == N_SUB ? x - y : x * y);
        }
        return vi(n->op == N_ADD ? a.i + b.i : n->op == N_SUB ? a.i - b.i : a.i * b.i);
    }
    case N_DIV: case N_MOD: {
        V a = eval(g, n->a, ab), b = eval(g, n->b, ab);
        if (n->isf)
            return vf(n->op == N_DIV ? f_div(asf(a), asf(b)) : f_mod(asf(a), asf(b)));
        return vi(n->op == N_DIV ? i_div(a.i, b.i) : i_mod(a.i, b.i));
    }
    case N_LT: case N_GT: case N_LE: case N_GE: case N_EQ: case N_NE: {
        double x = asf(eval(g, n->a, ab)), y = asf(eval(g, n->b, ab));
        int r = n->op == N_LT ? x < y : n->op == N_GT ? x > y : n->op == N_LE ? x <= y : n->op == N_GE ? x >= y :
                n->op == N_EQ ? x == y : x != y;
        return vi(r);
    }
    case N_AND: return vi(truth(eval(g, n->a, ab)) && truth(eval(g, n->b, ab)));
    case N_OR: return vi(truth(eval(g, n->a, ab)) || truth(eval(g, n->b, ab)));
    case N_COND: return cast(truth(eval(g, n->a, ab)) ? eval(g, n->b, ab) : eval(g, n->c, ab), n->isf);
    case N_MIN: case N_MAX: {
        V best = cast(eval(g, g->list[n->list], ab), n->isf);
        for (int i = 1; i < n->nlist; i++) {
            V v = cast(eval(g, g->list[n->list + i], ab), n->isf);
            int less = n->isf ? v.f < best.f : v.i < best.i;
            int greater = n->isf ? v.f > best.f : v.i > best.i;
            if ((n->op == N_MIN && less) || (n->op == N_MAX && greater))
                best = v;
        }
        return best;
    }
    case N_ABS: { V a = eval(g, n->a, ab); return a.isf ? vf(fabs(a.f)) : vi(a.i < 0 ? -a.i : a.i); }
    case N_TOI: { V a = eval(g, n->a, ab); return vi(a.isf ? (int64_t)a.f : a.i); }
    case N_TOF: return vf(asf(eval(g, n->a, ab)));
    }
    return vi(0);
}

/* ------------------------------------------------------------------ cache per skill / item */
static Prog *progs[2];     /* kind -> array by id */
static int nprogs[2];

static Prog *prog_for(int kind, int id)
{
    int k = kind == RPG_KIND_ITEM ? 1 : 0;
    if (id < 0 || id > 100000)
        return 0;
    if (id >= nprogs[k]) {
        int nn = id + 64;
        progs[k] = realloc(progs[k], sizeof(Prog) * nn);
        memset(progs[k] + nprogs[k], 0, sizeof(Prog) * (nn - nprogs[k]));
        nprogs[k] = nn;
    }
    Prog *g = &progs[k][id];
    if (g->nn || g->ok)
        return g;
    const RpgUsable *u = rpg_usable(kind, id);
    const char *src = u ? RPG_STR(u->damage.formula) : "";
    Lx x = {src && *src ? src : "0", g, 0};
    g->root = ternary(&x);
    skip(&x);
    g->ok = !x.fail && !*x.p;
    if (!g->ok) {
        /* leave a constant 0 */
        g->nn = 0;
        g->root = mk(g, N_NUM, 0, -1, -1, -1);
        g->ok = 1;
        rpg_log("formula: cannot read %s %d: %s", kind == RPG_KIND_ITEM ? "item" : "skill", id, src);
    }
    return g;
}

double rpg_formula_eval(int kind, int id, const RpgBattler *a, const RpgBattler *b)
{
    Prog *g = prog_for(kind, id);
    if (!g)
        return 0;
    const RpgBattler *ab[2] = {a, b};
    V v = eval(g, g->root, ab);
    return v.isf ? v.f : (double)v.i;
}
