/* Mote Studio — gallery data layer (see gallery.h). Pure data + IO, no UI. */
#include "gallery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image implementation is compiled in motecore.c; just borrow the symbols. */
extern unsigned char *stbi_load(const char *, int *, int *, int *, int);
extern void           stbi_image_free(void *);

#ifdef _WIN32
  #include <direct.h>
  #define NULDEV "NUL"
  #define MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define NULDEV "/dev/null"
  #define MKDIR(p) mkdir(p, 0755)
#endif

static void mkdir_p(const char *path) {
    char t[GAL_URLLEN]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++)
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; MKDIR(t); *p = c; }
    MKDIR(t);
}
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) { fclose(f); return 1; } return 0; }

/* ---------------- SHA-256 (compact, public-domain style) ------------------ */
typedef struct { uint32_t s[8]; uint64_t len; uint8_t buf[64]; int n; } sha256_t;
static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
static void sha256_init(sha256_t *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->s, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha256_block(sha256_t *c, const uint8_t *p) {
    static const uint32_t K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64], a,b,cc,d,e,f,g,h; int i;
    for (i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4]<<24 | p[i*4+1]<<16 | p[i*4+2]<<8 | p[i*4+3];
    for (; i < 64; i++) { uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    a=c->s[0];b=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^(~e&g), t1=h+S1+ch+K[i]+w[i];
        uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), mj=(a&b)^(a&cc)^(b&cc), t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void sha256_update(sha256_t *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n) { int t = 64 - c->n; if (t > (int)n) t = (int)n;
        memcpy(c->buf + c->n, p, t); c->n += t; p += t; n -= t;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; } }
}
static void sha256_final(sha256_t *c, char *hex) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80; sha256_update(c, &pad, 1);
    uint8_t z = 0; while (c->n != 56) sha256_update(c, &z, 1);
    uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - i*8)); sha256_update(c, L, 8);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) {
        uint8_t b = (uint8_t)(c->s[i] >> (24 - j*8)); *hex++ = H[b>>4]; *hex++ = H[b&15]; }
    *hex = 0;
}
int gallery_sha256_file(const char *path, char *out) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    sha256_t c; sha256_init(&c); uint8_t b[8192]; size_t r;
    while ((r = fread(b, 1, sizeof b, f)) > 0) sha256_update(&c, b, r);
    fclose(f); sha256_final(&c, out); return 0;
}

/* ---------------- version compare (dotted numeric) ------------------------ */
int gallery_vercmp(const char *a, const char *b) {
    while (*a || *b) {
        long na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') na = na*10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') nb = nb*10 + (*b++ - '0');
        if (na != nb) return na < nb ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
        if ((*a && *a < '0') || (*b && *b < '0')) break;   /* stop at pre-release tags */
    }
    return 0;
}

/* ---------------- curl fetch ---------------------------------------------- */
static const char *curl_bin(void) {
    const char *e = getenv("MOTE_CURL"); return (e && *e) ? e : "curl";
}
static int fetch(const char *url, const char *out, char *err, int errn) {
    char cmd[GAL_URLLEN + 320];
    snprintf(cmd, sizeof cmd, "%s -fsSL --max-time 45 -o \"%s\" \"%s\" 2>%s",
             curl_bin(), out, url, NULDEV);
    int rc = system(cmd);
    if (rc != 0) { if (err) snprintf(err, errn, "download failed (curl %d): %s", rc, url); return -1; }
    return 0;
}

/* ---------------- tiny JSON scan (for the known manifest shape) ----------- */
/* find the value token after "key": within [p,end); returns ptr to value or NULL */
static const char *j_field(const char *p, const char *end, const char *key) {
    char pat[48]; int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    for (const char *q = p; q + n < end; q++)
        if (!strncmp(q, pat, n)) {
            q += n; while (q < end && (*q == ' ' || *q == ':' || *q == '\t')) q++;
            return q;
        }
    return NULL;
}
static void j_str(const char *v, char *out, int outn) {
    out[0] = 0; if (!v || *v != '"') return;
    v++; int o = 0;
    while (*v && *v != '"' && o < outn - 1) {
        if (*v == '\\') { v++;
            char c = *v; if (c=='n')c='\n'; else if(c=='t')c='\t';
            else if(c=='u'){ /* \uXXXX -> keep ASCII if <128 else '?' */
                int cp=0; for(int k=0;k<4&&v[1];k++){ v++; char h=*v; cp=cp*16+(h<='9'?h-'0':(h|32)-'a'+10); }
                out[o++] = cp < 128 ? (char)cp : '?'; v++; continue; }
            out[o++] = c ? c : '"'; if (*v) v++; continue;
        }
        out[o++] = *v++;
    }
    out[o] = 0;
}
static long j_num(const char *v) { return v ? strtol(v, NULL, 10) : 0; }

