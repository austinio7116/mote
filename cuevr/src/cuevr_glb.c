/*
 * CueVR — GLB parsing. See cuevr_glb.h for what subset and why.
 *
 * Two halves: a small JSON reader, and the glTF walk that uses it.
 *
 * The JSON reader builds a flat array of nodes rather than a tree of pointers.
 * Every value is one entry; objects and arrays hold the index of their first
 * child and each child holds the index of its next sibling. One allocation, no
 * per-node malloc, and "the seventh accessor" is a loop rather than a cast.
 * It is about two hundred lines, which is the price of not taking a dependency
 * for a file format that arrives once at start-up.
 */
#include "cuevr_glb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

static const char *s_err = "";
const char *cuevr_glb_error(void) { return s_err; }
static int fail(const char *why) { s_err = why; return -1; }

/* ---- JSON ---------------------------------------------------------------- */

enum { JNULL = 0, JBOOL, JNUM, JSTR, JARR, JOBJ };

typedef struct {
    uint8_t  type;
    int      first;      /* first child (JARR/JOBJ), else -1 */
    int      next;       /* next sibling, else -1 */
    int      key;        /* for an object's members: offset of the key string */
    int      keylen;
    double   num;        /* JNUM, and 0/1 for JBOOL */
    int      str;        /* JSTR: offset into the source */
    int      strlen;
} JNode;

typedef struct {
    const char *s;
    int         n, at;
    JNode      *nd;
    int         nn, cap;
    int         bad;
} JParse;

static int jnew(JParse *p, int type) {
    if (p->nn >= p->cap) { p->bad = 1; return -1; }
    JNode *v = &p->nd[p->nn];
    memset(v, 0, sizeof *v);
    v->type = (uint8_t)type;
    v->first = v->next = -1;
    v->key = -1;
    return p->nn++;
}

static void jskip(JParse *p) {
    while (p->at < p->n) {
        char c = p->s[p->at];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->at++;
        else break;
    }
}

static int jvalue(JParse *p);

/* A string, as an offset and a length into the source. Escapes are SKIPPED
 * over, not decoded: every string this reader looks at is a glTF key or an
 * attribute name, and those are plain ASCII. A model whose material is called
 * "Body" will simply not match, which is the right kind of wrong. */
static int jstring(JParse *p, int *off, int *len) {
    if (p->at >= p->n || p->s[p->at] != '"') return -1;
    p->at++;
    int start = p->at;
    while (p->at < p->n && p->s[p->at] != '"') {
        if (p->s[p->at] == '\\') p->at++;
        p->at++;
    }
    if (p->at >= p->n) return -1;
    *off = start;
    *len = p->at - start;
    p->at++;
    return 0;
}

