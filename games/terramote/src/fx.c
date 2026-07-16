/* TerraMote — lighting, sky/parallax background, wall layer, liquid overlay,
 * particles and floating damage text. */
#include "terra.h"
#include <math.h>

#include "wall_dirt.tiles.h"
#include "wall_stone.tiles.h"
#include "wall_wood.tiles.h"
#include "wall_ebon.tiles.h"
#include "wall_ash.tiles.h"
#include "wall_snow.tiles.h"

static const MoteAutotile *k_walls[W_COUNT] = {
    0, &wall_dirt_at, &wall_stone_at, &wall_wood_at,
    &wall_ebon_at, &wall_ash_at, &wall_snow_at
};

Part  g_part[MAX_PART];
FText g_ftext[MAX_FTEXT];

/* ------------------------------------------------------------ light grid ----
 * A tile-light window around the camera, rebuilt every frame on core0.
 * Light 0..15. Sunlight seeds sky-exposed cells; emissive tiles/lava seed
 * themselves; 8 directional relax sweeps propagate with per-cell falloff
 * (1 through air, 3 through solids). The darkness pass samples it bilinearly. */
#define LW 44
#define LH 40
static uint8_t s_light[LW * LH];
static int s_lc0, s_lr0;                 /* window origin (tile coords) */

static inline int sun_level(void) {
    /* g_time: 0..0.60 day, 0.60..1 night; dawn/dusk ramps */
    float t = g_time;
    float s;
    if (t < 0.04f)       s = 0.35f + 0.65f * (t / 0.04f);          /* dawn */
    else if (t < 0.56f)  s = 1.0f;                                  /* day */
    else if (t < 0.62f)  s = 1.0f - 0.80f * ((t - 0.56f) / 0.06f);  /* dusk */
    else if (t < 0.96f)  s = 0.20f;                                 /* night (moon) */
    else                 s = 0.20f + 0.15f * ((t - 0.96f) / 0.04f);
    return (int)(s * 15.0f + 0.5f);
}

void fx_light_update(void) {
    s_lc0 = g_cam_x / TILE - (LW - 16) / 2;
    s_lr0 = g_cam_y / TILE - (LH - 16) / 2;
    int sun = sun_level();
    /* seed */
    for (int j = 0; j < LH; j++) {
        int r = s_lr0 + j;
        for (int i = 0; i < LW; i++) {
            int c = s_lc0 + i;
            int v = 0;
            if ((unsigned)c < WCOLS && (unsigned)r < WROWS) {
                uint8_t t = fg_at(c, r);
                uint8_t b = bg_at(c, r);
                v = g_tiles[t].light;
                if (BG_IS_LAVA(b) && BG_LIQ(b)) { int lv = 8 + BG_LIQ(b); if (lv > v) v = lv; }
                if (r < world_surface_row(c) && !BG_WALL(b)) { if (sun > v) v = sun; }
                else if (r <= world_surface_row(c) && r >= world_surface_row(c) - 1 && sun > v && !g_tiles[t].solid)
                    v = sun;                    /* the surface cell itself */
                if (r >= ROW_HELL - 6) { if (v < 3) v = 3; }   /* hell ambient glow */
            }
            s_light[j * LW + i] = (uint8_t)v;
        }
    }
    /* a faint glow around the player so unlit caves are scary, not unplayable */
    {
        int pi = px_c(g_pl.x) - s_lc0, pj = ((int)g_pl.y - 8) / TILE - s_lr0;
        if (pi >= 0 && pi < LW && pj >= 0 && pj < LH && s_light[pj * LW + pi] < 4)
            s_light[pj * LW + pi] = 4;
    }
    /* relax sweeps: 4 directions x 2 rounds */
    for (int round = 0; round < 3; round++) {
        for (int j = 0; j < LH; j++)                       /* left->right */
            for (int i = 1; i < LW; i++) {
                int c = s_lc0 + i, r = s_lr0 + j;
                int cost = ((unsigned)c < WCOLS && (unsigned)r < WROWS && g_tiles[fg_at(c, r)].solid == 1) ? 3 : 1;
                int v = s_light[j * LW + i - 1] - cost;
                if (v > s_light[j * LW + i]) s_light[j * LW + i] = (uint8_t)v;
            }
        for (int j = 0; j < LH; j++)                       /* right->left */
            for (int i = LW - 2; i >= 0; i--) {
                int c = s_lc0 + i, r = s_lr0 + j;
                int cost = ((unsigned)c < WCOLS && (unsigned)r < WROWS && g_tiles[fg_at(c, r)].solid == 1) ? 3 : 1;
                int v = s_light[j * LW + i + 1] - cost;
                if (v > s_light[j * LW + i]) s_light[j * LW + i] = (uint8_t)v;
            }
        for (int i = 0; i < LW; i++)                       /* top->bottom */
            for (int j = 1; j < LH; j++) {
                int c = s_lc0 + i, r = s_lr0 + j;
                int cost = ((unsigned)c < WCOLS && (unsigned)r < WROWS && g_tiles[fg_at(c, r)].solid == 1) ? 3 : 1;
                int v = s_light[(j - 1) * LW + i] - cost;
                if (v > s_light[j * LW + i]) s_light[j * LW + i] = (uint8_t)v;
            }
        for (int i = 0; i < LW; i++)                       /* bottom->top */
            for (int j = LH - 2; j >= 0; j--) {
                int c = s_lc0 + i, r = s_lr0 + j;
                int cost = ((unsigned)c < WCOLS && (unsigned)r < WROWS && g_tiles[fg_at(c, r)].solid == 1) ? 3 : 1;
                int v = s_light[(j + 1) * LW + i] - cost;
                if (v > s_light[j * LW + i]) s_light[j * LW + i] = (uint8_t)v;
            }
    }
}

