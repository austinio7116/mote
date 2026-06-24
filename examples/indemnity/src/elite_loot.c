/*
 * ThumbyElite — wreck salvage.
 *
 * Kills drop tumbling canisters: commodities (into the cargo hold) or
 * damaged weapon components (into the salvage rack — repair, fit or
 * sell them at a station: the MechWarrior Mercenaries loop). Fly within
 * scoop range to collect.
 */
#include "elite_loot.h"
#include "elite_entity.h"
#include "elite_player.h"
#include "r3d_scene.h"
#include "r3d_fx.h"
#include "elite_types.h"
#include <math.h>
#include "econ.h"
#include "elite_audio.h"
#include "meshes_gen.h"
#include <stdio.h>

/* Drop beacons (bright point + light-mast) are hidden on the title so the
 * cargo cubes don't read as prominent markers behind the wordmark. */
static bool s_loot_beacons = true;
void loot_set_beacons(bool on) { s_loot_beacons = on; }

#define MAX_CANS   6
#define SCOOP_RANGE 22.0f
#define CAN_LIFE   45.0f

typedef struct {
    bool  alive;
    Vec3  pos, vel;
    float spin, life;
    uint8_t is_component;
    WeaponInst comp;       /* component drops */
    uint8_t good, count;   /* commodity drops */
} Canister;

static Canister s_cans[MAX_CANS];
static char s_toast[28];

static uint32_t s_rng = 0x10075EEDu;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

void loot_seed(uint32_t seed) {
    s_rng = seed * 2654435761u;
    s_rng ^= s_rng >> 15;
    if (!s_rng) s_rng = 0x10075EEDu;
}

void loot_init(void) {
    for (int i = 0; i < MAX_CANS; i++) s_cans[i].alive = false;
}

void loot_on_kill(Vec3 pos, Vec3 vel, int tier,
                  const Ship *victim) {
    /* Mix kill-site bits into the stream: without this the sequence is
     * identical every boot and the first salvage is ALWAYS the same
     * weapon (user-reported: eternal PULSE-M). */
    union { float f; uint32_t u; } px = { pos.x }, pz = { pos.z };
    s_rng ^= px.u * 0x9E3779B9u ^ (pz.u >> 3);
    if (!s_rng) s_rng = 1;
    if ((rnd() % 100u) >= 72) return;          /* 72% drop chance */
    for (int i = 0; i < MAX_CANS; i++) {
        if (s_cans[i].alive) continue;
        Canister *c = &s_cans[i];
        c->alive = true;
        c->pos = pos;
        c->vel = v3_add(v3_scale(vel, 0.3f),
                        v3((float)(rnd() % 11) - 5.0f,
                           (float)(rnd() % 11) - 5.0f,
                           (float)(rnd() % 11) - 5.0f));
        c->spin = 0.5f + (float)(rnd() % 100) * 0.015f;
        c->life = CAN_LIFE;
        /* Component odds rise with the victim's tier (better pilots fly
         * better-kept gear). */
        c->is_component = (rnd() % 100u) < (uint32_t)(30 + tier * 10);
        if (c->is_component) {
            /* Drops are the ship's ACTUAL kit (user): 1-in-4 its real
             * ARMOUR/SHIELD (rolled tier+variant), else a gun from its
             * real loadout. Fall back to random only for ambient debris
             * / tests where we have no ship. */
            int has_def = victim &&
                          (victim->shield_tier || victim->armor_tier);
            if ((rnd() % 4u) == 0 && has_def) {
                int armor = victim->armor_tier &&
                            ((rnd() & 1) || !victim->shield_tier);
                c->comp.type = (uint8_t)(WPN_COUNT + (armor ? 1 : 0));
                c->comp.tier = armor ? victim->armor_tier
                                     : victim->shield_tier;
                c->comp.affix = armor ? victim->armor_var
                                      : victim->shield_var;
            } else {
                c->comp.type = (victim && victim->n_weapons > 0)
                    ? victim->weapons[rnd() % (uint32_t)victim->n_weapons]
                    : (uint8_t)(rnd() % WPN_COUNT);
                c->comp.tier = 0;
            }
            int q = (int)(rnd() % 100u);
            int rolled = (q < 50) ? Q_SALVAGED
                       : (q < 80) ? Q_STANDARD
                       : (q < 93) ? Q_REINFORCED
                       : (q < 99) ? Q_MILITARY : Q_PROTOTYPE;
            /* Quality FLOOR rises with the victim's rank -- the drop is
             * pulled from the kit they were flying (user). */
            static const uint8_t q_floor[5] = { Q_SALVAGED, Q_SALVAGED,
                Q_STANDARD, Q_REINFORCED, Q_MILITARY };
            int fl = q_floor[tier > 4 ? 4 : tier];
            c->comp.quality = (uint8_t)(rolled < fl ? fl : rolled);
            /* Affix roll (weapons only): ~25%, tier-sweetened; TUNED
             * only ever appears on PROTOTYPE drops. */
            c->comp.affix = AFX_NONE;
            if (c->comp.type < WPN_COUNT &&
                (int)(rnd() % 100u) < 18 + tier * 4) {
                int a = (int)(rnd() % 100u);
                c->comp.affix = (a < 35) ? AFX_OVERCLOCKED
                              : (a < 60) ? AFX_RAPID
                              : (a < 80) ? AFX_CALIBRATED : AFX_VENTED;
                if (c->comp.quality == Q_PROTOTYPE &&
                    (rnd() % 3u) == 0)
                    c->comp.affix = AFX_TUNED;
            }
            c->comp.integrity = (uint8_t)(20 + rnd() % 55);
            /* battle salvage arrives part-loaded, never sealed */
            c->comp.ammo_flag = 1;
            c->comp.ammo_lo = (uint8_t)(
                (c->comp.type < WPN_COUNT
                     ? k_weapons[c->comp.type].ammo_max : 0) * 2 / 5);
            c->comp.in_use = 1;
        } else {
            c->good = (uint8_t)(rnd() % 16u);   /* legal goods only */
            c->count = (uint8_t)(1 + rnd() % 3u);
        }
        return;
    }
}

