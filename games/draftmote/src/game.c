/* DraftMote — a room-drafting roguelike inspired by Blue Prince's estate
 * puzzle: draft rooms onto a 5x8 blueprint, walk them top-down, manage steps,
 * keys, gems and gold, and reach the Antechamber before you're exhausted.
 *
 * Rooms are 7x7 tile templates (rooms.h): wall ring from the walls_* rule
 * tilesets, floors from floors.png macro-tiles, furniture from the prop
 * sheets (props_meta.h). See DESIGN.md. */
#include "mote_api.h"
#include "mote_build.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "rooms.h"
#include "props_meta.h"
#include "floors.h"
#include "walls_stone.tiles.h"
#include "walls_red.tiles.h"
#include "walls_dark.tiles.h"
#include "doors.h"
#include "props_sheet.h"
#include "props_auth.h"
#include "hero.h"
#include "items.h"
#include "tick.sfx.h"
#include "draft.sfx.h"
#include "door.sfx.h"
#include "coin.sfx.h"
#include "key.sfx.h"
#include "gem.sfx.h"
#include "food.sfx.h"
#include "star.sfx.h"
#include "locked.sfx.h"
#include "unlock.sfx.h"
#include "buy.sfx.h"
#include "row.sfx.h"
#include "win.sfx.h"
#include "lose.sfx.h"

/* ------------------------------------------------------------------ state --- */
enum { GS_TITLE, GS_PLAY, GS_DRAFT, GS_SHOP, GS_MAP, GS_PAUSE, GS_RESULTS };

#define GRID_W 5
#define GRID_H 8
#define ANTE_GI  (0 * GRID_W + 2)          /* top centre */
#define START_GI (7 * GRID_W + 2)          /* bottom centre */
#define ROOM_T 7                            /* tiles per side */
#define TILE 16
#define ROOM_PX (ROOM_T * TILE)             /* 112 */
#define ORG_X 8                             /* interior position on screen */
#define ORG_Y 16
#define START_STEPS 50
#define MAX_LOOT 6
#define MAX_ROOM_PROPS 10

typedef struct {
    uint8_t room;        /* 0xFF = undrafted */
    uint8_t doors;       /* absolute NESW bits */
    uint8_t lock_roll;   /* sides whose lock has been rolled */
    uint8_t lock_on;     /* sides currently locked */
    uint8_t looted;      /* loot slot bits */
    uint8_t chests;      /* opened-chest bits */
    uint8_t entered;     /* first-entry effect given */
    uint8_t swept;       /* sweep bonus given */
} Cell;

static Cell   g_grid[GRID_W * GRID_H];
static uint8_t g_state = GS_TITLE;
static int    g_cur;                        /* grid index the player is in */
static float  g_px, g_py;                   /* player centre, room pixels */
static int    g_face = DIR_S;
static float  g_walk_t;
static int    g_moving;
static float  g_bump_cd;

static int    g_steps, g_keys, g_gems, g_gold;
static uint32_t g_score;
static uint8_t g_master;                    /* master key held */
static uint8_t g_compass;                   /* rotate draft offers (LB/RB) */
static uint8_t g_spyglass;                  /* 4-card draft offers */
static uint8_t g_won;
static int    g_rooms_placed;
static uint32_t g_day_seed;

static uint16_t g_hi;
static uint16_t g_days, g_wins;
static uint8_t g_new_best;

/* draft offer */
static int g_draft_gi, g_draft_entry, g_draft_sel, g_draft_n;
static uint8_t g_cards[4];
static uint8_t g_rot[4];

/* shop */
static int g_shop_sel, g_shop_kind;         /* 0 commissary, 1 locksmith */

static int g_pause_sel;
static float g_result_t;

static char  g_toast[3][40];               /* small queue: combos + entry effects stack up */
static int   g_toast_n;
static float g_toast_t;

/* current room's furniture, parsed from its template */
typedef struct { uint8_t prop; uint8_t x, y; } RoomProp;
static RoomProp g_props_cur[MAX_ROOM_PROPS];
static int g_nprops;

/* current room's treasure chests (interactive: bump to open) */
typedef struct { uint8_t x, y, locked; } RoomChest;
static RoomChest g_chests[4];
static int g_nchests;

static uint8_t g_no_locks;                  /* DRAFT_LOCKS=0 test hook */
static uint8_t g_all_locks;                 /* DRAFT_LOCKS=2 test hook */
static const char *g_force_rooms;           /* DRAFT_ROOMS test hook */

static void toast(const char *msg) {
    if (g_toast_n >= 3) {                              /* full: drop the oldest */
        memmove(g_toast[0], g_toast[1], sizeof g_toast[0] * 2);
        g_toast_n = 2;
        g_toast_t = 1.4f;
    }
    snprintf(g_toast[g_toast_n], sizeof g_toast[0], "%s", msg);
    if (g_toast_n == 0) g_toast_t = 1.6f;
    g_toast_n++;
}
static void toastf(const char *fmt, int v) {
    char b[40];
    snprintf(b, sizeof b, fmt, v);
    toast(b);
}
static void toast_tick(float dt) {
    if (!g_toast_n) return;
    g_toast_t -= dt;
    if (g_toast_t <= 0) {
        memmove(g_toast[0], g_toast[1], sizeof g_toast[0] * 2);
        g_toast_n--;
        g_toast_t = 1.4f;
    }
}

static const MoteImage *k_prop_sheets[2] = { &props_sheet_img, &props_auth_img };
static const MoteAutotile *k_walls[3] = { &walls_stone_at, &walls_red_at, &walls_dark_at };
static const uint8_t k_item_cell[10] = { 0, 0, 1, 2, 3, 4, 5, 5, 9, 12 };  /* IT_* -> items cell */

/* template letter -> prop id ('c'/'x' chests are parsed separately) */
static int prop_of_char(char ch) {
    switch (ch) {
    case 'u': return P_BUSH;
    case 'C': return P_CAMPFIRE;   case 'L': return P_SHELF_BIG;
    case 'l': return P_SHELF_SMALL;
    case 'b': return P_BED_BLUE;   case 'B': return P_BED_RED;
    case 't': return P_TABLE;      case 'h': return P_CHAIR;
    case 's': return P_SOFA;       case 'K': return P_COUNTER;
    case 'S': return P_STOVE;      case 'U': return P_TUB;
    case 'T': return P_TOILET;     case 'd': return P_DESK;
    case 'm': return P_MAP_TABLE;  case 'W': return P_WASHER;
    case 'r': return P_BARREL;     case 'g': return P_GOLD_PILE;
    case 'w': return P_WORKBENCH;  case 'p': return P_PLANT;
    }
    return -1;
}

/* ------------------------------------------------------- combos + goals ----- */
/* room tags: adjacent pairs of complementary tags score a combo on placement */
enum { TAG_NONE = 0, TAG_FOOD, TAG_REST, TAG_BATH, TAG_BOOK, TAG_DRINK, TAG_GRAND };
static const uint8_t k_tag[R_COUNT] = {
    [R_KITCHEN] = TAG_FOOD, [R_DINING] = TAG_FOOD, [R_PANTRY] = TAG_FOOD,
    [R_BEDROOM] = TAG_REST, [R_SUITE] = TAG_REST, [R_GUEST] = TAG_REST, [R_LOUNGE] = TAG_REST,
    [R_WASHROOM] = TAG_BATH, [R_LAUNDRY] = TAG_BATH,
    [R_LIBRARY] = TAG_BOOK, [R_STUDY] = TAG_BOOK, [R_DRAFTING] = TAG_BOOK,
    [R_WINECELLAR] = TAG_DRINK, [R_CELLAR] = TAG_DRINK, [R_HEARTH] = TAG_DRINK,
    [R_GREATHALL] = TAG_GRAND, [R_CHAPEL] = TAG_GRAND, [R_FOYER] = TAG_GRAND,
    [R_NOOK] = TAG_BOOK, [R_ATELIER] = TAG_BOOK,
    [R_SCULLERY] = TAG_FOOD, [R_BUNK] = TAG_REST, [R_GAMES] = TAG_REST,
};
typedef struct { uint8_t a, b; uint8_t pts; const char *name; } ComboDef;
static const ComboDef k_combos[] = {
    { TAG_FOOD, TAG_FOOD,   30, "SERVICE WING +30" },
    { TAG_REST, TAG_BATH,   30, "EN-SUITE +30" },
    { TAG_BOOK, TAG_BOOK,   30, "THE ARCHIVE +30" },
    { TAG_DRINK, TAG_FOOD,  25, "WELL STOCKED +25" },
    { TAG_DRINK, TAG_DRINK, 30, "CELLARAGE +30" },
    { TAG_GRAND, TAG_GRAND, 40, "PROCESSION +40" },
    { TAG_REST, TAG_REST,   20, "QUIET WING +20" },
};

/* two goals a day, dealt from the day seed */
typedef struct { const char *name; uint8_t target; uint16_t pts; } GoalDef;
enum { GO_GREENS, GO_CHESTS, GO_ROOMS, GO_SWEEPS, GO_COMBOS, GO_RANKS, GO_GOLD, GO_KEYS, GO_N };
static const GoalDef k_goals[GO_N] = {
    [GO_GREENS] = { "GREENS", 3, 150 },
    [GO_CHESTS] = { "CHESTS", 3, 100 },
    [GO_ROOMS]  = { "ROOMS", 10, 100 },
    [GO_SWEEPS] = { "SWEEPS", 4, 120 },
    [GO_COMBOS] = { "COMBOS", 2, 120 },
    [GO_RANKS]  = { "RANKS", 2, 150 },
    [GO_GOLD]   = { "GOLD HELD", 25, 100 },
    [GO_KEYS]   = { "KEYS HELD", 4, 100 },
};
static uint8_t g_goal[2], g_goal_prog[2], g_goal_done[2];

