/*
 * Mote — the handheld, on a touchscreen.
 *
 * SDL main thread. Draws the real Thumby Color product photo (the same
 * studio/assets/thumby_color.png the Studio's emulator uses, with the same
 * calibration in screen.cfg) and composites the live 128x128 frame onto its LCD.
 * Touches on the photo's actual buttons drive the engine; a connected game
 * controller does the same and fades the touch highlights out.
 *
 * The engine itself runs on a worker thread (os/android/mote_android_os.c), so a
 * slow frame in a game never blocks input or the compositor.
 *
 * Builds twice from one source:
 *   · Android (ndk-build, android/app/jni/src/Android.mk) — the app
 *   · desktop (-DMOTE_SHELL_DESKTOP, CMake target mote_shell) — the same shell
 *     with mouse+keyboard, for developing and screenshotting it without a phone
 */
#include "mote_config.h"
#include "mote_platform.h"
#include "mote_plat_android.h"
#include "mote_android_os.h"
#include "mote_ui.h"
#include "mote_perf.h"

#include <SDL.h>
#include <SDL_main.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#ifdef __ANDROID__
#include <jni.h>
#endif

/* ====================================================================== *
 *  chassis geometry
 *
 *  Normalised to the chassis photo, tuned against the product shot — the same
 *  numbers as the Studio's emu_hit()/emu_outline(), so a press lands exactly on
 *  the button you can see. x/r are fractions of the photo WIDTH, y of its HEIGHT.
 * ====================================================================== */
enum { EB_A, EB_B, EB_UP, EB_DOWN, EB_LEFT, EB_RIGHT, EB_LB, EB_RB, EB_MENU, EB_N };

#define A_CX   0.901f
#define A_CY   0.442f
#define A_R    0.062f
#define B_CX   0.793f
#define B_CY   0.510f
#define B_R    0.062f
#define M_CX   0.196f
#define M_CY   0.790f
#define M_R    0.050f
#define D_CX   0.153f            /* d-pad centre */
#define D_CY   0.471f
#define D_HALF 0.115f            /* half the d-pad cross, in WIDTH fractions */
#define D_ARM  0.105f            /* arm length for the outline */
#define D_WID  0.036f            /* arm half-width for the outline */
/* The shoulders are the one pair that cannot sit where the hardware puts them.
 * On the handheld they are under your index fingers on the back edge; on a phone
 * held in landscape there is nothing behind the glass and both thumbs are at the
 * BOTTOM corners. So these two pads are anchored to the window, not to the photo
 * — the only controls in the shell that are — and sized for a thumb. They clear
 * the d-pad, MENU and A/B, all of which stay on the chassis where you can see
 * them. Fractions are of the window's smaller side. */
#define SH_W    0.26f            /* pad width  */
#define SH_H    0.14f            /* pad height */
#define SH_EDGE 0.025f           /* inset from the left/right edge */
#define SH_LIFT 0.030f           /* lift off the bottom, clear of the nav gesture */

/* Screen square inside the photo, in photo pixels (studio/assets/screen.cfg). */
static float s_spx = 1011.2f, s_spy = 319.6f, s_sps = 888.8f;

/* ====================================================================== *
 *  assets
 * ====================================================================== */
static SDL_Renderer *ren;
static SDL_Window   *win;
static SDL_Texture  *tex_photo, *tex_clear;   /* solid / see-through chassis */
static SDL_Texture  *tex_frame;               /* the live 128x128 LCD */
static SDL_Texture  *tex_ui;                  /* settings panel (128x128) */
static int           photo_w, photo_h;

/* Assets live in the APK on Android and in the repo on the desktop. */
static void *asset_load(const char *name, size_t *out_len) {
    const char *tries[3];
    char rel[256];
    snprintf(rel, sizeof rel, "studio/assets/%s", name);
    tries[0] = name; tries[1] = rel; tries[2] = NULL;
    for (int i = 0; tries[i]; i++) {
        SDL_RWops *f = SDL_RWFromFile(tries[i], "rb");
        if (!f) continue;
        Sint64 n = SDL_RWsize(f);
        if (n <= 0) { SDL_RWclose(f); continue; }
        void *buf = SDL_malloc((size_t)n);
        if (!buf) { SDL_RWclose(f); return NULL; }
        size_t got = SDL_RWread(f, buf, 1, (size_t)n);
        SDL_RWclose(f);
        if (got != (size_t)n) { SDL_free(buf); continue; }
        *out_len = got;
        return buf;
    }
    SDL_Log("[mote] asset not found: %s", name);
    return NULL;
}

/* Box-halve an RGBA image in place-ish (returns a new buffer, frees nothing). */
static unsigned char *halve_rgba(const unsigned char *src, int w, int h, int *ow, int *oh) {
    int nw = w / 2, nh = h / 2;
    if (nw < 1 || nh < 1) return NULL;
    unsigned char *dst = malloc((size_t)nw * nh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < nh; y++) {
        const unsigned char *r0 = src + (size_t)(y * 2) * w * 4;
        const unsigned char *r1 = r0 + (size_t)w * 4;
        unsigned char *o = dst + (size_t)y * nw * 4;
        for (int x = 0; x < nw; x++) {
            for (int c = 0; c < 4; c++)
                o[x * 4 + c] = (unsigned char)((r0[x * 8 + c] + r0[x * 8 + 4 + c] +
                                                r1[x * 8 + c] + r1[x * 8 + 4 + c]) >> 2);
        }
    }
    *ow = nw; *oh = nh;
    return dst;
}

/* Decode a PNG asset into a texture, halving it while it exceeds the renderer's
 * texture limit (a 2872px chassis outgrows some older GPUs and every software
 * renderer). ow/oh report the ORIGINAL image size, not the texture's: the texture
 * is always drawn into a scaled dest rect, and screen.cfg's calibration is in
 * original-photo pixels, so layout must stay in those units. */
static SDL_Texture *load_png(const char *name, int *ow, int *oh) {
    size_t len = 0;
    void *raw = asset_load(name, &len);
    if (!raw) return NULL;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(raw, (int)len, &w, &h, &n, 4);
    SDL_free(raw);
    if (!px) { SDL_Log("[mote] %s: %s", name, stbi_failure_reason()); return NULL; }
    if (ow) *ow = w;
    if (oh) *oh = h;

    SDL_RendererInfo ri; SDL_GetRendererInfo(ren, &ri);
    int maxw = ri.max_texture_width  > 0 ? ri.max_texture_width  : 2048;
    int maxh = ri.max_texture_height > 0 ? ri.max_texture_height : 2048;
    unsigned char *cur = px; int owned_by_stb = 1;
    while (w > maxw || h > maxh) {
        int nw, nh;
        unsigned char *small = halve_rgba(cur, w, h, &nw, &nh);
        if (!small) break;
        if (owned_by_stb) { stbi_image_free(cur); owned_by_stb = 0; } else free(cur);
        cur = small; w = nw; h = nh;
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                      SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, NULL, cur, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    if (owned_by_stb) stbi_image_free(cur); else free(cur);
    return t;
}

static void load_screen_cfg(void) {
    size_t len = 0;
    char *txt = asset_load("screen.cfg", &len);
    if (!txt) return;
    char buf[64];
    size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, txt, n); buf[n] = 0;
    SDL_free(txt);
    float a, b, c;
    if (sscanf(buf, "%f %f %f", &a, &b, &c) == 3 && c > 1.0f) { s_spx = a; s_spy = b; s_sps = c; }
}

/* ====================================================================== *
 *  settings (persisted next to the saves)
 * ====================================================================== */
