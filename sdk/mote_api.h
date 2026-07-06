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
#include "mote_sphere.h"   /* MoteSphereTex — textured/oriented impostor */
#include "mote_2d.h"       /* MoteImage/Tileset/Tilemap/Sprite — header-only */
#include "mote_phys.h"     /* MoteWorld/MoteBody — header-only */
#include "mote_phys2d.h"   /* MoteWorld2D/MoteBody2D — 2D top-down rigid bodies */
#include "mote_splat.h"    /* MoteSplat — Gaussian-splat renderer */

#define MOTE_ABI_VERSION 46u  /* v46: per-game VERSION string in the module header
                               * (MoteModuleHeader.version_vaddr, set by MOTE_GAME_VERSION).
                               * The launcher/gallery read the installed version straight
                               * from flash (like the icon) to diff against the online
                               * gallery. No jump-table change — a header append only, so
                               * v46 firmware still runs v45 games (they just report no
                               * embedded version). Read version_vaddr only when abi>=46.
                               * v45: engine-owned link health — the OS keepalives the
                               * pipe itself and strips them on receipt, so net_health()
                               * reports transport truth (OK/STALLED/LOST) and the OS
                               * draws the stall banner. Games swap their own hard
                               * silence timeouts for net_health()==MOTE_NET_LOST.
                               * Appended one jump-table entry.
                               * v44: standard multiplayer lobby — net_lobby() runs an
                               * engine-drawn transport pick + connect + nonce handshake and
                               * hands back a CONNECTED link (link_send/recv after). MoteNetCfg /
                               * MOTE_NET_*. Appended one jump-table entry. */
#define MOTE_ABI_V43_NOTE  /* v43: 2-player link — link_start/stop/status/is_host/send/recv.
                               * Device: USB CDC dual-role (two units, one C-to-C cable);
                               * host emulator: a local socket (MOTE_LINK_SOCK). */
#define MOTE_ABI_V42_NOTE  /* v42: 2D rigid-body solver — phys2d_step + MoteBody2D/MoteWorld2D
                               * (engine/physics/mote_phys2d). Appended one jump-table entry. */
#define MOTE_ABI_V41_NOTE  /* v41: MoteImage gains an optional 4-/8-bit palette-indexed format
                               * (texture flash 1/4 or 1/2 of RGB565, decoded by a palette lookup at
                               * sample time). No jump-table change; the struct grew (old
                               * {px,w,h,key[,opaque]} headers zero the new fields = RGB565, unchanged).
                               * v40: MoteSound carries a sample RATE + BIT-DEPTH, so a clip can
                               * be stored at its native (lower) quality — 8-bit and/or sub-22050 Hz
                               * — and the mixer resamples/expands it at playback. Cuts flash ~4x for
                               * 8-bit/11025 SFX. No jump-table change; the struct grew (old
                               * {pcm,count} headers zero the new fields = 16-bit/22050, unchanged). */

/* link_status() values (ABI v43). */
#define MOTE_LINK_OFF        0   /* link not started */
#define MOTE_LINK_SEARCHING  1   /* looking for a peer */
#define MOTE_LINK_CONNECTED  2   /* byte pipe to the peer is up */

/* net_lobby() (ABI v44). transports is a bitmask of the options the lobby offers. */
#define MOTE_NET_USB        0x01
#define MOTE_NET_LAN        0x02
#define MOTE_NET_INTERNET   0x04
#define MOTE_NET_ALL        0x07
#define MOTE_NET_CANCELLED  0    /* net_lobby: player backed out */
#define MOTE_NET_CONNECTED  1    /* net_lobby: paired — use link_send/recv now */

/* net_health() (ABI v45) — engine-measured transport truth for a lobby session.
 * The OS exchanges its own keepalives whenever the game goes quiet (and strips
 * inbound ones before link_recv hands bytes over), so silence == a REAL stall:
 *   OK       bytes are flowing (or no lobby session is active)
 *   STALLED  nothing received for >2.5 s — the OS is already showing a banner;
 *            keep simulating, DON'T end the match (internet blips recover)
 *   LOST     nothing for >20 s — treat as the old 'link lost' */