static void goal_progress(int kind, int value, int absolute) {
    for (int i = 0; i < 2; i++) {
        if (g_goal[i] != kind || g_goal_done[i]) continue;
        int p = absolute ? value : g_goal_prog[i] + value;
        g_goal_prog[i] = (uint8_t)mote_clampi(p, 0, 255);
        if (g_goal_prog[i] >= k_goals[kind].target) {
            g_goal_done[i] = 1;
            g_score += k_goals[kind].pts;
            char buf[40];
            snprintf(buf, sizeof buf, "GOAL! %s +%d", k_goals[kind].name, k_goals[kind].pts);
            toast(buf);
            mote->audio_play_sfx(&row_sfx, 1.0f);
        }
    }
}
static void goal_check_held(void) {
    goal_progress(GO_GOLD, g_gold, 1);
    goal_progress(GO_KEYS, g_keys, 1);
}

/* room blurbs for draft cards */
static const char *k_blurb[R_COUNT] = {
    [R_HALLWAY] = "CORRIDOR",       [R_WPASS] = "CORRIDOR",
    [R_EPASS] = "CORRIDOR",         [R_FOYER] = "3 DOORS",
    [R_GREATHALL] = "4 DOORS +50",  [R_LOUNGE] = "+6 STEPS",
    [R_DRAWING] = "+25, +1 GEM",    [R_DINING] = "+4 STEPS",
    [R_KITCHEN] = "+4, FOOD",       [R_PANTRY] = "+4, FOOD",
    [R_BEDROOM] = "+6 STEPS",       [R_SUITE] = "+12, +1 KEY",
    [R_WASHROOM] = "+4 STEPS",      [R_LIBRARY] = "BIG STAR",
    [R_STUDY] = "+50, +1 GEM",      [R_DRAFTING] = "THE COMPASS",
    [R_LAUNDRY] = "A KEY INSIDE",   [R_STORE] = "COINS+STAR",
    [R_CELLAR] = "2 KEYS",          [R_LOCKSMITH] = "KEY SHOP",
    [R_COMMISSARY] = "SHOP",        [R_HEARTH] = "+8 STEPS",
    [R_STILLROOM] = "TONICS",       [R_TERRACE] = "GREEN +1GEM",
    [R_GARDEN] = "GREEN +1GEM",     [R_SUNROOM] = "GREEN +2GEM",
    [R_VAULT] = "GOLD HOARD",       [R_GUEST] = "+4, CHEST",
    [R_CHAPEL] = "BIG STAR",        [R_ARMORY] = "KEYS, CHEST",
    [R_WINECELLAR] = "TONIC, GOLD", [R_ORCHARD] = "GREEN, FOOD",
    [R_TREASURY] = "2 CHESTS",      [R_NOOK] = "+25 PTS",
    [R_SCULLERY] = "+2 STEPS",      [R_SOLARIUM] = "GREEN +1GEM",
    [R_GAMES] = "+25, STAR",        [R_BUNK] = "+8 STEPS",
    [R_ATELIER] = "STAR + GEM",
};

/* ------------------------------------------------------------------- save --- */
typedef struct { uint32_t magic; uint16_t hi, days, wins, pad; } SaveBlob;
#define SAVE_MAGIC 0x54465244u  /* 'DRFT' */

static void save_progress(void) {
    SaveBlob b = { SAVE_MAGIC, g_hi, g_days, g_wins, 0 };
    mote->save(0, &b, sizeof b);
}
static void load_progress(void) {
    SaveBlob b;
    if (mote->load(0, &b, sizeof b) == sizeof b && b.magic == SAVE_MAGIC) {
        g_hi = b.hi; g_days = b.days; g_wins = b.wins;
    }
}

/* ------------------------------------------------------------------- grid --- */
static int gi_col(int gi) { return gi % GRID_W; }
static int gi_row(int gi) { return gi / GRID_W; }
static int neighbor(int gi, int side) {
    int c = gi_col(gi), r = gi_row(gi);
    if (side == DIR_N) r--; else if (side == DIR_S) r++;
    else if (side == DIR_E) c++; else c--;
    if (c < 0 || c >= GRID_W || r < 0 || r >= GRID_H) return -1;
    return r * GRID_W + c;
}

/* door mask for a shape entered from `entry`, at orientation `rot`.
 * With the Compass, 2-door rooms swing their second door and T-rooms pick
 * which side stays walled — the entry door never moves. */
static uint8_t orient_mask(uint8_t shape, int entry, int rot) {
    int fwd = entry ^ 2, left = (fwd + 3) & 3, right = (fwd + 1) & 3;
    int opts[3];
    switch (shape) {
    case SH_DEAD: return DBIT(entry);
    case SH_X:    return 0xF;
    case SH_STR: case SH_L: case SH_R: {
        opts[0] = fwd; opts[1] = left; opts[2] = right;
        int base = shape == SH_STR ? 0 : shape == SH_L ? 1 : 2;
        return DBIT(entry) | DBIT(opts[(base + rot) % 3]);
    }
    default: /* SH_T */
        opts[0] = fwd; opts[1] = left; opts[2] = right;   /* which stays walled */
        return (uint8_t)(0xF & ~DBIT(opts[rot % 3]));
    }
}

/* door visual/interaction state */
enum { DS_NONE, DS_SEALED, DS_CLOSED, DS_OPEN, DS_LOCKED, DS_GOLD };

static int door_state(int gi, int side) {
    const Cell *cl = &g_grid[gi];
    if (!(cl->doors & DBIT(side))) return DS_NONE;
    int n = neighbor(gi, side);
    if (n < 0) return DS_SEALED;                       /* faces off the estate */
    const Cell *nc = &g_grid[n];
    if (nc->room != 0xFF) {
        if (!(nc->doors & DBIT(side ^ 2))) return DS_SEALED;   /* dead door */
        if (nc->room == R_ANTE && !g_won) return DS_GOLD;
        if ((cl->lock_on & DBIT(side)) || (nc->lock_on & DBIT(side ^ 2))) return DS_LOCKED;
        return DS_OPEN;
    }
    if ((cl->lock_roll & DBIT(side)) && (cl->lock_on & DBIT(side))) return DS_LOCKED;
    return DS_CLOSED;
}

/* --------------------------------------------------------- room furniture --- */
static void parse_room_props(int gi) {
    g_nprops = 0;
    g_nchests = 0;
    const char *t = k_rooms[g_grid[gi].room].tmpl;
    for (int ty = 0; ty < ROOM_T; ty++)
        for (int tx = 0; tx < ROOM_T; tx++) {
            char ch = t[ty * ROOM_T + tx];
            if ((ch == 'c' || ch == 'x') && g_nchests < 4) {
                g_chests[g_nchests].x = (uint8_t)(tx * TILE);
                g_chests[g_nchests].y = (uint8_t)(ty * TILE);
                g_chests[g_nchests].locked = ch == 'x';
                g_nchests++;
                continue;
            }
            int p = prop_of_char(ch);
            if (p >= 0 && g_nprops < MAX_ROOM_PROPS) {
                g_props_cur[g_nprops].prop = (uint8_t)p;
                g_props_cur[g_nprops].x = (uint8_t)(tx * TILE);
                g_props_cur[g_nprops].y = (uint8_t)(ty * TILE);
                g_nprops++;
            }
        }
}

/* ------------------------------------------------------------- collisions --- */
#define WALL_PX 8                           /* thin painted wall band */

static int box_solid(float x, float y) {
    if (x - 4 < WALL_PX || x + 4 > ROOM_PX - WALL_PX || y - 4 < WALL_PX || y + 4 > ROOM_PX - WALL_PX)
        return 1;
    for (int i = 0; i < g_nprops; i++) {
        const PropDef *d = &k_props[g_props_cur[i].prop];
        float px0 = g_props_cur[i].x, py0 = g_props_cur[i].y;
        if (x + 4 > px0 && x - 4 < px0 + d->fw && y + 4 > py0 && y - 4 < py0 + d->fh)
            return 1;
    }
    for (int i = 0; i < g_nchests; i++)
        if (x + 4 > g_chests[i].x && x - 4 < g_chests[i].x + 20 &&
            y + 4 > g_chests[i].y && y - 4 < g_chests[i].y + 16)
            return 1;
    return 0;
}

/* which chest a blocked probe point is pushing on, or -1 */
static int chest_at_px(float x, float y) {
    for (int i = 0; i < g_nchests; i++)
        if (x >= g_chests[i].x - 2 && x < g_chests[i].x + 22 &&
            y >= g_chests[i].y - 2 && y < g_chests[i].y + 18)
            return i;
    return -1;
}

/* which door a blocked probe point is pushing on, or -1 */
static int door_side_of_px(float x, float y) {
    if (x >= 46 && x < 66) {
        if (y < WALL_PX + 4) return DIR_N;
        if (y >= ROOM_PX - WALL_PX - 4) return DIR_S;
    }
    if (y >= 46 && y < 66) {
        if (x < WALL_PX + 4) return DIR_W;
        if (x >= ROOM_PX - WALL_PX - 4) return DIR_E;
    }
    return -1;
}

/* ---------------------------------------------------------------- loot ------ */
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

static int loot_count(const RoomDef *d) {
    int n = 0;
    while (n < MAX_LOOT && d->loot[n] != IT_NONE) n++;
    return n;
}

