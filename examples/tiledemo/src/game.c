/*
 * tiledemo — a 2D autotiled world built from FILE tilesheets plus a walking sprite.
 *
 * The three terrain layers (dirt / grass / water) and the level map are authored in
 * Mote Studio's Tiles tab: each tileset's art is a real PNG in assets/, baked to
 * src/<name>.tiles.h, and the level is a bit-packed map in src/world.level.h. The
 * engine pulls the right atlas cell per the ruleset AT RENDER TIME — there is no
 * resolved per-level image, and the map is const (flash, zero SRAM). Only the player
 * sprite is still generated in code, to show sprites compositing over the tiles.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "world.level.h"        /* world_map / world_COLS / world_ROWS / world_draw + the 3 tilesets */

#define TILE 16                 /* world tile size (matches the baked sheets) */
#define PT   8                  /* player sprite cell */
#define PLAYER_FRAMES 4         /* idle + 2 walk + bob */

static uint16_t player_px[PLAYER_FRAMES * PT * PT];
static MoteImage player = { player_px, PLAYER_FRAMES * PT, PT, MOTE_KEY_MAGENTA, 0 };

static int   px, py;             /* player world pixel position */
static int   facing = 1;         /* -1 left, +1 right */
static int   moving;             /* moved this frame? */
static float walk_t;             /* animation phase */
static int   steps;              /* HUD: tiles walked */

/* Build the player frames: a round body with a darker outline and a small "eye";
 * walk frames bob the legs for motion. */
static void build_player(void) {
    const uint16_t body    = MOTE_RGB565(245, 220, 50);
    const uint16_t shade   = MOTE_RGB565(200, 160, 30);
    const uint16_t outline = MOTE_RGB565(60, 45, 10);

    for (int f = 0; f < PLAYER_FRAMES; f++) {
        int legbob = (f == 1) ? -1 : (f == 2) ? 1 : 0;   /* frames 1/2 walk */

        for (int y = 0; y < PT; y++)
            for (int x = 0; x < PT; x++) {
                int dx = x - 4, dy = y - 4;
                int r2 = dx * dx + dy * dy;

                uint16_t c = MOTE_KEY_MAGENTA;
                if (r2 <= 9)        c = body;
                if (r2 == 9)        c = outline;          /* rim */
                if (dy >= 2 && (x == 3 + legbob || x == 5 - legbob))
                    c = shade;                            /* legs */
                if (x == 4 && y == 3) c = outline;        /* eye */

                player_px[y * (PLAYER_FRAMES * PT) + f * PT + x] = c;
            }
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(8, 10, 20));
    build_player();
    px = (world_COLS * TILE) / 2;
    py = (world_ROWS * TILE) / 2;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    int sp = (int)(70.0f * dt) + 1;
    int ox = px, oy = py;

    moving = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { px -= sp; facing = -1; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { px += sp; facing =  1; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_UP))    { py -= sp; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_DOWN))  { py += sp; moving = 1; }

    px = mote_clampi(px, 0, world_COLS * TILE - PT);
    py = mote_clampi(py, 0, world_ROWS * TILE - PT);

    /* accumulate the Manhattan distance moved, for the tile-walk counter */
    int dpx = px - ox, dpy = py - oy;
    steps += (dpx < 0 ? -dpx : dpx) + (dpy < 0 ? -dpy : dpy);

    if (moving) walk_t += dt * 8.0f; else walk_t = 0.0f;
    int frame = moving ? (1 + ((int)walk_t & 1)) : 0;

    /* Camera centres on the player, clamped to the world. */
    int cam_x = mote_clampi(px - MOTE_FB_W / 2, 0, world_COLS * TILE - MOTE_FB_W);
    int cam_y = mote_clampi(py - MOTE_FB_H / 2, 0, world_ROWS * TILE - MOTE_FB_H);

    mote->scene2d_begin(cam_x, cam_y);
    world_draw(mote);                                    /* the autotiled file tilesets */

    MoteSprite s = {
        .img   = &player,
        .x     = (int16_t)px,
        .y     = (int16_t)py,
        .fx    = (uint16_t)(frame * PT),
        .fy    = 0,
        .fw    = PT,
        .fh    = PT,
        .layer = 10,
        .flags = (facing < 0) ? MOTE_SPR_HFLIP : 0,
    };
    mote->scene2d_add(&s);
}

/* HUD: a clean top strip with the tile-walk counter. */
static void g_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 12,
                  MOTE_RGB565(18, 22, 36), MOTE_RGB565(70, 90, 130));
    mote_textf(mote, fb, 3, 2, MOTE_RGB565(235, 235, 245), "TILES %d", steps / TILE);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 8 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
