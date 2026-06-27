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
#include "craft_save.h"
#include "craft_player.h"   /* CraftInput */
#include "craft_world.h"    /* craft_world_set_buffer, CRAFT_WORLD_VOXELS */
#include "craft_audio.h"    /* craft_audio_render (PCM stream) */
#include "craft_types.h"

/* Arena passthrough for the vendored code's lazy allocations (e.g. the save
 * thumbnail). Returns NULL when the arena is exhausted; callers must cope. */
void *craft_port_alloc(uint32_t bytes) { return mote->alloc(bytes); }

/* KV-blob passthrough (ABI v38) — the chunk store persists per-chunk edits here. */
int  craft_port_kv_save(const char *key, const void *data, int len) { return mote->kv_save ? mote->kv_save(key, data, len) : 0; }
int  craft_port_kv_load(const char *key, void *data, int max)       { return mote->kv_load ? mote->kv_load(key, data, max) : 0; }
void craft_port_kv_list(const char *prefix, void (*cb)(const char *, void *), void *arg) { if (mote->kv_list) mote->kv_list(prefix, cb, arg); }

/* The one big save buffer (see craft_save.h): the serialised world record, also
 * reused as the chunk-store blob — they never run at the same time. */
uint8_t craft_save_scratch[CRAFT_SAVE_SCRATCH_BYTES];

/* The world RECORD (seed/player/inventory) rides the engine's per-game save
 * slots (mote->save/load → /mote/saves/thumbycraft/<slot>.sav). The chunk store
 * streams per-chunk edits separately through kv_* above. */
static int craft_port_save_write(int slot, const void *data, int len) { return mote->save ? mote->save(slot, data, len) : 0; }
int        craft_port_save_read (int slot, void *data, int max)        { return mote->load ? mote->load(slot, data, max) : 0; }

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

    /* Drain the menu/title save-load requests (mirrors the standalone device's
     * drain_requests): record -> mote->save slot; load -> mote->load + restore.
     * craft_main_save force-persists dirty chunks (through kv_*) and stamps the
     * nonce internally; craft_main_load pre-reads the nonce and binds the store. */
    if (craft_main_take_save_request()) {
        int slot = craft_main_save_slot();
        size_t n = craft_main_save(craft_save_scratch, CRAFT_SAVE_SCRATCH_BYTES);
        if (n > 0) {
            craft_port_save_write(slot, craft_save_scratch, (int)n);
            /* Stash the slot's preview thumbnail as a blob (the picker loads it via
             * craft_save_slot_thumb). Captured by craft_main on the autosave. */
            const uint16_t *th = craft_main_thumb();
            if (th && slot >= 0 && slot < 10) {
                char k[4]; k[0]='t'; k[1]='h'; k[2]=(char)('0'+slot); k[3]=0;
                craft_port_kv_save(k, th, CRAFT_SAVE_THUMB_DIM * CRAFT_SAVE_THUMB_DIM * 2);
            }
        }
    }
    if (craft_main_take_load_request()) {
        int slot = craft_main_save_slot();
        int n = craft_port_save_read(slot, craft_save_scratch, CRAFT_SAVE_SCRATCH_BYTES);
        if (n > 0) craft_main_load(craft_save_scratch, (size_t)n);
    }
    (void)craft_main_take_new_world_request();   /* the engine resets the world itself */

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
