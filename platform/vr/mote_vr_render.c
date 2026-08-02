/*
 * Mote VR — drawing the console.
 *
 * GLES 3.0, which is what a Quest runs and what a desktop GL 4.3 core context
 * accepts unchanged, so the preview and the headset execute the same shaders.
 * There is not much to draw — a chassis, a screen, a few glows — and all of it
 * goes through one program with a mode uniform rather than four programs that
 * would each need their own state.
 *
 * The screen is the point of the whole exercise, so it gets the care: the LCD
 * is uploaded as GL_RGB565 (the engine's own framebuffer format, byte for byte,
 * no conversion pass) and sampled GL_NEAREST, because a 128x128 display blown
 * up to the size of your hands should look like pixels, not like a photograph
 * of pixels.
 */
#include "mote_vr.h"
#include "mote_vr_chassis.h"
#include "mote_config.h"
#include "mote_platform.h"

#include <GLES3/gl3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

static void logf_(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    mote_plat_log(b);
}

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
"out vec3 v_world;\n"
"void main() {\n"
"    v_uv = a_uv;\n"
"    v_nrm = mat3(u_model) * a_nrm;\n"
"    v_world = (u_model * vec4(a_pos, 1.0)).xyz;\n"
"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
"}\n";

/* Colour lives in LINEAR here, start to finish.
 *
 * Both textures are decoded in the shader rather than by an sRGB texture format.
 * GL_SRGB8 is the tidier answer and it is one line — but the LCD is RGB565,
 * which has no sRGB format at all, so the shader needs the decode anyway; and a
 * silently-rejected internal format leaves a black texture, which is exactly
 * what it did here. One path, no format negotiation, same on every driver.
 * Shading happens on linear values. What happens at the end depends on the
 * target:
 * an sRGB swapchain (what a Quest gives us) re-encodes on write, so we hand it
 * linear; a plain window (the desktop preview) does not, so we encode here.
 *
 * Getting this wrong in the obvious direction — sample sRGB as if it were
 * linear, then let an sRGB swapchain encode it a second time — lifts the blacks
 * and squeezes everything into the top of the range. It reads as washed out,
 * and because the top of the range is where the levels are coarsest, it also
 * reads as though the colour depth had dropped. It had not.
 *
 * u_mode: 0 lit+textured (the shell), 1 the LCD, 2 button glow, 3 floor grid
 */
static const char *FS =
"#version 300 es\n"
"precision highp float;\n"
"in vec3 v_nrm;\n"
"in vec2 v_uv;\n"
"in vec3 v_world;\n"
"uniform sampler2D u_tex;\n"
"uniform int   u_mode;\n"
"uniform int   u_encode;\n"     /* 1 = target does not do sRGB for us */
"uniform vec4  u_colour;\n"
"uniform vec3  u_light;\n"
"out vec4 o_col;\n"
"vec3 to_linear(vec3 c) { return pow(c, vec3(2.2)); }\n"
"// A moulded purple shell is one big smooth gradient, and a smooth gradient is\n"
"// where an 8-bit buffer shows its seams — as bands wide enough to look like\n"
"// the colour depth dropped. Half a level of hash noise, applied in the space\n"
"// the buffer is actually quantised in, turns each seam into noise the eye\n"
"// integrates away. Costs nothing and is invisible except by its absence.\n"
"float hash12(vec2 p) {\n"
"    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
"}\n"
"vec4 emit(vec3 c, float a) {\n"
"    vec3 o = u_encode == 1 ? pow(max(c, 0.0), vec3(1.0/2.2)) : c;\n"
"    return vec4(o + (hash12(gl_FragCoord.xy) - 0.5) / 255.0, a);\n"
"}\n"
"void main() {\n"
"    if (u_mode == 1) {\n"
"        // The LCD. RGB565 holds what the panel would show, so decode it as\n"
"        // sRGB and let the pipeline put it back exactly — no shifted hue, no\n"
"        // crushed darks. The small lift is applied in linear so it reads as a\n"
"        // lit display rather than as a sticker, without greying the blacks.\n"
"        vec3 c = to_linear(texture(u_tex, v_uv).rgb);\n"
"        o_col = emit(c * 1.10 + 0.004, 1.0);\n"
"    } else if (u_mode == 2) {\n"
"        // Button feedback. Soft-edged, and elliptical in UV space so the same\n"
"        // quad reads as a disc on A/B and as a capsule on a d-pad arm — a hard\n"
"        // rectangle looks like a sticker stuck on the shell.\n"
"        float d = length(v_uv - vec2(0.5)) * 2.0;\n"
"        o_col = emit(to_linear(u_colour.rgb), u_colour.a * smoothstep(1.0, 0.25, d));\n"
"    } else if (u_mode == 3) {\n"
"        // A grid that fades out with distance, for runtimes with no\n"
"        // passthrough. Never drawn on a Quest.\n"
"        vec2 g = abs(fract(v_world.xz * 2.0) - 0.5) / fwidth(v_world.xz * 2.0);\n"
"        float line = 1.0 - min(min(g.x, g.y), 1.0);\n"
"        float fade = clamp(1.0 - length(v_world.xz) / 6.0, 0.0, 1.0);\n"
"        o_col = emit(to_linear(u_colour.rgb) * line * fade, u_colour.a * line * fade);\n"
"    } else {\n"
"        // The shell. The photograph already carries the product shot's own\n"
"        // lighting, so this adds only enough directionality for the rounded\n"
"        // edge to read as round. The rim term multiplies rather than adds:\n"
"        // adding a constant to every fragment lifts the black glass and the\n"
"        // black buttons along with the shell, which is most of the front.\n"
"        vec3 n = normalize(v_nrm);\n"
"        float d = max(dot(n, normalize(u_light)), 0.0);\n"
"        vec3 c = to_linear(texture(u_tex, v_uv).rgb);\n"
"        float rim = 1.0 + pow(1.0 - abs(n.z), 3.0) * 0.35;\n"
"        o_col = emit(c * min(0.72 + 0.36 * d, 1.15) * rim, 1.0);\n"
"    }\n"
"}\n";

