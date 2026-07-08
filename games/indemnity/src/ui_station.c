/*
 * ThumbyElite — docked station services.
 *
 * HOME menu -> MARKET (scrollable commodity table, A buy / B sell with
 * hold-autorepeat) / REFUEL / LAUNCH. Stubs list the Phase 7/8 services
 * so the player can see what's coming.
 */
#include "ui_station.h"
#include "elite_ui.h"       /* readable Audiowide menu text + scrollable lists */
#include "elite_types.h"
#include "elite_player.h"
#include "elite_game.h"    /* elite_game_police_stand_down (was implicit) */
#include "elite_combat.h"  /* player_turret_gunner_tier (was implicit) */
#include "system_sim.h"
#include "econ.h"
#include "elite_ships.h"
#include "ui_status.h"
#include "ui_icons.h"
#include "elite_audio.h"
#include "ui_detail.h"
#include "elite_entity.h"
#include "elite_platform.h"
#include "mission.h"
#include "enames.h"
#include "elite_weapons.h"
#include "craft_font.h"
#include "events.h"
#include "elite_engine.h"   /* g_em — engine jump table (blit) */
#include "goods.h"          /* goods_img — commodity icon spritesheet (assets/goods.png) */
#include <stdio.h>
#include <string.h>

#define COL_BG     RGB565C(  6,  10,  20)
#define COL_HDR    RGB565C(200, 210, 225)
#define COL_GRID   RGB565C( 28,  40,  58)
#define COL_TXT    RGB565C(120, 255, 120)
#define COL_DIM    RGB565C(110, 116, 135)
#define COL_CUR    RGB565C(120, 255, 120)
#define COL_CRED   RGB565C(255, 200,  60)
#define COL_WARN   RGB565C(255, 120,  70)
#define COL_ILL    RGB565C(220, 100, 200)

typedef enum {
    SCR_HOME = 0, SCR_MARKET, SCR_SHIPYARD, SCR_OUTFIT, SCR_STATUS,
    SCR_MISSIONS, SCR_BAR, SCR_CODEX
} Screen;

static Screen s_screen;
static int s_station;
static int s_cursor;
static int s_home_scroll;   /* station-hub menu scroll offset (readable font -> scrolling) */
static int s_scroll;
static int s_bought[N_GOODS];     /* session purchases (depletes stock) */
static CraftRawButtons s_prev;
static float s_hold_a, s_hold_b, s_repeat;
static char s_toast[24];
static float s_toast_t;
static int s_detail;       /* 0 = list, 1 = detail sheet open */
static uint8_t s_kit_view;  /* shipyard: showing a ship's included kit */

#define HOME_ITEMS 11
static const char *k_home[HOME_ITEMS] = {
    "MARKET", "SHIPYARD", "OUTFITTING", "MISSIONS", "BAR", "STATUS",
    "DATABASE", "REFUEL", "SERVICE", "PAY FINE", "LAUNCH",
};
static Mission s_offers[MISSION_OFFERS];

/* Bar encounter: rolled once per dock visit, on first BAR entry. */
static const Event *s_bar_ev;
static bool s_bar_rolled;

/* Shipyard stock: each dockyard rolls its own 5 ships. */
#define YARD_OFFERS 5
typedef struct {
    uint8_t  cls;
    uint8_t  bargain;       /* a special-offer discount is on this hull */
    uint32_t seed;
    int32_t  price;         /* economy + quality adjusted (incl. bargain) */
    char     name[16];
} YardOffer;
static YardOffer s_yard[YARD_OFFERS];

/* Ships price like the armoury: the station's economy times the hull's OWN
 * rolled quality (stats / slot config / utility bays / hold), with the odd
 * bargain to be found. */
static float econ_ship_mult(int econ) {
    static const float k_m[8] = {
        /* AGRI */ 1.12f, /* INDUST */ 0.92f, /* HITECH */ 0.90f,
        /* EXTRACT */ 1.06f, /* REFINE */ 0.98f, /* TOURISM */ 1.14f,
        /* MILITARY */ 0.88f, /* SERVICE */ 1.02f,
    };
    return k_m[econ & 7];
}
static float hull_quality_mult(int cls, const HullRoll *hr) {
    const HullDef *h = &k_hulls[cls];
    float perf = (hr->spd + hr->acc + hr->trn + hr->hull + hr->shd + hr->jmp)
                 * (1.0f / 6.0f);                 /* avg stat roll, ~1.0 */
    int basesl = 0, rollsl = 0;
    for (int i = 0; i < h->n_slots; i++)  basesl += h->slot_size[i];
    for (int i = 0; i < hr->n_slots; i++) rollsl += hr->slot_size[i];
    float slotf  = basesl ? (float)rollsl / (float)basesl : 1.0f;
    float cargof = (float)hr->cargo / (float)(h->cargo < 1 ? 1 : h->cargo);
    float utilf  = 1.0f + 0.05f * (float)(hr->utils - 2);
    float q = 0.40f * perf + 0.34f * slotf + 0.14f * cargof + 0.12f * utilf;
    return 1.0f + (q - 1.0f) * 1.7f;   /* amplify so features visibly matter */
}
static int hull_market_value(int cls, uint32_t seed, int econ) {
    HullRoll hr; hull_roll(cls, seed, &hr);
    float v = (float)k_hulls[cls].price * hull_quality_mult(cls, &hr) *
              econ_ship_mult(econ);
    return (int)(v + 0.5f);
}
static int player_tradein(int econ) {
    return (hull_market_value(g_player.hull_id, g_player.hull_seed, econ) * 7) / 10;
}

static void yard_build(void) {
    const SystemInfo *si = system_info();
    uint32_t h = (uint32_t)(si->seed >> 16) ^
                 (uint32_t)((s_station + 1) * 2654435761u);
    uint8_t used[N_HULLS] = {0};
    for (int i = 0; i < YARD_OFFERS; i++) {
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        /* Tech gates the big stuff; always at least one small hull. */
        int max_cls = 2 + (si->stations[s_station].tech * 8) / 15;
        if (max_cls >= N_HULLS) max_cls = N_HULLS - 1;
        int cls = (i == 0) ? (int)(h % 3u) : (int)(h % (uint32_t)(max_cls + 1));
        if (used[cls]) cls = (cls + 1) % (max_cls + 1);
        used[cls] = 1;
        s_yard[i].cls = (uint8_t)cls;
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        s_yard[i].seed = h;
        /* "SPARROW-K4": class name + seed-derived mark. */
        snprintf(s_yard[i].name, sizeof s_yard[i].name, "%s-%c%d",
                 k_hulls[cls].name, (char)('A' + (h >> 8) % 26u),
                 (int)(1 + (h >> 16) % 9u));
        /* Economy + quality price, with the occasional special offer. */
        int econ = si->stations[s_station].econ;
        int v = hull_market_value(cls, s_yard[i].seed, econ);
        uint32_t bh = s_yard[i].seed ^ 0x5A1E0FFEu;
        bh ^= bh >> 13; bh *= 1274126177u; bh ^= bh >> 16;
        s_yard[i].bargain = ((bh % 7u) == 0) ? 1 : 0;
        if (s_yard[i].bargain)
            v = (int)(v * (0.70f + 0.06f * (float)((bh >> 8) & 0xFF) / 255.0f));
        s_yard[i].price = v;
    }
}

void station_open(int station_idx) {
    s_detail = 0;
    s_screen = SCR_HOME;
    s_station = station_idx;
    s_cursor = 0;
    s_scroll = 0;
    s_bar_ev = NULL;
    s_bar_rolled = false;
    memset(s_bought, 0, sizeof s_bought);
    /* Debounce: everything counts as held until released once.
     * (Per-field true — memset(0xFF) breaks _Bool negation.) */
    s_prev.up = s_prev.down = s_prev.left = s_prev.right = true;
    s_prev.a = s_prev.b = s_prev.lb = s_prev.rb = s_prev.menu = true;
    s_hold_a = s_hold_b = s_repeat = 0;
    s_toast[0] = 0;
    s_toast_t = 0;
}

static void toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_t = 1.6f;
}

const Event *station_pending_event(void) {
    const Event *ev = s_bar_ev;
    s_bar_ev = NULL;          /* consumed — one approach per visit */
    return ev;
}

int station_preview2(uint32_t *mesh_seed, int *class_hint) {
    if (s_screen == SCR_HOME) return 1;
    if (s_screen == SCR_SHIPYARD) {
        if (s_cursor >= YARD_OFFERS) {        /* YOURS: your own hull */
            *mesh_seed = g_player.hull_seed;
            *class_hint = g_player.hull_id;
        } else {
            *mesh_seed = s_yard[s_cursor].seed;
            *class_hint = s_yard[s_cursor].cls;
        }
        return 2;
    }
    if (s_screen == SCR_STATUS) return 3;   /* own ship in the bay */
    return 0;
}

void station_toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_t = 2.8f;
}

static void try_buy(int good) {
    const SystemInfo *si = system_info();
    int price = econ_price(si, s_station, good, true);
    int stock = econ_stock(si, s_station, good) - s_bought[good];
    if (price <= 0) { toast("NO TRADE"); return; }
    if (stock <= 0) { toast("NO STOCK"); return; }
    if (g_player.credits < price) { toast("NO CREDITS"); return; }
    if (player_cargo_total() >= player_cargo_cap()) { toast("HOLD FULL"); return; }
    g_player.credits -= price;
    g_player.cargo[good]++;
    s_bought[good]++;
}

static void try_sell(int good) {
    const SystemInfo *si = system_info();
    int price = econ_price(si, s_station, good, false);
    if (g_player.cargo[good] == 0) { toast("NONE HELD"); return; }
    if (price <= 0) { toast("NO TRADE"); return; }
    g_player.cargo[good]--;
    g_player.credits += price;
}

/* Bulk variants for the market action menu (one toast, no spam). */
static void buy_max(int good) {
    const SystemInfo *si = system_info();
    int n = 0;
    for (;;) {
        int price = econ_price(si, s_station, good, true);
        int stock = econ_stock(si, s_station, good) - s_bought[good];
        if (price <= 0 || stock <= 0 || g_player.credits < price ||
            player_cargo_total() >= player_cargo_cap()) break;
        g_player.credits -= price; g_player.cargo[good]++; s_bought[good]++; n++;
    }
    if (n) { char b[20]; snprintf(b, sizeof b, "BOUGHT %d", n); toast(b); }
    else try_buy(good);                  /* shows why nothing happened */
}
static void sell_all(int good) {
    const SystemInfo *si = system_info();
    int n = 0;
    while (g_player.cargo[good] > 0) {
        int price = econ_price(si, s_station, good, false);
        if (price <= 0) break;
        g_player.cargo[good]--; g_player.credits += price; n++;
    }
    if (n) { char b[20]; snprintf(b, sizeof b, "SOLD %d", n); toast(b); }
    else try_sell(good);
}

static void try_refuel(void) {
    float need = g_player.fuel_max - g_player.fuel;
    if (need < 0.1f) { toast("TANK FULL"); return; }
    int cost = (int)(need * 12.0f) + 1;
    if (g_player.credits < cost) {
        /* Partial refuel with whatever credits allow. */
        float ly = (float)g_player.credits / 12.0f;
        if (ly < 0.1f) { toast("NO CREDITS"); return; }
        g_player.fuel += ly;
        g_player.credits = 0;
        toast("PART REFUEL");
        return;
    }
    g_player.credits -= cost;
    g_player.fuel = g_player.fuel_max;
    toast("REFUELLED");
}

static int service_hull_cost(void) {
    Ship *p = &g_ships[PLAYER];
    int missing = (int)(p->hull_max - p->hull);
    if (missing <= 0) return 0;
    return (int)(missing * 2.0f * skill_repair_mult()) + 1;
}

static void try_service(void) {
    /* SERVICE = rearm + hull patch in one bill (user-renamed). */
    int cost = player_rearm_cost() + service_hull_cost();
    if (cost <= 0) { toast("SHIP SHAPE"); return; }
    if (g_player.credits < cost) { toast("NO CREDITS"); return; }
    g_player.credits -= cost;
    player_rearm();
    g_ships[PLAYER].hull = g_ships[PLAYER].hull_max;
    player_apply_to_ship();
    char buf[24];
    snprintf(buf, sizeof buf, "SERVICED -%dCR", cost);
    toast(buf);
}

/* --- shipyard ----------------------------------------------------------*/
/* The part-worn KIT a used hull+seed arrives with -- rolled per SLOT
 * (deterministic, position-based) so a preview matches what's fitted on
 * purchase. Fills out[i] for i<rolled n_slots; empties stay in_use=0. */
