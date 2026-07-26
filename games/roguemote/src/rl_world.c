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
/* Shop DOORWAYS, not shops. The six traders are global -- one stock list each --
 * but a door is a place, and now more than one settlement has them: the walled
 * town keeps all six, and each outlying town puts two or three of the same
 * traders on its street. So the map is (x, y) -> shop index, many to one. */
#define SHOP_SITES 28
static uint8_t s_site_x[SHOP_SITES], s_site_y[SHOP_SITES], s_site_shop[SHOP_SITES];
static uint8_t s_n_site;

static void add_shop_site(int x, int y, int shop) {
    if (s_n_site >= SHOP_SITES) return;
    s_site_x[s_n_site] = (uint8_t)x;
    s_site_y[s_n_site] = (uint8_t)y;
    s_site_shop[s_n_site] = (uint8_t)shop;
    s_n_site++;
}

int rl_in_town(void) {
    return g_pl.depth == 0 &&
           g_pl.x >= s_town_x0 && g_pl.x <= s_town_x1 &&
           g_pl.y >= s_town_y0 && g_pl.y <= s_town_y1;
}

/* How deep a tower's shaft goes: set by how far from town it stands, so the
 * world map is worth reading before you set out and the geography IS the
 * difficulty gradient. Capped to six floors past your best, which keeps a tower
 * a shortcut rather than a way for a level-one character to walk somewhere
 * pretty and fall thirty floors. */
int rl_tower_depth(int x, int y) {
    int dx = x - s_town_x, dy = y - s_town_y;
    int d = (dx * dx + dy * dy) / 900;
    if (d < 2) d = 2;
    int cap = (int)g_pl.deepest + 6;
    if (d > cap) d = cap;
    if (d > 40) d = 40;
    return d;
}

int rl_shop_at(int x, int y) {
    for (int i = 0; i < s_n_site; i++)
        if (s_site_x[i] == x && s_site_y[i] == y) return s_site_shop[i];
    return -1;
}

/* --- value noise -------------------------------------------------------- */
static int noise2(int x, int y, uint32_t salt) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + salt;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int)((h >> 16) & 0xFF);
}

/* bilinear-ish smoothing over a coarse lattice: cheap, and at this cell size
 * the blockiness reads as terrain rather than as an artefact */
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
#define TR_X 11         /* footprint radii, before the wobble */
#define TR_Y 8

/* The footprint. A ring of radius (TR_X,TR_Y) pushed in and out by a hash on
 * the ANGLE, so the wall bulges and tucks the way a wall built over generations
 * does, then smoothed once so the boundary is a line rather than a fringe.
 * Perturbing per-angle rather than per-cell is what keeps it a closed blob. */
static uint8_t s_inside[MW * MH];

static int town_r2(int cx, int cy, int x, int y) {
    int dx = x - cx, dy = y - cy;
    /* Quantise the DIRECTION into eight sectors and give each its own radius.
     * It has to depend on angle alone: an earlier version folded (|dx|+|dy|)&1
     * into the sector, which is distance parity, so the radius alternated ring
     * by ring and the wall came out as scattered fragments instead of a line. */
    int oct = 0;
    if (dx || dy) {
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        oct = (dx < 0 ? 4 : 0) + (dy < 0 ? 2 : 0) + (ax > ay ? 1 : 0);
    }
    int wob = (int)(noise2(oct, 0, g_world_seed ^ 0x51ed270bu) & 63) - 26;   /* -26..+37 */
    /* normalised squared distance x1000, against 1000 plus the wobble */
    int nx = dx * 1000 / TR_X, ny = dy * 1000 / TR_Y;
    return (nx * nx + ny * ny) / 1000 - (1000 + wob * 12);
}

