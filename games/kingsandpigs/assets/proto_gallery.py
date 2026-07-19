#!/usr/bin/env python3
"""Render a piece library as a labelled gallery so we can eyeball each chamber
with the real tiles. Ports are shown as wall (enclosed room view).

Usage: python3 assets/proto_gallery.py <pieces_module> [out.png]
   <pieces_module> is an importable .py exposing PIECES = {name:{rows:...}}
"""
import os, sys, importlib.util
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proto_layout as PL
import proto_check as C
TS = 32

def render_piece(rows):
    # ceiling/door holes read as open; a floor drop gets a THIN PLANK across it
    # at floor level (walk over or drop through)
    cv = [list(r.replace('^','.').replace('<','.').replace('>','.').replace('v','=')) for r in rows]
    # pad a 1-tile void margin so the outer wall autotiles against void
    h, w = len(cv), len(cv[0])
    pad = [[' ']*(w+2) for _ in range(h+2)]
    for r in range(h):
        for c in range(w): pad[r+1][c+1] = cv[r][c]
    return PL.render(pad)

def main():
    mod_path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/kp_pieces_gallery.png"
    spec = importlib.util.spec_from_file_location("pl", mod_path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    pieces = m.PIECES
    imgs = []
    for name, d in pieces.items():
        errs = C.check_piece(d["rows"])
        imgs.append((name, render_piece(d["rows"]), not errs))
    cols = 4
    cw = max(im.size[0] for _, im, _ in imgs) + 16
    ch = max(im.size[1] for _, im, _ in imgs) + 30
    rows_n = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols*cw, rows_n*ch), (14, 12, 20))
    d = ImageDraw.Draw(sheet)
    for i, (name, im, ok) in enumerate(imgs):
        x, y = (i % cols)*cw + 8, (i//cols)*ch + 4
        d.text((x, y), name + ("" if ok else "  !FAIL"),
               fill=(150, 230, 150) if ok else (255, 120, 120))
        sheet.paste(im.convert("RGB"), (x, y + 14))
    sheet.save(out)
    print("wrote", out, f"({len(imgs)} pieces, {sum(ok for _,_,ok in imgs)} pass)")

if __name__ == "__main__":
    main()
