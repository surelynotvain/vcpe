/* VCPE cook: the input file system. Folders, ZIP files (self-extracting ones too: the archive is found from its end
 * and every offset is shifted by the stub's size) and RGSS3 archives (RPG Maker VX Ace's Game.rgss3a, decrypted as
 * it is read) are merged into one name space; a source added later overrides the files of the earlier ones, like
 * a patch copied over a game. */
#include "vcpe_cook.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef __PSP__
#include <pspiofilemgr.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

int (*cook_yield)(void);
void (*cook_feed)(const char *verb, const char *name);

typedef struct { char *path; int kind; FILE *f; } Source;   /* kind 0 dir, 1 zip, 2 rgss3 */

struct CookFs {
    Source src[8];
    int nsrc;
    CookFile *files;
    int nfiles, capfiles;
    int *hash;          /* key -> winning file index + 1 */
    int hcap;
    char err[160];
};

char *cook_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

int cook_keycmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

uint32_t cook_crc32(uint32_t crc, const void *p, size_t n) { return (uint32_t)crc32(crc, p, (uInt)n); }

const char *cook_fs_error(CookFs *fs) { return fs->err; }

CookFs *cook_fs_new(void) { return calloc(1, sizeof(CookFs)); }

void cook_fs_free(CookFs *fs)
{
    if (!fs)
        return;
    for (int i = 0; i < fs->nsrc; i++) {
        if (fs->src[i].f)
            fclose(fs->src[i].f);
        free(fs->src[i].path);
    }
    for (int i = 0; i < fs->nfiles; i++) {
        free(fs->files[i].key);
        free(fs->files[i].name);
        free(fs->files[i].path);
    }
    free(fs->files);
    free(fs->hash);
    free(fs);
}

