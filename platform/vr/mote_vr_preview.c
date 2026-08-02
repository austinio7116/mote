/*
 * Mote VR — the headset build, without the headset.
 *
 * The whole VR app except OpenXR: the real engine on its real worker thread,
 * the real launcher, the real chassis mesh, the real shaders, the real hold
 * logic. What it fakes is exactly one thing — the tracking — which it drives
 * from a mouse instead of from two controllers.
 *
 * This exists because a Quest build cannot be looked at from the machine that
 * writes it. Everything here runs headless (SDL_VIDEODRIVER=offscreen) and can
 * screenshot itself, so the model, the lighting, the LCD, the button glows and
 * the way the console sits between your hands are all verifiable before an APK
 * is ever installed.
 *
 *   MOTE_VR_SHOT=/tmp/x.png   render N frames, write a PNG, exit
 *   MOTE_VR_SHOT_FRAME=n      which frame to grab (default 120)
 *   MOTE_VR_KEYS="a b up"     press these buttons before the shot
 *   MOTE_VR_SPAN=0.22         how far apart to put the fake hands, metres
 *   MOTE_VR_VIEW=yaw,pitch,d  where to put the fake head
 *   MOTE_VR_GAME=wormote      boot straight into a game
 *   MOTE_VR_BACKDROP=0        no floor grid (what passthrough looks like)
 *   MOTE_VR_TILT=0            override the handheld pitch (degrees)
 *   MOTE_VR_TESTCARD=1        replace the LCD with a known pattern, to check
 *                             that the 128x128 lands exactly on the glass
 *   MOTE_VR_GRIP=both         hold the side trigger(s): both | left | right
 *   MOTE_VR_SRGB=1            render through an sRGB framebuffer with the shader
 *                             emitting linear — bit for bit what a Quest's sRGB
 *                             swapchain does, so the headset's colour pipeline
 *                             can be measured from a desk
 *   MOTE_VR_TESTFILL=r,g,b    fill the LCD with one known colour (0-255), for
 *                             measuring what actually comes out the far end
 *
 * Live, it is a normal window: drag to orbit, wheel to dolly, WASD/arrows for
 * the d-pad, J/K for A/B, U/I for LB/RB, Enter for MENU. G squeezes both side
 * triggers (F and H one at a time), which is how the console is picked up and
 * moved; [ and ] pull the fake hands together and apart, so holding G with [ or
 * ] exercises the real two-handed resize.
 */
#include "mote_vr.h"
#include "mote_config.h"
#include "mote_platform.h"
#include "mote_plat_android.h"
#include "mote_android_os.h"

#include <SDL.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* ---- assets from disk --------------------------------------------------- */
static void *slurp(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "mote-vr: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = (int)n;
    return p;
}

/* ---- the fake head and hands -------------------------------------------- */
static float s_yaw = 0.0f, s_pitch = -0.30f, s_dist = 0.55f;
static float s_span = 0.20f;
static MoteButtons s_keys;                 /* keyboard stand-in for controllers */
static int   s_grip[2];                    /* side triggers, per hand */
/* The console is world-anchored now, so the camera orbits *it* rather than the
 * origin — otherwise it would be placed in front of the head on frame 0 and
 * then sit off to one side of every screenshot. */
static MoteVrV3 s_focus;

