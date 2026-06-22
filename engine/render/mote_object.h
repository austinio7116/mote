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

#endif /* MOTE_OBJECT_H */
