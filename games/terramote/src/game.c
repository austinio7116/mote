/* TerraMote — a Terraria-style sandbox for the Thumby Color.
 * Dig, build, craft, fight. See DESIGN.md. */
#define TERRA_MAIN
#include "terra.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* foreground tile rulesets (baked from tilesets/*.tileset by mote bake) */
#include "tiles_dirt.tiles.h"
#include "tiles_grass.tiles.h"
#include "tiles_stone.tiles.h"
#include "tiles_wood.tiles.h"
#include "tiles_trunk.tiles.h"
#include "tiles_leaf.tiles.h"
#include "tiles_sand.tiles.h"
#include "tiles_snow.tiles.h"
#include "tiles_ebon.tiles.h"
#include "tiles_clay.tiles.h"
#include "tiles_copper.tiles.h"
#include "tiles_iron.tiles.h"
#include "tiles_gold.tiles.h"
#include "tiles_demonite.tiles.h"
#include "tiles_ash.tiles.h"
#include "tiles_hellstone.tiles.h"
#include "tiles_obsidian.tiles.h"
#include "tiles_torch.tiles.h"
#include "tiles_workbench.tiles.h"
#include "tiles_furnace.tiles.h"
#include "tiles_anvil.tiles.h"
#include "tiles_chest.tiles.h"
#include "tiles_door_c.tiles.h"
#include "tiles_door_o.tiles.h"
#include "tiles_platform.tiles.h"
#include "tiles_altar.tiles.h"
#include "tiles_mush.tiles.h"
#include "tiles_flower.tiles.h"
#include "tiles_sapling.tiles.h"
#include "grass_cap.h"                       /* cosmetic caps over exposed dirt */
#include "canopy.h"                          /* tree crowns: 3 leafy + 1 snowy, 40x28 */
#include "branch.h"                          /* trunk branches: 16x12 x4 */

const MoteApi *g_mote;
uint8_t g_state = GS_TITLE;
float   g_time = 0.25f;
uint8_t g_boss_down;
uint32_t g_seed;
float   g_dt;
int     g_cam_x, g_cam_y;
float   g_dead_t;

/* fg terrain value -> ruleset (index = tile id - 1) */
static const MoteAutotile *k_tiles[T_COUNT - 1] = {
    &tiles_dirt_at,      /* T_DIRT */
    &tiles_grass_at,     /* T_GRASS (unused by gen; kept for saves) */
    &tiles_stone_at,
    &tiles_wood_at,
    &tiles_trunk_at,
    &tiles_leaf_at,
    &tiles_sand_at,
    &tiles_snow_at,
    &tiles_ebon_at,
    &tiles_clay_at,
    &tiles_copper_at,
    &tiles_iron_at,
    &tiles_gold_at,
    &tiles_demonite_at,
    &tiles_ash_at,
    &tiles_hellstone_at,
    &tiles_obsidian_at,
    &tiles_torch_at,
    &tiles_workbench_at,
    &tiles_furnace_at,
    &tiles_anvil_at,
    &tiles_chest_at,
    &tiles_door_c_at,
    &tiles_door_o_at,
    &tiles_platform_at,
    &tiles_altar_at,
    &tiles_mush_at,
    &tiles_flower_at,
    &tiles_sapling_at,
};

void player_alloc(void);
void save_alloc(void);
void player_draw_swing(uint16_t *fb);
void player_draw_rope(uint16_t *fb);
void proj_draw(uint16_t *fb);
void fx_draw_particles(uint16_t *fb);
void parts_tick(float dt);

static int   s_gen_pct;
static int   s_gen_started, s_gen_hold;
static float s_autosave_t;
static float s_grow_t;
static uint8_t s_liq_flip;

/* ------------------------------------------------------------------ camera -- */
static void camera_update(void) {
    g_cam_x = mote_clampi((int)g_pl.x - MOTE_FB_W / 2, 0, WORLD_W - MOTE_FB_W);
    g_cam_y = mote_clampi((int)g_pl.y - 12 - MOTE_FB_H / 2, 0, WORLD_H - MOTE_FB_H);
}