uint8_t fx_light_at(int c, int r) {
    int i = c - s_lc0, j = r - s_lr0;
    if (i >= 0 && i < LW && j >= 0 && j < LH) return s_light[j * LW + i];
    if ((unsigned)c < WCOLS && r < world_surface_row(c)) return (uint8_t)sun_level();
    return 0;
}

/* --------------------------------------------------------------- sky ------- */
static uint16_t sky_col(int wy) {
    float t = g_time;
    /* day sky gradient; night: deep blue; dawn/dusk warm */
    float day;                       /* 1 = full day */
    if (t < 0.04f) day = t / 0.04f;
    else if (t < 0.56f) day = 1.0f;
    else if (t < 0.62f) day = 1.0f - (t - 0.56f) / 0.06f;
    else if (t < 0.96f) day = 0.0f;
    else day = (t - 0.96f) / 0.04f;
    float dusk = 0.0f;
    if (t > 0.52f && t < 0.66f) dusk = 1.0f - fabsf((t - 0.59f) / 0.07f);
    if (t < 0.06f) dusk = 1.0f - fabsf((t - 0.02f) / 0.045f);
    if (dusk < 0) dusk = 0;
    float depth = (float)wy / (float)(ROW_SURFACE_MAX * TILE);
    if (depth < 0) depth = 0; if (depth > 1) depth = 1;
    int rN = 10, gN = 12, bN = 40;                       /* night */
    int rD = 88 - (int)(28 * depth), gD = 160 - (int)(30 * depth), bD = 252 - (int)(30 * depth);
    int r = (int)(rN + (rD - rN) * day);
    int g = (int)(gN + (gD - gN) * day);
    int b = (int)(bN + (bD - bN) * day);
    r += (int)(dusk * 120 * (1.0f - depth * 0.6f));
    g += (int)(dusk * 30 * (1.0f - depth));
    if (r > 255) r = 255; if (g > 255) g = 255;
    return rgb(r, g, b);
}

static uint32_t phash(int x) { uint32_t h = (uint32_t)x * 2654435761u; return h ^ (h >> 15); }

