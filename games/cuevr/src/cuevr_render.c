/*
 * CueVR — drawing a real table in a real room.
 *
 * GLES 3.0, one program with a mode uniform, and colour handled exactly as the
 * console app handles it: everything lives in linear from sample to submit, and
 * whether the final encode happens here or in the swapchain is asked, not
 * assumed (see mote_xr_target_is_srgb).
 *
 * The table geometry is *built from the physics world*, not modelled beside it.
 * cue_table.c fills a CueWorld with the cushion nose segments, the jaw knuckles
 * and the pocket circles the balls actually collide with; the mesh here is
 * extruded from those same segments. So the cushion you can see and the cushion
 * the ball bounces off are the same numbers, on all seven tables, and a pocket
 * that looks tight is tight. There is no second source of truth to drift.
 *
 * Balls are one unit sphere drawn many times, each with its own CueBall.orient
 * — the physics integrates a real orientation matrix from the angular velocity,
 * so screw, follow and side are all visible on the ball as it runs. Stripes and
 * spots are computed in object space in the fragment shader, which means they
 * roll correctly and cost no texture at all.
 */
#include "cuevr.h"
#include "cuevr_render.h"
#include "craft_font.h"

#include <GLES3/gl3.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

#define PI 3.14159265358979f

/* ---- shaders ------------------------------------------------------------ */

static const char *VS =
"#version 300 es\n"
"layout(location=0) in vec3 a_pos;\n"
"layout(location=1) in vec3 a_nrm;\n"
"layout(location=2) in vec2 a_uv;\n"
"uniform mat4 u_mvp;\n"
"uniform mat4 u_model;\n"
"out vec3 v_nrm;\n"
"out vec2 v_uv;\n"
"out vec3 v_local;\n"
"void main() {\n"
"    v_uv = a_uv;\n"
"    v_local = a_pos;\n"
"    v_nrm = normalize(mat3(u_model) * a_nrm);\n"
"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
"}\n";

/* u_mode: 0 lit flat colour, 1 ball, 2 unlit textured (HUD), 3 room grid */
static const char *FS =
"#version 300 es\n"
"precision highp float;\n"
"in vec3 v_nrm;\n"
"in vec2 v_uv;\n"
"in vec3 v_local;\n"
"uniform sampler2D u_tex;\n"
"uniform int   u_mode;\n"
"uniform int   u_encode;\n"
"uniform vec4  u_colour;\n"
"uniform vec4  u_colour2;\n"   /* balls: the stripe/secondary colour */
"uniform vec3  u_light;\n"
"uniform float u_ballkind;\n"  /* 0 solid, 1 striped, 2 cue (spots) */
"out vec4 o_col;\n"
"vec3 to_linear(vec3 c) {\n"
"    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
"}\n"
"vec3 to_srgb(vec3 c) {\n"
"    c = max(c, 0.0);\n"
"    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0/2.4)) - 0.055, step(0.0031308, c));\n"
"}\n"
"float hash12(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
"vec4 emit(vec3 c, float a, float dither) {\n"
"    vec3 o = u_encode == 1 ? to_srgb(c) : c;\n"
"    return vec4(o + (hash12(gl_FragCoord.xy) - 0.5) * dither / 255.0, a);\n"
"}\n"
"void main() {\n"
"    vec3 L = normalize(u_light);\n"
"    if (u_mode == 2) {\n"
"        vec4 t = texture(u_tex, v_uv);\n"
"        o_col = emit(to_linear(t.rgb), t.a * u_colour.a, 0.0);\n"
"    } else if (u_mode == 3) {\n"
"        vec2 g = abs(fract(v_local.xz * 2.0) - 0.5) / fwidth(v_local.xz * 2.0);\n"
"        float line = 1.0 - min(min(g.x, g.y), 1.0);\n"
"        float fade = clamp(1.0 - length(v_local.xz) / 7.0, 0.0, 1.0);\n"
"        o_col = emit(to_linear(u_colour.rgb) * line * fade, line * fade, 1.0);\n"
"    } else if (u_mode == 1) {\n"
"        // A ball. v_local is the unit sphere in the BALL's own frame, so the\n"
"        // markings turn with it and you can read the spin off a rolling ball\n"
"        // exactly as you can on a table.\n"
"        vec3 base = to_linear(u_colour.rgb);\n"
"        vec3 mark = to_linear(u_colour2.rgb);\n"
"        vec3 c = base;\n"
"        if (u_ballkind > 1.5) {\n"
"            // cue ball: two small spots, so its own spin is legible too\n"
"            float d = min(length(v_local - vec3(0.0, 0.92, 0.0)),\n"
"                          length(v_local - vec3(0.0,-0.92, 0.0)));\n"
"            c = mix(mark, base, smoothstep(0.24, 0.30, d));\n"
"        } else if (u_ballkind > 0.5) {\n"
"            // striped: white ball with a band around its equator\n"
"            c = mix(base, mark, smoothstep(0.56, 0.50, abs(v_local.y)));\n"
"        }\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        // A polished phenolic ball: broad diffuse, one tight highlight, and a\n"
"        // little bounce off the cloth so the underside is not dead black.\n"
"        vec3 h = normalize(L + vec3(0.0, 0.0, 1.0));\n"
"        float spec = pow(max(dot(v_nrm, normalize(L + vec3(0.0, 1.0, 0.0))), 0.0), 48.0);\n"
"        float bounce = max(-v_nrm.y, 0.0) * 0.10;\n"
"        o_col = emit(c * (0.30 + 0.72 * d + bounce) + vec3(spec) * 0.45, 1.0, 1.0);\n"
"    } else {\n"
"        vec3 c = to_linear(u_colour.rgb);\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        o_col = emit(c * (0.42 + 0.62 * d), u_colour.a, 1.0);\n"
"    }\n"
"}\n";

