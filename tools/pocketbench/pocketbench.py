#!/usr/bin/env python3
"""ThumbyCue pocket bench — a live dial-and-look for the pocket shapes.

    tools/pocketbench/pocketbench.py        then open http://127.0.0.1:8765

Every image is drawn by cue_render_build_table itself, straight down from
above, so what you are adjusting is the game's own mesh and not a diagram of
it.  The red circle is the drop: the ball's centre inside it and the ball is
down, which since the jaw-tip line came out is the only thing that decides a
pot.

THE SLIDERS ARE THE GAME'S OWN FIELDS, in the game's own units, so a number
dialled here goes back into the source as itself:

    pr    cue_table_init        t->pr_corner  / t->pr_side    (xR)
    gap   cue_table_init        t->gap_corner / t->gap_side   (xR)
    off   cue_table_init        t->off_corner / t->off_side   (xR)
    capm  cue_table.c build_world, the margin taken off pr to get the drop
                                circle: 0.30f, or side_m at a middle  (xR)
    set   cue_table_default_cut CueCut.set                    (m)
    rad   cue_table_default_cut CueCut.rad                   (x pr)
    roll  cue_table_default_cut CueCut.roll                   (x pr)

The millimetres beside them are derived and read-only: the mouth the ball goes
through, the drop circle, the lip's outer edge and its thickness.

Save writes pockets.json beside this script and prints the source lines to
paste back, so a tuning session survives and can be picked up again.

L-SHAPED BEDS. Tick "L-shaped" and the bench builds a custom table instead of a
shipped one: bed half-extents, the notch, and a picker for WHICH of its seven
pockets to look at. That last part is the point — a rectangle has two kinds of
pocket and any one of each will do, while an L has seven that are not
interchangeable. The two beside the notch have the missing corner as their
outside, and the two middles sit on rails of different lengths. None of them
has ever been dialled: they inherit the rectangle's numbers, which is a guess.

The knobs are still the global corner/middle fields, so dialling the notch
corners moves every corner with them. If they turn out to want numbers of their
own, that is a new pair of fields on CueTable — and therefore a protocol bump —
worth doing once somebody knows what the numbers should be.
"""
import http.server
import io
import json
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
MOTE = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC  = os.path.join(HERE, "cuepocket.c")
SAVE = os.path.join(HERE, "pockets.json")
BIN  = os.path.join(MOTE, "build_host", "cuepocket")
PORT = int(os.environ.get("POCKETBENCH_PORT", "8765"))

KEYS = ["pr", "gap", "off", "capm", "back", "set", "rad", "roll", "bore", "bset",
        # THE THINGS THAT ARE NOT THE POCKET but decide what it meets. A pocket
        # is where the cushion, the timber and the cloth arrive at the same
        # place, and four of the numbers that put them there were not on the
        # bench at all — so a gap could be dialled away in the pocket's own
        # terms while the thing causing it sat off-screen.
        "flen", "ang", "jaw", "rail", "cush", "ball", "round"]

TMP = tempfile.mkdtemp(prefix="pocketbench-")

INC = ("-Igames/thumbycue/src -Iengine/core -Iengine/math -Iengine/render "
       "-Iengine/assets -Iengine/input -Iengine/physics -Iengine/audio "
       "-Iengine/scene -Isdk -Ios -DMOTE_HOST=1 -DCUE_JAW_SEGS=10 "
       "-DCUE_ARC_SEGS=20 -DCUE_MAX_SEG=256 -DMAX_TABLE_TRI=24000").split()


