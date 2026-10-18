/* VCPE cook: IMA ADPCM block encoder (the layout adpcm_enc.c writes and vcpe_audio plays) and SHA-256. */
#include "vcpe_cook.h"
#include <string.h>

static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int encode_one(CookAdpcm *s, int sample)
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

/* one block: per channel {int16 predictor, u8 index, u8 0}, then per channel 512 bytes of codes (low nibble first);
 * pcm is interleaved, frames < 1024 are padded with silence */
void cook_adpcm_block(CookAdpcm *st, int ch, const int16_t *pcm, int frames, uint8_t *out)
{
    uint8_t *codes = out + ch * 4;
    for (int c = 0; c < ch; c++) {
        out[c * 4] = (uint8_t)(st[c].pred & 255);
        out[c * 4 + 1] = (uint8_t)((st[c].pred >> 8) & 255);
        out[c * 4 + 2] = (uint8_t)st[c].index;
        out[c * 4 + 3] = 0;
    }
    memset(codes, 0, ch * COOK_ADPCM_BLOCK / 2);
    for (int i = 0; i < COOK_ADPCM_BLOCK; i++)
        for (int c = 0; c < ch; c++) {
            int code = encode_one(&st[c], i < frames ? pcm[i * ch + c] : 0);
            codes[c * (COOK_ADPCM_BLOCK / 2) + (i >> 1)] |= (uint8_t)(code << ((i & 1) * 4));
        }
}

/* ------------------------------------------------------------------ SHA-256 */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(CookSha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void cook_sha256_init(CookSha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                                   0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

void cook_sha256_update(CookSha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        uint32_t k = 64 - s->n;
        if (k > n) k = (uint32_t)n;
        memcpy(s->buf + s->n, p, k);
        s->n += k; p += k; n -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

void cook_sha256_final(CookSha256 *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    cook_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56)
        cook_sha256_update(s, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    cook_sha256_update(s, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = s->h[i] >> 24; out[i * 4 + 1] = s->h[i] >> 16; out[i * 4 + 2] = s->h[i] >> 8; out[i * 4 + 3] = s->h[i];
    }
}