/* parallax hill height (screen px from a virtual horizon) for layer k */
static int hill_h(int sx, int k) {
    float x = (g_cam_x * (k ? 0.18f : 0.38f) + sx) * (k ? 0.030f : 0.045f);
    int xi = (int)x; float f = x - xi; f = f * f * (3 - 2 * f);
    float a = (phash(xi * 2 + k * 977) & 0xFF) / 255.0f;
    float b = (phash((xi + 1) * 2 + k * 977) & 0xFF) / 255.0f;
    return (int)((a + (b - a) * f) * (k ? 26 : 38)) + (k ? 4 : 0);
}

void fx_background(uint16_t *fb, int y0, int y1) {
    /* per-column precompute: terrain surface + parallax hill lines (world px).
     * Hills anchor to the WORLD's highest terrain — a constant per world — so
     * they sit still like a horizon. (Anchoring to the highest terrain on
     * SCREEN made them bob up and down as the camera moved.) */
    int16_t srow_px[MOTE_FB_W], hill_far[MOTE_FB_W], hill_near[MOTE_FB_W];
    int hrow = WROWS;
    for (int c = 0; c < WCOLS; c++) {                  /* cached array walk: cheap */
        int s = world_surface_row(c);
        if (s < hrow) hrow = s;
    }
    int horizon = hrow * TILE;
    for (int x = 0; x < MOTE_FB_W; x++) {
        int c = (x + g_cam_x) / TILE;
        srow_px[x] = (int16_t)(world_surface_row(c) * TILE);
        hill_far[x]  = (int16_t)(horizon - 4 - hill_h(x, 0));
        hill_near[x] = (int16_t)(horizon - hill_h(x, 1));
    }
    int night = IS_NIGHT();
    uint16_t far_c  = night ? rgb(10, 20, 26) : rgb(52, 118, 84);
    uint16_t near_c = night ? rgb(14, 26, 20) : rgb(38, 92, 44);
    /* star density fades in as the sun goes down */
    float t = g_time;
    int stars = (t > 0.58f && t < 0.98f);
    for (int y = y0; y < y1; y++) {
        uint16_t *row = fb + y * MOTE_FB_W;
        int wy = y + g_cam_y;
        uint16_t sky = sky_col(wy);
        int rr = wy / TILE;
        uint16_t cavec;
        if (rr >= ROW_HELL - 4) cavec = rgb(52, 14, 10);
        else if (rr >= ROW_DIRT_END) cavec = rgb(18, 16, 20);
        else cavec = rgb(30, 20, 14);
        for (int x = 0; x < MOTE_FB_W; x++) {
            if (wy < srow_px[x]) {
                uint16_t col = sky;
                if (wy >= hill_near[x])     col = near_c;
                else if (wy >= hill_far[x]) col = far_c;
                else if (stars) {
                    /* sparse fixed starfield, gentle parallax */
                    uint32_t h = phash((((wy >> 1) & 0x7FFF) << 12) ^ ((x + (g_cam_x >> 3)) >> 1));
                    if ((h & 0x3FF) < 3) col = (h & 0x800) ? rgb(210, 214, 230) : rgb(150, 155, 180);
                }
                row[x] = col;
            } else {
                row[x] = cavec;
            }
        }
    }
    int sky_visible = horizon > g_cam_y;      /* any sky on screen? */
    /* drifting clouds (two parallax speeds), tinted by time of day */
    if (sky_visible) {
        uint16_t cl = night ? rgb(28, 34, 56) : rgb(236, 242, 250);
        uint16_t cd = night ? rgb(22, 27, 46) : rgb(206, 216, 232);
        for (int k = 0; k < 5; k++) {
            int span = MOTE_FB_W + 60;
            int drift = (int)(t * 2400.0f * (1.0f + (k & 1) * 0.6f));
            int sx = (int)((phash(k * 191) % 997) + drift - g_cam_x / (4 + (k & 1) * 3)) % span;
            if (sx < 0) sx += span;
            sx -= 30;
            int sy = 20 + (int)(phash(k * 47 + 5) % 26);
            mote->draw_circle(fb, sx, sy, 5, cl, 1, y0, y1);
            mote->draw_circle(fb, sx - 7, sy + 2, 4, cd, 1, y0, y1);
            mote->draw_circle(fb, sx + 7, sy + 2, 4, cd, 1, y0, y1);
        }
    }
    /* sun / moon disc — arc kept BELOW the hotbar (y >= ~22) so it's visible */
    if (sky_visible) {
        float ph = (t < 0.60f) ? (t / 0.60f) : ((t - 0.60f) / 0.40f);
        int sx = (int)(ph * (MOTE_FB_W + 40)) - 20;
        int sy = 46 - (int)(sinf(ph * 3.14159f) * 24.0f);
        if (t < 0.60f) {
            mote->draw_circle(fb, sx, sy, 7, rgb(255, 214, 80), 1, y0, y1);
            mote->draw_circle(fb, sx, sy, 5, rgb(255, 244, 160), 1, y0, y1);
        } else {
            mote->draw_circle(fb, sx, sy, 5, rgb(226, 230, 244), 1, y0, y1);
            mote->draw_circle(fb, sx - 2, sy - 1, 3, rgb(188, 194, 214), 1, y0, y1);  /* crescent shade */
        }
    }
    /* wall tiles (autotiled, art pre-darkened) */
    int r0 = g_cam_y / TILE, r1 = (g_cam_y + MOTE_FB_H - 1) / TILE;
    int c0 = g_cam_x / TILE, c1 = (g_cam_x + MOTE_FB_W - 1) / TILE;
    int band_r0 = (y0 + g_cam_y) / TILE, band_r1 = (y1 - 1 + g_cam_y) / TILE;
    if (band_r0 > r0) r0 = band_r0;
    if (band_r1 < r1) r1 = band_r1;
    for (int r = r0; r <= r1; r++) {
        if ((unsigned)r >= WROWS) continue;
        for (int c = c0; c <= c1; c++) {
            if ((unsigned)c >= WCOLS) continue;
            uint8_t w = BG_WALL(g_bgm[r * WCOLS + c]);
            if (!w || w >= W_COUNT) continue;
            uint8_t t = g_fgm[r * WCOLS + c];
            if (g_tiles[t].solid == 1 && t != T_DOOR_C) continue;   /* hidden */
            const MoteAutotile *at = k_walls[w];
            /* wall mask: any wall OR solid fg counts as connected (seamless) */
            int m = 0;
            static const int8_t nb[8][2] = { {0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1} };
            for (int k = 0; k < 8; k++) {
                int cc = c + nb[k][0], rr2 = r + nb[k][1];
                int same = 1;
                if ((unsigned)cc < WCOLS && (unsigned)rr2 < WROWS)
                    same = BG_WALL(g_bgm[rr2 * WCOLS + cc]) || g_tiles[g_fgm[rr2 * WCOLS + cc]].solid == 1;
                if (same) m |= 1 << k;
            }
            uint8_t cell = at->lut[m];
            int tpr = at->sheet->w / at->tile_w;
            mote->blit(fb, at->sheet, c * TILE - g_cam_x, r * TILE - g_cam_y,
                       (cell % tpr) * TILE, (cell / tpr) * TILE, TILE, TILE,
                       at->xform[m], y0, y1);
        }
    }
}

