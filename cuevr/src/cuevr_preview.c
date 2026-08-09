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
/* Rows from GAME down to START in the main menu. The appearance options moved
 * to their own screen, so this went from 10 to 6 — and a stale number here does
 * not fail, it just never starts the frame and every capture silently shows the
 * menu instead. */
#define MR_START_STEPS 6
/* ALIGN CONTROLS is PS_ALIGN rows below RESUME. Kept here rather than reaching
 * into the app's private enum, and it will drift if a row is inserted — which
 * is what the capture is for: a wrong number lands on a visibly wrong screen
 * rather than silently testing nothing. */
#define PS_ALIGN_STEPS 6
MoteVrV3 cuevr_app_rest(void);
int cuevr_app_aiming(void);
MoteVrV3 cuevr_app_pocket_room(void);
MoteVrV3 cuevr_app_table_room(float fx, float y, float fz);
float cuevr_app_table_yaw(void);
void cuevr_app_force_light(int i);
void cuevr_app_force_screen(const char *name);
MoteVrV3 cuevr_app_hud_room(void);
void cuevr_app_lock_hud_end(int sign);
void cuevr_app_force_start(int kind);
void cuevr_app_force_net(int join);
const char *cuevr_app_state_name(void);
int cuevr_app_table_kind(void);
void cuevr_app_break_selftest(void);
void cuevr_app_cue_probe(int *tracked, int *stroking, int *on_ball,
                         float *gap, float *speed, int *n);
void cuevr_app_cue_line(MoteVrV3 *tip, MoteVrV3 *axis);
int cuevr_app_net_turn(void);
int cuevr_app_net_seat(void);
int cuevr_app_score(int i);
int cuevr_app_frames(int i);
int cuevr_app_best_of(void);
int cuevr_app_balls_on(void);
unsigned cuevr_app_table_hash(void);
unsigned cuevr_app_object_hash(void);
unsigned cuevr_app_rules_hash(void);
float cuevr_app_cue_x(void);
float cuevr_app_cue_z(void);
void cuevr_app_force_body(int i);
void cuevr_app_force_framecol(int i);
void cuevr_app_force_surround(int i);
void cuevr_app_force_cue(int i);
MoteVrV3 cuevr_app_cue_mid(void);
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
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* ---- the two things the OpenXR host would answer ------------------------ */
static int s_target_srgb;
int  mote_xr_target_is_srgb(void) { return s_target_srgb; }
int  mote_xr_floor_relative(void) { return 1; }   /* the preview's y=0 is the floor */
int  mote_xr_multiview(void)      { return 0; }   /* one eye, one pass */
/* No runtime, so no render models. CUEVR_RENDER_MODEL=<file.glb> feeds one in
 * from disk anyway, which is the only way to look at the parser's output
 * without a headset — and the parser is the part most likely to be wrong. */
void *mote_xr_render_model_take(int hand, uint32_t *out_len) {
    static int done[2];
    if (out_len) *out_len = 0;
    if (hand < 0 || hand > 1 || done[hand]) return NULL;
    done[hand] = 1;
    const char *path = getenv(hand ? "CUEVR_RENDER_MODEL_R" : "CUEVR_RENDER_MODEL");
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cuevr: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
        if (out_len) *out_len = (uint32_t)n;
    } else { free(b); b = NULL; }
    fclose(f);
    return b;
}
void mote_xr_haptic(float i, int ms) { (void)i; (void)ms; }
void mote_xr_show_passthrough(int on) { (void)on; }   /* no cameras here */