const char *loot_tick(float dt) {
    const char *toast = NULL;
    Ship *p = &g_ships[PLAYER];
    for (int i = 0; i < MAX_CANS; i++) {
        Canister *c = &s_cans[i];
        if (!c->alive) continue;
        /* No expiry (user pref): loot drifts until scooped or you
         * leave the area. life stays for the pulse phase only. */
        c->pos = v3_add(c->pos, v3_scale(c->vel, dt));

        if (!p->alive) continue;
        if (v3_len2(v3_sub(c->pos, p->pos)) > SCOOP_RANGE * SCOOP_RANGE)
            continue;

        if (c->is_component) {
            /* Into the salvage rack (its own pool — components no
             * longer take trade-cargo slots). */
            int slot = player_free_rack_slot();
            if (slot < 0) {
                snprintf(s_toast, sizeof s_toast, "RACK FULL");
                toast = s_toast;
                continue;
            }
            g_player.salvage[slot] = c->comp;
            g_player.xp_tech += 2;
            snprintf(s_toast, sizeof s_toast, "SALVAGED %s",
                     item_name(c->comp.type));
        } else {
            int room = player_cargo_cap() - player_cargo_total();
            if (room <= 0) {
                snprintf(s_toast, sizeof s_toast, "HOLD FULL");
                toast = s_toast;
                continue;
            }
            int take = c->count < room ? c->count : room;
            g_player.cargo[c->good] += (uint8_t)take;
            snprintf(s_toast, sizeof s_toast, "+%d %s", take,
                     k_goods[c->good].name);
        }
        toast = s_toast;
        c->alive = false;
        sfx_scoop();
        fx_spawn_spark(c->pos, p->vel);
    }
    return toast;
}

