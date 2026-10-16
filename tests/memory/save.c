/* Check large saves, bounded scratch memory, failed reads, and sequence wrap without touching real saves. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t allocated, peak_bytes;
static int allocation_limit = -1, calls;
static void *save_malloc(size_t n)
{
    calls++;
    if (allocation_limit == 0) return 0;
    if (allocation_limit > 0) allocation_limit--;
    void *p = malloc(n);
    if (p) { allocated += n; if (allocated > peak_bytes) peak_bytes = allocated; }
    return p;
}
static void save_free(void *p) { if (p) allocated = 0; free(p); }
#define malloc save_malloc
#define free save_free
#include "../../src/vcpe_save.c"
#undef malloc
#undef free
void vcpe_path(char *out, int n, const char *file) { snprintf(out, n, "%s", file); }
static void slot(int which, uint32_t seq, const void *p, uint32_t size)
{
    FILE *f = fopen(which ? "LARGE.BAK" : "LARGE", "wb"); assert(f);
    Hdr h = {{'V','S','A','V'}, 7, size, seq, vcpe_crc32(p, size, 0)};
    assert(fwrite(&h, sizeof h, 1, f) == 1 && fwrite(p, 1, size, f) == size);
    assert(!fclose(f));
}
int main(void)
{
    enum { N = 131073 }; /* many chunks, non-aligned tail */
    static uint8_t data[N], out[N];
    for (int i = 0; i < N; i++) data[i] = (uint8_t)(i * 31);
    vcpe_save_erase("LARGE");
    memset(out, 0xa5, N);
    assert(vcpe_save_read("LARGE", out, N, 7) == -1 && out[0] == 0xa5);
    allocation_limit = 0;
    assert(vcpe_save_write("LARGE", data, N, 7) == 0);
    assert(calls == 0); /* validation does not allocate a full save */
    data[0] = 17;
    assert(vcpe_save_write("LARGE", data, N, 7) == 0 && calls == 0);
    assert(vcpe_save_read("LARGE", out, N, 7) == -2 && out[0] == 0xa5);
    allocation_limit = 1; calls = 0;
    assert(vcpe_save_read("LARGE", out, N, 7) == 0 && !memcmp(data, out, N));
    assert(calls == 1 && peak_bytes == N && allocated == 0);
    FILE *f = fopen("LARGE.BAK", "ab"); assert(f); fputc(1, f); fclose(f);
    allocation_limit = 1;
    assert(vcpe_save_read("LARGE", out, N, 7) == 0 && out[0] == 0); /* trailing byte rejects newest */
    slot(0, UINT32_MAX, data, N); data[0] = 29; slot(1, 0, data, N);
    allocation_limit = 1;
    assert(vcpe_save_read("LARGE", out, N, 7) == 0 && out[0] == 29); /* sequence wraps */
    memset(out, 0xa5, N);
    assert(vcpe_save_read("LARGE", out, N, 8) == -2);
    for (int i = 0; i < N; i++) assert(out[i] == 0xa5);
    allocation_limit = 0;
    assert(vcpe_save_write("EMPTY", data, 0, 1) == 0);
    allocation_limit = 1;
    assert(vcpe_save_read("EMPTY", out, 0, 1) == 0);
    vcpe_save_erase("LARGE"); vcpe_save_erase("EMPTY");
    printf("save checks passed: %d-byte payload, write scratch heap 0, read scratch heap %zu\n", N, peak_bytes);
}
