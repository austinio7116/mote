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

/* How many rows below GAME the START row sits. Must track the menu. */
#define MR_START_STEPS 7
MoteVrV3 cuevr_app_rest(void);
int cuevr_app_aiming(void);
MoteVrV3 cuevr_app_pocket_room(void);
float cuevr_app_grip(void);
#include "cuevr_app.h"
#include "cuevr_audio.h"
#include "cue_audio.h"

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
static float s_lsqueeze = 0.0f;
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
    t->hand[MOTE_VR_LEFT].squeeze = s_lsqueeze;
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
    /* MOTE_VR_FOCUS=pocket puts the eye on a corner pocket, which is the shot
     * worth comparing against a photograph. Resolved from the app each frame,
     * because the table has to be placed before its corner exists. */
    int focus_pocket = 0;
    { const char *v = getenv("MOTE_VR_FOCUS");
      if (v && !strcmp(v, "pocket")) focus_pocket = 1;
      else if (v && !strcmp(v, "butt")) focus_pocket = 2;
      else if (v) { float a, b, c; if (sscanf(v, "%f,%f,%f", &a, &b, &c) == 3)
          s_focus = mv3(a, b, c); } }
    { const char *v = getenv("MOTE_VR_VIEW");
      if (v) { float a, b, c; if (sscanf(v, "%f,%f,%f", &a, &b, &c) == 3) {
          s_yaw = a * 3.14159265f/180.0f; s_pitch = b * 3.14159265f/180.0f; s_dist = c; } } }
    const char *shot = getenv("MOTE_VR_SHOT");
    int shot_frame = 120;
    { const char *v = getenv("MOTE_VR_SHOT_FRAME"); if (v) shot_frame = atoi(v); }
    int auto_table = -1;
    { const char *v = getenv("CUEVR_TABLE"); if (v) auto_table = atoi(v); }
    int auto_stroke = getenv("CUEVR_STROKE") != NULL;
    /* CUEVR_ADJUST: hold the LEFT side trigger, move the bridge hand, let go —
     * through the app's real update path, not cuevr_cue.c in isolation. This is
     * the only way to settle "the offset never changes on the headset" from
     * here: an isolated unit test passing proves the maths, not the wiring. */
    int auto_adjust = getenv("CUEVR_ADJUST") != NULL;
    /* CUEVR_PAUSE=<frame>: tap MENU then, so the options screen can be
     * captured without a keyboard. */
    /* CUEVR_NET=host|join: drive the lobby to a LAN session, so two instances on
     * one machine can be paired from a script. Networking that has never had two
     * ends talk to each other is not networking, it is hope. */
    const char *auto_net = getenv("CUEVR_NET");
    int auto_pause = -1;
    { const char *v = getenv("CUEVR_PAUSE"); if (v) auto_pause = atoi(v); }

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

    { const char *d = getenv("CUEVR_PREFS_DIR"); cuevr_prefs_dir(d ? d : "."); }

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

        /* Scripted run-in, so a screenshot needs no keyboard.
         *
         * The order changed when levelling moved to the front, and this script
         * is part of that change rather than a victim of it: it now confirms the
         * level FIRST, then walks the menu. Left as fixed frame numbers on
         * purpose — the capture step is pinned to 1/72 s so frame N is always the
         * same moment, and a scripted run that drifts is a scripted run that
         * silently stops testing anything.
         *
         *   f4    confirm the levelling screen  -> the menu
         *   f10+  step DOWN to the GAME row is unnecessary (it starts there),
         *         so nudge RIGHT auto_table times to pick the table
         *   then  step down to START and press A
         */
        /* CUEVR_NET takes the menu over after the levelling: two scripts both
         * pressing A in the same menu is two scripts pressing A in the same
         * menu, and the table script got there first and started a CPU game. */
        if (auto_table >= 0 && !auto_net) {
            const int F_LEVEL = 4;                       /* confirm the level */
            const int F_PICK  = 10;                      /* start choosing the table */
            const int F_ROW   = F_PICK + auto_table * 2 + 4;
            const int F_GO    = F_ROW + MR_START_STEPS * 2 + 4;

            if (nframe == F_LEVEL)     s_a = 1;
            if (nframe == F_LEVEL + 2) s_a = 0;

            /* GAME row: nudge right once per table index. */
            if (nframe >= F_PICK && nframe < F_PICK + auto_table * 2)
                s_stick_r[0] = (nframe & 1) ? 1.0f : 0.0f;
            else if (nframe == F_PICK + auto_table * 2)
                s_stick_r[0] = 0.0f;

            /* Down to the START row. */
            if (nframe >= F_ROW && nframe < F_ROW + MR_START_STEPS * 2)
                s_stick_r[1] = (nframe & 1) ? -1.0f : 0.0f;
            else if (nframe == F_ROW + MR_START_STEPS * 2)
                s_stick_r[1] = 0.0f;

            if (nframe == F_GO)     s_a = 1;
            if (nframe == F_GO + 2) s_a = 0;
            /* and confirm the table placement the frame put us back into */
            if (nframe == F_GO + 10) s_a = 1;
            if (nframe == F_GO + 12) s_a = 0;
        }
        /* Hold the LEFT side trigger and move the bridge hand, then let go. At
         * TOP level, not inside the stroke script: the first version of this was
         * nested inside `if (auto_stroke)` and silently never ran, which is the
         * same class of mistake as the bug it is here to find. */
        if (focus_pocket == 1) s_focus = cuevr_app_pocket_room();
        else if (focus_pocket == 2) s_focus = s_butt;   /* the cue's butt hand */

        if (auto_net) {
            if (nframe == 4) s_a = 1;            /* confirm the levelling */
            if (nframe == 6) s_a = 0;
            /* From the menu: down to OPPONENT, right to ONLINE, down to START,
             * A, then LAN, then HOST or JOIN. */
            const int F = 12;
            if (nframe == F)      s_stick_r[1] = -1.0f;      /* -> OPPONENT */
            if (nframe == F + 1)  s_stick_r[1] = 0.0f;
            /* The default is VS CPU, so exactly ONE nudge right reaches ONLINE.
             * Two wrapped round to PRACTICE and started a local game, which the
             * state trace showed as MENU -> AIM instead of MENU -> LOBBY. */
            if (nframe == F + 3)  s_stick_r[0] = 1.0f;       /* VS CPU -> ONLINE */
            if (nframe == F + 4)  s_stick_r[0] = 0.0f;
            for (int k = 0; k < 5; k++) {                    /* -> START row */
                if (nframe == F + 10 + k * 2)     s_stick_r[1] = -1.0f;
                if (nframe == F + 11 + k * 2)     s_stick_r[1] = 0.0f;
            }
            if (nframe == F + 22) s_a = 1;                   /* enter the lobby */
            if (nframe == F + 24) s_a = 0;
            if (nframe == F + 28) s_a = 1;                   /* choose LAN */
            if (nframe == F + 30) s_a = 0;
            if (!strcmp(auto_net, "join")) {
                if (nframe == F + 33) s_stick_r[1] = -1.0f;  /* HOST -> JOIN */
                if (nframe == F + 34) s_stick_r[1] = 0.0f;
            }
            if (nframe == F + 38) s_a = 1;                   /* host or join */
            if (nframe == F + 40) s_a = 0;
        }
        if (auto_pause >= 0) {
            if (nframe == auto_pause)     s_menu = 1;
            if (nframe == auto_pause + 2) s_menu = 0;
        }
        if (auto_adjust) {
            if (nframe >= 90 && nframe < 120) {
                s_lsqueeze = 1.0f;
                s_bridge.y += 0.001f;          /* raise the hand 3 cm in total */
            } else if (nframe == 90) {
                s_lsqueeze = 0.0f;
                MoteVrV3 r = cuevr_app_rest();
                printf("[adjust] 30 held frames: rest = (%.4f %.4f %.4f) len %.4f\n",
                       (double)r.x, (double)r.y, (double)r.z, (double)mv3_len(r));
            } else if (nframe == 130) {
                MoteVrV3 r = cuevr_app_rest();
                printf("[adjust] 40 frames after release: rest = (%.4f %.4f %.4f) len %.4f\n",
                       (double)r.x, (double)r.y, (double)r.z, (double)mv3_len(r));
            }
        }

        /* A scripted cue action: take hold of the cue, then drive the grip hand
         * through. The bridge does not move — that is the point of it. */
        /* Frames since play actually began. Absolute frame numbers cannot work
         * here: the online run-in waits on a network handshake, which takes
         * however long it takes, and by the time the table was live the scripted
         * stroke frame was thousands of frames in the past. */
        static int play_f = -1;
        if (play_f < 0) { if (cuevr_app_aiming()) play_f = 0; }
        else play_f++;

        if (auto_stroke) {
            /* AFTER the run-in, not at a frame number that happened to be past
             * it once. The run-in grew when levelling moved to the front, and a
             * stroke scripted at frame 36 then fired inside the menu and quietly
             * did nothing — the same silent-harness failure as before. */
            /* The online run-in is longer again (lobby, transport, action), so
             * the stroke waits longer still. */
            if (play_f >= 6)  s_trig = 1;
            if (play_f == 10) s_butt.x += 0.12f;
        }

        /* Park the fake hands behind the cue ball the first time the table is
         * placed, so the cue starts pointing at something. After that WASD and
         * the arrows move them, as your real hands would. */
        /* Place the fake hands only once the table is actually sited and we are
         * aiming — at frame 24 the cue ball is still wherever the default
         * placement put it, so the hands were lined up on a table that then
         * moved. Another consequence of levelling moving to the front. */
        if (!hands_placed && cuevr_app_aiming()) {
            MoteVrV3 b = cuevr_app_cue_ball_room();
            /* The tip is (CUE_LEN - grip) in front of the GRIP hand now, so
             * place that hand from the ball backwards and put the bridge a
             * stance in front of it. */
            /* The cue now rests ABOVE the bridge hand by rest_lift, so the fake
             * hand goes that much lower to put the cue on the ball's line. A real
             * hand does this without being told. */
            float reach = CUEVR_CUE_LEN - cuevr_app_grip();
            MoteVrV3 rst = cuevr_app_rest();          /* whatever is loaded */
            s_butt   = mv3(b.x - reach - 0.10f, b.y, b.z);
            s_bridge = mv3_sub(mv3(s_butt.x + 0.90f, b.y, b.z), rst);
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

        /* The preview has no audio device, but the mixer is the real one — so
         * pull from it and report the peak. A shot that made no noise is a shot
         * whose events never reached the mixer, and that is worth failing on. */
        if (getenv("CUEVR_AUDIO")) {
            static int16_t buf[512];
            static long peak_at = -1;
            static int peak = 0;
            cue_audio_render(buf, 512);
            for (int i = 0; i < 512; i++) {
                int v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > peak) { peak = v; peak_at = nframe; }
            }
            if (nframe == shot_frame)
                printf("cuevr: audio peak %d at frame %ld — strike %d clack %d cushion %d pot %d\n",
                       peak, peak_at,
                       cuevr_audio_count(CUE_SFX_STRIKE), cuevr_audio_count(CUE_SFX_CLACK),
                       cuevr_audio_count(CUE_SFX_CUSHION), cuevr_audio_count(CUE_SFX_POT));
        }
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
