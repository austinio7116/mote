#!/usr/bin/env python3
"""
Sprite coverage: how much of source_tileset.png the game actually draws.

The standing goal is that every populated tile in the sheet is referenced by
something. This walks every consumer there is and reports what is left:

  content tables   monsters, bosses, items, classes      (via inuse.py)
  terrain bands    the blob47 / edge16 rulesets          (via gen_terrain.py)
  floor fills      cobble, road, grass, grass tufts      (via extract.py)
  draw sites       the SPR_* features, decor, chests,
                   shopfronts and the FX strips          (declared below)

A cell counts as POPULATED if it has any pixel that is neither transparent nor
opaque black -- the source is paletted with index 0 as the background, so
converting to RGBA turns "empty" into black rather than into alpha.

Writes docs/coverage.html: the whole 64x64 grid, every tile drawn at 3x and
tinted by what claims it, so the unclaimed blocks are visible as blocks.

Usage:  python3 authoring/coverage.py
"""
import base64
import io
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import inuse                                            # noqa: E402

GAME = os.path.dirname(HERE)
OUT = os.path.join(GAME, "docs", "coverage.html")
TS = 8

# --- draw sites that reference art directly, not through a content table -----
#
# These are hardcoded at the point of drawing, so nothing can discover them by
# parsing a table. Each entry is (subsheet, [cells]) and is resolved to source
# coordinates through the same geometry inuse.py uses.
DRAW_SITES = {
    # rl_draw.c SPR_* -- the feature sprites
    "stairs":             [0, 1, 2],           # down, up, the gold mine mouth
    "doors_gems_banners": [20, 21,             # the grey door, closed and open
                           14, 22, 6, 10, 18, 26],   # the six shopfronts
    "props_light":        [31, 32],            # cottage, stone tower
    # trinkets: the inn's bed, the rubble, and the floor decor table
    "trinkets":           [16, 69, 66, 67, 48, 55],
    # every chest tier, closed and open -- rl_chest_cell covers the sheet
    "chests":             list(range(10)),
    # rl_magic.c animates six-frame strips from these starts
    "fx_mono":            ([0, 1, 2, 3, 4, 5] + [6, 7, 8, 9, 10, 11] +
                           [38, 39, 40, 41, 42, 43] + [54, 55, 56, 57, 58, 59] +
                           [92, 93, 94, 95] + [106, 107, 108, 109, 110, 111]),
}


def populated(img, c, r):
    t = img.crop((c * TS, r * TS, c * TS + TS, r * TS + TS))
    return any(p[3] and p[:3] != (0, 0, 0) for p in t.get_flattened_data())


def band_cells(c0, r0, kind):
    """The source cells a blob47 (16x3) or edge16 (6x3) ruleset reads."""
    w, h = (16, 3) if kind == "blob47" else (6, 3)
    return {(c0 + c, r0 + r) for r in range(h) for c in range(w)}


def parse_terrains():
    """gen_terrain.py's TERRAINS -> the source cells each band consumes."""
    src = open(os.path.join(HERE, "gen_terrain.py"), encoding="utf-8").read()
    body = src[src.index("TERRAINS = {"):src.index("\n}", src.index("TERRAINS = {"))]
    out = {}
    for m in re.finditer(r'"(\w+)":\s*dict\(at=\((\d+),\s*(\d+)\)(.*?)\)', body, re.S):
        name, c0, r0, rest = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        kind = "edge16" if 'kind="edge16"' in rest else "blob47"
        out[name] = band_cells(c0, r0, kind)
    return out


def parse_fills():
    """extract.py's floor fills and the tinted grass tufts."""
    src = open(os.path.join(HERE, "extract.py"), encoding="utf-8").read()
    cells = set()
    for name in ("FLOOR_COBBLE", "FLOOR_ROAD", "FLOOR_GRASS", "FLOOR_JUNGLE"):
        m = re.search(name + r"\s*=\s*(\[.*?\])\n", src, re.S)
        if m:
            for c, r in re.findall(r"\((\d+),\s*(\d+)\)", m.group(1)):
                cells.add((int(c), int(r)))
    m = re.search(r"GRASS_TUFT_CELLS\s*=\s*\((.*?)\)\n", src, re.S)
    if m:
        for c, r in re.findall(r"\((\d+),\s*(\d+)\)", m.group(1)):
            cells.add((int(c), int(r)))
    return cells


