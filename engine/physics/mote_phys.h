/*
 * Mote — rigid-sphere physics (generalised from ThumbyCue's billiard solver).
 *
 * Full-3D rigid spheres under gravity in a bounding box. Impulse-based
 * collisions with restitution, Coulomb friction and the sphere's rotational
 * inertia (so bodies pick up spin off impacts), a fixed-substep integrator
 * driven by an accumulator (frame-rate independent), and continuous-ish
 * overlap separation so bodies never stick.
 *
 * SI units (metres, kg, seconds). Per-body inverse mass: 0 = immovable.
 * The engine runs the solver on the game's body array (via the ABI); the game
 * owns the bodies and renders them however it likes.
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
    float substep;        /* fixed substep seconds (0 -> default) */
    float _acc;           /* integrator accumulator (do not touch) */
} MoteWorld;

#define MOTE_PHYS_HIT  (1u << 0)   /* an impact (body or wall) occurred this step */

/* Sensible defaults (earth gravity, ~unit box, lively bounce). */
void mote_phys_world_defaults(MoteWorld *w);

/* Advance the simulation by dt seconds. Returns an event bitmask (MOTE_PHYS_*)
 * for sound/feedback. */
uint32_t mote_phys_step(MoteWorld *w, MoteBody *bodies, int n, float dt);

#endif /* MOTE_PHYS_H */
