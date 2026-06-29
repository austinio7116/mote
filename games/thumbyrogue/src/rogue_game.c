#include "rogue_game.h"
#include "rogue_camera.h"
#include "rogue_player.h"
#include "rogue_enemy.h"
#include "rogue_gen.h"
#include "rogue_render.h"
#include "rogue_hud.h"
#include "rogue_items.h"
#include "rogue_loot.h"
#include "rogue_proj.h"
#include "rogue_band.h"
#include "rogue_sfx.h"
#include "rogue_platform.h"
#include "rogue_inventory.h"
#include "rogue_shop.h"
#include "rogue_particle.h"
#include "rogue_dmgnum.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_render.h"
#include "craft_font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

static RoguePlayer    s_player;
static CraftCamera    s_cam;
static CraftRawButtons s_prev;
static RogueLevelInfo s_level;
static uint32_t s_seed;
static int   s_depth;
static float s_dead_t;     /* >0 while the death banner shows */
static float s_band_banner_t;  /* >0 while the band-name banner shows */
static float s_hitstop;        /* >0 → world runs in slow-mo (melee impact) */
static int   s_last_band = -1;
static int   s_kills;
static int   s_best_depth;
static bool  s_merchants_hostile;  /* killed a shopkeeper — the guild remembers */
static bool  s_title = true;

/* Cheat: hold LB+RB together for ~5s to open a level-skip menu. */
static bool  s_skip;
static int   s_skip_target;
static float s_lbrb_t;

static float s_anim_t;     /* animated-tile clock (water/lava/portal) */
static char  s_toast[48];  /* transient pickup/chest message */
static float s_toast_t;
static uint32_t s_loot_rng = 0x13572468u;

void rogue_game_toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_t = 2.6f;
}

/* Per-weapon-type melee impact FX so each weapon's hit reads differently:
 * light/quick for daggers, heavy blue for greatswords, fiery for axes, and a
 * grey dust shock-ring for the blunt mace/warhammer. */
static void melee_hit_fx(Vec3 hp, uint8_t wt, int elem) {
    if (elem != ELEM_NONE) {
        /* elemental weapons stamp their colour on every impact */
        uint16_t c = (elem == ELEM_FIRE)      ? RGB(255,130,40)
                   : (elem == ELEM_FROST)     ? RGB(150,215,255)
                   : (elem == ELEM_POISON)    ? RGB(130,235,90)
                   : (elem == ELEM_LIGHTNING) ? RGB(255,250,140)
                   : (elem == ELEM_HOLY)      ? RGB(255,225,100)
                   : (elem == ELEM_SHADOW)    ? RGB(150,70,210)
                   : (elem == ELEM_VOID)      ? RGB(105,95,240)
                   :                            RGB(255,95,235);
        rogue_particle_burst(hp, 12, 5.2f, 0.40f, c, 0.08f);
        if (elem == ELEM_FIRE) {            /* embers float up from a burn */
            for (int k = 0; k < 4; k++)
                rogue_particle_spawn(hp, (k - 1.5f) * 0.8f, 2.6f, 0.4f,
                                     0.45f, RGB(255,200,80), 0.05f, -2.0f);
        }
        return;
    }
    switch (wt) {
    case WT_DAGGER:     rogue_particle_burst(hp, 5,  5.5f, 0.25f, RGB(230,235,255), 0.05f); break;
    case WT_GREATSWORD: rogue_particle_burst(hp, 16, 5.5f, 0.45f, RGB(170,210,255), 0.10f); break;
    case WT_AXE:        rogue_particle_burst(hp, 12, 5.5f, 0.40f, RGB(255,120,50),  0.09f); break;
    case WT_SPEAR:      rogue_particle_burst(hp, 6,  6.0f, 0.30f, RGB(225,235,250), 0.06f); break;
    case WT_MACE:
    case WT_WARHAMMER: {
        int big = (wt == WT_WARHAMMER);
        rogue_particle_burst(hp, big ? 18 : 12, 3.8f, 0.50f, RGB(205,197,178), big ? 0.12f : 0.10f);
        /* a low ground shock-ring of dust motes — the blunt-impact signature */
        int ring = big ? 12 : 8;
        float spd = big ? 6.5f : 5.0f;
        Vec3 g = hp; g.y -= 0.45f;
        for (int k = 0; k < ring; k++) {
            float a = (float)k / ring * 6.2831853f;
            rogue_particle_spawn(g, cosf(a) * spd, 0.4f, sinf(a) * spd,
                                 0.40f, RGB(190,182,165), 0.08f, 5.0f);
        }
        break; }
    default:            rogue_particle_burst(hp, 9,  5.0f, 0.35f, RGB(255,245,190), 0.07f); break;  /* sword */
    }
}

/* Fog-of-war minimap: cells the hero has been near. */
static uint8_t s_visited[CRAFT_WORLD_X * CRAFT_WORLD_Z];

/* Spike traps — always visible (fair), damage on contact with a cooldown. */
#define MAX_TRAPS 8
static Vec3  s_trap[MAX_TRAPS];
static int   s_n_trap;
static float s_trap_cd[MAX_TRAPS];
static uint32_t loot_rng(void){ s_loot_rng^=s_loot_rng<<13; s_loot_rng^=s_loot_rng>>17; s_loot_rng^=s_loot_rng<<5; return s_loot_rng; }

/* (The stairs are now REAL world-block staircases carved by the generator —
 * a walled stairwell rising at the up-stairs, a stone-lined trench
 * descending under the floor at the down-stairs. No beacons, no models.) */
/* Minecraft-style floor torch: a thin wooden stick topped with a flame. */
static const RogueCuboid torch_model[] = {
    { 0.0f, 0.24f, 0.0f, 0.035f, 0.24f, 0.035f, RGB(110, 75, 40)  },  /* stick */
    { 0.0f, 0.50f, 0.0f, 0.075f, 0.06f, 0.075f, RGB(255, 150, 30) },  /* ember */
    { 0.0f, 0.58f, 0.0f, 0.055f, 0.06f, 0.055f, RGB(255, 225, 120) }, /* flame */
};

/* Furniture props (drawn like torches). Table: a plank top on four legs.
 * Brazier: a dark iron bowl on a tripod with a flame (a torch light cell
 * is placed under it in the generator so it actually lights the room). */
static const RogueCuboid table_model[] = {
    { 0.0f,  0.46f,  0.0f,  0.40f, 0.05f, 0.30f, RGB(120, 82, 44) },  /* top slab */
    { -0.32f,0.21f, -0.22f, 0.05f, 0.21f, 0.05f, RGB(86, 56, 28) },   /* legs */
    {  0.32f,0.21f, -0.22f, 0.05f, 0.21f, 0.05f, RGB(86, 56, 28) },
    { -0.32f,0.21f,  0.22f, 0.05f, 0.21f, 0.05f, RGB(86, 56, 28) },
    {  0.32f,0.21f,  0.22f, 0.05f, 0.21f, 0.05f, RGB(86, 56, 28) },
};
static const RogueCuboid brazier_model[] = {
    { -0.16f,0.20f, -0.10f, 0.03f, 0.22f, 0.03f, RGB(60, 60, 66) },   /* tripod legs */
    {  0.16f,0.20f, -0.10f, 0.03f, 0.22f, 0.03f, RGB(60, 60, 66) },
    {  0.0f, 0.20f,  0.18f, 0.03f, 0.22f, 0.03f, RGB(60, 60, 66) },
    { 0.0f,  0.44f,  0.0f,  0.22f, 0.07f, 0.22f, RGB(48, 48, 54) },   /* iron bowl */
    { 0.0f,  0.54f,  0.0f,  0.14f, 0.06f, 0.14f, RGB(255, 150, 30) }, /* ember */
    { 0.0f,  0.62f,  0.0f,  0.09f, 0.07f, 0.09f, RGB(255, 225, 120) },/* flame */
};

/* Carry the full paperdoll + gold across floors; only rebuild on death. */
static RogueItem s_keep_equip[SLOT_COUNT];
static int s_keep_gold;
static bool s_have_keep;

/* ---- save / load ---- */
int rogue_plat_save(const uint8_t *data, int len);   /* platform-provided */
int rogue_plat_load(uint8_t *data, int max);

#define ROGUE_SAVE_MAGIC 0x52475635u   /* 'RGV5' — shopkeeper aggro + calm flag */
typedef struct {
    uint32_t magic, version;
    uint32_t seed;
    int32_t  depth;        /* >0 = a live run to resume; <=0 = none */
    int32_t  kills, best, gold, hp;
    int32_t  merchants_hostile;   /* killed a shopkeeper this run — all hostile */
    float    torch;
    RogueItem equip[SLOT_COUNT];
    int32_t  bag_n;
    RogueItem bag[ROGUE_BAG_N];
    /* --- full mid-level suspend (valid only when `suspended` != 0) ----- *
     * The floor's blocks/scenery/chest-positions regenerate deterministically
     * from `seed`, so only the non-reproducible dynamic state is stored: the
     * hero's exact spot, the live enemies, ground drops, and which chests were
     * already opened. Lets a lobby-quit resume EXACTLY where you left off. */
    int32_t  suspended;
    float    px, py, pz, pyaw;
    int32_t  n_en;
    RogueEnemySave  en[ROGUE_MAX_ENEMIES];
    int32_t  n_ground;
    RogueGroundSave ground[ROGUE_MAX_GROUND];
    uint32_t chest_mask;
} RogueSave;
_Static_assert(sizeof(RogueSave) <= 8192, "suspend save must fit the standalone 8KB flash region");

