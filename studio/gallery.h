/*
 * Mote Studio — gallery / update-manager data layer.
 *
 * Fetches the online manifest (docs/games.json on GitHub Pages) via a shelled-out
 * curl, parses it, and — when a device is docked — diffs it against the device's
 * installed catalog+versions to decide Installed / Update / Not-installed. Downloads
 * are sha256-verified before install. No TLS in Studio: curl handles HTTPS.
 *
 * UI lives in main.c (the DEVICE tab's GALLERY panel); this file is pure data + IO.
 */
#ifndef MOTE_GALLERY_H
#define MOTE_GALLERY_H

#include <stdint.h>

#define GAL_MAX      64
#define GAL_IDLEN    32
#define GAL_NAMELEN  40
#define GAL_HEXLEN   65
#define GAL_URLLEN   200
#define GAL_DESCLEN  512
#define GAL_MAXSHOTS 6           /* screenshots per game the slideshow cycles through */

/* install state vs the docked device */
enum { GAL_NONE = 0, GAL_INSTALLED, GAL_UPDATE, GAL_INCOMPATIBLE };

typedef struct {
    char     id[GAL_IDLEN];
    char     name[GAL_NAMELEN];
    char     author[GAL_NAMELEN];
    char     version[16];        /* gallery (available) version */
    int      abi;                /* min engine ABI required */
    long     size;
    char     sha256[GAL_HEXLEN];
    char     file[GAL_URLLEN];   /* relative to the gallery base, e.g. games/Foo.mote */
    char     thumb[GAL_URLLEN];  /* relative screenshot for the UI, or "" */
    char     shots[GAL_MAXSHOTS][GAL_URLLEN]; int nshots;   /* the slideshow's screenshots */
    char     tag[GAL_NAMELEN];
    char     desc[GAL_DESCLEN];
    int      multiplayer;
    /* filled by gallery_diff() against the device */
    int      state;              /* GAL_NONE/INSTALLED/UPDATE/INCOMPATIBLE */
    char     installed_version[16];
    /* UI: lazily-loaded thumbnail (RGB565), 0 until fetched */
    uint16_t *thumb_px; int thumb_w, thumb_h;
} GalGame;

typedef struct {
    char    base[GAL_URLLEN];    /* gallery base URL, no trailing slash */
    char    cache[GAL_URLLEN];   /* on-disk cache dir (manifest + downloaded .mote + thumbs) */
    char    generated[40];
    int     n;
    GalGame g[GAL_MAX];
    int     loaded;              /* 1 once a manifest parsed OK */
    char    err[160];
} Gallery;

/* Set the gallery base URL (default https://austinio7116.github.io/mote) and the
 * on-disk cache dir. gallery_set_base picks a default cache under the user's cache
 * dir; call gallery_set_cache to override. The cache is the SINGLE copy both the
 * Studio UI and the on-device USB gallery-service read from — fetch once, reuse. */
void gallery_set_base(Gallery *G, const char *base);
void gallery_set_cache(Gallery *G, const char *dir);

/* The cached games.json path (valid after a successful gallery_refresh). The device
 * gallery-service streams these bytes to the handheld verbatim. */
const char *gallery_manifest_path(Gallery *G, char *out, int outn);

/* Cache-first: ensure game g's verified .mote exists in the cache and return its
 * path in out. Reuses a cached copy whose sha256 already matches (no re-download);
 * otherwise downloads + verifies + keeps it. This is what BOTH the Studio installer
 * and the device gallery-service call, so a game is fetched from GitHub at most once
 * per version. Returns 0 on success, -1 on error (G->err set). */
int  gallery_ensure_mote(Gallery *G, const GalGame *g, char *out, int outn);

/* Fetch <base>/games.json via curl and parse it. Returns 0 on success, -1 on error
 * (G->err set). Blocking; call from a worker thread. */
int  gallery_refresh(Gallery *G);

/* Diff against a device: dev_ver(id) must return the installed version string, or
 * NULL if not installed. dev_abi is the device engine's ABI (0 if unknown). Fills
 * each game's ->state / ->installed_version. */
int  gallery_diff(Gallery *G, int dev_abi, const char *(*dev_ver)(const char *id, void *ctx), void *ctx);

/* Download game g's .mote to outpath and verify its sha256 against the manifest.
 * Returns 0 on success, -1 on fetch/verify failure (G->err set). */
int  gallery_download(Gallery *G, const GalGame *g, const char *outpath);

/* Lazily fetch + decode g's thumbnail into g->thumb_px (RGB565). No-op if already
 * loaded or the game has no thumb. Returns 0 on success. */
int  gallery_load_thumb(Gallery *G, GalGame *g);

/* Decode screenshot `idx` of game g into a freshly-malloc'd RGB565 buffer (caller
 * frees *outpx). Cache-first like the .mote/thumb. Returns 0 on success. Used by the
 * device gallery-service to stream any screenshot for the on-device slideshow. */
int  gallery_load_shot(Gallery *G, const GalGame *g, int idx, uint16_t **outpx, int *outw, int *outh);

/* Compare two dotted-numeric versions: <0, 0, >0 (like strcmp semantics). */
int  gallery_vercmp(const char *a, const char *b);

/* sha256 of a file -> 64-char lowercase hex in out (>=65). 0 on success. */
int  gallery_sha256_file(const char *path, char *out);

#endif /* MOTE_GALLERY_H */
