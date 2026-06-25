/*
 * Mote — textured / oriented sphere impostor description (header-only).
 *
 * The engine owns the impostor GEOMETRY: it rasterises the screen disc,
 * reconstructs the per-pixel sphere normal, writes depth, and rotates the
 * normal into the sphere's LOCAL frame by the orientation basis you pass to
 * scene_add_sphere_tex(). A MoteSphereTex says where the surface COLOUR comes
 * from and how it is shaded — so one primitive covers spinning textured balls,
 * lit textured planets, and fully procedural surfaces:
 *
 *   surface albedo (pick one):
 *     - texels[]  : an equirectangular RGB565 texture (tex_w x tex_h), or
 *     - indices[] + palette[] : palette-indexed equirect (compact; planets), or
 *     - albedo()  : a per-pixel callback returning RGB565 from the local normal
 *                   (procedural patterns — stripes, numbers, measles).
 *   shading (shade_mode):
 *     - FLAT/LIT/SMOOTH/TOON/GLOSS : built-in, fast, sun-lit looks, or
 *     - CUSTOM : your shade() returns the FINAL pixel colour from the local +
 *                world normal and the engine-computed diffuse/specular/facing
 *                terms (full control — e.g. multi-lamp speculars).
 *
 * The struct is a `const` the game builds once (in flash); the engine reads it
 * per impostor. Callbacks run per visible pixel, so keep them cheap.
 */
#ifndef MOTE_SPHERE_H
#define MOTE_SPHERE_H

#include "mote_vec.h"
#include <stdint.h>

typedef enum {
    MOTE_SHADE_FLAT = 0,   /* albedo unlit (emissive — suns, glows) */
    MOTE_SHADE_LIT,        /* sun diffuse + limb darkening (planets) */
    MOTE_SHADE_SMOOTH,     /* soft diffuse + facing falloff + subtle spec */
    MOTE_SHADE_TOON,       /* banded diffuse + underside tint + crisp hotspot */
    MOTE_SHADE_GLOSS,      /* smooth diffuse + tint + sharp gloss hotspot */
    MOTE_SHADE_CUSTOM,     /* shade() returns the final colour */
} MoteShadeMode;

typedef struct MoteSphereTex {
    /* --- albedo source (first non-NULL wins) --- */
    const uint16_t *texels;     /* equirect RGB565, tex_w*tex_h row-major */
    const uint8_t  *indices;    /* equirect palette indices, tex_w*tex_h */
    const uint16_t *palette;    /* palette for `indices` */
    uint16_t tex_w, tex_h;      /* equirect texture dimensions */
    uint16_t (*albedo)(Vec3 n_local, void *ud);  /* procedural albedo (no texture) */

    /* --- shading --- */
    uint8_t  shade_mode;        /* MoteShadeMode */
    uint16_t tint;              /* underside/ambient tint for TOON/GLOSS (RGB565) */
    /* CUSTOM: returns the final pixel colour. n_local/n_world are unit normals,
     * diff = max(0, n_world.sun), spec = max(0, n_world.halfway), nz = facing
     * (1 at the disc centre, 0 at the limb). */
    uint16_t (*shade)(Vec3 n_local, Vec3 n_world, float diff, float spec, float nz, void *ud);

    void *ud;                   /* opaque, passed to albedo()/shade() */
} MoteSphereTex;

#endif /* MOTE_SPHERE_H */
