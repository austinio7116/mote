/*
 * ThumbyElite — in-flight HUD (128x128), Elite-style dashboard.
 *
 * Full-width console along the bottom with a raised centre housing for
 * the scanner (the iconic Elite silhouette). Speed/throttle instruments
 * live in the left panel, shield/hull/heat in the right. Canopy pillars
 * frame the view. Centre: crosshair, target brackets, hit/kill markers.
 */
#include "r3d_pipe.h"
#include "elite_engine.h"
#include "ui_hud.h"
#include "elite_player.h"
#include "elite_types.h"
#include "elite_entity.h"
#include "elite_combat.h"
#include "mission.h"
#include "elite_loot.h"
#include "elite_rocks.h"
#include "r3d_scene.h"
#include "craft_font.h"
#include "ui_icons.h"
#include <stdio.h>

#define COL_SHIELD  RGB565C( 80, 180, 255)
#define COL_HULL    RGB565C(255, 120,  70)
#define COL_HEAT    RGB565C(255, 200,  60)
#define COL_FRAME   RGB565C( 60,  90, 115)
#define COL_CROSS   RGB565C(140, 220, 170)
#define COL_TARGET  RGB565C(255,  90,  70)
#define COL_BLIP_H  RGB565C(255,  80,  60)
#define COL_BLIP_N  RGB565C(170, 170, 180)
#define COL_BLIP_L  RGB565C(255, 210,  70)   /* loot canisters */
#define COL_TEXT    RGB565C(120, 255, 120)
#define COL_NUM     RGB565C(200, 210, 225)
#define COL_CONSOLE RGB565C( 14,  18,  30)
#define COL_STRUT   RGB565C( 95, 110, 140)
#define COL_CANOPY  RGB565C( 55,  65,  88)
#define COL_PILLAR  RGB565C( 30,  38,  55)
#define COL_GRID    RGB565C( 32,  44,  62)
#define COL_CUR_DEST RGB565C(120, 230, 255)   /* supercruise destination */

/* Dashboard geometry. */
#define DASH_TOP   101
#define BULGE_TOP   93
#define BULGE_L0    36   /* bulge foot, left */
#define BULGE_L1    44   /* bulge shoulder, left */
#define BULGE_R1    83
#define BULGE_R0    91

static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < ELITE_FB_W && (unsigned)y < ELITE_FB_H)
        fb[y * ELITE_FB_W + x] = c;
}
static void hline(uint16_t *fb, int x0, int x1, int y, uint16_t c) {
    for (int x = x0; x <= x1; x++) px(fb, x, y, c);
}
static void vline(uint16_t *fb, int x, int y0, int y1, uint16_t c) {
    for (int y = y0; y <= y1; y++) px(fb, x, y, c);
}
static void line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (int)(adx > ady ? adx : ady) + 1;
    float stx = dx / steps, sty = dy / steps;
    float cx = (float)x0, cy = (float)y0;
    for (int i = 0; i <= steps; i++) {
        px(fb, (int)cx, (int)cy, c);
        cx += stx; cy += sty;
    }
}

static void bar(uint16_t *fb, int x, int y, int w, float frac, uint16_t c) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    hline(fb, x, x + w, y - 1, COL_GRID);
    hline(fb, x, x + w, y + 1, COL_GRID);
    px(fb, x - 1, y, COL_GRID);
    px(fb, x + w + 1, y, COL_GRID);
    int fill = (int)(w * frac + 0.5f);
    if (fill > 0) hline(fb, x, x + fill, y, c);
}

int ui_hud_dash_top(void) { return BULGE_TOP; }

