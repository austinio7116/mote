/*
 * Mote — 2D sprite + tilemap subsystem, alongside the 3D engine.
 *
 * A screen-space 2D scene the OS rasters AFTER the 3D scene (both banded
 * across cores), so a game can be pure-2D, pure-3D, or hybrid (3D world +
 * 2D HUD/sprites on top). The engine owns the sprite list — layers, frames,
 * flips — and composes it; games manipulate sprite structs, not raw blits.
 *
 * Images are RGB565 in flash (baked by tools/img2tex, or defined in code).
 * Transparency is a colour key (default magenta 0xF81F, matching the engine's
 * sprite convention).
 */
#ifndef MOTE_2D_H
#define MOTE_2D_H

#include <stdint.h>
#include "mote_config.h"

#define MOTE_KEY_MAGENTA 0xF81Fu     /* default transparent colour key */

/* A pixel rectangle in flash (sprite sheet / tileset atlas / single image). */
typedef struct {
    const uint16_t *pixels;   /* format 0: w*h RGB565, row-major. NULL when indexed. */
    uint16_t w, h;
    uint16_t key;             /* transparent colour key (ignored when opaque) */
    uint8_t  opaque;          /* 1 = no transparency: draw every pixel, ignore key.
                               * Defaults to 0 for older {px,w,h,key} initialisers. */
    /* --- ABI v41: optional palette-indexed pixels — a hand-painted texture with few
     * colours costs 1/4 (4bpp) or 1/2 (8bpp) the flash of RGB565, decoded to RGB565 by a
     * palette lookup at sample time. Both NULL / format 0 on a plain RGB565 image, so
     * older {px,w,h,key[,opaque]} initialisers keep working unchanged. */
    uint8_t  format;          /* 0 = RGB565 (pixels) · 1 = 4bpp indexed · 2 = 8bpp indexed */
    const uint8_t  *indices;  /* packed palette indices — format 1: 2 per byte, high nibble first; format 2: 1 per byte */
    const uint16_t *palette;  /* RGB565 palette — 16 entries (format 1) or up to 256 (format 2) */
} MoteImage;

/* Fetch one texel as RGB565, transparently handling RGB565 or palette-indexed images.
 * Hot raster loops branch on `format` once (it's the same for the whole primitive) so the
 * predictor hides the cost; the palette lookup is one extra read for indexed textures. */
static inline uint16_t mote_img_texel(const MoteImage *img, int u, int v) {
    unsigned i = (unsigned)v * img->w + (unsigned)u;
    if (!img->format) return img->pixels[i];
    if (img->format == 1) { uint8_t b = img->indices[i >> 1]; return img->palette[(i & 1u) ? (b & 0x0Fu) : (unsigned)(b >> 4)]; }
    return img->palette[img->indices[i]];
}

/* --- ABI v39: anti-aliased proportional bitmap font ---------------------------
 * Coverage is packed at `bpp` bits/pixel (MSB-first, row-major, byte-aligned per
 * glyph): the baker picks the smallest depth that's lossless — 1-bit for a binary
 * font (no AA), 2-bit for ≤4 levels, else 4-bit (16 levels). A value v decodes to
 * alpha = v*255/((1<<bpp)-1): 0 = transparent .. max = solid. TTF edges blend cleanly
 * at any size; a hand-drawn glyph paints any level. Baked by ttf2font / glyphs2font /
 * the Studio Font tab. Draw via mote->text_font(). Codepoints `first`..`first+count-1`. */
typedef struct {
    uint8_t  adv;        /* pen advance after this glyph, in pixels */
    uint8_t  w, h;       /* coverage bitmap size (0x0 for blank glyphs like space) */
    int8_t   xoff, yoff; /* bitmap top-left relative to the pen x / text-box top y */
    uint32_t off;        /* byte offset of this glyph's packed w*h coverage in cov[] */
} MoteGlyph;
typedef struct {
    const uint8_t   *cov;     /* packed coverage for every glyph, back-to-back */
    const MoteGlyph *glyphs;  /* `count` entries */
    uint16_t count;           /* number of glyphs */
    uint8_t  first;           /* first codepoint (usually 32 = space) */
    uint8_t  line_h;          /* line advance for '\n' / multi-line layout, in pixels */
    uint8_t  bpp;             /* bits/pixel of cov[]: 1, 2 or 4 (0 read as 4 for compat) */
} MoteFont;

