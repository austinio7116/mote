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
    """TECH_FOO -> the row's display name. The enum name is the display name uppercased
    with spaces stripped, except where it is not, so match on both."""
    by = {}
    for r in rows:
        key = "TECH_" + re.sub(r"[^A-Za-z]", "", r["name"]).upper()
        by[key] = r["name"]
    # the handful whose enum and label differ
    by.setdefault("TECH_UNIVERSITY", "the university")
    by.setdefault("TECH_RAILWAY", "the railway")
    by.setdefault("TECH_NUKE", "the bomb")
    by.setdefault("TECH_ELECTRIC", "electricity")
    return by


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


if __name__ == "__main__":
    main()
