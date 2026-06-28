/*
 * ThumbyCraft — text rendering.
 *
 * Glyph table transcribed from Pemsa (https://github.com/egordorichev/pemsa,
 * MIT license — see ThumbyNES/ThumbyDOOM for full attribution chain).
 * Encoding (per glyph, 16-bit little-endian):
 *   bit  0..2  = pixels at row 0 (LSB = leftmost)
 *   bit  3..5  = pixels at row 1
 *   bit  6..8  = pixels at row 2
 *   bit  9..11 = pixels at row 3
 *   bit 12..14 = pixels at row 4
 */
#include "craft_font.h"

static const uint16_t glyph_unknown = 0x7fff;

static const uint16_t font[256] = {
    [' '] = 0x0000,
    [ 33] = 0x2092, [ 34] = 0x002d, [ 35] = 0x5f7d, [ 36] = 0x2f9f,
    [ 37] = 0x52a5, [ 38] = 0x7adb, [ 39] = 0x0012, [ 40] = 0x224a,
    [ 41] = 0x2922, [ 42] = 0x55d5, [ 43] = 0x05d0, [ 44] = 0x1400,
    [ 45] = 0x01c0, [ 46] = 0x2000, [ 47] = 0x1494,
    [ 48] = 0x7b6f, [ 49] = 0x7493, [ 50] = 0x73e7, [ 51] = 0x79a7,
    [ 52] = 0x49ed, [ 53] = 0x79cf, [ 54] = 0x7bc9, [ 55] = 0x4927,
    [ 56] = 0x7bef, [ 57] = 0x49ef,
    [ 58] = 0x0410, [ 59] = 0x1410, [ 60] = 0x4454, [ 61] = 0x0e38,
    [ 62] = 0x1511, [ 63] = 0x21a7, [ 64] = 0x636a,
    [ 65] = 0x5bef, [ 66] = 0x7aef, [ 67] = 0x624e, [ 68] = 0x7b6b,
    [ 69] = 0x72cf, [ 70] = 0x12cf, [ 71] = 0x7a4e, [ 72] = 0x5bed,
    [ 73] = 0x7497, [ 74] = 0x3497, [ 75] = 0x5aed, [ 76] = 0x7249,
    [ 77] = 0x5b7f, [ 78] = 0x5b6b, [ 79] = 0x3b6e, [ 80] = 0x13ef,
    [ 81] = 0x676a, [ 82] = 0x5aef, [ 83] = 0x39ce, [ 84] = 0x2497,
    [ 85] = 0x6b6d, [ 86] = 0x2b6d, [ 87] = 0x7f6d, [ 88] = 0x5aad,
    [ 89] = 0x79ed, [ 90] = 0x72a7,
    [ 91] = 0x324b, [ 92] = 0x4491, [ 93] = 0x6926, [ 94] = 0x002a,
    [ 95] = 0x7000, [ 96] = 0x0022,
    [ 97] = 0x5f78, [ 98] = 0x7ad8, [ 99] = 0x7278, [100] = 0x3b58,
    [101] = 0x72f8, [102] = 0x12f8, [103] = 0x7a78, [104] = 0x5f68,
    [105] = 0x74b8, [106] = 0x34b8, [107] = 0x5ae8, [108] = 0x7248,
    [109] = 0x5bf8, [110] = 0x5b58, [111] = 0x3b70, [112] = 0x1f78,
    [113] = 0x6750, [114] = 0x5778, [115] = 0x3870, [116] = 0x24b8,
    [117] = 0x6b68, [118] = 0x2f68, [119] = 0x7f68, [120] = 0x5aa8,
    [121] = 0x79e8, [122] = 0x7338,
    [123] = 0x64d6, [124] = 0x2492, [125] = 0x3593, [126] = 0x03e0,
    [127] = 0x0550,
};

/* Render-target dimensions for the glyph writer. The HUD can draw text into
 * a smaller upscale overlay, so the stride/bounds must follow that target and
 * not the full framebuffer. On the RP2350/host (CRAFT_HUD_SCALE==1) these stay
 * compile-time constants == the framebuffer, so the writer is byte-identical
 * and craft_font_set_target is a no-op. */