def build():
    """Rebuild against the game's current source, so edits to it show up."""
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    cmd = ["gcc", "-O2", *INC, "-o", BIN, SRC,
           "games/thumbycue/src/cue_render.c",
           "games/thumbycue/src/cue_table.c",
           "games/thumbycue/src/cue_physics.c",
           "games/thumbycue/src/r3d_raster.c", "-lm"]
    r = subprocess.run(cmd, cwd=MOTE, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit("build failed")


def defaults():
    return json.loads(subprocess.run([BIN, "--defaults"],
                                     capture_output=True, text=True).stdout)


def render(q):
    out = os.path.join(TMP, "pb_%s_%s_%s_%s.ppm" % (
        q.get("table", "x"), q.get("type", "x"), q.get("view", "top"),
        q.get("pocket", "d")))
    cmd = [BIN, "--table", q.get("table", "snooker12"),
           "--type", q.get("type", "corner"), "--out", out,
           "--size", "700", "--zoom", q.get("zoom", "5"),
           "--view", q.get("view", "top")]
    # THE SHAPE, AND WHICH POCKET. A rectangle has two kinds of pocket and any
    # one of each will do; an L has seven that are not interchangeable, so the
    # bench has to be able to say which. Both are optional, so a session that
    # only ever looks at the shipped tables behaves exactly as it did.
    if q.get("bedl", "") not in ("", "0") and q.get("bedw", "") not in ("", "0"):
        cmd += ["--bed", q["bedl"], q["bedw"]]
    if q.get("notchx", "") not in ("", "0") and q.get("notchz", "") not in ("", "0"):
        cmd += ["--notch", q["notchx"], q["notchz"]]
    # S2: a regular bed. Sides, and a pocket every so many corners — one for the
    # polygons, ten of sixty for a round one.
    if q.get("ngon", "") not in ("", "0"):
        cmd += ["--ngon", q["ngon"], q.get("ngone", "1")]
    if q.get("pocket", "") not in ("", "-1"):
        cmd += ["--pocket", q["pocket"]]
    for cam in ("yaw", "pitch", "dist"):
        if q.get(cam, "") != "":
            cmd += ["--" + cam, q[cam]]
    for k in KEYS:
        if k in q:
            cmd += ["--" + k, q[k]]
    r = subprocess.run(cmd, capture_output=True, text=True)
    info = {}
    # The last line is the pocket's own numbers; a line before it, if present,
    # is the whole table's pocket list, which is what lets the front end offer
    # them by name instead of by guesswork.
    for ln in r.stderr.strip().splitlines():
        try:
            d = json.loads(ln)
        except Exception:
            continue
        info.update(d)
    from PIL import Image
    buf = io.BytesIO()
    Image.open(out).save(buf, "PNG")
    return buf.getvalue(), info


def source_block(name, d, v):
    """The lines to paste back, exactly as the source spells them."""
    c, m = v["corner"], v["middle"]
    return "\n".join([
        "/* %s  —  cue_table_init(), the %s */" % (d["label"], d["sym"]),
        "    t->pr_corner  = %.4ff * t->R; t->pr_side  = %.4ff * t->R;" % (c["pr"], m["pr"]),
        "    t->gap_corner = %.4ff * t->R; t->gap_side = %.4ff * t->R;" % (c["gap"], m["gap"]),
        "    t->off_corner = %.4ff * t->R; t->off_side = %.4ff * t->R;" % (c["off"], m["off"]),
        "    t->drop_back  = %.4ff * t->R; t->drop_back_side = %.4ff * t->R;" % (c["back"], m["back"]),
        "    t->bore_corner = %.4ff * t->R; t->bore_side = %.4ff * t->R;" % (c["bore"], m["bore"]),
        "    t->bore_set_corner = %.4ff * t->R; t->bore_set_side = %.4ff * t->R;" % (c["bset"], m["bset"]),
        "",
        "/* cue_table.c build_world() — the drop circle */",
        "    float capc = t->pr_corner - %.4ff * t->R," % c["capm"],
        "          caps = t->pr_side   - %.4ff * t->R;" % m["capm"],
        "",
        "/* cue_table_default_cut() — { set(m), rad(x pr), roll(x pr), arc(deg) } */",
        "    corner  { %.5ff, %.4ff, %.4ff, 90.0f }" % (c["set"], c["rad"], c["roll"]),
        "    middle  { %.5ff, %.4ff, %.4ff, 180.0f }" % (m["set"], m["rad"], m["roll"]),
    ])


PAGE = r"""<!doctype html><meta charset=utf-8>
<title>ThumbyCue pocket bench</title>
<style>
 :root{--bg:#111318;--fg:#e8eaf0;--dim:#8b93a6;--line:#252a35;--acc:#5cc8ff;--play:#ffc46b}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--fg);
      font:14px/1.45 ui-sans-serif,system-ui,"DejaVu Sans",sans-serif}
 header{padding:13px 18px;border-bottom:1px solid var(--line);
        display:flex;gap:16px;align-items:center;flex-wrap:wrap}
 h1{font-size:16px;margin:0;font-weight:600}
 .key{color:var(--dim);font-size:12.5px}
 .key b{font-weight:600}
 .r{color:#ff5a5a}.c{color:#5cc8ff}.d{color:#2f7fd6}.w{color:#fff}
 select,button{background:#1b1f28;color:var(--fg);border:1px solid var(--line);
        border-radius:6px;padding:6px 10px;font:inherit}
 button{cursor:pointer} button:hover{border-color:var(--acc)}
 main{display:grid;grid-template-columns:1fr 1fr;gap:16px;padding:16px;align-items:start}
 .pane{border:1px solid var(--line);border-radius:10px;overflow:hidden;background:#0d0f13}
 .pane h2{margin:0;padding:8px 13px;font-size:13px;font-weight:600;
          border-bottom:1px solid var(--line);background:#141821}
 .pane img{display:block;width:100%;height:auto;background:#0b0d11}
 .mm{display:flex;flex-wrap:wrap;gap:4px 16px;padding:9px 13px;
     border-top:1px solid var(--line);border-bottom:1px solid var(--line);
     background:#0b0d11;font:12.5px ui-monospace,"DejaVu Sans Mono",monospace;color:var(--dim)}
 .mm b{color:#dfe6f2;font-weight:600}
 .knobs{padding:9px 13px 13px}
 .k{display:grid;grid-template-columns:118px 1fr 76px 78px;gap:9px;
    align-items:center;margin:7px 0}
 .k label{cursor:help;line-height:1.2}
 .k label b{display:block;color:#dfe6f2;font-size:12.5px;font-weight:600}
 .k label i{display:block;color:#5a6274;font-size:10.5px;font-style:normal;
            font-family:ui-monospace,"DejaVu Sans Mono",monospace}
 .k .u{color:#5a6274;font-size:11.5px;text-align:right}
 .k.play label b{color:var(--play)}
 .k.play input[type=range]{accent-color:var(--play)}
 .k input[type=range]{width:100%;accent-color:var(--acc)}
 .k input[type=number]{background:#1b1f28;color:var(--fg);border:1px solid var(--line);
    border-radius:5px;padding:4px 6px;font:inherit;width:100%}
 footer{padding:0 16px 26px}
 pre{background:#0d0f13;border:1px solid var(--line);border-radius:8px;padding:13px;
     overflow:auto;font:12.5px/1.55 ui-monospace,"DejaVu Sans Mono",monospace;color:#cfd6e4}
 .note{color:var(--dim);font-size:12.5px;margin:14px 0 8px}
</style>
<header>
  <h1>ThumbyCue pocket bench</h1>
  <select id=table></select>
  <!-- THE SHAPE. An L is not a table kind — it is a bed a custom table can
       have — so it is a switch here rather than an entry in the list above,
       which is exactly how the game offers it. -->
  <!-- THE BED'S SHAPE, which is not one of the tables above: the pocket numbers
       belong to the GAME and the shape is applied to whichever one is chosen,
       exactly as the workshop does it. A regular bed brings no second dimension
       with it, so it needs no boxes — just how many sides. -->
  <label class=key>shape
    <select id=shape>
      <option value="">rectangle</option>
      <option value="3">triangle</option>
      <option value="4">square</option>
      <option value="5">pentagon</option>
      <option value="6">hexagon</option>
      <option value="7">heptagon</option>
      <option value="8">octagon</option>
      <option value="60">round</option>
    </select></label>
  <label class=key><input id=lshape type=checkbox> L-shaped</label>
  <span class=key id=lwrap style="display:none">
    bed <input id=bedl type=number step=0.025 value=1.05 style="width:62px">
        x <input id=bedw type=number step=0.025 value=1.00 style="width:62px">
    notch <input id=notchx type=number step=0.05 value=1.0 style="width:56px">
          x <input id=notchz type=number step=0.05 value=0.85 style="width:56px">
    pocket <select id=pocket></select>
  </span>
  <label class=key>zoom <input id=zoom type=range min=2.5 max=9 step=.25 value=5
        style="vertical-align:middle;width:100px;accent-color:var(--acc)"></label>
  <span class=key>view
    <select id=view>
      <option value=top>from above (the mouth)</option>
      <option value=out>outside, looking in and down (the jaw)</option>
      <option value=in>inside, over the cloth (the back of the pocket)</option>
    </select>
    <label id=camwrap style="display:none">
      yaw <input id=yaw type=range min=-180 max=180 step=1 value=45
           style="vertical-align:middle;width:90px;accent-color:var(--acc)">
      pitch <input id=pitch type=range min=-5 max=88 step=1 value=45
           style="vertical-align:middle;width:80px;accent-color:var(--acc)">
      dist <input id=dist type=range min=0.06 max=0.60 step=0.01 value=0.20
           style="vertical-align:middle;width:80px;accent-color:var(--acc)">
      <span id=camtxt class=key></span>
    </label>
    <br><b style="color:#ff5cff">magenta = you are seeing through the table</b> — that is
    the gap, and the timber hole is what closes it.</span>
  <span class=key><b class=r>red</b> drop &nbsp;<b class=w>white</b> a ball on it
    &nbsp;<b class=c>cyan</b> lip outer edge &nbsp;<b class=d>dashed</b> bottom of the roll<br>
    <b style="color:var(--play)">amber = changes how it PLAYS</b>; the rest is the cut and the lip.
    Hover a name for what it does and where it lives.</span>
  <span style="flex:1"></span>
  <button id=reset>reset table</button>
  <button id=save>save</button>
</header>
<main>
  <div class=pane><h2>corner</h2><img id=img_corner>
    <div class=mm id=mm_corner></div><div class=knobs id=k_corner></div></div>
  <div class=pane><h2>middle</h2><img id=img_middle>
    <div class=mm id=mm_middle></div><div class=knobs id=k_middle></div></div>
</main>
<footer>
  <div class=note>Paste back into the source:</div>
  <pre id=out></pre>
</footer>
<script>
const KEYS=['gap','pr','off','capm','back','set','rad','roll','bore','bset',
            'flen','ang','jaw','rail','cush','ball','round'];
const NAME ={gap:'mouth width', pr:'hole size', off:'pocket depth',
             capm:'drop size', back:'drop depth', set:'cut setback', rad:'lip outer edge',
             roll:'lip thickness', bore:'timber hole', bset:'timber hole setback',
             flen:'facing length', ang:'facing splay', jaw:'knuckle radius',
             rail:'rail width', cush:'cushion height', ball:'ball size',
             round:'jaw style'};
const FIELD={gap:'gap_corner / gap_side', pr:'pr_corner / pr_side',
             off:'off_corner / off_side', capm:'capc / caps margin',
             set:'CueCut.set', rad:'CueCut.rad', roll:'CueCut.roll',
             bore:'bore_corner / bore_side', bset:'bore_set_corner / bore_set_side',
             flen:'facing_len', ang:'ang_corner / ang_side', jaw:'jaw_r',
             rail:'rail_w', cush:'cushion_h', ball:'R', round:'pocket_round'};
const RANGE={pr:[1.0,3.2,.01], gap:[1.2,3.8,.005], off:[0,2.6,.01],
             capm:[0,1.0,.01], back:[-0.5,2.0,.01], set:[-0.02,0.06,.0005],
             rad:[0.5,2.6,.005], roll:[0,1.0,.005],
             bore:[1.0,3.2,.01], bset:[-1.0,1.0,.01],
             flen:[0,4.0,.01], ang:[0,90,.5], jaw:[0,1.2,.005],
             rail:[0.02,0.20,.001], cush:[0.6,2.0,.01],
             ball:[0.010,0.080,.0005], round:[0,1,1]};
const UNIT ={pr:'×R', gap:'×R', off:'×R', capm:'×R', back:'×R',
             set:'m', rad:'×pr', roll:'×pr', bore:'×R', bset:'×R',
             flen:'×R', ang:'deg', jaw:'×R', rail:'m', cush:'×R', ball:'m',
             round:'0=mitred 1=rounded'};
const TIP  ={gap:'How far apart the knuckles sit — the opening the ball goes through. THE CUSHIONS MOVE WITH IT. t->gap_corner / t->gap_side in cue_table_init.',
             pr:'The size of the hole itself: the drawn bore, and what the drop and the lip are measured from. t->pr_corner / t->pr_side.',
             off:'How far the pocket centre sits back beyond the cushion line. t->off_corner / t->off_side.',
             capm:'How much smaller the drop circle is than the hole. BIGGER MEANS A SMALLER DROP. The margin taken off pr in build_world (0.30f, or side_m at a middle).',
             back:'How much DEEPER than the pocket the drop circle is centred — pushes it back into the pocket without resizing it or moving the hole. t->drop_back / t->drop_back_side.',
             set:'How far the cut arc’s centre sits back from the pocket — slides the whole cut in and out. CueCut.set.',
             rad:'Where flat cloth stops and the roll begins. A multiple of the hole size, not of the drop, so the two move apart. CueCut.rad.',
             roll:'How wide the roll is, from that edge inwards and down. CueCut.roll.',
             bore:'The hole cut in the TIMBER, which started life equal to the mouth and does not have to be. Too big and there is a slot between the end of the cushion and the frame that you can see out of the table through — the magenta in the OUTSIDE and INSIDE views. t->bore_corner / t->bore_side.',
             bset:'How far out from the pocket centre that hole is cut, along the pocket\u2019s own outward normal. The other way to close the same slot: shrink the hole, or set it back. t->bore_set_corner / t->bore_set_side.',
             flen:'How long the facing is — the angled piece running from the knuckle back to the timber. Read only by a MITRED jaw; a rounded one is a bezier and ignores it. t->facing_len.',
             ang:'How far the facing splays outward from the rail. Mitred jaws only. t->ang_corner / t->ang_side.',
             jaw:'The radius of the knuckle circle the ball rattles off. Subtracted twice from the knuckle centres to give the mouth, so it changes the opening without moving a cushion. t->jaw_r.',
             rail:'How wide the timber is. The frame, the slate overhang and the cloth cut are all measured off it, so it moves everything outside the cushion at once. t->rail_w.',
             cush:'How tall the cushion is. Decides where the nose sits and how far a ball has to be off the bed to pass over it. t->cushion_h.',
             ball:'The ball itself. Everything above is a multiple of it, so this rescales the whole pocket — which is the point: a pocket that only works at one ball size is not a pocket, it is a coincidence. t->R.',
             round:'Which jaw the table is built with: 0 mitred (American, straight facings) or 1 rounded (English, bezier knuckles). They are different constructions, not a setting on one. t->pocket_round.'};
const PLAY=['gap','pr','off','capm','back'];
const DIG ={set:4, gap:3, rad:3, roll:3, bore:3, bset:3};
let DEF={}, CUR={}, TABLE=null, T={};

function fx(k,v){ return (+v).toFixed(DIG[k]||2); }
function mk(kind){
  const box=document.getElementById('k_'+kind); box.innerHTML='';
  for(const key of KEYS){
    const [lo,hi,st]=RANGE[key];
    const row=document.createElement('div'); row.className='k';
    if(PLAY.includes(key)) row.classList.add('play');
    row.innerHTML=`<label title="${TIP[key]}"><b>${NAME[key]}</b><i>${FIELD[key]}</i></label>
      <input type=range min=${lo} max=${hi} step=${st}>
      <input type=number min=${lo} max=${hi} step=${st}>
      <span class=u>${UNIT[key]}</span>`;
    const [rg,nm]=row.querySelectorAll('input');
    const set=v=>{ CUR[TABLE][kind][key]=+v; sync(kind); draw(kind); emit(); };
    rg.oninput=e=>set(e.target.value);
    nm.onchange=e=>set(e.target.value);
    row.dataset.key=key; box.appendChild(row);
  }
}
function sync(kind){
  for(const row of document.getElementById('k_'+kind).children){
    const k=row.dataset.key, v=CUR[TABLE][kind][k];
    const [rg,nm]=row.querySelectorAll('input');
    rg.value=v; nm.value=fx(k,v);
  }
}
function draw(kind){
  clearTimeout(T[kind]);
  T[kind]=setTimeout(()=>{
    const k=CUR[TABLE][kind];
    const view=document.getElementById('view').value;
    const q=new URLSearchParams({table:TABLE,type:kind,
        zoom:document.getElementById('zoom').value, view:view});
    const sh=document.getElementById('shape').value;
    if(sh!==''){
      q.set('ngon', sh);
      q.set('ngone', sh==='60' ? '10' : '1');
      const ps=document.getElementById('pocket').value;
      if(ps!=='') q.set('pocket', ps);
    }
    if(document.getElementById('lshape').checked){
      q.set('bedl',  document.getElementById('bedl').value);
      q.set('bedw',  document.getElementById('bedw').value);
      q.set('notchx',document.getElementById('notchx').value);
      q.set('notchz',document.getElementById('notchz').value);
      const ps=document.getElementById('pocket').value;
      if(ps!=='') q.set('pocket', ps);
    }
    if(view!=='top'){
      q.set('yaw',  document.getElementById('yaw').value);
      q.set('pitch',document.getElementById('pitch').value);
      q.set('dist', document.getElementById('dist').value);
    }
    for(const key of KEYS) q.set(key,k[key]);
    fetch('/render?'+q).then(async r=>{
      const info=JSON.parse(r.headers.get('X-Readout')||'{}');
      document.getElementById('img_'+kind).src=URL.createObjectURL(await r.blob());
      /* The table tells us how many pockets it has and where each one is, so
         the list is filled from the geometry rather than from an assumption
         about six in a ring. */
      if(info.pockets) fillPockets(info.pockets);
      const g=info.gap_to_drop;
      document.getElementById('mm_'+kind).innerHTML =
        `mouth <b>${info.mouth?.toFixed(1)}</b> mm`+
        ` &nbsp;(<b>${(info.mouth/info.ball).toFixed(2)}</b> balls wide)`+
        ` &nbsp; drop radius <b>${info.drop?.toFixed(1)}</b>`+
        ` &nbsp; lip edge <b>${info.edge?.toFixed(1)}</b>`+
        ` &nbsp; lip thickness <b>${info.thick?.toFixed(1)}</b>`+
        ` &nbsp; the ball floats <b style="color:${Math.abs(g)<1?'#7fe08a':'#ff9a6b'}">`+
        `${g>0?'+':''}${g?.toFixed(1)}</b> mm past the cloth before it drops`;
    });
  },70);
}
function emit(){
  fetch('/source?table='+TABLE,{method:'POST',body:JSON.stringify(CUR[TABLE])})
    .then(r=>r.text()).then(t=>document.getElementById('out').textContent=t);
}
function show(t){
  TABLE=t; mk('corner'); mk('middle'); sync('corner'); sync('middle');
  draw('corner'); draw('middle'); emit();
}
fetch('/state').then(r=>r.json()).then(s=>{
  DEF=s.defaults; CUR=JSON.parse(JSON.stringify(s.current));
  const sel=document.getElementById('table');
  for(const k in DEF){ const o=document.createElement('option');
    o.value=k; o.textContent=DEF[k].label; sel.appendChild(o); }
  sel.onchange=e=>show(e.target.value);
  document.getElementById('zoom').oninput=()=>{draw('corner');draw('middle');};
  /* NAMED BY WHERE THEY ARE. An index is not a thing anybody can point at, so
     each is described by its corner of the table and whether it is a middle —
     which is the only way to say "the one beside the notch" out loud. */
  function pocketName(p, all){
    const far = p.x > 0.001, near = p.x < -0.001;
    const right = p.z > 0.001, left = p.z < -0.001;
    if(p.mid) return 'middle, ' + (right ? 'right rail' : left ? 'left rail'
                                   : (far ? 'far' : 'near') + ' rail');
    return (far ? 'far ' : near ? 'near ' : 'mid ') +
           (right ? 'right' : left ? 'left' : 'centre');
  }
  let POCKETS=[];
  function fillPockets(list){
    if(JSON.stringify(list)===JSON.stringify(POCKETS)) return;
    POCKETS=list;
    const sel=document.getElementById('pocket'), had=sel.value;
    sel.innerHTML='';
    for(const p of list){
      const o=document.createElement('option');
      o.value=p.i; o.textContent=p.i+'  '+pocketName(p,list);
      sel.appendChild(o);
    }
    if(had!=='' && had<list.length) sel.value=had;
  }
  document.getElementById('shape').onchange=()=>{draw('corner');draw('middle');};
  document.getElementById('lshape').onchange=()=>{
    document.getElementById('lwrap').style.display =
      document.getElementById('lshape').checked ? '' : 'none';
    draw('corner'); draw('middle');
  };
  for(const id of ['bedl','bedw','notchx','notchz'])
    document.getElementById(id).onchange=()=>{draw('corner');draw('middle');};
  document.getElementById('pocket').onchange=()=>{draw('corner');draw('middle');};

  /* The view, and the eye when it is not straight down. The defaults are the
     angles the two faults were actually seen from — outside on the diagonal for
     a mitred jaw, inside over the cloth for a middle pocket — and the sliders
     move off there. */
  const camwrap=document.getElementById('camwrap');
  const camtxt=document.getElementById('camtxt');
  function camshow(){
    const v=document.getElementById('view').value;
    camwrap.style.display = (v==='top') ? 'none' : 'inline';
    if(v==='in'){ document.getElementById('yaw').value=180;
                  document.getElementById('pitch').value=8;
                  document.getElementById('dist').value=0.26; }
    if(v==='out'){ document.getElementById('yaw').value=45;
                   document.getElementById('pitch').value=45;
                   document.getElementById('dist').value=0.20; }
    camtext();
  }
  function camtext(){
    camtxt.textContent = 'yaw '+document.getElementById('yaw').value
      +'\u00b0  pitch '+document.getElementById('pitch').value
      +'\u00b0  '+(+document.getElementById('dist').value).toFixed(2)+' m';
  }
  document.getElementById('view').onchange=()=>{camshow();draw('corner');draw('middle');};
  for(const id of ['yaw','pitch','dist'])
    document.getElementById(id).oninput=()=>{camtext();draw('corner');draw('middle');};
  camshow();
  document.getElementById('reset').onclick=()=>{
    CUR[TABLE]={corner:{...DEF[TABLE].corner},middle:{...DEF[TABLE].middle}};
    show(TABLE); };
  document.getElementById('save').onclick=()=>
    fetch('/save',{method:'POST',body:JSON.stringify(CUR)})
      .then(r=>r.text()).then(t=>{document.getElementById('out').textContent=t;});
  show(Object.keys(DEF)[0]);
});
</script>
"""


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/":
            return self._send(200, "text/html; charset=utf-8", PAGE.encode())
        if u.path == "/state":
            return self._send(200, "application/json",
                              json.dumps({"defaults": DEF, "current": CUR}).encode())
        if u.path == "/render":
            png, info = render(q)
            return self._send(200, "image/png", png, {"X-Readout": json.dumps(info)})
        self._send(404, "text/plain", b"no")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        if u.path == "/source":
            t = q.get("table")
            CUR[t] = body
            return self._send(200, "text/plain",
                              source_block(t, DEF[t], body).encode())
        if u.path == "/save":
            CUR.clear()
            CUR.update(body)
            with open(SAVE, "w") as f:
                json.dump(body, f, indent=2)
            out = ["saved %s" % SAVE, ""]
            for t, v in body.items():
                out.append(source_block(t, DEF[t], v))
                out.append("")
            return self._send(200, "text/plain", "\n".join(out).encode())
        self._send(404, "text/plain", b"no")


if __name__ == "__main__":
    if not shutil.which("gcc"):
        raise SystemExit("need gcc")
    build()
    DEF = defaults()
    CUR = {t: {k: dict(DEF[t][k]) for k in ("corner", "middle")} for t in DEF}
    if os.path.exists(SAVE):
        saved = json.load(open(SAVE))
        for t in CUR:
            if t in saved and set(saved[t].get("corner", {})) == set(KEYS):
                CUR[t] = saved[t]
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", PORT), H) as s:
        print("pocket bench on http://127.0.0.1:%d   (ctrl-C to stop)" % PORT)
        s.serve_forever()
