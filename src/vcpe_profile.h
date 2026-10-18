/* VCPE profile: buffered measurement windows, no file I/O inside measured frames. Start a named window,
 * call vcpe_profile_frame once per frame, end it to print mean/p95/max of every metric to stdout (the
 * emulator/test log). Built-in metrics: cpu, ge, work, interval (us) and draws; a game adds up to 8 more. */
#ifndef VCPE_PROFILE_H
#define VCPE_PROFILE_H
#define VCPE_PROFILE_EXTRA 8
void vcpe_profile_metric(int slot, const char *name, const unsigned *value);   /* value read every frame */
void vcpe_profile_start(const char *label);
void vcpe_profile_frame(void);
void vcpe_profile_end(void);
#endif
