/*
 * Mote — Android platform backend (mote_platform.h on SDL2 + bionic).
 *
 * "Embedded" like the Studio backend rather than windowed like the host one: the
 * engine renders into its framebuffer on a worker thread and publishes the
 * finished frame here; the SDL main thread draws it inside the Thumby Color
 * chassis. Buttons arrive from the shell's touch/gamepad reader.
 *
 * Everything else is a straight bionic implementation: monotonic clock, SDL
 * audio pull, saves/kv as files under the app's private storage, rumble through
 * a Java Vibrator callback. The 2-player link lives in mote_link_android.c.
 *
 * Also compiles for the desktop (-DMOTE_SHELL_DESKTOP) so the shell UI can be
 * run and screenshotted without a phone; only logging and the storage default
 * differ.
 *
 * And it is the backend for the VR build too (-DMOTE_VR, platform/vr): a
 * headset is an embedded presentation surface like any other, so everything
 * below — the frame handoff, saves, key-value blobs, the Java hooks — is the
 * same code. Only the audio device differs: the phone pulls through SDL, and
 * the headset has no SDL at all, so it uses AAudio directly. Nothing else here
 * is allowed to know which app it is in.
 */
#include "mote_platform.h"
#include "mote_config.h"
#include "mote_plat_android.h"
#include "mote_audio.h"

#if MOTE_AUDIO_SDL
#include <SDL.h>
#else
#include <aaudio/AAudio.h>
#include "mote_vr_resample.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

/* ---- shell <-> engine handoff ------------------------------------------- */
static MoteButtons  s_btn;                 /* written by the UI thread, read here */
static volatile int s_quit;

/* Double-buffer: the engine renders into the shared launcher fb, then present
 * copies the COMPLETE frame here under a lock; the UI only ever reads this. */
static uint16_t s_display[MOTE_FB_W * MOTE_FB_H];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t   s_seq;
static uint64_t   s_last_present;
static int        s_present_cap = 60;      /* engine fps ceiling (0 = uncapped) */

static volatile int s_paused;              /* app in the background */
static volatile int s_exit_game;           /* panel asked to return to the game list */
static volatile int s_in_game;

void mote_shell_set_buttons(const MoteButtons *b) { if (b) s_btn = *b; }
void mote_shell_request_quit(void) { s_quit = 1; }
void mote_shell_set_present_cap(int fps) { s_present_cap = fps < 0 ? 0 : fps; }

void mote_shell_request_exit_game(void) { if (s_in_game) s_exit_game = 1; }
int  mote_shell_take_exit_game(void) { int v = s_exit_game; s_exit_game = 0; return v; }
void mote_shell_set_in_game(int in_game) { s_in_game = in_game ? 1 : 0; }
int  mote_shell_in_game(void) { return s_in_game; }

void mote_shell_get_frame(uint16_t *out) {
    if (!out) return;
    pthread_mutex_lock(&s_lock);
    memcpy(out, s_display, sizeof s_display);
    pthread_mutex_unlock(&s_lock);
}
uint32_t mote_shell_frame_seq(void) { return s_seq; }

static void publish(const uint16_t *fb) {
    pthread_mutex_lock(&s_lock);
    memcpy(s_display, fb, sizeof s_display);
    s_seq++;
    pthread_mutex_unlock(&s_lock);
    while (s_paused && !s_quit) mote_plat_sleep_us(60000);
    /* Pace the engine so a light game doesn't burn a core (and dt stays sane).
     * Start-to-start pacing, same shape as the Studio backend. */
    if (s_present_cap > 0) {
        uint64_t now = mote_plat_micros();
        uint64_t tgt = s_last_present + (uint64_t)(1000000 / s_present_cap);
        /* Sleep in a loop: a bare nanosleep returns early when a signal lands
         * (SDL's threads do generate them), which quietly let the engine run
         * above the cap. */
        while (s_last_present && now < tgt) {
            struct timespec ts = { 0, (long)((tgt - now) * 1000) };
            nanosleep(&ts, NULL);
            now = mote_plat_micros();
        }
        s_last_present = now;
    }
}

/* ---- audio -------------------------------------------------------------- */
#if MOTE_AUDIO_SDL

static SDL_AudioDeviceID s_audio;
static void audio_cb(void *u, Uint8 *stream, int len) { (void)u;
    mote_audio_render((int16_t *)stream, len / 2);   /* 16-bit mono */
}
static void audio_open(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    SDL_AudioSpec want; memset(&want, 0, sizeof want);
    want.freq = MOTE_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 512; want.callback = audio_cb;
    s_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (s_audio) SDL_PauseAudioDevice(s_audio, 0);
}
static void audio_pause(int paused) { if (s_audio) SDL_PauseAudioDevice(s_audio, paused); }
static void audio_close(void) { if (s_audio) { SDL_CloseAudioDevice(s_audio); s_audio = 0; } }

