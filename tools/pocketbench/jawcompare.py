#!/usr/bin/env python3
"""OLD JAW AGAINST NEW, in plan, drawn from two builds of the engine.

Not from remembered numbers: /tmp/dump_old and /tmp/dump_new are the same
dumper compiled against two trees, so what is drawn is what each version of
cue_table.c actually builds. Each pocket is drawn in its OWN frame — origin at
the pocket point, Y down the pocket's axis into the pocket, X across the throat
— so a corner and a middle, and a 12 ft table and a 7 ft one, all line up on
the same axes and can be compared directly.

    jawcompare.py snooker12 uk7 chinese10 --kind corner --out sheet.png
"""
import argparse
import json
import math
import os
import sys

C_BG   = (18, 18, 22)
C_GRID = (46, 46, 54)
C_AXIS = (78, 78, 92)
C_OLD  = (255, 111, 143)
C_NEW  = (89, 210, 255)
C_RAIL = (255, 214, 92)
C_DROP = (103, 255, 160)
C_JAW  = (255, 160, 60)
C_WAIST= (255, 85, 85)
C_TXT  = (235, 235, 240)
C_DIM  = (150, 150, 162)


def load(tag, table, kind):
    p = "/tmp/%s_%s_%s.json" % (tag, table, "c" if kind == "corner" else "m")
    if not os.path.exists(p):
        sys.exit("missing %s — run the dumpers first" % p)
    return json.load(open(p))


def narrowest(d):
    """The closest approach between the two sides of the mouth, and where."""
    segs = [s for s in d["nose"]]
    left  = [s for s in segs if (s["ax"] + s["bx"]) < 0]
    right = [s for s in segs if (s["ax"] + s["bx"]) > 0]
    circ  = d.get("jaws", [])
    lc = [c for c in circ if c["x"] < 0]
    rc = [c for c in circ if c["x"] > 0]

    def pt_seg(px, pz, s):
        ex, ez = s["bx"] - s["ax"], s["by"] - s["ay"]
        L = ex * ex + ez * ez
        t = ((px - s["ax"]) * ex + (pz - s["ay"]) * ez) / L if L > 1e-9 else 0.0
        t = max(0.0, min(1.0, t))
        return math.hypot(px - (s["ax"] + ex * t), pz - (s["ay"] + ez * t))

    best, pa, pb = 1e30, None, None
    for a in left:
        for b in right:
            for (px, pz, s) in ((a["ax"], a["ay"], b), (a["bx"], a["by"], b),
                                (b["ax"], b["ay"], a), (b["bx"], b["by"], a)):
                dd = pt_seg(px, pz, s)
                if dd < best:
                    best, pa, pb = dd, (px, pz), None
    # circles against circles and against the far side's segments
    for c in lc:
        for c2 in rc:
            dd = math.hypot(c["x"] - c2["x"], c["y"] - c2["y"]) - c["r"] - c2["r"]
            if dd < best:
                best, pa, pb = dd, (c["x"], c["y"]), (c2["x"], c2["y"])
    for cs, other in ((lc, right), (rc, left)):
        for c in cs:
            for s in other:
                dd = pt_seg(c["x"], c["y"], s) - c["r"]
                if dd < best:
                    best, pa, pb = dd, (c["x"], c["y"]), None
    return best, pa, pb


