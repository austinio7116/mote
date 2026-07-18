/* ThumbPrince — a room-drafting roguelike inspired by Blue Prince's estate
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
#include "floor_wood.tiles.h"
#include "floor_stone_tile.tiles.h"
#include "floor_red_carpet.tiles.h"
#include "floor_blue_carpet.tiles.h"
#include "floor_grass.tiles.h"
#include "floor_white_checker.tiles.h"
#include "floor_wood_dark.tiles.h"
#include "floor_grass_leafy.tiles.h"
#include "floor_autumn.tiles.h"
#include "walls_stone.tiles.h"
#include "walls_red.tiles.h"
#include "walls_dark.tiles.h"
#include "walls_hedge.tiles.h"
#include "doors.h"
#include "props_sheet.h"
#include "props_auth.h"
#include "hero.h"
#include "items.h"
#include "title_bg.h"       /* crowned-thumbprint title backdrop (make_title.py) */
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
enum { GS_TITLE, GS_PLAY, GS_DRAFT, GS_SHOP, GS_MAP, GS_PAUSE, GS_RESULTS,
       GS_PUZZLE, GS_SLOTS, GS_CASE };

#define GRID_W 5
#define GRID_H 8
#define ANTE_GI  (0 * GRID_W + 2)          /* top centre */
#define START_GI (7 * GRID_W + 2)          /* bottom centre */
#define ROOM_T 7                            /* tiles per side */
#define TILE 16
#define ROOM_PX (ROOM_T * TILE)             /* 112 */
#define ORG_X 8                             /* interior position on screen */
#define ORG_Y 16
#define START_STEPS 20
#define MAX_LOOT 6
#define MAX_ROOM_PROPS 10

