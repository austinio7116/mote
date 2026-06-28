/*
 * ThumbyElite — persistent player state.
 */
#include "elite_player.h"
#include <math.h>
#include "system_sim.h"
#include "elite_entity.h"
#include <string.h>

PlayerState g_player;

static const float k_qual_mult[5] = { 0.80f, 1.00f, 1.12f, 1.22f, 1.35f };
static const float k_qual_price[5] = { 0.45f, 1.00f, 1.60f, 2.50f, 4.00f };

/* Base shop prices per weapon type (STANDARD quality). */
static const int16_t k_wpn_base[WPN_COUNT] = {
    [WPN_PULSE_S] = 600,  [WPN_PULSE_M] = 1800, [WPN_PULSE_L] = 4800,
    [WPN_BEAM] = 2600,    [WPN_PHOTON] = 3400,  [WPN_GAUSS] = 5200,
    [WPN_AUTOCANNON] = 900, [WPN_MISSILE] = 1200, [WPN_HOMING] = 2800,
    [WPN_FLAK] = 1600, [WPN_RAILGUN] = 7800, [WPN_ION] = 3000,
    [WPN_MINE] = 1400, [WPN_TRACTOR] = 800, [WPN_MINING] = 700,
    [WPN_PLASMA] = 2400, [WPN_LANCE] = 6400, [WPN_BLASTER] = 3400,
};

void player_init(void) {
    memset(&g_player, 0, sizeof g_player);
    g_player.credits = 1000;
    g_player.difficulty = 2;            /* EASY by default (user) */
    g_player.hull_id = 0;               /* starter class — everyone starts low */
    g_player.hull_seed = 0xBEA7E12u;    /* replaced by test mode / shipyard */
    g_player.fuel_max = 30.0f;
    g_player.fuel = g_player.fuel_max;
    /* One battered salvaged pulse laser + bottom-shelf protection. */
    g_player.mounts[0] = (WeaponInst){ .type = WPN_PULSE_S,
        .quality = Q_SALVAGED, .integrity = 70, .in_use = 1 };
    for (int i = 0; i < HULL_SLOTS; i++) g_player.ammo[i] = -1;
    g_player.invert_y = 1;              /* default: push forward to dive */
    g_player.shield_eq = (WeaponInst){ .type = EQ_SHIELD,
        .quality = Q_SALVAGED, .integrity = 80, .in_use = 1, .tier = 1 };
    g_player.armor_eq = (WeaponInst){ .type = EQ_ARMOR,
        .quality = Q_SALVAGED, .integrity = 75, .in_use = 1, .tier = 1 };
}

int player_cargo_total(void) {
    /* Goods only — the component rack is its own pool now (user
     * decision: racked salvage no longer eats trade capacity). */
    int n = 0;
    for (int i = 0; i < N_GOODS; i++) n += g_player.cargo[i];
    return n;
}

int player_cargo_cap(void) { return player_roll()->cargo; }

int player_rack_cap(void) { return k_hulls[g_player.hull_id].rack; }

int player_free_rack_slot(void) {
    int cap = player_rack_cap();
    for (int i = 0; i < cap && i < MAX_SALVAGE; i++)
        if (!g_player.salvage[i].in_use) return i;
    return -1;
}

float quality_dmg_mult(int q) { return k_qual_mult[q > 4 ? 4 : q]; }

int weapon_price(int type, int q) {
    if (type >= WPN_COUNT)
        return equip_price(type, 1, q);
    return (int)(k_wpn_base[type] * k_qual_price[q > 4 ? 4 : q]);
}

int equip_price(int type, int tier, int q) {
    static const float k_tier_price[4] = { 1.0f, 1.0f, 2.0f, 3.6f };
    int base = k_equip[type - WPN_COUNT].base_price;
    return (int)(base * k_tier_price[tier > 3 ? 3 : tier] *
                 k_qual_price[q > 4 ? 4 : q]);
}

float equip_mult(const WeaponInst *e) {
    if (!e->in_use) return 0.55f;        /* flying bare: weak baseline */
    /* Defense gear climbs steeper than weapons (+12% cap for 1800cr
     * read as worthless — user report): HIGH-TECH +40%, PROTO +65%. */
    static const float k_def_qual[5] = { 0.80f, 1.00f, 1.35f,
                                         1.75f, 2.25f };
    return k_def_qual[e->quality > 4 ? 4 : e->quality] *
           (0.6f + 0.4f * (float)e->integrity * 0.01f);
}