static void build_town(int cx, int cy) {
    /* The continent is rebuilt from the world seed every time you surface, so
     * the shop table has to start empty each time. Left to accumulate it filled
     * with the doorways of previous worlds after two or three trips, and then
     * refused to register any more -- so the outlying towns silently stopped
     * having shops and rl_shop_at() answered from a map that no longer existed.
     * This is the first thing that registers a door, so it is where the reset
     * belongs. */
    s_n_site = 0;

    if (cx < TR_X + 3) cx = TR_X + 3;
    if (cy < TR_Y + 3) cy = TR_Y + 3;
    if (cx > MW - TR_X - 4) cx = MW - TR_X - 4;
    if (cy > MH - TR_Y - 4) cy = MH - TR_Y - 4;

    int x0 = cx - TR_X - 2, y0 = cy - TR_Y - 2;
    int x1 = cx + TR_X + 2, y1 = cy + TR_Y + 2;
    s_town_x0 = (uint8_t)x0; s_town_y0 = (uint8_t)y0;
    s_town_x1 = (uint8_t)x1; s_town_y1 = (uint8_t)y1;
    s_town_x = (uint8_t)cx;  s_town_y = (uint8_t)cy;

    for (int i = 0; i < MW * MH; i++) s_inside[i] = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (rl_in(x, y) && town_r2(cx, cy, x, y) < 0) s_inside[y * MW + x] = 1;

    /* one majority pass: a hash-perturbed edge is ragged at the pixel, and a
     * town wall should read as built, not as eroded */
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!rl_in(x, y)) continue;
            int n = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++)
                    if (rl_in(x + i, y + j) && s_inside[(y + j) * MW + x + i]) n++;
            g_lv.layer[y * MW + x] = (uint8_t)(n >= 5);
        }
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (rl_in(x, y)) s_inside[y * MW + x] = g_lv.layer[y * MW + x];

    /* level the ground the town stands on and a ring outside it */
    for (int y = y0 - 1; y <= y1 + 1; y++)
        for (int x = x0 - 1; x <= x1 + 1; x++)
            if (rl_in(x, y)) g_lv.terrain[y * MW + x] = T_FLOOR;

    /* The wall is TWO cells thick, and it has to be. blob47 resolves a tile from
     * its eight neighbours, so a one-cell-thick ring that steps diagonally hands
     * the ruleset nothing but isolated and end-cap cells -- it drew as a scatter
     * of loose blocks rather than a wall. Two cells give the ruleset mass, and
     * the rim reads as masonry all the way round.
     *
     * Pass one marks the cells touching outside; pass two marks the cells
     * touching those. */
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!rl_in(x, y) || !s_inside[y * MW + x]) continue;
            int edge = !s_inside[y * MW + x - 1] || !s_inside[y * MW + x + 1] ||
                       !s_inside[(y - 1) * MW + x] || !s_inside[(y + 1) * MW + x];
            g_lv.terrain[y * MW + x] = edge ? T_TOWN_WALL : T_FLOOR;
        }
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!rl_in(x, y) || !s_inside[y * MW + x]) continue;
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (rl_ter(x - 1, y) == T_TOWN_WALL || rl_ter(x + 1, y) == T_TOWN_WALL ||
                rl_ter(x, y - 1) == T_TOWN_WALL || rl_ter(x, y + 1) == T_TOWN_WALL)
                g_lv.layer[y * MW + x] = 2;      /* stage it; see below */
            else
                g_lv.layer[y * MW + x] = 0;
        }
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (rl_in(x, y) && s_inside[y * MW + x] && g_lv.layer[y * MW + x] == 2)
                g_lv.terrain[y * MW + x] = T_TOWN_WALL;

    /* The high street wanders. A ruler-straight cross through a wall that is not
     * a box looks like two different towns. Walk east-west through the centre,
     * drifting a row at a time, and punch out through the wall at both ends. */
    int ry = cy;
    for (int x = cx; x <= x1 + 3; x++) {
        if (!rl_in(x, ry)) break;
        g_lv.terrain[ry * MW + x] = T_ROAD;
        if (rl_pct(28) && ry < cy + 2 && s_inside[(ry + 1) * MW + x]) ry++;
        else if (rl_pct(28) && ry > cy - 2 && s_inside[(ry - 1) * MW + x]) ry--;
    }
    ry = cy;
    for (int x = cx; x >= x0 - 3; x--) {
        if (!rl_in(x, ry)) break;
        g_lv.terrain[ry * MW + x] = T_ROAD;
        if (rl_pct(28) && ry < cy + 2 && s_inside[(ry + 1) * MW + x]) ry++;
        else if (rl_pct(28) && ry > cy - 2 && s_inside[(ry - 1) * MW + x]) ry--;
    }
    /* a north-south lane off the middle, stopping at the wall */
    int rx = cx;
    for (int y = cy; y >= y0; y--) {
        if (!rl_in(rx, y) || !s_inside[y * MW + rx]) break;
        g_lv.terrain[y * MW + rx] = T_ROAD;
        if (rl_pct(25) && s_inside[y * MW + rx + 1]) rx++;
    }
    rx = cx;
    for (int y = cy; y <= y1; y++) {
        if (!rl_in(rx, y) || !s_inside[y * MW + rx]) break;
        g_lv.terrain[y * MW + rx] = T_ROAD;
        if (rl_pct(25) && s_inside[y * MW + rx - 1]) rx--;
    }

    /* Buildings go on the street front: an interior tile that touches a road.
     * Collected in scan order and taken with a spacing, so the six shops line
     * the lanes instead of clumping at the crossroads. */
    uint16_t front[128]; int nf = 0;
    for (int y = y0; y <= y1 && nf < 128; y++) {
        for (int x = x0; x <= x1 && nf < 128; x++) {
            if (!rl_in(x, y) || g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (!s_inside[y * MW + x]) continue;
            if (rl_ter(x - 1, y) != T_ROAD && rl_ter(x + 1, y) != T_ROAD &&
                rl_ter(x, y - 1) != T_ROAD && rl_ter(x, y + 1) != T_ROAD) continue;
            front[nf++] = (uint16_t)(y * MW + x);
        }
    }
    int placed = 0;
    for (int step = 2; step >= 1 && placed < SHOP_N; step--) {
        for (int i = 0; i < nf && placed < SHOP_N; i++) {
            int p = front[i], x = p % MW, y = p / MW;
            if (g_lv.terrain[p] != T_FLOOR) continue;
            int near = 0;                       /* keep the fronts apart */
            for (int k = 0; k < placed; k++) {
                int dx = s_site_x[k] - x, dy = s_site_y[k] - y;
                if (dx * dx + dy * dy < step * step * 4) { near = 1; break; }
            }
            if (near) continue;
            g_lv.terrain[p] = T_SHOP;
            add_shop_site(x, y, placed);
            placed++;
        }
    }
    /* A short street front must not cost a shop: rl_shop_at() would return -1
     * for a door the player is standing on, and the sixth trader would simply
     * not exist. Anything left over goes on the first interior floor going. */
    for (int y = y0; y <= y1 && placed < SHOP_N; y++) {
        for (int x = x0; x <= x1 && placed < SHOP_N; x++) {
            if (!rl_in(x, y) || !s_inside[y * MW + x]) continue;
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            g_lv.terrain[y * MW + x] = T_SHOP;
            add_shop_site(x, y, placed);
            placed++;
        }
    }

    /* the inn and a few houses fill the remaining street front */
    int want_inn = 1, houses = 4;
    for (int i = 0; i < nf && (want_inn || houses); i++) {
        int p = front[i];
        if (g_lv.terrain[p] != T_FLOOR) continue;
        if (want_inn) { g_lv.terrain[p] = T_INN; want_inn = 0; continue; }
        if (rl_pct(45)) { g_lv.terrain[p] = T_TOWN; houses--; }
    }
    /* the player arrives on the crossroads, which is road by construction */
    g_lv.terrain[cy * MW + cx] = T_ROAD;
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

    /* The Mines' own entrance, always within sight of the town. A 128x96
     * continent seen through a 16x13 window is nearly sixty screens to hunt a
     * cave you have never seen in; the game must not open with that. */
    for (int r = TR_X + 3; r < TR_X + 13; r++) {
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

    /* More mouths scattered further out. The count is per-area rather than a
     * fixed five: on the old 64x48 continent five was a mouth every other
     * screen, and the same five spread over four times the ground would be a
     * long walk between them. */
    int placed = 0, want_mouths = 5 + (MW * MH) / 3000;
    for (int tries = 0; tries < 2600 && placed < want_mouths; tries++) {
        int x = 4 + rl_range(MW - 8), y = 4 + rl_range(MH - 8);
        uint8_t t = g_lv.terrain[y * MW + x];
        if (t != T_FLOOR && t != T_TREE) continue;
        int dx = x - tx, dy = y - ty;
        if (dx * dx + dy * dy < 260) continue;         /* not on the doorstep */
        g_lv.terrain[y * MW + x] = T_DUNGEON_MOUTH;
        placed++;
    }

    /* Ruins: a broken ring of stone with something left in it. Six cave mouths
     * and a town was the whole of the surface, so crossing the continent was a
     * walk with nothing on the way. A ruin is a reason to go and look, and the
     * chest inside is worth the detour without being worth farming -- the tier
     * hash reads depth, and on the surface depth is zero. */
    for (int i = 0, ruins = 6 + (MW * MH) / 900 + rl_range(6); i < ruins; i++) {
        for (int tries = 0; tries < 260; tries++) {
            int x = 5 + rl_range(MW - 10), y = 5 + rl_range(MH - 10);
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            int dx = x - tx, dy = y - ty;
            if (dx * dx + dy * dy < 120) continue;
            int w = 3 + rl_range(3), h = 3 + rl_range(2);
            int ok = 1;
            for (int j = y; j < y + h && ok; j++)
                for (int k = x; k < x + w && ok; k++)
                    if (!rl_in(k, j) || g_lv.terrain[j * MW + k] != T_FLOOR) ok = 0;
            if (!ok) continue;
            for (int j = y; j < y + h; j++)
                for (int k = x; k < x + w; k++) {
                    int edge = (k == x || k == x + w - 1 || j == y || j == y + h - 1);
                    /* a third of the wall has fallen, so it reads as a ruin
                     * rather than as a shed, and you can always get inside */
                    if (edge && rl_pct(66)) g_lv.terrain[j * MW + k] = T_RUBBLE;
                }
            g_lv.terrain[(y + h / 2) * MW + x + w / 2] = T_CHEST;
            break;
        }
    }

    /* --- outlying towns ---------------------------------------------------
     * The walled town is the capital and keeps all six traders. These are the
     * other places people live: no wall, because a wall is a thing you build
     * when you are worth attacking, and these are a street with houses either
     * side. Two or three of the same six traders keep a door here, so a town on
     * the far side of the map is somewhere you can actually outfit from rather
     * than a diorama.
     *
     * The shop table is many-to-one now: the trader is global, the doorway is a
     * place, and the same trader can have a door in two towns. */
    for (int v = 0, want = 2 + rl_range(3); v < want; v++) {
        for (int tries = 0; tries < 400; tries++) {
            int x = 10 + rl_range(MW - 20), y = 8 + rl_range(MH - 16);
            int dx = x - tx, dy = y - ty;
            if (dx * dx + dy * dy < 1100) continue;     /* well clear of the capital */
            int half = 4 + rl_range(3);                 /* half the street's length */
            int ok = 1;
            for (int j = y - 2; j <= y + 2 && ok; j++)
                for (int k = x - half - 1; k <= x + half + 1 && ok; k++)
                    if (!rl_in(k, j) || g_lv.terrain[j * MW + k] != T_FLOOR) ok = 0;
            if (!ok) continue;

            /* the street */
            for (int k = x - half; k <= x + half; k++) g_lv.terrain[y * MW + k] = T_ROAD;
            g_lv.terrain[y * MW + x] = T_INN;

            /* Houses and shopfronts down both sides, every door on the road.
             * `taken` keeps one town from putting the same trader behind two of
             * its own doors -- three doors to the alchemist and none to the
             * smith is a town with one shop and a decorating problem. */
            int taken = 0;
            for (int k = x - half; k <= x + half; k++) {
                if (k == x) continue;
                for (int side = -1; side <= 1; side += 2) {
                    int hy = y + side;
                    if (!rl_in(k, hy) || g_lv.terrain[hy * MW + k] != T_FLOOR) continue;
                    if (rl_pct(14) && s_n_site < SHOP_SITES) {
                        int shop = -1;
                        for (int a = 0; a < 12 && shop < 0; a++) {
                            int c = rl_range(SHOP_N);
                            if (!(taken & (1 << c))) shop = c;
                        }
                        if (shop >= 0) {
                            taken |= 1 << shop;
                            g_lv.terrain[hy * MW + k] = T_SHOP;
                            add_shop_site(k, hy, shop);
                            continue;
                        }
                    }
                    if (rl_pct(70)) g_lv.terrain[hy * MW + k] = T_TOWN;
                }
            }
            break;
        }
    }

    /* --- towers -----------------------------------------------------------
     * The other way underground, and the one you can see coming. A cave mouth
     * is a hole in the ground that reads as scenery until you are on it; a
     * tower is a landmark, and its shaft goes deeper the further out it stands
     * (see rl_tower_depth), so the map has a difficulty gradient drawn on it.
     * They stand on high ground where there is any -- a tower in a hollow is a
     * tower nobody sees. */
    for (int i = 0, towers = 3 + rl_range(3); i < towers; i++) {
        int bx = -1, by = -1, best = -1;
        for (int tries = 0; tries < 500; tries++) {
            int x = 6 + rl_range(MW - 12), y = 6 + rl_range(MH - 12);
            uint8_t t = g_lv.terrain[y * MW + x];
            if (t != T_FLOOR && t != T_HILL) continue;
            int dx = x - tx, dy = y - ty;
            int d = dx * dx + dy * dy;
            if (d < 700) continue;                      /* not on the doorstep */
            /* prefer high ground, and prefer further out */
            int score = d + (t == T_HILL ? 4000 : 0);
            if (score > best) { best = score; bx = x; by = y; }
        }
        if (bx >= 0) g_lv.terrain[by * MW + bx] = T_TOWER;
    }

    /* --- roads ------------------------------------------------------------
     * A dogleg of road from the town gate out to the nearest cave mouths. It is
     * not pathfinding and does not need to be: the point is that stepping out
     * of the gate you can SEE a way to go, instead of a featureless green field
     * in every direction. Roads lay over grass and forest and stop at rock, so
     * a road never carves through a mountain. */
    for (int m = 0; m < 3; m++) {
        int bx = -1, by = -1, bd = 1 << 30;
        for (int y = 0; y < MH; y++)
            for (int x = 0; x < MW; x++) {
                if (g_lv.terrain[y * MW + x] != T_DUNGEON_MOUTH) continue;
                int dx = x - tx, dy = y - ty, d = dx * dx + dy * dy;
                if (d > 260 && d < bd && !(g_lv.flags[y * MW + x] & CF_ROOM)) {
                    bd = d; bx = x; by = y;
                }
            }
        if (bx < 0) break;
        g_lv.flags[by * MW + bx] |= CF_ROOM;        /* mark it as already served */

        int cx2 = tx, cy2 = ty;
        for (int step = 0; step < MW + MH; step++) {
            if (cx2 == bx && cy2 == by) break;
            if (cx2 != bx && (cy2 == by || rl_pct(55))) cx2 += cx2 < bx ? 1 : -1;
            else if (cy2 != by)                        cy2 += cy2 < by ? 1 : -1;
            uint8_t t = g_lv.terrain[cy2 * MW + cx2];
            if (t == T_FLOOR || t == T_TREE) g_lv.terrain[cy2 * MW + cx2] = T_ROAD;
        }
    }
    for (int i = 0; i < MW * MH; i++) g_lv.flags[i] &= (uint8_t)~CF_ROOM;

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
    int n = 10 + (MW * MH) / 900 + rl_range(8);
    for (int i = 0; i < n && g_lv.n_mon < MAX_MON; i++) {
        for (int tries = 0; tries < 140; tries++) {
            int x = rl_range(MW), y = rl_range(MH);
            uint8_t t = g_lv.terrain[y * MW + x];
            if (t != T_FLOOR && t != T_TREE) continue;
            /* nothing wanders the streets: a jackal inside the walls turns the
             * shopping trip into a fight you did not choose */
            if (x >= s_town_x0 - 1 && x <= s_town_x1 + 1 &&
                y >= s_town_y0 - 1 && y <= s_town_y1 + 1) continue;
            int dx = x - g_pl.x, dy = y - g_pl.y;
            if (dx * dx + dy * dy < 64) continue;
            /* The cap scales with how deep you have been. Fixed at 2 it meant
             * that once you could survive floor ten the surface was an empty
             * corridor between the town and the stairs -- nothing out here was
             * ever a reason to be careful again. */
            int cap = 2 + g_pl.deepest / 2;
            if (cap > 14) cap = 14;
            int k = 0;
            for (int a = 0; a < 40; a++) {
                int c = rl_range(g_mon_kind_n);
                if (g_mon_kind[c].flags & MK_MIMIC) continue;
                if (g_mon_kind[c].sheet == SH_ANIMALS && g_mon_kind[c].lvl <= cap) { k = c; break; }
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
    case 0: return ik->tv == TV_FOOD || ik->tv == TV_LIGHT;   /* lamps included */
    case 1: return ik->tv == TV_ARMOUR;
    case 2: return ik->tv == TV_WEAPON || ik->tv == TV_BOW || ik->tv == TV_AMMO;
    case 3: return ik->tv == TV_POTION;
    case 4: return ik->tv == TV_SCROLL || ik->tv == TV_WAND || ik->tv == TV_TOOL;
    default: return ik->tv == TV_RING || ik->tv == TV_WAND;   /* black market */
    }
}

void rl_shop_restock(void) {
    for (int s = 0; s < SHOP_N; s++) {
        g_shop_n[s] = 0;
        int want = 4 + rl_range(4);
        if (want > SHOP_SLOTS) want = SHOP_SLOTS;
        int depth = (s == 5) ? 20 + g_pl.deepest : 4 + g_pl.deepest / 2;
        for (int tries = 0; tries < 300 && g_shop_n[s] < want; tries++) {
            int k = rl_range(g_item_kind_n);
            const ItemKind *ik = &g_item_kind[k];
            if (!shop_wants(s, ik)) continue;
            /* the black market carries out-of-depth goods at a premium */
            if (s != 5 && ik->lvl > 12 + (int)g_pl.deepest) continue;

            /* One of each kind. A four-slot window showing the same potion
             * three times is a two-slot window, and with pools of six kinds
             * that happened about a third of the time. */
            int dup = 0;
            for (int i = 0; i < g_shop_n[s]; i++)
                if (g_shop_stock[s][i].kind == (uint8_t)k) { dup = 1; break; }
            if (dup) continue;

            Item *it = &g_shop_stock[s][g_shop_n[s]++];
            /* rl_make_item_kind, NOT rl_make_item: rolling a random kind and
             * then overwriting `kind` handed this item the enchantment rolled
             * for a different one. Rations arrived at +3 and cursed "of
             * Morgul", while weapons that happened to follow a potion roll
             * arrived stubbornly plain. */
            rl_make_item_kind(it, k, depth);
            it->qty = (ik->tv == TV_TOOL) ? (uint8_t)(3 + rl_range(6))
                    : (ik->tv == TV_AMMO) ? (uint8_t)(12 + rl_range(19))
                    : (ik->tv == TV_FOOD || ik->tv == TV_POTION || ik->tv == TV_SCROLL)
                      ? (uint8_t)(2 + rl_range(3)) : 1;
            /* The five honest traders do not knowingly sell cursed goods. The
             * Black Market is called that for a reason. */
            if (s != 5) {
                it->flags &= (uint8_t)~IF_CURSED;
                if (it->ego && (g_ego_kind[it->ego].flags & EGO_CURSED)) it->ego = 0;
            }
        }
    }
}

/* Price = utility x rarity, and both have to be read off the item itself.
 *
 * The first cut added a flat 45 gold per point of enchantment and a flat 300 per
 * ego, which said three untrue things at once: that +8 was worth the same on a
 * dagger as on a Blade of Chaos, that "of Attacks" (x1.33 AND an extra blow, so
 * about x2.67 in practice) was worth less than "of Slaying" at x1.67, and that a
 * cursed "of Morgul" was worth THREE TIMES a plain sword -- so the best trade in
 * the game was farming curses.
 *
 * Now each plus is worth a slice OF THE ITEM and the slices compound, because
 * high plusses are rare as well as strong; and the ego contributes a multiplier
 * from its own `worth` column, which is graded against effective power. */
int rl_shop_price(const Item *it, int shop) {
    const ItemKind *ik = &g_item_kind[it->kind];
    int32_t base = ik->cost;

    /* One enchantment scale for both: armour has a single to_ac, a weapon has
     * two plusses that roll apart, so a weapon's is their average. Using the
     * SUM put weapons on double armour's curve for the same quality of roll. */
    int plus = (ik->tv == TV_ARMOUR) ? it->to_ac : (it->to_hit + it->to_dam + 1) / 2;
    if (plus < 0) plus = 0;                      /* a penalty is not a discount you pay for */
    if (plus > 24) plus = 24;                    /* keeps the quadratic in range */

    int32_t p = base * (100 + plus * 15 + plus * plus) / 100 + plus * 30;
    if (it->ego) p = p * (int32_t)g_ego_kind[it->ego].worth / 8;
    if (it->flags & IF_CURSED) p /= 4;           /* and nobody wants it either */

    if (p < 2) p = 2;
    if (shop == 5) p *= 3;                       /* the classic money sink */
    /* CHA shaves a little off, so the stat is not dead weight */
    p = (int)((int64_t)p * (130 - rl_stat(5)) / 120);
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
    int32_t price = rl_shop_price(it, 0) / 3;    /* shops buy low */

    /* A trader's purse is finite, and it has to be. Uncapped, one legendary
     * find pays for every shop in the game for ever -- and the best thing you
     * ever pulled out of a vault becomes something you sold rather than
     * something you fought with. The purse grows as you go deeper, so the cap
     * is a ceiling on early windfalls rather than a tax on late ones. */
    int32_t purse = 400 + (int32_t)g_pl.deepest * 260;
    int capped = price > purse;
    if (capped) price = purse;

    if (price < 1) price = 1;                    /* nothing sells for nothing */
    g_pl.gold += price;
    it->qty = 0;
    if (capped) rl_msgf("That is all I can pay: %d.", (int)price);
    else        rl_msgf("Sold for %d gold.", (int)price);
    return 1;
}
