/* VCPE streams (4.5): see vcpe_stream.h.
 *
 * Three parties share each channel:
 * - the game thread asks for a track (vcpe_stream_play) by writing the request and bumping the channel's generation;
 * - the reader thread (io_thread) sees the new generation, empties the ring and reads blocks of the track into it,
 *   segment by segment (intro once, then the loop over and over), marking each block with its track position and the
 *   number of real samples in it;
 * - the mixer thread (mix, through vcpe_audio_mix_hook) decodes the blocks, resamples them to 44.1 kHz with the
 *   pitch (linear interpolation between two decoded frames), applies the gain ramp and adds them to the output.
 * The ring is only touched by the reader (write side) and the mixer (read side) once the mixer has seen the reader's
 * acknowledgement of the generation, the same scheme as vcpe_audio's music. */
#include "vcpe_stream.h"
#include "vcpe_audio.h"
#include "vcpe_sys.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

#define BLOCK 1024
#define BLOCK_BYTES_MAX (2 * (4 + BLOCK / 2))
#define RING 48
#define OUT_RATE 44100

typedef struct {
    uint8_t data[BLOCK_BYTES_MAX];
    uint32_t first;     /* track sample of the block's first sample */
    uint16_t valid;     /* real samples in it */
    uint16_t skip;      /* samples to drop at its start (a resumed position) */
    uint8_t last;       /* the track ends after this block */
} Slot;

typedef struct {
    /* request (game thread) */
    volatile int gen, track;
    volatile uint32_t start;
    /* reader */
    volatile int ack;
    volatile int rd, wr;
    Slot ring[RING];
    /* mixer */
    volatile int playing;           /* the mixer outputs this channel */
    volatile int paused;
    volatile int gain_target;       /* 0..256 */
    volatile int ramp;              /* gain change per output frame in 1/65536 (0: jump) */
    volatile int stop_at_zero;
    volatile uint32_t step;         /* source frames per output frame, 16.16 */
    volatile uint32_t pos;          /* track sample being played */
    int gain_cur;                   /* 16.16 */
    int mix_gen;
    int k;                          /* sample index in the current block */
    int st_pred[2], st_index[2];
    int cur[2], next[2];
    uint32_t frac;
    int primed;
} Chan;

static const VcpeStreamTrack *tracks;
static int ntracks, nchans;
static Chan chans[VCPE_STREAM_MAX];
static volatile int master = 256;
static volatile int running;
static int io_sema = -1, fd = -1, fd_gen;
static char path[256];
int vcpe_stream_underruns;

