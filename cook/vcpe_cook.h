/* VCPE cook (4.5): the asset converters as plain C, so a game can build its data from the player's own copy of the
 * original on the device itself (first boot) instead of shipping converted game files. Same output as the Python
 * tools (tools/rpgmaker/vrpg.py, vrpg_assets.py, vcpe_stream.py), with C decoders (stb_image, stb_vorbis, minimp3)
 * in place of PIL / ffmpeg and a median-cut quantizer in place of imagequant.
 *
 * Platform-free: stdio and zlib only (the PSP's short-name lookup for non-ASCII file names is the one #ifdef).
 *
 *   CookFs *fs = cook_fs_new();
 *   cook_fs_add_dir(fs, "ms0:/PSP/GAME/X/INSTALL/game");     // later sources override earlier ones
 *   cook_fs_add_zip(fs, "ms0:/.../patch.exe", 0);           // a ZIP, also self-extracting (data after a stub)
 *   cook_fs_add_rgss3(fs, "ms0:/.../Game.rgss3a");          // an RGSS3 archive (decrypted while read)
 *   CookFile *f = cook_fs_find(fs, "data/system.rvdata2"); uint8_t *b = cook_fs_read(fs, f, &n);
 */
#ifndef VCPE_COOK_H
#define VCPE_COOK_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ------------------------------------------------------------------ files */
typedef struct CookFile {
    char *key;           /* lowercase "graphics/pictures/foo.png" ('/' separators, ASCII lowercased) */
    char *name;          /* the original spelling of the last part, without folders */
    int src;             /* source index */
    uint32_t off, csize, size, crc, rkey;
    int method;          /* zip: 0 stored, 8 deflate; rgss3: -1; dir: -2 (off unused, path in name of source) */
    char *path;          /* dir sources: the path to open */
} CookFile;

typedef struct CookFs CookFs;
CookFs *cook_fs_new(void);
void cook_fs_free(CookFs *fs);
int cook_fs_add_dir(CookFs *fs, const char *root);          /* number of files, <0 when the folder is missing */
int cook_fs_add_zip(CookFs *fs, const char *path, const char *prefix);   /* prefix kept (e.g. "Audio/"), 0 = all */
int cook_fs_add_rgss3(CookFs *fs, const char *path);
CookFile *cook_fs_find(CookFs *fs, const char *key);        /* the winning (last added) file */
/* the winning files under folder ("graphics/" or "audio/se/"), sorted by key (UTF-8 byte order) */
int cook_fs_list(CookFs *fs, const char *folder, CookFile ***out);
uint8_t *cook_fs_read(CookFs *fs, CookFile *f, uint32_t *size);   /* malloc'd (+1 NUL byte), 0 on failure */
/* stream a file to a callback in pieces (big zip entries without holding them in memory); checks the CRC */
int cook_fs_stream(CookFs *fs, CookFile *f, int (*sink)(void *u, const uint8_t *p, uint32_t n), void *u);
uint32_t cook_crc32(uint32_t crc, const void *p, size_t n);
const char *cook_fs_error(CookFs *fs);
FILE *cook_fopen(const char *path, const char *mode);   /* fopen + a 128 KB buffer */
FILE *cook_fs_open_plain(CookFs *fs, CookFile *f);   /* folder sources only (caller fcloses), else 0 */
int cook_fs_files(CookFs *fs, int src, CookFile ***out);   /* every file of source src (malloc'd list) */
void cook_fs_rekey(CookFs *fs, CookFile *f, const char *key);   /* rename (a garbled name recovered) */

/* ------------------------------------------------------------------ Ruby Marshal 4.8 */
enum { RB_NIL, RB_TRUE, RB_FALSE, RB_INT, RB_FLOAT, RB_STR, RB_SYM, RB_ARRAY, RB_HASH, RB_OBJECT, RB_USER };
typedef struct RbNode RbNode;
struct RbNode {
    uint8_t type;
    int64_t i;            /* RB_INT */
    double f;             /* RB_FLOAT */
    const char *s;        /* RB_STR / RB_SYM bytes (NUL-terminated copy); RB_OBJECT / RB_USER class name */
    uint32_t len;         /* string length; array / hash / object entry count */
    RbNode **items;       /* array items; hash keys then values (2 * len); object: values */
    const char **names;   /* object attribute names ("@name") */
};
typedef struct RbDoc RbDoc;
RbDoc *rb_load(const uint8_t *data, uint32_t size, char *err, int errcap);   /* 0 on failure */
RbNode *rb_root(RbDoc *d);
void rb_free(RbDoc *d);
RbNode *rb_attr(const RbNode *o, const char *name);   /* "@name"; 0 when missing or not an object */
RbNode *rb_get(const RbNode *o, const char *field);    /* attr "@field" */
int64_t rb_int(const RbNode *n);                        /* nil/false 0, true 1, float truncated, int clamped */
double rb_num(const RbNode *n);