#else   /* AAudio: the headset build links no SDL */

static AAudioStream    *s_audio;
static int32_t          s_audio_rate = MOTE_AUDIO_RATE;
static MoteVrResampler  s_rs;

static aaudio_data_callback_result_t aa_cb(AAudioStream *st, void *u,
                                           void *data, int32_t frames) {
    (void)st; (void)u;
    if (s_audio_rate == MOTE_AUDIO_RATE) mote_audio_render((int16_t *)data, frames);
    else mote_vr_rs_render(&s_rs, (int16_t *)data, frames, mote_audio_render);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_open(void) {
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, MOTE_AUDIO_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, aa_cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s_audio);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !s_audio) { s_audio = NULL; mote_plat_log("[mote] no audio device"); return; }
    s_audio_rate = AAudioStream_getSampleRate(s_audio);
    if (s_audio_rate <= 0) s_audio_rate = MOTE_AUDIO_RATE;
    mote_vr_rs_init(&s_rs, MOTE_AUDIO_RATE, s_audio_rate);
    { char m[80]; snprintf(m, sizeof m, "[mote] audio %d Hz%s", (int)s_audio_rate,
        s_audio_rate == MOTE_AUDIO_RATE ? "" : " (resampling from 22050)");
      mote_plat_log(m); }
    AAudioStream_requestStart(s_audio);
}
static void audio_pause(int paused) {
    if (!s_audio) return;
    if (paused) AAudioStream_requestPause(s_audio);
    else        AAudioStream_requestStart(s_audio);
}
static void audio_close(void) {
    if (!s_audio) return;
    AAudioStream_requestStop(s_audio);
    AAudioStream_close(s_audio);
    s_audio = NULL;
}

#endif  /* MOTE_AUDIO_SDL */

/* Backgrounded: park the engine inside publish() (and silence audio) instead of
 * letting it keep simulating off-screen. The frame loop clamps dt, so a long park
 * resumes as one short frame rather than a time jump. */
void mote_shell_set_paused(int paused) {
    s_paused = paused ? 1 : 0;
    audio_pause(s_paused);
}

/* ---- storage ------------------------------------------------------------ */
static char s_root[512];
static char s_save_game[40];

void mote_shell_set_storage(const char *dir) {
    if (!dir || !dir[0]) { s_root[0] = 0; return; }
    snprintf(s_root, sizeof s_root, "%s", dir);
    /* strip a trailing slash so path building stays predictable */
    size_t n = strlen(s_root);
    if (n > 1 && s_root[n - 1] == '/') s_root[n - 1] = 0;
    char p[560];
    snprintf(p, sizeof p, "%s/saves", s_root); mkdir(p, 0770);
    snprintf(p, sizeof p, "%s/kv", s_root);    mkdir(p, 0770);
}

/* ---- platform surface --------------------------------------------------- */
int mote_plat_init(const char *title) { (void)title;
    mote_audio_init();
    audio_open();
    return 0;
}

void     mote_plat_present(const uint16_t *fb)       { publish(fb); }
void     mote_plat_present_async(const uint16_t *fb) { publish(fb); }
uint32_t mote_plat_wait_flush(void)                  { return 0; }

/* Single-threaded raster (the phone's single core beats the RP2350's two by a
 * wide margin at 128x128; games' render_band is core-safe but keeping it serial
 * keeps this backend free of cross-thread raster state). */
void mote_plat_render2(uint16_t *fb, MoteBandFn band, uint32_t *c0, uint32_t *c1) {
    uint64_t t0 = mote_plat_micros();
    band(fb, 0, MOTE_FB_H);
    if (c0) *c0 = (uint32_t)(mote_plat_micros() - t0);
    if (c1) *c1 = 0;
}

void mote_plat_buttons(MoteButtons *out) { if (out) *out = s_btn; }

uint64_t mote_plat_micros(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}

void mote_plat_sleep_us(uint32_t us) {
    if (us < 1000) return;
    struct timespec ts = { (time_t)(us / 1000000u), (long)((us % 1000000u) * 1000u) };
    nanosleep(&ts, NULL);
}

bool mote_plat_should_quit(void)    { return s_quit != 0 || s_exit_game != 0; }
int  mote_plat_pending_launch(void) { return -1; }

void mote_plat_log(const char *s) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "mote", "%s", s ? s : "");
#else
    printf("%s\n", s ? s : ""); fflush(stdout);
#endif
}

void mote_plat_shutdown(void) { audio_close(); }

void mote_plat_set_brightness(int pct) { (void)pct; }   /* the OS owns the panel */
/* No cross-process settings store here: the window/phone owns its own brightness
 * and the volume lives in this process only. */
