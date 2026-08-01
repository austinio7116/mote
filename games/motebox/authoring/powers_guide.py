#!/usr/bin/env python3
"""Generate the guide's table of all 48 powers: cost, reach, what it does, what to expect.

The guide showed the six pages of the wheel as six screenshots and described five casts. Forty
of the powers were named nowhere, so the only way to find out what THE EYE or BONE KING cost or
did was to spend the Faith and see.

Everything mechanical here comes from the game: the name, the Faith and the brush radius are
parsed out of the Power tables in src/mb_power.c, and the icon is cut from the same master-sheet
cell the game blits, using the atlas rectangles in extract_box.py. The prose is written against
the cast code in mb_power.c, and the "what to expect" figures are what mb_power_test_all()
MEASURED — one cast each on a living town, printed by MOTEBOX_PWTEST=1.

    python3 authoring/powers_guide.py        # -> docs/motebox-guide.html, between markers
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
GUIDE = os.path.join(ROOT, "docs", "motebox-guide.html")
SHEET = os.path.join(HERE, "source_tileset.png")
BEGIN, END = "<!-- POWERTABLE:BEGIN -->", "<!-- POWERTABLE:END -->"

sys.path.insert(0, HERE)
from extract_box import SPRITE_SHEETS as RECTS                 # noqa: E402

# an icon's atlas -> where that atlas starts in the master sheet
ATLAS = {
    "ui_status_img":    "ui_status",
    "ui_gauges_img":    "ui_gauges",
    "ui_buttons_img":   "ui_buttons",
    "nature_img":       "nature",
    "treasure_ore_img": "treasure_ore",
    "bosses_img":       "bosses",
    "characters_img":   "characters",
    "animals_img":      "animals",
    "monsters_img":     "monsters",
    "crowns_fx_img":    "crowns_fx",
    "fx_mono_img":      "fx_mono",
    # town_img is OUR sheet, drawn in build_sprites.py, not a rectangle of the master — the
    # VILLAGE and ROAD icons point into it, so those two cells are cut from it instead.
}
OURS = {"town_img": os.path.join(GAME, "assets", "sheets", "town.png")}

TAB_NOTE = {
    "LAND":   "Shaping the ground. Held down, these paint — the rest are single casts.",
    "LIFE":   "Putting things in the world that were not there.",
    "BLESS":  "Helping what is already there.",
    "CURSE":  "Harming what is already there.",
    "WRATH":  "Disasters. They keep going after the cast: a fire spreads, a vent erupts again.",
    "BEASTS": "Summoning something that walks off and acts on its own.",
}

# WHAT EACH ONE DOES, against the cast code — and what one cast measurably did in the harness.
# A dash means the harness figure would be misleading: RAISE on flat ground and RAISE on a cliff
# are not the same cast.
PROSE = {
    "RAISE":     ("Lifts the ground a step. Enough of it makes hills, then mountains, and water "
                  "drains off it.", "about 25 cells a cast at reach 2"),
    "MOUNTAIN":  ("Puts rock and peaks down directly, whatever was there.",
                  "clears what stood on it — four objects in the measured cast"),
    "FOREST":    ("Grows woodland, which is where timber and game come from.",
                  "filled a reach-3 patch, and put three trees on it"),
    "GRASS":     ("Plain grass: the ordinary ground everything else is measured against.",
                  "cleared eleven objects out of the way"),
    "LOWER":     ("Sinks the ground a step. Low enough and it floods.", "about 25 cells a cast"),
    "WATER":     ("Water directly — a lake, a channel, or a moat.",
                  "drowned twelve objects in the measured cast"),
    "DESERT":    ("Sand. Nothing grows and nobody settles.", "a reach-3 patch"),
    "ROAD":      ("A road, which is how far a town can reach and how fast anyone moves.",
                  "one cell at a time; hold A to draw"),
    "HUMANS":    ("Four people, grown, standing where you cast.",
                  "4 people, at full health and content"),
    "ELVES":     ("Four elves. They live longer and prefer the woods.", "4 people"),
    "DWARVES":   ("Four dwarves. Tougher, and drawn to the hills.", "4 people, one born tough"),
    "ORCS":      ("Four orcs. They fight harder and quarrel sooner.", "4 people"),
    "VILLAGE":   ("A whole settlement: a hall, people in it, food to start on, and its own "
                  "kingdom if nobody claims the ground.",
                  "a village of 15 with 30 food, and a new kingdom"),
    "HERD":      ("Grazing animals — sheep, hens, deer. A town that owns them keeps them.",
                  "6 animals"),
    "WOLVES":    ("A pack. They hunt whatever is nearest, including people.", "3 wolves"),
    "PLANTS":    ("Bushes, flowers and crops on the ground you cast over.",
                  "19 plants over a reach-3 patch"),
    "RAIN":      ("Rain over a wide area: it waters crops and puts fires out.",
                  "watered a reach-4 patch; pixel rain, and it splashes"),
    "FERTILITY": ("Makes the ground fertile and the people in it likelier to have children.",
                  "the patch, and everyone standing in it"),
    "HEAL":      ("Closes wounds on everyone in reach.", "60 points of healing shared out"),
    "INSPIRE":   ("A whole technology, given to the kingdom that owns the ground.",
                  "+1 tech, and the chronicle records the inspiration"),
    "PEACE":     ("Ends every war in the world at once and empties the mustering yards.",
                  "6 wars ended, 39 towns stood their levies down"),
    "GOLD VEIN": ("Ore in the ground where you cast it — gold if the rock allows.",
                  "5 deposits"),
    "BLESS":     ("Everyone in reach becomes blessed: +2 Faith each, for good.",
                  "the whole patch, permanently"),
    "RESURRECT": ("Raises the dead out of the graves in reach. They come back blessed and "
                  "chosen.", "2 raised from 2 graves"),
    "PLAGUE":    ("Sickness on everyone in reach, and they pass it on.",
                  "the patch; it spreads from there"),
    "MADNESS":   ("They attack anyone, including their own.", "the patch"),
    "CURSE":     ("Cursed: -2 Faith each, and it counts as an illness.", "the patch"),
    "WEAKEN":    ("Takes 40 health off everyone in reach, and lifts what protected them.",
                  "760 points of damage; two died of it"),
    "FAMINE":    ("Strips the crops and stores out of the ground.",
                  "the patch, and two objects with it"),
    "BARREN":    ("No more children from anyone in reach.", "the patch"),
    "GRUDGE":    ("The kingdom that owns this ground declares war on a neighbour.",
                  "2 wars started"),
    "MARK":      ("Marked: every hunting thing comes for them, and their own land will not "
                  "protect them.", "the patch"),
    "FIRE":      ("Sets fire to it. Fire spreads by itself through anything dry.",
                  "3 objects burnt in the first moment, and it kept going"),
    "LIGHTNING": ("A bolt on one cell: it kills, it burns, and it leaves a scorch mark.",
                  "4 dead, 400 points of damage"),
    "METEOR":    ("A strike that flattens the ground and everything on it.",
                  "28 objects gone, and a crater"),
    "VOLCANO":   ("Raises a vent that goes on erupting after the cast.",
                  "28 objects gone, a mountain raised, and a vent that keeps venting"),
    "QUAKE":     ("Throws the ground up and down and shakes buildings apart.",
                  "12 objects down"),
    "TORNADO":   ("A twister that walks and takes what it passes.",
                  "7 objects, and it wanders off"),
    "THE BOMB":  ("A warhead: the blast, the fires, the fallout and the graves.",
                  "3 dead outright at the middle, and the ground poisoned"),
    "FREEZE":    ("Frost over the area. It stops fires and kills crops.",
                  "the patch; 4 objects killed off"),
    "BONE KING": ("A skeleton titan. It walks, it kills, and the dead it leaves get up.",
                  "17 dead in the measured cast"),
    "MEDUSA":    ("She looks at people and they stop being people.",
                  "35 dead — the deadliest thing on the wheel"),
    "REAPER":    ("It walks and harvests. Graves follow it.", "7 dead, and 5 new graves"),
    "DRAGON":    ("It flies, it burns what it passes, and it holds its breath over water.",
                  "2 dead and a wide burn"),
    "GOLEM":     ("Stone, slow and unstoppable: it breaks buildings before it breaks people.",
                  "19 dead, 8 buildings down"),
    "SWARM":     ("Twelve small hungry things at once.", "12 creatures"),
    "THE EYE":   ("It watches, and what it watches goes mad.",
                  "madness across everything near it"),
    "ANGEL":     ("It heals and blesses whatever it passes, and nothing touches it.",
                  "620 points of healing"),
}


def tables():
    """The six pages, parsed out of the C tables so the figures cannot drift."""
    src = open(os.path.join(GAME, "src", "mb_power.c")).read()
    out = []
    for tab, var in re.findall(r'\{ "([A-Z ]+)", (P_[A-Z]+) \}', src):
        blk = src[src.index("static const Power %s[8] = {" % var):]
        blk = blk[:blk.index("\n};")]
        rows = []
        for m in re.finditer(r'\{\s*PW_[A-Z_]+,\s*"([^"]+)",\s*&(\w+),\s*'
                             r'(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', blk):
            rows.append(dict(name=m.group(1), atlas=m.group(2),
                             ix=int(m.group(3)), iy=int(m.group(4)),
                             radius=int(m.group(5)), brush=int(m.group(6)),
                             cost=int(m.group(7))))
        assert len(rows) == 8, (var, len(rows))
        out.append((tab, rows))
    assert len(out) == 6, len(out)
    return out


def icons(rows):
    """The power icons, cut from the master sheet at the atlas offset the game uses. Black is
    palette index 0 and means transparent — a raw crop bakes an opaque black square."""
    im = Image.open(SHEET).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, _a = px[x, y]
            if (r, g, b) == (0, 0, 0):
                px[x, y] = (0, 0, 0, 0)
    uris = {}
    ours = {}
    for p in rows:
        key = ATLAS.get(p["atlas"])
        if key is None:
            path = OURS.get(p["atlas"])
            if not path or not os.path.exists(path):
                uris[p["name"]] = None
                continue
            if path not in ours:
                ours[path] = Image.open(path).convert("RGBA")
            # THE TOWN SHEET IS 8 WIDE AND 14 TALL PER CELL (a building stands six pixels above
            # its tile), and the wheel takes the top eight rows plus two: mb_power.c does the
            # same arithmetic, and cropping this one on an 8x8 grid would slice a roof in half.
            src = ours[path]
            box = (p["ix"] * 8, p["iy"] * 14 + 2, p["ix"] * 8 + 8, p["iy"] * 14 + 10)
            cell = src.crop(box).resize((32, 32), Image.NEAREST)
            buf = io.BytesIO()
            cell.save(buf, "PNG")
            uris[p["name"]] = ("data:image/png;base64,"
                               + base64.b64encode(buf.getvalue()).decode())
            continue
        elif key not in RECTS:
            uris[p["name"]] = None
            continue
        else:
            src = im
            c, r = RECTS[key][0] + p["ix"], RECTS[key][1] + p["iy"]
        # a kaiju is 2x2 on its sheet, and the wheel draws its top-left quarter; the guide can
        # show the whole animal, which is what the beast icons were fixed to do in the game too
        w = 16 if key == "bosses" else 8
        cell = src.crop((c * 8, r * 8, c * 8 + w, r * 8 + w)).resize((32, 32), Image.NEAREST)
        buf = io.BytesIO()
        cell.save(buf, "PNG")
        uris[p["name"]] = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()
    return uris


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def main():
    tabs = tables()
    allrows = [p for _t, rows in tabs for p in rows]
    uris = icons(allrows)
    missing = [p["name"] for p in allrows if p["name"] not in PROSE]
    assert not missing, "no prose for: %s" % missing

    html = ['<div class="tw"><table class="pwtab">',
            '<thead><tr><th>Power</th><th class="num">Faith</th><th class="num">Reach</th>'
            '<th>What it does</th><th>What one cast did</th></tr></thead><tbody>']
    for tab, rows in tabs:
        html.append('<tr class="pwtabhead"><td colspan="5"><b>%s</b> &mdash; %s</td></tr>'
                    % (tab, esc(TAB_NOTE[tab])))
        for p in rows:
            does, expect = PROSE[p["name"]]
            ic = ('<img class="picon" src="%s" alt="">' % uris[p["name"]]) if uris[p["name"]] \
                 else '<span class="picon"></span>'
            html.append('<tr><td class="pname">%s<b>%s</b>%s</td>'
                        '<td class="num">%d</td><td class="num">%s</td>'
                        '<td>%s</td><td class="pexp">%s</td></tr>'
                        % (ic, esc(p["name"]),
                           '<span class="brush" title="hold A to keep casting">paints</span>'
                           if p["brush"] else "",
                           p["cost"],
                           "one cell" if p["radius"] == 0 else str(p["radius"]),
                           esc(does), esc(expect)))
    html.append("</tbody></table></div>")

    g = open(GUIDE).read()
    assert BEGIN in g and END in g, "the guide has no POWERTABLE markers"
    a, b = g.index(BEGIN) + len(BEGIN), g.index(END)
    open(GUIDE, "w").write(g[:a] + "\n" + "\n".join(html) + "\n  " + g[b:])
    print("[html] %s  (%d powers, %d pages)" % (GUIDE, len(allrows), len(tabs)))


if __name__ == "__main__":
    main()
