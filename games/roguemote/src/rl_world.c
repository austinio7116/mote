/*
 * The overworld and its towns.
 *
 * Depth 0 is the surface: a generated continent you walk across, with a town
 * and several dungeon mouths. It is the connective tissue, not where the depth
 * lives -- but it is what makes the world feel like a place rather than a
 * stack of levels.
 *
 * The continent is regenerated from g_world_seed every time you surface rather
 * than kept resident, because the arena cannot hold an overworld AND a dungeon
 * level at once (DESIGN.md section 14).
 */
#include "rl.h"

uint32_t g_world_seed = 1;

/* The town's bounding box, so "am I in town?" is a rectangle test and not a
 * search over the whole map. */
static uint8_t s_town_x, s_town_y;              /* centre */
static uint8_t s_town_x0, s_town_y0, s_town_x1, s_town_y1;
static uint8_t s_shop_x[SHOP_N], s_shop_y[SHOP_N];

int rl_in_town(void) {
    return g_pl.depth == 0 &&
           g_pl.x >= s_town_x0 && g_pl.x <= s_town_x1 &&
           g_pl.y >= s_town_y0 && g_pl.y <= s_town_y1;
}

int rl_shop_at(int x, int y) {
    for (int i = 0; i < SHOP_N; i++)
        if (s_shop_x[i] == x && s_shop_y[i] == y) return i;
    return -1;
}

/* --- value noise -------------------------------------------------------- */
static int noise2(int x, int y, uint32_t salt) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + salt;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int)((h >> 16) & 0xFF);
}

/* bilinear-ish smoothing over a coarse lattice: cheap, and at 64x48 the
 * blockiness reads as terrain rather than as an artefact */
static int height_at(int x, int y) {
    int s = 6;
    int gx = x / s, gy = y / s, fx = x % s, fy = y % s;
    int a = noise2(gx,     gy,     g_world_seed);
    int b = noise2(gx + 1, gy,     g_world_seed);
    int c = noise2(gx,     gy + 1, g_world_seed);
    int d = noise2(gx + 1, gy + 1, g_world_seed);
    int top = a + (b - a) * fx / s;
    int bot = c + (d - c) * fx / s;
    int h = top + (bot - top) * fy / s;
    /* a second octave for coastline detail */
    h = (h * 3 + noise2(x / 2, y / 2, g_world_seed ^ 0x5bf03635u)) / 4;
    return h;
}

/* Count 8-neighbours of `t`. Off-map counts as rock so the rim of the
 * continent stays solid instead of being eroded away by the smoothing pass. */
static int nbr(int x, int y, uint8_t t) {
    int n = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            if (!i && !j) continue;
            if (!rl_in(x + i, y + j)) { if (t == T_MOUNTAIN) n++; continue; }
            if (g_lv.terrain[(y + j) * MW + x + i] == t) n++;
        }
    return n;
}

/* Cellular smoothing. Raw value noise thresholded into bands gives a map
 * speckled with one-tile woods and one-tile crags, which reads as confetti
 * rather than terrain. Three majority passes turn the same noise into masses
 * with coherent edges -- which is what the blob47 rulesets are for: an
 * isolated mountain cell draws as the fully-bordered "island" cell, so a
 * scatter of them looks like dropped bricks, while a mass gets a real rim and
 * a terraced escarpment along its lower edge. */
