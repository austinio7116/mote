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

/* --- what a power is ---------------------------------------------------- */

enum {
    PW_RAISE, PW_MOUNTAIN, PW_FOREST, PW_GRASS, PW_LOWER, PW_WATER, PW_DESERT, PW_ROAD,
    PW_FIRE, PW_LIGHTNING, PW_METEOR, PW_VOLCANO, PW_QUAKE, PW_TORNADO, PW_ACID, PW_FREEZE
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
    { PW_DESERT,   "DESERT",   &nature_img,     9, 3, 3, 1,   3 },
    { PW_ROAD,     "ROAD",     &ui_status_img,  5, 5, 1, 1,   1 },
};
static const Power P_WRATH[8] = {
    { PW_FIRE,     "FIRE",     &ui_status_img, 10, 3, 2, 1,   8 },
    { PW_LIGHTNING,"LIGHTNING",&ui_status_img,  3, 3, 0, 0,  12 },
    { PW_METEOR,   "METEOR",   &crowns_fx_img,  4, 7, 3, 0, 120 },
    { PW_VOLCANO,  "VOLCANO",  &crowns_fx_img,  5, 5, 1, 0, 200 },
    { PW_QUAKE,    "QUAKE",    &ui_status_img,  5, 3, 0, 0,  60 },
    { PW_TORNADO,  "TORNADO",  &ui_status_img,  2, 3, 0, 0,  90 },
    { PW_ACID,     "ACID",     &ui_status_img, 10, 5, 2, 1,  40 },
    { PW_FREEZE,   "FREEZE",   &ui_status_img,  6, 3, 3, 1,  30 },
};

typedef struct { const char *name; const Power *p; } Tab;
static const Tab TABS[] = { { "LAND", P_LAND }, { "WRATH", P_WRATH } };
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
static void terra(int x, int y, int id)
{
    if (!mb_in(x, y)) return;
    int i = AT(x, y);
    uint8_t *b = &mb_w.biome[i], *o = &mb_w.obj[i], *e = &mb_w.elev[i];
    switch (id) {
    case PW_RAISE:
        *e = (uint8_t)(*e > 241 ? 255 : *e + 14);
        if (mb_water(*b) && *e >= mb_w.sea) { *b = B_BEACH; *o = O_NONE; }
        else if (*e > 200 && mb_land(*b))    *b = B_MOUNTAIN;
        else if (*e > 176 && mb_land(*b))    *b = B_HILL;
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
        if (*e < mb_w.sea) { *b = (*e < mb_w.sea - 10) ? B_SEA : B_SHALLOW; *o = O_NONE; }
        break;
    case PW_WATER:
        *e = (uint8_t)(*e < 20 ? 0 : *e - 20);
        *b = (*e < mb_w.sea - 12) ? B_SEA : B_SHALLOW;
        *o = O_NONE;
        break;
    case PW_DESERT:
        if (!mb_land(*b)) break;
        *b = B_DESERT;
        *o = ((mb_rand((uint32_t)i * 13u) & 15) < 2) ? O_CACTUS : O_NONE;
        break;
    case PW_ROAD:
        if (!mb_land(*b)) break;
        *b = B_ROAD; *o = O_NONE;
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
        mb_flux_ignite(cx, cy, r);
        mb_fx_burst((float)cx, (float)cy, 6, PK_SPARK, FXE_FIRE, 2.5f, 0.5f);
        break;

    case PW_LIGHTNING:
        mb_fx_spawn((float)cx, (float)cy - 3.0f, PK_BOLT, FXE_HOLY, 0.0f, 0.30f);
        mb_fx_impact((float)cx, (float)cy, FXE_FIRE, 0.6f);
        mb_flux_ignite(cx, cy, 1);
        break;

    case PW_METEOR:
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
        mb_agent_spawn(AG_TORNADO, cx, cy);
        mb_fx_burst((float)cx, (float)cy, 10, PK_GUST, FXE_ASH, 3.0f, 0.8f);
        break;

    case PW_ACID:
        mb_flux_blob(cx, cy, r, FX_ACID, 12);
        mb_fx_burst((float)cx, (float)cy, 8, PK_SPARK, FXE_ACID, 2.0f, 0.7f);
        break;

    case PW_FREEZE:
        mb_flux_blob(cx, cy, r, FX_FROST, 11);
        mb_fx_burst((float)cx, (float)cy, 10, PK_STAR, FXE_FROST, 1.8f, 0.9f);
        break;

    default: break;
    }
}

void mb_power_cast(int cx, int cy)
{
    const Power *p = cur();
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

    /* EDGE-triggered: the second press of the same direction is what steps out
     * to the far slot, so a held direction must not oscillate between them. */
    int arm = pressed_arm(in);
    if (arm >= 0) {
        int cur_arm = s_wheel_pick >> 1, cur_far = s_wheel_pick & 1;
        s_wheel_pick = (arm == cur_arm) ? (arm * 2 + (cur_far ^ 1)) : (arm * 2);
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
#define WR_NEAR 20
#define WR_FAR  40
static const int8_t ARMX[4] = {  0, 1, 0, -1 };
static const int8_t ARMY[4] = { -1, 0, 1,  0 };

void mb_power_draw_wheel(uint16_t *fb, const MoteFont *font)
{
    if (!s_wheel) return;
    const Power *tab = TABS[s_tab].p;
    const uint16_t dim = MOTE_RGB565( 12,  14,  26);
    const uint16_t rim = MOTE_RGB565(131, 118, 156);
    const uint16_t hi  = MOTE_RGB565(255, 236,  39);
    const uint16_t txt = MOTE_RGB565(255, 241, 232);

    /* Dim the world behind with scanlines rather than hiding it: you are choosing
     * WHERE as much as WHAT, and the wheel is only up while LB is held. */
    for (int y = 0; y < VIEW_H; y += 2)
        g_api->draw_line(fb, 0, y, 127, y, dim, 0, VIEW_H);

    for (int i = 0; i < 8; i++) {
        int arm = i >> 1, far = i & 1;
        int rr = far ? WR_FAR : WR_NEAR;
        int x = WCX + ARMX[arm] * rr - 4, y = WCY + ARMY[arm] * rr - 4;
        int on = (i == s_wheel_pick);
        /* one spoke per arm, drawn with the near icon, so the two stops on an arm
         * read as one arm with two steps out */
        if (!far)
            g_api->draw_line(fb, WCX, WCY, WCX + ARMX[arm] * (WR_FAR - 6),
                             WCY + ARMY[arm] * (WR_FAR - 6), rim, 0, VIEW_H);
        g_api->draw_rect(fb, x - 2, y - 2, 12, 12, on ? hi : dim, 1, 0, VIEW_H);
        if (on) g_api->draw_rect(fb, x - 3, y - 3, 14, 14, txt, 0, 0, VIEW_H);
        g_api->blit(fb, tab[i].icon, x, y, tab[i].ix * TILE, tab[i].iy * TILE,
                    TILE, TILE, 0, 0, VIEW_H);
    }

    /* hub: the tab, the picked power, and the one control that is not obvious */
    const char *tn = TABS[s_tab].name;
    const char *pn = tab[s_wheel_pick >= 0 ? s_wheel_pick : s_sel[s_tab]].name;
    g_api->text_font(fb, font, tn, WCX - (int)strlen(tn) * 3, WCY - 13, rim);
    g_api->text_font(fb, font, pn, WCX - (int)strlen(pn) * 3, WCY - 4, hi);
    g_api->text_font(fb, font, "RB tab", WCX - 18, WCY + 5, rim);
}
