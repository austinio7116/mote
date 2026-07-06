/*
 * ThumbyRogue — an endless real-time isometric hack-n-slash roguelike, ported
 * to the Mote engine.
 *
 * This file is the thin shim between Mote's game contract and ThumbyRogue's
 * platform-agnostic gameplay (the rogue_*.c files) which drive a lean subset
 * of the vendored ThumbyCraft voxel engine (craft_world/blocks/gen/render/
 * torches/tool_models/font/audio). The DDA voxel raycaster writes RGB565
 * straight into Mote's per-band framebuffer; entity cuboids + HUD are layered
 * in overlay() against the engine's z-buffer.
 *
 * Memory: the 256 KB world buffer comes from the Mote arena (mote->alloc); the
 * 64 KB lightmap, 16 KB z-buffer and small animated tile frames are the
 * module's static BSS; the texture atlas is baked const in flash
 * (CRAFT_TEXTURES_BAKED).
 *
 * Render split maps 1:1 onto Mote's vtable (mirrors the host's render_frame):
 *   update()      -> rogue_game_tick + get_camera + craft_render_begin (core 0)
 *   render_band() -> craft_render_strip(cam, fb, y0, y1)               (both cores)
 *   overlay()     -> rogue_game_draw_overlay (entities + HUD)          (core 0)
 */
#include <stddef.h>
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* Defines the weak `mote_game_icon_data[]` the launcher draws (baked from the
 * game-root icon.png by `mote bake`). Harmless const on the host .so. */
#include "icon.h"

#include "rogue_game.h"
#include "craft_buttons.h"      /* CraftRawButtons */
#include "craft_render.h"       /* CraftCamera, craft_render_begin/strip */
#include "craft_world.h"        /* craft_world_init, craft_world_set_buffer, CRAFT_WORLD_VOXELS */
#include "craft_blocks.h"       /* craft_blocks_build_textures, BlockId */
#include "craft_tool_models.h"  /* craft_tool_models_init */
#include "craft_audio.h"        /* craft_audio_render (PCM stream) */
#include "craft_types.h"

/* Arena passthrough for the vendored craft code's allocations (e.g. the light
 * BFS queue — kept off the 4 KB runner stack). NULL when the arena is full. */
void *craft_port_alloc(uint32_t bytes) { return mote->alloc(bytes); }

/* ---- platform RNG hook the engine calls (was device get_rand_32) ---------- */
static uint32_t s_rng = 0x12345678u;
uint32_t craft_platform_rand32(void) {
    /* xorshift32 — seeded from the boot microsecond clock in g_init. */
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (s_rng = x);
}

/* ---- save storage: a single run blob on the engine's per-game save slot 0
 *      (mote->save/load -> /mote/saves/thumbyrogue/0.sav).
 *
 * The blob is the whole RogueSave (~4.2 KB: equip/bag/enemies/ground for a full
 * mid-level suspend). That fits the FAT-backed slot store used by the ThumbyOne
 * slot runner and the host — the intended deployment. NOTE: the *bare* standalone
 * Mote OS backs each slot with a single 4 KB flash sector (4088 B usable), so a
 * RogueSave would be truncated there and fail the load size-check — saves only
 * round-trip on the FAT/slot-runner (or host) builds. ----------------------- */
int rogue_plat_save(const uint8_t *data, int len) {
    return (mote->save && mote->save(0, data, len) > 0) ? 1 : 0;
}
int rogue_plat_load(uint8_t *data, int max) {
    return mote->load ? mote->load(0, data, max) : 0;
}

/* ---- engine hooks ThumbyRogue does not use --------------------------------
 * ThumbyRogue has no redstone (craft_world calls these on block edits) and no
 * craft save-slot picker; no-op / empty them. */
void craft_redstone_note_change(BlockId prev_blk, BlockId new_blk) { (void)prev_blk; (void)new_blk; }
void craft_redstone_rescan(void)     {}
void craft_redstone_mark_dirty(void) {}
bool craft_save_slot_used(int slot)             { (void)slot; return false; }
const uint16_t *craft_save_slot_thumb(int slot) { (void)slot; return NULL; }

/* Camera pose for the current frame: filled in update(), consumed by every
 * render_band() call (which can run on both cores). */
static CraftCamera s_cam;

/* ---- vtable callbacks ----------------------------------------------------- */
static void g_init(void) {
    /* Seed the PRNG from the boot clock so each launch gets a fresh dungeon. */
    uint64_t t = mote->micros();
    s_rng = (uint32_t)(t ^ (t >> 32)) | 1u;

    /* The 256 KB world buffer and 16 KB z-buffer are too big for the module's
     * static BSS (GAME_RAM is 134 KB, dominated by the 64 KB voxel lightmap) —
     * take them from the 272 KB engine arena and inject them before any
     * world/render access. (world + zbuf = 272 KB fills the arena exactly.) */
    craft_world_set_buffer(mote->alloc(CRAFT_WORLD_VOXELS));
    craft_render_set_zbuf(mote->alloc(CRAFT_FB_W * CRAFT_FB_H));

    /* Engine bring-up, in the host's order: world -> textures -> tool models. */
    craft_world_init();
    craft_blocks_build_textures();   /* no-op under CRAFT_TEXTURES_BAKED */
    craft_tool_models_init();

    /* Starts a fresh run (or resumes a saved one via rogue_plat_load).
     * Internally calls rogue_sfx_init() -> craft_audio_init(). */
    rogue_game_init(craft_platform_rand32());

    /* Route ThumbyRogue's procedural SFX synth into the engine mixer. Its pull
     * callback (CRAFT_AUDIO_RATE = 22050) matches the stream signature. */
    mote->audio_set_stream(craft_audio_render);
}

static void g_update(float dt) {
    const MoteInput *m = mote->input();
    CraftRawButtons btn = {0};
    btn.up    = m->held[MOTE_BTN_UP];
    btn.down  = m->held[MOTE_BTN_DOWN];
    btn.left  = m->held[MOTE_BTN_LEFT];
    btn.right = m->held[MOTE_BTN_RIGHT];
    btn.a     = m->held[MOTE_BTN_A];
    btn.b     = m->held[MOTE_BTN_B];
    btn.lb    = m->held[MOTE_BTN_LB];
    btn.rb    = m->held[MOTE_BTN_RB];
    btn.menu  = m->held[MOTE_BTN_MENU];

    rogue_game_tick(&btn, dt);

    /* Latch the camera + precompute the raycaster basis for this frame's bands. */
    rogue_game_get_camera(&s_cam);
    craft_render_begin(&s_cam);
}

static void g_render_band(uint16_t *fb, int y0, int y1) {
    craft_render_strip(&s_cam, fb, y0, y1);
}

static void g_overlay(uint16_t *fb) {
    rogue_game_draw_overlay(fb);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = g_render_band,
    .overlay = g_overlay,
    .config = { 0 },   /* render_band owns the frame: no engine 3D pools, depth=0 */
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("ThumbyRogue", "austinio7116");
MOTE_GAME_VERSION("1.0.0");