/* Fill the common header/inventory fields shared by both save kinds. */
static void save_fill_common(RogueSave *s, int run_active) {
    memset(s, 0, sizeof *s);
    s->magic = ROGUE_SAVE_MAGIC; s->version = 5;
    s->seed = s_seed;
    s->depth = run_active ? s_depth : -1;
    s->kills = s_kills;
    s->best = (s_depth > s_best_depth) ? s_depth : s_best_depth;
    s->gold = s_player.gold;
    s->hp = s_player.hp;
    s->merchants_hostile = s_merchants_hostile ? 1 : 0;
    s->torch = s_player.torch_fuel;
    for (int i = 0; i < SLOT_COUNT; i++) s->equip[i] = s_player.equip[i];
    s->bag_n = rogue_inventory_export(s->bag, ROGUE_BAG_N);
}

/* Between-floor checkpoint: regenerates the floor at its entrance on resume. */
void rogue_game_save(int run_active) {
    RogueSave s;
    save_fill_common(&s, run_active);
    s.suspended = 0;
    rogue_plat_save((const uint8_t *)&s, (int)sizeof s);
}

/* Full mid-level suspend (lobby-quit): snapshots the live floor so resuming
 * drops you back on the same spot with the same enemies / loot / chests. */
void rogue_game_save_full(void) {
    if (s_title || !s_player.alive || s_depth <= 0) { rogue_game_save(1); return; }
    RogueSave s;
    save_fill_common(&s, 1);
    s.suspended = 1;
    s.px = s_player.pos.x; s.py = s_player.pos.y; s.pz = s_player.pos.z;
    s.pyaw = s_player.yaw;
    s.n_en = rogue_enemies_export(s.en, ROGUE_MAX_ENEMIES);
    s.n_ground = rogue_loot_export_ground(s.ground, ROGUE_MAX_GROUND);
    s.chest_mask = rogue_loot_chest_mask();
    rogue_plat_save((const uint8_t *)&s, (int)sizeof s);
}

static void load_level(void);
/* Try to resume a saved run. Returns true if a live run was restored. */
static bool try_resume(void) {
    RogueSave s;
    int n = rogue_plat_load((uint8_t *)&s, (int)sizeof s);
    if (n < (int)sizeof s || s.magic != ROGUE_SAVE_MAGIC) return false;
    if (s.best > s_best_depth) s_best_depth = s.best;
    if (s.depth <= 0) return false;        /* save exists but run is over */
    s_seed = s.seed;
    s_depth = s.depth;
    s_kills = s.kills;
    s_merchants_hostile = s.merchants_hostile != 0;
    load_level();                          /* regenerate the saved floor */
    for (int i = 0; i < SLOT_COUNT; i++) s_player.equip[i] = s.equip[i];
    rogue_player_recompute(&s_player);
    s_player.hp = s.hp > 0 ? s.hp : 1;
    if (s_player.hp > s_player.max_hp) s_player.hp = s_player.max_hp;
    s_player.gold = s.gold;
    s_player.torch_fuel = s.torch;
    rogue_inventory_import(s.bag, s.bag_n);
    s_have_keep = true;
    for (int i = 0; i < SLOT_COUNT; i++) s_keep_equip[i] = s.equip[i];
    s_keep_gold = s.gold;
    if (s.suspended) {
        /* Mid-level suspend: load_level() regenerated the floor and spawned a
         * fresh population; replace it with the saved snapshot and put the hero
         * back exactly where they quit. */
        rogue_enemies_import(s.en, s.n_en);
        rogue_loot_import_ground(s.ground, s.n_ground);
        rogue_loot_apply_chest_mask(s.chest_mask);
        s_player.pos = v3(s.px, s.py, s.pz);
        s_player.yaw = s.pyaw;
        rogue_camera_init(s_player.pos);
        rogue_camera_get(&s_cam);
    }
    return true;
}

static void load_level(void) {
    memset(s_visited, 0, sizeof s_visited);
    rogue_particle_clear();
    rogue_dmgnum_clear();
    rogue_gen_dungeon(s_seed, s_depth, &s_level);
    rogue_player_init(&s_player, s_level.spawn);
    if (s_have_keep) {
        for (int i = 0; i < SLOT_COUNT; i++) s_player.equip[i] = s_keep_equip[i];
        rogue_player_recompute(&s_player);
        s_player.hp = s_player.max_hp;
        s_player.gold = s_keep_gold;
    }
    rogue_camera_init(s_player.pos);
    rogue_enemies_spawn(s_level.room_cx, s_level.room_cz, s_level.n_rooms,
                        s_level.up_x, s_level.up_z, s_level.floor_y,
                        s_depth, s_seed);
    rogue_loot_clear();
    rogue_proj_clear();
    rogue_loot_place_chests(s_level.room_cx, s_level.room_cz, s_level.n_rooms,
                            s_level.up_x, s_level.up_z, s_level.floor_y,
                            s_depth, s_seed);
    rogue_platform_place(s_level.room_cx, s_level.room_cz, s_level.n_rooms,
                         s_level.up_x, s_level.up_z, s_level.floor_y,
                         s_depth, s_seed,
                         s_level.chasm_x, s_level.chasm_z, s_level.n_chasm);
    rogue_shop_place(&s_level, s_depth, s_seed);
    if (s_level.has_shop)
        rogue_enemies_add_shopkeeper(
            s_level.shop_x + 0.5f - s_level.shop_dx, (float)s_level.floor_y,
            s_level.shop_z + 0.5f - s_level.shop_dz,
            atan2f((float)s_level.shop_dx, (float)s_level.shop_dz),
            !s_merchants_hostile, s_depth);
    /* Bonus chest on each lava island — only reachable by riding the platform. */
    for (int c = 0; c < s_level.n_chasm; c++)
        rogue_loot_add_chest_at(s_level.island_x[c] + 0.5f, (float)s_level.floor_y,
                                s_level.island_z[c] + 0.5f);
    /* Spike traps in some rooms (not the up-stairs). */
    s_n_trap = 0;
    int twant = 1 + s_depth / 2;
    if (twant > MAX_TRAPS) twant = MAX_TRAPS;
    for (int a = 0; a < twant * 4 && s_n_trap < twant; a++) {
        int r = (int)(loot_rng() % (uint32_t)(s_level.n_rooms > 0 ? s_level.n_rooms : 1));
        if (s_level.room_cx[r] == s_level.up_x && s_level.room_cz[r] == s_level.up_z) continue;
        s_trap[s_n_trap] = v3(s_level.room_cx[r] + 0.5f + ((int)(loot_rng()%3)-1),
                              (float)s_level.floor_y,
                              s_level.room_cz[r] + 0.5f + ((int)(loot_rng()%3)-1));
        s_trap_cd[s_n_trap] = 0.0f;
        s_n_trap++;
    }

    /* Announce a new band the first time we enter it. */
    int band = (s_depth - 1) / ROGUE_BAND_FLOORS;
    if (band != s_last_band) { s_last_band = band; s_band_banner_t = 2.2f; }
}

void rogue_game_init(uint32_t seed) {
    s_seed = seed;
    s_depth = 1;
    s_dead_t = 0.0f;
    s_have_keep = false;
    s_last_band = -1;
    s_band_banner_t = 0.0f;
    s_loot_rng = seed | 1u;

    craft_render_set_fog(false);
    craft_render_set_clouds(false);
    craft_render_set_far_lod(false);
    craft_render_set_groundcover(true);    /* render the cross-sprite scenery */
    craft_render_set_interlace(false);
    craft_render_set_lowres(false);
    craft_render_set_coarse_skip(false);
    craft_render_set_torch_light(false);
    craft_render_set_player_light(true);   /* the hero's torch lights the scene */
    craft_render_set_time(240.0f);         /* deep-night ambient → dark dungeon */

    s_kills = 0;
    s_best_depth = 0;
    s_merchants_hostile = false;
    rogue_inventory_clear();
    rogue_sfx_init();
    /* Resume a saved run if there is one; otherwise start fresh at the title. */
    if (try_resume()) {
        s_title = false;
    } else {
        s_title = true;
        load_level();
    }
    s_prev = (CraftRawButtons){0};
}

static bool edge(bool now, bool prev) { return now && !prev; }

