/* VCPE ultra tools (see vcpe_ultra.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "vcpe_ultra.h"
#include "vcpe_test.h"
#if defined(__psp__)
#include <pspkernel.h>
#else
#include <time.h>
#endif

/* Engine state this module watches. Weak: a game (or a host test) that doesn't link a module simply has no such
 * symbol, its address is 0, and that check is skipped. */
extern volatile int vcpe_resume_gen __attribute__((weak));
extern int vcpe_stat_vblank_misses __attribute__((weak)), vcpe_stat_vblanks __attribute__((weak));
int vcpe_loading_active(void) __attribute__((weak));
int vcpe_loading_shown(void) __attribute__((weak));
int vcpe_heap_kb(void) __attribute__((weak));
void vcpe_path(char *out, int n, const char *file) __attribute__((weak));
void vcpe_log(const char *fmt, ...) __attribute__((weak));

/* microseconds from an arbitrary start (wraps after ~71 minutes; only differences are used) */
static uint32_t now_us(void)
{
#if defined(__psp__)
    return sceKernelGetSystemTimeLow();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
#endif
}

/* ================================================================== 1. event bus */
/* The main queue is a ring of QN events (power of two): head counts posts, tail counts deliveries, the slot is the
 * count modulo QN. Only the main thread touches it. */
#define QN 256
static VcpeEvent queue[QN];
static uint32_t q_head, q_tail;
int vcpe_ev_dropped;

/* The thread ring: one producer thread writes slots and then advances t_head; the main thread reads and then
 * advances t_tail. Each side only writes its own counter, so no lock is needed; the barrier makes sure the slot's
 * contents are in memory before the other side sees the new counter. */
#define TN 64
static VcpeEvent tring[TN];
static volatile uint32_t t_head, t_tail;

#define MAX_LISTENERS 64
static struct { int id; VcpeEvFn fn; void *user; } listeners[MAX_LISTENERS];

int vcpe_ev_on(int id, VcpeEvFn fn, void *user)
{
    for (int h = 0; h < MAX_LISTENERS; h++)
        if (!listeners[h].fn) {
            listeners[h].id = id;
            listeners[h].fn = fn;
            listeners[h].user = user;
            return h;
        }
    return -1;
}

void vcpe_ev_off(int h)
{
    if (h >= 0 && h < MAX_LISTENERS)
        listeners[h].fn = 0;
}

static void fill(VcpeEvent *e, int id, int32_t i, float f, const void *p)
{
    e->id = (uint16_t)id;
    e->flags = 0;
    e->i = i;
    e->f = f;
    e->p = p;
}

int vcpe_ev_post(int id, int32_t i, float f, const void *p)
{
    if (q_head - q_tail >= QN) {
        vcpe_ev_dropped++;
        return -1;
    }
    fill(&queue[q_head % QN], id, i, f, p);
    q_head++;
    return 0;
}

int vcpe_ev_post_thread(int id, int32_t i, float f, const void *p)
{
    uint32_t h = t_head;
    if (h - t_tail >= TN) {
        vcpe_ev_dropped++;
        return -1;
    }
    fill(&tring[h % TN], id, i, f, p);
    __sync_synchronize();   /* the slot is written before the new head is visible */
    t_head = h + 1;
    return 0;
}

int vcpe_ev_dispatch(void)
{
    /* move what other threads posted into the main queue, in their order */
    while (t_tail != t_head) {
        __sync_synchronize();   /* read the slot only after seeing the head that covers it */
        const VcpeEvent *e = &tring[t_tail % TN];
        vcpe_ev_post(e->id, e->i, e->f, e->p);
        t_tail = t_tail + 1;
    }
    /* deliver only what was queued before this point: listeners may post more, which waits for the next dispatch
     * (an event that re-posts itself can't loop forever) */
    uint32_t end = q_head;
    int n = 0;
    while (q_tail != end) {
        VcpeEvent e = queue[q_tail % QN];   /* a copy: the slot may be reused by a post from a listener */
        q_tail++;
        for (int h = 0; h < MAX_LISTENERS; h++)
            if (listeners[h].fn && (listeners[h].id == VCPE_EV_ANY || listeners[h].id == e.id))
                listeners[h].fn(&e, listeners[h].user);
        n++;
    }
    return n;
}

