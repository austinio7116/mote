#!/usr/bin/env python3
"""Generate games/moria/icon.png -- the launcher emblem for the Moria port.

A bold, high-contrast dungeon motif that reads at launcher size: a stone
archway (the descent into Moria) framing the classic roguelike '@' hero on a
torch-lit black background.  Edit the palette / shapes here and re-run, then
`mote bake games/moria` turns icon.png into src/icon.h.
"""
import os
from PIL import Image, ImageDraw

S = 60
img = Image.new("RGBA", (S, S), (0, 0, 0, 255))
d = ImageDraw.Draw(img)

# --- background: dark dungeon floor with a warm torch glow toward the arch ---
for y in range(S):
    for x in range(S):
        # radial glow centred a bit above middle
        dx, dy = x - S / 2, y - S * 0.42
        r = (dx * dx + dy * dy) ** 0.5
        g = max(0, 1.0 - r / 42.0)
        img.putpixel((x, y), (int(26 * g) + 4, int(16 * g) + 3, int(6 * g) + 6, 255))

# --- stone archway (grey blocks) framing a black doorway ---------------------
stone = (128, 128, 120)
stone_hi = (168, 168, 156)
stone_lo = (86, 86, 80)
# outer arch pillars
d.rectangle([6, 20, 16, 55], fill=stone)
d.rectangle([44, 20, 54, 55], fill=stone)
# arch top (stepped)
d.rectangle([6, 12, 54, 20], fill=stone)
d.rectangle([12, 7, 48, 13], fill=stone)
# block seams / shading
for yy in range(12, 55, 7):
    d.line([6, yy, 16, yy], fill=stone_lo)
    d.line([44, yy, 54, yy], fill=stone_lo)
d.line([6, 20, 6, 55], fill=stone_hi)
d.line([44, 20, 44, 55], fill=stone_hi)
# doorway (black opening)
d.rectangle([18, 16, 42, 55], fill=(0, 0, 0))

# --- '>' down-stair hint at the threshold ------------------------------------
stair = (110, 150, 110)
d.line([21, 48, 27, 51], fill=stair, width=2)
d.line([27, 51, 21, 54], fill=stair, width=2)

# --- the hero: a bold amber '@' ---------------------------------------------
amber = (255, 196, 64)
amber_hi = (255, 230, 150)
# ring of the @
d.ellipse([22, 22, 40, 42], outline=amber, width=3)
# inner tail
d.arc([26, 26, 36, 38], start=20, end=300, fill=amber_hi, width=2)
d.line([36, 34, 40, 40], fill=amber, width=3)

# subtle vignette border
d.rectangle([0, 0, S - 1, S - 1], outline=(60, 55, 45))

out = os.path.join(os.path.dirname(__file__), "..", "icon.png")
img.convert("RGB").save(os.path.normpath(out))
print("wrote", os.path.normpath(out))
