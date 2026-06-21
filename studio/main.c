/*
 * Mote Studio — the host IDE. Three panes:
 *   LEFT   library of game projects (click to open)
 *   CENTRE the embedded emulator inside a Thumby Color shell (purple, GBA-style
 *          landscape: 0.85" square screen, d-pad left, A/B right, L/R shoulders,
 *          menu) — the REAL engine, buttons light when pressed (keyboard/gamepad)
 *   RIGHT  inspector: the open game's info + actions (Edit in VS Code, Build,
 *          Reload, Push to device, Bake assets) + a status console
 *
 * Editing a game in VS Code and saving hot-reloads it. Opens to a home screen.
 * Run from the repo root: `mote studio`.
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

#define SIDEBAR_W 198
#define DEV_W     566
#define INSP_W    416
#define WIN_W     (SIDEBAR_W + DEV_W + INSP_W)
#define WIN_H     580
#define INSP_X    (SIDEBAR_W + DEV_W)
#define SCR   256
#define SCR_X ((DEV_W - SCR) / 2)
#define SCR_Y ((WIN_H - SCR) / 2)
#define ROW_H 24

typedef struct { Uint8 r, g, b; } Col;
static const Col C_BG     = { 16, 17, 24 };
static const Col C_SB     = { 24, 27, 38 };
static const Col C_SB_HDR = { 30, 36, 54 };
static const Col C_SEL    = { 46, 86, 150 };
static const Col C_TXT    = { 214, 222, 238 };
static const Col C_DIMTXT = { 132, 144, 170 };
static const Col C_TITLE  = { 255, 206, 92 };
/* Thumby Color: purple GBA-style body */
static const Col C_BODY   = { 124, 90, 182 };
static const Col C_BODY_HI= { 150, 118, 206 };
static const Col C_BODY_LO= { 96, 66, 146 };
static const Col C_BEZEL  = { 10, 11, 16 };
static const Col C_DPAD   = { 38, 32, 54 };
static const Col C_DPAD_L = { 150, 196, 255 };
static const Col C_A      = { 238, 96, 110 };
static const Col C_B      = { 250, 204, 84 };
static const Col C_SHLD   = { 70, 50, 104 };
static const Col C_ACCENT = { 150, 196, 255 };
static const Col C_INSP   = { 22, 24, 34 };
static const Col C_PANEL  = { 30, 34, 48 };
static const Col C_BTN    = { 52, 64, 96 };
static const Col C_BTN_HI = { 70, 96, 150 };

static Col mul(Col c, float f) { int r=(int)(c.r*f),g=(int)(c.g*f),b=(int)(c.b*f);
    if(r>255)r=255; if(g>255)g=255; if(b>255)b=255; Col o={(Uint8)r,(Uint8)g,(Uint8)b}; return o; }

/* ---- library ---- */
typedef struct { char dir[256], name[64]; } Game;
static Game   g_games[256];
static int    g_ngame, g_sel = -1;
static char   g_so[1024];
static time_t g_watch;
static char   g_status[160] = "select a game from the library";
static SDL_Thread *g_eng;

static int engine_thread(void *arg) { (void)arg;
    void *mod = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (!mod) { fprintf(stderr, "studio: dlopen: %s\n", dlerror()); return 1; }
    MoteGameRegisterFn reg = (MoteGameRegisterFn)dlsym(mod, "mote_game_register");
    const uint32_t *abi = (const uint32_t *)dlsym(mod, "mote_game_abi_version");
    if (!reg || !abi) { dlclose(mod); return 1; }
    MoteApi api; mote_api_fill(&api);
    const MoteGameVtbl *vt = reg(&api);
    if (vt) mote_os_run(&api, vt);
    dlclose(mod); return 0;
}
static void stop_engine(void) { if (!g_eng) return; mote_studio_request_quit(); SDL_WaitThread(g_eng, NULL); g_eng = NULL; }
static void start_engine(void) { mote_studio_reset(); g_eng = SDL_CreateThread(engine_thread, "engine", NULL); }

