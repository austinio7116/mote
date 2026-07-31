/*
 * Motebox — a god simulator for the Thumby Color.
 *
 * A living pixel world you nudge, bless and ruin. See DESIGN.md.
 *
 * Phase 1: the world and the two views. God's Eye (1 tile = 1 pixel, the whole
 * world at once) and Mortal View (8 px tiles, sprites), a cursor that survives
 * the zoom, the clock, and the HUD.
 *
 * Frame flow: update() reads input, advances the world clock by the current
 * speed, and submits whichever view is live; overlay() draws the cursor and HUD
 * over the top.
 */
#include "mb.h"
MOTE_GAME_MODULE();
MOTE_GAME_META("Motebox", "austinio7116");
MOTE_GAME_VERSION("0.1.0");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#if MOTE_HOST
#include <stdlib.h>          /* getenv — the headless test hooks */
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rogue8.font.h"

/* --- time ---------------------------------------------------------------
 * One tick is a WEEK, 52 to the year: at x1 a year takes 6.5 s and a 250-year
 * epic runs in about three and a half minutes at x8, which is the pace a
 * handheld session wants. */
#define TPY 52
/* FIVE SPEEDS, and the top one is a real fast-forward. A god game is watched more than it is
 * played, and x8 is not fast enough to see a tech tree finish or a war run its course — the
 * headless audit does four hundred years in nine seconds, so there is no reason the player
 * cannot skip a century either. LB TAPS THROUGH THEM (LB held is the power wheel). */
static const uint8_t SPEED_TPS[5] = { 0, 8, 24, 64, 200 };   /* ticks per second */
static const char *const SPEED_NAME[5] = { "||", "x1", "x3", "x8", "x25" };
static int s_speed = 1;
static float s_tick_acc;
#if MOTE_HOST
static int s_perf;                   /* MOTEBOX_PERF=1 */
static int s_stat;                   /* MOTEBOX_STAT=1 */
static char s_cast_script[160];       /* MOTEBOX_CAST=name@x,y;... */
static int s_trace;                  /* MOTEBOX_TRACE=1 */
static int s_frame;
#endif

/* --- view + cursor ------------------------------------------------------ */
static int s_god = 1;                /* 1 = God's Eye, 0 = Mortal View */
static int s_cx = MW / 2, s_cy = MH / 2;
static int s_cam_x, s_cam_y;
static float s_hold;                 /* how long the d-pad has been held */
static float s_move_acc;
static float s_cast_cool;            /* brush cadence */
static int   s_lb_was_wheel;         /* so releasing the wheel is not a speed tap */
static uint32_t s_shake_ph = 1;      /* camera-shake jitter (render only) */
static float s_denied;               /* 'not enough faith' message timer */
static float s_nobody;               /* 'the cast reached nobody' message timer */
static float s_rb_hold;              /* RB held: tap toggles the view, hold seeks */
/* --- FOLLOW HISTORY without stealing the camera --------------------------
 * The camera used to be TELEPORTED to each new headline the moment it happened,
 * gated only on whether the power wheel was open. At x3 and x8 a headline lands every
 * few seconds, so any attempt to look at something was yanked away mid-look and you
 * arrived somewhere else with no idea where you had been. It is the right behaviour
 * for a game you leave running and completely wrong for one you are using.
 *
 * Two rules fix it. It only acts when your HANDS ARE OFF — a few seconds with no input
 * at all — so it can never fight you, and any input aborts a move in progress. And it
 * PANS rather than jumps, so when it does take you somewhere you can see where you
 * went and roughly how far. */
static float s_idle;                 /* seconds since the player last touched anything */
static int   s_pan_x = -1, s_pan_y;  /* where follow is taking us; -1 = nowhere */
#define FOLLOW_IDLE 3.5f             /* hands off this long before history may lead */
static float s_seek_acc;
static int   s_rb_seeked;            /* so a release after seeking is not a zoom */

/* --- HUD ---------------------------------------------------------------- */
#define HUD_Y   VIEW_H
#define C_HUDBG MOTE_RGB565( 12,  14,  26)
#define C_TEXT  MOTE_RGB565(194, 195, 199)
#define C_HI    MOTE_RGB565(255, 236,  39)
#define C_WARN  MOTE_RGB565(255,   0,  77)
#define C_CURS  MOTE_RGB565(255, 241, 232)
#define C_DARK  MOTE_RGB565( 29,  43,  83)

static const char *const B_NAME[B_N] = {
    "-", "OCEAN", "SEA", "SHOAL", "ICE", "BEACH", "DESERT", "SAVANNA", "GRASS",
    "SWAMP", "HILL", "MOUNTAIN", "PEAK", "TUNDRA", "SNOW", "ASH", "SCORCHED",
    "LAVA", "ACID", "FARM", "RUBBLE", "MEADOW", "FOREST", "ROAD",
};
static const char *const FX_NAME[FX_N] = { "", "BURNING", "LAVA", "FLOOD", "ACID", "FROZEN" };
/* ONE ENTRY PER O_*, and the compiler will not tell you if it is short: the first
 * version stopped at "crag" and every id after it read as a NULL pointer, so
 * putting the cursor on a house crashed the game. Keep this in step with the enum. */
static const char *const O_NAME[O_N] = {
    "", "tree", "tree", "dead tree", "bush", "grass", "rock", "reeds",
    "reeds", "iron", "silver", "gold", "gems", "boulder", "crag", "grave",
    /* --- built --- */
    "campfire", "hall", "great hall", "castle",
    "house", "cottage", "manor",
    "farm", "mine", "woodcutter", "barracks", "temple", "tower", "dock", "wall",
    "granary", "market", "library", "foundry", "college", "hospital",
    "factory", "station", "power station", "missile silo",
    "plan",
};
/* A compile-time tripwire for exactly the mistake above. */
typedef char mb_onames_complete[(sizeof O_NAME / sizeof O_NAME[0]) == O_N ? 1 : -1];

/* Trait names for the Soul Card, in TR_*_B order. A trait the player cannot read is
 * a trait that may as well not exist: BLESSED, MARKED and VETERAN are the whole
 * reward for spending faith on one person. */
static const char *const TR_NAME[TR_N] = {
    "tough", "fast", "brave", "coward", "greedy", "loyal", "ambitious", "fertile",
    "barren", "genius", "stupid", "pious", "vengeful", "regenerating",
    "BLESSED", "CURSED", "CHOSEN", "MARKED", "plagued", "mad", "contagious",
    "risen", "immortal", "veteran", "immune",
};
typedef char mb_trnames_complete[(sizeof TR_NAME / sizeof TR_NAME[0]) == TR_N ? 1 : -1];
static const char *const JOB_NAME[JOB_N] = {
    "idle", "wandering", "foraging", "hunting", "fleeing", "courting",
    "working", "fighting", "FIGHTING THE FIRE",
};
typedef char mb_jobnames_complete[(sizeof JOB_NAME / sizeof JOB_NAME[0]) == JOB_N ? 1 : -1];


/* --- HUD text that cannot collide ---------------------------------------
 * rogue8 is PROPORTIONAL, so "Y9" and "Y412" are different widths and a HUD laid
 * out with hard-coded x positions collides as soon as a number grows a digit. It
 * did: at year 100 the year ran into the power name, and a headline ran over both.
 *
 * So every field declares a COLUMN, and text is measured against the real glyph
 * advances and truncated to fit it. Nothing can overlap anything, whatever the
 * world does. */
static int hud_w(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int i = (int)*p - rogue8.first;
        w += (i >= 0 && i < rogue8.count) ? rogue8.glyphs[i].adv : 4;
    }
    return w;
}

/* Draw `s` at (x,y), clipped to `maxw` pixels; returns the width actually used.
 * align: -1 left, +1 right within the column. */
static void hud_text(uint16_t *fb, const char *s, int x, int y, int maxw,
                     uint16_t col, int align)
{
    char buf[48];
    int n = 0;
    for (const char *p = s; *p && n < (int)sizeof buf - 1; p++) {
        buf[n] = *p; buf[n + 1] = 0;
        if (hud_w(buf) > maxw) { buf[n] = 0; break; }
        n++;
    }
    int w = hud_w(buf);
    mote->text_font(fb, &rogue8, buf, align > 0 ? x + maxw - w : x, y, col);
}

/* The columns. Two rows of 8 px in the 16 px strip, and every field's width is
 * declared here rather than discovered at runtime. */
#define HC_SPEED_X   1
#define HC_SPEED_W  20     /* "x1" measures 16 px in rogue8; 14 clipped the digit */
#define HC_YEAR_X   22
#define HC_YEAR_W   26
#define HC_POWER_X  48
#define HC_POWER_W  58
#define HC_VIEW_X  108
#define HC_VIEW_W   19
#define HC_INFO_X    1
#define HC_INFO_W   88
#define HC_FAITH_X  92
#define HC_FAITH_W  35

/* --- helpers ----------------------------------------------------------- */

/* Mortal View camera: centre on the cursor, clamped so the view never leaves
 * the world. Cursor position is shared between the views, so a zoom lands where
 * you were looking. */
static void cam_follow(void)
{
    s_cam_x = s_cx * TILE - 128 / 2 + TILE / 2;
    s_cam_y = s_cy * TILE - VIEW_H / 2 + TILE / 2;
    int maxx = MW * TILE - 128, maxy = MH * TILE - VIEW_H;
    if (s_cam_x < 0) s_cam_x = 0; if (s_cam_x > maxx) s_cam_x = maxx;
    if (s_cam_y < 0) s_cam_y = 0; if (s_cam_y > maxy) s_cam_y = maxy;
}

/* --- RB hold: SEEK -------------------------------------------------------
 * Also promised by DESIGN.md 4 and never built. Pixel-hunting a one-pixel villager
 * across a 128x112 world with a d-pad is misery, and it is the main reason the game
 * could feel like nothing was happening: plenty was, somewhere else. Seek walks a
 * priority list of places the simulation already knows are interesting — a fire, a
 * summoned monster, a war, then the villages by size — so it doubles as the "show me
 * a story" button for passive play. */
static void seek_next(void)
{
    static int turn;
    int bx = -1, by = -1;

    for (int pass = 0; pass < 4 && bx < 0; pass++) {
        int step = ++turn;
        switch (pass) {
        case 0: {                                  /* something burning or flooding */
            int hits = 0, want;
            for (int c = 0; c < NC; c++) if (mb_fkind(mb_w.flux[c])) hits++;
            if (!hits) break;
            want = step % hits; hits = 0;
            for (int c = 0; c < NC; c++)
                if (mb_fkind(mb_w.flux[c]) && hits++ == want) { bx = c % MW; by = c / MW; break; }
            break;
        }
        case 1: {                                  /* a monster walking the world */
            int n = mb_agent_max(), hits = 0, ax, ay, kind;
            for (int a = 0; a < n; a++) if (mb_agent_get(a, &ax, &ay, &kind)) hits++;
            if (!hits) break;
            int want = step % hits; hits = 0;
            for (int a = 0; a < n; a++)
                if (mb_agent_get(a, &ax, &ay, &kind) && hits++ == want) { bx = ax; by = ay; break; }
            break;
        }
        case 2: {                                  /* a village at war */
            int hits = 0;
            for (int v = 1; v < MAXV; v++)
                if (mb_v[v].alive && mb_k[mb_v[v].kingdom].war_with) hits++;
            if (!hits) break;
            int want = step % hits; hits = 0;
            for (int v = 1; v < MAXV; v++)
                if (mb_v[v].alive && mb_k[mb_v[v].kingdom].war_with && hits++ == want) {
                    bx = mb_v[v].x; by = mb_v[v].y; break;
                }
            break;
        }
        default: {                                 /* otherwise, the next village */
            int hits = 0;
            for (int v = 1; v < MAXV; v++) if (mb_v[v].alive && mb_v[v].pop) hits++;
            if (!hits) break;
            int want = step % hits; hits = 0;
            for (int v = 1; v < MAXV; v++)
                if (mb_v[v].alive && mb_v[v].pop && hits++ == want) {
                    bx = mb_v[v].x; by = mb_v[v].y; break;
                }
            break;
        }
        }
    }
    if (bx >= 0) { s_cx = bx; s_cy = by; if (!s_god) cam_follow(); }
}

static void god_menu(void);

/* --- B: THE INSPECT PAGES ----------------------------------------------
 *
 * Every loop this game runs is invisible by construction. In God's Eye a person is ONE
 * PIXEL; in Mortal View they are an 8x8 figure. Nothing on screen can show you that this
 * one is a miner married to that one, that their town is three points off masonry, that
 * its lord is 61 and failing, or that the caravan crossing the bridge is twelve loads of
 * grain going to a village that would otherwise starve. All of it is in the simulation and
 * none of it is reachable — which is exactly how a world of systems reads as noise.
 *
 * So the inspect is THREE PAGES, not one card: the person and the ground under the cursor,
 * the settlement that claims it, and the kingdom that claims the settlement. Each page
 * navigates to the others, so you can walk up the chain from a body in a field to the
 * crown that will avenge it.
 *
 * The status pips that used to float over the crowd were an attempt at the same problem
 * and the wrong shape for it — a legend you have to learn, painted over the world. Detail
 * belongs somewhere you go looking for it; the world itself should carry only what reads
 * without explanation (a profession's figure, a banner's colour, a burning roof).
 */
#define IR_ROWS 26
static char s_ir[IR_ROWS][34];

static int ir_add(const char **items, int n, const char *fmt, ...)
{
    if (n >= IR_ROWS) return n;
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_ir[n], sizeof s_ir[n], fmt, ap);
    va_end(ap);
    items[n] = s_ir[n];
    return n + 1;
}

/* the traits of a unit or a lord, packed onto as few lines as they need */
static int ir_traits(const char **items, int n, uint32_t traits)
{
    int t = 0;
    while (t < TR_N && n < IR_ROWS) {
        int o = 0; s_ir[n][0] = 0;
        for (; t < TR_N; t++) {
            if (!(traits & TRB(t))) continue;
            int need = (int)strlen(TR_NAME[t]) + (o ? 1 : 0);
            if (o && o + need >= (int)sizeof s_ir[n] - 1) break;
            o += snprintf(s_ir[n] + o, sizeof s_ir[n] - o, "%s%s", o ? " " : "", TR_NAME[t]);
        }
        if (!o) break;
        items[n] = s_ir[n]; n++;
    }
    return n;
}

/* Which village this inspect is about: the one that claims the cell, else the nearest
 * living one, so putting the cursor in a field still tells you whose field it is. */
static int ir_village(int x, int y)
{
    int v = mb_w.claim[AT(x, y)];
    if (v && v < MAXV && mb_v[v].alive) return v;
    int best = 0, bd = 0;
    for (int i = 1; i < MAXV; i++) {
        if (!mb_v[i].alive) continue;
        int dx = mb_v[i].x - x, dy = mb_v[i].y - y, d = dx * dx + dy * dy;
        if (d > 400) continue;                       /* twenty cells: still "near here" */
        if (!best || d < bd) { bd = d; best = i; }
    }
    return best;
}

static int page_here(const char **items, int x, int y)
{
    int n = 0, i = AT(x, y);
    int ui = mb_unit_at(x, y);
    if (ui >= 0 && mb_u[ui].alive) {
        const Unit *u = &mb_u[ui];
        const MbSpecies *S = &MB_SP[u->sp];
        char gn[24] = "", fn[24] = "";
        mb_name_str(gn, sizeof gn, 2, u->given);
        if (u->sp < SP_CIV_N) {
            mb_name_str(fn, sizeof fn, 2, u->family);
            n = ir_add(items, n, "%s %s", gn, fn);
            n = ir_add(items, n, "%s %s", S->name,
                       u->prof ? MB_PROF_NAME[u->prof < PROF_N ? u->prof : 0] : "of no trade");
        } else {
            n = ir_add(items, n, "%s, a %s", gn, S->name);
        }
        n = ir_add(items, n, "age %d of %d   hp %d", u->age, S->lifespan, u->hp);
        n = ir_add(items, n, "doing: %s", JOB_NAME[u->job < JOB_N ? u->job : 0]);
        n = ir_add(items, n, "mood %d   hunger %d%s", u->happy, u->hunger,
                   u->sick ? "   ILL" : "");
        if (u->mate && u->mate - 1 < MAXU && mb_u[u->mate - 1].alive) {
            char mn[24] = "";
            mb_name_str(mn, sizeof mn, 2, mb_u[u->mate - 1].given);
            n = ir_add(items, n, "wed to %s", mn);
        } else if (u->sp < SP_CIV_N) {
            n = ir_add(items, n, "unwed");
        }
        if (u->carry) {
            static const char *const CK[5] = { "grain", "timber", "stone", "iron", "gold" };
            if (u->job == JOB_HAUL && u->dest && u->dest < MAXV) {
                char dn[24] = "";
                mb_name_str(dn, sizeof dn, 0, mb_v[u->dest].name);
                n = ir_add(items, n, "hauling %d %s to %s", u->carry,
                           CK[u->carry_kind < 5 ? u->carry_kind : 0], dn);
            } else {
                n = ir_add(items, n, "carrying %d %s", u->carry,
                           CK[u->carry_kind < 5 ? u->carry_kind : 0]);
            }
        }
        if (u->kills) n = ir_add(items, n, "%d kills", u->kills);
        if (u->village && u->village < MAXV && mb_v[u->village].alive) {
            char vn[24]; mb_name_str(vn, sizeof vn, 0, mb_v[u->village].name);
            n = ir_add(items, n, "home: %s", vn);
        }
        n = ir_traits(items, n, u->traits);
        n = ir_add(items, n, "-----");
    }

    uint8_t b = mb_w.biome[i], ob = mb_w.obj[i];
    n = ir_add(items, n, "%s%s%s   elev %d", B_NAME[b], ob ? " / " : "",
               ob ? O_NAME[ob] : "", mb_w.elev[i]);
    if (mb_w.road[i]) n = ir_add(items, n, "a paved street");
    uint8_t fk = mb_fkind(mb_w.flux[i]);
    if (fk && fk < FX_N) n = ir_add(items, n, "%s (%d)", FX_NAME[fk], mb_fint(mb_w.flux[i]));
    return n;
}

