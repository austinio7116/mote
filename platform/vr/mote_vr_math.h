/*
 * Mote VR — just enough linear algebra, in the conventions OpenXR and GL use.
 *
 * Right-handed, +Y up, -Z forward. Matrices are column-major and indexed
 * m[col*4 + row], which is what glUniformMatrix4fv wants with transpose=0.
 * Quaternions are (x,y,z,w), which is XrQuaternionf's layout, so an XrPosef
 * can be copied straight into MoteVrPose.
 *
 * Header-only: the preview, the renderer and the OpenXR layer all want it and
 * none of it is worth a translation unit.
 */
#ifndef MOTE_VR_MATH_H
#define MOTE_VR_MATH_H

#include <math.h>

typedef struct { float x, y, z; }    MoteVrV3;
typedef struct { float x, y, z, w; } MoteVrQ;
typedef struct { MoteVrQ q; MoteVrV3 p; } MoteVrPose;

static inline MoteVrV3 mv3(float x, float y, float z) { MoteVrV3 v = {x, y, z}; return v; }
static inline MoteVrV3 mv3_add(MoteVrV3 a, MoteVrV3 b) { return mv3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline MoteVrV3 mv3_sub(MoteVrV3 a, MoteVrV3 b) { return mv3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline MoteVrV3 mv3_scale(MoteVrV3 a, float s)  { return mv3(a.x*s, a.y*s, a.z*s); }
static inline float    mv3_dot(MoteVrV3 a, MoteVrV3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float    mv3_len(MoteVrV3 a)             { return sqrtf(mv3_dot(a, a)); }
static inline MoteVrV3 mv3_cross(MoteVrV3 a, MoteVrV3 b) {
    return mv3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline MoteVrV3 mv3_norm(MoteVrV3 a) {
    float l = mv3_len(a);
    return l > 1e-8f ? mv3_scale(a, 1.0f/l) : mv3(0, 0, -1);
}
static inline MoteVrV3 mv3_lerp(MoteVrV3 a, MoteVrV3 b, float t) {
    return mv3(a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t);
}

static inline MoteVrQ mq_ident(void) { MoteVrQ q = {0, 0, 0, 1}; return q; }

static inline MoteVrQ mq_norm(MoteVrQ q) {
    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (l < 1e-8f) return mq_ident();
    l = 1.0f/l;
    MoteVrQ r = {q.x*l, q.y*l, q.z*l, q.w*l};
    return r;
}

/* Inverse, for unit quaternions. */
static inline MoteVrQ mq_conj(MoteVrQ q) {
    MoteVrQ r = {-q.x, -q.y, -q.z, q.w};
    return r;
}

static inline MoteVrQ mq_mul(MoteVrQ a, MoteVrQ b) {
    MoteVrQ r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return r;
}

static inline MoteVrV3 mq_rot(MoteVrQ q, MoteVrV3 v) {
    MoteVrV3 u = mv3(q.x, q.y, q.z);
    MoteVrV3 t = mv3_scale(mv3_cross(u, v), 2.0f);
    return mv3_add(mv3_add(v, mv3_scale(t, q.w)), mv3_cross(u, t));
}

static inline MoteVrQ mq_axis_angle(MoteVrV3 axis, float radians) {
    MoteVrV3 a = mv3_norm(axis);
    float s = sinf(radians*0.5f);
    MoteVrQ q = {a.x*s, a.y*s, a.z*s, cosf(radians*0.5f)};
    return q;
}

/* Shortest-arc interpolation. Normalised lerp rather than slerp: over the tiny
 * angles smoothing deals with, the difference is far below tracking noise. */
static inline MoteVrQ mq_nlerp(MoteVrQ a, MoteVrQ b, float t) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    float s = d < 0.0f ? -1.0f : 1.0f;
    MoteVrQ r = {a.x + (b.x*s - a.x)*t, a.y + (b.y*s - a.y)*t,
                 a.z + (b.z*s - a.z)*t, a.w + (b.w*s - a.w)*t};
    return mq_norm(r);
}

/* An orthonormal basis as a quaternion, from the three axes as columns. */
static inline MoteVrQ mq_from_axes(MoteVrV3 x, MoteVrV3 y, MoteVrV3 z) {
    float m00 = x.x, m01 = y.x, m02 = z.x;
    float m10 = x.y, m11 = y.y, m12 = z.y;
    float m20 = x.z, m21 = y.z, m22 = z.z;
    float tr = m00 + m11 + m22;
    MoteVrQ q;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f*s; q.x = (m21-m12)/s; q.y = (m02-m20)/s; q.z = (m10-m01)/s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21-m12)/s; q.x = 0.25f*s; q.y = (m01+m10)/s; q.z = (m02+m20)/s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02-m20)/s; q.x = (m01+m10)/s; q.y = 0.25f*s; q.z = (m12+m21)/s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10-m01)/s; q.x = (m02+m20)/s; q.y = (m12+m21)/s; q.z = 0.25f*s;
    }
    return mq_norm(q);
}

