/* VCPE coroutines on PSP threads, strictly handed over one at a time (see vcpe_co.h). */

/* HOW THIS FILE WORKS
 *
 * A coroutine is a function that can stop in the middle ("wait 2 seconds") and continue later from the same line. C
 * has no such thing, so each coroutine runs on its own PSP thread, which keeps its place (its stack) while it sleeps.
 * To keep the code as simple as single-threaded code, only one thread ever runs at a time:
 *
 * - every coroutine thread sleeps on its own semaphore, "wake";
 * - vcpe_co_update (main thread) signals a coroutine's wake semaphore, then sleeps on "back";
 * - the coroutine runs until it waits or ends, then signals "back" and sleeps on its "wake" again.
 *
 * So control passes back and forth like a baton, in a fixed slot order, and every run of the game executes the same
 * steps in the same order, which keeps autotest recordings repeatable.
 *
 * Thread pool (4.0): creating and deleting a thread costs time, enough for a hitch in a cutscene. The threads of the
 * first VCPE_CO_POOL slots therefore don't exit when their coroutine ends: they loop and sleep on "wake" until the
 * slot is given a new coroutine. */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>
#include "vcpe_co.h"

/* At most 48 coroutines at once, each with a 32 KB stack. */
#define MAX_CO 48
#define CO_STACK (32 * 1024)
#ifndef VCPE_CO_POOL
#define VCPE_CO_POOL 8   /* 4.0: threads of the first slots stay parked after their coroutine ends, for reuse */
#endif

/* One slot. used = a coroutine owns it; done = its function returned (the slot is freed at the next update). */
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

/* Ids combine the slot and a generation counter, so an id kept after its coroutine ended never refers to a newer
 * coroutine that reuses the slot. */
static int co_id(int i) { return (co[i].gen << 8) | i; }
static int co_index(int id) { int i = id & 0xFF; return (i < MAX_CO && co[i].used && co[i].gen == (id >> 8)) ? i : -1; }

/* Body of every coroutine thread: wait for the baton, run the function, mark the slot done, hand the baton back.
 * Pooled threads then loop to wait for their next coroutine; the others exit and delete themselves. */
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

/* Take the first free slot. If its thread is parked, just give it the new function (it is already waiting on "wake");
 * otherwise create a semaphore and a thread. The coroutine first runs at the next vcpe_co_update. */
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
/* Free a finished slot. Pooled slots keep their parked thread and semaphore; the rest delete the semaphore (their
 * thread has exited). */
static void release(int i)
{
    if (i >= VCPE_CO_POOL) {
        sceKernelDeleteSema(co[i].wake);
        co[i].thread = co[i].wake = 0;
    }
    co[i].used = 0;
}

/* hand the CPU back to the scheduler and sleep until resumed */
/* Inside a coroutine: give the baton back to the main thread and sleep until it is handed over again. */
static void yield(void)
{
    int me = current;
    current = -1;
    sceKernelSignalSema(back, 1);
    sceKernelWaitSema(co[me].wake, 1, 0);
}

/* Continue after this many game seconds (game time advances by the dt given to vcpe_co_update). */
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

/* Wait until another coroutine has ended ("await" in other languages). */
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

/* Main loop, once per update: advance game time, then hand the baton to each due coroutine in slot order. A coroutine
 * started during this pass, in a later slot, runs in the same pass. */
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

/* End every coroutine at once (scene reload). A coroutine stopped mid-function can't be resumed, so its thread is
 * killed; finished ones keep their parked thread. */
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

/* Also end the parked threads, giving their stacks back. */
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
