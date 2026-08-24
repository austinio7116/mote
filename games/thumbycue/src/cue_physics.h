/*
 * ThumbyCue — billiard physics.
 *
 * SI units everywhere (metres, kilograms, seconds). Table plane = world
 * X–Z, Y up; cloth at Y = 0, ball centre at Y = R (v1 is planar — no jumps,
 * vel.y and pos.y are pinned). Each ball carries full 3-D angular velocity
 * `w` so spin physics is real: the horizontal components are top/back/roll,
 * the vertical (Y) component is side / "english".
 *
 * Motion is a fixed-substep integrator (≈2 kHz) that an accumulator drives
 * from the frame dt, so behaviour is frame-rate independent. Per substep,
 * each ball is in one of three cloth-contact regimes derived from the
 * contact-point velocity u = vel + w × r_c  (r_c = (0,−R,0)):
 *   sliding  — kinetic friction decelerates vel AND torques w (this is what
 *              makes draw / follow / stun develop the correct roll);
 *   rolling  — light rolling resistance; w slaved to the rolling constraint;
 *   spinning — residual vertical spin decays on its own (side carries).
 *
 * Collisions are impulse-based with Coulomb friction and the sphere's
 * rotational inertia, so ball–ball throw and english-off-the-cushion fall
 * out of the same framework rather than being faked.
 */
#ifndef CUE_PHYSICS_H
#define CUE_PHYSICS_H

#include "mote_vec.h"
#include "mote_phys.h"        /* the skittles are rigid bodies; see sk[] below */   /* engine's Vec3/Mat3 + helpers (identical to ThumbyElite's vec.h) */
#include <stdint.h>

#define CUE_MAX_BALLS   22   /* snooker: cue + 15 reds + 6 colours */
#ifndef CUE_MAX_SEG
#define CUE_MAX_SEG     128  /* cushion nose segments (snooker uses curved jaws).
                              * RAISED FROM 96 FOR F2: an L has six vertices and
                              * eight pockets against a rectangle's four and six,
                              * so its chain is half as long again. 96 fitted
                              * every rectangle with room to spare and fits no L
                              * at all — and the overflow is silent, because
                              * add_seg simply stops adding when it is full,
                              * which draws a table with a wall missing. */
#endif
#define CUE_MAX_JAW     24   /* bed-boundary knuckle points */
#define CUE_MAX_POCKET  12   /* six on a rectangle; an L has five convex corners
                              * and two middles, which is eight; and bar
                              * billiards has NINE, none of them on a rail. */
#define CUE_MAX_RECT     4
/* Corners a regular bed may have. Eight is the largest the shapes list asks
 * for; the rest is headroom for a round bed's approximating polygon. */
#define CUE_MAX_BEDV    64

/* An axis-aligned box in table space, x0<x1 and z0<z1. See CueWorld.play_r. */
typedef struct { float x0, x1, z0, z1; } CueRect;

/* Is (x,z) inside any of them? The whole of F2's boundary question, and the
 * reason the shortcut is worth taking: for the one-rectangle case this is four
 * compares, which is what the half-extent test was. */
static inline int cue_rects_contain(const CueRect *r, int n, float x, float z) {
    for (int i = 0; i < n; i++)
        if (x >= r[i].x0 && x <= r[i].x1 && z >= r[i].z0 && z <= r[i].z1) return 1;
    return 0;
}
/* In CueBall.pocket: this ball did not go down, it went OFF. The two look
 * identical to everything downstream — a ball that was on and now is not — and
 * they are not remotely the same thing to the rules, so they have to be told
 * apart at the only point that knows. */
#define CUE_OFF_TABLE  0xFFu

typedef struct {
    Vec3 pos;        /* world metres (y = R) */
    Vec3 vel;        /* m/s (y = 0) */
    Vec3 w;          /* angular velocity rad/s (world) */
    Mat3 orient;     /* render orientation, integrated from w */
    uint8_t on;      /* 1 = on the table (incl. mid-drop), 0 = gone */
    uint8_t id;      /* ball number / colour code (game-defined) */
    uint8_t pocket;  /* if potted: which pocket index it fell in, or
                      * CUE_OFF_TABLE if it left over a cushion instead */
    /* WHICH BED HOLE THIS BALL IS CURRENTLY OVER, plus one, or 0 for none.
     * A bar billiards hole is judged ONCE, on the step the ball's centre
     * crosses into it (see check_pockets): a ball crossing at pace rims out,
     * and without this it would be re-judged every step while it was still
     * inside the circle and eventually swallowed anyway. */
    uint8_t over_hole;
    float drop;      /* >0 while falling into the pocket (seconds remaining);
                      * still renders (sinking) but is out of play */
    /* How long it has been off the cloth. A ball can legitimately spend a
     * moment on the rail; one that has come to rest up there is not coming
     * back, and a frame cannot wait on it for ever. */
    float astray;
    /* THIS BALL'S OWN SIZE AND WEIGHT, where it differs from the set.
     *
     * English pool is played with a cue ball smaller than the object balls —
     * 47.6 mm against 50.8 — a convention that comes from coin-op ball returns
     * and is still what a pub table has in it. Everything else in these games
     * uses one size, so ZERO MEANS "the same as the rest": every ball that is
     * memset and handed to the engine keeps working untouched, and only the
     * odd one out has to say so. */
    float r;         /* radius, metres. 0 = the world's R */
    float m;         /* mass, kg.       0 = the world's mass */
} CueBall;


