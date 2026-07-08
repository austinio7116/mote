/*
 * ThumbyElite — galaxy + system map screens.
 *
 * Galaxy map: 2D chart (4 px/ly), d-pad pans a cursor that snaps its
 * highlight to the nearest star; jump-range ring around the current
 * system; A engages hyperspace on an in-range highlight.
 *
 * System map: schematic orbit strip — star at the left, planets spaced
 * by orbit with their stations; up/down cycles POIs (beacon first),
 * A engages supercruise.
 */
#include "ui_map.h"
#include "elite_game.h"
#include "elite_platform.h"
#include "elite_audio.h"   /* sfx_ui_move (was implicit) */
#include "mission.h"

/* --- chart data layers (RB cycles SPECTRAL / THREAT / FACTION) -------- */
static int s_chart_layer;

/* Per-star gov/threat/faction, direct-mapped cache so panning never
 * regenerates a system twice (full gen is cheap but not 40x/frame). */
typedef struct {
    SysAddr a;
    int8_t gov, threat, fac;
    int8_t econ;          /* -1 none, -2 multi, else EconType */
    uint8_t valid;
} StarBrief;
static StarBrief s_brief[96];

static const StarBrief *star_brief(SysAddr a) {
    uint32_t h = (uint32_t)(a.sx * 73856093) ^ (uint32_t)(a.sy * 19349663)
                 ^ (uint32_t)(a.idx * 83492791u);
    StarBrief *b = &s_brief[h % 96u];
    if (!b->valid || !sysaddr_eq(b->a, a)) {
        SystemInfo si;
        galaxy_generate(a, &si);
        b->a = a;
        b->gov = (int8_t)si.gov;
        b->threat = (int8_t)si.threat;
        b->fac = (int8_t)system_faction(a);
        if (si.n_stations == 0) {
            b->econ = -1;
        } else {
            b->econ = (int8_t)si.stations[0].econ;
            for (int i = 1; i < si.n_stations; i++)
                if ((int8_t)si.stations[i].econ != b->econ) {
                    b->econ = -2;            /* MULTI */
                    break;
                }
        }
        b->valid = 1;
    }
    return b;
}

#include "craft_font.h"
#include "elite_ui.h"
#include "econ.h"
#include "elite_types.h"
#include "craft_font.h"
#include "econ.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define COL_BG      RGB565C(  4,   8,  18)
#define COL_GRID    RGB565C( 18,  28,  46)
#define COL_STAR    RGB565C(210, 215, 230)
#define COL_DIM     RGB565C(120, 126, 145)
#define COL_CUR     RGB565C(120, 255, 120)
#define COL_SELF    RGB565C( 80, 180, 255)
#define COL_RING    RGB565C( 50,  90,  70)
#define COL_TXT     RGB565C(120, 255, 120)
#define COL_WARN    RGB565C(255, 120,  70)
#define COL_TITLE   RGB565C(200, 210, 225)

static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < ELITE_FB_W && (unsigned)y < ELITE_FB_H)
        fb[y * ELITE_FB_W + x] = c;
}
static void fill(uint16_t *fb, uint16_t c) {
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++) fb[i] = c;
}

/* --- shared edge detection --------------------------------------------*/
typedef struct { CraftRawButtons prev; } Edges;
static Edges s_edge;
/* Debounce: treat every button as already-held on open, so whatever
 * press navigated INTO the screen must be released before it can act.
 * NOTE: never memset(0xFF) over _Bool — !x compiles as x^1 and 0xFE
 * stays truthy; assign true per field. */
static void edges_reset(void) {
    CraftRawButtons b;
    b.up = b.down = b.left = b.right = true;
    b.a = b.b = b.lb = b.rb = b.menu = true;
    s_edge.prev = b;
}
#define JUST(btn, field) ((btn)->field && !s_edge.prev.field)

/* ====================== GALAXY MAP ===================================== */
#define GMAP_SCALE (32.0f / SECTOR_LY)  /* px per ly — pinned to px per
                                            SECTOR so the chart layout
                                            never moves when SECTOR_LY
                                            is retuned */

static SysAddr s_cur_sys;
static float s_fuel, s_range;
static float s_cx_ly, s_cy_ly;   /* cursor (and view centre) */
static SysAddr s_hl;             /* highlighted star */
static bool s_hl_valid;
static bool s_gmap_info;       /* survey sheet over the chart */
static float s_hl_dist;

void map_galaxy_open(SysAddr current, float fuel_ly, float range_ly) {
    s_cur_sys = current;
    s_fuel = fuel_ly;
    s_range = range_ly;
    galaxy_star_pos(current, &s_cx_ly, &s_cy_ly);
    s_hl = current;
    s_hl_valid = true;
    s_hl_dist = 0;
    s_gmap_info = false;
    edges_reset();
}

