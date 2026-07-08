/*
 * ThumbyElite — detail sheets.
 */
#include "ui_detail.h"
#include "elite_types.h"
#include "elite_ships.h"
#include "elite_weapons.h"
#include "ui_icons.h"
#include "econ.h"
#include "craft_font.h"
#include "elite_ui.h"
#include <stdio.h>

#define COL_BG    RGB565C(  6,  10,  20)
#define COL_HDR   RGB565C(200, 210, 225)
#define COL_GRID  RGB565C( 28,  40,  58)
#define COL_DIM   RGB565C(110, 116, 135)
#define COL_VAL   RGB565C(120, 255, 120)
#define COL_CRED  RGB565C(255, 200,  60)
#define COL_WARN  RGB565C(255, 120,  70)
#define COL_ILL   RGB565C(220, 100, 200)

static void fill(uint16_t *fb, uint16_t c) {
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++) fb[i] = c;
}
static void hl(uint16_t *fb, int y, uint16_t c) {
    for (int x = 0; x < ELITE_FB_W; x++) fb[y * ELITE_FB_W + x] = c;
}
static void stat(uint16_t *fb, int y, const char *k, const char *v,
                 uint16_t vc) {
    craft_font_draw(fb, k, 4, y, COL_DIM);
    craft_font_draw(fb, v, 60, y, vc);
}

/* Delta tag beside a stat: green when better, red when worse.
 * lower_better flips the sense (heat). */
static void stat_delta(uint16_t *fb, int y, float delta, int lower_better) {
    if (delta > -0.05f && delta < 0.05f) {
        craft_font_draw(fb, "=", 104, y, COL_DIM);
        return;
    }
    char buf[12];
    int better = lower_better ? (delta < 0) : (delta > 0);
    if (delta >= 10.0f || delta <= -10.0f)
        snprintf(buf, sizeof buf, "%+d", (int)delta);
    else
        snprintf(buf, sizeof buf, "%+d.%d", (int)delta,
                 (int)(delta < 0 ? -delta * 10 : delta * 10) % 10);
    craft_font_draw(fb, buf, 96, y, better ? COL_VAL : COL_WARN);
}

static const char *k_qual_long[5] = {
    "SALVAGED", "STANDARD", "REINFORCED", "MILITARY", "PROTOTYPE",
};