void hull_kit_preview(int hull_id, uint32_t seed, WeaponInst out[HULL_SLOTS]) {
    for (int i = 0; i < HULL_SLOTS; i++) out[i] = (WeaponInst){0};
    HullRoll rv;
    hull_roll(hull_id, seed, &rv);
    uint32_t kr = seed ^ 0x6EA7u;
    for (int i = 0; i < rv.n_slots && i < HULL_SLOTS; i++) {
        kr ^= kr << 13; kr ^= kr >> 17; kr ^= kr << 5;
        if ((kr % 100u) >= 70) continue;        /* some stay empty */
        int sz = rv.slot_size[i];
        static const uint8_t z1[3] = { WPN_PULSE_S, WPN_AUTOCANNON,
                                       WPN_MINING };
        static const uint8_t z2[4] = { WPN_PULSE_M, WPN_ION,
                                       WPN_FLAK, WPN_HOMING };
        int ty = (sz >= 3) ? WPN_PULSE_L
               : (sz == 2) ? z2[(kr >> 8) % 4u]
                           : z1[(kr >> 8) % 3u];
        int q = (int)((kr >> 16) % 100u);
        out[i] = (WeaponInst){
            .type = (uint8_t)ty,
            .quality = (uint8_t)(q < 30 ? 0 : q < 80 ? 1 : q < 95 ? 2 : 3),
            .integrity = (uint8_t)(55 + (kr >> 24) % 41u),
            .in_use = 1,
            .affix = (uint8_t)(((kr >> 5) % 100u) < 15
                                   ? 1 + (kr >> 9) % (AFX_COUNT - 1) : 0),
            .ammo_flag = 1,
            .ammo_lo = (uint8_t)(k_weapons[ty].ammo_max * 2 / 5),
        };
    }
}

extern int turret_cal_for_seed(uint32_t seed);

/* Drill-down: the loadout a ship arrives with -- rolled weapons per
 * slot + the turret's calibration grade (user). Left column only;
 * the rotating 3D pane stays live. */
static void detail_draw_kit(uint16_t *fb, int hull_id, uint32_t seed,
                            const char *foot) {
    for (int yy = 0; yy < ELITE_FB_H; yy++) {
        int xmax = (yy >= 10 && yy < 95) ? 64 : ELITE_FB_W;
        uint16_t *r = fb + yy * ELITE_FB_W;
        for (int x = 0; x < xmax; x++) r[x] = COL_BG;
    }
    for (int yy = 10; yy < 95; yy++) fb[yy * ELITE_FB_W + 64] = COL_GRID;
    const HullDef *h = &k_hulls[hull_id];
    static const char *qt[5] = { "SLV", "STD", "RNF", "MIL", "PRO" };
    static const char *cal[4] = { "STANDARD", "REINFORCED",
                                  "MILITARY", "PROTOTYPE" };
    WeaponInst kit[HULL_SLOTS];
    hull_kit_preview(hull_id, seed, kit);
    HullRoll rv; hull_roll(hull_id, seed, &rv);
    char buf[28];
    int y = 4;
    craft_font_draw(fb, "WEAPONS:", 2, y, COL_HDR); y += 9;
    for (int i = 0; i < rv.n_slots && i < HULL_SLOTS; i++) {
        if (kit[i].in_use)
            snprintf(buf, sizeof buf, "Z%d %s %s", rv.slot_size[i],
                     k_weapons[kit[i].type].name, qt[kit[i].quality]);
        else
            snprintf(buf, sizeof buf, "Z%d EMPTY", rv.slot_size[i]);
        craft_font_draw(fb, buf, 4, y,
                        kit[i].in_use ? COL_TXT : COL_DIM);
        y += 8;
    }
    if (h->has_turret) {
        y += 3;
        craft_font_draw(fb, "TURRET:", 2, y, COL_HDR); y += 9;
        snprintf(buf, sizeof buf, "Z1 %s", cal[turret_cal_for_seed(seed)]);
        craft_font_draw(fb, buf, 4, y, COL_TXT); y += 8;
    }
    for (int x = 0; x < 64; x++) fb[118 * ELITE_FB_W + x] = COL_GRID;
    craft_font_draw(fb, foot, 2, 121, COL_DIM);
}

static void shipyard_buy(int offer) {
    int hull_id = s_yard[offer].cls;
    if (hull_id == g_player.hull_id &&
        s_yard[offer].seed == g_player.hull_seed) {
        toast("CURRENT SHIP");
        return;
    }
    const HullDef *h = &k_hulls[hull_id];
    int econ = system_info()->stations[s_station].econ;
    int tradein = player_tradein(econ);
    int cost = s_yard[offer].price - tradein;   /* <0 = trade-down, refund diff */
    if (cost > 0 && g_player.credits < cost) { toast("NO CREDITS"); return; }
    /* Cargo must fit the new hold. */
    if (player_cargo_total() > h->cargo) { toast("CARGO WONT FIT"); return; }
    g_player.credits -= cost;
    g_player.hull_id = (uint8_t)hull_id;
    g_player.hull_seed = s_yard[offer].seed;
    /* Turret: keep it if the new frame has a hardpoint; rack or sell
     * otherwise. */
    if (g_player.turret_eq.in_use && !h->has_turret) {
        int sl = -1;
        for (int t = 0; t < h->rack && t < MAX_SALVAGE; t++)
            if (!g_player.salvage[t].in_use) { sl = t; break; }
        if (sl >= 0)
            g_player.salvage[sl] = g_player.turret_eq;
        else
            g_player.credits += (int)(instance_price(&g_player.turret_eq) *
                                      0.35f);
        g_player.turret_eq.in_use = 0;
    }
    /* Fitted equipment over the frame's tier cap drops a tier. */
    if (g_player.shield_eq.in_use &&
        g_player.shield_eq.tier > h->max_shield_tier)
        g_player.shield_eq.tier = h->max_shield_tier;
    if (g_player.armor_eq.in_use &&
        g_player.armor_eq.tier > h->max_hull_tier)
        g_player.armor_eq.tier = h->max_hull_tier;
    /* Mounts that don't fit (count or size) drop to salvage, else sell. */
    for (int i = 0; i < HULL_SLOTS; i++) {
        WeaponInst *m = &g_player.mounts[i];
        if (!m->in_use) continue;
        bool fits = i < player_n_slots() &&
                    k_weapons[m->type].size <= player_slot_size(i);
        if (fits) continue;
        int sl = -1;
        for (int t = 0; t < h->rack && t < MAX_SALVAGE; t++)
            if (!g_player.salvage[t].in_use) { sl = t; break; }
        if (sl >= 0) {
            g_player.salvage[sl] = *m;
        } else {
            g_player.credits += weapon_price(m->type, m->quality) / 2;
        }
        m->in_use = 0;
    }
    /* The KIT (user req): used ships come with rolled gear — part-worn
     * guns of varied quality in some slots. Early-game progress is
     * swapping marginally better-rolled pieces; the empties are your
     * upgrade canvas. */
    {
        WeaponInst kit[HULL_SLOTS];
        hull_kit_preview(hull_id, s_yard[offer].seed, kit);
        for (int i = 0; i < player_n_slots(); i++) {
            if (g_player.mounts[i].in_use || !kit[i].in_use) continue;
            g_player.mounts[i] = kit[i];
            player_fit_restore_ammo(i);
        }
    }
    player_apply_to_ship();
    g_ships[PLAYER].hull = g_ships[PLAYER].hull_max;   /* delivered fresh */
    g_ships[PLAYER].shield = g_ships[PLAYER].shield_max;
    toast("SHIP DELIVERED");
}

/* --- outfitting ----------------------------------------------------------
 * Row model: mounts, then upgrades, then the salvage rack, then the
 * shop list. Rebuilt every tick (cheap; counts change under actions). */
typedef enum {
    ROW_MOUNT, ROW_EQUIP, ROW_UTIL, ROW_TURRET, ROW_SALV, ROW_SHOP,
    ROW_EQSHOP, ROW_UTILSHOP, ROW_HDR
} RowKind;
typedef struct { uint8_t kind, index; uint8_t tier; } OutfitRow;
static OutfitRow s_rows[HULL_SLOTS + 4 + MAX_SALVAGE + WPN_COUNT + 17];
static int s_n_rows;

/* Per-terminal armoury (user req): each station stocks a seeded,
 * tech-gated subset of the catalogue at economy-biased prices, plus
 * the occasional FEATURED non-standard instance (RNF/MIL/PRO) — the
 * legendary-gun reward for exploring far terminals. */
#define ARMORY_MAX 9
typedef struct {
    uint8_t type, quality, featured, affix;
    int32_t price;
} ArmoryItem;
static ArmoryItem s_armory[ARMORY_MAX];
static int s_n_armory;

static float econ_weapon_mult(int econ) {
    static const float k_m[8] = {
        /* AGRI */ 1.15f, /* INDUST */ 0.95f, /* HITECH */ 0.93f,
        /* EXTRACT */ 1.05f, /* REFINE */ 1.00f, /* TOURISM */ 1.10f,
        /* MILITARY */ 0.85f, /* SERVICE */ 1.00f,
    };
    return k_m[econ & 7];
}

static void armory_build(void) {
    const SystemInfo *si = system_info();
    const StationInfo *st = &si->stations[s_station];
    uint32_t h = (uint32_t)(si->seed >> 18) ^
                 (uint32_t)((s_station + 3) * 2654435761u) ^ 0xA4A4u;
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    float mult = econ_weapon_mult(st->econ) *
                 (0.95f + 0.10f * (float)(h & 0xFF) * (1.0f / 255.0f));

    /* Stock pool by tech: basics everywhere, exotics need a real yard. */
    uint8_t pool[WPN_COUNT];
    int pn = 0;
    pool[pn++] = WPN_PULSE_S;
    pool[pn++] = WPN_AUTOCANNON;
    pool[pn++] = WPN_PULSE_M;
    pool[pn++] = WPN_MISSILE;
    pool[pn++] = WPN_TRACTOR;
    pool[pn++] = WPN_MINING;
    if (st->tech >= 6) pool[pn++] = WPN_PLASMA;
    if (st->tech >= 11) pool[pn++] = WPN_LANCE;
    if (st->tech >= 8) pool[pn++] = WPN_BLASTER;
    if (st->tech >= 4) {
        pool[pn++] = WPN_FLAK;
        pool[pn++] = WPN_MINE;
    }
    if (st->tech >= 6) {
        pool[pn++] = WPN_BEAM;
        pool[pn++] = WPN_PULSE_L;
        pool[pn++] = WPN_HOMING;
    }
    if (st->tech >= 8)
        pool[pn++] = WPN_ION;
    if (st->tech >= 11) {
        pool[pn++] = WPN_PHOTON;
        pool[pn++] = WPN_GAUSS;
    }
    if (st->tech >= 12)
        pool[pn++] = WPN_RAILGUN;
    int want = 4 + (int)((h >> 8) % 3u);          /* 4-6 lines */
    if (want > pn) want = pn;
    int startp = (int)((h >> 16) % (uint32_t)pn);
    s_n_armory = 0;
    for (int k = 0; k < want; k++) {
        ArmoryItem *it = &s_armory[s_n_armory++];
        it->type = pool[(startp + k * 2 + (k > 2)) % pn];
        /* avoid dupes from the stride */
        for (int j = 0; j < s_n_armory - 1; j++)
            if (s_armory[j].type == it->type) { s_n_armory--; break; }
    }
    for (int i = 0; i < s_n_armory; i++) {
        s_armory[i].quality = Q_STANDARD;
        s_armory[i].featured = 0;
        s_armory[i].affix = AFX_NONE;
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        if ((h % 10u) == 0) s_armory[i].affix = AFX_SURPLUS;  /* bargain bin */
        s_armory[i].price =
            (int32_t)(weapon_price(s_armory[i].type, Q_STANDARD) * mult *
                      k_affixes[s_armory[i].affix].price);
    }

    /* Featured offers: 0-2, likelier at high tech. ANY weapon can
     * appear — a PRO gauss at a backwater is the exploration jackpot. */
    int n_feat = 0;
    uint32_t f = h * 0x9E3779B9u;
    f ^= f >> 15;
    if ((int)(f % 100u) < 25 + st->tech * 4) n_feat++;
    if (st->tech >= 9 && (int)((f >> 8) % 100u) < 30) n_feat++;
    for (int k = 0; k < n_feat && s_n_armory < ARMORY_MAX; k++) {
        f ^= f << 13; f ^= f >> 17; f ^= f << 5;
        ArmoryItem *it = &s_armory[s_n_armory++];
        it->type = (uint8_t)(f % WPN_COUNT);
        int qr = (int)((f >> 8) % 100u);
        it->quality = (qr < 60) ? Q_REINFORCED
                    : (qr < 90) ? Q_MILITARY : Q_PROTOTYPE;
        it->featured = 1;
        f ^= f << 13; f ^= f >> 17; f ^= f << 5;
        it->affix = (f & 1) ? (uint8_t)(AFX_OVERCLOCKED + f % 4u)
                            : AFX_NONE;
        if (it->affix >= AFX_COUNT) it->affix = AFX_VENTED;
        it->price = (int32_t)(weapon_price(it->type, it->quality) * mult *
                              k_affixes[it->affix].price);
    }
}

