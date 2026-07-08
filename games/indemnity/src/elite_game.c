/*
 * ThumbyElite — top-level game module.
 *
 * Phase 4: the galaxy. State machine over flight / supercruise /
 * hyperspace / maps / pause. TWO COORDINATE SCALES:
 *
 *   system space  — megameters (Mm), f32: planet/station/beacon layout
 *   local space   — meters relative to s_anchor_mm: ships, combat, FX
 *
 * Every supercruise drop RE-ANCHORS the local frame, so combat floats
 * stay tiny no matter where in the system you are. Planet impostors are
 * fed the camera's absolute Mm position and handle their own scaling.
 */
#include "elite_engine.h"
#include "r3d_pipe.h"
#include "elite_game.h"
#include "elite_ui.h"      /* readable Audiowide menu text (title/menus) */
#include "elite_types.h"
#include "r3d_scene.h"
#include "r3d_planet.h"
#include "r3d_fx.h"
#include "elite_entity.h"
#include "elite_input.h"
#include "elite_flight.h"
#include "elite_combat.h"
#include "elite_proj.h"
#include "elite_loot.h"
#include "elite_rocks.h"
#include "elite_collide.h"
#include "mission.h"
#include "elite_ai.h"
#include "ui_hud.h"
#include "ui_map.h"
#include "ui_station.h"
#include "ui_status.h"
#include "ui_ctrlsetup.h"
#include "events.h"
#include "ui_event.h"
#include "elite_player.h"
#include "elite_audio.h"
#include "elite_save.h"
#include "elite_platform.h"
#include "system_sim.h"
#include "station_gen.h"
#include "elite_ships.h"
#include "econ.h"
#include "galaxy_gen.h"
#include "craft_font.h"
#include "meshes_gen.h"
#include "elite_pvp.h"   /* PVP: 1v1 LINK ARENA */
#include <stdio.h>
#include <string.h>

typedef enum {
    ST_FLIGHT = 0, ST_SUPERCRUISE, ST_HYPERJUMP,
    ST_GALAXY_MAP, ST_SYSTEM_MAP, ST_PAUSE,
    ST_DOCKING, ST_DOCKED, ST_STATUS, ST_TITLE,
    ST_DASH = 12,   /* appended LAST — inserting mid-enum shifted
                       DOCKED & friends and broke every state check */
    ST_CTRLSETUP = 13,   /* controller binding screen (from SETTINGS) */
    ST_EVENT = 14,       /* dock-arrival hail modal (events.c)        */
    ST_SAVESEL = 15,     /* CONTINUE: scrollable multi-save selector  */
    ST_PVPWAIT = 16,     /* PVP: LINK ARENA link-wait / handshake     */
} GState;

#define DOCK_RANGE 600.0f

#define JUMP_RANGE 7.5f
#define HYPER_TIME 2.6f
#define SC_DROP_MM 1.2f          /* auto-drop distance to destination */

static GState  s_state;
/* Where ST_EVENT hands control back when the modal closes. */
enum { EVRET_DOCK_FINISH = 0,   /* arrival hail: finish docking after  */
       EVRET_DOCKED,            /* bar encounter: back to the station  */
       EVRET_FLIGHT };          /* in-space (derelict/arrival hail)    */
static uint8_t s_event_return;
static SysAddr s_addr;           /* current system */
static Vec3    s_anchor_mm;      /* local-frame origin in system space */
static Poi     s_anchor_poi;     /* what we're anchored at */
static bool    s_anchor_has_poi;

static Vec3    s_sc_pos_mm;      /* supercruise position */
static Poi     s_sc_dest;
static bool    s_sc_has_dest;
static float   s_sc_speed;
static float   s_sc_eta;

static SysAddr s_jump_target;
static float   s_jump_dist;
static float   s_hyper_t;
static uint32_t s_hyper_seed;
static Vec3    s_hyper_from_mm;   /* departure point: system recedes */

static int   s_target = -1;      /* combat lock */
static float s_cloak_t;          /* remaining cloak seconds */
static uint8_t s_cloak_used;     /* one charge per launch (user spec) */
static float s_scan_t;           /* manifest scan accumulator */
static int   s_scan_done = -1;   /* target already read */
static int   s_loot_target = -1; /* canister lock (no hostiles about) */
static int   s_rock_target = -1; /* prospector lock (belt finding aid) */
static int   s_prev_rock = -1;   /* last cycled rock (stepping state) */
static int   s_prev_loot = -1;
static struct {
    int valid, env;
    uint8_t tier, cls, nw, wpn[3], shv, arv, chaff, turret, police, wfac;
    float spd, trn, hull, shd;
} s_kr;                          /* the kill report (death screen) */
static bool  s_dead_latch;
static float s_respawn_t;
static uint32_t s_entry_salt;    /* per-system-entry, salts transient
                                    events (distress) so revisits differ */
static int   s_distress_civ = -1; /* live distress event: the victim */
static uint32_t s_distress_done;  /* per-POI resolved bits, this system
                                     (user bug: re-arriving respawned a
                                     paid rescue — farmable) */
static bool  s_distress_paid;
static int   s_derelict_idx = -1; /* live derelict hulk (boardable)   */
static uint32_t s_derelict_done;  /* per-POI boarded bits, this system */
static float s_war_wave_t;        /* reinforcement cadence            */
static bool  s_war_won_toast;     /* "zone secure" said once          */
static bool  s_recall_active;     /* an Underwriter recall wing is in play */
static bool  s_recall_pending;    /* armed arrival -> open the hail next tick */
static bool  s_recall_spawn;      /* hail shown -> spawn the wing on its close */
static bool  s_won_latch;         /* campaign-complete congrats shown once */
static int   s_tgt_class = 0;    /* 0 AUTO, 1 SALVAGE, 2 ROCKS,
                                    3 FRIENDLY — LB
                                    double-tap demotes the class so you
                                    can mine through floating salvage or
                                    loot mid-fight; single-tap still
                                    cycles WITHIN the class only */
static bool  s_station_lock;     /* station nav lock (nothing else) */
static float s_rail_charge01;    /* railgun charge for the HUD arc */
static bool  s_incoming;         /* seeker tracking the player */
static bool  s_in_settings;      /* SETTINGS submenu over the pause */
static bool  s_settings_eat_b;   /* swallow a held B returning from a subscreen */
/* Settings rows: device shows 4 (invert/fps/volume/bright); the PC and
 * Android shells define ELITE_ANALOG_SETTINGS to add gamepad + touch-stick
 * sensitivity sliders, plus a CONTROLLER row when a controller is present. */
/* Row 4 = COMBAT difficulty (all platforms); analog/platform rows shift
 * down by one from the old layout. */
#define ROW_DIFF 4
#ifdef ELITE_ANALOG_SETTINGS
#ifdef ELITE_INPUT_SELECT     /* PC: + FULLSCREEN (always) and INPUT (w/ a pad) */
#define SETTINGS_MAX 10       /* invert,fps,vol,bright,diff,gpad,stick,fullscreen,input,controller */
#define ROW_FULL  7
#define ROW_INPUT 8
#define ROW_CTRL  9
static int settings_rows(void) { return plat_ctrl_present() ? 10 : 8; }
#else                         /* Android: no HOTAS/keyboard/fullscreen rows */
#define SETTINGS_MAX 8        /* invert,fps,vol,bright,diff,gpad,stick,controller */
#define ROW_CTRL  7
static int settings_rows(void) { return plat_ctrl_present() ? 8 : 7; }
#endif
#else
#define SETTINGS_MAX 5        /* invert,fps,vol,bright,diff */
static int settings_rows(void) { return 5; }
#endif
static int   s_dash_sel;         /* dashboard region 0..3 */
static float s_dash_anim;        /* 0 closed .. 1 fully risen */
static bool  s_dash_closing;     /* sliding back down before resume */
static uint8_t s_dash_from;      /* state to resume (flight/SC) */
static bool  s_menus_live;       /* chart/map/status keep the sim running */
static int   s_settings_cursor;
static float s_fps;              /* smoothed, for the toggle readout */
static uint32_t s_boot_seed;
static int   s_title_cursor;
/* --- multi-save selector ------------------------------------------- */
#define SAVE_LIST_MAX 24
typedef struct { int slot; SavePeek pk; } SaveListEntry;
static SaveListEntry s_savelist[SAVE_LIST_MAX];
static int  s_savelist_n;
static int  s_sel_cursor, s_sel_scroll;
static bool s_pvp_select;      /* the save picker is choosing a ship for a PVP duel */
static bool s_sel_confirm_del;        /* "are you sure?" overlay */
static int  s_save_slot = -1;         /* active slot; -1 = unsaved new game */
static void save_rebuild_list(void) {
    s_savelist_n = 0;
    int mx = save_max_slots();
    if (mx > SAVE_LIST_MAX) mx = SAVE_LIST_MAX;
    for (int s = 0; s < mx; s++) {
        SavePeek pk;
        if (save_peek(s, &pk)) {
            s_savelist[s_savelist_n].slot = s;
            s_savelist[s_savelist_n].pk = pk;
            s_savelist_n++;
        }
    }
}
/* Dock save: a brand-new run (slot -1) claims the next free slot first. */
static void save_dock_write(void) {
    if (s_save_slot < 0) {
        int s = save_alloc_slot();
        s_save_slot = (s >= 0) ? s : 0;   /* full -> reuse slot 0 */
        save_set_slot(s_save_slot);
    }
    save_write(s_addr, s_anchor_poi.index, combat_kills());
}
/* Scenario cheat menu (RB x10 on the title screen). OFF for release.
 * To restore the picker, its test loadouts and scenarios, set this to 1:
 * everything below is gated on it; the rest of the code folds the cheat
 * paths away (and the optimiser drops the dead loadouts from flash). */
#ifndef ELITE_CHEATS
#define ELITE_CHEATS 0
#endif
#define N_SCENARIOS 6
#if ELITE_CHEATS
static int   s_cheat_taps;       /* RB x10 on the title -> cheat menu */
static bool  s_cheat_on;         /* cheat menu armed (scenario picker) */
static int   s_cheat_scenario;   /* 0 normal; 1.. = test scenario chosen */
static const char *const k_scenarios[N_SCENARIOS] = {
    "RICH START",       /* 1: fat wallet, otherwise normal */
    "ENCOUNTERS",       /* 2: hails fire every chance, near a dock */
    "WAR FRONT",        /* 3: high rep + combat ship, war contracts ready */
    "ACT1: UNDERWRITER",   /* 4: the Underwriter arc primed */
    "CLIMAX: RECALL",   /* 5: acts done, surrender + grey-ship recall armed */
    "ELITE SHIP",       /* 6: maxed combat hull to test the fight */
};
#else
enum { s_cheat_on = 0, s_cheat_scenario = 0 };  /* fold the cheat paths away */
#endif
static char  s_scoop_toast[28];
static float s_scoop_toast_t;
static float s_frame_ms;
static Vec3  s_title_ctr;        /* title brawl centre (local m) */
static Vec3  s_title_perp;       /* side axis separating the two factions */
static float s_intro_t;          /* lore-crawl scroll position (s) */
static bool  s_intro_active;     /* play the intro crawl before the menu */
static float intro_duration(void);
static int   s_pause_cursor;
static bool  s_prev_menu, s_prev_a;

static uint32_t s_rng;
static uint32_t xorshift32(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)(xorshift32() & 0xFFFF) * (1.0f / 65535.0f);
}

const char *elite_game_debug_toast(void) { return s_scoop_toast; }

/* The classic ladder: nine ranks earned with kills. */
const char *elite_rank_name(int kills) {
    static const struct { int k; const char *n; } R[9] = {
        { 0, "HARMLESS" }, { 5, "MOSTLY HARMLESS" }, { 12, "POOR" },
        { 25, "AVERAGE" }, { 50, "ABOVE AVERAGE" }, { 90, "COMPETENT" },
        { 150, "DANGEROUS" }, { 250, "DEADLY" }, { 400, "ELITE" },
    };
    const char *n = R[0].n;
    for (int i = 0; i < 9; i++)
        if (kills >= R[i].k) n = R[i].n;
    return n;
}

/* Wall-clock-ish for ambient animation (sum of frame steps). */
static float s_time;
float elite_game_time(void) { return s_time; }

int elite_game_state(void) { return (int)s_state; }
int elite_game_in_ctrlsetup(void) { return s_state == ST_CTRLSETUP; }
int elite_game_is_dead(void) { return s_dead_latch; }   /* death/insurance screen */

static void drop_anchor(Vec3 pos_mm, const Poi *poi);
static void spawn_poi_content(void);
static void arrive_in_system(SysAddr addr);

/* Debug: hard-jump to a system (harness only). */
void elite_game_debug_jump(SysAddr addr) {
    arrive_in_system(addr);
}

int elite_game_debug_target(void) { return s_target; }
bool elite_game_cloaked(void) { return s_cloak_t > 0.0f; }

/* Paying your fine calls off the law (user bug: police kept shooting
 * a CLEAN pilot — the flip to hostile outlived the record). */
void elite_game_police_stand_down(void) {
    for (int i = 1; i < MAX_SHIPS; i++) {
        Ship *sp = &g_ships[i];
        if (!sp->alive || !sp->is_police) continue;
        sp->team = TEAM_NEUTRAL;
        sp->ai_target = 0;
    }
}
int elite_game_debug_rock_target(void) { return s_rock_target; }
int elite_game_debug_ai_state(int idx) { return g_ships[idx].ai_state; }
int elite_game_debug_distress_civ(void) { return s_distress_civ; }
void elite_game_debug_set_distress_civ(int idx) {
    s_distress_civ = idx; s_distress_paid = false;
}

/* Debug: jump the anchor straight to POI n (harness only). */
void elite_game_debug_goto_poi(int n) {
    Poi pois[MAX_POIS];
    int np = system_pois(pois, MAX_POIS);
    if (n < 0 || n >= np) return;
    drop_anchor(pois[n].pos_mm, &pois[n]);
    g_ships[PLAYER].pos = v3(0, 0, -700.0f);
    spawn_poi_content();
}

void elite_game_crit_toast(const char *msg, bool mine) {
    if (s_state == ST_TITLE) return;        /* title brawl is silent in the HUD */
    snprintf(s_scoop_toast, sizeof s_scoop_toast, "%s", msg);
    s_scoop_toast_t = mine ? 2.6f : 1.8f;
    if (mine) sfx_lock_warn();
}

/* Debug: frame planet POI n from 2.6 radii out, sunward side, facing
 * it (planet-variety sheets). */
void elite_game_debug_view_planet(int n) {
    Poi pois[MAX_POIS];
    int np = system_pois(pois, MAX_POIS);
    if (n < 0 || n >= np || pois[n].kind != POI_PLANET) return;
    const SystemInfo *si = system_info();
    float r = si->planets[pois[n].index].radius_mm;
    Vec3 ppos = pois[n].pos_mm;
    float pd = v3_len(ppos);
    if (pd < 1.0f) return;
    float k = 1.0f - (r * 2.6f) / pd;     /* sunward of the planet */
    Vec3 anchor = v3_scale(ppos, k);
    drop_anchor(anchor, &pois[n]);
    g_ships[PLAYER].pos = v3(0, 0, 0);
    elite_game_debug_face_away_from_sun();   /* away from sun = at it */
    s_scoop_toast_t = 0;                     /* clean frame */
}

/* Crossfire forgiveness: while the distress wing is still shooting at
 * the victim, stray player hits on it don't flip it or flag you — it
 * knows who the real enemy is. Normal crime rules resume once the
 * wing is dead. */
bool elite_game_distress_protected(int idx) {
    return idx == s_distress_civ && s_distress_civ > 0 &&
           !s_distress_paid && ships_alive_hostile() > 0;
}

/* The player damaged a hostile: any distress wing drops the civilian
 * and turns on the player (user spec: they fight us once engaged). */
void elite_game_player_engaged(void) {
    if (s_distress_civ < 0) return;
    for (int i = 1; i < MAX_SHIPS; i++)
        if (g_ships[i].alive && g_ships[i].team == TEAM_HOSTILE)
            g_ships[i].ai_target = 0;
}

/* Debug (host harness): face the player directly away from the star
 * so staged screenshots aren't photobombed by the sun. */
