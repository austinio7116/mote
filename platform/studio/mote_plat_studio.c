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
void mote_plat_log(const char *s)    { printf("[game] %s\n", s); fflush(stdout); }
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