static void gmap_highlight(void) {
    /* Nearest star to the cursor within 2.5 ly, scanning nearby sectors. */
    int sx0 = (int)floorf((s_cx_ly - 4) / SECTOR_LY);
    int sx1 = (int)floorf((s_cx_ly + 4) / SECTOR_LY);
    int sy0 = (int)floorf((s_cy_ly - 4) / SECTOR_LY);
    int sy1 = (int)floorf((s_cy_ly + 4) / SECTOR_LY);
    float best = 2.5f * 2.5f;
    s_hl_valid = false;
    for (int sy = sy0; sy <= sy1; sy++)
        for (int sx = sx0; sx <= sx1; sx++) {
            int n = galaxy_sector_stars(sx, sy);
            for (int i = 0; i < n; i++) {
                SysAddr a = { sx, sy, (uint8_t)i };
                float x, y;
                galaxy_star_pos(a, &x, &y);
                float d2 = (x - s_cx_ly) * (x - s_cx_ly) +
                           (y - s_cy_ly) * (y - s_cy_ly);
                if (d2 < best) {
                    best = d2;
                    s_hl = a;
                    s_hl_valid = true;
                }
            }
        }
    if (s_hl_valid) {
        float hx, hy, px_, py_;
        galaxy_star_pos(s_hl, &hx, &hy);
        galaxy_star_pos(s_cur_sys, &px_, &py_);
        s_hl_dist = sqrtf((hx - px_) * (hx - px_) + (hy - py_) * (hy - py_));
    }
}

/* Snap the cursor to the nearest star in a +-60deg wedge along (dx,dy). */
static void gmap_snap(float dx, float dy) {
    int sx0 = (int)floorf((s_cx_ly - 18) / SECTOR_LY);
    int sx1 = (int)floorf((s_cx_ly + 18) / SECTOR_LY);
    int sy0 = (int)floorf((s_cy_ly - 18) / SECTOR_LY);
    int sy1 = (int)floorf((s_cy_ly + 18) / SECTOR_LY);
    float best = 1e9f, bx = s_cx_ly, by = s_cy_ly;
    for (int sy = sy0; sy <= sy1; sy++)
        for (int sx = sx0; sx <= sx1; sx++) {
            int n = galaxy_sector_stars(sx, sy);
            for (int i = 0; i < n; i++) {
                SysAddr a = { sx, sy, (uint8_t)i };
                float x, y;
                galaxy_star_pos(a, &x, &y);
                float ex = x - s_cx_ly, ey = y - s_cy_ly;
                float d = sqrtf(ex * ex + ey * ey);
                if (d < 0.05f) continue;                /* the one we're on */
                if (ex * dx + ey * dy < d * 0.5f) continue;   /* wedge */
                if (d < best) { best = d; bx = x; by = y; }
            }
        }
    s_cx_ly = bx;
    s_cy_ly = by;
}

MapAction map_galaxy_tick(const CraftRawButtons *btn, float dt,
                          SysAddr *out_addr, float *out_dist_ly) {
    /* Tap = snap to the next star that way; hold = autorepeat snapping,
     * same cadence as every list (0.35s, then steps) — the old smooth
     * pan lost the star-to-star focus (user req). */
    static float s_hold, s_rep;
    bool any = btn->left || btn->right || btn->up || btn->down;
    if (any) s_hold += dt; else { s_hold = 0; s_rep = 0; }

    if (JUST(btn, lb)) {                            /* Info = cycle data layers */
        s_chart_layer = (s_chart_layer + 1) % 4;
        sfx_ui_move();
    }
    if (JUST(btn, left))  gmap_snap(-1, 0);
    if (JUST(btn, right)) gmap_snap(1, 0);
    if (JUST(btn, up))    gmap_snap(0, -1);
    if (JUST(btn, down))  gmap_snap(0, 1);
    if (s_hold > 0.35f) {
        s_rep -= dt;
        if (s_rep <= 0.0f) {
            s_rep = 0.16f;
            if (btn->left)  gmap_snap(-1, 0);
            if (btn->right) gmap_snap(1, 0);
            if (btn->up)    gmap_snap(0, -1);
            if (btn->down)  gmap_snap(0, 1);
        }
    }
    gmap_highlight();

    MapAction act = MAP_NONE;
    /* A on a highlighted system opens the survey sheet; A inside it
     * commits the jump (user req: full intel before burning fuel). */
    if (s_gmap_info) {
        if (JUST(btn, a) && s_hl_valid && !sysaddr_eq(s_hl, s_cur_sys) &&
            s_hl_dist <= s_range && s_hl_dist <= s_fuel) {
            *out_addr = s_hl;
            *out_dist_ly = s_hl_dist;
            s_gmap_info = false;
            act = MAP_ENGAGE_JUMP;
        }
        if (JUST(btn, b) || JUST(btn, menu)) s_gmap_info = false;
        s_edge.prev = *btn;
        return act;
    }
    if (JUST(btn, a) && s_hl_valid) s_gmap_info = true;
    if (JUST(btn, b) || JUST(btn, menu)) act = MAP_CLOSE;
    s_edge.prev = *btn;
    return act;
}