enum { LAY_CHASSIS = 0, LAY_FILL = 1 };
static struct {
    int  layout;        /* LAY_* */
    int  clear;         /* see-through chassis */
    int  haptics;
    char relay[80];
} cfg = { LAY_CHASSIS, 0, 1, "" };

static char s_cfg_path[600];

static void cfg_load(void) {
    if (!s_cfg_path[0]) return;
    FILE *f = fopen(s_cfg_path, "r");
    if (!f) return;
    char ln[160];
    while (fgets(ln, sizeof ln, f)) {
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        char *nl = strpbrk(v, "\r\n"); if (nl) *nl = 0;
        if      (!strcmp(ln, "layout"))  cfg.layout  = atoi(v);
        else if (!strcmp(ln, "clear"))   cfg.clear   = atoi(v);
        else if (!strcmp(ln, "haptics")) cfg.haptics = atoi(v);
        else if (!strcmp(ln, "relay"))   snprintf(cfg.relay, sizeof cfg.relay, "%s", v);
    }
    fclose(f);
}
static void cfg_save(void) {
    if (!s_cfg_path[0]) return;
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) return;
    fprintf(f, "layout=%d\nclear=%d\nhaptics=%d\nrelay=%s\n",
            cfg.layout, cfg.clear, cfg.haptics, cfg.relay);
    fclose(f);
}

/* ====================================================================== *
 *  rumble / haptics (Android Vibrator through MoteActivity)
 * ====================================================================== */
#ifdef __ANDROID__
static jclass s_activity_cls;      /* global ref, resolved on the main thread */

static void jni_bind(void) {
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return;
    jclass local = (*env)->FindClass(env, "us/thumby/mote/MoteActivity");
    if (!local) { (*env)->ExceptionClear(env); return; }
    s_activity_cls = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
}
/* Callable from ANY thread: the class ref was resolved up front, so the engine
 * thread doesn't need the app class loader. */
static void jni_rumble(float intensity, int ms) {
    if (!s_activity_cls || !cfg.haptics) return;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return;
    jmethodID mid = (*env)->GetStaticMethodID(env, s_activity_cls, "moteVibrate", "(II)V");
    if (!mid) { (*env)->ExceptionClear(env); return; }
    int amp = (int)(intensity * 255.0f);
    if (amp < 1) amp = 1; if (amp > 255) amp = 255;
    (*env)->CallStaticVoidMethod(env, s_activity_cls, mid, (jint)ms, (jint)amp);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
#else
static void jni_bind(void) {}
static void jni_rumble(float intensity, int ms) { (void)intensity; (void)ms; }
#endif

/* The engine's rumble hook. A connected pad gets the buzz (that's where the
 * player's hands are); otherwise the phone does. Forward-declared statics live
 * further down, so this is wired up in main(). */
static SDL_GameController *s_pad;
static int s_using_pad;
static void shell_rumble(float intensity, int ms) {
    if (s_pad && s_using_pad) {
        Uint16 v = (Uint16)(intensity * 0xFFFF);
        if (SDL_GameControllerRumble(s_pad, v, v, (Uint32)(ms > 0 ? ms : 0)) == 0) return;
    }
    jni_rumble(intensity, ms);
}

static void tap_haptic(void) { if (cfg.haptics) jni_rumble(0.45f, 12); }

/* ====================================================================== *
 *  USB host: the byte pipe to a docked handheld
 *
 *  Android does it through Java (MoteUsb), because C cannot open a USB device
 *  here. The desktop build uses a Unix socket instead — MOTE_DOCK_SOCK — so the
 *  whole dock service (MN1 online proxy + gallery server) can be driven by a
 *  fake device without any hardware.
 * ====================================================================== */
#ifdef __ANDROID__
static jclass s_usb_cls;

static void usb_bind(void) {
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return;
    jclass local = (*env)->FindClass(env, "us/thumby/mote/MoteUsb");
    if (!local) { (*env)->ExceptionClear(env); return; }
    s_usb_cls = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
}
static int usb_call_i(const char *name, const char *sig, ...) {
    if (!s_usb_cls) return -1;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_usb_cls, name, sig);
    if (!m) { (*env)->ExceptionClear(env); return -1; }
    va_list ap; va_start(ap, sig);
    jint r = (*env)->CallStaticIntMethodV(env, s_usb_cls, m, ap);
    va_end(ap);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return (int)r;
}
static int  usb_present(void) { return usb_call_i("motePresent", "()I") == 1; }
static int  usb_open(void)    { return usb_call_i("moteOpen", "()I") == 1; }
static void usb_close(void) {
    if (!s_usb_cls) return;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return;
    jmethodID m = (*env)->GetStaticMethodID(env, s_usb_cls, "moteClose", "()V");
    if (m) (*env)->CallStaticVoidMethod(env, s_usb_cls, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
static int usb_read(void *buf, int max, int timeout_ms) {
    if (!s_usb_cls) return -1;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_usb_cls, "moteRead", "([BI)I");
    if (!m) { (*env)->ExceptionClear(env); return -1; }
    jbyteArray arr = (*env)->NewByteArray(env, max);
    if (!arr) return -1;
    jint n = (*env)->CallStaticIntMethod(env, s_usb_cls, m, arr, (jint)timeout_ms);
    if (n > 0) (*env)->GetByteArrayRegion(env, arr, 0, n, (jbyte *)buf);
    (*env)->DeleteLocalRef(env, arr);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return (int)n;
}
static int usb_write(const void *buf, int len) {
    if (!s_usb_cls) return -1;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_usb_cls, "moteWrite", "([BI)I");
    if (!m) { (*env)->ExceptionClear(env); return -1; }
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (!arr) return -1;
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)buf);
    jint n = (*env)->CallStaticIntMethod(env, s_usb_cls, m, arr, (jint)len);
    (*env)->DeleteLocalRef(env, arr);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return (int)n;
}
#else  /* desktop: a Unix socket stands in for the cable */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
static int s_dock_fd = -1;
static const char *dock_sock_path(void) { return SDL_getenv("MOTE_DOCK_SOCK"); }
static void usb_bind(void) {}
static int  usb_present(void) {
    const char *p = dock_sock_path();
    if (!p) return 0;
    if (s_dock_fd >= 0) return 1;
    struct stat st;
    return stat(p, &st) == 0;
}
static int usb_open(void) {
    if (s_dock_fd >= 0) return 1;
    const char *p = dock_sock_path();
    if (!p) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", p);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return 0; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    s_dock_fd = fd;
    return 1;
}
static void usb_close(void) { if (s_dock_fd >= 0) { close(s_dock_fd); s_dock_fd = -1; } }
static int usb_read(void *buf, int max, int timeout_ms) {
    if (s_dock_fd < 0) return -1;
    for (int waited = 0;; waited += 5) {
        ssize_t r = recv(s_dock_fd, buf, (size_t)max, MSG_DONTWAIT);
        if (r > 0) return (int)r;
        if (r == 0) return -1;                              /* peer closed */
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (waited >= timeout_ms) return 0;
        SDL_Delay(5);
    }
}
static int usb_write(const void *buf, int len) {
    if (s_dock_fd < 0) return -1;
    ssize_t w = send(s_dock_fd, buf, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w >= 0) return (int)w;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}
#endif

static const MoteUsbHost s_usb_host = {
    usb_present, usb_open, usb_close, usb_read, usb_write
};

/* ---- HTTPS for the gallery ---------------------------------------------- *
 * No TLS in C here, so Java does it on Android (HttpsURLConnection) and curl
 * does it on the desktop. Called from the gallery's worker thread. */