void elite_game_debug_face_away_from_sun(void) {
    Ship *p = &g_ships[PLAYER];
    Vec3 cm = v3_add(s_anchor_mm, v3_scale(p->pos, 1.0e-6f));
    Vec3 fwd = v3_norm(cm);                  /* away from origin/star */
    Vec3 up = (fwd.y > -0.9f && fwd.y < 0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
    Vec3 right = v3_norm(v3_cross(up, fwd));
    p->basis.r[0] = right;
    p->basis.r[1] = v3_cross(fwd, right);
    p->basis.r[2] = fwd;
}

/* Debug/demo: force-spawn hostiles around the player (host harness). */
void elite_game_debug_spawn(int n) {
    for (int i = 0; i < n; i++) {
        float a = frand(0, 6.2831f);
        float r = frand(500, 800);
        Vec3 pos = v3(cosf(a) * r, frand(-150, 150), sinf(a) * r);
        uint32_t mseed = 0xDEB06u ^ (uint32_t)((i & 1) * 77u);
        int cls = 2 + (i % 3);
        int idx = ship_spawn(hull_mesh(mseed, cls), pos, TEAM_HOSTILE);
        if (idx > 0) ship_set_tier(idx, i % 5, cls);
    }
}

/* Camera position in system space (Mm) for planet projection. */
static Vec3 cam_pos_mm(void) {
    if (s_state == ST_SUPERCRUISE) return s_sc_pos_mm;
    return v3_add(s_anchor_mm, v3_scale(g_ships[PLAYER].pos, 1.0e-6f));
}

/* --- player ------------------------------------------------------------*/
static void spawn_player(void) {
    Ship *p = &g_ships[PLAYER];
    p->alive = true;
    p->pos = v3(0, 0, 0);
    p->basis = m3_identity();
    p->vel = v3(0, 0, 0);
    p->throttle = 0.3f;
    p->assist = true;
    p->boost_t = 0;
    p->heat = 0;
    p->fire_cool = 0;
    p->team = TEAM_PLAYER;
    player_apply_to_ship();      /* hull, tiers, mounts, skills */
    p->hull = p->hull_max;
    p->shield = p->shield_max;
}

/* --- POI content ---------------------------------------------------------
 * Spawned once per supercruise drop / jump arrival. Pirates scale with
 * system threat; high-security space is quiet. */
/* Per-POI intel — the system map's scan strip reads this, and
 * spawn_poi_content uses the SAME belt hash, so what the map promises
 * is what arrival delivers. Belts are permanent geography (no visit
 * salt); pirates/salvage are odds, not facts. */
void elite_game_poi_intel(const Poi *poi, PoiIntel *out) {
    const SystemInfo *si = system_info();
    /* Persistent belt: deterministic per system+POI. */
    int mining_sys = 0;
    for (int st2 = 0; st2 < si->n_stations; st2++)
        if (si->stations[st2].econ == ECON_EXTRACT ||
            si->stations[st2].econ == ECON_REFINE)
            mining_sys = 1;
    uint32_t h = (uint32_t)(si->seed >> 16) ^
                 (uint32_t)(poi->kind * 73u) ^
                 (uint32_t)(poi->index * 0x9E37u);
    h *= 2654435761u; h ^= h >> 13;
    int belt_pct = (poi->kind == POI_STATION) ? 10 : 25;
    if (mining_sys) belt_pct += 35;
    out->belt = (h % 100u) < (uint32_t)belt_pct;
    out->belt_seed = h;
    out->belt_rocks = mining_sys ? 5 + (int)((h >> 8) % 4u)
                                 : 3 + (int)((h >> 8) % 3u);
    /* Police: deterministic from government. */
    out->police = (poi->kind == POI_STATION && si->gov >= GOV_CONFED);
    /* Pirate odds: the live arrival formula, contraband included. */
    int illegal = 0;
    for (int g2 = 0; g2 < N_GOODS; g2++)
        if (k_goods[g2].flags & GOOD_ILLEGAL) illegal += g_player.cargo[g2];
    int chance = 0;
    if (si->threat >= 1) {
        chance = (poi->kind == POI_STATION) ? 25 : 55;
        if (illegal > 0) {
            chance += 15 + (illegal > 10 ? 20 : illegal * 2);
            if (chance > 92) chance = 92;
        }
    }
    out->pirate_pct = (uint8_t)chance;
    /* Salvage odds: the debris formula. */
    int dch = ((poi->kind == POI_STATION) ? 12 : 30) + (int)si->threat * 8;
    out->debris_pct = (uint8_t)(dch > 99 ? 99 : dch);
    /* Distress calls: transient (salted per system entry), planets and
     * beacons in dangerous space. Same map<->arrival contract as
     * belts: what the list shows is what you find. */
    uint32_t dh = h ^ (s_entry_salt * 0x9E3779B9u) ^ 0xD157u;
    dh *= 2654435761u; dh ^= dh >> 15;
    out->distress = (poi->kind != POI_STATION) && si->threat >= 1 &&
                    (dh % 100u) < 11 &&   /* was 22 — user: too many */
                    !(s_distress_done & (1u << (poi->index & 31)));
}

/* One combatant of the battle line. Allies fly police AI (hunt every
 * hostile in the zone) — and friendly fire carries police consequences,
 * which is exactly what shooting your own side should do. */
static void war_spawn_ship(bool ally, int tier) {
    Faction enemy = (Faction)0;
    faction_contested(s_addr, &enemy);
    Faction own = system_faction(s_addr);
    float a = frand(0, 6.2831f);
    float r = frand(450, 850);
    Vec3 pos = v3(cosf(a) * r, frand(-200, 200), sinf(a) * r);
    int cls = ally ? 3 : 2 + (int)(xorshift32() % 3u);
    uint32_t livery = ally
        ? galaxy_get_seed() ^ (uint32_t)(own * 0x51u)
        : galaxy_get_seed() ^ (uint32_t)((int)enemy * 0x77u);
    int idx = ship_spawn(hull_mesh(livery, cls), pos,
                         ally ? TEAM_NEUTRAL : TEAM_HOSTILE);
    if (idx > 0) {
        ship_set_tier(idx, tier, cls);
        if (ally) g_ships[idx].is_police = 1;
        /* faction tag: the HUD names combatants by side, not 'PIRATE',
         * and war kills count whoever scores them (user req) */
        g_ships[idx].war_fac = (uint8_t)((ally ? own : enemy) + 1);
    }
}

static void war_spawn_battle(int quota, int tier) {
    /* The WHOLE enemy force spawns up front — no reinforcements; the
     * zone is won when they're all dead. Enemy rank = the contract's
     * battle tier (VOIDRAT skirmishes up to ELITE wars), and the
     * allied wing thickens for the big ones. */
    if (tier < 0) tier = 2;
    int ldr = tier < 4 ? tier + 1 : 4;
    int allies = 3 + (tier >= 2) + (tier >= 4);
    for (int i = 0; i < allies; i++) {
        int at = tier - (i % 2);               /* allies blend too */
        war_spawn_ship(true, i == 0 ? ldr : (at < 0 ? 0 : at));
    }
    /* Blended ranks (user req: 6-7 elites would be impossible): the
     * contract tier is the CEILING for grunts, thirds stepping down —
     * an ELITE war fields ranks 4/3/2 with one rank-4 leader, not a
     * wall of aces. */
    for (int i = 0; i < quota - 1; i++) {
        int gt = tier - (i % 3);
        war_spawn_ship(false, gt < 0 ? 0 : gt);
    }
    war_spawn_ship(false, ldr);                /* the wing leader */
    s_war_won_toast = false;
    snprintf(s_scoop_toast, sizeof s_scoop_toast, "%s WAR - ENGAGE",
             k_tier_names[tier]);
    s_scoop_toast_t = 3.0f;
}

/* THE RECALL: the Underwriters come to decommission you. A small wing of
 * identical grey BLADE hulls warps in and hunts the player -- the lead
 * carries the signature plasma lance, the rest a varied military arsenal.
 * Spawned at every non-station POI while the recall is armed -- dock to
 * reach the surrender choice, or drive them off. */
static void spawn_adjuster_recall(void) {
    extern int g_force_adjuster;
    g_force_adjuster = 1;                          /* BLADE silhouette */
    const Mesh *m = hull_mesh(0xAD15E00u, 5);      /* one seed -> all identical */
    g_force_adjuster = 0;
    Ship *pl = &g_ships[PLAYER];
    int n = 2;                                     /* a pair of blades */
    /* The Underwriter armoury: cold, military, precise. The lance (phases
     * shields straight to hull) is their signature and the lead blade
     * always carries it -- but the wing fields a mix so the fight isn't
     * one-note. No light peashooters; everything here bites. */
    static const uint8_t k_adjuster_arms[] = {
        WPN_LANCE, WPN_GAUSS, WPN_BEAM, WPN_PHOTON,
        WPN_PLASMA, WPN_BLASTER, WPN_ION, WPN_RAILGUN, WPN_HOMING,
    };
    const int n_arms = (int)(sizeof k_adjuster_arms / sizeof k_adjuster_arms[0]);
    for (int i = 0; i < n; i++) {
        float a = frand(0, 6.2831f);
        Vec3 pos = v3_add(pl->pos, v3(cosf(a) * frand(320, 620),
                                      frand(-160, 160),
                                      sinf(a) * frand(320, 620)));
        int idx = ship_spawn(m, pos, TEAM_HOSTILE);
        if (idx > 0) {
            ship_set_tier(idx, 4, 5);              /* elite-grade, tough.
                                                    * Speed is the MAULER
                                                    * hull's design (110),
                                                    * < the player REAVER. */
            /* Lead blade keeps the signature lance; the rest roll a
             * varied primary, and most pair it with a second weapon. */
            uint32_t r = xorshift32();
            WeaponType primary = (i == 0) ? WPN_LANCE
                               : (WeaponType)k_adjuster_arms[r % (uint32_t)n_arms];
            ship_fit_weapon(idx, 0, primary);
            r = xorshift32();
            if ((r & 3u) != 0u) {                  /* ~75%: a second gun */
                WeaponType sec = (WeaponType)
                    k_adjuster_arms[(r >> 3) % (uint32_t)n_arms];
                if (sec != primary) ship_fit_weapon(idx, 1, sec);
            }
            g_ships[idx].active_w = 0;
            g_ships[idx].ai_target = 0;
            g_ships[idx].is_mark = 1;              /* HUD names them, not PIRATE */
        }
    }
    s_recall_active = true;
    snprintf(s_scoop_toast, sizeof s_scoop_toast, "UNDERWRITERS INBOUND");
    s_scoop_toast_t = 3.5f;
}

/* armed once the surrender is on the table (flag 27) and neither ending
 * has been taken (28 lapse / 30 keep). */
static bool recall_armed(void) {
    return events_flag(27) && !events_flag(28) && !events_flag(30);
}

/* Campaign complete: either ending flag is now set (30 kept current, 28
 * lapsed/paid in full). The first time it happens -- by ANY path -- open
 * the CONGRATULATIONS event (#54), which wraps its text and lets the
 * player carry on. 'ret' is where ST_EVENT hands control back when the
 * banner closes. Returns true if it opened (caller should stop). */
static bool maybe_open_win(uint8_t ret) {
    if (s_won_latch) return false;
    if (!events_flag(28) && !events_flag(30)) return false;
    const Event *ev = events_get(54);
    if (!ev) return false;
    s_won_latch = true;
    ui_event_open(ev);
    s_event_return = ret;
    s_state = ST_EVENT;
    return true;
}

static void spawn_poi_content(void) {
    const SystemInfo *si = system_info();

    /* THE RECALL (campaign climax): the grey ships hunt you everywhere
     * but a station. Dock to reach the surrender (#52), or fight. The
     * Underwriter hails first (opened next tick) -- the wing spawns when
     * that hail closes. */
    if (recall_armed() && s_anchor_has_poi &&
        s_anchor_poi.kind != POI_STATION) {
        s_recall_pending = true;
        return;
    }

    /* Faction battle: an accepted WAR contract turns this system's
     * beacon into the front line — the battle replaces the ambient
     * spawns entirely. */
    if (s_anchor_has_poi && s_anchor_poi.kind == POI_BEACON) {
        int left;
        if (mission_warzone_here(s_addr, &left)) {
            mission_warzone_set_active(true);
            war_spawn_battle(left, mission_warzone_tier(s_addr));
            return;
        }
    }
    /* Contraband heat (user req): illegal cargo draws ambushes — every
     * unit of narcotics/weapons/slaves/contraband raises the odds, and
     * a serious load brings a bigger wing. */
    int illegal = 0;
    for (int g2 = 0; g2 < N_GOODS; g2++)
        if (k_goods[g2].flags & GOOD_ILLEGAL) illegal += g_player.cargo[g2];
    int pirates = 0;
    if (si->threat >= 1 && s_anchor_has_poi) {
        /* Beacons and planets attract trouble; stations are patrolled. */
        int roll = (int)(xorshift32() % 100u);
        int chance = (s_anchor_poi.kind == POI_STATION) ? 25 : 55;
        if (illegal > 0) {
            chance += 15 + (illegal > 10 ? 20 : illegal * 2);
            if (chance > 92) chance = 92;
        }
        if (roll < chance) {
            pirates = 1 + (int)(xorshift32() % si->threat);
            if (illegal >= 5) pirates++;
        }
        if (pirates > 4) pirates = 4;
    }
    for (int i = 0; i < pirates; i++) {
        float a = frand(0, 6.2831f);
        float r = frand(600, 1000);
        Vec3 pos = v3(cosf(a) * r, frand(-200, 200), sinf(a) * r);
        int tier = (int)si->threat - 1 + (int)(xorshift32() % 3u) - 1;
        if (tier < 0) tier = 0;
        /* Local pirate styling: this system's wings share two looks. */
        static const uint8_t k_tier_class[5] = { 1, 2, 3, 4, 5 };
        int cls = k_tier_class[tier > 4 ? 4 : tier];
        uint32_t mseed = (uint32_t)(si->seed >> 24) ^
                         (uint32_t)(cls * 0x9E3779B9u) ^
                         (uint32_t)(i % 3);   /* 3 looks per wing */
        int idx = ship_spawn(hull_mesh(mseed, cls), pos, TEAM_HOSTILE);
        if (idx > 0) ship_set_tier(idx, tier, cls);
    }

    /* Police patrol lawful station space: a Viper pair that minds its
     * own business — unless you're flagged, or you shoot first. */
    if (s_anchor_has_poi && s_anchor_poi.kind == POI_STATION &&
        si->gov >= GOV_CONFED) {
        int n_pol = 1 + (si->gov >= GOV_DEMOCRACY ? 1 : 0);
        for (int k = 0; k < n_pol; k++) {
            float a = frand(0, 6.2831f);
            Vec3 pos = v3(cosf(a) * 420.0f, frand(-80, 80),
                          sinf(a) * 420.0f);
            uint32_t pseed = galaxy_get_seed() ^ 0x70110CEu;  /* one livery */
            int idx = ship_spawn(hull_mesh(pseed, 3), pos, TEAM_NEUTRAL);
            if (idx > 0) {
                ship_set_tier(idx, 3, 3);
                g_ships[idx].is_police = 1;
                g_ships[idx].team = TEAM_NEUTRAL;
            }
        }
    }

    /* Distress call: a civilian under pirate attack — the wing fights
     * THEM until the player engages. Rescue pays credits and rep. */
    s_distress_civ = -1;
    s_distress_paid = false;
    if (s_anchor_has_poi) {
        PoiIntel di;
        elite_game_poi_intel(&s_anchor_poi, &di);
        if (di.distress) {
            uint32_t cseed = (uint32_t)(si->seed >> 20) ^ 0xD15Cu;
            int cls = (xorshift32() & 1) ? 7 : 6;
            Vec3 cpos = v3(frand(-80, 80), frand(-40, 40), 420.0f);
            int civ = ship_spawn(hull_mesh(cseed, cls), cpos,
                                 TEAM_NEUTRAL);
            if (civ > 0) {
                ship_set_tier(civ, 1, cls);
                Ship *cv = &g_ships[civ];
                cv->is_civilian = 1;
                cv->civ_kind = (uint8_t)(cls == 6 ? 0 : 1);
                cv->team = TEAM_NEUTRAL;
                cv->turret_type = 0;
                /* A STURDY hauler (rolled armour+shields) at near-full
                 * HP -- gives the player time to arrive (user). */
                ship_fit_defence(civ, 2);
                cv->hull_max *= 1.6f;
                cv->shield_max *= 1.4f;
                cv->hull = cv->hull_max;
                cv->shield = cv->shield_max * 0.85f;
                s_distress_civ = civ;
                int npir = 1 + (int)si->threat / 2;
                if (npir > 3) npir = 3;
                int first_pir = -1;
                for (int k = 0; k < npir; k++) {
                    float a2 = frand(0, 6.2831f);
                    Vec3 pp = v3_add(cpos, v3(cosf(a2) * 430.0f,
                                              frand(-110, 110),
                                              sinf(a2) * 430.0f));
                    int tier = (int)si->threat - 1;
                    if (tier < 0) tier = 0;
                    int pcls = 1 + tier;
                    uint32_t ms = (uint32_t)(si->seed >> 24) ^
                                  (uint32_t)(pcls * 0x9E3779B9u) ^ k;
                    int idx = ship_spawn(hull_mesh(ms, pcls), pp,
                                         TEAM_HOSTILE);
                    if (idx > 0) {
                        ship_set_tier(idx, tier, pcls);
                        g_ships[idx].ai_target = (uint8_t)civ;
                        if (first_pir < 0) first_pir = idx;
                    }
                }
                if (first_pir > 0)
                    cv->ai_target = (uint8_t)first_pir;  /* fights back */
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "DISTRESS CALL!");
                s_scoop_toast_t = 2.5f;
            }
        }
    }

    /* Civilian traffic: a miner works any belt; cargo ships run lanes
     * near stations and sometimes beacons. Green on the scanner. */
    {
        PoiIntel ci;
        if (s_anchor_has_poi) {
            elite_game_poi_intel(&s_anchor_poi, &ci);
            int want_miner = ci.belt && (int)(xorshift32() % 100u) < 65;
            int want_cargo =
                (s_anchor_poi.kind == POI_STATION &&
                 (int)(xorshift32() % 100u) < 70) ||
                (s_anchor_poi.kind != POI_STATION &&
                 (int)(xorshift32() % 100u) < 25);
            for (int k = 0; k < want_miner + want_cargo; k++) {
                int kind = (k == 0 && want_miner) ? 0 : 1;
                float a = frand(0, 6.2831f);
                float r = kind ? frand(350, 650) : frand(450, 800);
                Vec3 pos = v3(cosf(a) * r, frand(-120, 120),
                              sinf(a) * r);
                uint32_t cseed = (uint32_t)(si->seed >> 20) ^
                                 (uint32_t)(0xC1B1u + k * 77u);
                int cls = kind ? 7 : 6;          /* MULE / PACK MULE */
                int idx = ship_spawn(hull_mesh(cseed, cls), pos,
                                     TEAM_NEUTRAL);
                if (idx > 0) {
                    ship_set_tier(idx, 1, cls);
                    g_ships[idx].is_civilian = 1;
                    g_ships[idx].civ_kind = (uint8_t)kind;
                    g_ships[idx].team = TEAM_NEUTRAL;
                    g_ships[idx].turret_type = 0;
                    ship_fit_defence(idx, 1);   /* rolled trader kit */
                }
            }
        }
    }

    /* Asteroid fields: PERSISTENT geography — the same belt hash the
     * system map's scan strip reports (option-C design). A belt is
     * always at its POI, with a familiar field shape per visit. */
    if (s_anchor_has_poi) {
        PoiIntel intel;
        elite_game_poi_intel(&s_anchor_poi, &intel);
        if (intel.belt)
            rocks_spawn_field(intel.belt_seed, intel.belt_rocks);
    }

    /* Derelict debris (user req): some sites have loot just floating —
     * old wrecks, jettisoned cargo. More in lawless space; beacons and
     * planets are picked-over less often than patrolled stations. */
    {
        int chance = (s_anchor_poi.kind == POI_STATION) ? 12 : 30;
        chance += (int)si->threat * 8;
        if (s_anchor_has_poi && (int)(xorshift32() % 100u) < chance) {
            int n = 1 + (int)(xorshift32() % 3u);
            for (int i = 0; i < n; i++) {
                float a = frand(0, 6.2831f);
                float r = frand(250, 700);
                Vec3 pos = v3(cosf(a) * r, frand(-150, 150),
                              sinf(a) * r);
                loot_on_kill(pos, v3(frand(-2, 2), frand(-2, 2),
                                     frand(-2, 2)),
                             (int)si->threat, NULL);
            }
        }
    }

    /* Derelict hulk: a dead ship drifting off the lane. Fly within
     * boarding range (hostiles cleared) and a TRIG_SPACE event opens —
     * salvage, recorders, traps. One boarding per POI per visit. */
    s_derelict_idx = -1;
    if (s_anchor_has_poi && s_anchor_poi.kind != POI_STATION &&
        !(s_derelict_done & (1u << (s_anchor_poi.index & 31)))) {
        uint32_t dh = (uint32_t)(si->seed >> 18) ^
                      (uint32_t)(s_anchor_poi.kind * 97u) ^
                      (uint32_t)(s_anchor_poi.index * 0x68E31DA4u) ^
                      (s_entry_salt * 0x9E3779B9u);
        dh *= 2654435761u; dh ^= dh >> 15;
        if (dh % 100u < 24) {
            float a = frand(0, 6.2831f);
            float r = frand(380, 620);
            Vec3 pos = v3(cosf(a) * r, frand(-160, 160), sinf(a) * r);
            int cls = 5 + (int)(dh % 3u);     /* mid/heavy hulls wreck big */
            int idx = ship_spawn(hull_mesh(dh ^ 0xDEADu, cls), pos,
                                 TEAM_NEUTRAL);
            if (idx > 0) {
                ship_set_tier(idx, 0, cls);
                Ship *d = &g_ships[idx];
                d->vel = v3(0, 0, 0);
                d->throttle = 0;
                d->assist = false;
                d->turret_type = 0;
                d->shield = d->shield_max = 0;   /* cold and open */
                d->hull = d->hull_max * 0.3f;
                d->is_derelict = 1;     /* scanner + LB lock find it */
                /* not civilian, not police: ai_tick leaves it inert */
                s_derelict_idx = idx;
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "COLD CONTACT ON SCOPE");
                s_scoop_toast_t = 2.5f;
            }
        }
    }

    /* Bounty mark: a flagged pilot at the mission's tier. ACE marks
     * bring an escort. */
    int btier = (s_anchor_has_poi && s_anchor_poi.kind == POI_BEACON)
                    ? mission_bounty_tier_here(s_addr) : -1;
    if (btier > 0) {
        float a = frand(0, 6.2831f);
        Vec3 pos = v3(cosf(a) * 800.0f, frand(-150, 150), sinf(a) * 800.0f);
        uint32_t mseed = (uint32_t)(si->seed >> 20) ^ 0xB011B011u;
        int cls = 2 + btier;          /* bigger marks at higher tier */
        int idx = ship_spawn(hull_mesh(mseed, cls), pos, TEAM_HOSTILE);
        if (idx > 0) {
            ship_set_tier(idx, btier, cls);
            g_ships[idx].is_mark = 1;
            snprintf(s_scoop_toast, sizeof s_scoop_toast,
                     "BOUNTY MARK DETECTED");
            s_scoop_toast_t = 3.0f;
        }
        if (btier >= 4) {
            int e2 = ship_spawn(hull_mesh(mseed ^ 0x55u, 3),
                                v3_add(pos, v3(120, 30, 60)), TEAM_HOSTILE);
            if (e2 > 0) ship_set_tier(e2, 2, 3);
        }
    }

    /* Assassination target: a marked CIVILIAN at the contract beacon.
     * Killing it pays heavy and brands you a fugitive (the murder
     * penalty fires automatically on the kill). */
    if (s_anchor_has_poi && s_anchor_poi.kind == POI_BEACON &&
        mission_assassinate_here(s_addr)) {
        float a = frand(0, 6.2831f);
        Vec3 pos = v3(cosf(a) * 650.0f, frand(-130, 130), sinf(a) * 650.0f);
        uint32_t aseed = (uint32_t)(si->seed >> 16) ^ 0x4551A551u;
        int cls = (xorshift32() & 1) ? 7 : 6;   /* a trader/hauler */
        int idx = ship_spawn(hull_mesh(aseed, cls), pos, TEAM_NEUTRAL);
        if (idx > 0) {
            ship_set_tier(idx, 1, cls);
            g_ships[idx].is_civilian = 1;
            g_ships[idx].is_mark = 1;
            g_ships[idx].team = TEAM_NEUTRAL;
            ship_fit_defence(idx, 3);   /* a well-protected mark */
            snprintf(s_scoop_toast, sizeof s_scoop_toast,
                     "ASSASSINATION TARGET");
            s_scoop_toast_t = 3.0f;
        }
    }
}

