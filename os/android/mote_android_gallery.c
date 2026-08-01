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
#include <math.h>

#include "mote_2d.h"        /* mote_draw_rect for the progress bar */
#include "stb_image.h"      /* implementation lives in platform/android/mote_shell.c */

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
    char id[32], name[40], version[16], tag[64], author[32];
    int  abi;                       /* engine ABI the game needs */
    int  mp;                        /* multiplayer flag, for the device's list */
    char file[200];                 /* our own module path, relative to the base */
    char sha256[68];
    long size;
    int  have_module;               /* the manifest has a module for our ABI */
    /* The handheld's artifact. A docked device wants the .mote, not our .so, so
     * the dock serves these; they are in the same manifest either way. */
    char mote_file[80];
    char mote_sha[68];
    long mote_size;
    /* Screenshot paths, for the thumbnails the device's gallery screen asks for. */
    char shot[4][72];
    int  nshots;
    /* The description is up to ~2 KB per game, which is not worth keeping 31 of
     * in memory: record where it sits in the cached manifest and read the slice
     * back off disk when the device actually asks. */
    long desc_off, desc_len;
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
static int sha_file(const char *path, char *hex);

/* Also the dock's verifier: a downloaded .mote must match the manifest before it
 * is handed to a handheld that will run it in place from flash. */
int mote_gallery_sha256_file(const char *path, char *hex) { return sha_file(path, hex); }

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
        field_str(field(p, oe, "author"),  oe, g->author,  sizeof g->author);
        g->abi = (int)field_num(field(p, oe, "abi"), oe);
        /* the handheld's artifact, and the metadata its own gallery screen shows */
        field_str(field(p, oe, "file"),    oe, g->mote_file, sizeof g->mote_file);
        field_str(field(p, oe, "sha256"),  oe, g->mote_sha,  sizeof g->mote_sha);
        g->mote_size = field_num(field(p, oe, "size"), oe);
        { const char *mv = field(p, oe, "multiplayer");
          g->mp = (mv && mv < oe && *mv == 't') ? 1 : 0; }
        /* screenshots: a JSON array of paths, first four kept */
        { const char *sv = field(p, oe, "screenshots");
          if (sv && *sv == '[') {
              const char *se = obj_end(sv, oe);
              const char *q = sv;
              while (g->nshots < 4) {
                  q = memchr(q, '"', (size_t)(se - q));
                  if (!q) break;
                  field_str(q, se, g->shot[g->nshots], sizeof g->shot[0]);
                  if (!g->shot[g->nshots][0]) break;
                  g->nshots++;
                  q += 1 + strlen(g->shot[g->nshots - 1]) + 1;   /* past the close quote */
                  if (q >= se) break;
              }
          } }
        /* description: remember the slice rather than copying it */
        { const char *dv = field(p, oe, "desc");
          if (dv && *dv == '"') {
              const char *d = dv + 1, *q = d;
              while (q < oe && *q != '"') { if (*q == '\\' && q + 1 < oe) q++; q++; }
              g->desc_off = (long)(d - buf);
              g->desc_len = (long)(q - d);
          } }
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

const char *gal_cache_dir(void) {
    static char d[600];
    if (!d[0]) {
        const char *base = mote_android_os_install_dir();
        snprintf(d, sizeof d, "%s/dockcache", base ? base : ".");
        mkdir(d, 0770);
    }
    return d;
}

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
/* ================================================================== *
 *  Accessors for the USB dock (os/android/mote_android_dock.c)
 *
 *  A docked handheld asks for the same catalogue this screen shows, but wants
 *  the .mote artifacts and its own wire format. Rather than parse the manifest
 *  twice, the dock reads it through here.
 * ================================================================== */

/* Fetch + parse the manifest if it is not loaded yet. Blocking — dock thread
 * only. 0 on success. */
int mote_gallery_ensure(void) {
    if (s_n > 0) return 0;
    if (!s_base[0]) mote_android_gallery_set_base(NULL);
    gal_paths();
    char url[300];
    snprintf(url, sizeof url, "%s/games.json", s_base);
    /* A cached copy is good enough if the network is not there right now. */
    if (mote_shell_http_get(url, s_cache) != 0 && !file_size(s_cache)) {
        snprintf(s_err, sizeof s_err, "no manifest");
        return -1;
    }
    return parse_manifest(s_cache);
}