#ifdef __ANDROID__
static int shell_http_get(const char *url, const char *dest) {
    if (!s_activity_cls) return -1;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    jmethodID mid = (*env)->GetStaticMethodID(env, s_activity_cls, "moteHttpGet",
                                              "(Ljava/lang/String;Ljava/lang/String;)I");
    if (!mid) { (*env)->ExceptionClear(env); return -1; }
    jstring ju = (*env)->NewStringUTF(env, url);
    jstring jd = (*env)->NewStringUTF(env, dest);
    jint rc = (*env)->CallStaticIntMethod(env, s_activity_cls, mid, ju, jd);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); rc = -1; }
    (*env)->DeleteLocalRef(env, ju);
    (*env)->DeleteLocalRef(env, jd);
    return rc == 0 ? 0 : -1;
}
#else
static int shell_http_get(const char *url, const char *dest) {
    char cmd[900];
    snprintf(cmd, sizeof cmd,
             "curl -fsSL --max-time 120 -o '%s' '%s'", dest, url);
    return system(cmd) == 0 ? 0 : -1;
}
#endif

/* ====================================================================== *
 *  layout: where the chassis and its LCD land in the window
 * ====================================================================== */
static int s_dx, s_dy, s_dw, s_dh;      /* chassis rect, window px */
static int s_sx, s_sy, s_ss;            /* LCD square, window px */
static int s_ow = 1, s_oh = 1;          /* renderer output size */
static int s_down[EB_N];                /* buttons held this frame (touch OR pad) */
static SDL_Rect s_lb_rect, s_rb_rect;   /* shoulder pads, window-anchored */

/* Bottom corners, where the thumbs already are. Hit-test and draw share this so
 * they can never disagree. */
static void shoulder_layout(void) {
    int mind = s_ow < s_oh ? s_ow : s_oh;
    int w = (int)(SH_W * mind), h = (int)(SH_H * mind);
    int ex = (int)(SH_EDGE * mind), y = s_oh - h - (int)(SH_LIFT * mind);
    s_lb_rect = (SDL_Rect){ ex, y, w, h };
    s_rb_rect = (SDL_Rect){ s_ow - w - ex, y, w, h };
}

static void layout(void) {
    SDL_GetRendererOutputSize(ren, &s_ow, &s_oh);
    int W = s_ow, H = s_oh;
    if (photo_w <= 0 || photo_h <= 0) { s_dx = s_dy = 0; s_dw = W; s_dh = H; s_sx = s_sy = 0; s_ss = H; return; }

    float aspect = (float)photo_w / (float)photo_h;
    int integer_ok = 0;
    if (cfg.layout == LAY_CHASSIS) {
        /* Integer LCD scaling: pick the largest N where an N*128 screen keeps the
         * whole chassis on screen, so the game frame is pixel-exact. */
        int bestN = 0;
        for (int n = 1; n <= 24; n++) {
            float sc = (float)(n * MOTE_FB_W) / s_sps;
            if ((int)(photo_w * sc) <= W && (int)(photo_h * sc) <= H) bestN = n; else break;
        }
        if (bestN > 0) {
            float sc = (float)(bestN * MOTE_FB_W) / s_sps;
            s_dw = (int)(photo_w * sc); s_dh = (int)(photo_h * sc);
            s_ss = bestN * MOTE_FB_W;
            integer_ok = 1;
        }
    }
    if (!integer_ok) {
        /* Fill: largest chassis that fits, LCD scaled to match (soft, but bigger). */
        s_dh = H; s_dw = (int)(H * aspect);
        if (s_dw > W) { s_dw = W; s_dh = (int)(W / aspect); }
        s_ss = (int)(s_sps * (float)s_dw / (float)photo_w);
    }
    s_dx = (W - s_dw) / 2;
    s_dy = (H - s_dh) / 2;
    float sc = (float)s_dw / (float)photo_w;
    s_sx = s_dx + (int)(s_spx * sc);
    s_sy = s_dy + (int)(s_spy * sc);
    if (tex_frame)
        SDL_SetTextureScaleMode(tex_frame, (s_ss % MOTE_FB_W) == 0 ? SDL_ScaleModeNearest
                                                                  : SDL_ScaleModeLinear);
    shoulder_layout();
}

/* chassis-normalised -> window px */
static int BX(float n) { return s_dx + (int)(n * s_dw); }
static int BY(float n) { return s_dy + (int)(n * s_dh); }
static int BR(float n) { return (int)(n * s_dw); }

static int in_rect(const SDL_Rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* Which button is under a window-space point (-1 = none). Touch slop is generous:
 * a thumb landing near an edge should still count. The chassis buttons are tested
 * first, so where a window-anchored shoulder pad happens to reach across MENU's
 * slop the button you can actually see wins. */
static int hit_button(int mx, int my) {
    if (s_dw <= 0)
        return in_rect(&s_lb_rect, mx, my) ? EB_LB
             : in_rect(&s_rb_rect, mx, my) ? EB_RB : -1;
    struct { int b; float cx, cy, r; } round_[3] = {
        { EB_A, A_CX, A_CY, A_R }, { EB_B, B_CX, B_CY, B_R }, { EB_MENU, M_CX, M_CY, M_R },
    };
    for (int i = 0; i < 3; i++) {
        long dx = mx - BX(round_[i].cx), dy = my - BY(round_[i].cy);
        long r  = BR(round_[i].r * 1.25f);
        if (dx * dx + dy * dy <= r * r) return round_[i].b;
    }
    /* d-pad: a square split into quadrants by the dominant axis */
    float ndx = (mx - BX(D_CX)) / (float)s_dw, ndy = (my - BY(D_CY)) / (float)s_dw;
    if (fabsf(ndx) < D_HALF && fabsf(ndy) < D_HALF) {
        if (fabsf(ndx) > fabsf(ndy)) return ndx < 0 ? EB_LEFT : EB_RIGHT;
        return ndy < 0 ? EB_UP : EB_DOWN;
    }
    if (in_rect(&s_lb_rect, mx, my)) return EB_LB;
    if (in_rect(&s_rb_rect, mx, my)) return EB_RB;
    return -1;
}

/* ====================================================================== *
 *  drawing helpers
 * ====================================================================== */
static void disc(int cx, int cy, int r, Uint8 rr, Uint8 gg, Uint8 bb, Uint8 aa) {
    SDL_SetRenderDrawColor(ren, rr, gg, bb, aa);
    for (int dy = -r; dy <= r; dy++) {
        int o = (int)sqrtf((float)(r * r - dy * dy));
        SDL_RenderDrawLine(ren, cx - o, cy + dy, cx + o, cy + dy);
    }
}
static void ring(int cx, int cy, int r, int th, Uint8 rr, Uint8 gg, Uint8 bb, Uint8 aa) {
    SDL_SetRenderDrawColor(ren, rr, gg, bb, aa);
    int ir = r - th; if (ir < 0) ir = 0;
    for (int dy = -r; dy <= r; dy++) {
        int o = (int)sqrtf((float)(r * r - dy * dy));
        int i2 = ir * ir - dy * dy, in = i2 > 0 ? (int)sqrtf((float)i2) : 0;
        SDL_RenderDrawLine(ren, cx - o, cy + dy, cx - in, cy + dy);
        SDL_RenderDrawLine(ren, cx + in, cy + dy, cx + o, cy + dy);
    }
}
static void box(int x, int y, int w, int h, Uint8 rr, Uint8 gg, Uint8 bb, Uint8 aa) {
    SDL_SetRenderDrawColor(ren, rr, gg, bb, aa);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(ren, &r);
}
static void box_outline(int x, int y, int w, int h, int th, Uint8 rr, Uint8 gg, Uint8 bb, Uint8 aa) {
    SDL_SetRenderDrawColor(ren, rr, gg, bb, aa);
    for (int t = 0; t < th; t++) { SDL_Rect r = { x + t, y + t, w - 2 * t, h - 2 * t }; SDL_RenderDrawRect(ren, &r); }
}

/* A little label texture rendered with the OS's own UI font, so the shell needs
 * no second font: draw white-on-black into a scratch RGB565 buffer, then key the
 * black out to alpha. */
static SDL_Texture *label_tex(const char *s, int *out_w, int *out_h) {
    int w = mote_ui_text_w(s) + 2, h = mote_ui_text_h() + 2;
    if (w < 2 || h < 2 || w > MOTE_FB_W || h > MOTE_FB_H) return NULL;
    static uint16_t buf[MOTE_FB_W * MOTE_FB_H];
    memset(buf, 0, sizeof buf);
    mote_ui_text(buf, s, 1, 1, 0xFFFF);
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = buf[y * MOTE_FB_W + x];
            /* the font is anti-aliased, so luminance doubles as coverage */
            int lum = ((c >> 11) & 0x1F) << 3;
            px[y * w + x] = ((uint32_t)lum << 24) | 0x00FFFFFFu;
        }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(px);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return t;
}