/* grass caps + biome tint: cosmetic sprites over exposed dirt cells on screen */
static void draw_grass_caps(void) {
    int c0 = g_cam_x / TILE, c1 = (g_cam_x + MOTE_FB_W - 1) / TILE;
    int r0 = g_cam_y / TILE, r1 = (g_cam_y + MOTE_FB_H - 1) / TILE;
    int budget = 40;
    for (int r = r0; r <= r1 && budget; r++) {
        if ((unsigned)r >= WROWS || r > ROW_DIRT_END + 8) continue;
        for (int c = c0; c <= c1 && budget; c++) {
            if ((unsigned)c >= WCOLS) continue;
            if (g_fgm[r * WCOLS + c] != T_DIRT) continue;
            if (r > world_surface_row(c) + 3) continue;   /* grass hugs the surface */
            int up    = !g_tiles[fg_at(c, r - 1)].solid;
            int left  = !g_tiles[fg_at(c - 1, r)].solid;
            int right = !g_tiles[fg_at(c + 1, r)].solid;
            if (!up && !left && !right) continue;
            int cell;
            if (up && left && right) cell = 5;
            else if (up && left)  cell = 1;
            else if (up && right) cell = 2;
            else if (up)          cell = 0;
            else if (left)        cell = 3;
            else                  cell = 4;
            int corrupt = c >= 352;                     /* corruption biome purple */
            MoteSprite s = {
                .img = &grass_cap_img,
                .x = (int16_t)(c * TILE), .y = (int16_t)(r * TILE),
                .fx = (uint16_t)(cell * TILE), .fy = (uint16_t)(corrupt ? TILE : 0),
                .fw = TILE, .fh = TILE, .layer = 3, .flags = 0,
            };
            mote->scene2d_add(&s);
            budget--;
        }
    }
}

static int s_dev_c = -1, s_dev_r = -1;   /* TERRA_POS dev spawn override */
static uint32_t s_dev_seed = 0;          /* TERRA_SEED dev override (0 = use clock) */

/* Mix the raw microsecond clock into a well-distributed 32-bit seed, so worlds
 * differ strongly even when micros() values are small or close together. */
static uint32_t make_seed(void) {
    if (s_dev_seed) return s_dev_seed;
    uint32_t s = (uint32_t)mote->micros();
    s ^= s >> 16; s *= 0x7feb352dU; s ^= s >> 15; s *= 0x846ca68bU; s ^= s >> 16;
    return s ? s : 1u;
}

/* tree dressing: crowns on trunk tops, branch stubs along trunks (derived from
 * the tile map, so chopping the trunk drops the whole tree) */