/* ---- mesh building ------------------------------------------------------ */

typedef struct { float p[3], n[3], uv[2]; } Vtx;

typedef struct {
    Vtx     *v;
    uint16_t *i;
    int nv, ni, cap_v, cap_i;
} Builder;

static void b_init(Builder *b, int cv, int ci) {
    b->v = malloc(sizeof(Vtx) * (size_t)cv);
    b->i = malloc(sizeof(uint16_t) * (size_t)ci);
    b->nv = b->ni = 0; b->cap_v = cv; b->cap_i = ci;
}
static void b_free(Builder *b) { free(b->v); free(b->i); b->v = NULL; b->i = NULL; }

static int b_vert(Builder *b, float x, float y, float z,
                  float nx, float ny, float nz, float u, float v) {
    if (b->nv >= b->cap_v) return b->nv ? b->nv - 1 : 0;
    Vtx *t = &b->v[b->nv];
    t->p[0]=x; t->p[1]=y; t->p[2]=z;
    t->n[0]=nx; t->n[1]=ny; t->n[2]=nz;
    t->uv[0]=u; t->uv[1]=v;
    return b->nv++;
}
static void b_tri(Builder *b, int a, int c, int d) {
    if (b->ni + 3 > b->cap_i) return;
    b->i[b->ni++] = (uint16_t)a; b->i[b->ni++] = (uint16_t)c; b->i[b->ni++] = (uint16_t)d;
}
static void b_quad(Builder *b, int a, int c, int d, int e) { b_tri(b, a, c, d); b_tri(b, a, d, e); }

/* A flat quad with an explicit normal.
 *
 * The winding is derived rather than trusted. Every face here is described by
 * four corners and the direction it faces, and getting the corner order to
 * agree with that direction by hand — for a cloth in the XZ plane, a cushion
 * face leaning at a pocket angle, a rail skirt — is a mistake waiting at every
 * call site, and it fails silently: the face simply is not there, and you go
 * looking for it in the shader. So compare the corners' own winding against the
 * normal and swap if they disagree. Then a face is always visible from the side
 * it says it faces. */
static void b_face(Builder *b, const float *p0, const float *p1,
                   const float *p2, const float *p3, const float *n) {
    float u[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float v[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
    float c[3] = { u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0] };
    int flip = (c[0]*n[0] + c[1]*n[1] + c[2]*n[2]) < 0.0f;
    const float *q[4] = { p0, p1, p2, p3 };
    int idx[4];
    for (int k = 0; k < 4; k++) {
        const float *p = q[flip ? 3 - k : k];
        idx[k] = b_vert(b, p[0], p[1], p[2], n[0], n[1], n[2],
                        (k == 1 || k == 2) ? 1.0f : 0.0f,
                        (k >= 2) ? 1.0f : 0.0f);
    }
    b_quad(b, idx[0], idx[1], idx[2], idx[3]);
}

typedef struct { GLuint vao, vbo, ibo; int n; } Mesh;

static void mesh_upload(Mesh *m, const Builder *b) {
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->nv * (GLsizeiptr)sizeof(Vtx), b->v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)24);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)b->ni * 2, b->i, GL_STATIC_DRAW);
    m->n = b->ni;
    glBindVertexArray(0);
}