static int pt_in_props(float x, float y, float pad) {
    for (int i = 0; i < g_nprops; i++) {
        const PropDef *d = &k_props[g_props_cur[i].prop];
        if (x > g_props_cur[i].x - pad && x < g_props_cur[i].x + d->fw + pad &&
            y > g_props_cur[i].y - pad && y < g_props_cur[i].y + d->fh + pad)
            return 1;
    }
    for (int i = 0; i < g_nchests; i++)
        if (x > g_chests[i].x - pad && x < g_chests[i].x + 20 + pad &&
            y > g_chests[i].y - pad && y < g_chests[i].y + 16 + pad)
            return 1;
    return 0;
}

/* deterministic per-day open-floor spots for the CURRENT room's pickups */
static int loot_positions(int gi, uint8_t out[MAX_LOOT][2]) {
    const RoomDef *d = &k_rooms[g_grid[gi].room];
    int want = loot_count(d);
    uint32_t r = hash32(g_day_seed ^ (uint32_t)(gi * 2654435761u));
    int placed = 0;
    for (int tries = 0; tries < 240 && placed < want; tries++) {
        r = hash32(r + tries);
        float x = 24 + (float)(r % 64u), y = 24 + (float)((r >> 12) % 64u);
        if (pt_in_props(x, y, 7)) continue;
        int ok = 1;
        for (int i = 0; i < placed; i++) {
            float dx = x - out[i][0], dy = y - out[i][1];
            if (dx * dx + dy * dy < 16 * 16) ok = 0;
        }
        if (!ok) continue;
        out[placed][0] = (uint8_t)x; out[placed][1] = (uint8_t)y;
        placed++;
    }
    return placed;
}

/* --------------------------------------------------------------- economy ---- */
static void apply_entry_effects(int gi) {
    Cell *cl = &g_grid[gi];
    if (cl->entered) return;
    cl->entered = 1;
    const RoomDef *d = &k_rooms[cl->room];
    if (d->steps) { g_steps += d->steps; mote->audio_play_sfx(&food_sfx, 0.7f); toastf("+%d STEPS", d->steps); }
    if (d->keys)  { g_keys += d->keys; mote->audio_play_sfx(&key_sfx, 0.9f); toastf("+%d KEYS", d->keys); }
    if (d->gemsg) { g_gems += d->gemsg; mote->audio_play_sfx(&gem_sfx, 0.9f); toastf("+%d GEMS", d->gemsg); }
    if (d->pts)   { g_score += d->pts; mote->audio_play_sfx(&star_sfx, 0.9f); toastf("+%d PTS", d->pts); }
    if ((d->flags & RF_COMPASS) && !g_compass) {
        g_compass = 1;
        mote->audio_play_sfx(&star_sfx, 1.0f);
        toast("THE COMPASS! LB/RB TURNS DRAFTS");
    }
}

static void end_day(int won) {
    g_won = won;
    g_result_t = 0;
    if (won) {
        uint32_t bonus = 500 + (uint32_t)g_steps * 10 + (uint32_t)g_keys * 25
                       + (uint32_t)g_gems * 15 + (uint32_t)g_gold * 2;
        g_score += bonus;
        g_wins++;
        mote->audio_play_sfx(&win_sfx, 1.0f);
    } else {
        mote->audio_play_sfx(&lose_sfx, 0.9f);
    }
    g_days++;
    g_new_best = g_score > g_hi;
    if (g_new_best) g_hi = (uint16_t)mote_clampi((int)g_score, 0, 65535);
    save_progress();
    g_state = GS_RESULTS;
}

/* ------------------------------------------------------------- transitions --- */
static void enter_room(int gi, int entry_side) {
    g_cur = gi;
    g_steps--;
    parse_room_props(gi);
    mote->audio_play_sfx(&door_sfx, 0.8f);
    switch (entry_side) {
    case DIR_N: g_px = 56; g_py = 24; break;
    case DIR_S: g_px = 56; g_py = 88; break;
    case DIR_E: g_px = 88; g_py = 56; break;
    default:    g_px = 24; g_py = 56; break;
    }
    if (g_grid[gi].room == R_ANTE) { end_day(1); return; }
    apply_entry_effects(gi);
    if (g_steps <= 0) { g_steps = 0; end_day(0); return; }
    if (g_steps == 5) toast("5 STEPS LEFT!");
}

/* build a draft offer for cell gi entered from entry side */
static int room_placed(uint8_t id) {
    for (int i = 0; i < GRID_W * GRID_H; i++)
        if (g_grid[i].room == id) return 1;
    return 0;
}

static uint8_t pick_room(uint8_t e0, uint8_t e1, uint8_t e2) {
    int total = 0;
    for (uint8_t i = DRAFT_FIRST; i < R_COUNT; i++) {
        if (i == e0 || i == e1 || i == e2) continue;
        if ((k_rooms[i].flags & RF_UNIQUE) && room_placed(i)) continue;
        total += k_rarity_weight[k_rooms[i].rarity];
    }
    int roll = (int)(mote_rand() % (uint32_t)total);
    for (uint8_t i = DRAFT_FIRST; i < R_COUNT; i++) {
        if (i == e0 || i == e1 || i == e2) continue;
        if ((k_rooms[i].flags & RF_UNIQUE) && room_placed(i)) continue;
        roll -= k_rarity_weight[k_rooms[i].rarity];
        if (roll < 0) return i;
    }
    return R_HALLWAY;
}

static void deal_cards(void) {
    g_draft_n = g_spyglass ? 4 : 3;
    memset(g_rot, 0, sizeof g_rot);
    if (g_force_rooms) {                              /* DRAFT_ROOMS=a,b,c (every offer) */
        int a, b, c;
        if (sscanf(g_force_rooms, "%d,%d,%d", &a, &b, &c) == 3) {
            g_cards[0] = (uint8_t)a; g_cards[1] = (uint8_t)b; g_cards[2] = (uint8_t)c;
            g_cards[3] = (uint8_t)a;
            return;
        }
        g_force_rooms = 0;
    }
    g_cards[0] = pick_room(0xFF, 0xFF, 0xFF);
    g_cards[1] = pick_room(g_cards[0], 0xFF, 0xFF);
    g_cards[2] = pick_room(g_cards[0], g_cards[1], 0xFF);
    g_cards[3] = pick_room(g_cards[0], g_cards[1], g_cards[2]);
    /* guarantee one free, unlocked pick */
    int ok = 0;
    for (int i = 0; i < g_draft_n; i++)
        if (k_rooms[g_cards[i]].gems == 0 && !(k_rooms[g_cards[i]].flags & RF_LOCKED)) ok = 1;
    if (!ok) {
        static const uint8_t safe[4] = { R_HALLWAY, R_WPASS, R_EPASS, R_BEDROOM };
        g_cards[g_draft_n - 1] = safe[mote_rand() % 4];
    }
}

static int card_affordable(uint8_t id) {
    if (k_rooms[id].gems > g_gems) return 0;
    if ((k_rooms[id].flags & RF_LOCKED) && g_keys < 1 && !g_master) return 0;
    return 1;
}

static void place_card(int slot) {
    uint8_t id = g_cards[slot];
    const RoomDef *d = &k_rooms[id];
    g_gems -= d->gems;
    if (d->flags & RF_LOCKED) {
        if (g_master) toast("MASTER KEY OPENS THE VAULT");
        else { g_keys--; toast("USED A KEY"); }
    }
    Cell *cl = &g_grid[g_draft_gi];
    cl->room = id;
    cl->doors = orient_mask(d->shape, g_draft_entry, g_rot[slot]);
    g_rooms_placed++;
    g_score += 10u * (d->rarity + 1);
    mote->audio_play_sfx(&draft_sfx, 0.9f);
    goal_progress(GO_ROOMS, 1, 0);
    if (d->flags & RF_GREEN) goal_progress(GO_GREENS, 1, 0);
    /* green adjacency bonus */
    if (d->flags & RF_GREEN) {
        int n = 0;
        for (int s = 0; s < 4; s++) {
            int ni = neighbor(g_draft_gi, s);
            if (ni >= 0 && g_grid[ni].room != 0xFF && (k_rooms[g_grid[ni].room].flags & RF_GREEN)) n++;
        }
        if (n) { g_score += 25u * n; toastf("GARDEN BONUS +%d", 25 * n); mote->audio_play_sfx(&star_sfx, 0.9f); }
    }
    /* tag combos with the neighbours (service wing, en-suite, the archive...) */
    uint8_t mytag = k_tag[id];
    if (mytag) {
        for (int s = 0; s < 4; s++) {
            int ni = neighbor(g_draft_gi, s);
            if (ni < 0 || g_grid[ni].room == 0xFF) continue;
            uint8_t nt = k_tag[g_grid[ni].room];
            if (!nt) continue;
            for (unsigned k = 0; k < sizeof k_combos / sizeof k_combos[0]; k++) {
                if ((k_combos[k].a == mytag && k_combos[k].b == nt) ||
                    (k_combos[k].a == nt && k_combos[k].b == mytag)) {
                    g_score += k_combos[k].pts;
                    toast(k_combos[k].name);
                    mote->audio_play_sfx(&star_sfx, 0.9f);
                    goal_progress(GO_COMBOS, 1, 0);
                    break;
                }
            }
        }
    }
    /* matched wings: the mirror cell across the centre column holds the same room */
    {
        int mc = (GRID_W - 1) - gi_col(g_draft_gi);
        int mi = gi_row(g_draft_gi) * GRID_W + mc;
        if (mc != gi_col(g_draft_gi) && g_grid[mi].room == id) {
            g_score += 50;
            toast("MATCHED WINGS +50");
            mote->audio_play_sfx(&star_sfx, 1.0f);
            goal_progress(GO_COMBOS, 1, 0);
        }
    }
    /* rank complete bonus */
    int r = gi_row(g_draft_gi), full = 1;
    for (int c = 0; c < GRID_W; c++)
        if (g_grid[r * GRID_W + c].room == 0xFF) full = 0;
    if (full) {
        g_score += 100; toast("RANK COMPLETE +100"); mote->audio_play_sfx(&row_sfx, 1.0f);
        goal_progress(GO_RANKS, 1, 0);
    }
    g_state = GS_PLAY;
    enter_room(g_draft_gi, g_draft_entry);
}