/* Cheap 2D value-noise for the chart's nebula wash. */
static uint32_t nhash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float nnoise(float x, float y) {
    int xi = (int)x, yi = (int)y;
    if (x < 0) xi--;
    if (y < 0) yi--;
    float fx = x - xi, fy = y - yi;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float v00 = (nhash(xi, yi) & 0xFFFF) * (1.0f / 65535.0f);
    float v10 = (nhash(xi + 1, yi) & 0xFFFF) * (1.0f / 65535.0f);
    float v01 = (nhash(xi, yi + 1) & 0xFFFF) * (1.0f / 65535.0f);
    float v11 = (nhash(xi + 1, yi + 1) & 0xFFFF) * (1.0f / 65535.0f);
    float a = v00 + (v10 - v00) * sx, b = v01 + (v11 - v01) * sx;
    return a + (b - a) * sy;
}

/* Nebula density at a galaxy position (ly) — same field the chart washes with,
 * so in-flight nebula matches what you see on the map. 0 = clear. */
float gmap_nebula_density(float gx, float gy) {
    float n = nnoise(gx * 0.055f, gy * 0.055f) * 0.7f +
              nnoise(gx * 0.13f + 31.7f, gy * 0.13f) * 0.3f;
    return (n > 0.52f) ? (n - 0.52f) * 2.1f : 0.0f;
}

