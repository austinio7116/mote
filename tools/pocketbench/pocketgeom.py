#!/usr/bin/env python3
"""EVERY SHIPPED POCKET, IN PLAN, WITH THE NUMBERS IT WAS BUILT FROM.

Not remembered numbers and not a picture of a picture: every line here comes out
of `cuepocket --plan`, which builds the table with cue_table_build_world and
measures what it actually made. Run it after changing an authored value and the
sheet changes with it.

What is drawn, per pocket:

    the NOSE            yellow where it is a straight rail (kind 0), orange
                        where it is a jaw (kind 1). The join is the yellow point.
    the MOUTH           a red bar across the true narrowest passage, which is
                        what decides whether a ball fits — not the jaw-tip line.
    the DROP            green: the ball's centre inside it and the ball is down.
    the RIM             cyan: the flat cloth round the hole (cut_rad x mouth).
    the ROLL            cyan dashed: where that cloth turns over the edge.
    the BORE            brown: the hole cut through the timber.
    the JAW CIRCLES     grey: cue_table's rattle end-caps. Drawn because where
                        they sit relative to the nose is the thing worth seeing
                        — the AI's aim used to be computed from these.
    a BALL              white, on the mouth's centre line, to scale.

Every authored field is printed under each panel, with what it comes to in
millimetres and in ball widths where that is the meaningful unit.

    pocketgeom.py                        # every shipped table, corners
    pocketgeom.py --kind middle
    pocketgeom.py --tables snooker12 uk7 --out /tmp/sheet.png
"""
import argparse
import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOTE = os.path.normpath(os.path.join(HERE, "..", ".."))
BIN  = os.path.join(MOTE, "build_host", "cuepocket")

# the shipped tables, in the order a player meets them
TABLES = ["uk7", "snk6", "golf", "us9", "us9ball", "straight",
          "chinese10", "snooker12", "snooker10", "billiards",
          "pyramid", "pyramid7"]

C_BG    = (17, 17, 21)
C_PANEL = (26, 26, 32)
C_GRID  = (42, 42, 50)
C_RAIL  = (255, 214, 92)     # straight nose
C_JAW   = (255, 150, 60)     # curved jaw
C_MOUTH = (255, 70, 70)
C_DROP  = (100, 255, 150)
C_RIM   = (110, 215, 255)
C_BORE  = (168, 110, 66)
C_CIRC  = (130, 130, 145)
C_BALL  = (245, 245, 240)
C_TXT   = (232, 232, 238)
C_DIM   = (150, 150, 162)
C_WARN  = (255, 170, 90)


