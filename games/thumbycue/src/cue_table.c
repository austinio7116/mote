/*
 * ThumbyCue — table geometry & racks. See cue_table.h.
 *
 * The pocket-jaw model is the heart of the game. Each cushion is a straight
 * nose between two knuckle points, with a *facing* cut at each end running
 * back into the pocket throat. US pool uses straight mitred facings with
 * sharp points (corner cut 142°, side ~104°); snooker/UK uses short facings
 * with large rounded knuckles. Both the physics collision geometry (segments
 * + knuckle circles + capture points) and the 3D render mesh are generated
 * from this one description, so they can never disagree.
 */
#include "cue_table.h"

#ifndef CUE_JAW_SEGS
#define CUE_JAW_SEGS 3
#endif
#include "cue_types.h"
#include <string.h>
#include <math.h>

#define DEG (3.14159265f / 180.0f)

void cue_table_init(CueTable *t, CueGameKind kind) {
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->is_snooker = (kind == CUE_GAME_SNK10 || kind == CUE_GAME_SNK15 || kind == CUE_GAME_SNK6);

    if (kind == CUE_GAME_UK8 || kind == CUE_GAME_SNK6) {
        /* 7 ft UK pub 8-ball: 1.98 × 0.99 m, tight ROUNDED (curved) pockets. */
        t->half_len = 1.98f * 0.5f;
        t->half_wid = 0.99f * 0.5f;
        t->R = 0.028575f; t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.075f;
        t->pocket_round = 1;
        /* TIGHTER THAN THEY WERE, and about the size a 7 ft table's pockets
         * actually are. These are radii in ball-radii, so the mouth was 4.30
         * ball-radii across for a 2 in ball — 123 mm, where a real pub table
         * cuts about 89 mm. 1.72 took it to 98 mm and played a shade mean, so
         * it sits at 1.80 (103 mm) — still a pub pocket rather than the barn
         * door it was. Both games on this bed get it: one table, two rule sets. */
        t->pr_corner  = 1.80f * t->R; t->pr_side  = 1.63f * t->R;
        t->gap_corner = 2.667f * t->R; t->gap_side = 2.50f * t->R;
        t->facing_len = 1.667f * t->R;
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        /* Throat set back into the wood so the bore circle clears the (deepened)
         * cushion back and a proper wood ring is cut — see reach math in PLAN. */
        t->off_corner = 0.60f * t->R; t->off_side = 1.25f * t->R;
        t->jaw_r = 0.004f;
        t->cloth = RGB565C(22, 120, 70);
        t->rail = RGB565C(96, 54, 26); t->rail_top = RGB565C(128, 78, 38);
        t->spot = RGB565C(180, 180, 180); t->nballs = 16;
        /* UK 8-ball baulk line + D (white placed in the D after a foul). */
        t->baulk_x = -t->half_len * 0.6f; t->d_radius = t->half_wid * 0.35f;
        if (kind == CUE_GAME_SNK6) {
            /* 6-red snooker on the 7 ft UK table: same table geometry and ball
             * size as UK pool, but snooker balls/rules and snooker spots scaled
             * onto the small bed. */
            t->reds = 6;
            t->cloth = RGB565C(4, 135, 21);            /* snooker green */
            t->spot = RGB565C(200, 200, 200);
            t->blue_x  = 0.0f;                          /* centre spot */
            t->pink_x  = t->half_len * 0.5f;            /* between centre and top */
            t->black_x = t->half_len - 0.135f;          /* ~scaled from full table */
            t->nballs  = 13;                            /* cue + 6 reds + 6 colours */
        }
    } else if (kind == CUE_GAME_US8 || kind == CUE_GAME_US9) {
        /* 9 ft US table: 2.54 × 1.27 m, 2.25" balls, ANGLED straight-mitre
         * pockets (sharp points, more open than UK). */
        t->half_len = 2.54f * 0.5f;
        t->half_wid = 1.27f * 0.5f;
        t->R = 0.028575f; t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.080f;
        t->pocket_round = 0;                 /* straight mitred facings */
        /* TIGHTENED 20%, at the user's call: these were far and away the most
         * open pockets in the set — 2.75 R at the corner against snooker's 1.98
         * and the UK pub table's 2.15 — and they looked it next to the others.
         * Both numbers move together: `pr` is the mouth you see and `gap` is
         * where the jaw tips actually sit, so tightening one without the other
         * would draw a smaller pocket than the ball is allowed through.
         *     corner  2.75 -> 2.20 R      side  2.35 -> 1.88 R
         *     gap     3.20 -> 2.56 R      gap   2.95 -> 2.36 R
         * Still the widest mouths on any table here, which is right for an
         * American table; they are no longer in a class of their own. */
        t->pr_corner  = 2.20f * t->R; t->pr_side  = 1.88f * t->R;
        t->gap_corner = 2.56f * t->R; t->gap_side = 2.36f * t->R;
        t->facing_len = 1.55f * t->R;
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        t->off_corner = 1.30f * t->R; t->off_side = 1.20f * t->R;  /* corners set back into the pocket */
        t->jaw_r = 0.004f;
        t->cloth = RGB565C(18, 110, 120);    /* US tables often tournament blue-green */
        t->rail = RGB565C(70, 46, 30); t->rail_top = RGB565C(100, 66, 42);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = (kind == CUE_GAME_US9) ? 10 : 16;
    } else if (kind == CUE_GAME_CN8) {
        /* Chinese 8-ball: 10 ft table, full-size pool balls (solids/stripes),
         * but TIGHT ROUNDED ("Chinese template") pockets — closer to English
         * than American. Rack/break/spots as US 8-ball (foot spot, head string),
         * rules are WPA 8-ball (no UK two-shot). */
        t->half_len = 2.84f * 0.5f;
        t->half_wid = 1.42f * 0.5f;
        t->R = 0.028575f; t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.080f;
        t->pocket_round = 1;                 /* rounded jaws, tight */
        t->pr_corner  = 2.05f * t->R; t->pr_side  = 1.88f * t->R;   /* tighter than UK pub */
        t->gap_corner = 2.55f * t->R; t->gap_side = 2.42f * t->R;
        t->facing_len = 1.667f * t->R;
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        t->off_corner = 0.60f * t->R; t->off_side = 1.25f * t->R;
        t->jaw_r = 0.005f;
        t->cloth = RGB565C(22, 44, 155);     /* Chinese-8 royal blue cloth */
        t->rail = RGB565C(78, 48, 28); t->rail_top = RGB565C(112, 70, 36);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = 16;
    } else {
        /* Snooker — SNK10 (10 ft, 10 reds) or SNK15 (12 ft, 15 reds). Curved
         * jaws. Layout offsets scale with table length off the 12 ft master. */
        t->reds = (kind == CUE_GAME_SNK10) ? 10 : 15;
        float master = 3.569f * 0.5f;        /* 12 ft half-length */
        if (kind == CUE_GAME_SNK10) { t->half_len = 2.972f * 0.5f; t->half_wid = 1.483f * 0.5f; }
        else                        { t->half_len = 3.569f * 0.5f; t->half_wid = 1.778f * 0.5f; }
        float sc = t->half_len / master;     /* layout scale */
        t->R = 0.0262500f; t->mass = 0.142f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.085f;
        t->pocket_round = 1;
        t->pr_corner  = 1.98f * t->R; t->pr_side  = 1.82f * t->R;
        t->gap_corner = 2.45f * t->R; t->gap_side = 2.18f * t->R;
        t->facing_len = 1.25f * t->R;
        t->ang_corner = 60.0f; t->ang_side = 80.0f;
        /* Throat set well back into the wood: the small snooker pocket radius is
         * < the deepened cushion depth, so without this the bore circle never
         * reaches the wood and no cutaway is cut (the fall is realistically set
         * back behind the mouth anyway). */
        t->off_corner = 1.30f * t->R; t->off_side = 1.00f * t->R;  /* corners back, but not too far */
        t->jaw_r = 0.012f;
        t->baulk_x = -t->half_len + 0.737f * sc;
        t->d_radius = 0.292f * sc;
        t->blue_x = 0.0f;
        t->pink_x = t->half_len * 0.5f;
        t->black_x = t->half_len - 0.324f * sc;
        t->cloth = RGB565C(4, 135, 21);
        t->rail = RGB565C(74, 44, 22); t->rail_top = RGB565C(104, 62, 30);
        t->spot = RGB565C(200, 200, 200);
        t->nballs = (t->reds == 10) ? 17 : 22;
    }
    /* Drop-zone setback — how far the potted ball sinks BACK into the pocket
     * (past the cushion mouth) before it disappears. Scaled off each table's
     * pocket-mouth size so it tracks the official openings: a corner has a deep
     * fall (~0.6× its mouth radius), a middle pocket is shallow and the ball
     * must enter centrally, so it pulls straight back only a little (~0.3×). */
    /* The head string, for the US-style tables that have one instead of a D.
     * It has to live in `baulk_x` because that is the field every renderer asks
     * for the cross-bed line — leave it zero and the line is drawn down the
     * middle of the table, which is exactly what CueVR was doing. d_radius stays
     * zero: an American table has a head string and no D. */
    if (kind == CUE_GAME_US8 || kind == CUE_GAME_US9 || kind == CUE_GAME_CN8)
        t->baulk_x = -t->half_len * 0.5f;

    t->drop_back      = 0.60f * t->pr_corner;
    t->drop_back_side = 0.30f * t->pr_side;
}

