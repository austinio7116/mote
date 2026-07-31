/*
 * Motebox — powers and the wheel.
 *
 * 48 powers in 6 tabs is the plan (DESIGN.md 11); Phase 2 ships the two that
 * need no civilisation to act on: LAND (terraform) and WRATH (the disasters).
 *
 * Powers carry an explicit PW_* id and every cast dispatches on THAT, never on
 * the slot index. Rearranging a tab to put a power on a different arm then moves
 * an icon and nothing else; the first draft switched on position and a reorder
 * silently swapped fire for lightning.
 */
#include "mb.h"
#include <string.h>

#include "ui_status.h"
#include "ui_gauges.h"
#include "crowns_fx.h"
#include "nature.h"
#include "characters.h"
#include "animals.h"
#include "bosses.h"
#include "monsters.h"
#include "buildings.h"
#include "treasure_ore.h"
#include "ui_buttons.h"

/* --- what a power is ---------------------------------------------------- */

enum {
    /* LAND */
    PW_RAISE, PW_MOUNTAIN, PW_FOREST, PW_GRASS, PW_LOWER, PW_WATER, PW_DESERT, PW_ROAD,
    /* WRATH */
    PW_FIRE, PW_LIGHTNING, PW_METEOR, PW_VOLCANO, PW_QUAKE, PW_TORNADO, PW_ACID, PW_FREEZE,
    /* LIFE */
    PW_HUMAN, PW_ELF, PW_DWARF, PW_ORC, PW_VILLAGE, PW_HERD, PW_WOLVES, PW_PLANTS,
    /* BLESS */
    PW_RAIN, PW_FERTILITY, PW_HEAL, PW_INSPIRE, PW_PEACE, PW_GOLDVEIN, PW_BLESS, PW_RESURRECT,
    /* CURSE */
    PW_PLAGUE, PW_MADNESS, PW_CURSE, PW_WEAKEN, PW_FAMINE, PW_BARREN, PW_GRUDGE, PW_MARK,
    /* BEASTS */
    PW_TITAN, PW_MEDUSA, PW_REAPER, PW_DRAGON, PW_GOLEM, PW_SWARM, PW_EYE, PW_ANGEL
};

typedef struct {
    uint8_t          id;          /* PW_* — what casting does */
    const char      *name;
    const MoteImage *icon;
    uint8_t          ix, iy;      /* icon cell */
    uint8_t          radius;      /* brush radius in tiles */
    uint8_t          brush;       /* 1 = holding A keeps casting */
    uint16_t         cost;        /* Faith — charged from Phase 7; recorded now */
} Power;

/* FOUR ARMS, TWO STOPS EACH — because the Thumby Color's d-pad reports only the
 * four cardinals: UP+RIGHT does not arrive as both, so a conventional 8-slice
 * radial selected by direction is unreachable on the hardware however well it
 * reads on screen.
 *
 * Each arm therefore holds two powers, near and far. Pressing a direction picks
 * that arm's NEAR power; pressing the same direction again steps out to the FAR
 * one; pressing a different direction jumps to that arm's near slot. Four powers
 * are one press, four are two, and the spatial muscle memory survives.
 *
 * Slot order is arm-major — N-near, N-far, E-near, E-far, S-near, S-far, W-near,
 * W-far — so the even slots are the one-press ones and hold what you reach for.
 */
static const Power P_LAND[8] = {
    { PW_RAISE,    "RAISE",    &ui_gauges_img,  1, 1, 2, 1,   2 },
    { PW_MOUNTAIN, "MOUNTAIN", &ui_status_img,  7, 5, 2, 1,   8 },
    { PW_FOREST,   "FOREST",   &nature_img,     6, 4, 3, 1,   4 },
    { PW_GRASS,    "GRASS",    &ui_status_img,  9, 5, 3, 1,   2 },
    { PW_LOWER,    "LOWER",    &ui_gauges_img,  1, 3, 2, 1,   2 },
    { PW_WATER,    "WATER",    &ui_status_img,  7, 3, 2, 1,   4 },
    { PW_DESERT,   "DESERT",   &nature_img,     5, 4, 3, 1,   3 },   /* a rock, not "reeds" */
    { PW_ROAD,     "ROAD",     &ui_status_img,  5, 5, 1, 1,   1 },
};
static const Power P_WRATH[8] = {
    { PW_FIRE,     "FIRE",     &ui_status_img, 10, 3, 2, 1,   8 },
    { PW_LIGHTNING,"LIGHTNING",&ui_status_img,  3, 3, 0, 0,  12 },
    { PW_METEOR,   "METEOR",   &crowns_fx_img,  4, 7, 3, 0, 120 },
    { PW_VOLCANO,  "VOLCANO",  &crowns_fx_img,  6, 7, 1, 0, 200 },   /* fire/explosion, not the brown CROWN the old cell was */
    { PW_QUAKE,    "QUAKE",    &ui_status_img,  5, 3, 0, 0,  60 },
    { PW_TORNADO,  "TORNADO",  &ui_status_img,  2, 3, 0, 0,  90 },
    { PW_ACID,     "ACID",     &ui_status_img, 10, 5, 2, 1,  40 },
    { PW_FREEZE,   "FREEZE",   &ui_status_img,  6, 3, 3, 1,  30 },
};

