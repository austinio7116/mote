#!/usr/bin/env python3
"""Emit the guide's Weapon Codex section (recipes + effects for the 87 variants),
grouped by weapon family, matching guide.html's table/card classes."""
import os, json, re, html
HERE = os.path.dirname(os.path.abspath(__file__))
import gen_weapons as G   # reuse stats() + DATA + parsers

EFFECT = {
    "fire":   "Burn — fire damage over time",
    "poison": "Poison — damage over time",
    "blood":  "Bleed — DoT + lifesteal",
    "demonic":"Lifesteal on hit",
    "ice":    "Chill — slows the target",
    "nature": "Snare — slows the target",
    "holy":   "—", "arcane": "—", "none": "—",
}
STATION_NAME = {"ST_WORKBENCH":"Workbench","ST_FURNACE":"Furnace","ST_ANVIL":"Anvil","ST_ALTAR":"Demon Altar"}
GROUPS = [   # (title, predicate on (kind, vtype))
    ("Swords &amp; Blades",       lambda k,t: k=="IK_SWORD" and ("sword" in t or "blade" in t or "boomerang" in t)),
    ("Spears &amp; Lances",       lambda k,t: k=="IK_SWORD" and "spear" in t),
    ("Maces, Hammers &amp; Flails",lambda k,t: k=="IK_SWORD" and any(w in t for w in ("mace","hammer","flail","morningstar","warhammer","maul","bludgeon"))),
    ("Axes",                      lambda k,t: k=="IK_AXE"),
    ("Bows &amp; Guns",           lambda k,t: k=="IK_BOW" and any(w in t for w in ("bow","gun","crossbow","rifle","launcher","pistol","cannon","repeater","blaster","onslaught"))),
    ("Staves &amp; Wands",        lambda k,t: k=="IK_BOW" and any(w in t for w in ("staff","wand","rod","scepter","focus","nova"))),
    ("Pickaxes",                  lambda k,t: k=="IK_PICK"),
]

ING_NAME = {"I_HELL_BAR":"hellstone bar", "I_MUSHROOM":"glowshroom"}
def fmt_recipe(recipe):
    st, ings = G.parse_recipe(recipe)
    parts = []
    for iid, n in ings:
        nm = ING_NAME.get(iid, iid[2:].lower().replace("_", " "))
        parts.append(f"{n} {nm}")
    return STATION_NAME[st], " · ".join(parts)

def rows_for(pred):
    out = []
    for idx in sorted(G.DATA):
        vtype, kind, tier, name, elem, recipe = G.DATA[idx]
        if kind == "EXCLUDE": continue
        if not pred(kind, vtype.lower()): continue
        dmg = G.stats(kind, tier, vtype)[0]
        stn, ing = fmt_recipe(recipe)
        out.append((name, EFFECT.get(elem, "—"), dmg, stn, ing, tier))
    # sort by tier then damage
    torder = {"early":0,"prehard":1,"hard":2,"end":3}
    out.sort(key=lambda r: (torder.get(r[5],9), r[2]))
    return out

def table(title, rows):
    h = ['    <div class="tablewrap">', '      <table>',
         f'        <caption>{title}</caption>',
         '        <thead><tr><th>Weapon</th><th>Effect</th><th class="r">Dmg</th><th>Recipe</th></tr></thead>',
         '        <tbody>']
    for name, eff, dmg, stn, ing, tier in rows:
        rec = f'{stn}: {ing}' if ing else stn
        h.append(f'          <tr><td class="item">{html.escape(name.title())}</td>'
                 f'<td>{eff}</td><td class="r">{dmg}</td><td class="want">{html.escape(rec)}</td></tr>')
    h += ['        </tbody>', '      </table>', '    </div>']
    return "\n".join(h)

def main():
    total = sum(1 for i in G.DATA if G.DATA[i][1] != "EXCLUDE")
    s = ['<!-- ============ ARSENAL (generated: gen_guide_weapons.py) ============ -->',
         '<section id="arsenal">',
         '  <p class="eyebrow">09 — The Arsenal</p>',
         '  <h2>Weapon Codex</h2>',
         f'  <p class="lede">{total} craftable weapons beyond the starter tiers, each with its own '
         'damage, elemental effect and recipe. Craft them at the station shown once you have the bars.</p>',
         '  <div class="tips" style="margin-top:18px">',
         '    <div class="tip"><span class="n">◈</span><p><b>Elemental effects:</b> '
         '<strong>Burn/Poison/Bleed</strong> deal damage over time, <strong>Chill/Snare</strong> slow the '
         'enemy, and <strong>Lifesteal</strong> (demonic/blood) heals you on hit. Bows fire in spreads; '
         'axes chop trees <em>and</em> swing at foes.</p></div>',
         '  </div>',
         '  <div class="grid g2" style="margin-top:20px;align-items:start">']
    for title, pred in GROUPS:
        rows = rows_for(pred)
        if rows: s.append(table(title, rows))
    s += ['  </div>', '</section>', '']
    frag = "\n".join(s)
    open(os.path.join(HERE, "guide_arsenal.html"), "w").write(frag)
    print("[guide] arsenal section: %d weapons across %d groups -> assets/guide_arsenal.html"
          % (total, sum(1 for t,p in GROUPS if rows_for(p))))

if __name__ == "__main__":
    main()