/* Inward unit normal of segment a→b. The cushion boundary is built as one
 * closed loop traversed in a consistent sense (top rail +x, right rail +z, …),
 * so the inward normal is simply the edge direction rotated +90°: (-dz, dx).
 * (The earlier "point toward the origin" heuristic flipped corner-FACING
 * segments the wrong way — facings splay outward past the rail line so their
 * midpoint-to-centre direction is not the surface normal. That single flip
 * spiked the render jaws AND gave balls a wrong cushion normal off the jaws.) */
static Vec3 inward_n(float ax, float az, float bx, float bz) {
    float dx = bx - ax, dz = bz - az;
    return v3_norm(v3(-dz, 0, dx));
}
static void add_seg(CueWorld *w, Vec3 a, Vec3 b, uint8_t kind) {
    if (w->nseg >= CUE_MAX_SEG) return;
    CueSeg *s = &w->seg[w->nseg++];
    s->a = v3(a.x, w->R, a.z);
    s->b = v3(b.x, w->R, b.z);
    s->n = inward_n(a.x, a.z, b.x, b.z);
    s->kind = kind;
}
static void add_jaw(CueWorld *w, Vec3 k) {
    if (w->njaw >= CUE_MAX_SEG) return;
    w->jaw[w->njaw++] = v3(k.x, w->R, k.z);
}
static void add_pocket(CueWorld *w, float x, float z, float cap) {
    if (w->npocket >= CUE_MAX_POCKET) return;
    int i = w->npocket++;
    w->pocket[i] = v3(x, 0, z);
    w->pocket_r[i] = cap;
}

/* Straight cushion chain (US pool): facing-tip → knuckle → knuckle →
 * facing-tip. P2,P3 are pushed into w->jaw in boundary order so the renderer
 * can fan the bed off them. */
/* Recess a jaw (rattle) circle so its PLAYABLE edge sits flush with the rail
 * nose instead of poking past it: shift the centre outward (−nose inward normal)
 * by its radius + a small margin. A ball hugging the rail then clears it
 * cleanly; a ball entering the pocket mouth still rattles. */
static void add_jaw_recessed(CueWorld *w, Vec3 k, Vec3 nin) {
    float off = w->jaw_r + 0.15f * w->R;
    add_jaw(w, v3(k.x - nin.x * off, 0, k.z - nin.z * off));
}
static void add_chain(CueWorld *w, Vec3 P1, Vec3 P2, Vec3 P3, Vec3 P4) {
    add_seg(w, P1, P2, 1);
    add_seg(w, P2, P3, 0);
    add_seg(w, P3, P4, 1);
    Vec3 nin = inward_n(P2.x, P2.z, P3.x, P3.z);   /* nose inward normal */
    add_jaw_recessed(w, P2, nin);
    add_jaw_recessed(w, P3, nin);
}