int gallery_refresh(Gallery *G) {
    G->loaded = 0; G->n = 0; G->err[0] = 0;
    char url[GAL_URLLEN + 16], tmp[GAL_URLLEN + 16];
    snprintf(url, sizeof url, "%s/games.json", G->base);
    gallery_manifest_path(G, tmp, sizeof tmp);   /* cache it: the device-service streams this file */
    if (fetch(url, tmp, G->err, sizeof G->err) != 0) return -1;
    FILE *f = fopen(tmp, "rb"); if (!f) { snprintf(G->err, sizeof G->err, "manifest read failed"); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1<<20) { fclose(f); snprintf(G->err, sizeof G->err, "manifest too big/empty"); return -1; }
    char *buf = malloc(sz + 1); if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f); buf[sz] = 0;
    const char *end = buf + sz;
    const char *gv = j_field(buf, end, "generated"); if (gv) j_str(gv, G->generated, sizeof G->generated);
    /* find the games array and walk each {...} object (objects have no nested {}) */
    const char *arr = j_field(buf, end, "games"); if (!arr || *arr != '[') { free(buf); snprintf(G->err, sizeof G->err, "no games array"); return -1; }
    const char *p = arr + 1;
    while (p < end && G->n < GAL_MAX) {
        const char *ob = memchr(p, '{', end - p); if (!ob) break;
        const char *oe = memchr(ob, '}', end - ob); if (!oe) break;
        GalGame *g = &G->g[G->n]; memset(g, 0, sizeof *g);
        j_str(j_field(ob, oe, "id"),      g->id,      sizeof g->id);
        j_str(j_field(ob, oe, "name"),    g->name,    sizeof g->name);
        j_str(j_field(ob, oe, "author"),  g->author,  sizeof g->author);
        j_str(j_field(ob, oe, "version"), g->version, sizeof g->version);
        g->abi  = (int) j_num(j_field(ob, oe, "abi"));
        g->size =       j_num(j_field(ob, oe, "size"));
        j_str(j_field(ob, oe, "sha256"),  g->sha256,  sizeof g->sha256);
        j_str(j_field(ob, oe, "file"),    g->file,    sizeof g->file);
        j_str(j_field(ob, oe, "thumb"),   g->thumb,   sizeof g->thumb);
        /* screenshots: [ "a.png", "b.png", ... ] — walk the quoted strings up to `]` */
        { const char *sv = j_field(ob, oe, "screenshots"); g->nshots = 0;
          if (sv && *sv == '[') {
            const char *rb = memchr(sv, ']', oe - sv); const char *lim = rb ? rb : oe;
            const char *q = sv + 1;
            while (g->nshots < GAL_MAXSHOTS) {
                const char *qs = memchr(q, '"', lim - q); if (!qs) break;
                j_str(qs, g->shots[g->nshots], GAL_URLLEN); g->nshots++;
                q = qs + 1; while (q < lim && *q != '"') q++; q++;   /* past this string */
            }
          }
          if (g->nshots == 0 && g->thumb[0]) { snprintf(g->shots[0], GAL_URLLEN, "%s", g->thumb); g->nshots = 1; }
        }
        j_str(j_field(ob, oe, "tag"),     g->tag,     sizeof g->tag);
        j_str(j_field(ob, oe, "desc"),    g->desc,    sizeof g->desc);
        { const char *mv = j_field(ob, oe, "multiplayer"); g->multiplayer = mv && !strncmp(mv, "true", 4); }
        if (g->id[0]) G->n++;
        p = oe + 1;
    }
    free(buf);
    if (G->n == 0) { snprintf(G->err, sizeof G->err, "manifest has no games"); return -1; }
    G->loaded = 1; return 0;
}

int gallery_diff(Gallery *G, int dev_abi, const char *(*dev_ver)(const char *, void *), void *ctx) {
    for (int i = 0; i < G->n; i++) {
        GalGame *g = &G->g[i];
        const char *iv = dev_ver ? dev_ver(g->id, ctx) : NULL;
        g->installed_version[0] = 0;
        if (!iv) { g->state = (dev_abi && g->abi > dev_abi) ? GAL_INCOMPATIBLE : GAL_NONE; continue; }
        snprintf(g->installed_version, sizeof g->installed_version, "%s", iv);
        if (dev_abi && g->abi > dev_abi) g->state = GAL_INCOMPATIBLE;
        else if (gallery_vercmp(iv, g->version) < 0) g->state = GAL_UPDATE;
        else g->state = GAL_INSTALLED;
    }
    return 0;
}

int gallery_download(Gallery *G, const GalGame *g, const char *outpath) {
    /* go through the cache so a game is fetched from GitHub at most once per version,
     * then copy the verified cached .mote to the caller's path */
    char cached[GAL_URLLEN * 2];
    if (gallery_ensure_mote(G, g, cached, sizeof cached) != 0) return -1;
    FILE *in = fopen(cached, "rb"), *out = fopen(outpath, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out);
                       snprintf(G->err, sizeof G->err, "cache copy failed"); return -1; }
    char b[8192]; size_t r;
    while ((r = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, r, out);
    fclose(in); fclose(out);
    return 0;
}

