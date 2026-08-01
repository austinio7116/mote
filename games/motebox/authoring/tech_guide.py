#!/usr/bin/env python3
"""Draw the tech tree for the guide: the whole tree, and three kingdoms taking three roads.

The guide described the tree as four lists of eight, which is what it looked like before the
layout work and is not what it is. This emits two SVGs:

    motebox-tech-tree.svg     the whole tree, one rung per icon, era-coloured, labelled
    motebox-tech-roads.svg    the same tree three times over, with a different route lit in each

Both are drawn from src/tech_layout.h — the header the GAME reads — so the picture in the guide
is the layout on the handheld and cannot drift from it. The icons are the game's own icons, cut
from source_tileset.png at the cells in extract_box.TECH_ICON, embedded as data URIs and scaled
with nearest-neighbour so they stay pixels.

Crossings hop, exactly as they do on the handheld: where a vertical passes a shelf that is not
part of its route it breaks either side of it. That convention is the whole reason this tree
became readable, and a diagram that joined what the game breaks would teach the wrong thing.

    python3 authoring/tech_guide.py
"""
import base64
import io
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
OUT = os.path.join(ROOT, "docs", "img", "gallery")
SHEET = os.path.join(HERE, "source_tileset.png")

sys.path.insert(0, HERE)
import techtree as T                                            # noqa: E402
from extract_box import TECH_ICON                               # noqa: E402

# the game's own era colours (game.c tt_era_col), as hex
ERA_COL = {
    "ERA_STONE":       "#7a8496",
    "ERA_BRONZE":      "#968a5a",
    "ERA_IRON":        "#7e7e8c",
    "ERA_CLASSICAL":   "#629674",
    "ERA_MEDIEVAL":    "#747aa8",
    "ERA_RENAISSANCE": "#8e769e",
    "ERA_INDUSTRIAL":  "#927460",
    "ERA_MODERN":      "#608ea4",
    "ERA_ATOMIC":      "#a66c6c",
}
ERA_ORDER = list(ERA_COL)
BG, LINE, INK, DIM, GOLD = "#0d1018", "#3d4a70", "#e7e3d4", "#8f96ad", "#f0cf55"

# THE THREE ROADS. Each is a goal, and the route drawn is its ancestors — every rung that has to
# be learnt first, because both prerequisites of a rung are required. Three early goals were
# chosen on purpose: they need seven or eight rungs each out of thirty-five, so each picture is a
# small lit subtree in a dim tree, which is what a kingdom's first two centuries actually look
# like. A late goal would light almost everything and show nothing.
ROADS = [
    ("gunpowder",      "#e8894a", "THE WARMONGER", "bronze, iron, then muskets"),
    ("navigation",     "#5fb0e0", "THE SEAFARER",  "the wheel, the coast, then the open sea"),
    ("the university", "#7cc85e", "THE TALKER",    "writing, law, coin, then a college"),
]


# --------------------------------------------------------------- the data ----

def load():
    """The rungs, the graph, and where the handheld puts each one."""
    rows = T.parse()
    names = T.tech_to_name(rows)                 # TECH_* -> display name, matched by order
    ids = list(names)
    pre = {r["name"]: [names[p] for p in r["pre"]] for r in rows}
    era = {r["name"]: r["era"] for r in rows}
    cost = {r["name"]: r["cost"] for r in rows}

    h = open(os.path.join(GAME, "src", "tech_layout.h")).read()

    def arr(pat):
        return [int(x) for x in re.search(pat, h).group(1).split(",")]
    lay = arr(r"TT_LAYER\[\] = \{([^}]*)\}")
    col = arr(r"TT_COL\[\]   = \{([^}]*)\}")
    rown = arr(r"TT_ROWN\[TT_LAYERS\] = \{([^}]*)\}")
    layer, column, icon = {}, {}, {}
    for i, key in enumerate(ids):
        n = names[key]
        layer[n] = lay[i + 1]                    # entry 0 is TECH_NONE
        column[n] = col[i + 1]
        icon[n] = TECH_ICON[i]
    assert len(TECH_ICON) == len(ids), (len(TECH_ICON), len(ids))
    return dict(pre=pre, era=era, cost=cost, layer=layer, col=column, icon=icon, rown=rown,
                order=sorted(pre, key=lambda n: (layer[n], column[n])))


def ancestors(pre, goal):
    out = set()

    def walk(n):
        for p in pre[n]:
            if p not in out:
                out.add(p)
                walk(p)
    walk(goal)
    return out


def icon_uris(D):
    """Each rung's 8x8 cell as its own tiny PNG. Palette index 0 is black and means nothing
    here — a raw crop bakes an opaque black square, which is the mistake that put a black tile
    round the iron ingot in the game's own sheet."""
    im = Image.open(SHEET).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, _a = px[x, y]
            if (r, g, b) == (0, 0, 0):
                px[x, y] = (0, 0, 0, 0)
    uris = {}
    for n, (c, r) in D["icon"].items():
        cell = im.crop((c * 8, r * 8, c * 8 + 8, r * 8 + 8))
        # NEAREST to 32 px here rather than leaving it to the renderer: image-rendering:pixelated
        # is set as well, but a cell blown up eight times by a smoothing rasteriser is a smudge,
        # and the guide is also read as a printed page and as a rasterised preview.
        cell = cell.resize((32, 32), Image.NEAREST)
        buf = io.BytesIO()
        cell.save(buf, "PNG")
        uris[n] = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()
    return uris