/* ================================================================== 2. blackboard */
/* Open-addressing hash table keyed by the FNV hash of the name. The name itself is kept (truncated) for dumps and to
 * tell apart two names with the same hash. */
#define BB_N 256
typedef struct {
    uint32_t key;    /* 0 = empty slot */
    uint8_t type;
    char name[32];
    union { int32_t i; float f; char s[32]; } v;
} BbEntry;
static BbEntry bb[BB_N];
static int bb_n;

static uint32_t key_of(const char *name)
{
    uint32_t k = vcpe_fnv(name);
    return k ? k : 1;   /* 0 marks empty slots */
}

static BbEntry *bb_find(const char *name, int create)
{
    uint32_t k = key_of(name);
    for (int i = 0; i < BB_N; i++) {
        BbEntry *e = &bb[(k + i) % BB_N];
        if (e->key == k && !strncmp(e->name, name, sizeof e->name - 1))
            return e;
        if (!e->key) {
            if (!create)
                return 0;
            memset(e, 0, sizeof *e);
            e->key = k;
            snprintf(e->name, sizeof e->name, "%s", name);
            bb_n++;
            return e;
        }
    }
    return 0;   /* table full */
}

static void bb_changed(const BbEntry *e) { vcpe_ev_post(VCPE_EV_BB_CHANGED, (int32_t)e->key, 0, e->name); }

void vcpe_bb_set_int(const char *name, int32_t v)
{
    BbEntry *e = bb_find(name, 1);
    if (!e || (e->type == VCPE_BB_INT && e->v.i == v))
        return;   /* unchanged values post no event */
    e->type = VCPE_BB_INT;
    e->v.i = v;
    bb_changed(e);
}

void vcpe_bb_set_float(const char *name, float v)
{
    BbEntry *e = bb_find(name, 1);
    if (!e || (e->type == VCPE_BB_FLOAT && e->v.f == v))
        return;
    e->type = VCPE_BB_FLOAT;
    e->v.f = v;
    bb_changed(e);
}

void vcpe_bb_set_str(const char *name, const char *v)
{
    BbEntry *e = bb_find(name, 1);
    if (!e || !v || (e->type == VCPE_BB_STR && !strncmp(e->v.s, v, sizeof e->v.s - 1)))
        return;
    e->type = VCPE_BB_STR;
    snprintf(e->v.s, sizeof e->v.s, "%s", v);
    bb_changed(e);
}

int32_t vcpe_bb_int(const char *name, int32_t def)
{
    const BbEntry *e = bb_find(name, 0);
    return !e ? def : e->type == VCPE_BB_INT ? e->v.i : e->type == VCPE_BB_FLOAT ? (int32_t)e->v.f : def;
}

float vcpe_bb_float(const char *name, float def)
{
    const BbEntry *e = bb_find(name, 0);
    return !e ? def : e->type == VCPE_BB_FLOAT ? e->v.f : e->type == VCPE_BB_INT ? (float)e->v.i : def;
}

const char *vcpe_bb_str(const char *name, const char *def)
{
    const BbEntry *e = bb_find(name, 0);
    return e && e->type == VCPE_BB_STR ? e->v.s : def;
}

int vcpe_bb_type(const char *name)
{
    const BbEntry *e = bb_find(name, 0);
    return e ? e->type : VCPE_BB_NONE;
}

int vcpe_bb_count(void) { return bb_n; }
void vcpe_bb_clear(void) { memset(bb, 0, sizeof bb); bb_n = 0; }

void vcpe_bb_dump(void (*line)(const char *text))
{
    char t[96];
    for (int i = 0; i < BB_N; i++) {
        const BbEntry *e = &bb[i];
        if (!e->key)
            continue;
        if (e->type == VCPE_BB_INT) snprintf(t, sizeof t, "%s = %d", e->name, (int)e->v.i);
        else if (e->type == VCPE_BB_FLOAT) snprintf(t, sizeof t, "%s = %g", e->name, (double)e->v.f);
        else snprintf(t, sizeof t, "%s = \"%s\"", e->name, e->v.s);
        line(t);
    }
}

