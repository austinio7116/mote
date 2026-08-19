/* ThumbyCue pocket bench — one pocket, drawn straight down from above out of
 * the renderer's OWN mesh, with the circle the ball is taken on over the top.
 *
 * THE KNOBS ARE THE GAME'S OWN FIELDS, in the game's own units, so a number
 * dialled here goes back into the source as itself:
 *
 *   pr    cue_table_init  t->pr_corner   / t->pr_side     (xR)  the mouth
 *   gap   cue_table_init  t->gap_corner  / t->gap_side    (xR)  knuckle setback
 *   off   cue_table_init  t->off_corner  / t->off_side    (xR)  pocket centre
 *   back  cue_table_init  t->drop_back / t->drop_back_side (xR) how much
 *                         DEEPER than the pocket the drop circle is centred
 *   capm  cue_table.c build_world: the margin taken off pr to get the drop
 *                         circle — 0.30f, or side_m at a middle        (xR)
 *   set   cue_table_default_cut  CueCut.set                   (m)
 *   rad   cue_table_default_cut  CueCut.rad                         (x pr)
 *   roll  cue_table_default_cut  CueCut.roll                  (x pr)
 *   bore  cue_table_init  t->bore_corner / t->bore_side        (xR)  the hole
 *                         cut in the TIMBER, which is what closes the slot
 *                         between the end of a cushion and the pocket
 *   bset  cue_table_init  t->bore_set_corner / t->bore_set_side (xR)  how far
 *                         OUT that hole is cut from the pocket centre
 *
 * AND IT LOOKS FROM WHERE THE FAULT IS. Straight down is the right view for a
 * mouth and the wrong one for a gap: a slot between the cushion and the frame
 * is edge-on from above and invisible. --view out stands outside the pocket and
 * looks in and down at it; --view in stands inside the table just over the
 * cloth and looks at the back of the pocket; --yaw/--pitch/--dist move the eye
 * from there. The background is MAGENTA in every view, so anything you can see
 * through the table announces itself instead of hiding in a dark corner.
 *
 * Called once per image by the web bench (pocketbench.py).
 */
#include "cue_table.h"
#include "cue_render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { CueGameKind k; const char *name; const char *label; const char *sym; } TB[] = {
    {CUE_GAME_SNK15,"snooker12","snooker 12ft",      "SNK15 + SNK10 (one block)"},
    {CUE_GAME_SNK10,"snooker10","snooker 10ft",      "SNK15 + SNK10 (one block)"},
    {CUE_GAME_UK8,  "uk7",      "UK8 7ft (+ 6-red)", "UK8 + SNK6 (one block)"},
    {CUE_GAME_US8,  "us9",      "US8 9ft (+ 9-ball)","US8 + US9 (one block)"},
    {CUE_GAME_CN8,  "chinese10","Chinese 8 10ft",    "CN8"},
    /* SNK6 and US9 share a pocket block with uk7/us9 above, so the bench never
     * needed them — but they are their own kinds with their own beds and racks,
     * and a sweep of "every table" that silently omits two is not one. */
    {CUE_GAME_SNK6, "snk6",     "6-red snooker 7ft", "UK8 + SNK6 (one block)"},
    {CUE_GAME_US9,  "us9ball",  "9-ball 9ft",        "US8 + US9 (one block)"},
    /* Same bed and same pocket block as US8 — 14.1 is a rules game, not a table
     * game — but it racks fifteen and reracks fourteen, which is the only thing
     * about it there is to look at. */
    {CUE_GAME_STRAIGHT,"straight","straight pool 9ft","US8 + US9 (one block)"},
    /* THE ONE THE BENCH IS FOR. Its mouth is 1.07 ball radii where every other
     * table here is between 1.8 and 2.2, so every pocket number written as a
     * multiple of the BALL comes out roughly twice the size of the hole it is
     * shaping — and the corner is where that shows, because two facings come at
     * each other there and a long one makes them meet before the pocket does. */
    {CUE_GAME_PYRAMID, "pyramid", "russian pyramid 12ft", "PYRA"},
};
/* COUNTED, NOT WRITTEN DOWN. This was a literal 8 against a list of nine, so
 * the last table in it — russian pyramid, the one the bench was extended FOR —
 * could not be selected and did not appear in the "known:" list either. */
#define NT ((int)(sizeof TB / sizeof TB[0]))


static CueTable T; static CueWorld W;

/* The mouth a ball actually goes through: the two knuckles beside the pocket
 * are circles, so it is their centre distance less both radii. */
static float jaw_sep(const CueWorld *w, int p) {
    int j1=-1,j2=-1; float d1=1e30f,d2=1e30f;
    for (int j=0;j<w->njaw;j++){
        float dx=w->jaw[j].x-w->pocket[p].x, dz=w->jaw[j].z-w->pocket[p].z;
        float dd=dx*dx+dz*dz;
        if(dd<d1){d2=d1;j2=j1;d1=dd;j1=j;} else if(dd<d2){d2=dd;j2=j;}
    }
    if(j1<0||j2<0) return 0.0f;
    float dx=w->jaw[j1].x-w->jaw[j2].x, dz=w->jaw[j1].z-w->jaw[j2].z;
    float s = sqrtf(dx*dx+dz*dz) - 2.0f*w->jaw_r;
    return s > 0.0f ? s : 0.0f;
}

