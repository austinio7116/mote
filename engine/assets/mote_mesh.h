/*
 * Mote — mesh data format (ported from ThumbyElite r3d_mesh.h).
 *
 * Meshes live in flash (.rodata) as quantised int8 vertices + 8-byte faces.
 * Winding: counter-clockwise viewed from OUTSIDE (outward normal =
 * (b-a)x(c-a) in right-handed model space). The pipe's y-flip projection
 * turns that into clockwise/positive-area on screen, which is what the
 * rasteriser treats as front-facing.
 *
 * world_pos = vert * (scale / 127): scale is the model's half-extent in
 * world units (meters), so an int8 of +-127 spans +-scale.
 */
#ifndef MOTE_MESH_H
#define MOTE_MESH_H

#include <stdint.h>

typedef struct { int8_t x, y, z; } MeshVert;

typedef struct {
    uint8_t a, b, c;        /* vertex indices */
    int8_t  nx, ny, nz;     /* model-space face normal, quantised (unit*127) */
} MeshFace;                 /* 6 bytes — colour lives on the Mesh (see below) */

typedef struct Mesh {
    const MeshVert *verts;
    const MeshFace *faces;
    /* Colour: most meshes are one flat albedo, so it lives here (1x per mesh) rather
     * than 2 bytes on every face. A mesh that needs per-face colour (multi-material
     * models, height-tinted terrain) sets face_colors to an nfaces-long RGB565 array
     * and leaves `color` unused; otherwise face_colors is NULL and `color` applies to
     * every face. A per-draw MoteObject.color override beats both. */
    const uint16_t *face_colors;
    uint16_t nverts;
    uint16_t nfaces;
    uint16_t color;             /* base albedo (RGB565) when face_colors == NULL */
    float scale;                /* model half-extent in meters */
    float bound_r;              /* bounding-sphere radius in meters */
    const struct Mesh *lod_lo;  /* optional lower-detail swap, NULL if none */
} Mesh;

/* A bakeable MODEL is just its chunk list. A single STL is split into <=255-vert
 * Mesh chunks (the uint8 face-index cap); MoteModel groups them so a game draws the
 * whole thing in one call (mote_model_draw) and never touches the chunk array or the
 * count. The Studio/CLI baker emits a `static const MoteModel <name>` next to the
 * chunks, plus `<name>_TRIS` (total faces) to use as the .max_tris pool size. */
typedef struct MoteModel {
    const Mesh *chunks;
    uint16_t    count;
    uint16_t    tris;       /* total faces across all chunks (== <name>_TRIS) */
} MoteModel;

#endif /* MOTE_MESH_H */
