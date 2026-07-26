#!/usr/bin/env python3
"""
Build docs/item-review.html -- a correction sheet for the item sprite mapping.

The item table in rl_item.c points each of the 64 kinds at a cell in a baked
subsheet. Those cells were chosen by eye from magnified contact sheets, and a
fair number are guesses: "iron helm" is whichever of eleven coloured helmets
looked most iron. This page exists so those guesses can be corrected without
anyone having to read C.

Two halves:

  1. Every item as it currently appears, grouped by category, with the SOURCE
     coordinate it came from -- (col,row) in source_tileset.png, which is the
     only coordinate system worth quoting back.
  2. Every sheet the item table draws from, at 6x, with each cell labelled by
     its source coordinate, so a replacement can be named precisely.

Corrections come back as "leather cap -> (38,19)" and land in ITEM_FIX below,
which apply_item_fix.py folds into rl_item.c.

Usage:  python3 authoring/gen_item_review.py
"""
import base64
import io
import os
import re

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC = os.path.join(GAME, "src")
SHEETS = os.path.join(GAME, "assets", "sheets")
OUT = os.path.join(GAME, "docs", "item-review.html")
TS = 8

# Where each subsheet was cut from source_tileset.png. Must match the
# SPRITE_SHEETS table in extract.py -- this is what turns a sheet cell back
# into a source coordinate.
ORIGIN = {
    "weapons_potions":   (16, 0),
    "tools_wands":       (16, 28),
    "weapons_elemental": (16, 32),
    "treasure_ore":      (16, 16),
    "crowns_fx":         (32, 24),
    "food":              (16, 8),
    "runes":             (8, 1),
    "armour_set":        (16, 21),
    "trinkets":          (0, 8),
    "jewellery":         (25, 23),
    "ammo":              (25, 0),
    "devices":           (24, 5),
    "chest_wood":        (16, 5),
    "levers":            (2, 1),
    "characters":        (32, 0),
}
MACRO = {"W": "weapons_potions", "T": "tools_wands", "E": "weapons_elemental",
         "O": "treasure_ore", "G": "crowns_fx", "F": "food", "R": "runes",
         "L": "armour_set", "K": "trinkets", "J": "jewellery", "A": "ammo", "D": "devices",
              "V": "treasure_ore", "P": "characters"}
TV_LABEL = {"WEAPON": "Weapons", "ARMOUR": "Armour", "POTION": "Potions",
            "SCROLL": "Scrolls", "WAND": "Wands", "RING": "Rings",
            "FOOD": "Food", "LIGHT": "Light"}
TV_ORDER = ["ARMOUR", "WEAPON", "RING", "LIGHT", "WAND", "POTION", "SCROLL", "FOOD"]


def b64(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


sheets = {}
for n in MACRO.values():
    im = Image.open(os.path.join(SHEETS, n + ".png")).convert("RGBA")
    sheets[n] = dict(img=im, w=im.width, h=im.height, cols=im.width // TS,
                     uri=b64(im))

item_c = open(os.path.join(SRC, "rl_item.c"), encoding="utf-8").read()
body = item_c[item_c.index("g_item_kind[]"):item_c.index("};", item_c.index("g_item_kind[]"))]
body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)

