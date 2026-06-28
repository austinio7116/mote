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
    /* Optional XZ broad-phase grid (mote_phys_mesh_build_grid). When set, a body
     * tests only the triangles in/around its cell instead of all ntris — essential
     * once you have many bodies on a big mesh. 0 = brute force. */
    int    grid_n;
    float  grid_x0, grid_z0, grid_invx, grid_invz;
    const uint16_t *grid_start;   /* (grid_n*grid_n + 1) CSR offsets */
    const uint16_t *grid_tri;     /* triangle indices, CSR-packed by cell */
} MoteMesh;

/* Build an XZ broad-phase grid for `m` (call ONCE after filling verts/tris).
 * Caller owns the scratch: `start` holds grid_n*grid_n+1 uint16, `tri` holds
 * >= m->ntris uint16. Bins each triangle by its centroid; a query tests the 3x3
 * cells around a body. Pick grid_n so a cell is ~one triangle wide. Returns 0,
 * or -1 if tri_cap < ntris / grid_n out of [1,64]. Header-inline (no engine link). */
static inline int mote_phys_mesh_build_grid(MoteMesh *m, int grid_n,
                                            uint16_t *start, uint16_t *tri, int tri_cap) {
    if (grid_n < 1 || grid_n > 64 || m->ntris > tri_cap) return -1;
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (int i = 0; i < m->nverts; i++) {
        Vec3 v = m->verts[i];
        if (v.x < x0) x0 = v.x;
        if (v.x > x1) x1 = v.x;
        if (v.z < z0) z0 = v.z;
        if (v.z > z1) z1 = v.z;
    }
    x0 -= 1e-3f; z0 -= 1e-3f; x1 += 1e-3f; z1 += 1e-3f;
    float cx = (x1 - x0) / grid_n, cz = (z1 - z0) / grid_n;
    float invx = cx > 1e-6f ? 1.0f/cx : 0.0f, invz = cz > 1e-6f ? 1.0f/cz : 0.0f;
    int nc = grid_n * grid_n;
    for (int c = 0; c <= nc; c++) start[c] = 0;
    for (int t = 0; t < m->ntris; t++) {                         /* count per cell */
        const uint16_t *tr = &m->tris[t*3];
        float mx = (m->verts[tr[0]].x + m->verts[tr[1]].x + m->verts[tr[2]].x) * (1.0f/3.0f);
        float mz = (m->verts[tr[0]].z + m->verts[tr[1]].z + m->verts[tr[2]].z) * (1.0f/3.0f);
        int gx = (int)((mx - x0)*invx); if (gx < 0) gx = 0; if (gx >= grid_n) gx = grid_n-1;
        int gz = (int)((mz - z0)*invz); if (gz < 0) gz = 0; if (gz >= grid_n) gz = grid_n-1;
        start[gz*grid_n + gx + 1]++;
    }
    for (int c = 1; c <= nc; c++) start[c] += start[c-1];        /* prefix sum */
    for (int t = 0; t < m->ntris; t++) {                         /* scatter */
        const uint16_t *tr = &m->tris[t*3];
        float mx = (m->verts[tr[0]].x + m->verts[tr[1]].x + m->verts[tr[2]].x) * (1.0f/3.0f);
        float mz = (m->verts[tr[0]].z + m->verts[tr[1]].z + m->verts[tr[2]].z) * (1.0f/3.0f);
        int gx = (int)((mx - x0)*invx); if (gx < 0) gx = 0; if (gx >= grid_n) gx = grid_n-1;
        int gz = (int)((mz - z0)*invz); if (gz < 0) gz = 0; if (gz >= grid_n) gz = grid_n-1;
        tri[start[gz*grid_n + gx]++] = (uint16_t)t;
    }
    for (int c = nc; c >= 1; c--) start[c] = start[c-1];         /* shift cursors back */
    start[0] = 0;
    m->grid_n = grid_n; m->grid_x0 = x0; m->grid_z0 = z0;
    m->grid_invx = invx; m->grid_invz = invz;
    m->grid_start = start; m->grid_tri = tri;
    return 0;
}

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

/* Allocate the physics pools from the load-time arena, sized to the game's
 * MoteConfig (max_bodies==0 = physics opted out). Called by the OS at load,
 * before the game's init. Returns 0 if the arena couldn't fit the request. */
struct MoteArena;
int mote_phys_configure(struct MoteArena *arena, int max_bodies, int max_contacts);

#endif /* MOTE_PHYS_H */
