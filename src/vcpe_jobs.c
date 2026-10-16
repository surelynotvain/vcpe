/* VCPE jobs: reservations, clients and servers (see vcpe_jobs.h).
 *
 * HOW THIS FILE WORKS
 * Every reservation lives in a fixed slot (no allocation). A slot goes PENDING -> RUNNING (a server took it) ->
 * DONE (the function returned) -> free (the main thread delivered its done callback). One lock protects the slot
 * table; it is held only to pick or update a slot, never while a job runs. A counting semaphore ("work") holds one
 * count per pending reservation, so idle servers sleep in the kernel instead of polling.
 *
 * Picking: a server scans the pending slots and takes the one with the highest effective priority (client
 * priority + urgency + age); ties go to the oldest (lowest sequence number). 64 slots make the scan trivial.
 *
 * The PSP build uses kernel semaphores and threads; other systems (host tests) use pthreads, so the same code is
 * tested on the PC. */
#include <stdio.h>
#include <string.h>
#include "vcpe_jobs.h"
#if defined(__psp__)
#include <pspkernel.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ portability: lock, semaphore, thread, clock */
#if defined(__psp__)
static SceUID lock_id = -1, work_id = -1;
static void lock(void) { sceKernelWaitSema(lock_id, 1, 0); }
static void unlock(void) { sceKernelSignalSema(lock_id, 1); }
static void work_post(int n) { sceKernelSignalSema(work_id, n); }
static void work_wait(void) { sceKernelWaitSema(work_id, 1, 0); }
static uint32_t now_us(void) { return sceKernelGetSystemTimeLow(); }
static void nap(void) { sceKernelDelayThread(200); }
static void sync_init(void)
{
    if (lock_id < 0) lock_id = sceKernelCreateSema("vcpe_jobs_lock", 0, 1, 1, 0);
    if (work_id < 0) work_id = sceKernelCreateSema("vcpe_jobs_work", 0, 0, 1000, 0);
}
#else
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t work_sem;
static int sem_ready;
static void lock(void) { pthread_mutex_lock(&mtx); }
static void unlock(void) { pthread_mutex_unlock(&mtx); }
static void work_post(int n) { while (n-- > 0) sem_post(&work_sem); }
static void work_wait(void) { while (sem_wait(&work_sem) != 0) {} }
static uint32_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
}
static void nap(void) { usleep(200); }
static void sync_init(void) { if (!sem_ready) { sem_init(&work_sem, 0, 0); sem_ready = 1; } }
#endif

/* ------------------------------------------------------------------ tables */
typedef struct { char name[24]; int priority, served; uint32_t us; } Client;
typedef struct {
    volatile int state;          /* VCPE_RES_* */
    int client, urgency, gen, server;
    uint32_t seq, frame;         /* booking order, frame it was booked in (for aging) */
    uint32_t done_seq;           /* completion order, for delivering callbacks in that order */
    VcpeJobFn fn;
    VcpeJobDone done;
    void *arg;
} Res;

static Client clients[VCPE_MAX_CLIENTS] = {{"default", 50, 0, 0}};
static int nclients = 1;
static Res res[VCPE_MAX_RESERVATIONS];
static uint32_t seq_count, done_count;
static volatile uint32_t frame;
static int age_frames = 4, budget_us = 4000;
static int nservers;
static volatile int quitting;
static volatile int live_servers;

/* weak: posts an event when the game links vcpe_ultra */
int vcpe_ev_post(int id, int32_t i, float f, const void *p) __attribute__((weak));
#define EV_JOB_DONE 8   /* VCPE_EV_JOB_DONE in vcpe_ultra.h */

/* ------------------------------------------------------------------ clients */
int vcpe_client_add(const char *name, int priority)
{
    for (int c = 0; c < nclients; c++)
        if (!strncmp(clients[c].name, name, sizeof clients[c].name - 1))
            return c;
    if (nclients >= VCPE_MAX_CLIENTS)
        return -1;
    Client *c = &clients[nclients];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->priority = priority < 0 ? 0 : priority > 100 ? 100 : priority;
    c->served = 0;
    c->us = 0;
    return nclients++;
}