static void fake_tracking(MoteVrTracking *t, float dt) {
    memset(t, 0, sizeof *t);
    t->dt = dt;

    MoteVrV3 focus = s_focus;
    MoteVrV3 dir = mv3(cosf(s_pitch) * sinf(s_yaw), sinf(s_pitch),
                       cosf(s_pitch) * cosf(s_yaw));
    t->head.p = mv3_add(focus, mv3_scale(dir, s_dist));
    MoteVrV3 fwd = mv3_norm(mv3_sub(focus, t->head.p));
    MoteVrV3 right = mv3_norm(mv3_cross(mv3(0, 1, 0), mv3_scale(fwd, -1.0f)));
    MoteVrV3 up = mv3_cross(mv3_scale(fwd, -1.0f), right);
    t->head.q = mq_from_axes(right, up, mv3_scale(fwd, -1.0f));

    /* Hands: either side of the focus point, square to the head, as if the
     * console were being held up in front of the player. */
    for (int i = 0; i < 2; i++) {
        float side = (i == MOTE_VR_LEFT) ? -0.5f : 0.5f;
        t->hand[i].pose.p = mv3_add(focus, mv3_scale(right, side * s_span));
        t->hand[i].pose.q = mq_ident();
        t->hand[i].tracked = 1;
        t->hand[i].squeeze = s_grip[i] ? 1.0f : 0.0f;
    }
    /* Keyboard drives the same fields the controllers would. */
    t->hand[MOTE_VR_RIGHT].stick_x = s_keys.right ? 1.0f : (s_keys.left ? -1.0f : 0.0f);
    t->hand[MOTE_VR_RIGHT].stick_y = s_keys.up    ? 1.0f : (s_keys.down ? -1.0f : 0.0f);
    t->hand[MOTE_VR_RIGHT].btn_lower = s_keys.a;
    t->hand[MOTE_VR_RIGHT].btn_upper = s_keys.b;
    t->hand[MOTE_VR_LEFT].trigger  = s_keys.lb ? 1.0f : 0.0f;
    t->hand[MOTE_VR_RIGHT].trigger = s_keys.rb ? 1.0f : 0.0f;
    t->hand[MOTE_VR_LEFT].menu = s_keys.menu;
}

/* ---- engine thread ------------------------------------------------------ */
static int engine_thread(void *a) { (void)a; return mote_android_os_main(); }

static int shell_http_get(const char *url, const char *dest) {
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "curl -fsSL -o '%s' '%s'", dest, url);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---- screenshot --------------------------------------------------------- */
static void write_png(const char *path, int w, int h) {
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* GL hands back bottom-up */
    for (int y = 0; y < h / 2; y++) {
        unsigned char *a = px + (size_t)y * w * 4;
        unsigned char *b = px + (size_t)(h - 1 - y) * w * 4;
        for (int i = 0; i < w * 4; i++) { unsigned char t = a[i]; a[i] = b[i]; b[i] = t; }
    }
    int len = 0;
    unsigned char *png = stbi_write_png_to_mem(px, w * 4, w, h, 4, &len);
    if (png) {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(png, 1, (size_t)len, f); fclose(f); }
        free(png);
        printf("mote-vr: wrote %s (%dx%d)\n", path, w, h);
    }
    free(px);
}

/* A frame whose every edge and corner is unmistakable, so "is the LCD exactly
 * on the glass?" is a measurement rather than a squint: a white border, a
 * one-pixel inset frame, and four differently coloured corner blocks. */
static void testcard(uint16_t *fb) {
    for (int y = 0; y < MOTE_FB_H; y++)
        for (int x = 0; x < MOTE_FB_W; x++) {
            int edge = (x == 0 || y == 0 || x == MOTE_FB_W-1 || y == MOTE_FB_H-1);
            int grid = ((x % 16) == 0 || (y % 16) == 0);
            uint16_t c = edge ? MOTE_RGB565(255,255,255)
                      : grid ? MOTE_RGB565(60,60,90)
                             : MOTE_RGB565(12,14,26);
            if (x < 12 && y < 12)                       c = MOTE_RGB565(255,0,0);
            if (x >= MOTE_FB_W-12 && y < 12)            c = MOTE_RGB565(0,255,0);
            if (x < 12 && y >= MOTE_FB_H-12)            c = MOTE_RGB565(0,80,255);
            if (x >= MOTE_FB_W-12 && y >= MOTE_FB_H-12) c = MOTE_RGB565(255,220,0);
            fb[y * MOTE_FB_W + x] = c;
        }
}

/* Flat fill, for measuring the colour pipeline end to end. */
static void testfill(uint16_t *fb, int r, int g, int b) {
    uint16_t c = MOTE_RGB565(r, g, b);
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) fb[i] = c;
}

static void apply_keys(const char *spec) {
    if (!spec) return;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, " ,"); tok; tok = strtok(NULL, " ,")) {
        if      (!strcmp(tok, "a"))     s_keys.a = 1;
        else if (!strcmp(tok, "b"))     s_keys.b = 1;
        else if (!strcmp(tok, "up"))    s_keys.up = 1;
        else if (!strcmp(tok, "down"))  s_keys.down = 1;
        else if (!strcmp(tok, "left"))  s_keys.left = 1;
        else if (!strcmp(tok, "right")) s_keys.right = 1;
        else if (!strcmp(tok, "lb"))    s_keys.lb = 1;
        else if (!strcmp(tok, "rb"))    s_keys.rb = 1;
        else if (!strcmp(tok, "menu"))  s_keys.menu = 1;
    }
}