/* Arrival comm hail: rare TRIG_ARRIVAL modal right after a real drop —
 * never over a live fight (the arrival ambush IS the event then). */
static void try_arrival_hail(void) {
    if (ships_alive_hostile() > 0) return;
    const Event *ev = events_roll_arrival(system_info());
    if (!ev) return;
    ui_event_open(ev);
    s_event_return = EVRET_FLIGHT;
    s_state = ST_EVENT;
}

/* Guide harness: force-open an in-flight event modal (TRIG_ARRIVAL,
 * falling back to TRIG_SPACE). Returns 1 if a modal opened. */
int elite_game_debug_open_event(void) {
    const Event *ev = events_roll_arrival(system_info());
    if (!ev) ev = events_roll_space(system_info());
    if (!ev) return 0;
    ui_event_open(ev);
    s_event_return = EVRET_FLIGHT;
    s_state = ST_EVENT;
    return 1;
}

/* Re-anchor the local frame at a system-space position. */
static const Mesh *s_station_mesh;   /* generated for the anchored station */

static void drop_anchor(Vec3 pos_mm, const Poi *poi) {
    mission_warzone_set_active(false);   /* leaving the zone stops the clock */
    s_anchor_mm = pos_mm;
    s_anchor_has_poi = (poi != NULL);
    if (poi) s_anchor_poi = *poi;
    if (poi && poi->kind == POI_STATION)
        s_station_mesh = station_gen_mesh(
            (uint32_t)(system_info()->seed >> 8) ^
            (uint32_t)(poi->index * 0x9E3779B9u));
    ships_despawn_npcs();
    hull_cache_reset(g_ships[PLAYER].mesh);
    fx_init();
    loot_init();
    rocks_init();
    s_target = -1;
    s_cloak_t = 0; s_cloak_used = 0;
    s_scan_t = 0; s_scan_done = -1;
    s_tgt_class = 0;                 /* fresh site, AUTO priorities */
}

/* Tie the in-flight nebula wash to where this system sits on the galaxy chart:
 * fly into a charted nebula and the sky washes blue/red; elsewhere it's black. */
static void set_nebula_for_addr(SysAddr a) {
    float lx, ly;
    galaxy_star_pos(a, &lx, &ly);
    float dens = gmap_nebula_density(lx, ly);
    if (dens > 1.0f) dens = 1.0f;
    r3d_scene_set_nebula((uint32_t)((uint32_t)a.sx * 2654435761u ^ (uint32_t)a.sy) | 1u,
                         dens);
}

/* PVP: prepare elite_game's statics for the empty-space LINK ARENA — no
 * station/beacon anchor, HUD reticle locked to the peer, all other locks
 * cleared. elite_pvp.c builds the ships; this just tunes the shell. */
void elite_game_pvp_prep(void) {
    r3d_planet_clear();      /* the arena is EMPTY SPACE: entering PVP from the title
                              * left the last (or never-initialised) star system live,
                              * and its sun disc rendered with a garbage radius — the
                              * 'stalls on arena load' renderer hang */
    s_anchor_has_poi = false;
    s_anchor_mm = v3(0, 0, 0);
    s_station_mesh = NULL;
    s_target = PVP_REMOTE;
    s_loot_target = s_rock_target = -1;
    s_prev_rock = s_prev_loot = -1;
    s_station_lock = false;
    s_menus_live = false;
    s_dead_latch = false;
    s_cloak_t = 0.0f;
    s_incoming = false;
    s_scoop_toast_t = 0; s_scoop_toast[0] = 0;   /* clear any leftover crit toast ('ENGINES HIT!'
                                                    from a prior duel used to stick across menus) */
    /* NB: do NOT set s_state = ST_FLIGHT here. This runs in arena build STEP 7,
     * one frame before step 8 sets s_active=1 and returns PVP_START. Setting
     * FLIGHT here pulled the game out of ST_PVPWAIT before step 8 ran, so PVP
     * never activated (pvp_active()==0): single-player flight ran over the PVP
     * arena, no state packets were sent, and the link stalled then timed out
     * ('opponent flies on AI, not shooting, link lost'). The transition to
     * ST_FLIGHT is owned solely by the PVP_START return in the ST_PVPWAIT case. */
}

static void arrive_in_system(SysAddr addr) {
    s_addr = addr;
    s_entry_salt++;
    s_distress_done = 0;       /* fresh system, fresh emergencies */
    s_derelict_done = 0;
    set_nebula_for_addr(addr);
    loot_set_beacons(true);
    system_enter(addr);
    Poi beacon;
    Poi pois[MAX_POIS];
    int n = system_pois(pois, MAX_POIS);
    beacon = pois[0];                       /* beacon is always first */
    (void)n;
    drop_anchor(beacon.pos_mm, &beacon);
    Ship *p = &g_ships[PLAYER];
    p->pos = v3(frand(-300, 300), frand(-100, 100), -700.0f);
    p->vel = v3_scale(p->basis.r[2], 40.0f);
    spawn_poi_content();
}

/* --- target cycling (unchanged from Phase 3) ----------------------------*/
static void cycle_target(void) {
    Vec3 pp = g_ships[PLAYER].pos;
    s_loot_target = -1;
    if (s_tgt_class >= 1) {
        /* Forced class: skip the hostile scan entirely. */
        s_target = -1;
        s_station_lock = false;
        s_rock_target = -1;
        if (s_tgt_class == 1) {
            /* step to the next-farther canister; wrap to nearest
             * (user report: the lock stuck on one target) */
            Vec3 lp[6]; int lc[6];
            int nl = loot_positions(lp, lc, 6);
            float cur_d = (s_prev_loot >= 0 && s_prev_loot < nl)
                              ? v3_len(v3_sub(lp[s_prev_loot], pp))
                              : -1.0f;
            int first = -1, next = -1;
            float fd = 1e30f, nd = 1e30f;
            for (int i = 0; i < nl; i++) {
                float d = v3_len(v3_sub(lp[i], pp));
                if (d < fd) { fd = d; first = i; }
                if (cur_d >= 0 && d > cur_d && d < nd) { nd = d; next = i; }
            }
            s_loot_target = (next >= 0) ? next : first;
            s_prev_loot = s_loot_target;
        } else if (s_tgt_class == 3) {
            /* FRIENDLY (user req): step through neutral ships —
             * civilians, police/war allies, derelicts. */
            float cur_d = (s_target > 0 && g_ships[s_target].alive)
                              ? v3_len(v3_sub(g_ships[s_target].pos, pp))
                              : -1.0f;
            int first = -1, next = -1;
            float fd = 1e30f, nd = 1e30f;
            for (int i = 1; i < MAX_SHIPS; i++) {
                const Ship *fs = &g_ships[i];
                if (!fs->alive || fs->team == TEAM_HOSTILE) continue;
                if (!fs->is_civilian && !fs->is_police &&
                    !fs->is_derelict) continue;
                float d = v3_len(v3_sub(fs->pos, pp));
                if (d < fd) { fd = d; first = i; }
                if (cur_d >= 0 && d > cur_d && d < nd) { nd = d; next = i; }
            }
            s_target = (next >= 0) ? next : first;
        } else {
            /* same stepping for rocks */
            float cur_d = -1.0f;
            {
                Vec3 cp; float cr;
                if (s_prev_rock >= 0 && rocks_get(s_prev_rock, &cp, &cr))
                    cur_d = v3_len(v3_sub(cp, pp));
            }
            int first = -1, next = -1;
            float fd = 1e30f, nd = 1e30f;
            for (int i = 0; i < 8; i++) {
                Vec3 rp; float rr;
                if (!rocks_get(i, &rp, &rr)) continue;
                float d = v3_len(v3_sub(rp, pp));
                if (d < fd) { fd = d; first = i; }
                if (cur_d >= 0 && d > cur_d && d < nd) { nd = d; next = i; }
            }
            s_rock_target = (next >= 0) ? next : first;
            s_prev_rock = s_rock_target;
        }
        return;
    }
    int best = -1, first = -1;
    float cur_d = -1.0f, best_d = 1e30f, first_d = 1e30f;
    if (s_target >= 0 && g_ships[s_target].alive)
        cur_d = v3_len(v3_sub(g_ships[s_target].pos, pp));
    for (int i = 1; i < MAX_SHIPS; i++) {
        if (!g_ships[i].alive || g_ships[i].team != TEAM_HOSTILE) continue;
        float d = v3_len(v3_sub(g_ships[i].pos, pp));
        if (d < first_d) { first_d = d; first = i; }
        if (cur_d >= 0.0f && d > cur_d && d < best_d) { best_d = d; best = i; }
    }
    s_target = (best >= 0) ? best : first;
    /* Nothing hostile: NEUTRAL ships first (find the rescued civilian
     * — user report: the green ship was untargetable), then salvage,
     * rocks, station compass. */
    s_station_lock = false;
    s_rock_target = -1;
    if (s_target < 0) {
        int nbest = -1, nfirst = -1;
        float nbest_d = 1e30f, nfirst_d = 1e30f;
        for (int i = 1; i < MAX_SHIPS; i++) {
            if (!g_ships[i].alive || g_ships[i].team == TEAM_HOSTILE)
                continue;
            if (!g_ships[i].is_civilian && !g_ships[i].is_police &&
                !g_ships[i].is_derelict)
                continue;
            float d = v3_len(v3_sub(g_ships[i].pos, pp));
            if (d < nfirst_d) { nfirst_d = d; nfirst = i; }
            if (cur_d >= 0.0f && d > cur_d && d < nbest_d) {
                nbest_d = d; nbest = i;
            }
        }
        s_target = (nbest >= 0) ? nbest : nfirst;
    }
    if (s_target < 0) {
        s_loot_target = loot_nearest(pp, NULL);
        if (s_loot_target < 0) {
            /* Prospector lock: nearest belt rock (user req: BELT! on
             * the map needs a way to FIND the rocks). */
            Vec3 rk[8];
            int nr = rocks_positions(rk, 8);
            float bd2 = 1e30f;
            for (int i = 0; i < nr; i++) {
                float d2 = v3_len(v3_sub(rk[i], pp));
                if (d2 < bd2) { bd2 = d2; s_rock_target = i; }
            }
        }
        if (s_loot_target < 0 && s_rock_target < 0 &&
            s_anchor_has_poi && s_anchor_poi.kind == POI_STATION)
            s_station_lock = true;
    }
}

/* Resume docked at a saved station. */
static void arrive_docked(const SaveMeta *meta) {
    s_addr = meta->addr;
    set_nebula_for_addr(meta->addr);
    loot_set_beacons(true);
    system_enter(meta->addr);
    Poi pois[MAX_POIS];
    int n = system_pois(pois, MAX_POIS);
    const Poi *st = NULL;
    for (int i = 0; i < n; i++)
        if (pois[i].kind == POI_STATION && pois[i].index == meta->station)
            st = &pois[i];
    if (!st) {                       /* damaged save: fall back to beacon */
        drop_anchor(pois[0].pos_mm, &pois[0]);
        spawn_player();
        return;
    }
    drop_anchor(st->pos_mm, st);
    spawn_player();
    station_open(st->index);
    s_state = ST_DOCKED;
}

static void start_new_game(uint32_t seed);

void elite_game_init(uint32_t seed) {
    s_rng = seed * 2654435761u;
    s_rng ^= s_rng >> 15;
    if (!s_rng) s_rng = 1;
    loot_seed(seed ^ 0x100Du);
    s_boot_seed = seed;
    galaxy_set_seed(seed);
    ships_init();
    fx_init();
    audio_init();
    combat_init();
    elite_input_reset();
    spawn_player();
    player_init();
    missions_init();
    events_init();
    r3d_starfield_init(seed ^ 0x7117u);

    /* Title vista: park the sim on a nearby planet-bearing system so the title
     * has a world to frame. Replaced the instant the player enters the galaxy
     * (CONTINUE / NEW GAME both re-enter a system). */
    {
        SystemInfo probe;
        int so = (int)(seed % 7) - 3, done = 0;
        for (int sy = -5 + so; sy <= 5 + so && !done; sy++)
            for (int sx = -5 + so; sx <= 5 + so && !done; sx++) {
                int n = galaxy_sector_stars(sx, sy);
                for (int i = 0; i < n && !done; i++) {
                    SysAddr a = { (int16_t)sx, (int16_t)sy, (uint8_t)i };
                    galaxy_generate(a, &probe);
                    if (probe.n_planets >= 2) { system_enter(a); done = 1; }
                }
            }
        if (!done) system_enter((SysAddr){ 0, 0, 0 });   /* fallback: any system */
    }

    /* Title battle: the spectator camera anchors a few radii off a planet's lit
     * side and looks at it, with the brawl spawned right in front so the world
     * (and the star beyond) backdrop the dogfight. */
    {
        const SystemInfo *si = system_info();
        Ship *p = &g_ships[PLAYER];
        p->pos = v3(0, 0, 0);
        p->team = TEAM_NEUTRAL;
        Vec3 up0 = v3(0, 1, 0), fwd;
        if (si->n_planets > 0) {
            Vec3 P = system_planet_pos_mm(0);
            float pr = si->planets[0].radius_mm;
            Vec3 toStar = v3_norm(v3_scale(P, -1.0f));     /* lit side */
            Vec3 anchor = v3_add(P, v3_scale(toStar, pr * 3.3f));
            fwd = v3_norm(v3_sub(P, anchor));              /* = toward the world */
            Poi pp; pp.kind = POI_PLANET; pp.index = 0; pp.pos_mm = P;
            snprintf(pp.name, sizeof pp.name, "%s", si->name);
            drop_anchor(anchor, &pp);
        } else {
            fwd = v3_norm(v3(0.3f, 0.05f, 1.0f));
            Poi pois[MAX_POIS];
            if (system_pois(pois, MAX_POIS) > 0) drop_anchor(pois[0].pos_mm, &pois[0]);
        }
        p->basis.r[2] = fwd;
        p->basis.r[0] = v3_norm(v3_cross(up0, fwd));
        p->basis.r[1] = v3_cross(fwd, p->basis.r[0]);
        /* Brawl sits ~130 m ahead (in front of the world), a touch low. */
        s_title_ctr = v3_add(v3_scale(fwd, 130.0f), v3_scale(p->basis.r[1], -18.0f));
        s_title_perp = p->basis.r[0];
    }
    r3d_scene_set_nebula(seed | 1u, 1.0f);   /* blue/red galaxy wash on the title */
    loot_set_beacons(false);                 /* bare cubes on the title */

    s_state = ST_TITLE;
    save_rebuild_list();                       /* enumerate saved games */
    s_title_cursor = (s_savelist_n > 0) ? 0 : 1;
    s_intro_active = false;           /* crawl plays ONLY on NEW GAME */
    s_prev_menu = s_prev_a = false;
}


#if ELITE_CHEATS
/* Cheat-menu test loadout: a top-end combat ship so scenarios that drop
 * you into a fight (war front, the recall climax) are properly playable.
 * The REAVER is the fastest-turning hull with three hardpoints, so it
 * actually swings onto a target. Loadout (user spec): the best-rolled
 * prototype-grade gear -- PULSE-L (large laser), plasma BLASTER, HOMING
 * missiles, all AFX_TUNED; Z3 prototype shield + armour; a targeting
 * computer + repair drone in the util bays; full chaff. No turret. */
static void cheat_combat_ship(int hull_id) {
    g_player.hull_id = (uint8_t)hull_id;
    g_player.hull_seed = 0x5EED77u + (uint32_t)hull_id * 0x1000u;
    for (int i = 0; i < HULL_SLOTS; i++) {
        g_player.mounts[i].in_use = 0;
        g_player.ammo[i] = -1;
    }
    for (int i = 0; i < 4; i++) g_player.util_eq[i].in_use = 0;
    g_player.turret_eq.in_use = 0;
    g_player.mounts[0] = (WeaponInst){ .type = WPN_PULSE_L, .quality = Q_PROTOTYPE,
                                       .affix = AFX_TUNED, .integrity = 100, .in_use = 1 };
    g_player.mounts[1] = (WeaponInst){ .type = WPN_BLASTER, .quality = Q_PROTOTYPE,
                                       .affix = AFX_TUNED, .integrity = 100, .in_use = 1 };
    g_player.mounts[2] = (WeaponInst){ .type = WPN_HOMING, .quality = Q_PROTOTYPE,
                                       .affix = AFX_TUNED, .integrity = 100, .in_use = 1 };
    g_player.shield_eq = (WeaponInst){ .type = EQ_SHIELD, .quality = Q_PROTOTYPE,
                                       .integrity = 100, .in_use = 1, .tier = 3 };
    g_player.armor_eq  = (WeaponInst){ .type = EQ_ARMOR, .quality = Q_PROTOTYPE,
                                       .integrity = 100, .in_use = 1, .tier = 3 };
    /* Util bays (REAVER/BASILISK both roll >=2 bays): targeting computer
     * for the lead reticle + seeker agility, repair drone to mend hull in
     * flight. */
    g_player.util_eq[0] = (WeaponInst){ .type = EQ_TARGETCOMP, .quality = Q_STANDARD,
                                        .integrity = 100, .in_use = 1, .tier = 1 };
    g_player.util_eq[1] = (WeaponInst){ .type = EQ_DRONE, .quality = Q_STANDARD,
                                        .integrity = 100, .in_use = 1, .tier = 1 };
    g_player.chaff_charges = 4;
}

/* Apply a chosen test scenario (cheat menu). Runs after events_init so the
 * story flags below survive. */
static void apply_scenario(int sc) {
    g_player.credits = 300000;
    switch (sc) {
    case 1: break;                                          /* RICH: wallet only */
    case 2: cheat_combat_ship(4); events_set_chance(100); break;  /* ENCOUNTERS */
    case 3: cheat_combat_ship(4);                                  /* WAR FRONT */
            for (int f = 0; f < N_FACTIONS; f++) g_rep[f] = 25; break;
    case 4: cheat_combat_ship(4); events_set_chance(100); break;   /* ACT1 */
    case 5: cheat_combat_ship(4);                                  /* CLIMAX/RECALL */
            for (int f = 0; f < N_FACTIONS; f++) g_rep[f] = 25;
            for (int fl = 10; fl <= 27; fl++) events_set_flag(fl);
            for (int lr = 0; lr <= 24; lr++) events_set_lore(lr);
            events_set_chance(100); break;
    case 6: cheat_combat_ship(9); break;                          /* ELITE SHIP */
    }
    player_apply_to_ship();
    g_ships[PLAYER].hull = g_ships[PLAYER].hull_max;
    g_ships[PLAYER].shield = g_ships[PLAYER].shield_max;
}
#endif /* ELITE_CHEATS */