/* The shoulders have no on-photo target (they are on the back edge of the real
 * shell), and no thumb reaches the top of a phone held in landscape. So they get
 * labelled pads in the bottom corners, always visible, brighter while held. */
static SDL_Texture *tex_lb, *tex_rb;
static int lb_w, lb_h, rb_w, rb_h;
/* the docked-handheld banner: re-rendered only when the status text changes */
static SDL_Texture *tex_dock;
static int dock_w, dock_h;
static char s_dock_txt[120];

static void shoulder_pads(void) {
    if (!tex_lb) tex_lb = label_tex("LB", &lb_w, &lb_h);
    if (!tex_rb) tex_rb = label_tex("RB", &rb_w, &rb_h);
    struct { const SDL_Rect *r; SDL_Texture *t; int tw, th; int down; } p[2] = {
        { &s_lb_rect, tex_lb, lb_w, lb_h, s_down[EB_LB] },
        { &s_rb_rect, tex_rb, rb_w, rb_h, s_down[EB_RB] },
    };
    for (int i = 0; i < 2; i++) {
        const SDL_Rect *r = p[i].r;
        int rad = r->h / 4;
        if (p[i].down) {
            box(r->x, r->y, r->w, r->h, 100, 175, 235, 185);
            box_outline(r->x, r->y, r->w, r->h, 3, 190, 230, 255, 255);
        } else {
            box(r->x, r->y, r->w, r->h, 22, 27, 42, 120);
            box_outline(r->x, r->y, r->w, r->h, 2, 130, 190, 240, 130);
        }
        (void)rad;
        if (!p[i].t) continue;
        int lh = r->h / 2, lw = p[i].tw * lh / (p[i].th ? p[i].th : 1);
        SDL_Rect d = { r->x + (r->w - lw) / 2, r->y + (r->h - lh) / 2, lw, lh };
        SDL_SetTextureAlphaMod(p[i].t, p[i].down ? 255 : 190);
        SDL_RenderCopy(ren, p[i].t, NULL, &d);
    }
}

/* A pressed button: a soft fill plus a bright edge, shaped to that button. */
static void glow(int btn, Uint8 a) {
    int dcx = BX(D_CX), dcy = BY(D_CY);
    int arm = BR(D_ARM), wid = BR(D_WID);
    switch (btn) {
    case EB_A:    disc(BX(A_CX), BY(A_CY), BR(A_R), 130, 210, 255, (Uint8)(a * 0.42f));
                  ring(BX(A_CX), BY(A_CY), BR(A_R), BR(A_R) / 7 + 2, 150, 220, 255, a); break;
    case EB_B:    disc(BX(B_CX), BY(B_CY), BR(B_R), 130, 210, 255, (Uint8)(a * 0.42f));
                  ring(BX(B_CX), BY(B_CY), BR(B_R), BR(B_R) / 7 + 2, 150, 220, 255, a); break;
    case EB_MENU: disc(BX(M_CX), BY(M_CY), BR(M_R), 130, 210, 255, (Uint8)(a * 0.42f));
                  ring(BX(M_CX), BY(M_CY), BR(M_R), BR(M_R) / 6 + 2, 150, 220, 255, a); break;
    case EB_UP:    box(dcx - wid, dcy - arm, 2 * wid, arm, 130, 210, 255, (Uint8)(a * 0.42f));
                   box_outline(dcx - wid, dcy - arm, 2 * wid, arm, 3, 150, 220, 255, a); break;
    case EB_DOWN:  box(dcx - wid, dcy, 2 * wid, arm, 130, 210, 255, (Uint8)(a * 0.42f));
                   box_outline(dcx - wid, dcy, 2 * wid, arm, 3, 150, 220, 255, a); break;
    case EB_LEFT:  box(dcx - arm, dcy - wid, arm, 2 * wid, 130, 210, 255, (Uint8)(a * 0.42f));
                   box_outline(dcx - arm, dcy - wid, arm, 2 * wid, 3, 150, 220, 255, a); break;
    case EB_RIGHT: box(dcx, dcy - wid, arm, 2 * wid, 130, 210, 255, (Uint8)(a * 0.42f));
                   box_outline(dcx, dcy - wid, arm, 2 * wid, 3, 150, 220, 255, a); break;
    case EB_LB: case EB_RB: break;      /* shoulder_pads() draws its own pressed look */
    default: break;
    }
}

/* ====================================================================== *
 *  input
 * ====================================================================== */
#define MAX_TOUCH 10
typedef struct { SDL_FingerID id; int active, btn; float x, y; } Touch;
static Touch  s_touch[MAX_TOUCH];
static int    s_pad_down[EB_N];
static Uint32 s_last_touch_ms;

static Touch *touch_find(SDL_FingerID id) {
    for (int i = 0; i < MAX_TOUCH; i++) if (s_touch[i].active && s_touch[i].id == id) return &s_touch[i];
    return NULL;
}
static Touch *touch_alloc(void) {
    for (int i = 0; i < MAX_TOUCH; i++) if (!s_touch[i].active) return &s_touch[i];
    return NULL;
}
static void pads_scan(void) {
    if (s_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            s_pad = SDL_GameControllerOpen(i);
            if (s_pad) { SDL_Log("[mote] controller: %s", SDL_GameControllerName(s_pad)); return; }
        }
}

/* Controller -> the nine handheld buttons. Left stick doubles as the d-pad so a
 * pad with a mushy hat still works; L1/R1 are the shoulders, Start is MENU. */
static void pad_read(void) {
    memset(s_pad_down, 0, sizeof s_pad_down);
    if (!s_pad) return;
    const float DZ = 0.45f;
    float lx = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    float ly = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    if (lx < -DZ) s_pad_down[EB_LEFT] = 1;
    if (lx >  DZ) s_pad_down[EB_RIGHT] = 1;
    if (ly < -DZ) s_pad_down[EB_UP] = 1;
    if (ly >  DZ) s_pad_down[EB_DOWN] = 1;
    struct { int sdl, eb; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP,        EB_UP    },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN,      EB_DOWN  },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT,      EB_LEFT  },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,     EB_RIGHT },
        { SDL_CONTROLLER_BUTTON_A,              EB_A     },
        { SDL_CONTROLLER_BUTTON_B,              EB_B     },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,   EB_LB    },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,  EB_RB    },
        { SDL_CONTROLLER_BUTTON_START,          EB_MENU  },
        { SDL_CONTROLLER_BUTTON_BACK,           EB_MENU  },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(s_pad, map[i].sdl)) s_pad_down[map[i].eb] = 1;
    /* Triggers as extra A/B so twin-stick-ish pads feel right. */
    if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) s_pad_down[EB_A] = 1;
    if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 12000) s_pad_down[EB_B] = 1;
    for (int i = 0; i < EB_N; i++) if (s_pad_down[i]) { s_using_pad = 1; break; }
}