static uint32_t hkey(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void rehash(CookFs *fs)
{
    free(fs->hash);
    fs->hcap = 1024;
    while (fs->hcap < fs->nfiles * 3)
        fs->hcap *= 2;
    fs->hash = calloc(fs->hcap, sizeof(int));
    for (int i = 0; i < fs->nfiles; i++) {
        uint32_t h = hkey(fs->files[i].key) & (fs->hcap - 1);
        while (fs->hash[h] && strcmp(fs->files[fs->hash[h] - 1].key, fs->files[i].key))
            h = (h + 1) & (fs->hcap - 1);
        fs->hash[h] = i + 1;   /* later files win */
    }
}

/* "Graphics\Pictures\Foo.PNG" -> "graphics/pictures/foo.png" */
static char *make_key(const char *p)
{
    char *k = cook_strdup(p);
    for (char *c = k; *c; c++) {
        if (*c == '\\')
            *c = '/';
        else if (*c >= 'A' && *c <= 'Z')
            *c += 32;
    }
    return k;
}

static CookFile *add_file(CookFs *fs, const char *relpath)
{
    if (fs->nfiles == fs->capfiles) {
        fs->capfiles = fs->capfiles ? fs->capfiles * 2 : 1024;
        fs->files = realloc(fs->files, fs->capfiles * sizeof(CookFile));
    }
    CookFile *f = &fs->files[fs->nfiles++];
    memset(f, 0, sizeof *f);
    f->key = make_key(relpath);
    const char *slash = strrchr(f->key, '/');
    const char *orig = relpath + (slash ? slash - f->key + 1 : 0);
    f->name = cook_strdup(orig);
    f->src = fs->nsrc;
    return f;
}

/* ------------------------------------------------------------------ folders */
static int walk(CookFs *fs, const char *dir, const char *rel, int depth)
{
    int n = 0;
#ifdef __PSP__
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return -1;
    SceIoDirent e;
    SceIoFatDirentPrivate fat;
    for (;;) {
        memset(&e, 0, sizeof e);
        memset(&fat, 0, sizeof fat);
        fat.size = sizeof fat;
        e.d_private = &fat;
        if (sceIoDread(d, &e) <= 0)
            break;
        const char *nm = e.d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        /* names outside ASCII cannot be opened by the firmware: open them by their 8.3 alias */
        int ascii = 1;
        for (const char *c = nm; *c; c++)
            if ((uint8_t)*c >= 128)
                ascii = 0;
        const char *open_nm = nm;
        if (!ascii && fat.s_name[0]) {
            /* the long name when the firmware can open it (emulators can), else the 8.3 alias */
            char lp[1024];
            SceIoStat lst;
            snprintf(lp, sizeof lp, "%s/%s", dir, nm);
            if (sceIoGetstat(lp, &lst) < 0)
                open_nm = fat.s_name;
        }
        int is_dir = FIO_S_ISDIR(e.d_stat.st_mode);
        uint32_t size = (uint32_t)e.d_stat.st_size;
#else
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name, *open_nm = nm;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        char full0[1024];
        snprintf(full0, sizeof full0, "%s/%s", dir, nm);
        struct stat st;
        if (stat(full0, &st))
            continue;
        int is_dir = S_ISDIR(st.st_mode);
        uint32_t size = (uint32_t)st.st_size;
#endif
        char full[1024], r[1024];
        snprintf(full, sizeof full, "%s/%s", dir, open_nm);
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", nm);
        if (is_dir) {
            if (depth < 4) {
                int k = walk(fs, full, r, depth + 1);
                if (k > 0)
                    n += k;
            }
            continue;
        }
        CookFile *f = add_file(fs, r);
        f->method = -2;
        f->size = f->csize = size;
        f->path = cook_strdup(full);
        n++;
    }
#ifdef __PSP__
    sceIoDclose(d);
#else
    closedir(d);
#endif
    return n;
}

int cook_fs_add_dir(CookFs *fs, const char *root)
{
    if (fs->nsrc == 8)
        return -1;
    int n = walk(fs, root, "", 0);
    if (n < 0) {
        snprintf(fs->err, sizeof fs->err, "missing folder %s", root);
        return -1;
    }
    fs->src[fs->nsrc].path = cook_strdup(root);
    fs->src[fs->nsrc++].kind = 0;
    rehash(fs);
    return n;
}

/* ------------------------------------------------------------------ zip */
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int cook_fs_add_zip(CookFs *fs, const char *path, const char *prefix)
{
    if (fs->nsrc == 8)
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(fs->err, sizeof fs->err, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    long tail = len < 65557 ? len : 65557;
    uint8_t *t = malloc(tail);
    fseek(f, len - tail, SEEK_SET);
    if (fread(t, 1, tail, f) != (size_t)tail) {
        free(t);
        fclose(f);
        return -1;
    }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(t + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        free(t);
        fclose(f);
        snprintf(fs->err, sizeof fs->err, "%s is not a ZIP archive", path);
        return -1;
    }
    uint32_t count = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
    long eocd_pos = len - tail + eocd;
    long shift = eocd_pos - (long)cd_size - (long)cd_off;   /* the self-extractor stub before the archive */
    free(t);
    uint8_t *cd = malloc(cd_size);
    fseek(f, cd_off + shift, SEEK_SET);
    if (!cd || fread(cd, 1, cd_size, f) != cd_size) {
        free(cd);
        fclose(f);
        return -1;
    }
    int n = 0;
    size_t plen = prefix ? strlen(prefix) : 0;
    uint32_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cd_size; i++) {
        if (rd32(cd + p) != 0x02014b50u)
            break;
        uint32_t method = rd16(cd + p + 10), crc = rd32(cd + p + 16), csize = rd32(cd + p + 20),
                 usize = rd32(cd + p + 24), nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32),
                 lho = rd32(cd + p + 42);
        char name[1024];
        uint32_t k = nl < sizeof name - 1 ? nl : sizeof name - 1;
        memcpy(name, cd + p + 46, k);
        name[k] = 0;
        p += 46 + nl + xl + cl;
        for (char *c = name; *c; c++)
            if (*c == '\\')
                *c = '/';
        if (!k || name[k - 1] == '/' || strstr(name, "../"))
            continue;
        if (plen && strncasecmp(name, prefix, plen))
            continue;
        /* the local header's name / extra lengths can differ from the central ones */
        uint8_t lh[30];
        fseek(f, lho + shift, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50u)
            continue;
        CookFile *e = add_file(fs, name);
        e->method = (int)method;
        e->crc = crc;
        e->csize = csize;
        e->size = usize;
        e->off = (uint32_t)(lho + shift + 30 + rd16(lh + 26) + rd16(lh + 28));
        n++;
    }
    free(cd);
    fs->src[fs->nsrc].path = cook_strdup(path);
    fs->src[fs->nsrc].f = f;
    fs->src[fs->nsrc++].kind = 1;
    rehash(fs);
    return n;
}

