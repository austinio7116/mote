/*
 * Mote OS — the online gallery, Android edition.
 *
 * The handheld's gallery screen asks a docked Studio for the manifest and the
 * .mote bytes over USB (os/device/lobby_main.c + the Studio's gal_serve_*). A
 * phone has no dock and can't run Cortex-M33 code, so it goes straight to the
 * source instead: fetch docs/games.json over HTTPS, read each game's Android
 * module entry for this ABI, download the .so, verify its sha256 and drop it in
 * the app's writable games dir — where the launcher picks it up ahead of the
 * copy baked into the APK, so a gallery install is also how you take an update.
 *
 * The manifest's `android` block is optional and additive; games published
 * without one show as "no module" and stay unavailable rather than breaking the
 * screen. Manifest shape (tools/gen_gallery.py):
 *
 *   "android": { "arm64-v8a": { "file": "...", "size": N, "sha256": "..." } }
 *
 * Downloads run on a worker thread; the UI stays live and shows progress from the
 * part-file's size, so a slow connection never looks like a hang.
 */
#include "mote_android_os.h"
#include "mote_platform.h"
#include "mote_launcher.h"
#include "mote_ui.h"
#include "mote_plat_android.h"
#include "mote_api.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef MOTE_GALLERY_BASE_DEFAULT
#define MOTE_GALLERY_BASE_DEFAULT "https://austinio7116.github.io/mote"
#endif

/* The ABI key this build looks for in the manifest. */
#if defined(__aarch64__)
#define MOTE_ANDROID_ABI "arm64-v8a"
#elif defined(__arm__)
#define MOTE_ANDROID_ABI "armeabi-v7a"
#elif defined(__x86_64__)
#define MOTE_ANDROID_ABI "x86_64"
#elif defined(__i386__)
#define MOTE_ANDROID_ABI "x86"
#else
#define MOTE_ANDROID_ABI "unknown"
#endif

#define GAL_MAX 128

typedef struct {
    char id[32], name[40], version[16], tag[64];
    int  abi;                       /* engine ABI the game needs */
    char file[200];                 /* module path, relative to the base URL */
    char sha256[68];
    long size;
    int  have_module;               /* the manifest has a module for our ABI */
} GalEnt;

static GalEnt s_g[GAL_MAX];
static int    s_n;
static char   s_base[200];
static char   s_err[80];

/* ------------------------------------------------------------------ sha256 *
 * A twin of the one in studio/gallery.c; the two live on different platforms
 * with no shared object file between them. */
typedef struct { uint32_t s[8]; uint64_t len; uint8_t buf[64]; int n; } sha256_t;
static const uint32_t K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_init(sha256_t *c) {
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(c->s, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha_block(sha256_t *ctx, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = ctx->s[0], b = ctx->s[1], c = ctx->s[2], d = ctx->s[3],
             e = ctx->s[4], f = ctx->s[5], g = ctx->s[6], h = ctx->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->s[0]+=a; ctx->s[1]+=b; ctx->s[2]+=c; ctx->s[3]+=d;
    ctx->s[4]+=e; ctx->s[5]+=f; ctx->s[6]+=g; ctx->s[7]+=h;
}
static void sha_update(sha256_t *ctx, const uint8_t *p, size_t n) {
    ctx->len += n;
    while (n--) { ctx->buf[ctx->n++] = *p++;
                  if (ctx->n == 64) { sha_block(ctx, ctx->buf); ctx->n = 0; } }
}
static void sha_final(sha256_t *ctx, char *hex) {
    uint64_t bits = ctx->len * 8;        /* captured before the padding updates */
    uint8_t pad = 0x80;
    sha_update(ctx, &pad, 1);
    uint8_t z = 0; while (ctx->n != 56) sha_update(ctx, &z, 1);
    uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - i*8));
    sha_update(ctx, L, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + i*8, 9, "%08x", ctx->s[i]);
}
static int sha_file(const char *path, char *hex) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    sha256_t c; sha_init(&c);
    uint8_t b[4096]; size_t r;
    while ((r = fread(b, 1, sizeof b, f)) > 0) sha_update(&c, b, r);
    fclose(f); sha_final(&c, hex); return 0;
}