#ifdef MOTE_SHELL_DESKTOP
/* Desktop dev build: the host emulator's keyboard map, so muscle memory carries. */
static void keys_read(int *out) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    out[EB_UP]    |= k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W];
    out[EB_DOWN]  |= k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S];
    out[EB_LEFT]  |= k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A];
    out[EB_RIGHT] |= k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    out[EB_A]     |= k[SDL_SCANCODE_PERIOD] || k[SDL_SCANCODE_K];
    out[EB_B]     |= k[SDL_SCANCODE_COMMA]  || k[SDL_SCANCODE_J];
    out[EB_LB]    |= k[SDL_SCANCODE_LSHIFT];
    out[EB_RB]    |= k[SDL_SCANCODE_SPACE];
    out[EB_MENU]  |= k[SDL_SCANCODE_RETURN];
}
#endif

/* ====================================================================== *
 *  settings panel — drawn with the OS's own UI kit into a 128x128 buffer, so it
 *  looks like a Mote system screen and needs no second font.
 * ====================================================================== */
/* Every row is a tap target, and nothing in here depends on a key that a phone
 * doesn't have. BACK TO GAMES only appears while a game is running; the relay
 * editor swaps the list for a value + OK/CANCEL so the soft keyboard never hides
 * the way out. */
enum { ROW_LAYOUT = 0, ROW_CHASSIS, ROW_HAPTIC, ROW_FPS, ROW_RELAY, ROW_BACK, ROW_CLOSE, ROW_MAX };
enum { ED_OK = 0, ED_CANCEL, ED_N };
/* mirrors PERF_NAME in os/mote_menu.c */
static const char *const PERF_LEVEL[4] = { "OFF", "FPS", "MINI", "FULL" };

static int  s_settings_open, s_sel, s_top;
static int  s_editing_relay;
static char s_edit[80];
static uint16_t s_uifb[MOTE_FB_W * MOTE_FB_H];
static SDL_Rect s_row_rect[ROW_MAX];      /* window-space, for direct taps */
static int      s_row_id[ROW_MAX];        /* which ROW_* each drawn slot is */
static int      s_row_slots;              /* rows actually drawn this frame */
static SDL_Rect s_gear_rect;

/* Which rows exist right now (BACK TO GAMES is conditional). */
static int settings_ids(int *ids) {
    int n = 0;
    ids[n++] = ROW_LAYOUT;
    ids[n++] = ROW_CHASSIS;
    ids[n++] = ROW_HAPTIC;
    ids[n++] = ROW_FPS;
    ids[n++] = ROW_RELAY;
    if (mote_shell_in_game()) ids[n++] = ROW_BACK;
    ids[n++] = ROW_CLOSE;
    return n;
}

/* Engine frame rate, measured from the published-frame counter — so it reads
 * correctly in the launcher and in games alike, and it is the rate the player
 * actually sees rather than whatever the game thinks it is running at. */
static int s_fps;
static void fps_sample(void) {
    static Uint32 t0; static uint32_t seq0;
    Uint32 now = SDL_GetTicks();
    uint32_t seq = mote_shell_frame_seq();
    if (!t0) { t0 = now; seq0 = seq; return; }
    if (now - t0 < 1000) return;      /* 1s window: ±1 fps, which is all this needs */
    s_fps = (int)((seq - seq0) * 1000u / (now - t0));
    t0 = now; seq0 = seq;
}

static void settings_row_label(int id, char *out, int cap) {
    switch (id) {
    case ROW_LAYOUT:  snprintf(out, cap, "LAYOUT  %s", cfg.layout == LAY_CHASSIS ? "CHASSIS" : "FILL"); break;
    case ROW_CHASSIS: snprintf(out, cap, "SHELL   %s", cfg.clear ? "CLEAR" : "PHOTO"); break;
    case ROW_HAPTIC:  snprintf(out, cap, "HAPTICS %s", cfg.haptics ? "ON" : "OFF"); break;
    case ROW_FPS:     snprintf(out, cap, "FPS  %d   %s", s_fps, PERF_LEVEL[mote_perf_level() & 3]); break;
    case ROW_RELAY:   snprintf(out, cap, "RELAY"); break;   /* value goes below the list */
    case ROW_BACK:    snprintf(out, cap, "BACK TO GAMES"); break;
    default:          snprintf(out, cap, "CLOSE"); break;
    }
}

static void settings_activate(int id) {
    switch (id) {
    case ROW_LAYOUT:  cfg.layout = !cfg.layout; layout(); break;
    case ROW_CHASSIS: cfg.clear = !cfg.clear; break;
    case ROW_HAPTIC:  cfg.haptics = !cfg.haptics; break;
    case ROW_FPS:     mote_perf_toggle(); return;   /* cycles the on-LCD perf overlay */
    case ROW_RELAY:
        s_editing_relay = 1; s_sel = ED_OK;
        snprintf(s_edit, sizeof s_edit, "%s", mote_shell_get_relay());
        SDL_StartTextInput();
        return;
    case ROW_BACK:
        mote_shell_request_exit_game();
        s_settings_open = 0;
        return;
    default:          s_settings_open = 0; break;
    }
    cfg_save();
}

static void relay_commit(int keep) {
    if (keep) {
        snprintf(cfg.relay, sizeof cfg.relay, "%s", s_edit);
        mote_shell_set_relay(cfg.relay);
        cfg_save();
    }
    s_editing_relay = 0; s_sel = ROW_RELAY;
    SDL_StopTextInput();
}

/* mote_ui_list geometry (os/mote_ui.c): rows are ROW_H=15 tall from y0, and the
 * list stops FTR_H=15 above the bottom. */
#define UI_ROW_H  15
#define UI_ROW_Y0 22
#define UI_ROWS_VISIBLE ((MOTE_FB_H - 15 - UI_ROW_Y0) / UI_ROW_H)