static int jvalue(JParse *p) {
    jskip(p);
    if (p->at >= p->n) { p->bad = 1; return -1; }
    char c = p->s[p->at];

    if (c == '{') {
        int me = jnew(p, JOBJ);
        if (me < 0) return -1;
        p->at++;
        int last = -1;
        for (;;) {
            jskip(p);
            if (p->at >= p->n) { p->bad = 1; return -1; }
            if (p->s[p->at] == '}') { p->at++; break; }
            if (p->s[p->at] == ',') { p->at++; continue; }
            int koff, klen;
            if (jstring(p, &koff, &klen) != 0) { p->bad = 1; return -1; }
            jskip(p);
            if (p->at >= p->n || p->s[p->at] != ':') { p->bad = 1; return -1; }
            p->at++;
            int kid = jvalue(p);
            if (kid < 0) return -1;
            p->nd[kid].key = koff;
            p->nd[kid].keylen = klen;
            if (last < 0) p->nd[me].first = kid; else p->nd[last].next = kid;
            last = kid;
        }
        return me;
    }
    if (c == '[') {
        int me = jnew(p, JARR);
        if (me < 0) return -1;
        p->at++;
        int last = -1;
        for (;;) {
            jskip(p);
            if (p->at >= p->n) { p->bad = 1; return -1; }
            if (p->s[p->at] == ']') { p->at++; break; }
            if (p->s[p->at] == ',') { p->at++; continue; }
            int kid = jvalue(p);
            if (kid < 0) return -1;
            if (last < 0) p->nd[me].first = kid; else p->nd[last].next = kid;
            last = kid;
        }
        return me;
    }
    if (c == '"') {
        int me = jnew(p, JSTR);
        if (me < 0) return -1;
        if (jstring(p, &p->nd[me].str, &p->nd[me].strlen) != 0) { p->bad = 1; return -1; }
        return me;
    }
    if (!strncmp(p->s + p->at, "true", 4)) {
        int me = jnew(p, JBOOL); if (me < 0) return -1;
        p->nd[me].num = 1; p->at += 4; return me;
    }
    if (!strncmp(p->s + p->at, "false", 5)) {
        int me = jnew(p, JBOOL); if (me < 0) return -1;
        p->nd[me].num = 0; p->at += 5; return me;
    }
    if (!strncmp(p->s + p->at, "null", 4)) {
        int me = jnew(p, JNULL); if (me < 0) return -1;
        p->at += 4; return me;
    }
    {
        /* A number. strtod would need a NUL-terminated buffer and the JSON chunk
         * is not one, so it is parsed here. glTF numbers are plain: an optional
         * sign, digits, a fraction, an exponent. */
        int me = jnew(p, JNUM);
        if (me < 0) return -1;
        int sign = 1;
        if (p->s[p->at] == '-') { sign = -1; p->at++; }
        else if (p->s[p->at] == '+') p->at++;
        double v = 0;
        int any = 0;
        while (p->at < p->n && p->s[p->at] >= '0' && p->s[p->at] <= '9') {
            v = v * 10.0 + (p->s[p->at] - '0'); p->at++; any = 1;
        }
        if (p->at < p->n && p->s[p->at] == '.') {
            p->at++;
            double f = 0.1;
            while (p->at < p->n && p->s[p->at] >= '0' && p->s[p->at] <= '9') {
                v += (p->s[p->at] - '0') * f; f *= 0.1; p->at++; any = 1;
            }
        }
        if (!any) { p->bad = 1; return -1; }
        if (p->at < p->n && (p->s[p->at] == 'e' || p->s[p->at] == 'E')) {
            p->at++;
            int es = 1;
            if (p->at < p->n && p->s[p->at] == '-') { es = -1; p->at++; }
            else if (p->at < p->n && p->s[p->at] == '+') p->at++;
            int e = 0;
            while (p->at < p->n && p->s[p->at] >= '0' && p->s[p->at] <= '9') {
                e = e * 10 + (p->s[p->at] - '0'); p->at++;
            }
            v *= pow(10.0, es * e);
        }
        p->nd[me].num = sign * v;
        return me;
    }
}

/* ---- reading the tree ---------------------------------------------------- */

typedef struct { JParse *p; int root; } J;

static int jmember(J *j, int obj, const char *name) {
    if (obj < 0 || j->p->nd[obj].type != JOBJ) return -1;
    int nlen = (int)strlen(name);
    for (int k = j->p->nd[obj].first; k >= 0; k = j->p->nd[k].next)
        if (j->p->nd[k].keylen == nlen &&
            !strncmp(j->p->s + j->p->nd[k].key, name, (size_t)nlen)) return k;
    return -1;
}

static int jat(J *j, int arr, int i) {
    if (arr < 0 || j->p->nd[arr].type != JARR) return -1;
    int k = j->p->nd[arr].first;
    while (k >= 0 && i-- > 0) k = j->p->nd[k].next;
    return k;
}

static int jcount(J *j, int arr) {
    if (arr < 0 || j->p->nd[arr].type != JARR) return 0;
    int n = 0;
    for (int k = j->p->nd[arr].first; k >= 0; k = j->p->nd[k].next) n++;
    return n;
}

static double jnum(J *j, int node, double dflt) {
    if (node < 0 || (j->p->nd[node].type != JNUM && j->p->nd[node].type != JBOOL))
        return dflt;
    return j->p->nd[node].num;
}

static int jint(J *j, int obj, const char *name, int dflt) {
    int m = jmember(j, obj, name);
    return m < 0 ? dflt : (int)jnum(j, m, dflt);
}

