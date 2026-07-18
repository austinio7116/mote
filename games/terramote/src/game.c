/* TerraMote — a Terraria-style sandbox for the Thumby Color.
 * Dig, build, craft, fight. See DESIGN.md. */
#define TERRA_MAIN
#include "terra.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
#include "tiles_table.tiles.h"
#include "tiles_chair.tiles.h"
#include "tiles_lantern.tiles.h"
#include "tiles_fireplace.tiles.h"
#include "tiles_chain.tiles.h"
#include "tiles_roof.tiles.h"
#include "tiles_beam.tiles.h"
#include "tiles_brick_clay.tiles.h"
#include "tiles_brick_stone.tiles.h"
#include "grass_cap.h"                       /* cosmetic caps over exposed dirt */
#include "canopy.h"                          /* tree crowns: 3 leafy + 1 snowy, 40x28 */
#include "branch.h"                          /* trunk branches: 16x12 x4 */

const MoteApi *g_mote;
uint8_t g_state = GS_TITLE;
float   g_time = 0.25f;
uint8_t g_boss_down;
uint8_t g_autosave_opt = 2;              /* 2 minutes, as it always was */
static float s_dev_autosave = -1.0f;     /* TERRA_AUTOSAVE seconds override (tests) */
static const float k_autosave_secs[4] = { 0, 60.0f, 120.0f, 300.0f };
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
    &tiles_table_at,
    &tiles_chair_at,
    &tiles_lantern_at,
    &tiles_fireplace_at,
    &tiles_chain_at,
    &tiles_roof_at,
    &tiles_beam_at,
    &tiles_brick_clay_at,
    &tiles_brick_stone_at,
};

void player_alloc(void);
void save_alloc(void);
void player_draw_swing(uint16_t *fb);
void player_draw_rope(uint16_t *fb);
void proj_draw(uint16_t *fb);
void fx_draw_particles(uint16_t *fb);
void parts_tick(float dt);
void net_enter_play(void);            /* net.c sync -> play transition helpers */
void net_apply_meta(void);
void net_guest_spawn(int *c, int *r);
void fx_draw_ftext(uint16_t *fb);

static int   s_gen_pct;
static int   s_gen_started, s_gen_hold;
static float s_autosave_t;
static float s_grow_t;
static uint8_t s_liq_flip;
static uint8_t s_save_manual, s_quit_after_save;

/* manual world save: same incremental path as the autosave (no frame hitch,
 * no link silence). quit_after=1 leaves for the launcher once the save lands. */
void game_save_start(int quit_after) {
    s_save_manual = 1;
    if (quit_after) s_quit_after_save = 1;             /* even if a save is mid-flight */
    if (save_world_busy()) return;
    save_world_begin();
    net_ev_saving(1);
    ui_toast("SAVING WORLD...");
}

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
static float s_dev_time = -1.0f;         /* TERRA_TIME dev override */

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
                /* trunk top: crown — snow gets the snowy variant, corruption a dead one */
                int bio = world_biome(c);
                int v = bio == 1 ? 3 : bio == 3 ? 4 : (int)(mote__at_hash(c, 7) % 3u);
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

/* ---- co-op entries (title menu sets g_net_pending; we run them from the
 * update path because net_begin blocks inside the engine lobby modal) -------- */
int g_net_pending;                     /* 0 none · 1 host · 2 join */

static void back_to_title(void) {
    npc_reset();
    world_title_scene();
    g_time = 0.30f;
    g_cam_x = (WCOLS / 2) * TILE - MOTE_FB_W / 2;
    g_cam_y = world_surface_row(WCOLS / 2) * TILE - 86;
    g_state = GS_TITLE;
}

static void game_host_coop(void) {
    if (!(load_world() && load_player())) {
        ui_toast("SAVE DAMAGED");
        return;
    }
    player_build_palette();
    npc_reset();
    if (s_dev_time >= 0) g_time = s_dev_time;   /* TERRA_TIME works in co-op too */
    if (getenv("TERRA_NET")) net_begin_direct(1);
    else net_begin(1);                 /* blocking lobby; sets GS_NET_SYNC or returns to title */
}