int main(int argc, char **argv) {
    int w = 1280, h = 800;
    { const char *sz = getenv("MOTE_VR_SIZE");
      if (sz) { int a, b; if (sscanf(sz, "%dx%d", &a, &b) == 2 && a > 64 && b > 64) { w = a; h = b; } } }
    { const char *v = getenv("MOTE_VR_SPAN"); if (v) s_span = (float)atof(v); }
    { const char *v = getenv("MOTE_VR_VIEW");
      if (v) { float a, b, c; if (sscanf(v, "%f,%f,%f", &a, &b, &c) == 3) {
          s_yaw = a * 3.14159265f/180.0f; s_pitch = b * 3.14159265f/180.0f; s_dist = c; } } }
    int backdrop = 1;
    { const char *v = getenv("MOTE_VR_BACKDROP"); if (v) backdrop = atoi(v); }
    const char *shot = getenv("MOTE_VR_SHOT");
    int shot_frame = 120;
    { const char *v = getenv("MOTE_VR_SHOT_FRAME"); if (v) shot_frame = atoi(v); }
    apply_keys(getenv("MOTE_VR_KEYS"));
    { const char *v = getenv("MOTE_VR_GRIP");   /* "both" | "left" | "right" */
      if (v) { s_grip[MOTE_VR_LEFT]  = (!strcmp(v,"both") || !strcmp(v,"left"));
               s_grip[MOTE_VR_RIGHT] = (!strcmp(v,"both") || !strcmp(v,"right")); } }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_Window *win = SDL_CreateWindow("Mote VR (preview)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(shot ? 0 : 1);

    /* ---- the engine, exactly as the phone starts it ---- */
    const char *store = getenv("MOTE_VR_STORE");
    mote_shell_set_storage(store && store[0] ? store : ".");
    mote_shell_set_http_cb(shell_http_get);
    for (int i = 1; i < argc; i++) mote_android_os_add_dir(argv[i]);
    if (argc < 2) {
        const char *env = getenv("MOTE_GAME_DIR");
        mote_android_os_add_dir(env && env[0] ? env : "build_android/modules");
    }
    { const char *g = getenv("MOTE_VR_GAME");
      if (g && g[0]) setenv("MOTE_SHELL_AUTORUN", g, 1); }

    /* ---- assets ---- */
    MoteVrAssets assets;
    memset(&assets, 0, sizeof assets);
    const char *dir = getenv("MOTE_VR_ASSETS");
    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/chassis.mvm", dir && dir[0] ? dir : "vr/assets");
    snprintf(p2, sizeof p2, "%s/chassis.jpg", dir && dir[0] ? dir : "vr/assets");
    assets.chassis_mesh = slurp(p1, &assets.chassis_mesh_len);
    assets.chassis_tex  = slurp(p2, &assets.chassis_tex_len);
    if (!assets.chassis_mesh || !assets.chassis_tex) return 1;
    if (mote_vr_render_init(&assets) != 0) return 1;

    /* The headset renders into an sRGB swapchain, which encodes on write. To
     * measure that path rather than trust it, do the same here: an FBO with an
     * sRGB colour attachment, the shader emitting linear, and the read-back
     * taken from the FBO — so the bytes measured are the bytes a Quest's
     * compositor would be handed. */
    const int srgb_fbo = getenv("MOTE_VR_SRGB") != NULL;
    GLuint fbo = 0, fbo_tex = 0, fbo_depth = 0;
    if (srgb_fbo) {
        glGenTextures(1, &fbo_tex);
        glBindTexture(GL_TEXTURE_2D, fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &fbo_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "mote-vr: sRGB FBO incomplete (0x%04x)\n", st);
            return 1;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        mote_vr_render_set_target_srgb(1);
        printf("mote-vr: rendering through an sRGB framebuffer (headset path)\n");
    }

    MoteVrHoldState hold;
    float tilt = -18.0f;
    { const char *v = getenv("MOTE_VR_TILT"); if (v) tilt = (float)atof(v); }
    mote_vr_hold_init(&hold, MOTE_VR_SCALE_DEFAULT, tilt);
    const int card = getenv("MOTE_VR_TESTCARD") != NULL;
    int fill_rgb[3] = { -1, 0, 0 };
    { const char *v = getenv("MOTE_VR_TESTFILL");
      if (v) sscanf(v, "%d,%d,%d", &fill_rgb[0], &fill_rgb[1], &fill_rgb[2]); }

    SDL_Thread *eng = SDL_CreateThread(engine_thread, "mote-engine", NULL);
    if (!eng) { fprintf(stderr, "engine thread: %s\n", SDL_GetError()); return 1; }

    static uint16_t frame[MOTE_FB_W * MOTE_FB_H];
    uint32_t last_seq = (uint32_t)-1;
    int running = 1, dragging = 0;
    uint64_t t_prev = SDL_GetPerformanceCounter();
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
                    if (s_pitch >  1.4f) s_pitch =  1.4f;
                    if (s_pitch < -1.4f) s_pitch = -1.4f;
                }
                break;
            case SDL_MOUSEWHEEL:
                s_dist -= ev.wheel.y * 0.04f;
                if (s_dist < 0.12f) s_dist = 0.12f;
                if (s_dist > 3.0f)  s_dist = 3.0f;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    w = ev.window.data1; h = ev.window.data2;
                }
                break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                int d = (ev.type == SDL_KEYDOWN);
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_w: case SDLK_UP:    s_keys.up = d; break;
                case SDLK_s: case SDLK_DOWN:  s_keys.down = d; break;
                case SDLK_a: case SDLK_LEFT:  s_keys.left = d; break;
                case SDLK_d: case SDLK_RIGHT: s_keys.right = d; break;
                case SDLK_j: s_keys.a = d; break;
                case SDLK_k: s_keys.b = d; break;
                case SDLK_u: s_keys.lb = d; break;
                case SDLK_i: s_keys.rb = d; break;
                case SDLK_RETURN: s_keys.menu = d; break;
                case SDLK_g: s_grip[0] = s_grip[1] = d; break;
                case SDLK_f: s_grip[MOTE_VR_LEFT]  = d; break;
                case SDLK_h: s_grip[MOTE_VR_RIGHT] = d; break;
                case SDLK_LEFTBRACKET:  if (d) s_span *= 0.94f; break;
                case SDLK_RIGHTBRACKET: if (d) s_span *= 1.06f; break;
                default: break;
                }
                break;
            }
            default: break;
            }
        }

        uint64_t t_now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(t_now - t_prev) / (double)SDL_GetPerformanceFrequency());
        t_prev = t_now;
        if (dt > 0.1f) dt = 0.1f;

        MoteVrTracking track;
        fake_tracking(&track, dt);

        MoteVrConsole console;
        MoteButtons btn;
        int was_placed = hold.placed;
        mote_vr_hold_update(&hold, &track, &console, &btn);
        if (!was_placed && console.placed) s_focus = console.pose.p;
        mote_shell_set_buttons(&btn);

        uint32_t seq = mote_shell_frame_seq();
        if (fill_rgb[0] >= 0) {
            if (last_seq == (uint32_t)-1) {
                testfill(frame, fill_rgb[0], fill_rgb[1], fill_rgb[2]);
                mote_vr_render_set_frame(frame, 1); last_seq = 1;
            }
        } else if (card) {
            if (last_seq == (uint32_t)-1) { testcard(frame); mote_vr_render_set_frame(frame, 1); last_seq = 1; }
        } else if (seq != last_seq) {
            last_seq = seq;
            mote_shell_get_frame(frame);
            mote_vr_render_set_frame(frame, seq);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glClearColor(0.055f, 0.06f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float view[16], proj[16];
        mm4_view_from_pose(view, track.head);
        mm4_perspective(proj, 60.0f * 3.14159265f/180.0f, (float)w / (float)h, 0.02f, 50.0f);
        mote_vr_render_eye(view, proj, &console, &btn, &track, backdrop);

        if (shot && nframe == shot_frame) {
            glFinish();
            write_png(shot, w, h);      /* reads from whichever FBO is bound */
            running = 0;
        }
        if (srgb_fbo) {
            /* mirror it to the window so the live view still works */
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        SDL_GL_SwapWindow(win);
        nframe++;
    }

    mote_shell_request_quit();
    SDL_WaitThread(eng, NULL);
    mote_vr_render_shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