static void outfit_build_rows(void) {
    const HullDef *h = &k_hulls[g_player.hull_id];
    s_n_rows = 0;
    /* Sections: YOUR SHIP / YOUR HOLD / STATION SHOP (user req). */
    s_rows[s_n_rows++] = (OutfitRow){ ROW_HDR, 0, 0 };
    for (int i = 0; i < player_n_slots(); i++)
        s_rows[s_n_rows++] = (OutfitRow){ ROW_MOUNT, (uint8_t)i, 0 };
    s_rows[s_n_rows++] = (OutfitRow){ ROW_EQUIP, 0, 0 };   /* shield */
    s_rows[s_n_rows++] = (OutfitRow){ ROW_EQUIP, 1, 0 };   /* armor */
    for (int u = 0; u < player_util_slots(); u++)
        s_rows[s_n_rows++] = (OutfitRow){ ROW_UTIL, (uint8_t)u, 0 };
    if (h->has_turret)
        s_rows[s_n_rows++] = (OutfitRow){ ROW_TURRET, 0, 0 };
    s_rows[s_n_rows++] = (OutfitRow){ ROW_HDR, 1, 0 };
    for (int i = 0; i < MAX_SALVAGE; i++)
        if (g_player.salvage[i].in_use)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_SALV, (uint8_t)i, 0 };
    s_rows[s_n_rows++] = (OutfitRow){ ROW_HDR, 2, 0 };
    for (int i = 0; i < s_n_armory; i++)
        s_rows[s_n_rows++] = (OutfitRow){ ROW_SHOP, (uint8_t)i, 0 };
    /* Equipment shop: tiers the frame can take. */
    for (int t = 1; t <= h->max_shield_tier; t++)
        s_rows[s_n_rows++] = (OutfitRow){ ROW_EQSHOP, 0, (uint8_t)t };
    for (int t = 1; t <= h->max_hull_tier; t++)
        s_rows[s_n_rows++] = (OutfitRow){ ROW_EQSHOP, 1, (uint8_t)t };
    /* Gadgets: heatsink/scanner/tank everywhere; the fancy ones need
     * tech. index = EQ type offset from EQ_HEATSINK. */
    {
        const SystemInfo *si2 = system_info();
        int tech = si2->stations[s_station].tech;
        s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 0, 0 }; /* heatsink */
        s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 1, 0 }; /* scanner */
        s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 2, 0 }; /* tank */
        if (tech >= 7)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 3, 0 }; /* scoop */
        if (tech >= 9)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 4, 0 }; /* tcomp */
        if (tech >= 5)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 5, 0 }; /* chaff */
        if (tech >= 10)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 6, 0 }; /* drone */
        if (tech >= 9)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 7, 0 }; /* cloak */
        if (tech >= 7)
            s_rows[s_n_rows++] = (OutfitRow){ ROW_UTILSHOP, 8, 0 }; /* manifest */
    }
}

static WeaponInst *equip_slot(int which) {
    return which ? &g_player.armor_eq : &g_player.shield_eq;
}

/* Buy overflow: no compatible slot free -> the purchase goes to the
 * hold rack instead (user req: full mounts must not block buying). */
static bool buy_to_rack(const WeaponInst *inst) {
    int sl = player_free_rack_slot();
    if (sl < 0) return false;
    g_player.salvage[sl] = *inst;
    return true;
}

/* Can this row open a detail sheet? Single source of truth for the LB
 * open gate AND the in-sheet LB/RB cycle (they had drifted apart and
 * the new row types couldn't open at all — user report). */
static bool row_detailable(const OutfitRow *r) {
    switch (r->kind) {
    case ROW_MOUNT:  return g_player.mounts[r->index].in_use;
    case ROW_EQUIP:  return equip_slot(r->index)->in_use;
    case ROW_UTIL:   return g_player.util_eq[r->index].in_use;
    case ROW_TURRET: return g_player.turret_eq.in_use;
    case ROW_SALV:
    case ROW_SHOP:
    case ROW_EQSHOP:
    case ROW_UTILSHOP:
        return true;
    default:
        return false;
    }
}

static int repair_cost(const WeaponInst *w) {
    int base = weapon_price(w->type, w->quality);
    return (int)((100 - w->integrity) * base / 100 * 0.6f *
                 skill_repair_mult()) + 1;
}

static int free_slot_for(int wpn_type) {
    for (int i = 0; i < player_n_slots(); i++)
        if (!g_player.mounts[i].in_use &&
            k_weapons[wpn_type].size <= player_slot_size(i))
            return i;
    return -1;
}

/* --- action popup (user design: A on an item opens its actions) ---- */
enum { PACT_REPAIR, PACT_SWAP, PACT_UNFIT, PACT_SELL, PACT_FIT, PACT_TURRET };
static uint8_t s_pop_open, s_pop_n, s_pop_cur;
static uint8_t s_pop_acts[5];
static int s_pop_row;
/* swap picker: choose the counterpart explicitly (never lose a gun
 * to an implicit pick) */
static uint8_t s_pick_open, s_pick_n, s_pick_cur;
static int8_t s_pick_items[6];      /* mount idx or rack idx */
/* A opens an action menu (never an instant transaction): market BUY/SELL,
 * shipyard BUY confirm. Info stays for alternate views only. */
static uint8_t s_mkt_open, s_mkt_cur;   /* 0 = BUY, 1 = SELL */
static int     s_yard_confirm;          /* 0 none, else (ship index + 1) */
static const char *pact_name(int a) {
    switch (a) {
    case PACT_TURRET: return "TO TURRET";
    case PACT_REPAIR: return "REPAIR";
    case PACT_SWAP:   return "SWAP";
    case PACT_UNFIT:  return "UNFIT";
    case PACT_SELL:   return "SELL";
    default:          return "FIT";
    }
}

/* Ammo-aware 1-1 exchange between a weapon mount and a rack slot. */
static void swap_mount_rack(int mount, int rack) {
    WeaponInst *mv = &g_player.mounts[mount];
    WeaponInst *sv = &g_player.salvage[rack];
    player_stash_mount_ammo(mount);     /* outgoing keeps its magazine */
    WeaponInst tmp = *mv;
    *mv = *sv;
    *sv = tmp;
    player_fit_restore_ammo(mount);     /* incoming brings its own */
    player_apply_to_ship();
    toast("SWAPPED");
}

/* Build the popup's action list for the cursor row. Returns false if
 * the row has no popup (shop rows buy directly). */
static bool popup_build(int row) {
    if (row >= s_n_rows) return false;
    const OutfitRow *r = &s_rows[row];
    s_pop_n = 0;
    switch (r->kind) {
    case ROW_MOUNT: {
        const WeaponInst *m = &g_player.mounts[r->index];
        if (!m->in_use) return false;
        if (m->integrity < 100) s_pop_acts[s_pop_n++] = PACT_REPAIR;
        s_pop_acts[s_pop_n++] = PACT_SWAP;
        s_pop_acts[s_pop_n++] = PACT_UNFIT;
        s_pop_acts[s_pop_n++] = PACT_SELL;
        break;
    }
    case ROW_EQUIP:
    case ROW_UTIL:
    case ROW_TURRET: {
        const WeaponInst *e =
            (r->kind == ROW_EQUIP) ? equip_slot(r->index)
          : (r->kind == ROW_UTIL)  ? &g_player.util_eq[r->index]
                                   : &g_player.turret_eq;
        if (!e || !e->in_use) return false;
        if (e->integrity < 100) s_pop_acts[s_pop_n++] = PACT_REPAIR;
        s_pop_acts[s_pop_n++] = PACT_UNFIT;
        s_pop_acts[s_pop_n++] = PACT_SELL;
        break;
    }
    case ROW_SALV: {
        const WeaponInst *sv = &g_player.salvage[r->index];
        if (!sv->in_use) return false;
        if (sv->type < WPN_COUNT) {
            s_pop_acts[s_pop_n++] =
                (free_slot_for(sv->type) >= 0) ? PACT_FIT : PACT_SWAP;
            /* a Z1 gun can also arm a free turret hardpoint (user) */
            const HullDef *h = &k_hulls[g_player.hull_id];
            if (k_weapons[sv->type].size <= 1 && h->has_turret &&
                !g_player.turret_eq.in_use)
                s_pop_acts[s_pop_n++] = PACT_TURRET;
        } else {
            s_pop_acts[s_pop_n++] = PACT_FIT;   /* equip fit/swap path */
        }
        if (sv->integrity < 100) s_pop_acts[s_pop_n++] = PACT_REPAIR;
        s_pop_acts[s_pop_n++] = PACT_SELL;
        break;
    }
    default:
        return false;
    }
    s_pop_row = row;
    s_pop_cur = 0;
    return s_pop_n > 0;
}

/* Build the swap counterpart list. Fitted mount -> compatible racked
 * weapons; racked weapon -> occupied size-compatible mounts. */
static void pick_build(void) {
    const OutfitRow *r = &s_rows[s_pop_row];
    const HullDef *h = &k_hulls[g_player.hull_id];
    s_pick_n = 0;
    s_pick_cur = 0;
    if (r->kind == ROW_MOUNT) {
        int ssz = player_slot_size(r->index);
        for (int i = 0; i < MAX_SALVAGE && s_pick_n < 6; i++) {
            const WeaponInst *sv = &g_player.salvage[i];
            if (sv->in_use && sv->type < WPN_COUNT &&
                k_weapons[sv->type].size <= ssz)
                s_pick_items[s_pick_n++] = (int8_t)i;
        }
    } else {
        int wsz = k_weapons[g_player.salvage[r->index].type].size;
        for (int i = 0; i < player_n_slots() && s_pick_n < 6; i++)
            if (player_slot_size(i) >= wsz && g_player.mounts[i].in_use)
                s_pick_items[s_pick_n++] = (int8_t)i;
    }
}

static void outfit_action_a(int row);
static void outfit_action_b(int row);

/* Run the chosen popup action. REPAIR reuses the old A handlers; UNFIT
 * and SELL reuse B-paths or sell directly; FIT is the old rack fit;
 * SWAP opens the explicit counterpart picker. */
static void popup_execute(void) {
    const OutfitRow *r = &s_rows[s_pop_row];
    int act = s_pop_acts[s_pop_cur];
    s_pop_open = 0;
    switch (act) {
    case PACT_REPAIR:
        if (r->kind == ROW_SALV) {
            WeaponInst *sv = &g_player.salvage[r->index];
            int cost = (int)((100 - sv->integrity) *
                             instance_price(sv) / 100 *
                             0.6f * skill_repair_mult()) + 1;
            if (g_player.credits < cost) { toast("NO CREDITS"); return; }
            g_player.credits -= cost;
            sv->integrity = 100;
            g_player.xp_tech += 1;
            toast("REPAIRED");
        } else {
            outfit_action_a(s_pop_row);    /* old A = repair fitted */
        }
        break;
    case PACT_UNFIT:
        if (r->kind == ROW_MOUNT) player_stash_mount_ammo(r->index);
        outfit_action_b(s_pop_row);        /* old B = unfit to rack */
        break;
    case PACT_SELL:
        if (r->kind == ROW_SALV) {
            outfit_action_b(s_pop_row);    /* old B on rack = sell */
        } else if (r->kind == ROW_MOUNT) {
            WeaponInst *m = &g_player.mounts[r->index];
            player_stash_mount_ammo(r->index);
            int v = (int)(instance_price(m) *
                          (0.35f + 0.30f * m->integrity * 0.01f));
            g_player.credits += v;
            m->in_use = 0;
            player_apply_to_ship();
            toast("SOLD");
        } else {
            WeaponInst *e =
                (r->kind == ROW_EQUIP) ? equip_slot(r->index)
              : (r->kind == ROW_UTIL)  ? &g_player.util_eq[r->index]
                                       : &g_player.turret_eq;
            if (!e || !e->in_use) return;
            int v = (int)(instance_price(e) *
                          (0.35f + 0.30f * e->integrity * 0.01f));
            g_player.credits += v;
            e->in_use = 0;
            player_apply_to_ship();
            toast("SOLD");
        }
        break;
    case PACT_FIT:
        outfit_action_a(s_pop_row);        /* old A on rack = fit/swap */
        break;
    case PACT_TURRET: {
        if (r->kind != ROW_SALV) break;
        WeaponInst *sv = &g_player.salvage[r->index];
        if (!sv->in_use || sv->type >= WPN_COUNT) break;
        if (g_player.turret_eq.in_use) { toast("TURRET FULL"); break; }
        g_player.turret_eq = *sv;
        sv->in_use = 0;
        player_apply_to_ship();
        toast("TURRET ARMED");
        break;
    }
    case PACT_SWAP:
        pick_build();
        if (s_pick_n == 0) {
            if (r->kind == ROW_SALV) {
                char tb[20];
                snprintf(tb, sizeof tb, "NEEDS Z%d SLOT",
                         k_weapons[g_player.salvage[r->index].type].size);
                toast(tb);
            } else {
                toast("NOTHING RACKED");
            }
            return;
        }
        s_pick_open = 1;
        break;
    }
}

