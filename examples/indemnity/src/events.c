/*
 * ThumbyElite — random event engine (see events.h).
 */
#include "events.h"
#include "enames.h"
#include "mission.h"
#include "econ.h"
#include "elite_player.h"
#include "elite_entity.h"
#include "elite_weapons.h"
#include <string.h>
#include <stdio.h>

/* elite_game.c: spawn a hostile wing outside the station (they wait). */
void elite_game_event_ambush(int n, int tier);

#define EVENT_DOCK_PCT    35
#define EVENT_BAR_PCT     60
#define EVENT_SPACE_PCT   100   /* the derelict spawn WAS the odds */
#define EVENT_ARRIVAL_PCT 14
static int s_override = -1;            /* -1 = per-trigger defaults */
void events_set_chance(int pct) { s_override = pct; }
static int chance_for(int trig) {
    if (s_override >= 0) return s_override;
    switch (trig) {
    case TRIG_BAR:     return EVENT_BAR_PCT;
    case TRIG_SPACE:   return EVENT_SPACE_PCT;
    case TRIG_ARRIVAL: return EVENT_ARRIVAL_PCT;
    default:           return EVENT_DOCK_PCT;
    }
}

/* Persistent bits: lore 0..127, story flags 128..159, oneshot-seen
 * 160 + event id. Carried by the save (events_save_bits). */
static uint8_t  s_bits[EVENTS_BITS_LEN];
static uint8_t  s_recent[EVENTS_RECENT_LEN];   /* last picks, anti-repeat */
static uint8_t  s_recent_at;
static uint32_t s_salt;
static int32_t  s_pending_cr;    /* OP_LATER transfers, paid at dock */

/* Current pick (the modal + run_choice operate on this). */
static const SystemInfo *s_si;
static int      s_station;
static uint32_t s_npc_seed;
static uint32_t s_outcome_seed;     /* branch rng — fixed at pick time */

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16; return x;
}

static bool bit(int i)     { return (s_bits[(i >> 3) & 31] >> (i & 7)) & 1; }
static void bit_set(int i) { s_bits[(i >> 3) & 31] |= (uint8_t)(1 << (i & 7)); }

bool events_lore_seen(int id) { return bit(id & 127); }
bool events_flag(int id)      { return bit(128 + (id & 31)); }
void events_set_flag(int id)  { bit_set(128 + (id & 31)); }   /* cheat/climax */
void events_set_lore(int id)  { bit_set(id & 127); }          /* cheat: pre-seen */

/* Fetch a specific event by id (for scripted opens like the recall hail). */
const Event *events_get(int id) {
    for (int i = 0; i < k_n_events; i++)
        if (k_events[i].id == id) return &k_events[i];
    return NULL;
}
uint8_t *events_save_bits(void)   { return s_bits; }
uint8_t *events_save_recent(void) { return s_recent; }
void events_set_salt(uint32_t salt) { s_salt = salt; }
uint32_t events_npc_seed(void) { return s_npc_seed; }

void events_init(void) {
    memset(s_bits, 0, sizeof s_bits);
    memset(s_recent, 0xFF, sizeof s_recent);
    s_recent_at = 0;
    s_salt = 0;
    s_pending_cr = 0;
}

/* --- gates -------------------------------------------------------------- */
static int illegal_units(void) {
    int n = 0;
    for (int g = 0; g < N_GOODS; g++)
        if (k_goods[g].flags & GOOD_ILLEGAL) n += g_player.cargo[g];
    return n;
}

static bool gate_ok(uint16_t gate, const SystemInfo *si) {
    if ((gate & GATE_LAWFUL) && si->gov < GOV_CONFED) return false;
    if ((gate & GATE_ROUGH) && si->gov > GOV_FEUDAL) return false;
    if ((gate & GATE_THREAT) && si->threat < 2) return false;
    if ((gate & GATE_ILLEGAL) && illegal_units() == 0) return false;
    if ((gate & GATE_CARGO_SPACE) &&
        player_cargo_total() + 2 > player_cargo_cap()) return false;
    if ((gate & GATE_FUEL_SPARE) && g_player.fuel < 2.0f) return false;
    if ((gate & GATE_CLEAN) && g_player.legal != 0) return false;
    if ((gate & GATE_WANTED) && g_player.legal == 0) return false;
    if ((gate & GATE_HAS_MEDS) && g_player.cargo[5] == 0) return false;
    if ((gate & GATE_NO_ILLEGAL) && illegal_units() > 0) return false;
    if ((gate & GATE_FRONTLINE) && !mission_near_front(si->addr)) return false;
    if ((gate & GATE_REP_PLUS) &&
        g_rep[system_faction(si->addr)] < 2) return false;
    return true;
}

