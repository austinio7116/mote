#!/usr/bin/env python3
"""ThumbPrince authored furniture — original pixel art in the style of the
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

# ---- tub (28x20) ----
p = P(28, 20)
p.rect(1, 2, 26, 16, WHITE)
p.hl(2, 25, 2, (255, 255, 255))
p.rect(4, 5, 23, 13, (140, 196, 224))
p.rect(6, 6, 21, 8, (170, 216, 238))
p.rect(1, 17, 26, 18, WHITE_D)
p.px(3, 4, (160, 160, 172)); p.px(24, 4, (160, 160, 172))
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

# ---- open treasure chest (20x16), matches the sheet chest's palette ----
p = P(20, 16)
p.rect(2, 1, 17, 5, (120, 78, 40))                                # lid, thrown back
p.hl(3, 16, 1, (160, 108, 58))
p.rect(2, 6, 17, 14, (146, 96, 50))                               # body
p.rect(4, 7, 15, 11, (52, 36, 24))                                # dark interior
p.rect(5, 9, 14, 11, (216, 176, 62))                              # gold inside
p.px(7, 8, (255, 232, 130)); p.px(12, 8, (255, 232, 130))
p.rect(2, 13, 17, 14, (104, 66, 34))
p.vl(2, 6, 14, (104, 66, 34)); p.vl(17, 6, 14, (104, 66, 34))
p.rect(8, 5, 11, 6, (196, 198, 208))                              # clasp
add("chest_open", p)

# ---- security terminal (16x18) ----
p = P(16, 18)
p.rect(2, 1, 13, 10, (58, 60, 72))                                 # monitor shell
p.hl(3, 12, 1, (88, 90, 104))
p.rect(4, 3, 11, 8, (34, 80, 96))                                  # screen
p.hl(5, 4, 10, (110, 220, 240)) if False else None
for (x, y) in ((5, 4), (6, 4), (8, 5), (5, 6), (7, 6), (9, 7)):
    p.px(x, y, (110, 220, 240))                                    # glyphs
p.rect(6, 11, 9, 12, (48, 50, 60))                                 # stand
p.rect(2, 13, 13, 16, (78, 80, 94))                                # console
p.hl(2, 13, 13, (108, 110, 126))
for x in (4, 7, 10):
    p.px(x, 15, (200, 200, 90))
add("terminal", p)

# ---- power breaker panel (14x18) ----
p = P(14, 18)
p.rect(1, 1, 12, 16, (96, 100, 110))
p.hl(2, 11, 1, (130, 134, 146))
p.rect(3, 3, 10, 8, (60, 62, 72))                                  # lever slot
p.rect(6, 4, 7, 10, (200, 60, 50))                                 # the lever
p.hl(6, 7, 4, (240, 110, 90))
p.px(3, 13, (90, 220, 120)); p.px(6, 13, (220, 70, 60))            # lamps
for x in range(2, 12, 2):
    p.px(x, 15, (210, 190, 70))                                    # hazard stripe
add("breaker", p)

# ---- woven rug (28x20, walkable) ----
p = P(28, 20)
p.rect(1, 1, 26, 18, (150, 62, 58))
p.rect(3, 3, 24, 16, (178, 84, 70))
p.rect(6, 5, 21, 14, (150, 62, 58))
p.rect(11, 8, 16, 11, (216, 176, 100))
for x in range(2, 26, 4):
    p.px(x, 1, (196, 110, 90)); p.px(x + 1, 18, (196, 110, 90))
add("rug", p)

# ---- painting (16x8: exactly two wall tiles, IN the band; _v = side walls) ----
p = P(16, 8)
p.rect(0, 0, 15, 7, (110, 74, 42))
p.hl(1, 14, 0, (150, 104, 58))
p.rect(2, 2, 13, 5, (86, 120, 170))            # sky
p.rect(2, 4, 13, 5, (70, 130, 80))             # hills
p.px(4, 2, (240, 235, 200)); p.px(11, 3, (226, 200, 120))
add("painting", p)
pv = P(8, 16); pv.im = p.im.transpose(Image.ROTATE_270)
add("painting_v", pv)

# ---- window (16x8: two wall tiles; _v = side walls) ----
p = P(16, 8)
p.rect(0, 0, 15, 7, (86, 66, 44))
p.rect(2, 2, 6, 5, (120, 170, 210))
p.rect(9, 2, 13, 5, (120, 170, 210))
p.px(3, 2, (200, 230, 250)); p.px(10, 2, (200, 230, 250))
p.hl(2, 6, 5, (90, 130, 170)); p.hl(9, 13, 5, (90, 130, 170))
add("window", p)
pv = P(8, 16); pv.im = p.im.transpose(Image.ROTATE_270)
add("window_v", pv)

# ---- floor lamp (10x22) ----
p = P(10, 22)
p.rect(2, 0, 7, 5, (236, 214, 150))
p.hl(2, 7, 5, (190, 160, 100))
p.vl(4, 6, 18, (70, 62, 60)); p.vl(5, 6, 18, (94, 86, 82))
p.rect(2, 19, 7, 20, (70, 62, 60))
add("lamp", p)

# ---- crate (14x14) ----
p = P(14, 14)
p.rect(1, 1, 12, 12, (158, 116, 66))
for v in (1, 12):
    p.hl(1, 12, v, (114, 78, 44)); p.vl(v, 1, 12, (114, 78, 44))
for i in range(10):
    p.px(2 + i, 2 + i, (128, 92, 52))
p.hl(2, 11, 6, (138, 100, 56))
add("crate", p)

# ---- bench (24x12) ----
p = P(24, 12)
p.rect(1, 2, 22, 7, WOOD)
p.hl(1, 22, 2, WOOD_L)
p.hl(1, 22, 7, WOOD_D)
p.rect(2, 8, 3, 10, WOOD_DD); p.rect(20, 8, 21, 10, WOOD_DD)
add("bench", p)

# ---- candle stand (8x16) ----
p = P(8, 16)
p.px(3, 0, (255, 220, 110)); p.px(4, 1, (240, 150, 60))
p.rect(3, 2, 4, 5, (226, 222, 210))
p.vl(3, 6, 12, (120, 110, 100)); p.vl(4, 6, 12, (150, 140, 130))
p.rect(1, 13, 6, 14, (120, 110, 100))
add("candle", p)

# ---- potted plant (12x16) ----
p = P(12, 16)
p.rect(3, 10, 8, 14, (166, 106, 62)); p.hl(3, 8, 10, (196, 130, 80))
for (x, y) in ((5, 2), (3, 4), (8, 3), (2, 6), (9, 6), (5, 5), (7, 7), (4, 8), (6, 3)):
    p.px(x, y, (74, 138, 58)); p.px(x + 1, y, (96, 164, 74))
add("plant", p)

# ---- wall safe (16x16) — the Study's combination puzzle ----
p = P(16, 16)
p.rect(1, 1, 14, 14, (104, 108, 120))
p.hl(2, 13, 1, (140, 144, 158))
p.rect(3, 3, 12, 12, (78, 82, 94))
p.rect(6, 5, 9, 8, (58, 60, 72))                                   # dial plate
p.px(7, 6, (210, 210, 220)); p.px(8, 7, (210, 210, 220))           # dial marks
p.rect(11, 10, 12, 11, (216, 180, 80))                             # handle
add("safe", p)

# ---- keypad console (14x16) — the Lab's code lock ----
p = P(14, 16)
p.rect(1, 1, 12, 14, (58, 60, 72))
p.hl(2, 11, 1, (88, 90, 104))
p.rect(3, 3, 10, 5, (200, 150, 40))                                # amber readout
p.px(4, 4, (255, 220, 120)); p.px(6, 4, (255, 220, 120))
for yy in (7, 9, 11):
    for xx in (4, 7, 10):
        p.px(xx, yy, (170, 174, 190)); p.px(xx + 1, yy, (130, 134, 150))
add("keypad", p)

# ---- grandfather clock (14x28) — the Foyer ----
p = P(14, 28)
p.rect(2, 0, 11, 26, WOOD_D)
p.hl(2, 11, 0, WOOD_L); p.vl(2, 0, 26, WOOD_DD)
p.rect(4, 2, 9, 8, (232, 228, 210))                                # face
p.px(6, 4, (60, 50, 44)); p.px(7, 4, (60, 50, 44))                 # hands
p.px(6, 5, (60, 50, 44))
p.rect(4, 11, 9, 21, (52, 38, 26))                                 # case window
p.vl(6, 12, 17, (216, 180, 80))                                    # pendulum rod
p.rect(5, 17, 7, 19, (240, 205, 90))                               # bob
p.rect(1, 26, 12, 27, WOOD_DD)
add("clock", p)

# ---- globe (14x18) — the Drawing Room ----
p = P(14, 18)
p.rect(4, 2, 9, 9, (66, 106, 176))                                 # ocean
p.px(4, 2, (0,0,0,0)) if False else None
p.rect(5, 3, 7, 5, (96, 164, 74)); p.rect(8, 6, 9, 8, (96, 164, 74))  # land
p.px(9, 3, (140, 200, 240))
p.vl(11, 3, 8, (216, 180, 80))                                     # meridian arm
p.rect(6, 10, 7, 12, (120, 110, 100))                              # stem
p.rect(3, 13, 10, 15, WOOD_D); p.hl(3, 10, 13, WOOD_L)             # base
add("globe", p)

# ---- upright piano (28x20) — the Music Room ----
p = P(28, 20)
p.rect(1, 1, 26, 16, (60, 42, 32))                                 # body
p.hl(2, 25, 1, (96, 68, 48))
p.rect(3, 3, 24, 6, (78, 54, 40))                                  # music stand band
p.rect(11, 4, 16, 5, (232, 228, 210))                              # sheet
p.rect(2, 9, 25, 13, (236, 236, 240))                              # keys
for x in range(4, 25, 3):
    p.vl(x, 9, 11, (34, 24, 28))                                   # black keys
p.hl(2, 25, 13, (196, 198, 208))
p.rect(2, 17, 4, 18, WOOD_DD); p.rect(23, 17, 25, 18, WOOD_DD)
add("piano", p)

# ---- lectern (14x16) — census ledger / gallery docent stand ----
p = P(14, 16)
p.rect(2, 2, 11, 6, (232, 228, 210))                               # open book
p.vl(6, 2, 6, (196, 192, 176)); p.vl(7, 2, 6, (196, 192, 176))
p.hl(3, 5, 3, (150, 150, 160)); p.hl(8, 10, 4, (150, 150, 160))    # script
p.rect(3, 7, 10, 8, WOOD_D)
p.rect(5, 9, 8, 12, WOOD_DD)                                       # column
p.rect(3, 13, 10, 14, WOOD_D); p.hl(3, 10, 13, WOOD_L)
add("lectern", p)

# ---- wine rack (20x18) — the Wine Store's vintage puzzle ----
p = P(20, 18)
p.rect(1, 1, 18, 16, (114, 74, 42))
p.hl(2, 17, 1, (150, 104, 58))
for yy in (5, 10, 15):
    p.hl(1, 18, yy, (84, 52, 30))
for (xx, yy) in ((3, 2), (9, 2), (15, 2), (6, 6), (12, 6), (3, 11), (9, 11), (15, 11)):
    p.rect(xx, yy + 1, xx + 2, yy + 2, (54, 96, 60))               # bottle bellies
    p.px(xx + 1, yy, (100, 150, 100))                              # necks out
add("winerack", p)

# ---- chess table (18x16) — the Games Room ----
p = P(18, 16)
p.rect(1, 2, 16, 11, WOOD)
p.hl(2, 15, 2, WOOD_L)
for yy in range(3, 10, 2):
    for xx in range(3, 15, 4):
        off = 2 if (yy // 2) % 2 else 0
        p.rect(xx + off, yy, xx + off + 1, yy, (52, 38, 26))       # dark squares
        p.rect(xx + off - 2 if xx + off - 2 > 2 else 3, yy, xx + off - 1, yy, (226, 214, 170))
    p.hl(3, 14, yy + 1, (226, 214, 170))
p.px(5, 1, (236, 236, 240)); p.px(12, 1, (60, 50, 44))             # two pieces
p.rect(2, 12, 4, 14, WOOD_DD); p.rect(13, 12, 15, 14, WOOD_DD)
add("chessboard", p)

# ---- slot machine (18x24) — the Parlor ----
p = P(18, 24)
p.rect(1, 1, 14, 22, (150, 52, 48))                                # cabinet
p.hl(2, 13, 1, (196, 86, 76))
p.rect(3, 3, 12, 6, (240, 205, 90))                                # marquee
p.px(5, 4, (255, 120, 80)); p.px(8, 4, (255, 236, 140)); p.px(11, 4, (255, 120, 80))
p.rect(3, 8, 12, 13, (34, 24, 28))                                 # reel window
for xx in (4, 7, 10):
    p.rect(xx, 9, xx + 1, 12, (236, 236, 240))                     # reels
p.rect(3, 15, 12, 18, (120, 40, 38))                               # tray
p.rect(5, 16, 10, 17, (216, 176, 62))                              # coins
p.vl(15, 4, 10, (150, 152, 166))                                   # the arm
p.rect(15, 3, 16, 4, (220, 70, 60))                                # red knob
add("slots", p)

# ---- statue (12x20) — the Rotunda's turning figures ----
p = P(12, 20)
p.rect(4, 1, 7, 4, (170, 174, 186))                                # head
p.px(4, 1, (140, 144, 158))
p.rect(3, 5, 8, 12, (150, 154, 168))                               # robe
p.vl(5, 5, 12, (130, 134, 148))
p.rect(2, 13, 9, 14, (120, 124, 138))                              # plinth cap
p.rect(3, 15, 8, 18, (104, 108, 120))
p.hl(3, 8, 15, (140, 144, 158))
add("statue", p)

# ---- pressure plate (12x10) — walkable floor slab ----
p = P(12, 10)
p.rect(1, 1, 10, 8, (120, 116, 108))
p.hl(2, 9, 1, (150, 146, 136))
p.rect(3, 3, 8, 6, (96, 92, 86))
p.px(5, 4, (150, 146, 136)); p.px(6, 5, (150, 146, 136))
add("plate", p)

# ---- balance scales (16x14) — the Pantry's weighing puzzle ----
p = P(16, 14)
p.hl(3, 12, 2, (216, 180, 80))                                     # beam
p.vl(7, 2, 8, (190, 156, 64)); p.vl(8, 2, 8, (216, 180, 80))       # column
p.px(3, 3, (150, 130, 60)); p.px(12, 3, (150, 130, 60))            # chains
p.rect(1, 4, 5, 5, (170, 174, 186)); p.hl(2, 4, 4, (210, 214, 226))    # left pan
p.rect(10, 4, 14, 5, (170, 174, 186)); p.hl(11, 13, 4, (210, 214, 226))
p.rect(4, 9, 11, 11, WOOD_D); p.hl(4, 11, 9, WOOD_L)               # base
add("scales", p)

# ---- seal lever (12x16) — thrown with A on seal days ----
p = P(12, 16)
p.rect(2, 11, 9, 14, (104, 108, 120))                              # base
p.hl(2, 9, 11, (140, 144, 158))
p.vl(5, 3, 10, (150, 152, 166)); p.vl(6, 3, 10, (110, 112, 126))   # shaft
p.rect(4, 1, 7, 3, (220, 70, 60))                                  # knob
p.px(4, 1, (255, 120, 90))
add("lever", p)

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
