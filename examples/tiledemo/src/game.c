/*
 * tiledemo — a 2D tile-based game module, proving the sprite/tilemap system
 * works alongside the 3D engine.
 *
 * A scrolling tile world (grass/stone/water) with a player sprite that walks
 * with the D-pad; the camera follows. Art is generated procedurally at init
 * (into the module's RAM) so the demo is self-contained — real games bake
 * sprite sheets with `mote bake` (img2tex).
 */
#include "mote_api.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define TILE 8
#define ATLAS_TILES 4
#define MAP_COLS 24
#define MAP_ROWS 18

static uint16_t atlas_px[ATLAS_TILES * TILE * TILE];   /* 4 tiles in a row */
static uint16_t player_px[TILE * TILE];
static uint8_t  map_cells[MAP_COLS * MAP_ROWS];

static MoteImage   atlas   = { atlas_px, ATLAS_TILES * TILE, TILE, MOTE_KEY_MAGENTA };
static MoteTileset tileset = { &atlas, TILE, TILE };
static MoteTilemap tilemap = { map_cells, MAP_COLS, MAP_ROWS };
static MoteImage   player  = { player_px, TILE, TILE, MOTE_KEY_MAGENTA };

static int px, py;   /* player world pixel position */

static void fill_tile(int idx, uint16_t base, uint16_t fleck) {
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            uint16_t c = base;
            if (((x * 7 + y * 13 + idx * 5) & 7) == 0) c = fleck;
            atlas_px[y * (ATLAS_TILES * TILE) + idx * TILE + x] = c;
        }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(8, 10, 20));

    fill_tile(0, MOTE_RGB565(60, 150, 70),  MOTE_RGB565(80, 180, 90));   /* grass */
    fill_tile(1, MOTE_RGB565(120, 120, 130), MOTE_RGB565(90, 90, 100));  /* stone */
    fill_tile(2, MOTE_RGB565(40, 90, 200),  MOTE_RGB565(80, 130, 230));  /* water */
    fill_tile(3, MOTE_RGB565(110, 80, 50),  MOTE_RGB565(70, 50, 30));    /* dirt */

    /* A simple world: grass, a stone path, a water pond. */
    for (int r = 0; r < MAP_ROWS; r++)
        for (int c = 0; c < MAP_COLS; c++) {
            uint8_t t = 0;                               /* grass */
            if (r == 9) t = 1;                           /* stone path */
            if (c >= 4 && c <= 7 && r >= 3 && r <= 5) t = 2;  /* pond */
            if (((c * 3 + r * 5) % 11) == 0) t = 3;      /* dirt flecks */
            map_cells[r * MAP_COLS + c] = t;
        }

    /* Player: a yellow blob on a magenta (transparent) field. */
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            int dx = x - 4, dy = y - 4;
            player_px[y * TILE + x] = (dx * dx + dy * dy <= 9)
                ? MOTE_RGB565(240, 220, 40) : MOTE_KEY_MAGENTA;
        }

    px = (MAP_COLS * TILE) / 2;
    py = (MAP_ROWS * TILE) / 2;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    int sp = (int)(48.0f * dt) + 1;
    if (mote_pressed(in, MOTE_BTN_LEFT))  px -= sp;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) px += sp;
    if (mote_pressed(in, MOTE_BTN_UP))    py -= sp;
    if (mote_pressed(in, MOTE_BTN_DOWN))  py += sp;
    if (px < 0) px = 0; if (py < 0) py = 0;
    int maxx = MAP_COLS * TILE - TILE, maxy = MAP_ROWS * TILE - TILE;
    if (px > maxx) px = maxx; if (py > maxy) py = maxy;

    /* Camera centres on the player, clamped to the map. */
    int cam_x = px - MOTE_FB_W / 2, cam_y = py - MOTE_FB_H / 2;
    if (cam_x < 0) cam_x = 0; if (cam_y < 0) cam_y = 0;
    int cmaxx = MAP_COLS * TILE - MOTE_FB_W, cmaxy = MAP_ROWS * TILE - MOTE_FB_H;
    if (cam_x > cmaxx) cam_x = cmaxx; if (cam_y > cmaxy) cam_y = cmaxy;

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_tilemap(&tilemap, &tileset);
    MoteSprite s = { &player, (int16_t)px, (int16_t)py, 0, 0, TILE, TILE, 10, 0 };
    mote->scene2d_add(&s);
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
