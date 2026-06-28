/*
 * ThumbyElite — parametric NPC portraits (see r3d_face.h).
 *
 * Everything is drawn in box-relative pixels from float fractions of the
 * box size, so one code path serves any portrait size. Symmetry is what
 * makes a few rectangles read as a face — eyes, brows and ears mirror
 * around the centre line; asymmetry is reserved for markings (scars).
 */
#include "r3d_face.h"
#include "elite_types.h"
#include "events.h"
#include <math.h>

/* --- seed stream -------------------------------------------------------- */
typedef struct { uint32_t s; } FRng;
static uint32_t fr_u32(FRng *r) {
    r->s ^= r->s << 13; r->s ^= r->s >> 17; r->s ^= r->s << 5;
    return r->s;
}
static int fr_n(FRng *r, int n) { return (int)(fr_u32(r) % (uint32_t)n); }
static int fr_pct(FRng *r, int pct) { return fr_n(r, 100) < pct; }

/* --- tiny raster -------------------------------------------------------- */
typedef struct { uint16_t *fb; int x, y, s; } FCtx;

static void fpx(FCtx *c, int x, int y, uint16_t col) {
    if (x < 0 || y < 0 || x >= c->s || y >= c->s) return;
    c->fb[(c->y + y) * ELITE_FB_W + c->x + x] = col;
}
static void fspan(FCtx *c, int x0, int x1, int y, uint16_t col) {
    for (int x = x0; x <= x1; x++) fpx(c, x, y, col);
}
static void frect(FCtx *c, int x0, int y0, int w, int h, uint16_t col) {
    for (int y = y0; y < y0 + h; y++) fspan(c, x0, x0 + w - 1, y, col);
}