static time_t src_mtime(const char *dir) {
    char src[300]; snprintf(src, sizeof src, "%.250s/src", dir);
    DIR *d = opendir(src); if (!d) return 0; struct dirent *e; time_t m = 0;
    while ((e = readdir(d))) { size_t n = strlen(e->d_name);
        if (n > 2 && (!strcmp(e->d_name+n-2, ".c") || !strcmp(e->d_name+n-2, ".h"))) {
            char p[600]; snprintf(p, sizeof p, "%.300s/%.250s", src, e->d_name);
            struct stat st; if (stat(p, &st) == 0 && st.st_mtime > m) m = st.st_mtime; } }
    closedir(d); return m;
}
static void load_game(int idx, int rebuild) {
    if (idx < 0 || idx >= g_ngame) return;
    g_sel = idx;
    if (rebuild) { snprintf(g_status, sizeof g_status, "building %s...", g_games[idx].name);
        char cmd[700]; snprintf(cmd, sizeof cmd, "./tools/mote build %.250s >/dev/null 2>&1", g_games[idx].dir);
        snprintf(g_status, sizeof g_status, system(cmd) == 0 ? "running %s" : "BUILD FAILED: %s", g_games[idx].name); }
    snprintf(g_so, sizeof g_so, "%.200s/build/%.60s.so", g_games[idx].dir, g_games[idx].name);
    g_watch = src_mtime(g_games[idx].dir);
    stop_engine(); start_engine();
}

/* ---- cached text via the engine font ---- */
static struct { char s[48]; unsigned key; SDL_Texture *t; int w; } g_lc[256]; static int g_nlc;
static SDL_Texture *clabel(SDL_Renderer *R, const char *s, Col fg, Col bg, int *outw) {
    unsigned key = (fg.r<<16)^(fg.g<<8)^fg.b ^ ((unsigned)bg.r*131 + bg.g*17 + bg.b);
    for (int i = 0; i < g_nlc; i++) if (g_lc[i].key == key && !strcmp(g_lc[i].s, s)) { if(outw)*outw=g_lc[i].w; return g_lc[i].t; }
    static uint16_t buf[MOTE_FB_W * 9];
    uint16_t b565 = (uint16_t)MOTE_RGB565(bg.r, bg.g, bg.b);
    for (int i = 0; i < MOTE_FB_W * 9; i++) buf[i] = b565;
    mote_font_draw(buf, s, 0, 1, (uint16_t)MOTE_RGB565(fg.r, fg.g, fg.b));
    int w = mote_font_width(s); if (w < 1) w = 1; if (w > MOTE_FB_W) w = MOTE_FB_W;
    SDL_Texture *t = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, MOTE_FB_W, 9);
    SDL_UpdateTexture(t, NULL, buf, MOTE_FB_W * 2);
    if (g_nlc < 256) { snprintf(g_lc[g_nlc].s, 48, "%s", s); g_lc[g_nlc].key = key; g_lc[g_nlc].t = t; g_lc[g_nlc].w = w; g_nlc++; }
    if (outw) *outw = w; return t;
}
static void text(SDL_Renderer *R, const char *s, int x, int y, int scale, Col fg, Col bg) {
    int w; SDL_Texture *t = clabel(R, s, fg, bg, &w);
    SDL_Rect src = { 0, 0, w, 9 }, dst = { x, y, w*scale, 9*scale }; SDL_RenderCopy(R, t, &src, &dst);
}

