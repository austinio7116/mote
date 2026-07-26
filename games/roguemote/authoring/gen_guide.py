#!/usr/bin/env python3
"""
Build docs/guide.html -- the illustrated player's manual.

Everything in the page comes from the shipped game, not from a second copy of
the design:

  * the stat tables are PARSED out of src/rl_data.c, rl_item.c and rl_magic.c,
    so a balance change in the game changes the manual on the next run;
  * the illustrations are the baked sprite sheets themselves, embedded once
    each and cropped with CSS background-position -- 155 monsters cost one
    2 KB PNG, not 155 images;
  * the screenshots are frames recorded from the host emulator;
  * the display typeface is the game's own 8x8 CP437 atlas, converted to a
    WOFF2 (see mkfont in this file), so the manual's headings and the text on
    the device are the same glyphs.

Usage:  python3 authoring/gen_guide.py [shots_dir]
"""
import base64
import io
import sys as _sys
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
_sys.path.insert(0, HERE)
GAME = os.path.dirname(HERE)
SRC = os.path.join(GAME, "src")
SHEETS = os.path.join(GAME, "assets", "sheets")
SHOTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "guide_shots")
OUT = os.path.join(GAME, "docs", "guide.html")
TS = 8


# ---------------------------------------------------------------- assets
def b64_png(path_or_img):
    if isinstance(path_or_img, str):
        img = Image.open(path_or_img)
    else:
        img = path_or_img
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


SHEET_FILES = [
    "animals", "monsters", "bosses", "characters", "weapons_potions",
    "tools_wands", "weapons_elemental", "treasure_ore", "crowns_fx", "food",
    "runes", "armour_set", "trinkets", "props_light",
    "doors_gems_banners", "chests", "boulders_mountains", "fx_mono",
    "dungeon_mono", "stairs", "jewellery",
]
# sheet id -> name, matching the SH_* enum in rl.h
SH = {i: n for i, n in enumerate(SHEET_FILES)}
SH_BY_NAME = {n: i for i, n in SH.items()}

sheets = {}
for n in SHEET_FILES:
    im = Image.open(os.path.join(SHEETS, n + ".png")).convert("RGBA")
    sheets[n] = dict(w=im.width, h=im.height, cols=im.width // TS, uri=b64_png(im))


def spr(sheet, cell, scale=4, cls="spr"):
    """One sprite cell as a div cropped out of the embedded sheet."""
    s = sheets[sheet]
    col, row = cell % s["cols"], cell // s["cols"]
    return (
        '<i class="{cls} sh-{sheet}" style="--s:{scale};'
        '--x:{x};--y:{y}"></i>'
    ).format(cls=cls, sheet=sheet, scale=scale, x=col, y=row)


def terrain_thumb(layers, w=5, h=4, scale=5):
    """A small patch of overworld rendered through the real blob47 rulesets,
    so the manual's terrain key is the terrain the game draws -- not a swatch
    of approximate colour."""
    import blob47
    raised = [[1 if (1 <= x <= 3 and 1 <= y <= 2) or (x == 4 and y == 2) else 0
               for x in range(w)] for y in range(h)]
    out = Image.new("RGBA", (w * TS, h * TS), (0, 0, 0, 0))
    for name, edge, mask_it in layers:
        sheet = Image.open(os.path.join(GAME, "tilesets", name + ".png")).convert("RGBA")
        tpr = sheet.width // TS
        for y in range(h):
            for x in range(w):
                on = raised[y][x] if mask_it else 1
                if not on:
                    continue
                m = 0
                for i, (dx, dy) in enumerate([(0, -1), (1, -1), (1, 0), (1, 1),
                                              (0, 1), (-1, 1), (-1, 0), (-1, -1)]):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        v = raised[ny][nx] if mask_it else 1
                    else:
                        v = edge
                    if v:
                        m |= 1 << i
                cell = blob47.LUT[m] if tpr > 1 else 0
                fx, fy = (cell % tpr) * TS, (cell // tpr) * TS
                out.alpha_composite(sheet.crop((fx, fy, fx + TS, fy + TS)), (x * TS, y * TS))
    big = out.resize((w * TS * scale, h * TS * scale), Image.NEAREST)
    return b64_png(big)


def spr2(sheet, cell, scale=4):
    """A 2x2 block (bosses), as one element twice the size."""
    s = sheets[sheet]
    col, row = cell % s["cols"], cell // s["cols"]
    return (
        '<i class="spr big sh-{sheet}" style="--s:{scale};--x:{x};--y:{y}"></i>'
    ).format(sheet=sheet, scale=scale, x=col, y=row)


# ---------------------------------------------------------------- parsing
def read(name):
    return open(os.path.join(SRC, name), encoding="utf-8").read()


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def table(text, marker):
    """The braces-and-commas body of `const X g_name[] = { ... };`"""
    i = text.index(marker)
    j = text.index("=", i)
    k = text.index("};", j)
    return strip_comments(text[j + 1:k])


data_c = read("rl_data.c")
item_c = read("rl_item.c")
magic_c = read("rl_magic.c")
rl_h = read("rl.h")

# --- item ids -----------------------------------------------------------
# Bosses and classes name their reward through enum ItemId rather than by
# position, so the guide has to resolve the name the same way the compiler
# does: the enum's order is the item table's order.
ITEM_ID = {}
_enum = rl_h[rl_h.index("enum ItemId {"):]
_enum = strip_comments(_enum[:_enum.index("};")])
for _i, _n in enumerate(re.findall(r"ITM_[A-Z0-9_]+", _enum)):
    ITEM_ID.setdefault(_n, _i)

# --- monsters -----------------------------------------------------------
MON_SHEET = {"A": "animals", "M": "monsters", "G": "crowns_fx"}
MON_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*([AMG])\((\d+)\)\s*,'
    + r"".join([r"\s*(\d+)\s*,"] * 8)
    + r"\s*([A-Z0-9_|\s]*?)\s*\}"
)
monsters = []
for m in MON_RE.finditer(table(data_c, "g_mon_kind[]")):
    name, sheet, cell, lvl, spd, hpd, hps, ac, damd, dams, xp, flags = m.groups()
    monsters.append(dict(
        name=name, sheet=MON_SHEET[sheet],
        cell=int(cell), lvl=int(lvl), speed=int(spd),
        hp=(int(hpd), int(hps)), ac=int(ac), dam=(int(damd), int(dams)),
        xp=int(xp), flags=[f.strip() for f in flags.split("|") if f.strip() and f.strip() != "0"],
    ))

# --- bosses -------------------------------------------------------------
BOSS_RE = re.compile(r'\{\s*"([^"]+)"\s*,' + r"".join([r"\s*(\d+)\s*,"] * 8)
                     + r"\s*(ITM_[A-Z0-9_]+)\s*\}")
bosses = []
for m in BOSS_RE.finditer(table(data_c, "g_boss_kind[]")):
    name, cell, depth, spd, hp, ac, damd, dams, xp, drop = m.groups()
    bosses.append(dict(name=name, cell=int(cell), depth=int(depth), speed=int(spd),
                       hp=int(hp), ac=int(ac), dam=(int(damd), int(dams)),
                       xp=int(xp), drop=ITEM_ID[drop]))

# --- classes ------------------------------------------------------------
CLS_RE = re.compile(r'\{\s*"([^"]+)"\s*,' + r"".join([r"\s*(\d+)\s*,"] * 9)
                    + r"\s*(0x[0-9A-Fa-f]+)\s*,\s*(ITM_[A-Z0-9_]+)\s*\}")
classes = []
for m in CLS_RE.finditer(table(data_c, "g_class[]")):
    g = m.groups()
    classes.append(dict(name=g[0], cell=int(g[1]),
                        stats=[int(x) for x in g[2:8]],
                        hp=int(g[8]), sp=int(g[9]),
                        spells=int(g[10], 16), weapon=ITEM_ID[g[11]]))

# --- items --------------------------------------------------------------
ITEM_SHEET = {"W": "weapons_potions", "T": "tools_wands", "E": "weapons_elemental",
              "O": "treasure_ore", "G": "crowns_fx", "F": "food", "R": "runes",
              "L": "armour_set", "K": "trinkets", "J": "jewellery"}
ITEM_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*([WTEOGFRLKJ])\((\d+)\)\s*,\s*(TV_\w+)\s*,'
    r"\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(EF_\w+)\s*\}"
)
items = []
for m in ITEM_RE.finditer(table(item_c, "g_item_kind[]")):
    name, sh, cell, tv, lvl, cost, d, s_, ac, eff = m.groups()
    items.append(dict(name=name, sheet=ITEM_SHEET[sh], cell=int(cell), tv=tv[3:],
                      lvl=int(lvl), cost=int(cost), dice=(int(d), int(s_)),
                      ac=int(ac), eff=eff[3:]))

