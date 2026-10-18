/* VCPE cook: audio. OGG (stb_vorbis), MP3 (minimp3) and WAV are decoded in pieces, remixed and resampled to the target
 * rate and channel count (windowed-sinc polyphase), and written as IMA-ADPCM blocks: resident sound effects into one
 * blob (vrpg_assets.sound_effects) and streamed music tracks with an intro and a loop segment
 * (vcpe_stream.py, honouring LOOPSTART / LOOPLENGTH). Big files are read from the card in pieces, never whole. */
#include "vcpe_cook.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_NO_STDIO
#include "../src/ext/minimp3_ex.h"
#define STB_VORBIS_HEADER_ONLY
#include "../src/ext/stb_vorbis.c"

/* ------------------------------------------------------------------ byte source: a FILE or memory */
typedef struct { FILE *f; uint8_t *mem; size_t n, pos; } Rd;

static size_t rd_read(Rd *r, void *p, size_t n)
{
    if (r->f)
        return fread(p, 1, n, r->f);
    if (n > r->n - r->pos)
        n = r->n - r->pos;
    memcpy(p, r->mem + r->pos, n);
    r->pos += n;
    return n;
}

static int rd_seek(Rd *r, long off)
{
    if (r->f)
        return fseek(r->f, off, SEEK_SET);
    if (off < 0 || (size_t)off > r->n)
        return -1;
    r->pos = (size_t)off;
    return 0;
}

static size_t rd_size(Rd *r)
{
    if (r->mem)
        return r->n;
    long c = ftell(r->f);
    fseek(r->f, 0, SEEK_END);
    long e = ftell(r->f);
    fseek(r->f, c, SEEK_SET);
    return (size_t)e;
}

static void rd_close(Rd *r)
{
    if (r->f)
        fclose(r->f);
    free(r->mem);
    memset(r, 0, sizeof *r);
}

static int rd_open(CookFs *fs, CookFile *f, Rd *r)
{
    memset(r, 0, sizeof *r);
    r->f = cook_fs_open_plain(fs, f);
    if (r->f)
        return 0;
    uint32_t n;
    r->mem = cook_fs_read(fs, f, &n);
    r->n = n;
    return r->mem ? 0 : -1;
}

/* ------------------------------------------------------------------ decoders */
typedef struct {
    int kind;            /* 1 ogg, 2 mp3, 3 wav */
    int rate, ch;
    Rd rd;
    stb_vorbis *vb;
    mp3dec_ex_t mp3;
    mp3dec_io_t io;
    int bits;            /* wav */
    uint32_t wav_left;   /* wav bytes left */
    uint32_t loop_start, loop_len;   /* from tags, 0 when absent */
    int has_start, has_len;
    int16_t *tmp;
} Dec;

static size_t mp3_rd(void *buf, size_t size, void *user) { return rd_read(user, buf, size); }
static int mp3_sk(uint64_t pos, void *user) { return rd_seek(user, (long)pos); }

static uint32_t rd_le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void tag_parse(Dec *d, const char *c)
{
    if (!strncasecmp(c, "LOOPSTART=", 10)) { d->loop_start = (uint32_t)strtoul(c + 10, 0, 10); d->has_start = 1; }
    else if (!strncasecmp(c, "LOOPLENGTH=", 11)) { d->loop_len = (uint32_t)strtoul(c + 11, 0, 10); d->has_len = 1; }
}

