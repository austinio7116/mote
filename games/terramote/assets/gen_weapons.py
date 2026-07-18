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
       "DEMONITE_BAR":"I_DEMONITE_BAR","HELL_BAR":"I_HELL_BAR","OBSIDIAN":"I_OBSIDIAN","HELLSTONE":"I_HELLSTONE",
       "STINGER":"I_STINGER","SOUL":"I_SOUL","CURSED_CHUNK":"I_CURSED_CHUNK"}

# --- damage is EARNED, not rolled: it derives from the recipe's cost ----------
# per-unit ingredient value (roughly: how hard is one of these to get?)
ING_VALUE = {
    "I_WOOD": 0.3, "I_STONE": 0.3, "I_GEL": 0.4, "I_MUSHROOM": 1.0, "I_LENS": 1.5,
    "I_COPPER_BAR": 1.0, "I_IRON_BAR": 1.5, "I_GOLD_BAR": 2.2,
    "I_OBSIDIAN": 2.0, "I_HELLSTONE": 2.5, "I_DEMONITE_BAR": 3.2, "I_HELL_BAR": 4.0,
    # mob-drop materials: worth hunting for (value ≈ how dangerous the mob is)
    "I_STINGER": 1.8, "I_CURSED_CHUNK": 2.6, "I_SOUL": 3.5,
}
STATION_MULT = {"ST_WORKBENCH": 1.0, "ST_FURNACE": 1.0, "ST_ANVIL": 1.0, "ST_ALTAR": 1.1}

def recipe_cost(recipe):
    """Total ingredient value of a recipe string (the price you pay = the power
    you get). Altar recipes get a small premium (corruption-gated)."""
    st, ings = parse_recipe(recipe)
    cost = sum(ING_VALUE.get(iid, 1.0) * n for iid, n in ings)
    return cost * STATION_MULT[st]

def stats(kind, tier, vtype, recipe=""):
    cost = recipe_cost(recipe) if recipe else 10.0
    base = min(55, max(5, 4.0 + 0.72 * cost))          # cost -> damage backbone
    reach = 0; nshot = 0; spread = 0; power = 0
    t = vtype.lower()
    heavy = any(w in t for w in ("hammer","mace","flail","maul","morningstar","bludgeon","warhammer"))
    spear = "spear" in t or "lance" in t or "pike" in t
    if kind == "IK_BOW":
        kn = 0
        if any(w in t for w in ("gun","scatter","repeater","cannon","blaster","onslaught","pistol","rifle","launcher","musket","minishark","autocannon")):
            nshot = 2 + (cost > 25) + (cost > 40)      # pricier guns spray more pellets
            spread = 12 + nshot * 4
            dmg = max(3, int(base * 0.45)); spd = 14   # fast trigger, weak pellets
        elif "crossbow" in t:
            nshot = 2; spread = 10
            dmg = max(4, int(base * 0.65)); spd = 20
        elif any(w in t for w in ("staff","wand","rod","scepter","focus","nova")):
            dmg = int(base * 0.95); spd = 25           # slow, heavy bolts
        else:                                           # bows
            dmg = max(4, int(base * 0.7)); spd = 17
            if cost > 25: nshot = 2; spread = 10
    elif kind == "IK_PICK":
        power = 7 if tier == "hard" else 9
        dmg = max(5, int(base * 0.5)); kn = 90; spd = 14
    elif kind == "IK_AXE":
        power = 5 if tier in ("early", "prehard") else 7
        dmg = int(base * 1.05); spd = 22               # chunky swings
        kn = 150 + int(cost / 3)
        if tier in ("hard", "end"): reach = 1
    elif heavy:                                         # slow, brutal, big knockback
        dmg = int(base * 1.25); spd = 26
        kn = 180 + int(cost / 3); reach = 1 if tier in ("hard", "end") else 0
    elif spear:                                         # fast, light, long
        dmg = max(4, int(base * 0.85)); spd = 14; kn = 115
        reach = 1 + (1 if tier in ("hard", "end") else 0)
    else:                                               # swords: balanced
        dmg = int(base); spd = 18; kn = 135 + int(cost / 4)
        if tier == "hard": reach = 1
        if tier == "end":  reach = 2
    return dmg, spd, min(210, kn), reach, nshot, spread, power

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
        dmg, spd, kn, reach, nshot, spread, power = stats(kind, tier, vtype, recipe)
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
