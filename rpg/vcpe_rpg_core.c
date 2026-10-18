/* VCPE RPG core: the game state, switches / variables / self switches, input (RPG Maker's trigger / repeat), the
 * random generator, the scene manager and Graphics.brightness. */
#include "vcpe_rpg.h"
#include "vcpe_math.h"
#include <string.h>
#include <stdio.h>

RpgGame rpg;
RpgTemp rpg_temp;
RpgHooks rpg_hooks;
RpgInput rpg_in;
RpgScreen rpg_battle_screen;
int (*rpg_hooks_wait)(RpgInterp *in, int arg);

void rpg_sw_set(int id, int on)
{
    if (id > 0 && id < RPG_MAX_SWITCHES) {
        if (rpg.switches[id] != (on != 0))
            rpg.need_refresh = 1;
        rpg.switches[id] = on != 0;
    }
}

void rpg_var_set(int id, int32_t v)
{
    if (id > 0 && id < RPG_MAX_VARIABLES) {
        if (rpg.variables[id] != v)
            rpg.need_refresh = 1;
        rpg.variables[id] = v;
    }
}

/* self switches: an open-addressed set of the keys that are ON */
static uint32_t sw_key(int map, int event, int ch) { return (((uint32_t)map << 14) | ((uint32_t)event << 2) | (ch & 3)) + 1; }
static uint32_t sw_hash(uint32_t k) { k *= 2654435761u; return k >> 18; }

int rpg_self_sw(int map, int event, int ch)
{
    if (ch < 0 || ch > 3)
        return 0;
    uint32_t k = sw_key(map, event, ch);
    for (uint32_t i = sw_hash(k), n = 0; n < RPG_SELF_SW_CAP; i = (i + 1) % RPG_SELF_SW_CAP, n++) {
        if (rpg.self_sw[i] == k)
            return 1;
        if (rpg.self_sw[i] == 0)
            return 0;
    }
    return 0;
}

void rpg_self_sw_set(int map, int event, int ch, int on)
{
    if (ch < 0 || ch > 3)
        return;
    uint32_t k = sw_key(map, event, ch);
    uint32_t i = sw_hash(k) % RPG_SELF_SW_CAP;
    if (on) {
        for (uint32_t n = 0; n < RPG_SELF_SW_CAP; i = (i + 1) % RPG_SELF_SW_CAP, n++) {
            if (rpg.self_sw[i] == k)
                return;
            if (rpg.self_sw[i] == 0 || rpg.self_sw[i] == 0xffffffffu) {
                rpg.self_sw[i] = k;
                rpg.nself++;
                rpg.need_refresh = 1;
                return;
            }
        }
    } else {
        for (uint32_t n = 0; n < RPG_SELF_SW_CAP; i = (i + 1) % RPG_SELF_SW_CAP, n++) {
            if (rpg.self_sw[i] == 0)
                return;
            if (rpg.self_sw[i] == k) {
                rpg.self_sw[i] = 0xffffffffu;   /* tombstone: keeps later keys reachable */
                rpg.nself--;
                rpg.need_refresh = 1;
                return;
            }
        }
    }
}

void rpg_self_sw_clear_all(void)
{
    memset(rpg.self_sw, 0, sizeof rpg.self_sw);
    rpg.nself = 0;
    rpg.need_refresh = 1;
}

