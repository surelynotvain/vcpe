/* VCPE two-slot checked saves (see vcpe_save.h). Plain C file I/O, so host tests use it too. */

/* HOW THIS FILE WORKS
 *
 * A save that is half-written when the battery dies or the stick is pulled must not destroy the player's progress. So
 * every save has two files ("NAME" and "NAME.BAK") and each write goes to the one that is NOT the newest good save.
 * If the write is cut short, the other file still holds the previous save.
 *
 * Each file is a header {"VSAV", version, size, sequence number, CRC-32} followed by the data. Reading checks all of
 * it: wrong version or size (an old game build), a CRC mismatch (a torn write) or trailing bytes make that slot
 * unusable, and the newest usable slot (highest sequence number) wins. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_sys.h"
#include "vcpe_save.h"

typedef struct { char magic[4]; uint32_t version, size, seq, crc; } Hdr;

/* Standard CRC-32 (the zip/PNG one), table built on first use. Pass the previous result as crc to checksum data in
 * parts. */
uint32_t vcpe_crc32(const void *data, uint32_t n, uint32_t crc)
{
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* Slot 0 is NAME, slot 1 is NAME.BAK, both next to the EBOOT. */
static void slot_path(char *out, int n, const char *name, int slot)
{
    char f[256];
    snprintf(f, sizeof f, slot ? "%s.BAK" : "%s", name);
    vcpe_path(out, n, f);
}

/* 1: good, 0: missing, -1: unusable. A null buf validates in fixed-size chunks, without a save-sized allocation. */
static int read_slot(const char *name, int slot, void *buf, uint32_t size, uint32_t version, uint32_t *seq)
{
    char p[300];
    slot_path(p, sizeof p, name, slot);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    Hdr h;
    int ok = fread(&h, 1, sizeof h, f) == sizeof h && !memcmp(h.magic, "VSAV", 4) && h.version == version &&
             h.size == size;
    uint8_t scratch[512];
    uint32_t crc = 0, at = 0;
    while (ok && at < size) {
        uint32_t n = size - at;
        if (n > sizeof scratch)
            n = sizeof scratch;
        void *dst = buf ? (uint8_t *)buf + at : scratch;
        ok = fread(dst, 1, n, f) == n;
        if (ok)
            crc = vcpe_crc32(dst, n, crc);
        at += n;
    }
    ok = ok && fgetc(f) == EOF && !ferror(f) && crc == h.crc;
    fclose(f);
    if (ok)
        *seq = h.seq;
    return ok ? 1 : -1;
}

/* Read both slots and keep the newest good one. Sequence numbers are compared as a signed difference, so it still
 * works after they wrap around. -1: no save at all; -2: saves exist but none is usable. */
int vcpe_save_read(const char *name, void *data, uint32_t size, uint32_t version)
{
    uint32_t sa = 0, sb = 0;
    int ra = read_slot(name, 0, 0, size, version, &sa);
    int rb = read_slot(name, 1, 0, size, version, &sb);
    if (ra != 1 && rb != 1)
        return ra == 0 && rb == 0 ? -1 : -2;
    /* Only the selected copy needs storage. Revalidate it before touching the caller's data, since removable
     * media may fail between validation and this read. Fall back to the other validated slot if necessary. */
    void *tmp = malloc(size ? size : 1);
    if (!tmp)
        return -2;
    int first = ra == 1 && (rb != 1 || (int32_t)(sa - sb) >= 0) ? 0 : 1;
    uint32_t seq;
    int r = read_slot(name, first, tmp, size, version, &seq);
    if (r != 1 && (first ? ra : rb) == 1)
        r = read_slot(name, !first, tmp, size, version, &seq);
    if (r == 1)
        memcpy(data, tmp, size);
    free(tmp);
    r = r == 1 ? 0 : -2;
    return r;
}

/* Find which slot holds the newest good save and write to the other one, with the next sequence number. */
int vcpe_save_write(const char *name, const void *data, uint32_t size, uint32_t version)
{
    /* overwrite the slot that is not the newest good one */
    uint32_t sa = 0, sb = 0;
    int ra = read_slot(name, 0, 0, size, version, &sa);
    int rb = read_slot(name, 1, 0, size, version, &sb);
    int slot;
    uint32_t seq;
    if (ra == 1 && rb == 1) { slot = (int32_t)(sa - sb) >= 0 ? 1 : 0; seq = ((int32_t)(sa - sb) >= 0 ? sa : sb) + 1; }
    else if (ra == 1) { slot = 1; seq = sa + 1; }
    else if (rb == 1) { slot = 0; seq = sb + 1; }
    else { slot = 0; seq = 1; }
    Hdr h = {{'V', 'S', 'A', 'V'}, version, size, seq, vcpe_crc32(data, size, 0)};
    char p[300];
    slot_path(p, sizeof p, name, slot);
    FILE *f = fopen(p, "wb");
    if (!f)
        return -1;
    int ok = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

void vcpe_save_erase(const char *name)
{
    char p[300];
    for (int s = 0; s < 2; s++) {
        slot_path(p, sizeof p, name, s);
        remove(p);
    }
}

/* ------------------------------------------------------------------ 4.5: saves whose size varies (compressed) */
/* like read_slot, but the size comes from the header (at most cap); buf 0 only validates */
static int read_slot_var(const char *name, int slot, void *buf, uint32_t cap, uint32_t version, uint32_t *seq,
                         uint32_t *size)
{
    char p[300];
    slot_path(p, sizeof p, name, slot);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    Hdr h;
    int ok = fread(&h, 1, sizeof h, f) == sizeof h && !memcmp(h.magic, "VSAV", 4) && h.version == version &&
             h.size <= cap;
    uint8_t scratch[512];
    uint32_t crc = 0, at = 0;
    while (ok && at < h.size) {
        uint32_t n = h.size - at;
        if (n > sizeof scratch)
            n = sizeof scratch;
        void *dst = buf ? (uint8_t *)buf + at : scratch;
        ok = fread(dst, 1, n, f) == n;
        if (ok)
            crc = vcpe_crc32(dst, n, crc);
        at += n;
    }
    ok = ok && fgetc(f) == EOF && !ferror(f) && crc == h.crc;
    fclose(f);
    if (ok) {
        *seq = h.seq;
        *size = h.size;
    }
    return ok ? 1 : -1;
}

int vcpe_save_read_var(const char *name, void *data, uint32_t cap, uint32_t *size, uint32_t version)
{
    uint32_t sa = 0, sb = 0, za = 0, zb = 0;
    int ra = read_slot_var(name, 0, 0, cap, version, &sa, &za);
    int rb = read_slot_var(name, 1, 0, cap, version, &sb, &zb);
    if (ra != 1 && rb != 1)
        return ra == 0 && rb == 0 ? -1 : -2;
    int first = ra == 1 && (rb != 1 || (int32_t)(sa - sb) >= 0) ? 0 : 1;
    uint32_t seq;
    int r = read_slot_var(name, first, data, cap, version, &seq, size);
    if (r != 1 && (first ? ra : rb) == 1)
        r = read_slot_var(name, !first, data, cap, version, &seq, size);
    return r == 1 ? 0 : -2;
}

int vcpe_save_write_var(const char *name, const void *data, uint32_t size, uint32_t version)
{
    uint32_t sa = 0, sb = 0, za, zb;
    int ra = read_slot_var(name, 0, 0, 0xffffffffu, version, &sa, &za);
    int rb = read_slot_var(name, 1, 0, 0xffffffffu, version, &sb, &zb);
    int slot;
    uint32_t seq;
    if (ra == 1 && rb == 1) { slot = (int32_t)(sa - sb) >= 0 ? 1 : 0; seq = ((int32_t)(sa - sb) >= 0 ? sa : sb) + 1; }
    else if (ra == 1) { slot = 1; seq = sa + 1; }
    else if (rb == 1) { slot = 0; seq = sb + 1; }
    else { slot = 0; seq = 1; }
    Hdr h = {{'V', 'S', 'A', 'V'}, version, size, seq, vcpe_crc32(data, size, 0)};
    char p[300];
    slot_path(p, sizeof p, name, slot);
    FILE *f = fopen(p, "wb");
    if (!f)
        return -1;
    int ok = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

/* does a usable save exist (either slot)? */
int vcpe_save_exists_var(const char *name, uint32_t version)
{
    uint32_t s, z;
    return read_slot_var(name, 0, 0, 0xffffffffu, version, &s, &z) == 1 ||
           read_slot_var(name, 1, 0, 0xffffffffu, version, &s, &z) == 1;
}