/* --- Dashboard ---------------------------------------------------------*/
static void dashboard(uint16_t *fb) {
    /* Console fill: bulge rows then the full-width slab. */
    for (int y = BULGE_TOP; y < DASH_TOP; y++) {
        int t = y - BULGE_TOP;                       /* shoulder slope 1:1 */
        int x0 = BULGE_L1 - t, x1 = BULGE_R1 + t;
        hline(fb, x0, x1, y, COL_CONSOLE);
    }
    for (int y = DASH_TOP; y < ELITE_FB_H; y++)
        hline(fb, 0, ELITE_FB_W - 1, y, COL_CONSOLE);

    /* Edge highlights. */
    hline(fb, 0, BULGE_L0, DASH_TOP, COL_STRUT);
    hline(fb, BULGE_R0, ELITE_FB_W - 1, DASH_TOP, COL_STRUT);
    line(fb, BULGE_L0, DASH_TOP, BULGE_L1, BULGE_TOP, COL_STRUT);
    hline(fb, BULGE_L1, BULGE_R1, BULGE_TOP, COL_STRUT);
    line(fb, BULGE_R1, BULGE_TOP, BULGE_R0, DASH_TOP, COL_STRUT);

    /* Canopy pillars + diagonals. */
    line(fb, 0, 30, 30, 6, COL_CANOPY);
    line(fb, 127, 30, 97, 6, COL_CANOPY);
    vline(fb, 0, 30, DASH_TOP - 1, COL_PILLAR);
    vline(fb, 127, 30, DASH_TOP - 1, COL_PILLAR);
}

/* --- Scanner (Elite disc, in the bulge) -------------------------------- */
#define SC_CX 63
#define SC_CY 110
#define SC_RX 24
#define SC_RY 13
#define SC_RANGE_BASE 400.0f
#define SC_RANGE (player_has_util(EQ_SCANNER) ? 700.0f : SC_RANGE_BASE)

static uint16_t shade565_dim(uint16_t c) {
    return (uint16_t)(((c >> 1) & 0x7BEF));   /* ~half brightness */
}

static void scanner(uint16_t *fb) {
    /* Dim grid cross first, then the rim. */
    hline(fb, SC_CX - SC_RX + 2, SC_CX + SC_RX - 2, SC_CY, COL_GRID);
    vline(fb, SC_CX, SC_CY - SC_RY + 1, SC_CY + SC_RY - 1, COL_GRID);
    for (int i = 0; i < 64; i++) {
        float a = (float)i * (6.2831853f / 64.0f);
        px(fb, SC_CX + (int)(SC_RX * cosf(a)),
               SC_CY + (int)(SC_RY * sinf(a)), COL_FRAME);
    }
    px(fb, SC_CX, SC_CY, RGB565C(255, 255, 255));

    const Ship *p = &g_ships[PLAYER];
    for (int i = 1; i < MAX_SHIPS; i++) {
        const Ship *s = &g_ships[i];
        if (!s->alive) continue;
        Vec3 local = m3_mul_v3_t(&p->basis, v3_sub(s->pos, p->pos));
        float dx = local.x * (SC_RX / SC_RANGE);
        float dz = -local.z * (SC_RY / SC_RANGE);
        float e = (dx * dx) / (SC_RX * SC_RX) + (dz * dz) / (SC_RY * SC_RY);
        bool beyond = (e > 1.0f);
        if (beyond) {
            float k = 1.0f / sqrtf(e);
            dx *= k; dz *= k;
        }
        int fx = SC_CX + (int)dx, fy = SC_CY + (int)dz;
        uint16_t c = s->is_derelict ? RGB565C(190, 205, 220)
                   : s->is_police ? RGB565C(90, 180, 255)
                   : (s->team == TEAM_HOSTILE) ? COL_BLIP_H
                   : s->is_civilian ? RGB565C(110, 230, 110)
                                    : COL_BLIP_N;
        if (beyond) {
            /* Out of scanner range: a dim rim tick, direction only.
             * The old full blip + pegged stalk flipped top-to-bottom
             * on tiny pitch changes near alignment (user report) —
             * altitude data is meaningless out here. */
            px(fb, fx, fy, shade565_dim(c));
            continue;
        }
        int stalk = (int)(-local.y * (40.0f / SC_RANGE));
        if (stalk > 9) stalk = 9;
        if (stalk < -9) stalk = -9;
        px(fb, fx, fy, COL_FRAME);
        if (stalk != 0)
            vline(fb, fx, fy < fy + stalk ? fy : fy + stalk,
                  fy < fy + stalk ? fy + stalk : fy, c);
        px(fb, fx, fy + stalk, c);
        px(fb, fx + 1, fy + stalk, c);
    }

    /* Asteroids: steady dim brown blips. */
    {
        Vec3 rocks[8];
        int nr = rocks_positions(rocks, 8);
        for (int i = 0; i < nr; i++) {
            Vec3 local = m3_mul_v3_t(&p->basis, v3_sub(rocks[i], p->pos));
            float dx = local.x * (SC_RX / SC_RANGE);
            float dz = -local.z * (SC_RY / SC_RANGE);
            float e = (dx * dx) / (SC_RX * SC_RX) +
                      (dz * dz) / (SC_RY * SC_RY);
            if (e > 1.0f) continue;            /* off-disc: skip rocks */
            px(fb, SC_CX + (int)dx, SC_CY + (int)dz,
               RGB565C(150, 125, 95));
        }
    }

    /* Loot canisters: gold blips, blinking so they catch the eye.
     * Components blink brighter than commodity pods. */
    static uint8_t s_blink;
    s_blink++;
    Vec3 cans[6];
    int comp[6];
    int n = loot_positions(cans, comp, 6);
    for (int i = 0; i < n; i++) {
        if (!comp[i] && (s_blink & 16)) continue;       /* slow blink */
        if (comp[i] && (s_blink & 8)) continue;         /* fast blink */
        Vec3 local = m3_mul_v3_t(&p->basis, v3_sub(cans[i], p->pos));
        float dx = local.x * (SC_RX / SC_RANGE);
        float dz = -local.z * (SC_RY / SC_RANGE);
        float e = (dx * dx) / (SC_RX * SC_RX) + (dz * dz) / (SC_RY * SC_RY);
        if (e > 1.0f) {
            float k = 1.0f / sqrtf(e);
            dx *= k; dz *= k;
        }
        int fx = SC_CX + (int)dx, fy = SC_CY + (int)dz;
        int stalk = (int)(-local.y * (40.0f / SC_RANGE));
        if (stalk > 9) stalk = 9;
        if (stalk < -9) stalk = -9;
        if (stalk != 0)
            vline(fb, fx, fy < fy + stalk ? fy : fy + stalk,
                  fy < fy + stalk ? fy + stalk : fy,
                  RGB565C(140, 110, 40));
        px(fb, fx, fy + stalk, COL_BLIP_L);
    }
}