static void start_new_game(uint32_t seed) {
    s_save_slot = -1;                  /* unsaved until the first dock */
    s_won_latch = false;               /* a fresh pilot can earn the ending */
    galaxy_set_seed(seed);
    ships_init();
    fx_init();
    combat_init();
    elite_input_reset();
    player_init();
    /* Every commander starts at the bottom (user spec): a random cheap
     * hull, a battered low-grade loadout (always at least one weapon,
     * launchers very rare) and 1,000 credits. Earn the rest. */
    {
        uint32_t h = seed * 2654435761u;
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        /* Hull: SKIFF 50% / DART 25% / PACK MULE 15% / SPARROW 10%. */
        int r = (int)(h % 100u);
        g_player.hull_id = (r < 50) ? 0 : (r < 75) ? 1 : (r < 90) ? 6 : 2;
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        g_player.hull_seed = h;
        const HullDef *hd = &k_hulls[g_player.hull_id];
        for (int i = 0; i < hd->n_slots; i++) {
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            if (i > 0 && (h & 1)) {            /* extra slots often empty */
                g_player.mounts[i].in_use = 0;
                continue;
            }
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            int wr = (int)(h % 100u);
            WeaponType w = (wr < 45) ? WPN_PULSE_S
                         : (wr < 70) ? WPN_AUTOCANNON
                         : (wr < 80) ? WPN_PULSE_M
                         : (wr < 88) ? WPN_BEAM
                         : (wr < 93) ? WPN_PHOTON
                         : (wr < 97) ? WPN_GAUSS
                         : (wr < 99) ? WPN_MISSILE : WPN_HOMING;
            if (k_weapons[w].size > hd->slot_size[i]) w = WPN_PULSE_S;
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            int q = ((h >> 4) % 100u < 70) ? Q_SALVAGED : Q_STANDARD;
            g_player.mounts[i] = (WeaponInst){
                .type = (uint8_t)w, .quality = (uint8_t)q,
                .integrity = (uint8_t)(55 + (h % 36u)), .in_use = 1,
            };
        }
        g_player.credits = s_cheat_on ? 300000 : 1000;
    }
    spawn_player();    /* AFTER player state is final */
    missions_init();
    events_init();
#if ELITE_CHEATS
    if (s_cheat_scenario) apply_scenario(s_cheat_scenario);  /* test setup (after events_init) */
#endif
    s_state = ST_FLIGHT;

    /* Find a starting system: spiral out from the origin for a system
     * that (a) has a station and (b) sits in a REACHABLE CLUSTER — at
     * least 2 neighbours within a starter ship's jump range, one of
     * them with its own station (user req: never strand a new game
     * 12+ ly from everything). Best candidate wins; barren-galaxy
     * fallbacks keep the old behaviour. */
    SysAddr start = {0, 0, 0};
    SysAddr station_fallback = {0, 0, 0};
    SysAddr fallback = {0, 0, 0};
    SysAddr reach_fb = {0, 0, 0};      /* reachable but not safe */
    const float START_JUMP = 6.0f;     /* SKIFF range with margin */
    bool found = false, have_st_fb = false, have_fallback = false;
    bool have_reach_fb = false;
    for (int ring = 0; ring < 14 && !found; ring++)
        for (int sy = -ring; sy <= ring && !found; sy++)
            for (int sx = -ring; sx <= ring && !found; sx++) {
                if (sx > -ring && sx < ring && sy > -ring && sy < ring)
                    continue;
                int n = galaxy_sector_stars(sx, sy);
                for (int i = 0; i < n && !found; i++) {
                    SysAddr a = { sx, sy, (uint8_t)i };
                    if (!have_fallback) { fallback = a; have_fallback = true; }
                    SystemInfo probe;
                    galaxy_generate(a, &probe);
                    if (probe.n_stations == 0) continue;
                    if (!have_st_fb) { station_fallback = a; have_st_fb = true; }
                    /* Count starter-range neighbours. */
                    float px, py;
                    galaxy_star_pos(a, &px, &py);
                    int near = 0, near_station = 0;
                    for (int ny = a.sy - 1; ny <= a.sy + 1; ny++)
                        for (int nx = a.sx - 1; nx <= a.sx + 1; nx++) {
                            int nn = galaxy_sector_stars(nx, ny);
                            for (int j = 0; j < nn; j++) {
                                SysAddr b = { nx, ny, (uint8_t)j };
                                if (sysaddr_eq(b, a)) continue;
                                float bx, by;
                                galaxy_star_pos(b, &bx, &by);
                                float dx = bx - px, dy = by - py;
                                if (dx * dx + dy * dy >
                                    START_JUMP * START_JUMP) continue;
                                near++;
                                if (near_station == 0) {
                                    SystemInfo nb;
                                    galaxy_generate(b, &nb);
                                    if (nb.n_stations > 0) near_station = 1;
                                }
                            }
                        }
                    /* Always start a new game in a SAFE system (user):
                     * threat 0 = no pirates lurking. Reachable-but-not-
                     * safe is the fallback if no safe start is nearby. */
                    if (near >= 2 && near_station) {
                        if (probe.threat == 0) { start = a; found = true; }
                        else if (!have_reach_fb) {
                            reach_fb = a; have_reach_fb = true;
                        }
                    }
                }
            }
    if (!found)
        start = have_reach_fb ? reach_fb
              : have_st_fb ? station_fallback : fallback;
    arrive_in_system(start);
    r3d_starfield_init((uint32_t)(system_info()->seed >> 16));
}

void elite_game_set_frame_ms(float ms) { s_frame_ms = ms; }

/* --- state ticks ---------------------------------------------------------*/
/* Docking availability: anchored at a station, close, not under fire. */
static bool can_dock(void) {
    if (!s_anchor_has_poi || s_anchor_poi.kind != POI_STATION) return false;
    if (!g_ships[PLAYER].alive) return false;
    return v3_len(g_ships[PLAYER].pos) < DOCK_RANGE;
}

static float s_dock_t;

static void tick_flight(const CraftRawButtons *btn, float dt) {
    FlightInput in;
    elite_input_update(btn, dt, &in);

    /* THE RECALL: the first safe moment after an armed arrival, the
     * Underwriter hails (a clear voice, raised tension) -- the grey wing
     * spawns when that hail closes. Only in true flight, not menu-live. */
    if (s_recall_pending && s_state == ST_FLIGHT && g_ships[PLAYER].alive) {
        s_recall_pending = false;
        const Event *ev = events_get(53);
        if (ev) {
            ui_event_open(ev);
            s_event_return = EVRET_FLIGHT;
            s_state = ST_EVENT;
            s_recall_spawn = true;
            return;
        }
        spawn_adjuster_recall();          /* fallback: no hail, just the fight */
    }

    Ship *p = &g_ships[PLAYER];
    bool dead_latch = s_dead_latch;
    (void)dead_latch;
    if (p->alive) {
        s_dead_latch = false;

        /* LB+RB chord (or a dedicated DOCK button) near a station = dock. */
        if (((btn->lb && btn->rb) || in.dock) && can_dock()) {
            s_dock_t = 0;
            s_state = ST_DOCKING;
            return;
        }

        flight_apply_input(&in, dt);
        /* RAILGUN charges while A is held, fires on release at full
         * charge (rising arm tones + HUD arc). Everything else fires
         * on hold as usual. */
        if (p->weapons[p->active_w] == WPN_RAILGUN) {
            static float charge;
            static int charge_step;
            if (in.fire && combat_can_fire(p)) {
                charge += dt;
                int st2 = (int)(charge / 0.2f);
                if (st2 > charge_step && st2 <= 4) {
                    charge_step = st2;
                    sfx_charge_step(st2 - 1);
                }
                s_rail_charge01 = charge / 0.8f;
                if (s_rail_charge01 > 1.0f) s_rail_charge01 = 1.0f;
            } else {
                if (charge >= 0.8f)
                    combat_fire(PLAYER, 0.0f, s_target);
                charge = 0;
                charge_step = 0;
                s_rail_charge01 = 0;
            }
        } else if (in.fire) {
            combat_fire(PLAYER, 0.0f, s_target);
        }
        /* PC dedicated buttons: fire the 2nd / 3rd mounts directly. */
        if (in.fire2) combat_player_fire_slot(1, s_target);
        if (in.fire3) combat_player_fire_slot(2, s_target);
        if (in.secondary && p->n_weapons > 1)       /* B = next weapon */
            p->active_w = (uint8_t)((p->active_w + 1) % p->n_weapons);
        /* MANIFEST SCANNER: hold a lock on a civilian to read its hold. */
        if (player_has_util(EQ_MANIFEST) && s_target > 0 &&
            g_ships[s_target].alive && g_ships[s_target].is_civilian &&
            s_target != s_scan_done &&
            v3_len2(v3_sub(g_ships[s_target].pos, p->pos)) <
                350.0f * 350.0f) {
            s_scan_t += dt;
            if (s_scan_t >= 2.5f) {
                s_scan_done = s_target;
                s_scan_t = 0;
                uint32_t mh = (uint32_t)(s_target * 2654435761u) ^
                              g_ships[s_target].civ_kind * 977u;
                mh ^= mh >> 13;
                if (g_ships[s_target].civ_kind == 0) {     /* miner */
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "MANIFEST: %u MINERALS %u METALS",
                             2 + (mh % 5u), 1 + ((mh >> 4) % 4u));
                } else {
                    int g1 = (int)(mh % 16u);
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "MANIFEST: %u %s", 3 + ((mh >> 8) % 7u),
                             k_goods[g1].name);
                }
                s_scoop_toast_t = 3.0f;
                sfx_lock_acquire();
            }
        } else if (s_target != s_scan_done) {
            s_scan_t = 0;
        }

        if (in.cloak && player_has_util(EQ_CLOAK)) {
            if (s_cloak_t > 0) {
                /* manual decloak refunds nothing */
                s_cloak_t = 0;
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "CLOAK DISENGAGED");
                s_scoop_toast_t = 2.0f;
            } else if (s_cloak_used) {
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "CLOAK SPENT - RECHARGES IN DOCK");
                s_scoop_toast_t = 2.0f;
            } else {
                s_cloak_used = 1;
                s_cloak_t = 8.0f;
                s_target = -1;            /* your own lock drops too */
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "CLOAK ENGAGED");
                s_scoop_toast_t = 2.0f;
                sfx_sc_engage();
            }
        }
        if (s_cloak_t > 0) {
            s_cloak_t -= dt;
            p->heat += 28.0f * dt;        /* outruns dissipation (22/s):
                                             net +6/s — 8s of veil costs
                                             half the heat bar */
            if (in.fire) {                /* firing tears the veil */
                s_cloak_t = 0;
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "CLOAK BROKEN");
                s_scoop_toast_t = 2.0f;
            }
        }
        if (in.chaff && player_has_util(EQ_CHAFF) &&
            g_player.chaff_charges > 0) {
            g_player.chaff_charges--;
            int broke = proj_break_locks(PLAYER);
            fx_chaff_burst(v3_sub(p->pos, v3_scale(p->basis.r[2],
                                                   p->mesh->bound_r)),
                           p->vel);
            sfx_chaff();
            snprintf(s_scoop_toast, sizeof s_scoop_toast,
                     broke ? "CHAFF! %d LOCKS BROKEN" : "CHAFF AWAY",
                     broke);
            s_scoop_toast_t = 1.5f;
        }
        if (in.tgt_class_cycle) {
            /* LB double-tap: demote the lock class (input layer
             * classifies the double; 0.5s window). */
            s_tgt_class = (s_tgt_class + 1) % 4;
            static const char *k_tc[4] = { "TGT: AUTO", "TGT: SALVAGE",
                                           "TGT: ROCKS", "TGT: FRIENDLY" };
            snprintf(s_scoop_toast, sizeof s_scoop_toast, "%s",
                     k_tc[s_tgt_class]);
            s_scoop_toast_t = 1.4f;
            cycle_target();              /* re-lock within the class */
        } else if (in.cycle_target) {
            int before = s_target;
            cycle_target();
            if (s_target >= 0 && s_target != before) sfx_lock_acquire();
        }
        /* Seeker tracking you: repeating alarm + HUD INCOMING flash. */
        {
            static float warn_cd;
            if (proj_homing_on(PLAYER)) {
                s_incoming = true;
                warn_cd -= dt;
                if (warn_cd <= 0.0f) {
                    sfx_lock_warn();
                    warn_cd = 0.55f;
                }
            } else {
                s_incoming = false;
                warn_cd = 0;
            }
        }
        combat_set_player_target(
            (s_target >= 0 && g_ships[s_target].alive) ? s_target : -1);
        /* Police scans: a neutral patrol inside 300m sweeps your hold —
         * carrying contraband gets you FLAGGED, and flagged pilots get
         * engaged. Shooting first does too (see combat). */
        {
            static float scan_t;
            int illegal2 = 0;
            for (int g2 = 0; g2 < N_GOODS; g2++)
                if (k_goods[g2].flags & GOOD_ILLEGAL)
                    illegal2 += g_player.cargo[g2];
            int near_police = -1;
            for (int i = 1; i < MAX_SHIPS; i++)
                if (g_ships[i].alive && g_ships[i].is_police &&
                    g_ships[i].team == TEAM_NEUTRAL &&
                    v3_len(v3_sub(g_ships[i].pos, p->pos)) < 300.0f) {
                    near_police = i;
                    break;
                }
            if (near_police >= 0 && illegal2 > 0 &&
                g_player.legal == 0) {
                scan_t += dt;
                if (scan_t > 0.8f && scan_t < 0.9f) {
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "POLICE SCAN...");
                    s_scoop_toast_t = 1.6f;
                }
                if (scan_t > 2.6f) {
                    g_player.legal = 1;
                    g_player.fine += 120 + illegal2 * 30;
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "FLAGGED: SMUGGLER");
                    s_scoop_toast_t = 3.0f;
                    sfx_lock_warn();
                }
            } else {
                scan_t = 0;
            }
            /* Flagged pilots get engaged on sight. */
            if (g_player.legal >= 1)
                for (int i = 1; i < MAX_SHIPS; i++)
                    if (g_ships[i].alive && g_ships[i].is_police &&
                        g_ships[i].team == TEAM_NEUTRAL)
                        g_ships[i].team = TEAM_HOSTILE;
        }
        /* Miners attract vultures: while a rock field is live, an
         * ambush clock runs — one threat-scaled pirate jump per visit,
         * announced a beat before they arrive. */
        {
            static float ambush_t;
            static bool ambushed;
            Vec3 rk[8];
            if (!ambushed && rocks_positions(rk, 8) > 0) {
                const SystemInfo *si2 = system_info();
                ambush_t += dt;
                float due = 50.0f - (float)si2->threat * 8.0f;
                if (si2->threat > 0 && ambush_t > due) {
                    ambushed = true;
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "PIRATES INBOUND");
                    s_scoop_toast_t = 2.5f;
                    sfx_lock_warn();
                    int n2 = 1 + (int)(xorshift32() % 2u);
                    for (int k = 0; k < n2; k++) {
                        float a2 = frand(0, 6.2831f);
                        Vec3 pp2 = v3_add(g_ships[PLAYER].pos,
                                          v3(cosf(a2) * 900.0f,
                                             frand(-200, 200),
                                             sinf(a2) * 900.0f));
                        int tier2 = (int)si2->threat - 1 +
                                    (int)(xorshift32() % 2u);
                        if (tier2 < 0) tier2 = 0;
                        int cls2 = 1 + tier2;
                        if (cls2 > 5) cls2 = 5;
                        uint32_t ms2 = (uint32_t)(si2->seed >> 24) ^
                                       (uint32_t)(cls2 * 0x9E3779B9u) ^ k;
                        int idx2 = ship_spawn(hull_mesh(ms2, cls2), pp2,
                                              TEAM_HOSTILE);
                        if (idx2 > 0) ship_set_tier(idx2, tier2, cls2);
                    }
                }
            } else if (rocks_positions(rk, 8) == 0) {
                ambush_t = 0;     /* reset between fields */
                ambushed = false;
            }
        }
        /* REPAIR DRONE (R2 unit): given time it patches the hull, then
         * works through damaged items one by one — a critted mount can
         * come back ONLINE mid-fight. Toasts on job start + finish. */
        if (player_has_util(EQ_DRONE)) {
            static float dr_acc;
            static int dr_job = -1;       /* 0 hull, 1.. = item index */
            WeaponInst *items[10];
            const char *names[10];
            int ni = 0;
            for (int i = 0; i < HULL_SLOTS; i++) {
                static char wn[3][8];
                snprintf(wn[i], sizeof wn[i], "WPN %d", i + 1);
                items[ni] = &g_player.mounts[i]; names[ni++] = wn[i];
            }
            items[ni] = &g_player.shield_eq; names[ni++] = "SHIELD GEN";
            items[ni] = &g_player.armor_eq;  names[ni++] = "ARMOR";
            for (int u = 0; u < 4; u++) {
                WeaponInst *ue = &g_player.util_eq[u];
                /* never the drone itself (user report: a used-market
                 * drone under 100% announced REPAIRING GADGET forever
                 * — it was licking its own wounds) */
                if (ue->in_use && ue->type == EQ_DRONE) continue;
                items[ni] = ue;
                names[ni++] = (ue->in_use && ue->type >= WPN_COUNT &&
                               ue->type < ITEM_COUNT)
                                  ? k_equip[ue->type - WPN_COUNT].name
                                  : "GADGET";
            }
            int want = -1;
            if (p->hull < p->hull_max - 0.5f) want = 0;
            else
                for (int i = 0; i < ni; i++)
                    if (items[i]->in_use && items[i]->integrity < 100) {
                        want = 1 + i;
                        break;
                    }
            if (want != dr_job) {
                dr_job = want;
                dr_acc = 0;
                if (want == 0) {
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "DRONE: REPAIRING HULL..");
                    s_scoop_toast_t = 2.0f;
                } else if (want > 0) {
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "DRONE: REPAIRING %s..", names[want - 1]);
                    s_scoop_toast_t = 2.0f;
                }
            }
            if (dr_job == 0) {
                p->hull += 1.2f * dt;     /* slow: a full hull is minutes */
                if (p->hull >= p->hull_max) {
                    p->hull = p->hull_max;
                    snprintf(s_scoop_toast, sizeof s_scoop_toast,
                             "DRONE: HULL REPAIRED");
                    s_scoop_toast_t = 2.5f;
                    dr_job = -1;
                }
            } else if (dr_job > 0) {
                WeaponInst *it = items[dr_job - 1];
                dr_acc += dt;
                if (dr_acc >= 1.6f) {     /* +1 integrity / 1.6 s */
                    dr_acc -= 1.6f;
                    if (it->integrity < 100) it->integrity++;
                    if (it->integrity >= 100) {
                        snprintf(s_scoop_toast, sizeof s_scoop_toast,
                                 "DRONE: %s REPAIRED",
                                 names[dr_job - 1]);
                        s_scoop_toast_t = 2.5f;
                        player_apply_to_ship();
                        dr_job = -1;
                    } else if ((it->integrity % 25) == 0) {
                        player_apply_to_ship();   /* caps track repair */
                    }
                }
            }
        }
        /* Warzone: hold the line. Enemy waves keep coming while the
         * contract has kills left; allies thin out and get topped up
         * more slowly (you are the difference-maker, not a spectator). */
        if (s_anchor_has_poi && s_anchor_poi.kind == POI_BEACON) {
            int left;
            if (mission_warzone_here(s_addr, &left)) {
                /* No reinforcements (user req): the force you saw on
                 * arrival is the force you fight. The HUD toast keeps
                 * the count honest. */
                (void)left;
            }
            /* Completion: warzone_here goes false the moment the quota
             * fills (mission flips done). Say it once. */
            if (!mission_warzone_here(s_addr, NULL) && !s_war_won_toast) {
                for (int i = 0; i < MAX_MISSIONS; i++)
                    if (g_missions[i].type == MIS_WARZONE &&
                        g_missions[i].done &&
                        sysaddr_eq(g_missions[i].target, s_addr)) {
                        s_war_won_toast = true;
                        mission_warzone_set_active(false);
                        snprintf(s_scoop_toast, sizeof s_scoop_toast,
                                 "ZONE SECURE - PAY AT ANY DOCK");
                        s_scoop_toast_t = 4.0f;
                    }
            }
        }
        /* Recall driven off: you killed the grey wing rather than dock and
         * surrender. The system reads it as compliance under duress -- you
         * stay CURRENT (flag 30), the recall disarms, but you are not free. */
        if (s_recall_active && s_state == ST_FLIGHT &&
            ships_alive_hostile() == 0) {
            s_recall_active = false;
            events_set_flag(30);
            events_set_lore(25);                 /* KEPT CURRENT */
            /* Campaign-complete banner (wraps; replaces the old toast that
             * ran off-screen). Falls back to a short toast if it can't open. */
            if (!maybe_open_win(EVRET_FLIGHT)) {
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "STILL CURRENT");
                s_scoop_toast_t = 4.5f;
            }
        }
        /* Derelict boarding: drift up to the cold hull with the sky
         * clear and the hatch is yours — a TRIG_SPACE event decides
         * what's inside. One boarding per POI per visit. */
        if (s_derelict_idx > 0 && s_state == ST_FLIGHT) {
            Ship *d = &g_ships[s_derelict_idx];
            if (!d->alive) {
                s_derelict_idx = -1;          /* shot to pieces — gone */
            } else if (ships_alive_hostile() == 0) {
                float dd = v3_len(v3_sub(d->pos, g_ships[PLAYER].pos));
                if (dd < 180.0f) {
                    s_derelict_done |= 1u << (s_anchor_poi.index & 31);
                    s_derelict_idx = -1;      /* hulk stays as scenery */
                    const Event *ev = events_roll_space(system_info());
                    if (ev) {
                        ui_event_open(ev);
                        s_event_return = EVRET_FLIGHT;
                        s_state = ST_EVENT;
                    } else {
                        snprintf(s_scoop_toast, sizeof s_scoop_toast,
                                 "HULK ALREADY STRIPPED");
                        s_scoop_toast_t = 2.5f;
                    }
                }
            }
        }
        /* Distress rescue: wing dead, victim alive -> hail + reward. */
        if (s_distress_civ > 0 && !s_distress_paid) {
            Ship *cv = &g_ships[s_distress_civ];
            if (!cv->alive) {
                s_distress_civ = -1;
                s_distress_done |= 1u << (s_anchor_poi.index & 31);
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "THE VICTIM IS LOST");
                s_scoop_toast_t = 3.0f;
            } else if (ships_alive_hostile() == 0) {
                s_distress_paid = true;
                s_distress_done |= 1u << (s_anchor_poi.index & 31);
                const SystemInfo *si3 = system_info();
                int pay = 250 + (int)si3->threat * 300;
                g_player.credits += pay;
                extern void mission_rep_add_public(int fac, int amt);
                mission_rep_add_public(
                    (int)system_faction(si3->addr), 2);
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "\"THANK YOU CMDR\" +%dCR REP+", pay);
                s_scoop_toast_t = 5.0f;
                sfx_lock_acquire();
            }
        }
        /* Rank-up fanfare. */
        {
            static const char *last_rank;
            const char *r = elite_rank_name(combat_kills());
            if (last_rank && r != last_rank) {
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "RANK: %s", r);
                s_scoop_toast_t = 3.0f;
                sfx_lock_acquire();
            }
            last_rank = r;
        }
    } else {
        if (!s_dead_latch) {
            s_dead_latch = true;
            s_respawn_t = 1.5f;
            /* KILL REPORT (user req): who got you, in full. */
            s_kr.valid = 0;
            int kk = combat_pkiller();
            s_kr.env = combat_pkiller_env();
            if (s_kr.env == 0 && kk > 0 && kk < MAX_SHIPS) {
                Ship *k = &g_ships[kk];
                s_kr.valid = 1;
                s_kr.tier = k->tier > 4 ? 4 : k->tier;
                s_kr.cls = k->cls;
                s_kr.nw = k->n_weapons > 3 ? 3 : k->n_weapons;
                for (int i = 0; i < s_kr.nw; i++)
                    s_kr.wpn[i] = k->weapons[i];
                s_kr.shv = k->shield_var;
                s_kr.arv = k->armor_var;
                s_kr.chaff = k->chaff_n;
                s_kr.turret = k->turret_type;
                s_kr.spd = k->max_speed;
                s_kr.trn = k->turn_rate;
                s_kr.hull = k->hull_max;
                s_kr.shd = k->shield_max;
                s_kr.police = k->is_police;
                s_kr.wfac = k->war_fac;
            }
        }
        s_respawn_t -= dt;
        if (s_respawn_t <= 0.0f && btn->a) {
            /* LAPSED (campaign ending C): the policy was surrendered —
             * there is no re-issue. The save burns with the ship. */
            if (events_flag(28)) {
                save_wipe();
                /* full re-init: back to the title, no CONTINUE — the
                 * run is over and the galaxy never reloads this pilot */
                elite_game_init(s_boot_seed);
                return;
            }
            /* Insurance: revert to the last dock save (journey since is
             * lost). No save yet -> fresh hull at the local beacon. */
            SaveMeta meta;
            if (save_exists() &&
                save_matches_galaxy(galaxy_get_seed()) &&
                save_load(&meta)) {
                combat_set_kills(meta.kills);
                arrive_docked(&meta);
            } else {
                spawn_player();
                Poi pois[MAX_POIS];
                system_pois(pois, MAX_POIS);
                drop_anchor(pois[0].pos_mm, &pois[0]);
                p->pos = v3(0, 0, -700.0f);
                spawn_poi_content();
            }
        }
    }

    flight_tick(dt);
    ai_tick(dt);
    combat_tick(dt);
    combat_crit_cooldown_tick(dt);
    fx_tick(dt);
    rocks_tick(dt);
    /* Collisions (user spec): everything deflects; shields block hull
     * damage; size sets the split; station bites only in MANUAL flight
     * near an anchored station. */
    collide_tick(s_anchor_has_poi && s_anchor_poi.kind == POI_STATION &&
                     s_station_mesh != NULL,
                 s_station_mesh ? s_station_mesh->bound_r : 0.0f,
                 s_state == ST_FLIGHT);

    /* Overheat klaxon (repeats while hot) + throttle-following hum. */
    {
        static float klaxon_t;
        klaxon_t -= dt;
        if (p->alive && p->heat > 88.0f && klaxon_t <= 0.0f) {
            sfx_klaxon();
            klaxon_t = 0.7f;
        }
        audio_engine_set(p->throttle,
                         v3_len(p->vel) / (p->max_speed * 1.2f));
    }
    {
        int pay = combat_take_kill_pay();
        if (pay > 0) {
            snprintf(s_scoop_toast, sizeof s_scoop_toast,
                     "BOUNTY %dCR", pay);
            s_scoop_toast_t = 1.6f;
        }
    }
    {
        const char *scooped = loot_tick(dt);
        if (scooped) {
            snprintf(s_scoop_toast, sizeof s_scoop_toast, "%s", scooped);
            s_scoop_toast_t = 2.0f;
        }
        if (s_scoop_toast_t > 0) s_scoop_toast_t -= dt;
    }

    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *s = &g_ships[i];
        if (!s->alive) continue;
        Vec3 rear = v3_sub(s->pos, v3_scale(s->basis.r[2],
                                            s->mesh->bound_r * 0.8f));
        fx_engine_trail(rear, s->vel, s->throttle, dt);
    }
    if (s_target >= 0 && !g_ships[s_target].alive) s_target = -1;
}