/* One thing the cue ball touched, in the order it touched it. See CueWorld. */
enum { CUE_TOUCH_BALL = 0, CUE_TOUCH_CUSHION };
#define CUE_MAX_TOUCH 24
/* Three on a bar billiards table, and nothing else has any. */
#define CUE_MAX_SKITTLE 4
/* HOW THE RIGID-BODY STEP IS REACHED. On the handheld the engine owns the
 * solver and a game calls it through the ABI (MoteApi::phys_step); CueVR and
 * the tests link mote_phys.c and pass mote_phys_step itself. So the physics
 * takes a pointer rather than a dependency, and a front-end that sets none
 * simply gets skittles that stand still. */
/* HOW BIG THE SOLVER'S POOLS MUST BE. The pins are not the only bodies: the
 * bed and the four cushions are static planes in the same array, and mote
 * indexes its per-body scratch by body number — so a caller that sized the
 * pools by the skittle count alone was writing past them, and the symptom was
 * a pin that took a hit and then declined to move. Sized here, once, so no
 * caller has to know how many planes there are. */
#define CUE_SKITTLE_PLANES   5
#define CUE_SKITTLE_BODIES   (CUE_MAX_SKITTLE + CUE_SKITTLE_PLANES)
/* Every hull vertex against every plane it is touching, with room to spare. */
#define CUE_SKITTLE_CONTACTS 192
/* ...and the arena those pools come out of. mote sizes a warm-start cache at
 * the next power of two above twice the contact count, so this is not the
 * small number the body count suggests — it is worth stating rather than
 * leaving each caller to guess and get "the pools did not fit". */
#define CUE_SKITTLE_ARENA   (96 * 1024)

typedef uint32_t (*CuePhysRigidFn)(MoteWorld *w, MoteBody *b, int n, float dt);
void cue_phys_set_rigid(CuePhysRigidFn fn);
/* How hard a bar billiards skittle topples, in 1/s^2: m g d / I about the foot,
 * for 12 g with its centre of gravity 76.5 mm up (a 7 g mushroom head at 99 mm
 * on a 5 g stem). A uniform rod of the same length would be 3g/2L = 129.0; the
 * head makes this one fall more slowly, and heavily. */
#define CUE_SKITTLE_FALL 109.6f
typedef struct {
    uint8_t what;   /* CUE_TOUCH_* */
    uint8_t id;     /* the ball's id, for CUE_TOUCH_BALL */
    uint8_t idx;    /* ...and its index, since snooker reds share an id */
    uint8_t _pad;
} CueTouch;

/* A cushion nose segment in the X–Z plane with an inward unit normal
 * (pointing into the playable area). Rails and pocket facings are both built
 * from these. kind: 0 = straight rail nose, 1 = pocket facing/jaw.
 * na/nb are the smooth (vertex-averaged) normals at the a/b ends, so the
 * collision normal can be interpolated along the segment — a continuous normal
 * field across the whole chain (no kink at the rail↔facing junction). */
typedef struct { Vec3 a, b, n, na, nb; uint8_t kind; } CueSeg;

