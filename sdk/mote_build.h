/*
 * mote_build.h — developer convenience layer (header-only, no ABI cost).
 *
 * Build meshes the SAFE way: you give world-unit dimensions and a colour, and
 * these pick the quantisation scale, wind faces CCW-from-outside, and compute
 * per-face normals for you — no more int8 overflow, back-face-cull surprises, or
 * hand-rolled cross products. Meshes are allocated from the load-time arena, so
 * call these in init() and keep the returned pointer.
 *
 *   const Mesh *box   = mote_mesh_box(mote, 0.5f,1.0f,0.4f, col);
 *   const Mesh *ball  = mote_mesh_sphere(mote, 0.4f, 12, col);
 *   const Mesh *pawn  = mote_mesh_revolve(mote, profile, n, 10, col);
 *
 * Plus a camera helper (mote_camera_look) and a tiny immediate-mode UI.
 */
#ifndef MOTE_BUILD_H
#define MOTE_BUILD_H

#include "mote_api.h"
#include <math.h>

/* ---- camera: build a view basis from eye->target (subtract eye yourself for
 * the camera-relative object positions). ---- */
static inline Mat3 mote_camera_look(Vec3 eye, Vec3 target) {
    Vec3 f = v3_norm(v3_sub(target, eye));
    Vec3 r = v3_norm(v3_cross(v3(0, 1, 0), f));
    Mat3 m; m.r[0] = r; m.r[1] = v3_cross(f, r); m.r[2] = f; return m;
}

/* ---- mesh building ---- */
/* Add a triangle from CCW-from-outside indices; the outward normal is computed
 * from the (quantised) vertices, so you never set normals by hand. */
static inline void mote__face(MeshVert *v, MeshFace *f, int *nf, int a, int b, int c, uint16_t col) {
    (void)col;   /* colour now lives on the Mesh (these builders are single-colour; set m->color) */
    Vec3 pa = v3((float)v[a].x, (float)v[a].y, (float)v[a].z);
    Vec3 pb = v3((float)v[b].x, (float)v[b].y, (float)v[b].z);
    Vec3 pc = v3((float)v[c].x, (float)v[c].y, (float)v[c].z);
    Vec3 n = v3_norm(v3_cross(v3_sub(pb, pa), v3_sub(pc, pa)));
    MeshFace *o = &f[(*nf)++];
    o->a = (uint8_t)a; o->b = (uint8_t)b; o->c = (uint8_t)c;
    o->nx = (int8_t)(n.x * 127); o->ny = (int8_t)(n.y * 127); o->nz = (int8_t)(n.z * 127);
}
static inline void mote__qv(MeshVert *v, int i, float x, float y, float z, float sc) {
    v[i].x = (int8_t)(x / sc * 127); v[i].y = (int8_t)(y / sc * 127); v[i].z = (int8_t)(z / sc * 127);
}

/* Axis-aligned box, half-extents in world units. */
static inline const Mesh *mote_mesh_box(const MoteApi *api, float hx, float hy, float hz, uint16_t col) {
    MeshVert *v = (MeshVert *)api->alloc(8 * sizeof(MeshVert));
    MeshFace *f = (MeshFace *)api->alloc(12 * sizeof(MeshFace));
    Mesh *m = (Mesh *)api->alloc(sizeof(Mesh));
    if (!v || !f || !m) return 0;
    float sc = hx > hy ? hx : hy; if (hz > sc) sc = hz; if (sc < 1e-4f) sc = 1e-4f;
    float X[8] = {-hx, hx, hx, -hx, -hx, hx, hx, -hx};
    float Y[8] = {-hy, -hy, -hy, -hy, hy, hy, hy, hy};
    float Z[8] = {-hz, -hz, hz, hz, -hz, -hz, hz, hz};
    for (int i = 0; i < 8; i++) mote__qv(v, i, X[i], Y[i], Z[i], sc);
    int q[6][4] = {{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7},{4,5,6,7},{3,2,1,0}};
    int nf = 0;
    for (int s = 0; s < 6; s++) { mote__face(v, f, &nf, q[s][0], q[s][2], q[s][1], col);
                                  mote__face(v, f, &nf, q[s][0], q[s][3], q[s][2], col); }
    *m = (Mesh){v, f, 0, 8, 12, col, sc, sc * 1.8f, 0};
    return m;
}

/* Lathe a profile of {radius, height} pairs around the Y axis. A radius < 0.03
 * at either end is treated as a point (apex); otherwise the end is capped flat.
 * Handles the int8 cap by trimming segments if needed. */