void vcpe_client_priority(int c, int p) { if (c >= 0 && c < nclients) clients[c].priority = p < 0 ? 0 : p > 100 ? 100 : p; }
const char *vcpe_client_name(int c) { return c >= 0 && c < nclients ? clients[c].name : ""; }
int vcpe_client_count(void) { return nclients; }
int vcpe_client_served(int c) { return c >= 0 && c < nclients ? clients[c].served : 0; }
int vcpe_client_us(int c) { return c >= 0 && c < nclients ? (int)clients[c].us : 0; }

/* ------------------------------------------------------------------ picking and running (lock held for picks) */
static int effective(const Res *r)
{
    return clients[r->client].priority + r->urgency + (int)((frame - r->frame) / (uint32_t)age_frames);
}

/* the best pending reservation, or -1 (call with the lock held) */
static int pick(void)
{
    int best = -1, best_p = 0;
    for (int i = 0; i < VCPE_MAX_RESERVATIONS; i++) {
        const Res *r = &res[i];
        if (r->state != VCPE_RES_PENDING)
            continue;
        int p = effective(r);
        if (best < 0 || p > best_p || (p == best_p && r->seq < res[best].seq)) {
            best = i;
            best_p = p;
        }
    }
    return best;
}

/* run slot i (already marked RUNNING by the caller) and mark it DONE */
static void run(int i, int server)
{
    Res *r = &res[i];
    uint32_t t0 = now_us();
    r->fn(r->arg);
    uint32_t dt = now_us() - t0;
    lock();
    clients[r->client].served++;
    clients[r->client].us += dt;
    r->server = server;
    r->done_seq = done_count++;
    r->state = VCPE_RES_DONE;
    unlock();
}

/* a server: sleep until there is work, take the best reservation, run it, repeat */
static void server_loop(int id)
{
    for (;;) {
        work_wait();
        if (quitting)
            break;
        lock();
        int i = pick();
        if (i >= 0) {
            res[i].state = VCPE_RES_RUNNING;
            res[i].server = id;
        }
        unlock();
        if (i >= 0)
            run(i, id);   /* a count with nothing to pick belonged to a cancelled reservation */
    }
    lock();
    live_servers--;
    unlock();
}

#if defined(__psp__)
static int server_thread(SceSize len, void *argp) { server_loop(*(int *)argp); sceKernelExitDeleteThread(0); return 0; }
#else
static void *server_thread(void *arg) { server_loop((int)(intptr_t)arg); return 0; }
#endif

int vcpe_servers_start(int n, int thread_prio)
{
    sync_init();
    if (nservers)
        return nservers;
    if (n > VCPE_MAX_SERVERS) n = VCPE_MAX_SERVERS;
    quitting = 0;
    for (int s = 0; s < n; s++) {
#if defined(__psp__)
        SceUID t = sceKernelCreateThread("vcpe_server", server_thread, thread_prio ? thread_prio : 0x30, 64 * 1024,
                                         PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, 0);
        if (t < 0 || sceKernelStartThread(t, sizeof s, &s) < 0)
            break;
#else
        (void)thread_prio;
        pthread_t t;
        if (pthread_create(&t, 0, server_thread, (void *)(intptr_t)s))
            break;
        pthread_detach(t);
#endif
        nservers++;
        lock();
        live_servers++;
        unlock();
    }
    /* Reservations can be queued while the main-thread budget is zero, before workers are started. Those
     * submissions had no server to signal; wake the newly started workers for the pending batch now. */
    if (nservers) {
        int waiting=0;
        lock();
        for (int i=0;i<VCPE_MAX_RESERVATIONS;i++) waiting+=res[i].state==VCPE_RES_PENDING;
        unlock();
        if (waiting) work_post(waiting);
    }
    return nservers;
}

int vcpe_servers_count(void) { return nservers; }

void vcpe_jobs_shutdown(void)
{
    vcpe_jobs_wait_all();
    quitting = 1;
    work_post(nservers);   /* wake every server so it sees quitting */
    while (live_servers > 0)
        nap();
    nservers = 0;
}

/* ------------------------------------------------------------------ reservations */
static int res_id(int i) { return (res[i].gen << 8) | i; }
static Res *res_of(int id)
{
    int i = id & 0xFF;
    return id >= 0 && i < VCPE_MAX_RESERVATIONS && res[i].gen == (id >> 8) ? &res[i] : 0;
}