/* --- Target brackets ---------------------------------------------------*/
static void target_box(uint16_t *fb, int target) {
    const Ship *p = &g_ships[PLAYER];
    const Ship *t = &g_ships[target];
    /* Friendlies wear green brackets/arrows, police blue — red is for
     * hostiles (and for friends you've made hostile). */
    uint16_t bc = (t->team == TEAM_HOSTILE) ? COL_TARGET
                : t->is_police ? RGB565C(90, 180, 255)
                : t->is_civilian ? RGB565C(110, 230, 110)
                                 : COL_TARGET;
    float sx, sy;
    uint16_t d;
    if (r3d_pipe_project(v3_sub(t->pos, p->pos), &sx, &sy, &d) &&
        sx > -20 && sx < 148 && sy > -20 && sy < 148) {
        /* TARGETCOMP: lead pip for the active ballistic weapon — put
         * the cross on the pip and the rounds arrive on target. */
        if (player_has_util(EQ_TARGETCOMP)) {
            const WeaponDef *aw = &k_weapons[p->weapons[p->active_w]];
            if (aw->speed > 0.0f && aw->turn < 1.0f) {
                float tt = v3_len(v3_sub(t->pos, p->pos)) / aw->speed;
                Vec3 lead = v3_add(t->pos,
                                   v3_scale(v3_sub(t->vel, p->vel), tt));
                float lx, ly;
                uint16_t ld;
                if (r3d_pipe_project(v3_sub(lead, p->pos), &lx, &ly, &ld)) {
                    uint16_t lc = RGB565C(140, 255, 160);
                    px(fb, (int)lx, (int)ly - 2, lc);
                    px(fb, (int)lx, (int)ly + 2, lc);
                    px(fb, (int)lx - 2, (int)ly, lc);
                    px(fb, (int)lx + 2, (int)ly, lc);
                }
            }
        }
        float z = R3D_DEPTH_K / (float)d;
        int h = (int)(r3d_pipe_focal() * t->mesh->bound_r / z);
        if (h < 5) h = 5;
        if (h > 24) h = 24;
        int x0 = (int)sx - h, x1 = (int)sx + h;
        int y0 = (int)sy - h, y1 = (int)sy + h;
        int l = h / 2;
        hline(fb, x0, x0 + l, y0, bc);
        vline(fb, x0, y0, y0 + l, bc);
        hline(fb, x1 - l, x1, y0, bc);
        vline(fb, x1, y0, y0 + l, bc);
        hline(fb, x0, x0 + l, y1, bc);
        vline(fb, x0, y1 - l, y1, bc);
        hline(fb, x1 - l, x1, y1, bc);
        vline(fb, x1, y1 - l, y1, bc);
    } else {
        /* Edge arrow = the way to TURN: the view-space lateral direction
         * already encodes the shortest rotation, in front OR behind —
         * no rear-hemisphere flip (that sent players the long way). */
        Vec3 v = m3_mul_v3_t(&p->basis, v3_sub(t->pos, p->pos));
        float ax = v.x, ay = -v.y;
        float al = sqrtf(ax * ax + ay * ay);
        if (al < 1e-4f) { ax = 1; ay = 0; al = 1; }
        ax /= al; ay /= al;
        int ex = 64 + (int)(ax * 52.0f);
        int ey = 60 + (int)(ay * 44.0f);
        /* Arrowhead: tip leads, barbs flare BACK from it. */
        px(fb, ex, ey, bc);
        px(fb, ex - (int)(ax * 2 + ay * 2), ey - (int)(ay * 2 - ax * 2),
           bc);
        px(fb, ex - (int)(ax * 2 - ay * 2), ey - (int)(ay * 2 + ax * 2),
           bc);
        px(fb, ex - (int)(ax * 3), ey - (int)(ay * 3), bc);
        px(fb, ex - (int)(ax * 5), ey - (int)(ay * 5), bc);
    }

    /* Target readout under the canopy line, top-right. */
    char buf[24];
    float dist = v3_len(v3_sub(t->pos, p->pos));
    snprintf(buf, sizeof buf, "%dM", (int)dist);
    craft_font_draw(fb, buf, 93, 2, bc);
    bar(fb, 95, 10, 22, t->shield / (t->shield_max > 0 ? t->shield_max : 1),
        COL_SHIELD);
    bar(fb, 95, 14, 22, t->hull / (t->hull_max > 0 ? t->hull_max : 1),
        COL_HULL);
    /* Two lines: WHO they are (faction colour), then how GOOD they
     * are (tier). All raiders are pirates by trade (user q). */
    {
        /* Warzone combatants are SOLDIERS, not pirates (user req):
         * name them by faction, allies in friendly blue. */
        const char *id = t->war_fac
                       ? k_faction_names[(t->war_fac - 1) % N_FACTIONS]
                       : t->is_derelict ? "DERELICT"
                       : t->is_police ? "POLICE"
                       : t->is_civilian ? "CIVILIAN"
                       : t->is_mark ? "** MARK **" : "PIRATE";
        uint16_t idc = t->is_derelict ? RGB565C(170, 190, 210)
                     : (t->war_fac && t->team == TEAM_HOSTILE)
                           ? COL_TARGET
                     : t->is_police ? RGB565C(90, 180, 255)
                     : (t->is_civilian && t->team == TEAM_NEUTRAL)
                           ? RGB565C(110, 230, 110)
                           : COL_TARGET;
        int idx2 = 93;
        int idw = craft_font_width(id);
        if (idx2 + idw > 127) idx2 = 127 - idw;   /* COALITION fits */
        craft_font_draw(fb, id, idx2, 18, idc);
        craft_font_draw(fb, k_tier_names[t->tier > 4 ? 4 : t->tier],
                        93, 26, RGB565C(150, 156, 170));
    }
}