static void mesh_free(Mesh *m) {
    if (!m->vao) return;
    glDeleteBuffers(1, &m->vbo);
    glDeleteBuffers(1, &m->ibo);
    glDeleteVertexArrays(1, &m->vao);
    memset(m, 0, sizeof *m);
}

/* ---- state -------------------------------------------------------------- */

static struct {
    GLuint prog;
    GLint  u_mvp, u_model, u_tex, u_mode, u_encode, u_colour, u_colour2, u_light, u_ballkind;
    Mesh   cloth, cushions, rails, pockets, ball, cue, quad, floor;
    GLuint hud_tex;
    int    encode;
    int    ready;
    CueTable tab;
} G;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(s, sizeof l, NULL, l); LOGI("[cuevr] shader: %s", l); return 0; }
    return s;
}

/* ---- the table ---------------------------------------------------------- */

/* A UV sphere of unit radius. Enough segments that a ball 26 mm across still
 * reads as round with your eye 40 cm from it. */
static void build_sphere(Builder *b, int slices, int stacks) {
    for (int j = 0; j <= stacks; j++) {
        float v = (float)j / stacks, phi = v * PI;
        for (int i = 0; i <= slices; i++) {
            float u = (float)i / slices, th = u * 2.0f * PI;
            float x = sinf(phi) * cosf(th), y = cosf(phi), z = sinf(phi) * sinf(th);
            b_vert(b, x, y, z, x, y, z, u, v);
        }
    }
    for (int j = 0; j < stacks; j++)
        for (int i = 0; i < slices; i++) {
            int a = j * (slices + 1) + i, c = a + slices + 1;
            b_quad(b, a, c, c + 1, a + 1);
        }
}

/* A cone/cylinder along +Y from y=0 (r0) to y=1 (r1) — the cue stick. */
static void build_taper(Builder *b, int slices, float r0, float r1) {
    for (int i = 0; i <= slices; i++) {
        float u = (float)i / slices, th = u * 2.0f * PI;
        float cx = cosf(th), cz = sinf(th);
        b_vert(b, cx * r0, 0.0f, cz * r0, cx, 0.0f, cz, u, 0.0f);
        b_vert(b, cx * r1, 1.0f, cz * r1, cx, 0.0f, cz, u, 1.0f);
    }
    for (int i = 0; i < slices; i++) {
        int a = i * 2;
        b_quad(b, a, a + 2, a + 3, a + 1);
    }
}

/* The cloth bed: one quad at y = 0, plus a skirt so the bed has thickness when
 * you look at it edge-on from a low chair. */
static void build_cloth(Builder *b, const CueTable *t) {
    float L = t->half_len, W = t->half_wid;
    float p0[3] = {-L, 0, -W}, p1[3] = { L, 0, -W}, p2[3] = { L, 0, W}, p3[3] = {-L, 0, W};
    float up[3] = {0, 1, 0};
    b_face(b, p0, p1, p2, p3, up);
}

/* The cushions, extruded from the physics world's own nose segments.
 *
 * Each CueSeg is a line in the X-Z plane with an inward normal. A real cushion
 * is a wedge: the nose stands at cushion_h and the face slopes back and down to
 * the cloth, with a flat top running outward to the rail. Building that from
 * the collision segments — rather than from a second set of numbers — is what
 * guarantees that a pocket which looks tight plays tight.
 */