static void tick_supercruise(const CraftRawButtons *btn, float dt) {
    /* Star proximity HEAT — for everyone, scoop or not. Builds faster
     * than dissipation inside ~6 star radii (the old 9/s build lost to
     * the 12/s passive cooling and never registered — user-caught);
     * past redline the hull itself starts to burn. Sun-skimming is a
     * real risk now, exactly like the old game. */
    {
        const SystemInfo *si = system_info();
        float d = v3_len(s_sc_pos_mm);
        float hot_r = si->star_radius_mm * 6.0f;
        if (d < hot_r) {
            float k = 1.0f - d / hot_r;               /* 0 edge..1 core */
            g_ships[PLAYER].heat += (14.0f + 38.0f * k) * dt;
            if (g_ships[PLAYER].heat > 100.0f) {
                g_ships[PLAYER].heat = 100.0f + 0.0f;
                g_ships[PLAYER].hull -= 5.0f * dt;     /* burning */
                combat_note_env_hit(3);                /* the star */
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "HULL BURNING!");
                s_scoop_toast_t = 0.5f;
                if (g_ships[PLAYER].hull <= 0.0f) {
                    g_ships[PLAYER].alive = false;     /* flew too close */
                    g_ships[PLAYER].hull = 0.0f;
                }
            }
        }
        /* FUELSCOOP: free fuel while inside the heat zone. */
        if (player_has_util(EQ_FUELSCOOP) &&
            d < si->star_radius_mm * 4.5f &&
            g_player.fuel < g_player.fuel_max) {
            g_player.fuel += 1.6f * dt;
            if (g_player.fuel > g_player.fuel_max)
                g_player.fuel = g_player.fuel_max;
            if (((int)(s_time * 2.0f) & 1) == 0 &&
                g_ships[PLAYER].heat < 95.0f) {
                snprintf(s_scoop_toast, sizeof s_scoop_toast,
                         "SCOOPING %d.%d LY", (int)g_player.fuel,
                         ((int)(g_player.fuel * 10)) % 10);
                s_scoop_toast_t = 0.6f;
            }
        }
    }
    {
        /* Drive drone follows cruise speed (lower band than flight). */
        float k = s_sc_speed * (1.0f / 3000.0f);
        if (k > 1.0f) k = 1.0f;
        audio_engine_set(k * 0.55f, k);
    }
    FlightInput in;
    elite_input_update(btn, dt, &in);
    Ship *p = &g_ships[PLAYER];

    float tr = p->turn_rate * 0.6f * dt;     /* heavier helm at SC speed */
    if (in.pitch != 0.0f) m3_rotate_local(&p->basis, 0, in.pitch * tr);
    if (in.yaw   != 0.0f) m3_rotate_local(&p->basis, 1, in.yaw * tr);
    if (in.roll  != 0.0f) m3_rotate_local(&p->basis, 2, in.roll * tr * 1.5f);
    m3_orthonormalize(&p->basis);
    p->throttle += in.throttle_delta * 0.9f * dt;
    if (p->throttle < 0.0f) p->throttle = 0.0f;
    if (p->throttle > 1.0f) p->throttle = 1.0f;

    /* Speed envelope: approach-limited near the destination so arrival
     * is automatic, opening to 500 Mm/s in deep space. Planets keep a
     * standoff so we don't cruise into the lithosphere. */
    float standoff = 0.0f;
    if (s_sc_has_dest && s_sc_dest.kind == POI_PLANET) {
        /* AFFINE standoff (user bug: 'all planets look the same size').
         * The old radius*3.5 made apparent size constant by
         * construction — r/(3.5r) cancels. The affine form keeps the
         * lithosphere guard while letting true size show: small worlds
         * read small, giants loom. Pushed further on average (user:
         * 'a bit further away') without flattening the variation. */
        float r = system_info()->planets[s_sc_dest.index].radius_mm;
        standoff = r * 2.4f + 18.0f;
    }
    /* 12,000 cap + 0.6 slope (user: 42s end-to-end too long — worst
     * diameter now ~23s, short hops keep a ~5s journey feel; ODE-sim
     * picked). */
    float vmax = 12000.0f;
    float dist = 1e9f;
    if (s_sc_has_dest) {
        dist = v3_len(v3_sub(s_sc_dest.pos_mm, s_sc_pos_mm));
        float eff = dist - standoff;
        if (eff < 0.0f) eff = 0.0f;
        float lim = eff * 0.6f + 2.0f;
        if (lim < vmax) vmax = lim;
    }
    float want = p->throttle * vmax;
    /* Smooth speed chase. */
    s_sc_speed += (want - s_sc_speed) * (dt * 2.0f > 1 ? 1 : dt * 2.0f);

    /* Envelope-aware ETA: cruise at the cap, then the exponential
     * approach (v = eff/2 + 2 -> t = 2*ln ratio), scaled by throttle. */
    if (s_sc_has_dest) {
        float thr = p->throttle < 0.05f ? 0.05f : p->throttle;
        float eff = dist - standoff;
        if (eff < 0.0f) eff = 0.0f;
        float vcap = 12000.0f * thr;
        float eff_decay = (vcap - 2.0f) / 0.6f;
        if (eff_decay < 1.0f) eff_decay = 1.0f;
        float t = 0.0f;
        float e0 = eff;
        if (e0 > eff_decay) {
            t += (e0 - eff_decay) / vcap;
            e0 = eff_decay;
        }
        t += (1.0f / (0.6f * thr)) * logf((0.6f * e0 + 2.0f) / 2.6f);
        if (t < 0) t = 0;
        s_sc_eta = t + 1.0f;          /* spool-up allowance */
    } else {
        s_sc_eta = 0;
    }
    s_sc_pos_mm = v3_add(s_sc_pos_mm,
                         v3_scale(p->basis.r[2], s_sc_speed * dt));

    /* Arrival / manual drop. */
    bool arrived = s_sc_has_dest && dist < standoff + SC_DROP_MM;
    if (arrived || in.secondary) {
        Vec3 drop_mm = s_sc_pos_mm;
        const Poi *poi = NULL;
        if (arrived) {
            /* Drop at the standoff point on our approach line. */
            Vec3 in_dir = v3_norm(v3_sub(s_sc_dest.pos_mm, s_sc_pos_mm));
            drop_mm = v3_sub(s_sc_dest.pos_mm, v3_scale(in_dir, standoff));
            poi = &s_sc_dest;
        }
        drop_anchor(drop_mm, poi);
        Ship *pl = &g_ships[PLAYER];
        if (arrived) {
            /* Place the ship short of the POI, nose on it (closer for
             * man-made structures so they fill some screen). */
            float back = (s_sc_dest.kind == POI_PLANET) ? 900.0f : 350.0f;
            Vec3 in_dir = v3_norm(v3_sub(s_sc_dest.pos_mm, s_sc_pos_mm));
            pl->pos = v3_scale(in_dir, -back);
        } else {
            pl->pos = v3(0, 0, 0);
        }
        pl->vel = v3_scale(pl->basis.r[2], 60.0f);
        pl->throttle = 0.5f;
        s_state = ST_FLIGHT;
        spawn_poi_content();
        try_arrival_hail();
    }
}

static void tick_hyperjump(float dt) {
    s_hyper_t += dt;
    if (s_hyper_t >= HYPER_TIME) {
        g_player.fuel -= s_jump_dist;
        if (g_player.fuel < 0) g_player.fuel = 0;
        arrive_in_system(s_jump_target);
        r3d_starfield_init((uint32_t)(system_info()->seed >> 16));
        g_player.xp_piloting += 2;
        elite_input_reset();
        s_state = ST_FLIGHT;
        try_arrival_hail();          /* may flip to ST_EVENT */
    }
}

/* Spawn one title combatant: a lawful Viper (police) or a pirate, off to one
 * side of the brawl so the two sides close on each other. */
static void title_spawn_fighter(uint32_t s, bool police) {
    float a = (float)(s & 0xFFFF) / 65535.0f * 6.2831853f;
    float r = 35.0f + (float)((s >> 16) & 0xFF) / 255.0f * 55.0f;
    /* Around the brawl centre, the two factions offset to opposite sides. */
    Vec3 pos = v3_add(s_title_ctr,
                v3_add(v3_scale(s_title_perp, police ? 80.0f : -80.0f),
                       v3(cosf(a) * r,
                          (float)((int)((s >> 8) & 0xFF) - 128) * 0.5f,
                          sinf(a) * r)));
    int cls = police ? 4 : 2 + (int)((s >> 24) % 3u);
    int idx = ship_spawn(hull_mesh(s ^ (police ? 0x10Eu : 0xB0Au), cls),
                         pos, police ? TEAM_NEUTRAL : TEAM_HOSTILE);
    if (idx > 0) {
        ship_set_tier(idx, 2 + (int)((s >> 5) % 3u), cls);
        if (police) g_ships[idx].is_police = 1;
    }
}

/* Title screen runs a real police-vs-pirate brawl behind the wordmark, flown
 * entirely by the standard combat AI. The camera ship is a neutral spectator
 * (kept alive so the title never drops to the death screen) that slowly tracks
 * the centre of the action; the combatants are mortal and explode for real. */
static void title_battle_tick(float dt) {
    Ship *p = &g_ships[PLAYER];
    p->team = TEAM_NEUTRAL; p->alive = true;
    p->shield = p->shield_max; p->hull = p->hull_max;   /* the camera, not a fighter */
    p->throttle = 0.0f; p->vel = v3(0, 0, 0);

    static uint32_t sp = 0x1234abcdu;
    int npir = 0, npol = 0, pir[MAX_SHIPS], pol[MAX_SHIPS];
    for (int pass = 0; pass < 2; pass++) {
        npir = npol = 0;
        for (int i = 1; i < MAX_SHIPS; i++) {
            if (!g_ships[i].alive) continue;
            if (g_ships[i].team == TEAM_HOSTILE)        pir[npir++] = i;
            else if (g_ships[i].is_police)              pol[npol++] = i;
        }
        if (pass == 0) {   /* keep both sides stocked so the brawl never ends */
            while (npir < 4) { sp = sp*1664525u+1013904223u; title_spawn_fighter(sp, false); npir++; }
            while (npol < 4) { sp = sp*1664525u+1013904223u; title_spawn_fighter(sp, true);  npol++; }
        }
    }
    /* Pirates fight the law (the camera is neutral); police acquire pirates on
     * their own. Re-point a pirate only when its quarry is gone. */
    for (int a = 0; a < npir && npol > 0; a++) {
        Ship *s = &g_ships[pir[a]];
        int c = s->ai_target;
        if (!(c > 0 && c < MAX_SHIPS && g_ships[c].alive && g_ships[c].is_police)) {
            int bj = -1; float bd = 1e30f;
            for (int b = 0; b < npol; b++) {
                float d = v3_len2(v3_sub(g_ships[pol[b]].pos, s->pos));
                if (d < bd) { bd = d; bj = pol[b]; }
            }
            if (bj > 0) s->ai_target = (uint8_t)bj;
        }
    }
    /* Camera slowly swings to keep the action centred. */
    Vec3 ctr = v3(0, 0, 0); int cn = 0;
    for (int a = 0; a < npir; a++) { ctr = v3_add(ctr, g_ships[pir[a]].pos); cn++; }
    for (int a = 0; a < npol; a++) { ctr = v3_add(ctr, g_ships[pol[a]].pos); cn++; }
    if (cn) {
        ctr = v3_scale(ctr, 1.0f / cn);
        Vec3 want = v3_norm(v3_sub(ctr, p->pos));
        p->basis.r[2] = v3_norm(v3_lerp(p->basis.r[2], want, 0.8f * dt));
        Vec3 up0 = v3(0, 1, 0);
        p->basis.r[0] = v3_norm(v3_cross(up0, p->basis.r[2]));
        p->basis.r[1] = v3_cross(p->basis.r[2], p->basis.r[0]);
    }
    ai_tick(dt);
    combat_tick(dt);
    combat_crit_cooldown_tick(dt);
    fx_tick(dt);
    collide_tick(false, 0.0f, false);
    flight_tick(dt);
    /* Engine streams for every fighter (tick_flight does this in real flight). */
    for (int i = 1; i < MAX_SHIPS; i++) {
        Ship *s = &g_ships[i];
        if (!s->alive) continue;
        Vec3 rear = v3_sub(s->pos, v3_scale(s->basis.r[2], s->mesh->bound_r * 0.8f));
        fx_engine_trail(rear, s->vel, s->throttle, dt);
    }
}

/* Dock-arrival services: runs straight after the docking glide, or after
 * the arrival-hail modal closes (its outcome lands in the same save). */
static void dock_finish(void) {
    station_open(s_anchor_poi.index);
    mission_on_docked(system_info(), s_anchor_poi.index);
    int paid = mission_collect(system_info(), s_anchor_poi.index);
    if (paid > 0) {
        char buf[24];
        snprintf(buf, sizeof buf, "MISSION PAY %dCR", paid);
        station_toast(buf);
    }
    /* Deferred event transfers land here (OP_LATER — favours repaid). */
    int32_t owed = events_pending_take();
    if (owed > 0) {
        g_player.credits += owed;
        char buf[24];
        snprintf(buf, sizeof buf, "TRANSFER +%dCR", (int)owed);
        station_toast(buf);
    }
    save_dock_write();
    s_state = ST_DOCKED;
}

/* events.c OP_AMBUSH: a wing takes position outside the station — the
 * world is frozen while docked, so they're waiting at launch. */
void elite_game_event_ambush(int n, int tier) {
    const SystemInfo *si = system_info();
    if (n > 4) n = 4;
    if (tier < 0) tier = 0;
    if (tier > 4) tier = 4;
    static const uint8_t k_cls[5] = { 1, 2, 3, 4, 5 };
    for (int i = 0; i < n; i++) {
        float a = frand(0, 6.2831f);
        float r = frand(700, 1100);
        Vec3 pos = v3(cosf(a) * r, frand(-150, 150), sinf(a) * r);
        int cls = k_cls[tier];
        uint32_t mseed = (uint32_t)(si->seed >> 24) ^
                         (uint32_t)(cls * 0x9E3779B9u) ^ 0xA3B5u;
        int idx = ship_spawn(hull_mesh(mseed, cls), pos, TEAM_HOSTILE);
        if (idx > 0) ship_set_tier(idx, tier, cls);
    }
}

