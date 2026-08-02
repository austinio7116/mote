#!/usr/bin/env python3
"""Convert a real 3D model of the console into the mesh the VR app loads.

gen_vr_chassis.py builds a stand-in by extruding the product photo's outline.
It is good enough to hold, but it is a slab with a photo on the front — it has
no moulded buttons, no shoulder buttons you can see round, no back.

When a proper model turns up, this replaces the stand-in without touching a line
of C: the renderer loads `chassis.mvm` at runtime, so the only thing that has to
be true is that the file is in the right format, the right units and the right
orientation. This does that conversion and then tells you the two numbers you
need to check.

    python3 vr/tools/obj2mvm.py thumby.obj vr/assets/chassis.mvm \\
        --texture thumby_diffuse.png --up Y --front Z --width 51.6

What it guarantees:
  · units are metres and the model is exactly --width across
  · the origin is the centre of the bounding box
  · +X right, +Y up, +Z out of the screen face, whatever the source used
  · every vertex has a normal (smooth-shaded from the faces if the file has none)
  · indices fit in uint16

What it cannot know is where the LCD is. It prints the model-space bounds so you
can set MOTE_VR_SCREEN_* in platform/vr/mote_vr_chassis.h by hand — or, if the
model has a separate object or material for the screen, pass --screen-object
<name> and it will measure that instead and print the exact defines to paste in.
"""
import argparse
import os
import struct
import sys

MAGIC = b"MVM1"
AXES = {"X": 0, "Y": 1, "Z": 2, "-X": 0, "-Y": 1, "-Z": 2}


def axis(spec):
    s = spec.upper()
    if s not in AXES:
        raise SystemExit(f"obj2mvm: --up/--front must be one of X Y Z -X -Y -Z, not {spec}")
    return AXES[s], (-1.0 if s.startswith("-") else 1.0)