/* ---- accessors ----------------------------------------------------------- */

/* glTF component types and the sizes they imply. */
#define CT_BYTE   5120
#define CT_UBYTE  5121
#define CT_SHORT  5122
#define CT_USHORT 5123
#define CT_UINT   5125
#define CT_FLOAT  5126

static int comp_size(int ct) {
    switch (ct) {
    case CT_BYTE: case CT_UBYTE:   return 1;
    case CT_SHORT: case CT_USHORT: return 2;
    case CT_UINT: case CT_FLOAT:   return 4;
    default: return 0;
    }
}

static int type_count(const char *s, int len) {
    if (len == 6 && !strncmp(s, "SCALAR", 6)) return 1;
    if (len == 4 && !strncmp(s, "VEC2", 4))   return 2;
    if (len == 4 && !strncmp(s, "VEC3", 4))   return 3;
    if (len == 4 && !strncmp(s, "VEC4", 4))   return 4;
    if (len == 4 && !strncmp(s, "MAT4", 4))   return 16;
    return 0;
}

typedef struct {
    const uint8_t *base;   /* first byte of element 0 */
    int stride;            /* bytes between elements */
    int count;
    int ncomp;
    int ctype;
    int normalized;
} Acc;

typedef struct {
    J *j;
    int accessors, bufferViews, buffers;
    const uint8_t *bin;
    uint32_t binlen;
} Gltf;

static int accessor(Gltf *g, int index, Acc *out) {
    int a = jat(g->j, g->accessors, index);
    if (a < 0) return fail("accessor out of range");
    int bv = jint(g->j, a, "bufferView", -1);
    if (bv < 0) return fail("accessor without a bufferView (sparse?)");
    if (jmember(g->j, a, "sparse") >= 0) return fail("sparse accessor");

    int t = jmember(g->j, a, "type");
    if (t < 0) return fail("accessor without a type");
    out->ncomp = type_count(g->j->p->s + g->j->p->nd[t].str, g->j->p->nd[t].strlen);
    if (!out->ncomp) return fail("unknown accessor type");
    out->ctype = jint(g->j, a, "componentType", 0);
    int cs = comp_size(out->ctype);
    if (!cs) return fail("unknown component type");
    out->count = jint(g->j, a, "count", 0);
    out->normalized = jint(g->j, a, "normalized", 0);

    int v = jat(g->j, g->bufferViews, bv);
    if (v < 0) return fail("bufferView out of range");
    if (jint(g->j, v, "buffer", 0) != 0) return fail("more than one buffer");
    int voff = jint(g->j, v, "byteOffset", 0);
    int vlen = jint(g->j, v, "byteLength", 0);
    int vstride = jint(g->j, v, "byteStride", 0);
    int aoff = jint(g->j, a, "byteOffset", 0);

    out->stride = vstride ? vstride : cs * out->ncomp;
    long need = (long)aoff + (long)(out->count ? out->count - 1 : 0) * out->stride
              + (long)cs * out->ncomp;
    if (voff < 0 || vlen < 0 || (uint32_t)voff > g->binlen ||
        need > vlen || (uint32_t)(voff + vlen) > g->binlen)
        return fail("accessor runs past the buffer");
    out->base = g->bin + voff + aoff;
    return 0;
}

/* One component, as a float, with the glTF normalisation rules applied. */
static float acc_comp(const Acc *a, int elem, int c) {
    const uint8_t *p = a->base + (size_t)elem * a->stride;
    switch (a->ctype) {
    case CT_FLOAT: { float f; memcpy(&f, p + 4 * c, 4); return f; }
    case CT_UBYTE: { uint8_t v = p[c];
                     return a->normalized ? v / 255.0f : (float)v; }
    case CT_BYTE:  { int8_t v = (int8_t)p[c];
                     return a->normalized ? (v / 127.0f < -1.0f ? -1.0f : v / 127.0f)
                                          : (float)v; }
    case CT_USHORT:{ uint16_t v; memcpy(&v, p + 2 * c, 2);
                     return a->normalized ? v / 65535.0f : (float)v; }
    case CT_SHORT: { int16_t v; memcpy(&v, p + 2 * c, 2);
                     float f = v / 32767.0f;
                     return a->normalized ? (f < -1.0f ? -1.0f : f) : (float)v; }
    case CT_UINT:  { uint32_t v; memcpy(&v, p + 4 * c, 4); return (float)v; }
    default: return 0.0f;
    }
}