static int cmp_game(const void *a, const void *b) { return strcmp(((const Game*)a)->name, ((const Game*)b)->name); }
static void scan_games(void) {
    DIR *d = opendir("examples"); if (!d) return; struct dirent *e;
    while ((e = readdir(d)) && g_ngame < 256) { if (e->d_name[0] == '.') continue;
        char p[400]; snprintf(p, sizeof p, "examples/%.200s/src/game.c", e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        Game *g = &g_games[g_ngame++];
        snprintf(g->dir, sizeof g->dir, "examples/%.240s", e->d_name);
        snprintf(g->name, sizeof g->name, "%.60s", e->d_name); }
    closedir(d); qsort(g_games, g_ngame, sizeof g_games[0], cmp_game);
}

/* ---- async action runner (Edit/Build/Push/Bake — don't freeze the UI) ---- */
static char *g_cmd_label;
static int action_thread(void *arg) {
    char *cmd = arg; int rc = system(cmd);
    snprintf(g_status, sizeof g_status, "%s %s", g_cmd_label, rc == 0 ? "done" : "FAILED");
    free(cmd); return 0;
}
static void run_async(const char *cmd, const char *label) {
    g_cmd_label = (char*)label; snprintf(g_status, sizeof g_status, "%s...", label);
    SDL_CreateThread(action_thread, "action", strdup(cmd));
}

/* ---- drawing primitives ---- */
static void plain(SDL_Renderer *R, int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(R, &r); }
static void round_rect(SDL_Renderer *R, int x, int y, int w, int h, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int j = 0; j < h; j++) { int in = 0, dy = -1;
        if (j < rad) dy = rad - j; else if (j >= h - rad) dy = j - (h - rad) + 1;
        if (dy >= 0) in = rad - (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, x + in, y + j, x + w - 1 - in, y + j); } }
static void disc(SDL_Renderer *R, int cx, int cy, int rad, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int dy = -rad; dy <= rad; dy++) { int dx = (int)sqrtf((float)(rad*rad - dy*dy));
        SDL_RenderDrawLine(R, cx - dx, cy + dy, cx + dx, cy + dy); } }
static void button(SDL_Renderer *R, int cx, int cy, int rad, Col base, int lit) {
    disc(R, cx, cy + 3, rad + 1, (Col){ 10, 11, 18 });
    if (lit) disc(R, cx, cy, rad + 4, mul(base, 0.9f));
    disc(R, cx, cy, rad, mul(base, 0.5f));
    disc(R, cx, cy, rad - 3, lit ? mul(base, 1.2f) : base);
    disc(R, cx - rad/3, cy - rad/3, rad/3, mul(lit ? mul(base,1.2f) : base, 1.5f)); }

/* ---- panes ---- */
static void draw_sidebar(SDL_Renderer *R) {
    SDL_Rect vp = { 0, 0, SIDEBAR_W, WIN_H }; SDL_RenderSetViewport(R, &vp);
    plain(R, 0, 0, SIDEBAR_W, WIN_H, C_SB);
    plain(R, 0, 0, SIDEBAR_W, 30, C_SB_HDR); plain(R, 0, 30, SIDEBAR_W, 2, C_ACCENT);
    text(R, "MOTE STUDIO", 8, 8, 2, C_TITLE, C_SB_HDR);
    text(R, "LIBRARY", 10, 38, 1, C_DIMTXT, C_SB);
    for (int i = 0; i < g_ngame; i++) { int y = 52 + i * ROW_H; if (y > WIN_H - 18) break;
        if (i == g_sel) plain(R, 0, y, SIDEBAR_W, ROW_H - 2, C_SEL);
        text(R, g_games[i].name, 10, y + 5, 2, i == g_sel ? C_TXT : C_DIMTXT, i == g_sel ? C_SEL : C_SB); }
}