void rogue_game_tick(const CraftRawButtons *btn, float dt) {
    /* Hit-stop: a few frames of heavy slow-motion when a melee blow lands —
     * the classic impact "crunch". Everything (enemies, physics, animation)
     * crawls; rendering continues at full rate. */
    if (s_hitstop > 0.0f) {
        s_hitstop -= dt;
        dt *= 0.12f;
    }

    /* Advance the animated-tile clock every frame (water 4Hz, lava 2Hz,
     * portal 3Hz). The renderer already blends water see-through; this is
     * what makes the dank-green surface actually ripple. Runs in every
     * state so the world keeps animating behind menus too. */
    s_anim_t += dt;
    craft_blocks_animate_water(s_anim_t);

    /* Title screen — slowly orbit the camera until A starts the run. */
    if (s_title) {
        if (edge(btn->a, s_prev.a)) s_title = false;
        rogue_camera_follow(s_player.pos, dt);
        rogue_camera_update(dt);
        rogue_camera_get(&s_cam);
        s_prev = *btn;
        return;
    }

    /* Death → run-summary screen; A starts a fresh run (permadeath: gear +
     * gold lost, back to the starter dagger). */
    if (!s_player.alive) {
        if (s_depth > s_best_depth) s_best_depth = s_depth;
        if (s_dead_t == 0.0f) rogue_game_save(0);   /* permadeath: wipe run, keep best */
        s_dead_t += dt;
        /* require a brief beat before accepting input, then wait for A */
        if (s_dead_t > 0.6f && edge(btn->a, s_prev.a)) {
            s_dead_t = 0.0f;
            s_seed = s_seed * 1664525u + 1013904223u;
            s_depth = 1;
            s_have_keep = false;
            s_kills = 0;
            s_merchants_hostile = false;
            rogue_inventory_clear();
            load_level();
            rogue_game_save(1);
        }
        rogue_camera_update(dt);
        rogue_camera_get(&s_cam);
        s_prev = *btn;
        return;
    }

    /* Inventory screen — freezes gameplay; MENU toggles it. */
    if (rogue_inventory_is_open()) {
        rogue_inventory_input(&s_player, btn, &s_prev);
        if (edge(btn->menu, s_prev.menu)) rogue_inventory_close();
        rogue_camera_get(&s_cam);
        s_prev = *btn;
        return;
    }
    /* Shop screen — opened by the merchant pad; MENU leaves. */
    if (rogue_shop_is_open()) {
        rogue_shop_input(&s_player, btn, &s_prev);
        if (edge(btn->menu, s_prev.menu)) rogue_shop_close();
        rogue_camera_get(&s_cam);
        s_prev = *btn;
        return;
    }
    /* Level-skip cheat menu (opened by the LB+RB hold below). */
    if (s_skip) {
        if (edge(btn->up, s_prev.up) || edge(btn->right, s_prev.right)) s_skip_target++;
        if (edge(btn->down, s_prev.down) || edge(btn->left, s_prev.left)) s_skip_target--;
        if (s_skip_target < 1) s_skip_target = 1;
        if (edge(btn->a, s_prev.a)) {
            s_depth = s_skip_target; s_last_band = -1;
            load_level(); rogue_game_save(1);
            s_skip = false;
        }
        if (edge(btn->b, s_prev.b) || edge(btn->menu, s_prev.menu)) s_skip = false;
        rogue_camera_get(&s_cam);
        s_prev = *btn;
        return;
    }

    if (edge(btn->menu, s_prev.menu)) {
        rogue_inventory_open();
        s_prev = *btn;
        return;
    }

    /* LB/RB swivel the view SMOOTHLY (no more 90-degree snaps) — unless
     * BOTH are held, which arms the level-skip cheat (hold ~5s). Movement
     * stays screen-relative against the live camera yaw. */
    bool both_lr = btn->lb && btn->rb;
    if (!both_lr) {
        if (btn->lb) rogue_camera_rotate_smooth(-2.4f * dt);
        if (btn->rb) rogue_camera_rotate_smooth(+2.4f * dt);
        s_lbrb_t = 0.0f;
    } else {
        s_lbrb_t += dt;
        if (s_lbrb_t >= 5.0f) { s_skip = true; s_skip_target = s_depth; s_lbrb_t = 0.0f; }
    }

    bool atk_edge   = edge(btn->a, s_prev.a);
    bool jump_edge  = edge(btn->b, s_prev.b);
    int  hp0 = s_player.hp, gold0 = s_player.gold;
    if (atk_edge && s_player.atk_cd <= 0 && s_player.atk_t <= 0) rogue_sfx_swing();

    /* Torch burns down slowly; relight it FAST by standing near a lit brazier
     * or lava (a clear, discoverable recharge). Torch pickups also refill it. */
    if (s_player.torch_fuel > 0) s_player.torch_fuel -= dt * 0.7f;
    {
        int pcx = (int)floorf(s_player.pos.x);
        int pcz = (int)floorf(s_player.pos.z);
        int pcy = s_level.floor_y;
        bool near_fire = false;
        for (int dz = -2; dz <= 2 && !near_fire; dz++)
            for (int dx = -2; dx <= 2; dx++) {
                int b  = craft_world_get(pcx + dx, pcy, pcz + dz);
                int b2 = craft_world_get(pcx + dx, pcy - 1, pcz + dz);
                if (b == BLK_LAMP_ON || craft_is_lava_id((uint8_t)b) ||
                    craft_is_lava_id((uint8_t)b2)) { near_fire = true; break; }
            }
        if (near_fire && s_player.torch_fuel < 90.0f)
            s_player.torch_fuel += dt * 18.0f;   /* relight */
    }
    if (s_player.torch_fuel < 0) s_player.torch_fuel = 0;
    if (s_player.torch_fuel > 90.0f) s_player.torch_fuel = 90.0f;
    craft_render_set_player_light(s_player.torch_fuel > 0);
    /* Torch dims + shrinks as fuel runs low — brightness is a resource you
     * watch drain. Full above 14s, fading to a dim ember by 0. */
    {
        float f = s_player.torch_fuel;
        float lvl = f >= 14.0f ? 1.0f : 0.35f + 0.65f * (f / 14.0f);
        craft_render_set_light_intensity(lvl);
        craft_render_set_light_radius(5.0f + 4.0f * lvl);   /* ~9 blocks → ~6.4 */
    }
    /* Light the bubble around the HERO (head height), not the camera. */
    craft_render_set_light_pos(s_player.pos.x, s_player.pos.y + 0.9f, s_player.pos.z);
    /* X-ray only the walls on the camera->hero sightline (a thin cylinder),
     * so just the blocks covering the hero turn translucent. */
    /* Capsule = the hero's rendered body, no halo: only pixels the body
     * actually occupies can fade, and the drawn hero covers all of them. */
    craft_render_set_xray(s_player.pos.x, s_player.pos.y, s_player.pos.z, 0.40f);
    rogue_enemies_set_dark(s_player.torch_fuel <= 0);

    rogue_platform_update(dt);   /* before player: sets platform delta to ride */

    bool starting_attack = atk_edge && s_player.atk_cd <= 0 && s_player.atk_t <= 0;
    rogue_player_update(&s_player, btn, atk_edge, jump_edge, dt,
                        rogue_camera_snapped_yaw(), s_level.floor_y);
    if (s_player.jumped) rogue_sfx_dodge();   /* jump sound */

    /* Reveal the minimap around the hero (fog of war). */
    {
        int pcx = (int)floorf(s_player.pos.x), pcz = (int)floorf(s_player.pos.z);
        for (int dz = -4; dz <= 4; dz++)
            for (int dx = -4; dx <= 4; dx++) {
                if (dx*dx + dz*dz > 20) continue;
                int x = pcx + dx, z = pcz + dz;
                if ((unsigned)x < CRAFT_WORLD_X && (unsigned)z < CRAFT_WORLD_Z)
                    s_visited[z * CRAFT_WORLD_X + x] = 1;
            }
    }

    /* Melee auto-face: snap the swing toward the nearest enemy in lunge
     * range so daggers/swords land where you mean them to. */
    if (starting_attack && s_player.wpn_class == WCLASS_MELEE) {
        float ex, ez;
        if (rogue_enemies_nearest(s_player.pos.x, s_player.pos.z, &ex, &ez)) {
            float dx = ex - s_player.pos.x, dz = ez - s_player.pos.z;
            float reach = s_player.wpn_range * 1.8f;
            if (dx*dx + dz*dz <= reach*reach) s_player.yaw = atan2f(dx, dz);
        }
    }

    /* Lava contact: standing in a lava cell burns over time. */
    {
        int lx = (int)floorf(s_player.pos.x);
        int ly = (int)floorf(s_player.pos.y + 0.05f);
        int lz = (int)floorf(s_player.pos.z);
        if (craft_is_lava_id((uint8_t)craft_world_get(lx, ly, lz)) ||
            craft_is_lava_id((uint8_t)craft_world_get(lx, ly - 1, lz))) {
            /* Lava is INSTANT DEATH — bridges and the platform are the only
             * safe routes (the critical path always has a cross-bridge). */
            s_player.hp = 0;
            s_player.alive = false;
        }
    }

    /* Spike traps. */
    for (int i = 0; i < s_n_trap; i++) {
        if (s_trap_cd[i] > 0) s_trap_cd[i] -= dt;
        float dx = s_player.pos.x - s_trap[i].x, dz = s_player.pos.z - s_trap[i].z;
        if (dx*dx + dz*dz < 0.45f*0.45f && s_trap_cd[i] <= 0) {
            rogue_player_damage(&s_player, 12 + s_depth * 2, s_trap[i]);
            s_trap_cd[i] = 1.1f;
        }
    }

    /* Effective per-hit damage with a crit roll (from aggregated stats). */
    uint32_t asp = s_player.stats.aspects;
    int outdmg = s_player.wpn_dmg;
    if ((int)(loot_rng() % 100) < s_player.stats.crit)
        outdmg = outdmg * s_player.stats.crit_dmg / 100;
    /* Nightstalker aspect: extra damage while the torch is out. */
    if ((asp & (1u << ASP_DARK)) && s_player.torch_fuel <= 0) outdmg = outdmg * 7 / 5;

    /* Melee strike frame → damage every enemy in the swing arc. */
    if (s_player.atk_hit_pending) {
        s_player.atk_hit_pending = false;
        rogue_enemies_set_strike_element(s_player.stats.elem, s_player.stats.elem_pow);
        int hits = rogue_enemies_hit_arc(s_player.pos, s_player.yaw,
                              s_player.wpn_range, s_player.wpn_arc_cos, outdmg);
        /* Chaining aspect: a cleave around the hero on top of the arc. */
        if ((asp & (1u << ASP_CHAIN)) && hits > 0)
            rogue_enemies_hit_radius(s_player.pos.x, s_player.pos.z, 2.2f, outdmg / 2);
        rogue_enemies_set_strike_element(ELEM_NONE, 0);
        if (hits > 0) {
            rogue_sfx_hit();
            s_hitstop = 0.045f;     /* a heartbeat of slow-mo sells the impact */
            /* spark burst at the swing point */
            Vec3 hp = v3(s_player.pos.x + sinf(s_player.yaw) * s_player.wpn_range * 0.7f,
                         s_player.pos.y + 0.6f,
                         s_player.pos.z + cosf(s_player.yaw) * s_player.wpn_range * 0.7f);
            melee_hit_fx(hp, s_player.wpn_type, s_player.stats.elem);
            /* blunt weapons slam a dusty shockwave ring along the ground */
            if (s_player.wpn_type == WT_MACE || s_player.wpn_type == WT_WARHAMMER ||
                s_player.wpn_type == WT_AXE) {
                Vec3 gp = hp; gp.y = s_player.pos.y + 0.08f;
                rogue_particle_burst(gp, 14, 5.5f, 0.30f, RGB(190,176,150), 0.07f);
            }
            int heal = s_player.stats.life_on_hit * hits;
            if (asp & (1u << ASP_LIFESTEAL)) heal += outdmg / 8;  /* Vampiric */
            if (heal) {
                s_player.hp += heal;
                if (s_player.hp > s_player.max_hp) s_player.hp = s_player.max_hp;
            }
        }
    }
    /* Ranged/caster strike frame → fire a projectile (auto-aim a nearby foe). */
    if (s_player.fire_pending) {
        s_player.fire_pending = false;
        float aim = s_player.yaw, ex, ez;
        if (rogue_enemies_nearest(s_player.pos.x, s_player.pos.z, &ex, &ez)) {
            float dx = ex - s_player.pos.x, dz = ez - s_player.pos.z;
            if (dx*dx + dz*dz < s_player.wpn_range * s_player.wpn_range)
                aim = atan2f(dx, dz);
        }
        int pk = (s_player.wpn_type == WT_CROSSBOW) ? PROJ_BOLT
               : (s_player.wpn_type == WT_WAND)     ? PROJ_WAND
               : (s_player.wpn_type == WT_SCEPTER)  ? PROJ_SCEPTER
               : (s_player.wpn_type == WT_STAFF)    ? PROJ_STAFF
               :                                      PROJ_ARROW;
        rogue_proj_fire(s_player.pos, aim, s_player.wpn_proj_speed,
                        outdmg, pk,
                        s_player.wpn_range, (asp & (1u << ASP_PIERCE)) ? 1 : 0,
                        s_player.stats.elem, s_player.stats.elem_pow);
    }

    rogue_enemies_update(&s_player, dt, s_level.floor_y);
    rogue_proj_update(dt, s_level.floor_y);
    {   /* shadow-element drain heals the hero */
        int drain = rogue_enemies_take_drain();
        if (drain > 0) {
            s_player.hp += drain;
            if (s_player.hp > s_player.max_hp) s_player.hp = s_player.max_hp;
        }
    }
    rogue_loot_update(&s_player, dt);
    rogue_particle_update(dt);
    rogue_dmgnum_update(dt);

    /* Drop loot from anything that died this frame — weighted by the slain
     * creature's loot class (goblins/kobolds → gear, fire sprites → gems,
     * slimes/zombies → potions, demons → rare gear, the rest → mostly gold). */
    Vec3 dpos; int dtype;
    while (rogue_enemies_pop_death(&dpos, &dtype)) {
        s_kills++;
        rogue_sfx_enemy_die();
        /* death poof — fire sprites burst in flame, others in dust */
        Vec3 pp = dpos; pp.y += 0.4f;
        uint16_t pc = (dtype == EN_FIRESPRITE) ? RGB(255,140,30)
                    : (dtype == EN_DEMON) ? RGB(200,40,40) : RGB(170,160,150);
        rogue_particle_burst(pp, 12, 4.5f, 0.55f, pc, 0.09f);
        RogueItem it;
        if (dtype == EN_SHOPKEEPER) {
            /* his wares spill onto the floor — and every shopkeeper from
             * now on attacks on sight */
            int spilled = 0;
            while (rogue_shop_take_stock(&it)) {
                Vec3 sp = dpos;
                sp.x += 0.5f * sinf(spilled * 2.1f);
                sp.z += 0.5f * cosf(spilled * 2.1f);
                rogue_loot_drop(&it, sp);
                spilled++;
            }
            if (!s_merchants_hostile) {
                s_merchants_hostile = true;
                rogue_game_toast("The guild will remember this");
            }
        }
        EnemyLoot lk = rogue_enemy_loot(dtype);
        int goldmul = (lk == LOOT_RARE) ? 4 : (lk == LOOT_GOLD ? 2 : 1);
        rogue_item_make_gold(&it, 2 + (int)(loot_rng() % (5 + s_depth * 2)) * goldmul);
        rogue_loot_drop(&it, dpos);
        int r = loot_rng() % 100;
        switch (lk) {
        case LOOT_GEAR:   if (r < 45) { rogue_item_roll_drop(&it, s_depth, loot_rng()); rogue_loot_drop(&it, dpos); } break;
        case LOOT_GEM:    if (r < 50) { rogue_item_make_gem(&it, (GemType)(1 + loot_rng() % (GEM_COUNT - 1))); rogue_loot_drop(&it, dpos); } break;
        case LOOT_POTION: if (r < 45) { rogue_item_make_potion(&it, 30); rogue_loot_drop(&it, dpos); } break;
        case LOOT_RARE:   /* demons always cough up good gear */
            rogue_item_roll_drop(&it, s_depth + 3, loot_rng()); rogue_loot_drop(&it, dpos);
            if (r < 50) { rogue_item_make_gem(&it, (GemType)(1 + loot_rng() % (GEM_COUNT - 1))); rogue_loot_drop(&it, dpos); }
            break;
        default:          if (r < 10) { rogue_item_roll_drop(&it, s_depth, loot_rng()); rogue_loot_drop(&it, dpos); } break;
        }
        if (r >= 88) { rogue_item_make_torch(&it, 30); rogue_loot_drop(&it, dpos); }  /* torches universal */
    }

    /* Chests open automatically when you reach them (loot spills, then
     * vacuums into the backpack). */
    {
        int ci;
        if (rogue_loot_chest_near(s_player.pos.x, s_player.pos.y, s_player.pos.z, &ci))
            rogue_loot_open_chest(ci, s_depth, loot_rng());
    }
    /* Walk up to the counter → open the shop (rising edge) — but only
     * while the shopkeeper is alive and calm. */
    {
        static bool was_on_pad;
        bool on = rogue_shop_pad_near(s_player.pos.x, s_player.pos.y, s_player.pos.z) &&
                  rogue_enemies_shopkeeper_state() == 1;
        if (on && !was_on_pad) rogue_shop_open();
        was_on_pad = on;
    }
    if (rogue_shop_is_open() && rogue_enemies_shopkeeper_state() != 1)
        rogue_shop_close();

    /* Event SFX from state deltas this frame. */
    if (s_player.hp < hp0)   rogue_sfx_hurt();
    if (s_player.gold > gold0) rogue_sfx_pickup();

    /* Mark the down-stairs: a steady drift of teal motes rising out of the
     * trench mouth — visible across the room (and in the dark) without
     * bringing back a solid beam. Teal = down, matching the minimap. */
    {
        static float s_stair_mote_t;
        s_stair_mote_t -= dt;
        if (s_stair_mote_t <= 0.0f) {
            s_stair_mote_t = 0.11f;
            float along = 1.0f + (float)(loot_rng() % 100) / 100.0f;   /* over the steps */
            /* spread across the trench's full width (1 or 2 columns) */
            float side  = ((float)(loot_rng() % 100) / 100.0f) *
                          (s_level.down_wide ? 1.6f : 0.6f) - 0.3f;
            float mx = s_level.down_x + 0.5f + s_level.down_dx * along + s_level.down_px * side;
            float mz = s_level.down_z + 0.5f + s_level.down_dz * along + s_level.down_pz * side;
            rogue_particle_spawn(v3(mx, (float)s_level.floor_y - 0.3f, mz),
                                 0.0f, 1.2f, 0.0f, 1.5f, RGB(90, 245, 225), 0.12f, 0.0f);
        }
    }

    /* Descend by WALKING DOWN the stair trench: trigger once the hero stands
     * in one of the descending step cells, below floor level. */
    int pcx = (int)floorf(s_player.pos.x), pcz = (int)floorf(s_player.pos.z);
    bool on_step = false;
    for (int s = 1; s <= 2 && !on_step; s++)
        for (int w = 0; w <= s_level.down_wide && !on_step; w++)
            on_step = (pcx == s_level.down_x + s * s_level.down_dx + w * s_level.down_px &&
                       pcz == s_level.down_z + s * s_level.down_dz + w * s_level.down_pz);
    if (on_step && s_player.pos.y < (float)s_level.floor_y - 0.4f) {
        rogue_sfx_descend();
        for (int i = 0; i < SLOT_COUNT; i++) s_keep_equip[i] = s_player.equip[i];
        s_keep_gold = s_player.gold;
        s_have_keep = true;
        s_depth++;
        load_level();
        rogue_game_save(1);   /* checkpoint each new floor */
    }

    if (s_band_banner_t > 0) s_band_banner_t -= dt;
    if (s_toast_t > 0) s_toast_t -= dt;

    rogue_camera_follow(s_player.pos, dt);
    rogue_camera_update(dt);
    rogue_camera_get(&s_cam);
    s_prev = *btn;
}

