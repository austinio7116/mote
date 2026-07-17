/* DraftMote — a room-drafting roguelike inspired by Blue Prince's estate
 * puzzle: draft rooms onto a 5x8 grid, walk them top-down, manage steps,
 * keys, gems and gold, and reach the Antechamber before you're exhausted.
 * See DESIGN.md. */
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
#include "tiles.h"
#include "doors.h"
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
#define ROOM_W 8
#define ROOM_H 7
#define TILE 16
#define ROOM_Y0 16                          /* HUD strip height */
#define START_STEPS 50

typedef struct {
    uint8_t room;        /* 0xFF = undrafted */
    uint8_t doors;       /* absolute NESW bits */
    uint8_t lock_roll;   /* sides whose lock has been rolled */
    uint8_t lock_on;     /* sides currently locked */
    uint8_t looted;      /* loot slot bits */
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
static uint8_t g_won;
static int    g_rooms_placed;

static uint16_t g_hi;
static uint16_t g_days, g_wins;
static uint8_t g_new_best;

/* draft offer */
static int g_draft_gi, g_draft_entry, g_draft_sel;
static uint8_t g_cards[3];

/* shop */
static int g_shop_sel, g_shop_kind;         /* 0 commissary, 1 locksmith */

static int g_pause_sel;
static float g_result_t;

static char  g_toast[40];
static float g_toast_t;

static uint8_t g_no_locks;                  /* DRAFT_LOCKS=0 test hook */
static uint8_t g_all_locks;                 /* DRAFT_LOCKS=2 test hook */
static const char *g_force_rooms;           /* DRAFT_ROOMS test hook */

static void toast(const char *msg) { snprintf(g_toast, sizeof g_toast, "%s", msg); g_toast_t = 2.2f; }
static void toastf(const char *fmt, int v) { snprintf(g_toast, sizeof g_toast, fmt, v); g_toast_t = 2.2f; }

/* room blurbs for draft cards */
static const char *k_blurb[R_COUNT] = {
    [R_HALLWAY] = "CORRIDOR",       [R_WPASS] = "CORRIDOR",
    [R_EPASS] = "CORRIDOR",         [R_DINING] = "+4 STEPS",
    [R_KITCHEN] = "+4 STEPS, FOOD", [R_BEDROOM] = "+6 STEPS",
    [R_CLOSET] = "A KEY INSIDE",    [R_PANTRY] = "+4 STEPS, FOOD",
    [R_TERRACE] = "GREEN, +1 GEM",  [R_GARDEN] = "GREEN, +1 GEM",
    [R_FOYER] = "3 DOORS",          [R_STUDY] = "+50 PTS, +1 GEM",
    [R_CHAPEL] = "BIG STAR",        [R_STORE] = "COINS + STAR",
    [R_GEMDEN] = "+3 GEMS",         [R_SECURITY] = "2 KEYS",
    [R_COMMISSARY] = "SHOP",        [R_LOCKSMITH] = "KEY SHOP",
    [R_CONSERV] = "GREEN, +2 GEMS", [R_BALLROOM] = "+60 PTS, COINS",
    [R_SUITE] = "+12 STEPS, KEY",   [R_VAULT] = "GOLD HOARD",
    [R_OBSERV] = "HUGE STAR",       [R_GREATHALL] = "4 DOORS, +50",
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

static uint8_t shape_mask(uint8_t shape, int entry) {
    int fwd = entry ^ 2, left = (fwd + 3) & 3, right = (fwd + 1) & 3;
    switch (shape) {
    case SH_DEAD: return DBIT(entry);
    case SH_STR:  return DBIT(entry) | DBIT(fwd);
    case SH_L:    return DBIT(entry) | DBIT(left);
    case SH_R:    return DBIT(entry) | DBIT(right);
    case SH_T:    return DBIT(entry) | DBIT(left) | DBIT(right);
    default:      return 0xF;
    }
}

/* door tile positions per side, in room tiles */
static const int8_t k_doorpos[4][2] = { { 3, 0 }, { 7, 3 }, { 3, 6 }, { 0, 3 } };

/* door visual/interaction state */
enum { DS_NONE, DS_WINDOW, DS_BRICK, DS_CLOSED, DS_OPEN, DS_LOCKED, DS_GOLD };

static int door_state(int gi, int side) {
    const Cell *cl = &g_grid[gi];
    if (!(cl->doors & DBIT(side))) return DS_NONE;
    int n = neighbor(gi, side);
    if (n < 0) return DS_WINDOW;
    const Cell *nc = &g_grid[n];
    if (nc->room != 0xFF) {
        if (!(nc->doors & DBIT(side ^ 2))) return DS_BRICK;
        if (nc->room == R_ANTE && !g_won) return DS_GOLD;
        if ((cl->lock_on & DBIT(side)) || (nc->lock_on & DBIT(side ^ 2))) return DS_LOCKED;
        return DS_OPEN;
    }
    if ((cl->lock_roll & DBIT(side)) && (cl->lock_on & DBIT(side))) return DS_LOCKED;
    return DS_CLOSED;
}

/* -------------------------------------------------------------- room tiles --- */
static char tmpl_at(int gi, int tx, int ty) {
    return k_rooms[g_grid[gi].room].tmpl[ty * ROOM_W + tx];
}

/* prop char -> tiles.png cell (col + row*8), or -1 */
static int prop_cell(char ch, int *solid) {
    *solid = 1;
    switch (ch) {
    case 'c': return 0 + 2 * 8;
    case 'C': return 1 + 2 * 8;
    case 't': return 2 + 2 * 8;
    case 'h': return 3 + 2 * 8;
    case 'b': return 4 + 2 * 8;
    case 'B': return 5 + 2 * 8;
    case 'p': return 6 + 2 * 8;
    case 'x': return 7 + 2 * 8;
    case 'f': return 0 + 3 * 8;
    case 'a': return 1 + 3 * 8;
    case 'o': return 2 + 3 * 8;
    case 's': return 3 + 3 * 8;
    case 'r': return 4 + 3 * 8;
    case 'g': return 5 + 3 * 8;
    case 'R': *solid = 0; return 6 + 3 * 8;
    case 'A': return 7 + 3 * 8;
    }
    return -1;
}

/* which door side (or -1) owns tile (tx,ty) */
static int door_side_at(int tx, int ty) {
    for (int s = 0; s < 4; s++)
        if (k_doorpos[s][0] == tx && k_doorpos[s][1] == ty) return s;
    return -1;
}

/* is tile solid for movement? (doors are always solid: bumping them acts) */
static int tile_solid(int tx, int ty) {
    if (tx < 0 || tx >= ROOM_W || ty < 0 || ty >= ROOM_H) return 1;
    char ch = tmpl_at(g_cur, tx, ty);
    if (ch == '#') {
        int s = door_side_at(tx, ty);
        return s < 0 || 1;                 /* walls + doors both block; bump handles doors */
    }
    if (ch == '.' || (ch >= '0' && ch <= '5')) return 0;
    if (ch == 'K' || ch == 'S') return 1;
    int solid, cell = prop_cell(ch, &solid);
    return cell >= 0 ? solid : 0;
}

/* --------------------------------------------------------------- economy ---- */
static void give_steps(int n) { g_steps += n; if (n > 0) toastf("+%d STEPS", n); }

static void apply_entry_effects(int gi) {
    Cell *cl = &g_grid[gi];
    if (cl->entered) return;
    cl->entered = 1;
    const RoomDef *d = &k_rooms[cl->room];
    if (d->steps) { g_steps += d->steps; mote->audio_play_sfx(&food_sfx, 0.7f); toastf("+%d STEPS", d->steps); }
    if (d->keys)  { g_keys += d->keys; mote->audio_play_sfx(&key_sfx, 0.9f); toastf("+%d KEYS", d->keys); }
    if (d->gemsg) { g_gems += d->gemsg; mote->audio_play_sfx(&gem_sfx, 0.9f); toastf("+%d GEMS", d->gemsg); }
    if (d->pts)   { g_score += d->pts; mote->audio_play_sfx(&star_sfx, 0.9f); toastf("+%d PTS", d->pts); }
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
    mote->audio_play_sfx(&door_sfx, 0.8f);
    switch (entry_side) {
    case DIR_N: g_px = 3 * TILE + 8; g_py = 1 * TILE + 8; break;
    case DIR_S: g_px = 3 * TILE + 8; g_py = 5 * TILE + 8; break;
    case DIR_E: g_px = 6 * TILE + 8; g_py = 3 * TILE + 8; break;
    default:    g_px = 1 * TILE + 8; g_py = 3 * TILE + 8; break;
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

static uint8_t pick_room(uint8_t exclude0, uint8_t exclude1) {
    int total = 0;
    for (uint8_t i = DRAFT_FIRST; i < R_COUNT; i++) {
        if (i == exclude0 || i == exclude1) continue;
        if ((k_rooms[i].flags & RF_UNIQUE) && room_placed(i)) continue;
        total += k_rarity_weight[k_rooms[i].rarity];
    }
    int roll = (int)(mote_rand() % (uint32_t)total);
    for (uint8_t i = DRAFT_FIRST; i < R_COUNT; i++) {
        if (i == exclude0 || i == exclude1) continue;
        if ((k_rooms[i].flags & RF_UNIQUE) && room_placed(i)) continue;
        roll -= k_rarity_weight[k_rooms[i].rarity];
        if (roll < 0) return i;
    }
    return R_HALLWAY;
}

static void deal_cards(void) {
    if (g_force_rooms) {                              /* DRAFT_ROOMS=a,b,c (every offer) */
        int a, b, c;
        if (sscanf(g_force_rooms, "%d,%d,%d", &a, &b, &c) == 3) {
            g_cards[0] = (uint8_t)a; g_cards[1] = (uint8_t)b; g_cards[2] = (uint8_t)c;
            return;
        }
        g_force_rooms = 0;
    }
    g_cards[0] = pick_room(0xFF, 0xFF);
    g_cards[1] = pick_room(g_cards[0], 0xFF);
    g_cards[2] = pick_room(g_cards[0], g_cards[1]);
    /* guarantee one free, unlocked pick */
    int ok = 0;
    for (int i = 0; i < 3; i++)
        if (k_rooms[g_cards[i]].gems == 0 && !(k_rooms[g_cards[i]].flags & RF_LOCKED)) ok = 1;
    if (!ok) {
        static const uint8_t safe[4] = { R_HALLWAY, R_WPASS, R_EPASS, R_BEDROOM };
        g_cards[2] = safe[mote_rand() % 4];
    }
}

static int card_affordable(uint8_t id) {
    if (k_rooms[id].gems > g_gems) return 0;
    if ((k_rooms[id].flags & RF_LOCKED) && g_keys < 1 && !g_master) return 0;
    return 1;
}

static void place_card(uint8_t id) {
    const RoomDef *d = &k_rooms[id];
    g_gems -= d->gems;
    if (d->flags & RF_LOCKED) {
        if (g_master) toast("MASTER KEY OPENS THE VAULT");
        else { g_keys--; toast("USED A KEY"); }
    }
    Cell *cl = &g_grid[g_draft_gi];
    cl->room = id;
    cl->doors = shape_mask(d->shape, g_draft_entry);
    g_rooms_placed++;
    g_score += 10u * (d->rarity + 1);
    mote->audio_play_sfx(&draft_sfx, 0.9f);
    /* green adjacency bonus */
    if (d->flags & RF_GREEN) {
        int n = 0;
        for (int s = 0; s < 4; s++) {
            int ni = neighbor(g_draft_gi, s);
            if (ni >= 0 && g_grid[ni].room != 0xFF && (k_rooms[g_grid[ni].room].flags & RF_GREEN)) n++;
        }
        if (n) { g_score += 25u * n; toastf("GARDEN BONUS +%d", 25 * n); mote->audio_play_sfx(&star_sfx, 0.9f); }
    }
    /* rank complete bonus */
    int r = gi_row(g_draft_gi), full = 1;
    for (int c = 0; c < GRID_W; c++)
        if (g_grid[r * GRID_W + c].room == 0xFF) full = 0;
    if (full) { g_score += 100; toast("RANK COMPLETE +100"); mote->audio_play_sfx(&row_sfx, 1.0f); }
    g_state = GS_PLAY;
    enter_room(g_draft_gi, g_draft_entry);
}

/* bump into the door on `side` of the current room */
static void door_try(int side) {
    Cell *cl = &g_grid[g_cur];
    int st = door_state(g_cur, side);
    int n = neighbor(g_cur, side);
    if (st == DS_NONE || st == DS_WINDOW || st == DS_BRICK) return;

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
        int p = g_no_locks ? 0 : g_all_locks ? 100 : 8 + 5 * (7 - gi_row(n));
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

/* ------------------------------------------------------------------ pickups -- */
static void pickups_tick(void) {
    Cell *cl = &g_grid[g_cur];
    const RoomDef *d = &k_rooms[cl->room];
    int slots = 0, left = 0;
    for (int ty = 0; ty < ROOM_H; ty++)
        for (int tx = 0; tx < ROOM_W; tx++) {
            char ch = tmpl_at(g_cur, tx, ty);
            if (ch < '0' || ch > '5') continue;
            int slot = ch - '0';
            if (d->loot[slot] == IT_NONE) continue;
            slots++;
            if (cl->looted & (1 << slot)) continue;
            /* overlap: item is a 12x12 at tile+2 */
            float ix = tx * TILE + 8, iy = ty * TILE + 8;
            if (g_px - 5 < ix + 6 && g_px + 5 > ix - 6 && g_py - 5 < iy + 6 && g_py + 5 > iy - 6) {
                cl->looted |= (uint8_t)(1 << slot);
                switch (d->loot[slot]) {
                case IT_COIN: g_gold++; g_score += 5; mote->audio_play_sfx(&coin_sfx, 0.8f); break;
                case IT_KEY:  g_keys++; mote->audio_play_sfx(&key_sfx, 0.9f); toast("A KEY!"); break;
                case IT_GEM:  g_gems++; mote->audio_play_sfx(&gem_sfx, 0.9f); break;
                case IT_FOOD: g_steps += 6; mote->audio_play_sfx(&food_sfx, 0.8f); toast("+6 STEPS"); break;
                case IT_STAR: g_score += 25; mote->audio_play_sfx(&star_sfx, 0.9f); toast("+25"); break;
                case IT_STAR2: g_score += 75; mote->audio_play_sfx(&star_sfx, 1.0f); toast("+75!"); break;
                case IT_STAR3: g_score += 100; mote->audio_play_sfx(&star_sfx, 1.0f); toast("+100!"); break;
                }
            } else if (!(cl->looted & (1 << slot))) left++;
        }
    /* swept bonus: every slot collected */
    if (slots && !cl->swept) {
        int done = 1;
        for (int s = 0; s < 6; s++)
            if (d->loot[s] != IT_NONE && !(cl->looted & (1 << s))) done = 0;
        if (done) { cl->swept = 1; g_score += 20; toast("ROOM SWEPT +20"); }
    }
    (void)left;
}

/* ------------------------------------------------------------------- shops --- */
typedef struct { const char *name; int price; } ShopItem;
static const ShopItem k_shop_com[3]  = { { "KEY", 8 }, { "GEM", 5 }, { "SNACK +8", 6 } };
static const ShopItem k_shop_lock[2] = { { "KEY", 5 }, { "MASTER KEY", 30 } };

static void shop_buy(void) {
    const ShopItem *it = g_shop_kind ? &k_shop_lock[g_shop_sel] : &k_shop_com[g_shop_sel];
    if (g_shop_kind && g_shop_sel == 1 && g_master) { toast("ALREADY OWNED"); return; }
    if (g_gold < it->price) { toast("NOT ENOUGH GOLD"); mote->audio_play_sfx(&locked_sfx, 0.8f); return; }
    g_gold -= it->price;
    mote->audio_play_sfx(&buy_sfx, 0.9f);
    if (!g_shop_kind) {
        if (g_shop_sel == 0) { g_keys++; toast("BOUGHT A KEY"); }
        else if (g_shop_sel == 1) { g_gems++; toast("BOUGHT A GEM"); }
        else { g_steps += 8; toast("+8 STEPS"); }
    } else {
        if (g_shop_sel == 0) { g_keys++; toast("BOUGHT A KEY"); }
        else { g_master = 1; toast("THE MASTER KEY!"); }
    }
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
    g_px = 3 * TILE + 8; g_py = 3 * TILE + 8;
    g_face = DIR_N;
    g_steps = START_STEPS; g_keys = 1; g_gems = 2; g_gold = 0;
    g_score = 0; g_master = 0; g_won = 0; g_rooms_placed = 0;
    g_new_best = 0;
    g_toast_t = 0;

    const char *e;
    if ((e = getenv("DRAFT_GIVE"))) {          /* keys:gems:gold:steps */
        int k, gm, go, st;
        if (sscanf(e, "%d:%d:%d:%d", &k, &gm, &go, &st) == 4) {
            g_keys = k; g_gems = gm; g_gold = go; g_steps = st;
        }
    }
    g_state = GS_PLAY;
    toast("FIND THE ANTECHAMBER");
}

/* ------------------------------------------------------------------ movement -- */
static int box_solid(float x, float y) {
    int x0 = (int)(x - 4) / TILE, x1 = (int)(x + 4) / TILE;
    int y0 = (int)(y - 4) / TILE, y1 = (int)(y + 4) / TILE;
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++)
            if (tile_solid(tx, ty)) return 1;
    return 0;
}

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
            /* clamp flush to the blocking tile, note the bump */
            float step = dx > 0 ? 0.5f : -0.5f;
            while (!box_solid(g_px + step, g_py)) g_px += step;
            int tx = (int)(g_px + (dx > 0 ? 6 : -6)) / TILE, ty = (int)g_py / TILE;
            int s = door_side_at(tx, ty);
            if (s >= 0) bumped_side = s;
        }
    }
    if (dy != 0) {
        if (!box_solid(g_px, g_py + dy)) g_py += dy;
        else {
            float step = dy > 0 ? 0.5f : -0.5f;
            while (!box_solid(g_px, g_py + step)) g_py += step;
            int tx = (int)g_px / TILE, ty = (int)(g_py + (dy > 0 ? 6 : -6)) / TILE;
            int s = door_side_at(tx, ty);
            if (s >= 0) bumped_side = s;
        }
    }
    g_bump_cd -= dt;
    if (bumped_side >= 0 && g_bump_cd <= 0) {
        g_bump_cd = 0.35f;
        door_try(bumped_side);
    }
    pickups_tick();

    if (mote_just_pressed(in, MOTE_BTN_RB)) { g_state = GS_MAP; mote->audio_play_sfx(&tick_sfx, 0.7f); }
    if (mote_just_pressed(in, MOTE_BTN_MENU)) { g_pause_sel = 0; g_state = GS_PAUSE; }

    /* shop counters: bump a K tile with A */
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        const RoomDef *d = &k_rooms[g_grid[g_cur].room];
        if (d->flags & (RF_SHOP_COM | RF_SHOP_LOCK)) {
            int fx = (int)(g_px + (g_face == DIR_E ? 12 : g_face == DIR_W ? -12 : 0)) / TILE;
            int fy = (int)(g_py + (g_face == DIR_S ? 12 : g_face == DIR_N ? -12 : 0)) / TILE;
            if (fx >= 0 && fx < ROOM_W && fy >= 0 && fy < ROOM_H && tmpl_at(g_cur, fx, fy) == 'K') {
                g_shop_kind = (d->flags & RF_SHOP_LOCK) ? 1 : 0;
                g_shop_sel = 0;
                g_state = GS_SHOP;
                mote->audio_play_sfx(&tick_sfx, 0.8f);
            }
        }
    }
}