/* THE CLEARANCE BETWEEN THE EDGE OF THE BORE AND THE NEAREST RUBBER — the
 * number behind "the cushions should be laid out to touch the bore".
 *
 * The whole facing is measured, not its endpoints: the nearest point of every
 * kind-1 segment (pocket facing / jaw) belonging to this pocket, against the
 * bore circle. On a UK corner that nearest point turns out to be the knuckle
 * and it sits about 31mm outside the hole — which is what the pocket is, cloth
 * then lip then bore, and NOT a fault on its own.
 *
 * What the number is for is watching it while the pocket size is dialled:
 * ZERO is the cushions arriving exactly on the hole, and NEGATIVE is the bore
 * eaten in past the end of the rubber, which is the slot you can see out of
 * the table through in the OUTSIDE and INSIDE views. */
static float bore_clearance(const CueWorld *w, int p, float br, float bset) {
    const float bx = w->pocket[p].x + w->pmnorm[p].x*bset;
    const float bz = w->pocket[p].z + w->pmnorm[p].z*bset;
    float near = 1e30f;
    for (int i = 0; i < w->nseg; i++) {
        if (w->seg[i].kind != 1) continue;
        /* Facings belong to the pocket they are nearest — no index arithmetic,
         * which an L-shaped table would break anyway. */
        const Vec3 a = w->seg[i].a, b = w->seg[i].b;
        const float mx = 0.5f*(a.x+b.x) - w->pocket[p].x;
        const float mz = 0.5f*(a.z+b.z) - w->pocket[p].z;
        if (mx*mx + mz*mz > 0.35f*0.35f) continue;
        for (int q = 0; q <= 8; q++) {          /* along it, not just its ends */
            const float t = (float)q / 8.0f;
            const float ex = a.x + (b.x-a.x)*t - bx, ez = a.z + (b.z-a.z)*t - bz;
            const float d = sqrtf(ex*ex + ez*ez);
            if (d < near) near = d;
        }
    }
    /* NOT zero when there is nothing to measure. Zero is the ANSWER here —
     * the cushions landing exactly on the hole — so returning it for "this
     * build has no facings" makes a degenerate pocket read as a solved one,
     * which is how the first cut of the solver quietly gave up on four of the
     * eighteen and reported the authored gap as if it had converged. */
    if (near > 1e29f) return NAN;
    return near - w->jaw_r - br;      /* the rubber's surface, not its centreline */
}


typedef struct { float pr, gap, off, capm, back, set, rad, roll, bore, bset,
                       flen, ang,
                       /* THE THINGS THAT ARE NOT THE POCKET but decide what it
                        * meets. A pocket is where the cushion, the timber and
                        * the cloth arrive at the same place, and three of the
                        * four numbers that put them there were not on the
                        * bench at all — so a gap could be dialled away in the
                        * pocket's own terms while the thing actually causing it
                        * sat off-screen. */
                       jaw, rail, cush, ball;
                       int round_; } Knobs;

/* Built exactly the way the game builds it, with these numbers in place of the
 * shipped ones. Nothing here is a bench-only derivation: pr/gap/off go into
 * CueTable before build_world, capm reproduces build_world's own margin, and
 * set/rad/roll go through cue_table_derive_cut. */
static void build_tuned(int ti, int mid, const Knobs *k, CueTable *ot, CueWorld *ow) {
    CueTable t; cue_table_init(&t, TB[ti].k);
    /* THE BALL AND THE POCKET STYLE FIRST, because everything below is a
     * multiple of R and the jaw construction is chosen by pocket_round. */
    if (k->ball  > 0.0f) t.R = k->ball;
    if (k->round_ >= 0)  t.pocket_round = k->round_;
    if (k->jaw  >= 0.0f) t.jaw_r     = k->jaw * t.R;
    if (k->rail >  0.0f) t.rail_w    = k->rail;
    if (k->cush >  0.0f) t.cushion_h = k->cush * t.R;
    /* FACING LENGTH AND SPLAY, which the bench did not have and which are the
     * two that shape a MITRED jaw. On a rounded table the jaw is a bezier and
     * neither is read, so they were never missed; on a mitred one they decide
     * where the facing's tip lands, and a tip past the pocket circle is a
     * cushion standing proud of its own hole. facing_len is ONE field for both
     * pockets — the game has no separate corner and middle length — so the
     * slider moves both whichever type is being looked at. */
    t.facing_len = k->flen*t.R;
    if (mid) { t.pr_side   = k->pr*t.R;  t.gap_side   = k->gap*t.R;  t.off_side   = k->off*t.R;
               t.drop_back_side = k->back*t.R; t.cap_side = k->capm*t.R;
               t.bore_side  = k->bore*t.R; t.bore_set_side = k->bset*t.R;
               t.ang_side = k->ang; }
    else     { t.pr_corner = k->pr*t.R;  t.gap_corner = k->gap*t.R;  t.off_corner = k->off*t.R;
               t.drop_back      = k->back*t.R; t.cap_corner = k->capm*t.R;
               t.bore_corner = k->bore*t.R; t.bore_set_corner = k->bset*t.R;
               t.ang_corner = k->ang; }
    cue_table_build_world(&t, ow);
    int i = mid ? 1 : 0;
    ow->cut_set[i] = k->set; ow->cut_rad[i] = k->rad; ow->cut_roll[i] = k->roll;
    ow->cut_ref[i] = mid ? t.pr_side : t.pr_corner;
    cue_table_derive_cut(ow);
    *ot = t;
}

/* WHERE THE CUSHIONS HAVE TO END SO THEY KISS THE BORE.
 *
 * The pocket size is the drop radius and the bore is the same circle, and the
 * cushions are not free to sit wherever they were authored: they have to
 * arrive exactly on that circle. A slot between the rubber and the hole is a
 * fault you can see out of the table through, and rubber overhanging the hole
 * is a fault too — it is unsupported, there is no timber under it. The target
 * is neither. It is ZERO.
 *
 * `gap` is the knuckle setback and it is what moves the end of a cushion, so
 * this solves for it rather than asking an author to dial it. Bisection, not
 * a formula: the jaw is a bezier whose control points are struck off the ball
 * radius and the clearance is the nearest point along the whole facing, so
 * there is no closed form to write down and pretend to trust. Rebuilding the
 * world is the only honest evaluation of it, and forty of them is nothing.
 *
 * Monotonic in `gap` over any range worth solving on — pulling the knuckles
 * back can only move the rubber away from the hole — so bisection converges
 * on the one crossing. If the bracket does not contain one, the authored gap
 * is returned untouched and the readout goes on reporting the error, because
 * quietly clamping to an end of the range would look like a solution. */