int mote_gallery_count(void) { return s_n; }

/* One GMANIFEST line, in the wire format os/device/lobby_main.c parses:
 *   MN1 GAME <idx> <abi> <mp> <nshots> <size> <ver> <fname>|<author>|<name>
 * fname is the .mote basename without its extension — the name the device
 * stores the file under. Only <name> may contain spaces, so it goes last. */
int mote_gallery_manifest_line(int i, char *out, int cap) {
    if (i < 0 || i >= s_n || !out) return -1;
    const GalEnt *g = &s_g[i];
    /* "games/Moita.mote" -> "Moita" */
    const char *b = strrchr(g->mote_file, '/');
    b = b ? b + 1 : g->mote_file;
    char base[80];
    snprintf(base, sizeof base, "%s", b);
    char *dot = strstr(base, ".mote");
    if (dot) *dot = 0;
    if (!base[0]) return -1;
    snprintf(out, cap, "MN1 GAME %d %d %d %d %ld %s %s|%s|%s\n",
             i, g->abi, g->mp, g->nshots ? g->nshots : 1, g->mote_size,
             g->version[0] ? g->version : "0", base,
             g->author[0] ? g->author : "unknown", g->name);
    return 0;
}

int mote_gallery_shot_url(int i, int shot, char *out, int cap) {
    if (i < 0 || i >= s_n || !out) return -1;
    const GalEnt *g = &s_g[i];
    if (g->nshots <= 0) return -1;
    if (shot < 0 || shot >= g->nshots) shot = 0;
    snprintf(out, cap, "%s/%s", s_base, g->shot[shot]);
    return 0;
}

int mote_gallery_mote_url(int i, char *out, int cap) {
    if (i < 0 || i >= s_n || !out || !s_g[i].mote_file[0]) return -1;
    snprintf(out, cap, "%s/%s", s_base, s_g[i].mote_file);
    return 0;
}

const char *mote_gallery_mote_sha(int i) {
    return (i >= 0 && i < s_n) ? s_g[i].mote_sha : "";
}
long mote_gallery_mote_size(int i) {
    return (i >= 0 && i < s_n) ? s_g[i].mote_size : 0;
}

/* Read a description back out of the cached manifest. Returns the number of
 * bytes written, and un-escapes the two sequences the generator can emit. */
long mote_gallery_desc(int i, char *out, int cap) {
    if (i < 0 || i >= s_n || !out || cap <= 0) return 0;
    const GalEnt *g = &s_g[i];
    if (g->desc_len <= 0) { out[0] = 0; return 0; }
    FILE *f = fopen(s_cache, "rb");
    if (!f) { out[0] = 0; return 0; }
    long want = g->desc_len < cap - 1 ? g->desc_len : cap - 1;
    fseek(f, g->desc_off, SEEK_SET);
    size_t got = fread(out, 1, (size_t)want, f);
    fclose(f);
    /* JSON escapes: the manifest is generated with ensure_ascii=False, so only
     * backslash pairs appear. Collapse them so the device shows real text. */
    long o = 0;
    for (size_t k = 0; k < got; k++) {
        if (out[k] == '\\' && k + 1 < got) {
            k++;
            out[o++] = out[k] == 'n' ? '\n' : out[k] == 't' ? ' ' : out[k];
        } else out[o++] = out[k];
    }
    out[o] = 0;
    return o;
}

void mote_android_gallery_set_base(const char *base) {
    snprintf(s_base, sizeof s_base, "%s", (base && base[0]) ? base : MOTE_GALLERY_BASE_DEFAULT);
    size_t n = strlen(s_base);
    while (n && s_base[n - 1] == '/') s_base[--n] = 0;
}


/* ================================================================== *
 *  The gallery screen
 *
 *  A port of the handheld's own gallery (os/device/lobby_main.c gg_draw): the
 *  same hero layout, the same chips, the same attract-mode screenshot
 *  slideshow, the same LB description modal. It should be the same screen —
 *  only where the data comes from differs, and on a phone that is HTTPS instead
 *  of MN1 over a cable, which the player should never be able to tell.
 * ================================================================== */