static inline const Mesh *mote_mesh_revolve(const MoteApi *api, const float *prof, int n, int segs, uint16_t col) {
    if (segs < 3) segs = 3;
    int bot_apex = prof[0] < 0.03f, top_apex = prof[(n - 1) * 2] < 0.03f;
    int r0 = bot_apex ? 1 : 0, r1 = top_apex ? n - 1 : n, rings = r1 - r0;
    while (rings * segs > 250 && segs > 4) segs--;
    int maxv = rings * segs + 4, maxf = (rings - 1) * segs * 2 + segs * 2 + 8;
    MeshVert *v = (MeshVert *)api->alloc(maxv * sizeof(MeshVert));
    MeshFace *f = (MeshFace *)api->alloc(maxf * sizeof(MeshFace));
    Mesh *m = (Mesh *)api->alloc(sizeof(Mesh));
    if (!v || !f || !m) return 0;
    float sc = 1e-3f;
    for (int i = 0; i < n; i++) { float r = prof[i*2], y = fabsf(prof[i*2+1]); if (r > sc) sc = r; if (y > sc) sc = y; }
    int nv = 0;
    for (int i = 0; i < rings; i++) { float r = prof[(r0+i)*2], y = prof[(r0+i)*2+1];
        for (int s = 0; s < segs; s++) { float a = s * 6.2831853f / segs;
            mote__qv(v, nv++, r * cosf(a), y, r * sinf(a), sc); } }
    int nf = 0;
    for (int i = 0; i < rings - 1; i++) for (int s = 0; s < segs; s++) { int s2 = (s + 1) % segs;
        int a = i*segs+s, b = i*segs+s2, c = (i+1)*segs+s, d = (i+1)*segs+s2;
        mote__face(v, f, &nf, a, d, b, col); mote__face(v, f, &nf, a, c, d, col); }
    if (top_apex) { int ti = nv; mote__qv(v, nv++, 0, prof[(n-1)*2+1], 0, sc); int base = (rings-1)*segs;
        for (int s = 0; s < segs; s++) { int s2 = (s+1)%segs; mote__face(v, f, &nf, base+s2, base+s, ti, col); } }
    else { int ti = nv; mote__qv(v, nv++, 0, prof[(r1-1)*2+1], 0, sc); int base = (rings-1)*segs;
        for (int s = 0; s < segs; s++) { int s2 = (s+1)%segs; mote__face(v, f, &nf, ti, base+s2, base+s, col); } }
    if (bot_apex) { int bi = nv; mote__qv(v, nv++, 0, prof[1], 0, sc);
        for (int s = 0; s < segs; s++) { int s2 = (s+1)%segs; mote__face(v, f, &nf, bi, s, s2, col); } }
    else { int ci = nv; mote__qv(v, nv++, 0, prof[r0*2+1], 0, sc);
        for (int s = 0; s < segs; s++) { int s2 = (s+1)%segs; mote__face(v, f, &nf, ci, s, s2, col); } }
    *m = (Mesh){v, f, 0, (uint16_t)nv, (uint16_t)nf, col, sc, sc * 1.6f, 0};
    return m;
}
static inline const Mesh *mote_mesh_cylinder(const MoteApi *api, float r, float halfh, int segs, uint16_t col) {
    float p[4] = {r, -halfh, r, halfh}; return mote_mesh_revolve(api, p, 2, segs, col);
}
static inline const Mesh *mote_mesh_sphere(const MoteApi *api, float r, int segs, uint16_t col) {
    int K = segs / 2; if (K < 4) K = 4; if (K > 16) K = 16;
    float p[2 * 17];
    for (int i = 0; i <= K; i++) { float a = -1.5707963f + 3.14159265f * i / K; p[i*2] = r * cosf(a); p[i*2+1] = r * sinf(a); }
    return mote_mesh_revolve(api, p, K + 1, segs, col);
}

/* ---- heightfield terrain (auto-chunked past the 256-vert mesh cap) ---- */
typedef float    (*MoteHeightFn)(float x, float z, void *user);
typedef uint16_t (*MoteTerrColFn)(float x, float z, float ny, void *user);
/* Sample an nx*nz heightfield over [x0,z0]..[x1,z1] and emit it as one or more
 * arena-allocated render meshes, each <= 255 verts (the uint8 index cap). Fills
 * out[] (size max_out), returns the chunk count, and writes the grid CENTRE to
 * *center — render each chunk at (center - cam). hf gives height; cf gives the
 * RGB565 colour for a face (ny = its up-ness, for shading). */
