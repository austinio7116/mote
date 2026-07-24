#!/usr/bin/env python3
"""Fold a human correction pass (an annotator Export) into labels_human.json.

labels_human.json layers *over* labels_ai.json, so re-running merge_labels.py to
regenerate the agent guesses never clobbers a human decision.

Two kinds of propagation run over the corrections, because the source sheet
repeats every object family:

  1. TWINS  - some (col,row) tiles sit inside two overlapping subsheet regions
     (props_light x trinkets, grass_garden x wall_temple), so the annotator
     shows them twice. Correcting one occurrence has to fix the other; it is
     literally the same 8x8 tile, so this is always safe and fully automatic.

  2. COLOUR BLOCKS - most object families are a column repeated down five
     colour rows (red / blue / gold / green / grey-white). Correcting the red
     row should carry to the other four with the colour word swapped. This is
     NOT inferred: each block below was checked against source_tileset.png by
     eye before being added here. Only add a block once you have looked at it.

Usage:
    python3 apply_labels.py roguemote_labels.json      # an annotator export
    python3 apply_labels.py --report                   # show current state
"""
import os, sys, json

HERE = os.path.dirname(os.path.abspath(__file__))
AI = os.path.join(HERE, "labels_ai.json")
HUMAN = os.path.join(HERE, "labels_human.json")

VALID = {"player","npc","enemy","boss","animal","weapon","armor","consumable","treasure",
         "key","tool","container","furniture","prop","door","floor","wall","feature",
         "ui","fx","font","unknown"}

# --- colour-row blocks, each verified against the source sheet by eye ---------
# rows maps row -> (primary colour, [aliases to match in a name, longest first])
COLOUR_BLOCKS = [
    {
        "note": "cols 11-12 = door closed/open, 13-14 = house/shop + its right half; 5 colour rows",
        "cols": [11, 12, 13, 14],
        "rows": {
            1: ("red",        ["pink/red", "red", "pink"]),
            2: ("blue",       ["blue"]),
            3: ("gold",       ["gold/yellow", "yellow", "gold"]),
            4: ("green",      ["green"]),
            5: ("grey/white", ["grey/white", "silver/white", "white", "grey"]),
        },
    },
]

def load(p, default):
    return json.load(open(p)) if os.path.exists(p) else default

def twin_map():
    """(c,r) -> how many annotator cards show it. Needs the catalogue sections.
    Multi-tile sprites draw on their anchor's card only, so members count 0."""
    import gen_catalogue as gc
    from gen_annotator import GRP_AT
    from collections import defaultdict
    seen = defaultdict(int)
    for (c0, r0, c1, r1, title, blurb, use, labeler) in gc.SECTIONS:
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                g = GRP_AT.get((c, r))
                if g and (c, r) != (g[0], g[1]): continue     # drawn by its anchor
                if not gc.tile_datauri(c, r)[1]: continue
                if labeler(c, r) is None: continue
                seen[(c, r)] += 1
    return seen

def block_for(c, r):
    for b in COLOUR_BLOCKS:
        if c in b["cols"] and r in b["rows"]:
            return b
    return None

def recolour(name, src_row, dst_row, rows):
    """Swap the colour word in `name` from src_row's colour to dst_row's.
    Returns None if the name carries no colour word (propagate verbatim)."""
    _, aliases = rows[src_row]
    dst_primary, _ = rows[dst_row]
    for a in sorted(aliases, key=len, reverse=True):     # longest alias first
        i = name.lower().find(a)
        if i >= 0:
            return name[:i] + dst_primary + name[i + len(a):]
    return None

def propagate(corrections):
    """corrections: list of {c,r,cat,name}. Returns dict "c,r" -> record."""
    out = {}
    for e in corrections:                                 # human edits win outright
        c, r = int(e["c"]), int(e["r"])
        cat = (e.get("cat") or "unknown").strip().lower()
        if cat not in VALID: cat = "unknown"
        out[f"{c},{r}"] = {"cat": cat, "name": (e.get("name") or "").strip(), "by": "user"}

    added = []
    for e in corrections:
        c, r = int(e["c"]), int(e["r"])
        b = block_for(c, r)
        if not b: continue
        src = out[f"{c},{r}"]
        for r2 in b["rows"]:
            if r2 == r: continue
            k = f"{c},{r2}"
            if k in out: continue                         # never overwrite a human edit
            nm = recolour(src["name"], r, r2, b["rows"])
            out[k] = {"cat": src["cat"], "name": nm if nm is not None else src["name"],
                      "by": f"propagated from {c},{r}"}
            added.append((k, out[k]["name"]))
    return out, added

def main():
    if "--report" in sys.argv:
        h = load(HUMAN, {})
        from collections import Counter
        print(f"labels_human.json: {len(h)} tiles")
        print(" ", dict(Counter("user" if v["by"] == "user" else "propagated" for v in h.values())))
        return

    if len(sys.argv) < 2: sys.exit(__doc__)
    raw = json.load(open(sys.argv[1]))
    tiles = raw.get("tiles", raw) if isinstance(raw, dict) else raw
    # an export marks every human-touched tile rev=true; untouched ones are just
    # the agent guess coming back round, so they are not corrections.
    corr = [t for t in tiles if t.get("rev")] if any("rev" in t for t in tiles) else tiles
    print(f"read {len(tiles)} tiles, {len(corr)} marked corrected")

    merged, added = propagate(corr)
    prev = load(HUMAN, {})
    for k, v in prev.items():                             # keep earlier passes
        if k not in merged: merged[k] = v
    json.dump(merged, open(HUMAN, "w"), indent=0, sort_keys=True)

    ai = load(AI, {})
    changed = [k for k, v in merged.items()
               if v["by"] == "user" and ai.get(k, {}).get("name") != v["name"]]
    twins = twin_map()
    dup = [k for k in merged if twins.get(tuple(int(x) for x in k.split(",")), 1) > 1]
    print(f"wrote {HUMAN}: {len(merged)} tiles "
          f"({sum(1 for v in merged.values() if v['by']=='user')} user, {len(added)} propagated)")
    print(f"  {len(changed)} user edits actually differ from the agent guess")
    print(f"  {len(dup)} of them show twice in the annotator (both cards now fixed)")
    for k, nm in added: print(f"  + {k:6s} {nm}")

if __name__ == "__main__":
    main()