/* ---- 4x4 ---------------------------------------------------------------- */

static inline void mm4_identity(float *m) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mm4_mul(float *out, const float *a, const float *b) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++)
            r[c*4+i] = a[0*4+i]*b[c*4+0] + a[1*4+i]*b[c*4+1]
                     + a[2*4+i]*b[c*4+2] + a[3*4+i]*b[c*4+3];
    for (int i = 0; i < 16; i++) out[i] = r[i];
}

/* Model matrix for a pose with a uniform scale. */
static inline void mm4_from_pose(float *m, MoteVrPose pose, float s) {
    MoteVrQ q = pose.q;
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    m[0]  = (1-2*(yy+zz))*s; m[1]  = (2*(xy+wz))*s;   m[2]  = (2*(xz-wy))*s;   m[3]  = 0;
    m[4]  = (2*(xy-wz))*s;   m[5]  = (1-2*(xx+zz))*s; m[6]  = (2*(yz+wx))*s;   m[7]  = 0;
    m[8]  = (2*(xz+wy))*s;   m[9]  = (2*(yz-wx))*s;   m[10] = (1-2*(xx+yy))*s; m[11] = 0;
    m[12] = pose.p.x;        m[13] = pose.p.y;        m[14] = pose.p.z;        m[15] = 1;
}

/* The view matrix for an eye at `pose` — the inverse of a rotation+translation,
 * which for a rigid transform is the transpose of the rotation and a rotated
 * negated translation. No general inverse needed. */
static inline void mm4_view_from_pose(float *m, MoteVrPose pose) {
    float r[16];
    mm4_from_pose(r, pose, 1.0f);
    m[0] = r[0]; m[1] = r[4]; m[2]  = r[8];
    m[4] = r[1]; m[5] = r[5]; m[6]  = r[9];
    m[8] = r[2]; m[9] = r[6]; m[10] = r[10];
    m[3] = m[7] = m[11] = 0.0f;
    m[12] = -(m[0]*pose.p.x + m[4]*pose.p.y + m[8]*pose.p.z);
    m[13] = -(m[1]*pose.p.x + m[5]*pose.p.y + m[9]*pose.p.z);
    m[14] = -(m[2]*pose.p.x + m[6]*pose.p.y + m[10]*pose.p.z);
    m[15] = 1.0f;
}

/* Projection from the four half-angles OpenXR reports (XrFovf), which are
 * signed angles and generally NOT symmetric on a headset. */
static inline void mm4_proj_fov(float *m, float angle_left, float angle_right,
                                float angle_up, float angle_down,
                                float znear, float zfar) {
    float l = tanf(angle_left), r = tanf(angle_right);
    float u = tanf(angle_up),   d = tanf(angle_down);
    float w = r - l, h = u - d;
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (r + l) / w;
    m[9]  = (u + d) / h;
    m[10] = -(zfar + znear) / (zfar - znear);
    m[11] = -1.0f;
    m[14] = -(2.0f * zfar * znear) / (zfar - znear);
}

/* Symmetric perspective, for the desktop preview. Expressed through the same
 * routine the headset uses so there is one projection in the codebase. */
static inline void mm4_perspective(float *m, float fovy_rad, float aspect,
                                   float znear, float zfar) {
    float t = tanf(fovy_rad * 0.5f);
    mm4_proj_fov(m, atanf(-t*aspect), atanf(t*aspect), atanf(t), atanf(-t),
                 znear, zfar);
}

#endif /* MOTE_VR_MATH_H */