typedef struct {
    /* Ball / cloth. */
    float R, mass, g;
    float mu_s;       /* sliding (ball–cloth kinetic) friction */
    float mu_r;       /* rolling resistance */
    float spin_decel; /* vertical-spin angular deceleration (rad/s^2) */
    /* Ball–ball. */
    float e_bb;       /* restitution */
    float mu_bb;      /* friction (throw) */
    /* Cushion. */
    /* Cushion restitution at a CRAWL, and how fast it falls off with the
     * approach speed — see cushion_impact. Real rails are livelier the more
     * gently they are touched: 0.91 at nothing, about 0.82 across normal play,
     * and down near 0.55 on a firm one. A single number could not express that
     * and made every rail play the same at any pace. */
    float e_cush;     /* restitution at zero approach speed */
    float cush_efall; /* how much it drops per m/s of approach */
    float e_cush_min; /* and where it stops falling */
    float mu_cush;    /* rail friction (deflects the bounce; incoming english still bends it) */
    float cush_spin;  /* 0..1: how much of the rail friction impulse becomes NEW spin on the
                       * ball. <1 means the cushion imparts less spin while the bounce-angle
                       * effect of incoming spin is preserved (asymmetric). */
    float cush_tilt;  /* contact-normal tilt from horizontal (rad), from nose height */
    /* Bed. Only a jumped ball ever touches these. Cloth over slate is a poor
     * trampoline: a jumped ball takes two or three diminishing hops and stops,
     * and the settle speed is what stops it micro-bouncing for hundreds of
     * substeps — every bounce suspends cloth friction, so a ball that never
     * quite lands never develops roll and its draw and follow stop meaning
     * anything. */
    float e_bed;      /* bed restitution (~0.4) */
    float v_land;     /* below this downward speed a landing settles flat */
    float cushion_nose; /* nose height above the cloth: a ball whose underside
                         * is above this has cleared the rail (filled by
                         * cue_table from CueTable.cushion_h) */
    /* THE EDGE OF THE WORLD, at the outer face of the rail. Only a jumped ball
     * can ever reach it: everything on the cloth is held by the cushions. It
     * exists because a ball that clears a cushion has nothing beyond it to stop
     * it, and a ball rolling to infinity is a shot that never settles. Past
     * this it is simply off the table, which is what it would be in the room. */
    float bound_x, bound_z;
    /* ---- F2: THE BED AS A SHAPE, NOT AS TWO NUMBERS ----------------------
     *
     * bound_* and play_* above are half-extents, and a half-extent can only
     * describe a rectangle centred on the origin. Collision never needed them
     * — cue_phys_step does not reference half_len or half_wid at all, because
     * the cushions are a generic chain — but four things outside it did, and
     * every one of them would let a ball through the wall of any other shape:
     * the world edge a jumped ball is deleted at, the rail-top surface it can
     * land on, the stuck-ball test, and the placement clamp.
     *
     * A UNION OF AXIS-ALIGNED RECTANGLES answers all four cheaply, and it is
     * exactly what an L is. General convex polygons are S2's problem and cost
     * roughly double; this buys the L and leaves the fast paths fast, because
     * one rectangle is still one compare in each axis.
     *
     * The half-extents stay, and stay correct: they are the BOUNDING box of the
     * union now rather than the shape itself, which keeps them useful as a
     * first reject and keeps every existing reader honest — a bounding box is
     * never smaller than the shape, so nothing that used them to ask "could
     * this possibly be on the table" gets a wrong answer. */
    /* THE CLOTH, AS RECTANGLES OR AS A CONVEX OUTLINE.
     *
     * Rectangles answer a rectangle and an L in two compares, which is why they
     * are here. A regular bed is not a union of rectangles at all, so it brings
     * its own outline instead and `nbedv` says which of the two to believe.
     * Convex, always: every regular polygon is, so "inside" is "inside all N
     * edges" and there is no winding rule to get wrong. */
    float  bedv_x[CUE_MAX_BEDV], bedv_z[CUE_MAX_BEDV]; int nbedv;
    CueRect play_r[CUE_MAX_RECT];  int nplay;    /* the cloth */
    CueRect bound_r[CUE_MAX_RECT]; int nbound;   /* out to the frame edge */
    /* AND WHAT IT LANDS ON BETWEEN THE TWO. A ball that clears a cushion is
     * over the rail, and the rail is a surface: it can come down on it, run
     * along it, drop back onto the cloth or fall off the outside. Removing it
     * the instant it passed the cushion line — which is what the first version
     * did — deletes a shot that is still happening.
     *
     * play_* is the cushion nose line, rail_top the height of the cushion and
     * wood cap, which cue_render builds level with each other as one surface. */
    float play_x, play_z;
    float rail_top;
    /* HOW DEEP THE CUSHION IS, nose to back — and therefore where the frame's
     * inner edge is, since cue_render puts the timber exactly at the cushion
     * back. The jaw needs it: its outward reach was struck off the BALL radius
     * (1.4R at a corner, 1.6R at a middle) while the frame edge is struck off
     * the RAIL width, so the two were unrelated numbers and the jaw curve
     * stopped 11.7mm short of the timber at a UK corner and 6.6mm short at a
     * middle. That shortfall is the gap beside every pocket. */
    float cush_depth;

    /* Geometry (filled by cue_table). */
    CueSeg seg[CUE_MAX_SEG]; int nseg;
    /* HOW MANY SEGMENTS EACH JAW MAY SPEND. A rectangle has six runs and can
     * afford the full CUE_JAW_SEGS; a sixty-sided bed has sixty and cannot —
     * at 2*JAW+1 per run it wants 1260 segments against an array of 256, and
     * what actually happened is that the array filled and the LAST edges got
     * no cushion at all. A twelve-gon was already on the edge of it: 252
     * needed, 256 available, and two of its twelve rails came out bare.
     *
     * Spending the budget instead of overrunning it: the jaw gets coarser as
     * the bed gains sides, and every rail keeps its cushion. Coarse knuckles
     * on a thirty-gon are a shape nobody will look at closely; a missing
     * cushion is a wall a ball goes through. */
    int jaw_segs;
    /* The rounded jaw's shape, copied off the table — see CueTable::jaw_p0.
     * On the world because the curve is built here and the segments it makes
     * ARE the physics; there is only one curve and both sides read it. */
    float jaw_p0, jaw_p0_m, jaw_h1, jaw_h2, jaw_ang_c, jaw_ang_m;
    /* WHICH POCKET KINDS THE CUSHIONS ACTUALLY REACHED: bit 0 corner, bit 1
     * middle. A pocket small enough stops reaching: the bore no longer crosses
     * the frame's inner edge, so there is no point to stand a cushion tip on
     * and the gap stays at its seed. Where that happens is a property of the
     * TIMBER — the rail width and the setback decide it — not of the ball; on
     * a UK 7ft with its 75mm rail it is somewhere under 30mm of pocket, and a
     * 25mm one there reported 192mm across. Anything quoting a pocket size has
     * to check this first, because the wrong number looks perfectly plausible. */
    int linked;
    Vec3   jaw[CUE_MAX_SEG]; int njaw; float jaw_r;   /* immovable jaw-tip circles */
    Vec3   pocket[CUE_MAX_POCKET]; float pocket_r[CUE_MAX_POCKET]; int npocket;
    /* EACH POCKET'S OWN CENTRE LINE, pointing out of the pocket, recorded when
     * the pocket is placed.
     *
     * Every shape already works this out in order to offset the pocket from its
     * corner — a rectangle's 45 degree bisector, a polygon's radial, the L's
     * bisector of two edge normals — and then threw it away. The jaw needs it:
     * the bore is set back along it and the facing arrives down it. Guessing it
     * as "45 degrees off the rail normal" is right for a rectangle and a square
     * and wrong for every other polygon, which is why a triangle's pockets came
     * out at 1.19 ball widths against a square's 1.60.
     *
     * Not pmnorm: that is derived from the finished jaws, so it does not exist
     * while they are being built. */
    Vec3   paxis[CUE_MAX_POCKET];
    /* WHICH BALLS HAVE FULLY CROSSED the line between the middle pockets,
     * toward baulk, this stroke — a bitmask by ball index, reset by
     * cue_phys_shot_begin. Blackball's break legality (WPA 4b: two object
     * balls must fully pass it) is the customer; tracked while att_track is
     * on, like the rest of the referee's instruments. */
    uint32_t brk_cross;
    /* WHAT A HOLE IS WORTH. Zero on every table where a pocket is a pocket;
     * bar billiards is the one game whose holes are not interchangeable — nine
     * of them scoring from ten to two hundred, and which one a ball went down
     * IS the score. Filled in by cue_table_build_world beside the pocket. */
    int16_t pocket_score[CUE_MAX_POCKET];
    /* A HOLE IN THE MIDDLE OF THE BED, not a cut in its edge.
     *
     * Every other pocket in these games is a bite taken out of the slate's
     * boundary, which is why cue_phys_cut_out models one as an arc with two
     * legs running out to the rail. Bar billiards' nine holes are cut in the
     * OPEN BED with cloth all the way round them, and run through that same
     * function those legs claimed half the table as "no cloth here" — which
     * is exactly the reported fault of pockets gathering balls from nowhere
     * near them. A bed hole is a circle and nothing else. */
    uint8_t pocket_bed[CUE_MAX_POCKET];
    /* HOW DEEP A BALL MUST FALL, as a fraction of its own radius, before the
     * far lip of a bed hole can no longer throw it back out. A ball crossing
     * a hole is unsupported for the width of the hole and falls under gravity
     * for exactly that long; if its centre is still above the bed when it
     * reaches the far lip, the lip is below its equator and kicks it up and
     * onward — it rims out. That is why a bar billiards ball has to be rolled
     * into a hole rather than driven at it. */
    float hole_catch;
    /* IS THIS A MIDDLE POCKET? Carried rather than inferred from the index.
     * "p < 4 is a corner" was true of every rectangle and is written into the
     * drop-back choice, the AI's difficulty model and the HUD's pocket names —
     * and an L has FIVE corners, so all three would have called its fifth
     * corner a middle and priced it as one. */
    unsigned char pocket_mid[CUE_MAX_POCKET];
    /* THE MOUTH OF EACH POCKET: the midpoint of the line between its two jaw
     * tips, and the unit normal of that line pointing INTO the pocket. Past
     * that line the ball is in the throat and the only way back is out through
     * the mouth again — which is what the jaws and the back actually are. A
     * radius cannot express this: the opening is bounded by two points, and
     * everything else around it is solid. Filled in by cue_table_build_world
     * once the jaws are placed. */
    Vec3   pmouth[CUE_MAX_POCKET];
    Vec3   pmnorm[CUE_MAX_POCKET];
    /* WHERE THE SLATE ENDS, which is where the ball starts to fall.
     *
     * Not the jaw line. The cloth is cut away around each pocket in an arc with
     * two straight tangent legs, and the ball tips over the edge of THAT — at a
     * middle it happens well before the jaws, at a corner slightly after them.
     * The renderer draws this same curve, so one set of numbers decides both
     * what you see and where the ball goes. `lip_d` is how far the cloth rolls
     * over that edge before it turns vertical: a quarter circle of that radius,
     * out and down. Filled in by cue_table_build_world. */
    Vec3   cut_c[CUE_MAX_POCKET];   /* arc centre (the pocket, set back a little) */
    float  cut_r[CUE_MAX_POCKET];   /* arc radius */
    float  lip_d[CUE_MAX_POCKET];   /* the roll over the edge */
    /* The four tunables behind those, [0] = corners, [1] = middles. See CueCut
     * in cue_table.h, which is the shape of them and the only thing that should
     * be writing them. */
    float  cut_set[2], cut_rad[2], cut_roll[2], cut_arc[2];
    /* What `rad` and `roll` are multiples of: the VISIBLE mouth (pr_corner /
     * pr_side), not pocket_r — those are different radii and the drop circle is
     * the smaller of them. [0] corners, [1] middles. */
    float  cut_ref[2];
    /* ...and how far past that edge a point is: negative on cloth, zero at the
     * edge, positive out over the drop. The tuning screen asks this to stand a
     * ball exactly on the brink. */
    /* WHERE THE DROP CIRCLE IS CENTRED, which is not where the pocket is drawn.
     *
     * A real pocket takes the ball when it is far enough IN, and how far in is
     * not the same as how big the opening is — the two want moving separately
     * or the drop can only ever be a circle concentric with the hole. So the
     * centre of the drop is the pocket pushed this far further along the line
     * out through its own mouth. Zero puts it back on the pocket, which is
     * where it has always been. */
    float  drop_back;       /* CORNER drop pushed this far deeper (m) */
    float  drop_back_side;  /* MIDDLE drop pushed this far deeper (m) */
    Vec3   drop_c[CUE_MAX_POCKET];   /* derived: pocket + pmnorm * drop_back */

    /* First object ball the CUE ball contacts after a strike; -1 = none.
     * Reset by cue_phys_shot_begin(); read at settle for rules.
     * first_hit = ball id (for rules); first_hit_idx = ball index (for the
     * follow-camera, since snooker reds share an id). */
    int first_hit;
    int first_hit_idx;

    /* ---- WHAT THE ATTEMPT LOOKED LIKE, for the referee ------------------- *
     *
     * Foul and a miss is a judgement about EFFORT: did the stroke have the pace
     * to reach the ball on, and did its line pass close? Neither is knowable
     * from the settle alone — a cue ball that finished by the ball on may have
     * gone there the long way round, and one that finished in baulk may have
     * shaved it on the way. So the integrator keeps, per stroke:
     *
     *   att_min[i]  the closest the CUE ball's centre came to ball index i
     *   att_path    how far the cue ball actually travelled, in metres
     *
     * Reset by cue_phys_shot_begin, like first_hit. Indexed by ball INDEX, not
     * id, because snooker reds share an id. */
    float att_min[CUE_MAX_BALLS];
    float att_path;
    Vec3  att_prev;
    int   att_prev_ok;
    /* OFF UNLESS A REFEREE IS WATCHING. The log costs a length per ball per
     * step; nothing on a live table, but cue_phys_step also runs inside the
     * AI's planning at hundreds of candidate strokes a turn, and the referee's
     * instrument has no business in a simulation nobody will judge. (An early
     * version of this comment blamed the log for test_ai_frames' running time;
     * measured properly, that test is simply slow — sixty whole frames of
     * snooker — and was as slow before the log existed. The gate stays because
     * it is right, not because it was the cost.) The host arms it on the world
     * the referee actually judges; scratch worlds never do. Survives
     * shot_begin, because it is a property of the world, not of a stroke. */
    int   att_track;

    /* ---- WHAT THE CUE BALL TOUCHED, IN ORDER ---------------------------- *
     *
     * first_hit answers "what did it hit first", which is every question the
     * games shipped so far need to ask. No billiards game can be scored from
     * it. A cannon is "the cue ball contacted BOTH object balls"; a three-
     * cushion carom is that "with three or more cushions before the second
     * one". Those are questions about a SEQUENCE, and only the integrator ever
     * sees it — by settle the balls have stopped and the order is gone.
     *
     * So the cue ball keeps an account of its own shot. ONLY the cue ball's
     * contacts: the object balls' collisions with each other are numerous and
     * no rule in any of these games asks about them. That keeps this small
     * enough to be a fixed array, which it has to be — the match is lockstep,
     * so this must fill identically on both machines with no allocation.
     *
     * `touch_over` is set if a shot had more contacts than fitted. It should
     * not happen on a real stroke (the cue ball is not a break pack), but a
     * count taken from a truncated list is a wrong count, and a caller is
     * entitled to know that rather than be handed a plausible number. */
    CueTouch touch[CUE_MAX_TOUCH];
    int ntouch;
    int touch_over;

    /* ---- THE SKITTLES ---------------------------------------------------
     *
     * Bar billiards stands three wooden pegs on the bed: two white either side
     * of the 100 hole and one black in front of the 200. Knocking a white over
     * costs the break; knocking the black over costs the whole score. They are
     * the entire risk of the game and nothing else here has anything like them.
     *
     * Modelled as circles that a ball KNOCKS DOWN rather than bounces off. A
     * real skittle is 11 cm of light wood on a 15 mm base: a ball that reaches
     * one topples it and carries on, and pretending it is a post to rebound
     * from would be a worse lie than ignoring the deflection. `down` is per
     * shot — the host stands them up again — and it is what the rules read.
     *
     * ORDER MATTERS. Rule 112: if a white falls first the penalty is the
     * break, if the black falls first it is the whole score. So the sequence
     * is recorded, not just the fact. */
    Vec3   skittle[CUE_MAX_SKITTLE];
    uint8_t skittle_black[CUE_MAX_SKITTLE];  /* the fatal one */
    uint8_t skittle_down[CUE_MAX_SKITTLE];   /* knocked over this shot */
    /* AEBBA rule 103: a ball may knock a skittle OFF ITS SPOT without felling
     * it, and that is not a foul — the score counts and the skittle is put back
     * before the next shot. So a touch too gentle to topple one is recorded
     * separately from one that does. */
    uint8_t skittle_nudged[CUE_MAX_SKITTLE];
    /* A SKITTLE IS A RIGID BODY, and mote already has one.
     *
     * It was a rod hinged at its foot: theta from upright, falling under
     * (3g/2L) sin theta. That is the right motion for a pin toppling gently
     * and nothing like the right one for a pin struck at pace, which is
     * knocked off its foot, tumbles, slides and comes to rest somewhere else.
     *
     * The case made earlier for NOT using the engine — that a fallen pin can
     * never reach another pin or a hole — was arithmetic about a HINGE. It
     * assumed the answer. A pin that tumbles crosses the table, so the
     * argument evaporates along with the hinge, and hand-rolling a second
     * rigid-body solver next to the engine's would have been the worst of
     * both.
     *
     * So each pin is a mote capsule: `sk[k]` is the body — centre of mass in
     * `pos`, `orient` turning body space into world with body +Y along the
     * pin — and `sk_world` is the little world it lives in, whose walls are
     * the bed and the cushions. `skittle[k]` stays what it always was: the
     * SPOT the pin belongs on, so it can be stood back up between strokes. */
    /* The pins first, then the five STATIC PLANES they fall about: the bed
     * and the four cushions. Planes rather than mote's own bounding-box
     * walls, because those test a hull as a box of its `half` extent — which
     * a hull does not set, so the box is a point at the centre of mass and a
     * pin sinks to its own middle before anything stops it. gen_vs_plane
     * tests a hull vertex by vertex, which is the shape actually being
     * asked about. */
    MoteBody sk[CUE_SKITTLE_BODIES];
    MoteWorld sk_world;
    int      sk_n;                           /* pins + planes */
    int      sk_on;                          /* the bodies are set up */
    Vec3   skittle_spot[CUE_MAX_SKITTLE];
    uint8_t skittle_order[CUE_MAX_SKITTLE];  /* 1, 2, 3... in the order they fell */
    int    nskittle;
    float  skittle_r;
    /* The pin's own numbers: how long it is, how heavy, and where its centre of
     * mass sits above the foot. A mushroom is top-heavy and that is most of how
     * it falls, so it is not a uniform rod and must not be integrated as one. */
    float  skittle_len, skittle_mass;        /* 114 mm and 12 g of light wood */
    int    skittle_fell;                     /* how many went over this shot */
    /* A side cushion was struck this shot (normal across the table, not along
     * it). AEBBA Rule 108: the last-ball shot must go OFF ONE SIDE CUSHION
     * into the 100 or the 200, and this is the only witness. */
    uint8_t side_cushion;

    /* HOW MANY RAILS EACH BALL HAS TOUCHED THIS SHOT.
     *
     * `touch` is the CUE BALL'S account and nothing else — carom is a game
     * about where the cue ball has been, so that is all it ever needed. BANK
     * POOL asks the opposite question: did the OBJECT ball find a cushion
     * before it dropped, because a ball potted without one does not score and
     * goes back on the table. Nothing in the world could answer that, so this
     * counts it, per ball, from the start of the stroke.
     *
     * Cumulative over the whole shot, which is the rule as written — "contacted
     * a rail before being pocketed" — and a ball stops moving once it drops, so
     * the count at the settle is the count at the drop. */
    uint8_t rails[CUE_MAX_BALLS];

    /* ...AND HOW MANY BALLS EACH ONE TOUCHED, for the same reason.
     *
     * HONOLULU scores nothing for a straight-in pot: the ball has to arrive by
     * a bank, a kick, a combination, a carom or a billiard. `rails` answers the
     * first two between them; this answers the rest — a ball that was set off
     * by another ball, or that struck one on its way, has not gone in straight
     * however clean the line looked. Counted for every ball including the cue
     * ball's own contacts, from the start of the stroke. */
    uint8_t balls_hit[CUE_MAX_BALLS];
    /* ...AND WHETHER THE CUE BALL WAS THE ONE THAT HIT IT.
     *
     * Counting contacts is not enough to tell a combination from a straight
     * pot, and Mark found it in play: the cue ball strikes A, A cannons into B,
     * B drops. B has exactly ONE ball contact — the same count as a ball the
     * cue ball potted directly — so by count alone the combination read as a
     * straight pot and scored nothing. What separates them is not how many hit
     * it but WHO: a ball the cue ball never touched was moved by another ball,
     * and that is a combination however few contacts it had. */
    uint8_t hit_by_cue[CUE_MAX_BALLS];

    /* ---- jump shots, as snooker actually defines one ----------------------
     *
     * WPBSA Section 2, Definition 20: a jump shot is made when the cue-ball
     * PASSES OVER ANY PART of an object ball, whether hitting it in the process
     * or not — except (a) when it first hits one object ball and then jumps
     * over another; (b) when it jumps and hits an object ball and, at the
     * moment of landing, is not on the far side of that ball; or (c) when,
     * after legally hitting an object ball, it jumps over that ball after
     * hitting a cushion or another ball.
     *
     * So the offence is passing over a ball, not leaving the bed — a hop over
     * open cloth is not a jump shot at all, and three quite ordinary things
     * that DO pass over a ball are legal. It is an ordering question, which is
     * why it is answered here rather than in the rules: only the integrator
     * sees what happened between the strike and the settle.
     *
     * `jump_over` is the verdict. Reset it with first_hit at the start of each
     * shot; the rest is working state. */
    int jump_over;        /* an unexcused pass-over happened: this was a jump shot */
    int jump_over_id;     /* the ball it went over, for the referee's line */
    int jmp_pending;      /* mid-flight pass-over, waiting on the landing test for (b) */
    int jmp_idx;          /* which ball that pass-over is of */
    int jmp_hit_it;       /* and whether the cue ball has since contacted it */
    int jmp_bounced;      /* a cushion or another ball since first_hit, for (c) */

    /* Integrator accumulator (do not touch). */
    float _acc;
} CueWorld;

