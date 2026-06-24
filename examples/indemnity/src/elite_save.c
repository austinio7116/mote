/*
 * ThumbyElite — save game.
 *
 * Layout: header {magic, version, len, crc32} + payload {galaxy seed,
 * docked addr/station, kills, PlayerState, rep, missions}. Additive
 * schema: bump VERSION on change; loader rejects mismatches (fresh
 * start beats corrupt state).
 */
#include "elite_save.h"
#include "elite_audio.h"
#include <stddef.h>
#include "elite_player.h"
#include "elite_platform.h"
#include "mission.h"
#include "events.h"
#include <string.h>

#define SAVE_MAGIC   0x454C4954u   /* 'ELIT' */
#define SAVE_VERSION 6   /* v6: pending event transfer (v3/v4/v5 migrate) */

typedef struct {
    uint32_t magic, version, len, crc;
} SaveHeader;

typedef struct {
    uint32_t galaxy_seed;
    SysAddr  addr;
    uint8_t  station;
    uint8_t  pad[3];
    int32_t  kills;
    PlayerState player;
    int8_t   rep[N_FACTIONS];
    uint8_t  pad2;
    Mission  missions[MAX_MISSIONS];
    /* v5+: appended at the END so older saves migrate by zero-fill. */
    uint8_t  event_bits[EVENTS_BITS_LEN];      /* lore/flags/oneshots */
    uint8_t  event_recent[EVENTS_RECENT_LEN];  /* anti-repeat ring    */
    int32_t  event_pending;                    /* v6: OP_LATER credits */
} SavePayload;

typedef struct {
    SaveHeader h;
    SavePayload p;
} SaveBlob;

static uint32_t crc32_simple(const uint8_t *d, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (-(int)(c & 1)));
    }
    return ~c;
}

#define EV_TAIL (EVENTS_BITS_LEN + EVENTS_RECENT_LEN)

static bool read_blob(SaveBlob *blob) {
    memset(blob, 0, sizeof *blob);
    int n = plat_load((uint8_t *)blob, (int)sizeof *blob);
    if (n < (int)sizeof(SaveHeader)) return false;
    if (blob->h.magic != SAVE_MAGIC) return false;
    if (blob->h.len > sizeof(SavePayload)) return false;
    if (n < (int)(sizeof(SaveHeader) + blob->h.len)) return false;
    /* CRC covers the payload AS WRITTEN (h.len bytes) — older versions
     * are shorter; checking the current size would reject them all. */
    if (blob->h.crc != crc32_simple((const uint8_t *)&blob->p,
                                    (int)blob->h.len))
        return false;
    /* Additive tails by version: v5 lacks the pending transfer, v4
     * also lacks the event bits. Zero-fill what each is missing. */
    size_t v5_len = sizeof(SavePayload) - sizeof(int32_t);
    size_t v4_len = v5_len - EV_TAIL;
    if (blob->h.version == SAVE_VERSION) {
        if (blob->h.len != sizeof(SavePayload)) return false;
    } else if (blob->h.version == 5) {
        if (blob->h.len != v5_len) return false;
        memset((uint8_t *)&blob->p + v5_len, 0, sizeof(int32_t));
    } else if (blob->h.version == 4) {
        if (blob->h.len != v4_len) return false;
        memset((uint8_t *)&blob->p + v4_len, 0,
               sizeof(SavePayload) - v4_len);
    } else if (blob->h.version == 3) {
        /* v3 -> v4: PlayerState grew util_eq[2] -> [4]. The payload on
         * disk is 16 bytes shorter and everything after util_eq sits
         * earlier. Migrate by splitting at the insertion point. */
        if (blob->h.len + 2 * sizeof(WeaponInst) != v4_len) return false;
        uint8_t *p = (uint8_t *)&blob->p;
        size_t cut = offsetof(SavePayload, player) +
                     offsetof(PlayerState, util_eq) +
                     2 * sizeof(WeaponInst);
        size_t grow = 2 * sizeof(WeaponInst);
        size_t tail = blob->h.len - cut;
        memmove(p + cut + grow, p + cut, tail);
        memset(p + cut, 0, grow);          /* new bays arrive empty */
        memset(p + v4_len, 0, sizeof(SavePayload) - v4_len);
    } else {
        return false;
    }
    return true;
}