static int page_town(const char **items, int v)
{
    int n = 0;
    if (!v) return ir_add(items, 0, "no settlement near here");
    Village *V = &mb_v[v];
    char vn[24]; mb_name_str(vn, sizeof vn, 0, V->name);
    n = ir_add(items, n, "%s, a %s", vn, MB_TIER_NAME[V->tier < TIER_N ? V->tier : 0]);
    n = ir_add(items, n, "%s country, creed of %s",
               MB_PROF_NAME[V->spec < PROF_N ? V->spec : 0],
               MB_CREED_NAME[V->creed < CREED_N ? V->creed : 0]);
    n = ir_add(items, n, "pop %d   beds %d   founded y%d",
               V->pop, V->housing, (int)(V->founded / 52));
    n = ir_add(items, n, "-- the lord --");
    {
        char ln[24] = ""; mb_name_str(ln, sizeof ln, 2, V->lord_name);
        n = ir_add(items, n, "%s, aged %d", ln, V->lord_age);
        n = ir_add(items, n, "steward %d  diplomat %d  war %d",
                   V->lord_stew, V->lord_diplo, V->lord_war);
        n = ir_traits(items, n, V->lord_traits);
    }
    n = ir_add(items, n, "-- the store --");
    n = ir_add(items, n, "grain %d  timber %d  stone %d", V->food, V->wood, V->stone);
    n = ir_add(items, n, "iron %d  gold %d  caravans in %d", V->iron, V->gold, V->traded);
    n = ir_add(items, n, "-- what stands --");
    n = ir_add(items, n, "houses %d  farms %d  camps %d",
               mb_civ_count(v, O_HOUSE1) + mb_civ_count(v, O_HOUSE2) + mb_civ_count(v, O_HOUSE3),
               mb_civ_count(v, O_FARM), mb_civ_count(v, O_WOODCUT));
    n = ir_add(items, n, "mines %d  temples %d  towers %d",
               mb_civ_count(v, O_MINE), mb_civ_count(v, O_TEMPLE), mb_civ_count(v, O_TOWER));
    n = ir_add(items, n, "docks %d  barracks %d  wall %d",
               mb_civ_count(v, O_DOCK), mb_civ_count(v, O_BARRACKS), mb_civ_count(v, O_WALL));
    {   /* THE CIVIC LADDER, only the rungs it has. Nine more lines of "0" would bury the
         * three facts that matter, so this prints what is standing and nothing else. */
        static const uint8_t CV[9] = { O_GRANARY, O_MARKET, O_LIBRARY, O_FOUNDRY, O_COLLEGE,
                                       O_HOSPITAL, O_FACTORY, O_STATION, O_POWER };
        int t = 0;
        while (t < 9 && n < IR_ROWS - 6) {
            int o = 0; s_ir[n][0] = 0;
            for (; t < 9; t++) {
                int c = mb_civ_count(v, CV[t]);
                if (!c) continue;
                int need = (int)strlen(O_NAME[CV[t]]) + 4;
                if (o && o + need >= (int)sizeof s_ir[n] - 1) break;
                o += snprintf(s_ir[n] + o, sizeof s_ir[n] - o, "%s%s %d",
                              o ? "  " : "", O_NAME[CV[t]], c);
            }
            if (!o) break;
            items[n] = s_ir[n]; n++;
        }
    }
    if (V->plan_obj) n = ir_add(items, n, "planning: %s", O_NAME[V->plan_obj]);
    n = ir_add(items, n, "-- the mood --");
    n = ir_add(items, n, "happy %d  loyal %d  unrest %d", V->happy, V->loyalty, V->unrest);
    n = ir_add(items, n, "threat %d  soldiers %d%s", V->threat, V->soldiers,
               V->mustering ? "  MUSTERING" : "");
    {
        int k = V->kingdom;
        const Kingdom *K = (k > 0 && k < MAXK && mb_k[k].alive) ? &mb_k[k] : 0;
        if (!K || !K->goal)
            n = ir_add(items, n, "there is nothing left to learn");
        else
            n = ir_add(items, n, "research %d of %d: %s", K->bank,
                       MB_TECH[K->goal].cost, MB_TECH[K->goal].name);
    }
    return n;
}

static int page_kingdom(const char **items, int v)
{
    int n = 0;
    int k = v ? mb_kingdom_of(v) : 0;
    if (!k || k >= MAXK || !mb_k[k].alive) return ir_add(items, 0, "no crown here");
    Kingdom *K = &mb_k[k];
    char kn[24]; mb_name_str(kn, sizeof kn, 1, K->name);
    n = ir_add(items, n, "%s", kn);
    n = ir_add(items, n, "creed of %s", MB_CREED_NAME[K->creed < CREED_N ? K->creed : 0]);
    {
        int towns = 0, pop = 0, best = 0;
        for (int i = 1; i < MAXV; i++) {
            if (!mb_v[i].alive || mb_v[i].kingdom != k) continue;
            towns++; pop += mb_v[i].pop;
            if (!best || mb_v[i].pop > mb_v[best].pop) best = i;
        }
        n = ir_add(items, n, "%d towns   %d souls", towns, pop);
        if (best) {
            char bn[24]; mb_name_str(bn, sizeof bn, 0, mb_v[best].name);
            n = ir_add(items, n, "seat: %s", bn);
        }
    }
    n = ir_add(items, n, "-- what it knows --");
    n = ir_add(items, n, "the %s era   %d of %d techs",
               MB_ERA_NAME[K->era < ERA_N ? K->era : 0], K->tech, TECH_N - 1);
    if (K->goal)
        n = ir_add(items, n, "working on %s (%d/%d)", MB_TECH[K->goal].name,
                   K->bank, MB_TECH[K->goal].cost);
    else
        n = ir_add(items, n, "it has learned everything");
    if (mb_tech_known(k, TECH_NUKE))
        n = ir_add(items, n, "IT HAS THE BOMB%s", K->nuked ? " AND HAS USED IT" : "");
    /* Grouped by era, wrapped: thirty-six names do not fit on one row and a truncated list
     * is worse than none. */
    for (int era = ERA_STONE; era < ERA_N && n < IR_ROWS - 6; era++) {
        int any = 0;
        for (int t = TECH_TOOLS; t < TECH_N; t++)
            if (MB_TECH[t].era == era && mb_tech_known(k, t)) any = 1;
        if (!any) continue;
        int t = TECH_TOOLS, first = 1;
        while (t < TECH_N && n < IR_ROWS - 6) {
            int o = 0; s_ir[n][0] = 0;
            if (first) o = snprintf(s_ir[n], sizeof s_ir[n], "%s:", MB_ERA_NAME[era]);
            for (; t < TECH_N; t++) {
                if (MB_TECH[t].era != era || !mb_tech_known(k, t)) continue;
                int need = (int)strlen(MB_TECH_NAME[t]) + 1;
                if (o + need >= (int)sizeof s_ir[n] - 1) break;
                o += snprintf(s_ir[n] + o, sizeof s_ir[n] - o, " %s", MB_TECH_NAME[t]);
            }
            if (!o) break;
            items[n] = s_ir[n]; n++; first = 0;
            if (t >= TECH_N) break;
        }
    }
    n = ir_add(items, n, "-- how it stands --");
    n = ir_add(items, n, "war weariness %d", K->exhaustion);
    {
        int wars = 0, allies = 0;
        for (int i = 1; i < MAXK; i++) {
            if (K->war_with  & (1u << i)) wars++;
            if (K->ally_with & (1u << i)) allies++;
        }
        n = ir_add(items, n, "%d wars   %d allies", wars, allies);
        for (int i = 1; i < MAXK && n < IR_ROWS - 4; i++) {
            if (!(K->war_with & (1u << i)) || !mb_k[i].alive) continue;
            char en[24]; mb_name_str(en, sizeof en, 1, mb_k[i].name);
            n = ir_add(items, n, "at war with %s", en);
        }
    }
    return n;
}


static void view_set(int god)
{
    s_god = god;
    /* The whole view switch: the world rasteriser IS the background pass, so
     * handing it over (or handing back NULL) is all that changes. */
    /* Both views paint their own background: the map rasteriser in God's Eye, and in
     * Mortal View a live sea — deep ocean is the scene background rather than a
     * tileset, so a callback is the only place it can have a surface at all. */
    mote->set_background_cb(god ? mb_god_band : mb_draw_sea_band);
    if (!god) cam_follow();
}

/* --- the God Menu ------------------------------------------------------- */

static void law_menu(void)
{
    for (;;) {
        const char *items[LAW_N + 1];
        static char rows[LAW_N][28];
        for (int i = 0; i < LAW_N; i++) {
            snprintf(rows[i], sizeof rows[i], "%s %s", mb_law(i) ? "ON " : "off", mb_law_name(i));
            items[i] = rows[i];
        }
        items[LAW_N] = "BACK";
        int c = mote->menu("WORLD LAWS", items, LAW_N + 1);
        if (c < 0 || c == LAW_N) return;
        mb_law_toggle(c);
    }
}

/* ===================================================================== *
 *  THE INFORMATION SCREENS
 *
 *  These replace six mote->menu() lists. A menu was the wrong tool three times over: it is a
 *  LIST, so it can hold text and nothing else — no portrait, no bar, no sprite; it SCROLLS,
 *  so a screen becomes a corridor; and it BLOCKS, so it cannot animate and cannot be
 *  screenshotted (an entire session went by without anyone seeing one of these pages).
 *
 *  Exploring is what this game is for, so the screens are drawn in the ordinary frame like
 *  everything else: g_update takes the input, g_overlay paints over the scene. That means they
 *  can animate, they can show the world moving underneath, and they can be captured.
 *
 *  The journey is WHO IS HERE -> the subject -> what a god can do about it.
 * ===================================================================== */

enum { UI_NONE = 0, UI_PICK, UI_SOUL, UI_SHAPE, UI_LORD, UI_TOWN, UI_KING, UI_LAND, UI_WORLD };
static int s_ui;                 /* which screen, UI_NONE for the world */
static int s_ui_row;             /* the cursor within it */
static int s_subject;            /* unit index the SOUL/SHAPE screens are about */
static int s_subject_v;          /* village index the LORD/TOWN screens are about */
#define MB_LORD_RAISE 50   /* Faith for one step of a lord's stat: see ui_draw_lord */
static float s_ui_flash;         /* a spend just happened: flash the number */

/* WHAT IS ON THIS GROUND. Everything whose sprite OVERLAPS the cell, not merely everything
 * whose tile index equals it — figures are drawn at sub-pixel positions now, so a villager
 * standing on the boundary appears on the cell you are pointing at and must be listed there.
 * Buildings and the lord are listed too, so this doubles as "what am I looking at". */
enum { PK_UNIT = 0, PK_BUILD, PK_LORD };
#define PK_MAX 10
static struct { uint8_t kind; uint16_t idx; } s_pick[PK_MAX];
static int s_pick_n;

static void pick_build(void)
{
    s_pick_n = 0;
    const int cxs = s_cx * 16, cys = s_cy * 16;      /* the cell, in sixteenths */
    for (int i = 0; i < mb_nu && s_pick_n < PK_MAX; i++) {
        const Unit *u = &mb_u[i];
        if (!u->alive) continue;
        int dx = (int)u->x - cxs, dy = (int)u->y - cys;
        if (dx < -15 || dx > 15 || dy < -15 || dy > 15) continue;   /* sprite overlap */
        s_pick[s_pick_n].kind = PK_UNIT; s_pick[s_pick_n].idx = (uint16_t)i; s_pick_n++;
    }
    int v = mb_w.claim[AT(s_cx, s_cy)];
    uint8_t o = mb_w.obj[AT(s_cx, s_cy)];
    if (mb_is_build(o) && s_pick_n < PK_MAX) {
        s_pick[s_pick_n].kind = PK_BUILD; s_pick[s_pick_n].idx = o; s_pick_n++;
    }
    /* the lord belongs to the hall: it is the only place in the world you can meet them,
     * because a lord is stats and a name on the Village and has never been a person */
    if (v && mb_v[v].alive && s_pick_n < PK_MAX
        && (o == O_HALL1 || o == O_HALL2 || o == O_HALL3)) {
        s_pick[s_pick_n].kind = PK_LORD; s_pick[s_pick_n].idx = (uint16_t)v; s_pick_n++;
    }
    if (s_ui_row >= s_pick_n) s_ui_row = s_pick_n ? s_pick_n - 1 : 0;
}

/* the sprite for one pick row */
static void pick_sprite(int i, const MoteImage **img, int *cx, int *cy, int *h)
{
    *h = 1;
    if (s_pick[i].kind == PK_UNIT) {
        const Unit *u = &mb_u[s_pick[i].idx];
        int sh, sx, sy;
        mb_cast_pick_pub(u, mb_hash2(s_pick[i].idx, u->given), &sh, &sx, &sy);
        *img = mb_ui_sheet(sh); *cx = sx; *cy = sy;
    } else if (s_pick[i].kind == PK_BUILD) {
        *img = mb_ui_town_sheet();
        *cx = mb_draw_form_col((uint8_t)s_pick[i].idx, 0, s_cx, s_cy);
        *cy = 4; *h = 2;                       /* the unclaimed grey banner row */
    } else {
        int v = s_pick[i].idx, sh, sx, sy;
        mb_cast_role(mb_v[v].sp, mb_cast_role_lord(), (unsigned)v * 7919u, &sh, &sx, &sy);
        *img = mb_ui_sheet(sh); *cx = sx; *cy = sy;
    }
}

static void pick_label(int i, char *out, int n, char *role, int rn)
{
    out[0] = role[0] = 0;
    if (s_pick[i].kind == PK_UNIT) {
        const Unit *u = &mb_u[s_pick[i].idx];
        if (u->sp < SP_CIV_N) {
            mb_name_str(out, n, NK_PERSON, u->given);
            snprintf(role, (size_t)rn, "%s", MB_PROF_NAME[u->prof < PROF_N ? u->prof : 0]);
        } else {
            snprintf(out, (size_t)n, "%s", MB_SP[u->sp].name);
            snprintf(role, (size_t)rn, "beast");
        }
    } else if (s_pick[i].kind == PK_BUILD) {
        snprintf(out, (size_t)n, "%s", O_NAME[s_pick[i].idx]);
        snprintf(role, (size_t)rn, "built");
    } else {
        mb_name_str(out, n, NK_PERSON, mb_v[s_pick[i].idx].lord_name);
        snprintf(role, (size_t)rn, "LORD");
    }
}

/* ------------------------------------------------------------------ WHO IS HERE */
static void ui_draw_pick(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(18, 24, 44);
    mb_ui_panel(fb, 0, 0, 128, 128, MB_UI_SKY, FILL, 1);
    mb_ui_text(fb, 7, 6, "WHO IS HERE", MB_UI_CREAM, 100);
    char buf[32];
    snprintf(buf, sizeof buf, "%d on this ground", s_pick_n);
    mb_ui_text(fb, 7, 15, buf, MB_UI_DIM, 112);
    if (!s_pick_n) {
        mb_ui_text(fb, 7, 26, "bare ground", MB_UI_DIM, 112);
        mb_ui_actions(fb, "A LAND", "B<", FILL);
        return;
    }
    for (int i = 0; i < s_pick_n && i < 6; i++) {
        int y = 26 + i * 13;
        int sel = (i == s_ui_row);
        if (sel) g_api->draw_rect(fb, 5, y - 1, 118, 12, MOTE_RGB565(44, 58, 102), 1, 0, 128);
        const MoteImage *img; int cx, cy, h;
        pick_sprite(i, &img, &cx, &cy, &h);
        g_api->blit(fb, img, 8, y - (h > 1 ? 4 : 0), cx * 8, cy * 8, 8, 8 * h, 0, 0, 128);
        char name[24], role[16];
        pick_label(i, name, sizeof name, role, sizeof role);
        mb_ui_text(fb, 20, y + 1, name, sel ? MB_UI_CREAM : MB_UI_DIM, 44);
        mb_ui_text(fb, 66, y + 1, role,
                   s_pick[i].kind == PK_LORD ? MB_UI_GOLD : MB_UI_OK, 54);
    }
    mb_ui_actions(fb, "A LOOK", "B<", FILL);
}

/* ------------------------------------------------------------------ A SOUL */
static void ui_draw_soul(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(20, 26, 50);
    mb_ui_panel(fb, 0, 0, 128, 128, MB_UI_SKY, FILL, 1);
    if (s_subject < 0 || s_subject >= mb_nu || !mb_u[s_subject].alive) {
        mb_ui_text(fb, 7, 30, "they are gone", MB_UI_DIM, 112);
        mb_ui_actions(fb, 0, "B<", FILL);
        return;
    }
    const Unit *u = &mb_u[s_subject];
    const MbSpecies *S = &MB_SP[u->sp];
    char gn[24], fn[24], buf[40];
    mb_name_str(gn, sizeof gn, NK_PERSON, u->given);
    mb_name_str(fn, sizeof fn, NK_FAMILY, u->family);
    mb_ui_text(fb, 7, 6, gn, MB_UI_CREAM, 78);
    mb_ui_hearts(fb, 88, 6, 4, u->hp > 0 ? u->hp : 0, 100);
    snprintf(buf, sizeof buf, "%s %s", S->name,
             MB_PROF_NAME[u->prof < PROF_N ? u->prof : 0]);
    mb_ui_text(fb, 7, 15, buf, MB_UI_GOLD, 114);
    /* the likeness, on the plaque */
    mb_ui_plaque(fb, 5, 23, 32, 32, MOTE_RGB565(12, 16, 34));
    { int sh, cx, cy;
      mb_cast_pick_pub(u, mb_hash2(s_subject, u->given), &sh, &cx, &cy);
      mb_ui_blit3(fb, mb_ui_sheet(sh), cx, cy, 9, 27, 3); }
    snprintf(buf, sizeof buf, "aged %d", u->age);
    mb_ui_text(fb, 40, 25, buf, MB_UI_DIM, 82);
    mb_ui_text(fb, 40, 34, fn, MB_UI_DIM, 82);
    if (u->village && mb_v[u->village].alive) {
        char pl[24]; mb_name_str(pl, sizeof pl, NK_PLACE, mb_v[u->village].name);
        mb_ui_text(fb, 40, 43, pl, MB_UI_DIM, 82);
    }
    mb_ui_face(fb, 7, 56, u->happy);
    mb_ui_meter(fb, 20, 57, 7, u->happy + 100, 200, 0);
    mb_ui_rule(fb, 6, 64, 116, "WORK", MB_UI_SKY, FILL, MB_UI_DIM);
    mb_ui_text(fb, 7, 72, JOB_NAME[u->job < JOB_N ? u->job : 0], MB_UI_CREAM, 114);
    mb_ui_icon(fb, 7, 81, 4, 5);
    mb_ui_meter(fb, 20, 82, 7, 128 - u->hunger, 128, 0);
    mb_ui_rule(fb, 6, 92, 116, "TRAITS & KIN", MB_UI_SKY, FILL, MB_UI_DIM);
    mb_ui_traits(fb, 7, 101, u->traits);
    if (u->mate && mb_u[u->mate - 1].alive) {
        const Unit *m = &mb_u[u->mate - 1];
        int sh, cx, cy;
        mb_cast_pick_pub(m, mb_hash2(u->mate - 1, m->given), &sh, &cx, &cy);
        g_api->blit(fb, mb_ui_sheet(sh), 74, 101, cx * 8, cy * 8, 8, 8, 0, 0, 128);
        char mn[24]; mb_name_str(mn, sizeof mn, NK_PERSON, m->given);
        mb_ui_text(fb, 86, 102, mn, MB_UI_DIM, 34);
    } else {
        mb_ui_text(fb, 74, 102, "unwed", MB_UI_DIM, 46);
    }
    mb_ui_actions(fb, "A SHAPE", "B<", FILL);
}

/* ------------------------------------------------------------------ SHAPE A SOUL
 *
 * WHAT A GOD MAY DO TO ONE PERSON, and why these and not others: a Unit holds hp, happy,
 * hunger, age, traits, trade and a spouse — and nothing else. There is no strength or wit to
 * raise, so rather than invent three stats and leave them as decoration on this page, every
 * row here changes something the simulation ALREADY reads every tick.
 *
 * Prices are set against a measured income of 191-432 Faith a year and a 700 reserve, so a
 * gift is a real decision and a handful of them is a season's devotion.
 */