def plan(table, kind):
    r = subprocess.run([BIN, "--table", table, "--type", kind,
                        "--plan", "--out", os.devnull],
                       capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        return None
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        return None


def dashed(D, a, b, col, on=5, off=4, width=1):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = math.hypot(dx, dy)
    if L < 1e-6:
        return
    ux, uy, t = dx / L, dy / L, 0.0
    while t < L:
        t2 = min(L, t + on)
        D.line([(a[0] + ux * t, a[1] + uy * t),
                (a[0] + ux * t2, a[1] + uy * t2)], fill=col, width=width)
        t += on + off


def ring(D, T, cx, cz, r, col, width=2, dash=False):
    pts = [T(cx + r * math.cos(a * math.pi / 32),
             cz + r * math.sin(a * math.pi / 32)) for a in range(65)]
    for i in range(64):
        if dash and i % 2:
            continue
        D.line([pts[i], pts[i + 1]], fill=col, width=width)


def draw(D, d, x0, y0, px, span):
    """One pocket, in its own frame: origin at the pocket point."""
    spm = px / span
    def T(x, z):
        return (x0 + px * 0.5 + x * spm, y0 + px * 0.5 + z * spm)

    D.rectangle([x0, y0, x0 + px, y0 + px], fill=C_PANEL, outline=(44, 44, 54))
    # a 10 mm grid, so anything can be read off by eye
    g = 10.0
    v = -span
    while v <= span:
        D.line([T(v, -span), T(v, span)], fill=C_GRID)
        D.line([T(-span, v), T(span, v)], fill=C_GRID)
        v += g

    # the timber's bore, then the cut rings, then the drop: outside in
    b = d["bore"]
    ring(D, T, b["x"], b["z"], b["r"], C_BORE, 2)
    c = d["cut"]
    ring(D, T, c["cx"], c["cz"], c["rim_r"], C_RIM, 2)
    ring(D, T, c["cx"], c["cz"], c["rim_r"] + c["roll"], C_RIM, 1, dash=True)
    dr = d["drop"]
    ring(D, T, dr["x"], dr["z"], dr["r"], C_DROP, 2)

    # the rattle circles, so where they sit relative to the nose is visible
    for j in d.get("jaws", []):
        ring(D, T, j["x"], j["z"], j["r"], C_CIRC, 1)
        p = T(j["x"], j["z"])
        D.line([(p[0] - 3, p[1]), (p[0] + 3, p[1])], fill=C_CIRC)
        D.line([(p[0], p[1] - 3), (p[0], p[1] + 3)], fill=C_CIRC)

    # the nose, by kind
    for s in d["nose"]:
        col = C_RAIL if s["kind"] == 0 else C_JAW
        D.line([T(s["ax"], s["az"]), T(s["bx"], s["bz"])],
               fill=col, width=3 if s["kind"] == 1 else 2)

    # a ball to scale, on the mouth's centre line
    R = d["ball"] * 0.5
    ring(D, T, dr["x"], dr["z"], R, C_BALL, 1)

    # the pocket point itself
    p = T(0, 0)
    D.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=C_DIM)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tables", nargs="*", default=TABLES)
    ap.add_argument("--kind", default="corner", choices=("corner", "middle", "both"))
    ap.add_argument("--px", type=int, default=340, help="panel size in pixels")
    ap.add_argument("--span", type=float, default=0.0,
                    help="window in mm; 0 picks one that fits every panel")
    ap.add_argument("--out", default=os.path.join(MOTE, "pocketgeom.png"))
    a = ap.parse_args()

    if not os.path.exists(BIN):
        sys.exit("build cuepocket first:\n  see tools/pocketbench/README.md")

    kinds = ("corner", "middle") if a.kind == "both" else (a.kind,)
    cells = []
    for t in a.tables:
        for k in kinds:
            d = plan(t, k)
            if d:
                cells.append(d)
            else:
                print("  (no %s %s — the table has none, or it failed)" % (t, k))
    if not cells:
        sys.exit("nothing to draw")

    # one window for every panel, so the sheet compares shapes and not scales
    span = a.span
    if span <= 0.0:
        need = 0.0
        for d in cells:
            for s in d["nose"]:
                need = max(need, abs(s["ax"]), abs(s["az"]),
                                 abs(s["bx"]), abs(s["bz"]))
            need = max(need, d["cut"]["rim_r"] + d["cut"]["roll"])
        span = need * 2.2

    from PIL import Image, ImageDraw
    cols = min(5, len(cells))
    rows = (len(cells) + cols - 1) // cols
    pad, top, cap = 8, 56, 132
    W = cols * a.px + pad * (cols + 1)
    H = top + rows * (a.px + cap + pad)
    img = Image.new("RGB", (W, H), C_BG)
    D = ImageDraw.Draw(img)

    D.text((pad, 8), "POCKET GEOMETRY, AS BUILT — every number measured out of "
                     "cue_table_build_world, none remembered", fill=(255, 205, 120))
    D.text((pad, 24), "nose: yellow = straight rail, orange = jaw.   rings: "
                      "green = drop, cyan = rim (dashed = roll), brown = bore, "
                      "grey = rattle circles.   white = a ball, to scale.",
           fill=C_DIM)
    D.text((pad, 38), "window %.0f mm across, 10 mm grid." % span, fill=C_DIM)

    for i, d in enumerate(cells):
        cx, cy = i % cols, i // cols
        x0 = pad + cx * (a.px + pad)
        y0 = top + cy * (a.px + cap + pad)
        draw(D, d, x0, y0, a.px, span)

        ball = d["ball"]
        au, cu = d["authored"], d["cut"]
        bw = d["mouth"] / ball if ball else 0.0
        ty = y0 + a.px + 4
        D.text((x0 + 3, ty), "%s  %s" % (d["table"], d["kind"]),
               fill=(150, 235, 180)); ty += 14
        D.text((x0 + 3, ty), "mouth %.1f mm = %.2f BALLS   (ball %.1f)"
               % (d["mouth"], bw, ball),
               fill=C_TXT if d["mouth"] > 0 else C_WARN); ty += 13
        D.text((x0 + 3, ty), "authored pr %.1f   off %.1f   cap %.1f   gap %.1f"
               % (au["pr"], au["off"], au["cap"], au["gap"]), fill=C_DIM); ty += 13
        D.text((x0 + 3, ty), "jaw  p0 %.0f  h1 %.0f  h2 %.0f  ang %.0f  r %.1f  %s"
               % (d["jp0"], d["jh1"], d["jh2"], au["jaw_ang"], au["jaw_r"],
                  "rounded" if d["round"] else "mitred"), fill=C_DIM); ty += 13
        D.text((x0 + 3, ty), "drop r %.1f at %+.1f,%+.1f   back %.1f"
               % (d["drop"]["r"], d["drop"]["x"], d["drop"]["z"], au["drop_back"]),
               fill=C_DIM); ty += 13
        D.text((x0 + 3, ty), "rim  %.1f mm = %.3f x mouth = %.2f BALLS"
               % (cu["rim_r"], cu["rad_x"], cu["rim_r"] / ball if ball else 0),
               fill=C_TXT); ty += 13
        D.text((x0 + 3, ty), "roll %.1f mm = %.3f x mouth   set %.1f   arc %.0f deg"
               % (cu["roll"], cu["roll_x"], cu["set"], cu["arc"]), fill=C_DIM); ty += 13
        D.text((x0 + 3, ty), "bore %.1f at %+.1f,%+.1f   rail %.0f   cush %.1f"
               % (d["bore"]["r"], d["bore"]["x"], d["bore"]["z"],
                  d["rail_w"], d["cush"]), fill=C_DIM)

    img.save(a.out)
    print(a.out)

    # ...and the same thing as a table, because a number you want to compare
    # across ten tables should not have to be read off ten pictures
    print()
    hdr = ("%-11s %-7s %8s %7s %8s %8s %8s %7s %7s %6s"
           % ("table", "kind", "mouth", "BALLS", "rim", "rim/mth", "rimBALLS",
              "roll", "drop r", "jawang"))
    print(hdr); print("-" * len(hdr))
    for d in cells:
        ball, cu = d["ball"], d["cut"]
        print("%-11s %-7s %7.1f %7.2f %7.1f %8.3f %8.2f %7.1f %7.1f %6.0f"
              % (d["table"], d["kind"], d["mouth"], d["mouth"] / ball,
                 cu["rim_r"], cu["rad_x"], cu["rim_r"] / ball, cu["roll"],
                 d["drop"]["r"], d["authored"]["jaw_ang"]))


if __name__ == "__main__":
    main()
