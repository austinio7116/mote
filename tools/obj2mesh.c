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

#define MAX_V 4096
#define MAX_F 8192
#define MAX_MTL 64

typedef struct { float x, y, z; } V3;
typedef struct { int a, b, c; int mtl; } Face;
typedef struct { char name[64]; float r, g, b; } Mtl;

static V3   verts[MAX_V];
static Face faces[MAX_F];
static Mtl  mtls[MAX_MTL];
static int  nv, nf, nmtl;

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
        char name[64];
        float r, g, b;
        if (sscanf(line, "newmtl %63s", name) == 1) {
            if (nmtl < MAX_MTL) {
                cur = &mtls[nmtl++];
                snprintf(cur->name, sizeof cur->name, "%s", name);
                cur->r = cur->g = cur->b = 0.6f;
            }
        } else if (cur && sscanf(line, "Kd %f %f %f", &r, &g, &b) == 3) {
            cur->r = r; cur->g = g; cur->b = b;
        }
    }
    fclose(f);
}

static int parse_index(const char *tok) {
    int idx = atoi(tok);
    if (idx < 0) idx = nv + 1 + idx;
    return idx - 1;
}

static int load_obj(const char *path) {
    nv = nf = nmtl = 0;
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
        } else if (line[0] == 'v' && line[1] == ' ') {
            if (nv >= MAX_V) { fprintf(stderr, "too many verts\n"); return 0; }
            sscanf(line + 2, "%f %f %f", &verts[nv].x, &verts[nv].y, &verts[nv].z);
            nv++;
        } else if (line[0] == 'f' && line[1] == ' ') {
            int idx[16], n = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 16) { idx[n++] = parse_index(tok); tok = strtok(NULL, " \t\r\n"); }
            for (int i = 2; i < n; i++) {
                if (nf >= MAX_F) { fprintf(stderr, "too many faces\n"); return 0; }
                faces[nf].a = idx[0]; faces[nf].b = idx[i - 1]; faces[nf].c = idx[i];
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

    FILE *h = fopen(out, "w");
    if (!h) { perror("output"); return 1; }
    fprintf(h, "/* GENERATED by obj2mesh from %s — do not edit. */\n", in);
    fprintf(h, "#ifndef MOTE_MESH_%s_H\n#define MOTE_MESH_%s_H\n", name, name);
    fprintf(h, "#include \"mote_mesh.h\"\n\n");

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
    if (uniform)
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