/* ------------------------------------------------------------------ drawing --- */
static void add_spr(int cell16, int x, int y, int layer, uint8_t flags) {
    MoteSprite s = { .img = &tiles_img,
                     .x = (int16_t)x, .y = (int16_t)y,
                     .fx = (uint16_t)((cell16 & 7) * TILE), .fy = (uint16_t)((cell16 >> 3) * TILE),
                     .fw = TILE, .fh = TILE, .layer = (uint8_t)layer, .flags = flags };
    mote->scene2d_add(&s);
}

static void add_door_spr(int cell, int x, int y, uint8_t flags) {
    MoteSprite s = { .img = &doors_img,
                     .x = (int16_t)x, .y = (int16_t)y,
                     .fx = (uint16_t)(cell * TILE), .fy = 0,
                     .fw = TILE, .fh = TILE, .layer = 1, .flags = flags };
    mote->scene2d_add(&s);
}

static void add_item_spr(int cell, int x, int y) {
    MoteSprite s = { .img = &items_img,
                     .x = (int16_t)x, .y = (int16_t)y,
                     .fx = (uint16_t)(cell * 12), .fy = 0,
                     .fw = 12, .fh = 12, .layer = 3, .flags = 0 };
    mote->scene2d_add(&s);
}

static void room_draw(void) {
    const Cell *cl = &g_grid[g_cur];
    const RoomDef *d = &k_rooms[cl->room];
    int floor_cell = d->floor;             /* row 0 */
    int wall_cell = 8 + d->wall;           /* row 1 */

    for (int ty = 0; ty < ROOM_H; ty++)
        for (int tx = 0; tx < ROOM_W; tx++) {
            char ch = d->tmpl[ty * ROOM_W + tx];
            int x = tx * TILE, y = ty * TILE;
            if (ch == '#') {
                int s = door_side_at(tx, ty);
                int st = s >= 0 ? door_state(g_cur, s) : DS_NONE;
                switch (st) {
                case DS_NONE:   add_spr(wall_cell, x, y, 1, 0); break;
                case DS_WINDOW: add_spr(8 + 2, x, y, 1, 0); break;
                case DS_BRICK:  add_spr(8 + 3, x, y, 1, 0); break;
                default: {
                    int vert = (s == DIR_E || s == DIR_W);
                    int cell; uint8_t fl = 0;
                    if (st == DS_GOLD) { cell = 4; if (vert) fl = MOTE_SPR_ROT90; }
                    else if (st == DS_OPEN) cell = vert ? 3 : 1;
                    else cell = vert ? 2 : 0;
                    if (vert && s == DIR_W && st != DS_GOLD) fl = MOTE_SPR_HFLIP;
                    add_door_spr(cell, x, y, fl);
                    if (st == DS_LOCKED) add_item_spr(7, x + 2, y + 2);
                    break;
                }
                }
                continue;
            }
            if (ch == 'S') { add_spr(8 + 7, x, y, 1, 0); continue; }
            if (ch == 'K') { add_spr(8 + 6, x, y, 1, 0); continue; }
            add_spr(floor_cell, x, y, 0, 0);
            if (ch >= '0' && ch <= '5') {
                int slot = ch - '0';
                uint8_t it = d->loot[slot];
                if (it != IT_NONE && !(cl->looted & (1 << slot)))
                    add_item_spr(it - 1, x + 2, y + 2);
                continue;
            }
            int solid, pc = prop_cell(ch, &solid);
            if (pc >= 0) add_spr(pc, x, y, 2, 0);
        }

    /* player */
    int frame = g_moving ? ((int)(g_walk_t * 6.0f) & 1) : 0;
    int cell; uint8_t fl = 0;
    if (g_face == DIR_S) cell = 0 + frame;
    else if (g_face == DIR_N) cell = 2 + frame;
    else { cell = 4 + frame; if (g_face == DIR_E) fl = MOTE_SPR_HFLIP; }
    MoteSprite s = { .img = &hero_img,
                     .x = (int16_t)(g_px - 8), .y = (int16_t)(g_py - 10),
                     .fx = (uint16_t)(cell * 16), .fy = 0, .fw = 16, .fh = 16,
                     .layer = 5, .flags = fl };
    mote->scene2d_add(&s);
}

