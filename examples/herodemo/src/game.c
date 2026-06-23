/*
 * herodemo — a platformer that shows the sprite-animation runtime AND the tile/level system.
 *
 * The character is animated from hero.anim.h (baked in the Studio Anim tab): idle / walk /
 * jump / fall clips, an 8-cell side-profile sheet, pivot at the feet. A state machine picks
 * the clip from the physics state; mote_anim_tick advances it; the current cell drives a
 * MoteSprite (HFLIP for facing).
 *
 * The world is the tile/level system: a 'ground' autotile (ground.tiles.h) + three
 * bit-packed levels (level1/2/3.level.h), each authored in the Studio Tiles tab. The engine
 * autotiles the ground every frame (levelN_draw); collision tests the level's bitmask map
 * directly (layer 0 = solid). Reach the right edge to advance; the last loops back.
 */
#include "mote_api.h"
#include "mote_build.h"

#include "icon.h"
MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "hero.anim.h"
#include "level1.level.h"
#include "level2.level.h"
#include "level3.level.h"

#define TILE     16
#define GRAV    540.0f
#define MOVE     80.0f
#define JUMP_V (-220.0f)

typedef struct { const uint8_t *map; int cols, rows; void (*draw)(const MoteApi *); } Level;
static const Level LV[] = {
    { level1_map, level1_COLS, level1_ROWS, level1_draw },
    { level2_map, level2_COLS, level2_ROWS, level2_draw },
    { level3_map, level3_COLS, level3_ROWS, level3_draw },
};
#define NLV (int)(sizeof(LV)/sizeof(LV[0]))

static int   lvl;
static float hx, hy, vx, vy;     /* feet position + velocity (world px) */
static int   on_ground, facing = 1;
static MoteAnimPlayer anim;
static const MoteAnimClip *cur;

static void set_clip(const MoteAnimClip *c) {
    if (cur != c) {
        cur = c;
        mote_anim_play(&anim, c);
    }
}

/* is the tile at world pixel (wx,wy) solid (ground layer = bit 0)? */
static int solid(float wx, float wy) {
    const Level *L = &LV[lvl];
    int c = (int)(wx / TILE), r = (int)(wy / TILE);
    if (c < 0 || c >= L->cols || r < 0 || r >= L->rows) return 0;
    return L->map[r * L->cols + c] & 1u;
}

static void load_level(int n) {
    lvl = (n % NLV + NLV) % NLV;
    hx = 24;
    hy = (LV[lvl].rows - 1) * TILE;   /* on the floor near the left */
    vx = vy = 0;
    on_ground = 1;
    facing = 1;
    set_clip(&hero_idle);
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(135, 190, 235));
    load_level(0);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;
    const Level *L = &LV[lvl];

    /* horizontal move, blocked by a solid tile at chest height */
    vx = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { vx = -MOVE; facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { vx =  MOVE; facing =  1; }

    float nx = hx + vx * dt;
    float probe_x = nx + (vx > 0 ? 5 : -5);
    if (!solid(probe_x, hy - 8) && !solid(probe_x, hy - 1)) hx = nx;

    /* advance to the next level off the right edge */
    if (hx > L->cols * TILE - 4) { load_level(lvl + 1); return; }
    if (hx < 2) hx = 2;

    /* jump */
    if (on_ground && (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))) {
        vy = JUMP_V;
        on_ground = 0;
    }

    /* gravity + vertical resolve against the tile under the feet / over the head */
    vy += GRAV * dt;
    float ny = hy + vy * dt;
    on_ground = 0;
    if (vy >= 0) {                                  /* falling: land on a tile top */
        if (solid(hx, ny)) {
            ny = (float)((int)(ny / TILE) * TILE);
            vy = 0;
            on_ground = 1;
        }
    } else if (solid(hx, ny - 16)) {                /* rising: bonk head */
        ny = (float)(((int)((ny - 16) / TILE) + 1) * TILE + 16);
        vy = 0;
    }
    hy = ny;
    if (hy > L->rows * TILE + 24) load_level(lvl);  /* fell out — restart level */

    /* animation state machine */
    if (!on_ground)   set_clip(vy < 0 ? &hero_jump : &hero_fall);
    else if (vx != 0) set_clip(&hero_walk);
    else              set_clip(&hero_idle);
    mote_anim_tick(&anim, dt);

    int cam_x = mote_clampi((int)hx - MOTE_FB_W / 2, 0, L->cols * TILE - MOTE_FB_W);
    mote->scene2d_begin(cam_x, 0);
    L->draw(mote);                                   /* the autotiled ground */

    MoteSprite s = {
        .img   = hero_sheet.image,
        .x     = (int16_t)(hx - cur->pivot_x),
        .y     = (int16_t)(hy - cur->pivot_y),
        .fx    = (uint16_t)mote_anim_fx(&anim, &hero_sheet),
        .fy    = (uint16_t)mote_anim_fy(&anim, &hero_sheet),
        .fw    = hero_sheet.tile_w,
        .fh    = hero_sheet.tile_h,
        .layer = 5,
        .flags = facing < 0 ? MOTE_SPR_HFLIP : 0,
    };
    mote->scene2d_add(&s);
}

static void g_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 12, MOTE_RGB565(18, 22, 36), MOTE_RGB565(70, 90, 130));
    mote_textf(mote, fb, 3, 2, MOTE_RGB565(235, 235, 245), "LV %d  >>", lvl + 1);
    mote->text(fb, "dpad  A/B jump", 60, 2, MOTE_RGB565(150, 170, 200));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 8 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