static inline int mote_mesh_grid(const MoteApi *api, int nx, int nz,
        float x0, float z0, float x1, float z1,
        MoteHeightFn hf, MoteTerrColFn cf, void *user, const Mesh **out, int max_out, Vec3 *center) {
    float spanx = x1 - x0, spanz = z1 - z0;
    float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f, cy = hf(cx, cz, user);
    *center = v3(cx, cy, cz);
    float sc = (spanx > spanz ? spanx : spanz) * 0.5f; if (sc < 1e-3f) sc = 1e-3f;
    int rows = 255 / nx - 1; if (rows < 1) rows = 1;
    int produced = 0;
    for (int z0i = 0; z0i < nz - 1 && produced < max_out; z0i += rows) {
        int z1i = z0i + rows; if (z1i > nz - 1) z1i = nz - 1;
        int nr = z1i - z0i + 1, nv = nr * nx, nf = (nr - 1) * (nx - 1) * 2;
        MeshVert *v = (MeshVert *)api->alloc(nv * sizeof(MeshVert));
        MeshFace *f = (MeshFace *)api->alloc(nf * sizeof(MeshFace));
        uint16_t *fc = (uint16_t *)api->alloc(nf * sizeof(uint16_t));   /* per-face terrain colour */
        Mesh *m = (Mesh *)api->alloc(sizeof(Mesh));
        if (!v || !f || !fc || !m) break;
        for (int r = 0; r < nr; r++) for (int gx = 0; gx < nx; gx++) {
            float wx = x0 + spanx * gx / (nx - 1), wz = z0 + spanz * (z0i + r) / (nz - 1), wy = hf(wx, wz, user);
            mote__qv(v, r * nx + gx, wx - cx, wy - cy, wz - cz, sc);
        }
        int nff = 0;
        for (int r = 0; r < nr - 1; r++) for (int gx = 0; gx < nx - 1; gx++) {
            int a = r*nx+gx, b = a+1, c = a+nx, d = c+1;
            int tri[2][3] = {{a,c,b},{b,c,d}};
            for (int t = 0; t < 2; t++) {
                int i0=tri[t][0],i1=tri[t][1],i2=tri[t][2];
                Vec3 p0=v3(v[i0].x,v[i0].y,v[i0].z),p1=v3(v[i1].x,v[i1].y,v[i1].z),p2=v3(v[i2].x,v[i2].y,v[i2].z);
                Vec3 n=v3_norm(v3_cross(v3_sub(p1,p0),v3_sub(p2,p0)));
                float mx=x0+spanx*(gx+0.5f)/(nx-1), mz=z0+spanz*(z0i+r+0.5f)/(nz-1);
                fc[nff] = cf ? cf(mx,mz,n.y,user) : MOTE_RGB565(96,150,86);
                f[nff]=(MeshFace){(uint8_t)i0,(uint8_t)i1,(uint8_t)i2,(int8_t)(n.x*127),(int8_t)(n.y*127),(int8_t)(n.z*127)};
                nff++;
            }
        }
        *m = (Mesh){v, f, fc, (uint16_t)nv, (uint16_t)nf, 0, sc, sc * 1.2f, 0};
        out[produced++] = m;
    }
    return produced;
}

/* ---- tiny immediate-mode UI (pure framebuffer; pair with mote->text) ---- */
static inline void mote_ui_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) { if ((unsigned)j >= MOTE_FB_H) continue;
        for (int i = x; i < x + w; i++) { if ((unsigned)i >= MOTE_FB_W) continue; fb[j * MOTE_FB_W + i] = c; } }
}
static inline void mote_ui_panel(uint16_t *fb, int x, int y, int w, int h, uint16_t bg, uint16_t border) {
    mote_ui_rect(fb, x, y, w, h, bg);
    mote_ui_rect(fb, x, y, w, 1, border); mote_ui_rect(fb, x, y + h - 1, w, 1, border);
    mote_ui_rect(fb, x, y, 1, h, border); mote_ui_rect(fb, x + w - 1, y, 1, h, border);
}
static inline void mote_ui_bar(uint16_t *fb, int x, int y, int w, int h, float frac, uint16_t fg, uint16_t bg) {
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    mote_ui_rect(fb, x, y, w, h, bg);
    mote_ui_rect(fb, x, y, (int)(frac * w), h, fg);
}
/* int -> string, returns length (saves every game re-rolling itoa). */
static inline int mote_itoa(int n, char *o) {
    char t[12]; int p = 0, q = 0; if (n < 0) { o[q++] = '-'; n = -n; }
    if (n == 0) t[p++] = '0'; while (n) { t[p++] = (char)('0' + n % 10); n /= 10; }
    while (p) o[q++] = t[--p]; o[q] = 0; return q;
}