bool events_choice_enabled(const Event *ev, int choice) {
    if (choice < 0 || choice >= ev->n_choices) return false;
    const Choice *c = &ev->choices[choice];
    if (c->cost > 0 && g_player.credits < c->cost) return false;
    return gate_ok(c->gate, s_si);
}

/* --- selection ----------------------------------------------------------- */
static bool in_recent(uint8_t id) {
    for (int i = 0; i < EVENTS_RECENT_LEN; i++)
        if (s_recent[i] == id) return true;
    return false;
}

static bool eligible(const Event *e, int trig, const SystemInfo *si) {
    if (e->trig != trig) return false;
    if ((e->flags & EV_ONESHOT) && bit(160 + e->id)) return false;
    if (in_recent(e->id)) return false;
    if (e->need_flag && !bit(128 + ((e->need_flag - 1) & 31))) return false;
    if (e->not_flag && bit(128 + ((e->not_flag - 1) & 31))) return false;
    return gate_ok(e->gate, si);
}

static const Event *roll(const SystemInfo *si, int station, int trig) {
    s_salt += 0x9E3779B9u;
    uint32_t h = mix32((uint32_t)(si->seed >> 8) ^ s_salt ^
                       (uint32_t)((station + 1) * 0x85EBCA6Bu) ^
                       (uint32_t)(trig * 0x27D4EB2Fu));
    if ((int)(h % 100u) >= chance_for(trig)) return NULL;

    /* Weighted pick over the eligible pool. */
    int total = 0;
    for (int i = 0; i < k_n_events; i++)
        if (eligible(&k_events[i], trig, si)) total += k_events[i].weight;
    if (total <= 0) return NULL;
    int pick = (int)(mix32(h ^ 0xC2B2AE35u) % (uint32_t)total);
    const Event *ev = NULL;
    for (int i = 0; i < k_n_events; i++) {
        const Event *e = &k_events[i];
        if (!eligible(e, trig, si)) continue;
        pick -= e->weight;
        if (pick < 0) { ev = e; break; }
    }
    if (!ev) return NULL;

    s_si = si;
    s_station = station;
    /* Recurring characters carry a campaign-stable identity; everyone
     * else is minted from the pick. */
    s_npc_seed = ev->fixed_npc
                     ? mix32(galaxy_get_seed() ^
                             (uint32_t)ev->fixed_npc * 0x9E3779B9u)
                     : mix32(h ^ (uint32_t)ev->id * 0x9E3779B9u);
    s_outcome_seed = mix32(mix32(h) ^ 0x6A09E667u);
    if (ev->flags & EV_ONESHOT) bit_set(160 + ev->id);
    s_recent[s_recent_at] = ev->id;
    s_recent_at = (uint8_t)((s_recent_at + 1) % EVENTS_RECENT_LEN);
    return ev;
}

const Event *events_roll_dock(const SystemInfo *si, int station) {
    return roll(si, station, TRIG_DOCK);
}

const Event *events_roll_bar(const SystemInfo *si, int station) {
    return roll(si, station, TRIG_BAR);
}

const Event *events_roll_space(const SystemInfo *si) {
    return roll(si, -1, TRIG_SPACE);
}

const Event *events_roll_arrival(const SystemInfo *si) {
    return roll(si, -1, TRIG_ARRIVAL);
}

/* --- text ----------------------------------------------------------------
 * $N npc name, $S system, $T station, $F local faction, $G trade good. */
static int seeded_good(void) { return (int)(s_npc_seed % 16u); /* legal */ }

