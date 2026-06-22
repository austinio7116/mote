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
    const uint16_t *pixels;   /* w*h RGB565, row-major */
    uint16_t w, h;
    uint16_t key;             /* transparent colour key (ignored when opaque) */
    uint8_t  opaque;          /* 1 = no transparency: draw every pixel, ignore key.
                               * Defaults to 0 for older {px,w,h,key} initialisers. */
} MoteImage;

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

/* Sprite flags. */
#define MOTE_SPR_HFLIP 0x01
#define MOTE_SPR_VFLIP 0x02

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

#endif /* MOTE_2D_H */
