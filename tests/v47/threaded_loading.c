/* Host test for vcpe_jobs (clients, reservations, servers, automatic priority, aging) and vcpe_pack (one-read
 * loads, priority eviction, pinning, lookups, background loads through reservations).
 * Run: tests/host/jobs_pack_test.sh */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "vcpe_jobs.h"
#include "vcpe_pack.h"
#include "vcpe_ultra.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { fails++; printf("FAIL " __VA_ARGS__); } printf("\n"); } while (0)

/* vcpe_pak stand-in: reads the test PAK built by the script */
static const char *pak = "threaded.pak";
const char *vcpe_pak_path(void) { return pak; }
int vcpe_pak_read(uint32_t off, void *dst, uint32_t size)
{
    FILE *f = fopen(pak, "rb");
    int ok = f && !fseek(f, off, SEEK_SET) && fread(dst, 1, size, f) == size;
    if (f) fclose(f);
    return ok ? 0 : -1;
}

static char order[16];
static int norder;
static pthread_t main_thread;
static int done_on_main = 1, done_count;
static void job(void *arg) { order[norder++] = *(char *)arg; usleep(2000); }
static pthread_mutex_t slow_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slow_cv=PTHREAD_COND_INITIALIZER;
static int slow_started,slow_release;
static void slow(void *arg) {
    (void)arg; pthread_mutex_lock(&slow_lock); slow_started++; pthread_cond_broadcast(&slow_cv);
    while(!slow_release) pthread_cond_wait(&slow_cv,&slow_lock);
    pthread_mutex_unlock(&slow_lock);
}
static void done(int id, void *arg) { (void)id; (void)arg; done_count++; if (!pthread_equal(pthread_self(), main_thread)) done_on_main = 0; }

static int ready_ok = -1;
static void ready(int h, int ok, void *u) { (void)h; (void)u; ready_ok = ok; }