static uint32_t acc_index(const Acc *a, int elem) {
    const uint8_t *p = a->base + (size_t)elem * a->stride;
    switch (a->ctype) {
    case CT_UBYTE:  return p[0];
    case CT_USHORT: { uint16_t v; memcpy(&v, p, 2); return v; }
    case CT_UINT:   { uint32_t v; memcpy(&v, p, 4); return v; }
    default: return 0;
    }
}

/* ---- transforms ---------------------------------------------------------- */

static void m_ident(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Column-major, as glTF stores them and as GL wants them. */
static void m_mul(float *o, const float *a, const float *b) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++)
            r[c*4+i] = a[0*4+i]*b[c*4+0] + a[1*4+i]*b[c*4+1]
                     + a[2*4+i]*b[c*4+2] + a[3*4+i]*b[c*4+3];
    memcpy(o, r, sizeof r);
}

static void m_from_trs(float *m, const float *t, const float *q, const float *s) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float rr[16];
    m_ident(rr);
    rr[0] = 1 - 2*(y*y + z*z); rr[1] = 2*(x*y + z*w);     rr[2] = 2*(x*z - y*w);
    rr[4] = 2*(x*y - z*w);     rr[5] = 1 - 2*(x*x + z*z); rr[6] = 2*(y*z + x*w);
    rr[8] = 2*(x*z + y*w);     rr[9] = 2*(y*z - x*w);     rr[10] = 1 - 2*(x*x + y*y);
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) rr[c*4+i] *= s[c];
    rr[12] = t[0]; rr[13] = t[1]; rr[14] = t[2];
    memcpy(m, rr, sizeof rr);
}