static void game_join_coop(void) {
    /* bring YOUR character into their world (fresh explorer if none saved) */
    if (load_player()) player_build_palette();
    npc_reset();
    if (getenv("TERRA_NET")) net_begin_direct(0);
    else net_begin(0);
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
        else if (s_pause_row == 1) {
            if (net_guest()) { save_player_coop(); ui_toast("CHARACTER SAVED"); }
            else game_save_start(0);                 /* incremental: no hitch, no link silence */
            g_state = GS_PLAY;
        } else {
            if (net_guest()) {
                save_player_coop();
                net_stop(1);           /* wave goodbye before leaving */
                mote->exit_to_launcher();
            } else {
                game_save_start(1);    /* incremental; quits when the last step lands */
                g_state = GS_PLAY;
            }
        }
    }
}

/* ------------------------------------------------------------------- init --- */
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
    if ((e = getenv("TERRA_HOT"))) g_pl.hot = (uint8_t)atoi(e);   /* preselect a hotbar slot */
    if ((e = getenv("TERRA_TIME"))) s_dev_time = (float)atof(e);
    if ((e = getenv("TERRA_AUTOSAVE"))) s_dev_autosave = (float)atof(e);   /* seconds; tests */
    if ((e = getenv("TERRA_SEED"))) s_dev_seed = (uint32_t)strtoul(e, 0, 10);
    if ((e = getenv("TERRA_POS"))) {                 /* "col:row" spawn override */
        char *p = (char *)e;
        s_dev_c = (int)strtol(p, &p, 10);
        if (*p == ':') s_dev_r = (int)strtol(p + 1, &p, 10);
    }
    if (getenv("TERRA_SKIP")) game_new_world();      /* skip title+creator */
    /* TERRA_NET=host|join — co-op over the raw socket/USB link, no lobby UI
     * (two headless instances with the same MOTE_LINK_SOCK pair up) */
    if ((e = getenv("TERRA_NET"))) {
        if (e[0] == 'h') game_host_coop();
        else game_join_coop();
    }
}

/* TERRA_BUILD="f:c:r:tile,w:c:r:wall,..." — stamp tiles/walls post-gen (tests) */
static void dev_build(void) {
    const char *e = getenv("TERRA_BUILD");
    if (!e) return;
    char *p = (char *)e;
    while (*p) {
        char kind = *p;
        if ((kind == 'f' || kind == 'w') && p[1] == ':') {
            p += 2;
            int c = (int)strtol(p, &p, 10); if (*p == ':') p++;
            int r = (int)strtol(p, &p, 10); if (*p == ':') p++;
            int id = (int)strtol(p, &p, 10);
            if (kind == 'f') world_set_fg(c, r, (uint8_t)id);
            else world_set_wall(c, r, (uint8_t)id);
        } else p++;
        if (*p == ',') p++;
    }
    world_rebuild_caches();
}

static void g_init(void) {
    g_mote = mote;
    mote->set_fps_limit(30);
    g_fgm = (uint8_t *)mote->alloc(WCOLS * WROWS);
    g_bgm = (uint8_t *)mote->alloc(WCOLS * WROWS);
    player_alloc();
    player_net_alloc();
    save_alloc();
    fx_init();
    audio_init();
    npc_reset();
    player_reset(1);
    /* the title menu floats over a REAL rendered forest strip */
    world_title_scene();
    g_time = 0.30f;
    g_cam_x = (WCOLS / 2) * TILE - MOTE_FB_W / 2;
    g_cam_y = world_surface_row(WCOLS / 2) * TILE - 86;
    dev_hooks();
    mote->log("terramote up");
}

/* ------------------------------------------------------------------ update -- */
/* the world simulation shared by open play and (in co-op) the menu states —
 * with a friend connected the world can never freeze under a menu */
