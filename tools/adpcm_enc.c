/* IMA ADPCM block encoder: s16le interleaved PCM on stdin -> blocks on stdout.
 * Block = for each channel {int16 predictor, uint8 step index, uint8 0},
 *         then for each channel ADPCM_BLOCK/2 bytes (low nibble first).
 * Decoder: src/vcpe_audio.c (same constants). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ADPCM_BLOCK 1024

static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

typedef struct { int pred, index; } State;

/* Encode one sample: the 4-bit code says how far the sample is from the prediction, in steps of the current step size
 * (bit 3 = sign, bits 2-0 = step, step/2, step/4). The encoder then updates its prediction exactly as the decoder
 * will (same arithmetic, same clamping), so both stay in sync. */
static int encode_one(State *s, int sample)
{
    int step = step_table[s->index];
    int diff = sample - s->pred;
    int code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; delta += step; }
    if (code & 8) s->pred -= delta; else s->pred += delta;
    if (s->pred > 32767) s->pred = 32767;
    if (s->pred < -32768) s->pred = -32768;
    s->index += index_table[code];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
    return code;
}

/* Read all PCM from stdin (growing the buffer), then write blocks of 1024 samples per channel. */
int main(int argc, char **argv)
{
    int ch = argc > 1 ? atoi(argv[1]) : 1;
    size_t cap = 1 << 20, len = 0;
    int16_t *pcm = malloc(cap);
    size_t r;
    while ((r = fread((char *)pcm + len, 1, cap - len, stdin)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; pcm = realloc(pcm, cap); }
    }
    size_t frames = len / (2 * ch);
    State st[2] = {{0, 0}, {0, 0}};
    uint8_t nib[2][ADPCM_BLOCK / 2];
    for (size_t f0 = 0; f0 < frames; f0 += ADPCM_BLOCK) {
        for (int c = 0; c < ch; c++) {
            /* Block header per channel: the predictor and step index at the start of the block, so the decoder can
             * start at any block. */
            int16_t p = (int16_t)st[c].pred;
            fwrite(&p, 2, 1, stdout);
            uint8_t hdr[2] = {(uint8_t)st[c].index, 0};
            fwrite(hdr, 1, 2, stdout);
        }
        memset(nib, 0, sizeof nib);
        for (int i = 0; i < ADPCM_BLOCK; i++) {
            for (int c = 0; c < ch; c++) {
                size_t f = f0 + i;
                int sample = f < frames ? pcm[f * ch + c] : 0;
                int code = encode_one(&st[c], sample);
                nib[c][i >> 1] |= (uint8_t)(code << ((i & 1) * 4));
            }
        }
        for (int c = 0; c < ch; c++)
            fwrite(nib[c], 1, ADPCM_BLOCK / 2, stdout);
    }
    return 0;
}
