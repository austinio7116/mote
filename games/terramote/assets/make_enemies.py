#!/usr/bin/env python3
"""TerraMote — procedurally drawn sprites for the ADDED enemies (the original
roster is extracted from AI sheets in extract_sheets.py; these are hand-coded
pixel art in the same chunky, dark-outlined style).

Writes assets/<name>.png + anims/<name>.anims for each. `mote bake` turns the
pair into src/<name>.anim.h. Re-run after tweaking.

  slime_sand   16x12  desert slime hopper           (idle x2, jump)
  hornet       16x12  fast forest darter            (wings x2)
  crawler      16x12  cave beetle, ground walker    (legs x2)
  eater        16x14  corruption flyer, toothy maw  (maw x2)
  wraith       16x16  phasing phantom               (drift x2)
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ANIM = os.path.join(GAME, "anims")


def snap(c):
    r, g, b = c[:3]
    a = c[3] if len(c) > 3 else 255
    return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3, a)


class Cell:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[None] * w for _ in range(h)]

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h and c is not None:
            self.px[y][x] = snap(c)

    def get(self, x, y):
        return self.px[y][x] if 0 <= x < self.w and 0 <= y < self.h else None

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def outline(self, c):
        """dark 1px outline around every opaque pixel that borders empty space"""
        edits = []
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y][x] is not None:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if self.get(x + dx, y + dy) is not None:
                        edits.append((x, y)); break
        for x, y in edits:
            self.put(x, y, c)


def build(name, cells, tile_w, tile_h, clip, loop, fps, durs):
    n = len(cells)
    img = Image.new("RGBA", (n * tile_w, tile_h), (0, 0, 0, 0))
    for i, cell in enumerate(cells):
        for y in range(tile_h):
            for x in range(tile_w):
                c = cell.px[y][x] if y < cell.h and x < cell.w else None
                if c is not None:
                    img.putpixel((i * tile_w + x, y), c)
    img.save(os.path.join(HERE, name + ".png"))
    with open(os.path.join(ANIM, name + ".anims"), "w") as f:
        f.write("sheet assets/%s.png\n" % name)
        f.write("tile %d %d\n" % (tile_w, tile_h))
        f.write("clips 1\n")
        f.write("clip %s %d %d 0 0 %d\n" % (clip, loop, fps, n))
        for i in range(n):
            f.write("f %d %d -\n" % (i, durs[i]))
    print("wrote", name, img.size)


# ---------------------------------------------------------------- sand slime
def sand_slime():
    BASE = (226, 198, 116); SH = (196, 164, 84); HI = (248, 232, 168)
    OUT = (120, 96, 44); EYE = (60, 44, 20)
    # dome silhouette, per-row half-width, bottom flat
    def dome(squash):
        c = Cell(16, 12)
        rows = [(6, 2), (4, 3), (3, 4), (2, 6), (2, 8), (1, 10), (1, 10)]
        top = 5 + squash
        for i, (inset, _) in enumerate(rows):
            y = top + i
            if y >= 11: y = 11
            c.rect(inset, y, 15 - inset, y, BASE)
        c.rect(1, 11, 14, 11, SH)                 # flat wet base
        for x in range(3, 12, 2): c.put(x, top, HI)  # top sheen
        c.put(4, top + 1, HI); c.put(5, top, HI)
        # eyes
        ey = top + 3
        c.rect(5, ey, 6, ey + 1, EYE); c.rect(9, ey, 10, ey + 1, EYE)
        c.outline(OUT)
        return c
    idle0, idle1 = dome(0), dome(1)
    jump = Cell(16, 12)                            # stretched, airborne
    for i, inset in enumerate((5, 3, 2, 2, 2, 3, 4)):
        jump.rect(inset, 2 + i, 15 - inset, 2 + i, BASE)
    jump.rect(3, 9, 12, 9, SH)
    jump.rect(5, 5, 6, 6, EYE); jump.rect(9, 5, 10, 6, EYE)
    for x in range(4, 11, 2): jump.put(x, 2, HI)
    jump.outline(OUT)
    build("slime_sand", [idle0, idle1, jump], 16, 12, "idle", 1, 3, [260, 260, 120])
    # jump is cell 2 — same sheet, the game plays idle(0,1) grounded, jump(2) airborne
    with open(os.path.join(ANIM, "slime_sand.anims"), "w") as f:
        f.write("sheet assets/slime_sand.png\ntile 16 12\nclips 2\n")
        f.write("clip idle 1 3 0 0 2\nf 0 260 -\nf 1 260 -\n")
        f.write("clip jump 1 8 0 0 1\nf 2 120 -\n")


# ---------------------------------------------------------------- hornet
def hornet():
    BODY = (240, 196, 40); STRIPE = (40, 34, 22); HEAD = (60, 50, 30)
    WING = (210, 224, 245); WING2 = (150, 172, 205); OUT = (70, 56, 20)
    STING = (230, 230, 235)
    def frame(wing_up):
        c = Cell(16, 12)
        # abdomen (right), striped
        c.rect(7, 5, 12, 8, BODY)
        c.rect(8, 6, 8, 7, STRIPE); c.rect(10, 6, 10, 7, STRIPE)
        c.rect(12, 6, 12, 7, STRIPE)
        c.put(13, 6, STING); c.put(14, 6, STING)          # stinger tail
        # thorax + head (left)
        c.rect(4, 5, 6, 8, HEAD)
        c.rect(2, 6, 3, 7, HEAD)
        c.put(2, 6, (230, 60, 60))                        # red eye
        # wings
        if wing_up:
            c.rect(6, 1, 10, 3, WING); c.rect(7, 2, 9, 2, WING2)
        else:
            c.rect(6, 8, 10, 10, WING); c.rect(7, 9, 9, 9, WING2)
        c.outline(OUT)
        return c
    build("hornet", [frame(True), frame(False)], 16, 12, "fly", 1, 14, [70, 70])


# ---------------------------------------------------------------- crawler
def crawler():
    SHELL = (70, 58, 78); SHELL_HI = (108, 92, 120); BELLY = (44, 36, 50)
    LEG = (32, 26, 38); EYE = (240, 120, 60); MAND = (150, 130, 90)
    OUT = (20, 16, 24)
    def frame(step):
        c = Cell(16, 12)
        c.rect(3, 3, 12, 8, SHELL)                        # carapace
        c.rect(4, 3, 11, 3, SHELL_HI)                     # top sheen
        c.put(6, 4, SHELL_HI); c.put(9, 4, SHELL_HI)
        c.rect(4, 8, 11, 8, BELLY)
        # head + mandibles (right, faces right)
        c.rect(12, 4, 13, 7, SHELL)
        c.put(14, 4, MAND); c.put(14, 7, MAND)
        c.put(13, 5, EYE)                                 # glowing eye
        # legs (alternate up/down per step)
        for i, lx in enumerate((4, 7, 10)):
            dn = (i % 2) == step
            ly = 10 if dn else 9
            c.put(lx, 9, LEG); c.put(lx, ly, LEG)
            c.put(lx + 1, 9, LEG); c.put(lx + 1, ly, LEG)
        c.outline(OUT)
        return c
    build("crawler", [frame(0), frame(1)], 16, 12, "walk", 1, 9, [110, 110])


# ---------------------------------------------------------------- eater
def eater():
    BODY = (128, 70, 168); BODY_SH = (92, 46, 128); BODY_HI = (170, 110, 210)
    MAW = (30, 12, 40); TOOTH = (236, 226, 240); OUT = (46, 22, 66)
    EYE = (250, 210, 90)
    def frame(open_):
        c = Cell(16, 14)
        # rounded flying head
        rows = [(6, 1), (4, 3), (3, 5), (2, 7), (2, 9), (3, 10), (4, 11), (6, 12)]
        for inset, y in rows:
            c.rect(inset, y, 15 - inset, y, BODY)
        for x in range(4, 12, 2): c.put(x, 1, BODY_HI)
        c.put(5, 2, BODY_HI); c.put(3, 4, BODY_SH); c.put(12, 4, BODY_SH)
        # vertical maw down the middle
        mw = 3 if open_ else 1
        cx = 8
        for y in range(4, 12):
            c.rect(cx - mw, y, cx - 1 + (0 if open_ else 1), y, MAW)
        if open_:
            for y in range(4, 12, 2):
                c.put(cx - mw, y, TOOTH); c.put(cx + mw - 1, y, TOOTH)
        # two eyes flanking
        c.put(5, 5, EYE); c.put(11, 5, EYE)
        c.outline(OUT)
        return c
    build("eater", [frame(False), frame(True)], 16, 14, "fly", 1, 6, [220, 220])


# ---------------------------------------------------------------- wraith
def wraith():
    ROBE = (150, 168, 200); ROBE_SH = (104, 122, 158); HOOD = (70, 84, 116)
    EYE = (150, 240, 255); WISP = (120, 140, 178); OUT = (44, 54, 80)
    def frame(sway):
        c = Cell(16, 16)
        s = 1 if sway else 0
        # hood/head
        c.rect(5 + s, 1, 10 + s, 2, HOOD)
        c.rect(4 + s, 3, 11 + s, 6, HOOD)
        c.rect(6 + s, 4, 7 + s, 5, (12, 16, 26))          # shadowed face
        c.put(6 + s, 4, EYE); c.put(9 + s, 4, EYE)        # glowing eyes
        # robe body, tapering to wisps
        for i, inset in enumerate((3, 3, 4, 4, 5, 5, 6)):
            y = 6 + i
            off = s if i < 3 else (1 - s)
            c.rect(inset + off, y, 12 - inset + off + 1, y, ROBE if i % 2 else ROBE_SH)
        # wispy tails (dithered, translucent feel)
        for x in (5, 8, 11):
            c.put(x + (s if x < 8 else 1 - s), 13, WISP)
            if (x + sway) % 2 == 0:
                c.put(x + (s if x < 8 else 1 - s), 14, WISP)
        c.outline(OUT)
        return c
    build("wraith", [frame(False), frame(True)], 16, 16, "drift", 1, 4, [320, 320])


if __name__ == "__main__":
    os.makedirs(ANIM, exist_ok=True)
    sand_slime(); hornet(); crawler(); eater(); wraith()
    print("done")