int  mote_plat_settings_load(int *b, int *v) { (void)b; (void)v; return 0; }
void mote_plat_settings_save(int b, int v)   { (void)b; (void)v; }
void mote_plat_set_volume(int pct)     { mote_audio_set_volume(pct / 100.0f); }
void mote_plat_audio_pump(void)        { }              /* SDL pulls via the callback */
void mote_plat_audio_topup(void)       { }
void mote_plat_audio_start(void)       { mote_audio_off(); }

/* ---- hooks the shell owns (Java / desktop equivalents) ------------------ */
static void (*s_rumble_cb)(float, int);
void mote_shell_set_rumble_cb(void (*cb)(float, int)) { s_rumble_cb = cb; }
void mote_plat_rumble(float intensity, int ms) {
    if (s_rumble_cb) s_rumble_cb(intensity, ms);
}

static int (*s_http_cb)(const char *, const char *);
void mote_shell_set_http_cb(int (*cb)(const char *, const char *)) { s_http_cb = cb; }
int  mote_shell_http_get(const char *url, const char *dest) {
    return s_http_cb ? s_http_cb(url, dest) : -1;
}

static const MoteUsbHost *s_usb;
void               mote_shell_set_usb_host(const MoteUsbHost *h) { s_usb = h; }
const MoteUsbHost *mote_shell_usb_host(void) { return s_usb; }

/* ---- per-slot save (files under <storage>/saves) ------------------------ */
#define AND_SAVE_SLOTS 64        /* flash-free: give games the full slot range */

void mote_plat_set_save_game(const char *stem) {
    if (!stem) { s_save_game[0] = 0; return; }
    snprintf(s_save_game, sizeof s_save_game, "%s", stem);
}
int mote_plat_save_slots(void) { return AND_SAVE_SLOTS; }

static void save_path(int slot, char *p, int n) {
    snprintf(p, n, "%s/saves/%s_%d.bin", s_root[0] ? s_root : ".",
             s_save_game[0] ? s_save_game : "game", slot);
}
int mote_plat_save(int slot, const void *data, int len) {
    if (slot < 0 || slot >= AND_SAVE_SLOTS) return 0;
    char p[600]; save_path(slot, p, sizeof p);
    if (len <= 0) { remove(p); return 0; }
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    uint32_t L = (uint32_t)len;
    fwrite(&L, 4, 1, f); fwrite(data, 1, (size_t)len, f); fclose(f);
    return len;
}
int mote_plat_load(int slot, void *data, int max_len) {
    if (slot < 0 || slot >= AND_SAVE_SLOTS) return 0;
    char p[600]; save_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    uint32_t L = 0;
    if (fread(&L, 4, 1, f) != 1) { fclose(f); return 0; }
    if (data && max_len > 0) {
        int c = (int)L < max_len ? (int)L : max_len;
        if (fread(data, 1, (size_t)c, f) != (size_t)c) { /* short file: partial load */ }
    }
    fclose(f);
    return (int)L;
}

/* ---- v38 key-value blobs (files under <storage>/kv) -------------------- */
static void kv_path(const char *key, char *p, int n) {
    snprintf(p, n, "%s/kv/%s__%s", s_root[0] ? s_root : ".",
             s_save_game[0] ? s_save_game : "game", key);
}
int mote_plat_kv_save(const char *key, const void *data, int len) {
    if (!key || !key[0]) return 0;
    char p[700]; kv_path(key, p, sizeof p);
    if (len <= 0) { remove(p); return 0; }
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    size_t w = fwrite(data, 1, (size_t)len, f); fclose(f);
    return (int)w;
}
int mote_plat_kv_load(const char *key, void *data, int max) {
    if (!key || !key[0]) return 0;
    char p[700]; kv_path(key, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (data && max > 0) {
        int c = (int)sz < max ? (int)sz : max;
        if (fread(data, 1, (size_t)c, f) != (size_t)c) { /* truncated: partial load */ }
    }
    fclose(f);
    return (int)sz;
}
void mote_plat_kv_list(const char *prefix, void (*cb)(const char *, void *), void *arg) {
    char dir[560]; snprintf(dir, sizeof dir, "%s/kv", s_root[0] ? s_root : ".");
    DIR *d = opendir(dir); if (!d) return;
    char gp[60]; snprintf(gp, sizeof gp, "%s__", s_save_game[0] ? s_save_game : "game");
    size_t gpl = strlen(gp), pl = prefix ? strlen(prefix) : 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, gp, gpl) != 0) continue;
        const char *key = e->d_name + gpl;
        if (pl == 0 || strncmp(key, prefix, pl) == 0) cb(key, arg);
    }
    closedir(d);
}
