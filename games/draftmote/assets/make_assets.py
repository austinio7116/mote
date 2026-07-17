#!/usr/bin/env python3
"""DraftMote art generator — writes the editable PNGs under assets/.

  tiles.png  8x4 grid of 16x16 cells:
    row0 floors : wood stone carpet_red carpet_blue grass kitchen marble vault
    row1 walls  : wood_face stone_face window bricked hedge gold counter shelfwall
    row2 props  : chest chest_open table chair bed bookshelf plant crate
    row3 props  : fountain altar telescope stove barrel gemrock rug anvil
  doors.png  6x1 grid of 16x16: h_closed h_open v_closed v_open gold gold_open
  hero.png   6x1 grid of 16x16: down0 down1 up0 up1 left0 left1  (right = HFLIP)
  items.png  9x1 grid of 12x12: coin key gem food star bigstar masterkey padlock boot

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

def checker(s, a, b, n=8):
    for y in range(16):
        for x in range(16):
            s.px(x, y, a if ((x // n) + (y // n)) % 2 == 0 else b)

# ------------------------------------------------------------------ tiles ----
T = Sheet(8, 4, 16)

# --- row 0: floors ---
# wood: horizontal planks
T.at(0, 0)
for y in range(16):
    base = (150, 106, 62) if (y // 4) % 2 == 0 else (140, 96, 56)
    T.hline(0, 15, y, base)
for y in (3, 7, 11, 15):
    T.hline(0, 15, y, (110, 74, 42))
for (x, y) in ((3, 1), (11, 5), (6, 9), (13, 13), (1, 12)):
    T.px(x, y, (168, 122, 74))

# stone: big slabs
T.at(1, 0)
for y in range(16):
    for x in range(16):
        T.px(x, y, (118, 118, 126) if (x + y) % 7 else (108, 108, 118))
for v in (0, 8):
    T.hline(0, 15, v, (92, 92, 102)); T.vline(v, 0, 15, (92, 92, 102))
    T.vline(v + 8, 0, 7 if v else 15, None)
T.vline(8, 0, 7, (92, 92, 102)); T.vline(4, 8, 15, (92, 92, 102))

# carpet red (bedroom/suite)
T.at(2, 0)
T.rect(0, 0, 15, 15, (140, 44, 48))
for y in range(0, 16, 2):
    for x in range((y // 2) % 2, 16, 2):
        T.px(x, y, (152, 52, 56))
T.rect(0, 0, 15, 0, (120, 36, 40)); T.rect(0, 15, 15, 15, (120, 36, 40))

# carpet blue (study/chapel)
T.at(3, 0)
T.rect(0, 0, 15, 15, (52, 64, 128))
for y in range(0, 16, 2):
    for x in range((y // 2) % 2, 16, 2):
        T.px(x, y, (60, 74, 142))
T.rect(0, 0, 15, 0, (42, 52, 108)); T.rect(0, 15, 15, 15, (42, 52, 108))

# grass (terrace/conservatory)
T.at(4, 0)
T.rect(0, 0, 15, 15, (84, 138, 60))
for (x, y) in ((2, 3), (7, 1), (12, 5), (4, 9), (10, 11), (14, 14), (1, 13), (8, 7)):
    T.px(x, y, (102, 158, 72)); T.px(x + 1, y, (70, 118, 50))

# kitchen checker
T.at(5, 0)
checker(T, (196, 190, 176), (150, 144, 134), 4)

# marble (ballroom/great hall)
T.at(6, 0)
T.rect(0, 0, 15, 15, (198, 198, 208))
for (x, y) in ((1, 2), (5, 6), (9, 3), (13, 9), (3, 12), (11, 13), (7, 10)):
    T.px(x, y, (176, 176, 192)); T.px(x + 1, y + 1, (176, 176, 192))
T.hline(0, 15, 8, (184, 184, 198)); T.vline(8, 0, 15, (184, 184, 198))

# vault dark floor
T.at(7, 0)
checker(T, (66, 66, 78), (58, 58, 70), 8)
T.hline(0, 15, 0, (48, 48, 60)); T.vline(0, 0, 15, (48, 48, 60))

# --- row 1: walls ---
def wall_face(col, base, dark, light, panel=True):
    T.at(col, 1)
    T.rect(0, 0, 15, 15, base)
    T.hline(0, 15, 0, light); T.hline(0, 15, 1, light)
    T.hline(0, 15, 15, dark); T.hline(0, 15, 14, dark)
    if panel:
        for x in (0, 5, 10, 15):
            T.vline(x, 2, 13, dark)

wall_face(0, (94, 64, 40), (66, 44, 28), (122, 88, 56))            # wood panel
wall_face(1, (100, 100, 112), (72, 72, 84), (128, 128, 140), False)  # stone
T.at(1, 1)
T.hline(0, 15, 5, (72, 72, 84)); T.hline(0, 15, 10, (72, 72, 84))
T.vline(5, 2, 4, (72, 72, 84)); T.vline(11, 6, 9, (72, 72, 84)); T.vline(3, 11, 13, (72, 72, 84))

# window (sealed door -> a wall with a moonlit window)
wall_face(2, (94, 64, 40), (66, 44, 28), (122, 88, 56), False)
T.at(2, 1)
T.rect(4, 3, 11, 11, (40, 30, 22))
T.rect(5, 4, 10, 10, (96, 150, 198))
T.vline(7, 4, 10, (40, 30, 22)); T.hline(5, 10, 7, (40, 30, 22))
T.px(6, 5, (150, 198, 230))

# bricked-up dead door
T.at(3, 1)
T.rect(0, 0, 15, 15, (104, 78, 62))
for y in range(0, 16, 4):
    T.hline(0, 15, y, (70, 52, 42))
    off = 0 if (y // 4) % 2 == 0 else 4
    for x in range(off, 16, 8):
        T.vline(x, y, min(y + 3, 15), (70, 52, 42))

# hedge (garden wall)
T.at(4, 1)
T.rect(0, 0, 15, 15, (52, 100, 44))
for (x, y) in ((2, 2), (6, 4), (11, 2), (14, 6), (3, 8), (9, 9), (13, 12), (5, 13), (1, 11), (8, 1)):
    T.px(x, y, (72, 126, 58)); T.px(x + 1, y, (72, 126, 58)); T.px(x, y + 1, (38, 78, 34))
T.hline(0, 15, 0, (72, 126, 58)); T.hline(0, 15, 15, (32, 64, 30))

# gold wall (antechamber row)
wall_face(5, (168, 138, 58), (122, 96, 38), (208, 176, 84), False)
T.at(5, 1)
for x in range(2, 16, 5):
    T.px(x, 4, (224, 196, 110)); T.px(x, 10, (224, 196, 110))

# shop counter (front face + top)
T.at(6, 1)
T.rect(0, 0, 15, 4, (150, 106, 62))
T.hline(0, 15, 0, (176, 130, 80))
T.rect(0, 5, 15, 15, (110, 74, 42))
for x in (3, 8, 13):
    T.vline(x, 6, 13, (88, 58, 34))
T.hline(0, 15, 5, (66, 44, 28))

# shelf wall (shop back wall with goods)
wall_face(7, (94, 64, 40), (66, 44, 28), (122, 88, 56), False)
T.at(7, 1)
T.hline(1, 14, 5, (60, 40, 26)); T.hline(1, 14, 11, (60, 40, 26))
for (x, y, c) in ((3, 3, (200, 60, 60)), (6, 3, (80, 160, 220)), (10, 3, (230, 200, 90)),
                  (4, 9, (120, 200, 90)), (8, 9, (220, 140, 70)), (12, 9, (180, 180, 200))):
    T.rect(x, y, x + 1, y + 1, c)

# --- row 2: props A ---
# chest closed
T.at(0, 2)
T.rect(2, 5, 13, 13, (140, 96, 48))
T.rect(2, 5, 13, 8, (160, 112, 58))
T.hline(2, 13, 5, (190, 140, 76))
T.rect(2, 9, 13, 9, (96, 64, 32))
T.rect(6, 8, 9, 11, (216, 180, 80)); T.rect(7, 9, 8, 10, (120, 90, 30))
T.vline(2, 5, 13, (96, 64, 32)); T.vline(13, 5, 13, (96, 64, 32))
T.hline(2, 13, 13, (80, 52, 26))

# chest open
T.at(1, 2)
T.rect(2, 2, 13, 6, (110, 74, 36))
T.rect(3, 3, 12, 5, (60, 40, 20))
T.rect(2, 7, 13, 13, (150, 104, 52))
T.rect(3, 8, 12, 10, (36, 30, 24))
T.px(6, 9, (240, 220, 120)); T.px(9, 9, (240, 220, 120))
T.hline(2, 13, 13, (80, 52, 26))

# table
T.at(2, 2)
T.rect(1, 4, 14, 9, (150, 106, 62))
T.hline(1, 14, 4, (176, 130, 80))
T.hline(1, 14, 9, (110, 74, 42))
T.rect(2, 10, 3, 13, (96, 64, 32)); T.rect(12, 10, 13, 13, (96, 64, 32))

# chair
T.at(3, 2)
T.rect(4, 2, 11, 4, (140, 96, 56))
T.rect(4, 5, 11, 10, (160, 112, 66))
T.hline(4, 11, 10, (100, 66, 38))
T.rect(4, 11, 5, 13, (96, 64, 32)); T.rect(10, 11, 11, 13, (96, 64, 32))

# bed
T.at(4, 2)
T.rect(1, 1, 14, 14, (120, 80, 44))
T.rect(2, 2, 13, 13, (196, 196, 210))
T.rect(2, 2, 13, 5, (230, 230, 240))
T.rect(2, 6, 13, 13, (170, 60, 64))
T.hline(2, 13, 6, (140, 44, 48))
T.hline(2, 13, 14, (90, 58, 30))

# bookshelf
T.at(5, 2)
T.rect(1, 1, 14, 14, (110, 74, 42))
for ry in (3, 8):
    T.rect(2, ry, 13, ry + 3, (70, 46, 26))
    x = 2
    for (w, c) in ((2, (170, 60, 60)), (1, (70, 130, 190)), (2, (200, 170, 80)),
                   (1, (100, 160, 90)), (2, (160, 100, 160)), (1, (200, 120, 70))):
        T.rect(x, ry, x + w - 1, ry + 3, c); x += w + 1
T.hline(1, 14, 14, (60, 40, 24))

# potted plant
T.at(6, 2)
T.rect(5, 9, 10, 13, (160, 100, 60))
T.hline(5, 10, 9, (190, 126, 78))
for (x, y) in ((7, 2), (5, 4), (10, 3), (4, 6), (11, 6), (7, 5), (8, 4), (6, 7), (9, 7)):
    T.px(x, y, (70, 140, 60)); T.px(x + 1, y, (90, 165, 74))
T.px(7, 8, (50, 100, 44)); T.px(8, 8, (50, 100, 44))

# crate
T.at(7, 2)
T.rect(2, 3, 13, 13, (170, 128, 74))
T.rect(2, 3, 13, 13, None)
for v in (3, 13):
    T.hline(2, 13, v, (120, 86, 48))
T.vline(2, 3, 13, (120, 86, 48)); T.vline(13, 3, 13, (120, 86, 48))
for i in range(10):
    T.px(3 + i, 4 + i * 0.0 + i if False else 4 + i, None)
for i in range(9):
    T.px(3 + i, 4 + i, (140, 102, 58))

# --- row 3: props B ---
# fountain
T.at(0, 3)
T.rect(2, 4, 13, 13, (150, 150, 162))
T.rect(3, 5, 12, 12, (70, 130, 200))
T.rect(6, 7, 9, 10, (150, 150, 162))
T.px(7, 5, (170, 210, 240)); T.px(8, 6, (170, 210, 240)); T.px(5, 8, (120, 175, 225))
T.hline(2, 13, 13, (110, 110, 124))

# altar
T.at(1, 3)
T.rect(4, 6, 11, 13, (150, 150, 162))
T.hline(3, 12, 6, (190, 190, 200))
T.rect(6, 2, 9, 5, (230, 200, 100))
T.px(7, 1, (255, 240, 160)); T.px(8, 1, (255, 240, 160))
T.hline(4, 11, 13, (110, 110, 124))

# telescope
T.at(2, 3)
T.rect(3, 11, 12, 13, (96, 64, 32))
T.vline(7, 7, 11, (70, 70, 80)); T.vline(8, 7, 11, (70, 70, 80))
for i in range(6):
    T.px(4 + i, 8 - i, (90, 90, 104)); T.px(5 + i, 8 - i, (120, 120, 136))
T.px(3, 9, (60, 60, 72)); T.px(10, 2, (150, 200, 240))

# stove
T.at(3, 3)
T.rect(2, 3, 13, 13, (80, 80, 90))
T.hline(2, 13, 3, (110, 110, 122))
T.rect(4, 6, 11, 10, (50, 50, 58))
T.px(6, 8, (240, 140, 40)); T.px(8, 8, (255, 180, 60)); T.px(7, 7, (255, 210, 90))
T.rect(5, 1, 6, 3, (60, 60, 70))
T.hline(2, 13, 13, (56, 56, 64))

# barrel
T.at(4, 3)
T.rect(4, 3, 11, 13, (140, 96, 48))
T.vline(4, 3, 13, (100, 66, 32)); T.vline(11, 3, 13, (100, 66, 32))
T.hline(4, 11, 5, (90, 60, 30)); T.hline(4, 11, 11, (90, 60, 30))
T.hline(5, 10, 3, (170, 122, 64)); T.hline(4, 11, 13, (80, 52, 26))

# gem rock
T.at(5, 3)
T.rect(3, 8, 12, 13, (100, 100, 112))
T.px(3, 8, None); T.px(12, 8, None)
T.rect(5, 5, 7, 7, (110, 220, 200)); T.px(6, 4, (110, 220, 200)); T.px(6, 5, (200, 250, 240))
T.rect(9, 7, 10, 8, (190, 110, 220)); T.px(9, 6, (230, 170, 250))
T.hline(3, 12, 13, (70, 70, 82))

# rug centre medallion
T.at(6, 3)
T.rect(1, 1, 14, 14, (150, 52, 56))
T.rect(1, 1, 14, 14, None)
T.rect(2, 2, 13, 13, (170, 66, 66))
T.rect(4, 4, 11, 11, (150, 52, 56))
T.rect(6, 6, 9, 9, (216, 180, 100))
T.px(7, 7, (150, 52, 56)); T.px(8, 8, (150, 52, 56))
for v in (1, 14):
    T.hline(1, 14, v, (110, 36, 40)); T.vline(v, 1, 14, (110, 36, 40))

# anvil (locksmith)
T.at(7, 3)
T.rect(3, 6, 12, 8, (90, 90, 104))
T.hline(3, 12, 6, (130, 130, 146))
T.px(2, 6, (90, 90, 104)); T.px(2, 7, (90, 90, 104))
T.rect(6, 9, 9, 10, (70, 70, 84))
T.rect(5, 11, 10, 13, (80, 80, 94))
T.hline(5, 10, 13, (52, 52, 64))

T.save("tiles.png")

# ------------------------------------------------------------------ doors ----
D = Sheet(6, 1, 16)
WOODF = (94, 64, 40); WOODD = (66, 44, 28); WOODL = (122, 88, 56)

# h door closed (sits in a top/bottom wall): frame + door face
D.at(0, 0)
D.rect(0, 0, 15, 15, WOODF)
D.rect(2, 1, 13, 15, (58, 38, 24))
D.rect(3, 2, 12, 15, (128, 82, 40))
for y in range(3, 15, 3):
    D.hline(3, 12, y, (104, 66, 32))
D.px(10, 9, (220, 190, 110)); D.px(10, 10, (150, 120, 60))

# h door open — dark passage
D.at(1, 0)
D.rect(0, 0, 15, 15, WOODF)
D.rect(2, 1, 13, 15, (58, 38, 24))
D.rect(3, 2, 12, 15, (26, 20, 16))
D.rect(3, 13, 12, 15, (40, 32, 24))

# v door closed (left/right walls): vertical door slab
D.at(2, 0)
D.rect(0, 0, 15, 15, WOODF)
D.rect(1, 2, 15, 13, (58, 38, 24))
D.rect(2, 3, 15, 12, (128, 82, 40))
for x in range(3, 15, 3):
    D.vline(x, 3, 12, (104, 66, 32))
D.px(5, 7, (220, 190, 110))

# v door open
D.at(3, 0)
D.rect(0, 0, 15, 15, WOODF)
D.rect(1, 2, 15, 13, (58, 38, 24))
D.rect(2, 3, 15, 12, (26, 20, 16))

# gold antechamber door (closed, double)
D.at(4, 0)
D.rect(0, 0, 15, 15, (168, 138, 58))
D.rect(1, 1, 14, 15, (122, 96, 38))
D.rect(2, 2, 13, 15, (208, 176, 84))
D.vline(7, 2, 15, (150, 120, 52)); D.vline(8, 2, 15, (150, 120, 52))
D.px(5, 8, (255, 240, 160)); D.px(10, 8, (255, 240, 160))
D.hline(2, 13, 4, (232, 204, 110))

# gold door open
D.at(5, 0)
D.rect(0, 0, 15, 15, (168, 138, 58))
D.rect(1, 1, 14, 15, (122, 96, 38))
D.rect(2, 2, 13, 15, (255, 244, 200))
D.rect(4, 4, 11, 15, (255, 252, 235))

D.save("doors.png")

# ------------------------------------------------------------------- hero ----
H = Sheet(6, 1, 16)
SKIN = (232, 190, 150); HAIR = (60, 42, 30); COAT = (46, 84, 170); COATD = (32, 60, 130)
LEG = (40, 40, 52); SHOE = (30, 26, 30); EYE = (24, 24, 32)

def hero(cell, facing, step):
    """facing: 0 down, 1 up, 2 left. step: 0/1 walk frame."""
    H.at(cell, 0)
    # legs
    if step == 0:
        H.rect(6, 12, 7, 14, LEG); H.rect(8, 12, 9, 14, LEG)
        H.rect(6, 15, 7, 15, SHOE); H.rect(8, 15, 9, 15, SHOE)
    else:
        H.rect(5, 12, 6, 14, LEG); H.rect(9, 12, 10, 14, LEG)
        H.rect(5, 15, 6, 15, SHOE); H.rect(9, 15, 10, 15, SHOE)
    # coat body
    H.rect(4, 7, 11, 12, COAT)
    H.hline(4, 11, 12, COATD)
    H.vline(4, 7, 12, COATD); H.vline(11, 7, 12, COATD)
    if facing == 0:
        H.vline(7, 8, 11, COATD)                      # front seam
        H.px(7, 8, (216, 190, 90))                    # button
    # arms
    H.rect(3, 8, 3, 11, COATD); H.rect(12, 8, 12, 11, COATD)
    # head
    H.rect(4, 1, 11, 6, SKIN)
    if facing == 1:
        H.rect(4, 1, 11, 6, HAIR)                     # back of head: all hair
        H.hline(4, 11, 6, (44, 30, 22))
    else:
        H.rect(4, 1, 11, 2, HAIR)                     # fringe
        H.px(4, 3, HAIR); H.px(11, 3, HAIR)
        if facing == 0:
            H.px(6, 4, EYE); H.px(9, 4, EYE)
        else:                                          # left profile
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
I.rect(4, 3, 5, 4, None)
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

# big star (75/100pt) — same but with sparkle ring
I.at(5, 0)
I.px(5, 0, S); I.px(6, 0, S)
I.rect(4, 2, 7, 3, S); I.hline(0, 11, 3, S)
I.rect(3, 4, 8, 5, S)
I.rect(1, 6, 4, 8, S); I.rect(7, 6, 10, 8, S)
I.rect(5, 6, 6, 6, S)
I.px(5, 1, (255, 250, 200)); I.px(0, 10, (255, 245, 170)); I.px(11, 0, (255, 245, 170))

# master key (big ornate)
I.at(6, 0)
I.rect(2, 1, 7, 6, (250, 215, 90))
I.rect(3, 2, 6, 5, None)
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
