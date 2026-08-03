/*
 * CueVR — the headset game, without the headset.
 *
 * The whole app except OpenXR: the same table, the same physics, the same cue
 * geometry, the same shaders, the same HUD. What it fakes is the tracking,
 * which it drives from mouse and keyboard — and it stubs the two things the
 * OpenXR host would otherwise answer (is the target sRGB, and buzz the
 * controllers), because those are the only places the game asks.
 *
 * It exists for the same reason the console's preview does: a Quest build
 * cannot be looked at from the machine that writes it, and a cue game that is
 * subtly wrong is a cue game you discover is wrong one frustrating frame at a
 * time with a headset on.
 *
 *   MOTE_VR_SHOT=/tmp/x.png   render N frames, write a PNG, exit
 *   MOTE_VR_SHOT_FRAME=n      which frame to grab
 *   MOTE_VR_VIEW=yaw,pitch,d  where to put the fake head
 *   CUEVR_TABLE=0..6          skip the menu and start on this table
 *   CUEVR_STROKE=1            play a scripted centre-ball stroke at frame 40
 *
 * Live: drag to orbit, wheel to dolly. WASD moves the bridge hand, arrows the
 * butt, SPACE strokes through the ball. Enter = A (menu/confirm), M = the left
 * menu button. IJKL/UO drive the sticks for setup.
 */
#include "cuevr.h"
#include "cuevr_app.h"

#include <SDL.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* ---- the two things the OpenXR host would answer ------------------------ */
static int s_target_srgb;
int  mote_xr_target_is_srgb(void) { return s_target_srgb; }
int  mote_xr_floor_relative(void) { return 1; }   /* the preview's y=0 is the floor */
void mote_xr_haptic(float i, int ms) { (void)i; (void)ms; }

/* ---- the fake head and hands -------------------------------------------- */
static float s_yaw = 0.35f, s_pitch = 0.55f, s_dist = 1.6f;  /* eye above the cloth, as a player stands */
static MoteVrV3 s_focus = { 0.0f, 0.85f, 0.0f };
static MoteVrV3 s_bridge = { -0.20f, 0.90f, 0.0f };
static MoteVrV3 s_butt   = { -0.78f, 0.98f, 0.0f };
static float s_stick_l[2], s_stick_r[2];
static int   s_a, s_menu, s_trig;

static void fake_tracking(MoteVrTracking *t, float dt) {
    memset(t, 0, sizeof *t);
    t->dt = dt;
    MoteVrV3 dir = mv3(cosf(s_pitch) * sinf(s_yaw), sinf(s_pitch),
                       cosf(s_pitch) * cosf(s_yaw));
    t->head.p = mv3_add(s_focus, mv3_scale(dir, s_dist));
    MoteVrV3 fwd = mv3_norm(mv3_sub(s_focus, t->head.p));
    MoteVrV3 back = mv3_scale(fwd, -1.0f);
    MoteVrV3 right = mv3_norm(mv3_cross(mv3(0, 1, 0), back));
    t->head.q = mq_from_axes(right, mv3_cross(back, right), back);

    t->hand[MOTE_VR_LEFT].tracked = t->hand[MOTE_VR_RIGHT].tracked = 1;
    t->hand[MOTE_VR_LEFT].pose.p  = s_bridge;
    t->hand[MOTE_VR_RIGHT].pose.p = s_butt;
    t->hand[MOTE_VR_LEFT].pose.q  = mq_ident();
    t->hand[MOTE_VR_RIGHT].pose.q = mq_ident();
    t->hand[MOTE_VR_LEFT].stick_x  = s_stick_l[0];
    t->hand[MOTE_VR_LEFT].stick_y  = s_stick_l[1];
    t->hand[MOTE_VR_RIGHT].stick_x = s_stick_r[0];
    t->hand[MOTE_VR_RIGHT].stick_y = s_stick_r[1];
    t->hand[MOTE_VR_RIGHT].btn_lower = s_a;
    t->hand[MOTE_VR_RIGHT].trigger   = s_trig ? 1.0f : 0.0f;
    t->hand[MOTE_VR_LEFT].menu = s_menu;
}

