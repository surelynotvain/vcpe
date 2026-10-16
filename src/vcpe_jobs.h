/* VCPE jobs: reservations, clients and servers - background work with automatic priorities.
 *
 * Words used here:
 *   client       someone who needs work done: an engine module ("pack loader") or a game system ("AI", "pathing").
 *                Every client has a name and a priority 0..100. Client 0 is "default" (priority 50).
 *   reservation  one piece of work a client books: a function, its argument, an optional "done" callback and an
 *                urgency. Booking is one line:  vcpe_reserve(client, fn, arg, done, 0);   or  VCPE_RESERVE(fn, arg);
 *   server       a worker thread that hands reservations out: when free, it takes the pending reservation with the
 *                highest effective priority and runs it. vcpe_servers_start(n) starts n servers (up to 4).
 *
 * Automatic priority: effective = client priority + urgency + waiting time (1 point per age_frames frames, default
 * 4). A low-priority client is served later but never forgotten: its reservations climb while they wait.
 *
 * The function runs on a server thread, so it must only touch its own data (decode a file, build a path, unpack
 * a level). The "done" callback runs on the main thread inside vcpe_jobs_update(), in completion order, where it
 * may use the engine freely. With vcpe_ultra linked, each completion also posts VCPE_EV_JOB_DONE.
 *
 * The PSP has one CPU core. Servers run at a lower priority than the game's main thread, so they use the time the
 * main thread spends waiting: the vblank, the GE, file reads (a server blocked on the memory stick lets the game
 * run, and the other way round). That is where background work pays off on a PSP. With 0 servers, vcpe_jobs_update
 * runs reservations itself on the main thread, up to a time budget per frame (tests, or games that want no
 * threads).
 *
 * Example - 3 reservations at the same time for 2 clients, served by 2 servers:
 *     int ai = vcpe_client_add("ai", 70), loader = vcpe_client_add("loader", 30);
 *     vcpe_servers_start(2, 0);
 *     vcpe_reserve(loader, unpack_level, &lvl, level_ready, 0);
 *     vcpe_reserve(ai, plan_path, &enemy[0], path_ready, 0);
 *     vcpe_reserve(ai, plan_path, &enemy[1], path_ready, 10);   // more urgent
 *     every frame: vcpe_jobs_update();
 *   Both servers start at once with the two AI paths (70+10 and 70 beat 30); the level unpack goes to the first
 *   server that frees up. */
#ifndef VCPE_JOBS_H
#define VCPE_JOBS_H
#include <stdint.h>

#define VCPE_MAX_CLIENTS 16
#define VCPE_MAX_RESERVATIONS 64
#define VCPE_MAX_SERVERS 4

typedef void (*VcpeJobFn)(void *arg);
typedef void (*VcpeJobDone)(int reservation, void *arg);

enum { VCPE_RES_NONE, VCPE_RES_PENDING, VCPE_RES_RUNNING, VCPE_RES_DONE };

/* clients */
int vcpe_client_add(const char *name, int priority);   /* returns the client id (an existing name returns its id) */
void vcpe_client_priority(int client, int priority);   /* change at any time: pending reservations follow */
const char *vcpe_client_name(int client);
int vcpe_client_count(void);

/* servers: n worker threads (0 = run on the main thread in vcpe_jobs_update). thread_prio 0 = default (0x30, below
 * the main thread). Call once; vcpe_jobs_shutdown stops them */
int vcpe_servers_start(int n, int thread_prio);
int vcpe_servers_count(void);
void vcpe_jobs_shutdown(void);

/* reservations */
int vcpe_reserve(int client, VcpeJobFn fn, void *arg, VcpeJobDone done, int urgency);   /* id, -1 when full */
#define VCPE_RESERVE(fn, arg) vcpe_reserve(0, (fn), (arg), 0, 0)
int vcpe_reserve_cancel(int id);   /* 1 cancelled, 0 already running or finished */
int vcpe_reserve_state(int id);    /* VCPE_RES_* (NONE once its done callback ran) */
int vcpe_reserve_server(int id);   /* the server running / that ran it (-1 main thread or not yet) */
void vcpe_jobs_wait(int id);       /* block until that reservation finished (lets the servers run meanwhile) */
void vcpe_jobs_wait_all(void);

/* main loop, once per frame: completions (done callbacks), aging; with 0 servers, runs pending work for up to
 * budget_us (vcpe_jobs_budget, default 4000). Returns the number of completions delivered */
int vcpe_jobs_update(void);
void vcpe_jobs_budget(int us);
void vcpe_jobs_age_frames(int frames);   /* waiting frames per priority point (default 4) */

/* stats per client: reservations served and their total run time */
int vcpe_client_served(int client);
int vcpe_client_us(int client);
int vcpe_jobs_pending(void);
int vcpe_jobs_line(char *out, int n);   /* "jobs 2 servers, 1 pending: ai 2 (1.3 ms) loader 1 (4.0 ms)" */

#endif
