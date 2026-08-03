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
#include "cuevr_frame.h"
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
"out vec3 v_world;\n"
"void main() {\n"
"    v_uv = a_uv;\n"
"    v_col = a_col;\n"
"    v_local = a_pos;\n"
"    v_world = (u_model * vec4(a_pos, 1.0)).xyz;\n"
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
"in vec3 v_world;\n"
"uniform sampler2D u_tex;\n"
"uniform int   u_mode;\n"
"uniform int   u_encode;\n"
"uniform vec4  u_colour;\n"
"uniform vec4  u_colour2;\n"   /* balls: the stripe/secondary colour */
"uniform vec3  u_light;\n"
"uniform vec3  u_lampC[4];\n"   // lamp centres, world space
"uniform vec3  u_lampX[4];\n"   // half-extent along the table's length
"uniform vec3  u_lampZ[4];\n"   // half-extent across it
"uniform int   u_nlamp;\n"
"uniform vec3  u_eye;\n"
"uniform vec3  u_clothsh;\n"    // cloth bounce tint

"uniform float u_ballslice;\n"  /* which layer of the ball array */
/* GLSL ES has no default precision for sampler2DArray — unlike sampler2D —
 * so it must be stated or the shader will not compile at all. */
"uniform highp sampler2DArray u_balls;\n"
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
"        // A ball, shaded the way the handheld shades it — this is\n"
"        // cue_ball_shade()'s default mode, not an approximation of it. The\n"
"        // albedo is ball_sample() baked into an atlas and looked up by the\n"
"        // BALL-LOCAL normal, so the numbers turn with the ball; the lighting\n"
"        // on top is the snooker rig: one nearly-overhead key, the cloth\n"
"        // bouncing colour back up onto the underside, and four lamps whose\n"
"        // sharp reflections are what make a polished ball look polished.\n"
"        // All of it in the same non-linear space shade565() worked in, or the\n"
"        // balance between the three comes out wrong.\n"
"        vec3 nb = normalize(v_local);\n"
"        // Longitude runs the other way. The handheld's sphere impostor builds\n"
"        // its normal as (u, -v, -nz) — a MIRROR of the true outward normal in\n"
"        // the view's forward axis, not a rotation of it — so ball_sample() has\n"
"        // always been fed a left-handed normal and its numbers are painted for\n"
"        // one. Baking from a true outward normal therefore comes out mirrored.\n"
"        // Flipping longitude here mirrors it back, and leaves latitude and the\n"
"        // lighting normal alone.\n"
"        float u = 0.5 - atan(nb.z, nb.x) / 6.2831853;\n"
"        float vv = acos(clamp(nb.y, -1.0, 1.0)) / 3.14159265;\n"
"        // A texture ARRAY, not one tall atlas. An atlas cannot be mipmapped:\n"
"        // level 1 blends the bottom of one ball into the top of the next, so\n"
"        // the choice would be a sharp ball that shimmers or a smooth ball with\n"
"        // its neighbour bleeding into its poles. A layer per ball has its own\n"
"        // mip chain and has neither problem.\n"
"        vec3 bc = texture(u_balls, vec3(u, clamp(vv, 0.001, 0.999), u_ballslice)).rgb;\n"
"        vec3 nw = normalize(v_nrm);\n"
"        float diff = max(dot(nw, L), 0.0);\n"
"        float down = max(-nw.y, 0.0);\n"
"        // Diffuse, then the cloth's bounce ADDED rather than mixed in.\n"
"        // The handheld lerps up to 82% toward the cloth tint on the\n"
"        // shadow side, which at 128x128 reads as 'in shadow' and at this\n"
"        // size reads as a red ball turning muddy green. Adding the bounce\n"
"        // instead lights the underside without draining the hue out of it,\n"
"        // and a snooker ball under a lamp is a *saturated* object.\n"
"        vec3 c = bc * (0.52 + 0.62 * diff) + u_clothsh * (down * 0.34 + 0.10);\n"
"        // The lamps, reflected properly.\n"
"        //\n"
"        // The handheld tests the half-vector against 0.975 and lights a\n"
"        // pixel or two. That IS the right answer at 128x128 — a single white\n"
"        // dot is all the room there is to say 'lamp'. Here a ball is two\n"
"        // hundred pixels across and the same test renders as nothing at all,\n"
"        // so this does what a polished ball actually does: mirror the shade.\n"
"        // Reflect the view vector about the surface, intersect the reflected\n"
"        // ray with each lamp rectangle hanging over the table, and light the\n"
"        // fragment where it hits one. The highlights are then the shape of\n"
"        // the lamps, they stretch and skew across the curve the way real\n"
"        // ones do, and they slide correctly as you walk around the table —\n"
"        // none of which a half-vector threshold can do.\n"
"        vec3 V = normalize(u_eye - v_world);\n"
"        vec3 Rv = reflect(-V, nw);\n"
"        float refl = 0.0;\n"
"        if (Rv.y > 1e-4) {\n"
"            for (int i = 0; i < u_nlamp; i++) {\n"
"                float t = (u_lampC[i].y - v_world.y) / Rv.y;\n"
"                if (t <= 0.0) continue;\n"
"                vec3 d = (v_world + Rv * t) - u_lampC[i];\n"
"                float a = dot(d, u_lampX[i]) / dot(u_lampX[i], u_lampX[i]);\n"
"                float b = dot(d, u_lampZ[i]) / dot(u_lampZ[i], u_lampZ[i]);\n"
"                // A shade has a hard edge and a hot centre. smoothstep over\n"
"                // the last few percent keeps it from aliasing to a crawling\n"
"                // staircase as the ball rolls.\n"
"                float e = max(abs(a), abs(b));\n"
"                refl += (1.0 - smoothstep(0.88, 1.0, e)) * (1.0 - 0.25 * e);\n"
"            }\n"
"        }\n"
"        // The shade is a bright source: let it blow out to white.\n"
"        c += vec3(1.0) * clamp(refl, 0.0, 1.4);\n"
"        o_col = emit(to_linear(c), 1.0, 1.0);\n"
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
"        else if (t < 0.0221) { c = mix(vec3(0.93,0.91,0.84), vec3(0.45,1.0,0.55),\n"
"                                        u_colour.a); gloss = 70.0; spec_k = 0.45; }\n" // ferrule: green when the line is live
"        else {\n"
"            // Ash grain runs the LENGTH of the cue as fine lines, so it has\n"
"            // to vary with the angle round the shaft and only drift slowly\n"
"            // along it. Varying it along the length instead — which is the\n"
"            // obvious reading of \"grain along the shaft\" — draws rings, and a\n"
"            // few hundred rings on a 13 mm shaft is a blur.\n"
"            float ang = a * 6.2831853;\n"
"            float lines = sin(ang * 23.0 + sin(t * 7.0) * 0.9);\n"
"            float fleck = sin(ang * 6.0 - t * 31.0);\n"
"            float g = clamp(0.5 + 0.30 * lines + 0.14 * fleck, 0.0, 1.0);\n"
"            c = mix(ash * 0.84, ash * 1.08, g);\n"
"            // Four-point hand splice. The ebony points rise OUT of the butt\n"
"            // and taper to a needle toward the tip — so they are widest at the\n"
"            // butt end, not the tip end, and at the base they very nearly meet\n"
"            // one another. Built the other way up and too narrow, they read as\n"
"            // four thin darts pointing the wrong way down the cue.\n"
"            float sp_tip = 0.560, sp_base = 0.815;\n"
"            if (t > sp_tip) {\n"
"                float k = clamp((t - sp_tip) / (sp_base - sp_tip), 0.0, 1.0);\n"
"                float halfw = 0.44 * pow(k, 0.80);\n"
"                float f = fract(a * 4.0);\n"
"                float d = min(f, 1.0 - f);\n"
"                c = mix(c, ebony, smoothstep(halfw, halfw * 0.86, d));\n"
"            }\n"
"            if (t > sp_base) c = ebony;\n"                        // solid butt above the splice
"            if (t > 0.9793 && t < 0.9931) c = vec3(0.62, 0.50, 0.22);\n"  // brass collar
"        }\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        float spec = pow(max(dot(v_nrm, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), gloss);\n"
"        o_col = emit(to_linear(c) * (0.34 + 0.70 * d) + vec3(spec) * spec_k, 1.0, 1.0);\n"
"    } else if (u_mode == 7) {\n"
"        // Timber. The frame carries its own base colour per piece and a grain\n"
"        // coordinate that runs ALONG whichever length of wood the vertex\n"
"        // belongs to, so the grain follows the apron round the table and runs\n"
"        // UP the legs, instead of everything being striped in world space.\n"
"        vec3 base = to_linear(v_col);\n"
"        float g = v_uv.x;\n"
"        // Two octaves: close-spaced lines, and a slow wander so the figure is\n"
"        // not a barcode. Modulated across the grain so lines drift rather than\n"
"        // running dead straight for a metre.\n"
"        float fine = sin(g * 210.0 + sin(v_uv.y * 26.0) * 1.3);\n"
"        float slow = sin(g * 31.0 - v_uv.y * 3.0);\n"
"        float fig  = 0.5 + 0.30 * fine + 0.20 * slow;\n"
"        vec3 c = base * (0.80 + 0.42 * fig);\n"
"        vec3 n = normalize(v_nrm);\n"
"        float d = max(dot(n, L), 0.0);\n"
"        // French polish: a broad sheen plus a tight highlight, so the apron\n"
"        // catches the lamps the way varnished wood does.\n"
"        float spec = pow(max(dot(n, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), 26.0);\n"
"        o_col = emit(c * (0.26 + 0.72 * d) + vec3(spec) * 0.16, 1.0, 1.0);\n"
"    } else if (u_mode == 4) {\n"
"        // The table, with the handheld's own shading: colours authored per\n"
"        // triangle, lit by the ABSOLUTE dot with the overhead key. Absolute\n"
"        // because the mesh is double-sided — the cloth fan and the pocket\n"
"        // voids are wound for shading, not for a front-face convention.\n"
"        float ndl = abs(dot(normalize(v_nrm), L));\n"
"        o_col = emit(to_linear(v_col * (0.32 + 0.68 * ndl)), 1.0, 1.0);\n"
"    } else if (u_mode == 6) {\n"
"        // A ball's shadow on the cloth: a soft decal, as scene_add_shadow\n"
"        // draws it. Without these the balls hover.\n"
"        float d = length(v_uv - vec2(0.5)) * 2.0;\n"
"        float a = 0.55 * (1.0 - smoothstep(0.10, 1.0, d));\n"
"        o_col = emit(to_linear(u_clothsh) * 0.55, a, 0.0);\n"
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
    GLint  u_ballslice, u_balls, u_clothsh;
    GLint  u_lampC, u_lampX, u_lampZ, u_nlamp, u_eye;
    Mesh   table, lips, frame, ball, cue, quad, floor, grip;
    int    frame_sel;
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
    /* Wind outward. The obvious ordering here (a, c, c+1, a+1) walks DOWN a
     * stack before going round, which puts the face normal on the INSIDE of the
     * sphere: with culling on, every near surface is discarded, so you see the
     * cloth and its markings straight through the ball and the lamp highlight
     * lands on the far inner wall — which reads as reflections stuck under the
     * ball rather than as a winding bug. Go round first, then down. */
    for (int j = 0; j < stacks; j++)
        for (int i = 0; i < slices; i++) {
            int a = j * (slices + 1) + i, c = a + slices + 1;
            b_quad(b, a, a + 1, c + 1, c);
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
    /* Note the order is the REVERSE of build_sphere's, and has to be: the
     * sphere's rings run from +Y down (y = cos(phi)) while the cue's run from
     * the tip up (y = t * CUE_LEN), so the identical index pattern produces
     * opposite handedness. Getting it wrong here made the cue hollow — you saw
     * its inside wall, lit by a normal pointing the wrong way. */
    for (int j = 0; j < rings; j++)
        for (int i = 0; i < slices; i++) {
            int a = j * (slices + 1) + i, c = a + slices + 1;
            b_quad(b, a, c, c + 1, a + 1);
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
/* 256x128 per ball, up from 64x32. ball_sample() is analytic — sharp thresholds
 * on the ball-local normal — so it has no native resolution of its own and the
 * old bake was simply discarding detail it could have had. At 128x128 on the
 * handheld a ball was a few dozen pixels across and 64x32 was ample; in VR you
 * can put your eye next to one. */
#define BTEX_W   256
#define BTEX_H   128
/* Derived, not chosen. Snooker ids run past the pool set — cue, 1..15 reds,
 * then CUE_ID_YELLOW(20) through CUE_ID_BLACK(25) — and picking a round number
 * here instead of reading the enum is how pink and black ended up sharing the
 * cue ball's slice. */
#define BTEX_IDS (CUE_ID_BLACK + 1)

/* Re-baked whenever the table changes, NOT once at start-up.
 *
 * ball_sample() answers differently depending on s_is_snooker and the selected
 * ball set, and cue_render_build_table() is what sets the former. Baking once
 * meant every snooker frame was played with pool balls: ids 1..15 came out as
 * solids and stripes rather than reds. The original never had this failure
 * mode because it evaluates the function per pixel; the cache is mine, so
 * keeping it in step is mine too. */
static void bake_ball_atlas(void) {
    if (G.ball_tex) { glDeleteTextures(1, &G.ball_tex); G.ball_tex = 0; }
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
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.ball_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB565, BTEX_W, BTEX_H, BTEX_IDS, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    /* Mipmaps matter as much as the resolution does. A 256x128 ball seen across
     * a 12 ft table covers a handful of pixels, and point-sampling a sharp
     * number or stripe at that rate crawls and sparkles as the ball rolls.
     * Trilinear picks the level that matches the footprint. */
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* Wrap in u so the seam behind the ball closes. v is clamped, but a layer
     * has no neighbour to bleed from now, so this is only about the poles. */
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    {   /* Anisotropy if the driver has it: a ball is a sphere, so its texture
         * footprint is stretched badly near the silhouette. */
        GLfloat aniso = 0.0f;
        glGetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &aniso);
        if (aniso > 1.0f) {
            if (aniso > 8.0f) aniso = 8.0f;
            glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */, aniso);
        }
        while (glGetError() != GL_NO_ERROR) { }   /* absent extension is fine */
    }
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
    G.u_balls      = glGetUniformLocation(G.prog, "u_balls");
    /* Array uniforms: ask for element 0 by name. Querying the bare array name
     * is legal but not honoured by every driver, and a silent -1 here sets no
     * lamps at all — which looks exactly like a polished ball with no lamps
     * above it. Every location is checked below for the same reason. */
    G.u_lampC      = glGetUniformLocation(G.prog, "u_lampC[0]");
    G.u_lampX      = glGetUniformLocation(G.prog, "u_lampX[0]");
    G.u_lampZ      = glGetUniformLocation(G.prog, "u_lampZ[0]");
    G.u_nlamp      = glGetUniformLocation(G.prog, "u_nlamp");
    G.u_eye        = glGetUniformLocation(G.prog, "u_eye");
    G.u_clothsh    = glGetUniformLocation(G.prog, "u_clothsh");
    if (G.u_lampC < 0 || G.u_eye < 0 || G.u_clothsh < 0 || G.u_light < 0)
        LOGI("[cuevr] WARNING: lighting uniforms missing (lampC %d eye %d clothsh %d light %d)",
             G.u_lampC, G.u_eye, G.u_clothsh, G.u_light);
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

    /* Controller proxy: a small block, tapered like a grip. */
    b_init(&b, 64, 96);
    {
        const float w = 0.021f, h = 0.030f, d = 0.052f;
        const float f[3] = {0,0,1}, bk[3] = {0,0,-1}, l[3] = {-1,0,0},
                    r[3] = {1,0,0}, u[3] = {0,1,0}, dn[3] = {0,-1,0};
        float A[3]={-w,-h,-d}, B[3]={w,-h,-d}, C[3]={w*0.75f,h,-d*0.55f}, D[3]={-w*0.75f,h,-d*0.55f};
        float E[3]={-w,-h, d}, F[3]={w,-h, d}, G[3]={w*0.75f,h, d*0.35f}, H[3]={-w*0.75f,h, d*0.35f};
        b_face(&b, A,B,C,D, bk); b_face(&b, E,F,G,H, f);
        b_face(&b, A,E,H,D, l);  b_face(&b, B,F,G,C, r);
        b_face(&b, D,C,G,H, u);  b_face(&b, A,B,F,E, dn);
    }
    mesh_upload(&G.grip, &b);
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
    bake_ball_atlas();

    /* The frame is generated separately and drawn separately — nothing above
     * this line knows it exists, and swapping designs cannot disturb the bed,
     * the cushions or the pockets. */
    mesh_free(&G.frame);
    {
        int cv, ci;
        cuevr_frame_capacity(&cv, &ci);
        CueVrFrameMesh fm;
        memset(&fm, 0, sizeof fm);
        fm.v = malloc(sizeof(CueVrFrameVtx) * (size_t)cv);
        fm.idx = malloc(sizeof(uint16_t) * (size_t)ci);
        fm.cap_v = cv; fm.cap_i = ci;
        if (fm.v && fm.idx) {
            cuevr_frame_build(G.frame_sel, &fm, t);
            if (fm.overflow) LOGI("[cuevr] frame '%s' ran out of room",
                                  CUEVR_FRAMES[G.frame_sel].name);
            /* CueVrFrameVtx and the renderer's Vtx are the same layout, so this
             * goes straight to the GPU. */
            Builder fb;
            fb.v = (Vtx *)fm.v; fb.i = fm.idx;
            fb.nv = fm.nv; fb.ni = fm.ni; fb.cap_v = cv; fb.cap_i = ci;
            mesh_upload(&G.frame, &fb);
            LOGI("[cuevr] frame '%s': %d tris", CUEVR_FRAMES[G.frame_sel].name, fm.ni / 3);
        }
        free(fm.v); free(fm.idx);
    }

    LOGI("[cuevr] table mesh %d tris (%d bed, %d lip), balls re-baked (%s)",
         n, bed, nlip, t->is_snooker ? "snooker" : "pool");
}

