/*
 * Tumbler's mesh — same cube geometry as hello-mesh, recoloured to a neon
 * palette so the two games are visibly different in the launcher.
 */
#ifndef TUMBLER_CUBE2_H
#define TUMBLER_CUBE2_H

#include "mote_mesh.h"
#include "mote_config.h"

static const MeshVert k_verts[8] = {
    {-127, -127, -127}, { 127, -127, -127}, { 127,  127, -127}, {-127,  127, -127},
    {-127, -127,  127}, { 127, -127,  127}, { 127,  127,  127}, {-127,  127,  127},
};

#define N0 MOTE_RGB565(255,  64, 160)   /* neon pink */
#define N1 MOTE_RGB565( 64, 255, 200)   /* neon teal */
#define N2 MOTE_RGB565(255, 220,  64)   /* neon amber */

static const MeshFace k_faces[12] = {
    {4, 5, 6,    0,   0, 127, N0}, {4, 6, 7,    0,   0, 127, N0},   /* +Z */
    {1, 0, 3,    0,   0,-127, N0}, {1, 3, 2,    0,   0,-127, N0},   /* -Z */
    {5, 1, 2,  127,   0,   0, N1}, {5, 2, 6,  127,   0,   0, N1},   /* +X */
    {0, 4, 7, -127,   0,   0, N1}, {0, 7, 3, -127,   0,   0, N1},   /* -X */
    {7, 6, 2,    0, 127,   0, N2}, {7, 2, 3,    0, 127,   0, N2},   /* +Y */
    {0, 1, 5,    0,-127,   0, N2}, {0, 5, 4,    0,-127,   0, N2},   /* -Y */
};

static const Mesh k_mesh = {
    .verts = k_verts, .faces = k_faces, .nverts = 8, .nfaces = 12,
    .scale = 1.0f, .bound_r = 1.8f, .lod_lo = 0,
};

#endif /* TUMBLER_CUBE2_H */