typedef struct { const char *name; uint16_t cost; uint32_t trait; uint8_t op; } Gift;
enum { GF_TRAIT = 0, GF_HEAL, GF_CHEER, GF_FEED, GF_TRADE, GF_LORD };
static const Gift GIFTS[] = {
    { "MEND",      30, 0,           GF_HEAL  },   /* hp to full                      */
    { "GLADDEN",   25, 0,           GF_CHEER },   /* mood, which drives the tithe     */
    { "FEED",      20, 0,           GF_FEED  },   /* hunger is the top killer         */
    { "BLESS",     60, TR_BLESSED,  GF_TRAIT },
    { "TOUGHEN",   50, TR_TOUGH,    GF_TRAIT },   /* +8 melee, and survives a plague  */
    { "QUICKEN",   50, TR_FAST,     GF_TRAIT },   /* +50% speed                       */
    { "EMBOLDEN",  40, TR_BRAVE,    GF_TRAIT },   /* +20 to the will to fight         */
    { "MAKE FERTILE", 45, TR_FERTILE, GF_TRAIT },
    { "CURE",      55, 0,           GF_TRAIT },   /* lifts plague and curse: see below */
    { "RAISE UP",  90, 0,           GF_LORD  },   /* make them the lord of their town  */
};
#define N_GIFTS ((int)(sizeof GIFTS / sizeof GIFTS[0]))

static const char *gift_note(const Gift *g, const Unit *u)
{
    switch (g->op) {
    case GF_HEAL:  return u->hp > 90 ? "already whole" : "wounds close";
    case GF_CHEER: return u->happy > 60 ? "already glad" : "spirits lift";
    case GF_FEED:  return u->hunger < 20 ? "not hungry" : "belly filled";
    case GF_LORD:  return "the town obeys";
    case GF_TRAIT:
        if (!g->trait) return (u->traits & (TR_PLAGUE | TR_CURSED)) ? "the sickness leaves"
                                                                   : "nothing to cure";
        return (u->traits & g->trait) ? "already theirs" : "theirs for life";
    default: return "";
    }
}

static int gift_apply(const Gift *g, int ui)
{
    Unit *u = &mb_u[ui];
    switch (g->op) {
    case GF_HEAL:  if (u->hp > 90) return 0; u->hp = 100; return 1;
    case GF_CHEER: if (u->happy > 60) return 0; u->happy = 100; return 1;
    case GF_FEED:  if (u->hunger < 20) return 0; u->hunger = 0; return 1;
    case GF_LORD:  if (!u->village || !mb_v[u->village].alive) return 0;
                   mb_v[u->village].lord_name = u->given;
                   mb_v[u->village].lord_age  = u->age;
                   u->prof = PROF_LORD;
                   return 1;
    default:
        if (!g->trait) {
            if (!(u->traits & (TR_PLAGUE | TR_CURSED))) return 0;
            u->traits &= ~(TR_PLAGUE | TR_CONTAGIOUS | TR_CURSED);
            u->sick = 0; return 1;
        }
        if (u->traits & g->trait) return 0;
        u->traits |= g->trait; return 1;
    }
}

static void ui_draw_shape(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(28, 20, 44);
    const uint16_t LINE = MOTE_RGB565(210, 160, 250);
    mb_ui_panel(fb, 0, 0, 128, 128, LINE, FILL, 1);
    if (s_subject < 0 || s_subject >= mb_nu || !mb_u[s_subject].alive) {
        mb_ui_text(fb, 7, 30, "they are gone", MB_UI_DIM, 112);
        mb_ui_actions(fb, 0, "B<", FILL); return;
    }
    const Unit *u = &mb_u[s_subject];
    char buf[40], gn[24];
    mb_name_str(gn, sizeof gn, NK_PERSON, u->given);
    mb_ui_text(fb, 7, 6, "SHAPE", MB_UI_CREAM, 50);
    snprintf(buf, sizeof buf, "%d", mb_faith());
    mb_ui_text_r(fb, 121, 7, buf, s_ui_flash > 0.0f ? MB_UI_CREAM : MB_UI_GOLD);
    mb_ui_icon(fb, 121 - mb_ui_textw(buf) - 10, 6, 3, 3);
    /* the likeness, small: this page is about the list, not the portrait */
    mb_ui_plaque(fb, 6, 16, 22, 22, MOTE_RGB565(14, 10, 24));
    { int sh, cx, cy;
      mb_cast_pick_pub(u, mb_hash2(s_subject, u->given), &sh, &cx, &cy);
      mb_ui_blit3(fb, mb_ui_sheet(sh), cx, cy, 9, 19, 2); }
    mb_ui_text(fb, 32, 17, gn, MB_UI_CREAM, 88);
    mb_ui_text(fb, 32, 26, MB_PROF_NAME[u->prof < PROF_N ? u->prof : 0], MB_UI_DIM, 88);
    mb_ui_traits(fb, 32, 35, u->traits);
    /* the gift list, four rows at a time around the cursor */
    int first = s_ui_row - 1; if (first < 0) first = 0;
    if (first > N_GIFTS - 4) first = N_GIFTS - 4;
    for (int r = 0; r < 4; r++) {
        int i = first + r, y = 48 + r * 12;
        if (i >= N_GIFTS) break;
        const Gift *g = &GIFTS[i];
        int sel = (i == s_ui_row);
        int can = mb_faith_afford(g->cost);
        if (sel) g_api->draw_rect(fb, 5, y - 1, 118, 11, MOTE_RGB565(70, 50, 110), 1, 0, 128);
        mb_ui_text(fb, 8, y, g->name, sel ? MB_UI_CREAM : MB_UI_DIM, 84);
        snprintf(buf, sizeof buf, "%d", g->cost);
        mb_ui_text_r(fb, 120, y, buf, can ? (sel ? MB_UI_GOLD : MB_UI_DIM) : MB_UI_RED);
    }
    /* AND WHAT IT WILL DO. A price with no consequence beside it is a guess. */
    mb_ui_rule(fb, 6, 96, 116, "EFFECT", LINE, FILL, MB_UI_DIM);
    mb_ui_text(fb, 7, 104, gift_note(&GIFTS[s_ui_row], u), MB_UI_OK, 114);
    mb_ui_actions(fb, "A GIVE", "B<", FILL);
}

/* ------------------------------------------------------------------ A LORD
 * The three stats here are the highest-leverage numbers a god can touch, because the
 * simulation already reads all three every rotation — and it says so on the page, because a
 * bar with no consequence written beside it teaches nobody anything. */
static void ui_draw_lord(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(30, 24, 16);
    mb_ui_panel(fb, 0, 0, 128, 128, MB_UI_GOLD, FILL, 1);
    int v = s_subject_v;
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) {
        mb_ui_text(fb, 7, 30, "no lord here", MB_UI_DIM, 112);
        mb_ui_actions(fb, 0, "B<", FILL); return;
    }
    Village *V = &mb_v[v];
    char ln[24], pl[24], buf[40];
    mb_name_str(ln, sizeof ln, NK_PERSON, V->lord_name);
    mb_name_str(pl, sizeof pl, NK_PLACE, V->name);
    snprintf(buf, sizeof buf, "LORD %s", ln);
    mb_ui_text(fb, 7, 7, buf, MB_UI_CREAM, 78);
    snprintf(buf, sizeof buf, "%d", mb_faith());
    mb_ui_text_r(fb, 121, 7, buf, MB_UI_GOLD);
    snprintf(buf, sizeof buf, "%s  aged %d", pl, V->lord_age);
    mb_ui_text(fb, 7, 16, buf, MB_UI_DIM, 114);
    /* THE LORD'S OWN FACE — the CR_LORD figures were hand-picked for all four races and had
     * never been drawn anywhere, because no unit is ever given PROF_LORD. */
    mb_ui_plaque(fb, 6, 26, 26, 26, MOTE_RGB565(14, 12, 20));
    { int sh, cx, cy;
      mb_cast_role(V->sp, mb_cast_role_lord(), (unsigned)v * 7919u, &sh, &cx, &cy);
      mb_ui_blit3(fb, mb_ui_sheet(sh), cx, cy, 9, 29, 2); }
    static const char *const SN[3] = { "STEWARD", "DIPLOMAT", "WARLORD" };
    const int val[3] = { V->lord_stew, V->lord_diplo, V->lord_war };
    for (int i = 0; i < 3; i++) {
        int y = 27 + i * 9, sel = (i == s_ui_row);
        if (sel) g_api->draw_rect(fb, 34, y - 1, 88, 10, MOTE_RGB565(76, 60, 26), 1, 0, 128);
        mb_ui_text(fb, 36, y, SN[i], sel ? MB_UI_CREAM : MB_UI_DIM, 46);
        mb_ui_pips(fb, 84, y + 2, (val[i] + 19) / 20, 5, MB_UI_OK, MB_UI_OFF);
    }
    /* WHAT THAT MEANS — the real thresholds, read back in words */
    mb_ui_rule(fb, 6, 56, 116, "CONSEQUENCE", MB_UI_GOLD, FILL, MB_UI_DIM);
    int hawk = V->lord_war
             + ((V->lord_traits & TR_AMBITIOUS) ? 25 : 0)
             + ((V->lord_traits & TR_VENGEFUL)  ? 30 : 0)
             + ((V->lord_traits & TR_MADNESS)   ? 40 : 0);
    int loyal = (V->lord_diplo >> 2) + ((V->lord_traits & TR_LOYAL) ? 25 : 0)
              - ((V->lord_traits & TR_AMBITIOUS) ? 45 : 0);
    int ln2 = 65;
    mb_ui_text(fb, 7, ln2, hawk >= 80 ? "strikes first" :
                           hawk >= 60 ? "wants a war" : "keeps the peace",
               hawk >= 80 ? MB_UI_RED : hawk >= 60 ? MB_UI_GOLD : MB_UI_OK, 114);
    ln2 += 9;
    mb_ui_text(fb, 7, ln2, loyal < 0  ? "rebellion near" :
                           loyal < 15 ? "loyalty is thin" : "loyal to it",
               loyal < 0 ? MB_UI_RED : loyal < 15 ? MB_UI_GOLD : MB_UI_OK, 114);
    ln2 += 9;
    mb_ui_text(fb, 7, ln2, V->lord_stew >= 60 ? "builds boldly" :
                           V->lord_stew >= 30 ? "builds if it can" : "builds little",
               V->lord_stew >= 60 ? MB_UI_OK : MB_UI_DIM, 114);
    mb_ui_rule(fb, 6, 92, 116, "TEMPERAMENT", MB_UI_GOLD, FILL, MB_UI_DIM);
    mb_ui_traits(fb, 7, 100, V->lord_traits);
    snprintf(buf, sizeof buf, "A RAISE %d", MB_LORD_RAISE);
    mb_ui_actions(fb, buf, "B<", FILL);
}

/* ------------------------------------------------------------------ A TOWN
 * Everything the town-development loops produce, as things you can see rather than a column of
 * numbers: its own hall at double size, the stores as meters in the artist's colours, and a
 * strip of the buildings that ACTUALLY STAND there drawn from the town sheet. */
static void ui_draw_town(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(18, 30, 26);
    const uint16_t LINE = MOTE_RGB565(120, 220, 130);
    mb_ui_panel(fb, 0, 0, 128, 128, LINE, FILL, 1);
    int v = s_subject_v;
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) {
        mb_ui_text(fb, 7, 26, "no town here", MB_UI_DIM, 112);
        mb_ui_actions(fb, "RB NEXT", "B<", FILL); return;
    }
    Village *V = &mb_v[v];
    char buf[40], pl[24];
    mb_name_str(pl, sizeof pl, NK_PLACE, V->name);
    mb_ui_text(fb, 7, 6, pl, MB_UI_CREAM, 98);
    /* the banner, so whose town this is needs no reading */
    { int k = V->kingdom;
      uint16_t kc = (k && mb_k[k].alive) ? mb_kingdom_colour(k) : MB_UI_OFF;
      g_api->draw_rect(fb, 112, 6, 8, 8, kc, 1, 0, 128); }
    mb_ui_text(fb, 7, 15, MB_TIER_NAME[V->tier < TIER_N ? V->tier : 0], MB_UI_GOLD, 56);
    mb_ui_text_r(fb, 120, 15, MB_PROF_NAME[V->spec < PROF_N ? V->spec : 0], MB_UI_DIM);
    /* its own hall, at double size */
    mb_ui_plaque(fb, 5, 24, 30, 32, MOTE_RGB565(10, 18, 16));
    { int hall = (V->hall >= 3) ? O_HALL3 : (V->hall >= 2) ? O_HALL2 : O_HALL1;
      int k = V->kingdom;
      int col = mb_draw_form_col((uint8_t)hall, k, V->x, V->y);
      int row = (k && mb_k[k].alive) ? mb_k[k].colour % 5 : 4;
      g_api->blit_ex(fb, mb_ui_town_sheet(), 20.0f, 40.0f,
                     col * 8, row * 14, 8, 14, 0.0f, 2.0f, 0, 0, 128); }
    snprintf(buf, sizeof buf, "%d of %d", V->pop, V->housing);
    mb_ui_text(fb, 40, 25, buf, MB_UI_CREAM, 80);
    mb_ui_meter(fb, 40, 34, 7, V->pop, V->housing ? V->housing : 1, 0);
    mb_ui_text(fb, 40, 45, MB_CREED_NAME[V->creed < CREED_N ? V->creed : 0], MB_UI_DIM, 80);
    mb_ui_rule(fb, 6, 58, 116, "STOCKPILE", LINE, FILL, MB_UI_DIM);
    { static const uint8_t IC[4][2] = { { 1, 3 }, { 3, 5 }, { 5, 3 }, { 3, 3 } };
      const int val[4] = { V->food, V->wood, V->stone, V->gold };
      for (int i = 0; i < 4; i++) {
          int x = 7 + (i & 1) * 58, y = 67 + (i >> 1) * 11;
          mb_ui_icon(fb, x, y, IC[i][0], IC[i][1]);
          mb_ui_meter(fb, x + 11, y, 4, val[i], 120, 0);
      } }
    mb_ui_rule(fb, 6, 90, 116, "BUILDINGS", LINE, FILL, MB_UI_DIM);
    /* what actually stands: walk the claim once and show the first eight kinds found */
    { int shown = 0;
      for (int o = O_BUILD0 + 1; o < O_N && shown < 8; o++) {
          if (o == O_PLAN || !mb_civ_count(v, o)) continue;
          int k = V->kingdom;
          int col = mb_draw_form_col((uint8_t)o, k, V->x + shown, V->y);
          int row = (k && mb_k[k].alive) ? mb_k[k].colour % 5 : 4;
          g_api->blit(fb, mb_ui_town_sheet(), 7 + shown * 14, 98,
                      col * 8, row * 14, 8, 14, 0, 0, 128);
          shown++;
      }
      if (!shown) mb_ui_text(fb, 7, 101, "nothing built", MB_UI_DIM, 112); }
    mb_ui_actions(fb, "RB NEXT", "B<", FILL);
}

/* ------------------------------------------------------------------ A KINGDOM */
static void ui_draw_king(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(34, 28, 14);
    mb_ui_panel(fb, 0, 0, 128, 128, MB_UI_GOLD, FILL, 1);
    int k = (s_subject_v > 0 && s_subject_v < MAXV) ? mb_v[s_subject_v].kingdom : 0;
    if (k <= 0 || k >= MAXK || !mb_k[k].alive) {
        mb_ui_text(fb, 7, 26, "no crown here", MB_UI_DIM, 112);
        mb_ui_actions(fb, "RB NEXT", "B<", FILL); return;
    }
    Kingdom *K = &mb_k[k];
    char buf[40], kn[24];
    mb_name_str(kn, sizeof kn, NK_KINGDOM, K->name);
    g_api->draw_rect(fb, 7, 6, 8, 8, mb_kingdom_colour(k), 1, 0, 128);
    mb_ui_text(fb, 19, 6, kn, MB_UI_CREAM, 100);
    mb_ui_text(fb, 7, 15, MB_ERA_NAME[K->era < ERA_N ? K->era : 0], MB_UI_DIM, 112);
    mb_ui_rule(fb, 6, 27, 116, "RESEARCH", MB_UI_GOLD, FILL, MB_UI_DIM);
    snprintf(buf, sizeof buf, "%d of %d known", K->tech, TECH_N - 1);
    mb_ui_text(fb, 7, 36, buf, MB_UI_CREAM, 112);
    mb_ui_meter(fb, 7, 45, 14, K->tech, TECH_N - 1, 0);
    if (K->goal) {
        snprintf(buf, sizeof buf, "next: %s", MB_TECH_NAME[K->goal]);
        mb_ui_text(fb, 7, 55, buf, MB_UI_GOLD, 112);
    }
    mb_ui_rule(fb, 6, 65, 116, "SETTLEMENTS", MB_UI_GOLD, FILL, MB_UI_DIM);
    /* one bar per town, its height the tier: a realm's shape at a glance */
    { int n = 0;
      for (int i = 1; i < MAXV && n < 8; i++) {
          if (!mb_v[i].alive || mb_v[i].kingdom != k) continue;
          int t = mb_v[i].tier < TIER_N ? mb_v[i].tier : 0;
          int h = 4 + t * 3;
          g_api->draw_rect(fb, 8 + n * 14, 86 - h, 9, h,
                           i == s_subject_v ? MB_UI_CREAM : MB_UI_OK, 1, 0, 128);
          n++;
      }
      if (!n) mb_ui_text(fb, 7, 76, "no towns left", MB_UI_DIM, 112); }
    mb_ui_rule(fb, 6, 90, 116, "AT WAR", MB_UI_GOLD, FILL, MB_UI_DIM);
    { int n = 0;
      for (int o = 1; o < MAXK && n < 3; o++) {
          if (o == k || !mb_k[o].alive || !(K->war_with & (1u << o))) continue;
          char on[24]; mb_name_str(on, sizeof on, NK_KINGDOM, mb_k[o].name);
          g_api->draw_rect(fb, 7 + n * 40, 99, 7, 7, mb_kingdom_colour(o), 1, 0, 128);
          mb_ui_text(fb, 17 + n * 40, 99, on, MB_UI_DIM, 26);
          n++;
      }
      if (!n) mb_ui_text(fb, 7, 99, "at peace", MB_UI_OK, 112); }
    if (mb_tech_known(k, TECH_NUKE)) mb_ui_text_r(fb, 120, 90, "THE BOMB", MB_UI_RED);
    mb_ui_actions(fb, "RB NEXT", "B<", FILL);
}