EGO_RE = re.compile(r'\{\s*"([^"]*)"\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*([A-Z0-9_|\s]+?)\s*\}')
egos = [dict(name=m.group(1), mult=int(m.group(2)), bonus=int(m.group(3)))
        for m in EGO_RE.finditer(table(item_c, "g_ego_kind[]")) if m.group(1)]

# --- spells -------------------------------------------------------------
SPELL_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,'
                      r"\s*(SP_\w+)\s*,\s*(\d+)\s*\}")
spells = [dict(name=m.group(1), lvl=int(m.group(2)), cost=int(m.group(3)),
               fail=int(m.group(4)), shape=m.group(5)[3:], power=int(m.group(6)))
          for m in SPELL_RE.finditer(table(magic_c, "g_spell[]"))]

# --- shops --------------------------------------------------------------
world_c = read("rl_world.c")
shop_names = re.findall(r'"([^"]+)"', world_c[world_c.index("g_shop_name[SHOP_N]"):]
                        [:200])
shop_sheets = re.findall(r"SH_\w+", world_c[world_c.index("g_shop_sheet[SHOP_N]"):][:200])
shop_cells = [int(x) for x in re.findall(
    r"\d+", world_c[world_c.index("g_shop_cell[SHOP_N]"):].split("}")[0].split("{")[1])]
SH_ENUM = {"SH_DOORS": "doors_gems_banners", "SH_PROPS": "props_light"}
shops = [dict(name=shop_names[i], sheet=SH_ENUM[shop_sheets[i]], cell=shop_cells[i])
         for i in range(len(shop_names))]

assert monsters and bosses and classes and items and spells and shops, "table parse failed"
print("[guide] parsed %d monsters, %d bosses, %d classes, %d items, %d spells"
      % (len(monsters), len(bosses), len(classes), len(items), len(spells)))


# ---------------------------------------------------------------- shots
def shot(name):
    p = os.path.join(SHOTS, name + ".png")
    return b64_png(p) if os.path.isfile(p) else None


SHOT_NAMES = ["title", "class", "overworld", "worldmap", "town", "shop", "pack",
              "gear", "character", "dungeon", "levelmap", "spells", "missile",
              "nova", "fireball", "boss"]
shots = {n: shot(n) for n in SHOT_NAMES}
missing = [n for n, v in shots.items() if not v]
if missing:
    print("[guide] WARNING missing screenshots: %s" % ", ".join(missing))

font_path = os.path.join(SHOTS, "rogue8.woff2")
font_b64 = ""
if os.path.isfile(font_path):
    font_b64 = base64.b64encode(open(font_path, "rb").read()).decode()
else:
    print("[guide] WARNING no rogue8.woff2 -- display face will fall back")


# ---------------------------------------------------------------- helpers
FLAG_LABEL = {
    "GRP": "packs", "ERR": "erratic", "STIL": "rooted", "DOOR": "opens doors",
    "EVIL": "evil", "UNDD": "undead",
}
TV_LABEL = {
    "WEAPON": "Weapons", "ARMOUR": "Armour", "POTION": "Potions",
    "SCROLL": "Scrolls", "WAND": "Wands", "RING": "Rings & amulets",
    "FOOD": "Food", "LIGHT": "Light",
}
TV_NOTE = {
    "WEAPON": "Damage is the listed dice plus your enchantment plus a quarter "
              "of your Strength. Dexterity buys extra blows at 12 and 24.",
    "ARMOUR": "Armour class subtracts from a monster's chance to land a blow, "
              "and soaks a sixth of every blow that does land.",
    "POTION": "Unidentified until you drink one. The flavour attached to each "
              "kind is shuffled per character, so knowledge does not carry over.",
    "SCROLL": "Read once, consumed. Two of the nine are traps in disguise.",
    "WAND": "A spell you do not need the level for, and the only offensive "
            "magic a Warrior will ever hold.",
    "RING": "Worn in a single slot. A cursed one will not come off until a "
            "scroll of remove curse says otherwise.",
    "FOOD": "The hunger clock is the reason you cannot simply rest off every "
            "wound. Regeneration burns it faster.",
    "LIGHT": "Light radius decides how much of a room you see before it sees "
             "you. A lantern is worth more than most weapons.",
}
EFF_LABEL = {
    "CURE_LIGHT": "restores 15 hp", "CURE_SERIOUS": "restores 40 hp",
    "FULL_HEAL": "restores all hp", "MANA": "restores 25 mana",
    "SPEED": "+10 speed for 60 turns", "HEROISM": "+10 hp, +1 max hp",
    "POISON": "8 damage", "XP": "a quarter of your experience again",
    "MAP": "reveals the level", "TELEPORT": "moves you at random",
    "IDENTIFY": "identifies everything", "ENCHANT_W": "+1 to your weapon",
    "ENCHANT_A": "+1 to your armour", "DEEP_DESCENT": "drops you two floors",
    "LIGHT": "lights the area", "REMOVE_CURSE": "lifts every curse you carry",
    "SUMMON": "summons four monsters", "W_MISSILE": "3d6 to one target",
    "W_FIRE": "4d8 to one target", "W_FROST": "4d9 over a radius",
    "W_DRAIN": "5d9, and you heal a third of it",
    "R_PROT": "+8 armour class", "R_STR": "+2 Strength", "R_INT": "+2 Intelligence",
    "R_SPEED": "+10 speed, permanently", "R_REGEN": "heal three times as fast",
    "NONE": "",
}
SHAPE_NOTE = {
    "BOLT": "a single target, struck by a travelling head",
    "BALL": "a blast two tiles across, centred on your target",
    "BEAM": "everything on the line, not just the first thing",
    "HEAL": "restores your own wounds",
    "BUFF": "changes you, not them",
    "DETECT": "reveals every living thing on the floor",
    "NOVA": "a ring four tiles across, centred on you",
}
STAT_NAMES = ["Str", "Int", "Wis", "Dex", "Con", "Cha"]


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def dice(d):
    return "%dd%d" % d


