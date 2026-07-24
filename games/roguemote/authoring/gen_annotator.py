#!/usr/bin/env python3
"""
Generate a self-contained interactive sprite ANNOTATOR (Artifact-ready HTML).
Reuses the catalogue's tiles + my guesses as pre-fills; the developer corrects
category + name per tile (with bulk row/col/sheet apply + keyboard nav), then
Exports a JSON blob that feeds back into extract.py.  Output: /tmp/roguemote_annotator.html
"""
import os, io, json, base64
import gen_catalogue as gc          # SECTIONS, tile_datauri, COLORS

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
# default category per section title (first token before " (")
SEC_CAT = {
    "Chests":"container","Stone furniture":"furniture","Faces · skulls · keys":"key",
    "Runes":"prop","Doors · gems · banners":"door","Light props & structures":"prop",
    "Trinkets & small nature":"prop","Weapons & potions":"weapon","Food":"consumable",
    "Treasure & ore":"treasure","Loot furniture & bones":"prop","Tools & wands":"tool",
    "Elemental weapons":"weapon","Guns":"weapon","Large portrait":"npc","Characters":"npc",
    "Animals & vermin":"animal","Monsters":"enemy","Crowns · armour · FX":"treasure",
    "Bosses":"boss","Boulders & mountains":"feature","Tiny UI icons (magenta strip)":"ui",
    "Arrows & gauges":"ui","Button prompts":"ui","Status · emotes · elements":"ui",
    "Symbols":"ui","White furniture (top-down)":"furniture","Blueprint tiles":"ui",
    "Colour panels (terrain)":"ui","Purple brick wall (terrain)":"wall",
    "Stone-brick wall set (terrain)":"wall","Temple / aztec wall (terrain)":"wall",
    "Jungle grass & steps (terrain)":"floor","Cobblestone floors (terrain)":"floor",
    "Terrain edges (terrain)":"feature",
}

def build():
    # agent identifications (batches/out_*.json merged) override my earlier guesses
    ai = {}
    ap = os.path.join(os.path.dirname(os.path.abspath(__file__)), "labels_ai.json")
    if os.path.exists(ap): ai = json.load(open(ap))
    tiles = []          # flat, in section order
    sections = []       # {title, blurb, ids:[...]}
    for (c0,r0,c1,r1,title,blurb,use,labeler) in gc.SECTIONS:
        defcat = SEC_CAT.get(title.split(" (")[0], "unknown")
        ids = []
        for r in range(r0,r1+1):
            for c in range(c0,c1+1):
                uri, ink = gc.tile_datauri(c,r)
                if not ink: continue
                res = labeler(c,r)
                if res is None: continue
                name, _use, _q = res
                cat = defcat; conf = ""
                if title == "Characters" and c==32 and r==0: cat, name = "player","@ player glyph"
                a = ai.get(f"{c},{r}")
                if a:                              # prefer the agent label
                    cat = a.get("cat", cat); name = a.get("name") or name; conf = a.get("conf","")
                ids.append(len(tiles))
                tiles.append({"c":c,"r":r,"u":uri,"cat":cat,"name":name,"conf":conf})
        if ids: sections.append({"t":title,"b":blurb,"ids":ids})
    # font glyphs individually (so they can be named too, but default font)
    fids=[]
    for i in range(128):
        cc, rr = 48+(i%16), (i//16)
        uri, ink = gc.tile_datauri(cc,rr)
        if not ink and i!=32: continue
        fids.append(len(tiles))
        tiles.append({"c":cc,"r":rr,"u":uri,"cat":"font","name":f"CP437 #{i}" + (" (space)" if i==32 else ""),"conf":"high"})
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
.card select{width:100%;font-size:11px;padding:2px;border:1px solid var(--line);border-radius:4px;background:var(--bg)}
.card input.nm{width:100%;font-size:12px;padding:3px 4px;border:1px solid var(--line);border-radius:4px;background:var(--bg)}
.card input.nm:focus,.card select:focus{outline:2px solid var(--sel);outline-offset:-1px}
.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--ink);color:var(--bg);padding:8px 14px;border-radius:8px;font-size:13px;opacity:0;transition:opacity .2s;z-index:50;pointer-events:none}
.toast.show{opacity:.95}
.help{color:var(--muted);font-size:12px;max-width:80ch;margin:14px 0 0}
.help kbd{font-family:ui-monospace,Menlo,monospace;background:var(--panel2);border:1px solid var(--line);border-radius:4px;padding:0 4px}
dialog{background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:10px;max-width:640px;width:92%}
dialog textarea{width:100%;height:220px;background:var(--bg);color:var(--ink);border:1px solid var(--line);border-radius:6px;font-family:ui-monospace,Menlo,monospace;font-size:12px;padding:8px}
</style>

