#include "gta3_veh.h"

#include "mote_build.h"    /* mote__face: normals computed from the geometry */

/* CAR_CLS order, from game.c:
 *  0 SEDAN  1 COMPACT  2 COUPE  3 SPORTS  4 RACER  5 MUSCLE  6 HOTHATCH
 *  7 CLASSIC  8 CLASSICSPT  9 LUXURY  10 WAGON  11 VAN  12 PICKUP  13 JEEP
 * 14 TAXI  15 POLICE  16 AMBULANCE  17 FIRETRUCK  18 TOWTRUCK */
static const unsigned char SIL[19] = {
    GTA3_SIL_SEDAN,    /*  0 SEDAN      */
    GTA3_SIL_COMPACT,  /*  1 COMPACT    */
    GTA3_SIL_WEDGE,    /*  2 COUPE      */
    GTA3_SIL_WEDGE,    /*  3 SPORTS     */
    GTA3_SIL_RACER,    /*  4 RACER      */
    GTA3_SIL_LONGHOOD, /*  5 MUSCLE     */
    GTA3_SIL_COMPACT,  /*  6 HOTHATCH   */
    GTA3_SIL_LONGHOOD, /*  7 CLASSIC    */
    GTA3_SIL_WEDGE,    /*  8 CLASSICSPT */
    GTA3_SIL_LUXURY,   /*  9 LUXURY     */
    GTA3_SIL_WAGON,    /* 10 WAGON      */
    GTA3_SIL_VAN,      /* 11 VAN        */
    GTA3_SIL_PICKUP,   /* 12 PICKUP     */
    GTA3_SIL_JEEP,     /* 13 JEEP       */
    GTA3_SIL_SEDAN,    /* 14 TAXI       */
    GTA3_SIL_SEDAN,    /* 15 POLICE     */
    GTA3_SIL_TRUCK,    /* 16 AMBULANCE  */
    GTA3_SIL_TRUCK,    /* 17 FIRETRUCK  */
    GTA3_SIL_TRUCK,    /* 18 TOWTRUCK   */
};

int gta3_sil_for_class(int car_cls) {
    if (car_cls < 0 || car_cls >= 19) return GTA3_SIL_SEDAN;
    return SIL[car_cls];
}

/* Per silhouette, in normalised int8 space (127 = the car's half-length):
 *   body_h  : how tall the body box is
 *   cab_top : the roofline
 *   cab_z0  : where the cabin starts, toward the tail
 *   cab_z1  : where the cabin ends, toward the nose
 *   cab_w   : the cabin's half-width as a fraction of the body's
 * A long hood pushes cab_z0/z1 toward the tail; a van runs the cabin nearly the
 * whole length and stands tall; a wedge is low with a shallow cabin. */
typedef struct { int body_h, cab_top, cab_z0, cab_z1, cab_w; } Sil;
static const Sil SILDEF[GTA3_SIL_N] = {
    /* SEDAN    */ { 34, 62, -55,  40, 88 },
    /* COMPACT  */ { 36, 64, -50,  50, 88 },
    /* WEDGE    */ { 28, 48, -50,  25, 84 },
    /* LONGHOOD */ { 32, 58, -72,  14, 86 },
    /* WAGON    */ { 40, 74, -55,  55, 90 },
    /* VAN      */ { 46, 96, -80,  70, 94 },
    /* TRUCK    */ { 44, 88, -30,  80, 92 },
    /* LUXURY   */ { 32, 58, -62,  30, 88 },   /* longer bonnet, lower roof than SEDAN */
    /* RACER    */ { 24, 42, -46,  18, 82 },   /* lowest of the lot, shallow glasshouse */
    /* PICKUP   */ { 38, 74, -18,  62, 90 },   /* cab over the front half; bed behind is bare body */
    /* JEEP     */ { 44, 86, -60,  58, 92 },   /* short and tall, near-vertical glass */
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
    gta3_box(m->bv, m->bf, &nf, -118, 118, 0, s->body_h, -127, 127);
    m->body = (Mesh){ .verts=m->bv, .faces=m->bf, .nverts=8, .nfaces=nf,
                      .scale=1.0f, .bound_r=1.8f, .color=0xFFFF };

    int cw = (118 * s->cab_w) / 100;   /* cab_w is a fraction of the BODY half-width */
    gta3_box(m->cv, m->cf, &nf, -cw, cw, s->body_h, s->cab_top, s->cab_z0, s->cab_z1);
    m->cabin = (Mesh){ .verts=m->cv, .faces=m->cf, .nverts=8, .nfaces=nf,
                       .scale=1.0f, .bound_r=1.8f, .color=MOTE_RGB565(40,46,60) };

    /* Wheel line: one slab, proud of the body in x and hanging below it, so from
     * the chase camera it reads as the tyre track and the dark gap under the sill.
     * Inset in z so it stops short of the bumpers rather than running the full
     * length. It does NOT dip below y=0: the road is a flat quad at y=0, so
     * anything under that is simply buried. 12 triangles — a box per corner
     * would be 48, and at 18 live cars that alone exceeds max_tris. */
    gta3_box(m->wv, m->wf, &nf, -127, 127, 0, 16, -96, 96);
    m->wheels = (Mesh){ .verts=m->wv, .faces=m->wf, .nverts=8, .nfaces=nf,
                        .scale=1.0f, .bound_r=1.8f, .color=MOTE_RGB565(24,24,28) };
}