static void sim_tick(float dt, int with_player) {
    int authority = !net_active() || net_is_host();   /* guest: host owns the world sim */

    g_time += dt / DAY_SECONDS;                        /* guest drift corrected by 'S' */
    if (g_time >= 1.0f) g_time -= 1.0f;

    if (with_player) player_tick(dt);
    npc_tick(dt);
    parts_tick(dt);

    s_liq_flip ^= 1;
    if (s_liq_flip) {
        if (authority) world_liquid_tick();
    } else if (net_is_host()) {
        float px, py;                                  /* liquids also flow at the friend */
        if (net_peer_pos(&px, &py)) world_liquid_tick_at((int)px, (int)py);
    }
    s_grow_t += dt;
    if (s_grow_t > 0.5f) { s_grow_t = 0; if (authority) world_grow_tick(); }
    s_autosave_t += dt;
    {
        float ivl = s_dev_autosave >= 0 ? s_dev_autosave : k_autosave_secs[g_autosave_opt & 3];
        if (net_guest()) {                             /* guest: character only */
            if (ivl > 0 && s_autosave_t > ivl) { s_autosave_t = 0; save_player_coop(); }
        } else {
            /* INCREMENTAL saving (auto AND manual): one kv key per frame, so
             * the frame never hitches and a co-op host never goes link-silent
             * (atomic saves tripped the engine's LINK STALLED banner on the
             * guest — including manual ones, per the user's field test) */
            if (ivl > 0 && s_autosave_t > ivl && !save_world_busy()) {
                s_autosave_t = 0;
                s_save_manual = 0;
                save_world_begin();
                net_ev_saving(1);                      /* friend sees "HOST IS SAVING..." */
                ui_toast("AUTOSAVING...");
            }
            if (save_world_busy() && !save_world_step()) {
                ui_toast(s_save_manual ? "WORLD SAVED" : "AUTOSAVED");
                net_ev_saving(0);
                s_save_manual = 0;
                if (s_quit_after_save) {
                    s_quit_after_save = 0;
                    net_stop(1);
                    mote->exit_to_launcher();
                }
            }
        }
    }
    net_tick(dt);
}

/* co-op: keep the world + link alive under menus; the player reads no input */
static void coop_menu_tick(float dt, int with_player) {
    if (!net_active()) return;
    g_pl_freeze = 1;
    sim_tick(dt, with_player);
    g_pl_freeze = 0;
    if (net_failed()) { net_stop(0); back_to_title(); }
}

static void play_tick(float dt) {
    {   /* TERRA_MOBS=<kind> dev hook: spawn once, a beat after play starts */
        static float t = -1.0f; const char *e = getenv("TERRA_MOBS");
        if (e) { if (t < 0) t = 0; t += dt;
            static int done; if (!done && t > 0.4f) { done = 1; void npc_dev_spawn(int); npc_dev_spawn(atoi(e)); } }
    }
    sim_tick(dt, 1);

    const MoteInput *in = mote->input();
    /* MENU opens the tabbed menu (Items / Craft / Game); LB+RB are the hotbar
     * selectors during play (handled in player.c) */
    if (mote_just_pressed(in, MOTE_BTN_MENU)) { g_state = GS_INV; audio_sfx(SFX_TICK, 0.6f); }
}

static void world_submit(void) {
    if (g_state != GS_TITLE) camera_update();      /* title pans its own camera */
    mote->scene2d_begin(g_cam_x, g_cam_y);
    mote->scene2d_set_autotiles(g_fgm, WCOLS, WROWS, k_tiles, T_COUNT - 1);
    draw_grass_caps();
    draw_trees();
    if (g_state != GS_TITLE) player_draw();        /* nobody stands in the vista */
    net_draw_remote();                             /* the co-op friend */
    npc_draw();
}

uint8_t g_ui_fresh;      /* 1 on the first overlay after a state change: UI skips input */

