/*
 * CueVR — does the GLB reader read a GLB?
 *
 * This one earns its keep more than most. The models it parses arrive from
 * XR_FB_render_model, which only exists on a headset, so the first time the
 * real code path runs is the first time it runs on hardware with no debugger —
 * and a parser that is subtly wrong there shows up as a controller that is
 * inside-out, or nothing at all, with nothing to look at.
 *
 * So the test builds GLBs by hand, byte by byte, and asserts what comes back:
 * the container, the JSON, strided and offset accessors, indices of each width,
 * the node hierarchy baked into the vertices, materials, an embedded PNG — and
 * that malformed input is REFUSED rather than read past.
 *
 *   cc -I. -I../../studio/third_party -o /tmp/test_glb test_glb.c cuevr_glb.c -lm
 */
#include "cuevr_glb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

static int fails;
static void check(int cond, const char *what) {
    printf(cond ? "ok   %s\n" : "FAIL %s\n", what);
    if (!cond) fails++;
}
static void near(float got, float want, const char *what) {
    int ok = fabsf(got - want) < 1e-4f;
    printf(ok ? "ok   %s\n" : "FAIL %s (got %.5f want %.5f)\n", what, got, want);
    if (!ok) fails++;
}

/* ---- building a GLB ------------------------------------------------------ */

typedef struct { uint8_t *b; uint32_t n, cap; } Buf;

static void put(Buf *b, const void *p, uint32_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->b = realloc(b->b, b->cap);
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(b, x, 4);
}
static void putf(Buf *b, float f) { put(b, &f, 4); }
static void put16(Buf *b, uint16_t v) { put(b, &v, 2); }

/* Wrap a JSON string and a BIN blob into a GLB. */
static uint8_t *glb(const char *json, const uint8_t *bin, uint32_t binlen,
                    uint32_t *out_len) {
    uint32_t jl = (uint32_t)strlen(json);
    uint32_t jpad = (4 - (jl & 3)) & 3;
    uint32_t bpad = (4 - (binlen & 3)) & 3;
    Buf g = { 0, 0, 0 };
    put32(&g, 0x46546C67u);                       /* 'glTF' */
    put32(&g, 2);
    put32(&g, 12 + 8 + jl + jpad + (binlen ? 8 + binlen + bpad : 0));
    put32(&g, jl + jpad);
    put32(&g, 0x4E4F534Au);                       /* JSON */
    put(&g, json, jl);
    for (uint32_t i = 0; i < jpad; i++) put(&g, " ", 1);
    if (binlen) {
        put32(&g, binlen + bpad);
        put32(&g, 0x004E4942u);                   /* BIN */
        put(&g, bin, binlen);
        for (uint32_t i = 0; i < bpad; i++) put(&g, "\0", 1);
    }
    *out_len = g.n;
    return g.b;
}

/* ---- a model to parse ---------------------------------------------------- */

/* A quad, under a parent that translates and a child that scales, with a
 * base-colour PNG. Everything the extension's subset can throw at us in one
 * file — including a strided position accessor, because interleaved vertex
 * buffers are what a real exporter emits and a tightly packed one would let a
 * stride bug through. */
static uint8_t *build_model(uint32_t *len, int *pngw, int *pngh) {
    Buf bin = { 0, 0, 0 };

    /* 0: POSITION, interleaved with a junk float so the stride is 16, not 12. */
    const float P[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
    for (int i = 0; i < 4; i++) {
        putf(&bin, P[i][0]); putf(&bin, P[i][1]); putf(&bin, P[i][2]);
        putf(&bin, -999.0f);                      /* padding the stride steps over */
    }
    uint32_t off_n = bin.n;
    for (int i = 0; i < 4; i++) { putf(&bin, 0); putf(&bin, 0); putf(&bin, 1); }
    uint32_t off_uv = bin.n;
    const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 4; i++) { putf(&bin, UV[i][0]); putf(&bin, UV[i][1]); }
    uint32_t off_i = bin.n;
    const uint16_t I[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) put16(&bin, I[i]);
    while (bin.n & 3) put(&bin, "\0", 1);

    /* A 4x2 PNG whose first pixel is a known colour. */
    *pngw = 4; *pngh = 2;
    uint8_t px[4 * 2 * 4];
    for (int i = 0; i < 4 * 2; i++) {
        px[i*4+0] = (uint8_t)(10 + i); px[i*4+1] = 200; px[i*4+2] = 30; px[i*4+3] = 255;
    }
    int plen = 0;
    unsigned char *png = stbi_write_png_to_mem(px, 4 * 4, 4, 2, 4, &plen);
    uint32_t off_png = bin.n;
    put(&bin, png, (uint32_t)plen);
    free(png);

    char json[2048];
    snprintf(json, sizeof json,
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":["
        "{\"translation\":[1,2,3],\"children\":[1]},"
        "{\"mesh\":0,\"scale\":[2.0,2.0,2.0]}"
      "],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"material\":0,\"mode\":4}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.5,2.5e-1,1,1],"
        "\"baseColorTexture\":{\"index\":0}}}],"
      "\"textures\":[{\"source\":0}],"
      "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}],"
      "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}"
      "],"
      "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"byteStride\":16},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%d}"
      "],"
      "\"buffers\":[{\"byteLength\":%u}]}",
      off_n, off_n, off_uv, off_i, off_png, plen, bin.n);

    uint8_t *out = glb(json, bin.b, bin.n, len);
    free(bin.b);
    return out;
}