/* bump into the door on `side` of the current room */
static void door_try(int side) {
    Cell *cl = &g_grid[g_cur];
    int st = door_state(g_cur, side);
    int n = neighbor(g_cur, side);
    if (st == DS_NONE || st == DS_SEALED) return;

    if (st == DS_GOLD) {                       /* the Antechamber */
        if (g_master) { toast("THE MASTER KEY TURNS..."); enter_room(n, side ^ 2); }
        else if (g_keys >= 3) { g_keys -= 3; mote->audio_play_sfx(&unlock_sfx, 1.0f); enter_room(n, side ^ 2); }
        else { toastf("TRIPLE LOCKED - NEED 3 KEYS (%d)", g_keys); mote->audio_play_sfx(&locked_sfx, 0.9f); }
        return;
    }

    if (st == DS_LOCKED) {
        if (g_master) {
            toast("MASTER KEY");
        } else if (g_keys > 0) {
            g_keys--; toast("UNLOCKED  -1 KEY");
        } else {
            toast("LOCKED - NEED A KEY");
            mote->audio_play_sfx(&locked_sfx, 0.9f);
            return;
        }
        mote->audio_play_sfx(&unlock_sfx, 1.0f);
        cl->lock_on &= (uint8_t)~DBIT(side);
        if (n >= 0) g_grid[n].lock_on &= (uint8_t)~DBIT(side ^ 2);
        st = (g_grid[n].room != 0xFF) ? DS_OPEN : DS_CLOSED;
        if (st == DS_OPEN) { enter_room(n, side ^ 2); return; }
    }

    if (st == DS_OPEN) { enter_room(n, side ^ 2); return; }

    /* DS_CLOSED: an undrafted cell — roll the lock, then draft */
    if (!(cl->lock_roll & DBIT(side))) {
        cl->lock_roll |= DBIT(side);
        int p = g_no_locks ? 0 : g_all_locks ? 100 : 5 + 7 * (7 - gi_row(n));
        if ((int)(mote_rand() % 100u) < p) {
            cl->lock_on |= DBIT(side);
            toast("LOCKED - NEED A KEY");
            mote->audio_play_sfx(&locked_sfx, 0.9f);
            if (g_keys == 0 && !g_master) return;
        }
    }
    if (cl->lock_on & DBIT(side)) {
        if (g_master) toast("MASTER KEY");
        else if (g_keys > 0) { g_keys--; toast("UNLOCKED  -1 KEY"); }
        else { mote->audio_play_sfx(&locked_sfx, 0.9f); return; }
        mote->audio_play_sfx(&unlock_sfx, 1.0f);
        cl->lock_on &= (uint8_t)~DBIT(side);
    }
    g_draft_gi = n;
    g_draft_entry = side ^ 2;
    g_draft_sel = 0;
    deal_cards();
    mote->audio_play_sfx(&tick_sfx, 0.8f);
    g_state = GS_DRAFT;
}

/* ------------------------------------------------------------------ chests --- */
static void chest_try(int i) {
    Cell *cl = &g_grid[g_cur];
    if (cl->chests & (1 << i)) return;                 /* already opened */
    if (g_chests[i].locked && !g_master) {
        if (g_keys > 0) { g_keys--; toast("UNLOCKED THE CHEST  -1 KEY"); }
        else { toast("PADLOCKED - NEED A KEY"); mote->audio_play_sfx(&locked_sfx, 0.9f); return; }
    }
    cl->chests |= (uint8_t)(1 << i);
    mote->audio_play_sfx(&unlock_sfx, 1.0f);
    g_score += 10;
    /* seeded contents; padlocked chests hold more */
    uint32_t r = hash32(g_day_seed ^ (uint32_t)(g_cur * 2654435761u) ^ (0xC0FFEEu + (uint32_t)i));
    int gold = g_chests[i].locked ? 4 + (int)(r % 4u) : 2 + (int)(r % 3u);
    g_gold += gold;
    int roll = (int)((r >> 8) % 100u);
    char buf[40];
    const char *extra = "";
    if (g_chests[i].locked) {
        if (roll < 25)      { g_gems++; extra = " +GEM"; }
        else if (roll < 55) { g_keys++; extra = " +KEY"; }
        else if (roll < 78) { g_score += 75; extra = " +75!"; mote->audio_play_sfx(&star_sfx, 1.0f); }
        else                { g_gold += 3; gold += 3; }
    } else {
        if (roll < 18)      { g_gems++; extra = " +GEM"; }
        else if (roll < 32) { g_keys++; extra = " +KEY"; }
        else if (roll < 48) { g_score += 25; extra = " +25"; mote->audio_play_sfx(&star_sfx, 0.9f); }
    }
    mote->audio_play_sfx(&coin_sfx, 0.9f);
    snprintf(buf, sizeof buf, "CHEST: %d GOLD%s", gold, extra);
    toast(buf);
    goal_progress(GO_CHESTS, 1, 0);
    goal_check_held();
}

/* ------------------------------------------------------------------ pickups -- */
static void pickups_tick(void) {
    Cell *cl = &g_grid[g_cur];
    const RoomDef *d = &k_rooms[cl->room];
    uint8_t pos[MAX_LOOT][2];
    int n = loot_positions(g_cur, pos);
    int all = 1;
    for (int i = 0; i < n; i++) {
        if (cl->looted & (1 << i)) continue;
        float ix = pos[i][0], iy = pos[i][1];
        if (g_px - 5 < ix + 6 && g_px + 5 > ix - 6 && g_py - 5 < iy + 6 && g_py + 5 > iy - 6) {
            cl->looted |= (uint8_t)(1 << i);
            switch (d->loot[i]) {
            case IT_COIN: g_gold++; g_score += 5; mote->audio_play_sfx(&coin_sfx, 0.8f); break;
            case IT_KEY:  g_keys++; mote->audio_play_sfx(&key_sfx, 0.9f); toast("A KEY!"); break;
            case IT_GEM:  g_gems++; mote->audio_play_sfx(&gem_sfx, 0.9f); break;
            case IT_FOOD: g_steps += 6; mote->audio_play_sfx(&food_sfx, 0.8f); toast("+6 STEPS"); break;
            case IT_POTION: g_steps += 10; mote->audio_play_sfx(&food_sfx, 0.9f); toast("TONIC! +10 STEPS"); break;
            case IT_POUCH: g_gold += 3; g_score += 5; mote->audio_play_sfx(&coin_sfx, 0.9f); toast("POUCH: +3 GOLD"); break;
            case IT_STAR: g_score += 25; mote->audio_play_sfx(&star_sfx, 0.9f); toast("+25"); break;
            case IT_STAR2: g_score += 75; mote->audio_play_sfx(&star_sfx, 1.0f); toast("+75!"); break;
            case IT_STAR3: g_score += 100; mote->audio_play_sfx(&star_sfx, 1.0f); toast("+100!"); break;
            }
        }
        if (!(cl->looted & (1 << i))) all = 0;
    }
    if (n && all && !cl->swept) {
        cl->swept = 1; g_score += 20; toast("ROOM SWEPT +20");
        goal_progress(GO_SWEEPS, 1, 0);
    }
    goal_check_held();
}

/* ------------------------------------------------------------------- shops --- */
typedef struct { const char *name; int price; } ShopItem;
static const ShopItem k_shop_com[4]  = { { "KEY", 8 }, { "GEM", 5 }, { "SNACK +8", 6 }, { "SPYGLASS", 12 } };
static const ShopItem k_shop_lock[2] = { { "KEY", 5 }, { "MASTER KEY", 30 } };

static int shop_sold_out(int kind, int i) {
    if (kind == 1 && i == 1) return g_master;
    if (kind == 0 && i == 3) return g_spyglass;
    return 0;
}

static void shop_buy(void) {
    const ShopItem *it = g_shop_kind ? &k_shop_lock[g_shop_sel] : &k_shop_com[g_shop_sel];
    if (shop_sold_out(g_shop_kind, g_shop_sel)) { toast("SOLD OUT"); return; }
    if (g_gold < it->price) { toast("NOT ENOUGH GOLD"); mote->audio_play_sfx(&locked_sfx, 0.8f); return; }
    g_gold -= it->price;
    mote->audio_play_sfx(&buy_sfx, 0.9f);
    if (!g_shop_kind) {
        if (g_shop_sel == 0) { g_keys++; toast("BOUGHT A KEY"); }
        else if (g_shop_sel == 1) { g_gems++; toast("BOUGHT A GEM"); }
        else if (g_shop_sel == 2) { g_steps += 8; toast("+8 STEPS"); }
        else { g_spyglass = 1; toast("SPYGLASS: 4-CARD DRAFTS"); }
    } else {
        if (g_shop_sel == 0) { g_keys++; toast("BOUGHT A KEY"); }
        else { g_master = 1; toast("THE MASTER KEY!"); }
    }
    goal_check_held();
}