# ---------------------------------------------------------------- sections
def sec_screen():
    rows = [
        ("Health", "The red bar and the number beside it. It regenerates one "
                   "point every sixteen turns, and faster with a ring of regeneration."),
        ("Mana", "The blue bar. Every cast spends it whether or not the spell "
                 "succeeds."),
        ("Level", "Your character level, after <code>L</code>. Experience "
                  "comes from kills, and each level raises health, mana and accuracy."),
        ("Depth", "<code>TOWN</code> on the surface, <code>DL</code> and a "
                  "number below it. Depth is difficulty: everything scales with it."),
        ("State", "A <code>*</code> when you are close to starving, a "
                  "<code>&gt;</code> while you are hastened."),
    ]
    body = "".join(
        '<div class="kv"><dt>%s</dt><dd>%s</dd></div>' % (k, v) for k, v in rows)
    return """
<section id="screen">
  <h2><span class="num">01</span>The screen</h2>
  <p>The Thumby Color gives you 128&#215;128 pixels. Roguemote spends 128&#215;104
     of them on the map &mdash; sixteen tiles across, thirteen down &mdash; and the
     remaining 24 rows on a status strip and two lines of log. Nothing else is
     ever on screen during play, which is why the log is worth reading: it is
     the only place the game tells you what just happened.</p>
  %s
  <dl class="kvlist">%s</dl>
</section>""" % (plate(shots["dungeon"], "The map viewport, status strip and log. "
                       "Walls, floor and every actor are 8&#215;8 tiles."), body)


def sec_controls():
    keys = [
        ("D-pad", "Move one tile. Walking into something attacks it. Hold a "
                  "direction to keep walking."),
        ("LB + D-pad", "Move diagonally. The Mines are eight-way; corridors are not."),
        ("A", "Whatever the tile under you offers &mdash; take the stairs, pick "
              "up what is lying there, enter the town, enter a cave mouth."),
        ("B", "Wait one turn. Useful for letting something come to you in a corridor."),
        ("RB", "Open the spell list. Casting targets the nearest thing you can see."),
        ("MENU", "Open and close the book: <b>Pack</b>, <b>Gear</b>, <b>Spell</b>, "
                 "<b>Char</b>, <b>Map</b>. Press it again from any page to get back "
                 "to play."),
        ("LB / RB", "Turn to the previous or next page. The tab strip at the top "
                    "shows where you are."),
        ("&larr;", "The second action on a page: drop from the Pack, take a slot "
                   "off in Gear."),
    ]
    body = "".join(
        '<div class="key"><kbd>%s</kbd><p>%s</p></div>' % (k, v) for k, v in keys)
    return """
<section id="controls">
  <h2><span class="num">02</span>Controls</h2>
  <p>Nine buttons, and a roguelike traditionally wants forty. The compromise is
     that <kbd>A</kbd> is context-sensitive: it does the one sensible thing
     available where you are standing, and tells you in the log when there is
     nothing.</p>
  <div class="keys">%s</div>
</section>""" % body


def sec_classes():
    cards = []
    for i, c in enumerate(classes):
        known = [s for b, s in enumerate(spells) if c["spells"] & (1 << b)]
        stats = "".join(
            '<div><span>%s</span><b>%d</b></div>' % (STAT_NAMES[j], c["stats"][j])
            for j in range(6))
        wp = items[c["weapon"]]["name"]
        if known:
            magic = ('<p class="spells"><span>Casts</span> %s</p>'
                     % ", ".join(esc(s["name"]) for s in known))
        else:
            magic = '<p class="spells nomagic"><span>Casts</span> nothing at all</p>'
        cards.append("""
    <article class="cls">
      <header>%s<h3>%s</h3></header>
      <div class="stats">%s</div>
      <p class="starts">Starts with a %s.</p>
      %s
    </article>""" % (spr("characters", c["cell"], 6), esc(c["name"]), stats,
                     esc(wp), magic))
    return """
<section id="classes">
  <h2><span class="num">03</span>The twelve</h2>
  <p>You pick one of twelve at the start and live with it. The choice sets your
     six statistics, your starting weapon, and &mdash; the part that matters most
     &mdash; which of the sixteen spells you may ever learn. A Warrior never casts
     anything; a Mage gets twelve of them but starts with nine Strength and a dagger.</p>
  %s
  <div class="grid cls-grid">%s</div>
</section>""" % (plate(shots["class"], "Choosing a class. Statistics for the "
                       "highlighted class sit under the grid."), "".join(cards))


def sec_overworld():
    plain = terrain_thumb([("floor_grass", 1, False)])
    hills = terrain_thumb([("floor_grass", 1, False), ("floor_jungle", 0, True)])
    mount = terrain_thumb([("floor_grass", 1, False), ("floor_jungle", 0, True),
                           ("wall_aztec", 1, True)])
    terrain = [
        ("Plain", ("img", plain), "Open ground. Most of the continent."),
        ("Forest", ("spr", ("trinkets", 70)), "Walkable, and it blocks nothing "
         "&mdash; but you cannot see through a canopy either."),
        ("Hills", ("img", hills), "Walkable highland, drawn with a terraced "
         "escarpment along its lower edge."),
        ("Mountain", ("img", mount), "Impassable. The rim of rock around the "
         "continent is what makes it an island."),
        ("Town", ("spr", ("props_light", 31)), "One per world: a walled compound "
         "with gates, streets, six shops and an inn."),
        ("Cave mouth", ("spr", ("stairs", 2)), "The way into the Mines. Six of "
         "them, one always within sight of the town."),
        ("Ruins", ("spr", ("trinkets", 69)), "A broken ring of stone with a chest "
         "inside. Four to eight a continent."),
    ]
    rows = []
    for name, art, note in terrain:
        if art[0] == "img":
            icon = '<img class="tthumb" src="%s" alt="" width="200" height="160">' % art[1]
            cls = "terr wide"
        else:
            icon = spr(art[1][0], art[1][1], 5)
            cls = "terr"
        rows.append('<div class="%s">%s<div><h4>%s</h4><p>%s</p></div></div>'
                    % (cls, icon, name, note))
    return """
<section id="overworld">
  <h2><span class="num">04</span>The continent</h2>
  <p>You start on the surface, on the town, with a hundred and twenty gold and
     four rations. The continent is generated fresh for each character and does
     not change afterwards &mdash; the same seed rebuilds it every time you
     surface, so the cave you left is where you left it.</p>
  <p>There is no sea. The tileset this game is built from has no water anywhere
     in it, so rather than ship invented art the island is ringed by impassable
     rock instead.</p>
  <div class="terr-grid">%s</div>
  %s
  %s
  <p><kbd>MENU</kbd> then <kbd>LB</kbd> for the map. On a 64&#215;48 continent seen
     through a sixteen-tile window it is not a luxury &mdash; the yellow mark is
     the town, the red marks are cave mouths.</p>
</section>""" % ("".join(rows),
                 plate(shots["overworld"], "Forest, plain and a terraced hill. The "
                       "gold flight of steps is a cave mouth."),
                 plate(shots["worldmap"], "The continent. Brown is impassable rock, "
                       "dark green is forest, pale green is highland."))


