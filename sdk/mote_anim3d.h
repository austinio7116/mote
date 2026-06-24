#ifndef MOTE_ANIM3D_H
#define MOTE_ANIM3D_H
/*
 * Mote 3D model animation — rigid-part (hierarchical) skeletal animation,
 * header-only, no engine ABI.
 *
 * A model is split into named PARTS (each part = one or more <=255-vert mesh
 * chunks). Parts form a tree (each has a parent and a pivot — the joint it
 * rotates about). An animation CLIP holds, per part, a track of keyframes
 * (local rotation + translation + time). At runtime a tiny MoteModelPlayer
 * advances the clip; mote_rig_draw() composes parent x local transforms down
 * the tree and submits each part through the engine's normal mote_draw path —
 * so animation costs only a handful of small matrix composes (no per-vertex
 * work) and needs no firmware change.
 *
 *   #include "mech.anim3d.h"     // MoteRig mech_rig; clips mech_walk, mech_fire...
 *   static MoteModelPlayer p;
 *   mote_rig_play(&p, &mech_walk);
 *   // each frame, after scene_camera():
 *   mote_rig_tick(&p, dt);
 *   mote_rig_draw(mote, &mech_rig, &p, world_pos);   // or _ex(pos,basis,scale)
 *
 * Parts MUST be listed parent-before-child (root first). The Studio rig/anim
 * tab and the obj2rig baker guarantee this; hand-authored rigs must too.
 *
 * Rotations are stored as quaternions (correct, shortest-path interpolation);
 * author them as Euler angles in the IDE and bake to quats. This pairs with the
 * 2D MoteAnim loop modes (mote_anim.h): MOTE_ANIM_ONCE / _LOOP / _PINGPONG.
 */
#include <math.h>
#include "mote_api.h"
#include "mote_anim.h"   /* MOTE_ANIM_ONCE / _LOOP / _PINGPONG */

#ifndef MOTE_RIG_MAX_PARTS
#define MOTE_RIG_MAX_PARTS 32      /* stack pose buffer in mote_rig_draw */
#endif

/* ---- quaternion (x,y,z,w) — header-only, pairs with Vec3/Mat3 ---- */
typedef struct { float x, y, z, w; } MoteQuat;

static inline MoteQuat mote_quat_identity(void) { MoteQuat q = {0,0,0,1}; return q; }

static inline MoteQuat mote_quat_axis(Vec3 axis, float ang) {
    float h = ang * 0.5f, s = sinf(h);
    return (MoteQuat){ axis.x*s, axis.y*s, axis.z*s, cosf(h) };
}
static inline MoteQuat mote_quat_mul(MoteQuat a, MoteQuat b) {
    return (MoteQuat){
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
/* Intrinsic X then Y then Z (radians) — the order the rig editor presents. */
static inline MoteQuat mote_quat_euler(float rx, float ry, float rz) {
    MoteQuat qx = mote_quat_axis(v3(1,0,0), rx);
    MoteQuat qy = mote_quat_axis(v3(0,1,0), ry);
    MoteQuat qz = mote_quat_axis(v3(0,0,1), rz);
    return mote_quat_mul(mote_quat_mul(qz, qy), qx);
}
/* Normalised lerp — cheap, shortest-path, plenty for keyframe spans. */
static inline MoteQuat mote_quat_nlerp(MoteQuat a, MoteQuat b, float t) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    MoteQuat r = { a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t };
    float l = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z + r.w*r.w);
    if (l > 1e-6f) { float inv = 1.0f/l; r.x*=inv; r.y*=inv; r.z*=inv; r.w*=inv; } else r = mote_quat_identity();
    return r;
}
/* To a Mat3 in the engine's row-as-image convention (matches m3_mul_v3). */
static inline Mat3 mote_quat_m3(MoteQuat q) {
    float xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z, xy=q.x*q.y, xz=q.x*q.z, yz=q.y*q.z, wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
    Mat3 m;
    m.r[0] = v3(1-2*(yy+zz),   2*(xy+wz),   2*(xz-wy));
    m.r[1] = v3(2*(xy-wz),   1-2*(xx+zz),   2*(yz+wx));
    m.r[2] = v3(2*(xz+wy),     2*(yz-wx), 1-2*(xx+yy));
    return m;
}

