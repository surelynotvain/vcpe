"""VX Ace damage formulas (Ruby expressions in RPG::UsableItem::Damage#formula) -> plain C, compiled into the game.

    from vrpg_formula import compile_formulas
    c_source = compile_formulas(skills, items)   # [(id, formula string)], [(id, formula string)]

The generated file defines  double rpg_formula_c(int kind, int id, const RpgBattler *a, const RpgBattler *b)
(kind RPG_KIND_SKILL / RPG_KIND_ITEM), to be installed as rpg_hooks.formula. It returns the formula's value; the
runtime then applies [value, 0].max and the recovery sign as Damage#eval does.

Supported Ruby: integer and float literals, a.STAT / b.STAT (atk def mat mdf agi luk mhp mmp hp mp tp level, and
the x/s-parameters hit eva cri ... exr), v[n], + - * / %, parentheses, unary minus, comparisons, && ||, ternary
c ? x : y, b.state?(n), [x, y].min / .max. Ruby's numeric rules are kept: integer division floors, a float anywhere
makes the operation a float. Anything else is rejected at build time with the formula's text, so nothing is
silently approximated."""
import re

PARAMS = {'mhp': 0, 'mmp': 1, 'atk': 2, 'def': 3, 'mat': 4, 'mdf': 5, 'agi': 6, 'luk': 7}
XPARAMS = {'hit': 0, 'eva': 1, 'cri': 2, 'cev': 3, 'mev': 4, 'mrf': 5, 'cnt': 6, 'hrg': 7, 'mrg': 8, 'trg': 9}
SPARAMS = {'tgr': 0, 'grd': 1, 'rec': 2, 'pha': 3, 'mcr': 4, 'tcr': 5, 'pdr': 6, 'mdr': 7, 'fdr': 8, 'exr': 9}

TOKEN = re.compile(r'\s*(?:(\d+\.\d+)|(\d+)|([A-Za-z_][A-Za-z_0-9]*\??)|(&&|\|\||==|!=|<=|>=|[-+*/%()?:<>\[\],.!]))')


class Fail(Exception):
    pass


def tokens(s):
    out, pos = [], 0
    s = s.strip()
    while pos < len(s):
        m = TOKEN.match(s, pos)
        if not m or m.end() == pos:
            raise Fail('cannot read %r' % s[pos:])
        pos = m.end()
        f, i, name, op = m.groups()
        if f:
            out.append(('num', f, True))
        elif i:
            out.append(('num', i, False))
        elif name:
            out.append(('name', name))
        else:
            out.append(('op', op))
    out.append(('end',))
    return out


class Parser:
    """recursive descent; each node -> (C expression, is_float)"""

    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, *ops):
        tk = self.t[self.i]
        return tk[0] == 'op' and tk[1] in ops

    def take(self, op=None):
        tk = self.t[self.i]
        if op and not (tk[0] == 'op' and tk[1] == op):
            raise Fail('expected %r' % op)
        self.i += 1
        return tk

    def parse(self):
        e = self.ternary()
        if self.t[self.i][0] != 'end':
            raise Fail('unexpected %r' % (self.t[self.i],))
        return e

    def ternary(self):
        c = self.logic()
        if self.peek('?'):
            self.take('?')
            a = self.ternary()
            self.take(':')
            b = self.ternary()
            fl = a[1] or b[1]
            return ('((%s) ? (%s) : (%s))' % (truth(c), cast(a, fl), cast(b, fl)), fl)
        return c

    def logic(self):
        a = self.compare()
        while self.peek('&&', '||'):
            op = self.take()[1]
            b = self.compare()
            a = ('(%s %s %s)' % (truth(a), op, truth(b)), False)
        return a

    def compare(self):
        a = self.add()
        while self.peek('==', '!=', '<', '>', '<=', '>='):
            op = self.take()[1]
            b = self.add()
            a = ('((double)(%s) %s (double)(%s))' % (a[0], op, b[0]), False)
        return a

    def add(self):
        a = self.mul()
        while self.peek('+', '-'):
            op = self.take()[1]
            b = self.mul()
            fl = a[1] or b[1]
            a = ('(%s %s %s)' % (cast(a, fl), op, cast(b, fl)), fl)
        return a

    def mul(self):
        a = self.unary()
        while self.peek('*', '/', '%'):
            op = self.take()[1]
            b = self.unary()
            fl = a[1] or b[1]
            if op == '*':
                a = ('(%s * %s)' % (cast(a, fl), cast(b, fl)), fl)
            elif fl:
                a = (('rf_div(%s, %s)' if op == '/' else 'rf_fmod(%s, %s)') % (cast(a, 1), cast(b, 1)), True)
            else:
                a = (('ri_div(%s, %s)' if op == '/' else 'ri_mod(%s, %s)') % (a[0], b[0]), False)
        return a

    def unary(self):
        if self.peek('-'):
            self.take()
            a = self.unary()
            return ('(-%s)' % a[0], a[1])
        if self.peek('+'):
            self.take()
            return self.unary()
        if self.peek('!'):
            self.take()
            a = self.unary()
            return ('(!%s)' % truth(a), False)
        return self.postfix()

    def postfix(self):
        a = self.primary()
        while self.peek('.'):
            self.take('.')
            name = self.take()
            if name[0] != 'name':
                raise Fail('method expected')
            if name[1] in ('min', 'max') and a[0].startswith('LIST:'):
                items = a[2]
                fl = any(x[1] for x in items)
                fn = ('rf_' if fl else 'ri_') + name[1]
                expr = cast(items[0], fl)
                for x in items[1:]:
                    expr = '%s(%s, %s)' % (fn, expr, cast(x, fl))
                a = (expr, fl)
            elif name[1] == 'to_i':
                a = ('((int64_t)(%s))' % a[0], False)
            elif name[1] == 'to_f':
                a = ('((double)(%s))' % a[0], True)
            elif name[1] == 'abs':
                a = (('fabs(%s)' if a[1] else 'ri_abs(%s)') % a[0], a[1])
            else:
                raise Fail('method %s' % name[1])
        return a

    def primary(self):
        tk = self.t[self.i]
        if tk[0] == 'num':
            self.i += 1
            return (tk[1] if tk[2] else 'INT64_C(%s)' % tk[1], tk[2])
        if self.peek('('):
            self.take('(')
            e = self.ternary()
            self.take(')')
            return ('(%s)' % e[0], e[1])
        if self.peek('['):
            self.take('[')
            items = [self.ternary()]
            while self.peek(','):
                self.take(',')
                items.append(self.ternary())
            self.take(']')
            return ('LIST:', False, items)
        if tk[0] == 'name' and tk[1] in ('a', 'b'):
            self.i += 1
            who = tk[1]
            self.take('.')
            m = self.take()
            if m[0] != 'name':
                raise Fail('stat expected')
            s = m[1]
            if s in PARAMS:
                return ('((int64_t)rpg_battler_param(%s, %d))' % (who, PARAMS[s]), False)
            if s == 'hp':
                return ('((int64_t)%s->hp)' % who, False)
            if s == 'mp':
                return ('((int64_t)%s->mp)' % who, False)
            if s == 'tp':
                return ('((double)%s->tp)' % who, True)
            if s == 'level':
                return ('((int64_t)rf_level(%s))' % who, False)
            if s in XPARAMS:
                return ('rpg_battler_xparam(%s, %d)' % (who, XPARAMS[s]), True)
            if s in SPARAMS:
                return ('rpg_battler_sparam(%s, %d)' % (who, SPARAMS[s]), True)
            if s == 'state?':
                self.take('(')
                n = self.ternary()
                self.take(')')
                return ('rpg_battler_state(%s, (int)(%s))' % (who, n[0]), False)
            raise Fail('unknown stat %s.%s' % (who, s))
        if tk[0] == 'name' and tk[1] == 'v':
            self.i += 1
            self.take('[')
            n = self.ternary()
            self.take(']')
            return ('((int64_t)rpg_var((int)(%s)))' % n[0], False)
        if tk[0] == 'name' and tk[1] in ('true', 'false'):
            self.i += 1
            return ('1' if tk[1] == 'true' else '0', False)
        raise Fail('unexpected %r' % (tk,))