void cuevr_render_shutdown(void) {
    if (!G.ready) return;
    mesh_free(&G.grip);
    mesh_free(&G.table); mesh_free(&G.lips); mesh_free(&G.frame); mesh_free(&G.ball);
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

    /* The lighting rig: shades hanging over the table, as a real room has.
     *
     * The handheld carries four lamp DIRECTIONS and tests a half-vector,
     * because at 128x128 a reflection is one white pixel and a direction is all
     * you need to place it. Here the reflection is a shape, so the lamps have
     * to be objects: rectangles at a height above the cloth, which the shader
     * intersects with the reflected view ray. They hang over the table, so they
     * are built in table space and carried out into the room with it.
     */
    /* Sized and hung like the real thing. A snooker light is a long low bar of
     * big shades — and the size of the reflection is the size of the SOURCE:
     * a small lamp high up reflects in a 26 mm ball as a speck, which is what
     * the first attempt at this looked like. Low and wide gives the long bright
     * streaks you actually see down a table. */
    const float LAMP_H  = 0.62f;      /* above the cloth */
    const float LAMP_HX = 0.30f;      /* half the shade, along the table */
    const float LAMP_HZ = 0.17f;      /* and across it */
    float cy = cosf(s->place->yaw), sy = sinf(s->place->yaw);
    int nlamp = (int)(G.tab.half_len * 2.0f / 0.85f + 0.5f);
    if (nlamp < 2) nlamp = 2;
    if (nlamp > 4) nlamp = 4;
    {
        float cen[12], axx[12], axz[12];
        float L = G.tab.half_len * 2.0f;
        for (int i = 0; i < nlamp; i++) {
            float tx = ((float)i + 0.5f) / nlamp * L - L * 0.5f;
            cen[i*3+0] = s->place->pos.x + tx * cy;
            cen[i*3+1] = s->place->pos.y + LAMP_H;
            cen[i*3+2] = s->place->pos.z + tx * sy;
            axx[i*3+0] = LAMP_HX * cy;  axx[i*3+1] = 0.0f; axx[i*3+2] = LAMP_HX * sy;
            axz[i*3+0] = -LAMP_HZ * sy; axz[i*3+1] = 0.0f; axz[i*3+2] = LAMP_HZ * cy;
        }
        glUniform3fv(G.u_lampC, nlamp, cen);
        glUniform3fv(G.u_lampX, nlamp, axx);
        glUniform3fv(G.u_lampZ, nlamp, axz);
        glUniform1i(G.u_nlamp, nlamp);
    }

    /* The eye, recovered from the view matrix: its rows are the camera basis
     * and its translation is that basis applied to -eye, so undo it. */
    {
        float e0 = -(view[12]*view[0] + view[13]*view[1] + view[14]*view[2]);
        float e1 = -(view[12]*view[4] + view[13]*view[5] + view[14]*view[6]);
        float e2 = -(view[12]*view[8] + view[13]*view[9] + view[14]*view[10]);
        glUniform3f(G.u_eye, e0, e1, e2);
    }

    /* The key light stays the handheld's: nearly overhead, rotated with the
     * table so it is over the cloth and not over your kitchen. */
    {
        MoteVrV3 k = mv3_norm(mv3(0.10f * cy - 0.20f * sy, 0.975f,
                                  0.10f * sy + 0.20f * cy));
        glUniform3f(G.u_light, k.x, k.y, k.z);
    }

    {   /* cloth bounce: the cloth's own colour at 0.42, as shade565 gives it */
        float r = ((G.tab.cloth >> 11) & 31) / 31.0f;
        float g = ((G.tab.cloth >> 5) & 63) / 63.0f;
        float b = (G.tab.cloth & 31) / 31.0f;
        glUniform3f(G.u_clothsh, r * 0.42f, g * 0.42f, b * 0.42f);
    }

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

    /* The frame first: it is underneath everything and honestly wound, so it
     * keeps backface culling. */
    if (G.frame.n) {
        glUniform1i(G.u_mode, 7);
        set_model(T);
        draw(&G.frame);
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

    /* ---- ball shadows on the cloth ---- *
     * The handheld drops a soft decal under every ball. Without them the balls
     * hover a centimetre above the cloth and nothing on the table feels like it
     * is resting on anything. */
    {
        glEnable(GL_BLEND);
        /* Colour blends; ALPHA IS LEFT ALONE.
         *
         * A plain glBlendFunc applies to the alpha channel too, so a shadow at
         * half alpha drops the framebuffer's alpha from 1 to 0.75 — and that
         * alpha is exactly what the Quest compositor uses to blend this layer
         * over the passthrough camera feed. The shadows were not dark blobs on
         * the cloth, they were holes punched through it: you could see the real
         * room, and the real table you had matched the height to, straight
         * through the baize. Anything translucent drawn into a passthrough layer
         * has to preserve destination alpha. */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glUniform1i(G.u_mode, 6);
        /* Darken toward the cloth's own shadowed tone rather than toward black:
         * a ball on baize does not cast an inky disc. */
        glUniform4f(G.u_colour, 0.0f, 0.0f, 0.0f, 1.0f);
        float rad = G.tab.R * 1.55f;
        for (int i = 0; i < s->nballs; i++) {
            const CueBall *bl = &s->balls[i];
            if (!bl->on) continue;
            float local[16], model[16], rot[16];
            mm4_identity(local);
            local[0] = rad * 2.0f; local[5] = rad * 2.0f;
            mm4_identity(rot);
            rot[5] = 0.0f; rot[6] = -1.0f; rot[9] = 1.0f; rot[10] = 0.0f;  /* lie it flat */
            mm4_mul(local, rot, local);
            local[12] = bl->pos.x; local[13] = 0.0015f; local[14] = bl->pos.z;
            mm4_mul(model, T, local);
            set_model(model);
            draw(&G.quad);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
    }

    /* ---- balls ---- */
    glUniform1i(G.u_mode, 1);
    /* The ball array lives on unit 1 for the whole pass; u_tex keeps unit 0. */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.ball_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(G.u_balls, 1);
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
        /* The one piece of aim feedback there is: the ferrule goes green when
         * the cue line actually meets the ball. No aim line, no ghost ball —
         * but you should not have to guess whether you are even on it. */
        glUniform4f(G.u_colour, 1, 1, 1, s->cue_on_ball ? 1.0f : 0.0f);
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

    /* ---- your hands ---- */
    if (s->hands_valid) {
        glUniform1i(G.u_mode, 0);
        colour(0.10f, 0.105f, 0.12f, 1.0f);
        for (int i = 0; i < 2; i++) {
            float M[16];
            mm4_from_pose(M, s->hand[i], 1.0f);
            set_model(M);
            draw(&G.grip);
        }
        /* Where the cue is resting on the bridge: a small pale marker, so the
         * rest adjustment has something to show for itself. */
        if (s->rest_visible) {
            glUniform1i(G.u_mode, 0);
            colour(0.55f, 0.52f, 0.44f, 1.0f);
            float L[16], M2[16];
            mm4_identity(L);
            L[0] = 0.016f; L[5] = 0.016f;
            L[12] = s->rest_pos.x; L[13] = s->rest_pos.y; L[14] = s->rest_pos.z;
            mm4_mul(M2, L, L);
            set_model(L);
            draw(&G.quad);
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