def draw_one(d, col, D, T, width=3, circles=True):
    for s in d["nose"]:
        D.line([T(s["ax"], s["ay"]), T(s["bx"], s["by"])],
               fill=(C_RAIL if s["k"] == 0 else col),
               width=(2 if s["k"] == 0 else width))
    if circles:
        for c in d.get("jaws", []):
            p = T(c["x"], c["y"])
            r = c["r"] * T.spm
            D.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r],
                      outline=C_JAW, width=2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tables", nargs="+")
    ap.add_argument("--kind", default="corner", choices=("corner", "middle"))
    ap.add_argument("--px", type=int, default=520, help="per panel")
    ap.add_argument("--span", type=float, default=0.0,
                    help="window in mm; 0 to fit each panel")
    ap.add_argument("--out", default="/tmp/jawcompare.png")
    a = ap.parse_args()

    from PIL import Image, ImageDraw

    cols, rows = len(a.tables), 2
    pad, top, cap = 8, 52, 74
    W = cols * a.px + pad * (cols + 1)
    H = top + rows * (a.px + cap) + pad * rows
    img = Image.new("RGB", (W, H), C_BG)
    D = ImageDraw.Draw(img)
    D.text((pad, 8), "THE JAW CURVE, %s POCKETS — pink is what the engine built "
                     "before, blue is the four-point curve" % a.kind.upper(),
           fill=(255, 205, 120))
    D.text((pad, 26), "each panel in its own pocket's frame: origin at the pocket "
                      "point, down the page is into the pocket. yellow = straight "
                      "rail nose, orange = jaw circles, green = the drop.",
           fill=C_DIM)

    for ci, tbl in enumerate(a.tables):
        for ri, tag in enumerate(("old", "new")):
            d = load(tag, tbl, a.kind)
            x0 = pad + ci * (a.px + pad)
            y0 = top + ri * (a.px + cap + pad)

            # a window that holds both versions, so the two panels are the same
            # scale and the shapes can be compared rather than just admired
            allpts = []
            for t2 in ("old", "new"):
                dd = load(t2, tbl, a.kind)
                for s in dd["nose"]:
                    if s["k"] == 1:
                        allpts += [(s["ax"], s["ay"]), (s["bx"], s["by"])]
                allpts += [(dd["drop"]["x"] - dd["drop"]["r"], dd["drop"]["y"]),
                           (dd["drop"]["x"] + dd["drop"]["r"], dd["drop"]["y"]),
                           (dd["drop"]["x"], dd["drop"]["y"] - dd["drop"]["r"]),
                           (dd["drop"]["x"], dd["drop"]["y"] + dd["drop"]["r"])]
            xs = [p[0] for p in allpts]; ys = [p[1] for p in allpts]
            span = a.span or (max(max(xs) - min(xs), max(ys) - min(ys)) * 1.12)
            mx, my = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
            spm = a.px / span

            def T(x, y, x0=x0, y0=y0, mx=mx, my=my, spm=spm):
                return (x0 + a.px / 2 + (x - mx) * spm,
                        y0 + a.px / 2 + (y - my) * spm)
            T.spm = spm

            # frame and grid
            D.rectangle([x0, y0, x0 + a.px, y0 + a.px], outline=(40, 40, 48))
            g = 10.0
            v = math.floor((mx - span / 2) / g) * g
            while v < mx + span / 2:
                D.line([T(v, my - span / 2), T(v, my + span / 2)], fill=C_GRID)
                v += g
            v = math.floor((my - span / 2) / g) * g
            while v < my + span / 2:
                D.line([T(mx - span / 2, v), T(mx + span / 2, v)], fill=C_GRID)
                v += g
            # the pocket's axis and the pocket point
            D.line([T(0, my - span / 2), T(0, my + span / 2)], fill=C_AXIS)
            p = T(0, 0)
            D.ellipse([p[0] - 3, p[1] - 3, p[0] + 3, p[1] + 3], fill=C_AXIS)

            # the drop
            c = T(d["drop"]["x"], d["drop"]["y"])
            r = d["drop"]["r"] * spm
            D.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r],
                      outline=C_DROP, width=2)

            # the other version, faint, for shape reference
            other = load("new" if tag == "old" else "old", tbl, a.kind)
            for s in other["nose"]:
                if s["k"] != 1:
                    continue
                D.line([T(s["ax"], s["ay"]), T(s["bx"], s["by"])],
                       fill=(58, 58, 70), width=2)

            draw_one(d, C_OLD if tag == "old" else C_NEW, D, T)

            w, pa, pb = narrowest(d)
            if pa and pb:
                D.line([T(*pa), T(*pb)], fill=C_WAIST, width=2)

            lab = "%s  %s  %s" % (tbl, a.kind, tag.upper())
            D.text((x0 + 6, y0 + 4), lab,
                   fill=(C_OLD if tag == "old" else C_NEW))
            ty = y0 + a.px + 5
            D.text((x0 + 4, ty), "narrowest %.1f mm   ball %.1f   %+.1f mm spare"
                   % (w, d["ball"], w - d["ball"]), fill=C_TXT); ty += 15
            D.text((x0 + 4, ty), "drop r %.1f at y %+.1f   rail %.0f   cush %.1f"
                   % (d["drop"]["r"], d["drop"]["y"], d["railw"], d["cush"]),
                   fill=C_DIM); ty += 15
            jw = [s for s in d["nose"] if s["k"] == 1]
            if jw:
                ends = [(s["ax"], s["ay"]) for s in jw] + [(s["bx"], s["by"]) for s in jw]
                far = max(ends, key=lambda q: q[0] * q[0] + q[1] * q[1])
                near = min(ends, key=lambda q: q[0] * q[0] + q[1] * q[1])
                D.text((x0 + 4, ty), "curve runs %.0f mm out to %.0f mm out, %d segs"
                       % (math.hypot(*near), math.hypot(*far), len(jw)),
                       fill=C_DIM); ty += 15
                D.text((x0 + 4, ty), "its ends sit at x %+.1f and y %+.1f"
                       % (near[0], near[1]), fill=C_DIM)

    img.save(a.out)
    print(a.out)


if __name__ == "__main__":
    main()