/* ---- screenshot tiles ----------------------------------------------------
 * A published shot is a 2-4 KB PNG, but there are ~100 of them and each one
 * arrives over the phone's mobile connection, so the cost that matters is the
 * round trip, not the bytes. Two things keep that off the critical path: every
 * decoded tile is written to disk (8 KB of RGB565, so a revisit — or a later
 * run of the app — is a file read), and a background thread walks the whole
 * catalogue filling that cache while the player is still reading the first
 * page. Java's connection pool does the rest; see moteHttpGet.
 * -------------------------------------------------------------------------- */
static void tile_path(int idx, int shot, char *out, int cap) {
    snprintf(out, cap, "%s/tb_%.24s_%d.tile", gal_cache_dir(),
             idx >= 0 && idx < s_n ? s_g[idx].id : "x", shot);
}
static int tile_load(const char *path, uint16_t *out64) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out64, 1, 64 * 64 * sizeof(uint16_t), f);
    fclose(f);
    return n == 64 * 64 * sizeof(uint16_t) ? 0 : -1;
}
static void tile_store(const char *path, const uint16_t *in64) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(in64, 1, 64 * 64 * sizeof(uint16_t), f);
    fclose(f);
}

/* Decode a published screenshot into a 64x64 RGB565 tile. The gallery's shots
 * are 128x128, so this is a clean 2:1 point sample. Shared with the USB dock,
 * which serves exactly these bytes to a docked handheld. */
int mote_gallery_thumb(int idx, int shot, uint16_t *out64) {
    char url[400], path[700], tile[700];
    memset(out64, 0, 64 * 64 * sizeof(uint16_t));
    if (mote_gallery_shot_url(idx, shot, url, sizeof url) != 0) return -1;

    tile_path(idx, shot, tile, sizeof tile);
    if (tile_load(tile, out64) == 0) return 0;              /* already have it */

    /* The UI thread and the prefetcher can both be downloading; give each call
     * its own scratch name so one cannot truncate the other's file. */
    static SDL_atomic_t tag;
    snprintf(path, sizeof path, "%s/dl%d.png", gal_cache_dir(),
             SDL_AtomicAdd(&tag, 1) & 7);
    if (mote_shell_http_get(url, path) != 0) return -1;

    long len = 0;
    unsigned char *png = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len > 0 && len < 4 * 1024 * 1024 && (png = malloc((size_t)len)))
            if (fread(png, 1, (size_t)len, f) != (size_t)len) { free(png); png = NULL; }
        fclose(f);
    }
    remove(path);
    if (!png) return -1;
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(png, (int)len, &w, &h, &comp, 4);
    free(png);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return -1; }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const unsigned char *s = px + ((size_t)(y * h / 64) * w + (x * w / 64)) * 4;
            out64[y * 64 + x] = (uint16_t)(((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3));
        }
    stbi_image_free(px);
    tile_store(tile, out64);
    return 0;
}

/* ---- background prefetch -------------------------------------------------
 * Walks the catalogue from wherever the player is standing, outwards, caching
 * every shot it does not already have. It never touches the UI's buffers — it
 * only warms the disk cache, so the worst it can do is finish late. */
static SDL_Thread  *s_pf_thread;
static volatile int s_pf_stop, s_pf_from;

static int prefetch_thread(void *a) { (void)a;
    static uint16_t tmp[64 * 64];
    for (int step = 0; step < s_n && !s_pf_stop; step++) {
        int i = (s_pf_from + step) % s_n;
        for (int sh = 0; sh < s_g[i].nshots && !s_pf_stop; sh++) {
            char tile[700];
            tile_path(i, sh, tile, sizeof tile);
            if (tile_load(tile, tmp) == 0) continue;
            mote_gallery_thumb(i, sh, tmp);
            if (s_pf_stop) break;
            SDL_Delay(15);            /* leave the UI's fetch room to get in front */
        }
    }
    return 0;
}
static void prefetch_start(int from) {
    if (s_pf_thread || s_n <= 0) return;
    s_pf_stop = 0; s_pf_from = from;
    s_pf_thread = SDL_CreateThread(prefetch_thread, "galprefetch", NULL);
}
static void prefetch_stop(void) {
    if (!s_pf_thread) return;
    s_pf_stop = 1;
    SDL_WaitThread(s_pf_thread, NULL);
    s_pf_thread = NULL;
}