/* A tileset: an atlas image divided into tile_w x tile_h cells, indexed
 * left-to-right, top-to-bottom. */
typedef struct {
    const MoteImage *sheet;
    uint16_t tile_w, tile_h;
} MoteTileset;

/* A tilemap: cols*rows byte indices into a tileset (0xFF = empty cell). */
typedef struct {
    const uint8_t *cells;
    uint16_t cols, rows;
} MoteTilemap;

/* Sprite flags. Bits 2-3 hold a clockwise rotation (square tiles only); rotation is
 * applied before the flips. */
#define MOTE_SPR_HFLIP  0x01
#define MOTE_SPR_VFLIP  0x02
#define MOTE_SPR_ROT90  0x04
#define MOTE_SPR_ROT180 0x08
#define MOTE_SPR_ROT270 0x0C
#define MOTE_SPR_ROT_MASK 0x0C

/* A sprite instance: a frame of an image at a screen position. The sprite's
 * frame rect (fx,fy,fw,fh) selects a cell from `img` (sheet); for a single
 * image use fx=fy=0, fw=img->w, fh=img->h. */
typedef struct {
    const MoteImage *img;
    int16_t  x, y;            /* world position (camera-relative applied at raster) */
    uint16_t fx, fy, fw, fh;  /* source frame rect in img */
    uint8_t  layer;           /* draw order: lower first */
    uint8_t  flags;           /* MOTE_SPR_* */
} MoteSprite;

#define MOTE_SCENE2D_MAX_SPRITES 128

/* Frame flow (core0): begin -> set tilemap -> add sprites; then BOTH cores
 * raster their band. */
void mote_scene2d_begin(int cam_x, int cam_y);
void mote_scene2d_clear(void);   /* empty the 2D scene (OS calls each frame) */
void mote_scene2d_set_tilemap(const MoteTilemap *map, const MoteTileset *tiles);
struct MoteAutotile;
void mote_scene2d_set_autotiles(const uint8_t *terrain, int cols, int rows,
                                const struct MoteAutotile *const *tiles, int n);
void mote_scene2d_set_autotile_layers(const uint8_t *map, int cols, int rows,
                                      const struct MoteAutotile *const *tiles, int n);   /* bit-packed layers */
int  mote_scene2d_add(const MoteSprite *spr);     /* returns 0 if full */
void mote_scene2d_raster(uint16_t *fb, int y0, int y1);
int  mote_scene2d_sprite_count(void);

/* Low-level blit (band-clipped, colour-keyed). Used by the scene; also handy
 * for immediate-mode overlay drawing. */
void mote_blit(uint16_t *fb, const MoteImage *img,
               int x, int y, int fx, int fy, int fw, int fh,
               uint8_t flags, int y0, int y1);

/* Free rotate + uniform-scale blit, centred at (cx,cy) in framebuffer pixels.
 * angle in radians, scale 1.0 = original size. fx/fy/fw/fh select a source
 * sub-rect (0 width/height = the whole image). Colour-keyed; blend =
 * MOTE_BLEND_* (NONE/ALPHA/ADD). Immediate-mode (no scene state). */
void mote_blit_ex(uint16_t *fb, const MoteImage *img,
                  float cx, float cy, int fx, int fy, int fw, int fh,
                  float angle, float scale, uint8_t blend, int y0, int y1);

/* Immediate-mode 2D framebuffer drawing (screen space, RGB565, no depth). For
 * HUDs/overlays and background passes. yc0/yc1 bound the rows written: pass the
 * band [y0,y1) inside a background/render callback (dual-core), or 0..MOTE_FB_H
 * in overlay(). draw_pixel is bounds-checked to the screen. */
void mote_draw_pixel(uint16_t *fb, int x, int y, uint16_t color);
void mote_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1,
                    uint16_t color, int yc0, int yc1);
void mote_draw_rect(uint16_t *fb, int x, int y, int w, int h,
                    uint16_t color, int fill, int yc0, int yc1);
void mote_draw_circle(uint16_t *fb, int cx, int cy, int r,
                      uint16_t color, int fill, int yc0, int yc1);

#endif /* MOTE_2D_H */