#if CRAFT_HUD_SCALE > 1
static int s_font_w = CRAFT_FB_W;
static int s_font_h = CRAFT_FB_H;
void craft_font_set_target(int w, int h) { s_font_w = w; s_font_h = h; }
#define FONT_TW s_font_w
#define FONT_TH s_font_h
#else
void craft_font_set_target(int w, int h) { (void)w; (void)h; }
#define FONT_TW CRAFT_FB_W
#define FONT_TH CRAFT_FB_H
#endif

static inline void put(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)FONT_TW && (unsigned)y < (unsigned)FONT_TH)
        fb[y * FONT_TW + x] = c;
}

int craft_font_draw(uint16_t *fb, const char *text, int x, int y, uint16_t color) {
    if (!text || !fb) return x;
    int cur_x = x, cur_y = y;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '\n') { cur_x = x; cur_y += CRAFT_FONT_CELL_H; continue; }
        uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
        for (int row = 0; row < 5; row++) {
            int bits = (g >> (row * 3)) & 0x7;
            if (bits & 0x1) put(fb, cur_x + 0, cur_y + row, color);
            if (bits & 0x2) put(fb, cur_x + 1, cur_y + row, color);
            if (bits & 0x4) put(fb, cur_x + 2, cur_y + row, color);
        }
        cur_x += CRAFT_FONT_CELL_W;
    }
    return cur_x;
}

int craft_font_draw_2x(uint16_t *fb, const char *text, int x, int y, uint16_t color) {
    if (!text || !fb) return x;
    int cur_x = x, cur_y = y;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '\n') { cur_x = x; cur_y += CRAFT_FONT_CELL_H * 2; continue; }
        uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
        for (int row = 0; row < 5; row++) {
            int bits = (g >> (row * 3)) & 0x7;
            for (int col = 0; col < 3; col++) {
                if (!(bits & (1 << col))) continue;
                int px = cur_x + col * 2;
                int py = cur_y + row * 2;
                put(fb, px,     py,     color);
                put(fb, px + 1, py,     color);
                put(fb, px,     py + 1, color);
                put(fb, px + 1, py + 1, color);
            }
        }
        cur_x += CRAFT_FONT_CELL_W * 2;
    }
    return cur_x;
}

static inline uint16_t rgb_lerp(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Big title text: each glyph pixel becomes an S×S block, drawn with a vertical
 * gradient (top→bot over the glyph height) and wrapped in a 1px outline. Two
 * passes (all outlines, then all fills) so neighbouring glyph pixels don't let
 * the outline bleed over the fill. Returns the advanced x. */
int craft_font_draw_title(uint16_t *fb, const char *text, int x, int y, int s,
                          uint16_t top, uint16_t bot, uint16_t outline) {
    if (!text || !fb || s < 1) return x;
    int h = 5 * s;
    for (int pass = 0; pass < 2; pass++) {
        int cx = x;
        for (const char *t = text; *t; t++) {
            unsigned char ch = (unsigned char)*t;
            uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
            for (int row = 0; row < 5; row++) {
                int bits = (g >> (row * 3)) & 0x7;
                for (int col = 0; col < 3; col++) {
                    if (!(bits & (1 << col))) continue;
                    int gx = cx + col * s, gy = y + row * s;
                    if (pass == 0) {                 /* outline */
                        for (int yy = -1; yy <= s; yy++)
                            for (int xx = -1; xx <= s; xx++)
                                put(fb, gx + xx, gy + yy, outline);
                    } else {                          /* gradient fill */
                        for (int yy = 0; yy < s; yy++) {
                            float tt = (float)(row * s + yy) / (float)(h - 1);
                            uint16_t c = rgb_lerp(top, bot, tt);
                            for (int xx = 0; xx < s; xx++)
                                put(fb, gx + xx, gy + yy, c);
                        }
                    }
                }
            }
            cx += CRAFT_FONT_CELL_W * s;
        }
    }
    return x + craft_font_width(text) * s;
}

int craft_font_width(const char *text) {
    if (!text) return 0;
    int n = 0;
    for (; *text; text++) if (*text != '\n') n++;
    return n * CRAFT_FONT_CELL_W;
}
int craft_font_width_2x(const char *text) {
    return craft_font_width(text) * 2;
}

/* Fractional nearest-neighbour scale (e.g. num=3,den=2 -> 1.5x: a 3x5 glyph
 * becomes ~5x8 — a readable size between 1x and 2x). */
int craft_font_draw_frac(uint16_t *fb, const char *text, int x, int y,
                         int num, int den, uint16_t color) {
    if (!text || !fb || num < 1 || den < 1) return x;
    int cur_x = x, cur_y = y;
    int ow = (3 * num + den - 1) / den, oh = (5 * num + den - 1) / den;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '\n') { cur_x = x; cur_y += (CRAFT_FONT_CELL_H * num) / den; continue; }
        uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
        for (int oy = 0; oy < oh; oy++) {
            int sy = (oy * den) / num; if (sy > 4) sy = 4;
            int bits = (g >> (sy * 3)) & 0x7;
            for (int ox = 0; ox < ow; ox++) {
                int sx = (ox * den) / num; if (sx > 2) sx = 2;
                if (bits & (1 << sx)) put(fb, cur_x + ox, cur_y + oy, color);
            }
        }
        cur_x += (CRAFT_FONT_CELL_W * num) / den;
    }
    return cur_x;
}
int craft_font_width_frac(const char *text, int num, int den) {
    if (!text) return 0;
    int n = 0; for (; *text; text++) if (*text != '\n') n++;
    return n * (CRAFT_FONT_CELL_W * num) / den;
}

