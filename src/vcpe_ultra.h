/* VCPE ultra tools: fast communication between the engine and the game, plus the helpers every game rewrites.
 *
 * 1. Event bus. The engine and the game post small events ({id, int, float, pointer}); listeners subscribe by id.
 *    Events are queued and delivered in order by vcpe_ev_dispatch(), once per frame on the main thread, so a
 *    listener never runs in the middle of someone else's code. No allocation: fixed queues and listener tables.
 *    vcpe_ultra_update() (once per frame) posts the engine's own events: resume after sleep, loading screen
 *    begin/end, a late frame (missed vblank), and low memory when a threshold is set.
 *    Other threads (audio, a loader) post with vcpe_ev_post_thread: a lock-free single-producer ring, drained at
 *    the next dispatch.
 *
 * 2. Blackboard. Named values (int, float, short string) that the engine, the game and test scripts read and
 *    write by name, e.g. "player.hp" or "level". Names are hashed once; a change posts VCPE_EV_BB_CHANGED with the
 *    name's hash in .i. vcpe_bb_dump writes every value to a log line callback.
 *
 * 3. Live tuning. The game registers its tweakable variables (vcpe_tune_float("player.speed", &speed, 0, 10)).
 *    A text file (default TUNE.TXT next to the EBOOT, "name = value" per line, '#' comments) is checked every
 *    poll_frames frames and re-read when its size or time changed, so editing it on the PC while the game runs
 *    from PSPLink's host0: (or copying it to the stick) changes the values without a rebuild. Values are clamped
 *    to their range; each change posts VCPE_EV_TUNE_CHANGED. vcpe_tune_save writes the current values back.
 *
 * 4. Toolkit: FNV-1a hashing, 16.16 fixed point, a ring buffer, an O(1) object pool, countdown timers and a
 *    profiler with named scopes (per-frame microseconds, averaged).
 *
 * Autotest commands (chain vcpe_ultra_command before the game's own): "tune NAME VALUE", "bb NAME VALUE",
 * "bbcheck NAME VALUE" (logs PASS/FAIL), "ev ID INT" (post an event).
 * Pure C with stdio: only the clock and the engine hooks are PSP-specific, so host tests use the same code.
 * Games that do not link vcpe_ultra are unchanged. */
#ifndef VCPE_ULTRA_H
#define VCPE_ULTRA_H
#include <stdint.h>

/* ------------------------------------------------------------------ 1. event bus */
typedef struct {
    uint16_t id;        /* VCPE_EV_* (engine, below 256) or VCPE_EV_GAME + n */
    uint16_t flags;
    int32_t i;          /* payload: meaning depends on the id */
    float f;
    const void *p;      /* must stay valid until the event is dispatched */
} VcpeEvent;

enum {
    VCPE_EV_ANY = 0,            /* subscribe to every event */
    VCPE_EV_RESUME = 1,         /* the console woke from sleep (i = resume count) */
    VCPE_EV_LOAD_BEGIN = 2,     /* a loading screen started */
    VCPE_EV_LOAD_END = 3,       /* it ended (i = frames it drew) */
    VCPE_EV_LATE_FRAME = 4,     /* a frame missed its vblank (i = vblanks it took) */
    VCPE_EV_LOW_MEMORY = 5,     /* largest free block fell below the threshold (i = KB) */
    VCPE_EV_BB_CHANGED = 6,     /* a blackboard value changed (i = name hash) */
    VCPE_EV_TUNE_CHANGED = 7,   /* a tuned variable changed (i = name hash, p = the variable) */
    VCPE_EV_JOB_DONE = 8,       /* a vcpe_jobs reservation finished (i = its id, f = client, p = its argument) */
    VCPE_EV_GAME = 256          /* first id for the game's own events */
};

typedef void (*VcpeEvFn)(const VcpeEvent *ev, void *user);

int vcpe_ev_on(int id, VcpeEvFn fn, void *user);        /* returns a handle for vcpe_ev_off, -1 when full */
void vcpe_ev_off(int handle);
int vcpe_ev_post(int id, int32_t i, float f, const void *p);          /* main thread; 0 ok, -1 queue full */
int vcpe_ev_post_thread(int id, int32_t i, float f, const void *p);   /* one other thread at a time */
int vcpe_ev_dispatch(void);   /* deliver everything queued so far; returns the count. Events posted by listeners
                               * during the dispatch are delivered at the next one */
extern int vcpe_ev_dropped;   /* events lost because a queue was full */

/* once per frame, after vcpe_gfx_end: engine events, live-tuning poll, profiler frame, then vcpe_ev_dispatch */
void vcpe_ultra_update(void);
void vcpe_ultra_low_memory_kb(int kb);   /* post VCPE_EV_LOW_MEMORY below this (checked twice a second; 0 off) */