/* ---------------------------------------------------------------- overlays --- */
static uint16_t rgb(int r, int g, int b) { return MOTE_RGB565(r, g, b); }

static void dim(uint16_t *fb) {
    for (int i = 0; i < 128 * 128; i++) fb[i] = (uint16_t)((fb[i] >> 1) & 0x7BEF);
}

static void hud_draw(uint16_t *fb) {
    mote->draw_rect(fb, 0, 0, 128, ROOM_Y0, rgb(16, 20, 40), 1, 0, 128);
    mote->draw_rect(fb, 0, ROOM_Y0 - 1, 128, 1, rgb(60, 80, 140), 1, 0, 128);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    char buf[16];
    /* steps (flash red when low) */
    uint16_t sc = g_steps <= 8 ? (((int)(g_walk_t * 4) & 1) ? rgb(255, 70, 60) : rgb(255, 160, 60))
                               : rgb(240, 240, 250);
    snprintf(buf, sizeof buf, "%d", g_steps);
    mote->blit(fb, &items_img, 2, 2, 8 * 12, 0, 12, 12, 0, 0, 128);   /* boot = steps */
    mote->text_font(fb, f, buf, 15, 2, sc);
    int x = 44;
    mote->blit(fb, &items_img, x, 2, 1 * 12, 0, 12, 12, 0, 0, 128);
    snprintf(buf, sizeof buf, "%d", g_keys);
    x = mote->text_font(fb, f, buf, x + 12, 2, rgb(240, 220, 140)) + 8;
    mote->blit(fb, &items_img, x, 2, 2 * 12, 0, 12, 12, 0, 0, 128);
    snprintf(buf, sizeof buf, "%d", g_gems);
    x = mote->text_font(fb, f, buf, x + 12, 2, rgb(140, 240, 220)) + 8;
    mote->blit(fb, &items_img, x, 2, 0 * 12, 0, 12, 12, 0, 0, 128);
    snprintf(buf, sizeof buf, "%d", g_gold);
    mote->text_font(fb, f, buf, x + 12, 2, rgb(250, 210, 110));
    if (g_master) mote->blit(fb, &items_img, 114, 2, 6 * 12, 0, 12, 12, 0, 0, 128);
}