static const Power P_LIFE[8] = {
    /* The wheel icon is the race's own FIRST CIVILIAN CELL (see MB_CAST in mb_draw.c),
     * so what you summon looks like what appears. */
    { PW_HUMAN,   "HUMANS",   &characters_img, 4, 1, 0, 0,  20 },
    { PW_ELF,     "ELVES",    &characters_img, 8, 1, 0, 0,  20 },
    { PW_DWARF,   "DWARVES",  &characters_img, 5, 3, 0, 0,  20 },
    { PW_ORC,     "ORCS",     &monsters_img,   7, 4, 0, 0,  20 },
    { PW_VILLAGE, "VILLAGE",  &buildings_img,  2, 2, 0, 0,  90 },
    { PW_HERD,    "HERD",     &animals_img,    0, 0, 1, 0,  10 },
    { PW_WOLVES,  "WOLVES",   &animals_img,    3, 1, 0, 0,  10 },
    { PW_PLANTS,  "PLANTS",   &nature_img,     4, 4, 3, 1,   4 },
};
static const Power P_BLESS[8] = {
    { PW_RAIN,      "RAIN",      &ui_status_img,  8, 3, 4, 1,   6 },
    { PW_FERTILITY, "FERTILITY", &ui_status_img,  9, 5, 4, 0,  25 },
    { PW_HEAL,      "HEAL",      &ui_status_img, 12, 3, 3, 0,  15 },
    { PW_INSPIRE,   "INSPIRE",   &ui_status_img,  3, 3, 0, 0,  60 },
    { PW_PEACE,     "PEACE",     &ui_status_img,  5, 1, 0, 0, 100 },
    { PW_GOLDVEIN,  "GOLD VEIN", &treasure_ore_img, 1, 0, 2, 0,  45 },
    { PW_BLESS,     "BLESS",     &ui_status_img, 10, 1, 3, 0,  80 },
    { PW_RESURRECT, "RESURRECT", &ui_status_img,  0, 5, 2, 0, 120 },
};
static const Power P_CURSE[8] = {
    { PW_PLAGUE,  "PLAGUE",   &ui_status_img, 10, 5, 3, 0,  70 },
    { PW_MADNESS, "MADNESS",  &ui_status_img,  2, 3, 2, 0,  55 },
    { PW_CURSE,   "CURSE",    &ui_status_img, 11, 3, 3, 0,  40 },
    { PW_WEAKEN,  "WEAKEN",   &ui_status_img,  4, 3, 2, 0,  25 },
    { PW_FAMINE,  "FAMINE",   &ui_status_img,  6, 6, 3, 0,  60 },
    { PW_BARREN,  "BARREN",   &ui_status_img, 15, 3, 3, 0,  45 },
    { PW_GRUDGE,  "GRUDGE",   &ui_status_img,  9, 3, 0, 0,  35 },
    { PW_MARK,    "MARK",     &ui_status_img, 11, 1, 2, 0,  30 },
};
/* The seventeen 2x2 kaiju live on the bosses sheet; eight are summonable. */
static const Power P_BEASTS[8] = {
    /* Each icon is the TOP-LEFT cell of that kaiju's 2x2 block on the bosses sheet.
     * They were half-cells of the wrong block before — the "titan" icon was the right
     * half of the great eye, and the "golem" the right half of the maw. */
    /* PRICED AGAINST THE CEILING, which is the thing that actually decides whether a power
     * exists. The reserve caps at 700 now (mb_faith.c) and these were 200-550 against a cap
     * of 400 — half of them unreachable. A beast should be a season's devotion, not a
     * lifetime's: at a couple of hundred Faith a year the dearest is about two years of it. */
    /* The enum is still PW_TITAN/AG_TITAN — renaming it across five files buys nothing —
     * but what it summons is the armoured skeleton at (10,1), which is a general and looks
     * like one. The giant skull it used to be is left on the sheet unused. */
    { PW_TITAN,   "BONE KING",   &bosses_img, 10, 1, 0, 0, 400 },
    { PW_MEDUSA,  "MEDUSA",      &bosses_img,  8, 1, 0, 0, 300 },
    { PW_REAPER,  "REAPER",      &bosses_img,  4, 3, 0, 0, 360 },
    { PW_DRAGON,  "DRAGON",      &bosses_img,  0, 1, 0, 0, 320 },
    { PW_GOLEM,   "GOLEM",       &bosses_img,  2, 1, 0, 0, 240 },
    { PW_SWARM,   "SWARM",       &animals_img, 0, 4, 2, 0, 160 },
    { PW_EYE,     "THE EYE",     &bosses_img,  6, 1, 0, 0, 280 },
    { PW_ANGEL,   "ANGEL",       &bosses_img,  0, 5, 0, 0, 420 },
};

typedef struct { const char *name; const Power *p; } Tab;
static const Tab TABS[] = {
    { "LAND", P_LAND }, { "LIFE", P_LIFE }, { "BLESS", P_BLESS },
    { "CURSE", P_CURSE }, { "WRATH", P_WRATH }, { "BEASTS", P_BEASTS },
};
#define NTAB ((int)(sizeof TABS / sizeof TABS[0]))

/* --- selection state ---------------------------------------------------- */
static int s_tab;
static int s_sel[NTAB];              /* last slot used per tab, remembered */
static int s_wheel;                  /* wheel showing */
static int s_wheel_pick = -1;

static const Power *cur(void) { return &TABS[s_tab].p[s_sel[s_tab]]; }

const char *mb_power_name(void)   { return cur()->name; }
const char *mb_power_tab(void)    { return TABS[s_tab].name; }
int         mb_power_cost(void)   { return cur()->cost; }
int         mb_power_radius(void) { return cur()->radius; }
int         mb_power_brush(void)  { return cur()->brush; }
int         mb_wheel_open(void)   { return s_wheel; }

/* --- terraforming ------------------------------------------------------- */

/* One cell. Separate from the flux rules because terraforming is the player's
 * hand acting instantly, not a process the world runs. */
/* How many people the last cast reached, or -1 when the power does not target
 * people at all. A blessing that catches nobody looks exactly like a blessing that
 * is broken — you spend eighty faith on a village square and the world does not
 * move — so the HUD gets to say "NOBODY THERE" instead of leaving you guessing.
 *
 * The unit-targeting radii were also simply too tight: BLESS and CURSE at radius 1
 * cover five tiles, while a village spreads over seventeen by seventeen, so most
 * casts on the hall genuinely reached no one. They are radius 3 now. */
static int s_last_reach = -1;
int mb_power_last_reach(void) { return s_last_reach; }

