#!/usr/bin/env python3
"""Bake the chassis photo for the APK.

studio/assets/thumby_color.png is a 2872x1668 RGBA photo — 3.6 MB, and lossless
PNG is the wrong format for a photograph. With no games bundled it was most of
the download, so the APK gets its own copy:

  · scaled to SCALE, chosen so the source is still at least as tall as the
    tallest phone we care about (a Pixel 9 Pro is 1280 tall in landscape, and
    the shell fits the chassis to the window height — so anything above that is
    pixels the screen cannot show)
  · RGB as a JPEG, alpha as a separate 8-bit PNG. stb_image reads both and the
    shell recombines them; JPEG has no alpha channel of its own, and the mask
    compresses to almost nothing because it is a clean cut-out
  · screen.cfg scaled by the same factor, since its calibration is in
    photo pixels and every layout number is derived from it

Measured at the Pixel 9 Pro's display size the JPEG differs from the original by
0.66/255 on average, which is invisible; the ~0.5% of pixels that move more than
8 levels are on the hard shell edge, where the alpha mask hides the ringing.

Run from the repo root:  python3 android/tools/gen_chassis.py
"""
import os
import sys

from PIL import Image

SCALE = 0.8          # 2872x1668 -> 2297x1334, comfortably above 1280
JPEG_Q = 88

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "studio", "assets")
OUT = os.path.join(ROOT, "android", "assets")


def main():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(SRC, "thumby_color.png")
    im = Image.open(src).convert("RGBA")
    w, h = int(im.width * SCALE), int(im.height * SCALE)
    if h < 1280:
        sys.exit("refusing to bake below 1280 tall: a Pixel 9 Pro would upscale it")
    r = im.resize((w, h), Image.LANCZOS)

    jpg = os.path.join(OUT, "thumby_color.jpg")
    msk = os.path.join(OUT, "thumby_color_a.png")
    # subsampling=0: the shell is a flat saturated purple, and chroma
    # subsampling shows on its edges before it shows anywhere else.
    r.convert("RGB").save(jpg, quality=JPEG_Q, subsampling=0, optimize=True)
    r.getchannel("A").save(msk, optimize=True)

    with open(os.path.join(SRC, "screen.cfg")) as f:
        txt = f.read()
    nums = [float(t) for t in txt.split()[:3]]
    cfg = os.path.join(OUT, "screen.cfg")
    with open(cfg, "w") as f:
        f.write(" ".join("%.1f" % (n * SCALE) for n in nums) + "\n")

    src_kb = os.path.getsize(src) // 1024
    out_kb = (os.path.getsize(jpg) + os.path.getsize(msk)) // 1024
    print("%dx%d  %d KB -> %d KB (jpeg %d KB + alpha %d KB)"
          % (w, h, src_kb, out_kb,
             os.path.getsize(jpg) // 1024, os.path.getsize(msk) // 1024))
    print("screen.cfg: %s -> %s" % (" ".join("%g" % n for n in nums),
                                    open(cfg).read().strip()))


if __name__ == "__main__":
    main()
