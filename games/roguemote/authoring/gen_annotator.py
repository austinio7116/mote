#!/usr/bin/env python3
"""
Generate a self-contained interactive sprite ANNOTATOR (Artifact-ready HTML).
Reuses the catalogue's tiles + my guesses as pre-fills; the developer corrects
category + name per tile (with bulk row/col/sheet apply + keyboard nav), then
Exports a JSON blob that feeds back into extract.py.  Output: /tmp/roguemote_annotator.html
"""
import os, io, json, base64
import gen_catalogue as gc          # SECTIONS, tile_datauri, COLORS
import inuse                        # what the GAME currently draws each tile as

# category id -> (label, hotkey, hue)
CATS = [
    ("player","Player","1",45), ("npc","NPC / townsfolk","2",210),
    ("enemy","Enemy","3",0), ("boss","Boss","4",320),
    ("animal","Animal","5",95), ("weapon","Weapon","6",25),
    ("armor","Armor","7",260), ("consumable","Consumable","8",145),
    ("treasure","Treasure","9",50), ("key","Key","0",190),
    ("tool","Tool","q",30), ("container","Container","w",170),
    ("furniture","Furniture","e",280), ("prop","Prop / decor","r",230),
    ("door","Door","t",15), ("floor","Terrain: floor","y",110),
    ("wall","Terrain: wall","u",20), ("feature","Terrain: feature","i",130),
    ("ui","UI / HUD","o",300), ("fx","FX / effect","p",340),
    ("font","Font glyph","[",200), ("unknown","Unknown","\\",0),
]
# --- multi-tile sprites -----------------------------------------------------
# Some sprites span several 8x8 tiles, so showing one card per tile makes you
# name the same creature four times (and lets the halves drift apart). Each
# entry is (col, row, w, h) of the TOP-LEFT tile; the annotator draws the whole
# block as one card and expands it back to per-tile rows on export.
# Every entry below was checked at 13x against source_tileset.png. Do NOT derive
# groups from the agent labels repeating across tiles -- that is how (40,33) got
# merged into a bogus 2x4: the agent called all 8 tiles "green medusa", but it is
# a medusa at rows 33-34 and a separate giant green insect at rows 35-36.
def _boss_groups():
    """All 17 bosses are 2x2; the region is a uniform grid over its inked tiles."""
    g = [(c, r, 2, 2) for r in (33, 35, 37) for c in (32, 34, 36, 38)]  # 4 wide x 3 bands
    g += [(c, 33, 2, 2) for c in (40, 42, 44, 46)]                      # top band only
    g += [(40, 35, 2, 2)]                                               # giant green insect
    return g

# NOTE: (24,0)-(25,1) is NOT a group. CATALOGUE.md calls it a "large NPC portrait"
# but it is four unrelated items -- scroll, arrow, small bomb, large bomb.
GROUPS = _boss_groups()
# every member tile -> its group, so non-anchor members can be skipped
GRP_AT = {(c + dx, r + dy): (c, r, w, h)
          for (c, r, w, h) in GROUPS for dy in range(h) for dx in range(w)}

# default category per section title (first token before " (")
SEC_CAT = {
    "Chests":"container","Stone furniture":"furniture","Faces · skulls · keys":"key",
    "Runes":"prop","Doors · gems · banners":"door","Light props & structures":"prop",
    "Trinkets & small nature":"prop","Weapons & potions":"weapon","Food":"consumable",
    "Treasure & ore":"treasure","Loot furniture & bones":"prop","Tools & wands":"tool",
    "Elemental weapons":"weapon","Guns":"weapon","Scroll · arrow · bombs":"consumable","Characters":"npc",
    "Animals & vermin":"animal","Monsters":"enemy","Crowns · armour · FX":"treasure",
    "Bosses":"boss","Boulders & mountains":"feature","Tiny UI icons (magenta strip)":"ui",
    "Arrows & gauges":"ui","Button prompts":"ui","Status · emotes · elements":"ui",
    "Rings, amulets & earrings":"treasure","Helms, hoods & shields":"armor",
    "Symbols":"ui","White furniture (top-down)":"furniture","Blueprint tiles":"ui",
    "Colour panels (terrain)":"ui","Purple brick wall (terrain)":"wall",
    "Stone-brick wall set (terrain)":"wall","Temple / aztec wall (terrain)":"wall",
    "Jungle grass & steps (terrain)":"floor","Cobblestone floors (terrain)":"floor",
    "Terrain edges (terrain)":"feature",
}