static void terra(int x, int y, int id)
{
    if (!mb_in(x, y)) return;
    int i = AT(x, y);
    uint8_t *b = &mb_w.biome[i], *o = &mb_w.obj[i], *e = &mb_w.elev[i];
    switch (id) {
    /* RAISE AND LOWER STEP THE LADDER, which is the only way they read.
     *
     * They used to move elevation by fourteen and change the biome only above 176 or 200.
     * Ordinary grass sits around 150, so a cast on it produced no terrain change at all and
     * the entire feedback was a faint shift in the hillshade — a smudge. Photographed side
     * by side with WATER and DESERT (both instantly obvious) they were the two powers you
     * could not tell had fired.
     *
     * So each cast moves the ground one rung as well as bumping the elevation: sea, shoal,
     * beach, grass, hill, mountain, peak. Two casts make a hill out of a meadow and four
     * make a mountain, which is what a player pushing a brush around expects, and it is the
     * exact mirror of LOWER and of the acid ladder. */
    case PW_RAISE:
        *e = (uint8_t)(*e > 241 ? 255 : *e + 14);
        switch (*b) {
        case B_SEA: case B_OCEAN:      *b = B_SHALLOW; *o = O_NONE; break;
        case B_SHALLOW: case B_ICE:    *b = B_BEACH;   *o = O_NONE; break;
        case B_BEACH:                  *b = B_GRASS;   break;
        case B_GRASS: case B_MEADOW: case B_SAVANNA: case B_DESERT:
        case B_FOREST: case B_SWAMP: case B_FARM: case B_TUNDRA:
                                       *b = B_HILL;    break;
        case B_HILL: case B_RUBBLE: case B_ASH: case B_SCORCHED:
                                       *b = B_MOUNTAIN; break;
        case B_MOUNTAIN:               *b = B_PEAK;    break;
        default: break;                /* a peak is as high as it goes */
        }
        if (*b == B_HILL || *b == B_MOUNTAIN || *b == B_PEAK) {
            /* the elevation has to agree with the biome or the hillshade contradicts it */
            int floor_e = (*b == B_PEAK) ? 220 : (*b == B_MOUNTAIN) ? 200 : 178;
            if (*e < floor_e) *e = (uint8_t)floor_e;
        }
        break;
    case PW_MOUNTAIN:
        *e = (uint8_t)(*e > 215 ? 255 : *e + 40);
        *b = (*e > 224) ? B_PEAK : B_MOUNTAIN;
        *o = ((mb_rand((uint32_t)i) & 7) < 3) ? O_BOULDER : O_NONE;
        break;
    case PW_FOREST:
        if (!mb_land(*b)) break;
        *b = B_FOREST;
        *o = (mb_rand((uint32_t)i * 7u) & 3)
           ? ((mb_rand((uint32_t)i) & 1) ? O_TREE : O_TREE2) : O_NONE;
        break;
    case PW_GRASS:
        if (!mb_land(*b)) break;
        *b = B_GRASS;
        *o = ((mb_rand((uint32_t)i * 11u) & 7) < 2) ? O_TUFT : O_NONE;
        break;
    case PW_LOWER:
        *e = (uint8_t)(*e < 14 ? 0 : *e - 14);
        switch (*b) {                  /* the mirror of RAISE, rung for rung */
        case B_PEAK:                   *b = B_MOUNTAIN; break;
        case B_MOUNTAIN:               *b = B_HILL;     break;
        case B_HILL: case B_TUNDRA: case B_RUBBLE:
                                       *b = B_GRASS;    break;
        case B_GRASS: case B_MEADOW: case B_SAVANNA: case B_DESERT:
        case B_FOREST: case B_SWAMP: case B_FARM: case B_ASH: case B_SCORCHED:
                                       *b = B_BEACH;   *o = O_NONE; break;
        case B_BEACH: case B_SNOW: case B_ICE:
                                       *b = B_SHALLOW; *o = O_NONE; break;
        case B_SHALLOW:                *b = B_SEA;     *o = O_NONE; break;
        default: break;                /* the sea floor is the bottom */
        }
        if (!mb_land(*b) && *e >= mb_w.sea) *e = (uint8_t)(mb_w.sea > 8 ? mb_w.sea - 8 : 0);
        break;
    case PW_WATER:
        *e = (uint8_t)(*e < 20 ? 0 : *e - 20);
        *b = (*e < mb_w.sea - 12) ? B_SEA : B_SHALLOW;
        *o = O_NONE;
        break;
    case PW_DESERT:
        if (!mb_land(*b)) break;
        *b = B_DESERT;
        *o = ((mb_rand((uint32_t)i * 13u) & 15) < 2) ? O_REEDS : O_NONE;
        break;
    case PW_ROAD:
        /* Paving no longer replaces the terrain: a road LIES ON the land, so it goes
         * in its own layer and the grass, sand or field underneath stays. */
        if (!mb_land(*b)) break;
        mb_w.road[i] = 1;
        if (*o && !mb_is_build(*o)) *o = O_NONE;   /* clear the clutter, keep buildings */
        break;
    default: break;
    }
}

/* --- casting ------------------------------------------------------------ */

