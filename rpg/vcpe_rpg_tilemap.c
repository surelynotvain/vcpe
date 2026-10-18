/* VCPE RPG tilemap: draws VX Ace map layers from the tileset's nine images (A1 A2 A3 A4 A5 B C D E).
 *
 * Tile ids: B..E 0-1023 (256 per image), A5 1536-1663, A1 2048-2815, A2 2816-4351, A3 4352-5887, A4 5888-8191.
 * Autotiles (A1-A4) are 48 ids per kind: kind = (id - 2048) / 48, shape = (id - 2048) % 48. A shape is drawn from four
 * 16x16 quarters picked out of the kind's block:
 *
 * floor block (64 x 96, A1 / A2 / A4 tops): quarters on a 4 x 6 grid
 *     rows 0-1, columns 2-3: the inner corners (where both sides connect but the diagonal does not)
 *     rows 2-5: a 2 x 2 sample: outer corners at its corners, edges along its sides, the centre inside
 * wall block (64 x 64, A3 / A4 faces): quarters on a 4 x 4 grid, corners / edges / centre the same way, no inner
 *     corners
 * waterfall block (64 x 32, A1): two halves, edge or centre by the left / right neighbours
 *
 * Each quarter only depends on its two sides and its diagonal (e.g. top-left: above, left, above-left), so the
 * shape tables are built at start from the shape's neighbour mask (rpg_autotile_mask) instead of being stored. */
#include "vcpe_rpg.h"
#include "vcpe_tiles.h"
#include "vcpe_spr.h"
#include <string.h>

int rpg_tile_anim;

/* neighbour mask bits: which sides are a different tile (the shape draws an edge there) */
enum { E_L = 1, E_T = 2, E_R = 4, E_B = 8, C_TL = 16, C_TR = 32, C_BR = 64, C_BL = 128 };   /* C_*: inner corner */

/* VX Ace's floor shape order: 0-15 every side connected, inner corners as bits (TL 1, TR 2, BR 4, BL 8);
 * 16-19 left edge (+TR, +BR inner corners), 20-23 top edge (+BR, +BL), 24-27 right edge (+BL, +TL), 28-31 bottom
 * edge (+TL, +TR); 32 left+right, 33 top+bottom; 34-35 top-left (+BR), 36-37 top-right (+BL), 38-39 bottom-right
 * (+TL), 40-41 bottom-left (+TR); 42 open below, 43 open right, 44 open above, 45 open left, 46 isolated */
static int floor_mask(int s)
{
    if (s < 16)
        return (s & 1 ? C_TL : 0) | (s & 2 ? C_TR : 0) | (s & 4 ? C_BR : 0) | (s & 8 ? C_BL : 0);
    if (s < 20)
        return E_L | (s & 1 ? C_TR : 0) | (s & 2 ? C_BR : 0);
    if (s < 24)
        return E_T | (s & 1 ? C_BR : 0) | (s & 2 ? C_BL : 0);
    if (s < 28)
        return E_R | (s & 1 ? C_BL : 0) | (s & 2 ? C_TL : 0);
    if (s < 32)
        return E_B | (s & 1 ? C_TL : 0) | (s & 2 ? C_TR : 0);
    switch (s) {
    case 32: return E_L | E_R;
    case 33: return E_T | E_B;
    case 34: return E_T | E_L;
    case 35: return E_T | E_L | C_BR;
    case 36: return E_T | E_R;
    case 37: return E_T | E_R | C_BL;
    case 38: return E_B | E_R;
    case 39: return E_B | E_R | C_TL;
    case 40: return E_B | E_L;
    case 41: return E_B | E_L | C_TR;
    case 42: return E_T | E_L | E_R;
    case 43: return E_T | E_L | E_B;
    case 44: return E_L | E_B | E_R;
    case 45: return E_T | E_R | E_B;
    default: return E_T | E_L | E_R | E_B;
    }
}