int main(int argc, char **argv)
{
    setbuf(stdout,0);
    unsigned off[3], size[3];
    if (argc < 7) { printf("usage: offsets and sizes of 3 packs\n"); return 2; }
    for (int i = 0; i < 3; i++) { off[i] = strtoul(argv[1 + i * 2], 0, 0); size[i] = strtoul(argv[2 + i * 2], 0, 0); }
    main_thread = pthread_self();

    /* -------- jobs: 3 reservations, 2 clients; with no servers the main thread serves them best first */
    int ai = vcpe_client_add("ai", 70), loader = vcpe_client_add("loader", 30);
    CHECK(ai == 1 && loader == 2 && vcpe_client_add("ai", 10) == ai && vcpe_client_count() == 3, "client list");
    static char L = 'L', A = 'A', U = 'U';
    vcpe_reserve(loader, job, &L, done, 0);
    vcpe_reserve(ai, job, &A, done, 0);
    vcpe_reserve(ai, job, &U, done, 10);
    vcpe_jobs_budget(1000000);
    int n = vcpe_jobs_update();
    order[norder] = 0;
    CHECK(n == 3 && !strcmp(order, "UAL") && done_count == 3, "priority order on the main thread: %s", order);

    /* aging: a low-priority reservation that waited long enough beats a fresh high-priority one */
    norder = 0;
    vcpe_jobs_budget(0);
    vcpe_jobs_age_frames(1);
    vcpe_reserve(loader, job, &L, 0, 0);
    for (int f = 0; f < 50; f++) vcpe_jobs_update();   /* 30 + 50 > 70 */
    vcpe_reserve(ai, job, &A, 0, 0);
    vcpe_jobs_budget(1000000);
    vcpe_jobs_update();
    order[norder] = 0;
    CHECK(!strcmp(order, "LA"), "aging lifts a waiting low-priority reservation: %s", order);
    vcpe_jobs_age_frames(4);

    /* cancel */
    vcpe_jobs_budget(0);
    int c = vcpe_reserve(ai, job, &A, 0, 0);
    CHECK(vcpe_reserve_cancel(c) == 1 && vcpe_reserve_state(c) == 0 && vcpe_jobs_pending() == 0, "cancel a pending reservation");

    /* -------- servers: 3 reservations for 2 clients handed out by 2 servers */
    done_count = 0;
    int r1 = vcpe_reserve(loader, slow, 0, done, 0);
    int r2 = vcpe_reserve(ai, slow, 0, done, 0);
    int r3 = vcpe_reserve(ai, slow, 0, done, 10);
    /* Queue the whole priority set before launching workers: a worker cannot rank work not submitted yet. */
    CHECK(vcpe_servers_start(2, 0) == 2, "2 servers started");
    pthread_mutex_lock(&slow_lock);
    while(slow_started<2) pthread_cond_wait(&slow_cv,&slow_lock);
    pthread_mutex_unlock(&slow_lock);
    int running = (vcpe_reserve_state(r1) == VCPE_RES_RUNNING) + (vcpe_reserve_state(r2) == VCPE_RES_RUNNING) +
                  (vcpe_reserve_state(r3) == VCPE_RES_RUNNING);
    int loader_waits = vcpe_reserve_state(r1) == VCPE_RES_PENDING;
    pthread_mutex_lock(&slow_lock); slow_release=1; pthread_cond_broadcast(&slow_cv); pthread_mutex_unlock(&slow_lock);
    vcpe_jobs_wait_all();
    int s1 = vcpe_reserve_server(r1), s2 = vcpe_reserve_server(r2), s3 = vcpe_reserve_server(r3);
    vcpe_jobs_update();
    CHECK(running == 2 && loader_waits, "both servers took the two AI reservations first, the loader waited");
    CHECK(done_count == 3 && done_on_main && s1 >= 0 && s2 >= 0 && s3 >= 0 && s2 != s3, "all 3 served (servers %d %d %d), done callbacks on the main thread", s1, s2, s3);
    char line[160];
    vcpe_jobs_line(line, sizeof line);
    printf("     %s\n", line);
    CHECK(vcpe_client_served(ai) >= 4 && vcpe_client_served(loader) >= 3 && vcpe_client_us(ai) > 0, "per-client stats");

    /* -------- packs: 2 slots, 3 packs, priority eviction */
    CHECK(vcpe_pack_pool(0, 2 * 4096, 2) == 4096, "pool of 2 slots");
    int pa = vcpe_pack_load(off[0], size[0], 60), pb = vcpe_pack_load(off[1], size[1], 20);
    uint32_t sz; const char *t = vcpe_pack_find(pa, "hello.txt", &sz);
    CHECK(pa >= 0 && pb >= 0 && t && sz == 11 && !memcmp(t, "hello world", 11) && ((uintptr_t)t & 63) == 0,
          "one-read load, asset found by name, 64-byte aligned");
    CHECK(vcpe_pack_find(pa, "missing", 0) == 0 && vcpe_pack_count(pa) == 2, "missing names and entry count");
    CHECK(vcpe_pack_load(off[0], size[0], 10) == pa, "loading the same pack again returns its handle");
    int pc = vcpe_pack_load(off[2], size[2], 50);
    CHECK(pc >= 0 && vcpe_pack_state(pb) == VCPE_PACK_FREE && vcpe_pack_state(pa) == VCPE_PACK_READY && vcpe_pack_evictions == 1,
          "a third pack evicts the lowest priority one; its handle goes stale");
    vcpe_pack_pin(pc, 1);
    vcpe_pack_priority(pa, 5);
    vcpe_pack_line(line, sizeof line);
    printf("     %s\n", line);
    int pb2 = vcpe_pack_load(off[1], size[1], 90);
    CHECK(pb2 >= 0 && vcpe_pack_state(pa) == VCPE_PACK_FREE && vcpe_pack_state(pc) == VCPE_PACK_READY, "pinned pack kept, priority 5 evicted");
    vcpe_pack_pin(pb2, 1);
    CHECK(vcpe_pack_load(off[0], size[0], 99) == -1, "every slot pinned: the load fails instead of evicting");
    vcpe_pack_unload(pb2);
    vcpe_pack_pin(pc, 0);
    CHECK(vcpe_pack_state(pb2) == VCPE_PACK_FREE, "unload is instant");

    /* background load through a reservation */
    int pa2 = vcpe_pack_load_async(off[0], size[0], 70, ready, 0);
    int st0 = vcpe_pack_state(pa2);
    vcpe_jobs_wait_all();
    vcpe_jobs_update();
    CHECK(pa2 >= 0 && st0 == VCPE_PACK_LOADING && ready_ok == 1 && vcpe_pack_state(pa2) == VCPE_PACK_READY &&
          vcpe_pack_find(pa2, "hello.txt", 0), "background load: LOADING, then READY via the done callback");
    vcpe_jobs_shutdown();
    CHECK(vcpe_servers_count() == 0, "servers shut down");
    printf("RESULT %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
