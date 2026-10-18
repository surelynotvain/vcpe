/* VCPE cook: sprites. Graphics/<folder>/<name>.png|jpg|bmp -> the VSPR section vcpe_spr reads, the same layout
 * tools/rpgmaker/vrpg_assets.py writes: hue change, Lanczos resize (premultiplied, like PIL), trim, exact palette up to
 * 255 colours or a median-cut quantizer, 256x256 skyline pages (last 4 tried), swizzled, 8-bit pages first. */
#include "vcpe_cook.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../src/ext/stb_image.h"

#define PAGE 256

/* ------------------------------------------------------------------ images */
typedef struct { int w, h; uint8_t *px; } Img;   /* RGBA8 */

static void img_free(Img *im) { free(im->px); im->px = 0; }

static int img_load(CookFs *fs, CookFile *f, Img *im)
{
    uint32_t n;
    uint8_t *b = cook_fs_read(fs, f, &n);
    if (!b)
        return -1;
    int c;
    im->px = stbi_load_from_memory(b, (int)n, &im->w, &im->h, &c, 4);
    free(b);
    return im->px ? 0 : -1;
}

static void hue_change(Img *im, int hue)
{
    size_t n = (size_t)im->w * im->h;
    for (size_t p = 0; p < n; p++) {
        uint8_t *q = im->px + p * 4;
        float r = q[0] / 255.f, g = q[1] / 255.f, b = q[2] / 255.f;
        float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn, h = 0;
        if (d > 1e-6f) {
            if (mx == r)
                h = fmodf((g - b) / d, 6.f);
            else if (mx == g)
                h = (b - r) / d + 2;
            else
                h = (r - g) / d + 4;
            if (h < 0)
                h += 6;
        }
        h = fmodf(h * 60 + hue, 360.f);
        if (h < 0)
            h += 360;
        float s = mx > 0 ? d / mx : 0, v = mx, c = v * s;
        float x = c * (1 - fabsf(fmodf(h / 60, 2.f) - 1));
        int sec = ((int)(h / 60)) % 6;
        static const int RC[6][3] = {{0, 1, 3}, {1, 0, 3}, {3, 0, 1}, {3, 1, 0}, {1, 3, 0}, {0, 3, 1}};
        float t[4] = {c, x, 0, 0};
        float o[3];
        for (int k = 0; k < 3; k++)
            o[k] = t[RC[sec][k]] + (v - c);
        for (int k = 0; k < 3; k++) {
            float u = o[k] * 255 + 0.5f;
            q[k] = u < 0 ? 0 : u > 255 ? 255 : (uint8_t)u;
        }
    }
}

/* PIL-style separable Lanczos (a = 3) on premultiplied RGBA */
static double lanczos(double x)
{
    if (x <= -3 || x >= 3)
        return 0;
    if (x > -1e-9 && x < 1e-9)
        return 1;
    double px = M_PI * x;
    return 3 * sin(px) * sin(px / 3) / (px * px);
}

typedef struct { int first, n; float *w; } Taps;

static Taps *make_taps(int in, int out)
{
    double scale = (double)in / out, fs = scale < 1 ? 1 : scale, support = 3 * fs;
    Taps *t = calloc(out, sizeof(Taps));
    int ksize = (int)ceil(support) * 2 + 1;
    float *all = malloc(sizeof(float) * ksize * out);
    for (int i = 0; i < out; i++) {
        double center = (i + 0.5) * scale;
        int lo = (int)(center - support + 0.5), hi = (int)(center + support + 0.5);
        if (lo < 0)
            lo = 0;
        if (hi > in)
            hi = in;
        float *w = all + (size_t)i * ksize;
        double sum = 0;
        for (int x = lo; x < hi; x++) {
            double v = lanczos((x - center + 0.5) / fs);
            w[x - lo] = (float)v;
            sum += v;
        }
        if (sum != 0)
            for (int x = lo; x < hi; x++)
                w[x - lo] = (float)(w[x - lo] / sum);
        t[i].first = lo;
        t[i].n = hi - lo;
        t[i].w = w;
    }
    return t;
}

static void free_taps(Taps *t) { free(t[0].w); free(t); }

static int16_t q6(float v)
{
    v *= 64.f;
    return v < -32767 ? -32767 : v > 32767 ? 32767 : (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* two passes; the source is premultiplied on the fly and the middle image kept as int16 (x64), so a 1600x900
 * battleback needs about 9 MB instead of 40 */
static int img_resize(Img *im, int nw, int nh)
{
    int w = im->w, h = im->h;
    int16_t *mid = malloc(sizeof(int16_t) * 4 * (size_t)nw * h);
    uint8_t *out = malloc((size_t)nw * nh * 4);
    if (!mid || !out) {
        free(mid); free(out);
        return -1;
    }
    Taps *tx = make_taps(w, nw), *ty = make_taps(h, nh);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < nw; x++) {
            float s[4] = {0, 0, 0, 0};
            const float *wt = tx[x].w;
            for (int k = 0; k < tx[x].n; k++) {
                const uint8_t *q = im->px + ((size_t)y * w + tx[x].first + k) * 4;
                float a = q[3] * (1.f / 255.f), c = wt[k];
                s[0] += q[0] * a * c;
                s[1] += q[1] * a * c;
                s[2] += q[2] * a * c;
                s[3] += q[3] * c;
            }
            int16_t *m = mid + ((size_t)y * nw + x) * 4;
            for (int c = 0; c < 4; c++)
                m[c] = q6(s[c]);
        }
    free(im->px);
    im->px = 0;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            float s[4] = {0, 0, 0, 0};
            const float *wt = ty[y].w;
            for (int k = 0; k < ty[y].n; k++) {
                const int16_t *q = mid + ((size_t)(ty[y].first + k) * nw + x) * 4;
                for (int c = 0; c < 4; c++)
                    s[c] += q[c] * wt[k];
            }
            for (int c = 0; c < 4; c++)
                s[c] *= 1.f / 64.f;
            float a = s[3] < 0 ? 0 : s[3] > 255 ? 255 : s[3];
            uint8_t *o = out + ((size_t)y * nw + x) * 4;
            int ai = (int)(a + 0.5f);
            o[3] = (uint8_t)ai;
            for (int c = 0; c < 3; c++) {
                float v = ai ? s[c] * 255.f / a : 0;
                o[c] = v < 0 ? 0 : v > 254.5f ? 255 : (uint8_t)(v + 0.5f);
            }
        }
    free_taps(tx);
    free_taps(ty);
    free(mid);
    im->px = out;
    im->w = nw;
    im->h = nh;
    return 0;
}

