#include "gta3_veh.h"

#include "mote_build.h"    /* mote__face: normals computed from the geometry */

/* CAR_CLS order, from game.c:
 *  0 SEDAN  1 COMPACT  2 COUPE  3 SPORTS  4 RACER  5 MUSCLE  6 HOTHATCH
 *  7 CLASSIC  8 CLASSICSPT  9 LUXURY  10 WAGON  11 VAN  12 PICKUP  13 JEEP
 * 14 TAXI  15 POLICE  16 AMBULANCE  17 FIRETRUCK  18 TOWTRUCK */
static const unsigned char SIL[19] = {
    GTA3_SIL_SEDAN,      /*  0 SEDAN      */
    GTA3_SIL_COMPACT,    /*  1 COMPACT    */
    GTA3_SIL_COUPE,      /*  2 COUPE      */
    GTA3_SIL_SPORTS,     /*  3 SPORTS     */
    GTA3_SIL_RACER,      /*  4 RACER      */
    GTA3_SIL_LONGHOOD,   /*  5 MUSCLE     */
    GTA3_SIL_COMPACT,    /*  6 HOTHATCH   */
    GTA3_SIL_LONGHOOD,   /*  7 CLASSIC    */
    GTA3_SIL_CLASSICSPT, /*  8 CLASSICSPT */
    GTA3_SIL_COUPE,      /*  9 LUXURY  -> folded into COUPE, see gta3_veh.h */
    GTA3_SIL_WAGON,      /* 10 WAGON      */
    GTA3_SIL_VAN,        /* 11 VAN        */
    GTA3_SIL_PICKUP,     /* 12 PICKUP     */
    GTA3_SIL_JEEP,       /* 13 JEEP       */
    GTA3_SIL_TAXI,       /* 14 TAXI       */
    GTA3_SIL_SEDAN,      /* 15 POLICE     */
    GTA3_SIL_TRUCK,      /* 16 AMBULANCE  */
    GTA3_SIL_TRUCK,      /* 17 FIRETRUCK  */
    GTA3_SIL_TRUCK,      /* 18 TOWTRUCK   */
};

int gta3_sil_for_class(int car_cls) {
    if (car_cls < 0 || car_cls >= 19) return GTA3_SIL_SEDAN;
    return SIL[car_cls];
}

/* Lamp shape per silhouette. Round is the default and costs nothing (the
 * caller draws discs); the other two cost triangles, so they go to the shapes
 * where they actually read: square lamps on the tall slab-sided vehicles, a
 * light bar on the low ones where a round pair would look wrong. */
static const unsigned char LAMP[GTA3_SIL_N] = {
    /* SEDAN      */ GTA3_LAMP_ROUND,
    /* COMPACT    */ GTA3_LAMP_ROUND,
    /* COUPE      */ GTA3_LAMP_RECT,
    /* LONGHOOD   */ GTA3_LAMP_ROUND,
    /* WAGON      */ GTA3_LAMP_RECT,
    /* VAN        */ GTA3_LAMP_RECT,
    /* TRUCK      */ GTA3_LAMP_RECT,
    /* RACER      */ GTA3_LAMP_BAR,
    /* PICKUP     */ GTA3_LAMP_RECT,
    /* JEEP       */ GTA3_LAMP_ROUND,
    /* SPORTS     */ GTA3_LAMP_BAR,
    /* CLASSICSPT */ GTA3_LAMP_ROUND,
    /* TAXI       */ GTA3_LAMP_ROUND,
    /* HELI       */ GTA3_LAMP_ROUND,
    /* BUS        */ GTA3_LAMP_RECT,
};

int gta3_lamp_style(int sil) {
    if (sil < 0 || sil >= GTA3_SIL_N) return GTA3_LAMP_ROUND;
    return LAMP[sil];
}

/* Per silhouette, in normalised int8 space (127 = the car's half-length):
 *   body_h  : how tall the body box is
 *   cab_top : the roofline
 *   cab_z0  : where the cabin starts, toward the tail
 *   cab_z1  : where the cabin ends, toward the nose
 *   cab_w   : the cabin's half-width as a fraction of the body's
 * A long hood pushes cab_z0/z1 toward the tail; a van runs the cabin nearly the
 * whole length and stands tall; a racer is low with a shallow glasshouse. */