/* Append nseg facing segments along a quadratic-bezier curve from s to e with
 * a perpendicular bulge (matches the 2D game's generateCurvePoints). */
static void seg_curve(CueWorld *w, Vec3 s, Vec3 e, float dir, int nseg) {
    float dx = e.x - s.x, dz = e.z - s.z;
    float len = sqrtf(dx*dx + dz*dz);
    if (len < 1e-6f) return;
    float px = -dz/len, pz = dx/len, depth = len * 0.4f * dir;
    float cx = (s.x+e.x)*0.5f + px*depth, cz = (s.z+e.z)*0.5f + pz*depth;
    Vec3 prev = s;
    for (int i = 1; i <= nseg; i++) {
        float t = (float)i/nseg, o = 1.0f - t;
        Vec3 p = v3(o*o*s.x + 2*o*t*cx + t*t*e.x, 0,
                    o*o*s.z + 2*o*t*cz + t*t*e.z);
        add_seg(w, prev, p, 1);
        prev = p;
    }
}

/* Curved cushion chain (snooker/UK): bezier jaw → straight nose → bezier jaw.
 * The curves bulge into the pocket, giving rounded knuckles. */
static void add_curved_chain(CueWorld *w, Vec3 tipIn, Vec3 kIn, Vec3 kMid,
                             Vec3 tipMid, float aIn, float aOut, int nIn, int nOut) {
    seg_curve(w, tipIn, kIn, aIn, nIn);
    add_seg(w, kIn, kMid, 0);
    seg_curve(w, kMid, tipMid, aOut, nOut);
    Vec3 nin = inward_n(kIn.x, kIn.z, kMid.x, kMid.z);   /* nose inward normal */
    add_jaw_recessed(w, kIn, nin);
    add_jaw_recessed(w, kMid, nin);
}

