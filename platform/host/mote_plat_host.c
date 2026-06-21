/*
 * Mote — SDL2 host platform (the PC emulator).
 *
 * Implements mote_platform.h on SDL2: a 128x128 RGB565 frame scaled up into a
 * window, keyboard mapped to the handheld's buttons. Same engine + game code
 * runs here and on the device; only this file (and the device twin) differ.
 *
 * Keyboard (matches the Thumby Color emulator convention):
 *   D-pad : arrow keys or W/A/S/D
 *   A     : '.'  or  K        B : ','  or  J
 *   LB    : Left Shift        RB: Space
 *   MENU  : Enter             Quit: Esc / window close
 */
#include "../../engine/core/mote_platform.h"
#include "../../engine/audio/mote_audio.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <stdio.h>

static void mote_host_audio_cb(void *u, Uint8 *stream, int len){ (void)u;
    mote_audio_render((int16_t *)stream, len / 2);   /* 16-bit mono */
}

#ifndef MOTE_HOST_SCALE
#define MOTE_HOST_SCALE 4
#endif

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static bool          s_quit;
static bool          s_headless;   /* no display: present is a no-op */
static uint64_t      s_freq;

/* Headless capture: MOTE_SHOT=/path.ppm dumps frame MOTE_SHOT_FRAME (default 20)
 * then quits. Works with or without a display — handy for CI and parity
 * checks since it reads the engine's own framebuffer. */
static const char *s_shot_path;
static int          s_shot_frame = 20;
static int          s_frame;
static uint64_t     s_dt_us;     /* fixed timestep (0 = real clock) */
static uint64_t     s_vclock;    /* virtual clock for fixed-timestep mode */

static void dump_ppm(const char *path, const uint16_t *fb) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", MOTE_FB_W, MOTE_FB_H);
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) {
        uint16_t c = fb[i];
        fputc(((c >> 11) & 0x1F) << 3, f);
        fputc(((c >> 5) & 0x3F) << 2, f);
        fputc((c & 0x1F) << 3, f);
    }
    fclose(f);
}

int mote_plat_init(const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    mote_audio_init();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
        want.freq = MOTE_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 1;
        want.samples = 512; want.callback = mote_host_audio_cb;
        if (SDL_OpenAudio(&want, NULL) == 0) SDL_PauseAudio(0);
    }
    s_freq = SDL_GetPerformanceFrequency();
    s_quit = false;
    s_headless = false;
    /* Fixed-timestep mode: MOTE_DT_MS makes the clock advance a fixed amount
     * per frame, so emulation is deterministic and decoupled from wall-clock
     * (reproducible runs, headless physics capture). */
    s_dt_us = getenv("MOTE_DT_MS") ? (uint64_t)(atof(getenv("MOTE_DT_MS")) * 1000.0) : 0;
    s_vclock = 0;
    s_shot_path = getenv("MOTE_SHOT");
    if (getenv("MOTE_SHOT_FRAME")) s_shot_frame = atoi(getenv("MOTE_SHOT_FRAME"));
    s_frame = 0;

    s_win = SDL_CreateWindow(title ? title : "Mote",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             MOTE_FB_W * MOTE_HOST_SCALE, MOTE_FB_H * MOTE_HOST_SCALE,
                             SDL_WINDOW_SHOWN);
    if (s_win)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (s_ren) {
        SDL_RenderSetLogicalSize(s_ren, MOTE_FB_W, MOTE_FB_H);
        s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING, MOTE_FB_W, MOTE_FB_H);
    }
    if (!s_win || !s_ren || !s_tex) {
        /* No usable display (headless CI / dummy driver). The engine still
         * renders into its framebuffer; present just does nothing. */
        SDL_Log("mote_plat: no display (%s) — running headless", SDL_GetError());
        s_headless = true;
    }
    return 0;
}

void mote_plat_present(const uint16_t *fb565) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) s_quit = true;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) s_quit = true;
    }
    if (s_dt_us) s_vclock += s_dt_us;     /* advance the virtual clock per frame */
    if (s_shot_path && ++s_frame == s_shot_frame) {
        dump_ppm(s_shot_path, fb565);
        SDL_Log("mote_plat: wrote %s at frame %d", s_shot_path, s_frame);
        s_quit = true;
    }
    if (s_headless) return;
    SDL_UpdateTexture(s_tex, NULL, fb565, MOTE_FB_W * (int)sizeof(uint16_t));
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

void mote_plat_buttons(MoteButtons *out) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    out->up    = k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W];
    out->down  = k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S];
    out->left  = k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A];
    out->right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    out->a     = k[SDL_SCANCODE_PERIOD] || k[SDL_SCANCODE_K];
    out->b     = k[SDL_SCANCODE_COMMA]  || k[SDL_SCANCODE_J];
    out->lb    = k[SDL_SCANCODE_LSHIFT];
    out->rb    = k[SDL_SCANCODE_SPACE];
    out->menu  = k[SDL_SCANCODE_RETURN];
}

uint64_t mote_plat_micros(void) {
    if (s_dt_us) return s_vclock;     /* fixed-timestep: constant within a frame */
    return (uint64_t)((SDL_GetPerformanceCounter() * 1000000ull) / s_freq);
}

bool mote_plat_should_quit(void) { return s_quit; }

int mote_plat_pending_launch(void) { return -1; }

void mote_plat_log(const char *s) { printf("%s\n", s); fflush(stdout); }

void mote_plat_render2(uint16_t *fb, MoteBandFn band,
                       uint32_t *out_c0_us, uint32_t *out_c1_us) {
    uint64_t t0 = mote_plat_micros();
    band(fb, 0, MOTE_FB_H);          /* host is single-threaded */
    *out_c0_us = (uint32_t)(mote_plat_micros() - t0);
    *out_c1_us = 0;
}

/* No async DMA on host — present immediately, nothing to wait for. */
void mote_plat_present_async(const uint16_t *fb565) { mote_plat_present(fb565); }
uint32_t mote_plat_wait_flush(void) { return 0; }

void mote_plat_shutdown(void) {
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    SDL_Quit();
}

void mote_plat_set_brightness(int pct) { (void)pct; }   /* host: no backlight */
void mote_plat_set_volume(int pct) { mote_audio_set_volume(pct / 100.0f); }
void mote_plat_audio_pump(void) { }                      /* host: SDL pulls via callback */
void mote_plat_audio_start(void) { mote_audio_off(); }   /* host: SDL persists; just clear voices */
