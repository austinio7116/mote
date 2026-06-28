/*
 * tex_embed.h — shared PNG -> RGB565 MoteImage emitter for the mesh bakers
 * (obj2mesh / stl2mesh / obj2rig). Single-header, include from exactly the
 * tools that need it; depends on stb_image.h being on the include path
 * (compiled with `-I studio/third_party`).
 *
 * tex_embed() loads a PNG, box-downsamples it to at most TEX_MAX_DIM on each
 * axis, encodes RGB565 row-major (top-left origin, matching the engine's
 * tpix[v*tw+u] sampling), and writes:
 *     static const uint16_t <name>_texpix[w*h] = { ... };
 *     static const MoteImage <name>_tex = { <name>_texpix, w, h, 0xF81F, 1 };
 * Returns 1 on success (and sets *out_w/*out_h to the emitted size, and
 * *out_avg565 to the texture's average RGB565 — the bakers store that as the
 * mesh's flat `color` so a textured mesh still shows if drawn without a
 * textured-tri pool; any of the out-pointers may be NULL), 0 if the PNG could
 * not be loaded. The image is emitted opaque (opaque=1) — every texel
 * is drawn; the 0xF81F key is only honoured by the engine when opaque==0, so it
 * is stored as a harmless default. Source alpha is ignored (RGB averaged).
 *
 * Texture-size cap: 64x64. Rationale — the Mesh face_uvs are uint8 (0..255)
 * spanning the texture, so anything past 256px loses UV precision; 64x64 RGB565
 * is 8 KB of flash per texture, ample for a 128x128 screen and easy on the
 * RP2350's 2 MB flash. PNGs larger than the cap are box-averaged down.
 */
#ifndef TEX_EMBED_H
#define TEX_EMBED_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* Each mesh-baker tool is its own translation unit and includes this header
 * exactly once, so it carries the stb_image implementation. Guarded so a TU
 * that already pulled in the implementation (e.g. the Studio) won't redefine. */
#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "stb_image.h"

#define TEX_MAX_DIM 64

/* RGB888 -> RGB565 */
static inline uint16_t tex_565(int r, int g, int b) {
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Emit <name>_texpix[] + <name>_tex from `pngpath`. Returns 1 on success. */
static int tex_embed(FILE *h, const char *name, const char *pngpath,
                     int *out_w, int *out_h, int *out_avg565) {
    int w, h_, n;
    unsigned char *d = stbi_load(pngpath, &w, &h_, &n, 4);  /* RGBA */
    if (!d) { fprintf(stderr, "tex_embed: cannot load %s\n", pngpath); return 0; }

    /* target size: shrink to fit TEX_MAX_DIM, preserving the larger axis */
    int tw = w, th = h_;
    if (tw > TEX_MAX_DIM || th > TEX_MAX_DIM) {
        if (tw >= th) { th = (int)((long)th * TEX_MAX_DIM / tw); tw = TEX_MAX_DIM; }
        else          { tw = (int)((long)tw * TEX_MAX_DIM / th); th = TEX_MAX_DIM; }
        if (tw < 1) tw = 1; if (th < 1) th = 1;
    }

    uint16_t *px = (uint16_t *)malloc((size_t)tw * th * sizeof(uint16_t));
    if (!px) { stbi_image_free(d); return 0; }

    /* box-average each destination texel over its source footprint */
    for (int y = 0; y < th; y++) {
        int sy0 = (int)((long)y * h_ / th), sy1 = (int)((long)(y + 1) * h_ / th);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = (int)((long)x * w / tw), sx1 = (int)((long)(x + 1) * w / tw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            long rs = 0, gs = 0, bs = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *p = &d[((size_t)sy * w + sx) * 4];
                    rs += p[0]; gs += p[1]; bs += p[2]; cnt++;
                }
            if (cnt < 1) cnt = 1;
            px[y * tw + x] = tex_565((int)(rs / cnt), (int)(gs / cnt), (int)(bs / cnt));
        }
    }
    stbi_image_free(d);

    fprintf(h, "static const uint16_t %s_texpix[%d] = {\n", name, tw * th);
    for (int i = 0; i < tw * th; i++)
        fprintf(h, "0x%04X,%s", px[i], (i % 12 == 11 || i == tw * th - 1) ? "\n" : "");
    fprintf(h, "};\n");
    fprintf(h, "static const MoteImage %s_tex = { %s_texpix, %d, %d, 0xF81F, 1 };\n",
            name, name, tw, th);

    /* Average colour (RGB565) over the texels — the bakers store this as the
     * mesh's flat `color` so a textured mesh rendered without a textured-tri
     * pool (MoteConfig.max_tex_tris == 0) falls back to a sensible flat shade
     * instead of black/invisible. */
    if (out_avg565) {
        long ar = 0, ag = 0, ab = 0; int npx = tw * th; if (npx < 1) npx = 1;
        for (int i = 0; i < tw * th; i++) {
            uint16_t p = px[i];
            ar += (p >> 11) & 0x1F; ag += (p >> 5) & 0x3F; ab += p & 0x1F;
        }
        *out_avg565 = (int)(((ar / npx) << 11) | ((ag / npx) << 5) | (ab / npx));
    }

    free(px);
    if (out_w) *out_w = tw;
    if (out_h) *out_h = th;
    return 1;
}

#endif /* TEX_EMBED_H */