def b64(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


src = Image.open(os.path.join(HERE, "source_tileset.png")).convert("RGBA")
GW, GH = src.width // TS, src.height // TS

pop = {(c, r) for r in range(GH) for c in range(GW) if populated(src, c, r)}

# --- who claims what --------------------------------------------------------
claim = {}                                   # (c,r) -> reason


def claim_all(cells, why):
    for cell in cells:
        claim.setdefault(cell, why)


for cell, uses in inuse.build().items():
    kind = uses[0].split(":")[0].split("(")[0]
    claim_all([cell], "table/" + kind)

for name, cells in parse_terrains().items():
    claim_all(cells, "terrain")

claim_all(parse_fills(), "fill")

# The rogue8 font. extract.py bakes codepoints 0-127 out of cols 48-63, rows
# 0-7, one glyph a cell -- 128 tiles that are drawn on every screen in the game
# and were showing as unclaimed because no content table mentions them.
claim_all({(48 + i % 16, i // 16) for i in range(128)}, "font")

for sheet, cells in DRAW_SITES.items():
    for c in cells:
        try:
            claim_all([inuse.src_coord(sheet, c)], "draw site")
        except (KeyError, IndexError):
            pass

used = pop & set(claim)
unused = sorted(pop - set(claim))

# --- terminal summary -------------------------------------------------------
print("source grid %dx%d  |  %d populated of %d" % (GW, GH, len(pop), GW * GH))
print("  claimed   %5d  (%d%%)" % (len(used), 100 * len(used) // max(1, len(pop))))
print("  unclaimed %5d" % len(unused))
print()
by = {}
for cell in used:
    by[claim[cell]] = by.get(claim[cell], 0) + 1
for k in sorted(by, key=lambda k: -by[k]):
    print("    %-18s %4d" % (k, by[k]))

# Where the unclaimed art is, and -- from the labelling pass -- WHAT it is,
# because "312 tiles in cols 0-15" is not an answer anybody can act on.
LABELS = os.path.join(HERE, "labels_export_2026-07-26.json")
label = {}
if os.path.exists(LABELS):
    import json
    raw = json.load(open(LABELS, encoding="utf-8"))
    for t in (raw if isinstance(raw, list) else raw.get("tiles", [])):
        if t.get("name"):
            label[(t["c"], t["r"])] = t.get("cat") or "?"

print("\nunclaimed, by region -- and what the labelling pass called it:")
blocks = {}
for c, r in unused:
    blocks.setdefault((c // 16 * 16, r // 8 * 8), []).append((c, r))
region_rows = []
for key in sorted(blocks, key=lambda k: -len(blocks[k])):
    bc, br = key
    cells = blocks[key]
    kinds = {}
    for cell in cells:
        k = label.get(cell, "unlabelled")
        kinds[k] = kinds.get(k, 0) + 1
    what = ", ".join("%s %d" % (k, v)
                     for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:3])
    region_rows.append((bc, br, len(cells), what))
for bc, br, n, what in region_rows[:12]:
    print("    cols %2d-%2d rows %2d-%2d : %4d   %s" % (bc, bc + 15, br, br + 7, n, what))

# --- the picture ------------------------------------------------------------
SCALE = 3
rows = []
for r in range(GH):
    cells = []
    for c in range(GW):
        if (c, r) not in pop:
            cells.append('<i class="e"></i>')
            continue
        t = src.crop((c * TS, r * TS, c * TS + TS, r * TS + TS))
        why = claim.get((c, r))
        cls = "u" if why else "x"
        cells.append('<i class="%s" title="%d,%d %s"><img src="%s" alt=""></i>'
                     % (cls, c, r, esc(why or "unclaimed"), b64(t)))
    rows.append('<div class="row"><b>%d</b>%s</div>' % (r, "".join(cells)))

legend = "".join(
    '<li><i class="sw %s"></i>%s</li>' % (k, v) for k, v in
    (("u", "drawn by the game"), ("x", "populated, nothing draws it"),
     ("e", "empty cell")))

HTML = """<title>Roguemote &mdash; sprite coverage</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{
  color-scheme:light dark;
  --ground:#0a0910; --panel:#14121e; --rule:#3e3856; --ink:#e2ded2;
  --mute:#8b85a6; --gold:#ffc850; --good:#00e436; --warn:#ff77a8;
}
@media (prefers-color-scheme:light){:root{
  --ground:#e9e7ee; --panel:#fbfafd; --rule:#c2bed0; --ink:#16141f;
  --mute:#56526a; --gold:#8a5c00; --good:#00733a; --warn:#a3306a}}
:root[data-theme=dark]{--ground:#0a0910;--panel:#14121e;--rule:#3e3856;
  --ink:#e2ded2;--mute:#8b85a6;--gold:#ffc850;--good:#00e436;--warn:#ff77a8}
:root[data-theme=light]{--ground:#e9e7ee;--panel:#fbfafd;--rule:#c2bed0;
  --ink:#16141f;--mute:#56526a;--gold:#8a5c00;--good:#00733a;--warn:#a3306a}
*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);line-height:1.5;
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:1120px;margin:0 auto;padding:26px 16px 70px}
h1{font-size:clamp(21px,4vw,29px);margin:0 0 6px}
h1 b{color:var(--gold);font-weight:inherit}
.lede{color:var(--mute);max-width:64ch;margin:0 0 22px}
.stats{display:flex;flex-wrap:wrap;gap:10px;margin:0 0 20px}
.stat{background:var(--panel);border:1px solid var(--rule);border-radius:9px;
  padding:9px 14px;min-width:120px}
.stat b{display:block;font-size:22px;font-variant-numeric:tabular-nums}
.stat span{font-size:12px;color:var(--mute)}
ul.key{list-style:none;display:flex;flex-wrap:wrap;gap:16px;padding:0;margin:0 0 16px;
  font-size:13px;color:var(--mute)}
ul.key li{display:flex;align-items:center;gap:6px}
i.sw{width:13px;height:13px;border-radius:3px;display:inline-block}
.scroll{overflow-x:auto;background:var(--panel);border:1px solid var(--rule);
  border-radius:10px;padding:8px}
.grid{display:inline-block;font-size:0}
.row{white-space:nowrap;height:%(px)dpx}
.row b{display:inline-block;width:26px;font-size:9px;color:var(--mute);
  text-align:right;padding-right:4px;vertical-align:top;font-family:ui-monospace,monospace}
.grid i{display:inline-block;width:%(px)dpx;height:%(px)dpx;vertical-align:top}
.grid i img{width:100%%;height:100%%;image-rendering:pixelated;display:block}
i.u{outline:1px solid transparent}
i.x{background:color-mix(in srgb,var(--warn) 42%%, transparent);
  outline:1px solid var(--warn);outline-offset:-1px}
i.e{background:transparent}
i.sw.u{background:var(--good)} i.sw.x{background:var(--warn)}
i.sw.e{background:var(--rule)}
table{border-collapse:collapse;margin:14px 0 0;font-size:13px}
th,td{text-align:left;padding:5px 14px 5px 0;border-bottom:1px solid var(--rule)}
td.n{text-align:right;font-variant-numeric:tabular-nums;color:var(--gold)}
.note{background:var(--panel);border:1px solid var(--rule);border-left:3px solid var(--gold);
  border-radius:9px;padding:12px 14px;margin:20px 0 0;color:var(--mute);font-size:13.5px}
</style>
<div class="wrap">
<h1>Sprite coverage, <b>%(pct)d%% of the sheet</b></h1>
<p class="lede">Every populated tile in <code>source_tileset.png</code>, and
whether anything in the game draws it. Pink cells are art that exists and is
never shown. Hover a tile for its coordinate and what claims it.</p>

<div class="stats">
  <div class="stat"><b>%(pop)d</b><span>populated tiles</span></div>
  <div class="stat"><b>%(used)d</b><span>drawn by the game</span></div>
  <div class="stat"><b>%(unused)d</b><span>never drawn</span></div>
  <div class="stat"><b>%(pct)d%%</b><span>coverage</span></div>
</div>

<ul class="key">%(legend)s</ul>
<div class="scroll"><div class="grid">%(rows)s</div></div>

<h2 style="font-size:16px;margin:28px 0 0">What claims what</h2>
<table><tbody>%(by)s</tbody></table>

<h2 style="font-size:16px;margin:28px 0 0">Where the unclaimed art is</h2>
<table><thead><tr><th>region</th><th>tiles</th><th>what it is</th></tr></thead><tbody>%(blocks)s</tbody></table>

<div class="note">Regenerate with <code>python3 authoring/coverage.py</code>.
A tile counts as populated if it has any pixel that is neither transparent nor
opaque black &mdash; the source is paletted with index 0 as the background, so a
plain RGBA conversion turns every empty cell into black.</div>
</div>
""" % dict(
    px=TS * SCALE,
    pop=len(pop), used=len(used), unused=len(unused),
    pct=100 * len(used) // max(1, len(pop)),
    legend=legend, rows="".join(rows),
    by="".join('<tr><td>%s</td><td class="n">%d</td></tr>' % (esc(k), by[k])
               for k in sorted(by, key=lambda k: -by[k])),
    blocks="".join(
        '<tr><td>cols %d&ndash;%d, rows %d&ndash;%d</td><td class="n">%d</td>'
        '<td style="color:var(--mute)">%s</td></tr>'
        % (bc, bc + 15, br, br + 7, n, esc(what))
        for bc, br, n, what in region_rows),
)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8").write(HTML)
print("\n[coverage] %s  %d KB" % (OUT, len(HTML.encode()) // 1024))