/* ---- the fake head and hands -------------------------------------------- */
static float s_yaw = 0.35f, s_pitch = 0.55f, s_dist = 1.6f;  /* eye above the cloth, as a player stands */
static float s_yaw0 = 0.35f;          /* the requested yaw, before the table's own */
static float s_tfocus[3];             /* MOTE_VR_FOCUS=t:fx,y,fz — table space */
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
    /* The host has no runtime to ask for an aim pose, so the grip stands in.
     * Good enough to exercise the pointer maths; the real ray comes from
     * XR_ACTION aim on the headset. */
    for (int i = 0; i < 2; i++) { t->hand[i].aim = t->hand[i].pose; t->hand[i].aim_tracked = 1; }

    t->hand[MOTE_VR_LEFT].tracked = t->hand[MOTE_VR_RIGHT].tracked = 1;
    t->hand[MOTE_VR_LEFT].squeeze = s_lsqueeze;
    t->hand[MOTE_VR_LEFT].pose.p  = s_bridge;
    t->hand[MOTE_VR_RIGHT].pose.p = s_butt;

    /* Give the fake hands a REAL grip orientation, not identity. Without it the
     * controller models render in their own axes and there is no way to tell
     * whether the model-to-grip transform is right — which is the only thing that
     * matters for them.
     *
     * OpenXR/WebXR grip space: the origin is the centroid of the fist, -Z runs
     * along the handle toward the thumb, and X is perpendicular to the palm (+X out
     * of the back of the RIGHT hand, -X for the left). Holding a cue, the handle
     * lies along the cue, so -Z points up the cue toward the tip. */
    {
        MoteVrV3 fwd = mv3_sub(s_bridge, s_butt);
        if (mv3_len(fwd) > 1e-4f) {
            fwd = mv3_norm(fwd);                       /* toward the tip */
            MoteVrV3 zax = mv3_scale(fwd, -1.0f);      /* grip -Z is forward */
            MoteVrV3 up  = mv3(0, 1, 0);
            MoteVrV3 xax = mv3_cross(up, zax);
            if (mv3_len(xax) < 1e-3f) xax = mv3(1, 0, 0);
            xax = mv3_norm(xax);
            MoteVrV3 yax = mv3_cross(zax, xax);
            t->hand[MOTE_VR_RIGHT].pose.q = mq_from_axes(xax, yax, zax);
            /* the left hand is the mirror: +X the other way out of the palm */
            t->hand[MOTE_VR_LEFT].pose.q =
                mq_from_axes(mv3_scale(xax, -1.0f), mv3_scale(yax, -1.0f), zax);
        }
    }
    /* NOTE: these two lines used to sit here and overwrote the grip frame set
     * above with identity, so every controller render was unrotated. Removed — the
     * frame above is the point of the block. */
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
      /* MOTE_VR_FOCUS=cue: square on the butt of the cue the menu is showing,
       * wherever the table put it. Guessing table-space fractions for this
       * missed three times running. */
      else if (v && !strcmp(v, "cue")) focus_pocket = 4;
      /* MOTE_VR_FOCUS=hud: square on the panel, wherever the app has hung it.
       * Menu screens were framed by guessing table-space fractions, and the
       * panel moves — it sits past whichever end of the table you are NOT at,
       * so the guess is wrong half the time and wrong by a different amount on
       * every table size. Ask the app where it put it. */
      else if (v && !strcmp(v, "hud")) focus_pocket = 5;
      else if (v && !strncmp(v, "t:", 2)) {
          float a, b, c;
          if (sscanf(v + 2, "%f,%f,%f", &a, &b, &c) == 3) {
              focus_pocket = 3; s_tfocus[0] = a; s_tfocus[1] = b; s_tfocus[2] = c;
          }
      }
      else if (v) { float a, b, c; if (sscanf(v, "%f,%f,%f", &a, &b, &c) == 3)
          s_focus = mv3(a, b, c); } }
    { const char *v = getenv("MOTE_VR_VIEW");
      if (v) { float a, b, c; if (sscanf(v, "%f,%f,%f", &a, &b, &c) == 3) {
          s_yaw = a * 3.14159265f/180.0f; s_pitch = b * 3.14159265f/180.0f; s_dist = c; } } }
    s_yaw0 = s_yaw;
    /* MOTE_VR_BENCH=n: run n frames as fast as the machine will go and report
     * the frame time. The only honest way to say what a feature COSTS. */
    int bench = 0;
    { const char *v = getenv("MOTE_VR_BENCH"); if (v) bench = atoi(v); }
    int bg_light = getenv("CUEVR_BG") != NULL;
    const char *shot = getenv("MOTE_VR_SHOT");
    int shot_frame = 120;
    { const char *v = getenv("MOTE_VR_SHOT_FRAME"); if (v) shot_frame = atoi(v); }
    int auto_table = -1;
    { const char *v = getenv("CUEVR_TABLE"); if (v) auto_table = atoi(v); }
    int auto_stroke = getenv("CUEVR_STROKE") != NULL;
    /* CUEVR_AUTOPLAY=n: play n shots, one per visit to the table. CUEVR_TAG
     * labels this instance's trace so two of them can be told apart. */
    int autoplay = 0, ap_phase = 0, ap_shots = 0, ap_addressed = 0, ap_place = 0, ap_retry = 0, ap_fired = 0, ap_dec = 0, ap_overs = 0;
    MoteVrV3 ap_dir = mv3(1, 0, 0);   /* the line this shot is being played on */
    { const char *v = getenv("CUEVR_AUTOPLAY"); if (v) autoplay = atoi(v); }
    int quit_on_frame = getenv("CUEVR_QUIT_ON_FRAME") != NULL;
    int quit_shots = 0;
    { const char *v = getenv("CUEVR_QUIT_AFTER_SHOTS"); if (v) quit_shots = atoi(v); }
    long qof_at = -1;
    const char *tag = getenv("CUEVR_TAG");
    if (!tag) tag = "cuevr";
    /* CUEVR_LIGHT / CUEVR_BODY: force a rig and a frame design without walking
     * the menu, so each one can be photographed from a script. Applied after the
     * app has loaded its preferences, or the saved value would win. */
    int force_light = -1, force_body = -2;
    { const char *v = getenv("CUEVR_LIGHT"); if (v) force_light = atoi(v); }
    { const char *v = getenv("CUEVR_BODY");  if (v) force_body  = atoi(v); }
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
    /* CUEVR_ALIGN=n: at frame n, pause, walk down to ALIGN CONTROLS and open it,
     * so the alignment screen can be captured without a headset on. */
    int auto_align = -1;
    { const char *v = getenv("CUEVR_ALIGN"); if (v) auto_align = atoi(v); }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_Window *win = SDL_CreateWindow("CueVR (preview)", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, w, h,
        /* HIDDEN when we are only rendering into it. SDL_VIDEODRIVER=dummy is
         * the usual way to go headless and is no use here — it has no GL at all
         * — but a hidden window still gets a real context, and it does not take
         * the keyboard away from whoever is using the machine. Every scripted
         * capture and benchmark was stealing focus. */
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
        | ((shot || bench) ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval((shot || bench) ? 0 : 1);

    { const char *d = getenv("CUEVR_PREFS_DIR"); cuevr_prefs_dir(d ? d : "."); }

    MoteXrApp app;
    cuevr_app_describe(&app);
    if (app.gl_init(app.user) != 0) { fprintf(stderr, "cuevr: init failed\n"); return 1; }
    if (force_light >= 0) cuevr_app_force_light(force_light);
    /* Pin the panel to one end before anything looks at it, or the camera and
     * the panel chase each other round the table (see cuevr_app_lock_hud_end). */
    if (focus_pocket == 5) cuevr_app_lock_hud_end(1);
    /* START first, SCREEN second. They were the other way round, which meant
     * CUEVR_START=6 CUEVR_SCREEN=over racked a snooker table and then threw the
     * screen away — a screen that needs a game under it could not be reached at
     * all. Racking then jumping is the order that composes. */
    { const char *st = getenv("CUEVR_START"); if (st) cuevr_app_force_start(atoi(st)); }
    { const char *sc = getenv("CUEVR_SCREEN"); if (sc) cuevr_app_force_screen(sc); }
    if (getenv("CUEVR_BRKTEST")) { cuevr_app_break_selftest(); return 0; }
    if (auto_net) cuevr_app_force_net(!strcmp(auto_net, "join"));
    if (force_body >= -1) cuevr_app_force_body(force_body);
    { const char *v = getenv("CUEVR_CUE"); if (v) cuevr_app_force_cue(atoi(v)); }
    { const char *v = getenv("CUEVR_FRAMECOL"); if (v) cuevr_app_force_framecol(atoi(v)); }
    { const char *v = getenv("CUEVR_SURROUND"); if (v) cuevr_app_force_surround(atoi(v)); }

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
        else if (focus_pocket == 4) s_focus = cuevr_app_cue_mid();
        else if (focus_pocket == 5) s_focus = cuevr_app_hud_room();
        else if (focus_pocket == 3) {
            /* Table-space focus, as fractions of the half-extents. Yaw follows
             * the table too, so "look along the side rail" is one number and not
             * a guess about where the table was dropped in the room. */
            s_focus = cuevr_app_table_room(s_tfocus[0], s_tfocus[1], s_tfocus[2]);
            s_yaw = s_yaw0 + cuevr_app_table_yaw();
        }

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
            /* and tap it again to RESUME. A pause that is never lifted cannot
             * test what resuming does, which is where the bug was: the menu
             * stopped a rolling shot and put you back in AIM, so the shot never
             * resolved and the table was stuck. */
            if (nframe == auto_pause + 40) s_menu = 1;
            if (nframe == auto_pause + 42) s_menu = 0;
        }
        if (auto_align >= 0) {
            const int F = auto_align;
            if (nframe == F)     s_menu = 1;          /* pause */
            if (nframe == F + 2) s_menu = 0;
            /* down to ALIGN CONTROLS: it is the second row from the bottom */
            if (nframe >= F + 8 && nframe < F + 8 + PS_ALIGN_STEPS * 2)
                s_stick_r[1] = (nframe & 1) ? -1.0f : 0.0f;
            else if (nframe == F + 8 + PS_ALIGN_STEPS * 2) s_stick_r[1] = 0.0f;
            if (nframe == F + 8 + PS_ALIGN_STEPS * 2 + 4) s_a = 1;
            if (nframe == F + 8 + PS_ALIGN_STEPS * 2 + 6) s_a = 0;
        }
        /* CUEVR_HANDMOVE: walk BOTH hands sideways from frame 130, with no
         * trigger held. The cue is a rigid thing in your hands, so it must go
         * with them whatever the game is doing — and it did not, because the
         * update that reads the hands lived inside ST_AIM. */
        if (getenv("CUEVR_HANDMOVE") && nframe >= 130 && nframe < 200) {
            s_bridge.z += 0.004f;
            s_butt.z   += 0.004f;
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

        /* CUEVR_AUTOPLAY=n — play n shots, one whenever this end is at the
         * table, re-addressing the ball each time.
         *
         * This is the test the online code never had. CUEVR_STROKE plays exactly
         * ONE stroke and then stops, so the two-instance test could only ever
         * see a break — which is precisely the one shot that worked while the
         * shot after it was going nowhere. Whoever's turn it is plays; the other
         * end simply runs, which is the point: it has to receive.
         *
         * Deliberately not clever about aim. A shot into the pack from wherever
         * the white is scatters balls, ends turns and produces fouls, which is
         * more of the rules than an aimed pot would exercise. */
        if (autoplay > 0) {
            int aiming = cuevr_app_aiming();
            /* PUT THE BALL DOWN FIRST. Every frame begins with the break in
             * hand, so an autoplayer that only knows how to stroke sits in
             * PLACE for ever holding a ball — which is what the first run of
             * this did, and it looks exactly like a network stall. The trigger
             * drops it wherever the hand is; the rack is legal by construction
             * so anywhere will do. */
            /* ANSWER A FOUL DECISION. Snooker parks BOTH ends on this screen —
             * they each hold a frame that cannot go on until it is answered —
             * and only the fouled-against player may answer. Pressing A at both
             * ends is the right test: one of them is ignored, and if that gate
             * were missing the two ends would apply different decisions to the
             * same frame. Without this the snooker run stopped on the first
             * foul, one shot in.
             *
             * Down then up: the state arms a latch on entry, like placing. */
            /* AND START THE NEXT FRAME. In a best-of-N a won frame parks both
             * ends on the frame-over screen until somebody presses A, and each
             * end presses its own — they rack independently, from a break
             * alternation both compute. Worth driving, because it is the one
             * moment in a match where the two ends act without a packet
             * between them. */
            if (!strcmp(cuevr_app_state_name(), "OVER")) {
                /* Bounded. When the other end quits, ITS departure puts this one
                 * on the frame-over screen for good — and an autoplayer that
                 * presses A there for ever racks a new frame every twenty
                 * frames, which buried the real transcript under three hundred
                 * spurious tables. Three frames is more than any test needs. */
                if (ap_overs < 3) {
                    ap_dec++;
                    if (ap_dec == 21) ap_overs++;
                    s_a = (ap_dec > 20 && ap_dec < 26) ? 1 : 0;
                } else s_a = 0;
                ap_addressed = 0; ap_phase = 0; s_trig = 0;
            } else if (!strcmp(cuevr_app_state_name(), "DECIDE")) {
                ap_dec++;
                s_a = (ap_dec > 10 && ap_dec < 16) ? 1 : 0;
                ap_addressed = 0; ap_phase = 0; s_trig = 0;
            } else { ap_dec = 0; if (!auto_net || nframe > 80) s_a = 0; }
            if (!strcmp(cuevr_app_state_name(), "PLACE")) {
                ap_place++;
                /* Down, then up: the state arms a latch on entry, so the
                 * trigger has to be seen released before a pull counts. */
                s_trig = (ap_place > 10 && ap_place < 16) ? 1 : 0;
                ap_addressed = 0; ap_phase = 0;
            } else {
                ap_place = 0;
                /* Count a shot when the table leaves AIM having been struck.
                 * Counting it at the last frame of the delivery never fired: the
                 * tip crosses the ball part way through, so the state is already
                 * ROLL by then and the counter never moved — CUEVR_AUTOPLAY=14
                 * played a hundred and twenty. */
                if (!aiming) {
                    if (ap_fired) { ap_shots++; ap_fired = 0; }
                    ap_phase = 0; ap_addressed = 0; s_trig = 0;
                }
                else if (ap_shots < autoplay) {
                if (!ap_addressed) {
                    /* Address the ball where it is NOW: it has moved, and a
                     * stroke played from where the last one started goes
                     * nowhere near it.
                     *
                     * Straight down the +x line, exactly as the manual placement
                     * below does, because that line is known to reach the ball.
                     * The first version of this swung the whole stance round by
                     * up to 25 degrees to vary the shot and simply missed —
                     * which from the outside looks identical to a network stall,
                     * and cost a run to tell apart. Variety comes from hitting
                     * the ball off centre instead, which is what a player does
                     * anyway: well inside the ball's radius, so it always
                     * connects, and enough to send it somewhere different. */
                    /* AIM DOWN THE TABLE, not along the room's x axis.
                     *
                     * The table is dropped wherever the player is standing and
                     * turned to suit, so "+x in the room" is a different line
                     * every session — in the two-instance run it pointed across
                     * the width, and ten shots in a row rolled into a cushion
                     * without touching an object ball. Every one of them was a
                     * legitimate foul, the turn changed hands correctly, and the
                     * test proved nothing about a rack it never reached. Aiming
                     * at a point in TABLE space works whatever the room does. */
                    MoteVrV3 b = cuevr_app_cue_ball_room();
                    int v = (ap_shots + ap_retry) % 5;
                    MoteVrV3 tgt = cuevr_app_table_room(
                        (ap_retry & 1) ? -0.45f : 0.45f, 0.0f,
                        0.30f * (float)(v - 2));
                    MoteVrV3 d = mv3_sub(tgt, b);
                    d.y = 0.0f;
                    d = (mv3_len(d) > 1e-4f) ? mv3_norm(d) : mv3(1, 0, 0);
                    ap_dir = d;
                    float reach = CUEVR_CUE_LEN - cuevr_app_grip();
                    MoteVrV3 rst = cuevr_app_rest();
                    float back = reach + 0.10f;
                    s_butt   = mv3(b.x - d.x * back, b.y, b.z - d.z * back);
                    s_bridge = mv3_sub(mv3(b.x - d.x * (back - 0.90f), b.y,
                                           b.z - d.z * (back - 0.90f)), rst);
                    s_focus  = b;
                    ap_addressed = 1;
                    ap_phase = 0;
                    s_trig = 0;
                }
                ap_phase++;
                /* THEN CORRECT ONTO THE BALL, from the cue's own reported line.
                 *
                 * Placing the hands by arithmetic is not enough and never was:
                 * the bridge sits a rest-height above the hand, the hand carries
                 * an orientation, and the cue takes a minimum elevation from the
                 * cushions — so the axis this produces is close to the ball and
                 * not through it, and `on_ball` stays 0 while the tip sails past
                 * within a centimetre. That is what the first two runs of this
                 * were: a stroke played every time, connecting with nothing.
                 * Translating both hands by the perpendicular miss puts the axis
                 * through the centre in one step, whatever the geometry is
                 * doing, and a deliberate few millimetres off centre after that
                 * varies the shot without ever missing. */
                if (ap_phase < 8) {
                    MoteVrV3 tip, ax, b = cuevr_app_cue_ball_room();
                    cuevr_app_cue_line(&tip, &ax);
                    MoteVrV3 to_b = mv3_sub(b, tip);
                    MoteVrV3 perp = mv3_sub(to_b, mv3_scale(ax, mv3_dot(to_b, ax)));
                    /* and a few mm off centre, across the cue, for variety */
                    MoteVrV3 side = mv3_cross(mv3(0, 1, 0), ax);
                    if (mv3_len(side) > 1e-4f) {
                        side = mv3_norm(side);
                        perp = mv3_add(perp, mv3_scale(side,
                                   0.004f * (float)((ap_shots % 3) - 1)));
                    }
                    s_butt   = mv3_add(s_butt, perp);
                    s_bridge = mv3_add(s_bridge, perp);
                }
                if (ap_phase >= 8)  s_trig = 1;         /* take hold */
                /* Delivered over four frames rather than teleported in one: a
                 * single-frame jab reads as 22 m/s at the tip, which is every
                 * shot played at the clamp and no variety at all. */
                if (ap_phase >= 14 && ap_phase <= 17) {
                    float step = 0.030f + 0.006f * (float)(ap_shots % 4);
                    s_butt.x += step * ap_dir.x;
                    s_butt.z += step * ap_dir.z;
                    ap_fired = 1;
                }
                /* IF THAT DID NOT LAND, TRY A DIFFERENT LINE.
                 *
                 * A shot fires at phase 17 and the state leaves AIM, so still
                 * being here at 40 means the tip never connected — the white
                 * finishes in a corner often enough, and from there one fixed
                 * aim line can be blocked by the cushion the ball is against.
                 * Without this the run simply stopped, which reads exactly like
                 * the network bug this test exists to catch. */
                if (ap_phase > 40) { ap_addressed = 0; ap_retry++; }
                }
            }
        }

        /* CUEVR_QUIT_ON_FRAME=1 — stop once a frame has been won, plus a grace
         * period for the last state correction to land.
         *
         * A two-instance comparison has to end at the same LOGICAL point on both
         * ends or the tails are meaningless: cut at a fixed frame number, one
         * end is mid-shot and the other is not, and the traces differ for a
         * reason that has nothing to do with the network. A frame ending is a
         * point both ends reach. */
        if (quit_on_frame) {
            int fr = cuevr_app_frames(0) + cuevr_app_frames(1);
            if (fr > 0 && qof_at < 0) qof_at = nframe;
        }
        /* CUEVR_QUIT_AFTER_SHOTS=n — or after n shots have been SEEN, counting
         * the opponent's as well as our own. A snooker frame played by a random
         * autoplayer may never end, and both ends see the same shots, so this
         * is a logical stopping point too. */
        if (quit_shots > 0) {
            static int seen; static int was_roll;
            int roll = !strcmp(cuevr_app_state_name(), "ROLL");
            if (roll && !was_roll) seen++;
            was_roll = roll;
            if (seen >= quit_shots && !roll && qof_at < 0) qof_at = nframe;
        }
        if (qof_at >= 0 && nframe > qof_at + 250) running = 0;

        if (getenv("CUEVR_APDBG") && autoplay > 0) {
            int tr, st2, ob, sn; float gap, sp;
            cuevr_app_cue_probe(&tr, &st2, &ob, &gap, &sp, &sn);
            printf("[%s] f%-5ld %-6s ph=%-3d trig=%d tracked=%d strok=%d onball=%d "
                   "gap=%+.4f speed=%.2f n=%d butt=%.3f,%.3f bridge=%.3f,%.3f\n",
                   tag, nframe, cuevr_app_state_name(), ap_phase, s_trig,
                   tr, st2, ob, (double)gap, (double)sp, sn,
                   (double)s_butt.x, (double)s_butt.z,
                   (double)s_bridge.x, (double)s_bridge.z);
            fflush(stdout);
        }

        /* One line whenever anything a test cares about moves, on both ends, so
         * the transcripts can be laid side by side. */
        if (autoplay > 0 || auto_net) {
            static char last[200];
            char now[160];
            /* Keyed on what a TEST cares about, not on the table hash: the hash
             * changes every frame of a rolling shot, and a trace with a line per
             * frame of roll is a trace nobody can diff. The hash is printed —
             * it is the thing the two ends have to agree on — but it does not
             * trigger a line on its own, so what lands in the log is one line
             * per settled table. */
            const char *sn = cuevr_app_state_name();
            int rolling = !strcmp(sn, "ROLL");
            snprintf(now, sizeof now, "%-8s kind=%d turn=%d me=%d bo=%d score=%d/%d "
                     "frames=%d/%d on=%d",
                     sn, cuevr_app_table_kind(), cuevr_app_net_turn(),
                     cuevr_app_net_seat(), cuevr_app_best_of(),
                     cuevr_app_score(0), cuevr_app_score(1),
                     cuevr_app_frames(0), cuevr_app_frames(1),
                     cuevr_app_balls_on());
            /* The settled table IS part of what changed — a late state
             * correction moves the balls without moving anything else, and
             * leaving it out of the key hid exactly that: both ends finished a
             * frame identically and the log showed them differing, because the
             * joiner's correcting line was never printed. Not while rolling,
             * or there is a line a frame. */
            char key[200];
            snprintf(key, sizeof key, "%s|%08x|%08x", now,
                     rolling ? 0u : cuevr_app_object_hash(),
                     rolling ? 0u : cuevr_app_rules_hash());
            if (strcmp(key, last)) {
                snprintf(last, sizeof last, "%s", key);
                printf("[%s] f%-6ld %s hash=%08x obj=%08x rules=%08x "
                       "cue=%+.5f,%+.5f\n", tag, nframe, now,
                       cuevr_app_table_hash(), cuevr_app_object_hash(),
                       cuevr_app_rules_hash(),
                       (double)cuevr_app_cue_x(), (double)cuevr_app_cue_z());
                fflush(stdout);
            }
        }

        /* Park the fake hands behind the cue ball the first time the table is
         * placed, so the cue starts pointing at something. After that WASD and
         * the arrows move them, as your real hands would. */
        /* Place the fake hands only once the table is actually sited and we are
         * aiming — at frame 24 the cue ball is still wherever the default
         * placement put it, so the hands were lined up on a table that then
         * moved. Another consequence of levelling moving to the front. */
        /* The autoplayer owns the hands: it re-addresses the ball before every
         * shot, and this one-shot placement running in the same frame would
         * overwrite the address it had just made. */
        if (!hands_placed && autoplay > 0 && cuevr_app_aiming()) hands_placed = 1;
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
        /* CUEVR_BG=light: clear to a glaring colour instead of the dark room.
         * On the headset anything the app fails to draw shows the passthrough
         * camera, and a dark hole in a dark render is invisible — so for a
         * capture that is asking "is this actually closed?", the background has
         * to be the loudest thing on screen. */
        if (bg_light) glClearColor(0.95f, 0.30f, 0.85f, 1.0f);
        else          glClearColor(0.05f, 0.055f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float view[16], proj[16];
        mm4_view_from_pose(view, t.head);
        mm4_perspective(proj, 62.0f * 3.14159265f/180.0f, (float)w / (float)h, 0.02f, 60.0f);
        /* ...and no room floor either, or the floor grid becomes the thing you
         * see through the hole and the hole looks closed. */
        app.draw_eye(app.user, view, proj, bg_light ? 0 : 1);

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
        /* Benchmark. Timed with glFinish either side so the number is the GPU's
         * work and not how far ahead the driver has queued. The first 60 frames
         * are discarded: shader compilation, the first upload of every texture
         * and the AI's opening plan all land in them. */
        if (bench) {
            static double t_prev, acc, worst;
            static long n_meas;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = ts.tv_sec + ts.tv_nsec * 1e-9;
            glFinish();
            if (nframe > 60 && t_prev > 0.0) {
                double e = now - t_prev;
                acc += e; n_meas++;
                if (e > worst) worst = e;
            }
            t_prev = now;
            if (nframe >= bench) {
                printf("BENCH frames=%ld mean=%.3f ms worst=%.3f ms (%.1f fps)\n",
                       n_meas, n_meas ? acc * 1e3 / n_meas : 0.0, worst * 1e3,
                       n_meas ? n_meas / acc : 0.0);
                running = 0;
            }
        }
        SDL_GL_SwapWindow(win);
        nframe++;
    }

    app.gl_shutdown(app.user);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