static void smooth_overworld(void) {
    for (int pass = 0; pass < 3; pass++) {
        /* stage into layer[]: deciding against a half-updated map makes the
         * result depend on scan order, which shows up as a diagonal grain */
        for (int y = 0; y < MH; y++) {
            for (int x = 0; x < MW; x++) {
                uint8_t t = g_lv.terrain[y * MW + x], o = t;
                /* three neighbours, not two: at two, a diagonal chain of single
                 * cells survives, and that is exactly what reads as rubble */
                if (t == T_TREE     && nbr(x, y, T_TREE)     < 3) o = T_FLOOR;
                if (t == T_HILL     && nbr(x, y, T_HILL)     < 3) o = T_FLOOR;
                if (t == T_MOUNTAIN && nbr(x, y, T_MOUNTAIN) < 3) o = T_HILL;
                if (t == T_FLOOR) {
                    if      (nbr(x, y, T_MOUNTAIN) >= 5) o = T_MOUNTAIN;
                    else if (nbr(x, y, T_HILL)     >= 5) o = T_HILL;
                    else if (nbr(x, y, T_TREE)     >= 5) o = T_TREE;
                }
                g_lv.layer[y * MW + x] = o;
            }
        }
        for (int i = 0; i < MW * MH; i++) g_lv.terrain[i] = g_lv.layer[i];
    }

    /* A final erode-only pass. The growth rules above can themselves create a
     * lone cell on the last iteration, and a lone raised cell autotiles to the
     * fully-bordered "island" blob47 cell -- a little walled box sitting in a
     * meadow, which is the most conspicuous artefact this generator produces. */
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            uint8_t t = g_lv.terrain[y * MW + x], o = t;
            if (t == T_TREE     && nbr(x, y, T_TREE)     < 2) o = T_FLOOR;
            if (t == T_HILL     && nbr(x, y, T_HILL)     < 3) o = T_FLOOR;
            if (t == T_MOUNTAIN && nbr(x, y, T_MOUNTAIN) < 3) o = T_HILL;
            g_lv.layer[y * MW + x] = o;
        }
    }
    for (int i = 0; i < MW * MH; i++) g_lv.terrain[i] = g_lv.layer[i];
}

/* --- the town ------------------------------------------------------------
 *
 * Not a menu. A walled compound stamped straight into the overworld, the way
 * Moria's town is a place on the map rather than a screen you open: you walk in
 * through the gate, up the street, and into the shop you want.
 *
 * TW x TH tiles of brick wall around a cobbled cross of streets. The six shops
 * sit on the street front, three facing north and three facing south, so every
 * door is one step off a road. The inn is on the crossroads. Gates are simply
 * road tiles punched through the wall east and west, with a short approach road
 * run out into the grass so the town is visible as a destination from a
 * distance rather than as a wall you happen to bump into.
 *
 * The compound is levelled first -- forest and highland inside a town wall look
 * like the generator lost an argument -- and the ring immediately outside is
 * flattened to plain for the same reason. */
#define TW 15
#define TH 11