/* ===================================================================== */
/* Convenience helpers added to smooth the sharpest edges (all header-only, */
/* no ABI change). Use them or ignore them.                                */
/* ===================================================================== */
#include <stdarg.h>

/* ---- printf-style HUD text — replaces the pervasive buf[q++]=… + mote_itoa
 * boilerplate. Supports %d %i %u %x %c %s %f (%f = 2 decimals) and %%.
 *   mote_textf(mote, fb, 4, 4, white, "FPS %d  pos %.2f", fps, x);          */
static inline int mote__ftoa(float f, char *o, int dec) {
    int q = 0; if (f < 0) { o[q++] = '-'; f = -f; }
    int scale = 1; for (int i = 0; i < dec; i++) scale *= 10;
    long whole = (long)f; long frac = (long)((f - (float)whole) * (float)scale + 0.5f);
    if (frac >= scale) { whole++; frac -= scale; }
    q += mote_itoa((int)whole, o + q);
    if (dec > 0) { o[q++] = '.'; for (int s = scale / 10; s >= 1; s /= 10) o[q++] = (char)('0' + (frac / s) % 10); }
    o[q] = 0; return q;
}
static inline int mote_vtextf(const MoteApi *mote, uint16_t *fb, int x, int y, uint16_t col, const char *fmt, va_list ap) {
    char b[128]; int q = 0;
    for (const char *p = fmt; *p && q < 120; p++) {
        if (*p != '%') { b[q++] = *p; continue; }
        p++;
        switch (*p) {
            case 'd': case 'i': q += mote_itoa(va_arg(ap, int), b + q); break;
            case 'u': { unsigned u = va_arg(ap, unsigned); char t[12]; int tp = 0; if (!u) t[tp++] = '0'; while (u) { t[tp++] = (char)('0'+u%10); u/=10; } while (tp) b[q++] = t[--tp]; } break;
            case 'x': { unsigned u = va_arg(ap, unsigned); char t[12]; int tp = 0; if (!u) t[tp++]='0'; while (u) { unsigned d=u&15u; t[tp++]=(char)(d<10?'0'+d:'a'+d-10); u>>=4; } while (tp) b[q++]=t[--tp]; } break;
            case 'c': b[q++] = (char)va_arg(ap, int); break;
            case 's': { const char *s = va_arg(ap, const char *); while (*s && q < 120) b[q++] = *s++; } break;
            case 'f': q += mote__ftoa((float)va_arg(ap, double), b + q, 2); break;
            case '%': b[q++] = '%'; break;
            default: b[q++] = '%'; if (*p) b[q++] = *p; break;
        }
    }
    b[q] = 0; return mote->text(fb, b, x, y, col);
}
static inline int mote_textf(const MoteApi *mote, uint16_t *fb, int x, int y, uint16_t col, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = mote_vtextf(mote, fb, x, y, col, fmt, ap); va_end(ap); return r;
}

/* ---- sprites — build a MoteSprite without hand-ordering its 9 positional fields. */
static inline MoteSprite mote_sprite(const MoteImage *img, int x, int y) {
    MoteSprite s = {0}; s.img = img; s.x = (int16_t)x; s.y = (int16_t)y; s.fw = img->w; s.fh = img->h; return s;
}
static inline MoteSprite mote_sprite_cell(const MoteImage *img, int x, int y, int cw, int ch, int col, int row) {
    MoteSprite s = {0}; s.img = img; s.x = (int16_t)x; s.y = (int16_t)y;
    s.fx = (uint16_t)(col * cw); s.fy = (uint16_t)(row * ch); s.fw = (uint16_t)cw; s.fh = (uint16_t)ch; return s;
}
static inline int mote_sprite_add(const MoteApi *mote, const MoteImage *img, int x, int y) {
    MoteSprite s = mote_sprite(img, x, y); return mote->scene2d_add(&s);
}

/* ---- physics body factories — set the shape and AUTO-COMPUTE the box bounding
 * radius (forgetting it silently disables box collisions). mass <= 0 -> static. */
