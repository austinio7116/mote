/*
 * CueVR — drawing a real table in a real room.
 *
 * GLES 3.0, one program with a mode uniform, and colour handled exactly as the
 * console app handles it: everything lives in linear from sample to submit, and
 * whether the final encode happens here or in the swapchain is asked, not
 * assumed (see mote_xr_target_is_srgb).
 *
 * The table is not modelled here at all. cue_render.c already builds it — the
 * cloth bed fanned over the real knuckle boundary so pocket mouths are true
 * gaps, the K66 cushion cross-section with its overhanging nose and undercut,
 * the splayed jaw facings, the pocket voids, the drop lips, the baulk line and
 * the D — and those shapes, tuned table by table, ARE the game. So CueVR calls
 * cue_render_build_table() and uploads the triangles it produces straight to
 * GL. Same geometry the handheld draws, on all seven tables; the only thing
 * that changes is who rasterises it.
 *
 * Balls are one unit sphere drawn many times, each with its own CueBall.orient
 * — the physics integrates a real orientation matrix from the angular velocity,
 * so screw, follow and side are all visible on the ball as it runs. Stripes and
 * spots are computed in object space in the fragment shader, which means they
 * roll correctly and cost no texture at all.
 */
#include "cuevr.h"
#include "cuevr_render.h"
#include "cue_render.h"
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
"layout(location=3) in vec3 a_col;\n"
"uniform mat4 u_mvp;\n"
"uniform mat4 u_model;\n"
"out vec3 v_nrm;\n"
"out vec2 v_uv;\n"
"out vec3 v_local;\n"
"out vec3 v_col;\n"
"void main() {\n"
"    v_uv = a_uv;\n"
"    v_col = a_col;\n"
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
"in vec3 v_col;\n"
"uniform sampler2D u_tex;\n"
"uniform int   u_mode;\n"
"uniform int   u_encode;\n"
"uniform vec4  u_colour;\n"
"uniform vec4  u_colour2;\n"   /* balls: the stripe/secondary colour */
"uniform vec3  u_light;\n"
"uniform float u_ballslice;\n" /* which id's slice of the ball atlas */
"uniform float u_ballslices;\n"
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
"        // A ball, painted by the handheld's own ball_sample() baked into an\n"
"        // equirectangular atlas. v_local is the unit sphere in the BALL's\n"
"        // frame, so the lookup turns with the ball and you can read the spin\n"
"        // off it exactly as on a table.\n"
"        vec3 nb = normalize(v_local);\n"
"        float u = atan(nb.z, nb.x) / 6.2831853 + 0.5;\n"
"        float v = acos(clamp(nb.y, -1.0, 1.0)) / 3.14159265;\n"
"        v = (u_ballslice + clamp(v, 0.001, 0.999)) / u_ballslices;\n"
"        vec3 c = to_linear(texture(u_tex, vec2(u, v)).rgb);\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        float spec = pow(max(dot(v_nrm, normalize(L + vec3(0.0, 1.0, 0.0))), 0.0), 48.0);\n"
"        float bounce = max(-v_nrm.y, 0.0) * 0.10;\n"
"        o_col = emit(c * (0.42 + 0.62 * d + bounce) + vec3(spec) * 0.40, 1.0, 1.0);\n"
"    } else if (u_mode == 5) {\n"
"        // The cue. v_uv.y is the fraction along it (0 = tip) and v_uv.x runs\n"
"        // around it, which is all a hand-spliced cue needs: leather, ferrule,\n"
"        // ash, four ebony points and an ebony butt, with grain along the\n"
"        // shaft. No texture, and it stays sharp with the tip a hand's width\n"
"        // from your eye.\n"
"        float t = v_uv.y, a = v_uv.x;\n"
"        vec3 ash   = vec3(0.86, 0.74, 0.54);\n"
"        vec3 ebony = vec3(0.085, 0.065, 0.055);\n"
"        vec3 c;\n"
"        float gloss = 42.0, spec_k = 0.30;\n"
"        if (t < 0.0069) { c = vec3(0.42, 0.55, 0.62); gloss = 8.0; spec_k = 0.05; }\n"      // leather tip
"        else if (t < 0.0221) { c = vec3(0.93, 0.91, 0.84); gloss = 70.0; spec_k = 0.45; }\n" // ferrule
"        else {\n"
"            // Grain: fine rings along the shaft, plus a slow wander so it is\n"
"            // not a barcode.\n"
"            float g = sin(t * 520.0 + sin(a * 6.2831853) * 1.7) * 0.5 + 0.5;\n"
"            c = mix(ash * 0.90, ash * 1.06, g);\n"
"            // Four-point hand splice: ebony points running up out of the butt,\n"
"            // each narrowing to nothing at the top of the splice.\n"
"            float sp0 = 0.586, sp1 = 0.800;\n"   // where the points start and end
"            if (t > sp0) {\n"
"                float k = clamp((t - sp0) / (sp1 - sp0), 0.0, 1.0);\n"
"                float halfw = mix(0.125, 0.0, k * k);\n"      // width tapers to a point
"                float f = fract(a * 4.0);\n"                   // four points
"                float d = min(f, 1.0 - f);\n"
"                c = mix(c, ebony, smoothstep(halfw, halfw * 0.72, d));\n"
"            }\n"
"            if (t > sp1) c = ebony;\n"                        // solid butt above the splice
"            if (t > 0.9793 && t < 0.9931) c = vec3(0.62, 0.50, 0.22);\n"  // brass collar
"        }\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        float spec = pow(max(dot(v_nrm, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), gloss);\n"
"        o_col = emit(to_linear(c) * (0.34 + 0.70 * d) + vec3(spec) * spec_k, 1.0, 1.0);\n"
"    } else if (u_mode == 4) {\n"
"        // The table, exactly as cue_render.c built it: flat-shaded triangles\n"
"        // carrying their own colours, which already encode the cloth, the\n"
"        // shaded undercut, the wood, the pocket voids and the drop lips.\n"
"        // Lighting here is gentle so those authored tones survive.\n"
"        vec3 c = to_linear(v_col);\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        o_col = emit(c * (0.68 + 0.42 * d), 1.0, 1.0);\n"
"    } else {\n"
"        vec3 c = to_linear(u_colour.rgb);\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        o_col = emit(c * (0.42 + 0.62 * d), u_colour.a, 1.0);\n"
"    }\n"
"}\n";