<header class="top">
 <div class="row1">
   <h1><b>Roguemote</b> sprite annotator</h1>
   <div class="progwrap"><div class="prog" id="prog"></div></div>
   <span class="pnum" id="pnum">0 / 0 reviewed</span>
   <button class="btn pri" id="export">Export JSON</button>
   <button class="btn" id="import">Import</button>
   <button class="btn" id="reset">Reset</button>
 </div>
 <div class="tools">
   <span class="selinfo">Selected: <b id="selcount">0</b></span>
   <div class="grp"><label>Name selection</label><input type="text" id="bulkname" placeholder="e.g. skeleton {i}"><button class="btn" id="applyname">Apply</button></div>
   <div class="grp"><label>Select</label>
     <button class="btn" id="selrow">row</button>
     <button class="btn" id="selcol">col</button>
     <button class="btn" id="selsheet">sheet</button>
     <button class="btn" id="selnone">none</button></div>
   <label><input type="checkbox" id="onlyunrev"> only unreviewed</label>
 </div>
 <div class="cats" id="cats"></div>
 <nav class="secs" id="secnav"></nav>
</header>
<main id="main"></main>
<p class="help" style="max-width:1250px;margin:10px auto;padding:0 16px">
 Every label below is a <b>detailed identification pass by vision agents</b> (each studied one
 zoomed section). A <b style="color:var(--torch)">?</b> after a coordinate = the agent marked it
 <b>low confidence</b> &mdash; skim those first. <b>How to drive it fast:</b> click a sprite to select it (the <b style="color:var(--gold)">gold</b> anchor);
 <kbd>&larr;&uarr;&darr;&rarr;</kbd> move · <kbd>Shift</kbd>+click or <kbd>Shift</kbd>+arrows range-select ·
 press a category <b>hotkey</b> (shown on each chip) to stamp the selection · <kbd>Enter</kbd> edit the name,
 <kbd>Enter</kbd> again jumps to the next · <kbd>N</kbd> next unreviewed. Use <b>Select row/col/sheet</b> +
 a category chip or the <b>Name selection</b> box (<span class="mono">{i}</span> = auto-number) for whole
 families at once. Everything autosaves in your browser — <b>Export JSON</b> when done and paste it back to me.
</p>
<div class="toast" id="toast"></div>
<dialog id="iodlg"><form method="dialog"><h3 id="iotitle" style="margin:0 0 8px"></h3>
 <textarea id="iotext"></textarea>
 <div style="display:flex;gap:8px;justify-content:flex-end;margin-top:10px">
   <button class="btn" value="cancel">Close</button>
   <button class="btn pri" id="iook" value="ok" type="button">Load</button></div></form></dialog>

<script>
/*__DATA__*/
const KEY="roguemote_annot_v1";
const CATBY={}; APP.cats.forEach(c=>CATBY[c[0]]=c);
const HOTKEY={}; APP.cats.forEach(c=>HOTKEY[c[2]]=c[0]);
// state: per tile {cat,name,rev}. seed from APP.tiles then overlay saved.
let ST = APP.tiles.map(t=>({cat:t.cat,name:t.name,rev:false}));
try{const s=JSON.parse(localStorage.getItem(KEY)||"null");
 if(s&&s.length===ST.length) ST=s;}catch(e){}
let anchor=0; const sel=new Set();