void loot_render(Vec3 cam_pos) {
    extern float elite_game_time(void);
    float t = elite_game_time();
    for (int i = 0; i < MAX_CANS; i++) {
        const Canister *c = &s_cans[i];
        if (!c->alive) continue;
        R3DObject obj;
        obj.mesh = &mesh_canister;
        obj.basis = m3_identity();
        m3_rotate_local(&obj.basis, 1, t * c->spin);
        m3_rotate_local(&obj.basis, 0, t * c->spin * 0.7f);
        obj.pos = v3_sub(c->pos, cam_pos);
        r3d_scene_add_object(&obj);

        /* Pulsing beacon so drops read at combat ranges: bright core
         * point + a short light-mast above, gold for cargo, cyan for
         * components (matches the scanner blips). */
        float pulse = 0.5f + 0.5f * sinf(t * 6.0f + (float)i * 1.7f);
        uint16_t bc = c->is_component
                          ? RGB565C(120, 230, 255)
                          : RGB565C(255, 210, 70);
        if (!s_loot_beacons) continue;     /* title: bare cubes, no markers */
        float sx, sy;
        uint16_t d;
        Vec3 rel = v3_sub(c->pos, cam_pos);
        if (r3d_scene_project(rel, &sx, &sy, &d)) {
            r3d_scene_add_point(sx, sy, d, bc, pulse > 0.5f ? 2 : 1);
            float mx, my;
            uint16_t md;
            Vec3 mast = v3_add(rel, v3(0, 6.0f + 3.0f * pulse, 0));
            if (r3d_scene_project(mast, &mx, &my, &md))
                r3d_scene_add_line(sx, sy, d, mx, my, md, bc);
        }
    }
}

/* Nearest live canister to a point (target lock), -1 if none. */
int loot_nearest(Vec3 from, Vec3 *out_pos) {
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < MAX_CANS; i++) {
        if (!s_cans[i].alive) continue;
        float d = v3_len2(v3_sub(s_cans[i].pos, from));
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && out_pos) *out_pos = s_cans[best].pos;
    return best;
}

void loot_spawn_ore(Vec3 pos, Vec3 vel) {
    for (int i = 0; i < MAX_CANS; i++) {
        Canister *c = &s_cans[i];
        if (c->alive) continue;
        c->alive = true;
        c->is_component = 0;
        int roll = (int)(rnd() % 100u);
        c->good = (roll < 56) ? 12          /* MINERALS */
                : (roll < 94) ? 11          /* METALS */
                              : 15;         /* RARE GEMS — the jackpot */
        c->count = (uint8_t)(1 + (rnd() & 1));
        c->pos = pos;
        c->vel = v3_add(vel, v3(((int)(rnd() % 7) - 3) * 1.0f,
                                ((int)(rnd() % 5) - 2) * 1.0f,
                                ((int)(rnd() % 7) - 3) * 1.0f));
        c->spin = 0.8f;
        c->life = 1.0f;
        return;
    }
}

void loot_spawn_good(Vec3 pos, Vec3 vel, int good, int count) {
    for (int i = 0; i < MAX_CANS; i++) {
        Canister *c = &s_cans[i];
        if (c->alive) continue;
        c->alive = true;
        c->is_component = 0;
        c->good = (uint8_t)good;
        c->count = (uint8_t)count;
        c->pos = pos;
        c->vel = v3_add(vel, v3(((int)(rnd() % 7) - 3) * 1.2f,
                                ((int)(rnd() % 5) - 2) * 1.2f,
                                ((int)(rnd() % 7) - 3) * 1.2f));
        c->spin = 0.8f;
        c->life = 1.0f;
        return;
    }
}

void loot_tractor_pull(Vec3 to, float range, float speed) {
    for (int i = 0; i < MAX_CANS; i++) {
        Canister *c = &s_cans[i];
        if (!c->alive) continue;
        Vec3 d = v3_sub(to, c->pos);
        float dist = v3_len(d);
        if (dist > range || dist < 4.0f) continue;
        c->vel = v3_scale(d, speed / dist);
    }
}

int loot_dbg_comp_type(int slot) {
    if (slot < 0 || slot >= MAX_CANS || !s_cans[slot].alive ||
        !s_cans[slot].is_component) return -1;
    return s_cans[slot].comp.type;
}

int loot_positions(Vec3 *out, int *is_component, int max) {
    int n = 0;
    for (int i = 0; i < MAX_CANS && n < max; i++) {
        if (!s_cans[i].alive) continue;
        out[n] = s_cans[i].pos;
        is_component[n] = s_cans[i].is_component;
        n++;
    }
    return n;
}
