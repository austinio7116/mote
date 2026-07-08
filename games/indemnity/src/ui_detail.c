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
/* Readable key/value row for the roomy sheets (the weapon sheet keeps the
   compact craft stat() above — it packs up to ten numeric rows and would
   overflow the screen in the taller font). */
static void stat_r(uint16_t *fb, int y, const char *k, const char *v,
                   uint16_t vc) {
    eui_textclip(fb, k, 4, 58, y, COL_DIM);
    eui_text(fb, v, 62, y, vc);
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
        eui_textclip(fb, item_name(wi->type), 32, 124, 2, COL_HDR);
        craft_font_draw(fb, "UTILITY GADGET", 32, 13, COL_DIM);
        hl(fb, 21, COL_GRID);
        int y = 25;
        eui_text(fb, k_fx1[wi->type - EQ_HEATSINK], 4, y, COL_VAL);
        y += 13;
        if (wi->type == EQ_CHAFF) {
            eui_text(fb, "4 CHARGES, RESTOCK", 4, y, COL_DIM);
            y += 12;
            eui_text(fb, "AT REARM (20CR EA)", 4, y, COL_DIM);
            y += 13;
        }
        if (wi->type == EQ_FUELSCOOP) {
            eui_text(fb, "HEAT BUILDS - WATCH T", 4, y, COL_WARN);
            y += 13;
        }
        char ibuf[16];
        snprintf(ibuf, sizeof ibuf, "%d%%", wi->integrity);
        stat_r(fb, y, "INTEGRITY", ibuf,
               wi->integrity < 60 ? COL_WARN : COL_VAL);
        y += 13;
        if (price >= 0) {
            hl(fb, y + 1, COL_GRID);
            snprintf(ibuf, sizeof ibuf, "%s %dCR", price_label, price);
            eui_text(fb, ibuf, 4, y + 4, COL_CRED);
        }
        hl(fb, 118, COL_GRID);
        craft_font_draw(fb, footer, 2, 121, COL_DIM);
        return;
    }

    if (wi->type >= WPN_COUNT) {
        /* Equipment sheet: protection rather than firepower. */
        icon_weapon_2x(fb, 4, 3, wi->type);
        eui_textclip(fb, item_name(wi->type), 32, 124, 2, COL_HDR);
        craft_font_draw(fb, k_qual_long[wi->quality > 4 ? 4 : wi->quality],
                        32, 13,
                        (wi->quality >= Q_MILITARY) ? COL_CRED : COL_DIM);
        hl(fb, 21, COL_GRID);
        int y = 25;
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
            eui_text(fb, vn, 4, y, RGB565C(150, 220, 255));
            y += 12;
            eui_textclip(fb, fx2, 4, 124, y, COL_DIM);
            y += 13;
        }
        snprintf(buf, sizeof buf, "Z%d", wi->tier);
        stat_r(fb, y, "SIZE", buf, COL_VAL); y += 13;
        float mult = k_tier_mult[wi->tier > 3 ? 3 : wi->tier] *
                     quality_dmg_mult(wi->quality) *
                     (0.6f + 0.4f * wi->integrity * 0.01f);
        snprintf(buf, sizeof buf, "X%d.%d", (int)mult,
                 ((int)(mult * 10)) % 10);
        stat_r(fb, y, "PROTECTION", buf, COL_VAL);
        if (cmp && cmp->in_use && cmp->type == wi->type && cmp != wi) {
            float cm = k_tier_mult[cmp->tier > 3 ? 3 : cmp->tier] *
                       quality_dmg_mult(cmp->quality) *
                       (0.6f + 0.4f * cmp->integrity * 0.01f);
            stat_delta(fb, y, mult - cm, 0);
        }
        y += 13;
        snprintf(buf, sizeof buf, "%d%%", wi->integrity);
        stat_r(fb, y, "INTEGRITY", buf,
               wi->integrity < 60 ? COL_WARN : COL_VAL); y += 13;
        if (price >= 0) {
            hl(fb, y + 1, COL_GRID);
            snprintf(buf, sizeof buf, "%s %dCR", price_label, price);
            eui_text(fb, buf, 4, y + 4, COL_CRED);
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
    /* Readable headline; the VS benchmark tag (right) stays compact so a long
       weapon name and its comparator never collide. The numeric stat block
       below stays in the dense craft font (up to ten rows — a taller font
       would overrun the sheet). */
    int hxmax = cw ? 92 : 124;
    /* Affixed weapons show "VENTED GAUSS" as the headline. */
    if (wi->affix && wi->affix < AFX_COUNT) {
        char nbuf[26];
        snprintf(nbuf, sizeof nbuf, "%.11s %s", k_affixes[wi->affix].name,
                 w->name);
        eui_textclip(fb, nbuf, 32, hxmax, 2, RGB565C(150, 220, 255));
    } else {
        eui_textclip(fb, w->name, 32, hxmax, 2, COL_HDR);
    }
    craft_font_draw(fb, k_qual_long[wi->quality > 4 ? 4 : wi->quality],
                    32, 13, (wi->quality >= Q_MILITARY) ? COL_CRED : COL_DIM);
    if (cw) {
        /* Comparison benchmark as a two-row header tag, top right —
         * the deltas below are measured against this. */
        int tw = craft_font_width("VS");
        craft_font_draw(fb, "VS", 126 - tw, 2, COL_DIM);
        tw = craft_font_width(cw->name);
        craft_font_draw(fb, cw->name, 126 - tw, 13,
                        RGB565C(90, 170, 220));
    }
    hl(fb, 21, COL_GRID);

    float dm = mount_dmg_mult(wi);
    /* Readable stat rows. Pitch shrinks to 10 only when the seeker/blast rows
       push the count to nine or ten, so the densest sheet still fits. */
    int extra = (w->aoe > 0) + (w->turn > 0);
    int rp = extra >= 1 ? 9 : 10;
    int y = 22;
    snprintf(buf, sizeof buf, "Z%d", w->size);
    stat_r(fb, y, "SLOT SIZE", buf, COL_VAL); y += rp;
    snprintf(buf, sizeof buf, "%d.%d", (int)(w->dmg * dm),
             ((int)(w->dmg * dm * 10)) % 10);
    stat_r(fb, y, "DAMAGE", buf, COL_VAL);
    if (cw) stat_delta(fb, y, w->dmg * dm - cw->dmg * cdm, 0);
    y += rp;
    float dps = w->dmg * dm / w->cooldown;
    snprintf(buf, sizeof buf, "%d.%d", (int)dps, ((int)(dps * 10)) % 10);
    stat_r(fb, y, "DPS", buf, COL_VAL);
    if (cw) stat_delta(fb, y, dps - cw->dmg * cdm / cw->cooldown, 0);
    y += rp;
    snprintf(buf, sizeof buf, "%d.%d/S", (int)(w->heat / w->cooldown),
             ((int)(w->heat / w->cooldown * 10)) % 10);
    stat_r(fb, y, "HEAT", buf,
         (w->heat / w->cooldown > 30) ? COL_WARN : COL_VAL);
    if (cw) stat_delta(fb, y, w->heat / w->cooldown -
                                  cw->heat / cw->cooldown, 1);
    y += rp;
    if (w->speed > 0)
        snprintf(buf, sizeof buf, "%dM/S", (int)w->speed);
    else
        snprintf(buf, sizeof buf, "HITSCAN");
    stat_r(fb, y, "VELOCITY", buf, COL_VAL); y += rp;
    snprintf(buf, sizeof buf, "%dM", (int)w->range);
    stat_r(fb, y, "RANGE", buf, COL_VAL);
    if (cw) stat_delta(fb, y, w->range - cw->range, 0);
    y += rp;
    if (w->ammo_max)
        snprintf(buf, sizeof buf, "%d RNDS", w->ammo_max);
    else
        snprintf(buf, sizeof buf, "ENERGY");
    stat_r(fb, y, "AMMO", buf, COL_VAL); y += rp;
    if (w->aoe > 0) {
        snprintf(buf, sizeof buf, "%dM BLAST", (int)w->aoe);
        stat_r(fb, y, "WARHEAD", buf, COL_WARN); y += rp;
    }
    if (w->turn > 0) {
        stat_r(fb, y, "GUIDANCE", "SEEKER", COL_ILL); y += rp;
    }
    snprintf(buf, sizeof buf, "%d%%", wi->integrity);
    stat_r(fb, y, "INTEGRITY", buf,
         wi->integrity < 60 ? COL_WARN : COL_VAL); y += rp;

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
    /* Fill everything EXCEPT a big top-right box, where the live 3D ship renders.
       All specs fit on one screen: 7 in the left column, 3 bottom-right (aligned
       with the left column's bottom three rows). */
    for (int y = 0; y < ELITE_FB_H; y++) {
        int hi = (y < 57) ? 63 : ELITE_FB_W;   /* leave x63..127 open up top for the ship */
        uint16_t *row = fb + y * ELITE_FB_W;
        for (int x = 0; x < hi; x++) row[x] = COL_BG;
    }

    const HullDef *h = &k_hulls[hull_id];
    const HullDef *cur = &k_hulls[g_player.hull_id];
    HullRoll rv;
    hull_roll(hull_id, seed, &rv);
    const HullRoll *rc = player_roll();
    char buf[28];
    eui_textclip(fb, h->name, 2, 60, 1, COL_HDR);
    for (int x = 0; x < 61; x++) fb[13 * ELITE_FB_W + x] = COL_GRID;

    #define CMPC(nv, cv) cmp_col((float)(nv), (float)(cv))
    #define HS(lx, vx, yy, label, col, fmt, ...) do { \
        eui_text(fb, label, lx, yy, COL_DIM); \
        snprintf(buf, sizeof buf, fmt, __VA_ARGS__); \
        eui_textclip(fb, buf, vx, (lx) < 40 ? 71 : 126, yy, col); \
    } while (0)
    /* Left column (x2): the flight-performance + defence stats. */
    int y = 15;
    HS(2, 32, y, "SPD", CMPC(h->max_speed*rv.spd, cur->max_speed*rc->spd), "%d", (int)(h->max_speed*rv.spd)); y+=11;
    HS(2, 32, y, "ACC", CMPC(h->accel*rv.acc, cur->accel*rc->acc), "%d", (int)(h->accel*rv.acc)); y+=11;
    { float tn=h->turn_rate*rv.trn; HS(2, 32, y, "TRN", CMPC(tn, cur->turn_rate*rc->trn), "%d.%d",(int)tn,((int)(tn*10))%10); } y+=11;
    HS(2, 32, y, "CRG", CMPC(rv.cargo, rc->cargo), "%dT", rv.cargo); y+=11;
    { float jn=h->jump_range*rv.jmp; HS(2, 32, y, "JMP", CMPC(jn, cur->jump_range*rc->jmp), "%d.%d",(int)jn,((int)(jn*10))%10); } y+=11;
    HS(2, 32, y, "HUL", CMPC(h->hull_base*rv.hull, cur->hull_base*rc->hull), "%d",(int)(h->hull_base*rv.hull)); y+=11;
    HS(2, 32, y, "SHD", CMPC(h->shield_base*rv.shd, cur->shield_base*rc->shd), "%d",(int)(h->shield_base*rv.shd)); y+=11;
    /* Bottom-right (x66): fitting stats, aligned with the left column's JMP/HUL/SHD. */
    int yr = 59;
    HS(66, 96, yr, "TIER", CMPC(h->max_shield_tier+h->max_hull_tier, cur->max_shield_tier+cur->max_hull_tier), "S%dH%d", h->max_shield_tier, h->max_hull_tier); yr+=11;
    {   int ng=0, cg=0;
        for (int i=0;i<rv.n_slots;i++) ng+=rv.slot_size[i];
        for (int i=0;i<rc->n_slots;i++) cg+=rc->slot_size[i];
        char s[14]; int sl=0;
        for (int i=0;i<rv.n_slots && sl<11;i++){ s[sl++]='Z'; s[sl++]=(char)('0'+rv.slot_size[i]); }
        s[sl]=0;
        HS(66, 92, yr, "GUN", CMPC(ng,cg), "%s", s); } yr+=11;
    HS(66, 96, yr, "UTL", CMPC(rv.utils, rc->utils), "%d", rv.utils); yr+=11;
    #undef HS
    #undef CMPC

    hl(fb, 94, COL_GRID);
    if (cost == DETAIL_OWNED)
        eui_text(fb, "OWNED", 3, 97, COL_CRED);
    else if (cost < 0) { snprintf(buf, sizeof buf, "GET %dCR (TRADE-IN)", -cost);
                         eui_textclip(fb, buf, 3, 126, 97, COL_CRED); }
    else { snprintf(buf, sizeof buf, "COST %dCR (TRADE-IN)", cost);
           eui_textclip(fb, buf, 3, 126, 97, COL_CRED); }
    snprintf(buf, sizeof buf, "LIST %dCR", h->price);
    craft_font_draw(fb, buf, 3, 110, COL_DIM);
    craft_font_draw(fb, footer, 2, 121, COL_DIM);
}

