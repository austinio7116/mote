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

#include "mote_vec.h"   /* engine's Vec3/Mat3 + helpers (identical to ThumbyElite's vec.h) */
#include <stdint.h>

#define CUE_MAX_BALLS   22   /* snooker: cue + 15 reds + 6 colours */
#ifndef CUE_MAX_SEG
#define CUE_MAX_SEG     96   /* cushion nose segments (snooker uses curved jaws) */
#endif
#define CUE_MAX_JAW     24   /* bed-boundary knuckle points */
#define CUE_MAX_POCKET   6
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
    uint8_t _pad;
    float drop;      /* >0 while falling into the pocket (seconds remaining);
                      * still renders (sinking) but is out of play */
    /* How long it has been off the cloth. A ball can legitimately spend a
     * moment on the rail; one that has come to rest up there is not coming
     * back, and a frame cannot wait on it for ever. */
    float astray;
} CueBall;

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
    float e_cush;     /* restitution */
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

    /* Geometry (filled by cue_table). */
    CueSeg seg[CUE_MAX_SEG]; int nseg;
    Vec3   jaw[CUE_MAX_SEG]; int njaw; float jaw_r;   /* immovable jaw-tip circles */
    Vec3   pocket[CUE_MAX_POCKET]; float pocket_r[CUE_MAX_POCKET]; int npocket;
    /* THE MOUTH OF EACH POCKET: the midpoint of the line between its two jaw
     * tips, and the unit normal of that line pointing INTO the pocket. Past
     * that line the ball is in the throat and the only way back is out through
     * the mouth again — which is what the jaws and the back actually are. A
     * radius cannot express this: the opening is bounded by two points, and
     * everything else around it is solid. Filled in by cue_table_build_world
     * once the jaws are placed. */
    Vec3   pmouth[CUE_MAX_POCKET];
    Vec3   pmnorm[CUE_MAX_POCKET];
    float  drop_back;       /* CORNER drop pulled this far further INTO the pocket (m) */
    float  drop_back_side;  /* MIDDLE drop pulled straight back into the pocket (m) */

    /* First object ball the CUE ball contacts after a strike; -1 = none.
     * Reset to -1 before each shot; read at settle for rules.
     * first_hit = ball id (for rules); first_hit_idx = ball index (for the
     * follow-camera, since snooker reds share an id). */
    int first_hit;
    int first_hit_idx;

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

/* Advance the simulation by dt seconds. Returns 1 while any ball is still
 * moving, 0 once the table has settled. `events` (optional) receives a
 * bitwise OR of CUE_EV_* for sound/feedback this call. */
enum {
    CUE_EV_BALL_HIT  = 1 << 0,   /* ball–ball contact */
    CUE_EV_CUSHION   = 1 << 1,   /* ball–cushion contact */
    CUE_EV_POCKET    = 1 << 2,   /* a ball was potted */
    CUE_EV_JAW       = 1 << 3,   /* ball rattled a jaw */
    CUE_EV_BED       = 1 << 4,   /* a jumped ball came down on the slate */
};
int cue_phys_step(CueWorld *w, CueBall *balls, int n, float dt, uint32_t *events);
float cue_phys_cushion_impact(void);   /* loudest rail-approach speed from last step */

/* Override the integrator substep (0 = restore the default 2 kHz CUE_H). The AI
 * uses a coarser step for its headless ranking sims to run ~2x faster. */
void cue_phys_set_substep(float h);

int cue_phys_moving(const CueWorld *w, const CueBall *balls, int n);

#endif
