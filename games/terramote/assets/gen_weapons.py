#!/usr/bin/env python3
"""TerraMote weapon-variant GENERATOR.

Turns assets/weapon_class.json (the classified sprites from
sources_sheet2_weapons_big.png) into:
  - src/weapon_ids.inc      item-enum entries      (#included in terra.h)
  - src/weapon_defs.inc     g_items[] rows         (#included in items.c)
  - src/weapon_fx.inc       g_wfx[] rows           (#included in items.c)
  - src/weapon_bigcell.inc  big_cell() cases       (#included in player.c)
  - src/weapon_recipes.inc  g_recipes[] rows       (#included in items.c)
  - assets/weapon_variants.py  ROSTER for extract_sheets (asset gen)

Each variant is a distinct craftable item with its own stats + elemental
WeaponFx + recipe. Standard tiers (cells 0-15) stay hand-written; variants get
weapons_big cells 16.. in this file's order.
"""
import os, json, re

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
DATA = {int(k): v for k, v in json.load(open(os.path.join(HERE, "weapon_class.json"))).items()}
BASE_ID = 71          # first free item id (must match terra.h I_COUNT before variants)
STD_CELLS = 16        # standard weapons occupy weapons_big cells 0..15

EL = {"none":"EL_NONE","fire":"EL_FIRE","ice":"EL_ICE","poison":"EL_POISON","holy":"EL_HOLY",
      "demonic":"EL_DEMONIC","arcane":"EL_ARCANE","blood":"EL_BLOOD","nature":"EL_NATURE"}
STATION = {"WORKBENCH":"ST_WORKBENCH","FURNACE":"ST_FURNACE","ANVIL":"ST_ANVIL","ALTAR":"ST_ALTAR"}
ING = {"WOOD":"I_WOOD","STONE":"I_STONE","GEL":"I_GEL","LENS":"I_LENS","GLOWSHROOM":"I_MUSHROOM",
       "MUSHROOM":"I_MUSHROOM","COPPER_BAR":"I_COPPER_BAR","IRON_BAR":"I_IRON_BAR","GOLD_BAR":"I_GOLD_BAR",
       "DEMONITE_BAR":"I_DEMONITE_BAR","HELL_BAR":"I_HELL_BAR","OBSIDIAN":"I_OBSIDIAN","HELLSTONE":"I_HELLSTONE"}

import zlib
# tier -> melee damage BAND (lo, hi). Every weapon rolls its own point in the
# band from a stable hash of its name, then its TYPE reshapes the trade-offs —
# so no two weapons play the same.
TIER_BAND = {"early":(6,13), "prehard":(13,21), "hard":(21,33), "end":(33,50)}

def _roll(name, salt, lo, hi):
    """Deterministic per-weapon roll in [lo,hi] (crc32 is stable across runs)."""
    h = zlib.crc32((name + "/" + str(salt)).encode()) & 0xffff
    return lo + h % (hi - lo + 1)

