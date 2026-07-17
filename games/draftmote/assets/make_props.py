#!/usr/bin/env python3
"""DraftMote authored furniture — original pixel art in the style of the
reference sheets (dark outlines, warm saturated ramps, chunky highlights).

Writes assets/props_auth.png (one packed row, 32px tall) and emits
src/props_meta.h covering BOTH prop sheets:
  sheet 0 = props_sheet.png (extracted: bush chest campfire shelves)
  sheet 1 = props_auth.png  (authored: beds, tables, sofa, kitchen, bath...)

Run after extract_sheet2.py:  python3 assets/make_props.py
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

OUT_H = 32
OUTLINE = (34, 24, 28)

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3, 255)

class P:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    def px(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h and c is not None:
            self.im.putpixel((x, y), snap(c))
    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)
    def hl(self, x0, x1, y, c): self.rect(x0, y, x1, y, c)
    def vl(self, x, y0, y1, c): self.rect(x, y0, x, y1, c)
    def outline(self):
        src = self.im.copy()
        for y in range(self.h):
            for x in range(self.w):
                if src.getpixel((x, y))[3]:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < self.w and 0 <= yy < self.h and src.getpixel((xx, yy))[3]:
                        self.im.putpixel((x, y), snap(OUTLINE))
                        break

WOOD  = (150, 103, 58); WOOD_D = (114, 74, 42); WOOD_DD = (84, 52, 30); WOOD_L = (178, 130, 78)
WHITE = (236, 236, 240); WHITE_D = (196, 198, 208)
props = []

def add(name, p, sol=None):
    p.outline()
    props.append((name, p, sol or (0, 0, p.w, p.h)))

# ---- beds (24x30): headboard, pillow, blanket ----
def bed(blanket, blanket_d):
    p = P(24, 30)
    p.rect(1, 1, 22, 6, WOOD_D); p.hl(2, 21, 1, WOOD_L)          # headboard
    p.rect(2, 5, 21, 10, WHITE); p.rect(2, 9, 21, 10, WHITE_D)   # pillow band
    p.rect(2, 11, 21, 27, blanket)
    p.hl(2, 21, 13, (255, 255, 255)); p.hl(2, 21, 14, blanket_d)  # fold
    for y in range(16, 27, 4):
        p.hl(3, 20, y, blanket_d)
    p.rect(1, 27, 22, 28, WOOD_DD)                               # footboard
    return p

add("bed_blue", bed((66, 106, 176), (48, 80, 140)))
add("bed_red",  bed((178, 70, 64), (140, 48, 46)))

# ---- table (28x20) ----
p = P(28, 20)
p.rect(1, 3, 26, 14, WOOD)
p.hl(2, 25, 3, WOOD_L)
for y in (6, 9, 12):
    p.hl(3, 24, y, WOOD_D)
p.rect(2, 15, 4, 18, WOOD_DD); p.rect(23, 15, 25, 18, WOOD_DD)
add("table", p)

# ---- chair (12x14) ----
p = P(12, 14)
p.rect(2, 1, 9, 3, WOOD_D)
p.rect(2, 4, 9, 9, WOOD); p.hl(2, 9, 4, WOOD_L)
p.rect(2, 10, 3, 12, WOOD_DD); p.rect(8, 10, 9, 12, WOOD_DD)
add("chair", p)

# ---- sofa (34x20, red, front-facing) ----
p = P(34, 20)
p.rect(1, 2, 32, 8, (150, 52, 48))                                # back
p.hl(2, 31, 2, (196, 86, 76))
p.rect(1, 8, 5, 16, (150, 52, 48)); p.rect(28, 8, 32, 16, (150, 52, 48))   # arms
p.hl(1, 5, 8, (196, 86, 76)); p.hl(28, 32, 8, (196, 86, 76))
p.rect(6, 9, 27, 15, (178, 70, 64))                               # cushions
p.vl(16, 9, 15, (140, 48, 46)); p.hl(6, 27, 9, (208, 96, 84))
p.rect(2, 17, 31, 17, WOOD_DD)
add("sofa", p)

# ---- counter (32x14) ----
p = P(32, 14)
p.rect(1, 1, 30, 5, WOOD_L); p.hl(1, 30, 1, (208, 164, 106))
p.rect(1, 6, 30, 12, WOOD_D)
for x in (8, 16, 24):
    p.vl(x, 7, 11, WOOD_DD)
add("counter", p)

# ---- stove (20x22) ----
p = P(20, 22)
p.rect(1, 1, 18, 20, (78, 80, 90))
p.hl(2, 17, 1, (118, 120, 132))
for (cx, cy) in ((6, 4), (13, 4)):
    p.rect(cx - 2, cy - 1, cx + 2, cy + 1, (44, 44, 52))
    p.px(cx, cy, (60, 60, 70))
p.rect(4, 9, 15, 17, (48, 48, 56))
p.rect(6, 11, 13, 15, (232, 130, 40)); p.rect(8, 12, 11, 14, (255, 190, 80))
add("stove", p)

# ---- tub (36x22) ----
p = P(36, 22)
p.rect(1, 2, 34, 18, WHITE)
p.hl(2, 33, 2, (255, 255, 255))
p.rect(4, 5, 31, 15, (140, 196, 224))
p.rect(6, 6, 29, 9, (170, 216, 238))
p.rect(1, 19, 34, 20, WHITE_D)
p.px(3, 4, (160, 160, 172)); p.px(32, 4, (160, 160, 172))
add("tub", p)

# ---- toilet (14x18) ----
p = P(14, 18)
p.rect(3, 1, 10, 6, WHITE); p.hl(3, 10, 1, (255, 255, 255))       # tank
p.rect(2, 7, 11, 14, WHITE)
p.rect(4, 9, 9, 12, (170, 200, 220))
p.rect(3, 15, 10, 16, WHITE_D)
add("toilet", p)

# ---- desk (30x20, green inlay) ----
p = P(30, 20)
p.rect(1, 2, 28, 13, WOOD)
p.hl(2, 27, 2, WOOD_L)
p.rect(4, 4, 25, 11, (58, 108, 76))
p.rect(6, 5, 12, 9, (232, 228, 210))                               # papers
p.px(20, 6, (216, 180, 80)); p.px(21, 6, (216, 180, 80))           # inkwell
p.rect(2, 14, 5, 18, WOOD_DD); p.rect(24, 14, 27, 18, WOOD_DD)
add("desk", p)

# ---- map table (26x18, blueprint top) — the Drafting Room ----
p = P(26, 18)
p.rect(1, 2, 24, 12, WOOD_D)
p.rect(3, 3, 22, 11, (40, 72, 140))
for x in range(5, 22, 4):
    p.vl(x, 4, 10, (86, 126, 196))
for y in range(5, 11, 3):
    p.hl(4, 21, y, (86, 126, 196))
p.rect(9, 6, 12, 8, (226, 214, 170))                               # the estate plan
p.rect(2, 13, 4, 16, WOOD_DD); p.rect(21, 13, 23, 16, WOOD_DD)
add("map_table", p)

# ---- washer (18x22) ----
p = P(18, 22)
p.rect(1, 1, 16, 20, WHITE)
p.hl(2, 15, 1, (255, 255, 255))
p.rect(4, 6, 13, 15, (90, 96, 110))
p.rect(6, 8, 11, 13, (140, 180, 210))
p.px(3, 3, (90, 200, 120)); p.px(6, 3, (216, 180, 80))
p.rect(1, 19, 16, 20, WHITE_D)
add("washer", p)

# ---- barrel (16x20) ----
p = P(16, 20)
p.rect(2, 1, 13, 18, (134, 90, 48))
p.vl(2, 2, 17, (100, 64, 34)); p.vl(13, 2, 17, (100, 64, 34))
p.vl(5, 1, 18, (150, 104, 58)); p.vl(9, 1, 18, (150, 104, 58))
p.hl(2, 13, 4, (70, 66, 74)); p.hl(2, 13, 14, (70, 66, 74))
p.hl(3, 12, 1, (160, 116, 66))
add("barrel", p)

# ---- gold pile (20x12) ----
p = P(20, 12)
for (x, y, r) in ((6, 8, 4), (13, 8, 4), (10, 5, 4), (4, 9, 2), (16, 9, 2)):
    p.rect(x - r, y - r // 2, x + r, y + r // 2 + 1, (214, 176, 60))
p.px(6, 5, (255, 236, 140)); p.px(13, 5, (255, 236, 140)); p.px(10, 3, (255, 236, 140))
p.hl(2, 17, 11, (140, 108, 32))
add("gold_pile", p)

# ---- workbench (30x16) — the locksmith ----
p = P(30, 16)
p.rect(1, 4, 28, 12, WOOD_D)
p.hl(2, 27, 4, WOOD_L)
p.rect(5, 1, 11, 5, (110, 112, 124))                               # vice/anvil block
p.hl(5, 11, 1, (150, 152, 166))
p.px(17, 3, (216, 180, 80)); p.px(20, 2, (196, 198, 208)); p.px(23, 3, (196, 198, 208))
p.rect(2, 13, 4, 15, WOOD_DD); p.rect(25, 13, 27, 15, WOOD_DD)
add("workbench", p)

# ---- potted plant (12x16) ----
p = P(12, 16)
p.rect(3, 10, 8, 14, (166, 106, 62)); p.hl(3, 8, 10, (196, 130, 80))
for (x, y) in ((5, 2), (3, 4), (8, 3), (2, 6), (9, 6), (5, 5), (7, 7), (4, 8), (6, 3)):
    p.px(x, y, (74, 138, 58)); p.px(x + 1, y, (96, 164, 74))
add("plant", p)

# ------------------------------------------------------------------- pack ----
total = sum(p.w for (_, p, _) in props)
sheet = Image.new("RGBA", (total, OUT_H), (0, 0, 0, 0))
x = 0
placed = []
for (name, p, sol) in props:
    sheet.paste(p.im, (x, OUT_H - p.h))
    placed.append((name, x, OUT_H - p.h, p.w, p.h, sol))
    x += p.w
sheet.save(os.path.join(HERE, "props_auth.png"))
print("wrote props_auth.png (%dx%d)" % (total, OUT_H))

# extracted sheet cells (from extract_sheet2.py output, sheet 0)
EXTRACTED = [
    ("bush",        0,   8, 24, 24),
    ("chest",       24, 16, 20, 16),
    ("campfire",    44, 12, 20, 20),
    ("shelf_big",   64,  6, 32, 26),
    ("shelf_small", 96,  6, 30, 26),
]

lines = ["/* GENERATED by assets/make_props.py — prop sprite metadata for both",
         " * sheets (0 = props_sheet.png extracted, 1 = props_auth.png authored). */",
         "#ifndef DRAFT_PROPS_META_H", "#define DRAFT_PROPS_META_H", "#include <stdint.h>", "",
         "typedef struct { uint8_t sheet; uint16_t fx; uint8_t fy, fw, fh; } PropDef;", "",
         "enum {"]
names = [n for (n, *_ ) in EXTRACTED] + [n for (n, *_ ) in placed]
lines.append("    " + ", ".join("P_" + n.upper() for n in names) + ", P_COUNT")
lines.append("};")
lines.append("")
lines.append("static const PropDef k_props[P_COUNT] = {")
for (n, fx, fy, fw, fh) in EXTRACTED:
    lines.append("    { 0, %3d, %2d, %2d, %2d },  /* %s */" % (fx, fy, fw, fh, n))
for (n, fx, fy, fw, fh, sol) in placed:
    lines.append("    { 1, %3d, %2d, %2d, %2d },  /* %s */" % (fx, fy, fw, fh, n))
lines.append("};")
lines.append("")
lines.append("#endif")
with open(os.path.join(GAME, "src", "props_meta.h"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("wrote src/props_meta.h (%d props)" % len(names))