static void gfill(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= CRAFT_FB_H) continue;
        for (int i = x; i < x + w; i++)
            if ((unsigned)i < CRAFT_FB_W) fb[j * CRAFT_FB_W + i] = c;
    }
}

/* Level-skip cheat overlay: a fill bar while LB+RB is held, then the menu. */
static void draw_skip(uint16_t *fb) {
    if (s_lbrb_t > 0.0f && !s_skip) {
        craft_font_draw(fb, "LEVEL SKIP...", 30, 50, RGB(240,210,60));
        int w = (int)(s_lbrb_t / 5.0f * 80.0f);
        gfill(fb, 24, 60, 80, 6, RGB(24,22,34));
        gfill(fb, 24, 60, w, 6, RGB(240,210,60));
    }
    if (s_skip) {
        gfill(fb, 18, 40, 92, 46, RGB(20,18,30));
        gfill(fb, 18, 40, 92, 2, RGB(240,210,60)); gfill(fb, 18, 84, 92, 2, RGB(240,210,60));
        gfill(fb, 18, 40, 2, 46, RGB(240,210,60));  gfill(fb, 108, 40, 2, 46, RGB(240,210,60));
        craft_font_draw(fb, "LEVEL SKIP", 24, 46, RGB(240,210,60));
        char b[24]; snprintf(b, sizeof b, "Depth  %d", s_skip_target);
        craft_font_draw(fb, b, 24, 58, RGB(255,255,255));
        craft_font_draw(fb, "up/dn  A go  B x", 24, 72, RGB(160,160,175));
    }
}

