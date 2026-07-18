#!/usr/bin/env python3
"""Emit the guide's Weapon Codex — ONE sortable table, each row showing the
weapon's actual in-game sprite (embedded as a data-URI so the guide stays
self-contained). Matches guide.html's table classes; adds a tiny sprite style."""
import os, json, io, base64, html
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
import gen_weapons as G
import weapon_variants as WV

BIG = Image.open(os.path.join(HERE, "weapons_big.png")).convert("RGBA")   # FINAL baked art (incl. overrides), 1 row of 32px cells
EFFECT = {
    "fire":"Burn","poison":"Poison","blood":"Bleed + lifesteal","demonic":"Lifesteal",
    "ice":"Chill (slow)","nature":"Snare (slow)","holy":"—","arcane":"—","none":"—",
}
FAMILY = [
    ("Sword", lambda k,t: k=="IK_SWORD" and ("sword" in t or "blade" in t or "boomerang" in t)),
    ("Spear", lambda k,t: k=="IK_SWORD" and "spear" in t),
    ("Mace",  lambda k,t: k=="IK_SWORD"),   # remaining melee (hammer/flail/mace)
    ("Axe",   lambda k,t: k=="IK_AXE"),
    ("Bow/Gun",lambda k,t: k=="IK_BOW" and any(w in t for w in ("bow","gun","crossbow","rifle","launcher","pistol","cannon","repeater","blaster","onslaught"))),
    ("Staff", lambda k,t: k=="IK_BOW"),
    ("Pick",  lambda k,t: k=="IK_PICK"),
]
STATION = {"ST_WORKBENCH":"Workbench","ST_FURNACE":"Furnace","ST_ANVIL":"Anvil","ST_ALTAR":"Demon Altar"}
ING_NAME = {"I_HELL_BAR":"hellstone bar","I_MUSHROOM":"glowshroom"}

# ingredient icons come from the game's items.png (cell index == item id)
from extract_sheets import ITEM_IDS
ITEMS_IMG = Image.open(os.path.join(HERE, "items.png")).convert("RGBA")
# mob-drop materials live past the generated weapon-variant gap, so they aren't
# in ITEM_IDS (which only maps the base items 0..70) — their icon cell == item id
MAT_ID = {"STINGER": 169, "SOUL": 170, "CURSED_CHUNK": 171}
def _icon_uri(item_name):
    name = item_name[2:] if item_name.startswith("I_") else item_name
    i = MAT_ID.get(name, None)
    if i is None: i = ITEM_IDS.index(name)
    c = ITEMS_IMG.crop(((i % 8) * 16, (i // 8) * 16, (i % 8) * 16 + 16, (i // 8) * 16 + 16))
    b = io.BytesIO(); c.save(b, "PNG")
    return "data:image/png;base64," + base64.b64encode(b.getvalue()).decode()
TORDER = {"early":0,"prehard":1,"hard":2,"end":3}
TIER_LABEL = {"early":"Early","prehard":"Pre-hard","hard":"Hardmode","end":"Endgame"}

def family(kind, vtype):
    for name, pred in FAMILY:
        if pred(kind, vtype): return name
    return "Weapon"

def sprite_uri(big_cell):
    c = BIG.crop((big_cell * 32, 0, big_cell * 32 + 32, 32))   # row 0 = right-facing
    import numpy as np
    a = np.array(c.convert("RGBA"))
    key = (a[..., 0] >= 248) & (a[..., 1] < 8) & (a[..., 2] >= 248)   # magenta colour-key
    a[key, 3] = 0
    b = io.BytesIO(); Image.fromarray(a).save(b, "PNG")
    return "data:image/png;base64," + base64.b64encode(b.getvalue()).decode()

USED_INGS = set()
def fmt_recipe(recipe):
    """station name + icon-rich HTML: <icon> xN per ingredient (name in title=)"""
    st, ings = G.parse_recipe(recipe)
    spans = []
    for iid, n in ings:
        USED_INGS.add(iid)
        nm = ING_NAME.get(iid, iid[2:].lower().replace('_', ' '))
        spans.append(f'<span class="ig g-{iid[2:].lower()}" title="{nm}"></span>{n}')
    return STATION[st], " ".join(spans)

def main():
    rows = []
    for vi, idx in enumerate(sorted(i for i in G.DATA if G.DATA[i][1] != "EXCLUDE")):
        vtype, kind, tier, name, elem, recipe = G.DATA[idx]
        ti = WV.VARIANTS[vi][3]        # this variant's cell in the baked weapons_big.png
        stn, ing = fmt_recipe(recipe)
        rows.append((name.title(), family(kind, vtype.lower()), EFFECT.get(elem,"—"),
                     G.stats(kind, tier, vtype, recipe)[0], tier, f"{stn}: {ing}" if ing else stn,
                     sprite_uri(ti)))
    rows.sort(key=lambda r: (TORDER.get(r[4],9), r[1], -r[3]))
    out = ['<!-- ============ ARSENAL (generated: gen_guide_weapons.py) ============ -->',
        '<section id="arsenal">',
        '  <style>',
        '    #arsenal img.ws{width:34px;height:34px;image-rendering:pixelated;vertical-align:middle;'
        'background:rgba(128,128,128,.14);border-radius:4px}',
        '    #arsenal td.sp{width:40px;padding:2px 4px}',
        '    #arsenal .tier{font-size:.72em;letter-spacing:.06em;text-transform:uppercase;opacity:.7}',
        '    #arsenal .ig{width:18px;height:18px;display:inline-block;vertical-align:middle;'
        'image-rendering:pixelated;background-size:cover;margin:0 1px 0 6px}',
        '__ING_STYLES__',
        '  </style>',
        '  <p class="eyebrow">09 — The Arsenal</p>',
        '  <h2>Weapon Codex</h2>',
        f'  <p class="lede">{len(rows)} craftable weapons beyond the starter tiers — each its own sprite, '
        'elemental effect, damage and recipe. <strong>Burn / Poison / Bleed</strong> deal damage over time, '
        '<strong>Chill / Snare</strong> slow the target, and <strong>Lifesteal</strong> heals you on hit. '
        'Ordered by progression tier.</p>',
        '  <div class="tablewrap" style="margin-top:20px">',
        '    <table>',
        '      <thead><tr><th></th><th>Weapon</th><th>Type</th><th>Effect</th>'
        '<th class="r">Dmg</th><th>Tier</th><th>Recipe</th></tr></thead>',
        '      <tbody>']
    for name, fam, eff, dmg, tier, rec, uri in rows:
        out.append(
            f'        <tr><td class="sp"><img class="ws" alt="{html.escape(name)}" src="{uri}"></td>'
            f'<td class="item">{html.escape(name)}</td><td>{fam}</td><td>{eff}</td>'
            f'<td class="r">{dmg}</td><td class="tier">{TIER_LABEL[tier]}</td>'
            f'<td class="want">{rec}</td></tr>')
    out += ['      </tbody>', '    </table>', '  </div>', '</section>', '']
    frag = "\n".join(out)
    ing_css = "\n".join("    #arsenal .g-%s{background-image:url(%s)}" % (iid[2:].lower(), _icon_uri(iid))
                        for iid in sorted(USED_INGS))
    frag = frag.replace("__ING_STYLES__", ing_css)      # placeholder sits inside the <style> block
    open(os.path.join(HERE, "guide_arsenal.html"), "w").write(frag)
    print("[guide] single-table codex with sprites: %d weapons -> assets/guide_arsenal.html" % len(rows))

if __name__ == "__main__":
    main()
