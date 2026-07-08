/*
 * Mote Studio platform backend. Unlike the host backend (which owns an SDL
 * window + reads the keyboard), this is "embedded": the engine renders into the
 * shared framebuffer (mote_launcher_fb) which the Studio reads and draws inside
 * the device shell, and input comes from a bitfield the Studio fills. Audio
 * reuses the same SDL synth path as the host. The engine's mote_os_run loop runs
 * unchanged on a worker thread.
 */
#include "mote_platform.h"
#include "mote_config.h"
#include "mote_plat_studio.h"
#include "../../engine/audio/mote_audio.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <time.h>

static MoteButtons   s_btn;
static volatile int  s_quit;

/* Double-buffer: the engine renders into the shared fb, then present copies the
 * COMPLETE frame here under a lock; the UI reads only this. No more tearing. */
static uint16_t      s_display[MOTE_FB_W * MOTE_FB_H];
static SDL_mutex    *s_lock;
static uint64_t      s_last_present;

void mote_studio_set_buttons(const MoteButtons *b) { s_btn = *b; }
void mote_studio_request_quit(void) { s_quit = 1; }
void mote_studio_reset(void) { s_quit = 0; SDL_memset(&s_btn, 0, sizeof s_btn); }

void mote_studio_get_frame(uint16_t *out) {
    if (!s_lock) { SDL_memset(out, 0, sizeof s_display); return; }
    SDL_LockMutex(s_lock); SDL_memcpy(out, s_display, sizeof s_display); SDL_UnlockMutex(s_lock);
}

static void publish(const uint16_t *fb) {
    if (!s_lock) s_lock = SDL_CreateMutex();
    SDL_LockMutex(s_lock); SDL_memcpy(s_display, fb, sizeof s_display); SDL_UnlockMutex(s_lock);
    /* cap the engine at ~60 fps so it doesn't spin a core (and dt stays sane) */
    uint64_t now = mote_plat_micros(), tgt = s_last_present + 16667;
    if (s_last_present && now < tgt) { struct timespec ts = { 0, (long)((tgt - now) * 1000) };
        nanosleep(&ts, NULL); now = mote_plat_micros(); }
    s_last_present = now;
}

static void audio_cb(void *u, Uint8 *stream, int len) { (void)u;
    mote_audio_render((int16_t *)stream, len / 2);
}

int mote_plat_init(const char *title) { (void)title;
    mote_audio_init();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
        want.freq = MOTE_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 1;
        want.samples = 512; want.callback = audio_cb;
        if (SDL_OpenAudio(&want, NULL) == 0) SDL_PauseAudio(0);
    }
    return 0;
}

void     mote_plat_present(const uint16_t *fb)       { publish(fb); }
void     mote_plat_present_async(const uint16_t *fb) { publish(fb); }
uint32_t mote_plat_wait_flush(void)                  { return 0; }

void mote_plat_render2(uint16_t *fb, MoteBandFn band, uint32_t *c0, uint32_t *c1) {
    band(fb, 0, MOTE_FB_H / 2);
    band(fb, MOTE_FB_H / 2, MOTE_FB_H);
    if (c0) *c0 = 0; if (c1) *c1 = 0;
}

void mote_plat_buttons(MoteButtons *out) { *out = s_btn; }

uint64_t mote_plat_micros(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}

void mote_plat_sleep_us(uint32_t us) { if (us >= 1000) SDL_Delay(us / 1000); }

bool mote_plat_should_quit(void)     { return s_quit != 0; }
int  mote_plat_pending_launch(void)  { return -1; }
/* Optional sink so the Studio routes game/engine log lines into its Console
 * instead of the terminal. main.c sets this to its log ring at startup; when
 * NULL (headless/CLI) we fall back to stdout. */
void (*mote_studio_log_sink)(const char *) = 0;
void mote_plat_log(const char *s) {
    if (mote_studio_log_sink) { char b[256]; snprintf(b, sizeof b, "[game] %s", s); mote_studio_log_sink(b); }
    else { printf("[game] %s\n", s); fflush(stdout); }
}
void mote_plat_shutdown(void)        { }
void mote_plat_set_brightness(int p) { (void)p; }
void mote_plat_set_volume(int p)     { mote_audio_set_volume(p / 100.0f); }
void mote_plat_audio_pump(void)      { }
void mote_plat_audio_start(void)     { mote_audio_off(); }

