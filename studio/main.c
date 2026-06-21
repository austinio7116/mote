/*
 * Mote Studio (Phase 1) — the embedded emulator in a Thumby Color shell.
 *
 * Runs the REAL engine + a game module on a worker thread (the studio platform
 * backend), and draws the 128x128 framebuffer at 2x inside an illustrated device
 * body with the buttons lighting up as they're pressed. Input: keyboard (+ a
 * gamepad if present). This is the core "see it run" panel the rest of the IDE
 * will be built around.
 *
 * Usage: mote_studio [game.so]     (default: examples/piano3d/build/piano3d.so)
 * Headless capture: MOTE_STUDIO_SHOT=/tmp/s.bmp mote_studio <game.so>
 */
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_config.h"
#include "mote_api.h"
#include "../platform/studio/mote_plat_studio.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WIN_W 760
#define WIN_H 520
#define SCR   256                 /* 128 * 2 */
#define SCR_X 252
#define SCR_Y 96

typedef struct { Uint8 r, g, b; } Col;
static const Col C_BG    = { 14, 16, 24 };
static const Col C_BODY  = { 38, 42, 52 };
static const Col C_EDGE  = { 92, 100, 118 };
static const Col C_LIT   = { 96, 176, 255 };
static const Col C_DIM   = { 58, 64, 78 };
static const Col C_BEZEL = { 8, 9, 14 };

static char g_so[1024];

/* ---- engine worker: load the game module + run the real OS loop ---- */
static int engine_thread(void *arg) {
    (void)arg;
    void *mod = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (!mod) { fprintf(stderr, "studio: dlopen failed: %s\n", dlerror()); return 1; }
    const uint32_t *abi = (const uint32_t *)dlsym(mod, "mote_game_abi_version");
    MoteGameRegisterFn reg = (MoteGameRegisterFn)dlsym(mod, "mote_game_register");
    if (!abi || !reg) { fprintf(stderr, "studio: '%s' is not a game module\n", g_so); return 1; }
    MoteApi api; mote_api_fill(&api);
    const MoteGameVtbl *vt = reg(&api);
    if (!vt) return 1;
    printf("studio: running '%s' (ABI v%u)\n", g_so, *abi);
    mote_os_run(&api, vt);
    dlclose(mod);
    return 0;
}

/* ---- drawing helpers ---- */
static void rect(SDL_Renderer *R, int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(R, &r);
}
static void outline(SDL_Renderer *R, int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderDrawRect(R, &r);
}
static void circ(SDL_Renderer *R, int cx, int cy, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int dy = -rad; dy <= rad; dy++) { int dx = (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, cx - dx, cy + dy, cx + dx, cy + dy); }
}
static void key_rect(SDL_Renderer *R, int x, int y, int w, int h, int lit) {
    rect(R, x, y, w, h, lit ? C_LIT : C_DIM);
    outline(R, x, y, w, h, C_EDGE);
}
static void key_circ(SDL_Renderer *R, int cx, int cy, int rad, int lit) {
    circ(R, cx, cy, rad, lit ? C_LIT : C_DIM);
}

static void draw(SDL_Renderer *R, SDL_Texture *tex, const MoteButtons *b) {
    SDL_SetRenderDrawColor(R, C_BG.r, C_BG.g, C_BG.b, 255); SDL_RenderClear(R);
    /* device body */
    rect(R, 44, 24, 672, 472, C_BODY);
    outline(R, 44, 24, 672, 472, C_EDGE);
    /* screen bezel + the live framebuffer at 2x */
    rect(R, SCR_X - 5, SCR_Y - 5, SCR + 10, SCR + 10, C_BEZEL);
    SDL_UpdateTexture(tex, NULL, mote_launcher_fb(), MOTE_FB_W * (int)sizeof(uint16_t));
    SDL_Rect dst = { SCR_X, SCR_Y, SCR, SCR }; SDL_RenderCopy(R, tex, NULL, &dst);
    /* D-pad (left) */
    int dx = 120, dy = 300, s = 30;
    key_rect(R, dx - s/2, dy - s - s/2, s, s, b->up);
    key_rect(R, dx - s/2, dy + s/2, s, s, b->down);
    key_rect(R, dx - s - s/2, dy - s/2, s, s, b->left);
    key_rect(R, dx + s/2, dy - s/2, s, s, b->right);
    /* A / B (right) */
    key_circ(R, 648, 300, 22, b->a);
    key_circ(R, 600, 344, 22, b->b);
    /* shoulders */
    key_rect(R, 70, 40, 96, 26, b->lb);
    key_rect(R, 594, 40, 96, 26, b->rb);
    /* MENU */
    key_rect(R, 330, 430, 100, 24, b->menu);
}

static void poll_input(MoteButtons *b, SDL_GameController *pad) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    memset(b, 0, sizeof *b);
    b->up    = k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W];
    b->down  = k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S];
    b->left  = k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A];
    b->right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    b->a     = k[SDL_SCANCODE_K] || k[SDL_SCANCODE_PERIOD];
    b->b     = k[SDL_SCANCODE_J] || k[SDL_SCANCODE_COMMA];
    b->lb    = k[SDL_SCANCODE_LSHIFT];
    b->rb    = k[SDL_SCANCODE_SPACE];
    b->menu  = k[SDL_SCANCODE_RETURN];
    if (pad) {
        b->up    |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
        b->down  |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        b->left  |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        b->right |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        b->a     |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
        b->b     |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
        b->lb    |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        b->rb    |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        b->menu  |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
    }
}

int main(int argc, char **argv) {
    snprintf(g_so, sizeof g_so, "%s", argc > 1 ? argv[1] : "examples/piano3d/build/piano3d.so");

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    mote_plat_init("Mote Studio");

    const char *shot = getenv("MOTE_STUDIO_SHOT");
    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Surface *surf = NULL;
    if (shot) {
        surf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_RGBA8888);
        ren  = SDL_CreateSoftwareRenderer(surf);
    } else {
        win = SDL_CreateWindow("Mote Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               WIN_W, WIN_H, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    }
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                         MOTE_FB_W, MOTE_FB_H);

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }

    mote_studio_reset();
    SDL_Thread *eng = SDL_CreateThread(engine_thread, "engine", NULL);

    if (shot) {
        SDL_Delay(700);                                  /* let the game render */
        MoteButtons b; memset(&b, 0, sizeof b); b.a = true; b.right = true;  /* show lit keys */
        mote_studio_set_buttons(&b);
        draw(ren, tex, &b);
        SDL_RenderPresent(ren);
        SDL_SaveBMP(surf, shot);
        printf("studio: wrote %s\n", shot);
    } else {
        int running = 1;
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) running = 0;
            MoteButtons b; poll_input(&b, pad);
            mote_studio_set_buttons(&b);
            draw(ren, tex, &b);
            SDL_RenderPresent(ren);
        }
    }

    mote_studio_request_quit();
    SDL_WaitThread(eng, NULL);
    SDL_DestroyTexture(tex);
    if (ren)  SDL_DestroyRenderer(ren);
    if (win)  SDL_DestroyWindow(win);
    if (surf) SDL_FreeSurface(surf);
    SDL_Quit();
    return 0;
}
