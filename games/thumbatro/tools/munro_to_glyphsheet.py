#!/usr/bin/env python3
"""Convert the original Thumby `munro-narrow_10.bmp` FontResource into a Mote
glyph-sheet (`munro_glyphs.png` + `.gsheet`) that `mote bake` -> glyphs2font
turns into `src/munro.font.h` (symbol `munro`, used via mote->text_font).

Thumby FontResource format: glyphs are `H-1` px tall packed left-to-right; the
BOTTOM row encodes each glyph's width as a run of one of two alternating marker
colours (here red 0xF800 / blue 0x001F). Glyph ink is white (0xFFFF).

Output sheet: a uniform CELL x CELL grid (glyphs2font needs square cells), each
glyph top-left in its cell on a magenta (transparent) field. The .gsheet carries
the exact per-glyph advances so spacing matches Munro precisely (not ink-derived).
"""
import struct, sys, os
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.expanduser("~/thumby-color/TinyCircuits-Thumby-Color-Games/ThumbAtro/munro-narrow_10.bmp")
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else \
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets")

d = open(SRC, "rb").read()
off = struct.unpack_from("<I", d, 10)[0]
w   = struct.unpack_from("<i", d, 18)[0]
h   = struct.unpack_from("<i", d, 22)[0]
H = abs(h); rowb = ((w * 2 + 3) // 4) * 4
def px(x, y):
    sy = (H - 1 - y) if h > 0 else y
    return struct.unpack_from("<H", d, off + sy * rowb + x * 2)[0]

gh = H - 1                      # glyph height (last row is the width-marker strip)
marker_y = H - 1
# walk the marker row: each run of one colour = one glyph's advance width
widths, xoffs, x = [], [], 0
cur = px(0, marker_y); run = 0; start = 0
while x < w:
    c = px(x, marker_y)
    if c != cur:
        widths.append(run); xoffs.append(start)
        cur = c; run = 0; start = x
    run += 1; x += 1
widths.append(run); xoffs.append(start)
COUNT = 95                      # codepoints 32..126
assert len(widths) >= COUNT, "found %d glyph runs, need %d" % (len(widths), COUNT)
widths, xoffs = widths[:COUNT], xoffs[:COUNT]

CELL = gh + 1                   # square cell big enough for the tallest/widest glyph
assert max(widths) <= CELL, "glyph wider (%d) than cell (%d)" % (max(widths), CELL)
COLS = 16
ROWS = (COUNT + COLS - 1) // COLS
sheet = Image.new("RGBA", (COLS * CELL, ROWS * CELL), (255, 0, 255, 255))  # magenta key
sp = sheet.load()
WHITE = 0xFFFF
for i in range(COUNT):
    cx = (i % COLS) * CELL
    cy = (i // COLS) * CELL
    for gx in range(widths[i]):
        for gy in range(gh):
            if px(xoffs[i] + gx, gy) == WHITE:
                sp[cx + gx, cy + gy] = (255, 255, 255, 255)

os.makedirs(OUTDIR, exist_ok=True)
sheet.save(os.path.join(OUTDIR, "munro_glyphs.png"))
# .gsheet: bake reads [cols cell line_h first count]; glyphs2font reads 8 ints
# (8th = pen origin) then COUNT advances. line_h matches the original 13px box.
LINE_H = H
hdr = [COLS, CELL, LINE_H, 32, COUNT, 0, 0, 0]   # parts[5],[6] unused; parts[7]=origin=0
open(os.path.join(OUTDIR, "munro_glyphs.gsheet"), "w").write(
    " ".join(map(str, hdr)) + "\n" + " ".join(map(str, widths)) + "\n")
print("wrote munro_glyphs.png (%dx%d) + .gsheet  cell=%d line_h=%d glyphs=%d"
      % (sheet.width, sheet.height, CELL, LINE_H, COUNT))
print("advances:", widths)