static void write_png(const char *path, int w, int h) {
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h / 2; y++) {
        unsigned char *a = px + (size_t)y * w * 4, *b = px + (size_t)(h - 1 - y) * w * 4;
        for (int i = 0; i < w * 4; i++) { unsigned char t = a[i]; a[i] = b[i]; b[i] = t; }
    }
    int len = 0;
    unsigned char *png = stbi_write_png_to_mem(px, w * 4, w, h, 4, &len);
    if (png) {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(png, 1, (size_t)len, f); fclose(f); }
        free(png);
        printf("cuevr: wrote %s (%dx%d)\n", path, w, h);
    }
    free(px);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int w = 1200, h = 800;
    { const char *v = getenv("MOTE_VR_SIZE");
      if (v) { int a, b; if (sscanf(v, "%dx%d", &a, &b) == 2 && a > 64 && b > 64) { w = a; h = b; } } }
    { const char *v = getenv("MOTE_VR_VIEW");
      if (v) { float a, b, c; if (sscanf(v, "%f,%f,%f", &a, &b, &c) == 3) {
          s_yaw = a * 3.14159265f/180.0f; s_pitch = b * 3.14159265f/180.0f; s_dist = c; } } }
    const char *shot = getenv("MOTE_VR_SHOT");
    int shot_frame = 120;
    { const char *v = getenv("MOTE_VR_SHOT_FRAME"); if (v) shot_frame = atoi(v); }
    int auto_table = -1;
    { const char *v = getenv("CUEVR_TABLE"); if (v) auto_table = atoi(v); }
    int auto_stroke = getenv("CUEVR_STROKE") != NULL;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_Window *win = SDL_CreateWindow("CueVR (preview)", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(shot ? 0 : 1);

    MoteXrApp app;
    cuevr_app_describe(&app);
    if (app.gl_init(app.user) != 0) { fprintf(stderr, "cuevr: init failed\n"); return 1; }

    int running = 1, dragging = 0, hands_placed = 0;
    uint64_t prev = SDL_GetPerformanceCounter();
    long nframe = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_MOUSEBUTTONDOWN: dragging = 1; break;
            case SDL_MOUSEBUTTONUP:   dragging = 0; break;
            case SDL_MOUSEMOTION:
                if (dragging) {
                    s_yaw   -= ev.motion.xrel * 0.006f;
                    s_pitch += ev.motion.yrel * 0.006f;
                    if (s_pitch >  1.45f) s_pitch =  1.45f;
                    if (s_pitch < -1.45f) s_pitch = -1.45f;
                }
                break;
            case SDL_MOUSEWHEEL:
                s_dist -= ev.wheel.y * 0.08f;
                if (s_dist < 0.25f) s_dist = 0.25f;
                if (s_dist > 8.0f)  s_dist = 8.0f;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { w = ev.window.data1; h = ev.window.data2; }
                break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                int d = (ev.type == SDL_KEYDOWN);
                float step = 0.02f;
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_w: if (d) s_bridge.x += step; break;
                case SDLK_s: if (d) s_bridge.x -= step; break;
                case SDLK_a: if (d) s_bridge.z -= step; break;
                case SDLK_d: if (d) s_bridge.z += step; break;
                case SDLK_q: if (d) s_bridge.y += step * 0.5f; break;
                case SDLK_e: if (d) s_bridge.y -= step * 0.5f; break;
                case SDLK_UP:    if (d) s_butt.x += step; break;
                case SDLK_DOWN:  if (d) s_butt.x -= step; break;
                case SDLK_LEFT:  if (d) s_butt.z -= step; break;
                case SDLK_RIGHT: if (d) s_butt.z += step; break;
                case SDLK_PAGEUP:   if (d) s_butt.y += step * 0.5f; break;
                case SDLK_PAGEDOWN: if (d) s_butt.y -= step * 0.5f; break;
                case SDLK_i: s_stick_r[1] =  d ? 1.0f : 0.0f; break;
                case SDLK_k: s_stick_r[1] =  d ? -1.0f : 0.0f; break;
                case SDLK_j: s_stick_r[0] =  d ? -1.0f : 0.0f; break;
                case SDLK_l: s_stick_r[0] =  d ? 1.0f : 0.0f; break;
                case SDLK_u: s_stick_l[1] =  d ? 1.0f : 0.0f; break;
                case SDLK_o: s_stick_l[1] =  d ? -1.0f : 0.0f; break;
                case SDLK_RETURN: s_a = d; break;
                case SDLK_m: s_menu = d; break;
                case SDLK_SPACE:  if (d) s_bridge.x += 0.10f; break;   /* stroke */
                default: break;
                }
                break;
            }
            default: break;
            }
        }

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - prev) / (double)SDL_GetPerformanceFrequency());
        prev = now;
        if (dt > 0.1f) dt = 0.1f;
        /* A capture runs as fast as the GPU will go, which means real frame
         * times of a millisecond and a shot that has barely started by the
         * frame you asked for. Pin the step to a headset's when capturing:
         * frame N is then always N/72 seconds in, and the same command always
         * produces the same picture. */
        if (shot) dt = 1.0f / 72.0f;

        /* Scripted run-in, so a screenshot needs no keyboard. */
        if (auto_table >= 0) {
            if (nframe == 3)  { s_stick_r[1] = 0.0f; }
            if (nframe >= 4 && nframe < 4 + auto_table * 2) {
                s_stick_r[1] = (nframe & 1) ? -1.0f : 0.0f;   /* step down the menu */
            } else if (nframe == 4 + auto_table * 2) {
                s_stick_r[1] = 0.0f;
            }
            if (nframe == 10 + auto_table * 2) s_a = 1;
            if (nframe == 12 + auto_table * 2) s_a = 0;
            if (nframe == 18 + auto_table * 2) s_a = 1;       /* confirm setup */
            if (nframe == 20 + auto_table * 2) s_a = 0;
        }
        if (auto_stroke && nframe == 40) s_bridge.x += 0.12f;

        /* Park the fake hands behind the cue ball the first time the table is
         * placed, so the cue starts pointing at something. After that WASD and
         * the arrows move them, as your real hands would. */
        if (!hands_placed && nframe > 24) {
            MoteVrV3 b = cuevr_app_cue_ball_room();
            /* The tip sits bridge_len (0.28 m) ahead of the bridge hand, so
             * the bridge has to be further back than that or the cue starts
             * already through the ball and no stroke can ever begin. */
            s_bridge = mv3(b.x - 0.40f, b.y + 0.030f, b.z);
            s_butt   = mv3(b.x - 1.02f, b.y + 0.075f, b.z);
            s_focus  = b;
            hands_placed = 1;
        }

        MoteVrTracking t;
        fake_tracking(&t, dt);
        app.update(app.user, &t);

        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.055f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float view[16], proj[16];
        mm4_view_from_pose(view, t.head);
        mm4_perspective(proj, 62.0f * 3.14159265f/180.0f, (float)w / (float)h, 0.02f, 60.0f);
        app.draw_eye(app.user, view, proj, 1);

        if (shot && nframe == shot_frame) { glFinish(); write_png(shot, w, h); running = 0; }
        SDL_GL_SwapWindow(win);
        nframe++;
    }

    app.gl_shutdown(app.user);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