void ui_hud_draw(uint16_t *fb, const HudInfo *info) {
    const Ship *p = &g_ships[PLAYER];

    dashboard(fb);

    /* Crosshair. */
    hline(fb, 59, 61, 64, COL_CROSS);
    hline(fb, 67, 69, 64, COL_CROSS);
    vline(fb, 64, 59, 61, COL_CROSS);
    vline(fb, 64, 67, 69, COL_CROSS);

    /* Hit marker: diagonal ticks flaring from the crosshair. */
    if (combat_hitmarker() > 0.0f) {
        for (int k = 3; k <= 5; k++) {
            px(fb, 64 - k, 64 - k, COL_TARGET);
            px(fb, 64 + k, 64 - k, COL_TARGET);
            px(fb, 64 - k, 64 + k, COL_TARGET);
            px(fb, 64 + k, 64 + k, COL_TARGET);
        }
    }
    /* Kill marker: brief confirm under the crosshair. */
    if (combat_killmarker() > 0.0f)
        craft_font_draw(fb, "KILL", 57, 74, COL_TARGET);

    /* Active weapon + ammo, top-centre under the perf line. */
    char buf[24];
    if (p->n_weapons > 0) {
        const WeaponDef *w = &k_weapons[p->weapons[p->active_w]];
        if (w->ammo_max)
            snprintf(buf, sizeof buf, "%s %d", w->name,
                     (int)p->ammo[p->active_w]);
        else
            snprintf(buf, sizeof buf, "%s", w->name);
        int wx = 64 - craft_font_width(buf) / 2 + 7;
        craft_font_draw(fb, buf, wx,
                        2, (w->ammo_max && p->ammo[p->active_w] <= 0)
                                ? COL_HULL : COL_NUM);
        icon_weapon(fb, wx - 15, 1, p->weapons[p->active_w]);
    }

    /* Left panel: THREE clean rows, nothing below y123 (the old loose
     * cluster put kills + chaff at y124 where the glyph bottoms clip
     * off-screen — 'K0' read as KO; user report). Flags ride the bar
     * rows; chaff lives by the weapon readout top-centre. */
    if (player_has_util(EQ_CHAFF)) {
        char cb[6];
        snprintf(cb, sizeof cb, "C%d", g_player.chaff_charges);
        craft_font_draw(fb, cb, 2, 10, RGB565C(200, 200, 215));
    }
    craft_font_draw(fb, "SP", 2, 102, COL_TEXT);
    bar(fb, 13, 105, 20, v3_len(p->vel) / (p->max_speed * 1.8f), COL_TEXT);
    if (!p->assist) craft_font_draw(fb, "DR", 35, 102, COL_HEAT);
    craft_font_draw(fb, "TH", 2, 109, COL_NUM);
    bar(fb, 13, 112, 20, p->throttle, COL_NUM);
    if (p->boost_t > 0) craft_font_draw(fb, "BS", 35, 109, COL_SHIELD);
    snprintf(buf, sizeof buf, "%3d", (int)v3_len(p->vel));
    craft_font_draw(fb, buf, 2, 117, COL_NUM);


    /* Right panel: shield / hull / heat + wave/kills. */
    craft_font_draw(fb, "S", 94, 102, COL_SHIELD);
    bar(fb, 101, 105, 22, p->shield / p->shield_max, COL_SHIELD);
    craft_font_draw(fb, "H", 94, 109, COL_HULL);
    bar(fb, 101, 112, 22, p->hull / p->hull_max, COL_HULL);
    craft_font_draw(fb, "T", 94, 116, COL_HEAT);
    bar(fb, 101, 119, 22, p->heat / 100.0f, COL_HEAT);
    craft_font_draw(fb, "F", 94, 122, COL_NUM);
    bar(fb, 101, 124, 22, info->fuel01, COL_NUM);
    snprintf(buf, sizeof buf, "K%d", info->kills);
    craft_font_draw(fb, buf, 22, 117, COL_TEXT);

    scanner(fb);

    if (info->target >= 0 && g_ships[info->target].alive)
        target_box(fb, info->target);
    if (info->rail_charge01 > 0.0f) {
        /* Charge brackets close on the reticle; full = bright flash. */
        int g = (int)(8.0f - 5.0f * info->rail_charge01);
        uint16_t cc = (info->rail_charge01 >= 1.0f)
                          ? RGB565C(220, 250, 255)
                          : RGB565C(120, 180, 220);
        px(fb, 64 - g, 60, cc); px(fb, 64 + g, 60, cc);
        px(fb, 64, 60 - g, cc); px(fb, 64, 60 + g, cc);
        px(fb, 64 - g, 60 - 1, cc); px(fb, 64 + g, 60 - 1, cc);
        px(fb, 64 - g, 60 + 1, cc); px(fb, 64 + g, 60 + 1, cc);
    }
    if (info->incoming) {
        static uint8_t iflash;
        iflash++;
        if (iflash & 8) {
            const char *iw = "! INCOMING !";
            craft_font_draw(fb, iw, 64 - craft_font_width(iw) / 2, 18,
                            RGB565C(255, 70, 50));
        }
    }
    if (g_ships[PLAYER].sys_offline_t > 0.0f) {
        /* Scrambled: weapons dead — make sure the pilot knows why. */
        const char *sb = "SYSTEMS SCRAMBLED";
        craft_font_draw(fb, sb, 64 - craft_font_width(sb) / 2, 28,
                        RGB565C(120, 190, 255));
    }
    if (info->station_valid) {
        /* Station nav lock: cyan diamond + distance, edge arrow when
         * off screen. The station anchors the local frame at origin. */
        Ship *p = &g_ships[PLAYER];
        Vec3 rel = v3_scale(p->pos, -1.0f);
        float sx, sy;
        uint16_t d;
        uint16_t sc2 = RGB565C(90, 210, 255);
        if (r3d_pipe_project(rel, &sx, &sy, &d)) {
            int bx = (int)sx, by = (int)sy;
            px(fb, bx - 6, by, sc2); px(fb, bx + 6, by, sc2);
            px(fb, bx, by - 6, sc2); px(fb, bx, by + 6, sc2);
            px(fb, bx - 4, by - 4, sc2); px(fb, bx + 4, by - 4, sc2);
            px(fb, bx - 4, by + 4, sc2); px(fb, bx + 4, by + 4, sc2);
            char buf[16];
            snprintf(buf, sizeof buf, "STN %dM", (int)v3_len(rel));
            craft_font_draw(fb, buf, bx - 14, by + 9, sc2);
        } else {
            Vec3 v = m3_mul_v3_t(&p->basis, rel);
            float ax = v.x, ay = -v.y;
            float al = sqrtf(ax * ax + ay * ay);
            if (al < 1e-4f) { ax = 1; ay = 0; al = 1; }
            ax /= al; ay /= al;
            int ex = 64 + (int)(ax * 52.0f);
            int ey = 60 + (int)(ay * 44.0f);
            px(fb, ex, ey, sc2);
            px(fb, ex - (int)(ax * 2 + ay * 2), ey - (int)(ay * 2 - ax * 2), sc2);
            px(fb, ex - (int)(ax * 2 - ay * 2), ey - (int)(ay * 2 + ax * 2), sc2);
            px(fb, ex - (int)(ax * 3), ey - (int)(ay * 3), sc2);
        }
    }
    else if (info->loot_valid) {
        /* Salvage lock: gold brackets + distance, same edge-arrow cue. */
        Ship *p = &g_ships[PLAYER];
        float sx, sy;
        uint16_t d;
        Vec3 rel = v3_sub(info->loot_pos, p->pos);
        if (r3d_pipe_project(rel, &sx, &sy, &d) &&
            sx >= 6.0f && sx < 122.0f && sy >= 6.0f && sy < 116.0f) {
            int bx = (int)sx, by = (int)sy;
            uint16_t gc = RGB565C(255, 210, 70);
            for (int k = 0; k < 3; k++) {
                px(fb, bx - 5 + k, by - 5, gc); px(fb, bx + 5 - k, by - 5, gc);
                px(fb, bx - 5 + k, by + 5, gc); px(fb, bx + 5 - k, by + 5, gc);
                px(fb, bx - 5, by - 5 + k, gc); px(fb, bx - 5, by + 5 - k, gc);
                px(fb, bx + 5, by - 5 + k, gc); px(fb, bx + 5, by + 5 - k, gc);
            }
            char buf[16];
            snprintf(buf, sizeof buf, "%dM", (int)v3_len(rel));
            craft_font_draw(fb, buf, bx - 8, by + 8, gc);
        } else {
            Vec3 v = m3_mul_v3_t(&p->basis, rel);
            float ax = v.x, ay = -v.y;
            float al = sqrtf(ax * ax + ay * ay);
            if (al < 1e-4f) { ax = 1; ay = 0; al = 1; }
            ax /= al; ay /= al;
            int ex = 64 + (int)(ax * 52.0f);
            int ey = 60 + (int)(ay * 44.0f);
            uint16_t gc = RGB565C(255, 210, 70);
            px(fb, ex, ey, gc);
            px(fb, ex - (int)(ax * 2 + ay * 2), ey - (int)(ay * 2 - ax * 2), gc);
            px(fb, ex - (int)(ax * 2 - ay * 2), ey - (int)(ay * 2 + ax * 2), gc);
            px(fb, ex - (int)(ax * 3), ey - (int)(ay * 3), gc);
        }
    }
    else if (info->rock_valid) {
        /* Prospector lock: amber corner-ticks + distance + edge arrow —
         * the BELT! on the map, findable in the sky. */
        Ship *p = &g_ships[PLAYER];
        float sx, sy;
        uint16_t d;
        Vec3 rel = v3_sub(info->rock_pos, p->pos);
        uint16_t rc = RGB565C(220, 165, 90);
        /* project() is true for anything in FRONT — guard the frame
         * bounds or the brackets draw off-screen and clip silently. */
        if (r3d_pipe_project(rel, &sx, &sy, &d) &&
            sx >= 6.0f && sx < 122.0f && sy >= 6.0f && sy < 116.0f) {
            int bx = (int)sx, by = (int)sy;
            /* corner ticks only (visually distinct from salvage) */
            for (int k = 1; k <= 2; k++) {
                px(fb, bx - 5, by - 5 + k, rc); px(fb, bx - 5 + k, by - 5, rc);
                px(fb, bx + 5, by - 5 + k, rc); px(fb, bx + 5 - k, by - 5, rc);
                px(fb, bx - 5, by + 5 - k, rc); px(fb, bx - 5 + k, by + 5, rc);
                px(fb, bx + 5, by + 5 - k, rc); px(fb, bx + 5 - k, by + 5, rc);
            }
            char rbuf[16];
            snprintf(rbuf, sizeof rbuf, "ROCK %dM", (int)v3_len(rel));
            craft_font_draw(fb, rbuf, bx - 14, by + 8, rc);
        } else {
            Vec3 v = m3_mul_v3_t(&p->basis, rel);
            float ax = v.x, ay = -v.y;
            float al = sqrtf(ax * ax + ay * ay);
            if (al < 1e-4f) { ax = 1; ay = 0; al = 1; }
            ax /= al; ay /= al;
            int ex = 64 + (int)(ax * 52.0f);
            int ey = 60 + (int)(ay * 44.0f);
            px(fb, ex, ey, rc);
            px(fb, ex - (int)(ax * 2 + ay * 2), ey - (int)(ay * 2 - ax * 2), rc);
            px(fb, ex - (int)(ax * 2 - ay * 2), ey - (int)(ay * 2 + ax * 2), rc);
            px(fb, ex - (int)(ax * 3), ey - (int)(ay * 3), rc);
        }
    }

    if (info->show_perf) {
        snprintf(buf, sizeof buf, "%d.%dMS %dT",
                 (int)info->render_ms,
                 ((int)(info->render_ms * 10.0f)) % 10,
                 g_em->scene_tri_count());
        craft_font_draw(fb, buf, 40, 2, COL_TEXT);
    }
}

