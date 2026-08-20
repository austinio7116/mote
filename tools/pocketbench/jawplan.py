#!/usr/bin/env python3
"""THE JAW CURVE, DRAWN FLAT, so it can be laid over a photograph.

The bench renders the real pocket in 3D, which is the right thing for judging
how a pocket LOOKS and the wrong thing for judging a curve: the cushion nose is
a soft boundary between two shades of green, a few pixels wide, and it moves
when the light does. A curve you are trying to match to a reference has to be a
LINE.

So this draws the plan: the cushion nose as a polyline in millimetres, straight
rail in one colour and curved jaw in another, with the bore and the drop circles
for position. Origin is the pocket point and x runs along the rail, so a corner
from any of the four is drawn the same way up and one reference serves all of
them.

Two ways to use it:

  Superimpose candidates, to see what the tip angle does and pick a shape:
      jawplan.py --table snooker12 --sweep "0, 10, 25"

  Trace a photograph, to match a real table:
      jawplan.py --table snooker12 --tipang 8 \
                 --ref ref/snooker_corner.jpg --ref-scale 0.72 --ref-x -40 --ref-y 12

The reference wants scaling and shifting by hand because a photograph has no
scale bar. Work it out from something you know the size of: the mouth is
printed on the drawing, and so is the ball.
"""
import argparse
import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOTE = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = os.path.join(MOTE, "build_host", "cuepocket")

# Colours chosen so a photograph shows through underneath them.
C_BG    = (18, 18, 22)
C_RAIL  = (255, 214, 92)     # the straight nose
C_JAW   = (120, 220, 255)    # the curve being tuned
C_BORE  = (255, 120, 120)
C_DROP  = (120, 255, 150)
C_GRID  = (52, 52, 60)
C_TXT   = (235, 235, 240)
SWEEP_COLS = [(120, 220, 255), (255, 150, 90), (170, 255, 130),
              (255, 120, 220), (200, 200, 255), (255, 255, 140)]


def plan(table, kind, tipang=None, extra=()):
    """Ask the bench for one pocket's plan, in millimetres."""
    cmd = [BIN, "--table", table, "--type", kind, "--plan",
           "--out", os.devnull]
    if tipang is not None:
        cmd += ["--tipang", "%.4f" % tipang]
    cmd += list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        sys.exit("cuepocket --plan failed for %s/%s:\n%s" % (table, kind, r.stderr[-800:]))
    return json.loads(r.stdout)