/* ------------------------------------------------------------------ THE WORLD */
static void ui_draw_world(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(16, 22, 42);
    mb_ui_panel(fb, 0, 0, 128, 128, MB_UI_SKY, FILL, 1);
    char buf[40];
    mb_ui_text(fb, 7, 6, "THE WORLD", MB_UI_CREAM, 78);
    snprintf(buf, sizeof buf, "y%d", (int)(mb_w.tick / TPY));
    mb_ui_text_r(fb, 120, 6, buf, MB_UI_DIM);
    mb_ui_text(fb, 7, 15, mb_age_name(), MB_UI_GOLD, 112);
    mb_ui_rule(fb, 6, 27, 116, "KINGDOMS", MB_UI_SKY, FILL, MB_UI_DIM);
    /* strongest first by era, which is the state of play in one column */
    { int n = 0;
      for (int era = ERA_N - 1; era >= 0 && n < 5; era--)
          for (int k = 1; k < MAXK && n < 5; k++) {
              if (!mb_k[k].alive || mb_k[k].era != era) continue;
              int towns = 0;
              for (int i = 1; i < MAXV; i++)
                  if (mb_v[i].alive && mb_v[i].kingdom == k) towns++;
              int y = 36 + n * 10;
              char kn[24]; mb_name_str(kn, sizeof kn, NK_KINGDOM, mb_k[k].name);
              g_api->draw_rect(fb, 7, y, 7, 7, mb_kingdom_colour(k), 1, 0, 128);
              mb_ui_text(fb, 17, y, kn, MB_UI_CREAM, 60);
              snprintf(buf, sizeof buf, "%d", towns);
              mb_ui_text(fb, 82, y, buf, MB_UI_DIM, 18);
              if (mb_tech_known(k, TECH_NUKE)) mb_ui_icon(fb, 100, y - 1, 11, 1);
              n++;
          }
      if (!n) mb_ui_text(fb, 7, 36, "no crowns left", MB_UI_DIM, 112); }
    mb_ui_rule(fb, 6, 88, 116, "POPULATION", MB_UI_SKY, FILL, MB_UI_DIM);
    snprintf(buf, sizeof buf, "%d souls", mb_pop_civ());
    mb_ui_text(fb, 7, 97, buf, MB_UI_CREAM, 74);
    snprintf(buf, sizeof buf, "%d", mb_village_count());
    mb_ui_text_r(fb, 120, 97, buf, MB_UI_DIM);
    mb_ui_actions(fb, "RB NEXT", "B<", FILL);
}

/* ------------------------------------------------------------------ THE LAND */
static void ui_draw_land(uint16_t *fb)
{
    const uint16_t FILL = MOTE_RGB565(26, 22, 14);
    const uint16_t LINE = MOTE_RGB565(210, 170, 110);
    mb_ui_panel(fb, 0, 0, 128, 128, LINE, FILL, 1);
    int i = AT(s_cx, s_cy);
    uint8_t b = mb_w.biome[i], o = mb_w.obj[i], fk = mb_fkind(mb_w.flux[i]);
    int v = mb_w.claim[i];
    char buf[40];
    mb_ui_text(fb, 7, 6, B_NAME[b < B_N ? b : 0], MB_UI_CREAM, 100);
    if (v && mb_v[v].alive) {
        char pl[24]; mb_name_str(pl, sizeof pl, NK_PLACE, mb_v[v].name);
        mb_ui_text(fb, 7, 15, pl, MB_UI_DIM, 78);
        mb_ui_text_r(fb, 120, 15, "held", MB_UI_DIM);
    } else {
        mb_ui_text(fb, 7, 15, "nobody's land", MB_UI_DIM, 112);
    }
    mb_ui_rule(fb, 6, 26, 116, "TERRAIN", LINE, FILL, MB_UI_DIM);
    mb_ui_text(fb, 7, 35, "height", MB_UI_DIM, 50);
    mb_ui_meter(fb, 54, 34, 7, mb_w.elev[i], 255, 0);
    mb_ui_text(fb, 7, 45, mb_w.road[i] ? "a road here" : "no road",
               MB_UI_DIM, 112);
    if (fk && fk < FX_N) mb_ui_text(fb, 7, 55, FX_NAME[fk], MB_UI_RED, 112);
    else                 mb_ui_text(fb, 7, 55, "all is still", MB_UI_DIM, 112);
    /* THE LAND AROUND IT, as God's Eye would colour it: a 7x7 of the same per-biome colours
     * the map uses, so you can see what kind of place this is without leaving the page. */
    mb_ui_rule(fb, 6, 66, 116, "SURROUNDINGS", LINE, FILL, MB_UI_DIM);
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++) {
            int x = s_cx + dx, y = s_cy + dy;
            uint16_t c = mb_in(x, y) ? mb_biome_colour(mb_w.biome[AT(x, y)])
                                     : MOTE_RGB565(10, 12, 24);
            g_api->draw_rect(fb, 8 + (dx + 3) * 5, 76 + (dy + 3) * 5, 5, 5, c, 1, 0, 128);
        }
    g_api->draw_rect(fb, 8 + 3 * 5 - 1, 76 + 3 * 5 - 1, 7, 7, MB_UI_CREAM, 0, 0, 128);
    /* and what stands on the cell itself */
    if (o && o < O_N && O_NAME[o][0]) {
        if (mb_is_build(o)) {
            int k = mb_kingdom_of(v);
            int col = mb_draw_form_col(o, k, s_cx, s_cy);
            int row = (k && mb_k[k].alive) ? mb_k[k].colour % 5 : 4;
            g_api->blit(fb, mb_ui_town_sheet(), 52, 76, col * 8, row * 14, 8, 14, 0, 0, 128);
            mb_ui_text(fb, 66, 82, O_NAME[o], MB_UI_CREAM, 54);
        } else {
            mb_ui_text(fb, 52, 82, O_NAME[o], MB_UI_CREAM, 68);
        }
    } else {
        mb_ui_text(fb, 52, 82, "bare", MB_UI_DIM, 68);
    }
    mb_ui_actions(fb, "RB NEXT", "B<", FILL);
}

/* --- THE CHRONICLE, as a log you can navigate ---------------------------
 *
 * The world pans itself to each new headline after a few seconds of no input, which is right
 * for a game you leave running and disorienting for one you are watching: you arrive
 * somewhere with no idea what happened or where you were. The chronicle was the answer and
 * was not usable as one — twenty-four lines of text about places there was no way to reach.
 *
 * So: the whole ring (ninety-six entries, scrolling), newest first, and SELECTING A LINE TAKES
 * YOU THERE. That turns the log from a wall of history into the way you get around, and it
 * turns the auto-pan from something that happens to you into something you can do on purpose.
 *
 * If the panning itself is the problem, MENU > WORLD LAWS > FOLLOW switches it off, and the
 * chronicle then becomes the only way the world tells you anything — which is why it had to
 * be navigable before that was a reasonable thing to suggest.
 */
static void chronicle_menu(void)
{
    /* The engine menu is a list, and a history IS a list — so the chronicle needs no screen
     * of its own, and it scrolls with the same buttons as everything else. */
    for (;;) {
        int n = mb_chron_count();
        if (n <= 0) {
            const char *none[] = { "nothing has happened yet", "BACK" };
            mote->menu("EVENT LOG", none, 2);
            return;
        }
        static char rows[41][34];
        static int  back_of[41];        /* which ring entry each row is, so BACK still travels */
        const char *items[42];
        int rown = 0;
        for (int i = 0; i < n && rown < 40; i++) {
            if (!mb_chron_notable(i)) continue;      /* see mb_chron_notable() */
            int year = 0; char line[30];
            mb_chron_line(line, sizeof line, i, &year);
            back_of[rown] = i;
            /* NO MARKER AND NO COORDINATES. A log is read as history; a grid reference tells
             * you nothing you can act on, and a symbol in front of every other line is just
             * clutter. Selecting a line still travels to it — the navigation is silent. */
            snprintf(rows[rown], sizeof rows[rown], "Y%d %s", year, line);
            items[rown] = rows[rown];
            rown++;
        }
        if (rown == 0) {
            const char *none[] = { "nothing of note yet", "BACK" };
            mote->menu("EVENT LOG", none, 2);
            return;
        }
        items[rown] = "BACK";
        int c = mote->menu("EVENT LOG", items, rown + 1);
        if (c < 0 || c >= rown) return;
        int wx, wy;
        if (mb_chron_where(back_of[c], &wx, &wy)) {
            s_cx = wx; s_cy = wy;
            s_pan_x = -1;                  /* cancel any auto-pan: this was deliberate */
            s_idle = 0.0f;                 /* and do not immediately pan away again */
            if (!s_god) cam_follow();
            return;
        }
    }
}

/* --- THE WORLD REPORT ---------------------------------------------------
 * The inspect pages answer "what is this"; this answers "what is going on". Every
 * town-development loop rolled up to world scale, so a run has a readable state of play
 * without walking the cursor over forty settlements: which crown is ahead in learning,
 * how the faiths are divided, what the economy is made of, how far the biggest places
 * have got, and how much is moving on the roads.
 *
 * This is the same data MOTEBOX_LOOPS prints for the audit — the tooling and the game
 * read the simulation the same way, which is why the numbers can be trusted. */

static void god_menu(void)
{
    for (;;) {
        static char st[4][30];
        snprintf(st[0], sizeof st[0], "%s", mb_age_name());
        snprintf(st[1], sizeof st[1], "FAITH %d  (+%d/yr)", mb_faith(), mb_faith_income());
        snprintf(st[2], sizeof st[2], "MODE: %s", mb_mode() == MODE_SANDBOX ? "SANDBOX" : "PANTHEON");
        snprintf(st[3], sizeof st[3], "pop %d  villages %d  kingdoms %d",
                 mb_pop_civ(), mb_village_count(), mb_kingdom_count());
        static char st4[30];
        snprintf(st4, sizeof st4, "MAP: %s", MB_MAPMODE_NAME[mb_draw_mapmode()]);
        /* THE THINGS YOU CAME HERE TO DO GO FIRST. This menu used to open on four rows of
         * status, so the log was the seventh line down a screen that did not look like a
         * list of actions — and it was called CHRONICLE, which is a lovely word and not a
         * word anybody hunting for a log would read as one. Both fixed: it is the top row,
         * and it says what it is. */
        const char *items[] = { "EVENT LOG", "THE WORLD REPORT",
                                st4, st[0], st[1], st[2], st[3],
                                "WORLD LAWS", "SAVE WORLD", "LOAD WORLD",
                                "NEW WORLD", "CLOSE" };
        int c = mote->menu("MOTEBOX", items, 12);
        switch (c) {
        case 0: chronicle_menu(); break;
        case 1: s_ui = UI_WORLD; s_ui_row = 0; return;   /* the drawn screen */
        /* THE MAP IS A LENS. Cycling it here rather than on a button keeps the world's own
         * controls for the world, and the four modes are the only place tech, creed and tier
         * are visible at world scale — see mb_draw_prepare(). */
        case 2: mb_draw_mapmode_set((mb_draw_mapmode() + 1) % MAPMODE_N);
                mb_draw_init(); break;
        case 5: mb_mode_set(mb_mode() == MODE_SANDBOX ? MODE_PANTHEON : MODE_SANDBOX); break;
        case 7: law_menu(); break;
        case 8: {
            int ok = mb_save_write(0, s_cx, s_cy, s_god);
            const char *m[] = { ok ? "saved" : "SAVE FAILED", "BACK" };
            mote->menu("SAVE", m, 2);
            break;
        }
        case 9: {
            int cx = s_cx, cy = s_cy, god = s_god;
            int ok = mb_save_read(0, &cx, &cy, &god);
            if (ok) { s_cx = cx; s_cy = cy; view_set(god); }
            const char *m[] = { ok ? "loaded" : "NO SAVE", "BACK" };
            mote->menu("LOAD", m, 2);
            break;
        }
        case 10: {
            uint32_t ns = (uint32_t)mote->micros() ^ (mb_w.seed * 2654435761u);
            mb_world_gen(ns);
            /* RESET, not init: the arena is bump-only, so re-initialising would
             * leak a whole world's worth of buffers per reroll. */
            mb_unit_reset(); mb_civ_reset(); mb_flux_reset();
            mb_chron_init(); mb_age_init(); mb_faith_init(); mb_fx_init();
            mb_unit_seed_wildlife();
    /* AND THEN THE PEOPLES. A fresh world used to contain deer, goats and a wolf and
     * nothing else, because founding a civilisation was a power you had to know about.
     * Boot into a living history instead. */
    mb_civ_seed_world(4);
    mb_world_start(&s_cx, &s_cy);
    /* AND OPEN ON A CIVILISATION. Worldgen picks a pleasant spot, which on a 96x96 map
     * is usually nobody's spot — so the first thing the player saw was empty coastline
     * even once the world had peoples in it. Start the cursor on a founding party: the
     * opening frame should contain the thing the game is about. */
    for (int i = 1; i < MAXV; i++)
        if (mb_v[i].alive) { s_cx = mb_v[i].x; s_cy = mb_v[i].y; break; }
            return;
        }
        default: return;
        }
    }
}

#if MOTE_HOST
/* --- the fast-forward (MOTEBOX_YEARS=500) ------------------------------
 * One sim tick has no rendering in it, so a headless run does not need frames at
 * all: this spins the world forward as fast as the machine will go and prints a
 * yearly CSV. It is what makes balance a measurement instead of an argument —
 * every finding in this file's history came out of a curve, not a screenshot. */
static void fast_forward(int years)
{
    fprintf(stderr, "year,pop,wild,villages,kingdoms,wars,faith,ruin,age,"
                    "d_age,d_wound,d_eaten,d_slain,d_disaster,d_plague,d_starved\n");
    for (int y = 0; y < years; y++) {
        for (int t = 0; t < 52; t++) {
            mb_w.tick++;
            mb_flux_step();
            mb_flux_natural();
            mb_unit_step();
            mb_unit_plague_step();
            mb_unit_madness_step();
            mb_unit_migrate();
            mb_civ_step();
            mb_grow_step();
            mb_grave_step();
            mb_bands_rebuild();
            mb_faith_step();
            mb_age_step();
        }
        int wars = 0;
        for (int k = 1; k < MAXK; k++) if (mb_k[k].alive && mb_k[k].war_with) wars++;
        int ruin = 0;
        for (int i = 0; i < NC; i += 7) {
            uint8_t b = mb_w.biome[i];
            if (b == B_ASH || b == B_SCORCHED || b == B_RUBBLE) ruin++;
        }
        fprintf(stderr, "%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d\n",
                y, mb_pop_civ(), mb_pop_wild(), mb_village_count(), mb_kingdom_count(),
                wars, mb_faith(), ruin * 700 / NC, mb_age_name(),
                mb_deaths_by(CAUSE_AGE), mb_deaths_by(CAUSE_WOUNDS),
                mb_deaths_by(CAUSE_EATEN), mb_deaths_by(CAUSE_SLAIN),
                mb_deaths_by(CAUSE_DISASTER), mb_deaths_by(CAUSE_PLAGUE),
                mb_deaths_by(CAUSE_STARVED));
    }
}
#endif

/* --- vtbl -------------------------------------------------------------- */

