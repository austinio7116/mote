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
#define SAVE_VERSION 10  /* v10: + voyages (the Unit struct grew a boat) */

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

/* --- WHY THIS IS SEVERAL BLOBS AND NOT ONE ------------------------------
 *
 * It was one blob in a single 33 KB buffer (`NC + 24 KB`), and it OVERFLOWED on any world
 * anybody had actually played: five RLE'd layers can each reach 2 x NC when a developed map
 * stops repeating, the unit table alone is MAXU x sizeof(Unit), and the villages have grown all
 * session. Save then returned 0 and the god menu said SAVE FAILED — with no clue that the
 * cause was arithmetic rather than storage.
 *
 * kv_save takes any number of named blobs, so there is no reason to concatenate them: the
 * tables are written STRAIGHT OUT OF MEMORY with no copy at all, and only the map layers need a
 * buffer — one layer at a time, so the buffer is NC + a few bytes instead of everything at once.
 * The peak allocation went from 33 KB to 9 KB and the failure mode went away.
 *
 * Layers also fall back to RAW when RLE would be bigger, which is exactly the case that used to
 * blow the cap: a chewed-up world is the one that compresses worst. */
#define LAYCAP (NC + 8)
static uint8_t *s_lay;

/* mbx<slot><tag>: h head, b biome, e elev, o obj, f flux, c claim, u units, v villages,
 * k kingdoms. Short ASCII, no '/', as the kv contract asks. */
static void sv_key(char *key, int slot, char tag)
{
    key[0] = 'm'; key[1] = 'b'; key[2] = 'x';
    key[3] = (char)('0' + (slot % 10)); key[4] = tag; key[5] = 0;
}

/* One layer: [0]=RLE with a 16-bit length, or [1]=raw NC bytes. */
static int sv_layer(int slot, char tag, const uint8_t *layer)
{
    char key[16]; sv_key(key, slot, tag);
    int packed = rle_pack(layer, NC, s_lay + 3, LAYCAP - 3);
    if (packed > 0 && packed + 3 < NC + 1) {
        s_lay[0] = 0;
        s_lay[1] = (uint8_t)(packed & 0xFF);
        s_lay[2] = (uint8_t)(packed >> 8);
        return g_api->kv_save(key, s_lay, packed + 3) > 0;
    }
    s_lay[0] = 1;
    memcpy(s_lay + 1, layer, NC);
    return g_api->kv_save(key, s_lay, NC + 1) > 0;
}

static int ld_layer(int slot, char tag, uint8_t *layer)
{
    char key[16]; sv_key(key, slot, tag);
    int n = (int)g_api->kv_load(key, s_lay, LAYCAP);
    if (n < 2) return 0;
    if (s_lay[0] == 1) {
        if (n < NC + 1) return 0;
        memcpy(layer, s_lay + 1, NC);
        return 1;
    }
    int len = (int)s_lay[1] | ((int)s_lay[2] << 8);
    if (len <= 0 || len + 3 > n) return 0;
    return rle_unpack(s_lay + 3, len, layer, NC) == NC;
}

int mb_save_write(int slot, int cx, int cy, int god)
{
    if (!s_lay) s_lay = (uint8_t *)g_api->alloc(LAYCAP);
    if (!s_lay) return 0;

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
    h.nv = MAXV; h.nk = MAXK;

    if (!sv_layer(slot, 'b', mb_w.biome)) return 0;
    if (!sv_layer(slot, 'e', mb_w.elev))  return 0;
    if (!sv_layer(slot, 'o', mb_w.obj))   return 0;
    if (!sv_layer(slot, 'f', mb_w.flux))  return 0;
    if (!sv_layer(slot, 'c', mb_w.claim)) return 0;
    /* THE ROAD LAYER WAS NEVER SAVED. It is its own layer (not a biome) and it was simply
     * missing from the list, so every load came back to a town with no streets in it. */
    if (!sv_layer(slot, 'r', mb_w.road))  return 0;

    char key[16];
    sv_key(key, slot, 'u');
    if (g_api->kv_save(key, mb_u, (int)(sizeof(Unit) * (size_t)mb_nu)) <= 0) return 0;
    sv_key(key, slot, 'v');
    if (g_api->kv_save(key, mb_v, (int)sizeof mb_v) <= 0) return 0;
    sv_key(key, slot, 'k');
    if (g_api->kv_save(key, mb_k, (int)sizeof mb_k) <= 0) return 0;
    /* the head LAST, so a half-written save has no valid head and reads as absent */
    sv_key(key, slot, 'h');
    return g_api->kv_save(key, &h, (int)sizeof h) > 0;
}

int mb_save_read(int slot, int *cx, int *cy, int *god)
{
    if (!s_lay) s_lay = (uint8_t *)g_api->alloc(LAYCAP);
    if (!s_lay) return 0;

    char key[16];
    SaveHead h;
    sv_key(key, slot, 'h');
    if ((int)g_api->kv_load(key, &h, (int)sizeof h) < (int)sizeof h) return 0;
    /* Refuse a foreign or older blob rather than reading garbage into the world: a corrupt
     * load in a game whose whole content is one world is worse than no load at all. */
    if (h.magic != SAVE_MAGIC || h.version != SAVE_VERSION) return 0;
    if (h.nunits > MAXU) return 0;

    if (!ld_layer(slot, 'b', mb_w.biome)) return 0;
    if (!ld_layer(slot, 'e', mb_w.elev))  return 0;
    if (!ld_layer(slot, 'o', mb_w.obj))   return 0;
    if (!ld_layer(slot, 'f', mb_w.flux))  return 0;
    if (!ld_layer(slot, 'c', mb_w.claim)) return 0;
    if (!ld_layer(slot, 'r', mb_w.road))  return 0;

    memset(mb_u, 0, sizeof(Unit) * MAXU);
    sv_key(key, slot, 'u');
    int ubytes = (int)(sizeof(Unit) * (size_t)h.nunits);
    if (ubytes && (int)g_api->kv_load(key, mb_u, ubytes) < ubytes) return 0;
    mb_nu = h.nunits;
    sv_key(key, slot, 'v');
    if ((int)g_api->kv_load(key, mb_v, (int)sizeof mb_v) < (int)sizeof mb_v) return 0;
    sv_key(key, slot, 'k');
    if ((int)g_api->kv_load(key, mb_k, (int)sizeof mb_k) < (int)sizeof mb_k) return 0;

    mb_w.seed = h.seed; mb_w.tick = h.tick;
    mb_w.shape = h.shape; mb_w.climate = h.climate; mb_w.sea = h.sea;
    mb_age_set(h.age);
    mb_law_set_bits(h.laws);
    mb_faith_set(h.faith);
    mb_mode_set(h.mode);
    *cx = h.cx; *cy = h.cy; *god = h.god;

    /* The population counters live outside the unit array, so they have to be rebuilt from
     * it — the alternative (saving them) lets them disagree. */
    mb_unit_recount();
    /* Every village's commute field is derived, so it is not saved; mark them all dirty and
     * the civ tick rebuilds one per tick. And the band map is derived from the biome. */
    for (int v = 1; v < MAXV; v++) if (mb_v[v].alive) mb_v[v].dirty = 1;
    mb_bands_rebuild();
    return 1;
}

int mb_save_exists(int slot)
{
    char key[16]; sv_key(key, slot, 'h');
    SaveHead probe;
    return (int)g_api->kv_load(key, &probe, (int)sizeof probe) >= (int)sizeof probe;
}
