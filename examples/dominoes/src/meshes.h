/*
 * Meshes for the dominoes demo: one thin-tall domino box and a floor quad.
 *
 * Mesh verts are int8 spanning +-127 and scaled by mesh.scale/127 into world
 * metres, so a box's per-axis proportions are baked into the vertex coords:
 * the LONGEST half-extent maps to +-127 (and equals mesh.scale), the others
 * scale down proportionally. Domino half-extents (0.05, 0.30, 0.18) m:
 *   longest = 0.30 (Y) -> scale = 0.30, y verts = +-127
 *   x: 0.05/0.30*127 ~= 21   z: 0.18/0.30*127 ~= 76
 * Rendered with scene_add_object (the body's orient is the basis, scale 1).
 */
#ifndef DOMINO_MESHES_H
#define DOMINO_MESHES_H

#include "mote_mesh.h"
#include "mote_config.h"

/* Domino box: thin in X (21), tall in Y (127), medium in Z (76). */
static const MeshVert k_dom_v[8] = {
    {-21,-127,-76},{21,-127,-76},{21,127,-76},{-21,127,-76},
    {-21,-127, 76},{21,-127, 76},{21,127, 76},{-21,127, 76},
};
/* Faces: CCW from outside. Light face/edge colours so shading reads clearly. */
#define DF MOTE_RGB565(225, 225, 235)   /* broad faces (front/back, +-Z) */
#define DE MOTE_RGB565(180, 185, 200)   /* thin ends (left/right, +-X) */
#define DT MOTE_RGB565(240, 240, 250)   /* top/bottom (+-Y) */
static const MeshFace k_dom_f[12] = {
    {4,5,6,0,0,127,DF},{4,6,7,0,0,127,DF},   /* +Z front */
    {1,0,3,0,0,-127,DF},{1,3,2,0,0,-127,DF}, /* -Z back  */
    {5,1,2,127,0,0,DE},{5,2,6,127,0,0,DE},   /* +X end   */
    {0,4,7,-127,0,0,DE},{0,7,3,-127,0,0,DE}, /* -X end   */
    {7,6,2,0,127,0,DT},{7,2,3,0,127,0,DT},   /* +Y top   */
    {0,1,5,0,-127,0,DT},{0,5,4,0,-127,0,DT}, /* -Y bottom */
};
static const Mesh k_domino_mesh = { k_dom_v, k_dom_f, 8, 12, 0.30f, 0.40f, 0 };

/* Floor: a unit quad in the X-Z plane (y=0), normal up. */
static const MeshVert k_floor_v[4] = {
    {-127, 0, -127}, {127, 0, -127}, {127, 0, 127}, {-127, 0, 127},
};
#define FC MOTE_RGB565(55, 80, 60)
static const MeshFace k_floor_f[2] = {
    {0, 3, 2,  0, 127, 0, FC}, {0, 2, 1,  0, 127, 0, FC},
};
static const Mesh k_floor_mesh = { k_floor_v, k_floor_f, 4, 2, 1.0f, 1.5f, 0 };

#endif