static uint16_t shade(uint16_t col, int pct) {   /* pct 100 = unchanged */
    int r = ((col >> 11) & 31) * pct / 100;
    int g = ((col >> 5) & 63) * pct / 100;
    int b = (col & 31) * pct / 100;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* --- palettes ------------------------------------------------------------*/
static const uint16_t k_skin[10] = {       /* wider human range (user) */
    RGB565C(245, 205, 178), RGB565C(232, 190, 160), RGB565C(214, 168, 132),
    RGB565C(208, 158, 120), RGB565C(186, 138, 100), RGB565C(176, 126, 90),
    RGB565C(150, 104, 72),  RGB565C(130, 90, 62),   RGB565C(104, 70, 50),
    RGB565C(78, 52, 38),
};
static const uint16_t k_hair[8] = {
    RGB565C(28, 22, 20),  RGB565C(54, 38, 28),   RGB565C(86, 58, 32),
    RGB565C(150, 110, 50), RGB565C(210, 170, 90), RGB565C(190, 186, 178),
    RGB565C(120, 40, 28),  RGB565C(70, 74, 82),
};
static const uint16_t k_iris[6] = {
    RGB565C(70, 48, 28),  RGB565C(60, 100, 160), RGB565C(70, 120, 70),
    RGB565C(150, 110, 40), RGB565C(110, 70, 140), RGB565C(70, 110, 110),
};
static const uint16_t k_suit[12] = {       /* garment colours, widened */
    RGB565C(40, 52, 78),  RGB565C(70, 60, 48),  RGB565C(58, 70, 58),
    RGB565C(120, 70, 30), RGB565C(56, 48, 64),  RGB565C(120, 36, 40),
    RGB565C(36, 84, 96),  RGB565C(150, 130, 70), RGB565C(44, 46, 52),
    RGB565C(96, 60, 110), RGB565C(70, 96, 60),  RGB565C(150, 150, 158),
};
static const uint16_t COL_SCLERA = RGB565C(225, 222, 205);
static const uint16_t COL_SYNTH  = RGB565C(120, 130, 145);
static const uint16_t COL_GLOW   = RGB565C(120, 230, 255);

/* ADOPTED 2026-06-12: the v2 proposal faces are the live look (species,
 * volumed hair, helmets). Style 0 kept selectable for sheet comparisons. */
static int s_style = 1;
void face_set_style(int s) { s_style = s; }

/* PROPOSAL palettes: species skins + louder hair. */
static const uint16_t k_skin_avian[7] = {  /* plumage hues, widened */
    RGB565C(210, 168, 96),  RGB565C(150, 172, 184), RGB565C(228, 216, 198),
    RGB565C(164, 120, 86),  RGB565C(120, 150, 200), RGB565C(196, 96, 84),
    RGB565C(96, 110, 96),
};
static const uint16_t k_skin_saur[7] = {   /* scale hues, widened */
    RGB565C(108, 152, 92),  RGB565C(82, 132, 122),  RGB565C(154, 150, 78),
    RGB565C(92, 112, 138),  RGB565C(168, 112, 64),  RGB565C(70, 120, 84),
    RGB565C(132, 80, 110),
};
static const uint16_t k_hair_punk[3] = {
    RGB565C(60, 200, 190), RGB565C(220, 80, 160), RGB565C(235, 235, 235),
};

/* ellipse half-width at normalised row ny (clamped) — hair masses reuse
 * the skull's profile so they follow it instead of boxing it */
static int ell_w(int rx, float ny) {
    if (ny < -1.0f) ny = -1.0f;
    if (ny > 1.0f) ny = 1.0f;
    return (int)((float)rx * sqrtf(1.0f - ny * ny));
}

void face_draw(uint16_t *fb, int bx, int by, int size, uint32_t seed,
               int kind) {
    FCtx c = { fb, bx, by, size };
    FRng r = { seed ? seed : 1u };
    /* style-1 extras draw from their own stream so the style-0 fr_n/fr_pct
     * sequence is never disturbed (style-0 must stay byte-identical) */
    FRng r2 = { (seed ? seed : 1u) * 2654435761u };
    float S = (float)size;

    int synth = (kind != NK_MYSTIC) && fr_pct(&r, 10);
    /* species: 0 human, 1 synth, 2 avian, 3 saurian, 4 heavyworlder */
    int species = synth ? 1 : 0;
    if (s_style == 1 && kind != NK_MYSTIC) {
        int sr = fr_n(&r, 100);
        /* aliens are common AND as varied as humans (user): wide skin
         * palettes + per-species feature genes below. */
        species = (sr < 54) ? 0 : (sr < 64) ? 1 : (sr < 77) ? 2
                : (sr < 89) ? 3 : 4;
        synth = (species == 1);
    }
    uint16_t skin = synth ? COL_SYNTH : k_skin[fr_n(&r, 10)];
    if (kind == NK_MYSTIC) skin = shade(skin, 88);          /* pallid */
    if (kind == NK_PIRATE) skin = shade(skin, 92);
    uint16_t skin_d = shade(skin, 72), skin_l = shade(skin, 116);
    uint16_t hair = k_hair[fr_n(&r, 8)];
    uint16_t iris = synth ? COL_GLOW : k_iris[fr_n(&r, 6)];
    if (s_style == 1) {
        if (species == 2) { skin = k_skin_avian[fr_n(&r, 7)];
                            iris = RGB565C(20, 16, 12); }
        if (species == 3) { skin = k_skin_saur[fr_n(&r, 7)];
                            iris = RGB565C(230, 190, 60); }
        if (species == 4) skin = shade(k_skin[fr_n(&r, 10)], 92);
        if ((species <= 1 || species == 4) && kind != NK_MYSTIC &&
            fr_pct(&r, 14))                  /* not under mystic hoods */
            hair = k_hair_punk[fr_n(&r, 3)];
        skin_d = shade(skin, 72);
        skin_l = shade(skin, 116);
    }
    uint16_t suit = k_suit[kind == NK_OFFICIAL ? 0 : fr_n(&r, 12)];
    int garb = fr_n(&r, 6);   /* clothing cut/detail (user) */

    /* panel backdrop: deep blue-grey, faint floor glow */
    for (int y = 0; y < size; y++) {
        uint16_t bg = shade(RGB565C(16, 22, 36), 70 + 50 * y / size);
        fspan(&c, 0, size - 1, y, bg);
    }

    /* geometry (box fractions) */
    int cx = size / 2;
    int cy = (int)(S * 0.44f);
    int rx = (int)(S * (0.24f + 0.05f * (float)fr_n(&r, 4) / 3.0f));
    int ry = (int)(S * (0.30f + 0.05f * (float)fr_n(&r, 4) / 3.0f));
    float jaw = 0.10f + 0.35f * (float)fr_n(&r, 4) / 3.0f;
    /* per-species feature genes (user: aliens should vary as much as
     * humans) — head shape + the species' signature features all jitter. */
    int   av_crest = 3 + fr_n(&r, 4);          /* 3-6 plume pairs        */
    int   av_chgt  = 2 + fr_n(&r, 4);          /* crest height           */
    float sr_snout = 0.78f + 0.10f * fr_n(&r, 5);  /* muzzle length      */
    int   sr_teeth = 3 + fr_n(&r, 4);          /* crown-fin tooth count  */
    float hv_jowl  = 0.14f + 0.05f * fr_n(&r, 4);  /* jowl bulge         */
    if (s_style == 1) {
        if (species == 2) {                    /* avian: slim, narrow head */
            rx = (int)(rx * (0.78f + 0.04f * fr_n(&r, 4)));
            ry = (int)(ry * (0.95f + 0.04f * fr_n(&r, 3)));
            jaw = 0.34f + 0.07f * fr_n(&r, 4);
        }
        if (species == 3)                      /* saurian: longer skull */
            ry = (int)(ry * (1.0f + 0.05f * fr_n(&r, 4)));
        if (species == 4) {                    /* heavyworlder: broad */
            rx = (int)(rx * (1.14f + 0.06f * fr_n(&r, 3)));
            jaw = 0.05f;
            ry = (int)(ry * 0.95f);
        }
    }

    /* shoulders + neck under the head. Cut varies per garb: width,
     * slope, and a garment detail (collar V, zip seam, yoke, sash,
     * tabs) so two pilots in the same colour still differ. */
    int sh_y = cy + ry - (int)(S * 0.04f);
    int sh_w0 = (int)(S * (0.14f + 0.04f * (garb & 1)));
    int slope = 2 + (garb % 3);
    uint16_t suit_d = shade(suit, 70), suit_l = shade(suit, 132);
    for (int y = sh_y; y < size; y++) {
        int w = sh_w0 + (y - sh_y) * slope / 2;
        fspan(&c, cx - w, cx + w, y, suit);
        fpx(&c, cx - w, y, suit_l);
        fpx(&c, cx + w, y, suit_d);
    }
    {
        int gy = sh_y + (int)(S * 0.03f);
        if (garb == 0) {
            for (int t = 0; t < (int)(S * 0.10f); t++) {
                fpx(&c, cx - t, gy + t, suit_d);
                fpx(&c, cx + t, gy + t, suit_d);
            }
        } else if (garb == 1) {
            for (int y = gy; y < size; y++) fpx(&c, cx, y, suit_l);
        } else if (garb == 2) {
            fspan(&c, cx - sh_w0 - slope, cx + sh_w0 + slope,
                  sh_y + 2, suit_l);
        } else if (garb == 3) {
            for (int t = 0; t < (int)(S * 0.22f); t++)
                fpx(&c, cx - (int)(S * 0.16f) + t, sh_y + 2 + t,
                    RGB565C(190, 170, 90));
        } else if (garb == 4) {
            frect(&c, cx - (int)(S * 0.07f), gy, 3, 3, suit_l);
            frect(&c, cx + (int)(S * 0.07f) - 2, gy, 3, 3, suit_l);
        }
    }
    if (kind == NK_OFFICIAL) {
        fspan(&c, cx - (int)(S * 0.2f), cx + (int)(S * 0.2f),
              sh_y + 2, RGB565C(200, 170, 60));
        frect(&c, cx - sh_w0 - 1, sh_y + (int)(S * 0.06f), 4, 2,
              RGB565C(220, 185, 70));
        frect(&c, cx + sh_w0 - 2, sh_y + (int)(S * 0.06f), 4, 2,
              RGB565C(220, 185, 70));
    }
    if (s_style == 1 && species == 4)            /* heavyworlder: bull neck */
        frect(&c, cx - (int)(S * 0.16f), cy + ry - (int)(S * 0.10f),
              (int)(S * 0.32f) + 1, (int)(S * 0.14f), skin_d);
    else
    frect(&c, cx - (int)(S * 0.09f), cy + ry - (int)(S * 0.10f),
          (int)(S * 0.18f) + 1, (int)(S * 0.14f), skin_d);   /* neck */

    /* head: per-row width profile — oval with a shaped chin */
    for (int y = cy - ry; y <= cy + ry; y++) {
        float ny = (float)(y - cy) / (float)ry;
        float w = (float)rx * sqrtf(1.0f - ny * ny);
        if (ny > 0.15f) w *= 1.0f - jaw * (ny - 0.15f) / 0.85f;
        if (s_style == 1 && species == 4 && ny > 0.0f)
            w *= 1.0f + hv_jowl * 4.0f * ny * (1.0f - ny);  /* jowls */
        int wi = (int)w;
        if (wi < 1) continue;
        fspan(&c, cx - wi, cx + wi, y, skin);
        /* simple right-side shade + left rim light */
        for (int x = cx + wi - wi / 4; x <= cx + wi; x++)
            fpx(&c, x, y, skin_d);
        fpx(&c, cx - wi, y, skin_l);
    }
    if (synth) {                                   /* faceplate seam */
        for (int y = cy - ry + 2; y <= cy + ry - 2; y += 2)
            fpx(&c, cx, y, shade(skin, 60));
    }

    /* ears */
    int ey = cy - (int)(S * 0.02f);
    int hood = (kind == NK_MYSTIC) ? fr_pct(&r, 80) : fr_pct(&r, 6);
    int helmet = 0, breather = 0;
    if (s_style == 1 && !hood && (species <= 1 || species == 4)) {
        int g = fr_n(&r, 100);
        helmet   = g < 14;
        breather = g >= 14 && g < 24;
    }
    if (!hood && !synth && !helmet && species < 2) {
        frect(&c, cx - rx - 1, ey - 1, 2, (int)(S * 0.10f) + 1, skin_d);
        frect(&c, cx + rx - 1, ey - 1, 2, (int)(S * 0.10f) + 1, skin_d);
    }

    /* hair / headgear base (drawn over the skull, under the eyes) */
    int bandana = (kind == NK_PIRATE) && fr_pct(&r, 55);
    int cap = (kind == NK_OFFICIAL || kind == NK_DOCKHAND) && fr_pct(&r, 55);
    int bald = synth || fr_pct(&r, 18);
    int hl = (int)(S * (0.08f + 0.07f * (float)fr_n(&r, 3) / 2.0f));
    if (s_style == 1 && (helmet || species >= 2 ||
                         (!hood && !bandana && !cap && !bald))) {
        if (helmet) {
            /* full helm: dome to below the ears + bright rim; the visor
             * band lands at the eye stage. */
            uint16_t hm = (kind == NK_OFFICIAL) ? RGB565C(46, 58, 88)
                                                : RGB565C(105, 112, 122);
            int hb = cy + (int)(S * 0.06f);
            for (int y = cy - ry - 2; y <= hb; y++) {
                float nyh = (float)(y - cy) / (float)(ry + 2);
                if (nyh < -1.0f) nyh = -1.0f;
                int wi = (int)((float)(rx + 2) *
                               sqrtf(1.0f - nyh * nyh)) + 1;
                fspan(&c, cx - wi, cx + wi, y,
                      y > hb - 2 ? shade(hm, 140) : hm);
            }
            fpx(&c, cx + rx - 1, cy - ry + 2, RGB565C(255, 90, 60));
        } else if (species == 2) {
            /* feather crest: plumes ROOTED ON THE SKULL ARC (not floated
             * over it), swept backwards — back plumes drawn first and
             * darker so the overlap between neighbours reads */
            uint16_t fc = shade(skin, 124), fd = shade(skin, 84);
            int dir = fr_pct(&r2, 50) ? 1 : -1;
            for (int k = av_crest - 1; k >= 0; k--) {
                int bxo = 2 * k - (av_crest + 1);    /* front → back */
                if (bxo > rx - 2) bxo = rx - 2;
                if (bxo < 2 - rx) bxo = 2 - rx;
                float fx = (float)bxo / (float)rx;
                int ys = cy - (int)((float)ry *
                                    sqrtf(1.0f - fx * fx));
                int sx = cx + dir * bxo;
                int len = av_chgt + (av_crest - k); /* front plumes taller */
                uint16_t col = (k & 1) ? fd : fc;
                for (int t = 0; t <= len; t++) {
                    float f = (float)t / (float)len;
                    int x = sx + dir * (int)(f * f * (float)(2 + k));
                    int y = ys + 1 - t;
                    fpx(&c, x, y, col);
                    fpx(&c, x + dir, y, col);        /* 2px-thick plume */
                    if (t == len) fpx(&c, x + dir, y - 1, col); /* tip */
                }
            }
        } else if (species == 3) {
            /* crown ridge: serrated fin — wide-based teeth rooted on the
             * skull arc, fusing into one mass, tall in the centre. Tooth
             * count varies per saurian. */
            int half_t = sr_teeth / 2;
            for (int k = -half_t; k <= half_t; k++) {
                int ak = k < 0 ? -k : k;
                int h = 5 - ak * 4 / (half_t + 1) + (ak == 0);
                if (h < 2) h = 2;
                int sx = cx + k * 3;
                float fx = (float)(k * 3) / (float)rx;
                if (fx > 0.95f) fx = 0.95f;
                if (fx < -0.95f) fx = -0.95f;
                int ys = cy - (int)((float)ry *
                                    sqrtf(1.0f - fx * fx));
                for (int i = 0; i <= h; i++) {
                    int half = i / 2;                /* fuses at the base */
                    fspan(&c, sx - half, sx + half, ys - h + i, skin_d);
                    fpx(&c, sx - half, ys - h + i, shade(skin, 108));
                }
            }
        } else {
            /* hair proper: every style starts from a crown mass that
             * follows the skull profile and overhangs it 1-2px; a darker
             * underside / parting row keeps it from reading as flat fill.
             * 0 crop, 1 swept parting, 2 curly, 3 bob+bangs, 4 long,
             * 5 ponytail, 6 bun, 7 mohawk */
            uint16_t hd = shade(hair, 60), hg = shade(hair, 128);
            int hs = fr_n(&r2, 8);
            int dir = fr_pct(&r2, 50) ? 1 : -1;
            int m = (hs == 2) ? 2 : 1;               /* overhang margin */
            int rxm = rx + m, rym = ry + m;
            int ytop = cy - rym;
            int depth = hl + m;
            if (hs == 2) depth = hl + 2;
            if (hs == 3) depth = (ey - 3) - ytop;    /* bangs to brow line */
            if (hs == 7) {
                /* mohawk: solid tapered fin — 1px tip, 3px body — kept
                 * inside the box; sides stay shaved */
                int mh = (int)(S * 0.20f);
                if (mh > cy - ry - 2) mh = cy - ry - 2;
                for (int i = 0; i <= mh; i++) {
                    /* tall fins get a 1px tip; stubby ones stay solid */
                    int half = (mh >= 5) ? (i >= 2) : 1;
                    int y = cy - ry - mh + i;
                    fspan(&c, cx - half, cx + half, y, hair);
                    if (half)                        /* shaded flank */
                        fpx(&c, cx + half, y, hd);
                }
                for (int y = cy - ry; y < cy - ry + hl; y++) {
                    fspan(&c, cx - 1, cx + 1, y, hair);
                    fpx(&c, cx + 1, y, hd);
                }
            } else {
                /* crown cap */
                for (int y = ytop; y < ytop + depth; y++) {
                    float nyh = (float)(y - cy) / (float)rym;
                    int wi = ell_w(rxm, nyh);
                    if (hs == 2) wi += fr_n(&r2, 2); /* curly: bumpy edge */
                    uint16_t col =
                        (hs != 3 && y == ytop + depth - 1) ? hd : hair;
                    fspan(&c, cx - wi, cx + wi, y, col);
                    if (hs == 2)                     /* curl texture dots */
                        for (int x = cx - wi + 1; x < cx + wi; x++)
                            if (((x * 7 + y * 13) % 9) == 0)
                                fpx(&c, x, y, hd);
                }
                if (hs != 2)                         /* top sheen */
                    fspan(&c, cx - rxm / 2, cx - rxm / 4, ytop + 1, hg);
                if (hs == 0) {
                    /* short crop: sideburns hug the head a few rows on */
                    for (int y = ytop + depth; y < ytop + depth + 3; y++) {
                        int wi = ell_w(rx, (float)(y - cy) / (float)ry);
                        fpx(&c, cx - wi, y, hair);
                        fpx(&c, cx + wi, y, hd);
                    }
                } else if (hs == 1) {
                    /* swept parting: diagonal fringe descends toward dir,
                     * dark parting line on the other side */
                    int ext = 3 + fr_n(&r2, 2);
                    for (int e = 0; e < ext; e++) {
                        int y = ytop + depth + e;
                        int wi = ell_w(rx, (float)(y - cy) / (float)ry);
                        int cut = -wi + (2 * wi * (e + 1)) / (ext + 1);
                        uint16_t col = (e == ext - 1) ? hd : hair;
                        if (dir > 0) fspan(&c, cx + cut, cx + wi, y, col);
                        else         fspan(&c, cx - wi, cx - cut, y, col);
                    }
                    for (int y = ytop + 1; y < ytop + 4; y++)
                        fpx(&c, cx - dir * (rx / 2), y, hd);  /* parting */
                } else if (hs == 2) {
                    /* curly: two rounding rows close the bottom edge */
                    for (int e = 0; e < 2; e++) {
                        int y = ytop + depth + e;
                        int wi = ell_w(rx, (float)(y - cy) / (float)ry) - e;
                        fspan(&c, cx - wi + fr_n(&r2, 2), cx - wi / 3, y,
                              hair);
                        fspan(&c, cx + wi / 3, cx + wi - fr_n(&r2, 2), y,
                              hair);
                    }
                } else if (hs == 3) {
                    /* bob: scalloped fringe over the forehead + curtains
                     * down past the ears, curling inwards at the jaw */
                    int yb = ytop + depth - 1;
                    for (int x = cx - rx + 2; x <= cx + rx - 2; x += 3)
                        fpx(&c, x, yb + 1, hair);    /* scallop teeth */
                    int yj = cy + (int)(ry * 0.45f);
                    for (int y = yb; y <= yj; y++) {
                        int wi = ell_w(rxm, (float)(y - cy) / (float)rym);
                        frect(&c, cx - wi, y, 2, 1, hair);
                        frect(&c, cx + wi - 1, y, 2, 1, hd);
                    }
                    int wj = ell_w(rxm, (float)(yj - cy) / (float)rym);
                    fspan(&c, cx - wj, cx - wj + 2, yj + 1, hd);  /* curl */
                    fspan(&c, cx + wj - 2, cx + wj, yj + 1, hd);
                } else if (hs == 4) {
                    /* long flowing: falls straight past the cheeks and
                     * behind the shoulders, ends ragged */
                    int yend = sh_y + (int)(S * 0.08f);
                    for (int y = ytop + depth - 1; y <= yend; y++) {
                        float nyh = (float)(y - cy) / (float)ry;
                        if (nyh > 0.55f) nyh = 0.55f;
                        int wi = ell_w(rx, nyh);
                        int out = 2 + (y - ytop - depth) / 4;
                        if (out > 4) out = 4;
                        int rag = (y > yend - 2) ? fr_n(&r2, 3) : 0;
                        fspan(&c, cx - wi - out + rag, cx - wi, y, hair);
                        fpx(&c, cx - wi - out + rag, y, hg);  /* rim */
                        rag = (y > yend - 2) ? fr_n(&r2, 3) : 0;
                        fspan(&c, cx + wi, cx + wi + out - rag, y, hair);
                        fpx(&c, cx + wi, y, hd);     /* inner shadow */
                    }
                } else if (hs == 5) {
                    /* ponytail: knot gathered high on one side, the tail
                     * swings clear of the head (and any jowls) and hangs
                     * to the shoulder with a tapered tip */
                    int rxw = (species == 4) ? (int)((float)rx * 1.25f)
                                             : rx;
                    int px2 = cx + dir * (rxw + 2);
                    int kx = cx + dir * (rx - 2);    /* knot on the crown */
                    for (int yy = -1; yy <= 1; yy++)
                        fspan(&c, kx - 1, kx + 1, ytop - 1 + yy, hair);
                    fpx(&c, kx + dir, ytop, hd);     /* tie */
                    int yend = cy + (int)(ry * 0.8f);
                    int reach = (px2 - kx) * dir;    /* px from knot out */
                    for (int y = ytop + 1; y <= yend; y++) {
                        /* swing out from the knot, then hang straight */
                        int off = y - ytop;
                        if (off > reach) off = reach;
                        int x0 = kx + dir * off;
                        int wpt = (y > yend - 2) ? 0 : 1; /* taper */
                        fspan(&c, x0 - wpt, x0 + wpt, y, hair);
                        fpx(&c, x0 + dir * wpt, y, hd);
                    }
                } else if (hs == 6) {
                    /* bun: round knot riding above the crown */
                    int bx2 = cx + dir * 2, by2 = ytop - 2;
                    for (int yy = -2; yy <= 2; yy++)
                        for (int xx = -2; xx <= 2; xx++)
                            if (xx * xx + yy * yy <= 5)
                                fpx(&c, bx2 + xx, by2 + yy, hair);
                    fpx(&c, bx2 - 1, by2 - 1, hg);   /* sheen */
                    fspan(&c, bx2 - 1, bx2 + 1, ytop, hd);  /* tie */
                }
            }
        }
    } else
    if (bandana) {
        uint16_t bc = RGB565C(140, 40, 40);
        for (int y = cy - ry; y < cy - ry + (int)(S * 0.14f); y++) {
            float ny = (float)(y - cy) / (float)ry;
            int wi = (int)((float)rx * sqrtf(1.0f - ny * ny));
            fspan(&c, cx - wi, cx + wi, y, bc);
        }
        frect(&c, cx + rx - 2, cy - ry + 2, 3, 4, shade(RGB565C(140,40,40), 70));
    } else if (cap) {
        uint16_t cc = (kind == NK_OFFICIAL) ? RGB565C(36, 46, 70)
                                            : RGB565C(150, 110, 40);
        for (int y = cy - ry - 1; y < cy - ry + (int)(S * 0.12f); y++) {
            float ny = (float)(y - cy) / (float)ry;
            if (ny < -1.0f) ny = -1.0f;
            int wi = (int)((float)rx * sqrtf(1.0f - ny * ny)) + 1;
            fspan(&c, cx - wi, cx + wi, y, cc);
        }
        fspan(&c, cx - rx - 1, cx + rx + 1,
              cy - ry + (int)(S * 0.12f), shade(RGB565C(36,46,70), 60));
    } else if (!bald) {
        /* hairline cap + optional side hair */
        for (int y = cy - ry - 1; y < cy - ry + hl; y++) {
            float ny = (float)(y - cy) / (float)ry;
            if (ny < -1.0f) ny = -1.0f;
            int wi = (int)((float)rx * sqrtf(1.0f - ny * ny)) + 1;
            fspan(&c, cx - wi, cx + wi, y, hair);
        }
        if (fr_pct(&r, 50)) {                      /* side hair hugs the head */
            int sl = (int)(S * 0.16f);
            for (int y = cy - ry + hl; y < cy - ry + hl + sl; y++) {
                float ny = (float)(y - cy) / (float)ry;
                if (ny > 0.99f) break;
                int wi = (int)((float)rx * sqrtf(1.0f - ny * ny));
                frect(&c, cx - wi - 1, y, 2, 1, hair);
                frect(&c, cx + wi, y, 2, 1, hair);
            }
        }
    }

    /* eyes — mirrored; the single most load-bearing feature */
    int edx = (int)(S * (0.10f + 0.04f * (float)fr_n(&r, 3) / 2.0f));
    int ew = (int)(S * 0.09f); if (ew < 2) ew = 2;
    int eh = (int)(S * 0.05f); if (eh < 1) eh = 1;
    int visor = ((kind == NK_OFFICIAL) && fr_pct(&r, 45)) || helmet;
    int slit = 0;
    if (s_style == 1) {
        slit = (species == 3);
        if (species == 2) {                  /* round, side-set */
            ew = (int)(S * 0.10f); if (ew < 3) ew = 3;
            eh = ew - 1;
            edx = rx - ew / 2 - 2;           /* hug the narrowed head */
            if (edx < 2) edx = 2;
        }
    }
    if (!visor) {
        uint16_t sclera = hood ? shade(COL_SCLERA, 55) : COL_SCLERA;
        if (slit) sclera = RGB565C(215, 185, 70);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int ex = cx + sgn * edx - ew / 2;
            frect(&c, ex, ey, ew, eh + 1, sclera);
            if (slit) {
                frect(&c, ex + ew / 2, ey, 1, eh + 1, RGB565C(8, 8, 10));
            } else {
                frect(&c, ex + ew / 2 - 1, ey, 2, eh + 1, iris);
                fpx(&c, ex + ew / 2 - 1 + fr_n(&r, 2), ey + eh / 2,
                    RGB565C(8, 8, 10));
            }
        }
        if (s_style == 1 && species == 2)    /* knock corners off → round */
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                int ex = cx + sgn * edx - ew / 2;
                fpx(&c, ex, ey, skin); fpx(&c, ex + ew - 1, ey, skin);
                fpx(&c, ex, ey + eh, skin);
                fpx(&c, ex + ew - 1, ey + eh, skin);
            }
        /* brows: tilt 0 neutral, 1 angry-in, 2 raised */
        int tilt = (kind == NK_PIRATE) ? 1 : fr_n(&r, 3);
        uint16_t bcol = synth ? shade(COL_SYNTH, 55) : shade(hair, 70);
        if (s_style == 1 && species == 4) {
            /* heavyworlder: one continuous brow shelf — lit top edge,
             * cast shadow right over the eyes */
            int bx0 = cx - edx - ew / 2 - 1, bw = edx * 2 + ew + 2;
            frect(&c, bx0, ey - eh - 3, bw, 1, skin_l);
            frect(&c, bx0, ey - eh - 2, bw, 2, skin_d);
            frect(&c, bx0 + 1, ey - eh, bw - 2, 1, shade(skin, 52));
        } else if (s_style == 1 && species == 3) {
            /* saurian: bony ridge over each eye, dropping at the outer
             * corner */
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                int ex = cx + sgn * edx - ew / 2;
                frect(&c, ex, ey - eh - 2, ew, 1, skin_d);
                fpx(&c, ex + (sgn > 0 ? ew - 1 : 0), ey - eh - 1, skin_d);
            }
        } else if (s_style == 1 && species == 2) {
            /* avian: no brows — the crest and beak carry the face */
        } else
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int ex = cx + sgn * edx - ew / 2;
            int yo = ey - eh - 1;
            for (int i = 0; i < ew; i++) {
                int dy = 0;
                if (tilt == 1) dy = (sgn < 0 ? i : ew - 1 - i) / 3;
                if (tilt == 2) dy = -((sgn < 0 ? ew - 1 - i : i) / 4);
                fpx(&c, ex + i, yo + dy, bcol);
            }
        }
    } else {
        /* mirrored visor band with a glint */
        frect(&c, cx - rx + 1, ey - 1, 2 * rx - 1, eh + 3,
              RGB565C(20, 26, 34));
        fspan(&c, cx - rx + 2, cx - rx / 3, ey, RGB565C(90, 140, 190));
    }

    /* nose + mouth */
    int nly = ey + (int)(S * 0.12f);
    int my = nly + (int)(S * 0.08f);
    int mw = (int)(S * 0.08f) + fr_n(&r, 3);
    int mood = fr_n(&r, 3);                 /* 0 flat 1 frown 2 smile */
    uint16_t mcol = shade(skin, 48);
    if (s_style == 1 && species == 2) {
        /* hooked beak: rounded two-tone wedge — upper mandible lighter,
         * the gape sags at the ends, the tip hooks down to a point */
        uint16_t bk = RGB565C(226, 148, 36);     /* hot horn-orange */
        uint16_t bkl = shade(bk, 118), bkd = shade(bk, 68);
        uint16_t bko = shade(bk, 42);            /* outline vs gold skin */
        int b0 = ey + 1, b1 = my + 3;
        int gp = b0 + (b1 - b0) * 3 / 5;     /* gape row */
        for (int y = b0; y <= b1; y++) {
            float f = (float)(y - b0) / (float)(b1 - b0);
            int half = (int)(S * 0.095f * (1.0f - 0.72f * f * f));
            if (half < 1) half = 1;
            fspan(&c, cx - half, cx + half, y, y <= gp ? bkl : bkd);
            fpx(&c, cx + half, y, bko);      /* dark edges both sides — */
            fpx(&c, cx - half, y, bko);      /* keeps it off gold skin */
        }
        fpx(&c, cx - 1, b0 + 1, bkd);        /* cere nostrils */
        fpx(&c, cx + 1, b0 + 1, bkd);
        {
            int hg2 = (int)(S * 0.095f *
                            (1.0f - 0.72f * 0.36f));    /* width at gape */
            fspan(&c, cx - hg2, cx + hg2, gp, bko);     /* full gape split */
            fpx(&c, cx - hg2 - 1, gp + 1, bko);  /* sagging gape ends */
            fpx(&c, cx + hg2 + 1, gp + 1, bko);
        }
        fspan(&c, cx - 1, cx + 1, b1 + 1, bkd);  /* hooked tip curls under */
        fpx(&c, cx, b1 + 2, bko);
    } else if (s_style == 1 && species == 3) {
        /* muzzle: a lit snout mass protrudes below the eyes — end-on
         * nostrils up top, the gape runs back wider than the snout */
        int mz0 = nly - 2, mz1 = my + 1 + (int)(4 * sr_snout);
        uint16_t snt = shade(skin, 128);     /* catches the light */
        for (int y = mz0; y <= mz1; y++) {
            float f = (float)(y - mz0) / (float)(mz1 - mz0);
            int half = (int)(S * 0.125f * (1.0f - 0.30f * f));
            fspan(&c, cx - half, cx + half, y,
                  y == mz1 ? shade(skin, 55) : snt);
            fpx(&c, cx + half, y, skin_d);   /* right shade */
            fpx(&c, cx - half, y, shade(skin, 145));
        }
        fpx(&c, cx - 2, mz0 + 1, RGB565C(8, 8, 10));  /* end-on nostrils */
        fpx(&c, cx + 2, mz0 + 1, RGB565C(8, 8, 10));
        fpx(&c, cx - 2, mz0 + 2, skin_d);    /* nostril underlip */
        fpx(&c, cx + 2, mz0 + 2, skin_d);
        int gw = (int)(S * 0.16f);
        fspan(&c, cx - gw, cx + gw, my + 1, shade(skin, 42));
        fpx(&c, cx - gw, my, shade(skin, 42));        /* gape upturn */
        fpx(&c, cx + gw, my, shade(skin, 42));
    } else
    {
        frect(&c, cx - 1, ey + 2, 1, nly - ey - 2, skin_d);
        fspan(&c, cx - 1, cx + 1, nly, skin_d);
        fspan(&c, cx - mw, cx + mw, my, mcol);
        if (mood == 1) { fpx(&c, cx - mw, my - 1, mcol); fpx(&c, cx + mw, my - 1, mcol); }
        if (mood == 2) { fpx(&c, cx - mw, my + 1, mcol); fpx(&c, cx + mw, my + 1, mcol); }
    }
    if (s_style == 1 && breather) {
        /* sealed breather mask over the mouth, twin feed lines */
        uint16_t mk = RGB565C(58, 66, 80);
        frect(&c, cx - mw - 1, my - 3, 2 * mw + 3, 6, mk);
        frect(&c, cx - mw - 1, my - 3, 2 * mw + 3, 1, shade(mk, 140));
        fpx(&c, cx - mw - 2, my + 4, shade(mk, 70));
        fpx(&c, cx + mw + 2, my + 4, shade(mk, 70));
        fpx(&c, cx, my, RGB565C(120, 230, 255));        /* status led */
    }

    /* stubble / beard */
    if (!synth && fr_pct(&r, kind == NK_DOCKHAND ? 55 : 30) && species < 2
        && !breather) {
        uint16_t st = shade(hair, 60);
        for (int y = my - 2; y < cy + ry; y++) {
            float ny = (float)(y - cy) / (float)ry;
            if (ny > 1.0f) break;
            float w = (float)rx * sqrtf(1.0f - ny * ny);
            if (ny > 0.15f) w *= 1.0f - jaw * (ny - 0.15f) / 0.85f;
            for (int x = cx - (int)w; x <= cx + (int)w; x += 2)
                if (((x ^ y) & 3) == 0 && !(x > cx - mw && x < cx + mw &&
                                            y <= my + 1))
                    fpx(&c, x, y, st);
        }
    }

    /* markings: scar (asymmetric on purpose). Layering (user bug): a
     * scar lives on SKIN — helmets and visors cover that whole region,
     * so they suppress it; the rng rolls still burn either way so
     * every other feature keeps its seed. Beaks own the avian face. */
    {
        int scar = fr_pct(&r, kind == NK_PIRATE ? 70 : 12);
        int sside = fr_pct(&r, 50);
        if (scar && !helmet && !visor && species != 2) {
            int sx = cx + (sside ? -edx : edx);
            int sy0 = ey - eh - 2, sy1 = nly;
            for (int y = sy0; y <= sy1; y++)
                fpx(&c, sx + (y - sy0) / 3, y, shade(skin, 135));
        }
    }

    /* hood overlay: frames the face last */
    if (hood) {
        uint16_t hc = RGB565C(44, 38, 58), hd = shade(RGB565C(44, 38, 58), 60);
        for (int y = 0; y < size; y++) {
            float ny = (float)(y - cy) / (float)(ry + 3);
            float w = (ny < 1.0f && ny > -1.0f)
                          ? (float)(rx + 2) * sqrtf(1.0f - ny * ny)
                          : (y > cy ? (float)(rx + 2) : 0);
            int wi = (int)w;
            if (y < cy - ry + (int)(S * 0.10f)) wi = -1;   /* cowl top */
            fspan(&c, 0, cx - wi - 1, y, y < cy ? hc : hd);
            fspan(&c, cx + wi + 1, size - 1, y, y < cy ? hc : hd);
        }
    }

    /* frame */
    fspan(&c, 0, size - 1, 0, RGB565C(70, 90, 120));
    fspan(&c, 0, size - 1, size - 1, RGB565C(70, 90, 120));
    for (int y = 0; y < size; y++) {
        fpx(&c, 0, y, RGB565C(70, 90, 120));
        fpx(&c, size - 1, y, RGB565C(70, 90, 120));
    }
}