/* ------------------------------------------------------------------ quantizer */
static uint32_t pack_px(const uint8_t *q) { return q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24; }

static int u32cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct { uint32_t c; uint32_t n; int box; } UCol;

static int ucol_cmp_ch;
static int ucol_cmp(const void *a, const void *b)
{
    int s = ucol_cmp_ch * 8;
    int x = (((const UCol *)a)->c >> s) & 255, y = (((const UCol *)b)->c >> s) & 255;
    return x - y;
}

/* Only the two boxes changed by a split need their range/weight rescanned. */
static double box_score(const UCol *u, int lo, int hi, int *channel)
{
    if (hi - lo < 2) return 0;
    int mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0};
    double tot = 0, best = 0;
    for (int i = lo; i < hi; i++) {
        for (int c = 0; c < 4; c++) {
            int v = (u[i].c >> (c * 8)) & 255;
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }
        tot += u[i].n;
    }
    for (int c = 0; c < 4; c++) {
        double sc = (mx[c] - mn[c]) * sqrt(tot);
        if (sc > best) { best = sc; *channel = c; }
    }
    return best;
}

/* Weighted RGBA median cut, preserving split order and palette byte order. */
static int median_cut(UCol *u, int nu, uint32_t *pal, int maxc)
{
    int lo[255], hi[255], channel[255] = {0}, nb = 1;
    double score[255];
    lo[0] = 0;
    hi[0] = nu;
    score[0] = box_score(u, 0, nu, &channel[0]);
    while (nb < maxc) {
        int best = -1, bch = 0;
        double bscore = 0;
        for (int b = 0; b < nb; b++) {
            if (score[b] > bscore) { bscore = score[b]; best = b; bch = channel[b]; }
        }
        if (best < 0)
            break;
        ucol_cmp_ch = bch;
        qsort(u + lo[best], hi[best] - lo[best], sizeof(UCol), ucol_cmp);
        double tot = 0, half;
        for (int i = lo[best]; i < hi[best]; i++)
            tot += u[i].n;
        half = tot / 2;
        double acc = 0;
        int cut = lo[best] + 1;
        for (int i = lo[best]; i < hi[best] - 1; i++) {
            acc += u[i].n;
            cut = i + 1;
            if (acc >= half)
                break;
        }
        lo[nb] = cut;
        hi[nb] = hi[best];
        hi[best] = cut;
        score[best] = box_score(u, lo[best], hi[best], &channel[best]);
        score[nb] = box_score(u, lo[nb], hi[nb], &channel[nb]);
        nb++;
    }
    for (int b = 0; b < nb; b++) {
        double s[4] = {0, 0, 0, 0}, tot = 0;
        for (int i = lo[b]; i < hi[b]; i++) {
            u[i].box = b;
            for (int c = 0; c < 4; c++)
                s[c] += ((u[i].c >> (c * 8)) & 255) * (double)u[i].n;
            tot += u[i].n;
        }
        uint32_t p = 0;
        for (int c = 0; c < 4; c++)
            p |= (uint32_t)(s[c] / tot + 0.5) << (c * 8);
        pal[b] = p;
    }
    return nb;
}

