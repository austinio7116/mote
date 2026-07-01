/*
 * Mote — scene draw-list + dual-core banded rasterisation.
 */
#include "mote_arena.h"
#include "mote_scene3d.h"
#include "mote_raster.h"
#include "mote_config.h"
#include "mote_2d.h"      /* MoteImage (3D sprite billboards) */
#include "mote_object.h"  /* MOTE_BLEND_* */
#include <math.h>
#include <string.h>

/* color + flags packed into the floats' natural padding -> still 36 bytes, so
 * the per-object draw flag (MOTE_DRAW_NO_DEPTH_WRITE) costs no extra arena. */
typedef struct {
    float ax, ay; uint16_t az; uint16_t color;
    float bx, by; uint16_t bz; uint8_t flags;
    float cx, cy; uint16_t cz;
} ScreenTri;

/* Sphere impostor: a screen disc shaded per-pixel as a sphere (front
 * hemisphere normal reconstructed from the disc), depth-tested + writing. */
typedef struct {
    float sx, sy, sr;   /* logical screen centre + radius */
    float vz, radius;   /* view-space centre depth + world radius */
    uint16_t color;
} ScreenSphere;

/* Textured / oriented sphere impostor: like ScreenSphere but carries the local
 * orientation basis and a surface descriptor for per-pixel texture/shading. */
typedef struct {
    float sx, sy, sr;   /* logical screen centre + radius */
    float vz, radius;   /* view-space centre depth + world radius */
    Mat3  orient;       /* world->local applied per pixel (m3_mul_v3_t) */
    const MoteSphereTex *tex;
} ScreenSphereTex;

/* Soft ground-shadow decal: a projected ground-plane ellipse (centre + the
 * screen projections of world +X/+Z offsets, so it foreshortens) that DARKENS
 * the framebuffer with a radial falloff. Depth-tested (raised geometry occludes
 * it) but never depth-writing. */
typedef struct { float cx, cy, ux, uy, vx, vy;
                 float iz_c, diz_u, diz_v;   /* inv-z at centre + deltas toward +X/+Z (for per-pixel ground depth) */
                 float strength; } ScreenShadow;

/* Depth-tested FX primitives (logical screen coords; scaled by MOTE_SS at
 * raster). Depth value d = K/z (larger = nearer), tested but not written. */
typedef struct { float x, y; uint16_t d, color; uint8_t size; } ScreenPoint;
typedef struct { float x0, y0; uint16_t d0; float x1, y1; uint16_t d1, color; } ScreenLine;
typedef struct { float cx, cy, r; uint16_t d, color; } ScreenDisc;
typedef struct { float cx, cy, r; uint16_t d, color; } ScreenRing;   /* outline (billboard circle) */
/* Camera-facing textured quad (a 2D sprite at a world position), depth-tested. */
typedef struct {
    float cx, cy, hw, hh;     /* screen centre + half width/height (logical px) */
    uint16_t d;               /* depth at the billboard plane */
    const MoteImage *img;
    uint16_t fx, fy, fw, fh;  /* source frame rect in img */
    uint8_t blend;            /* MOTE_BLEND_* */
} ScreenBillboard;
/* UV-mapped (textured) screen triangle. Affine UV/depth interpolation. */
typedef struct {
    float ax, ay, bx, by, cx, cy;   /* logical screen coords */
    float au, av, bu, bv, cu, cv;   /* texel coords (0..tex w/h) */
    uint16_t ad, bd, cd;            /* per-vertex depth */
    const MoteImage *tex;
    uint8_t shade;                  /* 0..255 sun-lighting multiplier */
    uint8_t flags;                  /* MOTE_BLEND_* | MOTE_DRAW_NO_DEPTH_WRITE */
} ScreenTexTri;

/* Draw-list + impostor + FX pools are arena-allocated at load, sized to the
 * game's MoteConfig. */
static ScreenTri       *s_tris;     static int s_max_tris;       static int s_ntris;
static ScreenSphere    *s_spheres;  static int s_max_spheres;    static int s_nspheres;
static ScreenSphereTex *s_texsph;   static int s_max_texsph;     static int s_ntexsph;
static ScreenPoint     *s_points;   static int s_max_points;     static int s_npoints;
static ScreenLine      *s_lines;    static int s_max_lines;      static int s_nlines;
static ScreenDisc      *s_discs;    static int s_max_discs;      static int s_ndiscs;
static ScreenRing      *s_rings;    static int s_max_rings;      static int s_nrings;
static ScreenBillboard *s_bbs;      static int s_max_bbs;        static int s_nbbs;
static ScreenTexTri    *s_textris;  static int s_max_textris;    static int s_ntextris;
static ScreenShadow    *s_shadows;  static int s_max_shadows;    static int s_nshadows;
static uint16_t     s_bg = 0x0000;
static MoteBackgroundFn s_bg_cb;    /* optional per-band background pass */
static uint8_t      s_emit_flags;   /* flags for the object currently being added */