/* wall shapes 0-15: edges as bits left 1, top 2, right 4, bottom 8 */
static int wall_mask(int s)
{
    return (s & 1 ? E_L : 0) | (s & 2 ? E_T : 0) | (s & 4 ? E_R : 0) | (s & 8 ? E_B : 0);
}

/* quarter q (0 TL, 1 TR, 2 BL, 3 BR) of a floor shape -> source quarter (column, row) on the 4 x 6 grid */
static void floor_quarter(int mask, int q, int *cx, int *cy)
{
    int right = q & 1, bottom = q >> 1;
    int side_h = right ? (mask & E_R) : (mask & E_L);       /* horizontal neighbour missing */
    int side_v = bottom ? (mask & E_B) : (mask & E_T);      /* vertical neighbour missing */
    int corner = (q == 0 ? C_TL : q == 1 ? C_TR : q == 3 ? C_BR : C_BL) & mask;
    if (!side_h && !side_v && corner) {
        *cx = 2 + right;
        *cy = bottom;
        return;
    }
    /* the 2x2 sample: column 0 / 3 = outer left / right, 1 / 2 = inside; row 2 / 5 = outer top / bottom */
    *cx = side_h ? (right ? 3 : 0) : (right ? 1 : 2);
    *cy = side_v ? (bottom ? 5 : 2) : (bottom ? 3 : 4);
}

static void wall_quarter(int mask, int q, int *cx, int *cy)
{
    int right = q & 1, bottom = q >> 1;
    int side_h = right ? (mask & E_R) : (mask & E_L);
    int side_v = bottom ? (mask & E_B) : (mask & E_T);
    *cx = side_h ? (right ? 3 : 0) : (right ? 1 : 2);
    *cy = side_v ? (bottom ? 3 : 0) : (bottom ? 1 : 2);
}

static uint8_t floor_tab[48][4][2], wall_tab[48][4][2];
/* A2 "table" tiles (the counter flag): the upper quarters as usual, the lower ones from the block's third row, and
 * the legs, 8 px lower, from the bottom edge (or the inner corners where the table continues below) */
static int8_t table_tab[48][6][2];

void rpg_tilemap_init(void)
{
    for (int s = 0; s < 48; s++)
        for (int q = 0; q < 4; q++) {
            int x, y;
            floor_quarter(floor_mask(s), q, &x, &y);
            if (s == 47) {   /* the block's top-left 32x32 preview tile */
                x = q & 1;
                y = q >> 1;
            }
            floor_tab[s][q][0] = x * 16;
            floor_tab[s][q][1] = y * 16;
            if (q < 2) {
                table_tab[s][q][0] = floor_tab[s][q][0];
                table_tab[s][q][1] = floor_tab[s][q][1];
            } else {
                int m = floor_mask(s), right = q & 1;
                table_tab[s][q][0] = (right ? (m & E_R ? 3 : 1) : (m & E_L ? 0 : 2)) * 16;
                table_tab[s][q][1] = 48;
                int leg = right ? 5 : 4;
                if (m & E_B) {
                    table_tab[s][leg][0] = (right ? (m & E_R ? 3 : 1) : (m & E_L ? 0 : 2)) * 16;
                    table_tab[s][leg][1] = 80;
                } else if (m & (right ? C_BR : C_BL)) {
                    table_tab[s][leg][0] = (right ? 3 : 2) * 16;
                    table_tab[s][leg][1] = 16;
                } else {
                    table_tab[s][leg][0] = -1;
                }
            }
            if (s == 47) {
                static const int8_t pv[6][2] = {{0, 0}, {16, 0}, {0, 48}, {48, 48}, {0, 16}, {16, 16}};
                if (q == 3)
                    memcpy(table_tab[47], pv, sizeof pv);
            }
            wall_quarter(wall_mask(s & 15), q, &x, &y);
            wall_tab[s][q][0] = x * 16;
            wall_tab[s][q][1] = y * 16;
        }
}