static void toast_draw(uint16_t *fb) {
    if (g_toast_t <= 0) return;
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 0, 98, 128, 14, rgb(10, 12, 26), 1, 0, 128);
    mote_ftextc(mote, fb, f, 64, 99, rgb(250, 240, 190), g_toast);
}

/* small NESW cross showing a card's exits, centred at (cx,cy) */
static void door_diagram(uint16_t *fb, int cx, int cy, uint8_t mask, uint16_t col) {
    mote->draw_rect(fb, cx - 2, cy - 2, 5, 5, rgb(70, 80, 110), 1, 0, 128);
    if (mask & DBIT(DIR_N)) mote->draw_rect(fb, cx - 1, cy - 7, 3, 5, col, 1, 0, 128);
    if (mask & DBIT(DIR_S)) mote->draw_rect(fb, cx - 1, cy + 3, 3, 5, col, 1, 0, 128);
    if (mask & DBIT(DIR_E)) mote->draw_rect(fb, cx + 3, cy - 1, 5, 3, col, 1, 0, 128);
    if (mask & DBIT(DIR_W)) mote->draw_rect(fb, cx - 7, cy - 1, 5, 3, col, 1, 0, 128);
}

static const uint16_t k_rarity_col[3] = { 0x8410, 0x2E9F, 0xFD00 };  /* grey, blue, gold */