static int nearest(const uint32_t *pal, int n, const int v[4])
{
    int best = 0;
    int bd = 5 * 255 * 255 + 1; /* RGBA distance fits 32 bits, including on MIPS */
    for (int i = 0; i < n; i++) {
        int d = 0;
        for (int c = 0; c < 4; c++) {
            int e = (int)((pal[i] >> (c * 8)) & 255) - v[c];
            d += e * e * (c == 3 ? 2 : 1);
        }
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* -> palette entries [1..np] (index 0 transparent); ind[] filled (0 = transparent) */
static int quantize(const uint8_t *px, int w, int h, float dither, uint32_t *pal, uint8_t *ind)
{
    size_t n = (size_t)w * h;
    uint32_t *all = malloc(n * 4);
    if (!all)
        return -1;
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (px[i * 4 + 3])
            all[m++] = pack_px(px + i * 4);
    qsort(all, m, 4, u32cmp);
    UCol *u = malloc(sizeof(UCol) * (m ? m : 1));
    int nu = 0;
    for (size_t i = 0; i < m; i++) {
        if (nu && u[nu - 1].c == all[i])
            u[nu - 1].n++;
        else {
            u[nu].c = all[i]; u[nu].n = 1; u[nu].box = 0; nu++;
        }
    }
    free(all);
    uint32_t p0[255];
    int np = median_cut(u, nu, p0, 255);
    for (int i = 0; i < np; i++)
        pal[i + 1] = p0[i];
    /* lookup: exact colour -> box (binary search over the sorted-by-channel order is gone, so use a hash on the colour) */
    int hc = 1;
    while (hc < nu * 2 + 16)
        hc <<= 1;
    int *hash = calloc(hc, sizeof(int));
    for (int i = 0; i < nu; i++) {
        uint32_t k = (u[i].c * 2654435761u) & (hc - 1);
        while (hash[k])
            k = (k + 1) & (hc - 1);
        hash[k] = i + 1;
    }
    uint8_t *memo = 0;
    float *err = 0;
    if (dither > 0) {
        memo = malloc(1 << 17);
        memset(memo, 255, 1 << 17);
        err = calloc((size_t)(w + 2) * 2 * 4, sizeof(float));
    }
    for (int y = 0; y < h; y++) {
        float *e0 = err ? err + (size_t)(y & 1) * (w + 2) * 4 : 0, *e1 = err ? err + (size_t)((y + 1) & 1) * (w + 2) * 4 : 0;
        if (err)
            memset(e1, 0, sizeof(float) * (w + 2) * 4);
        for (int x = 0; x < w; x++) {
            const uint8_t *q = px + ((size_t)y * w + x) * 4;
            uint8_t *o = ind + (size_t)y * w + x;
            if (!q[3]) {
                *o = 0;
                continue;
            }
            if (!err) {
                uint32_t c = pack_px(q), k = (c * 2654435761u) & (hc - 1);
                while (u[hash[k] - 1].c != c)
                    k = (k + 1) & (hc - 1);
                *o = (uint8_t)(u[hash[k] - 1].box + 1);
                continue;
            }
            int v[4];
            for (int c = 0; c < 4; c++) {
                float t = q[c] + e0[(x + 1) * 4 + c];
                v[c] = t < 0 ? 0 : t > 255 ? 255 : (int)(t + 0.5f);
            }
            unsigned mk = (v[0] >> 3) | (v[1] >> 3) << 5 | (v[2] >> 3) << 10 | (v[3] >> 6) << 15;
            if (memo[mk] == 255)
                memo[mk] = (uint8_t)nearest(p0, np, v);
            int bi = memo[mk];
            *o = (uint8_t)(bi + 1);
            for (int c = 0; c < 3; c++) {
                float d = (v[c] - (int)((p0[bi] >> (c * 8)) & 255)) * dither;
                e0[(x + 2) * 4 + c] += d * 7 / 16;
                e1[x * 4 + c] += d * 3 / 16;
                e1[(x + 1) * 4 + c] += d * 5 / 16;
                e1[(x + 2) * 4 + c] += d * 1 / 16;
            }
        }
    }
    free(u);
    free(hash);
    free(memo);
    free(err);
    return np;
}

/* ------------------------------------------------------------------ pages */
typedef struct { int x, y, w; } Seg;
typedef struct { Seg sky[PAGE + 1]; int n; uint8_t *px; } Page;

static void page_init(Page *p)
{
    p->n = 1;
    p->sky[0] = (Seg){0, 0, PAGE};
    p->px = calloc(PAGE * PAGE, 1);
}

static int page_place(Page *p, int rw, int rh, int *ox, int *oy)
{
    int bx = -1, by = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->sky[i].x + rw > PAGE)
            break;
        int top = 0, span = 0, j = i;
        while (span < rw && j < p->n) {
            if (p->sky[j].y > top) top = p->sky[j].y;
            span += p->sky[j].w;
            j++;
        }
        if (span < rw || top + rh > PAGE)
            continue;
        if (bx < 0 || top < by || (top == by && p->sky[i].x < bx)) { bx = p->sky[i].x; by = top; }
    }
    if (bx < 0)
        return 0;
    Seg nw[PAGE + 4];
    int nn = 0;
    for (int i = 0; i < p->n; i++) {
        Seg s = p->sky[i];
        int e = s.x + s.w;
        if (e <= bx || s.x >= bx + rw)
            nw[nn++] = s;
        else {
            if (s.x < bx) nw[nn++] = (Seg){s.x, s.y, bx - s.x};
            if (e > bx + rw) nw[nn++] = (Seg){bx + rw, s.y, e - (bx + rw)};
        }
    }
    nw[nn++] = (Seg){bx, by + rh, rw};
    /* sort by x, then merge equal-height neighbours */
    for (int i = 1; i < nn; i++) {
        Seg t = nw[i];
        int j = i - 1;
        while (j >= 0 && (nw[j].x > t.x || (nw[j].x == t.x && nw[j].y > t.y))) { nw[j + 1] = nw[j]; j--; }
        nw[j + 1] = t;
    }
    int m = 0;
    for (int i = 0; i < nn; i++) {
        if (m && p->sky[m - 1].y == nw[i].y && p->sky[m - 1].x + p->sky[m - 1].w == nw[i].x)
            p->sky[m - 1].w += nw[i].w;
        else
            p->sky[m++] = nw[i];
    }
    p->n = m;
    *ox = bx;
    *oy = by;
    return 1;
}

