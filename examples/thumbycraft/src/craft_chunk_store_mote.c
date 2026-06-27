/*
 * ThumbyCraft — chunk store, Mote KV backend.
 *
 * Replaces the host stub: persists per-chunk player edits through the
 * Mote v38 key-value API (mote->kv_*, reached here via the craft_port_kv_*
 * shim in game.c). The runner backs each key as a file under the game's
 * save folder (/mote/saves/thumbycraft/kv/<key>); host/Studio back it with
 * files too, so a world persists in the emulator as well.
 *
 * One blob per chunk, keyed "r<region>_<cx>_<cz>". Blob layout:
 *   u32 nonce   region-binding nonce (must match on read)
 *   u16 count   mod count           u16 pad
 *   ChunkMod[count]                 u32 crc32(header+mods)
 *
 * No hashing/probing — the key IS the lookup. The nonce filter still works:
 * bind(region, nonce) sets the current nonce, and load() ignores blobs whose
 * embedded nonce doesn't match (so "New World" just bumps the scratch nonce
 * and the old chunks become invisible, overwritten in place on next save).
 */
#include "craft_chunk_store.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* KV shim (game.c) — thin wrappers over mote->kv_*. */
extern int  craft_port_kv_save(const char *key, const void *data, int len);
extern int  craft_port_kv_load(const char *key, void *data, int max);
extern void craft_port_kv_list(const char *prefix,
                               void (*cb)(const char *key, void *arg), void *arg);

#define HDR_BYTES          8                       /* nonce(4) + count(2) + pad(2) */
#define BLOB_MAX           (HDR_BYTES + CHUNK_STORE_MAX_MODS_PER_CHUNK * 4 + 4)

/* Reuse the shared save scratch (game.c) for the per-chunk blob — GAME_RAM is
 * maxed, and the record save and chunk ops never run concurrently (single core,
 * sequential). The buffer is sized for the larger record (>= BLOB_MAX). */
extern uint8_t craft_save_scratch[];
#define s_blob craft_save_scratch

static int      s_region = -1;
static uint32_t s_nonce;

/* Existence bitmap — "does region have a blob for this (cx,cz)?" — rebuilt on
 * bind from the key list, checked by load() to skip absent-chunk round-trips. */
static uint8_t  s_exists_bm[32];
static uint32_t exists_hash(int cx, int cz) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u + (uint32_t)cz * 0x85EBCA77u;
    h ^= h >> 16; return h & 0xFFu;
}
static bool exists_test(int cx, int cz) { uint32_t h = exists_hash(cx, cz); return (s_exists_bm[h >> 3] >> (h & 7)) & 1; }
static void exists_set (int cx, int cz) { uint32_t h = exists_hash(cx, cz); s_exists_bm[h >> 3] |= (uint8_t)(1u << (h & 7)); }

static void key_for(char *out, size_t n, int region, int cx, int cz) {
    snprintf(out, n, "r%d_%d_%d", region, cx, cz);
}
/* Parse "r<region>_<cx>_<cz>" -> (cx,cz). Returns true on a well-formed key. */
static bool parse_key(const char *k, int *cx, int *cz) {
    int r, x, z, used = 0;
    if (sscanf(k, "r%d_%d_%d%n", &r, &x, &z, &used) >= 3 && k[used] == '\0') {
        if (cx) *cx = x; if (cz) *cz = z; return true;
    }
    return false;
}

/* --- encode / decode -------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void     wr32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

static uint32_t crc32_calc(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i];
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1)); }
    return ~c;
}

/* Build a blob into s_blob; returns its length. */
static int build_blob(uint32_t nonce, const ChunkMod *mods, int n) {
    wr32(s_blob, nonce);
    s_blob[4] = (uint8_t)(n & 0xFF); s_blob[5] = (uint8_t)((n >> 8) & 0xFF);
    s_blob[6] = 0; s_blob[7] = 0;
    for (int i = 0; i < n; i++) {
        s_blob[HDR_BYTES + i*4 + 0] = mods[i].lx;
        s_blob[HDR_BYTES + i*4 + 1] = mods[i].y;
        s_blob[HDR_BYTES + i*4 + 2] = mods[i].lz;
        s_blob[HDR_BYTES + i*4 + 3] = mods[i].blk;
    }
    int data_end = HDR_BYTES + n*4;
    wr32(s_blob + data_end, crc32_calc(s_blob, (size_t)data_end));
    return data_end + 4;
}
/* Validate s_blob (already loaded, `len` bytes) under `nonce`; returns mod count or -1. */
static int parse_blob(int len, uint32_t nonce) {
    if (len < HDR_BYTES + 4) return -1;
    if (rd32(s_blob) != nonce) return -1;
    int cnt = s_blob[4] | (s_blob[5] << 8);
    if (cnt < 0 || cnt > CHUNK_STORE_MAX_MODS_PER_CHUNK) return -1;
    int data_end = HDR_BYTES + cnt*4;
    if (data_end + 4 > len) return -1;
    if (crc32_calc(s_blob, (size_t)data_end) != rd32(s_blob + data_end)) return -1;
    return cnt;
}

