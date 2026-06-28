/*
 * obj2mesh — Wavefront OBJ -> a self-contained Mote mesh header.
 *
 * Parses OBJ (v / f / mtllib / usemtl; f as a, a/t, a//n, a/t/n; polygons
 * fan-triangulated) + .mtl Kd colours, quantises verts to int8 (scale =
 * max |coord|), precomputes int8 face normals from the winding, and emits a
 * header the game #includes: static const <name>_verts/_faces + a static
 * const Mesh <name>_mesh. (Ported from ThumbyElite tools/obj2mesh.c; output
 * adapted from the engine's .c/.h pair to one drop-in header per model.)
 *
 * Usage: obj2mesh <name> <in.obj> <out.h>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "tex_embed.h"      /* PNG -> RGB565 MoteImage (ABI v35 textured meshes) */

#define MAX_V 4096
#define MAX_VT 8192
#define MAX_F 8192
#define MAX_MTL 64

typedef struct { float x, y, z; } V3;
typedef struct { float u, v; } V2;
typedef struct { int a, b, c; int mtl; int ta, tb, tc; } Face;  /* t* = vt index per corner, -1 if none */
typedef struct { char name[64]; float r, g, b; char map_Kd[256]; } Mtl;

static V3   verts[MAX_V];
static V2   texc[MAX_VT];
static Face faces[MAX_F];
static Mtl  mtls[MAX_MTL];
static int  nv, nvt, nf, nmtl;
static char g_objdir[400];          /* directory of the .obj, for resolving map_Kd */

static int mtl_find(const char *name) {
    for (int i = 0; i < nmtl; i++)
        if (!strcmp(mtls[i].name, name)) return i;
    return -1;
}

static void load_mtl(const char *objpath, const char *mtlname) {
    char path[512];
    const char *slash = strrchr(objpath, '/');
    if (slash)
        snprintf(path, sizeof path, "%.*s/%s",
                 (int)(slash - objpath), objpath, mtlname);
    else
        snprintf(path, sizeof path, "%s", mtlname);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "warn: no mtl %s\n", path); return; }
    char line[256];
    Mtl *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char name[64], tex[256];
        float r, g, b;
        if (sscanf(line, "newmtl %63s", name) == 1) {
            if (nmtl < MAX_MTL) {
                cur = &mtls[nmtl++];
                snprintf(cur->name, sizeof cur->name, "%s", name);
                cur->r = cur->g = cur->b = 0.6f;
                cur->map_Kd[0] = 0;
            }
        } else if (cur && sscanf(line, "Kd %f %f %f", &r, &g, &b) == 3) {
            cur->r = r; cur->g = g; cur->b = b;
        } else if (cur && sscanf(line, "map_Kd %255[^\r\n]", tex) == 1) {
            /* strip leading whitespace + trailing CR; store relative to the .obj dir */
            char *t = tex; while (*t == ' ' || *t == '\t') t++;
            snprintf(cur->map_Kd, sizeof cur->map_Kd, "%s", t);
        }
    }
    fclose(f);
}

/* Resolve a map_Kd path (relative to the .obj/.mtl dir) into `out`. */
static void resolve_tex(const char *rel, char *out, size_t outsz) {
    if (rel[0] == '/' || g_objdir[0] == 0) snprintf(out, outsz, "%s", rel);
    else snprintf(out, outsz, "%s/%s", g_objdir, rel);
}

/* parse "a", "a/t", "a//n" or "a/t/n" -> vertex index in *vi, texcoord in *ti (-1 if none). */
static void parse_ref(const char *tok, int *vi, int *ti) {
    int a = atoi(tok); if (a < 0) a = nv + 1 + a; *vi = a - 1;
    *ti = -1;
    const char *s = strchr(tok, '/');
    if (s && s[1] && s[1] != '/') { int t = atoi(s + 1); if (t < 0) t = nvt + 1 + t; *ti = t - 1; }
}

static int load_obj(const char *path) {
    nv = nvt = nf = nmtl = 0;
    const char *slash = strrchr(path, '/');
    if (slash) snprintf(g_objdir, sizeof g_objdir, "%.*s", (int)(slash - path), path);
    else g_objdir[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return 0; }
    char line[512];
    int cur_mtl = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "mtllib ", 7)) {
            char name[256];
            if (sscanf(line, "mtllib %255s", name) == 1) load_mtl(path, name);
        } else if (!strncmp(line, "usemtl ", 7)) {
            char name[64];
            if (sscanf(line, "usemtl %63s", name) == 1) cur_mtl = mtl_find(name);
        } else if (line[0] == 'v' && line[1] == 't') {
            if (nvt >= MAX_VT) { fprintf(stderr, "too many texcoords\n"); return 0; }
            sscanf(line + 3, "%f %f", &texc[nvt].u, &texc[nvt].v);
            nvt++;
        } else if (line[0] == 'v' && line[1] == ' ') {
            if (nv >= MAX_V) { fprintf(stderr, "too many verts\n"); return 0; }
            sscanf(line + 2, "%f %f %f", &verts[nv].x, &verts[nv].y, &verts[nv].z);
            nv++;
        } else if (line[0] == 'f' && line[1] == ' ') {
            int idx[16], tdx[16], n = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 16) { parse_ref(tok, &idx[n], &tdx[n]); n++; tok = strtok(NULL, " \t\r\n"); }
            for (int i = 2; i < n; i++) {
                if (nf >= MAX_F) { fprintf(stderr, "too many faces\n"); return 0; }
                faces[nf].a = idx[0]; faces[nf].b = idx[i - 1]; faces[nf].c = idx[i];
                faces[nf].ta = tdx[0]; faces[nf].tb = tdx[i - 1]; faces[nf].tc = tdx[i];
                faces[nf].mtl = cur_mtl; nf++;
            }
        }
    }
    fclose(f);
    return 1;
}