/* swizzled page bytes: 8-bit 256x256 / 4-bit packed 128x256 (low nibble = left pixel) */
static void page_blob(const Page *p, int bpp, uint8_t *out)
{
    static uint8_t lin[PAGE * PAGE];   /* static: the converter runs on a thread with a small stack */
    int wb = bpp == 8 ? PAGE : PAGE / 2;
    if (bpp == 8)
        memcpy(lin, p->px, PAGE * PAGE);
    else
        for (int y = 0; y < PAGE; y++)
            for (int x = 0; x < wb; x++)
                lin[y * wb + x] = (p->px[y * PAGE + x * 2] & 15) | (p->px[y * PAGE + x * 2 + 1] & 15) << 4;
    /* (H/8, 8, W/16, 16) -> (H/8, W/16, 8, 16) */
    size_t o = 0;
    for (int by = 0; by < PAGE / 8; by++)
        for (int bx = 0; bx < wb / 16; bx++)
            for (int r = 0; r < 8; r++, o += 16)
                memcpy(out + o, lin + (size_t)(by * 8 + r) * wb + bx * 16, 16);
}

typedef struct { uint16_t page; uint8_t u, v; uint16_t w, h; int16_t dx, dy; } Piece;   /* page: idx | 0x8000 for 4-bit */
typedef struct { uint16_t w, h; uint8_t bpp; uint32_t piece0, npieces, pal_off; } SImg;

typedef struct {
    Page *pages[2];     /* 0: 8-bit, 1: 4-bit; open pages */
    int count[2];       /* pages created */
    int first_open[2];  /* index of pages[k][0] */
    int cap[2];
    FILE *spill[2];
    SImg *imgs; int nimg, capimg;
    Piece *pieces; uint32_t npiece, cappiece;
    uint8_t *pal; uint32_t palsize, cappal;
} Packer;

static int pk_place(Packer *pk, int bpp, const uint8_t *ind, int w, int h, int *page, int *u, int *v)
{
    int k = bpp == 8 ? 0 : 1, pw = w + 1 > PAGE ? PAGE : w + 1, ph = h + 1 > PAGE ? PAGE : h + 1, ox = 0, oy = 0, found = -1;
    int lo = pk->count[k] - 4;
    if (lo < pk->first_open[k])
        lo = pk->first_open[k];
    for (int i = lo; i < pk->count[k]; i++)
        if (page_place(&pk->pages[k][i - pk->first_open[k]], pw, ph, &ox, &oy)) { found = i; break; }
    if (found < 0) {
        /* retire pages that leave the 4-page window, then open a new one */
        int keep = pk->count[k] + 1 - 4;
        while (pk->first_open[k] < keep) {
            Page *p = &pk->pages[k][0];
            static uint8_t blob[PAGE * PAGE];
            page_blob(p, bpp, blob);
            if (fwrite(blob, 1, bpp == 8 ? PAGE * PAGE : PAGE * PAGE / 2, pk->spill[k]) == 0)
                return -1;
            free(p->px);
            memmove(pk->pages[k], pk->pages[k] + 1, sizeof(Page) * (pk->count[k] - pk->first_open[k] - 1));
            pk->first_open[k]++;
        }
        if (pk->count[k] - pk->first_open[k] + 1 > pk->cap[k]) {
            pk->cap[k] = pk->cap[k] ? pk->cap[k] * 2 : 8;
            pk->pages[k] = realloc(pk->pages[k], sizeof(Page) * pk->cap[k]);
        }
        page_init(&pk->pages[k][pk->count[k] - pk->first_open[k]]);
        found = pk->count[k]++;
        page_place(&pk->pages[k][found - pk->first_open[k]], pw, ph, &ox, &oy);
    }
    Page *p = &pk->pages[k][found - pk->first_open[k]];
    for (int y = 0; y < h; y++)
        memcpy(p->px + (size_t)(oy + y) * PAGE + ox, ind + (size_t)y * w, w);
    *page = found | (k ? 0x8000 : 0);
    *u = ox;
    *v = oy;
    return 0;
}

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t esz)
{
    if (need <= *cap)
        return p;
    uint32_t c = *cap ? *cap : 256;
    while (c < need)
        c *= 2;
    *cap = c;
    return realloc(p, (size_t)c * esz);
}

/* add one indexed image (trimmed crop at ox,oy of an image sized iw x ih) */
static int pk_add(Packer *pk, const uint8_t *ind, int w, int h, const uint32_t *pal, int bpp, int ox, int oy, int iw, int ih)
{
    if (pk->nimg + 1 > pk->capimg) {
        pk->capimg = pk->capimg ? pk->capimg * 2 : 512;
        pk->imgs = realloc(pk->imgs, sizeof(SImg) * pk->capimg);
    }
    SImg *si = &pk->imgs[pk->nimg];
    si->w = iw; si->h = ih; si->bpp = bpp; si->piece0 = pk->npiece;
    int np = 0;
    for (int py = 0; py < h; py += PAGE)
        for (int px = 0; px < w; px += PAGE) {
            int pw = w - px < PAGE ? w - px : PAGE, ph = h - py < PAGE ? h - py : PAGE;
            uint8_t *part = malloc((size_t)pw * ph);
            for (int y = 0; y < ph; y++)
                memcpy(part + (size_t)y * pw, ind + (size_t)(py + y) * w + px, pw);
            int pg, u, v;
            int r = pk_place(pk, bpp, part, pw, ph, &pg, &u, &v);
            free(part);
            if (r)
                return -1;
            pk->pieces = grow(pk->pieces, &pk->cappiece, pk->npiece + 1, sizeof(Piece));
            pk->pieces[pk->npiece++] = (Piece){(uint16_t)pg, (uint8_t)u, (uint8_t)v, (uint16_t)pw, (uint16_t)ph,
                                               (int16_t)(ox + px), (int16_t)(oy + py)};
            np++;
        }
    si->npieces = np;
    int nent = bpp == 4 ? 16 : 256;
    pk->pal = grow(pk->pal, &pk->cappal, pk->palsize + nent * 4, 1);
    si->pal_off = pk->palsize;
    memcpy(pk->pal + pk->palsize, pal, nent * 4);
    pk->palsize += nent * 4;
    return pk->nimg++;
}

