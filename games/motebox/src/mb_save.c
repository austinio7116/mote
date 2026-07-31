/*
 * Motebox — save and load.
 *
 * A god sim has to save the WORLD, not a score: it is the player's world, and
 * losing it to a flat battery is the one unforgivable bug. So the whole thing
 * goes: five map layers, every unit, every village and kingdom, the chronicle
 * ring, the age, the laws and the Faith.
 *
 * The layers are RLE'd, which is not a micro-optimisation — a young world is
 * mostly ocean and mostly one biome per region, so 14 KB of biome compresses to
 * about a tenth of that, and the whole save lands in a few kilobytes. A ruined
 * world compresses worse, which is the right way round: the more you have done to
 * a world, the more there is to remember.
 *
 * Written through kv_save (ABI v38) under one key per slot.
 */
#include "mb.h"
#include <string.h>

#define SAVE_MAGIC   0x4d425831u      /* "MBX1" */
/* v2: units gained a given name, a spouse, a trade and a haul destination; villages
 * gained a lord name and age, a creed, a specialisation, a tier and a research store. */
/* v3: villages gained `coast`, the water count a quay is gated on. */
#define SAVE_VERSION 5   /* v5: Village gained ripe/fallow and the work sites */

typedef struct {
    uint32_t magic, version;
    uint32_t seed;
    int32_t  tick;
    uint8_t  shape, climate, sea, age;
    uint16_t laws;
    int32_t  faith;
    uint8_t  mode, cx, cy, god;        /* the view, so a load resumes where you were */
    uint16_t nunits, nv, nk, pad;
} SaveHead;

/* --- RLE ---------------------------------------------------------------- *
 * Byte-oriented: a run is (count, value) with count 1..255. On terrain that
 * repeats in long horizontal stretches this is most of the win, and it decodes in
 * one pass with no dictionary. */
static int rle_pack(const uint8_t *src, int n, uint8_t *dst, int cap)
{
    int o = 0, i = 0;
    while (i < n) {
        uint8_t v = src[i];
        int run = 1;
        while (i + run < n && src[i + run] == v && run < 255) run++;
        if (o + 2 > cap) return -1;
        dst[o++] = (uint8_t)run;
        dst[o++] = v;
        i += run;
    }
    return o;
}

static int rle_unpack(const uint8_t *src, int n, uint8_t *dst, int cap)
{
    int o = 0, i = 0;
    while (i + 1 < n) {
        int run = src[i++];
        uint8_t v = src[i++];
        if (o + run > cap) return -1;
        for (int k = 0; k < run; k++) dst[o++] = v;
    }
    return o;
}

/* One scratch buffer, sized for the worst case (no run longer than 1 anywhere) of
 * the biggest layer, reused for every layer in turn. */
static uint8_t *s_buf;
#define BUFCAP (NC * 2 + 64)

void mb_save_init(void) { s_buf = (uint8_t *)g_api->alloc(BUFCAP); }

/* --- the blob ----------------------------------------------------------- */

static int append(uint8_t *out, int *o, int cap, const void *src, int n)
{
    if (*o + n > cap) return 0;
    memcpy(out + *o, src, (size_t)n);
    *o += n;
    return 1;
}

static int append_layer(uint8_t *out, int *o, int cap, const uint8_t *layer)
{
    int packed = rle_pack(layer, NC, s_buf, BUFCAP);
    if (packed < 0) return 0;
    uint32_t len = (uint32_t)packed;
    if (!append(out, o, cap, &len, 4)) return 0;
    return append(out, o, cap, s_buf, packed);
}

static int read_layer(const uint8_t *in, int *o, int n, uint8_t *layer)
{
    if (*o + 4 > n) return 0;
    uint32_t len;
    memcpy(&len, in + *o, 4); *o += 4;
    if (*o + (int)len > n) return 0;
    int got = rle_unpack(in + *o, (int)len, layer, NC);
    *o += (int)len;
    return got == NC;
}

/* The save buffer. Worst case is five packed layers plus the tables; in practice
 * a settled world is a few KB. Claimed from the arena once. */
#define SAVECAP (NC + 24 * 1024)
static uint8_t *s_save;