/* for tests: the source offsets of shape s, quarter q */
void rpg_autotile_quarter(int wall, int s, int q, int *x, int *y)
{
    *x = wall ? wall_tab[s][q][0] : floor_tab[s][q][0];
    *y = wall ? wall_tab[s][q][1] : floor_tab[s][q][1];
}

void rpg_autotile_table(int s, int i, int *x, int *y)
{
    *x = table_tab[s][i][0];
    *y = table_tab[s][i][1];
}

/* draw tile id with its top-left at world pixel (x, y); color is the vertex colour (opacity) */
void rpg_tile_draw(int id, float x, float y, uint32_t color)
{
    if (id <= 0)
        return;
    if (id < 1024) {
        int img = rpg_map_tile_image(5 + id / 256);
        int n = id % 256;
        vcpe_tiles_part(img, (n % 8 + n / 128 * 8) * 32, (n / 8 % 16) * 32, 32, 32, x, y, color);
        return;
    }
    if (id >= 1536 && id < 1664) {
        int n = id - 1536;
        vcpe_tiles_part(rpg_map_tile_image(4), n % 8 * 32, n / 8 * 32, 32, 32, x, y, color);
        return;
    }
    if (id < 2048 || id >= 8192)
        return;
    int shape = (id - 2048) % 48;
    int ox = 0, oy = 0, img, wall = 0;
    static const int wave[4] = {0, 1, 2, 1};
    int w = wave[(rpg_tile_anim / 30) % 4];
    if (id < 2816) {
        int k = (id - 2048) / 48;
        img = rpg_map_tile_image(0);
        switch (k) {
        case 0: ox = w * 64; break;
        case 1: ox = w * 64; oy = 96; break;
        case 2: ox = 192; break;
        case 3: ox = 192; oy = 96; break;
        default: {
            int tx = k % 8, ty = k / 8;
            ox = tx / 4 * 256;
            oy = ty * 192 + (tx % 4) / 2 * 96;
            if (k % 2 == 0) {
                ox += w * 64;
            } else {
                /* waterfall: three frames stacked, two 16-wide halves */
                ox += 192;
                oy += (rpg_tile_anim / 30 % 3) * 32;
                int s = shape & 3;
                int lx = (s & 1) ? 0 : 32, rx = (s & 2) ? 48 : 16;
                vcpe_tiles_part(img, ox + lx, oy, 16, 32, x, y, color);
                vcpe_tiles_part(img, ox + rx, oy, 16, 32, x + 16, y, color);
                return;
            }
        }
        }
    } else if (id < 4352) {
        int k = (id - 2816) / 48;
        img = rpg_map_tile_image(1);
        ox = k % 8 * 64;
        oy = k / 8 * 96;
        if (rpg_map_flag(id) & 0x80) {
            for (int i = 0; i < 6; i++) {
                const int8_t *p = table_tab[shape][i];
                if (p[0] < 0)
                    continue;
                float dy = i < 4 ? (i >> 1) * 16 : 24;
                vcpe_tiles_part(img, ox + p[0], oy + p[1], 16, 16, x + (i & 1) * 16, y + dy, color);
            }
            return;
        }
    } else if (id < 5888) {
        int k = (id - 4352) / 48;
        img = rpg_map_tile_image(2);
        ox = k % 8 * 64;
        oy = k / 8 * 64;
        wall = 1;
    } else {
        int k = (id - 5888) / 48;
        static const int rows[6] = {0, 3, 5, 8, 10, 13};
        img = rpg_map_tile_image(3);
        ox = k % 8 * 64;
        oy = rows[k / 8] * 32;
        wall = (k / 8) & 1;
    }
    for (int q = 0; q < 4; q++) {
        const uint8_t *p = wall ? wall_tab[shape][q] : floor_tab[shape][q];
        vcpe_tiles_part(img, ox + p[0], oy + p[1], 16, 16, x + (q & 1) * 16, y + (q >> 1) * 16, color);
    }
}