/* ---- state -------------------------------------------------------------- */

static GLuint s_prog, s_vao_mesh, s_vbo_mesh, s_ibo_mesh;
static GLuint s_vao_quad, s_vbo_quad;
static GLuint s_tex_shell, s_tex_lcd;
static GLint  s_u_mvp, s_u_model, s_u_tex, s_u_mode, s_u_colour, s_u_light, s_u_encode;
static int    s_encode = 1;   /* assume a plain window until told otherwise */
static int    s_mesh_indices;
static uint32_t s_lcd_seq = (uint32_t)-1;
static int    s_ready;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        logf_("[mote-vr] shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* A unit quad in XY, drawn with the same vertex layout as the mesh so one
 * program and one attribute setup serve both. */
static const float QUAD[] = {
    /* pos            nrm           uv   */
    -0.5f,-0.5f, 0.f,  0.f,0.f,1.f, 0.f,1.f,
     0.5f,-0.5f, 0.f,  0.f,0.f,1.f, 1.f,1.f,
     0.5f, 0.5f, 0.f,  0.f,0.f,1.f, 1.f,0.f,
    -0.5f,-0.5f, 0.f,  0.f,0.f,1.f, 0.f,1.f,
     0.5f, 0.5f, 0.f,  0.f,0.f,1.f, 1.f,0.f,
    -0.5f, 0.5f, 0.f,  0.f,0.f,1.f, 0.f,0.f,
};

static void attribs(void) {
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
}

int mote_vr_render_init(const MoteVrAssets *a) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return -1;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(s_prog, sizeof log, NULL, log);
        logf_("[mote-vr] link: %s", log);
        return -1;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    s_u_mvp    = glGetUniformLocation(s_prog, "u_mvp");
    s_u_model  = glGetUniformLocation(s_prog, "u_model");
    s_u_tex    = glGetUniformLocation(s_prog, "u_tex");
    s_u_mode   = glGetUniformLocation(s_prog, "u_mode");
    s_u_colour = glGetUniformLocation(s_prog, "u_colour");
    s_u_light  = glGetUniformLocation(s_prog, "u_light");
    s_u_encode = glGetUniformLocation(s_prog, "u_encode");

    /* ---- the chassis mesh ---- */
    const unsigned char *m = a->chassis_mesh;
    if (!m || a->chassis_mesh_len < 12 || memcmp(m, "MVM1", 4) != 0) {
        logf_("[mote-vr] chassis.mvm missing or not a mesh");
        return -1;
    }
    uint32_t nv, ni;
    memcpy(&nv, m + 4, 4);
    memcpy(&ni, m + 8, 4);
    size_t need = 12 + (size_t)nv * 32 + (size_t)ni * 2;
    if ((size_t)a->chassis_mesh_len < need) {
        logf_("[mote-vr] chassis.mvm truncated (%d < %zu)", a->chassis_mesh_len, need);
        return -1;
    }
    s_mesh_indices = (int)ni;

    glGenVertexArrays(1, &s_vao_mesh);
    glBindVertexArray(s_vao_mesh);
    glGenBuffers(1, &s_vbo_mesh);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo_mesh);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * 32, m + 12, GL_STATIC_DRAW);
    attribs();
    glGenBuffers(1, &s_ibo_mesh);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo_mesh);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ni * 2, m + 12 + (size_t)nv * 32,
                 GL_STATIC_DRAW);

    glGenVertexArrays(1, &s_vao_quad);
    glBindVertexArray(s_vao_quad);
    glGenBuffers(1, &s_vbo_quad);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof QUAD, QUAD, GL_STATIC_DRAW);
    attribs();
    glBindVertexArray(0);

    /* ---- the shell texture ---- */
    int tw = 0, th = 0, tc = 0;
    unsigned char *px = stbi_load_from_memory((const stbi_uc *)a->chassis_tex,
                                              a->chassis_tex_len, &tw, &th, &tc, 3);
    if (!px) { logf_("[mote-vr] chassis.jpg: %s", stbi_failure_reason()); return -1; }
    glGenTextures(1, &s_tex_shell);
    glBindTexture(GL_TEXTURE_2D, s_tex_shell);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    { GLenum e = glGetError();
      if (e) logf_("[mote-vr] shell texture upload: GL error 0x%04x", (unsigned)e); }
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(px);

    /* ---- the LCD ----
     * GL_RGB565 is exactly MOTE_RGB565's bit layout in native byte order, so
     * the engine's framebuffer goes to the GPU with no conversion at all. */
    glGenTextures(1, &s_tex_lcd);
    glBindTexture(GL_TEXTURE_2D, s_tex_lcd);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, MOTE_FB_W, MOTE_FB_H, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    logf_("[mote-vr] chassis %u verts / %u tris, shell tex %dx%d", nv, ni/3, tw, th);
    s_ready = 1;
    return 0;
}

