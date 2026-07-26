#!/usr/bin/env python3
"""
Build docs/weapon-proposal.html -- a proposal to be corrected, not a record.

The same story as the armour block. Source cols 16-24, rows 29-33 are ONE melee
set: nine weapon types across the columns, five materials down the rows. Cols
25-30 are the ranged set on the same rows. The game's seventeen weapons are
scattered through it and land on the wrong types, because the block was cut up
by three different subsheets and never read as a block -- the dagger and the
pick-axe both sit on pickaxes, the long sword and the war hammer both sit on
axes, and the battle axe is on a crossbow.

This page shows the grid as a grid, every cell at 7x, with the column's proposed
weapon type, the row's material, and what the game draws there today. Underneath,
the seventeen weapons with the cell I propose moving each to.

Nothing here is applied. Correct the column names and the page regenerates.

Usage:  python3 authoring/gen_weapon_proposal.py
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
OUT = os.path.join(GAME, "docs", "weapon-proposal.html")
TS = 8

# What I think each column is. THIS IS THE PART TO CORRECT.
COLUMN = {
    16: ("Broad blade", "Wide, flared, gem at the base. Short for its width -- "
                        "a falchion or a great sword rather than a long one."),
    17: ("Dagger", "The same blade, small."),
    18: ("Long sword", "Long, straight, thin. The clearest column in the block."),
    19: ("War pick", "Angled head on a brown haft. This is the column the dagger "
                     "and the pick-axe are BOTH sitting on today."),
    20: ("Spear", "Thin shaft, small leaf point."),
    21: ("Mace", "Shaft with a compact spiked head."),
    22: ("Battle axe", "Shaft with a big curved bit. The long sword and the war "
                       "hammer are both on this column today."),
    23: ("Star", "An eight-pointed burst, no shaft. Could be a thrown shuriken or "
                 "a morning-star head -- I cannot tell at 8x8, and it changes "
                 "which of the two items belongs here."),
    24: ("War hammer", "Chunky rectangular head."),
}
RANGED = {
    25: ("Bow", "Curve and string."),
    26: ("Crossbow", "The three crossbow-ish columns are the ones I am least "
                     "sure about."),
    27: ("Crossbow, heavy", ""),
    28: ("Crossbow, variant", ""),
    29: ("Staff / wand", "Gem at the tip. The wands already use this column."),
    30: ("Arrow / bolt", "A bare shaft."),
}
ROW = {
    28: ("(sparse)", "Only two cells: a wooden club and a green staff."),
    29: ("Wood / bronze", "Only cols 16-17 and the ranged columns exist here."),
    30: ("Steel", "The full row -- the workhorse tier."),
    31: ("Gold", ""),
    32: ("Ice / blue", ""),
    33: ("Fire / red", ""),
}

# item name -> the cell I propose moving it to, and why.
#
# The materials are the point: they give every weapon type a power tier without
# needing new art, so a mace and a morning star are the same head in steel and
# gold, and Frost Brand and Flame Tongue are the long sword in ice and fire.
PROPOSAL = [
    ("dagger",         (17, 29), "the bronze dagger -- cheapest blade, cheapest metal"),
    ("throwing star",  (23, 30), "the steel burst, IF that column is a shuriken"),
    ("pick-axe",       (19, 30), "the steel pick; it is on this column already"),
    ("short sword",    (17, 30), "the same blade as the dagger, in steel"),
    ("spear",          (20, 30), "the steel spear"),
    ("mace",           (21, 30), "the steel mace"),
    ("long sword",     (18, 30), "the steel long blade; was on an axe"),
    ("war hammer",     (24, 30), "the steel hammer; was on an axe"),
    ("morning star",   (21, 31), "the mace head in gold -- one tier up from the mace"),
    ("broad sword",    (16, 30), "the steel broad blade"),
    ("battle axe",     (22, 30), "the steel axe; was on a crossbow"),
    ("gilded blade",   (18, 31), "the long blade in gold, which is its name"),
    ("trident",        (20, 31), "the spear in gold"),
    ("great axe",      (22, 31), "the axe in gold"),
    ("Frost Brand",    (18, 32), "the long blade in ice"),
    ("Flame Tongue",   (18, 33), "the long blade in fire"),
    ("Blade of Chaos", (16, 33), "the broad blade in fire -- the biggest weapon in the game"),
]

QUESTIONS = [
    "Is col 16 a broad sword or an axe? It reads wide and short to me, and the "
    "Blade of Chaos hangs on the answer.",
    "Is col 23 a thrown shuriken or a morning-star head? Whichever it is, the "
    "other item needs somewhere else to go.",
    "Are cols 26-28 really three crossbows? There is no bow or crossbow item in "
    "the game yet, so nothing depends on it today -- but there could be.",
    "Row 29 only fills cols 16-17 in the melee half. Is the bronze tier meant to "
    "be blades only?",
]


def b64(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


src = Image.open(os.path.join(HERE, "source_tileset.png")).convert("RGBA")
USES = inuse.build()


def tile(c, r, scale=7):
    t = src.crop((c * TS, r * TS, c * TS + TS, r * TS + TS))
    return ('<img class="t" width="%d" height="%d" src="%s" alt="">'
            % (TS * scale, TS * scale, b64(t)))


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def empty(c, r):
    t = src.crop((c * TS, r * TS, c * TS + TS, r * TS + TS))
    # the source is paletted with index 0 as the background, so converting to
    # RGBA makes "empty" opaque black rather than transparent
    return not any(p[3] and p[:3] != (0, 0, 0) for p in t.get_flattened_data())


def grid(colspec, r0, r1):
    cols = sorted(colspec)
    head = "".join(
        '<th><span class="cn">%s</span><span class="cc">col %d</span></th>'
        % (esc(colspec[c][0]), c) for c in cols)
    body = []
    for r in range(r0, r1 + 1):
        cells = []
        for c in cols:
            if empty(c, r):
                cells.append('<td class="empty"></td>')
                continue
            uses = [u for u in USES.get((c, r), [])]
            chip = ('<span class="use">%s</span>' % esc(uses[0].split(": ")[-1])
                    if uses else '<span class="free">unused</span>')
            cells.append('<td>%s<span class="co">%d,%d</span>%s</td>'
                         % (tile(c, r), c, r, chip))
        body.append('<tr><th class="rh"><span class="rn">%s</span>'
                    '<span class="cc">row %d</span></th>%s</tr>'
                    % (esc(ROW[r][0]), r, "".join(cells)))
    return ('<div class="scroll"><table><thead><tr><th></th>%s</tr></thead>'
            '<tbody>%s</tbody></table></div>' % (head, "".join(body)))


# where each weapon sits today
item_c = open(os.path.join(GAME, "src", "rl_item.c"), encoding="utf-8").read()
tbl = inuse.table(item_c, "g_item_kind[]")
SHEET = {"W": "weapons_potions", "T": "tools_wands", "E": "weapons_elemental"}
now = {}
for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*([WTE])\((\d+)\)\s*,\s*TV_WEAPON', tbl):
    now[m.group(1)] = inuse.src_coord(SHEET[m.group(2)], int(m.group(3)))

rows = []
for name, (c, r), why in PROPOSAL:
    oc, orr = now.get(name, (0, 0))
    same = (oc, orr) == (c, r)
    rows.append(
        '<tr class="%s"><td class="n">%s</td>'
        '<td class="was">%s<span class="co">%d,%d</span></td>'
        '<td class="arrow">&rarr;</td>'
        '<td class="now">%s<span class="co">%d,%d</span></td>'
        '<td class="why">%s</td></tr>'
        % ("same" if same else "", esc(name),
           tile(oc, orr, 4), oc, orr, tile(c, r, 4), c, r, esc(why)))

notes = "".join('<div><dt>%s</dt><dd>%s</dd></div>'
                % (esc(v[0]), esc(v[1]))
                for d in (COLUMN, RANGED) for v in (d[k] for k in sorted(d)) if v[1])
qs = "".join("<li>%s</li>" % esc(q) for q in QUESTIONS)

HTML = """<title>Roguemote &mdash; weapon block, proposed</title>
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
.cc,.co{display:block;font-family:ui-monospace,Menlo,Consolas,monospace;
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
.note ol{margin:8px 0 0;padding-left:20px}
.note li{margin:5px 0}
</style>
<div class="wrap">
<h1>The weapon block, <b>as I read it</b></h1>
<p class="lede">Source columns 16&ndash;24, rows 29&ndash;33 are one melee set:
nine weapon types across, five materials down. Columns 25&ndash;30 are the ranged
set on the same rows. The game&rsquo;s seventeen weapons are scattered through it
and most landed on the wrong type, because three different subsheets cut the
block up and nobody read it as a block. Everything below is a proposal &mdash;
nothing is applied.</p>

<h2>The melee set</h2>
<p class="lede">Pink is what the game draws on that tile today. Green is a tile
nothing uses.</p>
__MELEE__

<h2>The ranged set</h2>
<p class="lede">No bow or crossbow item exists yet. The wands already live on
column 29.</p>
__RANGED__

<h2>What I think each column is</h2>
<dl>__NOTES__</dl>

<h2>Where I would move the seventeen</h2>
<p class="lede">The materials do the work: they give each weapon type a power
tier without needing new art, so the mace and the morning star are one head in
steel and gold, and Frost Brand and Flame Tongue are the long sword in ice and
fire.</p>
<div class="scroll"><table class="prop"><tbody>__ROWS__</tbody></table></div>

<div class="note"><b>What I am least sure of.</b><ol>__QS__</ol></div>
<div class="note"><b>Not applied.</b> Correct the column names and I will redo
the mapping in one pass, the way the armour block went.</div>
</div>
"""

html = (HTML.replace("__MELEE__", grid(COLUMN, 29, 33))
            .replace("__RANGED__", grid(RANGED, 28, 33))
            .replace("__NOTES__", notes)
            .replace("__ROWS__", "".join(rows))
            .replace("__QS__", qs))

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8").write(html)
print("[weapons] %s  %d KB" % (OUT, len(html.encode()) // 1024))
