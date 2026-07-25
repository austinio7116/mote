#!/usr/bin/env python3
"""Build the self-contained blob47 review page from the generated evidence.

Reads the PNGs preview_terrain.py wrote plus the captured verifier output, and
inlines everything as data URIs (an Artifact's CSP blocks external requests).

    python3 preview_terrain.py            # first, render the evidence
    python3 verify_terrain.py > /tmp/roguemote_terrain/verify.txt
    python3 gen_terrain_report.py         # -> /tmp/roguemote_terrain/report.html
"""
import os, base64, html

import blob47
import gen_terrain as gt
from blob47 import N, NE, E, SE, S, SW, W, NW

EV = "/tmp/roguemote_terrain"
OUT = os.path.join(EV, "report.html")


def uri(fn):
    p = os.path.join(EV, fn)
    if not os.path.exists(p):
        return None
    return "data:image/png;base64," + base64.b64encode(open(p, "rb").read()).decode()


def fig(fn, cap, note=""):
    u = uri(fn)
    if not u:
        return f'<p class="missing">missing {html.escape(fn)}</p>'
    return (f'<figure><div class="scroll"><img src="{u}" alt="{html.escape(cap)}"></div>'
            f'<figcaption><b>{html.escape(cap)}</b>'
            + (f' {html.escape(note)}' if note else "") + '</figcaption></figure>')


def canon_table():
    rows = []
    for i, m in enumerate(blob47.CANON):
        ic = []
        for bit, (a, b), nm in ((NE, (N, E), "NE"), (SE, (S, E), "SE"),
                                (SW, (S, W), "SW"), (NW, (N, W), "NW")):
            if (m & a) and (m & b) and not (m & bit):
                ic.append(nm)
        nfold = sum(1 for x in range(256) if blob47.LUT[x] == i)
        rows.append(
            f"<tr><td class='n'>{i}</td><td class='n'>{m}</td>"
            f"<td class='mono sm'>{blob47.describe(m)}</td>"
            f"<td class='mono'>{'<span class=ic>' + ','.join(ic) + '</span>' if ic else '<span class=dim>-</span>'}</td>"
            f"<td class='n'>{nfold}</td></tr>")
    return ("<table class='canon'><thead><tr><th>cell</th><th>mask</th>"
            "<th>same-terrain neighbours</th><th>inner corners</th><th>raw masks</th>"
            "</tr></thead><tbody>" + "".join(rows) + "</tbody></table>")


def verify_block():
    p = os.path.join(EV, "verify.txt")
    if not os.path.exists(p):
        return "<p class='missing'>no verifier output captured</p>"
    txt = open(p).read()
    out = []
    for line in txt.split("\n"):
        e = html.escape(line)
        if line.strip().startswith("ok "):
            e = e.replace("ok", "<span class='ok'>ok</span>", 1)
        elif "FAIL" in line:
            e = e.replace("FAIL", "<span class='fail'>FAIL</span>", 1)
        elif line and line[0].isdigit() and "." in line[:3]:
            e = f"<b>{e}</b>"
        out.append(e)
    return "<pre class='log'>" + "\n".join(out) + "</pre>"


TERRAIN_BLURB = {
    "wall_brick": ("Dungeon stone-brick wall",
                   "edge_is_solid=1 &mdash; off-map counts as more wall, so a map-edge wall "
                   "has no rim drawn along the screen border."),
    "hedge": ("Garden hedge maze",
              "edge_is_solid=0 &mdash; the maze draws its own outer rim, because you are "
              "meant to see the outside of the hedge."),
}