static void settings_draw(void) {
    char labels[ROW_MAX][30];
    const char *items[ROW_MAX];
    int ids[ROW_MAX], n;

    if (s_editing_relay) {
        /* The keyboard is about to cover the bottom of the screen, so the editor
         * lives at the TOP of the panel: value, then OK / CANCEL. */
        static const char *ED[ED_N] = { "OK", "CANCEL" };
        n = ED_N;
        for (int i = 0; i < n; i++) { ids[i] = i; items[i] = ED[i]; }
        mote_ui_ground(s_uifb);
        mote_ui_header(s_uifb, "RELAY", -1, -1);
        { char v[40]; snprintf(v, sizeof v, "%.24s_", s_edit);
          mote_ui_text(s_uifb, v, 4, UI_ROW_Y0, MOTE_UI_GOLD); }
        mote_ui_list(s_uifb, items, n, s_sel, 0, UI_ROW_Y0 + UI_ROW_H);
        mote_ui_footer(s_uifb, "host  or  host:port");
    } else {
        n = settings_ids(ids);
        for (int i = 0; i < n; i++) {
            settings_row_label(ids[i], labels[i], sizeof labels[i]);
            items[i] = labels[i];
        }
        if (s_sel >= n) s_sel = n - 1;
        mote_ui_ground(s_uifb);
        mote_ui_header(s_uifb, "MOTE SHELL", s_sel + 1, n);
        s_top = mote_ui_list(s_uifb, items, n, s_sel, s_top, UI_ROW_Y0);
        /* The footer doubles as the selected row's detail line — that's where the
         * relay address and the live link state go, since the list fills the panel. */
        const char *lk = mote_shell_link_info();
        char foot[48];
        if (ids[s_sel] == ROW_RELAY)   snprintf(foot, sizeof foot, "%.30s", mote_shell_get_relay());
        else if (strncmp(lk, "off", 3)) snprintf(foot, sizeof foot, "%.30s", lk);
        else                            snprintf(foot, sizeof foot, "TAP A ROW TO CHANGE IT");
        mote_ui_footer(s_uifb, foot);
    }

    if (!tex_ui)
        tex_ui = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                   MOTE_FB_W, MOTE_FB_H);
    if (!tex_ui) return;
    SDL_UpdateTexture(tex_ui, NULL, s_uifb, MOTE_FB_W * (int)sizeof(uint16_t));
    SDL_SetTextureScaleMode(tex_ui, SDL_ScaleModeNearest);

    /* Panel: integer-scaled, a little larger than the LCD so it reads. Centred
     * normally; pinned near the top while the keyboard is up. */
    int n2 = 1;
    while ((n2 + 1) * MOTE_FB_W <= (s_ow < s_oh ? s_ow : s_oh) * 8 / 10) n2++;
    int side = n2 * MOTE_FB_W;
    SDL_Rect dst = { (s_ow - side) / 2,
                     s_editing_relay ? s_oh / 40 : (s_oh - side) / 2, side, side };
    box(0, 0, s_ow, s_oh, 0, 0, 0, 170);
    SDL_RenderCopy(ren, tex_ui, NULL, &dst);
    box_outline(dst.x - 2, dst.y - 2, dst.w + 4, dst.h + 4, 2, 96, 176, 255, 255);

    /* Hit-boxes for the rows that were actually drawn (the list scrolls, so slot
     * i shows ids[s_top + i]). */
    float sc = (float)side / (float)MOTE_FB_H;
    int y0 = s_editing_relay ? UI_ROW_Y0 + UI_ROW_H : UI_ROW_Y0;
    int top = s_editing_relay ? 0 : s_top;
    s_row_slots = 0;
    for (int i = 0; i < UI_ROWS_VISIBLE && top + i < n; i++) {
        s_row_rect[i].x = dst.x;
        s_row_rect[i].y = dst.y + (int)((y0 + i * UI_ROW_H) * sc);
        s_row_rect[i].w = dst.w;
        s_row_rect[i].h = (int)(UI_ROW_H * sc);
        s_row_id[i] = ids[top + i];
        s_row_slots++;
    }
}

/* A tap anywhere in the panel: the row hit-boxes were laid out by the previous
 * frame's draw, which is also what the player was looking at when they tapped. */
static void panel_tap(int mx, int my) {
    for (int i = 0; i < s_row_slots; i++) {
        if (mx < s_row_rect[i].x || mx >= s_row_rect[i].x + s_row_rect[i].w) continue;
        if (my < s_row_rect[i].y || my >= s_row_rect[i].y + s_row_rect[i].h) continue;
        tap_haptic();
        if (s_editing_relay) { s_sel = s_row_id[i]; relay_commit(s_row_id[i] == ED_OK); return; }
        s_sel = i + s_top;
        settings_activate(s_row_id[i]);
        return;
    }
}

/* ====================================================================== *
 *  headless capture (dev/CI): the host backend's MOTE_SHOT, one level up — this
 *  grabs the COMPOSITED window (chassis + LCD + highlights), which is the thing
 *  worth eyeballing when the layout changes.
 *
 *    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
 *    MOTE_SHELL_SHOT=/tmp/shell.ppm MOTE_SHELL_KEYS="a:40-50" ./mote_shell
 * ====================================================================== */
static const char *s_shot_path;
static int s_shot_frame = 90, s_frame_no;

static void dump_ppm(const char *path) {
    int w = s_ow, h = s_oh;
    unsigned char *px = malloc((size_t)w * h * 3);
    if (!px) return;
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, px, w * 3) == 0) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            fwrite(px, 1, (size_t)w * h * 3, f);
            fclose(f);
            SDL_Log("[mote] wrote %s (%dx%d) at frame %d", path, w, h, s_frame_no);
        }
    } else SDL_Log("[mote] RenderReadPixels: %s", SDL_GetError());
    free(px);
}

/* MOTE_SHELL_KEYS="a:5-15 up:40-60" — hold a button over a frame range.
 * "panel:from-to" is a pseudo-button that holds the settings panel open, so the
 * panel's own rows can be driven from a script too. */
#define SCRIPT_PANEL EB_N
static struct { int btn, from, to; } s_script[24];
static int s_nscript = -1;
static int script_name(const char *s, int n) {
    static const struct { const char *n; int b; } m[] = {
        {"up",EB_UP},{"down",EB_DOWN},{"left",EB_LEFT},{"right",EB_RIGHT},
        {"a",EB_A},{"b",EB_B},{"lb",EB_LB},{"rb",EB_RB},{"menu",EB_MENU},
        {"panel",SCRIPT_PANEL} };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        if ((int)strlen(m[i].n) == n && !strncmp(m[i].n, s, (size_t)n)) return m[i].b;
    return -1;
}
static void script_parse(void) {
    s_nscript = 0;
    const char *e = SDL_getenv("MOTE_SHELL_KEYS");
    if (!e) return;
    while (*e && s_nscript < 24) {
        while (*e == ' ' || *e == ',') e++;
        const char *name = e;
        while (*e && *e != ':') e++;
        if (*e != ':') break;
        int b = script_name(name, (int)(e - name)); e++;
        int from = atoi(e);
        while (*e && *e != '-' && *e != ' ' && *e != ',') e++;
        int to = (*e == '-') ? atoi(e + 1) : from;
        while (*e && *e != ' ' && *e != ',') e++;
        if (b >= 0) { s_script[s_nscript].btn = b; s_script[s_nscript].from = from;
                      s_script[s_nscript].to = to; s_nscript++; }
    }
}
static void script_apply(int *down) {
    if (s_nscript < 0) script_parse();
    int has_panel = 0, want_panel = 0;
    for (int i = 0; i < s_nscript; i++) {
        int panel = (s_script[i].btn == SCRIPT_PANEL);
        if (panel) has_panel = 1;
        if (s_frame_no < s_script[i].from || s_frame_no > s_script[i].to) continue;
        if (panel) want_panel = 1;
        else       down[s_script[i].btn] = 1;
    }
    if (has_panel) s_settings_open = want_panel;   /* held open, not toggled */
}

/* ====================================================================== *
 *  main
 * ====================================================================== */
static SDL_Thread *s_engine;
static int engine_thread(void *a) { (void)a; return mote_android_os_main(); }