int mb_save_write(int slot, int cx, int cy, int god)
{
    if (!s_save) s_save = (uint8_t *)g_api->alloc(SAVECAP);
    if (!s_save) return 0;

    SaveHead h;
    memset(&h, 0, sizeof h);
    h.magic = SAVE_MAGIC; h.version = SAVE_VERSION;
    h.seed = mb_w.seed; h.tick = mb_w.tick;
    h.shape = mb_w.shape; h.climate = mb_w.climate; h.sea = mb_w.sea;
    h.age = (uint8_t)mb_age_id();
    h.laws = mb_law_bits();
    h.faith = mb_faith();
    h.mode = (uint8_t)mb_mode();
    h.cx = (uint8_t)cx; h.cy = (uint8_t)cy; h.god = (uint8_t)god;
    h.nunits = (uint16_t)mb_nu;

    int o = 0;
    if (!append(s_save, &o, SAVECAP, &h, sizeof h)) return 0;
    if (!append_layer(s_save, &o, SAVECAP, mb_w.biome)) return 0;
    if (!append_layer(s_save, &o, SAVECAP, mb_w.elev))  return 0;
    if (!append_layer(s_save, &o, SAVECAP, mb_w.obj))   return 0;
    if (!append_layer(s_save, &o, SAVECAP, mb_w.flux))  return 0;
    if (!append_layer(s_save, &o, SAVECAP, mb_w.claim)) return 0;
    if (!append(s_save, &o, SAVECAP, mb_u, (int)(sizeof(Unit) * (size_t)mb_nu))) return 0;
    if (!append(s_save, &o, SAVECAP, mb_v, (int)sizeof mb_v)) return 0;
    if (!append(s_save, &o, SAVECAP, mb_k, (int)sizeof mb_k)) return 0;

    char key[16];
    key[0] = 'm'; key[1] = 'b'; key[2] = 'x'; key[3] = (char)('0' + (slot % 10)); key[4] = 0;
    return g_api->kv_save(key, s_save, (uint32_t)o) ? o : 0;
}

int mb_save_read(int slot, int *cx, int *cy, int *god)
{
    if (!s_save) s_save = (uint8_t *)g_api->alloc(SAVECAP);
    if (!s_save) return 0;
    char key[16];
    key[0] = 'm'; key[1] = 'b'; key[2] = 'x'; key[3] = (char)('0' + (slot % 10)); key[4] = 0;
    int n = (int)g_api->kv_load(key, s_save, SAVECAP);
    if (n < (int)sizeof(SaveHead)) return 0;

    SaveHead h;
    memcpy(&h, s_save, sizeof h);
    /* Refuse a foreign or older blob rather than reading garbage into the world:
     * a corrupt load in a game whose whole content is one world is worse than no
     * load at all. */
    if (h.magic != SAVE_MAGIC || h.version != SAVE_VERSION) return 0;

    int o = (int)sizeof h;
    if (!read_layer(s_save, &o, n, mb_w.biome)) return 0;
    if (!read_layer(s_save, &o, n, mb_w.elev))  return 0;
    if (!read_layer(s_save, &o, n, mb_w.obj))   return 0;
    if (!read_layer(s_save, &o, n, mb_w.flux))  return 0;
    if (!read_layer(s_save, &o, n, mb_w.claim)) return 0;

    int ubytes = (int)(sizeof(Unit) * (size_t)h.nunits);
    if (o + ubytes > n) return 0;
    memset(mb_u, 0, sizeof(Unit) * MAXU);
    memcpy(mb_u, s_save + o, (size_t)ubytes); o += ubytes;
    mb_nu = h.nunits;

    if (o + (int)sizeof mb_v > n) return 0;
    memcpy(mb_v, s_save + o, sizeof mb_v); o += (int)sizeof mb_v;
    if (o + (int)sizeof mb_k > n) return 0;
    memcpy(mb_k, s_save + o, sizeof mb_k); o += (int)sizeof mb_k;

    mb_w.seed = h.seed; mb_w.tick = h.tick;
    mb_w.shape = h.shape; mb_w.climate = h.climate; mb_w.sea = h.sea;
    mb_age_set(h.age);
    mb_law_set_bits(h.laws);
    mb_faith_set(h.faith);
    mb_mode_set(h.mode);
    *cx = h.cx; *cy = h.cy; *god = h.god;

    /* The population counters live outside the unit array, so they have to be
     * rebuilt from it — the alternative (saving them) lets them disagree. */
    mb_unit_recount();
    /* Every village's commute field is derived, so it is not saved; mark them all
     * dirty and the civ tick rebuilds one per tick. */
    for (int v = 1; v < MAXV; v++) if (mb_v[v].alive) mb_v[v].dirty = 1;
    return 1;
}

int mb_save_exists(int slot)
{
    char key[16];
    key[0] = 'm'; key[1] = 'b'; key[2] = 'x'; key[3] = (char)('0' + (slot % 10)); key[4] = 0;
    uint8_t probe[8];
    return (int)g_api->kv_load(key, probe, sizeof probe) > 0;
}