/* ------------------------------------------------------------------ new day --- */
static void day_start(void) {
    memset(g_grid, 0, sizeof g_grid);
    for (int i = 0; i < GRID_W * GRID_H; i++) g_grid[i].room = 0xFF;
    g_grid[START_GI].room = R_ENTRANCE;
    g_grid[START_GI].doors = DBIT(DIR_N) | DBIT(DIR_E) | DBIT(DIR_W);
    g_grid[START_GI].entered = 1;
    g_grid[ANTE_GI].room = R_ANTE;
    g_grid[ANTE_GI].doors = DBIT(DIR_S) | DBIT(DIR_E) | DBIT(DIR_W);
    g_cur = START_GI;
    g_px = 56; g_py = 60;
    g_face = DIR_N;
    parse_room_props(g_cur);
    g_steps = START_STEPS; g_keys = 1; g_gems = 2; g_gold = 0;
    g_score = 0; g_master = 0; g_compass = 0; g_spyglass = 0;
    g_won = 0; g_rooms_placed = 0;
    g_new_best = 0;
    g_toast_n = 0; g_toast_t = 0;
    g_day_seed = mote_rand();
    /* two distinct goals for the day */
    {
        uint32_t r = hash32(g_day_seed ^ 0x60A15u);
        g_goal[0] = (uint8_t)(r % GO_N);
        g_goal[1] = (uint8_t)((g_goal[0] + 1 + (r >> 8) % (GO_N - 1)) % GO_N);
        g_goal_prog[0] = g_goal_prog[1] = 0;
        g_goal_done[0] = g_goal_done[1] = 0;
    }

    const char *e;
    if ((e = getenv("DRAFT_GIVE"))) {          /* keys:gems:gold:steps */
        int k, gm, go, st;
        if (sscanf(e, "%d:%d:%d:%d", &k, &gm, &go, &st) == 4) {
            g_keys = k; g_gems = gm; g_gold = go; g_steps = st;
        }
    }
    if (getenv("DRAFT_TOOLS")) { g_compass = 1; g_spyglass = 1; }
    g_state = GS_PLAY;
    toast("FIND THE ANTECHAMBER");
}

/* ------------------------------------------------------------------ movement -- */
static void player_tick(float dt) {
    const MoteInput *in = mote->input();
    float sp = 58.0f * dt;
    float dx = 0, dy = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { dx -= sp; g_face = DIR_W; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { dx += sp; g_face = DIR_E; }
    if (mote_pressed(in, MOTE_BTN_UP))    { dy -= sp; g_face = DIR_N; }
    if (mote_pressed(in, MOTE_BTN_DOWN))  { dy += sp; g_face = DIR_S; }
    g_moving = (dx != 0 || dy != 0);
    if (g_moving) g_walk_t += dt; else g_walk_t = 0;

    int bumped_side = -1;
    if (dx != 0) {
        if (!box_solid(g_px + dx, g_py)) g_px += dx;
        else {
            float step = dx > 0 ? 0.5f : -0.5f;
            while (!box_solid(g_px + step, g_py)) g_px += step;
            int s = door_side_of_px(g_px + (dx > 0 ? 7 : -7), g_py);
            if (s >= 0) bumped_side = s;
        }
    }
    if (dy != 0) {
        if (!box_solid(g_px, g_py + dy)) g_py += dy;
        else {
            float step = dy > 0 ? 0.5f : -0.5f;
            while (!box_solid(g_px, g_py + step)) g_py += step;
            int s = door_side_of_px(g_px, g_py + (dy > 0 ? 7 : -7));
            if (s >= 0) bumped_side = s;
        }
    }
    g_bump_cd -= dt;
    if (g_bump_cd <= 0) {
        /* pushing on a chest? (checked before doors: chests sit inside) */
        float fx = g_px + (g_face == DIR_E ? 7 : g_face == DIR_W ? -7 : 0);
        float fy = g_py + (g_face == DIR_S ? 7 : g_face == DIR_N ? -7 : 0);
        int ci = g_moving ? chest_at_px(fx, fy) : -1;
        if (ci >= 0) { g_bump_cd = 0.4f; chest_try(ci); }
        else if (bumped_side >= 0) { g_bump_cd = 0.35f; door_try(bumped_side); }
    }
    pickups_tick();

    if (mote_just_pressed(in, MOTE_BTN_RB)) { g_state = GS_MAP; mote->audio_play_sfx(&tick_sfx, 0.7f); }
    if (mote_just_pressed(in, MOTE_BTN_MENU)) { g_pause_sel = 0; g_state = GS_PAUSE; }

    /* shops: A opens the counter menu anywhere in the room */
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        const RoomDef *d = &k_rooms[g_grid[g_cur].room];
        if (d->flags & (RF_SHOP_COM | RF_SHOP_LOCK)) {
            g_shop_kind = (d->flags & RF_SHOP_LOCK) ? 1 : 0;
            g_shop_sel = 0;
            g_state = GS_SHOP;
            mote->audio_play_sfx(&tick_sfx, 0.8f);
        }
    }
}

/* ------------------------------------------------------------------ drawing --- */
static void add_spr(const MoteImage *img, int x, int y, int fx, int fy, int fw, int fh,
                    int layer, uint8_t flags) {
    MoteSprite s = { .img = img, .x = (int16_t)x, .y = (int16_t)y,
                     .fx = (uint16_t)fx, .fy = (uint16_t)fy,
                     .fw = (uint16_t)fw, .fh = (uint16_t)fh,
                     .layer = (uint8_t)layer, .flags = flags };
    mote->scene2d_add(&s);
}

static void room_draw(void) {
    const Cell *cl = &g_grid[g_cur];
    const RoomDef *rd = &k_rooms[cl->room];
    const MoteAutotile *wall = k_walls[rd->wall];

    /* floor everywhere (16px texture tiles, variant hash-picked per tile so
     * the grain doesn't grid up), thin wall band painted over the rim */
    for (int ty = 0; ty < ROOM_T; ty++)
        for (int tx = 0; tx < ROOM_T; tx++)
            add_spr(&floors_img, tx * TILE, ty * TILE,
                    rd->floor * TILE, ((tx * 7 + ty * 13) & 1) * TILE, TILE, TILE, 0, 0);
    for (int i = 0; i < ROOM_T; i++) {
        add_spr(wall->sheet, i * TILE, 0, 0, 0, TILE, WALL_PX, 1, 0);
        add_spr(wall->sheet, i * TILE, ROOM_PX - WALL_PX, 0, TILE - WALL_PX, TILE, WALL_PX, 1, 0);
        add_spr(wall->sheet, 0, i * TILE, 0, 0, WALL_PX, TILE, 1, 0);
        add_spr(wall->sheet, ROOM_PX - WALL_PX, i * TILE, TILE - WALL_PX, 0, WALL_PX, TILE, 1, 0);
    }

    /* door overlays at the four mid-edges (nothing drawn for sealed walls) */
    static const int8_t k_door_px[4][2] = { { 48, 0 }, { 96, 48 }, { 48, 96 }, { 0, 48 } };
    for (int s = 0; s < 4; s++) {
        int st = door_state(g_cur, s);
        if (st == DS_NONE || st == DS_SEALED) continue;
        int vert = (s == DIR_E || s == DIR_W);
        int cell; uint8_t fl = 0;
        /* h doors hang from the top wall; v cells are CCW-rotated (lintel left
         * = a W-wall door), so the E side mirrors */
        if (st == DS_GOLD) {
            cell = 4;
            if (vert) { fl = MOTE_SPR_ROT90; if (s == DIR_W) fl |= MOTE_SPR_HFLIP; }
        }
        else if (st == DS_OPEN) cell = vert ? 3 : 1;
        else cell = vert ? 2 : 0;
        if (!vert && s == DIR_S) fl |= MOTE_SPR_VFLIP;
        if (vert && s == DIR_E && st != DS_GOLD) fl |= MOTE_SPR_HFLIP;
        add_spr(&doors_img, k_door_px[s][0], k_door_px[s][1], cell * TILE, 0, TILE, TILE, 2, fl);
        if (st == DS_LOCKED)
            add_spr(&items_img, k_door_px[s][0] + 2, k_door_px[s][1] + 2, 7 * 12, 0, 12, 12, 30, 0);
    }

    /* furniture, painter-ordered by base line */
    for (int i = 0; i < g_nprops; i++) {
        const PropDef *d = &k_props[g_props_cur[i].prop];
        int layer = 3 + ((g_props_cur[i].y + d->fh) >> 3);
        add_spr(k_prop_sheets[d->sheet], g_props_cur[i].x, g_props_cur[i].y,
                d->fx, d->fy, d->fw, d->fh, layer, 0);
    }

    /* treasure chests (closed / padlocked / open) */
    for (int i = 0; i < g_nchests; i++) {
        int open = (cl->chests >> i) & 1;
        const PropDef *d = &k_props[open ? P_CHEST_OPEN : P_CHEST];
        int layer = 3 + ((g_chests[i].y + 16) >> 3);
        add_spr(k_prop_sheets[d->sheet], g_chests[i].x, g_chests[i].y,
                d->fx, d->fy, d->fw, d->fh, layer, 0);
        if (!open && g_chests[i].locked)
            add_spr(&items_img, g_chests[i].x + 4, g_chests[i].y + 5, 7 * 12, 0, 12, 12, layer, 0);
    }

    /* pickups */
    uint8_t pos[MAX_LOOT][2];
    int n = loot_positions(g_cur, pos);
    for (int i = 0; i < n; i++)
        if (!(cl->looted & (1 << i)))
            add_spr(&items_img, pos[i][0] - 6, pos[i][1] - 6,
                    k_item_cell[rd->loot[i]] * 12, 0, 12, 12, 3 + ((pos[i][1] + 6) >> 3), 0);

    /* player (16x20 sprite, feet at py+6; side walk is a 4-frame cycle) */
    int cell; uint8_t fl = 0;
    if (g_face == DIR_S) cell = 0 + (g_moving ? ((int)(g_walk_t * 6.0f) & 1) : 0);
    else if (g_face == DIR_N) cell = 2 + (g_moving ? ((int)(g_walk_t * 6.0f) & 1) : 0);
    else {
        cell = 4 + (g_moving ? ((int)(g_walk_t * 8.0f) & 3) : 0);
        if (g_face == DIR_W) fl = MOTE_SPR_HFLIP;    /* sheet cells face right */
    }
    add_spr(&hero_img, (int)g_px - 8, (int)g_py - 14, cell * 16, 0, 16, 20,
            3 + (((int)g_py + 6) >> 3), fl);
}