/* ---- affine transform { basis, origin }: p_world = basis*p + origin ---- */
typedef struct { Mat3 b; Vec3 o; } MoteXform;
static inline Mat3 mote__m3_mul(const Mat3 *A, const Mat3 *B) {   /* (A.B)v = A(Bv) */
    Mat3 m; for (int k = 0; k < 3; k++) m.r[k] = m3_mul_v3(A, B->r[k]); return m;
}
static inline MoteXform mote__xf_mul(MoteXform A, MoteXform B) {  /* first B, then A */
    MoteXform r; r.b = mote__m3_mul(&A.b, &B.b); r.o = v3_add(m3_mul_v3(&A.b, B.o), A.o); return r;
}

/* ---- rig (skeleton + geometry) ---- */
typedef struct {
    const Mesh *chunks;     /* this part's geometry (1+ chunks) */
    uint16_t    nchunks;
    int8_t      parent;     /* parent part index, -1 = root. MUST be < this part's index. */
    Vec3        pivot;      /* joint location in model space (the part rotates about it) */
} MoteRigPart;

typedef struct {
    const MoteRigPart *parts;
    uint16_t           count;     /* <= MOTE_RIG_MAX_PARTS */
    uint16_t           tris;      /* total faces (use for .max_tris) */
} MoteRig;

/* ---- animation clip ---- */
typedef struct {
    uint16_t t_ms;                /* time within the clip */
    MoteQuat rot;                 /* local rotation about the part's pivot */
    Vec3     pos;                 /* local translation (model units) */
} MoteModelKey;

typedef struct {
    uint16_t            part;     /* index into rig->parts */
    const MoteModelKey *keys;
    uint16_t            nkeys;
} MoteModelTrack;

typedef struct {
    const char           *name;
    const MoteModelTrack *tracks;
    uint16_t              ntracks;
    uint16_t              duration_ms;
    uint8_t               loop;   /* MOTE_ANIM_ONCE / _LOOP / _PINGPONG */
} MoteModelClip;

/* ---- per-instance playback cursor ---- */
typedef struct {
    const MoteModelClip *clip;
    uint32_t             t_ms;
    int8_t               dir;     /* +1 / -1 for ping-pong */
    uint8_t              done;
} MoteModelPlayer;

static inline void mote_rig_play(MoteModelPlayer *p, const MoteModelClip *c) {
    p->clip = c; p->t_ms = 0; p->dir = 1; p->done = 0;
}

/* Advance the clock by dt seconds, honouring the loop mode. */
static inline void mote_rig_tick(MoteModelPlayer *p, float dt) {
    const MoteModelClip *c = p->clip;
    if (!c || c->duration_ms == 0 || p->done) return;
    int32_t ms = (int32_t)(dt * 1000.0f + 0.5f); if (ms < 0) ms = 0;
    uint32_t dur = c->duration_ms;
    if (c->loop == MOTE_ANIM_PINGPONG) {
        uint32_t span = dur * 2;
        uint32_t t = p->t_ms + (uint32_t)ms;
        t %= span ? span : 1;
        p->t_ms = t;                 /* stored 0..2*dur; folded to 0..dur in sampling */
    } else if (c->loop == MOTE_ANIM_LOOP) {
        p->t_ms = (p->t_ms + (uint32_t)ms) % dur;
    } else { /* ONCE */
        p->t_ms += (uint32_t)ms;
        if (p->t_ms >= dur) { p->t_ms = dur; p->done = 1; }
    }
}