int gallery_load_thumb(Gallery *G, GalGame *g) {
    if (g->thumb_px || !g->thumb[0]) return 0;
    char url[GAL_URLLEN * 2], tmp[GAL_URLLEN * 2];
    snprintf(url, sizeof url, "%s/%s", G->base, g->thumb);
    snprintf(tmp, sizeof tmp, "%s/thumb_%s.png", G->cache, g->id);   /* cached, reusable */
    if (!file_exists(tmp) && fetch(url, tmp, G->err, sizeof G->err) != 0) return -1;
    int w, h, n; unsigned char *d = stbi_load(tmp, &w, &h, &n, 4);
    if (!d) { snprintf(G->err, sizeof G->err, "thumb decode failed"); return -1; }
    uint16_t *px = malloc((size_t)w * h * 2);
    if (px) { for (int i = 0; i < w*h; i++) {
        int r = d[i*4], gg = d[i*4+1], b = d[i*4+2];
        px[i] = (uint16_t)(((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3)); }
        g->thumb_px = px; g->thumb_w = w; g->thumb_h = h; }
    stbi_image_free(d);
    return px ? 0 : -1;
}

int gallery_load_shot(Gallery *G, const GalGame *g, int idx, uint16_t **outpx, int *outw, int *outh) {
    if (idx < 0 || idx >= g->nshots || !g->shots[idx][0]) return -1;
    char url[GAL_URLLEN * 2], tmp[GAL_URLLEN * 2];
    snprintf(url, sizeof url, "%s/%s", G->base, g->shots[idx]);
    snprintf(tmp, sizeof tmp, "%s/shot_%s_%d.png", G->cache, g->id, idx);   /* cached, reusable */
    if (!file_exists(tmp) && fetch(url, tmp, G->err, sizeof G->err) != 0) return -1;
    int w, h, n; unsigned char *d = stbi_load(tmp, &w, &h, &n, 4);
    if (!d) { snprintf(G->err, sizeof G->err, "shot decode failed"); return -1; }
    uint16_t *px = malloc((size_t)w * h * 2);
    if (px) { for (int i = 0; i < w*h; i++) {
        int r = d[i*4], gg = d[i*4+1], b = d[i*4+2];
        px[i] = (uint16_t)(((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3)); } }
    stbi_image_free(d);
    if (!px) return -1;
    *outpx = px; *outw = w; *outh = h; return 0;
}

void gallery_set_base(Gallery *G, const char *base) {
    snprintf(G->base, sizeof G->base, "%s", base);
    size_t l = strlen(G->base); while (l && G->base[l-1] == '/') G->base[--l] = 0;
    if (!G->cache[0]) {                       /* default cache under the user's cache dir */
        const char *root =
#ifdef _WIN32
            getenv("LOCALAPPDATA");
#else
            getenv("XDG_CACHE_HOME");
#endif
        char def[GAL_URLLEN];
        if (root && *root) snprintf(def, sizeof def, "%s/mote-studio/gallery", root);
        else {
            const char *home = getenv("HOME"); if (!home || !*home) home =
#ifdef _WIN32
                getenv("USERPROFILE") ? getenv("USERPROFILE") : ".";
#else
                ".";
#endif
            snprintf(def, sizeof def, "%s/.cache/mote-studio/gallery", home);
        }
        gallery_set_cache(G, def);
    }
}
void gallery_set_cache(Gallery *G, const char *dir) {
    snprintf(G->cache, sizeof G->cache, "%s", dir);
    size_t l = strlen(G->cache); while (l && (G->cache[l-1] == '/' || G->cache[l-1] == '\\')) G->cache[--l] = 0;
    mkdir_p(G->cache);
}
const char *gallery_manifest_path(Gallery *G, char *out, int outn) {
    snprintf(out, outn, "%s/games.json", G->cache); return out;
}
int gallery_ensure_mote(Gallery *G, const GalGame *g, char *out, int outn) {
    snprintf(out, outn, "%s/%s_%s.mote", G->cache, g->id, g->version);
    char hex[GAL_HEXLEN];
    if (file_exists(out) && gallery_sha256_file(out, hex) == 0 &&
        g->sha256[0] && strcasecmp(hex, g->sha256) == 0)
        return 0;                             /* cache hit — no download */
    char url[GAL_URLLEN * 2];
    snprintf(url, sizeof url, "%s/%s", G->base, g->file);
    if (fetch(url, out, G->err, sizeof G->err) != 0) return -1;
    if (gallery_sha256_file(out, hex) != 0) { snprintf(G->err, sizeof G->err, "hash read failed"); return -1; }
    if (g->sha256[0] && strcasecmp(hex, g->sha256) != 0) {
        remove(out); snprintf(G->err, sizeof G->err, "checksum mismatch for %s (corrupt download)", g->name);
        return -1;
    }
    return 0;
}