def load_obj(path, screen_object=None):
    pos, uv, nrm = [], [], []
    tris = []               # (vi, ti, ni) triples
    screen_tris = []
    cur = None
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                pos.append(tuple(float(v) for v in line.split()[1:4]))
            elif line.startswith("vt "):
                p = line.split()
                uv.append((float(p[1]), float(p[2]) if len(p) > 2 else 0.0))
            elif line.startswith("vn "):
                nrm.append(tuple(float(v) for v in line.split()[1:4]))
            elif line.startswith(("o ", "g ", "usemtl ")):
                cur = line.split(None, 1)[1].strip() if len(line.split()) > 1 else None
            elif line.startswith("f "):
                verts = []
                for tok in line.split()[1:]:
                    bits = (tok.split("/") + ["", ""])[:3]
                    def idx(s, n):
                        if not s:
                            return -1
                        i = int(s)
                        return i - 1 if i > 0 else n + i
                    verts.append((idx(bits[0], len(pos)), idx(bits[1], len(uv)),
                                  idx(bits[2], len(nrm))))
                # fan-triangulate the polygon
                for k in range(1, len(verts) - 1):
                    t = (verts[0], verts[k], verts[k + 1])
                    tris.append(t)
                    if screen_object and cur == screen_object:
                        screen_tris.append(t)
    return pos, uv, nrm, tris, screen_tris


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("out", nargs="?", default="vr/assets/chassis.mvm")
    ap.add_argument("--up", default="Y", help="which source axis points up")
    ap.add_argument("--front", default="Z", help="which source axis points out of the screen")
    ap.add_argument("--width", type=float, default=51.6, help="real width in mm")
    ap.add_argument("--screen-object", default=None,
                    help="object/material name of the LCD face, to measure it")
    a = ap.parse_args()

    pos, uv, nrm, tris, screen_tris = load_obj(a.obj, a.screen_object)
    if not tris:
        raise SystemExit("obj2mvm: no faces in that file")

    (ui, us), (fi, fs) = axis(a.up), axis(a.front)
    if ui == fi:
        raise SystemExit("obj2mvm: --up and --front must be different axes")
    ri = 3 - ui - fi
    # right-handed: X = Y cross Z. If the source triple is left-handed once
    # remapped, flip X rather than silently mirroring the whole model.
    rs = 1.0
    if (ui, fi) in ((0, 1), (1, 2), (2, 0)):
        rs = -1.0

    def remap(p):
        return (p[ri] * rs, p[ui] * us, p[fi] * fs)

    P = [remap(p) for p in pos]
    lo = [min(p[k] for p in P) for k in range(3)]
    hi = [max(p[k] for p in P) for k in range(3)]
    span = hi[0] - lo[0]
    if span <= 0:
        raise SystemExit("obj2mvm: model has no width")
    scale = (a.width / 1000.0) / span
    ctr = [(lo[k] + hi[k]) * 0.5 for k in range(3)]
    P = [tuple((p[k] - ctr[k]) * scale for k in range(3)) for p in P]
    N = [remap(n) for n in nrm]

    # Build the vertex list, sharing on the (v,t,n) triple as OBJ intends.
    seen, verts, idx = {}, [], []
    accum = {}                       # for meshes with no normals
    for t in tris:
        for key in t:
            if key not in seen:
                seen[key] = len(verts)
                vi, ti, ni = key
                p = P[vi]
                n = N[ni] if 0 <= ni < len(N) else (0.0, 0.0, 0.0)
                t2 = uv[ti] if 0 <= ti < len(uv) else (0.0, 0.0)
                # OBJ's V runs up from the bottom; GL's texture rows run down
                verts.append([p[0], p[1], p[2], n[0], n[1], n[2], t2[0], 1.0 - t2[1]])
            idx.append(seen[key])

    if not nrm:
        for k in range(0, len(idx), 3):
            i0, i1, i2 = idx[k], idx[k+1], idx[k+2]
            a0, b0, c0 = verts[i0], verts[i1], verts[i2]
            u = [b0[j] - a0[j] for j in range(3)]
            v = [c0[j] - a0[j] for j in range(3)]
            fn = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
            for i in (i0, i1, i2):
                acc = accum.setdefault(i, [0.0, 0.0, 0.0])
                for j in range(3):
                    acc[j] += fn[j]
        for i, acc in accum.items():
            L = sum(c*c for c in acc) ** 0.5 or 1.0
            verts[i][3:6] = [c / L for c in acc]
        print("obj2mvm: source had no normals — smooth-shaded from the faces")

    if len(verts) > 65535:
        raise SystemExit(f"obj2mvm: {len(verts)} vertices — over the uint16 index limit. "
                         "Decimate the model first.")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", len(verts), len(idx)))
        for v in verts:
            f.write(struct.pack("<8f", *v))
        for i in idx:
            f.write(struct.pack("<H", i))

    nlo = [min(v[k] for v in verts) for k in range(3)]
    nhi = [max(v[k] for v in verts) for k in range(3)]
    print(f"obj2mvm: {len(verts)} verts, {len(idx)//3} tris -> {a.out}")
    print(f"  bounds  X {nlo[0]*1000:+.1f}..{nhi[0]*1000:+.1f} mm"
          f"  Y {nlo[1]*1000:+.1f}..{nhi[1]*1000:+.1f} mm"
          f"  Z {nlo[2]*1000:+.1f}..{nhi[2]*1000:+.1f} mm")
    print(f"  set MOTE_VR_CHASSIS_HALF to {nhi[2]:.6f}f (the front face's z)")

    if screen_tris:
        si = [seen[k] for t in screen_tris for k in t]
        sl = [min(verts[i][k] for i in si) for k in range(3)]
        sh = [max(verts[i][k] for i in si) for k in range(3)]
        print("\n  paste into platform/vr/mote_vr_chassis.h:")
        print(f"    #define MOTE_VR_SCREEN_L  {sl[0]:.6f}f")
        print(f"    #define MOTE_VR_SCREEN_R  {sh[0]:.6f}f")
        print(f"    #define MOTE_VR_SCREEN_B  {sl[1]:.6f}f")
        print(f"    #define MOTE_VR_SCREEN_T  {sh[1]:.6f}f")
        print(f"    #define MOTE_VR_CHASSIS_HALF {sh[2]:.6f}f")
    elif a.screen_object:
        print(f"  WARNING: no object or material called '{a.screen_object}' in that file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
