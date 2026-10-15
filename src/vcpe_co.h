/* VCPE coroutines: game logic written like a script with waits ("await a timer", "await another routine").
 * Each coroutine is a PSP thread, but only one runs at a time: the main loop resumes them in a fixed order with
 * vcpe_co_update(), and each runs until it waits or ends. So code needs no locks and runs the same way every
 * time. Waits are in game seconds (scaled by the time scale the game passes to vcpe_co_update). */
#ifndef VCPE_CO_H
#define VCPE_CO_H

typedef void (*VcpeCoFn)(void *arg);

int vcpe_co_start(VcpeCoFn fn, void *arg);   /* starts on the next vcpe_co_update; -1 if none free. Returns an id */
void vcpe_co_wait(float seconds);            /* inside a coroutine: continue after this many game seconds */
void vcpe_co_wait_frame(void);               /* inside a coroutine: continue on the next update */
void vcpe_co_join(int id);                   /* inside a coroutine: continue when that one has ended ("await") */
int vcpe_co_alive(int id);
void vcpe_co_update(float dt);               /* main loop: advance game time by dt, run every coroutine that is due */
int vcpe_co_running(void);                   /* number of live coroutines */
void vcpe_co_reset(void);                    /* main loop only: end every coroutine at once (scene reload) */
/* 4.0: the threads of the first VCPE_CO_POOL slots (default 8, -DVCPE_CO_POOL=n) are kept parked after their
 * coroutine ends and reused by the next vcpe_co_start, so cutscenes do not create and delete threads while playing
 * (32 KB stack each). vcpe_co_shutdown also ends the parked ones and frees their stacks */
void vcpe_co_shutdown(void);

#endif