/* Survey sheet: everything about the highlighted system. */
static void draw_gmap_info(uint16_t *fb) {
    SystemInfo si;
    galaxy_generate(s_hl, &si);
    uint16_t BG = RGB565C(6, 10, 20), GRID = RGB565C(28, 40, 58);
    uint16_t HDR = RGB565C(200, 210, 225), VAL = RGB565C(120, 255, 120);
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++) fb[i] = BG;
    char buf[30];

    craft_font_draw(fb, si.name, 2, 2, HDR);
    /* star chip */
    uint16_t sc2 = si.star_color;
    for (int yy = 2; yy < 8; yy++)
        for (int xx = 110; xx < 116; xx++)
            fb[yy * ELITE_FB_W + xx] = sc2;
    for (int x = 0; x < 128; x++) fb[10 * ELITE_FB_W + x] = GRID;

    int y = 14;
    static const char *k_cls[6] = { "M DWARF", "K ORANGE", "G YELLOW",
                                    "F WHITE", "A WHITE", "B GIANT" };
    static const char *k_gov[6] = { "ANARCHY", "FEUDAL", "DICTATOR",
                                    "CONFED", "DEMOCRACY", "CORPORATE" };
    static const char *k_econ[8] = { "AGRI", "INDUST", "HITECH", "EXTRACT",
                                     "REFINE", "TOURISM", "MILITARY",
                                     "SERVICE" };
    #define ROW(label, fmt, ...) do { \
        craft_font_draw(fb, label, 2, y, RGB565C(110, 116, 135)); \
        snprintf(buf, sizeof buf, fmt, __VA_ARGS__); \
        craft_font_draw(fb, buf, 52, y, VAL); \
        y += 8; \
    } while (0)
    ROW("DIST", "%d.%dLY%s", (int)s_hl_dist,
        ((int)(s_hl_dist * 10)) % 10,
        sysaddr_eq(s_hl, s_cur_sys) ? " (HERE)"
        : (s_hl_dist <= s_range && s_hl_dist <= s_fuel) ? "" : " OUT!");
    /* Value colours mirror the chart's layer keys (user req): star =
     * spectral, threat = the green->red ramp, faction = empire hue,
     * stations = the economy hues. */
    #define ROWC(label, col, fmt, ...) do { \
        craft_font_draw(fb, label, 2, y, RGB565C(110, 116, 135)); \
        snprintf(buf, sizeof buf, fmt, __VA_ARGS__); \
        craft_font_draw(fb, buf, 52, y, col); \
        y += 8; \
    } while (0)
    ROWC("STAR", si.star_color, "%s", k_cls[si.star_class]);
    {
        uint16_t gc = (si.gov == GOV_ANARCHY) ? RGB565C(255, 70, 50)
                    : (si.gov <= GOV_DICTATOR) ? RGB565C(240, 150, 60)
                                               : RGB565C(90, 180, 255);
        ROWC("GOV", gc, "%s", k_gov[si.gov]);
    }
    {
        static const char *k_threat[5] = { "SAFE", "LOW", "MEDIUM",
                                           "HIGH", "LETHAL" };
        static const uint16_t tcol[5] = {
            RGB565C(70, 150, 110), RGB565C(110, 190, 90),
            RGB565C(220, 190, 70), RGB565C(240, 130, 50),
            RGB565C(255, 70, 50) };
        int t = si.threat > 4 ? 4 : si.threat;
        ROWC("THREAT", tcol[t], "%s", k_threat[t]);
    }
    {
        static const uint16_t fcol[3] = {
            RGB565C(90, 160, 255), RGB565C(255, 95, 120),
            RGB565C(245, 200, 80) };
        int f = (int)system_faction(s_hl);
        ROWC("FACTION", fcol[f % 3], "%s", k_faction_names[f]);
    }
    ROW("PLANETS", "%d", si.n_planets);
    if (econ_has_black_market(&si)) {
        craft_font_draw(fb, "BLACK MARKET", 2, y, RGB565C(220, 100, 200));
        y += 8;
    }
    /* Per-mission objectives flagged in THIS system, by their log label —
     * so a full mission log shows you which contract points where. */
    for (int i = 0; i < MAX_MISSIONS && y < 104; i++) {
        const Mission *m = &g_missions[i];
        if (m->done || m->type == MIS_NONE) continue;
        if ((m->type == MIS_BOUNTY || m->type == MIS_DELIVERY ||
             m->type == MIS_ASSASSINATE || m->type == MIS_WARZONE) &&
            sysaddr_eq(m->target, s_hl)) {
            snprintf(buf, sizeof buf, "! %s", m->label);
            craft_font_draw(fb, buf, 2, y, RGB565C(255, 120, 70));
            y += 8;
        }
    }
    #undef ROW

    y += 2;
    craft_font_draw(fb, si.n_stations ? "STATIONS:" : "NO STATIONS", 2, y,
                    HDR);
    y += 8;
    for (int i = 0; i < si.n_stations && y < 108; i++) {
        snprintf(buf, sizeof buf, "%s", si.stations[i].name);
        craft_font_draw(fb, buf, 4, y, RGB565C(160, 170, 190));
        y += 7;
        {
            static const uint16_t ecol[8] = {
                RGB565C(110, 210, 90), RGB565C(245, 150, 60),
                RGB565C(90, 210, 255), RGB565C(190, 130, 80),
                RGB565C(150, 160, 190), RGB565C(245, 120, 210),
                RGB565C(255, 80, 70), RGB565C(150, 150, 150) };
            snprintf(buf, sizeof buf, " %s T%d",
                     k_econ[si.stations[i].econ], si.stations[i].tech);
            craft_font_draw(fb, buf, 4, y,
                            ecol[si.stations[i].econ & 7]);
        }
        y += 9;
    }

    for (int x = 0; x < 128; x++) fb[118 * ELITE_FB_W + x] = GRID;
    bool can = s_hl_valid && !sysaddr_eq(s_hl, s_cur_sys) &&
               s_hl_dist <= s_range && s_hl_dist <= s_fuel;
    { char h[40];
      if (can) snprintf(h, sizeof h, "%s:JUMP %s:DATA %s:BACK",
          plat_menu_btn(MB_A), plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
      else snprintf(h, sizeof h, "%s:DATA %s:BACK",
          plat_menu_btn(MB_INFO), plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 2, 121, RGB565C(110, 116, 135)); }
}