void cue_table_build_world(const CueTable *t, CueWorld *w) {
    cue_world_defaults(w, t->R, t->mass);
    w->cush_tilt = asinf((t->cushion_h - t->R) / t->R);
    /* The height a ball has to be above the cloth to be over the rail rather
     * than bouncing off it. The table knows it; the physics only needed telling. */
    w->cushion_nose = t->cushion_h;
    /* THE FRAME IS A SURFACE, AND IT IS WIDER THAN THE RAIL CAP.
     *
     * rail_w alone is 75-85 mm and a ball is 52-57 mm across, so a ball that
     * cleared a cushion landed on the cap, rolled about one ball's width and
     * fell off the outer edge — reported as vanishing the moment it bounced.
     * The woodwork does not stop there: CueVR's frame builds the body's outer
     * face at rail_w + 55 mm (SURR_X/SURR_Z) and the apron runs out to it, so
     * there is another 55 mm of table to land on and it should behave like it.
     *
     * That roughly doubles the width a jumped ball has to run along, which is
     * the difference between "it fell off immediately" and a ball that rattles
     * along the top and may still drop into a pocket. */
    w->bound_x = t->half_len + t->rail_w + CUE_FRAME_OUT;
    w->bound_z = t->half_wid + t->rail_w + CUE_FRAME_OUT;
    w->play_x  = t->half_len;
    w->play_z  = t->half_wid;
    /* The same height cue_table_surface reports and cue_render draws: the
     * cushion top and the wood cap are one surface, not a step. */
    w->rail_top = t->cushion_h * 1.30f;
    w->jaw_r = t->jaw_r;
    w->drop_back = t->drop_back;
    w->drop_back_side = t->drop_back_side;

    const float hl = t->half_len, hw = t->half_wid, R = t->R;

    if (!t->pocket_round) {
        /* US pool: straight mitred facings. */
        const float g = t->gap_corner, sg = t->gap_side, sl = t->facing_len;
        const float cc = cosf(t->ang_corner*DEG), sc = sinf(t->ang_corner*DEG);
        const float cs = cosf(t->ang_side*DEG),   ss = sinf(t->ang_side*DEG);
        add_chain(w, v3(-hl+g - cc*sl, 0, -hw - sc*sl), v3(-hl+g, 0, -hw),
                     v3(-sg, 0, -hw),                   v3(-sg + cs*sl, 0, -hw - ss*sl));
        add_chain(w, v3(sg - cs*sl, 0, -hw - ss*sl),    v3(sg, 0, -hw),
                     v3(hl-g, 0, -hw),                  v3(hl-g + cc*sl, 0, -hw - sc*sl));
        add_chain(w, v3(hl + sc*sl, 0, -hw+g - cc*sl),  v3(hl, 0, -hw+g),
                     v3(hl, 0, hw-g),                   v3(hl + sc*sl, 0, hw-g + cc*sl));
        add_chain(w, v3(hl-g + cc*sl, 0, hw + sc*sl),   v3(hl-g, 0, hw),
                     v3(sg, 0, hw),                     v3(sg - cs*sl, 0, hw + ss*sl));
        add_chain(w, v3(-sg + cs*sl, 0, hw + ss*sl),    v3(-sg, 0, hw),
                     v3(-hl+g, 0, hw),                  v3(-hl+g - cc*sl, 0, hw + sc*sl));
        add_chain(w, v3(-hl - sc*sl, 0, hw-g + cc*sl),  v3(-hl, 0, hw-g),
                     v3(-hl, 0, -hw+g),                 v3(-hl - sc*sl, 0, -hw+g - cc*sl));
    } else {
        /* Snooker/UK: bezier-curved jaws bulging into the pocket (rounded
         * knuckles), matching the 2D game's createCurvedRailChains. */
        const float bg = t->pr_corner + 0.5f*R;
        const float cgap = bg + 0.333f*R, mgap = bg + 0.583f*R;
        const float cl = 2.0f*R, ml = 1.6f*R, e3 = 0.25f*R;
        /* Bezier steps per jaw. Three is right for a 128x128 screen and reads as
         * blocky knuckles when the pocket is 30 cm from your eye, so the VR build
         * raises it. More steps approximate the SAME intended curve more closely,
         * so this refines the collision geometry rather than changing it — but it
         * is a compile-time knob so the handheld's physics stays bit-identical. */
        const float ca = 0.6f, ma = 0.7f;
        const int nc = CUE_JAW_SEGS, nm = CUE_JAW_SEGS;
        /* C1 top-left */
        add_curved_chain(w, v3(-hl+cgap - cl*0.7f - e3,0,-hw - cl*0.7f), v3(-hl+cgap,0,-hw),
                            v3(-mgap,0,-hw), v3(-bg + ml*0.3f + e3,0,-hw - ml), ca, ma, nc, nm);
        /* C2 top-right */
        add_curved_chain(w, v3(bg - ml*0.3f - e3,0,-hw - ml), v3(mgap,0,-hw),
                            v3(hl-cgap,0,-hw), v3(hl-cgap + cl*0.7f + e3,0,-hw - cl*0.7f), ma, ca, nm, nc);
        /* C3 right */
        add_curved_chain(w, v3(hl + cl*0.7f,0,-hw+cgap - cl*0.7f - e3), v3(hl,0,-hw+cgap),
                            v3(hl,0,hw-cgap), v3(hl + cl*0.7f,0,hw-cgap + cl*0.7f + e3), ca, ca, nc, nc);
        /* C4 bottom-right */
        add_curved_chain(w, v3(hl-cgap + cl*0.7f + e3,0,hw + cl*0.7f), v3(hl-cgap,0,hw),
                            v3(mgap,0,hw), v3(bg - ml*0.3f - e3,0,hw + ml), ca, ma, nc, nm);
        /* C5 bottom-left */
        add_curved_chain(w, v3(-bg + ml*0.3f + e3,0,hw + ml), v3(-mgap,0,hw),
                            v3(-hl+cgap,0,hw), v3(-hl+cgap - cl*0.7f - e3,0,hw + cl*0.7f), ma, ca, nm, nc);
        /* C6 left */
        add_curved_chain(w, v3(-hl - cl*0.7f,0,hw-cgap + cl*0.7f + e3), v3(-hl,0,hw-cgap),
                            v3(-hl,0,-hw+cgap), v3(-hl - cl*0.7f,0,-hw+cgap - cl*0.7f - e3), ca, ca, nc, nc);
    }

    /* Smooth vertex normals: at each endpoint shared with a neighbouring
     * segment, average the two face normals so the collision normal can be
     * interpolated continuously along GENTLE curves (snooker rounded knuckles,
     * bezier jaw steps) — no kink there. But ONLY when the junction is actually
     * smooth: a sharp CONVEX corner (the US straight-pocket rail↔facing mitre,
     * which juts into play) must NOT be averaged, or the rail's endpoint normal
     * gets pulled toward the facing and a ball gliding along the rail bounces
     * off "the back of the knuckle" instead of glancing off the flat rail. The
     * jaw circle handles contact at those sharp knuckles. Threshold ≈ 37°. */
    const float SMOOTH_COS = 0.80f;
    for (int s = 0; s < w->nseg; s++) {
        Vec3 ns = w->seg[s].n;
        Vec3 na = ns, nb = ns;
        for (int o = 0; o < w->nseg; o++) {
            if (o == s) continue;
            /* Only smooth WITHIN a run of the same kind: the bezier jaw (all
             * kind=1) stays a continuous rounded knuckle, but a facing must NEVER
             * pull the straight rail nose's (kind=0) endpoint normal. Snooker's
             * rail→curve junction is gentle enough to pass the dot test below, so
             * without this a ball hugging the rail near the knuckle saw a normal
             * tilted toward the pocket throat — funnelling it into the pocket and
             * spinning it up off the rail. The jaw circle handles the junction. */
            if (w->seg[o].kind != w->seg[s].kind) continue;
            Vec3 no = w->seg[o].n;
            if (ns.x*no.x + ns.z*no.z < SMOOTH_COS) continue;   /* sharp corner: keep crisp */
            if (v3_len2(v3_sub(w->seg[o].b, w->seg[s].a)) < 1e-8f ||
                v3_len2(v3_sub(w->seg[o].a, w->seg[s].a)) < 1e-8f)
                na = v3_add(na, no);
            if (v3_len2(v3_sub(w->seg[o].a, w->seg[s].b)) < 1e-8f ||
                v3_len2(v3_sub(w->seg[o].b, w->seg[s].b)) < 1e-8f)
                nb = v3_add(nb, no);
        }
        w->seg[s].na = v3_norm(na);
        w->seg[s].nb = v3_norm(nb);
    }

    /* Pocket circles: centre offset just beyond the boundary; drop-capture
     * when the ball centre is within (radius − 0.3R), matching the 2D game. */
    const float d = 0.70710678f, oc = t->off_corner, os = t->off_side;
    /* Drop-capture radius (independent of the visible mouth/pr_side). UK8 pub
     * tables have notoriously tight side pockets, so shrink their side capture. */
    float side_m = (t->kind == CUE_GAME_UK8) ? 0.15f : 0.30f;  /* UK middle drop ~ corner size */
    float capc = t->pr_corner - 0.3f * t->R, caps = t->pr_side - side_m * t->R;
    add_pocket(w, -hl - oc*d, -hw - oc*d, capc);
    add_pocket(w,  hl + oc*d, -hw - oc*d, capc);
    add_pocket(w,  hl + oc*d,  hw + oc*d, capc);
    add_pocket(w, -hl - oc*d,  hw + oc*d, capc);
    add_pocket(w, 0.0f, -hw - os, caps);
    add_pocket(w, 0.0f,  hw + os, caps);

    /* ---- each pocket's mouth, from the two jaw tips beside it ------------ */
    for (int p = 0; p < w->npocket; p++) {
        int j1 = -1, j2 = -1; float d1 = 1e30f, d2 = 1e30f;
        for (int j = 0; j < w->njaw; j++) {
            float dx = w->jaw[j].x - w->pocket[p].x;
            float dz = w->jaw[j].z - w->pocket[p].z;
            float dd = dx*dx + dz*dz;
            if (dd < d1) { d2 = d1; j2 = j1; d1 = dd; j1 = j; }
            else if (dd < d2) { d2 = dd; j2 = j; }
        }
        if (j1 < 0 || j2 < 0) {                 /* no jaws: fall back to radial */
            float l = sqrtf(w->pocket[p].x*w->pocket[p].x + w->pocket[p].z*w->pocket[p].z);
            if (l < 1e-6f) l = 1.0f;
            w->pmnorm[p] = v3(w->pocket[p].x/l, 0, w->pocket[p].z/l);
            w->pmouth[p] = w->pocket[p];
            continue;
        }
        Vec3 a = w->jaw[j1], b2 = w->jaw[j2];
        w->pmouth[p] = v3((a.x + b2.x) * 0.5f, 0, (a.z + b2.z) * 0.5f);
        /* Normal of the jaw-to-jaw line, pointing away from the table centre. */
        float ex = b2.x - a.x, ez = b2.z - a.z;
        float el = sqrtf(ex*ex + ez*ez); if (el < 1e-6f) el = 1.0f;
        Vec3 nrm = v3(-ez/el, 0, ex/el);
        if (nrm.x * w->pocket[p].x + nrm.z * w->pocket[p].z < 0.0f)
            nrm = v3(-nrm.x, 0, -nrm.z);
        w->pmnorm[p] = nrm;
    }

    cue_table_derive_cut(w);
}