/* Thumbnails load on a worker so scrolling the list never stalls on the network.
 * TB_SLOTS of them stay decoded: enough that scrolling back over the last screen
 * of games is instant, small enough to be a rounding error against the heap. */
#define TB_SLOTS 8
static uint16_t s_tb[TB_SLOTS][64 * 64];
static int      s_tb_game[TB_SLOTS], s_tb_shot[TB_SLOTS];
static unsigned s_tb_use[TB_SLOTS], s_tb_clock;

static void tb_init(void) {
    for (int i = 0; i < TB_SLOTS; i++) { s_tb_game[i] = -1; s_tb_shot[i] = -1; s_tb_use[i] = 0; }
    s_tb_clock = 0;
}
static int tb_find(int g, int sh) {
    for (int i = 0; i < TB_SLOTS; i++)
        if (s_tb_game[i] == g && s_tb_shot[i] == sh) { s_tb_use[i] = ++s_tb_clock; return i; }
    return -1;
}
static int tb_victim(void) {          /* least recently shown */
    int v = 0;
    for (int i = 1; i < TB_SLOTS; i++) if (s_tb_use[i] < s_tb_use[v]) v = i;
    return v;
}

static SDL_Thread  *s_th_thread;
static volatile int s_th_busy, s_th_done;
static int          s_th_slot, s_th_game = -1, s_th_shot = -1;

static int thumb_thread(void *a) { (void)a;
    if (mote_gallery_thumb(s_th_game, s_th_shot, s_tb[s_th_slot]) != 0)
        memset(s_tb[s_th_slot], 0, sizeof s_tb[0]);
    s_th_done = 1; s_th_busy = 0;
    return 0;
}
static void th_pump(void) {
    if (!s_th_done) return;
    s_th_done = 0;
    if (s_th_thread) { SDL_WaitThread(s_th_thread, NULL); s_th_thread = NULL; }
    s_tb_game[s_th_slot] = s_th_game; s_tb_shot[s_th_slot] = s_th_shot;
    s_tb_use[s_th_slot] = ++s_tb_clock;
}
/* Fetch what is on screen; once that is in, quietly fetch the next shot so the
 * slideshow always has somewhere to go. */
static void th_schedule(int game, int shot, int nshots) {
    if (s_th_busy) return;
    int want = shot;
    if (tb_find(game, shot) >= 0) {
        if (nshots < 2) return;
        want = (shot + 1) % nshots;
        if (tb_find(game, want) >= 0) return;
    }
    int slot = tb_victim();
    if (s_tb_game[slot] == game && s_tb_shot[slot] == shot) {   /* never evict the visible tile */
        s_tb_use[slot] = ++s_tb_clock;
        slot = tb_victim();
    }
    s_tb_game[slot] = -1; s_tb_shot[slot] = -1;
    s_th_slot = slot; s_th_game = game; s_th_shot = want;
    s_th_busy = 1; s_th_done = 0;
    s_th_thread = SDL_CreateThread(thumb_thread, "galthumb", NULL);
    if (!s_th_thread) s_th_busy = 0;
}

static void fillrect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= MOTE_FB_H) continue;
        for (int i = x; i < x + w; i++) {
            if ((unsigned)i >= MOTE_FB_W) continue;
            fb[j * MOTE_FB_W + i] = c;
        }
    }
}
static void human_size(long b, char *out, int n) {
    if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024.0));
    else if (b >= 1024)   snprintf(out, n, "%ld KB", b / 1024);
    else                  snprintf(out, n, "%ld B", b);
}
static void gg_spinner(uint16_t *fb, int cx, int cy) {
    int ph = (int)((mote_plat_micros() / 90000) % 8);
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.7853981f;
        int x = cx + (int)(9.0f * cosf(a)), y = cy + (int)(9.0f * sinf(a));
        int d = (i - ph + 8) % 8;
        uint16_t c = d < 2 ? MOTE_UI_TEXT : d < 4 ? MOTE_UI_ACCENT : MOTE_UI_BAR;
        fillrect(fb, x - 1, y - 1, 3, 3, c);
    }
}