/* ---- ABI v23: rumble (no motor in the emulator) + per-slot save (files) ---- */
void mote_plat_rumble(float intensity, int ms) { (void)intensity; (void)ms; }
#define STUDIO_SAVE_SLOTS 8
static char s_save_game[40] = "";
void mote_plat_set_save_game(const char *stem) {
    if (!stem) { s_save_game[0] = 0; return; }
    int i = 0; for (; stem[i] && i < (int)sizeof(s_save_game) - 1; i++) s_save_game[i] = stem[i];
    s_save_game[i] = 0;
}
int mote_plat_save_slots(void) { return STUDIO_SAVE_SLOTS; }
static void studio_save_path(int slot, char *p, int n) {
    if (s_save_game[0]) snprintf(p, n, "mote_save_%s_%d.bin", s_save_game, slot);
    else                snprintf(p, n, "mote_save%d.bin", slot);
}
int mote_plat_save(int slot, const void *data, int len) {
    if (slot < 0 || slot >= STUDIO_SAVE_SLOTS) return 0;
    char p[64]; studio_save_path(slot, p, sizeof p);
    if (len <= 0) { remove(p); return 0; }
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    uint32_t L = (uint32_t)len; fwrite(&L, 4, 1, f); fwrite(data, 1, (size_t)len, f); fclose(f); return len;
}
int mote_plat_load(int slot, void *data, int max_len) {
    if (slot < 0 || slot >= STUDIO_SAVE_SLOTS) return 0;
    char p[64]; studio_save_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    uint32_t L = 0; if (fread(&L, 4, 1, f) != 1) { fclose(f); return 0; }
    if (data && max_len > 0) { int c = (int)L < max_len ? (int)L : max_len;
        if (fread(data, 1, (size_t)c, f) != (size_t)c) {} }
    fclose(f); return (int)L;
}