typedef struct {
    uint8_t room;        /* 0xFF = undrafted */
    uint8_t doors;       /* absolute NESW bits */
    uint8_t lock_roll;   /* sides whose lock has been rolled */
    uint8_t lock_on;     /* sides currently key-locked */
    uint8_t card_on;     /* sides gated by a keycard reader */
    uint8_t looted;      /* loot slot bits */
    uint8_t chests;      /* opened-chest bits */
    uint8_t entered;     /* first-entry effect given */
    uint8_t swept;       /* sweep bonus given */
    uint8_t extra;       /* bit0 dig done, bit1 note taken */
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
static uint8_t g_pencil;                    /* redraw offers for a gem (B) */
static uint8_t g_chain;                     /* placement bonus chain (mult = 1+chain) */
static uint8_t g_spade;                     /* dig spots need it */
static uint8_t g_keycard;                   /* opens keycard rooms outright */
static uint8_t g_override;                  /* armed terminals: one keycard door each */
static uint8_t g_power_off;                 /* breaker thrown: readers dark, locks stiffen */
static uint8_t g_cond;                      /* today's condition */
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

/* the room's 8px wall grid: ring + (corridors) carved interior walls */
#define WGRID 14
static uint8_t g_wgrid[WGRID][WGRID];

/* current room's treasure chests (interactive: bump to open) */
typedef struct { uint8_t x, y, locked; } RoomChest;
static RoomChest g_chests[4];
static int g_nchests;

/* dig spot + crumpled clue note in the current room (positions seeded per day) */
static uint8_t g_dig_on, g_dig_x, g_dig_y;
static uint8_t g_note_on, g_note_x, g_note_y;

/* ---- the investigation: in-room puzzles, fed by clues from notes ---------- */
enum { PZ_SAFE, PZ_KEYPAD, PZ_CLOCK, PZ_GLOBE, PZ_PIANO, PZ_CANDLES, PZ_TILES,
       PZ_STATUES, PZ_BOOK, PZ_PORTRAIT, PZ_WINE, PZ_SCALES, PZ_CENSUS, PZ_CHESS,
       PZ_SNOOKER, PZ_IQ, PZ_N };
static uint16_t g_pz_solved, g_pz_clue;     /* per-type bits, reset each day */
static uint8_t  g_pz_sec[PZ_N][4];          /* seeded per-day secrets */
static uint8_t  g_pz_active, g_pz_stage;    /* GS_PUZZLE overlay */
static uint8_t  g_pz_dval[4], g_pz_dsel;
static uint8_t  g_seq_n, g_seq_lit;         /* candle/plate order progress */
static uint8_t  g_statue_face[4];
static int8_t   g_plate_under = -1;
static uint8_t  g_census_room = 0xFF, g_census_kind, g_census_count;
static uint8_t  g_map_page;                 /* 0 estate map, 1 notebook, 2 satchel */
/* the Classroom's pattern lessons */
typedef struct { uint8_t shape, size, fill, count, rot; } IqCell;
static IqCell  g_iq_seq[3], g_iq_opt[4];
static uint8_t g_iq_answer, g_iq_round, g_iq_attempt, g_iq_sel;
/* solved puzzles open a floor hatch; the prizes wait beside it */
#define PRIZE_KEYCARD 100
static uint8_t g_prize_item[PZ_N][3];
static uint8_t g_prize_left[PZ_N];
static uint8_t g_hatch_x[PZ_N], g_hatch_y[PZ_N];
static float   g_hatch_t;
static uint8_t  g_map_cur;                  /* map-page cursor (grid index) */
static uint8_t  g_item_cur;                 /* satchel-page cursor */

/* the Parlor's one-armed bandit */
static uint8_t g_slot_sym[3], g_slot_spinning, g_slot_done, g_slot_pulls;
static float   g_slot_t;
static char    g_slot_msg[24];

/* seal days: the golden door needs three thrown levers instead of 3 keys */
static uint8_t g_blind_draft;               /* darkroom flash: next offer is face-down */
static uint8_t g_seal_day, g_seal_thrown;
static int8_t  g_lever_gi[3];
static uint8_t g_lever_ord[3], g_lever_x[3], g_lever_y[3];

/* the case: one story fragment recovered per win, kept across days */
#define NFRAG 8
static uint8_t g_frags, g_frag_new;

/* secret passages (bookshelf puzzle): door bits that cost no step */
static uint8_t g_secret[GRID_W * GRID_H];
static uint8_t g_free_move;

/* score appraisal */
static uint16_t g_sc_rooms, g_sc_loot, g_sc_bonus, g_sc_goal;
static uint32_t g_sc_win;

static uint8_t g_no_locks;                  /* DRAFT_LOCKS=0 test hook */
static uint8_t g_all_locks;                 /* DRAFT_LOCKS=2 test hook */
static const char *g_force_rooms;           /* DRAFT_ROOMS test hook */
static uint8_t g_force_dig;                 /* DRAFT_DIG=1: digs+notes everywhere */

static int puzzle_of_room(uint8_t room);
static int reveal_clue(uint32_t r);

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
/* decor with no footprint: the rug is walked on, wall pieces sit in the band */
static int prop_no_collide(int p) {
    return p == P_RUG || p == P_PAINTING || p == P_WINDOW ||
           p == P_PAINTING_V || p == P_WINDOW_V || p == P_PLATE ||
           p == P_BANNER || p == P_BANNER_V || p == P_RACK || p == P_RACK_V ||
           p == P_BLACKBOARD || p == P_BLACKBOARD_V;
}
static int prop_wall_mounted(int p) {
    return p == P_PAINTING || p == P_WINDOW || p == P_PAINTING_V || p == P_WINDOW_V ||
           p == P_BANNER || p == P_BANNER_V || p == P_RACK || p == P_RACK_V ||
           p == P_BLACKBOARD || p == P_BLACKBOARD_V;
}
static const MoteAutotile *k_walls[4] = { &walls_stone_at, &walls_red_at, &walls_dark_at, &walls_hedge_at };
static const MoteAutotile *k_floors[9] = {
    &floor_wood_at, &floor_stone_tile_at, &floor_red_carpet_at, &floor_blue_carpet_at,
    &floor_grass_at, &floor_white_checker_at, &floor_wood_dark_at, &floor_grass_leafy_at,
    &floor_autumn_at,
};
static uint8_t k_room_terrain[14 * 14];        /* all-floor 8px map, filled once */
static const uint8_t k_item_cell[10] = { 0, 17, 1, 2, 3, 4, 5, 18, 9, 12 };  /* IT_* -> items cell */

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
    case 'e': return P_TERMINAL;   case 'v': return P_BREAKER;
    case 'R': return P_RUG;        case 'P': return P_PAINTING;
    case 'O': return P_WINDOW;     case 'M': return P_LAMP;
    case 'q': return P_CRATE;      case 'n': return P_BENCH;
    case 'i': return P_CANDLE;
    case 'f': return P_SAFE;       case 'j': return P_KEYPAD;
    case 'k': return P_CLOCK;      case 'G': return P_GLOBE;
    case 'J': return P_PIANO;      case 'N': return P_LECTERN;
    case 'y': return P_WINERACK;   case 'a': return P_SCALES;
    case 'H': return P_CHESSBOARD; case 'Z': return P_SLOTS;
    case 'Y': return P_STATUE;     case 'o': return P_PLATE;
    case 'A': return P_ARMCHAIR;   case 'F': return P_FIREPLACE;
    case 'D': return P_ARMOUR;     case 'I': return P_MIRROR;
    case 'Q': return P_HARP;       case 'V': return P_BANNER;
    case 'z': return P_FERN;       case 'E': return P_PEDESTAL;
    case 'X': return P_BOOKSTACK;  case '1': return P_SNOOKER;
    case '2': return P_RACK;      case '3': return P_BLACKBOARD;
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
    [R_BANQUET] = TAG_FOOD, [R_ROTUNDA] = TAG_GRAND,
    [R_BILLIARDS] = TAG_REST, [R_MUSIC] = TAG_GRAND, [R_CLASSROOM] = TAG_BOOK,
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

/* scored points, itemised for the appraisal */
enum { SC_ROOMS, SC_LOOT, SC_BONUS, SC_GOAL, SC_WIN };
static void score_add(int cat, uint32_t pts);

/* two goals a day, dealt from the day seed */
typedef struct { const char *name; uint8_t target; uint16_t pts; } GoalDef;
enum { GO_GREENS, GO_CHESTS, GO_ROOMS, GO_SWEEPS, GO_COMBOS, GO_RANKS, GO_GOLD, GO_KEYS,
       GO_PUZZLES, GO_N };
static const GoalDef k_goals[GO_N] = {
    [GO_GREENS] = { "GREENS", 3, 150 },
    [GO_CHESTS] = { "CHESTS", 3, 100 },
    [GO_ROOMS]  = { "ROOMS", 10, 100 },
    [GO_SWEEPS] = { "CLEAROUTS", 4, 120 },
    [GO_COMBOS] = { "COMBOS", 2, 120 },
    [GO_RANKS]  = { "RANKS", 2, 150 },
    [GO_GOLD]   = { "GOLD HELD", 25, 100 },
    [GO_KEYS]   = { "KEYS HELD", 4, 100 },
    [GO_PUZZLES] = { "PUZZLES", 2, 150 },
};
static uint8_t g_goal[2], g_goal_prog[2], g_goal_done[2];

static void goal_progress(int kind, int value, int absolute) {
    for (int i = 0; i < 2; i++) {
        if (g_goal[i] != kind || g_goal_done[i]) continue;
        int p = absolute ? value : g_goal_prog[i] + value;
        g_goal_prog[i] = (uint8_t)mote_clampi(p, 0, 255);
        if (g_goal_prog[i] >= k_goals[kind].target) {
            g_goal_done[i] = 1;
            score_add(SC_GOAL, k_goals[kind].pts);
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

/* one estate condition per day, dealt at dawn */
enum { CD_FROSTY, CD_CREAKY, CD_GENEROUS, CD_MARKET, CD_BRIGHT, CD_RESTFUL, CD_MISER, CD_LUCKY, CD_N };
static const struct { const char *name; const char *desc; } k_conds[CD_N] = {
    [CD_FROSTY]   = { "FROSTY",     "GREENS +1 GEM" },
    [CD_CREAKY]   = { "CREAKY",     "MORE LOCKS" },
    [CD_GENEROUS] = { "GENEROUS",   "RICHER CHESTS" },
    [CD_MARKET]   = { "MARKET", "CHEAP SHOPS" },
    [CD_BRIGHT]   = { "BRIGHT",     "RANKS +125" },
    [CD_RESTFUL]  = { "RESTFUL",    "REST +2 STEPS" },
    [CD_MISER]    = { "MISER", "COINS +10 PTS" },
    [CD_LUCKY]    = { "LUCKY",      "MORE DIG SPOTS" },
};

static void score_add(int cat, uint32_t pts) {
    g_score += pts;
    switch (cat) {
    case SC_ROOMS: g_sc_rooms += pts; break;
    case SC_LOOT:  g_sc_loot += pts; break;
    case SC_BONUS: g_sc_bonus += pts; break;
    case SC_GOAL:  g_sc_goal += pts; break;
    default:       g_sc_win += pts; break;
    }
}

/* room blurbs for draft cards */
static const char *k_blurb[R_COUNT] = {
    [R_HALLWAY] = "CORRIDOR",       [R_WPASS] = "CORRIDOR",
    [R_EPASS] = "CORRIDOR",         [R_FOYER] = "3 DOORS",
    [R_GREATHALL] = "4 DOORS +50",  [R_LOUNGE] = "+6 STEPS",
    [R_DRAWING] = "+25, +1 GEM",    [R_DINING] = "+4 STEPS",
    [R_KITCHEN] = "+4, FOOD",       [R_PANTRY] = "+4, FOOD",
    [R_BEDROOM] = "+6 STEPS",       [R_SUITE] = "+12, +1 KEY",
    [R_WASHROOM] = "+4 STEPS",      [R_LIBRARY] = "A PHOTOGRAPH",
    [R_STUDY] = "THE PENCIL",      [R_DRAFTING] = "THE COMPASS",
    [R_LAUNDRY] = "A KEY INSIDE",   [R_STORE] = "COINS+PROOF",
    [R_CELLAR] = "2 KEYS",          [R_LOCKSMITH] = "KEY SHOP",
    [R_COMMISSARY] = "SHOP",        [R_HEARTH] = "+8 STEPS",
    [R_STILLROOM] = "TONICS",       [R_TERRACE] = "GREEN +1GEM",
    [R_GARDEN] = "GREEN +1GEM",     [R_SUNROOM] = "GREEN +2GEM",
    [R_VAULT] = "GOLD HOARD",       [R_GUEST] = "+4, CHEST",
    [R_CHAPEL] = "A PHOTOGRAPH",        [R_ARMORY] = "KEYS, CHEST",
    [R_WINECELLAR] = "TONIC, GOLD", [R_ORCHARD] = "GREEN, FOOD",
    [R_TREASURY] = "2 CHESTS",      [R_NOOK] = "+25 PTS",
    [R_SCULLERY] = "+2 STEPS",      [R_SOLARIUM] = "GREEN +1GEM",
    [R_GAMES] = "+25, PROOF",        [R_BUNK] = "+8 STEPS",
    [R_ATELIER] = "PROOF + GEM",     [R_CRYPT] = "RICH, -4 ST",
    [R_TRICKHALL] = "SEALS SHUT",   [R_SECURITY] = "TERMINAL",
    [R_POWER] = "THE BREAKER",      [R_LABORATORY] = "TONICS, PROOF",
    [R_STRONGROOM] = "GOLD, CHESTS",
    [R_CROSSROADS] = "4 DOORS",     [R_LANDING] = "3 DOORS",
    [R_SERVHALL] = "3 DOORS +2",    [R_CLOISTER] = "GREEN, 3 DR",
    [R_BANQUET] = "3 DR, +4 ST",    [R_ROTUNDA] = "4 DOORS +50",
    [R_APOTHECARY] = "TONIC SHOP",  [R_MUSIC] = "THE PIANO",
    [R_GALLERY] = "PORTRAITS",      [R_PARLOR] = "SLOT MACHINE",
    [R_BILLIARDS] = "+25, PROOF",    [R_CLASSROOM] = "THE IQ TEST",
    [R_DARKROOM] = "CLUE + BLIND",  [R_MAZE] = "GREEN MAZE",
};

/* ------------------------------------------------------------------- save --- */
/* frags rides in what used to be the pad word, so old saves read back cleanly */
typedef struct { uint32_t magic; uint16_t hi, days, wins; uint8_t frags, pad; } SaveBlob;
#define SAVE_MAGIC 0x54465244u  /* 'DRFT' */

static void save_progress(void) {
    SaveBlob b = { SAVE_MAGIC, g_hi, g_days, g_wins, g_frags, 0 };
    mote->save(0, &b, sizeof b);
}
static void load_progress(void) {
    SaveBlob b;
    if (mote->load(0, &b, sizeof b) == sizeof b && b.magic == SAVE_MAGIC) {
        g_hi = b.hi; g_days = b.days; g_wins = b.wins; g_frags = b.frags;
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
enum { DS_NONE, DS_SEALED, DS_CLOSED, DS_OPEN, DS_LOCKED, DS_CARD, DS_GOLD };

static int door_state(int gi, int side) {
    const Cell *cl = &g_grid[gi];
    if (!(cl->doors & DBIT(side))) return DS_NONE;
    int n = neighbor(gi, side);
    if (n < 0) return DS_SEALED;                       /* faces off the estate */
    const Cell *nc = &g_grid[n];
    if (nc->room != 0xFF) {
        if (!(nc->doors & DBIT(side ^ 2))) return DS_SEALED;   /* dead door */
        if (nc->room == R_ANTE && !g_won) return DS_GOLD;
        if ((cl->card_on & DBIT(side)) || (nc->card_on & DBIT(side ^ 2))) return DS_CARD;
        if ((cl->lock_on & DBIT(side)) || (nc->lock_on & DBIT(side ^ 2))) return DS_LOCKED;
        return DS_OPEN;
    }
    if (cl->lock_roll & DBIT(side)) {
        if (cl->card_on & DBIT(side)) return DS_CARD;
        if (cl->lock_on & DBIT(side)) return DS_LOCKED;
    }
    return DS_CLOSED;
}

/* --------------------------------------------------------- room furniture --- */
static void roll_room_finds(int gi);

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
                int px = tx * TILE, py = ty * TILE;
                if (p == P_SNOOKER) {          /* the table owns the room: centred */
                    px = (ROOM_PX - k_props[p].fw) / 2;
                    py = (ROOM_PX - k_props[p].fh) / 2;
                }
                if (prop_wall_mounted(p)) {
                    /* 16x8 band sprites: snap into the nearest wall band,
                     * rotated variant on the side walls — and never over
                     * the door strip at the band's centre */
                    if (ty == 1)      py = 0;
                    else if (ty == ROOM_T - 2) py = ROOM_PX - 8;
                    else if (tx == 1) { px = 0;
                        p = p == P_PAINTING ? P_PAINTING_V
                          : p == P_BANNER ? P_BANNER_V
                          : p == P_RACK ? P_RACK_V
                          : p == P_BLACKBOARD ? P_BLACKBOARD_V : P_WINDOW_V; }
                    else if (tx == ROOM_T - 2) { px = ROOM_PX - 8;
                        p = p == P_PAINTING ? P_PAINTING_V
                          : p == P_BANNER ? P_BANNER_V : P_WINDOW_V; }
                    if (py == 0 || py == ROOM_PX - 8) {
                        if (px > 32 && px < 64) px = 32;
                        else if (px >= 64 && px < 80) px = 64 + 16;
                    } else {
                        if (py > 32 && py < 64) py = 32;
                        else if (py >= 64 && py < 80) py = 64 + 16;
                    }
                }
                g_props_cur[g_nprops].prop = (uint8_t)p;
                g_props_cur[g_nprops].x = (uint8_t)px;
                g_props_cur[g_nprops].y = (uint8_t)py;
                g_nprops++;
            }
        }
    /* the wall grid: ring always; corridor rooms wall the interior and
     * carve one-tile passages toward their actual doors */
    memset(g_wgrid, 0, sizeof g_wgrid);
    for (int i = 0; i < WGRID; i++)
        g_wgrid[0][i] = g_wgrid[WGRID - 1][i] = g_wgrid[i][0] = g_wgrid[i][WGRID - 1] = 1;
    if (k_rooms[g_grid[gi].room].flags & RF_CORRIDOR) {
        for (int cy = 1; cy < WGRID - 1; cy++)
            for (int cx = 1; cx < WGRID - 1; cx++)
                g_wgrid[cy][cx] = 1;
        uint8_t doors = g_grid[gi].doors;
        if (g_grid[gi].room == R_MAZE) {
            /* hedge maze: a ring walk looping a clipped hedge island */
            for (int cy = 2; cy <= 11; cy++)
                for (int cx = 2; cx <= 11; cx++)
                    if (!(cx >= 5 && cx <= 8 && cy >= 5 && cy <= 8))
                        g_wgrid[cy][cx] = 0;
            for (int w = 5; w <= 8; w++) {
                if (doors & DBIT(DIR_N)) g_wgrid[1][w] = 0;
                if (doors & DBIT(DIR_S)) g_wgrid[WGRID - 2][w] = 0;
                if (doors & DBIT(DIR_W)) g_wgrid[w][1] = 0;
                if (doors & DBIT(DIR_E)) g_wgrid[w][WGRID - 2] = 0;
            }
        } else {
            for (int cy = 4; cy <= 9; cy++)                   /* the crossing */
                for (int cx = 4; cx <= 9; cx++)
                    g_wgrid[cy][cx] = 0;
            for (int v = 1; v <= 9; v++)                      /* 3-tile-wide walks */
                for (int w = 4; w <= 9; w++) {
                    if (doors & DBIT(DIR_N)) g_wgrid[v][w] = 0;
                    if (doors & DBIT(DIR_S)) g_wgrid[WGRID - 1 - v][w] = 0;
                    if (doors & DBIT(DIR_W)) g_wgrid[w][v] = 0;
                    if (doors & DBIT(DIR_E)) g_wgrid[w][WGRID - 1 - v] = 0;
                }
        }
    }
    roll_room_finds(gi);
}

/* dig spots + riddle notes: seeded per day+room, positioned on open floor */
static uint32_t hash32(uint32_t x);
static int pt_in_props(float x, float y, float pad);

static int seeded_spot(int gi, uint32_t salt, uint8_t *ox, uint8_t *oy) {
    uint32_t r = hash32(g_day_seed ^ (uint32_t)(gi * 2654435761u) ^ salt);
    for (int t = 0; t < 60; t++) {
        r = hash32(r + t);
        float x = 26 + (float)(r % 60u), y = 26 + (float)((r >> 11) % 60u);
        if (pt_in_props(x, y, 8)) continue;
        *ox = (uint8_t)x; *oy = (uint8_t)y;
        return 1;
    }
    return 0;
}

static void roll_room_finds(int gi) {
    const Cell *cl = &g_grid[gi];
    int dig_pct = g_force_dig ? 100 : (g_cond == CD_LUCKY ? 55 : 30);
    int note_pct = g_force_dig ? 100 : 30;
    int corridor = (k_rooms[cl->room].flags & RF_CORRIDOR) != 0;
    g_dig_on = !corridor && !(cl->extra & 1) && cl->room != R_ANTE &&
               (int)(hash32(g_day_seed ^ (uint32_t)(gi * 40503u) ^ 0xD16D16u) % 100u) < dig_pct &&
               seeded_spot(gi, 0xD16u, &g_dig_x, &g_dig_y);
    g_note_on = !corridor && !(cl->extra & 2) && cl->room != R_ANTE && gi != START_GI &&
                (int)(hash32(g_day_seed ^ (uint32_t)(gi * 69069u) ^ 0x407E3u) % 100u) < note_pct &&
                seeded_spot(gi, 0x407Eu, &g_note_x, &g_note_y);
    /* in-room puzzle scratch state resets on every room entry */
    g_seq_n = 0; g_seq_lit = 0; g_plate_under = -1;
    {
        uint32_t sr = hash32(g_day_seed ^ (uint32_t)(gi * 7919u) ^ 0x57A7u);
        for (int i = 0; i < 4; i++)          /* statues start turned off-solution */
            g_statue_face[i] = (uint8_t)((g_pz_sec[PZ_STATUES][i] + 1 + ((sr >> (i * 4)) & 1)) % 4);
    }
    {
        int hpz = puzzle_of_room(cl->room);      /* reopen a solved room's hatch */
        if (hpz >= 0 && (g_pz_solved & (1 << hpz)) && g_prize_left[hpz])
            seeded_spot(gi, 0xA7C4u + (uint32_t)hpz, &g_hatch_x[hpz], &g_hatch_y[hpz]);
    }
    for (int k = 0; k < 3; k++)              /* a seal lever hides in this cell? */
        if (g_lever_gi[k] == gi) {
            int ok = seeded_spot(gi, 0x1E7E4u + (uint32_t)k, &g_lever_x[k], &g_lever_y[k]);
            if (!ok) { g_lever_x[k] = 56; g_lever_y[k] = 56; }   /* centre fallback */
            if (getenv("DRAFT_PUZ")) {
                char lb[40];
                snprintf(lb, sizeof lb, "lever %d in cell %d at %d,%d ok=%d",
                         k, gi, g_lever_x[k], g_lever_y[k], ok);
                mote->log(lb);
            }
        }
    if (g_force_dig && g_dig_on) {
        char b[32];
        snprintf(b, sizeof b, "dig at %d,%d", g_dig_x, g_dig_y);
        mote->log(b);
    }
    if (g_force_dig && g_note_on) {
        char b[32];
        snprintf(b, sizeof b, "note at %d,%d", g_note_x, g_note_y);
        mote->log(b);
    }
}

/* ------------------------------------------------------------- collisions --- */
#define WALL_PX 8                           /* thin painted wall band */

static int wall_at(float x, float y) {
    int cx = (int)x >> 3, cy = (int)y >> 3;
    if (cx < 0 || cx >= WGRID || cy < 0 || cy >= WGRID) return 1;
    return g_wgrid[cy][cx];
}

static int box_solid(float x, float y) {
    if (wall_at(x - 4, y - 4) || wall_at(x + 4, y - 4) ||
        wall_at(x - 4, y + 4) || wall_at(x + 4, y + 4))
        return 1;
    for (int i = 0; i < g_nprops; i++) {
        if (prop_no_collide(g_props_cur[i].prop)) continue;
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
    if (wall_at(x, y) || wall_at(x - pad, y) || wall_at(x + pad, y) ||
        wall_at(x, y - pad) || wall_at(x, y + pad))
        return 1;
    for (int i = 0; i < g_nprops; i++) {
        if (prop_no_collide(g_props_cur[i].prop)) continue;
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
    int steps = d->steps;
    if (g_cond == CD_RESTFUL && steps > 0) steps += 2;
    int gems = d->gemsg + ((g_cond == CD_FROSTY && (d->flags & RF_GREEN)) ? 1 : 0);
    if (steps) {
        g_steps += steps;
        mote->audio_play_sfx(steps > 0 ? &food_sfx : &locked_sfx, steps > 0 ? 0.7f : 0.9f);
        toastf("%+d STEPS", steps);
    }
    if (d->keys)  { g_keys += d->keys; mote->audio_play_sfx(&key_sfx, 0.9f); toastf("+%d KEYS", d->keys); }
    if (gems)     { g_gems += gems; mote->audio_play_sfx(&gem_sfx, 0.9f); toastf("+%d GEMS", gems); }
    if (d->pts)   { score_add(SC_ROOMS, d->pts); mote->audio_play_sfx(&star_sfx, 0.9f); toastf("+%d PTS", d->pts); }
    if ((d->flags & RF_COMPASS) && !g_compass) {
        g_compass = 1;
        mote->audio_play_sfx(&star_sfx, 1.0f);
        toast("THE COMPASS! LB/RB TURNS DRAFTS");
    }
    if ((d->flags & RF_PENCIL) && !g_pencil) {
        g_pencil = 1;
        mote->audio_play_sfx(&star_sfx, 1.0f);
        toast("THE PENCIL! B REDRAWS OFFERS");
    }
    if ((d->flags & RF_SPADE) && !g_spade) {
        g_spade = 1;
        mote->audio_play_sfx(&star_sfx, 1.0f);
        toast("A SPADE! DIG THE SPARKLES");
    }
    if (cl->room == R_DARKROOM) {
        /* groping in the dark turns up a clue - but your eyes pay for it */
        if (!reveal_clue(hash32(g_day_seed ^ 0xDA2C00u)))
            { score_add(SC_LOOT, 25); toast("SOMETHING IN THE DARK +25"); }
        g_blind_draft = 1;
        toast("DAZZLED: NEXT DRAFT BLIND");
        mote->audio_play_sfx(&locked_sfx, 0.8f);
    }
}

static void end_day(int won) {
    g_won = won;
    g_result_t = 0;
    if (won) {
        uint32_t bonus = 500 + (uint32_t)g_steps * 10 + (uint32_t)g_keys * 25
                       + (uint32_t)g_gems * 15 + (uint32_t)g_gold * 2;
        score_add(SC_WIN, bonus);
        g_wins++;
        for (int i = 0; i < NFRAG; i++)        /* one case fragment per win */
            if (!(g_frags & (1 << i))) {
                g_frags |= (uint8_t)(1 << i);
                g_frag_new = (uint8_t)(i + 1);
                break;
            }
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
    if (g_free_move) g_free_move = 0;          /* secret passages cost nothing */
    else g_steps--;
    parse_room_props(gi);
    mote->audio_play_sfx(&door_sfx, 0.8f);
    switch (entry_side) {
    case DIR_N: g_px = 56; g_py = 24; break;
    case DIR_S: g_px = 56; g_py = 88; break;
    case DIR_E: g_px = 88; g_py = 56; break;
    default:    g_px = 24; g_py = 56; break;
    }
    if (g_grid[gi].room == R_ANTE) { end_day(1); return; }
    if ((k_rooms[g_grid[gi].room].flags & RF_SEAL) && !g_grid[gi].entered) {
        g_grid[gi].doors &= (uint8_t)~DBIT(entry_side);
        toast("THE DOOR SEALS BEHIND YOU!");
        mote->audio_play_sfx(&locked_sfx, 1.0f);
    }
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
    if (g_seal_day)                            /* seal levers ride set placement ordinals */
        for (int k = 0; k < 3; k++)
            if (g_lever_gi[k] < 0 && g_rooms_placed == g_lever_ord[k])
                g_lever_gi[k] = (int8_t)g_draft_gi;
    score_add(SC_ROOMS, 10u * (d->rarity + 1));
    mote->audio_play_sfx(&draft_sfx, 0.9f);
    goal_progress(GO_ROOMS, 1, 0);
    if (d->flags & RF_GREEN) goal_progress(GO_GREENS, 1, 0);
    /* placement bonuses accumulate, then the chain multiplier pays them out */
    uint32_t bonus = 0;
    int hits = 0;
    int mult = 1 + g_chain;
    /* green adjacency bonus */
    if (d->flags & RF_GREEN) {
        int n = 0;
        for (int s = 0; s < 4; s++) {
            int ni = neighbor(g_draft_gi, s);
            if (ni >= 0 && g_grid[ni].room != 0xFF && (k_rooms[g_grid[ni].room].flags & RF_GREEN)) n++;
        }
        if (n) { bonus += 25u * n; hits++; toastf("GARDEN BONUS +%d", 25 * n); mote->audio_play_sfx(&star_sfx, 0.9f); }
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
                    bonus += k_combos[k].pts;
                    hits++;
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
            bonus += 50;
            hits++;
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
        int rank_pts = g_cond == CD_BRIGHT ? 125 : 100;
        bonus += rank_pts;
        hits++;
        toastf("RANK COMPLETE +%d", rank_pts);
        mote->audio_play_sfx(&row_sfx, 1.0f);
        goal_progress(GO_RANKS, 1, 0);
    }
    /* chain: bonus placements build the multiplier, dry ones break it */
    if (hits) {
        score_add(SC_BONUS, bonus * (uint32_t)mult);
        if (mult > 1) toastf("CHAIN PAYS x%d!", mult);
        if (g_chain < 3) {
            g_chain++;
            toastf("CHAIN x%d", 1 + g_chain);
        }
    } else if (g_chain) {
        g_chain = 0;
        toast("CHAIN BROKEN");
    }
    g_blind_draft = 0;
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
        else if (g_seal_day) {
            if (g_seal_thrown == 7) { mote->audio_play_sfx(&unlock_sfx, 1.0f); enter_room(n, side ^ 2); }
            else {
                int c = (g_seal_thrown & 1) + ((g_seal_thrown >> 1) & 1) + ((g_seal_thrown >> 2) & 1);
                toastf("SEALED - LEVERS THROWN %d/3", c);
                mote->audio_play_sfx(&locked_sfx, 0.9f);
            }
        }
        else if (g_keys >= 3) { g_keys -= 3; mote->audio_play_sfx(&unlock_sfx, 1.0f); enter_room(n, side ^ 2); }
        else { toastf("TRIPLE LOCKED - NEED 3 KEYS (%d)", g_keys); mote->audio_play_sfx(&locked_sfx, 0.9f); }
        return;
    }

    if (st == DS_CARD) {
        if (g_keycard) toast("THE KEYCARD READS GREEN");
        else if (g_override > 0) { g_override--; toast("OVERRIDE: READER RELEASED"); }
        else if (g_power_off) toast("READER DARK - IT SWINGS OPEN");
        else {
            toast("A KEYCARD READER");
            mote->audio_play_sfx(&locked_sfx, 0.9f);
            return;
        }
        mote->audio_play_sfx(&unlock_sfx, 1.0f);
        cl->card_on &= (uint8_t)~DBIT(side);
        if (n >= 0) g_grid[n].card_on &= (uint8_t)~DBIT(side ^ 2);
        if (g_grid[n].room != 0xFF) { enter_room(n, side ^ 2); return; }
        st = DS_CLOSED;
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

    if (st == DS_OPEN) {
        if ((g_secret[g_cur] >> side) & 1) g_free_move = 1;
        enter_room(n, side ^ 2);
        return;
    }

    /* DS_CLOSED: an undrafted cell — roll the lock, then draft */
    if (!(cl->lock_roll & DBIT(side))) {
        cl->lock_roll |= DBIT(side);
        int p = g_no_locks ? 0 : g_all_locks ? 100 : 5 + 7 * (7 - gi_row(n))
              + (g_cond == CD_CREAKY ? 12 : 0) + (g_power_off ? 8 : 0);
        if ((int)(mote_rand() % 100u) < p) {
            /* near the top some locks are keycard readers instead */
            int card_pct = gi_row(n) <= 3 ? 30 : gi_row(n) <= 5 ? 12 : 0;
            if ((int)(mote_rand() % 100u) < card_pct) {
                cl->card_on |= DBIT(side);
                toast("A KEYCARD READER");
                mote->audio_play_sfx(&locked_sfx, 0.9f);
                if (!g_keycard && !g_override && !g_power_off) return;
                door_try(side);               /* re-enter through the reader path */
                return;
            }
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
    score_add(SC_LOOT, 10);
    /* seeded contents; padlocked chests hold more */
    uint32_t r = hash32(g_day_seed ^ (uint32_t)(g_cur * 2654435761u) ^ (0xC0FFEEu + (uint32_t)i));
    int gold = g_chests[i].locked ? 4 + (int)(r % 4u) : 2 + (int)(r % 3u);
    if (g_cond == CD_GENEROUS) gold += 2;
    g_gold += gold;
    int roll = (int)((r >> 8) % 100u);
    char buf[40];
    const char *extra = "";
    if (g_chests[i].locked) {
        if (roll < 25)      { g_gems++; extra = " +GEM"; }
        else if (roll < 55) { g_keys++; extra = " +KEY"; }
        else if (roll < 72) { score_add(SC_LOOT, 75); extra = " +PHOTO 75"; mote->audio_play_sfx(&star_sfx, 1.0f); }
        else if (roll < 82 && !g_keycard) { g_keycard = 1; extra = " +KEYCARD!"; mote->audio_play_sfx(&star_sfx, 1.0f); }
        else                { g_gold += 3; gold += 3; }
    } else {
        if (roll < 18)      { g_gems++; extra = " +GEM"; }
        else if (roll < 32) { g_keys++; extra = " +KEY"; }
        else if (roll < 48) { score_add(SC_LOOT, 25); extra = " +EVIDENCE 25"; mote->audio_play_sfx(&star_sfx, 0.9f); }
    }
    mote->audio_play_sfx(&coin_sfx, 0.9f);
    snprintf(buf, sizeof buf, "CHEST: %d GOLD%s", gold, extra);
    toast(buf);
    goal_progress(GO_CHESTS, 1, 0);
    goal_check_held();
}

/* dig up a sparkling spot */
static void dig_here(void) {
    Cell *cl = &g_grid[g_cur];
    cl->extra |= 1;
    g_dig_on = 0;
    score_add(SC_LOOT, 5);
    uint32_t r = hash32(g_day_seed ^ (uint32_t)(g_cur * 48271u) ^ 0xD1DDu);
    int roll = (int)(r % 100u);
    char buf[40];
    if (roll < 40) {
        int gold = 2 + (int)((r >> 8) % 3u);
        g_gold += gold;
        snprintf(buf, sizeof buf, "DUG UP %d GOLD", gold);
        mote->audio_play_sfx(&coin_sfx, 0.9f);
    } else if (roll < 60) {
        g_gems++; snprintf(buf, sizeof buf, "DUG UP A GEM");
        mote->audio_play_sfx(&gem_sfx, 0.9f);
    } else if (roll < 75) {
        g_keys++; snprintf(buf, sizeof buf, "DUG UP A KEY");
        mote->audio_play_sfx(&key_sfx, 0.9f);
    } else if (roll < 90) {
        score_add(SC_LOOT, 25); snprintf(buf, sizeof buf, "DUG UP EVIDENCE +25");
        mote->audio_play_sfx(&star_sfx, 0.9f);
    } else {
        g_steps += 4; snprintf(buf, sizeof buf, "DUG UP A SNACK +4");
        mote->audio_play_sfx(&food_sfx, 0.9f);
    }
    toast(buf);
    goal_check_held();
}

/* -------------------------------------------------------------- puzzles ----- */
/* each puzzle lives in one host room; a crumpled note reveals one puzzle's
 * secret into the notebook; solving without the clue costs a step per guess */
static const char *k_dirname[4] = { "N", "E", "S", "W" };
static const uint8_t k_pz_room[PZ_N] = {
    [PZ_SAFE] = R_STUDY,      [PZ_KEYPAD] = R_LABORATORY, [PZ_CLOCK] = R_FOYER,
    [PZ_GLOBE] = R_DRAWING,   [PZ_PIANO] = R_MUSIC,       [PZ_CANDLES] = R_CHAPEL,
    [PZ_TILES] = R_GREATHALL, [PZ_STATUES] = R_ROTUNDA,   [PZ_BOOK] = R_LIBRARY,
    [PZ_PORTRAIT] = R_GALLERY, [PZ_WINE] = R_WINECELLAR,  [PZ_SCALES] = R_PANTRY,
    [PZ_CENSUS] = R_NOOK,     [PZ_CHESS] = R_GAMES,   [PZ_SNOOKER] = R_BILLIARDS,
    [PZ_IQ] = R_CLASSROOM,
};
static const char *k_pz_name[PZ_N] = {
    "THE SAFE", "THE KEYPAD", "THE CLOCK", "THE GLOBE", "THE PIANO",
    "THE CANDLES", "THE PLATES", "THE STATUES", "THE BOOKSHELF", "THE PORTRAITS",
    "THE WINE RACK", "THE SCALES", "THE CENSUS", "THE CHESSBOARD", "THE BALL RACK",
    "THE TEST",
};
static const char *k_head8[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
static const char *k_books[4] = { "RED", "BLUE", "GREEN", "GOLD" };
static const char *k_faces[4] = { "THE LORD", "THE LADY", "THE HEIR", "THE COUSIN" };
static const char *k_wines[4] = { "'61", "'74", "'88", "'99" };
static const char *k_weighs[3] = { "A KEY", "A GEM", "A COIN" };
static const char *k_pieces[4] = { "KNIGHT", "ROOK", "BISHOP", "QUEEN" };
static const char *k_census_nm[5] = { "WINDOWS", "PAINTINGS", "PLANTS", "CANDLES", "CRATES" };
static const char  k_census_ch[5] = { 'O', 'P', 'p', 'i', 'q' };

static int puzzle_of_room(uint8_t room) {
    for (int i = 0; i < PZ_N; i++)
        if (k_pz_room[i] == room) return i;
    return -1;
}

static void perm4(uint8_t *out, uint32_t r) {
    out[0] = 0; out[1] = 1; out[2] = 2; out[3] = 3;
    for (int i = 3; i > 0; i--) {
        r = hash32(r + (uint32_t)i);
        int j = (int)(r % (uint32_t)(i + 1));
        uint8_t t = out[i]; out[i] = out[j]; out[j] = t;
    }
}

static void puzzles_deal(void) {
    g_pz_solved = 0; g_pz_clue = 0;
    g_census_room = 0xFF;
    uint32_t r = hash32(g_day_seed ^ 0x9E3779B9u);
    for (int i = 0; i < PZ_N; i++)
        for (int k = 0; k < 4; k++) {
            r = hash32(r + 17u);
            g_pz_sec[i][k] = (uint8_t)(r % 251u);
        }
    for (int k = 0; k < 3; k++) g_pz_sec[PZ_SAFE][k] %= 10;
    for (int k = 0; k < 4; k++) g_pz_sec[PZ_KEYPAD][k] %= 10;
    g_pz_sec[PZ_CLOCK][0] = (uint8_t)(1 + g_pz_sec[PZ_CLOCK][0] % 12);
    g_pz_sec[PZ_GLOBE][0] %= 8;
    for (int k = 0; k < 4; k++) g_pz_sec[PZ_PIANO][k] %= 7;   /* C D E F G A B */
    perm4(g_pz_sec[PZ_CANDLES], hash32(g_day_seed ^ 0xCA9D1Eu));
    perm4(g_pz_sec[PZ_TILES], hash32(g_day_seed ^ 0x71E5u));
    for (int k = 0; k < 4; k++) g_pz_sec[PZ_STATUES][k] %= 4;
    g_pz_sec[PZ_BOOK][0] %= 4;
    g_pz_sec[PZ_PORTRAIT][0] %= 4;
    g_pz_sec[PZ_WINE][0] %= 4;
    g_pz_sec[PZ_SCALES][0] %= 3;
    g_pz_sec[PZ_CHESS][0] %= 4;                                /* piece */
    g_pz_sec[PZ_CHESS][1] %= 8; g_pz_sec[PZ_CHESS][2] %= 8;    /* file A-H, rank 1-8 */
    g_pz_sec[PZ_SNOOKER][0] %= 7;                              /* ball, ball, op */
    g_pz_sec[PZ_SNOOKER][1] %= 3;
    g_pz_sec[PZ_SNOOKER][2] %= 7;
}

/* one Classroom lesson, 11+ style: a 4-term sequence where SEVERAL
 * attributes progress at once - quantity (count or size), form (shape
 * cycle or triangle spin), fill parity. Lesson 1 runs two rules, lessons
 * 2-3 run all three. Distractors are near-misses: right in every
 * attribute but one. */
static void iq_gen(void) {
    uint32_t r = hash32(g_day_seed ^ (uint32_t)(g_iq_attempt * 977u + g_iq_round * 131u) ^ 0x1C0DEu);
    int nrules = g_iq_round == 0 ? 2 : 3;
    uint8_t use[3] = { 1, 1, 1 };                /* quantity, form, fill */
    if (nrules == 2) use[r % 3u] = 0;
    int q_is_count = (int)((r >> 2) & 1u);
    int f_is_rot   = (int)((r >> 3) & 1u);
    int dir_q = (int)((r >> 4) & 1u), dir_f = (int)((r >> 5) & 1u);
    int s0 = (int)((r >> 6) % 4u), f0 = (int)((r >> 8) & 1u);
    int r0 = (int)((r >> 9) % 4u), sz0 = 1 + (int)((r >> 11) & 1u);
    IqCell t[4];
    for (int i = 0; i < 4; i++) {
        IqCell *c = &t[i];
        c->count = 1; c->size = (uint8_t)sz0; c->fill = (uint8_t)f0; c->rot = 0;
        c->shape = (uint8_t)((use[1] && f_is_rot) ? 2 : s0);
        if (use[0]) {
            if (q_is_count) { c->count = (uint8_t)(dir_q ? 1 + i : 4 - i); c->size = 0; }
            else            c->size = (uint8_t)(dir_q ? i : 3 - i);
        }
        if (use[1]) {
            if (f_is_rot) c->rot = (uint8_t)((r0 + (dir_f ? i : 4 - i)) % 4);
            else          c->shape = (uint8_t)((s0 + (dir_f ? i : 4 - i)) % 4);
        }
        if (use[2]) c->fill = (uint8_t)((f0 + i) & 1);
    }
    memcpy(g_iq_seq, t, sizeof g_iq_seq);
    IqCell d[3];
    int nd = 0;
    if (use[0]) {
        d[nd] = t[3];
        if (q_is_count) { d[nd].count = t[2].count; d[nd].size = 0; }
        else d[nd].size = t[2].size;
        nd++;
    }
    if (use[1]) {
        d[nd] = t[3];
        if (f_is_rot) d[nd].rot = t[2].rot;
        else d[nd].shape = t[2].shape;
        nd++;
    }
    if (use[2]) { d[nd] = t[3]; d[nd].fill = t[2].fill; nd++; }
    if (nd < 3) {                                /* one axis idle: perturb it */
        d[nd] = t[3];
        if (!use[0])      d[nd].size = (uint8_t)((t[3].size + 2) % 4);
        else if (!use[1]) d[nd].shape = (uint8_t)((t[3].shape + 1) % 4);
        else              d[nd].fill = (uint8_t)!t[3].fill;
        nd++;
    }
    g_iq_answer = (uint8_t)((r >> 13) % 4u);
    int di = 0;
    for (int i = 0; i < 4; i++)
        g_iq_opt[i] = i == (int)g_iq_answer ? t[3] : d[di++];
    g_iq_sel = 0;
}

/* the rack's sum: balls score their snooker worth, red 1 .. black 7 */
static int snooker_answer(void) {
    int a = g_pz_sec[PZ_SNOOKER][0] + 1, b = g_pz_sec[PZ_SNOOKER][2] + 1;
    switch (g_pz_sec[PZ_SNOOKER][1]) {
    case 0:  return a + b;
    case 1:  return a > b ? a - b : b - a;
    default: return a * b;
    }
}

static void pz_clue_text(int i, char *b, int cap) {
    const uint8_t *s = g_pz_sec[i];
    switch (i) {
    case PZ_SAFE:    snprintf(b, cap, "SAFE: %d-%d-%d", s[0], s[1], s[2]); break;
    case PZ_KEYPAD:  snprintf(b, cap, "KEYPAD: %d %d %d %d", s[0], s[1], s[2], s[3]); break;
    case PZ_CLOCK:   snprintf(b, cap, "CLOCK: STRIKE %d", s[0]); break;
    case PZ_GLOBE:   snprintf(b, cap, "GLOBE: SPIN TO %s", k_head8[s[0]]); break;
    case PZ_PIANO:   snprintf(b, cap, "PIANO: %c %c %c %c",
                              "CDEFGAB"[s[0]], "CDEFGAB"[s[1]],
                              "CDEFGAB"[s[2]], "CDEFGAB"[s[3]]); break;
    case PZ_CANDLES: snprintf(b, cap, "CANDLES: %d %d %d %d",
                              s[0] + 1, s[1] + 1, s[2] + 1, s[3] + 1); break;
    case PZ_TILES:   snprintf(b, cap, "PLATES: %s %s %s %s", k_dirname[s[0]],
                              k_dirname[s[1]], k_dirname[s[2]], k_dirname[s[3]]); break;
    case PZ_STATUES: snprintf(b, cap, "STATUES: %s %s %s %s", k_dirname[s[0]],
                              k_dirname[s[1]], k_dirname[s[2]], k_dirname[s[3]]); break;
    case PZ_BOOK:    snprintf(b, cap, "PULL THE %s BOOK", k_books[s[0]]); break;
    case PZ_PORTRAIT: snprintf(b, cap, "ACCUSE %s", k_faces[s[0]]); break;
    case PZ_WINE:    snprintf(b, cap, "DRAW THE %s", k_wines[s[0]]); break;
    case PZ_SCALES:  snprintf(b, cap, "WEIGH %s", k_weighs[s[0]]); break;
    case PZ_CHESS:   snprintf(b, cap, "%s TO %c%d", k_pieces[s[0]], 'A' + s[1], s[2] + 1); break;
    case PZ_SNOOKER: snprintf(b, cap, "RED IS 1 BLACK IS 7"); break;
    default:         snprintf(b, cap, "THE ESTATE IS THE CLUE"); break;
    }
}

/* count a template char in a room's interior */
static int tmpl_count(uint8_t room, char ch) {
    int n = 0;
    for (const char *t = k_rooms[room].tmpl; *t; t++)
        if (*t == ch) n++;
    return n;
}

/* the census ledger asks for a head-count in a room you've already drafted */
static int census_pick(void) {
    if (g_census_room != 0xFF) return 1;
    uint32_t r = hash32(g_day_seed ^ 0xCE9505u);
    int seen = 0, kind = 0, count = 0;
    uint8_t room = 0xFF;
    for (int gi = 0; gi < GRID_W * GRID_H; gi++) {
        if (g_grid[gi].room == 0xFF || gi == g_cur || gi == START_GI || gi == ANTE_GI)
            continue;
        for (int k = 0; k < 5; k++) {
            int c = tmpl_count(g_grid[gi].room, k_census_ch[k]);
            if (!c) continue;
            seen++;
            r = hash32(r + (uint32_t)(gi * 5 + k));
            if ((int)(r % (uint32_t)seen) == 0) { room = g_grid[gi].room; kind = k; count = c; }
        }
    }
    if (room == 0xFF) return 0;
    g_census_room = room;
    g_census_kind = (uint8_t)kind;
    g_census_count = (uint8_t)count;
    return 1;
}

/* dial layout of the overlay puzzles; the value's display text goes in b */
static void pz_dials(int pz, int *n, uint8_t *dmax) {
    switch (pz) {
    case PZ_SAFE:    *n = 3; dmax[0] = dmax[1] = dmax[2] = 10; break;
    case PZ_KEYPAD:  *n = 4; dmax[0] = dmax[1] = dmax[2] = dmax[3] = 10; break;
    case PZ_CLOCK:   *n = 1; dmax[0] = 12; break;
    case PZ_GLOBE:   *n = 1; dmax[0] = 8; break;
    case PZ_CENSUS:  *n = 1; dmax[0] = 13; break;
    case PZ_BOOK: case PZ_PORTRAIT: case PZ_WINE:
                     *n = 1; dmax[0] = 4; break;
    case PZ_SCALES:  *n = 1; dmax[0] = 3; break;
    case PZ_CHESS:   *n = 3; dmax[0] = 4; dmax[1] = dmax[2] = 8; break;
    case PZ_SNOOKER: *n = 2; dmax[0] = 5; dmax[1] = 10; break;
    default:         *n = 0; break;
    }
}
static void pz_dial_text(int pz, int dial, int val, char *b, int cap) {
    switch (pz) {
    case PZ_CLOCK:   snprintf(b, cap, "%d", val + 1); break;
    case PZ_GLOBE:   snprintf(b, cap, "%s", k_head8[val]); break;
    case PZ_BOOK:    snprintf(b, cap, "%s", k_books[val]); break;
    case PZ_PORTRAIT: snprintf(b, cap, "%s", k_faces[val] + 4); break;   /* drop "THE " */
    case PZ_WINE:    snprintf(b, cap, "%s", k_wines[val]); break;
    case PZ_SCALES:  snprintf(b, cap, "%s", k_weighs[val] + 2); break;   /* drop "A " */
    case PZ_CHESS:   if (dial == 0) snprintf(b, cap, "%s", k_pieces[val]);
                     else if (dial == 1) snprintf(b, cap, "%c", 'A' + val);
                     else snprintf(b, cap, "%d", val + 1);
                     break;
    default:         snprintf(b, cap, "%d", val); break;
    }
}

/* does the dial state match the day's secret? */
static int pz_dials_right(int pz) {
    const uint8_t *s = g_pz_sec[pz];
    switch (pz) {
    case PZ_SAFE:   return g_pz_dval[0] == s[0] && g_pz_dval[1] == s[1] && g_pz_dval[2] == s[2];
    case PZ_KEYPAD: return !memcmp(g_pz_dval, s, 4);
    case PZ_CLOCK:  return g_pz_dval[0] + 1 == s[0];
    case PZ_CENSUS: return g_pz_dval[0] == g_census_count;
    case PZ_CHESS:  return g_pz_dval[0] == s[0] && g_pz_dval[1] == s[1] && g_pz_dval[2] == s[2];
    case PZ_SNOOKER: return g_pz_dval[0] * 10 + g_pz_dval[1] == snooker_answer();
    default:        return g_pz_dval[0] == s[0];
    }
}

static void book_passage(void);

/* what waits under the hatch, per puzzle */
static void pz_prizes(int pz, uint8_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    switch (pz) {
    case PZ_SAFE:    out[0] = IT_POUCH; out[1] = IT_POUCH; out[2] = IT_GEM; break;
    case PZ_KEYPAD:  if (!g_keycard) out[0] = PRIZE_KEYCARD;
                     else { out[0] = IT_POUCH; out[1] = IT_POUCH; out[2] = IT_COIN; } break;
    case PZ_CLOCK:   out[0] = IT_POUCH; out[1] = IT_COIN; break;
    case PZ_GLOBE:   out[0] = IT_KEY; break;
    case PZ_PIANO:   out[0] = IT_GEM; break;
    case PZ_CANDLES: out[0] = IT_KEY; break;
    case PZ_TILES:   out[0] = IT_POUCH; out[1] = IT_COIN; out[2] = IT_COIN; break;
    case PZ_STATUES: out[0] = IT_GEM; break;
    case PZ_PORTRAIT: out[0] = IT_POUCH; out[1] = IT_COIN; out[2] = IT_COIN; break;
    case PZ_WINE:    out[0] = IT_POTION; break;
    case PZ_SCALES:  if (g_pz_sec[PZ_SCALES][0] == 0) out[0] = IT_KEY;
                     else if (g_pz_sec[PZ_SCALES][0] == 1) out[0] = IT_GEM;
                     else { out[0] = IT_POUCH; out[1] = IT_POUCH; } break;
    case PZ_CENSUS:  out[0] = IT_POUCH; break;
    case PZ_CHESS:   out[0] = IT_GEM; break;
    case PZ_SNOOKER: out[0] = IT_POUCH; out[1] = IT_COIN; out[2] = IT_COIN; break;
    case PZ_IQ:      out[0] = IT_GEM; out[1] = IT_POUCH; break;
    default: break;
    }
}

static void apply_prize(uint8_t it) {
    switch (it) {
    case IT_COIN:   g_gold++; mote->audio_play_sfx(&coin_sfx, 0.8f); break;
    case IT_POUCH:  g_gold += 3; mote->audio_play_sfx(&coin_sfx, 0.9f); toast("POUCH: +3 GOLD"); break;
    case IT_GEM:    g_gems++; mote->audio_play_sfx(&gem_sfx, 0.9f); break;
    case IT_KEY:    g_keys++; mote->audio_play_sfx(&key_sfx, 0.9f); toast("A KEY!"); break;
    case IT_POTION: g_steps += 10; mote->audio_play_sfx(&food_sfx, 0.9f); toast("TONIC! +10 STEPS"); break;
    case PRIZE_KEYCARD: g_keycard = 1; mote->audio_play_sfx(&star_sfx, 1.0f); toast("THE KEYCARD!"); break;
    }
    goal_check_held();
}

/* the floor slides open by the solved puzzle; prizes wait on the boards */
static void pz_spawn_prizes(int pz) {
    uint8_t items[3];
    pz_prizes(pz, items);
    if (!items[0]) return;
    if (g_grid[g_cur].room == k_pz_room[pz] &&
        seeded_spot(g_cur, 0xA7C4u + (uint32_t)pz, &g_hatch_x[pz], &g_hatch_y[pz])) {
        memcpy(g_prize_item[pz], items, 3);
        g_prize_left[pz] = (uint8_t)((items[0] ? 1 : 0) | (items[1] ? 2 : 0) | (items[2] ? 4 : 0));
        g_hatch_t = 0;
        toast("THE FLOOR SLIDES OPEN");
        mote->audio_play_sfx(&unlock_sfx, 0.9f);
    } else {
        for (int k = 0; k < 3; k++)              /* nowhere to open: hand it over */
            if (items[k]) apply_prize(items[k]);
    }
}

static void pz_solve(int pz) {
    g_pz_solved |= (uint16_t)(1 << pz);
    mote->audio_play_sfx(&star_sfx, 1.0f);
    /* the flavour + points land now; the material prizes wait under the hatch */
    switch (pz) {
    case PZ_SAFE:    score_add(SC_BONUS, 100); toast("THE SAFE SWINGS OPEN +100"); break;
    case PZ_KEYPAD:  score_add(SC_BONUS, 75);  toast("THE CLOSET UNLOCKS +75"); break;
    case PZ_CLOCK:   score_add(SC_BONUS, 75);  toast("THE CLOCK CHIMES +75"); break;
    case PZ_GLOBE:   score_add(SC_BONUS, 50);  toast("THE GLOBE CLICKS +50"); break;
    case PZ_PIANO:   score_add(SC_BONUS, 100); toast("THE CHORD RINGS TRUE +100"); break;
    case PZ_CANDLES: score_add(SC_BONUS, 100); toast("THE ALTAR OPENS +100"); break;
    case PZ_TILES:   score_add(SC_BONUS, 100); toast("THE FLOOR RUMBLES +100"); break;
    case PZ_STATUES: score_add(SC_BONUS, 125); toast("THE STATUES BOW +125"); break;
    case PZ_BOOK:    score_add(SC_BONUS, 75);  book_passage(); break;
    case PZ_PORTRAIT: score_add(SC_BONUS, 100); toast("THE GUILTY REVEALED +100"); break;
    case PZ_WINE:    score_add(SC_BONUS, 50);  toast("A FINE VINTAGE +50"); break;
    case PZ_SCALES:  score_add(SC_BONUS, 50);  toast("THE SCALES TIP +50"); break;
    case PZ_CENSUS:  score_add(SC_BONUS, 100); toast("THE LEDGER BALANCES +100"); break;
    case PZ_CHESS:   score_add(SC_BONUS, 100); toast("CHECKMATE +100"); break;
    case PZ_SNOOKER: score_add(SC_BONUS, 100); toast("THE RACK CLICKS OPEN +100"); break;
    case PZ_IQ:      score_add(SC_BONUS, 125); toast("TOP OF THE CLASS +125"); break;
    }
    if (pz != PZ_BOOK) pz_spawn_prizes(pz);
    goal_progress(GO_PUZZLES, 1, 0);
    goal_check_held();
}

/* a wrong overlay guess without the clue: the estate takes a step */
static void pz_penalty(void) {
    toast("WRONG - THE HOUSE CREAKS -1");
    mote->audio_play_sfx(&locked_sfx, 0.9f);
    g_steps--;
    if (g_steps <= 0) { g_steps = 0; g_state = GS_PLAY; end_day(0); }
}

/* reveal one unsolved puzzle's secret into the notebook (drafted rooms
 * first); 0 if every clue is already out */
static int reveal_clue(uint32_t r) {
    int pick = -1, seen = 0;
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        seen = 0;
        for (int i = 0; i < PZ_N; i++) {
            if (i == PZ_CENSUS || i == PZ_IQ || (g_pz_solved & (1 << i)) || (g_pz_clue & (1 << i)))
                continue;
            if (pass == 0) {                      /* prefer puzzles already on the map */
                int drafted = 0;
                for (int gi = 0; gi < GRID_W * GRID_H; gi++)
                    if (g_grid[gi].room == k_pz_room[i]) drafted = 1;
                if (!drafted) continue;
            }
            seen++;
            r = hash32(r + (uint32_t)i);
            if ((int)(r % (uint32_t)seen) == 0) pick = i;
        }
    }
    if (pick < 0) return 0;
    g_pz_clue |= (uint16_t)(1 << pick);
    char b[40];
    pz_clue_text(pick, b, sizeof b);
    toast("A CLUE FOR THE NOTEBOOK:");
    toast(b);
    mote->audio_play_sfx(&tick_sfx, 0.9f);
    return 1;
}

/* read a crumpled note */
static void read_note(void) {
    Cell *cl = &g_grid[g_cur];
    cl->extra |= 2;
    g_note_on = 0;
    if (!reveal_clue(hash32(g_day_seed ^ (uint32_t)(g_cur * 30269u) ^ 0x2071u))) {
        score_add(SC_LOOT, 10);
        toast("THE NOTE IS FADED +10");
    }
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
            case IT_COIN: g_gold++; score_add(SC_LOOT, g_cond == CD_MISER ? 10 : 5); mote->audio_play_sfx(&coin_sfx, 0.8f); break;
            case IT_KEY:  g_keys++; mote->audio_play_sfx(&key_sfx, 0.9f); toast("A KEY!"); break;
            case IT_GEM:  g_gems++; mote->audio_play_sfx(&gem_sfx, 0.9f); break;
            case IT_FOOD: g_steps += 6; mote->audio_play_sfx(&food_sfx, 0.8f); toast("+6 STEPS"); break;
            case IT_POTION: g_steps += 10; mote->audio_play_sfx(&food_sfx, 0.9f); toast("TONIC! +10 STEPS"); break;
            case IT_POUCH: g_gold += 3; score_add(SC_LOOT, 5); mote->audio_play_sfx(&coin_sfx, 0.9f); toast("POUCH: +3 GOLD"); break;
            case IT_STAR: score_add(SC_LOOT, 25); mote->audio_play_sfx(&star_sfx, 0.9f); toast("EVIDENCE +25"); break;
            case IT_STAR2: score_add(SC_LOOT, 75); mote->audio_play_sfx(&star_sfx, 1.0f); toast("A PHOTOGRAPH +75"); break;
            case IT_STAR3: score_add(SC_LOOT, 100); mote->audio_play_sfx(&star_sfx, 1.0f); toast("A DOSSIER +100"); break;
            }
        }
        if (!(cl->looted & (1 << i))) all = 0;
    }
    if (n && all && !cl->swept) {
        cl->swept = 1; score_add(SC_LOOT, 20); toast("ROOM CLEARED +20");
        goal_progress(GO_SWEEPS, 1, 0);
    }
    /* hatch prizes are walk-over pickups once the lid has slid clear */
    {
        int hpz = puzzle_of_room(cl->room);
        if (hpz >= 0 && g_prize_left[hpz] && g_hatch_t > 0.55f) {
            static const int8_t k_poff[3][2] = { { 0, -10 }, { -12, 3 }, { 12, 3 } };
            for (int k = 0; k < 3; k++) {
                if (!(g_prize_left[hpz] & (1 << k))) continue;
                float ix = g_hatch_x[hpz] + k_poff[k][0], iy = g_hatch_y[hpz] + k_poff[k][1];
                if (g_px - 5 < ix + 6 && g_px + 5 > ix - 6 &&
                    g_py - 5 < iy + 6 && g_py + 5 > iy - 6) {
                    g_prize_left[hpz] &= (uint8_t)~(1 << k);
                    apply_prize(g_prize_item[hpz][k]);
                }
            }
        }
    }
    /* the crumpled note is a walk-over pickup too */
    if (g_note_on && g_px - 5 < g_note_x + 6 && g_px + 5 > g_note_x - 6 &&
        g_py - 5 < g_note_y + 6 && g_py + 5 > g_note_y - 6)
        read_note();
    /* pressure plates: the Great Hall's step-order puzzle */
    if (g_grid[g_cur].room == R_GREATHALL && !(g_pz_solved & (1 << PZ_TILES))) {
        int under = -1, nth = 0;
        for (int i = 0; i < g_nprops; i++) {
            if (g_props_cur[i].prop != P_PLATE) continue;
            float cx = g_props_cur[i].x + 6, cy = g_props_cur[i].y + 5;
            if (g_px > cx - 8 && g_px < cx + 8 && g_py > cy - 7 && g_py < cy + 7) under = nth;
            nth++;
        }
        if (under >= 0 && under != g_plate_under) {
            /* classify the plate by its compass position in the room */
            int dir = DIR_N, nth2 = 0;
            for (int i = 0; i < g_nprops; i++) {
                if (g_props_cur[i].prop != P_PLATE) continue;
                if (nth2++ != under) continue;
                float dx = g_props_cur[i].x + 6 - 56, dy = g_props_cur[i].y + 5 - 56;
                dir = (dx * dx > dy * dy) ? (dx > 0 ? DIR_E : DIR_W) : (dy > 0 ? DIR_S : DIR_N);
            }
            if (g_seq_lit & (1 << under)) {
                /* already sunk: harmless */
            } else if (dir == g_pz_sec[PZ_TILES][g_seq_n]) {
                g_seq_lit |= (uint8_t)(1 << under);
                g_seq_n++;
                mote->audio_note(262.0f + 88.0f * g_seq_n, 0.5f);
                if (g_seq_n >= 4) pz_solve(PZ_TILES);
            } else if (g_seq_n) {
                g_seq_n = 0; g_seq_lit = 0;
                toast("THE PLATES RESET");
                mote->audio_play_sfx(&locked_sfx, 0.7f);
            } else {
                mote->audio_play_sfx(&locked_sfx, 0.5f);
            }
        }
        g_plate_under = (int8_t)under;
    }
    goal_check_held();
}

/* ------------------------------------------------------------------- shops --- */
enum { SI_KEY, SI_GEM, SI_SNACK, SI_TONIC, SI_SPADE, SI_SPYGLASS,
       SI_COMPASS, SI_PENCIL, SI_MASTER, SI_KEYCARD };
typedef struct { const char *name; uint8_t price, effect; } ShopItem;
typedef struct { const char *title; const ShopItem *items; uint8_t n; } ShopDef;

static const ShopItem k_com_items[] = {
    { "KEY", 8, SI_KEY }, { "GEM", 5, SI_GEM }, { "SNACK +8", 6, SI_SNACK }, { "SPADE", 10, SI_SPADE } };
static const ShopItem k_lock_items[] = {
    { "KEY", 5, SI_KEY }, { "MASTER KEY", 30, SI_MASTER }, { "KEYCARD", 18, SI_KEYCARD } };
static const ShopItem k_apo_items[] = {
    { "TONIC +10", 8, SI_TONIC }, { "GEM", 5, SI_GEM }, { "COMPASS", 14, SI_COMPASS },
    { "PENCIL", 14, SI_PENCIL }, { "SPYGLASS", 12, SI_SPYGLASS } };
static const ShopDef k_shops[3] = {
    { "TUCK SHOP",  k_com_items,  4 },
    { "KEY SMITH",  k_lock_items, 3 },
    { "APOTHECARY", k_apo_items,  5 },
};

/* one-time tools show SOLD OUT once owned */
static int shop_owned(uint8_t effect) {
    switch (effect) {
    case SI_MASTER:   return g_master;
    case SI_KEYCARD:  return g_keycard;
    case SI_SPYGLASS: return g_spyglass;
    case SI_SPADE:    return g_spade;
    case SI_COMPASS:  return g_compass;
    case SI_PENCIL:   return g_pencil;
    default:          return 0;
    }
}

static int shop_price(const ShopItem *it) {
    int p = it->price;
    if (g_cond == CD_MARKET) p = p > 2 ? p - 2 : 1;
    return p;
}

static void shop_buy(void) {
    const ShopItem *it = &k_shops[g_shop_kind].items[g_shop_sel];
    if (shop_owned(it->effect)) { toast("SOLD OUT"); return; }
    int price = shop_price(it);
    if (g_gold < price) { toast("NOT ENOUGH GOLD"); mote->audio_play_sfx(&locked_sfx, 0.8f); return; }
    g_gold -= price;
    mote->audio_play_sfx(&buy_sfx, 0.9f);
    switch (it->effect) {
    case SI_KEY:      g_keys++;  toast("BOUGHT A KEY"); break;
    case SI_GEM:      g_gems++;  toast("BOUGHT A GEM"); break;
    case SI_SNACK:    g_steps += 8;  toast("+8 STEPS"); break;
    case SI_TONIC:    g_steps += 10; toast("TONIC +10 STEPS"); break;
    case SI_SPADE:    g_spade = 1;    toast("A STURDY SPADE"); break;
    case SI_SPYGLASS: g_spyglass = 1; toast("SPYGLASS: 4-CARD DRAFTS"); break;
    case SI_COMPASS:  g_compass = 1;  toast("THE COMPASS! LB/RB TURNS"); break;
    case SI_PENCIL:   g_pencil = 1;   toast("THE PENCIL! B REDRAWS"); break;
    case SI_MASTER:   g_master = 1;   toast("THE MASTER KEY!"); break;
    case SI_KEYCARD:  g_keycard = 1;  toast("THE KEYCARD!"); break;
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
    g_score = 0; g_master = 0; g_compass = 0; g_spyglass = 0; g_pencil = 0;
    g_chain = 0;
    g_spade = 0; g_keycard = 0; g_override = 0; g_power_off = 0;
    g_sc_rooms = g_sc_loot = g_sc_bonus = g_sc_goal = 0; g_sc_win = 0;
    g_won = 0; g_rooms_placed = 0;
    g_new_best = 0; g_frag_new = 0;
    g_toast_n = 0; g_toast_t = 0;
    memset(g_secret, 0, sizeof g_secret);
    memset(g_prize_item, 0, sizeof g_prize_item);
    memset(g_prize_left, 0, sizeof g_prize_left);
    g_hatch_t = 99; g_iq_attempt = 0; g_blind_draft = 0;
    g_map_page = 0; g_slot_pulls = 0; g_slot_spinning = 0; g_slot_done = 0;
    g_day_seed = mote_rand();
    puzzles_deal();
    /* one day in three, the golden door is sealed by three hidden levers */
    g_seal_day = (hash32(g_day_seed ^ 0x5EA15u) % 3u) == 0;
    {
        const char *es = getenv("DRAFT_SEAL");
        if (es) g_seal_day = (uint8_t)(*es != '0');
    }
    g_seal_thrown = 0;
    for (int k = 0; k < 3; k++) {
        g_lever_gi[k] = -1;
        static const uint8_t base[3] = { 2, 5, 8 };
        g_lever_ord[k] = (uint8_t)(base[k] + (hash32(g_day_seed ^ (0x1EAFu + (uint32_t)k)) & 1u));
    }
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
    if (getenv("DRAFT_TOOLS")) { g_compass = 1; g_spyglass = 1; g_pencil = 1; g_spade = 1; g_keycard = 1; }
    /* the day's condition, announced at dawn */
    g_cond = (uint8_t)(hash32(g_day_seed ^ 0xC02D5u) % CD_N);
    {
        const char *e2 = getenv("DRAFT_COND");
        if (e2) g_cond = (uint8_t)(atoi(e2) % CD_N);
        char buf[40];
        snprintf(buf, sizeof buf, "%s: %s", k_conds[g_cond].name, k_conds[g_cond].desc);
        toast(buf);
    }
    parse_room_props(g_cur);                /* re-roll finds now the seed is set */
    g_state = GS_PLAY;
    if (g_seal_day) toast("THE GOLD DOOR BEARS 3 SEALS");
    toast("FIND THE ANTECHAMBER");
    if (getenv("DRAFT_PUZ")) {              /* test hook: log the day's secrets */
        char b[48];
        for (int i = 0; i < PZ_N; i++) {
            char c[40];
            pz_clue_text(i, c, sizeof c);
            snprintf(b, sizeof b, "pz %d %s", i, c);
            mote->log(b);
        }
        snprintf(b, sizeof b, "seal_day %d ords %d %d %d", g_seal_day,
                 g_lever_ord[0], g_lever_ord[1], g_lever_ord[2]);
        mote->log(b);
    }
}

/* ------------------------------------------------- puzzle interactions ------ */
/* the bookshelf puzzle knocks a free passage through to a drafted neighbour */
static void book_passage(void) {
    Cell *cl = &g_grid[g_cur];
    for (int s = 0; s < 4; s++) {
        int n = neighbor(g_cur, s);
        if (n < 0 || g_grid[n].room == 0xFF || n == ANTE_GI) continue;
        if (cl->doors & DBIT(s)) continue;
        cl->doors |= (uint8_t)DBIT(s);
        g_grid[n].doors |= (uint8_t)DBIT(s ^ 2);
        g_secret[g_cur] |= (uint8_t)DBIT(s);
        g_secret[n] |= (uint8_t)DBIT(s ^ 2);
        char b[40];
        snprintf(b, sizeof b, "A SHELF SWINGS OPEN: %s! +75", k_dirname[s]);
        toast(b);
        toast("SECRET DOORS COST NO STEPS");
        return;
    }
    /* no wall to open: the shelf floor gives instead */
    if (g_grid[g_cur].room == R_LIBRARY &&
        seeded_spot(g_cur, 0xA7C4u + (uint32_t)PZ_BOOK, &g_hatch_x[PZ_BOOK], &g_hatch_y[PZ_BOOK])) {
        g_prize_item[PZ_BOOK][0] = IT_KEY;
        g_prize_item[PZ_BOOK][1] = 0; g_prize_item[PZ_BOOK][2] = 0;
        g_prize_left[PZ_BOOK] = 1;
        g_hatch_t = 0;
        toast("THE FLOOR SLIDES OPEN");
        mote->audio_play_sfx(&unlock_sfx, 0.9f);
    } else {
        g_keys++;
        toast("BEHIND THE BOOKS: A KEY +75");
    }
}

/* nth prop of a kind near the player (returns its ordinal, or -1) */
static int prop_near_nth(int prop_id, float range) {
    int nth = 0;
    for (int i = 0; i < g_nprops; i++) {
        if (g_props_cur[i].prop != prop_id) continue;
        const PropDef *d = &k_props[prop_id];
        float cx = g_props_cur[i].x + d->fw * 0.5f, cy = g_props_cur[i].y + d->fh * 0.5f;
        float dx = g_px - cx, dy = g_py - cy;
        if (dx * dx + dy * dy < range * range) return nth;
        nth++;
    }
    return -1;
}

static void pz_open(int pz) {
    g_pz_active = (uint8_t)pz;
    g_pz_stage = 0;
    g_pz_dsel = 0;
    memset(g_pz_dval, 0, sizeof g_pz_dval);
    g_seq_n = 0;
    if (pz == PZ_IQ) { g_iq_round = 0; iq_gen(); }
    g_state = GS_PUZZLE;
    mote->audio_play_sfx(&tick_sfx, 0.8f);
}

/* A pressed in a room: try its puzzle furniture; 1 if something answered */
static int puzzle_interact(void) {
    /* the slot machine isn't a puzzle, but it answers A the same way */
    if (g_grid[g_cur].room == R_PARLOR && prop_near_nth(P_SLOTS, 26) >= 0) {
        g_slot_spinning = 0; g_slot_done = 0; g_slot_msg[0] = 0;
        g_state = GS_SLOTS;
        mote->audio_play_sfx(&tick_sfx, 0.8f);
        return 1;
    }
    int pz = puzzle_of_room(g_grid[g_cur].room);
    if (pz < 0) return 0;
    int solved = (g_pz_solved >> pz) & 1;

    if (pz == PZ_CANDLES) {
        int c = prop_near_nth(P_CANDLE, 16);
        if (c < 0) return 0;
        if (solved) { toast("THE FLAMES BURN STEADY"); return 1; }
        if (g_seq_lit & (1 << c)) { toast("IT ALREADY BURNS"); return 1; }
        if (c == g_pz_sec[PZ_CANDLES][g_seq_n]) {
            g_seq_lit |= (uint8_t)(1 << c);
            g_seq_n++;
            mote->audio_note(392.0f + 60.0f * g_seq_n, 0.5f);
            if (g_seq_n >= 4) pz_solve(PZ_CANDLES);
        } else {
            g_seq_n = 0; g_seq_lit = 0;
            toast("THE FLAMES GUTTER OUT");
            mote->audio_play_sfx(&locked_sfx, 0.7f);
        }
        return 1;
    }
    if (pz == PZ_STATUES) {
        int c = prop_near_nth(P_STATUE, 18);
        if (c < 0) return 0;
        if (solved) { toast("THE STATUES REST"); return 1; }
        g_statue_face[c] = (uint8_t)((g_statue_face[c] + 1) % 4);
        mote->audio_play_sfx(&tick_sfx, 0.8f);
        if (!memcmp(g_statue_face, g_pz_sec[PZ_STATUES], 4)) pz_solve(PZ_STATUES);
        return 1;
    }
    if (pz == PZ_TILES) return 0;              /* stepped, not pressed */

    /* overlay puzzles hang off one anchor prop each */
    static const uint8_t k_anchor[PZ_N] = {
        [PZ_SAFE] = P_SAFE, [PZ_KEYPAD] = P_KEYPAD, [PZ_CLOCK] = P_CLOCK,
        [PZ_GLOBE] = P_GLOBE, [PZ_PIANO] = P_PIANO, [PZ_BOOK] = P_SHELF_BIG,
        [PZ_PORTRAIT] = P_LECTERN, [PZ_WINE] = P_WINERACK, [PZ_SCALES] = P_SCALES,
        [PZ_CENSUS] = P_LECTERN, [PZ_CHESS] = P_CHESSBOARD, [PZ_SNOOKER] = P_RACK,
        [PZ_IQ] = P_BLACKBOARD,
    };
    if (prop_near_nth(k_anchor[pz], 24) < 0) return 0;
    if (solved) { toast("NOTHING MORE HERE"); return 1; }
    if (pz == PZ_CENSUS && !census_pick()) {
        toast("THE LEDGER NEEDS MORE ROOMS");
        return 1;
    }
    pz_open(pz);
    return 1;
}

/* ------------------------------------------------------------------ movement -- */
static void player_tick(float dt) {
    const MoteInput *in = mote->input();
    if (g_hatch_t < 60.0f) g_hatch_t += dt;
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

    if (mote_just_pressed(in, MOTE_BTN_RB)) {
        g_state = GS_MAP;
        g_map_page = 0;
        g_map_cur = (uint8_t)g_cur;
        mote->audio_play_sfx(&tick_sfx, 0.7f);
    }
    if (mote_just_pressed(in, MOTE_BTN_MENU)) { g_pause_sel = 0; g_state = GS_PAUSE; }

    /* A: dig a sparkling spot underfoot, else open a shop counter */
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        const RoomDef *d = &k_rooms[g_grid[g_cur].room];
        float ddx = g_px - g_dig_x, ddy = g_py - g_dig_y;
        int on_dig = g_dig_on && ddx * ddx + ddy * ddy < 10 * 10;
        int used = 0;
        if (on_dig && !g_spade) {
            toast("PACKED EARTH - NEED A SPADE");
            mote->audio_play_sfx(&locked_sfx, 0.8f);
            used = 1;
        } else if (on_dig) {
            dig_here();
            used = 1;
        }
        /* terminals + the breaker */
        for (int i = 0; !used && i < g_nprops; i++) {
            int pr = g_props_cur[i].prop;
            if (pr != P_TERMINAL && pr != P_BREAKER) continue;
            float tx = g_props_cur[i].x + 8, ty = g_props_cur[i].y + 16;
            float dx2 = g_px - tx, dy2 = g_py - ty;
            if (dx2 * dx2 + dy2 * dy2 > 20 * 20) continue;
            Cell *cl2 = &g_grid[g_cur];
            if (pr == P_TERMINAL) {
                if (cl2->extra & 4) { toast("TERMINAL SPENT"); }
                else {
                    cl2->extra |= 4;
                    g_override++;
                    toast("OVERRIDE ARMED:");
                    toast("ONE KEYCARD DOOR");
                    mote->audio_play_sfx(&unlock_sfx, 1.0f);
                }
            } else {
                if (g_power_off) { toast("THE BREAKER IS DOWN"); }
                else {
                    g_power_off = 1;
                    toast("BREAKER THROWN - READERS DARK");
                    toast("THE HALLS DIM... LOCKS STIFFEN");
                    mote->audio_play_sfx(&locked_sfx, 1.0f);
                }
            }
            used = 1;
        }
        /* seal levers */
        for (int k = 0; !used && k < 3; k++) {
            if (g_lever_gi[k] != g_cur) continue;
            float dx2 = g_px - g_lever_x[k], dy2 = g_py - g_lever_y[k];
            if (dx2 * dx2 + dy2 * dy2 > 18 * 18) continue;
            used = 1;
            if (g_seal_thrown & (1 << k)) { toast("THE LEVER IS THROWN"); continue; }
            g_seal_thrown |= (uint8_t)(1 << k);
            mote->audio_play_sfx(&unlock_sfx, 1.0f);
            int c = (g_seal_thrown & 1) + ((g_seal_thrown >> 1) & 1) + ((g_seal_thrown >> 2) & 1);
            toastf("A SEAL RELEASES  %d/3", c);
            if (g_seal_thrown == 7) {
                toast("THE GOLDEN DOOR UNSEALS!");
                mote->audio_play_sfx(&star_sfx, 1.0f);
            }
        }
        if (!used) used = puzzle_interact();
        if (!used && (d->flags & RF_ANY_SHOP)) {
            g_shop_kind = (d->flags & RF_SHOP_LOCK) ? 1 : (d->flags & RF_SHOP_APO) ? 2 : 0;
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

    /* floor: the room's rule tileset, rendered by the engine autotiler
     * (its nvar hash picks the variant row per tile) */
    static const MoteAutotile *floor_set[1];      /* engine reads this at raster time */
    floor_set[0] = k_floors[rd->floor];
    if (!k_room_terrain[0])
        memset(k_room_terrain, 1, sizeof k_room_terrain);
    mote->scene2d_set_autotiles(k_room_terrain, 14, 14, floor_set, 1);

    /* walls: 8x8 rule tiles over the whole wall grid (ring + any carved
     * corridor interior), cell picked by the EDGE16 neighbour mask */
    for (int gy = 0; gy < WGRID; gy++)
        for (int gx = 0; gx < WGRID; gx++) {
            if (!g_wgrid[gy][gx]) continue;
            int m = 0;
            if (gy > 0 && g_wgrid[gy - 1][gx]) m |= 1;
            if (gx < WGRID - 1 && g_wgrid[gy][gx + 1]) m |= 2;
            if (gy < WGRID - 1 && g_wgrid[gy + 1][gx]) m |= 4;
            if (gx > 0 && g_wgrid[gy][gx - 1]) m |= 8;
            add_spr(wall->sheet, gx * 8, gy * 8,
                    (m % 4) * 8, (m / 4) * 8, 8, 8, 1, 0);
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
        if (st == DS_CARD)
            add_spr(&items_img, k_door_px[s][0] + 2, k_door_px[s][1] + 2, 15 * 12, 0, 12, 12, 30, 0);
    }

    /* furniture, painter-ordered by base line; the rug lies flat, wall
     * pieces hang up on the band */
    for (int i = 0; i < g_nprops; i++) {
        int pid = g_props_cur[i].prop;
        const PropDef *d = &k_props[pid];
        int y = g_props_cur[i].y;
        int layer = 3 + ((y + d->fh) >> 3);
        if (pid == P_RUG) layer = 2;
        if (prop_wall_mounted(pid)) layer = 2;
        add_spr(k_prop_sheets[d->sheet], g_props_cur[i].x, y,
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

    /* the opened prize hatch: pit, sliding lid, and what still waits inside */
    {
        int hpz = puzzle_of_room(cl->room);
        if (hpz >= 0 && (g_pz_solved & (1 << hpz)) && g_prize_item[hpz][0]) {
            const PropDef *pit = &k_props[P_HATCH_PIT], *lid = &k_props[P_HATCH_LID];
            int hx = g_hatch_x[hpz] - 8, hy = g_hatch_y[hpz] - 7;
            float sl = g_hatch_t * 2.2f;
            if (sl > 1.0f) sl = 1.0f;
            add_spr(k_prop_sheets[pit->sheet], hx, hy, pit->fx, pit->fy, pit->fw, pit->fh, 2, 0);
            add_spr(k_prop_sheets[lid->sheet], hx + (int)(sl * 15.0f), hy,
                    lid->fx, lid->fy, lid->fw, lid->fh, 2, 0);
            static const int8_t k_poff[3][2] = { { 0, -10 }, { -12, 3 }, { 12, 3 } };
            for (int k = 0; k < 3; k++) {
                if (!(g_prize_left[hpz] & (1 << k))) continue;
                uint8_t it = g_prize_item[hpz][k];
                int cell2 = it == PRIZE_KEYCARD ? 15 : k_item_cell[it];
                int ix = g_hatch_x[hpz] + k_poff[k][0], iy = g_hatch_y[hpz] + k_poff[k][1];
                add_spr(&items_img, ix - 6, iy - 6, cell2 * 12, 0, 12, 12,
                        3 + ((iy + 6) >> 3), 0);
            }
        }
    }
    /* crumpled clue note */
    if (g_note_on)
        add_spr(&items_img, g_note_x - 6, g_note_y - 6, 14 * 12, 0, 12, 12,
                3 + ((g_note_y + 6) >> 3), 0);
    /* seal levers (thrown = handle laid over, lamp goes green) */
    for (int k = 0; k < 3; k++)
        if (g_lever_gi[k] == g_cur) {
            const PropDef *ld = &k_props[(g_seal_thrown & (1 << k)) ? P_LEVER_ON : P_LEVER];
            add_spr(k_prop_sheets[ld->sheet], g_lever_x[k] - 6, g_lever_y[k] - 8,
                    ld->fx, ld->fy, ld->fw, ld->fh,
                    3 + (((int)g_lever_y[k] + 8) >> 3), 0);
        }

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

/* darken a horizontal band to a quarter — translucent backdrop for text */
static void dim_band(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < 128; x++) {
            uint16_t p = fb[y * 128 + x];
            p = (uint16_t)((p >> 1) & 0x7BEF);
            fb[y * 128 + x] = (uint16_t)((p >> 1) & 0x7BEF);
        }
}

/* chunky pixel arrowhead, tip at (x,y), pointing DIR_N/E/S/W */
static void draw_arrow(uint16_t *fb, int x, int y, int dir, uint16_t c) {
    for (int i = 0; i < 3; i++) {
        int w = 1 + 2 * i;
        switch (dir) {
        case DIR_N: mote->draw_rect(fb, x - i, y + i, w, 1, c, 1, 0, 128); break;
        case DIR_S: mote->draw_rect(fb, x - i, y - i, w, 1, c, 1, 0, 128); break;
        case DIR_W: mote->draw_rect(fb, x + i, y - i, 1, w, c, 1, 0, 128); break;
        default:    mote->draw_rect(fb, x - i, y - i, 1, w, c, 1, 0, 128); break;
        }
    }
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
}

/* the toolkit, stacked beside the estate map */
static void tools_draw(uint16_t *fb, int x, int y) {
    static const uint8_t cells[6] = { 6, 10, 11, 13, 15, 16 };
    uint8_t held[6] = { g_master, g_compass, g_spyglass, g_pencil, g_keycard, g_spade };
    for (int i = 0; i < 6; i++) {
        if (!held[i]) continue;
        mote->blit(fb, &items_img, x, y, cells[i] * 12, 0, 12, 12, 0, 0, 128);
        y += 13;
    }
}

static void toast_draw(uint16_t *fb) {
    if (!g_toast_n) return;
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    const char *s = g_toast[0];
    int len = (int)strlen(s);
    if (len <= 19) {
        dim_band(fb, 97, 112);
        mote_ftextc(mote, fb, f, 64, 99, rgb(250, 240, 190), s);
        return;
    }
    /* wrap at the last space that fits the screen */
    int cut = 19;
    for (int i = 19; i > 0; i--)
        if (s[i] == ' ') { cut = i; break; }
    char l1[40], l2[40];
    snprintf(l1, sizeof l1, "%.*s", cut, s);
    snprintf(l2, sizeof l2, "%s", s + cut + (s[cut] == ' ' ? 1 : 0));
    dim_band(fb, 85, 112);
    mote_ftextc(mote, fb, f, 64, 87, rgb(250, 240, 190), l1);
    mote_ftextc(mote, fb, f, 64, 99, rgb(250, 240, 190), l2);
}

/* one estate cell on a blueprint panel */
/* one estate cell, cw x ch (the big map page uses wide cells) */
static void bp_cell(uint16_t *fb, int x, int y, int cw, int ch, int gi, int hilite) {
    const Cell *cl = &g_grid[gi];
    mote->draw_rect(fb, x, y, cw - 1, ch - 1, rgb(20, 30, 62), 1, 0, 128);
    if (cl->room != 0xFF) {
        mote->draw_rect(fb, x + 1, y + 1, cw - 3, ch - 3, k_rooms[cl->room].map_col, 1, 0, 128);
        /* doors as WHITE notches (dark backing keeps them visible on pale fills) */
        uint16_t pip = rgb(255, 255, 255), bk = rgb(14, 18, 34);
        int mx = cw / 2 - 1, my = ch / 2 - 1;
        if (cl->doors & DBIT(DIR_N)) { mote->draw_rect(fb, x + mx - 1, y, 4, 3, bk, 1, 0, 128);
                                       mote->draw_rect(fb, x + mx, y, 2, 2, pip, 1, 0, 128); }
        if (cl->doors & DBIT(DIR_S)) { mote->draw_rect(fb, x + mx - 1, y + ch - 4, 4, 3, bk, 1, 0, 128);
                                       mote->draw_rect(fb, x + mx, y + ch - 3, 2, 2, pip, 1, 0, 128); }
        if (cl->doors & DBIT(DIR_W)) { mote->draw_rect(fb, x, y + my - 1, 3, 4, bk, 1, 0, 128);
                                       mote->draw_rect(fb, x, y + my, 2, 2, pip, 1, 0, 128); }
        if (cl->doors & DBIT(DIR_E)) { mote->draw_rect(fb, x + cw - 4, y + my - 1, 3, 4, bk, 1, 0, 128);
                                       mote->draw_rect(fb, x + cw - 3, y + my, 2, 2, pip, 1, 0, 128); }
    }
    if (gi == ANTE_GI && ch >= 8) {
        uint16_t gold = ((int)(g_result_t * 2) & 1) ? rgb(240, 205, 90) : rgb(170, 140, 60);
        int cx2 = x + cw / 2, cy2 = y + ch / 2 - 1;
        mote->draw_rect(fb, cx2 - 1, cy2 - 2, 3, 3, gold, 1, 0, 128);   /* the eye */
        mote->draw_rect(fb, cx2, cy2 + 1, 1, 1, gold, 1, 0, 128);       /* the flare */
        mote->draw_rect(fb, cx2 - 1, cy2 + 2, 3, 1, gold, 1, 0, 128);
    }
    if (hilite)
        mote->draw_rect(fb, x - 1, y - 1, cw + 1, ch + 1, rgb(255, 255, 255), 0, 0, 128);
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

static const uint16_t k_rarity_col[3] = { 0x8410, 0x2E9F, 0xFD00 };
static const char *k_rarity_name[3] = { "COMMON", "UNCOMMON", "RARE" };

/* miniature render of a room: real floor texture, wall ring, door gaps, and
 * the room's own furniture blitted at miniature scale */
static void room_icon(uint16_t *fb, int x, int y, int s, uint8_t room, uint8_t mask, int bright) {
    const RoomDef *d = &k_rooms[room];
    /* floor tiled at true texture scale, from the floor rule tileset's sheet */
    const MoteImage *fl = k_floors[d->floor]->sheet;
    int inner = s - 4;
    for (int oy = 0; oy < inner; oy += 8)
        for (int ox = 0; ox < inner; ox += 8)
            mote->blit(fb, fl, x + 2 + ox, y + 2 + oy, 0, 0,
                       inner - ox < 8 ? inner - ox : 8,
                       inner - oy < 8 ? inner - oy : 8, 0, 0, 128);
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
    /* the wall ring wears the rarity colour (dimmed when unselected) */
    uint16_t wc = k_rarity_col[d->rarity];
    if (!bright) wc = (uint16_t)((wc >> 1) & 0x7BEF);
    mote->draw_rect(fb, x, y, s, 2, wc, 1, 0, 128);
    mote->draw_rect(fb, x, y + s - 2, s, 2, wc, 1, 0, 128);
    mote->draw_rect(fb, x, y, 2, s, wc, 1, 0, 128);
    mote->draw_rect(fb, x + s - 2, y, 2, s, wc, 1, 0, 128);
    mote->draw_rect(fb, x + 2, y + 2, s - 4, s - 4, rgb(20, 24, 40), 0, 0, 128);
    uint16_t dc = bright ? rgb(255, 235, 150) : rgb(250, 252, 255);
    uint16_t db = rgb(0, 0, 0);
    int m = s / 2 - 3, g = 6;
    /* 1px dark backing keeps the door blocks readable on pale walls */
    if (mask & DBIT(DIR_N)) { mote->draw_rect(fb, x + m - 1, y, g + 2, 4, db, 1, 0, 128);
                              mote->draw_rect(fb, x + m, y, g, 3, dc, 1, 0, 128); }
    if (mask & DBIT(DIR_S)) { mote->draw_rect(fb, x + m - 1, y + s - 4, g + 2, 4, db, 1, 0, 128);
                              mote->draw_rect(fb, x + m, y + s - 3, g, 3, dc, 1, 0, 128); }
    if (mask & DBIT(DIR_W)) { mote->draw_rect(fb, x, y + m - 1, 4, g + 2, db, 1, 0, 128);
                              mote->draw_rect(fb, x, y + m, 3, g, dc, 1, 0, 128); }
    if (mask & DBIT(DIR_E)) { mote->draw_rect(fb, x + s - 4, y + m - 1, 4, g + 2, db, 1, 0, 128);
                              mote->draw_rect(fb, x + s - 3, y + m, 3, g, dc, 1, 0, 128); }
}

/* the estate blueprint, with the draft target previewed live (target_gi < 0: none) */
static void estate_map(uint16_t *fb, int ox, int oy, int cw, int ch, int target_gi) {
    mote->draw_rect(fb, ox - 3, oy - 3, GRID_W * cw + 5, GRID_H * ch + 5, rgb(10, 17, 42), 1, 0, 128);
    mote->draw_rect(fb, ox - 3, oy - 3, GRID_W * cw + 5, GRID_H * ch + 5, rgb(110, 150, 220), 0, 0, 128);
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++) {
            int gi = r * GRID_W + c;
            if (gi == target_gi) continue;
            bp_cell(fb, ox + c * cw, oy + r * ch, cw, ch, gi, gi == g_cur);
        }
    if (target_gi >= 0) {
        int c = gi_col(target_gi), r = gi_row(target_gi);
        int x = ox + c * cw, y = oy + r * ch;
        const RoomDef *d = &k_rooms[g_cards[g_draft_sel]];
        uint8_t mask = orient_mask(d->shape, g_draft_entry, g_rot[g_draft_sel]);
        int on = ((int)(g_result_t * 3) & 1) == 0;
        if (g_blind_draft) mask = 0;             /* the flash hides doors too */
        mote->draw_rect(fb, x + 1, y + 1, cw - 3, ch - 3,
                        g_blind_draft ? rgb(56, 66, 96) : on ? d->map_col : rgb(40, 56, 100),
                        1, 0, 128);
        uint16_t pip = rgb(255, 255, 255);
        int mx = cw / 2 - 1, my = ch / 2 - 1;
        if (mask & DBIT(DIR_N)) mote->draw_rect(fb, x + mx, y, 2, 2, pip, 1, 0, 128);
        if (mask & DBIT(DIR_S)) mote->draw_rect(fb, x + mx, y + ch - 3, 2, 2, pip, 1, 0, 128);
        if (mask & DBIT(DIR_W)) mote->draw_rect(fb, x, y + my, 2, 2, pip, 1, 0, 128);
        if (mask & DBIT(DIR_E)) mote->draw_rect(fb, x + cw - 3, y + my, 2, 2, pip, 1, 0, 128);
        mote->draw_rect(fb, x - 1, y - 1, cw + 1, ch + 1, rgb(255, 230, 120), 0, 0, 128);
    }
}

static void draft_footer(uint16_t *fb, const MoteFont *f) {
    char buf[8];
    snprintf(buf, sizeof buf, "%d", g_gems);
    mote->blit(fb, &items_img, 6, 115, 2 * 12, 0, 12, 12, 0, 0, 128);
    mote->text_font(fb, f, buf, 19, 115, rgb(140, 240, 220));
    int can_rd = g_pencil && g_gems > 0;
    const char *hint = g_compass ? (can_rd ? "B RD  LB/RB TURN" : "LB/RB TURN")
                                 : (can_rd ? "B REROLL" : "A PLACE");
    mote->text_font(fb, f, hint, 36, 115, rgb(150, 165, 205));
}


/* split a string onto two short lines (prefer a space break) */
static void wrap2(const char *src, int maxc, char *l1, char *l2, int cap) {
    int len = (int)strlen(src);
    if (len <= maxc) { snprintf(l1, cap, "%s", src); l2[0] = 0; return; }
    int cut = maxc;
    for (int i = maxc; i > 2; i--)
        if (src[i] == ' ') { cut = i; break; }
    snprintf(l1, cap, "%.*s", cut, src);
    snprintf(l2, cap, "%.*s", maxc, src + cut + (src[cut] == ' ' ? 1 : 0));
}

/* Layout A "drafting desk": the blueprint takes the full height of the left
 * side; a narrow dossier top-right; the room choices as a small grid of
 * miniatures bottom-right. */
static void draft_draw_a(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    estate_map(fb, 8, 8, 12, 12, g_draft_gi);

    /* dossier: name in the readable font, details in the small one */
    const RoomDef *d = &k_rooms[g_cards[g_draft_sel]];
    int afford = card_affordable(g_cards[g_draft_sel]);
    char l1[14], l2[14];
    uint16_t nc = afford ? rgb(250, 250, 255) : rgb(120, 120, 132);
    if (g_blind_draft) {
        mote->text_font(fb, f, "? ? ?", 74, 6, rgb(150, 165, 205));
        mote->text(fb, "EYES STILL", 74, 20, rgb(120, 135, 175));
        mote->text(fb, "ADJUSTING", 74, 29, rgb(120, 135, 175));
    } else {
    wrap2(d->name, 8, l1, l2, sizeof l1);
    int ty = 6;
    mote->text_font(fb, f, l1, 74, ty, nc); ty += 11;
    if (l2[0]) { mote->text_font(fb, f, l2, 74, ty, nc); ty += 11; }
    mote->text(fb, k_rarity_name[d->rarity], 74, ty + 1, k_rarity_col[d->rarity]); ty += 9;
    if (k_blurb[g_cards[g_draft_sel]]) {
        wrap2(k_blurb[g_cards[g_draft_sel]], 12, l1, l2, sizeof l1);
        uint16_t bc = afford ? rgb(180, 200, 240) : rgb(110, 110, 122);
        mote->text(fb, l1, 74, ty + 1, bc); ty += 8;
        if (l2[0]) { mote->text(fb, l2, 74, ty + 1, bc); ty += 8; }
    }
    }
    if (!afford) mote->text_font(fb, f, "!", 118, 6, rgb(255, 110, 90));

    /* resources, bottom left under the map */
    {
        char buf[8];
        static const uint8_t rc[3] = { 1, 2, 0 };          /* key, gem, gold */
        int vals[3] = { g_keys, g_gems, g_gold };
        int x = 6;
        for (int i = 0; i < 3; i++) {
            mote->blit(fb, &items_img, x, 109, rc[i] * 12, 0, 12, 12, 0, 0, 128);
            snprintf(buf, sizeof buf, "%d", vals[i]);
            x = mote->text_font(fb, f, buf, x + 12, 108,
                                i == 0 ? rgb(240, 220, 140) : i == 1 ? rgb(140, 240, 220)
                                       : rgb(250, 210, 110)) + 3;
        }
    }

    /* the hand: a vertical list of miniatures, each cost to its right
     * (larger tiles when the offer is three; compact when the spyglass
     * deals four) */
    {
        int ts = g_draft_n == 4 ? 18 : 22;
        int y0 = g_draft_n == 4 ? 46 : 50;
        int step = g_draft_n == 4 ? 20 : 25;
        for (int i = 0; i < g_draft_n; i++) {
            const RoomDef *cd = &k_rooms[g_cards[i]];
            uint8_t mask = orient_mask(cd->shape, g_draft_entry, g_rot[i]);
            int sel = i == g_draft_sel;
            int x = 74, y = y0 + i * step;
            if (sel) mote->draw_rect(fb, x - 2, y - 2, ts + 4, ts + 4, rgb(255, 230, 120), 0, 0, 128);
            if (g_blind_draft) {
                mote->draw_rect(fb, x, y, ts, ts, rgb(26, 34, 66), 1, 0, 128);
                mote->draw_rect(fb, x, y, ts, ts, sel ? rgb(150, 165, 205) : rgb(52, 74, 130), 0, 0, 128);
                mote->draw_rect(fb, x + 3, y + 3, ts - 6, ts - 6, rgb(38, 50, 92), 0, 0, 128);
                mote_ftextc(mote, fb, f, x + ts / 2, y + (ts - 10) / 2, rgb(150, 165, 205), "?");
            } else {
                room_icon(fb, x, y, ts, g_cards[i], mask, sel);
            }
            int cx = x + ts + 6;
            int cy = y + (ts - 12) / 2;
            for (int c = 0; c < cd->gems; c++) { mote->blit(fb, &items_img, cx, cy, 2 * 12, 0, 12, 12, 0, 0, 128); cx += 10; }
            if (cd->flags & RF_LOCKED) { mote->blit(fb, &items_img, cx, cy, 7 * 12, 0, 12, 12, 0, 0, 128); cx += 10; }
            if (!card_affordable(g_cards[i]))
                mote->blit(fb, &items_img, x + (ts - 12) / 2, cy, 7 * 12, 0, 12, 12, 0, 0, 128);
        }
    }
}

/* Layout B "ledger": card list left with shape icons, blueprint right. */
static void draft_draw_b(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->text_font(fb, f, "DRAFT", 8, 6, rgb(210, 224, 250));
    estate_map(fb, 86, 24, 8, 8, g_draft_gi);

    int ch = g_draft_n == 4 ? 23 : 29;
    for (int i = 0; i < g_draft_n; i++) {
        const RoomDef *d = &k_rooms[g_cards[i]];
        uint8_t mask = orient_mask(d->shape, g_draft_entry, g_rot[i]);
        int x = 6, y = 20 + i * (ch + 2), w = 74;
        int sel = i == g_draft_sel;
        int afford = card_affordable(g_cards[i]);
        mote->draw_rect(fb, x, y, w, ch, sel ? rgb(24, 36, 78) : rgb(15, 23, 54), 1, 0, 128);
        mote->draw_rect(fb, x, y, w, ch, sel ? rgb(255, 230, 120) : rgb(52, 74, 130), 0, 0, 128);
        if (g_blind_draft) {
            mote->draw_rect(fb, x + 2, y + (ch - 20) / 2, 20, 20, rgb(26, 34, 66), 1, 0, 128);
            mote_ftextc(mote, fb, f, x + 12, y + (ch - 20) / 2 + 5, rgb(150, 165, 205), "?");
        } else {
            room_icon(fb, x + 2, y + (ch - 20) / 2, 20, g_cards[i], mask, sel);
        }
        uint16_t nc = afford ? rgb(250, 250, 255) : rgb(110, 110, 122);
        mote->text_font(fb, f, g_blind_draft ? "? ? ?" : d->name, x + 24, y + 1, nc);
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
        if (!g_pencil) { toast("REDRAWS NEED THE PENCIL"); mote->audio_play_sfx(&locked_sfx, 0.8f); }
        else if (g_gems > 0) { g_gems--; deal_cards(); g_draft_sel = 0; mote->audio_play_sfx(&gem_sfx, 0.8f); }
        else { toast("NO GEMS TO REROLL"); mote->audio_play_sfx(&locked_sfx, 0.8f); }
    }
}

static void notebook_draw(uint16_t *fb);
static void satchel_draw(uint16_t *fb);

/* shared page furniture: LB/RB arrows + the page title */
static void page_header(uint16_t *fb, const char *title) {
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, f, 64, 3, rgb(250, 240, 190), title);
    draw_arrow(fb, 6, 8, DIR_W, rgb(150, 165, 205));
    draw_arrow(fb, 121, 8, DIR_E, rgb(150, 165, 205));
}

static void map_draw(uint16_t *fb) {
    if (g_map_page == 1) { notebook_draw(fb); return; }
    if (g_map_page == 2) { satchel_draw(fb); return; }
    paper(fb);
    page_header(fb, "THE ESTATE");
    estate_map(fb, 34, 17, 12, 12, -1);
    /* the browse cursor */
    {
        int c = gi_col(g_map_cur), r = gi_row(g_map_cur);
        int x = 34 + c * 12, y = 17 + r * 12;
        mote->draw_rect(fb, x - 1, y - 1, 13, 13, rgb(255, 230, 120), 0, 0, 128);
    }
    /* the single name line: what the cursor rests on */
    {
        const Cell *cl = &g_grid[g_map_cur];
        char buf[32];
        if (cl->room == 0xFF)
            snprintf(buf, sizeof buf, "UNDRAFTED");
        else if (g_map_cur == g_cur)
            snprintf(buf, sizeof buf, "%s - HERE", k_rooms[cl->room].name);
        else
            snprintf(buf, sizeof buf, "%s", k_rooms[cl->room].name);
        mote_ftextc(mote, fb, mote->ui_font(MOTE_FONT_MED), 64, 114,
                    cl->room == 0xFF ? rgb(120, 135, 175) : rgb(240, 240, 250), buf);
    }
}

static void shop_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    const ShopDef *s = &k_shops[g_shop_kind];
    int n = s->n;
    mote->draw_rect(fb, 10, 22, 108, 20 + n * 15, rgb(16, 20, 40), 1, 0, 128);
    mote->draw_rect(fb, 10, 22, 108, 20 + n * 15, rgb(120, 140, 200), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 24, rgb(250, 230, 150), s->title);
    char buf[32];
    for (int i = 0; i < n; i++) {
        const ShopItem *it = &s->items[i];
        int y = 39 + i * 15;
        if (i == g_shop_sel) mote->draw_rect(fb, 12, y - 1, 104, 14, rgb(40, 50, 90), 1, 0, 128);
        int sold = shop_owned(it->effect);
        snprintf(buf, sizeof buf, "%s", sold ? "SOLD OUT" : it->name);
        mote->text_font(fb, f, buf, 16, y, g_gold >= shop_price(it) && !sold ? rgb(240, 240, 250) : rgb(120, 120, 130));
        if (!sold) {
            snprintf(buf, sizeof buf, "%d", shop_price(it));
            mote->blit(fb, &items_img, 92, y, 0, 0, 12, 12, 0, 0, 128);
            mote->text_font(fb, f, buf, 105, y, rgb(250, 210, 110));
        }
    }
}

static void shop_tick(void) {
    const MoteInput *in = mote->input();
    int n = k_shops[g_shop_kind].n;
    if (mote_just_pressed(in, MOTE_BTN_UP))   { g_shop_sel = (g_shop_sel + n - 1) % n; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) { g_shop_sel = (g_shop_sel + 1) % n; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_A)) shop_buy();
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU)) g_state = GS_PLAY;
}

/* ------------------------------------------------------- puzzle overlay ----- */
static void puzzle_tick(void) {
    const MoteInput *in = mote->input();
    int pz = g_pz_active;
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU)) {
        g_state = GS_PLAY;
        return;
    }
    if (pz == PZ_PIANO) {
        /* seven real keys, C major octave; walk the keyboard and strike with A */
        static const float k_freq[7] = { 261.6f, 293.7f, 329.6f, 349.2f, 392.0f, 440.0f, 493.9f };
        if (mote_just_pressed(in, MOTE_BTN_LEFT) && g_pz_dsel > 0)
            { g_pz_dsel--; mote->audio_play_sfx(&tick_sfx, 0.5f); }
        if (mote_just_pressed(in, MOTE_BTN_RIGHT) && g_pz_dsel < 6)
            { g_pz_dsel++; mote->audio_play_sfx(&tick_sfx, 0.5f); }
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            mote->audio_note(k_freq[g_pz_dsel], 0.5f);
            g_pz_dval[g_seq_n++] = g_pz_dsel;
            if (g_seq_n >= 4) {
                if (!memcmp(g_pz_dval, g_pz_sec[PZ_PIANO], 4)) {
                    pz_solve(PZ_PIANO);
                    g_state = GS_PLAY;
                } else {
                    toast("...DISCORD");
                    g_seq_n = 0;
                }
            }
        }
        return;
    }
    if (pz == PZ_IQ) {
        if (mote_just_pressed(in, MOTE_BTN_LEFT) && g_iq_sel > 0)
            { g_iq_sel--; mote->audio_play_sfx(&tick_sfx, 0.6f); }
        if (mote_just_pressed(in, MOTE_BTN_RIGHT) && g_iq_sel < 3)
            { g_iq_sel++; mote->audio_play_sfx(&tick_sfx, 0.6f); }
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            if (g_iq_sel == g_iq_answer) {
                g_iq_round++;
                mote->audio_note(392.0f + 88.0f * g_iq_round, 0.5f);
                if (g_iq_round >= 3) {
                    pz_solve(PZ_IQ);
                    g_state = GS_PLAY;
                } else {
                    iq_gen();
                }
            } else {
                g_iq_attempt++;
                g_iq_round = 0;
                iq_gen();
                pz_penalty();
            }
        }
        return;
    }
    int n = 1; uint8_t dmax[4] = { 1, 1, 1, 1 };
    pz_dials(pz, &n, dmax);
    if (n > 1 && mote_just_pressed(in, MOTE_BTN_LEFT))
        { g_pz_dsel = (uint8_t)((g_pz_dsel + n - 1) % n); mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (n > 1 && mote_just_pressed(in, MOTE_BTN_RIGHT))
        { g_pz_dsel = (uint8_t)((g_pz_dsel + 1) % n); mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_UP))
        { g_pz_dval[g_pz_dsel] = (uint8_t)((g_pz_dval[g_pz_dsel] + 1) % dmax[g_pz_dsel]); mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN))
        { g_pz_dval[g_pz_dsel] = (uint8_t)((g_pz_dval[g_pz_dsel] + dmax[g_pz_dsel] - 1) % dmax[g_pz_dsel]); mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (pz_dials_right(pz)) {
            pz_solve(pz);
            g_state = GS_PLAY;
        } else if (g_pz_clue & (1 << pz)) {
            toast("THAT'S NOT IT");                /* you hold the clue: typos are free */
            mote->audio_play_sfx(&locked_sfx, 0.7f);
        } else {
            pz_penalty();
        }
    }
}

