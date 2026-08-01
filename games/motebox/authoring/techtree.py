"""Generate the tech-tree diagram for DESIGN.md from the game's OWN table.

The design doc said "compress to 5 tech tiers", which stopped being true a long time ago —
the tree is thirty-five techs across nine eras with real prerequisites. A diagram typed by
hand would be wrong within a week, so this parses `MB_TECH` out of mb_civ.c and emits a
mermaid graph between markers in DESIGN.md. Rerun it whenever the table changes; if the
document and the game disagree, the document is the one that is wrong.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.abspath(os.path.join(HERE, ".."))
SRC  = os.path.join(GAME, "src", "mb_civ.c")
DOC  = os.path.join(GAME, "DESIGN.md")

BEGIN = "<!-- BEGIN GENERATED TECH TREE (authoring/techtree.py) -->"
END   = "<!-- END GENERATED TECH TREE -->"

ERA_LABEL = {
    "ERA_STONE": "Stone", "ERA_BRONZE": "Bronze", "ERA_IRON": "Iron",
    "ERA_CLASSICAL": "Classical", "ERA_MEDIEVAL": "Medieval",
    "ERA_RENAISSANCE": "Renaissance", "ERA_INDUSTRIAL": "Industrial",
    "ERA_MODERN": "Modern", "ERA_ATOMIC": "Atomic",
}
ERA_ORDER = list(ERA_LABEL)


def parse():
    src = open(SRC).read()
    blk = src[src.index("const TechDef MB_TECH[TECH_N] = {"):]
    blk = blk[:blk.index("\n};")]
    rows = []
    for m in re.finditer(
            r'\{\s*"([^"]+)",\s*(ERA_[A-Z]+),\s*(\d+),\s*'
            r'(P0|P1\(\s*(TECH_[A-Z]+)\s*\)|P2\(\s*(TECH_[A-Z]+)\s*,\s*(TECH_[A-Z]+)\s*\))',
            blk):
        name, era, cost = m.group(1), m.group(2), int(m.group(3))
        pre = [g for g in (m.group(5), m.group(6), m.group(7)) if g and g != "TECH_NONE"]
        if name == "-":
            continue
        rows.append({"name": name, "era": era, "cost": cost, "pre": pre})
    return rows


def ident(name):
    """A mermaid node id: letters and digits only."""
    return "T" + re.sub(r"[^A-Za-z0-9]", "", name.title())


def tech_to_name(rows):
    """TECH_FOO -> the row's display name, matched BY ORDER rather than by spelling.

    This used to build the key from the display name — "ironwork" giving TECH_IRONWORK — with a
    handful of hand-written exceptions. Four names do not follow that rule (TECH_IRON,
    TECH_MATHS, TECH_WHEEL, TECH_ENGINEER) and were not among the exceptions, so every
    prerequisite edge out of ironwork, mathematics, the wheel and engineering was SILENTLY
    DROPPED from the diagram: gunpowder and cavalry, whose parents are exactly those, appeared
    to come from nowhere.

    The enum in mb.h and the table in mb_civ.c are in the same order by construction — there is
    a compile-time assert on their lengths — so zipping them is exact and cannot drift.
    """
    hdr = open(os.path.join(GAME, "src", "mb.h")).read()
    blk = hdr[hdr.index("TECH_NONE = 0,"):]
    blk = blk[:blk.index("TECH_N\n")] if "TECH_N\n" in blk else blk[:blk.index("TECH_N")]
    ids = re.findall(r"(TECH_[A-Z0-9_]+)", blk)
    ids = [i for i in ids if i != "TECH_NONE"]
    if len(ids) != len(rows):
        raise SystemExit("techtree: %d enum names but %d table rows — the two have drifted"
                         % (len(ids), len(rows)))
    return dict(zip(ids, [r["name"] for r in rows]))


# ---------------------------------------------------------------- the SVG ----
# THE GUIDE NEEDED THE SHAPE, not four columns of bullets. A mermaid block renders on
# GitHub and in an editor but not in a static page, so the guide gets an SVG built here
# from the same parse: nine era rows, the rungs of each ordered towards their parents so
# the edges come out short, and every edge drawn as an elbow. Same layout algorithm as the
# in-game screen (ui_draw_tech), for the same reason — a tech tree drawn in table order is
# a spider's web.

SVG_OUT = os.path.join(os.path.dirname(os.path.dirname(GAME)), "docs", "img", "gallery",
                       "motebox-techtree.svg")
ERA_COL = {                                     # a hue a row, cool early and warm late
    "ERA_STONE": "#6b7a8f", "ERA_BRONZE": "#7d7350", "ERA_IRON": "#6e6e78",
    "ERA_CLASSICAL": "#5d7f6a", "ERA_MEDIEVAL": "#6a6f92", "ERA_RENAISSANCE": "#7b6a86",
    "ERA_INDUSTRIAL": "#7a6656", "ERA_MODERN": "#5c7c8c", "ERA_ATOMIC": "#8a5f5f",
}


def layout(rows, names):
    """era row -> ordered list of rows, with each rung near the average column of its
    parents. Two passes, because moving one row changes what the next one wants."""
    by_era = {e: [r for r in rows if r["era"] == e] for e in ERA_ORDER}
    col = {}
    for e in ERA_ORDER:
        for i, r in enumerate(by_era[e]):
            col[r["name"]] = i
    for _ in range(2):
        for e in ERA_ORDER[1:]:
            def key(r):
                ps = [col.get(names.get(p, ""), 0) for p in r["pre"] if names.get(p) in col]
                return sum(ps) / len(ps) if ps else col[r["name"]]
            by_era[e].sort(key=key)
            for i, r in enumerate(by_era[e]):
                col[r["name"]] = i
    return by_era, col


def crossings(order, parent, rowof):
    """Crossings between the primary-parent edges landing in one row, plus nodes that a
    two-generation edge would pass over."""
    col = {n: i for e in ERA_ORDER for i, n in enumerate(order[e])}
    ec = nc = 0
    for e in ERA_ORDER:
        band = [(col[parent[c]], col[c]) for c in order[e]
                if parent.get(c) and rowof[c] - rowof[parent[c]] == 1]
        for a in range(len(band)):
            for b in range(a + 1, len(band)):
                (a0, a1), (b0, b1) = band[a], band[b]
                if (a0 - b0) * (a1 - b1) < 0:
                    ec += 1
    for c, p in parent.items():
        if p and rowof[c] - rowof[p] > 1:
            for r in range(rowof[p] + 1, rowof[c]):
                for n in order[ERA_ORDER[r]]:
                    if min(col[p], col[c]) <= col[n] <= max(col[p], col[c]):
                        nc += 1
    return ec, nc


def svg():
    """The high-level tree: ONE parent a rung.

    A rung's second prerequisite is flavour rather than structure — gunpowder wanting mathematics
    as well as ironwork — and drawing all fifty-seven links made a schematic that no amount of
    routing could rescue: twenty-five of thirty-five rungs had two parents, and the best possible
    layout still had fifteen crossings and twenty-nine lines passing over a rung. The second
    parents are still prerequisites in the GAME; they are simply not in this picture.

    That leaves thirty-two links. Twenty step down one generation and are drawn as elbows with a
    lane each. Five join two rungs of the SAME era — the atomic sequence and steam's two children
    — and those are drawn as horizontal arrows between neighbours, which is what a sequence looks
    like. Seven reach back two generations and run down a channel between two columns, so they
    pass between rungs rather than over them.
    """
    import itertools
    rows = parse()
    names = tech_to_name(rows)
    by_era, _c = layout(rows, names)
    rowof = {r["name"]: ERA_ORDER.index(r["era"]) for r in rows}
    era_of = {r["name"]: r["era"] for r in rows}
    parent = {r["name"]: (names[r["pre"][0]] if r["pre"] else None) for r in rows}
    order = {e: [r["name"] for r in by_era[e]] for e in ERA_ORDER}

    # SAME-ERA CHAINS ARE LAID OUT IN THEIR OWN ORDER, LEFT TO RIGHT.
    #
    # The atomic era is a sequence — electricity, then physics, then fission, then the bomb —
    # and the first version merely tried to put each child next to its parent, one pair at a
    # time, which the next pair then undid: the row came out as [the bomb, rocketry, physics,
    # fission], so the bomb's link to fission was a long horizontal running back across two
    # other rungs and fission appeared to have no parent at all.
    #
    # A chain has to be treated as a chain: follow it from its head and place the whole run
    # contiguously, in order, so every hop is one step to the right.
    for e in ERA_ORDER:
        here = set(order[e])
        kids = {}
        for c, p in parent.items():
            if p in here and c in here and rowof[c] == rowof[p]:
                kids.setdefault(p, []).append(c)
        child = {c for cs in kids.values() for c in cs}
        heads = [n for n in order[e] if n not in child]
        seq = []

        def walk(n):
            if n in seq:
                return
            seq.append(n)
            for c in kids.get(n, []):
                walk(c)
        for h in heads:
            walk(h)
        for n in order[e]:
            if n not in seq:
                seq.append(n)
        order[e] = seq

    def chains_ok(perm, e):
        """every same-era link one step to the right"""
        idx = {n: i for i, n in enumerate(perm)}
        for c, p in parent.items():
            if p in idx and c in idx and rowof[c] == rowof[p]:
                if abs(idx[c] - idx[p]) != 1:
                    return False
        return True

    best = crossings(order, parent, rowof)
    for _sweep in range(10):
        moved = False
        for e in ERA_ORDER:
            cur = order[e][:]
            for perm in itertools.permutations(cur):
                order[e] = list(perm)
                # keep same-era pairs adjacent, or the horizontals stop being hops
                okp = chains_ok(list(perm), e)
                sc = crossings(order, parent, rowof)
                if okp and sum(sc) < sum(best):
                    best, cur, moved = sc, list(perm), True
            order[e] = cur
        if not moved:
            break
    col = {n: i for e in ERA_ORDER for i, n in enumerate(order[e])}

    NW, NH, CW, GAP, LEFT = 132, 26, 160, 44, 44
    RH = NH + GAP
    W = LEFT + 4 * CW + 16
    H = len(ERA_ORDER) * RH + 26
    pos = {n: (LEFT + col[n] * CW + (CW - NW) / 2, 18 + rowof[n] * RH) for n in col}
    BG = "#0f1320"
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="100%%" '
           'font-family="-apple-system,Segoe UI,Roboto,sans-serif">' % (W, H),
           '<rect width="%d" height="%d" fill="%s"/>' % (W, H, BG)]

    def path(d, c):
        out.append('<path d="%s" fill="none" stroke="%s" stroke-width="5.5"/>' % (d, BG))
        out.append('<path d="%s" fill="none" stroke="%s" stroke-width="1.9"/>' % (d, c))

    def arrow(x, y, c, dirn="down"):
        if dirn == "down":
            out.append('<path d="M%.0f %.0f l-3.6 -5 l7.2 0 z" fill="%s"/>' % (x, y, c))
        else:
            out.append('<path d="M%.0f %.0f l-5 -3.6 l0 7.2 z" fill="%s"/>' % (x, y, c))

    for e in ERA_ORDER:
        y = 18 + ERA_ORDER.index(e) * RH
        out.append('<rect x="0" y="%.0f" width="%d" height="%d" fill="%s" opacity="0.10"/>'
                   % (y - 5, W, NH + 10, ERA_COL[e]))
        out.append('<text x="6" y="%.0f" fill="#8f96ad" font-size="10" font-weight="600">%s</text>'
                   % (y + NH / 2 + 4, ERA_LABEL[e][:3].upper()))

    # one generation down: an elbow with a lane of its own
    for e in ERA_ORDER:
        band = [c for c in order[e] if parent.get(c) and rowof[c] - rowof[parent[c]] == 1]
        band.sort(key=lambda c: -abs(col[parent[c]] - col[c]))
        n = max(1, len(band))
        for li, c in enumerate(band):
            p = parent[c]
            x0, y0 = pos[p]; x1, y1 = pos[c]
            lane = y1 - 8 - li * ((GAP - 14) / float(n))
            path("M%.0f %.0f V%.1f H%.0f V%.0f"
                 % (x0 + NW / 2, y0 + NH, lane, x1 + NW / 2, y1 - 5), ERA_COL[era_of[p]])
            arrow(x1 + NW / 2, y1, ERA_COL[era_of[p]])
    # same era: a horizontal hop to the neighbour on the right, which is what a sequence is
    for c, p in parent.items():
        if not p or rowof[c] != rowof[p]:
            continue
        x0, y0 = pos[p]; x1, _y = pos[c]
        cy = y0 + NH / 2
        if x1 > x0:
            path("M%.0f %.0f H%.0f" % (x0 + NW, cy, x1 - 7), ERA_COL[era_of[p]])
            arrow(x1, cy, ERA_COL[era_of[p]], "right")
        else:
            path("M%.0f %.0f H%.0f" % (x0, cy, x1 + NW + 7), ERA_COL[era_of[p]])
            out.append('<path d="M%.0f %.0f l5 -3.6 l0 7.2 z" fill="%s"/>'
                       % (x1 + NW, cy, ERA_COL[era_of[p]]))

    # two generations: down a channel BETWEEN two columns, so it passes between rungs
    longs = [(c, p) for c, p in parent.items() if p and rowof[c] - rowof[p] > 1]
    for li, (c, p) in enumerate(sorted(longs, key=lambda t: rowof[t[0]])):
        x0, y0 = pos[p]; x1, y1 = pos[c]
        lo, hi = min(col[p], col[c]), max(col[p], col[c])
        chan = LEFT + (lo + 1) * CW - 8 + (li % 3) * 5      # the gap right of the left-hand one
        if lo == hi:
            chan = LEFT + col[c] * CW + (CW - NW) / 2 - 10 - (li % 3) * 5
        path("M%.0f %.0f V%.0f H%.1f V%.0f H%.0f V%.0f"
             % (x0 + NW / 2, y0 + NH, y0 + NH + 10, chan, y1 - 14,
                x1 + NW / 2, y1 - 5), ERA_COL[era_of[p]])
        arrow(x1 + NW / 2, y1, ERA_COL[era_of[p]])

    for n in col:
        x, y = pos[n]
        out.append('<rect x="%.0f" y="%.0f" width="%d" height="%d" rx="4" fill="#1b2032" '
                   'stroke="%s" stroke-width="1.7"/>' % (x, y, NW, NH, ERA_COL[era_of[n]]))
        out.append('<text x="%.0f" y="%.0f" fill="#f2f4fa" font-size="11.5" '
                   'text-anchor="middle">%s</text>' % (x + NW / 2, y + NH / 2 + 4, n))
    out.append("</svg>")
    open(SVG_OUT, "w").write("\n".join(out))
    print("wrote %s  (%d links: crossings %d, pass-overs %d)"
          % (SVG_OUT, sum(1 for v in parent.values() if v), best[0], best[1]))


def build():
    rows = parse()
    names = tech_to_name(rows)
    out = [BEGIN, "", "```mermaid", "graph LR"]
    for era in ERA_ORDER:
        era_rows = [r for r in rows if r["era"] == era]
        if not era_rows:
            continue
        out.append('  subgraph %s' % ERA_LABEL[era])
        out.append("    direction TB")
        for r in era_rows:
            out.append('    %s["%s<br/>%d"]' % (ident(r["name"]), r["name"], r["cost"]))
        out.append("  end")
    for r in rows:
        for p in r["pre"]:
            pn = names.get(p)
            if pn:
                out.append("  %s --> %s" % (ident(pn), ident(r["name"])))
    out += ["```", ""]

    # and the same thing as a table, because a diagram is not searchable and a reader
    # chasing one tech's cost should not have to read a graph
    out += ["| era | techs (cost) |", "|---|---|"]
    for era in ERA_ORDER:
        era_rows = [r for r in rows if r["era"] == era]
        if not era_rows:
            continue
        out.append("| **%s** | %s |" % (
            ERA_LABEL[era],
            " · ".join("%s (%d)" % (r["name"], r["cost"]) for r in era_rows)))
    out += ["", "%d techs, %d eras. Generated from `MB_TECH` in `src/mb_civ.c` by "
            "`authoring/techtree.py` — do not edit by hand." % (
                len(rows), len({r["era"] for r in rows})), "", END]
    return "\n".join(out)


def main():
    block = build()
    doc = open(DOC).read()
    if BEGIN in doc and END in doc:
        a, b = doc.index(BEGIN), doc.index(END) + len(END)
        doc = doc[:a] + block + doc[b:]
    else:
        doc = doc.rstrip() + "\n\n## The tech tree\n\n" + block + "\n"
    open(DOC, "w").write(doc)
    print("[techtree] %s" % DOC)


# ------------------------------------------------------- the baked layout ----
# The in-game screen lays the tree out by DEPTH — a rung one row below its deepest
# prerequisite — which is the layout that made the diagram readable: every link a single step
# between adjacent layers, no era grid to fight. Working out the column order needs crossing
# minimisation, which is not something to do on a handheld at boot, so it is computed here and
# baked into a header the game reads.

LAYOUT_H = os.path.join(GAME, "src", "tech_layout.h")


def bake_layout():
    rows = parse()
    names = tech_to_name(rows)
    ids = list(tech_to_name(rows).keys())
    pre = {r["name"]: [names[p] for p in r["pre"]] for r in rows}
    depth = {}

    def d(n):
        if n not in depth:
            depth[n] = 0 if not pre[n] else 1 + max(d(p) for p in pre[n])
        return depth[n]
    for n in pre:
        d(n)
    layers = {}
    for n, v in depth.items():
        layers.setdefault(v, []).append(n)
    order = [sorted(layers[k]) for k in sorted(layers)]

    def cross(order):
        col = {n: i for lay in order for i, n in enumerate(lay)}
        c = 0
        for lay in order:
            band = [(col[p], col[n]) for n in lay for p in pre[n] if p in col]
            for a in range(len(band)):
                for b in range(a + 1, len(band)):
                    (a0, a1), (b0, b1) = band[a], band[b]
                    if (a0 - b0) * (a1 - b1) < 0:
                        c += 1
        return c
    for _ in range(40):                       # barycentre sweeps, then greedy swaps
        moved = False
        for li in range(1, len(order)):
            col = {n: i for lay in order for i, n in enumerate(lay)}

            def key(n):
                ps = [col[p] for p in pre[n] if p in col]
                return sum(ps) / len(ps) if ps else col[n]
            new = sorted(order[li], key=key)
            if new != order[li]:
                order[li] = new
                moved = True
        for li in range(len(order)):
            best = cross(order)
            for a in range(len(order[li])):
                for b in range(a + 1, len(order[li])):
                    order[li][a], order[li][b] = order[li][b], order[li][a]
                    if cross(order) < best:
                        best = cross(order)
                        moved = True
                    else:
                        order[li][a], order[li][b] = order[li][b], order[li][a]
        if not moved:
            break
    col = {n: i for lay in order for i, n in enumerate(lay)}
    name_of = {v: k for k, v in names.items()}          # display name -> TECH_*
    lay_by_id = ["0"] * (len(ids) + 1)
    col_by_id = ["0"] * (len(ids) + 1)
    for n in depth:
        idx = ids.index(name_of[n]) + 1                 # +1: TECH_NONE is 0
        lay_by_id[idx] = str(depth[n])
        col_by_id[idx] = str(col[n])
    wide = max(len(l) for l in order)
    out = ["/* GENERATED by authoring/techtree.py — do not edit.",
           " *",
           " * WHERE EACH RUNG SITS when the tree is drawn by dependency depth rather than by era:",
           " * one row below its deepest prerequisite, so every link is a single step between",
           " * adjacent layers. The column order is the one that minimises crossings, found by",
           " * barycentre sweeps and then exhaustive swaps — %d crossings over %d layers." % (cross(order), len(order)),
           " *",
           " * Indexed by TECH_*; entry 0 is TECH_NONE and unused. */",
           "#ifndef MOTEBOX_TECH_LAYOUT_H",
           "#define MOTEBOX_TECH_LAYOUT_H",
           "#define TT_LAYERS %d" % len(order),
           "#define TT_WIDE   %d" % wide,
           "static const uint8_t TT_LAYER[] = { %s };" % ", ".join(lay_by_id),
           "static const uint8_t TT_COL[]   = { %s };" % ", ".join(col_by_id),
           "static const uint8_t TT_ROWN[TT_LAYERS] = { %s };"
           % ", ".join(str(len(l)) for l in order),
           "#endif"]
    open(LAYOUT_H, "w").write("\n".join(out) + "\n")
    print("[layout]  %s  (%d layers, widest %d, %d crossings)"
          % (LAYOUT_H, len(order), wide, cross(order)))


if __name__ == "__main__":
    bake_layout()
    svg()
    main()