/* The title, always large and one line; if it is wider than the screen it rocks
 * back and forth as a ticker rather than wrapping. */
static void gg_title(uint16_t *fb, const char *nm, int y) {
    int tw = mote_ui_title_w(nm);
    if (tw <= MOTE_FB_W - 6) { mote_ui_title(fb, nm, (MOTE_FB_W - tw) / 2, y, 0xFFFF); return; }
    int margin = 4, span = tw - (MOTE_FB_W - 2 * margin);
    int travel = span * 14, pause = 800, cyc = 2 * (pause + travel);
    int ph = (int)((mote_plat_micros() / 1000) % (uint64_t)cyc), off;
    if      (ph < pause)              off = 0;
    else if (ph < pause + travel)     off = (ph - pause) * span / travel;
    else if (ph < 2 * pause + travel) off = span;
    else                              off = span - (ph - 2 * pause - travel) * span / travel;
    mote_ui_title(fb, nm, margin - off, y, 0xFFFF);
}

/* HERO view — the same furniture as the handheld's: framed 64x64 shot, dots,
 * state chip left, 2P chip right, browse arrows, big title, version + size. */
static void gg_draw(uint16_t *fb, int sel, int shot, const uint16_t *thumb,
                    int msg_t, const char *msg) {
    const GalEnt *g = &s_g[sel];
    int st = ent_state(g);
    mote_ui_ground(fb);
    mote_ui_header(fb, "GALLERY", sel + 1, s_n);

    const int TS = 64;
    int tx = (MOTE_FB_W - TS) / 2, ty = 20;
    for (int x = tx - 2; x < tx + TS + 2; x++) {
        fb[(ty - 2) * MOTE_FB_W + x] = 0x18E3;
        fb[(ty + TS + 1) * MOTE_FB_W + x] = 0x18E3;
    }
    for (int y = ty - 2; y < ty + TS + 2; y++) {
        fb[y * MOTE_FB_W + tx - 2] = 0x18E3;
        fb[y * MOTE_FB_W + tx + TS + 1] = 0x18E3;
    }
    if (thumb) {
        for (int y = 0; y < TS; y++)
            for (int x = 0; x < TS; x++) fb[(ty + y) * MOTE_FB_W + tx + x] = thumb[y * TS + x];
    } else {
        fillrect(fb, tx, ty, TS, TS, 0x0841);
        gg_spinner(fb, tx + TS / 2, ty + TS / 2);
    }
    if (g->nshots > 1) {                              /* screenshot dots */
        int d = g->nshots > 8 ? 8 : g->nshots, gap = 6;
        int x0 = tx + TS / 2 - (d * gap - 2) / 2, y = ty + TS - 6;
        for (int i = 0; i < d; i++)
            fillrect(fb, x0 + i * gap, y, 3, 3, i == shot ? 0xFFFF : 0x39C7);
    }
    {   /* state chip at the left margin */
        const char *tag = st == ST_UPDATE ? "UPD" : st == ST_INSTALLED ? "OK" :
                          st == ST_GET ? "NEW" : st == ST_ABI ? "ENG" : "--";
        uint16_t bg = st == ST_UPDATE ? 0x8200 : st == ST_INSTALLED ? 0x02E0 : 0x0269;
        uint16_t fg = st == ST_UPDATE ? 0xFE60 : st == ST_INSTALLED ? 0x9FF4 : 0x9EFF;
        int bw = mote_ui_text_w(tag) + 4;
        fillrect(fb, 1, ty, bw, 13, bg);
        mote_ui_text(fb, tag, 3, ty + 1, fg);
    }
    if (g->mp) {                                      /* 2P chip at the right */
        int bw = mote_ui_text_w("2P") + 4, cx = MOTE_FB_W - bw - 1;
        fillrect(fb, cx, ty, bw, 13, 0x2A4A);
        mote_ui_text(fb, "2P", cx + 2, ty + 1, 0x8FF4);
    }
    {   /* browse arrows either side of the shot */
        int ay = ty + TS / 2 - mote_ui_title_h() / 2;
        mote_ui_title(fb, "<", 3, ay, 0xACD3);
        mote_ui_title(fb, ">", MOTE_FB_W - mote_ui_title_w(">") - 3, ay, 0xACD3);
    }
    gg_title(fb, g->name, 87);

    if (msg_t > 0) {
        mote_ui_text(fb, msg, (MOTE_FB_W - mote_ui_text_w(msg)) / 2, 104, 0x07E0);
    } else {
        char sz[16], info[32];
        /* The size is the module this phone would download; with nothing
         * published for this ABI there is no number to show, so show none
         * rather than a meaningless zero. */
        if (g->have_module) {
            human_size(g->size, sz, sizeof sz);
            snprintf(info, sizeof info, "v%.8s   %s", g->version, sz);
        } else snprintf(info, sizeof info, "v%.8s", g->version);
        mote_ui_text(fb, info, (MOTE_FB_W - mote_ui_text_w(info)) / 2, 104, 0x9CD3);
        const char *act = st == ST_INSTALLED ? "reinstall" : st == ST_UPDATE ? "update" :
                          st == ST_GET ? "install" : NULL;
        char ft[44];
        if (act) snprintf(ft, sizeof ft, "A %s  LB info  B", act);
        else     snprintf(ft, sizeof ft, "%s  LB info  B",
                          st == ST_ABI ? "needs a newer app" : "no " MOTE_ANDROID_ABI);
        mote_ui_text(fb, ft, (MOTE_FB_W - mote_ui_text_w(ft)) / 2, 116, 0x8410);
    }
}