def stats(kind, tier, vtype, name=""):
    lo, hi = TIER_BAND[tier]
    dmg = _roll(name, "d", lo, hi)
    spd = _roll(name, "s", 16, 24)                    # use-time frames (lower = faster)
    kn  = _roll(name, "k", 105, 175)
    reach = 0; nshot = 0; spread = 0; power = 0
    t = vtype.lower()
    heavy = any(w in t for w in ("hammer","mace","flail","maul","morningstar","bludgeon","warhammer"))
    spear = "spear" in t or "lance" in t or "pike" in t
    if kind == "IK_BOW":
        kn = 0
        if any(w in t for w in ("gun","scatter","repeater","cannon","blaster","onslaught","pistol","rifle","launcher","musket","minishark","autocannon")):
            nshot = _roll(name, "n", 2, 4)            # sprays: more pellets, weaker each
            spread = _roll(name, "sp", 14, 28)
            dmg = max(3, int(dmg * 0.45)); spd = _roll(name, "s2", 12, 18)
        elif "crossbow" in t:
            nshot = 2; spread = 10
            dmg = max(4, int(dmg * 0.65)); spd = _roll(name, "s2", 18, 24)
        elif any(w in t for w in ("staff","wand","rod","scepter","focus","nova")):
            dmg = int(dmg * 0.95)                     # staves: heavy single bolts, slow
            spd = _roll(name, "s2", 22, 30)
        else:                                          # bows
            dmg = max(4, int(dmg * 0.7)); spd = _roll(name, "s2", 15, 21)
            if tier in ("hard", "end"): nshot = 2; spread = 10
    elif kind == "IK_PICK":
        power = 7 if tier == "hard" else 9
        dmg = max(5, dmg // 2); kn = 90; spd = _roll(name, "s2", 13, 17)
    elif kind == "IK_AXE":
        power = 5 if tier in ("early", "prehard") else 7
        dmg = int(dmg * 1.05); spd = _roll(name, "s2", 19, 26)   # chunky swings
        kn = _roll(name, "k2", 130, 185)
        if tier in ("hard", "end"): reach = 1
    else:  # melee
        if heavy:                                      # slow, brutal, big knockback
            dmg = int(dmg * 1.3); spd = _roll(name, "s2", 24, 30)
            kn = _roll(name, "k2", 160, 210); reach = 1 if tier in ("hard", "end") else 0
        elif spear:                                    # fast, light, long
            dmg = max(4, int(dmg * 0.85)); spd = _roll(name, "s2", 13, 17)
            reach = 1 + (1 if tier in ("hard", "end") else 0)
        else:                                          # swords: balanced
            if tier == "hard": reach = 1
            if tier == "end":  reach = 2
    return dmg, spd, kn, reach, nshot, spread, power

def parse_recipe(s):
    st, rest = s.split(":", 1)
    st = STATION[st.strip()]
    ings = []
    for part in rest.split("+"):
        part = part.strip()
        m = re.match(r"([A-Z_]+)\s*x?(\d+)", part)
        if not m: continue
        name, n = m.group(1), int(m.group(2))
        if name in ING: ings.append((ING[name], n))
    return st, ings[:3]

def suffix(name, seen):
    s = re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")
    base = s; i = 2
    while s in seen: s = f"{base}_{i}"; i += 1
    seen.add(s); return s

def main():
    ids, defs, fx, cells, recs, roster = [], [], [], [], [], []
    seen = set()
    order = [i for i in sorted(DATA) if DATA[i][1] != "EXCLUDE"]
    for vi, idx in enumerate(order):
        vtype, kind, tier, name, elem, recipe = DATA[idx]
        suf = suffix(name, seen)
        iid = "I_" + suf
        dmg, spd, kn, reach, nshot, spread, power = stats(kind, tier, vtype, name)
        cell = STD_CELLS + vi
        item_id = BASE_ID + vi
        ids.append(iid)
        defs.append(f'    [{iid}] = {{ "{name.upper()}", {kind}, IC({iid}), 0, {power}, {dmg}, 1, {spd} }},')
        fx.append(f'    [{iid}] = {{ {EL[elem]}, {kn}, {reach}, {nshot}, {spread} }},')
        cells.append(f'    case {iid}: return {cell};')
        st, ings = parse_recipe(recipe)
        ing_s = ", ".join(f"{{ {n}, {c} }}" for n, c in ings)   # Recipe.in = { item, n }
        recs.append(f'    {{ {st}, {iid}, 1, {{ {ing_s} }} }},')
        roster.append((suf, idx, item_id, cell))
    sd = os.path.join(GAME, "src")
    open(os.path.join(sd, "weapon_ids.inc"), "w").write(",\n".join("    " + i for i in ids) + ",\n")
    open(os.path.join(sd, "weapon_defs.inc"), "w").write("\n".join(defs) + "\n")
    open(os.path.join(sd, "weapon_fx.inc"), "w").write("\n".join(fx) + "\n")
    open(os.path.join(sd, "weapon_bigcell.inc"), "w").write("\n".join(cells) + "\n")
    open(os.path.join(sd, "weapon_recipes.inc"), "w").write("\n".join(recs) + "\n")
    with open(os.path.join(HERE, "weapon_variants.py"), "w") as f:
        f.write("# GENERATED by gen_weapons.py — (enum_suffix, tools_idx, item_id, big_cell)\n")
        f.write("BASE_ID = %d\nSTD_CELLS = %d\n" % (BASE_ID, STD_CELLS))
        f.write("VARIANTS = [\n")
        for suf, idx, iid, cell in roster:
            f.write(f"    ({suf!r}, {idx}, {iid}, {cell}),\n")
        f.write("]\n")
    print("[gen] %d weapon variants -> src/weapon_*.inc + assets/weapon_variants.py" % len(roster))

if __name__ == "__main__":
    main()