/* one reasoning glyph: square / diamond / triangle (rotatable) / cross */
static void iq_shape(uint16_t *fb, int cx, int cy, int shape, int s, int rot, uint16_t col) {
    int h = s / 2;
    if (shape == 0) {
        mote->draw_rect(fb, cx - h, cy - h, s, s, col, 1, 0, 128);
    } else if (shape == 1) {
        for (int i = -h; i <= h; i++) {
            int a = i < 0 ? -i : i, w = s - 2 * a;
            mote->draw_rect(fb, cx - w / 2, cy + i, w, 1, col, 1, 0, 128);
        }
    } else if (shape == 2) {
        for (int i = 0; i < s; i++) {
            int w = i | 1;
            if (w > s) w = s;
            switch (rot) {
            case 0: mote->draw_rect(fb, cx - w / 2, cy - h + i, w, 1, col, 1, 0, 128); break;
            case 2: mote->draw_rect(fb, cx - w / 2, cy + h - i, w, 1, col, 1, 0, 128); break;
            case 1: mote->draw_rect(fb, cx + h - i, cy - w / 2, 1, w, col, 1, 0, 128); break;
            default: mote->draw_rect(fb, cx - h + i, cy - w / 2, 1, w, col, 1, 0, 128); break;
            }
        }
    } else {
        int bar = s >= 9 ? 3 : 2;
        mote->draw_rect(fb, cx - h, cy - bar / 2, s, bar, col, 1, 0, 128);
        mote->draw_rect(fb, cx - bar / 2, cy - h, bar, s, col, 1, 0, 128);
    }
}