static inline MoteBody mote_body_sphere(Vec3 pos, float r, float mass) {
    MoteBody b = {0}; b.pos = pos; b.orient = m3_identity(); b.shape = MOTE_SHAPE_SPHERE;
    b.radius = r; b.inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f; return b;
}
static inline MoteBody mote_body_box(Vec3 pos, Vec3 half, float mass) {
    MoteBody b = {0}; b.pos = pos; b.orient = m3_identity(); b.shape = MOTE_SHAPE_BOX;
    b.half = half; b.radius = v3_len(half); b.inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f; return b;
}

/* ---- fixed timestep — run game logic at a steady rate regardless of frame dt:
 *   static MoteFixed t; mote_fixed_feed(&t, dt);
 *   while (mote_fixed_step(&t, 1.0f/60)) update_logic(1.0f/60);              */
typedef struct { float acc; } MoteFixed;
static inline void mote_fixed_feed(MoteFixed *f, float dt) { f->acc += dt; if (f->acc > 0.25f) f->acc = 0.25f; }
static inline int  mote_fixed_step(MoteFixed *f, float step) { if (f->acc >= step) { f->acc -= step; return 1; } return 0; }

/* ---- draw a mesh at a WORLD position. Use with scene_camera() (pass the camera position
 * once per frame) so you never hand-subtract the camera. mote_draw_ex adds orientation +
 * uniform scale. These replace the `MoteObject o={...}; scene_add_object(&o);` boilerplate. */
static inline int mote_draw(const MoteApi *m, const Mesh *mesh, Vec3 pos) {
    MoteObject o = { .pos = pos, .basis = m3_identity(), .mesh = mesh };
    return m->scene_add_object(&o);
}
static inline int mote_draw_ex(const MoteApi *m, const Mesh *mesh, Vec3 pos, Mat3 basis, float scale) {
    MoteObject o = { .pos = pos, .basis = basis, .mesh = mesh };
    return scale == 1.0f ? m->scene_add_object(&o) : m->scene_add_object_scaled(&o, scale);
}

/* ---- draw a whole baked MODEL (all its chunks) in one call. The baker splits an STL
 * into <=255-vert chunks; these loop over them so the game never sees the chunk array
 * or the count. Pair with `.max_tris = <name>_TRIS` so the draw-list pool fits exactly. */
static inline void mote_model_draw(const MoteApi *m, const MoteModel *model, Vec3 pos) {
    for (uint16_t i = 0; i < model->count; i++) {
        MoteObject o = { .pos = pos, .basis = m3_identity(), .mesh = &model->chunks[i] };
        m->scene_add_object(&o);
    }
}
static inline void mote_model_draw_ex(const MoteApi *m, const MoteModel *model, Vec3 pos, Mat3 basis, float scale) {
    for (uint16_t i = 0; i < model->count; i++) {
        MoteObject o = { .pos = pos, .basis = basis, .mesh = &model->chunks[i] };
        if (scale == 1.0f) m->scene_add_object(&o); else m->scene_add_object_scaled(&o, scale);
    }
}
/* As mote_model_draw_ex, but TINT every chunk with `color` (RGB565), ignoring the baked
 * colour — handy for team colours, damage flashes, selection highlights, palette swaps.
 * Pass a single mesh via mote_draw_tint. (color must be non-zero; 0 means "no override".) */
static inline void mote_draw_tint(const MoteApi *m, const Mesh *mesh, Vec3 pos, Mat3 basis, float scale, uint16_t color) {
    MoteObject o = { .pos = pos, .basis = basis, .mesh = mesh, .color = color };
    if (scale == 1.0f) m->scene_add_object(&o); else m->scene_add_object_scaled(&o, scale);
}
static inline void mote_model_draw_tint(const MoteApi *m, const MoteModel *model, Vec3 pos, Mat3 basis, float scale, uint16_t color) {
    for (uint16_t i = 0; i < model->count; i++)
        mote_draw_tint(m, &model->chunks[i], pos, basis, scale, color);
}

/* ---- bake a MoteSfx recipe into a playable MoteSound at load: the engine synthesises
 * the PCM into an arena buffer (measure, then render). Ship ~88-byte recipes instead of
 * WAVs; call once in init(), then mote->audio_play(&snd, gain). Returns {0,0} on failure.
 *
 * PREFER mote->audio_play_sfx(&recipe, gain) (ABI v37) for most SFX: it STREAMS the recipe
 * — synthesised on the fly by a dedicated voice pool — so it costs only the ~88-byte recipe
 * in flash and ~0 RAM, any length, up to 8 concurrent voices. That is the tiny-flash path
 * (a whole game's hand-tuned SFX set), and what Studio's "Save to assets" now recommends.
 *
 * mote_sfx_bake (this) renders the WHOLE clip into the arena up front: arena RAM = (sample
 * count × 2 bytes) PER sound (~13 KB for ~0.3 s) — reach for it only when you need the
 * finished PCM (e.g. to vary/resample it). The const <name>_snd PCM that Studio also bakes
 * (wav2snd, `mote->audio_play(&name_snd)`) is the 0-CPU alternative — bigger flash, no synth
 * cost at play time — for the rare game with very heavy simultaneous polyphony. */