static void draft_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, f, 64, 2, rgb(240, 240, 255), "DRAFT A ROOM");
    for (int i = 0; i < 3; i++) {
        const RoomDef *d = &k_rooms[g_cards[i]];
        int y = 17 + i * 31;
        int sel = i == g_draft_sel;
        int afford = card_affordable(g_cards[i]);
        mote->draw_rect(fb, 4, y, 120, 29, sel ? rgb(40, 50, 90) : rgb(20, 24, 46), 1, 0, 128);
        if (sel) mote->draw_rect(fb, 4, y, 120, 29, rgb(150, 180, 255), 0, 0, 128);
        mote->draw_rect(fb, 4, y, 3, 29, k_rarity_col[d->rarity], 1, 0, 128);
        uint16_t nc = afford ? rgb(250, 250, 255) : rgb(120, 120, 130);
        mote->text_font(fb, f, d->name, 10, y + 2, nc);
        if (k_blurb[g_cards[i]])
            mote->text_font(fb, f, k_blurb[g_cards[i]], 10, y + 15, afford ? rgb(170, 190, 230) : rgb(100, 100, 110));
        /* cost icons, right-aligned */
        int cx = 110;
        for (int c = 0; c < d->gems; c++) { mote->blit(fb, &items_img, cx, y + 2, 2 * 12, 0, 12, 12, 0, 0, 128); cx -= 9; }
        if (d->flags & RF_LOCKED) mote->blit(fb, &items_img, cx, y + 2, 7 * 12, 0, 12, 12, 0, 0, 128);
        door_diagram(fb, 113, y + 19, shape_mask(d->shape, g_draft_entry), afford ? rgb(210, 220, 255) : rgb(110, 110, 120));
    }
    mote_ftextc(mote, fb, f, 64, 112, rgb(160, 170, 200),
                g_gems > 0 ? "A PLACE  B REROLL" : "A PLACE");
    char buf[24];
    snprintf(buf, sizeof buf, "%d", g_gems);
    mote->blit(fb, &items_img, 4, 2, 2 * 12, 0, 12, 12, 0, 0, 128);
    mote->text_font(fb, f, buf, 17, 2, rgb(140, 240, 220));
}