/* ---------------------------------------------------------------- overlays --- */
static uint16_t rgb(int r, int g, int b) { return MOTE_RGB565(r, g, b); }

static void dim(uint16_t *fb) {
    for (int i = 0; i < 128 * 128; i++) fb[i] = (uint16_t)((fb[i] >> 1) & 0x7BEF);
}

static void hud_draw(uint16_t *fb) {
    mote->draw_rect(fb, 0, 0, 128, ORG_Y, rgb(16, 20, 40), 1, 0, 128);
    mote->draw_rect(fb, 0, ORG_Y - 1, 128, 1, rgb(60, 80, 140), 1, 0, 128);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    char buf[16];
    uint16_t sc = g_steps <= 8 ? (((int)(g_result_t * 4) & 1) ? rgb(255, 70, 60) : rgb(255, 160, 60))
                               : rgb(240, 240, 250);
    snprintf(buf, sizeof buf, "%d", g_steps);
    mote->blit(fb, &items_img, 2, 2, 8 * 12, 0, 12, 12, 0, 0, 128);   /* boot = steps */
    mote->text_font(fb, f, buf, 15, 2, sc);
    int x = 42;
    mote->blit(fb, &items_img, x, 2, 1 * 12, 0, 12, 12, 0, 0, 128);
    snprintf(buf, sizeof buf, "%d", g_keys);
    x = mote->text_font(fb, f, buf, x + 12, 2, rgb(240, 220, 140)) + 6;
    mote->blit(fb, &items_img, x, 2, 2 * 12, 0, 12, 12, 0, 0, 128);
    snprintf(buf, sizeof buf, "%d", g_gems);
    x = mote->text_font(fb, f, buf, x + 12, 2, rgb(140, 240, 220)) + 6;
    mote->blit(fb, &items_img, x, 2, 0 * 12, 0, 12, 12, 0, 0, 128);
    snprintf(buf, sizeof buf, "%d", g_gold);
    x = mote->text_font(fb, f, buf, x + 12, 2, rgb(250, 210, 110)) + 4;
    int ix = 128 - 12;
    if (g_master)   { mote->blit(fb, &items_img, ix, 2, 6 * 12, 0, 12, 12, 0, 0, 128); ix -= 11; }
    if (g_compass)  { mote->blit(fb, &items_img, ix, 2, 10 * 12, 0, 12, 12, 0, 0, 128); ix -= 11; }
    if (g_spyglass) { mote->blit(fb, &items_img, ix, 2, 11 * 12, 0, 12, 12, 0, 0, 128); }
}

static void toast_draw(uint16_t *fb) {
    if (!g_toast_n) return;
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 0, 98, 128, 14, rgb(10, 12, 26), 1, 0, 128);
    mote_ftextc(mote, fb, f, 64, 99, rgb(250, 240, 190), g_toast[0]);
}

/* one estate cell on a blueprint panel */
static void bp_cell(uint16_t *fb, int x, int y, int cs, int gi, int hilite) {
    const Cell *cl = &g_grid[gi];
    mote->draw_rect(fb, x, y, cs - 1, cs - 1, rgb(20, 30, 62), 1, 0, 128);
    if (cl->room != 0xFF) {
        mote->draw_rect(fb, x + 1, y + 1, cs - 3, cs - 3, k_rooms[cl->room].map_col, 1, 0, 128);
        uint16_t pip = rgb(16, 18, 30);
        int m = cs / 2 - 1;
        if (cl->doors & DBIT(DIR_N)) mote->draw_rect(fb, x + m, y, 2, 2, pip, 1, 0, 128);
        if (cl->doors & DBIT(DIR_S)) mote->draw_rect(fb, x + m, y + cs - 3, 2, 2, pip, 1, 0, 128);
        if (cl->doors & DBIT(DIR_W)) mote->draw_rect(fb, x, y + m, 2, 2, pip, 1, 0, 128);
        if (cl->doors & DBIT(DIR_E)) mote->draw_rect(fb, x + cs - 3, y + m, 2, 2, pip, 1, 0, 128);
    }
    if (gi == ANTE_GI && cs >= 8)
        mote->blit(fb, &items_img, x + (cs - 12) / 2, y + (cs - 12) / 2, 4 * 12, 0, 12, 12, 0, 0, 128);
    if (hilite)
        mote->draw_rect(fb, x - 1, y - 1, cs + 1, cs + 1, rgb(255, 255, 255), 0, 0, 128);
}

/* full-screen blueprint paper backdrop */
static void paper(uint16_t *fb) {
    mote->draw_rect(fb, 0, 0, 128, 128, rgb(13, 21, 50), 1, 0, 128);
    for (int v = 8; v < 128; v += 8) {
        uint16_t c = (v & 31) ? rgb(18, 28, 64) : rgb(25, 40, 88);
        mote->draw_rect(fb, v, 0, 1, 128, c, 1, 0, 128);
        mote->draw_rect(fb, 0, v, 128, 1, c, 1, 0, 128);
    }
    mote->draw_rect(fb, 1, 1, 126, 126, rgb(96, 136, 210), 0, 0, 128);
    mote->draw_rect(fb, 3, 3, 122, 122, rgb(44, 66, 124), 0, 0, 128);
}

/* miniature render of a room: real floor texture, wall ring, door gaps, and
 * the room's own furniture blitted at miniature scale */
static void room_icon(uint16_t *fb, int x, int y, int s, uint8_t room, uint8_t mask, int bright) {
    const RoomDef *d = &k_rooms[room];
    /* floor tiled at true texture scale */
    int inner = s - 4;
    for (int oy = 0; oy < inner; oy += TILE)
        for (int ox = 0; ox < inner; ox += TILE)
            mote->blit(fb, &floors_img, x + 2 + ox, y + 2 + oy, d->floor * TILE, 0,
                       inner - ox < TILE ? inner - ox : TILE,
                       inner - oy < TILE ? inner - oy : TILE, 0, 0, 128);
    /* the template's furniture + chests, scaled into the interior */
    float ps = (float)(s - 4) / (float)ROOM_PX;
    for (int ty = 0; ty < ROOM_T; ty++)
        for (int tx = 0; tx < ROOM_T; tx++) {
            char ch = d->tmpl[ty * ROOM_T + tx];
            const PropDef *pd;
            if (ch == 'c' || ch == 'x') pd = &k_props[P_CHEST];
            else {
                int p = prop_of_char(ch);
                if (p < 0) continue;
                pd = &k_props[p];
            }
            int cx = x + 2 + (int)((tx * TILE + pd->fw * 0.5f) * ps);
            int cy = y + 2 + (int)((ty * TILE + pd->fh * 0.5f) * ps);
            float pscale = ps * 1.4f;              /* nudge up so tiny props survive */
            mote->blit_ex(fb, k_prop_sheets[pd->sheet], cx, cy,
                          pd->fx, pd->fy, pd->fw, pd->fh, 0.0f, pscale, MOTE_BLEND_NONE, 0, 128);
        }
    uint16_t wc = bright ? rgb(180, 190, 214) : rgb(110, 120, 148);
    mote->draw_rect(fb, x, y, s, 2, wc, 1, 0, 128);
    mote->draw_rect(fb, x, y + s - 2, s, 2, wc, 1, 0, 128);
    mote->draw_rect(fb, x, y, 2, s, wc, 1, 0, 128);
    mote->draw_rect(fb, x + s - 2, y, 2, s, wc, 1, 0, 128);
    mote->draw_rect(fb, x + 2, y + 2, s - 4, s - 4, rgb(20, 24, 40), 0, 0, 128);
    uint16_t dc = bright ? rgb(255, 235, 150) : rgb(250, 252, 255);
    int m = s / 2 - 3, g = 6;
    if (mask & DBIT(DIR_N)) mote->draw_rect(fb, x + m, y, g, 3, dc, 1, 0, 128);
    if (mask & DBIT(DIR_S)) mote->draw_rect(fb, x + m, y + s - 3, g, 3, dc, 1, 0, 128);
    if (mask & DBIT(DIR_W)) mote->draw_rect(fb, x, y + m, 3, g, dc, 1, 0, 128);
    if (mask & DBIT(DIR_E)) mote->draw_rect(fb, x + s - 3, y + m, 3, g, dc, 1, 0, 128);
}

