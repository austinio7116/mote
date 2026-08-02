#!/usr/bin/env python3
"""Turn the chassis photograph into a 3D object you can hold.

The phone shell draws `studio/assets/thumby_color.png` flat and uses its alpha to
cut the console out of the background. In VR there is no background to cut out
of — the console has to be a *thing*, with edges you can see round and a back
that faces away from you. So the alpha mask stops being a mask and becomes
geometry: trace its outline, and extrude that outline into a slab with a rounded
rim.

The result is photo-accurate where it matters. The photo is a straight-on shot
(its 2872x1668 is aspect 1.722; the real console is 51.6 x 30.0 mm, aspect 1.72),
so the traced silhouette *is* the console's outline, and mapping the photo onto
the front face puts every button, the screen bezel and the THUMBYCOLOR print
exactly where they belong — with the real product's own lighting baked in.

Three things come out of here:

  vr/assets/chassis.jpg   the front-face texture. No alpha channel: the cut-out
                          is the mesh now. 1024 wide, which at the scale the
                          console is held is already ~4x what the headset can
                          resolve.
  vr/assets/chassis.mvm   the mesh: interleaved position/normal/uv + uint16
                          indices. ~1600 verts, ~3200 tris.
  platform/vr/mote_vr_chassis.h   the model-space numbers the renderer needs
                          that are not vertices: overall size, where the LCD
                          square sits, where each button sits.

Everything is in metres, at 1:1 life size, origin at the centre of the slab,
+X right, +Y up, +Z out of the screen towards the player. The renderer scales it
up from there — 51.6 mm is a keychain, and a 128x128 LCD that small is not
legible in a headset (see MOTE_VR_SCALE).

Run from the repo root:  python3 vr/tools/gen_vr_chassis.py
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

# ---------------------------------------------------------------------------
# The real object. TinyCircuits give 51.6 x 30.0 x 11.6 mm.
# ---------------------------------------------------------------------------
REAL_W = 0.0516          # metres, the console's full width
REAL_T = 0.0116          # metres, its thickness
RIM_R  = 0.0026          # metres, how far the shell rolls over at the edge
RIM_STEPS = 4            # quarter-round subdivisions per side (4 = smooth enough)

TEX_W  = 1024            # baked texture width
JPEG_Q = 96          # smooth purple is where JPEG blocking shows as banding
BLEED_PX = 12            # how far to push the shell out past the silhouette
OUTLINE_N = 224          # outline vertices, evenly spaced
OUTLINE_SMOOTH = 6       # [1,2,1] passes over the traced edge

MAGIC = b"MVM1"

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC  = os.path.join(ROOT, "studio", "assets")
OUT_ASSETS = os.path.join(ROOT, "vr", "assets")
OUT_HDR    = os.path.join(ROOT, "platform", "vr", "mote_vr_chassis.h")


# ---------------------------------------------------------------------------
# 1. the outline
# ---------------------------------------------------------------------------
def trace_outline(mask):
    """Moore-neighbour boundary trace of the largest blob in a binary mask.

    The mask is padded first because the photo is cropped tight: the console's
    left and right edges sit exactly on the image border, so without a margin
    the trace would have nowhere to walk.
    """
    m = np.pad(mask, 1, mode="constant", constant_values=False)
    h, w = m.shape

    start = None
    for y in range(h):
        xs = np.flatnonzero(m[y])
        if xs.size:
            start = (int(xs[0]), y)
            break
    if start is None:
        raise SystemExit("gen_vr_chassis: the alpha mask is empty")

    # clockwise from west, in image coords (y down)
    nbr = [(-1, 0), (-1, -1), (0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1)]

    contour = [start]
    cur = start
    # we entered the start pixel from the west (it is the leftmost of its row)
    back = 0
    guard = 8 * (w + h) * 4
    while guard > 0:
        guard -= 1
        found = False
        # scan the 8-neighbourhood starting just after the direction we came from
        for k in range(1, 9):
            d = nbr[(back + k) % 8]
            nx, ny = cur[0] + d[0], cur[1] + d[1]
            if 0 <= nx < w and 0 <= ny < h and m[ny, nx]:
                # we will arrive at (nx,ny) coming from cur; record that direction
                back = ((back + k) % 8 + 4) % 8
                cur = (nx, ny)
                found = True
                break
        if not found:
            break                      # isolated pixel
        if cur == start and len(contour) > 2:
            break
        contour.append(cur)
    if guard <= 0:
        raise SystemExit("gen_vr_chassis: outline trace did not close")

    return [(x - 1, y - 1) for (x, y) in contour]     # undo the pad


def resample_ring(pts, n):
    """Resample a closed contour to n points, evenly spaced along its length.

    Douglas-Peucker was the obvious tool and the wrong one: it keeps the points
    that deviate most, which on a photographic alpha edge are exactly the
    single-pixel staircase artefacts. Extruding those turned a moulded shell
    into something inflatable. Even spacing plus the smoothing below keeps the
    shape and drops the noise.
    """
    p = np.asarray(pts, dtype=np.float64)
    d = np.linalg.norm(np.diff(np.vstack([p, p[:1]]), axis=0), axis=1)
    s = np.concatenate([[0.0], np.cumsum(d)])
    total = s[-1]
    want = np.linspace(0.0, total, n, endpoint=False)
    out = np.empty((n, 2))
    for k in range(2):
        out[:, k] = np.interp(want, s, np.concatenate([p[:, k], p[:1, k]]))
    return out


def smooth_ring(p, passes):
    """A few [1,2,1] passes around the loop. Shrinks the outline by well under
    a tenth of a millimetre at this scale, and takes the staircase off."""
    for _ in range(passes):
        p = 0.25 * np.roll(p, 1, axis=0) + 0.5 * p + 0.25 * np.roll(p, -1, axis=0)
    return p


# ---------------------------------------------------------------------------
# 2. polygon helpers
# ---------------------------------------------------------------------------
def signed_area(poly):
    s = 0.0
    for i in range(len(poly)):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % len(poly)]
        s += x0 * y1 - x1 * y0
    return 0.5 * s


def inward_normals(poly):
    """Per-vertex inward bisector and the scale that offsets edges by exactly 1.

    CCW winding, so the interior is to the left of every edge and the inward
    edge normal is the left normal.
    """
    n = len(poly)
    out = []
    for i in range(n):
        px, py = poly[(i - 1) % n]
        cx, cy = poly[i]
        nx_, ny_ = poly[(i + 1) % n]

        def left_normal(ax, ay, bx, by):
            dx, dy = bx - ax, by - ay
            L = math.hypot(dx, dy) or 1.0
            return (-dy / L, dx / L)

        n1 = left_normal(px, py, cx, cy)
        n2 = left_normal(cx, cy, nx_, ny_)
        bx_, by_ = n1[0] + n2[0], n1[1] + n2[1]
        L = math.hypot(bx_, by_)
        if L < 1e-9:
            bx_, by_, L = n1[0], n1[1], 1.0
        bx_, by_ = bx_ / L, by_ / L
        # offsetting both edges by d moves the vertex by d / cos(half-angle)
        c = bx_ * n1[0] + by_ * n1[1]
        out.append((bx_, by_, 1.0 / max(c, 0.35)))     # clamp: no spikes
    return out


def ear_clip(poly):
    """Triangulate a simple CCW polygon. O(n^2), n is ~160 here."""
    n = len(poly)
    idx = list(range(n))
    tris = []

    def area2(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    def inside(p, a, b, c):
        d1 = area2(a, b, p)
        d2 = area2(b, c, p)
        d3 = area2(c, a, p)
        neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
        pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
        return not (neg and pos)

    guard = n * n + 16
    while len(idx) > 3 and guard > 0:
        guard -= 1
        clipped = False
        for k in range(len(idx)):
            i0 = idx[(k - 1) % len(idx)]
            i1 = idx[k]
            i2 = idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if area2(a, b, c) <= 0:
                continue                       # reflex
            bad = False
            for j in idx:
                if j in (i0, i1, i2):
                    continue
                if inside(poly[j], a, b, c):
                    bad = True
                    break
            if bad:
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            clipped = True
            break
        if not clipped:
            break                              # degenerate; take what we have
    if len(idx) == 3:
        tris.append(tuple(idx))
    return tris


# ---------------------------------------------------------------------------
# 3. build
# ---------------------------------------------------------------------------
def main():
    src_png = os.path.join(SRC, "thumby_color.png")
    im = Image.open(src_png).convert("RGBA")
    W, H = im.size
    px = np.array(im)
    mask = px[:, :, 3] > 128

    real_h = REAL_W * H / W
    print(f"photo {W}x{H}  ->  {REAL_W*1000:.1f} x {real_h*1000:.1f} x {REAL_T*1000:.1f} mm")

    raw = trace_outline(mask)
    ring_px = smooth_ring(resample_ring(raw, OUTLINE_N), OUTLINE_SMOOTH)
    print(f"outline {len(raw)} px -> {len(ring_px)} verts")

    # photo pixels -> model metres (origin centre, +Y up) and their UVs
    def to_model(p):
        return ((p[0] + 0.5) / W - 0.5) * REAL_W, (0.5 - (p[1] + 0.5) / H) * real_h

    def to_uv(p):
        return (p[0] + 0.5) / W, (p[1] + 0.5) / H

    poly = [to_model(p) for p in ring_px]
    uv = [to_uv(p) for p in ring_px]
    if signed_area(poly) < 0:                  # want CCW for +Z front faces
        poly.reverse()
        uv.reverse()
    N = len(poly)

    # A patch of plain shell to paint the back and sides with. Taken as a UV
    # rather than an RGB so the mesh needs only the one texture and the shader
    # needs no branch.
    #
    # The median of the whole interior lands on near-black: between the glass,
    # the d-pad and the two face buttons, most of the front of this console is
    # dark. So drop everything below mid grey first and take the modal colour of
    # what is left on a coarse quantisation — the moulded purple, by a mile.
    inner = mask.copy()
    inner[:, : W // 8] = False                 # keep away from the rim shading
    inner[:, -W // 8:] = False
    ys, xs = np.nonzero(inner)
    rgb = px[ys, xs, :3].astype(np.int32)
    lit = rgb.max(axis=1) > 60
    ys, xs, rgb = ys[lit], xs[lit], rgb[lit]
    q = (rgb >> 4)
    key = (q[:, 0] << 8) | (q[:, 1] << 4) | q[:, 2]
    modal = np.bincount(key).argmax()
    d = np.abs(rgb - np.median(rgb[key == modal], axis=0)).sum(axis=1)
    k = int(np.argmin(d))
    shell_uv = to_uv((int(xs[k]), int(ys[k])))
    print(f"shell colour {tuple(int(v) for v in rgb[k])} at uv {shell_uv[0]:.3f},{shell_uv[1]:.3f}")

    half = REAL_T * 0.5
    r = min(RIM_R, half * 0.98)
    nrm = inward_normals(poly)

    # rings: front cap boundary -> front rim -> (side wall) -> back rim -> back cap
    rings = []
    for k in range(RIM_STEPS + 1):
        th = (k / RIM_STEPS) * (math.pi / 2)
        rings.append((r * (1.0 - math.sin(th)), half - r * (1.0 - math.cos(th)),
                      math.sin(th), math.cos(th)))
    for k in range(RIM_STEPS, -1, -1):
        th = (k / RIM_STEPS) * (math.pi / 2)
        rings.append((r * (1.0 - math.sin(th)), -(half - r * (1.0 - math.cos(th))),
                      math.sin(th), -math.cos(th)))
    front_rings = RIM_STEPS + 1                # these carry photo UVs

    verts = []                                 # (px,py,pz, nx,ny,nz, u,v)
    for ri, (inset, z, sn, cs) in enumerate(rings):
        textured = ri < front_rings
        for i in range(N):
            bx, by, sc = nrm[i]
            x = poly[i][0] + bx * inset * sc
            y = poly[i][1] + by * inset * sc
            # outward xy normal is the inward bisector reversed
            nx_, ny_ = -bx * sn, -by * sn
            if textured:
                # The photo maps orthographically onto the object, so a vertex's
                # UV is just where it is. Derived from the final position rather
                # than by nudging the outline's UV along with the inset: that is
                # a sign and a scale factor you can get wrong, and getting it
                # wrong shrinks the entire front face by the rim radius while
                # leaving the LCD — placed from exact model coordinates —
                # correctly sized, so the screen looks too big for its own bezel.
                u = x / REAL_W + 0.5
                v = 0.5 - y / real_h
                u, v = min(max(u, 0.0), 1.0), min(max(v, 0.0), 1.0)
            else:
                u, v = shell_uv
            verts.append((x, y, z, nx_, ny_, cs, u, v))

    tris = []
    # bands
    for ri in range(len(rings) - 1):
        a0 = ri * N
        b0 = (ri + 1) * N
        for i in range(N):
            j = (i + 1) % N
            tris.append((a0 + i, b0 + i, b0 + j))
            tris.append((a0 + i, b0 + j, a0 + j))

    # caps: ring 0 is already the front boundary, the last ring the back one.
    # The outline is star-shaped about its centroid, so a fan from a centre
    # vertex beats ear clipping — even triangles instead of slivers, and no
    # long thin ones sweeping across the face.
    inner_poly = [(poly[i][0] + nrm[i][0] * r * nrm[i][2],
                   poly[i][1] + nrm[i][1] * r * nrm[i][2]) for i in range(N)]
    cx = sum(p[0] for p in inner_poly) / N
    cy = sum(p[1] for p in inner_poly) / N
    star = all((inner_poly[(i + 1) % N][0] - inner_poly[i][0]) * (cy - inner_poly[i][1])
               - (inner_poly[(i + 1) % N][1] - inner_poly[i][1]) * (cx - inner_poly[i][0]) > 0
               for i in range(N))
    last = (len(rings) - 1) * N
    if star:
        cu = (cx / REAL_W + 0.5)
        cv = (0.5 - cy / real_h)
        fc, bc = len(verts), len(verts) + 1
        verts.append((cx, cy, half, 0.0, 0.0, 1.0, cu, cv))
        verts.append((cx, cy, -half, 0.0, 0.0, -1.0, shell_uv[0], shell_uv[1]))
        for i in range(N):
            j = (i + 1) % N
            tris.append((fc, i, j))
            tris.append((bc, last + j, last + i))
    else:
        print("outline is not star-shaped about its centroid; ear clipping")
        for (a, b, c) in ear_clip(inner_poly):
            tris.append((a, b, c))             # front, CCW seen from +Z
            tris.append((last + c, last + b, last + a))

    print(f"mesh {len(verts)} verts, {len(tris)} tris")
    if len(verts) > 65535:
        raise SystemExit("gen_vr_chassis: too many verts for uint16 indices")

    # ---- write the mesh -------------------------------------------------
    os.makedirs(OUT_ASSETS, exist_ok=True)
    with open(os.path.join(OUT_ASSETS, "chassis.mvm"), "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", len(verts), len(tris) * 3))
        for v in verts:
            f.write(struct.pack("<8f", *v))
        for t in tris:
            f.write(struct.pack("<3H", *t))

    # ---- bake the texture ------------------------------------------------
    # Outside the silhouette the photo is grey studio backdrop, and the rim's
    # UVs graze it: the outermost ring sits ON the outline, so bilinear filtering
    # — and the rounding in a JPEG's 8x8 blocks — drags backdrop onto the shell's
    # edge. Bleed the shell outward into the transparent region first so there
    # is nothing there to drag in.
    th = int(round(TEX_W * H / W))
    rgbf = np.asarray(im.convert("RGB").resize((TEX_W, th), Image.LANCZOS), dtype=np.float32)
    known = np.asarray(Image.fromarray((mask * 255).astype(np.uint8))
                       .resize((TEX_W, th), Image.NEAREST), dtype=bool)
    for _ in range(BLEED_PX):
        if known.all():
            break
        acc = np.zeros_like(rgbf)
        cnt = np.zeros((th, TEX_W), dtype=np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            s = np.roll(np.roll(rgbf * known[..., None], dy, 0), dx, 1)
            c = np.roll(np.roll(known.astype(np.float32), dy, 0), dx, 1)
            acc += s
            cnt += c
        grow = (~known) & (cnt > 0)
        rgbf[grow] = acc[grow] / cnt[grow, None]
        known |= grow
    tex = Image.fromarray(np.clip(rgbf, 0, 255).astype(np.uint8))
    tex.save(os.path.join(OUT_ASSETS, "chassis.jpg"), quality=JPEG_Q, optimize=True)

    # ---- the layout header ----------------------------------------------
    # screen.cfg is in photo pixels: the LCD square's left, top and side.
    with open(os.path.join(SRC, "screen.cfg")) as f:
        sx, sy, ss = (float(v) for v in f.read().split())
    s_l, s_t = to_model((sx, sy))
    s_r, s_b = to_model((sx + ss, sy + ss))

    hdr = f"""/*
 * Mote VR — where the console's parts are, in model space.
 *
 * GENERATED by vr/tools/gen_vr_chassis.py from studio/assets/thumby_color.png
 * and studio/assets/screen.cfg. Do not edit; re-run the tool.
 *
 * Metres, life size, origin at the centre of the slab, +X right, +Y up, +Z out
 * of the screen towards the player. The button positions are the same
 * normalised photo coordinates the phone shell hit-tests against
 * (platform/android/mote_shell.c), converted to model space, so a press lands
 * on the button you can see in both.
 */
