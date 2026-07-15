#!/usr/bin/env python3
"""Generate docs/redmote-guide.html — a full, self-contained player's guide.
Run from anywhere in the repo:  python3 games/redmote/assets/gen_guide.py"""
import os, base64
# repo root = three levels up from games/redmote/assets/
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))

def _b64(p):
    with open(os.path.join(ROOT, p), "rb") as f:
        return "data:image/png;base64," + base64.b64encode(f.read()).decode()

ICON  = _b64("games/redmote/icon.png")
SHOT1 = _b64("docs/img/gallery/redmote-1.png")
SHOT2 = _b64("docs/img/gallery/redmote-2.png")
SHOT3 = _b64("docs/img/gallery/redmote-3.png")

# ---- accurate data pulled from games/redmote/src/game.c ----------------------
# units: (name, full, cost, hp, speed, range, rof, dmg, weapon, armour, role, prereq)
UNITS = [
    ("Rifleman", "RIFL", 100, 45, 26, 26, 0.65, 8, "MG", "Infantry", "Cheap anti-infantry line trooper", "Barracks"),
    ("Rocketeer", "RCKT", 300, 50, 24, 38, 1.90, 24, "Rocket", "Infantry", "Anti-tank & the only infantry that hits aircraft", "Barracks"),
    ("Flamer", "FLAM", 200, 55, 26, 17, 1.30, 20, "Flame", "Infantry", "Short-range infantry shredder (130% vs troops)", "Barracks"),
    ("Harvester", "HARV", 800, 350, 21, 0, 0, 0, "—", "Heavy", "Unarmed. Mines ore and crystal, banks credits", "War Factory + Refinery"),
    ("Light Tank", "LTNK", 600, 160, 33, 32, 1.10, 18, "Cannon", "Light", "Fast raider; rotating turret, great vs light armour", "War Factory"),
    ("Heavy Tank", "HTNK", 950, 300, 23, 34, 1.45, 27, "Cannon", "Heavy", "The main battle line — twin cannons, thick armour", "War Factory + Radar"),
    ("Artillery", "ARTY", 700, 90, 19, 64, 3.20, 42, "Arty", "Light", "Longest range, arcing splash; fragile up close (20px min range)", "War Factory + Radar"),
    ("Tesla Tank", "TSLA", 1200, 180, 25, 30, 2.20, 46, "Tesla", "Heavy", "Ignores all armour — full damage to everything", "War Factory + Tech Center"),
    ("Gunship", "HELI", 1000, 140, 46, 30, 0.55, 11, "Rocket", "Air", "Flies over any terrain; fast rocket pods", "Helipad"),
]
# buildings: (name, full, w, h, cost, hp, power, role, prereq)
BLD = [
    ("Construction Yard", "YARD", 3, 3, 2500, 1000, 0, "The base core — everything is built from here", "—"),
    ("Power Plant", "POW", 2, 2, 300, 300, +100, "Supplies power; build these first and often", "Construction Yard"),
    ("Ore Refinery", "REF", 3, 2, 1500, 600, -30, "Ore drop-off — ships with a free Harvester", "Power Plant"),
    ("Barracks", "RAX", 2, 2, 400, 500, -20, "Trains all infantry", "Power Plant"),
    ("War Factory", "FACT", 3, 2, 1800, 700, -30, "Builds all vehicles", "Ore Refinery"),
    ("Radar Dome", "RDR", 2, 2, 1000, 500, -40, "Unlocks the minimap and higher-tech vehicles", "Ore Refinery"),
    ("Helipad", "PAD", 2, 2, 1200, 450, -30, "Builds Gunships", "Radar + War Factory"),
    ("Tech Center", "TECH", 2, 2, 1500, 400, -60, "Top tech — unlocks the Tesla Tank & Tesla Coil", "Radar + War Factory"),
    ("Pillbox", "PILL", 1, 1, 400, 350, 0, "Cheap MG defence — mows down infantry", "Barracks"),
    ("Gun Turret", "GUN", 1, 1, 600, 400, 0, "Cannon defence — stops tanks", "War Factory"),
    ("Tesla Coil", "COIL", 1, 1, 1200, 350, -50, "Devastating tesla defence — needs power to fire", "Tech Center"),
]
# damage matrix DMUL[weapon][armour] %, armour order below
ARMOURS = ["Infantry", "Light", "Heavy", "Building", "Air"]
DMG = {
    "MG":     [100, 55, 30, 25, 60],
    "Cannon": [45, 100, 80, 70, 0],
    "Rocket": [35, 90, 100, 90, 100],
    "Flame":  [130, 55, 35, 90, 0],
    "Tesla":  [100, 100, 100, 100, 100],
    "Arty":   [110, 90, 70, 120, 0],
}
# missions: (n, name, brief, your_start, enemy, teaches)
MISSIONS = [
    ("First Blood", "No base at all — you command a fixed strike force. March across the map and raze the enemy outpost.",
     "12-unit force, no base, field radar", "A small guarded outpost", "Selection, movement, attacking"),
    ("Foothold", "Your first base. Build a Power Plant and a Refinery, let the harvester fund you, then train a force and crush the guard post.",
     "Construction Yard, $3000, tier-1 tech", "An outpost with a squad", "Power, refineries, harvesters, production"),
    ("Hold the Line", "Infantry raids are coming. Wall the approaches with Pillboxes, weather the waves, then counter-attack.",
     "Basic base, $4000, tier-1 tech", "Basic base, attacks in waves", "Base defence, holding under pressure"),
    ("Iron Fist", "The enemy fields armour. Build a War Factory and answer tanks with tanks.",
     "Basic base, $6000, tier-2 tech", "Fortress + a mobile force", "Vehicles and armour matchups"),
    ("Dark Territory", "Three outposts hide in the fog. Build a Radar Dome and hunt every last one down.",
     "Basic base, 12-unit force, $6000, tier-3", "Three hidden outposts", "Radar, fog of war, scouting"),
    ("Crystal War", "A rich crystal seam runs through the centre. Out-mine the enemy and guard your harvesters.",
     "Basic base, $5000, tier-3 + Helipad", "Full base, strong force", "Economy warfare, harvester defence"),
    ("Siege", "Their base bristles with turrets and never leaves it. Artillery cracks it open — take it slow.",
     "Basic base, 12-unit force, $9000, all tech", "A gun fortress (won't attack)", "Artillery sieging, patience"),
    ("Thunderbirds", "A water moat guards their shore. Gunships answer to no terrain.",
     "Full base, $8000, all tech", "Full base behind water", "Air power, terrain"),
    ("Red Dawn", "Everything they have against everything you've learned. Finish it.",
     "Construction Yard, $8000, all tech", "Full base, huge force", "The complete game"),
]