static inline MoteSound mote_sfx_bake(const MoteApi *m, const MoteSfx *recipe) {
    MoteSound s = { 0, 0 };
    int n = m->audio_render_sfx(recipe, 0, 0);
    if (n <= 0) return s;
    int16_t *pcm = (int16_t *)m->alloc((uint32_t)n * sizeof(int16_t));
    if (!pcm) return s;
    m->audio_render_sfx(recipe, pcm, n);
    s.pcm = pcm; s.count = n; return s;
}

/* ---- one shared RNG, so games stop hand-rolling xorshift. Seed once (e.g. from
 * mote->micros()); mote_frand() is [0,1), mote_randf(lo,hi) a range, mote_rand() raw bits. */
static uint32_t mote__rng = 0x2545F491u;
static inline void     mote_rand_seed(uint32_t s) { mote__rng = s ? s : 1u; }
static inline uint32_t mote_rand(void) { mote__rng ^= mote__rng << 13; mote__rng ^= mote__rng >> 17; mote__rng ^= mote__rng << 5; return mote__rng; }
static inline float    mote_frand(void) { return (float)(mote_rand() & 0xFFFFFF) / (float)0x1000000; }
static inline float    mote_randf(float lo, float hi) { return lo + (hi - lo) * mote_frand(); }
static inline float    mote_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int      mote_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- reusable particle pool (drawn as impostor spheres). World positions, so it pairs
 * with scene_camera(). Set .gravity / .drag (0 -> default 0.92 per tick) before use, or
 * leave zeroed for drifting sparks. */
#ifndef MOTE_PARTICLES_MAX
#define MOTE_PARTICLES_MAX 48
#endif
typedef struct { Vec3 pos, vel; float life, life0; uint16_t col; } MoteParticle;
typedef struct { MoteParticle p[MOTE_PARTICLES_MAX]; Vec3 gravity; float drag; } MoteParticles;
static inline void mote_particles_burst(MoteParticles *s, Vec3 pos, uint16_t col, int count, float speed, float life) {
    for (int n = 0; n < count; n++)
        for (int i = 0; i < MOTE_PARTICLES_MAX; i++)
            if (s->p[i].life <= 0) {
                s->p[i].pos = pos;
                s->p[i].vel = v3(mote_randf(-speed, speed), mote_randf(-speed, speed), mote_randf(-speed, speed));
                s->p[i].life = s->p[i].life0 = life; s->p[i].col = col; break;
            }
}
static inline void mote_particles_tick(MoteParticles *s, float dt) {
    float drag = s->drag > 0 ? s->drag : 0.92f;
    for (int i = 0; i < MOTE_PARTICLES_MAX; i++)
        if (s->p[i].life > 0) {
            s->p[i].life -= dt;
            s->p[i].vel = v3_scale(v3_add(s->p[i].vel, v3_scale(s->gravity, dt)), drag);
            s->p[i].pos = v3_add(s->p[i].pos, v3_scale(s->p[i].vel, dt));
        }
}
static inline void mote_particles_draw(const MoteApi *m, const MoteParticles *s, float radius) {
    for (int i = 0; i < MOTE_PARTICLES_MAX; i++)
        if (s->p[i].life > 0) {
            float f = s->p[i].life0 > 0 ? s->p[i].life / s->p[i].life0 : 1.0f;
            m->scene_add_sphere(s->p[i].pos, radius * (0.35f + 0.65f * f), s->p[i].col);
        }
}

/* Launcher icon, handled by the build — not the game. If `mote bake` produced a
 * src/icon.h (from the game's icon.png), pull it in automatically so the icon travels
 * in the module; the game never #includes it. The baked symbol is weak, so this is
 * harmless even across a game's multiple .c files. No icon.h -> name-accent fallback. */
#if defined(__has_include)
#  if __has_include("icon.h")
#    include "icon.h"
#  endif
#endif

#endif /* MOTE_BUILD_H */
