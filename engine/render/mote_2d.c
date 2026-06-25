/*
 * Mote — 2D sprite + tilemap rasteriser.
 */
#include "mote_2d.h"
#include "mote_tile.h"
#include "mote_object.h"   /* MOTE_BLEND_* */
#include <string.h>
#include <math.h>

/* Per-pixel source/destination combine (shared by the rotate/scale blit). */
static inline uint16_t blit_blend565(uint16_t dst, uint16_t src, uint8_t mode) {
    if (mode == MOTE_BLEND_ALPHA) {            /* 50/50 mix, RGB565 */
        return (uint16_t)(((dst & 0xF7DE) >> 1) + ((src & 0xF7DE) >> 1));
    }
    if (mode == MOTE_BLEND_ADD) {              /* saturating per-channel add */
        int r = ((dst >> 11) & 0x1F) + ((src >> 11) & 0x1F);
        int g = ((dst >> 5) & 0x3F) + ((src >> 5) & 0x3F);
        int b = (dst & 0x1F) + (src & 0x1F);
        if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
    return src;
}

static const MoteTilemap *s_map;
static const MoteTileset *s_tiles;
static int s_cam_x, s_cam_y;

/* Autotile layer: a logical terrain map + per-terrain rulesets, resolved to
 * atlas cells AT RENDER TIME (no resolved buffer — see mote_tile.h). */
static const uint8_t *s_terrain;
static int s_at_cols, s_at_rows, s_at_n, s_at_tw, s_at_th;
static const MoteAutotile *const *s_at_tiles;

/* Layered autotiling: one bitmask map (bit L = layer L occupied) + a ruleset per
 * layer, drawn bottom-up, each layer autotiled against its own bit. */
static const uint8_t *s_lay_map;
static int s_lay_cols, s_lay_rows, s_lay_n, s_lay_tw, s_lay_th;
static const MoteAutotile *const *s_lay_tiles;

static MoteSprite s_spr[MOTE_SCENE2D_MAX_SPRITES];
static int s_nspr;

void mote_scene2d_begin(int cam_x, int cam_y) {
    s_cam_x = cam_x; s_cam_y = cam_y;
    s_map = 0; s_tiles = 0;
    s_terrain = 0; s_at_n = 0;
    s_lay_map = 0; s_lay_n = 0;
    s_nspr = 0;
}

void mote_scene2d_clear(void) {
    s_map = 0; s_tiles = 0;
    s_terrain = 0; s_at_n = 0;
    s_lay_map = 0; s_lay_n = 0;
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

void mote_scene2d_set_autotile_layers(const uint8_t *map, int cols, int rows,
                                      const MoteAutotile *const *tiles, int n) {
    s_lay_map = map; s_lay_cols = cols; s_lay_rows = rows; s_lay_tiles = tiles; s_lay_n = n;
    s_lay_tw = (n > 0 && tiles && tiles[0]) ? tiles[0]->tile_w : 8;
    s_lay_th = (n > 0 && tiles && tiles[0]) ? tiles[0]->tile_h : 8;
}

int mote_scene2d_add(const MoteSprite *spr) {
    if (!spr || !spr->img || s_nspr >= MOTE_SCENE2D_MAX_SPRITES) return 0;   /* fail soft on a bad sprite */
    s_spr[s_nspr++] = *spr;
    return 1;
}

int mote_scene2d_sprite_count(void) { return s_nspr; }

/* Blit a source rect of `img` to screen (x,y), clipped to band [y0,y1) and
 * the 128-wide screen, skipping colour-key pixels. */
/* Rotation path (square cell: fw == fh == n). Kept OUT of the RAM-resident fast blit —
 * rotated tiles are uncommon, and a flash call here keeps mote_blit small. */
__attribute__((noinline))
static void mote_blit_rot(uint16_t *fb, const MoteImage *img,
                          int x, int y, int fx, int fy, int fw, int fh,
                          uint8_t flags, int y0, int y1, int rot) {
    const uint16_t key = img->key;
    const int opaque = img->opaque, iw = img->w, n = fw;
    for (int row = 0; row < fh; row++) {
        int sy = y + row;
        if (sy < y0 || sy >= y1 || sy >= MOTE_FB_H || sy < 0) continue;
        uint16_t *drow = fb + sy * MOTE_FB_W;
        for (int col = 0; col < fw; col++) {
            int sx = x + col;
            if ((unsigned)sx >= MOTE_FB_W) continue;
            int sc, sr;
            switch (rot) {
                case 1:  sc = row;         sr = n - 1 - col; break;   /* 90 CW  */
                case 2:  sc = n - 1 - col; sr = n - 1 - row; break;   /* 180    */
                default: sc = n - 1 - row; sr = col;         break;   /* 270 CW */
            }
            if (flags & MOTE_SPR_HFLIP) sc = n - 1 - sc;
            if (flags & MOTE_SPR_VFLIP) sr = n - 1 - sr;
            uint16_t px = img->pixels[(fy + sr) * iw + fx + sc];
            if (opaque || px != key) drow[sx] = px;
        }
    }
}

MOTE_HOT
void mote_blit(uint16_t *fb, const MoteImage *img,
               int x, int y, int fx, int fy, int fw, int fh,
               uint8_t flags, int y0, int y1) {
    if (flags & MOTE_SPR_ROT_MASK) { mote_blit_rot(fb, img, x, y, fx, fy, fw, fh, flags, y0, y1, (flags & MOTE_SPR_ROT_MASK) >> 2); return; }
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

/* Free rotate + uniform-scale blit, centred at (cx,cy) in framebuffer pixels.
 * Inverse-transform sampling (nearest neighbour): walk the destination AABB and
 * map each pixel back into the source rect, so there are no gaps. Colour-keyed,
 * with optional alpha/additive blend. Immediate-mode — no scene state. */
__attribute__((noinline))
void mote_blit_ex(uint16_t *fb, const MoteImage *img,
                  float cx, float cy, int fx, int fy, int fw, int fh,
                  float angle, float scale, uint8_t blend, int y0, int y1) {
    if (!img || scale <= 0.0f) return;
    if (fw <= 0) fw = img->w;
    if (fh <= 0) fh = img->h;
    const uint16_t key = img->key;
    const int opaque = img->opaque, iw = img->w;
    float s = sinf(angle), c = cosf(angle);
    /* Destination AABB half-extents of the rotated, scaled source rect. */
    float hw = fw * 0.5f, hh = fh * 0.5f;
    float ahw = (fabsf(c) * hw + fabsf(s) * hh) * scale;
    float ahh = (fabsf(s) * hw + fabsf(c) * hh) * scale;
    int x0 = (int)floorf(cx - ahw), x1 = (int)ceilf(cx + ahw);
    int yb0 = (int)floorf(cy - ahh), yb1 = (int)ceilf(cy + ahh);
    if (x0 < 0) x0 = 0;
    if (x1 > MOTE_FB_W) x1 = MOTE_FB_W;
    if (yb0 < y0) yb0 = y0;
    if (yb0 < 0) yb0 = 0;
    if (yb1 > y1) yb1 = y1;
    if (yb1 > MOTE_FB_H) yb1 = MOTE_FB_H;
    float inv = 1.0f / scale;
    for (int py = yb0; py < yb1; py++) {
        float dy = py - cy;
        uint16_t *drow = fb + py * MOTE_FB_W;
        for (int px = x0; px < x1; px++) {
            float dx = px - cx;
            /* inverse-rotate then unscale, into source-rect coords */
            float u = (c * dx + s * dy) * inv + hw;
            float v = (-s * dx + c * dy) * inv + hh;
            if (u < 0.0f || v < 0.0f) continue;
            int su = (int)u, sv = (int)v;
            if (su >= fw || sv >= fh) continue;
            uint16_t sp = img->pixels[(fy + sv) * iw + fx + su];
            if (!opaque && sp == key) continue;
            drow[px] = blend ? blit_blend565(drow[px], sp, blend) : sp;
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
__attribute__((noinline))
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
            /* variant rows: the sheet is base_rows tall per variant (grid layout), so a
             * variant steps a whole base block, not one row. base_rows=1 for 47x1 sheets. */
            int nv = at->nvar < 1 ? 1 : at->nvar, base_rows = (at->sheet->h / at->tile_h) / nv;
            fy += mote__at_variant(at, c, r) * base_rows * at->tile_h;
            mote_blit(fb, at->sheet, sx, r * th - s_cam_y, fx, fy, at->tile_w, at->tile_h, at->xform[mask], y0, y1);
        }
    }
}

/* Layered autotiling: bit-packed map, each layer (bit) drawn bottom-up, autotiled
 * against its own bit, so layers overlap and composite. noinline keeps this in flash
 * (the hot inner loop is mote_blit, which stays RAM-resident). */
__attribute__((noinline))
static void draw_autotile_layers(uint16_t *fb, int y0, int y1) {
    if (!s_lay_map || s_lay_n <= 0 || s_lay_tw <= 0 || s_lay_th <= 0) return;
    const int tw = s_lay_tw, th = s_lay_th;
    int first_row = (y0 + s_cam_y) / th;
    int last_row  = (y1 - 1 + s_cam_y) / th;
    if (first_row < 0) first_row = 0;
    if (last_row >= s_lay_rows) last_row = s_lay_rows - 1;
    for (int L = 0; L < s_lay_n && L < 8; L++) {
        const MoteAutotile *at = s_lay_tiles[L];
        if (!at || !at->sheet) continue;
        int tpr = at->sheet->w / (at->tile_w ? at->tile_w : 1);
        int nv = at->nvar < 1 ? 1 : at->nvar, base_rows = (at->sheet->h / at->tile_h) / nv;
        for (int r = first_row; r <= last_row; r++) {
            for (int c = 0; c < s_lay_cols; c++) {
                if (!((s_lay_map[r * s_lay_cols + c] >> L) & 1u)) continue;
                int sx = c * tw - s_cam_x;
                if (sx <= -tw || sx >= MOTE_FB_W) continue;
                int mask = mote_autotile_mask_layer(s_lay_map, s_lay_cols, s_lay_rows, c, r, L, at->edge_is_solid);
                uint8_t cell = at->lut[mask];
                int fx = (cell % tpr) * at->tile_w, fy = (cell / tpr) * at->tile_h;
                fy += mote__at_variant(at, c, r) * base_rows * at->tile_h;
                mote_blit(fb, at->sheet, sx, r * th - s_cam_y, fx, fy, at->tile_w, at->tile_h, at->xform[mask], y0, y1);
            }
        }
    }
}

MOTE_HOT
void mote_scene2d_raster(uint16_t *fb, int y0, int y1) {
    draw_tilemap(fb, y0, y1);
    draw_autotile(fb, y0, y1);
    draw_autotile_layers(fb, y0, y1);

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

/* ---- immediate-mode 2D framebuffer drawing (see mote_2d.h) -------------- */
void mote_draw_pixel(uint16_t *fb, int x, int y, uint16_t color) {
    if ((unsigned)x < MOTE_FB_W && (unsigned)y < MOTE_FB_H) fb[y * MOTE_FB_W + x] = color;
}

void mote_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1,
                    uint16_t color, int yc0, int yc1) {
    if (yc0 < 0) yc0 = 0;
    if (yc1 > MOTE_FB_H) yc1 = MOTE_FB_H;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady);
    if (steps < 1) steps = 1;
    float inv = 1.0f / (float)steps;
    float px = (float)x0, py = (float)y0, sx = dx * inv, sy = dy * inv;
    for (int i = 0; i <= steps; i++, px += sx, py += sy) {
        int ix = (int)px, iy = (int)py;
        if (iy >= yc0 && iy < yc1 && (unsigned)ix < MOTE_FB_W) fb[iy * MOTE_FB_W + ix] = color;
    }
}

void mote_draw_rect(uint16_t *fb, int x, int y, int w, int h,
                    uint16_t color, int fill, int yc0, int yc1) {
    if (yc0 < 0) yc0 = 0;
    if (yc1 > MOTE_FB_H) yc1 = MOTE_FB_H;
    int x1 = x + w, y1 = y + h;
    if (fill) {
        int xa = x < 0 ? 0 : x, xb = x1 > MOTE_FB_W ? MOTE_FB_W : x1;
        int ya = y < yc0 ? yc0 : y, yb = y1 > yc1 ? yc1 : y1;
        for (int j = ya; j < yb; j++) {
            uint16_t *row = fb + j * MOTE_FB_W;
            for (int i = xa; i < xb; i++) row[i] = color;
        }
    } else {
        mote_draw_line(fb, x, y, x1 - 1, y, color, yc0, yc1);
        mote_draw_line(fb, x, y1 - 1, x1 - 1, y1 - 1, color, yc0, yc1);
        mote_draw_line(fb, x, y, x, y1 - 1, color, yc0, yc1);
        mote_draw_line(fb, x1 - 1, y, x1 - 1, y1 - 1, color, yc0, yc1);
    }
}

void mote_draw_circle(uint16_t *fb, int cx, int cy, int r,
                      uint16_t color, int fill, int yc0, int yc1){
    if (r < 1) r = 1;
    if (yc0 < 0) yc0 = 0;
    if (yc1 > MOTE_FB_H) yc1 = MOTE_FB_H;
    if (fill){
        for (int dy = -r; dy <= r; dy++){
            int py = cy + dy;
            if (py < yc0 || py >= yc1) continue;
            int half = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
            int x0 = cx - half, x1 = cx + half;
            if (x0 < 0) x0 = 0;
            if (x1 > MOTE_FB_W - 1) x1 = MOTE_FB_W - 1;
            uint16_t *row = fb + py * MOTE_FB_W;
            for (int px = x0; px <= x1; px++) row[px] = color;
        }
        return;
    }
    int x = r, y = 0, err = 1 - r;
    #define MOTE_CPLOT(PX,PY) do { int _x=(PX),_y=(PY); \
        if (_y>=yc0 && _y<yc1 && (unsigned)_x<(unsigned)MOTE_FB_W) fb[_y*MOTE_FB_W+_x]=color; } while(0)
    while (x >= y){
        MOTE_CPLOT(cx+x,cy+y); MOTE_CPLOT(cx+y,cy+x); MOTE_CPLOT(cx-y,cy+x); MOTE_CPLOT(cx-x,cy+y);
        MOTE_CPLOT(cx-x,cy-y); MOTE_CPLOT(cx-y,cy-x); MOTE_CPLOT(cx+y,cy-x); MOTE_CPLOT(cx+x,cy-y);
        y++; if (err < 0) err += 2*y+1; else { x--; err += 2*(y-x)+1; }
    }
    #undef MOTE_CPLOT
}