static void outfit_action_a(int row) {
    if (row >= s_n_rows) return;
    const OutfitRow *r = &s_rows[row];
    const HullDef *h = &k_hulls[g_player.hull_id];
    switch (r->kind) {
    case ROW_MOUNT: {
        WeaponInst *m = &g_player.mounts[r->index];
        if (!m->in_use) { toast("EMPTY MOUNT"); return; }
        if (m->integrity >= 100) { toast("NO DAMAGE"); return; }
        int cost = repair_cost(m);
        if (g_player.credits < cost) { toast("NO CREDITS"); return; }
        g_player.credits -= cost;
        m->integrity = 100;
        g_player.xp_tech += 1;
        player_apply_to_ship();
        toast("REPAIRED");
        break;
    }
    case ROW_EQUIP: {
        WeaponInst *e = equip_slot(r->index);
        if (!e->in_use) { toast("NOT FITTED"); return; }
        if (e->integrity >= 100) { toast("NO DAMAGE"); return; }
        int cost = (int)((100 - e->integrity) *
                         equip_price(e->type, e->tier, e->quality) / 100 *
                         0.6f * skill_repair_mult()) + 1;
        if (g_player.credits < cost) { toast("NO CREDITS"); return; }
        g_player.credits -= cost;
        e->integrity = 100;
        g_player.xp_tech += 1;
        player_apply_to_ship();
        toast("REPAIRED");
        break;
    }
    case ROW_UTIL: {
        WeaponInst *e = &g_player.util_eq[r->index];
        if (!e->in_use) { toast("EMPTY BAY"); return; }
        if (e->integrity >= 100) { toast("NO DAMAGE"); return; }
        int cost = (int)((100 - e->integrity) * instance_price(e) / 100 *
                         0.6f * skill_repair_mult()) + 1;
        if (g_player.credits < cost) { toast("NO CREDITS"); return; }
        g_player.credits -= cost;
        e->integrity = 100;
        g_player.xp_tech += 1;
        toast("REPAIRED");
        break;
    }
    case ROW_UTILSHOP: {
        int type = EQ_HEATSINK + r->index;
        const SystemInfo *sie = system_info();
        int price = (int)(k_equip[type - WPN_COUNT].base_price *
                          econ_weapon_mult(sie->stations[s_station].econ) *
                          skill_price_mult());
        if (g_player.credits < price) { toast("NO CREDITS"); return; }
        int slot = -1;
        for (int u = 0; u < player_util_slots(); u++)
            if (!g_player.util_eq[u].in_use) { slot = u; break; }
        WeaponInst inst = { .type = (uint8_t)type, .quality = Q_STANDARD,
                            .integrity = 100, .in_use = 1 };
        if (slot >= 0) {
            g_player.credits -= price;
            g_player.util_eq[slot] = inst;
            if (type == EQ_CHAFF) g_player.chaff_charges = 4;
            toast("FITTED");
        } else if (buy_to_rack(&inst)) {
            g_player.credits -= price;
            toast("TO HOLD");
        } else {
            toast("NO SPACE");
        }
        break;
    }
    case ROW_TURRET: {
        if (g_player.turret_eq.in_use) { toast("ALREADY FITTED"); return; }
        for (int t = 0; t < MAX_SALVAGE; t++) {
            WeaponInst *sv = &g_player.salvage[t];
            if (!sv->in_use || sv->type >= WPN_COUNT) continue;
            if (k_weapons[sv->type].size > 1) continue;
            g_player.turret_eq = *sv;
            sv->in_use = 0;
            player_apply_to_ship();
            toast("TURRET ARMED");
            return;
        }
        toast("NEED Z1 ON RACK");
        break;
    }
    case ROW_EQSHOP: {
        int type = WPN_COUNT + r->index;
        const SystemInfo *sie = system_info();
        /* This station's variant for this tier (seeded). */
        uint32_t vh = (uint32_t)(sie->seed >> 22) ^
                      (uint32_t)((s_station + 1) * 7129u) ^
                      (uint32_t)(r->index * 31u + r->tier);
        vh ^= vh >> 13; vh *= 1274126177u; vh ^= vh >> 16;
        uint8_t variant = (uint8_t)(vh % 4u);
        float vprice = (variant == SHV_BULWARK || variant == ARV_ABLATIVE)
                           ? 1.25f : (variant ? 1.15f : 1.0f);
        int price = (int)(equip_price(type, r->tier, Q_STANDARD) *
                          econ_weapon_mult(sie->stations[s_station].econ) *
                          vprice * skill_price_mult());
        if (g_player.credits < price) { toast("NO CREDITS"); return; }
        WeaponInst *e = equip_slot(r->index);
        WeaponInst inst = { .type = (uint8_t)type, .quality = Q_STANDARD,
                            .integrity = 100, .in_use = 1,
                            .tier = (uint8_t)r->tier, .affix = variant };
        if (!e->in_use) {
            g_player.credits -= price;
            *e = inst;
            player_apply_to_ship();
            toast("FITTED");
        } else if (buy_to_rack(&inst)) {
            /* Slot occupied: the new unit goes to the hold — swap it in
             * from the rack later; no more forced trade-in. */
            g_player.credits -= price;
            toast("TO HOLD");
        } else {
            toast("NO SPACE");
        }
        break;
    }
    case ROW_SALV: {
        WeaponInst *sv = &g_player.salvage[r->index];
        if (sv->type >= EQ_HEATSINK) {
            /* Gadget from the rack: into a free utility bay. */
            int slot2 = -1;
            for (int u = 0; u < player_util_slots(); u++)
                if (!g_player.util_eq[u].in_use) { slot2 = u; break; }
            if (slot2 < 0) { toast("BAYS FULL"); return; }
            g_player.util_eq[slot2] = *sv;
            sv->in_use = 0;
            if (g_player.util_eq[slot2].type == EQ_CHAFF)
                g_player.chaff_charges = 4;
            toast("FITTED");
            return;
        }
        if (sv->type >= WPN_COUNT) {
            /* Equipment from the rack: swap into its slot. */
            int which = sv->type - WPN_COUNT;
            const HullDef *hh = &k_hulls[g_player.hull_id];
            int cap = which ? hh->max_hull_tier : hh->max_shield_tier;
            if (sv->tier > cap) { toast("FRAME LIMIT"); return; }
            WeaponInst *e = equip_slot(which);
            WeaponInst old = *e;
            *e = *sv;
            if (old.in_use) *sv = old; else sv->in_use = 0;
            player_apply_to_ship();
            toast("FITTED");
            return;
        }
        int slot = free_slot_for(sv->type);
        if (slot < 0) {
            /* shouldn't happen via the popup (FIT is gated on a free
             * slot; SWAP uses the picker) — safety toast only */
            char tb[20];
            snprintf(tb, sizeof tb, "NEEDS Z%d SLOT",
                     k_weapons[sv->type].size);
            toast(tb);
            return;
        }
        g_player.mounts[slot] = *sv;
        sv->in_use = 0;
        /* Stored magazine if the instance carries one; factory rules
         * otherwise (sealed = full, salvage = 40%). */
        player_fit_restore_ammo(slot);
        player_apply_to_ship();
        toast("FITTED");
        break;
    }
    case ROW_SHOP: {
        /* r->index is an ARMOURY list index, not a weapon type — the
         * pre-armoury code here checked/bought by list position
         * (user-reported: Z1 autocannon refused 'NO FREE SLOT'). */
        const ArmoryItem *it = &s_armory[r->index];
        int price = (int)(it->price * skill_price_mult());
        if (g_player.credits < price) { toast("NO CREDITS"); return; }
        WeaponInst inst = { .type = it->type, .quality = it->quality,
                            .integrity = 100, .in_use = 1,
                            .affix = it->affix };
        int slot = free_slot_for(it->type);
        if (slot >= 0) {
            g_player.credits -= price;
            g_player.mounts[slot] = inst;
            player_load_mount_ammo(slot, 1.0f);   /* sold fully loaded */
            player_apply_to_ship();
            toast("FITTED");
        } else if (buy_to_rack(&inst)) {
            g_player.credits -= price;
            toast("TO HOLD");
        } else {
            toast("NO SPACE");
        }
        break;
    }
    }
}

static void outfit_action_b(int row) {
    if (row >= s_n_rows) return;
    const OutfitRow *r = &s_rows[row];
    switch (r->kind) {
    case ROW_TURRET: {
        if (!g_player.turret_eq.in_use) return;
        int sl = player_free_rack_slot();
        if (sl < 0) {
            toast("NO RACK SPACE");
            return;
        }
        g_player.salvage[sl] = g_player.turret_eq;
        g_player.turret_eq.in_use = 0;
        player_apply_to_ship();
        toast("UNFITTED");
        break;
    }
    case ROW_UTIL: {
        WeaponInst *e = &g_player.util_eq[r->index];
        if (!e->in_use) return;
        int sl = player_free_rack_slot();
        if (sl < 0) {
            toast("NO RACK SPACE");
            return;
        }
        g_player.salvage[sl] = *e;
        e->in_use = 0;
        toast("UNFITTED");
        break;
    }
    case ROW_EQUIP: {
        WeaponInst *e = equip_slot(r->index);
        if (!e->in_use) return;
        int sl = player_free_rack_slot();
        if (sl < 0) {
            toast("NO RACK SPACE");
            return;
        }
        g_player.salvage[sl] = *e;
        e->in_use = 0;
        player_apply_to_ship();
        toast("UNFITTED");
        break;
    }
    case ROW_MOUNT: {
        /* Unmount into the salvage rack (magazine rides along). */
        WeaponInst *m = &g_player.mounts[r->index];
        if (!m->in_use) return;
        player_stash_mount_ammo(r->index);
        int sl = player_free_rack_slot();
        if (sl < 0) {
            toast("NO RACK SPACE");
            return;
        }
        g_player.salvage[sl] = *m;
        m->in_use = 0;
        player_apply_to_ship();
        toast("UNMOUNTED");
        break;
    }
    case ROW_SALV: {
        /* Sell from the rack: value scales with quality + integrity. */
        WeaponInst *sv = &g_player.salvage[r->index];
        int v = (int)(instance_price(sv) *
                      (0.35f + 0.30f * sv->integrity * 0.01f));
        g_player.credits += v;
        sv->in_use = 0;
        toast("SOLD");
        break;
    }
    default:
        break;
    }
}

/* Guide harness: which docked screen is open (SCR_* as int:
 * 0 HOME, 1 MARKET, 2 SHIPYARD, 3 OUTFIT, 4 STATUS, 5 MISSIONS,
 * 6 BAR, 7 CODEX). Lets the recorder verify a menu actually opened
 * before captioning, instead of guessing at button timing. */
int station_debug_screen(void) { return (int)s_screen; }