static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static inline int adpcm(int *pred, int *index, int code)
{
    int step = step_table[*index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    int p = *pred + ((code & 8) ? -delta : delta);
    if (p > 32767) p = 32767;
    else if (p < -32768) p = -32768;
    *pred = p;
    int i = *index + index_table[code];
    *index = i < 0 ? 0 : (i > 88 ? 88 : i);
    return p;
}

static int block_bytes(const VcpeStreamTrack *t) { return t->channels * (4 + BLOCK / 2); }

/* ---------------------------------------------------------------- reader thread */
/* where track sample s is: segment (0 intro, 1 loop) and sample in it */
static void locate(const VcpeStreamTrack *t, uint32_t s, int *seg, uint32_t *in)
{
    if (s < t->intro_samples || !t->loop_samples) {
        *seg = 0;
        *in = s < t->intro_samples ? s : 0;
        return;
    }
    *seg = 1;
    *in = (s - t->intro_samples) % t->loop_samples;
}

static int read_at(uint32_t off, void *dst, int n)
{
    if (fd_gen != vcpe_resume_gen || fd < 0) {
        if (fd >= 0)
            sceIoClose(fd);
        fd_gen = vcpe_resume_gen;
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    }
    if (fd < 0)
        return -1;
    sceIoLseek32(fd, off, PSP_SEEK_SET);
    int r = sceIoRead(fd, dst, n);
    if (r != n) {
        sceIoClose(fd);
        fd = -1;
        return -1;
    }
    return 0;
}

static int io_thread(SceSize args, void *argp)
{
    /* per channel reader cursor */
    int my_gen[VCPE_STREAM_MAX], seg[VCPE_STREAM_MAX], done[VCPE_STREAM_MAX];
    uint32_t blk[VCPE_STREAM_MAX], skip[VCPE_STREAM_MAX];
    (void)args; (void)argp;
    for (int c = 0; c < VCPE_STREAM_MAX; c++)
        my_gen[c] = -1, done[c] = 1;
    while (running) {
        int work = 0;
        for (int c = 0; c < nchans; c++) {
            Chan *ch = &chans[c];
            if (my_gen[c] != ch->gen) {
                my_gen[c] = ch->gen;
                ch->rd = ch->wr = 0;
                done[c] = 1;
                if (ch->track >= 0 && ch->track < ntracks) {
                    const VcpeStreamTrack *t = &tracks[ch->track];
                    uint32_t in;
                    locate(t, ch->start, &seg[c], &in);
                    if (seg[c] == 0 && !t->intro_samples)
                        seg[c] = 1;
                    blk[c] = in / BLOCK;
                    skip[c] = in % BLOCK;
                    done[c] = 0;
                }
                ch->ack = my_gen[c];
            }
            if (done[c])
                continue;
            const VcpeStreamTrack *t = &tracks[ch->track];
            int bb = block_bytes(t);
            /* fill: up to 8 blocks per read, contiguous in the ring and in the segment */
            int space = RING - (ch->wr - ch->rd);
            if (space < 4)
                continue;
            uint32_t seg_samples = seg[c] ? t->loop_samples : t->intro_samples;
            uint32_t seg_off = seg[c] ? t->loop_off : t->intro_off;
            uint32_t seg_blocks = (seg_samples + BLOCK - 1) / BLOCK;
            int n = 8;
            int contig = RING - ch->wr % RING;
            if (n > space) n = space;
            if (n > contig) n = contig;
            if ((uint32_t)n > seg_blocks - blk[c]) n = seg_blocks - blk[c];
            if (n <= 0) {
                done[c] = 1;
                continue;
            }
            /* blocks go to slots, which are larger than a block: read into the slots one by one from one buffer */
            static uint8_t buf[8 * BLOCK_BYTES_MAX];
            if (read_at(seg_off + blk[c] * bb, buf, n * bb) < 0) {
                sceKernelDelayThread(100000);
                work = 1;
                break;
            }
            if (my_gen[c] != ch->gen)
                continue;   /* changed while reading: the next pass restarts it */
            for (int i = 0; i < n; i++) {
                Slot *s = &ch->ring[(ch->wr + i) % RING];
                memcpy(s->data, buf + i * bb, bb);
                uint32_t b = blk[c] + i;
                s->first = (seg[c] ? t->intro_samples : 0) + b * BLOCK;
                uint32_t left = seg_samples - b * BLOCK;
                s->valid = left < BLOCK ? left : BLOCK;
                s->skip = (i == 0) ? skip[c] : 0;
                s->last = 0;
                if (b + 1 == seg_blocks && (seg[c] == 1 ? 0 : !t->loop_samples))
                    s->last = 1;
            }
            skip[c] = 0;
            blk[c] += n;
            ch->wr += n;
            work = 1;
            if (blk[c] >= seg_blocks) {
                if (t->loop_samples) {
                    seg[c] = 1;
                    blk[c] = 0;
                } else {
                    done[c] = 1;
                }
            }
        }
        if (!work)
            sceKernelWaitSemaCB(io_sema, 1, 0);
    }
    return 0;
}

/* ---------------------------------------------------------------- mixer side */
/* decode the next source frame of a channel into ch->next; 0 when no block is there (underrun or the end) */
static int next_frame(Chan *ch, const VcpeStreamTrack *t)
{
    for (;;) {
        if (ch->rd >= ch->wr)
            return 0;
        Slot *s = &ch->ring[ch->rd % RING];
        if (ch->k == 0) {
            for (int c = 0; c < t->channels; c++) {
                ch->st_pred[c] = (int16_t)(s->data[c * 4] | (s->data[c * 4 + 1] << 8));
                ch->st_index[c] = s->data[c * 4 + 2] > 88 ? 88 : s->data[c * 4 + 2];
            }
        }
        if (ch->k >= s->valid) {
            int last = s->last;
            ch->k = 0;
            ch->rd++;
            sceKernelSignalSema(io_sema, 1);
            if (last) {
                ch->playing = 0;
                return 0;
            }
            continue;
        }
        const uint8_t *d = s->data + t->channels * 4;
        int k = ch->k, sh = (k & 1) * 4;
        int l = adpcm(&ch->st_pred[0], &ch->st_index[0], (d[k >> 1] >> sh) & 15);
        int r = l;
        if (t->channels == 2)
            r = adpcm(&ch->st_pred[1], &ch->st_index[1], (d[BLOCK / 2 + (k >> 1)] >> sh) & 15);
        ch->k++;
        ch->pos = s->first + k;
        if (k < s->skip)
            continue;   /* before the resume point: decoded (the decoder must run) but not heard */
        ch->next[0] = l;
        ch->next[1] = r;
        return 1;
    }
}

static void mix(int *lr, int frames)
{
    for (int c = 0; c < nchans; c++) {
        Chan *ch = &chans[c];
        if (!ch->playing || ch->ack != ch->gen || ch->track < 0)
            continue;
        if (ch->mix_gen != ch->gen) {
            ch->mix_gen = ch->gen;
            ch->k = 0;
            ch->frac = 0;
            ch->primed = 0;
        }
        if (ch->paused)
            continue;
        const VcpeStreamTrack *t = &tracks[ch->track];
        if (!ch->primed) {
            if (!next_frame(ch, t))
                continue;   /* the first blocks are still being read */
            ch->cur[0] = ch->next[0];
            ch->cur[1] = ch->next[1];
            if (!next_frame(ch, t)) {
                ch->next[0] = ch->cur[0];
                ch->next[1] = ch->cur[1];
            }
            ch->primed = 1;
        }
        int target = (ch->gain_target * master) >> 8;   /* 0..256 */
        int g = ch->gain_cur, tg = target << 16, ramp = ch->ramp;
        uint32_t step = ch->step;
        for (int i = 0; i < frames; i++) {
            if (g != tg) {
                if (!ramp) g = tg;
                else if (g < tg) { g += ramp; if (g > tg) g = tg; }
                else { g -= ramp; if (g < tg) g = tg; }
            }
            int f = ch->frac >> 8;   /* 0..255 */
            int l = ch->cur[0] + (((ch->next[0] - ch->cur[0]) * f) >> 8);
            int r = ch->cur[1] + (((ch->next[1] - ch->cur[1]) * f) >> 8);
            int gi = g >> 16;
            lr[i * 2] += (l * gi) >> 8;
            lr[i * 2 + 1] += (r * gi) >> 8;
            ch->frac += step;
            while (ch->frac >= 65536) {
                ch->frac -= 65536;
                ch->cur[0] = ch->next[0];
                ch->cur[1] = ch->next[1];
                if (!next_frame(ch, t)) {
                    if (!ch->playing)
                        break;
                    vcpe_stream_underruns++;
                    ch->frac = 0;
                    break;
                }
            }
            if (!ch->playing)
                break;
        }
        ch->gain_cur = g;
        if (ch->stop_at_zero && g == 0 && tg == 0)
            ch->playing = 0;
    }
}

/* ---------------------------------------------------------------- game side */
int vcpe_stream_init(const char *pak, const VcpeStreamTrack *t, int n, int channels)
{
    tracks = t;
    ntracks = n;
    nchans = channels > VCPE_STREAM_MAX ? VCPE_STREAM_MAX : channels;
    snprintf(path, sizeof path, "%s", pak);
    for (int c = 0; c < VCPE_STREAM_MAX; c++) {
        chans[c].track = -1;
        chans[c].gen = 0;
        chans[c].ack = -1;
        chans[c].mix_gen = -1;
    }
    int st = vcpe_step_begin("Opening streams %s", strrchr(pak, '/') ? strrchr(pak, '/') + 1 : pak);
    fd_gen = vcpe_resume_gen;
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    vcpe_step_end(st, fd >= 0);
    if (fd < 0)
        return -1;
    running = 1;
    io_sema = sceKernelCreateSema("stream_io", 0, 0, 256, 0);
    int th = sceKernelCreateThread("stream_io", io_thread, 0x14, 0x4000, PSP_THREAD_ATTR_USER, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);
    vcpe_audio_mix_hook = mix;
    return 0;
}

void vcpe_stream_shutdown(void)
{
    vcpe_audio_mix_hook = 0;
    running = 0;
    if (io_sema >= 0)
        sceKernelSignalSema(io_sema, 1);
    sceKernelDelayThread(50000);
    if (fd >= 0)
        sceIoClose(fd);
    fd = -1;
}

static uint32_t step_for(int track, int pitch)
{
    if (track < 0 || track >= ntracks)
        return 65536;
    if (pitch < 10) pitch = 10;
    if (pitch > 400) pitch = 400;
    return (uint32_t)(((uint64_t)tracks[track].rate * pitch * 65536) / (100 * OUT_RATE));
}

static int ramp_for(int gain, int ms)
{
    if (ms <= 0)
        return 0;
    int frames = ms * (OUT_RATE / 1000);
    int r = ((gain > 0 ? gain : 256) << 16) / (frames > 0 ? frames : 1);
    return r > 0 ? r : 1;
}

void vcpe_stream_play(int c, int track, int gain, int pitch, int fade_ms, uint32_t start)
{
    if (c < 0 || c >= nchans)
        return;
    Chan *ch = &chans[c];
    ch->playing = 0;
    ch->track = (track >= 0 && track < ntracks) ? track : -1;
    ch->start = start;
    ch->step = step_for(track, pitch);
    ch->gain_target = gain;
    ch->ramp = ramp_for(gain, fade_ms);
    ch->gain_cur = fade_ms > 0 ? 0 : gain << 16;
    ch->stop_at_zero = 0;
    ch->paused = 0;
    ch->pos = start;
    ch->gen++;
    ch->playing = ch->track >= 0;
    if (io_sema >= 0)
        sceKernelSignalSema(io_sema, 1);
}

void vcpe_stream_stop(int c, int fade_ms)
{
    if (c < 0 || c >= nchans)
        return;
    Chan *ch = &chans[c];
    if (fade_ms <= 0 || !ch->playing) {
        ch->playing = 0;
        ch->track = -1;
        ch->gen++;
        if (io_sema >= 0)
            sceKernelSignalSema(io_sema, 1);
        return;
    }
    ch->ramp = ramp_for(ch->gain_target, fade_ms);
    ch->gain_target = 0;
    ch->stop_at_zero = 1;
}

void vcpe_stream_gain(int c, int gain)
{
    if (c >= 0 && c < nchans) {
        chans[c].gain_target = gain;
        chans[c].ramp = 0;
    }
}
void vcpe_stream_pitch(int c, int pitch)
{
    if (c >= 0 && c < nchans)
        chans[c].step = step_for(chans[c].track, pitch);
}
void vcpe_stream_pause(int c, int on)
{
    if (c >= 0 && c < nchans)
        chans[c].paused = on;
}
int vcpe_stream_track(int c)
{
    return (c >= 0 && c < nchans && chans[c].playing && !chans[c].stop_at_zero) ? chans[c].track : -1;
}
int vcpe_stream_active(int c) { return c >= 0 && c < nchans && chans[c].playing; }
uint32_t vcpe_stream_pos(int c) { return (c >= 0 && c < nchans) ? chans[c].pos : 0; }
void vcpe_stream_master(int gain) { master = gain; }