/* ================================================================== 3. live tuning */
#define TUNE_N 128
typedef struct { uint32_t key; char name[40]; int is_int; float *f; int *i; float lo, hi; } Tune;
static Tune tunes[TUNE_N];
static int ntunes;
static char tune_path[300];
static int tune_poll = 30, tune_age;
static long tune_size = -1;
static long long tune_mtime = -1;

static int tune_add(const char *name, float *f, int *i, float lo, float hi)
{
    if (ntunes >= TUNE_N)
        return -1;
    Tune *t = &tunes[ntunes++];
    t->key = key_of(name);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->is_int = i != 0;
    t->f = f;
    t->i = i;
    t->lo = lo;
    t->hi = hi;
    return 0;
}

int vcpe_tune_float(const char *name, float *var, float lo, float hi) { return tune_add(name, var, 0, lo, hi); }
int vcpe_tune_int(const char *name, int *var, int lo, int hi) { return tune_add(name, 0, var, (float)lo, (float)hi); }

static const char *path(void)
{
    if (!tune_path[0]) {
        if (vcpe_path)
            vcpe_path(tune_path, sizeof tune_path, "TUNE.TXT");
        else
            snprintf(tune_path, sizeof tune_path, "TUNE.TXT");
    }
    return tune_path;
}

void vcpe_tune_file(const char *p, int poll_frames)
{
    tune_path[0] = 0;
    if (p && strchr(p, ':'))
        snprintf(tune_path, sizeof tune_path, "%s", p);         /* a full device path: host0:/..., ms0:/... */
    else if (p && vcpe_path)
        vcpe_path(tune_path, sizeof tune_path, p);              /* a file name next to the EBOOT */
    else if (p)
        snprintf(tune_path, sizeof tune_path, "%s", p);
    tune_poll = poll_frames > 0 ? poll_frames : 0;
    tune_size = -1;
    tune_mtime = -1;
}

int vcpe_tune_set(const char *name, const char *value)
{
    uint32_t k = key_of(name);
    for (int n = 0; n < ntunes; n++) {
        Tune *t = &tunes[n];
        if (t->key != k || strcmp(t->name, name))
            continue;
        float v = strtof(value, 0);
        if (t->hi > t->lo)
            v = v < t->lo ? t->lo : v > t->hi ? t->hi : v;   /* clamp to the registered range */
        if (t->is_int) {
            int iv = (int)(v < 0 ? v - 0.5f : v + 0.5f);
            if (*t->i == iv)
                return 0;
            *t->i = iv;
            vcpe_ev_post(VCPE_EV_TUNE_CHANGED, (int32_t)t->key, (float)iv, t->i);
        } else {
            if (*t->f == v)
                return 0;
            *t->f = v;
            vcpe_ev_post(VCPE_EV_TUNE_CHANGED, (int32_t)t->key, v, t->f);
        }
        return 1;
    }
    return -1;
}

int vcpe_tune_reload(void)
{
    FILE *f = fopen(path(), "r");
    if (!f)
        return -1;
    char line[160], name[64], value[64];
    int changed = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;   /* the rest of the line is a comment */
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = ' ';       /* "name = value" and "name=value" both become "name value" */
        if (sscanf(line, "%63s %63s", name, value) == 2 && vcpe_tune_set(name, value) > 0)
            changed++;
    }
    fclose(f);
    return changed;
}

/* re-read only when the file changed: a stat call every poll_frames frames is cheap, a parse is not */
static void tune_poll_file(void)
{
    if (!ntunes || !tune_poll || ++tune_age < tune_poll)
        return;
    tune_age = 0;
    struct stat st;
    if (stat(path(), &st) != 0)
        return;
    if ((long)st.st_size == tune_size && (long long)st.st_mtime == tune_mtime)
        return;
    tune_size = (long)st.st_size;
    tune_mtime = (long long)st.st_mtime;
    vcpe_tune_reload();
}