/* ---- mesh building ------------------------------------------------------ */

typedef struct { float p[3], n[3], uv[2], c[3]; } Vtx;

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
    t->c[0]=t->c[1]=t->c[2]=1.0f;
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
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)32);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
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
    GLint  u_mvp, u_model, u_tex, u_mode, u_encode, u_colour, u_colour2, u_light;
    GLint  u_ballslice, u_ballslices;
    Mesh   table, lips, ball, cue, quad, floor;
    GLuint ball_tex;      /* equirect atlas, one slice per ball id */
    GLuint hud_tex;
    int    encode;
    int    ready;
    CueTable tab;
    void *tab_buf, *stri_buf;
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

/* A cue, turned on a lathe.
 *
 * The one thing here that is NOT the handheld's — its cue is drawn in screen
 * space as part of the aim overlay, so there is no mesh to borrow. A cue is
 * also the object closest to your face for the whole game, so it earns the
 * geometry.
 *
 * Built from a real cue's profile along its 1.45 m: a 10 mm leather tip, a
 * brass-white ferrule, an ash shaft swelling from 9.5 mm to 19 mm, then the
 * ebony butt out to 29 mm with a rounded cap. The four-point hand splice and
 * the grain are shaded in cuevr's fragment shader from the axial and angular
 * coordinates carried in uv, so the cue needs no texture at all.
 *
 * y = 0 is the TIP and +Y runs back to the butt, which is how it is positioned:
 * the tip goes where the cue line meets the ball and the rest follows behind,
 * through and past your hands, as a real cue does. */
#define CUE_LEN 1.45f

/* radius (m) at a fraction along the cue, tip to butt */
static float cue_radius(float t) {
    const float x = t * CUE_LEN;
    if (x < 0.010f) return 0.0050f;                                  /* tip */
    if (x < 0.032f) return 0.0051f;                                  /* ferrule */
    if (x < 1.100f) {                                                /* ash shaft */
        float k = (x - 0.032f) / (1.100f - 0.032f);
        return 0.0051f + k * (0.0095f - 0.0051f);
    }
    if (x < 1.410f) {                                                /* ebony butt */
        float k = (x - 1.100f) / (1.410f - 1.100f);
        return 0.0095f + k * (0.0145f - 0.0095f);
    }
    /* rounded butt cap */
    float k = (x - 1.410f) / (CUE_LEN - 1.410f);
    if (k > 1.0f) k = 1.0f;
    return 0.0145f * sqrtf(1.0f - k * k * 0.95f);
}

static void build_cue(Builder *b, int slices, int rings) {
    for (int j = 0; j <= rings; j++) {
        /* Rings bunch towards the tip, where the profile actually changes and
         * where your eye is. */
        float f = (float)j / rings;
        float t = f * f * 0.55f + f * 0.45f;
        float y = t * CUE_LEN, r = cue_radius(t);
        float r2 = cue_radius(t + 0.004f > 1.0f ? 1.0f : t + 0.004f);
        float slope = (r2 - r) / (0.004f * CUE_LEN);
        for (int i = 0; i <= slices; i++) {
            float u = (float)i / slices, th = u * 2.0f * PI;
            float cx = cosf(th), cz = sinf(th);
            /* normal follows the taper, so the shaft is not lit like a cylinder */
            float nl = 1.0f / sqrtf(1.0f + slope * slope);
            b_vert(b, cx * r, y, cz * r, cx * nl, -slope * nl, cz * nl, u, t);
        }
    }
    for (int j = 0; j < rings; j++)
        for (int i = 0; i < slices; i++) {
            int a = j * (slices + 1) + i, c = a + slices + 1;
            b_quad(b, a, a + 1, c + 1, c);
        }
}

