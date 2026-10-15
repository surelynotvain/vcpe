/* VCPE sprites: 2D images with their own palette (T4 / T8 CLUT per image) on shared 256x256 pages, pages loaded
 * from the data PAK on first use and kept in an LRU cache with a memory budget. Draws follow GameMaker rules:
 * origin, frame index wraps, x/y scale (negative flips), rotation in degrees counter-clockwise, colour + alpha in
 * the vertex colour (0xAABBGGRR). Bitmap fonts (glyph rectangles on one image) draw text.
 * The converter (tools/gm/gmtex.py + the game's packer) writes the "VSPR" section this module reads. */
#ifndef VCPE_SPR_H
#define VCPE_SPR_H
#include <stdint.h>

typedef struct {
    uint16_t w, h;          /* frame size */
    int16_t ox, oy;         /* origin */
    int16_t bl, bt, br, bb; /* collision box (inclusive, frame space) */
    uint16_t nframes;
    uint8_t bpp, flags;     /* flags 1: precise mask in the game data */
    uint32_t first_frame;
    uint32_t pal;           /* palette offset in the palette blob */
} VcpeSprImage;
typedef struct { uint32_t first_piece; uint16_t npieces, pad; } VcpeSprFrame;
typedef struct { uint16_t page; uint8_t u, v; uint16_t w, h; int16_t dx, dy; } VcpeSprPiece;
typedef struct { uint32_t off; uint8_t bpp, pad[3]; } VcpeSprPage;
typedef struct { uint16_t ch, x, y, w, h; int16_t shift, off; uint16_t pad; } VcpeSprGlyph;
typedef struct { uint16_t image, first, last, nglyphs; uint32_t glyph0; int16_t height, pad; } VcpeSprFont;

typedef struct {
    char magic[4];
    uint32_t nimages, nframes, npieces, npages, nfonts, nglyphs;
    uint32_t images, frames, pieces, pages, fonts, glyphs, pal, pal_size, page_data;   /* offsets in the section */
} VcpeSprHeader;

/* section_off: where the VSPR section starts in the opened PAK; budget: bytes of page cache */
int vcpe_spr_init(uint32_t section_off, uint32_t budget);
int vcpe_spr_count(void);
const VcpeSprImage *vcpe_spr_image(int img);
void vcpe_spr_frame_begin(void);          /* once per frame before drawing (LRU clock) */
void vcpe_spr_preload(int img);           /* load all pages of an image now (loading screens) */
/* read every page into one block (when the budget allows it): no loads or evictions while playing.
 * progress(done, total) is called between reads (e.g. to draw a loading bar). 0 ok, <0 failed (pages stay lazy).
 * 4.0: when no single block is free (fragmented heap), every page gets its own allocation instead */
int vcpe_spr_load_all(void (*progress)(int done, int total));
int vcpe_spr_resident_kb(void);
extern int vcpe_spr_loads;                    /* page loads since start (perf lines) */
extern int vcpe_spr_fails;                    /* 4.0: pages that could not be loaded (drawn as nothing) */
/* 4.0: the page cache in fixed slots inside one block instead of one malloc per page: no heap traffic while playing
 * and a bounded worst case (PSP-1000). mem 0: the engine allocates size bytes now; else the game's memory (64-byte
 * aligned, e.g. part of the volatile partition, never freed by the engine). A slot holds the largest page (64 KB with
 * any 8-bit page, else 32 KB), so size / slot pages stay resident; eviction is least recently drawn first, never a
 * page drawn this frame. size 0 goes back to malloc per page. Returns the slot count, -1 if it can't. Call it
 * between frames, after vcpe_spr_init; vcpe_spr_load_all replaces it */
int vcpe_spr_arena(void *mem, uint32_t size);

/* the camera: room coordinates minus (x, y) = screen */
void vcpe_spr_view(float x, float y);
void vcpe_spr_additive(int on);
void vcpe_spr_flash(int on);
/* an image's palette (RGBA 0xAABBGGRR entries, index 0 transparent) and a substitute palette for the next draws
 * (palette swaps: 64-byte aligned, same size, kept alive until the frame is drawn; 0 restores the image's own) */
const uint32_t *vcpe_spr_clut(int img, int *count);
void vcpe_spr_clut_override(const uint32_t *clut);            /* 1: draw as a silhouette in the vertex colour (hit flash) */

void vcpe_spr_draw(int img, int frame, float x, float y, float xs, float ys, float angle, uint32_t color);
/* a rectangle of the frame (left, top, w, h in frame pixels) with its top-left at (x, y) */
void vcpe_spr_draw_part(int img, int frame, int left, int top, int w, int h, float x, float y, float xs, float ys,
                        uint32_t color);
/* Rotated source rectangle; (x,y) is its center. Negative scale mirrors. */
void vcpe_spr_draw_part_ex(int img,int frame,int left,int top,int w,int h,float x,float y,float xs,float ys,float angle,uint32_t color);
/* repeat the image to cover the screen: tile_x / tile_y 0 draws a single copy on that axis */
void vcpe_spr_draw_tiled(int img, int frame, float x, float y, int tile_x, int tile_y, float xs, float ys,
                         uint32_t color);

/* the drawn (non-transparent) rectangle of a frame, [x0,x1) x [y0,y1) in frame pixels; 0 when the frame is empty.
 * GameMaker sprite fonts (font_add_sprite, proportional) space glyphs by it */
int vcpe_spr_frame_box(int img, int frame, int *x0, int *y0, int *x1, int *y1);

/* text with a converted GameMaker font; halign 0 left 1 centre 2 right, valign 0 top 1 middle 2 bottom.
 * '#' and '\n' break lines like GameMaker */
const VcpeSprFont *vcpe_spr_font(int f);
float vcpe_spr_text_width(int font, const char *s, float scale);
float vcpe_spr_text_height(int font, const char *s, float scale);
void vcpe_spr_text(int font, float x, float y, const char *s, int halign, int valign, float scale, uint32_t color);

#endif