static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int pad64(FILE *f, uint32_t *pos)
{
    static const uint8_t z[64];
    uint32_t n = (64 - (*pos & 63)) & 63;
    if (n && fwrite(z, 1, n, f) != n)
        return -1;
    *pos += n;
    return 0;
}

/* ------------------------------------------------------------------ checkpoint (stop and resume mid-stage) */
#define CKPT_MAGIC 0x54504B43u   /* "CKPT" */

static int ck_w(FILE *f, const void *p, size_t n) { return n && fwrite(p, 1, n, f) != n ? -1 : 0; }
static int ck_r(FILE *f, void *p, size_t n) { return n && fread(p, 1, n, f) != n ? -1 : 0; }

/* everything the packer holds after `done` entries; written beside the spill files, renamed into place */
static int ckpt_save(const char *dir, const Packer *pk, int done, int ne, const CookImage *names)
{
    char p[512], t[512];
    snprintf(p, sizeof p, "%s/SPR.CKPT", dir);
    snprintf(t, sizeof t, "%s/SPR.TMP", dir);
    fflush(pk->spill[0]);
    fflush(pk->spill[1]);
    FILE *f = fopen(t, "wb");
    if (!f)
        return -1;
    uint32_t h[12] = {CKPT_MAGIC, (uint32_t)done, (uint32_t)ne, (uint32_t)pk->nimg, pk->npiece, pk->palsize,
                      (uint32_t)pk->count[0], (uint32_t)pk->count[1], (uint32_t)pk->first_open[0],
                      (uint32_t)pk->first_open[1], (uint32_t)ftell(pk->spill[0]), (uint32_t)ftell(pk->spill[1])};
    int r = ck_w(f, h, sizeof h) | ck_w(f, pk->imgs, sizeof(SImg) * pk->nimg) |
            ck_w(f, pk->pieces, sizeof(Piece) * pk->npiece) | ck_w(f, pk->pal, pk->palsize);
    for (int i = 0; i < done && !r; i++) {
        uint32_t kl = (uint32_t)strlen(names[i].key);
        r |= ck_w(f, &names[i].id, 4) | ck_w(f, &names[i].scale, 4) | ck_w(f, &kl, 4) | ck_w(f, names[i].key, kl);
    }
    for (int k = 0; k < 2 && !r; k++)
        for (int i = pk->first_open[k]; i < pk->count[k]; i++) {
            const Page *pg = &pk->pages[k][i - pk->first_open[k]];
            r |= ck_w(f, &pg->n, sizeof pg->n) | ck_w(f, pg->sky, sizeof(Seg) * pg->n) | ck_w(f, pg->px, PAGE * PAGE);
        }
    if (fclose(f) || r)
        return -1;
    remove(p);
    return rename(t, p);
}

/* -> entries already done (0: start over); restores the packer, the names and the spill file positions */
static int ckpt_load(const char *dir, Packer *pk, int ne, CookImage *names)
{
    char p[512];
    snprintf(p, sizeof p, "%s/SPR.CKPT", dir);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    uint32_t h[12];
    int done = 0;
    if (ck_r(f, h, sizeof h) || h[0] != CKPT_MAGIC || (int)h[2] != ne || (int)h[1] > ne)
        goto bad;
    pk->nimg = pk->capimg = (int)h[3];
    pk->npiece = pk->cappiece = h[4];
    pk->palsize = pk->cappal = h[5];
    pk->imgs = malloc(sizeof(SImg) * (pk->nimg + 1));
    pk->pieces = malloc(sizeof(Piece) * (pk->npiece + 1));
    pk->pal = malloc(pk->palsize + 1);
    if (!pk->imgs || !pk->pieces || !pk->pal || ck_r(f, pk->imgs, sizeof(SImg) * pk->nimg) ||
        ck_r(f, pk->pieces, sizeof(Piece) * pk->npiece) || ck_r(f, pk->pal, pk->palsize))
        goto bad;
    for (uint32_t i = 0; i < h[1]; i++) {
        uint32_t kl;
        if (ck_r(f, &names[i].id, 4) || ck_r(f, &names[i].scale, 4) || ck_r(f, &kl, 4) || kl > 1000)
            goto bad;
        names[i].key = malloc(kl + 1);
        if (ck_r(f, names[i].key, kl))
            goto bad;
        names[i].key[kl] = 0;
    }
    for (int k = 0; k < 2; k++) {
        pk->count[k] = (int)h[6 + k];
        pk->first_open[k] = (int)h[8 + k];
        int open = pk->count[k] - pk->first_open[k];
        pk->cap[k] = open + 8;
        pk->pages[k] = calloc(pk->cap[k], sizeof(Page));
        for (int i = 0; i < open; i++) {
            Page *pg = &pk->pages[k][i];
            pg->px = malloc(PAGE * PAGE);
            if (!pg->px || ck_r(f, &pg->n, sizeof pg->n) || pg->n < 0 || pg->n > PAGE + 1 ||
                ck_r(f, pg->sky, sizeof(Seg) * pg->n) || ck_r(f, pg->px, PAGE * PAGE))
                goto bad;
        }
    }
    for (int k = 0; k < 2; k++)
        if (fseek(pk->spill[k], (long)h[10 + k], SEEK_SET))
            goto bad;
    done = (int)h[1];
    fclose(f);
    return done;
bad:
    fclose(f);
    remove(p);
    return -1;   /* the caller starts over with a fresh packer */
}