void elite_game_tick(const CraftRawButtons *btn, float dt) {
    if (dt > 1e-4f)
        s_fps += (1.0f / dt - s_fps) * 0.08f;     /* smoothed FPS */
    s_time += dt;                                 /* real-time animation clock */
    bool menu_edge = btn->menu && !s_prev_menu;
    s_prev_menu = btn->menu;
    bool a_edge = btn->a && !s_prev_a;
    s_prev_a = btn->a;
#ifdef MOTE_HOST
    { static int dvd=0; if (!dvd && getenv("MOTE_IR_DOCK") && s_state==ST_FLIGHT){   /* test: jump to a docked station */
        Poi pois[MAX_POIS]; int n=system_pois(pois,MAX_POIS);
        for (int i=0;i<n;i++) if(pois[i].kind==POI_STATION){
            drop_anchor(pois[i].pos_mm,&pois[i]); spawn_player(); station_open(pois[i].index); s_state=ST_DOCKED; break; }
        dvd=1; } }
#endif

    /* Engine hum only exists in flight states; everything else mutes
     * it (prevents any residual tone on title/menus). */
    if (s_state != ST_FLIGHT && s_state != ST_SUPERCRUISE &&
        !(s_state == ST_DASH && s_dash_anim < 1.0f))
        audio_engine_set(0, 0);

    /* Only ST_FLIGHT/ST_SUPERCRUISE read the analog stick to fly. Every
     * other state (dashboard, maps, status, docked, title) ticks with a
     * neutral button struct — so wipe the shell-fed analog here too, or a
     * held stick / HOTAS throttle would keep steering the ship in menus. */
    if (s_state != ST_FLIGHT && s_state != ST_SUPERCRUISE)
        elite_input_neutralize();

    switch (s_state) {
    case ST_TITLE: {
        if (s_intro_active) {                       /* NEW GAME lore crawl */
            s_intro_t += dt;
            if (a_edge || menu_edge || s_intro_t >= intro_duration()) {
                s_intro_active = false;
                start_new_game(s_boot_seed);        /* crawl done -> begin */
                break;            /* do NOT tick the title brawl into the new game */
            }
            title_battle_tick(dt);
            break;
        }
        bool has_save = (s_savelist_n > 0);
        /* Items: 0 CONTINUE · 1 NEW GAME · 2 PVP ARENA · (cheats) 3.. scenarios. */
        int title_max = s_cheat_on ? (2 + N_SCENARIOS) : 2;
        static bool tu, td, trb;
        if (btn->up && !tu && s_title_cursor > 0) { s_title_cursor--; sfx_ui_move(); }
        if (btn->down && !td && s_title_cursor < title_max) { s_title_cursor++; sfx_ui_move(); }
        if (btn->rb && !trb) {
#if ELITE_CHEATS
            if (++s_cheat_taps >= 10) {
                s_cheat_taps = 0;
                s_cheat_on = !s_cheat_on;       /* reveal/hide the scenario picker */
                if (!s_cheat_on && s_title_cursor > 1) s_title_cursor = 1;
                sfx_lock_acquire();             /* audible confirmation */
            } else {
                sfx_ui_move();
            }
#endif
        }
        tu = btn->up; td = btn->down; trb = btn->rb;
        if (a_edge) {
            /* No chime on game start (user pref) — straight in. */
            if (s_title_cursor == 0 && has_save) {
                save_rebuild_list();            /* CONTINUE -> the save picker */
                s_sel_cursor = 0; s_sel_scroll = 0; s_sel_confirm_del = false;
                s_state = ST_SAVESEL;
                sfx_ui_select();
                break;
            }
            if (s_title_cursor == 2) {          /* PVP: LINK ARENA */
                save_rebuild_list();
                if (s_savelist_n > 0) {         /* have saves: pick which ship to bring */
                    s_pvp_select = true;
                    s_sel_cursor = 0; s_sel_scroll = 0; s_sel_confirm_del = false;
                    s_state = ST_SAVESEL;
                    sfx_ui_select();
                } else if (pvp_begin()) {       /* no save: random fit, straight to the lobby */
                    s_state = ST_PVPWAIT;
                    sfx_ui_select();
                }
                break;
            }
#if ELITE_CHEATS
            if (s_title_cursor >= 3) {           /* a test scenario — straight in */
                s_cheat_scenario = s_title_cursor - 2;   /* 1..N */
                start_new_game(s_boot_seed);
                break;
            }
            s_cheat_scenario = 0;
#endif
            s_intro_active = true;          /* NEW GAME: play the lore crawl first */
            s_intro_t = 0.0f;
            break;
        }
        title_battle_tick(dt);          /* live brawl behind the wordmark */
        break;
    }

    case ST_SAVESEL: {
        static bool su, sd, sb, slb;
        bool up = btn->up && !su, dn = btn->down && !sd;
        bool bk = btn->b && !sb, del = btn->lb && !slb;
        su = btn->up; sd = btn->down; sb = btn->b; slb = btn->lb;
        if (s_sel_confirm_del) {                 /* "are you sure?" overlay */
            if (a_edge && s_sel_cursor < s_savelist_n) {
                save_delete(s_savelist[s_sel_cursor].slot);
                save_rebuild_list();
                if (s_sel_cursor >= s_savelist_n) s_sel_cursor = s_savelist_n - 1;
                if (s_sel_cursor < 0) s_sel_cursor = 0;
                s_sel_confirm_del = false; sfx_ui_select();
            } else if (bk) { s_sel_confirm_del = false; sfx_ui_move(); }
            break;
        }
        if (s_savelist_n == 0) { if (bk || a_edge) { s_pvp_select = false; s_state = ST_TITLE; } break; }
        if (up && s_sel_cursor > 0) { s_sel_cursor--; sfx_ui_move(); }
        if (dn && s_sel_cursor < s_savelist_n - 1) { s_sel_cursor++; sfx_ui_move(); }
        if (del && !s_pvp_select) { s_sel_confirm_del = true; sfx_ui_move(); break; }  /* no deleting mid-duel-pick */
        if (bk) { s_pvp_select = false; s_state = ST_TITLE; sfx_ui_move(); break; }
        if (a_edge) {
            int slot = s_savelist[s_sel_cursor].slot;
            save_set_slot(slot); s_save_slot = slot;
            if (s_pvp_select) {                       /* PVP: bring THIS saved ship to the duel */
                s_pvp_select = false;
                pvp_set_slot(slot);
                if (pvp_begin()) { s_state = ST_PVPWAIT; sfx_ui_select(); }
                else s_state = ST_TITLE;
                break;
            }
            SaveMeta meta;
            if (save_load(&meta)) { combat_set_kills(meta.kills); arrive_docked(&meta); }
            break;
        }
        break;
    }

    case ST_PVPWAIT: {
        /* PVP: pump the handshake; the arena is built inside pvp_wait_tick. */
        int r = pvp_wait_tick(btn, dt);
        if (r == PVP_START) s_state = ST_FLIGHT;           /* fight! (the ONLY path to FLIGHT) */
        else if (r == PVP_CANCEL) { s_state = ST_TITLE; save_rebuild_list(); }
        break;
    }

    case ST_FLIGHT:
        if (pvp_active()) {
            /* PVP: the LINK ARENA reuses the flight camera/HUD but runs its
             * own sim (no dash, no docking, no hyperspace). */
            if (pvp_arena_tick(btn, dt) == PVP_EXIT) {
                pvp_end();
                s_state = ST_TITLE;
                save_rebuild_list();
            }
            break;
        }
        if (menu_edge) {
            s_state = ST_DASH;
            s_dash_from = ST_FLIGHT;
            s_dash_sel = 0;
            s_dash_anim = 0;
            s_dash_closing = false;
            s_in_settings = false;
            break;
        }
        tick_flight(btn, dt);
        break;

    case ST_SUPERCRUISE:
        if (menu_edge) {
            s_state = ST_DASH;
            s_dash_from = ST_SUPERCRUISE;
            s_dash_sel = 0;
            s_dash_anim = 1.0f;          /* SC: no slide, straight in */
            s_dash_closing = false;
            s_in_settings = false;
            break;
        }
        tick_supercruise(btn, dt);
        if (!g_ships[PLAYER].alive) {
            /* Burned up at the star: drop to flight, whose death path
             * runs the insurance respawn. */
            fx_spawn_explosion(g_ships[PLAYER].pos, v3(0, 0, 0));
            s_state = ST_FLIGHT;
        }
        break;

    case ST_HYPERJUMP:
        tick_hyperjump(dt);
        break;

    case ST_DOCKING: {
        /* Docking computer: glide to the bay mouth, then services. */
        s_dock_t += dt;
        Ship *p = &g_ships[PLAYER];
        Vec3 bay = v3(0, 0, 0);
        float k = dt * 1.4f;
        if (k > 1) k = 1;
        p->pos = v3_lerp(p->pos, bay, k);
        p->vel = v3(0, 0, 0);
        /* Ease the nose onto the station. */
        Vec3 want = v3_norm(v3_sub(bay, v3_len2(p->pos) > 1 ? p->pos
                                                            : v3(0, 0, -1)));
        p->basis.r[2] = v3_norm(v3_lerp(p->basis.r[2], want, k));
        m3_orthonormalize(&p->basis);
        fx_tick(dt);
        if (s_dock_t >= 2.2f) {
            g_player.xp_piloting += 1;
            plat_rumble(0.4f, 0.12f);
            /* Arrival hail: most docks are quiet; when one fires, the
             * modal runs FIRST and dock_finish() (services + save)
             * happens after the choice — so the outcome is banked. */
            const Event *ev =
                events_roll_dock(system_info(), s_anchor_poi.index);
            if (ev) {
                ui_event_open(ev);
                s_event_return = EVRET_DOCK_FINISH;
                s_state = ST_EVENT;
                break;
            }
            dock_finish();
        }
        break;
    }

    case ST_EVENT:
        audio_engine_set(0, 0);
        if (ui_event_tick(btn, dt)) {
            switch (s_event_return) {
            case EVRET_DOCKED:
                /* A campaign ending just resolved at dock -> congratulate
                 * once before returning to the station. */
                if (maybe_open_win(EVRET_DOCKED)) break;
                /* Bar encounter: bank the outcome, back to the station
                 * (services stay open — no dock_finish re-run). */
                save_dock_write();
                s_state = ST_DOCKED;
                break;
            case EVRET_FLIGHT:
                /* In-space: straight back to the stick. No save out
                 * here — the next dock banks it, same as combat loot. */
                elite_input_reset();
                s_state = ST_FLIGHT;
                if (s_recall_spawn) {        /* the recall hail just closed */
                    s_recall_spawn = false;
                    spawn_adjuster_recall();
                }
                break;
            default:
                dock_finish();
            }
        }
        break;

    case ST_DOCKED: {
        audio_engine_set(0, 0);
        DockAction act = station_tick(btn, dt);
        if (act == DOCK_EVENT) {
            const Event *ev = station_pending_event();
            if (ev) {
                ui_event_open(ev);
                s_event_return = EVRET_DOCKED;
                s_state = ST_EVENT;
            }
            break;
        }
        if (act == DOCK_LAUNCH) {
            /* Save on the way OUT too (user: otherwise you re-dock just
             * to bank a good purchase). */
            save_dock_write();
            /* Emerge from the bay face (station +z, rotated by its spin),
             * nose out, gentle drift. */
            Ship *p = &g_ships[PLAYER];
            float yaw = s_time * 0.05f;
            Vec3 out = v3(sinf(yaw), 0, cosf(yaw));
            p->pos = v3_scale(out, 320.0f);
            p->basis.r[2] = out;
            p->basis.r[1] = v3(0, 1, 0);
            p->basis.r[0] = v3_norm(v3_cross(p->basis.r[1], out));
            p->basis.r[1] = v3_cross(p->basis.r[2], p->basis.r[0]);
            p->vel = v3_scale(out, 25.0f);
            p->throttle = 0.25f;
            p->shield = p->shield_max;     /* station services top you up */
            p->heat = 0;
            elite_input_reset();
            s_state = ST_FLIGHT;
        }
        break;
    }

    case ST_STATUS:
        if (s_menus_live) {
            CraftRawButtons none3 = {0};
            if (s_dash_from == ST_SUPERCRUISE) tick_supercruise(&none3, dt);
            else tick_flight(&none3, dt);
            if (!g_ships[PLAYER].alive) { s_state = s_dash_from; break; }
        }
        if (status_tick(btn, dt)) {
            elite_input_reset();
            s_state = s_menus_live ? ST_DASH : ST_FLIGHT;
        }
        break;

#ifdef ELITE_ANALOG_SETTINGS
    case ST_CTRLSETUP:
        /* Reached from the dash SETTINGS, so the world is live underneath. */
        {
            CraftRawButtons none4 = {0};
            tick_flight(&none4, dt);
            if (!g_ships[PLAYER].alive) { s_state = s_dash_from; break; }
        }
        if (ctrlsetup_tick(btn, dt)) {
            elite_input_reset();
            s_settings_eat_b = true; /* don't let the closing B exit SETTINGS */
            s_state = ST_DASH;       /* back to the SETTINGS overlay */
        }
        break;
#endif

    case ST_DASH: {
        /* THE GAME PLAYS ON. Neutral stick; peril remains — taking
         * hull damage kicks you back to the cockpit. */
        CraftRawButtons none2 = {0};
        if (s_dash_from == ST_SUPERCRUISE)
            tick_supercruise(&none2, dt);
        else
            tick_flight(&none2, dt);
        if (!g_ships[PLAYER].alive) {
            s_state = (uint8_t)s_dash_from;
            break;
        }
        if (s_dash_closing) {
            /* Slide back down, then hand the stick back (user req: no
             * teleporting console). MENU mid-close reopens. */
            if (menu_edge) { s_dash_closing = false; break; }
            s_dash_anim -= dt * 4.0f;
            if (s_dash_anim <= 0.0f) {
                s_dash_anim = 0;
                s_dash_closing = false;
                elite_input_reset();
                s_state = (uint8_t)s_dash_from;
            }
            break;
        }
        if (s_dash_anim < 1.0f) {
            s_dash_anim += dt * 4.0f;
            if (s_dash_anim > 1.0f) s_dash_anim = 1.0f;
        }
        if (s_in_settings) {
            static bool pu2, pd2, pb2, pl3, pr3;
            if (btn->up && !pu2 && s_settings_cursor > 0)
                s_settings_cursor--;
            if (btn->down && !pd2 && s_settings_cursor < settings_rows() - 1)
                s_settings_cursor++;
            pu2 = btn->up; pd2 = btn->down;
            int dir = 0;
            if (btn->right && !pr3) dir = 1;
            if (btn->left && !pl3) dir = -1;
            pl3 = btn->left; pr3 = btn->right;
            if (a_edge) {
                if (s_settings_cursor == 0)
                    g_player.invert_y = !g_player.invert_y;
                else if (s_settings_cursor == 1)
                    g_player.show_fps = !g_player.show_fps;
                else if (s_settings_cursor == ROW_DIFF) {
                    g_player.difficulty = (uint8_t)((g_player.difficulty + 1) % 3);
                    sfx_ui_select();
                }
#ifdef ELITE_ANALOG_SETTINGS
                else if (s_settings_cursor == ROW_CTRL) {   /* CONTROLLER row */
                    ctrlsetup_open();
                    s_state = ST_CTRLSETUP;
                    sfx_ui_select();
                    break;
                }
#endif
                else
                    dir = 1;                 /* A nudges sliders / cycles input */
            }
            if (dir && s_settings_cursor == 2) {
                int v = plat_setting_get(0) + dir * 2;     /* 0..20 */
                if (v < 0) v = 0;
                if (v > 20) v = 20;
                plat_setting_set(0, v);
                sfx_ui_move();
            } else if (dir && s_settings_cursor == 3) {
                int b2 = plat_setting_get(1) + dir * 32;   /* 0..255 */
                if (b2 < 31) b2 = 31;        /* never fully dark */
                if (b2 > 255) b2 = 255;
                plat_setting_set(1, b2);
            } else if (dir && s_settings_cursor == ROW_DIFF) {
                int nd = g_player.difficulty + (dir > 0 ? 1 : 2);   /* wrap 3 */
                g_player.difficulty = (uint8_t)(nd % 3);
                sfx_ui_move();
            } else if (dir && (s_settings_cursor == 5 || s_settings_cursor == 6)) {
                /* Controller / touch-stick sensitivity: int 3..20 = 0.3..2.0x.
                 * Shells multiply analog input by this; no effect on device. */
                int which = (s_settings_cursor == 5) ? 2 : 3;
                int s = plat_setting_get(which) + dir;
                if (s < 3) s = 3;
                if (s > 20) s = 20;
                plat_setting_set(which, s);
                sfx_ui_move();
            }
#ifdef ELITE_INPUT_SELECT
            else if (dir && s_settings_cursor == ROW_INPUT) {
                int n = plat_setting_get(4) + dir;   /* INPUT: AUTO/HOTAS/PAD/KBD */
                if (n < 0) n = 3;
                if (n > 3) n = 0;
                plat_setting_set(4, n);
                sfx_ui_move();
            } else if (dir && s_settings_cursor == ROW_FULL) {
                plat_setting_set(5, !plat_setting_get(5));   /* FULLSCREEN */
                sfx_ui_move();
            }
#endif
            bool b_back = btn->b && !pb2;
            if (s_settings_eat_b) {       /* swallow B held from a subscreen */
                if (!btn->b) s_settings_eat_b = false;
                b_back = false;
            }
            if (b_back || menu_edge) s_in_settings = false;
            pb2 = btn->b;
            break;
        }
        {
            static bool pl2, pr2, pu3, pd3, pb3;
            if (btn->left && !pl2) s_dash_sel &= ~1;
            if (btn->right && !pr2) s_dash_sel |= 1;
            if (btn->up && !pu3) s_dash_sel &= ~2;
            if (btn->down && !pd3) s_dash_sel |= 2;
            pl2 = btn->left; pr2 = btn->right;
            pu3 = btn->up; pd3 = btn->down;
            if (a_edge) {
                s_menus_live = true;
                if (s_dash_sel == 0) {
                    map_galaxy_open(s_addr, g_player.fuel,
                                    (k_hulls[g_player.hull_id].jump_range * player_roll()->jmp));
                    s_state = ST_GALAXY_MAP;
                } else if (s_dash_sel == 1) {
                    map_system_open(cam_pos_mm());
                    s_state = ST_SYSTEM_MAP;
                } else if (s_dash_sel == 2) {
                    status_open();
                    s_state = ST_STATUS;
                } else {
                    s_in_settings = true;
                    s_settings_cursor = 0;
                }
            }
            if ((btn->b && !pb3) || menu_edge)
                s_dash_closing = true;       /* animated exit */
            pb3 = btn->b;
        }
        break;
    }

    case ST_GALAXY_MAP: {
        if (s_menus_live) {
            CraftRawButtons none3 = {0};
            if (s_dash_from == ST_SUPERCRUISE) tick_supercruise(&none3, dt);
            else tick_flight(&none3, dt);
            if (!g_ships[PLAYER].alive) { s_state = s_dash_from; break; }
        }
        SysAddr target;
        float dist;
        MapAction act = map_galaxy_tick(btn, dt, &target, &dist);
        if (act == MAP_CLOSE) {
            elite_input_reset();
            s_state = s_menus_live ? ST_DASH : ST_FLIGHT;
        }
        else if (act == MAP_ENGAGE_JUMP) {
            sfx_jump();
            s_jump_target = target;
            s_jump_dist = dist;
            s_hyper_from_mm = v3_add(s_anchor_mm,
                                     v3_scale(g_ships[PLAYER].pos, 1.0e-6f));
            s_hyper_t = 0;
            s_hyper_seed = xorshift32();
            ships_despawn_npcs();
            s_menus_live = false;
            s_state = ST_HYPERJUMP;
        }
        break;
    }

    case ST_SYSTEM_MAP: {
        if (s_menus_live) {
            CraftRawButtons none3 = {0};
            if (s_dash_from == ST_SUPERCRUISE) tick_supercruise(&none3, dt);
            else tick_flight(&none3, dt);
            if (!g_ships[PLAYER].alive) { s_state = s_dash_from; break; }
        }
        Poi dest;
        MapAction act = map_system_tick(btn, dt, &dest);
        if (act == MAP_CLOSE) {
            elite_input_reset();
            s_state = s_menus_live ? ST_DASH : ST_FLIGHT;
        }
        else if (act == MAP_ENGAGE_SC) {
            s_menus_live = false;
            s_sc_dest = dest;
            s_sc_has_dest = true;
            s_sc_pos_mm = cam_pos_mm();
            s_sc_speed = 0.01f;
            ships_despawn_npcs();
            fx_init();
            /* Auto-align the nose at the destination. */
            Ship *p = &g_ships[PLAYER];
            Vec3 fwd = v3_norm(v3_sub(dest.pos_mm, s_sc_pos_mm));
            Vec3 up0 = (fabsf(fwd.y) < 0.95f) ? v3(0, 1, 0) : v3(1, 0, 0);
            p->basis.r[2] = fwd;
            p->basis.r[0] = v3_norm(v3_cross(up0, fwd));
            p->basis.r[1] = v3_cross(fwd, p->basis.r[0]);
            p->throttle = 1.0f;     /* full burn by default */
            p->pos = v3(0, 0, 0);
            p->vel = v3(0, 0, 0);
            elite_input_reset();
            sfx_sc_engage();
            s_state = ST_SUPERCRUISE;
        }
        break;
    }
    }
}

