/*
 * Mote — rigid-body physics (spheres + OBB boxes; generalised from ThumbyCue's
 * billiard solver).
 *
 * Full-3D rigid bodies under gravity in a bounding box. Impulse-based collisions
 * with restitution, Coulomb friction and rotational inertia (so bodies pick up
 * spin off impacts), a fixed-substep integrator driven by an accumulator
 * (frame-rate independent), a uniform-grid broad phase, and continuous-ish
 * overlap separation so bodies never stick.
 *
 * SI units (metres, kg, seconds). Per-body inverse mass: 0 = immovable.
 * The engine runs the solver on the game's body array (via the ABI); the game
 * owns the bodies and renders them however it likes.
 *
 * SLEEPING: a body that barely MOVES (small net displacement from an anchor)
 * and isn't spinning for ~20 frames goes to sleep — it stops integrating, skips
 * wall checks, and sleeper-vs-sleeper collision pairs are skipped — so a settled
 * pile costs almost nothing. Sleep is POSITION-based, not velocity-based, so a
 * body that jitters in place (the residual velocity a deep stack never fully
 * solves out) still sleeps. Net movement re-anchors and wakes it: a fall, a
 * hit's depenetration, or a fresh position/velocity from the game; disturbances
 * cascade through a heap from the point of impact. State lives in
 * MoteBody._reserved[0..3] (still-frame counter + anchor position) — do not
 * repurpose those if you use the solver.
 *
 *   LIMITATION: waking requires *net motion*. If a body's support is removed
 *   with no nudge (you delete the body underneath it, or move a kinematic floor
 *   out from under a resting stack), the sleepers above don't move and will hang
 *   frozen in mid-air until something disturbs them. For destructible /
 *   removable stacks, displace the affected bodies a touch (e.g.
 *   b->pos.y -= 0.05f) to force a wake, or re-toss.
 */
#ifndef MOTE_PHYS_H
#define MOTE_PHYS_H

#include <stdint.h>
#include "mote_vec.h"

#define MOTE_SHAPE_SPHERE 0u
#define MOTE_SHAPE_BOX    1u

typedef struct {
    Vec3  pos;        /* centre, world metres */
    Vec3  vel;        /* m/s */
    Vec3  w;          /* angular velocity, rad/s (world) */
    Mat3  orient;     /* orientation, integrated from w */
    float radius;     /* sphere radius; box: bounding radius for body-body */
    float inv_mass;   /* 1/kg; 0 = immovable */
    uint32_t shape;   /* MOTE_SHAPE_SPHERE (default) or MOTE_SHAPE_BOX */
    Vec3  half;       /* box half-extents (box only) */
    uint32_t _reserved[4];   /* room to grow without breaking the data ABI */
} MoteBody;

typedef struct {
    Vec3  gravity;        /* e.g. (0,-9.8,0) */
    Vec3  bmin, bmax;     /* bounding box (6 walls: floor/ceiling/sides) */
    float restitution;    /* bounce 0..1 (body-body and body-wall) */
    float friction;       /* Coulomb coefficient */
    float linear_damp;    /* per-second linear velocity damping (air drag) */
    float angular_damp;   /* per-second angular damping */
    float substep;        /* fixed substep seconds (0 -> default ~1/240). Raise
                           * the RATE (e.g. 1/2000) for few, fast bodies that
                           * need accuracy/no-tunnel (pool); lower it (1/120) for
                           * many slow bodies where the per-step cost dominates. */
    int   max_substeps;   /* cap substeps/frame, 0 -> default. Bound against the
                           * spiral-of-death; a high-rate game (pool) must raise
                           * this so its intended substeps run (e.g. 40). */
    float _acc;           /* integrator accumulator (do not touch) */
} MoteWorld;

#define MOTE_PHYS_HIT  (1u << 0)   /* an impact (body or wall) occurred this step */

/* Sensible defaults (earth gravity, ~unit box, lively bounce). */
void mote_phys_world_defaults(MoteWorld *w);

/* Advance the simulation by dt seconds. Returns an event bitmask (MOTE_PHYS_*)
 * for sound/feedback. */
uint32_t mote_phys_step(MoteWorld *w, MoteBody *bodies, int n, float dt);

#endif /* MOTE_PHYS_H */