/* ------------------------------------------------------------------ VRPG documents (vrpg.py) */
typedef struct { uint8_t *data; uint32_t size; } CookBlob;
typedef struct CookFs CookFs;
/* the database document from data/<name>.rvdata2; maps: one document per map id (blob.size 0 = no map) */
int cook_vrpg_db(CookFs *fs, CookBlob *out, char *err, int errcap);
int cook_vrpg_maps(CookFs *fs, CookBlob **maps, int *max_map, char *err, int errcap);

/* ------------------------------------------------------------------ sprites (vrpg_assets.Assets.sprites) */
typedef struct {
    const char *folder;   /* lowercase folder under graphics/ */
    float scale;          /* stored size / original (big art stored smaller; the runtime scales back) */
    float dither;         /* 0..1, for images quantized to 255 colours */
} CookImageRule;
typedef struct { const char *folder, *name; int hue; } CookHue;
typedef struct {
    char *key;            /* "pictures/foo" or "battlers/slime@120" */
    int id;
    float scale;
} CookImage;
typedef struct {
    const CookImageRule *rules; int nrules;
    const CookHue *hues; int nhues;
    const char *tmp_dir;                  /* page spill files and the checkpoint */
    void (*progress)(int done, int total);
} CookSpriteOpts;
/* writes the VSPR section to out at its current position; images sorted by key (UTF-8 byte order) */
int cook_sprites(CookFs *fs, const CookSpriteOpts *o, FILE *out, uint32_t *section_size, CookImage **images,
                 int *nimages, char *err, int errcap);
int cook_windowskin_colors(CookFs *fs, const char *key, uint32_t colors[32]);

/* ------------------------------------------------------------------ audio (vrpg_assets sound_effects / streams) */
typedef struct { char *key; uint32_t off, size, samples; } CookSound;
typedef struct { char *key; uint32_t intro_off, intro_samples, loop_off, loop_samples; uint16_t rate; uint8_t ch; } CookTrack;
typedef struct { const char *folder; int rate, channels, loop; } CookStreamRule;
/* mono ADPCM sound effects of folder (e.g. "se") into one blob */
int cook_sounds(CookFs *fs, const char *folder, int rate, uint8_t **blob, uint32_t *size, CookSound **tab, int *n,
                void (*progress)(int, int), char *err, int errcap);
/* streamed tracks of the rules' folders appended to out (offsets = base + bytes written so far); resumable: tracks
 * already in *tab (*n of them) are kept and the file is expected at that length */
int cook_streams(CookFs *fs, const CookStreamRule *rules, int nrules, FILE *out, uint32_t base, CookTrack **tab,
                 int *n, void (*checkpoint)(void *u, int n), void *u, void (*progress)(int, int), char *err,
                 int errcap);

/* IMA ADPCM blocks of 1024 samples, the vcpe_audio layout (adpcm_enc.c) */
typedef struct { int pred, index; } CookAdpcm;
void cook_adpcm_block(CookAdpcm *st, int ch, const int16_t *pcm, int frames, uint8_t *out);   /* frames <= 1024 */
#define COOK_ADPCM_BLOCK 1024
#define COOK_ADPCM_BYTES(ch) ((ch) * (4 + COOK_ADPCM_BLOCK / 2))

/* SHA-256 (for checking an input is the expected one) */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } CookSha256;
void cook_sha256_init(CookSha256 *s);
void cook_sha256_update(CookSha256 *s, const void *p, size_t n);
void cook_sha256_final(CookSha256 *s, uint8_t out[32]);

/* the long loops call this between files (redraw a progress screen, keep the console awake); nonzero = stop */
extern int (*cook_yield)(void);
/* a live feed of what is being done ("image", "graphics/battlers/slime"), for an install screen; may be 0 */
extern void (*cook_feed)(const char *verb, const char *name);
#define COOK_FEED(v, n) do { if (cook_feed) cook_feed((v), (n)); } while (0)

/* 0..1 per-stage progress of the current file, for the progress bar */
char *cook_strdup(const char *s);
int cook_keycmp(const void *a, const void *b);   /* qsort comparator over char* (UTF-8 byte order) */
#endif