def sec_mines():
    bands = [
        (1, 8, "wall_brick", "Stone brick", "Rats, kobolds and jackals. Survivable."),
        (9, 16, "wall_bone", "Bone", "Serpents, wargs and the first demons."),
        (17, 24, "wall_marble", "Marble", "Wights, hags and greater demons."),
        (25, 32, "wall_aztec", "Temple", "Liches. Nothing here is a fair fight."),
        (33, 99, "wall_plaster", "Plaster", "Morgoth is on thirty-five."),
    ]
    rows = "".join(
        '<tr><td class="d">%s</td><td>%s</td><td>%s</td></tr>'
        % ("%d&ndash;%d" % (a, b) if b < 99 else "%d and below" % a, n, note)
        for a, b, _, n, note in bands)
    return """
<section id="mines">
  <h2><span class="num">05</span>The Mines</h2>
  <p>Each floor is rooms and corridors with a staircase at each end. Floors are
     <em>not</em> persistent: climbing back up generates a new level at that
     depth. That is deliberate, and it is what makes going down feel one-way.</p>
  <p>Rather more than half the rooms <strong>bud straight off a neighbour</strong>
     and are joined by a door through the shared wall, so a floor comes out as
     clusters of connected chambers with corridors running between the clusters
     &mdash; not one room per corridor. The corridors that remain are not pipes
     either: they bulge and narrow as they run.</p>
  %s
  <p>Not every room is a lit rectangle. Some have a corner bitten out, some are
     halls full of pillars you can lose a monster behind, and some are
     <strong>vaults</strong> &mdash; a sealed inner chamber behind one closed
     door, holding chests and something awake that is standing over them. About
     a third of floors come out as warrens of small cells rather than halls.
     Deeper floors have more unlit rooms, which have to be walked by torchlight;
     the room you arrive in is never one of them.</p>
  <p>A level is a graph, not a line. Beyond the corridor that chains the rooms
     together there are one to three extra links, so there is more than one way
     round and somewhere to run to. <strong>Rubble</strong> blocks corridors and
     blocks sight; walking into it clears it and costs you the turn.</p>
  <p><strong>Chests</strong> come in five colours, poorest to richest: red,
     blue, gold, green, white. The richer the chest the likelier it is trapped
     &mdash; eight percent on a red, thirty-two on a white. Deep floors roll
     high tiers more often, but a white chest on floor two is possible, and it
     is possible on purpose.</p>
  <p>Roughly one chest in six is a <strong>mimic</strong>. There is no way to
     tell by looking: it draws itself as a real chest, in the colour that tile
     would have had. It springs when you are within reach.</p>
  <p>The walls change as you descend, so the floor you are on is legible at a
     glance without reading the number.</p>
  <div class="tablewrap"><table class="bands">
    <thead><tr><th>Depth</th><th>Walls</th><th>What lives there</th></tr></thead>
    <tbody>%s</tbody>
  </table></div>
  %s
</section>""" % (plate(shots["dungeon"], "Depth five. Lit rooms reveal whole at "
                       "once; corridors reveal as you walk them."),
                 rows,
                 plate(shots["levelmap"], "The level map shows only what you have "
                       "seen. Red is the way down, blue the way up."))


def sec_turns():
    return """
<section id="turns">
  <h2><span class="num">06</span>Turns, speed and combat</h2>
  <p>Nothing moves until you do. Every actor accumulates energy each tick from
     its speed and acts when it reaches a hundred, which is Moria's model and
     has one consequence worth internalising: <strong>normal speed is 110, and
     +10 speed is exactly double actions</strong>. A potion of speed does not make
     you 9% faster. It gives you two turns for each one your enemies get, and
     that is usually the difference between a fight and an escape.</p>
  <div class="kvlist">
    <div class="kv"><dt>To hit</dt><dd>30, plus three per character level, plus
      your Dexterity, plus three per point of weapon enchantment, less twice the
      target's armour class.</dd></div>
    <div class="kv"><dt>Damage</dt><dd>The weapon's dice, plus its enchantment,
      plus a quarter of your Strength &mdash; then multiplied if the weapon has an
      ego that applies to what you hit.</dd></div>
    <div class="kv"><dt>Blows</dt><dd>One, plus one per twelve Dexterity, plus
      one more for a weapon <em>of Attacks</em>. Four is the ceiling.</dd></div>
    <div class="kv"><dt>Being hit</dt><dd>Your armour class lowers their chance
      to connect, and soaks a sixth of anything that does.</dd></div>
  </div>
  <p>Monsters hunt on their own senses, not on yours. Something can be closing
     on you from down a corridor you have never seen, and if it is within
     fourteen tiles it already knows where you are.</p>
</section>"""