DockAction station_tick(const CraftRawButtons *btn, float dt) {
    DockAction act = DOCK_NONE;
    bool a_edge = btn->a && !s_prev.a;
    bool b_edge = btn->b && !s_prev.b;
    bool lb_edge = btn->lb && !s_prev.lb;
    bool left  = btn->left  && !s_prev.left;    /* compare prev */
    bool right = btn->right && !s_prev.right;   /* compare next */
    /* Hold-to-scroll (user req): edge fires immediately, then repeats
     * after 0.35s at ~8/s. */
    static float s_rep_up, s_rep_dn;
    bool up = false, down = false;
    if (btn->up) {
        if (!s_prev.up) { up = true; s_rep_up = 0; }
        else {
            s_rep_up += dt;
            if (s_rep_up > 0.35f) { s_rep_up -= 0.12f; up = true; }
        }
    } else s_rep_up = 0;
    if (btn->down) {
        if (!s_prev.down) { down = true; s_rep_dn = 0; }
        else {
            s_rep_dn += dt;
            if (s_rep_dn > 0.35f) { s_rep_dn -= 0.12f; down = true; }
        }
    } else s_rep_dn = 0;
    bool back = (btn->menu && !s_prev.menu) || b_edge;   /* B or MENU = back */

    if (s_toast_t > 0) s_toast_t -= dt;
    if (up || down) sfx_ui_move();
    if (a_edge) sfx_ui_select();

    /* Hold-to-repeat for market trading. */
    bool a_rep = false, b_rep = false;
    s_hold_a = btn->a ? s_hold_a + dt : 0;
    s_hold_b = btn->b ? s_hold_b + dt : 0;
    if (s_hold_a > 0.45f || s_hold_b > 0.45f) {
        s_repeat += dt;
        if (s_repeat >= 0.09f) {
            s_repeat = 0;
            if (s_hold_a > 0.45f) a_rep = true;
            if (s_hold_b > 0.45f) b_rep = true;
        }
    }

    switch (s_screen) {
    case SCR_HOME:
        if (up && s_cursor > 0) s_cursor--;
        if (down && s_cursor < HOME_ITEMS - 1) s_cursor++;
        if (a_edge) {
            if (s_cursor == 0) { s_screen = SCR_MARKET; s_cursor = 0; s_scroll = 0; }
            else if (s_cursor == 1) {
                yard_build();
                s_screen = SCR_SHIPYARD; s_cursor = 0; s_scroll = 0;
            }
            else if (s_cursor == 2) {
                armory_build();
                s_screen = SCR_OUTFIT; s_cursor = 1; s_scroll = 0;
            }
            else if (s_cursor == 3) {
                mission_make_offers(system_info(), s_station, s_offers);
                s_screen = SCR_MISSIONS; s_cursor = 0;
            }
            else if (s_cursor == 4) {
                s_screen = SCR_BAR;
                if (!s_bar_rolled) {           /* one roll per dock visit */
                    s_bar_rolled = true;
                    s_bar_ev = events_roll_bar(system_info(), s_station);
                }
            }
            else if (s_cursor == 5) { s_screen = SCR_STATUS; status_open(); }
            else if (s_cursor == 6) { s_screen = SCR_CODEX; s_cursor = 0; }
            else if (s_cursor == 7) try_refuel();
            else if (s_cursor == 8) try_service();
            else if (s_cursor == 9) {
                if (g_player.fine <= 0) toast("RECORD CLEAN");
                else if (g_player.credits < g_player.fine)
                    toast("NO CREDITS");
                else {
                    g_player.credits -= g_player.fine;
                    g_player.fine = 0;
                    g_player.legal = 0;
                    elite_game_police_stand_down();
                    toast("RECORD CLEARED");
                }
            }
            else if (s_cursor == 10) act = DOCK_LAUNCH;
        }
        if (back) act = DOCK_LAUNCH;           /* MENU = leave */
        break;

    case SCR_SHIPYARD:
        /* Row YARD_OFFERS (the 6th) is YOUR ship — comparable in the
         * detail cycle, never buyable (user req). */
        if (s_yard_confirm) {                  /* A = select, then confirm */
            if (a_edge) {
                shipyard_buy(s_yard_confirm - 1);
                s_yard_confirm = 0; s_detail = 0; s_kit_view = 0;
            } else if (back) s_yard_confirm = 0;
            break;
        }
        if (s_detail) {
            if (left)  s_cursor = (s_cursor + YARD_OFFERS) % (YARD_OFFERS + 1);
            if (right) s_cursor = (s_cursor + 1) % (YARD_OFFERS + 1);
            if (lb_edge) s_kit_view = !s_kit_view;       /* Info = kit view */
            if (a_edge && s_cursor < YARD_OFFERS) s_yard_confirm = s_cursor + 1;
            if (back) { if (s_kit_view) s_kit_view = 0; else s_detail = 0; }
            break;
        }
        if (up && s_cursor > 0) s_cursor--;
        if (down && s_cursor < YARD_OFFERS) s_cursor++;
        if (lb_edge) { s_detail = 1; s_kit_view = 0; }   /* Info = details */
        if (a_edge && s_cursor < YARD_OFFERS) s_yard_confirm = s_cursor + 1;
        if (back) { s_screen = SCR_HOME; s_cursor = 1; }
        break;

    case SCR_OUTFIT:
        outfit_build_rows();
        if (s_pick_open) {
            /* explicit swap counterpart picker */
            if (up && s_pick_cur > 0) s_pick_cur--;
            if (down && s_pick_cur < s_pick_n - 1) s_pick_cur++;
            if (a_edge) {
                const OutfitRow *r = &s_rows[s_pop_row];
                int other = s_pick_items[s_pick_cur];
                if (r->kind == ROW_MOUNT)
                    swap_mount_rack(r->index, other);
                else
                    swap_mount_rack(other, r->index);
                s_pick_open = 0;
            }
            if (b_edge || back) s_pick_open = 0;
            break;
        }
        if (s_pop_open) {
            if (up && s_pop_cur > 0) s_pop_cur--;
            if (down && s_pop_cur < s_pop_n - 1) s_pop_cur++;
            if (a_edge) popup_execute();
            if (b_edge || back) s_pop_open = 0;
            break;
        }
        if (s_detail) {
            /* Left/Right loop through every row WITH a sheet, wrapping at the
             * ends — headers, empty mounts and bare slots are skipped. */
            if (left || right) {
                int dir = right ? 1 : -1;
                int n = s_cursor;
                for (int tries = 0; tries < s_n_rows; tries++) {
                    n = (n + dir + s_n_rows) % s_n_rows;
                    if (row_detailable(&s_rows[n])) { s_cursor = n; break; }
                }
            }
            if (a_edge) { outfit_action_a(s_cursor); s_detail = 0; }
            if (back) s_detail = 0;
            break;
        }
        if (up && s_cursor > 0) {
            s_cursor--;
            while (s_cursor > 0 && s_rows[s_cursor].kind == ROW_HDR)
                s_cursor--;
            if (s_rows[s_cursor].kind == ROW_HDR) s_cursor++;
        }
        if (down && s_cursor < s_n_rows - 1) {
            s_cursor++;
            while (s_cursor < s_n_rows - 1 && s_rows[s_cursor].kind == ROW_HDR)
                s_cursor++;
            if (s_rows[s_cursor].kind == ROW_HDR) s_cursor--;
        }
        if (s_cursor < s_scroll) s_scroll = s_cursor;
        if (s_cursor > s_scroll + 5) s_scroll = s_cursor - 5;  /* ~6 readable rows */
        if (lb_edge && row_detailable(&s_rows[s_cursor]))
            s_detail = 1;                       /* Info = detail sheet */
        if (a_edge) {
            /* Items open their action popup (user design); shop rows
             * still buy directly. UNFIT/SELL live in the popup, so B is
             * free for Back. */
            if (popup_build(s_cursor)) s_pop_open = 1;
            else outfit_action_a(s_cursor);
        }
        if (back) { s_screen = SCR_HOME; s_cursor = 2; }
        break;

    case SCR_MISSIONS: {
        if (up && s_cursor > 0) s_cursor--;
        if (down && s_cursor < MISSION_OFFERS - 1) s_cursor++;
        if (a_edge) {
            const Mission *m = &s_offers[s_cursor];
            if (m->type == MIS_NONE) toast("NO OFFER");
            else if (mission_accept(m)) {
                toast("ACCEPTED");
                s_offers[s_cursor].type = MIS_NONE;
            } else toast("LOG/HOLD FULL");
        }
        if (back) { s_screen = SCR_HOME; s_cursor = 3; }
        break;
    }

    case SCR_BAR:
        if (s_bar_ev && a_edge) { act = DOCK_EVENT; break; }
        if (back || a_edge) { s_screen = SCR_HOME; s_cursor = 4; }
        break;

    case SCR_CODEX: {
        /* Unlocked entries are selectable; locked rows show as ???. */
        if (s_detail) {
            if (back || a_edge) s_detail = 0;
            break;
        }
        if (up && s_cursor > 0) s_cursor--;
        if (down && s_cursor < k_n_lore - 1) s_cursor++;
        if (a_edge && events_lore_seen(s_cursor)) s_detail = 1;
        if (back) { s_detail = 0; s_screen = SCR_HOME; s_cursor = 6; }
        break;
    }

    case SCR_STATUS:
        if (status_tick(btn, dt)) { s_screen = SCR_HOME; s_cursor = 5; }
        s_prev = *btn;
        return act;

    case SCR_MARKET:
        if (s_mkt_open) {                  /* A's BUY / BUY MAX / SELL / SELL ALL */
            if (up   && s_mkt_cur > 0) s_mkt_cur--;
            if (down && s_mkt_cur < 3) s_mkt_cur++;
            bool single = (s_mkt_cur == 0 || s_mkt_cur == 2);
            if (a_edge || (a_rep && single)) {     /* hold A repeats single buy/sell */
                switch (s_mkt_cur) {
                case 0: try_buy(s_cursor);  break;
                case 1: buy_max(s_cursor);  break;
                case 2: try_sell(s_cursor); break;
                default: sell_all(s_cursor); break;
                }
            }
            if (back) s_mkt_open = 0;
            break;
        }
        if (up && s_cursor > 0) s_cursor--;
        if (down && s_cursor < N_GOODS - 1) s_cursor++;
        if (s_cursor < s_scroll) s_scroll = s_cursor;
        if (s_cursor > s_scroll + 8) s_scroll = s_cursor - 8;
        if (a_edge) { s_mkt_open = 1; s_mkt_cur = 0; }
        if (back) { s_screen = SCR_HOME; s_cursor = 0; }
        break;
    }

    s_prev = *btn;
    return act;
}

/* --- drawing ------------------------------------------------------------*/
static void fill(uint16_t *fb, uint16_t c) {
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++) fb[i] = c;
}
/* Fill everything EXCEPT the 3D preview pane (right column, body rows) —
 * the scene behind shows the rotating station/ship there. */
static void fill_with_pane(uint16_t *fb, uint16_t c, int body_y0, int body_y1) {
    for (int y = 0; y < ELITE_FB_H; y++) {
        int xmax = (y >= body_y0 && y < body_y1) ? 64 : ELITE_FB_W;
        uint16_t *row = fb + y * ELITE_FB_W;
        for (int x = 0; x < xmax; x++) row[x] = c;
    }
    for (int y = body_y0; y < body_y1; y++)
        fb[y * ELITE_FB_W + 64] = COL_GRID;
}
static void hl(uint16_t *fb, int y, uint16_t c) {
    for (int x = 0; x < ELITE_FB_W; x++) fb[y * ELITE_FB_W + x] = c;
}

static void draw_header(uint16_t *fb) {
    const SystemInfo *si = system_info();
    char buf[24];
    snprintf(buf, sizeof buf, "%dCR", g_player.credits);
    int credx = 126 - eui_textw(buf);           /* left edge of the credits */
    /* Clip the (variable-length) station name so it never slides under the
       credits — long names like "QUATAR STATION" used to collide. */
    char nm[24];
    snprintf(nm, sizeof nm, "%s", si->stations[s_station].name);
    for (int n = (int)strlen(nm); n > 0 && eui_textw(nm) > credx - 5; n--) nm[n - 1] = 0;
    eui_text(fb, nm, 2, 1, COL_HDR);
    eui_textr(fb, buf, 126, 1, COL_CRED);
    hl(fb, 13, COL_GRID);
}

static void draw_home(uint16_t *fb) {
    draw_header(fb);
    static const char *k_econ[8] = {   /* short tags: sit over the 3D render, not the menu */
        "AGRI", "IND", "TECH", "EXTR", "REFN", "TOUR", "MIL", "SVC",
    };
    const SystemInfo *si = system_info();
    char buf[32];
    snprintf(buf, sizeof buf, "%s T%d", k_econ[si->stations[s_station].econ],
             si->stations[s_station].tech);
    craft_font_draw(fb, buf, 126 - craft_font_width(buf), 15, COL_DIM);

    /* Readable Audiowide menu in the LEFT column (the 3D station render owns the right).
     * Colour alone marks the selection (no caret — saves width); tight line spacing and
     * a tall window fit more rows; scrollbar sits at the column edge, clear of the render. */
    int y0 = 14, win = 117 - y0, lh;
    int vis = eui_fit(win, HOME_ITEMS, &lh);        /* fill the column: optimal rows + spacing */
    if (s_cursor < s_home_scroll)         s_home_scroll = s_cursor;
    if (s_cursor >= s_home_scroll + vis)  s_home_scroll = s_cursor - vis + 1;
    if (s_home_scroll > HOME_ITEMS - vis) s_home_scroll = HOME_ITEMS - vis;
    if (s_home_scroll < 0) s_home_scroll = 0;
    for (int r = 0; r < vis && s_home_scroll + r < HOME_ITEMS; r++) {
        int i = s_home_scroll + r, y = y0 + r*lh;
        uint16_t c = (i == s_cursor) ? COL_CUR : COL_DIM;
        eui_text(fb, k_home[i], 3, y, c);
        if (i == 7) {                              /* REFUEL cost */
            float need = g_player.fuel_max - g_player.fuel;
            if (need >= 0.1f) { snprintf(buf, sizeof buf, "%d", (int)(need * 12.0f) + 1);
                                eui_textr(fb, buf, 58, y, COL_CRED); }
        } else if (i == 8) {                       /* SERVICE cost */
            int rc = player_rearm_cost() + service_hull_cost();
            if (rc > 0) { snprintf(buf, sizeof buf, "%d", rc); eui_textr(fb, buf, 58, y, COL_CRED); }
        } else if (i == 9 && g_player.fine > 0) {  /* PAY FINE */
            snprintf(buf, sizeof buf, "%d", g_player.fine);
            eui_textr(fb, buf, 58, y, RGB565C(255, 120, 70));
        }
    }
    eui_scrollbar(fb, 63, y0, win, HOME_ITEMS, vis, s_home_scroll, COL_CUR, COL_GRID);

    /* Fuel + cargo live under the station pane on the right — the
     * 10-row service list (PAY FINE) reclaimed their old left slot. */
    char fuel[24];
    snprintf(fuel, sizeof fuel, "FUEL %d.%d/%dLY",
             (int)g_player.fuel, ((int)(g_player.fuel * 10)) % 10,
             (int)g_player.fuel_max);
    craft_font_draw(fb, fuel, 66, 100, COL_DIM);
    snprintf(fuel, sizeof fuel, "CARGO %d/%d", player_cargo_total(),
             player_cargo_cap());
    craft_font_draw(fb, fuel, 66, 108, COL_DIM);
    hl(fb, 118, COL_GRID);
    { char h[32]; snprintf(h, sizeof h, "%s:OPEN  %s:LEAVE",
        plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 2, 121, COL_DIM); }
}

