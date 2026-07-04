#!/usr/bin/env python3
"""Hang Time art generator — writes the EDITABLE source PNGs (bake with `mote bake`).

Outputs (all transparent-background RGBA):
  hero.png    64x16 — four 16x16 frames: hang-back, hang-fore, tuck (flying), flail (falling)
  anchor.png  12x24 — two 12x12 frames: normal, in-grab-range highlight
  pad.png     24x16 — two 24x8 frames: bounce pad normal, squashed
  flag.png    16x72 — level marker: checkered flag on a tall pole
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
    # frame 0: grey stud with a rim and a top-left glint
    d.ellipse([0, 0, 11, 11], fill=(84, 84, 92, 255))
    d.ellipse([1, 1, 10, 10], fill=(126, 126, 136, 255))
    d.ellipse([3, 3, 6, 6], fill=(158, 158, 168, 255))
    d.point([(4, 3), (3, 4)], fill=(198, 198, 208, 255))
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


def flag():
    img = Image.new("RGBA", (16, 72), NONE)
    d = ImageDraw.Draw(img)
    POLE = (74, 74, 82, 255)
    POLE_D = (46, 46, 52, 255)
    BLACK = (30, 30, 34, 255)
    WHITE = (245, 242, 232, 255)
    # pole with a darker right edge and a rounded cap
    d.rectangle([2, 1, 3, 71], fill=POLE)
    d.line([(3, 1), (3, 71)], fill=POLE_D)
    d.point([(2, 0), (3, 0)], fill=POLE_D)
    # checkered "level complete" flag, 2px squares
    for gy in range(4):
        for gx in range(5):
            c = BLACK if (gx + gy) % 2 == 0 else WHITE
            d.rectangle([4 + gx * 2, 2 + gy * 2, 5 + gx * 2, 3 + gy * 2], fill=c)
    img.save(os.path.join(HERE, "flag.png"))


def icon():
    img = Image.new("RGBA", (60, 60), NONE)
    d = ImageDraw.Draw(img)
    # yellow gradient sky
    for y in range(60):
        t = y / 59.0
        d.line([(0, y), (59, y)],
               fill=(int(252 - 90 * t), int(238 - 122 * t), int(168 - 150 * t), 255))

    SCALE, ANGLE = 3, -38          # hero blow-up + swing tilt (deg, CW)
    ax, ay = 45, 10                # anchor stud centre
    hx, hy = 31, 23                # where the hero's hands must land

    # hero mid-swing, big and tilted along the rope
    hero_img = Image.new("RGBA", (16, 16), NONE)
    paint(hero_img, 0, 0, HANG_BACK)
    big = hero_img.resize((16 * SCALE, 16 * SCALE), Image.NEAREST)
    rot = big.rotate(ANGLE, Image.NEAREST, expand=True)
    # run a probe pixel at the hands (6.5,1 in the 16px art) through the SAME
    # transform to find where they end up, then paste so they sit at (hx,hy)
    probe = Image.new("RGBA", big.size, NONE)
    pd = ImageDraw.Draw(probe)
    cx0, cy0 = int(6.5 * SCALE), int(1 * SCALE)
    pd.rectangle([cx0 - 1, cy0 - 1, cx0 + 1, cy0 + 1], fill=(255, 0, 0, 255))
    pr = probe.rotate(ANGLE, Image.NEAREST, expand=True)
    bbox = pr.getbbox()
    px_, py_ = (bbox[0] + bbox[2]) // 2, (bbox[1] + bbox[3]) // 2

    d.line([(ax, ay), (hx, hy)], fill=(96, 66, 30, 255), width=2)   # rope into the hands
    d.ellipse([ax - 7, ay - 7, ax + 7, ay + 7], fill=(84, 84, 92, 255))
    d.ellipse([ax - 6, ay - 6, ax + 6, ay + 6], fill=(126, 126, 136, 255))
    d.ellipse([ax - 4, ay - 4, ax - 1, ay - 1], fill=(158, 158, 168, 255))
    img.alpha_composite(rot, (hx - px_, hy - py_))
    # a few faint dots tracing the swing arc ahead of the hero
    for mx, my in [(41, 48), (48, 43), (53, 35)]:
        d.ellipse([mx, my, mx + 1, my + 1], fill=(255, 255, 245, 235))
    img.save(os.path.join(HERE, "..", "icon.png"))


hero()
flag()
anchor()
pad()
icon()
print("wrote hero.png anchor.png pad.png flag.png ../icon.png")
