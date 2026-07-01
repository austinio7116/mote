#!/usr/bin/env python3
"""Convert the original Thumbalaga Thumby BMP/WAV assets into Mote-bakeable
EDITABLE SOURCE files under games/thumbalaga/assets/.

  - Sprite BMPs (RGB565, magenta 0xF81F = transparent) -> PNG with alpha=0 on
    the key, so `mote bake` re-keys them (Pixel Art tab editable).
  - logo.bmp (opaque) -> logo.png + the launcher icon.png (game root).
  - WAVs are copied verbatim (Audio tab editable, baked to MoteSound PCM).

Run:  python3 tools/convert_assets.py <src Thumbalaga dir>
"""
import struct, os, sys, shutil
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
OUT  = os.path.join(GAME, "assets")
SRC  = sys.argv[1] if len(sys.argv) > 1 else \
       os.path.expanduser("~/thumby-color/TinyCircuits-Thumby-Color-Games/Thumbalaga")
SRCA = os.path.join(SRC, "assets")
KEY  = 0xF81F  # magenta colour-key used by the original sprites

def load_bmp_rgb565(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w   = struct.unpack_from("<i", d, 18)[0]
    h   = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 16, "%s is %d bpp" % (path, bpp)
    topdown = h < 0; H = abs(h)
    row = ((w * 2 + 3) // 4) * 4
    px = []
    for y in range(H):
        sy = y if topdown else (H - 1 - y)
        base = off + sy * row
        px.append([struct.unpack_from("<H", d, base + x * 2)[0] for x in range(w)])
    return w, H, px

def rgb565_to_888(v):
    r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)

def conv(name, keyed=True, out=None):
    w, h, px = load_bmp_rgb565(os.path.join(SRCA, name))
    img = Image.new("RGBA", (w, h)); pa = img.load()
    for y in range(h):
        for x in range(w):
            v = px[y][x]
            if keyed and v == KEY:
                pa[x, y] = (0, 0, 0, 0)
            else:
                r, g, b = rgb565_to_888(v)
                pa[x, y] = (r, g, b, 255)
    op = os.path.join(OUT, out or (os.path.splitext(name)[0] + ".png"))
    img.save(op)
    print("wrote %s (%dx%d)%s" % (op, w, h, " keyed" if keyed else ""))
    return img

os.makedirs(OUT, exist_ok=True)

# Keyed sprite sheets (magenta -> alpha).
for n in ("player.bmp", "player_captured.bmp", "enemies.bmp", "explosion.bmp",
          "player_explosion.bmp", "life_icon.bmp", "badge_narrow.bmp",
          "badge_shields.bmp"):
    conv(n, keyed=True)

# Opaque logo + launcher icon.
logo = conv("logo.bmp", keyed=False, out="logo.png")
icon = logo.convert("RGB").resize((64, 64), Image.LANCZOS)
icon.save(os.path.join(GAME, "icon.png"))
print("wrote", os.path.join(GAME, "icon.png"), "(64x64)")

# Copy the editable WAVs verbatim — `mote bake` (wav2snd) converts any PCM WAV
# (8/16-bit, any rate) to the engine's 16-bit/22050/mono MoteSound format.
for w in sorted(os.listdir(SRCA)):
    if w.endswith(".wav"):
        shutil.copy2(os.path.join(SRCA, w), os.path.join(OUT, w))
        print("copied", w)

print("\n=== DONE ===")
