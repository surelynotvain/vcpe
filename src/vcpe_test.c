/* VCPE test harness: AUTOTEST.TXT input scripts and frame recording (REC.RAW). */

/* HOW THIS FILE WORKS (the autotest harness)
 *
 * If a file AUTOTEST.TXT sits next to the EBOOT, the game plays itself from that script instead of the pad. The pad
 * module calls vcpe_test_step once per update; the script decides which buttons are "held" for that update: wait N
 * do nothing for N updates press X       hold X for one update          hold X N   hold X for N updates rec N
 * save every Nth shown frame to REC.RAW (480x272 RGBA, appended) snap NAME     save the current screen to
 * SNAPNAME.RAW quit          end the program Games add their own commands through VcpeTestHooks (jump to a scene,
 * check a value, ...).
 *
 * Together with a fixed time step and a fixed random seed in autotest runs, the same script always produces the same
 * frames, byte for byte. That is what the engine's regression tests compare after every change. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vcpe_sys.h"
#include "vcpe_test.h"

int vcpe_autotest;
static VcpeTestHooks hk;
static char *script, *script_pos;
static int script_wait, script_hold_frames;
static unsigned script_hold;
static int rec_every, rec_fd = -1, rec_n;

/* Read the whole script into memory at start-up; no file means a normal run. */
int vcpe_test_load(const VcpeTestHooks *hooks)
{
    char p[300];
    vcpe_path(p, sizeof p, "AUTOTEST.TXT");
    int fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    script = size >= 0 ? malloc(size + 1) : 0;
    if (!script) {
        sceIoClose(fd);
        return 0;
    }
    sceIoRead(fd, script, size);
    script[size] = 0;
    sceIoClose(fd);
    script_pos = script;
    hk = *hooks;
    vcpe_autotest = 1;
    return 1;
}

/* Run the script up to the next command that takes a frame, and report the buttons held for this update. */
void vcpe_test_step(unsigned *held)
{
    *held = 0;
    if (!script)
        return;
    if (vcpe_present_hook)
        vcpe_present_hook();   /* pipeline: the screen shows the last frame, as without it (snapshots, recordings) */
    if (hk.pre)
        hk.pre();
    /* Still inside a "hold" or "wait": nothing new is read from the script. */
    if (script_hold_frames > 0) {
        script_hold_frames--;
        *held = script_hold;
        return;
    }
    if (hk.busy && hk.busy(held))
        return;
    if (script_wait > 0) {
        script_wait--;
        return;
    }
    /* Read lines until a command that takes a frame. Commands like "rec" or game checks run instantly and the next
     * line follows in the same update. */
    while (*script_pos) {
        /* Engine-specific assertions/scripts can exceed the original 63 bytes.
         * Consume the whole line even when oversized: never execute fragments. */
        char line[2048];
        int n = 0;
        int oversized = 0;
        while (*script_pos && *script_pos != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = *script_pos;
            else oversized = 1;
            script_pos++;
        }
        line[n] = 0;
        if (*script_pos == '\n')
            script_pos++;
        if (oversized) {
            fprintf(stderr, "VCPE autotest line exceeds %zu bytes\n", sizeof(line) - 1);
            continue;
        }
        /* Every line is: command, argument word, up to two numbers. '#' starts a comment line. */
        char cmd[16] = {0}, arg[16] = {0};
        int num = 0, num2 = 0;
        int k = sscanf(line, "%15s %15s %d %d", cmd, arg, &num, &num2);
        if (k <= 0 || cmd[0] == '#')
            continue;
        int r = VCPE_CMD_UNKNOWN;
        if (!strcmp(cmd, "wait")) {
            script_wait = atoi(arg) - 1;
            r = VCPE_CMD_FRAME;
        } else if (!strcmp(cmd, "press")) {
            *held = hk.button ? hk.button(arg) : 0;
            r = VCPE_CMD_FRAME;
        } else if (!strcmp(cmd, "hold")) {
            script_hold = hk.button ? hk.button(arg) : 0;
            script_hold_frames = num - 1;
            *held = script_hold;
            r = VCPE_CMD_FRAME;
        } else if (!strcmp(cmd, "quit")) {
            sceKernelExitGame();
            r = VCPE_CMD_FRAME;
        } else if (!strcmp(cmd, "rec")) {
            /* rec N: append every Nth displayed frame (480x272 RGBA) to REC.RAW; rec 0 stops */
            if (rec_fd >= 0) { sceIoClose(rec_fd); rec_fd = -1; }
            rec_every = atoi(arg);
            if (rec_every > 0) {
                char p[300];
                vcpe_path(p, sizeof p, "REC.RAW");
                rec_fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
                rec_n = 0;
            }
            r = VCPE_CMD_NEXT;
        } else if (hk.command) {
            r = hk.command(cmd, arg, num, num2, line, held);
        }
        if (r == VCPE_CMD_UNKNOWN && !strcmp(cmd, "snap")) {
            /* built-in fallback (a game's own "snap" wins): snap NAME -> SNAPNAME.RAW, 480x272 RGBA */
            vcpe_test_snap(arg);
            r = VCPE_CMD_NEXT;
        }
        if (r == VCPE_CMD_FRAME || (r != VCPE_CMD_UNKNOWN && hk.every_command_takes_a_frame))
            return;
    }
}

/* Copy the shown frame buffer row by row (its stride is 512 pixels, the file's 480) through the uncached address, so
 * what is saved is what is in VRAM. */
void vcpe_test_snap(const char *name)
{
    if (vcpe_present_hook)
        vcpe_present_hook();
    void *top; int bw, pf;
    sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE);
    if (!top || bw <= 0 || pf != PSP_DISPLAY_PIXEL_FORMAT_8888)
        return;
    const uint8_t *fb = (const uint8_t *)((uintptr_t)top | 0x40000000);
    char f[64], p[300];
    snprintf(f, sizeof f, "SNAP%s.RAW", name);
    vcpe_path(p, sizeof p, f);
    int fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    for (int y = 0; y < 272; y++)
        sceIoWrite(fd, fb + y * bw * 4, 480 * 4);
    sceIoClose(fd);
}

/* Called after every shown frame: append it to the recording when one is running. */
void vcpe_test_frame_end(void)
{
    if (rec_fd >= 0 && ++rec_n % rec_every == 0) {
        if (vcpe_present_hook)
            vcpe_present_hook();   /* pipeline: show the frame just built before reading the screen */
        void *top; int bw, pf;
        sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE);
        const uint8_t *fb = (const uint8_t *)((uintptr_t)top | 0x40000000);
        for (int y = 0; y < 272; y++)
            sceIoWrite(rec_fd, fb + y * bw * 4, 480 * 4);
    }
}