/* Centred action menu (market BUY/SELL, etc.). */
static void draw_action_box(uint16_t *fb, const char *title,
                            const char *const *items, int n, int cur) {
    int w = 100, h = 15 + n * 10;
    int x0 = (128 - w) / 2, y0 = (128 - h) / 2;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) fb[y * ELITE_FB_W + x] = RGB565C(8, 11, 20);
    for (int x = x0; x < x0 + w; x++) {
        fb[y0 * ELITE_FB_W + x] = COL_GRID;
        fb[(y0 + h - 1) * ELITE_FB_W + x] = COL_GRID;
    }
    for (int y = y0; y < y0 + h; y++) {
        fb[y * ELITE_FB_W + x0] = COL_GRID;
        fb[y * ELITE_FB_W + x0 + w - 1] = COL_GRID;
    }
    craft_font_draw(fb, title, x0 + 5, y0 + 3, COL_HDR);
    for (int i = 0; i < n; i++) {
        uint16_t c = (i == cur) ? COL_CUR : COL_DIM;
        if (i == cur) craft_font_draw(fb, ">", x0 + 5, y0 + 14 + i * 10, c);
        craft_font_draw(fb, items[i], x0 + 14, y0 + 14 + i * 10, c);
    }
}

/* Cyan = bargain to buy (well under galactic base), gold = sells high here. */
static uint16_t mkt_buy_col(int buy, int base, uint16_t fallback) {
    return (buy > 0 && buy * 100 < base * 95) ? RGB565C(90, 200, 255) : fallback;
}
static uint16_t mkt_sell_col(int sell, int base, uint16_t fallback) {
    return (sell * 100 > base * 108) ? RGB565C(245, 200, 80) : fallback;
}

static void draw_market(uint16_t *fb) {
    draw_header(fb);
    const SystemInfo *si = system_info();
    char buf[16];

    /* Small column header: goods on the left, buy/sell over their numbers. */
    craft_font_draw(fb, "GOODS", 4, 15, COL_DIM);
    craft_font_draw(fb, "BUY",  100 - craft_font_width("BUY"),  15, COL_DIM);
    craft_font_draw(fb, "SELL", 122 - craft_font_width("SELL"), 15, COL_DIM);

    /* Icon browser: one commodity per compact row (12px icon + readable name +
       buy/sell), five at a time; the selection's stock/held shows on the detail
       strip above the footer. */
    int y0 = 22, row_h = 13;
    int vis = (101 - y0) / row_h; if (vis < 1) vis = 1;   /* 6 rows */
    if (s_cursor < s_scroll)          s_scroll = s_cursor;
    if (s_cursor >= s_scroll + vis)   s_scroll = s_cursor - vis + 1;
    if (N_GOODS > vis && s_scroll > N_GOODS - vis) s_scroll = N_GOODS - vis;
    if (s_scroll < 0) s_scroll = 0;

    for (int r = 0; r < vis && s_scroll + r < N_GOODS; r++) {
        int i = s_scroll + r, ry = y0 + r * row_h;
        bool illegal = (k_goods[i].flags & GOOD_ILLEGAL) != 0;
        bool sel = (i == s_cursor);
        if (sel && g_em && g_em->draw_rect)          /* selection band */
            g_em->draw_rect(fb, 0, ry, 128, row_h, RGB565C(30, 38, 58), 1, 0, 128);
        if (g_em && g_em->blit)                       /* commodity icon from the sheet */
            g_em->blit(fb, &goods_img, 2, ry + 1, (i % goods_COLS) * goods_CELLW,
                       (i / goods_COLS) * goods_CELLH, goods_CELLW, goods_CELLH, 0, 0, 128);
        uint16_t nc = sel ? COL_CUR : illegal ? COL_ILL : COL_DIM;
        eui_textclip(fb, k_goods[i].name, 18, 76, ry + 2, nc);
        int buy = econ_price(si, s_station, i, true);
        int sell = econ_price(si, s_station, i, false);
        int base = (int)k_goods[i].base;
        if (buy > 0) {
            snprintf(buf, sizeof buf, "%d", buy);
            eui_textr(fb, buf, 100, ry + 2, mkt_buy_col(buy, base, nc));
            snprintf(buf, sizeof buf, "%d", sell);
            eui_textr(fb, buf, 122, ry + 2, mkt_sell_col(sell, base, nc));
        } else {
            eui_textr(fb, "--", 122, ry + 2, RGB565C(60, 66, 84));
        }
    }
    /* Scrollbar at the right edge spans the list window. */
    eui_scrollbar(fb, 125, y0, vis * row_h, N_GOODS, vis, s_scroll, COL_CUR, COL_GRID);

    /* Detail strip for the selection — STOCK and HELD in the readable font, with
       the ship's total hold (used/size) as small text at the row end. */
    hl(fb, 101, COL_GRID);
    {
        int i = s_cursor;
        int stock = econ_stock(si, s_station, i) - s_bought[i]; if (stock < 0) stock = 0;
        snprintf(buf, sizeof buf, "STOCK %d", stock);
        eui_text(fb, buf, 4, 104, COL_TXT);
        snprintf(buf, sizeof buf, "HELD %d", g_player.cargo[i]);
        eui_text(fb, buf, 58, 104, g_player.cargo[i] ? COL_CRED : COL_TXT);
        snprintf(buf, sizeof buf, "%d/%d", player_cargo_total(), player_cargo_cap());
        craft_font_draw(fb, buf, 126 - craft_font_width(buf), 107, COL_DIM);
    }

    /* Footer: prompts (position is shown by the scrollbar). */
    { char h[20]; snprintf(h, sizeof h, "%s:TRADE  %s:BACK",
        plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 2, 118, COL_DIM); }

    if (s_mkt_open) {
        static const char *const it[4] = { "BUY", "BUY MAX", "SELL", "SELL ALL" };
        draw_action_box(fb, k_goods[s_cursor].name, it, 4, s_mkt_cur);
    }
}

static const char *k_qtag[5] = { "SLV", "STD", "RNF", "MIL", "PRO" };