def sec_items():
    order = ["WEAPON", "ARMOUR", "POTION", "SCROLL", "WAND", "RING", "FOOD", "LIGHT"]
    out = []
    for tv in order:
        group = [it for it in items if it["tv"] == tv]
        if not group:
            continue
        rows = []
        for it in group:
            if tv == "WEAPON":
                right = dice(it["dice"])
            elif tv == "ARMOUR":
                right = "+%d ac" % it["ac"]
            elif tv == "LIGHT":
                right = "radius %d" % it["ac"]
            else:
                right = esc(EFF_LABEL.get(it["eff"], ""))
            rows.append(
                '<tr><td class="ic">%s</td><td class="nm">%s</td>'
                '<td class="ef">%s</td><td class="n">%d</td><td class="n">%d</td></tr>'
                % (spr(it["sheet"], it["cell"], 3), esc(it["name"]), right,
                   it["lvl"], it["cost"]))
        out.append("""
  <article class="itemgroup">
    <h3>%s</h3>
    <p class="note">%s</p>
    <div class="tablewrap"><table class="items">
      <thead><tr><th></th><th>Name</th><th>Effect</th><th class="n">Depth</th><th class="n">Gold</th></tr></thead>
      <tbody>%s</tbody>
    </table></div>
  </article>""" % (TV_LABEL[tv], TV_NOTE[tv], "".join(rows)))

    ego_rows = "".join(
        '<tr><td class="nm">%s</td><td class="n">%s</td><td class="n">%s</td></tr>'
        % (esc(e["name"].strip()),
           ("&#215;%.2f" % (e["mult"] / 3.0)) if e["mult"] > 0 else "&#215;0.5",
           "%+d" % e["bonus"])
        for e in egos)

    return """
<section id="items">
  <h2><span class="num">07</span>What you find</h2>
  <p>An item rolls a base kind weighted by depth, then an enchantment, then a
     chance at an ego &mdash; the same pipeline Moria used. Depth below is the
     shallowest floor a kind appears on; gold is its base price before
     enchantment and before the shopkeeper takes a view of your Charisma.</p>
  <div class="shots2">%s%s</div>
  <p>Four things are worn rather than carried: a weapon, body armour, one ring
     or amulet, and a light. The gear screen shows what each contributes and
     what the four add up to, so you can judge a swap without doing the
     arithmetic yourself. <kbd>&larr;</kbd> there takes a slot off &mdash; the only
     way back to bare hands, and the only way to shed a light before selling it.
     A cursed item will not come off at all.</p>
  %s
  <h3>Egos</h3>
  <p>Roughly one weapon or piece of armour in fifteen carries one, and the odds
     climb with depth. The multiplier applies against whatever the ego is for
     &mdash; <em>Holy</em> only against evil, the elemental brands against
     everything. Two of the ten are curses, and a cursed item will not come off.</p>
  <div class="tablewrap"><table class="items ego">
    <thead><tr><th>Ego</th><th class="n">Damage</th><th class="n">Enchant</th></tr></thead>
    <tbody>%s</tbody>
  </table></div>
</section>""" % (plate(shots["pack"], "The pack holds sixteen kinds.", small=True),
                 plate(shots["gear"], "Wield / wear. <kbd>RB</kbd> from the "
                       "pack.", small=True),
                 "".join(out), ego_rows)


def sec_magic():
    rows = "".join(
        '<tr><td class="nm">%s</td><td class="n">%d</td><td class="n">%d</td>'
        '<td class="n">%d%%</td><td>%s</td></tr>'
        % (esc(s["name"]), s["lvl"], s["cost"], s["fail"],
           SHAPE_NOTE.get(s["shape"], s["shape"].lower()))
        for s in spells)
    strips = [
        ("Beam", "fx_mono", range(0, 6)),
        ("Impact", "fx_mono", range(6, 12)),
        ("Spark", "fx_mono", range(38, 44)),
        ("Shockwave", "fx_mono", range(54, 60)),
        ("Star", "fx_mono", range(92, 96)),
        ("Fade", "fx_mono", range(106, 112)),
    ]
    strip_html = "".join(
        '<div class="strip"><span>%s</span><div>%s</div></div>'
        % (n, "".join(spr(sh, c, 4) for c in cells))
        for n, sh, cells in strips)
    return """
<section id="magic">
  <h2><span class="num">08</span>Magic</h2>
  <p>Sixteen spells across seven shapes. Your class decides which of them you
     may learn and your level decides when; the failure chance falls as you gain
     levels and Intelligence, but never below five percent. <strong>A failed cast
     still costs the mana and the turn.</strong></p>
  %s
  <div class="tablewrap"><table class="items spells">
    <thead><tr><th>Spell</th><th class="n">Level</th><th class="n">Mana</th><th class="n">Fail</th><th>Shape</th></tr></thead>
    <tbody>%s</tbody>
  </table></div>
  <h3>How an effect is drawn</h3>
  <p>The source tileset shipped a set of monochrome animation strips &mdash; a
     streak growing and shrinking, an expanding shockwave, a burst scattering.
     Each frame is one 8&#215;8 tile, so an effect is drawn once per tile it
     covers rather than as a single scaled sprite: a fireball is a bloom of
     shockwave frames across its blast radius, timed so the outer ring starts
     late and the whole thing reads as expanding.</p>
  <div class="strips">%s</div>
  <div class="shots3">%s%s%s</div>
</section>""" % (plate(shots["spells"], "The spell list. Greyed entries are ones "
                       "your level or mana cannot reach yet."),
                 rows, strip_html,
                 plate(shots["missile"], "Magic Missile", small=True),
                 plate(shots["nova"], "Light Room", small=True),
                 plate(shots["fireball"], "Fireball", small=True))


def sec_town():
    cards = "".join(
        '<div class="shop">%s<h4>%s</h4></div>' % (spr(s["sheet"], s["cell"], 5),
                                                   esc(s["name"]))
        for s in shops)
    return """
<section id="town">
  <h2><span class="num">09</span>Town</h2>
  <p>The town is a place, not a screen. It is a walled compound on the
     overworld with gates east and west, a cobbled cross of streets, and six
     shopfronts standing on the street. Walk onto a shopfront and you are in
     that shop; walk onto the bed and you have slept. Nothing wild spawns
     inside the walls.</p>
  <div class="shops">%s</div>
  <p>Shops restock every time you surface, and the inn restores health, mana
     and food for twenty gold &mdash; and saves your game. Shops buy at a third
     of what they sell for, which is the usual arrangement.</p>
  <p>The Black Market carries things far out of depth at five times the price.
     It is a money sink and it is meant to be one, but it is also the only place
     a low-level character will ever see a wand of frost.</p>
  <div class="shots2">%s%s</div>
  <p class="note">Charisma shaves the price a little, which is the only thing
     Charisma does. Bards and Paladins get a real discount; Berserks do not.</p>
</section>""" % (cards,
                 plate(shots["town"], "The six shops and the inn.", small=True),
                 plate(shots["shop"], "Stock, with what you can afford in gold.",
                       small=True))


def sec_bestiary():
    by_lvl = sorted(monsters, key=lambda m: (m["lvl"], m["name"]))
    cells = []
    for m in by_lvl:
        tags = " ".join(FLAG_LABEL.get(f, "") for f in m["flags"]).strip()
        title = "%s &middot; level %d &middot; %s hp &middot; %s damage%s" % (
            esc(m["name"]), m["lvl"], dice(m["hp"]), dice(m["dam"]),
            (" &middot; " + tags) if tags else "")
        cells.append(
            '<figure class="mon" title="%s">%s<figcaption>'
            '<b>%s</b><span>%d</span></figcaption></figure>'
            % (title, spr(m["sheet"], m["cell"], 4), esc(m["name"]), m["lvl"]))
    return """
<section id="bestiary">
  <h2><span class="num">10</span>Bestiary</h2>
  <p>%d kinds, ordered by the depth at which they start appearing. Anything can
     turn up out of depth &mdash; roughly one spawn in ten rolls deeper than the
     floor it is on, which is where most deaths come from.</p>
  <div class="bestiary">%s</div>
</section>""" % (len(monsters), "".join(cells))