/* The clip's effective sample time (folds ping-pong's 0..2*dur into 0..dur). */
static inline uint32_t mote__rig_sample_ms(const MoteModelPlayer *p) {
    const MoteModelClip *c = p->clip;
    if (c->loop == MOTE_ANIM_PINGPONG && p->t_ms > c->duration_ms)
        return (uint32_t)c->duration_ms * 2 - p->t_ms;
    return p->t_ms > c->duration_ms ? c->duration_ms : p->t_ms;
}

/* Sample one track at time t_ms into (rot,pos). Clamps before first / after last key. */
static inline void mote__rig_sample(const MoteModelTrack *tr, uint32_t t_ms, MoteQuat *rot, Vec3 *pos) {
    if (tr->nkeys == 0) { *rot = mote_quat_identity(); *pos = v3(0,0,0); return; }
    if (t_ms <= tr->keys[0].t_ms) { *rot = tr->keys[0].rot; *pos = tr->keys[0].pos; return; }
    const MoteModelKey *last = &tr->keys[tr->nkeys - 1];
    if (t_ms >= last->t_ms) { *rot = last->rot; *pos = last->pos; return; }
    int i = 0; while (i + 1 < tr->nkeys && tr->keys[i + 1].t_ms <= t_ms) i++;
    const MoteModelKey *a = &tr->keys[i], *b = &tr->keys[i + 1];
    float span = (float)(b->t_ms - a->t_ms);
    float f = span > 0 ? (float)(t_ms - a->t_ms) / span : 0.0f;
    *rot = mote_quat_nlerp(a->rot, b->rot, f);
    *pos = v3_lerp(a->pos, b->pos, f);
}

/* One part's LOCAL pose (rotation about its pivot + translation). The unit a
 * game manipulates for procedural posing (turret yaw, barrel recoil, …). */
typedef struct { MoteQuat rot; Vec3 pos; } MoteRigLocal;

/* Fill locals[0..count) from a clip (rest pose where a part has no track). Pass
 * pl = NULL for the full rest pose, which you then override per part by hand. */
static inline void mote_rig_eval(const MoteRig *rig, const MoteModelPlayer *pl, MoteRigLocal *locals) {
    uint32_t t = (pl && pl->clip) ? mote__rig_sample_ms(pl) : 0;
    for (uint16_t i = 0; i < rig->count; i++) {
        locals[i].rot = mote_quat_identity(); locals[i].pos = v3(0,0,0);
        if (pl && pl->clip)
            for (uint16_t k = 0; k < pl->clip->ntracks; k++)
                if (pl->clip->tracks[k].part == i) {
                    mote__rig_sample(&pl->clip->tracks[k], t, &locals[i].rot, &locals[i].pos); break;
                }
    }
}

/* Compose per-part model-space world transforms from locals (parent x local,
 * rotating each about its pivot). Parent-before-child order required. */
static inline void mote_rig_compose(const MoteRig *rig, const MoteRigLocal *locals, MoteXform *out) {
    for (uint16_t i = 0; i < rig->count; i++) {
        const MoteRigPart *part = &rig->parts[i];
        Mat3 R = mote_quat_m3(locals[i].rot);
        /* rotate about the pivot, then translate: p -> R*(p-pivot)+pivot + pos */
        MoteXform local = { R, v3_add(v3_sub(part->pivot, m3_mul_v3(&R, part->pivot)), locals[i].pos) };
        out[i] = (part->parent < 0) ? local : mote__xf_mul(out[part->parent], local);
    }
}

/* Draw a rig from explicit per-part locals at a world placement, tinting every
 * part with `color` (RGB565; 0 = each mesh's own colour). The procedural path:
 *   MoteRigLocal loc[N]; mote_rig_eval(rig, NULL, loc);
 *   loc[TURRET].rot = mote_quat_axis(v3(0,1,0), aim);   // point the turret
 *   mote_rig_draw_locals_tint(mote, rig, loc, pos, bodyBasis, 1, teamColor); */
