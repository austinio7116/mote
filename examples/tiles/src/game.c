/*
 * tiles — the autotile (rule-tile) showcase.
 *
 * A cellular-automata cave whose walls are drawn with the engine's RENDER-TIME
 * autotiler: we store only a logical terrain map (1 byte per cell, 1 = rock,
 * 0 = open) and hand it to mote->scene2d_set_autotiles() with a Blob-47 ruleset.
 * The engine picks each wall tile from its 8 neighbours every frame — no
 * resolved buffer — so digging is instant: hold A to carve and the edges
 * re-tile live. Art is generated at init so the demo is self-contained.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_tile.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define TILE   8
#define NCELL  47                       /* Blob-47 */
#define COLS   48
#define ROWS   48

static uint16_t atlas_px[NCELL * TILE * TILE];   /* 47 cells in a row */
static uint8_t  terrain[COLS * ROWS];            /* the LOGICAL map: 1 = rock */

static MoteImage    atlas = { atlas_px, NCELL * TILE, TILE, MOTE_KEY_MAGENTA, 0 };
static MoteAutotile rock;                         /* the ruleset */
static const MoteAutotile *tiles[1] = { &rock };

static int cam_x = 64, cam_y = 64;

/* deterministic hash -> [0,1) */
static unsigned hsh(int x, int y) { unsigned h = (unsigned)x*374761393u + (unsigned)y*668265263u; h = (h^(h>>13))*1274126177u; return h^(h>>16); }

/* Draw one Blob cell: solid rock, with a lit rim on every OPEN side (where the
 * config's neighbour bit is clear) so walls read as bordered tunnels. */
static void draw_cell(int ci, uint8_t mask) {
    const uint16_t rockc = MOTE_RGB565(96, 88, 78);
    const uint16_t fleck = MOTE_RGB565(78, 72, 64);
    const uint16_t rim   = MOTE_RGB565(168, 156, 130);
    int openN = !(mask & MOTE_NB_N), openS = !(mask & MOTE_NB_S);
    int openW = !(mask & MOTE_NB_W), openE = !(mask & MOTE_NB_E);
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            uint16_t c = (((x*7 + y*13 + ci*5) & 7) == 0) ? fleck : rockc;
            if (openN && y == 0)        c = rim;
            if (openS && y == TILE-1)   c = rim;
            if (openW && x == 0)        c = rim;
            if (openE && x == TILE-1)   c = rim;
            /* round an exposed outer corner to magenta (transparent) */
            if (openN && openW && x == 0 && y == 0)              c = MOTE_KEY_MAGENTA;
            if (openN && openE && x == TILE-1 && y == 0)         c = MOTE_KEY_MAGENTA;
            if (openS && openW && x == 0 && y == TILE-1)         c = MOTE_KEY_MAGENTA;
            if (openS && openE && x == TILE-1 && y == TILE-1)    c = MOTE_KEY_MAGENTA;
            atlas_px[y * (NCELL*TILE) + ci*TILE + x] = c;
        }
}

/* Build the Blob-47 ruleset + paint each cell from its representative config. */
static void build_tileset(void) {
    mote_autotile_template(&rock, MOTE_AT_BLOB47);
    rock.sheet = &atlas; rock.tile_w = TILE; rock.tile_h = TILE; rock.edge_is_solid = 1;
    uint8_t rep[NCELL]; int got[NCELL]; for (int i = 0; i < NCELL; i++) got[i] = 0;
    for (int m = 0; m < 256; m++) { int ci = rock.lut[m]; if (ci < NCELL && !got[ci]) { rep[ci] = mote__at_reduce((uint8_t)m); got[ci] = 1; } }
    for (int ci = 0; ci < NCELL; ci++) draw_cell(ci, got[ci] ? rep[ci] : 0xFF);
}

/* Cellular-automata cave: random fill, then smooth (a cell becomes rock if a
 * majority of its 3x3 neighbourhood is rock). Borders stay solid. */
static void gen_cave(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            terrain[y*COLS + x] = (x < 2 || y < 2 || x >= COLS-2 || y >= ROWS-2 || (hsh(x, y) & 255) < 118) ? 1 : 0;
    static uint8_t tmp[COLS*ROWS];
    for (int pass = 0; pass < 4; pass++) {
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int xx = x+dx, yy = y+dy;
                    if (xx < 0 || xx >= COLS || yy < 0 || yy >= ROWS) { n++; continue; }
                    n += terrain[yy*COLS + xx];
                }
                tmp[y*COLS + x] = (n >= 5) ? 1 : 0;
            }
        for (int i = 0; i < COLS*ROWS; i++) terrain[i] = tmp[i];
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(14, 12, 22));
    build_tileset();
    gen_cave();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    int sp = (int)(70 * dt) + 1;
    if (mote_pressed(in, MOTE_BTN_LEFT))  cam_x -= sp;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) cam_x += sp;
    if (mote_pressed(in, MOTE_BTN_UP))    cam_y -= sp;
    if (mote_pressed(in, MOTE_BTN_DOWN))  cam_y += sp;
    if (cam_x < 0) cam_x = 0; if (cam_x > COLS*TILE - 128) cam_x = COLS*TILE - 128;
    if (cam_y < 0) cam_y = 0; if (cam_y > ROWS*TILE - 128) cam_y = ROWS*TILE - 128;

    /* hold A to carve a hole at screen centre — the edges re-tile instantly */
    if (mote_pressed(in, MOTE_BTN_A)) {
        int cx = (cam_x + 64) / TILE, cy = (cam_y + 64) / TILE;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int x = cx+dx, y = cy+dy;
            if (x > 1 && y > 1 && x < COLS-2 && y < ROWS-2) terrain[y*COLS + x] = 0;
        }
    }

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_autotiles(terrain, COLS, ROWS, tiles, 1);
}

static void g_overlay(uint16_t *fb) {
    mote->text(fb, "AUTOTILE: blob-47 cave", 4, 4, MOTE_RGB565(230, 220, 160));
    mote->text(fb, "dpad move  A carve", 4, 118, MOTE_RGB565(150, 150, 170));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