float mount_dmg_mult(const WeaponInst *w) {
    /* Integrity below 100 bleeds output: 60% output at 0 integrity.
     * Affix damage modifier folds in here so every consumer (fire,
     * detail sheets, compare deltas) sees effective numbers. */
    return quality_dmg_mult(w->quality) *
           (0.6f + 0.4f * (float)w->integrity * 0.01f) *
           k_affixes[w->affix < AFX_COUNT ? w->affix : 0].dmg;
}

/* Instance market value: base x quality x affix premium. */
int instance_price(const WeaponInst *w) {
    int base = (w->type >= WPN_COUNT)
                   ? equip_price(w->type, w->tier, w->quality)
                   : weapon_price(w->type, w->quality);
    return (int)(base * k_affixes[w->affix < AFX_COUNT ? w->affix : 0].price);
}

int skill_level(uint16_t xp) {
    /* Levels at 2,5,10,18,30,50,80,120,170 - simple quadratic-ish curve. */
    static const uint16_t th[9] = { 2, 5, 10, 18, 30, 50, 80, 120, 170 };
    int lvl = 0;
    for (int i = 0; i < 9; i++)
        if (xp >= th[i]) lvl = i + 1;
    return lvl;
}

float skill_heat_mult(void) {
    return 1.0f - 0.025f * (float)skill_level(g_player.xp_gunnery);
}
float skill_turn_mult(void) {
    return 1.0f + 0.02f * (float)skill_level(g_player.xp_piloting);
}
float skill_price_mult(void) {
    return 1.0f - 0.012f * (float)skill_level(g_player.xp_trading);
}
float skill_repair_mult(void) {
    return 1.0f - 0.05f * (float)skill_level(g_player.xp_tech);
}

static uint8_t s_mount_map[HULL_SLOTS];

const WeaponInst *player_mount_for_ship_slot(int slot) {
    if (slot < 0 || slot >= HULL_SLOTS) return 0;
    return &g_player.mounts[s_mount_map[slot]];
}

void player_apply_to_ship(void) {
    Ship *p = &g_ships[PLAYER];
    const HullDef *h = &k_hulls[g_player.hull_id];
    p->mesh = hull_mesh(g_player.hull_seed, g_player.hull_id);
    const HullRoll *rl = player_roll();
    p->max_speed = h->max_speed * rl->spd;
    p->accel = h->accel * rl->acc;
    p->turn_rate = h->turn_rate * rl->trn * skill_turn_mult();
    /* Protection from fitted equipment: tier x quality x wear x
     * variant character. */
    float sh_t = g_player.shield_eq.in_use
                     ? k_tier_mult[g_player.shield_eq.tier] : 1.0f;
    float ar_t = g_player.armor_eq.in_use
                     ? k_tier_mult[g_player.armor_eq.tier] : 1.0f;
    int shv = g_player.shield_eq.in_use ? g_player.shield_eq.affix : 0;
    int arv = g_player.armor_eq.in_use ? g_player.armor_eq.affix : 0;
    static const float k_shv_cap[4] = { 1.0f, 0.70f, 1.50f, 0.85f };
    static const float k_shv_rgn[4] = { 1.0f, 3.90f, 0.55f, 1.00f };
    static const float k_arv_hp[4]  = { 1.0f, 1.00f, 1.35f, 0.85f };
    p->hull_max = h->hull_base * rl->hull * ar_t *
                  equip_mult(&g_player.armor_eq) * k_arv_hp[arv & 3];
    p->shield_max = h->shield_base * rl->shd * sh_t *
                    equip_mult(&g_player.shield_eq) * k_shv_cap[shv & 3];
    /* Baseline regen cut (3.0 -> 1.4) so widened enemy aim can't fall
     * below it into an unkillable wall; the REGEN affix multiplier is
     * raised to keep regen shields a strong top-tier pick (~7.1/s). */
    p->shield_regen = 1.8f * k_shv_rgn[shv & 3];
    p->shield_delay = (shv == SHV_REGEN) ? 2.0f : 4.5f;
    p->shield_var = (uint8_t)shv;
    p->armor_var = (uint8_t)arv;
    p->turret_type = (k_hulls[g_player.hull_id].has_turret &&
                      g_player.turret_eq.in_use)
                         ? (uint8_t)(g_player.turret_eq.type + 1) : 0;
    p->turret_cool = 0;
    if (arv == ARV_COMPOSITE) {
        p->max_speed *= 1.08f;
        p->turn_rate *= 1.08f;
    }
    if (p->hull > p->hull_max) p->hull = p->hull_max;
    if (p->shield > p->shield_max) p->shield = p->shield_max;

    p->n_weapons = 0;
    p->active_w = 0;
    for (int i = 0; i < player_n_slots(); i++) {
        if (!g_player.mounts[i].in_use) continue;
        int m = p->n_weapons;
        s_mount_map[m] = (uint8_t)i;
        p->weapons[m] = g_player.mounts[i].type;
        const WeaponDef *w = &k_weapons[g_player.mounts[i].type];
        /* Ammo persists per session; refilled when docked (free, v1). */
        /* -1 = never tracked (fresh fit) -> full magazine. */
        p->ammo[m] = !w->ammo_max ? -1
                   : (g_player.ammo[i] < 0) ? (int16_t)w->ammo_max
                                            : g_player.ammo[i];
        p->n_weapons++;
    }
}