void detail_draw_weapon(uint16_t *fb, const WeaponInst *wi,
                        const WeaponInst *cmp,
                        int price, const char *price_label,
                        const char *footer) {
    fill(fb, COL_BG);
    char buf[28];

    if (wi->type >= EQ_HEATSINK && wi->type < ITEM_COUNT) {
        /* Gadget sheet: what it does, in one line. */
        static const char *k_fx1[6] = {
            "-25% WEAPON HEAT", "RADAR 400>700M", "BOOST 2.2>4.0S",
            "SKIM STARS FOR FUEL", "+40% SEEKERS, LEAD PIP",
            "LB+B BREAKS LOCKS",
        };
        icon_weapon_2x(fb, 4, 3, wi->type);
        craft_font_draw(fb, item_name(wi->type), 32, 4, COL_HDR);
        craft_font_draw(fb, "UTILITY GADGET", 32, 11, COL_DIM);
        hl(fb, 19, COL_GRID);
        int y = 26;
        craft_font_draw(fb, k_fx1[wi->type - EQ_HEATSINK], 4, y, COL_VAL);
        y += 10;
        if (wi->type == EQ_CHAFF) {
            craft_font_draw(fb, "4 CHARGES, RESTOCK", 4, y, COL_DIM);
            y += 8;
            craft_font_draw(fb, "AT REARM (20CR EA)", 4, y, COL_DIM);
            y += 10;
        }
        if (wi->type == EQ_FUELSCOOP) {
            craft_font_draw(fb, "HEAT BUILDS - WATCH T", 4, y, COL_WARN);
            y += 10;
        }
        char ibuf[16];
        snprintf(ibuf, sizeof ibuf, "%d%%", wi->integrity);
        stat(fb, y, "INTEGRITY", ibuf,
             wi->integrity < 60 ? COL_WARN : COL_VAL);
        y += 8;
        if (price >= 0) {
            hl(fb, y + 1, COL_GRID);
            snprintf(ibuf, sizeof ibuf, "%s %dCR", price_label, price);
            craft_font_draw(fb, ibuf, 4, y + 5, COL_CRED);
        }
        hl(fb, 118, COL_GRID);
        craft_font_draw(fb, footer, 2, 121, COL_DIM);
        return;
    }

    if (wi->type >= WPN_COUNT) {
        /* Equipment sheet: protection rather than firepower. */
        icon_weapon_2x(fb, 4, 3, wi->type);
        craft_font_draw(fb, item_name(wi->type), 32, 4, COL_HDR);
        craft_font_draw(fb, k_qual_long[wi->quality > 4 ? 4 : wi->quality],
                        32, 11,
                        (wi->quality >= Q_MILITARY) ? COL_CRED : COL_DIM);
        hl(fb, 19, COL_GRID);
        int y = 24;
        if (wi->affix) {
            static const char *k_shv_fx[4] = { "", "FAST REGEN, -CAP",
                                               "+50% CAP, SLOW REGEN",
                                               "15% HITS PASS THRU" };
            static const char *k_arv_fx[4] = { "", "-50% BLAST DAMAGE",
                                               "+35% HP, FAST WEAR",
                                               "-15% HP, +8% SPD/TRN" };
            const char *vn = (wi->type == EQ_ARMOR)
                                 ? k_armor_var_names[wi->affix & 3]
                                 : k_shield_var_names[wi->affix & 3];
            const char *fx2 = (wi->type == EQ_ARMOR)
                                  ? k_arv_fx[wi->affix & 3]
                                  : k_shv_fx[wi->affix & 3];
            craft_font_draw(fb, vn, 4, y, RGB565C(150, 220, 255));
            y += 8;
            craft_font_draw(fb, fx2, 4, y, COL_DIM);
            y += 9;
        }
        snprintf(buf, sizeof buf, "Z%d", wi->tier);
        stat(fb, y, "SIZE", buf, COL_VAL); y += 8;
        float mult = k_tier_mult[wi->tier > 3 ? 3 : wi->tier] *
                     quality_dmg_mult(wi->quality) *
                     (0.6f + 0.4f * wi->integrity * 0.01f);
        snprintf(buf, sizeof buf, "X%d.%d", (int)mult,
                 ((int)(mult * 10)) % 10);
        stat(fb, y, "PROTECTION", buf, COL_VAL);
        if (cmp && cmp->in_use && cmp->type == wi->type && cmp != wi) {
            float cm = k_tier_mult[cmp->tier > 3 ? 3 : cmp->tier] *
                       quality_dmg_mult(cmp->quality) *
                       (0.6f + 0.4f * cmp->integrity * 0.01f);
            stat_delta(fb, y, mult - cm, 0);
        }
        y += 8;
        snprintf(buf, sizeof buf, "%d%%", wi->integrity);
        stat(fb, y, "INTEGRITY", buf,
             wi->integrity < 60 ? COL_WARN : COL_VAL); y += 8;
        if (price >= 0) {
            hl(fb, y + 1, COL_GRID);
            snprintf(buf, sizeof buf, "%s %dCR", price_label, price);
            craft_font_draw(fb, buf, 4, y + 5, COL_CRED);
        }
        hl(fb, 118, COL_GRID);
        craft_font_draw(fb, footer, 2, 121, COL_DIM);
        return;
    }

    const WeaponDef *w = &k_weapons[wi->type];
    /* Comparator stats (effective). */
    const WeaponDef *cw = NULL;
    float cdm = 0;
    if (cmp && cmp->in_use && cmp->type < WPN_COUNT && cmp != wi) {
        cw = &k_weapons[cmp->type];
        cdm = mount_dmg_mult(cmp);
    }

    icon_weapon_2x(fb, 4, 3, wi->type);
    /* Affixed weapons show "VENTED GAUSS" as the headline. */
    if (wi->affix && wi->affix < AFX_COUNT) {
        char nbuf[26];
        snprintf(nbuf, sizeof nbuf, "%.11s %s", k_affixes[wi->affix].name,
                 w->name);
        craft_font_draw(fb, nbuf, 32, 4, RGB565C(150, 220, 255));
    } else {
        craft_font_draw(fb, w->name, 32, 4, COL_HDR);
    }
    craft_font_draw(fb, k_qual_long[wi->quality > 4 ? 4 : wi->quality],
                    32, 11, (wi->quality >= Q_MILITARY) ? COL_CRED : COL_DIM);
    if (cw) {
        /* Comparison benchmark as a two-row header tag, top right —
         * the deltas below are measured against this. */
        int tw = craft_font_width("VS");
        craft_font_draw(fb, "VS", 126 - tw, 4, COL_DIM);
        tw = craft_font_width(cw->name);
        craft_font_draw(fb, cw->name, 126 - tw, 11,
                        RGB565C(90, 170, 220));
    }
    hl(fb, 19, COL_GRID);

    float dm = mount_dmg_mult(wi);
    int y = 24;
    snprintf(buf, sizeof buf, "Z%d", w->size);
    stat(fb, y, "SLOT SIZE", buf, COL_VAL); y += 8;
    snprintf(buf, sizeof buf, "%d.%d", (int)(w->dmg * dm),
             ((int)(w->dmg * dm * 10)) % 10);
    stat(fb, y, "DAMAGE", buf, COL_VAL);
    if (cw) stat_delta(fb, y, w->dmg * dm - cw->dmg * cdm, 0);
    y += 8;
    float dps = w->dmg * dm / w->cooldown;
    snprintf(buf, sizeof buf, "%d.%d", (int)dps, ((int)(dps * 10)) % 10);
    stat(fb, y, "DPS", buf, COL_VAL);
    if (cw) stat_delta(fb, y, dps - cw->dmg * cdm / cw->cooldown, 0);
    y += 8;
    snprintf(buf, sizeof buf, "%d.%d/S", (int)(w->heat / w->cooldown),
             ((int)(w->heat / w->cooldown * 10)) % 10);
    stat(fb, y, "HEAT", buf,
         (w->heat / w->cooldown > 30) ? COL_WARN : COL_VAL);
    if (cw) stat_delta(fb, y, w->heat / w->cooldown -
                                  cw->heat / cw->cooldown, 1);
    y += 8;
    if (w->speed > 0)
        snprintf(buf, sizeof buf, "%dM/S", (int)w->speed);
    else
        snprintf(buf, sizeof buf, "HITSCAN");
    stat(fb, y, "VELOCITY", buf, COL_VAL); y += 8;
    snprintf(buf, sizeof buf, "%dM", (int)w->range);
    stat(fb, y, "RANGE", buf, COL_VAL);
    if (cw) stat_delta(fb, y, w->range - cw->range, 0);
    y += 8;
    if (w->ammo_max)
        snprintf(buf, sizeof buf, "%d RNDS", w->ammo_max);
    else
        snprintf(buf, sizeof buf, "ENERGY");
    stat(fb, y, "AMMO", buf, COL_VAL); y += 8;
    if (w->aoe > 0) {
        snprintf(buf, sizeof buf, "%dM BLAST", (int)w->aoe);
        stat(fb, y, "WARHEAD", buf, COL_WARN); y += 8;
    }
    if (w->turn > 0) {
        stat(fb, y, "GUIDANCE", "SEEKER", COL_ILL); y += 8;
    }
    snprintf(buf, sizeof buf, "%d%%", wi->integrity);
    stat(fb, y, "INTEGRITY", buf,
         wi->integrity < 60 ? COL_WARN : COL_VAL); y += 8;

    if (price >= 0) {
        hl(fb, y + 1, COL_GRID);
        snprintf(buf, sizeof buf, "%s %dCR", price_label, price);
        craft_font_draw(fb, buf, 4, y + 5, COL_CRED);
    }

    hl(fb, 118, COL_GRID);
    craft_font_draw(fb, footer, 2, 121, COL_DIM);
}