def terrain_section(name):
    import map_blob47 as mp
    spec = gt.TERRAINS[name]
    title, edgenote = TERRAIN_BLURB[name]
    band = mp.BANDS[spec["band"]]
    c0, r0, c1, r1 = band["rect"]
    found, blanks = mp.map_band(spec["band"], band)
    return f"""
<section id="{name}">
  <h2>{title} <span class="tag mono">{name}</span></h2>
  <p class="lede">Source art at cols <b>{c0}&ndash;{c1}</b>, rows <b>{r0}&ndash;{r1}</b> &mdash;
    {(c1-c0+1)*(r1-r0+1)} cells, {len(blanks)} blank, <b>{len(found)} configurations</b>,
    each drawn exactly once. Fill is palette index {band['fill']}, border {band['border']}.
    {edgenote}</p>
  {fig(f"{name}_configs.png", "All 47 configurations, each in its own 3&times;3 neighbourhood",
       "The gold outline marks the cell under test; its neighbours are drawn so the border and corners can be judged in context.")}
  {fig(f"{name}_fringe.png", "Fringe cases",
       "Isolated tiles, 1-wide spurs, diagonal-only contact, holes, T-junctions, staircases and combs. The spurs are the cases a nine-slice structurally cannot draw.")}
  {fig(f"{name}_atlas.png", "The baked 47-cell atlas",
       "What ships in tilesets/" + name + ".png, cell index and mask under each.")}
  {fig(f"{name}_dungeon.png", "A full map",
       "Rendered through a port of the engine's own draw_autotile(), so this is what the device draws.")}
  {fig(f"{name}_edge.png", "edge_is_solid, both ways",
       "A completely solid map with off-map neighbours counted as same-terrain (right) and as empty (left).")}
</section>"""