/* LB — the description, word-wrapped to the reading font and scrollable. The
 * text is already on the phone (it came down with the manifest), so unlike the
 * handheld's version there is nothing to fetch. */
static void gg_desc_modal(uint16_t *fb, int idx) {
    static char desc[2600];
    if (mote_gallery_desc(idx, desc, (int)sizeof desc) <= 0)
        snprintf(desc, sizeof desc, "(no description available)");

    char lines[64][40]; int nl = 0;
    const char *p = desc; const int MAXW = MOTE_FB_W - 10;
    while (*p && nl < 64) {
        int end = 0, brk = -1;
        while (p[end] && p[end] != '\n') {
            char t[40]; int n;
            if (p[end] == ' ') {
                n = end < 39 ? end : 39; memcpy(t, p, (size_t)n); t[n] = 0;
                if (mote_ui_read_w(t) > MAXW) break;
                brk = end;
            }
            n = (end + 1) < 39 ? (end + 1) : 39; memcpy(t, p, (size_t)n); t[n] = 0;
            if (mote_ui_read_w(t) > MAXW && brk > 0) break;
            end++;
        }
        int take = (p[end] == 0 || p[end] == '\n') ? end : (brk > 0 ? brk : end);
        if (take > 39) take = 39;
        memcpy(lines[nl], p, (size_t)take); lines[nl][take] = 0; nl++;
        p += take; while (*p == ' ') p++; if (*p == '\n') p++;
    }

    int lh = mote_ui_read_h() + 2, body_y = 32, ftr_y = MOTE_FB_H - 15;
    int vis = (ftr_y - body_y) / lh, scroll = 0;
    MoteInput in; memset(&in, 0, sizeof in);
    { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
    uint64_t last = mote_plat_micros();
    for (;;) {
        uint64_t now = mote_plat_micros();
        uint32_t dt = (uint32_t)((now - last) / 1000); last = now;
        MoteButtons raw; mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, dt);
        mote_plat_audio_pump();
        if (mote_plat_should_quit()) return;
        if (mote_just_pressed(&in, MOTE_BTN_B) || mote_just_pressed(&in, MOTE_BTN_LB)) return;
        if (mote_just_pressed(&in, MOTE_BTN_DOWN) && scroll + vis < nl) scroll++;
        if (mote_just_pressed(&in, MOTE_BTN_UP)   && scroll > 0)        scroll--;

        mote_ui_ground(fb);
        fillrect(fb, 0, 0, MOTE_FB_W, 18, MOTE_UI_BAR);
        fillrect(fb, 0, 16, MOTE_FB_W, 1, MOTE_UI_ACCENT);
        mote_ui_text(fb, s_g[idx].name, 4, 3, MOTE_UI_GOLD);
        { char by[44]; snprintf(by, sizeof by, "by %.30s", s_g[idx].author);
          mote_ui_text(fb, by, 4, 20, MOTE_UI_DIM); }
        for (int r = 0; r < vis && scroll + r < nl; r++)
            mote_ui_read(fb, lines[scroll + r], 4, body_y + r * lh, MOTE_UI_TEXT);
        if (scroll > 0)          mote_ui_text(fb, "^", MOTE_FB_W - 10, body_y, MOTE_UI_ACCENT);
        if (scroll + vis < nl)   mote_ui_text(fb, "v", MOTE_FB_W - 10, ftr_y - lh, MOTE_UI_ACCENT);
        fillrect(fb, 0, ftr_y, MOTE_FB_W, MOTE_FB_H - ftr_y, MOTE_UI_BAR);
        fillrect(fb, 0, ftr_y, MOTE_FB_W, 1, MOTE_UI_ACCENT);
        mote_ui_text(fb, "^v scroll  B close",
                     (MOTE_FB_W - mote_ui_text_w("^v scroll  B close")) / 2, ftr_y + 2, MOTE_UI_DIM);
        mote_plat_present(fb);
    }
}