static void cast_at(int id, int r, int cx, int cy)
{
    switch (id) {

    /* --- LAND: a disc of the same edit ------------------------------- */
    case PW_RAISE: case PW_MOUNTAIN: case PW_FOREST: case PW_GRASS:
    case PW_LOWER: case PW_WATER: case PW_DESERT: case PW_ROAD:
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) continue;
                terra(x, y, id);
            }
        mb_fx_burst((float)cx, (float)cy, 5, PK_STAR, FXE_HOLY, 2.0f, 0.35f);
        break;

    /* --- WRATH ------------------------------------------------------- */
    case PW_FIRE:
        mb_snd(SND_FIRE);
        mb_flux_ignite(cx, cy, r);
        mb_fx_burst((float)cx, (float)cy, 6, PK_SPARK, FXE_FIRE, 2.5f, 0.5f);
        break;

    /* LIGHTNING HAS TO LEAVE A MARK. One bolt particle three cells up, one impact puff,
     * and an ignite on a single cell: the particles are gone within a tick and a single
     * cell in a paved town has nothing to catch, so a strike photographed as absolutely
     * nothing — the least legible power on the wheel by a wide margin.
     *
     * A strike now SCORCHES the cell it hits, which is permanent and unmistakable, throws
     * the bolt as a column rather than a single spark so the flash reads as a bolt, and
     * lights a small radius so there is usually a fire to deal with afterwards. */
    case PW_LIGHTNING: {
        mb_snd(SND_THUNDER);
        for (int k = 0; k < 7; k++)      /* the bolt, drawn down the sky */
            mb_fx_spawn((float)cx, (float)cy - 7.0f + (float)k, PK_BOLT, FXE_HOLY,
                        0.0f, 0.22f + 0.03f * (float)k);
        mb_fx_impact((float)cx, (float)cy, FXE_FIRE, 0.9f);
        if (mb_in(cx, cy) && mb_land(mb_w.biome[AT(cx, cy)])) {
            mb_w.biome[AT(cx, cy)] = B_SCORCHED;
            if (!mb_is_build(mb_w.obj[AT(cx, cy)])) mb_w.obj[AT(cx, cy)] = O_NONE;
        }
        mb_unit_area(cx, cy, 1, UAP_KILL, CAUSE_DISASTER);
        /* radius ONE, not two. At two the strike lit twenty-five cells at once and a
         * twelve-faith cast took the whole neighbourhood — the scorch mark is what makes it
         * legible, so the fire does not have to be enormous as well. */
        mb_flux_ignite(cx, cy, 1);
        break;
    }

    case PW_METEOR:
        mb_snd(SND_BOOM);
        for (int y = cy - r - 2; y <= cy + r + 2; y++)
            for (int x = cx - r - 2; x <= cx + r + 2; x++) {
                if (!mb_in(x, y)) continue;
                int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy, i = AT(x, y);
                if (d2 <= r * r) {                              /* the crater */
                    if (mb_land(mb_w.biome[i])) mb_w.biome[i] = B_SCORCHED;
                    mb_w.obj[i] = O_NONE;
                    mb_w.elev[i] = (uint8_t)(mb_w.elev[i] < 8 ? 0 : mb_w.elev[i] - 8);
                } else if (d2 <= (r + 2) * (r + 2)) {           /* the firestorm */
                    mb_flux_ignite(x, y, 0);
                }
            }
        mb_fx_impact((float)cx, (float)cy, FXE_FIRE, 1.5f);
        mb_fx_burst((float)cx, (float)cy, 16, PK_SMOKE, FXE_ASH, 3.0f, 1.6f);
        break;

    case PW_VOLCANO:
        mb_snd(SND_BOOM);
        mb_agent_spawn(AG_VENT, cx, cy);
        mb_fx_impact((float)cx, (float)cy, FXE_FIRE, 1.1f);
        break;

    case PW_QUAKE: {
        /* A fissure, not a circle: a torn line reads as a fault and shows which
         * way the ground moved, which a disc never does. */
        uint32_t rr = mb_rand((uint32_t)AT(cx, cy) * 31u);
        static const int8_t FDX[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
        static const int8_t FDY[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        int ang = (int)(rr & 7), x = cx, y = cy, len = 10 + (int)((rr >> 3) & 7);
        for (int s = 0; s < len; s++) {
            for (int w = -1; w <= 1; w++) {
                int px = x + (FDY[ang] ? w : 0), py = y + (FDX[ang] ? w : 0);
                if (!mb_in(px, py) || !mb_land(mb_w.biome[AT(px, py)])) continue;
                /* the flanks tear too, half the time: a one-tile line was a
                 * scratch at 1 px/tile, and a fault should read as a fault */
                if (w == 0) { mb_w.biome[AT(px, py)] = B_RUBBLE; mb_w.obj[AT(px, py)] = O_NONE; }
                else if (((rr >> (s & 15)) & 1) == 0) {
                    mb_w.biome[AT(px, py)] = B_RUBBLE;
                    mb_w.obj[AT(px, py)] = (((rr >> (s & 7)) & 3) == 0) ? O_ROCK : O_NONE;
                }
            }
            x += FDX[ang]; y += FDY[ang];
            if ((s & 3) == 3) ang = (ang + 8 + (int)(((rr >> s) & 3) - 1)) & 7;   /* it wanders */
        }
        mb_fx_impact((float)cx, (float)cy, FXE_ASH, 1.3f);
        mb_fx_burst((float)cx, (float)cy, 14, PK_SMOKE, FXE_ASH, 2.0f, 1.3f);
        break;
    }

    case PW_TORNADO:
        mb_snd(SND_QUAKE);
        mb_agent_spawn(AG_TORNADO, cx, cy);
        mb_fx_burst((float)cx, (float)cy, 10, PK_GUST, FXE_ASH, 3.0f, 0.8f);
        break;

    case PW_ACID:
        mb_snd(SND_CURSE);
        mb_flux_blob(cx, cy, r, FX_ACID, 12);
        mb_fx_burst((float)cx, (float)cy, 8, PK_SPARK, FXE_ACID, 2.0f, 0.7f);
        break;

    case PW_FREEZE:
        mb_snd(SND_FREEZE);
        mb_flux_blob(cx, cy, r, FX_FROST, 11);
        mb_fx_burst((float)cx, (float)cy, 10, PK_STAR, FXE_FROST, 1.8f, 0.9f);
        break;

    /* --- LIFE -------------------------------------------------------- */
    case PW_HUMAN: case PW_ELF: case PW_DWARF: case PW_ORC: {
        int sp = SP_HUMAN + (id - PW_HUMAN);
        for (int i = 0; i < 4; i++) {
            uint32_t rr = mb_rand((uint32_t)(i * 733u + (uint32_t)mb_w.tick));
            mb_unit_spawn(sp, cx + (int)(rr % 5) - 2, cy + (int)((rr >> 5) % 5) - 2);
        }
        mb_fx_burst((float)cx, (float)cy, 8, PK_STAR, FXE_HOLY, 2.0f, 0.6f);
        break;
    }
    case PW_VILLAGE:
        /* A founding party. The ground has to allow it — WorldBox's own rule —
         * so this can legitimately do nothing, and says so with a dud sparkle. */
        if (mb_civ_drop_village(SP_HUMAN + (int)(mb_rand((uint32_t)mb_w.tick) % 4), cx, cy))
            mb_fx_burst((float)cx, (float)cy, 14, PK_STAR, FXE_HOLY, 3.0f, 0.9f);
        else
            mb_fx_burst((float)cx, (float)cy, 4, PK_SMOKE, FXE_ASH, 1.0f, 0.5f);
        break;
    case PW_HERD:
        for (int i = 0; i < 6; i++) {
            uint32_t rr = mb_rand((uint32_t)(i * 911u + (uint32_t)mb_w.tick));
            static const uint8_t HERD[4] = { SP_DEER, SP_SHEEP, SP_BOAR, SP_HEN };
            mb_unit_spawn(HERD[rr & 3], cx + (int)((rr >> 3) % 7) - 3, cy + (int)((rr >> 7) % 7) - 3);
        }
        break;
    case PW_WOLVES:
        for (int i = 0; i < 3; i++) {
            uint32_t rr = mb_rand((uint32_t)(i * 313u + (uint32_t)mb_w.tick));
            mb_unit_spawn((rr & 3) ? SP_WOLF : SP_DOG, cx + (int)((rr >> 3) % 5) - 2,
                          cy + (int)((rr >> 7) % 5) - 2);
        }
        break;
    case PW_PLANTS:
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (!mb_in(x, y) || (x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
                if (!mb_land(mb_w.biome[AT(x, y)]) || mb_w.obj[AT(x, y)]) continue;
                uint32_t rr = mb_rand((uint32_t)AT(x, y) * 51u);
                mb_w.obj[AT(x, y)] = (rr & 3) ? O_BUSH : ((rr & 4) ? O_TREE : O_TUFT);
            }
        break;

    /* --- BLESS ------------------------------------------------------- */
    case PW_RAIN:
        mb_snd(SND_SPLASH);
        /* water flux with a short life: extinguishes fire, greens the ground,
         * and does not flood — the counter to a firestorm */
        mb_flux_blob(cx, cy, r, FX_WATER, 4);
        mb_fx_burst((float)cx, (float)cy, 12, PK_SMOKE, FXE_FROST, 1.5f, 1.0f);
        break;
    case PW_FERTILITY:
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (!mb_in(x, y) || (x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
                uint8_t *b = &mb_w.biome[AT(x, y)];
                if (*b == B_ASH || *b == B_SCORCHED || *b == B_RUBBLE) *b = B_GRASS;
                else if (*b == B_GRASS || *b == B_SAVANNA) *b = B_MEADOW;
                else if (*b == B_MEADOW) *b = B_FOREST;
            }
        mb_fx_burst((float)cx, (float)cy, 12, PK_STAR, FXE_ACID, 1.6f, 0.9f);
        break;
    case PW_HEAL:
        s_last_reach = mb_unit_area(cx, cy, r, UAP_HEAL, 0);
        mb_fx_burst((float)cx, (float)cy, 10, PK_STAR, FXE_HOLY, 1.5f, 0.8f);
        break;
    case PW_INSPIRE: {
        int v = mb_w.claim[AT(cx, cy)];
        if (v) { mb_k[mb_kingdom_of(v)].tech++; mb_chron_disaster("inspiration", cx, cy); }
        mb_fx_burst((float)cx, (float)cy, 10, PK_BOLT, FXE_HOLY, 2.0f, 0.6f);
        break;
    }
    case PW_PEACE:
        for (int a = 1; a < MAXK; a++) { mb_k[a].war_with = 0; mb_k[a].exhaustion = 0; }
        for (int v = 1; v < MAXV; v++) mb_v[v].mustering = 0;
        mb_chron_disaster("a great peace", cx, cy);
        mb_fx_burst((float)cx, (float)cy, 16, PK_RING, FXE_HOLY, 0.5f, 1.2f);
        break;
    case PW_GOLDVEIN:
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (!mb_in(x, y) || (x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
                if (!mb_land(mb_w.biome[AT(x, y)])) continue;
                uint32_t rr = mb_rand((uint32_t)AT(x, y) * 97u);
                mb_w.obj[AT(x, y)] = (rr & 1) ? O_GOLD : ((rr & 2) ? O_GEM : O_SILVER);
            }
        break;
    case PW_BLESS:      s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_TRAIT, TR_BLESSED); break;
    case PW_RESURRECT:
        /* graves give their dead back, which is the one power that undoes a
         * disaster after the fact */
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (!mb_in(x, y) || mb_w.obj[AT(x, y)] != O_GRAVE) continue;
                mb_w.obj[AT(x, y)] = O_NONE;
                int u = mb_unit_spawn(SP_HUMAN, x, y);
                if (u >= 0) mb_u[u].traits |= TR_BLESSED;
            }
        mb_fx_burst((float)cx, (float)cy, 14, PK_STAR, FXE_HOLY, 2.0f, 1.1f);
        break;

    /* --- CURSE ------------------------------------------------------- */
    case PW_PLAGUE:   s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_TRAIT, TR_PLAGUE | TR_CONTAGIOUS); break;
    case PW_MADNESS:  s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_TRAIT, TR_MADNESS); break;
    case PW_CURSE:    s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_TRAIT, TR_CURSED); break;
    case PW_WEAKEN:   s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_HURT, 40); break;
    case PW_FAMINE:
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (!mb_in(x, y)) continue;
                if (mb_w.biome[AT(x, y)] == B_FARM) mb_w.biome[AT(x, y)] = B_SAVANNA;
                uint8_t *o = &mb_w.obj[AT(x, y)];
                if (*o == O_BUSH || *o == O_FLOWER || *o == O_TUFT) *o = O_NONE;
            }
        s_last_reach = mb_unit_area(cx, cy, r, UAP_STARVE, 90);
        break;
    case PW_BARREN:   s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_TRAIT, TR_BARREN); break;
    case PW_GRUDGE: {
        /* set two kingdoms at each other: the player as agent provocateur */
        int v = mb_w.claim[AT(cx, cy)], k = mb_kingdom_of(v);
        if (k) for (int o = 1; o < MAXK; o++)
            if (o != k && mb_k[o].alive && mb_border_len(k, o)) {
                mb_k[k].war_with |= (uint32_t)1u << o;
                mb_k[o].war_with |= (uint32_t)1u << k;
                mb_chron_war(k, o);
                break;
            }
        break;
    }
    case PW_MARK:     s_last_reach = mb_unit_area(cx, cy, r + 1, UAP_TRAIT, TR_MARKED); break;

    /* --- BEASTS: the kaiju ------------------------------------------- */
    case PW_TITAN: case PW_MEDUSA: case PW_REAPER: case PW_DRAGON:
    case PW_GOLEM: case PW_EYE:   case PW_ANGEL: {
        /* AN EXPLICIT MAP, because arithmetic across two enums is exactly the bug this
         * file's own header warns about. PW_SWARM sits between GOLEM and EYE and has no
         * agent of its own, so `AG_KAIJU0 + (id - PW_TITAN)` was off by one from EYE
         * onward: casting THE EYE summoned an ANGEL, and casting ANGEL — a 550-faith
         * summon — opened THE MAW, the endgame void that never stops. Four hundred years
         * of a world could be ended by picking the wrong arm of the wheel. */
        static const uint8_t KAIJU_OF[] = {
            [PW_TITAN - PW_TITAN]   = AG_TITAN,
            [PW_MEDUSA - PW_TITAN]  = AG_MEDUSA,
            [PW_REAPER - PW_TITAN]  = AG_REAPER,
            [PW_DRAGON - PW_TITAN] = AG_DRAGON,
            [PW_GOLEM - PW_TITAN]   = AG_GOLEM,
            [PW_SWARM - PW_TITAN]   = AG_N,        /* not an agent; handled below */
            [PW_EYE - PW_TITAN]     = AG_EYE,
            [PW_ANGEL - PW_TITAN]   = AG_ANGEL,
        };
        int k = KAIJU_OF[id - PW_TITAN];
        if (k < AG_N) mb_agent_spawn(k, cx, cy);
        break;
    }
        mb_fx_impact((float)cx, (float)cy, FXE_VOID, 1.2f);
        break;
    case PW_SWARM:
        for (int i = 0; i < 12; i++) {
            uint32_t rr = mb_rand((uint32_t)(i * 577u + (uint32_t)mb_w.tick));
            mb_unit_spawn(SP_BAT, cx + (int)(rr % 7) - 3, cy + (int)((rr >> 4) % 7) - 3);
        }
        mb_chron_disaster("a swarm", cx, cy);
        break;

    default: break;
    }
}

