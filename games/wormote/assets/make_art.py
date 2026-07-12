#!/usr/bin/env python3
"""wormote art — writes the EDITABLE source PNGs (worms.png + ../icon.png).

worms.png: 4 team-colour rows x 4 frames (stand, walk1, walk2, air),
12x12 cells, worm drawn from the pixel templates below (facing RIGHT;
the game HFLIPs for left). Edit the templates or the PNG in the Studio.
"""
from PIL import Image, ImageDraw
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CELL = 12

# . transparent  k outline  o base  d dark  l light  w eye-white  p pupil
FRAMES = [
    [  # stand
        ".........",
        "....kk...",
        "...klok..",
        "...kwpk..",
        "..koook..",
        "..koook..",
        "..kodok..",
        "..koook..",
        "..kodk...",
        "..kdk....",
        "..kk.....",
        ".........",
    ],
    [  # walk1 — tail kicks back
        ".........",
        "....kk...",
        "...klok..",
        "...kwpk..",
        "..koook..",
        "..koook..",
        "..kodok..",
        "..koook..",
        "...kodk..",
        "....kdk..",
        ".....kk..",
        ".........",
    ],
    [  # walk2 — tail kicks forward
        ".........",
        "....kk...",
        "...klok..",
        "...kwpk..",
        "..koook..",
        "..koook..",
        "..kodok..",
        "..koook..",
        ".kdok....",
        ".kdk.....",
        ".kk......",
        ".........",
    ],
    [  # air — stretched, tail trailing
        "....kk...",
        "...klok..",
        "...kwpk..",
        "..koook..",
        "..koook..",
        "..kodok..",
        "..koook..",
        "..kodok..",
        "..kook...",
        "..kdk....",
        "..kk.....",
        ".........",
    ],
]

TEAMS = [  # base colour per row: green, red, blue, yellow
    (86, 198, 66),
    (226, 66, 54),
    (74, 134, 236),
    (232, 198, 58),
]

def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)

def draw_worm(img, ox, oy, rows, base):
    cols = {
        'k': (16, 12, 10, 255),
        'o': base + (255,),
        'd': shade(base, 0.62) + (255,),
        'l': shade(base, 1.45) + (255,),
        'w': (245, 245, 240, 255),
        'p': (14, 10, 10, 255),
    }
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != '.':
                img.putpixel((ox + x, oy + y), cols[ch])

def make_worms():
    img = Image.new("RGBA", (CELL * len(FRAMES), CELL * len(TEAMS)), (0, 0, 0, 0))
    for r, team in enumerate(TEAMS):
        for f, rows in enumerate(FRAMES):
            ox = f * CELL + (CELL - len(rows[0])) // 2
            oy = r * CELL
            draw_worm(img, ox, oy, rows, team)
    img.save(os.path.join(HERE, "worms.png"))
    print("worms.png", img.size)

def make_icon():
    S = 60
    img = Image.new("RGBA", (S, S), (14, 11, 18, 255))
    d = ImageDraw.Draw(img)
    # dirt frame (3 flat shades — the launcher icon palette is small) + dark cave
    dirt = [(96, 60, 30), (112, 72, 38), (84, 52, 26)]
    for y in range(S):
        for x in range(S):
            d.point((x, y), dirt[(x * 7 + y * 13 + (x * y) % 5) % 9 % 3] + (255,))
    d.ellipse((3, 3, 57, 57), fill=(15, 12, 20, 255))
    # explosion top-right: spiky layered fireball
    cx, cy = 43, 17
    for r, col in ((13, (168, 48, 16)), (10, (240, 120, 24)), (7, (255, 190, 48)), (4, (255, 245, 180))):
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=col + (255,))
    import math
    for k in range(8):  # flare spikes
        a = k * math.pi / 4 + 0.4
        x1, y1 = cx + math.cos(a) * 10, cy + math.sin(a) * 10
        x2, y2 = cx + math.cos(a) * 16, cy + math.sin(a) * 16
        d.line((x1, y1, x2, y2), fill=(255, 170, 40, 255), width=2)
    # rope: from the worm's back up to the rim, with a hook
    d.line((14, 30, 8, 6), fill=(205, 200, 185, 255), width=1)
    d.line((15, 30, 9, 6), fill=(150, 145, 130, 255), width=1)
    d.rectangle((7, 4, 10, 7), fill=(220, 220, 230, 255))
    # the worm: big green maggot, bottom-left, leaning at the blast
    base = TEAMS[0]
    dark = shade(base, 0.58)
    lite = shade(base, 1.4)
    body = [  # (cx, cy, rx, ry) segment ellipses, tail -> head
        (16, 50, 8, 6), (17, 44, 8, 7), (19, 37, 8, 8), (22, 30, 8, 8), (25, 24, 8, 8),
    ]
    for sx2, sy2, rx, ry in body:  # outline pass
        d.ellipse((sx2 - rx - 1, sy2 - ry - 1, sx2 + rx + 1, sy2 + ry + 1), fill=(12, 10, 8, 255))
    for sx2, sy2, rx, ry in body:
        d.ellipse((sx2 - rx, sy2 - ry, sx2 + rx, sy2 + ry), fill=base + (255,))
    for sx2, sy2, rx, ry in body:  # belly light
        d.ellipse((sx2 - rx + 2, sy2 - ry + 2, sx2 + 1, sy2 + ry - 2), fill=lite + (255,))
    for sx2, sy2, rx, ry in body[:3]:  # underside shade
        d.ellipse((sx2, sy2 + 1, sx2 + rx - 1, sy2 + ry - 1), fill=dark + (255,))
    # face: big eye looking at the blast + a grin
    d.ellipse((26, 17, 34, 25), fill=(250, 250, 245, 255))
    d.ellipse((30, 19, 34, 23), fill=(14, 10, 10, 255))
    d.arc((24, 24, 33, 31), 20, 120, fill=(12, 10, 8, 255), width=2)
    # gun under the arm, barrel toward the blast, muzzle flash
    d.line((24, 30, 37, 25), fill=(30, 30, 36, 255), width=4)
    d.line((25, 29, 36, 25), fill=(120, 124, 138, 255), width=2)
    d.polygon([(37, 22), (42, 24), (38, 27), (40, 24)], fill=(255, 240, 160, 255))
    img.convert("RGB").save(os.path.join(HERE, "..", "icon.png"))
    print("icon.png", (S, S))

if __name__ == "__main__":
    make_worms()
    make_icon()