/* ------------------------------------------------------------------ driver */
static int key_cmp_bytes(const void *a, const void *b)
{
    return strcmp(((const CookImage *)a)->key, ((const CookImage *)b)->key);
}

int cook_sprites(CookFs *fs, const CookSpriteOpts *o, FILE *out, uint32_t *section_size, CookImage **images,
                 int *nimages, char *err, int errcap)
{
#define FAIL(...) do { snprintf(err, errcap, __VA_ARGS__); ret = -1; goto done; } while (0)
    int ret = 0;
    Packer pk;
    memset(&pk, 0, sizeof pk);
    CookFile **files = 0;
    int nf = cook_fs_list(fs, "graphics/", &files);
    CookImage *names = 0;
    float *scales = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/sp8.tmp", o->tmp_dir);
    char ck[512];
    snprintf(ck, sizeof ck, "%s/SPR.CKPT", o->tmp_dir);
    FILE *probe = fopen(ck, "rb");
    const char *mode = probe ? "r+b" : "w+b";   /* resuming keeps the pages already spilled */
    if (probe)
        fclose(probe);
    pk.spill[0] = cook_fopen(path, mode);
    snprintf(path, sizeof path, "%s/sp4.tmp", o->tmp_dir);
    pk.spill[1] = cook_fopen(path, mode);
    if (!pk.spill[0] || !pk.spill[1])
        FAIL("cannot create the page spill files in %s", o->tmp_dir);

    /* entries: images (name order) then the hue variants */
    typedef struct { CookFile *f; char *key; int hue; const CookImageRule *rule; } Entry;
    Entry *ent = calloc(nf * 2 + o->nhues + 8, sizeof(Entry));
    int ne = 0;
    for (int i = 0; i < nf; i++) {
        const char *k = files[i]->key;
        const char *s1 = strchr(k + 9, '/');
        if (!s1 || strchr(s1 + 1, '/'))
            continue;
        size_t kl = strlen(k);
        const char *ext = strrchr(k, '.');
        if (!ext || ext < s1 || !(!strcmp(ext, ".png") || !strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg") || !strcmp(ext, ".bmp")))
            continue;
        (void)kl;
        char *nk = cook_strdup(k + 9);
        nk[strlen(nk) - strlen(ext)] = 0;
        if (ne && !strcmp(ent[ne - 1].key, nk)) {   /* foo.jpg then foo.png: the later wins */
            free(ent[ne - 1].key);
            ne--;
        }
        ent[ne].f = files[i];
        ent[ne].key = nk;
        ne++;
    }
    int nbase = ne;
    for (int i = 0; i < o->nhues; i++) {
        char hk[300];
        snprintf(hk, sizeof hk, "%s/%s", o->hues[i].folder, o->hues[i].name);
        for (char *c = hk; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c += 32;
        for (int j = 0; j < nbase; j++)
            if (!strcmp(ent[j].key, hk) && o->hues[i].hue) {
                char vk[320];
                snprintf(vk, sizeof vk, "%s@%d", hk, o->hues[i].hue);
                ent[ne] = ent[j];
                ent[ne].key = cook_strdup(vk);
                ent[ne].hue = o->hues[i].hue;
                ne++;
                break;
            }
    }
    names = calloc(ne + 1, sizeof(CookImage));
    scales = calloc(ne + 1, sizeof(float));
    int start = ckpt_load(o->tmp_dir, &pk, ne, names);
    if (start < 0) {
        /* a damaged checkpoint: start the stage over */
        for (int i = 0; i < ne; i++) { free(names[i].key); names[i].key = 0; }
        free(pk.imgs); free(pk.pieces); free(pk.pal);
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < pk.count[k] - pk.first_open[k] && pk.pages[k]; i++) free(pk.pages[k][i].px);
            free(pk.pages[k]);
        }
        FILE *s0 = pk.spill[0], *s1 = pk.spill[1];
        memset(&pk, 0, sizeof pk);
        pk.spill[0] = s0; pk.spill[1] = s1;
        fseek(s0, 0, SEEK_SET);
        fseek(s1, 0, SEEK_SET);
        start = 0;
    }
    for (int e = 0; e < start; e++)
        if (ent[e].key != names[e].key) {   /* the checkpoint's copies replace the fresh keys */
            free(ent[e].key);
            ent[e].key = names[e].key;
        }
    for (int e = start; e < ne; e++) {
        COOK_FEED(ent[e].hue ? "hue" : "image", ent[e].key);
        if (o->progress)
            o->progress(e, ne);
        if (e > start && e % 25 == 0 && ckpt_save(o->tmp_dir, &pk, e, ne, names))
            FAIL("cannot save the checkpoint (memory stick full?)");
        if (cook_yield && cook_yield()) {
            ckpt_save(o->tmp_dir, &pk, e, ne, names);
            snprintf(err, errcap, "stopped");
            ret = -2;
            goto done;
        }
        char folder[64];
        const char *sl = strchr(ent[e].key, '/');
        size_t fl = (size_t)(sl - ent[e].key);
        if (fl >= sizeof folder) fl = sizeof folder - 1;
        memcpy(folder, ent[e].key, fl);
        folder[fl] = 0;
        float scale = 1, dither = 0;
        for (int r = 0; r < o->nrules; r++)
            if (!strcmp(o->rules[r].folder, folder)) { scale = o->rules[r].scale; dither = o->rules[r].dither; }
        Img im;
        if (img_load(fs, ent[e].f, &im))
            FAIL("cannot decode graphics/%s", ent[e].key);
        if (ent[e].hue)
            hue_change(&im, ent[e].hue);
        if (scale != 1.0f) {
            int nw = (int)floorf(im.w * scale + 0.5f), nh = (int)floorf(im.h * scale + 0.5f);
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            if (img_resize(&im, nw, nh))
                FAIL("out of memory resizing %s", ent[e].key);
        }
        /* trim */
        int x0 = im.w, y0 = im.h, x1 = -1, y1 = -1;
        for (int y = 0; y < im.h; y++)
            for (int x = 0; x < im.w; x++)
                if (im.px[((size_t)y * im.w + x) * 4 + 3]) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        uint32_t pal[256] = {0};
        int id;
        if (x1 < 0) {
            uint8_t z = 0;
            id = pk_add(&pk, &z, 1, 1, pal, 4, 0, 0, im.w, im.h);
        } else {
            int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
            uint8_t *crop = malloc((size_t)cw * ch * 4);
            for (int y = 0; y < ch; y++)
                memcpy(crop + (size_t)y * cw * 4, im.px + ((size_t)(y0 + y) * im.w + x0) * 4, (size_t)cw * 4);
            for (size_t p = 0; p < (size_t)cw * ch; p++)
                if (!crop[p * 4 + 3])
                    crop[p * 4] = crop[p * 4 + 1] = crop[p * 4 + 2] = 0;
            uint8_t *ind = malloc((size_t)cw * ch);
            /* exact colours? */
            uint32_t *u = malloc((size_t)cw * ch * 4);
            size_t m = 0;
            for (size_t p = 0; p < (size_t)cw * ch; p++)
                if (crop[p * 4 + 3])
                    u[m++] = pack_px(crop + p * 4);
            qsort(u, m, 4, u32cmp);
            int nu = 0;
            uint32_t uq[256];
            for (size_t p = 0; p < m; p++)
                if (!nu || uq[nu - 1] != u[p]) {
                    if (nu == 255) { nu = 256; break; }
                    uq[nu++] = u[p];
                }
            free(u);
            int bpp;
            if (nu <= 255) {
                bpp = nu <= 15 ? 4 : 8;
                for (int i = 0; i < nu; i++)
                    pal[i + 1] = uq[i];
                for (size_t p = 0; p < (size_t)cw * ch; p++) {
                    if (!crop[p * 4 + 3]) { ind[p] = 0; continue; }
                    uint32_t c = pack_px(crop + p * 4);
                    int lo = 0, hi = nu - 1;
                    while (lo < hi) {
                        int mid = (lo + hi) / 2;
                        if (uq[mid] < c) lo = mid + 1; else hi = mid;
                    }
                    ind[p] = (uint8_t)(lo + 1);
                }
            } else {
                bpp = 8;
                if (quantize(crop, cw, ch, dither, pal, ind) < 0)
                    FAIL("out of memory quantizing %s", ent[e].key);
            }
            id = pk_add(&pk, ind, cw, ch, pal, bpp, x0, y0, im.w, im.h);
            free(ind);
            free(crop);
        }
        if (id < 0)
            FAIL("out of memory or disk full packing %s", ent[e].key);
        names[e].key = ent[e].key;
        names[e].id = id;
        names[e].scale = scale;
        scales[e] = scale;
        img_free(&im);
    }

    /* flush the open pages */
    for (int k = 0; k < 2; k++) {
        int bpp = k ? 4 : 8;
        for (int i = pk.first_open[k]; i < pk.count[k]; i++) {
            static uint8_t blob[PAGE * PAGE];
            page_blob(&pk.pages[k][i - pk.first_open[k]], bpp, blob);
            if (fwrite(blob, 1, bpp == 8 ? PAGE * PAGE : PAGE * PAGE / 2, pk.spill[k]) == 0)
                FAIL("disk full");
            free(pk.pages[k][i - pk.first_open[k]].px);
        }
        pk.first_open[k] = pk.count[k];
    }

    /* the section */
    {
        uint32_t nimg = pk.nimg, npieces = pk.npiece, npages = pk.count[0] + pk.count[1];
        uint32_t pos = 0, offs[8];
        uint8_t hdr[64];
        memset(hdr, 0, sizeof hdr);
        if (fwrite(hdr, 1, 64, out) != 64)
            FAIL("write error");
        pos = 64;
        uint8_t *itab = malloc(nimg * 28 + 1), *ftab = malloc(nimg * 8 + 1), *ptab = malloc(npieces * 12 + 1);
        for (uint32_t i = 0; i < nimg; i++) {
            uint8_t *r = itab + i * 28;
            memset(r, 0, 28);
            put16(r, pk.imgs[i].w); put16(r + 2, pk.imgs[i].h);
            put16(r + 12, pk.imgs[i].w - 1); put16(r + 14, pk.imgs[i].h - 1);
            put16(r + 16, 1); r[18] = pk.imgs[i].bpp; r[19] = 0;
            put32(r + 20, i); put32(r + 24, pk.imgs[i].pal_off);
            uint8_t *f = ftab + i * 8;
            put32(f, pk.imgs[i].piece0); put16(f + 4, pk.imgs[i].npieces); put16(f + 6, 0);
        }
        for (uint32_t i = 0; i < npieces; i++) {
            uint8_t *r = ptab + i * 12;
            Piece *p = &pk.pieces[i];
            unsigned pg = (p->page & 0x8000) ? pk.count[0] + (p->page & 0x7fff) : p->page;
            put16(r, pg); r[2] = p->u; r[3] = p->v; put16(r + 4, p->w); put16(r + 6, p->h);
            put16(r + 8, (uint16_t)p->dx); put16(r + 10, (uint16_t)p->dy);
        }
        uint8_t *gtab = malloc(npages * 8 + 1);
        uint32_t po = 0;
        for (uint32_t i = 0; i < npages; i++) {
            int bpp = i < (uint32_t)pk.count[0] ? 8 : 4;
            put32(gtab + i * 8, po);
            gtab[i * 8 + 4] = bpp; gtab[i * 8 + 5] = gtab[i * 8 + 6] = gtab[i * 8 + 7] = 0;
            po += bpp == 8 ? PAGE * PAGE : PAGE * PAGE / 2;
        }
        struct { uint8_t *d; uint32_t n; } blocks[5] = {{itab, nimg * 28}, {ftab, nimg * 8}, {ptab, npieces * 12},
                                                         {gtab, npages * 8}, {pk.pal, pk.palsize}};
        static const int slot[5] = {0, 1, 2, 3, 6};   /* images, frames, pieces, pages, (fonts, glyphs), pal */
        for (int b = 0; b < 5; b++) {
            if (pad64(out, &pos))
                FAIL("write error");
            if (b == 4) {
                offs[4] = offs[5] = pos;   /* empty fonts and glyphs tables sit before the palettes */
            }
            offs[b < 4 ? slot[b] : 6] = pos;
            if (blocks[b].n && fwrite(blocks[b].d, 1, blocks[b].n, out) != blocks[b].n)
                FAIL("write error");
            pos += blocks[b].n;
        }
        if (pad64(out, &pos))
            FAIL("write error");
        uint32_t page_data = pos;
        for (int k = 0; k < 2; k++) {
            rewind(pk.spill[k]);
            uint8_t buf[8192];
            size_t r;
            while ((r = fread(buf, 1, sizeof buf, pk.spill[k])) > 0) {
                if (fwrite(buf, 1, r, out) != r)
                    FAIL("write error");
                pos += (uint32_t)r;
            }
        }
        free(itab); free(ftab); free(ptab); free(gtab);
        uint32_t palsize = pk.palsize;
        /* header: 'VSPR', then 15 words: images frames pieces pages 0 0 | off images frames pieces pages fonts glyphs pal | palsize page_data */
        put32(hdr + 0, 0x52505356u);
        uint32_t w[15] = {nimg, nimg, npieces, npages, 0, 0, offs[0], offs[1], offs[2], offs[3], offs[4], offs[5], offs[6],
                          palsize, page_data};
        for (int i = 0; i < 15; i++)
            put32(hdr + 4 + i * 4, w[i]);
        long here = ftell(out);
        fseek(out, here - (long)pos, SEEK_SET);
        fwrite(hdr, 1, 64, out);
        fseek(out, here, SEEK_SET);
        *section_size = pos;
    }
    remove(ck);
    qsort(names, ne, sizeof(CookImage), key_cmp_bytes);
    *images = names;
    *nimages = ne;
    names = 0;
done:
    if (names) {
        for (int i = 0; i < ne; i++)
            free(names[i].key);
        free(names);
    }
    free(scales);
    if (pk.spill[0]) fclose(pk.spill[0]);
    if (pk.spill[1]) fclose(pk.spill[1]);
    free(pk.imgs); free(pk.pieces); free(pk.pal);
    free(files);
    return ret;
}

/* the 32 text colours of a VX Ace window skin (8x8 squares at 64, 96), as 0xAABBGGRR */
int cook_windowskin_colors(CookFs *fs, const char *key, uint32_t colors[32])
{
    CookFile *f = cook_fs_find(fs, key);
    if (!f)
        return -1;
    Img im;
    if (img_load(fs, f, &im))
        return -1;
    for (int n = 0; n < 32; n++) {
        int x = 64 + (n % 8) * 8 + 4, y = 96 + (n / 8) * 8 + 4;
        if (x >= im.w || y >= im.h) {
            colors[n] = 0xff000000u;
            continue;
        }
        const uint8_t *q = im.px + ((size_t)y * im.w + x) * 4;
        colors[n] = q[0] | q[1] << 8 | q[2] << 16 | 0xff000000u;
    }
    img_free(&im);
    return 0;
}