static void build_town(int cx, int cy) {
    int x0 = cx - TW / 2, y0 = cy - TH / 2;
    if (x0 < 2) x0 = 2;
    if (y0 < 2) y0 = 2;
    if (x0 + TW > MW - 2) x0 = MW - 2 - TW;
    if (y0 + TH > MH - 2) y0 = MH - 2 - TH;
    int x1 = x0 + TW - 1, y1 = y0 + TH - 1;

    s_town_x0 = (uint8_t)x0; s_town_y0 = (uint8_t)y0;
    s_town_x1 = (uint8_t)x1; s_town_y1 = (uint8_t)y1;
    s_town_x = (uint8_t)(x0 + TW / 2); s_town_y = (uint8_t)(y0 + TH / 2);

    /* level the ground the town stands on, and a one-tile apron around it */
    for (int y = y0 - 1; y <= y1 + 1; y++)
        for (int x = x0 - 1; x <= x1 + 1; x++)
            if (rl_in(x, y)) g_lv.terrain[y * MW + x] = T_FLOOR;

    /* wall, then hollow it out */
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            g_lv.terrain[y * MW + x] =
                (x == x0 || x == x1 || y == y0 || y == y1) ? T_TOWN_WALL : T_FLOOR;

    /* the streets: one east-west through the middle, one north-south */
    int rx = x0 + TW / 2, ry = y0 + TH / 2;
    for (int x = x0; x <= x1; x++) g_lv.terrain[ry * MW + x] = T_ROAD;
    for (int y = y0; y <= y1; y++) g_lv.terrain[y * MW + rx] = T_ROAD;

    /* gates east and west, with an approach road running out to the grass */
    for (int i = 1; i <= 3; i++) {
        if (rl_in(x0 - i, ry)) g_lv.terrain[ry * MW + x0 - i] = T_ROAD;
        if (rl_in(x1 + i, ry)) g_lv.terrain[ry * MW + x1 + i] = T_ROAD;
    }

    /* Six shopfronts on the street. Two rows either side of the high street,
     * skipping the crossroads column so the inn has the corner to itself. */
    static const int8_t sx[SHOP_N] = { -5, -3, 3, -5, -3, 3 };
    static const int8_t sy[SHOP_N] = { -1, -1, -1, 1, 1, 1 };
    for (int i = 0; i < SHOP_N; i++) {
        int x = rx + sx[i], y = ry + sy[i];
        if (!rl_in(x, y) || x <= x0 || x >= x1 || y <= y0 || y >= y1) {
            /* the clamp above can squeeze the compound; fall back to the
             * nearest free interior tile rather than dropping a shop */
            x = rx + (i % 3) - 1; y = ry + (i < 3 ? -1 : 1);
        }
        g_lv.terrain[y * MW + x] = T_SHOP;
        s_shop_x[i] = (uint8_t)x; s_shop_y[i] = (uint8_t)y;
    }

    /* the inn, one step off the crossroads, and a few houses for the look */
    g_lv.terrain[(ry - 1) * MW + rx + 1] = T_INN;
    g_lv.terrain[(ry + 1) * MW + rx + 1] = T_TOWN;
    if (rl_in(rx - 1, ry - 3)) g_lv.terrain[(ry - 3) * MW + rx - 1] = T_TOWN;
    if (rl_in(rx + 2, ry + 3)) g_lv.terrain[(ry + 3) * MW + rx + 2] = T_TOWN;
}