static inline float cue_ball_r(const CueWorld *w, const CueBall *b) {
    return (b->r > 0.0f) ? b->r : w->R;
}
static inline float cue_ball_m(const CueWorld *w, const CueBall *b) {
    return (b->m > 0.0f) ? b->m : w->mass;
}

/* Sensible default constants for the given ball radius/mass. cue_table then
 * fills the geometry arrays. */
void cue_world_defaults(CueWorld *w, float R, float mass);

/* Strike ball b: dir = unit aim direction in world X–Z (y=0); speed in m/s;
 * tip_side / tip_vert = cue-tip contact offset as a fraction of R
 * (+side = right english, +vert = follow/top, −vert = draw/bottom). The
 * miscue limit (|offset| ≲ 0.5R) should be enforced by the caller. */
void cue_phys_strike(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                     float tip_side, float tip_vert);

/* As above, with cue elevation `elev` (radians above horizontal — butt raised,
 * cueing down on the ball). Side spin + elevation curves the path (swerve). */
void cue_phys_strike_elev(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                          float tip_side, float tip_vert, float elev);

/* As above, and the ball LEAVES THE BED at `vy` metres per second.
 *
 * The caller computes vy, not this function, and that is deliberate twice over.
 *
 * First, the table forces the cue up. cue_table_min_elev raises the shaft to
 * clear the rail, and near a cushion that is thirty degrees with no intent from
 * anybody — so a jump measured off ABSOLUTE elevation launches the ball on
 * every shot played off a cushion. It has to be measured off the elevation the
 * player added BEYOND what the table demands, and only the caller knows that.
 *
 * Second, lockstep. Two machines that each derive vy from their own
 * reconstruction of min_elev can disagree about whether the ball left the bed
 * at all, which is not a divergence any amount of position correction repairs.
 * One number crosses the wire and both ends jump or neither does.
 *
 * vy = 0 is exactly cue_phys_strike_elev, bit for bit. That matters: it is what
 * keeps every shot anyone has already learned unchanged. */