static void draw_trees(void) {
    int c0 = g_cam_x / TILE - 3, c1 = (g_cam_x + MOTE_FB_W - 1) / TILE + 3;
    int r0 = g_cam_y / TILE - 4, r1 = (g_cam_y + MOTE_FB_H - 1) / TILE + 4;
    int budget = 14;
    for (int c = c0; c <= c1 && budget; c++) {
        if ((unsigned)c >= WCOLS) continue;
        for (int r = r0; r <= r1 && budget; r++) {
            if ((unsigned)r >= WROWS) continue;
            if (fg_at(c, r) != T_TRUNK) continue;
            if (fg_at(c, r - 1) != T_TRUNK) {
                /* trunk top: crown (snow biome gets the snowy variant) */
                int v = world_biome(c) == 1 ? 3 : (int)(mote__at_hash(c, 7) % 3u);
                MoteSprite s = {
                    .img = &canopy_img,
                    .x = (int16_t)(c * TILE + 4 - 20), .y = (int16_t)(r * TILE - 20),
                    .fx = (uint16_t)(v * 40), .fy = 0, .fw = 40, .fh = 28,
                    .layer = 4, .flags = 0,
                };
                mote->scene2d_add(&s);
                budget--;
            } else if (fg_at(c, r + 1) == T_TRUNK) {
                /* mid-trunk: occasional branch, side + leafiness by position hash */
                unsigned h = mote__at_hash(c, r);
                if ((h % 4u) == 0) {
                    int left = (h >> 4) & 1, leafy = ((h >> 5) & 3u) != 0;
                    MoteSprite s = {
                        .img = &branch_img,
                        .x = (int16_t)(left ? c * TILE - 14 : c * TILE + 6),
                        .y = (int16_t)(r * TILE - 4),
                        .fx = (uint16_t)(((leafy ? 2 : 0) + (left ? 0 : 1)) * 16), .fy = 0,
                        .fw = 16, .fh = 12,
                        .layer = 4, .flags = 0,
                    };
                    mote->scene2d_add(&s);
                    budget--;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------- flow --- */
void game_new_world(void) {
    world_generate(make_seed());
    s_gen_pct = 0;
    s_gen_started = 0;
    s_gen_hold = 0;
    g_time = 0.25f;
    g_boss_down = 0;
    g_state = GS_GENERATING;
}

void game_continue(void) {
    if (load_world() && load_player()) {
        player_build_palette();
        npc_reset();
        if (s_dev_c >= 0) {
            g_pl.spawn_c = (int16_t)s_dev_c;
            g_pl.spawn_r = (int16_t)(s_dev_r >= 0 ? s_dev_r : world_surface_row(s_dev_c) - 1);
            player_reset(0);
        }
        g_state = GS_PLAY;
        camera_update();
        ui_toast("WELCOME BACK");
    } else {
        ui_toast("SAVE DAMAGED - NEW WORLD");
        player_reset(1);
        g_state = GS_CREATE;
    }
}

/* pause is a game STATE (not a blocking mote->menu modal), so the OS loop —
 * and with it the engine menu's 3s MENU hold — keeps working while paused. */
static int s_pause_row;
static void ui_pause(uint16_t *fb) {
    for (int i = 0; i < 128 * 128; i++) {
        uint16_t c = fb[i];
        fb[i] = (uint16_t)(((c >> 1) & 0x7BEF));
    }
    mote->draw_rect(fb, 24, 34, 80, 60, MOTE_RGB565(14, 14, 22), 1, 0, 128);
    mote->draw_rect(fb, 24, 34, 80, 60, MOTE_RGB565(120, 120, 140), 0, 0, 128);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->text_font(fb, f, "PAUSED", 41, 34, MOTE_RGB565(240, 220, 120));
    static const char *PR[3] = { "RESUME", "SAVE", "SAVE + QUIT" };
    for (int r = 0; r < 3; r++) {
        if (r == s_pause_row)
            mote->draw_rect(fb, 27, 49 + r * 13, 74, 13, MOTE_RGB565(40, 40, 60), 1, 0, 128);
        mote->text_font(fb, f, PR[r], 33, 49 + r * 13,
                        r == s_pause_row ? MOTE_RGB565(255, 255, 255) : MOTE_RGB565(170, 170, 185));
    }
}
static void pause_tick(void) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_UP))   s_pause_row = (s_pause_row + 2) % 3;
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) s_pause_row = (s_pause_row + 1) % 3;
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU))
        g_state = GS_PLAY;
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (s_pause_row == 0) g_state = GS_PLAY;
        else if (s_pause_row == 1) { save_world(); ui_toast("WORLD SAVED"); g_state = GS_PLAY; }
        else { save_world(); mote->exit_to_launcher(); }
    }
}

/* ------------------------------------------------------------------- init --- */
#include <stdlib.h>
static float s_dev_time = -1.0f;
static void dev_hooks(void) {
    /* host-testing hooks (harmless on device: getenv returns NULL) */
    const char *e;
    if ((e = getenv("TERRA_GIVE"))) {                /* "item:count,item:count" */
        char *p = (char *)e;
        while (*p) {
            int id = (int)strtol(p, &p, 10), n = 1;
            if (*p == ':') n = (int)strtol(p + 1, &p, 10);
            if (id > 0 && id < I_COUNT) inv_add((uint8_t)id, n);
            if (*p == ',') p++; else break;
        }
    }
    if ((e = getenv("TERRA_TIME"))) s_dev_time = (float)atof(e);
    if ((e = getenv("TERRA_SEED"))) s_dev_seed = (uint32_t)strtoul(e, 0, 10);
    if ((e = getenv("TERRA_POS"))) {                 /* "col:row" spawn override */
        char *p = (char *)e;
        s_dev_c = (int)strtol(p, &p, 10);
        if (*p == ':') s_dev_r = (int)strtol(p + 1, &p, 10);
    }
    if (getenv("TERRA_SKIP")) game_new_world();      /* skip title+creator */
}

static void g_init(void) {
    g_mote = mote;
    mote->set_fps_limit(30);
    g_fgm = (uint8_t *)mote->alloc(WCOLS * WROWS);
    g_bgm = (uint8_t *)mote->alloc(WCOLS * WROWS);
    player_alloc();
    save_alloc();
    fx_init();
    audio_init();
    npc_reset();
    player_reset(1);
    dev_hooks();
    mote->log("terramote up");
}