static void draw_welcome(SDL_Renderer *R) {
    Col sbg = { 12, 16, 30 }; plain(R, SCR_X, SCR_Y, SCR, SCR, sbg);
    int w; clabel(R, "MOTE", C_TITLE, sbg, &w);
    text(R, "MOTE", SCR_X + (SCR - w*5)/2, SCR_Y + 78, 5, C_TITLE, sbg);
    clabel(R, "STUDIO", (Col){150,200,255}, sbg, &w);
    text(R, "STUDIO", SCR_X + (SCR - w*3)/2, SCR_Y + 124, 3, (Col){150,200,255}, sbg);
    clabel(R, "pick a game", (Col){120,140,170}, sbg, &w);
    text(R, "pick a game", SCR_X + (SCR - w*2)/2, SCR_Y + 172, 2, (Col){120,140,170}, sbg);
}

static void draw_device(SDL_Renderer *R, SDL_Texture *tex, const MoteButtons *b) {
    SDL_Rect vp = { SIDEBAR_W, 0, DEV_W, WIN_H }; SDL_RenderSetViewport(R, &vp);
    static uint16_t fr[MOTE_FB_W * MOTE_FB_H];
    mote_studio_get_frame(fr); SDL_UpdateTexture(tex, NULL, fr, MOTE_FB_W * (int)sizeof(uint16_t));

    /* GBA-style landscape body: a wide pill, purple, bevelled */
    int bx = 18, by = 126, bw = DEV_W - 36, bh = 326, br = 78;
    round_rect(R, bx, by + 6, bw, bh, br, C_BODY_LO);
    round_rect(R, bx, by, bw, bh - 8, br, C_BODY);
    round_rect(R, bx + 10, by + 6, bw - 20, 54, br - 10, C_BODY_HI);
    /* shoulder buttons on the top edge */
    round_rect(R, bx + 50, by - 6, 110, 26, 12, mul(C_SHLD, 0.7f));
    round_rect(R, bx + 52, by - 4, 106, 22, 10, b->lb ? C_ACCENT : C_SHLD);
    round_rect(R, bw + bx - 160, by - 6, 110, 26, 12, mul(C_SHLD, 0.7f));
    round_rect(R, bw + bx - 158, by - 4, 106, 22, 10, b->rb ? C_ACCENT : C_SHLD);

    /* screen: glossy bezel + the live fb at 2x */
    round_rect(R, SCR_X - 16, SCR_Y - 16, SCR + 32, SCR + 32, 14, C_BEZEL);
    round_rect(R, SCR_X - 16, SCR_Y - 16, SCR + 32, 9, 14, (Col){ 28, 30, 40 });
    plain(R, SCR_X - 2, SCR_Y - 2, SCR + 4, SCR + 4, (Col){ 40, 44, 56 });
    SDL_Rect dst = { SCR_X, SCR_Y, SCR, SCR };
    if (g_sel >= 0) SDL_RenderCopy(R, tex, NULL, &dst); else draw_welcome(R);

    /* d-pad (left bezel) */
    int dcx = (SCR_X) / 2 + 6, dcy = SCR_Y + SCR/2, dw = 32, al = 42;
    round_rect(R, dcx - al, dcy - dw/2, 2*al, dw, dw/2, C_DPAD);
    round_rect(R, dcx - dw/2, dcy - al, dw, 2*al, dw/2, C_DPAD);
    if (b->up)    plain(R, dcx-dw/2+4, dcy-al+4,  dw-8, al-dw/2, C_DPAD_L);
    if (b->down)  plain(R, dcx-dw/2+4, dcy+dw/2,  dw-8, al-dw/2-4, C_DPAD_L);
    if (b->left)  plain(R, dcx-al+4,   dcy-dw/2+4, al-dw/2, dw-8, C_DPAD_L);
    if (b->right) plain(R, dcx+dw/2,   dcy-dw/2+4, al-dw/2-4, dw-8, C_DPAD_L);
    disc(R, dcx, dcy, 6, mul(C_DPAD, 1.5f));
    /* A / B (right bezel, GBA side-by-side diagonal) */
    int rcx = SCR_X + SCR + (DEV_W - (SCR_X + SCR)) / 2 - 4;
    button(R, rcx + 22, dcy + 6, 24, C_A, b->a);
    button(R, rcx - 26, dcy + 22, 24, C_B, b->b);
    /* menu pill below the screen */
    round_rect(R, DEV_W/2 - 36, by + bh - 42, 72, 20, 10, mul(C_SHLD, 0.7f));
    round_rect(R, DEV_W/2 - 34, by + bh - 40, 68, 16, 8, b->menu ? C_ACCENT : C_SHLD);
}