static inline void mote_rig_draw_locals_tint(const MoteApi *m, const MoteRig *rig, const MoteRigLocal *locals,
                                             Vec3 pos, Mat3 basis, float scale, uint16_t color) {
    MoteXform world[MOTE_RIG_MAX_PARTS];
    uint16_t n = rig->count < MOTE_RIG_MAX_PARTS ? rig->count : MOTE_RIG_MAX_PARTS;
    mote_rig_compose(rig, locals, world);
    for (uint16_t i = 0; i < n; i++) {
        const MoteRigPart *part = &rig->parts[i];
        Mat3 fb = mote__m3_mul(&basis, &world[i].b);
        Vec3 fp = v3_add(pos, m3_mul_v3(&basis, v3_scale(world[i].o, scale)));
        for (uint16_t c = 0; c < part->nchunks; c++) {
            MoteObject o = { .pos = fp, .basis = fb, .mesh = &part->chunks[c], .color = color };
            if (scale == 1.0f) m->scene_add_object(&o); else m->scene_add_object_scaled(&o, scale);
        }
    }
}
static inline void mote_rig_draw_locals(const MoteApi *m, const MoteRig *rig, const MoteRigLocal *locals,
                                        Vec3 pos, Mat3 basis, float scale) {
    mote_rig_draw_locals_tint(m, rig, locals, pos, basis, scale, 0);
}

/* Like _tint, but each part gets its own flat colour from part_colors[part] (a 0
 * entry falls back to that part's baked mesh colours). Lets one rig render in
 * several tones — e.g. a team-coloured hull with dark tracks and a steel gun. */
static inline void mote_rig_draw_locals_palette(const MoteApi *m, const MoteRig *rig, const MoteRigLocal *locals,
                                                Vec3 pos, Mat3 basis, float scale, const uint16_t *part_colors) {
    MoteXform world[MOTE_RIG_MAX_PARTS];
    uint16_t n = rig->count < MOTE_RIG_MAX_PARTS ? rig->count : MOTE_RIG_MAX_PARTS;
    mote_rig_compose(rig, locals, world);
    for (uint16_t i = 0; i < n; i++) {
        const MoteRigPart *part = &rig->parts[i];
        Mat3 fb = mote__m3_mul(&basis, &world[i].b);
        Vec3 fp = v3_add(pos, m3_mul_v3(&basis, v3_scale(world[i].o, scale)));
        uint16_t col = part_colors ? part_colors[i] : 0;
        for (uint16_t c = 0; c < part->nchunks; c++) {
            MoteObject o = { .pos = fp, .basis = fb, .mesh = &part->chunks[c], .color = col };
            if (scale == 1.0f) m->scene_add_object(&o); else m->scene_add_object_scaled(&o, scale);
        }
    }
}

/* Compute every part's world transform for the current clip pose (convenience). */
static inline void mote_rig_pose(const MoteRig *rig, const MoteModelPlayer *pl, MoteXform *out) {
    MoteRigLocal loc[MOTE_RIG_MAX_PARTS];
    mote_rig_eval(rig, pl, loc);
    mote_rig_compose(rig, loc, out);
}

/* Draw a rig playing a clip at a world placement (convenience over eval+draw). */
static inline void mote_rig_draw_ex(const MoteApi *m, const MoteRig *rig, const MoteModelPlayer *pl,
                                    Vec3 pos, Mat3 basis, float scale) {
    MoteRigLocal loc[MOTE_RIG_MAX_PARTS];
    mote_rig_eval(rig, pl, loc);
    mote_rig_draw_locals(m, rig, loc, pos, basis, scale);
}
static inline void mote_rig_draw(const MoteApi *m, const MoteRig *rig, const MoteModelPlayer *pl, Vec3 pos) {
    mote_rig_draw_ex(m, rig, pl, pos, m3_identity(), 1.0f);
}

#endif /* MOTE_ANIM3D_H */
