/* jawcheck — how sharply does the cushion chain turn, and what does it cost?
 *
 * The jaw-to-rail junction reads as a corner when the chain turns through a big
 * angle at one vertex. Spreading that turn over several vertices is the whole
 * point of the blend, so the number that matters is the WORST turn anywhere on
 * the chain, not the vertex count.
 *
 * Build twice, once with -DCUE_JAW_BLEND=0.0f, to compare. */
#include "cue_table.h"
#include "cue_render.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* cue_table.c owns this knob and does not export it, the same way CUE_JAW_SEGS
 * is not exported. Mirror its default so the tool can say what it was built
 * with; override both together on the command line to compare. */
#ifndef CUE_JAW_BLEND
#define CUE_JAW_BLEND 0.30f
#endif

static float turn_deg(Vec3 a, Vec3 b, Vec3 c) {
    float ux = b.x-a.x, uz = b.z-a.z, vx = c.x-b.x, vz = c.z-b.z;
    float lu = sqrtf(ux*ux+uz*uz), lv = sqrtf(vx*vx+vz*vz);
    if (lu < 1e-9f || lv < 1e-9f) return 0.0f;
    float d = (ux*vx + uz*vz) / (lu*lv);
    if (d >  1.0f) d =  1.0f;
    if (d < -1.0f) d = -1.0f;
    return acosf(d) * 57.2957795f;
}

int main(void) {
    static CueTable t; static CueWorld w;
    cue_table_init(&t, CUE_GAME_SNK15);
    cue_table_build_world(&t, &w);
    cue_table_derive_cut(&w);   /* the mesh reads cut_c/cut_r/lip_d */

    /* Worst turn on the chain, and where. Only consecutive segments that
     * actually share a vertex are a junction. */
    float worst = 0.0f; int worst_at = -1; int worst_kinds = 0;
    float worst_j2r = 0.0f;              /* worst across a kind change: jaw<->rail */
    int njoin = 0; double sum = 0.0;
    for (int s = 1; s < w.nseg; s++) {
        if (fabsf(w.seg[s].a.x - w.seg[s-1].b.x) > 1e-6f ||
            fabsf(w.seg[s].a.z - w.seg[s-1].b.z) > 1e-6f) continue;
        float d = turn_deg(w.seg[s-1].a, w.seg[s].a, w.seg[s].b);
        njoin++; sum += d;
        if (d > worst) { worst = d; worst_at = s; worst_kinds = w.seg[s-1].kind*10 + w.seg[s].kind; }
        if (w.seg[s-1].kind != w.seg[s].kind && d > worst_j2r) worst_j2r = d;
    }

    /* And what the mesh costs. The renderer takes its scratch from the caller. */
    cue_render_set_buffers(malloc(cue_render_tab_bytes()),
                           malloc(cue_render_stri_bytes()));
    cue_render_build_table(&t, &w);
    const CueTri *tri = NULL; int bed = 0, lip = 0;
    int ntri = cue_render_table_tris(&tri, &bed, &lip);

    printf("blend      %.2f R\n", (double)CUE_JAW_BLEND);
    printf("segments   %d\n", w.nseg);
    printf("junctions  %d\n", njoin);
    printf("worst turn %.2f deg  (seg %d, kinds %d)\n", worst, worst_at, worst_kinds);
    printf("worst jaw<->rail turn %.2f deg\n", worst_j2r);
    printf("mean turn  %.2f deg\n", njoin ? sum/njoin : 0.0);
    printf("triangles  %d\n", ntri);
    return 0;
}