void mb_power_cast(int cx, int cy)
{
    const Power *p = cur();
    s_last_reach = -1;
    /* the blessings and the curses share two sounds; the disasters have their own */
    if (p->id >= PW_RAIN && p->id <= PW_RESURRECT) mb_snd(SND_BLESS);
    else if (p->id >= PW_PLAGUE && p->id <= PW_MARK) mb_snd(SND_CURSE);
    else if (p->id >= PW_TITAN) mb_snd(SND_TITAN);
    cast_at(p->id, p->radius, cx, cy);
}

/* Cast by NAME, for the headless hooks (MOTEBOX_CAST=fire@70,60). Testing a
 * disaster by driving the wheel with scripted key presses tests the wheel, not
 * the disaster — and it cannot aim, which is how the first fire test ended up on
 * bare mountain where there is nothing to burn. */
int mb_power_cast_named(const char *name, int cx, int cy)
{
    for (int t = 0; t < NTAB; t++)
        for (int i = 0; i < 8; i++) {
            const Power *p = &TABS[t].p[i];
            const char *a = name, *b = p->name;
            while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
            if (*a || *b) continue;
            cast_at(p->id, p->radius, cx, cy);
            return 1;
        }
    return 0;
}

#if MOTE_HOST
#include <stdio.h>
/* Every power, cast in turn, with the world state before and after.
 *
 * Forty-eight powers in six tabs is far too many to check by hand, and a power that
 * silently does nothing looks exactly like one that is working — the wheel still
 * names it, the faith is still spent. So this walks the whole table, casts each one
 * on a target that has something for it to act on, ticks the world on so the slow
 * ones (a spreading fire, a walking titan) have a chance to show, and prints what
 * measurably changed. A row of zeroes is the bug report.
 */
