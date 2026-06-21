/*
 * Mote — 2D sprite + tilemap rasteriser.
 */
#include "mote_2d.h"
#include "mote_tile.h"
#include <string.h>

static const MoteTilemap *s_map;
static const MoteTileset *s_tiles;
static int s_cam_x, s_cam_y;

/* Autotile layer: a logical terrain map + per-terrain rulesets, resolved to
 * atlas cells AT RENDER TIME (no resolved buffer — see mote_tile.h). */
static const uint8_t *s_terrain;
static int s_at_cols, s_at_rows, s_at_n, s_at_tw, s_at_th;
static const MoteAutotile *const *s_at_tiles;

static MoteSprite s_spr[MOTE_SCENE2D_MAX_SPRITES];
static int s_nspr;

void mote_scene2d_begin(int cam_x, int cam_y) {
    s_cam_x = cam_x; s_cam_y = cam_y;
    s_map = 0; s_tiles = 0;
    s_terrain = 0; s_at_n = 0;
    s_nspr = 0;
}

void mote_scene2d_clear(void) {
    s_map = 0; s_tiles = 0;
    s_terrain = 0; s_at_n = 0;
    s_nspr = 0;
}

void mote_scene2d_set_tilemap(const MoteTilemap *map, const MoteTileset *tiles) {
    s_map = map; s_tiles = tiles;
}

void mote_scene2d_set_autotiles(const uint8_t *terrain, int cols, int rows,
                                const MoteAutotile *const *tiles, int n) {
    s_terrain = terrain; s_at_cols = cols; s_at_rows = rows; s_at_tiles = tiles; s_at_n = n;
    s_at_tw = (n > 0 && tiles && tiles[0]) ? tiles[0]->tile_w : 8;
    s_at_th = (n > 0 && tiles && tiles[0]) ? tiles[0]->tile_h : 8;
}

int mote_scene2d_add(const MoteSprite *spr) {
    if (s_nspr >= MOTE_SCENE2D_MAX_SPRITES) return 0;
    s_spr[s_nspr++] = *spr;
    return 1;
}

int mote_scene2d_sprite_count(void) { return s_nspr; }

/* Blit a source rect of `img` to screen (x,y), clipped to band [y0,y1) and
 * the 128-wide screen, skipping colour-key pixels. */
MOTE_HOT
void mote_blit(uint16_t *fb, const MoteImage *img,
               int x, int y, int fx, int fy, int fw, int fh,
               uint8_t flags, int y0, int y1) {
    const uint16_t key = img->key;
    const int opaque = img->opaque;
    const int iw = img->w;
    for (int row = 0; row < fh; row++) {
        int sy = y + row;
        if (sy < y0 || sy >= y1 || sy >= MOTE_FB_H) continue;
        if (sy < 0) continue;
        int src_row = (flags & MOTE_SPR_VFLIP) ? (fh - 1 - row) : row;
        const uint16_t *srow = img->pixels + (fy + src_row) * iw + fx;
        uint16_t *drow = fb + sy * MOTE_FB_W;
        for (int col = 0; col < fw; col++) {
            int sx = x + col;
            if ((unsigned)sx >= MOTE_FB_W) continue;
            int src_col = (flags & MOTE_SPR_HFLIP) ? (fw - 1 - col) : col;
            uint16_t px = srow[src_col];
            if (opaque || px != key) drow[sx] = px;
        }
    }
}

static void draw_tilemap(uint16_t *fb, int y0, int y1) {
    if (!s_map || !s_tiles || !s_tiles->sheet) return;
    const int tw = s_tiles->tile_w, th = s_tiles->tile_h;
    const int tpr = s_tiles->sheet->w / (tw ? tw : 1);
    /* Tile rows overlapping the band. */
    int first_row = (y0 + s_cam_y) / th;
    int last_row  = (y1 - 1 + s_cam_y) / th;
    if (first_row < 0) first_row = 0;
    if (last_row >= s_map->rows) last_row = s_map->rows - 1;
    for (int r = first_row; r <= last_row; r++) {
        for (int c = 0; c < s_map->cols; c++) {
            int sx = c * tw - s_cam_x;
            if (sx <= -tw || sx >= MOTE_FB_W) continue;
            uint8_t idx = s_map->cells[r * s_map->cols + c];
            if (idx == 0xFF) continue;                  /* empty */
            int fx = (idx % tpr) * tw, fy = (idx / tpr) * th;
            mote_blit(fb, s_tiles->sheet, sx, r * th - s_cam_y,
                      fx, fy, tw, th, 0, y0, y1);
        }
    }
}

/* Render-time autotiling: per visible cell, build the neighbour mask from the
 * logical terrain map and pick the atlas cell via the ruleset LUT. */
static void draw_autotile(uint16_t *fb, int y0, int y1) {
    if (!s_terrain || s_at_n <= 0 || s_at_tw <= 0 || s_at_th <= 0) return;
    const int tw = s_at_tw, th = s_at_th;
    int first_row = (y0 + s_cam_y) / th;
    int last_row  = (y1 - 1 + s_cam_y) / th;
    if (first_row < 0) first_row = 0;
    if (last_row >= s_at_rows) last_row = s_at_rows - 1;
    for (int r = first_row; r <= last_row; r++) {
        for (int c = 0; c < s_at_cols; c++) {
            int sx = c * tw - s_cam_x;
            if (sx <= -tw || sx >= MOTE_FB_W) continue;
            uint8_t t = s_terrain[r * s_at_cols + c];
            if (t == 0 || t > s_at_n) continue;             /* empty / out of range */
            const MoteAutotile *at = s_at_tiles[t - 1];
            if (!at || !at->sheet) continue;
            int mask = mote_autotile_mask(s_terrain, s_at_cols, s_at_rows, c, r, t, at->edge_is_solid);
            uint8_t cell = at->lut[mask];
            int tpr = at->sheet->w / (at->tile_w ? at->tile_w : 1);
            int fx = (cell % tpr) * at->tile_w, fy = (cell / tpr) * at->tile_h;
            if (at->nvar > 1) fy += (int)(mote__at_hash(c, r) % at->nvar) * at->tile_h;   /* pick a random variant row */
            mote_blit(fb, at->sheet, sx, r * th - s_cam_y, fx, fy, at->tile_w, at->tile_h, 0, y0, y1);
        }
    }
}

MOTE_HOT
void mote_scene2d_raster(uint16_t *fb, int y0, int y1) {
    draw_tilemap(fb, y0, y1);
    draw_autotile(fb, y0, y1);

    /* Sprites in layer order (lower first). Read-only over s_spr so both
     * cores can raster the same list concurrently — no in-place sort. */
    int maxl = 0;
    for (int i = 0; i < s_nspr; i++) if (s_spr[i].layer > maxl) maxl = s_spr[i].layer;
    for (int pass = 0; pass <= maxl; pass++) {
        for (int i = 0; i < s_nspr; i++) {
            const MoteSprite *s = &s_spr[i];
            if (s->layer != pass) continue;
            mote_blit(fb, s->img, s->x - s_cam_x, s->y - s_cam_y,
                      s->fx, s->fy, s->fw, s->fh, s->flags, y0, y1);
        }
    }
}
