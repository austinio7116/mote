#!/usr/bin/env python3
"""
Build docs/armour-proposal.html -- a proposal to be corrected, not a record.

Source cols 16-24, rows 21-26 is ONE armour set: seven pieces across the
columns, five colours down the rows (cols 18 and 21 are empty gutters). The game
currently draws its thirteen armour pieces from scattered cells in that block and
most of them are furniture and bones, because the block was never read as a
block.

This page shows the grid as a grid -- every cell at 7x with the column's proposed
name, the row's colour, what the game draws there today, and which of the two
subsheets it currently falls in. Underneath, the thirteen items with the cell I
propose to move each one to.

Nothing here is applied. Correct the column names and the page regenerates.

Usage:  python3 authoring/gen_armour_proposal.py
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
OUT = os.path.join(GAME, "docs", "armour-proposal.html")
TS = 8

C0, C1, R0, R1 = 16, 24, 21, 26
GUTTERS = {18, 21}

# What I think each column is. THIS IS THE PART TO CORRECT.
COLUMN = {
    16: ("Open helm", "A helm seen face-on with a vertical nose guard."),
    17: ("Great helm", "Full helm with a banded brow and a visor slit. "
                       "Row 22 is a low brimmed cap instead."),
    19: ("Mage hat", "Pointed, worn at an angle. You called (19,23) a white "
                     "mage hat."),
    20: ("Hood", "Rounded, two dark eye holes. You called (20,23) a hood."),
    22: ("Cuirass", "Flared shoulders over a panelled chest plate. Row 21 is a "
                    "soft tunic with buttons."),
    23: ("Hauberk", "A collared body piece with an open neck and two studs."),
    24: ("Round shield", "Face-on, with a centre boss."),
}
ROW = {
    21: ("Soft / leather", "Only two pieces exist on this row."),
    22: ("Brown / iron", ""),
    23: ("Silver / steel", ""),
    24: ("Gold", ""),
    25: ("Blue / mithril", ""),
    26: ("Red / crimson", ""),
}

# item name -> the cell I propose moving it to, and why
PROPOSAL = [
    ("leather cap",     (16, 21), "the brimmed leather cap"),
    ("iron helm",       (16, 22), "the same helm in iron"),
    ("horned helm",     (17, 22), "the low brimmed helm"),
    ("steel helm",      (16, 23), "already correct -- you confirmed this one"),
    ("great helm",      (17, 23), "the banded full helm"),
    ("golden helm",     (17, 24), "the same helm in gold"),
    ("soft leather",    (22, 21), "the buttoned tunic"),
    ("studded leather", (22, 22), "the leather cuirass"),
    ("chain mail",      (23, 23), "the steel hauberk"),
    ("plate mail",      (22, 23), "the steel cuirass"),
    ("mithril coat",    (22, 25), "the cuirass in blue"),
    ("leather shield",  (24, 22), "the brown round shield"),
    ("iron shield",     (24, 23), "the steel round shield"),
]

SPARE = "Columns 19 and 20 -- the mage hats and hoods, five colours of each -- " \
        "have no item on them at all. Ten unused pieces of head armour."


def b64(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


src = Image.open(os.path.join(HERE, "source_tileset.png")).convert("RGBA")
USES = inuse.build()

# which subsheet each cell currently falls in, so the split is visible
SHEET_OF = {}
for name, (c0, r0, cols) in inuse.GEO.items():
    im = Image.open(os.path.join(GAME, "assets", "sheets", name + ".png"))
    for r in range(im.height // TS):
        for c in range(cols):
            SHEET_OF.setdefault((c0 + c, r0 + r), []).append(name)


def tile(c, r, scale=7):
    t = src.crop((c * TS, r * TS, c * TS + TS, r * TS + TS))
    return ('<img class="t" width="%d" height="%d" src="%s" alt="">'
            % (TS * scale, TS * scale, b64(t)))


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


cols = [c for c in range(C0, C1 + 1) if c not in GUTTERS]

head = "".join(
    '<th><span class="cn">%s</span><span class="cc">col %d</span></th>'
    % (esc(COLUMN[c][0]), c) for c in cols)

body = []
for r in range(R0, R1 + 1):
    cells = []
    for c in cols:
        t = src.crop((c * TS, r * TS, c * TS + TS, r * TS + TS))
        # the source is paletted with index 0 as the background, and converting
        # to RGBA makes that opaque black -- so "empty" is all-black, not alpha
        if not any(p[3] and p[:3] != (0, 0, 0) for p in t.get_flattened_data()):
            cells.append('<td class="empty"></td>')
            continue
        uses = USES.get((c, r), [])
        sheets = SHEET_OF.get((c, r), [])
        chip = ('<span class="use">%s</span>' % esc(uses[0].split(": ")[-1])
                if uses else '<span class="free">unused</span>')
        cells.append('<td>%s<span class="co">%d,%d</span>%s%s</td>'
                     % (tile(c, r), c, r, chip,
                        '<span class="sh">%s</span>' % esc(sheets[0]) if sheets else ""))
    body.append('<tr><th class="rh"><span class="rn">%s</span>'
                '<span class="cc">row %d</span></th>%s</tr>'
                % (esc(ROW[r][0]), r, "".join(cells)))

notes = "".join('<div><dt>%s</dt><dd>%s</dd></div>' % (esc(COLUMN[c][0]), esc(COLUMN[c][1]))
                for c in cols)

item_c = open(os.path.join(GAME, "src", "rl_item.c"), encoding="utf-8").read()
tbl = inuse.table(item_c, "g_item_kind[]")
now = {}
for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*([WTEOGFRLKJ])\((\d+)\)\s*,\s*TV_ARMOUR', tbl):
    now[m.group(1)] = inuse.src_coord(
        {"L": "loot_furniture", "O": "treasure_ore", "G": "crowns_fx"}[m.group(2)],
        int(m.group(3)))

rows = []
for name, (c, r), why in PROPOSAL:
    oc, orr = now.get(name, (0, 0))
    was = USES.get((oc, orr), [])
    same = (oc, orr) == (c, r)
    rows.append(
        '<tr class="%s"><td class="n">%s</td>'
        '<td class="was">%s<span class="co">%d,%d</span></td>'
        '<td class="arrow">&rarr;</td>'
        '<td class="now">%s<span class="co">%d,%d</span></td>'
        '<td class="why">%s</td></tr>'
        % ("same" if same else "", esc(name),
           tile(oc, orr, 4), oc, orr, tile(c, r, 4), c, r, esc(why)))

HTML = """<title>Roguemote &mdash; armour block, proposed</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{
  color-scheme:light dark;
  --ground:#0a0910; --panel:#14121e; --raised:#221e33; --rule:#3e3856;
  --ink:#e2ded2; --mute:#8b85a6; --gold:#ffc850; --good:#00e436; --warn:#ff77a8;
}
@media (prefers-color-scheme:light){:root{
  --ground:#e5e3ea; --panel:#fbfafd; --raised:#efedf4; --rule:#c2bed0;
  --ink:#16141f; --mute:#56526a; --gold:#8a5c00; --good:#00733a; --warn:#a3306a}}
:root[data-theme=dark]{--ground:#0a0910;--panel:#14121e;--raised:#221e33;
  --rule:#3e3856;--ink:#e2ded2;--mute:#8b85a6;--gold:#ffc850;--good:#00e436;--warn:#ff77a8}
:root[data-theme=light]{--ground:#e5e3ea;--panel:#fbfafd;--raised:#efedf4;
  --rule:#c2bed0;--ink:#16141f;--mute:#56526a;--gold:#8a5c00;--good:#00733a;--warn:#a3306a}
*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);line-height:1.5;
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
img.t{image-rendering:pixelated;display:block;margin:0 auto}
.wrap{max-width:1180px;margin:0 auto;padding:28px 18px 80px}
h1{font-size:clamp(22px,4vw,30px);margin:0 0 6px;letter-spacing:.01em}
h1 b{color:var(--gold);font-weight:inherit}
.lede{color:var(--mute);max-width:66ch;margin:0 0 26px}
h2{font-size:17px;margin:34px 0 10px;padding-bottom:7px;border-bottom:1px solid var(--rule)}
.scroll{overflow-x:auto;background:var(--panel);border:1px solid var(--rule);border-radius:10px}
table{border-collapse:collapse;width:100%;min-width:720px}
th,td{padding:8px 6px;text-align:center;vertical-align:top;border:1px solid var(--rule)}
thead th{background:var(--raised)}
.cn{display:block;font-size:13px;color:var(--gold)}
.cc,.co,.sh{display:block;font-family:ui-monospace,Menlo,Consolas,monospace;
  font-size:10px;color:var(--mute)}
.rh{background:var(--raised);text-align:right;white-space:nowrap;min-width:104px}
.rn{display:block;font-size:12.5px}
td.empty{background:repeating-linear-gradient(45deg,transparent,transparent 5px,
  color-mix(in srgb,var(--rule) 45%, transparent) 5px,
  color-mix(in srgb,var(--rule) 45%, transparent) 10px)}
.use{display:block;font-size:10.5px;color:var(--warn);margin-top:3px;max-width:92px}
.free{display:block;font-size:10.5px;color:var(--good);margin-top:3px}
dl{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));margin:14px 0 0}
dl>div{background:var(--panel);border:1px solid var(--rule);border-radius:9px;padding:10px 12px}
dt{color:var(--gold);font-size:13.5px}
dd{margin:2px 0 0;color:var(--mute);font-size:13px}
table.prop{min-width:600px}
table.prop td{text-align:left;vertical-align:middle}
table.prop td.n{color:var(--gold);font-size:14px;white-space:nowrap}
table.prop td.was,table.prop td.now{text-align:center;width:64px}
table.prop td.arrow{color:var(--mute);width:26px;text-align:center}
table.prop td.why{color:var(--mute);font-size:13px}
tr.same td.n::after{content:" (unchanged)";color:var(--good);font-size:11.5px}
.note{background:var(--panel);border:1px solid var(--rule);border-left:3px solid var(--gold);
  border-radius:9px;padding:12px 14px;margin:16px 0 0;color:var(--mute);font-size:13.5px}
.note b{color:var(--ink);font-weight:600}
</style>
<div class="wrap">
<h1>The armour block, <b>as I read it</b></h1>
<p class="lede">Source columns 16&ndash;24, rows 21&ndash;26 are one set: seven
pieces across, five colours down, with two empty gutter columns. The game&rsquo;s
thirteen armour items are scattered through it and most of them landed on
furniture and bones, because nobody had read the block as a block. Everything
below is a proposal &mdash; nothing is applied.</p>

<h2>The grid</h2>
<p class="lede">Pink is what the game draws on that tile today. Green is a tile
nothing uses.</p>
<div class="scroll"><table>
<thead><tr><th></th>__HEAD__</tr></thead>
<tbody>__BODY__</tbody>
</table></div>

<h2>What I think each column is</h2>
<dl>__NOTES__</dl>
<div class="note">__SPARE__</div>

<h2>Where I would move the thirteen</h2>
<div class="scroll"><table class="prop"><tbody>__ROWS__</tbody></table></div>
<div class="note"><b>Not applied.</b> Tell me which column names are wrong and
I will redo the mapping in one pass, the way the classes went.</div>
</div>
"""
for _k, _v in (("__HEAD__", head), ("__BODY__", "".join(body)),
               ("__NOTES__", notes), ("__SPARE__", esc(SPARE)),
               ("__ROWS__", "".join(rows))):
    HTML = HTML.replace(_k, _v)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8").write(HTML)
print("[armour] %s  %d KB" % (OUT, len(HTML) // 1024))