static void draft_tick(void) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_UP))   { g_draft_sel = (g_draft_sel + 2) % 3; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) { g_draft_sel = (g_draft_sel + 1) % 3; mote->audio_play_sfx(&tick_sfx, 0.6f); }
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (card_affordable(g_cards[g_draft_sel])) place_card(g_cards[g_draft_sel]);
        else { toast("CAN'T AFFORD THAT ROOM"); mote->audio_play_sfx(&locked_sfx, 0.8f); }
    }
    if (mote_just_pressed(in, MOTE_BTN_B)) {
        if (g_gems > 0) { g_gems--; deal_cards(); g_draft_sel = 0; mote->audio_play_sfx(&gem_sfx, 0.8f); }
        else { toast("NO GEMS TO REROLL"); mote->audio_play_sfx(&locked_sfx, 0.8f); }
    }
}

static void map_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    char buf[32];
    snprintf(buf, sizeof buf, "DAY %d", g_days + 1);
    mote->text_font(fb, f, buf, 4, 2, rgb(200, 210, 240));
    snprintf(buf, sizeof buf, "%u", (unsigned)g_score);
    mote->text_font(fb, f, buf, 90, 2, rgb(250, 240, 190));
    int ox = 34, oy = 17, cs = 12;
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++) {
            int gi = r * GRID_W + c, x = ox + c * cs, y = oy + r * cs;
            const Cell *cl = &g_grid[gi];
            mote->draw_rect(fb, x, y, cs - 1, cs - 1, rgb(24, 32, 60), 1, 0, 128);
            if (cl->room != 0xFF) {
                mote->draw_rect(fb, x + 1, y + 1, cs - 3, cs - 3, k_rooms[cl->room].map_col, 1, 0, 128);
                /* door pips */
                uint16_t pip = rgb(20, 20, 30);
                if (cl->doors & DBIT(DIR_N)) mote->draw_rect(fb, x + 4, y, 3, 2, pip, 1, 0, 128);
                if (cl->doors & DBIT(DIR_S)) mote->draw_rect(fb, x + 4, y + cs - 3, 3, 2, pip, 1, 0, 128);
                if (cl->doors & DBIT(DIR_W)) mote->draw_rect(fb, x, y + 4, 2, 3, pip, 1, 0, 128);
                if (cl->doors & DBIT(DIR_E)) mote->draw_rect(fb, x + cs - 3, y + 4, 2, 3, pip, 1, 0, 128);
            }
            if (gi == ANTE_GI)
                mote->blit(fb, &items_img, x, y, 5 * 12, 0, 12, 12, 0, 0, 128);
            if (gi == g_cur && ((int)(g_result_t * 3) & 1) == 0)
                mote->draw_rect(fb, x - 1, y - 1, cs + 1, cs + 1, rgb(255, 255, 255), 0, 0, 128);
        }
    mote_ftextc(mote, fb, f, 64, 116, rgb(220, 225, 245), k_rooms[g_grid[g_cur].room].name);
}