def dmg_class(v):
    if v == 0: return "d0"
    if v < 50: return "d1"
    if v < 90: return "d2"
    if v <= 100: return "d3"
    return "d4"


def esc(s): return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


rows_units = "\n".join(
    f'<tr><td class="nm">{esc(n)}<span class="ab">{esc(a)}</span></td>'
    f'<td class="num">${c}</td><td class="num">{hp}</td><td class="num">{sp}</td>'
    f'<td class="num">{rng if rng else "—"}</td><td class="num">{rof if rof else "—"}</td>'
    f'<td>{esc(w)}</td><td>{esc(ar)}</td><td class="role">{esc(role)}</td></tr>'
    for (n, a, c, hp, sp, rng, rof, dm, w, ar, role, pr) in UNITS)

rows_bld = "\n".join(
    f'<tr><td class="nm">{esc(n)}<span class="ab">{esc(a)}</span></td>'
    f'<td class="num">${c}</td><td class="num">{hp}</td>'
    f'<td class="num pw {"pos" if pw>0 else ("neg" if pw<0 else "")}">{("+"+str(pw)) if pw>0 else (str(pw) if pw<0 else "0")}</td>'
    f'<td class="num">{w}×{h}</td><td class="role">{esc(role)}</td><td class="pre">{esc(pr)}</td></tr>'
    for (n, a, w, h, c, hp, pw, role, pr) in BLD)

