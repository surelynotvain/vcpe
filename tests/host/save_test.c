/* host test for vcpe_save: newest-slot pick, torn writes, version/size changes, CRC */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vcpe_save.h"
static char dir[256];
void vcpe_path(char *out, int n, const char *file) { snprintf(out, n, "%s/%s", dir, file); }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
typedef struct { int level, coins; char name[8]; } S;
int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argv[1]);
    S s = {0}, r;
    vcpe_save_erase("SAVE.DAT");
    CHECK(vcpe_save_read("SAVE.DAT", &r, sizeof r, 1) == -1);
    for (int i = 1; i <= 7; i++) { s.level = i; CHECK(vcpe_save_write("SAVE.DAT", &s, sizeof s, 1) == 0);
        memset(&r, 0, sizeof r); CHECK(vcpe_save_read("SAVE.DAT", &r, sizeof r, 1) == 0); CHECK(r.level == i); }
    /* tear the newest slot (level 7) mid-write: the previous save (6) comes back */
    char p[300]; FILE *f;
    for (int slot = 0; slot < 2; slot++) {
        snprintf(p, sizeof p, slot ? "%s/SAVE.DAT.BAK" : "%s/SAVE.DAT", dir);
        f = fopen(p, "rb"); fseek(f, 12, SEEK_SET); unsigned seq; fread(&seq, 4, 1, f); fclose(f);
        if (seq == 7) { f = fopen(p, "r+b"); fseek(f, 20, SEEK_SET); fputc(0x55, f); fclose(f); }
    }
    CHECK(vcpe_save_read("SAVE.DAT", &r, sizeof r, 1) == 0); CHECK(r.level == 6);
    /* the next write replaces the broken slot, never the good one */
    s.level = 8; CHECK(vcpe_save_write("SAVE.DAT", &s, sizeof s, 1) == 0);
    CHECK(vcpe_save_read("SAVE.DAT", &r, sizeof r, 1) == 0); CHECK(r.level == 8);
    /* another version or size reads as "no usable save" and leaves data alone */
    r.level = 99; CHECK(vcpe_save_read("SAVE.DAT", &r, sizeof r, 2) == -2); CHECK(r.level == 99);
    CHECK(vcpe_save_read("SAVE.DAT", &r, sizeof r - 1, 1) == -2);
    CHECK(vcpe_crc32("123456789", 9, 0) == 0xCBF43926u);
    printf("save ok\n");
    return 0;
}