/* A blocking screen while an install runs, with the progress the part-file gives. */
static void gg_install_wait(uint16_t *fb, int sel) {
    while (s_job == JOB_INSTALL && !mote_plat_should_quit()) {
        MoteButtons raw; mote_plat_buttons(&raw);
        mote_plat_audio_pump();
        mote_ui_ground(fb);
        mote_ui_header(fb, s_g[sel].name, -1, -1);
        const char *m = "DOWNLOADING...";
        mote_ui_read(fb, m, (MOTE_FB_W - mote_ui_read_w(m)) / 2, 40, MOTE_UI_GOLD);
        long total = s_g[sel].size;
        if (total > 0) {
            long have = file_size(s_part);
            int pct = (int)(have * 100 / total); if (pct > 100) pct = 100;
            mote_draw_rect(fb, 12, 66, 104, 10, MOTE_UI_BAR, 1, 0, MOTE_FB_H);
            if (pct > 0) mote_draw_rect(fb, 13, 67, 102 * pct / 100, 8, MOTE_UI_ACCENT, 1, 0, MOTE_FB_H);
            char pc[16]; snprintf(pc, sizeof pc, "%d%%", pct);
            mote_ui_text(fb, pc, (MOTE_FB_W - mote_ui_text_w(pc)) / 2, 82, MOTE_UI_TEXT);
        }
        mote_ui_footer(fb, "PLEASE WAIT");
        mote_plat_present(fb);
    }
    job_reap();
}

