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

void mote_studio_set_buttons(const MoteButtons *b) { s_btn = *b; }
void mote_studio_request_quit(void) { s_quit = 1; }
void mote_studio_reset(void) { s_quit = 0; SDL_memset(&s_btn, 0, sizeof s_btn); }

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

void     mote_plat_present(const uint16_t *fb)       { (void)fb; }   /* Studio reads the fb */
void     mote_plat_present_async(const uint16_t *fb) { (void)fb; }
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

bool mote_plat_should_quit(void)     { return s_quit != 0; }
int  mote_plat_pending_launch(void)  { return -1; }
void mote_plat_log(const char *s)    { printf("[game] %s\n", s); fflush(stdout); }
void mote_plat_shutdown(void)        { }
void mote_plat_set_brightness(int p) { (void)p; }
void mote_plat_set_volume(int p)     { mote_audio_set_volume(p / 100.0f); }
void mote_plat_audio_pump(void)      { }
void mote_plat_audio_start(void)     { mote_audio_off(); }
