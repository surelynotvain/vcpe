/* VCPE system: HOME / sleep callbacks, CPU clock, files next to the EBOOT. */
#ifndef VCPE_SYS_H
#define VCPE_SYS_H

extern volatile int vcpe_running;      /* cleared by HOME > Exit or vcpe_quit() */
extern volatile int vcpe_resume_gen;   /* bumped after every sleep/resume: file readers reopen their files */

void vcpe_sys_init(int argc, char **argv);              /* callbacks thread, 333 MHz, game folder */
void vcpe_path(char *out, int n, const char *file);    /* "ms0:/PSP/GAME/<game>/" + file */
void vcpe_quit(void);
/* set by vcpe_gfx_pipeline(1): shows the frame handed to the GE (autotest reads, fatal screens). 0 when off */
extern void (*vcpe_present_hook)(void);
void vcpe_fatal(const char *title, const char *text);   /* readable start-up error, never returns */
/* largest block malloc can give right now, in KB. A game built with PSP_LARGE_MEMORY gets about 50 MB on a
 * PSP-2000/3000/Street/Go and about 20 MB on a PSP-1000, so this tells a 32 MB console from a 64 MB one */
int vcpe_heap_kb(void);
/* start-up checks with the standard player-facing messages (vcpe_fatal when they fail):
 * the 64 MB models (PSP-2000/3000/Go) - a PSP-1000 has ~20 MB for homebrew */
void vcpe_require_64mb(const char *title);
/* a file next to the EBOOT; what = how the player gets it ("Build it with tools/pack.py from your own copy...") */
void vcpe_require_file(const char *file, const char *title, const char *what);

#endif
