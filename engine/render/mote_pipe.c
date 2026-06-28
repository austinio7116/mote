/*
 * Mote — 3D geometry pipeline. (Ported from ThumbyElite r3d_pipe.c.)
 *
 * Per object: compose model->view into one 3x3 + translation, transform all
 * verts to view space once, then per face: backface pre-cull, flat-shade
 * against the sun, near-plane clip (Sutherland-Hodgman, 3-in fast path),
 * project, and emit final screen triangles to the frame draw-list (mote_scene).
 *
 * Projection works in LOGICAL 128x128 space (centre = 64, focal = 64/tan).
 * mote_scene scales by MOTE_SS before rasterising.
 */
#include "mote_pipe.h"
#include <math.h>

static Mat3  s_cam;            /* camera basis (rows: right/up/forward) */
static Vec3  s_cam_pos;        /* camera world position; {0,0,0} -> positions are camera-relative (legacy) */
static float s_focal;          /* pixels: 64 / tan(fov/2) */
static Vec3  s_sun_view;       /* sun dir rotated into view space */
static Vec3  s_sun_world = {0.577f, 0.577f, -0.577f};
/* Near plane + depth scale, runtime-settable per game (small scenes like a
 * snooker table need a much nearer plane than the 0.5 m space-sim default). */
static float s_near    = MOTE_NEAR;
static float s_depth_k = MOTE_DEPTH_K;

void mote_pipe_set_near(float near_m) {
    if (near_m < 0.001f) near_m = 0.001f;
    s_near = near_m;
    s_depth_k = 65535.0f * near_m;
}
float mote_pipe_near(void)    { return s_near; }
float mote_pipe_depth_k(void) { return s_depth_k; }

/* Latched when a textured mesh is drawn but the game declared no textured-tri
 * pool (MoteConfig.max_tex_tris == 0): we render it flat rather than dropping
 * it, and the OS surfaces a one-time warning so the dev knows to budget for it. */
static int s_textri_starved;
int  mote_pipe_textri_starved(void)       { return s_textri_starved; }
void mote_pipe_clear_textri_starved(void) { s_textri_starved = 0; }

static Vec3    s_view[MOTE_MAX_VERTS];
static float   s_sx[MOTE_MAX_VERTS], s_sy[MOTE_MAX_VERTS];
static uint16_t s_sd[MOTE_MAX_VERTS];
static uint8_t  s_front[MOTE_MAX_VERTS];

void mote_pipe_set_camera(const Mat3 *cam_basis, float fov_deg) {
    s_cam = *cam_basis;
    s_cam_pos = v3(0, 0, 0);    /* legacy scene_begin: positions stay camera-relative */
    s_focal = (MOTE_FB_W * 0.5f) / tanf(fov_deg * (3.14159265f / 180.0f) * 0.5f);
    s_sun_view = m3_mul_v3_t(&s_cam, s_sun_world);
}
/* Opt into absolute world positions: subtract this camera pos before the basis. */
void mote_pipe_set_camera_pos(Vec3 cam_pos) { s_cam_pos = cam_pos; }
Vec3 mote_pipe_cam_pos(void) { return s_cam_pos; }

void mote_pipe_set_sun(Vec3 dir_toward_light_world) {
    s_sun_world = v3_norm(dir_toward_light_world);
    s_sun_view = m3_mul_v3_t(&s_cam, s_sun_world);
}

static inline void project(Vec3 v, float *sx, float *sy, uint16_t *sd) {
    float inv_z = 1.0f / v.z;
    *sx = (MOTE_FB_W * 0.5f) + s_focal * v.x * inv_z;
    *sy = (MOTE_FB_H * 0.5f) - s_focal * v.y * inv_z;
    float d = s_depth_k * inv_z;
    *sd = (d >= 65535.0f) ? 65535u : (uint16_t)d;
}

