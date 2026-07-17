#!/usr/bin/env python3
"""DraftMote sprite generator — writes the editable PNGs under assets/.

  hero.png   6x1 grid of 16x16: down0 down1 up0 up1 left0 left1  (right = HFLIP)
  items.png  9x1 grid of 12x12: coin key gem food star bigstar masterkey padlock boot

Room interiors + door overlays come from the art pipeline instead:
extract_rooms.py -> gen_rooms.py (see art_sources/ref/).

Bake: `mote bake games/draftmote` (Studio Save does the same).
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3)

class Sheet:
    def __init__(self, cols, rows, cell):
        self.cell = cell
        self.im = Image.new("RGBA", (cols * cell, rows * cell), (0, 0, 0, 0))
        self.ox = self.oy = 0
    def at(self, col, row):
        self.ox, self.oy = col * self.cell, row * self.cell
        return self
    def px(self, x, y, c):
        if c is None: return
        x += self.ox; y += self.oy
        if 0 <= x < self.im.width and 0 <= y < self.im.height:
            s = snap(c); self.im.putpixel((x, y), (s[0], s[1], s[2], 255))
    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)
    def hline(self, x0, x1, y, c):
        self.rect(x0, y, x1, y, c)
    def vline(self, x, y0, y1, c):
        self.rect(x, y0, x, y1, c)
    def save(self, name):
        p = os.path.join(HERE, name)
        self.im.save(p)
        print("wrote", p)

# ------------------------------------------------------------------- hero ----
H = Sheet(6, 1, 16)
SKIN = (232, 190, 150); HAIR = (60, 42, 30); COAT = (46, 84, 170); COATD = (32, 60, 130)
LEG = (40, 40, 52); SHOE = (30, 26, 30); EYE = (24, 24, 32)

def hero(cell, facing, step):
    """facing: 0 down, 1 up, 2 left. step: 0/1 walk frame."""
    H.at(cell, 0)
    if step == 0:
        H.rect(6, 12, 7, 14, LEG); H.rect(8, 12, 9, 14, LEG)
        H.rect(6, 15, 7, 15, SHOE); H.rect(8, 15, 9, 15, SHOE)
    else:
        H.rect(5, 12, 6, 14, LEG); H.rect(9, 12, 10, 14, LEG)
        H.rect(5, 15, 6, 15, SHOE); H.rect(9, 15, 10, 15, SHOE)
    H.rect(4, 7, 11, 12, COAT)
    H.hline(4, 11, 12, COATD)
    H.vline(4, 7, 12, COATD); H.vline(11, 7, 12, COATD)
    if facing == 0:
        H.vline(7, 8, 11, COATD)
        H.px(7, 8, (216, 190, 90))
    H.rect(3, 8, 3, 11, COATD); H.rect(12, 8, 12, 11, COATD)
    H.rect(4, 1, 11, 6, SKIN)
    if facing == 1:
        H.rect(4, 1, 11, 6, HAIR)
        H.hline(4, 11, 6, (44, 30, 22))
    else:
        H.rect(4, 1, 11, 2, HAIR)
        H.px(4, 3, HAIR); H.px(11, 3, HAIR)
        if facing == 0:
            H.px(6, 4, EYE); H.px(9, 4, EYE)
        else:
            H.px(5, 4, EYE)
            H.rect(9, 1, 11, 6, HAIR)

hero(0, 0, 0); hero(1, 0, 1)
hero(2, 1, 0); hero(3, 1, 1)
hero(4, 2, 0); hero(5, 2, 1)
H.save("hero.png")

# ------------------------------------------------------------------ items ----
I = Sheet(9, 1, 12)

# coin
I.at(0, 0)
I.rect(3, 2, 8, 9, (230, 190, 60))
I.px(3, 2, None); I.px(8, 2, None); I.px(3, 9, None); I.px(8, 9, None)
I.vline(3, 3, 8, (250, 220, 110)); I.hline(4, 7, 2, (250, 220, 110))
I.vline(8, 3, 8, (170, 130, 40)); I.hline(4, 7, 9, (170, 130, 40))
I.rect(5, 4, 6, 7, (200, 160, 50))

# key
I.at(1, 0)
I.rect(3, 2, 6, 5, (220, 180, 70))
I.px(4, 3, (120, 90, 30)); I.px(5, 4, (120, 90, 30))
I.vline(5, 6, 10, (220, 180, 70))
I.px(6, 8, (220, 180, 70)); I.px(6, 10, (220, 180, 70)); I.px(7, 10, (220, 180, 70))
I.px(3, 2, (250, 225, 120))

# gem
I.at(2, 0)
I.hline(4, 7, 2, (110, 220, 200))
I.rect(3, 3, 8, 5, (80, 190, 170))
for i in range(3):
    I.hline(4 + i, 7 - i, 6 + i, (60, 160, 140))
I.px(5, 3, (200, 250, 240)); I.px(4, 4, (150, 235, 215))

# food (sandwich)
I.at(3, 0)
I.rect(2, 4, 9, 5, (218, 168, 92))
I.rect(2, 6, 9, 6, (110, 180, 70))
I.rect(2, 7, 9, 7, (230, 120, 100))
I.rect(2, 8, 9, 9, (196, 146, 76))
I.hline(3, 8, 3, (236, 192, 120))

# star
I.at(4, 0)
S = (255, 220, 80); Sd = (200, 160, 40)
I.px(5, 1, S); I.px(6, 1, S)
I.rect(4, 3, 7, 4, S); I.hline(1, 10, 4, S)
I.rect(3, 5, 8, 6, S)
I.rect(2, 7, 4, 9, S); I.rect(7, 7, 9, 9, S)
I.px(5, 7, S); I.px(6, 7, S)
I.px(5, 2, (255, 245, 170))
I.px(9, 9, Sd); I.px(2, 9, Sd)

# big star
I.at(5, 0)
I.px(5, 0, S); I.px(6, 0, S)
I.rect(4, 2, 7, 3, S); I.hline(0, 11, 3, S)
I.rect(3, 4, 8, 5, S)
I.rect(1, 6, 4, 8, S); I.rect(7, 6, 10, 8, S)
I.rect(5, 6, 6, 6, S)
I.px(5, 1, (255, 250, 200)); I.px(0, 10, (255, 245, 170)); I.px(11, 0, (255, 245, 170))

# master key
I.at(6, 0)
I.rect(2, 1, 7, 6, (250, 215, 90))
I.rect(3, 2, 6, 5, (140, 105, 30))
I.rect(4, 3, 5, 4, (250, 215, 90))
I.vline(4, 7, 10, (250, 215, 90)); I.vline(5, 7, 10, (250, 215, 90))
I.px(6, 8, (250, 215, 90)); I.rect(6, 10, 8, 10, (250, 215, 90))
I.px(2, 1, (255, 245, 180))

# padlock
I.at(7, 0)
I.rect(3, 5, 8, 10, (200, 170, 60))
I.hline(3, 8, 5, (230, 205, 100))
I.px(5, 7, (110, 85, 25)); I.px(6, 7, (110, 85, 25)); I.px(5, 8, (110, 85, 25)); I.px(6, 8, (110, 85, 25))
I.vline(3, 2, 4, (160, 160, 170)); I.vline(8, 2, 4, (160, 160, 170))
I.hline(4, 7, 1, (160, 160, 170))

# boot (steps HUD icon)
I.at(8, 0)
I.rect(4, 1, 7, 6, (150, 100, 55))
I.rect(4, 7, 9, 9, (150, 100, 55))
I.hline(4, 9, 9, (100, 64, 34))
I.hline(4, 7, 1, (180, 128, 74))
I.px(9, 7, (180, 128, 74))
I.rect(4, 10, 9, 10, (60, 50, 46))

I.save("items.png")
