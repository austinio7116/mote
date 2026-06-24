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
        t->pr_corner  = 2.15f * t->R; t->pr_side  = 1.95f * t->R;
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
        t->pr_corner  = 2.75f * t->R; t->pr_side  = 2.35f * t->R;
        t->gap_corner = 3.20f * t->R; t->gap_side = 2.95f * t->R;   /* corners opened a touch */
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
        const float ca = 0.6f, ma = 0.7f; const int nc = 3, nm = 3;
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
Vec3 cue_table_clamp_placement(const CueTable *t, Vec3 p) {
    float R = t->R;
    if (t->is_snooker || t->kind == CUE_GAME_UK8) {
        /* the D: a half-disc of radius d_radius centred on (baulk_x,0), bulging
         * toward the baulk cushion (−x). Keep the ball wholly inside it. */
        float rmax = t->d_radius - R;
        if (rmax < 0.0f) rmax = 0.0f;
        if (p.x > t->baulk_x) p.x = t->baulk_x;        /* not past the baulk line */
        float dx = p.x - t->baulk_x, dz = p.z;
        float d = sqrtf(dx*dx + dz*dz);
        if (d > rmax) { float s = rmax / d; p.x = t->baulk_x + dx*s; p.z = dz*s; }
        return p;
    }
    /* US pool: behind the head string (at quarter table from the baulk end). */
    float head = -t->half_len * 0.5f;
    float lim = t->half_wid - R;
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
    set_ball(&b[0], CUE_ID_CUE, -t->half_len * 0.5f, 0.0f, R);
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
    set_ball(&b[0], CUE_ID_CUE, -t->half_len * 0.5f, 0.0f, R);
    return 10;
}

static int rack_snooker(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    set_ball(&b[n++], CUE_ID_CUE, t->baulk_x, -t->d_radius * 0.4f, R);
    set_ball(&b[n++], CUE_ID_YELLOW, t->baulk_x, -t->d_radius, R);   /* yellow = right of the D */
    set_ball(&b[n++], CUE_ID_GREEN,  t->baulk_x, +t->d_radius, R);   /* green  = left of the D */
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

int cue_table_rack(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    if (t->is_snooker)            return rack_snooker(t, balls);
    if (t->kind == CUE_GAME_US9)  return rack_9ball(t, balls);
    return rack_pool(t, balls);   /* UK8 + US8 */
}