int mote_scene_configure(MoteArena *arena, int max_tris, int max_spheres,
                         int max_points, int max_lines, int max_discs,
                         int max_tex_spheres, int max_shadows, int max_rings,
                         int max_billboards, int max_tex_tris) {
    s_bg_cb = 0;                     /* reset per game load */
    mote_pipe_set_near(MOTE_NEAR);   /* reset near plane to the default per load */
    s_max_bbs = max_billboards;
    s_bbs = max_billboards > 0 ? mote_arena_alloc(arena, (size_t)max_billboards * sizeof(ScreenBillboard)) : 0;
    s_max_textris = max_tex_tris;
    s_textris = max_tex_tris > 0 ? mote_arena_alloc(arena, (size_t)max_tex_tris * sizeof(ScreenTexTri)) : 0;
    s_max_tris    = max_tris;
    s_max_spheres = max_spheres;
    s_max_texsph  = max_tex_spheres;
    s_max_points  = max_points;
    s_max_lines   = max_lines;
    s_max_discs   = max_discs;
    s_max_shadows = max_shadows;
    s_max_rings   = max_rings;
    s_tris    = max_tris        > 0 ? mote_arena_alloc(arena, (size_t)max_tris        * sizeof(ScreenTri))       : 0;
    s_spheres = max_spheres     > 0 ? mote_arena_alloc(arena, (size_t)max_spheres     * sizeof(ScreenSphere))    : 0;
    s_texsph  = max_tex_spheres > 0 ? mote_arena_alloc(arena, (size_t)max_tex_spheres * sizeof(ScreenSphereTex)) : 0;
    s_points  = max_points      > 0 ? mote_arena_alloc(arena, (size_t)max_points      * sizeof(ScreenPoint))     : 0;
    s_lines   = max_lines       > 0 ? mote_arena_alloc(arena, (size_t)max_lines       * sizeof(ScreenLine))      : 0;
    s_discs   = max_discs       > 0 ? mote_arena_alloc(arena, (size_t)max_discs       * sizeof(ScreenDisc))      : 0;
    s_shadows = max_shadows     > 0 ? mote_arena_alloc(arena, (size_t)max_shadows     * sizeof(ScreenShadow))    : 0;
    s_rings   = max_rings       > 0 ? mote_arena_alloc(arena, (size_t)max_rings       * sizeof(ScreenRing))      : 0;
    return (max_tris        == 0 || s_tris)
        && (max_spheres     == 0 || s_spheres)
        && (max_tex_spheres == 0 || s_texsph)
        && (max_points      == 0 || s_points)
        && (max_lines       == 0 || s_lines)
        && (max_discs       == 0 || s_discs)
        && (max_shadows     == 0 || s_shadows)
        && (max_rings       == 0 || s_rings)
        && (max_billboards  == 0 || s_bbs)
        && (max_tex_tris    == 0 || s_textris);
}

/* Capacity of the textured-triangle pool (0 if the game didn't budget any).
 * The pipeline queries this so a textured mesh with no pool falls back to flat
 * shading instead of silently emitting nothing. */
int mote_scene_textri_cap(void) { return s_textris ? s_max_textris : 0; }

