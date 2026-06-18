/*
 * ThumbyEngine — compile-time configuration and framebuffer constants.
 *
 * The engine targets the Thumby Color: a 128x128 RGB565 display. Game logic,
 * UI and projection all live in 128x128 "logical" space; the raster layer can
 * supersample into a physical TE_FB_PW x TE_FB_PH buffer (TE_SS > 1) for host
 * screenshot tooling. On device TE_SS == 1, so logical and physical coincide.
 *
 * Render backends are selected at compile time. The resident OS engine builds
 * with all of them; a future compile-to-native game can drop unused backends
 * to reclaim flash/SRAM.
 */
#ifndef TE_CONFIG_H
#define TE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* --- Display ------------------------------------------------------------ */
#define TE_FB_W 128
#define TE_FB_H 128

#ifndef TE_SS
#define TE_SS 1                 /* 3D supersample factor (host tooling: >1) */
#endif
#define TE_FB_PW (TE_FB_W * TE_SS)
#define TE_FB_PH (TE_FB_H * TE_SS)

/* --- 3D pipeline -------------------------------------------------------- */
#define TE_NEAR     0.5f                    /* near plane, meters */
#define TE_DEPTH_K  (65535.0f * TE_NEAR)    /* depth d = K/z, 65535 at z=NEAR */
#define TE_MAX_VERTS 320                    /* largest single mesh */

/* --- Render backend feature flags --------------------------------------- */
#ifndef TE_RENDER_RASTER
#define TE_RENDER_RASTER 1      /* triangle rasterizer + 3D pipeline (Elite) */
#endif
#ifndef TE_RENDER_RAYCAST
#define TE_RENDER_RAYCAST 0     /* DDA voxel raycaster (Craft) — Phase 3 */
#endif
#ifndef TE_RENDER_SPRITE
#define TE_RENDER_SPRITE 0      /* 2D blit / framebuffer ops — Phase 3 */
#endif
#ifndef TE_PHYSICS
#define TE_PHYSICS 0            /* rigid-body solver (Cue) — Phase 3 */
#endif

/* --- Audio -------------------------------------------------------------- */
#define TE_AUDIO_RATE 22050

/* --- IRAM hot-path placement -------------------------------------------- */
/* On device, mark a function to live in SRAM (avoids XIP flash latency in the
 * render/physics inner loops). Resolves to nothing on host. */
#if defined(TE_DEVICE) && !defined(TE_HOST)
#define TE_HOT __attribute__((section(".time_critical.te")))
#else
#define TE_HOT
#endif

/* --- RGB565 helpers ----------------------------------------------------- */
#define TE_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static inline uint16_t te_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return TE_RGB565(r, g, b);
}

#endif /* TE_CONFIG_H */