static float solve_gap(int ti, int mid, const Knobs *k0, int pidx) {
    Knobs k = *k0;
    CueTable t; CueWorld w;
    #define CLEAR_AT(G) (k.gap = (G), build_tuned(ti, mid, &k, &t, &w), \
        bore_clearance(&w, (pidx >= 0 && pidx < w.npocket) ? pidx : (mid ? 5 : 2), \
                       mid ? t.bore_side : t.bore_corner, \
                       mid ? t.bore_set_side : t.bore_set_corner))
    /* WALK IT, then bisect. A fixed bracket is not safe: past a certain
     * setback the pocket stops being a pocket and the facings disappear
     * entirely, and an endpoint in that region tells the solver nothing. So
     * step along the range, ignore any build that has no facings to measure,
     * and bisect the FIRST sign change found among the ones that do. */
    const int N = 48;
    const float g0 = 0.05f, g1 = 8.0f;
    float plo = 0.0f, pc = 0.0f; int have = 0;
    for (int i = 0; i <= N; i++) {
        const float g = g0 + (g1 - g0) * (float)i / (float)N;
        const float c = CLEAR_AT(g);
        if (c != c) { have = 0; continue; }              /* NaN: nothing there */
        if (have && ((pc > 0.0f) != (c > 0.0f))) {
            float lo = plo, hi = g, clo = pc;
            for (int j = 0; j < 40; j++) {
                const float m = 0.5f*(lo + hi), cm = CLEAR_AT(m);
                if (cm != cm) { hi = m; continue; }
                if ((cm > 0.0f) == (clo > 0.0f)) { lo = m; clo = cm; } else hi = m;
            }
            #undef CLEAR_AT
            return 0.5f*(lo + hi);
        }
        plo = g; pc = c; have = 1;
    }
    #undef CLEAR_AT
    return k0->gap;        /* no crossing: leave it alone and let the readout say so */
}


static int IW = 700, IH = 700;
static unsigned char *img;
static float *zb;
static float ox, oz, spm;

/* WHERE THE EYE IS.
 *
 * The bench was orthographic and straight down, which is the correct view of a
 * pocket mouth and a useless one for a gap: a slot between the end of a cushion
 * and the frame is edge-on from up there. So there is a real camera as well —
 * eye, basis, focal length — and the top view is just the case that keeps the
 * old projection. Depth is the eye-space distance and it is interpolated as
 * 1/z, because in a perspective view a linear one bends a big triangle through
 * its neighbours. */
static int   s_persp = 0;
static Vec3  s_eye, s_rt, s_up, s_fw;
static float s_focal;

static Vec3 v3n(Vec3 a){ float l=sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-9f?v3(a.x/l,a.y/l,a.z/l):a; }
static Vec3 v3x(Vec3 a, Vec3 b){ return v3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static float v3d(Vec3 a, Vec3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }

static void cam_look(Vec3 target, float yaw_deg, float pitch_deg, float dist, float fov_deg) {
    float y = yaw_deg*3.14159265f/180.0f, p = pitch_deg*3.14159265f/180.0f;
    Vec3 dir = v3(sinf(y)*cosf(p), sinf(p), cosf(y)*cosf(p));   /* eye offset */
    s_eye = v3(target.x + dir.x*dist, target.y + dir.y*dist, target.z + dir.z*dist);
    s_fw  = v3n(v3(target.x-s_eye.x, target.y-s_eye.y, target.z-s_eye.z));
    s_rt  = v3n(v3x(s_fw, v3(0,1,0)));
    s_up  = v3x(s_rt, s_fw);
    s_focal = (IW*0.5f) / tanf(fov_deg*0.5f*3.14159265f/180.0f);
    s_persp = 1;
}

/* Screen position and depth of a world point. Depth is what the z-buffer
 * compares: bigger is nearer, so the top view keeps handing back the height it
 * always did and the perspective one hands back -distance. */
static int proj(Vec3 v, float *px, float *py, float *depth) {
    if (!s_persp) { *px=(v.x-ox)*spm+IW*0.5f; *py=(v.z-oz)*spm+IH*0.5f; *depth=v.y; return 1; }
    Vec3 d = v3(v.x-s_eye.x, v.y-s_eye.y, v.z-s_eye.z);
    float z = v3d(d, s_fw);
    if (z < 0.004f) return 0;                       /* behind the eye */
    *px = IW*0.5f + s_focal * v3d(d, s_rt) / z;
    *py = IH*0.5f - s_focal * v3d(d, s_up) / z;
    *depth = -z;
    return 1;
}
/* CLIPPED, not rejected.
 *
 * A triangle with a vertex behind the eye used to be thrown away whole. The
 * cloth bed is a fan from the CENTRE of the table, so standing inside the table
 * and looking at a rail threw away every bed triangle in the picture and the
 * view came back with no cloth in it at all — which reads as a hole in the
 * table, which is exactly the thing this tool is for finding. So the polygon is
 * cut on the near plane and what is in front of the eye is kept. */
#define PK_NEAR 0.004f
static Vec3 eyespace(Vec3 v) {
    Vec3 d = v3(v.x-s_eye.x, v.y-s_eye.y, v.z-s_eye.z);
    return v3(v3d(d, s_rt), v3d(d, s_up), v3d(d, s_fw));   /* x right, y up, z fwd */
}
static int clip_near(const Vec3 *in, int n, Vec3 *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vec3 a = in[i], b = in[(i+1)%n];
        int ain = a.z >= PK_NEAR, bin = b.z >= PK_NEAR;
        if (ain) out[m++] = a;
        if (ain != bin) {
            float t = (PK_NEAR - a.z) / (b.z - a.z);
            out[m++] = v3(a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, PK_NEAR);
        }
    }
    return m;
}
static void m2px(float x,float z,float*px,float*py){
    float d; Vec3 v = v3(x, s_persp ? 0.0005f : 0.0f, z); proj(v, px, py, &d); }