/* Fog-of-war minimap. Shown only on the inventory/menu screen (it's too
 * large to leave on during play), tucked into the free area to the right
 * of the paperdoll's stat column. */
/* Map a minimap cell (i,j) to world (wx,wz), oriented to the current camera
 * so the map matches what you see: up = away-from-camera, right = screen-right.
 * The 90deg snap index picks one of four axis permutations/flips. */
static void mm_cell_to_world(int i, int j, int MS, int *wx, int *wz) {
    int u, v;   /* normalised 0..MS-1 along world +x (u) and +z (v) */
    switch (rogue_camera_yaw_index() & 3) {
        default:
        case 0: u = i;          v = MS - 1 - j; break;
        case 1: u = MS - 1 - j; v = MS - 1 - i; break;
        case 2: u = MS - 1 - i; v = j;          break;
        case 3: u = j;          v = i;          break;
    }
    *wx = u * CRAFT_WORLD_X / MS;
    *wz = v * CRAFT_WORLD_Z / MS;
}
/* Inverse: world (wx,wz) -> minimap cell (i,j). */
static void mm_world_to_cell(int wx, int wz, int MS, int *i, int *j) {
    int u = wx * MS / CRAFT_WORLD_X, v = wz * MS / CRAFT_WORLD_Z;
    switch (rogue_camera_yaw_index() & 3) {
        default:
        case 0: *i = u;          *j = MS - 1 - v; break;
        case 1: *i = MS - 1 - v; *j = MS - 1 - u; break;
        case 2: *i = MS - 1 - u; *j = v;          break;
        case 3: *i = v;          *j = u;          break;
    }
}

static void draw_minimap(uint16_t *fb) {
    const int MS = 36, MX = CRAFT_FB_W - MS - 1, MY = 12;
    craft_font_draw(fb, "MAP", MX, MY - 9, RGB(150,150,160));
    for (int j = -1; j <= MS; j++)
        for (int i = -1; i <= MS; i++) {
            int sx = MX + i, sy = MY + j;
            if ((unsigned)sx >= CRAFT_FB_W || (unsigned)sy >= CRAFT_FB_H) continue;
            if (i < 0 || j < 0 || i >= MS || j >= MS) { fb[sy*CRAFT_FB_W+sx] = RGB(40,38,48); continue; } /* border */
            int wx, wz; mm_cell_to_world(i, j, MS, &wx, &wz);
            uint16_t c = RGB(10, 9, 14);                 /* unexplored */
            if (s_visited[wz * CRAFT_WORLD_X + wx]) {
                int b = craft_world_get(wx, s_level.floor_y, wz);
                if (b == BLK_AIR)                       c = RGB(120,116,130); /* floor */
                else if (craft_is_lava_id((uint8_t)b))  c = RGB(230,110,30);  /* lava */
                else                                    c = RGB(48,46,58);    /* wall */
            }
            fb[sy*CRAFT_FB_W+sx] = c;
        }
    /* markers: down-stairs (teal), up (amber), player (white) */
    #define MM_PT(wx,wz,col) do { \
        int _i, _j; mm_world_to_cell((int)(wx), (int)(wz), MS, &_i, &_j); \
        int _x = MX + _i, _y = MY + _j; \
        for (int a=0;a<2;a++) for (int b=0;b<2;b++){ int xx=_x+a, yy=_y+b; \
            if ((unsigned)xx<CRAFT_FB_W && (unsigned)yy<CRAFT_FB_H) fb[yy*CRAFT_FB_W+xx]=(col); } } while(0)
    MM_PT(s_level.down_x, s_level.down_z, RGB(40,230,210));
    MM_PT(s_level.up_x,   s_level.up_z,   RGB(245,180,60));
    MM_PT((int)s_player.pos.x, (int)s_player.pos.z, RGB(255,255,255));
    #undef MM_PT
}

void rogue_game_get_camera(CraftCamera *out) { *out = s_cam; }
int rogue_game_depth(void) { return s_depth; }
int rogue_game_player_hp(void) { return s_player.hp; }
int rogue_game_player_gold(void) { return s_player.gold; }
float rogue_game_player_y(void) { return s_player.pos.y; }
const char *rogue_game_weapon_name(void) { return s_player.equip[SLOT_WEAPON].name; }

/* Test hook: drop a strong weapon at the hero's feet then run the real
 * equip path (weapon_near -> take -> equip -> drop old). Verifies the
 * gear-defined playstyle swap end-to-end. */
void rogue_game_debug_kill(void) { s_player.hp = 0; s_player.alive = false; s_kills = 7; }
void rogue_game_debug_beam(void) {
    /* plant gear of each rarity a few tiles out so the loot beams are visible */
    for (int k = 0; k < 4; k++) {
        RogueItem it; rogue_item_roll_drop(&it, 2 + k * 6, loot_rng());
        it.rarity = (Rarity)k; it.color = rogue_rarity_color((Rarity)k);
        Vec3 p = s_player.pos; p.x += 2.0f + k * 1.2f; p.z += 2.0f;
        rogue_loot_drop(&it, p);
    }
}
void rogue_game_debug_set_torch(float s) { s_player.torch_fuel = s; rogue_game_tick(&s_prev, 0.0f); }

int rogue_game_player_maxhp(void) { return s_player.max_hp; }
int rogue_game_player_armor(void) { return s_player.stats.armor; }
int rogue_game_player_wdmg(void)  { return s_player.wpn_dmg; }

void rogue_game_debug_gear_up(void);   /* fwd */
void rogue_game_debug_open_shop(void){ s_player.gold=999; rogue_shop_open(); }

/* Fill the backpack with rolled drops + open the inventory (UI screenshot). */
void rogue_game_debug_fill_bag(void) {
    rogue_game_debug_gear_up();   /* equip a set so paperdoll shows gear */
    for (int i = 0; i < 9; i++) {
        RogueItem it;
        rogue_item_roll_drop(&it, 9, loot_rng());
        rogue_inventory_add(&it);
    }
    RogueItem pot; rogue_item_make_potion(&pot, 40); rogue_inventory_add(&pot);
    rogue_inventory_open();
}

/* One of every weapon type into the bag + open inventory (icon check). */
void rogue_game_debug_weapon_sheet(void) {
    rogue_inventory_clear();
    for (int wt = 0; wt < WT_COUNT && wt < ROGUE_BAG_N; wt++) {
        RogueItem it;
        /* roll until we land on this weapon type so base stats/name are real */
        for (int tries = 0; tries < 200; tries++) {
            rogue_item_roll_weapon(&it, 6, loot_rng());
            if (it.wtype == wt) break;
        }
        rogue_inventory_add(&it);
    }
    rogue_inventory_open();
}

static bool dbg_standable(int x, int z);

/* Stand the hero a few tiles in front of the first lava cell (screenshot).
 * mode 2: stand on the chasm's bonus island instead (lava on all sides). */