/* ------------------------------------------------------------------ update -- */
static void play_tick(float dt) {
    g_time += dt / DAY_SECONDS;
    if (g_time >= 1.0f) g_time -= 1.0f;

    player_tick(dt);
    npc_tick(dt);
    parts_tick(dt);

    s_liq_flip ^= 1;
    if (s_liq_flip) world_liquid_tick();
    s_grow_t += dt;
    if (s_grow_t > 0.5f) { s_grow_t = 0; world_grow_tick(); }
    s_autosave_t += dt;
    if (s_autosave_t > 120.0f) { s_autosave_t = 0; save_world(); ui_toast("AUTOSAVED"); }

    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_RB)) { g_state = GS_INV; audio_sfx(SFX_TICK, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_MENU)) { g_state = GS_PAUSE; s_pause_row = 0; }
}

static void world_submit(void) {
    camera_update();
    mote->scene2d_begin(g_cam_x, g_cam_y);
    mote->scene2d_set_autotiles(g_fgm, WCOLS, WROWS, k_tiles, T_COUNT - 1);
    draw_grass_caps();
    draw_trees();
    player_draw();
    npc_draw();
}

uint8_t g_ui_fresh;      /* 1 on the first overlay after a state change: UI skips input */

static void g_update(float dt) {
    g_dt = dt;
    ui_tick(dt);
    audio_music_tick();
    fx_light_update();

    switch (g_state) {
    case GS_TITLE:
    case GS_CREATE:
        mote->set_background_cb(0);
        mote->scene_set_background(0x0000);
        break;
    case GS_GENERATING: {
        mote->set_background_cb(0);
        mote->scene_set_background(MOTE_RGB565(16, 14, 22));
        /* One stage per frame, and the FIRST frame does no work — so the
         * "GENERATING 0%" screen is on-screen while the heavy first stages run,
         * and the bar visibly climbs 0->100 instead of flashing one value. */
        if (!s_gen_started) { s_gen_started = 1; break; }
        if (s_gen_pct < 100) { s_gen_pct = world_gen_step(); break; }
        if (++s_gen_hold < 3) break;    /* hold 100% a few frames before the world */
        {
            player_reset(0);            /* keep appearance/inventory, spawn there */
            npc_reset();
            if (s_dev_time >= 0) g_time = s_dev_time;
            if (s_dev_c >= 0) {
                g_pl.spawn_c = (int16_t)s_dev_c;
                if (s_dev_r >= 0) g_pl.spawn_r = (int16_t)s_dev_r;
                else g_pl.spawn_r = (int16_t)(world_surface_row(s_dev_c) - 1);
                player_reset(0);
            }
            g_state = GS_PLAY;
            camera_update();
            mote->set_background_cb(fx_background);
            save_world();
            ui_toast("WELCOME TO TERRAMOTE");
        }
        break;
    }
    case GS_PLAY:
        mote->set_background_cb(fx_background);
        play_tick(dt);
        if (g_state == GS_DEAD) g_dead_t = 3.0f;
        world_submit();
        break;
    case GS_INV:
    case GS_CHEST:
        /* world frozen, still rendered */
        world_submit();
        break;
    case GS_PAUSE:
        pause_tick();
        world_submit();
        break;
    case GS_DEAD:
        g_dead_t -= dt;
        parts_tick(dt);
        world_submit();
        if (g_dead_t <= 0) {
            player_reset(0);
            npc_clear_mobs();          /* the boss (if any) stays on the hunt */
            g_state = GS_PLAY;
        }
        break;
    }
}

/* ----------------------------------------------------------------- overlay -- */
static void g_overlay(uint16_t *fb) {
    static uint8_t prev_state = 255;
    g_ui_fresh = (g_state != prev_state);
    prev_state = g_state;
    switch (g_state) {
    case GS_TITLE:  ui_title(fb); return;
    case GS_CREATE: ui_create(fb); return;
    case GS_GENERATING: {
        const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
        mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 50, rgb(235, 230, 215), "GENERATING WORLD");
        mote_ui_bar(fb, 24, 66, 80, 6, s_gen_pct / 100.0f, rgb(90, 200, 80), rgb(30, 34, 30));
        return;
    }
    default: break;
    }
    /* world states: swing + projectiles under water/darkness */
    player_draw_rope(fb);
    player_draw_swing(fb);
    proj_draw(fb);
    fx_overlay_world(fb);
    fx_draw_particles(fb);
    switch (g_state) {
    case GS_PLAY:  ui_hud(fb); break;
    case GS_INV:   ui_inventory(fb); break;
    case GS_CHEST: ui_chest(fb); break;
    case GS_DEAD:  ui_hud(fb); ui_dead(fb); break;
    case GS_PAUSE: ui_pause(fb); break;
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 128 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("TerraMote", "mote");
MOTE_GAME_VERSION("1.0.0");