/* Per-round restock prices (credits). */
static int round_cost(int type) {
    switch (type) {
    case WPN_AUTOCANNON: return 1;
    case WPN_GAUSS: return 8;
    case WPN_MISSILE: return 35;
    case WPN_HOMING: return 40;
    case WPN_FLAK: return 3;
    case WPN_RAILGUN: return 22;
    case WPN_MINE: return 30;
    default: return 0;
    }
}

void player_sync_ammo(int ship_slot, int ammo) {
    for (int i = 0; i < HULL_SLOTS; i++)
        if (s_mount_map[i] == ship_slot && g_player.mounts[i].in_use) {
            g_player.ammo[i] = (int16_t)ammo;
            return;
        }
}

int player_rearm_cost(void) {
    int cost = 0;
    if (player_has_util(EQ_CHAFF))
        cost += (4 - g_player.chaff_charges) * 20;
    for (int i = 0; i < HULL_SLOTS; i++) {
        const WeaponInst *m = &g_player.mounts[i];
        if (!m->in_use) continue;
        int maxa = k_weapons[m->type].ammo_max;
        if (!maxa) continue;
        int have = g_player.ammo[i] < 0 ? maxa : g_player.ammo[i];
        cost += (maxa - have) * round_cost(m->type);
    }
    return cost;
}

void player_rearm(void) {
    if (player_has_util(EQ_CHAFF)) g_player.chaff_charges = 4;
    for (int i = 0; i < HULL_SLOTS; i++) {
        const WeaponInst *m = &g_player.mounts[i];
        if (!m->in_use) continue;
        int maxa = k_weapons[m->type].ammo_max;
        g_player.ammo[i] = maxa ? (int16_t)maxa : -1;
    }
}

/* Write the live magazine into the instance before it leaves the
 * mount (unfit / swap-out / sell). */
void player_stash_mount_ammo(int mount) {
    WeaponInst *m = &g_player.mounts[mount];
    if (!m->in_use || !k_weapons[m->type].ammo_max) return;
    int a = g_player.ammo[mount];
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    m->ammo_lo = (uint8_t)a;
    m->ammo_flag = 1;
}

/* Fit-time load: stored magazine if the instance carries one,
 * factory rules otherwise (sealed = full, battle salvage = 40%). */
void player_fit_restore_ammo(int mount) {
    WeaponInst *m = &g_player.mounts[mount];
    int maxa = m->in_use ? k_weapons[m->type].ammo_max : 0;
    if (!maxa) { g_player.ammo[mount] = -1; return; }
    if (m->ammo_flag) {
        int a = m->ammo_lo;
        if (a > maxa) a = maxa;
        g_player.ammo[mount] = (int16_t)a;
    } else {
        g_player.ammo[mount] =
            (int16_t)(m->integrity >= 100 ? maxa : maxa * 2 / 5);
    }
}

void player_load_mount_ammo(int mount, float fill01) {
    const WeaponInst *m = &g_player.mounts[mount];
    int maxa = m->in_use ? k_weapons[m->type].ammo_max : 0;
    g_player.ammo[mount] = maxa ? (int16_t)((float)maxa * fill01) : -1;
}

/* The player's hull roll (per-instance quirks from hull_seed). */
const HullRoll *player_roll(void) {
    static HullRoll r;
    static uint32_t for_seed = 0;
    static int for_hull = -1;
    if (for_seed != g_player.hull_seed || for_hull != g_player.hull_id) {
        hull_roll(g_player.hull_id, g_player.hull_seed, &r);
        for_seed = g_player.hull_seed;
        for_hull = g_player.hull_id;
    }
    return &r;
}

int player_n_slots(void) { return player_roll()->n_slots; }
int player_slot_size(int i) { return player_roll()->slot_size[i]; }

int player_util_slots(void) {
    return player_roll()->utils;
}

bool player_has_util(int eq_type) {
    int n = player_util_slots();
    for (int i = 0; i < n && i < 4; i++)
        if (g_player.util_eq[i].in_use &&
            g_player.util_eq[i].type == eq_type &&
            g_player.util_eq[i].integrity > 0)   /* critted = dead */
            return true;
    return false;
}
