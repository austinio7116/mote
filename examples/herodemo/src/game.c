/*
 * herodemo — a tiny platformer showcasing the sprite-animation runtime (sdk/mote_anim.h).
 *
 * A Mario-style character (side profile) runs and jumps across floating platforms. The
 * animation is entirely data: hero.anim.h (baked in Mote Studio's Anim tab from
 * assets/hero.png) gives four const clips — idle / walk / jump / fall — plus the sheet.
 * Each frame we run a tiny state machine, play the matching clip when the state changes,
 * tick it by dt, and read the current cell into a MoteSprite. The clip pivot (feet, 8,15)
 * anchors the sprite so frames sit on the ground. Platforms are the ground cell of the
 * same sheet, drawn as sprites. Facing flips the sprite horizontally.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "hero.anim.h"          /* hero_sheet + hero_idle / hero_walk / hero_jump / hero_fall */

#define TILE        16
#define GROUND_CELL 5           /* the grass-topped block in the hero sheet */
#define GRAV    520.0f          /* px/s^2 */
#define MOVE     74.0f          /* px/s   */
#define JUMP_V (-210.0f)        /* px/s   */
#define WORLD_W 320

typedef struct { int x, y, w, h; } Rect;
static const Rect PLAT[] = {
    {  0, 116, 320, 24 },       /* the long floor */
    { 56,  92,  48, 16 },
    {136,  72,  48, 16 },
    {212,  92,  48, 16 },
    {268,  60,  40, 16 },
};
#define NPLAT (int)(sizeof(PLAT)/sizeof(PLAT[0]))

static float hx, hy, vx, vy;     /* hero feet position + velocity (world px) */
static int   on_ground, facing = 1;
static MoteAnimPlayer anim;
static const MoteAnimClip *cur;  /* current clip — only re-play on change */

static void set_clip(const MoteAnimClip *c) { if (cur != c) { cur = c; mote_anim_play(&anim, c); } }

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(135, 190, 235));  /* light sky — clear of the blue overalls */
    hx = 40; hy = 116;
    set_clip(&hero_idle);
}

/* one-way platform: returns the top y if the feet crossed it falling this frame, else -1 */
static int land(float fx, float fy, float prev) {
    for (int i = 0; i < NPLAT; i++) {
        const Rect *p = &PLAT[i];
        if (fx > p->x - 6 && fx < p->x + p->w + 6 && prev <= p->y + 2 && fy >= p->y) return p->y;
    }
    return -1;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;

    vx = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { vx = -MOVE; facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { vx =  MOVE; facing =  1; }
    hx += vx * dt;
    if (hx < 6) hx = 6;
    if (hx > WORLD_W - 6) hx = WORLD_W - 6;

    if (on_ground && (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))) {
        vy = JUMP_V; on_ground = 0;
    }

    float prev = hy;
    vy += GRAV * dt;
    hy += vy * dt;
    on_ground = 0;
    if (vy >= 0) {
        int top = land(hx, hy, prev);
        if (top >= 0) { hy = (float)top; vy = 0; on_ground = 1; }
    }
    if (hy > 200) { hx = 40; hy = 116; vy = 0; }                /* fell off — respawn */

    if (!on_ground)   set_clip(vy < 0 ? &hero_jump : &hero_fall);
    else if (vx != 0) set_clip(&hero_walk);
    else              set_clip(&hero_idle);
    mote_anim_tick(&anim, dt);

    int cam_x = (int)hx - MOTE_FB_W / 2;
    if (cam_x < 0) cam_x = 0;
    if (cam_x > WORLD_W - MOTE_FB_W) cam_x = WORLD_W - MOTE_FB_W;
    mote->scene2d_begin(cam_x, 0);

    for (int i = 0; i < NPLAT; i++) {
        const Rect *p = &PLAT[i];
        for (int x = 0; x < p->w; x += TILE) {
            MoteSprite g = { hero_sheet.image, (int16_t)(p->x + x), (int16_t)p->y,
                             GROUND_CELL * TILE, 0, TILE, TILE, 2, 0 };
            mote->scene2d_add(&g);
        }
    }

    uint8_t flags = facing < 0 ? MOTE_SPR_HFLIP : 0;
    MoteSprite s = { hero_sheet.image,
                     (int16_t)(hx - cur->pivot_x), (int16_t)(hy - cur->pivot_y),
                     (uint16_t)mote_anim_fx(&anim, &hero_sheet),
                     (uint16_t)mote_anim_fy(&anim, &hero_sheet),
                     hero_sheet.tile_w, hero_sheet.tile_h, 5, flags };
    mote->scene2d_add(&s);
}

static void g_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 12, MOTE_RGB565(18, 22, 36), MOTE_RGB565(70, 90, 130));
    mote->text(fb, "dpad move   A/B jump", 3, 2, MOTE_RGB565(235, 235, 245));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 48 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