void events_expand(const char *tmpl, char *out, int cap) {
    int o = 0;
    for (const char *p = tmpl; *p && o < cap - 1; p++) {
        if (*p != '$' || !p[1]) { out[o++] = *p; continue; }
        char tok[20] = "";
        switch (*++p) {
        case 'N': ename_system(s_npc_seed | 1u, tok); break;
        case 'S': if (s_si) memcpy(tok, s_si->name, sizeof s_si->name); break;
        case 'T': if (s_si && s_station >= 0 && s_station < s_si->n_stations)
                      memcpy(tok, s_si->stations[s_station].name, 20);
                  break;
        case 'F': if (s_si) snprintf(tok, sizeof tok, "%s",
                      k_faction_names[system_faction(s_si->addr)]);
                  break;
        case 'G': snprintf(tok, sizeof tok, "%s",
                      k_goods[seeded_good()].name);
                  break;
        default:  tok[0] = *p; tok[1] = 0; break;
        }
        tok[19] = 0;
        for (const char *t = tok; *t && o < cap - 1; t++) out[o++] = *t;
    }
    out[o] = 0;
}

/* --- outcome interpreter -------------------------------------------------*/
static EvReceipt s_rcpt;
const EvReceipt *events_receipt(void) { return &s_rcpt; }
int32_t *events_save_pending(void) { return &s_pending_cr; }
int32_t events_pending_take(void) {
    int32_t p = s_pending_cr;
    s_pending_cr = 0;
    return p;
}

/* Snapshot -> diff: the receipt reports what REALLY changed (clamps,
 * confiscations and cargo-space limits included). */
typedef struct {
    int32_t cr; float fuel, hull; int legal;
    int8_t rep[N_FACTIONS]; uint8_t cargo[N_GOODS];
    int32_t pending;
} EvSnap;

static void ev_snap(EvSnap *s) {
    s->cr = g_player.credits;
    s->fuel = g_player.fuel;
    s->hull = g_ships[PLAYER].hull;
    s->legal = g_player.legal;
    memcpy(s->rep, g_rep, sizeof s->rep);
    memcpy(s->cargo, g_player.cargo, sizeof s->cargo);
    s->pending = s_pending_cr;
}

static void ev_diff(const EvSnap *a) {
    s_rcpt.cr = g_player.credits - a->cr;
    s_rcpt.later_cr = s_pending_cr - a->pending;
    s_rcpt.fuel = g_player.fuel - a->fuel;
    float hm = g_ships[PLAYER].hull_max;
    s_rcpt.hull_pct = hm > 0
        ? (int)((g_ships[PLAYER].hull - a->hull) * 100.0f / hm + 0.5f) : 0;
    s_rcpt.legal = (int)g_player.legal - a->legal;
    for (int f = 0; f < N_FACTIONS; f++)
        s_rcpt.rep[f] = (int8_t)(g_rep[f] - a->rep[f]);
    s_rcpt.n_goods = 0;
    for (int g = 0; g < N_GOODS && s_rcpt.n_goods < 3; g++)
        if (g_player.cargo[g] != a->cargo[g]) {
            s_rcpt.goods_id[s_rcpt.n_goods] = (uint8_t)g;
            s_rcpt.goods_d[s_rcpt.n_goods] =
                (int8_t)(g_player.cargo[g] - a->cargo[g]);
            s_rcpt.n_goods++;
        }
}