#define MOTE_NET_OK         0
#define MOTE_NET_STALLED    1
#define MOTE_NET_LOST       2

/* What a game passes to net_lobby(). game_name + proto_version form the game id
 * that gates rooms (only same-game, same-proto peers ever pair). transports = which
 * of USB/LAN/INTERNET the lobby offers (0 = all). */
typedef struct MoteNetCfg {
    const char *game_name;
    uint16_t    proto_version;
    uint8_t     transports;
} MoteNetCfg;

struct MoteAutotile;   /* full definition in mote_tile.h; the ABI only passes a pointer */
/* MOTE_DRAW_* per-object draw flags for scene_add_object_ex() live in mote_object.h. */

/* A one-shot PCM sound effect, mono, produced by baking a .wav (Studio Audio tab
 * ▸ Save, or `mote bake`) into a header. Stored at its NATIVE quality: `bits` is 8
 * or 16 (0 = 16), `rate` is the source sample rate in Hz (0 = the 22050 mixer
 * rate). The mixer resamples to 22050 and expands 8-bit on playback, so a low-fi
 * clip (e.g. 8-bit/11025) costs ~1/4 the flash of a 16-bit/22050 one.
 *   16-bit: `pcm` points at int16 samples · 8-bit: at signed int8 samples.
 * Legacy {pcm,count} initialisers leave rate=bits=0 → 16-bit/22050 (unchanged). */
typedef struct { const void *pcm; int count; uint16_t rate; uint8_t bits; uint8_t _reserved; } MoteSound;

/* An SFXR-style sound RECIPE (~88 bytes) — the editable source behind a SFX. The
 * Studio Audio tab authors these; baked to a `static const MoteSfx` header it is
 * ~1000x smaller than the equivalent PCM and the engine synthesises it on load
 * (mote_sfx_bake in mote_build.h) instead of shipping a .wav. Fields/units match
 * the Studio's synth exactly. */
