/* Compile against either engine tree: compare the actual particle squares and trail vertices with a fixed seed.
 * Also fault-inject realloc for the optimized tree. Unused rendering entry points are removed by --gc-sections. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
static int fail_alloc, allocs;
static void *test_realloc(void *p, size_t n)
{
    allocs++;
    if (fail_alloc) { fail_alloc--; return 0; }
    return realloc(p, n);
}
#define realloc test_realloc
#include ENGINE_PART
#include ENGINE_RENDER
#undef realloc

World W;
static uint32_t hash = 2166136261u;
static int squares, vertices;
static void digest(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { hash ^= *b++; hash *= 16777619u; }
}
void obj_pos(int o, float *x, float *y) { *x = W.obj[o].lx; *y = W.obj[o].ly; }
int obj_active(int o) { return W.obj[o].active; }
void gfx_fill(float x, float y, float w, float h, uint32_t c)
{
    squares++;
    digest(&x, sizeof x); digest(&y, sizeof y); digest(&w, sizeof w); digest(&h, sizeof h); digest(&c, sizeof c);
}
void gfx_strip(const float *xy, const uint32_t *c, int n, int add)
{
    vertices += n;
    (void)add; digest(xy, (size_t)n * 2 * sizeof *xy); digest(c, (size_t)n * sizeof *c);
}

static void setup(void)
{
    static WPart part[8];
    static WTrail trail[8];
    W.part = part; W.npart = 2; W.cappart = W.npart + RESERVE;
    W.trail = trail; W.ntrail = 1; W.captrail = W.ntrail + RESERVE;
    for (int i = 0; i < 8; i++) {
        W.obj[i].active = 1;
        part[i] = (WPart){.go=i, .loop=1, .len=.75f, .life0=.1f, .life1=.9f, .speed0=.2f, .speed1=4,
            .size0=.1f, .size1=.4f, .grav=.1f, .radius=.2f, .r=255, .g=170, .b=31, .a=180,
            .awake=1, .nburst=4, .burst_t={0,.1f,.3f,.5f}, .burst_n={12,16,32,12}};
        trail[i] = (WTrail){.go=i, .enabled=1, .time=.5f, .width=.2f, .r0=255, .a0=255, .b1=255};
    }
    cam_x = cam_y = 0; pxu = 20;
    srand(42);
    particles_reset();
}

int main(void)
{
    setup();
    for (int frame = 0; frame < 600; frame++) {
        if (frame == 80) particles_stop(0);
        if (frame == 120) particles_play(0);
        if (frame == 180) { W.npart = 4; W.ntrail = 3; particles_play(2); particles_play(3); }
        if (frame == 300) W.obj[1].active = 0;
        if (frame == 330) W.obj[1].active = 1;
        if (frame == 420) { W.part[3].go = -1; W.trail[2].go = -1; }
        for (int i = 0; i < 8; i++) { W.obj[i].lx = (float)(frame % 100) * .05f + i; W.obj[i].ly = i * .2f; }
        particles_update(1.0f/60);
        trail_update(1.0f/60);
        for (int i = 0; i < W.npart; i++) render_particles_draw(i, 0, 0, 20);
        for (int i = 0; i < W.ntrail; i++) draw_trail(i);
    }
    printf("effects hash %08x\n", hash);
#ifdef MEMORY_OPTIMIZED
    assert(nps == 4 && ntrails == 3);
    assert(particles_memory_bytes() == 4 * sizeof *ps);
    assert(render_memory_bytes() == 3 * sizeof *trails);
    printf("effects bytes: %u particles + %u trails; reserve avoided: %zu bytes\n",
        particles_memory_bytes(), render_memory_bytes(), (size_t)RESERVE * (sizeof *ps + sizeof *trails));
    int before = allocs;
    particles_update(1.0f/60); trail_update(1.0f/60);
    assert(allocs == before); /* no heap traffic at steady state */
    W.npart = W.ntrail = 6;
    P live = ps[0].p[0]; int live_n = ps[0].n;
    fail_alloc = 1;
    particles_play(5);
    assert(nps == 4 && particles_alloc_fails == 1 && ps[0].n == live_n);
    assert(!memcmp(&live, &ps[0].p[0], sizeof live));
    render_particles_draw(5, 0, 0, 20); /* failed-grow index is safe */
    particles_play(5);
    assert(nps == 6 && ps[0].n == live_n && ps[5].playing);
    fail_alloc = 1; trail_update(1.0f/60);
    assert(ntrails == 3 && render_alloc_fails == 1);
    draw_trail(5); /* failed-grow index is safe */
    trail_update(1.0f/60); assert(ntrails == 6);
    fail_alloc = 1; push(IT_PART, 0, 0, 0, 0, 0, 0, 0);
    assert(!items && !nitems && !capitems && render_alloc_fails == 2);
    push(IT_PART, 0, 0, 0, 0, 0, 0, 0);
    assert(items && nitems == 1 && capitems == 1024);
    W.npart = 0; particles_reset();
    assert(!ps && !nps && !particles_memory_bytes());
    W.npart = 1; fail_alloc = 1; particles_reset();
    assert(!ps && nps == 0 && particles_alloc_fails == 2);
    particles_update(1.0f/60); assert(ps && nps == 1 && ps[0].playing);
    puts("effect growth, failure recovery, empty scene and steady-state allocation checks passed");
#ifdef ADAPT_CHECKS
    for (int i=0; i<20; i++) particles_update(1.0f/60);
    assert(ps[0].n > 1);
    P saved = ps[0].p[0];
    squares = 0; render_particles_draw(0, 0, 0, 20); int full = squares;
    VcpeAdaptConfig c; vcpe_adapt_defaults(&c); c.probe_frames = 10000;
    vcpe_adapt_config(&c); vcpe_adapt_enable(1);
    for (int i=0; i<500; i++) vcpe_adapt_sample(35000, 8192);
    assert(vcpe_adapt_effect_scale() == 128);
    squares = 0;
    srand(7); int expected = rand(); srand(7);
    render_particles_draw(0, 0, 0, 20);
    assert(squares == full / 2 && rand() == expected && !memcmp(&saved, &ps[0].p[0], sizeof saved));
    vcpe_adapt_enable(0);
    vertices=0; draw_trail(0); int full_vertices=vertices;
    vcpe_adapt_enable(1); for (int i=0; i<500; i++) vcpe_adapt_sample(35000,8192);
    vertices=0; draw_trail(0); assert(vertices >= 4 && vertices < full_vertices);
    vcpe_adapt_enable(0);
    W.ntrail=2;
    uint32_t before_bytes=render_memory_bytes();
    assert(render_trim() > 0 && render_memory_bytes() < before_bytes && ntrails == 2 && capitems == 64);
    W.npart=0; assert(particles_trim() == sizeof *ps && !ps && !nps);
    printf("adaptive drawing: %d -> %d squares, %d -> %d trail vertices; simulation/RNG unchanged\n",
        full, squares, full_vertices, vertices);
#endif
#endif
    (void)allocs;
    (void)squares; (void)vertices;
    free(ps); free(trails); free(items);
    return 0;
}
