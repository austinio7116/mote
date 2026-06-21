/*
 * tiledemo — a 2D tile-based game module, proving the sprite/tilemap system
 * works alongside the 3D engine.
 *
 * A scrolling tile world (grass/stone/water/dirt) with an animated player
 * sprite that walks with the D-pad; the camera follows. Art is generated
 * procedurally at init (into the module's RAM) so the demo is self-contained —
 * real games bake sprite sheets with `mote bake` (img2tex). The HUD is drawn
 * with the immediate-mode helpers from sdk/mote_build.h.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define TILE 8
#define ATLAS_TILES 4
#define MAP_COLS 24
#define MAP_ROWS 18
#define PLAYER_FRAMES 4          /* idle + 2 walk + bob */

static uint16_t atlas_px[ATLAS_TILES * TILE * TILE];          /* 4 tiles in a row */
static uint16_t player_px[PLAYER_FRAMES * TILE * TILE];       /* frames in a row */
static uint8_t  map_cells[MAP_COLS * MAP_ROWS];

static MoteImage   atlas   = { atlas_px, ATLAS_TILES * TILE, TILE, MOTE_KEY_MAGENTA };
static MoteTileset tileset = { &atlas, TILE, TILE };
static MoteTilemap tilemap = { map_cells, MAP_COLS, MAP_ROWS };
static MoteImage   player  = { player_px, PLAYER_FRAMES * TILE, TILE, MOTE_KEY_MAGENTA };

static int   px, py;             /* player world pixel position */
static int   facing = 1;         /* -1 left, +1 right */
static int   moving;             /* moved this frame? */
static float walk_t;             /* animation phase */
static int   steps;              /* HUD: tiles walked */

/* A soft, low-contrast fleck pattern keeps the cloth/ground readable rather
 * than noisy: only ~1 pixel in 8 differs, and stone gets a subtle dither. */
static void fill_tile(int idx, uint16_t base, uint16_t fleck) {
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            uint16_t c = base;
            if (((x * 7 + y * 13 + idx * 5) & 7) == 0) c = fleck;
            atlas_px[y * (ATLAS_TILES * TILE) + idx * TILE + x] = c;
        }
}

/* Build the player frames: a round body with a darker outline and a small
 * "eye", offset by `facing`; walk frames bob the legs for motion. */
static void build_player(void) {
    const uint16_t body    = MOTE_RGB565(245, 220, 50);
    const uint16_t shade   = MOTE_RGB565(200, 160, 30);
    const uint16_t outline = MOTE_RGB565(60, 45, 10);
    for (int f = 0; f < PLAYER_FRAMES; f++) {
        int legbob = (f == 1) ? -1 : (f == 2) ? 1 : 0;   /* frames 1/2 walk */
        for (int y = 0; y < TILE; y++)
            for (int x = 0; x < TILE; x++) {
                int dx = x - 4, dy = y - 4;
                int r2 = dx * dx + dy * dy;
                uint16_t c = MOTE_KEY_MAGENTA;
                if (r2 <= 9)        c = body;
                if (r2 == 9)        c = outline;          /* rim */
                if (dy >= 2 && (x == 3 + legbob || x == 5 - legbob))
                    c = shade;                            /* legs */
                if (x == 4 && y == 3) c = outline;        /* eye, faces ahead */
                player_px[y * (PLAYER_FRAMES * TILE) + f * TILE + x] = c;
            }
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(8, 10, 20));

    fill_tile(0, MOTE_RGB565(58, 148, 70),  MOTE_RGB565(78, 176, 92));   /* grass */
    fill_tile(1, MOTE_RGB565(122, 124, 134), MOTE_RGB565(96, 98, 108));  /* stone */
    fill_tile(2, MOTE_RGB565(44, 96, 200),  MOTE_RGB565(86, 138, 232));  /* water */
    fill_tile(3, MOTE_RGB565(120, 88, 54),  MOTE_RGB565(96, 70, 42));    /* dirt  */

    /* A readable world: grass field, a stone path across the middle, a water
     * pond top-left, and a few dirt patches — not random per-tile speckle. */
    for (int r = 0; r < MAP_ROWS; r++)
        for (int c = 0; c < MAP_COLS; c++) {
            uint8_t t = 0;                                       /* grass */
            if (r == 9 || r == 10) t = 1;                        /* 2-wide stone path */
            if (c >= 3 && c <= 7 && r >= 2 && r <= 5) t = 2;     /* pond */
            /* a couple of compact dirt patches */
            if ((c >= 16 && c <= 18 && r >= 4 && r <= 5) ||
                (c >= 9 && c <= 11 && r >= 13 && r <= 14)) t = 3;
            map_cells[r * MAP_COLS + c] = t;
        }

    build_player();

    px = (MAP_COLS * TILE) / 2;
    py = (MAP_ROWS * TILE) / 2;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    int sp = (int)(48.0f * dt) + 1;
    int ox = px, oy = py;
    moving = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { px -= sp; facing = -1; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { px += sp; facing =  1; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_UP))    { py -= sp; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_DOWN))  { py += sp; moving = 1; }
    if (px < 0) px = 0; if (py < 0) py = 0;
    int maxx = MAP_COLS * TILE - TILE, maxy = MAP_ROWS * TILE - TILE;
    if (px > maxx) px = maxx; if (py > maxy) py = maxy;

    /* tally tiles walked, for the HUD */
    int dpx = px - ox, dpy = py - oy;
    steps += (dpx < 0 ? -dpx : dpx) + (dpy < 0 ? -dpy : dpy);

    /* walk animation: cycle frames 1<->2 while moving, idle frame 0 otherwise */
    if (moving) walk_t += dt * 8.0f; else walk_t = 0.0f;
    int frame = moving ? (1 + ((int)walk_t & 1)) : 0;

    /* Camera centres on the player, clamped to the map. */
    int cam_x = px - MOTE_FB_W / 2, cam_y = py - MOTE_FB_H / 2;
    if (cam_x < 0) cam_x = 0; if (cam_y < 0) cam_y = 0;
    int cmaxx = MAP_COLS * TILE - MOTE_FB_W, cmaxy = MAP_ROWS * TILE - MOTE_FB_H;
    if (cam_x > cmaxx) cam_x = cmaxx; if (cam_y > cmaxy) cam_y = cmaxy;

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_tilemap(&tilemap, &tileset);
    uint8_t flags = (facing < 0) ? MOTE_SPR_HFLIP : 0;
    MoteSprite s = { &player, (int16_t)px, (int16_t)py,
                     (int16_t)(frame * TILE), 0, TILE, TILE, 10, flags };
    mote->scene2d_add(&s);
}

/* --- HUD: a clean top strip with the tile-walk counter, drawn with the
 * immediate-mode helpers from mote_build.h. ------------------------------- */
static void g_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 12,
                  MOTE_RGB565(18, 22, 36), MOTE_RGB565(70, 90, 130));
    char line[24]; int q = 0;
    line[q++] = 'T'; line[q++] = 'I'; line[q++] = 'L'; line[q++] = 'E';
    line[q++] = 'S'; line[q++] = ' ';
    q += mote_itoa(steps / TILE, line + q);
    line[q] = 0;
    mote->text(fb, line, 3, 2, MOTE_RGB565(235, 235, 245));
}

/* 2D-only game: no 3D triangle list, no depth buffer. Declare just the sprite
 * pool we use so the loader sizes the arena to this game. */
static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 8 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
