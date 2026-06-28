/*
 * ThumbyElite — ship entity pool.
 */
#include "elite_entity.h"
#include "elite_ships.h"
#include <string.h>

Ship g_ships[MAX_SHIPS];

void ships_init(void) {
    memset(g_ships, 0, sizeof g_ships);
    for (int i = 0; i < MAX_SHIPS; i++) g_ships[i].target = -1;
}

static void ship_defaults(Ship *s, const Mesh *mesh) {
    s->mesh = mesh;
    s->basis = m3_identity();
    s->vel = v3(0, 0, 0);
    s->throttle = 0;
    s->assist = true;
    s->boost_t = 0;
    /* Stats roughly by hull size (placeholder until outfitting, Phase 7):
     * bigger bounding radius -> slower, tougher. */
    float k = mesh->bound_r / 5.0f;        /* fighter ~1.3, freighter ~1.4 */
    s->max_speed = 110.0f / k;
    s->accel = 55.0f / k;
    s->turn_rate = 2.2f / k;
    s->hull_max = 45.0f * k;
    s->shield_max = 30.0f * k;
    s->heat = 0;
    s->fire_cool = 0;
    s->ai_state = AI_NONE;
    s->ai_timer = 0;
    s->target = -1;
}

int ship_spawn(const Mesh *mesh, Vec3 pos, uint8_t team) {
    for (int i = 1; i < MAX_SHIPS; i++) {
        if (g_ships[i].alive) continue;
        Ship *s = &g_ships[i];
        memset(s, 0, sizeof *s);
        ship_defaults(s, mesh);
        s->alive = true;
        s->pos = pos;
        s->team = team;
        s->hull = s->hull_max;
        s->shield = s->shield_max;
        return i;
    }
    return -1;
}

void ships_despawn_npcs(void) {
    for (int i = 1; i < MAX_SHIPS; i++) g_ships[i].alive = false;
}

const char *k_tier_names[5] = {
    "VOIDRAT", "ROGUE", "MARAUDER", "REAVER", "ELITE",
};

void ship_fit_weapon(int idx, int mount, WeaponType w) {
    Ship *s = &g_ships[idx];
    if (mount >= MAX_HARDPOINTS) return;
    s->weapons[mount] = (uint8_t)w;
    s->ammo[mount] = k_weapons[w].ammo_max ? (int16_t)k_weapons[w].ammo_max
                                           : -1;
    if (mount >= s->n_weapons) s->n_weapons = (uint8_t)(mount + 1);
}

/* WEAPON VARIETY (user: variety is key — all weapons at all tiers,
 * weighted by rank). Each combat weapon has a grade 0 (light) .. 4
 * (heavy); a pilot rolls a grade from a tier-biased distribution then
 * picks a random weapon of that grade — so a HARMLESS pirate USUALLY
 * has a peashooter but might surprise you, and an ELITE usually packs
 * heavy iron but occasionally slums it. */
static const uint8_t k_grade0[] = { WPN_PULSE_S, WPN_AUTOCANNON };
static const uint8_t k_grade1[] = { WPN_PULSE_M, WPN_MISSILE,
                                    WPN_FLAK };   /* BLASTER pulled: it
                                    bypasses the aim-spread system, so
                                    weak pilots hit ~65%% with it (user) */
static const uint8_t k_grade2[] = { WPN_PULSE_L, WPN_BEAM, WPN_ION,
                                    WPN_HOMING };
static const uint8_t k_grade3[] = { WPN_PHOTON, WPN_PLASMA, WPN_GAUSS };
static const uint8_t k_grade4[] = { WPN_RAILGUN, WPN_LANCE };
static const struct { const uint8_t *w; int n; } k_grades[5] = {
    { k_grade0, 2 }, { k_grade1, 3 }, { k_grade2, 4 },
    { k_grade3, 3 }, { k_grade4, 2 },
};
/* per-tier grade probabilities x100 (rows sum to 100). */
static const uint8_t k_gradeprob[5][5] = {
    /* g0  g1  g2  g3  g4 */
    {  70, 25,  5,  0,  0 },   /* T0 HARMLESS */
    {  48, 35, 14,  3,  0 },   /* T1 NOVICE   */
    {  24, 34, 28, 11,  3 },   /* T2 CAPABLE  */
    {  10, 24, 34, 22, 10 },   /* T3 VETERAN  */
    {   3, 12, 30, 35, 20 },   /* T4 DEADLY   */
};
static uint32_t s_wrng = 0x5151u;
static WeaponType roll_weapon(int tier, uint32_t salt) {
    s_wrng ^= salt * 2654435761u;
    s_wrng ^= s_wrng << 13; s_wrng ^= s_wrng >> 17; s_wrng ^= s_wrng << 5;
    int r = (int)(s_wrng % 100u);
    const uint8_t *gp = k_gradeprob[tier > 4 ? 4 : tier];
    int g = 0, acc = 0;
    for (g = 0; g < 5; g++) { acc += gp[g]; if (r < acc) break; }
    if (g > 4) g = 4;
    s_wrng ^= s_wrng << 13; s_wrng ^= s_wrng >> 17; s_wrng ^= s_wrng << 5;
    return (WeaponType)k_grades[g].w[s_wrng % (uint32_t)k_grades[g].n];
}