static uint16_t cf_lerp(uint16_t a, uint16_t b, int t /*0..256*/) {
    int ar=(a>>11)&31, ag=(a>>5)&63, ab=a&31;
    int br=(b>>11)&31, bg=(b>>5)&63, bb=b&31;
    int r=ar+(((br-ar)*t)>>8), g=ag+(((bg-ag)*t)>>8), bl=ab+(((bb-ab)*t)>>8);
    return (uint16_t)((r<<11)|(g<<5)|bl);
}
/* Anti-aliased fractional scale: supersample each output pixel's coverage of
 * the 3x5 glyph and alpha-blend `color` onto whatever is already in fb. Far
 * smoother/more legible than the nearest-neighbour craft_font_draw_frac at
 * non-integer scales (e.g. 1.5x). */
int craft_font_draw_frac_aa(uint16_t *fb, const char *text, int x, int y,
                            int num, int den, uint16_t color) {
    if (!text || !fb || num < 1 || den < 1) return x;
    const int SS = 3;
    int ow = (3 * num + den - 1) / den, oh = (5 * num + den - 1) / den;
    int cur_x = x;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '\n') { cur_x = x; y += (CRAFT_FONT_CELL_H * num) / den; continue; }
        uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
        for (int oy = 0; oy < oh; oy++) {
            for (int ox = 0; ox < ow; ox++) {
                int hit = 0;
                for (int sj = 0; sj < SS; sj++) {
                    int sr = ((oy * SS + sj) * den) / (num * SS);
                    if (sr < 0 || sr > 4) continue;
                    int bits = (g >> (sr * 3)) & 0x7;
                    for (int si = 0; si < SS; si++) {
                        int sc = ((ox * SS + si) * den) / (num * SS);
                        if (sc >= 0 && sc < 3 && (bits & (1 << sc))) hit++;
                    }
                }
                if (!hit) continue;
                int px = cur_x + ox, py = y + oy;
                if ((unsigned)px < (unsigned)FONT_TW && (unsigned)py < (unsigned)FONT_TH) {
                    int t = (hit * 256) / (SS * SS);
                    uint16_t *d = &fb[py * FONT_TW + px];
                    *d = cf_lerp(*d, color, t > 256 ? 256 : t);
                }
            }
        }
        cur_x += (CRAFT_FONT_CELL_W * num) / den;
    }
    return cur_x;
}