/* Margin-scaled comparison colour: bright green >= +30%, soft green
 * +5..30%, grey within 5%, orange -5..-30%, red <= -30%. */
static uint16_t cmp_col(float nv, float cv) {
    if (cv <= 0.0001f) return RGB565C(150, 156, 170);
    float d = (nv - cv) / cv;
    if (d >= 0.30f) return RGB565C(70, 255, 90);
    if (d >= 0.05f) return RGB565C(140, 215, 125);
    if (d > -0.05f) return RGB565C(150, 156, 170);   /* match = grey */
    if (d > -0.30f) return RGB565C(235, 150, 70);
    return RGB565C(255, 75, 60);
}

void detail_draw_hull(uint16_t *fb, int hull_id, uint32_t seed, int cost,
                      const char *footer) {
    /* Left column only — the shipyard's rotating 3D pane stays live. */
    for (int y = 0; y < ELITE_FB_H; y++) {
        int xmax = (y >= 10 && y < 95) ? 64 : ELITE_FB_W;
        uint16_t *row = fb + y * ELITE_FB_W;
        for (int x = 0; x < xmax; x++) row[x] = COL_BG;
    }
    for (int y = 10; y < 95; y++) fb[y * ELITE_FB_W + 64] = COL_GRID;

    const HullDef *h = &k_hulls[hull_id];
    const HullDef *cur = &k_hulls[g_player.hull_id];
    /* This INSTANCE's roll vs YOUR ship's roll — shopping is reading
     * the quirks, not the brochure. */
    HullRoll rv;
    hull_roll(hull_id, seed, &rv);
    const HullRoll *rc = player_roll();
    char buf[28];
    craft_font_draw(fb, h->name, 2, 2, COL_HDR);
    hl(fb, 9, COL_GRID);

    int y = 13;
    /* Stat colour vs YOUR ship (user spec: colour only, no deltas —
     * green better / red worse on a margin scale, grey = matching). */
    #define CMPC(nv, cv) cmp_col((float)(nv), (float)(cv))
    #define HSTATC(label, col, fmt, ...) do { \
        craft_font_draw(fb, label, 2, y, COL_DIM); \
        snprintf(buf, sizeof buf, fmt, __VA_ARGS__); \
        craft_font_draw(fb, buf, 34, y, col); \
        y += 8; \
    } while (0)
    HSTATC("SPD", CMPC(h->max_speed * rv.spd,
                       cur->max_speed * rc->spd), "%d",
           (int)(h->max_speed * rv.spd));
    HSTATC("ACC", CMPC(h->accel * rv.acc, cur->accel * rc->acc), "%d",
           (int)(h->accel * rv.acc));
    {
        float tn = h->turn_rate * rv.trn, tc = cur->turn_rate * rc->trn;
        HSTATC("TRN", CMPC(tn, tc), "%d.%d", (int)tn,
               ((int)(tn * 10)) % 10);
    }
    HSTATC("CRG", CMPC(rv.cargo, rc->cargo), "%dT", rv.cargo);
    {
        float jn = h->jump_range * rv.jmp;
        HSTATC("JMP", CMPC(jn, cur->jump_range * rc->jmp), "%d.%dLY",
               (int)jn, ((int)(jn * 10)) % 10);
    }
    HSTATC("HUL", CMPC(h->hull_base * rv.hull,
                       cur->hull_base * rc->hull), "%d",
           (int)(h->hull_base * rv.hull));
    HSTATC("SHD", CMPC(h->shield_base * rv.shd,
                       cur->shield_base * rc->shd), "%d",
           (int)(h->shield_base * rv.shd));
    /* TIER + GUNS: colour compares TOTALS across slots, but the text
     * stays in its original form (user spec). */
    {
        int nt = h->max_shield_tier + h->max_hull_tier;
        int ct = cur->max_shield_tier + cur->max_hull_tier;
        HSTATC("TIER", CMPC(nt, ct), "S%d H%d",
               h->max_shield_tier, h->max_hull_tier);
        int ng = 0, cg = 0;
        for (int i = 0; i < rv.n_slots; i++) ng += rv.slot_size[i];
        for (int i = 0; i < rc->n_slots; i++) cg += rc->slot_size[i];
        char slots[14];
        int sl = 0;
        for (int i = 0; i < rv.n_slots; i++) {
            slots[sl++] = 'Z';
            slots[sl++] = (char)('0' + rv.slot_size[i]);
            slots[sl++] = ' ';
        }
        slots[sl] = 0;
        HSTATC("GUNS", CMPC(ng, cg), "%s", slots);
        HSTATC("UTIL", CMPC(rv.utils, rc->utils), "%d BAY%s",
               rv.utils, rv.utils == 1 ? "" : "S");
    }
    #undef HSTATC
    #undef CMPC

    hl(fb, 95, COL_GRID);
    if (cost == DETAIL_OWNED)
        craft_font_draw(fb, "OWNED", 2, 99, COL_CRED);
    else if (cost < 0) {           /* trade-down: difference refunded */
        snprintf(buf, sizeof buf, "GET %d CR (TRADE-IN)", -cost);
        craft_font_draw(fb, buf, 2, 99, COL_CRED);
    } else {
        snprintf(buf, sizeof buf, "COST %d CR (TRADE-IN)", cost);
        craft_font_draw(fb, buf, 2, 99, COL_CRED);
    }
    snprintf(buf, sizeof buf, "LIST %d CR", h->price);
    craft_font_draw(fb, buf, 2, 107, COL_DIM);
    hl(fb, 118, COL_GRID);
    craft_font_draw(fb, footer, 2, 121, COL_DIM);
}