typedef struct { int body_h, cab_top, cab_z0, cab_z1, cab_w; } Sil;
static const Sil SILDEF[GTA3_SIL_N] = {
    /* SEDAN      */ { 34, 62, -55,  40, 88 },
    /* COMPACT    */ { 36, 64, -50,  50, 88 },
    /* COUPE      */ { 30, 54, -62,  22, 86 },   /* notchback: cabin set back over a short rear deck */
    /* LONGHOOD   */ { 32, 58, -72,  14, 86 },
    /* WAGON      */ { 40, 74, -55,  55, 90 },
    /* VAN: a BOX with a short hood. It used to run the cabin -80..70, which
     * left a long bonnet at each end and read as a tall estate car. The cabin
     * now runs -122..76 at 96% of the body width, so the only thing in front
     * of it is the 76..127 stub of hood, and the sides are flush. */
    /* VAN        */ { 42,104,-122,  76, 96 },
    /* TRUCK: short cab set FORWARD, with a bonnet in front of it and a long
     * flat bed behind. It used to run the cabin from -30 to 80 at 92% of the
     * body width, which is a box van: one slab most of the length of the
     * chassis. The cab is now 34..96 — about a quarter of the length, up at
     * the front — so z 96..127 reads as the engine and everything behind 34 is
     * open deck. Narrower too, at 84%, so the bed's sides show past it. */
    /* TRUCK      */ { 40, 92,  34,  96, 84 },
    /* RACER      */ { 20, 36, -52,   8, 80 },   /* lowest of the lot, shallow glasshouse */
    /* PICKUP     */ { 38, 74, -18,  62, 90 },   /* cab over the front half; bed behind is bare body */
    /* JEEP       */ { 44, 86, -60,  58, 92 },   /* short and tall, near-vertical glass */
    /* SPORTS     */ { 26, 46, -58,   2, 82 },   /* long bonnet, cabin set back toward the tail */
    /* CLASSICSPT */ { 32, 60, -58,   6, 84 },   /* long bonnet under a tall upright glasshouse */
    /* TAXI       */ { 38, 70, -52,  44, 90 },   /* a sedan made taller and squarer */
    /* HELI: the three boxes read as an aircraft rather than a car.
     *   body_h 44 / cab_top 76 — a shallower fuselage under a lower canopy.
     *     At 54/86 both boxes were deep enough that the whole thing read as
     *     one stacked block.
     *   cab_z0 50 / cab_z1 118 — the canopy is now 68 units long against its
     *     68 units of width, i.e. SQUARE in plan and sitting over the nose. It
     *     used to run 94 units back, which is a long rectangle on top of a
     *     longer one: two boxes, nothing aircraft about it.
     *   cab_w 78 — narrower than the fuselage, so the canopy reads as glass
     *     set into the shell rather than a second storey.
     * The fuselage itself is cut short at z = -24 (see gta3_veh_build); the
     * tail boom and both rotors are triangles the caller draws, because a
     * rotor spins and this mesh is built once at init. */
    /* HELI       */ { 44, 76,  50, 118, 78 },
    /* BUS: one long rectangle. It used to borrow the VAN silhouette, which
     * gave an 8.6 m vehicle a van's nose and shoulders; at that length the
     * proportions read as a stretched van rather than a bus. The cabin spans
     * -124..114 at 97% of the width and stands 116 tall, so what you see is a
     * single flush box with a flat front. */
    /* BUS        */ { 34,116,-124, 114, 97 },
};