/* ---------------------------------------------------------- JSON scraping *
 * The manifest is machine-generated with a fixed shape, so a targeted scan beats
 * a general parser here: find each game object, pull the handful of fields we
 * need, and treat anything missing as "not offered". */
static const char *obj_end(const char *p, const char *e) {
    int depth = 0, instr = 0;
    for (; p < e; p++) {
        if (instr) { if (*p == '\\') p++; else if (*p == '"') instr = 0; continue; }
        if (*p == '"') instr = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
    }
    return e;
}
/* Find "key" inside [b,e) and return the first char of its value. */
static const char *field(const char *b, const char *e, const char *key) {
    char pat[40]; int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    for (const char *p = b; p + n < e; p++) {
        if (memcmp(p, pat, (size_t)n) != 0) continue;
        p += n;
        while (p < e && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        return p;
    }
    return NULL;
}
static void field_str(const char *v, const char *e, char *out, int cap) {
    out[0] = 0;
    if (!v || v >= e || *v != '"') return;
    v++;
    int i = 0;
    while (v < e && *v != '"' && i < cap - 1) {
        if (*v == '\\' && v + 1 < e) v++;
        out[i++] = *v++;
    }
    out[i] = 0;
}
static long field_num(const char *v, const char *e) {
    if (!v || v >= e) return 0;
    return strtol(v, NULL, 10);
}

static int parse_manifest(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(s_err, sizeof s_err, "no manifest"); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); snprintf(s_err, sizeof s_err, "manifest size"); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); snprintf(s_err, sizeof s_err, "out of memory"); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    const char *end = buf + got;

    const char *games = field(buf, end, "games");
    if (!games || *games != '[') { free(buf); snprintf(s_err, sizeof s_err, "bad manifest"); return -1; }
    const char *p = games + 1;
    s_n = 0;
    while (p < end && s_n < GAL_MAX) {
        while (p < end && *p != '{' && *p != ']') p++;
        if (p >= end || *p == ']') break;
        const char *oe = obj_end(p, end);
        GalEnt *g = &s_g[s_n];
        memset(g, 0, sizeof *g);
        field_str(field(p, oe, "id"),      oe, g->id,      sizeof g->id);
        field_str(field(p, oe, "name"),    oe, g->name,    sizeof g->name);
        field_str(field(p, oe, "version"), oe, g->version, sizeof g->version);
        field_str(field(p, oe, "tag"),     oe, g->tag,     sizeof g->tag);
        g->abi = (int)field_num(field(p, oe, "abi"), oe);
        /* the per-ABI module block, if this game publishes one */
        const char *av = field(p, oe, "android");
        if (av && *av == '{') {
            const char *ae = obj_end(av, oe);
            const char *mv = field(av, ae, MOTE_ANDROID_ABI);
            if (mv && *mv == '{') {
                const char *me = obj_end(mv, ae);
                field_str(field(mv, me, "file"),   me, g->file,   sizeof g->file);
                field_str(field(mv, me, "sha256"), me, g->sha256, sizeof g->sha256);
                g->size = field_num(field(mv, me, "size"), me);
                g->have_module = g->file[0] != 0;
            }
        }
        if (g->id[0]) {
            if (!g->name[0]) snprintf(g->name, sizeof g->name, "%s", g->id);
            s_n++;
        }
        p = oe;
    }
    free(buf);
    if (s_n == 0) { snprintf(s_err, sizeof s_err, "no games in manifest"); return -1; }
    return 0;
}

/* ------------------------------------------------------------- work thread */
enum { JOB_IDLE = 0, JOB_FETCH, JOB_INSTALL, JOB_OK, JOB_FAIL };
static volatile int  s_job;
static volatile int  s_job_idx;
static SDL_Thread   *s_thread;
static char          s_cache[600];      /* manifest download path */
static char          s_part[600];       /* module download path (…​.part) */
static char          s_dest[600];

static void gal_paths(void) {
    const char *dir = mote_android_os_install_dir();
    snprintf(s_cache, sizeof s_cache, "%s/games.json", dir ? dir : ".");
}

static int fetch_thread(void *a) { (void)a;
    gal_paths();
    char url[300];
    snprintf(url, sizeof url, "%s/games.json", s_base);
    if (mote_shell_http_get(url, s_cache) != 0) {
        snprintf(s_err, sizeof s_err, "fetch failed - no network?");
        s_job = JOB_FAIL; return 0;
    }
    s_job = parse_manifest(s_cache) == 0 ? JOB_OK : JOB_FAIL;
    return 0;
}