static void build_cushions(Builder *b, const CueTable *t, const CueWorld *w) {
    const float h = t->cushion_h;
    const float top_out = t->rail_w * 0.55f;   /* how far the flat top runs out */
    const float undercut = h * 0.45f;          /* how far the face leans back */

    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *g = &w->seg[s];
        /* Interpolated end normals keep the chain continuous around the jaws. */
        float na[3] = { g->na.x, 0, g->na.z }, nb[3] = { g->nb.x, 0, g->nb.z };
        float ax = g->a.x, az = g->a.z, bx = g->b.x, bz = g->b.z;

        /* nose (top of the face), bed (bottom, pushed outward), top-outer */
        float n_a[3] = { ax, h, az }, n_b[3] = { bx, h, bz };
        float d_a[3] = { ax - na[0]*(-undercut), 0.0f, az - na[2]*(-undercut) };
        float d_b[3] = { bx - nb[0]*(-undercut), 0.0f, bz - nb[2]*(-undercut) };
        float o_a[3] = { ax - na[0]*top_out, h, az - na[2]*top_out };
        float o_b[3] = { bx - nb[0]*top_out, h, bz - nb[2]*top_out };

        /* The playing face, which is what the ball meets. Normal points in. */
        float fn[3] = { (na[0]+nb[0])*0.5f, 0.30f, (na[2]+nb[2])*0.5f };
        float fl = sqrtf(fn[0]*fn[0]+fn[1]*fn[1]+fn[2]*fn[2]);
        if (fl > 1e-6f) { fn[0]/=fl; fn[1]/=fl; fn[2]/=fl; }
        b_face(b, d_a, d_b, n_b, n_a, fn);

        /* The flat top. */
        float up[3] = {0, 1, 0};
        b_face(b, n_a, n_b, o_b, o_a, up);
    }
}

/* The rail frame: a box around the outside of the playing area, at cushion
 * height, in the darker rail colour. Drawn as four slabs so the corners meet. */
static void build_rails(Builder *b, const CueTable *t) {
    float L = t->half_len, W = t->half_wid, rw = t->rail_w, h = t->cushion_h;
    float top = h * 1.06f;
    float ex = L + rw, ez = W + rw;
    float up[3] = {0,1,0};
    struct { float x0, z0, x1, z1; } slab[4] = {
        { -ex, -ez,  ex, -W - rw*0.45f },
        { -ex,  W + rw*0.45f,  ex,  ez },
        { -ex, -ez, -L - rw*0.45f,  ez },
        {  L + rw*0.45f, -ez,  ex,  ez },
    };
    for (int k = 0; k < 4; k++) {
        float x0 = slab[k].x0, z0 = slab[k].z0, x1 = slab[k].x1, z1 = slab[k].z1;
        float a[3]={x0,top,z0}, c[3]={x1,top,z0}, d[3]={x1,top,z1}, e[3]={x0,top,z1};
        b_face(b, a, c, d, e, up);
        /* outer skirt down to the floor line, so the table has a body */
        float depth = 0.18f;
        float s0[3]={x0,top,z0}, s1[3]={x1,top,z0}, s2[3]={x1,top-depth,z0}, s3[3]={x0,top-depth,z0};
        float nz[3]={0,0,-1};
        b_face(b, s3, s2, s1, s0, nz);
        float t0[3]={x0,top,z1}, t1[3]={x1,top,z1}, t2[3]={x1,top-depth,z1}, t3[3]={x0,top-depth,z1};
        float pz[3]={0,0,1};
        b_face(b, t0, t1, t2, t3, pz);
    }
}

/* Pocket mouths: a dark disc sunk just under the cloth at each pocket centre.
 * Cheap, and from any angle you actually play from it reads as a hole. */
static void build_pockets(Builder *b, const CueWorld *w) {
    const int seg = 20;
    for (int p = 0; p < w->npocket; p++) {
        float cx = w->pocket[p].x, cz = w->pocket[p].z, r = w->pocket_r[p];
        int centre = b_vert(b, cx, -0.004f, cz, 0, 1, 0, 0.5f, 0.5f);
        int first = 0;
        for (int i = 0; i <= seg; i++) {
            float th = (float)i / seg * 2.0f * PI;
            int v = b_vert(b, cx + cosf(th)*r, -0.010f, cz + sinf(th)*r, 0, 1, 0,
                           0.5f + cosf(th)*0.5f, 0.5f + sinf(th)*0.5f);
            if (i == 0) first = v;
            else b_tri(b, centre, v - 1, v);
        }
        (void)first;
    }
}

/* ---- init --------------------------------------------------------------- */