static void iq_cell_draw(uint16_t *fb, int cx, int cy, const IqCell *c) {
    uint16_t col = rgb(240, 240, 250), bg = rgb(24, 30, 58);
    if (c->count == 1) {
        int s = 5 + 2 * c->size;
        iq_shape(fb, cx, cy, c->shape, s, c->rot, col);
        if (!c->fill && s - 4 >= 1)
            iq_shape(fb, cx, cy, c->shape, s - 4, c->rot, bg);
    } else {
        static const int8_t o[4][4][2] = {
            { { 0, 0 } },
            { { -4, 0 }, { 4, 0 } },
            { { -4, -4 }, { 4, -4 }, { 0, 4 } },
            { { -4, -4 }, { 4, -4 }, { -4, 4 }, { 4, 4 } },
        };
        for (int k = 0; k < c->count; k++) {
            iq_shape(fb, cx + o[c->count - 1][k][0], cy + o[c->count - 1][k][1],
                     c->shape, 5, c->rot, col);
            if (!c->fill)
                iq_shape(fb, cx + o[c->count - 1][k][0], cy + o[c->count - 1][k][1],
                         c->shape, 1, c->rot, bg);
        }
    }
}

static void puzzle_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    int pz = g_pz_active;
    mote->draw_rect(fb, 8, 18, 112, 88, rgb(16, 20, 40), 1, 0, 128);
    mote->draw_rect(fb, 8, 18, 112, 88, rgb(120, 140, 200), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 21, rgb(250, 230, 150), k_pz_name[pz]);
    char b[40];
    if (pz == PZ_IQ) {
        /* the sequence, then the four candidates */
        for (int i = 0; i < 4; i++) {
            int x = 19 + i * 23;
            mote->draw_rect(fb, x, 33, 18, 18, rgb(24, 30, 58), 1, 0, 128);
            mote->draw_rect(fb, x, 33, 18, 18, rgb(70, 90, 150), 0, 0, 128);
            if (i < 3) iq_cell_draw(fb, x + 9, 42, &g_iq_seq[i]);
            else mote_ftextc(mote, fb, f, x + 9, 37, rgb(250, 220, 130), "?");
        }
        for (int i = 0; i < 4; i++) {
            int x = 19 + i * 23;
            int sel = i == (int)g_iq_sel;
            mote->draw_rect(fb, x, 60, 18, 18, sel ? rgb(40, 50, 90) : rgb(24, 30, 58), 1, 0, 128);
            mote->draw_rect(fb, x, 60, 18, 18, sel ? rgb(255, 230, 120) : rgb(70, 90, 150), 0, 0, 128);
            iq_cell_draw(fb, x + 9, 69, &g_iq_opt[i]);
            if (sel) draw_arrow(fb, x + 9, 55, DIR_N, rgb(255, 230, 120));
        }
        snprintf(b, sizeof b, "QUESTION %d OF 3", g_iq_round + 1);
        mote_ftextc(mote, fb, f, 64, 82, rgb(190, 205, 240), b);
        mote_ftextc(mote, fb, f, 64, 93, rgb(150, 165, 205), "A ANSWER   B LEAVE");
        return;
    }
    if (pz == PZ_PIANO) {
        /* a real keyboard: C D E F G A B, cursor walks it, A strikes */
        for (int i = 0; i < 7; i++) {
            int x = 12 + i * 15;
            int sel = i == g_pz_dsel;
            mote->draw_rect(fb, x, 38, 13, 28, sel ? rgb(255, 244, 200) : rgb(236, 236, 240), 1, 0, 128);
            mote->draw_rect(fb, x, 38, 13, 28, sel ? rgb(255, 230, 120) : rgb(60, 60, 80), 0, 0, 128);
            if (i != 2 && i != 6)                    /* black-key notches (none at E-F, B-C) */
                mote->draw_rect(fb, x + 9, 38, 7, 12, rgb(30, 30, 44), 1, 0, 128);
            char kb[2] = { "CDEFGAB"[i], 0 };
            mote->text_font(fb, f, kb, x + 3, 54, rgb(30, 30, 44));
            if (sel) draw_arrow(fb, x + 6, 32, DIR_N, rgb(255, 230, 120));
        }
        for (int i = 0; i < 4; i++) {                /* the four-note staff below */
            int x = 46 + i * 10;
            if (i < (int)g_seq_n)
                mote->draw_rect(fb, x, 72, 6, 6, rgb(250, 220, 110), 1, 0, 128);
            else
                mote->draw_rect(fb, x, 72, 6, 6, rgb(60, 74, 120), 0, 0, 128);
        }
    } else {
        int n = 1; uint8_t dmax[4] = { 1, 1, 1, 1 };
        pz_dials(pz, &n, dmax);
        int dy = 42;
        if (pz == PZ_CENSUS) {                       /* the question sits above the dial */
            snprintf(b, sizeof b, "COUNT %s", k_census_nm[g_census_kind]);
            mote_ftextc(mote, fb, f, 64, 31, rgb(190, 205, 240), b);
            snprintf(b, sizeof b, "IN THE %s", k_rooms[g_census_room].name);
            mote_ftextc(mote, fb, f, 64, 42, rgb(190, 205, 240), b);
            dy = 56;
        }
        if (pz == PZ_SNOOKER) {                      /* the rack's sum, in balls */
            static const uint16_t k_ball[7] = {
                MOTE_RGB565(200, 40, 34),  MOTE_RGB565(232, 202, 54),
                MOTE_RGB565(64, 182, 84),  MOTE_RGB565(150, 90, 45),
                MOTE_RGB565(70, 120, 220), MOTE_RGB565(242, 142, 172),
                MOTE_RGB565(26, 22, 26),
            };
            static const uint8_t k_ball_row[7][2] = {
                { 2, 3 }, { 1, 5 }, { 0, 7 }, { 0, 7 }, { 0, 7 }, { 1, 5 }, { 2, 3 } };
            static const char *k_op[3] = { "+", "-", "X" };
            const uint8_t *sc = g_pz_sec[PZ_SNOOKER];
            int bxs[2] = { 30, 58 }, bi[2] = { sc[0], sc[2] };
            for (int k = 0; k < 2; k++) {
                uint16_t c = k_ball[bi[k]];
                uint16_t shade = (uint16_t)(((c >> 1) & 0x7BEF) + ((c >> 2) & 0x39E7));
                for (int ry = 0; ry < 7; ry++)
                    mote->draw_rect(fb, bxs[k] + 2 + k_ball_row[ry][0], 32 + ry,
                                    k_ball_row[ry][1], 1, ry >= 5 ? shade : c, 1, 0, 128);
                mote->draw_rect(fb, bxs[k] + 4, 33, 1, 1, rgb(255, 255, 255), 1, 0, 128);
                mote->draw_rect(fb, bxs[k] + 5, 34, 1, 1, rgb(255, 255, 255), 1, 0, 128);
            }
            mote_ftextc(mote, fb, f, 50, 32, rgb(240, 240, 250), k_op[sc[1]]);
            mote_ftextc(mote, fb, f, 78, 32, rgb(240, 240, 250), "=");
            mote_ftextc(mote, fb, f, 92, 32, rgb(150, 165, 205), "?");
            dy = 56;
        }
        int bw = n > 2 ? 24 : 34;
        if (pz == PZ_CHESS) bw = 0;                  /* mixed widths: piece wide, coords slim */
        int total = pz == PZ_CHESS ? 46 + 6 + 18 + 6 + 18 : n * bw + (n - 1) * 6;
        int x0 = 64 - total / 2;
        int x = x0;
        for (int i = 0; i < n; i++) {
            int w = pz == PZ_CHESS ? (i == 0 ? 46 : 18) : bw;
            int sel = i == g_pz_dsel;
            mote->draw_rect(fb, x, dy, w, 18, sel ? rgb(40, 50, 90) : rgb(24, 30, 58), 1, 0, 128);
            mote->draw_rect(fb, x, dy, w, 18, sel ? rgb(255, 230, 120) : rgb(70, 90, 150), 0, 0, 128);
            pz_dial_text(pz, i, g_pz_dval[i], b, sizeof b);
            mote_ftextc(mote, fb, f, x + w / 2, dy + 4, rgb(240, 240, 250), b);
            if (sel) {
                draw_arrow(fb, x + w / 2, dy - 6, DIR_N, rgb(255, 230, 120));
                draw_arrow(fb, x + w / 2, dy + 23, DIR_S, rgb(255, 230, 120));
            }
            x += w + 6;
        }
    }
    /* the clue, if the notebook holds it */
    if (g_pz_clue & (1 << pz)) {
        pz_clue_text(pz, b, sizeof b);
        mote_ftextc(mote, fb, f, 64, 78, rgb(250, 220, 130), b);
    } else if (pz != PZ_CENSUS) {
        mote->text(fb, "NO CLUE: GUESS -1 STEP", 16, 81, rgb(150, 150, 170));
    }
    mote_ftextc(mote, fb, f, 64, 93, rgb(150, 165, 205),
                pz == PZ_PIANO ? "A PLAY   B LEAVE" : "A TRY   B LEAVE");
}

