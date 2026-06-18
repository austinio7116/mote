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
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <stdio.h>

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
    s_freq = SDL_GetPerformanceFrequency();
    s_quit = false;
    s_headless = false;
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
    return (uint64_t)((SDL_GetPerformanceCounter() * 1000000ull) / s_freq);
}

bool mote_plat_should_quit(void) { return s_quit; }

void mote_plat_shutdown(void) {
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    SDL_Quit();
}