void rl_gen_overworld(void) {
    uint32_t keep = g_seed;
    g_seed = g_world_seed ? g_world_seed : 1;

    g_lv.n_mon = 0; g_lv.n_item = 0;

    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            int h = height_at(x, y);
            /* fall off toward the map edge so the continent is an island */
            int ex = x < MW - x ? x : MW - 1 - x;
            int ey = y < MH - y ? y : MH - 1 - y;
            int edge = ex < ey ? ex : ey;
            if (edge < 5) h -= (5 - edge) * 22;

            /* Five elevation bands. There is no water band -- the source
             * tileset has no water art (see rl_draw.c) -- so the continent is
             * ringed by impassable rock instead of sea, which is what the edge
             * falloff above produces. Thresholds are tuned so mountain lands
             * near 15% of the map: past about 20% the ranges stop reading as
             * ranges and start reading as scattered rubble. */
            uint8_t t;
            if      (h <  40) t = T_MOUNTAIN;   /* the coastal wall */
            else if (h < 140) t = T_FLOOR;      /* plain */
            else if (h < 178) t = T_TREE;       /* forest */
            else if (h < 220) t = T_HILL;       /* walkable highland */
            else              t = T_MOUNTAIN;   /* peaks */
            g_lv.terrain[y * MW + x] = t;
            g_lv.flags[y * MW + x] = CF_KNOWN;  /* the surface is not fogged */
        }
    }
    smooth_overworld();

    /* a town near the middle, on walkable ground */
    int tx = MW / 2, ty = MH / 2;
    for (int tries = 0; tries < 400; tries++) {
        int x = 8 + rl_range(MW - 16), y = 6 + rl_range(MH - 12);
        if (g_lv.terrain[y * MW + x] == T_FLOOR) { tx = x; ty = y; break; }
    }
    build_town(tx, ty);
    tx = s_town_x; ty = s_town_y;              /* build_town may have clamped it */

    /* The Mines' own entrance, always within sight of the town. A 64x48
     * continent seen through a 16x13 window is a big place to hunt for a cave
     * you have never seen; the game should not open with that. */
    for (int r = TW / 2 + 2; r < TW / 2 + 12; r++) {
        int done = 0;
        for (int j = -r; j <= r && !done; j++) {
            for (int i = -r; i <= r && !done; i++) {
                if ((i * i + j * j) < (r - 1) * (r - 1)) continue;
                int x = tx + i, y = ty + j;
                if (!rl_in(x, y)) continue;
                uint8_t t = g_lv.terrain[y * MW + x];
                if (t != T_FLOOR && t != T_TREE) continue;
                g_lv.terrain[y * MW + x] = T_DUNGEON_MOUTH;
                done = 1;
            }
        }
        if (done) break;
    }

    /* more mouths scattered further out, for the look of the thing */
    int placed = 0;
    for (int tries = 0; tries < 900 && placed < 5; tries++) {
        int x = 4 + rl_range(MW - 8), y = 4 + rl_range(MH - 8);
        uint8_t t = g_lv.terrain[y * MW + x];
        if (t != T_FLOOR && t != T_TREE) continue;
        int dx = x - tx, dy = y - ty;
        if (dx * dx + dy * dy < 200) continue;         /* not on the doorstep */
        g_lv.terrain[y * MW + x] = T_DUNGEON_MOUTH;
        placed++;
    }

    /* the player arrives where they left, or at the town on a new game */
    if (g_pl.wx && rl_in(g_pl.wx, g_pl.wy) && rl_walkable(g_pl.wx, g_pl.wy)) {
        g_pl.x = g_pl.wx; g_pl.y = g_pl.wy;
    } else {
        g_pl.x = (uint8_t)tx; g_pl.y = (uint8_t)ty;
    }

    /* Surface wildlife. Level-capped at 2, NOT by table index: the animals
     * sheet is ordered by sprite, not by difficulty, so "the first eight kinds"
     * happens to include a level-6 great elk -- which killed a fresh level-1
     * warrior on the overworld in the first playtest. */
    g_seed = keep;
    int n = 6 + rl_range(6);
    for (int i = 0; i < n && g_lv.n_mon < MAX_MON; i++) {
        for (int tries = 0; tries < 60; tries++) {
            int x = rl_range(MW), y = rl_range(MH);
            uint8_t t = g_lv.terrain[y * MW + x];
            if (t != T_FLOOR && t != T_TREE) continue;
            /* nothing wanders the streets: a jackal inside the walls turns the
             * shopping trip into a fight you did not choose */
            if (x >= s_town_x0 - 1 && x <= s_town_x1 + 1 &&
                y >= s_town_y0 - 1 && y <= s_town_y1 + 1) continue;
            int dx = x - g_pl.x, dy = y - g_pl.y;
            if (dx * dx + dy * dy < 64) continue;
            int k = 0;
            for (int a = 0; a < 40; a++) {
                int c = rl_range(g_mon_kind_n);
                if (g_mon_kind[c].sheet == SH_ANIMALS && g_mon_kind[c].lvl <= 2) { k = c; break; }
            }
            const MonKind *mk = &g_mon_kind[k];
            Mon *m = &g_lv.mon[g_lv.n_mon++];
            m->x = (uint8_t)x; m->y = (uint8_t)y; m->kind = (uint8_t)k; m->boss = 0;
            m->mhp = m->hp = (int16_t)rl_dice(mk->hp_d, mk->hp_s);
            m->speed = mk->speed; m->energy = (int16_t)rl_range(100);
            m->flags = MF_ASLEEP;
            break;
        }
    }
    rl_fov();
}

/* --- shops -------------------------------------------------------------- */
/* Six shops, Moria style. Stock is rolled from the item table filtered by the
 * shop's remit, restocked whenever you re-enter town. */
const char *const g_shop_name[SHOP_N] = {
    "General Store", "Armoury", "Weaponsmith",
    "Alchemist", "Magic Shop", "Black Market",
};

/* Shopfronts. The first five are the coloured house/shop tiles the annotator
 * pass labelled at source (13,1)..(13,5) -- doors_gems_banners cells 6, 10, 14,
 * 18 and 22. The Black Market gets the grey "station/shop" from props_light
 * (6,8), because five shopfronts is what the tileset has and the sixth trader
 * should not look like one of the honest ones anyway. */