int vcpe_tune_save(void)
{
    FILE *f = fopen(path(), "w");
    if (!f)
        return -1;
    fprintf(f, "# VCPE live tuning: name = value (edit while the game runs)\n");
    for (int n = 0; n < ntunes; n++) {
        const Tune *t = &tunes[n];
        if (t->is_int) fprintf(f, "%s = %d\n", t->name, *t->i);
        else fprintf(f, "%s = %g\n", t->name, (double)*t->f);
    }
    int ok = fclose(f) == 0;
    struct stat st;   /* our own write is not a change to read back */
    if (ok && stat(path(), &st) == 0) {
        tune_size = (long)st.st_size;
        tune_mtime = (long long)st.st_mtime;
    }
    return ok ? 0 : -1;
}

/* ================================================================== 4. toolkit */
void vcpe_ring_init(VcpeRing *r, void *mem, uint32_t elem, uint32_t cap)
{
    r->mem = mem;
    r->elem = elem;
    r->cap = cap;
    r->head = r->tail = 0;
}

int vcpe_ring_push(VcpeRing *r, const void *item)
{
    if (r->head - r->tail >= r->cap)
        return -1;
    memcpy(r->mem + (r->head & (r->cap - 1)) * r->elem, item, r->elem);
    r->head++;
    return 0;
}

int vcpe_ring_pop(VcpeRing *r, void *item)
{
    if (r->head == r->tail)
        return -1;
    memcpy(item, r->mem + (r->tail & (r->cap - 1)) * r->elem, r->elem);
    r->tail++;
    return 0;
}

/* The pool's free blocks form a linked list through their own first bytes, so no extra memory is needed. */
int vcpe_pool_init(VcpePool *p, void *mem, uint32_t block_size, uint32_t count)
{
    block_size = (block_size + sizeof(void *) - 1) & ~(uint32_t)(sizeof(void *) - 1);   /* keep blocks aligned */
    if (!mem || block_size < sizeof(void *) || !count)
        return -1;
    p->mem = mem;
    p->size = block_size;
    p->count = count;
    p->used = 0;
    p->free_list = 0;
    for (uint32_t i = count; i-- > 0;) {   /* backwards, so block 0 is handed out first */
        void **b = (void **)(p->mem + i * block_size);
        *b = p->free_list;
        p->free_list = b;
    }
    return 0;
}

void *vcpe_pool_alloc(VcpePool *p)
{
    void **b = p->free_list;
    if (!b)
        return 0;
    p->free_list = *b;
    p->used++;
    return b;
}

void vcpe_pool_free(VcpePool *p, void *block)
{
    if (!block)
        return;
    *(void **)block = p->free_list;
    p->free_list = block;
    p->used--;
}

/* profiler: acc collects this frame's time per scope; avg is a running mean over about 30 frames */
static struct { char name[16]; uint32_t t0, acc; int avg; } prof[VCPE_PROF_MAX];
static int nprof;

int vcpe_prof_id(const char *name)
{
    for (int i = 0; i < nprof; i++)
        if (!strncmp(prof[i].name, name, sizeof prof[i].name - 1))
            return i;
    if (nprof >= VCPE_PROF_MAX)
        return -1;
    snprintf(prof[nprof].name, sizeof prof[nprof].name, "%s", name);
    return nprof++;
}

void vcpe_prof_begin(int id) { if (id >= 0 && id < nprof) prof[id].t0 = now_us(); }
void vcpe_prof_end(int id) { if (id >= 0 && id < nprof) prof[id].acc += now_us() - prof[id].t0; }
int vcpe_prof_us(int id) { return id >= 0 && id < nprof ? prof[id].avg : 0; }

static void prof_frame(void)
{
    for (int i = 0; i < nprof; i++) {
        prof[i].avg += ((int)prof[i].acc - prof[i].avg) / 30;   /* move 1/30 of the way: smooth but responsive */
        prof[i].acc = 0;
    }
}

int vcpe_prof_line(char *out, int n)
{
    int len = snprintf(out, n, "prof");
    for (int i = 0; i < nprof && len < n; i++)
        len += snprintf(out + len, n - len, " %s %d.%d", prof[i].name, prof[i].avg / 1000, prof[i].avg / 100 % 10);
    if (len < n)
        len += snprintf(out + len, n - len, " ms");
    return len;
}