typedef struct {
    long elev, bio, obj, grave, flux, units, hp, sick, traits, happy;
    long villages, vfood, vhappy, vhouse, kingdoms, warbits, agents, faith, tech, muster;
} PwSnap;

static void pw_snap(PwSnap *s)
{
    memset(s, 0, sizeof *s);
    for (int c = 0; c < NC; c++) {
        s->elev += mb_w.elev[c];
        s->bio  += mb_w.biome[c] * (c % 7 + 1);      /* order-sensitive: any change shows */
        if (mb_w.obj[c]) s->obj++;
        if (mb_w.obj[c] == O_GRAVE) s->grave++;
        if (mb_fkind(mb_w.flux[c])) s->flux++;
    }
    for (int k = 0; k < mb_nu; k++) {
        if (!mb_u[k].alive) continue;
        s->units++; s->hp += mb_u[k].hp; s->traits += mb_u[k].traits;
        s->happy += mb_u[k].happy;
        if (mb_u[k].sick) s->sick++;
    }
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive) continue;
        s->villages++; s->vfood += mb_v[v].food; s->vhappy += mb_v[v].happy;
        s->vhouse += mb_v[v].housing; s->muster += mb_v[v].mustering;
    }
    for (int k = 1; k < MAXK; k++) {
        if (!mb_k[k].alive) continue;
        s->kingdoms++; s->tech += mb_k[k].tech;
        for (uint32_t w = mb_k[k].war_with; w; w >>= 1) s->warbits += (w & 1);
    }
    s->agents = mb_agent_count();
    s->faith  = mb_faith();
}

/* Every power, cast in turn, with the world state before and after.
 *
 * Forty-eight powers in six tabs is far too many to check by hand, and a power that
 * silently does nothing looks exactly like one that works — the wheel still names it,
 * the faith is still spent. So this walks the whole table, casts each on a target that
 * has something to act on, ticks the world so the slow ones (a spreading fire, a
 * walking titan) get to show, and prints every field that moved. A row of dashes is
 * the bug report.
 *
 * The FIRST version of this measured only biome, objects, flux, units, hp and sick,
 * and reported 17 of 48 dead — most of which were the harness failing to look at the
 * right field (elevation for RAISE, a village ledger for FAMINE, a kingdom's war bits
 * for GRUDGE) rather than the power failing. A test that cannot see the effect is not
 * evidence of no effect.
 */
