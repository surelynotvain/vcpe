/* VCPE system: HOME / sleep callbacks, CPU clock, files next to the EBOOT. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "vcpe_sys.h"
#include "vcpe_errimg.h"

volatile int vcpe_running = 1;
volatile int vcpe_resume_gen;
static char base_dir[256];

static int exit_cb(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    vcpe_running = 0;
    sceKernelExitGame();
    return 0;
}

static int power_cb(int unknown, int flags, void *arg)
{
    (void)unknown; (void)arg;
    if (flags & PSP_POWER_CB_RESUME_COMPLETE)
        vcpe_resume_gen++;   /* file readers reopen their files */
    return 0;
}

static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("exit", exit_cb, 0);
    sceKernelRegisterExitCallback(cb);
    int pcb = sceKernelCreateCallback("power", power_cb, 0);
    if (scePowerRegisterCallback(-1, pcb) < 0)
        scePowerRegisterCallback(0, pcb);
    sceKernelSleepThreadCB();
    return 0;
}

void vcpe_sys_init(int argc, char **argv)
{
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);
    scePowerSetClockFrequency(333, 333, 166);
    base_dir[0] = 0;
    if (argc > 0 && argv[0]) {
        strncpy(base_dir, argv[0], sizeof base_dir - 1);
        char *slash = strrchr(base_dir, '/');
        if (slash)
            slash[1] = 0;
        else
            base_dir[0] = 0;
    }
}

void vcpe_path(char *out, int n, const char *file)
{
    snprintf(out, n, "%s%s", base_dir, file);
}

void vcpe_quit(void) { vcpe_running = 0; }

/* A start-up problem the player has to fix (missing or broken data files): plain text on the debug
 * screen, which needs no game data. Call vcpe_gfx_shutdown() first if the renderer is running.
 * Stays until HOME > Exit. */
void vcpe_fatal(const char *title, const char *text)
{
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF000028);   /* dark red (ABGR), like the old red screen */
    pspDebugScreenClear();
    pspDebugScreenSetTextColor(0xFF5050FF);
    pspDebugScreenPrintf("\n %s\n\n", title);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    /* the debug font is 8 px wide: 60 columns; wrap the text at spaces */
    char line[64];
    const char *p = text;
    while (*p) {
        int n = 0, cut = -1;
        while (p[n] && p[n] != '\n' && n < 58) {
            if (p[n] == ' ')
                cut = n;
            n++;
        }
        if (p[n] && p[n] != '\n' && cut > 0)
            n = cut;
        snprintf(line, sizeof line, "%.*s", n, p);
        pspDebugScreenPrintf(" %s\n", line);
        p += n;
        if (*p == ' ' || *p == '\n')
            p++;
    }
    pspDebugScreenSetTextColor(0xFFA0A0A0);
    pspDebugScreenPrintf("\n Press HOME to quit.\n");
    /* the engine's error picture in the bottom-right corner (tools/mkerrimg.py), with a thin frame */
    void *top = 0; int bw = 0, pf = 0;
    sceDisplayWaitVblankStart();             /* the debug screen's buffer is shown from the next frame */
    sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (top && bw > 0) {
        const int x0 = 480 - 8 - VCPE_ERRIMG_W, y0 = 272 - 8 - VCPE_ERRIMG_H;
        for (int y = -1; y <= VCPE_ERRIMG_H; y++)
            for (int x = -1; x <= VCPE_ERRIMG_W; x++) {
                int edge = x < 0 || y < 0 || x == VCPE_ERRIMG_W || y == VCPE_ERRIMG_H;
                unsigned v = edge ? 0x7A08u : vcpe_errimg[y * VCPE_ERRIMG_W + x];   /* frame: dim red */
                unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                int at = (y0 + y) * bw + x0 + x;
                if (pf == PSP_DISPLAY_PIXEL_FORMAT_8888)
                    ((volatile unsigned *)((unsigned)top | 0x40000000))[at] =
                        0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
                else if (pf == PSP_DISPLAY_PIXEL_FORMAT_565)
                    ((volatile unsigned short *)((unsigned)top | 0x40000000))[at] = (unsigned short)(b << 11 | g << 5 | r);
            }
    }
    while (vcpe_running)
        sceDisplayWaitVblankStart();
    sceKernelExitGame();
}

int vcpe_heap_kb(void)
{
    unsigned lo = 0, hi = 64u << 20;   /* binary search on malloc, 64 KB steps */
    while (hi - lo > 65536) {
        unsigned mid = (lo + hi) / 2;
        void *p = malloc(mid);
        if (p) { free(p); lo = mid; } else hi = mid;
    }
    return (int)(lo / 1024);
}
