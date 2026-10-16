/* Host test: run the VCPE mixer (the file given as SRC) over a fixed script of voice starts/stops/pitches/gains and
 * write the PCM it produces. Built once against the old and once against the new vcpe_audio.c; outputs must match. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <pspaudio.h>
#include "vcpe_sys.h"
#include "vcpe_audio.h"
#include "vcpe_pak.h"
volatile int vcpe_running = 1, vcpe_resume_gen;
void (*vcpe_present_hook)(void);
int vcpe_step_begin(const char *fmt, ...) { (void)fmt; return 0; }
void vcpe_step_end(int id, int ok) { (void)id; (void)ok; }
static uint8_t *fake_sfx;
void *vcpe_pak_read_alloc(uint32_t off, uint32_t size) { (void)off; void *p = malloc(size); memcpy(p, fake_sfx, size); return p; }
static SceKernelThreadEntry mixer;
int sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int prio, int stack, int attr, void *opt)
{ (void)prio; (void)stack; (void)attr; (void)opt; if (!strcmp(n, "mixer")) mixer = e; return 1; }
int sceKernelStartThread(int th, SceSize len, void *argp) { (void)th; (void)len; (void)argp; return 0; }
int sceKernelCreateSema(const char *n, int a, int i, int m, void *o) { (void)n; (void)a; (void)i; (void)m; (void)o; return 1; }
int sceAudioChReserve(int ch, int s, int f) { (void)ch; (void)s; (void)f; return 0; }
static jmp_buf done; static int blocks; static FILE *out;
static void script(int b);
int sceAudioOutputPannedBlocking(int ch, int l, int r, void *buf)
{ (void)ch; (void)l; (void)r; fwrite(buf, 4, 1024, out); if (++blocks == 400) longjmp(done, 1); script(blocks); return 0; }
#include SRC
static uint32_t rng = 1234567;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void script(int b)
{
    /* every block: a few random starts / stops / pitch / gain / pause changes */
    for (int k = 0; k < 3; k++) {
        int vi = rnd() % 16, a = rnd() % 10;
        if (a < 4) vcpe_voice_start(vi, rnd() % 6, 0.25f + (rnd() % 400) / 100.0f, rnd() % 3 == 0);
        else if (a == 4) vcpe_voice_stop(vi);
        else if (a == 5) vcpe_voice_pitch(vi, 0.5f + (rnd() % 300) / 100.0f);
        else if (a == 6) vcpe_voice_gain(vi, rnd() % 300);
        else if (a == 7) vcpe_voice_pause(vi, rnd() & 1);
        else if (a == 8) vcpe_sfx_gain(rnd() % 300);
    }
    (void)b;
}
int main(int argc, char **argv)
{
    /* six sounds of different lengths (incl. 1 and 2 samples, block boundaries), random ADPCM blocks */
    static const uint32_t lens[6] = {1, 2, 1023, 1024, 1025, 7000};
    static VcpeSound tab[6];
    uint32_t off = 0;
    for (int i = 0; i < 6; i++) { tab[i].off = off; tab[i].samples = lens[i]; off += ((lens[i] + 1023) / 1024) * (4 + 512); tab[i].size = off - tab[i].off; }
    fake_sfx = malloc(off);
    for (uint32_t i = 0; i < off; i++) fake_sfx[i] = rnd();
    for (int i = 0; i < 6; i++) for (uint32_t b = 0; b < (lens[i] + 1023) / 1024; b++) fake_sfx[tab[i].off + b * 516 + 2] %= 89;
    out = fopen(argv[1], "wb");
    vcpe_audio_init("none", 16, 0, off, tab, 6, tab);
    if (!setjmp(done)) mixer(0, 0);
    fclose(out);
    (void)argc;
    return 0;
}
