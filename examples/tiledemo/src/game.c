/*
 * tiledemo — a 6-LAYER autotiled world that leans on transparency, plus a walking sprite.
 *
 * Six WANG16 tilesets (sand · water · dirt · grass · green · rough) are stacked as
 * layers: each cell of world_map is a bitmask of which layers occupy it, and the
 * engine autotiles every layer independently and draws them bottom-to-top. Each
 * tile is transparent except where its terrain sits, so a layer shows the one below
 * through its edges — sand beaches under the water and grass, lighter "green" clumps
 * blending over the grass, rocky/dirt patches over the sand. WANG16 keys each tile on
 * its four CORNERS, which gives crisp straight edges, rounded outer + inner corners,
 * isolated tufts, and smooth diagonal saddles all from one 3x6 sheet.
 *
 * It's all resolved AT RENDER TIME from the const bit-packed map (flash, zero SRAM);
 * only the player sprite is generated in code, to show sprites compositing on top.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "world.level.h"        /* world_map / world_COLS / world_ROWS / world_draw + the 6 tilesets */
#include "hero.h"               /* top-down hero sheet: 6 frames of 16x16 (down/up/side) */

#define TILE 16                 /* world tile size (matches the baked sheets) */
#define HT   16                 /* hero sprite cell */

enum { FACE_DOWN, FACE_UP, FACE_LEFT, FACE_RIGHT };
/* first sheet frame for each facing; LEFT/RIGHT share the SIDE frames (RIGHT flips) */
static const int FACE_BASE[4] = { 0, 2, 4, 4 };

static int   px, py;             /* hero world pixel position (top-left) */
static int   facing = FACE_DOWN;
static int   moving;             /* moved this frame? */
static float walk_t;             /* animation phase */
static int   steps;              /* HUD: tiles walked */

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(8, 10, 20));
    px = (world_COLS * TILE) / 2;
    py = (world_ROWS * TILE) / 2;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    int sp = (int)(60.0f * dt) + 1;
    int ox = px, oy = py;

    moving = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { px -= sp; facing = FACE_LEFT;  moving = 1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { px += sp; facing = FACE_RIGHT; moving = 1; }
    if (mote_pressed(in, MOTE_BTN_UP))    { py -= sp; facing = FACE_UP;    moving = 1; }
    if (mote_pressed(in, MOTE_BTN_DOWN))  { py += sp; facing = FACE_DOWN;  moving = 1; }

    px = mote_clampi(px, 0, world_COLS * TILE - HT);
    py = mote_clampi(py, 0, world_ROWS * TILE - HT);

    /* accumulate the Manhattan distance moved, for the tile-walk counter */
    int dpx = px - ox, dpy = py - oy;
    steps += (dpx < 0 ? -dpx : dpx) + (dpy < 0 ? -dpy : dpy);

    /* animate: step between the two walk frames while moving, stand on frame 0 idle */
    if (moving) walk_t += dt * 8.0f; else walk_t = 0.0f;
    int sub   = moving ? ((int)walk_t & 1) : 0;
    int frame = FACE_BASE[facing] + sub;

    /* Camera centres on the hero, clamped to the world. */
    int cam_x = mote_clampi(px + HT / 2 - MOTE_FB_W / 2, 0, world_COLS * TILE - MOTE_FB_W);
    int cam_y = mote_clampi(py + HT / 2 - MOTE_FB_H / 2, 0, world_ROWS * TILE - MOTE_FB_H);

    mote->scene2d_begin(cam_x, cam_y);
    world_draw(mote);                                    /* the 6 autotiled layers */

    MoteSprite s = {
        .img   = &hero_img,
        .x     = (int16_t)px,
        .y     = (int16_t)py,
        .fx    = (uint16_t)(frame * HT),
        .fy    = 0,
        .fw    = HT,
        .fh    = HT,
        .layer = 10,
        .flags = (facing == FACE_RIGHT) ? MOTE_SPR_HFLIP : 0,
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