/* ---- inspector with clickable actions ---- */
enum { A_EDIT, A_BUILD, A_RELOAD, A_PUSH, A_BAKE, A_N };
static SDL_Rect g_arect[A_N];
static const char *A_LABEL[A_N] = { "EDIT IN VSCODE", "BUILD", "RELOAD", "PUSH DEVICE", "BAKE ASSETS" };

static void draw_inspector(SDL_Renderer *R) {
    SDL_RenderSetViewport(R, NULL);
    plain(R, INSP_X, 0, INSP_W, WIN_H, C_INSP);
    plain(R, INSP_X, 0, INSP_W, 30, C_SB_HDR); plain(R, INSP_X, 30, INSP_W, 2, C_ACCENT);
    text(R, "INSPECTOR", INSP_X + 8, 8, 2, C_TITLE, C_SB_HDR);

    int x = INSP_X + 16, y = 46;
    if (g_sel < 0) { text(R, "no game open", x, y, 2, C_DIMTXT, C_INSP); }
    else {
        text(R, g_games[g_sel].name, x, y, 3, C_TXT, C_INSP); y += 30;
        text(R, g_games[g_sel].dir, x, y, 1, C_DIMTXT, C_INSP); y += 18;
    }

    /* action buttons (2 columns) */
    y = 96; int bw = (INSP_W - 40) / 2, bh = 34;
    for (int i = 0; i < A_N; i++) {
        int col = i & 1, row = i / 2;
        SDL_Rect r = { INSP_X + 16 + col * (bw + 8), y + row * (bh + 8), bw, bh };
        g_arect[i] = r;
        int en = (g_sel >= 0) || i == A_EDIT;
        round_rect(R, r.x, r.y, r.w, r.h, 8, en ? C_BTN : mul(C_BTN, 0.6f));
        round_rect(R, r.x, r.y, r.w, 2, 8, en ? C_BTN_HI : C_BTN);
        int tw; clabel(R, A_LABEL[i], C_TXT, C_BTN, &tw);
        text(R, A_LABEL[i], r.x + (r.w - tw*2)/2, r.y + (r.h - 16)/2, 2, en ? C_TXT : C_DIMTXT, en ? C_BTN : mul(C_BTN,0.6f));
    }

    /* status console */
    int cy = y + 3 * (bh + 8) + 14;
    text(R, "CONSOLE", x, cy, 1, C_DIMTXT, C_INSP); cy += 14;
    round_rect(R, INSP_X + 12, cy, INSP_W - 24, WIN_H - cy - 14, 8, (Col){ 12, 14, 20 });
    text(R, g_status, INSP_X + 20, cy + 8, 1, (Col){ 150, 230, 150 }, (Col){ 12, 14, 20 });
    text(R, "ARROWS d-pad  K=A J=B  SHIFT/SPACE L/R  ENTER menu", INSP_X + 20, cy + 22, 1, C_DIMTXT, (Col){12,14,20});
}