/* ---- the cloth cut, which both the renderer and the physics obey ---------- *
 *
 * These are set in the headset, with a ball rolling at the pocket, which is the
 * only place they could honestly be judged. The radii are multiples of the
 * ball's drop circle, so one pair of numbers carries across the whole set of
 * tables and the millimetres follow from each table's own pocket size. The
 * setbacks push each arc's centre away from the table, into the frame. */
static float s_cut_cr = CUE_CUT_CORNER, s_cut_cs = CUE_COR_SETBACK;
static float s_cut_mr = CUE_CUT_MIDDLE, s_cut_ms = CUE_MID_SETBACK;

void cue_table_derive_cut(CueWorld *w) {
    for (int p = 0; p < w->npocket; p++) {
        Vec3 C = w->pocket[p];
        if (p < 4) {                    /* a corner sets back along its diagonal */
            float sx = (C.x < 0) ? -1.0f : 1.0f, sz = (C.z < 0) ? -1.0f : 1.0f;
            const float k = 0.70710678f;
            w->cut_c[p] = v3(C.x + sx*s_cut_cs*k, 0, C.z + sz*s_cut_cs*k);
            w->cut_r[p] = w->pocket_r[p] * s_cut_cr;
        } else {                        /* a middle sets straight back into the rail */
            float sz = (C.z < 0) ? -1.0f : 1.0f;
            w->cut_c[p] = v3(C.x, 0, C.z + sz * s_cut_ms);
            w->cut_r[p] = w->pocket_r[p] * s_cut_mr;
        }
        w->lip_d[p] = CUE_LIP_ROLL * w->pocket_r[p];
    }
}

void cue_table_set_pocket_cut(CueWorld *w, float cr, float cs, float mr, float ms) {
    s_cut_cr = cr; s_cut_cs = cs; s_cut_mr = mr; s_cut_ms = ms;
    if (w) cue_table_derive_cut(w);
}

void cue_table_get_pocket_cut(float *cr, float *cs, float *mr, float *ms) {
    if (cr) *cr = s_cut_cr; if (cs) *cs = s_cut_cs;
    if (mr) *mr = s_cut_mr; if (ms) *ms = s_cut_ms;
}

Vec3 cue_table_cue_home(const CueTable *t) {
    /* All games start OFF the centre line so a break naturally strikes the pack
     * at an angle (a dead-straight break into the apex splits poorly). Snooker &
     * UK8 break from one side of the D; US pool from the side of the kitchen. */
    if (t->is_snooker)            return v3(t->baulk_x, t->R, -t->d_radius * 0.55f);
    if (t->kind == CUE_GAME_UK8)  return v3(t->baulk_x, t->R, -t->d_radius * 0.55f);
    return v3(-t->half_len * 0.5f, t->R, t->half_wid * 0.40f);
}

/* Clamp a desired cue-ball placement to the legal ball-in-hand region:
 * inside the D (snooker / UK8) or behind the head string (US pool). Returns the
 * clamped XZ (y left to the caller). */
static Vec3 clamp_region(const CueTable *t, Vec3 p, int breaking);

Vec3 cue_table_clamp_placement(const CueTable *t, Vec3 p) {
    return cue_table_clamp_placement_balls(t, p, NULL, 0, 0);
}

/* The legal region, and then out of anything already standing in it.
 *
 * Region-only clamping let the player park the cue ball inside another ball,
 * which the solver then resolved by firing both of them apart on the first
 * tick. Pushing out and re-clamping alternately is what makes the ball SLIDE
 * around an obstruction instead of stopping dead at it or passing through: each
 * pass moves it clear of the nearest ball, then back inside the region, and the
 * two settle within a few passes. Six is enough for the worst case anyone can
 * drive a stick into — a ball wedged against the baulk cushion between two
 * others — and if it somehow does not settle, the last legal position stands. */
/* True if p is clear of every ball on the table. */
static int placement_clear(const CueTable *t, Vec3 p, const CueBall *balls, int n) {
    const float sep = 2.0f * t->R + 0.0004f;
    for (int i = 1; i < n; i++) {
        if (!balls[i].on) continue;
        float dx = p.x - balls[i].pos.x, dz = p.z - balls[i].pos.z;
        if (dx * dx + dz * dz < sep * sep) return 0;
    }
    return 1;
}