/* The table, exactly as the handheld builds it.
 *
 * cue_render_build_table() emits a world-space triangle soup — cloth bed fanned
 * over the real knuckle boundary, K66 cushion cross-sections with the
 * overhanging nose and its undercut, splayed jaw facings, pocket voids, drop
 * lips, baulk line, D and spots — each triangle carrying its own authored
 * colour. Those shapes were tuned table by table and they are what the game is,
 * so nothing here reshapes them: the triangles are copied into a vertex buffer
 * and handed to GL as they come.
 *
 * The handheld's own draw order is preserved too: [0, lip) normally, then the
 * drop lips last with depth writes off, so a ball sitting in a pocket covers
 * them rather than being covered.
 */
static void build_from_cue_render(Builder *b, const CueTri *tri_, int from, int to) {
    for (int i = from; i < to; i++) {
        const CueTri *t = &tri_[i];
        float r = ((t->color >> 11) & 31) / 31.0f;
        float g = ((t->color >> 5) & 63) / 63.0f;
        float bl = (t->color & 31) / 31.0f;
        int idx[3];
        for (int k = 0; k < 3; k++) {
            idx[k] = b_vert(b, t->v[k].x, t->v[k].y, t->v[k].z,
                            t->nrm.x, t->nrm.y, t->nrm.z, 0.0f, 0.0f);
            Vtx *vx = &b->v[idx[k]];
            vx->c[0] = r; vx->c[1] = g; vx->c[2] = bl;
        }
        b_tri(b, idx[0], idx[1], idx[2]);
    }
}

/* The balls, exactly as the handheld paints them.
 *
 * cue_render shades every ball through ball_sample(id, nb, base), a function of
 * the BALL-LOCAL normal — which is to say it is already a texture over the
 * sphere, complete with the numbers, the stripes, the spots and whichever of
 * the five authored ball sets is selected. So bake it: sample that function
 * over an equirectangular grid once per ball id at start-up and upload the lot
 * as one atlas, a slice per id. The fragment shader then looks up the same
 * colour the handheld would have computed, and because the lookup is by
 * ball-local direction it rotates with the ball's own orientation matrix —
 * the markings roll exactly as they should.
 *
 * An earlier version of this file invented its own palette and drew stripes
 * with a smoothstep. It looked plausible and it was not the game. */
#define BTEX_W   64
#define BTEX_H   32
#define BTEX_IDS 24            /* 0..15 pool, 20..23+ snooker colours */

