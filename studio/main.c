/*
 * Mote Studio (Phase 1+2) — the embedded emulator in a Thumby Color shell, with
 * a game library and hot-reload.
 *
 * Left: a library of game projects (click to switch). Right: the real engine +
 * the selected game running in an illustrated device body with the buttons
 * lighting up. Editing a game's source (in VS Code) and saving triggers a
 * rebuild + live reload. The emulator is the shipping engine (the studio platform
 * backend), so what you see is what ships.
 *
 * Usage: mote_studio [game-dir-or-so]   Headless: MOTE_STUDIO_SHOT=/tmp/s.bmp ...
 * Keys: arrows/WASD d-pad, K/. = A, J/, = B, LShift = LB, Space = RB, Enter = MENU.
 */
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_config.h"
#include "mote_font.h"
#include "mote_api.h"
#include "../platform/studio/mote_plat_studio.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SIDEBAR_W 196
#define DEV_W     724
#define WIN_W     (SIDEBAR_W + DEV_W)
#define WIN_H     500
#define SCR   256                 /* 128 * 2 */
#define SCR_X 232
#define SCR_Y 104
#define ROW_H 24

typedef struct { Uint8 r, g, b; } Col;
static const Col C_BG     = { 18, 20, 28 };
static const Col C_SB     = { 24, 27, 38 };      /* sidebar */
static const Col C_SB_HDR = { 30, 36, 54 };
static const Col C_SEL    = { 46, 86, 150 };
static const Col C_TXT    = { 210, 220, 238 };
static const Col C_DIMTXT = { 130, 142, 168 };
static const Col C_TITLE  = { 255, 206, 92 };
static const Col C_BODY   = { 64, 86, 124 };
static const Col C_BODY_HI= { 86, 110, 150 };
static const Col C_BODY_LO= { 44, 60, 90 };
static const Col C_BEZEL  = { 10, 11, 16 };
static const Col C_DPAD   = { 32, 40, 58 };
static const Col C_DPAD_L = { 120, 196, 255 };
static const Col C_A      = { 240, 96, 110 };
static const Col C_B      = { 250, 204, 84 };
static const Col C_SHLD   = { 40, 52, 76 };
static const Col C_ACCENT = { 120, 196, 255 };

static Col mul(Col c, float f) {
    int r=(int)(c.r*f), g=(int)(c.g*f), b=(int)(c.b*f);
    if(r>255)r=255; if(g>255)g=255; if(b>255)b=255;
    Col o={(Uint8)r,(Uint8)g,(Uint8)b}; return o;
}

/* ---- game library ---- */
typedef struct { char dir[256], name[64]; SDL_Texture *label; int lw; } Game;
static Game  g_games[256];
static int   g_ngame, g_sel = -1;
static char  g_so[1024];
static time_t g_watch;

static SDL_Thread *g_eng;

/* engine worker: dlopen the current module + run the real OS loop */
static int engine_thread(void *arg) { (void)arg;
    void *mod = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (!mod) { fprintf(stderr, "studio: dlopen: %s\n", dlerror()); return 1; }
    const uint32_t *abi = (const uint32_t *)dlsym(mod, "mote_game_abi_version");
    MoteGameRegisterFn reg = (MoteGameRegisterFn)dlsym(mod, "mote_game_register");
    if (!abi || !reg) { fprintf(stderr, "studio: not a game module\n"); dlclose(mod); return 1; }
    MoteApi api; mote_api_fill(&api);
    const MoteGameVtbl *vt = reg(&api);
    if (vt) mote_os_run(&api, vt);
    dlclose(mod);
    return 0;
}
static void stop_engine(void) {
    if (!g_eng) return;
    mote_studio_request_quit(); SDL_WaitThread(g_eng, NULL); g_eng = NULL;
}
static void start_engine(void) { mote_studio_reset(); g_eng = SDL_CreateThread(engine_thread, "engine", NULL); }