/* ---------------------------------------------------------- slot machine ---- */
enum { SY_COIN, SY_BOOT, SY_GEM, SY_KEY, SY_CROWN };
static const uint8_t k_sym_cell[5] = { 0, 8, 2, 1, 19 };

static uint8_t slot_pick(void) {
    static const uint8_t wn[5] = { 34, 26, 16, 14, 10 };
    static const uint8_t wl[5] = { 30, 22, 17, 15, 16 };
    const uint8_t *w = g_cond == CD_LUCKY ? wl : wn;
    int roll = (int)(mote_rand() % 100u);
    for (int i = 0; i < 5; i++) {
        roll -= w[i];
        if (roll < 0) return (uint8_t)i;
    }
    return SY_COIN;
}

static void slot_eval(void) {
    int a = g_slot_sym[0], b = g_slot_sym[1], c = g_slot_sym[2];
    if (a == b && b == c) {
        switch (a) {
        case SY_COIN:  g_gold += 12; snprintf(g_slot_msg, sizeof g_slot_msg, "3 COINS! +12 GOLD"); break;
        case SY_BOOT:  g_steps += 8; snprintf(g_slot_msg, sizeof g_slot_msg, "3 BOOTS! +8 STEPS"); break;
        case SY_GEM:   g_gems++; g_gold += 6; snprintf(g_slot_msg, sizeof g_slot_msg, "3 GEMS! +GEM +6G"); break;
        case SY_KEY:   g_keys++; g_gold += 4; snprintf(g_slot_msg, sizeof g_slot_msg, "3 KEYS! +KEY +4G"); break;
        default:       g_gold += 30; score_add(SC_LOOT, 100);
                       snprintf(g_slot_msg, sizeof g_slot_msg, "JACKPOT! +30G +100");
                       toast("CROWNS! THE HOUSE PAYS OUT");
                       break;
        }
        mote->audio_play_sfx(a == SY_CROWN ? &win_sfx : &star_sfx, 1.0f);
    } else if (a == b || b == c || a == c) {
        int p = g_cond == CD_LUCKY ? 5 : 4;
        g_gold += p;
        snprintf(g_slot_msg, sizeof g_slot_msg, "A PAIR  +%d GOLD", p);
        mote->audio_play_sfx(&coin_sfx, 0.9f);
    } else {
        snprintf(g_slot_msg, sizeof g_slot_msg, "NO LUCK");
        mote->audio_play_sfx(&locked_sfx, 0.6f);
    }
    goal_check_held();
}