typedef struct MoteSfx {
    int   wave;                 /* 0 square · 1 saw · 2 sine · 3 noise */
    float base_freq, freq_limit, freq_ramp, freq_dramp, duty, duty_ramp;
    float vib_strength, vib_speed, env_attack, env_sustain, env_punch, env_decay;
    float lpf_freq, lpf_ramp, lpf_resonance, hpf_freq, hpf_ramp, pha_offset, pha_ramp, arp_speed, arp_mod;
} MoteSfx;

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
    /* --- ABI v24: depth-tested 3D FX primitive pools (0 = none). These draw in
     * the 3D pass, depth-TESTED against meshes/spheres but not depth-writing. */
    uint16_t max_points;      /* point/particle draws per frame — 16 B/entry */
    uint16_t max_lines;       /* line/beam draws per frame      — 24 B/entry */
    uint16_t max_discs;       /* disc/fireball draws per frame   — 16 B/entry */
    /* --- ABI v25: textured/oriented sphere impostors (balls, planets). */
    uint16_t max_tex_spheres; /* scene_add_sphere_tex draws per frame — ~64 B/entry */
    /* --- ABI v28: soft ground-shadow decals. */
    uint16_t max_shadows;     /* scene_add_shadow draws per frame — ~32 B/entry */
    /* --- ABI v31: camera-facing ring outlines (ghost balls, reticles). */
    uint16_t max_rings;       /* scene_add_ring draws per frame — ~16 B/entry */
    /* --- ABI v33: camera-facing textured quads (3D sprites). */
    uint16_t max_billboards;  /* scene_add_billboard draws per frame — ~32 B/entry */
    /* --- ABI v35: textured (UV-mapped) mesh triangles per frame. Size to the
     * total textured faces visible at once (a textured mesh's nfaces). 0 = none. */
    uint16_t max_tex_tris;    /* textured-triangle draws per frame — ~56 B/entry */
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

    /* --- ABI v12: PCM sample playback. Fire a one-shot 22050 Hz mono sample
     * (e.g. a sound effect authored in the Studio's SFX editor and baked to a
     * header — see `mote bake` / Studio Audio ▸ Save). `gain` 0..1+. Up to 4
     * samples mix at once (oldest is stolen); they sum on top of the synth. */
    void (*audio_play)(const MoteSound *snd, float gain);

    /* --- ABI v13: camera-aware 3D scene. Like scene_begin(), but you also pass
     * the camera's world position — then add objects with ABSOLUTE world
     * positions instead of pre-subtracting cam_pos (the legacy scene_begin()
     * convention). Pass the SAME cam_pos to scene_set_splats() for a consistent
     * scene. scene_begin() still works (it's just scene_camera with pos 0). */
    void (*scene_camera)(const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg);

    /* --- ABI v14: render-time autotiling. Hand the engine a LOGICAL terrain map
     * (one byte per cell: 0 = empty, 1..n index `tiles`) plus a ruleset per
     * terrain; the 2D raster picks each cell's atlas tile from its 8 neighbours.
     * No resolved buffer — re-call it for free when the map changes (procgen,
     * destructible). Build rulesets with mote_autotile_template() (mote_tile.h)
     * or bake them in Mote Studio's Tiles tab. */
    void (*scene2d_set_autotiles)(const uint8_t *terrain, int cols, int rows,
                                  const struct MoteAutotile *const *tiles, int n);

    /* --- ABI v15: layered autotiling. `map` is ONE byte per cell where each bit
     * is a layer (bit L occupied -> tiles[L] drawn there); up to 8 independent,
     * overlapping layers, each autotiled against its own bit and drawn in order
     * (0 = bottom). The map is typically a `const` baked level, so it lives in
     * flash and costs no SRAM. Replaces the need for one terrain map per layer. */
    void (*scene2d_set_autotile_layers)(const uint8_t *map, int cols, int rows,
                                        const struct MoteAutotile *const *tiles, int n);

    /* --- ABI v19: synthesise a MoteSfx recipe to 22050 Hz mono PCM. Returns the
     * sample count; writes into `out` only when out != NULL and bounded by `max`.
     * Call it twice (measure, then render into an arena buffer) — that's what the
     * mote_sfx_bake() helper does — so a game can ship tiny recipes instead of WAVs
     * and have the engine generate the audio at load. */
    int (*audio_render_sfx)(const MoteSfx *recipe, int16_t *out, int max);

    /* --- ABI v21: cap the frame rate. fps <= 0 runs uncapped (the default —
     * the device free-runs, the host emulator runs as fast as the machine
     * allows). A positive value paces the main loop to that many frames per
     * second on BOTH the device and the host emulator, so a game can lock 30/60
     * fps for steady timing. Call it once from update() on the first frame, or
     * whenever you want to change the cap. */
    void (*set_fps_limit)(int fps);

    /* --- ABI v23: rumble + persistent per-slot save.
     *
     * rumble(intensity, ms): buzz the motor at intensity 0..1 for ms milliseconds
     * (it eases out, then stops itself). intensity<=0 or ms<=0 stops immediately.
     * No-op on the host emulator and Studio. */
    void (*rumble)(float intensity, int ms);

    /* Persistent storage: a handful of fixed-size save SLOTS (see save_slots()).
     * On device each slot is a flash sector that survives power-off and OS reflash;
     * on host/Studio it's a file. save() writes `len` bytes (len==0 clears the slot)
     * and returns len on success, <=0 on failure. load() copies up to max_len bytes
     * into `data` and returns the saved length (0 if the slot is empty) — so you can
     * call load(slot, NULL, 0) to test for a save. Keep your own magic/version inside
     * the blob. */
    int  (*save)(int slot, const void *data, int len);
    int  (*load)(int slot, void *data, int max_len);
    int  (*save_slots)(void);     /* number of slots available */

    /* --- ABI v24: depth-tested 3D scene primitives (particles, beams, glows).
     * Build them in update() alongside scene_add_object/sphere; the OS rasters
     * them in the same dual-core banded 3D pass. All take CAMERA-RELATIVE world
     * positions (like scene_add_sphere) — or absolute, if you used scene_camera.
     * Depth-TESTED against meshes/spheres but NOT depth-writing, so they layer
     * like additive FX. Sized by MoteConfig.max_points/lines/discs (0 = unused).
     *  - scene_add_point: a `size`x`size` screen dot (1 = single pixel).
     *  - scene_add_line:  a 3D segment a->b (lasers/beams); near-plane clipped.
     *  - scene_add_disc:  a screen-facing filled circle of world `radius`
     *                     (fireballs/explosion glows), size scaling with depth.
     * Each returns 1 if added, 0 if culled (behind near) or the pool is full. */
    int  (*scene_add_point)(Vec3 cam_rel_pos, uint16_t color, int size);
    int  (*scene_add_line)(Vec3 a_cam_rel, Vec3 b_cam_rel, uint16_t color);
    int  (*scene_add_disc)(Vec3 cam_rel_pos, float radius, uint16_t color);

    /* Draw a mesh with per-object flags (MOTE_DRAW_* above). flags==0 is exactly
     * scene_add_object(). MOTE_DRAW_NO_DEPTH_WRITE depth-tests but doesn't write,
     * for coplanar overlays (e.g. pocket lips) that later geometry must cover. */
    int  (*scene_add_object_ex)(const MoteObject *obj, uint32_t flags);

    /* --- ABI v25: a textured / oriented sphere impostor. The engine rasterises
     * the disc, reconstructs the per-pixel sphere normal, writes depth, and
     * rotates the normal into the sphere's LOCAL frame by `orient` (rows
     * right/up/forward; NULL = identity). The surface colour + shading come from
     * `tex` (a const MoteSphereTex): an equirectangular texture or a per-pixel
     * callback, with built-in or custom shading. Covers spinning textured balls,
     * lit planets, and procedural surfaces. cam_rel_pos like scene_add_sphere. */
    int  (*scene_add_sphere_tex)(Vec3 cam_rel_pos, float radius,
                                 const Mat3 *orient, const MoteSphereTex *tex);

    /* --- ABI v26: a per-band background pass. `fn` is called for each core's
     * logical row band [y0,y1) BEFORE any 3D geometry (depth already cleared),
     * so you can paint a gradient / starfield / nebula that the scene draws over
     * — the generic alternative to owning the whole frame with render_band. It
     * runs on BOTH cores (disjoint bands), like render_band. NULL restores the
     * solid scene_set_background colour. Set it once from init()/update(). */
    void (*set_background_cb)(void (*fn)(uint16_t *fb, int y0, int y1));

    /* --- ABI v27: an immediate-mode world-space triangle with a caller-supplied
     * flat colour (the engine projects, near-clips, depth-tests and fills it but
     * does NOT light it — you pass the final shaded colour). For dynamic or
     * procedural geometry that isn't a baked int8 Mesh: generated tables, voxel
     * faces, debug shapes. cam_rel_pos convention like scene_add_object. `flags`
     * are MOTE_DRAW_* (e.g. NO_DEPTH_WRITE). DOUBLE-SIDED — drawn regardless of
     * winding (unlike scene_add_object, which backface-culls). Returns tris
     * emitted after clip. */
    int  (*scene_add_tri)(Vec3 a, Vec3 b, Vec3 c, uint16_t color, uint32_t flags);

    /* --- ABI v28: a soft ground-shadow decal + a runtime near plane.
     *
     * scene_add_shadow: a darkening ellipse on the ground plane at `ground_pos`
     * (radius in world metres), foreshortened with the view and faded from the
     * centre out (strength 0..1; 0 -> default). Drawn after the scene tris but
     * before impostor balls, so an object paints over its own shadow. Sized by
     * MoteConfig.max_shadows.
     *
     * scene_set_near: move the near clip plane (metres). The 0.5 m default suits
     * a space sim; small scenes (a snooker table) need ~0.05 m or the close
     * geometry clips. Also rescales the depth buffer. Set once in init(). */
    int  (*scene_add_shadow)(Vec3 ground_pos, float radius, float strength);
    void (*scene_set_near)(float near_m);

    /* --- ABI v29: the engine-owned MASTER VOLUME (0..1), applied to all audio
     * (synth notes + PCM samples) in the mixer. This is the SAME knob the engine
     * menu's VOLUME row drives, so a game's own volume setting and the system
     * menu stay in sync — a game should route its volume option here instead of
     * scaling each sound itself. Persists across the session. */
    void  (*audio_set_master)(float v);
    float (*audio_get_master)(void);

    /* --- ABI v30: immediate-mode 2D framebuffer drawing (screen space, RGB565,
     * no depth) — for HUDs/overlays (call from overlay() with yc0=0,yc1=128) and
     * background passes (call from a set_background_cb with the band [y0,y1)).
     * Rounds out the 2D kit alongside blit()/text(). draw_rect: fill!=0 solid,
     * else outline. */
    void (*draw_pixel)(uint16_t *fb, int x, int y, uint16_t color);
    void (*draw_line)(uint16_t *fb, int x0, int y0, int x1, int y1,
                      uint16_t color, int yc0, int yc1);
    void (*draw_rect)(uint16_t *fb, int x, int y, int w, int h,
                      uint16_t color, int fill, int yc0, int yc1);

    /* --- ABI v31: a camera-facing CIRCLE OUTLINE in the 3D scene + a 2D circle.
     *
     * scene_add_ring: a billboard ring of world `radius` at a camera-relative
     * world position — depth-tested, drawn in the 3D pass. It always faces the
     * camera, so it reads as an object's circular silhouette (ghost ball at the
     * contact point, target reticle, selection ring). Sized by config.max_rings.
     *
     * draw_circle: a 2D framebuffer circle (fill!=0 solid, else outline) — the
     * circle companion to draw_line/rect for HUDs/overlays. */
    int  (*scene_add_ring)(Vec3 cam_rel_pos, float radius, uint16_t color);
    void (*draw_circle)(uint16_t *fb, int cx, int cy, int r,
                        uint16_t color, int fill, int yc0, int yc1);

    /* --- ABI v32: an ORIENTED elliptical ground shadow. `semi_a` and `semi_b`
     * are the footprint's two semi-axis vectors in WORLD space on the ground
     * plane — so the shadow matches the object's shape and heading (a tank gets
     * a long oval along its hull; a thin object a thin oval) instead of a fixed
     * circle. scene_add_shadow(radius) is just this with axial radii. */
    int  (*scene_add_shadow_ex)(Vec3 ground_pos, Vec3 semi_a, Vec3 semi_b, float strength);

    /* --- ABI v33: a camera-facing TEXTURED QUAD in the 3D scene (a "3D sprite").
     *
     * scene_add_billboard: draws `img` (or the fx/fy/fw/fh sub-rect of it, for
     * sprite sheets — pass 0 sizes for the whole image) as an upright quad at a
     * camera-relative world position. `world_h` is its full height in world
     * units, so it shrinks with distance and keeps the image's aspect ratio. It
     * is depth-tested against the scene; opaque (MOTE_BLEND_NONE) sprites also
     * write depth, while blended ones (MOTE_BLEND_ALPHA / _ADD) layer on top.
     * Colour-keyed by the image's transparent key. Sized by config.max_billboards.
     * Use for trees, pickups, enemies, smoke, muzzle flashes, explosions. */
    int  (*scene_add_billboard)(Vec3 cam_rel_pos, const MoteImage *img,
                                int fx, int fy, int fw, int fh,
                                float world_h, uint8_t blend);

    /* --- ABI v34: free rotate + scale 2D blit (HUDs, twin-stick sprites, FX).
     *
     * blit_ex: draws `img` (or its fx/fy/fw/fh sub-rect) centred at (cx,cy) in
     * framebuffer pixels, rotated `angle` radians and uniformly scaled (1.0 =
     * original size). Colour-keyed; blend = MOTE_BLEND_* for alpha/additive.
     * Unlike `blit` (axis-aligned + 90° steps only), this is any angle/scale.
     * Immediate-mode: call it from a render/background callback with the band
     * [y0,y1), or from overlay() with 0..128. */
    void (*blit_ex)(uint16_t *fb, const MoteImage *img,
                    float cx, float cy, int fx, int fy, int fw, int fh,
                    float angle, float scale, uint8_t blend, int yc0, int yc1);

    /* --- ABI v36: register a PCM source the audio mixer pulls each block.
     * `fill(out, n)` writes up to n mono int16 samples at 22050 Hz and returns
     * the count written; it is mixed (added, master-scaled, clamped) on top of
     * the synth voices. Pass NULL to unregister. For games that run their own
     * software synth (full music + SFX) instead of the note/play/sfx API —
     * e.g. the ThumbyCraft port. The callback runs on the audio path, so keep
     * it non-blocking and read-only w.r.t. heavy game state. */
    void (*audio_set_stream)(int (*fill)(int16_t *out, int n));

    /* --- ABI v37: play a MoteSfx recipe by STREAMING it — the mixer synthesises
     * the recipe block-by-block on the fly (a dedicated voice pool), so a sound
     * costs only its ~88-byte recipe in flash and ~no RAM, instead of baking the
     * whole clip to PCM. Ideal for a whole game's hand-tuned SFX set (a weapon
     * rack, UI blips) where the baked-PCM clips would bloat flash. `gain` is the
     * per-shot level; up to 4 recipe voices play at once (oldest is stolen). */
    void (*audio_play_sfx)(const MoteSfx *recipe, float gain);

    /* --- ABI v38: key-value blob storage. Unlike save()/load() (a fixed set of
     * numbered slots), this is arbitrary named blobs — for games that persist many
     * pieces (e.g. a voxel world's per-chunk edits). Backed by real files under the
     * game's save folder, so blobs are any size and survive reboots.
     *   kv_save(key, data, len): write/overwrite a blob; len==0 deletes it. Returns
     *                            bytes written (<=0 on failure).
     *   kv_load(key, data, max):  read up to max bytes; returns the blob's size
     *                            (0 if absent). Pass data=NULL to query size.
     *   kv_list(prefix, cb, arg): call cb(key, arg) for every key starting with
     *                            `prefix` (cb must not call kv_* re-entrantly).
     * Keys are short ASCII (no '/'); treat as case-insensitive on FAT. */
    int  (*kv_save)(const char *key, const void *data, int len);
    int  (*kv_load)(const char *key, void *data, int max);
    void (*kv_list)(const char *prefix, void (*cb)(const char *key, void *arg), void *arg);

    /* --- ABI v39: anti-aliased proportional text. Draws `s` with `font` (baked
     * from a TTF, or hand-drawn in the Studio's Font tab) at top-left (x,y),
     * alpha-blending each glyph's coverage over the framebuffer. '\n' wraps to x
     * and down font->line_h. Returns the advanced x. The built-in mote->text()
     * 3x5 font stays available for tiny fixed HUD text. */
    int (*text_font)(uint16_t *fb, const MoteFont *font, const char *s, int x, int y, uint16_t color);

    /* --- ABI v42: 2D rigid-body solver (top-down). The game owns the body array;
     * the engine steps it (impulse resolution + friction + one rotational DOF). Its
     * contact pool is arena-sized by config.max_bodies/max_contacts (like the 3D
     * solver). See mote_phys2d.h. Returns contacts resolved on the last substep. */
    uint32_t (*phys2d_step)(MoteWorld2D *w, MoteBody2D *b, int n, float dt);

    /* --- ABI v43: 2-player LINK — a raw byte pipe to a second unit.
     *
     * On device the transport is USB: both units run the same discovery, flipping
     * randomly between USB device and USB host roles until one enumerates the
     * other over a single USB-C cable (the TinyCircuits engine_link scheme). On
     * the host emulator it is a local socket (MOTE_LINK_SOCK, default
     * /tmp/mote_link.sock) so two emulator instances can play each other.
     *
     *   link_start():  begin discovery. The OS pumps the transport every frame.
     *   link_stop():   drop the link and return USB to the system (CLI/logs).
     *                  The OS also stops it automatically when the game exits.
     *   link_status(): MOTE_LINK_OFF / SEARCHING / CONNECTED. A lost cable drops
     *                  back to SEARCHING — treat that as a disconnect.
     *   link_is_host():valid while connected: 1 on exactly one side (the USB
     *                  host / socket listener). Use it to break symmetry — who
     *                  plays white, who is the server.
     *   link_send():   queue up to `len` bytes; returns bytes accepted (may be
     *                  short if the FIFO is full; 0 when not connected).
     *   link_recv():   read up to `max` received bytes; returns the count (0 if
     *                  none). Non-blocking. The engine buffers ~512 B — poll it
     *                  every frame.
     *
     * The pipe is unframed bytes: define your own tiny message format and start
     * with a hello/magic exchange so a PC or a stray CDC peer can't be mistaken
     * for the other player. */
    void (*link_start)(void);
    void (*link_stop)(void);
    int  (*link_status)(void);
    int  (*link_is_host)(void);
    int  (*link_send)(const void *data, int len);
    int  (*link_recv)(void *buf, int max);

    /* --- ABI v44: standard multiplayer lobby. Runs a BLOCKING engine-drawn lobby
     * (pick USB/LAN/INTERNET, host/browse/join, then connect) and returns with a
     * live link — use link_send/recv afterwards exactly as before. The engine owns
     * the nonce handshake and writes the resolved authority to *out_is_host (1 on
     * exactly one side; use it for who-seeds / who-goes-first). Rooms are gated by
     * game (cfg->game_name + proto_version), so only the same game can pair — even
     * over a cable. Returns MOTE_NET_CONNECTED or MOTE_NET_CANCELLED. */
    int (*net_lobby)(const MoteNetCfg *cfg, int *out_is_host);
    /* ABI v45: engine-owned link health (see MOTE_NET_OK/STALLED/LOST). */
    int (*net_health)(void);
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

