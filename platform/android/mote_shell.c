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
/* PNG for the alpha mask and the icons, JPEG for the chassis photograph — see
 * android/tools/gen_chassis.py for why the photo is not a PNG. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
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
/* Shoulder placement. On the handheld these are on the back top edge, under your
 * index fingers; held like a gamepad, a phone puts those fingers at the TOP
 * corners, which is the default. SHELL puts them on the chassis instead, at the
 * real angles its top edges run at. Fractions are of the window's smaller side. */
#define SH_W    0.26f            /* pad width  */
#define SH_H    0.11f            /* pad height */
#define SH_EDGE 0.020f           /* inset from the left/right edge */
#define SH_LIFT 0.020f           /* drop from the top edge */
/* SHELL mode tilt. Fitting each bumper to the alpha edge of its own shoulder
 * gave -23.2 and +17.4 degrees, but the shell is symmetric — the light falls
 * differently on the two shoulders, so the edge the fit found is not quite the
 * edge the shell has. One tilt, the mean of the two, mirrored; x mirrored about
 * the chassis centre and y identical, so the pair reads level. */
#define SHELL_TILT_DEG 20.3f
#define SHELL_LB_DEG (-SHELL_TILT_DEG)
#define SHELL_RB_DEG (+SHELL_TILT_DEG)

/* The chassis photo: the APK carries a baked JPEG + alpha mask (240 KB), while
 * a desktop dev build falls back to studio/assets/thumby_color.png so editing
 * the source photo needs no bake step. Either way screen.cfg sits beside it and
 * is in that image's own pixels. */
static void load_chassis(void);

/* Screen square inside the photo, in photo pixels (studio/assets/screen.cfg). */
static float s_spx = 1011.2f, s_spy = 319.6f, s_sps = 888.8f;

/* ====================================================================== *
 *  assets
 * ====================================================================== */
static SDL_Renderer *ren;
static SDL_Window   *win;
static SDL_Texture  *tex_photo;
static SDL_Texture  *tex_frame;               /* the live 128x128 LCD */
static SDL_Texture  *tex_ui;                  /* settings panel (128x128) */
static int           photo_w, photo_h;

/* Assets live in the APK on Android and in the repo on the desktop. */
/* Same as asset_load but silent when missing — used for optional assets. */
static void *asset_load_quiet(const char *name, size_t *out_len);

static void *asset_load(const char *name, size_t *out_len) {
    void *p = asset_load_quiet(name, out_len);
    if (!p) SDL_Log("[mote] asset not found: %s", name);
    return p;
}

