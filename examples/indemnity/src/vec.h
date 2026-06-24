/*
 * ThumbyElite — small float vector/matrix helpers (header-only).
 *
 * Vec3 + Mat3 (row-vectors are basis axes: right/up/forward). All floats —
 * the RP2350 M33 FPU + -ffast-math makes these competitive with fixed point
 * and far less error-prone (proven by ThumbyCraft's all-float raycaster).
 *
 * Convention: world is right-handed, camera looks down +Z in view space,
 * screen x right / y down. Mat3 rows: m[0]=right, m[1]=up, m[2]=forward.
 */
#ifndef ELITE_VEC_H
#define ELITE_VEC_H

#include <math.h>

typedef struct { float x, y, z; } Vec3;
typedef struct { Vec3 r[3]; } Mat3;   /* r[0]=right, r[1]=up, r[2]=forward */

static inline Vec3 v3(float x, float y, float z) { Vec3 v = {x, y, z}; return v; }
static inline Vec3 v3_add(Vec3 a, Vec3 b)  { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b)  { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 v3_scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 v3_cross(Vec3 a, Vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len2(Vec3 a) { return v3_dot(a, a); }
static inline float v3_len(Vec3 a)  { return sqrtf(v3_len2(a)); }
static inline Vec3 v3_norm(Vec3 a) {
    float l = v3_len(a);
    return (l > 1e-12f) ? v3_scale(a, 1.0f / l) : v3(0, 0, 1);
}
static inline Vec3 v3_lerp(Vec3 a, Vec3 b, float t) {
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

static inline Mat3 m3_identity(void) {
    Mat3 m = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    return m;
}

/* Row-vector transform: world = local.x*right + local.y*up + local.z*forward. */
static inline Vec3 m3_mul_v3(const Mat3 *m, Vec3 v) {
    return v3(v.x * m->r[0].x + v.y * m->r[1].x + v.z * m->r[2].x,
              v.x * m->r[0].y + v.y * m->r[1].y + v.z * m->r[2].y,
              v.x * m->r[0].z + v.y * m->r[1].z + v.z * m->r[2].z);
}

/* Transpose-transform: project a world vector onto the basis axes (the
 * inverse of m3_mul_v3 for orthonormal m) — used for world -> view. */
static inline Vec3 m3_mul_v3_t(const Mat3 *m, Vec3 v) {
    return v3(v3_dot(v, m->r[0]), v3_dot(v, m->r[1]), v3_dot(v, m->r[2]));
}

/* Rotate the basis about one of its own (local) axes by angle a. */
static inline void m3_rotate_local(Mat3 *m, int axis, float a) {
    float c = cosf(a), s = sinf(a);
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    Vec3 ri = m->r[i], rj = m->r[j];
    m->r[i] = v3_add(v3_scale(ri, c), v3_scale(rj, s));
    m->r[j] = v3_sub(v3_scale(rj, c), v3_scale(ri, s));
}

/* Rotate the basis about an arbitrary WORLD-space unit axis (Rodrigues).
 * Used to steer a ship toward a target direction. */
static inline void m3_rotate_world(Mat3 *m, Vec3 k, float a) {
    float c = cosf(a), s = sinf(a), omc = 1.0f - c;
    for (int i = 0; i < 3; i++) {
        Vec3 v = m->r[i];
        Vec3 kxv = v3_cross(k, v);
        float kdv = v3_dot(k, v);
        m->r[i] = v3_add(v3_add(v3_scale(v, c), v3_scale(kxv, s)),
                         v3_scale(k, kdv * omc));
    }
}

/* Re-orthonormalise (drift control after many incremental rotations). */
static inline void m3_orthonormalize(Mat3 *m) {
    m->r[2] = v3_norm(m->r[2]);
    m->r[0] = v3_norm(v3_cross(m->r[1], m->r[2]));
    m->r[1] = v3_cross(m->r[2], m->r[0]);
}

#endif