def sec_bosses():
    rows = []
    for b in bosses:
        drop = items[b["drop"]]
        rows.append("""
    <article class="boss">
      %s
      <div>
        <h3>%s</h3>
        <p class="depth">Depth %d</p>
        <dl>
          <div><dt>Health</dt><dd>%d</dd></div>
          <div><dt>Armour</dt><dd>%d</dd></div>
          <div><dt>Damage</dt><dd>%s</dd></div>
          <div><dt>Speed</dt><dd>%d</dd></div>
          <div><dt>Experience</dt><dd>%s</dd></div>
        </dl>
        <p class="drop">Drops %s <b>%s</b></p>
      </div>
    </article>""" % (spr2("bosses", b["cell"], 5), esc(b["name"]), b["depth"],
                     b["hp"], b["ac"], dice(b["dam"]), b["speed"],
                     "{:,}".format(b["xp"]),
                     spr(drop["sheet"], drop["cell"], 3), esc(drop["name"])))
    return """
<section id="bosses">
  <h2><span class="num">11</span>The seventeen</h2>
  <p>Seventeen floors are guarded. The guardian is not rolled and cannot be
     missed by bad luck &mdash; when you take the stairs to its depth it is there,
     in the room furthest from where you arrived, awake. Each is a 16&#215;16
     sprite rather than the usual 8&#215;8, and each drops one guaranteed item so
     the fight always pays.</p>
  %s
  <div class="bosses">%s</div>
</section>""" % (plate(shots["boss"], "Bloat the Chief on depth five. Bosses are "
                       "drawn at twice the size of anything else."), "".join(rows))


def sec_death():
    return """
<section id="death">
  <h2><span class="num">12</span>Dying</h2>
  <p>Death is permanent and it deletes the save. The tombstone records your
     class, your level, the deepest floor you reached and your kills, and your
     score goes to the title screen if it beats the one already there.</p>
  <p class="formula">score = experience + 250 &#215; deepest floor
     + 1500 &#215; bosses slain</p>
  <p>The inn saves. So does closing the pack, the town screen, or resting.
     There is one save slot and one character in it; a reload puts you on a
     freshly generated floor at the depth you left, because that is what the
     stairs do anyway.</p>
  <div class="kvlist">
    <div class="kv"><dt>Before you descend</dt><dd>Buy food. Buy a lantern
      before you buy a better sword &mdash; seeing the room first is worth more
      than hitting harder.</dd></div>
    <div class="kv"><dt>In a corridor</dt><dd>Fight in one. Groups cannot
      surround you there, and half the shallow bestiary comes in groups.</dd></div>
    <div class="kv"><dt>When it is going badly</dt><dd>The stairs are an escape,
      not just a way down. So is a scroll of teleport, which is why you should
      read one early to find out which flavour it is.</dd></div>
  </div>
</section>"""


def plate(uri, caption, small=False):
    if not uri:
        return ""
    return ('<figure class="plate%s"><div class="screen">'
            '<img src="%s" alt="" width="128" height="128"></div>'
            '<figcaption>%s</figcaption></figure>'
            % (" small" if small else "", uri, caption))


# ---------------------------------------------------------------- assemble
SHEET_CSS = "\n".join(
    ".sh-%s{background-image:url(%s);--sw:%d;--sh:%d}" % (n, s["uri"], s["w"], s["h"])
    for n, s in sheets.items())

NAV = [("screen", "The screen"), ("controls", "Controls"), ("classes", "The twelve"),
       ("overworld", "The continent"), ("mines", "The Mines"),
       ("turns", "Turns & combat"), ("items", "What you find"), ("magic", "Magic"),
       ("town", "Town"), ("bestiary", "Bestiary"), ("bosses", "The seventeen"),
       ("death", "Dying")]
nav_html = "".join('<li><a href="#%s"><span>%02d</span>%s</a></li>' % (i, n + 1, t)
                   for n, (i, t) in enumerate(NAV))

FONT_FACE = ("""@font-face{font-family:Rogue8;src:url(data:font/woff2;base64,%s)
 format("woff2");font-display:block}""" % font_b64) if font_b64 else ""