def build():
    # agent identifications (batches/out_*.json merged) override my earlier guesses,
    # and human corrections (apply_labels.py -> labels_human.json) override those.
    here = os.path.dirname(os.path.abspath(__file__))
    ai, human = {}, {}
    ap = os.path.join(here, "labels_ai.json")
    hp = os.path.join(here, "labels_human.json")
    if os.path.exists(ap): ai = json.load(open(ap))
    if os.path.exists(hp): human = json.load(open(hp))
    USES = inuse.build()
    tiles = []          # flat, in section order
    sections = []       # {title, blurb, ids:[...]}
    for (c0,r0,c1,r1,title,blurb,use,labeler) in gc.SECTIONS:
        defcat = SEC_CAT.get(title.split(" (")[0], "unknown")
        ids = []
        for r in range(r0,r1+1):
            for c in range(c0,c1+1):
                grp = GRP_AT.get((c,r))
                if grp and (c,r) != (grp[0],grp[1]):
                    continue                       # a member; its anchor draws the whole sprite
                w, h = (grp[2], grp[3]) if grp else (1, 1)
                uri, ink = gc.tile_datauri(c,r,w,h)
                if not ink: continue
                res = labeler(c,r)
                if res is None: continue
                name, _use, _q = res
                cat = defcat; conf = ""
                if title == "Characters" and c==32 and r==0: cat, name = "player","@ player glyph"
                a = ai.get(f"{c},{r}")
                if a:                              # prefer the agent label
                    cat = a.get("cat", cat); name = a.get("name") or name; conf = a.get("conf","")
                rv = 0
                hu = human.get(f"{c},{r}")
                if hu:                             # a human decision outranks everything
                    cat = hu.get("cat", cat); name = hu.get("name") or name
                    conf = "user" if hu.get("by") == "user" else "propagated"
                    rv = 1
                t = {"c":c,"r":r,"u":uri,"cat":cat,"name":name,"conf":conf,"rv":rv}
                # What the shipped game draws with this tile, so a wrong mapping
                # is visible on the card instead of only in the running game.
                inu = sorted(set(USES.get((c, r), [])))
                if grp:
                    inu = sorted({x for dy in range(h) for dx in range(w)
                                  for x in USES.get((c + dx, r + dy), [])})
                if inu: t["use"] = inu
                if grp:
                    # only inked members get a row on export -- no labelling blank tiles
                    t["m"] = [[c+dx, r+dy] for dy in range(h) for dx in range(w)
                              if gc.tile_datauri(c+dx, r+dy)[1]]
                    t["w"], t["h"] = w, h
                ids.append(len(tiles))
                tiles.append(t)
        if ids: sections.append({"t":title,"b":blurb,"ids":ids})
    # font glyphs individually (so they can be named too, but default font)
    fids=[]
    for i in range(128):
        cc, rr = 48+(i%16), (i//16)
        uri, ink = gc.tile_datauri(cc,rr)
        if not ink and i!=32: continue
        fids.append(len(tiles))
        tiles.append({"c":cc,"r":rr,"u":uri,"cat":"font","name":f"CP437 #{i}" + (" (space)" if i==32 else ""),"conf":"high","rv":0})
    if fids: sections.append({"t":"CP437 font","b":"256-glyph 8x8 CP437 font (codepoint = row*16 + col-48).","ids":fids})

    payload = {"cats":[[c[0],c[1],c[2],c[3]] for c in CATS],
               "tiles":tiles, "sections":sections}
    data_js = json.dumps(payload, separators=(",",":"))

    html = TEMPLATE.replace("/*__DATA__*/", "const APP = " + data_js + ";")
    out = "/tmp/roguemote_annotator.html"
    open(out,"w").write(html)
    print(f"wrote {out}: {len(tiles)} tiles, {len(sections)} sections, {len(CATS)} categories")

TEMPLATE = r"""<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Roguemote — sprite annotator</title>
<style>
:root{color-scheme:light dark;
 --bg:#f4f2ec;--panel:#fffdf8;--panel2:#efece3;--ink:#26242c;--muted:#6f6a7d;
 --line:#ddd8cc;--gold:#a9791a;--gold-ink:#7c580f;--torch:#c1471f;--cyan:#137a80;
 --sel:#2b6cb0;--chk-a:#d9d5cb;--chk-b:#c7c2b6;--catL:52%;}
@media (prefers-color-scheme:dark){:root{
 --bg:#111219;--panel:#191b24;--panel2:#20222e;--ink:#e9e6df;--muted:#9a96ac;
 --line:#2b2d3a;--gold:#f2b83a;--gold-ink:#f4c85f;--torch:#e8663c;--cyan:#6fd3d8;
 --sel:#6fb1ff;--chk-a:#2a2c38;--chk-b:#22242f;--catL:62%;}}
:root[data-theme="light"]{color-scheme:light;--bg:#f4f2ec;--panel:#fffdf8;--panel2:#efece3;--ink:#26242c;--muted:#6f6a7d;--line:#ddd8cc;--gold:#a9791a;--gold-ink:#7c580f;--torch:#c1471f;--cyan:#137a80;--sel:#2b6cb0;--chk-a:#d9d5cb;--chk-b:#c7c2b6;--catL:52%;}
:root[data-theme="dark"]{color-scheme:dark;--bg:#111219;--panel:#191b24;--panel2:#20222e;--ink:#e9e6df;--muted:#9a96ac;--line:#2b2d3a;--gold:#f2b83a;--gold-ink:#f4c85f;--torch:#e8663c;--cyan:#6fd3d8;--sel:#6fb1ff;--chk-a:#2a2c38;--chk-b:#22242f;--catL:62%;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;line-height:1.4}
.mono{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace}
header.top{position:sticky;top:0;z-index:20;background:color-mix(in srgb,var(--panel) 94%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:10px 16px}
.row1{display:flex;align-items:center;gap:14px;flex-wrap:wrap}
h1{margin:0;font-size:16px;letter-spacing:.02em}
h1 b{color:var(--gold)}
.progwrap{flex:1;min-width:160px;height:8px;background:var(--panel2);border-radius:99px;overflow:hidden;border:1px solid var(--line)}
.prog{height:100%;background:var(--cyan);width:0}
.pnum{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--muted)}
button,select,input{font:inherit;color:var(--ink)}
.btn{background:var(--panel2);border:1px solid var(--line);border-radius:6px;padding:4px 9px;font-size:12.5px;cursor:pointer}
.btn:hover{border-color:var(--gold)}
.btn.pri{background:var(--cyan);color:#04222a;border-color:transparent;font-weight:600}
.tools{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-top:8px;font-size:12.5px}
.tools .grp{display:flex;align-items:center;gap:5px;background:var(--panel2);border:1px solid var(--line);border-radius:7px;padding:4px 7px}
.tools label{color:var(--muted)}
.tools input[type=text]{background:var(--bg);border:1px solid var(--line);border-radius:5px;padding:3px 6px;width:150px}
.selinfo{color:var(--muted)}
.selinfo b{color:var(--ink);font-family:ui-monospace,Menlo,monospace}
.cats{display:flex;flex-wrap:wrap;gap:5px;margin-top:8px}
.cat{--h:0;font-size:11.5px;border:1px solid hsl(var(--h) 55% var(--catL));color:hsl(var(--h) 60% var(--catL));background:hsl(var(--h) 55% var(--catL)/.12);border-radius:99px;padding:2px 9px;cursor:pointer;user-select:none;white-space:nowrap}
.cat:hover{background:hsl(var(--h) 55% var(--catL)/.28)}
.cat kbd{font-family:ui-monospace,Menlo,monospace;opacity:.7;margin-right:4px}
nav.secs{display:flex;gap:5px;flex-wrap:wrap;margin-top:8px}
nav.secs a{font-size:11px;text-decoration:none;color:var(--muted);border:1px solid var(--line);border-radius:99px;padding:2px 8px}
nav.secs a:hover{color:var(--ink);border-color:var(--gold)}
main{max-width:1250px;margin:0 auto;padding:6px 16px 90px}
section{margin-top:24px;scroll-margin-top:180px}
.sh{display:flex;justify-content:space-between;align-items:baseline;gap:16px;border-bottom:1px solid var(--line);padding-bottom:8px;margin-bottom:12px}
.sh h2{margin:0;font-size:18px}
.sh .b{color:var(--muted);font-size:12.5px;max-width:60ch}
.sh .selall{font-size:11.5px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(132px,1fr));gap:8px}
.card{margin:0;background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden;display:flex;flex-direction:column;position:relative;--h:0}
.card::before{content:"";position:absolute;left:0;top:0;bottom:0;width:4px;background:hsl(var(--h) 60% var(--catL))}
.card.sel{outline:2px solid var(--sel);outline-offset:-2px}
.card.anchor{box-shadow:0 0 0 2px var(--gold) inset}
.card.hide{display:none}
.thumb{aspect-ratio:1/1;display:flex;align-items:center;justify-content:center;background-image:conic-gradient(var(--chk-a) 25%,var(--chk-b) 0 50%,var(--chk-a) 0 75%,var(--chk-b) 0);background-size:14px 14px;cursor:pointer}
.thumb img{width:74%;height:74%;object-fit:contain;image-rendering:pixelated}
.meta{padding:5px 6px 7px;display:flex;flex-direction:column;gap:4px;border-top:1px solid var(--line)}
.coord{font-family:ui-monospace,Menlo,monospace;font-size:10.5px;color:var(--gold-ink)}
.spn{color:var(--cyan);font-weight:700}          /* multi-tile sprite, drawn as one card */
.card.grp .thumb img{width:90%;height:90%}       /* give the bigger sprites more room */
.bdg{font-weight:700;cursor:help}
.bdg.user{color:var(--cyan)}
.bdg.propagated{color:var(--gold)}
.bdg.low{color:var(--torch)}
.card select{width:100%;font-size:11px;padding:2px;border:1px solid var(--line);border-radius:4px;background:var(--bg)}
.card input.nm{width:100%;font-size:12px;padding:3px 4px;border:1px solid var(--line);border-radius:4px;background:var(--bg)}
.card .use{display:flex;flex-wrap:wrap;gap:2px;margin-top:3px}
.card .use span{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:9.5px;line-height:1.35;
  padding:1px 4px;border-radius:3px;background:color-mix(in srgb,var(--gold) 16%,transparent);
  color:var(--gold-ink);border:1px solid color-mix(in srgb,var(--gold) 32%,transparent)}
.card.inuse::after{content:"";position:absolute;right:5px;top:5px;width:6px;height:6px;border-radius:50%;background:var(--gold)}
.card input.nm:focus,.card select:focus{outline:2px solid var(--sel);outline-offset:-1px}
.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--ink);color:var(--bg);padding:8px 14px;border-radius:8px;font-size:13px;opacity:0;transition:opacity .2s;z-index:50;pointer-events:none}
.toast.show{opacity:.95}
.sheet.open ~ .toast{bottom:auto;top:96px}   /* keep toasts clear of the open editor */
.help{color:var(--muted);font-size:12px;max-width:80ch;margin:14px 0 0}
.help kbd{font-family:ui-monospace,Menlo,monospace;background:var(--panel2);border:1px solid var(--line);border-radius:4px;padding:0 4px}
dialog{background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:10px;max-width:640px;width:92%}
dialog textarea{width:100%;height:220px;background:var(--bg);color:var(--ink);border:1px solid var(--line);border-radius:6px;font-family:ui-monospace,Menlo,monospace;font-size:12px;padding:8px}
.menu{display:none}
.jump{display:none;width:100%;margin-top:8px;padding:7px 8px;border:1px solid var(--line);border-radius:7px;background:var(--panel2)}

/* --- tap-to-edit bottom sheet (the mobile workhorse) --------------------- */
.sheet{position:fixed;left:0;right:0;bottom:0;z-index:40;background:var(--panel);
 border-top:1px solid var(--line);border-radius:16px 16px 0 0;padding:12px 14px calc(16px + env(safe-area-inset-bottom));
 box-shadow:0 -10px 34px rgba(0,0,0,.34);max-height:86vh;overflow-y:auto;
 transform:translateY(115%);transition:transform .22s ease;visibility:hidden}
.sheet.open{transform:none;visibility:visible}
@media (prefers-reduced-motion:reduce){.sheet{transition:none}}
.shead{display:flex;gap:12px;align-items:flex-start}
.shimg{width:76px;height:76px;flex:none;object-fit:contain;image-rendering:pixelated;border-radius:8px;
 background-image:conic-gradient(var(--chk-a) 25%,var(--chk-b) 0 50%,var(--chk-a) 0 75%,var(--chk-b) 0);background-size:16px 16px}
.sinfo{flex:1;min-width:0;display:flex;flex-direction:column;gap:6px}
.scoord{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--gold-ink)}
.scoord .lo{color:var(--torch);font-weight:700}
.snm{width:100%;font-size:16px;padding:9px 10px;border:1px solid var(--line);border-radius:8px;background:var(--bg)}
.shx{flex:none;min-width:40px;min-height:40px;font-size:15px;line-height:1}
.scats{display:grid;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:6px;margin-top:12px}
.scat{--h:0;font-size:12.5px;min-height:38px;display:flex;align-items:center;justify-content:center;text-align:center;
 border:1px solid hsl(var(--h) 55% var(--catL));color:hsl(var(--h) 60% var(--catL));
 background:hsl(var(--h) 55% var(--catL)/.12);border-radius:8px;padding:4px 6px;cursor:pointer;user-select:none}
.scat.on{background:hsl(var(--h) 60% var(--catL));color:var(--panel);font-weight:600;border-color:transparent}
.snav{display:grid;grid-template-columns:1fr 1.4fr 1fr;gap:8px;margin-top:12px}
.snav .btn,.wide{min-height:44px}
.wide{width:100%;margin-top:8px}

/* --- phones / narrow tablets --------------------------------------------- */
@media (max-width:720px){
  header.top{padding:8px 10px}
  h1{font-size:14px;white-space:nowrap}
  .wideonly{display:none}
  .row1{gap:8px;flex-wrap:nowrap}
  .progwrap{min-width:0}
  .pnum{font-size:11px;white-space:nowrap}
  .sh .selall{display:none}          /* Tools ▸ Select ▸ sheet covers this */
  .menu{display:inline-block}
  .drawer{display:none}
  .drawer.open{display:block}
  nav.secs{display:none}
  .jump{display:block}
  .tools{gap:6px}
  .tools input[type=text]{width:120px}
  .btn{min-height:36px;padding:6px 10px}
  /* re-state after .btn: same specificity, so source order has to win here */
  .snav .btn,.wide{min-height:44px}
  .scat{min-height:44px}
  .cat{min-height:34px;display:inline-flex;align-items:center;padding:2px 11px}
  .cat kbd{display:none}             /* no keyboard here */
  main{max-width:none;padding:6px 10px 96px}
  section{scroll-margin-top:104px}
  .sh{flex-direction:column;gap:6px}
  .sh .selall{align-self:flex-start}
  .grid{grid-template-columns:repeat(auto-fill,minmax(84px,1fr));gap:6px}
  /* the card becomes a tap target: edit happens in the sheet, not inline */
  .card select{display:none}
  .card input.nm{pointer-events:none;border:0;background:transparent;padding:1px 0;font-size:11px;
   white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .thumb{pointer-events:none}
  .meta{padding:4px 5px 5px;gap:1px}
  .coord{font-size:10px}
  .help{padding:0 10px}
  dialog{width:96%}
  dialog textarea{height:160px}
}
</style>

<header class="top">
 <div class="row1">
   <h1><b>Roguemote</b><span class="wideonly"> sprite annotator</span></h1>
   <div class="progwrap"><div class="prog" id="prog"></div></div>
   <span class="pnum" id="pnum">0 / 0 reviewed</span>
   <button class="btn pri" id="export">Export</button>
   <button class="btn" id="expfix" title="Only the tiles you changed, one line each">Corrections</button>
   <button class="btn menu" id="menu" aria-expanded="false">Tools</button>
 </div>
 <div class="drawer" id="drawer">
   <div class="tools">
     <span class="selinfo">Selected: <b id="selcount">0</b></span>
     <div class="grp"><label>Name selection</label><input type="text" id="bulkname" placeholder="e.g. skeleton {i}"><button class="btn" id="applyname">Apply</button></div>
     <div class="grp"><label>Select</label>
       <button class="btn" id="selrow">row</button>
       <button class="btn" id="selcol">col</button>
       <button class="btn" id="selsheet">sheet</button>
       <button class="btn" id="selnone">none</button></div>
     <label><input type="checkbox" id="onlyunrev"> only unreviewed</label>
     <label><input type="checkbox" id="onlyused"> only in-game</label>
     <label><input type="checkbox" id="onlyfree"> only unused</label>
     <div class="grp"><button class="btn" id="import">Import</button>
     <button class="btn" id="rawdump" title="The untouched save this page found when it loaded">Raw backup</button>
     <button class="btn" id="reset">Reset</button></div>
   </div>
   <div class="cats" id="cats"></div>
   <nav class="secs" id="secnav"></nav>
   <select class="jump" id="jump"><option value="">Jump to a sheet…</option></select>
 </div>
</header>
<div id="note" style="display:none;max-width:1250px;margin:10px auto 0;padding:10px 14px;
 border:1px solid var(--gold);border-radius:8px;background:color-mix(in srgb,var(--gold) 12%,transparent);
 color:var(--gold-ink);font-size:13px"></div>
<main id="main"></main>
<details class="help" style="max-width:1250px;margin:10px auto;padding:0 16px">
 <summary style="cursor:pointer;padding:6px 0">How this works</summary>
 <p style="margin:4px 0 0">
 Labels start as a <b>detailed identification pass by vision agents</b> (each studied one zoomed
 section), with your own corrections layered on top. The mark after a coordinate says where a label
 came from: <b style="color:var(--torch)">?</b> the agent was unsure &mdash; check these first;
 <b style="color:var(--gold)">≈</b> carried over from a colour sibling you corrected;
 <b style="color:var(--cyan)">✓</b> you named it. Everything autosaves in your browser; hit
 <b>Export</b> when done.
 </p>
 <p style="margin:8px 0 0">
 <b>Gold chips</b> under a card are what the <b>shipped game currently draws with that tile</b>
 &mdash; <span class="mono">CLASS:</span>, <span class="mono">monster:</span>,
 <span class="mono">item(ring):</span>, <span class="mono">BOSS:</span>,
 <span class="mono">shop:</span>, <span class="mono">terrain:</span> &mdash; read straight out of
 the content tables in <span class="mono">src/</span>. A card with a gold dot is in use; a card
 without one is art the game has never drawn. Use <b>only in-game</b> to audit what is mapped and
 <b>only unused</b> to find something better to map it to. If a chip is wrong, the label you type
 is the correction &mdash; the tile is what it is, and the table follows the tile.
 </p>
 <p style="margin:8px 0 0">
 A few tiles sit in two overlapping subsheets and so appear on <b>two cards</b> (the editor tells you
 when). Editing either one now updates both, so a stale twin can't overwrite your work on export.
 </p>
 <p style="margin:8px 0 0">
 <b>On a phone:</b> tap any sprite to open the editor. Tap a category to stamp it (that also marks it
 reviewed), or <b>✓ Looks right</b> to accept the guess as-is and move on. <b>Prev/Next</b> walk the
 sheet in order; <b>Skip to next unreviewed</b> jumps ahead. <b>Tools</b> (top right) opens bulk
 select/name, the sheet jump menu, and Import/Reset.
 </p>
 <p style="margin:8px 0 0">
 <b>On a desktop:</b> click a sprite to select it (the <b style="color:var(--gold)">gold</b> anchor);
 <kbd>&larr;&uarr;&darr;&rarr;</kbd> move · <kbd>Shift</kbd>+click or <kbd>Shift</kbd>+arrows range-select ·
 press a category <b>hotkey</b> to stamp the selection · <kbd>Enter</kbd> edit the name,
 <kbd>Enter</kbd> again jumps to the next · <kbd>N</kbd> next unreviewed. Use <b>Select row/col/sheet</b> +
 a category chip or the <b>Name selection</b> box (<span class="mono">{i}</span> = auto-number) for whole
 families at once.
 </p>
</details>
<div class="sheet" id="sheet" role="dialog" aria-label="Edit sprite">
 <div class="shead">
   <img class="shimg" id="shimg" alt="">
   <div class="sinfo">
     <div class="scoord" id="shcoord"></div>
     <input class="snm" id="shname" type="text" placeholder="name" autocomplete="off" autocapitalize="off">
   </div>
   <button class="btn shx" id="shclose" aria-label="Close editor">&#10005;</button>
 </div>
 <div class="scats" id="shcats"></div>
 <div class="snav">
   <button class="btn" id="shprev">&lsaquo; Prev</button>
   <button class="btn pri" id="shok">&#10003; Looks right</button>
   <button class="btn" id="shnext">Next &rsaquo;</button>
 </div>
 <button class="btn wide" id="shunrev">Skip to next unreviewed</button>
</div>
<div class="toast" id="toast"></div>
<dialog id="iodlg"><form method="dialog"><h3 id="iotitle" style="margin:0 0 8px"></h3>
 <textarea id="iotext"></textarea>
 <div style="display:flex;gap:8px;justify-content:flex-end;margin-top:10px;flex-wrap:wrap">
   <button class="btn" value="cancel">Close</button>
   <button class="btn" id="iosave" type="button">Save file</button>
   <button class="btn" id="iocopy" type="button">Copy to clipboard</button>
   <button class="btn pri" id="iook" value="ok" type="button">Load</button></div></form></dialog>

<script>
/*__DATA__*/
const KEY="roguemote_annot_v1";    // legacy: a positional array, one slot per card
const KEY2="roguemote_annot_v2";   // current: keyed by "c,r"
const BACKUP="roguemote_annot_v1_backup";  // write-once copy of the legacy save
const CATBY={}; APP.cats.forEach(c=>CATBY[c[0]]=c);
const HOTKEY={}; APP.cats.forEach(c=>HOTKEY[c[2]]=c[0]);
// state: per tile {cat,name,rev}. seed from APP.tiles then overlay saved.
//
// v1 stored a flat array indexed by card, and loaded it only when the length
// matched exactly. That made the save silently unreadable the moment a section
// was added or resized -- an hour of labelling orphaned by a page rebuild. v2 is
// keyed by the tile's own (col,row), which no section change can invalidate.
let ST = APP.tiles.map(t=>({cat:t.cat,name:t.name,rev:!!t.rv}));
let loadNote="";

/* NEVER destroy a save we could not read. The first thing that happens is an
 * immutable, write-once copy of whatever v1 holds; every recovery path below
 * reads from that and nothing overwrites it. An unreadable save is a save to be
 * recovered later, not a slot to reuse. */
let rawV1=null;
try{ rawV1=localStorage.getItem(KEY);
     if(rawV1 && !localStorage.getItem(BACKUP)) localStorage.setItem(BACKUP, rawV1);
}catch(e){}

function applyByCoord(map){
  let n=0;
  APP.tiles.forEach((t,i)=>{const v=map[t.c+","+t.r];
    if(v){ST[i]={cat:v.cat,name:v.name,rev:!!v.rev};n++;}});
  return n;
}

let loaded=0, recovered=false;
try{const s2=JSON.parse(localStorage.getItem(KEY2)||"null");
 if(s2&&typeof s2==="object"&&!Array.isArray(s2)) loaded=applyByCoord(s2);
}catch(e){}

if(!loaded && rawV1){
  let arr=null; try{arr=JSON.parse(rawV1);}catch(e){}
  if(Array.isArray(arr)){
    if(arr.length===ST.length){                 // same build: exact restore
      ST=arr; loaded=ST.length;
    }else{
      /* A different build wrote this. Sections are only ever appended or
       * resized, so card order is identical up to the first one that moved:
       * restore the common prefix and say so rather than silently starting
       * over. Anything past the seam is still in the backup. */
      const n=Math.min(arr.length, ST.length);
      for(let i=0;i<n;i++) if(arr[i]) ST[i]={cat:arr[i].cat,name:arr[i].name,rev:!!arr[i].rev};
      loaded=n; recovered=true;
      loadNote="Recovered "+n+" of "+arr.length+" labels from an earlier build of this page"
             + " ("+ST.length+" cards now). Export a diff before editing further.";
    }
  }
}
if(loaded) writeSave();      // mirror into the coordinate-keyed storelet anchor=0; const sel=new Set();
// Overlapping subsheets (props x trinkets, grass x temple) show the SAME (c,r)
// tile on two cards. Edits have to hit every card for a tile or the twin keeps
// a stale label and silently wins on export.
const TWINS={}; APP.tiles.forEach((t,i)=>{const k=t.c+','+t.r;(TWINS[k]=TWINS[k]||[]).push(i);});
function withTwins(ids){const o=new Set();
  ids.forEach(i=>TWINS[APP.tiles[i].c+','+APP.tiles[i].r].forEach(j=>o.add(j)));return [...o];}

const main=document.getElementById('main');
const cardEl=[]; // index -> element
function catHue(id){return CATBY[id]?CATBY[id][3]:0;}
// provenance marker: yours, carried to a colour sibling, or still a guess
const BADGE={user:['✓','you named this'],propagated:['≈','carried over from a colour sibling — check it'],
             low:['?','agent was unsure — check it first']};
function badge(conf){const b=BADGE[conf];return b?` <span class="bdg ${conf}" title="${b[1]}">${b[0]}</span>`:'';}
APP.sections.forEach((s,si)=>{
  const sec=document.createElement('section'); sec.id='s'+si;
  sec.innerHTML=`<div class="sh"><div><h2>${s.t}</h2><div class="b">${s.b}</div></div>
    <button class="btn selall" data-si="${si}">select sheet</button></div>`;
  const g=document.createElement('div'); g.className='grid';
  s.ids.forEach(id=>{
    const t=APP.tiles[id], st=ST[id];
    const fig=document.createElement('figure'); fig.className='card'+(t.w?' grp':'')+(t.use?' inuse':''); fig.dataset.id=id;
    fig.style.setProperty('--h',catHue(st.cat));
    fig.innerHTML=`<div class="thumb"><img src="${t.u}" alt=""></div>
      <div class="meta"><span class="coord">${t.c},${t.r}${t.w?` <span class="spn">${t.w}×${t.h}</span>`:''}${badge(t.conf)}</span>
      <select class="catsel">${APP.cats.map(c=>`<option value="${c[0]}">${c[1]}</option>`).join('')}</select>
      <input class="nm" type="text" value="">
      ${t.use?`<div class="use">${t.use.map(u=>`<span>${u.replace(/&/g,'&amp;').replace(/</g,'&lt;')}</span>`).join('')}</div>`:''}</div>`;
    g.appendChild(fig); cardEl[id]=fig;
    fig.querySelector('.catsel').value=st.cat;
    fig.querySelector('.nm').value=st.name;
    fig.querySelector('.thumb').addEventListener('mousedown',e=>{e.preventDefault();pick(id,e);});
    fig.querySelector('.catsel').addEventListener('change',e=>{setCat([id],e.target.value);});
    fig.querySelector('.nm').addEventListener('input',e=>{setName([id],e.target.value);});
    fig.querySelector('.nm').addEventListener('keydown',e=>{
      if(e.key==='Enter'){e.preventDefault();ST[id].rev=true;const nx=id+1;if(cardEl[nx]){pick(nx);cardEl[nx].querySelector('.nm').focus();}}
      if(e.key==='Escape'){e.target.blur();}
    });
    // touch layout: the whole card is one tap target, editing happens in the sheet
    fig.addEventListener('click',e=>{if(isMobile()){e.preventDefault();openSheet(id);}});
  });
  sec.appendChild(g); main.appendChild(sec);
});

// category palette + section nav
const catsBox=document.getElementById('cats');
APP.cats.forEach(c=>{const b=document.createElement('span');b.className='cat';b.style.setProperty('--h',c[3]);
  b.innerHTML=`<kbd>${c[2]==='\\'?'\\':c[2]}</kbd>${c[1]}`;b.title='hotkey: '+c[2];
  b.addEventListener('click',()=>{setCat(selArr(),c[0]);});catsBox.appendChild(b);});
const secnav=document.getElementById('secnav');
APP.sections.forEach((s,si)=>{const a=document.createElement('a');a.href='#s'+si;a.textContent=s.t;secnav.appendChild(a);});

function selArr(){return sel.size?[...sel]:(anchor!=null?[anchor]:[]);}
function pick(id,e){
  if(e&&e.shiftKey&&anchor!=null){const a=Math.min(anchor,id),b=Math.max(anchor,id);
    for(let i=a;i<=b;i++)if(cardEl[i])sel.add(i);}
  else if(e&&(e.metaKey||e.ctrlKey)){sel.has(id)?sel.delete(id):sel.add(id);anchor=id;}
  else {sel.clear();sel.add(id);anchor=id;}
  render();
}
function setCat(ids,cat){const n=ids.length;
  withTwins(ids).forEach(i=>{ST[i].cat=cat;ST[i].rev=true;cardEl[i].style.setProperty('--h',catHue(cat));cardEl[i].querySelector('.catsel').value=cat;});
  save();render();toast(n+' → '+CATBY[cat][1]);}
function setName(ids,nm){withTwins(ids).forEach(i=>{ST[i].name=nm;ST[i].rev=true;
  const el=cardEl[i].querySelector('.nm');           // don't fight the field being typed in
  if(el!==document.activeElement) el.value=nm;});save();}
function applyName(){const pat=document.getElementById('bulkname').value;const ids=selArr();if(!ids.length||!pat)return;
  ids.forEach((i,k)=>setName([i],pat.replace('{i}',k+1)));render();toast('named '+ids.length);}

function render(){
  const only=document.getElementById('onlyunrev').checked;
  const usedOnly=document.getElementById('onlyused').checked;
  const freeOnly=document.getElementById('onlyfree').checked;
  cardEl.forEach((el,i)=>{el.classList.toggle('sel',sel.has(i));el.classList.toggle('anchor',i===anchor&&!sel.size||sel.size&&i===anchor);
    const used=!!APP.tiles[i].use;
    el.classList.toggle('hide',(only&&ST[i].rev)||(usedOnly&&!used)||(freeOnly&&used));});
  document.getElementById('selcount').textContent=sel.size||(anchor!=null?1:0);
  const rev=ST.filter(s=>s.rev).length;
  document.getElementById('prog').style.width=(100*rev/ST.length)+'%';
  document.getElementById('pnum').textContent=isMobile()?rev+'/'+ST.length:rev+' / '+ST.length+' reviewed';
  if(sheetId!=null) fillSheet();          // keep the open editor in step with bulk edits
}
let saveT; function save(){clearTimeout(saveT);saveT=setTimeout(writeSave,250);}
function writeSave(){
  const o={};
  APP.tiles.forEach((t,i)=>{(t.m||[[t.c,t.r]]).forEach(m=>{o[m[0]+","+m[1]]=ST[i];});});
  try{localStorage.setItem(KEY2,JSON.stringify(o));
      localStorage.setItem(KEY,JSON.stringify(ST));}catch(e){}
}
writeSave();   // mirror whatever we just loaded into v2 before any edit happens
let toastT;function toast(m){const t=document.getElementById('toast');t.textContent=m;t.classList.add('show');clearTimeout(toastT);toastT=setTimeout(()=>t.classList.remove('show'),1200);}

// section-local helpers
function sectionOf(id){return APP.sections.find(s=>s.ids.includes(id));}
document.querySelectorAll('.selall').forEach(b=>b.addEventListener('click',()=>{
  const s=APP.sections[+b.dataset.si];sel.clear();s.ids.forEach(i=>sel.add(i));anchor=s.ids[0];render();}));
document.getElementById('selrow').onclick=()=>{if(anchor==null)return;const r=APP.tiles[anchor].r;const s=sectionOf(anchor);sel.clear();s.ids.forEach(i=>{if(APP.tiles[i].r===r)sel.add(i);});render();};
document.getElementById('selcol').onclick=()=>{if(anchor==null)return;const c=APP.tiles[anchor].c;const s=sectionOf(anchor);sel.clear();s.ids.forEach(i=>{if(APP.tiles[i].c===c)sel.add(i);});render();};
document.getElementById('selsheet').onclick=()=>{if(anchor==null)return;const s=sectionOf(anchor);sel.clear();s.ids.forEach(i=>sel.add(i));render();};
document.getElementById('selnone').onclick=()=>{sel.clear();render();};
document.getElementById('applyname').onclick=applyName;
document.getElementById('onlyunrev').onchange=render;
document.getElementById('onlyused').onchange=render;
document.getElementById('onlyfree').onchange=render;

// keyboard
document.addEventListener('keydown',e=>{
  const tag=(e.target.tagName||'').toLowerCase();
  if(tag==='input'||tag==='textarea'||tag==='select')return;
  if(e.key in HOTKEY){setCat(selArr(),HOTKEY[e.key]);e.preventDefault();return;}
  if(e.key==='Enter'){if(anchor!=null){cardEl[anchor].querySelector('.nm').focus();e.preventDefault();}return;}
  if(e.key==='n'||e.key==='N'){const nx=ST.findIndex((s,i)=>!s.rev&&i>anchor);const j=nx<0?ST.findIndex(s=>!s.rev):nx;if(j>=0){pick(j);cardEl[j].scrollIntoView({block:'center'});}return;}
  const cols=colsPerRow();let d=0;
  if(e.key==='ArrowRight')d=1;else if(e.key==='ArrowLeft')d=-1;
  else if(e.key==='ArrowDown')d=cols;else if(e.key==='ArrowUp')d=-cols;else return;
  e.preventDefault();let nx=(anchor==null?0:anchor+d);if(nx<0||nx>=cardEl.length)return;
  if(e.shiftKey){sel.add(anchor);sel.add(nx);}else sel.clear();
  anchor=nx;if(!e.shiftKey){} render();cardEl[nx].scrollIntoView({block:'nearest'});
});
function colsPerRow(){const g=cardEl[anchor]?cardEl[anchor].parentElement:main.querySelector('.grid');
  if(!g)return 1;const s=getComputedStyle(g);return s.gridTemplateColumns.split(' ').length;}

// export / import / reset
const dlg=document.getElementById('iodlg');
/* The DIFF export: only tiles whose label no longer matches the one baked into
 * this page, one line of "col,row|cat|name" each.
 *
 * The full JSON export is one object per tile over fourteen hundred tiles. It
 * does not survive a phone clipboard and it did not survive being pasted into a
 * chat message -- one arrived truncated a third of the way through. A diff of an
 * afternoon's labelling is a few hundred lines. apply_labels.py reads both. */
function changedTiles(){
  const out={};
  APP.tiles.forEach((t,i)=>{
    const s=ST[i];
    if(s.cat===t.cat && s.name===t.name) return;   // untouched, or confirmed as-is
    (t.m||[[t.c,t.r]]).forEach(m=>{out[m[0]+','+m[1]]=s.cat+'|'+s.name;});
  });
  return out;
}
let ioFile=null;                                   // what "Save file" writes
function showIO(title, text, filename){
  ioFile={name:filename, text:text};
  document.getElementById('iotitle').textContent=title;
  document.getElementById('iotext').value=text;
  document.getElementById('iook').style.display='none';
  document.getElementById('iocopy').style.display='';
  document.getElementById('iosave').style.display='';
  closeSheet(); dlg.showModal();
}
if(loadNote){const n=document.getElementById('note');n.textContent=loadNote;n.style.display='';}

/* The legacy save, verbatim, whatever state this page managed to make of it.
 * If every clever path above got it wrong, this is still the bytes the browser
 * had, and it can be pasted or saved out unchanged. */
document.getElementById('rawdump').onclick=()=>{
  let raw=null; try{raw=localStorage.getItem(BACKUP)||localStorage.getItem(KEY);}catch(e){}
  if(!raw){toast('no earlier save found in this browser');return;}
  showIO('Raw backup ('+raw.length+' bytes) — Save file', raw, 'roguemote_rawsave.json');
};

document.getElementById('expfix').onclick=()=>{
  const ch=changedTiles();
  const keys=Object.keys(ch).sort((a,b)=>{const[x,y]=a.split(',').map(Number),[p,q]=b.split(',').map(Number);
    return y-q||x-p;});
  const rev=ST.filter(s=>s.rev).length;
  const txt='ROGUEMOTE-DIFF v1 '+keys.length+' changed of '+rev+' reviewed\n'
          + keys.map(k=>k+'|'+ch[k]).join('\n');
  showIO('Changed labels ('+keys.length+') — Save file, or Copy', txt, 'roguemote_diff.txt');
};
/* Saving goes through the host, which shows the viewer a confirmation. An
 * anchor with a Blob URL -- what this used to do -- is silently swallowed inside
 * a sandboxed frame on a phone, which looks exactly like nothing happening. */
document.getElementById('iosave').onclick=async ()=>{
  if(!ioFile) return;
  const dl = window.claude && window.claude.downloads;
  if(dl){
    try{ await dl.save({filename:ioFile.name, data:ioFile.text}); toast('saved '+ioFile.name); }
    catch(e){ const c=e&&e.code;
      toast(c==='declined'?'save cancelled':c==='too_large'?'file too large':'save failed ('+(c||'?')+')'); }
    return;
  }
  try{ const b=new Blob([ioFile.text],{type:'text/plain'});
       const a=document.createElement('a'); a.href=URL.createObjectURL(b);
       a.download=ioFile.name; a.click(); toast('saved'); }
  catch(e){ toast('saving is not available here — use Copy'); }
};
document.getElementById('export').onclick=()=>{
  // a multi-tile sprite is ONE card here but expands back to one row per 8x8
  // tile, so labels_ai/labels_human stay keyed by "c,r" and nothing downstream
  // has to know groups exist.
  const out={version:1,source:"simple-roguelike-tileset v0.16 CC0",tiles:[]};
  APP.tiles.forEach((t,i)=>{(t.m||[[t.c,t.r]]).forEach(m=>
    out.tiles.push({c:m[0],r:m[1],cat:ST[i].cat,name:ST[i].name,rev:ST[i].rev}));});
  const txt=JSON.stringify(out,null,1);
  showIO("Full export ("+out.tiles.length+" tiles) — Save file", txt, 'roguemote_labels.json');
};
document.getElementById('iocopy').onclick=()=>{const ta=document.getElementById('iotext');
  ta.select();ta.setSelectionRange(0,ta.value.length);      // iOS needs the explicit range
  navigator.clipboard.writeText(ta.value).then(()=>toast('copied to clipboard'),
    ()=>{try{document.execCommand('copy');toast('copied');}catch(e){toast('select the text and copy');}});};
document.getElementById('import').onclick=()=>{document.getElementById('iotitle').textContent="Import — paste a previously exported JSON";
  document.getElementById('iotext').value='';document.getElementById('iook').style.display='';
  document.getElementById('iocopy').style.display='none';
  document.getElementById('iosave').style.display='none';closeSheet();dlg.showModal();};
document.getElementById('iook').onclick=()=>{try{const o=JSON.parse(document.getElementById('iotext').value);
  const by={};(o.tiles||o).forEach(t=>by[t.c+','+t.r]={cat:t.cat,name:t.name,rev:t.rev!==false});
  APP.tiles.forEach((t,i)=>{const k=by[t.c+','+t.r];if(k){ST[i].cat=k.cat;ST[i].name=k.name;ST[i].rev=k.rev;
    cardEl[i].querySelector('.catsel').value=k.cat;cardEl[i].querySelector('.nm').value=k.name;cardEl[i].style.setProperty('--h',catHue(k.cat));}});
  save();render();dlg.close();toast('imported');}catch(e){alert('Bad JSON: '+e.message);}};
document.getElementById('reset').onclick=()=>{if(confirm('Discard edits made in this browser and go back to the labels baked into the page?')){
  ST=APP.tiles.map(t=>({cat:t.cat,name:t.name,rev:!!t.rv}));localStorage.removeItem(KEY);
  APP.tiles.forEach((t,i)=>{cardEl[i].querySelector('.catsel').value=ST[i].cat;cardEl[i].querySelector('.nm').value=ST[i].name;cardEl[i].style.setProperty('--h',catHue(ST[i].cat));});closeSheet();render();}};

// --- touch layout: header drawer + sheet jump menu ---------------------------
function isMobile(){return window.matchMedia('(max-width:720px)').matches;}
window.matchMedia('(max-width:720px)').addEventListener('change',()=>render());   // rotation
const drawer=document.getElementById('drawer'), menuBtn=document.getElementById('menu');
menuBtn.onclick=()=>{const on=drawer.classList.toggle('open');menuBtn.setAttribute('aria-expanded',on);};
const jump=document.getElementById('jump');
APP.sections.forEach((s,si)=>{const o=document.createElement('option');o.value='s'+si;o.textContent=s.t;jump.appendChild(o);});
jump.onchange=()=>{const el=document.getElementById(jump.value);if(el)el.scrollIntoView({block:'start'});
  jump.value='';drawer.classList.remove('open');menuBtn.setAttribute('aria-expanded',false);};

// --- tap-to-edit sheet -------------------------------------------------------
let sheetId=null;
const sheetEl=document.getElementById('sheet'), shName=document.getElementById('shname');
const shCats=document.getElementById('shcats');
APP.cats.forEach(c=>{const b=document.createElement('span');b.className='scat';b.dataset.cat=c[0];
  b.style.setProperty('--h',c[3]);b.textContent=c[1];
  b.addEventListener('click',()=>{if(sheetId==null)return;setCat([sheetId],c[0]);fillSheet();});
  shCats.appendChild(b);});

function openSheet(id){sheetId=id;anchor=id;sel.clear();sel.add(id);
  sheetEl.classList.add('open');fillSheet();render();}
function closeSheet(){sheetId=null;sheetEl.classList.remove('open');}
function fillSheet(){
  if(sheetId==null)return;
  const t=APP.tiles[sheetId], st=ST[sheetId];
  document.getElementById('shimg').src=t.u;
  const NOTE={user:'you named this',propagated:'carried over from a colour sibling — check it',
              low:'<span class="lo">agent was unsure</span>',med:'agent: medium confidence',
              high:'agent: high confidence'};
  const twins=TWINS[t.c+','+t.r].length;
  document.getElementById('shcoord').innerHTML=(t.w?'sprite ':'tile ')+t.c+','+t.r
    +(t.w?' · <span class="spn">'+t.w+'×'+t.h+' tiles</span> ('+t.m.length+' rows on export)':'')
    +(NOTE[t.conf]?' · '+NOTE[t.conf]:'')+(st.rev?' · reviewed':'')
    +(twins>1?' · <b>shown on '+twins+' cards</b> (edits sync)':'');
  if(document.activeElement!==shName) shName.value=st.name;   // don't clobber live typing
  [...shCats.children].forEach(el=>el.classList.toggle('on',el.dataset.cat===st.cat));
}
function sheetGo(id){if(!cardEl[id])return;sheetId=id;anchor=id;sel.clear();sel.add(id);
  fillSheet();render();cardEl[id].scrollIntoView({block:'center'});}
shName.addEventListener('input',()=>{if(sheetId==null)return;setName([sheetId],shName.value);render();});
shName.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();shName.blur();}});
document.getElementById('shclose').onclick=closeSheet;
document.getElementById('shprev').onclick=()=>sheetGo(sheetId-1);
document.getElementById('shnext').onclick=()=>sheetGo(sheetId+1);
document.getElementById('shok').onclick=()=>{if(sheetId==null)return;
  withTwins([sheetId]).forEach(i=>ST[i].rev=true);save();
  if(cardEl[sheetId+1])sheetGo(sheetId+1);else{render();toast('end of the list');}};
document.getElementById('shunrev').onclick=()=>{
  const from=sheetId==null?-1:sheetId;
  let j=ST.findIndex((s,i)=>!s.rev&&i>from); if(j<0) j=ST.findIndex(s=>!s.rev);
  if(j<0){toast('everything reviewed');return;} sheetGo(j);};

render();
</script>
"""

if __name__ == "__main__":
    build()