/* --- Supercruise HUD ---------------------------------------------------*/
void ui_hud_draw_sc(uint16_t *fb, const HudScInfo *info) {
    const Ship *p = &g_ships[PLAYER];
    char buf[32];

    dashboard(fb);

    /* Crosshair. */
    hline(fb, 59, 61, 64, COL_CROSS);
    hline(fb, 67, 69, 64, COL_CROSS);
    vline(fb, 64, 59, 61, COL_CROSS);
    vline(fb, 64, 67, 69, COL_CROSS);

    /* Destination marker: project the direction (clamped to an edge
     * arrow when off-screen), plus distance/alignment readout. */
    if (info->dest_name) {
        Vec3 rel_m = v3_scale(info->dest_rel_mm, 1.0e6f);
        float sx, sy;
        uint16_t d;
        if (r3d_pipe_project(rel_m, &sx, &sy, &d) &&
            sx >= 4 && sx < 124 && sy >= 12 && sy < 92) {
            int x = (int)sx, y = (int)sy;
            /* Diamond reticle. */
            for (int k = 3; k <= 5; k++) {
                px(fb, x + k, y, COL_CUR_DEST); px(fb, x - k, y, COL_CUR_DEST);
                px(fb, x, y + k, COL_CUR_DEST); px(fb, x, y - k, COL_CUR_DEST);
            }
        } else {
            Vec3 v = m3_mul_v3_t(&p->basis, rel_m);
            float ax = v.x, ay = -v.y;
            float al = sqrtf(ax * ax + ay * ay);
            if (al < 1e-4f) { ax = 1; ay = 0; al = 1; }
            ax /= al; ay /= al;
            int ex = 64 + (int)(ax * 52.0f), ey = 60 + (int)(ay * 44.0f);
            px(fb, ex, ey, COL_CUR_DEST);
            px(fb, ex - (int)(ax * 2 + ay * 2), ey - (int)(ay * 2 - ax * 2),
               COL_CUR_DEST);
            px(fb, ex - (int)(ax * 2 - ay * 2), ey - (int)(ay * 2 + ax * 2),
               COL_CUR_DEST);
            for (int k = 3; k <= 5; k++)
                px(fb, ex - (int)(ax * k), ey - (int)(ay * k), COL_CUR_DEST);
        }
        float dist = v3_len(info->dest_rel_mm);
        snprintf(buf, sizeof buf, "%s", info->dest_name);
        craft_font_draw(fb, buf, 2, 12, COL_CUR_DEST);
        if (dist >= 100.0f)
            snprintf(buf, sizeof buf, "%dMM", (int)dist);
        else
            snprintf(buf, sizeof buf, "%d.%dMM", (int)dist,
                     ((int)(dist * 10)) % 10);
        craft_font_draw(fb, buf, 2, 19, COL_NUM);
        if (info->eta_s > 0 && info->eta_s < 999) {
            snprintf(buf, sizeof buf, "ETA %dS", (int)info->eta_s);
            craft_font_draw(fb, buf, 2, 26, COL_NUM);
        }
    }

    craft_font_draw(fb, "SUPERCRUISE", 42, 110, COL_SHIELD);

    /* Left panel: speed (Mm/s) + throttle. (No chaff readout here —
     * nothing fires missiles at you in supercruise.) */
    craft_font_draw(fb, "SP", 2, 102, COL_TEXT);
    bar(fb, 13, 105, 20, info->speed_mms / 50.0f, COL_TEXT);
    craft_font_draw(fb, "TH", 2, 109, COL_NUM);
    bar(fb, 13, 112, 20, info->throttle, COL_NUM);
    if (info->speed_mms >= 1.0f)
        snprintf(buf, sizeof buf, "%dMM/S", (int)info->speed_mms);
    else
        snprintf(buf, sizeof buf, "%dKM/S", (int)(info->speed_mms * 1000.0f));
    craft_font_draw(fb, buf, 2, 118, COL_NUM);

    /* Right panel: fuel + hull (shields don't matter in SC). */
    craft_font_draw(fb, "H", 94, 102, COL_HULL);
    bar(fb, 101, 105, 22, p->hull / p->hull_max, COL_HULL);
    craft_font_draw(fb, "F", 94, 109, COL_NUM);
    bar(fb, 101, 112, 22, info->fuel01, COL_NUM);
    craft_font_draw(fb, "B:DROP", 94, 122, COL_NUM);

    if (info->show_perf) {
        snprintf(buf, sizeof buf, "%d.%dMS %dT",
                 (int)info->render_ms,
                 ((int)(info->render_ms * 10.0f)) % 10,
                 g_em->scene_tri_count());
        craft_font_draw(fb, buf, 40, 2, COL_TEXT);
    }
}