static inline uint16_t shade565(uint16_t c, float sh) {
    int r = (int)(((c >> 11) & 0x1F) * sh);
    int g = (int)(((c >> 5) & 0x3F) * sh);
    int b = (int)((c & 0x1F) * sh);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t mix565(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static inline uint16_t add565(uint16_t c, int dr, int dg, int db) {
    int r = ((c >> 11) & 0x1F) + dr, g = ((c >> 5) & 0x3F) + dg, b = (c & 0x1F) + db;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Combine a source pixel onto a destination per the blend mode. */
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t mode) {
    if (mode == MOTE_BLEND_ALPHA) return mix565(dst, src, 0.5f);
    if (mode == MOTE_BLEND_ADD)
        return add565(dst, (src >> 11) & 0x1F, (src >> 5) & 0x3F, src & 0x1F);
    return src;
}

/* Equirectangular albedo lookup from a unit local normal. */
static inline uint16_t tex_albedo(const MoteSphereTex *tx, Vec3 nl) {
    if (tx->texels || tx->indices) {
        float u = atan2f(nl.z, nl.x) * 0.1591549f + 0.5f;   /* 1/(2pi) */
        float ny = nl.y < -1.0f ? -1.0f : (nl.y > 1.0f ? 1.0f : nl.y);
        float v = asinf(ny) * 0.3183099f + 0.5f;            /* 1/pi */
        int tu = (int)(u * tx->tex_w), tv = (int)(v * tx->tex_h);
        if (tu < 0) tu = 0; else if (tu >= tx->tex_w) tu = tx->tex_w - 1;
        if (tv < 0) tv = 0; else if (tv >= tx->tex_h) tv = tx->tex_h - 1;
        int idx = tv * tx->tex_w + tu;
        return tx->texels ? tx->texels[idx] : tx->palette[tx->indices[idx]];
    }
    return tx->albedo ? tx->albedo(nl, tx->ud) : 0xFFFF;
}

/* Final pixel colour for a textured-impostor pixel. */
static inline uint16_t tex_shade(const MoteSphereTex *tx, Vec3 nl, Vec3 nw,
                                 float diff, float spec, float nz) {
    if (tx->shade_mode == MOTE_SHADE_CUSTOM && tx->shade)
        return tx->shade(nl, nw, diff, spec, nz, tx->ud);
    uint16_t base = tex_albedo(tx, nl);
    switch (tx->shade_mode) {
    case MOTE_SHADE_FLAT:
        return base;
    case MOTE_SHADE_LIT:
        return shade565(base, 0.25f + 0.75f * diff);
    case MOTE_SHADE_TOON: {
        uint16_t c = shade565(base, diff > 0.62f ? 1.0f : diff > 0.30f ? 0.74f : 0.52f);
        if (tx->tint) c = mix565(c, tx->tint, (1.0f - diff) * 0.40f);
        if (spec > 0.82f) c = 0xFFFF;
        return c;
    }
    case MOTE_SHADE_GLOSS: {
        uint16_t c = shade565(base, 0.30f + 0.70f * diff);
        if (tx->tint) c = mix565(c, tx->tint, (1.0f - diff) * 0.50f);
        if (spec > 0.60f) { float h = (spec - 0.60f) * 2.5f; h *= h * h; int hi = (int)(h * 30.0f);
                            if (hi > 0) c = add565(c, hi, hi, hi); }
        return c;
    }
    case MOTE_SHADE_SMOOTH:
    default: {
        uint16_t c = shade565(base, (0.30f + 0.70f * diff) * (0.78f + 0.22f * nz));
        float ss = spec; ss *= ss; ss *= ss; ss *= ss; int hi = (int)(ss * 26.0f);
        if (hi > 0) c = add565(c, hi, hi * 2, hi);
        return c;
    }
    }
}

void mote_scene_set_background(uint16_t rgb565) { s_bg = rgb565; }
void mote_scene_set_background_cb(MoteBackgroundFn fn) { s_bg_cb = fn; }

/* Called by mote_pipe for each projected, lit, clipped screen triangle. */
void mote_emit_tri(float ax, float ay, uint16_t az,
                 float bx, float by, uint16_t bz,
                 float cx, float cy, uint16_t cz, uint16_t color) {
    if (s_ntris >= s_max_tris) return;
    ScreenTri *t = &s_tris[s_ntris++];
    t->ax = ax; t->ay = ay; t->az = az;
    t->bx = bx; t->by = by; t->bz = bz;
    t->cx = cx; t->cy = cy; t->cz = cz;
    t->color = color; t->flags = s_emit_flags;
}

/* Called by mote_pipe for each projected, clipped TEXTURED screen triangle.
 * UVs are in texels (0..tex->w / tex->h); shade is the face's sun lighting. */
void mote_emit_textri(float ax, float ay, uint16_t az, float au, float av,
                      float bx, float by, uint16_t bz, float bu, float bv,
                      float cx, float cy, uint16_t cz, float cu, float cv,
                      const MoteImage *tex, uint8_t shade) {
    if (s_ntextris >= s_max_textris || !tex) return;
    ScreenTexTri *t = &s_textris[s_ntextris++];
    t->ax = ax; t->ay = ay; t->ad = az; t->au = au; t->av = av;
    t->bx = bx; t->by = by; t->bd = bz; t->bu = bu; t->bv = bv;
    t->cx = cx; t->cy = cy; t->cd = cz; t->cu = cu; t->cv = cv;
    t->tex = tex; t->shade = shade; t->flags = s_emit_flags;
}

static void scene_reset_lists(void) {
    s_ntextris = 0;
    s_ntris = 0; s_nspheres = 0; s_ntexsph = 0;
    s_npoints = 0; s_nlines = 0; s_ndiscs = 0; s_nshadows = 0; s_nrings = 0; s_nbbs = 0;
    s_emit_flags = 0;
}

void mote_scene_begin(const Mat3 *cam_basis, float fov_deg) {
    scene_reset_lists();
    mote_pipe_set_camera(cam_basis, fov_deg);
}

/* Camera-aware begin: pass the camera world position once, then add objects/
 * spheres/splats with ABSOLUTE world positions (no manual v3_sub(.., cam_pos)). */
void mote_scene_camera(const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg) {
    scene_reset_lists();
    mote_pipe_set_camera(cam_basis, fov_deg);
    mote_pipe_set_camera_pos(cam_pos);
}

/* Drop the draw-list without touching the camera — the OS calls this at the
 * start of every frame so a game that doesn't use the 3D scene never inherits
 * stale triangles from a previously-run game. */
void mote_scene_clear(void) { scene_reset_lists(); }

/* Add a sphere (camera-relative world position). Projected now; shaded as a
 * per-pixel impostor during the band raster — perfect spheres, cheap. */
int mote_scene_add_sphere(Vec3 cam_rel_pos, float radius, uint16_t color) {
    if (s_nspheres >= s_max_spheres) return 0;
    const Mat3 *cam = mote_pipe_camera();
    Vec3 v = m3_mul_v3_t(cam, v3_sub(cam_rel_pos, mote_pipe_cam_pos()));   /* world->view (cam_pos 0 = relative) */
    if (v.z <= mote_pipe_near()) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenSphere *s = &s_spheres[s_nspheres++];
    s->sx = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    s->sy = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    s->sr = focal * radius * inv;
    s->vz = v.z; s->radius = radius; s->color = color;
    return 1;
}

int mote_scene_add_sphere_tex(Vec3 cam_rel_pos, float radius,
                              const Mat3 *orient, const MoteSphereTex *tex) {
    if (s_ntexsph >= s_max_texsph || !tex) return 0;
    const Mat3 *cam = mote_pipe_camera();
    Vec3 v = m3_mul_v3_t(cam, v3_sub(cam_rel_pos, mote_pipe_cam_pos()));
    if (v.z <= mote_pipe_near()) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenSphereTex *s = &s_texsph[s_ntexsph++];
    s->sx = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    s->sy = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    s->sr = focal * radius * inv;
    s->vz = v.z; s->radius = radius;
    s->orient = orient ? *orient : m3_identity();
    s->tex = tex;
    return 1;
}

int mote_scene_add_object(const MoteObject *obj) {
    return mote_pipe_draw_object(obj);
}
int mote_scene_add_object_scaled(const MoteObject *obj, float scale) {
    return mote_pipe_draw_object_scaled(obj, scale);
}
int mote_scene_add_object_ex(const MoteObject *obj, uint32_t flags) {
    s_emit_flags = (uint8_t)flags;
    int n = mote_pipe_draw_object(obj);
    s_emit_flags = 0;
    return n;
}

/* world (camera-relative) -> view space. */
static inline Vec3 scene_to_view(Vec3 cam_rel) {
    return m3_mul_v3_t(mote_pipe_camera(), v3_sub(cam_rel, mote_pipe_cam_pos()));
}
static inline Vec3 scene_v3lerp(Vec3 a, Vec3 b, float t) {
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

/* Immediate-mode world-space triangle: project + near-clip + emit, with a
 * caller-supplied flat colour (already shaded — the engine does NOT light it).
 * For dynamic / procedural geometry that isn't a baked int8 Mesh (e.g. a
 * generated table, voxel faces). Returns triangles emitted (0/1/2 after clip). */
int mote_scene_add_tri(Vec3 a, Vec3 b, Vec3 c, uint16_t color, uint32_t flags) {
    Vec3 in[3] = { scene_to_view(a), scene_to_view(b), scene_to_view(c) };
    Vec3 out[4]; int nout = 0;
    for (int i = 0; i < 3; i++) {           /* near-clip (Sutherland-Hodgman) */
        Vec3 p = in[i], q = in[(i + 1) % 3];
        int pin = p.z >= mote_pipe_near(), qin = q.z >= mote_pipe_near();
        if (pin) out[nout++] = p;
        if (pin != qin) out[nout++] = scene_v3lerp(p, q, (mote_pipe_near() - p.z) / (q.z - p.z));
    }
    if (nout < 3) return 0;
    float focal = mote_pipe_focal();
    float sx[4], sy[4]; uint16_t sd[4];
    for (int i = 0; i < nout; i++) {
        float inv = 1.0f / out[i].z;
        sx[i] = (MOTE_FB_W * 0.5f) + focal * out[i].x * inv;
        sy[i] = (MOTE_FB_H * 0.5f) - focal * out[i].y * inv;
        sd[i] = (uint16_t)(mote_pipe_depth_k() / out[i].z);
    }
    uint8_t save = s_emit_flags; s_emit_flags = (uint8_t)flags;
    int n = 0;
    for (int i = 1; i + 1 < nout; i++) {    /* fan; double-sided (reorder to */
        float ax = sx[0], ay = sy[0], bx = sx[i], by = sy[i], cx = sx[i + 1], cy = sy[i + 1];
        uint16_t az = sd[0], bz = sd[i], cz = sd[i + 1];
        /* positive screen area so the raster's backface cull never drops it — an
         * immediate-mode triangle is drawn exactly as asked, regardless of winding). */
        if ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax) >= 0.0f)
            mote_emit_tri(ax, ay, az, bx, by, bz, cx, cy, cz, color);
        else
            mote_emit_tri(ax, ay, az, cx, cy, cz, bx, by, bz, color);
        n++;
    }
    s_emit_flags = save;
    return n;
}

int mote_scene_add_point(Vec3 p, uint16_t color, int size) {
    if (s_npoints >= s_max_points) return 0;
    Vec3 v = scene_to_view(p);
    if (v.z <= mote_pipe_near()) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenPoint *q = &s_points[s_npoints++];
    q->x = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    q->y = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    q->d = (uint16_t)(mote_pipe_depth_k() / v.z);
    q->color = color;
    q->size = (uint8_t)(size < 1 ? 1 : (size > 255 ? 255 : size));
    return 1;
}

int mote_scene_add_disc(Vec3 p, float radius, uint16_t color) {
    if (s_ndiscs >= s_max_discs) return 0;
    Vec3 v = scene_to_view(p);
    if (v.z <= mote_pipe_near()) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenDisc *q = &s_discs[s_ndiscs++];
    q->cx = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    q->cy = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    q->r  = focal * radius * inv;
    q->d  = (uint16_t)(mote_pipe_depth_k() / v.z);
    q->color = color;
    return 1;
}

int mote_scene_add_ring(Vec3 p, float radius, uint16_t color) {
    if (s_nrings >= s_max_rings) return 0;
    Vec3 v = scene_to_view(p);
    if (v.z <= mote_pipe_near()) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenRing *q = &s_rings[s_nrings++];
    q->cx = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    q->cy = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    q->r  = focal * radius * inv;
    q->d  = (uint16_t)(mote_pipe_depth_k() / v.z);
    q->color = color;
    return 1;
}

/* Camera-facing textured quad at a world position — a "3D sprite". Always
 * upright (axis-aligned to the screen), sized in world units so it shrinks with
 * distance, depth-tested against the scene. fw/fh select a sub-rect of the
 * image (sprite sheets); world_h is the quad's full height in world units. */
int mote_scene_add_billboard(Vec3 cam_rel_pos, const MoteImage *img,
                             int fx, int fy, int fw, int fh,
                             float world_h, uint8_t blend) {
    if (s_nbbs >= s_max_bbs || !img) return 0;
    if (fw <= 0) fw = img->w;
    if (fh <= 0) fh = img->h;
    Vec3 v = scene_to_view(cam_rel_pos);
    if (v.z <= mote_pipe_near()) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenBillboard *q = &s_bbs[s_nbbs++];
    q->cx = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    q->cy = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    q->hh = focal * (world_h * 0.5f) * inv;
    q->hw = q->hh * ((float)fw / (float)fh);   /* preserve the image's aspect */
    q->d  = (uint16_t)(mote_pipe_depth_k() / v.z);
    q->img = img;
    q->fx = (uint16_t)fx; q->fy = (uint16_t)fy;
    q->fw = (uint16_t)fw; q->fh = (uint16_t)fh;
    q->blend = blend;
    return 1;
}

int mote_scene_add_line(Vec3 a, Vec3 b, uint16_t color) {
    if (s_nlines >= s_max_lines) return 0;
    Vec3 va = scene_to_view(a), vb = scene_to_view(b);
    if (va.z <= mote_pipe_near() && vb.z <= mote_pipe_near()) return 0;
    if (va.z < mote_pipe_near())      va = scene_v3lerp(va, vb, (mote_pipe_near() - va.z) / (vb.z - va.z));
    else if (vb.z < mote_pipe_near()) vb = scene_v3lerp(vb, va, (mote_pipe_near() - vb.z) / (va.z - vb.z));
    float focal = mote_pipe_focal(), ia = 1.0f / va.z, ib = 1.0f / vb.z;
    ScreenLine *q = &s_lines[s_nlines++];
    q->x0 = (MOTE_FB_W * 0.5f) + focal * va.x * ia;
    q->y0 = (MOTE_FB_H * 0.5f) - focal * va.y * ia;
    q->d0 = (uint16_t)(mote_pipe_depth_k() / va.z);
    q->x1 = (MOTE_FB_W * 0.5f) + focal * vb.x * ib;
    q->y1 = (MOTE_FB_H * 0.5f) - focal * vb.y * ib;
    q->d1 = (uint16_t)(mote_pipe_depth_k() / vb.z);
    q->color = color;
    return 1;
}

/* Oriented elliptical ground shadow: the footprint is the ellipse spanned by the
 * two WORLD semi-axis vectors (on the ground plane). Lets the caller match the
 * object's shape + orientation (a tank gets a long oval along its hull) rather
 * than a fixed circle. Projects centre + the two semi-axis tips, foreshortened. */
int mote_scene_add_shadow_ex(Vec3 ground_pos, Vec3 semi_a, Vec3 semi_b, float strength) {
    if (s_nshadows >= s_max_shadows) return 0;
    Vec3 vc = scene_to_view(ground_pos);
    Vec3 va = scene_to_view(v3_add(ground_pos, semi_a));
    Vec3 vb = scene_to_view(v3_add(ground_pos, semi_b));
    float nearz = mote_pipe_near();
    if (vc.z <= nearz || va.z <= nearz || vb.z <= nearz) return 0;
    float focal = mote_pipe_focal();
    float cx  = (MOTE_FB_W * 0.5f) + focal * vc.x / vc.z, cy  = (MOTE_FB_H * 0.5f) - focal * vc.y / vc.z;
    float aax = (MOTE_FB_W * 0.5f) + focal * va.x / va.z, aay = (MOTE_FB_H * 0.5f) - focal * va.y / va.z;
    float abx = (MOTE_FB_W * 0.5f) + focal * vb.x / vb.z, aby = (MOTE_FB_H * 0.5f) - focal * vb.y / vb.z;
    ScreenShadow *s = &s_shadows[s_nshadows++];
    s->cx = cx; s->cy = cy;
    s->ux = aax - cx; s->uy = aay - cy;
    s->vx = abx - cx; s->vy = aby - cy;
    float izc = 1.0f / vc.z;
    s->iz_c = izc; s->diz_u = 1.0f / va.z - izc; s->diz_v = 1.0f / vb.z - izc;
    s->strength = strength <= 0.0f ? 0.5f : (strength > 1.0f ? 1.0f : strength);
    return 1;
}

/* Round shadow of `radius` — the common case, a wrapper over the oriented form. */
int mote_scene_add_shadow(Vec3 ground_pos, float radius, float strength) {
    return mote_scene_add_shadow_ex(ground_pos, v3(radius, 0, 0), v3(0, 0, radius), strength);
}

int mote_scene_tri_count(void) { return s_ntris; }

MOTE_HOT
/* Affine triangle fill in PHYSICAL coords, shared by textured meshes and
 * blended flat meshes. tex==NULL -> flat `flatcol`; else sample tex (UVs in
 * texels) and modulate by `shade` (0..255 sun lighting). blend = MOTE_BLEND_*;
 * writes!=0 stores depth. Depth-tested against the shared depth buffer. */
static void raster_tex_tri(float ax, float ay, float ad,
                           float bx, float by, float bd,
                           float cx, float cy, float cd,
                           float au, float av, float bu, float bv,
                           float cu, float cv, const MoteImage *tex,
                           uint16_t flatcol, uint8_t shade, uint8_t blend,
                           int writes, uint16_t *fb, int py0, int py1) {
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area > -1e-3f && area < 1e-3f) return;
    float inv = 1.0f / area;
    int minx = (int)floorf(fminf(ax, fminf(bx, cx)));
    int maxx = (int)ceilf (fmaxf(ax, fmaxf(bx, cx)));
    int miny = (int)floorf(fminf(ay, fminf(by, cy)));
    int maxy = (int)ceilf (fmaxf(ay, fmaxf(by, cy)));
    if (minx < 0) minx = 0;
    if (maxx > MOTE_FB_PW) maxx = MOTE_FB_PW;
    if (miny < py0) miny = py0;
    if (maxy > py1) maxy = py1;
    uint16_t *depth = mote_depth_buffer();
    int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    uint16_t tkey = tex ? tex->key : 0;
    int tkeyed = tex && !tex->opaque;
    float shf = shade * (1.0f / 255.0f);
    /* Perspective-correct texturing: the stored depth d = k/z is linear in 1/z,
     * so interpolate (u*d, v*d, d) affinely across the triangle and divide —
     * u = sum(w*u*d) / sum(w*d). Affine UV (the old way) visibly skews/swims on
     * large near quads (walls); this removes it. Pre-scale the corner UVs by d. */
    float aud = au * ad, avd = av * ad;
    float bud = bu * bd, bvd = bv * bd;
    float cud = cu * cd, cvd = cv * cd;
    for (int py = miny; py < maxy; py++) {
        float pyc = py + 0.5f;
        int basep = py * MOTE_FB_PW;
        for (int px = minx; px < maxx; px++) {
            float pxc = px + 0.5f;
            float w0 = ((cx - bx) * (pyc - by) - (cy - by) * (pxc - bx)) * inv;
            float w1 = ((ax - cx) * (pyc - cy) - (ay - cy) * (pxc - cx)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            float iz = w0 * ad + w1 * bd + w2 * cd;     /* ∝ 1/z */
            uint16_t d = (uint16_t)iz;
            int idx = basep + px;
            if (depth && d <= depth[idx]) continue;
            uint16_t src;
            if (tex) {
                float r = iz != 0.0f ? 1.0f / iz : 0.0f;
                int u = (int)((w0 * aud + w1 * bud + w2 * cud) * r);
                int v = (int)((w0 * avd + w1 * bvd + w2 * cvd) * r);
                if (u < 0) u = 0; else if (u >= tw) u = tw - 1;
                if (v < 0) v = 0; else if (v >= th) v = th - 1;
                src = mote_img_texel(tex, u, v);
                if (tkeyed && src == tkey) continue;
                if (shade < 255) src = shade565(src, shf);
            } else {
                src = flatcol;
            }
            fb[idx] = blend ? blend565(fb[idx], src, blend) : src;
            if (writes && depth) depth[idx] = d;
        }
    }
}

void mote_scene_raster(uint16_t *fb, int y0, int y1) {
    /* Logical band -> physical band. */
    int py0 = y0 * MOTE_SS, py1 = y1 * MOTE_SS;
    if (py0 < 0) py0 = 0;
    if (py1 > MOTE_FB_PH) py1 = MOTE_FB_PH;

    mote_raster_set_fb(fb);
    mote_depth_clear(py0, py1);

    /* Background fill for this band: a game callback (gradient/starfield) if
     * registered, else the solid background colour. */
    if (s_bg_cb) {
        s_bg_cb(fb, y0, y1);
    } else {
        for (int y = py0; y < py1; y++) {
            uint16_t *row = fb + y * MOTE_FB_PW;
            for (int x = 0; x < MOTE_FB_PW; x++) row[x] = s_bg;
        }
    }

    const float ss = (float)MOTE_SS;
    for (int i = 0; i < s_ntris; i++) {
        const ScreenTri *t = &s_tris[i];
        uint8_t blend = (uint8_t)MOTE_FLAGS_BLEND(t->flags);
        if (blend) {   /* translucent flat mesh (water/glass): blended fill */
            raster_tex_tri(t->ax * ss, t->ay * ss, t->az,
                           t->bx * ss, t->by * ss, t->bz,
                           t->cx * ss, t->cy * ss, t->cz,
                           0, 0, 0, 0, 0, 0, 0, t->color, 255, blend,
                           !(t->flags & MOTE_DRAW_NO_DEPTH_WRITE), fb, py0, py1);
        } else if (t->flags & MOTE_DRAW_NO_DEPTH_WRITE)
            mote_tri_nowrite(t->ax * ss, t->ay * ss, t->az,
                   t->bx * ss, t->by * ss, t->bz,
                   t->cx * ss, t->cy * ss, t->cz,
                   t->color, py0, py1);
        else
            mote_tri(t->ax * ss, t->ay * ss, t->az,
                   t->bx * ss, t->by * ss, t->bz,
                   t->cx * ss, t->cy * ss, t->cz,
                   t->color, py0, py1);
    }
    /* Textured (UV-mapped) mesh triangles. */
    for (int i = 0; i < s_ntextris; i++) {
        const ScreenTexTri *t = &s_textris[i];
        raster_tex_tri(t->ax * ss, t->ay * ss, t->ad,
                       t->bx * ss, t->by * ss, t->bd,
                       t->cx * ss, t->cy * ss, t->cd,
                       t->au, t->av, t->bu, t->bv, t->cu, t->cv,
                       t->tex, 0, t->shade, (uint8_t)MOTE_FLAGS_BLEND(t->flags),
                       !(t->flags & MOTE_DRAW_NO_DEPTH_WRITE), fb, py0, py1);
    }

    /* Soft ground-shadow decals: darken the framebuffer with a radial r^4
     * falloff. Drawn after the table/scene tris but before the impostor balls,
     * so a ball paints over its own shadow. Depth-tested per pixel against the
     * decal's own ground plane (interpolated inv-z): only the felt darkens, not
     * the cue/cushions standing above it. */
    if (s_nshadows > 0) {
        uint16_t *depth = mote_depth_buffer();
        const float dk = mote_pipe_depth_k();
        for (int i = 0; i < s_nshadows; i++) {
            const ScreenShadow *s = &s_shadows[i];
            float cx = s->cx * ss, cy = s->cy * ss;
            float ux = s->ux * ss, uy = s->uy * ss, vx = s->vx * ss, vy = s->vy * ss;
            float det = ux * vy - uy * vx;
            if (det > -1e-4f && det < 1e-4f) continue;
            float inv = 1.0f / det, st = s->strength, b0 = 1.0f - st;
            int bx = (int)(fabsf(ux) + fabsf(vx)) + 1, by = (int)(fabsf(uy) + fabsf(vy)) + 1;
            int x0 = (int)cx - bx, x1 = (int)cx + bx, ya = (int)cy - by, yb = (int)cy + by;
            if (x0 < 0) x0 = 0; if (x1 > MOTE_FB_PW - 1) x1 = MOTE_FB_PW - 1;
            if (ya < py0) ya = py0; if (yb > py1 - 1) yb = py1 - 1;
            for (int py = ya; py <= yb; py++) {
                uint16_t *frow = fb + py * MOTE_FB_PW;
                uint16_t *drow = depth ? depth + py * MOTE_FB_PW : 0;
                float ry = py + 0.5f - cy;
                for (int px = x0; px <= x1; px++) {
                    float rx = px + 0.5f - cx;
                    float sp = ( rx * vy - ry * vx) * inv;
                    float tp = (-rx * uy + ry * ux) * inv;
                    float r2 = sp * sp + tp * tp;
                    if (r2 > 1.0f) continue;
                    if (drow) {   /* skip pixels nearer than the ground (cue, cushions, rims) */
                        float gd = dk * (s->iz_c + sp * s->diz_u + tp * s->diz_v);
                        if ((float)drow[px] > gd + gd * 0.03f + 2.0f) continue;
                    }
                    frow[px] = shade565(frow[px], b0 + st * r2 * r2);
                }
            }
        }
    }

    /* Sphere impostors (after tris; depth-tested + writing). */
    if (s_nspheres > 0) {
        Vec3 sun = mote_pipe_sun_view();
        uint16_t *depth = mote_depth_buffer();
        const float nearz = mote_pipe_near(), dk = mote_pipe_depth_k();
        for (int i = 0; i < s_nspheres; i++) {
            const ScreenSphere *s = &s_spheres[i];
            float cx = s->sx * ss, cy = s->sy * ss, r = s->sr * ss;
            if (r < 0.5f) continue;
            int minx = (int)(cx - r); if (minx < 0) minx = 0;
            int maxx = (int)(cx + r) + 1; if (maxx > MOTE_FB_PW) maxx = MOTE_FB_PW;
            int miny = (int)(cy - r); if (miny < py0) miny = py0;
            int maxy = (int)(cy + r) + 1; if (maxy > py1) maxy = py1;
            float invr = 1.0f / r;
            for (int y = miny; y < maxy; y++) {
                float ndy = (y + 0.5f - cy) * invr;
                uint16_t *frow = fb + y * MOTE_FB_PW;
                uint16_t *drow = depth + y * MOTE_FB_PW;
                for (int x = minx; x < maxx; x++) {
                    float ndx = (x + 0.5f - cx) * invr;
                    float rr = ndx * ndx + ndy * ndy;
                    if (rr > 1.0f) continue;
                    float nz = sqrtf(1.0f - rr);
                    /* view-space front-hemisphere normal = (ndx, -ndy, -nz) */
                    float ndotl = ndx * sun.x - ndy * sun.y - nz * sun.z;
                    float sh = 0.22f + (ndotl > 0.0f ? 0.78f * ndotl : 0.0f);
                    float zf = s->vz - s->radius * nz;
                    if (zf < nearz) zf = nearz;
                    uint16_t d = (uint16_t)(dk / zf);
                    int idx = x;
                    if (d > drow[idx]) { drow[idx] = d; frow[idx] = shade565(s->color, sh); }
                }
            }
        }
    }

    /* Textured / oriented sphere impostors (opaque, depth-tested + writing). */
    if (s_ntexsph > 0) {
        Vec3 sun = mote_pipe_sun_world();
        const Mat3 *cam = mote_pipe_camera();
        Vec3 vcam = v3_scale(cam->r[2], -1.0f);          /* world dir toward camera */
        Vec3 H = v3_norm(v3_add(sun, vcam));
        uint16_t *depth = mote_depth_buffer();
        const float nearz = mote_pipe_near(), dk = mote_pipe_depth_k();
        for (int i = 0; i < s_ntexsph; i++) {
            const ScreenSphereTex *s = &s_texsph[i];
            float cx = s->sx * ss, cy = s->sy * ss, r = s->sr * ss;
            if (r < 0.5f) continue;
            int minx = (int)(cx - r); if (minx < 0) minx = 0;
            int maxx = (int)(cx + r) + 1; if (maxx > MOTE_FB_PW) maxx = MOTE_FB_PW;
            int miny = (int)(cy - r); if (miny < py0) miny = py0;
            int maxy = (int)(cy + r) + 1; if (maxy > py1) maxy = py1;
            float invr = 1.0f / r;
            for (int y = miny; y < maxy; y++) {
                float ndy = (y + 0.5f - cy) * invr;
                uint16_t *frow = fb + y * MOTE_FB_PW;
                uint16_t *drow = depth + y * MOTE_FB_PW;
                for (int x = minx; x < maxx; x++) {
                    float ndx = (x + 0.5f - cx) * invr;
                    float rr = ndx * ndx + ndy * ndy;
                    if (rr > 1.0f) continue;
                    float nz = sqrtf(1.0f - rr);
                    float zf = s->vz - s->radius * nz; if (zf < nearz) zf = nearz;
                    uint16_t d = (uint16_t)(dk / zf);
                    if (d <= drow[x]) continue;
                    Vec3 Nv = v3(ndx, -ndy, -nz);            /* view-space normal */
                    Vec3 Nw = m3_mul_v3(cam, Nv);            /* view -> world */
                    Vec3 Nl = m3_mul_v3_t(&s->orient, Nw);   /* world -> local */
                    float diff = v3_dot(Nw, sun); if (diff < 0.0f) diff = 0.0f;
                    float spec = v3_dot(Nw, H);   if (spec < 0.0f) spec = 0.0f;
                    drow[x] = d;
                    frow[x] = tex_shade(s->tex, Nl, Nw, diff, spec, nz);
                }
            }
        }
    }

    /* 3D sprite billboards: camera-facing textured quads, depth-tested. Opaque
     * (BLEND_NONE) billboards write depth so they occlude later FX; blended ones
     * (glows, soft sprites) test-only. Nearest-neighbour scaled blit. */
    if (s_nbbs > 0) {
        uint16_t *depth = mote_depth_buffer();
        for (int i = 0; i < s_nbbs; i++) {
            const ScreenBillboard *q = &s_bbs[i];
            const MoteImage *img = q->img;
            int x0 = (int)((q->cx - q->hw) * ss), x1 = (int)((q->cx + q->hw) * ss);
            int y0 = (int)((q->cy - q->hh) * ss), y1 = (int)((q->cy + q->hh) * ss);
            int pw = x1 - x0, ph = y1 - y0;
            if (pw < 1 || ph < 1) continue;
            uint16_t d = q->d, key = img->key;
            int keyed = !img->opaque;
            uint8_t bl = q->blend;
            int writes = (bl == MOTE_BLEND_NONE);
            int cx0 = x0 < 0 ? 0 : x0, cx1 = x1 > MOTE_FB_PW ? MOTE_FB_PW : x1;
            int cy0 = y0 < py0 ? py0 : y0, cy1 = y1 > py1 ? py1 : y1;
            float du = (float)q->fw / (float)pw, dv = (float)q->fh / (float)ph;
            for (int py = cy0; py < cy1; py++) {
                int sv = q->fy + (int)((py - y0) * dv);
                const uint16_t *srow = img->pixels + (size_t)sv * img->w + q->fx;
                int base = py * MOTE_FB_PW;
                for (int px = cx0; px < cx1; px++) {
                    int idx = base + px;
                    if (depth && d <= depth[idx]) continue;     /* behind scene */
                    int su = (int)((px - x0) * du);
                    uint16_t src = img->format ? mote_img_texel(img, q->fx + su, sv) : srow[su];
                    if (keyed && src == key) continue;
                    fb[idx] = writes ? src : blend565(fb[idx], src, bl);
                    if (writes && depth) depth[idx] = d;
                }
            }
        }
    }

    /* Depth-tested FX primitives, after opaque geometry: discs (glows/fireballs),
     * then lines (beams), then points (particles). They test depth but don't
     * write it, so they layer over the scene without occluding each other. */
    for (int i = 0; i < s_ndiscs; i++) {
        const ScreenDisc *q = &s_discs[i];
        mote_disc((int)(q->cx * ss), (int)(q->cy * ss), q->d, (int)(q->r * ss), q->color, py0, py1);
    }
    /* Billboard ring outlines (ghost balls, reticles): midpoint circle, depth-tested. */
    if (s_nrings > 0) {
        uint16_t *depth = mote_depth_buffer();
        for (int i = 0; i < s_nrings; i++) {
            const ScreenRing *q = &s_rings[i];
            int cx = (int)(q->cx * ss), cy = (int)(q->cy * ss), rad = (int)(q->r * ss);
            if (rad < 1) continue;
            uint16_t d = q->d, col = q->color;
            int x = rad, y = 0, err = 1 - rad;
            #define MOTE_RPLOT(PX,PY) do { int _x=(PX),_y=(PY); \
                if (_y>=py0 && _y<py1 && (unsigned)_x<(unsigned)MOTE_FB_PW) { \
                    int _i=_y*MOTE_FB_PW+_x; if (!depth || d > depth[_i]) fb[_i]=col; } } while(0)
            while (x >= y) {
                MOTE_RPLOT(cx+x,cy+y); MOTE_RPLOT(cx+y,cy+x);
                MOTE_RPLOT(cx-y,cy+x); MOTE_RPLOT(cx-x,cy+y);
                MOTE_RPLOT(cx-x,cy-y); MOTE_RPLOT(cx-y,cy-x);
                MOTE_RPLOT(cx+y,cy-x); MOTE_RPLOT(cx+x,cy-y);
                y++; if (err < 0) err += 2*y+1; else { x--; err += 2*(y-x)+1; }
            }
            #undef MOTE_RPLOT
        }
    }
    for (int i = 0; i < s_nlines; i++) {
        const ScreenLine *q = &s_lines[i];
        mote_line(q->x0 * ss, q->y0 * ss, q->d0, q->x1 * ss, q->y1 * ss, q->d1, q->color, py0, py1);
    }
    for (int i = 0; i < s_npoints; i++) {
        const ScreenPoint *q = &s_points[i];
        int sz = (int)q->size * MOTE_SS; if (sz < 1) sz = 1;
        int half = sz >> 1;
        mote_point((int)(q->x * ss) - half, (int)(q->y * ss) - half, q->d, q->color, sz, py0, py1);
    }
}