void mote_android_gallery_screen(void) {
    uint16_t *fb = mote_launcher_fb();
    if (!s_base[0]) mote_android_gallery_set_base(NULL);

    /* Fetch the manifest, with a spinner and a way out, as the handheld does. */
    if (s_n == 0) {
        job_start(fetch_thread, JOB_FETCH, 0);
        MoteInput in; memset(&in, 0, sizeof in);
        { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
        uint64_t last = mote_plat_micros();
        while (s_job == JOB_FETCH) {
            uint64_t now = mote_plat_micros();
            uint32_t dt = (uint32_t)((now - last) / 1000); last = now;
            MoteButtons raw; mote_plat_buttons(&raw);
            mote_input_update(&in, &raw, dt);
            mote_plat_audio_pump();
            if (mote_plat_should_quit()) return;
            if (mote_just_pressed(&in, MOTE_BTN_B)) { job_reap(); return; }
            mote_ui_ground(fb);
            mote_ui_header(fb, "GALLERY", -1, -1);
            gg_spinner(fb, MOTE_FB_W / 2, 50);
            const char *m = "fetching the gallery...";
            mote_ui_text(fb, m, (MOTE_FB_W - mote_ui_text_w(m)) / 2, 72, MOTE_UI_DIM);
            mote_ui_footer(fb, "B CANCEL");
            mote_plat_present(fb);
        }
        job_reap();
    }
    if (s_n == 0) {                                   /* offline / bad manifest */
        MoteInput in; memset(&in, 0, sizeof in);
        { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
        uint64_t last = mote_plat_micros();
        for (;;) {
            uint64_t now = mote_plat_micros();
            uint32_t dt = (uint32_t)((now - last) / 1000); last = now;
            MoteButtons raw; mote_plat_buttons(&raw);
            mote_input_update(&in, &raw, dt);
            mote_plat_audio_pump();
            if (mote_plat_should_quit() || mote_just_pressed(&in, MOTE_BTN_B)) return;
            if (mote_just_pressed(&in, MOTE_BTN_A)) { mote_android_gallery_screen(); return; }
            mote_ui_ground(fb);
            mote_ui_title(fb, "NO GALLERY", (MOTE_FB_W - mote_ui_title_w("NO GALLERY")) / 2, 16, 0xFD40);
            mote_ui_text(fb, s_err[0] ? s_err : "could not reach the gallery",
                         (MOTE_FB_W - mote_ui_text_w(s_err[0] ? s_err : "could not reach the gallery")) / 2,
                         44, MOTE_UI_TEXT);
            mote_ui_text(fb, "check the connection",
                         (MOTE_FB_W - mote_ui_text_w("check the connection")) / 2, 64, 0x9CD3);
            mote_ui_text(fb, "A retry      B back",
                         (MOTE_FB_W - mote_ui_text_w("A retry      B back")) / 2, 104, 0xACD3);
            mote_plat_present(fb);
        }
    }

    int sel = 0, shot = 0, msg_t = 0, last_sel = -1;
    char msg[48] = { 0 };
    uint64_t settled = 0;
    tb_init();
    prefetch_start(0);          /* warm the whole catalogue behind the player */
    MoteInput in; memset(&in, 0, sizeof in);
    { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
    uint64_t last = mote_plat_micros(), last_input = last;

    while (!mote_plat_should_quit()) {
        uint64_t now = mote_plat_micros();
        uint32_t dt = (uint32_t)((now - last) / 1000); last = now;
        MoteButtons raw; mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, dt);
        mote_plat_audio_pump();

        int acted = 0;
        if (mote_just_pressed(&in, MOTE_BTN_B)) break;
        if (mote_just_pressed(&in, MOTE_BTN_DOWN) || mote_just_pressed(&in, MOTE_BTN_RIGHT)) {
            sel = (sel + 1) % s_n; shot = 0; acted = 1;
        }
        if (mote_just_pressed(&in, MOTE_BTN_UP) || mote_just_pressed(&in, MOTE_BTN_LEFT)) {
            sel = (sel - 1 + s_n) % s_n; shot = 0; acted = 1;
        }
        if (mote_just_pressed(&in, MOTE_BTN_LB)) {
            gg_desc_modal(fb, sel);
            { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
            acted = 1;
        }
        if (mote_just_pressed(&in, MOTE_BTN_A)) {
            int st = ent_state(&s_g[sel]);
            if (st == ST_GET || st == ST_UPDATE || st == ST_INSTALLED) {
                job_start(install_thread, JOB_INSTALL, sel);
                gg_install_wait(fb, sel);
                if (s_job == JOB_OK && !s_err[0])
                    snprintf(msg, sizeof msg, "installed v%.8s", s_g[sel].version);
                else
                    snprintf(msg, sizeof msg, "%.38s", s_err[0] ? s_err : "install failed");
                msg_t = 140;
                { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
            }
            acted = 1;
        }
        if (acted) last_input = now;
        /* Attract mode: after ~2.5s idle, cycle THIS game's screenshots — and only
         * once the next one is already decoded, so it never shows a spinner. */
        else if ((now - last_input) / 1000 >= 2500 && s_g[sel].nshots > 1) {
            int nx = (shot + 1) % s_g[sel].nshots;
            if (tb_find(sel, nx) >= 0) { shot = nx; last_input = now; }
        }
        if (sel != last_sel) { last_sel = sel; settled = now; }   /* debounce fast scrolling */
        th_pump();
        if ((now - settled) / 1000 >= 120)
            th_schedule(sel, shot, s_g[sel].nshots);
        if (msg_t > 0) msg_t--;

        int ds = tb_find(sel, shot);
        gg_draw(fb, sel, shot, ds >= 0 ? s_tb[ds] : NULL, msg_t, msg);
        mote_plat_present(fb);
    }
    prefetch_stop();
    if (s_th_thread) { SDL_WaitThread(s_th_thread, NULL); s_th_thread = NULL; s_th_busy = 0; }
}
