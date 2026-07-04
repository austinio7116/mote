#!/usr/bin/env python3
"""Hang Time art generator — writes the EDITABLE source PNGs (bake with `mote bake`).

Outputs (all transparent-background RGBA):
  hero.png    64x16 — four 16x16 frames: hang-back, hang-fore, tuck (flying), flail (falling)
  anchor.png  12x24 — two 12x12 frames: normal, in-grab-range highlight
  pad.png     24x16 — two 24x8 frames: bounce pad normal, squashed
  ../icon.png 60x60 — launcher icon
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))

# palette
SKIN = (240, 195, 150, 255)
HAIR = (58, 38, 26, 255)
SHIRT = (36, 70, 120, 255)     # navy — pops on the yellow sky
SHIRT_D = (24, 48, 88, 255)
PANTS = (40, 42, 58, 255)
SHOE = (25, 25, 30, 255)
NONE = (0, 0, 0, 0)

PAL = {'.': NONE, 's': SKIN, 'h': HAIR, 'b': SHIRT, 'd': SHIRT_D,
       'p': PANTS, 'k': SHOE}

# 16x16 frames. Hands (the rope grip) are at the TOP CENTRE (x=7..8, y=0)
# for the two hang frames; the sprite pivots about its centre (8,8) in-game.
HANG_BACK = [   # swinging, legs trailing behind (left)
    "......ss........",
    "......ss........",
    ".....s..s.......",
    ".....s..s.......",
    ".....shhs.......",
    "....shhhhs......",
    "....shssh.......",
    ".....ssss.......",
    ".....bbbb.......",
    "....bbbbbb......",
    "....dbbbbd......",
    ".....pppp.......",
    "....pp..pp......",
    "...pp....pp.....",
    "..kk......kk....",
    "................",
]
HANG_FORE = [   # swinging, legs kicked forward (right)
    "......ss........",
    "......ss........",
    ".....s..s.......",
    ".....s..s.......",
    ".....shhs.......",
    "....shhhhs......",
    "....shssh.......",
    ".....ssss.......",
    ".....bbbb.......",
    "....bbbbbb......",
    "....dbbbbd......",
    ".....pppp.......",
    "......pppp......",
    ".......pppp.....",
    "........kkkk....",
    "................",
]
TUCK = [        # released, sailing — knees up, arms swept back
    "................",
    "......hhh.......",
    ".....hhhhh......",
    ".....shsh.......",
    ".....ssss.......",
    "....sbbbb.......",
    "...s.bbbbb......",
    "..s..dbbbd......",
    ".....pppp.......",
    "....ppppp.......",
    "....pp.pp.......",
    "....pp.pp.......",
    "...kk..kk.......",
    "................",
    "................",
    "................",
]
FLAIL = [       # falling — arms up and out, legs apart
    "..s........s....",
    "..s........s....",
    "...s......s.....",
    "...s.hhh..s.....",
    "....shhhhs......",
    ".....shsh.......",
    ".....ssss.......",
    ".....bbbb.......",
    "....bbbbbb......",
    "....dbbbbd......",
    ".....pppp.......",
    "....pp..pp......",
    "...pp....pp.....",
    "...pp....pp.....",
    "..kk......kk....",
    "................",
]


def paint(img, ox, oy, rows):
    px = img.load()
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            c = PAL[ch]
            if c[3]:
                px[ox + x, oy + y] = c


def hero():
    img = Image.new("RGBA", (64, 16), NONE)
    for i, rows in enumerate([HANG_BACK, HANG_FORE, TUCK, FLAIL]):
        paint(img, i * 16, 0, rows)
    img.save(os.path.join(HERE, "hero.png"))


def anchor():
    img = Image.new("RGBA", (12, 24), NONE)
    d = ImageDraw.Draw(img)
    # frame 0: dark grey stud with a rim and a top-left glint
    d.ellipse([0, 0, 11, 11], fill=(46, 46, 52, 255))
    d.ellipse([1, 1, 10, 10], fill=(74, 74, 82, 255))
    d.ellipse([3, 3, 6, 6], fill=(100, 100, 110, 255))
    d.point([(4, 3), (3, 4)], fill=(140, 140, 150, 255))
    # frame 1: in-grab-range — lighter body, pale rim
    d.ellipse([0, 12, 11, 23], fill=(235, 235, 225, 255))
    d.ellipse([1, 13, 10, 22], fill=(96, 96, 106, 255))
    d.ellipse([3, 15, 6, 18], fill=(130, 130, 142, 255))
    d.point([(4, 15), (3, 16)], fill=(180, 180, 190, 255))
    img.save(os.path.join(HERE, "anchor.png"))


def pad():
    img = Image.new("RGBA", (24, 16), NONE)
    d = ImageDraw.Draw(img)
    RED = (200, 60, 40, 255)
    WHITE = (245, 240, 230, 255)
    LEG = (52, 52, 58, 255)
    # frame 0: sprung pad — striped canvas on dark legs. The canvas spans the
    # full 24px (stripe period 6 divides 24) so cluster segments tile
    # seamlessly into one long platform.
    d.rectangle([0, 0, 23, 3], fill=RED)
    for x in range(0, 24, 6):
        d.rectangle([x + 3, 0, x + 5, 3], fill=WHITE)
    d.rectangle([0, 1, 23, 1], fill=(120, 30, 20, 255))
    d.rectangle([3, 4, 5, 7], fill=LEG)
    d.rectangle([18, 4, 20, 7], fill=LEG)
    # frame 1: squashed — canvas compressed at the bottom of the cell
    d.rectangle([0, 12, 23, 14], fill=RED)
    for x in range(0, 24, 6):
        d.rectangle([x + 3, 12, x + 5, 14], fill=WHITE)
    d.rectangle([3, 15, 5, 15], fill=LEG)
    d.rectangle([18, 15, 20, 15], fill=LEG)
    img.save(os.path.join(HERE, "pad.png"))


def icon():
    img = Image.new("RGBA", (60, 60), NONE)
    d = ImageDraw.Draw(img)
    # yellow gradient sky
    for y in range(60):
        t = y / 59.0
        d.line([(0, y), (59, y)],
               fill=(int(252 - 90 * t), int(238 - 122 * t), int(168 - 150 * t), 255))
    # anchor stud, rope, swinging hero
    ax, ay = 36, 10
    d.line([(ax, ay), (22, 34)], fill=(90, 60, 30, 255), width=2)
    d.ellipse([ax - 6, ay - 6, ax + 6, ay + 6], fill=(46, 46, 52, 255))
    d.ellipse([ax - 5, ay - 5, ax + 5, ay + 5], fill=(74, 74, 82, 255))
    d.ellipse([ax - 3, ay - 3, ax, ay], fill=(110, 110, 120, 255))
    # hero (chunky, tilted along the rope) at rope end
    hero_img = Image.new("RGBA", (16, 16), NONE)
    paint(hero_img, 0, 0, HANG_BACK)
    hero_big = hero_img.resize((40, 40), Image.NEAREST).rotate(-32, Image.NEAREST, expand=True)
    img.alpha_composite(hero_big, (2, 22))
    img.save(os.path.join(HERE, "..", "icon.png"))


hero()
anchor()
pad()
icon()
print("wrote hero.png anchor.png pad.png ../icon.png")
