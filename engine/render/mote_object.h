/*
 * Mote — renderable 3D object (header-only).
 *
 * A mesh placed in the camera-relative world with an orientation basis.
 * Split out from mote_pipe.h so game modules can use the type via the ABI
 * without seeing the pipe's function declarations.
 */
#ifndef MOTE_OBJECT_H
#define MOTE_OBJECT_H

#include "mote_vec.h"
#include "mote_mesh.h"

typedef struct {
    Vec3 pos;          /* camera-relative world position */
    Mat3 basis;        /* object orientation (rows: right/up/forward) */
    const Mesh *mesh;
    uint16_t color;    /* optional per-draw colour override (RGB565); 0 = use the mesh's own colour(s) */
} MoteObject;

/* Per-object draw flags for scene_add_object_ex() (ABI v24). */
#define MOTE_DRAW_NO_DEPTH_WRITE 1u   /* depth-test but don't write (coplanar overlays) */

/* Blend modes for sprite / billboard / textured draws (ABI v33+). Opaque draws
 * are colour-keyed (the image's transparent key); blended draws mix or add the
 * source onto the destination pixel. */
#define MOTE_BLEND_NONE  0u   /* opaque replace (colour-keyed) */
#define MOTE_BLEND_ALPHA 1u   /* ~50% alpha — mix half source, half destination */
#define MOTE_BLEND_ADD   2u   /* additive (saturating) — glows, lasers, sparks */

/* Packing of a blend mode into a scene_add_object_ex() flags word (bits 4-5),
 * so a mesh can be drawn translucent: scene_add_object_ex(obj,
 * MOTE_DRAW_BLEND(MOTE_BLEND_ALPHA)). Coexists with MOTE_DRAW_NO_DEPTH_WRITE. */
#define MOTE_BLEND_SHIFT      4u
#define MOTE_BLEND_MASK       (3u << MOTE_BLEND_SHIFT)
#define MOTE_DRAW_BLEND(m)    (((uint32_t)(m) & 3u) << MOTE_BLEND_SHIFT)
#define MOTE_FLAGS_BLEND(f)   (((f) >> MOTE_BLEND_SHIFT) & 3u)

#endif /* MOTE_OBJECT_H */
