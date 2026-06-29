#!/usr/bin/env python3
"""Convert the original ThumbAtro Thumby BMP/WAV assets to Mote-bakeable PNGs.
The card sheet uses an in-engine colour key of RGB565 0x0400 (a dark green) for
transparency; we turn those pixels into alpha=0 so `mote bake` keys them. The
other images are fully opaque."""
import struct, os, sys
from PIL import Image
SRC = sys.argv[1]; OUT = sys.argv[2]

def load_bmp_rgb565(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w   = struct.unpack_from("<i", d, 18)[0]
    h   = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 16, "%s is %d bpp" % (path, bpp)
    topdown = h < 0; H = abs(h)
    row = ((w*2 + 3)//4)*4               # rows padded to 4 bytes
    px = []                              # list of (r5,g6,b5,raw16)
    for y in range(H):
        sy = y if topdown else (H-1-y)
        base = off + sy*row
        line = []
        for x in range(w):
            v = struct.unpack_from("<H", d, base + x*2)[0]
            line.append(v)
        px.append(line)
    return w, H, px

def rgb565_to_888(v):
    r = (v>>11)&0x1F; g=(v>>5)&0x3F; b=v&0x1F
    return (r*255//31, g*255//63, b*255//31)

def conv(name, key=None, out=None, size=None):
    w,h,px = load_bmp_rgb565(os.path.join(SRC, name))
    img = Image.new("RGBA", (w,h))
    pa = img.load()
    for y in range(h):
        for x in range(w):
            v = px[y][x]
            if key is not None and v == key:
                pa[x,y] = (0,0,0,0)
            else:
                r,g,b = rgb565_to_888(v)
                pa[x,y] = (r,g,b,255)
    if size: img = img.resize(size, Image.NEAREST)
    op = os.path.join(OUT, out)
    img.save(op)
    print("wrote %s (%dx%d)%s" % (op, img.width, img.height, " keyed" if key is not None else ""))

conv("BiggerCards2read.bmp", key=0x0400, out="cards.png")
conv("thumbatrobackground.bmp", out="background.png")
conv("title.bmp", out="title.png")
