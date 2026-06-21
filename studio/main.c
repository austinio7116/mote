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

#define WIN_W 720
#define WIN_H 500
#define SCR   256                 /* 128 * 2 */
#define SCR_X 232
#define SCR_Y 104

typedef struct { Uint8 r, g, b; } Col;
static const Col C_BG     = { 18, 20, 28 };
static const Col C_BODY   = { 64, 86, 124 };     /* the Thumby body */
static const Col C_BODY_HI= { 86, 110, 150 };    /* top bevel highlight */
static const Col C_BODY_LO= { 44, 60, 90 };      /* bottom shade */
static const Col C_EDGE   = { 28, 38, 58 };
static const Col C_BEZEL  = { 10, 11, 16 };
static const Col C_DPAD   = { 32, 40, 58 };
static const Col C_DPAD_L = { 120, 196, 255 };   /* lit d-pad */
static const Col C_A      = { 240, 96, 110 };    /* A red */
static const Col C_B      = { 250, 204, 84 };    /* B yellow */
static const Col C_SHLD   = { 40, 52, 76 };
static const Col C_ACCENT = { 120, 196, 255 };

static char g_so[1024];

static Col mul(Col c, float f) {
    int r=(int)(c.r*f), g=(int)(c.g*f), b=(int)(c.b*f);
    if(r>255)r=255; if(g>255)g=255; if(b>255)b=255;
    Col o={(Uint8)r,(Uint8)g,(Uint8)b}; return o;
}

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
static void plain(SDL_Renderer *R, int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(R, &r);
}
/* filled rounded rectangle (scanline-inset corners) */
static void round_rect(SDL_Renderer *R, int x, int y, int w, int h, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int j = 0; j < h; j++) {
        int in = 0, dy = -1;
        if (j < rad) dy = rad - j; else if (j >= h - rad) dy = j - (h - rad) + 1;
        if (dy >= 0) in = rad - (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, x + in, y + j, x + w - 1 - in, y + j);
    }
}
static void disc(SDL_Renderer *R, int cx, int cy, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int dy = -rad; dy <= rad; dy++) { int dx = (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, cx - dx, cy + dy, cx + dx, cy + dy); }
}
/* a chunky 3D action button (shadow, bevel ring, face, highlight; glows when lit) */
static void button(SDL_Renderer *R, int cx, int cy, int rad, Col base, int lit) {
    disc(R, cx, cy + 3, rad + 1, (Col){ 12, 14, 22 });          /* drop shadow */
    if (lit) disc(R, cx, cy, rad + 4, mul(base, 0.9f));          /* glow halo */
    disc(R, cx, cy, rad, mul(base, 0.55f));                      /* bevel ring */
    disc(R, cx, cy, rad - 3, lit ? mul(base, 1.2f) : base);      /* face */
    disc(R, cx - rad/3, cy - rad/3, rad/3, mul(lit ? mul(base,1.2f) : base, 1.5f)); /* highlight */
}

static void draw(SDL_Renderer *R, SDL_Texture *tex, const MoteButtons *b) {
    static uint16_t fr[MOTE_FB_W * MOTE_FB_H];
    mote_studio_get_frame(fr);                                  /* tear-free latest frame */
    SDL_UpdateTexture(tex, NULL, fr, MOTE_FB_W * (int)sizeof(uint16_t));
    SDL_SetRenderDrawColor(R, C_BG.r, C_BG.g, C_BG.b, 255); SDL_RenderClear(R);

    /* --- device body: a rounded shell with a top bevel + bottom shade --- */
    int bx = 36, by = 18, bw = 648, bh = 464, br = 46;
    round_rect(R, bx, by + 6, bw, bh, br, C_BODY_LO);           /* bottom shade */
    round_rect(R, bx, by, bw, bh - 8, br, C_BODY);             /* body */
    round_rect(R, bx + 8, by + 6, bw - 16, 60, br - 8, C_BODY_HI); /* top highlight band */

    /* --- shoulder buttons recessed into the top edge --- */
    round_rect(R, bx + 40, by + 2, 120, 30, 12, mul(C_SHLD, 0.7f));
    round_rect(R, bx + 42, by + 4, 116, 24, 10, b->lb ? C_ACCENT : C_SHLD);
    round_rect(R, bw + bx - 160, by + 2, 120, 30, 12, mul(C_SHLD, 0.7f));
    round_rect(R, bw + bx - 158, by + 4, 116, 24, 10, b->rb ? C_ACCENT : C_SHLD);

    /* --- screen: glossy black bezel + a thin inner frame + the live fb at 2x --- */
    round_rect(R, SCR_X - 14, SCR_Y - 14, SCR + 28, SCR + 28, 16, C_BEZEL);
    round_rect(R, SCR_X - 14, SCR_Y - 14, SCR + 28, 10, 16, (Col){ 30, 32, 42 });   /* glossy top */
    plain(R, SCR_X - 2, SCR_Y - 2, SCR + 4, SCR + 4, (Col){ 40, 44, 56 });          /* inner frame */
    SDL_Rect dst = { SCR_X, SCR_Y, SCR, SCR }; SDL_RenderCopy(R, tex, NULL, &dst);

    /* --- D-pad (left): a cross with the held arm lit --- */
    int dcx = 116, dcy = 300, dw = 34, al = 44;
    round_rect(R, dcx - al, dcy - dw/2, 2*al, dw, dw/2, C_DPAD);       /* horizontal */
    round_rect(R, dcx - dw/2, dcy - al, dw, 2*al, dw/2, C_DPAD);       /* vertical */
    if (b->up)    plain(R, dcx - dw/2 + 4, dcy - al + 4,  dw - 8, al - dw/2, C_DPAD_L);
    if (b->down)  plain(R, dcx - dw/2 + 4, dcy + dw/2,    dw - 8, al - dw/2 - 4, C_DPAD_L);
    if (b->left)  plain(R, dcx - al + 4,   dcy - dw/2 + 4, al - dw/2, dw - 8, C_DPAD_L);
    if (b->right) plain(R, dcx + dw/2,     dcy - dw/2 + 4, al - dw/2 - 4, dw - 8, C_DPAD_L);
    disc(R, dcx, dcy, 7, mul(C_DPAD, 1.4f));                            /* centre pivot */

    /* --- A / B (right), Game-Boy diagonal --- */
    button(R, 612, 318, 26, C_A, b->a);
    button(R, 556, 280, 26, C_B, b->b);

    /* --- MENU pill (bottom centre) --- */
    round_rect(R, 322, 432, 76, 22, 11, mul(C_SHLD, 0.7f));
    round_rect(R, 324, 434, 72, 18, 9, b->menu ? C_ACCENT : C_SHLD);
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