static time_t src_mtime(const char *dir) {
    char src[300]; snprintf(src, sizeof src, "%s/src", dir);
    DIR *d = opendir(src); if (!d) return 0;
    struct dirent *e; time_t m = 0;
    while ((e = readdir(d))) { size_t n = strlen(e->d_name);
        if (n > 2 && (strcmp(e->d_name+n-2, ".c") == 0 || strcmp(e->d_name+n-2, ".h") == 0)) {
            char p[600]; snprintf(p, sizeof p, "%s/%s", src, e->d_name);
            struct stat st; if (stat(p, &st) == 0 && st.st_mtime > m) m = st.st_mtime; } }
    closedir(d); return m;
}
/* build dir + (re)load it into the emulator */
static void load_game(int idx, int rebuild) {
    if (idx < 0 || idx >= g_ngame) return;
    g_sel = idx;
    if (rebuild) { char cmd[600]; snprintf(cmd, sizeof cmd, "./tools/mote build %s >/dev/null 2>&1", g_games[idx].dir);
        if (system(cmd) != 0) fprintf(stderr, "studio: build failed for %s\n", g_games[idx].dir); }
    snprintf(g_so, sizeof g_so, "%s/build/%s.so", g_games[idx].dir, g_games[idx].name);
    g_watch = src_mtime(g_games[idx].dir);
    stop_engine(); start_engine();
}

/* ---- text via the engine font, baked to a texture ---- */
static SDL_Texture *label_tex(SDL_Renderer *R, const char *s, Col fg, Col bg, int *outw) {
    static uint16_t buf[MOTE_FB_W * 9];
    uint16_t b565 = (uint16_t)MOTE_RGB565(bg.r, bg.g, bg.b);
    for (int i = 0; i < MOTE_FB_W * 9; i++) buf[i] = b565;
    mote_font_draw(buf, s, 0, 1, (uint16_t)MOTE_RGB565(fg.r, fg.g, fg.b));
    int w = mote_font_width(s); if (w < 1) w = 1; if (w > MOTE_FB_W) w = MOTE_FB_W;
    SDL_Texture *t = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, MOTE_FB_W, 9);
    SDL_UpdateTexture(t, NULL, buf, MOTE_FB_W * 2);
    if (outw) *outw = w; return t;
}
static void blit_label(SDL_Renderer *R, SDL_Texture *t, int w, int x, int y, int scale) {
    SDL_Rect src = { 0, 0, w, 9 }, dst = { x, y, w * scale, 9 * scale };
    SDL_RenderCopy(R, t, &src, &dst);
}

static int cmp_game(const void *a, const void *b) { return strcmp(((const Game*)a)->name, ((const Game*)b)->name); }
static void scan_games(void) {
    DIR *d = opendir("examples"); if (!d) return; struct dirent *e;
    while ((e = readdir(d)) && g_ngame < 256) { if (e->d_name[0] == '.') continue;
        char p[400]; snprintf(p, sizeof p, "examples/%s/src/game.c", e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        Game *g = &g_games[g_ngame++];
        snprintf(g->dir, sizeof g->dir, "examples/%s", e->d_name);
        snprintf(g->name, sizeof g->name, "%s", e->d_name);
    }
    closedir(d);
    qsort(g_games, g_ngame, sizeof g_games[0], cmp_game);
}

/* ---- drawing ---- */
static void plain(SDL_Renderer *R, int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(R, &r);
}
static void round_rect(SDL_Renderer *R, int x, int y, int w, int h, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int j = 0; j < h; j++) { int in = 0, dy = -1;
        if (j < rad) dy = rad - j; else if (j >= h - rad) dy = j - (h - rad) + 1;
        if (dy >= 0) in = rad - (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, x + in, y + j, x + w - 1 - in, y + j); }
}
static void disc(SDL_Renderer *R, int cx, int cy, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int dy = -rad; dy <= rad; dy++) { int dx = (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, cx - dx, cy + dy, cx + dx, cy + dy); }
}
static void button(SDL_Renderer *R, int cx, int cy, int rad, Col base, int lit) {
    disc(R, cx, cy + 3, rad + 1, (Col){ 12, 14, 22 });
    if (lit) disc(R, cx, cy, rad + 4, mul(base, 0.9f));
    disc(R, cx, cy, rad, mul(base, 0.55f));
    disc(R, cx, cy, rad - 3, lit ? mul(base, 1.2f) : base);
    disc(R, cx - rad/3, cy - rad/3, rad/3, mul(lit ? mul(base,1.2f) : base, 1.5f));
}

