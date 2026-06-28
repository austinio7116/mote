/*
 * ThumbyElite — tiny procedural icons.
 *
 * 12x7 weapon glyphs drawn with pixel ops — distinct silhouettes per
 * family: laser emitters (barrel mass by size), beam dashes, photon orb,
 * gauss rail, autocannon twin barrels, missile darts (homing adds a
 * seeker arc). Bodies grey, muzzle/payload in the weapon's colour.
 */
#include "ui_icons.h"
#include "elite_types.h"
#include "elite_weapons.h"

#define BODY  RGB565C(150, 155, 170)
#define BODY2 RGB565C(100, 105, 122)

static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < ELITE_FB_W && (unsigned)y < ELITE_FB_H)
        fb[y * ELITE_FB_W + x] = c;
}
static void hbar(uint16_t *fb, int x0, int x1, int y, uint16_t c) {
    for (int x = x0; x <= x1; x++) px(fb, x, y, c);
}

void icon_weapon(uint16_t *fb, int x, int y, int wpn_type) {
    uint16_t hot = (wpn_type == EQ_SHIELD) ? RGB565C(80, 180, 255)
                 : (wpn_type == EQ_ARMOR) ? RGB565C(255, 140, 70)
                 : (wpn_type >= EQ_HEATSINK && wpn_type < ITEM_COUNT)
                       ? RGB565C(140, 255, 180)
                 : k_weapons[wpn_type].color;
    switch (wpn_type) {
    case WPN_PULSE_S:
        hbar(fb, x + 2, x + 7, y + 3, BODY);
        px(fb, x + 8, y + 3, hot);
        px(fb, x + 2, y + 4, BODY2);
        break;
    case WPN_PULSE_M:
        hbar(fb, x + 1, x + 7, y + 2, BODY);
        hbar(fb, x + 1, x + 7, y + 4, BODY);
        hbar(fb, x + 1, x + 7, y + 3, BODY2);
        px(fb, x + 8, y + 3, hot);
        px(fb, x + 9, y + 3, hot);
        break;
    case WPN_PULSE_L:
        for (int r = 1; r <= 5; r++) hbar(fb, x, x + 7, y + r, BODY);
        hbar(fb, x + 1, x + 6, y + 3, BODY2);
        px(fb, x + 8, y + 2, hot); px(fb, x + 9, y + 3, hot);
        px(fb, x + 8, y + 4, hot); px(fb, x + 10, y + 3, hot);
        break;
    case WPN_BEAM:
        hbar(fb, x, x + 5, y + 3, BODY);
        px(fb, x + 7, y + 3, hot); px(fb, x + 9, y + 3, hot);
        px(fb, x + 11, y + 3, hot);
        break;
    case WPN_PHOTON:
        hbar(fb, x, x + 4, y + 3, BODY);
        px(fb, x + 8, y + 2, hot); px(fb, x + 7, y + 3, hot);
        px(fb, x + 8, y + 3, RGB565C(255, 255, 255));
        px(fb, x + 9, y + 3, hot); px(fb, x + 8, y + 4, hot);
        break;
    case WPN_GAUSS:
        hbar(fb, x, x + 10, y + 3, BODY2);
        px(fb, x + 2, y + 2, BODY); px(fb, x + 5, y + 2, BODY);
        px(fb, x + 8, y + 2, BODY);
        px(fb, x + 2, y + 4, BODY); px(fb, x + 5, y + 4, BODY);
        px(fb, x + 8, y + 4, BODY);
        px(fb, x + 11, y + 3, hot);
        break;
    case WPN_AUTOCANNON:
        hbar(fb, x + 2, x + 8, y + 2, BODY);
        hbar(fb, x + 2, x + 8, y + 4, BODY);
        px(fb, x, y + 3, BODY2); px(fb, x + 1, y + 3, BODY2);
        px(fb, x + 9, y + 2, hot); px(fb, x + 9, y + 4, hot);
        break;
    case WPN_MISSILE:
        hbar(fb, x + 2, x + 8, y + 3, BODY);
        px(fb, x + 2, y + 2, BODY2); px(fb, x + 2, y + 4, BODY2);
        px(fb, x + 3, y + 2, BODY2); px(fb, x + 3, y + 4, BODY2);
        px(fb, x + 9, y + 3, hot); px(fb, x + 10, y + 3, hot);
        break;
    case WPN_FLAK:
        /* stub barrel + pellet spray */
        hbar(fb, x + 1, x + 5, y + 3, BODY);
        px(fb, x + 7, y + 1, hot); px(fb, x + 8, y + 3, hot);
        px(fb, x + 7, y + 5, hot); px(fb, x + 10, y + 2, hot);
        px(fb, x + 10, y + 4, hot);
        break;
    case WPN_RAILGUN:
        /* long twin rails + charge tip */
        hbar(fb, x, x + 9, y + 2, BODY);
        hbar(fb, x, x + 9, y + 4, BODY);
        px(fb, x + 1, y + 3, BODY2); px(fb, x + 4, y + 3, BODY2);
        px(fb, x + 7, y + 3, BODY2);
        px(fb, x + 10, y + 3, hot); px(fb, x + 11, y + 3, hot);
        break;
    case WPN_ION:
        /* emitter + arc rings */
        hbar(fb, x + 1, x + 4, y + 3, BODY);
        px(fb, x + 6, y + 2, hot); px(fb, x + 6, y + 4, hot);
        px(fb, x + 8, y + 1, hot); px(fb, x + 8, y + 5, hot);
        px(fb, x + 10, y + 3, hot);
        break;
    case WPN_MINE:
        /* spiky ball */
        px(fb, x + 4, y + 2, BODY); px(fb, x + 5, y + 2, BODY);
        px(fb, x + 3, y + 3, BODY); px(fb, x + 6, y + 3, BODY);
        px(fb, x + 4, y + 4, BODY); px(fb, x + 5, y + 4, BODY);
        px(fb, x + 4, y + 0, hot); px(fb, x + 1, y + 3, hot);
        px(fb, x + 8, y + 3, hot); px(fb, x + 4, y + 6, hot);
        break;
    case WPN_TRACTOR:
        /* dish + pull waves */
        px(fb, x + 1, y + 2, BODY); px(fb, x + 1, y + 4, BODY);
        px(fb, x + 2, y + 3, BODY);
        px(fb, x + 5, y + 3, hot); px(fb, x + 7, y + 2, hot);
        px(fb, x + 7, y + 4, hot); px(fb, x + 9, y + 1, hot);
        px(fb, x + 9, y + 5, hot);
        break;
    case WPN_LANCE:
        /* long violet lance with a phase shimmer */
        hbar(fb, x, x + 2, y + 3, BODY);
        hbar(fb, x + 3, x + 11, y + 3, hot);
        px(fb, x + 6, y + 2, BODY2); px(fb, x + 9, y + 4, BODY2);
        break;
    case WPN_BLASTER:
        /* bolts curving toward a mark */
        hbar(fb, x, x + 2, y + 4, BODY);
        px(fb, x + 4, y + 4, hot); px(fb, x + 7, y + 3, hot);
        px(fb, x + 9, y + 2, hot); px(fb, x + 11, y + 1, BODY2);
        break;
    case WPN_PLASMA:
        /* stub barrel + three plasma balls streaming out */
        hbar(fb, x, x + 3, y + 3, BODY);
        px(fb, x + 5, y + 3, hot); px(fb, x + 8, y + 3, hot);
        px(fb, x + 11, y + 3, hot);
        px(fb, x + 5, y + 2, BODY2); px(fb, x + 8, y + 4, BODY2);
        break;
    case WPN_MINING:
        /* stub emitter + ore chips */
        hbar(fb, x + 1, x + 5, y + 3, BODY);
        px(fb, x + 7, y + 3, hot);
        px(fb, x + 9, y + 1, BODY2); px(fb, x + 10, y + 4, BODY2);
        px(fb, x + 8, y + 5, BODY2); px(fb, x + 11, y + 2, BODY2);
        break;
    case EQ_SHIELD:
        /* arc shell + core */
        px(fb, x + 4, y + 1, hot); px(fb, x + 5, y + 1, hot);
        px(fb, x + 6, y + 1, hot);
        px(fb, x + 3, y + 2, hot); px(fb, x + 7, y + 2, hot);
        px(fb, x + 2, y + 3, hot); px(fb, x + 8, y + 3, hot);
        px(fb, x + 3, y + 4, hot); px(fb, x + 7, y + 4, hot);
        px(fb, x + 5, y + 3, BODY);
        break;
    case EQ_HEATSINK:
        /* finned radiator */
        for (int k = 0; k < 4; k++) {
            px(fb, x + 2 + k * 2, y + 1, BODY);
            px(fb, x + 2 + k * 2, y + 2, BODY);
            px(fb, x + 2 + k * 2, y + 3, BODY);
            px(fb, x + 2 + k * 2, y + 4, BODY2);
        }
        px(fb, x + 10, y + 2, hot);
        break;
    case EQ_SCANNER:
        /* radar sweep */
        px(fb, x + 5, y + 3, BODY);
        px(fb, x + 3, y + 1, hot); px(fb, x + 7, y + 1, hot);
        px(fb, x + 1, y + 3, hot); px(fb, x + 9, y + 3, hot);
        px(fb, x + 3, y + 5, hot); px(fb, x + 7, y + 5, hot);
        break;
    case EQ_TANK:
        /* twin bottles */
        for (int k = 0; k < 2; k++) {
            hbar(fb, x + 2 + k * 5, x + 4 + k * 5, y + 2, BODY);
            hbar(fb, x + 2 + k * 5, x + 4 + k * 5, y + 3, BODY);
            hbar(fb, x + 2 + k * 5, x + 4 + k * 5, y + 4, BODY2);
            px(fb, x + 3 + k * 5, y + 1, hot);
        }
        break;
    case EQ_FUELSCOOP:
        /* intake funnel */
        px(fb, x + 1, y + 1, hot); px(fb, x + 1, y + 5, hot);
        px(fb, x + 2, y + 2, BODY); px(fb, x + 2, y + 4, BODY);
        hbar(fb, x + 3, x + 8, y + 3, BODY);
        px(fb, x + 9, y + 3, BODY2);
        break;
    case EQ_TARGETCOMP:
        /* reticle chip */
        hbar(fb, x + 2, x + 8, y + 1, BODY2);
        hbar(fb, x + 2, x + 8, y + 5, BODY2);
        px(fb, x + 2, y + 2, BODY2); px(fb, x + 2, y + 4, BODY2);
        px(fb, x + 8, y + 2, BODY2); px(fb, x + 8, y + 4, BODY2);
        px(fb, x + 5, y + 3, hot);
        px(fb, x + 4, y + 3, hot); px(fb, x + 6, y + 3, hot);
        break;
    case EQ_CLOAK:
        /* fading ship outline: solid nose, dissolving tail */
        px(fb, x + 9, y + 3, BODY); px(fb, x + 8, y + 2, BODY);
        px(fb, x + 8, y + 4, BODY); px(fb, x + 6, y + 3, BODY2);
        px(fb, x + 4, y + 2, BODY2); px(fb, x + 2, y + 4, hot);
        px(fb, x, y + 2, hot);
        break;
    case EQ_MANIFEST:
        /* clipboard: frame + scan lines */
        hbar(fb, x + 2, x + 9, y, BODY);
        hbar(fb, x + 2, x + 9, y + 6, BODY);
        px(fb, x + 2, y + 2, BODY); px(fb, x + 2, y + 4, BODY);
        px(fb, x + 9, y + 2, BODY); px(fb, x + 9, y + 4, BODY);
        hbar(fb, x + 4, x + 7, y + 2, hot);
        hbar(fb, x + 4, x + 6, y + 4, BODY2);
        break;
    case EQ_DRONE:
        /* little R2: dome + body + tool arm */
        px(fb, x + 4, y, BODY2); px(fb, x + 5, y, BODY2);
        hbar(fb, x + 3, x + 6, y + 1, BODY);
        hbar(fb, x + 3, x + 6, y + 3, BODY);
        px(fb, x + 8, y + 2, hot); px(fb, x + 7, y + 2, BODY2);
        break;
    case EQ_CHAFF:
        /* burst spray */
        px(fb, x + 2, y + 3, BODY);
        px(fb, x + 5, y + 1, hot); px(fb, x + 6, y + 3, hot);
        px(fb, x + 5, y + 5, hot); px(fb, x + 8, y + 2, hot);
        px(fb, x + 8, y + 4, hot); px(fb, x + 10, y + 3, hot);
        break;
    case EQ_ARMOR:
        /* layered plates */
        hbar(fb, x + 1, x + 9, y + 1, BODY);
        hbar(fb, x + 2, x + 10, y + 3, BODY2);
        hbar(fb, x + 1, x + 9, y + 5, BODY);
        px(fb, x + 11, y + 3, hot);
        break;
    case WPN_HOMING:
        hbar(fb, x + 2, x + 7, y + 3, BODY);
        px(fb, x + 2, y + 2, BODY2); px(fb, x + 2, y + 4, BODY2);
        px(fb, x + 8, y + 3, hot);
        /* Seeker arc. */
        px(fb, x + 10, y + 1, hot); px(fb, x + 11, y + 3, hot);
        px(fb, x + 10, y + 5, hot);
        break;
    default:
        break;
    }
}

void icon_weapon_2x(uint16_t *fb, int x, int y, int wpn_type) {
    /* Rasterise the 1x glyph into a scratch strip (fb-width stride so
     * the px() maths inside icon_weapon stays valid), then 2x upscale. */
    static uint16_t strip[ELITE_FB_W * 8];
    const uint16_t SENTINEL = 0x0821;
    for (int i = 0; i < ELITE_FB_W * 8; i++) strip[i] = SENTINEL;
    icon_weapon(strip, 0, 0, wpn_type);
    for (int sy = 0; sy < 7; sy++) {
        for (int sx = 0; sx < 12; sx++) {
            uint16_t c = strip[sy * ELITE_FB_W + sx];
            if (c == SENTINEL) continue;
            int dx = x + sx * 2, dy = y + sy * 2;
            if ((unsigned)(dx + 1) >= ELITE_FB_W) continue;
            if ((unsigned)(dy + 1) >= ELITE_FB_H) continue;
            fb[dy * ELITE_FB_W + dx] = c;
            fb[dy * ELITE_FB_W + dx + 1] = c;
            fb[(dy + 1) * ELITE_FB_W + dx] = c;
            fb[(dy + 1) * ELITE_FB_W + dx + 1] = c;
        }
    }
}