void detail_draw_good(uint16_t *fb, int good, int held,
                      const char *footer) {
    fill(fb, COL_BG);
    const GoodDef *g = &k_goods[good];
    char buf[28];
    craft_font_draw(fb, g->name, 4, 4, COL_HDR);
    if (g->flags & GOOD_ILLEGAL)
        craft_font_draw(fb, "ILLEGAL", 90, 4, COL_ILL);
    hl(fb, 12, COL_GRID);
    int y = 18;
    snprintf(buf, sizeof buf, "%d UNITS HELD", held);
    craft_font_draw(fb, buf, 4, y, COL_VAL); y += 9;
    snprintf(buf, sizeof buf, "GALACTIC AVG %dCR", g->base);
    craft_font_draw(fb, buf, 4, y, COL_DIM); y += 9;
    if (g->flags & GOOD_ILLEGAL) {
        craft_font_draw(fb, "ONLY BLACK MARKETS", 4, y, COL_ILL); y += 7;
        craft_font_draw(fb, "WILL TOUCH THIS", 4, y, COL_ILL); y += 9;
    } else {
        craft_font_draw(fb, "SELL WHERE THE", 4, y, COL_DIM); y += 7;
        craft_font_draw(fb, "ECONOMY IMPORTS IT", 4, y, COL_DIM); y += 9;
    }
    hl(fb, 118, COL_GRID);
    craft_font_draw(fb, footer, 2, 121, COL_DIM);
}
