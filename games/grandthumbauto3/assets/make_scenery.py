#!/usr/bin/env python3
"""Park scenery — scenery.png, 5 cells of 20x20 (100x20).
Cells 0-1 are the ORIGINAL GrandThumbAuto tree canopies (recovered generator,
same seed + colours). New additions use the SAME soft dappled-canopy style:
  0 oak (original)  1 hedge-green (original)  2 autumn  3 bush  4 flowerbed
In-game: hash %8 -> {0,1,2:oak  3,4:hedge  5:autumn  6:bush  7:flowers} so the
classic trees stay dominant. Trees get trunk colliders; bush/flowers walk-through."""
import random
from PIL import Image, ImageDraw

TS = 20
random.seed(23)                     # SAME seed as the original tree.png
sheet = Image.new("RGBA", (TS*5, TS), (0, 0, 0, 0))
d = ImageDraw.Draw(sheet)

def canopy(ox, base, hi, lo):       # the ORIGINAL canopy routine, untouched
    d.ellipse([ox+2, 3, ox+TS-2, TS-1], fill=lo+(255,))          # shadow-side
    d.ellipse([ox+2, 2, ox+TS-3, TS-3], fill=base+(255,))        # canopy
    for _ in range(14):                                          # dappled highlights
        a = random.randint(ox+5, ox+TS-6); b = random.randint(4, TS-6)
        if (a-(ox+TS//2))**2 + (b-TS//2)**2 < (TS//2-3)**2:
            d.point([a, b], fill=hi+(255,))
    d.ellipse([ox+6, 4, ox+11, 9], fill=hi+(255,))               # top-left sheen

def small_canopy(ox, base, hi, lo, inset):   # same look, smaller footprint
    d.ellipse([ox+inset, inset+1, ox+TS-inset, TS-inset+1], fill=lo+(255,))
    d.ellipse([ox+inset, inset, ox+TS-inset-1, TS-inset-1], fill=base+(255,))
    for _ in range(8):
        a = random.randint(ox+inset+2, ox+TS-inset-3); b = random.randint(inset+2, TS-inset-3)
        if (a-(ox+TS//2))**2 + (b-TS//2)**2 < (TS//2-inset-1)**2:
            d.point([a, b], fill=hi+(255,))
    d.ellipse([ox+inset+2, inset+1, ox+inset+6, inset+5], fill=hi+(255,))

canopy(0,    (70,120,54), (104,158,74), (44,84,40))     # 0: ORIGINAL oak
canopy(TS,   (92,120,60), (128,158,86), (62,86,46))     # 1: ORIGINAL hedge-green
canopy(TS*2, (152,104,44),(190,142,62), (110,70,34))    # 2: autumn — same style, warm
small_canopy(TS*3, (78,116,56),(112,150,78),(52,82,42), 4)   # 3: bush — small soft canopy
# 4: flowerbed — a low green pad with dappled blossom colours (same visual language)
ox = TS*4
d.ellipse([ox+3, 4, ox+TS-3, TS-2], fill=(56,88,42,255))
d.ellipse([ox+3, 3, ox+TS-4, TS-4], fill=(74,108,50,255))
random.seed(51)
for col in [(214,96,116),(232,214,110),(228,232,232),(178,116,214)]:
    for _ in range(4):
        a = random.randint(ox+6, ox+TS-7); b = random.randint(6, TS-7)
        if (a-(ox+TS//2))**2 + (b-TS//2)**2 < (TS//2-4)**2:
            d.rectangle([a, b, a+1, b+1], fill=col+(255,))
d.ellipse([ox+6, 4, ox+10, 8], fill=(96,132,64,255))

sheet.save("/home/maustin/thumby-color/mote/games/grandthumbauto/assets/scenery.png")
print("wrote assets/scenery.png 100x20 (original trees + matching additions)")
