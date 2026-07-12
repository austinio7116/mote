/* TerraMote — persistence: RLE-compressed world planes + player blob (kv store). */
#include "terra.h"
#include <string.h>

#define SAVE_MAGIC   0x544D3031u   /* "TM01" */
#define BAND_ROWS    31            /* 8 bands x 31 rows = 248 */
#define BANDS        8
#define BAND_BYTES   (BAND_ROWS * WCOLS)

static uint8_t *s_scratch;         /* arena, worst-case RLE = 2x band */

void save_alloc(void);
void save_alloc(void) {
    s_scratch = (uint8_t *)mote->alloc(BAND_BYTES * 2 + 16);
}

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

typedef struct {
    uint32_t magic;
    uint32_t seed;
    float    time;
    uint8_t  boss_down;
    uint8_t  pad[3];
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

void save_world(void) {
    char key[8];
    for (int b = 0; b < BANDS; b++) {
        for (int plane = 0; plane < 2; plane++) {
            const uint8_t *src = (plane ? g_bgm : g_fgm) + b * BAND_BYTES;
            int n = rle_pack(src, BAND_BYTES, s_scratch, BAND_BYTES * 2 + 16);
            key[0] = 'w'; key[1] = (char)('f' + plane * 2); key[2] = (char)('0' + b); key[3] = 0;
            if (n > 0) mote->kv_save(key, s_scratch, n);
        }
    }
    WorldMeta m = { SAVE_MAGIC, g_seed, g_time, g_boss_down, {0}, {{0}} };
    memcpy(m.chests, g_chests, sizeof(g_chests));
    mote->kv_save("meta", &m, sizeof(m));
    save_player();
}

int load_world(void) {
    WorldMeta m;
    if (mote->kv_load("meta", &m, sizeof(m)) < (int)sizeof(m) || m.magic != SAVE_MAGIC) return 0;
    char key[8];
    for (int b = 0; b < BANDS; b++) {
        for (int plane = 0; plane < 2; plane++) {
            key[0] = 'w'; key[1] = (char)('f' + plane * 2); key[2] = (char)('0' + b); key[3] = 0;
            int n = mote->kv_load(key, s_scratch, BAND_BYTES * 2 + 16);
            if (n <= 0) return 0;
            uint8_t *dst = (plane ? g_bgm : g_fgm) + b * BAND_BYTES;
            if (rle_unpack(s_scratch, n, dst, BAND_BYTES) != BAND_BYTES) return 0;
        }
    }
    memcpy(g_chests, m.chests, sizeof(g_chests));
    g_seed = m.seed;
    g_time = m.time;
    g_boss_down = m.boss_down;
    return 1;
}

void save_player(void) {
    PlayerBlob b = { SAVE_MAGIC, g_pl };
    mote->kv_save("plr", &b, sizeof(b));
}

int load_player(void) {
    PlayerBlob b;
    if (mote->kv_load("plr", &b, sizeof(b)) < (int)sizeof(b) || b.magic != SAVE_MAGIC) return 0;
    g_pl = b.pl;
    return 1;
}
