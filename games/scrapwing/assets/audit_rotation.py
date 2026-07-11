#!/usr/bin/env python3
"""Rotation audit: flag ship sprites whose face-RIGHT orientation looks doubtful.

Re-runs the extractor's orientation decision on the raw sources and flags cells
where the call was ambiguous or contradicted by engine-flame position:
  A  near-square aspect AND small mirror-symmetry margin (coin-flip decision)
  F  flame-coloured pixels (engine glow) NOT on the left/tail after orienting
Writes a numbered review grid (catalogue numbers, matching the in-game info
page) to /tmp/scrapwing_rotation_audit.png. Confirmed-bad indices go into
ROTATION_FIX in extract_sheets.py (as catalogue# - 1).
"""
import os
import numpy as np
from PIL import Image, ImageDraw
from extract_sheets import extract, orient_right, shrink_by, SRC, PLAYER_IDX

def flame_side(spr):
    """Centroid x (0..1) of bright orange/yellow 'engine flame' pixels; None if few."""
    a = np.asarray(spr).astype(int)
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    flame = (al >= 128) & (r > 170) & (g > 80) & (b < 140) & (r > b + 60)
    n = flame.sum()
    if n < 3:
        return None
    xs = np.where(flame)[1]
    return xs.mean() / max(1, spr.width - 1)

def main():
    raw = extract(os.path.join(SRC, "ships_src.png"))
    s = float(np.median([max(t.size) for t in raw])) / 16.0
    flags = {}
    for i, spr in enumerate(raw):
        w, h = spr.size
        aspect = w / h
        a = np.asarray(spr).astype(int)
        sv = np.abs(a - a[:, ::-1]).mean()
        sh = np.abs(a - a[::-1, :]).mean()
        margin = abs(sv - sh) / max(sv, sh, 1e-6)
        reasons = []
        if 0.87 < aspect < 1.15 and margin < 0.12:
            reasons.append("A")                      # ambiguous call
        fs = flame_side(orient_right(spr))
        if fs is not None and fs > 0.55:
            reasons.append("F")                      # flame on the nose side?!
        if reasons:
            flags[i] = "".join(reasons)

    print("flagged %d of %d" % (len(flags), len(raw)))
    final = [orient_right(shrink_by(t, s, cap=16)) for t in raw]
    idxs = sorted(flags)
    cols, cell = 10, 76
    rows = (len(idxs) + cols - 1) // cols
    img = Image.new("RGB", (cols * cell, rows * cell), (24, 24, 36))
    d = ImageDraw.Draw(img)
    for k, i in enumerate(idxs):
        x, y = (k % cols) * cell, (k // cols) * cell
        t = final[i].resize((final[i].width * 4, final[i].height * 4), Image.NEAREST)
        img.paste(t.convert("RGB"), (x + (cell - t.width) // 2, y + 12), t)
        d.text((x + 3, y + 1), "#%d %s%s" % (i + 1, flags[i],
               " P" if i == PLAYER_IDX else ""), fill=(255, 220, 90))
        d.rectangle([x, y, x + cell - 1, y + cell - 1], outline=(50, 50, 70))
    img.save("/tmp/scrapwing_rotation_audit.png")
    print("wrote /tmp/scrapwing_rotation_audit.png")

    # the rest: everything NOT flagged, for a full-coverage eyeball pass
    ok_idxs = [i for i in range(len(raw)) if i not in flags]
    cols2, cell2 = 14, 58
    rows2 = (len(ok_idxs) + cols2 - 1) // cols2
    img2 = Image.new("RGB", (cols2 * cell2, rows2 * cell2), (24, 24, 36))
    d2 = ImageDraw.Draw(img2)
    for k, i in enumerate(ok_idxs):
        x, y = (k % cols2) * cell2, (k // cols2) * cell2
        t = final[i].resize((final[i].width * 3, final[i].height * 3), Image.NEAREST)
        img2.paste(t.convert("RGB"), (x + (cell2 - t.width) // 2, y + 9), t)
        d2.text((x + 2, y), "#%d" % (i + 1), fill=(150, 160, 190))
        d2.rectangle([x, y, x + cell2 - 1, y + cell2 - 1], outline=(50, 50, 70))
    img2.save("/tmp/scrapwing_rotation_ok.png")
    print("wrote /tmp/scrapwing_rotation_ok.png (%d ships)" % len(ok_idxs))

if __name__ == "__main__":
    main()