int rogue_game_debug_goto_lava(int mode) {
    if (mode == 2 && s_level.n_chasm > 0) {
        s_player.pos = v3(s_level.island_x[0] + 0.5f, (float)s_level.floor_y,
                          s_level.island_z[0] + 0.5f);
        return 2;
    }
    if (mode == 3 && s_level.n_chasm > 0) {
        /* Stand ON the cross-bridge at the chasm centre — lava both sides.
         * Spiral out to the nearest standable cell. */
        int cx = s_level.chasm_x[0], cz = s_level.chasm_z[0];
        for (int r = 0; r <= 4; r++)
            for (int dz = -r; dz <= r; dz++)
                for (int dx = -r; dx <= r; dx++) {
                    int ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
                    if (ax != r && az != r) continue;     /* ring cells only */
                    if (dbg_standable(cx + dx, cz + dz)) {
                        s_player.pos = v3(cx + dx + 0.5f, (float)s_level.floor_y,
                                          cz + dz + 0.5f);
                        return 3;
                    }
                }
    }
    /* Default: the chasm's north rim, camera looking south into the pit —
     * but ONLY on a genuinely standable cell (never inside the bank). */
    for (int z = 0; z < CRAFT_WORLD_Z; z++)
        for (int x = 0; x < CRAFT_WORLD_X; x++)
            for (int y = s_level.floor_y - 3; y <= s_level.floor_y; y++)
                if (craft_is_lava_id((uint8_t)craft_world_get(x, y, z))) {
                    for (int back = 2; back <= 6; back++)
                        if (dbg_standable(x, z - back)) {
                            s_player.pos = v3(x + 0.5f, (float)s_level.floor_y,
                                              z - back + 0.5f);
                            return 1;
                        }
                    /* this lava cell has no standable rim — keep scanning */
                }
    return 0;
}

/* Move the hero onto the first water pool found (water-render verification). */
int rogue_game_debug_goto_water(void) {
    for (int z = 0; z < CRAFT_WORLD_Z; z++)
        for (int x = 0; x < CRAFT_WORLD_X; x++)
            if (craft_is_water_id((uint8_t)craft_world_get(x, s_level.floor_y - 1, z))) {
                s_player.pos = v3(x + 0.5f, (float)s_level.floor_y, z + 0.5f);
                return 1;
            }
    return 0;
}

/* Move the hero next to the first room scenery found (scenery render
 * verification). Stands a few cells camera-side so the clutter is in view. */
static bool dbg_standable(int x, int z) {
    int fy = s_level.floor_y;
    return craft_block_solid((BlockId)craft_world_get(x, fy - 1, z)) &&
           craft_world_get(x, fy, z)     == BLK_AIR &&
           craft_world_get(x, fy + 1, z) == BLK_AIR;
}
int rogue_game_debug_goto_deco(void) {
    int mcx = CRAFT_WORLD_X / 2, mcz = CRAFT_WORLD_Z / 2;
    int bx = -1, bz = -1, bestd = 1 << 30;
    /* Prefer a bulky cube set-piece (bookcase/sarcophagus/crystal) near the
     * map centre — the most photogenic scenery. */
    for (int z = 0; z < CRAFT_WORLD_Z; z++)
        for (int x = 0; x < CRAFT_WORLD_X; x++) {
            uint8_t b = (uint8_t)craft_world_get(x, s_level.floor_y, z);
            if (b >= BLK_BOOKCASE && b <= BLK_CRYSTAL) {
                int d = (x - mcx) * (x - mcx) + (z - mcz) * (z - mcz);
                if (d < bestd) { bestd = d; bx = x; bz = z; }
            }
        }
    if (bx < 0) return 0;
    /* Find a standable floor cell a few steps camera-side (-z) of the feature,
     * scanning outward so we never teleport onto the surround terrain. */
    for (int r = 2; r <= 5; r++) {
        if (dbg_standable(bx, bz - r)) {
            s_player.pos = v3(bx + 0.5f, (float)s_level.floor_y, bz - r + 0.5f);
            rogue_camera_init(s_player.pos);
            return 1;
        }
    }
    for (int dz = -4; dz <= 4; dz++)
        for (int dx = -4; dx <= 4; dx++)
            if (dbg_standable(bx + dx, bz + dz)) {
                s_player.pos = v3(bx + dx + 0.5f, (float)s_level.floor_y, bz + dz + 0.5f);
                rogue_camera_init(s_player.pos);
                return 1;
            }
    return 1;
}

/* Stand next to the first prop of the given kind (PROP_TABLE/PROP_BRAZIER). */
int rogue_game_debug_goto_prop(int kind) {
    for (int i = 0; i < s_level.n_prop; i++)
        if (s_level.prop_kind[i] == kind) {
            s_player.pos = v3(s_level.prop_x[i] + 0.5f, (float)s_level.floor_y,
                              s_level.prop_z[i] - 3.0f);
            rogue_camera_init(s_player.pos);
            return 1;
        }
    return 0;
}

/* Teleport ONTO the first descending step (descend-trigger test). */
void rogue_game_debug_step_into_trench(void) {
    s_player.pos = v3(s_level.down_x + s_level.down_dx + 0.5f,
                      (float)s_level.floor_y,
                      s_level.down_z + s_level.down_dz + 0.5f);
}

int rogue_game_debug_goto_shop(void);
void rogue_game_debug_set_depth(int depth);

/* Elemental status check: poison a zombie, chill a second one — print HP
 * over time (DoT must tick) and displacement (chill must halve speed). */
void rogue_game_debug_elemtest(void) {
    float zx = s_player.pos.x, zz = s_player.pos.z - 2.4f;
    rogue_enemies_debug_showcase(EN_ZOMBIE, zx, (float)s_level.floor_y, zz, 0, 0);
    RogueEnemySave es[ROGUE_MAX_ENEMIES];
    int n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
    int hp0 = -1;
    for (int k = 0; k < n; k++) if (es[k].type == EN_ZOMBIE) hp0 = es[k].hp;
    rogue_enemies_set_strike_element(ELEM_POISON, 6);
    rogue_enemies_hit_point(zx, zz, 0.8f, 1);
    rogue_enemies_set_strike_element(ELEM_NONE, 0);
    n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
    int hp1 = -1;
    for (int k = 0; k < n; k++) if (es[k].type == EN_ZOMBIE) hp1 = es[k].hp;
    CraftRawButtons none = {0};
    for (int i = 0; i < 105; i++) rogue_game_tick(&none, 1.0f / 30.0f);  /* 3.5s */
    n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
    int hp2 = -1;
    for (int k = 0; k < n; k++) if (es[k].type == EN_ZOMBIE) hp2 = es[k].hp;
    printf("[elemtest] poison: hp %d -> %d (direct) -> %d (after 3.5s dot)\n",
           hp0, hp1, hp2);
    /* chill: identical zombies, one frosted one not — compare 1.2s lurch */
    float moved[2];
    for (int pass = 0; pass < 2; pass++) {
        rogue_enemies_debug_showcase(EN_ZOMBIE, zx, (float)s_level.floor_y, zz, 0, 0);
        rogue_enemies_set_strike_element(pass == 0 ? ELEM_NONE : ELEM_FROST, 12);
        rogue_enemies_hit_point(zx, zz, 0.8f, 1);
        rogue_enemies_set_strike_element(ELEM_NONE, 0);
        for (int i = 0; i < 36; i++) rogue_game_tick(&none, 1.0f / 30.0f);
        n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
        moved[pass] = 0;
        for (int k = 0; k < n; k++) if (es[k].type == EN_ZOMBIE)
            moved[pass] = fabsf(es[k].x - zx) + fabsf(es[k].z - zz);
    }
    printf("[elemtest] chill: moved %.2f normally vs %.2f chilled\n",
           moved[0], moved[1]);
    /* lightning: hit a zombie, the bolt must arc to the keeper beside it */
    rogue_enemies_debug_showcase(EN_ZOMBIE, zx, (float)s_level.floor_y, zz, 0, 0);
    rogue_enemies_add_shopkeeper(zx + 1.6f, (float)s_level.floor_y, zz, 0, false, s_depth);
    n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
    int khp0 = -1;
    for (int k = 0; k < n; k++) if (es[k].type == EN_SHOPKEEPER) khp0 = es[k].hp;
    rogue_enemies_set_strike_element(ELEM_LIGHTNING, 8);
    rogue_enemies_hit_point(zx, zz, 0.8f, 1);
    rogue_enemies_set_strike_element(ELEM_NONE, 0);
    n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
    int khp1 = -1;
    for (int k = 0; k < n; k++) if (es[k].type == EN_SHOPKEEPER) khp1 = es[k].hp;
    printf("[elemtest] lightning arc: bystander hp %d -> %d (expect -8)\n", khp0, khp1);
    /* shadow: the drain must heal the hero */
    rogue_enemies_debug_showcase(EN_ZOMBIE, zx, (float)s_level.floor_y, zz, 0, 0);
    s_player.hp = 50;
    rogue_enemies_set_strike_element(ELEM_SHADOW, 8);
    rogue_enemies_hit_point(zx, zz, 0.8f, 1);
    rogue_enemies_set_strike_element(ELEM_NONE, 0);
    rogue_game_tick(&none, 1.0f / 30.0f);   /* harvest tick */
    printf("[elemtest] shadow drain: hero hp 50 -> %d (expect 54)\n", s_player.hp);

    /* an elemental gem socketed in the weapon must imbue it */
    RogueItem *w = &s_player.equip[SLOT_WEAPON];
    w->sockets = 1; w->gem[0] = GEM_GLACITE;
    for (int a = 0; a < w->n_affix; a++)
        if (w->affix[a].type >= AFX_FIRE) w->affix[a].type = AFX_DMG;
    rogue_player_recompute(&s_player);
    printf("[elemtest] glacite-socketed weapon: elem=%d pow=%d (2=frost)\n",
           s_player.stats.elem, s_player.stats.elem_pow);
}

float rogue_game_debug_player_yaw(void) { return s_player.yaw; }
void rogue_game_debug_player_pos(float *x, float *z) { *x = s_player.pos.x; *z = s_player.pos.z; }

