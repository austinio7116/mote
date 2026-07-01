/*
 * Mote — 2D rigid-body physics (top-down plane), a planar sibling of mote_phys.
 *
 * Impulse-based collisions with restitution + Coulomb friction and a single
 * rotational DOF, so bodies pick up spin off off-centre impacts (cars spin out
 * when clipped). Sequential-impulse solver, AABB broad phase, positional
 * depenetration so bodies never stick. The engine runs the solver on the game's
 * body array (via the ABI, mote_phys2d_step); the game owns the bodies and
 * renders them however it likes.
 *
 * SI-ish units (metres, kg, seconds) on an X/Y plane — a top-down game maps its
 * world (x,z) -> body (x,y). Per-body inverse mass: 0 = immovable (static walls
 * / kerbs). Set inv_inertia 0 to lock a body's rotation (peds, bollards).
 */
#ifndef MOTE_PHYS2D_H
#define MOTE_PHYS2D_H

#include <stdint.h>

#define MOTE_C2D_CIRCLE 0u
#define MOTE_C2D_BOX    1u    /* oriented box: half-extents hx (local +x) x hy (local +y) */

/* per-body flags */
#define MOTE_B2D_SENSOR 0x01u /* detect overlaps but apply no impulse */
#define MOTE_B2D_ASLEEP 0x02u /* engine-managed: skips integration while settled */

typedef struct MoteBody2D {
    float x, y;              /* position (plane) */
    float vx, vy;            /* linear velocity */
    float angle;            /* orientation, radians */
    float avel;             /* angular velocity, rad/s */
    float inv_mass;         /* 0 = static/immovable */
    float inv_inertia;      /* 0 = rotation locked */
    float restitution;      /* bounciness 0..1 */
    float friction;         /* Coulomb coefficient (combined as sqrt) */
    float lin_damp;         /* isotropic linear damping: v *= exp(-lin_damp*dt) each substep */
    float ang_damp;
    uint8_t shape;          /* MOTE_C2D_CIRCLE / MOTE_C2D_BOX */
    uint8_t flags;          /* MOTE_B2D_* */
    uint16_t mask;          /* collision groups bitmask; a pair collides if (mask_a & mask_b) != 0 (0 => collide all) */
    float radius;           /* circle radius */
    float hx, hy;           /* box half-extents */
    void *user;             /* game pointer (entity back-reference) */
    float lat_damp;         /* ANISOTROPIC (Coulomb) lateral friction, in m/s^2 (default 0 = OFF):
                             * removes up to lat_damp*dt of the velocity component PERPENDICULAR to
                             * the body's local axis (`angle`) each substep — a FIXED rate, so a fast
                             * sideways slide isn't fully caught and the body DRIFTS (more so the
                             * faster it slides). Domain-neutral — a driving game uses it as tyre
                             * grip; air-hockey/curling leave it 0. */
    float _rsv[1];          /* reserved */
} MoteBody2D;

typedef struct MoteWorld2D {
    float gx, gy;           /* gravity (0,0 for top-down) */
    int   iterations;       /* velocity solver iterations (0 => 8) */
    float min_x, min_y, max_x, max_y;   /* optional static bounds; all 0 => none */
} MoteWorld2D;

/* The contact scratch pool is ARENA-allocated at load by mote_phys2d_configure()
 * (sized by config.max_bodies/max_contacts) — the engine keeps only a pointer,
 * no fixed .bss — mirroring the 3D solver (mote_phys_configure). */
struct MoteArena;
int mote_phys2d_configure(struct MoteArena *arena, int max_bodies, int max_contacts);

/* Advance the world by dt (runs internal fixed substeps). Returns the number of
 * contacts resolved on the last substep. Bodies are updated in place. */
uint32_t mote_phys2d_step(MoteWorld2D *w, MoteBody2D *b, int n, float dt);

/* ---- inline constructors (header-only, no engine link) -------------------- */
static inline MoteBody2D mote_body2d_circle(float x, float y, float r, float mass) {
    MoteBody2D b; for (unsigned i=0;i<sizeof b;i++) ((char*)&b)[i]=0;
    b.x=x; b.y=y; b.radius=r; b.shape=MOTE_C2D_CIRCLE;
    b.inv_mass = mass>0 ? 1.0f/mass : 0.0f;
    /* disc inertia I = 0.5 m r^2 */
    b.inv_inertia = mass>0 ? 1.0f/(0.5f*mass*r*r) : 0.0f;
    b.restitution=0.1f; b.friction=0.6f; b.mask=0;
    return b;
}
static inline MoteBody2D mote_body2d_box(float x, float y, float hx, float hy, float angle, float mass) {
    MoteBody2D b; for (unsigned i=0;i<sizeof b;i++) ((char*)&b)[i]=0;
    b.x=x; b.y=y; b.hx=hx; b.hy=hy; b.angle=angle; b.shape=MOTE_C2D_BOX;
    b.inv_mass = mass>0 ? 1.0f/mass : 0.0f;
    /* rectangle inertia I = m (w^2 + h^2)/12 */
    b.inv_inertia = mass>0 ? 1.0f/(mass*((2*hx)*(2*hx)+(2*hy)*(2*hy))/12.0f) : 0.0f;
    b.restitution=0.1f; b.friction=0.6f; b.mask=0;
    return b;
}

#endif /* MOTE_PHYS2D_H */