/* Cue-ball deflection (squirt) at full side, radians. ~1.4 degrees: low-deflection
 * territory, which is where a decent modern shaft sits, and low enough that it
 * reads as character rather than as the aim being unreliable.
 *
 * IT IS PART OF THE SHOT, NOT AN ERROR. Anything that applies side and wants to
 * arrive where it aimed must aim off by tip_side * this, in the opposite
 * direction — a player does it without thinking and the AI has to do it on
 * purpose. Cueing side without allowing for it puts the cue ball a centimetre
 * wide over the length of a table, which is the difference between clipping the
 * outside of the pack and missing it. */
#define CUE_SQUIRT_RAD 0.025f

void cue_phys_strike_jump(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                          float tip_side, float tip_vert, float elev, float vy);

/* Is this ball off the bed right now? Rules ask, to price a ball that has left
 * the table, and the renderer asks nothing — it just draws pos.y. */
int cue_phys_airborne(const CueWorld *w, const CueBall *b);

/* How far past the edge of the cut cloth a point is at pocket p: negative still
 * on slate, zero at the edge, positive out over the drop. This is the line the
 * ball tips over, so it is also the line to stand a ball on when judging the
 * drawn pocket against the played one. */
float cue_phys_cut_out(const CueWorld *w, int p, float x, float z);