/* every self switch that is ON: map, event, ch (for scripts that change many at once) */
int rpg_self_sw_each(void (*fn)(int map, int event, int ch, void *u), void *u)
{
    int n = 0;
    for (int i = 0; i < RPG_SELF_SW_CAP; i++) {
        uint32_t k = rpg.self_sw[i];
        if (k == 0 || k == 0xffffffffu)
            continue;
        k--;
        fn(k >> 14, (k >> 2) & 4095, k & 3, u);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ input */
static uint32_t last_held;
static int repeat_count[16];

void rpg_input_set(uint32_t held)
{
    rpg_in.trigger = held & ~last_held;
    rpg_in.press = held;
    rpg_in.repeat = 0;
    /* Input.repeat?: true on the press, then after 24 frames every 6 frames */
    for (int b = 0; b < 16; b++) {
        uint32_t m = 1u << b;
        if (held & m) {
            int c = ++repeat_count[b];
            if (c == 1 || (c > 24 && (c - 24) % 6 == 0))
                rpg_in.repeat |= m;
        } else {
            repeat_count[b] = 0;
        }
    }
    last_held = held;
}

int rpg_dir4(void)
{
    /* Input.dir4: the last pressed direction wins when several are held (RGSS keeps press order; most recent) */
    static int last;
    uint32_t p = rpg_in.press;
    if (rpg_in.trigger & RPG_K_DOWN) last = 2;
    if (rpg_in.trigger & RPG_K_LEFT) last = 4;
    if (rpg_in.trigger & RPG_K_RIGHT) last = 6;
    if (rpg_in.trigger & RPG_K_UP) last = 8;
    int ok = (last == 2 && (p & RPG_K_DOWN)) || (last == 4 && (p & RPG_K_LEFT)) || (last == 6 && (p & RPG_K_RIGHT)) ||
             (last == 8 && (p & RPG_K_UP));
    if (ok)
        return last;
    if (p & RPG_K_DOWN) return 2;
    if (p & RPG_K_LEFT) return 4;
    if (p & RPG_K_RIGHT) return 6;
    if (p & RPG_K_UP) return 8;
    return 0;
}

int rpg_dir8(void)
{
    uint32_t p = rpg_in.press;
    int h = (p & RPG_K_LEFT) ? 4 : (p & RPG_K_RIGHT) ? 6 : 0;
    int v = (p & RPG_K_DOWN) ? 2 : (p & RPG_K_UP) ? 8 : 0;
    if (h && v) {
        if (h == 4) return v == 2 ? 1 : 7;
        return v == 2 ? 3 : 9;
    }
    return h ? h : v;
}

/* conditional branch "button": the Input symbols 2 4 6 8 DOWN LEFT RIGHT UP, 11.. A B C X Y Z L R */
int rpg_input_press_code(int code)
{
    static const uint32_t map[32] = {[2] = RPG_K_DOWN, [4] = RPG_K_LEFT, [6] = RPG_K_RIGHT, [8] = RPG_K_UP,
                                     [11] = RPG_K_A, [12] = RPG_K_B, [13] = RPG_K_C, [14] = RPG_K_X,
                                     [15] = RPG_K_Y, [16] = RPG_K_Z, [17] = RPG_K_L, [18] = RPG_K_R};
    return code >= 0 && code < 32 && (rpg_in.press & map[code]);
}

int rpg_rand(int n) { return n > 0 ? (int)vcpe_irand(n) : 0; }
float rpg_randf(void) { return vcpe_frand(); }

/* ------------------------------------------------------------------ vocabulary (VX Ace's English defaults) */
RpgVocab rpg_vocab = {
    "Buy", "Sell", "Cancel", "Possession", "Current Exp", "To Next %s", "Save to which file?", "Load which file?",
    "File", "%s's Party", "%s emerged!", "%s got the upper hand!", "%s was surprised!", "%s has started to escape!",
    "However, it was unable to escape!", "%s was victorious!", "%s was defeated.", "%s EXP received!",
    "%s\\G found!", "%s found!", "%s is now %s %s!", "%s learned!", "%s uses %s!", "An excellent hit!!",
    "A painful blow!!", "%s took %s damage!", "%s recovered %s %s!", "%s gained %s %s!", "%s lost %s %s!",
    "%s was drained of %s %s!", "%s took no damage!", "Miss! %s took no damage!", "%s took %s damage!",
    "%s recovered %s %s!", "%s gained %s %s!", "%s lost %s %s!", "Drained %s %s from %s!", "%s took no damage!",
    "Miss! %s took no damage!", "%s evaded the attack!", "%s nullified the magic!", "%s reflected the magic!",
    "%s counterattacked!", "%s protected %s!", "%s's %s went up!", "%s's %s went down!",
    "%s's %s returned to normal!", "There was no effect on %s!"};

const char *rpg_term_basic(int i) { return i >= 0 && i < 8 ? RPG_STR(rpg_system()->terms.basic[i]) : ""; }
const char *rpg_term_param(int i) { return i >= 0 && i < 8 ? RPG_STR(rpg_system()->terms.params[i]) : ""; }
const char *rpg_term_etype(int i) { return i >= 0 && i < 5 ? RPG_STR(rpg_system()->terms.etypes[i]) : ""; }
const char *rpg_term_command(int i) { return i >= 0 && i < 23 ? RPG_STR(rpg_system()->terms.commands[i]) : ""; }
