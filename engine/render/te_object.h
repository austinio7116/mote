/*
 * ThumbyEngine — renderable 3D object (header-only).
 *
 * A mesh placed in the camera-relative world with an orientation basis.
 * Split out from te_pipe.h so game modules can use the type via the ABI
 * without seeing the pipe's function declarations.
 */
#ifndef TE_OBJECT_H
#define TE_OBJECT_H

#include "te_vec.h"
#include "te_mesh.h"

typedef struct {
    Vec3 pos;          /* camera-relative world position */
    Mat3 basis;        /* object orientation (rows: right/up/forward) */
    const Mesh *mesh;
} TeObject;

#endif /* TE_OBJECT_H */
