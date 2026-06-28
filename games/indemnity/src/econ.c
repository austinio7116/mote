/*
 * ThumbyElite — commodity economy.
 */
#include "econ.h"

const GoodDef k_goods[N_GOODS] = {
    { "FOOD",     16, 0 },             /* 0 */
    { "TEXTILES", 20, 0 },             /* 1 */
    { "WATER",     9, 0 },             /* 2 */
    { "LIQUOR",   35, 0 },             /* 3 */
    { "LUXURIES", 180, 0 },            /* 4 */
    { "MEDICINE", 60, 0 },             /* 5 */
    { "COMPUTERS", 220, 0 },           /* 6 */
    { "ELECTRONIC", 140, 0 },          /* 7 */
    { "MACHINERY", 95, 0 },            /* 8 */
    { "TOOLS",    55, 0 },             /* 9 */
    { "ALLOYS",   70, 0 },             /* 10 */
    { "METALS",   45, 0 },             /* 11 */
    { "MINERALS", 25, 0 },             /* 12 */
    { "FUELCELLS", 26, 0 },            /* 13 */
    { "HYDROGEN", 15, 0 },             /* 14 */
    { "RARE GEMS", 400, 0 },           /* 15 */
    { "NARCOTICS", 320, GOOD_ILLEGAL },/* 16 */
    { "WEAPONS",  150, GOOD_ILLEGAL }, /* 17 */
    { "SLAVES",   90, GOOD_ILLEGAL },  /* 18 */
    { "CONTRABAND", 65, GOOD_ILLEGAL },/* 19 */
};

/* Economy bias, percent: negative = this economy PRODUCES it (cheap),
 * positive = it CONSUMES/imports it (dear). Rows = EconType. */
static const int8_t k_bias[8][N_GOODS] = {
    /*               FOOD TEX  WAT  LIQ  LUX  MED  COM  ELE  MAC  TOO  ALO  MET  MIN  FUE  HYD  GEM  NAR  WEA  SLA  CON */
    /* AGRI     */ { -35, -20, -15, -25,  10,   5,  30,  25,  20,  10,  15,  10,   5,   0,  -5,   0,   8, - 5,  10,   0 },
    /* INDUST   */ {  25,  10,  10,   5,   5,   0, -10, -15, -30, -20,  15,  20,  25,  10,   5,   0,   0,  10,   5,   5 },
    /* HITECH   */ {  20,   5,   5,  10,  15,  -15, -35, -30,  -10, -10,  10,  15,  20,   5,   0, -10,  10,  -10,  20,  10 },
    /* EXTRACT  */ {  30,  15,  20,  10,   5,  10,  20,  15,   0,  -5, -10, -25, -35, -10, -15,  -20,   5,  10,  15,   5 },
    /* REFINE   */ {  20,  10,  10,   5,   0,   5,  10,   5,  -5, -10, -30, -20,  15, -15, -10,   0,   0,   5,  10,   5 },
    /* TOURISM  */ {  15,  10,  10, -10, -20,   5,  10,  10,  15,  10,  10,  10,  10,   5,   5,  15,  25,  15,   5,  20 },
    /* MILITARY */ {  20,  10,  10,  15,   5,  10,   5,   0,   5,   0,  -5,   0,   5,  -5,   0,   5, -10, -25,  -5, -10 },
    /* SERVICE  */ {  10,   5,   5,   0,   0,   0,   5,   5,   5,   5,   5,   5,   5,   0,   0,   5,   5,   5,   0,   5 },
};

static uint32_t emix(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

bool econ_has_black_market(const SystemInfo *si) {
    return si->gov == GOV_ANARCHY || si->gov == GOV_FEUDAL;
}

int econ_price(const SystemInfo *si, int station, int good, bool buying) {
    const StationInfo *st = &si->stations[station];
    const GoodDef *g = &k_goods[good];
    if ((g->flags & GOOD_ILLEGAL) && !econ_has_black_market(si)) return 0;

    int pct = 100 + k_bias[st->econ][good];
    /* Tech: high-tech makes manufactured goods cheaper, raw dearer. */
    int tech_adj = 0;
    if (good == 6 || good == 7 || good == 8 || good == 9)
        tech_adj = (8 - (int)st->tech);            /* -7 .. +7 */
    else if (good >= 10 && good <= 14)
        tech_adj = ((int)st->tech - 8) / 2;
    pct += tech_adj;
    /* Seeded jitter +-8%. */
    uint32_t h = emix((uint32_t)si->seed ^ (uint32_t)(station * 197 + good * 31));
    pct += (int)(h % 25u) - 12;     /* local jitter +-12% */

    int price = ((int)g->base * pct) / 100;
    if (price < 1) price = 1;
    /* Spread: stations sell to you above what they pay you. */
    if (buying) price = price + (price / 12) + 1;
    return price;
}

int econ_stock(const SystemInfo *si, int station, int good) {
    const GoodDef *g = &k_goods[good];
    if ((g->flags & GOOD_ILLEGAL) && !econ_has_black_market(si)) return 0;
    uint32_t h = emix((uint32_t)(si->seed >> 16) ^
                      (uint32_t)(station * 911 + good * 53));
    /* Producers hold more stock. */
    int bias = k_bias[si->stations[station].econ][good];
    int max = (bias < -15) ? 60 : (bias < 0) ? 35 : 18;
    return (int)(h % (uint32_t)(max + 1));
}
