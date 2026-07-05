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
/* Frame-sequence recorder: MOTE_REC=/dir dumps every frame as /dir/NNNNN.ppm,
 * up to MOTE_REC_N frames (default 180), then quits. With MOTE_DT_MS (fixed
 * timestep) + MOTE_KEYS (scripted input) this records a deterministic gameplay
 * clip headlessly — stitch the PPMs with ffmpeg. */
static const char *s_rec_dir;
static int          s_rec_n = 180;

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
    s_rec_dir = getenv("MOTE_REC");
    if (getenv("MOTE_REC_N")) s_rec_n = atoi(getenv("MOTE_REC_N"));
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
    if (s_rec_dir) {
        if (s_frame < s_rec_n) {
            char p[1024]; snprintf(p, sizeof p, "%s/%05d.ppm", s_rec_dir, s_frame);
            dump_ppm(p, fb565);
        }
        if (++s_frame >= s_rec_n) { SDL_Log("mote_plat: recorded %d frames to %s", s_rec_n, s_rec_dir); s_quit = true; }
    } else if (s_shot_path && ++s_frame == s_shot_frame) {
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

/* Headless scripted input: MOTE_KEYS="a:5-15 lb:60-70 up:100-200" presses the
 * named button on input frames [start,end] inclusive (one frame == one buttons
 * read). Lets ported games be driven through their states with no display. */
static struct { uint8_t btn; int from, to; } s_keys[32];
static int  s_nkeys = -1;       /* -1 = not yet parsed */
static int  s_btn_frame;        /* incremented per buttons read */
static int  key_name(const char *s, int n) {
    struct { const char *n; uint8_t b; } map[] = {
        {"up",0},{"down",1},{"left",2},{"right",3},{"a",4},{"b",5},
        {"lb",6},{"rb",7},{"menu",8} };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if ((int)strlen(map[i].n) == n && !strncmp(map[i].n, s, n)) return map[i].b;
    return -1;
}
static void parse_keys(void) {
    s_nkeys = 0; s_btn_frame = 0;
    const char *e = getenv("MOTE_KEYS");
    if (!e) return;
    while (*e && s_nkeys < 32) {
        while (*e == ' ' || *e == ',') e++;
        const char *name = e;
        while (*e && *e != ':') e++;
        if (*e != ':') break;
        int b = key_name(name, (int)(e - name)); e++;
        int from = atoi(e); while (*e && *e != '-') e++;
        int to = (*e == '-') ? atoi(e + 1) : from;
        while (*e && *e != ' ' && *e != ',') e++;
        if (b >= 0) { s_keys[s_nkeys].btn = (uint8_t)b;
                      s_keys[s_nkeys].from = from; s_keys[s_nkeys].to = to; s_nkeys++; }
    }
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

    if (s_nkeys < 0) parse_keys();
    if (s_nkeys > 0) {
        bool *b = &out->up;   /* MoteButtons is 9 contiguous bools in btn order */
        for (int i = 0; i < s_nkeys; i++)
            if (s_btn_frame >= s_keys[i].from && s_btn_frame <= s_keys[i].to)
                b[s_keys[i].btn] = true;
    }
    s_btn_frame++;
}

uint64_t mote_plat_micros(void) {
    if (s_dt_us) return s_vclock;     /* fixed-timestep: constant within a frame */
    return (uint64_t)((SDL_GetPerformanceCounter() * 1000000ull) / s_freq);
}

void mote_plat_sleep_us(uint32_t us) {
    if (s_dt_us) return;              /* fixed-timestep capture: never wall-clock sleep */
    if (us >= 1000) SDL_Delay(us / 1000);
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

/* ---- ABI v23: rumble (no motor on the PC) + per-slot save (files in the cwd) ---- */
void mote_plat_rumble(float intensity, int ms) { (void)intensity; (void)ms; }

#define HOST_SAVE_SLOTS 8
static char s_save_game[40] = "";
void mote_plat_set_save_game(const char *stem) {
    if (!stem) { s_save_game[0] = 0; return; }
    int i = 0; for (; stem[i] && i < (int)sizeof(s_save_game) - 1; i++) s_save_game[i] = stem[i];
    s_save_game[i] = 0;
}
int mote_plat_save_slots(void) { return HOST_SAVE_SLOTS; }
static void host_save_path(int slot, char *p, int n) {
    if (s_save_game[0]) snprintf(p, n, "mote_save_%s_%d.bin", s_save_game, slot);
    else                snprintf(p, n, "mote_save%d.bin", slot);
}
int mote_plat_save(int slot, const void *data, int len) {
    if (slot < 0 || slot >= HOST_SAVE_SLOTS) return 0;
    char p[64]; host_save_path(slot, p, sizeof p);
    if (len <= 0) { remove(p); return 0; }
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    uint32_t L = (uint32_t)len; fwrite(&L, 4, 1, f); fwrite(data, 1, (size_t)len, f); fclose(f); return len;
}
int mote_plat_load(int slot, void *data, int max_len) {
    if (slot < 0 || slot >= HOST_SAVE_SLOTS) return 0;
    char p[64]; host_save_path(slot, p, sizeof p);
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
static void kv_hpath(const char *key, char *p, int n) {
    snprintf(p, n, "mote_kv/%s__%s", s_save_game[0] ? s_save_game : "game", key);
}
int mote_plat_kv_save(const char *key, const void *data, int len) {
    mkdir("mote_kv", 0777);
    char p[160]; kv_hpath(key, p, sizeof p);
    if (len <= 0) { remove(p); return 0; }
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    size_t w = fwrite(data, 1, (size_t)len, f); fclose(f); return (int)w;
}
int mote_plat_kv_load(const char *key, void *data, int max) {
    char p[160]; kv_hpath(key, p, sizeof p);
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

/* --- ABI v43: 2-player link — the host twin of the device's USB dual-role.
 *
 * Transport: a Unix stream socket at MOTE_LINK_SOCK (default /tmp/mote_link.sock).
 * Discovery mirrors the device flip: try to connect() to a waiting peer; if
 * nobody's there, bind+listen and wait. The listener side reports is_host=1
 * (the analogue of winning the USB host role). Everything is non-blocking and
 * pumped from mote_plat_link_task() once per frame. */
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int  s_lk_state;        /* MOTE_LINK_OFF/SEARCHING/CONNECTED (mote_api.h values) */
static int  s_lk_is_host;
static int  s_lk_fd = -1;      /* the connected pipe */
static int  s_lk_listen = -1;  /* listener while waiting (host side) */
static int  s_lk_bound;        /* we own the socket file (unlink on stop) */
static char s_lk_path[108];

static void lk_sock_path(struct sockaddr_un *a) {
    if (!s_lk_path[0]) {
        const char *p = getenv("MOTE_LINK_SOCK");
        snprintf(s_lk_path, sizeof s_lk_path, "%s", p ? p : "/tmp/mote_link.sock");
    }
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof a->sun_path, "%s", s_lk_path);
}

static void lk_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

static void lk_close_conn(void) {
    if (s_lk_fd >= 0) { close(s_lk_fd); s_lk_fd = -1; }
}

void mote_plat_link_start(void) {
    if (s_lk_state != 0) return;
    s_lk_state = 1;                       /* SEARCHING; task() does the work */
    s_lk_is_host = 0;
}

void mote_plat_link_stop(void) {
    lk_close_conn();
    if (s_lk_listen >= 0) { close(s_lk_listen); s_lk_listen = -1; }
    if (s_lk_bound) { unlink(s_lk_path); s_lk_bound = 0; }
    s_lk_state = 0;
    s_lk_is_host = 0;
}

void mote_plat_link_task(void) {
    if (s_lk_state == 0) return;

    if (s_lk_state == 2) {                /* connected: detect peer loss via EOF */
        char probe;
        ssize_t r = recv(s_lk_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) {                     /* orderly shutdown by the peer */
            lk_close_conn();
            s_lk_state = 1;
            s_lk_is_host = 0;
        }
        return;
    }

    /* SEARCHING. Host side: poll accept. */
    if (s_lk_listen >= 0) {
        int c = accept(s_lk_listen, NULL, NULL);
        if (c >= 0) {
            lk_nonblock(c);
            s_lk_fd = c; s_lk_is_host = 1; s_lk_state = 2;
        }
        return;
    }

    /* No role yet: try to connect to a waiting peer, else become the listener. */
    struct sockaddr_un addr; lk_sock_path(&addr);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        lk_nonblock(fd);
        s_lk_fd = fd; s_lk_is_host = 0; s_lk_state = 2;
        return;
    }
    close(fd);
    /* connect refused/absent: claim the listener role. A stale socket file from
     * a crashed run would make bind fail forever, so clear it first — but only
     * when connect() said nobody is home (ECONNREFUSED / ENOENT). */
    if (errno == ECONNREFUSED) unlink(addr.sun_path);
    int lf = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lf < 0) return;
    if (bind(lf, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(lf, 1) == 0) {
        lk_nonblock(lf);
        s_lk_listen = lf; s_lk_bound = 1;
    } else {
        close(lf);   /* EADDRINUSE: the peer just bound — connect() next frame */
    }
}

int mote_plat_link_status(void)  { return s_lk_state; }
int mote_plat_link_is_host(void) { return s_lk_state == 2 ? s_lk_is_host : 0; }

int mote_plat_link_send(const void *data, int len) {
    if (s_lk_state != 2 || len <= 0) return 0;
    ssize_t w = send(s_lk_fd, data, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w < 0) {
        if (errno == EPIPE || errno == ECONNRESET) { lk_close_conn(); s_lk_state = 1; }
        return 0;
    }
    return (int)w;
}

int mote_plat_link_recv(void *buf, int max) {
    if (s_lk_state != 2 || max <= 0) return 0;
    /* MOTE_LINK_FRAG=1: return adversarial 1..7-byte chunks so emulator tests
     * exercise device-CDC-like delivery (coalesced bursts / arbitrary splits) —
     * the exact conditions that expose framing and byte-loss bugs. */
    static int s_frag = -1;
    if (s_frag < 0) { const char *e = getenv("MOTE_LINK_FRAG"); s_frag = (e && e[0] && e[0] != '0') ? 1 : 0; }
    if (s_frag) { static unsigned rng = 0x1234u; rng = rng * 1664525u + 1013904223u;
                  int cap = 1 + (int)((rng >> 20) % 7); if (max > cap) max = cap; }
    ssize_t r = recv(s_lk_fd, buf, (size_t)max, MSG_DONTWAIT);
    /* MOTE_LINK_LOSS=N: drop roughly 1-in-N received bytes — simulates the
     * device-side ring-overflow loss so integrity layers can be tested. */
    { static int s_loss = -1;
      if (s_loss < 0) { const char *e = getenv("MOTE_LINK_LOSS"); s_loss = e ? atoi(e) : 0; }
      if (s_loss > 0 && r > 0) {
          static unsigned lr = 0x9e37u; uint8_t *p = (uint8_t *)buf; ssize_t o = 0;
          for (ssize_t i = 0; i < r; i++) {
              lr = lr * 1664525u + 1013904223u;
              if ((int)((lr >> 16) % (unsigned)s_loss) == 0) continue;   /* dropped */
              p[o++] = p[i];
          }
          r = o;
      } }
    if (r > 0) return (int)r;
    if (r == 0) { lk_close_conn(); s_lk_state = 1; s_lk_is_host = 0; }   /* EOF */
    return 0;
}