/* --- rendering -----------------------------------------------------------*/
void elite_game_render_begin(void) {
    Ship *p = &g_ships[PLAYER];
    fx_sc_dust_off();   /* supercruise dust shows only on frames that re-emit it */

    switch (s_state) {

    case ST_HYPERJUMP: {
        /* The departure system stays on screen and RECEDES (user req):
         * a virtual camera accelerates exponentially along the nose, so
         * planets and the sun shrink away as the drive spools, then the
         * starline tunnel takes over. */
        Mat3 cam = p->basis;
        m3_rotate_local(&cam, 2, s_hyper_t * 0.4f);
        g_em->scene_begin(&cam, 60.0f); r3d_pipe_set_camera(&cam, 60.0f);
        float d_mm = 60.0f * (expf(s_hyper_t * 3.4f) - 1.0f);
        Vec3 vcam = v3_add(s_hyper_from_mm,
                           v3_scale(p->basis.r[2], d_mm));
        g_em->scene_set_sun(v3_norm(v3_scale(vcam, -1.0f)));
        r3d_planet_emit(vcam);
        break;
    }
    case ST_GALAXY_MAP:
    case ST_SYSTEM_MAP:
    case ST_EVENT:
    case ST_PVPWAIT:        /* PVP: bare starfield behind the link-wait text */
        /* Fullscreen UI: minimal empty scene (UI fills the band). */
        g_em->scene_begin(&p->basis, 60.0f); r3d_pipe_set_camera(&p->basis, 60.0f);
        break;

    case ST_DOCKED: {
        /* Starfield backdrop + rotating preview (station or shipyard
         * hull) in the right-hand pane the UI leaves open. */
        Mat3 cam = m3_identity();
        g_em->scene_begin(&cam, 60.0f); r3d_pipe_set_camera(&cam, 60.0f);
        g_em->scene_set_sun(v3(0.35f, 0.45f, -0.82f));   /* showroom light */
        uint32_t pv_seed;
        int pv_cls;
        int pv = station_preview2(&pv_seed, &pv_cls);
        if (pv == 3) {
            /* Hangar bay: your ship parked over a deck grid, dimmed by
             * the status sheet into a backdrop (user req). */
            MoteObject obj; obj.color = 0;
            obj.mesh = hull_mesh(g_player.hull_seed, g_player.hull_id);
            obj.basis = m3_identity();
            m3_rotate_local(&obj.basis, 1, s_time * 0.25f);
            m3_rotate_local(&obj.basis, 0, 0.22f);
            float dist = obj.mesh->bound_r * 2.4f;
            obj.pos = v3(0, 0, dist);
            g_em->scene_add_object(&obj);
            /* Deck grid under the ship. */
            float fy = -obj.mesh->bound_r * 1.05f;
            uint16_t gc = RGB565C(50, 70, 100);
            for (int k = -2; k <= 2; k++) {
                Vec3 a = v3(k * dist * 0.30f, fy, dist * 0.45f);
                Vec3 b = v3(k * dist * 0.30f, fy, dist * 1.8f);
                g_em->scene_add_line(a, b, gc);
                Vec3 c2 = v3(-dist * 0.62f, fy, dist * (0.6f + 0.3f * (k + 2)));
                Vec3 e2 = v3(dist * 0.62f, fy, dist * (0.6f + 0.3f * (k + 2)));
                g_em->scene_add_line(c2, e2, gc);
            }
        } else if (pv != 0) {
            const Mesh *m = (pv == 1)
                ? (s_station_mesh ? s_station_mesh : &mesh_station)
                : hull_mesh(pv_seed, pv_cls);
            MoteObject obj; obj.color = 0;
            obj.mesh = m;
            obj.basis = m3_identity();
            m3_rotate_local(&obj.basis, 1, s_time * 0.5f);
            m3_rotate_local(&obj.basis, 0, 0.30f);
            float dist = m->bound_r * 2.5f;
            if (pv == 2 && station_hull_detail_view()) {
                /* Detail sheet: tuck the hull into a small top-right box so the
                   spec column + bottom-right stats fill the rest of the screen. */
                dist = m->bound_r * 3.4f;                 /* larger — box is x63..127, y2..56 */
                obj.pos = v3(dist * 0.28f, dist * 0.32f, dist);  /* centred in that box */
            } else {
                obj.pos = v3(dist * 0.29f, 0, dist);
            }
            g_em->scene_add_object(&obj);
        }
        break;
    }

    case ST_STATUS: {
        /* Your ship turning gently in the sheet's top-right window. */
        Mat3 cam = m3_identity();
        g_em->scene_begin(&cam, 60.0f); r3d_pipe_set_camera(&cam, 60.0f);
        g_em->scene_set_sun(v3(0.35f, 0.45f, -0.82f));   /* showroom light */
        /* Centred backdrop, pulled back so the whole hull fits. */
        MoteObject obj; obj.color = 0;
        obj.mesh = hull_mesh(g_player.hull_seed, g_player.hull_id);
        obj.basis = m3_identity();
        m3_rotate_local(&obj.basis, 1, s_time * 0.5f);
        m3_rotate_local(&obj.basis, 0, 0.30f);
        float dist = obj.mesh->bound_r * 2.4f;
        obj.pos = v3(0, 0, dist);
        g_em->scene_add_object(&obj);
        break;
    }

    case ST_SAVESEL: {
        /* The highlighted save's ship, turning in the upper pane (its
         * "icon"); the stats list draws over the lower half. */
        Mat3 cam = m3_identity();
        g_em->scene_begin(&cam, 60.0f); r3d_pipe_set_camera(&cam, 60.0f);
        g_em->scene_set_sun(v3(0.35f, 0.45f, -0.82f));
        if (s_savelist_n > 0 && s_sel_cursor < s_savelist_n) {
            SavePeek *pk = &s_savelist[s_sel_cursor].pk;
            MoteObject obj; obj.color = 0;
            obj.mesh = hull_mesh(pk->hull_seed, pk->hull_id);
            obj.basis = m3_identity();
            m3_rotate_local(&obj.basis, 1, s_time * 0.6f);
            m3_rotate_local(&obj.basis, 0, 0.32f);
            float dist = obj.mesh->bound_r * 3.4f;     /* smaller -> sits in the pane */
            obj.pos = v3(0, obj.mesh->bound_r * 1.1f, dist);  /* lifted up */
            g_em->scene_add_object(&obj);
        }
        break;
    }

    case ST_SUPERCRUISE:
        g_em->scene_begin(&p->basis, 60.0f); r3d_pipe_set_camera(&p->basis, 60.0f);
        r3d_planet_emit(cam_pos_mm());
        fx_sc_dust_emit(s_sc_pos_mm,
                        v3_scale(p->basis.r[2], s_sc_speed));
        break;

    default: {   /* FLIGHT + PAUSE render the world */
        g_em->scene_begin(&p->basis, 60.0f); r3d_pipe_set_camera(&p->basis, 60.0f);
        /* Sunlight from the system star (camera-relative direction). */
        {
            Vec3 cm = cam_pos_mm();
            g_em->scene_set_sun(v3_norm(v3_scale(cm, -1.0f)));
        }
        r3d_planet_emit(cam_pos_mm());

        /* Anchored POI structure (station / beacon). */
        if (s_anchor_has_poi && s_anchor_poi.kind != POI_PLANET) {
            MoteObject obj; obj.color = 0;
            obj.mesh = (s_anchor_poi.kind == POI_STATION && s_station_mesh)
                           ? s_station_mesh : &mesh_beacon;
            obj.basis = m3_identity();
            /* Slow majestic spin. */
            m3_rotate_local(&obj.basis, 1, s_time * 0.05f);
            obj.pos = v3_sub(v3(0, 0, 0), p->pos);
            g_em->scene_add_object(&obj);
        }

        for (int i = 1; i < MAX_SHIPS; i++) {
            if (!g_ships[i].alive) continue;
            MoteObject obj; obj.color = 0;
            obj.mesh = g_ships[i].mesh;
            obj.basis = g_ships[i].basis;
            obj.pos = v3_sub(g_ships[i].pos, p->pos);
            g_em->scene_add_object(&obj);
        }
        loot_render(p->pos);
        rocks_render(p->pos, s_time);
        fx_emit_all(p->pos, p->vel);
        proj_emit(p->pos);
        break;
    }
    }
}

/* The engine now rasterises the scene (built in elite_game_render_begin) across
 * both cores — no game render_band. This stays only for link compatibility. */
void elite_game_render(uint16_t *fb, int y_min, int y_max) {
    (void)fb; (void)y_min; (void)y_max;
}

/* --- overlays ------------------------------------------------------------*/
static void draw_hyperjump_overlay(uint16_t *fb) {
    /* Z-space starlines: each star is a fixed direction with a cycling
     * depth; radius = F/z, so streaks are born dim near the centre and
     * accelerate past the edges — continuous, no banding, no popping.
     * Streak length = one frame of travel, stretched as speed builds. */
    float t = s_hyper_t;
    float spool = t < 0.6f ? t / 0.6f : 1.0f;
    for (int i = 0; i < 90; i++) {
        uint32_t h = s_hyper_seed ^ (uint32_t)(i * 2654435761u);
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        float ang = (float)(h & 0x3FF) * (6.2831853f / 1024.0f);
        float spd = 0.5f + (float)((h >> 10) & 0xFF) * (1.0f / 255.0f);
        float z0 = (float)((h >> 18) & 0x3FF) * (2.2f / 1024.0f);
        float zz = z0 - t * spd * (0.6f + spool * 1.4f);
        zz = zz - 2.2f * floorf(zz / 2.2f);          /* wrap 0..2.2 */
        zz += 0.10f;
        float dz = spd * (0.6f + spool * 1.4f) * 0.06f * (1.0f + spool);
        float r1 = 9.5f / zz;
        float r0 = 9.5f / (zz + dz);
        if (r0 > 100.0f) continue;
        if (r1 < 5.0f) continue;                     /* skip centre clump */
        if (r1 > 100.0f) r1 = 100.0f;
        float ca = cosf(ang), sa = sinf(ang) * 0.92f;
        int steps = (int)(r1 - r0) + 1;
        if (steps > 30) steps = 30;
        for (int s2 = 0; s2 <= steps; s2++) {
            float rr = r0 + (r1 - r0) * (float)s2 / (float)steps;
            int x = 64 + (int)(ca * rr);
            int y = 60 + (int)(sa * rr);
            if ((unsigned)x >= ELITE_FB_W || (unsigned)y >= ELITE_FB_H)
                continue;
            /* Head (far end of the streak) brightest. */
            float k = (float)s2 / (float)steps;
            uint16_t c = (k > 0.8f) ? RGB565C(240, 245, 255)
                       : (k > 0.45f) ? RGB565C(150, 175, 245)
                                     : RGB565C(60, 80, 165);
            fb[y * ELITE_FB_W + x] = c;
        }
    }
    char name[14];
    galaxy_system_name(s_jump_target, name);
    char buf[28];
    snprintf(buf, sizeof buf, "JUMPING: %s", name);
    craft_font_draw(fb, buf, 30, 100, RGB565C(150, 170, 255));
}

/* The flight dashboard instruments. The real console rows are blitted
 * to the top of the screen; below them: two live MFD screens (mini
 * galaxy chart + mini system schematic) and two small buttons,
 * cockpit-bezel styled (user req: instruments, not grey rectangles). */
static void dash_bezel(uint16_t *fb, int x0, int y0, int x1, int y1,
                       bool sel, int cut) {
    /* chamfered MFD bezel: corners cut by `cut` px */
    uint16_t bc = sel ? RGB565C(120, 255, 120) : RGB565C(70, 86, 115);
    uint16_t fill = RGB565C(6, 9, 16);
    for (int y = y0; y <= y1; y++) {
        int inset = 0;
        if (y - y0 < cut) inset = cut - (y - y0);
        if (y1 - y < cut) inset = cut - (y1 - y);
        for (int x = x0 + inset; x <= x1 - inset; x++)
            fb[y * ELITE_FB_W + x] = fill;
        fb[y * ELITE_FB_W + x0 + inset] = bc;
        fb[y * ELITE_FB_W + x1 - inset] = bc;
    }
    for (int x = x0 + cut; x <= x1 - cut; x++) {
        fb[y0 * ELITE_FB_W + x] = bc;
        fb[y1 * ELITE_FB_W + x] = bc;
    }
}

static void dash_mini_galaxy(uint16_t *fb, int x0, int y0, int w, int h) {
    /* a LIVE little chart: stars around us, range ring, us centred */
    float scale = 2.6f;                       /* px per ly */
    float cxl, cyl;
    galaxy_star_pos(s_addr, &cxl, &cyl);
    int cx = x0 + w / 2, cy = y0 + h / 2;
    float half_ly = (float)w * 0.5f / scale;
    int sx0 = (int)floorf((cxl - half_ly) / SECTOR_LY);
    int sx1 = (int)floorf((cxl + half_ly) / SECTOR_LY);
    int sy0 = (int)floorf((cyl - half_ly) / SECTOR_LY);
    int sy1 = (int)floorf((cyl + half_ly) / SECTOR_LY);
    for (int sy2 = sy0; sy2 <= sy1; sy2++)
        for (int sx2 = sx0; sx2 <= sx1; sx2++) {
            int n = galaxy_sector_stars(sx2, sy2);
            for (int i = 0; i < n; i++) {
                SysAddr a2 = { sx2, sy2, (uint8_t)i };
                float px2, py2;
                galaxy_star_pos(a2, &px2, &py2);
                int x = cx + (int)((px2 - cxl) * scale);
                int y = cy + (int)((py2 - cyl) * scale);
                if (x <= x0 + 1 || x >= x0 + w - 2 ||
                    y <= y0 + 1 || y >= y0 + h - 2)
                    continue;
                fb[y * ELITE_FB_W + x] = RGB565C(165, 175, 205);
            }
        }
    /* jump-range ring */
    float rr = (k_hulls[g_player.hull_id].jump_range * player_roll()->jmp) * scale;
    for (int a2 = 0; a2 < 28; a2++) {
        float th = (float)a2 * (6.2831853f / 28.0f);
        int x = cx + (int)(cosf(th) * rr);
        int y = cy + (int)(sinf(th) * rr);
        if (x > x0 + 1 && x < x0 + w - 2 && y > y0 + 1 && y < y0 + h - 2)
            fb[y * ELITE_FB_W + x] = RGB565C(70, 150, 90);
    }
    /* us */
    fb[cy * ELITE_FB_W + cx] = RGB565C(120, 255, 255);
    fb[(cy - 1) * ELITE_FB_W + cx] = RGB565C(60, 140, 150);
    fb[(cy + 1) * ELITE_FB_W + cx] = RGB565C(60, 140, 150);
    fb[cy * ELITE_FB_W + cx - 1] = RGB565C(60, 140, 150);
    fb[cy * ELITE_FB_W + cx + 1] = RGB565C(60, 140, 150);
}

static void dash_mini_system(uint16_t *fb, int x0, int y0, int w, int h) {
    /* the system schematic: star + planets, station ticks, our anchor */
    const SystemInfo *si = system_info();
    int cy = y0 + h / 2;
    /* star */
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx * dx + dy * dy <= 5)
                fb[(cy + dy) * ELITE_FB_W + x0 + 6 + dx] = si->star_color;
    int n = si->n_planets > 0 ? si->n_planets : 1;
    int step = (w - 18) / n;
    if (step > 13) step = 13;
    for (int i = 0; i < si->n_planets; i++) {
        int x = x0 + 14 + i * step;
        int r = si->planets[i].type == PT_GAS ? 3 : 2;
        uint16_t c = (si->planets[i].type == PT_LAVA)
                         ? RGB565C(200, 90, 30)
                   : (si->planets[i].type == PT_ICE)
                         ? RGB565C(210, 225, 240)
                   : (si->planets[i].type == PT_GAS)
                         ? RGB565C(200, 170, 120)
                   : (si->planets[i].type == PT_ROCK)
                         ? RGB565C(150, 130, 105)
                         : RGB565C(60, 130, 180);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r)
                    fb[(cy + dy) * ELITE_FB_W + x + dx] = c;
        if (si->planets[i].station >= 0) {
            fb[(cy - 6) * ELITE_FB_W + x] = RGB565C(120, 130, 155);
            fb[(cy - 7) * ELITE_FB_W + x - 1] = RGB565C(120, 130, 155);
            fb[(cy - 7) * ELITE_FB_W + x + 1] = RGB565C(120, 130, 155);
        }
        /* our anchor: cyan chevron under the body we're at */
        if (s_anchor_has_poi && s_anchor_poi.kind == POI_PLANET &&
            s_anchor_poi.index == i) {
            fb[(cy + 6) * ELITE_FB_W + x] = RGB565C(120, 255, 255);
            fb[(cy + 7) * ELITE_FB_W + x - 1] = RGB565C(120, 255, 255);
            fb[(cy + 7) * ELITE_FB_W + x + 1] = RGB565C(120, 255, 255);
        }
    }
    if (s_anchor_has_poi && s_anchor_poi.kind == POI_BEACON) {
        fb[(cy + 6) * ELITE_FB_W + x0 + 6] = RGB565C(120, 255, 255);
        fb[(cy + 7) * ELITE_FB_W + x0 + 5] = RGB565C(120, 255, 255);
        fb[(cy + 7) * ELITE_FB_W + x0 + 7] = RGB565C(120, 255, 255);
    }
}

static void dash_draw_panels(uint16_t *fb, int y0) {
    if (y0 >= ELITE_FB_H) return;
    /* console-coloured backing so it reads as one piece of cockpit */
    for (int y = y0; y < ELITE_FB_H; y++)
        for (int x = 0; x < 128; x++)
            fb[y * ELITE_FB_W + x] = RGB565C(13, 17, 27);
    int avail = ELITE_FB_H - y0;
    if (avail < 24) return;                  /* still mostly closed */
    uint16_t on = RGB565C(120, 255, 120), off = RGB565C(120, 132, 156);
    /* MFD pair. The chart keeps its FULL size (57x47, unchanged) — the pill is
       made taller to seat the readable label above it, never smaller. */
    int mh = avail - 30;
    if (mh > 62) mh = 62;
    int my0 = y0 + 2, my1 = my0 + mh;
    dash_bezel(fb, 2, my0, 62, my1, s_dash_sel == 0, 4);
    dash_bezel(fb, 65, my0, 125, my1, s_dash_sel == 1, 4);
    if (mh > 24) {                           /* chart origin +13, height mh-15 -> same 47px at mh=62 */
        dash_mini_galaxy(fb, 4, my0 + 13, 57, mh - 15);
        dash_mini_system(fb, 67, my0 + 13, 57, mh - 15);
    }
    eui_textc(fb, "GALAXY", 32, my0 + 1, s_dash_sel == 0 ? on : off);
    eui_textc(fb, "SYSTEM", 95, my0 + 1, s_dash_sel == 1 ? on : off);
    /* button row: taller pills so the readable label fits */
    int by = my1 + 3;
    if (by + 14 < ELITE_FB_H) {
        dash_bezel(fb, 2, by, 62, by + 13, s_dash_sel == 2, 3);
        dash_bezel(fb, 65, by, 125, by + 13, s_dash_sel == 3, 3);
        eui_textc(fb, "STATUS",   32, by + 1, s_dash_sel == 2 ? on : off);
        eui_textc(fb, "SETTINGS", 95, by + 1, s_dash_sel == 3 ? on : off);
    }
    if (by + 24 < ELITE_FB_H)
        { char h[28]; snprintf(h, sizeof h, "%s:OPEN  %s:RESUME",
            plat_menu_btn(MB_A), plat_menu_btn(MB_B));
          eui_textc(fb, h, 64, by + 16, RGB565C(90, 105, 132)); }
}