int cuevr_render_init(const CueTable *t, const CueWorld *w, int target_is_srgb) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return -1;
    G.prog = glCreateProgram();
    glAttachShader(G.prog, vs);
    glAttachShader(G.prog, fs);
    glLinkProgram(G.prog);
    GLint ok = 0;
    glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
    if (!ok) { char l[1024]; glGetProgramInfoLog(G.prog, sizeof l, NULL, l); LOGI("[cuevr] link: %s", l); return -1; }
    glDeleteShader(vs); glDeleteShader(fs);

    G.u_mvp      = glGetUniformLocation(G.prog, "u_mvp");
    G.u_model    = glGetUniformLocation(G.prog, "u_model");
    G.u_tex      = glGetUniformLocation(G.prog, "u_tex");
    G.u_mode     = glGetUniformLocation(G.prog, "u_mode");
    G.u_encode   = glGetUniformLocation(G.prog, "u_encode");
    G.u_colour   = glGetUniformLocation(G.prog, "u_colour");
    G.u_colour2  = glGetUniformLocation(G.prog, "u_colour2");
    G.u_light    = glGetUniformLocation(G.prog, "u_light");
    G.u_ballkind = glGetUniformLocation(G.prog, "u_ballkind");
    G.encode = target_is_srgb ? 0 : 1;
    G.tab = *t;

    cuevr_render_set_table(t, w);

    Builder b;
    b_init(&b, 4096, 12288);
    build_sphere(&b, 24, 16);
    mesh_upload(&G.ball, &b);
    b_free(&b);

    b_init(&b, 256, 512);
    build_taper(&b, 16, 0.0065f, 0.014f);      /* tip 13 mm, butt 28 mm */
    mesh_upload(&G.cue, &b);
    b_free(&b);

    b_init(&b, 8, 12);
    {   float p0[3]={-0.5f,-0.5f,0}, p1[3]={0.5f,-0.5f,0}, p2[3]={0.5f,0.5f,0}, p3[3]={-0.5f,0.5f,0};
        float n[3]={0,0,1};
        int a = b_vert(&b, p0[0],p0[1],p0[2], n[0],n[1],n[2], 0,1);
        int c = b_vert(&b, p1[0],p1[1],p1[2], n[0],n[1],n[2], 1,1);
        int d = b_vert(&b, p2[0],p2[1],p2[2], n[0],n[1],n[2], 1,0);
        int e = b_vert(&b, p3[0],p3[1],p3[2], n[0],n[1],n[2], 0,0);
        b_quad(&b, a, c, d, e);
    }
    mesh_upload(&G.quad, &b);
    b_free(&b);

    b_init(&b, 8, 12);
    {   float s = 8.0f;
        float p0[3]={-s,0,-s}, p1[3]={s,0,-s}, p2[3]={s,0,s}, p3[3]={-s,0,s};
        float n[3]={0,1,0};
        b_face(&b, p0, p1, p2, p3, n);
    }
    mesh_upload(&G.floor, &b);
    b_free(&b);

    glGenTextures(1, &G.hud_tex);
    glBindTexture(GL_TEXTURE_2D, G.hud_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, CUEVR_HUD_W, CUEVR_HUD_H, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    G.ready = 1;
    LOGI("[cuevr] table %.2f x %.2f m, %d cushion segs, %d pockets",
         t->half_len * 2, t->half_wid * 2, w->nseg, w->npocket);
    return 0;
}

void cuevr_render_set_table(const CueTable *t, const CueWorld *w) {
    G.tab = *t;
    mesh_free(&G.cloth); mesh_free(&G.cushions); mesh_free(&G.rails); mesh_free(&G.pockets);
    Builder b;
    b_init(&b, 64, 96);      build_cloth(&b, t);        mesh_upload(&G.cloth, &b);    b_free(&b);
    b_init(&b, CUE_MAX_SEG * 16, CUE_MAX_SEG * 24); build_cushions(&b, t, w);
    mesh_upload(&G.cushions, &b); b_free(&b);
    b_init(&b, 512, 768);    build_rails(&b, t);        mesh_upload(&G.rails, &b);    b_free(&b);
    b_init(&b, 256, 384);    build_pockets(&b, w);      mesh_upload(&G.pockets, &b);  b_free(&b);
}

void cuevr_render_shutdown(void) {
    if (!G.ready) return;
    mesh_free(&G.cloth); mesh_free(&G.cushions); mesh_free(&G.rails);
    mesh_free(&G.pockets); mesh_free(&G.ball); mesh_free(&G.cue);
    mesh_free(&G.quad); mesh_free(&G.floor);
    glDeleteTextures(1, &G.hud_tex);
    glDeleteProgram(G.prog);
    G.ready = 0;
}