int events_run_choice(const Event *ev, int choice) {
    if (!events_choice_enabled(ev, choice)) return -1;
    const Choice *c = &ev->choices[choice];
    if (c->cost > 0) g_player.credits -= c->cost;

    memset(&s_rcpt, 0, sizeof s_rcpt);
    s_rcpt.lore_id = -1;
    s_rcpt.item_type = -1;
    EvSnap snap;
    ev_snap(&snap);
    snap.cr += c->cost;            /* receipt includes the price paid */

    /* Branch rng: seeded at pick time + choice — same visit, same fate. */
    uint32_t rng = mix32(s_outcome_seed ^ (uint32_t)(choice * 0x9E3779B9u));
    int result = -1;
    const Op *ops = c->ops;
    for (int pc = 0; ops && pc < 32; pc++) {
        const Op *op = &ops[pc];
        switch (op->op) {
        case OP_END: ev_diff(&snap); return result;
        case OP_CR: {
            g_player.credits += (int32_t)op->a * 25;
            if (g_player.credits < 0) g_player.credits = 0;
            break;
        }
        case OP_CARGO: {
            int g = (op->a < 0) ? seeded_good() : op->a;
            if (g < 0 || g >= N_GOODS) break;
            int n = g_player.cargo[g] + op->b;
            if (n < 0) n = 0;
            int over = (player_cargo_total() - g_player.cargo[g] + n) -
                       player_cargo_cap();
            if (over > 0) n -= over;
            if (n < 0) n = 0;
            g_player.cargo[g] = (uint8_t)n;
            break;
        }
        case OP_REP: {
            int f = (op->a < 0) ? (int)system_faction(s_si->addr) : op->a;
            int r = g_rep[f] + op->b;
            if (r < -100) r = -100;
            if (r > 100) r = 100;
            g_rep[f] = (int8_t)r;
            break;
        }
        case OP_FUEL: {
            float fu = g_player.fuel + (float)op->a * 0.1f;
            if (fu < 0) fu = 0;
            if (fu > g_player.fuel_max) fu = g_player.fuel_max;
            g_player.fuel = fu;
            break;
        }
        case OP_DMG: {
            Ship *p = &g_ships[PLAYER];
            p->hull -= p->hull_max * (float)op->a * 0.01f;
            if (p->hull < 1.0f) p->hull = 1.0f;   /* events never kill */
            if (p->hull > p->hull_max) p->hull = p->hull_max;
            break;
        }
        case OP_AMBUSH:
            elite_game_event_ambush(op->a, op->b);
            s_rcpt.ambush_n = (uint8_t)(s_rcpt.ambush_n + op->a);
            break;
        case OP_LORE:
            if (!bit(op->a & 127)) s_rcpt.lore_id = op->a & 127;
            bit_set(op->a & 127);
            break;
        case OP_FLAG:
            bit_set(128 + (op->a & 31));
            break;
        case OP_BRANCH:
            rng = mix32(rng);
            if ((int)(rng % 100u) < op->a) pc = op->b - 1;
            break;
        case OP_RESULT:
            result = op->a;
            break;
        case OP_LEGAL: {
            int l = (int)g_player.legal + op->a;
            if (l < 0) l = 0;
            if (l > 2) l = 2;
            g_player.legal = (uint8_t)l;
            break;
        }
        case OP_CONTRA:
            for (int g = 0; g < N_GOODS; g++)
                if (k_goods[g].flags & GOOD_ILLEGAL) g_player.cargo[g] = 0;
            break;
        case OP_ITEM: {
            /* Salvaged hardware, rolled like a combat drop. Rack full
             * pays scrap value instead — never a dead reward. */
            int slot = player_free_rack_slot();
            if (slot < 0) { g_player.credits += 100; break; }
            WeaponInst w;
            memset(&w, 0, sizeof w);
            rng = mix32(rng);
            w.type = (uint8_t)(rng % WPN_COUNT);
            rng = mix32(rng);
            int q = (int)(rng % 100u);
            int rolled = (q < 50) ? Q_SALVAGED
                       : (q < 80) ? Q_STANDARD
                       : (q < 93) ? Q_REINFORCED
                       : (q < 99) ? Q_MILITARY : Q_PROTOTYPE;
            if (rolled < op->a) rolled = op->a;
            w.quality = (uint8_t)rolled;
            rng = mix32(rng);
            w.integrity = (uint8_t)(20 + rng % 55);
            w.ammo_flag = 1;
            w.ammo_lo = (uint8_t)(k_weapons[w.type].ammo_max * 2 / 5);
            w.in_use = 1;
            g_player.salvage[slot] = w;
            s_rcpt.item_type = w.type;
            break;
        }
        case OP_LATER:
            s_pending_cr += (int32_t)op->a * 25;
            break;
        case OP_MISSION:
            if (mission_grant_warzone(s_si)) s_rcpt.mission = 1;
            break;
        case OP_TIER: {
            /* Bump a fitted defensive item up by op->b tiers (clamp 1..3).
             * The aftermath text spells out the result. */
            WeaponInst *eq = (op->a == 0) ? &g_player.shield_eq
                                          : &g_player.armor_eq;
            if (eq->in_use) {
                int t = (int)eq->tier + op->b;
                if (t < 1) t = 1;
                if (t > 3) t = 3;
                eq->tier = (uint8_t)t;
            }
            break;
        }
        case OP_AFFIX:
            /* Re-tune the primary fitted weapon (first occupied mount). */
            for (int i = 0; i < HULL_SLOTS; i++)
                if (g_player.mounts[i].in_use) {
                    g_player.mounts[i].affix = (uint8_t)op->a;
                    break;
                }
            break;
        default:
            ev_diff(&snap);
            return result;
        }
    }
    ev_diff(&snap);
    return result;
}
