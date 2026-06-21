/*
 * Mote — the game ABI. THE forever-stable contract between a game
 * module and the resident engine/OS.
 *
 * A game NEVER links libte. It is handed a `const MoteApi *` (a jump table of
 * engine function pointers) and calls everything through it. Math (mote_vec.h)
 * and data formats (mote_mesh.h) are header-only, so the game compiles those in
 * directly — only stateful engine subsystems go through the table.
 *
 * Loading (host: dlopen a .so; device: map/copy a .mote and jump):
 *   1. OS resolves the game's exported abi version, refuses a mismatch.
 *   2. OS calls mote_game_register(api) -> the game stashes `api` and returns
 *      its MoteGameVtbl.
 *   3. OS owns the frame loop and calls the vtable callbacks.
 *
 * ABI rule: only ever APPEND to MoteApi / MoteGameVtbl, never reorder or remove.
 * Bump MOTE_ABI_VERSION on any append; the OS accepts games built against any
 * version <= its own (new fields a stale game never calls stay harmless).
 */
#ifndef MOTE_API_H
#define MOTE_API_H

#include <stdint.h>
#include <stdbool.h>

#include "mote_vec.h"      /* Vec3, Mat3 — header-only */
#include "mote_mesh.h"     /* Mesh / MeshVert / MeshFace — data only */
#include "mote_input.h"    /* MoteInput, MoteButtons, MoteBtnId */
#include "mote_object.h"   /* MoteObject — header-only */
#include "mote_2d.h"       /* MoteImage/Tileset/Tilemap/Sprite — header-only */
#include "mote_phys.h"     /* MoteWorld/MoteBody — header-only */
#include "mote_splat.h"    /* MoteSplat — Gaussian-splat renderer */

#define MOTE_ABI_VERSION 11u  /* v11: styled modal menu (menu) */

/* ---------------------------------------------------------------------------
 * MoteConfig — the game declares the resource pools it needs. The OS sizes the
 * engine's buffers to THIS at load (not a fixed worst case), allocates them
 * from one shared SRAM arena, and hands the remainder to the game via
 * MoteApi.alloc. A pool left 0 costs nothing. If the declared pools + the
 * game's allocations don't fit, the loader refuses to launch (clear failure).
 * This is the whole resource model: you pay only for what you declare.
 * ------------------------------------------------------------------------- */
typedef struct MoteConfig {
    uint16_t max_tris;        /* 3D triangle draw-list capacity (0 = no 3D raster) */
    uint16_t max_spheres;     /* sphere impostors in the 3D pass */
    uint16_t max_splats;      /* Gaussian splats (0 = none) */
    uint16_t max_sprites;     /* 2D sprites per frame (0 = no 2D scene) */
    uint16_t max_bodies;      /* physics bodies (0 = no physics engine) */
    uint16_t max_contacts;    /* physics contact manifolds */
    uint16_t max_mesh_tris;   /* largest mesh collider's triangle count (0 = none) */
    uint8_t  depth;           /* 1 = allocate the 32KB depth buffer (3D / splats) */
} MoteConfig;

/* ---------------------------------------------------------------------------
 * The engine jump table. Populated by the OS, called by the game.
 * ------------------------------------------------------------------------- */
