#!/usr/bin/env python3
"""Generate the Android launcher icon, as a real adaptive icon.

A legacy square bitmap gets pasted onto a white plate by the system launcher,
which is why the Mote icon looked like a sticker on a white circle. An adaptive
icon is two full-bleed layers instead — background and foreground — that the
launcher masks to whatever shape it uses and moves against each other when you
touch it. Android 13+ can also take a monochrome layer and tint it with the
system palette, which is the "themed icon" look.

The art is the same design as studio/assets/mote.svg — a speck of light caught in
a sunbeam — split into those layers:

    background   the dark gradient, the diagonal beam and its dust, edge to edge
    foreground   the mote itself, sized to the 72dp safe zone
    monochrome   beam + mote as one flat silhouette for themed icons

Adaptive drawables are 108dp square; only the middle 72dp is guaranteed visible,
so nothing that matters may sit outside it.

    python3 android/tools/gen_icon.py
"""
import io
import os
import sys

try:
    import cairosvg
except ImportError:
    sys.exit("gen_icon: needs cairosvg (pip install cairosvg)")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RES = os.path.join(ROOT, "android", "app", "src", "main", "res")

# dp -> px per density bucket
ADAPTIVE = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
LEGACY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}

# The canvas is 108 units; the safe zone is the middle 72 (centre 54,54 r 36).
C = 54.0
SAFE_R = 36.0

DEFS = """
  <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#1b1f30"/>
    <stop offset="1" stop-color="#0b0d16"/>
  </linearGradient>
  <linearGradient id="beam" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0.00" stop-color="#ffe6a0" stop-opacity="0"/>
    <stop offset="0.50" stop-color="#ffd779" stop-opacity="0.50"/>
    <stop offset="1.00" stop-color="#ffe6a0" stop-opacity="0"/>
  </linearGradient>
  <radialGradient id="halo" cx="0.5" cy="0.5" r="0.5">
    <stop offset="0" stop-color="#fff0c0" stop-opacity="0.85"/>
    <stop offset="1" stop-color="#ffd779" stop-opacity="0"/>
  </radialGradient>
  <radialGradient id="core" cx="0.5" cy="0.5" r="0.5">
    <stop offset="0.0" stop-color="#ffffff"/>
    <stop offset="0.35" stop-color="#fff6da"/>
    <stop offset="1.0" stop-color="#ffdd86" stop-opacity="0"/>
  </radialGradient>
"""


def svg(body, defs=DEFS):
    return ('<svg width="108" height="108" viewBox="0 0 108 108" '
            'xmlns="http://www.w3.org/2000/svg"><defs>%s</defs>%s</svg>' % (defs, body))


def background():
    """Edge to edge: no rounded corners of our own — the launcher's mask owns the
    shape, and a rounded rect here would show its corners inside a circle mask.
    The beam is oversized so it still crosses the frame after any mask."""
    return svg("""
  <rect width="108" height="108" fill="url(#bg)"/>
  <g transform="rotate(24 54 54)">
    <rect x="39"   y="-40" width="30" height="188" fill="url(#beam)"/>
    <rect x="49.5" y="-40" width="9"  height="188" fill="url(#beam)" opacity="0.5"/>
  </g>
  <circle cx="30" cy="24" r="1.1" fill="#ffe6a0" opacity="0.55"/>
  <circle cx="76" cy="38" r="0.8" fill="#ffe6a0" opacity="0.40"/>
  <circle cx="26" cy="76" r="0.9" fill="#ffe6a0" opacity="0.45"/>
  <circle cx="78" cy="86" r="1.2" fill="#ffe6a0" opacity="0.45"/>
""")


def foreground():
    """Just the mote. The halo is a fade to transparent, so it may reach past the
    safe zone; the core and the white centre stay well inside it."""
    return svg("""
  <circle cx="%(c)s" cy="%(c)s" r="%(halo)s" fill="url(#halo)"/>
  <circle cx="%(c)s" cy="%(c)s" r="%(core)s" fill="url(#core)"/>
  <circle cx="%(c)s" cy="%(c)s" r="%(dot)s"  fill="#ffffff"/>
""" % {"c": C, "halo": SAFE_R * 0.86, "core": SAFE_R * 0.30, "dot": SAFE_R * 0.095})


def monochrome():
    """One flat silhouette for Android 13+ themed icons, which tint whatever alpha
    they are given. A soft glow disappears at that size, so this is the beam as a
    hard bar with the mote as a solid disc sitting in it."""
    return svg("""
  <g fill="#ffffff">
    <g transform="rotate(24 54 54)">
      <rect x="50.5" y="9" width="7" height="90" rx="3.5"/>
    </g>
    <circle cx="54" cy="54" r="14"/>
  </g>
""", defs="")


def legacy():
    """Pre-API-26 launchers get one bitmap and draw it as-is, so this one keeps
    the rounded-square plate the design was made with."""
    return ('<svg width="108" height="108" viewBox="0 0 108 108" '
            'xmlns="http://www.w3.org/2000/svg"><defs>%s</defs>'
            '<rect width="108" height="108" rx="24" fill="url(#bg)"/>'
            '<g clip-path="inset(0 round 24)">'
            '<g transform="rotate(24 54 54)">'
            '<rect x="34" y="-20" width="30" height="148" fill="url(#beam)"/>'
            '<rect x="45" y="-20" width="9" height="148" fill="url(#beam)" opacity="0.5"/>'
            '</g></g>'
            '<circle cx="55" cy="56" r="33" fill="url(#halo)"/>'
            '<circle cx="55" cy="56" r="13" fill="url(#core)"/>'
            '<circle cx="55" cy="56" r="3.8" fill="#ffffff"/>'
            '</svg>' % DEFS)


def render(src, path, px):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cairosvg.svg2png(bytestring=src.encode(), write_to=path,
                     output_width=px, output_height=px)


def main():
    layers = [("ic_launcher_background", background(), ADAPTIVE),
              ("ic_launcher_foreground", foreground(), ADAPTIVE),
              ("ic_launcher_monochrome", monochrome(), ADAPTIVE),
              ("ic_launcher", legacy(), LEGACY),
              ("ic_launcher_round", legacy(), LEGACY)]
    for name, src, sizes in layers:
        for bucket, px in sizes.items():
            render(src, os.path.join(RES, "mipmap-" + bucket, name + ".png"), px)
        print("  %-24s %s" % (name, " ".join("%s:%d" % (b, p) for b, p in sizes.items())))
    print("icons -> %s" % os.path.relpath(RES, ROOT))


if __name__ == "__main__":
    main()
