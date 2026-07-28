/*
 * Motebox — the eight ages, and the world laws.
 *
 * An age is a set of global modifiers plus a palette shift, chosen by a weighted
 * roll off the world's own state and lasting at least thirty years (DESIGN.md 5).
 * They are the sim's seasons: the thing that stops a stable world from being a
 * boring one, and they cost one struct lookup per query.
 *
 * The laws are the other half of the same idea — WorldBox's most-loved menu, and
 * how a sandbox becomes personal. Cheap to build, so they exist from the start
 * rather than being promised.
 */
#include "mb.h"

/* --- the ages ----------------------------------------------------------- */

typedef struct {
    const char *name;
    int8_t  war_bias;      /* added to every kingdom's war score */
    uint8_t armies;        /* 0 = nobody musters (Age of Ash) */
    uint8_t fertility;     /* percent: 130 = a third more births */
    uint8_t dryness;       /* fire risk: 0 none, 255 tinderbox */
    uint8_t cold;          /* frost risk */
    int8_t  faith;         /* percent modifier on Faith income */
    uint16_t tint;         /* a wash the God's Eye pass blends over the world */
    uint8_t  tint_amt;     /* 0 = none */
} Age;

/* Tints are one of the master's own palette colours, so an age recolours the
 * world without ever leaving the sixteen the artist used. */
#define T_NONE   0
#define T_WARM   MOTE_RGB565(255, 163,   0)
#define T_COLD   MOTE_RGB565( 41, 173, 255)
#define T_WHITE  MOTE_RGB565(255, 241, 232)
#define T_GREY   MOTE_RGB565( 95,  87,  79)
#define T_DARK   MOTE_RGB565( 29,  43,  83)
#define T_RED    MOTE_RGB565(255,   0,  77)

static const Age AGES[AGE_N] = {
/*   name           war  arm fert  dry cold faith  tint     amt */
    { "AGE OF HOPE",  -40, 1, 130,  40,  40,  10, T_NONE,   0 },
    { "AGE OF SUN",     0, 1, 100, 210,   0,   0, T_WARM,  40 },
    { "AGE OF MOON",  -10, 1, 110,  40,  60,  50, T_COLD,  50 },
    { "AGE OF IRON",   25, 1,  95,  60,  40, -10, T_GREY,  25 },
    { "AGE OF CHAOS",  60, 1,  80,  90,  40, -20, T_RED,   45 },
    { "AGE OF ICE",   -10, 1,  70,  10, 140,  10, T_WHITE, 55 },
    { "AGE OF ASH",     0, 0,  60, 120,  60, -30, T_GREY,  75 },
    { "AGE OF DESPAIR", 20, 0, 45,  20, 150, -40, T_DARK,  85 },
};

static int   s_age;
static int32_t s_age_started;

void mb_age_init(void) { s_age = AGE_HOPE; s_age_started = 0; }

const char *mb_age_name(void)        { return AGES[s_age].name; }
int  mb_age_id(void)                 { return s_age; }
int  mb_age_war_bias(void)           { return AGES[s_age].war_bias; }
int  mb_age_allows_armies(void)      { return AGES[s_age].armies; }
int  mb_age_fertility(void)          { return AGES[s_age].fertility; }
int  mb_age_dryness(void)            { return AGES[s_age].dryness; }
int  mb_age_cold(void)               { return AGES[s_age].cold; }
int  mb_age_faith_mod(void)          { return AGES[s_age].faith; }
uint16_t mb_age_tint(int *amt)       { *amt = AGES[s_age].tint_amt; return AGES[s_age].tint; }

/* The roll. Weighted by what the world is actually doing, exactly as WorldBox
 * gates its disasters: a burnt, warring world is far more likely to fall into Ash
 * than into Hope, so an age reads as a consequence rather than a dice roll. */
void mb_age_step(void)
{
    if (!mb_law(LAW_AGES)) return;
    if (mb_w.tick - s_age_started < 30 * 52) return;          /* at least 30 years */
    if ((mb_w.tick % 52) != 0) return;                        /* check once a year */
    if ((mb_rand(0xa6e5u) & 15) != 0) return;                 /* then rarely */

    /* the world's own mood, as three numbers */
    int wars = 0, ruin = 0, pop = mb_pop_civ();
    for (int k = 1; k < MAXK; k++) if (mb_k[k].alive && mb_k[k].war_with) wars++;
    for (int i = 0; i < NC; i += 7) {                          /* a 1-in-7 sample */
        uint8_t b = mb_w.biome[i];
        if (b == B_ASH || b == B_SCORCHED || b == B_RUBBLE) ruin++;
    }
    ruin = ruin * 7 * 100 / NC;                                /* percent, roughly */

    int w[AGE_N];
    w[AGE_HOPE]    = 40 + (pop > 60 ? 30 : 0) - wars * 8 - ruin;
    w[AGE_SUN]     = 25;
    w[AGE_MOON]    = 25;
    w[AGE_IRON]    = 15 + wars * 10;
    w[AGE_CHAOS]   = 10 + wars * 12 + ruin / 2;
    w[AGE_ICE]     = 20;
    w[AGE_ASH]     = 5 + ruin * 2;
    w[AGE_DESPAIR] = 2 + ruin + (pop < 20 ? 25 : 0);
    /* an age never immediately repeats: the point is that things change */
    w[s_age] = 0;

    int total = 0;
    for (int i = 0; i < AGE_N; i++) { if (w[i] < 0) w[i] = 0; total += w[i]; }
    if (total <= 0) return;
    int pick = (int)(mb_rand(0x51eeu) % (uint32_t)total);
    for (int i = 0; i < AGE_N; i++) {
        if (pick < w[i]) {
            s_age = i; s_age_started = mb_w.tick;
            mb_chron_age(AGES[i].name);
            return;
        }
        pick -= w[i];
    }
}

/* --- the laws ----------------------------------------------------------- */

static const char *const LAW_NAME[LAW_N] = {
    "DISASTERS", "WAR", "REBELLION", "PLAGUE", "AGEING", "BREEDING",
    "MONSTERS", "AGES", "FOLLOW HISTORY", "POLITICAL TINT", "NAMEPLATES"
};

/* Everything on by default except the two that are display preferences and one
 * (nameplates) that is off because it clutters a 128 px screen until you want it. */
static uint16_t s_laws = 0xFFFF & ~(uint16_t)(1u << LAW_NAMES);

int  mb_law(int which)        { return (which >= 0 && which < LAW_N) ? ((s_laws >> which) & 1) : 1; }
void mb_law_toggle(int which) { if (which >= 0 && which < LAW_N) s_laws ^= (uint16_t)(1u << which); }
const char *mb_law_name(int which) { return (which >= 0 && which < LAW_N) ? LAW_NAME[which] : "?"; }
uint16_t mb_law_bits(void)         { return s_laws; }
void mb_law_set_bits(uint16_t b)   { s_laws = b; }
void mb_age_set(int a)             { if (a >= 0 && a < AGE_N) { s_age = a; s_age_started = mb_w.tick; } }