bool save_exists(void) {
    SaveBlob b;
    return read_blob(&b);
}

bool save_write(SysAddr addr, int station, int kills) {
    SaveBlob b;
    memset(&b, 0, sizeof b);
    b.p.galaxy_seed = galaxy_get_seed();
    b.p.addr = addr;
    b.p.station = (uint8_t)station;
    b.p.kills = kills;
    b.p.player = g_player;
    memcpy(b.p.rep, g_rep, sizeof b.p.rep);
    memcpy(b.p.missions, g_missions, sizeof b.p.missions);
    memcpy(b.p.event_bits, events_save_bits(), EVENTS_BITS_LEN);
    memcpy(b.p.event_recent, events_save_recent(), EVENTS_RECENT_LEN);
    b.p.event_pending = *events_save_pending();
    b.h.magic = SAVE_MAGIC;
    b.h.version = SAVE_VERSION;
    b.h.len = sizeof(SavePayload);
    b.h.crc = crc32_simple((const uint8_t *)&b.p, (int)sizeof b.p);
    return plat_save((const uint8_t *)&b, (int)sizeof b) != 0;
}

/* True if the stored save belongs to the given galaxy — insurance
 * must NOT resurrect a pilot into a previous campaign (NEW GAME never
 * deletes the old save; first dock of the new run overwrites it). */
bool save_matches_galaxy(uint32_t seed) {
    SaveBlob b;
    return read_blob(&b) && b.p.galaxy_seed == seed;
}

void save_wipe(void) {
    SaveHeader h;
    memset(&h, 0, sizeof h);          /* bad magic = no save */
    plat_save((const uint8_t *)&h, (int)sizeof h);
}

bool save_load(SaveMeta *out) {
    SaveBlob b;
    if (!read_blob(&b)) return false;
    galaxy_set_seed(b.p.galaxy_seed);
    g_player = b.p.player;
    memcpy(g_rep, b.p.rep, sizeof b.p.rep);
    memcpy(g_missions, b.p.missions, sizeof b.p.missions);
    memcpy(events_save_bits(), b.p.event_bits, EVENTS_BITS_LEN);
    memcpy(events_save_recent(), b.p.event_recent, EVENTS_RECENT_LEN);
    *events_save_pending() = b.p.event_pending;
    out->addr = b.p.addr;
    out->station = b.p.station;
    out->kills = b.p.kills;
    return true;
}

/* --- multi-save: slot selection, peek, allocate, delete -------------- */
static int s_slot = 0;

void save_set_slot(int slot) {
    s_slot = (slot < 0) ? 0 : slot;
    plat_save_slot(s_slot);
}
int save_get_slot(void)  { return s_slot; }
int save_max_slots(void) { return plat_save_max_slots(); }

bool save_peek(int slot, SavePeek *out) {
    out->valid = false;
    plat_save_slot(slot);
    SaveBlob b;
    bool ok = read_blob(&b);
    plat_save_slot(s_slot);            /* restore the active slot */
    if (!ok) return false;
    out->valid       = true;
    out->credits     = b.p.player.credits;
    out->kills       = b.p.kills;
    out->hull_id     = b.p.player.hull_id;
    out->hull_seed   = b.p.player.hull_seed;
    out->galaxy_seed = b.p.galaxy_seed;
    out->addr        = b.p.addr;
    return true;
}

int save_alloc_slot(void) {
    int mx = plat_save_max_slots();
    for (int s = 0; s < mx; s++) {
        plat_save_slot(s);
        SaveBlob b;
        bool used = read_blob(&b);
        plat_save_slot(s_slot);
        if (!used) return s;
    }
    return -1;                         /* all slots full */
}

void save_delete(int slot) {
    plat_save_remove(slot);
    if (slot == s_slot) plat_save_slot(s_slot);   /* keep active slot path live */
}
