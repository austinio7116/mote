#!/usr/bin/env python3
"""Narrow the character sprites by one pixel each side of every WIDE row.

The billboard quad takes its aspect straight from the source rect
(mote_scene3d.c: q->hw = q->hh * fw/fh), so there is no way to draw the same
pixels in a narrower quad. Past passes trimmed the rect from 16 to 12 to 10
columns, which is now the figure's own measured extent -- 3..12 on the
front/back cells. Any further narrowing has to come out of the ART.

What is actually wide is the torso-plus-arms block: the head is 4 px and the
legs 6, but rows 7..9 span the full 10. This erodes one occupied pixel from
each end of every row that is 9 px or wider, which pulls the arms in and leaves
the head, legs and every already-narrow row untouched.

The player sheet's firing rows (4 and 5) are skipped: the gun arm reaching to
column 15 is the pose, and eroding it clips the weapon.

ONE-SHOT. Running it twice erodes twice. The committed PNGs are the source of
truth; recover an original with `git show HEAD:<path>` before re-running.
"""
import os, sys
from PIL import Image

ROOT = os.path.dirname(os.path.abspath(__file__))
WIDE = 9                 # erode rows at least this wide
KEY  = lambda r, g, b, a: a == 0 or (r > 200 and b > 200 and g < 80)

def slim(path, skip_rows=()):
    im = Image.open(path).convert("RGBA")
    px = im.load()
    W, H = im.size
    blank = (0, 0, 0, 0)
    cut = 0
    for crow in range(H // 16):
        if crow in skip_rows:
            continue
        for ccol in range(W // 16):
            for y in range(16):
                gy = crow * 16 + y
                xs = [x for x in range(16)
                      if not KEY(*px[ccol * 16 + x, gy])]
                if len(xs) < WIDE:
                    continue
                lo, hi = min(xs), max(xs)
                if hi - lo + 1 < WIDE:
                    continue
                px[ccol * 16 + lo, gy] = blank
                px[ccol * 16 + hi, gy] = blank
                cut += 2
    im.save(path)
    print("slimmed %s (%d pixels removed)" % (os.path.basename(path), cut))

slim(f"{ROOT}/player.png", skip_rows=(4, 5))   # rows 4/5 are the firing poses
slim(f"{ROOT}/ped.png")
slim(f"{ROOT}/cop.png")
