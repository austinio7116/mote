/*
 * CueVR — the table's body: apron, cabinet and legs.
 *
 * Deliberately NOT part of the table. cue_render.c owns the bed, the cushions,
 * the jaws and the pockets — the shapes that were tuned table by table and that
 * the balls actually collide with — and nothing here touches them. This builds
 * only what is underneath: the woodwork the slate sits on.
 *
 * Which means a frame is swappable. A design is a name and a function that
 * fits itself to a CueTable, so all seven tables get a frame in proportion
 * without anyone drawing seven frames, and a second design is a second
 * function in the array at the bottom of cuevr_frame.c. There is no GL in
 * here: a design emits triangles and the renderer uploads them, so a frame can
 * also be measured, diffed or dumped without a graphics context.
 */
#ifndef CUEVR_FRAME_H
#define CUEVR_FRAME_H

#include <stdint.h>
#include "cue_table.h"

/* Same layout the renderer's vertex buffer uses, so a built frame goes to the
 * GPU without a translation pass. uv.x runs ALONG the grain of whichever piece
 * of timber the vertex belongs to and uv.y across it, which is what lets the
 * shader draw grain that follows the wood rather than the world. */
typedef struct {
    float p[3];      /* table space: metres, X length, Z width, Y up from cloth */
    float n[3];
    float uv[2];     /* along-grain, across-grain */
    float c[3];      /* linear-ish base colour, so one design can mix timbers */
} CueVrFrameVtx;

typedef struct {
    CueVrFrameVtx *v;
    uint16_t      *idx;
    int nv, ni;
    int cap_v, cap_i;
    int overflow;    /* set if the design ran out of room — check it */
} CueVrFrameMesh;

typedef struct {
    const char *name;
    void (*build)(CueVrFrameMesh *m, const CueTable *t);
} CueVrFrameDesign;

extern const CueVrFrameDesign CUEVR_FRAMES[];
extern const int CUEVR_FRAME_COUNT;

/* Build design `which` for `t` into `m`. The caller owns m->v / m->idx and sets
 * the caps; cuevr_frame_capacity() says how much is enough for any design. */
/* The timber every design is made of: the colour the player picked for the
 * cushion rails. Set it before building, or the body will be a different wood
 * from the rails standing on it. Metal and the black down a pocket are not
 * affected. */
void cuevr_frame_set_timber(const float rgb[3]);

void cuevr_frame_build(int which, CueVrFrameMesh *m, const CueTable *t);

/* The design that suits `t` when the player has not picked one — a pub table is
 * a cabinet, a 9 ft American is an American, and so on. */
int  cuevr_frame_default(const CueTable *t);
void cuevr_frame_capacity(int *max_verts, int *max_indices);

/* How far below the cloth the frame reaches — the renderer wants it to know
 * where the floor is relative to the table. Always the table's own height. */
float cuevr_frame_depth(const CueTable *t);

#endif /* CUEVR_FRAME_H */