void ship_set_tier(int idx, int tier, int hull_class) {
    Ship *s = &g_ships[idx];
    if (tier < 0) tier = 0;
    if (tier > 4) tier = 4;
    if (hull_class < 0) hull_class = 0;
    if (hull_class >= N_HULLS) hull_class = N_HULLS - 1;
    s->tier = (uint8_t)tier;
    /* Class template stats, scaled down for NPCs and up with tier. */
    const HullDef *h = &k_hulls[hull_class];
    float k = 1.0f + 0.13f * (float)tier;
    /* The hull defines its OWN top speed -- tier must never raise it above
     * what the ship is designed for. A pilot's SKILL governs how much of
     * that ceiling they actually use (AI k_fight_speed[tier]), not the
     * ceiling itself. */
    s->max_speed = h->max_speed;
    s->accel = h->accel;
    /* Low tiers fly wide, lazy loops — easy to out-turn. */
    s->turn_rate = h->turn_rate * (0.42f + 0.17f * (float)tier);
    s->hull_max = h->hull_base * 0.55f * k;
    s->hull = s->hull_max;
    s->shield_max = h->shield_base * 0.55f * k;
    s->armor_tier = 0;       /* pirates: no fitted defensive kit */
    s->shield_tier = 0;
    /* High-tier pilots fly variant gear: BULWARK walls or REGEN
     * skirmish shields — they FIGHT differently. */
    s->shield_var = SHV_STANDARD;
    s->shield_regen = 0;
    s->shield_delay = 0;
    if (tier >= 3) {
        if (idx & 1) {
            s->shield_var = SHV_BULWARK;
            s->shield_max *= 1.5f;
            s->shield_regen = 1.2f;
        } else {
            s->shield_var = SHV_REGEN;
            s->shield_max *= 0.7f;
            s->shield_regen = 7.2f;
            s->shield_delay = 2.0f;
        }
    }
    if (tier >= 4 && (idx % 3) == 0) s->shield_var = SHV_PHASE;
    s->armor_var = (tier >= 3 && (idx & 1)) ? ARV_REACTIVE : ARV_STANDARD;
    /* VETERAN+ carries chaff (user design: missile countermeasures) */
    s->chaff_n = (uint8_t)((tier >= 3) ? 1 + (idx & 1) : 0);
    s->cls = (uint8_t)hull_class;
    /* Hauler-class pirates (big bound radius) carry a belly turret. */
    s->turret_type = (s->mesh->bound_r > 9.0f && tier >= 2)
                         ? (uint8_t)(WPN_PULSE_S + 1) : 0;
    s->turret_cool = 0;
    s->shield = s->shield_max;
    /* Loadout: weighted variety (user). Gun count rises with rank;
     * each mount rolls independently for genuine mixed arsenals. */
    s->n_weapons = 0;
    s->active_w = 0;
    int nguns = (tier >= 3) ? 2 : (tier == 2 && (idx & 1)) ? 2 : 1;
    for (int m = 0; m < nguns; m++) {
        WeaponType w = roll_weapon(tier,
                           (uint32_t)(idx * 977 + m * 31 + tier * 7));
        /* Mount 0 must be a real PRIMARY — flak/mine/tractor/mining are
         * supplements (a flak burst on a run), never a pilot's only
         * gun. Re-roll the primary slot until it's a forward weapon. */
        if (m == 0) {
            int tries = 0;
            while ((w == WPN_FLAK || w == WPN_MINE || w == WPN_TRACTOR ||
                    w == WPN_MINING) && tries < 8) {
                w = roll_weapon(tier, (uint32_t)(idx * 977 + 53 +
                                                 tries * 101 + tier * 7));
                tries++;
            }
            if (w == WPN_FLAK || w == WPN_MINE || w == WPN_TRACTOR ||
                w == WPN_MINING)
                w = (tier >= 4) ? WPN_PULSE_L
                  : (tier >= 2) ? WPN_PULSE_M : WPN_PULSE_S;
        }
        ship_fit_weapon(idx, m, w);
    }
}

/* CIVILIANS fly rolled armour + shields (user): tankier than a bare
 * hull and the kit is real salvage on the wreck. Call AFTER
 * ship_set_tier (it reads s->cls/tier and overrides the HP). */
void ship_fit_defence(int idx, int tier) {
    Ship *s = &g_ships[idx];
    if (tier < 0) tier = 0; if (tier > 4) tier = 4;
    const HullDef *h = &k_hulls[s->cls];
    float k = 1.0f + 0.13f * (float)tier;
    static const uint8_t k_eqtier[5][4] = {
        { 0, 0, 1, 1 }, { 0, 1, 1, 2 }, { 1, 1, 2, 2 },
        { 1, 2, 2, 3 }, { 2, 2, 3, 3 },
    };
    uint32_t er = (uint32_t)(idx * 2654435761u) ^ (uint32_t)(tier * 40503u);
    er ^= er >> 13;
    int arm_t = k_eqtier[tier][er & 3];
    int shd_t = k_eqtier[tier][(er >> 4) & 3];
    s->armor_tier = (uint8_t)arm_t;
    s->shield_tier = (uint8_t)shd_t;
    float arm_f = arm_t ? k_tier_mult[arm_t] * 0.85f : 0.65f;
    float shd_f = shd_t ? k_tier_mult[shd_t] * 0.85f : 0.65f;
    s->hull_max = h->hull_base * k * arm_f;
    s->shield_max = h->shield_base * k * shd_f;
    s->hull = s->hull_max;
    s->shield = s->shield_max;
}

int ships_alive_hostile(void) {
    int n = 0;
    for (int i = 1; i < MAX_SHIPS; i++)
        if (g_ships[i].alive && g_ships[i].team == TEAM_HOSTILE) n++;
    return n;
}
