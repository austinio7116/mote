/*
 * ThumbyCue — shared framebuffer constants + colour helpers.
 *
 * The vendored craft_font.c (ThumbyCraft lineage) expects CRAFT_FB_W/H and
 * CRAFT_HUD_SCALE; this header provides them so the font compiles unmodified.
 */
#ifndef CUE_TYPES_H
#define CUE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#define CUE_FB_W 128
#define CUE_FB_H 128

/* 3D supersample factor (host icon/debug only; 1 = device). Game logic, UI
 * and projection stay in 128×128 "logical" space; the raster layer multiplies
 * up into an R3D_FB_W × R3D_FB_H physical buffer. */
#ifndef R3D_SS
#define R3D_SS 1
#endif
#define R3D_FB_W (CUE_FB_W * R3D_SS)
#define R3D_FB_H (CUE_FB_H * R3D_SS)

/* Aliases the vendored r3d_raster.c and craft_font.c expect. */
#define ELITE_FB_W CUE_FB_W
#define ELITE_FB_H CUE_FB_H
#define CRAFT_FB_W CUE_FB_W
#define CRAFT_FB_H CUE_FB_H
#define CRAFT_HUD_SCALE 1

#define CUE_AUDIO_RATE 22050

/* Compile-time constant RGB565 (static initialisers / const tables). */
#define RGB565C(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#endif
