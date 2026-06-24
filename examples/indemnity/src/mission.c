/*
 * ThumbyElite — procedural missions + faction reputation.
 */
#include "mission.h"
#include "elite_entity.h"
#include "elite_player.h"
#include "econ.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

Mission g_missions[MAX_MISSIONS];
int8_t  g_rep[N_FACTIONS];

const char *k_faction_names[N_FACTIONS] = {
    "COALITION", "DOMINION", "FREEHOLDS",
};

static uint32_t s_visit_salt;     /* changes offers between visits */

Faction system_faction(SysAddr a) {
    /* Big contiguous blobs: hash coarse 4x4-sector cells. */
    uint32_t h = (uint32_t)(a.sx >> 2) * 2654435761u ^
                 (uint32_t)(a.sy >> 2) * 668265263u ^
                 (galaxy_get_seed() * 951274213u);
    h ^= h >> 13;
    return (Faction)(h % N_FACTIONS);
}

void missions_init(void) {
    memset(g_missions, 0, sizeof g_missions);
    memset(g_rep, 0, sizeof g_rep);
    s_visit_salt = 0;
}

/* Public wrapper (distress rescues pay rep from elite_game). */
void mission_rep_add_public(int faction, int amt);
static void rep_add(int faction, int amt);
void mission_rep_add_public(int faction, int amt) {
    rep_add(faction, amt);
}

static void rep_add(int faction, int amt) {
    int v = g_rep[faction] + amt;
    if (v > 100) v = 100;
    if (v < -100) v = -100;
    g_rep[faction] = (int8_t)v;
}

/* --- faction war --------------------------------------------------------
 * Factions own 4x4-sector cells (system_faction). A cell that borders a
 * different-faction cell is a FRONT; systems in it are contested. */
static Faction cell_faction(int cx, int cy) {
    uint32_t h = (uint32_t)cx * 2654435761u ^ (uint32_t)cy * 668265263u ^
                 (galaxy_get_seed() * 951274213u);
    h ^= h >> 13;
    return (Faction)(h % N_FACTIONS);
}

