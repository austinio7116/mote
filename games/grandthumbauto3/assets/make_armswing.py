#!/usr/bin/env python3
"""Calm the walk cycle's arm swing without removing it.

The four walk frames differ by up to 2 px at the shoulder and hand rows, and on
some rows a frame is 2 px WIDER than its neighbours rather than shifted -- so a
small block appears at the end of an arm, vanishes, and appears on the other
side. That reads as the hands waving rather than the arms swinging.

The rule is per facing column and per pixel row: take the four walk frames'
spans, use the median edge as neutral, and trim any frame that reaches more
than ONE pixel past it. Movement is kept (a pixel of swing each way, in
antiphase, which is what an arm does) and the 2 px lurch is gone.

Only the walk rows are touched. The player sheet's rows 4 and 5 are the firing
poses, where the arm reaching out IS the pose.

ONE-SHOT. Running it twice trims twice. The committed PNGs are the source of
truth; recover an original with `git show HEAD:<path>` before re-running.
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.abspath(__file__))
KEY = lambda r, g, b, a: a == 0 or (r > 200 and b > 200 and g < 80)

def median(vals):
    v = sorted(vals)
    return v[len(v) // 2]

def calm(path, walk_rows):
    im = Image.open(path).convert("RGBA")
    px = im.load()
    W, H = im.size
    cols = W // 16
    blank = (0, 0, 0, 0)
    cut = 0
    for ccol in range(cols):
        for y in range(16):
            spans = []
            for crow in walk_rows:
                xs = [x for x in range(16)
                      if not KEY(*px[ccol * 16 + x, crow * 16 + y])]
                spans.append((min(xs), max(xs)) if xs else None)
            live = [s for s in spans if s]
            if len(live) < 2:
                continue
            lo_n = median([s[0] for s in live])
            hi_n = median([s[1] for s in live])
            for crow, sp in zip(walk_rows, spans):
                if not sp:
                    continue
                lo, hi = sp
                while lo < lo_n - 1:
                    px[ccol * 16 + lo, crow * 16 + y] = blank; lo += 1; cut += 1
                while hi > hi_n + 1:
                    px[ccol * 16 + hi, crow * 16 + y] = blank; hi -= 1; cut += 1
    im.save(path)
    print("calmed %s (%d pixels trimmed)" % (os.path.basename(path), cut))

calm(f"{ROOT}/player.png", walk_rows=(0, 1, 2, 3))   # 4 and 5 are the firing poses
calm(f"{ROOT}/ped.png",    walk_rows=tuple(range(Image.open(f"{ROOT}/ped.png").size[1] // 16)))
calm(f"{ROOT}/cop.png",    walk_rows=tuple(range(Image.open(f"{ROOT}/cop.png").size[1] // 16)))