static int install_thread(void *a) { (void)a;
    GalEnt *g = &s_g[s_job_idx];
    const char *dir = mote_android_os_install_dir();
    if (!dir) { snprintf(s_err, sizeof s_err, "nowhere to install"); s_job = JOB_FAIL; return 0; }
    snprintf(s_dest, sizeof s_dest, "%s/libmg_%s.so", dir, g->id);
    snprintf(s_part, sizeof s_part, "%s.part", s_dest);
    remove(s_part);

    char url[420];
    snprintf(url, sizeof url, "%s/%s", s_base, g->file);
    if (mote_shell_http_get(url, s_part) != 0) {
        snprintf(s_err, sizeof s_err, "download failed");
        remove(s_part); s_job = JOB_FAIL; return 0;
    }
    char hex[70] = {0};
    if (sha_file(s_part, hex) != 0) {
        snprintf(s_err, sizeof s_err, "cannot read download");
        remove(s_part); s_job = JOB_FAIL; return 0;
    }
    if (g->sha256[0] && strcasecmp(hex, g->sha256) != 0) {
        snprintf(s_err, sizeof s_err, "checksum mismatch");
        remove(s_part); s_job = JOB_FAIL; return 0;
    }
    remove(s_dest);
    if (rename(s_part, s_dest) != 0) {
        snprintf(s_err, sizeof s_err, "cannot save module");
        remove(s_part); s_job = JOB_FAIL; return 0;
    }
    /* Hand it to the launcher's catalog straight away — no relaunch needed. */
    if (mote_android_os_add_module(s_dest) != 0)
        snprintf(s_err, sizeof s_err, "installed, needs a restart");
    s_job = JOB_OK;
    return 0;
}

static void job_start(int (*fn)(void *), int state, int idx) {
    if (s_thread) { SDL_WaitThread(s_thread, NULL); s_thread = NULL; }
    s_err[0] = 0; s_job_idx = idx; s_job = state;
    s_thread = SDL_CreateThread(fn, "gallery", NULL);
    if (!s_thread) { snprintf(s_err, sizeof s_err, "no thread"); s_job = JOB_FAIL; }
}
static void job_reap(void) {
    if (s_thread && (s_job == JOB_OK || s_job == JOB_FAIL)) {
        SDL_WaitThread(s_thread, NULL); s_thread = NULL;
    }
}
static long file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : 0;
}

/* -------------------------------------------------------------------- state */
enum { ST_NONE = 0, ST_INSTALLED, ST_UPDATE, ST_GET, ST_NOMOD, ST_ABI };
static int ent_state(const GalEnt *g) {
    const char *inst = mote_android_os_installed_version(g->id);
    if (!g->have_module) return inst ? ST_INSTALLED : ST_NOMOD;
    if (g->abi > (int)MOTE_ABI_VERSION) return ST_ABI;
    if (!inst) return ST_GET;
    /* dotted-numeric compare, same rule as the Studio's gallery_vercmp */
    const char *a = inst, *b = g->version;
    for (;;) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return y > x ? ST_UPDATE : ST_INSTALLED;
        while (*a == '.') a++;
        while (*b == '.') b++;
        if (!*a && !*b) return ST_INSTALLED;
        if (!*a) return ST_UPDATE;
        if (!*b) return ST_INSTALLED;
    }
}
static const char *state_tag(int st) {
    switch (st) {
    case ST_GET:       return "GET";
    case ST_UPDATE:    return "UPDATE";
    case ST_INSTALLED: return "OK";
    case ST_ABI:       return "NEW ENGINE";
    default:           return "-";
    }
}

void mote_android_gallery_set_base(const char *base) {
    snprintf(s_base, sizeof s_base, "%s", (base && base[0]) ? base : MOTE_GALLERY_BASE_DEFAULT);
    size_t n = strlen(s_base);
    while (n && s_base[n - 1] == '/') s_base[--n] = 0;
}

