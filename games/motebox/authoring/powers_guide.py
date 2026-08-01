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

# WHAT EACH ONE DOES, and WHAT TO EXPECT of it. Both are written against the cast code in
# mb_power.c, and the figures in the second column are the ones mb_power_test_all() measures — but
# they are stated as what a player should expect from a cast, because this is a guide and not a
# test log.
PROSE = {
    "RAISE":     ("Lifts the ground a step. Enough of it makes hills, then mountains, and water "
                  "drains off it.", "Raises about a 5x5 patch by one step."),
    "MOUNTAIN":  ("Puts rock and peaks down directly, whatever was there.",
                  "Rock replaces what stood there \u2014 trees, crops and buildings with it."),
    "FOREST":    ("Grows woodland, which is where timber and game come from.",
                  "A 7x7 patch becomes woodland, with trees on it."),
    "GRASS":     ("Plain grass: the ordinary ground everything else is measured against.",
                  "Clears the patch back to grass; whatever grew on it is gone."),
    "LOWER":     ("Sinks the ground a step. Low enough and it floods.",
                  "Sinks about a 5x5 patch by one step. Keep going and it fills with water."),
    "WATER":     ("Water directly \u2014 a lake, a channel, or a moat.",
                  "The cells flood, and anything standing on them is lost."),
    "DESERT":    ("Sand. Nothing grows and nobody settles.",
                  "A 7x7 patch turns to sand and stays barren."),
    "ROAD":      ("A road, which is how far a town can reach and how fast anyone moves.",
                  "One cell of road. Hold A to lay a route."),
    "HUMANS":    ("Four people, grown, standing where you cast.",
                  "Four grown people, healthy and content. They will look for a town or found "
                  "one."),
    "ELVES":     ("Four elves. They live longer and prefer the woods.", "Four grown elves."),
    "DWARVES":   ("Four dwarves. Tougher, and drawn to the hills.",
                  "Four grown dwarves, usually one of them born tough."),
    "ORCS":      ("Four orcs. They fight harder and quarrel sooner.", "Four grown orcs."),
    "VILLAGE":   ("A whole settlement: a hall, people in it, food to start on, and its own "
                  "kingdom if nobody claims the ground.",
                  "A hamlet of about fifteen people with food to start on, and a new kingdom if "
                  "the ground is unclaimed."),
    "HERD":      ("Grazing animals \u2014 sheep, hens, deer. A town that owns them keeps them.",
                  "Six animals. A town within reach will take them into its flock."),
    "WOLVES":    ("A pack. They hunt whatever is nearest, including people.",
                  "Three wolves, hunting from the moment they land."),
    "PLANTS":    ("Bushes, flowers and crops on the ground you cast over.",
                  "Around twenty plants across the patch, and food for whatever grazes."),
    "RAIN":      ("Rain over a wide area: it waters crops and puts fires out.",
                  "Fires go out and crops are watered for a wide radius."),
    "FERTILITY": ("Makes the ground fertile and the people in it likelier to have children.",
                  "The ground and everyone on it. Expect children within a few years."),
    "HEAL":      ("Closes wounds on everyone in reach.",
                  "Everyone in reach is healed, about sixty points shared between them."),
    "INSPIRE":   ("A whole technology, given to the kingdom that owns the ground.",
                  "One free technology, and the chronicle records it."),
    "PEACE":     ("Ends every war in the world at once and empties the mustering yards.",
                  "Every war in the world ends and the levies go home. In a busy world that is "
                  "half a dozen wars at once."),
    "GOLD VEIN": ("Ore in the ground where you cast it \u2014 gold if the rock allows.",
                  "About five deposits for a nearby town to mine."),
    "BLESS":     ("Everyone in reach becomes blessed: +2 Faith each, for good.",
                  "Everyone in reach, permanently, and more Faith coming in every year."),
    "RESURRECT": ("Raises the dead out of the graves in reach. They come back blessed and "
                  "chosen.", "Every grave in reach opens; two of them is typical."),
    "PLAGUE":    ("Sickness on everyone in reach, and they pass it on.",
                  "Everyone in reach falls ill, and it spreads outward from them."),
    "MADNESS":   ("They attack anyone, including their own.",
                  "Everyone in reach turns on whoever is nearest, family included."),
    "CURSE":     ("Cursed: -2 Faith each, and it counts as an illness.",
                  "Everyone in reach: less Faith from them, and they sicken."),
    "WEAKEN":    ("Takes 40 health off everyone in reach, and lifts what protected them.",
                  "Forty health off everyone in reach. The frail die of it."),
    "FAMINE":    ("Strips the crops and stores out of the ground.",
                  "The crops across the patch are gone, and a town living on them will feel it "
                  "within a season."),
    "BARREN":    ("No more children from anyone in reach.",
                  "No more children from anyone in reach, for the rest of their lives."),
    "GRUDGE":    ("The kingdom that owns this ground declares war on a neighbour.",
                  "A war, usually within a year."),
    "MARK":      ("Marked: every hunting thing comes for them, and their own land will not "
                  "protect them.", "Everyone in reach. Beasts will come for them from a long "
                  "way off."),
    "FIRE":      ("Sets fire to it. Fire spreads by itself through anything dry.",
                  "A few cells catch at once, then it spreads on its own through anything dry."),
    "LIGHTNING": ("A bolt on one cell: it kills, it burns, and it leaves a scorch mark.",
                  "Kills whoever is standing there \u2014 three or four in a crowd \u2014 and "
                  "leaves the ground scorched."),
    "METEOR":    ("A strike that flattens the ground and everything on it.",
                  "Everything within three cells is destroyed, and the ground is left a crater."),
    "VOLCANO":   ("Raises a vent that goes on erupting after the cast.",
                  "A mountain where you cast it, and a vent that keeps erupting for years."),
    "QUAKE":     ("Throws the ground up and down and shakes buildings apart.",
                  "A dozen buildings come down, and the ground is left broken."),
    "TORNADO":   ("A twister that walks and takes what it passes.",
                  "It takes what it passes and then wanders off across the map."),
    "THE BOMB":  ("A warhead: the blast, the fires, the fallout and the graves.",
                  "The middle is emptied, fires spread from it, and the ground stays poisoned "
                  "for years."),
    "FREEZE":    ("Frost over the area. It stops fires and kills crops.",
                  "Fires go out; the crops die with them."),
    "BONE KING": ("A skeleton titan. It walks, it kills, and the dead it leaves get up.",
                  "Kills by the dozen, and the dead it leaves behind rise and follow it."),
    "MEDUSA":    ("She looks at people and they stop being people.",
                  "The deadliest thing on the wheel: dozens dead unless an army stops her."),
    "REAPER":    ("It walks and harvests. Graves follow it.",
                  "Kills steadily as it walks, and leaves a line of graves."),
    "DRAGON":    ("It flies, it burns what it passes, and it holds its breath over water.",
                  "A wide burnt path and whoever was standing in it."),
    "GOLEM":     ("Stone, slow and unstoppable: it breaks buildings before it breaks people.",
                  "Levels a town's buildings, and kills what comes out to defend them."),
    "SWARM":     ("Twelve small hungry things at once.",
                  "Twelve creatures, all hungry, all at once."),
    "THE EYE":   ("It watches, and what it watches goes mad.",
                  "Everything near it goes mad, and stays mad."),
    "ANGEL":     ("It heals and blesses whatever it passes, and nothing touches it.",
                  "Heals and blesses whoever it passes. Nothing can hurt it."),
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
            '<th>What it does</th><th>Impact</th></tr></thead><tbody>']
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