int main(int argc, char *argv[]) {
    /* AUDIO is initialised here, on the main thread, even though the platform
     * backend is the one that opens the device (from the engine thread). */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                 SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

#ifdef MOTE_SHELL_DESKTOP
    win = SDL_CreateWindow("Mote (Android shell)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#else
    win = SDL_CreateWindow("Mote", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
                           SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
#endif
    if (!win) { SDL_Log("CreateWindow: %s", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { SDL_Log("CreateRenderer: %s", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    /* ---- storage + settings ---- */
    const char *store = NULL;
#ifdef __ANDROID__
    store = SDL_AndroidGetInternalStoragePath();
#endif
    if (!store || !store[0]) store = ".";
    mote_shell_set_storage(store);
    snprintf(s_cfg_path, sizeof s_cfg_path, "%s/shell.cfg", store);
    cfg_load();
    if (cfg.relay[0]) mote_shell_set_relay(cfg.relay);
    else              snprintf(cfg.relay, sizeof cfg.relay, "%s", mote_shell_get_relay());
    jni_bind();
    usb_bind();
    mote_shell_set_rumble_cb(shell_rumble);
    mote_shell_set_http_cb(shell_http_get);
    mote_shell_set_usb_host(&s_usb_host);
    mote_android_gallery_set_base(SDL_getenv("MOTE_GALLERY_BASE"));

    /* ---- where game modules live ----
     * MoteActivity.getArguments() hands us the APK's native-library dir (the
     * games bundled with the build) and the app's external games dir (side-loaded
     * modules), in that priority order. */
    for (int i = 1; i < argc; i++) mote_android_os_add_dir(argv[i]);
    if (argc < 2) {
        const char *env = SDL_getenv("MOTE_GAME_DIR");
        mote_android_os_add_dir(env && env[0] ? env : "build_android/modules");
    }

    /* ---- assets ---- */
    load_screen_cfg();
    tex_photo = load_png("thumby_color.png", &photo_w, &photo_h);
    { int cw, ch; tex_clear = load_png("thumby_color_clear.png", &cw, &ch); }
    tex_frame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                 MOTE_FB_W, MOTE_FB_H);
    SDL_SetTextureScaleMode(tex_frame, SDL_ScaleModeNearest);
    layout();

    s_shot_path = SDL_getenv("MOTE_SHELL_SHOT");
    if (SDL_getenv("MOTE_SHELL_SHOT_FRAME")) s_shot_frame = atoi(SDL_getenv("MOTE_SHELL_SHOT_FRAME"));
    if (SDL_getenv("MOTE_SHELL_PANEL")) s_settings_open = 1;   /* capture the panel */

    pads_scan();
    /* The dock idles until a handheld is on the cable, so starting it always is
     * free — and it means plugging one in Just Works with no mode to select. */
    mote_android_dock_start();
    s_engine = SDL_CreateThread(engine_thread, "mote-engine", NULL);
    if (!s_engine) { SDL_Log("engine thread failed: %s", SDL_GetError()); return 1; }

    static uint16_t frame[MOTE_FB_W * MOTE_FB_H];
    uint32_t last_seq = (uint32_t)-1;
    int running = 1;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_RESIZED) layout();
                break;
            case SDL_APP_WILLENTERBACKGROUND: mote_shell_set_paused(1); break;
            case SDL_APP_DIDENTERFOREGROUND:  mote_shell_set_paused(0); break;
            case SDL_RENDER_TARGETS_RESET:
            case SDL_RENDER_DEVICE_RESET:
                if (tex_photo) { SDL_DestroyTexture(tex_photo); tex_photo = NULL; }
                if (tex_clear) { SDL_DestroyTexture(tex_clear); tex_clear = NULL; }
                if (tex_ui)    { SDL_DestroyTexture(tex_ui);    tex_ui = NULL; }
                if (tex_frame) { SDL_DestroyTexture(tex_frame); tex_frame = NULL; }
                if (tex_lb)    { SDL_DestroyTexture(tex_lb);    tex_lb = NULL; }
                if (tex_rb)    { SDL_DestroyTexture(tex_rb);    tex_rb = NULL; }
                tex_photo = load_png("thumby_color.png", &photo_w, &photo_h);
                { int cw, ch; tex_clear = load_png("thumby_color_clear.png", &cw, &ch); }
                tex_frame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                              SDL_TEXTUREACCESS_STREAMING, MOTE_FB_W, MOTE_FB_H);
                last_seq = (uint32_t)-1;
                layout();
                break;
            case SDL_CONTROLLERDEVICEADDED: pads_scan(); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (s_pad) { SDL_GameControllerClose(s_pad); s_pad = NULL; }
                s_using_pad = 0;
                break;

            case SDL_TEXTINPUT:
                if (s_editing_relay) {
                    size_t n = strlen(s_edit);
                    snprintf(s_edit + n, sizeof s_edit - n, "%s", ev.text.text);
                }
                break;
            case SDL_KEYDOWN:
                if (s_editing_relay) {
                    SDL_Keycode k = ev.key.keysym.sym;
                    if (k == SDLK_BACKSPACE) { size_t n = strlen(s_edit); if (n) s_edit[n - 1] = 0; }
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) relay_commit(1);
                    else if (k == SDLK_ESCAPE || k == SDLK_AC_BACK)  relay_commit(0);
                    break;
                }
                if (ev.key.keysym.sym == SDLK_AC_BACK) {      /* Android back button */
                    if (s_settings_open) s_settings_open = 0; else s_settings_open = 1;
                }
#ifdef MOTE_SHELL_DESKTOP
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                if (ev.key.keysym.sym == SDLK_F1) s_settings_open = !s_settings_open;
#endif
                break;

            /* ---- touch: fingers are tracked so a slide off a button releases it ---- */
            case SDL_FINGERDOWN: {
                s_last_touch_ms = SDL_GetTicks();
                s_using_pad = 0;
                int mx = (int)(ev.tfinger.x * s_ow), my = (int)(ev.tfinger.y * s_oh);
                if (s_settings_open) { panel_tap(mx, my); break; }
                if (mx >= s_gear_rect.x && mx < s_gear_rect.x + s_gear_rect.w &&
                    my >= s_gear_rect.y && my < s_gear_rect.y + s_gear_rect.h) {
                    s_settings_open = 1; s_sel = 0; tap_haptic(); break;
                }
                Touch *t = touch_alloc();
                if (!t) break;
                t->active = 1; t->id = ev.tfinger.fingerId;
                t->x = ev.tfinger.x; t->y = ev.tfinger.y;
                t->btn = hit_button(mx, my);
                if (t->btn >= 0) tap_haptic();
                break;
            }
            case SDL_FINGERMOTION: {
                Touch *t = touch_find(ev.tfinger.fingerId);
                if (!t) break;
                t->x = ev.tfinger.x; t->y = ev.tfinger.y;
                int nb = hit_button((int)(t->x * s_ow), (int)(t->y * s_oh));
                if (nb != t->btn) { t->btn = nb; if (nb >= 0) tap_haptic(); }
                break;
            }
            case SDL_FINGERUP: {
                Touch *t = touch_find(ev.tfinger.fingerId);
                if (t) { t->active = 0; t->btn = -1; }
                break;
            }
#ifdef MOTE_SHELL_DESKTOP
            /* Mouse stands in for a finger on the desktop dev build. */
            case SDL_MOUSEBUTTONDOWN: {
                int mx = ev.button.x, my = ev.button.y;
                if (s_settings_open) { panel_tap(mx, my); break; }
                if (mx >= s_gear_rect.x && mx < s_gear_rect.x + s_gear_rect.w &&
                    my >= s_gear_rect.y && my < s_gear_rect.y + s_gear_rect.h) { s_settings_open = 1; break; }
                Touch *t = touch_alloc();
                if (t) { t->active = 1; t->id = -1 - ev.button.button;
                         t->x = mx / (float)s_ow; t->y = my / (float)s_oh;
                         t->btn = hit_button(mx, my); }
                break;
            }
            case SDL_MOUSEMOTION: {
                Touch *t = touch_find(-1 - SDL_BUTTON_LEFT);
                if (t) { t->x = ev.motion.x / (float)s_ow; t->y = ev.motion.y / (float)s_oh;
                         t->btn = hit_button(ev.motion.x, ev.motion.y); }
                break;
            }
            case SDL_MOUSEBUTTONUP: {
                Touch *t = touch_find(-1 - ev.button.button);
                if (t) { t->active = 0; t->btn = -1; }
                break;
            }
#endif
            default: break;
            }
        }

        /* ---- assemble the handheld's button state ---- */
        memset(s_down, 0, sizeof s_down);
        for (int i = 0; i < MAX_TOUCH; i++)
            if (s_touch[i].active && s_touch[i].btn >= 0) s_down[s_touch[i].btn] = 1;
        pad_read();
        for (int i = 0; i < EB_N; i++) if (s_pad_down[i]) s_down[i] = 1;
#ifdef MOTE_SHELL_DESKTOP
        keys_read(s_down);
#endif
        script_apply(s_down);
        /* The settings panel eats the game's input while it's up. */
        MoteButtons btn;
        memset(&btn, 0, sizeof btn);
        if (!s_settings_open) {
            btn.up = s_down[EB_UP]; btn.down = s_down[EB_DOWN];
            btn.left = s_down[EB_LEFT]; btn.right = s_down[EB_RIGHT];
            btn.a = s_down[EB_A]; btn.b = s_down[EB_B];
            btn.lb = s_down[EB_LB]; btn.rb = s_down[EB_RB];
            btn.menu = s_down[EB_MENU];
        } else {
            /* ...but a controller (or the desktop keyboard) can still drive it. */
            static int prev[EB_N];
            int ids[ROW_MAX];
            int n = s_editing_relay ? ED_N : settings_ids(ids);
            if (s_editing_relay) for (int i = 0; i < n; i++) ids[i] = i;
            if (s_sel >= n) s_sel = n - 1;
            if (s_down[EB_DOWN] && !prev[EB_DOWN]) s_sel = (s_sel + 1) % n;
            if (s_down[EB_UP]   && !prev[EB_UP])   s_sel = (s_sel + n - 1) % n;
            if (s_down[EB_A]    && !prev[EB_A]) {
                if (s_editing_relay) relay_commit(s_sel == ED_OK);
                else                 settings_activate(ids[s_sel]);
            }
            if (s_down[EB_B] && !prev[EB_B]) {
                if (s_editing_relay) relay_commit(0);
                else                 s_settings_open = 0;
            }
            memcpy(prev, s_down, sizeof prev);
        }
        mote_shell_set_buttons(&btn);

        /* ---- present ---- */
        fps_sample();
        uint32_t seq = mote_shell_frame_seq();
        if (seq != last_seq) {
            last_seq = seq;
            mote_shell_get_frame(frame);
            if (tex_frame) SDL_UpdateTexture(tex_frame, NULL, frame, MOTE_FB_W * (int)sizeof(uint16_t));
        }
        SDL_SetRenderDrawColor(ren, 8, 9, 14, 255);
        SDL_RenderClear(ren);

        SDL_Texture *chassis = (cfg.clear && tex_clear) ? tex_clear : tex_photo;
        if (chassis) {
            SDL_Rect d = { s_dx, s_dy, s_dw, s_dh };
            SDL_RenderCopy(ren, chassis, NULL, &d);
        }
        if (tex_frame) {
            SDL_Rect sc = { s_sx, s_sy, s_ss, s_ss };
            SDL_RenderCopy(ren, tex_frame, NULL, &sc);
        }

        /* Pressed-button highlights fade out once a controller takes over. */
        Uint8 ov = 255;
        if (s_using_pad) {
            Uint32 since = SDL_GetTicks() - s_last_touch_ms;
            ov = since > 1200 ? 0 : (Uint8)(255 - since * 255 / 1200);
        }
        if (ov > 0) {
            shoulder_pads();
            for (int i = 0; i < EB_N; i++)
                if (s_down[i]) glow(i, ov);
        }

        /* Settings pip: top-centre, between the two shoulder pads and clear of
         * both the buttons and the phone's edge gestures. */
        {
            int s = s_oh / 16; if (s < 28) s = 28; if (s > 64) s = 64;
            s_gear_rect = (SDL_Rect){ (s_ow - s) / 2, s / 3, s, s };
            box(s_gear_rect.x, s_gear_rect.y, s, s, 20, 24, 38, 170);
            box_outline(s_gear_rect.x, s_gear_rect.y, s, s, 1, 96, 176, 255, 150);
            for (int i = 0; i < 3; i++)
                box(s_gear_rect.x + s / 4, s_gear_rect.y + s / 4 + i * s / 5,
                    s / 2, s / 12 + 1, 200, 220, 240, 220);
        }

        /* Docked-handheld banner. The phone is busy being a dock as well as a
         * console, and the player needs to see that it took the cable — and what
         * it is doing with it — without opening anything. */
        if (mote_android_dock_attached()) {
            const char *st = mote_android_dock_status();
            int lh = s_oh / 26; if (lh < 16) lh = 16;
            int bw = s_ow / 3;  if (bw < 200) bw = 200;
            int bx = (s_ow - bw) / 2, by = s_gear_rect.y + s_gear_rect.h + lh / 3;
            box(bx, by, bw, lh, 16, 34, 26, 205);
            box_outline(bx, by, bw, lh, 1, 90, 220, 150, 220);
            if (!tex_dock || strcmp(st, s_dock_txt)) {
                if (tex_dock) SDL_DestroyTexture(tex_dock);
                snprintf(s_dock_txt, sizeof s_dock_txt, "%s", st);
                tex_dock = label_tex(st[0] ? st : "Thumby docked", &dock_w, &dock_h);
            }
            if (tex_dock && dock_h > 0) {
                int th = lh * 3 / 5, tw = dock_w * th / dock_h;
                if (tw > bw - 12) { tw = bw - 12; }
                SDL_Rect d = { bx + (bw - tw) / 2, by + (lh - th) / 2, tw, th };
                SDL_SetTextureAlphaMod(tex_dock, 235);
                SDL_RenderCopy(ren, tex_dock, NULL, &d);
            }
        }

        if (s_settings_open) settings_draw();

        if (s_shot_path && ++s_frame_no >= s_shot_frame) { dump_ppm(s_shot_path); running = 0; }
        else if (!s_shot_path) s_frame_no++;

        SDL_RenderPresent(ren);
        /* Backstop for the frame cap: vsync normally paces this loop, but a
         * headless/dummy driver has none and would spin a core (and make the
         * scripted-input frame windows meaningless). */
        {
            static Uint32 next_ms;
            Uint32 now = SDL_GetTicks();
            if (next_ms > now && next_ms - now <= 32) SDL_Delay(next_ms - now);
            next_ms = (next_ms > now ? next_ms : now) + 16;
        }
    }

    mote_shell_request_quit();
    mote_android_dock_stop();
    if (s_engine) SDL_WaitThread(s_engine, NULL);
    if (s_pad) SDL_GameControllerClose(s_pad);
    if (tex_lb) SDL_DestroyTexture(tex_lb);
    if (tex_rb) SDL_DestroyTexture(tex_rb);
    if (tex_ui) SDL_DestroyTexture(tex_ui);
    if (tex_frame) SDL_DestroyTexture(tex_frame);
    if (tex_clear) SDL_DestroyTexture(tex_clear);
    if (tex_photo) SDL_DestroyTexture(tex_photo);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