static int dec_open(CookFs *fs, CookFile *f, Dec *d)
{
    memset(d, 0, sizeof *d);
    if (rd_open(fs, f, &d->rd))
        return -1;
    const char *ext = strrchr(f->key, '.');
    ext = ext ? ext : "";
    if (!strcmp(ext, ".ogg")) {
        int err = 0;
        if (d->rd.f)
            d->vb = stb_vorbis_open_file(d->rd.f, 1, &err, 0);
        else
            d->vb = stb_vorbis_open_memory(d->rd.mem, (int)d->rd.n, &err, 0);
        if (!d->vb)
            return -1;
        if (d->rd.f)
            d->rd.f = 0;   /* stb_vorbis owns (and closes) the FILE now */
        stb_vorbis_info vi = stb_vorbis_get_info(d->vb);
        d->kind = 1;
        d->rate = (int)vi.sample_rate;
        d->ch = vi.channels > 2 ? 2 : vi.channels;
        stb_vorbis_comment vc = stb_vorbis_get_comment(d->vb);
        for (int i = 0; i < vc.comment_list_length; i++)
            tag_parse(d, vc.comment_list[i]);
        return 0;
    }
    if (!strcmp(ext, ".mp3")) {
        d->io.read = mp3_rd;
        d->io.read_data = &d->rd;
        d->io.seek = mp3_sk;
        d->io.seek_data = &d->rd;
        if (mp3dec_ex_open_cb(&d->mp3, &d->io, MP3D_SEEK_TO_BYTE))
            return -1;
        d->kind = 2;
        d->rate = d->mp3.info.hz;
        d->ch = d->mp3.info.channels > 2 ? 2 : d->mp3.info.channels;
        return 0;
    }
    if (!strcmp(ext, ".wav")) {
        uint8_t h[12];
        if (rd_read(&d->rd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
            return -1;
        int fmt = 0;
        for (;;) {
            uint8_t c[8];
            if (rd_read(&d->rd, c, 8) != 8)
                return -1;
            uint32_t n = rd_le(c + 4, 4);
            if (!memcmp(c, "fmt ", 4)) {
                uint8_t b[40] = {0};
                uint32_t k = n < sizeof b ? n : (uint32_t)sizeof b;
                if (rd_read(&d->rd, b, k) != k)
                    return -1;
                if (n > k)
                    rd_seek(&d->rd, (long)(d->rd.f ? ftell(d->rd.f) : (long)d->rd.pos) + (long)(n - k));
                if (n & 1) rd_seek(&d->rd, (long)(d->rd.f ? ftell(d->rd.f) : (long)d->rd.pos) + 1);
                int tag = (int)rd_le(b, 2);
                if (tag == 0xfffe && n >= 26)
                    tag = (int)rd_le(b + 24, 2);
                if (tag != 1)
                    return -1;
                d->ch = (int)rd_le(b + 2, 2);
                d->rate = (int)rd_le(b + 4, 4);
                d->bits = (int)rd_le(b + 14, 2);
                fmt = 1;
            } else if (!memcmp(c, "data", 4)) {
                if (!fmt)
                    return -1;
                size_t left = rd_size(&d->rd) - (size_t)(d->rd.f ? ftell(d->rd.f) : (long)d->rd.pos);
                d->wav_left = n > left ? (uint32_t)left : n;
                break;
            } else {
                long cur = d->rd.f ? ftell(d->rd.f) : (long)d->rd.pos;
                if (rd_seek(&d->rd, cur + (long)n + (long)(n & 1)))
                    return -1;
            }
        }
        if (d->ch < 1 || d->ch > 8 || (d->bits != 8 && d->bits != 16 && d->bits != 24 && d->bits != 32))
            return -1;
        d->kind = 3;
        d->tmp = malloc(sizeof(int16_t) * 4096 * d->ch);
        int wch = d->ch;
        (void)wch;
        return 0;
    }
    return -1;
}

static void dec_close(Dec *d)
{
    if (d->vb)
        stb_vorbis_close(d->vb);
    if (d->kind == 2)
        mp3dec_ex_close(&d->mp3);
    free(d->tmp);
    rd_close(&d->rd);
}

/* up to `frames` interleaved frames of d->ch channels; returns 0 at the end */
static int dec_read(Dec *d, int16_t *out, int frames)
{
    if (d->kind == 1) {
        if (stb_vorbis_get_info(d->vb).channels == d->ch)
            return stb_vorbis_get_samples_short_interleaved(d->vb, d->ch, out, frames * d->ch);
        return 0;
    }
    if (d->kind == 2) {
        size_t n = mp3dec_ex_read(&d->mp3, out, (size_t)frames * d->mp3.info.channels);
        return (int)(n / d->mp3.info.channels);
    }
    int wch = d->ch, bytes = d->bits / 8, got = 0;
    uint8_t buf[4096 * 8 * 4 / 4];
    while (got < frames && d->wav_left) {
        int want = frames - got < 512 ? frames - got : 512;
        uint32_t need = (uint32_t)want * wch * bytes;
        if (need > d->wav_left)
            need = d->wav_left;
        if (need > sizeof buf)
            need = sizeof buf - sizeof buf % (wch * bytes);
        size_t r = rd_read(&d->rd, buf, need);
        r -= r % (wch * bytes);
        if (!r) { d->wav_left = 0; break; }
        d->wav_left -= (uint32_t)r;
        int nf = (int)(r / (wch * bytes));
        for (int i = 0; i < nf * wch; i++) {
            const uint8_t *p = buf + (size_t)i * bytes;
            int v;
            if (bytes == 1) v = ((int)p[0] - 128) << 8;
            else if (bytes == 2) v = (int16_t)(p[0] | p[1] << 8);
            else if (bytes == 3) v = (int16_t)(p[1] | p[2] << 8);
            else v = (int16_t)(p[2] | p[3] << 8);
            out[(size_t)got * wch + i] = (int16_t)v;
        }
        got += nf;
    }
    return got;
}

/* ------------------------------------------------------------------ resampler + remix + encoder */
#define TAPS 16
#define PHASES 256

typedef struct {
    int in_rate, out_rate, in_ch, out_ch;
    int16_t coef[PHASES][TAPS];
    int16_t *hist[2];        /* per out channel, circular over the last HN input frames */
    int hn;                  /* power of two */
    uint64_t in_pos;         /* input frames pushed */
    uint64_t next_num;       /* next output position in input frames, 32.32 fixed */
    uint64_t step;
    int same;                /* rates equal: copy */
    uint64_t out_n;          /* output frames produced */
} Rsmp;

static double bessel0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

static int rs_init(Rsmp *r, int in_rate, int out_rate, int in_ch, int out_ch)
{
    memset(r, 0, sizeof *r);
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->in_ch = in_ch;
    r->out_ch = out_ch;
    r->same = in_rate == out_rate;
    r->hn = 64;
    for (int c = 0; c < out_ch; c++)
        r->hist[c] = calloc(r->hn, sizeof(int16_t));
    r->step = (uint64_t)(((double)in_rate / out_rate) * 4294967296.0);
    double fc = in_rate > out_rate ? 0.94 * out_rate / in_rate : 0.97;   /* cutoff, in input Nyquists */
    const double beta = 8.0, bnorm = bessel0(beta);
    for (int p = 0; p < PHASES; p++) {
        double frac = (double)p / PHASES, sum = 0, w[TAPS];
        for (int k = 0; k < TAPS; k++) {
            double x = (k - (TAPS / 2 - 1)) - frac;        /* input offset from the output position */
            double t = x / (TAPS / 2);
            double win = fabs(t) < 1 ? bessel0(beta * sqrt(1 - t * t)) / bnorm : 0;
            double sx = M_PI * x * fc;
            w[k] = (fabs(sx) < 1e-9 ? 1.0 : sin(sx) / sx) * fc * win;
            sum += w[k];
        }
        int tot = 0;
        for (int k = 0; k < TAPS; k++) {
            int v = (int)floor(w[k] / sum * 32768 + 0.5);
            r->coef[p][k] = (int16_t)v;
            tot += v;
        }
        r->coef[p][TAPS / 2 - 1] += (int16_t)(32768 - tot);
    }
    return 0;
}

static void rs_free(Rsmp *r)
{
    free(r->hist[0]);
    free(r->hist[1]);
}

static int16_t clip16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

/* push input frames (d->ch channels interleaved), produce output frames into out (interleaved out_ch) up to cap;
 * returns the number produced. Call with in = 0 frames and flush = 1 at the end to drain. */
static int rs_push(Rsmp *r, const int16_t *in, int nin, int16_t *out, int cap)
{
    int produced = 0;
    /* feed one input frame at a time into the history, producing outputs whose window is complete */
    for (int i = 0; i < nin; i++) {
        for (int c = 0; c < r->out_ch; c++) {
            int v;
            if (r->in_ch == r->out_ch)
                v = in[i * r->in_ch + c];
            else if (r->out_ch == 1)
                v = (in[i * 2] + in[i * 2 + 1]) / 2;
            else
                v = in[i];
            r->hist[c][r->in_pos & (r->hn - 1)] = (int16_t)v;
        }
        r->in_pos++;
        for (;;) {
            uint64_t base = r->next_num >> 32;           /* input frame at/below the output position */
            if (base + TAPS / 2 >= r->in_pos)
                break;                                   /* needs frames up to base + TAPS/2 */
            if (produced >= cap)
                return produced;
            if (r->same) {
                for (int c = 0; c < r->out_ch; c++)
                    out[produced * r->out_ch + c] = r->hist[c][base & (r->hn - 1)];
            } else {
                int ph = (int)((r->next_num >> 24) & 255);
                const int16_t *cf = r->coef[ph];
                for (int c = 0; c < r->out_ch; c++) {
                    int32_t acc = 0;
                    for (int k = 0; k < TAPS; k++) {
                        int64_t idx = (int64_t)base + k - (TAPS / 2 - 1);
                        int s = idx < 0 ? 0 : r->hist[c][(uint64_t)idx & (r->hn - 1)];
                        acc += s * cf[k];
                    }
                    out[produced * r->out_ch + c] = clip16((acc + 16384) >> 15);
                }
            }
            produced++;
            r->out_n++;
            r->next_num += r->step;
        }
    }
    return produced;
}

/* flush: feed zeros so the last real frames get their windows */
static int rs_drain(Rsmp *r, int16_t *out, int cap)
{
    int16_t z[2 * 8] = {0};
    int total = 0;
    uint64_t real = r->in_pos;
    uint64_t end_num = (uint64_t)ceil((double)real * r->out_rate / r->in_rate);   /* outputs expected overall */
    while (r->out_n < end_num && total < cap) {
        total += rs_push(r, z, 1, out + total * r->out_ch, cap - total);
        if (r->in_pos > real + TAPS * 4 + 8)
            break;
    }
    return total;
}

/* ------------------------------------------------------------------ writer: whole ADPCM blocks into a FILE or memory */
typedef struct {
    FILE *f;
    uint8_t *mem; size_t n, cap;
    CookAdpcm st[2];
    int16_t blk[COOK_ADPCM_BLOCK * 2];
    int nb;              /* frames in the block */
    int ch;
    uint64_t bytes, frames;
} Wr;

static int wr_emit(Wr *w, const uint8_t *p, size_t n)
{
    w->bytes += n;
    if (w->f)
        return fwrite(p, 1, n, w->f) == n ? 0 : -1;
    if (w->n + n > w->cap) {
        size_t c = w->cap ? w->cap * 2 : 1 << 20;
        while (c < w->n + n)
            c *= 2;
        uint8_t *m = realloc(w->mem, c);
        if (!m)
            return -1;
        w->mem = m;
        w->cap = c;
    }
    memcpy(w->mem + w->n, p, n);
    w->n += n;
    return 0;
}

static int wr_flush_block(Wr *w)
{
    if (!w->nb)
        return 0;
    uint8_t out[COOK_ADPCM_BYTES(2)];
    cook_adpcm_block(w->st, w->ch, w->blk, w->nb, out);
    w->frames += w->nb;
    w->nb = 0;
    return wr_emit(w, out, COOK_ADPCM_BYTES(w->ch));
}

static int wr_frames(Wr *w, const int16_t *p, int n)
{
    while (n > 0) {
        int k = COOK_ADPCM_BLOCK - w->nb;
        if (k > n)
            k = n;
        memcpy(w->blk + (size_t)w->nb * w->ch, p, sizeof(int16_t) * k * w->ch);
        w->nb += k;
        p += k * w->ch;
        n -= k;
        if (w->nb == COOK_ADPCM_BLOCK && wr_flush_block(w))
            return -1;
    }
    return 0;
}

/* end a segment: pad to a whole block and restart the predictor state like a fresh encoder run */
static int wr_end_segment(Wr *w)
{
    int r = wr_flush_block(w);
    memset(w->st, 0, sizeof w->st);
    return r;
}

/* decode a file to out rate / channels; the callback sees converted frames in order, 1 = stop early */
typedef int (*FrameSink)(void *u, const int16_t *p, int n);

static int convert_file(CookFs *fs, CookFile *f, int rate, int ch, Dec *d, FrameSink sink, void *u, char *err, int errcap)
{
    (void)fs; (void)f;
    Rsmp r;
    rs_init(&r, d->rate, rate, d->ch, ch);
    int16_t *in = malloc(sizeof(int16_t) * 4096 * 2), *out = malloc(sizeof(int16_t) * 8192 * 2);
    int stop = 0;
    if (!in || !out) {
        snprintf(err, errcap, "out of memory");
        free(in); free(out); rs_free(&r);
        return -1;
    }
    while (!stop) {
        int n = dec_read(d, in, 4096);
        if (n <= 0)
            break;
        int off = 0;
        while (off < n && !stop) {
            int chunk = n - off < 512 ? n - off : 512;
            int p = rs_push(&r, in + (size_t)off * d->ch, chunk, out, 8192);
            if (p && sink(u, out, p))
                stop = 1;
            off += chunk;
        }
        if (cook_yield && cook_yield())
            stop = 2;
    }
    if (!stop) {
        int p = rs_drain(&r, out, 8192);
        if (p)
            sink(u, out, p);
    }
    free(in);
    free(out);
    rs_free(&r);
    return stop == 2 ? -2 : 0;
}

/* ------------------------------------------------------------------ sound effects */
typedef struct { Wr w; } SeCtx;
static int se_sink(void *u, const int16_t *p, int n) { return wr_frames(&((SeCtx *)u)->w, p, n); }

int cook_sounds(CookFs *fs, const char *folder, int rate, uint8_t **blob, uint32_t *size, CookSound **tab, int *n,
                void (*progress)(int, int), char *err, int errcap)
{
    char pre[64];
    snprintf(pre, sizeof pre, "audio/%s/", folder);
    CookFile **files;
    int nf = cook_fs_list(fs, pre, &files);
    Wr w;
    memset(&w, 0, sizeof w);
    w.ch = 1;
    CookSound *t = calloc(nf + 1, sizeof(CookSound));
    int nt = 0, ret = 0;
    for (int i = 0; i < nf; i++) {
        const char *k = files[i]->key + strlen(pre);
        const char *ext = strrchr(k, '.');
        if (!ext || strchr(k, '/') || !(!strcmp(ext, ".ogg") || !strcmp(ext, ".mp3") || !strcmp(ext, ".wav")))
            continue;
        char *nk = cook_strdup(files[i]->key + 6);
        nk[strlen(nk) - strlen(ext)] = 0;
        if (nt && !strcmp(t[nt - 1].key, nk)) {
            free(t[nt - 1].key);
            nt--;
            /* a later file of the same name replaces the earlier one: its data is dropped below by position */
        }
        t[nt].key = nk;
        t[nt].off = 0;
        t[nt].size = 0;
        t[nt].samples = 0;
        nt++;
        (void)progress;
        files[nt - 1] = files[i];   /* keep the winning file at the table's position */
    }
    for (int i = 0; i < nt; i++) {
        COOK_FEED("sound", t[i].key);
        if (progress)
            progress(i, nt);
        Dec d;
        if (dec_open(fs, files[i], &d)) {
            snprintf(err, errcap, "cannot decode %s", t[i].key);
            ret = -1;
            break;
        }
        while (w.n & 3) {
            uint8_t z = 0;
            wr_emit(&w, &z, 1);
        }
        t[i].off = (uint32_t)w.n;
        w.frames = 0;
        memset(w.st, 0, sizeof w.st);
        SeCtx c = {w};
        int rc = convert_file(fs, files[i], rate, 1, &d, se_sink, &c, err, errcap);
        w = c.w;
        dec_close(&d);
        if (rc == -2) {
            snprintf(err, errcap, "stopped");
            ret = -2;
            break;
        }
        if (rc || wr_end_segment(&w)) {
            if (!err[0])
                snprintf(err, errcap, "failed on %s", t[i].key);
            ret = -1;
            break;
        }
        t[i].size = (uint32_t)(w.n - t[i].off);
        /* samples = frames before padding */
        t[i].samples = (uint32_t)w.frames;
    }
    free(files);
    if (ret) {
        free(w.mem);
        for (int i = 0; i < nt; i++)
            free(t[i].key);
        free(t);
        return ret;
    }
    *blob = w.mem;
    *size = (uint32_t)w.n;
    *tab = t;
    *n = nt;
    return 0;
}

/* ------------------------------------------------------------------ streamed tracks */
typedef struct {
    Wr w;
    uint64_t start, end;        /* output frames: intro = [0, start), loop = [start, end) (end 0: to the file's end) */
    uint64_t pos;
    int looped, in_loop;
    uint64_t intro_off, intro_frames, loop_off, loop_frames;
    int has_loop;
} StCtx;

static int st_sink(void *u, const int16_t *p, int n)
{
    StCtx *s = u;
    while (n > 0) {
        if (s->has_loop && !s->in_loop && s->pos >= s->start) {
            s->intro_frames = s->w.frames + s->w.nb;
            if (wr_end_segment(&s->w))
                return -1;
            s->in_loop = 1;
        }
        int k = n;
        if (s->has_loop && !s->in_loop && s->pos + k > s->start)
            k = (int)(s->start - s->pos);
        if (s->has_loop && s->in_loop && s->end && s->pos + k > s->end)
            k = (int)(s->end - s->pos);
        if (k > 0 && wr_frames(&s->w, p, k))
            return -1;
        s->pos += k;
        p += k * s->w.ch;
        n -= k;
        if (s->has_loop && s->in_loop && s->end && s->pos >= s->end)
            return 1;
    }
    return 0;
}

int cook_streams(CookFs *fs, const CookStreamRule *rules, int nrules, FILE *out, uint32_t base, CookTrack **tab,
                 int *n, void (*checkpoint)(void *u, int n), void *u, void (*progress)(int, int), char *err,
                 int errcap)
{
    /* all tracks of the rules' folders, in key order (the same order as the Python tool) */
    typedef struct { CookFile *f; const CookStreamRule *rule; char *key; } Ent;
    Ent *ent = 0;
    int ne = 0, cap = 0;
    CookFile **files;
    for (int r = 0; r < nrules; r++) {
        char pre[64];
        snprintf(pre, sizeof pre, "audio/%s/", rules[r].folder);
        int nf = cook_fs_list(fs, pre, &files);
        for (int i = 0; i < nf; i++) {
            const char *k = files[i]->key + strlen(pre);
            const char *ext = strrchr(k, '.');
            if (!ext || strchr(k, '/') || !(!strcmp(ext, ".ogg") || !strcmp(ext, ".mp3") || !strcmp(ext, ".wav")))
                continue;
            char *nk = cook_strdup(files[i]->key + 6);
            nk[strlen(nk) - strlen(ext)] = 0;
            if (ne && !strcmp(ent[ne - 1].key, nk)) {
                free(ent[ne - 1].key);
                ne--;
            }
            if (ne == cap) {
                cap = cap ? cap * 2 : 256;
                ent = realloc(ent, sizeof(Ent) * cap);
            }
            ent[ne].f = files[i];
            ent[ne].rule = &rules[r];
            ent[ne].key = nk;
            ne++;
        }
        free(files);
    }
    /* sort by key (stable by construction: folders are separate prefixes) */
    for (int i = 1; i < ne; i++) {
        Ent t = ent[i];
        int j = i - 1;
        while (j >= 0 && strcmp(ent[j].key, t.key) > 0) { ent[j + 1] = ent[j]; j--; }
        ent[j + 1] = t;
    }
    int ret = 0;
    CookTrack *t = *tab;
    int nt = *n;
    t = realloc(t, sizeof(CookTrack) * (ne + 1));
    if (!t) {
        snprintf(err, errcap, "out of memory");
        return -1;
    }
    *tab = t;   /* the checkpoint callback may read the table */
    if (nt > ne)
        nt = ne;
    /* the caller positions out at the end of the tracks already kept (a header, or the resume point) */
    for (int i = nt; i < ne && !ret; i++) {
        COOK_FEED("music", ent[i].key);
        if (progress)
            progress(i, ne);
        Dec d;
        if (dec_open(fs, ent[i].f, &d)) {
            snprintf(err, errcap, "cannot decode %s", ent[i].key);
            ret = -1;
            break;
        }
        int rate = ent[i].rule->rate, ch = ent[i].rule->channels;
        StCtx s;
        memset(&s, 0, sizeof s);
        s.w.f = out;
        s.w.ch = ch;
        s.has_loop = ent[i].rule->loop;
        s.start = 0;
        s.end = 0;
        if (s.has_loop && d.has_start) {
            s.start = (uint64_t)((double)d.loop_start * rate / d.rate);
            if (d.has_len)
                s.end = s.start + (uint64_t)((double)d.loop_len * rate / d.rate);
        }
        uint32_t seg0 = base + (uint32_t)ftell(out);
        s.intro_off = seg0;
        int rc = convert_file(fs, ent[i].f, rate, ch, &d, st_sink, &s, err, errcap);
        dec_close(&d);
        if (rc == -2) {
            ret = -2;
            break;
        }
        if (rc) {
            ret = -1;
            break;
        }
        uint32_t loop_off;
        uint32_t ifr, lfr;
        if (!s.has_loop) {
            ifr = (uint32_t)(s.w.frames + s.w.nb);
            wr_end_segment(&s.w);
            loop_off = base + (uint32_t)ftell(out);
            lfr = 0;
        } else if (!s.in_loop) {
            /* the file ended before the loop start: everything is intro, no loop */
            ifr = (uint32_t)(s.w.frames + s.w.nb);
            wr_end_segment(&s.w);
            loop_off = base + (uint32_t)ftell(out);
            lfr = 0;
        } else {
            uint32_t total = (uint32_t)(s.w.frames + s.w.nb);
            wr_end_segment(&s.w);
            ifr = (uint32_t)s.intro_frames;
            lfr = total - ifr;
            /* the loop segment starts after the intro's blocks */
            loop_off = seg0 + (uint32_t)((s.intro_frames + COOK_ADPCM_BLOCK - 1) / COOK_ADPCM_BLOCK) * COOK_ADPCM_BYTES(ch);
        }
        if (s.has_loop && s.in_loop && s.start == 0)
            ifr = 0;
        if (s.has_loop && !s.in_loop && ifr == 0) {
            /* empty track: nothing */
        }
        t[i].key = ent[i].key;
        t[i].intro_off = seg0;
        t[i].intro_samples = ifr;
        t[i].loop_off = loop_off;
        t[i].loop_samples = lfr;
        t[i].rate = (uint16_t)rate;
        t[i].ch = (uint8_t)ch;
        nt = i + 1;
        if (checkpoint) {
            fflush(out);
            checkpoint(u, nt);
        }
    }
    *tab = t;
    *n = nt;
    for (int i = nt; i < ne; i++)
        free(ent[i].key);
    free(ent);
    return ret;
}
