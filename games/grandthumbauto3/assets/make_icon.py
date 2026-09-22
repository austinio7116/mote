#!/usr/bin/env python3
"""Grand Thumb Auto III launcher icon (60x60).

The original game's icon was a top-down police chase, which was exactly right
for a top-down game and says nothing about this one. This is the third-person
read instead: a car seen from BEHIND on a road running to a vanishing point,
a city skyline on the horizon, and the III numeral over it.

Colours are lifted from the game rather than invented, so the icon and the
running game agree: the dusk keyframe of the day/night palette in game.c
(sky top 46,44,92 -> horizon 226,110,70), the title screen's gold and its
near-black drop shadow, and CAR_COL[34], the red sports car.

Lives in the game ROOT as icon.png; `mote bake` turns it into src/icon.h.
"""
from PIL import Image, ImageDraw

S = 60
SKY_TOP   = (46, 44, 92)
SKY_HOR   = (226, 110, 70)
GOLD      = (244, 204, 72)
SHADOW    = (12, 10, 14)
ASPHALT   = (58, 60, 70)
KERB      = (120, 122, 132)
CAR_RED   = (168, 44, 32)      # CAR_COL[34]
CAR_DARK  = (96, 24, 18)
GLASS     = (34, 30, 44)

HORIZON = 26                    # where sky meets road

im = Image.new("RGBA", (S, S), (0, 0, 0, 255))
d = ImageDraw.Draw(im)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# ---- sky: the same vertical ramp the game paints, top -> horizon ----------
for y in range(HORIZON):
    d.line([(0, y), (S, y)], fill=lerp(SKY_TOP, SKY_HOR, y / (HORIZON - 1)))

# ---- skyline: blocky silhouettes sitting on the horizon -------------------
# Heights chosen by hand so the roofline reads as a city rather than a comb.
towers = [(0, 9), (5, 15), (11, 6), (15, 18), (21, 11), (26, 7),
          (31, 20), (37, 12), (42, 8), (46, 16), (52, 10), (56, 13)]
for x, h in towers:
    w = 5 if x % 2 == 0 else 4
    top = HORIZON - h
    # darker the taller, so the skyline has depth rather than one flat tone
    shade = 26 + (h * 10) // 20
    d.rectangle([x, top, x + w - 1, HORIZON - 1], fill=(shade, shade + 2, shade + 10))
    for wy in range(top + 2, HORIZON - 1, 3):      # lit windows
        for wx in range(x + 1, x + w - 1, 2):
            if (wx * 7 + wy * 13) % 5 == 0:
                d.point((wx, wy), fill=(232, 186, 92))

# ---- road: a trapezoid to a vanishing point on the horizon ---------------
VP = S // 2
d.polygon([(VP - 3, HORIZON), (VP + 3, HORIZON), (S + 14, S), (-14, S)], fill=ASPHALT)
d.line([(VP - 3, HORIZON), (-14, S)], fill=KERB)
d.line([(VP + 3, HORIZON), (S + 14, S)], fill=KERB)

# centre dashes, spaced so they compress toward the vanishing point
for i, (y0, y1) in enumerate([(30, 32), (35, 38), (42, 46), (51, 57)]):
    half = 0.4 + i * 0.5
    d.polygon([(VP - half, y0), (VP + half, y0),
               (VP + half + 0.5, y1), (VP - half - 0.5, y1)],
              fill=(226, 228, 234))

# ---- the car, from behind: body slab, cabin, wheel line, lights ----------
# Matches how the game actually builds a car — body box, cabin box on top,
# dark slab under the sill — so the icon is the same object the player drives.
CY = 33
d.rectangle([15, CY + 5, 45, CY + 9], fill=(20, 20, 24))        # wheel line
d.rectangle([16, CY - 2, 44, CY + 6], fill=CAR_RED)             # body
d.rectangle([16, CY + 4, 44, CY + 6], fill=CAR_DARK)            # shadowed sill
d.rectangle([21, CY - 8, 39, CY - 1], fill=CAR_DARK)            # cabin
d.rectangle([23, CY - 7, 37, CY - 3], fill=GLASS)               # rear glass
d.rectangle([17, CY + 1, 21, CY + 3], fill=(236, 92, 70))       # tail lights
d.rectangle([39, CY + 1, 43, CY + 3], fill=(236, 92, 70))

# ---- III: the numeral, gold on a near-black shadow, like the title -------
BAR_W, BAR_H, GAP = 5, 17, 5
total = BAR_W * 3 + GAP * 2
x0 = (S - total) // 2
y0 = S - BAR_H - 3
for i in range(3):
    bx = x0 + i * (BAR_W + GAP)
    d.rectangle([bx + 1, y0 + 1, bx + BAR_W, y0 + BAR_H], fill=SHADOW)
    d.rectangle([bx, y0, bx + BAR_W - 1, y0 + BAR_H - 1], fill=GOLD)
    # serif caps top and bottom, so it reads as a numeral not three bars
    d.rectangle([bx - 2, y0, bx + BAR_W, y0 + 1], fill=GOLD)
    d.rectangle([bx - 2, y0 + BAR_H - 2, bx + BAR_W, y0 + BAR_H - 1], fill=GOLD)

im.convert("RGB").save("../icon.png")
print("wrote games/grandthumbauto3/icon.png (60x60)")