static void shop_draw(uint16_t *fb) {
    dim(fb);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    int n = g_shop_kind ? 2 : 3;
    mote->draw_rect(fb, 10, 28, 108, 26 + n * 15, rgb(16, 20, 40), 1, 0, 128);
    mote->draw_rect(fb, 10, 28, 108, 26 + n * 15, rgb(120, 140, 200), 0, 0, 128);
    mote_ftextc(mote, fb, f, 64, 30, rgb(250, 230, 150), g_shop_kind ? "LOCKSMITH" : "COMMISSARY");
    char buf[32];
    for (int i = 0; i < n; i++) {
        const ShopItem *it = g_shop_kind ? &k_shop_lock[i] : &k_shop_com[i];
        int y = 45 + i * 15;
        if (i == g_shop_sel) mote->draw_rect(fb, 12, y - 1, 104, 14, rgb(40, 50, 90), 1, 0, 128);
        int sold = g_shop_kind && i == 1 && g_master;
        snprintf(buf, sizeof buf, "%s", sold ? "SOLD OUT" : it->name);
        mote->text_font(fb, f, buf, 16, y, g_gold >= it->price && !sold ? rgb(240, 240, 250) : rgb(120, 120, 130));
        if (!sold) {
            snprintf(buf, sizeof buf, "%d", it->price);
            mote->blit(fb, &items_img, 92, y, 0, 0, 12, 12, 0, 0, 128);
            mote->text_font(fb, f, buf, 105, y, rgb(250, 210, 110));
        }
    }
    snprintf(buf, sizeof buf, "GOLD %d", g_gold);
    mote_ftextc(mote, fb, f, 64, 47 + n * 15, rgb(250, 210, 110), buf);
}

