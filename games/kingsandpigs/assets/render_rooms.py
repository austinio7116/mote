#!/usr/bin/env python3
"""Render the room catalogue (src/kp_rooms.h) as a visual gallery: real
tilesets, windows with alpha-blended light shafts, doors/banners/crates/bombs,
loot and enemy sprites at their rest positions. For eyeballing playability
and layout after editing rooms.

Usage: python3 assets/render_rooms.py [out.png] [group]
"""
import os
import re
import sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
W, H, TS = 16, 8, 32
NB = [(0, -1, 1), (1, -1, 2), (1, 0, 4), (1, 1, 8),
      (0, 1, 16), (-1, 1, 32), (-1, 0, 64), (-1, -1, 128)]

def load_tileset(name):
    conf = {"lut": [0] * 256, "edge": 0, "sheet": None}
    for line in open(os.path.join(GAME, "tilesets", name + ".tileset")):
        t = line.split()
        if not t: continue
        if t[0] == "sheet": conf["sheet"] = Image.open(os.path.join(GAME, t[1])).convert("RGBA")
        elif t[0] == "edge": conf["edge"] = int(t[1])
        elif t[0] == "lut": conf["lut"] = [int(x) for x in t[1:]]
    return conf

def sprite(path, crop=None):
    im = Image.open(os.path.join(GAME, path)).convert("RGBA")
    if crop: im = im.crop(crop)
    bb = im.getchannel("A").getbbox()
    return im.crop(bb) if bb else im

def feet(rows, r, c):
    for rr in range(r + 1, H):
        if rows[rr][c] in "#=-S":
            return rr
    return H - 1

def render_room(rows, ts, spr, meta):
    bits = [[0] * W for _ in range(H)]
    for r in range(H):
        for c in range(W):
            ch = rows[r][c]
            bits[r][c] = (8 if ch == "#" else
                          1 | 2 if ch == "=" else
                          1 | 4 if ch in "-S" else
                          0 if ch == "O" else 1)
    out = Image.new("RGBA", (W * TS, H * TS), (57, 49, 75, 255))
    for lname, bit in (("bgwall", 1), ("platthin", 2), ("platthick", 4), ("solidt", 8)):
        t = ts[lname]
        tpr = t["sheet"].size[0] // TS
        for r in range(H):
            for c in range(W):
                if not (bits[r][c] & bit): continue
                m = 0
                for dx, dy, b in NB:
                    rr, cc = r + dy, c + dx
                    same = t["edge"] if not (0 <= rr < H and 0 <= cc < W) \
                        else (bits[rr][cc] & bit) != 0
                    if same: m |= b
                cell = t["lut"][m]
                fx, fy = (cell % tpr) * TS, (cell // tpr) * TS
                out.alpha_composite(t["sheet"].crop((fx, fy, fx + TS, fy + TS)), (c * TS, r * TS))
    # deco + entities at their game positions
    for r in range(H):
        for c in range(W):
            ch = rows[r][c]
            x, y = c * TS, r * TS
            if ch == "W":
                out.alpha_composite(spr["window"], (x - 1, y + 4))
                # additive premultiplied shaft, exactly like the game's blend
                import numpy as np
                ray = np.array(spr["wray"])
                ox, oy = x - 1 + meta["rdx"], y + 4 + meta["rdy"]
                base = np.array(out)
                h2, w2 = ray.shape[0], ray.shape[1]
                oy2, ox2 = min(oy + h2, base.shape[0]), min(ox + w2, base.shape[1])
                if oy < base.shape[0] and ox < base.shape[1]:
                    reg = base[oy:oy2, ox:ox2, :3].astype(int)
                    add = ray[:oy2 - oy, :ox2 - ox, :3].astype(int)
                    base[oy:oy2, ox:ox2, :3] = np.clip(reg + add, 0, 255).astype('uint8')
                    out.paste(Image.fromarray(base))
            elif ch == "F":
                out.alpha_composite(spr["banner"], (x + 1, y + 2))
            elif ch in "exOdDhBbEC":
                fr = feet(rows, r, c) if ch != "O" else None
                if ch == "O": continue
                fy = fr * TS
                def ground(im, cx):
                    out.alpha_composite(im, (cx - im.size[0] // 2, fy - im.size[1]))
                cx = x + TS // 2
                if ch in "ex": ground(spr["door"], cx)
                elif ch == "d": ground(spr["gem"], cx)
                elif ch == "D":
                    ground(spr["gem"], cx - 10); ground(spr["gem"], cx + 10)
                    out.alpha_composite(spr["gem"], (cx - spr["gem"].size[0] // 2, fy - spr["gem"].size[1] - 8))
                elif ch == "h": ground(spr["heart"], cx)
                elif ch == "B": ground(spr["crate"], cx)
                elif ch == "b": ground(spr["bomb"], cx)
                elif ch == "E": ground(spr["pig"], cx)
                elif ch == "C": ground(spr["cannon"], cx)
    return out

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/kp_rooms_gallery.png"
    only = sys.argv[2] if len(sys.argv) > 2 else None
    txt = open(os.path.join(GAME, "src", "kp_rooms.h")).read()
    ts = {n: load_tileset(n) for n in ("bgwall", "platthin", "platthick", "solidt")}
    meta = {}
    m = open(os.path.join(GAME, "src", "kp_meta.h")).read()
    meta["rdx"] = int(re.search(r"KP_WRAY_DX (\-?\d+)", m).group(1))
    meta["rdy"] = int(re.search(r"KP_WRAY_DY (\-?\d+)", m).group(1))
    spr = {
        "window": sprite("assets/window.png"),
        "wray": Image.open(os.path.join(GAME, "assets/wray.png")).convert("RGBA"),
        "banner": sprite("assets/banner1.png"),
        "door": sprite("anims/door.png", (0, 0, 48, 58)),
        "gem": sprite("anims/pickups.png", (0, 0, 24, 14)),
        "heart": sprite("anims/pickups.png", (48, 14, 72, 28)),
        "crate": sprite("anims/box.png", (0, 0, 24, 18)),
        "bomb": sprite("anims/bomb.png", (0, 0, 24, 22)),
        "pig": sprite("anims/pig.png", (0, 0, 40, 30)),
        "cannon": sprite("anims/cannon.png", (0, 0, 56, 30)),
    }
    rooms = []
    for gname, body in re.findall(r"kp_(\w+)\[\] = \{(.*?)\n\};", txt, re.S):
        if only and gname != only: continue
        for i, room in enumerate(re.findall(r"\{\{(.*?)\}\}", body, re.S)):
            rooms.append((f"{gname}[{i}]", re.findall(r'"([^"]*)"', room)))
    COLSN = 3
    cw, chh = W * TS + 8, H * TS + 22
    sheet = Image.new("RGB", (COLSN * cw, ((len(rooms) + COLSN - 1) // COLSN) * chh + 4), (12, 12, 16))
    from PIL import ImageDraw
    d = ImageDraw.Draw(sheet)
    for i, (label, rows) in enumerate(rooms):
        x, y = (i % COLSN) * cw + 4, (i // COLSN) * chh + 4
        d.text((x, y), label, fill=(220, 210, 160))
        sheet.paste(render_room(rows, ts, spr, meta).convert("RGB"), (x, y + 14))
    sheet.save(out_path)
    print("wrote", out_path, f"({len(rooms)} rooms)")

if __name__ == "__main__":
    main()