void gta3_box(MeshVert *v, MeshFace *f, int *nf,
             int x0, int x1, int y0, int y1, int z0, int z1) {
    const int C[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
                          {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
    for (int i = 0; i < 8; i++) {
        v[i].x = (signed char)C[i][0];
        v[i].y = (signed char)C[i][1];
        v[i].z = (signed char)C[i][2];
    }
    /* CCW from outside, so mote__face derives outward normals. */
    *nf = 0;
    mote__face(v, f, nf, 0, 2, 1, 0); mote__face(v, f, nf, 0, 3, 2, 0);  /* -Z */
    mote__face(v, f, nf, 4, 5, 6, 0); mote__face(v, f, nf, 4, 6, 7, 0);  /* +Z */
    mote__face(v, f, nf, 0, 4, 7, 0); mote__face(v, f, nf, 0, 7, 3, 0);  /* -X */
    mote__face(v, f, nf, 1, 2, 6, 0); mote__face(v, f, nf, 1, 6, 5, 0);  /* +X */
    mote__face(v, f, nf, 0, 1, 5, 0); mote__face(v, f, nf, 0, 5, 4, 0);  /* -Y */
    mote__face(v, f, nf, 3, 7, 6, 0); mote__face(v, f, nf, 3, 6, 2, 0);  /* +Y */
}

void gta3_veh_build(Gta3VehMesh *m, int sil) {
    if (sil < 0 || sil >= GTA3_SIL_N) sil = GTA3_SIL_SEDAN;
    const Sil *s = &SILDEF[sil];
    int nf;

    /* 118, not 127: the wheel slab below runs to the int8 limit at 127 so it sits
     * PROUD of the bodywork, which is what reads as a tyre track. The body being
     * a few percent narrower than the track is also true of real cars. */
    /* The helicopter is authored at its FINAL proportions, not squeezed later.
     * Every car is authored square in x/z and the caller narrows x by the
     * measured wid/len; that path assigns one x to all four corners of both
     * third-box slabs at once, which is right for axles and would collapse
     * two skids onto the centreline. So the aircraft carries its own
     * half-width here (44 of 127, i.e. a 2.4 m fuselage on a 7 m airframe)
     * and draw_vehicle_mesh skips the squeeze for it.
     *
     * Its fuselage also stops short of the tail: a full-length slab reads as
     * a bus, and the space behind it is where the caller draws the boom. */
    int heli = (sil == GTA3_SIL_HELI);
    int halfw = heli ? 44 : 118;
    int bz0   = heli ? -24 : -127;
    gta3_box(m->bv, m->bf, &nf, -halfw, halfw, 0, s->body_h, bz0, 127);
    m->body = (Mesh){ .verts=m->bv, .faces=m->bf, .nverts=8, .nfaces=nf,
                      .scale=1.0f, .bound_r=1.8f, .color=0xFFFF };

    int cw = (halfw * s->cab_w) / 100;   /* cab_w is a fraction of the BODY half-width */
    gta3_box(m->cv, m->cf, &nf, -cw, cw, s->body_h, s->cab_top, s->cab_z0, s->cab_z1);
    m->cabin = (Mesh){ .verts=m->cv, .faces=m->cf, .nverts=8, .nfaces=nf,
                       .scale=1.0f, .bound_r=1.8f, .color=MOTE_RGB565(40,46,60) };

    /* Wheel line: one slab, proud of the body in x and hanging below it, so from
     * the chase camera it reads as the tyre track and the dark gap under the sill.
     * Inset in z so it stops short of the bumpers rather than running the full
     * length. It does NOT dip below y=0: the road is a flat quad at y=0, so
     * anything under that is simply buried. 12 triangles — a box per corner
     * would be 48, and at 18 live cars that alone exceeds max_tris. */
    /* TWO axle slabs with the middle open, so the gap between them reads as
     * four tyres rather than one rectangle down the sill. Each is built with
     * the shared box builder into its own half of the arrays, and the second
     * box's face indices are shifted by 8 because mote__face writes indices
     * relative to the vertex pointer it was handed. */
    /* A helicopter's third box pair is SKIDS, not axles: two rails running
     * most of the length, outboard of the fuselage and standing the aircraft
     * clear of the ground. Same 24 triangles, same two boxes, different
     * extents — so nothing else in the builder or the caller changes. */
    int nf2;
    if (heli) {
        gta3_box(m->wv,     m->wf,      &nf,  -62, -46, 0, 14, -16, 104);   /* left skid */
        gta3_box(m->wv + 8, m->wf + 12, &nf2,  46,  62, 0, 14, -16, 104);   /* right skid */
    } else {
        gta3_box(m->wv,     m->wf,      &nf,  -127, 127, 0, 16, -96, -40);  /* rear axle */
        gta3_box(m->wv + 8, m->wf + 12, &nf2, -127, 127, 0, 16,  40,  96);  /* front axle */
    }
    for (int i = 0; i < nf2; i++) {
        m->wf[12+i].a = (uint8_t)(m->wf[12+i].a + 8);
        m->wf[12+i].b = (uint8_t)(m->wf[12+i].b + 8);
        m->wf[12+i].c = (uint8_t)(m->wf[12+i].c + 8);
    }
    m->wheels = (Mesh){ .verts=m->wv, .faces=m->wf, .nverts=16, .nfaces=nf+nf2,
                        .scale=1.0f, .bound_r=1.8f, .color=MOTE_RGB565(24,24,28) };
}