static void *asset_load_quiet(const char *name, size_t *out_len) {
    const char *tries[4];
    char baked[256], rel[256];
    /* On Android these are all one directory; on the desktop the baked APK copy
     * is tried first so a dev build shows exactly what ships, falling back to
     * the full-resolution source when it has not been baked. */
    snprintf(baked, sizeof baked, "android/assets/%s", name);
    snprintf(rel, sizeof rel, "studio/assets/%s", name);
    tries[0] = name; tries[1] = baked; tries[2] = rel; tries[3] = NULL;
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

/* Paint an 8-bit mask into an RGBA buffer's alpha channel. The APK's chassis is
 * a JPEG (a photograph; PNG was costing 3.6 MB for it) plus a separate mask,
 * because JPEG has no alpha of its own — see android/tools/gen_chassis.py. */
static void apply_alpha(unsigned char *px, int w, int h, const char *mask_name) {
    size_t len = 0;
    void *raw = asset_load_quiet(mask_name, &len);
    if (!raw) return;
    int mw, mh, mn;
    unsigned char *m = stbi_load_from_memory(raw, (int)len, &mw, &mh, &mn, 1);
    SDL_free(raw);
    if (!m) return;
    if (mw == w && mh == h)
        for (int i = 0; i < w * h; i++) px[i * 4 + 3] = m[i];
    else
        SDL_Log("[mote] %s: %dx%d does not match the photo's %dx%d", mask_name, mw, mh, w, h);
    stbi_image_free(m);
}

/* Decode an image asset into a texture, halving it while it exceeds the
 * renderer's texture limit (a 2872px chassis outgrows some older GPUs and every
 * software renderer). ow/oh report the ORIGINAL image size, not the texture's:
 * the texture is always drawn into a scaled dest rect, and screen.cfg's
 * calibration is in photo pixels, so layout must stay in those units.
 *
 * `name` may be a JPEG; if it is, an alpha mask named <stem>_a.png is applied
 * over it when one exists. */
static SDL_Texture *load_png_ex(const char *name, int *ow, int *oh, int quiet) {
    size_t len = 0;
    void *raw = quiet ? asset_load_quiet(name, &len) : asset_load(name, &len);
    if (!raw) return NULL;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(raw, (int)len, &w, &h, &n, 4);
    SDL_free(raw);
    if (!px) { SDL_Log("[mote] %s: %s", name, stbi_failure_reason()); return NULL; }
    { const char *dot = strrchr(name, '.');
      if (dot && (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg"))) {
          char mask[256];
          snprintf(mask, sizeof mask, "%.*s_a.png", (int)(dot - name), name);
          apply_alpha(px, w, h, mask);
      } }
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
static SDL_Texture *load_png(const char *n, int *w, int *h)       { return load_png_ex(n, w, h, 0); }
static SDL_Texture *load_png_quiet(const char *n, int *w, int *h) { return load_png_ex(n, w, h, 1); }

static void load_chassis(void) {
    int w = 0, h = 0;
    SDL_Texture *t = load_png_quiet("thumby_color.jpg", &w, &h);
    if (!t) t = load_png("thumby_color.png", &w, &h);
    if (!t) return;
    if (tex_photo) SDL_DestroyTexture(tex_photo);
    tex_photo = t; photo_w = w; photo_h = h;
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
enum { SH_TOP = 0, SH_SHELL = 1 };   /* shoulder placement */
static struct {
    int  layout;        /* LAY_* */
    int  shoulder;      /* SH_*  */
    int  clear;         /* unused since the see-through photo was dropped; the
                         * field stays so an older shell.cfg still parses */
    int  haptics;
    char relay[80];
} cfg = { LAY_CHASSIS, SH_SHELL, 0, 1, "" };

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
        else if (!strcmp(ln, "shoulder")) cfg.shoulder = atoi(v);
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
    fprintf(f, "layout=%d\nshoulder=%d\nclear=%d\nhaptics=%d\nrelay=%s\n",
            cfg.layout, cfg.shoulder, cfg.clear, cfg.haptics, cfg.relay);
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
/* One reusable transfer array, kept as a global ref: the splice loop runs every
 * few ms and a NewByteArray per call would churn the JNI heap for nothing. */
#define USB_XFER 4096
static jbyteArray s_usb_arr;
static jbyteArray usb_arr(JNIEnv *env) {
    if (!s_usb_arr) {
        jbyteArray a = (*env)->NewByteArray(env, USB_XFER);
        if (!a) return NULL;
        s_usb_arr = (jbyteArray)(*env)->NewGlobalRef(env, a);
        (*env)->DeleteLocalRef(env, a);
    }
    return s_usb_arr;
}
static int usb_read(void *buf, int max, int timeout_ms) {
    if (!s_usb_cls) return -1;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_usb_cls, "moteRead", "([BII)I");
    jbyteArray arr = usb_arr(env);
    if (!m || !arr) { (*env)->ExceptionClear(env); return -1; }
    if (max > USB_XFER) max = USB_XFER;
    jint n = (*env)->CallStaticIntMethod(env, s_usb_cls, m, arr, (jint)max, (jint)timeout_ms);
    if (n > 0) {
        if (n > max) n = max;
        (*env)->GetByteArrayRegion(env, arr, 0, n, (jbyte *)buf);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return (int)n;
}
static int usb_write(const void *buf, int len) {
    if (!s_usb_cls) return -1;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_usb_cls, "moteWrite", "([BI)I");
    jbyteArray arr = usb_arr(env);
    if (!m || !arr) { (*env)->ExceptionClear(env); return -1; }
    if (len > USB_XFER) len = USB_XFER;
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)buf);
    jint n = (*env)->CallStaticIntMethod(env, s_usb_cls, m, arr, (jint)len);
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

/* Shoulder bumpers: centre, size and tilt in window pixels. Both the hit-test
 * and the draw read these, so they cannot disagree. */
typedef struct { float cx, cy, w, h, deg; } Bumper;
static Bumper s_bump[2];                /* 0 = LB, 1 = RB */

static void shoulder_layout(void);

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

/* Where the two bumpers sit. Hit-test and draw both read s_pad, so they cannot
 * disagree. SHELL mode is anchored to the chassis (it must move with the photo);
 * TOP mode is anchored to the window. */
static void shoulder_layout(void) {
    int mind = s_ow < s_oh ? s_ow : s_oh;
    if (cfg.shoulder == SH_SHELL && s_dw > 0) {
        /* Straddle the shell's own top edges, at the angles those edges run at. */
        float w = 0.195f * s_dw, h = 0.075f * s_dh;
        /* Pulled a little inboard of the fitted midpoint, because the shell's
         * corner curves away from the straight line the angle was fitted to.
         * Both sit at the SAME height — the right one's, lifted slightly — so the
         * pair reads level even though each follows its own edge's tilt. */
        const float sy = 0.088f;   /* half a button higher than the shell fit */
        const float sx = 0.130f;
        /* Mirror RB from LB in float about the chassis centre, rather than
         * evaluating BX(1-sx): two integer truncations left the pair four pixels
         * out of true, which is small but is exactly the kind of thing that
         * makes a symmetric pair look wrong. */
        float lbx = (float)s_dx + sx * (float)s_dw;
        float rbx = 2.0f * (float)s_dx + (float)s_dw - lbx;
        s_bump[0] = (Bumper){ lbx, (float)BY(sy), w, h, SHELL_LB_DEG };
        s_bump[1] = (Bumper){ rbx, (float)BY(sy), w, h, SHELL_RB_DEG };
    } else {
        float w = SH_W * mind, h = SH_H * mind;
        float ex = SH_EDGE * mind + w * 0.5f, y = SH_LIFT * mind + h * 0.5f;
        s_bump[0] = (Bumper){ ex, y, w, h, 0.0f };
        s_bump[1] = (Bumper){ s_ow - ex, y, w, h, 0.0f };
    }
}

/* Point in a tilted pad, with a little slop for a thumb landing on the edge. */
static int in_pad(const Bumper *b, int x, int y) {
    float a = -b->deg * 3.14159265f / 180.0f;
    float dx = x - b->cx, dy = y - b->cy;
    float rx = dx * cosf(a) - dy * sinf(a);
    float ry = dx * sinf(a) + dy * cosf(a);
    return fabsf(rx) <= b->w * 0.60f && fabsf(ry) <= b->h * 0.70f;
}

/* Which button is under a window-space point (-1 = none). Touch slop is generous:
 * a thumb landing near an edge should still count. The chassis buttons are tested
 * first, so where a window-anchored shoulder pad happens to reach across MENU's
 * slop the button you can actually see wins. */
static int hit_button(int mx, int my) {
    if (s_dw <= 0)
        return in_pad(&s_bump[0], mx, my) ? EB_LB
             : in_pad(&s_bump[1], mx, my) ? EB_RB : -1;
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
    if (in_pad(&s_bump[0], mx, my)) return EB_LB;
    if (in_pad(&s_bump[1], mx, my)) return EB_RB;
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

/* Rasterise a label with the OS's own UI font, so the shell needs no second font:
 * draw white-on-black into a scratch RGB565 buffer and read the luminance back as
 * coverage (the font is anti-aliased). Returns a malloc'd 8-bit alpha bitmap. */
static uint8_t *text_alpha(const char *s, int *out_w, int *out_h) {
    int w = mote_ui_text_w(s) + 2, h = mote_ui_text_h() + 2;
    if (w < 2 || h < 2 || h > MOTE_FB_H) return NULL;
    uint8_t *a = calloc((size_t)w * h, 1);
    if (!a) return NULL;
    /* The engine's text routine only ever draws into a 128-wide framebuffer, and
     * these labels are wider than the handheld's entire screen — so rasterise a
     * glyph at a time into the scratch and stitch. The font has no kerning (the
     * width is a plain sum of advances), so the seams land exactly where
     * mote_ui_text_w says they do. MARGIN covers glyphs that lean outside their
     * own advance. */
    enum { MARGIN = 8 };
    static uint16_t buf[MOTE_FB_W * MOTE_FB_H];
    char one[2] = { 0, 0 };
    int pen = 1;
    for (const char *p = s; *p; p++) {
        one[0] = *p;
        int adv = mote_ui_text_w(one);
        if (*p != ' ') {
            memset(buf, 0, (size_t)MOTE_FB_W * h * sizeof(uint16_t));
            mote_ui_text(buf, one, MARGIN, 1, 0xFFFF);
            for (int y = 0; y < h; y++) {
                const uint16_t *src = buf + y * MOTE_FB_W;
                for (int x = 0; x < adv + 2 * MARGIN && x < MOTE_FB_W; x++) {
                    int dx = pen - MARGIN + x;
                    if (dx < 0 || dx >= w) continue;
                    uint8_t v = (uint8_t)(((src[x] >> 11) & 0x1F) << 3);
                    if (v > a[y * w + dx]) a[y * w + dx] = v;
                }
            }
        }
        pen += adv;
    }
    *out_w = w; *out_h = h;
    return a;
}

static SDL_Texture *label_tex(const char *s, int *out_w, int *out_h) {
    int w = 0, h = 0;
    uint8_t *a = text_alpha(s, &w, &h);
    if (!a) return NULL;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) { free(a); return NULL; }
    for (int i = 0; i < w * h; i++) px[i] = ((uint32_t)a[i] << 24) | 0x00FFFFFFu;
    free(a);
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

/* ====================================================================== *
 *  Shoulder bumpers
 *
 *  The shoulders are the one pair with nothing to aim at on a front-on photo:
 *  on the real shell they are on the back top edge. So they are drawn — as
 *  bumpers, not rectangles: a rounded body with a lit top edge and a shaded
 *  underside, the way a moulded button catches light, with the label baked in
 *  so one rotated blit draws the whole thing.
 *
 *  Two placements (SHOULDER in the settings panel):
 *    TOP    the window's top corners, level, where index fingers rest when the
 *           phone is held like a gamepad
 *    SHELL  the shell's own shoulder positions, tilted to the angles measured
 *           off the product photo's top edges (-23.2 and +17.4 degrees), so they
 *           sit along the chassis exactly where the hardware's buttons are
 * ====================================================================== */
/* Draw a rounded bumper into an ARGB buffer: gradient body, lit top bevel,
 * shaded underside, and the label centred in it. `lit` is the pressed state. */
static SDL_Texture *gen_bumper(const char *label, int lit) {
    enum { W = 160, H = 56 };
    const float rad = 18.0f;
    uint32_t *px = calloc(W * H, 4);
    if (!px) return NULL;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            /* rounded-rect coverage, anti-aliased off the corner distance */
            float dx = fabsf(x + 0.5f - W * 0.5f) - (W * 0.5f - rad);
            float dy = fabsf(y + 0.5f - H * 0.5f) - (H * 0.5f - rad);
            float d = (dx > 0 && dy > 0) ? sqrtf(dx * dx + dy * dy) - rad
                                         : fmaxf(dx, dy) - rad;
            float cov = 1.0f - (d + 1.0f);            /* ~1px feather */
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            float t = (float)y / (float)(H - 1);      /* 0 top .. 1 bottom */
            float shade = 1.0f - 0.45f * t;           /* body falls off downwards */
            int r, g, b;
            if (lit) { r = (int)(120 * shade + 60); g = (int)(190 * shade + 55); b = (int)(240 * shade + 15); }
            else     { r = (int)( 54 * shade + 18); g = (int)( 62 * shade + 22); b = (int)( 86 * shade + 30); }

            /* bevel: a bright rim along the top, a dark one under the bottom */
            float edge_top = 1.0f - (float)y / 5.0f;
            float edge_bot = 1.0f - (float)(H - 1 - y) / 4.0f;
            if (edge_top > 0) { r += (int)(150 * edge_top); g += (int)(165 * edge_top); b += (int)(185 * edge_top); }
            if (edge_bot > 0) { r -= (int)(28 * edge_bot);  g -= (int)(28 * edge_bot);  b -= (int)(26 * edge_bot); }
            r = r > 255 ? 255 : r < 0 ? 0 : r;
            g = g > 255 ? 255 : g < 0 ? 0 : g;
            b = b > 255 ? 255 : b < 0 ? 0 : b;

            int a = (int)(cov * (lit ? 236 : 170));
            px[y * W + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                            ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    /* the label, scaled to the bumper and composited over the body */
    int tw = 0, th = 0;
    uint8_t *ta = text_alpha(label, &tw, &th);
    if (ta && tw > 0 && th > 0) {
        int lh = H / 2, lw = tw * lh / th;
        if (lw > W - 24) { lw = W - 24; lh = th * lw / tw; }
        int ox = (W - lw) / 2, oy = (H - lh) / 2;
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++) {
                int a = ta[(y * th / lh) * tw + (x * tw / lw)];
                if (!a) continue;
                uint32_t *d = &px[(oy + y) * W + ox + x];
                uint32_t da = (*d >> 24) & 0xFF;
                uint32_t na = da > (uint32_t)a ? da : (uint32_t)a;
                int mix = a * 255 / 255;
                int dr = (*d >> 16) & 0xFF, dg = (*d >> 8) & 0xFF, db = *d & 0xFF;
                int lr = lit ? 20 : 235, lg = lit ? 30 : 242, lb = lit ? 46 : 255;
                dr = (dr * (255 - mix) + lr * mix) / 255;
                dg = (dg * (255 - mix) + lg * mix) / 255;
                db = (db * (255 - mix) + lb * mix) / 255;
                *d = (na << 24) | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
            }
    }
    free(ta);

    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, W, H);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, W * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(px);
    return t;
}

static SDL_Texture *tex_sh[2][2];   /* [side][pressed] */
/* the docked-handheld banner: re-rendered only when the status text changes */
static SDL_Texture *tex_dock;
static int dock_w, dock_h;
static char s_dock_txt[120];

static void shoulder_pads(void) {
    static const char *L[2] = { "LB", "RB" };
    for (int i = 0; i < 2; i++) {
        int down = s_down[i == 0 ? EB_LB : EB_RB];
        if (!tex_sh[i][down]) tex_sh[i][down] = gen_bumper(L[i], down);
        SDL_Texture *t = tex_sh[i][down];
        if (!t) continue;
        const Bumper *b = &s_bump[i];
        /* Round rather than truncate: truncation biases left/up, which breaks the
         * mirror between the two pads. */
        SDL_Rect d = { (int)floorf(b->cx - b->w * 0.5f + 0.5f),
                       (int)floorf(b->cy - b->h * 0.5f + 0.5f),
                       (int)(b->w + 0.5f), (int)(b->h + 0.5f) };
        /* A drop shadow first, so a bumper reads as sitting above the shell
         * rather than painted onto it. */
        SDL_SetTextureColorMod(t, 0, 0, 0);
        SDL_SetTextureAlphaMod(t, 70);
        SDL_Rect sh = { d.x, d.y + (int)(b->h * 0.10f), d.w, d.h };
        SDL_RenderCopyEx(ren, t, NULL, &sh, b->deg, NULL, SDL_FLIP_NONE);
        SDL_SetTextureColorMod(t, 255, 255, 255);
        /* Translucent enough to read the shell through, opaque enough to press
         * with confidence — and brighter while held, so the press still shows. */
        SDL_SetTextureAlphaMod(t, down ? 235 : 170);
        SDL_RenderCopyEx(ren, t, NULL, &d, b->deg, NULL, SDL_FLIP_NONE);
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
 *  Settings sheet
 *
 *  This is the one screen in the app that is NOT the handheld, and the first
 *  cut treated it like one: drawn into a 128x128 buffer with the OS list widget,
 *  which left rows a few millimetres tall, more of them than fitted, and no way
 *  to reach the ones below the fold — a tap selects a row rather than scrolling
 *  it. So it is drawn natively here instead, at the phone's own resolution, with
 *  rows sized for a finger and a line under each one saying what it does. It
 *  still uses the engine's UI font, so it still looks like Mote.
 *
 *  Everything fits without scrolling on any normal screen; the drag handling
 *  below exists only so a very short window cannot trap a row off-screen.
 * ====================================================================== */
static const char *const PERF_LEVEL[4] = { "OFF", "FPS", "MINI", "FULL" };

enum { ROW_LAYOUT = 0, ROW_SHOULDER, ROW_HAPTIC, ROW_FPS, ROW_RELAY,
       ROW_BACK, ROW_CLOSE, ROW_MAX };

static int  s_settings_open, s_sel;
static int  s_editing_relay;
static char s_edit[80];
static int  s_scroll, s_scroll_max, s_drag_active, s_drag_y0, s_drag_s0, s_dragged;
static SDL_Rect s_row_rect[ROW_MAX];
static int      s_row_id[ROW_MAX];
static int      s_row_slots;
static SDL_Rect s_gear_rect, s_ed_ok, s_ed_cancel;
static int      s_gear_armed;
static int in_gear(int x, int y) {
    return x >= s_gear_rect.x && x < s_gear_rect.x + s_gear_rect.w &&
           y >= s_gear_rect.y && y < s_gear_rect.y + s_gear_rect.h;
}

/* ---- cached text textures ------------------------------------------------
 * Rasterising a string through the engine font is cheap, but making an SDL
 * texture for every label every frame is not, so they are kept by content. */
#define TXT_CACHE 32
static struct { char key[96]; SDL_Texture *t; int w, h; unsigned use; } s_txt[TXT_CACHE];
static unsigned s_txt_clock;

static SDL_Texture *txt_get(const char *s, int *w, int *h) {
    if (!s || !s[0]) return NULL;
    int free_i = -1, oldest = 0;
    for (int i = 0; i < TXT_CACHE; i++) {
        if (s_txt[i].t && !strcmp(s_txt[i].key, s)) {
            s_txt[i].use = ++s_txt_clock;
            *w = s_txt[i].w; *h = s_txt[i].h;
            return s_txt[i].t;
        }
        if (!s_txt[i].t && free_i < 0) free_i = i;
        if (s_txt[i].use < s_txt[oldest].use) oldest = i;
    }
    int i = free_i >= 0 ? free_i : oldest;
    if (s_txt[i].t) { SDL_DestroyTexture(s_txt[i].t); s_txt[i].t = NULL; }
    int tw = 0, th = 0;
    uint8_t *a = text_alpha(s, &tw, &th);
    if (!a) return NULL;
    uint32_t *px = malloc((size_t)tw * th * 4);
    if (!px) { free(a); return NULL; }
    for (int k = 0; k < tw * th; k++) px[k] = ((uint32_t)a[k] << 24) | 0x00FFFFFFu;
    free(a);
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, tw, th);
    free(px ? (SDL_UpdateTexture(t, NULL, px, tw * 4), px) : px);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    snprintf(s_txt[i].key, sizeof s_txt[i].key, "%s", s);
    s_txt[i].t = t; s_txt[i].w = tw; s_txt[i].h = th; s_txt[i].use = ++s_txt_clock;
    *w = tw; *h = th;
    return t;
}
/* Freed on a renderer reset; the cache refills itself on the next frame. */
static void txt_free(void) {
    for (int i = 0; i < TXT_CACHE; i++)
        if (s_txt[i].t) { SDL_DestroyTexture(s_txt[i].t); s_txt[i].t = NULL; s_txt[i].key[0] = 0; }
}
enum { TXT_L = 0, TXT_R, TXT_C };
static int txt_draw(const char *s, int x, int y, int px_h, int align,
                    Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    int tw = 0, th = 0;
    SDL_Texture *t = txt_get(s, &tw, &th);
    if (!t || th <= 0) return 0;
    int w = tw * px_h / th;
    SDL_Rect d = { align == TXT_R ? x - w : align == TXT_C ? x - w / 2 : x, y, w, px_h };
    SDL_SetTextureColorMod(t, r, g, b);
    SDL_SetTextureAlphaMod(t, a);
    SDL_RenderCopy(ren, t, NULL, &d);
    return w;
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

/* Which rows exist right now (BACK TO GAMES is conditional). */
static int settings_ids(int *ids) {
    int n = 0;
    ids[n++] = ROW_LAYOUT;
    ids[n++] = ROW_SHOULDER;
    ids[n++] = ROW_HAPTIC;
    ids[n++] = ROW_FPS;
    ids[n++] = ROW_RELAY;
    if (mote_shell_in_game()) ids[n++] = ROW_BACK;
    ids[n++] = ROW_CLOSE;
    return n;
}

/* name, current value, and the line that says what the row is for. A setting
 * nobody can explain to themselves is a setting nobody should have to guess at
 * — which is what the bare "RELAY" row was. */
static int is_default_relay(const char *r) {
    return !r || !r[0] || !strcmp(r, MOTE_ANDROID_RELAY_DEFAULT);
}

static void row_text(int id, char *name, char *val, char *hint, int cap) {
    val[0] = 0;
    switch (id) {
    case ROW_LAYOUT:
        snprintf(name, cap, "Screen");
        snprintf(val, cap, "%s", cfg.layout == LAY_CHASSIS ? "Crisp" : "Bigger");
        snprintf(hint, cap, "Pixel-perfect, or fill more of the phone");
        break;
    case ROW_SHOULDER:
        snprintf(name, cap, "LB / RB buttons");
        snprintf(val, cap, "%s", cfg.shoulder == SH_TOP ? "Top corners" : "On the shell");
        snprintf(hint, cap, "Where the shoulder buttons sit");
        break;
    case ROW_HAPTIC:
        snprintf(name, cap, "Vibration");
        snprintf(val, cap, "%s", cfg.haptics ? "On" : "Off");
        snprintf(hint, cap, "Buzz on button presses, and rumble in games");
        break;
    case ROW_FPS:
        snprintf(name, cap, "Frame rate");
        snprintf(val, cap, "%d fps  %s", s_fps, PERF_LEVEL[mote_perf_level() & 3]);
        snprintf(hint, cap, "Tap to show the performance overlay on screen");
        break;
    case ROW_RELAY: {
        /* "Relay" means nothing to a player, so the row says what it is FOR and
         * the value says "Default" rather than an IP address they never chose. */
        const char *r = mote_shell_get_relay();
        snprintf(name, cap, "Internet match server");
        snprintf(val, cap, "%s", is_default_relay(r) ? "Default" : r);
        snprintf(hint, cap, "Nothing to set up. LAN and USB play never use it.");
        break;
    }
    case ROW_BACK:
        snprintf(name, cap, "Back to games");
        snprintf(hint, cap, "Leave this game and return to the list");
        break;
    default:
        snprintf(name, cap, "Close");
        snprintf(hint, cap, "Or tap anywhere outside this panel");
        break;
    }
}

static void relay_commit(int keep) {
    if (keep) {
        snprintf(cfg.relay, sizeof cfg.relay, "%s", s_edit);
        mote_shell_set_relay(cfg.relay);
        cfg_save();
    }
    s_editing_relay = 0;
    SDL_StopTextInput();
}

static void settings_activate(int id) {
    switch (id) {
    case ROW_LAYOUT:   cfg.layout = !cfg.layout; layout(); break;
    case ROW_SHOULDER: cfg.shoulder = !cfg.shoulder; layout(); break;
    case ROW_HAPTIC:   cfg.haptics = !cfg.haptics; break;
    case ROW_FPS:      mote_perf_toggle(); return;   /* cycles the on-LCD overlay */
    case ROW_RELAY:
        s_editing_relay = 1;
        snprintf(s_edit, sizeof s_edit, "%s", mote_shell_get_relay());
        SDL_StartTextInput();
        return;
    case ROW_BACK:
        mote_shell_request_exit_game();
        s_settings_open = 0;
        return;
    default:           s_settings_open = 0; break;
    }
    cfg_save();
}

/* ---- the sheet ---------------------------------------------------------- */
static void card(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    box(x, y, w, h, r, g, b, a);
}

static void settings_draw(void) {
    int mind = s_ow < s_oh ? s_ow : s_oh;
    int cw = (int)(s_ow * 0.66f); if (cw > mind * 5 / 3) cw = mind * 5 / 3;
    if (cw < mind) cw = mind < s_ow ? mind : s_ow;
    int cx = (s_ow - cw) / 2;
    int pad = mind / 36;
    int title_h = mind / 12;

    box(0, 0, s_ow, s_oh, 0, 0, 0, 205);                       /* backdrop */

    if (s_editing_relay) {
        /* Keyboard is about to cover the lower half, so this sheet lives at the
         * top and its two buttons are large and unmissable. */
        int h = title_h + mind * 2 / 5;
        int y = pad;
        card(cx, y, cw, h, 16, 19, 28, 245);
        box(cx, y, cw, 3, 96, 176, 255, 255);
        txt_draw("Internet match server", cx + pad, y + pad, mind / 20, TXT_L, 255, 206, 92, 255);
        char v[96]; snprintf(v, sizeof v, "%.40s_", s_edit);
        txt_draw(v, cx + pad, y + pad + mind / 20 + pad / 2, mind / 17, TXT_L, 235, 240, 250, 255);
        static const char *const why[] = {
            "Games you play over the Internet meet here.",
            "You do not need to change this - the address",
            "above is the public one, and it is already set.",
            "LAN games and a docked handheld never use it.",
        };
        int hy = y + pad + mind / 20 + mind / 17 + pad, lh = mind / 26;
        for (int i = 0; i < 4; i++)
            txt_draw(why[i], cx + pad, hy + i * lh, mind / 32, TXT_L, 130, 142, 168, 255);
        int bh = mind / 9, by = y + h - bh - pad, bw = (cw - 3 * pad) / 2;
        s_ed_ok     = (SDL_Rect){ cx + pad, by, bw, bh };
        s_ed_cancel = (SDL_Rect){ cx + cw - pad - bw, by, bw, bh };
        box(s_ed_ok.x, s_ed_ok.y, bw, bh, 40, 96, 150, 255);
        box_outline(s_ed_ok.x, s_ed_ok.y, bw, bh, 2, 120, 200, 255, 255);
        txt_draw("OK", s_ed_ok.x + bw / 2, by + (bh - mind / 22) / 2, mind / 22, TXT_C, 255, 255, 255, 255);
        box(s_ed_cancel.x, s_ed_cancel.y, bw, bh, 40, 44, 58, 255);
        box_outline(s_ed_cancel.x, s_ed_cancel.y, bw, bh, 2, 110, 118, 140, 255);
        txt_draw("Cancel", s_ed_cancel.x + bw / 2, by + (bh - mind / 22) / 2, mind / 22, TXT_C,
                 210, 216, 230, 255);
        s_row_slots = 0;
        return;
    }

    int ids[ROW_MAX];
    int n = settings_ids(ids);
    int top = pad, ch = s_oh - 2 * pad;
    int list_y = top + title_h;
    int view_h = ch - title_h;
    /* Rows are sized to make the whole sheet fit — a settings list of eight
     * items should never need scrolling, and the first cut of this screen was
     * unusable precisely because it did. The clamps keep them a finger tall
     * whatever the window shape; if a window is so short that even the floor
     * overflows, the drag below picks up the slack. */
    int row_h = n > 0 ? view_h / n : view_h;
    if (row_h > mind / 7)  row_h = mind / 7;
    if (row_h < mind / 13) row_h = mind / 13;
    int content = n * row_h;
    s_scroll_max = content > view_h ? content - view_h : 0;
    if (s_scroll > s_scroll_max) s_scroll = s_scroll_max;
    if (s_scroll < 0) s_scroll = 0;

    card(cx, top, cw, ch, 16, 19, 28, 242);
    box(cx, top, cw, 3, 96, 176, 255, 255);
    txt_draw("MOTE", cx + pad, top + (title_h - mind / 20) / 2, mind / 20, TXT_L, 255, 206, 92, 255);
    txt_draw("settings", cx + pad + mind / 6, top + (title_h - mind / 26) / 2 + mind / 60,
             mind / 26, TXT_L, 130, 142, 168, 255);

    s_row_slots = 0;
    for (int i = 0; i < n; i++) {
        int ry = list_y + i * row_h - s_scroll;
        if (ry + row_h < list_y || ry > top + ch) continue;      /* clipped away */
        char name[96], val[96], hint[96];
        row_text(ids[i], name, val, hint, 96);

        int sel = (i == s_sel);
        if (sel) box(cx + pad / 2, ry + 2, cw - pad, row_h - 4, 38, 62, 104, 255);
        if (ids[i] == ROW_BACK || ids[i] == ROW_CLOSE)
            box(cx + pad / 2, ry + 2, 4, row_h - 4, 120, 200, 255, 200);

        int nh = mind / 24, hh = mind / 36;
        txt_draw(name, cx + pad, ry + row_h / 2 - nh, nh, TXT_L, 236, 240, 250, 255);
        txt_draw(hint, cx + pad, ry + row_h / 2 + nh / 5, hh, TXT_L, 128, 140, 166, 255);
        if (val[0])
            txt_draw(val, cx + cw - pad, ry + row_h / 2 - nh / 2, nh, TXT_R, 150, 210, 255, 255);
        /* the whole row is the target */
        s_row_rect[s_row_slots] = (SDL_Rect){ cx, ry, cw, row_h };
        s_row_id[s_row_slots] = ids[i];
        s_row_slots++;
        if (i < n - 1) box(cx + pad, ry + row_h - 1, cw - 2 * pad, 1, 44, 50, 66, 255);
    }
}

/* ---- panel touch: press / drag / release --------------------------------
 * Acting on the release rather than the press is what makes a drag possible at
 * all, and it is also what a touch UI is expected to do: a finger that lands on
 * the wrong row can slide off it without setting anything. */
static void panel_press(int mx, int my) {
    (void)mx;
    s_drag_active = 1; s_drag_y0 = my; s_drag_s0 = s_scroll; s_dragged = 0;
}
static void panel_drag(int my) {
    if (!s_drag_active || !s_scroll_max) return;   /* nothing to scroll: it stays a tap */
    int mind = s_ow < s_oh ? s_ow : s_oh;
    int slop = mind / 40;                          /* ~7mm of finger roll, not 12 raw px */
    if (slop < 6) slop = 6;
    int dy = my - s_drag_y0;
    if (dy > slop || dy < -slop) s_dragged = 1;
    s_scroll = s_drag_s0 - dy;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_scroll_max) s_scroll = s_scroll_max;
}
static void panel_release(int mx, int my) {
    int was_drag = s_dragged;
    s_drag_active = 0; s_dragged = 0;
    if (was_drag) return;

    if (s_editing_relay) {
        if (mx >= s_ed_ok.x && mx < s_ed_ok.x + s_ed_ok.w &&
            my >= s_ed_ok.y && my < s_ed_ok.y + s_ed_ok.h) { tap_haptic(); relay_commit(1); return; }
        if (mx >= s_ed_cancel.x && mx < s_ed_cancel.x + s_ed_cancel.w &&
            my >= s_ed_cancel.y && my < s_ed_cancel.y + s_ed_cancel.h) { tap_haptic(); relay_commit(0); return; }
        return;
    }
    for (int i = 0; i < s_row_slots; i++) {
        const SDL_Rect *r = &s_row_rect[i];
        if (mx < r->x || mx >= r->x + r->w || my < r->y || my >= r->y + r->h) continue;
        tap_haptic();
        s_sel = i;
        settings_activate(s_row_id[i]);
        return;
    }
    s_settings_open = 0;                 /* tapped off the sheet */
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

/* MOTE_SHELL_TAP="gear@30" or "640,120@30" — inject one press+release at a
 * window pixel (or the settings gear) on the given frame. The panel captures
 * used MOTE_SHELL_PANEL, which forces the sheet open and so never exercised the
 * gear or the release that follows it — which is precisely where the sheet grew
 * a bug that shut it the instant it opened. */
static int s_tap_frame = -1, s_tap_x, s_tap_y, s_tap_gear, s_tap_done;
static void tap_parse(void) {
    const char *e = SDL_getenv("MOTE_SHELL_TAP");
    if (!e) return;
    const char *at = strchr(e, '@');
    if (!at) return;
    s_tap_frame = atoi(at + 1);
    if (!strncmp(e, "gear", 4)) s_tap_gear = 1;
    else { s_tap_x = atoi(e); const char *c = strchr(e, ','); s_tap_y = c ? atoi(c + 1) : 0; }
}
static void tap_pump(void) {
    if (s_tap_done || s_tap_frame < 0 || s_frame_no < s_tap_frame) return;
    s_tap_done = 1;
    int x = s_tap_gear ? s_gear_rect.x + s_gear_rect.w / 2 : s_tap_x;
    int y = s_tap_gear ? s_gear_rect.y + s_gear_rect.h / 2 : s_tap_y;
    SDL_Event d = {0}, u = {0};
    d.type = SDL_MOUSEBUTTONDOWN; d.button.button = SDL_BUTTON_LEFT; d.button.x = x; d.button.y = y;
    u.type = SDL_MOUSEBUTTONUP;   u.button.button = SDL_BUTTON_LEFT; u.button.x = x; u.button.y = y;
    SDL_PushEvent(&d); SDL_PushEvent(&u);
    SDL_Log("[mote] tap %d,%d at frame %d", x, y, s_frame_no);
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
    if (s_nscript < 0) { script_parse(); tap_parse(); }
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
    /* MOTE_SHELL_SIZE=2340x1080 — check a layout at a real phone's proportions
     * (a 16:9 desktop window is a poor stand-in for a 19.5:9 handset). */
    int dw = 1280, dh = 720;
    { const char *sz = SDL_getenv("MOTE_SHELL_SIZE");
      if (sz) { int a, b; if (sscanf(sz, "%dx%d", &a, &b) == 2 && a > 200 && b > 200) { dw = a; dh = b; } } }
    win = SDL_CreateWindow("Mote (Android shell)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           dw, dh, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
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
    load_chassis();
    tex_frame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                 MOTE_FB_W, MOTE_FB_H);
    SDL_SetTextureScaleMode(tex_frame, SDL_ScaleModeNearest);
    layout();

    s_shot_path = SDL_getenv("MOTE_SHELL_SHOT");
    if (SDL_getenv("MOTE_SHELL_SHOT_FRAME")) s_shot_frame = atoi(SDL_getenv("MOTE_SHELL_SHOT_FRAME"));
    if (SDL_getenv("MOTE_SHELL_PANEL")) {                      /* capture the panel */
        s_settings_open = 1;
        if (!strcmp(SDL_getenv("MOTE_SHELL_PANEL"), "relay")) {
            s_editing_relay = 1;
            snprintf(s_edit, sizeof s_edit, "%s", MOTE_ANDROID_RELAY_DEFAULT);
        }
    }

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
                if (tex_ui)    { SDL_DestroyTexture(tex_ui);    tex_ui = NULL; }
                if (tex_frame) { SDL_DestroyTexture(tex_frame); tex_frame = NULL; }
                for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++)
                    if (tex_sh[i][j]) { SDL_DestroyTexture(tex_sh[i][j]); tex_sh[i][j] = NULL; }
                txt_free();
                load_chassis();
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
                if (s_settings_open) { panel_press(mx, my); break; }
                if (in_gear(mx, my)) { s_gear_armed = 1; tap_haptic(); break; }
                Touch *t = touch_alloc();
                if (!t) break;
                t->active = 1; t->id = ev.tfinger.fingerId;
                t->x = ev.tfinger.x; t->y = ev.tfinger.y;
                t->btn = hit_button(mx, my);
                if (t->btn >= 0) tap_haptic();
                break;
            }
            case SDL_FINGERMOTION: {
                if (s_settings_open) { panel_drag((int)(ev.tfinger.y * s_oh)); break; }
                Touch *t = touch_find(ev.tfinger.fingerId);
                if (!t) break;
                t->x = ev.tfinger.x; t->y = ev.tfinger.y;
                int nb = hit_button((int)(t->x * s_ow), (int)(t->y * s_oh));
                if (nb != t->btn) { t->btn = nb; if (nb >= 0) tap_haptic(); }
                break;
            }
            case SDL_FINGERUP: {
                int ux = (int)(ev.tfinger.x * s_ow), uy = (int)(ev.tfinger.y * s_oh);
                /* The gear opens on release. Opening it on press looked fine and
                 * was not: the sheet was already up by the time the finger
                 * lifted, so its own release landed in panel_release(), hit no
                 * row, and read as a tap outside. The menu shut itself. */
                if (s_gear_armed) {
                    s_gear_armed = 0;
                    if (in_gear(ux, uy)) { s_settings_open = 1; s_sel = 0; s_scroll = 0; }
                    break;
                }
                if (s_settings_open) { panel_release(ux, uy); break; }
                Touch *t = touch_find(ev.tfinger.fingerId);
                if (t) { t->active = 0; t->btn = -1; }
                break;
            }
#ifdef MOTE_SHELL_DESKTOP
            /* Mouse stands in for a finger on the desktop dev build. */
            case SDL_MOUSEBUTTONDOWN: {
                int mx = ev.button.x, my = ev.button.y;
                if (s_settings_open) { panel_press(mx, my); break; }
                if (in_gear(mx, my)) { s_gear_armed = 1; break; }
                Touch *t = touch_alloc();
                if (t) { t->active = 1; t->id = -1 - ev.button.button;
                         t->x = mx / (float)s_ow; t->y = my / (float)s_oh;
                         t->btn = hit_button(mx, my); }
                break;
            }
            case SDL_MOUSEMOTION: {
                if (s_settings_open) { panel_drag(ev.motion.y); break; }
                Touch *t = touch_find(-1 - SDL_BUTTON_LEFT);
                if (t) { t->x = ev.motion.x / (float)s_ow; t->y = ev.motion.y / (float)s_oh;
                         t->btn = hit_button(ev.motion.x, ev.motion.y); }
                break;
            }
            case SDL_MOUSEBUTTONUP: {
                if (s_gear_armed) {
                    s_gear_armed = 0;
                    if (in_gear(ev.button.x, ev.button.y)) { s_settings_open = 1; s_sel = 0; s_scroll = 0; }
                    break;
                }
                if (s_settings_open) { panel_release(ev.button.x, ev.button.y); break; }
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
            int n = settings_ids(ids);
            if (s_sel >= n) s_sel = n - 1;
            if (s_sel < 0) s_sel = 0;
            if (s_editing_relay) {
                if (s_down[EB_A] && !prev[EB_A]) relay_commit(1);
                if (s_down[EB_B] && !prev[EB_B]) relay_commit(0);
            } else {
                if (s_down[EB_DOWN] && !prev[EB_DOWN]) s_sel = (s_sel + 1) % n;
                if (s_down[EB_UP]   && !prev[EB_UP])   s_sel = (s_sel + n - 1) % n;
                if (s_down[EB_A]    && !prev[EB_A])    settings_activate(ids[s_sel]);
                if (s_down[EB_B]    && !prev[EB_B])    s_settings_open = 0;
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

        SDL_Texture *chassis = tex_photo;
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

        tap_pump();                       /* scripted tap, before the frame counter moves on */
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
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++)
        if (tex_sh[i][j]) SDL_DestroyTexture(tex_sh[i][j]);
    txt_free();
    if (tex_ui) SDL_DestroyTexture(tex_ui);
    if (tex_frame) SDL_DestroyTexture(tex_frame);
    if (tex_photo) SDL_DestroyTexture(tex_photo);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
