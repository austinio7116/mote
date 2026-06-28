#!/usr/bin/env python3
"""
gen_chess_stl.py — emit binary STL meshes for the chess pieces.

Each piece is a surface of revolution of a {radius, height} profile (the same
profiles the old procedural chess used), in world units with the base at y=0.
The king and queen are split into TWO parts each — a body STL and a topper STL
(cross / coronet) — so the game can colour the topper differently from the body
(STL carries no colour; the game tints each part at draw time).

Winding is CCW-from-outside, matching the engine; stl2mesh recomputes normals
from the winding, so the STL normal field is advisory only.

Usage: python3 tools/gen_chess_stl.py [out_dir=games/chess/assets]
"""
import math, struct, sys, os

TAU = 2 * math.pi

def revolve(prof, segs):
    """prof: list of (r, y). Returns list of triangles (each = 3 (x,y,z) tuples)."""
    has_apex = prof[-1][0] < 0.03
    rings = len(prof) - 1 if has_apex else len(prof)
    V = []
    for i in range(rings):
        r, y = prof[i]
        for s in range(segs):
            a = s * TAU / segs
            V.append((r * math.cos(a), y, r * math.sin(a)))
    tris = []
    def tri(i0, i1, i2): tris.append((V[i0], V[i1], V[i2]))
    for i in range(rings - 1):                      # side quads
        for s in range(segs):
            s2 = (s + 1) % segs
            a = i * segs + s; b = i * segs + s2
            c = (i + 1) * segs + s; d = (i + 1) * segs + s2
            tri(a, d, b); tri(a, c, d)
    base = (rings - 1) * segs
    if has_apex:                                    # pointed top
        V.append((0.0, prof[-1][1], 0.0)); apex = len(V) - 1
        for s in range(segs):
            s2 = (s + 1) % segs
            tri(base + s2, base + s, apex)
    else:                                           # flat cap
        V.append((0.0, prof[rings - 1][1], 0.0)); top = len(V) - 1
        for s in range(segs):
            s2 = (s + 1) % segs
            tri(top, base + s2, base + s)
    V.append((0.0, prof[0][1], 0.0)); ctr = len(V) - 1   # bottom cap
    for s in range(segs):
        s2 = (s + 1) % segs
        tri(ctr, s, s2)
    return tris

def box(cx, cy, cz, hx, hy, hz):
    X = [-hx, hx, hx, -hx, -hx, hx, hx, -hx]
    Y = [-hy, -hy, -hy, -hy, hy, hy, hy, hy]
    Z = [-hz, -hz, hz, hz, -hz, -hz, hz, hz]
    P = [(cx + X[i], cy + Y[i], cz + Z[i]) for i in range(8)]
    q = [[0,1,5,4],[1,2,6,5],[2,3,7,6],[3,0,4,7],[4,5,6,7],[3,2,1,0]]
    tris = []
    for f in q:
        tris.append((P[f[0]], P[f[2]], P[f[1]]))
        tris.append((P[f[0]], P[f[3]], P[f[2]]))
    return tris

def knight():
    """Revolved base + an extruded horse-head silhouette (forward = +z) + ears."""
    base = [(0.27,0),(0.35,0.05),(0.30,0.12),(0.22,0.22),(0.20,0.32)]
    tris = revolve(base, 8)
    sil = [(-0.16,0.30),(-0.23,0.50),(-0.16,0.66),(-0.02,0.76),(0.17,0.71),
           (0.30,0.58),(0.37,0.46),(0.26,0.40),(0.10,0.36),(-0.05,0.32)]
    hw = 0.155
    R = [(hw, p[1], p[0]) for p in sil]      # right face (x = +hw), (z = sil_x, y = sil_y)
    L = [(-hw, p[1], p[0]) for p in sil]     # left  face (x = -hw)
    n = len(sil)
    for i in range(1, n - 1):                # fan-fill each side
        tris.append((R[0], R[i], R[i + 1]))
        tris.append((L[0], L[i + 1], L[i]))
    for i in range(n):                       # rim joining the two silhouettes
        j = (i + 1) % n
        tris.append((R[i], L[i], L[j]))
        tris.append((R[i], L[j], R[j]))
    tris += box( 0.10, 0.80, -0.06, 0.05, 0.10, 0.05)   # ears
    tris += box(-0.10, 0.80, -0.06, 0.05, 0.10, 0.05)
    return tris

def queen_crown():
    tris = []
    for k in range(6):
        a = k * TAU / 6
        tris += box(math.cos(a) * 0.25, 1.04, math.sin(a) * 0.25, 0.05, 0.09, 0.05)
    tris += box(0, 1.20, 0, 0.10, 0.10, 0.10)           # coronet ball
    return tris

def king_cross():
    return box(0, 1.46, 0, 0.05, 0.14, 0.05) + box(0, 1.50, 0, 0.14, 0.05, 0.05)

def write_stl(path, tris):
    with open(path, "wb") as f:
        f.write(b"mote chess piece" + b" " * 64)       # 80-byte header
        f.write(struct.pack("<I", len(tris)))
        for a, b, c in tris:
            ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
            vx, vy, vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
            nx, ny, nz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
            l = math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0
            f.write(struct.pack("<3f", nx/l, ny/l, nz/l))
            for v in (a, b, c): f.write(struct.pack("<3f", *v))
            f.write(struct.pack("<H", 0))
    print(f"  {os.path.basename(path):20s} {len(tris):4d} tris")

PAWN   = [(0.26,0),(0.34,0.05),(0.30,0.11),(0.17,0.20),(0.15,0.38),(0.22,0.45),(0.15,0.50),(0.205,0.61),(0.16,0.68),(0.0,0.83)]
ROOK   = [(0.28,0),(0.36,0.05),(0.31,0.12),(0.28,0.22),(0.27,0.60),(0.32,0.68),(0.35,0.78),(0.35,0.88),(0.26,0.84),(0.26,0.93)]
BISHOP = [(0.27,0),(0.35,0.05),(0.29,0.12),(0.15,0.22),(0.13,0.58),(0.20,0.70),(0.22,0.78),(0.14,0.90),(0.175,0.97),(0.08,1.08),(0.105,1.13),(0.0,1.18)]
QUEEN  = [(0.29,0),(0.38,0.05),(0.31,0.12),(0.17,0.22),(0.15,0.64),(0.23,0.78),(0.17,0.86),(0.29,0.98),(0.23,1.04),(0.16,1.14),(0.20,1.19),(0.0,1.33)]
KING   = [(0.30,0),(0.39,0.05),(0.32,0.12),(0.18,0.22),(0.16,0.70),(0.24,0.84),(0.18,0.92),(0.29,1.04),(0.225,1.10),(0.17,1.20),(0.13,1.28),(0.10,1.34)]

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "games/chess/assets"
    os.makedirs(out, exist_ok=True)
    pieces = {
        "pawn":        revolve(PAWN, 6),
        "rook":        revolve(ROOK, 10),
        "bishop":      revolve(BISHOP, 10),
        "knight":      knight(),
        "queen_body":  revolve(QUEEN, 10),
        "queen_crown": queen_crown(),
        "king_body":   revolve(KING, 10),
        "king_cross":  king_cross(),
    }
    print(f"writing chess STLs -> {out}")
    for name, tris in pieces.items():
        write_stl(os.path.join(out, name + ".stl"), tris)

if __name__ == "__main__":
    main()