static void slots_tick(float dt) {
    const MoteInput *in = mote->input();
    if (g_slot_spinning) {
        g_slot_t += dt;
        int stopped = g_slot_t > 1.3f ? 3 : g_slot_t > 0.9f ? 2 : g_slot_t > 0.5f ? 1 : 0;
        if (stopped > g_slot_done) {
            g_slot_done = (uint8_t)stopped;
            mote->audio_play_sfx(&tick_sfx, 0.8f);
        }
        if (stopped == 3) {
            g_slot_spinning = 0;
            slot_eval();
        }
        return;
    }
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU)) {
        g_state = GS_PLAY;
        return;
    }
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (g_slot_pulls >= 6) {
            snprintf(g_slot_msg, sizeof g_slot_msg, "THE MACHINE JAMS");
            mote->audio_play_sfx(&locked_sfx, 0.8f);
        } else if (g_gold < 3) {
            snprintf(g_slot_msg, sizeof g_slot_msg, "NEED 3 GOLD");
            mote->audio_play_sfx(&locked_sfx, 0.8f);
        } else {
            g_gold -= 3;
            g_slot_pulls++;
            for (int i = 0; i < 3; i++) g_slot_sym[i] = slot_pick();
            g_slot_spinning = 1;
            g_slot_done = 0;
            g_slot_t = 0;
            g_slot_msg[0] = 0;
            mote->audio_play_sfx(&coin_sfx, 0.8f);
        }
    }
}