/* the estate blueprint, with the draft target previewed live (target_gi < 0: none) */
static void estate_map(uint16_t *fb, int ox, int oy, int cs, int target_gi) {
    mote->draw_rect(fb, ox - 3, oy - 3, GRID_W * cs + 5, GRID_H * cs + 5, rgb(10, 17, 42), 1, 0, 128);
    mote->draw_rect(fb, ox - 3, oy - 3, GRID_W * cs + 5, GRID_H * cs + 5, rgb(110, 150, 220), 0, 0, 128);
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++) {
            int gi = r * GRID_W + c;
            if (gi == target_gi) continue;
            bp_cell(fb, ox + c * cs, oy + r * cs, cs, gi, gi == g_cur);
        }
    if (target_gi >= 0) {
        int c = gi_col(target_gi), r = gi_row(target_gi);
        int x = ox + c * cs, y = oy + r * cs;
        const RoomDef *d = &k_rooms[g_cards[g_draft_sel]];
        uint8_t mask = orient_mask(d->shape, g_draft_entry, g_rot[g_draft_sel]);
        int on = ((int)(g_result_t * 3) & 1) == 0;
        mote->draw_rect(fb, x + 1, y + 1, cs - 3, cs - 3,
                        on ? d->map_col : rgb(40, 56, 100), 1, 0, 128);
        uint16_t pip = rgb(255, 255, 255);
        int m = cs / 2 - 1;
        if (mask & DBIT(DIR_N)) mote->draw_rect(fb, x + m, y, 2, 2, pip, 1, 0, 128);
        if (mask & DBIT(DIR_S)) mote->draw_rect(fb, x + m, y + cs - 3, 2, 2, pip, 1, 0, 128);
        if (mask & DBIT(DIR_W)) mote->draw_rect(fb, x, y + m, 2, 2, pip, 1, 0, 128);
        if (mask & DBIT(DIR_E)) mote->draw_rect(fb, x + cs - 3, y + m, 2, 2, pip, 1, 0, 128);
        mote->draw_rect(fb, x - 1, y - 1, cs + 1, cs + 1, rgb(255, 230, 120), 0, 0, 128);
    }
}

static void draft_footer(uint16_t *fb, const MoteFont *f) {
    char buf[8];
    snprintf(buf, sizeof buf, "%d", g_gems);
    mote->blit(fb, &items_img, 6, 115, 2 * 12, 0, 12, 12, 0, 0, 128);
    mote->text_font(fb, f, buf, 19, 115, rgb(140, 240, 220));
    const char *hint = g_compass ? (g_gems > 0 ? "B RD  LB/RB TURN" : "LB/RB TURN")
                                 : (g_gems > 0 ? "B REROLL" : "A PLACE");
    mote->text_font(fb, f, hint, 36, 115, rgb(150, 165, 205));
}

static const uint16_t k_rarity_col[3] = { 0x8410, 0x2E9F, 0xFD00 };
static const char *k_rarity_name[3] = { "COMMON", "UNCOMMON", "RARE" };

/* Layout A "drafting desk": blueprint left, selected card's dossier right,
 * the hand of room shapes along the bottom. */
static void draft_draw_a(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->text_font(fb, f, "DRAFT", 8, 3, rgb(210, 224, 250));
    estate_map(fb, 10, 18, 8, g_draft_gi);

    /* dossier for the selected card */
    const RoomDef *d = &k_rooms[g_cards[g_draft_sel]];
    int afford = card_affordable(g_cards[g_draft_sel]);
    mote->draw_rect(fb, 56, 15, 68, 65, rgb(16, 25, 58), 1, 0, 128);
    mote->draw_rect(fb, 56, 15, 68, 65, rgb(70, 100, 170), 0, 0, 128);
    mote->text_font(fb, f, d->name, 60, 18, afford ? rgb(250, 250, 255) : rgb(120, 120, 132));
    mote->text_font(fb, f, k_rarity_name[d->rarity], 60, 31, k_rarity_col[d->rarity]);
    if (k_blurb[g_cards[g_draft_sel]])
        mote->text_font(fb, f, k_blurb[g_cards[g_draft_sel]], 60, 47,
                        afford ? rgb(165, 190, 235) : rgb(100, 100, 112));
    int cx = 60;
    for (int c = 0; c < d->gems; c++) { mote->blit(fb, &items_img, cx, 64, 2 * 12, 0, 12, 12, 0, 0, 128); cx += 10; }
    if (d->flags & RF_LOCKED) { mote->blit(fb, &items_img, cx, 64, 7 * 12, 0, 12, 12, 0, 0, 128); cx += 12; }
    if (!afford) mote->text_font(fb, f, "!", cx + 2, 63, rgb(255, 110, 90));

    /* the hand: every option's SHAPE always on show */
    int tw = 26, total = g_draft_n * tw - 2;
    int hx = (128 - total) / 2, hy = 90;
    for (int i = 0; i < g_draft_n; i++) {
        const RoomDef *cd = &k_rooms[g_cards[i]];
        uint8_t mask = orient_mask(cd->shape, g_draft_entry, g_rot[i]);
        int sel = i == g_draft_sel;
        int x = hx + i * tw, y = hy - (sel ? 4 : 0);
        if (sel) mote->draw_rect(fb, x - 2, y - 2, 28, 28, rgb(255, 230, 120), 0, 0, 128);
        room_icon(fb, x, y, 24, g_cards[i], mask, sel);
        if (!card_affordable(g_cards[i]))
            mote->blit(fb, &items_img, x + 6, y + 6, 7 * 12, 0, 12, 12, 0, 0, 128);
        mote->draw_rect(fb, x, y + 24, 24, 2, k_rarity_col[cd->rarity], 1, 0, 128);
    }
    draft_footer(fb, f);
}

/* Layout B "ledger": card list left with shape icons, blueprint right. */
static void draft_draw_b(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->text_font(fb, f, "DRAFT", 8, 6, rgb(210, 224, 250));
    estate_map(fb, 86, 24, 8, g_draft_gi);

    int ch = g_draft_n == 4 ? 23 : 29;
    for (int i = 0; i < g_draft_n; i++) {
        const RoomDef *d = &k_rooms[g_cards[i]];
        uint8_t mask = orient_mask(d->shape, g_draft_entry, g_rot[i]);
        int x = 6, y = 20 + i * (ch + 2), w = 74;
        int sel = i == g_draft_sel;
        int afford = card_affordable(g_cards[i]);
        mote->draw_rect(fb, x, y, w, ch, sel ? rgb(24, 36, 78) : rgb(15, 23, 54), 1, 0, 128);
        mote->draw_rect(fb, x, y, w, ch, sel ? rgb(255, 230, 120) : rgb(52, 74, 130), 0, 0, 128);
        room_icon(fb, x + 2, y + (ch - 20) / 2, 20, g_cards[i], mask, sel);
        uint16_t nc = afford ? rgb(250, 250, 255) : rgb(110, 110, 122);
        mote->text_font(fb, f, d->name, x + 24, y + 1, nc);
        if (ch > 24 && k_blurb[g_cards[i]])
            mote->text_font(fb, f, k_blurb[g_cards[i]], x + 24, y + 15,
                            afford ? rgb(165, 190, 235) : rgb(95, 95, 108));
        int cx = x + w - 11;
        int cy2 = y + 1;
        for (int c = 0; c < d->gems; c++) { mote->blit(fb, &items_img, cx, cy2, 2 * 12, 0, 12, 12, 0, 0, 128); cy2 += 8; }
        if (d->flags & RF_LOCKED) mote->blit(fb, &items_img, cx, cy2, 7 * 12, 0, 12, 12, 0, 0, 128);
    }
    draft_footer(fb, f);
}

static uint8_t g_draft_ui;                 /* DRAFT_UI=b -> layout B */

static void draft_draw(uint16_t *fb) {
    if (g_draft_ui) draft_draw_b(fb);
    else draft_draw_a(fb);
}

static int card_orientations(uint8_t id) {
    uint8_t sh = k_rooms[id].shape;
    return (sh == SH_STR || sh == SH_L || sh == SH_R || sh == SH_T) ? 3 : 1;
}

static void draft_tick(void) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_UP) || mote_just_pressed(in, MOTE_BTN_LEFT))
        { g_draft_sel = (g_draft_sel + g_draft_n - 1) % g_draft_n; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN) || mote_just_pressed(in, MOTE_BTN_RIGHT))
        { g_draft_sel = (g_draft_sel + 1) % g_draft_n; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (g_compass) {
        int n = card_orientations(g_cards[g_draft_sel]);
        if (mote_just_pressed(in, MOTE_BTN_RB)) { g_rot[g_draft_sel] = (uint8_t)((g_rot[g_draft_sel] + 1) % n); mote->audio_play_sfx(&tick_sfx, 0.7f); }
        if (mote_just_pressed(in, MOTE_BTN_LB)) { g_rot[g_draft_sel] = (uint8_t)((g_rot[g_draft_sel] + n - 1) % n); mote->audio_play_sfx(&tick_sfx, 0.7f); }
    }
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (card_affordable(g_cards[g_draft_sel])) place_card(g_draft_sel);
        else { toast("CAN'T AFFORD THAT ROOM"); mote->audio_play_sfx(&locked_sfx, 0.8f); }
    }
    if (mote_just_pressed(in, MOTE_BTN_B)) {
        if (g_gems > 0) { g_gems--; deal_cards(); g_draft_sel = 0; mote->audio_play_sfx(&gem_sfx, 0.8f); }
        else { toast("NO GEMS TO REROLL"); mote->audio_play_sfx(&locked_sfx, 0.8f); }
    }
}

static void map_draw(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    char buf[40];
    snprintf(buf, sizeof buf, "DAY %d", g_days + 1);
    mote->text_font(fb, f, buf, 8, 3, rgb(200, 210, 240));
    snprintf(buf, sizeof buf, "%u", (unsigned)g_score);
    mote->text_font(fb, f, buf, 92, 3, rgb(250, 240, 190));
    estate_map(fb, 39, 14, 10, -1);
    /* the day's goals */
    for (int i = 0; i < 2; i++) {
        const GoalDef *gd = &k_goals[g_goal[i]];
        int y = 98 + i * 13;
        if (g_goal_done[i]) {
            snprintf(buf, sizeof buf, "%s  DONE", gd->name);
            mote->text_font(fb, f, buf, 8, y, rgb(250, 220, 110));
        } else {
            snprintf(buf, sizeof buf, "%s %d/%d", gd->name,
                     g_goal_prog[i] > gd->target ? gd->target : g_goal_prog[i], gd->target);
            mote->text_font(fb, f, buf, 8, y, rgb(190, 205, 240));
            snprintf(buf, sizeof buf, "+%d", gd->pts);
            mote->text_font(fb, f, buf, 102, y, rgb(150, 165, 205));
        }
    }
}