/* --------------------------------------------------- animated light tiles ----
 * The static autotiles stay; a small overlay re-blits the FLAME art frames on
 * top so torches flicker, furnaces pulse, and fireplaces shed embers. */
#include "Torch.h"      /* 4 frames, 8x8: f0 full torch, f1-3 flame wisps */
#include "Furnace.h"    /* 3 frames, 16x16: unlit / mid / lit */

void fx_draw_flames(uint16_t *fb) {
    float ft = g_time * DAY_SECONDS;              /* absolute in-day seconds */
    int r0 = g_cam_y / TILE, r1 = (g_cam_y + MOTE_FB_H - 1) / TILE;
    int c0 = g_cam_x / TILE, c1 = (g_cam_x + MOTE_FB_W - 1) / TILE;
    int tf = 1 + ((int)(ft * 7.0f) % 3);          /* torch wisp frame 1..3 */
    int ff = 1 + ((int)(ft * 4.0f) & 1);          /* furnace fire frame 1..2 */
    for (int r = r0; r <= r1; r++) {
        if ((unsigned)r >= WROWS) continue;
        for (int c = c0; c <= c1; c++) {
            if ((unsigned)c >= WCOLS) continue;
            uint8_t t = g_fgm[r * WCOLS + c];
            int sx = c * TILE - g_cam_x, sy = r * TILE - g_cam_y;
            if (t == T_TORCH) {
                mote->blit(fb, &Torch_img, sx, sy, tf * 8, 0, 8, 8, 0, 0, MOTE_FB_H);
            } else if (t == T_FURNACE) {
                /* anchor = top-left of the 2x2 */
                if (fg_at(c - 1, r) != T_FURNACE && fg_at(c, r - 1) != T_FURNACE)
                    mote->blit(fb, &Furnace_img, sx, sy, ff * 16, 0, 16, 16, 0, 0, MOTE_FB_H);
            } else if (t == T_FIREPLACE) {
                /* anchor = top-left of the 3x2: embers rise from the hearth */
                if (fg_at(c - 1, r) != T_FIREPLACE && fg_at(c, r - 1) != T_FIREPLACE &&
                    (mote_rand() % 9) == 0)
                    part_burst(c * TILE + 12, r * TILE + 10,
                               (mote_rand() & 1) ? rgb(255, 170, 50) : rgb(255, 120, 30), 1, 14);
            }
        }
    }
}

