#!/usr/bin/env python3
"""Bake several hill palettes and photograph each one at both zoom levels, for choosing from.

Hills are brown because they share the bd_dry band with farmland and tundra, and the god's-eye
pixel is C_BROWN to match. "Less brown" is therefore two different questions — put hills in a
different band, or change what that band's brown is — and both answers change other ground with
them. So each candidate below says exactly what it touches, and every one is BUILT and SHOT
rather than described: two pictures each, the whole world at one pixel per cell and hill country
close up, from the same seed at the same frame.

    python3 authoring/hill_palettes.py            # -> /tmp/motebox_hills.png
    python3 authoring/hill_palettes.py current    # just re-shoot what is in the tree

It restores src/mb_draw.c when it is done, so the tree is left exactly as it was found.
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

# name, what it does, band for hills, bd_dry body override, bd_dry fringe override, god pixel
CANDS = [
    ("as now", "hills, farm and tundra all in the red-brown bd_dry band",
     "bd_dry", "", "", (171, 82, 54)),
    ("dun", "the same band, but a dry khaki instead of red-brown (farm follows)",
     "bd_dry", "150,132,96", "196,180,132", (150, 132, 96)),
    ("olive", "bd_dry as scrubby olive (farm follows)",
     "bd_dry", "122,124,74", "168,168,104", (122, 124, 74)),
    ("moor", "bd_dry as heather over peat (farm follows)",
     "bd_dry", "128,104,112", "170,140,150", (128, 104, 112)),
    ("green uplands", "hills join the GRASS band; only the shaded relief says hill",
     "bd_green", "", "", (0, 128, 60)),
    ("stony", "hills join the ROCK band with the peaks; farm keeps its brown",
     "bd_rock", "", "", (140, 140, 146)),
]


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


def set_god_colour(rgb):
    """The god's-eye pixel is a compile-time constant, so it is patched in place and put back."""
    s = open(DRAW).read()
    s = re.sub(r"#define MB_HILL_COL .*", "#define MB_HILL_COL MOTE_RGB565(%d, %d, %d)" % rgb, s)
    open(DRAW, "w").write(s)


def shoot(tag, band, body, fringe, rgb):
    set_god_colour(rgb)
    env = {"MB_HILL_BAND": band, "MB_DRY_BODY": body, "MB_DRY_FRINGE": fringe}
    sh("python3 authoring/extract_box.py", env=env, cwd=GAME)
    sh("./tools/mote bake games/motebox")
    sh("./tools/mote build games/motebox")
    common = ("SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MOTE_AUTORUN=1 MOTE_DT_MS=33 "
              "MOTEBOX_SEED=%s MOTEBOX_SEEDN=5 MOTEBOX_YEARS=0 MOTEBOX_NOFOLLOW=1 " % SEED)
    out = {}
    for view, extra in (("god", ""), ("mortal", "MOTEBOX_MORTAL=1 MOTEBOX_CAM=h ")):
        p = "/tmp/hill_%s_%s.png" % (tag, view)
        sh(common + extra + "MOTE_SHOT=%s MOTE_SHOT_FRAME=%s ./tools/mote run games/motebox "
           "> /dev/null" % (p, FRAME))
        out[view] = p
    return out


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    backup = DRAW + ".hillbak"
    shutil.copy(DRAW, backup)
    shots = []
    try:
        for name, note, band, body, fringe, rgb in CANDS:
            tag = name.split()[0].lower()
            if only and only != tag:
                continue
            print("[hills] %s — %s" % (name, note))
            shots.append((name, note, shoot(tag, band, body, fringe, rgb)))
    finally:
        shutil.move(backup, DRAW)
        # and put the tree's own art back, so the next build is the real one
        sh("python3 authoring/extract_box.py", cwd=GAME)
        sh("./tools/mote bake games/motebox")
        sh("./tools/mote build games/motebox")

    # THREE ACROSS AND TWO DOWN, at three times size. The first version put six candidates in
    # one row and the pictures came out too small to judge a ground tone from, which is the only
    # thing they exist for.
    Z = 3
    W = 128 * Z
    pair = W * 2 + 8
    cols = 3
    rows = (len(shots) + cols - 1) // cols
    colw = pair + 22
    rowh = W + 52
    out = Image.new("RGB", (cols * colw + 16, rows * rowh + 34), (26, 28, 38))
    d = ImageDraw.Draw(out)
    d.text((10, 8), "hill palettes at year 0 — the whole world (left) and hill country close up "
                    "(right), seed %s" % SEED, fill=(240, 208, 96))
    for i, (name, note, s) in enumerate(shots):
        x = 10 + (i % cols) * colw
        y = 30 + (i // cols) * rowh
        d.text((x, y), name, fill=(240, 240, 250))
        d.text((x, y + 14), note[:68], fill=(150, 158, 180))
        for j, view in enumerate(("god", "mortal")):
            im = Image.open(s[view]).resize((W, W), Image.NEAREST)
            out.paste(im, (x + j * (W + 8), y + 30))
    out.save("/tmp/motebox_hills.png")
    print("wrote /tmp/motebox_hills.png", out.size)


if __name__ == "__main__":
    main()