void cuevr_render_hud(const uint16_t *px) {
    if (!G.ready) return;
    glBindTexture(GL_TEXTURE_2D, G.hud_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CUEVR_HUD_W, CUEVR_HUD_H,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
}

/* ---- drawing ------------------------------------------------------------ */

static float VP[16];

static void set_model(const float *m) {
    float mvp[16];
    mm4_mul(mvp, VP, m);
    glUniformMatrix4fv(G.u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(G.u_model, 1, GL_FALSE, m);
}

static void draw(const Mesh *m) {
    if (!m->n) return;
    glBindVertexArray(m->vao);
    glDrawElements(GL_TRIANGLES, m->n, GL_UNSIGNED_SHORT, 0);
}

static void colour(float r, float g, float b, float a) { glUniform4f(G.u_colour, r, g, b, a); }

/* rgb565 -> 0..1 floats, so the table's own palette drives the 3D colours too */
static void colour565(uint16_t c, float mul) {
    float r = ((c >> 11) & 31) / 31.0f, g = ((c >> 5) & 63) / 63.0f, b = (c & 31) / 31.0f;
    colour(r * mul, g * mul, b * mul, 1.0f);
}

/* Ball colours. Pool: 1-7 solids, 8 black, 9-15 the same hues striped.
 * Snooker: the six colours by id. */
static void ball_colours(int id, int snooker, float *base, float *mark, float *kind) {
    static const float POOL[8][3] = {
        {0.98f,0.98f,0.96f},          /* 0 cue */
        {0.98f,0.78f,0.10f},          /* 1 yellow */
        {0.10f,0.28f,0.78f},          /* 2 blue */
        {0.85f,0.12f,0.12f},          /* 3 red */
        {0.38f,0.12f,0.58f},          /* 4 purple */
        {0.95f,0.45f,0.08f},          /* 5 orange */
        {0.05f,0.45f,0.22f},          /* 6 green */
        {0.55f,0.12f,0.16f},          /* 7 maroon */
    };
    kind[0] = 0.0f;
    mark[0] = mark[1] = mark[2] = 0.98f;
    if (id == CUE_ID_CUE) {
        base[0]=POOL[0][0]; base[1]=POOL[0][1]; base[2]=POOL[0][2];
        mark[0]=0.85f; mark[1]=0.15f; mark[2]=0.12f;    /* red spots */
        kind[0] = 2.0f;
        return;
    }
    if (snooker) {
        static const float SNK[6][3] = {
            {0.95f,0.80f,0.15f}, {0.06f,0.45f,0.20f}, {0.45f,0.28f,0.10f},
            {0.10f,0.30f,0.80f}, {0.95f,0.55f,0.65f}, {0.05f,0.05f,0.06f},
        };
        if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) {
            const float *c = SNK[id - CUE_ID_YELLOW];
            base[0]=c[0]; base[1]=c[1]; base[2]=c[2];
        } else {                                        /* a red */
            base[0]=0.80f; base[1]=0.09f; base[2]=0.09f;
        }
        return;
    }
    if (id == 8) { base[0]=0.05f; base[1]=0.05f; base[2]=0.06f; return; }
    int hue = id > 8 ? id - 8 : id;
    if (hue < 1) hue = 1;
    if (hue > 7) hue = 7;
    if (id > 8) {                                       /* striped */
        base[0]=0.97f; base[1]=0.97f; base[2]=0.95f;
        mark[0]=POOL[hue][0]; mark[1]=POOL[hue][1]; mark[2]=POOL[hue][2];
        kind[0] = 1.0f;
    } else {
        base[0]=POOL[hue][0]; base[1]=POOL[hue][1]; base[2]=POOL[hue][2];
    }
}

void cuevr_render_eye(const float *view, const float *proj,
                      const CueVrScene *s, int draw_room)
{
    if (!G.ready) return;
    mm4_mul(VP, proj, view);

    glUseProgram(G.prog);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(G.u_tex, 0);
    glUniform1i(G.u_encode, G.encode);
    glUniform1f(G.u_ballkind, 0.0f);
    /* Overhead and slightly behind, like the light over a real table. */
    glUniform3f(G.u_light, -0.25f, 0.90f, 0.36f);

    /* The table's own transform: yaw about vertical, then out into the room. */
    float T[16];
    {
        MoteVrPose pose;
        pose.p = s->place->pos;
        pose.q = mq_axis_angle(mv3(0, 1, 0), s->place->yaw);
        mm4_from_pose(T, pose, 1.0f);
    }

    if (draw_room) {
        glUniform1i(G.u_mode, 3);
        colour(0.35f, 0.42f, 0.55f, 1.0f);
        float F[16];
        mm4_identity(F);
        F[13] = s->place->pos.y - s->place->height;   /* the room's floor */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        set_model(F);
        draw(&G.floor);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
    }

    glUniform1i(G.u_mode, 0);
    set_model(T);
    colour565(G.tab.cloth, 1.0f);   draw(&G.cloth);
    colour565(G.tab.cloth, 0.86f);  draw(&G.cushions);
    colour565(G.tab.rail,  1.0f);   draw(&G.rails);
    colour(0.015f, 0.012f, 0.010f, 1.0f); draw(&G.pockets);

    /* ---- balls ---- */
    glUniform1i(G.u_mode, 1);
    for (int i = 0; i < s->nballs; i++) {
        const CueBall *bl = &s->balls[i];
        if (!bl->on) continue;
        float base[3], mark[3], kind;
        ball_colours(bl->id, G.tab.is_snooker, base, mark, &kind);
        glUniform4f(G.u_colour,  base[0], base[1], base[2], 1.0f);
        glUniform4f(G.u_colour2, mark[0], mark[1], mark[2], 1.0f);
        glUniform1f(G.u_ballkind, kind);

        /* model = table * translate(ball) * orient * scale(R) */
        float B[16], M[16];
        const Mat3 *o = &bl->orient;
        float R = G.tab.R;
        B[0]=o->r[0].x*R; B[1]=o->r[0].y*R; B[2]=o->r[0].z*R; B[3]=0;
        B[4]=o->r[1].x*R; B[5]=o->r[1].y*R; B[6]=o->r[1].z*R; B[7]=0;
        B[8]=o->r[2].x*R; B[9]=o->r[2].y*R; B[10]=o->r[2].z*R; B[11]=0;
        B[12]=bl->pos.x; B[13]=bl->pos.y; B[14]=bl->pos.z; B[15]=1;
        mm4_mul(M, T, B);
        set_model(M);
        draw(&G.ball);
    }
    glUniform1f(G.u_ballkind, 0.0f);

    /* ---- the cue ---- */
    if (s->cue_visible) {
        glUniform1i(G.u_mode, 0);
        colour(0.68f, 0.50f, 0.28f, 1.0f);
        /* A unit taper runs along +Y; aim it down the cue's axis and stretch it
         * from the butt to the tip. */
        MoteVrV3 a = s->cue_butt, b = s->cue_tip;
        MoteVrV3 d = mv3_sub(b, a);
        float len = mv3_len(d);
        if (len > 0.02f) {
            MoteVrV3 u = mv3_scale(d, 1.0f / len);
            MoteVrV3 up = mv3(0, 1, 0);
            MoteVrV3 ax = mv3_cross(up, u);
            float s_ = mv3_len(ax), c_ = mv3_dot(up, u);
            MoteVrQ q = (s_ < 1e-5f)
                ? (c_ > 0.0f ? mq_ident() : mq_axis_angle(mv3(1,0,0), PI))
                : mq_axis_angle(ax, atan2f(s_, c_));
            MoteVrPose cp; cp.p = a; cp.q = q;
            float C[16], S[16], M[16];
            mm4_from_pose(C, cp, 1.0f);
            mm4_identity(S); S[5] = len;      /* stretch along its own +Y */
            mm4_mul(M, C, S);
            set_model(M);
            draw(&G.cue);
        }
    }

    /* ---- the HUD panel ---- */
    if (s->hud_visible) {
        glUniform1i(G.u_mode, 2);
        glUniform4f(G.u_colour, 1, 1, 1, 1);
        glBindTexture(GL_TEXTURE_2D, G.hud_tex);
        glDisable(GL_CULL_FACE);
        float P[16];
        MoteVrPose hp;
        hp.p = s->hud_pos;
        hp.q = s->hud_rot;
        mm4_from_pose(P, hp, 1.0f);
        float S[16];
        mm4_identity(S);
        S[0] = s->hud_w;
        S[5] = s->hud_w * (float)CUEVR_HUD_H / (float)CUEVR_HUD_W;
        float M[16];
        mm4_mul(M, P, S);
        set_model(M);
        draw(&G.quad);
        glEnable(GL_CULL_FACE);
    }

    glBindVertexArray(0);
}