void mote_vr_render_shutdown(void) {
    if (!s_ready) return;
    glDeleteProgram(s_prog);
    glDeleteBuffers(1, &s_vbo_mesh);
    glDeleteBuffers(1, &s_ibo_mesh);
    glDeleteBuffers(1, &s_vbo_quad);
    glDeleteVertexArrays(1, &s_vao_mesh);
    glDeleteVertexArrays(1, &s_vao_quad);
    glDeleteTextures(1, &s_tex_shell);
    glDeleteTextures(1, &s_tex_lcd);
    s_ready = 0;
}

void mote_vr_render_set_target_srgb(int target_encodes_srgb) {
    s_encode = target_encodes_srgb ? 0 : 1;
}

void mote_vr_render_set_frame(const uint16_t *fb, uint32_t seq) {
    if (!s_ready || !fb || seq == s_lcd_seq) return;
    s_lcd_seq = seq;
    glBindTexture(GL_TEXTURE_2D, s_tex_lcd);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MOTE_FB_W, MOTE_FB_H,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, fb);
}

/* ---- drawing ------------------------------------------------------------ */

static void set_mvp(const float *vp, const float *model) {
    float mvp[16];
    mm4_mul(mvp, vp, model);
    glUniformMatrix4fv(s_u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_u_model, 1, GL_FALSE, model);
}

