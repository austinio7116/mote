/* Deterministic headless box-stacking test — no rendering, just positions. */
#include "mote_phys.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    MoteWorld w;
    mote_phys_world_defaults(&w);
    w.bmin = v3(-2, -2, -2);
    w.bmax = v3( 2,  2,  2);
    w.substep = 1.0f / 120.0f;
    w.max_substeps = 8;
    w.restitution = 0.0f;          /* no bounce: should settle flat */

#define NB 8
    MoteBody b[NB];
    memset(b, 0, sizeof b);
    for (int i = 0; i < NB; i++) {
        b[i].shape = MOTE_SHAPE_BOX;
        b[i].half = v3(0.24f, 0.24f, 0.24f);
        b[i].radius = 0.26f;
        b[i].inv_mass = 1.0f / 0.3f;
        b[i].orient = m3_identity();
        b[i].pos = v3(0.0f, -1.4f + i * 0.55f, 0.0f);   /* a column, small gaps */
    }
    for (int f = 0; f < 2000; f++) {
        mote_phys_step(&w, b, NB, 1.0f / 60.0f);
        if (f == 30 || f == 100 || f == 300 || f == 1000) {
            printf("f%-4d:", f);
            for (int i = 0; i < NB; i++) printf(" %.2f", b[i].pos.y);
            printf("\n");
        }
    }

    float base = w.bmin.y + 0.24f;
    int ok = 1;
    printf("expect each box ~0.48 above the one below; floor box ~%.2f\n", base);
    for (int i = 0; i < NB; i++) {
        float gap = (i == 0) ? (b[0].pos.y - base) : (b[i].pos.y - b[i-1].pos.y);
        printf("box%d y=%.3f gap=%.3f\n", i, b[i].pos.y, gap);
        if (i > 0 && gap < 0.40f) ok = 0;            /* interpenetrating */
        if (i == 0 && (gap < -0.05f || gap > 0.10f)) ok = 0;
    }
    printf("RESULT: %s\n", ok ? "5-STACK HOLDS" : "STACK COLLAPSED / INTERPENETRATING");
    return 0;
}