dmg_head = "".join(f"<th>{esc(a)}</th>" for a in ARMOURS)
dmg_rows = "\n".join(
    f'<tr><td class="wnm">{esc(w)}</td>' +
    "".join(f'<td class="dm {dmg_class(v)}">{v}%</td>' for v in vals) + "</tr>"
    for w, vals in DMG.items())

mission_cards = "\n".join(
    f'''<div class="mcard">
  <div class="mnum">{i+1}</div>
  <div class="mbody">
    <h3>{esc(name)}</h3>
    <p>{esc(brief)}</p>
    <div class="mmeta">
      <span><b>You:</b> {esc(you)}</span>
      <span><b>Enemy:</b> {esc(en)}</span>
      <span class="teach"><b>Teaches:</b> {esc(t)}</span>
    </div>
  </div></div>'''
    for i, (name, brief, you, en, t) in enumerate(MISSIONS))

HTML = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RedMote — Field Manual</title>
<style>
  :root{{
    --bg:#0d0f0c; --panel:#161a13; --panel2:#1d2218; --ink:#d8dccb;
    --dim:#8b917c; --red:#c8342a; --redl:#e05a48; --gold:#e6cd6e; --steel:#8892a0;
    --line:#2c3324; --grid:#242a1c;
  }}
  *{{box-sizing:border-box}}
  html{{scroll-behavior:smooth}}
  body{{margin:0;background:var(--bg);color:var(--ink);
    font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;}}
  img{{max-width:100%;image-rendering:pixelated;image-rendering:crisp-edges;}}
  a{{color:var(--gold);text-decoration:none}}
  a:hover{{text-decoration:underline}}
  .wrap{{max-width:960px;margin:0 auto;padding:0 20px}}

  /* header */
  header.hero{{background:
      radial-gradient(120% 140% at 80% -20%, #2a1512 0%, transparent 55%),
      linear-gradient(180deg,#14170f,#0d0f0c);
    border-bottom:2px solid var(--red);padding:38px 0 30px;}}
  .hrow{{display:flex;gap:22px;align-items:center;flex-wrap:wrap}}
  .hicon{{width:96px;height:96px;border:2px solid var(--steel);border-radius:8px;
    background:#0a0a08;flex:0 0 auto;box-shadow:0 6px 20px rgba(0,0,0,.5)}}
  .htext h1{{margin:0;font-size:2.6rem;letter-spacing:.06em;color:var(--red);
    text-shadow:0 2px 0 #4a100c;font-weight:800}}
  .htext .sub{{color:var(--ink);font-size:1.15rem;margin-top:2px}}
  .htext .by{{color:var(--dim);font-size:.9rem;margin-top:8px;letter-spacing:.04em}}
  .badges{{margin-top:14px;display:flex;gap:8px;flex-wrap:wrap}}
  .badge{{background:var(--panel2);border:1px solid var(--line);color:var(--gold);
    font-size:.78rem;padding:4px 10px;border-radius:20px;letter-spacing:.04em}}

  /* sticky nav */
  nav.toc{{position:sticky;top:0;z-index:5;background:rgba(13,15,12,.94);
    backdrop-filter:blur(6px);border-bottom:1px solid var(--line)}}
  nav.toc .wrap{{display:flex;gap:4px;flex-wrap:wrap;padding:8px 20px}}
  nav.toc a{{color:var(--dim);font-size:.82rem;padding:5px 10px;border-radius:6px}}
  nav.toc a:hover{{color:var(--ink);background:var(--panel);text-decoration:none}}

  section{{padding:34px 0 8px;border-bottom:1px solid var(--grid)}}
  h2{{font-size:1.5rem;color:var(--gold);margin:0 0 4px;letter-spacing:.03em;
    display:flex;align-items:baseline;gap:10px}}
  h2::before{{content:"▸";color:var(--red);font-size:1rem}}
  .lede{{color:var(--dim);margin:0 0 20px;max-width:70ch}}
  h3{{color:var(--redl);margin:22px 0 8px;font-size:1.05rem}}
  p{{max-width:74ch}}
  ul{{max-width:74ch}}
  li{{margin:4px 0}}
  b,strong{{color:var(--ink)}}
  code{{background:var(--panel2);border:1px solid var(--line);border-radius:4px;
    padding:1px 6px;font-size:.88em;color:var(--gold)}}

  /* cards / grids */
  .grid{{display:grid;gap:14px}}
  .g2{{grid-template-columns:repeat(2,1fr)}}
  .g3{{grid-template-columns:repeat(3,1fr)}}
  @media(max-width:720px){{.g2,.g3{{grid-template-columns:1fr}}}}
  .card{{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px}}
  .card h3{{margin-top:0}}
  .shot{{border:1px solid var(--line);border-radius:8px;display:block;width:100%}}
  .shotcap{{color:var(--dim);font-size:.82rem;text-align:center;margin-top:6px}}

  /* tables */
  .tw{{overflow-x:auto;margin:6px 0 4px;-webkit-overflow-scrolling:touch}}
  table{{border-collapse:collapse;width:100%;min-width:560px;font-size:.9rem}}
  th,td{{padding:8px 10px;text-align:left;border-bottom:1px solid var(--line)}}
  thead th{{color:var(--gold);font-size:.76rem;text-transform:uppercase;letter-spacing:.06em;
    border-bottom:2px solid var(--line);white-space:nowrap}}
  tbody tr:hover{{background:var(--panel)}}
  td.num{{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}}
  td.nm{{font-weight:600;color:var(--ink);white-space:nowrap}}
  .ab{{display:inline-block;margin-left:8px;color:var(--dim);font-weight:400;font-size:.78rem;
    letter-spacing:.06em}}
  td.role,td.pre{{color:var(--dim);font-size:.86rem}}
  td.pw.pos{{color:#7cc85e}} td.pw.neg{{color:var(--redl)}}

  /* damage matrix */
  table.dmg{{min-width:480px}}
  table.dmg td.wnm{{font-weight:600;color:var(--ink)}}
  td.dm{{text-align:center;font-variant-numeric:tabular-nums;font-weight:600;color:#0c0f0a}}
  .d0{{background:#2a2320;color:#6b6355;font-weight:400}}
  .d1{{background:#7d2b22;color:#f2d3cd}}
  .d2{{background:#9c6b1f;color:#fbe7c4}}
  .d3{{background:#4f7d2b;color:#e8f5d6}}
  .d4{{background:#2f9c52;color:#e6fbe9}}
  .legend{{display:flex;gap:14px;flex-wrap:wrap;margin:12px 0 0;font-size:.8rem;color:var(--dim)}}
  .legend span{{display:flex;align-items:center;gap:6px}}
  .sw{{width:14px;height:14px;border-radius:3px;display:inline-block}}

  /* controls */
  .keys{{display:grid;grid-template-columns:auto 1fr;gap:10px 16px;align-items:start;
    max-width:74ch}}
  .kb{{background:var(--panel2);border:1px solid var(--steel);border-bottom-width:3px;
    border-radius:6px;padding:3px 10px;font-weight:700;color:var(--gold);font-size:.85rem;
    white-space:nowrap;justify-self:start;text-align:center;min-width:44px}}

  /* tech tree */
  .tiers{{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}}
  @media(max-width:720px){{.tiers{{grid-template-columns:1fr 1fr}}}}
  .tier{{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px}}
  .tier h4{{margin:0 0 8px;color:var(--gold);font-size:.9rem;letter-spacing:.04em}}
  .tier ul{{margin:0;padding-left:18px;font-size:.9rem}}
  .tier li{{color:var(--dim)}} .tier li b{{color:var(--ink)}}
  .arrow{{text-align:center;color:var(--red);font-size:1.4rem;line-height:1;margin:-4px 0}}

  /* mission cards */
  .mcard{{display:flex;gap:16px;background:var(--panel);border:1px solid var(--line);
    border-left:3px solid var(--red);border-radius:10px;padding:16px;margin-bottom:12px}}
  .mnum{{flex:0 0 auto;width:40px;height:40px;border-radius:50%;background:#22110f;
    border:2px solid var(--red);color:var(--redl);font-weight:800;font-size:1.2rem;
    display:flex;align-items:center;justify-content:center}}
  .mbody h3{{margin:2px 0 6px;color:var(--ink)}}
  .mbody p{{margin:0 0 10px;color:var(--dim)}}
  .mmeta{{display:flex;flex-wrap:wrap;gap:6px 18px;font-size:.83rem;color:var(--dim)}}
  .mmeta b{{color:var(--gold);font-weight:600}}
  .mmeta .teach b{{color:var(--redl)}}

  .callout{{background:var(--panel2);border:1px solid var(--line);border-left:3px solid var(--gold);
    border-radius:8px;padding:14px 16px;margin:16px 0;color:var(--ink)}}
  .callout b{{color:var(--gold)}}
  footer{{padding:30px 0 50px;color:var(--dim);font-size:.85rem;text-align:center}}

  /* light theme fallback */
  @media (prefers-color-scheme:light){{
    :root{{--bg:#f3f1e8;--panel:#fff;--panel2:#efece0;--ink:#23281c;--dim:#5c614f;
      --line:#d8d5c4;--grid:#e6e3d4;}}
    .htext h1{{text-shadow:none}}
    td.dm{{color:#0c0f0a}}
  }}

  /* print / PDF: light palette, keep the heatmap colours, avoid ugly breaks */
  @media print{{
    :root{{--bg:#fff;--panel:#fff;--panel2:#f4f2ea;--ink:#1c2016;--dim:#4a4f3e;
      --line:#cfccbc;--grid:#e2dfd0;}}
    *{{-webkit-print-color-adjust:exact;print-color-adjust:exact}}
    body{{font-size:11pt}}
    nav.toc{{display:none}}
    header.hero{{border-bottom:2px solid var(--red)}}
    .wrap{{max-width:none;padding:0}}
    section{{padding:14px 0 4px;border-bottom:none}}
    h2{{page-break-after:avoid}}  h3{{page-break-after:avoid}}
    .card,.tier,.mcard,.callout,tr,.grid>div{{page-break-inside:avoid}}
    table{{min-width:0}}  .tw{{overflow:visible}}
    #intro,#buildings,#tech,#units,#counters,#campaign{{page-break-before:auto}}
    .htext h1{{text-shadow:none}}
    td.dm{{color:#0c0f0a}}
    a{{color:var(--ink)}}
    footer{{padding:16px 0}}
  }}
  @page{{margin:14mm 12mm}}
</style>
</head>
<body>
<header class="hero"><div class="wrap"><div class="hrow">
  <img class="hicon" src="{ICON}" alt="RedMote icon">
  <div class="htext">
    <h1>RED MOTE</h1>
    <div class="sub">A comically tiny — but fully functional — Red Alert-style RTS</div>
    <div class="by">for the Thumby Color · by austinio7116 · v1.1.1</div>
    <div class="badges">
      <span class="badge">9-mission campaign</span>
      <span class="badge">Skirmish mode</span>
      <span class="badge">Full tech tree</span>
      <span class="badge">Fog of war</span>
      <span class="badge">~6-pixel units</span>
    </div>
  </div>
</div></div></header>

<nav class="toc"><div class="wrap">
  <a href="#intro">Overview</a>
  <a href="#controls">Controls</a>
  <a href="#loop">First 5 minutes</a>
  <a href="#economy">Economy</a>
  <a href="#buildings">Buildings</a>
  <a href="#tech">Tech tree</a>
  <a href="#units">Units</a>
  <a href="#combat">Combat</a>
  <a href="#counters">Counters</a>
  <a href="#campaign">Campaign</a>
  <a href="#skirmish">Skirmish</a>
  <a href="#tips">Tips</a>
</div></nav>

<div class="wrap">

<section id="intro">
  <h2>What is RedMote?</h2>
  <p class="lede">RedMote is a complete real-time strategy game on a 128×128 screen. Units are
  only about six pixels tall, so entire armies fight on-screen at once — but under the
  hood it's the full RTS: a base to build, an economy to run, a tech tree to climb, fog of
  war to scout, and an enemy that builds its own base and sends attack waves at you.</p>
  <div class="grid g3">
    <div><img class="shot" src="{SHOT1}" alt="battle"><div class="shotcap">Armies clash — tracers, tesla arcs, explosions</div></div>
    <div><img class="shot" src="{SHOT2}" alt="base"><div class="shotcap">A base built out, roads knitting it together</div></div>
    <div><img class="shot" src="{SHOT3}" alt="build menu"><div class="shotcap">The paused build menu — the whole tech tree</div></div>
  </div>
  <p style="margin-top:18px">You play the <b>blue</b> army; the enemy is <b>red</b>. Win by destroying
  everything the enemy has — every building <i>and</i> every unit. Lose if the same happens to you.</p>
</section>

<section id="controls">
  <h2>Controls</h2>
  <p class="lede">Eight buttons do everything. Orders are context-sensitive — the cursor shows a
  label (<code>MOVE</code>, <code>ATTACK</code>, <code>MINE</code>, <code>RALLY</code>) for what
  <b>A</b> will do where you're pointing.</p>
  <div class="keys">
    <span class="kb">D-pad</span><span>Move the cursor. Push it against a screen edge to scroll the map.</span>
    <span class="kb">A</span><span>Tap a unit to select it · tap the ground with a selection to issue the order the cursor shows · <b>drag</b> to box-select a group.</span>
    <span class="kb">A A</span><span>Double-tap a unit — select every unit of that type on screen. Double-tap the <b>ground</b> — set a <b>waypoint</b>: keep tapping to stack a patrol path; a single tap adds the final leg. Attack orders clear the path.</span>
    <span class="kb">A A A</span><span>Triple-tap — select your <b>whole army</b>, map-wide.</span>
    <span class="kb">B</span><span>Deselect / cancel placement / close a menu.</span>
    <span class="kb">LB</span><span>Open the <b>build menu</b> (pauses the game). LB again cycles tabs; D-pad picks a card; A queues or places it; B closes.</span>
    <span class="kb">RB</span><span><b>Hold</b> for the radar minimap — press A over the map to order or recenter there. A quick <b>tap</b> jumps the view to your base.</span>
    <span class="kb">MENU</span><span>Pause (Resume / Restart / To Title). Hold MENU 3s for the engine's system menu.</span>
  </div>
  <div class="callout"><b>Minimap orders:</b> with a group selected, hold <b>RB</b> and press <b>A</b>
  on the radar map to send them straight to that spot anywhere on the map — you don't have to
  scroll there first.</div>
</section>

<section id="loop">
  <h2>Your first five minutes</h2>
  <p class="lede">Nearly every mission (and skirmish) opens the same way. Get the economy running
  before you build anything else.</p>
  <ol>
    <li><b>Power first.</b> Open the build menu (LB) and put down a <b>Power Plant</b> — most
      buildings drain power and stall without it.</li>
    <li><b>Refinery next.</b> A <b>Refinery</b> arrives with a free <b>Harvester</b> that
      immediately drives to the nearest ore and starts banking credits automatically.</li>
    <li><b>Add production.</b> A <b>Barracks</b> for infantry, then a <b>War Factory</b> for tanks
      (and more harvesters — two per refinery keeps the money flowing).</li>
    <li><b>Tech up.</b> A <b>Radar Dome</b> lights the minimap and unlocks heavier vehicles; a
      <b>Tech Center</b> opens the Tesla Tank and Tesla Coil.</li>
    <li><b>Defend, then attack.</b> Screen your base with Pillboxes and Gun Turrets, build an
      army, select it (triple-tap), and push into the enemy.</li>
  </ol>
</section>

<section id="economy">
  <h2>Economy &amp; power</h2>
  <div class="grid g2">
    <div class="card">
      <h3>Ore &amp; crystal</h3>
      <p><b>Ore</b> is worth <b>50</b> credits per patch; <b>crystal</b> is worth <b>100</b> —
      double value, so contest it. Harvesters fill up to <b>700</b> credits, drive back to a
      Refinery to unload, and repeat forever, all on their own. Mined ground is spent — send
      harvesters to fresh seams as fields run dry.</p>
    </div>
    <div class="card">
      <h3>Power</h3>
      <p>Every Power Plant supplies <b>+100</b>. Most other buildings draw power (shown on each
      build card). If your usage exceeds supply, <b>production slows to a crawl</b>, the radar map
      goes dark, and <b>Tesla Coils stop firing</b>. The top bar's power meter turns red when you're
      short — build another Power Plant.</p>
    </div>
  </div>
  <div class="callout"><b>Rule of thumb:</b> keep a little power headroom and two harvesters per
  refinery. A stalled economy loses games faster than any battle.</div>
</section>

<section id="buildings">
  <h2>Buildings</h2>
  <p class="lede">Build order is gated by prerequisites — you can't jump straight to the good stuff.
  Power (+/−) is per building; negatives draw from your Power Plants.</p>
  <div class="tw"><table>
    <thead><tr><th>Building</th><th>Cost</th><th>HP</th><th>Power</th><th>Size</th><th>Role</th><th>Requires</th></tr></thead>
    <tbody>{rows_bld}</tbody>
  </table></div>
</section>

<section id="tech">
  <h2>The tech tree</h2>
  <p class="lede">From a bare Construction Yard to Tesla, the path branches through power, the
  refinery, and the factory. Each tier unlocks the next.</p>
  <div class="tiers">
    <div class="tier"><h4>TIER 1 · Base</h4><ul>
      <li><b>Construction Yard</b> — the core</li>
      <li><b>Power Plant</b></li>
      <li><b>Refinery</b> (+Harvester)</li>
      <li><b>Barracks</b> → Rifleman, Rocketeer, Flamer</li>
      <li><b>Pillbox</b> defence</li></ul></div>
    <div class="tier"><h4>TIER 2 · Armour</h4><ul>
      <li><b>War Factory</b> → Light Tank, Harvester</li>
      <li><b>Gun Turret</b> defence</li></ul></div>
    <div class="tier"><h4>TIER 3 · Radar</h4><ul>
      <li><b>Radar Dome</b> → minimap</li>
      <li>unlocks <b>Heavy Tank</b>, <b>Artillery</b></li></ul></div>
    <div class="tier"><h4>TIER 4 · High-tech</h4><ul>
      <li><b>Helipad</b> → Gunship</li>
      <li><b>Tech Center</b> → <b>Tesla Tank</b></li>
      <li><b>Tesla Coil</b> defence</li></ul></div>
  </div>
</section>

<section id="units">
  <h2>Units</h2>
  <p class="lede">Nine types across infantry, vehicles and air. Range and fire-rate are in the
  game's own units; higher speed = faster, lower reload = faster firing.</p>
  <div class="tw"><table>
    <thead><tr><th>Unit</th><th>Cost</th><th>HP</th><th>Speed</th><th>Range</th><th>Reload s</th><th>Weapon</th><th>Armour</th><th>Role</th></tr></thead>
    <tbody>{rows_units}</tbody>
  </table></div>
  <div class="callout"><b>Reading armour:</b> a unit's <i>armour</i> type decides how much damage it
  takes from each weapon (next section). Infantry melt to machine-guns and fire; heavy armour
  shrugs off small-arms but not rockets; only Rocketeers and Gunships can hit <b>air</b>.</div>
</section>

<section id="counters">
  <h2>Weapons vs armour</h2>
  <p class="lede">Damage is multiplied by this matrix — pick the right tool. 100% is full damage;
  green beats the armour, red bounces off, and <b>0%</b> means that weapon simply can't hurt it.</p>
  <div class="tw"><table class="dmg">
    <thead><tr><th>Weapon ↓ / Armour →</th>{dmg_head}</tr></thead>
    <tbody>{dmg_rows}</tbody>
  </table></div>
  <div class="legend">
    <span><i class="sw d4"></i> strong (&gt;100%)</span>
    <span><i class="sw d3"></i> full (90–100%)</span>
    <span><i class="sw d2"></i> reduced (50–89%)</span>
    <span><i class="sw d1"></i> weak (&lt;50%)</span>
    <span><i class="sw d0"></i> can't damage (0%)</span>
  </div>
  <h3>What this means</h3>
  <ul>
    <li><b>Machine-guns &amp; Flamers</b> shred infantry but barely dent tanks — screen them out with your own troops or a Pillbox.</li>
    <li><b>Cannons</b> (Light/Heavy Tank, Gun Turret) are the anti-vehicle backbone but <b>can't touch aircraft</b>.</li>
    <li><b>Rockets</b> (Rocketeer, Gunship) are the answer to heavy armour <i>and</i> the only reliable anti-air.</li>
    <li><b>Artillery</b> out-ranges everything and hits buildings hardest (120%) — but has a 20px minimum range, so keep it behind the line.</li>
    <li><b>Tesla</b> deals full damage to <i>everything</i>, armour be damned — expensive, but the ultimate answer.</li>
  </ul>
</section>

<section id="combat">
  <h2>Commanding your army</h2>
  <div class="grid g2">
    <div class="card"><h3>Selecting</h3><p>Tap a unit to pick one, drag a box to grab a group, or
    triple-tap to select your entire army at once. Double-tap grabs every unit of that type on screen —
    handy for pulling all your tanks together.</p></div>
    <div class="card"><h3>Formations</h3><p>Order a group to move and they spread into a formation
    around the target instead of piling onto one tile — they arrive as a battle line, not a scrum.</p></div>
    <div class="card"><h3>Attacking</h3><p>With a selection, point at an enemy and the cursor reads
    <code>ATTACK</code>. Units chase and fire; artillery and tanks keep their range, infantry close in.</p></div>
    <div class="card"><h3>Rally points</h3><p>Select a Barracks, Factory or Helipad and tap the ground
    to set a rally point — new units drive there automatically as they roll off the line.</p></div>
  </div>
</section>

<section id="campaign">
  <h2>The campaign</h2>
  <p class="lede">Nine missions that teach the game one system at a time — from commanding a fixed
  force with no base, up to a full-tech final battle. Each win unlocks the next and is saved, so you
  can pick up where you left off.</p>
  {mission_cards}
</section>

<section id="skirmish">
  <h2>Skirmish mode</h2>
  <p class="lede">A one-off battle against the AI with the setup you choose — both sides get the same
  start, so it's a fair fight. Tune it from the skirmish screen:</p>
  <div class="grid g2">
    <div class="card"><h3>Army</h3><p><b>None / Squad / Force / Horde</b> — how big a starting army each
    side fields, from nothing up to a 22-unit horde for an instant war.</p></div>
    <div class="card"><h3>Base</h3><p><b>Conyard / Basic / Full</b> — start from a single Construction
    Yard and build out, or drop into a pre-built base with radar, defences and tech ready.</p></div>
    <div class="card"><h3>Funds</h3><p><b>$3,000 / $8,000 / $20,000</b> — starting credits for both sides.</p></div>
    <div class="card"><h3>Enemy</h3><p><b>Easy / Normal / Hard</b> — the AI builds faster, richer and
    attacks sooner as you raise it. On Easy it lives on harvesters like you; on Hard it gets a small income bonus.</p></div>
  </div>
</section>

<section id="tips">
  <h2>Field tips</h2>
  <ul>
    <li><b>Never stop building power.</b> A low-power base can't produce, can't see, and can't fire its Tesla Coils.</li>
    <li><b>Two harvesters per refinery.</b> One is never enough once you're building an army.</li>
    <li><b>Protect harvesters.</b> They're unarmed and expensive ($800). A couple of raiders can starve your whole economy — a Pillbox near the ore field pays for itself.</li>
    <li><b>Mix your army.</b> Rockets for their tanks and air, cannons for their vehicles, flamers/rifles to clear their infantry. A pure tank ball dies to massed rocketeers.</li>
    <li><b>Artillery wins sieges</b> but folds in a brawl — always keep it behind heavies.</li>
    <li><b>Scout with cheap units.</b> Fog hides everything; a lone rifleman reveals where the enemy is weak before you commit.</li>
    <li><b>Contest crystal.</b> At double the value of ore, holding the crystal seam can out-fund the enemy on its own.</li>
    <li><b>Queue ahead.</b> Line up several units in the build menu so production never idles while you fight.</li>
  </ul>
</section>

</div>
<footer>RedMote v1.1.1 · a Mote game for the Thumby Color · this manual is generated from the game's own data tables.</footer>
</body>
</html>'''

out = os.path.join(ROOT, "docs", "redmote-guide.html")
open(out, "w", encoding="utf-8").write(HTML)
print("wrote", out, "(%d KB)" % (len(HTML) // 1024))
