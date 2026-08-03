/*
 * CueVR — anti-aliased proportional text at headset resolution.
 *
 * The handheld's font is 3x5 pixels in a 4x6 cell. That is a good font for a
 * 128x128 screen held at arm's length and a bad one on a panel you can walk up
 * to: there is no detail in a 3x5 glyph to recover by drawing it bigger, so
 * scaling it up gives smooth blocks and nothing more. This draws real Audiowide,
 * rasterised by tools/ttf2font at the size it will actually be seen at, with
 * 4-bit coverage per pixel.
 *
 * It is a local copy of mote_font_draw_aa's inner loop for one reason: that one
 * addresses its target through the compile-time MOTE_FB_W/H, and the HUD here is
 * 512x512 rather than the console's framebuffer. Same coverage format, same
 * MoteFont data, runtime dimensions.
 */
#include "cuevr_text.h"

int cuevr_text_draw(uint16_t *fb, int tw, int th, const MoteFont *f,
                    const char *s, int x, int y, uint16_t colour)
{
    if (!fb || !f || !f->glyphs || !f->cov || !s) return x;
    const int cr = (colour >> 11) & 0x1F, cg = (colour >> 5) & 0x3F, cb = colour & 0x1F;
    const int bpp = f->bpp ? f->bpp : 4, maxv = (1 << bpp) - 1;
    int pen = x, line = y;

    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n') { pen = x; line += f->line_h ? f->line_h : 1; continue; }
        if (ch < f->first || ch >= f->first + f->count) continue;
        const MoteGlyph *g = &f->glyphs[ch - f->first];
        const uint8_t *cov = f->cov + g->off;
        for (int gy = 0; gy < g->h; ++gy) {
            int py = line + g->yoff + gy;
            if (py < 0 || py >= th) continue;
            uint16_t *row = fb + (size_t)py * tw;
            int base = gy * g->w;
            for (int gx = 0; gx < g->w; ++gx) {
                int bitpos = (base + gx) * bpp;
                int v = (cov[bitpos >> 3] >> (8 - bpp - (bitpos & 7))) & maxv;
                if (!v) continue;
                int px = pen + g->xoff + gx;
                if (px < 0 || px >= tw) continue;
                if (v >= maxv) { row[px] = colour; continue; }
                int a = v * 255 / maxv;
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

int cuevr_text_width(const MoteFont *f, const char *s) {
    if (!f || !f->glyphs || !s) return 0;
    int w = 0, best = 0;
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n') { if (w > best) best = w; w = 0; continue; }
        if (ch < f->first || ch >= f->first + f->count) continue;
        w += f->glyphs[ch - f->first].adv;
    }
    return w > best ? w : best;
}
