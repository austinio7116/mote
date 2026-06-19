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

#define MOTE_SHAPE_SPHERE  0u
#define MOTE_SHAPE_BOX     1u
#define MOTE_SHAPE_PLANE   2u  /* infinite static half-space; normal = orient.r[1]
                                * (up), solid below it. Use for ground/walls/ramps.
                                * Always treated as static (set inv_mass 0). */
#define MOTE_SHAPE_CAPSULE 3u  /* swept sphere: segment along local +Y of half-
                                * length `half.y`, thickened by `radius`. Great
                                * for characters / pills / rounded props. */
#define MOTE_SHAPE_HULL    4u  /* arbitrary CONVEX shape: shape_data -> MoteHull.
                                * Multi-point manifolds (SAT + clip) for stable
                                * collision/stacking. */
#define MOTE_SHAPE_MESH    5u  /* STATIC concave triangle mesh (the actual model,
                                * not its hull): shape_data -> MoteMesh. Dynamic
                                * bodies collide against its triangles. inv_mass 0. */

/* Convex hull (local space). Verts power the support function; faces + edges
 * give SAT axes and face clipping for multi-point contact manifolds (stable
 * stacking — not the single-point GJK fallback). Build with the obj2hull baker
 * or by hand. facev is a flat list of vertex indices; face f spans
 * faceoff[f]..faceoff[f+1]. edges are vertex-index pairs. */
typedef struct {
    const Vec3   *verts;    int nverts;
    const Vec3   *fnorm;                    /* per-face outward unit normal */
    const uint8_t *faceoff;                 /* nfaces+1 offsets into facev */
    const uint8_t *facev;                   /* flat face vertex indices */
    int           nfaces;
    const uint8_t *edges;   int nedges;     /* edge vertex-index pairs (2*nedges) */
    float         bound_r;                  /* bounding-sphere radius (local) */
} MoteHull;

/* Static concave triangle-mesh collider (the model's actual surface). */
typedef struct {
    const Vec3     *verts;  int nverts;
    const uint16_t *tris;   int ntris;      /* 3 vertex indices per triangle */
    float           bound_r;
} MoteMesh;

typedef struct {
    Vec3  pos;        /* centre, world metres (plane: a point on the surface) */
    Vec3  vel;        /* m/s */
    Vec3  w;          /* angular velocity, rad/s (world) */
    Mat3  orient;     /* orientation (plane: r[1] = normal; capsule: r[1] = axis) */
    float radius;     /* sphere/capsule radius; box: bounding radius for body-body */
    float inv_mass;   /* 1/kg; 0 = immovable (static collider) */
    uint32_t shape;   /* MOTE_SHAPE_SPHERE / _BOX / _PLANE / _CAPSULE / _HULL */
    Vec3  half;       /* box half-extents; capsule: half.y = segment half-length */
    float friction;    /* per-body Coulomb coefficient; 0 -> use world.friction */
    float restitution; /* per-body bounce 0..1; 0 -> use world.restitution */
    const void *shape_data;  /* MOTE_SHAPE_HULL -> const MoteHull *; else NULL */
    uint32_t _reserved[4];   /* sleep state (counter + anchor) — do not repurpose */
} MoteBody;

typedef struct {
    Vec3  gravity;        /* e.g. (0,-9.8,0) */
    Vec3  bmin, bmax;     /* bounding box (6 walls) — used only if walls != 0 */
    int   walls;          /* 1 = auto bounding-box walls (default); 0 = none
                           * (build your own world from static PLANE/BOX bodies) */
    float restitution;    /* default bounce 0..1 (per-body can override) */
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

/* --- queries (no simulation; for aiming / ground checks / picking / AI) --- */

typedef struct {
    int   body;       /* index of the hit body, -1 if none */
    float t;          /* distance along the ray to the hit point */
    Vec3  point;      /* world-space hit point */
    Vec3  normal;     /* surface normal (points back toward the ray origin) */
} MoteRayHit;

/* Cast a ray from origin along unit dir, up to max_dist, against every body
 * (sphere / box / plane). Returns 1 and fills `hit` with the NEAREST
 * intersection, or 0 if nothing is hit. Pass skip = a body index to ignore
 * (e.g. the shooter), or skip < 0 to test all. */
int mote_phys_raycast(const MoteWorld *w, const MoteBody *bodies, int n,
                      Vec3 origin, Vec3 dir, float max_dist, int skip,
                      MoteRayHit *hit);

/* Sphere-overlap query: fill out[] with up to `max` indices of bodies whose
 * shape overlaps the test sphere (center,radius). Returns the count found. */
int mote_phys_overlap(const MoteWorld *w, const MoteBody *bodies, int n,
                      Vec3 center, float radius, int *out, int max);

#endif /* MOTE_PHYS_H */
