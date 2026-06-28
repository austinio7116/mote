/*
 * fontdemo — anti-aliased proportional fonts (ABI v39).
 *
 * assets/bigfont.ttf is baked to src/bigfont.font.h by `mote bake` (the pixel
 * size comes from assets/bigfont.size). Draw it with mote->text_font(); the
 * glyph coverage is alpha-blended, so edges stay smooth at any size — unlike the
 * tiny built-in 3x5 mote->text() shown at the bottom for contrast.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "bigfont.font.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static float t = 0;

static void g_init(void) { mote->scene_set_background(MOTE_RGB565(16, 18, 30)); }
static void g_update(float dt) { t += dt; }

static void g_overlay(uint16_t *fb) {
    mote->text_font(fb, &bigfont, "Mote Fonts", 6, 6, MOTE_RGB565(255, 255, 255));
    mote->text_font(fb, &bigfont, "Antialiased", 6, 30, MOTE_RGB565(120, 200, 255));
    /* a colour cycle so the smooth edges are obvious in motion */
    uint8_t r = (uint8_t)(160 + 95 * (0.5f + 0.5f * (float)__builtin_sinf(t * 2.0f)));
    mote->text_font(fb, &bigfont, "Sphinx judge,", 6, 56, MOTE_RGB565(r, 210, 120));
    mote->text_font(fb, &bigfont, "vow my quartz!", 6, 80, MOTE_RGB565(r, 210, 120));
    /* the built-in 3x5 font for size/quality contrast */
    mote->text(fb, "built-in 3x5 mote->text()", 6, 118, MOTE_RGB565(140, 144, 160));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 1 },   /* 2D-only demo — no 3D pools needed */
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("FontDemo", "mote");