/* Scale an RGB565 colour by shade in [0,1]. */
static inline uint16_t shade565(uint16_t c, float shade) {
    int r = (int)(((c >> 11) & 0x1F) * shade);
    int g = (int)(((c >> 5) & 0x3F) * shade);
    int b = (int)((c & 0x1F) * shade);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

const Mat3 *mote_pipe_camera(void) { return &s_cam; }
float mote_pipe_focal(void) { return s_focal; }
Vec3 mote_pipe_sun_view(void) { return s_sun_view; }
Vec3 mote_pipe_sun_world(void) { return s_sun_world; }

int mote_pipe_draw_object(const MoteObject *obj) {
    return mote_pipe_draw_object_scaled(obj, 1.0f);
}

int mote_pipe_draw_object_scaled(const MoteObject *obj, float os) {
    const Mesh *mesh = obj->mesh;
    if (!mesh || mesh->nverts > MOTE_MAX_VERTS) return 0;
    float br = mesh->bound_r * os;

    /* Whole-object cull: bounding sphere behind near plane or outside cone. */
    Vec3 c_view = m3_mul_v3_t(&s_cam, v3_sub(obj->pos, s_cam_pos));
    if (c_view.z + br < s_near) return 0;
    float lim = c_view.z + br * 2.0f;
    if (c_view.x - br > lim || -c_view.x - br > lim ||
        c_view.y - br > lim || -c_view.y - br > lim)
        return 0;

    /* Compose model->view: M = camT o obj.basis, t = camT * obj.pos. */
    Mat3 M;
    M.r[0] = m3_mul_v3_t(&s_cam, obj->basis.r[0]);
    M.r[1] = m3_mul_v3_t(&s_cam, obj->basis.r[1]);
    M.r[2] = m3_mul_v3_t(&s_cam, obj->basis.r[2]);
    Vec3 t = c_view;

    float vscale = mesh->scale * os * (1.0f / 127.0f);
    for (int i = 0; i < mesh->nverts; i++) {
        Vec3 mv = v3(mesh->verts[i].x * vscale,
                     mesh->verts[i].y * vscale,
                     mesh->verts[i].z * vscale);
        Vec3 vv = v3_add(m3_mul_v3(&M, mv), t);
        s_view[i] = vv;
        if (vv.z > s_near) {
            s_front[i] = 1;
            project(vv, &s_sx[i], &s_sy[i], &s_sd[i]);
        } else {
            s_front[i] = 0;
        }
    }

    int drawn = 0;
    const float nscale = 1.0f / 127.0f;
    /* Resolve colour source once: a per-draw override wins; else the mesh's optional
     * per-face array; else its single base colour. */
    const uint16_t *fcols = obj->color ? 0 : mesh->face_colors;
    uint16_t mcol = obj->color ? obj->color : mesh->color;
    /* Textured (UV-mapped) mesh: per-corner UVs scaled from 0..255 to texels.
     * A textured mesh needs a textured-tri pool (MoteConfig.max_tex_tris); with
     * none, fall back to flat shading in the mesh's baked average colour instead
     * of silently emitting nothing, and latch a warning for the OS to surface. */
    const int want_tex = (mesh->texture && mesh->face_uvs) ? 1 : 0;
    const int textured = want_tex && mote_scene_textri_cap() > 0;
    if (want_tex && !textured) s_textri_starved = 1;
    const float uvsw = textured ? mesh->texture->w * (1.0f / 255.0f) : 0.0f;
    const float uvsh = textured ? mesh->texture->h * (1.0f / 255.0f) : 0.0f;
    for (int f = 0; f < mesh->nfaces; f++) {
        const MeshFace *face = &mesh->faces[f];
        int a = face->a, b = face->b, c = face->c;

        Vec3 nm = v3(face->nx * nscale, face->ny * nscale, face->nz * nscale);
        Vec3 nv = m3_mul_v3(&M, nm);

        /* Backface pre-cull: visible iff normal points back toward origin. */
        if (v3_dot(nv, s_view[a]) >= 0.0f) continue;

        float ndotl = v3_dot(nv, s_sun_view);
        float shade = 0.25f + (ndotl > 0.0f ? 0.75f * ndotl : 0.0f);

        if (textured) {
            uint8_t sh8 = (uint8_t)(shade >= 1.0f ? 255 : (int)(shade * 255.0f));
            const uint8_t *fu = &mesh->face_uvs[f * 6];
            float uv[3][2] = { {fu[0] * uvsw, fu[1] * uvsh},
                               {fu[2] * uvsw, fu[3] * uvsh},
                               {fu[4] * uvsw, fu[5] * uvsh} };
            int ic = s_front[a] + s_front[b] + s_front[c];
            if (ic == 3) {
                mote_emit_textri(s_sx[a], s_sy[a], s_sd[a], uv[0][0], uv[0][1],
                                 s_sx[b], s_sy[b], s_sd[b], uv[1][0], uv[1][1],
                                 s_sx[c], s_sy[c], s_sd[c], uv[2][0], uv[2][1],
                                 mesh->texture, sh8);
                drawn++;
                continue;
            }
            if (ic == 0) continue;
            /* Near-clip carrying UVs alongside positions. */
            int idx3[3] = { a, b, c };
            Vec3 out[4]; float ou[4], ov[4]; int n_out = 0;
            for (int i = 0; i < 3; i++) {
                Vec3 p = s_view[idx3[i]], q = s_view[idx3[(i + 1) % 3]];
                int p_in = p.z > s_near, q_in = q.z > s_near;
                if (p_in) { out[n_out] = p; ou[n_out] = uv[i][0]; ov[n_out] = uv[i][1]; n_out++; }
                if (p_in != q_in) {
                    float tt = (s_near - p.z) / (q.z - p.z);
                    out[n_out] = v3_lerp(p, q, tt);
                    ou[n_out] = uv[i][0] + (uv[(i + 1) % 3][0] - uv[i][0]) * tt;
                    ov[n_out] = uv[i][1] + (uv[(i + 1) % 3][1] - uv[i][1]) * tt;
                    n_out++;
                }
            }
            if (n_out < 3) continue;
            float ox[4], oy[4]; uint16_t od[4];
            for (int i = 0; i < n_out; i++) {
                Vec3 v = out[i];
                if (v.z < s_near * 1.0001f) v.z = s_near * 1.0001f;
                project(v, &ox[i], &oy[i], &od[i]);
            }
            mote_emit_textri(ox[0], oy[0], od[0], ou[0], ov[0],
                             ox[1], oy[1], od[1], ou[1], ov[1],
                             ox[2], oy[2], od[2], ou[2], ov[2], mesh->texture, sh8);
            drawn++;
            if (n_out == 4) {
                mote_emit_textri(ox[0], oy[0], od[0], ou[0], ov[0],
                                 ox[2], oy[2], od[2], ou[2], ov[2],
                                 ox[3], oy[3], od[3], ou[3], ov[3], mesh->texture, sh8);
                drawn++;
            }
            continue;
        }

        uint16_t col = shade565(fcols ? fcols[f] : mcol, shade);

        int in_a = s_front[a], in_b = s_front[b], in_c = s_front[c];
        int in_count = in_a + in_b + in_c;
        if (in_count == 3) {
            mote_emit_tri(s_sx[a], s_sy[a], s_sd[a],
                        s_sx[b], s_sy[b], s_sd[b],
                        s_sx[c], s_sy[c], s_sd[c], col);
            drawn++;
            continue;
        }
        if (in_count == 0) continue;

        /* Near-plane clip (Sutherland-Hodgman, one plane). */
        Vec3 poly[3] = { s_view[a], s_view[b], s_view[c] };
        Vec3 out[4];
        int n_out = 0;
        for (int i = 0; i < 3; i++) {
            Vec3 p = poly[i], q = poly[(i + 1) % 3];
            int p_in = p.z > s_near, q_in = q.z > s_near;
            if (p_in) out[n_out++] = p;
            if (p_in != q_in) {
                float tt = (s_near - p.z) / (q.z - p.z);
                out[n_out++] = v3_lerp(p, q, tt);
            }
        }
        if (n_out < 3) continue;

        float ox[4], oy[4];
        uint16_t od[4];
        for (int i = 0; i < n_out; i++) {
            Vec3 v = out[i];
            if (v.z < s_near * 1.0001f) v.z = s_near * 1.0001f;
            project(v, &ox[i], &oy[i], &od[i]);
        }
        mote_emit_tri(ox[0], oy[0], od[0], ox[1], oy[1], od[1],
                    ox[2], oy[2], od[2], col);
        drawn++;
        if (n_out == 4) {
            mote_emit_tri(ox[0], oy[0], od[0], ox[2], oy[2], od[2],
                        ox[3], oy[3], od[3], col);
            drawn++;
        }
    }
    return drawn;
}