static V3 v3sub(V3 a, V3 b) { V3 r = {a.x-b.x, a.y-b.y, a.z-b.z}; return r; }
static V3 v3cross(V3 a, V3 b) {
    V3 r = {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; return r;
}
static float v3dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float v3len(V3 a) { return sqrtf(v3dot(a, a)); }

static uint16_t kd565(int mtl) {
    float r = 0.6f, g = 0.6f, b = 0.65f;
    if (mtl >= 0) { r = mtls[mtl].r; g = mtls[mtl].g; b = mtls[mtl].b; }
    int ri = (int)(r*255+0.5f), gi = (int)(g*255+0.5f), bi = (int)(b*255+0.5f);
    if (ri > 255) ri = 255; if (gi > 255) gi = 255; if (bi > 255) bi = 255;
    return (uint16_t)(((ri & 0xF8) << 8) | ((gi & 0xFC) << 3) | (bi >> 3));
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <name> <in.obj> <out.h>\n", argv[0]);
        return 1;
    }
    const char *name = argv[1], *in = argv[2], *out = argv[3];
    if (!load_obj(in)) return 1;
    if (nv == 0 || nf == 0) { fprintf(stderr, "error: %s has no geometry\n", in); return 1; }

    float maxc = 0, bound_r = 0;
    V3 centre = {0, 0, 0};
    for (int i = 0; i < nv; i++) {
        float ax = fabsf(verts[i].x), ay = fabsf(verts[i].y), az = fabsf(verts[i].z);
        if (ax > maxc) maxc = ax; if (ay > maxc) maxc = ay; if (az > maxc) maxc = az;
        float l = v3len(verts[i]); if (l > bound_r) bound_r = l;
        centre.x += verts[i].x; centre.y += verts[i].y; centre.z += verts[i].z;
    }
    centre.x /= nv; centre.y /= nv; centre.z /= nv;
    if (maxc < 1e-6f) maxc = 1.0f;
    float q = 127.0f / maxc;

    /* Texture source priority (so the Studio's "Assign texture" UI always wins):
     *   1. a sidecar PNG <basename>.png next to the .obj (assigned via the GUI),
     *   2. else the first material's map_Kd in the .mtl.
     * The sidecar makes texturing work with NO .mtl at all. */
    char texpath[700] = ""; int textured = 0, tex_avg = 0;
    { const char *dot = strrchr(in, '.'); size_t base = dot ? (size_t)(dot - in) : strlen(in);
      char sidecar[700]; snprintf(sidecar, sizeof sidecar, "%.*s.png", (int)base, in);
      FILE *sf = fopen(sidecar, "rb");
      if (sf) { fclose(sf); snprintf(texpath, sizeof texpath, "%s", sidecar); }
      else { int tex_mtl = -1; for (int i = 0; i < nmtl; i++) if (mtls[i].map_Kd[0]) { tex_mtl = i; break; }
             if (tex_mtl >= 0) resolve_tex(mtls[tex_mtl].map_Kd, texpath, sizeof texpath); } }

    FILE *h = fopen(out, "w");
    if (!h) { perror("output"); return 1; }
    fprintf(h, "/* GENERATED by obj2mesh from %s — do not edit. */\n", in);
    fprintf(h, "#ifndef MOTE_MESH_%s_H\n#define MOTE_MESH_%s_H\n", name, name);
    fprintf(h, "#include \"mote_mesh.h\"\n\n");

    if (texpath[0]) {
        int tw, th;
        if (tex_embed(h, name, texpath, &tw, &th, &tex_avg)) { textured = 1; fprintf(h, "\n"); }
        else fprintf(stderr, "warn: %s texture '%s' failed to load; emitting flat colour\n", name, texpath);
    }

    fprintf(h, "static const MeshVert %s_verts[%d] = {\n", name, nv);
    for (int i = 0; i < nv; i++)
        fprintf(h, "    {%4d,%4d,%4d},%s",
                (int)lrintf(verts[i].x*q), (int)lrintf(verts[i].y*q),
                (int)lrintf(verts[i].z*q), (i % 4 == 3 || i == nv-1) ? "\n" : "");
    fprintf(h, "};\n");

    /* Colour: if every face shares one material colour, store it once on the Mesh
     * (6-byte faces, face_colors = NULL). Otherwise emit a per-face colour array. */
    uint16_t c0 = nf ? kd565(faces[0].mtl) : 0; int uniform = 1;
    for (int i = 1; i < nf; i++) if (kd565(faces[i].mtl) != c0) { uniform = 0; break; }

    fprintf(h, "static const MeshFace %s_faces[%d] = {\n", name, nf);
    int warned = 0;
    for (int i = 0; i < nf; i++) {
        V3 a = verts[faces[i].a], b = verts[faces[i].b], cc = verts[faces[i].c];
        V3 n = v3cross(v3sub(b, a), v3sub(cc, a));
        float l = v3len(n);
        if (l < 1e-9f) { n.x = 0; n.y = 0; n.z = 1; l = 1; }
        n.x /= l; n.y /= l; n.z /= l;
        V3 fc = {(a.x+b.x+cc.x)/3, (a.y+b.y+cc.y)/3, (a.z+b.z+cc.z)/3};
        if (v3dot(n, v3sub(fc, centre)) < -1e-4f && warned < 8) {
            fprintf(stderr, "warn: %s face %d may be inward-wound\n", name, i); warned++;
        }
        fprintf(h, "    {%3d,%3d,%3d, %4d,%4d,%4d},\n",
                faces[i].a, faces[i].b, faces[i].c,
                (int)lrintf(n.x*127), (int)lrintf(n.y*127), (int)lrintf(n.z*127));
    }
    fprintf(h, "};\n");
    if (!uniform) {
        fprintf(h, "static const uint16_t %s_fcol[%d] = {\n", name, nf);
        for (int i = 0; i < nf; i++)
            fprintf(h, "0x%04X,%s", kd565(faces[i].mtl), (i % 8 == 7 || i == nf-1) ? "\n" : "");
        fprintf(h, "};\n");
    }

    /* Per-face UVs (only when textured). OBJ uv origin is bottom-left; the engine
     * samples tpix[v*w+u] (top-left origin), so flip v: v_byte = 255 - v*255.
     * Coords are wrapped into [0,1) first so tiling textures don't all clamp to an
     * edge, then quantised to 0..255. Faces with no texcoord get 0,0. */
    if (textured) {
        fprintf(h, "static const uint8_t %s_uvs[%d] = {\n", name, nf * 6);
        for (int i = 0; i < nf; i++) {
            int ti[3] = { faces[i].ta, faces[i].tb, faces[i].tc };
            for (int k = 0; k < 3; k++) {
                int ub = 0, vb = 0;
                if (ti[k] >= 0 && ti[k] < nvt) {
                    float u = texc[ti[k]].u, v = texc[ti[k]].v;
                    if (u < 0.0f || u > 1.0f) u -= floorf(u);    /* wrap only out-of-range (keep exact 1.0) */
                    if (v < 0.0f || v > 1.0f) v -= floorf(v);
                    ub = (int)lrintf(u * 255.0f); if (ub < 0) ub = 0; if (ub > 255) ub = 255;
                    vb = 255 - (int)lrintf(v * 255.0f); if (vb < 0) vb = 0; if (vb > 255) vb = 255;
                }
                fprintf(h, "%d,%d,%s", ub, vb, k == 2 ? "\n" : "");
            }
        }
        fprintf(h, "};\n");
    }

    /* Mesh initializer. Append the v35 .texture/.face_uvs after the lod_lo (0) slot
     * when textured; otherwise keep the original short (flat-colour) form. */
    if (textured)
        fprintf(h, "static const Mesh %s_mesh = { %s_verts, %s_faces, 0, %d, %d, 0x%04X, "
                   "%.6ff, %.6ff, 0, &%s_tex, %s_uvs };\n\n#endif\n",
                name, name, name, nv, nf, tex_avg, maxc, bound_r, name, name);
    else if (uniform)
        fprintf(h, "static const Mesh %s_mesh = { %s_verts, %s_faces, 0, %d, %d, 0x%04X, "
                   "%.6ff, %.6ff, 0 };\n\n#endif\n",
                name, name, name, nv, nf, c0, maxc, bound_r);
    else
        fprintf(h, "static const Mesh %s_mesh = { %s_verts, %s_faces, %s_fcol, %d, %d, 0, "
                   "%.6ff, %.6ff, 0 };\n\n#endif\n",
                name, name, name, name, nv, nf, maxc, bound_r);
    fclose(h);
    printf("[obj2mesh] %s: %d verts %d tris scale=%.2fm r=%.2fm -> %s\n",
           name, nv, nf, maxc, bound_r, out);
    return 0;
}