static void slots_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 16, 20, 96, 86, rgb(70, 22, 22), 1, 0, 128);
    mote->draw_rect(fb, 16, 20, 96, 86, rgb(240, 205, 90), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 23, rgb(255, 236, 140), "LUCKY CROWNS");
    /* three reel windows */
    for (int i = 0; i < 3; i++) {
        int x = 32 + i * 24;
        mote->draw_rect(fb, x, 38, 20, 20, rgb(20, 16, 20), 1, 0, 128);
        mote->draw_rect(fb, x, 38, 20, 20, rgb(150, 120, 60), 0, 0, 128);
        int sym = g_slot_sym[i];
        if (g_slot_spinning && (int)g_slot_done <= i)
            sym = (int)(g_slot_t * 16.0f + i * 2) % 5;   /* blur through the drum */
        mote->blit(fb, &items_img, x + 4, 42, k_sym_cell[sym] * 12, 0, 12, 12, 0, 0, 128);
    }
    char b[24];
    if (g_slot_msg[0]) mote_ftextc(mote, fb, f, 64, 63, rgb(255, 236, 140), g_slot_msg);
    snprintf(b, sizeof b, "PULLS LEFT %d", 6 - g_slot_pulls);
    mote->text(fb, b, 24, 78, rgb(230, 200, 160));
    snprintf(b, sizeof b, "GOLD %d", g_gold);
    mote->text(fb, b, 86, 78, rgb(255, 220, 120));
    mote_ftextc(mote, fb, f, 64, 92, rgb(230, 200, 160), "A PULL 3G  B LEAVE");
}

/* ------------------------------------------------------------- case board --- */
/* the case, one fragment per win: someone was here before you — the house
 * knows you — it was always you */
static const char *k_frag[NFRAG][2] = {
    { "THE EVIDENCE WAS",    "HANDLED BEFORE ME." },
    { "SECURITY LOGS ONE",   "VISITOR. EVERY DAY." },
    { "EVERY ANSWER FEELS",  "LIKE A MEMORY." },
    { "THE SPADE TURNS UP",  "THINGS I BURIED." },
    { "THE CRUMPLED NOTES",  "ARE IN MY OWN HAND." },
    { "NOTHING REDRAWS",     "AT DAWN BUT ME." },
    { "I SET THE THREE",     "SEALS MYSELF." },
    { "THE MISSING MAN'S",   "PRINT IS MY OWN." },
};
static int g_case_page;

static void case_draw(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    int found = 0;
    for (int i = 0; i < NFRAG; i++) found += (g_frags >> i) & 1;
    mote_ftextc(mote, fb, f, 64, 6, rgb(250, 240, 190), "THE CASE");
    char b[40];
    snprintf(b, sizeof b, "FRAGMENTS %d/%d", found, NFRAG);
    mote_ftextc(mote, fb, f, 64, 19, rgb(170, 200, 250), b);
    int i = g_case_page;
    snprintf(b, sizeof b, "FRAGMENT %d", i + 1);
    mote_ftextc(mote, fb, f, 64, 44, rgb(210, 220, 245), b);
    if ((g_frags >> i) & 1) {
        mote_ftextc(mote, fb, f, 64, 60, rgb(250, 250, 255), k_frag[i][0]);
        mote_ftextc(mote, fb, f, 64, 72, rgb(250, 250, 255), k_frag[i][1]);
    } else {
        mote_ftextc(mote, fb, f, 64, 60, rgb(120, 130, 160), "SEALED");
        mote_ftextc(mote, fb, f, 64, 72, rgb(120, 130, 160), "WIN A DAY TO UNSEAL");
    }
    if (found == NFRAG) {
        mote_ftextc(mote, fb, f, 64, 90, rgb(250, 220, 110), "I LEFT MY MARK ON");
        mote_ftextc(mote, fb, f, 64, 101, rgb(250, 220, 110), "EVERY ROOM. AGAIN.");
    }
    /* page dots */
    for (int k = 0; k < NFRAG; k++)
        mote->draw_rect(fb, 40 + k * 6, 110, 4, 4,
                        k == g_case_page ? rgb(255, 230, 120)
                        : ((g_frags >> k) & 1) ? rgb(170, 200, 250) : rgb(50, 66, 110),
                        1, 0, 128);
    mote_ftextc(mote, fb, f, 64, 117, rgb(150, 165, 205), "< >  B BACK");
}