typedef struct MoteApi {
    uint32_t abi_version;          /* == the OS's MOTE_ABI_VERSION */

    /* Input — current frame's derived button state (valid during update). */
    const MoteInput *(*input)(void);

    /* 3D scene (triangle pipeline). Build the frame in update(); the OS
     * rasterises it across both cores. */
    void (*scene_set_background)(uint16_t rgb565);
    void (*scene_set_sun)(Vec3 dir_toward_light_world);
    void (*scene_begin)(const Mat3 *cam_basis, float fov_deg);
    int  (*scene_add_object)(const MoteObject *obj);
    int  (*scene_add_object_scaled)(const MoteObject *obj, float scale);
    int  (*scene_tri_count)(void);

    /* Control / misc. */
    uint64_t (*micros)(void);
    void     (*exit_to_launcher)(void);

    /* --- ABI v2: 2D scene (sprites + tilemap), rastered AFTER the 3D scene
     * (both banded across cores). Pure-2D, pure-3D, or hybrid (3D + 2D HUD).
     * Build in update(); the OS rasters it. APPEND-ONLY past this point. */
    void (*scene2d_begin)(int cam_x, int cam_y);
    void (*scene2d_set_tilemap)(const MoteTilemap *map, const MoteTileset *tiles);
    int  (*scene2d_add)(const MoteSprite *spr);
    /* Immediate-mode sprite blit (HUD/UI), band-clipped + colour-keyed. */
    void (*blit)(uint16_t *fb, const MoteImage *img, int x, int y,
                 int fx, int fy, int fw, int fh, uint8_t flags, int y0, int y1);

    /* --- ABI v3: rigid-sphere physics. The game owns the body array; the
     * engine runs the solver on it. */
    void     (*phys_world_defaults)(MoteWorld *w);
    uint32_t (*phys_step)(MoteWorld *w, MoteBody *bodies, int n, float dt);

    /* --- ABI v4: per-pixel shaded sphere impostor (camera-relative position),
     * drawn in the 3D scene pass (depth-tested with meshes). */
    int (*scene_add_sphere)(Vec3 cam_rel_pos, float radius, uint16_t color);

    /* --- ABI v5: bitmap text into the framebuffer (HUD/overlay). Returns the
     * advanced x. Draw from overlay(). */
    int (*text)(uint16_t *fb, const char *s, int x, int y, uint16_t color);
    int (*text_2x)(uint16_t *fb, const char *s, int x, int y, uint16_t color);

    /* --- ABI v6: telemetry. log() streams a line to the host (`mote logs`);
     * perf() fills [fps, update_us, raster_us, flush_us, core0_pct, core1_pct]
     * for the latest frame. APPEND-ONLY. */
    void (*log)(const char *s);
    void (*perf)(uint32_t out[6]);

    /* --- ABI v7: physics queries (no simulation). Static colliders, per-body
     * materials and optional walls are plain MoteBody/MoteWorld fields (set them
     * directly); these are the function-table entries. APPEND-ONLY. */
    int (*phys_raycast)(const MoteWorld *w, const MoteBody *bodies, int n,
                        Vec3 origin, Vec3 dir, float max_dist, int skip, MoteRayHit *hit);
    int (*phys_overlap)(const MoteWorld *w, const MoteBody *bodies, int n,
                        Vec3 center, float radius, int *out, int max);

    /* --- ABI v8: Gaussian-splat renderer. Call from overlay() with the frame
     * buffer (blends OVER the rastered 3D scene), or with a cleared bg. `order`
     * is your scratch of >= n ints. Pass depth_buffer() to have the rastered
     * scene (e.g. terrain) occlude splats behind it, or NULL. APPEND-ONLY. */
    int (*splat_render)(uint16_t *fb, const MoteSplat *splats, int n,
                        const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg,
                        int *order, const uint16_t *depth);
    const uint16_t *(*depth_buffer)(void);   /* scene depth (K/z, larger=nearer) */

    /* Register a splat cloud to render THIS frame as a measured, dual-core
     * banded pass AFTER the 3D scene (composites with depth, and its cost shows
     * in the perf graph instead of hiding in overlay()). Call from update();
     * `order` is your scratch of >= n ints, `depth` from depth_buffer() or NULL.
     * Preferred over splat_render() for anything heavy. APPEND-ONLY. */
    void (*scene_set_splats)(const MoteSplat *splats, int n, int *order,
                             const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg,
                             const uint16_t *depth);

    /* --- ABI v9: load-time arena. alloc() carves the game's own large buffers
     * out of the SAME arena the engine pools came from (sized by MoteConfig),
     * so a lean game keeps the slack. 8-byte aligned + zeroed; NULL if the
     * arena is exhausted. Valid from init() onward; freed wholesale on exit.
     * arena_free() reports the bytes left for the game. */
    void   *(*alloc)(uint32_t bytes);
    uint32_t (*arena_free)(void);

    /* --- ABI v10: audio. A small polyphonic synth mixed to 22050 Hz (SDL on the
     * host, 12-bit PWM on the device). audio_note() strikes a one-shot note with
     * a piano-ish decay (freq in Hz, amp 0..1) — fire one per key press; voices
     * are stolen when all 8 are busy. audio_off() silences everything. Master
     * volume follows the engine menu. */
    void (*audio_note)(float freq, float amp);
    void (*audio_off)(void);

    /* --- ABI v11: a styled modal menu, in the system look (the launcher / engine
     * menu share it). Pops up over the current frame, lists `items` (n labels),
     * UP/DOWN to move; returns the chosen index (A) or -1 (B / quit). Blocking —
     * call it from update() for pause / game-over / level menus. */
    int (*menu)(const char *title, const char *const *items, int n);
} MoteApi;

/* ---------------------------------------------------------------------------
 * The game's side of the contract: callbacks the OS drives.
 * ------------------------------------------------------------------------- */
typedef struct MoteGameVtbl {
    void (*init)(void);                                  /* once, after register */
    void (*update)(float dt);                            /* per frame (core0) */
    /* Optional custom band renderer (raycaster games). NULL => the OS uses
     * the built-in 3D scene rasteriser. Called from BOTH cores with disjoint
     * row bands [y0,y1). */
    void (*render_band)(uint16_t *fb, int y0, int y1);
    void (*overlay)(uint16_t *fb);                       /* HUD, core0, optional */
    /* --- ABI v9: resource pools. The OS reads this BEFORE init() to size the
     * engine arena. Leave a field 0 to opt out of that subsystem entirely. */
    MoteConfig config;
} MoteGameVtbl;

/* The game module's single entry symbol. Stash `api`, return your vtable. */
typedef const MoteGameVtbl *(*MoteGameRegisterFn)(const MoteApi *api);

/* Games declare these (see MOTE_GAME_MODULE below). */
#ifdef __cplusplus
extern "C" {
#endif
const MoteGameVtbl *mote_game_register(const MoteApi *api);
#ifdef __cplusplus
}
#endif

/* Convenience for a game's translation unit: stashes the api pointer in a
 * file-scope `mote` and exports the abi version symbol the loader checks. */
#define MOTE_GAME_MODULE()                                                   \
    const uint32_t mote_game_abi_version = MOTE_ABI_VERSION;                   \
    static const MoteApi *mote;                                                \
    /* defined by the game: returns its vtable; we set `mote` first */       \
    static const MoteGameVtbl *mote_game_vtbl(void);                           \
    const MoteGameVtbl *mote_game_register(const MoteApi *api) {                 \
        mote = api;                                                          \
        return mote_game_vtbl();                                             \
    }

#endif /* MOTE_API_H */