/* ------------------------------------------------------------------ 2. blackboard */
enum { VCPE_BB_NONE, VCPE_BB_INT, VCPE_BB_FLOAT, VCPE_BB_STR };
void vcpe_bb_set_int(const char *name, int32_t v);
void vcpe_bb_set_float(const char *name, float v);
void vcpe_bb_set_str(const char *name, const char *v);   /* up to 31 bytes are kept */
int32_t vcpe_bb_int(const char *name, int32_t def);     /* a float value reads truncated */
float vcpe_bb_float(const char *name, float def);
const char *vcpe_bb_str(const char *name, const char *def);
int vcpe_bb_type(const char *name);                     /* VCPE_BB_NONE when not set */
int vcpe_bb_count(void);
void vcpe_bb_clear(void);
void vcpe_bb_dump(void (*line)(const char *text));      /* one "name = value" line per entry */

/* ------------------------------------------------------------------ 3. live tuning */
int vcpe_tune_float(const char *name, float *var, float lo, float hi);   /* 0 ok, -1 table full */
int vcpe_tune_int(const char *name, int *var, int lo, int hi);
void vcpe_tune_file(const char *path, int poll_frames);   /* default: TUNE.TXT next to the EBOOT, every 30 frames;
                                                           * a path with ':' is used as is (host0:/tune.txt) */
int vcpe_tune_reload(void);    /* read the file now; returns the number of values changed, -1 no file */
int vcpe_tune_set(const char *name, const char *value);   /* one "name = value" by hand; 1 changed, 0 same, -1 unknown */
int vcpe_tune_save(void);      /* write every tuned value to the file; 0 ok */

/* ------------------------------------------------------------------ 4. toolkit */
/* FNV-1a, 32-bit: names to ids (the converter can compute the same hashes) */
static inline uint32_t vcpe_fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* 16.16 fixed point: deterministic arithmetic that is identical on every machine (replays, lockstep) */
typedef int32_t vcpe_fx;
#define VCPE_FX_ONE 65536
static inline vcpe_fx vcpe_fx_from(float v) { return (vcpe_fx)(v * 65536.0f); }
static inline float vcpe_fx_to(vcpe_fx v) { return v * (1.0f / 65536.0f); }
static inline vcpe_fx vcpe_fx_mul(vcpe_fx a, vcpe_fx b) { return (vcpe_fx)(((int64_t)a * b) >> 16); }
static inline vcpe_fx vcpe_fx_div(vcpe_fx a, vcpe_fx b) { return b ? (vcpe_fx)(((int64_t)a << 16) / b) : 0; }

/* ring buffer of fixed-size elements over memory the caller gives; cap must be a power of two */
typedef struct { uint8_t *mem; uint32_t elem, cap, head, tail; } VcpeRing;
void vcpe_ring_init(VcpeRing *r, void *mem, uint32_t elem, uint32_t cap);
int vcpe_ring_push(VcpeRing *r, const void *item);   /* 0 ok, -1 full */
int vcpe_ring_pop(VcpeRing *r, void *item);          /* 0 ok, -1 empty */
static inline uint32_t vcpe_ring_count(const VcpeRing *r) { return r->head - r->tail; }

/* object pool: fixed-size blocks carved from one memory area, O(1) alloc and free, no fragmentation */
typedef struct { uint8_t *mem; uint32_t size, count, used; void *free_list; } VcpePool;
int vcpe_pool_init(VcpePool *p, void *mem, uint32_t block_size, uint32_t count);   /* block_size >= pointer size */
void *vcpe_pool_alloc(VcpePool *p);   /* 0 when empty */
void vcpe_pool_free(VcpePool *p, void *block);

/* countdown timers in seconds: vcpe_timer_tick returns 1 on the frame it runs out (and restarts if repeating) */
typedef struct { float left, period; int repeat; } VcpeTimer;
static inline void vcpe_timer_start(VcpeTimer *t, float s, int repeat) { t->left = t->period = s; t->repeat = repeat; }
static inline int vcpe_timer_tick(VcpeTimer *t, float dt)
{
    if (t->left <= 0)
        return 0;
    t->left -= dt;
    if (t->left > 1e-5f)   /* float steps (60 x 1/60) leave a few millionths: count those as done */
        return 0;
    if (t->repeat && t->period > 0)
        t->left += t->period;   /* keep the remainder, so repeats don't drift */
    return 1;
}

/* profiler: up to 16 named scopes; times add up during a frame and are averaged over 30 frames */
#define VCPE_PROF_MAX 16
int vcpe_prof_id(const char *name);   /* register once, keep the id */
void vcpe_prof_begin(int id);
void vcpe_prof_end(int id);
int vcpe_prof_us(int id);             /* averaged microseconds per frame */
int vcpe_prof_line(char *out, int n); /* "prof update 3.1 ai 0.8 draw 4.0 ms" */

/* autotest: chain before the game's command hook (same signature as VcpeTestHooks.command) */
int vcpe_ultra_command(const char *cmd, const char *arg, int num, int num2, const char *line, unsigned *held);

#endif
