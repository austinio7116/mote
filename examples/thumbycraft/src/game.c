/*
 * ThumbyCraft — a full Minecraft-style voxel sandbox, ported to the Mote engine.
 *
 * This file is only the thin shim between Mote's game contract and ThumbyCraft's
 * engine (the vendored craft_*.c files, unchanged). The whole game — DDA voxel
 * raycaster, infinite procedural worldgen with 8 biomes, mobs, redstone, torches
 * & lighting, day/night, crafting, combat — lives in those files.
 *
 * Memory: the 256 KB world buffer comes from the Mote arena (mote->alloc); the
 * lightmap, z-buffer and maps are the module's static BSS; the 157 KB texture
 * atlas is baked const in flash (CRAFT_TEXTURES_BAKED).
 *
 * Render: ThumbyCraft already splits a frame the way Mote's vtable wants —
 *   update()      -> craft_main_tick + craft_main_render_begin   (core 0)
 *   render_band() -> craft_main_render_strip(y0,y1)              (both cores)
 *   overlay()     -> craft_main_draw_hud                         (core 0)
 * and both already do dual-core top/bottom banding, so it maps 1:1.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "craft_main.h"
#include "craft_player.h"   /* CraftInput */
#include "craft_world.h"    /* craft_world_set_buffer, CRAFT_WORLD_VOXELS */
#include "craft_audio.h"    /* craft_audio_render (PCM stream) */
#include "craft_types.h"

/* Arena passthrough for the vendored code's lazy allocations (e.g. the save
 * thumbnail). Returns NULL when the arena is exhausted; callers must cope. */
void *craft_port_alloc(uint32_t bytes) { return mote->alloc(bytes); }

/* ---- platform RNG hook the engine calls (was device get_rand_32) ---------- */
static uint32_t s_rng = 0x12345678u;
uint32_t craft_platform_rand32(void) {
    /* xorshift32 — seeded from the boot microsecond clock in g_init. */
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (s_rng = x);
}

/* ---- vtable callbacks ----------------------------------------------------- */
static void g_init(void) {
    /* Seed the PRNG from the boot clock so each launch gets a fresh world. */
    uint64_t t = mote->micros();
    s_rng = (uint32_t)(t ^ (t >> 32)) | 1u;

    /* The world buffer (256 KB) and z-buffer (16 KB) are too big for the
     * module's 128 KB static-BSS region — take them from the engine arena
     * (272 KB) and inject them before any world/render access. */
    craft_world_set_buffer(mote->alloc(CRAFT_WORLD_VOXELS));
    craft_render_set_zbuf(mote->alloc(CRAFT_FB_W * CRAFT_FB_H));

    /* fb is handed to us per render_band/overlay, not now — pass NULL; the
     * shim sets it each frame via craft_main_set_fb. */
    craft_main_init(NULL, craft_platform_rand32());

    /* Route ThumbyCraft's own synth (music + SFX) into the engine mixer. Its
     * pull callback matches the engine's stream signature exactly. */
    mote->audio_set_stream(craft_audio_render);
}

static void g_update(float dt) {
    const MoteInput *m = mote->input();
    CraftInput in = {0};
    in.up    = m->held[MOTE_BTN_UP];
    in.down  = m->held[MOTE_BTN_DOWN];
    in.left  = m->held[MOTE_BTN_LEFT];
    in.right = m->held[MOTE_BTN_RIGHT];
    in.a     = m->held[MOTE_BTN_A];
    in.b     = m->held[MOTE_BTN_B];
    in.lb    = m->held[MOTE_BTN_LB];
    in.rb    = m->held[MOTE_BTN_RB];
    in.menu  = m->held[MOTE_BTN_MENU];
    in.a_pressed    = m->just_pressed[MOTE_BTN_A];
    in.b_pressed    = m->just_pressed[MOTE_BTN_B];
    in.lb_pressed   = m->just_pressed[MOTE_BTN_LB];
    in.rb_pressed   = m->just_pressed[MOTE_BTN_RB];
    in.menu_pressed = m->just_pressed[MOTE_BTN_MENU];
    in.a_long    = m->hold_ms[MOTE_BTN_A]    >= 400;
    in.menu_long = m->hold_ms[MOTE_BTN_MENU] >= 400;

    craft_main_tick(&in, dt);
    craft_main_render_begin();
}

static void g_render_band(uint16_t *fb, int y0, int y1) {
    craft_main_set_fb(fb);
    craft_main_render_strip(y0, y1);
}

static void g_overlay(uint16_t *fb) {
    craft_main_set_fb(fb);
    uint32_t p[6]; mote->perf(p);
    craft_main_draw_hud((int)p[0]);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = g_render_band,
    .overlay = g_overlay,
    .config = { 0 },   /* render_band owns the frame: no engine 3D pools, depth=0 */
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