def cast(e, fl):
    return '((double)(%s))' % e[0] if fl and not e[1] else e[0]


def truth(e):
    return '(%s != 0)' % e[0]


def compile_one(formula):
    f = (formula or '').strip() or '0'
    expr, fl = Parser(tokens(f)).parse()
    return expr, fl


HEADER = r'''/* GENERATED by vcpe/tools/rpgmaker/vrpg_formula.py from the project's damage formulas: do not edit */
#include "vcpe_rpg.h"
#include <math.h>
#include <stdint.h>
__attribute__((unused)) static int64_t ri_div(int64_t a, int64_t b) { if (!b) return 0; int64_t q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }
__attribute__((unused)) static int64_t ri_mod(int64_t a, int64_t b) { if (!b) return 0; int64_t r = a % b; if (r && ((r < 0) != (b < 0))) r += b; return r; }
__attribute__((unused)) static double rf_div(double a, double b) { return b != 0 ? a / b : (a == 0 ? NAN : (a > 0 ? INFINITY : -INFINITY)); }
__attribute__((unused)) static double rf_fmod(double a, double b) { double r = fmod(a, b); if (r != 0 && ((r < 0) != (b < 0))) r += b; return r; }
__attribute__((unused)) static int64_t ri_min(int64_t a, int64_t b) { return a < b ? a : b; }
__attribute__((unused)) static int64_t ri_max(int64_t a, int64_t b) { return a > b ? a : b; }
__attribute__((unused)) static double rf_min(double a, double b) { return a < b ? a : b; }
__attribute__((unused)) static double rf_max(double a, double b) { return a > b ? a : b; }
__attribute__((unused)) static int64_t ri_abs(int64_t a) { return a < 0 ? -a : a; }
__attribute__((unused)) static int rf_level(const RpgBattler *x) { const RpgActorState *s = rpg_battler_actor(x); return s ? s->level : 0; }
'''


def compile_formulas(skills, items):
    """skills / items: [(id, formula)] -> C source. Raises ValueError naming any formula it cannot translate."""
    out = [HEADER]
    out.append('double rpg_formula_c(int kind, int id, const RpgBattler *a, const RpgBattler *b)\n{')
    out.append('    (void)a; (void)b;')
    for kind, rows in (('RPG_KIND_SKILL', skills), ('RPG_KIND_ITEM', items)):
        out.append('    if (kind == %s) switch (id) {' % kind)
        for i, f in rows:
            try:
                expr, fl = compile_one(f)
            except Fail as e:
                raise ValueError('%s %d formula %r: %s' % (kind, i, f, e))
            if expr in ('INT64_C(0)', '0'):
                continue
            out.append('    case %d: return (double)(%s);   /* %s */' % (i, expr, f.replace('*/', '* /')))
        out.append('    }')
    out.append('    return 0;\n}')
    return '\n'.join(out) + '\n'