void mote_android_gallery_screen(void) {
    uint16_t *fb = mote_launcher_fb();
    MoteInput in; memset(&in, 0, sizeof in);
    { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
    uint64_t last = mote_plat_micros();
    int sel = 0, top = 0;
    if (!s_base[0]) mote_android_gallery_set_base(NULL);
    if (s_n == 0) job_start(fetch_thread, JOB_FETCH, 0);

    for (;;) {
        uint64_t now = mote_plat_micros();
        uint32_t dt = (uint32_t)((now - last) / 1000); last = now;
        MoteButtons raw; mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, dt);
        mote_plat_audio_pump();
        job_reap();
        if (mote_plat_should_quit()) return;

        int busy = (s_job == JOB_FETCH || s_job == JOB_INSTALL);

        if (!busy) {
            if (mote_just_pressed(&in, MOTE_BTN_B)) return;
            if (s_n > 0) {
                if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % s_n;
                if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel + s_n - 1) % s_n;
                if (mote_just_pressed(&in, MOTE_BTN_RB))   job_start(fetch_thread, JOB_FETCH, 0);
                if (mote_just_pressed(&in, MOTE_BTN_A)) {
                    int st = ent_state(&s_g[sel]);
                    if (st == ST_GET || st == ST_UPDATE) job_start(install_thread, JOB_INSTALL, sel);
                }
            } else if (mote_just_pressed(&in, MOTE_BTN_A)) {
                job_start(fetch_thread, JOB_FETCH, 0);
            }
        }

        /* ---- draw ---- */
        mote_ui_ground(fb);
        if (busy) {
            const char *t = s_job == JOB_FETCH ? "GALLERY" : s_g[s_job_idx].name;
            mote_ui_header(fb, t, -1, -1);
            const char *msg = s_job == JOB_FETCH ? "FETCHING..." : "DOWNLOADING...";
            mote_ui_read(fb, msg, (MOTE_FB_W - mote_ui_read_w(msg)) / 2, 40, MOTE_UI_GOLD);
            if (s_job == JOB_INSTALL && s_g[s_job_idx].size > 0) {
                long have = file_size(s_part);
                int pct = (int)(have * 100 / s_g[s_job_idx].size);
                if (pct > 100) pct = 100;
                mote_draw_rect(fb, 12, 66, 104, 10, MOTE_UI_BAR, 1, 0, MOTE_FB_H);
                if (pct > 0) mote_draw_rect(fb, 13, 67, 102 * pct / 100, 8, MOTE_UI_ACCENT, 1, 0, MOTE_FB_H);
                char pc[16]; snprintf(pc, sizeof pc, "%d%%", pct);
                mote_ui_text(fb, pc, (MOTE_FB_W - mote_ui_text_w(pc)) / 2, 82, MOTE_UI_TEXT);
            }
            mote_ui_footer(fb, "PLEASE WAIT");
        } else if (s_n == 0) {
            mote_ui_header(fb, "GALLERY", -1, -1);
            mote_ui_text(fb, s_err[0] ? s_err : "no games", 4, 44, MOTE_UI_TEXT);
            mote_ui_footer(fb, "A RETRY   B BACK");
        } else {
            char rows[GAL_MAX][34];
            const char *items[GAL_MAX];
            for (int i = 0; i < s_n; i++) {
                int st = ent_state(&s_g[i]);
                if (st == ST_INSTALLED) snprintf(rows[i], sizeof rows[i], "%.20s", s_g[i].name);
                else snprintf(rows[i], sizeof rows[i], "%.14s  %s", s_g[i].name, state_tag(st));
                items[i] = rows[i];
            }
            mote_ui_header(fb, "GALLERY", sel + 1, s_n);
            top = mote_ui_list(fb, items, s_n, sel, top, 22);
            char foot[64];
            const char *inst = mote_android_os_installed_version(s_g[sel].id);
            if (s_err[0])                snprintf(foot, sizeof foot, "%.30s", s_err);
            else if (!s_g[sel].have_module) snprintf(foot, sizeof foot, "no " MOTE_ANDROID_ABI " module yet");
            else if (inst)               snprintf(foot, sizeof foot, "v%.8s -> v%.8s", inst, s_g[sel].version);
            else                         snprintf(foot, sizeof foot, "v%.8s  %ld KB", s_g[sel].version, s_g[sel].size / 1024);
            mote_ui_footer(fb, foot);
        }
        mote_plat_present(fb);
    }
}
