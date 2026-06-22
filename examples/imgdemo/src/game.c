/*
 * imgdemo — image asset loading. The smiley and the MOTE banner are real PNGs
 * baked to RGB565 headers by `mote bake` (img2tex), loaded as MoteImages with a
 * magenta colour-key for transparency. A swarm of the sprites bounce around the
 * 2D scene (animated 2-frame sheet, H-flipped by direction); the logo is blitted
 * as an overlay. Pure 2D — no 3D pass.
 *
 * Controls: A add a sprite · B reset
 */
#include "mote_api.h"
#include "mote_build.h"
#include "sprite.h"            /* baked: sprite_img (48x24, two 24x24 frames) */
#include "logo.h"             /* baked: logo_img (104x30) */
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define FRAME_W   24          /* one animation frame of the sprite sheet */
#define MAX_BUGS  24          /* sprite pool size (matches max_sprites) */

/* one bouncing sprite: position, velocity, and an animation phase offset */
typedef struct {
    float x, y;
    float vx, vy;
    float phase;
} Bug;

static Bug   bugs[MAX_BUGS];
static int   bug_count;
static int   add_armed;       /* debounce: re-arm A between presses */
static float anim_time;

/* Spawn one bug at a random on-screen spot, heading in a random direction. */
static void add_bug(void) {
    if (bug_count >= MAX_BUGS) return;

    Bug *b = &bugs[bug_count];
    b->x = mote_randf(8, 8 + (128 - FRAME_W - 16));
    b->y = mote_randf(24, 24 + (128 - FRAME_W - 32));

    float angle = mote_randf(0, 6.28f);
    float speed = mote_randf(28, 28 + 34);
    b->vx = cosf(angle) * speed;
    b->vy = sinf(angle) * speed;
    b->phase = mote_randf(0, 6.28f);

    bug_count++;
}

/* Clear the swarm and seed it with a starting handful. */
static void reset(void) {
    bug_count = 0;
    for (int i = 0; i < 6; i++) add_bug();
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(60, 90, 150));
    mote_rand_seed((uint32_t)mote->micros() | 1u);
    reset();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    if (!mote_pressed(in, MOTE_BTN_A)) add_armed = 1;
    if (add_armed && mote_just_pressed(in, MOTE_BTN_A)) add_bug();
    if (mote_just_pressed(in, MOTE_BTN_B)) reset();

    anim_time += dt;

    /* move each bug and bounce it off the play-area edges */
    for (int i = 0; i < bug_count; i++) {
        Bug *b = &bugs[i];
        b->x += b->vx * dt;
        b->y += b->vy * dt;

        if (b->x < 2)              { b->x = 2;              b->vx = -b->vx; }
        if (b->x > 128 - FRAME_W - 2) { b->x = 128 - FRAME_W - 2; b->vx = -b->vx; }
        if (b->y < 16)             { b->y = 16;             b->vy = -b->vy; }
        if (b->y > 128 - FRAME_W - 2) { b->y = 128 - FRAME_W - 2; b->vy = -b->vy; }
    }

    mote->scene2d_begin(0, 0);
    for (int i = 0; i < bug_count; i++) {
        Bug *b = &bugs[i];
        int frame = ((int)(anim_time * 4.0f + b->phase)) & 1;   /* animate the 2 frames */

        MoteSprite s = {
            .img   = &sprite_img,
            .x     = (int16_t)b->x,
            .y     = (int16_t)b->y,
            .fx    = (uint16_t)(frame * FRAME_W),
            .fy    = 0,
            .fw    = FRAME_W,
            .fh    = FRAME_W,
            .layer = 1,
            .flags = (uint8_t)(b->vx < 0 ? MOTE_SPR_HFLIP : 0),
        };
        mote->scene2d_add(&s);
    }
}

static void g_overlay(uint16_t *fb) {
    mote->blit(fb, &logo_img, (128 - logo_W) / 2, 2, 0, 0, logo_W, logo_H, 0, 0, 128);

    mote_textf(mote, fb, 4, 118, MOTE_RGB565(235, 240, 250), "%d PNG", bug_count);
    mote->text(fb, "A ADD  B RESET", 46, 118, MOTE_RGB565(150, 170, 210));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = MAX_BUGS },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