static void case_tick(void) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_LEFT))
        { g_case_page = (g_case_page + NFRAG - 1) % NFRAG; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_RIGHT))
        { g_case_page = (g_case_page + 1) % NFRAG; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_A)
        || mote_just_pressed(in, MOTE_BTN_MENU))
        g_state = GS_TITLE;
}

/* --------------------------------------------------------------- notebook --- */
static void notebook_draw(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    page_header(fb, "NOTEBOOK");
    char b[44];
    int y = 20;
    int shown = 0, solved = 0;
    for (int i = 0; i < PZ_N; i++) {
        if ((g_pz_solved >> i) & 1) { solved++; continue; }
        if (!((g_pz_clue >> i) & 1)) continue;
        char c[40];
        pz_clue_text(i, c, sizeof c);
        mote->draw_rect(fb, 6, y - 2, 116, 11, rgb(16, 26, 58), 1, 0, 128);
        mote->draw_rect(fb, 8, y + 1, 3, 3, rgb(250, 220, 130), 1, 0, 128);
        mote->text(fb, c, 15, y, rgb(220, 228, 250));
        y += 12;
        shown++;
    }
    if (g_census_room != 0xFF && !((g_pz_solved >> PZ_CENSUS) & 1)) {
        snprintf(b, sizeof b, "LEDGER: %s IN %s", k_census_nm[g_census_kind],
                 k_rooms[g_census_room].name);
        mote->draw_rect(fb, 6, y - 2, 116, 11, rgb(16, 26, 58), 1, 0, 128);
        mote->draw_rect(fb, 8, y + 1, 3, 3, rgb(250, 220, 130), 1, 0, 128);
        mote->text(fb, b, 15, y, rgb(220, 228, 250));
        y += 12;
        shown++;
    }
    if (!shown) {
        mote->text(fb, "CRUMPLED NOTES HOLD CLUES.", 8, 44, rgb(150, 165, 205));
        mote->text(fb, "PUZZLES WAIT IN THE ROOMS.", 8, 54, rgb(150, 165, 205));
    }
    snprintf(b, sizeof b, "PUZZLES SOLVED %d/%d", solved, PZ_N);
    mote_ftextc(mote, fb, f, 64, 114, rgb(190, 205, 240), b);
}

/* ---------------------------------------------------------------- satchel --- */
static const struct { const char *name, *desc; } k_tool_info[6] = {
    { "MASTER KEY", "OPENS EVERY LOCK" },
    { "COMPASS",    "LB/RB TURNS OFFERS" },
    { "SPYGLASS",   "OFFERS DEAL 4 CARDS" },
    { "PENCIL",     "B REDRAWS FOR 1 GEM" },
    { "KEYCARD",    "OPENS EVERY READER" },
    { "SPADE",      "DIGS SPARKLING SPOTS" },
};
static const uint8_t k_tool_cell[6] = { 6, 10, 11, 13, 15, 16 };

static void satchel_draw(uint16_t *fb) {
    paper(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    page_header(fb, "SATCHEL");
    char b[40];
    uint8_t held[6] = { g_master, g_compass, g_spyglass, g_pencil, g_keycard, g_spade };
    /* the toolkit, one slot per tool; the cursor reads each one */
    for (int i = 0; i < 6; i++) {
        int x = 7 + i * 20, y = 18;
        mote->draw_rect(fb, x, y, 18, 18, rgb(16, 26, 58), 1, 0, 128);
        mote->draw_rect(fb, x, y, 18, 18,
                        i == g_item_cur ? rgb(255, 230, 120) : rgb(52, 74, 130), 0, 0, 128);
        if (held[i])
            mote->blit(fb, &items_img, x + 3, y + 3, k_tool_cell[i] * 12, 0, 12, 12, 0, 0, 128);
        else
            mote_ftextc(mote, fb, f, x + 9, y + 4, rgb(60, 76, 122), "?");
    }
    if (held[g_item_cur]) {
        mote_ftextc(mote, fb, f, 64, 41, rgb(240, 240, 250), k_tool_info[g_item_cur].name);
        mote->text(fb, k_tool_info[g_item_cur].desc,
                   64 - (int)strlen(k_tool_info[g_item_cur].desc) * 3, 54, rgb(190, 205, 240));
    } else {
        mote_ftextc(mote, fb, f, 64, 41, rgb(120, 135, 175), k_tool_info[g_item_cur].name);
        mote->text(fb, "NOT FOUND YET", 64 - 39, 54, rgb(120, 135, 175));
    }
    mote->draw_rect(fb, 6, 59, 116, 1, rgb(44, 66, 124), 1, 0, 128);
    /* the day: number, condition, score */
    snprintf(b, sizeof b, "DAY %d", g_days + 1);
    mote->text_font(fb, f, b, 8, 63, rgb(200, 210, 240));
    snprintf(b, sizeof b, "SCORE %u", (unsigned)g_score);
    mote->text_font(fb, f, b, 70, 63, rgb(250, 240, 190));
    snprintf(b, sizeof b, "%s: %s", k_conds[g_cond].name, k_conds[g_cond].desc);
    mote->text(fb, b, 8, 76, rgb(170, 200, 250));
    /* goals */
    for (int i = 0; i < 2; i++) {
        const GoalDef *gd = &k_goals[g_goal[i]];
        int y = 86 + i * 9;
        if (g_goal_done[i]) {
            snprintf(b, sizeof b, "%s DONE +%d", gd->name, gd->pts);
            mote->text(fb, b, 8, y, rgb(250, 220, 110));
        } else {
            snprintf(b, sizeof b, "%s %d/%d", gd->name,
                     g_goal_prog[i] > gd->target ? gd->target : g_goal_prog[i], gd->target);
            mote->text(fb, b, 8, y, rgb(190, 205, 240));
            snprintf(b, sizeof b, "+%d", gd->pts);
            mote->text(fb, b, 100, y, rgb(150, 165, 205));
        }
    }
    /* ranks + seals, right-hand column beside the goals */
    {
        int ranks = 0;
        for (int r = 0; r < GRID_H; r++) {
            int full = 1;
            for (int c = 0; c < GRID_W; c++)
                if (g_grid[r * GRID_W + c].room == 0xFF) full = 0;
            ranks += full;
        }
        snprintf(b, sizeof b, "RANKS %d", ranks);
        mote->text(fb, b, 8, 104, rgb(190, 205, 240));
        if (g_seal_day) {
            mote->text(fb, "SEALS", 60, 104, rgb(250, 220, 130));
            for (int k = 0; k < 3; k++)
                mote->draw_rect(fb, 96 + k * 8, 104, 6, 6,
                                (g_seal_thrown & (1 << k)) ? rgb(255, 230, 120) : rgb(50, 66, 110),
                                1, 0, 128);
        }
    }
    /* resources along the bottom */
    {
        static const uint8_t rc[4] = { 8, 1, 2, 0 };          /* steps, key, gem, gold */
        int vals[4] = { g_steps, g_keys, g_gems, g_gold };
        int x = 8;
        for (int i = 0; i < 4; i++) {
            mote->blit(fb, &items_img, x, 112, rc[i] * 12, 0, 12, 12, 0, 0, 128);
            snprintf(b, sizeof b, "%d", vals[i]);
            x = mote->text_font(fb, f, b, x + 13, 112, rgb(230, 235, 250)) + 8;
        }
    }
}

static void title_draw(uint16_t *fb) {
    /* the crowned-thumbprint mark (spiral whorl + chalk crown on blueprint) —
     * pre-drawn by assets/make_title.py; text goes over its calm bands */
    mote->blit(fb, &title_bg_img, 0, 0, 0, 0, 128, 128, 0, 0, 128);
    mote_ftextc(mote, fb, mote->ui_font(MOTE_FONT_LARGE), 64, 7, rgb(10, 16, 36), "THUMBPRINCE");
    mote_ftextc(mote, fb, mote->ui_font(MOTE_FONT_LARGE), 64, 6, rgb(250, 250, 255), "THUMBPRINCE");
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, f, 64, 21, rgb(10, 16, 36), "LEAVE YOUR MARK");
    mote_ftextc(mote, fb, f, 64, 20, rgb(160, 190, 245), "LEAVE YOUR MARK");
    char buf[40];
    if (((int)(g_result_t * 2) & 1) == 0) {
        mote_ftextc(mote, fb, f, 64, 105, rgb(10, 16, 36), "A - ENTER THE ESTATE");
        mote_ftextc(mote, fb, f, 64, 104, rgb(240, 240, 250), "A - ENTER THE ESTATE");
    }
    mote_ftextc(mote, fb, f, 64, 92, rgb(10, 16, 36), "B - THE CASE");
    mote_ftextc(mote, fb, f, 64, 91, rgb(170, 200, 250), "B - THE CASE");
    if (g_hi || g_wins) {                        /* one stats line in the bottom band */
        if (g_wins) snprintf(buf, sizeof buf, "BEST %u  WINS %u/%u",
                             (unsigned)g_hi, (unsigned)g_wins, (unsigned)g_days);
        else snprintf(buf, sizeof buf, "BEST %u", (unsigned)g_hi);
        mote_ftextc(mote, fb, f, 64, 119, rgb(10, 16, 36), buf);
        mote_ftextc(mote, fb, f, 64, 118, rgb(250, 240, 190), buf);
    }
}

static void results_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 8, 14, 112, 100, rgb(14, 16, 32), 1, 0, 128);
    mote->draw_rect(fb, 8, 14, 112, 100, g_won ? rgb(240, 205, 90) : rgb(120, 140, 200), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 17,
                g_won ? rgb(250, 220, 110) : rgb(230, 230, 245),
                g_won ? "EVIDENCE RECOVERED!" : "OUT OF STEPS");
    /* the case report: where the points came from */
    char buf[32];
    if (g_won && g_frag_new) {
        snprintf(buf, sizeof buf, "CASE FRAGMENT %d/%d", g_frag_new, NFRAG);
        mote->text(fb, buf, 34, 29, rgb(170, 200, 250));
    }
    static const char *cat[5] = { "ROOMS", "LOOT", "BONUSES", "GOALS", "THE WIN" };
    uint32_t vals[5] = { g_sc_rooms, g_sc_loot, g_sc_bonus, g_sc_goal, g_sc_win };
    int y = g_won && g_frag_new ? 38 : 31;
    for (int i = 0; i < 5; i++) {
        if (!vals[i]) continue;
        mote->text_font(fb, f, cat[i], 16, y, rgb(170, 185, 220));
        snprintf(buf, sizeof buf, "%u", (unsigned)vals[i]);
        mote->text_font(fb, f, buf, 82, y, rgb(220, 225, 245));
        y += 11;
    }
    snprintf(buf, sizeof buf, "SCORE %u", (unsigned)g_score);
    mote->text_font(fb, f, buf, 16, y + 3, rgb(250, 240, 190));
    if (g_new_best && ((int)(g_result_t * 3) & 1) == 0)
        mote->text_font(fb, f, "NEW BEST!", 16, y + 15, rgb(255, 120, 80));
    else if (!g_new_best) {
        snprintf(buf, sizeof buf, "BEST %u", (unsigned)g_hi);
        mote->text_font(fb, f, buf, 16, y + 15, rgb(150, 160, 190));
    }
    if (g_result_t > 1.0f)
        mote_ftextc(mote, fb, f, 64, 100, rgb(240, 240, 250), "A - NEW DAY");
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
    if (getenv("DRAFT_DIG")) g_force_dig = 1;
    if ((e = getenv("DRAFT_UI")) && *e == 'b') g_draft_ui = 1;
    if (getenv("DRAFT_SKIP")) day_start();
    mote->log("thumbprince up");
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
        if (mote_just_pressed(in, MOTE_BTN_B)) { g_case_page = 0; g_state = GS_CASE; }
        break;
    }
    case GS_CASE:
        case_tick();
        break;
    case GS_PLAY:
    case GS_DRAFT:
    case GS_SHOP:
    case GS_MAP:
    case GS_PAUSE:
    case GS_PUZZLE:
    case GS_SLOTS:
        if (st == GS_PLAY) player_tick(dt);
        else if (st == GS_DRAFT) draft_tick();
        else if (st == GS_SHOP) shop_tick();
        else if (st == GS_PAUSE) pause_tick();
        else if (st == GS_PUZZLE) puzzle_tick();
        else if (st == GS_SLOTS) slots_tick(dt);
        else {
            const MoteInput *in = mote->input();
            if (mote_just_pressed(in, MOTE_BTN_RB))
                { g_map_page = (uint8_t)((g_map_page + 1) % 3); mote->audio_play_sfx(&tick_sfx, 0.6f); }
            if (mote_just_pressed(in, MOTE_BTN_LB))
                { g_map_page = (uint8_t)((g_map_page + 2) % 3); mote->audio_play_sfx(&tick_sfx, 0.6f); }
            if (g_map_page == 0) {               /* browse the blueprint */
                int c = gi_col(g_map_cur), r = gi_row(g_map_cur);
                int moved = 0;
                if (mote_just_pressed(in, MOTE_BTN_LEFT) && c > 0)  { c--; moved = 1; }
                if (mote_just_pressed(in, MOTE_BTN_RIGHT) && c < GRID_W - 1) { c++; moved = 1; }
                if (mote_just_pressed(in, MOTE_BTN_UP) && r > 0)    { r--; moved = 1; }
                if (mote_just_pressed(in, MOTE_BTN_DOWN) && r < GRID_H - 1)  { r++; moved = 1; }
                if (moved) { g_map_cur = (uint8_t)(r * GRID_W + c); mote->audio_play_sfx(&tick_sfx, 0.5f); }
            } else if (g_map_page == 2) {        /* read the satchel */
                if (mote_just_pressed(in, MOTE_BTN_LEFT) && g_item_cur > 0)
                    { g_item_cur--; mote->audio_play_sfx(&tick_sfx, 0.5f); }
                if (mote_just_pressed(in, MOTE_BTN_RIGHT) && g_item_cur < 5)
                    { g_item_cur++; mote->audio_play_sfx(&tick_sfx, 0.5f); }
            }
            if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU))
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
    case GS_PUZZLE:  hud_draw(fb); puzzle_draw(fb); return;
    case GS_SLOTS:   hud_draw(fb); slots_draw(fb); return;
    case GS_CASE:    case_draw(fb); return;
    default:
        /* the Dark Room: a narrow ring of sight, pitch black past it */
        if (g_grid[g_cur].room == R_DARKROOM) {
            int pcx = ORG_X + (int)g_px, pcy = ORG_Y + (int)g_py;
            for (int y = ORG_Y; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    int dx = x - pcx, dy = y - pcy;
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= 24 * 24) continue;
                    uint16_t p = fb[y * 128 + x];
                    p = (uint16_t)((p >> 1) & 0x7BEF);
                    if (d2 > 34 * 34) p = (uint16_t)((p >> 2) & 0x39E7);
                    else if (d2 > 28 * 28) p = (uint16_t)((p >> 1) & 0x7BEF);
                    fb[y * 128 + x] = p;
                }
        }
        /* unsolved sequence puzzles wear their markings */
        {
            int room = g_grid[g_cur].room;
            char nb[2] = { 0, 0 };
            if (room == R_CHAPEL && !(g_pz_solved & (1 << PZ_CANDLES))) {
                int nth = 0;
                for (int i = 0; i < g_nprops; i++) {
                    if (g_props_cur[i].prop != P_CANDLE) continue;
                    int x = ORG_X + g_props_cur[i].x, y = ORG_Y + g_props_cur[i].y;
                    if (g_seq_lit & (1 << nth))
                        mote->draw_rect(fb, x + 1, y - 3, 6, 6, rgb(255, 220, 110), 0, 0, 128);
                    nb[0] = (char)('1' + nth);
                    mote->text(fb, nb, x + 2, y - 11, rgb(250, 240, 190));
                    nth++;
                }
            }
            if (room == R_ROTUNDA && !(g_pz_solved & (1 << PZ_STATUES))) {
                int nth = 0;
                for (int i = 0; i < g_nprops; i++) {
                    if (g_props_cur[i].prop != P_STATUE) continue;
                    int x = ORG_X + g_props_cur[i].x, y = ORG_Y + g_props_cur[i].y;
                    mote->text(fb, k_dirname[g_statue_face[nth]], x + 4, y - 8, rgb(190, 205, 240));
                    nth++;
                }
            }
            if (room == R_GREATHALL && !(g_pz_solved & (1 << PZ_TILES))) {
                int nth = 0;
                for (int i = 0; i < g_nprops; i++) {
                    if (g_props_cur[i].prop != P_PLATE) continue;
                    float dx = g_props_cur[i].x + 6 - 56, dy = g_props_cur[i].y + 5 - 56;
                    int dir = (dx * dx > dy * dy) ? (dx > 0 ? DIR_E : DIR_W)
                                                  : (dy > 0 ? DIR_S : DIR_N);
                    int x = ORG_X + g_props_cur[i].x, y = ORG_Y + g_props_cur[i].y;
                    if (g_seq_lit & (1 << nth))
                        mote->draw_rect(fb, x, y - 1, 12, 12, rgb(255, 220, 110), 0, 0, 128);
                    mote->text(fb, k_dirname[dir], x + 4, y + 2,
                               (g_seq_lit & (1 << nth)) ? rgb(255, 236, 140) : rgb(220, 225, 245));
                    nth++;
                }
            }
        }
        /* dig spot: a pulsing twinkle on the floor */
        if (g_dig_on) {
            int ph = (int)(g_result_t * 5) % 3;
            int x = ORG_X + g_dig_x, y = ORG_Y + g_dig_y;
            uint16_t c = ph == 0 ? rgb(255, 250, 200) : ph == 1 ? rgb(240, 210, 110) : rgb(180, 150, 80);
            mote->draw_rect(fb, x - 1 - ph, y, 3 + 2 * ph, 1, c, 1, 0, 128);
            mote->draw_rect(fb, x, y - 1 - ph, 1, 3 + 2 * ph, c, 1, 0, 128);
        }
        hud_draw(fb); toast_draw(fb); return;
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 128 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("ThumbPrince", "austinio7116");
MOTE_GAME_VERSION("1.2.0");