void map_galaxy_draw(uint16_t *fb) {
    fill(fb, COL_BG);

    /* Nebula wash: two-octave noise in galaxy space (pans with the
     * view), blue-violet with a warmer second field. 4x4 blocks. */
    {
        float wx0 = s_cx_ly - 64.0f / GMAP_SCALE;
        float wy0 = s_cy_ly - 64.0f / GMAP_SCALE;
        for (int by = 10; by < 118; by += 4) {
            for (int bx = 0; bx < 128; bx += 4) {
                float gx = wx0 + bx / GMAP_SCALE, gy = wy0 + by / GMAP_SCALE;
                float n = nnoise(gx * 0.055f, gy * 0.055f) * 0.7f +
                          nnoise(gx * 0.13f + 31.7f, gy * 0.13f) * 0.3f;
                if (n < 0.52f) continue;
                float k = (n - 0.52f) * 2.1f;
                if (k > 1.0f) k = 1.0f;
                float w = nnoise(gx * 0.08f + 77.0f, gy * 0.08f - 19.0f);
                int r = (int)(k * (w > 0.55f ? 13 : 6));
                int g = (int)(k * 5);
                int b = (int)(k * (w > 0.55f ? 12 : 18));
                uint16_t c = (uint16_t)(((r & 31) << 11) | ((g & 63) << 5) |
                                        (b & 31));
                for (int yy = by; yy < by + 4 && yy < 118; yy++)
                    for (int xx = bx; xx < bx + 4 && xx < 128; xx++)
                        fb[yy * ELITE_FB_W + xx] = c;
            }
        }
    }

    /* Sector grid lines. */
    float x0_ly = s_cx_ly - 64.0f / GMAP_SCALE;
    float y0_ly = s_cy_ly - 64.0f / GMAP_SCALE;
    for (float gx = floorf(x0_ly / SECTOR_LY) * SECTOR_LY;; gx += SECTOR_LY) {
        int sx = (int)((gx - x0_ly) * GMAP_SCALE);
        if (sx > 127) break;
        if (sx >= 0)
            for (int y = 10; y < 118; y += 2) px(fb, sx, y, COL_GRID);
    }
    for (float gy = floorf(y0_ly / SECTOR_LY) * SECTOR_LY;; gy += SECTOR_LY) {
        int sy = (int)((gy - y0_ly) * GMAP_SCALE);
        if (sy > 117) break;
        if (sy >= 10)
            for (int x = 0; x < 128; x += 2) px(fb, x, sy, COL_GRID);
    }

    /* Jump-range ring around the current system. */
    float pxl, pyl;
    galaxy_star_pos(s_cur_sys, &pxl, &pyl);
    int rcx = (int)((pxl - x0_ly) * GMAP_SCALE);
    int rcy = (int)((pyl - y0_ly) * GMAP_SCALE);
    float rr = (s_range < s_fuel ? s_range : s_fuel) * GMAP_SCALE;
    for (int i = 0; i < 72; i++) {
        float a = (float)i * (6.2831853f / 72.0f);
        px(fb, rcx + (int)(rr * cosf(a)), rcy + (int)(rr * sinf(a)), COL_RING);
    }

    /* Stars in view. */
    int sx0 = (int)floorf(x0_ly / SECTOR_LY) - 1;
    int sx1 = (int)floorf((x0_ly + 128 / GMAP_SCALE) / SECTOR_LY) + 1;
    int sy0 = (int)floorf(y0_ly / SECTOR_LY) - 1;
    int sy1 = (int)floorf((y0_ly + 128 / GMAP_SCALE) / SECTOR_LY) + 1;
    static uint8_t s_twinkle;
    s_twinkle++;
    for (int sy = sy0; sy <= sy1; sy++)
        for (int sx = sx0; sx <= sx1; sx++) {
            int n = galaxy_sector_stars(sx, sy);
            for (int i = 0; i < n; i++) {
                SysAddr a = { sx, sy, (uint8_t)i };
                float x, y;
                galaxy_star_pos(a, &x, &y);
                int dx = (int)((x - x0_ly) * GMAP_SCALE);
                int dy = (int)((y - y0_ly) * GMAP_SCALE);
                int cls = galaxy_star_class(a);
                uint16_t sc2 = galaxy_star_color(a);
                if (s_chart_layer == 1) {
                    /* THREAT heat-map: green..red, steady (no twinkle
                     * on data — it reads as flicker, not stars). */
                    static const uint16_t tcol[5] = {
                        RGB565C(70, 150, 110), RGB565C(110, 190, 90),
                        RGB565C(220, 190, 70), RGB565C(240, 130, 50),
                        RGB565C(255, 70, 50) };
                    const StarBrief *br = star_brief(a);
                    sc2 = tcol[br->threat < 0 ? 0
                              : br->threat > 4 ? 4 : br->threat];
                } else if (s_chart_layer == 2) {
                    /* FACTION territory: one hue per empire. */
                    static const uint16_t fcol[3] = {
                        RGB565C(90, 160, 255), RGB565C(255, 95, 120),
                        RGB565C(245, 200, 80) };
                    const StarBrief *br = star_brief(a);
                    sc2 = fcol[br->fac % 3];
                } else if (s_chart_layer == 3) {
                    /* ECONOMY: one hue per type; white = mixed ports,
                     * near-dark = no stations at all. */
                    static const uint16_t ecol[8] = {
                        RGB565C(110, 210, 90),    /* AGRI  green  */
                        RGB565C(245, 150, 60),    /* INDUST orange*/
                        RGB565C(90, 210, 255),    /* HITECH cyan  */
                        RGB565C(190, 130, 80),    /* EXTRACT copper*/
                        RGB565C(150, 160, 190),   /* REFINE steel */
                        RGB565C(245, 120, 210),   /* TOURISM pink */
                        RGB565C(255, 80, 70),     /* MILITARY red */
                        RGB565C(150, 150, 150),   /* SERVICE grey */
                    };
                    const StarBrief *br = star_brief(a);
                    sc2 = (br->econ == -1) ? RGB565C(45, 50, 62)
                        : (br->econ == -2) ? RGB565C(255, 255, 255)
                                           : ecol[br->econ & 7];
                }
                /* Dim halo for big stars, twinkle phase per star. */
                uint32_t tw = nhash(sx * 31 + i, sy * 17);
                int bright = (s_chart_layer != 0) ||
                             ((s_twinkle + (tw & 31)) & 31) < 28;
                uint16_t dimc = (uint16_t)((sc2 >> 1) & 0x7BEF);
                if (cls >= STAR_F && s_chart_layer == 0) { /* hot glow */
                    px(fb, dx - 1, dy, dimc);
                    px(fb, dx + 1, dy, dimc);
                    px(fb, dx, dy - 1, dimc);
                    px(fb, dx, dy + 1, dimc);
                }
                if (cls >= STAR_A && s_chart_layer == 0) {
                    px(fb, dx + 1, dy + 1, dimc);  /* giants: big core */
                    px(fb, dx - 1, dy - 1, dimc);
                }
                px(fb, dx, dy, bright ? sc2 : dimc);
                /* Mission objective: pulsing red diamond. */
                if (mission_objective_here(a) && (s_twinkle & 8)) {
                    px(fb, dx - 3, dy, COL_WARN);
                    px(fb, dx + 3, dy, COL_WARN);
                    px(fb, dx, dy - 3, COL_WARN);
                    px(fb, dx, dy + 3, COL_WARN);
                }
                if (sysaddr_eq(a, s_cur_sys)) {     /* our marker */
                    px(fb, dx - 2, dy, COL_SELF);
                    px(fb, dx + 2, dy, COL_SELF);
                    px(fb, dx, dy - 2, COL_SELF);
                    px(fb, dx, dy + 2, COL_SELF);
                }
            }
        }

    if (s_gmap_info && s_hl_valid) {
        draw_gmap_info(fb);
        return;
    }

    /* Highlight + cursor + floating name tag. */
    if (s_hl_valid) {
        float hx, hy;
        galaxy_star_pos(s_hl, &hx, &hy);
        int dx = (int)((hx - x0_ly) * GMAP_SCALE);
        int dy = (int)((hy - y0_ly) * GMAP_SCALE);
        uint16_t c = (s_hl_dist <= s_range && s_hl_dist <= s_fuel)
                         ? COL_CUR : COL_WARN;
        for (int k = 2; k <= 3; k++) {
            px(fb, dx - k, dy - k, c); px(fb, dx + k, dy - k, c);
            px(fb, dx - k, dy + k, c); px(fb, dx + k, dy + k, c);
        }
        char name[14];
        galaxy_system_name(s_hl, name);
        int tw2 = eui_textw(name);
        int lx = dx + 6;
        if (lx + tw2 > 126) lx = dx - 6 - tw2;
        if (lx < 1) lx = 1;
        int ly = dy - 13;
        if (ly < 15) ly = dy + 5;
        eui_text(fb, name, lx, ly, c);
    }

    /* Header + footer info. */
    if (s_chart_layer == 0)      eui_text(fb, "GALAXY CHART",   2, 1, COL_TITLE);
    else if (s_chart_layer == 1) eui_text(fb, "CHART: THREAT",  2, 1, RGB565C(240, 140, 60));
    else if (s_chart_layer == 2) eui_text(fb, "CHART: FACTION", 2, 1, RGB565C(120, 170, 255));
    else                         eui_text(fb, "CHART: ECONOMY", 2, 1, RGB565C(245, 150, 60));
    char buf[32];
    snprintf(buf, sizeof buf, "%d.%dLY", (int)s_fuel, ((int)(s_fuel * 10)) % 10);
    craft_font_draw(fb, buf, 126 - craft_font_width(buf), 3, COL_TXT);
    for (int x = 0; x < 128; x++) px(fb, x, 13, COL_GRID);
    for (int x = 0; x < 128; x++) px(fb, x, 115, COL_GRID);
    if (s_hl_valid) {
        char name[14];
        galaxy_system_name(s_hl, name);
        if (sysaddr_eq(s_hl, s_cur_sys))
            snprintf(buf, sizeof buf, "%s  HERE", name);
        else
            snprintf(buf, sizeof buf, "%s  %d.%dLY %s", name,
                     (int)s_hl_dist, ((int)(s_hl_dist * 10)) % 10,
                     (s_hl_dist <= s_range && s_hl_dist <= s_fuel)
                         ? "A:JUMP" : "RANGE!");
        eui_textclip(fb, buf, 2, 126, 117, COL_TXT);
    } else {
        { char h[16]; snprintf(h, sizeof h, "%s:BACK", plat_menu_btn(MB_B));
          craft_font_draw(fb, h, 2, 119, COL_DIM); }
    }
    /* Layer legends, bottom-right of the map area. */
    if (s_chart_layer == 1) {
        craft_font_draw(fb, "SAFE", 70, 110, RGB565C(110, 190, 90));
        craft_font_draw(fb, "DEADLY", 98, 110, RGB565C(255, 70, 50));
    } else if (s_chart_layer == 2) {
        craft_font_draw(fb, "COA", 70, 110, RGB565C(90, 160, 255));
        craft_font_draw(fb, "DOM", 90, 110, RGB565C(255, 95, 120));
        craft_font_draw(fb, "FRE", 110, 110, RGB565C(245, 200, 80));
    } else if (s_chart_layer == 3) {
        /* two-letter coloured key per economy + ?? for mixed */
        static const char *k_el[9] = { "Ag", "In", "Hi", "Ex", "Re",
                                       "To", "Mi", "Sv", "??" };
        static const uint16_t k_ec[9] = {
            RGB565C(110, 210, 90), RGB565C(245, 150, 60),
            RGB565C(90, 210, 255), RGB565C(190, 130, 80),
            RGB565C(150, 160, 190), RGB565C(245, 120, 210),
            RGB565C(255, 80, 70), RGB565C(150, 150, 150),
            RGB565C(255, 255, 255) };
        for (int i = 0; i < 9; i++)
            craft_font_draw(fb, k_el[i], 11 + i * 13, 110, k_ec[i]);
    }
}