ITEM_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*([WTEOGFRLKJADVP])\((\d+)\)\s*,\s*TV_(\w+)\s*,'
    r"\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*EF_(\w+)\s*\}"
)
items = []
for i, m in enumerate(ITEM_RE.finditer(body)):
    name, mac, cell, tv, lvl, cost, d, s_, ac, eff = m.groups()
    sheet = MACRO[mac]
    cell = int(cell)
    cols = sheets[sheet]["cols"]
    ox, oy = ORIGIN[sheet]
    items.append(dict(idx=i, name=name, macro=mac, sheet=sheet, cell=cell,
                      tv=tv, eff=eff, lvl=int(lvl), cost=int(cost),
                      dice=(int(d), int(s_)), ac=int(ac),
                      src=(ox + cell % cols, oy + cell // cols)))
# The item table's length is pinned to enum ItemId by a _Static_assert in
# rl_item.c, so read the expected count from the enum rather than freezing a
# number here -- the table grows every time a mapping correction lands.
_h = open(os.path.join(SRC, "rl.h"), encoding="utf-8").read()
_enum = _h[_h.index("enum ItemId {"):]
_enum = re.sub(r"/\*.*?\*/", " ", _enum[:_enum.index("};")], flags=re.S)
ITM_N = len(set(re.findall(r"ITM_[A-Z0-9_]+", _enum))) - 1  # less ITM_N itself
assert len(items) == ITM_N, "enum has %d item kinds, parsed %d" % (ITM_N, len(items))
print("[review] %d items across %d sheets" % (len(items), len(sheets)))


def spr(sheet, cell, scale):
    s = sheets[sheet]
    return ('<i class="spr sh-{sh}" style="--s:{sc};--x:{x};--y:{y}"></i>'
            .format(sh=sheet, sc=scale, x=cell % s["cols"], y=cell // s["cols"]))


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# --- half one: what each item currently uses -----------------------------
groups = []
for tv in TV_ORDER:
    rows = []
    for it in [x for x in items if x["tv"] == tv]:
        detail = ("%dd%d" % it["dice"] if tv == "WEAPON"
                  else "+%d ac" % it["ac"] if tv == "ARMOUR"
                  else "radius %d" % it["ac"] if tv == "LIGHT"
                  else it["eff"].lower().replace("_", " "))
        rows.append("""
      <li>
        <div class="art">%s</div>
        <div class="meta">
          <b>%s</b>
          <span class="det">%s</span>
          <code>%s(%d)</code>
          <code class="src">(%d,%d)</code>
        </div>
      </li>""" % (spr(it["sheet"], it["cell"], 6), esc(it["name"]), esc(detail),
                  it["macro"], it["cell"], it["src"][0], it["src"][1]))
    groups.append('<section><h2>%s</h2><ul class="items">%s</ul></section>'
                  % (TV_LABEL[tv], "".join(rows)))

# --- half two: every sheet, every cell, labelled by source coordinate ----
plates = []
for name in sorted(sheets):
    s = sheets[name]
    px = s["img"].load()
    ox, oy = ORIGIN[name]
    used = {it["cell"] for it in items if it["sheet"] == name}
    cells = []
    for r in range(s["h"] // TS):
        for c in range(s["cols"]):
            cell = r * s["cols"] + c
            blank = all(px[c * TS + x, r * TS + y][3] == 0
                        for y in range(TS) for x in range(TS))
            if blank:
                cells.append('<div class="cell blank"></div>')
                continue
            cells.append(
                '<div class="cell%s">%s<span>%d,%d</span></div>'
                % (" used" if cell in used else "", spr(name, cell, 5),
                   ox + c, oy + r))
    plates.append("""
  <section class="plate">
    <h2>%s <span class="dim">source (%d,%d) &middot; %d&#215;%d cells</span></h2>
    <div class="grid" style="--cols:%d">%s</div>
  </section>""" % (name, ox, oy, s["cols"], s["h"] // TS, s["cols"], "".join(cells)))

SHEET_CSS = "\n".join(
    ".sh-%s{background-image:url(%s);--sw:%d;--sh:%d}" % (n, s["uri"], s["w"], s["h"])
    for n, s in sheets.items())

HTML = """<title>Roguemote &mdash; item sprite review</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{
  color-scheme:light dark;
  --ground:#0a0910; --panel:#14121e; --raised:#221e33; --rule:#3e3856;
  --ink:#e2ded2; --mute:#8b85a6; --gold:#ffc850; --used:#00e436;
}
@media (prefers-color-scheme:light){
  :root{--ground:#e3e1e8; --panel:#faf9fc; --raised:#eeecf3; --rule:#bfbbcb;
        --ink:#16141f; --mute:#57536b; --gold:#8a5c00; --used:#007a3d}
}
:root[data-theme="dark"]{--ground:#0a0910; --panel:#14121e; --raised:#221e33;
  --rule:#3e3856; --ink:#e2ded2; --mute:#8b85a6; --gold:#ffc850; --used:#00e436}
:root[data-theme="light"]{--ground:#e3e1e8; --panel:#faf9fc; --raised:#eeecf3;
  --rule:#bfbbcb; --ink:#16141f; --mute:#57536b; --gold:#8a5c00; --used:#007a3d}

*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);
  font:400 16px/1.6 "Iowan Old Style","Palatino Linotype",Palatino,Georgia,serif}
code,.src,.cell span{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;
  font-variant-numeric:tabular-nums}
.wrap{max-width:64rem;margin:0 auto;padding:0 1rem 5rem}
header{border-bottom:1px solid var(--rule);background:var(--panel);
  margin-bottom:2rem;padding:2rem 0}
h1{margin:0 0 .6rem;font-size:1.9rem;color:var(--gold);text-wrap:balance}
header p{margin:0;max-width:36rem;color:var(--mute)}
h2{font-size:1.05rem;margin:2.25rem 0 .75rem;color:var(--gold);
  border-bottom:1px solid var(--rule);padding-bottom:.4rem}
h2 .dim{color:var(--mute);font-weight:400;font-size:.72rem;letter-spacing:.06em}
.lede{max-width:36rem}

.spr{display:inline-block;vertical-align:middle;
  width:calc(8px * var(--s));height:calc(8px * var(--s));
  background-repeat:no-repeat;
  background-size:calc(var(--sw) * var(--s) * 1px) calc(var(--sh) * var(--s) * 1px);
  background-position:calc(var(--x) * -8px * var(--s)) calc(var(--y) * -8px * var(--s));
  image-rendering:pixelated;image-rendering:crisp-edges;flex:none}

ul.items{list-style:none;margin:0;padding:0;
  display:grid;grid-template-columns:repeat(auto-fill,minmax(14rem,1fr));
  background:var(--panel);border:1px solid var(--rule);border-width:1px 0 0 1px}
ul.items li{display:flex;gap:.7rem;align-items:center;padding:.6rem .7rem;
  border:0 solid var(--rule);border-width:0 1px 1px 0}
.art{width:48px;height:48px;display:grid;place-items:center;background:#000;
  border:1px solid var(--rule);flex:none}
.meta{display:flex;flex-direction:column;gap:.05rem;min-width:0}
.meta b{font-weight:400;font-size:.92rem}
.det{font-size:.76rem;color:var(--mute)}
.meta code{font-size:.68rem;color:var(--mute)}
.meta code.src{color:var(--gold)}

.plate .grid{display:grid;grid-template-columns:repeat(var(--cols),1fr);
  background:var(--panel);border:1px solid var(--rule);border-width:1px 0 0 1px;
  overflow-x:auto}
.cell{border:0 solid var(--rule);border-width:0 1px 1px 0;padding:.25rem;
  display:flex;flex-direction:column;align-items:center;gap:.15rem;
  min-width:0;background:var(--panel)}
.cell.blank{background:repeating-linear-gradient(45deg,transparent 0 4px,
  color-mix(in srgb,var(--rule) 40%%, transparent) 4px 5px)}
.cell.used{background:var(--raised);box-shadow:inset 0 0 0 1px var(--used)}
.cell span{font-size:.58rem;color:var(--mute);white-space:nowrap}
.cell.used span{color:var(--used)}
.key{display:flex;gap:1.25rem;flex-wrap:wrap;margin:.75rem 0 0;
  font-size:.78rem;color:var(--mute)}
.key i{display:inline-block;width:.8rem;height:.8rem;vertical-align:-1px;
  margin-right:.35rem;border:1px solid var(--rule)}
.key .u{background:var(--raised);box-shadow:inset 0 0 0 1px var(--used)}
.key .b{background:repeating-linear-gradient(45deg,transparent 0 4px,
  color-mix(in srgb,var(--rule) 40%%, transparent) 4px 5px)}
:focus-visible{outline:2px solid var(--gold);outline-offset:2px}
</style>

<header><div class="wrap">
  <h1>Item sprite review</h1>
  <p class="lede">Every item kind and the cell it currently draws. The gold
    coordinate is the position in <code>source_tileset.png</code> &mdash; quote
    that back to change one. Second half is every sheet the item table draws
    from, with each cell labelled and the ones already in use ringed in green.</p>
  <p class="lede" style="margin-top:.6rem">Corrections in the form
    <code>iron helm &rarr; (38,19)</code>; anything not listed stays as it is.</p>
</div></header>

<div class="wrap">
%(groups)s

<h2 style="margin-top:3rem">The sheets</h2>
<p class="lede">Cells ringed in green are already referenced by an item.
  Hatched cells are empty in the source.</p>
<div class="key">
  <span><i class="u"></i>in use</span>
  <span><i class="b"></i>empty</span>
</div>
%(plates)s
</div>
<style>%(sheets)s</style>
"""

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(HTML % dict(groups="\n".join(groups), plates="\n".join(plates),
                        sheets=SHEET_CSS))
print("[review] %s  %.0f KB" % (OUT, os.path.getsize(OUT) / 1024))