void mb_power_test_all(int cx, int cy)
{
    /* TWO TARGETS PER POWER, and the report is the better of the two.
     *
     * The first run of this cast everything on the village centre and called ten
     * powers dead. But a village square is paved, built on and treeless — the worst
     * possible target for FIRE (nothing to burn), ROAD (already a road) and VILLAGE
     * (one is already here). "Nothing to act on" and "broken" produce identical
     * output, so each power is tried on the town AND on wild vegetated ground. */
    fprintf(stderr, "town target %d,%d; each power also gets its own untouched wild tile\n",
            cx, cy);

    int fails = 0;
    for (int t = 0; t < NTAB; t++) {
        fprintf(stderr, "\n== %s ==\n", TABS[t].name);
        for (int i = 0; i < 8; i++) {
            const Power *p = &TABS[t].p[i];
            PwSnap a, b;

            /* A FRESH LIVING VILLAGE PER POWER, as well as a fresh wild tile. The
             * town target was fixed for the whole run, and the LAND tab turns it into
             * mountains, water and desert before BLESS ever gets there — so BLESS was
             * blessing a crater. Anything that acts on PEOPLE needs a town that still
             * has people in it. */
            int ncand = 0, cand[MAXV];
            for (int v = 1; v < MAXV; v++)
                if (mb_v[v].alive && mb_v[v].pop > 0) cand[ncand++] = v;
            int tcx = cx, tcy = cy;
            if (ncand) {
                int pick = cand[(t * 8 + i) % ncand];
                tcx = mb_v[pick].x; tcy = mb_v[pick].y;
            }

            /* A FRESH WILD TILE PER POWER. Sharing one made the report a lie: the
             * forty-eight casts mutate the same ground, so by the time WRATH ran, the
             * wild tile had already been terraformed, burned, frozen, acid-bathed and
             * meteored — and LIGHTNING "did nothing" because FIRE, two rows earlier,
             * had already burnt everything there was to burn. */
            int wx = cx, wy = cy;
            for (int step = 0; step < 6000; step++) {
                uint32_t rr = mb_rand((uint32_t)(step * 2654435761u
                                                 + (uint32_t)(t * 8 + i) * 7919u + 0x7357u));
                int x = (int)(rr % MW), y = (int)((rr >> 11) % MH);
                if (!mb_in(x, y)) continue;
                uint8_t bb = mb_w.biome[AT(x, y)];
                if ((bb == B_FOREST || bb == B_GRASS || bb == B_MEADOW)
                    && !mb_w.claim[AT(x, y)] && !mb_fkind(mb_w.flux[AT(x, y)])) {
                    wx = x; wy = y; break;
                }
            }

            /* PRECONDITIONS. A power that undoes a state needs that state to exist, or
             * it correctly does nothing and the test learns nothing. PEACE ends wars,
             * so give it a war; it sits in BLESS and the power that starts wars sits
             * two tabs later, which is the only reason it looked broken. */
            if (p->id == PW_GRUDGE) {
                /* GRUDGE sets two BORDERING kingdoms at each other, so it needs a pair
                 * whose territory touches. Without one it correctly does nothing, and
                 * the test would be reporting the world's shape as a bug. */
                int k1 = 0, k2 = 0;
                for (int k = 1; k < MAXK; k++)
                    if (mb_k[k].alive) { if (!k1) k1 = k; else if (!k2) k2 = k; }
                if (k1 && k2 && !mb_border_len(k1, k2))
                    for (int c = 0; c < NC && !mb_border_len(k1, k2); c++)
                        if (mb_w.claim[c] && mb_kingdom_of(mb_w.claim[c]) == k1) {
                            int x = c % MW, y = c / MW;
                            for (int v = 1; v < MAXV; v++)
                                if (mb_v[v].alive && mb_v[v].kingdom == k2 && mb_in(x + 1, y)) {
                                    mb_w.claim[AT(x + 1, y)] = (uint8_t)v; break;
                                }
                        }
            }
            if (p->id == PW_PEACE) {
                int n = 0;
                for (int k = 1; k < MAXK && n < 2; k++)
                    if (mb_k[k].alive) { mb_k[k].war_with |= 0xEu; n++; }
                for (int v = 1; v < MAXV; v++) if (mb_v[v].alive) mb_v[v].mustering = 3;
            }

            /* Give the power something to work on, so "nothing happened" means the
             * power is broken and not that the target was bare ground: wound a few
             * people (HEAL), sicken none (PLAGUE must do that itself), and make sure
             * there is a grave in reach (RESURRECT). */
            for (int k = 0, n = 0; k < mb_nu && n < 6; k++)
                if (mb_u[k].alive && mb_u[k].hp > 40) { mb_u[k].hp -= 20; n++; }
            if (mb_in(tcx + 1, tcy) && mb_land(mb_w.biome[AT(tcx + 1, tcy)])
                && !mb_w.obj[AT(tcx + 1, tcy)])
                mb_w.obj[AT(tcx + 1, tcy)] = O_GRAVE;

            char out[220]; int o = 0;
            for (int pass = 0; pass < 2 && !o; pass++) {
                int tx = pass ? wx : tcx, ty = pass ? wy : tcy;
                /* Faith is topped up BEFORE the snapshot, so the only faith movement
                 * the report can show is the cast's own cost — or a power that GRANTS
                 * faith, which is a real effect worth seeing. */
                mb_faith_set(4000);
                pw_snap(&a);
                cast_at(p->id, p->radius, tx, ty);
                /* mb_flux_step walks the agents too, so slow disasters get to move */
                for (int n = 0; n < 24; n++) mb_flux_step();
                pw_snap(&b);
                b.faith = a.faith;         /* every cast spends; that is not an effect */
                #define FLD(name, f) do { long d = b.f - a.f; if (d) \
                    o += snprintf(out + o, sizeof out - o, " %s%+ld", name, d); } while (0)
            FLD("elev", elev); FLD("terrain", bio); FLD("obj", obj); FLD("graves", grave);
            FLD("flux", flux); FLD("units", units); FLD("hp", hp); FLD("sick", sick);
            FLD("traits", traits); FLD("mood", happy); FLD("villages", villages);
            FLD("food", vfood); FLD("vmood", vhappy); FLD("beds", vhouse);
                FLD("kingdoms", kingdoms); FLD("wars", warbits); FLD("monsters", agents);
                FLD("tech", tech); FLD("mustering", muster); FLD("faith", faith);
                #undef FLD
                if (o) o += snprintf(out + o, sizeof out - o, "   [%s]",
                                     pass ? "wild" : "town");
            }
            if (!o) { snprintf(out, sizeof out, "  --- NOTHING CHANGED ---"); fails++; }
            fprintf(stderr, "  %-11s %4d faith r%d %s\n", p->name, p->cost, p->radius, out);
        }
    }
    fprintf(stderr, "\n%d of %d powers changed nothing measurable\n", fails, NTAB * 8);
}
#endif

/* --- input -------------------------------------------------------------- */

/* Arm order N, E, S, W — the four the hardware can actually report. */
static int pressed_arm(const MoteInput *in)
{
    if (mote_just_pressed(in, MOTE_BTN_UP))    return 0;
    if (mote_just_pressed(in, MOTE_BTN_RIGHT)) return 1;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))  return 2;
    if (mote_just_pressed(in, MOTE_BTN_LEFT))  return 3;
    return -1;
}

/* Returns 1 while the wheel owns the d-pad, so the cursor does not move. */
int mb_power_input(const MoteInput *in)
{
    int lb = mote_pressed(in, MOTE_BTN_LB);
    if (lb && !s_wheel) {
        s_wheel = 1;
        s_wheel_pick = s_sel[s_tab];      /* open on what you last used */
    }
    if (!s_wheel) return 0;

    /* A GRID CURSOR. The radial version asked you to learn that a direction pressed ONCE
     * meant the near slot and pressed AGAIN meant the far one on the same arm — eight powers
     * behind four directions with a hidden second state each, and six tabs behind a shoulder
     * button. It worked and nobody could read it.
     *
     * Now the d-pad moves a cursor over a 4x2 page of icons, which is the arrangement every
     * console menu has used for thirty years. Running off the left or right edge PAGES to the
     * neighbouring tab and enters it from the opposite side, so all forty-eight powers are
     * reachable by holding one direction — and RB still jumps a whole page for anyone who
     * knows where they are going. */
    int arm = pressed_arm(in);
    if (arm >= 0) {
        int col = s_wheel_pick & 3, row = (s_wheel_pick >> 2) & 1;
        switch (arm) {
        case 0: case 2: row ^= 1; break;                       /* two rows: either flips */
        case 1:
            if (col < 3) col++;
            else { s_tab = (s_tab + 1) % NTAB; col = 0; }
            break;
        default:
            if (col > 0) col--;
            else { s_tab = (s_tab + NTAB - 1) % NTAB; col = 3; }
            break;
        }
        s_wheel_pick = row * 4 + col;
    }
    if (mote_just_pressed(in, MOTE_BTN_RB)) {
        s_tab = (s_tab + 1) % NTAB;
        s_wheel_pick = s_sel[s_tab];
    }

    if (!lb) {                            /* released: commit */
        if (s_wheel_pick >= 0) s_sel[s_tab] = s_wheel_pick;
        s_wheel = 0; s_wheel_pick = -1;
    }
    return 1;
}