static void g_init(void)
{
    /* A WORLD SEED THAT ACTUALLY VARIES.
     *
     * This was the constant 0x1d0f2c31 — so every session of the game, on every
     * device, generated the SAME WORLD. For a god sim whose entire replay value is
     * "what is this world like", that is close to fatal, and it is invisible to any
     * test, because every test passes its own MOTEBOX_SEED.
     *
     * Two sources, mixed, because neither is trustworthy alone: micros() at init
     * varies with how long the launcher took to hand over, which on a cold boot into
     * the same game can be nearly identical; and a COUNTER kept in the save store,
     * bumped every time a world is made, which cannot repeat even if the clock is
     * useless. Whichever one is working carries the other. */
    uint32_t roll = 0;
    /* `mote`, not g_api: g_api is handed to the other translation units further down
     * this function, and reaching for it here dereferenced NULL and dumped core. */
    mote->kv_load("mb_roll", &roll, sizeof roll);
    roll++;
    mote->kv_save("mb_roll", &roll, sizeof roll);
    uint32_t seed = (uint32_t)mote->micros();
    seed ^= roll * 2654435761u;
    seed = (seed ^ (seed >> 15)) * 2246822519u;
    seed ^= seed >> 13;
    if (!seed) seed = 0x1d0f2c31u;
#if MOTE_HOST
    const char *e = getenv("MOTEBOX_SEED");
    if (e && *e) seed = (uint32_t)strtoul(e, 0, 0);
    e = getenv("MOTEBOX_PERF"); s_perf = (e && *e && *e != '0');
    e = getenv("MOTEBOX_STAT"); s_stat = (e && *e && *e != '0');
    /* MOTEBOX_CAST="fire@70,60;meteor@64,40" — fired on frame 12, once, so a
     * disaster can be tested where there is something for it to act on. */
    e = getenv("MOTEBOX_TRACE"); s_trace = (e && *e && *e != '0');
    e = getenv("MOTEBOX_CAST");
    if (e && *e) snprintf(s_cast_script, sizeof s_cast_script, "%s", e);
#endif
    g_api = mote;                 /* hand the api to the other TUs (see mb.h) */
    mote->set_fps_limit(30);
    mote->scene_set_background(C_HUDBG);
    mb_world_alloc();
    mb_world_gen(seed);
    mb_flux_init();
    mb_fx_init();
    mb_unit_init();
    mb_civ_init();
    mb_chron_init();
    mb_age_init();
    mb_faith_init();
    mb_save_init();
    mb_unit_seed_wildlife();
    /* AND THEN THE PEOPLES. A fresh world used to contain deer, goats and a wolf and
     * nothing else, because founding a civilisation was a power you had to know about.
     * Boot into a living history instead. */
    mb_civ_seed_world(4);
    mb_draw_init();
#if MOTE_HOST
    {   /* MOTEBOX_SEEDV=x,y;x,y drops founding parties before the fast-forward,
         * because a world with nobody in it has no history to measure. */
        const char *sv = getenv("MOTEBOX_SEEDV");
        if (sv) {
            while (*sv) {
                int x = 0, y = 0;
                while (*sv >= '0' && *sv <= '9') x = x * 10 + (*sv++ - '0');
                if (*sv == ',') sv++;
                while (*sv >= '0' && *sv <= '9') y = y * 10 + (*sv++ - '0');
                fprintf(stderr, "drop village at %d,%d -> %d\n", x, y,
                        mb_civ_drop_village(SP_HUMAN, x, y));
                if (*sv == ';') sv++; else break;
            }
        }
        /* MOTEBOX_SEEDN=n drops n founding parties on LAND, spread across the map.
         *
         * The audit used to name five fixed coordinates, and on an ocean-heavy or
         * archipelago world all five landed in the sea — so four of eight audited
         * worlds ran four hundred years with nobody in them and reported "ok". A
         * test that passes because it tested nothing is worse than no test. */
        const char *sn = getenv("MOTEBOX_SEEDN");
        if (sn) {
            int want = atoi(sn), got = 0;
            for (int step = 0; step < 4000 && got < want; step++) {
                uint32_t r = mb_rand((uint32_t)step * 2654435761u + 0x5eedu);
                int x = (int)(r % MW), y = (int)((r >> 11) % MH);
                if (!mb_in(x, y) || !mb_land(mb_w.biome[AT(x, y)])) continue;
                int clear = 1;                     /* keep founders apart */
                for (int v = 0; v < MAXV && clear; v++)
                    if (mb_v[v].alive) {
                        int dx = mb_v[v].x - x, dy = mb_v[v].y - y;
                        if (dx * dx + dy * dy < 18 * 18) clear = 0;
                    }
                if (!clear) continue;
                if (mb_civ_drop_village((uint8_t)(SP_HUMAN + got % SP_CIV_N), x, y) >= 0) {
                    fprintf(stderr, "drop village at %d,%d\n", x, y);
                    got++;
                }
            }
            fprintf(stderr, "seeded %d/%d founding parties\n", got, want);
        }

        const char *yy = getenv("MOTEBOX_YEARS");
        if (yy && *yy) fast_forward(atoi(yy));
    }
#endif
    mb_world_start(&s_cx, &s_cy);
    /* AND OPEN ON A CIVILISATION. Worldgen picks a pleasant spot, which on a 96x96 map
     * is usually nobody's spot — so the first thing the player saw was empty coastline
     * even once the world had peoples in it. Start the cursor on a founding party: the
     * opening frame should contain the thing the game is about. */
    for (int i = 1; i < MAXV; i++)
        if (mb_v[i].alive) { s_cx = mb_v[i].x; s_cy = mb_v[i].y; break; }
#if MOTE_HOST
    /* MOTEBOX_CAM=x,y parks the camera on a tile, or CAM=v parks it on the biggest
     * village. Without this every visual check landed wherever worldgen chose, which
     * was usually empty wilderness — three rounds of "where are the civilisations?"
     * were partly me photographing the wrong 16x14 tiles of a 96x96 world. */
    {
        const char *cv = getenv("MOTEBOX_CAM");
        if (cv && *cv == 'v') {
            int best = -1;
            for (int i = 0; i < MAXV; i++)
                if (mb_v[i].alive && (best < 0 || mb_v[i].pop > mb_v[best].pop)) best = i;
            if (best >= 0) { s_cx = mb_v[best].x; s_cy = mb_v[best].y;
                             fprintf(stderr, "cam -> village %d pop %d at %d,%d\n",
                                     best, mb_v[best].pop, s_cx, s_cy); }
            else fprintf(stderr, "cam -> no village alive\n");
        } else if (cv && *cv == 'f') {
            /* CAM=f parks on FARMLAND, because the fields are wherever the good soil was and
             * CAM=v lands on a town that may have none in shot. */
            int fx = -1, fy = -1, bestn = 0;
            for (int y = 4; y < MH - 4 && 1; y += 2)
                for (int x = 4; x < MW - 4; x += 2) {
                    int n = 0;
                    for (int dy = -3; dy <= 3; dy++)
                        for (int dx = -3; dx <= 3; dx++)
                            if (mb_in(x + dx, y + dy)
                                && mb_w.biome[AT(x + dx, y + dy)] == B_FARM) n++;
                    if (n > bestn) { bestn = n; fx = x; fy = y; }
                }
            if (fx >= 0) { s_cx = fx; s_cy = fy;
                           fprintf(stderr, "cam -> %d field cells around %d,%d\n",
                                   bestn, fx, fy); }
            else fprintf(stderr, "cam -> no farmland anywhere\n");
        } else if (cv && *cv == 'u') {
            /* CAM=u parks on a PERSON. The soul screen is the richest page in the game and
             * CAM=v lands on a hall, where the only thing to inspect is masonry. */
            for (int i = 0; i < mb_nu; i++)
                if (mb_u[i].alive && mb_u[i].sp < SP_CIV_N && mb_u[i].village) {
                    s_cx = mb_u[i].x >> 4; s_cy = mb_u[i].y >> 4;
                    fprintf(stderr, "cam -> a person at %d,%d\n", s_cx, s_cy);
                    break;
                }
        } else if (cv && *cv == 'w') {
            /* CAM=w parks on the thickest run of city WALL, which is the only way to see
             * whether the forty-seven-cell sets are turning corners. */
            int bx = -1, by = 0, best = 0, total = 0;
            for (int i = 0; i < NC; i++) if (mb_w.obj[i] == O_WALL) total++;
            for (int y = 3; y < MH - 3; y++)
                for (int x = 3; x < MW - 3; x++) {
                    int n = 0;
                    for (int dy = -3; dy <= 3; dy++)
                        for (int dx = -3; dx <= 3; dx++)
                            if (mb_in(x + dx, y + dy)
                                && mb_w.obj[AT(x + dx, y + dy)] == O_WALL) n++;
                    if (n > best) { best = n; bx = x; by = y; }
                }
            if (bx >= 0) { s_cx = bx; s_cy = by;
                           fprintf(stderr, "cam -> %d wall cells in the world, %d around %d,%d\n",
                                   total, best, bx, by); }
            else fprintf(stderr, "cam -> no walls anywhere\n");
        } else if (cv && *cv == 'm') {
            /* CAM=m parks on a PLAZA. CAM=v aims at a village centre, and a plaza is two
             * rows south of the hall — close enough to be in shot, far enough that "is the
             * monument there" was answered by scanning a screenshot for a small pale column. */
            int fx = -1, fy = -1;
            for (int y = 0; y < MH && fx < 0; y++)
                for (int x = 0; x < MW; x++)
                    if (mb_w.obj[AT(x, y)] == O_MONUMENT) { fx = x; fy = y; break; }
            if (fx >= 0) {
                s_cx = fx; s_cy = fy;
                fprintf(stderr, "cam -> monument at %d,%d\n", fx, fy);
                /* and the plaza around it as text, because "is the square paved" is a
                 * question about eight cells and reading it off a screenshot is guesswork:
                 * '#' paved, letters a building, '.' bare, ',' planted */
                for (int y = fy - 4; y <= fy + 3; y++) {
                    char row[20]; int n = 0;
                    for (int x = fx - 5; x <= fx + 5; x++) {
                        if (!mb_in(x, y)) { row[n++] = ' '; continue; }
                        int i = AT(x, y); uint8_t o = mb_w.obj[i];
                        row[n++] = (o == O_MONUMENT) ? 'M' : (o == O_FOUNTAIN) ? 'F'
                                 : (o == O_HALL1 || o == O_HALL2 || o == O_HALL3) ? 'H'
                                 : mb_is_build(o) ? 'B' : mb_w.road[i] ? '#'
                                 : o ? ',' : '.';
                    }
                    row[n] = 0; fprintf(stderr, "  %s\n", row);
                }
            }
            else fprintf(stderr, "cam -> no monument standing\n");
        } else if (cv) {
            int x = 0, y = 0;
            while (*cv >= '0' && *cv <= '9') x = x * 10 + (*cv++ - '0');
            if (*cv == ',') cv++;
            while (*cv >= '0' && *cv <= '9') y = y * 10 + (*cv++ - '0');
            s_cx = x; s_cy = y;
        }
    }
    /* MOTEBOX_VSTAT=1 dumps every living village's ledger. "Starvation is the top
     * killer" has two completely different causes — granaries full and the feeding
     * broken, or granaries empty and production short — and no amount of staring at
     * the aggregate CSV tells them apart. */
    /* MOTEBOX_FIRETEST=1 sets fire to the biggest village and reports, tick by tick,
     * whether the town saves itself: how much is burning, how many buildings are still
     * standing, how many people are beating at it and how many it has killed. "Fire
     * should be a contest" is a claim about a CURVE, and only a curve can settle it. */
    { extern int mb_nodouse; const char *nd = getenv("MOTEBOX_NODOUSE");
      mb_nodouse = (nd && *nd && *nd != '0'); }
    if (getenv("MOTEBOX_FIRETEST")) {
        int best = -1;
        for (int i = 1; i < MAXV; i++)
            if (mb_v[i].alive && (best < 0 || mb_v[i].pop > mb_v[best].pop)) best = i;
        if (best >= 0) {
            int fx0 = mb_v[best].x, fy0 = mb_v[best].y;
            int b0 = 0;
            for (int c = 0; c < NC; c++) if (mb_is_build(mb_w.obj[c])) b0++;
            int before = 0;
            for (int c = 0; c < NC; c++) if (mb_fkind(mb_w.flux[c]) == FX_FIRE) before++;
            int nearv = 0;
            for (int k = 0; k < mb_nu; k++) {
                if (!mb_u[k].alive || mb_u[k].sp >= SP_CIV_N) continue;
                int dx = (mb_u[k].x >> 4) - fx0, dy = (mb_u[k].y >> 4) - fy0;
                if (dx * dx + dy * dy <= 25) nearv++;
            }
            fprintf(stderr, "village at %d,%d pop %d; %d civ units within 5 tiles; "
                            "%d buildings; %d cells already burning\n",
                    fx0, fy0, mb_v[best].pop, nearv, b0, before);
            const char *pw = getenv("MOTEBOX_FIREPOWER");
            mb_power_cast_named(pw && *pw ? pw : "FIRE", fx0, fy0);
            int after = 0;
            for (int c = 0; c < NC; c++) if (mb_fkind(mb_w.flux[c]) == FX_FIRE) after++;
            fprintf(stderr, "the cast lit %d cells\n", after - before);
            fprintf(stderr, "tick burning built dousing fleeing pop\n");
            for (int t = 0; t <= 60; t++) {
                if (t) { mb_flux_step(); mb_unit_step(); mb_civ_step(); mb_w.tick++; }
                int burning = 0, built = 0, dousing = 0, fleeing = 0, pop = 0;
                for (int c = 0; c < NC; c++) {
                    if (mb_fkind(mb_w.flux[c]) == FX_FIRE) burning++;
                    if (mb_is_build(mb_w.obj[c])) built++;
                }
                for (int k = 0; k < mb_nu; k++) {
                    if (!mb_u[k].alive || mb_u[k].sp >= SP_CIV_N) continue;
                    pop++;
                    if (mb_u[k].job == JOB_DOUSE) dousing++;
                    if (mb_u[k].job == JOB_FLEE)  fleeing++;
                }
                if (t % 5 == 0) {
                    /* why is nobody dousing? count the preconditions separately */
                    int near_any = 0, near_claimed = 0, in_fire = 0;
                    for (int k = 0; k < mb_nu; k++) {
                        const Unit *uu = &mb_u[k];
                        if (!uu->alive || uu->sp >= SP_CIV_N || !uu->village) continue;
                        int ux = uu->x >> 4, uy = uu->y >> 4;
                        if (mb_fkind(mb_w.flux[AT(ux, uy)]) == FX_FIRE) { in_fire++; continue; }
                        int seen = 0, claimed = 0;
                        for (int dy = -3; dy <= 3; dy++)
                            for (int dx = -3; dx <= 3; dx++) {
                                int nx = ux + dx, ny = uy + dy;
                                if (!mb_in(nx, ny)) continue;
                                int ni = AT(nx, ny);
                                if (mb_fkind(mb_w.flux[ni]) != FX_FIRE) continue;
                                seen = 1;
                                if (mb_w.claim[ni] == uu->village) claimed = 1;
                            }
                        near_any += seen; near_claimed += claimed;
                    }
                    fprintf(stderr, "%4d %7d %6d %7d %7d %4d   near=%d claimed=%d infire=%d\n",
                            t, burning, built, dousing, fleeing, pop,
                            near_any, near_claimed, in_fire);
                }
            }
        }
    }
    /* MOTEBOX_WILDFIRE=1 lights ONE forest cell and measures the front.
     *
     * "It moves too fast and stays burning in the centre too long" is a claim about a
     * SHAPE, and a screenshot cannot settle it — a disc and a ring look similar in orange.
     * So this prints, per tick, how many cells are alight, how far the front has got, and
     * how THICK the burning band is: r_max - r_min. A wave is a thin band whose radius
     * grows steadily; a disc is a band as thick as its own radius. That one number is the
     * whole difference, and it is what the tuning below was fitted to.
     */
    if (getenv("MOTEBOX_WILDFIRE")) {
        /* MOTEBOX_WILDFIRE=savanna picks a different fuel: fine fuel and coarse fuel are
         * meant to burn with different characters now, so both have to be measurable. */
        const char *wf = getenv("MOTEBOX_WILDFIRE");
        uint8_t want = B_FOREST;
        if (wf) {
            if (!strcmp(wf, "savanna")) want = B_SAVANNA;
            else if (!strcmp(wf, "grass")) want = B_GRASS;
            else if (!strcmp(wf, "meadow")) want = B_MEADOW;
        }
        int fx0 = -1, fy0 = -1, bestn = 0;
        for (int y = 4; y < MH - 4; y += 3) {          /* the deepest such fuel we can find */
            for (int x = 4; x < MW - 4; x += 3) {
                if (mb_w.biome[AT(x, y)] != want) continue;
                int n = 0;
                for (int dy = -4; dy <= 4; dy++)
                    for (int dx = -4; dx <= 4; dx++)
                        if (mb_w.biome[AT(x + dx, y + dy)] == want) n++;
                if (n > bestn) { bestn = n; fx0 = x; fy0 = y; }
            }
        }
        if (fx0 < 0) fprintf(stderr, "wildfire: no %s in this world\n", B_NAME[want]);
        else {
            fprintf(stderr, "wildfire at %d,%d (%d/81 %s around it)\n", fx0, fy0, bestn, B_NAME[want]);
            mb_w.flux[AT(fx0, fy0)] = (uint8_t)((FX_FIRE << 4) | 12);
            fprintf(stderr, "tick  burning   r_min r_max  band   burnt\n");
            for (int t = 0; t <= 90; t++) {
                if (t) { mb_flux_step(); mb_w.tick++; }
                int burning = 0, burnt = 0, rmin = 999, rmax = 0;
                for (int y = 0; y < MH; y++)
                    for (int x = 0; x < MW; x++) {
                        int i = AT(x, y);
                        int dx = x - fx0, dy = y - fy0;
                        int d2 = dx * dx + dy * dy, d = 0;
                        while ((d + 1) * (d + 1) <= d2) d++;
                        if (mb_fkind(mb_w.flux[i]) == FX_FIRE) {
                            burning++;
                            if (d < rmin) rmin = d;
                            if (d > rmax) rmax = d;
                        }
                        if (mb_w.biome[i] == B_ASH || mb_w.biome[i] == B_SCORCHED) burnt++;
                    }
                if (t % 5 == 0)
                    fprintf(stderr, "%4d %8d %7d %5d %5d %7d\n", t, burning,
                            burning ? rmin : 0, rmax, burning ? rmax - rmin : 0, burnt);
                if (!burning && t > 2) { fprintf(stderr, "out at tick %d\n", t); break; }
            }
        }
    }
    /* MOTEBOX_INSPECT=1 prints the three inspect pages for the cursor's cell.
     *
     * The MENUS these came from are gone — B opens the drawn screens now. The page builders
     * stay because this hook is the only thing that dumps a cell's whole state as TEXT, which
     * is what a balance question wants; a screenshot of a panel is not greppable. */
    if (getenv("MOTEBOX_INSPECT")) {
        const char *items[IR_ROWS + 4];
        int v = ir_village(s_cx, s_cy);
        static const char *const TT[3] = { "HERE", "THE TOWN", "THE CROWN" };
        for (int page = 0; page < 3; page++) {
            int n = (page == 0) ? page_here(items, s_cx, s_cy)
                  : (page == 1) ? page_town(items, v)
                                : page_kingdom(items, v);
            fprintf(stderr, "\n=== %s (%d rows) ===\n", TT[page], n);
            for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", items[i]);
        }
    }
    /* MOTEBOX_SANDBOX=1 makes casting free. The audit of the wheel was silently useless
     * without it: the kaiju cost 300 to 550 faith and a ninety-year test world has less, so
     * every beast cast was DENIED and eight powers photographed as "nothing happens". */
    if (getenv("MOTEBOX_SANDBOX")) mb_mode_set(MODE_SANDBOX);
    /* MOTEBOX_BATTLE=1 starts a war between the two biggest kingdoms and sets a crowd of
     * both to fighting at the camera. Wars are rare and their front lines wander, so
     * "does a battle look like a battle" was not a question a screenshot could answer
     * without this — the same reason the tsunami and the strike have hooks. */
    if (getenv("MOTEBOX_BATTLE")) {
        int a = 0, b = 0;
        for (int k = 1; k < MAXK; k++) {
            if (!mb_k[k].alive) continue;
            if (!a) a = k; else if (!b) b = k;
        }
        if (a && b) {
            mb_k[a].war_with |= (1u << b);
            mb_k[b].war_with |= (1u << a);
            int put = 0;
            for (int i = 0; i < mb_nu && put < 40; i++) {
                Unit *u = &mb_u[i];
                if (!u->alive || u->sp >= SP_CIV_N) continue;
                int kk = u->village ? mb_v[u->village].kingdom : 0;
                if (kk != a && kk != b) continue;
                /* line them up either side of the cursor, a few tiles apart */
                int side = (kk == a) ? -1 : 1;
                /* CLAMPED. Placing them at s_cx +/- an offset with no bound put units off the
                 * map when the chosen town was near an edge, and a unit whose tile index is
                 * out of range segfaults the moment anything reads the terrain under it. */
                /* FOUR TO EIGHT CELLS EACH SIDE, which is a firing line.
                 *
                 * At the original three to seven both sides were in melee within a tick, so a
                 * capture of a "battle" was two crowds touching with nothing in the air. I
                 * widened it to ten-to-sixteen and it got worse, not better: nearest() searches
                 * the 3x3 grid of eight-cell buckets around a unit, so at twenty-plus cells
                 * apart neither side can SEE the other, nobody picks JOB_FIGHT, and forty
                 * mustered soldiers walk home. Measured "fighting 0" for a hundred and ninety
                 * frames. This sits inside the search and outside sword reach. */
                int px = s_cx + side * (4 + (put % 5));
                int py = s_cy - 4 + (put % 9);
                if (px < 1) px = 1; if (px > MW - 2) px = MW - 2;
                if (py < 1) py = 1; if (py > MH - 2) py = MH - 2;
                u->x = (uint16_t)(px * 16 + 8);
                u->y = (uint16_t)(py * 16 + 8);
                u->job = JOB_FIGHT; u->target = 0xFFFF;
                put++;
            }
            fprintf(stderr, "battle: kingdoms %d vs %d, %d fighters, shots %d/%d\n",
                    a, b, put, mb_war_shot_kind(a), mb_war_shot_kind(b));
        }
    }
    /* MOTEBOX_NUKE=1 fires a strike at the biggest town. A kingdom has to reach the last
     * rung of a thirty-six tech tree, build a silo and then be losing a war before this
     * happens on its own — which is right for the game and useless for looking at it. */
    if (getenv("MOTEBOX_NUKE")) {
        int best = 0;
        for (int v = 1; v < MAXV; v++)
            if (mb_v[v].alive && (!best || mb_v[v].pop > mb_v[best].pop)) best = v;
        if (best) { s_cx = mb_v[best].x; s_cy = mb_v[best].y;
                    mb_nuke_strike(mb_v[best].kingdom, s_cx, s_cy); }
    }
    /* MOTEBOX_EVENT=tsunami|sinkhole fires a world event now, so a rare disaster can be
     * watched instead of waited for. */
    {
        const char *ev = getenv("MOTEBOX_EVENT");
        if (ev && *ev) {
            mb_flux_test_event(ev, (uint32_t)mb_w.tick * 2654435761u + mb_w.seed);
            fprintf(stderr, "fired world event '%s'\n", ev);
        }
    }
    /* MOTEBOX_MAP=faith|craft|growth picks a God's Eye lens for a headless screenshot,
     * because the only other way in is a menu a script cannot drive. */
    {
        const char *mm = getenv("MOTEBOX_MAP");
        if (mm && *mm) {
            for (int i = 0; i < MAPMODE_N; i++) {
                char a = (char)(MB_MAPMODE_NAME[i][0] | 32);
                if ((*mm | 32) == a) { mb_draw_mapmode_set(i); break; }
            }
            mb_draw_init();
        }
    }
    /* MOTEBOX_LOOPS=1 reports the town-development systems, because every one of them is
     * a CURVE and none of them can be checked from a screenshot. "Does tech advance", "do
     * towns specialise by terrain", "do caravans actually arrive", "does anybody get
     * married" are four questions a picture cannot answer, and each of them has been
     * silently broken at least once — Kingdom.tech sat unused for the whole of development
     * because nothing ever printed it. */
    /* MOTEBOX_BEAST=<n> summons kaiju n at the biggest village and reports, tick by tick, what
     * it actually did to the world and whether the militia brought it down. Seven monsters that
     * all used to run the same six lines of code is exactly the sort of thing a screenshot
     * cannot tell you about, and "each one is different now" is not a claim to make from having
     * typed it. Deltas are measured over the whole map, so nothing hides. */
    {
        const char *bs = getenv("MOTEBOX_BEAST");
        if (bs && *bs) {
            extern int mb_shots_fired[MB_SHOT_N];
            int which = atoi(bs);
            static const char *const NAME[] = { "TITAN", "MEDUSA", "REAPER", "DRAGON",
                                                "GOLEM", "EYE", "ANGEL" };
            int v = 0;
            for (int i = 1; i < MAXV; i++)
                if (mb_v[i].alive && (!v || mb_v[i].pop > mb_v[v].pop)) v = i;
            if (!v) { fprintf(stderr, "beast: no village to attack\n"); }
            else {
                int b0[8] = {0}, pop0 = mb_pop_civ();
                #define TALLY(dst) do { \
                    for (int q = 0; q < 8; q++) dst[q] = 0; \
                    for (int i2 = 0; i2 < NC; i2++) { \
                        uint8_t o = mb_w.obj[i2], bm = mb_w.biome[i2]; \
                        if (mb_is_build(o)) dst[0]++; \
                        if (o == O_BOULDER) dst[1]++; \
                        if (o == O_GRAVE)   dst[2]++; \
                        if (bm == B_RUBBLE) dst[3]++; \
                        if (mb_fkind(mb_w.flux[i2]) == FX_FIRE) dst[4]++; \
                    } \
                    for (int i2 = 0; i2 < mb_nu; i2++) \
                        if (mb_u[i2].alive && (mb_u[i2].traits & TR_MADNESS)) dst[5]++; \
                } while (0)
                TALLY(b0);
                mb_agent_spawn(AG_KAIJU0 + which, mb_v[v].x - 6, mb_v[v].y);
                int shots0 = 0;
                for (int q = 0; q < MB_SHOT_N; q++) shots0 += mb_shots_fired[q];
                int gone_at = -1, last_hp = 6000, hp0 = 6000;
                for (int t = 0; t < 900; t++) {
                    mb_w.tick++;
                    mb_flux_step(); mb_unit_step(); mb_civ_step(); mb_fx_step(0.125f);
                    int ax, ay, ak, live = 0;
                    for (int ai = 0; ai < mb_agent_max(); ai++)
                        if (mb_agent_get(ai, &ax, &ay, &ak) && ak >= AG_KAIJU0) {
                            live = 1; last_hp = mb_agent_hp(ai);
                        }
                    if (!live) { gone_at = t; break; }
                }
                /* KILLED means its hit points ran out, not merely that it is no longer there:
                 * a kaiju lives 900 ticks and then leaves on its own. */
                const char *fate = (gone_at >= 0 && gone_at < 880)
                                 ? "KILLED by the militia"
                                 : (last_hp < hp0 ? "survived, but wounded"
                                                  : "survived untouched");
                int b1[8]; TALLY(b1);
                int shots1 = 0;
                for (int q = 0; q < MB_SHOT_N; q++) shots1 += mb_shots_fired[q];
                fprintf(stderr,
                    "beast %-7s builds %+d  boulders %+d  graves %+d  rubble %+d  "
                    "burning %+d  mad %+d  pop %+d  shots %d  %s\n",
                    NAME[which], b1[0]-b0[0], b1[1]-b0[1], b1[2]-b0[2], b1[3]-b0[3],
                    b1[4]-b0[4], b1[5]-b0[5], mb_pop_civ()-pop0, shots1-shots0, fate);
                fprintf(stderr, "             hp %d/6000 after %d ticks\n",
                        last_hp, gone_at >= 0 ? gone_at : 900);
                #undef TALLY
            }
        }
    }
    /* MOTEBOX_RISE=1 kills a crowd, buries them and raises the boneyard, then counts what
     * actually stood up. The rising is the one disaster whose victims become the disaster, and
     * SP_DEMON had a sprite, sixty years of life and the fastest legs in the game while being
     * spawned by nothing at all — so "do demons appear" needed counting, not hoping. */
    /* MOTEBOX_SPAWN=<n> drops a kaiju at the biggest village and RETURNS, so the fight happens
     * in the live rendered frames rather than inside a headless loop. Every previous attempt to
     * photograph a projectile used a hook that simulated the whole battle first and then handed
     * the renderer the aftermath. */
    /* MOTEBOX_SPAWN=<n> drops a kaiju by the biggest village and RETURNS, so the battle happens
     * in the live rendered frames. Every earlier attempt to photograph a projectile used a hook
     * that simulated the whole fight headlessly first and then handed the renderer the
     * aftermath — which is why three rounds of "adjust the frame number" found nothing. */
    {
        const char *spw = getenv("MOTEBOX_SPAWN");
        if (spw && *spw) {
            int v = 0;
            for (int i = 1; i < MAXV; i++)
                if (mb_v[i].alive && (!v || mb_v[i].pop > mb_v[v].pop)) v = i;
            if (v) {
                s_cx = mb_v[v].x; s_cy = mb_v[v].y;
                mb_agent_spawn(AG_KAIJU0 + atoi(spw), mb_v[v].x - 5, mb_v[v].y);
                fprintf(stderr, "spawned beast %d beside the largest village\n", atoi(spw));
            }
        }
    }
    if (getenv("MOTEBOX_RISE")) {
        int v = 0;
        for (int i = 1; i < MAXV; i++)
            if (mb_v[i].alive && (!v || mb_v[i].pop > mb_v[v].pop)) v = i;
        if (v) {
            int gx = mb_v[v].x, gy = mb_v[v].y, buried = 0;
            for (int dy = -5; dy <= 5; dy++)
                for (int dx = -5; dx <= 5; dx++) {
                    int x = gx + dx, y = gy + dy;
                    if (!mb_in(x, y) || !mb_land(mb_w.biome[AT(x, y)])) continue;
                    if (mb_is_build(mb_w.obj[AT(x, y)])) continue;
                    mb_w.obj[AT(x, y)] = O_GRAVE; buried++;
                }
            int n = mb_unit_raise_dead(gx, gy, 5);
            int w = 0, g = 0, d = 0;
            for (int i = 0; i < mb_nu; i++) {
                if (!mb_u[i].alive) continue;
                if (mb_u[i].sp == SP_WIGHT) w++;
                else if (mb_u[i].sp == SP_GHOST) g++;
                else if (mb_u[i].sp == SP_DEMON) d++;
            }
            fprintf(stderr, "rising: %d graves -> %d risen (%d wights %d ghosts %d DEMONS)\n",
                    buried, n, w, g, d);
        }
    }
    if (getenv("MOTEBOX_LOG")) {
        /* WHAT THE LOG ACTUALLY SHOWS. The engine's menu blocks, so the frame counter never
         * advances inside it and a submenu cannot be screenshotted — and "the event log has
         * only births and deaths" is a claim about the LIST, not about the ring. So the list
         * is printed exactly as chronicle_menu() builds it. */
        int n = mb_chron_count(), shown = 0;
        fprintf(stderr, "\n--- the event log, %d in the ring ---\n", n);
        for (int i = 0; i < n && shown < 20; i++) {
            if (!mb_chron_notable(i)) continue;
            int year = 0; char line[30];
            mb_chron_line(line, sizeof line, i, &year);
            fprintf(stderr, "  Y%-4d %s\n", year, line);
            shown++;
        }
        int notable = 0;
        for (int i = 0; i < n; i++) notable += mb_chron_notable(i) ? 1 : 0;
        fprintf(stderr, "  (%d of %d entries are news)\n", notable, n);
    }
    if (getenv("MOTEBOX_LOOPS")) {
        fprintf(stderr, "\n--- the loops, year %d ---\n", (int)(mb_w.tick / 52));
        for (int k = 1; k < MAXK; k++) {
            if (!mb_k[k].alive) continue;
            char kn[24]; mb_name_str(kn, sizeof kn, 1, mb_k[k].name);
            int towns = 0, pop = 0;
            for (int v = 1; v < MAXV; v++)
                if (mb_v[v].alive && mb_v[v].kingdom == k) { towns++; pop += mb_v[v].pop; }
            fprintf(stderr, "kingdom %-14s %-11s %2d/%d techs%s  goal %-13s creed %-5s "
                            "%d towns %d souls\n",
                    kn, MB_ERA_NAME[mb_k[k].era < ERA_N ? mb_k[k].era : 0],
                    mb_k[k].tech, TECH_N - 1,
                    mb_tech_known(k, TECH_NUKE) ? " BOMB" : "     ",
                    mb_k[k].goal ? MB_TECH_NAME[mb_k[k].goal] : "-",
                    MB_CREED_NAME[mb_k[k].creed < CREED_N ? mb_k[k].creed : 0], towns, pop);
        }
        {   int spec[PROF_N] = {0}, tier[TIER_N] = {0}, creed[CREED_N] = {0}, traded = 0;
            for (int v = 1; v < MAXV; v++) {
                if (!mb_v[v].alive) continue;
                spec[mb_v[v].spec < PROF_N ? mb_v[v].spec : 0]++;
                tier[mb_v[v].tier < TIER_N ? mb_v[v].tier : 0]++;
                creed[mb_v[v].creed < CREED_N ? mb_v[v].creed : 0]++;
                traded += mb_v[v].traded;
            }
            fprintf(stderr, "towns by trade:");
            for (int i = 1; i < PROF_N; i++) if (spec[i]) fprintf(stderr, " %s=%d", MB_PROF_NAME[i], spec[i]);
            fprintf(stderr, "\ntowns by tier: ");
            for (int i = 0; i < TIER_N; i++) if (tier[i]) fprintf(stderr, " %s=%d", MB_TIER_NAME[i], tier[i]);
            fprintf(stderr, "\ntowns by creed:");
            for (int i = 0; i < CREED_N; i++) if (creed[i]) fprintf(stderr, " %s=%d", MB_CREED_NAME[i], creed[i]);
            fprintf(stderr, "\ncaravans delivered (lifetime): %d\n", traded);
            /* WHAT ARE THEY ACTUALLY DOING? "They swarm around the town and nothing changes"
             * is a question about the JOB histogram, and nothing was printing it — the trade
             * census says what somebody IS, not what they are doing this tick. */
            {   int jc[JOB_N] = {0}, civ = 0;
                for (int i = 0; i < mb_nu; i++) {
                    if (!mb_u[i].alive || mb_u[i].sp >= SP_CIV_N) continue;
                    civ++;
                    jc[mb_u[i].job < JOB_N ? mb_u[i].job : 0]++;
                }
                fprintf(stderr, "doing (%d civ):", civ);
                for (int j = 0; j < JOB_N; j++)
                    if (jc[j]) fprintf(stderr, " %s=%d", JOB_NAME[j], jc[j]);
                fprintf(stderr, "\n");
            }
            /* THE METAL ECONOMY, which had none. Iron and gold were listed as build costs and
             * produced by nothing that depended on the ground, so the inspect dump read
             * `iron 0, gold 0` in every world and every expensive project needed exempting
             * from the affordability test. If these numbers are zero the economy is still
             * fictional, whatever the buildings look like. */
            {   int iron = 0, gold = 0, mines = 0, stations = 0, haveiron = 0, havegold = 0;
                for (int v = 1; v < MAXV; v++) {
                    if (!mb_v[v].alive) continue;
                    iron += mb_v[v].iron; gold += mb_v[v].gold;
                    if (mb_v[v].iron) haveiron++;
                    if (mb_v[v].gold) havegold++;
                    mines    += mb_civ_count(v, O_MINE);
                    stations += mb_civ_count(v, O_STATION);
                }
                int dep_i = 0, dep_g = 0;
                for (int i = 0; i < NC; i++) {
                    uint8_t o = mb_w.obj[i];
                    if (o == O_ORE) dep_i++;
                    else if (o == O_GOLD || o == O_SILVER || o == O_GEM) dep_g++;
                }
                int temples = 0;
                for (int i = 0; i < NC; i++) if (mb_w.obj[i] == O_TEMPLE) temples++;
                /* THE FAITH CEILING IS THE REAL COST OF A POWER. The reserve is capped at
                 * 400 + 120 a temple, and four of the eight beasts cost MORE THAN 400 — so
                 * with no temple standing they cannot be cast at all, ever, at any income. */
                fprintf(stderr, "faith: %d held, +%d/yr, cap %d (%d temples)\n",
                        mb_faith(), mb_faith_income(), 400 + temples * 120, temples);
                fprintf(stderr, "metal: held %d iron / %d gold; MINED %d iron / %d gold "
                                "(%d mines, %d stations, %d ore seams, %d gold seams)\n",
                        iron, gold, mb_mined_iron, mb_mined_gold,
                        mines, stations, dep_i, dep_g);
                (void)haveiron; (void)havegold;
            }
            {   /* and which of the four dead branches are alive now */
                int cav = 0, nav = 0, rail = 0, med = 0;
                for (int k = 1; k < MAXK; k++) {
                    if (!mb_k[k].alive) continue;
                    cav  += mb_tech_known(k, TECH_CAVALRY)    ? 1 : 0;
                    nav  += mb_tech_known(k, TECH_NAVIGATION) ? 1 : 0;
                    rail += mb_tech_known(k, TECH_RAILWAY)    ? 1 : 0;
                    med  += mb_tech_known(k, TECH_MEDICINE)   ? 1 : 0;
                }
                fprintf(stderr, "crowns knowing: %d cavalry %d navigation %d railway "
                                "%d medicine\n", cav, nav, rail, med);
            }
            /* THE TOWNSCAPE, COUNTED. "Do the towns look varied" is not a question a
             * screenshot of one settlement answers — a plaza that only ever appears in the
             * capital, or gardens that never plant, look identical to working code from the
             * outside. These are the three cosmetic loops, in numbers. */
            {   int mon = 0, fnt = 0, garden = 0, paved = 0;
                for (int i = 0; i < MW * MH; i++) {
                    uint8_t o = mb_w.obj[i];
                    if (o == O_MONUMENT) mon++;
                    else if (o == O_FOUNTAIN) fnt++;
                    else if (mb_w.claim[i] && (o == O_TUFT || o == O_BUSH
                                               || o == O_FLOWER || o == O_TREE)) garden++;
                    if (mb_w.road[i]) paved++;
                }
                int farms = 0, fields = 0;
                for (int i = 0; i < NC; i++) {
                    if (mb_w.obj[i] == O_FARM) farms++;
                    if (mb_w.biome[i] == B_FARM) fields++;
                }
                fprintf(stderr, "townscape: %d monuments %d fountains, "
                                "%d planted cells, %d paved, %d farms, %d field cells\n",
                        mon, fnt, garden, paved, farms, fields);
                /* AND WHAT THE FIRE HAS BECOME. The founding campfire climbs a ladder — fire,
                 * well, gas lamp, street light — and every rung is one column on the sheet, so
                 * the tally of resolved columns is the whole answer. */
                {   int rung[4] = {0};
                    for (int y = 0; y < MH; y++)
                        for (int x = 0; x < MW; x++) {
                            if (mb_w.obj[AT(x, y)] != O_FIRE_PIT) continue;
                            int kk = mb_kingdom_of(mb_w.claim[AT(x, y)]);
                            int cc = mb_draw_form_col(O_FIRE_PIT, kk, x, y);
                            int base = O_N - O_BUILD0;
                            rung[cc == base + 9 ? 1 : cc == base + 10 ? 2
                                 : cc == base + 11 ? 3 : 0]++;
                        }
                    fprintf(stderr, "founding marks: %d fires %d wells %d gas lamps "
                                    "%d street lights\n", rung[0], rung[1], rung[2], rung[3]);
                }
            }
            /* SEAFARING, CONFIRMED OR NOT. A feature nobody can see is a feature nobody
             * can trust: this is the only place that says whether a kingdom which learned to
             * sail ever actually settled across water. */
            { extern int mb_seaborne_colonies;
              fprintf(stderr, "colonies founded by ship: %d\n", mb_seaborne_colonies); }
            {   /* WAR, COUNTED BY WEAPON. Battles are rare and brief, so "do armies shoot"
                 * and "does the weapon follow the tech" are not questions a screenshot of one
                 * moment can answer. */
                extern int mb_shots_fired[MB_SHOT_N];
                static const char *const SN[MB_SHOT_N] = { "rocks", "arrows", "bullets",
                                                           "shells", "missiles" };
                fprintf(stderr, "shots fired:");
                for (int i = 0; i < MB_SHOT_N; i++)
                    fprintf(stderr, " %s=%d", SN[i], mb_shots_fired[i]);
                fprintf(stderr, "\n");
            }
            {   /* THE END OF THE TREE, counted. "Does anybody ever get the bomb, build a
                 * silo and use it" is three separate questions and none of them can be
                 * answered from a screenshot. */
                int silos = 0, armed = 0, fired = 0;
                for (int v = 1; v < MAXV; v++)
                    if (mb_v[v].alive) silos += mb_civ_count(v, O_SILO);
                for (int k = 1; k < MAXK; k++) {
                    if (!mb_k[k].alive) continue;
                    if (mb_tech_known(k, TECH_NUKE)) armed++;
                    fired += mb_k[k].nuked;
                }
                fprintf(stderr, "the bomb: %d kingdoms have it, %d silos standing, "
                                "%d strikes launched\n", armed, silos, fired);
                /* WHY IT DID OR DID NOT FIRE. Three preconditions have to hold at once — a
                 * silo, a living rival, and either a hawkish lord or a losing war — and
                 * "0 strikes launched" says nothing about which one was missing. */
                for (int k = 1; k < MAXK; k++) {
                    if (!mb_k[k].alive || !mb_tech_known(k, TECH_NUKE)) continue;
                    int mysilo = 0, rivals = 0, hawk = 0, mytowns = 0;
                    for (int v = 1; v < MAXV; v++) {
                        if (!mb_v[v].alive || mb_v[v].kingdom != k) continue;
                        mytowns++; mysilo += mb_civ_count(v, O_SILO);
                        int h = mb_v[v].lord_war;
                        if (mb_v[v].lord_traits & TR_AMBITIOUS) h += 25;
                        if (mb_v[v].lord_traits & TR_VENGEFUL)  h += 30;
                        if (mb_v[v].lord_traits & TR_MADNESS)   h += 40;
                        if (h > hawk) hawk = h;
                    }
                    for (int e = 1; e < MAXK; e++) if (e != k && mb_k[e].alive) rivals++;
                    char kn[24]; mb_name_str(kn, sizeof kn, 1, mb_k[k].name);
                    fprintf(stderr, "   %s: %d silos, %d towns, %d rivals, hawk %d%s, "
                                    "wars %d, weariness %d\n", kn, mysilo, mytowns, rivals,
                            hawk, hawk >= 95 ? " FIRST-STRIKE" : "",
                            mb_k[k].war_with ? 1 : 0, mb_k[k].exhaustion);
                }
                {   /* THE CIVIC LADDER, counted. Nine buildings gated on nine techs is nine
                     * more chances for a feature to exist and never be built — which has
                     * happened three times in this game already. */
                    static const uint8_t CV[9] = { O_GRANARY, O_MARKET, O_LIBRARY, O_FOUNDRY,
                                                   O_COLLEGE, O_HOSPITAL, O_FACTORY,
                                                   O_STATION, O_POWER };
                    fprintf(stderr, "civic:");
                    for (int t = 0; t < 9; t++) {
                        int c = 0;
                        for (int v = 1; v < MAXV; v++)
                            if (mb_v[v].alive) c += mb_civ_count(v, CV[t]);
                        fprintf(stderr, " %s=%d", O_NAME[CV[t]], c);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        {   int prof[PROF_N] = {0}, wed = 0, hauling = 0, named = 0, immune = 0;
            for (int i = 0; i < mb_nu; i++) {
                const Unit *u = &mb_u[i];
                if (!u->alive || u->sp >= SP_CIV_N) continue;
                prof[u->prof < PROF_N ? u->prof : 0]++;
                if (u->mate) wed++;
                if (u->job == JOB_HAUL) hauling++;
                if (u->given) named++;
                if (u->traits & TR_IMMUNE) immune++;
            }
            fprintf(stderr, "people by trade: ");
            for (int i = 0; i < PROF_N; i++) if (prof[i]) fprintf(stderr, " %s=%d", MB_PROF_NAME[i], prof[i]);
            fprintf(stderr, "\nwed %d   on the road %d   named %d   plague-immune %d\n",
                    wed, hauling, named, immune);
        }
        {   int lo = 255, hi = 0, sum = 0, n = 0;
            for (int v = 1; v < MAXV; v++) {
                if (!mb_v[v].alive) continue;
                int a = mb_v[v].lord_age;
                if (a < lo) lo = a; if (a > hi) hi = a; sum += a; n++;
            }
            if (n) fprintf(stderr, "lords: %d of them, aged %d..%d (mean %d)\n", n, lo, hi, sum / n);
        }
    }
    /* MOTEBOX_DUMPBIOME=<path> writes the raw biome array, so the authoring side can
     * compare palettes on a REAL world instead of a synthetic one. */
    {
        const char *bp = getenv("MOTEBOX_DUMPBIOME");
        if (bp && *bp) {
            FILE *f = fopen(bp, "wb");
            /* biome then ELEVATION, because the shaded relief is derived from elev and
             * "why is that massif flat" is not answerable from the biome alone. */
            if (f) { fwrite(mb_w.biome, 1, NC, f); fwrite(mb_w.elev, 1, NC, f); fclose(f);
                     fprintf(stderr, "dumped %d biome + %d elev cells to %s\n", NC, NC, bp); }
        }
    }
    /* MOTEBOX_PWTEST=1 casts all forty-eight powers and reports what each did. */
    if (getenv("MOTEBOX_PWTEST")) mb_power_test_all(s_cx, s_cy);
    if (getenv("MOTEBOX_VSTAT")) {
        for (int i = 0; i < MAXV; i++) {
            if (!mb_v[i].alive) continue;
            int farms = 0, fields = 0, nwalls = 0;
            for (int y = mb_v[i].y - 8; y <= mb_v[i].y + 8; y++)
                for (int x = mb_v[i].x - 8; x <= mb_v[i].x + 8; x++)
                    if (mb_in(x, y) && mb_w.claim[AT(x, y)] == i) {
                        if (mb_w.obj[AT(x, y)] == O_FARM) farms++;
                        if (mb_w.biome[AT(x, y)] == B_FARM) fields++;
                    }
            fprintf(stderr, "v%-3d pop=%-4d house=%-4d food=%-4d farms=%-3d hall=%d "
                            "threat=%-3d walls=%-3d %s\n",
                    i, mb_v[i].pop, mb_v[i].housing, mb_v[i].food, farms,
                    mb_v[i].hall, mb_v[i].threat, nwalls,
                    B_NAME[mb_w.biome[AT(mb_v[i].x, mb_v[i].y)]]);
        }
    }
    /* MOTEBOX_CENSUS=1 lists what is actually standing in the visible window,
     * because "what does the screen show" and "what does the sim think" are two
     * different questions and only the first one is the complaint. */
    if (getenv("MOTEBOX_VSTAT")) {
        int homeless = 0, housed = 0, bysp[SP_N] = { 0 };
        for (int i = 0; i < mb_nu; i++) {
            if (!mb_u[i].alive || mb_u[i].sp >= SP_DEER) continue;
            bysp[mb_u[i].sp]++;
            int v = mb_u[i].village;
            if (v > 0 && v < MAXV && mb_v[v].alive) housed++; else homeless++;
        }
        fprintf(stderr, "civ units: %d in a village, %d HOMELESS\n", housed, homeless);
        for (int s = 0; s < SP_DEER; s++)
            if (bysp[s]) fprintf(stderr, "   %-10s %d\n", MB_SP[s].name, bysp[s]);
    }
    if (getenv("MOTEBOX_VSTAT")) {
        int world_walls = 0, plans = 0, qualify = 0;
        for (int c = 0; c < NC; c++) if (mb_w.obj[c] == O_WALL) world_walls++;
        for (int i = 1; i < MAXV; i++) {
            if (!mb_v[i].alive) continue;
            if (mb_v[i].plan_obj == O_WALL) plans++;
            if (mb_v[i].hall >= 2 && (mb_v[i].threat > 25 || mb_v[i].pop >= 14
                                      || mb_v[i].stone > 40)) qualify++;
        }
        fprintf(stderr, "walls in the world: %d   qualifying villages: %d   "
                        "wall plans pending: %d\n", world_walls, qualify, plans);
    }
    if (getenv("MOTEBOX_CENSUS")) {
        int seen[SP_N] = { 0 }, build[O_N] = { 0 };
        for (int i = 0; i < mb_nu; i++) {
            if (!mb_u[i].alive) continue;
            int ux = mb_u[i].x >> 4, uy = mb_u[i].y >> 4;
            if (ux < s_cx - 8 || ux > s_cx + 8 || uy < s_cy - 7 || uy > s_cy + 7) continue;
            seen[mb_u[i].sp]++;
        }
        for (int y = s_cy - 7; y <= s_cy + 7; y++)
            for (int x = s_cx - 8; x <= s_cx + 8; x++)
                if (mb_in(x, y) && mb_w.obj[AT(x, y)] < O_N) build[mb_w.obj[AT(x, y)]]++;
        fprintf(stderr, "census at %d,%d (16x14 window):\n", s_cx, s_cy);
        for (int s = 0; s < SP_N; s++)
            if (seen[s]) fprintf(stderr, "   %-10s %d\n", MB_SP[s].name, seen[s]);
        for (int o = 1; o < O_N; o++)
            if (build[o]) fprintf(stderr, "   obj %-12s %d\n", O_NAME[o], build[o]);
        /* AND THE BIOMES. Objects alone are half the window: a screen full of grey
         * squares was chased through the building tables twice before a biome
         * histogram said what they actually were. */
        int bio[B_N] = { 0 };
        for (int y = s_cy - 7; y <= s_cy + 7; y++)
            for (int x = s_cx - 8; x <= s_cx + 8; x++)
                if (mb_in(x, y)) bio[mb_w.biome[AT(x, y)]]++;
        for (int b = 0; b < B_N; b++)
            if (bio[b]) fprintf(stderr, "   bio %-12s %d\n", B_NAME[b], bio[b]);
        int roads = 0;
        for (int c = 0; c < NC; c++) if (mb_w.road[c]) roads++;
        fprintf(stderr, "   road cells in the world: %d\n", roads);
    {   /* THE ROAD GRADE, counted. Roads pick their surface from the owning kingdom's tech at
         * draw time, and "why is a year-150 town paved in tarmac" is not answerable from a
         * screenshot — the five surfaces differ by a few RGB565 steps once quantised. */
        int hist[5] = { 0, 0, 0, 0, 0 };
        for (int i = 0; i < NC; i++) {
            if (!mb_w.road[i]) continue;
            int ov = mb_w.claim[i];
            int ok = (ov && ov < MAXV && mb_v[ov].alive) ? mb_v[ov].kingdom : 0;
            int g = 0;
            if (mb_tech_known(ok, TECH_FLIGHT))          g = 4;
            else if (mb_tech_known(ok, TECH_COMBUSTION)) g = 3;
            else if (mb_tech_known(ok, TECH_ENGINEER))   g = 2;
            else if (mb_tech_known(ok, TECH_MASONRY))    g = 1;
            hist[g]++;
        }
        fprintf(stderr, "   road grades: track=%d cobble=%d flag=%d tarmac=%d marked=%d\n",
                hist[0], hist[1], hist[2], hist[3], hist[4]);
    }
        int want, lost;
        mb_draw_sprite_load(&want, &lost);
        fprintf(stderr, "   sprites wanted %d, DROPPED %d\n", want, lost);
        int dead_rich = 0;
        for (int i = 0; i < MAXV; i++)
            if (mb_v[i].alive && mb_v[i].pop == 0) dead_rich++;
        fprintf(stderr, "   villages alive with nobody in them: %d\n", dead_rich);
    }
    if (s_stat) mb_world_stats();
#endif
    /* THE FIRST FRAME NEEDS A BAND MAP. Without this the terrain is a navy void until the
     * first world tick — which at 400 fps is a tenth of a second, long enough to be the
     * first thing a screenshot catches and long enough to look like a bug. */
    mb_bands_rebuild();
    view_set(1);
}

/* The overlay has no dt of its own — the engine hands it only a framebuffer — so the update
 * leaves the last one here for the effects that animate in world time. */
static float s_dt;

static void g_update(float dt)
{
    s_dt = dt;
    const MoteInput *in = mote->input();

    /* --- clock: every tick is one pass of the world's rules --- */
    int tps = SPEED_TPS[s_speed];
    if (tps) {
        s_tick_acc += dt * (float)tps;
        int steps = (int)s_tick_acc;
        if (steps > 0) {
            if (steps > 8) steps = 8;          /* never let the sim eat the frame */
            s_tick_acc -= (float)steps;
            for (int i = 0; i < steps; i++) {
                mb_w.tick++;
                /* ORDER MATTERS. The field pass runs first so a unit stepping onto
                 * a burning cell this tick is hurt by it rather than a tick late;
                 * the civ pass runs after the units so its census sees the day's
                 * deaths; Faith and the age read the finished state. */
                mb_flux_step();
                mb_flux_natural();
                mb_unit_step();
                mb_unit_plague_step();
                mb_unit_madness_step();
                mb_unit_migrate();
                mb_civ_step();
                mb_grow_step();
                mb_grave_step();
                mb_bands_rebuild();
                mb_faith_step();
                mb_age_step();
#if MOTE_HOST
                if (s_trace)
                    fprintf(stderr, "tick %d flux=%d pop=%d civ=%d v=%d k=%d faith=%d %s"
                                    " wild=%d died a%d w%d e%d s%d d%d p%d f%d"
                                    " V1[pop%d h%d hs%d f%d w%d s%d]\n",
                            (int)mb_w.tick, mb_flux_count(), mb_pop_all(), mb_pop_civ(),
                            mb_village_count(), mb_kingdom_count(), mb_faith(), mb_age_name(),
                            mb_pop_wild(),
                            mb_deaths_by(CAUSE_AGE), mb_deaths_by(CAUSE_WOUNDS),
                            mb_deaths_by(CAUSE_EATEN), mb_deaths_by(CAUSE_SLAIN),
                            mb_deaths_by(CAUSE_DISASTER), mb_deaths_by(CAUSE_PLAGUE),
                            mb_deaths_by(CAUSE_STARVED),
                            mb_v[1].pop, mb_v[1].hall, mb_v[1].housing,
                            mb_v[1].food, mb_v[1].wood, mb_v[1].stone);
#endif
            }
        }
    }
    /* THE MOVEMENT CLOCK, advanced AFTER the ticks have run.
     *
     * mb_draw_set_lerp() resets itself when it sees a new mb_w.tick, so it has to be called
     * once the tick has actually advanced. Calling it earlier in the frame — which is where it
     * naturally wants to go — meant it always observed the OLD tick, so the reset landed a
     * frame late and the first frame of every tick drew with the previous tick's maxed-out
     * alpha. A walker's drawn x went 961 -> 970 -> 965: the once-per-cell flicker. Three
     * attempts at fixing the arithmetic failed because the arithmetic was never wrong; printing
     * the setter's own calls interleaved with the draws is what showed the ordering.
     *
     * This game draws BEFORE it updates, so the frame after this one is the one that uses it. */
    if (tps) mb_draw_set_lerp(dt * (float)tps);

    mb_fx_step(dt);
    mb_chron_step(dt);
    mb_draw_prepare();
    mb_audio_step(dt);
    if (s_denied > 0.0f) s_denied -= dt;
    if (s_nobody > 0.0f) s_nobody -= dt;

#if MOTE_HOST
    /* the scripted cast, once, after the world has settled into a frame */
    if (s_cast_script[0] && ++s_frame == 12) {
        char *p = s_cast_script;
        while (*p) {
            char name[24]; int x = 0, y = 0, n = 0;
            while (*p && *p != '@' && n < 23) name[n++] = *p++;
            name[n] = 0;
            if (*p == '@') p++;
            while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
            if (*p == ',') p++;
            while (*p >= '0' && *p <= '9') y = y * 10 + (*p++ - '0');
            if (!mb_power_cast_named(name, x, y))
                fprintf(stderr, "MOTEBOX_CAST: no power named '%s'\n", name);
            else
                fprintf(stderr, "cast %s at %d,%d flux=%d (%s)\n", name, x, y, mb_flux_count(),
                        B_NAME[mb_w.biome[AT(x, y)] < B_N ? mb_w.biome[AT(x, y)] : 0]);
            if (*p == ';') p++; else break;
        }
    }
#endif

    /* --- the wheel gets first refusal on the d-pad: while LB is held the
     * direction is choosing a power, not moving the cursor --- */
    int wheel = mb_power_input(in);

    /* --- cursor: accelerates while held, so crossing the world is quick but a
     * single tap is still one tile --- */
    int dx = wheel ? 0 : mote_pressed(in, MOTE_BTN_RIGHT) - mote_pressed(in, MOTE_BTN_LEFT);
    int dy = wheel ? 0 : mote_pressed(in, MOTE_BTN_DOWN)  - mote_pressed(in, MOTE_BTN_UP);
    if (dx || dy) {
        int first = (s_hold == 0.0f);
        s_hold += dt;
        float rate = s_hold < 0.35f ? 0.0f : (s_hold < 0.9f ? 12.0f : 34.0f);
        s_move_acc += rate * dt;
        int steps = first ? 1 : (int)s_move_acc;
        if (steps > 0) {
            s_move_acc -= (float)steps;
            s_cx += dx * steps; s_cy += dy * steps;
            if (s_cx < 0) s_cx = 0; if (s_cx >= MW) s_cx = MW - 1;
            if (s_cy < 0) s_cy = 0; if (s_cy >= MH) s_cy = MH - 1;
        }
    } else {
        s_hold = 0.0f; s_move_acc = 0.0f;
    }

    /* --- THE INFORMATION SCREENS OWN THE PAD WHILE ONE IS OPEN ---
     * Returning early is what makes them screens rather than menus: the world keeps ticking
     * behind them (a soul's hunger really is rising while you read about it) but the cursor
     * does not wander and A does not cast a power into the map you cannot see. */
    if (s_ui) {
        int up   = mote_just_pressed(in, MOTE_BTN_UP);
        int down = mote_just_pressed(in, MOTE_BTN_DOWN);
        if (s_ui == UI_PICK) {
            pick_build();                        /* the crowd moves; the list must follow */
            if (up   && s_ui_row > 0) s_ui_row--;
            if (down && s_ui_row + 1 < s_pick_n) s_ui_row++;
            if (mote_just_pressed(in, MOTE_BTN_A) && s_pick_n) {
                int k = s_pick[s_ui_row].kind;
                if (k == PK_UNIT)  { s_subject = s_pick[s_ui_row].idx; s_ui = UI_SOUL; }
                else if (k == PK_LORD) { s_subject_v = s_pick[s_ui_row].idx; s_ui = UI_LORD; }
                else               { s_ui = UI_LAND; }
                s_ui_row = 0;
            } else if (mote_just_pressed(in, MOTE_BTN_A)) {
                s_ui = UI_LAND;
            }
            if (mote_just_pressed(in, MOTE_BTN_B)) s_ui = UI_NONE;
        } else if (s_ui == UI_SHAPE) {
            if (up   && s_ui_row > 0) s_ui_row--;
            if (down && s_ui_row + 1 < N_GIFTS) s_ui_row++;
            if (mote_just_pressed(in, MOTE_BTN_A)) {
                const Gift *g = &GIFTS[s_ui_row];
                /* A GIFT THAT WOULD DO NOTHING COSTS NOTHING. Charging for MEND on somebody
                 * already whole is the sort of thing that teaches a player to distrust a
                 * screen — gift_apply reports whether it changed anything and the Faith only
                 * leaves if it did. */
                if (s_subject >= 0 && s_subject < mb_nu && mb_u[s_subject].alive
                    && mb_faith_afford(g->cost) && gift_apply(g, s_subject)) {
                    mb_faith_spend(g->cost);
                    s_ui_flash = 0.5f;
                    mb_snd(SND_BLESS);
                } else {
                    mb_snd(SND_DENY);
                }
            }
            if (mote_just_pressed(in, MOTE_BTN_B)) { s_ui = UI_SOUL; s_ui_row = 0; }
        } else if (s_ui == UI_LORD) {
            if (up   && s_ui_row > 0) s_ui_row--;
            if (down && s_ui_row < 2) s_ui_row++;
            if (mote_just_pressed(in, MOTE_BTN_A)) {
                Village *V = &mb_v[s_subject_v];
                uint8_t *stat = (s_ui_row == 0) ? &V->lord_stew
                              : (s_ui_row == 1) ? &V->lord_diplo : &V->lord_war;
                if (V->alive && *stat < 100 && mb_faith_afford(MB_LORD_RAISE)) {
                    int nv = *stat + 20; *stat = (uint8_t)(nv > 100 ? 100 : nv);
                    mb_faith_spend(MB_LORD_RAISE);
                    s_ui_flash = 0.5f;
                    mb_snd(SND_BLESS);
                } else {
                    mb_snd(SND_DENY);
                }
            }
            if (mote_just_pressed(in, MOTE_BTN_B)) { s_ui = UI_PICK; s_ui_row = 0; }
        } else {
            if (mote_just_pressed(in, MOTE_BTN_B)) { s_ui = UI_PICK; s_ui_row = 0; }
            if (mote_just_pressed(in, MOTE_BTN_A) && s_ui == UI_SOUL) {
                s_ui = UI_SHAPE; s_ui_row = 0;
            }
            /* RB PAGES OUT. From a person to their town, to the crown above it, to the world:
             * the whole hierarchy on one button, in the order you would ask the questions. */
            if (mote_just_pressed(in, MOTE_BTN_RB)) {
                switch (s_ui) {
                case UI_SOUL:
                    if (s_subject >= 0 && s_subject < mb_nu) s_subject_v = mb_u[s_subject].village;
                    s_ui = UI_TOWN; break;
                case UI_LORD: s_ui = UI_TOWN;  break;
                case UI_TOWN: s_ui = UI_KING;  break;
                case UI_KING: s_ui = UI_WORLD; break;
                case UI_WORLD: s_ui = UI_LAND; break;
                default:      s_ui = UI_TOWN;  break;
                }
                s_ui_row = 0;
            }
        }
        if (s_ui_flash > 0.0f) s_ui_flash -= dt;
        return;
    }

    /* MENU opens the God Menu: the laws, the chronicle, the legends, the save.
     * A blocking engine menu is the right tool — it is a pause, and the sim
     * genuinely should stop while you read a history. */
    if (mote_just_pressed(in, MOTE_BTN_MENU) && !wheel) god_menu();
    /* B: WHO IS HERE — every figure and thing overlapping the cell, so you can choose which
     * of a crowd of twenty to look at. It used to open a text card about whatever the cursor
     * happened to resolve to, which in a town is a lottery. */
    if (mote_just_pressed(in, MOTE_BTN_B) && !wheel) {
        pick_build(); s_ui_row = 0; s_ui = UI_PICK;
    }

    /* --- A casts. A brush power keeps casting while held, on a fixed cadence so
     * painting terrain feels like a brush rather than a machine gun; everything
     * else fires once per press, because a meteor should cost a decision. --- */
    if (!wheel) {
        int fire = mb_power_brush()
                 ? (mote_pressed(in, MOTE_BTN_A) && (s_cast_cool -= dt) <= 0.0f)
                 : mote_just_pressed(in, MOTE_BTN_A);
        if (fire) {
            int cost = mb_power_cost();
            if (mb_faith_afford(cost)) {
                mb_faith_spend(cost);
                mb_power_cast(s_cx, s_cy);
                /* A CAST THAT REACHED NOBODY SAYS SO. Spending eighty faith on a
                 * blessing and watching the world not move is indistinguishable from a
                 * broken button — and the powers that act on PEOPLE are exactly the
                 * ones whose target you cannot see, because a villager is one pixel in
                 * God's Eye. -1 means the power does not target people at all. */
                if (mb_power_last_reach() == 0) { s_nobody = 0.9f; mb_snd(SND_DENY); }
                s_cast_cool = 0.10f;
            } else {
                /* no Faith: say so once rather than silently doing nothing, which
                 * reads as a broken button */
                s_denied = 0.7f;
                mb_snd(SND_DENY);
                s_cast_cool = 0.35f;
            }
        }
        if (!mote_pressed(in, MOTE_BTN_A)) s_cast_cool = 0.0f;
    }

    /* FOLLOW HISTORY: the camera jumps to the last headline, which is what makes
     * a paused-thumb session tell you stories instead of needing to be hunted. */
    {
        /* Any input at all counts as "hands on", including a held direction — the point
         * is not which button, it is that somebody is using the thing. */
        int active = mote_pressed(in, MOTE_BTN_UP)   || mote_pressed(in, MOTE_BTN_DOWN)
                  || mote_pressed(in, MOTE_BTN_LEFT) || mote_pressed(in, MOTE_BTN_RIGHT)
                  || mote_pressed(in, MOTE_BTN_A)    || mote_pressed(in, MOTE_BTN_B)
                  || mote_pressed(in, MOTE_BTN_LB)   || mote_pressed(in, MOTE_BTN_RB)
                  || mote_pressed(in, MOTE_BTN_MENU) || wheel;
        if (active) { s_idle = 0.0f; s_pan_x = -1; }     /* your move wins, always */
        else        { s_idle += dt; }

        /* Don't even ASK for a focus while the player is busy: leaving it pending means
         * that when they do put it down, history takes them to the LATEST thing that
         * happened rather than to whatever fired at the moment they stopped. */
        if (mb_law(LAW_FOLLOW) && !wheel && s_idle > FOLLOW_IDLE) {
            int fx2, fy2;
            if (mb_chron_focus(&fx2, &fy2)) { s_pan_x = fx2; s_pan_y = fy2; }
        }

        if (s_pan_x >= 0) {
            int dxp = s_pan_x - s_cx, dyp = s_pan_y - s_cy;
            int far = (dxp < 0 ? -dxp : dxp) + (dyp < 0 ? -dyp : dyp);
            int step = 1 + far / 24;                     /* a glide, not a jump */
            if (step > 3) step = 3;
            for (int n = 0; n < step; n++) {
                if (s_cx != s_pan_x) s_cx += (s_pan_x > s_cx) ? 1 : -1;
                if (s_cy != s_pan_y) s_cy += (s_pan_y > s_cy) ? 1 : -1;
            }
            if (s_cx == s_pan_x && s_cy == s_pan_y) s_pan_x = -1;
            if (!s_god) cam_follow();
        }
    }

    /* --- LB TAP cycles speed (LB HOLD is the wheel, handled above); RB toggles
     * the zoom, unless the wheel is up, where RB pages tabs --- */
    if (!wheel && mote_just_released(in, MOTE_BTN_LB) && !s_lb_was_wheel)
        s_speed = (s_speed + 1) % 5;
    s_lb_was_wheel = wheel;
    /* RB: TAP TOGGLES THE VIEW, HOLD SEEKS. One shoulder, two verbs, no chord — the
     * same tap/hold split LB already uses for speed and the wheel. The threshold has
     * to be short enough that a zoom still feels instant on release. */
    if (!wheel) {
        if (mote_pressed(in, MOTE_BTN_RB)) {
            s_rb_hold += dt;
            if (s_rb_hold > 0.30f) {
                s_seek_acc += dt;
                if (!s_rb_seeked || s_seek_acc > 0.33f) {   /* ~3 a second while held */
                    seek_next(); s_seek_acc = 0.0f; s_rb_seeked = 1;
                }
            }
        } else {
            if (s_rb_hold > 0.0f && !s_rb_seeked) view_set(!s_god);
            s_rb_hold = 0.0f; s_seek_acc = 0.0f; s_rb_seeked = 0;
        }
    }

    if (!s_god) {
        cam_follow();
        /* shake offsets the CAMERA, which only exists in Mortal View; God's Eye
         * spends the same impact on a frame flash instead (mb_fx) */
        float sh = mb_fx_shake_amt();
        if (sh > 0.0f) {
            int amp = (int)(sh + 0.5f);
            if (amp > 4) amp = 4;
            s_shake_ph = (s_shake_ph * 1103515245u + 12345u);
            s_cam_x += (int)((s_shake_ph >> 16) % (uint32_t)(2 * amp + 1)) - amp;
            s_cam_y += (int)((s_shake_ph >> 24) % (uint32_t)(2 * amp + 1)) - amp;
        }
        mb_draw_mortal(s_cam_x, s_cam_y);
        /* disasters breathe particles from the cells in view, then get drawn as
         * pixels in the overlay — no tile sprite anywhere in the chain */
        mb_fx_flux_emit(s_cam_x, s_cam_y, dt);
    } else {
        mote->scene2d_begin(0, 0);               /* nothing but the background */
    }
}

static void g_overlay(uint16_t *fb)
{
    char buf[40];
    int year = (int)(mb_w.tick / TPY);

    /* AN INFORMATION SCREEN COVERS THE WORLD, and is drawn here rather than through
     * mote->menu() so it can hold sprites, animate, and be captured. */
    if (s_ui) {
        switch (s_ui) {
        case UI_PICK:  ui_draw_pick(fb);  break;
        case UI_SOUL:  ui_draw_soul(fb);  break;
        case UI_SHAPE: ui_draw_shape(fb); break;
        case UI_LORD:  ui_draw_lord(fb);  break;
        case UI_TOWN:  ui_draw_town(fb);  break;
        case UI_KING:  ui_draw_king(fb);  break;
        case UI_WORLD: ui_draw_world(fb); break;
        case UI_LAND:  ui_draw_land(fb);  break;
        default:       ui_draw_pick(fb);  break;
        }
        return;
    }

#if MOTE_HOST
    /* MOTEBOX_PERF=1 times the world rasteriser, so a change to it (or to the sim
     * feeding it) is MEASURED rather than eyeballed. The engine's perf() counters
     * are device-side, so on the host we time the pass ourselves — one extra
     * full-frame call of the SAME function the background pass just ran, on core
     * 0, which repaints identical pixels and so leaves the frame untouched. Only
     * in God's Eye: in Mortal View it would paint over the tile view.
     *
     * The absolute figure is HOST speed, not device speed (the device number
     * needs `mote push` and real hardware) — but it is the right regression
     * tripwire for the one pass that grows with every later phase. */
    if (s_perf) {
        static uint64_t acc; static int n;
        if (s_god) {
            uint64_t t0 = mote->micros();
            mb_god_band(fb, 0, VIEW_H);
            acc += mote->micros() - t0;
        }
        if ((++n & 63) == 0) {
            uint32_t p[6]; mote->perf(p);
            fprintf(stderr, "perf %s fps=%u raster=%uus update=%uus god_band=%.1fus "
                            "flux=%d agents=%d Y%d\n",
                    s_god ? "GOD " : "LAND", p[0], p[2], p[1], (double)acc / 64.0,
                    mb_flux_count(), mb_agent_count(), year);
            /* AND THE SPRITE LOAD, because a dropped sprite is silent and the last
             * time it went unnoticed the whole town went missing. */
            int want, lost;
            mb_draw_sprite_load(&want, &lost);
            fprintf(stderr, "     sprites wanted %d, dropped %d%s"
                            "   cursor %d,%d idle %.1fs%s\n", want, lost,
                    lost ? "  <-- SCENE FULL" : "", s_cx, s_cy, (double)s_idle,
                    s_pan_x >= 0 ? "  PANNING" : "");
            acc = 0;
        }
    }
#endif

    /* Units and particles in God's Eye are drawn HERE rather than in the band
     * pass, because both are lists the pass would have to filter per band. One
     * pixel each, after the terrain, so they sit on top of it. */
    if (s_god) { mb_god_towns(fb, 0, VIEW_H); mb_god_units(fb, 0, VIEW_H); mb_fx_draw_god(fb); }

    /* THE WORLD IS FINISHED BEFORE THE UI STARTS. This pass used to run at the very
     * end of the overlay, which was harmless while the engine drew the world's sprites —
     * but they are deferred now so the hillshade cannot shade them, and deferring them
     * past the cursor drew houses and burn scars ON TOP OF THE CROSSHAIR. The reticule is
     * a tool, not scenery: nothing in the world may cover it. */
    if (!s_god) {
        /* THE GROUND, then the relief that shades it, THEN everything standing on it.
         * The sprites are held back by mb_draw_mortal() precisely so the hillshade cannot
         * reach them — a lit deer and a snow-capped house are what happens otherwise. */
#if MOTE_HOST
        if (!getenv("MOTEBOX_NORELIEF"))
#endif
            mb_draw_relief(fb, s_cam_x, s_cam_y);
        /* worked land, over the band that drew the bare ground and under everything that
         * stands on it — see mb_draw_fields() */
        mb_draw_fields(fb, s_cam_x, s_cam_y);
        mb_draw_mortal_sprites(fb);
        mb_fx_flux_render(fb, s_cam_x, s_cam_y);
        mb_fx_draw_mortal_px(fb, s_cam_x, s_cam_y);
        /* THE CLOUD LAST, over everything. It is the one effect that is meant to dominate
         * the frame — a strike should be unmistakable from across the map, and a mushroom
         * behind a row of houses is not. */
#if MOTE_HOST
        if (getenv("MOTEBOX_SHOTDBG")) {
            static int fno;
            extern int mb_shots_fired[MB_SHOT_N];
            int tot = 0;
            for (int q = 0; q < MB_SHOT_N; q++) tot += mb_shots_fired[q];
            int must = 0, fighting = 0;
            for (int q = 1; q < MAXV; q++) if (mb_v[q].alive && mb_v[q].mustering) must++;
            for (int q = 0; q < mb_nu; q++)
                if (mb_u[q].alive && mb_u[q].job == JOB_FIGHT) fighting++;
            if (fno < 200 && (fno % 20) == 0)
                fprintf(stderr, "frame %3d live %d fired %d armies-allowed %d "
                                "mustering %d fighting %d\n",
                        fno, mb_fx_shots_live(), tot, mb_age_allows_armies(), must, fighting);
            fno++;
        }
#endif
        mb_fx_draw_shots(fb, s_cam_x, s_cam_y, s_dt);
        mb_fx_draw_nuke(fb, s_cam_x, s_cam_y, s_dt);
    }

    /* --- cursor ---
     * Drawn as a box AROUND the target so the tile itself still shows, and in
     * TWO TONES — dark outside, light inside. A single light box disappeared
     * against beach and snow, which is most of a polar coastline; two tones read
     * on every one of the 23 biome colours without a blink to wait for. */
    if (s_god) {
        mote->draw_rect(fb, s_cx - 3, s_cy - 3, 7, 7, C_DARK, 0, 0, VIEW_H);
        mote->draw_rect(fb, s_cx - 2, s_cy - 2, 5, 5, C_CURS, 0, 0, VIEW_H);
        mote->draw_pixel(fb, s_cx, s_cy, C_HI);
    } else {
        int px = s_cx * TILE - s_cam_x, py = s_cy * TILE - s_cam_y;
        mote->draw_rect(fb, px - 2, py - 2, TILE + 4, TILE + 4, C_DARK, 0, 0, VIEW_H);
        mote->draw_rect(fb, px - 1, py - 1, TILE + 2, TILE + 2, C_CURS, 0, 0, VIEW_H);
    }
    /* the brush footprint, so an area power shows what it will take */
    int br = mb_power_radius();
    if (br > 0 && !mb_wheel_open()) {
        if (s_god) mote->draw_circle(fb, s_cx, s_cy, br, C_CURS, 0, 0, VIEW_H);
        else mote->draw_circle(fb, s_cx * TILE - s_cam_x + TILE / 2,
                               s_cy * TILE - s_cam_y + TILE / 2,
                               br * TILE, C_CURS, 0, 0, VIEW_H);
    }

    /* --- HUD ---------------------------------------------------------- */
    mote->draw_rect(fb, 0, HUD_Y, 128, 128 - HUD_Y, C_HUDBG, 1, 0, 128);

    const char *toast = mb_chron_toast();
    /* and while the world is driving itself, the last headline STAYS on the strip — see
     * mb_chron_headline(). The cursor readout is only worth the row when there is a hand
     * on the cursor. */
    if (!toast && s_idle > FOLLOW_IDLE && mb_law(LAW_FOLLOW)) toast = mb_chron_headline();
    if (toast) {
        /* A HEADLINE TAKES THE WHOLE STRIP for a few seconds — interrupting the
         * readout is the point, the story is more urgent than the coordinates —
         * and it gets its own row so nothing sits on top of it. */
        /* TWO LINES OF LOG, not one line of log and one of "AGE OF IRON". The age changes
         * about once a century, it is on the world screen, and it was eating most of a
         * fourteen-pixel strip that is the game's only running commentary. The older entry
         * goes above the newer one in the dimmer colour, so the pair reads as a feed. */
        const char *prev = mb_chron_recent(1);
        if (prev) hud_text(fb, prev, 1, HUD_Y, 126, C_TEXT, -1);
        else {
            snprintf(buf, sizeof buf, "Y%d %s", year, mb_age_name());
            hud_text(fb, buf, 1, HUD_Y, 126, C_TEXT, -1);
        }
        hud_text(fb, toast, 1, HUD_Y + 8, 126, C_CURS, -1);
    } else {
        /* row one: speed, year, the selected power, the view */
        hud_text(fb, SPEED_NAME[s_speed], HC_SPEED_X, HUD_Y, HC_SPEED_W, C_HI, -1);
        snprintf(buf, sizeof buf, "Y%d", year);
        hud_text(fb, buf, HC_YEAR_X, HUD_Y, HC_YEAR_W, C_HI, -1);
        hud_text(fb, mb_power_name(), HC_POWER_X, HUD_Y, HC_POWER_W, C_HI, -1);
        /* A RECOLOURED WORLD MUST SAY WHY. In a non-default map mode the view field shows
         * the LENS instead of EYE/GND: a map washed in creed colours with no label on it
         * is just a map that looks wrong. */
        hud_text(fb, (s_god && mb_draw_mapmode() != MAPMODE_POWER)
                        ? MB_MAPMODE_NAME[mb_draw_mapmode()]
                        : (s_god ? "EYE" : "GND"),
                 HC_VIEW_X, HUD_Y, HC_VIEW_W, C_TEXT, 1);

        /* row two: what is under the cursor, and what casting costs. A burning cell
         * says so, because that is the more urgent fact about it. */
        uint8_t b = mb_w.biome[AT(s_cx, s_cy)];
        uint8_t o = mb_w.obj[AT(s_cx, s_cy)];
        uint8_t k = mb_fkind(mb_w.flux[AT(s_cx, s_cy)]);
        int v = mb_w.claim[AT(s_cx, s_cy)];
        if (k && k < FX_N)
            snprintf(buf, sizeof buf, "%s %s", B_NAME[b < B_N ? b : 0], FX_NAME[k]);
        else if (v && mb_v[v].alive) {
            /* On someone's land, name the place AND say what it is. "Ravenburn 24" told
             * you a size; "Ravenburn, TOWN of MINER" tells you what four of the
             * simulation's loops have made of it, in the same nineteen characters, and it
             * is on screen the whole time rather than behind a menu. */
            char pl[24];
            mb_name_str(pl, sizeof pl, 0, mb_v[v].name);
            snprintf(buf, sizeof buf, "%s %s %d", pl,
                     MB_TIER_NAME[mb_v[v].tier < TIER_N ? mb_v[v].tier : 0], mb_v[v].pop);
        } else if (o && o < O_N && O_NAME[o][0])
            snprintf(buf, sizeof buf, "%s %s", B_NAME[b < B_N ? b : 0], O_NAME[o]);
        else {
            /* NO GRID REFERENCE. The fallback used to print "GRASS 68,15", and a coordinate is
             * the one thing on this HUD a player can do nothing with — there is no way to type
             * one in and nothing else displays them. Elevation at least says something about
             * the ground you are looking at, which is what the relief is drawn from. */
            snprintf(buf, sizeof buf, "%s  elev %d", B_NAME[b < B_N ? b : 0],
                     mb_w.elev[AT(s_cx, s_cy)]);
        }
        hud_text(fb, buf, HC_INFO_X, HUD_Y + 8, HC_INFO_W, k ? C_WARN : C_TEXT, -1);

        if (s_denied > 0.0f) {
            hud_text(fb, "NO FAITH", HC_FAITH_X, HUD_Y + 8, HC_FAITH_W, C_WARN, 1);
        } else if (s_nobody > 0.0f) {
            hud_text(fb, "NOBODY", HC_FAITH_X, HUD_Y + 8, HC_FAITH_W, C_WARN, 1);
        } else if (mb_mode() == MODE_PANTHEON) {
            snprintf(buf, sizeof buf, "%d", mb_faith());
            hud_text(fb, buf, HC_FAITH_X, HUD_Y + 8, HC_FAITH_W,
                     mb_faith_afford(mb_power_cost()) ? C_HI : C_WARN, 1);
        }
    }

    mb_power_draw_wheel(fb, &rogue8);

}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = {
        /* Pure 2D: no triangles, no depth buffer. The sprite pool covers the
         * visible object grid (18x16 worst case) plus the units, FX particles
         * and emotes the later phases add. */
        .max_sprites = 320,
    },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
