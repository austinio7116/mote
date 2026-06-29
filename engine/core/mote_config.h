/*
 * Mote — compile-time configuration and framebuffer constants.
 *
 * The engine targets the Thumby Color: a 128x128 RGB565 display. Game logic,
 * UI and projection all live in 128x128 "logical" space; the raster layer can
 * supersample into a physical MOTE_FB_PW x MOTE_FB_PH buffer (MOTE_SS > 1) for host
 * screenshot tooling. On device MOTE_SS == 1, so logical and physical coincide.
 *
 * Render backends are selected at compile time. The resident OS engine builds
 * with all of them; a future compile-to-native game can drop unused backends
 * to reclaim flash/SRAM.
 */
#ifndef MOTE_CONFIG_H
#define MOTE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* --- Display ------------------------------------------------------------ */
#define MOTE_FB_W 128
#define MOTE_FB_H 128

#ifndef MOTE_SS
#define MOTE_SS 1                 /* 3D supersample factor (host tooling: >1) */
#endif
#define MOTE_FB_PW (MOTE_FB_W * MOTE_SS)
#define MOTE_FB_PH (MOTE_FB_H * MOTE_SS)

/* --- 3D pipeline -------------------------------------------------------- */
#define MOTE_NEAR     0.5f                    /* near plane, meters */
#define MOTE_DEPTH_K  (65535.0f * MOTE_NEAR)    /* depth d = K/z, 65535 at z=NEAR */
#define MOTE_MAX_VERTS 320                    /* largest single mesh */

/* --- Render backend feature flags --------------------------------------- */
#ifndef MOTE_RENDER_RASTER
#define MOTE_RENDER_RASTER 1      /* triangle rasterizer + 3D pipeline (Elite) */
#endif
#ifndef MOTE_RENDER_RAYCAST
#define MOTE_RENDER_RAYCAST 0     /* DDA voxel raycaster — DESCOPED (2026-06-29):
                                   * stays a game-side render_band renderer, not an
                                   * engine path. See docs/PLAN.md "Renderer scope". */
#endif
#ifndef MOTE_RENDER_SPRITE
#define MOTE_RENDER_SPRITE 0      /* 2D blit / framebuffer ops — Phase 3 */
#endif
#ifndef MOTE_PHYSICS
#define MOTE_PHYSICS 0            /* rigid-body solver (Cue) — Phase 3 */
#endif

/* --- Audio -------------------------------------------------------------- */
#define MOTE_AUDIO_RATE 22050

/* --- IRAM hot-path placement -------------------------------------------- */
/* On device, mark a function to live in SRAM (avoids XIP flash latency in the
 * render/physics inner loops). Resolves to nothing on host. */
#if defined(MOTE_DEVICE) && !defined(MOTE_HOST)
#define MOTE_HOT __attribute__((section(".time_critical.mote")))
#else
#define MOTE_HOT
#endif

/* --- RGB565 helpers ----------------------------------------------------- */
#define MOTE_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static inline uint16_t mote_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return MOTE_RGB565(r, g, b);
}

#endif /* MOTE_CONFIG_H */