def bounds(plans, pad=18.0):
    """A window that holds every curve, in mm, kept square so nothing skews."""
    xs, zs = [], []
    for d in plans:
        for s in d["nose"]:
            xs += [s["ax"], s["bx"]]
            zs += [s["az"], s["bz"]]
        for c in ("bore", "drop"):
            xs += [d[c]["x"] - d[c]["r"], d[c]["x"] + d[c]["r"]]
            zs += [d[c]["z"] - d[c]["r"], d[c]["z"] + d[c]["r"]]
    # THE STRAIGHT RAIL RUNS THE WHOLE LENGTH OF THE TABLE and would set the
    # window to a metre and a half, leaving the jaw four pixels across. The
    # interesting part is the pocket, so the window is taken from the CURVE and
    # the rail is simply clipped to it.
    cx, cz = [], []
    for d in plans:
        for s in d["nose"]:
            if s["kind"] == 1:
                cx += [s["ax"], s["bx"]]
                cz += [s["az"], s["bz"]]
    if cx:
        r = max(d["bore"]["r"] for d in plans)
        x0, x1 = min(cx) - pad, max(cx) + pad
        z0, z1 = min(cz) - pad, max(cz) + pad
        # keep the drop circle in shot; it is how you line up on a photograph
        for d in plans:
            x0 = min(x0, d["drop"]["x"] - d["drop"]["r"] - pad)
            x1 = max(x1, d["drop"]["x"] + d["drop"]["r"] + pad)
            z0 = min(z0, d["drop"]["z"] - d["drop"]["r"] - pad)
            z1 = max(z1, d["drop"]["z"] + d["drop"]["r"] + pad)
        del r
    else:
        x0, x1, z0, z1 = min(xs) - pad, max(xs) + pad, min(zs) - pad, max(zs) + pad
    # square it
    w, h = x1 - x0, z1 - z0
    side = max(w, h)
    mx, mz = (x0 + x1) * 0.5, (z0 + z1) * 0.5
    return mx - side / 2, mz - side / 2, side


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", default="snooker12")
    ap.add_argument("--type", default="corner", choices=("corner", "middle"))
    ap.add_argument("--tipang", type=float, default=None,
                    help="degrees off the pocket axis; 0 = parallel throat")
    ap.add_argument("--sweep", default=None,
                    help="comma-separated tip angles drawn on one plan")
    ap.add_argument("--px", type=int, default=1100, help="output size in pixels")
    ap.add_argument("--out", default=None)
    ap.add_argument("--ref", default=None, help="reference photograph to trace")
    ap.add_argument("--ref-scale", type=float, default=1.0)
    ap.add_argument("--ref-x", type=float, default=0.0, help="mm")
    ap.add_argument("--ref-y", type=float, default=0.0, help="mm")
    ap.add_argument("--ref-rot", type=float, default=0.0, help="degrees")
    ap.add_argument("--ref-alpha", type=float, default=0.55)
    ap.add_argument("--flip", action="store_true",
                    help="mirror the drawing, for a photograph of the other hand")
    a = ap.parse_args()

    from PIL import Image, ImageDraw

    cases = []
    if a.sweep:
        for tok in a.sweep.split(","):
            tok = tok.strip()
            if not tok:
                continue
            cases.append((float(tok),))
    else:
        cases.append((a.tipang,))

    plans = [plan(a.table, a.type, *c) for c in cases]
    x0, z0, side = bounds(plans)
    px = a.px
    spm = px / side                       # pixels per mm

    def T(x, z):
        if a.flip:
            x = 2 * (x0 + side / 2) - x
        return ((x - x0) * spm, (z - z0) * spm)

    img = Image.new("RGB", (px, px + 96), C_BG)

    # ---- the reference photograph, underneath everything -------------------
    if a.ref:
        ref = Image.open(a.ref).convert("RGB")
        if a.ref_rot:
            ref = ref.rotate(a.ref_rot, expand=True, resample=Image.BICUBIC)
        rw = max(1, int(ref.width * a.ref_scale))
        rh = max(1, int(ref.height * a.ref_scale))
        ref = ref.resize((rw, rh), Image.LANCZOS)
        # placed by its CENTRE, at the given millimetre offset from the pocket
        # point, so the two controls mean something physical
        cxp, czp = T(a.ref_x, a.ref_y)
        box = (int(cxp - rw / 2), int(czp - rh / 2))
        base = Image.new("RGB", img.size, C_BG)
        base.paste(ref, box)
        img = Image.blend(img, base, max(0.0, min(1.0, a.ref_alpha)))

    d = ImageDraw.Draw(img)

    # ---- a 10 mm grid, so the drawing can be measured ---------------------
    g = 10.0
    gx = math.floor(x0 / g) * g
    while gx < x0 + side:
        p0 = T(gx, z0); p1 = T(gx, z0 + side)
        d.line([p0, p1], fill=C_GRID, width=1)
        gx += g
    gz = math.floor(z0 / g) * g
    while gz < z0 + side:
        p0 = T(x0, gz); p1 = T(x0 + side, gz)
        d.line([p0, p1], fill=C_GRID, width=1)
        gz += g

    # ---- circles, from the first case (they do not depend on the curve) ----
    d0 = plans[0]
    for key, col, nm in (("bore", C_BORE, "bore"), ("drop", C_DROP, "drop")):
        c = d0[key]
        cx, cz = T(c["x"], c["z"])
        r = c["r"] * spm
        d.ellipse([cx - r, cz - r, cx + r, cz + r], outline=col, width=2)

    # ---- THE KNUCKLE CIRCLES, which are what a ball rattles off ----------
    #
    # Worth drawing because once the curve stops bowing into the throat THESE
    # become the narrowest point, and then the pocket's width is set by where
    # they sit rather than by the curve at all.
    for c in d0.get("jaws", []):
        cx, cz = T(c["x"], c["z"])
        r = max(1.5, c["r"] * spm)
        d.ellipse([cx - r, cz - r, cx + r, cz + r], outline=(255, 170, 60), width=2)

    # ---- the nose ---------------------------------------------------------
    for n, dd in enumerate(plans):
        jaw = SWEEP_COLS[n % len(SWEEP_COLS)] if len(plans) > 1 else C_JAW
        for s in dd["nose"]:
            p0 = T(s["ax"], s["az"]); p1 = T(s["bx"], s["bz"])
            if s["kind"] == 0:
                if len(plans) == 1:
                    d.line([p0, p1], fill=C_RAIL, width=3)
            else:
                d.line([p0, p1], fill=jaw, width=3)

    # ---- THE THROAT, measured, at the narrowest ---------------------------
    if "waist" in d0 and d0["waist"]:
        wa = d0["waist"]
        p0 = T(wa["ax"], wa["az"]); p1 = T(wa["bx"], wa["bz"])
        d.line([p0, p1], fill=(255, 80, 80), width=2)
        mx2, mz2 = (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2
        d.text((mx2 + 6, mz2 - 6), "%.1f mm" % d0["mouth"], fill=(255, 120, 120))

    # ---- the legend -------------------------------------------------------
    y = px + 6
    d.text((8, y), "%s %s   mouth %.1f mm   ball %.1f mm   rail %.0f mm   "
                   "grid 10 mm   window %.0f mm"
           % (d0["table"], d0["kind"], d0["mouth"], d0["ball"],
              d0["rail_w"], side), fill=C_TXT)
    y += 16
    if len(plans) > 1:
        for n, dd in enumerate(plans):
            col = SWEEP_COLS[n % len(SWEEP_COLS)]
            d.text((8 + n * 175, y), "tip %+.1f deg   MOUTH %.1f mm"
                   % (dd["tipang"], dd["mouth"]), fill=col)
        y += 16
    else:
        d.text((8, y), "tip angle %+.2f deg   %s jaw"
               % (d0["tipang"],
                  "rounded" if d0["round"] else "MITRED "
                  "(these knobs do nothing on a mitred pocket)"), fill=C_TXT)
        y += 16
        d.text((8, y), "yellow = straight rail nose    blue = the curve being tuned    "
                       "red = bore    green = drop", fill=(150, 150, 160))
        y += 16
    if a.ref:
        d.text((8, y), "ref %s  scale %.3f  x %+.1f mm  y %+.1f mm  rot %.1f deg"
               % (os.path.basename(a.ref), a.ref_scale, a.ref_x, a.ref_y, a.ref_rot),
               fill=(150, 150, 160))

    out = a.out or os.path.join("/tmp", "jawplan_%s_%s.png" % (a.table, a.type))
    img.save(out)
    print(out)


if __name__ == "__main__":
    main()
