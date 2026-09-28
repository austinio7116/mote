#!/usr/bin/env python3
"""Pin rows that TRANSLATE across the walk cycle instead of swinging.

make_slim.py narrowed the figures and make_armswing.py capped each frame's
deviation from the median at one pixel. Neither caught this: a row whose span
is the same WIDTH in every frame but sits at a different POSITION is not an arm
swinging, it is a block of the body sliding sideways. The shoulder line (row 6
of the front and back cells) slides two pixels across the cycle while the torso
directly below it does not move at all, and at running speed -- the sprint
plays the same four frames at 13 fps instead of 8 -- that reads as the hands
popping in and out.

Per-frame deviation from the median was within one pixel on every frame, which
is why the earlier pass left it alone; the damage is in the frame-to-frame
distance, not the distance from the middle.

The rule here: for each facing column and each pixel row, if the span width is
identical in all four frames but the position varies by two or more, copy the
median frame's pixels for that row into every frame. Rows that change WIDTH are
left alone -- that is a real arm or leg extending, which is the animation.

ONE-SHOT, though re-running is harmless: once the rows are pinned they no
longer vary, so nothing matches and nothing changes.
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.abspath(__file__))
KEY  = lambda r, g, b, a: a == 0 or (r > 200 and b > 200 and g < 80)

def pin(path, walk_rows):
    im = Image.open(path).convert("RGBA")
    px = im.load()
    W, H = im.size
    pinned = 0
    for ccol in range(W // 16):
        for y in range(16):
            spans = []
            for crow in walk_rows:
                xs = [x for x in range(16)
                      if not KEY(*px[ccol * 16 + x, crow * 16 + y])]
                spans.append((min(xs), max(xs)) if xs else None)
            if any(s is None for s in spans):
                continue
            widths = {hi - lo for lo, hi in spans}
            if len(widths) != 1:
                continue                       # width changes: a real swing
            los = [lo for lo, _ in spans]
            if max(los) - min(los) < 2:
                continue                       # a one-pixel shift is fine
            # the median frame is the one to keep
            order = sorted(range(len(spans)), key=lambda i: spans[i][0])
            src = walk_rows[order[len(order) // 2]]
            for crow in walk_rows:
                if crow == src:
                    continue
                for x in range(16):
                    px[ccol * 16 + x, crow * 16 + y] = px[ccol * 16 + x, src * 16 + y]
            pinned += 1
    im.save(path)
    print("pinned %d translating rows in %s" % (pinned, os.path.basename(path)))

pin(f"{ROOT}/player.png", walk_rows=(0, 1, 2, 3))   # 4 and 5 are the firing poses
pin(f"{ROOT}/ped.png",    walk_rows=tuple(range(Image.open(f"{ROOT}/ped.png").size[1] // 16)))
pin(f"{ROOT}/cop.png",    walk_rows=tuple(range(Image.open(f"{ROOT}/cop.png").size[1] // 16)))