HTML = """<title>Roguemote &mdash; player's manual</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
%(font)s

:root{
  color-scheme:light dark;
  --ground:#0a0910; --panel:#14121e; --raised:#221e33;
  --rule:#3e3856; --ink:#e2ded2; --mute:#8b85a6;
  --gold:#ffc850; --blood:#c8283c; --mana:#29adff; --verdant:#00e436;
  --shadow:0 2px 0 rgba(0,0,0,.5);
  --measure:34rem;
}
/* The light neutral leans violet, away from the accent, so it reads as a
   chosen ground rather than inherited paper. */
@media (prefers-color-scheme:light){
  :root{
    --ground:#e3e1e8; --panel:#faf9fc; --raised:#eeecf3;
    --rule:#bfbbcb; --ink:#16141f; --mute:#57536b;
    --gold:#8a5c00; --blood:#a3182c; --mana:#0a6da6; --verdant:#007a3d;
    --shadow:0 1px 0 rgba(0,0,0,.08);
  }
}
:root[data-theme="dark"]{
  --ground:#0a0910; --panel:#14121e; --raised:#221e33;
  --rule:#3e3856; --ink:#e2ded2; --mute:#8b85a6;
  --gold:#ffc850; --blood:#c8283c; --mana:#29adff; --verdant:#00e436;
  --shadow:0 2px 0 rgba(0,0,0,.5);
}
:root[data-theme="light"]{
  --ground:#e3e1e8; --panel:#faf9fc; --raised:#eeecf3;
  --rule:#bfbbcb; --ink:#16141f; --mute:#57536b;
  --gold:#8a5c00; --blood:#a3182c; --mana:#0a6da6; --verdant:#007a3d;
  --shadow:0 1px 0 rgba(0,0,0,.08);
}

*{box-sizing:border-box}
body{
  margin:0; background:var(--ground); color:var(--ink);
  font:400 17px/1.65 "Iowan Old Style","Palatino Linotype",Palatino,Georgia,
       "Times New Roman",serif;
  -webkit-font-smoothing:antialiased;
}
.px{font-family:Rogue8,ui-monospace,Menlo,Consolas,monospace;
    letter-spacing:.06em; font-weight:400}
code,kbd,.n,.mono{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;
  font-variant-numeric:tabular-nums}

/* --- sprites: one embedded sheet per family, cropped by position --- */
.spr{
  display:inline-block; vertical-align:middle;
  width:calc(8px * var(--s)); height:calc(8px * var(--s));
  background-repeat:no-repeat;
  background-size:calc(var(--sw) * var(--s) * 1px) calc(var(--sh) * var(--s) * 1px);
  background-position:calc(var(--x) * -8px * var(--s)) calc(var(--y) * -8px * var(--s));
  image-rendering:pixelated; image-rendering:crisp-edges;
  flex:none;
}
.spr.big{width:calc(16px * var(--s)); height:calc(16px * var(--s))}

/* --- shell --- */
.wrap{max-width:78rem; margin:0 auto; padding:0 1.25rem}
.layout{display:grid; grid-template-columns:14rem minmax(0,1fr); gap:3.5rem;
        align-items:start; padding-bottom:6rem}
@media (max-width:64rem){ .layout{grid-template-columns:1fr; gap:2rem} }

/* --- masthead --- */
header.mast{
  border-bottom:1px solid var(--rule); background:var(--panel);
  margin-bottom:3rem;
}
.mast .wrap{padding-top:3.5rem; padding-bottom:2.5rem}
.mast h1{
  margin:0; font-size:clamp(2.4rem,7vw,4.6rem); line-height:1;
  color:var(--gold); text-wrap:balance;
}
.mast .sub{margin:1rem 0 0; max-width:var(--measure); color:var(--mute);
           font-size:1.05rem}
.mast .cast{display:flex; gap:.5rem; margin-top:2rem; flex-wrap:wrap}
.mast .meta{display:flex; gap:1.25rem; flex-wrap:wrap; margin-top:1.75rem;
  font-size:.72rem; text-transform:uppercase; letter-spacing:.12em;
  color:var(--mute)}
.mast .meta b{color:var(--ink); font-weight:400}

/* --- index rail --- */
nav.index{position:sticky; top:1.5rem}
nav.index h2{font-size:.7rem; letter-spacing:.16em; text-transform:uppercase;
  color:var(--mute); margin:0 0 .9rem}
nav.index ol{list-style:none; margin:0; padding:0; display:flex;
  flex-direction:column; gap:.1rem}
nav.index a{display:flex; gap:.6rem; align-items:baseline; padding:.3rem .4rem;
  color:var(--ink); text-decoration:none; font-size:.82rem; border-radius:2px}
nav.index a span{color:var(--mute); font-size:.68rem}
nav.index a:hover,nav.index a:focus-visible{background:var(--raised); color:var(--gold)}
@media (max-width:64rem){
  nav.index{position:static; border-bottom:1px solid var(--rule);
            padding-bottom:1.5rem}
  nav.index ol{flex-direction:row; flex-wrap:wrap; gap:.35rem}
  nav.index a{background:var(--panel); border:1px solid var(--rule)}
}

/* --- sections --- */
main{min-width:0}
section{margin:0 0 4.5rem; scroll-margin-top:1.5rem}
section > p, section > .kvlist, section > .keys, section > .formula{
  max-width:var(--measure)}
h2{font-size:1.55rem; margin:0 0 1.1rem; display:flex; align-items:baseline;
   gap:.75rem; text-wrap:balance; border-bottom:1px solid var(--rule);
   padding-bottom:.6rem}
h2 .num{font-size:.75rem; color:var(--gold); letter-spacing:.1em}
h3{font-size:1.05rem; margin:2.25rem 0 .5rem; color:var(--gold)}
h4{margin:0; font-size:.95rem}
p{margin:0 0 1rem}
p.note{color:var(--mute); font-size:.92rem}
strong{font-weight:400; color:var(--gold)}
em{font-style:italic}
kbd{display:inline-block; padding:.05em .45em; font-size:.82em;
  border:1px solid var(--rule); border-bottom-width:2px; border-radius:3px;
  background:var(--raised); color:var(--ink)}
code{font-size:.88em; color:var(--gold)}
.formula{background:var(--panel); border-left:2px solid var(--gold);
  padding:.85rem 1rem; font-family:ui-monospace,Menlo,Consolas,monospace;
  font-size:.86rem; color:var(--ink)}

/* --- key/value lists --- */
.kvlist{display:flex; flex-direction:column; gap:.1rem; margin:1.5rem 0}
.kv{display:grid; grid-template-columns:7.5rem minmax(0,1fr); gap:1rem;
  padding:.7rem 0; border-top:1px solid var(--rule)}
.kv dt{color:var(--gold); font-size:.78rem; letter-spacing:.08em;
  text-transform:uppercase; padding-top:.22rem}
.kv dd{margin:0; font-size:.95rem}
@media (max-width:36rem){ .kv{grid-template-columns:1fr; gap:.2rem} }

.keys{display:flex; flex-direction:column; gap:.1rem; margin:1.5rem 0}
.key{display:grid; grid-template-columns:7.5rem minmax(0,1fr); gap:1rem;
  padding:.7rem 0; border-top:1px solid var(--rule); align-items:start}
.key kbd{justify-self:start}
.key p{margin:0; font-size:.95rem}
@media (max-width:36rem){ .key{grid-template-columns:1fr; gap:.35rem} }

/* --- plates: a device screenshot in a bezel --- */
.plate{margin:1.75rem 0; width:max-content; max-width:100%%}
.plate.small{margin:0}
.plate .screen{
  background:#000; border:1px solid var(--rule); border-radius:10px;
  padding:10px; box-shadow:var(--shadow); width:max-content; max-width:100%%;
}
.plate img{
  width:20rem; max-width:100%%; height:auto; image-rendering:pixelated;
  image-rendering:crisp-edges; display:block; border-radius:2px;
}
.plate figcaption{max-width:22rem}
.plate figcaption{margin-top:.6rem; font-size:.8rem; color:var(--mute);
  line-height:1.45}
.shots2{display:grid; grid-template-columns:repeat(auto-fit,minmax(13rem,1fr));
  gap:1.5rem; margin:1.75rem 0}
.shots3{display:grid; grid-template-columns:repeat(auto-fit,minmax(11rem,1fr));
  gap:1.25rem; margin:1.75rem 0}

/* --- classes --- */
.grid,.terr-grid,.shops,.bestiary,.bosses{
  display:grid; gap:0; background:var(--panel);
  border:1px solid var(--rule); border-width:1px 0 0 1px}
.grid > *,.terr-grid > *,.shops > *,.bestiary > *,.bosses > *{
  border:0 solid var(--rule); border-width:0 1px 1px 0}
.cls-grid{grid-template-columns:repeat(auto-fill,minmax(15rem,1fr)); margin:1.75rem 0}
.cls{padding:1rem}
.cls header{display:flex; align-items:center; gap:.7rem; margin-bottom:.75rem}
.cls h3{margin:0; font-size:1rem; color:var(--gold)}
.cls .stats{display:grid; grid-template-columns:repeat(3,1fr); gap:.3rem .5rem;
  margin-bottom:.75rem}
.cls .stats div{display:flex; justify-content:space-between;
  font-family:ui-monospace,Menlo,Consolas,monospace; font-size:.78rem;
  font-variant-numeric:tabular-nums; border-bottom:1px dotted var(--rule)}
.cls .stats span{color:var(--mute)}
.cls .starts{font-size:.85rem; margin:0 0 .5rem}
.cls .spells{font-size:.8rem; margin:0; color:var(--mute); line-height:1.5}
.cls .spells span{color:var(--gold); text-transform:uppercase;
  letter-spacing:.08em; font-size:.68rem; margin-right:.4rem}
.cls .nomagic{font-style:italic}

/* --- terrain --- */
.terr-grid{grid-template-columns:repeat(auto-fill,minmax(18rem,1fr)); margin:1.75rem 0}
.terr{padding:.9rem; display:flex; gap:.8rem; align-items:flex-start}
.tthumb{width:6.25rem; height:5rem; image-rendering:pixelated;
  image-rendering:crisp-edges; border:1px solid var(--rule); flex:none}
.terr h4{color:var(--gold); margin-bottom:.15rem}
.terr p{margin:0; font-size:.85rem; color:var(--mute)}

/* --- tables --- */
.tablewrap{overflow-x:auto; margin:1rem 0 1.5rem;
  border:1px solid var(--rule); background:var(--panel)}
table{border-collapse:collapse; width:100%%; font-size:.85rem}
th{text-align:left; font:400 .68rem/1 inherit; letter-spacing:.12em;
  text-transform:uppercase; color:var(--mute); padding:.7rem .8rem;
  border-bottom:1px solid var(--rule); white-space:nowrap;
  font-family:ui-monospace,Menlo,Consolas,monospace}
td{padding:.42rem .8rem; border-bottom:1px solid var(--rule)}
tbody tr:last-child td{border-bottom:0}
th:not(:nth-child(1)):not(:nth-child(2)):not(:nth-child(3)),
td.n{width:1%%; white-space:nowrap}
td.n,th.n{text-align:right; font-family:ui-monospace,Menlo,Consolas,monospace;
  font-variant-numeric:tabular-nums; white-space:nowrap; color:var(--mute)}
td.ic{width:1.5rem; padding-right:0}
td.nm{white-space:nowrap}
td.ef{color:var(--mute); font-size:.82rem}
td.d{white-space:nowrap; font-family:ui-monospace,Menlo,Consolas,monospace}
.items tbody tr:hover{background:var(--raised)}
.itemgroup{margin-bottom:2rem}

/* --- fx strips --- */
.strips{display:flex; flex-direction:column; gap:.1rem; margin:1.5rem 0}
.strip{display:grid; grid-template-columns:6rem minmax(0,1fr); gap:1rem;
  align-items:center; padding:.5rem 0; border-top:1px solid var(--rule)}
.strip span{font-size:.72rem; text-transform:uppercase; letter-spacing:.1em;
  color:var(--gold)}
.strip div{display:flex; gap:.35rem; flex-wrap:wrap; background:#000;
  padding:.35rem; border:1px solid var(--rule); width:max-content;
  max-width:100%%}

/* --- shops --- */
.shops{grid-template-columns:repeat(auto-fill,minmax(11rem,1fr)); margin:1.75rem 0}
.shop{padding:.9rem; display:flex; gap:.7rem; align-items:center}
.shop h4{font-size:.88rem}

/* --- bestiary --- */
.bestiary{grid-template-columns:repeat(auto-fill,minmax(7.5rem,1fr)); margin:1.75rem 0}
.mon{margin:0; padding:.6rem .5rem; display:flex;
  flex-direction:column; align-items:center; gap:.35rem; text-align:center}
.mon:hover{background:var(--raised)}
.mon figcaption{display:flex; flex-direction:column; gap:.1rem; line-height:1.25}
.mon b{font-weight:400; font-size:.72rem}
.mon span{font-family:ui-monospace,Menlo,Consolas,monospace; font-size:.62rem;
  color:var(--gold)}

/* --- bosses --- */
.bosses{grid-template-columns:repeat(auto-fill,minmax(19rem,1fr)); margin:1.75rem 0}
.boss{padding:1rem; display:flex; gap:1rem;
  align-items:flex-start; margin:0}
.boss h3{margin:0; font-size:1rem}
.boss .depth{font-family:ui-monospace,Menlo,Consolas,monospace; font-size:.7rem;
  color:var(--mute); margin:.1rem 0 .6rem; text-transform:uppercase;
  letter-spacing:.1em}
.boss dl{margin:0 0 .6rem; display:grid; grid-template-columns:1fr 1fr;
  gap:.1rem .8rem}
.boss dl div{display:flex; justify-content:space-between; gap:.5rem;
  border-bottom:1px dotted var(--rule); font-size:.75rem;
  font-family:ui-monospace,Menlo,Consolas,monospace;
  font-variant-numeric:tabular-nums}
.boss dt{color:var(--mute)}
.boss dd{margin:0}
.boss .drop{margin:0; font-size:.8rem; display:flex; align-items:center;
  gap:.4rem}
.boss .drop b{font-weight:400; color:var(--gold)}

footer{border-top:1px solid var(--rule); padding:2rem 0 3rem; color:var(--mute);
  font-size:.82rem}
footer p{max-width:var(--measure)}
a{color:var(--gold)}
:focus-visible{outline:2px solid var(--gold); outline-offset:2px}
@media (prefers-reduced-motion:reduce){*{animation:none!important;
  transition:none!important}}
%(sheets)s
</style>

<header class="mast">
  <div class="wrap">
    <h1 class="px">ROGUEMOTE</h1>
    <p class="sub">A turn-based roguelike for the Thumby Color. Moria-shaped
      dungeons under a continent you walk across, in 128&#215;128 pixels.
      This is the manual.</p>
    <div class="cast">%(cast)s</div>
    <div class="meta">
      <span><b>%(nmon)d</b> monsters</span>
      <span><b>%(nboss)d</b> bosses</span>
      <span><b>%(ncls)d</b> classes</span>
      <span><b>%(nitem)d</b> item kinds</span>
      <span><b>%(nspell)d</b> spells</span>
      <span><b>35</b> guarded floors</span>
    </div>
  </div>
</header>

<div class="wrap layout">
  <nav class="index">
    <h2>Contents</h2>
    <ol>%(nav)s</ol>
  </nav>
  <main>
%(body)s
    <footer>
      <p>Every sprite on this page is cropped live out of the game's own baked
        sheets, every table is parsed from its source, and the headings are set
        in the 8&#215;8 CP437 face the game renders with. Art is the CC0 Simple
        Roguelike Tileset; the engine is Mote.</p>
    </footer>
  </main>
</div>
"""

cast = "".join(spr("characters", c["cell"], 4) for c in classes)
body = "\n".join([
    sec_screen(), sec_controls(), sec_classes(), sec_overworld(), sec_mines(),
    sec_turns(), sec_items(), sec_magic(), sec_town(), sec_bestiary(),
    sec_bosses(), sec_death(),
])

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(HTML % dict(font=FONT_FACE, sheets=SHEET_CSS, nav=nav_html, cast=cast,
                        body=body, nmon=len(monsters), nboss=len(bosses),
                        ncls=len(classes), nitem=len(items), nspell=len(spells)))
print("[guide] %s  %.0f KB" % (OUT, os.path.getsize(OUT) / 1024))