/* A quad on the console's face, given a rect in model space and a z lift. */
static void face_quad(const float *vp, const float *console, float cx, float cy,
                      float w, float h, float lift)
{
    float local[16], model[16];
    mm4_identity(local);
    local[0] = w; local[5] = h;
    local[12] = cx; local[13] = cy; local[14] = MOTE_VR_CHASSIS_HALF + lift;
    mm4_mul(model, console, local);
    set_mvp(vp, model);
    glBindVertexArray(s_vao_quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void mote_vr_render_eye(const float *view, const float *proj,
                        const MoteVrConsole *c, const MoteButtons *lit,
                        const MoteVrTracking *t, int backdrop)
{
    if (!s_ready) return;
    (void)t;

    float vp[16];
    mm4_mul(vp, proj, view);

    glUseProgram(s_prog);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_u_tex, 0);
    /* Over the player's left shoulder: the direction a room light usually is,
     * and it keeps the rounded edge nearest the eye the bright one. */
    glUniform3f(s_u_light, -0.35f, 0.62f, 0.70f);
    glUniform1i(s_u_encode, s_encode);

    if (backdrop) {
        /* Lay the quad down as a floor 1.1 m below the eye line: scale, then
         * tip local +Z (its normal) up to world +Y, then drop it. */
        float model[16], rot[16];
        mm4_identity(model);
        model[0] = 14.0f; model[5] = 14.0f;
        mm4_identity(rot);
        rot[5] = 0.0f; rot[6] = -1.0f; rot[9] = 1.0f; rot[10] = 0.0f;
        mm4_mul(model, rot, model);
        model[13] = -1.1f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glUniform1i(s_u_mode, 3);
        glUniform4f(s_u_colour, 0.35f, 0.42f, 0.55f, 1.0f);
        set_mvp(vp, model);
        glBindVertexArray(s_vao_quad);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
    }

    if (!c->placed) return;

    float console[16];
    mm4_from_pose(console, c->pose, c->scale);

    /* the shell */
    glUniform1i(s_u_mode, 0);
    glBindTexture(GL_TEXTURE_2D, s_tex_shell);
    set_mvp(vp, console);
    glBindVertexArray(s_vao_mesh);
    glDrawElements(GL_TRIANGLES, s_mesh_indices, GL_UNSIGNED_SHORT, 0);

    /* the LCD, sitting a hair proud of the glass so it never z-fights */
    glUniform1i(s_u_mode, 1);
    glBindTexture(GL_TEXTURE_2D, s_tex_lcd);
    face_quad(vp, console,
              (MOTE_VR_SCREEN_L + MOTE_VR_SCREEN_R) * 0.5f,
              (MOTE_VR_SCREEN_B + MOTE_VR_SCREEN_T) * 0.5f,
              MOTE_VR_SCREEN_R - MOTE_VR_SCREEN_L,
              MOTE_VR_SCREEN_T - MOTE_VR_SCREEN_B,
              0.00012f);

    /* Which button a press landed on. There is no travel in a controller
     * trigger to tell you the d-pad took it, so the console says so itself. */
    if (lit) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        glUniform1i(s_u_mode, 2);
        glUniform4f(s_u_colour, 0.55f, 0.80f, 1.0f, 0.55f);
        if (lit->a)
            face_quad(vp, console, MOTE_VR_BTN_A_X, MOTE_VR_BTN_A_Y,
                      MOTE_VR_BTN_A_R * 2.0f, MOTE_VR_BTN_A_R * 2.0f, 0.00020f);
        if (lit->b)
            face_quad(vp, console, MOTE_VR_BTN_B_X, MOTE_VR_BTN_B_Y,
                      MOTE_VR_BTN_B_R * 2.0f, MOTE_VR_BTN_B_R * 2.0f, 0.00020f);
        if (lit->menu)
            face_quad(vp, console, MOTE_VR_BTN_MENU_X, MOTE_VR_BTN_MENU_Y,
                      MOTE_VR_BTN_MENU_R * 2.0f, MOTE_VR_BTN_MENU_R * 2.0f, 0.00020f);
        {   /* the d-pad arm that is down */
            float r = MOTE_VR_BTN_DPAD_R, arm = r * 0.62f, wid = r * 0.42f;
            float x = MOTE_VR_BTN_DPAD_X, y = MOTE_VR_BTN_DPAD_Y;
            if (lit->up)    face_quad(vp, console, x, y + arm*0.5f, wid, arm, 0.00020f);
            if (lit->down)  face_quad(vp, console, x, y - arm*0.5f, wid, arm, 0.00020f);
            if (lit->left)  face_quad(vp, console, x - arm*0.5f, y, arm, wid, 0.00020f);
            if (lit->right) face_quad(vp, console, x + arm*0.5f, y, arm, wid, 0.00020f);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    glBindVertexArray(0);
}