/* ================================================================== frame update */
static int last_resume = -1, last_misses = -1, was_loading, low_kb, low_age, low_sent;

void vcpe_ultra_low_memory_kb(int kb) { low_kb = kb; low_sent = 0; }

void vcpe_ultra_update(void)
{
    /* sleep / resume: the system bumps vcpe_resume_gen after waking */
    if (&vcpe_resume_gen) {
        int g = vcpe_resume_gen;
        if (last_resume >= 0 && g != last_resume)
            vcpe_ev_post(VCPE_EV_RESUME, g, 0, 0);
        last_resume = g;
    }
    /* a missed vblank since the last update */
    if (&vcpe_stat_vblank_misses) {
        int m = vcpe_stat_vblank_misses;
        if (last_misses >= 0 && m > last_misses)
            vcpe_ev_post(VCPE_EV_LATE_FRAME, &vcpe_stat_vblanks ? vcpe_stat_vblanks : 2, 0, 0);
        last_misses = m;
    }
    /* loading screen edges (a load happens between frames, so it is seen as begun and ended together) */
    if (vcpe_loading_active) {
        int l = vcpe_loading_active();
        if (l && !was_loading)
            vcpe_ev_post(VCPE_EV_LOAD_BEGIN, 0, 0, 0);
        if (!l && was_loading)
            vcpe_ev_post(VCPE_EV_LOAD_END, vcpe_loading_shown ? vcpe_loading_shown() : 0, 0, 0);
        was_loading = l;
    }
    /* low memory: the heap probe is not free, so only twice a second, and only once until memory recovers */
    if (low_kb && vcpe_heap_kb && ++low_age >= 30) {
        low_age = 0;
        int kb = vcpe_heap_kb();
        if (kb < low_kb && !low_sent)
            vcpe_ev_post(VCPE_EV_LOW_MEMORY, kb, 0, 0);
        low_sent = kb < low_kb;
    }
    tune_poll_file();
    prof_frame();
    vcpe_ev_dispatch();
}

/* ================================================================== autotest commands */
int vcpe_ultra_command(const char *cmd, const char *arg, int num, int num2, const char *line, unsigned *held)
{
    (void)num2; (void)held;
    char name[64], value[64];
    if (!strcmp(cmd, "tune")) {          /* tune NAME VALUE */
        if (sscanf(line, "%*s %63s %63s", name, value) == 2)
            vcpe_tune_set(name, value);
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(cmd, "bb")) {            /* bb NAME VALUE: an int, a float (has '.') or a string */
        if (sscanf(line, "%*s %63s %63s", name, value) == 2) {
            char *end;
            long iv = strtol(value, &end, 10);
            if (!*end) vcpe_bb_set_int(name, (int32_t)iv);
            else if (strtof(value, &end), !*end) vcpe_bb_set_float(name, strtof(value, 0));
            else vcpe_bb_set_str(name, value);
        }
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(cmd, "bbcheck")) {       /* bbcheck NAME VALUE: compare as text */
        if (sscanf(line, "%*s %63s %63s", name, value) == 2 && vcpe_log) {
            char now[64];
            int t = vcpe_bb_type(name);
            if (t == VCPE_BB_INT) snprintf(now, sizeof now, "%d", (int)vcpe_bb_int(name, 0));
            else if (t == VCPE_BB_FLOAT) snprintf(now, sizeof now, "%g", (double)vcpe_bb_float(name, 0));
            else snprintf(now, sizeof now, "%s", vcpe_bb_str(name, "(none)"));
            vcpe_log("%s bb %s = %s (want %s)", strcmp(now, value) ? "FAIL" : "PASS", name, now, value);
        }
        return VCPE_CMD_NEXT;
    }
    if (!strcmp(cmd, "ev")) {            /* ev ID INT */
        vcpe_ev_post(atoi(arg), num, 0, 0);
        return VCPE_CMD_NEXT;
    }
    return VCPE_CMD_UNKNOWN;
}