static void shop_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    int n = g_shop_kind ? 2 : 4;
    mote->draw_rect(fb, 10, 22, 108, 26 + n * 15, rgb(16, 20, 40), 1, 0, 128);
    mote->draw_rect(fb, 10, 22, 108, 26 + n * 15, rgb(120, 140, 200), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 24, rgb(250, 230, 150), g_shop_kind ? "LOCKSMITH" : "COMMISSARY");
    char buf[32];
    for (int i = 0; i < n; i++) {
        const ShopItem *it = g_shop_kind ? &k_shop_lock[i] : &k_shop_com[i];
        int y = 39 + i * 15;
        if (i == g_shop_sel) mote->draw_rect(fb, 12, y - 1, 104, 14, rgb(40, 50, 90), 1, 0, 128);
        int sold = shop_sold_out(g_shop_kind, i);
        snprintf(buf, sizeof buf, "%s", sold ? "SOLD OUT" : it->name);
        mote->text_font(fb, f, buf, 16, y, g_gold >= it->price && !sold ? rgb(240, 240, 250) : rgb(120, 120, 130));
        if (!sold) {
            snprintf(buf, sizeof buf, "%d", it->price);
            mote->blit(fb, &items_img, 92, y, 0, 0, 12, 12, 0, 0, 128);
            mote->text_font(fb, f, buf, 105, y, rgb(250, 210, 110));
        }
    }
    snprintf(buf, sizeof buf, "GOLD %d", g_gold);
    mote_ftextc(mote, fb, f, 64, 41 + n * 15, rgb(250, 210, 110), buf);
}

static void shop_tick(void) {
    const MoteInput *in = mote->input();
    int n = g_shop_kind ? 2 : 4;
    if (mote_just_pressed(in, MOTE_BTN_UP))   { g_shop_sel = (g_shop_sel + n - 1) % n; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) { g_shop_sel = (g_shop_sel + 1) % n; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_A)) shop_buy();
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU)) g_state = GS_PLAY;
}

static void title_draw(uint16_t *fb) {
    for (int y = 0; y < 128; y++) {
        uint16_t c = y < 64 ? rgb(14, 20, 44 + y / 4) : rgb(14, 20, 60 - (y - 64) / 4);
        mote->draw_rect(fb, 0, y, 128, 1, c, 1, 0, 128);
    }
    for (int v = 0; v < 128; v += 16) {
        for (int y = 0; y < 128; y += 2) mote->draw_pixel(fb, v, y, rgb(30, 44, 90));
        for (int x = 0; x < 128; x += 2) mote->draw_pixel(fb, x, v, rgb(30, 44, 90));
    }
    mote_ftextc(mote, fb, mote->ui_font(MOTE_FONT_LARGE), 64, 18, rgb(250, 250, 255), "DRAFTMOTE");
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, f, 64, 38, rgb(150, 180, 240), "DRAFT THE ESTATE");
    mote->blit(fb, &doors_img, 46, 56, 4 * 16, 0, 16, 16, 0, 0, 128);
    mote->blit(fb, &hero_img, 66, 54, 0, 0, 16, 20, 0, 0, 128);
    char buf[32];
    if (g_hi) {
        snprintf(buf, sizeof buf, "BEST %u", (unsigned)g_hi);
        mote_ftextc(mote, fb, f, 64, 82, rgb(250, 240, 190), buf);
    }
    if (g_wins) {
        snprintf(buf, sizeof buf, "WINS %u / %u DAYS", (unsigned)g_wins, (unsigned)g_days);
        mote_ftextc(mote, fb, f, 64, 96, rgb(160, 170, 200), buf);
    }
    if (((int)(g_result_t * 2) & 1) == 0)
        mote_ftextc(mote, fb, f, 64, 112, rgb(240, 240, 250), "A - ENTER THE ESTATE");
}

static void results_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 8, 14, 112, 100, rgb(14, 16, 32), 1, 0, 128);
    mote->draw_rect(fb, 8, 14, 112, 100, g_won ? rgb(240, 205, 90) : rgb(120, 140, 200), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 18,
                g_won ? rgb(250, 220, 110) : rgb(230, 230, 245),
                g_won ? "THE ANTECHAMBER!" : "OUT OF STEPS");
    char buf[32];
    snprintf(buf, sizeof buf, "DAY %u", (unsigned)g_days);
    mote->text_font(fb, f, buf, 16, 36, rgb(160, 170, 200));
    snprintf(buf, sizeof buf, "ROOMS %d", g_rooms_placed);
    mote->text_font(fb, f, buf, 16, 50, rgb(200, 210, 240));
    snprintf(buf, sizeof buf, "SCORE %u", (unsigned)g_score);
    mote->text_font(fb, f, buf, 16, 64, rgb(250, 240, 190));
    if (g_new_best && ((int)(g_result_t * 3) & 1) == 0)
        mote->text_font(fb, f, "NEW BEST!", 16, 78, rgb(255, 120, 80));
    else if (!g_new_best) {
        snprintf(buf, sizeof buf, "BEST %u", (unsigned)g_hi);
        mote->text_font(fb, f, buf, 16, 78, rgb(150, 160, 190));
    }
    if (g_result_t > 1.0f)
        mote_ftextc(mote, fb, f, 64, 96, rgb(240, 240, 250), "A - NEW DAY");
}

static void pause_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 24, 34, 80, 62, rgb(14, 14, 26), 1, 0, 128);
    mote->draw_rect(fb, 24, 34, 80, 62, rgb(120, 120, 150), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 36, rgb(240, 220, 120), "PAUSED");
    static const char *rows[3] = { "RESUME", "END DAY", "QUIT" };
    for (int r = 0; r < 3; r++) {
        if (r == g_pause_sel)
            mote->draw_rect(fb, 27, 51 + r * 14, 74, 13, rgb(40, 40, 66), 1, 0, 128);
        mote->text_font(fb, f, rows[r], 33, 51 + r * 14,
                        r == g_pause_sel ? rgb(255, 255, 255) : rgb(170, 170, 190));
    }
}

static void pause_tick(void) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_UP))   g_pause_sel = (g_pause_sel + 2) % 3;
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) g_pause_sel = (g_pause_sel + 1) % 3;
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU)) g_state = GS_PLAY;
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (g_pause_sel == 0) g_state = GS_PLAY;
        else if (g_pause_sel == 1) end_day(0);
        else mote->exit_to_launcher();
    }
}

/* ------------------------------------------------------------------- hooks --- */
static void g_init(void) {
    mote->set_fps_limit(30);
    load_progress();
    uint32_t s = hash32((uint32_t)mote->micros());
    const char *e;
    if ((e = getenv("DRAFT_SEED"))) s = (uint32_t)strtoul(e, 0, 10);
    mote_rand_seed(s ? s : 1u);
    if ((e = getenv("DRAFT_LOCKS"))) {
        if (*e == '0') g_no_locks = 1;
        else if (*e == '2') g_all_locks = 1;
    }
    g_force_rooms = getenv("DRAFT_ROOMS");
    if ((e = getenv("DRAFT_UI")) && *e == 'b') g_draft_ui = 1;
    if (getenv("DRAFT_SKIP")) day_start();
    mote->log("draftmote up");
}

static void g_update(float dt) {
    toast_tick(dt);
    g_result_t += dt;
    mote->scene_set_background(rgb(8, 10, 20));

    int st = g_state;                    /* dispatch on frame-start state */
    switch (st) {
    case GS_TITLE: {
        const MoteInput *in = mote->input();
        if (mote_just_pressed(in, MOTE_BTN_A)) day_start();
        break;
    }
    case GS_PLAY:
    case GS_DRAFT:
    case GS_SHOP:
    case GS_MAP:
    case GS_PAUSE:
        if (st == GS_PLAY) player_tick(dt);
        else if (st == GS_DRAFT) draft_tick();
        else if (st == GS_SHOP) shop_tick();
        else if (st == GS_PAUSE) pause_tick();
        else {
            const MoteInput *in = mote->input();
            if (mote_just_pressed(in, MOTE_BTN_RB) || mote_just_pressed(in, MOTE_BTN_B)
                || mote_just_pressed(in, MOTE_BTN_A))
                g_state = GS_PLAY;
        }
        mote->scene2d_begin(-ORG_X, -ORG_Y);
        room_draw();
        break;
    case GS_RESULTS: {
        const MoteInput *in = mote->input();
        if (g_result_t > 1.0f && mote_just_pressed(in, MOTE_BTN_A)) g_state = GS_TITLE;
        mote->scene2d_begin(-ORG_X, -ORG_Y);
        room_draw();
        break;
    }
    }
}

static void g_overlay(uint16_t *fb) {
    switch (g_state) {
    case GS_TITLE:   title_draw(fb); return;
    case GS_DRAFT:   draft_draw(fb); return;
    case GS_MAP:     map_draw(fb); return;
    case GS_RESULTS: results_draw(fb); return;
    case GS_SHOP:    hud_draw(fb); shop_draw(fb); return;
    case GS_PAUSE:   hud_draw(fb); pause_draw(fb); return;
    default:
        /* crisp inner edge on the thin wall band */
        mote->draw_rect(fb, ORG_X + WALL_PX - 1, ORG_Y + WALL_PX - 1,
                        ROOM_PX - 2 * WALL_PX + 2, ROOM_PX - 2 * WALL_PX + 2,
                        rgb(28, 22, 24), 0, 0, 128);
        hud_draw(fb); toast_draw(fb); return;
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 128 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("DraftMote", "austinio7116");
MOTE_GAME_VERSION("0.3.0");