static void draw_sidebar(SDL_Renderer *R) {
    SDL_Rect vp = { 0, 0, SIDEBAR_W, WIN_H }; SDL_RenderSetViewport(R, &vp);
    plain(R, 0, 0, SIDEBAR_W, WIN_H, C_SB);
    plain(R, 0, 0, SIDEBAR_W, 30, C_SB_HDR);
    plain(R, 0, 30, SIDEBAR_W, 2, C_ACCENT);
    static SDL_Texture *hdr; static int hdr_w;
    if (!hdr) hdr = label_tex(R, "MOTE STUDIO", C_TITLE, C_SB_HDR, &hdr_w);
    blit_label(R, hdr, hdr_w, 8, 8, 2);
    for (int i = 0; i < g_ngame; i++) {
        int y = 38 + i * ROW_H;
        if (y > WIN_H - ROW_H) break;
        Col bg = (i == g_sel) ? C_SEL : C_SB;
        if (i == g_sel) plain(R, 0, y, SIDEBAR_W, ROW_H - 2, C_SEL);
        if (!g_games[i].label) g_games[i].label = label_tex(R, g_games[i].name,
            (i == g_sel) ? C_TXT : C_DIMTXT, bg, &g_games[i].lw);
        blit_label(R, g_games[i].label, g_games[i].lw, 10, y + 5, 2);
    }
}

static void draw_device(SDL_Renderer *R, SDL_Texture *tex, const MoteButtons *b) {
    SDL_Rect vp = { SIDEBAR_W, 0, DEV_W, WIN_H }; SDL_RenderSetViewport(R, &vp);
    static uint16_t fr[MOTE_FB_W * MOTE_FB_H];
    mote_studio_get_frame(fr);
    SDL_UpdateTexture(tex, NULL, fr, MOTE_FB_W * (int)sizeof(uint16_t));

    int bx = 38, by = 18, bw = 648, bh = 464, br = 46;
    round_rect(R, bx, by + 6, bw, bh, br, C_BODY_LO);
    round_rect(R, bx, by, bw, bh - 8, br, C_BODY);
    round_rect(R, bx + 8, by + 6, bw - 16, 60, br - 8, C_BODY_HI);
    round_rect(R, bx + 40, by + 2, 120, 30, 12, mul(C_SHLD, 0.7f));
    round_rect(R, bx + 42, by + 4, 116, 24, 10, b->lb ? C_ACCENT : C_SHLD);
    round_rect(R, bw + bx - 160, by + 2, 120, 30, 12, mul(C_SHLD, 0.7f));
    round_rect(R, bw + bx - 158, by + 4, 116, 24, 10, b->rb ? C_ACCENT : C_SHLD);

    round_rect(R, SCR_X - 14, SCR_Y - 14, SCR + 28, SCR + 28, 16, C_BEZEL);
    round_rect(R, SCR_X - 14, SCR_Y - 14, SCR + 28, 10, 16, (Col){ 30, 32, 42 });
    plain(R, SCR_X - 2, SCR_Y - 2, SCR + 4, SCR + 4, (Col){ 40, 44, 56 });
    SDL_Rect dst = { SCR_X, SCR_Y, SCR, SCR }; SDL_RenderCopy(R, tex, NULL, &dst);

    int dcx = 116, dcy = 300, dw = 34, al = 44;
    round_rect(R, dcx - al, dcy - dw/2, 2*al, dw, dw/2, C_DPAD);
    round_rect(R, dcx - dw/2, dcy - al, dw, 2*al, dw/2, C_DPAD);
    if (b->up)    plain(R, dcx - dw/2 + 4, dcy - al + 4,  dw - 8, al - dw/2, C_DPAD_L);
    if (b->down)  plain(R, dcx - dw/2 + 4, dcy + dw/2,    dw - 8, al - dw/2 - 4, C_DPAD_L);
    if (b->left)  plain(R, dcx - al + 4,   dcy - dw/2 + 4, al - dw/2, dw - 8, C_DPAD_L);
    if (b->right) plain(R, dcx + dw/2,     dcy - dw/2 + 4, al - dw/2 - 4, dw - 8, C_DPAD_L);
    disc(R, dcx, dcy, 7, mul(C_DPAD, 1.4f));

    button(R, 612, 318, 26, C_A, b->a);
    button(R, 556, 280, 26, C_B, b->b);
    round_rect(R, 322, 432, 76, 22, 11, mul(C_SHLD, 0.7f));
    round_rect(R, 324, 434, 72, 18, 9, b->menu ? C_ACCENT : C_SHLD);
    SDL_RenderSetViewport(R, NULL);
}