/* ------------------------------------------------------------------ RGSS3 (Game.rgss3a) */
int cook_fs_add_rgss3(CookFs *fs, const char *path)
{
    if (fs->nsrc == 8)
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(fs->err, sizeof fs->err, "cannot open %s", path);
        return -1;
    }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RGSSAD\0\3", 8)) {
        fclose(f);
        snprintf(fs->err, sizeof fs->err, "%s is not an RGSS3 archive", path);
        return -1;
    }
    uint32_t key = rd32(h + 8) * 9 + 3;
    uint8_t kb[4] = {key, key >> 8, key >> 16, key >> 24};
    int n = 0;
    for (;;) {
        uint8_t e[16];
        if (fread(e, 1, 16, f) != 16)
            break;
        uint32_t off = rd32(e) ^ key, size = rd32(e + 4) ^ key, fkey = rd32(e + 8) ^ key, nl = rd32(e + 12) ^ key;
        if (!off)
            break;
        if (nl > 1023) {
            fclose(f);
            snprintf(fs->err, sizeof fs->err, "%s: bad archive entry", path);
            return -1;
        }
        char name[1024];
        if (fread(name, 1, nl, f) != nl)
            break;
        for (uint32_t i = 0; i < nl; i++)
            name[i] ^= kb[i & 3];
        name[nl] = 0;
        if (strstr(name, ".."))
            continue;
        CookFile *c = add_file(fs, name);
        c->method = -1;
        c->off = off;
        c->size = c->csize = size;
        c->rkey = fkey;
        n++;
    }
    fs->src[fs->nsrc].path = cook_strdup(path);
    fs->src[fs->nsrc].f = f;
    fs->src[fs->nsrc++].kind = 2;
    rehash(fs);
    return n;
}

/* ------------------------------------------------------------------ lookup */
CookFile *cook_fs_find(CookFs *fs, const char *key)
{
    if (!fs->hcap)
        return 0;
    uint32_t h = hkey(key) & (fs->hcap - 1);
    while (fs->hash[h]) {
        CookFile *f = &fs->files[fs->hash[h] - 1];
        if (!strcmp(f->key, key))
            return f;
        h = (h + 1) & (fs->hcap - 1);
    }
    return 0;
}

static int filecmp(const void *a, const void *b) { return strcmp((*(CookFile *const *)a)->key, (*(CookFile *const *)b)->key); }

int cook_fs_list(CookFs *fs, const char *folder, CookFile ***out)
{
    size_t fl = strlen(folder);
    int n = 0;
    CookFile **v = malloc((fs->nfiles + 1) * sizeof *v);
    for (int i = 0; i < fs->nfiles; i++) {
        CookFile *f = &fs->files[i];
        if (strncmp(f->key, folder, fl) || cook_fs_find(fs, f->key) != f)
            continue;
        v[n++] = f;
    }
    qsort(v, n, sizeof *v, filecmp);
    *out = v;
    return n;
}

int cook_fs_files(CookFs *fs, int src, CookFile ***out)
{
    int n = 0;
    CookFile **v = malloc((fs->nfiles + 1) * sizeof *v);
    for (int i = 0; i < fs->nfiles; i++)
        if (fs->files[i].src == src)
            v[n++] = &fs->files[i];
    *out = v;
    return n;
}

void cook_fs_rekey(CookFs *fs, CookFile *f, const char *key)
{
    free(f->key);
    f->key = make_key(key);
    rehash(fs);
}

/* ------------------------------------------------------------------ reading */
#define CHUNK (256 * 1024)