static void dash_settings_overlay(uint16_t *fb) {
    char vrow[20], brow[20], grow[20], srow[20], irow[20], drow[20];
    snprintf(vrow, sizeof vrow, "VOLUME    %3d%%", plat_setting_get(0) * 5);
    snprintf(brow, sizeof brow, "BRIGHT    %3d%%",
             (plat_setting_get(1) * 100) / 255);
    static const char *k_diff[3] = { "HARD", "MEDIUM", "EASY" };
    snprintf(drow, sizeof drow, "COMBAT  %s", k_diff[g_player.difficulty % 3]);
    const char *si2[SETTINGS_MAX];
    si2[0] = g_player.invert_y ? "INVERT Y: ON" : "INVERT Y: OFF";
    si2[1] = g_player.show_fps ? "SHOW FPS: ON" : "SHOW FPS: OFF";
    si2[2] = vrow;
    si2[3] = brow;
    si2[ROW_DIFF] = drow;        /* combat difficulty — every platform */
#ifdef ELITE_ANALOG_SETTINGS
    /* Controller / touch-stick sensitivity + controller setup (PC/Android). */
    snprintf(grow, sizeof grow, "GAMEPAD   %3d%%", plat_setting_get(2) * 10);
    snprintf(srow, sizeof srow, "STICK     %3d%%", plat_setting_get(3) * 10);
    si2[5] = grow;
    si2[6] = srow;
#ifdef ELITE_INPUT_SELECT
    static const char *k_indev[4] = { "AUTO", "HOTAS", "GAMEPAD", "KEYBOARD" };
    snprintf(irow, sizeof irow, "INPUT   %s", k_indev[plat_setting_get(4) & 3]);
    si2[ROW_INPUT] = irow;
    si2[ROW_FULL] = plat_setting_get(5) ? "FULLSCREEN: ON" : "FULLSCREEN: OFF";
#else
    (void)irow;
#endif
    si2[ROW_CTRL] = "CONTROLLER...";
#else
    (void)grow; (void)srow; (void)irow;
#endif
    int top = 46, n = settings_rows();
    int bot = top + n * 11 + 14;
    if (bot > 125) { top -= (bot - 125); bot = 125; }   /* fit all rows */
    int row_y = top;
    int y0 = top - 18; if (y0 < 0) y0 = 0;
    if (bot > ELITE_FB_H) bot = ELITE_FB_H;
    for (int y = y0; y < bot; y++)
        for (int x = 10; x < 118; x++)
            fb[y * ELITE_FB_W + x] = RGB565C(8, 11, 20);
    eui_textc(fb, "SETTINGS", 64, top - 15, RGB565C(200, 210, 225));
    for (int i = 0; i < n; i++) {
        uint16_t c = (i == s_settings_cursor) ? RGB565C(120, 255, 120)
                                              : RGB565C(120, 126, 145);
        if (i == s_settings_cursor)
            eui_text(fb, ">", 14, row_y + i * 11, c);
        eui_textclip(fb, si2[i], 24, 116, row_y + i * 11, c);
    }
    { char h[28]; snprintf(h, sizeof h, "</>:ADJUST %s:BACK", plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 14, bot - 11, RGB565C(95, 110, 140)); }
}

/* Lore intro: a slow upward crawl over the live title brawl, in place of the
 * wordmark. Blank entries are page gaps. Skippable with A or MENU. */
static const char *const k_intro[] = {
    "NOBODY BUILT THE",
    "INDEMNITY. IT HAS",
    "ALWAYS BEEN HERE.",
    "",
    "DIE INSURED, AND IT",
    "MAKES YOU WHOLE",
    "AGAIN - YOU WAKE",
    "AT PORT, YOUR",
    "DEATH UNDONE.",
    "",
    "WHETHER THE PILOT",
    "WHO WAKES IS STILL",
    "YOU, NO ONE DARES",
    "ASK.",
    "",
    "THE POWER BEHIND IT:",
    "THE UNDERWRITER.",
    "IT COVERS THOSE WHO",
    "CAN PAY. WHAT IT",
    "TAKES IN RETURN,",
    "NOBODY KNOWS.",
    "",
    "THOSE WHO CAN'T PAY",
    "ARE THE UNINSURED -",
    "THEY TURN PIRATE,",
    "CLAWING BACK THE",
    "COVER THEY LOST.",
    "",
    "AN INFINITE GALAXY.",
    "TRADE. FIGHT.",
    "EXPLORE. RUN.",
    "",
    "THE INDEMNITY",
    "WILL COVER YOU..",
};
#define INTRO_LINES ((int)(sizeof(k_intro) / sizeof(k_intro[0])))
#define INTRO_SPEED 13.0f
#define INTRO_LH    10

static float intro_duration(void) {
    return (128.0f + INTRO_LINES * INTRO_LH) / INTRO_SPEED;
}

static void draw_intro_crawl(uint16_t *fb) {
    float topy = 128.0f - s_intro_t * INTRO_SPEED;
    for (int i = 0; i < INTRO_LINES; i++) {
        if (!k_intro[i][0]) continue;              /* page gap */
        int y = (int)(topy + i * INTRO_LH);
        if (y < -10 || y > 120) continue;
        int x = (128 - craft_font_width_frac(k_intro[i], 3, 2)) / 2;
        if (x < 0) x = 0;
        int b = 255;                               /* fade in/out at the edges */
        if (y < 24)        b = 255 * y / 24;
        else if (y > 96)   b = 255 * (116 - y) / 20;
        if (b < 24) b = 24; if (b > 255) b = 255;
        uint16_t col = RGB565C(b * 205 / 255, b * 220 / 255, b);
        craft_font_draw_frac_aa(fb, k_intro[i], x, y, 3, 2, col);
    }
    craft_font_draw(fb, "A / MENU: SKIP", 35, 121, RGB565C(70, 80, 100));
}

/* --- Title wordmark: custom N2 angular 5x7 font, purple gradient + gold border ------
 * Three passes per glyph (teal outer ring +2, dark inner ring +1, gradient
 * fill) give the layered border the design called for. rows: 5 bits, bit4 = left
 * column. Only the letters in "INDEMNITY RUN" are defined. */
typedef struct { char ch; uint8_t rows[7]; } TitleGlyph;
static const TitleGlyph k_tw[] = {
    {'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'N',{0x11,0x19,0x15,0x15,0x13,0x11,0x11}},
    {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E',{0x1F,0x10,0x10,0x1C,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'Y',{0x11,0x0A,0x04,0x04,0x04,0x04,0x04}},
    {'R',{0x1C,0x12,0x12,0x1C,0x14,0x12,0x11}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
};
static uint16_t tw_lerp(uint16_t a, uint16_t b, float t) {
    int ar=(a>>11)&31, ag=(a>>5)&63, ab=a&31;
    int br=(b>>11)&31, bg=(b>>5)&63, bb=b&31;
    int r=ar+(int)((br-ar)*t+0.5f), g=ag+(int)((bg-ag)*t+0.5f), bl=ab+(int)((bb-ab)*t+0.5f);
    return (uint16_t)((r<<11)|(g<<5)|bl);
}
static inline void tw_px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < ELITE_FB_W && (unsigned)y < ELITE_FB_H) fb[y*ELITE_FB_W + x] = c;
}
static int tw_width(const char *s, int scale) {
    int n=0; for (const char *p=s; *p; p++) n++;
    return n*(6*scale) - scale;           /* glyph 5px + 1px gap, *scale */
}
static void tw_draw(uint16_t *fb, const char *s, int x, int y, int scale,
                    uint16_t top, uint16_t bot, uint16_t inner, uint16_t outer) {
    for (int pass=0; pass<3; pass++) {
        int cx=x;
        for (const char *p=s; *p; p++) {
            const uint8_t *rows=NULL;
            for (int i=0;i<(int)(sizeof k_tw/sizeof k_tw[0]);i++)
                if (k_tw[i].ch==*p){ rows=k_tw[i].rows; break; }
            if (rows) for (int r=0;r<7;r++) {
                uint8_t bits=rows[r];
                for (int c=0;c<5;c++) {
                    if (!(bits & (1<<(4-c)))) continue;
                    int gx=cx+c*scale, gy=y+r*scale;
                    if (pass==0)
                        for(int yy=-2;yy<scale+2;yy++)for(int xx=-2;xx<scale+2;xx++) tw_px(fb,gx+xx,gy+yy,outer);
                    else if (pass==1)
                        for(int yy=-1;yy<scale+1;yy++)for(int xx=-1;xx<scale+1;xx++) tw_px(fb,gx+xx,gy+yy,inner);
                    else
                        for(int yy=0;yy<scale;yy++){
                            float tt=(float)(r*scale+yy)/(float)(7*scale-1);
                            uint16_t col=tw_lerp(top,bot,tt);
                            for(int xx=0;xx<scale;xx++) tw_px(fb,gx+xx,gy+yy,col);
                        }
                }
            }
            cx += 6*scale;
        }
    }
}

void elite_game_draw_overlay(uint16_t *fb) {
    if (g_player.show_fps) {
        char fbuf[12];
        snprintf(fbuf, sizeof fbuf, "%d FPS", (int)(s_fps + 0.5f));
        craft_font_draw(fb, fbuf, 2, 1,
                        RGB565C(110, 255, 110));   /* above the chaff */
    }

    /* (s_time advances in elite_game_tick by real dt — frame-rate
     * independent, so station/ship preview spin at a fixed real speed.) */

    switch (s_state) {
    case ST_TITLE: {
        if (s_intro_active) { draw_intro_crawl(fb); return; }
        /* Title wordmark: custom angular font, purple gradient + gold border. RUN matches
         * INDEMNITY's size (both scale 2). */
        {
            uint16_t T_top = RGB565C(196,120,240), T_bot = RGB565C(72,22,116);
            uint16_t T_in  = RGB565C(10,10,14),    T_out = RGB565C(255,205,70);
            int s = 2;
            tw_draw(fb, "INDEMNITY", (128 - tw_width("INDEMNITY", s)) / 2, 14, s,
                    T_top, T_bot, T_in, T_out);
            tw_draw(fb, "RUN", (128 - tw_width("RUN", s)) / 2, 32, s,
                    T_top, T_bot, T_in, T_out);
        }
        bool has_save = save_exists();
        const char *base_items[3] = { "CONTINUE", "NEW GAME", "PVP ARENA" };
        if (!s_cheat_on) {
            /* Readable centred menu, colour marks the selection (no caret). */
            int lh = eui_lineh();
            for (int i = 0; i < 3; i++) {
                uint16_t c = (i == 0 && !has_save) ? RGB565C(60, 66, 84)
                           : (i == s_title_cursor) ? RGB565C(120, 255, 120)
                                                   : RGB565C(120, 126, 145);
                eui_textc(fb, base_items[i], 64, 72 + i * lh, c);
            }
            craft_font_draw(fb, "AN INFINITE GALAXY TO EXPLORE", 6, 118,
                            RGB565C(80, 100, 125));
        } else {
            /* Cheat scenario picker stays compact — many rows. */
            int n = 3 + N_SCENARIOS;
            for (int i = 0; i < n; i++) {
                const char *label = base_items[i < 3 ? i : 0];
#if ELITE_CHEATS
                if (i >= 3) label = k_scenarios[i - 3];
#endif
                uint16_t c = (i == 0 && !has_save) ? RGB565C(60, 66, 84)
                           : (i == s_title_cursor) ? RGB565C(120, 255, 120)
                           : (i >= 3)               ? RGB565C(225, 185, 85)
                                                    : RGB565C(120, 126, 145);
                int yy = 46 + i * 8;
                if (i == s_title_cursor)
                    craft_font_draw(fb, ">", 16, yy, RGB565C(120, 255, 120));
                craft_font_draw(fb, label, 24, yy, c);
            }
            craft_font_draw(fb, "TEST SCENARIOS  (RBx10 HIDES)", 6, 119,
                            RGB565C(120, 110, 70));
        }
        return;
    }

    case ST_SAVESEL: {
        eui_textc(fb, s_pvp_select ? "DUEL SHIP" : "CONTINUE", 64, 2, RGB565C(200, 210, 225));
        if (s_savelist_n == 0) {
            eui_textc(fb, "NO SAVED GAMES", 64, 56, RGB565C(160, 130, 100));
            craft_font_draw(fb, "B:BACK", 46, 112, RGB565C(95, 110, 140));
            return;
        }
        /* focused save: its ship is the 3D icon above; show its stats (readable) */
        SavePeek *pk = &s_savelist[s_sel_cursor].pk;
        char sysn[14]; galaxy_system_name(pk->addr, sysn);
        char line[44];
        eui_text(fb, sysn, 6, 48, RGB565C(150, 200, 235));
        snprintf(line, sizeof line, "%ld CR", (long)pk->credits);
        eui_text(fb, line, 6, 60, RGB565C(255, 210, 90));
        snprintf(line, sizeof line, "%s - %d KILLS",
                 elite_rank_name(pk->kills), (int)pk->kills);
        eui_textclip(fb, line, 6, 126, 72, RGB565C(180, 190, 210));
        /* scrollable game list, readable */
        int VIS = 2;
        if (s_sel_cursor < s_sel_scroll) s_sel_scroll = s_sel_cursor;
        if (s_sel_cursor >= s_sel_scroll + VIS) s_sel_scroll = s_sel_cursor - VIS + 1;
        int ly = 86;
        for (int i = s_sel_scroll; i < s_savelist_n && i < s_sel_scroll + VIS;
             i++, ly += 13) {
            uint16_t c = (i == s_sel_cursor) ? RGB565C(120, 255, 120)
                                             : RGB565C(110, 116, 135);
            if (i == s_sel_cursor) eui_text(fb, ">", 6, ly, c);
            char row[20];
            snprintf(row, sizeof row, "GAME %d", s_savelist[i].slot + 1);
            eui_text(fb, row, 16, ly, c);
        }
        eui_scrollbar(fb, 125, 86, VIS * 13, s_savelist_n, VIS, s_sel_scroll,
                      RGB565C(120, 255, 120), RGB565C(60, 70, 90));
        if (s_sel_confirm_del) {
            for (int y = 50; y < 84; y++)
                for (int x = 8; x < 120; x++) fb[y * ELITE_FB_W + x] = RGB565C(28, 8, 8);
            eui_textc(fb, "DELETE THIS GAME?", 64, 56, RGB565C(255, 120, 90));
            eui_textc(fb, "A:YES   B:NO", 64, 70, RGB565C(205, 205, 215));
        } else {
            craft_font_draw(fb, "A:PLAY LB:DEL B:BACK", 6, 119,
                            RGB565C(95, 110, 140));
        }
        return;
    }

    case ST_PVPWAIT:    pvp_draw_overlay(fb); return;   /* PVP: link-wait card */
    case ST_GALAXY_MAP: map_galaxy_draw(fb); return;
    case ST_SYSTEM_MAP: map_system_draw(fb); return;
    case ST_HYPERJUMP:  draw_hyperjump_overlay(fb); return;
    case ST_DOCKED:     station_draw(fb); return;
    case ST_EVENT:      ui_event_draw(fb); return;
    case ST_STATUS:     status_draw(fb); return;
#ifdef ELITE_ANALOG_SETTINGS
    case ST_CTRLSETUP:  ctrlsetup_draw(fb); return;
#endif
    default: break;
    }

    Ship *p = &g_ships[PLAYER];
    if (s_state == ST_SUPERCRUISE) {
        HudScInfo info = {
            .dest_name = s_sc_has_dest ? s_sc_dest.name : NULL,
            .dest_rel_mm = s_sc_has_dest
                ? v3_sub(s_sc_dest.pos_mm, s_sc_pos_mm) : v3(0, 0, 1),
            .speed_mms = s_sc_speed,
            .eta_s = s_sc_eta,
            .throttle = p->throttle,
            .fuel01 = g_player.fuel / g_player.fuel_max,
            .render_ms = s_frame_ms,
            .show_perf = 0,   /* perf validated on device; readout off */
        };
        ui_hud_draw_sc(fb, &info);
        return;
    }

    if (p->alive) {
        Vec3 lpos = v3(0, 0, 0);
        if (s_loot_target >= 0 &&
            loot_nearest(g_ships[PLAYER].pos, &lpos) < 0)
            s_loot_target = -1;          /* scooped/expired */
        Vec3 rpos = v3(0, 0, 0);
        if (s_rock_target >= 0) {
            Vec3 rk[8];
            int nr = rocks_positions(rk, 8);
            if (s_rock_target < nr) rpos = rk[s_rock_target];
            else s_rock_target = -1;     /* cracked it */
        }
        HudInfo info = {
            .target = s_target,
            .loot_valid = (s_target < 0 && s_loot_target >= 0),
            .loot_pos = lpos,
            .rock_valid = (s_target < 0 && s_loot_target < 0 &&
                           s_rock_target >= 0),
            .rock_pos = rpos,
            .station_valid = (s_target < 0 && s_loot_target < 0 &&
                              s_station_lock),
            .rail_charge01 = s_rail_charge01,
            .incoming = s_incoming,
            .kills = combat_kills(),
            .fuel01 = g_player.fuel / g_player.fuel_max,
            .render_ms = s_frame_ms,
            .show_perf = 0,   /* perf validated on device; readout off */
        };
        ui_hud_draw(fb, &info);
        if (s_cloak_t > 0) {
            /* cyan veil readout: label + draining bar */
            craft_font_draw(fb, "CLOAKED", 46, 14, RGB565C(90, 230, 255));
            int w = (int)(36.0f * (s_cloak_t / 8.0f));
            for (int x = 0; x < w; x++)
                fb[46 + x + 22 * 128] = RGB565C(60, 170, 200);
        }
        if (s_scoop_toast_t > 0)
            eui_textc(fb, s_scoop_toast, 64, 39, RGB565C(255, 200, 60));
        if (s_state == ST_DOCKING)
            eui_textc(fb, "DOCKING...", 64, 27, RGB565C(120, 230, 255));
        else if (can_dock())
            eui_textc(fb, "LB+RB: DOCK", 64, 27, RGB565C(120, 230, 255));
        if (pvp_active()) pvp_draw_overlay(fb);   /* PVP: VICTORY / LINK LOST banner */
    } else if (pvp_active()) {
        pvp_draw_overlay(fb);                      /* PVP: DEFEAT card (no kill report) */
    } else {
        /* THE KILL REPORT (user req): who got you, flying what. */
        uint16_t hd = RGB565C(255, 90, 70);
        uint16_t tx = RGB565C(210, 214, 222);
        uint16_t dm = RGB565C(150, 156, 170);
        uint16_t gd = RGB565C(245, 200, 80);
        craft_font_draw_2x(fb, "SHIP LOST", 28, 16, hd);
        char b[28];
        if (s_kr.env) {
            static const char *envn[4] = { "", "AN ASTEROID",
                                           "THE STATION", "THE STAR" };
            eui_text(fb, "FLEW INTO", 12, 38, tx);
            eui_textclip(fb, envn[s_kr.env & 3], 12, 126, 52, gd);
        } else if (s_kr.valid) {
            snprintf(b, sizeof b, "KILLED BY %s %s",
                     s_kr.wfac ? k_faction_names[(s_kr.wfac - 1) %
                                                 N_FACTIONS]
                     : s_kr.police ? "POLICE" : "PIRATE",
                     k_tier_names[s_kr.tier]);
            eui_textclip(fb, b, 12, 126, 36, gd);
            snprintf(b, sizeof b, "%s CLASS RAIDER",
                     k_hulls[s_kr.cls % N_HULLS].name);
            eui_textclip(fb, b, 12, 126, 49, tx);
            craft_font_draw(fb, "GUNS", 12, 64, dm);
            int yy = 64;
            for (int i = 0; i < s_kr.nw; i++) {
                craft_font_draw(fb, k_weapons[s_kr.wpn[i]].name, 38, yy,
                                tx);
                yy += 9;
            }
            if (s_kr.turret) {
                craft_font_draw(fb, "+TURRET", 38, yy, tx);
                yy += 9;
            }
            yy += 2;
            snprintf(b, sizeof b, "SHD %d %s", (int)s_kr.shd,
                     k_shield_var_names[s_kr.shv & 3]);
            craft_font_draw(fb, b, 12, yy, tx); yy += 9;
            snprintf(b, sizeof b, "HUL %d %s", (int)s_kr.hull,
                     k_armor_var_names[s_kr.arv & 3]);
            craft_font_draw(fb, b, 12, yy, tx); yy += 9;
            snprintf(b, sizeof b, "SPD %d  TRN %d.%d  CHAFF %d",
                     (int)s_kr.spd, (int)s_kr.trn,
                     (int)(s_kr.trn * 10) % 10, s_kr.chaff);
            craft_font_draw(fb, b, 12, yy, tx);
        } else {
            eui_textc(fb, "CAUSE UNKNOWN", 64, 42, tx);
        }
        if (s_respawn_t <= 0) {
            char h[28]; snprintf(h, sizeof h, "%s: INSURANCE CLAIM", plat_menu_btn(MB_A));
            eui_textclip(fb, h, 12, 126, 116, gd);
        }
    }

    if (s_state == ST_DASH) {
        /* Move the REAL console (the rows the HUD just painted) up the
         * screen; the instrument panels slide into view beneath it. */
        int dtop = ui_hud_dash_top();
        int dash_h = ELITE_FB_H - dtop;
        int up = (int)(s_dash_anim * (float)dtop);
        if (up > 0) {
            memmove(&fb[(dtop - up) * ELITE_FB_W],
                    &fb[dtop * ELITE_FB_W],
                    (size_t)dash_h * ELITE_FB_W * 2);
            dash_draw_panels(fb, dtop - up + dash_h);
            /* Toasts must survive the panels (rescue hails, warnings —
             * the sim is live under here). */
            if (s_scoop_toast_t > 0)
                eui_textc(fb, s_scoop_toast, 64,
                          dtop - up + dash_h + 2,
                          RGB565C(255, 200, 60));
        }
        if (s_in_settings) dash_settings_overlay(fb);
    }
}
