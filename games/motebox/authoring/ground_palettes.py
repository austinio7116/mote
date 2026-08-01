#!/usr/bin/env python3
"""Bake a set of candidate tones for hill, tundra and savanna, and photograph every one.

Hills and tundra were both the clay red-brown of a ploughed field, because all three shared the
bd_dry band; merging beach and desert into one sand band freed a layer, so hills have a band of
their own and hill, tundra and savanna can each be told apart. What they should each BE is a
choice, so this makes it a choice you can look at: every candidate is built into the game and
photographed twice — the whole world at one pixel per cell, and that ground close up.

    python3 authoring/ground_palettes.py            # all three sets
    python3 authoring/ground_palettes.py hill       # just one

    -> /tmp/motebox_pal_hill.png, _tundra.png, _savanna.png

The tree is restored afterwards: src/mb_draw.c is put back and the shipped art rebaked.
"""
import os
import re
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
DRAW = os.path.join(GAME, "src", "mb_draw.c")
SEED = "39595"
FRAME = "120"

# Each candidate: label, body colour, fringe colour. The body is what the ground reads as; the
# fringe is what its edge bleeds towards, which is what makes a boundary look like a boundary.
SETS = {
    "hill": ("MB_HILL_BODY", "MB_HILL_FRINGE", "MB_HILL_COL", "b:hill", [
        ("clay (as it was)",  (171,  82,  54), (214, 130,  84)),
        ("olive scrub",       (122, 124,  74), (168, 168, 104)),
        ("upland grass",      ( 96, 134,  72), (150, 180, 104)),
        ("bracken",           (150, 118,  62), (200, 168,  96)),
        ("moor grey-green",   (104, 118, 100), (156, 168, 140)),
        ("heather",           (124, 106, 118), (172, 150, 162)),
        ("sage",              (134, 146, 112), (186, 196, 156)),
        ("dry gold",          (176, 150,  86), (222, 202, 138)),
    ]),
    "tundra": ("MB_DRY_BODY", "MB_DRY_FRINGE", "MB_TUNDRA_COL", "b:tundra", [
        ("clay (as it was)",  (171,  82,  54), (214, 130,  84)),
        ("cold earth",        (126, 104,  80), (170, 148, 116)),
        ("lichen grey-green", (128, 134, 112), (176, 182, 156)),
        ("peat",              ( 98,  84,  70), (144, 126, 104)),
        ("ochre moss",        (146, 128,  82), (192, 176, 124)),
        ("frost grey",        (150, 152, 148), (196, 198, 196)),
    ]),
    "savanna": ("MB_SAV_BODY", "MB_SAV_FRINGE", None, "b:savanna", [
        ("orange (as it was)", (255, 163,   0), (255, 236,  39)),
        ("tawny",              (214, 168,  74), (240, 212, 132)),
        ("wheat",              (222, 194, 118), (246, 228, 168)),
        ("dry olive-gold",     (186, 168,  86), (222, 210, 140)),
        ("ochre",              (196, 152,  76), (232, 198, 128)),
    ]),
}
# THE TUNDRA SET MOVES THE WHOLE DRY BAND, because tundra shares it with ploughed farmland, ash
# and scorched earth — all four are bare ground, and the eight autotile layers are spent. So a
# tundra tone is also a farm tone, which is the honest thing to see while choosing it.


def sh(cmd, env=None, cwd=ROOT):
    e = dict(os.environ)
    if env:
        e.update(env)
    r = subprocess.run(cmd, shell=True, cwd=cwd, env=e,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.stdout.write(r.stdout.decode(errors="replace"))
        raise SystemExit("failed: %s" % cmd)
    return r.stdout.decode(errors="replace")


def set_god(define, rgb):
    if not define:
        return
    s = open(DRAW).read()
    s = re.sub(r"#define %s .*" % define,
               "#define %s MOTE_RGB565(%d, %d, %d)" % ((define,) + rgb), s)
    open(DRAW, "w").write(s)


def shoot(tag, i, envkeys, body, fringe, define, cam):
    bkey, fkey = envkeys
    set_god(define, body)
    env = {bkey: "%d,%d,%d" % body, fkey: "%d,%d,%d" % fringe}
    sh("python3 authoring/extract_box.py", env=env, cwd=GAME)
    sh("./tools/mote bake games/motebox")
    sh("./tools/mote build games/motebox")
    common = ("SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MOTE_AUTORUN=1 MOTE_DT_MS=33 "
              "MOTEBOX_SEED=%s MOTEBOX_SEEDN=5 MOTEBOX_YEARS=0 MOTEBOX_NOFOLLOW=1 " % SEED)
    out = {}
    for view, extra in (("god", ""), ("close", "MOTEBOX_MORTAL=1 MOTEBOX_CAM=%s " % cam)):
        p = "/tmp/pal_%s_%d_%s.png" % (tag, i, view)
        sh(common + extra + "MOTE_SHOT=%s MOTE_SHOT_FRAME=%s ./tools/mote run games/motebox "
           "> /dev/null" % (p, FRAME))
        out[view] = p
    return out


def montage(tag, rows):
    Z = 3
    W = 128 * Z
    cols = 4 if len(rows) > 6 else 3
    colw = W * 2 + 8 + 20
    rowh = W + 40
    nrows = (len(rows) + cols - 1) // cols
    out = Image.new("RGB", (cols * colw + 16, nrows * rowh + 34), (26, 28, 38))
    d = ImageDraw.Draw(out)
    d.text((10, 8), "%s tones — the whole world (left) and %s country close up (right), seed %s"
           % (tag, tag, SEED), fill=(240, 208, 96))
    for i, (name, body, shots) in enumerate(rows):
        x = 10 + (i % cols) * colw
        y = 30 + (i // cols) * rowh
        d.text((x, y), "%s   rgb %d,%d,%d" % ((name,) + body), fill=(240, 240, 250))
        for j, view in enumerate(("god", "close")):
            im = Image.open(shots[view]).resize((W, W), Image.NEAREST)
            out.paste(im, (x + j * (W + 8), y + 16))
    p = "/tmp/motebox_pal_%s.png" % tag
    out.save(p)
    print("wrote %s  %s" % (p, out.size))


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    backup = DRAW + ".palbak"
    shutil.copy(DRAW, backup)
    try:
        for tag, (bkey, fkey, define, cam, cands) in SETS.items():
            if only and only != tag:
                continue
            rows = []
            for i, (name, body, fringe) in enumerate(cands):
                print("[%s] %s" % (tag, name))
                rows.append((name, body,
                             shoot(tag, i, (bkey, fkey), body, fringe, define, cam)))
                shutil.copy(backup, DRAW)          # each candidate starts from the tree's own C
            montage(tag, rows)
    finally:
        shutil.move(backup, DRAW)
        sh("python3 authoring/extract_box.py", cwd=GAME)
        sh("./tools/mote bake games/motebox")
        sh("./tools/mote build games/motebox")


if __name__ == "__main__":
    main()