void detail_draw_good(uint16_t *fb, int good, int held,
                      const char *footer) {
    fill(fb, COL_BG);
    const GoodDef *g = &k_goods[good];
    char buf[28];
    int illegal = (g->flags & GOOD_ILLEGAL) != 0;
    eui_textclip(fb, g->name, 4, illegal ? 82 : 124, 2, COL_HDR);
    if (illegal)
        eui_textr(fb, "ILLEGAL", 124, 2, COL_ILL);
    hl(fb, 15, COL_GRID);
    int y = 19;
    snprintf(buf, sizeof buf, "%d UNITS HELD", held);
    eui_text(fb, buf, 4, y, COL_VAL); y += 13;
    snprintf(buf, sizeof buf, "GALACTIC AVG %dCR", g->base);
    eui_text(fb, buf, 4, y, COL_DIM); y += 15;
    if (illegal)
        y = eui_wrap(fb, "ONLY BLACK MARKETS WILL TOUCH THIS",
                     4, 124, y, 116, COL_ILL);
    else
        y = eui_wrap(fb, "SELL WHERE THE ECONOMY IMPORTS IT",
                     4, 124, y, 116, COL_DIM);
    hl(fb, 118, COL_GRID);
    craft_font_draw(fb, footer, 2, 121, COL_DIM);
}