#ifndef MOTE_VR_CHASSIS_LAYOUT_H
#define MOTE_VR_CHASSIS_LAYOUT_H

#define MOTE_VR_CHASSIS_W    {REAL_W:.6f}f    /* {REAL_W*1000:.1f} mm */
#define MOTE_VR_CHASSIS_H    {real_h:.6f}f    /* {real_h*1000:.1f} mm */
#define MOTE_VR_CHASSIS_T    {REAL_T:.6f}f    /* {REAL_T*1000:.1f} mm */
#define MOTE_VR_CHASSIS_HALF {half:.6f}f    /* front face z */

/* The LCD's 128x128 active square. */
#define MOTE_VR_SCREEN_L  {s_l:.6f}f
#define MOTE_VR_SCREEN_R  {s_r:.6f}f
#define MOTE_VR_SCREEN_B  {s_b:.6f}f
#define MOTE_VR_SCREEN_T  {s_t:.6f}f

/* Button discs, for the press-feedback glow. x, y, radius. Order matches
 * MoteBtnId's UP..MENU only loosely — see mote_vr_render.c. */
"""
    # the shell's own normalised geometry (fractions of photo width/height)
    buttons = [
        ("A",     0.901, 0.442, 0.062),
        ("B",     0.793, 0.510, 0.062),
        ("MENU",  0.196, 0.790, 0.050),
        ("DPAD",  0.153, 0.471, 0.115),
    ]
    for name, cx, cy, rr in buttons:
        mx, my = to_model((cx * W, cy * H))
        hdr += f"#define MOTE_VR_BTN_{name}_X {mx:+.6f}f\n"
        hdr += f"#define MOTE_VR_BTN_{name}_Y {my:+.6f}f\n"
        hdr += f"#define MOTE_VR_BTN_{name}_R {rr*REAL_W:.6f}f\n"
    hdr += "\n#endif /* MOTE_VR_CHASSIS_LAYOUT_H */\n"

    os.makedirs(os.path.dirname(OUT_HDR), exist_ok=True)
    with open(OUT_HDR, "w") as f:
        f.write(hdr)

    print(f"wrote vr/assets/chassis.mvm ({os.path.getsize(os.path.join(OUT_ASSETS,'chassis.mvm'))} B), "
          f"chassis.jpg ({os.path.getsize(os.path.join(OUT_ASSETS,'chassis.jpg'))} B), "
          f"platform/vr/mote_vr_chassis.h")
    return 0


if __name__ == "__main__":
    sys.setrecursionlimit(10000)
    sys.exit(main())
