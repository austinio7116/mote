/*
 * tiles — the autotile (rule-tile) showcase.
 *
 * A cellular-automata cave whose walls are drawn with the engine's RENDER-TIME
 * autotiler: we store only a logical terrain map (1 byte per cell, 1 = rock,
 * 0 = open) and hand it to mote->scene2d_set_autotiles() with a Blob-47 ruleset.
 * The engine picks each wall tile from its 8 neighbours every frame — no
 * resolved buffer — so digging is instant: hold A to carve and the edges
 * re-tile live. The tileset is a real FILE (assets/rock.png, baked to
 * src/rock.tiles.h in Mote Studio) — only the cave MAP is generated at runtime.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_tile.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "rock.tiles.h"     /* file tileset baked from assets/rock.png: rock_img + rock_at */

#define TILE   16                       /* matches the baked sheet */
#define COLS   48
#define ROWS   48

static uint8_t terrain[COLS * ROWS];                 /* the LOGICAL map: 1 = rock */
static const MoteAutotile *tilesets[1] = { &rock_at };

static int cam_x = 64;
static int cam_y = 64;

/* deterministic hash of a cell coordinate -> scrambled 32-bit value */
static unsigned cell_hash(int x, int y) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* True if (x,y) lies in the always-solid border ring. */
static int is_border(int x, int y) {
    return x < 2 || y < 2 || x >= COLS - 2 || y >= ROWS - 2;
}

/* Cellular-automata cave: random fill, then smooth (a cell becomes rock if a
 * majority of its 3x3 neighbourhood is rock). Borders stay solid. */
static void gen_cave(void) {
    /* random fill: borders solid, interior rock with ~55% probability */
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            terrain[y * COLS + x] = (is_border(x, y) || (cell_hash(x, y) & 255) < 140) ? 1 : 0;

    /* smoothing passes: each cell follows the rock majority of its 3x3 region
     * (off-map neighbours count as rock, keeping the cave enclosed) */
    static uint8_t next[COLS * ROWS];
    for (int pass = 0; pass < 4; pass++) {
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                int rock_neighbours = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || xx >= COLS || yy < 0 || yy >= ROWS) {
                            rock_neighbours++;
                            continue;
                        }
                        rock_neighbours += terrain[yy * COLS + xx];
                    }
                next[y * COLS + x] = (rock_neighbours >= 5) ? 1 : 0;
            }
        for (int i = 0; i < COLS * ROWS; i++) terrain[i] = next[i];
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(14, 12, 22));
    gen_cave();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    int speed = (int)(70 * dt) + 1;
    if (mote_pressed(in, MOTE_BTN_LEFT))  cam_x -= speed;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) cam_x += speed;
    if (mote_pressed(in, MOTE_BTN_UP))    cam_y -= speed;
    if (mote_pressed(in, MOTE_BTN_DOWN))  cam_y += speed;

    cam_x = mote_clampi(cam_x, 0, COLS * TILE - 128);
    cam_y = mote_clampi(cam_y, 0, ROWS * TILE - 128);

    /* hold A to carve a hole at screen centre — the edges re-tile instantly */
    if (mote_pressed(in, MOTE_BTN_A)) {
        int cx = (cam_x + 64) / TILE, cy = (cam_y + 64) / TILE;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x > 1 && y > 1 && x < COLS - 2 && y < ROWS - 2) terrain[y * COLS + x] = 0;
            }
    }

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_autotiles(terrain, COLS, ROWS, tilesets, 1);
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