static void put(int x,int y,int r,int g,int b){ if(x<0||y<0||x>=IW||y>=IH)return;
    unsigned char*q=img+((size_t)y*IW+x)*3; q[0]=r;q[1]=g;q[2]=b; }
static void dot(float fx,float fy,int rad,int r,int g,int b){
    for(int dy=-rad;dy<=rad;dy++)for(int dx=-rad;dx<=rad;dx++)
        if(dx*dx+dy*dy<=rad*rad) put((int)(fx+dx),(int)(fy+dy),r,g,b); }
static void circ_m(float cx,float cz,float rm,int r,int g,int b,int th,int dash){
    if(rm<=0) return;
    int n=(int)(rm*spm*8.0f)+360; if(n>20000)n=20000;
    for(int i=0;i<n;i++){
        if(dash && ((i*24/n)&1)) continue;
        float a=i*6.2831853f/n, p,q;
        m2px(cx+rm*cosf(a),cz+rm*sinf(a),&p,&q); dot(p,q,th,r,g,b); }
}

/* The light the oblique views shade against. Flat lambert, one direction: this
 * is a diagnostic view, not a picture. */
static const Vec3 LIT = { 0.32f, 0.90f, 0.28f };

/* One triangle, clipped, shaded and z-buffered. Pulled out of the loop so
 * the bench's own floor quad goes through exactly the same path. */
static void draw_tri(const CueTri *q) {
        int R=(q->color>>11&31)*255/31, G=(q->color>>5&63)*255/63, B=(q->color&31)*255/31;
        /* Flat lambert in the oblique views. The top view keeps the authored
         * colour: it is read against the overlay circles, not looked at. */
        if (s_persp) {
            float sh = 0.34f + 0.66f * fabsf(v3d(q->nrm, LIT));
            R=(int)(R*sh); G=(int)(G*sh); B=(int)(B*sh);
            if(R>255)R=255; if(G>255)G=255; if(B>255)B=255;
        }
        /* Screen positions and depths, however many corners survive the clip. */
        float sx[8], sy[8], sd[8]; int np = 0;
        if (s_persp) {
            Vec3 e[3] = { eyespace(q->v[0]), eyespace(q->v[1]), eyespace(q->v[2]) };
            Vec3 cp[8]; int nc = clip_near(e, 3, cp);
            if (nc < 3) return;
            for (int i = 0; i < nc && i < 8; i++, np++) {
                sx[np] = IW*0.5f + s_focal * cp[i].x / cp[i].z;
                sy[np] = IH*0.5f - s_focal * cp[i].y / cp[i].z;
                sd[np] = -cp[i].z;
            }
        } else {
            for (int i = 0; i < 3; i++, np++) {
                sx[np] = (q->v[i].x-ox)*spm + IW*0.5f;
                sy[np] = (q->v[i].z-oz)*spm + IH*0.5f;
                sd[np] = q->v[i].y;
            }
        }
        for (int f = 2; f < np; f++) {
            float px0=sx[0],py0=sy[0],d0=sd[0];
            float px1=sx[f-1],py1=sy[f-1],d1=sd[f-1];
            float px2=sx[f],py2=sy[f],d2=sd[f];
            int lx=(int)floorf(fminf(px0,fminf(px1,px2))), hx=(int)ceilf(fmaxf(px0,fmaxf(px1,px2)));
            int ly=(int)floorf(fminf(py0,fminf(py1,py2))), hy=(int)ceilf(fmaxf(py0,fmaxf(py1,py2)));
            if(hx<0||hy<0||lx>=IW||ly>=IH) continue;
            if(lx<0)lx=0; if(ly<0)ly=0; if(hx>=IW)hx=IW-1; if(hy>=IH)hy=IH-1;
            float dd=(py1-py2)*(px0-px2)+(px2-px1)*(py0-py2); if(fabsf(dd)<1e-9f) continue;
            for(int y=ly;y<=hy;y++)for(int x=lx;x<=hx;x++){
                float fx=x+0.5f, fy=y+0.5f;
                float w0=((py1-py2)*(fx-px2)+(px2-px1)*(fy-py2))/dd;
                float w1=((py2-py0)*(fx-px2)+(px0-px2)*(fy-py2))/dd;
                float w2=1.0f-w0-w1;
                if(w0<0||w1<0||w2<0) continue;
                float hyt;
                if (s_persp) hyt = -1.0f/(w0/(-d0) + w1/(-d1) + w2/(-d2));
                else         hyt = w0*d0 + w1*d1 + w2*d2;
                size_t o=(size_t)y*IW+x;
                if(hyt<=zb[o]) continue;
                zb[o]=hyt; unsigned char*c=img+o*3; c[0]=R;c[1]=G;c[2]=B;
            }
        }
}