/* ====================== SYSTEM MAP ===================================== */
static Poi  s_pois[MAX_POIS];
static int  s_npois, s_cursor;
static Vec3 s_player_mm;

void map_system_open(Vec3 player_pos_mm) {
    s_npois = system_pois(s_pois, MAX_POIS);
    s_cursor = 0;
    s_player_mm = player_pos_mm;
    edges_reset();
}

MapAction map_system_tick(const CraftRawButtons *btn, float dt, Poi *out_poi) {
    MapAction act = MAP_NONE;
    /* Hold-to-scroll, same cadence as every station list (the original
     * 'sector map' request — taps only was the miss). */
    static float s_rep_up, s_rep_dn;
    bool step_dn = JUST(btn, down), step_up = JUST(btn, up);
    if (btn->down) {
        s_rep_dn += dt;
        if (s_rep_dn > 0.35f) { step_dn = true; s_rep_dn = 0.23f; }
    } else s_rep_dn = 0;
    if (btn->up) {
        s_rep_up += dt;
        if (s_rep_up > 0.35f) { step_up = true; s_rep_up = 0.23f; }
    } else s_rep_up = 0;
    if (step_dn && s_cursor < s_npois - 1) s_cursor++;
    if (step_up && s_cursor > 0) s_cursor--;
    if (JUST(btn, a) && s_npois > 0) {
        *out_poi = s_pois[s_cursor];
        act = MAP_ENGAGE_SC;
    }
    if (JUST(btn, b) || JUST(btn, menu)) act = MAP_CLOSE;
    s_edge.prev = *btn;
    return act;
}