/* Advance the simulation by dt seconds. Returns 1 while any ball is still
 * moving, 0 once the table has settled. `events` (optional) receives a
 * bitwise OR of CUE_EV_* for sound/feedback this call. */
enum {
    CUE_EV_BALL_HIT  = 1 << 0,   /* ball–ball contact */
    CUE_EV_CUSHION   = 1 << 1,   /* ball–cushion contact */
    CUE_EV_POCKET    = 1 << 2,   /* a ball was potted */
    CUE_EV_JAW       = 1 << 3,   /* ball rattled a jaw */
    CUE_EV_BED       = 1 << 4,   /* a jumped ball came down on the slate */
    CUE_EV_SKITTLE   = 1 << 5,   /* a bar billiards skittle went over */
    CUE_EV_SIDE_CUSH = 1 << 6,   /* ...and the cushion struck was a SIDE one:
                                  * how the flag crosses the const collision
                                  * path to be booked on the world by the step */
};
/* Is (x, z) on the cloth? Reads the outline when the world carries one and the
 * rectangles otherwise, so callers need not know which kind of bed it is. */
int cue_world_on_bed(const CueWorld *w, float x, float z);
/* ...and whether a whole BALL of radius r fits there. The point test is what a
 * rolling ball is judged by; this is what anything being PLACED must use, or a
 * centre a hair inside the cushion line puts most of the ball in the rubber. */
