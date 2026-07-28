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
#include <stdio.h>
#include <string.h>

#include "rogue8.font.h"

/* --- time ---------------------------------------------------------------
 * One tick is a WEEK, 52 to the year: at x1 a year takes 6.5 s and a 250-year
 * epic runs in about three and a half minutes at x8, which is the pace a
 * handheld session wants. */
#define TPY 52
static const uint8_t SPEED_TPS[4] = { 0, 8, 24, 64 };   /* ticks per second */
static const char *const SPEED_NAME[4] = { "||", "x1", "x3", "x8" };
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
    "plan",
};
/* A compile-time tripwire for exactly the mistake above. */
typedef char mb_onames_complete[(sizeof O_NAME / sizeof O_NAME[0]) == O_N ? 1 : -1];


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

static void god_menu(void);

static void view_set(int god)
{
    s_god = god;
    /* The whole view switch: the world rasteriser IS the background pass, so
     * handing it over (or handing back NULL) is all that changes. */
    mote->set_background_cb(god ? mb_god_band : 0);
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

static void chronicle_menu(void)
{
    /* The engine menu is a list, and a history IS a list — so the chronicle needs
     * no screen of its own, and scrolls with the same buttons as everything else. */
    int n = mb_chron_count();
    if (n <= 0) {
        const char *none[] = { "nothing has happened yet", "BACK" };
        mote->menu("CHRONICLE", none, 2);
        return;
    }
    if (n > 24) n = 24;
    static char rows[25][34];
    const char *items[25];
    for (int i = 0; i < n; i++) {
        int year = 0; char line[30];
        mb_chron_line(line, sizeof line, i, &year);
        snprintf(rows[i], sizeof rows[i], "Y%d %s", year, line);
        items[i] = rows[i];
    }
    items[n] = "BACK";
    mote->menu("CHRONICLE", items, n + 1);
}

static void god_menu(void)
{
    for (;;) {
        static char st[4][30];
        snprintf(st[0], sizeof st[0], "%s", mb_age_name());
        snprintf(st[1], sizeof st[1], "FAITH %d  (+%d/yr)", mb_faith(), mb_faith_income());
        snprintf(st[2], sizeof st[2], "MODE: %s", mb_mode() == MODE_SANDBOX ? "SANDBOX" : "PANTHEON");
        snprintf(st[3], sizeof st[3], "pop %d  villages %d  kingdoms %d",
                 mb_pop_civ(), mb_village_count(), mb_kingdom_count());
        const char *items[] = { st[0], st[1], st[2], st[3],
                                "CHRONICLE", "WORLD LAWS", "SAVE WORLD", "LOAD WORLD",
                                "NEW WORLD", "CLOSE" };
        int c = mote->menu("MOTEBOX", items, 10);
        switch (c) {
        case 2: mb_mode_set(mb_mode() == MODE_SANDBOX ? MODE_PANTHEON : MODE_SANDBOX); break;
        case 4: chronicle_menu(); break;
        case 5: law_menu(); break;
        case 6: {
            int ok = mb_save_write(0, s_cx, s_cy, s_god);
            const char *m[] = { ok ? "saved" : "SAVE FAILED", "BACK" };
            mote->menu("SAVE", m, 2);
            break;
        }
        case 7: {
            int cx = s_cx, cy = s_cy, god = s_god;
            int ok = mb_save_read(0, &cx, &cy, &god);
            if (ok) { s_cx = cx; s_cy = cy; view_set(god); }
            const char *m[] = { ok ? "loaded" : "NO SAVE", "BACK" };
            mote->menu("LOAD", m, 2);
            break;
        }
        case 8: {
            uint32_t ns = (uint32_t)mote->micros() ^ (mb_w.seed * 2654435761u);
            mb_world_gen(ns);
            /* RESET, not init: the arena is bump-only, so re-initialising would
             * leak a whole world's worth of buffers per reroll. */
            mb_unit_reset(); mb_civ_reset(); mb_flux_reset();
            mb_chron_init(); mb_age_init(); mb_faith_init(); mb_fx_init();
            mb_unit_seed_wildlife();
            mb_world_start(&s_cx, &s_cy);
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
    uint32_t seed = 0x1d0f2c31u;
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
    if (getenv("MOTEBOX_VSTAT")) {
        for (int i = 0; i < MAXV; i++) {
            if (!mb_v[i].alive) continue;
            int farms = 0, fields = 0;
            for (int y = mb_v[i].y - 8; y <= mb_v[i].y + 8; y++)
                for (int x = mb_v[i].x - 8; x <= mb_v[i].x + 8; x++)
                    if (mb_in(x, y) && mb_w.claim[AT(x, y)] == i) {
                        if (mb_w.obj[AT(x, y)] == O_FARM) farms++;
                        if (mb_w.biome[AT(x, y)] == B_FARM) fields++;
                    }
            fprintf(stderr, "v%-3d pop=%-4d house=%-4d food=%-4d farms=%-3d fields=%-3d %s\n",
                    i, mb_v[i].pop, mb_v[i].housing, mb_v[i].food, farms, fields,
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
    }
    if (s_stat) mb_world_stats();
#endif
    view_set(1);
}

static void g_update(float dt)
{
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
    mb_fx_step(dt);
    mb_chron_step(dt);
    mb_draw_prepare();
    mb_audio_step(dt);
    if (s_denied > 0.0f) s_denied -= dt;

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

    /* MENU opens the God Menu: the laws, the chronicle, the legends, the save.
     * A blocking engine menu is the right tool — it is a pause, and the sim
     * genuinely should stop while you read a history. */
    if (mote_just_pressed(in, MOTE_BTN_MENU) && !wheel) god_menu();

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
    if (mb_law(LAW_FOLLOW) && !wheel) {
        int fx2, fy2;
        if (mb_chron_focus(&fx2, &fy2)) { s_cx = fx2; s_cy = fy2; }
    }

    /* --- LB TAP cycles speed (LB HOLD is the wheel, handled above); RB toggles
     * the zoom, unless the wheel is up, where RB pages tabs --- */
    if (!wheel && mote_just_released(in, MOTE_BTN_LB) && !s_lb_was_wheel)
        s_speed = (s_speed + 1) & 3;
    s_lb_was_wheel = wheel;
    if (!wheel && mote_just_pressed(in, MOTE_BTN_RB)) view_set(!s_god);

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
        mb_fx_draw_mortal(s_cam_x, s_cam_y);
    } else {
        mote->scene2d_begin(0, 0);               /* nothing but the background */
    }
}

static void g_overlay(uint16_t *fb)
{
    char buf[40];
    int year = (int)(mb_w.tick / TPY);

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
            acc = 0;
        }
    }
#endif

    /* Units and particles in God's Eye are drawn HERE rather than in the band
     * pass, because both are lists the pass would have to filter per band. One
     * pixel each, after the terrain, so they sit on top of it. */
    if (s_god) { mb_god_units(fb, 0, VIEW_H); mb_fx_draw_god(fb); }

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
    if (toast) {
        /* A HEADLINE TAKES THE WHOLE STRIP for a few seconds — interrupting the
         * readout is the point, the story is more urgent than the coordinates —
         * and it gets its own row so nothing sits on top of it. */
        snprintf(buf, sizeof buf, "Y%d %s", year, mb_age_name());
        hud_text(fb, buf, 1, HUD_Y, 126, C_TEXT, -1);
        hud_text(fb, toast, 1, HUD_Y + 8, 126, C_CURS, -1);
    } else {
        /* row one: speed, year, the selected power, the view */
        hud_text(fb, SPEED_NAME[s_speed], HC_SPEED_X, HUD_Y, HC_SPEED_W, C_HI, -1);
        snprintf(buf, sizeof buf, "Y%d", year);
        hud_text(fb, buf, HC_YEAR_X, HUD_Y, HC_YEAR_W, C_HI, -1);
        hud_text(fb, mb_power_name(), HC_POWER_X, HUD_Y, HC_POWER_W, C_HI, -1);
        hud_text(fb, s_god ? "EYE" : "GND", HC_VIEW_X, HUD_Y, HC_VIEW_W, C_TEXT, 1);

        /* row two: what is under the cursor, and what casting costs. A burning cell
         * says so, because that is the more urgent fact about it. */
        uint8_t b = mb_w.biome[AT(s_cx, s_cy)];
        uint8_t o = mb_w.obj[AT(s_cx, s_cy)];
        uint8_t k = mb_fkind(mb_w.flux[AT(s_cx, s_cy)]);
        int v = mb_w.claim[AT(s_cx, s_cy)];
        if (k && k < FX_N)
            snprintf(buf, sizeof buf, "%s %s", B_NAME[b < B_N ? b : 0], FX_NAME[k]);
        else if (v && mb_v[v].alive) {
            /* on someone's land, name the place: it is the most interesting fact */
            char pl[24];
            mb_name_str(pl, sizeof pl, 0, mb_v[v].name);
            snprintf(buf, sizeof buf, "%s %d", pl, mb_v[v].pop);
        } else if (o && o < O_N && O_NAME[o][0])
            snprintf(buf, sizeof buf, "%s %s", B_NAME[b < B_N ? b : 0], O_NAME[o]);
        else
            snprintf(buf, sizeof buf, "%s %d,%d", B_NAME[b < B_N ? b : 0], s_cx, s_cy);
        hud_text(fb, buf, HC_INFO_X, HUD_Y + 8, HC_INFO_W, k ? C_WARN : C_TEXT, -1);

        if (s_denied > 0.0f) {
            hud_text(fb, "NO FAITH", HC_FAITH_X, HUD_Y + 8, HC_FAITH_W, C_WARN, 1);
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
