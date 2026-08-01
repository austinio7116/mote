#!/usr/bin/env python3
"""Emit a self-contained HTML tool for choosing a master-sheet cell per technology.

Picking 8x8 icons by reading a catalogue and guessing coordinates does not work — it needs
eyes on the actual cells. This puts every inked cell of source_tileset.png in a page, laid out
AS THE SHEET IS — four bands of sixteen columns, the sheet's own row and column numbers, empty
rows dropped — with the 35 technologies down the side: click a tech, click a cell, done. The choices survive a refresh (localStorage) and the box at
the bottom emits the table to paste back.

    python3 authoring/icon_picker.py        # -> /tmp/motebox_icon_picker.html
"""
import base64
import io
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC = os.path.join(HERE, "source_tileset.png")
OUT = "/tmp/motebox_icon_picker.html"

def cell_ink(im):
    """Which cells have anything in them. The sheet is mostly empty and the first version of
    this page rendered all four thousand cells at equal weight, so the eye had nowhere to
    start."""
    px = im.load()
    ink = {}
    for r in range(im.height // 8):
        for c in range(im.width // 8):
            n = 0
            for y in range(r * 8, r * 8 + 8):
                for x in range(c * 8, c * 8 + 8):
                    if px[x, y][3]:
                        n += 1
            ink[(c, r)] = n
    return ink


def sheet_data_uri():
    """The master sheet with palette index 0 (black) made transparent, as a data URI."""
    im = Image.open(SRC).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, _a = px[x, y]
            if (r, g, b) == (0, 0, 0):
                px[x, y] = (0, 0, 0, 0)
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return ("data:image/png;base64," + base64.b64encode(buf.getvalue()).decode(),
            im.size, cell_ink(im))


def techs():
    sys.path.insert(0, HERE)
    import techtree as T
    rows = T.parse()
    names = T.tech_to_name(rows)
    pre = {r["name"]: [names[p] for p in r["pre"]] for r in rows}
    depth = {}

    def d(n):
        if n not in depth:
            depth[n] = 0 if not pre[n] else 1 + max(d(p) for p in pre[n])
        return depth[n]
    for n in pre:
        d(n)
    return [(n, depth[n]) for n in sorted(pre, key=lambda x: (depth[x], x))]


CSS = """
*{box-sizing:border-box}
body{margin:0;background:#0b0d14;color:#dfe3ef;font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
header{position:sticky;top:0;z-index:9;background:#0f1320;border-bottom:1px solid #28304a;padding:10px 16px}
h1{margin:0;font-size:1.05rem;color:#f0cf55}
.hint{color:#8f96ad;font-size:.82rem;margin-top:3px}
.layout{display:flex;align-items:flex-start;gap:16px;padding:14px 16px}
.side{position:sticky;top:64px;flex:0 0 262px;max-height:calc(100vh - 80px);overflow:auto;
      background:#141826;border:1px solid #28304a;border-radius:10px;padding:8px}
.t{display:flex;align-items:center;gap:8px;padding:4px 6px;border-radius:6px;cursor:pointer}
.t:hover{background:#1b2032}
.t.on{background:#2a3350;outline:1px solid #6aa8e8}
.t .sw{width:32px;height:32px;flex:0 0 32px;background:#0c1020;border:1px solid #28304a;
       image-rendering:pixelated;background-repeat:no-repeat}
.t .nm{flex:1;font-size:.86rem}
.t .lv{color:#8f96ad;font-size:.72rem}
.t .cell{color:#f0cf55;font-size:.72rem;font-variant-numeric:tabular-nums}
.main{flex:1;min-width:0}
.band{margin-bottom:22px}
.band h2{font-size:.82rem;color:#6aa8e8;margin:0 0 6px;text-transform:uppercase;letter-spacing:.05em}
.sheet{display:grid;gap:2px;align-items:center;overflow-x:auto}
.hd,.rw{color:#565e78;font:10px/1 ui-monospace,Menlo,monospace;text-align:center}
.rw{text-align:right;padding-right:4px}
.c.empty{background:#0e1220;border-color:#161c2e;cursor:default}
.c.empty:hover{border-color:#161c2e}
.c{width:34px;height:34px;background:#0c1020;border:1px solid #222a42;border-radius:3px;
   image-rendering:pixelated;background-repeat:no-repeat;cursor:pointer;position:relative}
.c:hover{border-color:#6aa8e8}
.c.used{border-color:#7cc85e}
.c.used::after{content:"";position:absolute;inset:auto 1px 1px auto;width:5px;height:5px;
               background:#7cc85e;border-radius:50%}
footer{padding:14px 16px 40px}
textarea{width:100%;height:190px;background:#141826;color:#dfe3ef;border:1px solid #28304a;
         border-radius:8px;padding:10px;font:12px/1.6 ui-monospace,Menlo,monospace}
button{background:#2a3350;color:#dfe3ef;border:1px solid #6aa8e8;border-radius:6px;
       padding:6px 12px;cursor:pointer;font-size:.85rem;margin-right:8px}
button:hover{background:#33406a}
.count{color:#f0cf55}
"""


def main():
    uri, (W, H), ink = sheet_data_uri()
    tl = techs()
    Z = 34.0 / 8.0                      # a cell is drawn at 34 px, so the sheet scales by this
    bg = ('background-image:url(%s);background-size:%.1fpx %.1fpx'
          % (uri, W * Z, H * Z))
    bg_sw = ('background-image:url(%s);background-size:%.1fpx %.1fpx'
             % (uri, W * 4.0, H * 4.0))

    side = []
    for n, lv in tl:
        side.append('<div class="t" data-t="%s"><div class="sw" id="sw-%s"></div>'
                    '<div class="nm">%s<div class="lv">layer %d</div></div>'
                    '<div class="cell" id="cl-%s">--</div></div>'
                    % (n, n, n, lv, n))

    # THE SHEET'S OWN LAYOUT. It is four bands of sixteen columns — items and terrain, items and
    # treasure, creatures, then the font and UI — and within a band the rows mean something: a
    # colour family runs down, a set runs across. Wrapping cells to fit a container threw all of
    # that away, which is why it read as noise. Each band is a sixteen-wide grid with the sheet's
    # real row and column numbers, and a row with nothing in it is dropped rather than drawn.
    BANDS = [(0, 15, "Items, terrain, autotiles"),
             (16, 31, "Items, treasure, tools, weapons"),
             (32, 47, "Characters, animals, monsters, bosses"),
             (48, 63, "Font, UI, status, terrain edges")]
    ROWS = H // 8
    zones = []
    for c0, c1, label in BANDS:
        head = "".join('<div class="hd">%d</div>' % c for c in range(c0, c1 + 1))
        body = ['<div class="band"><h2>columns %d&ndash;%d &mdash; %s</h2>'
                '<div class="sheet" style="grid-template-columns:26px repeat(%d,34px)">'
                '<div class="hd"></div>%s' % (c0, c1, label, c1 - c0 + 1, head)]
        for r in range(ROWS):
            if not any(ink[(c, r)] for c in range(c0, c1 + 1)):
                continue                               # an empty row is not information
            body.append('<div class="rw">%d</div>' % r)
            for c in range(c0, c1 + 1):
                if not ink[(c, r)]:
                    body.append('<div class="c empty" title="%d,%d empty"></div>' % (c, r))
                else:
                    body.append('<div class="c" data-c="%d" data-r="%d" title="%d,%d" '
                                'style="background-position:%.1fpx %.1fpx"></div>'
                                % (c, r, c, r, -c * 34, -r * 34))
        body.append('</div></div>')
        zones.append("".join(body))

    js = """
const TECHS = %s;
let sel = TECHS[0][0];
const pick = JSON.parse(localStorage.getItem('mbicons') || '{}');
const CELL = 34, ZOOM = 4;
function paint(){
  document.querySelectorAll('.t').forEach(e=>e.classList.toggle('on', e.dataset.t===sel));
  const used = new Set();
  TECHS.forEach(([n])=>{
    const sw = document.getElementById('sw-'+n), cl = document.getElementById('cl-'+n);
    if(pick[n]){ const [c,r]=pick[n];
      sw.style.backgroundPosition = (-c*8*ZOOM)+'px '+(-r*8*ZOOM)+'px';
      cl.textContent = c+','+r; used.add(c+','+r);
    } else { sw.style.backgroundPosition='9999px 9999px'; cl.textContent='--'; }
  });
  document.querySelectorAll('.c').forEach(e=>
    e.classList.toggle('used', used.has(e.dataset.c+','+e.dataset.r)));
  document.getElementById('done').textContent =
    Object.keys(pick).length + ' of ' + TECHS.length;
  out();
}
function out(){
  const lines = TECHS.map(([n])=> '    "'+n+'": '+(pick[n]?'('+pick[n][0]+', '+pick[n][1]+'),':'None,  # not chosen'));
  document.getElementById('out').value = 'TECH_ICON = {\\n' + lines.join('\\n') + '\\n}';
}
document.addEventListener('click', ev=>{
  const t = ev.target.closest('.t');
  if(t){ sel = t.dataset.t; paint(); return; }
  const c = ev.target.closest('.c');
  if(c){ pick[sel] = [ +c.dataset.c, +c.dataset.r ];
    localStorage.setItem('mbicons', JSON.stringify(pick));
    const i = TECHS.findIndex(x=>x[0]===sel);        /* advance, so a run of picks is quick */
    if(i>=0 && i+1<TECHS.length) sel = TECHS[i+1][0];
    paint(); }
});
document.getElementById('clear').onclick = ()=>{
  if(confirm('Clear every choice?')){ Object.keys(pick).forEach(k=>delete pick[k]);
    localStorage.removeItem('mbicons'); paint(); } };
document.getElementById('copy').onclick = ()=>{
  const t=document.getElementById('out'); t.select(); document.execCommand('copy'); };
paint();
""" % str([[n, lv] for n, lv in tl])

    html = ("<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>Motebox icon picker</title><style>%s\n.sw{%s}\n.c{%s}</style></head><body>"
            "<header><h1>Motebox &mdash; choose an icon for each technology</h1>"
            "<div class=\"hint\">Click a technology on the left, then click a cell. It advances to "
            "the next technology automatically, so you can work straight down the list. Green dots "
            "mark cells already in use. Choices are kept if you reload. "
            "<span class=\"count\" id=\"done\"></span></div></header>"
            "<div class=\"layout\"><div class=\"side\">%s</div><div class=\"main\">%s</div></div>"
            "<footer><button id=\"copy\">Copy the table</button>"
            "<button id=\"clear\">Clear</button>"
            "<textarea id=\"out\" spellcheck=\"false\"></textarea></footer>"
            "<script>%s</script></body></html>"
            % (CSS, bg_sw, bg, "".join(side), "".join(zones), js))
    open(OUT, "w").write(html)
    print("wrote %s  (%d cells, %d technologies)" % (OUT, (W // 8) * (H // 8), len(tl)))


if __name__ == "__main__":
    main()