Vec3 cue_table_clamp_placement_balls(const CueTable *t, Vec3 p,
                                     const CueBall *balls, int n, int breaking) {
    const float R = t->R;
    const float sep = 2.0f * R + 0.0004f;      /* a whisker of daylight */
    for (int pass = 0; pass < 6; pass++) {
        p = clamp_region(t, p, breaking);
        int moved = 0;
        for (int i = 1; i < n; i++) {
            if (!balls[i].on) continue;
            float dx = p.x - balls[i].pos.x, dz = p.z - balls[i].pos.z;
            float d2 = dx*dx + dz*dz;
            if (d2 >= sep*sep) continue;
            float d = sqrtf(d2);
            if (d < 1e-5f) {                   /* dead centre: pick a direction */
                dx = 1.0f; dz = 0.0f; d = 1.0f;
            }
            p.x = balls[i].pos.x + dx / d * sep;
            p.z = balls[i].pos.z + dz / d * sep;
            moved = 1;
        }
        if (!moved) break;
    }

    /* THE REGION IS A RULE; THE SEPARATION IS A COURTESY, and the loop above had
     * that the wrong way round. Its last act is a PUSH, not a clamp, so a ball
     * placed against one of the baulk colours was shoved clear of the colour and
     * returned there — outside the D, in front of the line, which is not a legal
     * position in any of these games. Reported as the cue ball ending up forward
     * of the D whenever something was in the way, and it happened to the CPU as
     * well because cue_ai_place asks this same function.
     *
     * So the region wins. Clamp, and if that lands on top of something, look
     * for the nearest spot that is BOTH legal and clear rather than accepting
     * one that is neither: rings of candidates outward from where the player
     * asked, each one clamped back into the region before it is judged. Half a
     * ball at a time, out to a couple of ball widths, is more than enough room
     * for the worst case there is — the cue ball wanted where the brown sits
     * with the green and yellow either side. */
    p = clamp_region(t, p, breaking);
    if (n > 0 && !placement_clear(t, p, balls, n)) {
        const Vec3 want = p;
        for (int ring = 1; ring <= 6; ring++) {
            float rad = (float)ring * R;
            for (int a = 0; a < 16; a++) {
                float th = (float)a * (6.2831853f / 16.0f);
                Vec3 c = { want.x + cosf(th) * rad, want.y, want.z + sinf(th) * rad };
                c = clamp_region(t, c, breaking);
                if (placement_clear(t, c, balls, n)) return c;
            }
        }
        /* Nowhere legal and clear — a table so full there is no room in the D.
         * The legal spot stands: overlapping is recoverable, illegal is not. */
    }
    return p;
}

static Vec3 clamp_region(const CueTable *t, Vec3 p, int breaking) {
    float R = t->R;
    if (t->is_snooker || t->kind == CUE_GAME_UK8) {
        /* The D: a half-disc of radius d_radius centred on (baulk_x, 0), bulging
         * toward the baulk cushion (−x).
         *
         * The CENTRE goes up to the line, not the ball's edge. A ball played
         * from hand is in the D if its centre is within the D or on the lines
         * bounding it — half of it may hang outside, and on a tight angle from
         * the D that half is the difference between having the shot and not.
         * This kept the whole ball inside, which quietly shrank the D by a ball
         * radius all the way round. */
        float rmax = t->d_radius;
        if (p.x > t->baulk_x) p.x = t->baulk_x;        /* not past the baulk line */
        float dx = p.x - t->baulk_x, dz = p.z;
        float d = sqrtf(dx*dx + dz*dz);
        if (d > rmax) { float s = rmax / d; p.x = t->baulk_x + dx*s; p.z = dz*s; }
        return p;
    }
    /* US pool — 8-ball, 9-ball and Chinese 8: ball in hand is ANYWHERE ON THE
     * TABLE. Behind the head string is the rule for the BREAK, and only for the
     * break; this applied it to every foul as well, so a player fouled at the
     * black end and was marched back to baulk to place. Chinese 8 is where it
     * was noticed and all three were doing it.
     *
     * Snooker and UK 8-ball are different and are handled above: there the ball
     * really does go back in the D every time. */
    float lim = t->half_wid - R;
    float lenlim = t->half_len - R;
    if (!breaking) {
        if (p.x >  lenlim) p.x =  lenlim;
        if (p.x < -lenlim) p.x = -lenlim;
        if (p.z >  lim) p.z =  lim;
        if (p.z < -lim) p.z = -lim;
        return p;
    }
    float head = t->baulk_x;
    if (p.x > head - R) p.x = head - R;
    if (p.x < -(t->half_len - R)) p.x = -(t->half_len - R);
    if (p.z >  lim) p.z =  lim;
    if (p.z < -lim) p.z = -lim;
    return p;
}

/* Per-rack RNG (render-only ball orientation; advances each ball + each rack so
 * the balls don't all face the same way and racks differ between frames). */
static uint32_t s_orient_rng = 0x2545F491u;
static float orient_rand(void) {
    s_orient_rng ^= s_orient_rng << 13; s_orient_rng ^= s_orient_rng >> 17; s_orient_rng ^= s_orient_rng << 5;
    return (float)(s_orient_rng & 0xFFFFu) * (1.0f / 65536.0f);
}
static Mat3 rand_orient(void) {
    Mat3 m = m3_identity();
    m3_rotate_local(&m, 0, orient_rand() * 6.2831853f);
    m3_rotate_local(&m, 1, orient_rand() * 6.2831853f);
    m3_rotate_local(&m, 2, orient_rand() * 6.2831853f);
    return m;
}

static void set_ball(CueBall *b, int id, float x, float z, float R) {
    b->pos = v3(x, R, z);
    b->vel = v3(0, 0, 0);
    b->w = v3(0, 0, 0);
    b->orient = rand_orient();      /* random facing so the rack isn't uniform */
    b->on = 1;
    b->id = (uint8_t)id;
    b->pocket = 0;
}