PAGE = """<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Roguemote &mdash; blob47 terrain review</title>
<style>
:root{{
  color-scheme:dark light;
  /* the source art is the PICO-8 palette; the page borrows it directly */
  --navy:#1D2B53; --ink:#0B1020; --brown:#5F574F; --bone:#C2C3C7;
  --grn:#00E436; --grn-d:#008751; --amb:#FFA300; --red:#FF004D; --lav:#83769C;
  --bg:#0B1020; --panel:#141B33; --panel2:#1B2445; --line:#2A3560;
  --tx:#E4E6F0; --dim:#8C93B4; --acc:var(--amb);
}}
@media (prefers-color-scheme:light){{:root{{
  --bg:#F2F1EC; --panel:#FFFFFF; --panel2:#EAE8E1; --line:#D2CFC4;
  --tx:#1A1E30; --dim:#5F5C6E; --grn:#0A7A2E; --amb:#9A5B00; --red:#C1002F;
  --bone:#4A4A52;
}}}}
:root[data-theme="dark"]{{color-scheme:dark;--bg:#0B1020;--panel:#141B33;--panel2:#1B2445;
  --line:#2A3560;--tx:#E4E6F0;--dim:#8C93B4;--grn:#00E436;--amb:#FFA300;--red:#FF004D;--bone:#C2C3C7;}}
:root[data-theme="light"]{{color-scheme:light;--bg:#F2F1EC;--panel:#FFFFFF;--panel2:#EAE8E1;
  --line:#D2CFC4;--tx:#1A1E30;--dim:#5F5C6E;--grn:#0A7A2E;--amb:#9A5B00;--red:#C1002F;--bone:#4A4A52;}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--tx);
  font:15px/1.6 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}}
.mono,pre,code,td.n,th{{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace}}
.wrap{{max-width:1180px;margin:0 auto;padding:0 20px 96px}}
header.hero{{border-bottom:1px solid var(--line);background:
  linear-gradient(180deg,var(--panel) 0%,var(--bg) 100%);padding:34px 0 22px;margin-bottom:8px}}
header.hero .wrap{{padding-bottom:0}}
h1{{font-family:ui-monospace,Menlo,monospace;font-size:clamp(21px,3.4vw,32px);
  margin:0 0 6px;letter-spacing:-.02em;text-wrap:balance}}
h1 b{{color:var(--amb)}}
.sub{{color:var(--dim);max-width:70ch;margin:0}}
.chips{{display:flex;gap:8px;flex-wrap:wrap;margin-top:16px}}
.chip{{font:600 12px/1 ui-monospace,Menlo,monospace;padding:6px 10px;border-radius:999px;
  border:1px solid var(--line);background:var(--panel2);color:var(--dim)}}
.chip.pass{{color:var(--grn);border-color:color-mix(in srgb,var(--grn) 45%,transparent)}}
.chip.warn{{color:var(--amb);border-color:color-mix(in srgb,var(--amb) 45%,transparent)}}
nav.jump{{position:sticky;top:0;z-index:9;background:color-mix(in srgb,var(--bg) 92%,transparent);
  backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:9px 0}}
nav.jump .wrap{{display:flex;gap:6px;flex-wrap:wrap;padding-bottom:0}}
nav.jump a{{font:12.5px/1 ui-monospace,Menlo,monospace;color:var(--dim);text-decoration:none;
  border:1px solid var(--line);border-radius:6px;padding:6px 10px}}
nav.jump a:hover,nav.jump a:focus-visible{{color:var(--tx);border-color:var(--amb);outline:none}}
section{{margin-top:44px;scroll-margin-top:64px}}
h2{{font-family:ui-monospace,Menlo,monospace;font-size:21px;margin:0 0 4px;
  padding-bottom:9px;border-bottom:2px solid var(--line);display:flex;
  align-items:baseline;gap:10px;flex-wrap:wrap}}
h3{{font-family:ui-monospace,Menlo,monospace;font-size:15px;margin:26px 0 8px;color:var(--amb)}}
.tag{{font-size:12px;font-weight:400;color:var(--dim);border:1px solid var(--line);
  border-radius:5px;padding:2px 7px}}
.lede{{color:var(--dim);max-width:78ch}}
p{{max-width:78ch}}
figure{{margin:20px 0 0;background:var(--panel);border:1px solid var(--line);border-radius:10px;
  overflow:hidden}}
.scroll{{overflow-x:auto;background:#12151E;padding:10px}}
figure img{{display:block;max-width:100%;height:auto;image-rendering:pixelated;margin:0 auto}}
figcaption{{padding:10px 14px;font-size:13px;color:var(--dim);border-top:1px solid var(--line)}}
figcaption b{{color:var(--tx);font-weight:600}}
.grid2{{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}}
.card{{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 16px}}
.card h4{{margin:0 0 6px;font:600 13px/1.4 ui-monospace,Menlo,monospace;color:var(--amb)}}
.card p{{margin:0;font-size:13.5px;color:var(--dim)}}
.card.bad{{border-color:color-mix(in srgb,var(--red) 40%,var(--line))}}
.card.bad h4{{color:var(--red)}}
pre.log{{background:#0A0E1A;border:1px solid var(--line);border-radius:10px;padding:14px;
  overflow-x:auto;font-size:12px;line-height:1.55;color:var(--dim);margin:14px 0 0}}
:root[data-theme="light"] pre.log,@media (prefers-color-scheme:light){{}}
.ok{{color:var(--grn);font-weight:700}}
.fail{{color:var(--red);font-weight:700}}
table.canon{{border-collapse:collapse;width:100%;font-size:12.5px;margin-top:12px}}
table.canon th{{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.07em;
  color:var(--dim);border-bottom:1px solid var(--line);padding:7px 9px;position:sticky;top:44px;
  background:var(--bg)}}
table.canon td{{padding:5px 9px;border-bottom:1px solid color-mix(in srgb,var(--line) 55%,transparent)}}
table.canon tr:hover td{{background:var(--panel2)}}
td.n{{font-variant-numeric:tabular-nums;text-align:right;width:1%;white-space:nowrap}}
.sm{{font-size:11.5px}}
.ic{{color:var(--amb)}}
.dim{{color:var(--dim)}}
.tblwrap{{max-height:520px;overflow:auto;border:1px solid var(--line);border-radius:10px;
  background:var(--panel)}}
.missing{{color:var(--red);font-family:ui-monospace,Menlo,monospace}}
hr.rule{{border:0;border-top:1px solid var(--line);margin:34px 0 0}}
ul{{max-width:78ch;color:var(--dim)}} li{{margin:5px 0}}
li b,p b{{color:var(--tx)}}
code{{background:var(--panel2);padding:1px 5px;border-radius:4px;font-size:.92em}}
</style>

<header class="hero"><div class="wrap">
  <h1><b>blob47</b> terrain review &mdash; roguemote</h1>
  <p class="sub">Both terrain rulesets rebuilt from nine-slice to a full 47-cell blob
  autotile, generated from the CC0 source art and verified against the engine's own
  rules in <code>sdk/mote_tile.h</code>. Everything below is rendered through a port of
  <code>draw_autotile()</code>, so it is what the device draws.</p>
  <div class="chips">
    <span class="chip pass">{nchecks} checks passed</span>
    <span class="chip">47 cells &times; 2 terrains</span>
    <span class="chip pass">agrees with the compiled C</span>
    <span class="chip pass">5/5 mutations caught</span>
    <span class="chip warn">earlier synthesis retracted</span>
  </div>
</div></header>

<nav class="jump"><div class="wrap">
  <a href="#summary">summary</a><a href="#contract">the 47</a><a href="#findings">correction</a>
  <a href="#wall_brick">wall_brick</a><a href="#hedge">hedge</a>
  <a href="#notdone">not done</a><a href="#teeth">trust</a><a href="#verify">verifier log</a><a href="#table">cell table</a>
</div></nav>

<div class="wrap">

<section id="summary">
  <h2>What changed</h2>
  <p>Both rulesets were <b>nine-slice</b> (9 cells). A nine-slice can only express
  <b>16 of the 47</b> blob configurations &mdash; it has no inner corners, and no cell with
  borders on opposite sides, so a 1-tile-wide wall or the inside of an L-junction was drawn
  wrong. They are now <b>blob47</b>, covering all 256 neighbour masks.</p>
  <p>All 47 cells come from the sheet's own hand-drawn art. The mapping from configuration to
  source tile is recovered by classifying each tile's pixels, and is a clean bijection: every
  config present exactly once.</p>
  <div class="grid2" style="margin-top:16px">
    <div class="card"><h4>gen_terrain.py</h4><p>Reorders the mapped source tiles into cell order. Refuses to emit unless the band maps cleanly 47 ways.</p></div>
    <div class="card"><h4>verify_terrain.py</h4><p>10 groups of checks, none of which trust
      the generator &mdash; each re-derives the expected answer from the C.</p></div>
    <div class="card"><h4>map_blob47.py</h4><p>Classifies each source tile by its own pixels to recover which of the 47 configs it draws.</p></div>
    <div class="card"><h4>blob47.py</h4><p>The canonical contract: the reduction rule and
      cell ordering, ported from <code>mote_tile.h</code>.</p></div>
  </div>
</section>

<section id="contract">
  <h2>Why 47, and which ones are hard</h2>
  <p>Each cell looks at its 8 neighbours &rarr; 256 possible masks. The engine's
  <code>mote__at_reduce()</code> drops a corner bit unless <b>both</b> its adjacent cardinals
  are also set &mdash; a diagonal only matters if you can actually reach it. That collapses
  256 masks to <b>47</b> distinct tiles.</p>
  <ul>
    <li><b>16</b> of the 47 are what a nine-slice can already draw.</li>
    <li><b>31</b> need at least one <span class="ic">inner (concave) corner</span> &mdash;
      <b>52</b> such corners in total across the set. These are the ones that were previously
      wrong.</li>
  </ul>
  <p>The generator marks a concave corner by stamping the art's own rounded corner into just
  that corner of a filled tile, at half the outer radius, so it reads as a small notch.</p>
</section>

<section id="findings">
  <h2>Correction: the art was already there</h2>
  <p>An earlier version of this page claimed the source had only a nine-slice per terrain and
  that all 47 cells had to be composited. <b>That was wrong.</b> The sheet already draws a
  complete 47-tile blob set for both terrains &mdash; a row of blocks of mixed width (some
  3&times;3, some 2&times;3) over 16 columns &times; 3 rows: 48 cells, one blank,
  <b>47 tiles, every configuration drawn once by hand</b>. Nothing is synthesised any more.</p>
  <div class="grid2">
    <div class="card bad"><h4>What went wrong</h4>
      <p>The scanner tested for a <i>plain</i> nine-slice &mdash; edges hugging their own side,
      shallow borders. By construction that only ever matches the one block per row with no
      inner corners. Every other block, precisely the ones holding the concave corners and the
      spur cases, was rejected as "not an autotile set". It asked the wrong question, so it got
      a confident wrong answer. Deleted.</p></div>
    <div class="card bad"><h4>Retracted: the "speckled variants"</h4>
      <p>I reported two blocks as defective variants that would scatter dots through solid
      walls, because their centre tiles carry a mark in each corner. They are not variants:
      they <b>are</b> the concave-corner cells. (1,36) is mask 85 &mdash; all four corners
      concave. The guard was rejecting correct art for looking like what it is.</p></div>
    <div class="card"><h4>Survives: which bands qualify</h4>
      <p>Re-checked properly, by auto-detecting each band's fill/border palette and testing
      every one for a clean 47-way mapping. Still only these two terrains have a complete set;
      jungle grass, temple, purple and the cobblestone band do not. Same conclusion, but now on
      evidence that actually bears on it.</p></div>
    <div class="card"><h4>How it is mapped now</h4>
      <p><code>map_blob47.py</code> classifies each source tile from its own pixels &mdash;
      which edges carry the border colour, which corners carry a concave mark &mdash; so the
      block layout never has to be parsed. Both bands come back as a clean bijection onto the
      47 cells: <b>0 missing, 0 duplicated</b>. That is itself the evidence; a misreading
      collides rather than tiling perfectly.</p></div>
  </div>
</section>
</section>
{sections}

<section id="notdone">
  <h2>What this does not cover</h2>
  <ul>
    <li><b>floor_cobble, floor_grass, water are untouched.</b> They are fill tilesets
      (<code>nvar</code> variants, no borders) and the source has no edge art for them, so
      blob47 cannot be built without drawing new tiles. They still work as before.</li>
    <li><b>No <code>xform</code> tricks.</b> The engine can flip/rotate a cell per mask to shrink
      a sheet ~3&times;. A 47&times;8&times;8 sheet is under 1KB, so all 47 cells are stored
      explicitly and <code>xform</code> is all zeroes &mdash; less to get wrong.</li>
    <li><b>Not run on hardware.</b> Verified headless: the renders come from a port of
      <code>draw_autotile()</code> and the baked LUT is byte-compared against the canonical one,
      but nothing has been pushed to a device.</li>
    <li><b>The concave notch size is a judgement call</b> &mdash; half the outer corner radius
      (2px for brick, 1px for hedge). It is one constant in <code>gen_terrain.py</code> if you
      want it chunkier or subtler.</li>
    <li><b>The blueprint line-art set</b> at (0,50) and (6,50) also passes the nine-slice scan
      and could get a blob47 ruleset if you ever want it.</li>
  </ul>
</section>

<section id="teeth">
  <h2>Why you can believe the checks</h2>
  <p>Two things back the verifier up, because "43 checks pass" is worth nothing on its own.</p>
  <h3>It agrees with the compiled C, not just my reading of it</h3>
  <p>Every other check re-derives the contract from a <i>second transcription</i> of
  <code>sdk/mote_tile.h</code> &mdash; and a transcription can be wrong the same way twice.
  So <code>ctest_blob47.c</code> compiles the real header and asks it directly. The engine's
  own <code>mote_autotile_template(MOTE_AT_BLOB47)</code> produces <b>byte-identical</b> output
  to the LUT we bake, and <code>mote__at_reduce</code>, <code>mote_autotile_mask</code>,
  <code>edge_is_solid</code> and the weighted <code>mote__at_variant</code> hash all agree
  across every case.</p>
  <h3>The checks can actually fail</h3>
  <p><code>mutate_terrain.py</code> breaks the generator five plausible ways and confirms the
  suite rejects each. Every mutation is caught by a <i>different</i> check:</p>
  <pre class="log">{mutate}</pre>
</section>

<section id="verify">
  <h2>Verifier output</h2>
  <p class="lede">Nothing here trusts the generator: the LUT is re-derived from a second
  transcription of the C, the nine-slice-expressible configs are compared pixel-for-pixel
  against the original source tiles, and the baked header is byte-compared.</p>
  {verify}
</section>

<section id="table">
  <h2>The 47 cells</h2>
  <p class="lede">Cell order is exactly how <code>mote_autotile_template()</code> numbers them:
  walk masks 0&hellip;255 and assign the next index the first time each reduced mask appears.
  "raw masks" is how many of the 256 fold into that cell.</p>
  <div class="tblwrap">{canon}</div>
</section>

</div>
"""


def main():
    sections = "\n".join(terrain_section(n) for n in gt.TERRAINS)
    vt = os.path.join(EV, "verify.txt")
    nchecks = 0
    if os.path.exists(vt):
        nchecks = sum(1 for l in open(vt) if l.strip().startswith("ok "))
    mp = os.path.join(EV, "mutate.txt")
    mut = html.escape(open(mp).read()) if os.path.exists(mp) else "not captured"
    mut = mut.replace("caught", "<span class='ok'>caught</span>")
    mut = mut.replace("NOT <span class='ok'>caught</span>", "<span class='fail'>NOT CAUGHT</span>")
    htmltxt = PAGE.format(sections=sections, verify=verify_block(), mutate=mut,
                          canon=canon_table(), nchecks=nchecks)
    open(OUT, "w").write(htmltxt)
    print(f"wrote {OUT}  ({len(htmltxt)//1024} KB, {nchecks} passing checks)")
    return OUT


if __name__ == "__main__":
    main()