/* Lava test (chasm level): a zombie over the lava must burn down; a fire
 * sprite must shrug it off. */
void rogue_game_debug_lavatest(void) {
    int fy = s_level.floor_y;
    for (int z = 0; z < CRAFT_WORLD_Z; z++)
        for (int x = 0; x < CRAFT_WORLD_X; x++) {
            uint8_t b1 = (uint8_t)craft_world_get(x, fy - 1, z);
            uint8_t b2 = (uint8_t)craft_world_get(x, fy - 2, z);
            bool surf = craft_is_lava_id(b1) ||
                        (!craft_block_solid((BlockId)b1) && craft_is_lava_id(b2));
            if (!surf) continue;
            CraftRawButtons none = {0};
            RogueEnemySave es[ROGUE_MAX_ENEMIES];
            rogue_enemies_debug_showcase(EN_ZOMBIE, x + 0.5f, (float)fy, z + 0.5f, 0, 0);
            for (int i = 0; i < 45; i++) rogue_game_tick(&none, 1.0f / 30.0f);
            int n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
            int zhp = 0;   /* dead -> stays 0 */
            for (int k = 0; k < n; k++) if (es[k].type == EN_ZOMBIE) zhp = es[k].hp;
            rogue_enemies_debug_showcase(EN_FIRESPRITE, x + 0.5f, (float)fy, z + 0.5f, 0, 0);
            for (int i = 0; i < 45; i++) rogue_game_tick(&none, 1.0f / 30.0f);
            n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
            int fhp = 0;
            for (int k = 0; k < n; k++) if (es[k].type == EN_FIRESPRITE) fhp = es[k].hp;
            printf("[lavatest] zombie hp after 1.5s in lava: %d (expect <=0/dead);"
                   " fire sprite: %d (expect 10)\n", zhp, fhp);
            return;
        }
    printf("[lavatest] no lava on this level\n");
}

/* Force an element onto the equipped weapon (impact/projectile tints). */
void rogue_game_debug_force_element(int elem, int pow) {
    RogueItem *w = &s_player.equip[SLOT_WEAPON];
    if (elem < 1) elem = 1;
    if (elem > ELEM_ARCANE) elem = ELEM_ARCANE;
    w->affix[0].type = (uint8_t)(AFX_FIRE + (elem - 1));
    w->affix[0].val = (int16_t)pow;
    if (w->n_affix < 1) w->n_affix = 1;
    rogue_player_recompute(&s_player);
}

/* Provoke the shopkeeper and leave him alive (combat screenshots). */
void rogue_game_debug_shoppoke(void) {
    if (!s_level.has_shop) return;
    rogue_enemies_hit_radius(s_level.shop_x + 0.5f - s_level.shop_dx,
                             s_level.shop_z + 0.5f - s_level.shop_dz, 0.6f, 5);
}

/* Scripted shopkeeper-aggro test: provoke → he turns wizard; kill → wares
 * spill + the guild goes hostile. Prints each step for the harness. */
void rogue_game_debug_shoptest(void) {
    if (!s_level.has_shop) { printf("[shoptest] no shop\n"); return; }
    rogue_game_debug_goto_shop();          /* fight from the counter */
    float mx = s_level.shop_x + 0.5f - s_level.shop_dx;
    float mz = s_level.shop_z + 0.5f - s_level.shop_dz;
    printf("[shoptest] state0=%d hostile0=%d\n",
           rogue_enemies_shopkeeper_state(), s_merchants_hostile ? 1 : 0);
    rogue_enemies_hit_radius(mx, mz, 0.6f, 5);          /* provoke */
    printf("[shoptest] after-poke state=%d\n", rogue_enemies_shopkeeper_state());
    CraftRawButtons none = {0};
    for (int i = 0; i < 90; i++) {
        rogue_game_tick(&none, 1.0f / 30.0f);
        if ((i % 30) == 29) {
            RogueEnemySave es[ROGUE_MAX_ENEMIES];
            int n = rogue_enemies_export(es, ROGUE_MAX_ENEMIES);
            for (int k = 0; k < n; k++)
                if (es[k].type == EN_SHOPKEEPER)
                    printf("[shoptest] t=%d keeper at (%.1f,%.1f) player (%.1f,%.1f)\n",
                           i + 1, es[k].x, es[k].z, s_player.pos.x, s_player.pos.z);
        }
    }
    printf("[shoptest] after-3s state=%d hp=%d\n",
           rogue_enemies_shopkeeper_state(), s_player.hp);
    /* hunt him down wherever he blinked to and finish him */
    float ex, ez;
    for (int tries = 0; tries < 50 && rogue_enemies_shopkeeper_state(); tries++) {
        if (rogue_enemies_nearest(s_player.pos.x, s_player.pos.z, &ex, &ez))
            rogue_enemies_hit_radius(ex, ez, 9.0f, 80);
        rogue_game_tick(&none, 1.0f / 30.0f);
    }
    RogueGroundSave gr[ROGUE_MAX_GROUND];
    printf("[shoptest] dead state=%d hostile=%d ground=%d\n",
           rogue_enemies_shopkeeper_state(), s_merchants_hostile ? 1 : 0,
           rogue_loot_export_ground(gr, ROGUE_MAX_GROUND));
    /* the next floor's shopkeeper must spawn hostile on sight */
    rogue_game_debug_set_depth(s_depth + 1);
    printf("[shoptest] next-floor state=%d\n", rogue_enemies_shopkeeper_state());
}

/* Stand at the shop counter's customer side, camera facing the stall. */
int rogue_game_debug_goto_shop(void) {
    if (!s_level.has_shop) return 0;
    /* back away from the counter to the farthest STANDABLE cell (shops can
     * sit against the room edge — never bury the hero in the wall) */
    {
        float ox = 1.4f;
        for (float t = 2.6f; t >= 1.4f; t -= 0.6f) {
            int cx = (int)floorf(s_level.shop_x + 0.5f + s_level.shop_dx * t + s_level.shop_dz * 0.8f);
            int cz = (int)floorf(s_level.shop_z + 0.5f + s_level.shop_dz * t + s_level.shop_dx * 0.8f);
            if (dbg_standable(cx, cz)) { ox = t; break; }
        }
        s_player.pos = v3(s_level.shop_x + 0.5f + s_level.shop_dx * ox + s_level.shop_dz * 0.8f,
                          (float)s_level.floor_y,
                          s_level.shop_z + 0.5f + s_level.shop_dz * ox + s_level.shop_dx * 0.8f);
    }
    rogue_camera_init(s_player.pos);
    /* camera view dir should be -shop_d (looking AT the counter) */
    int vx = -s_level.shop_dx, vz = -s_level.shop_dz;
    int n = (vx == 1) ? 1 : (vz == -1) ? 2 : (vx == -1) ? 3 : 0;
    for (int i = 0; i < n; i++) rogue_camera_rotate(+1);
    return 1;
}

/* Stand just outside the down-stairs trench, looking at it (stair visuals).
 * Spins the camera so the view runs straight down the descending steps;
 * mode 2 instead faces the mouth from the room side (tread risers in view). */
void rogue_game_debug_goto_down(int mode) {
    s_player.pos = v3(s_level.down_x + 0.5f - s_level.down_dx * 1.2f - s_level.down_px * 1.1f,
                      (float)s_level.floor_y,
                      s_level.down_z + 0.5f - s_level.down_dz * 1.2f - s_level.down_pz * 1.1f);
    rogue_camera_init(s_player.pos);
    int n = (s_level.down_dx == 1) ? 1 : (s_level.down_dz == -1) ? 2 :
            (s_level.down_dx == -1) ? 3 : 0;
    if (mode == 2) n = (n + 2) & 3;
    for (int i = 0; i < n; i++) rogue_camera_rotate(+1);
}

/* Pose a single enemy 2.4 cells in front of the hero, facing the camera, at
 * a chosen gait phase — for capturing the animation sheet. */
void rogue_game_debug_showcase(int type, float anim, int moving) {
    rogue_enemies_debug_showcase(type, s_player.pos.x, (float)s_level.floor_y,
                                 s_player.pos.z - 2.4f, anim, moving);
}

/* Drop a gold pile + a rare weapon next to the hero (loot-render check:
 * gold should be a ground coin, gear a rarity beam). */
void rogue_game_debug_drop_loot(void) {
    RogueItem it;
    rogue_item_make_gold(&it, 25);
    rogue_loot_drop(&it, v3(s_player.pos.x - 1.0f, s_player.pos.y, s_player.pos.z + 0.4f));
    rogue_item_roll_weapon(&it, 8, loot_rng()); it.rarity = RAR_RARE;
    rogue_loot_drop(&it, v3(s_player.pos.x + 1.0f, s_player.pos.y, s_player.pos.z + 0.4f));
    rogue_item_make_torch(&it, 30);
    rogue_loot_drop(&it, v3(s_player.pos.x - 1.8f, s_player.pos.y, s_player.pos.z - 0.6f));
    rogue_item_make_gem(&it, GEM_TOPAZ);
    rogue_loot_drop(&it, v3(s_player.pos.x + 1.8f, s_player.pos.y, s_player.pos.z - 0.6f));
}

/* Print the live run state (suspend round-trip verification). */
void rogue_game_debug_print_state(const char *tag) {
    RogueGroundSave g[ROGUE_MAX_GROUND];
    int ng = rogue_loot_export_ground(g, ROGUE_MAX_GROUND);
    printf("[state %s] depth=%d pos=(%.2f,%.2f) yaw=%.2f en=%d ground=%d gold=%d hp=%d chest=0x%x\n",
           tag, s_depth, s_player.pos.x, s_player.pos.z, s_player.yaw,
           rogue_enemies_alive_count(), ng, s_player.gold, s_player.hp,
           rogue_loot_chest_mask());
}

