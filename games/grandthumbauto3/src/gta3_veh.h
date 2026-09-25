/*
 * Grand Thumb Auto III — vehicle silhouettes.
 *
 * The game has 54 car types. Authoring 54 meshes would be absurd and shipping
 * 54 sprite sheets of 8 yaw angles each would not fit in flash, so identity
 * comes from COLOUR and the geometry collapses to fourteen shapes, which the 19
 * CAR_CLS handling classes map onto. Adding a silhouette is nearly free: it is
 * one SILDEF row and one more Gta3VehMesh built at init (~400 B), with no
 * per-frame cost at all, since the mesh is picked per car type.
 *
 * Each vehicle is two boxes — a body and a cabin — drawn as two objects with
 * separate per-draw colours, because MoteObject.color is a single colour for
 * the whole object and a car with no windows reads as a brick.
 *
 * Wheels are TWO boxes, not four. A box per corner is 48 triangles per car,
 * and with 18 cars alive that exceeds the whole max_tris budget on its own. A
 * single slab spanning the whole wheelbase was 12 triangles but read as a
 * rectangle painted along the sill — no wheels at all. Two slabs, one per
 * axle, with the middle of the car left open, read as a front and rear pair
 * for 24 triangles, and the caller only draws them for cars close enough to
 * tell.
 *
 * Dimensions are normalised: the body spans +/-1 in each axis before the Mesh
 * scale is applied, so the caller sizes each car from its own measured art
 * (cars2_meta.h) and handling stays exactly as it was tuned.
 */
#ifndef GTA3_VEH_H
#define GTA3_VEH_H

#include "mote_mesh.h"

/* TAXI is GONE too, folded into WAGON. On the same weighted parameter distance
 * that retired LUXURY, WAGON and TAXI were 14.4 apart against the roughly 12
 * the per-type cabin jitter already covers. A taxi is not identified by its
 * shape any more: draw_vehicle_mesh paints CAR_TAXI yellow and gives it a black
 * roof sign, and both survive the fold. What it loses is two units of body
 * height and eleven of cabin length, which buys the 576 bytes of GAME_RAM the
 * boat's silhouette costs.
 *
 * NOT folded, though it is closer at 13.0: SEDAN and COMPACT. They are the two
 * biggest users, 15 of the 54 car types between them, and merging them takes
 * the worst-case silhouette share from 17% to 28% — undoing the split that
 * brought it down from 46%. Closest is not the same as foldable.
 */

/* LUXURY is GONE, folded into COUPE. Measured across all sixteen rows on a
 * weighted parameter distance, COUPE and LUXURY were the closest pair that was
 * not simply the two biggest users: 13.8, against the roughly 12 that the
 * per-type cabin jitter (+-3 units of roof and +-3 fore/aft) already covers.
 * The two rows differed by 2 units of body height, 4 of roofline, 2 of cabin
 * width and 8 of cabin length, with an identical cab_z0 — a difference no one
 * could see, for 504 bytes of GAME_RAM, which is the scarcest thing in this
 * game. LUXURY's three car types now draw as COUPE, and take its RECT lamps in
 * place of the light BAR.
 *
 * There used to be a single GTA3_SIL_WEDGE carrying the COUPE, SPORTS and
 * CLASSICSPT handling classes at once — 14 of the 54 car types, 26% of every
 * car on the road, all the same two boxes in different paint. Counted, not
 * assumed: WEDGE 14 types and SEDAN 11 meant 46% of traffic was one of two
 * shapes. WEDGE is now three shapes and TAXI has its own, which takes the
 * worst-case share to 11%. A silhouette is cheap — one SILDEF row and ~400 B
 * of mesh built once at init, with no per-frame cost, since the mesh is
 * picked per car type. */
enum { GTA3_SIL_SEDAN, GTA3_SIL_COMPACT, GTA3_SIL_COUPE, GTA3_SIL_LONGHOOD,
       GTA3_SIL_WAGON, GTA3_SIL_VAN, GTA3_SIL_TRUCK,
       GTA3_SIL_RACER,    /* very low and long — the supercars */
       GTA3_SIL_PICKUP,   /* cab over the front half only, open bed behind */
       GTA3_SIL_JEEP,     /* short, tall, upright glasshouse */
       GTA3_SIL_SPORTS,   /* low, cabin pushed forward over a long tail */
       GTA3_SIL_CLASSICSPT, /* long bonnet, tall upright glasshouse */
       /* The helicopter reuses the three-box layout as fuselage / canopy /
        * skids rather than body / cabin / wheels, so it needs no new fields and
        * no new builder — only its own SILDEF row. The rotor is not in the mesh:
        * it spins, so the caller draws it as four triangles at a phase angle. */
       GTA3_SIL_HELI,
       GTA3_SIL_BUS,      /* one long rectangular box: no nose to speak of */
       GTA3_SIL_TANK,     /* slab hull, low superstructure, full-length tracks */
       GTA3_SIL_N };

/* Map one of the 19 CAR_CLS handling classes to a silhouette. Out-of-range
 * input returns GTA3_SIL_SEDAN rather than reading off the end. */
int gta3_sil_for_class(int car_cls);

/* Lamp shape, chosen by silhouette so the lights agree with the bodywork: a
 * van gets square lamps and a supercar a light bar, rather than every car on
 * the road wearing the same round pair. Read by the caller, which owns the
 * geometry — this header only says which shape a shape should wear. */
enum { GTA3_LAMP_ROUND, GTA3_LAMP_RECT, GTA3_LAMP_BAR };
int gta3_lamp_style(int sil);

typedef struct {
    MeshVert bv[8]; MeshFace bf[12];    /* body box */
    MeshVert cv[8]; MeshFace cf[12];    /* cabin box */
    MeshVert wv[16]; MeshFace wf[24];   /* wheels: TWO axle slabs, front and rear */
    Mesh body, cabin, wheels;
} Gta3VehMesh;

/* Build all three boxes for a silhouette. Local axes: +Z along the heading (nose),
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