bool faction_contested(SysAddr a, Faction *enemy) {
    int cx = a.sx >> 2, cy = a.sy >> 2;
    Faction own = cell_faction(cx, cy);
    static const int k_n4[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    for (int i = 0; i < 4; i++) {
        Faction f = cell_faction(cx + k_n4[i][0], cy + k_n4[i][1]);
        if (f != own) {
            if (enemy) *enemy = f;
            return true;
        }
    }
    return false;
}

bool mission_near_front(SysAddr a) {
    if (faction_contested(a, NULL)) return true;
    /* one cell of slack: garrison systems behind the line recruit too */
    int cx = a.sx >> 2, cy = a.sy >> 2;
    Faction own = cell_faction(cx, cy);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (cell_faction(cx + dx, cy + dy) != own) return true;
    return false;
}

static bool s_war_active;                /* anchored at the target beacon */
void mission_warzone_set_active(bool active) { s_war_active = active; }

bool mission_warzone_here(SysAddr a, int *kills_left) {
    for (int i = 0; i < MAX_MISSIONS; i++) {
        const Mission *m = &g_missions[i];
        if (m->type == MIS_WARZONE && !m->done &&
            sysaddr_eq(m->target, a)) {
            if (kills_left) *kills_left = m->count;
            return true;
        }
    }
    return false;
}

/* Find a contested friendly system within recruiting reach. */
static bool find_warzone(SysAddr from, uint32_t salt, Faction fac,
                         SysAddr *out, Faction *enemy) {
    for (int tries = 0; tries < 32; tries++) {
        uint32_t h = salt * 2654435761u + (uint32_t)tries * 131u;
        h ^= h >> 15;
        int dx = (int)(h % 9u) - 4;
        int dy = (int)((h >> 8) % 9u) - 4;
        SysAddr a = { from.sx + dx, from.sy + dy, 0 };
        int n = galaxy_sector_stars(a.sx, a.sy);
        if (n == 0) continue;
        a.idx = (uint8_t)((h >> 16) % (uint32_t)n);
        Faction en;
        if (!faction_contested(a, &en)) continue;
        if (system_faction(a) != fac) continue;   /* defend OUR side */
        *out = a;
        *enemy = en;
        return true;
    }
    return false;
}

static bool warzone_build(const SystemInfo *si, uint32_t h, Mission *m) {
    Faction fac = system_faction(si->addr);
    SysAddr dest;
    Faction enemy;
    if (!find_warzone(si->addr, h, fac, &dest, &enemy)) return false;
    if (mission_warzone_here(dest, NULL)) return false;   /* one per zone */
    /* Loyalty (user req): factions only trust proven friends with war
     * work — rep 2+ to be offered ANY contract, and the ladder climbs
     * with standing (ELITE wars need rep 20+, ~3 completed contracts).
     * War pay also rewards loyalty at double the usual rep scaling. */
    int rep = g_rep[fac];
    if (rep < 2) return false;
    int tmax = rep / 5;
    if (tmax > 4) tmax = 4;
    float rep_bonus = 1.0f + 0.008f * (float)rep;
    int roll = (int)((h >> 24) % 100u);
    int t = (roll < 30) ? 0 : (roll < 55) ? 1 : (roll < 75) ? 2
          : (roll < 90) ? 3 : 4;
    if (t > tmax) t = tmax;
    static const int32_t k_war_base[5] = { 2000, 3200, 5000, 7500, 19000 };
    static const int32_t k_war_var[5]  = { 800, 1200, 1800, 2500, 3000 };
    static const uint8_t k_war_n[5]    = { 4, 5, 5, 6, 6 };
    m->type = MIS_WARZONE;
    m->faction = (uint8_t)fac;
    m->tier = (uint8_t)t;                /* battle tier = enemy rank */
    m->target = dest;
    m->count = (uint8_t)(k_war_n[t] + ((h >> 18) & 1u));
    m->reward = (int32_t)((k_war_base[t] +
                           (int32_t)((h >> 9) % (uint32_t)k_war_var[t])) *
                          rep_bonus);
    char dname[14];
    galaxy_system_name(dest, dname);
    snprintf(m->label, sizeof m->label, "%s WAR>%s",
             k_tier_names[t], dname);
    (void)enemy;
    return true;
}

/* Battle tier of the active contract here, or -1. */
int mission_warzone_tier(SysAddr a) {
    for (int i = 0; i < MAX_MISSIONS; i++)
        if (g_missions[i].type == MIS_WARZONE && !g_missions[i].done &&
            sysaddr_eq(g_missions[i].target, a))
            return g_missions[i].tier;
    return -1;
}

bool mission_grant_warzone(const SystemInfo *si) {
    int slot = -1;
    for (int i = 0; i < MAX_MISSIONS; i++)
        if (g_missions[i].type == MIS_NONE) { slot = i; break; }
    if (slot < 0) return false;
    Mission m;
    memset(&m, 0, sizeof m);
    uint32_t h = (uint32_t)(si->seed >> 10) ^ s_visit_salt ^ 0x3AA3u;
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    if (!warzone_build(si, h, &m)) return false;
    g_missions[slot] = m;
    return true;
}

/* Find a nearby system with a station (delivery destinations). */
static bool find_dest(SysAddr from, uint32_t salt, SysAddr *out, int *out_st) {
    for (int tries = 0; tries < 24; tries++) {
        uint32_t h = salt * 2654435761u + (uint32_t)tries * 97u;
        h ^= h >> 15;
        int dx = (int)(h % 7u) - 3;
        int dy = (int)((h >> 8) % 7u) - 3;
        if (!dx && !dy) continue;
        SysAddr a = { from.sx + dx, from.sy + dy, 0 };
        int n = galaxy_sector_stars(a.sx, a.sy);
        if (n == 0) continue;
        a.idx = (uint8_t)((h >> 16) % (uint32_t)n);
        SystemInfo si;
        galaxy_generate(a, &si);
        if (si.n_stations == 0) continue;
        *out = a;
        *out_st = (int)((h >> 20) % si.n_stations);
        return true;
    }
    return false;
}

#ifdef ELITE_STYLE_LAB
int g_force_war_offer = 0;   /* guide-screenshot hook */
#endif
void mission_make_offers(const SystemInfo *si, int station,
                         Mission out[MISSION_OFFERS]) {
    Faction fac = system_faction(si->addr);
    float rep_bonus = 1.0f + 0.004f * (float)g_rep[fac];
    for (int i = 0; i < MISSION_OFFERS; i++) {
        Mission *m = &out[i];
        memset(m, 0, sizeof *m);
#ifdef ELITE_STYLE_LAB
        if (g_force_war_offer && i == 0) {
            uint32_t wh = (uint32_t)(si->seed >> 10) ^ s_visit_salt ^ 0x3AA3u;
            wh ^= wh >> 13; wh *= 1274126177u; wh ^= wh >> 16;
            if (warzone_build(si, wh, m)) continue;
        }
#endif
        uint32_t h = (uint32_t)(si->seed >> 12) ^ s_visit_salt ^
                     (uint32_t)((station + 1) * 7919) ^
                     (uint32_t)(i * 104729);
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;

        int roll = (int)(h % 100u);
        m->faction = (uint8_t)fac;

        /* Near a front, the war recruits hard — a quarter of the board
         * becomes HOLD-the-line contracts (replacing some deliveries). */
        if (mission_near_front(si->addr) && roll < 25 &&
            warzone_build(si, h, m))
            continue;

        if (roll < 45) {
            /* Delivery: goods the local economy exports. */
            SysAddr dest;
            int dst_st;
            if (!find_dest(si->addr, h, &dest, &dst_st)) continue;
            m->type = MIS_DELIVERY;
            m->good = (uint8_t)((h >> 8) % 16u);
            m->count = (uint8_t)(2 + ((h >> 16) % 6u));
            m->target = dest;
            m->station = (uint8_t)dst_st;
            char dname[14];
            galaxy_system_name(dest, dname);
            float dx = 0, dy = 0, px = 0, py = 0;
            galaxy_star_pos(dest, &dx, &dy);
            galaxy_star_pos(si->addr, &px, &py);
            float dist = sqrtf((dx - px) * (dx - px) + (dy - py) * (dy - py));
            m->reward = (int32_t)((120 + m->count * k_goods[m->good].base / 2 +
                                   (int)(dist * 50)) * rep_bonus);
            snprintf(m->label, sizeof m->label, "%dX %s>%s",
                     m->count, k_goods[m->good].name, dname);
        } else if (roll < 75) {
            m->type = MIS_CULL;
            m->count = (uint8_t)(2 + ((h >> 9) % 4u));
            m->reward = (int32_t)(m->count * 320 * rep_bonus);
            snprintf(m->label, sizeof m->label, "CULL %d PIRATES", m->count);
        } else if (roll < 92) {
            /* Bounty: a marked pilot waits at a nearby beacon. Tier
             * varies — EASY marks for starter ships, ACE paydays for
             * the brave (user spec). */
            SysAddr dest;
            int dst_st;
            if (!find_dest(si->addr, h ^ 0xB011u, &dest, &dst_st)) continue;
            m->type = MIS_BOUNTY;
            m->target = dest;
            m->tier = (uint8_t)(1 + ((h >> 24) % 4u));
            static const int k_pay[5] = { 0, 600, 1300, 2800, 6500 };
            static const char *k_tag[5] = { "", "EASY", "RISKY", "HARD",
                                            "ACE" };
            m->reward = (int32_t)(k_pay[m->tier] * rep_bonus);
            char dname[14];
            galaxy_system_name(dest, dname);
            snprintf(m->label, sizeof m->label, "%s MARK>%s",
                     k_tag[m->tier], dname);
        } else {
            /* ASSASSINATE: murder a marked civilian. Pays HEAVY but the
             * kill brands you a fugitive (user). A dark contract. */
            SysAddr dest;
            int dst_st;
            if (!find_dest(si->addr, h ^ 0x4551u, &dest, &dst_st)) continue;
            m->type = MIS_ASSASSINATE;
            m->target = dest;
            m->tier = (uint8_t)(1 + ((h >> 24) % 3u));
            m->reward = (int32_t)((2600 + m->tier * 1100) * rep_bonus);
            char dname[14];
            galaxy_system_name(dest, dname);
            snprintf(m->label, sizeof m->label, "HIT >%s", dname);
        }
    }
}

bool mission_assassinate_here(SysAddr a) {
    for (int i = 0; i < MAX_MISSIONS; i++)
        if (g_missions[i].type == MIS_ASSASSINATE && !g_missions[i].done &&
            sysaddr_eq(g_missions[i].target, a))
            return true;
    return false;
}

bool mission_accept(const Mission *m) {
    for (int i = 0; i < MAX_MISSIONS; i++) {
        if (g_missions[i].type != MIS_NONE) continue;
        g_missions[i] = *m;
        /* Delivery missions hand you the cargo. */
        if (m->type == MIS_DELIVERY) {
            int room = player_cargo_cap() - player_cargo_total();
            if (room < m->count) { g_missions[i].type = MIS_NONE; return false; }
            g_player.cargo[m->good] += m->count;
        }
        return true;
    }
    return false;
}

void mission_on_kill(int victim_tier, bool was_bounty_mark,
                     bool was_civilian) {
    for (int i = 0; i < MAX_MISSIONS; i++) {
        Mission *m = &g_missions[i];
        if (m->done) continue;
        if (m->type == MIS_CULL && m->count > 0 && !was_civilian) {
            m->count--;
            if (m->count == 0) m->done = true;
        } else if (m->type == MIS_BOUNTY && was_bounty_mark && !was_civilian) {
            m->done = true;
        } else if (m->type == MIS_ASSASSINATE && was_bounty_mark &&
                   was_civilian) {
            m->done = true;
        }
    }
    (void)victim_tier;
}

void mission_warzone_enemy_down(void) {
    if (!s_war_active) return;
    for (int i = 0; i < MAX_MISSIONS; i++) {
        Mission *m = &g_missions[i];
        if (m->type == MIS_WARZONE && !m->done && m->count > 0) {
            m->count--;
            if (m->count == 0) m->done = true;
            return;
        }
    }
}

int mission_bounty_tier_here(SysAddr a) {
    for (int i = 0; i < MAX_MISSIONS; i++)
        if (g_missions[i].type == MIS_BOUNTY && !g_missions[i].done &&
            sysaddr_eq(g_missions[i].target, a))
            return g_missions[i].tier;
    return -1;
}

bool mission_objective_here(SysAddr a) {
    for (int i = 0; i < MAX_MISSIONS; i++) {
        const Mission *m = &g_missions[i];
        if (m->done) continue;
        if ((m->type == MIS_BOUNTY || m->type == MIS_DELIVERY ||
             m->type == MIS_ASSASSINATE || m->type == MIS_WARZONE) &&
            sysaddr_eq(m->target, a))
            return true;
    }
    return false;
}

void mission_on_docked(const SystemInfo *si, int station) {
    (void)si; (void)station;
    s_visit_salt += 0x9E3779B9u;     /* fresh offers next visit */
}

int mission_collect(const SystemInfo *si, int station) {
    int paid = 0;
    for (int i = 0; i < MAX_MISSIONS; i++) {
        Mission *m = &g_missions[i];
        if (m->type == MIS_NONE) continue;

        /* Deliveries complete at their named station with cargo aboard. */
        if (m->type == MIS_DELIVERY && !m->done) {
            if (sysaddr_eq(m->target, si->addr) && station == m->station &&
                g_player.cargo[m->good] >= m->count) {
                g_player.cargo[m->good] -= m->count;
                m->done = true;
            }
        }
        if (!m->done) continue;

        paid += m->reward;
        g_player.credits += m->reward;
        rep_add(m->faction, (m->type == MIS_BOUNTY) ? 8
                          : (m->type == MIS_ASSASSINATE) ? 5
                          : (m->type == MIS_WARZONE) ? 7 : 4);
        if (m->type == MIS_WARZONE) {
            Faction en;                  /* tier is the BATTLE tier now */
            if (faction_contested(m->target, &en))
                rep_add(en, -7);         /* the other side remembers */
        }
        g_player.xp_trading += (m->type == MIS_DELIVERY) ? 2 : 0;
        g_player.xp_gunnery += (m->type != MIS_DELIVERY) ? 1 : 0;
        m->type = MIS_NONE;
    }
    return paid;
}