/* ------------------------------------------------- liquids + darkness ------- */
static inline uint16_t blend565(uint16_t a, uint16_t b) {
    return (uint16_t)((((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1)));
}

void fx_overlay_world(uint16_t *fb) {
    /* liquid translucency: per visible cell, tint the filled part */
    int r0 = g_cam_y / TILE, r1 = (g_cam_y + MOTE_FB_H - 1) / TILE;
    int c0 = g_cam_x / TILE, c1 = (g_cam_x + MOTE_FB_W - 1) / TILE;
    for (int r = r0; r <= r1; r++) {
        if ((unsigned)r >= WROWS) continue;
        for (int c = c0; c <= c1; c++) {
            if ((unsigned)c >= WCOLS) continue;
            uint8_t b = g_bgm[r * WCOLS + c];
            int lv = BG_LIQ(b);
            if (!lv) continue;
            int lava = BG_IS_LAVA(b);
            /* full cell if the cell above also holds liquid */
            int above = (r > 0) ? BG_LIQ(g_bgm[(r - 1) * WCOLS + c]) : 0;
            int hpx = above ? TILE : (lv + 1);
            if (hpx > TILE) hpx = TILE;
            int sx0 = c * TILE - g_cam_x, sy0 = r * TILE - g_cam_y + (TILE - hpx);
            uint16_t tint = lava ? rgb(255, 110, 20) : rgb(40, 90, 220);
            uint16_t surf = lava ? rgb(255, 200, 70) : rgb(120, 170, 255);
            for (int y = sy0; y < sy0 + hpx; y++) {
                if ((unsigned)y >= MOTE_FB_H) continue;
                uint16_t *row = fb + y * MOTE_FB_W;
                for (int x = sx0; x < sx0 + TILE; x++) {
                    if ((unsigned)x >= MOTE_FB_W) continue;
                    uint16_t px = blend565(row[x], tint);
                    if (lava) px = blend565(px, tint);          /* lava more opaque */
                    if (y == sy0 && !above) px = blend565(px, surf);
                    row[x] = px;
                }
            }
        }
    }

    /* darkness: bilinear per-pixel from the tile-light window */
    for (int y = 0; y < MOTE_FB_H; y++) {
        int wy = y + g_cam_y;
        int fy = wy - TILE / 2;
        int j = fy / TILE - s_lr0;   /* upper sample row */
        int wy_frac = fy & (TILE - 1);
        if (j < 0) { j = 0; wy_frac = 0; }
        if (j >= LH - 1) { j = LH - 2; wy_frac = TILE - 1; }
        const uint8_t *rowa = &s_light[j * LW];
        const uint8_t *rowb = rowa + LW;
        uint16_t *row = fb + y * MOTE_FB_W;
        for (int x = 0; x < MOTE_FB_W; x++) {
            int wx = x + g_cam_x;
            int fx = wx - TILE / 2;
            int i = fx / TILE - s_lc0;
            int wx_frac = fx & (TILE - 1);
            if (i < 0) { i = 0; wx_frac = 0; }
            if (i >= LW - 1) { i = LW - 2; wx_frac = TILE - 1; }
            /* bilinear in 1/8ths of a tile -> light 0..15 scaled to 0..60 */
            int la = rowa[i] * (TILE - wx_frac) + rowa[i + 1] * wx_frac;
            int lb = rowb[i] * (TILE - wx_frac) + rowb[i + 1] * wx_frac;
            int l = (la * (TILE - wy_frac) + lb * wy_frac);   /* 0 .. 15*64 */
            int s = (l * 3) >> 6;                              /* full bright at ~10+ */
            if (s >= 31) continue;                             /* fully lit */
            if (s <= 0) { row[x] = 0; continue; }
            uint16_t px = row[x];
            int r5 = ((px >> 11) & 31) * s >> 5;
            int g6 = ((px >> 5) & 63) * s >> 5;
            int b5 = (px & 31) * s >> 5;
            row[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

/* ------------------------------------------------------------- particles ---- */
void part_burst(float x, float y, uint16_t col, int n, float speed) {
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < MAX_PART; i++) {
            if (g_part[i].t > 0) continue;
            float a = mote_randf(0, 6.28318f);
            float v = mote_randf(0.3f, 1.0f) * speed;
            float life = mote_randf(0.25f, 0.6f);
            g_part[i] = (Part){ x, y, cosf(a) * v, sinf(a) * v - speed * 0.4f, life, life, col, PFX_BURST };
            break;
        }
    }
}

/* a single spark with explicit velocity/lifetime/behaviour — the building
 * block of weapon swing trails and elemental effects. */
void part_spark(float x, float y, float vx, float vy, float life, uint16_t col, int fx_mode) {
    for (int i = 0; i < MAX_PART; i++) {
        if (g_part[i].t > 0) continue;
        g_part[i] = (Part){ x, y, vx, vy, life, life, col, (uint8_t)fx_mode };
        return;
    }
}

int element_pfx(uint8_t el) {
    switch (el) {
    case EL_FIRE:    return PFX_EMBER;
    case EL_ICE:     return PFX_CRYSTAL;
    case EL_POISON:  return PFX_BUBBLE;
    case EL_HOLY:    return PFX_HOLY;
    case EL_DEMONIC: case EL_ARCANE: return PFX_SWIRL;
    case EL_BLOOD:   return PFX_DROP;
    case EL_NATURE:  return PFX_LEAF;
    }
    return PFX_TRAIL;
}

/* radial burst whose particles BEHAVE like the element (on-hit flare, arrow trail) */
void part_element(float x, float y, uint8_t el, int n, float speed) {
    int mode = element_pfx(el);
    uint16_t col = element_color(el);
    for (int k = 0; k < n; k++) {
        float a = mote_randf(0, 6.28318f);
        float v = mote_randf(0.4f, 1.0f) * speed;
        part_spark(x, y, cosf(a) * v, sinf(a) * v, mote_randf(0.30f, 0.55f), col, mode);
    }
}
void parts_tick(float dt);
void parts_tick(float dt) {
    for (int i = 0; i < MAX_PART; i++) {
        Part *p = &g_part[i];
        if (p->t <= 0) continue;
        p->t -= dt;
        float age = p->t0 - p->t;
        switch (p->fx) {
        case PFX_BURST:   p->vy += 300.0f * dt; break;
        case PFX_TRAIL:   p->vx *= 0.90f; p->vy *= 0.90f; break;   /* smear drags to a stop */
        case PFX_EMBER:   p->vy -= 260.0f * dt;                     /* embers rise... */
                          p->vx += mote_randf(-160, 160) * dt;      /* ...and flicker sideways */
                          p->vx *= 0.93f; break;
        case PFX_CRYSTAL: p->vy += 90.0f * dt;                      /* shards settle gently */
                          if (p->vy > 24) p->vy = 24;
                          p->vx *= 0.92f; break;
        case PFX_BUBBLE:  p->vx = sinf(age * 13.0f + (float)i * 2.1f) * 20.0f;  /* wobble */
                          p->vy -= 70.0f * dt; break;               /* bubbling upward */
        case PFX_HOLY:    p->vy -= 120.0f * dt; p->vx *= 0.96f; break;  /* serene rise */
        case PFX_SWIRL: { float w = 8.5f * dt;                      /* velocity curls -> spirals */
                          float s = sinf(w), c0 = cosf(w);
                          float nvx = p->vx * c0 - p->vy * s;
                          p->vy = p->vx * s + p->vy * c0; p->vx = nvx; } break;
        case PFX_DROP:    p->vy += 520.0f * dt; break;              /* heavy droplets */
        case PFX_LEAF:    p->vx = sinf(age * 8.0f + (float)i) * 24.0f;  /* flutter */
                          p->vy = 18.0f; break;
        }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if ((p->fx == PFX_BURST || p->fx == PFX_DROP) &&
            world_solid_px((int)p->x, (int)p->y)) p->t = 0;
    }
    for (int i = 0; i < MAX_FTEXT; i++)
        if (g_ftext[i].t > 0) { g_ftext[i].t -= dt; g_ftext[i].y -= 14.0f * dt; }
}

void ftext_add(float x, float y, int val, uint16_t col) {
    int best = 0; float bt = 1e9f;
    for (int i = 0; i < MAX_FTEXT; i++) {
        if (g_ftext[i].t <= 0) { best = i; break; }
        if (g_ftext[i].t < bt) { bt = g_ftext[i].t; best = i; }
    }
    g_ftext[best] = (FText){ x, y, 0.9f, (int16_t)val, col };
}

/* particles + damage text draw in screen space AFTER darkness (they read as
 * sparks/UI); called from the overlay path in game.c */
void fx_draw_particles(uint16_t *fb);
void fx_draw_particles(uint16_t *fb) {
    for (int i = 0; i < MAX_PART; i++) {
        Part *p = &g_part[i];
        if (p->t <= 0) continue;
        int sx = (int)p->x - g_cam_x, sy = (int)p->y - g_cam_y;
        uint16_t c = p->col;
        float f = p->t0 > 0 ? p->t / p->t0 : 1.0f;      /* 1 fresh .. 0 dying */
        int tw = ((int)(p->t * 24.0f) + i) & 3;          /* cheap twinkle phase */
        switch (p->fx) {
        case PFX_EMBER:                                   /* hot -> dark colour ramp */
            c = f > 0.66f ? rgb(255, 244, 190) : f > 0.33f ? rgb(255, 150, 40) : rgb(160, 48, 18);
            break;
        case PFX_CRYSTAL: if (tw == 0) c = rgb(235, 250, 255); break;   /* glitter */
        case PFX_HOLY:    if (tw & 1)  c = rgb(255, 255, 255); break;   /* shimmer */
        case PFX_SWIRL:   if (tw == 0) c = rgb(255, 235, 255); break;   /* magic flicker */
        default: break;
        }
        /* fade toward black over the particle's life so trails taper to a tail */
        if (f < 0.55f && p->fx != PFX_EMBER) {
            int s = (int)(f * 1.8f * 32.0f); if (s > 31) s = 31;
            c = (uint16_t)(((((c >> 11) & 31) * s >> 5) << 11) |
                           ((((c >> 5) & 63) * s >> 5) << 5) | ((c & 31) * s >> 5));
        }
        mote->draw_pixel(fb, sx, sy, c);
    }
    for (int i = 0; i < MAX_FTEXT; i++) {
        if (g_ftext[i].t <= 0) continue;
        char buf[8];
        mote_itoa(g_ftext[i].val, buf);
        mote->text(fb, buf, (int)g_ftext[i].x - g_cam_x - 4, (int)g_ftext[i].y - g_cam_y, g_ftext[i].col);
    }
}

void fx_init(void) {
    for (int i = 0; i < MAX_PART; i++) g_part[i].t = 0;
    for (int i = 0; i < MAX_FTEXT; i++) g_ftext[i].t = 0;
}