/* Suspend round-trip self-test: snapshot → perturb → resume → compare.
 * The two "before" and "after-resume" lines should match. */
void rogue_game_debug_suspend_test(void) {
    rogue_game_debug_print_state("before");
    rogue_game_save_full();
    s_player.pos.x += 9.0f; s_player.gold += 999;   /* corrupt live state */
    rogue_enemies_clear();
    rogue_game_debug_print_state("perturbed");
    try_resume();                                   /* reload from the suspend */
    rogue_game_debug_print_state("after-resume");
}

/* Spawn a spread of floating damage numbers near the hero (FX verification). */
void rogue_game_debug_dmgnum(void) {
    Vec3 p = s_player.pos;
    rogue_dmgnum_spawn(v3(p.x + 1.2f, p.y, p.z + 0.6f), 12,  false);
    rogue_dmgnum_spawn(v3(p.x - 1.1f, p.y, p.z + 1.2f), 37,  false);
    rogue_dmgnum_spawn(v3(p.x + 0.4f, p.y, p.z - 1.1f), 144, false);
    rogue_dmgnum_spawn(v3(p.x,        p.y, p.z),          8,  true);
    rogue_dmgnum_update(0.18f);   /* let them rise a touch before the shot */
}

/* Set up the item-detail page demo: equip a socketed legendary weapon and
 * drop a couple of gems in the bag, cursor on the weapon. */
void rogue_game_debug_detail_setup(void) {
    rogue_inventory_clear();
    RogueItem w;
    for (int t = 0; t < 200; t++) { rogue_item_roll_weapon(&w, 8, loot_rng()); if (w.wtype == WT_SWORD) break; }
    w.rarity = RAR_LEGENDARY; w.aspect = ASP_CHAIN; w.sockets = 2;
    w.gem[0] = GEM_NONE; w.gem[1] = GEM_NONE;
    w.color = rogue_rarity_color(RAR_LEGENDARY);
    rogue_player_equip(&s_player, &w);
    RogueItem g1, g2; rogue_item_make_gem(&g1, GEM_RUBY); rogue_item_make_gem(&g2, GEM_EMERALD);
    rogue_inventory_add(&g1); rogue_inventory_add(&g2);
    rogue_inventory_open();
}

/* Force-equip a specific weapon type (FX verification). */
void rogue_game_debug_force_weapon(int wt) {
    for (int tries = 0; tries < 400; tries++) {
        RogueItem it; rogue_item_roll_weapon(&it, 6, loot_rng());
        if (it.wtype == wt) { rogue_player_equip(&s_player, &it); return; }
    }
}
/* Force the hero's facing (radians) for FX direction checks. */
void rogue_game_debug_set_yaw(float yaw) { s_player.yaw = yaw; }
/* Freeze a mid-stride walk pose (animation check). */
void rogue_game_debug_walkpose(float ph) { s_player.walk_blend = 1.0f; s_player.move_phase = ph; }
/* Reveal the whole fog-of-war map (minimap verification). */
void rogue_game_debug_reveal_map(void) {
    for (int i = 0; i < CRAFT_WORLD_X * CRAFT_WORLD_Z; i++) s_visited[i] = 1;
}

/* Roll + equip one item into every slot (verify stat aggregation). */
void rogue_game_debug_gear_up(void) {
    for (int s = 0; s < SLOT_COUNT; s++) {
        RogueItem it;
        rogue_item_roll_gear(&it, (EquipSlot)s, 10, loot_rng());
        rogue_player_equip(&s_player, &it);
    }
    s_player.hp = s_player.max_hp;
}

void rogue_game_debug_set_depth(int depth) {
    s_depth = depth < 1 ? 1 : depth;
    s_last_band = -1;
    load_level();
}

void rogue_game_debug_drop_weapon(void) {
    RogueItem it;
    rogue_item_roll_drop(&it, 8, loot_rng());
    rogue_loot_drop(&it, s_player.pos);
    RogueItem w; int idx;
    if (rogue_loot_weapon_near(s_player.pos.x, s_player.pos.z, &w, &idx)) {
        RogueItem taken;
        if (rogue_loot_take(idx, &taken)) {
            RogueItem old = s_player.equip[taken.slot];
            rogue_player_equip(&s_player, &taken);
            if (rogue_item_is_equip(&old)) rogue_loot_drop(&old, s_player.pos);
        }
    }
}

/* Headless autopilot step: steer toward the nearest foe (screen-relative,
 * via the snapped camera yaw) and swing when close. For verification only. */
void rogue_game_demo_step(float dt, int frame) {
    CraftRawButtons b = {0};
    float ex, ez;
    if (rogue_enemies_nearest(s_player.pos.x, s_player.pos.z, &ex, &ez)) {
        float dx = ex - s_player.pos.x, dz = ez - s_player.pos.z;
        float dist = sqrtf(dx*dx + dz*dz);
        float yaw = rogue_camera_snapped_yaw();
        float fwd = dx * sinf(yaw) + dz * cosf(yaw);
        float rgt = dx * cosf(yaw) - dz * sinf(yaw);
        if (dist > 1.2f) {
            if (fwd >  0.4f) b.up = true;
            if (fwd < -0.4f) b.down = true;
            if (rgt >  0.4f) b.right = true;
            if (rgt < -0.4f) b.left = true;
        }
        if (dist < 1.9f) b.a = (frame % 6) < 2;   /* in range → swing */
    }
    if (frame % 40 == 10) b.b = true;      /* periodic jump (verify physics) */
    rogue_game_tick(&b, dt);
}

void rogue_game_draw_overlay(uint16_t *fb) {
    /* Wall/floor torches (the room light sources). */
    for (int i = 0; i < s_level.n_torch; i++) {
        Vec3 tp = v3(s_level.torch_x[i] + 0.5f, (float)s_level.floor_y, s_level.torch_z[i] + 0.5f);
        rogue_render_model(&s_cam, fb, tp, 0.0f, torch_model, 3, 0.12f, 0.7f, 0.0f, 256);
    }

    /* Furniture props (tables, braziers). */
    for (int i = 0; i < s_level.n_prop; i++) {
        Vec3 pp = v3(s_level.prop_x[i] + 0.5f, (float)s_level.floor_y, s_level.prop_z[i] + 0.5f);
        if (s_level.prop_kind[i] == PROP_TABLE)
            rogue_render_model(&s_cam, fb, pp, 0.0f, table_model, 5, 0.45f, 0.55f, 0.0f, 256);
        else /* PROP_BRAZIER */
            rogue_render_model(&s_cam, fb, pp, 0.0f, brazier_model, 6, 0.30f, 0.72f, 0.0f, 256);
    }

    /* (The shopkeeper is a real enemy now — rogue_enemy draws him.) */

    /* Spike traps — dark pad + steel spikes (telegraphed; pulses when armed). */
    for (int i = 0; i < s_n_trap; i++) {
        float warn = (s_trap_cd[i] > 0) ? 0.0f : 0.18f;
        RogueCuboid m[5] = {
            { 0.0f, 0.03f, 0.0f, 0.42f, 0.03f, 0.42f, RGB(35, 30, 30) },
            { -0.2f, 0.12f, -0.2f, 0.04f, 0.10f, 0.04f, RGB(190,190,200) },
            {  0.2f, 0.12f, -0.2f, 0.04f, 0.10f, 0.04f, RGB(190,190,200) },
            { -0.2f, 0.12f,  0.2f, 0.04f, 0.10f, 0.04f, RGB(190,190,200) },
            {  0.2f, 0.12f,  0.2f, 0.04f, 0.10f, 0.04f, RGB(190,190,200) },
        };
        rogue_render_model(&s_cam, fb, s_trap[i], 0.0f, m, 5, 0.45f, 0.3f, warn, 256);
    }

    rogue_platform_draw(&s_cam, fb);
    rogue_loot_draw(&s_cam, fb);
    rogue_proj_draw(&s_cam, fb);
    rogue_enemies_draw(&s_cam, fb);
    rogue_player_draw(&s_player, &s_cam, fb, 256);
    rogue_particle_draw(&s_cam, fb);
    rogue_dmgnum_draw(&s_cam, fb);

    if (s_title) { rogue_hud_title(fb, s_best_depth); return; }
    if (rogue_inventory_is_open()) {
        rogue_inventory_draw(fb, &s_player);
        /* map shares the inventory grid only — hide it on the detail sub-pages */
        if (!rogue_inventory_detail_open()) draw_minimap(fb);
        return;
    }
    if (rogue_shop_is_open()) { rogue_shop_draw(fb, &s_player); return; }

    rogue_hud_draw(fb, &s_player, s_depth, rogue_enemies_alive_count());
    if (s_toast_t > 0) rogue_hud_prompt(fb, s_toast);

    if (!s_player.alive) {
        int best = s_depth > s_best_depth ? s_depth : s_best_depth;
        rogue_hud_summary(fb, s_depth, s_player.gold, s_kills, best);
    } else if (s_band_banner_t > 0) {
        const RogueBand *b = rogue_band_get(s_depth);
        rogue_hud_banner(fb, b->name, b->tint);
    }
    draw_skip(fb);   /* cheat: arming bar / level-skip menu on top */
}