static void m_pt(const float *m, const float *p, float *o) {
    o[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
    o[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
    o[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

/* Normals go through the inverse transpose. For the rigid-plus-uniform-scale
 * transforms a controller model actually uses, the upper 3x3 renormalised is
 * the same answer for far less code — but a NON-uniform scale would shear them,
 * so it is worth saying that this is the assumption rather than pretending. */
static void m_dir(const float *m, const float *v, float *o) {
    o[0] = m[0]*v[0] + m[4]*v[1] + m[8]*v[2];
    o[1] = m[1]*v[0] + m[5]*v[1] + m[9]*v[2];
    o[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2];
    float l = sqrtf(o[0]*o[0] + o[1]*o[1] + o[2]*o[2]);
    if (l > 1e-8f) { o[0] /= l; o[1] /= l; o[2] /= l; }
}

/* ---- the walk ------------------------------------------------------------ */

typedef struct {
    Gltf *g;
    CueVrGlbModel *m;
    int meshes, nodes, materials, textures, images, samplers;
    int cap_v, cap_i;
    /* material index -> part index, so two nodes sharing a material share a
     * part and the model draws in as few calls as it has materials. */
    int part_of_mat[64];
} Walk;

static int want_part(Walk *w, int mat) {
    int key = (mat < 0 || mat >= 64) ? 0 : mat;
    if (w->part_of_mat[key] >= 0) return w->part_of_mat[key];
    if (w->m->nparts >= CUEVR_GLB_MAX_PARTS) return -1;
    int pi = w->m->nparts++;
    CueVrGlbPart *pt = &w->m->part[pi];
    pt->first_index = w->m->ni;
    pt->n_index = 0;
    pt->tex = -1;
    pt->base[0] = pt->base[1] = pt->base[2] = pt->base[3] = 1.0f;
    w->part_of_mat[key] = pi;
    return pi;
}

static int add_primitive(Walk *w, int prim, const float *xf) {
    J *j = w->g->j;
    int attrs = jmember(j, prim, "attributes");
    if (attrs < 0) return 0;                 /* nothing to draw; not an error */
    if (jint(j, prim, "mode", 4) != 4) return 0;   /* triangles only */

    int ap = jmember(j, attrs, "POSITION");
    if (ap < 0) return 0;
    Acc pos, nrm, uv, ind;
    if (accessor(w->g, (int)jnum(j, ap, -1), &pos) != 0) return -1;

    int have_n = 0, have_uv = 0;
    int an = jmember(j, attrs, "NORMAL");
    if (an >= 0 && accessor(w->g, (int)jnum(j, an, -1), &nrm) == 0
        && nrm.count >= pos.count) have_n = 1;
    int au = jmember(j, attrs, "TEXCOORD_0");
    if (au >= 0 && accessor(w->g, (int)jnum(j, au, -1), &uv) == 0
        && uv.count >= pos.count) have_uv = 1;

    int ai = jint(j, prim, "indices", -1);
    int nidx;
    if (ai >= 0) {
        if (accessor(w->g, ai, &ind) != 0) return -1;
        nidx = ind.count;
    } else {
        nidx = pos.count;                    /* unindexed: 0,1,2,... */
    }
    if (nidx % 3) return fail("triangle count is not a whole number");

    if (w->m->nv + pos.count > w->cap_v || w->m->ni + nidx > w->cap_i)
        return fail("model is larger than the reserved buffers");

    int pi = want_part(w, jint(j, prim, "material", -1));
    if (pi < 0) return fail("more materials than parts");
    /* Parts must stay contiguous in the index array, so a primitive can only
     * join the part that is currently last. Anything else would need the
     * indices resorted, and a controller model does not interleave. */
    if (pi != w->m->nparts - 1) {
        if (w->m->nparts >= CUEVR_GLB_MAX_PARTS) return fail("materials interleave");
        pi = w->m->nparts++;
        CueVrGlbPart *pt = &w->m->part[pi];
        pt->first_index = w->m->ni;
        pt->n_index = 0;
        pt->tex = w->m->part[w->part_of_mat[jint(j, prim, "material", 0) & 63]].tex;
        memcpy(pt->base,
               w->m->part[w->part_of_mat[jint(j, prim, "material", 0) & 63]].base,
               sizeof pt->base);
    }

    int base_v = w->m->nv;
    for (int i = 0; i < pos.count; i++) {
        CueVrGlbVtx *o = &w->m->v[w->m->nv++];
        float p[3] = { acc_comp(&pos, i, 0), acc_comp(&pos, i, 1), acc_comp(&pos, i, 2) };
        m_pt(xf, p, o->p);
        if (have_n) {
            float nn[3] = { acc_comp(&nrm, i, 0), acc_comp(&nrm, i, 1), acc_comp(&nrm, i, 2) };
            m_dir(xf, nn, o->n);
        } else {
            o->n[0] = 0.0f; o->n[1] = 1.0f; o->n[2] = 0.0f;
        }
        if (have_uv) { o->uv[0] = acc_comp(&uv, i, 0); o->uv[1] = acc_comp(&uv, i, 1); }
        else         { o->uv[0] = o->uv[1] = 0.0f; }
    }
    for (int i = 0; i < nidx; i++) {
        uint32_t k = ai >= 0 ? acc_index(&ind, i) : (uint32_t)i;
        if ((int)k >= pos.count) return fail("index past the end of the vertices");
        w->m->idx[w->m->ni++] = (uint32_t)base_v + k;
    }
    w->m->part[pi].n_index += nidx;
    return 0;
}

static int add_node(Walk *w, int ni, const float *parent, int depth) {
    if (depth > 16) return fail("node hierarchy is too deep");
    J *j = w->g->j;
    int node = jat(j, w->nodes, ni);
    if (node < 0) return 0;

    float local[16], xf[16];
    int mm = jmember(j, node, "matrix");
    if (mm >= 0 && jcount(j, mm) == 16) {
        for (int i = 0; i < 16; i++) local[i] = (float)jnum(j, jat(j, mm, i), 0);
    } else {
        float t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
        int a;
        if ((a = jmember(j, node, "translation")) >= 0 && jcount(j, a) == 3)
            for (int i = 0; i < 3; i++) t[i] = (float)jnum(j, jat(j, a, i), 0);
        if ((a = jmember(j, node, "rotation")) >= 0 && jcount(j, a) == 4)
            for (int i = 0; i < 4; i++) q[i] = (float)jnum(j, jat(j, a, i), i == 3 ? 1 : 0);
        if ((a = jmember(j, node, "scale")) >= 0 && jcount(j, a) == 3)
            for (int i = 0; i < 3; i++) s[i] = (float)jnum(j, jat(j, a, i), 1);
        m_from_trs(local, t, q, s);
    }
    m_mul(xf, parent, local);

    int mi = jint(j, node, "mesh", -1);
    if (mi >= 0) {
        int mesh = jat(j, w->meshes, mi);
        int prims = jmember(j, mesh, "primitives");
        int np = jcount(j, prims);
        for (int i = 0; i < np; i++)
            if (add_primitive(w, jat(j, prims, i), xf) != 0) return -1;
    }
    int ch = jmember(j, node, "children");
    int nc = jcount(j, ch);
    for (int i = 0; i < nc; i++)
        if (add_node(w, (int)jnum(j, jat(j, ch, i), -1), xf, depth + 1) != 0) return -1;
    return 0;
}

/* ---- materials and textures ---------------------------------------------- */

static int load_material(Walk *w, int mat_index, CueVrGlbPart *pt) {
    J *j = w->g->j;
    int mat = jat(j, w->materials, mat_index);
    if (mat < 0) return 0;
    int pbr = jmember(j, mat, "pbrMetallicRoughness");
    if (pbr < 0) return 0;

    int bcf = jmember(j, pbr, "baseColorFactor");
    if (bcf >= 0 && jcount(j, bcf) >= 4)
        for (int i = 0; i < 4; i++) pt->base[i] = (float)jnum(j, jat(j, bcf, i), 1);

    int bct = jmember(j, pbr, "baseColorTexture");
    if (bct < 0) return 0;
    int ti = jint(j, bct, "index", -1);
    int tex = jat(j, w->textures, ti);
    if (tex < 0) return 0;
    int src = jint(j, tex, "source", -1);
    int img = jat(j, w->images, src);
    if (img < 0) return 0;
    int bv = jint(j, img, "bufferView", -1);
    if (bv < 0) return 0;                  /* an external URI: no texture, not fatal */

    int v = jat(j, w->g->bufferViews, bv);
    if (v < 0) return 0;
    int off = jint(j, v, "byteOffset", 0);
    int len = jint(j, v, "byteLength", 0);
    if (off < 0 || len <= 0 || (uint32_t)(off + len) > w->g->binlen) return 0;

    if (w->m->ntex >= CUEVR_GLB_MAX_PARTS) return 0;
    int comp = 0, tw = 0, th = 0;
    uint8_t *px = stbi_load_from_memory(w->g->bin + off, len, &tw, &th, &comp, 4);
    if (!px) return 0;                     /* not a PNG we can read: draw it flat */
    w->m->tex[w->m->ntex].px = px;
    w->m->tex[w->m->ntex].w = tw;
    w->m->tex[w->m->ntex].h = th;
    pt->tex = w->m->ntex++;
    return 0;
}

/* ---- the front door ------------------------------------------------------ */

#define GLB_MAGIC  0x46546C67u   /* 'glTF' */
#define CHUNK_JSON 0x4E4F534Au
#define CHUNK_BIN  0x004E4942u

/* What a controller model costs. Meta's are a few thousand triangles; the caps
 * are generous and the parse refuses rather than overruns. */
#define MAX_V 65536
#define MAX_I 196608

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int cuevr_glb_parse(const void *bytes, uint32_t len, CueVrGlbModel *out) {
    memset(out, 0, sizeof *out);
    s_err = "";
    const uint8_t *b = (const uint8_t *)bytes;
    if (!b || len < 20) return fail("too short to be a GLB");
    if (rd32(b) != GLB_MAGIC) return fail("not a GLB");
    if (rd32(b + 4) != 2) return fail("not glTF 2.0");
    uint32_t total = rd32(b + 8);
    if (total > len) return fail("GLB length runs past the buffer");
    if (total < 20) total = len;

    const uint8_t *json = NULL, *bin = NULL;
    uint32_t jlen = 0, blen = 0;
    uint32_t at = 12;
    while (at + 8 <= total) {
        uint32_t clen = rd32(b + at), ctype = rd32(b + at + 4);
        at += 8;
        if ((uint64_t)at + clen > total) return fail("chunk runs past the file");
        if (ctype == CHUNK_JSON && !json) { json = b + at; jlen = clen; }
        else if (ctype == CHUNK_BIN && !bin) { bin = b + at; blen = clen; }
        at += clen;
        at = (at + 3) & ~3u;               /* chunks are 4-byte aligned */
    }
    if (!json) return fail("no JSON chunk");

    /* One allocation for the whole document. 24 bytes of JSON per node is a
     * safe floor — the shortest thing a node can be is `0,` — so this cannot
     * under-count, and it is freed before the function returns either way. */
    int cap = (int)(jlen / 4) + 64;
    JNode *nd = (JNode *)malloc(sizeof(JNode) * (size_t)cap);
    if (!nd) return fail("out of memory for the JSON");
    JParse jp;
    memset(&jp, 0, sizeof jp);
    jp.s = (const char *)json;
    jp.n = (int)jlen;
    jp.nd = nd;
    jp.cap = cap;
    int root = jvalue(&jp);
    if (root < 0 || jp.bad) { free(nd); return fail("the JSON did not parse"); }

    J j = { &jp, root };
    Gltf g;
    memset(&g, 0, sizeof g);
    g.j = &j;
    g.accessors   = jmember(&j, root, "accessors");
    g.bufferViews = jmember(&j, root, "bufferViews");
    g.buffers     = jmember(&j, root, "buffers");
    g.bin = bin;
    g.binlen = blen;

    Walk w;
    memset(&w, 0, sizeof w);
    w.g = &g;
    w.m = out;
    w.meshes    = jmember(&j, root, "meshes");
    w.nodes     = jmember(&j, root, "nodes");
    w.materials = jmember(&j, root, "materials");
    w.textures  = jmember(&j, root, "textures");
    w.images    = jmember(&j, root, "images");
    for (int i = 0; i < 64; i++) w.part_of_mat[i] = -1;
    w.cap_v = MAX_V;
    w.cap_i = MAX_I;

    out->v   = (CueVrGlbVtx *)malloc(sizeof(CueVrGlbVtx) * MAX_V);
    out->idx = (uint32_t *)malloc(sizeof(uint32_t) * MAX_I);
    if (!out->v || !out->idx) {
        free(nd); cuevr_glb_free(out); return fail("out of memory for the mesh");
    }

    float ident[16];
    m_ident(ident);

    int rc = 0;
    int scenes = jmember(&j, root, "scenes");
    int si = jint(&j, root, "scene", 0);
    int scene = jat(&j, scenes, si);
    int roots = scene >= 0 ? jmember(&j, scene, "nodes") : -1;
    if (roots >= 0) {
        int nr = jcount(&j, roots);
        for (int i = 0; i < nr && rc == 0; i++)
            rc = add_node(&w, (int)jnum(&j, jat(&j, roots, i), -1), ident, 0);
    } else {
        /* No scene: draw every node that is not somebody's child. A model
         * without a default scene is legal and Meta has shipped them. */
        int nn = jcount(&j, w.nodes);
        for (int i = 0; i < nn && rc == 0; i++)
            rc = add_node(&w, i, ident, 0);
    }

    /* Materials last, so a part exists to hang each one on. */
    if (rc == 0) {
        for (int m = 0; m < 64; m++) {
            int pi = w.part_of_mat[m];
            if (pi >= 0 && pi < out->nparts) load_material(&w, m, &out->part[pi]);
        }
    }

    free(nd);
    if (rc != 0) { cuevr_glb_free(out); return -1; }
    if (out->ni == 0) { cuevr_glb_free(out); return fail("no triangles in the model"); }
    return 0;
}

void cuevr_glb_free(CueVrGlbModel *m) {
    if (!m) return;
    free(m->v);
    free(m->idx);
    for (int i = 0; i < m->ntex; i++) stbi_image_free(m->tex[i].px);
    memset(m, 0, sizeof *m);
}