/* A BALL, as the game would wear it.
 *
 * Tessellated here rather than taken from cue_render, because the handheld
 * draws balls as textured spheres through the engine's scene layer and this
 * tool has no engine. The SURFACE is the real one though: cue_render_ball_texel
 * is the same function the device shades every ball with, asked per facet, so
 * the stripes, the spots and the blacks are the authored ones and not an
 * approximation of them. */
static void draw_ball(const CueBall *b, float R) {
    const int SL = 22, ST = 12;
    if (!b->on) return;
    float br = (b->r > 0.0f) ? b->r : R;
    for (int i = 0; i < ST; i++) {
        float t0 = 3.14159265f * i / ST, t1 = 3.14159265f * (i + 1) / ST;
        for (int j = 0; j < SL; j++) {
            float p0 = 6.2831853f * j / SL, p1 = 6.2831853f * (j + 1) / SL;
            Vec3 n[4] = {
                v3(sinf(t0)*cosf(p0), cosf(t0), sinf(t0)*sinf(p0)),
                v3(sinf(t0)*cosf(p1), cosf(t0), sinf(t0)*sinf(p1)),
                v3(sinf(t1)*cosf(p1), cosf(t1), sinf(t1)*sinf(p1)),
                v3(sinf(t1)*cosf(p0), cosf(t1), sinf(t1)*sinf(p0)),
            };
            Vec3 v[4];
            for (int k = 0; k < 4; k++)
                v[k] = v3(b->pos.x + n[k].x*br, b->pos.y + n[k].y*br, b->pos.z + n[k].z*br);
            /* The facet's own outward direction, in BALL space, is what the
             * surface function is asked about — so a ball that has rolled shows
             * its markings where they actually ended up. */
            Vec3 c = v3n(v3(n[0].x+n[2].x, n[0].y+n[2].y, n[0].z+n[2].z));
            Vec3 lb = v3(v3d(c, b->orient.r[0]), v3d(c, b->orient.r[1]), v3d(c, b->orient.r[2]));
            uint16_t col = cue_render_ball_texel(b->id, lb);
            CueTri q0 = { { v[0], v[1], v[2] }, c, col, CUE_MAT_WOOD };
            CueTri q1 = { { v[0], v[2], v[3] }, c, col, CUE_MAT_WOOD };
            draw_tri(&q0); draw_tri(&q1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--defaults")) {
        printf("{\n");
        for (int i = 0; i < NT; i++) {
            cue_table_init(&T, TB[i].k); cue_table_build_world(&T, &W);
            printf("  \"%s\": {\"label\": \"%s\", \"sym\": \"%s\", \"ball\": %.4f",
                   TB[i].name, TB[i].label, TB[i].sym, (double)(W.R*1000.0f));
            for (int m = 0; m < 2; m++) {
                int p = m ? 5 : 2;
                CueCut c; cue_table_get_cut(&W, m, &c);
                printf(", \"%s\": {\"pr\": %.4f, \"gap\": %.4f, \"off\": %.4f, "
                       "\"capm\": %.4f, \"back\": %.4f, "
                       "\"set\": %.5f, \"rad\": %.4f, \"roll\": %.4f, "
                       "\"bore\": %.4f, \"bset\": %.4f, "
                       "\"flen\": %.4f, \"ang\": %.2f, "
                       "\"jaw\": %.4f, \"rail\": %.4f, \"cush\": %.4f, "
                       "\"ball\": %.4f, \"round\": %d}",
                    m?"middle":"corner",
                    (double)((m ? T.pr_side  : T.pr_corner ) / T.R),
                    (double)((m ? T.gap_side : T.gap_corner) / T.R),
                    (double)((m ? T.off_side : T.off_corner) / T.R),
                    (double)((m ? T.cap_side : T.cap_corner) / T.R),
                    (double)((m ? T.drop_back_side : T.drop_back) / T.R),
                    (double)c.set, (double)c.rad, (double)c.roll,
                    /* A knob the page does not receive is a knob the page
                     * cannot draw — and the paste-back block asks for every
                     * key by name, so a missing one takes the whole page
                     * down. */
                    (double)((m ? T.bore_side : T.bore_corner) / T.R),
                    (double)((m ? T.bore_set_side : T.bore_set_corner) / T.R),
                    (double)(T.facing_len / T.R),
                    (double)(m ? T.ang_side : T.ang_corner),
                    (double)(T.jaw_r / T.R), (double)T.rail_w,
                    (double)(T.cushion_h / T.R), (double)T.R,
                    T.pocket_round);
            }
            printf("}%s\n", i+1<NT ? "," : "");
        }
        printf("}\n");
        return 0;
    }

    const char *tbl="snooker12", *ty="corner", *out="/tmp/pk.ppm";
    int pocket_idx = -1;
    int kiss = 0;   /* solve `gap` so the cushions land on the bore */
    /* WHERE THE EYE GOES. "top" is the old straight-down view; "out" stands
     * outside the pocket looking in and down at it, which is where a slot
     * behind a mitred jaw shows; "in" stands inside the table just over the
     * cloth looking at the back of the pocket, which is where the middle-pocket
     * one shows. --yaw/--pitch/--dist take over from there. */
    const char *vw="top"; float cyaw=0, cpit=0, cdst=0; int have_cam=0;
    const char *lay=NULL;
    int whole=0, rack=0, bset=-1;
    float notch_x=0.0f, notch_z=0.0f, bed_l=0.0f, bed_w=0.0f;
    int   ngon_n=0, ngon_e=1;
    /* EVERY knob starts unset. A field added to Knobs without a -1 here reads
     * as ZERO, which for the bore meant a rail with no hole cut in it — the
     * plank closed straight over the pocket and the bore wall vanished. */
    /* Every knob unset. bset alone uses -99 because a real setback may legally
     * be negative; the rest cannot be, so -1 says "not given". round_ is an
     * int and takes -1 for the same reason. */
    Knobs k = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-99, -1,-1, -1,-1,-1,-1, -1};
    float zoom=5.0f;
    for (int i=1;i<argc-1;i++){
        if(!strcmp(argv[i],"--table")) tbl=argv[++i];
        else if(!strcmp(argv[i],"--type")) ty=argv[++i];
        else if(!strcmp(argv[i],"--pocket")) pocket_idx=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--out"))  out=argv[++i];
        else if(!strcmp(argv[i],"--pr"))   k.pr  =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--gap"))  k.gap =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--off"))  k.off =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--capm")) k.capm=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--back")) k.back=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--set"))  k.set =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--rad"))  k.rad =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--roll")) k.roll=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--zoom")) zoom=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--size")) { IW=IH=atoi(argv[++i]); }
        else if(!strcmp(argv[i],"--kiss")) kiss=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--bore")) k.bore=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--bset")) k.bset=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--flen")) k.flen=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--ang"))  k.ang =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--jaw"))  k.jaw =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--rail")) k.rail=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--cush")) k.cush=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--ball")) k.ball=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--round")) k.round_=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--view")) vw=argv[++i];
        else if(!strcmp(argv[i],"--layer")) lay=argv[++i];
        else if(!strcmp(argv[i],"--whole")) whole=1;
        else if(!strcmp(argv[i],"--rack"))  rack=1;
        else if(!strcmp(argv[i],"--rerack")) rack=2;   /* 14.1: the apex left empty */
        /* THE L IS A SHAPE, NOT A TABLE. Applied to whichever table is being
         * drawn, the same way a custom table will carry it — so the bench can
         * look at an L-shaped snooker table or an L-shaped 7 ft without either
         * having to become a game mode. */
        else if(!strcmp(argv[i],"--notch")) { notch_x=atof(argv[++i]); notch_z=atof(argv[++i]); }
        /* An L wants a squarish bed under it — see the note in cuevr_app.c's
         * apply_bed_shape. Half-extents, in metres. */
        else if(!strcmp(argv[i],"--bed")) { bed_l=atof(argv[++i]); bed_w=atof(argv[++i]); }
        /* S2: A REGULAR BED IS A SHAPE TOO. --ngon 6 for a hexagon, --ngon 60 10
         * for a round one. Applied the same way the notch is, to whichever
         * table is being drawn: the pocket numbers under test belong to the
         * GAME, and what this asks is how they behave on a bed with no
         * rectangle in it. */
        else if(!strcmp(argv[i],"--ngon")) { ngon_n=atoi(argv[++i]);
                                             ngon_e=(i+1<argc && argv[i+1][0]!='-')
                                                    ? atoi(argv[++i]) : 1; }
        else if(!strcmp(argv[i],"--ballset")&&i+1<argc) bset=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--yaw"))  { cyaw=(float)atof(argv[++i]); have_cam=1; }
        else if(!strcmp(argv[i],"--pitch")){ cpit=(float)atof(argv[++i]); have_cam=1; }
        else if(!strcmp(argv[i],"--dist")) { cdst=(float)atof(argv[++i]); have_cam=1; }
    }
    /* An unknown name used to fall back to table 0 and render it without
     * comment, which hands back a plausible picture of the WRONG table — the
     * one thing a tool for looking at geometry must never do. */
    int ti=-1; for(int i=0;i<NT;i++) if(!strcmp(tbl,TB[i].name)) ti=i;
    if (ti < 0) {
        fprintf(stderr, "unknown --table \"%s\". known:", tbl);
        for(int i=0;i<NT;i++) fprintf(stderr, " %s", TB[i].name);
        fprintf(stderr, "\n");
        return 2;
    }
    int mid = (ty[0]=='m');
    /* anything not given falls back to what the game ships */
    {   CueTable t0; cue_table_init(&t0, TB[ti].k);
        CueWorld w0; cue_table_build_world(&t0, &w0);
        CueCut c0;   cue_table_get_cut(&w0, mid, &c0);
        if(k.pr  <0) k.pr  = (mid?t0.pr_side :t0.pr_corner )/t0.R;
        if(k.gap <0) k.gap = (mid?t0.gap_side:t0.gap_corner)/t0.R;
        if(k.off <0) k.off = (mid?t0.off_side:t0.off_corner)/t0.R;
        if(k.capm<0) k.capm= (mid?t0.cap_side:t0.cap_corner)/t0.R;
        if(k.back<0) k.back= (mid?t0.drop_back_side:t0.drop_back)/t0.R;
        if(k.set <0) k.set = c0.set;
        if(k.rad <0) k.rad = c0.rad;
        if(k.roll<0) k.roll= c0.roll;
        if(k.bore<0) k.bore= (mid?t0.bore_side:t0.bore_corner)/t0.R;
        if(k.flen<0) k.flen= t0.facing_len/t0.R;
        if(k.jaw <0) k.jaw = t0.jaw_r/t0.R;
        if(k.rail<0) k.rail= t0.rail_w;
        if(k.cush<0) k.cush= t0.cushion_h/t0.R;
        if(k.ball<0) k.ball= t0.R;
        if(k.round_<0) k.round_= t0.pocket_round;
        if(k.ang <0) k.ang = (mid?t0.ang_side:t0.ang_corner);
        /* Setback CAN be negative — pulling the hole in toward the cloth is a
         * legitimate direction — so it is flagged unset with a sentinel that a
         * real value could never be rather than by its sign. */
        if(k.bset<-90.0f) k.bset= (mid?t0.bore_set_side:t0.bore_set_corner)/t0.R;
    }
    /* THE CUSHIONS FOLLOW THE HOLE, not the other way round. Solved before
     * the world is built, so what is drawn and what is measured are the same
     * table — the alternative is building it twice and reporting the first. */
    float gap_solved = -1.0f;
    if (kiss) { gap_solved = solve_gap(ti, mid, &k, pocket_idx); k.gap = gap_solved; }
    build_tuned(ti, mid, &k, &T, &W);
    if (bed_l > 0.0f && bed_w > 0.0f) {
        T.half_len = bed_l; T.half_wid = bed_w;
        cue_table_build_world(&T, &W);
    }
    if (ngon_n >= 3) {
        /* Applied to the finished table and the world rebuilt from it, exactly
         * as the notch is below and as a custom table does. The markings go
         * onto the APOTHEM — a regular bed's corners are pockets, so the
         * half-length that means anything is the distance to a cushion. */
        float ca = cosf(3.14159265f / (float)ngon_n);
        T.bed_shape = CUE_BED_NGON;
        T.bed_sides = ngon_n;
        T.bed_pocket_every = ngon_e > 0 ? ngon_e : 1;
        T.half_wid = T.half_len;
        T.notch_x = T.notch_z = 0.0f;
        T.baulk_x *= ca; T.blue_x *= ca; T.pink_x *= ca; T.black_x *= ca;
        T.d_radius *= ca;
        cue_table_build_world(&T, &W);
    }
    if (notch_x > 0.0f && notch_z > 0.0f) {
        /* Applied to the finished table and the world rebuilt from it — the
         * same order a custom table takes, so what the bench draws is what the
         * game would build. */
        T.bed_shape = CUE_BED_L;
        T.notch_x = notch_x * T.half_len;
        T.notch_z = notch_z * T.half_wid;
        cue_table_build_world(&T, &W);
    }

    img=malloc((size_t)IW*IH*3); zb=malloc(sizeof(float)*(size_t)IW*IH);
    cue_render_set_buffers(malloc(cue_render_tab_bytes()), malloc(cue_render_stri_bytes()));
    cue_render_build_table(&T,&W);
    const CueTri *tri; int bd=0,lp=0; int ntri=cue_render_table_tris(&tri,&bd,&lp);

    /* WHICH POCKET. On a rectangle the two named types are enough — every
     * corner is the same corner and every middle the same middle — so the
     * bench picked 2 and 5 and never needed to say which.
     *
     * An L breaks that. Its seven are not interchangeable: the two beside the
     * notch have the missing corner as their outside, the reflex has no pocket
     * at all, and the two middles sit on rails of different lengths. Dialling
     * them means being able to LOOK at each one, so the index is selectable and
     * the type is only the fallback. */
    int p = (pocket_idx >= 0 && pocket_idx < W.npocket) ? pocket_idx
          : (mid ? 5 : 2);
    if (p >= W.npocket) p = 0;
    mid = W.pocket_mid[p];        /* the chosen pocket's own kind, not the flag */
    ox=W.pocket[p].x; oz=W.pocket[p].z;
    float span = zoom*T.pr_corner; spm = IW/span;
    ox -= W.pmnorm[p].x*span*0.16f; oz -= W.pmnorm[p].z*span*0.16f;
    if (strcmp(vw, "top")) {
        /* Pocket 2 is the +x/+z corner and pocket 5 the +z middle, so "out" is
         * the diagonal at a corner and straight out at a middle. The defaults
         * are the angle the fault was actually seen from; the sliders move off
         * it. */
        float yaw = have_cam ? cyaw : (!strcmp(vw,"in") ? (mid ? 180.0f : 225.0f)
                                                        : (mid ?   0.0f :  45.0f));
        float pit = have_cam ? cpit : (!strcmp(vw,"in") ? 8.0f : 35.0f);
        float dst = have_cam ? cdst : (!strcmp(vw,"in") ? T.R*9.0f : T.R*12.0f);
        cam_look(v3(W.pocket[p].x, T.R*0.35f, W.pocket[p].z), yaw, pit, dst, 55.0f);
    }
    /* THE WHOLE TABLE, rather than one pocket. Same camera, framed on the
     * cloth's centre and far enough back to hold the long rail — so a change to
     * the bed can be looked at as a table instead of inferred from a corner. */
    if (whole) {
        ox = oz = 0.0f;
        spm = IW / (T.half_len * 2.35f);
        if (strcmp(vw, "top")) {
            /* Per-parameter defaults, NOT all-or-nothing. have_cam is set by any
             * one of --yaw/--pitch/--dist, so gating the distance on it puts the
             * camera at the origin — inside the table, looking at nothing —
             * whenever an angle is given without one. */
            float yaw = have_cam ? cyaw : 208.0f;
            float pit = have_cam ? cpit : 30.0f;
            float dst = (cdst > 0.0f) ? cdst : T.half_len * 2.1f;
            cam_look(v3(0.0f, 0.0f, 0.0f), yaw, pit, dst, 55.0f);
        }
    }
    /* MAGENTA in the oblique views, everywhere the table is not: a hole in the
     * mesh is the whole point of them, and a dark hole in a dark corner is
     * invisible. The view from above keeps its dark ground — it is read against
     * the overlay circles, and a magenta disc in the mouth fights them. */
    if (s_persp) { for (size_t i = 0; i < (size_t)IW*IH; i++)
                       { img[i*3]=255; img[i*3+1]=0; img[i*3+2]=255; } }
    else memset(img,12,(size_t)IW*IH*3);
    for(size_t i=0;i<(size_t)IW*IH;i++) zb[i]=-1e9f;
    /* --layer bed|mid|lip draws one band of the mesh on its own. The bands are
     * the renderer's own draw order: flat cloth, everything raised, then the
     * drop lips. Isolating them is how you tell "the face is not being drawn"
     * from "the face is not there". */
    int t_lo = 0, t_hi = ntri;
    if (lay && !strcmp(lay,"bed")) { t_lo = 0;  t_hi = bd; }
    else if (lay && !strcmp(lay,"mid")) { t_lo = bd; t_hi = lp; }
    else if (lay && !strcmp(lay,"lip")) { t_lo = lp; t_hi = ntri; }
    /* A FLOOR UNDER THE TABLE, so magenta means what it says.
     *
     * cue_render's mesh has no bottom to its pockets — in the game a tray under
     * the frame is that floor — so looking down a pocket showed the background
     * and every pocket read as a hole clean through the table. With a floor
     * laid under it, down a pocket is DARK and the only magenta left is a line
     * of sight that really does leave the table sideways: the slot between a
     * cushion and the frame, which is the thing being hunted. */
    if (s_persp) {
        const float fy = -0.09f, e = 3.0f;
        Vec3 fa = v3(-e,fy,-e), fb = v3(e,fy,-e), fc = v3(e,fy,e), fd = v3(-e,fy,e);
        CueTri fl[2];
        fl[0].v[0]=fa; fl[0].v[1]=fb; fl[0].v[2]=fc;
        fl[1].v[0]=fa; fl[1].v[1]=fc; fl[1].v[2]=fd;
        for (int i=0;i<2;i++){ fl[i].nrm=v3(0,1,0); fl[i].color=(uint16_t)(((26>>3)<<11)|((26>>2)<<5)|(30>>3));  /* dark grey */ fl[i].mat=0; }
        for (int i=0;i<2;i++) draw_tri(&fl[i]);
    }
    for(int t=t_lo;t<t_hi;t++) draw_tri(&tri[t]);
    /* ...and the balls on top of it, if asked. */
    if (rack) {
        CueBall bl[CUE_MAX_BALLS];
        int nb = cue_table_rack(&T, bl);
        if (rack == 2) {
            /* THE 14.1 RERACK. Take fourteen off, leave one out on the table as
             * the break ball, and ask for the triangle back: the apex gap is
             * the whole point of the picture. */
            for (int i = 1; i < nb; i++)
                if (bl[i].id >= 1 && bl[i].id <= 14) bl[i].on = 0;
            for (int i = 1; i < nb; i++)
                if (bl[i].id == 15) bl[i].pos = v3(T.baulk_x + T.half_len * 0.35f,
                                                   T.R, T.half_wid * 0.45f);
            cue_table_rack_14(&T, bl, nb);
        }
        if (bset >= 0) cue_render_set_ball_set(bset);
        for (int i = 0; i < nb; i++) if (bl[i].on) draw_ball(&bl[i], T.R);
    }
    Vec3 pc=W.drop_c[p], cc=W.cut_c[p], n=W.pmnorm[p];
    if (!s_persp) {
    circ_m(cc.x,cc.z,W.cut_r[p],            80,200,255,0,0);
    circ_m(cc.x,cc.z,W.cut_r[p]-W.lip_d[p], 30,120,210,0,1);
    circ_m(pc.x,pc.z,W.pocket_r[p],        255, 45, 45,1,0);
    circ_m(pc.x-n.x*W.pocket_r[p], pc.z-n.z*W.pocket_r[p], W.R, 255,255,255,0,0);
    float q0,q1; m2px(pc.x,pc.z,&q0,&q1); dot(q0,q1,3,255,45,45);
    m2px(cc.x,cc.z,&q0,&q1); dot(q0,q1,3,80,200,255);
    }
    /* Every pocket on this table, so a front end can list them by name and
     * show where each one actually is rather than assuming six in a ring. */
    {   fprintf(stderr, "{\"npocket\": %d, \"shown\": %d, \"pockets\": [", W.npocket, p);
        for (int q = 0; q < W.npocket; q++)
            fprintf(stderr, "%s{\"i\": %d, \"mid\": %d, \"x\": %.4f, \"z\": %.4f}",
                    q ? ", " : "", q, (int)W.pocket_mid[q],
                    (double)W.pocket[q].x, (double)W.pocket[q].z);
        fprintf(stderr, "]}\n"); }

    FILE *o=fopen(out,"wb"); fprintf(o,"P6\n%d %d\n255\n",IW,IH);
    fwrite(img,1,(size_t)IW*IH*3,o); fclose(o);

    /* the millimetres the page shows beside the sliders — derived, never dialled */
    const float tipmm = bore_clearance(&W, p, mid ? T.bore_side : T.bore_corner,
                                       mid ? T.bore_set_side : T.bore_set_corner) * 1000.0f;
    fprintf(stderr, "{\"mouth\": %.2f, \"drop\": %.2f, \"edge\": %.2f, "
                    "\"thick\": %.2f, \"ball\": %.2f, \"bore\": %.2f, "
                    "\"tip\": %.2f, \"solved_gap\": %.4f, \"gap_to_drop\": %.2f}\n",
        (double)(jaw_sep(&W,p)*1000.0f), (double)(W.pocket_r[p]*1000.0f),
        (double)(W.cut_r[p]*1000.0f), (double)(W.lip_d[p]*1000.0f),
        (double)(W.R*2000.0f),
        (double)((mid ? T.bore_side : T.bore_corner) * 1000.0f),
        (double)(tipmm == tipmm ? tipmm : 9999.0f),
        (double)gap_solved,
        (double)(( (W.cut_r[p] - sqrtf((W.cut_c[p].x-W.drop_c[p].x)*(W.cut_c[p].x-W.drop_c[p].x)
                                     + (W.cut_c[p].z-W.drop_c[p].z)*(W.cut_c[p].z-W.drop_c[p].z)))
                   - W.pocket_r[p]) * 1000.0f));
    return 0;
}