static void draw_shipyard(uint16_t *fb) {
    draw_header(fb);
    /* Ship offers in the readable font (short hull names); the price + stat
       strip below the rule stays compact. */
    int lh = eui_lineh();
    int y = 16;
    for (int i = 0; i < YARD_OFFERS; i++, y += lh) {
        uint16_t c = (i == s_cursor) ? COL_CUR : COL_DIM;   /* colour marks selection */
        eui_text(fb, s_yard[i].name, 3, y, c);
        if (s_yard[i].bargain)                       /* special offer */
            eui_text(fb, "*", 5 + eui_textw(s_yard[i].name), y, RGB565C(255, 210, 70));
    }
    {   /* YOUR ship: compare row, no purchase — blue IS the label. */
        uint16_t c = (s_cursor == YARD_OFFERS) ? RGB565C(120, 210, 235)
                                               : RGB565C(80, 150, 175);
        eui_text(fb, k_hulls[g_player.hull_id].name, 3, y, c);
    }
    /* Selected offer: price + stat strip in the full-width footer. */
    const HullDef *sel = (s_cursor == YARD_OFFERS)
                             ? &k_hulls[g_player.hull_id]
                             : &k_hulls[s_yard[s_cursor].cls];
    HullRoll selr;
    hull_roll((s_cursor == YARD_OFFERS) ? g_player.hull_id
                                        : s_yard[s_cursor].cls,
              (s_cursor == YARD_OFFERS) ? g_player.hull_seed
                                        : s_yard[s_cursor].seed,
              &selr);
    hl(fb, 95, COL_GRID);
    char buf[36];
    if (s_cursor == YARD_OFFERS) {
        snprintf(buf, sizeof buf, "%s  YOUR SHIP",
                 k_hulls[g_player.hull_id].name);
    } else {
        int econ = system_info()->stations[s_station].econ;
        int tradein = player_tradein(econ);
        int cost = s_yard[s_cursor].price - tradein;
        const char *lab = (cost < 0) ? "GET"
                        : s_yard[s_cursor].bargain ? "OFFER" : "COST";
        snprintf(buf, sizeof buf, "%s %s %d CR", s_yard[s_cursor].name,
                 lab, cost < 0 ? -cost : cost);
    }
    craft_font_draw(fb, buf, 2, 98, COL_CRED);
    /* Label/value colour pairs: "SPD85 CRG6 H70 S50 SL1" */
    {
        char slots[8];
        int sl = 0;
        for (int i = 0; i < selr.n_slots && sl < 7; i++)
            slots[sl++] = (char)('0' + selr.slot_size[i]);
        slots[sl] = 0;
        char vals[5][8];
        snprintf(vals[0], 8, "%d", (int)(sel->max_speed * selr.spd));
        snprintf(vals[1], 8, "%d", selr.cargo);
        snprintf(vals[2], 8, "%d", (int)(sel->hull_base * selr.hull));
        snprintf(vals[3], 8, "%d", (int)(sel->shield_base * selr.shd));
        snprintf(vals[4], 8, "%s", slots);
        static const char *labs[5] = { "SPD", "CRG", "H", "S", "SL" };
        int x = 2;
        for (int i = 0; i < 5; i++) {
            x = craft_font_draw(fb, labs[i], x, 105, RGB565C(80, 95, 120));
            x = craft_font_draw(fb, vals[i], x, 105, RGB565C(140, 255, 140));
            x += 3;
        }
    }
    hl(fb, 113, COL_GRID);
    { char h[44];
      if (s_detail) snprintf(h, sizeof h, "%s:BUY %s:KIT </>:CMP %s:BACK",
          plat_menu_btn(MB_A), plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
      else snprintf(h, sizeof h, "%s:BUY %s:DETAIL %s:BACK",
          plat_menu_btn(MB_A), plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 2, 117, COL_DIM); }
    if (s_yard_confirm) {
        int idx = s_yard_confirm - 1;
        int econ = system_info()->stations[s_station].econ;
        int tradein = player_tradein(econ);
        int cost = s_yard[idx].price - tradein;
        char t[40], c[28];
        snprintf(t, sizeof t, "BUY %s%s", s_yard[idx].name,
                 s_yard[idx].bargain ? " *" : "");
        if (cost < 0)
            snprintf(c, sizeof c, "GET %d CR  A:YES B:NO", -cost);
        else
            snprintf(c, sizeof c, "%d CR    A:YES  B:NO", cost);
        const char *const it[1] = { c };
        draw_action_box(fb, t, it, 1, -1);
    }
}

static void draw_outfit(uint16_t *fb) {
    draw_header(fb);
    /* Readable section label; the header's divider (y13) already separates it. */
    eui_text(fb, "OUTFITTING", 2, 14, COL_DIM);
    hl(fb, 27, COL_GRID);
    const HullDef *h = &k_hulls[g_player.hull_id]; (void)h;
    outfit_build_rows();
    if (s_cursor >= s_n_rows) s_cursor = s_n_rows - 1;
    /* Readable rows (full spec of the selection is one detail-sheet press away, so
       list text may clip and the mount/equip quality tag is dropped here). */
    int y0 = 29, rh = 13;
    int vis = (112 - y0) / rh; if (vis < 1) vis = 1;   /* ~6 rows */
    if (s_cursor < s_scroll)        s_scroll = s_cursor;
    if (s_cursor >= s_scroll + vis) s_scroll = s_cursor - vis + 1;
    if (s_n_rows > vis && s_scroll > s_n_rows - vis) s_scroll = s_n_rows - vis;
    if (s_scroll < 0) s_scroll = 0;
    int y = y0;
    char buf[36];
    for (int i = s_scroll; i < s_n_rows && i < s_scroll + vis; i++, y += rh) {
        const OutfitRow *r = &s_rows[i];
        uint16_t c = (i == s_cursor) ? COL_CUR : COL_DIM;
        int ty = y + 1;                              /* text baseline */
        if (i == s_cursor) eui_text(fb, ">", 0, ty, COL_CUR);
        switch (r->kind) {
        case ROW_MOUNT: {
            const WeaponInst *m = &g_player.mounts[r->index];
            if (m->in_use) {
                icon_weapon(fb, 6, y + 3, m->type);
                /* quality tag dropped from list row — shown on the detail sheet */
                snprintf(buf, sizeof buf, "Z%d %s%s%s %d%%",
                         player_slot_size(r->index),
                         k_weapons[m->type].name,
                         m->affix ? "-" : "",
                         m->affix ? k_affixes[m->affix].tag : "",
                         m->integrity);
                int cost = m->integrity < 100;
                eui_textclip(fb, buf, 20, cost ? 100 : 124, ty, c);
                if (cost) {
                    snprintf(buf, sizeof buf, "%d", repair_cost(m));
                    eui_textr(fb, buf, 122, ty, COL_CRED);
                }
            } else {
                snprintf(buf, sizeof buf, "Z%d EMPTY",
                         player_slot_size(r->index));
                eui_textclip(fb, buf, 6, 124, ty, c);
            }
            break;
        }
        case ROW_EQUIP: {
            const WeaponInst *e = r->index ? &g_player.armor_eq
                                           : &g_player.shield_eq;
            icon_weapon(fb, 6, y + 3, WPN_COUNT + r->index);
            if (e->in_use) {
                const char *vn = (e->type == EQ_ARMOR)
                                     ? k_armor_var_names[e->affix & 3]
                                     : k_shield_var_names[e->affix & 3];
                snprintf(buf, sizeof buf, "%s%s%s Z%d %d%%", vn,
                         e->affix ? " " : "", item_name(e->type),
                         e->tier, e->integrity);
                int cost = e->integrity < 100;
                eui_textclip(fb, buf, 20, cost ? 100 : 124, ty, c);
                if (cost) {
                    int cst = (int)((100 - e->integrity) *
                                     equip_price(e->type, e->tier,
                                                 e->quality) / 100 * 0.6f *
                                     skill_repair_mult()) + 1;
                    snprintf(buf, sizeof buf, "%d", cst);
                    eui_textr(fb, buf, 122, ty, COL_CRED);
                }
            } else {
                snprintf(buf, sizeof buf, "%s ----",
                         item_name(WPN_COUNT + r->index));
                eui_textclip(fb, buf, 20, 124, ty, c);
            }
            break;
        }
        case ROW_TURRET: {
            const WeaponInst *t2 = &g_player.turret_eq;
            if (t2->in_use) {
                icon_weapon(fb, 6, y + 3, t2->type);
                {
                    static const char *k_cal[4] = { "STANDARD",
                        "REINFORCED", "MILITARY", "PROTOTYPE" };
                    snprintf(buf, sizeof buf, "TURRET %s %s",
                             k_weapons[t2->type].name,
                             k_cal[player_turret_gunner_tier()]);
                }
                eui_textclip(fb, buf, 20, 124, ty, c);
            } else {
                snprintf(buf, sizeof buf, "TURRET ---- (Z1)");
                eui_textclip(fb, buf, 6, 124, ty, c);
            }
            break;
        }
        case ROW_UTIL: {
            const WeaponInst *e = &g_player.util_eq[r->index];
            if (e->in_use) {
                icon_weapon(fb, 6, y + 3, e->type);
                snprintf(buf, sizeof buf, "%s %d%%",
                         item_name(e->type), e->integrity);
                eui_textclip(fb, buf, 20, 124, ty, c);
            } else {
                snprintf(buf, sizeof buf, "UTIL BAY %d ----", r->index + 1);
                eui_textclip(fb, buf, 6, 124, ty, c);
            }
            break;
        }
        case ROW_UTILSHOP: {
            int ity = EQ_HEATSINK + r->index;
            icon_weapon(fb, 6, y + 3, ity);
            snprintf(buf, sizeof buf, "BUY %s", item_name(ity));
            eui_textclip(fb, buf, 20, 100, ty, c);
            {
                const SystemInfo *sie = system_info();
                snprintf(buf, sizeof buf, "%d",
                         (int)(k_equip[ity - WPN_COUNT].base_price *
                               econ_weapon_mult(
                                   sie->stations[s_station].econ) *
                               skill_price_mult()));
            }
            eui_textr(fb, buf, 122, ty, COL_CRED);
            break;
        }
        case ROW_EQSHOP: {
            icon_weapon(fb, 6, y + 3, WPN_COUNT + r->index);
            {
                const SystemInfo *sie = system_info();
                uint32_t vh = (uint32_t)(sie->seed >> 22) ^
                              (uint32_t)((s_station + 1) * 7129u) ^
                              (uint32_t)(r->index * 31u + r->tier);
                vh ^= vh >> 13; vh *= 1274126177u; vh ^= vh >> 16;
                uint8_t variant = (uint8_t)(vh % 4u);
                const char *vn = r->index ? k_armor_var_names[variant]
                                          : k_shield_var_names[variant];
                snprintf(buf, sizeof buf, "BUY %s%s%s Z%d", vn,
                         variant ? " " : "",
                         item_name(WPN_COUNT + r->index), r->tier);
            }
            eui_textclip(fb, buf, 20, 100, ty, c);
            {
                const SystemInfo *sie = system_info();
                snprintf(buf, sizeof buf, "%d",
                         (int)(equip_price(WPN_COUNT + r->index, r->tier,
                                           Q_STANDARD) *
                               econ_weapon_mult(
                                   sie->stations[s_station].econ) *
                               skill_price_mult()));
            }
            eui_textr(fb, buf, 122, ty, COL_CRED);
            break;
        }
        case ROW_HDR: {
            static const char *k_hdr[3] = { "-YOUR SHIP-", "-YOUR HOLD-",
                                            "-STATION SHOP-" };
            eui_text(fb, k_hdr[r->index], 4, ty, RGB565C(90, 140, 190));
            break;
        }
        case ROW_SALV: {
            const WeaponInst *m = &g_player.salvage[r->index];
            icon_weapon(fb, 6, y + 3, m->type);
            snprintf(buf, sizeof buf, "%s%s%s %s %d%%",
                     item_name(m->type),
                     m->affix ? "-" : "",
                     m->affix ? k_affixes[m->affix].tag : "",
                     k_qtag[m->quality], m->integrity);
            eui_textclip(fb, buf, 20, 100, ty, c);
            /* What the shop pays (B sells). */
            snprintf(buf, sizeof buf, "+%d",
                     (int)(instance_price(m) *
                           (0.35f + 0.30f * m->integrity * 0.01f)));
            eui_textr(fb, buf, 122, ty, RGB565C(120, 200, 120));
            break;
        }
        case ROW_SHOP: {
            const ArmoryItem *it = &s_armory[r->index];
            icon_weapon(fb, 6, y + 3, it->type);
            if (it->featured) {
                /* Featured rare: starred, quality-tagged, gold name. */
                snprintf(buf, sizeof buf, "*%s %s%s%s",
                         k_qtag[it->quality], k_weapons[it->type].name,
                         it->affix ? "-" : "",
                         it->affix ? k_affixes[it->affix].tag : "");
                eui_textclip(fb, buf, 20, 100, ty,
                             (i == s_cursor) ? COL_CUR
                                             : RGB565C(255, 200, 90));
            } else {
                snprintf(buf, sizeof buf, "BUY %s Z%d",
                         k_weapons[it->type].name, k_weapons[it->type].size);
                eui_textclip(fb, buf, 20, 100, ty, c);
            }
            snprintf(buf, sizeof buf, "%d",
                     (int)(it->price * skill_price_mult()));
            eui_textr(fb, buf, 122, ty, COL_CRED);
            break;
        }
        }
    }
    eui_scrollbar(fb, 125, y0, vis * rh, s_n_rows, vis, s_scroll, COL_CUR, COL_GRID);
    hl(fb, 113, COL_GRID);
    /* action popup */
    if (s_pop_open) {
        int ph = 14 + s_pop_n * 9;
        int py0 = 56 - ph / 2;
        for (int y = py0; y < py0 + ph; y++)
            for (int x = 34; x < 94; x++)
                fb[y * ELITE_FB_W + x] = RGB565C(8, 11, 20);
        for (int x = 34; x < 94; x++) {
            fb[py0 * ELITE_FB_W + x] = RGB565C(70, 86, 115);
            fb[(py0 + ph - 1) * ELITE_FB_W + x] = RGB565C(70, 86, 115);
        }
        for (int y = py0; y < py0 + ph; y++) {
            fb[y * ELITE_FB_W + 34] = RGB565C(70, 86, 115);
            fb[y * ELITE_FB_W + 93] = RGB565C(70, 86, 115);
        }
        for (int i = 0; i < s_pop_n; i++) {
            uint16_t c = (i == s_pop_cur) ? COL_CUR : COL_DIM;
            if (i == s_pop_cur)
                craft_font_draw(fb, ">", 39, py0 + 7 + i * 9, c);
            craft_font_draw(fb, pact_name(s_pop_acts[i]), 46,
                            py0 + 7 + i * 9, c);
        }
    }
    /* swap counterpart picker */
    if (s_pick_open) {
        const OutfitRow *r = &s_rows[s_pop_row];
        int ph = 22 + s_pick_n * 9;
        int py0 = 56 - ph / 2;
        for (int y = py0; y < py0 + ph; y++)
            for (int x = 16; x < 112; x++)
                fb[y * ELITE_FB_W + x] = RGB565C(8, 11, 20);
        for (int x = 16; x < 112; x++) {
            fb[py0 * ELITE_FB_W + x] = RGB565C(245, 200, 80);
            fb[(py0 + ph - 1) * ELITE_FB_W + x] = RGB565C(245, 200, 80);
        }
        craft_font_draw(fb, "SWAP WITH:", 22, py0 + 4,
                        RGB565C(245, 200, 80));
        for (int i = 0; i < s_pick_n; i++) {
            const WeaponInst *o =
                (r->kind == ROW_MOUNT)
                    ? &g_player.salvage[s_pick_items[i]]
                    : &g_player.mounts[s_pick_items[i]];
            uint16_t c = (i == s_pick_cur) ? COL_CUR : COL_DIM;
            char nb[22];
            snprintf(nb, sizeof nb, "%s %s",
                     k_weapons[o->type].name, k_qtag[o->quality]);
            if (i == s_pick_cur)
                craft_font_draw(fb, ">", 20, py0 + 14 + i * 9, c);
            craft_font_draw(fb, nb, 27, py0 + 14 + i * 9, c);
        }
    }
    { char h[40];
      if (s_pop_open || s_pick_open)
          snprintf(h, sizeof h, "%s:SELECT  %s:BACK",
                   plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      else
          snprintf(h, sizeof h, "%s:ACTIONS  %s:DETAIL  %s:BACK",
                   plat_menu_btn(MB_A), plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 2, 116, COL_DIM); }
}

static void draw_missions(uint16_t *fb) {
    draw_header(fb);
    char buf[34];
    Faction fac = system_faction(system_info()->addr);

    /* Readable section headers + faction standing; the mission rows themselves
       stay compact so the ">"-notation label AND reward stay fully visible. */
    int y = 15;
    eui_text(fb, "LOG", 2, y, COL_HDR);
    snprintf(buf, sizeof buf, "%s %d", k_faction_names[fac], g_rep[fac]);
    eui_textr(fb, buf, 126, y, COL_DIM);
    y += 13;
    int any = 0;
    /* LOG rows stay compact: the OFFERS list below is the interactive part and
       gets the readable font; four readable LOG rows + four readable OFFERS +
       both headers cannot share this screen without overrunning the footer. */
    for (int i = 0; i < MAX_MISSIONS; i++) {
        const Mission *m = &g_missions[i];
        if (m->type == MIS_NONE) continue;
        any = 1;
        uint16_t c = m->done ? COL_CRED : COL_DIM;
        snprintf(buf, sizeof buf, "%s%s", m->label, m->done ? " *DONE" : "");
        craft_font_draw(fb, buf, 8, y, c);
        y += 9;
    }
    if (!any) { craft_font_draw(fb, "(NONE ACTIVE)", 8, y, COL_DIM); y += 9; }

    y += 3;
    eui_text(fb, "OFFERS", 2, y, COL_HDR);
    y += 13;
    for (int i = 0; i < MISSION_OFFERS; i++, y += 11) {
        const Mission *m = &s_offers[i];
        uint16_t c = (i == s_cursor) ? COL_CUR : COL_DIM;
        if (i == s_cursor) eui_text(fb, ">", 0, y, COL_CUR);
        if (m->type == MIS_NONE) {
            eui_text(fb, "----", 8, y, c);
        } else {
            snprintf(buf, sizeof buf, "%d", m->reward);
            int rw = eui_textw(buf);
            eui_textclip(fb, m->label, 8, 122 - rw - 4, y, c);
            eui_textr(fb, buf, 122, y, COL_CRED);
        }
    }
    hl(fb, 113, COL_GRID);
    { char h[28]; snprintf(h, sizeof h, "%s:ACCEPT %s:BACK",
        plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      eui_text(fb, h, 2, 115, COL_DIM); }
}

static void draw_bar(uint16_t *fb) {
    draw_header(fb);
    const SystemInfo *si = system_info();
    char buf[64];
    int lh = eui_lineh();
    /* Prose region (event + rumour + tip) fills the top in the readable font;
       the black-market note and faction rep table stay compact, anchored below. */
    const int PROSE_MAX = 80;
    int y = 16;
    if (s_bar_ev) {                 /* someone here wants a word */
        eui_text(fb, ">", 2, y, COL_TXT);
        eui_textclip(fb, s_bar_ev->title, 11, 126, y, COL_TXT);
        y += lh + 2;
    }
    /* Rumours: seeded flavour + a genuine trade tip. */
    uint32_t h = (uint32_t)(si->seed >> 20) ^ (uint32_t)(s_station * 131);
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    static const char *k_chatter[6] = {
        "\"PIRATES THICK OUT BY",
        "\"DOMINION PATROLS ARE",
        "\"SAW A DERELICT NEAR",
        "\"FUEL PRICES CLIMBING",
        "\"LOST A WING AT THE",
        "\"KEEP YOUR SCANNER ON",
    };
    snprintf(buf, sizeof buf, "%s THE BEACON...\"", k_chatter[h % 6u]);
    y = eui_wrap(fb, buf, 2, 126, y, PROSE_MAX, COL_DIM) + 3;
    /* Trade tip: what this economy exports cheap. */
    int best = 0;
    int best_price = 999999;
    for (int g = 0; g < 16; g++) {
        int p = econ_price(si, s_station, g, true);
        if (p > 0 && p * 100 / (k_goods[g].base + 1) < best_price) {
            best_price = p * 100 / (k_goods[g].base + 1);
            best = g;
        }
    }
    snprintf(buf, sizeof buf, "TIP: %s IS CHEAP HERE", k_goods[best].name);
    eui_wrap(fb, buf, 2, 126, y, PROSE_MAX, COL_TXT);
    /* Compact status block, anchored above the footer. */
    craft_font_draw(fb, econ_has_black_market(si) ? "BLACK MARKET: SEE MARKET"
                                                  : "LAWFUL - NO BLACK MARKET",
                    2, 82, econ_has_black_market(si) ? COL_ILL : COL_DIM);
    int ry = 90;
    for (int f = 0; f < N_FACTIONS; f++) {
        snprintf(buf, sizeof buf, "%-10s REP %d", k_faction_names[f], g_rep[f]);
        craft_font_draw(fb, buf, 2, ry, COL_DIM);
        ry += 8;
    }
    hl(fb, 113, COL_GRID);
    { char hh[32];
      if (s_bar_ev) snprintf(hh, sizeof hh, "%s:APPROACH %s:BACK",
                             plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      else snprintf(hh, sizeof hh, "%s:BACK", plat_menu_btn(MB_B));
      eui_text(fb, hh, 2, 115, COL_DIM); }
}

/* DATABASE: re-read unlocked lore fragments (events OP_LORE bits). */
static void draw_codex(uint16_t *fb) {
    int seen = 0;
    for (int i = 0; i < k_n_lore; i++)
        if (events_lore_seen(i)) seen++;

    if (s_detail && events_lore_seen(s_cursor)) {   /* article: title owns the header row */
        eui_text(fb, k_lore[s_cursor].title, 2, 1, COL_HDR);
        hl(fb, 13, COL_GRID);
        eui_wrap(fb, k_lore[s_cursor].body, 2, 126, 16, 115, COL_TXT);
        hl(fb, 116, COL_GRID);
        { char h[16]; snprintf(h, sizeof h, "%s:BACK", plat_menu_btn(MB_B));
          eui_text(fb, h, 2, 118, COL_DIM); }
        return;
    }

    draw_header(fb);
    /* Header shows the station + credits; the archive count rides the footer.
       Readable, height-filling scroll window (was a fixed 10-row bitmap list). */
    int y0 = 15, lh;
    int rows = eui_fit(116 - y0, k_n_lore, &lh);
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + rows) s_scroll = s_cursor - rows + 1;
    if (k_n_lore > rows && s_scroll > k_n_lore - rows) s_scroll = k_n_lore - rows;
    if (s_scroll < 0) s_scroll = 0;
    for (int r = 0; r < rows && s_scroll + r < k_n_lore; r++) {
        int i = s_scroll + r, y = y0 + r * lh;
        bool unlocked = events_lore_seen(i);
        uint16_t c = unlocked ? (i == s_cursor ? COL_TXT : COL_HDR) : COL_GRID;
        if (i == s_cursor) eui_text(fb, ">", 2, y, unlocked ? COL_TXT : COL_DIM);
        eui_text(fb, unlocked ? k_lore[i].title : "- ENCRYPTED -", 11, y, c);
    }
    eui_scrollbar(fb, 125, y0, 116 - y0, k_n_lore, rows, s_scroll, COL_TXT, COL_GRID);
    hl(fb, 116, COL_GRID);
    { char h[36]; snprintf(h, sizeof h, "%d/%d  %s:READ %s:BACK",
        seen, k_n_lore, plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      eui_text(fb, h, 2, 118, COL_DIM); }
}

void station_draw(uint16_t *fb) {
    /* Status renders over the live hangar-bay scene: route it BEFORE
     * the fill chain below (which was wiping the 3D backdrop). */
    if (s_screen == SCR_STATUS) {
        status_draw(fb);
        return;
    }

    /* Detail sheets replace the list view. */
    if (s_detail && s_screen == SCR_SHIPYARD) {
        int yours = (s_cursor == YARD_OFFERS);
        int cls = yours ? g_player.hull_id : s_yard[s_cursor].cls;
        uint32_t sd = yours ? g_player.hull_seed : s_yard[s_cursor].seed;
        char f[44];
        if (s_kit_view) {
            snprintf(f, sizeof f, "%s:STATS </>:CMP %s:BACK",
                     plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
            detail_draw_kit(fb, cls, sd, f);
            return;
        }
        if (yours) {
            snprintf(f, sizeof f, "</>:CMP %s:KIT %s:BACK",
                     plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
            detail_draw_hull(fb, cls, sd, DETAIL_OWNED, f);
            return;
        }
        int econ = system_info()->stations[s_station].econ;
        int tradein = player_tradein(econ);
        int cost = s_yard[s_cursor].price - tradein;   /* <0 = trade-down refund */
        snprintf(f, sizeof f, "%s:BUY %s:KIT </>:CMP %s:BACK",
                 plat_menu_btn(MB_A), plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
        detail_draw_hull(fb, cls, sd, cost, f);
        return;
    }
    if (s_detail && s_screen == SCR_OUTFIT) {
        outfit_build_rows();
        const OutfitRow *r = &s_rows[s_cursor];
        WeaponInst tmp;
        const WeaponInst *wi = NULL;
        const WeaponInst *cmp = NULL;
        int price = -1;
        const char *plabel = "COST";
        const char *av = "ACT";
        if (r->kind == ROW_TURRET && g_player.turret_eq.in_use) {
            wi = &g_player.turret_eq;
            av = 0;
        } else if (r->kind == ROW_UTIL) {
            const WeaponInst *e = &g_player.util_eq[r->index];
            if (e->in_use) {
                wi = e;
                if (e->integrity < 100) {
                    price = (int)((100 - e->integrity) *
                                  instance_price(e) / 100 * 0.6f *
                                  skill_repair_mult()) + 1;
                    plabel = "REPAIR";
                    av = "RPR";
                } else av = 0;
            }
        } else if (r->kind == ROW_UTILSHOP) {
            const SystemInfo *sie = system_info();
            tmp = (WeaponInst){ .type = (uint8_t)(EQ_HEATSINK + r->index),
                                .quality = Q_STANDARD, .integrity = 100,
                                .in_use = 1 };
            wi = &tmp;
            price = (int)(k_equip[EQ_HEATSINK + r->index - WPN_COUNT]
                              .base_price *
                          econ_weapon_mult(sie->stations[s_station].econ) *
                          skill_price_mult());
            av = "BUY";
        } else if (r->kind == ROW_EQUIP) {
            const WeaponInst *e = equip_slot(r->index);
            if (e->in_use) {
                wi = e;
                if (e->integrity < 100) {
                    price = (int)((100 - e->integrity) *
                                  equip_price(e->type, e->tier, e->quality) /
                                  100 * 0.6f * skill_repair_mult()) + 1;
                    plabel = "REPAIR";
                    av = "RPR";
                } else av = 0;
            }
        } else if (r->kind == ROW_EQSHOP) {
            const SystemInfo *sie = system_info();
            tmp = (WeaponInst){ .type = (uint8_t)(WPN_COUNT + r->index),
                                .quality = Q_STANDARD, .integrity = 100,
                                .in_use = 1, .tier = r->tier };
            wi = &tmp;
            price = (int)(equip_price(WPN_COUNT + r->index, r->tier,
                                      Q_STANDARD) *
                          econ_weapon_mult(sie->stations[s_station].econ) *
                          skill_price_mult());
            av = "BUY";
        } else if (r->kind == ROW_MOUNT && g_player.mounts[r->index].in_use) {
            wi = &g_player.mounts[r->index];
            if (wi->integrity < 100) {
                price = repair_cost(wi);
                plabel = "REPAIR";
                av = "RPR";
            } else av = 0;
        } else if (r->kind == ROW_SALV) {
            wi = &g_player.salvage[r->index];
            price = (int)(weapon_price(wi->type, wi->quality) *
                          (0.35f + 0.30f * wi->integrity * 0.01f));
            plabel = "SELLS FOR";
            av = "FIT";
        } else if (r->kind == ROW_SHOP) {
            const ArmoryItem *it = &s_armory[r->index];
            tmp = (WeaponInst){ .type = it->type, .quality = it->quality,
                                .integrity = 100, .in_use = 1,
                                .affix = it->affix };
            wi = &tmp;
            price = (int)(it->price * skill_price_mult());
            av = "BUY";
        }
        if (wi) {
            /* Comparator (user spec): the fitted weapon of the SAME
             * type if you have one, else your most expensive fitted
             * weapon. Equipment compares to its fitted counterpart.
             * Viewing an already-fitted item itself: no diff. */
            if (wi->type >= WPN_COUNT) {
                const WeaponInst *e = equip_slot(wi->type - WPN_COUNT);
                if (e->in_use && e != wi) cmp = e;
            } else if (r->kind != ROW_MOUNT) {
                int best_price = -1;
                for (int m = 0; m < player_n_slots(); m++) {
                    const WeaponInst *mw = &g_player.mounts[m];
                    if (!mw->in_use || mw->type >= WPN_COUNT) continue;
                    if (mw->type == wi->type) { cmp = mw; best_price = -2; break; }
                    int pr = weapon_price(mw->type, mw->quality);
                    if (best_price >= -1 && pr > best_price) {
                        best_price = pr;
                        cmp = mw;
                    }
                }
            }
            char foot[44];
            if (av) snprintf(foot, sizeof foot, "</>:CMP %s:%s %s:BACK",
                             plat_menu_btn(MB_A), av, plat_menu_btn(MB_B));
            else snprintf(foot, sizeof foot, "</>:CMP %s:BACK", plat_menu_btn(MB_B));
            detail_draw_weapon(fb, wi, cmp, price, plabel, foot);
            return;
        }
        s_detail = 0;
    }

    if (s_screen == SCR_HOME) fill_with_pane(fb, COL_BG, 10, 119);
    else if (s_screen == SCR_SHIPYARD) fill_with_pane(fb, COL_BG, 10, 95);
    else fill(fb, COL_BG);
    if (s_screen == SCR_MARKET) draw_market(fb);
    else if (s_screen == SCR_SHIPYARD) draw_shipyard(fb);
    else if (s_screen == SCR_OUTFIT) draw_outfit(fb);
    else if (s_screen == SCR_MISSIONS) draw_missions(fb);
    else if (s_screen == SCR_BAR) draw_bar(fb);
    else if (s_screen == SCR_CODEX) draw_codex(fb);
    else if (s_screen == SCR_STATUS) { status_draw(fb); return; }
    else draw_home(fb);

    if (s_toast_t > 0) {
        int w = craft_font_width(s_toast) + 8;
        int x0 = 64 - w / 2;
        for (int y = 58; y < 70; y++)
            for (int x = x0; x < x0 + w; x++)
                if ((unsigned)x < ELITE_FB_W)
                    fb[y * ELITE_FB_W + x] = RGB565C(30, 20, 12);
        craft_font_draw(fb, s_toast, x0 + 4, 61, COL_WARN);
    }
}
