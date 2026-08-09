/*
 * CueVR — the referee's voice. See cuevr_refcall.h.
 */
#include "cuevr_refcall.h"
#include "cue_audio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/log.h>
#define RLOG(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#define RLOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

#define REF_RATE  22050
#define REF_COUNT 153
#define REF_MAGIC "CUEREF01"

static uint8_t *s_blob;          /* the whole packed file */
static long     s_blob_len;
static int      s_count;
static const uint32_t *s_index;  /* count x (offset, samples) */
static const int16_t  *s_pcm;
static int      s_voice = CUEVR_REF_OFF;

const char *cuevr_refcall_voice_name(int v) {
    return v == CUEVR_REF_MALE   ? "MALE"
         : v == CUEVR_REF_FEMALE ? "FEMALE" : "OFF";
}
int cuevr_refcall_voice(void) { return s_voice; }

#ifdef __ANDROID__
static AAssetManager *s_am;
void cuevr_refcall_assets(struct AAssetManager *am) { s_am = (AAssetManager *)am; }
#endif

static void unload(void) {
    free(s_blob);
    s_blob = NULL; s_blob_len = 0;
    s_index = NULL; s_pcm = NULL; s_count = 0;
}

/* Read the whole packed file. Android has an asset manager; the host reads a
 * file, from CUEVR_REFCALLS_DIR or from the asset directory in the tree, so the
 * preview says the same words as the headset without a second copy. */
static uint8_t *slurp(const char *name, long *out_len) {
#ifdef __ANDROID__
    if (!s_am) return NULL;
    AAsset *a = AAssetManager_open(s_am, name, AASSET_MODE_BUFFER);
    if (!a) return NULL;
    off_t n = AAsset_getLength(a);
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (p && AAsset_read(a, p, (size_t)n) != (int)n) { free(p); p = NULL; }
    AAsset_close(a);
    if (p) *out_len = (long)n;
    return p;
#else
    char path[512];
    const char *dir = getenv("CUEVR_REFCALLS_DIR");
    if (dir && dir[0]) snprintf(path, sizeof path, "%s/%s", dir, name);
    else snprintf(path, sizeof path, "cuevr/app/src/main/assets/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* and from beside the binary's own source tree, so it works whichever
         * directory the preview was launched from */
        snprintf(path, sizeof path, "../../cuevr/app/src/main/assets/%s", name);
        f = fopen(path, "rb");
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (p) *out_len = n;
    return p;
#endif
}

void cuevr_refcall_set_voice(int v) {
    if (v < 0 || v >= CUEVR_REF_N) v = CUEVR_REF_OFF;
    if (v == s_voice && (v == CUEVR_REF_OFF || s_blob)) return;
    unload();
    s_voice = v;
    if (v == CUEVR_REF_OFF) return;

    const char *name = (v == CUEVR_REF_MALE) ? "refcalls_m.bin" : "refcalls_f.bin";
    long n = 0;
    uint8_t *p = slurp(name, &n);
    /* A missing or short file is a game without a referee, not a crash: every
     * read below is bounds-checked against what actually arrived. */
    if (!p) { RLOG("[cuevr] refcalls: %s not found", name); s_voice = CUEVR_REF_OFF; return; }
    const long hdr = 8 + 4 + 4;
    if (n < hdr || memcmp(p, REF_MAGIC, 8) != 0) {
        RLOG("[cuevr] refcalls: %s is not a call set", name);
        free(p); s_voice = CUEVR_REF_OFF; return;
    }
    uint32_t count, rate;
    memcpy(&count, p + 8, 4);
    memcpy(&rate,  p + 12, 4);
    if (rate != REF_RATE || count == 0 || count > 4096 ||
        n < hdr + (long)count * 8) {
        RLOG("[cuevr] refcalls: %s is %u calls at %u Hz, need %d Hz",
             name, count, rate, REF_RATE);
        free(p); s_voice = CUEVR_REF_OFF; return;
    }
    s_blob = p; s_blob_len = n; s_count = (int)count;
    s_index = (const uint32_t *)(p + hdr);
    s_pcm   = (const int16_t *)(p + hdr + (long)count * 8);
    RLOG("[cuevr] refcalls: %s, %d calls, %ld KB", name, s_count, n >> 10);
}

/* One place decides which clip index n is and whether it exists; both the
 * interrupting and the queued forms go through it. */
static const int16_t *clip(int n, int *out_len) {
    if (!s_blob || n < 1 || n > s_count) return NULL;
    uint32_t off, len;
    memcpy(&off, &s_index[(n - 1) * 2 + 0], 4);
    memcpy(&len, &s_index[(n - 1) * 2 + 1], 4);
    long base = 8 + 4 + 4 + (long)s_count * 8;
    if ((long)off + (long)len * 2 > s_blob_len - base) return NULL;
    *out_len = (int)len;
    return s_pcm + off / 2;
}

void cuevr_refcall_say_after(int n) {
    int len;
    const int16_t *p = clip(n, &len);
    if (!p) return;
    if (getenv("CUEVR_REFDBG"))
        RLOG("[refcall] queued \"%d\" (%.2f s)", n, (double)len / (double)REF_RATE);
    cue_audio_speak_after(p, len, 0.55f);
}

void cuevr_refcall_say(int n) {
    if (!s_blob || n < 1 || n > s_count) return;
    uint32_t off, len;
    memcpy(&off, &s_index[(n - 1) * 2 + 0], 4);
    memcpy(&len, &s_index[(n - 1) * 2 + 1], 4);
    /* Byte offsets into the PCM block, so both are checked against what is
     * actually there before anything is handed to the mixer. */
    long base = 8 + 4 + 4 + (long)s_count * 8;
    if ((long)off + (long)len * 2 > s_blob_len - base) return;
    /* Its OWN slot, not one of the eight. The pool steals the quietest voice
     * and a call runs for over a second while the break that follows it is a
     * dozen loud clacks — on the pool's rule the referee is cut off mid-number
     * every single time. Quieter than the -1 dBFS it was normalised to, or it
     * sits on top of the game rather than in the room with it. */
    if (getenv("CUEVR_REFDBG"))
        RLOG("[refcall] \"%d\" (%d samples, %.2f s)", n, (int)len,
             (double)len / (double)REF_RATE);
    cue_audio_speak(s_pcm + off / 2, (int)len, 0.55f);
}

void cuevr_refcall_shutdown(void) { unload(); s_voice = CUEVR_REF_OFF; }