/* --- the wheel ---------------------------------------------------------- */

#define WCX 64
#define WCY (VIEW_H / 2)
#define WR_ARROW 12          /* the direction hint, just off the hub */
#define WR_NEAR  23
#define WR_FAR   37
static const int8_t ARMX[4] = {  0, 1, 0, -1 };
static const int8_t ARMY[4] = { -1, 0, 1,  0 };

/* The four cardinal arrows out of ui_gauges, cream when live and grey when not:
 * "arrow (up, cream)" [1,1] · right [2,2] · down [1,3] · left [0,2], and the grey
 * set sits three columns over. The hub is its "ring/target icon". */
static const uint8_t ARROW_CX[4] = { 1, 2, 1, 0 };
static const uint8_t ARROW_CY[4] = { 1, 2, 3, 2 };

/* ui_buttons "blank button (white)" [0,2] and "blank button (grey)" [0,3] — a real
 * drawn disc to seat each icon on. The first version of this wheel seated them on
 * draw_rect boxes, which is why it looked like a debug overlay. */
#define BTN_LIT_CX 0
#define BTN_LIT_CY 2
#define BTN_DIM_CX 0
#define BTN_DIM_CY 3

static void slot_pos(int arm, int far, int *ox, int *oy)
{
    int r = far ? WR_FAR : WR_NEAR;
    *ox = WCX + ARMX[arm] * r - 4;
    *oy = WCY + ARMY[arm] * r - 4;
}

void mb_power_draw_wheel(uint16_t *fb, const MoteFont *font)
{
    if (!s_wheel) return;
    (void)font;
    const Power *tab = TABS[s_tab].p;
    const uint16_t FILL = MOTE_RGB565(20, 18, 40);
    const uint16_t LINE = MOTE_RGB565(180, 170, 240);

    /* THE SAME PANEL AS EVERY OTHER SCREEN. The first replacement for the radial wheel was a
     * bare grid of icons on a dimmed rectangle — the navigation was right and it looked like a
     * debug overlay, because it shared nothing with the information screens: no frame, no
     * hatch, no section rules, and icons at their world size of eight pixels.
     *
     * So it is built from mb_ui: the CP437 frame, the hatched interior, a titled rule, and the
     * SELECTED power drawn at triple size on the master's plaque — because the one you are
     * about to spend Faith on should be the biggest thing on the screen. */
    mb_ui_panel(fb, 0, 0, 128, 128, LINE, FILL, 1);

    int pick = (s_wheel_pick >= 0) ? s_wheel_pick : s_sel[s_tab];
    if (pick < 0 || pick > 7) pick = 0;
    const Power *p = &tab[pick];

    /* the page, and where it sits in the six */
    mb_ui_text(fb, 7, 6, TABS[s_tab].name, MB_UI_CREAM, 70);
    for (int t = 0; t < NTAB; t++)
        g_api->draw_rect(fb, 84 + t * 6, 8, 4, 4,
                         t == s_tab ? MB_UI_GOLD : MOTE_RGB565(70, 64, 96), 1, 0, 128);

    /* the chosen power, large, with its name and price */
    mb_ui_plaque(fb, 5, 18, 32, 32, MOTE_RGB565(12, 10, 26));
    g_api->blit_ex(fb, p->icon, 21.0f, 34.0f, p->ix * TILE, p->iy * TILE, TILE, TILE,
                   0.0f, 3.0f, 0, 0, 128);
    mb_ui_text(fb, 42, 20, p->name, MB_UI_CREAM, 80);
    { char buf[24];
      snprintf(buf, sizeof buf, "%d faith", (int)p->cost);
      mb_ui_text(fb, 42, 30, buf, mb_faith_afford(p->cost) ? MB_UI_GOLD : MB_UI_RED, 80); }
    if (p->radius) {
        char buf[24];
        snprintf(buf, sizeof buf, "reach %d", (int)p->radius);
        mb_ui_text(fb, 42, 40, buf, MB_UI_DIM, 80);
    } else if (p->brush) {
        mb_ui_text(fb, 42, 40, "a brush", MB_UI_DIM, 80);
    }

    /* and the page it came from, four across and two down */
    mb_ui_rule(fb, 6, 54, 116, "POWERS", LINE, FILL, MB_UI_DIM);
    const int CW = 27, CH = 22, GX = 64 - (4 * CW) / 2 + (CW - TILE) / 2, GY = 64;
    for (int i = 0; i < 8; i++) {
        int col = i & 3, row = i >> 2;
        int x = GX + col * CW, y = GY + row * CH;
        int on = (i == pick);
        g_api->blit(fb, &ui_buttons_img, x, y,
                    (on ? BTN_LIT_CX : BTN_DIM_CX) * TILE,
                    (on ? BTN_LIT_CY : BTN_DIM_CY) * TILE, TILE, TILE, 0, 0, 128);
        g_api->blit(fb, tab[i].icon, x, y, tab[i].ix * TILE, tab[i].iy * TILE,
                    TILE, TILE, 0, 0, 128);
        if (on) {
            g_api->draw_rect(fb, x - 2, y - 2, TILE + 4, TILE + 4, MB_UI_GOLD, 0, 0, 128);
            g_api->draw_rect(fb, x - 3, y - 3, TILE + 6, TILE + 6, FILL, 0, 0, 128);
        }
        /* a power you cannot pay for says so here rather than on the cast */
        if (!mb_faith_afford(tab[i].cost))
            mb_dim_rect(fb, x - 1, y - 1, TILE + 2, TILE + 2, FILL, 140);
    }
    mb_ui_actions(fb, "LB HOLD  RB PAGE", 0, FILL);
}