int main(void) {
    uint32_t len = 0;
    int pw = 0, ph = 0;
    uint8_t *g = build_model(&len, &pw, &ph);

    CueVrGlbModel m;
    int rc = cuevr_glb_parse(g, len, &m);
    check(rc == 0, "a well-formed GLB parses");
    if (rc != 0) { printf("  (%s)\n", cuevr_glb_error()); return 1; }

    check(m.nv == 4, "four vertices");
    check(m.ni == 6, "six indices");
    check(m.nparts == 1, "one material, one part");

    /* The node hierarchy is BAKED: scale 2 under a translation of (1,2,3), so
     * the corner at (1,0,0) lands at (3,2,3). Getting the multiply order the
     * other way round would put it at (4,4,6) and look plausible. */
    near(m.v[1].p[0], 3.0f, "parent translation and child scale compose");
    near(m.v[1].p[1], 2.0f, "  y");
    near(m.v[1].p[2], 3.0f, "  z");
    near(m.v[2].p[0], 3.0f, "third corner x");
    near(m.v[2].p[1], 4.0f, "third corner y");

    /* Strided: if the stride were assumed tight, vertex 1 would read the -999
     * padding and land nowhere near. */
    check(m.v[1].p[0] > -100.0f, "byteStride is honoured");

    near(m.v[0].n[2], 1.0f, "normals survive the transform");
    near(m.v[2].uv[0], 1.0f, "texcoords come through");
    near(m.v[2].uv[1], 1.0f, "  v");

    check(m.idx[3] == 0 && m.idx[4] == 2 && m.idx[5] == 3, "indices in order");

    near(m.part[0].base[0], 0.5f,  "baseColorFactor");
    near(m.part[0].base[1], 0.25f, "  and its exponent notation");
    check(m.part[0].first_index == 0 && m.part[0].n_index == 6, "the part spans it");

    check(m.ntex == 1, "the embedded PNG was decoded");
    check(m.tex[0].w == pw && m.tex[0].h == ph, "at the right size");
    check(m.part[0].tex == 0, "and the part points at it");
    if (m.ntex == 1) {
        check(m.tex[0].px[0] == 10 && m.tex[0].px[1] == 200,
              "with the right pixels");
    }
    cuevr_glb_free(&m);

    /* ---- refusing what it should refuse ---- */
    CueVrGlbModel bad;
    check(cuevr_glb_parse(g, 8, &bad) != 0, "a truncated file is refused");
    check(cuevr_glb_parse(NULL, 0, &bad) != 0, "so is nothing at all");
    {
        uint8_t *c = malloc(len);
        memcpy(c, g, len);
        c[0] = 'X';
        check(cuevr_glb_parse(c, len, &bad) != 0, "a bad magic is refused");
        memcpy(c, g, len);
        /* Claim the file is longer than it is: the chunk walk must notice. */
        c[8] = 0xFF; c[9] = 0xFF;
        check(cuevr_glb_parse(c, len, &bad) != 0, "a lying header length is refused");
        free(c);
    }
    {
        /* An accessor whose count runs off the end of its bufferView. This is
         * the one that matters: getting it wrong is a read past the end of a
         * buffer the runtime handed us, not a wrong picture. */
        const char *j =
          "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
          "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
          "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
            "\"count\":10000,\"type\":\"VEC3\"}],"
          "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
          "\"buffers\":[{\"byteLength\":12}]}";
        uint8_t bin[12] = { 0 };
        uint32_t l2 = 0;
        uint8_t *g2 = glb(j, bin, 12, &l2);
        check(cuevr_glb_parse(g2, l2, &bad) != 0,
              "an accessor past the end of its buffer is refused");
        free(g2);
    }
    {
        /* No triangles at all is not a model. */
        const char *j = "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[]}";
        uint32_t l2 = 0;
        uint8_t *g2 = glb(j, NULL, 0, &l2);
        check(cuevr_glb_parse(g2, l2, &bad) != 0, "an empty model is refused");
        free(g2);
    }

    free(g);
    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