# ------------------------------------------------------------- the drawing ----

class Tree:
    """One tree, at one scale. Holds the geometry so the links can be worked out before any of
    them is drawn — a line cannot hop over a shelf that has not been placed yet."""

    def __init__(self, D, x0, y0, size, px, py):
        self.D, self.x0, self.y0 = D, x0, y0
        self.s, self.px, self.py = size, px, py
        self.w = D["rown"] and max(D["rown"]) * px
        self.links = []
        for n in D["order"]:
            for i, p in enumerate(D["pre"][n]):
                a, b = self.cx(p), self.cx(n)
                self.links.append(dict(
                    pa=p, ch=n, x0=a, y0=self.top(p) + size, x1=b, y1=self.top(n),
                    ym=self.top(n) - max(6, size // 4) - i * max(4, size // 6)))

    def cx(self, n):
        lay, c = self.D["layer"][n], self.D["col"][n]
        span = self.D["rown"][lay] * self.px - (self.px - self.s)
        return self.x0 + self.w // 2 - span // 2 + c * self.px + self.s // 2

    def top(self, n):
        return self.y0 + self.D["layer"][n] * self.py

    def height(self):
        return (len(self.D["rown"]) - 1) * self.py + self.s

    def breaks(self, x, ya, yb):
        """The shelves this vertical crosses without meeting: level with an end is a corner, and
        touching an end in x is a junction."""
        out = []
        for l in self.links:
            hy, xa, xb = l["ym"], min(l["x0"], l["x1"]), max(l["x0"], l["x1"])
            if ya < hy < yb and xa < x < xb:
                out.append(hy)
        return sorted(set(out))

    def wire(self, l, colour, width, gap):
        """One link, as a path with the crossings taken out of it."""
        d = []
        for x, ya, yb in ((l["x0"], l["y0"], l["ym"]), (l["x1"], l["ym"], l["y1"])):
            lo, hi = min(ya, yb), max(ya, yb)
            cur = lo
            for b in self.breaks(x, lo, hi):
                if b - gap > cur:
                    d.append("M%d %dV%d" % (x, cur, b - gap))
                cur = b + gap
            if hi > cur:
                d.append("M%d %dV%d" % (x, cur, hi))
        d.append("M%d %dH%d" % (l["x0"], l["ym"], l["x1"]))
        return ('<path d="%s" stroke="%s" stroke-width="%d" fill="none" '
                'stroke-linecap="square"/>' % ("".join(d), colour, width))


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def para(text, x, y, w, size, colour, lead=None):
    """A caption that STAYS INSIDE THE PICTURE. SVG does not wrap text, so both captions ran off
    the right-hand edge of their own canvas; this breaks them on words at an estimated advance of
    just over half the point size, which is close enough for a sans-serif line."""
    lead = lead or int(size * 1.45)
    per = max(1, int(w / (size * 0.53)))
    out, line = [], ""
    for word in text.split():
        if len(line) + len(word) + 1 > per and line:
            out.append(line); line = word
        else:
            line = (line + " " + word).strip()
    if line:
        out.append(line)
    spans = "".join('<tspan x="%d" dy="%d">%s</tspan>' % (x, 0 if i == 0 else lead, esc(t))
                    for i, t in enumerate(out))
    return ('<text y="%d" fill="%s" font-size="%s">%s</text>' % (y, colour, size, spans),
            y + lead * (len(out) - 1))


def full_tree(D, uris):
    """THE WHOLE TREE, labelled. Eleven layers, widest five: the layout the handheld scrolls."""
    S, PX, PY, M = 34, 118, 92, 26
    W = M * 2 + max(D["rown"]) * PX
    # THE HEADER IS MEASURED, NOT ASSUMED. A fixed header height put the era key through the
    # middle of a caption that had wrapped onto three lines.
    head = ['<text x="%d" y="34" fill="%s" font-size="20" font-weight="700">The tech tree, '
            'as the handheld draws it</text>' % (M, INK)]
    cap, capb = para("35 rungs in 11 layers. A rung sits one layer below its deepest "
                     "prerequisite and needs BOTH of the rungs that feed it, so a link can also "
                     "reach up past a layer. Where one line crosses another it breaks: a break "
                     "is a crossing, a corner is a join.", M, 54, W - M * 2, 13, DIM)
    head.append(cap)
    ky = capb + 22
    x = M
    for e in ERA_ORDER:                       # the key, spaced by the width of its own words
        w = e[4:].lower()
        adv = 15 + int(len(w) * 6.1) + 14
        if x + adv > W - M:                   # nine era names are wider than five columns
            x, ky = M, ky + 18
        head.append('<rect x="%d" y="%d" width="11" height="11" fill="%s"/>'
                    % (x, ky - 9, ERA_COL[e]))
        head.append('<text x="%d" y="%d" fill="%s" font-size="11">%s</text>' % (x + 15, ky, DIM, w))
        x += adv

    t = Tree(D, M, ky + 30, S, PX, PY)
    H = ky + 30 + t.height() + 30
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
         'font-family="ui-sans-serif,-apple-system,Segoe UI,Roboto,sans-serif">' % (W, H, W, H),
         '<rect width="%d" height="%d" fill="%s"/>' % (W, H, BG)] + head
    for l in t.links:
        o.append(t.wire(l, LINE, 2, 4))
    for n in D["order"]:
        x, y = t.cx(n) - S // 2, t.top(n)
        o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#141a28" stroke="%s" '
                 'stroke-width="2"/>' % (x - 3, y - 3, S + 6, S + 6, ERA_COL[D["era"][n]]))
        o.append('<image x="%d" y="%d" width="%d" height="%d" href="%s" '
                 'style="image-rendering:pixelated"/>' % (x, y, S, S, uris[n]))
        o.append('<text x="%d" y="%d" fill="%s" font-size="12" text-anchor="middle">%s</text>'
                 % (t.cx(n), y + S + 15, INK, esc(n)))
    o.append("</svg>")
    return "\n".join(o), (W, H)


def roads(D, uris):
    """THE SAME TREE THREE TIMES, with a different route lit in each. This is the branching the
    guide claims and could not show: three lords in the same world, three subtrees."""
    S, PX, PY = 18, 60, 48
    M, GAP = 22, 30
    panel = max(D["rown"]) * PX
    W = M * 2 + panel * 3 + GAP * 2
    head = ['<text x="%d" y="32" fill="%s" font-size="19" font-weight="700">Three lords, three '
            'roads through the same tree</text>' % (M, INK)]
    cap, capb = para("What a kingdom learns is chosen by its lord and its land, so the same tree "
                     "is walked differently in every realm. Lit in each panel: the rungs that "
                     "road needs, and every rung it needs first.", M, 52, W - M * 2, 12.5, DIM)
    head.append(cap)
    TOPH = capb + 56                              # room for each panel's own two-line heading
    trees = [Tree(D, M + i * (panel + GAP), TOPH, S, PX, PY) for i in range(3)]
    H = TOPH + trees[0].height() + 40
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
         'font-family="ui-sans-serif,-apple-system,Segoe UI,Roboto,sans-serif">' % (W, H, W, H),
         '<rect width="%d" height="%d" fill="%s"/>' % (W, H, BG)] + head
    for t, (goal, colour, who, blurb) in zip(trees, ROADS):
        lit = ancestors(D["pre"], goal) | {goal}
        o.append('<text x="%d" y="%d" fill="%s" font-size="14" font-weight="700">%s</text>'
                 % (t.x0, TOPH - 30, colour, who))
        o.append('<text x="%d" y="%d" fill="%s" font-size="11.5">%s</text>'
                 % (t.x0, TOPH - 14, DIM, esc(blurb)))
        for l in t.links:                                   # the tree it is not taking
            if not (l["pa"] in lit and l["ch"] in lit):
                o.append(t.wire(l, "#28304c", 1.5, 3))
        for l in t.links:                                   # and the road it is
            if l["pa"] in lit and l["ch"] in lit:
                o.append(t.wire(l, colour, 2, 3))
        for n in D["order"]:
            x, y = t.cx(n) - S // 2, t.top(n)
            on = n in lit
            o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#141a28" stroke="%s" '
                     'stroke-width="%s" opacity="%s"/>'
                     % (x - 2, y - 2, S + 4, S + 4, colour if n == goal else
                        (ERA_COL[D["era"][n]] if on else "#232a40"), 2 if on else 1,
                        1 if on else 0.85))
            o.append('<image x="%d" y="%d" width="%d" height="%d" href="%s" opacity="%s" '
                     'style="image-rendering:pixelated"/>'
                     % (x, y, S, S, uris[n], 1 if on else 0.3))
        # name the goal under its own panel, since the icons are too small to label here
        o.append('<text x="%d" y="%d" fill="%s" font-size="12.5" font-weight="700">%s</text>'
                 % (t.x0, TOPH + t.height() + 24, colour,
                    "%s — %d rungs" % (goal, len(lit))))
    o.append("</svg>")
    return "\n".join(o), (W, H)


def main():
    D = load()
    uris = icon_uris(D)
    for name, fn in (("motebox-tech-tree.svg", full_tree),
                     ("motebox-tech-roads.svg", roads)):
        svg, (w, h) = fn(D, uris)
        p = os.path.join(OUT, name)
        open(p, "w").write(svg + "\n")
        print("[svg] %s  %dx%d  %.0f kB" % (p, w, h, len(svg) / 1024.0))


if __name__ == "__main__":
    main()
