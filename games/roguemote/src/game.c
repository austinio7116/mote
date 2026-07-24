/*
 * roguemote — asset-library verification harness.
 *
 * Not a game yet: it renders a slice of every extracted asset group so we can
 * confirm the whole pipeline (sprite subsheets, autotile rulesets, the rogue8
 * MoteFont) bakes and draws correctly. The real roguelike is built on top later.
 */
#include "mote_api.h"
#include "mote_build.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* autotile rulesets */
#include "floor_cobble.tiles.h"
#include "wall_brick.tiles.h"
/* sprite subsheets */
#include "chests.h"
#include "characters.h"
#include "monsters.h"
#include "ui_status_emotes.h"
/* fonts */
#include "rogue8.font.h"

#define COLS 16
#define ROWS 16
static uint8_t g_map[COLS * ROWS];   /* bit0 = floor, bit1 = wall */
static const MoteAutotile *g_tiles[2];

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(18, 16, 28));
    g_tiles[0] = &floor_cobble_at;
    g_tiles[1] = &wall_brick_at;
    /* a little dungeon: floor everywhere, a bordered room with a door gap and
     * a free-standing pillar block. */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            uint8_t v = 1;                                  /* floor */
            int border = (r == 2 || r == 12 || c == 1 || c == 14);
            int inside = (r >= 2 && r <= 12 && c >= 1 && c <= 14);
            if (inside && border) v |= 2;                   /* room wall */
            if (r == 12 && (c == 7 || c == 8)) v = 1;        /* door gap */
            if ((r == 6 || r == 7) && (c == 5 || c == 6)) v |= 2; /* pillar */
            g_map[r * COLS + c] = v;
        }
}

static void g_update(float dt) {
    (void)dt;
    mote->scene2d_begin(0, 0);
    mote->scene2d_set_autotile_layers(g_map, COLS, ROWS, g_tiles, 2);
    /* a few sprites on the dungeon floor */
    MoteSprite chest = { &chests_img, 24, 24, 0, 0, 8, 8, 20, 0 };       /* red closed chest */
    MoteSprite hero  = { &characters_img, 64, 56, 0, 0, 8, 8, 22, 0 };   /* @ player glyph */
    MoteSprite mob   = { &monsters_img, 96, 40, 0, 0, 8, 8, 21, 0 };     /* first monster */
    mote->scene2d_add(&chest);
    mote->scene2d_add(&hero);
    mote->scene2d_add(&mob);
}

static void g_overlay(uint16_t *fb) {
    /* readable engine title + our baked rogue8 CP437 font */
    mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "ROGUEMOTE", 4, 2, MOTE_RGB565(255, 220, 90));
    mote->text_font(fb, &rogue8, "CC0 ASSET LIBRARY", 4, 20, MOTE_RGB565(200, 230, 255));
    /* heart meter from the UI/status sheet (hearts row) */
    for (int i = 0; i < 5; i++)
        mote->blit(fb, &ui_status_emotes_img, 4 + i * 9, 116, 0, 56, 8, 8, 0, 0, 128);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 16 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("Roguemote", "austinio7116");
