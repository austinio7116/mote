/*
 * Indemnity Run — mesh data format.
 *
 * Now SHARED with the engine: forwards to the engine's mote_mesh.h so a mesh IS
 * a MoteModel/Mesh the engine renders directly via scene_add_object. The engine
 * face is 6 bytes (a,b,c + quantised normal); per-face colour lives in a
 * parallel `face_colors` array on the Mesh (NULL = use the single `color`).
 * Indemnity's generators (ship_gen, station_gen) and baked meshes (meshes_gen)
 * populate face_colors.
 */
#ifndef R3D_MESH_H
#define R3D_MESH_H

#include "mote_mesh.h"

#endif