/* Game metadata. NAME is shown in the launcher and used for the .mote filename;
 * AUTHOR credits you. Put it at file scope in game.c (anywhere after this header):
 *     MOTE_GAME_META("My Game", "me");
 * This is the source of truth for a game's name. Tools that need the name without
 * compiling text-scan game.c for MOTE_GAME_META; if absent they fall back to a
 * legacy game.toml [game] name, then to the folder name. */
#define MOTE_GAME_META(NAME, AUTHOR)                                          \
    const char *const mote_game_name   = NAME;                                \
    const char *const mote_game_author = AUTHOR

/* Optional per-game VERSION (semver string, e.g. "1.2.0"). Put it at file scope
 * in game.c alongside MOTE_GAME_META:
 *     MOTE_GAME_VERSION("1.2.0");
 * It is emitted as a plain char[] in .rodata so the launcher/gallery can read the
 * INSTALLED version straight from the module's flash image (via
 * MoteModuleHeader.version_vaddr) without running the game — that's how "update
 * available" is decided against the online gallery. Tools also text-scan this from
 * game.c for the gallery manifest. A game that omits it reports version "0"
 * (unversioned) and always shows an update once the gallery copy carries a real
 * version. `used` keeps it even though nothing references it in C. */
#define MOTE_GAME_VERSION(VER)                                                 \
    const char mote_game_version[] __attribute__((used)) = VER

#endif /* MOTE_API_H */