const uint8_t g_shop_sheet[SHOP_N] = { SH_DOORS, SH_DOORS, SH_DOORS,
                                       SH_DOORS, SH_DOORS, SH_PROPS };
const uint8_t g_shop_cell[SHOP_N]  = { 14, 22, 6, 10, 18, 26 };

Item  g_shop_stock[SHOP_N][SHOP_SLOTS];
uint8_t g_shop_n[SHOP_N];

static int shop_wants(int shop, const ItemKind *ik) {
    switch (shop) {
    case 0: return ik->tv == TV_FOOD || ik->tv == TV_LIGHT;
    case 1: return ik->tv == TV_ARMOUR;
    case 2: return ik->tv == TV_WEAPON;
    case 3: return ik->tv == TV_POTION;
    case 4: return ik->tv == TV_SCROLL || ik->tv == TV_WAND;
    default: return ik->tv == TV_RING || ik->tv == TV_WAND;   /* black market */
    }
}

void rl_shop_restock(void) {
    for (int s = 0; s < SHOP_N; s++) {
        g_shop_n[s] = 0;
        int want = 4 + rl_range(4);
        for (int tries = 0; tries < 300 && g_shop_n[s] < want; tries++) {
            int k = rl_range(g_item_kind_n);
            const ItemKind *ik = &g_item_kind[k];
            if (!shop_wants(s, ik)) continue;
            /* the black market carries out-of-depth goods at a premium */
            if (s != 5 && ik->lvl > 12 + (int)g_pl.deepest) continue;
            Item *it = &g_shop_stock[s][g_shop_n[s]++];
            rl_make_item(it, s == 5 ? 20 + g_pl.deepest : 4 + g_pl.deepest / 2);
            it->kind = (uint8_t)k;
            it->qty = (ik->tv == TV_FOOD || ik->tv == TV_POTION) ? (uint8_t)(2 + rl_range(3)) : 1;
        }
    }
}

int rl_shop_price(const Item *it, int shop) {
    const ItemKind *ik = &g_item_kind[it->kind];
    int p = ik->cost;
    p += (it->to_hit + it->to_dam + it->to_ac) * 45;
    if (it->ego) p += 300 * (g_ego_kind[it->ego].bonus > 0 ? g_ego_kind[it->ego].bonus : 1);
    if (p < 2) p = 2;
    if (shop == 5) p *= 5;                       /* the classic money sink */
    /* CHA shaves a little off, so the stat is not dead weight */
    p = p * (130 - g_pl.stat[5]) / 120;
    return p < 1 ? 1 : p;
}

int rl_shop_buy(int shop, int slot) {
    if (slot < 0 || slot >= g_shop_n[shop]) return 0;
    Item *it = &g_shop_stock[shop][slot];
    int price = rl_shop_price(it, shop);
    if (g_pl.gold < price) { rl_msg("You cannot afford it."); return 0; }
    Item one = *it; one.qty = 1;
    if (rl_inv_add(&one) < 0) { rl_msg("Pack is full."); return 0; }
    g_pl.gold -= price;
    rl_item_learn(it->kind);          /* buying it tells you what it is */
    if (--it->qty == 0) {
        for (int i = slot; i < g_shop_n[shop] - 1; i++)
            g_shop_stock[shop][i] = g_shop_stock[shop][i + 1];
        g_shop_n[shop]--;
    }
    rl_msg("Bought.");
    return 1;
}

int rl_shop_sell(int slot) {
    if (slot < 0 || slot >= INV_N || !g_pl.inv[slot].qty) return 0;
    Item *it = &g_pl.inv[slot];
    for (int e = 0; e < EQ_N; e++)
        if (*rl_slot_ptr(e) == slot) { rl_msg("Take it off first."); return 0; }
    int price = rl_shop_price(it, 0) / 3;        /* shops buy low */
    g_pl.gold += price;
    it->qty = 0;
    rl_msgf("Sold for %d gold.", price);
    return 1;
}