/* --- v38 key-value blobs: files in ./mote_kv/<game>__<key> --- */
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#ifdef _WIN32                       /* Windows/mingw mkdir() takes a single arg */
#include <direct.h>
#define KV_MKDIR(p) _mkdir(p)
#else
#define KV_MKDIR(p) mkdir((p), 0777)
#endif
static void kv_spath(const char *key, char *p, int n) {
    snprintf(p, n, "mote_kv/%s__%s", s_save_game[0] ? s_save_game : "game", key);
}
int mote_plat_kv_save(const char *key, const void *data, int len) {
    KV_MKDIR("mote_kv");
    char p[160]; kv_spath(key, p, sizeof p);
    if (len <= 0) { remove(p); return 0; }
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    size_t w = fwrite(data, 1, (size_t)len, f); fclose(f); return (int)w;
}
int mote_plat_kv_load(const char *key, void *data, int max) {
    char p[160]; kv_spath(key, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (data && max > 0) { int c = (int)sz < max ? (int)sz : max; if (fread(data, 1, (size_t)c, f) != (size_t)c) {} }
    fclose(f); return (int)sz;
}
void mote_plat_kv_list(const char *prefix, void (*cb)(const char *, void *), void *arg) {
    DIR *d = opendir("mote_kv"); if (!d) return;
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

/* --- ABI v43: 2-player link — rides the Studio's LAN link (studio/link_net).
 *
 * The pipe endpoints are UI-OWNED: the DEVICE tab's Host/Join buttons open the
 * LAN session and it deliberately SURVIVES game restarts (hot reload would drop
 * a game-owned connection on every Save). A preview game's link_start/stop only
 * gate whether the game sees the pipe. When the USB device BRIDGE is relaying a
 * real Thumby over the same pipe, preview games see SEARCHING (one consumer at
 * a time; mote_studio_link_bridge_active is set by the Studio UI). */
#include "../../studio/link_net.h"
int mote_studio_link_bridge_active;   /* set by the Studio UI while bridging */
static int s_lk_started;

/* --- Preview vs USB DEVICE (the UI's "Vs Device" mode): the preview game's
 * link rides a LOCAL pipe to the docked Thumby — two mutex-guarded byte rings
 * (game->device / device->game) whose serial end is pumped by the bridge
 * thread in studio/main.c. No network involved.
 *
 * The SAME rings also carry the preview's ONLINE play (s_pv_on): when a preview
 * game enters its multiplayer lobby and picks LAN/Internet, the Studio's preview
 * proxy (studio/main.c pvproxy_thread) reads the MN1 control protocol off these
 * rings — exactly as the USB proxy reads it off the CDC pipe of a docked device —
 * opens the relay/LAN room, then splices the rings to link_net. The preview is a
 * "virtual docked device": one owner at a time (Vs Device OR online, never both). */
static SDL_mutex *s_dl_mx;
static uint8_t s_dl_g2d[1024], s_dl_d2g[1024];
static int s_dl_g2d_h, s_dl_g2d_t, s_dl_d2g_h, s_dl_d2g_t;
static int s_dl_on;   /* Vs Device: rings pumped to a docked Thumby over USB */
static int s_pv_on;   /* preview online: rings serviced by the preview proxy (relay/LAN) */

static int dl_put(uint8_t *ring, int cap, int *h, int t, const uint8_t *d, int n) {
    int put = 0;
    while (put < n) { int nx = (*h + 1) % cap; if (nx == t) break; ring[*h] = d[put++]; *h = nx; }
    return put;
}
static int dl_get(const uint8_t *ring, int cap, int h, int *t, uint8_t *d, int max) {
    int got = 0;
    while (got < max && *t != h) { d[got++] = ring[*t]; *t = (*t + 1) % cap; }
    return got;
}
static void dl_lock(void) { if (!s_dl_mx) s_dl_mx = SDL_CreateMutex(); SDL_LockMutex(s_dl_mx); }

void mote_studio_devlink_set(int on) {
    dl_lock();
    s_dl_on = on;
    s_dl_g2d_h = s_dl_g2d_t = s_dl_d2g_h = s_dl_d2g_t = 0;
    SDL_UnlockMutex(s_dl_mx);
}
/* preview online proxy owns the link rings (relay/LAN, not the USB device) */
void mote_studio_pvlink_set(int on) {
    dl_lock();
    s_pv_on = on;
    s_dl_g2d_h = s_dl_g2d_t = s_dl_d2g_h = s_dl_d2g_t = 0;
    SDL_UnlockMutex(s_dl_mx);
}
int mote_studio_pvlink_active(void) { return s_pv_on; }
int mote_studio_devlink_active(void) { return s_dl_on; }
int mote_studio_preview_link_on(void) { return s_lk_started; }   /* preview game has link up */
int mote_studio_preview_link_waiting(void) {                     /* ...and needs an opponent */
    return s_lk_started && !s_dl_on && !s_pv_on && !mote_studio_link_bridge_active; }
int mote_studio_devlink_pull_tx(void *buf, int max) {        /* bridge: game -> serial */
    dl_lock(); int n = dl_get(s_dl_g2d, sizeof s_dl_g2d, s_dl_g2d_h, &s_dl_g2d_t, buf, max);
    SDL_UnlockMutex(s_dl_mx); return n;
}
int mote_studio_devlink_push_rx(const void *buf, int n) {    /* bridge: serial -> game */
    dl_lock(); int w = dl_put(s_dl_d2g, sizeof s_dl_d2g, &s_dl_d2g_h, s_dl_d2g_t, buf, n);
    SDL_UnlockMutex(s_dl_mx); return w;
}

void mote_plat_link_start(void)  { s_lk_started = 1; }
void mote_plat_link_stop(void)   { s_lk_started = 0; }
void mote_plat_link_task(void)   { link_net_task(); }
int  mote_plat_link_status(void) {
    if (!s_lk_started) return 0;
    if (s_dl_on || s_pv_on) return 2;                    /* ring pipe up = connected; the MN1
                                                          * control + game hello gate the rest */
    if (mote_studio_link_bridge_active) return 1;        /* LAN pipe busy: keep searching */
    return link_net_status();                            /* same 0/1/2 meanings */
}
int  mote_plat_link_is_host(void){
    if (s_dl_on || s_pv_on || mote_studio_link_bridge_active) return 0;   /* symmetric: games use a nonce */
    return link_net_is_host();
}
int  mote_plat_link_send(const void *data, int len) {
    if (!s_lk_started) return 0;
    if (s_dl_on || s_pv_on) { dl_lock(); int w = dl_put(s_dl_g2d, sizeof s_dl_g2d, &s_dl_g2d_h, s_dl_g2d_t, data, len);
                   SDL_UnlockMutex(s_dl_mx); return w; }
    if (mote_studio_link_bridge_active) return 0;
    return link_net_send(data, len);
}
int  mote_plat_link_recv(void *buf, int max) {
    if (!s_lk_started) return 0;
    if (s_dl_on || s_pv_on) { dl_lock(); int n = dl_get(s_dl_d2g, sizeof s_dl_d2g, s_dl_d2g_h, &s_dl_d2g_t, buf, max);
                   SDL_UnlockMutex(s_dl_mx); return n; }
    if (mote_studio_link_bridge_active) return 0;
    return link_net_recv(buf, max);
}
