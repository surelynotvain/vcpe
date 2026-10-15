/* VCPE coroutines on PSP threads, strictly handed over one at a time (see vcpe_co.h). */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>
#include "vcpe_co.h"

#define MAX_CO 48
#define CO_STACK (32 * 1024)
#ifndef VCPE_CO_POOL
#define VCPE_CO_POOL 8   /* 4.0: threads of the first slots stay parked after their coroutine ends, for reuse */
#endif

typedef struct {
    int used, done;
    SceUID thread, wake;       /* the coroutine sleeps on "wake" until the scheduler hands over; thread 0: none */
    VcpeCoFn fn;
    void *arg;
    double until;              /* resume at this game time (0: next update) */
    int join, gen;             /* waiting for coroutine "join" (id) to end; generation for ids */
} Co;

static Co co[MAX_CO];
static SceUID back;           /* the scheduler sleeps on this until the running coroutine yields */
static int current = -1;
static double now;

static int co_id(int i) { return (co[i].gen << 8) | i; }
static int co_index(int id) { int i = id & 0xFF; return (i < MAX_CO && co[i].used && co[i].gen == (id >> 8)) ? i : -1; }

static int entry(SceSize len, void *argp)
{
    int i = *(int *)argp;
    for (;;) {
        sceKernelWaitSema(co[i].wake, 1, 0);
        co[i].fn(co[i].arg);
        co[i].done = 1;
        current = -1;
        sceKernelSignalSema(back, 1);
        if (i >= VCPE_CO_POOL)
            break;
        /* pooled: park until the slot is given a new coroutine (thread create/delete costs a hitch) */
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

int vcpe_co_start(VcpeCoFn fn, void *arg)
{
    if (!back)
        back = sceKernelCreateSema("vcpe_co_back", 0, 0, 1, 0);
    for (int i = 0; i < MAX_CO; i++) {
        if (co[i].used)
            continue;
        int gen = co[i].gen + 1;
        SceUID thread = co[i].thread, wake = co[i].wake;
        memset(&co[i], 0, sizeof co[i]);
        co[i].gen = gen & 0xFFFFFF;
        co[i].used = 1;
        co[i].fn = fn;
        co[i].arg = arg;
        co[i].join = -1;
        if (thread > 0) {   /* a parked thread waits on its semaphore already */
            co[i].thread = thread;
            co[i].wake = wake;
            return co_id(i);
        }
        co[i].wake = sceKernelCreateSema("vcpe_co", 0, 0, 1, 0);
        co[i].thread = sceKernelCreateThread("vcpe_co", entry, 0x20, CO_STACK, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, 0);
        if (co[i].thread < 0) {
            sceKernelDeleteSema(co[i].wake);
            co[i].thread = co[i].wake = 0;
            co[i].used = 0;
            return -1;
        }
        sceKernelStartThread(co[i].thread, sizeof i, &i);
        return co_id(i);
    }
    return -1;
}

/* a coroutine has ended: its slot is free; a pooled thread stays parked, any other has exited */
static void release(int i)
{
    if (i >= VCPE_CO_POOL) {
        sceKernelDeleteSema(co[i].wake);
        co[i].thread = co[i].wake = 0;
    }
    co[i].used = 0;
}

/* hand the CPU back to the scheduler and sleep until resumed */
static void yield(void)
{
    int me = current;
    current = -1;
    sceKernelSignalSema(back, 1);
    sceKernelWaitSema(co[me].wake, 1, 0);
}

void vcpe_co_wait(float seconds)
{
    if (current < 0)
        return;
    co[current].until = now + (seconds > 0 ? seconds : 0);
    yield();
}

void vcpe_co_wait_frame(void)
{
    if (current < 0)
        return;
    co[current].until = 0;
    yield();
}

void vcpe_co_join(int id)
{
    if (current < 0)
        return;
    while (vcpe_co_alive(id)) {
        co[current].join = id;
        co[current].until = 0;
        yield();
    }
    co[current].join = -1;
}

int vcpe_co_alive(int id) { int i = co_index(id); return i >= 0 && !co[i].done; }

int vcpe_co_running(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CO; i++)
        n += co[i].used && !co[i].done;
    return n;
}

void vcpe_co_update(float dt)
{
    now += dt;
    /* run due coroutines in slot order; ones started during this pass run in the same pass (like Godot calls) */
    for (int i = 0; i < MAX_CO; i++) {
        if (!co[i].used)
            continue;
        if (co[i].done) {
            release(i);
            continue;
        }
        if (co[i].join >= 0 && vcpe_co_alive(co[i].join))
            continue;
        if (co[i].until > now)
            continue;
        current = i;
        sceKernelSignalSema(co[i].wake, 1);
        sceKernelWaitSema(back, 1, 0);
        if (co[i].done)
            release(i);
    }
}

void vcpe_co_reset(void)
{
    for (int i = 0; i < MAX_CO; i++) {
        if (!co[i].used)
            continue;
        if (!co[i].done) {   /* stopped in the middle: that thread can't be reused */
            sceKernelTerminateDeleteThread(co[i].thread);
            sceKernelDeleteSema(co[i].wake);
            co[i].thread = co[i].wake = 0;
            co[i].used = 0;
        } else {
            release(i);
        }
    }
    current = -1;
}

void vcpe_co_shutdown(void)
{
    vcpe_co_reset();
    for (int i = 0; i < MAX_CO; i++)
        if (co[i].thread > 0) {   /* parked on its semaphore */
            sceKernelTerminateDeleteThread(co[i].thread);
            sceKernelDeleteSema(co[i].wake);
            co[i].thread = co[i].wake = 0;
        }
}