static void poll_input(MoteButtons *b, SDL_GameController *pad) {
    const Uint8 *k = SDL_GetKeyboardState(NULL); memset(b, 0, sizeof *b);
    b->up=k[SDL_SCANCODE_UP]||k[SDL_SCANCODE_W]; b->down=k[SDL_SCANCODE_DOWN]||k[SDL_SCANCODE_S];
    b->left=k[SDL_SCANCODE_LEFT]||k[SDL_SCANCODE_A]; b->right=k[SDL_SCANCODE_RIGHT]||k[SDL_SCANCODE_D];
    b->a=k[SDL_SCANCODE_K]||k[SDL_SCANCODE_PERIOD]; b->b=k[SDL_SCANCODE_J]||k[SDL_SCANCODE_COMMA];
    b->lb=k[SDL_SCANCODE_LSHIFT]; b->rb=k[SDL_SCANCODE_SPACE]; b->menu=k[SDL_SCANCODE_RETURN];
    if (pad) {
        b->up|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP); b->down|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        b->left|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT); b->right|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        b->a|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A); b->b|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_B);
        b->lb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_LEFTSHOULDER); b->rb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        b->menu|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_START); }
}

static void do_action(int a) {
    if (a == A_EDIT) { char c[400]; snprintf(c, sizeof c, "code %.250s >/dev/null 2>&1 &",
        g_sel >= 0 ? g_games[g_sel].dir : "."); run_async(c, "open VS Code"); return; }
    if (g_sel < 0) return;
    char dir[260]; snprintf(dir, sizeof dir, "%.250s", g_games[g_sel].dir);
    char c[500];
    if (a == A_BUILD)  { snprintf(c, sizeof c, "./tools/mote build %.250s --device", dir); run_async(c, "build"); }
    if (a == A_RELOAD) load_game(g_sel, 1);
    if (a == A_PUSH)   { snprintf(c, sizeof c, "./tools/mote push %.250s", dir); run_async(c, "push"); }
    if (a == A_BAKE)   { snprintf(c, sizeof c, "./tools/mote bake %.250s", dir); run_async(c, "bake"); }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    mote_plat_init("Mote Studio");
    scan_games();

    const char *shot = getenv("MOTE_STUDIO_SHOT");
    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Surface *surf = NULL;
    if (shot) { surf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_RGBA8888); ren = SDL_CreateSoftwareRenderer(surf); }
    else { win = SDL_CreateWindow("Mote Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC); }
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, MOTE_FB_W, MOTE_FB_H);
    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }

    const char *g0 = getenv("MOTE_STUDIO_GAME");   /* capture/test hook: preload a game */
    if (g0) { for (int i = 0; i < g_ngame; i++) if (!strcmp(g_games[i].name, g0)) { load_game(i, 1); break; }
        if (shot) SDL_Delay(700); }

    int running = 1, watch_tick = 0;
    do {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = e.button.x, my = e.button.y;
                if (mx < SIDEBAR_W) { int i = (my - 52) / ROW_H; if (i >= 0 && i < g_ngame && i != g_sel) load_game(i, 1); }
                else if (mx >= INSP_X) for (int i = 0; i < A_N; i++)
                    if (mx >= g_arect[i].x && mx < g_arect[i].x+g_arect[i].w && my >= g_arect[i].y && my < g_arect[i].y+g_arect[i].h) do_action(i);
            }
        }
        if (++watch_tick >= 30 && g_sel >= 0) { watch_tick = 0; time_t m = src_mtime(g_games[g_sel].dir);
            if (m > g_watch) { snprintf(g_status, sizeof g_status, "source changed, reloading..."); load_game(g_sel, 1); } }

        MoteButtons b; poll_input(&b, pad); mote_studio_set_buttons(&b);
        SDL_SetRenderDrawColor(ren, C_BG.r, C_BG.g, C_BG.b, 255); SDL_RenderClear(ren);
        draw_sidebar(ren); draw_device(ren, tex, &b); draw_inspector(ren);
        SDL_RenderPresent(ren);
        if (shot) { SDL_SaveBMP(surf, shot); printf("studio: wrote %s\n", shot); break; }
    } while (running);

    stop_engine();
    SDL_DestroyTexture(tex); if (ren) SDL_DestroyRenderer(ren); if (win) SDL_DestroyWindow(win); if (surf) SDL_FreeSurface(surf);
    SDL_Quit(); return 0;
}