static void poll_input(MoteButtons *b, SDL_GameController *pad) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    memset(b, 0, sizeof *b);
    b->up = k[SDL_SCANCODE_UP]||k[SDL_SCANCODE_W];   b->down  = k[SDL_SCANCODE_DOWN]||k[SDL_SCANCODE_S];
    b->left = k[SDL_SCANCODE_LEFT]||k[SDL_SCANCODE_A]; b->right = k[SDL_SCANCODE_RIGHT]||k[SDL_SCANCODE_D];
    b->a = k[SDL_SCANCODE_K]||k[SDL_SCANCODE_PERIOD]; b->b = k[SDL_SCANCODE_J]||k[SDL_SCANCODE_COMMA];
    b->lb = k[SDL_SCANCODE_LSHIFT]; b->rb = k[SDL_SCANCODE_SPACE]; b->menu = k[SDL_SCANCODE_RETURN];
    if (pad) {
        b->up|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP);
        b->down|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        b->left|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        b->right|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        b->a|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A);
        b->b|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_B);
        b->lb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        b->rb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        b->menu|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_START);
    }
}

int main(int argc, char **argv) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    mote_plat_init("Mote Studio");
    scan_games();

    /* select the requested game (dir or .so) or default to the first */
    int start = 0;
    if (argc > 1) { for (int i = 0; i < g_ngame; i++)
        if (strstr(argv[1], g_games[i].name)) { start = i; break; } }

    const char *shot = getenv("MOTE_STUDIO_SHOT");
    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Surface *surf = NULL;
    if (shot) { surf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_RGBA8888);
        ren = SDL_CreateSoftwareRenderer(surf); }
    else { win = SDL_CreateWindow("Mote Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC); }
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, MOTE_FB_W, MOTE_FB_H);

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }

    if (g_ngame) load_game(start, 1);

    if (shot) {
        SDL_Delay(700);
        MoteButtons b; memset(&b, 0, sizeof b); b.a = true; b.right = true; mote_studio_set_buttons(&b);
        draw_sidebar(ren); draw_device(ren, tex, &b); SDL_RenderPresent(ren);
        SDL_SaveBMP(surf, shot); printf("studio: wrote %s\n", shot);
    } else {
        int running = 1, watch_tick = 0;
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) running = 0;
                else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.x < SIDEBAR_W) {
                    int i = (e.button.y - 38) / ROW_H;
                    if (i >= 0 && i < g_ngame && i != g_sel) load_game(i, 1);
                }
            }
            /* hot-reload: poll the current game's sources ~2x/sec */
            if (++watch_tick >= 30 && g_sel >= 0) { watch_tick = 0;
                time_t m = src_mtime(g_games[g_sel].dir);
                if (m > g_watch) { printf("studio: source changed, reloading %s\n", g_games[g_sel].name); load_game(g_sel, 1); }
            }
            MoteButtons b; poll_input(&b, pad); mote_studio_set_buttons(&b);
            SDL_SetRenderDrawColor(ren, C_BG.r, C_BG.g, C_BG.b, 255); SDL_RenderClear(ren);
            draw_sidebar(ren); draw_device(ren, tex, &b);
            SDL_RenderPresent(ren);
        }
    }

    stop_engine();
    SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (surf) SDL_FreeSurface(surf);
    SDL_Quit();
    return 0;
}
