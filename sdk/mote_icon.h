/*
 * Mote — compact launcher-icon codec (paletted, adaptive bit depth).
 *
 * A raw 60x60 RGB565 icon is 7,200 bytes in flash. From ABI v22 a game's
 * `mote_game_icon_data[]` is instead this compact BLOB, decoded by the launcher
 * into a scratch buffer before it's blitted:
 *
 *   [0] u8  bpp        bits per index: 1, 2, 4 or 8
 *   [1] u8  reserved   (0)
 *   [2] u16 npal       palette entry count (1..256), little-endian
 *   [4] u16 pal[npal]  RGB565 palette, little-endian
 *       u8  idx[...]   ceil(W*H*bpp/8) packed indices, MSB-first within a byte
 *
 * Pixel-art icons (few colours) pack to ~1.8 KB at 4bpp; busier icons quantise to
 * 256 colours (8bpp, ~4.1 KB). Decode is a palette lookup — no decompressor.
 *
 * All multi-byte reads/writes are byte-wise so the blob needs no alignment.
 */
#ifndef MOTE_ICON_CODEC_H   /* not MOTE_ICON_H — that's the icon-height macro in mote_catalog.h */
#define MOTE_ICON_CODEC_H
#include <stdint.h>

static inline int mote_icon_bpp_for(int npal) { return npal <= 2 ? 1 : npal <= 4 ? 2 : npal <= 16 ? 4 : 8; }

/* Decode a blob into `out` (count = W*H RGB565 pixels). Safe on unaligned blobs. */
static inline void mote_icon_decode(const void *blob, uint16_t *out, int count) {
    const uint8_t *b = (const uint8_t *)blob;
    int bpp = b[0]; int npal = b[2] | (b[3] << 8);
    const uint8_t *pal = b + 4;
    const uint8_t *idx = pal + npal * 2;
    if (bpp == 8) {
        for (int i = 0; i < count; i++) { int v = idx[i]; out[i] = (uint16_t)(pal[v*2] | (pal[v*2+1] << 8)); }
    } else {
        int per = 8 / bpp, mask = (1 << bpp) - 1;
        for (int i = 0; i < count; i++) {
            int byte = idx[i / per], sh = (per - 1 - (i % per)) * bpp, v = (byte >> sh) & mask;
            out[i] = (uint16_t)(pal[v*2] | (pal[v*2+1] << 8));
        }
    }
}

#ifndef MOTE_ICON_NO_ENCODE
#include <string.h>
/* Encode `count` RGB565 pixels into `out` (caller gives a buffer >= count*2 + 516).
 * Lossless when the image has <=256 distinct colours; otherwise quantises to a
 * 256-entry RGB332 palette. Returns the blob length in bytes. */
static inline int mote_icon_encode(const uint16_t *px, int count, uint8_t *out) {
    uint16_t pal[256]; int npal = 0; int over = 0;
    static uint8_t idx[60*60];                 /* W*H index scratch (icons are 60x60) */
    if (count > (int)sizeof idx) count = (int)sizeof idx;
    for (int i = 0; i < count; i++) {
        uint16_t c = px[i]; int j = 0;
        for (; j < npal; j++) if (pal[j] == c) break;
        if (j < npal) { idx[i] = (uint8_t)j; }
        else if (npal < 256) { pal[npal] = c; idx[i] = (uint8_t)npal++; }
        else { over = 1; break; }
    }
    if (over) {                                /* >256 colours: RGB332 quantise */
        npal = 256;
        for (int q = 0; q < 256; q++) {
            int r3 = (q >> 5) & 7, g3 = (q >> 2) & 7, b2 = q & 3;
            int r5 = (r3 << 2) | (r3 >> 1), g6 = (g3 << 3) | (g3 >> 0), b5 = (b2 << 3) | (b2 << 1);
            pal[q] = (uint16_t)((r5 << 11) | ((g6 & 63) << 5) | b5);
        }
        for (int i = 0; i < count; i++) {
            uint16_t c = px[i]; int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
            idx[i] = (uint8_t)(((r5 >> 2) << 5) | ((g6 >> 3) << 2) | (b5 >> 3));
        }
    }
    int bpp = mote_icon_bpp_for(npal);
    uint8_t *o = out;
    *o++ = (uint8_t)bpp; *o++ = 0; *o++ = (uint8_t)(npal & 0xFF); *o++ = (uint8_t)(npal >> 8);
    for (int j = 0; j < npal; j++) { *o++ = (uint8_t)(pal[j] & 0xFF); *o++ = (uint8_t)(pal[j] >> 8); }
    if (bpp == 8) { for (int i = 0; i < count; i++) *o++ = idx[i]; }
    else {
        int per = 8 / bpp; uint8_t acc = 0; int filled = 0;
        for (int i = 0; i < count; i++) {
            acc = (uint8_t)((acc << bpp) | (idx[i] & ((1 << bpp) - 1)));
            if (++filled == per) { *o++ = acc; acc = 0; filled = 0; }
        }
        if (filled) { acc <<= (per - filled) * bpp; *o++ = acc; }
    }
    return (int)(o - out);
}
#endif /* MOTE_ICON_NO_ENCODE */

#endif /* MOTE_ICON_CODEC_H */