int cue_world_ball_on_bed(const CueWorld *w, float x, float z, float r);

int cue_phys_step(CueWorld *w, CueBall *balls, int n, float dt, uint32_t *events);
float cue_phys_cushion_impact(void);   /* loudest rail-approach speed from last step */

/* Override the integrator substep (0 = restore the default 2 kHz CUE_H). The AI
 * uses a coarser step for its headless ranking sims to run ~2x faster. */
void cue_phys_set_substep(float h);

int cue_phys_moving(const CueWorld *w, const CueBall *balls, int n);

/* ---- one shot begins ------------------------------------------------------
 *
 * Clears everything the LAST shot recorded: the first contact, the jump
 * verdict, and the cue ball's touch log.
 *
 * It exists because that reset was written out by hand at seven call sites —
 * the game loop, the AI's ranking sims, the VR app and four tests — each
 * setting the fields it happened to know about. Every field added to the
 * per-shot state since has had to be added to all seven, and the failure when
 * one is missed is a stale reading attributed to the current shot, which looks
 * like a rules bug and is not one. One call, and a field added here is cleared
 * everywhere at once. */
void cue_phys_shot_begin(CueWorld *w);
/* Stand the skittles up as rigid bodies and give them the little world they
 * fall about in: the bed is its floor, the cushions its walls. Called by
 * cue_table_build_world once the pins and the bed are known. */
