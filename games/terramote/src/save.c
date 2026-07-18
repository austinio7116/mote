/* TerraMote — persistence: RLE-compressed world planes + player blob (kv store). */
#include "terra.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SAVE_MAGIC   0x544D3032u   /* "TM02" — MAX_CHESTS grew, old saves incompatible */
#define BAND_ROWS    31            /* 8 bands x 31 rows = 248 */
#define BANDS        8
#define BAND_BYTES   (BAND_ROWS * WCOLS)

static uint8_t *s_scratch;         /* arena, worst-case RLE = 2x band */

void save_alloc(void);
void save_alloc(void) {
    s_scratch = (uint8_t *)mote->alloc(BAND_BYTES * 2 + 16);
}

/* net.c streams the world with this exact codec + buffer (no save runs mid-transfer) */
uint8_t *save_scratch(void) { return s_scratch; }

static int rle_pack(const uint8_t *src, int n, uint8_t *dst, int max) {
    int o = 0, i = 0;
    while (i < n) {
        uint8_t v = src[i];
        int run = 1;
        while (i + run < n && src[i + run] == v && run < 255) run++;
        if (o + 2 > max) return -1;
        dst[o++] = (uint8_t)run; dst[o++] = v;
        i += run;
    }
    return o;
}
static int rle_unpack(const uint8_t *src, int n, uint8_t *dst, int max) {
    int o = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        int run = src[i]; uint8_t v = src[i + 1];
        if (o + run > max) return -1;
        memset(dst + o, v, run); o += run;
    }
    return o;
}
int save_rle_pack(const uint8_t *src, int n, uint8_t *dst, int max) { return rle_pack(src, n, dst, max); }
int save_rle_unpack(const uint8_t *src, int n, uint8_t *dst, int max) { return rle_unpack(src, n, dst, max); }

typedef struct {
    uint32_t magic;
    uint32_t seed;
    float    time;
    uint8_t  boss_down;
    uint8_t  wallsfix;   /* 1 = world has the below-surface wall backfill (upgrade once) */
    uint8_t  autosave;   /* player's autosave option + 1 (0 = legacy save: default) */
    uint8_t  pad[1];
    Chest    chests[MAX_CHESTS];
} WorldMeta;

typedef struct {
    uint32_t magic;
    Player   pl;
} PlayerBlob;

int save_world_exists(void) {
    WorldMeta m;
    if (mote->kv_load("meta", &m, sizeof(m)) < (int)sizeof(m)) return 0;
    return m.magic == SAVE_MAGIC;
}

/* One save step: 16 band-plane keys, then meta+chests, explored, player.
 * The AUTOSAVE runs these one per frame (save_world_begin/step) so a co-op
 * host never goes link-silent long enough to trip the engine's stall banner
 * on the guest — and the host's own frame never hitches. Manual saves still
 * run all steps back to back (save_world). */
#define SAVE_STEPS (BANDS * 2 + 3)
static int s_save_step = -1;

static void save_one_step(int s) {
    if (s < BANDS * 2) {
        int b = s >> 1, plane = s & 1;
        const uint8_t *src = (plane ? g_bgm : g_fgm) + b * BAND_BYTES;
        int n = rle_pack(src, BAND_BYTES, s_scratch, BAND_BYTES * 2 + 16);
        char key[8];
        key[0] = 'w'; key[1] = (char)('f' + plane * 2); key[2] = (char)('0' + b); key[3] = 0;
        if (n > 0) mote->kv_save(key, s_scratch, n);
    } else if (s == BANDS * 2) {
        WorldMeta m = { SAVE_MAGIC, g_seed, g_time, g_boss_down, 1,
                        (uint8_t)(g_autosave_opt + 1), {0}, {{0}} };
        memcpy(m.chests, g_chests, sizeof(g_chests));
        mote->kv_save("meta", &m, sizeof(m));
    } else if (s == BANDS * 2 + 1) {
        /* fog of war: RLE the explored bitmap under its own key (back-compat:
         * old saves simply lack it and start unexplored) */
        int n = rle_pack(g_explored, sizeof(g_explored), s_scratch, BAND_BYTES * 2 + 16);
        if (n > 0) mote->kv_save("exp", s_scratch, n);
    } else {
        save_player();
    }
}