static void g_update(float dt) {
    g_dt = dt;
    {   /* The OS resets the fps limit after init, so a cap must be re-armed
         * from update. 60 matches the GC9107's own panel refresh: frames past
         * it are never shown (they overwrite GRAM mid-scan and only add
         * tearing + battery burn), and the limiter degrades gracefully when a
         * scene costs more than 16.6ms. The sim is dt-based throughout.
         * Headless LINK TESTS pace at 30 so two instances stay in step. */
        static uint8_t s_fps_set;
        if (!s_fps_set) {
            s_fps_set = 1;
            if (getenv("TERRA_NET") || getenv("TERRA_FPS30")) mote->set_fps_limit(30);
            else mote->set_fps_limit(60);
        }
    }
    {   /* dev: trace state flips (TERRA_DBG) */
        static int s_dbg_on = -1; static uint8_t prev = 255; static int frame;
        if (s_dbg_on < 0) s_dbg_on = getenv("TERRA_DBG") != 0;
        frame++;
        if (s_dbg_on && g_state != prev) {
            char b[48];
            snprintf(b, sizeof b, "dbg state %d->%d f=%d", prev, g_state, frame);
            mote->log(b);
            prev = g_state;
        }
    }
    ui_tick(dt);
    audio_music_tick();
    fx_light_update();

    switch (g_state) {
    case GS_TITLE: {
        if (g_net_pending) {                       /* co-op picked on the menu */
            int host = g_net_pending == 1;
            g_net_pending = 0;
            if (host) game_host_coop();
            else game_join_coop();
            break;
        }
        /* live world render behind the menu: slow pan across the title forest */
        static float drift;
        drift += dt * 6.0f;
        if (drift >= 1.0f) {
            int step = (int)drift; drift -= (float)step;
            g_cam_x += step;
            if (g_cam_x > (WCOLS - 20) * TILE) g_cam_x = 4 * TILE;
        }
        g_cam_y = world_surface_row((g_cam_x + MOTE_FB_W / 2) / TILE) * TILE - 86;
        mote->set_background_cb(fx_background);
        parts_tick(dt);
        world_submit();
        break;
    }
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
            dev_build();               /* TERRA_BUILD test structures */
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
        if (net_failed()) {            /* dropped mid-game (guest): save + bail out */
            save_player_coop();
            net_stop(0);
            back_to_title();
            break;
        }
        world_explore_view();          /* fog of war: reveal what you can see */
        if (g_state == GS_DEAD) g_dead_t = 3.0f;
        world_submit();
        break;
    case GS_INV:
    case GS_CHEST:
        /* solo: world frozen, still rendered · co-op: it keeps running */
        coop_menu_tick(dt, 1);
        world_submit();
        break;
    case GS_PAUSE:
        coop_menu_tick(dt, 1);
        pause_tick();
        world_submit();
        break;
    case GS_DEAD:
        g_dead_t -= dt;
        coop_menu_tick(dt, 0);         /* co-op: the world doesn't wait for a corpse */
        parts_tick(dt);
        world_submit();
        if (g_dead_t <= 0) {
            player_reset(0);
            if (!net_guest()) npc_clear_mobs();   /* the boss (if any) stays on the hunt */
            g_pl.iframes = 4.0f;       /* respawn protection — time to run or gear up */
            g_state = GS_PLAY;
        }
        break;
    case GS_NET_SYNC:
        mote->set_background_cb(0);
        mote->scene_set_background(MOTE_RGB565(16, 14, 22));
        net_tick(dt);
        if (net_failed()) {
            net_stop(0);
            back_to_title();
            break;
        }
        if (net_ready()) {
            net_enter_play();
            int host = net_is_host();
            if (!host) {
                net_apply_meta();
                int sc, sr;
                net_guest_spawn(&sc, &sr);
                g_pl.spawn_c = (int16_t)sc;
                g_pl.spawn_r = (int16_t)sr;
                memset(g_explored, 0, sizeof(g_explored));   /* your own fog of war */
                player_reset(0);
            }
            npc_reset();
            g_state = GS_PLAY;
            camera_update();
            mote->set_background_cb(fx_background);
            ui_toast(host ? "YOUR FRIEND IS HERE!" : "WELCOME, FRIEND!");
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
    case GS_NET_SYNC: {
        const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
        mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 44, rgb(235, 230, 215), "CO-OP");
        mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 58, rgb(170, 200, 235), net_phase_text());
        mote_ui_bar(fb, 24, 74, 80, 6, net_progress() / 100.0f, rgb(90, 160, 235), rgb(28, 30, 38));
        return;
    }
    default: break;
    }
    /* world states: flames, then particles BEHIND the held weapon, then the
     * swing + projectiles, darkness, and damage text on top */
    fx_draw_flames(fb);
    player_draw_rope(fb);
    fx_draw_particles(fb);
    player_draw_swing(fb);
    net_draw_remote_overlay(fb);       /* the friend's swing + rope */
    proj_draw(fb);
    fx_overlay_world(fb);
    fx_draw_ftext(fb);
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
MOTE_GAME_VERSION("1.3.3");
