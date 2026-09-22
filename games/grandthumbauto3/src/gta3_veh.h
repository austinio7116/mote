/*
 * Grand Thumb Auto III — vehicle silhouettes.
 *
 * The game has 54 car types. Authoring 54 meshes would be absurd and shipping
 * 54 sprite sheets of 8 yaw angles each would not fit in flash, so identity
 * comes from COLOUR and the geometry collapses to seven shapes: the 19
 * CAR_CLS handling classes map onto a sedan, a compact, a low wedge, a long
 * hood, a wagon, a van and a truck.
 *
 * Each vehicle is two boxes — a body and a cabin — drawn as two objects with
 * separate per-draw colours, because MoteObject.color is a single colour for
 * the whole object and a car with no windows reads as a brick.
 *
 * No wheels. Four more boxes is 48 triangles per car for something two pixels
 * tall on a 128x128 screen.
 *
 * Dimensions are normalised: the body spans +/-1 in each axis before the Mesh
 * scale is applied, so the caller sizes each car from its own measured art
 * (cars2_meta.h) and handling stays exactly as it was tuned.
 */
#ifndef GTA3_VEH_H
#define GTA3_VEH_H

#include "mote_mesh.h"

enum { GTA3_SIL_SEDAN, GTA3_SIL_COMPACT, GTA3_SIL_WEDGE, GTA3_SIL_LONGHOOD,
       GTA3_SIL_WAGON, GTA3_SIL_VAN, GTA3_SIL_TRUCK, GTA3_SIL_N };

/* Map one of the 19 CAR_CLS handling classes to a silhouette. Out-of-range
 * input returns GTA3_SIL_SEDAN rather than reading off the end. */
int gta3_sil_for_class(int car_cls);

typedef struct {
    MeshVert bv[8]; MeshFace bf[12];    /* body box */
    MeshVert cv[8]; MeshFace cf[12];    /* cabin box */
    Mesh body, cabin;
} Gta3VehMesh;

/* Build both boxes for a silhouette. Local axes: +Z along the heading (nose),
 * +X to the right, +Y up. The body sits on y=0 and both meshes carry
 * .scale = 1, so the caller draws with scene_add_object_scaled or sets .scale
 * to half the car's longest dimension. */
void gta3_veh_build(Gta3VehMesh *m, int sil);

/* The box builder gta3_veh_build uses internally, exposed so other geometry
 * (Task 9's tank turret) can build off-hull boxes without duplicating the
 * winding: CCW from outside in this normalised int8 space, so mote__face
 * derives outward normals AND the result is front-facing in SCREEN space
 * once projected (see test_veh.c's screen_winding_ok for why those two are
 * not the same guarantee). x0<x1, y0<y1, z0<z1 is assumed but not enforced;
 * *nf is reset to 0 and left at 12. */
void gta3_box(MeshVert *v, MeshFace *f, int *nf,
             int x0, int x1, int y0, int y1, int z0, int z1);

#endif /* GTA3_VEH_H */