static int rack_pool(const CueTable *t, CueBall *b) {
    const float R = t->R;
    float footx = t->half_len * 0.5f;
    float dx = R * 1.7320508f;
    /* Fixed arrangement matching 2dpool (RackPatterns.eightBall): 8 in the centre
     * of row 3, one solid + one stripe in the two back corners. */
    static const int rows[5][5] = {
        { 1 }, { 9, 2 }, { 3, 8, 10 }, { 11, 4, 5, 12 }, { 6, 13, 14, 7, 15 },
    };
    int n = 1;
    for (int row = 0; row < 5; row++) {
        float x = footx + row * dx;
        for (int k = 0; k <= row; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            set_ball(&b[n++], rows[row][k], x, z, R);
        }
    }
    /* From cue_table_cue_home(), not from a repeat of the head-string number:
     * a UK table breaks from inside the D, and hard-coding -hl/2 here put the
     * cue ball a hand's breadth IN FRONT of its own baulk line — an illegal
     * break, on the centre line, on the one table whose rules this file
     * already knew. */
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    return n;
}

/* US 9-ball: diamond rack — 1 at the apex (foot spot), 9 in the centre. */
static int rack_9ball(const CueTable *t, CueBall *b) {
    const float R = t->R;
    float footx = t->half_len * 0.5f;
    float dx = R * 1.7320508f;
    set_ball(&b[1], 1, footx,          0.0f,  R);
    set_ball(&b[2], 2, footx + dx,    -R,     R);
    set_ball(&b[3], 3, footx + dx,    +R,     R);
    set_ball(&b[4], 4, footx + 2*dx,  -2*R,   R);
    set_ball(&b[5], 9, footx + 2*dx,   0.0f,  R);   /* 9 in the middle */
    set_ball(&b[6], 5, footx + 2*dx,  +2*R,   R);
    set_ball(&b[7], 6, footx + 3*dx,  -R,     R);
    set_ball(&b[8], 7, footx + 3*dx,  +R,     R);
    set_ball(&b[9], 8, footx + 4*dx,   0.0f,  R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    return 10;
}

static int rack_snooker(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    set_ball(&b[n++], CUE_ID_CUE, t->baulk_x, -t->d_radius * 0.4f, R);
    /* Green, brown, yellow reading LEFT TO RIGHT from behind the D — "God
     * Bless You". +Z is the player's right facing up the table, so yellow is
     * +d_radius. This and cue_rules.c's respot table were both mirrored, and
     * both carried a comment saying the opposite of what the code did. */
    set_ball(&b[n++], CUE_ID_YELLOW, t->baulk_x, +t->d_radius, R);   /* yellow — right of the D */
    set_ball(&b[n++], CUE_ID_GREEN,  t->baulk_x, -t->d_radius, R);   /* green  — left of the D  */
    set_ball(&b[n++], CUE_ID_BROWN,  t->baulk_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_BLUE,   t->blue_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_PINK,   t->pink_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_BLACK,  t->black_x, 0.0f, R);
    /* reds triangle: 3 rows (6), 4 rows (10) or 5 rows (15), apex behind pink. */
    int rows = (t->reds <= 6) ? 3 : (t->reds <= 10) ? 4 : 5;
    float apexx = t->pink_x + 2.0f * R + 0.002f;
    float dx = R * 1.7320508f;
    int red_id = 1;
    for (int row = 0; row < rows; row++) {
        float x = apexx + row * dx;
        for (int k = 0; k <= row; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            set_ball(&b[n++], red_id++, x, z, R);
        }
    }
    return n;
}

/* A SIX-BALL RACK for the practice challenges: a small triangle on the foot
 * spot and the white at home. Ids 1..6, which are reds on a snooker table and
 * the low solids on a pool one — so the rack is always made of balls the table
 * you are on actually uses, without the caller having to know which. */
int cue_table_rack_six(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    const float R = t->R;
    const float footx = t->half_len * 0.5f;
    const float dx = R * 1.7320508f;
    int n = 1, id = 1;
    for (int row = 0; row < 3; row++) {
        float x = footx + row * dx;
        for (int k = 0; k <= row; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            set_ball(&balls[n++], id++, x, z, R);
        }
    }
    { Vec3 h = cue_table_cue_home(t); set_ball(&balls[0], CUE_ID_CUE, h.x, h.z, R); }
    return n;
}

int cue_table_rack(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    if (t->is_snooker)            return rack_snooker(t, balls);
    if (t->kind == CUE_GAME_US9)  return rack_9ball(t, balls);
    return rack_pool(t, balls);   /* UK8 + US8 */
}

/* ---- forced cue elevation --------------------------------------------- *
 * The cue is a SOLID TAPERED STICK sitting somewhere in the world, and the only
 * question is whether it is inside something. Nothing here knows about the cue
 * ball, and that is the point: the previous version modelled the SHOT — a shaft
 * starting at the contact point on the white and running back — which is fine
 * on the handheld, where the cue exists only at address, and nonsense in VR the
 * moment you pick the cue up. It went on answering "the shot from that ball
 * needs thirty degrees" while the player stood a metre away with the cue in the
 * air, and the cue was dragged off their hands to match.
 *
 * So: take the tip where it actually is, run back along the aim for the cue's
 * length, and ask what the stick would pass through. The answer is the smallest
 * elevation that clears all of it, pivoting about the tip.
 *
 * What it can be inside:
 *   - the bed, cloth at y = 0 across the playing area;
 *   - the RAIL BAND, from the cushion nose out to the frame edge, solid to the
 *     rail top. Its absence is why a cue could be laid under the rail and
 *     through the table: a cushion height alone says nothing about the plank
 *     beside it;
 *   - the balls;
 *   - and past the frame edge, nothing at all — that is open air.
 *
 * Clearance follows the taper, so the fat end of the cue is given the room it
 * actually needs rather than being treated as a line down its axis. */

#define CUE_SHAFT_REACH 1.45f       /* == CUEVR_CUE_LEN */
#define CUE_TIP_R    0.00475f       /* 9.5 mm tip ferrule */
#define CUE_BUTT_R   0.01500f       /* 30 mm at the butt */
#define CUE_ELEV_MAX 1.30f          /* steep masse; past this it is not a shot */

/* Half-thickness of the cue `dd` back from the tip. */
static float cue_shaft_r(float dd) {
    /* THE TIP RADIUS, ALL THE WAY BACK. Clearing the true taper is honest and
     * plays badly: the fat end is a metre behind the ball and its half-inch of
     * timber was forcing the cue up on shots a player would simply make. What
     * a player actually judges is whether the BUSINESS END clears, and the
     * rest of the cue is somewhere out over the rail past the point anyone
     * cares. A deliberate compromise, in the direction of feeling right. */
    (void)dd;
    return CUE_TIP_R;
}

/* Height of whatever is solid under (x,z). Off the table entirely returns a
 * large negative, meaning "nothing below you here". */
static float cue_table_surface(const CueTable *t, float x, float z) {
    float px = t->half_len, pz = t->half_wid;              /* cushion nose */
    float ox = px + t->rail_w, oz = pz + t->rail_w;        /* outer frame edge */
    float ax = fabsf(x), az = fabsf(z);
    if (ax <= px && az <= pz) return 0.0f;                 /* open cloth */
    if (ax > ox || az > oz) return -1.0e9f;                /* past the frame */

    /* EXACTLY WHAT IS DRAWN, and nothing on top of it.
     *
     * cue_render.c builds the cushion top and the wood top level with each
     * other at cushion_h * 1.30 — the rail cap is NOT a step above the cushion,
     * they are one surface. (cushion_h itself is the ball-CONTACT line at 63.5%
     * of ball height, which is a different thing again and easy to mistake for
     * the top of the cushion.)
     *
     * What was wrong here was an extra 0.085R — 2.2 mm — added above that. It
     * had the cue clearing a rail 2.2 mm taller than the one on screen, and on
     * the one shot where every millimetre counts, the white frozen on the
     * cushion with the shaft laid across it, 2.2 mm is most of the window. The
     * cue is a cylinder resting on a surface: its centreline sits one shaft
     * radius above, which cue_elev_for already adds, and there is nothing left
     * to account for. */
    return t->cushion_h * 1.30f;
}

/* Elevation needed for the shaft to sit above `surf` at distance `dd` back. */
static float cue_elev_for(float tip_y, float dd, float surf) {
    if (surf < -1.0f || dd <= 1.0e-4f) return 0.0f;
    float want = surf + cue_shaft_r(dd);
    if (want <= tip_y) return 0.0f;
    return atan2f(want - tip_y, dd);
}

float cue_table_min_elev(const CueTable *t, const CueBall *balls, int n,
                         Vec3 tip, float aim) {
    const float R = t->R;
    float bx = -cosf(aim), bz = -sinf(aim);      /* the stick runs BACK from the tip */
    float need = 0.0f;

    /* Walk back along the cue. Sampling alone can step over the rail edge, which
     * is the one place the answer jumps, so the four boundary crossings are
     * measured exactly and added to the samples. */
    const int NS = 24;
    for (int k = 1; k <= NS; k++) {
        float dd = CUE_SHAFT_REACH * (float)k / (float)NS;
        float e = cue_elev_for(tip.y, dd,
                               cue_table_surface(t, tip.x + bx*dd, tip.z + bz*dd));
        if (e > need) need = e;
    }
    {   /* the nose lines and the frame edges, hit exactly */
        float edge[4];
        edge[0] = t->half_len; edge[1] = t->half_wid;
        edge[2] = t->half_len + t->rail_w; edge[3] = t->half_wid + t->rail_w;
        for (int q = 0; q < 4; q++) {
            float lim = edge[q];
            float cand[2]; int nc = 0;
            if (q == 0 || q == 2) {
                if (bx >  1.0e-4f) cand[nc++] = ( lim - tip.x) / bx;
                if (bx < -1.0e-4f) cand[nc++] = (-lim - tip.x) / bx;
            } else {
                if (bz >  1.0e-4f) cand[nc++] = ( lim - tip.z) / bz;
                if (bz < -1.0e-4f) cand[nc++] = (-lim - tip.z) / bz;
            }
            for (int m = 0; m < nc; m++) {
                float dd = cand[m];
                if (dd <= 1.0e-4f || dd > CUE_SHAFT_REACH) continue;
                /* sample a whisker either side: the tall side is what binds */
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    float d2 = dd + (float)sgn * 0.002f;
                    if (d2 <= 1.0e-4f || d2 > CUE_SHAFT_REACH) continue;
                    float e = cue_elev_for(tip.y, d2,
                                cue_table_surface(t, tip.x + bx*d2, tip.z + bz*d2));
                    if (e > need) need = e;
                }
            }
        }
    }

    /* Balls. You go over them, never through, and the stick's own thickness
     * counts here too. balls[0] is the cue ball and is never an obstacle. */
    for (int i = 1; i < n; i++) {
        if (!balls[i].on) continue;
        float dx = balls[i].pos.x - tip.x, dz = balls[i].pos.z - tip.z;
        float along = dx*bx + dz*bz;
        if (along <= 0.0f || along > CUE_SHAFT_REACH) continue;
        float perp2 = (dx*dx + dz*dz) - along*along;
        if (perp2 < 0.0f) perp2 = 0.0f;
        float room = R + cue_shaft_r(along);
        if (perp2 < room*room) {
            /* A BALL IS A SPHERE. This asked for clearance over 2R — the very
             * crown — for any shaft passing within a ball's width of the
             * centre, so a cue skimming the far edge of a ball was lifted as
             * high as one going straight over the middle of it. That is a
             * cylinder, or near enough a cube, and it is why the planner
             * thought whole classes of ordinary shot needed the butt in the air.
             *
             * The real condition is the obvious one: the shaft's axis must stay
             * at least (R + shaft radius) away from the ball's centre in THREE
             * dimensions. With the axis at horizontal offset `perp` and height
             * y, that distance squared is perp^2 + (y - R)^2, so
             *
             *     y >= R + sqrt((R + r)^2 - perp^2)
             *
             * which gives 2R + r straight over the middle — the old answer, in
             * the one case it was right for — and falls smoothly to R at the
             * edge, where a cue alongside a ball needs no lift at all. */
            float need_y = R + sqrtf(room*room - perp2);
            if (need_y > tip.y) {
                float e = atan2f(need_y - tip.y, along);
                if (e > need) need = e;
            }
        }
    }

    if (need > CUE_ELEV_MAX) need = CUE_ELEV_MAX;
    return need;
}
