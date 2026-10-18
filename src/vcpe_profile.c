#include "vcpe_profile.h"
#include "vcpe.h"
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 768
#define BUILTIN 5
#define METRICS (BUILTIN + VCPE_PROFILE_EXTRA)
static unsigned samples[METRICS][LIMIT], sorted[LIMIT], last_time;
static int count, active, warmup;
static char label[40];
static const char *names[METRICS] = {"cpu", "ge", "work", "interval", "draws"};
static const unsigned *extra[VCPE_PROFILE_EXTRA];

static int compare(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x > y) - (x < y);
}

void vcpe_profile_metric(int slot, const char *name, const unsigned *value)
{
    if (slot < 0 || slot >= VCPE_PROFILE_EXTRA)
        return;
    names[BUILTIN + slot] = value ? name : 0;
    extra[slot] = value;
}

void vcpe_profile_start(const char *name)
{
    snprintf(label, sizeof label, "%s", name);
    count = 0; active = 1; warmup = 2;
    last_time = sceKernelGetSystemTimeLow();
}

void vcpe_profile_frame(void)
{
    unsigned now = sceKernelGetSystemTimeLow(), interval = now - last_time;
    last_time = now;
    if (!active || count == LIMIT)
        return;
    if (warmup) { warmup--; return; }   /* the frames that started/ended the window are not representative */
    unsigned v[BUILTIN] = {vcpe_stat_build_us, vcpe_stat_gpu_wait_us, vcpe_stat_frame_us, interval, vcpe_stat_draws};
    for (int i = 0; i < BUILTIN; i++) samples[i][count] = v[i];
    for (int i = 0; i < VCPE_PROFILE_EXTRA; i++) samples[BUILTIN + i][count] = extra[i] ? *extra[i] : 0;
    count++;
}

void vcpe_profile_end(void)
{
    active = 0;
    printf("PROFILE name=%s count=%d", label, count);
    for (int k = 0; k < METRICS && count; k++) {
        if (!names[k])
            continue;
        unsigned long long sum = 0;
        for (int i = 0; i < count; i++) { sorted[i] = samples[k][i]; sum += sorted[i]; }
        qsort(sorted, count, sizeof *sorted, compare);
        printf(" %s_mean=%u %s_p95=%u %s_max=%u", names[k], (unsigned)(sum / count), names[k],
               sorted[(count - 1) * 95 / 100], names[k], sorted[count - 1]);
    }
    printf("\n");
}
