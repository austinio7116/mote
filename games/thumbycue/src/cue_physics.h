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
#define CUE_MAX_SEG     96   /* cushion nose segments (snooker uses curved jaws) */
#define CUE_MAX_JAW     24   /* bed-boundary knuckle points */
#define CUE_MAX_POCKET   6

typedef struct {
    Vec3 pos;        /* world metres (y = R) */
    Vec3 vel;        /* m/s (y = 0) */
    Vec3 w;          /* angular velocity rad/s (world) */
    Mat3 orient;     /* render orientation, integrated from w */
    uint8_t on;      /* 1 = on the table (incl. mid-drop), 0 = gone */
    uint8_t id;      /* ball number / colour code (game-defined) */
    uint8_t pocket;  /* if potted: which pocket index it fell in */
    uint8_t _pad;
    float drop;      /* >0 while falling into the pocket (seconds remaining);
                      * still renders (sinking) but is out of play */
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

    /* Geometry (filled by cue_table). */
    CueSeg seg[CUE_MAX_SEG]; int nseg;
    Vec3   jaw[CUE_MAX_SEG]; int njaw; float jaw_r;   /* immovable jaw-tip circles */
    Vec3   pocket[CUE_MAX_POCKET]; float pocket_r[CUE_MAX_POCKET]; int npocket;
    float  drop_back;       /* CORNER drop pulled this far further INTO the pocket (m) */
    float  drop_back_side;  /* MIDDLE drop pulled straight back into the pocket (m) */

    /* First object ball the CUE ball contacts after a strike; -1 = none.
     * Reset to -1 before each shot; read at settle for rules.
     * first_hit = ball id (for rules); first_hit_idx = ball index (for the
     * follow-camera, since snooker reds share an id). */
    int first_hit;
    int first_hit_idx;

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

/* Advance the simulation by dt seconds. Returns 1 while any ball is still
 * moving, 0 once the table has settled. `events` (optional) receives a
 * bitwise OR of CUE_EV_* for sound/feedback this call. */
enum {
    CUE_EV_BALL_HIT  = 1 << 0,   /* ball–ball contact */
    CUE_EV_CUSHION   = 1 << 1,   /* ball–cushion contact */
    CUE_EV_POCKET    = 1 << 2,   /* a ball was potted */
    CUE_EV_JAW       = 1 << 3,   /* ball rattled a jaw */
};
int cue_phys_step(CueWorld *w, CueBall *balls, int n, float dt, uint32_t *events);
float cue_phys_cushion_impact(void);   /* loudest rail-approach speed from last step */

/* Override the integrator substep (0 = restore the default 2 kHz CUE_H). The AI
 * uses a coarser step for its headless ranking sims to run ~2x faster. */
void cue_phys_set_substep(float h);

int cue_phys_moving(const CueWorld *w, const CueBall *balls, int n);

#endif