void map_system_draw(uint16_t *fb) {
    fill(fb, COL_BG);
    const SystemInfo *si = system_info();

    char buf[36];
    snprintf(buf, sizeof buf, "%s SYSTEM", si->name);
    eui_textclip(fb, buf, 2, 126, 1, COL_TITLE);
    for (int x = 0; x < 128; x++) px(fb, x, 13, COL_GRID);

    /* Schematic strip: star at left, planets by orbit order. */
    int strip_y = 24;
    /* Star. */
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (dx * dx + dy * dy <= 16)
                px(fb, 8 + dx, strip_y + dy, si->star_color);
    /* Cursor link: highlight the selected POI's body in the strip. */
    int hi_x = -1;
    if (s_npois > 0) {
        const Poi *cp2 = &s_pois[s_cursor];
        if (cp2->kind == POI_BEACON) hi_x = 8;             /* the star end */
        else if (cp2->kind == POI_PLANET) hi_x = 24 + cp2->index * 14;
        else
            for (int i = 0; i < si->n_planets; i++)
                if (si->planets[i].station == cp2->index)
                    hi_x = 24 + i * 14;                    /* host planet */
    }
    if (hi_x >= 0) {
        /* up-chevron under the body, cursor green */
        px(fb, hi_x, strip_y + 8, COL_CUR);
        px(fb, hi_x - 1, strip_y + 9, COL_CUR);
        px(fb, hi_x + 1, strip_y + 9, COL_CUR);
        px(fb, hi_x - 2, strip_y + 10, COL_CUR);
        px(fb, hi_x + 2, strip_y + 10, COL_CUR);
    }
    for (int i = 0; i < si->n_planets; i++) {
        int x = 24 + i * 14;
        int r = si->planets[i].type == PT_GAS ? 4 : 2;
        uint16_t c = (si->planets[i].type == PT_LAVA) ? RGB565C(200, 90, 30)
                   : (si->planets[i].type == PT_ICE) ? RGB565C(210, 225, 240)
                   : (si->planets[i].type == PT_GAS) ? RGB565C(200, 170, 120)
                   : (si->planets[i].type == PT_ROCK) ? RGB565C(150, 130, 105)
                   : RGB565C(60, 130, 180);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r) px(fb, x + dx, strip_y + dy, c);
        if (si->planets[i].rings) {            /* ringed: a thin line barely
                                                * past the disc (keeps the
                                                * strip spacing tight) */
            uint16_t rc = RGB565C(205, 198, 178);
            for (int dx = -(r + 1); dx <= r + 1; dx++)
                px(fb, x + dx, strip_y, rc);
        }
        if (si->planets[i].station >= 0) {     /* station tick above */
            uint16_t tc = (s_npois > 0 &&
                           s_pois[s_cursor].kind == POI_STATION &&
                           si->planets[i].station ==
                               s_pois[s_cursor].index) ? COL_CUR : COL_DIM;
            px(fb, x, strip_y - 7, tc);
            px(fb, x - 1, strip_y - 8, tc);
            px(fb, x + 1, strip_y - 8, tc);
        }
    }

    /* POI list with cursor (shortened — scan strip below). */
    int list_y = 36;
    int first = s_cursor - 4 > 0 ? s_cursor - 4 : 0;
    for (int i = first; i < s_npois && list_y < 92; i++, list_y += 9) {
        const Poi *p = &s_pois[i];
        float dist = v3_len(v3_sub(p->pos_mm, s_player_mm));
        const char *icon = (p->kind == POI_BEACON) ? "@"
                         : (p->kind == POI_STATION) ? "#" : "O";
        snprintf(buf, sizeof buf, "%s %-13s %5dMM", icon, p->name, (int)dist);
        craft_font_draw(fb, buf, 8, list_y,
                        (i == s_cursor) ? COL_CUR : COL_DIM);
        if (i == s_cursor) craft_font_draw(fb, ">", 2, list_y, COL_CUR);
        {
            PoiIntel li;
            elite_game_poi_intel(p, &li);
            if (li.distress)
                craft_font_draw(fb, "!", 122, list_y,
                                RGB565C(255, 90, 70));
        }
    }

    /* Scan strip: live intel for the cursor POI. Belts are certain
     * (persistent geography); pirates/salvage are odds. */
    if (s_npois > 0) {
        for (int x = 0; x < 128; x++) px(fb, x, 96, COL_GRID);
        PoiIntel in2;
        elite_game_poi_intel(&s_pois[s_cursor], &in2);
        const SystemInfo *si2 = system_info();
        const Poi *cp = &s_pois[s_cursor];
        /* line 1: what this place is */
        if (cp->kind == POI_STATION) {
            const StationInfo *st = &si2->stations[cp->index];
            static const char *k_se[8] = { "AGRI", "INDUST", "HITECH",
                                           "EXTRACT", "REFINE", "TOURISM",
                                           "MILITARY", "SERVICE" };
            snprintf(buf, sizeof buf, "%s T%d%s",
                     k_se[st->econ], st->tech,
                     in2.police ? "  POLICE" : "");
        } else if (cp->kind == POI_PLANET) {
            static const char *k_pt[5] = { "ROCK", "OCEAN", "ICE",
                                           "LAVA", "GAS" };
            snprintf(buf, sizeof buf, "%s WORLD",
                     k_pt[si2->planets[cp->index].type]);
        } else {
            snprintf(buf, sizeof buf, "NAV BEACON");
        }
        craft_font_draw(fb, buf, 2, 99, COL_TXT);
        /* line 2: rocks + danger odds */
        const char *pir = in2.pirate_pct == 0 ? "NONE"
                        : in2.pirate_pct < 30 ? "LOW"
                        : in2.pirate_pct < 60 ? "MED" : "HIGH";
        snprintf(buf, sizeof buf, "%s PIRATE:%s SALV:%d%%",
                 in2.belt ? "BELT!" : "ROCKS:-", pir,
                 (int)in2.debris_pct);
        craft_font_draw(fb, buf, 2, 108,
                        in2.belt ? RGB565C(255, 200, 90) : COL_DIM);
        if (in2.distress)
            craft_font_draw(fb, "DISTRESS CALL!",
                            128 - craft_font_width("DISTRESS CALL!") - 2,
                            99, RGB565C(255, 90, 70));
    }

    for (int x = 0; x < 128; x++) px(fb, x, 118, COL_GRID);
    { char h[36]; snprintf(h, sizeof h, "%s:SUPERCRUISE %s:BACK",
        plat_menu_btn(MB_A), plat_menu_btn(MB_B));
      craft_font_draw(fb, h, 2, 121, COL_TXT); }
}