int vcpe_reserve(int client, VcpeJobFn fn, void *arg, VcpeJobDone done, int urgency)
{
    if (!fn || client < 0 || client >= nclients)
        return -1;
    sync_init();
    lock();
    int i = 0;
    while (i < VCPE_MAX_RESERVATIONS && res[i].state != VCPE_RES_NONE)
        i++;
    if (i == VCPE_MAX_RESERVATIONS) {
        unlock();
        return -1;
    }
    Res *r = &res[i];
    r->gen = (r->gen + 1) & 0xFFFFFF;
    r->client = client;
    r->urgency = urgency;
    r->fn = fn;
    r->arg = arg;
    r->done = done;
    r->server = -1;
    r->seq = seq_count++;
    r->frame = frame;
    r->state = VCPE_RES_PENDING;
    int id = res_id(i);
    unlock();
    if (nservers)
        work_post(1);
    return id;
}

int vcpe_reserve_cancel(int id)
{
    lock();
    Res *r = res_of(id);
    int ok = r && r->state == VCPE_RES_PENDING;
    if (ok)
        r->state = VCPE_RES_NONE;   /* its work count stays: a server will wake, find nothing and sleep again */
    unlock();
    return ok;
}

int vcpe_reserve_state(int id) { Res *r = res_of(id); return r ? r->state : VCPE_RES_NONE; }
int vcpe_reserve_server(int id) { Res *r = res_of(id); return r ? r->server : -1; }

int vcpe_jobs_pending(void)
{
    int n = 0;
    for (int i = 0; i < VCPE_MAX_RESERVATIONS; i++)
        n += res[i].state == VCPE_RES_PENDING || res[i].state == VCPE_RES_RUNNING;
    return n;
}

void vcpe_jobs_budget(int us) { budget_us = us > 0 ? us : 0; }
void vcpe_jobs_age_frames(int f) { age_frames = f > 0 ? f : 1; }

/* deliver finished reservations in the order they finished */
static int deliver(void)
{
    int n = 0;
    for (;;) {
        lock();
        int best = -1;
        for (int i = 0; i < VCPE_MAX_RESERVATIONS; i++)
            if (res[i].state == VCPE_RES_DONE && (best < 0 || res[i].done_seq < res[best].done_seq))
                best = i;
        if (best < 0) {
            unlock();
            return n;
        }
        Res r = res[best];
        int id = res_id(best);
        res[best].state = VCPE_RES_NONE;   /* free before the callback, which may book again */
        unlock();
        if (r.done)
            r.done(id, r.arg);
        if (vcpe_ev_post)
            vcpe_ev_post(EV_JOB_DONE, id, (float)r.client, r.arg);
        n++;
    }
}

int vcpe_jobs_update(void)
{
    frame++;
    if (!nservers && budget_us) {
        /* no servers: the main thread serves, best first, until the frame's budget is used */
        uint32_t t0 = now_us();
        while (now_us() - t0 < (uint32_t)budget_us) {
            lock();
            int i = pick();
            if (i >= 0)
                res[i].state = VCPE_RES_RUNNING;
            unlock();
            if (i < 0)
                break;
            run(i, -1);
        }
    }
    return deliver();
}

void vcpe_jobs_wait(int id)
{
    for (;;) {
        Res *r = res_of(id);
        if (!r || r->state == VCPE_RES_NONE || r->state == VCPE_RES_DONE)
            break;
        if (!nservers && r->state == VCPE_RES_PENDING) {
            lock();
            int run_it = r->state == VCPE_RES_PENDING;
            if (run_it)
                r->state = VCPE_RES_RUNNING;
            unlock();
            if (run_it)
                run(id & 0xFF, -1);
            continue;
        }
        nap();   /* sleeping lets the lower-priority servers run */
    }
}

void vcpe_jobs_wait_all(void)
{
    for (int i = 0; i < VCPE_MAX_RESERVATIONS; i++)
        if (res[i].state == VCPE_RES_PENDING || res[i].state == VCPE_RES_RUNNING)
            vcpe_jobs_wait(res_id(i));
}

int vcpe_jobs_line(char *out, int n)
{
    int len = snprintf(out, n, "jobs %d servers, %d pending:", nservers, vcpe_jobs_pending());
    for (int c = 0; c < nclients && len < n; c++)
        if (clients[c].served)
            len += snprintf(out + len, n - len, " %s %d (%d.%d ms)", clients[c].name, clients[c].served,
                            (int)(clients[c].us / 1000), (int)(clients[c].us / 100 % 10));
    return len;
}