static void bake_ball_atlas(void) {
    uint16_t *px = malloc((size_t)BTEX_W * BTEX_H * BTEX_IDS * sizeof(uint16_t));
    if (!px) { LOGI("[cuevr] no memory for the ball atlas"); return; }
    for (int id = 0; id < BTEX_IDS; id++) {
        uint16_t *slice = px + (size_t)id * BTEX_W * BTEX_H;
        for (int y = 0; y < BTEX_H; y++) {
            float phi = ((float)y + 0.5f) / BTEX_H * PI;          /* 0 = +Y pole */
            for (int x = 0; x < BTEX_W; x++) {
                float th = (((float)x + 0.5f) / BTEX_W - 0.5f) * 2.0f * PI;
                Vec3 nb;
                nb.x = sinf(phi) * cosf(th);
                nb.y = cosf(phi);
                nb.z = sinf(phi) * sinf(th);
                slice[y * BTEX_W + x] = cue_render_ball_texel((uint8_t)id, nb);
            }
        }
    }
    glGenTextures(1, &G.ball_tex);
    glBindTexture(GL_TEXTURE_2D, G.ball_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, BTEX_W, BTEX_H * BTEX_IDS, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* Wrap in u so the seam behind the ball closes; clamp in v so a slice
     * cannot bleed into its neighbour at the poles. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    LOGI("[cuevr] baked %d ball surfaces from cue_render", BTEX_IDS);
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
    G.u_ballslice  = glGetUniformLocation(G.prog, "u_ballslice");
    G.u_ballslices = glGetUniformLocation(G.prog, "u_ballslices");
    G.encode = target_is_srgb ? 0 : 1;
    G.tab = *t;

    cuevr_render_set_table(t, w);

    Builder b;
    b_init(&b, 4096, 12288);
    build_sphere(&b, 24, 16);
    mesh_upload(&G.ball, &b);
    b_free(&b);

    b_init(&b, 20 * 64 + 64, 20 * 64 * 6 + 64);
    build_cue(&b, 20, 48);
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

    bake_ball_atlas();

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
    mesh_free(&G.table); mesh_free(&G.lips);

    /* cue_render keeps its two big buffers outside itself (on the handheld they
     * live in the engine arena); here they are just malloc'd once. */
    if (!G.tab_buf) {
        G.tab_buf  = malloc(cue_render_tab_bytes());
        G.stri_buf = malloc(cue_render_stri_bytes());
        if (!G.tab_buf || !G.stri_buf) { LOGI("[cuevr] out of memory for the table mesh"); return; }
        cue_render_set_buffers(G.tab_buf, G.stri_buf);
    }
    cue_render_build_table(t, w);

    const CueTri *tris = NULL;
    int bed = 0, lip = 0;
    int n = cue_render_table_tris(&tris, &bed, &lip);
    if (!tris || n <= 0) { LOGI("[cuevr] the table mesh came back empty"); return; }

    Builder b;
    b_init(&b, lip * 3 + 8, lip * 3 + 8);
    build_from_cue_render(&b, tris, 0, lip);
    mesh_upload(&G.table, &b);
    b_free(&b);

    int nlip = n - lip;
    if (nlip > 0) {
        b_init(&b, nlip * 3 + 8, nlip * 3 + 8);
        build_from_cue_render(&b, tris, lip, n);
        mesh_upload(&G.lips, &b);
        b_free(&b);
    }
    LOGI("[cuevr] table mesh %d tris (%d bed, %d lip) from cue_render", n, bed, nlip);
}

void cuevr_render_shutdown(void) {
    if (!G.ready) return;
    mesh_free(&G.table); mesh_free(&G.lips); mesh_free(&G.ball);
    mesh_free(&G.cue); mesh_free(&G.quad); mesh_free(&G.floor);
    glDeleteTextures(1, &G.hud_tex);
    glDeleteTextures(1, &G.ball_tex);
    free(G.tab_buf); free(G.stri_buf); G.tab_buf = G.stri_buf = NULL;
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

    /* No backface culling on the table. cue_render's mesh is authored for a
     * software rasteriser that does not cull — the cloth fan, the cushion
     * faces and the pocket voids are wound for shading, not for a front-face
     * convention — so culling it silently drops the bed and half the rails.
     * Depth sorts it correctly regardless. */
    glDisable(GL_CULL_FACE);
    glUniform1i(G.u_mode, 4);          /* vertex colours, as authored */
    set_model(T);
    draw(&G.table);

    /* ---- balls ---- */
    glUniform1i(G.u_mode, 1);
    glBindTexture(GL_TEXTURE_2D, G.ball_tex);
    glUniform1f(G.u_ballslices, (float)BTEX_IDS);
    for (int i = 0; i < s->nballs; i++) {
        const CueBall *bl = &s->balls[i];
        if (!bl->on) continue;
        int slice = bl->id < BTEX_IDS ? bl->id : 0;
        glUniform1f(G.u_ballslice, (float)slice);

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

    /* The pocket drop lips last, with depth writes off — the handheld's own
     * order, so a ball resting in a pocket covers the lip rather than the lip
     * drawing across it. */
    if (G.lips.n) {
        glDepthMask(GL_FALSE);
        glUniform1i(G.u_mode, 4);
        set_model(T);
        draw(&G.lips);
        glDepthMask(GL_TRUE);
    }
    glEnable(GL_CULL_FACE);

    /* ---- the cue ---- */
    if (s->cue_visible) {
        glUniform1i(G.u_mode, 5);
        /* The mesh is a real cue, 1.45 m with y=0 at the tip. So put the tip
         * where the cue line meets the ball and run +Y back along the shaft —
         * past your hands and out behind you, as a real cue does. It is NOT
         * stretched between the controllers: a cue is a fixed length, and
         * making it rubbery is the fastest way to stop believing in it. */
        MoteVrV3 d = mv3_sub(s->cue_butt, s->cue_tip);
        float len = mv3_len(d);
        if (len > 0.02f) {
            MoteVrV3 u = mv3_scale(d, 1.0f / len);
            MoteVrV3 up = mv3(0, 1, 0);
            MoteVrV3 ax = mv3_cross(up, u);
            float s_ = mv3_len(ax), c_ = mv3_dot(up, u);
            MoteVrQ q = (s_ < 1e-5f)
                ? (c_ > 0.0f ? mq_ident() : mq_axis_angle(mv3(1,0,0), PI))
                : mq_axis_angle(ax, atan2f(s_, c_));
            MoteVrPose cp; cp.p = s->cue_tip; cp.q = q;
            float M[16];
            mm4_from_pose(M, cp, 1.0f);
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