void save_world_begin(void) { s_save_step = 0; }
int save_world_step(void) {                    /* 1 = still saving, 0 = done/idle */
    if (s_save_step < 0) return 0;
    save_one_step(s_save_step++);
    if (s_save_step >= SAVE_STEPS) { s_save_step = -1; return 0; }
    return 1;
}
int save_world_busy(void) { return s_save_step >= 0; }

void save_world(void) {
    for (int s = 0; s < SAVE_STEPS; s++) save_one_step(s);
    s_save_step = -1;
}

int load_world(void) {
    WorldMeta m;
    if (mote->kv_load("meta", &m, sizeof(m)) < (int)sizeof(m) || m.magic != SAVE_MAGIC) return 0;
    char key[8];
    for (int b = 0; b < BANDS; b++) {
        for (int plane = 0; plane < 2; plane++) {
            key[0] = 'w'; key[1] = (char)('f' + plane * 2); key[2] = (char)('0' + b); key[3] = 0;
            int n = mote->kv_load(key, s_scratch, BAND_BYTES * 2 + 16);
            int u = n > 0 ? rle_unpack(s_scratch, n, (plane ? g_bgm : g_fgm) + b * BAND_BYTES, BAND_BYTES) : -1;
            if (getenv("TERRA_DBG")) {
                char msg[64];
                snprintf(msg, 64, "dbg load %s n=%d unpacked=%d", key, n, u);
                mote->log(msg);
            }
            if (n <= 0 || u != BAND_BYTES) return 0;
        }
    }
    memcpy(g_chests, m.chests, sizeof(g_chests));
    g_seed = m.seed;
    g_time = m.time;
    g_boss_down = m.boss_down;
    g_autosave_opt = m.autosave ? (uint8_t)((m.autosave - 1) & 3) : 2;
    memset(g_explored, 0, sizeof(g_explored));
    {   /* explored bitmap (optional key — pre-fog saves start unexplored) */
        int n = mote->kv_load("exp", s_scratch, BAND_BYTES * 2 + 16);
        if (n > 0) rle_unpack(s_scratch, n, g_explored, sizeof(g_explored));
    }
    world_rebuild_caches();          /* the surface cache feeds sunlight + the sky */
    if (!m.wallsfix) world_backfill_walls();   /* one-time upgrade: kill the black
                                                  pockets under overhangs; the next
                                                  save stamps the flag so axed-out
                                                  walls stay gone */
    return 1;
}

void save_player(void) {
    PlayerBlob b = { SAVE_MAGIC, g_pl };
    mote->kv_save("plr", &b, sizeof(b));
}

/* guest in co-op: persist the CHARACTER (inventory, armor, hp, looks) but keep
 * the position/spawn of THEIR OWN world — the coords here are the host's map */
void save_player_coop(void) {
    PlayerBlob b;
    Player p = g_pl;
    if (mote->kv_load("plr", &b, sizeof(b)) == (int)sizeof(b) && b.magic == SAVE_MAGIC) {
        p.x = b.pl.x; p.y = b.pl.y;
        p.spawn_c = b.pl.spawn_c; p.spawn_r = b.pl.spawn_r;
    }
    b.magic = SAVE_MAGIC;
    b.pl = p;
    mote->kv_save("plr", &b, sizeof(b));
}

int load_player(void) {
    PlayerBlob b;
    if (mote->kv_load("plr", &b, sizeof(b)) < (int)sizeof(b) || b.magic != SAVE_MAGIC) return 0;
    g_pl = b.pl;
    return 1;
}