int cook_fs_stream(CookFs *fs, CookFile *f, int (*sink)(void *u, const uint8_t *p, uint32_t n), void *u)
{
    Source *s = &fs->src[f->src];
    FILE *fp = s->f;
    int own = 0;
    if (f->method == -2) {
        fp = fopen(f->path, "rb");
        own = 1;
        if (!fp) {
            snprintf(fs->err, sizeof fs->err, "cannot open %.140s", f->path);
            return -1;
        }
    } else if (fseek(fp, f->off, SEEK_SET)) {
        return -1;
    }
    uint8_t *in = malloc(CHUNK), *outb = malloc(CHUNK);
    int rc = 0;
    uint32_t crc = 0, left = f->csize, total = 0;
    uint32_t key = f->rkey;
    if (f->method == 8) {
        z_stream z;
        memset(&z, 0, sizeof z);
        inflateInit2(&z, -MAX_WBITS);
        int zr = Z_OK;
        while (zr != Z_STREAM_END && rc == 0) {
            if (!z.avail_in) {
                uint32_t k = left < CHUNK ? left : CHUNK;
                if (!k || fread(in, 1, k, fp) != k) {
                    rc = -1;
                    break;
                }
                left -= k;
                z.next_in = in;
                z.avail_in = k;
            }
            z.next_out = outb;
            z.avail_out = CHUNK;
            zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) {
                rc = -1;
                break;
            }
            uint32_t got = CHUNK - z.avail_out;
            crc = cook_crc32(crc, outb, got);
            total += got;
            if (got && sink(u, outb, got))
                rc = -2;
        }
        inflateEnd(&z);
    } else {
        while (left && rc == 0) {
            uint32_t k = left < CHUNK ? left : CHUNK;
            if (fread(in, 1, k, fp) != k) {
                rc = -1;
                break;
            }
            left -= k;
            if (f->method == -1) {   /* RGSS3: each 32-bit word XOR a key that advances key = key * 7 + 3 */
                for (uint32_t i = 0; i < k; i += 4) {
                    uint8_t kb[4] = {key, key >> 8, key >> 16, key >> 24};
                    for (int j = 0; j < 4 && i + j < k; j++)
                        in[i + j] ^= kb[j];
                    key = key * 7 + 3;
                }
            }
            crc = cook_crc32(crc, in, k);
            total += k;
            if (sink(u, in, k))
                rc = -2;
        }
    }
    free(in);
    free(outb);
    if (own)
        fclose(fp);
    if (rc == 0 && f->method >= 0 && (crc != f->crc || total != f->size)) {
        snprintf(fs->err, sizeof fs->err, "%.120s is damaged (CRC)", f->key);
        rc = -1;
    }
    if (rc == -1 && !fs->err[0])
        snprintf(fs->err, sizeof fs->err, "cannot read %.120s", f->key);
    return rc;
}

typedef struct { uint8_t *p; uint32_t n, cap; } Buf;

static int buf_sink(void *u, const uint8_t *p, uint32_t n)
{
    Buf *b = u;
    if (b->n + n > b->cap)
        return 1;
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}

uint8_t *cook_fs_read(CookFs *fs, CookFile *f, uint32_t *size)
{
    Buf b = {malloc(f->size + 1), 0, f->size};
    if (!b.p)
        return 0;
    if (cook_fs_stream(fs, f, buf_sink, &b) || b.n != f->size) {
        free(b.p);
        return 0;
    }
    b.p[b.n] = 0;
    if (size)
        *size = b.n;
    return b.p;
}

/* fopen with a big stdio buffer: the PSP's default (1 KB) turns every small read or write into a memory stick
 * request, and stb_vorbis reads a FILE byte by byte */
FILE *cook_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f)
        setvbuf(f, 0, _IOFBF, 128 * 1024);
    return f;
}

/* a plain FILE for a file of a folder source (big audio is read from it in pieces), 0 for archive entries */
FILE *cook_fs_open_plain(CookFs *fs, CookFile *f)
{
    (void)fs;
    return f->method == -2 ? cook_fopen(f->path, "rb") : 0;
}