static void shop_tick(void) {
    const MoteInput *in = mote->input();
    int n = g_shop_kind ? 2 : 3;
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
    /* blueprint grid lines */
    for (int v = 0; v < 128; v += 16) {
        for (int y = 0; y < 128; y += 2) mote->draw_pixel(fb, v, y, rgb(30, 44, 90));
        for (int x = 0; x < 128; x += 2) mote->draw_pixel(fb, x, v, rgb(30, 44, 90));
    }
    mote_ftextc(mote, fb, mote->ui_font(MOTE_FONT_LARGE), 64, 18, rgb(250, 250, 255), "DRAFTMOTE");
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, f, 64, 38, rgb(150, 180, 240), "DRAFT THE ESTATE");
    /* gold door + hero vignette */
    mote->blit(fb, &doors_img, 48, 56, 4 * 16, 0, 16, 16, 0, 0, 128);
    mote->blit(fb, &hero_img, 66, 56, 0, 0, 16, 16, 0, 0, 128);
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
    uint32_t s = (uint32_t)mote->micros();
    s ^= s >> 16; s *= 0x7feb352dU; s ^= s >> 15;
    const char *e;
    if ((e = getenv("DRAFT_SEED"))) s = (uint32_t)strtoul(e, 0, 10);
    mote_rand_seed(s ? s : 1u);
    if ((e = getenv("DRAFT_LOCKS"))) {
        if (*e == '0') g_no_locks = 1;
        else if (*e == '2') g_all_locks = 1;
    }
    g_force_rooms = getenv("DRAFT_ROOMS");
    if (getenv("DRAFT_SKIP")) day_start();
    mote->log("draftmote up");
}

static void g_update(float dt) {
    g_toast_t -= dt;
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
        mote->scene2d_begin(0, -ROOM_Y0);
        room_draw();
        break;
    case GS_RESULTS: {
        const MoteInput *in = mote->input();
        if (g_result_t > 1.0f && mote_just_pressed(in, MOTE_BTN_A)) g_state = GS_TITLE;
        mote->scene2d_begin(0, -ROOM_Y0);
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
    default:         hud_draw(fb); toast_draw(fb); return;
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 128 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("DraftMote", "austinio7116");
MOTE_GAME_VERSION("0.1.0");