const main=document.getElementById('main');
const cardEl=[]; // index -> element
function catHue(id){return CATBY[id]?CATBY[id][3]:0;}
APP.sections.forEach((s,si)=>{
  const sec=document.createElement('section'); sec.id='s'+si;
  sec.innerHTML=`<div class="sh"><div><h2>${s.t}</h2><div class="b">${s.b}</div></div>
    <button class="btn selall" data-si="${si}">select sheet</button></div>`;
  const g=document.createElement('div'); g.className='grid';
  s.ids.forEach(id=>{
    const t=APP.tiles[id], st=ST[id];
    const fig=document.createElement('figure'); fig.className='card'; fig.dataset.id=id;
    fig.style.setProperty('--h',catHue(st.cat));
    fig.innerHTML=`<div class="thumb"><img src="${t.u}" alt=""></div>
      <div class="meta"><span class="coord">${t.c},${t.r}</span>
      <select class="catsel">${APP.cats.map(c=>`<option value="${c[0]}">${c[1]}</option>`).join('')}</select>
      <input class="nm" type="text" value=""></div>`;
    if(t.conf==='low') fig.classList.add('q');
    g.appendChild(fig); cardEl[id]=fig;
    fig.querySelector('.catsel').value=st.cat;
    fig.querySelector('.nm').value=st.name;
    fig.querySelector('.thumb').addEventListener('mousedown',e=>{e.preventDefault();pick(id,e);});
    fig.querySelector('.catsel').addEventListener('change',e=>{setCat([id],e.target.value);});
    fig.querySelector('.nm').addEventListener('input',e=>{ST[id].name=e.target.value;ST[id].rev=true;save();});
    fig.querySelector('.nm').addEventListener('keydown',e=>{
      if(e.key==='Enter'){e.preventDefault();ST[id].rev=true;const nx=id+1;if(cardEl[nx]){pick(nx);cardEl[nx].querySelector('.nm').focus();}}
      if(e.key==='Escape'){e.target.blur();}
    });
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
function setCat(ids,cat){ids.forEach(i=>{ST[i].cat=cat;ST[i].rev=true;cardEl[i].style.setProperty('--h',catHue(cat));cardEl[i].querySelector('.catsel').value=cat;});save();render();toast(ids.length+' → '+CATBY[cat][1]);}
function applyName(){const pat=document.getElementById('bulkname').value;const ids=selArr();if(!ids.length||!pat)return;
  ids.forEach((i,k)=>{const nm=pat.replace('{i}',k+1);ST[i].name=nm;ST[i].rev=true;cardEl[i].querySelector('.nm').value=nm;});save();render();toast('named '+ids.length);}

function render(){
  const only=document.getElementById('onlyunrev').checked;
  cardEl.forEach((el,i)=>{el.classList.toggle('sel',sel.has(i));el.classList.toggle('anchor',i===anchor&&!sel.size||sel.size&&i===anchor);
    el.classList.toggle('hide',only&&ST[i].rev);});
  document.getElementById('selcount').textContent=sel.size||(anchor!=null?1:0);
  const rev=ST.filter(s=>s.rev).length;
  document.getElementById('prog').style.width=(100*rev/ST.length)+'%';
  document.getElementById('pnum').textContent=rev+' / '+ST.length+' reviewed';
}
let saveT; function save(){clearTimeout(saveT);saveT=setTimeout(()=>localStorage.setItem(KEY,JSON.stringify(ST)),250);}
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
document.getElementById('export').onclick=()=>{
  const out={version:1,source:"simple-roguelike-tileset v0.16 CC0",
    tiles:APP.tiles.map((t,i)=>({c:t.c,r:t.r,cat:ST[i].cat,name:ST[i].name,rev:ST[i].rev}))};
  const txt=JSON.stringify(out,null,1);
  document.getElementById('iotitle').textContent="Export — copy this, or it downloads automatically";
  document.getElementById('iotext').value=txt;document.getElementById('iook').style.display='none';
  try{const b=new Blob([txt],{type:'application/json'});const a=document.createElement('a');
    a.href=URL.createObjectURL(b);a.download='roguemote_labels.json';a.click();}catch(e){}
  try{navigator.clipboard.writeText(txt);toast('copied to clipboard');}catch(e){}
  dlg.showModal();
};
document.getElementById('import').onclick=()=>{document.getElementById('iotitle').textContent="Import — paste a previously exported JSON";
  document.getElementById('iotext').value='';document.getElementById('iook').style.display='';dlg.showModal();};
document.getElementById('iook').onclick=()=>{try{const o=JSON.parse(document.getElementById('iotext').value);
  const by={};(o.tiles||o).forEach(t=>by[t.c+','+t.r]={cat:t.cat,name:t.name,rev:t.rev!==false});
  APP.tiles.forEach((t,i)=>{const k=by[t.c+','+t.r];if(k){ST[i].cat=k.cat;ST[i].name=k.name;ST[i].rev=k.rev;
    cardEl[i].querySelector('.catsel').value=k.cat;cardEl[i].querySelector('.nm').value=k.name;cardEl[i].style.setProperty('--h',catHue(k.cat));}});
  save();render();dlg.close();toast('imported');}catch(e){alert('Bad JSON: '+e.message);}};
document.getElementById('reset').onclick=()=>{if(confirm('Discard all your edits and restore my original guesses?')){
  ST=APP.tiles.map(t=>({cat:t.cat,name:t.name,rev:false}));localStorage.removeItem(KEY);
  APP.tiles.forEach((t,i)=>{cardEl[i].querySelector('.catsel').value=ST[i].cat;cardEl[i].querySelector('.nm').value=ST[i].name;cardEl[i].style.setProperty('--h',catHue(ST[i].cat));});render();}};

render();
</script>
"""

if __name__ == "__main__":
    build()