/* --- public API ------------------------------------------------- */
static void cb_set_bit(const char *key, void *arg) {
    (void)arg; int cx, cz; if (parse_key(key, &cx, &cz)) exists_set(cx, cz);
}

void craft_chunk_store_bind(int region, uint32_t nonce) {
    s_region = region; s_nonce = nonce;
    memset(s_exists_bm, 0, sizeof s_exists_bm);
    char prefix[8]; snprintf(prefix, sizeof prefix, "r%d_", region);
    craft_port_kv_list(prefix, cb_set_bit, NULL);
}
int      craft_chunk_store_bound(void)       { return s_region; }
uint32_t craft_chunk_store_bound_nonce(void) { return s_nonce; }

int craft_chunk_store_load(int chunk_x, int chunk_z, ChunkMod *out, int max_entries) {
    if (s_region < 0) return 0;
    if (!exists_test(chunk_x, chunk_z)) return 0;
    char key[24]; key_for(key, sizeof key, s_region, chunk_x, chunk_z);
    int len = craft_port_kv_load(key, s_blob, BLOB_MAX);
    if (len <= 0) return 0;
    int cnt = parse_blob(len, s_nonce);
    if (cnt < 0) return 0;
    if (cnt > max_entries) cnt = max_entries;
    for (int i = 0; i < cnt; i++) {
        out[i].lx = s_blob[HDR_BYTES + i*4 + 0]; out[i].y   = s_blob[HDR_BYTES + i*4 + 1];
        out[i].lz = s_blob[HDR_BYTES + i*4 + 2]; out[i].blk = s_blob[HDR_BYTES + i*4 + 3];
    }
    return cnt;
}

bool craft_chunk_store_save(int chunk_x, int chunk_z, const ChunkMod *mods, int n) {
    if (s_region < 0) return false;
    if (n > CHUNK_STORE_MAX_MODS_PER_CHUNK) n = CHUNK_STORE_MAX_MODS_PER_CHUNK;
    char key[24]; key_for(key, sizeof key, s_region, chunk_x, chunk_z);
    if (n == 0) { craft_port_kv_save(key, NULL, 0); return true; }   /* delete */
    int len = build_blob(s_nonce, mods, n);
    exists_set(chunk_x, chunk_z);
    return craft_port_kv_save(key, s_blob, len) == len;
}

static void cb_delete(const char *key, void *arg) { (void)arg; craft_port_kv_save(key, NULL, 0); }

void craft_chunk_store_erase_region(int region) {
    char prefix[8]; snprintf(prefix, sizeof prefix, "r%d_", region);
    craft_port_kv_list(prefix, cb_delete, NULL);
    if (region == s_region) memset(s_exists_bm, 0, sizeof s_exists_bm);
}

typedef struct { int src_region, dst_region; uint32_t src_nonce, dst_nonce; } CopyCtx;
static void cb_copy(const char *key, void *arg) {
    CopyCtx *c = (CopyCtx *)arg;
    int cx, cz; if (!parse_key(key, &cx, &cz)) return;
    int len = craft_port_kv_load(key, s_blob, BLOB_MAX);
    if (len <= 0) return;
    int cnt = parse_blob(len, c->src_nonce);
    if (cnt < 0) return;
    /* Re-stamp in place: only the nonce + CRC change (mods stay put), so no
     * separate snapshot buffer is needed. */
    wr32(s_blob, c->dst_nonce);
    int data_end = HDR_BYTES + cnt*4;
    wr32(s_blob + data_end, crc32_calc(s_blob, (size_t)data_end));
    char dkey[24]; key_for(dkey, sizeof dkey, c->dst_region, cx, cz);
    craft_port_kv_save(dkey, s_blob, data_end + 4);
}

void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce) {
    if (src_region == dst_region && src_nonce == dst_nonce) return;
    CopyCtx ctx = { src_region, dst_region, src_nonce, dst_nonce };
    char prefix[8]; snprintf(prefix, sizeof prefix, "r%d_", src_region);
    craft_port_kv_list(prefix, cb_copy, &ctx);
}
