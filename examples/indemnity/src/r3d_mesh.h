/*
 * ThumbyElite — mesh data format.
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
#ifndef R3D_MESH_H
#define R3D_MESH_H

#include <stdint.h>

typedef struct { int8_t x, y, z; } MeshVert;

typedef struct {
    uint8_t a, b, c;        /* vertex indices */
    int8_t  nx, ny, nz;     /* model-space face normal, quantised (unit*127) */
    uint16_t color;         /* RGB565 base albedo */
} MeshFace;

typedef struct Mesh {
    const MeshVert *verts;
    const MeshFace *faces;
    uint16_t nverts;
    uint16_t nfaces;
    float scale;            /* model half-extent in meters */
    float bound_r;          /* bounding-sphere radius in meters */
    const struct Mesh *lod_lo;  /* optional lower-detail swap, NULL if none */
} Mesh;

#endif