void cue_phys_skittles_init(CueWorld *w, float half_len, float half_wid);
/* Stand the skittles back on their spots and forget what this stroke did to
 * them. Called by the host AFTER cue_rules_resolve — the rules have to see the
 * fallen pins to price the stroke, and standing them up any earlier is why
 * they used to right themselves as the next shot was struck. */
void cue_phys_skittles_respot(CueWorld *w);

/* ---- reading the cue ball's account of the shot ---------------------------
 * All are safe to call at any time; they describe the shot so far. */
int cue_touch_count(const CueWorld *w);
int cue_touch_get(const CueWorld *w, int i, CueTouch *out);

/* How many cushions the cue ball struck, over the whole shot. */
int cue_touch_cushions(const CueWorld *w);

/* Did the cue ball contact both of these balls, in either order? That is a
 * CANNON, and it is what English billiards scores two for. Ids, not indices,
 * so it reads the way the rule does. */
int cue_touch_cannon(const CueWorld *w, int id_a, int id_b);

/* How many cushions the cue ball struck BEFORE it reached the second distinct
 * object ball — the whole of the three-cushion carom rule. Returns -1 if it
 * never reached a second ball at all, which is a different answer from zero
 * and must not be confused with it. */
int cue_touch_cushions_before_second_ball(const CueWorld *w);

/* THE MISCUE LIMIT: how far off centre a tip can strike, as a fraction of the
 * ball's radius, and with it the most spin anybody can put on a ball.
 *
 * Half a ball (0.50) is the accepted figure and gives omega*R/v = 1.25.
 * Alciatore measured 0.55 as reachable "with some effort", which is where this
 * sits — the ceiling a good player can actually get to rather than the textbook
 * one, worth 1.375. Note that his chalk comparison found NO measurable
 * difference in the miscue limit between chalk brands, so this is about the
 * player, not the equipment. */
#ifndef CUE_TIP_MAX
#define CUE_TIP_MAX 0.55f
#endif

#endif
