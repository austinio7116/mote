/*
 * Mote — text rendering (ported from ThumbyCraft craft_font).
 *
 * Glyph table transcribed from Pemsa (https://github.com/egordorichev/pemsa,
 * MIT). Encoding (per glyph, 16-bit little-endian): bits 0..2 = row 0 pixels
 * (LSB leftmost), 3..5 = row 1, ... 12..14 = row 4.
 */
#include "mote_font.h"
#include "../core/mote_config.h"

#define TW MOTE_FB_W
#define TH MOTE_FB_H

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

static inline void put(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)TW && (unsigned)y < (unsigned)TH)
        fb[y * TW + x] = c;
}

int mote_font_draw(uint16_t *fb, const char *text, int x, int y, uint16_t color) {
    if (!text || !fb) return x;
    int cur_x = x, cur_y = y;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '\n') { cur_x = x; cur_y += MOTE_FONT_CELL_H; continue; }
        uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
        for (int row = 0; row < 5; row++) {
            int bits = (g >> (row * 3)) & 0x7;
            if (bits & 0x1) put(fb, cur_x + 0, cur_y + row, color);
            if (bits & 0x2) put(fb, cur_x + 1, cur_y + row, color);
            if (bits & 0x4) put(fb, cur_x + 2, cur_y + row, color);
        }
        cur_x += MOTE_FONT_CELL_W;
    }
    return cur_x;
}

int mote_font_draw_2x(uint16_t *fb, const char *text, int x, int y, uint16_t color) {
    if (!text || !fb) return x;
    int cur_x = x, cur_y = y;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '\n') { cur_x = x; cur_y += MOTE_FONT_CELL_H * 2; continue; }
        uint16_t g = (ch < 128) ? font[ch] : glyph_unknown;
        for (int row = 0; row < 5; row++) {
            int bits = (g >> (row * 3)) & 0x7;
            for (int col = 0; col < 3; col++) {
                if (!(bits & (1 << col))) continue;
                int px = cur_x + col * 2, py = cur_y + row * 2;
                put(fb, px,     py,     color);
                put(fb, px + 1, py,     color);
                put(fb, px,     py + 1, color);
                put(fb, px + 1, py + 1, color);
            }
        }
        cur_x += MOTE_FONT_CELL_W * 2;
    }
    return cur_x;
}

int mote_font_width(const char *text) {
    int w = 0, run = 0;
    for (; text && *text; text++) {
        if (*text == '\n') { if (run > w) w = run; run = 0; }
        else run += MOTE_FONT_CELL_W;
    }
    return run > w ? run : w;
}

int mote_font_width_2x(const char *text) { return mote_font_width(text) * 2; }

/* --- ABI v39: anti-aliased proportional font ------------------------------- */
int mote_font_draw_aa(uint16_t *fb, const MoteFont *f, const char *s, int x, int y, uint16_t color) {
    if (!f || !f->glyphs || !f->cov) return x;
    const int cr = (color >> 11) & 0x1F, cg = (color >> 5) & 0x3F, cb = color & 0x1F;
    const int bpp = f->bpp ? f->bpp : 4, maxv = (1 << bpp) - 1;   /* 1/2/4-bit coverage */
    int pen = x, line = y;
    for (; s && *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n') { pen = x; line += f->line_h ? f->line_h : 1; continue; }
        if (ch < f->first || ch >= f->first + f->count) continue;
        const MoteGlyph *g = &f->glyphs[ch - f->first];
        /* coverage is packed MSB-first at `bpp` bits/pixel (row-major, byte-aligned
         * per glyph): 1-bit binary, 2-bit (4 levels) or 4-bit (16 levels). */
        const uint8_t *cov = f->cov + g->off;
        for (int gy = 0; gy < g->h; ++gy) {
            int py = line + g->yoff + gy;
            if (py < 0 || py >= TH) continue;
            uint16_t *row = fb + py * TW;
            int base = gy * g->w;
            for (int gx = 0; gx < g->w; ++gx) {
                int bitpos = (base + gx) * bpp;
                int v = (cov[bitpos >> 3] >> (8 - bpp - (bitpos & 7))) & maxv;
                if (!v) continue;
                int px = pen + g->xoff + gx;
                if (px < 0 || px >= TW) continue;
                if (v >= maxv) { row[px] = color; continue; }     /* opaque: skip the blend */
                int a = v * 255 / maxv;                           /* coverage -> 0..255 */
                uint16_t d = row[px];
                int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
                dr += ((cr - dr) * a) >> 8;
                dg += ((cg - dg) * a) >> 8;
                db += ((cb - db) * a) >> 8;
                row[px] = (uint16_t)((dr << 11) | (dg << 5) | db);
            }
        }
        pen += g->adv;
    }
    return pen;
}
int mote_font_aa_width(const MoteFont *f, const char *s) {
    if (!f || !f->glyphs) return 0;
    int run = 0, w = 0;
    for (; s && *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n') { if (run > w) w = run; run = 0; continue; }
        if (ch < f->first || ch >= f->first + f->count) continue;
        run += f->glyphs[ch - f->first].adv;
    }
    return run > w ? run : w;
}
