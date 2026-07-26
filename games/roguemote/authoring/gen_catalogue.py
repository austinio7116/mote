#!/usr/bin/env python3
"""
Generate a self-contained HTML catalogue of every extracted sprite, with my
best-guess interpretation + an in-game use idea per tile, for the developer to
review and correct. Sprites are embedded as base64 data URIs (Artifact CSP
blocks external requests). Output: /tmp/roguemote_sprites.html
"""
import os, io, base64
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = Image.open(os.path.join(HERE, "source_tileset.png"))
IDX  = SRC.load(); PAL = SRC.getpalette(); TS = 8

def tile_datauri(c, r, cw=1, ch=1):
    """RGBA PNG (idx0 transparent) of a cw x ch tile block at (c,r) -> data URI + non-empty flag."""
    im = Image.new("RGBA", (cw*TS, ch*TS)); px = im.load(); any_ink = False
    for yy in range(ch*TS):
        for xx in range(cw*TS):
            ix = IDX[c*TS+xx, r*TS+yy]
            if ix == 0:
                px[xx, yy] = (0, 0, 0, 0)
            else:
                px[xx, yy] = (PAL[ix*3], PAL[ix*3+1], PAL[ix*3+2], 255); any_ink = True
    buf = io.BytesIO(); im.save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode(), any_ink

COLORS = ["Red", "Blue", "Yellow", "Green", "Grey/white"]   # the 5 colour-variant rows (r1..r5)

# ---------------------------------------------------------------------------
# Sections: (abs col0,row0,col1,row1, title, blurb, use, labeler)
# labeler(col,row) -> (name, use_or_None, uncertain_bool). Return None to skip a
# tile entirely (treated as intentionally blank).
# ---------------------------------------------------------------------------
def colorword(row, base_row):
    i = row - base_row
    return COLORS[i] if 0 <= i < len(COLORS) else "?"

def L(name, use=None, q=False):
    return (name, use, q)

SECTIONS = []
def section(c0, r0, c1, r1, title, blurb, use, labeler):
    SECTIONS.append((c0, r0, c1, r1, title, blurb, use, labeler))

# 1. Chests -----------------------------------------------------------------
def lab_chests(c, r):
    state = "Closed" if c == 0 else "Open"
    return L(f"{colorword(r,1)} chest ({state.lower()})",
             "Loot container. Colour = rarity tier (red common -> grey rare?).")
section(0,1,1,5, "Chests", "Two columns (closed / open) x five colour rows.",
        "Treasure containers; pair closed+open frames for an open animation.", lab_chests)

# 2. Stone furniture --------------------------------------------------------
def lab_furn(c, r):
    kind = {2:"Table / workbench", 3:"Altar / cabinet", 4:"Bed / sarcophagus"}[c]
    return L(f"{colorword(r,1)} {kind.lower()}",
             "Room dressing. Bed = rest/save point; altar = shrine; table = shop/crafting.", q=True)
section(2,1,4,5, "Stone furniture", "Grey dungeon furniture in three columns x five tints.",
        "Fill rooms; interactables (rest, pray, craft).", lab_furn)

# 3. Faces / skulls / keys --------------------------------------------------
def lab_fsk(c, r):
    kind = {5:"Creature face / mask", 6:"Skull", 7:"Key"}[c]
    hint = {5:"Enemy token or mask pickup.", 6:"Decor, drop, or death marker.",
            7:"Colour-locked door key."}[c]
    return L(f"{colorword(r,1)} {kind.lower()}", hint, q=(c==5))
section(5,1,7,5, "Faces · skulls · keys", "Creature face, skull, and key — five colours each.",
        "Keys for colour-locked doors; skulls as drops/decor.", lab_fsk)

# 4. Runes ------------------------------------------------------------------
def lab_rune(c, r):
    return L(f"{colorword(r,1)} rune glyph", "Spell/ability icon, inscription, or magic lock.", q=True)
section(8,1,10,4, "Runes", "Colour rune/letter glyphs (3 columns x 4 colours).",
        "Spell runes, ability icons, magical inscriptions.", lab_rune)

# 5. Doors / gems / banners -------------------------------------------------
def lab_dgb(c, r):
    if r == 0:
        return L("Ladder / curtain top", "Vertical connector top, or banner header.", q=True)
    kind = {11:"Door", 12:"Door (variant)", 13:"Large gem / orb / slime", 14:"Banner / hanging"}[c]
    return L(f"{colorword(r,1)} {kind.lower()}", "Doors, big gem pickups, or wall banners.", q=True)
section(11,0,14,5, "Doors · gems · banners", "Doors, big colour orbs/gems, hanging banners; row 0 ladders.",
        "Level exits, boss-gem objectives, wall decoration.", lab_dgb)

# 6. Light props ------------------------------------------------------------
PROPS = {
 (0,6):"Computer terminal (dark)",(1,6):"Computer terminal (green)",
 (0,7):"Monitor / screen",(1,7):"Terminal (green text)",
 (2,6):"Log / plank",(3,6):"Log pile",(4,6):"Wood beam",(5,6):"Bridge plank",
 (6,6):"Fence rail",(7,6):"Fence post",(8,6):"Wood bridge",(9,6):"Wood bridge end",
 (2,7):"Log",(3,7):"Planks",(4,7):"Bridge",(5,7):"Fence",(6,7):"Fence",(7,7):"Bridge",(8,7):"Bridge",(9,7):"Bridge cap",
 (0,8):"Book (red)",(1,8):"Book (brown)",(2,8):"Book (blue)",(3,8):"Tome / spellbook",
 (0,9):"House / tent (red roof)",(1,9):"Igloo / dome",(2,9):"Helmet",(3,9):"Fireball / ember",(4,9):"Flag / banner",(5,9):"Flag",
 (7,8):"Torch (lit)",(8,8):"Torch",(6,8):"Stone block",
}
def lab_prop(c, r):
    if (c,r) in PROPS: return L(PROPS[(c,r)], "Dungeon/overworld dressing & structures.", q=True)
    return L("Prop / structure", "Dressing (wood, terminals, torches, buildings).", q=True)
section(0,6,9,9, "Light props & structures",
        "Terminals, logs/planks/bridges/fences, books, buildings, torches.",
        "Overworld & dungeon set-dressing; sci-fi terminals for a tech theme.", lab_prop)

# 7. Trinkets / small items -------------------------------------------------
def lab_trink(c, r):
    return L("Small item / trinket",
             "Slimes, gems, mushrooms, snakes, rocks, clouds, bushes, books — pickups & tiny props.", q=True)
section(0,8,15,13, "Trinkets & small nature",
        "A dense grab-bag: colour slimes/puddles, gems, red mushrooms, green snakes, rocks, clouds, bushes.",
        "Ground pickups, foraging, ambient critters & foliage.", lab_trink)

# 8. Weapons & potions ------------------------------------------------------
def lab_wp(c, r):
    return L("Weapon or potion", "Swords, spears, bows, flasks — equipment & consumables.", q=True)
section(16,0,23,5, "Weapons & potions",
        "Melee/ranged weapons plus coloured potions/flasks.",
        "Equipment drops; potions as consumables (colour = effect).", lab_wp)

# 9. Food -------------------------------------------------------------------
def lab_food(c, r):
    return L("Food item", "Meat, fruit, bread, pie, egg, cheese — healing/rations.", q=True)
section(16,8,27,13, "Food",
        "Meat, cherries, apple, watermelon, carrot, cheese, bread, burger, pie, egg (+ a chef portrait).",
        "Healing consumables, rations, cooking/crafting inputs.", lab_food)

# 10. Treasure & ore --------------------------------------------------------
def lab_ore(c, r):
    if r == 19: return L("Helmet / armour (colour)", "Wearable armour drop; colour = material tier.", q=True)
    return L("Treasure / ore", "Gold/silver/copper nuggets, gems, crown — currency & mining loot.", q=True)
section(16,16,27,19, "Treasure & ore",
        "Gold/silver/copper ore piles, gems, a crown; bottom row is coloured helmets/armour.",
        "Currency, mining yields, gem objectives, armour pickups.", lab_ore)

# 11. Loot furniture --------------------------------------------------------
LOOT = {(16,21):"Table",(22,22):"Barrel / crate",(23,22):"Barrel",(16,23):"Chalice / goblet",
        (24,20):"Shield",(24,23):"Shield"}
def lab_loot(c, r):
    if (c,r) in LOOT: return L(LOOT[(c,r)], "Breakable/lootable dungeon objects.", q=True)
    return L("Bones / debris / prop", "Skeleton parts, barrels, chalice, shield — dungeon loot & decor.", q=True)
section(16,20,24,23, "Loot furniture & bones",
        "Table, barrels/crates, chalice, scattered bones, shield.",
        "Breakables, loot spawners, grisly decor.", lab_loot)

# 12. Tools & wands ---------------------------------------------------------
def lab_tool(c, r):
    return L("Tool / wand", "Pickaxes, axes, hammers, staffs, wands — tools & magic implements.", q=True)
section(16,28,31,31, "Tools & wands",
        "Pickaxes, axes, hammers (gold/iron), staffs and wands (some tipped).",
        "Mining/chopping tools, mage weapons, crafting gear.", lab_tool)

# 13. Elemental weapons -----------------------------------------------------
def lab_elem(c, r):
    return L("Elemental weapon", "Ice/fire/colour swords & staves — enchanted gear.", q=True)
section(16,32,30,34, "Elemental weapons",
        "Ice/fire and other coloured swords & staves.",
        "Rare enchanted weapons; colour = damage element.", lab_elem)

# 14. Guns ------------------------------------------------------------------
def lab_gun(c, r):
    return L("Gun", "Pistol/rifle — modern or sci-fi ranged weapon.", q=True)
section(16,35,22,35, "Guns",
        "Pistols and rifles (grey/sci-fi styling).",
        "Ranged weapons for a modern/sci-fi roguelike.", lab_gun)

# 15. Scroll / arrow / bombs -------------------------------------------------
# Was catalogued as a "2x2 large NPC portrait (dark-haired man)" -- wrong. At 20x
# it is four unrelated items; the scroll's parchment reads as a face at 8x8.
def lab_scrollbomb(c, r):
    return L({(24,0):"Scroll", (25,0):"Arrow",
              (24,1):"Bomb (small)", (25,1):"Bomb (large)"}.get((c,r), "?"),
             "Pickup item.")
section(24,0,25,1, "Scroll · arrow · bombs",
        "Four separate items, not one sprite: scroll, arrow, small and large bomb.",
        "Consumables and ammo pickups.", lab_scrollbomb)

# 16. Characters ------------------------------------------------------------
def lab_char(c, r):
    if c == 32 and r == 0: return L("@ player glyph", "Classic roguelike player token.")
    if r <= 2: return L("Character head / portrait", "Small face for NPC/party icons.", q=True)
    if r >= 5: return L("Villager / townsfolk", "Neutral NPC body sprite.", q=True)
    return L("Adventurer body", "Playable class / party member (warrior, mage, rogue, ranger).", q=True)
section(32,0,47,7, "Characters",
        "The @ glyph, small head portraits, full-body adventurers (classes) and villager heads.",
        "Player & party sprites, NPCs, class select.", lab_char)

# 17. Animals ---------------------------------------------------------------
def lab_anim(c, r):
    return L("Animal / vermin", "Beasts, birds, bats, pigs, rats, snakes, spiders, frogs, lizards.", q=True)
section(32,8,47,15, "Animals & vermin",
        "Dogs, deer, chickens, rooster, birds, bats, pigs, rats, snakes, spiders, frogs, mushroom-critters.",
        "Low-tier enemies, wildlife, familiars, farm animals.", lab_anim)

# 18. Monsters --------------------------------------------------------------
def lab_mon(c, r):
    return L("Monster", "Cows/goats, goblins, demons, ghosts, skeletal foes.", q=True)
section(32,16,47,23, "Monsters",
        "Cows/goats, goblins, imps/demons, ghosts and skeletal enemies.",
        "Mid-tier enemy roster.", lab_mon)

# 19. Crowns & FX -----------------------------------------------------------
def lab_cr(c, r):
    if r == 29: return L("Crown (colour)", "Royalty item, boss drop, rank marker.", q=True)
    return L("Armour / slime / FX", "Armour, slimes, fire & explosion frames.", q=True)
section(32,24,40,31, "Crowns · armour · FX",
        "Coloured crowns, armour pieces, slimes, fire/explosion effect frames.",
        "Boss loot, status effects, explosion/impact FX.", lab_cr)

# 20. Bosses ----------------------------------------------------------------
def lab_boss(c, r):
    return L("Boss / large enemy", "2x2 demons, golems, dragon/hydra, skeletons, wizards.", q=True)
section(32,32,47,39, "Bosses",
        "Large 2x2 enemies: demons, golems, a dragon/hydra, skeleton warriors, wizards.",
        "Boss encounters, elite foes.", lab_boss)

# 21. Boulders & mountains --------------------------------------------------
def lab_bm(c, r):
    return L("Boulder / mountain", "Large rocks and snow-capped peaks (terrain features).", q=True)
section(5,25,10,28, "Boulders & mountains",
        "Big grey boulders and navy/snow mountain peaks.",
        "Overworld terrain features, cave obstacles.", lab_bm)

# 22. Tiny UI icons ---------------------------------------------------------
def lab_uii(c, r):
    return L("Tiny UI icon", "Hearts, eyes, stars, snowflakes, droplets, fire, notes, letters.", q=True)
section(0,14,15,15, "Tiny UI icons (magenta strip)",
        "A dense row of status glyphs: hearts, eyes, stars, snowflakes, droplets, fire, music notes, letters.",
        "HUD status icons, buffs/debuffs, minimap markers.", lab_uii)

# 23. Arrows & gauges -------------------------------------------------------
def lab_arr(c, r):
    if r <= 19 and c <= 53: return L("Direction arrow", "Cursor/movement/menu arrow (8 directions).", q=True)
    return L("Gauge / bar", "Signal/battery/progress bar or green screen.", q=True)
section(48,16,59,19, "Arrows & gauges",
        "Eight-direction arrows plus signal/progress bars and green screens.",
        "Menu cursors, movement hints, HUD meters.", lab_arr)

# 24. Buttons ---------------------------------------------------------------
def lab_btn(c, r):
    return L("Button prompt", "Gamepad prompts: coloured circles, A/B/X/Y, O/X face buttons.", q=True)
section(48,20,63,23, "Button prompts",
        "Coloured button circles and A / B / X / Y / O / X gamepad glyphs.",
        "On-screen control hints, tutorials, menus.", lab_btn)

# 25. Status & emotes -------------------------------------------------------
def lab_se(c, r):
    if r == 31 and c <= 52: return L("Heart (health)", "Full/half/empty heart for a health meter.")
    return L("Status / emote / element", "Faces, check/cross, skull, element icons, speech bubbles.", q=True)
section(48,24,63,31, "Status · emotes · elements",
        "Face emotes, check/cross, skull, element icons (leaf/note/snow/fire/lightning/cross), hearts, speech bubbles.",
        "HUD feedback, health meter, dialogue bubbles, damage-type icons.", lab_se)

# 26. Symbols ---------------------------------------------------------------
def lab_sym(c, r):
    return L("Gender / alchemy symbol", "Male/female/mercury and a mirror.", q=True)
section(48,32,51,32, "Symbols",
        "Gender / alchemy symbols and a hand-mirror.",
        "Alchemy, character sex, item flavour.", lab_sym)

# 27. White furniture -------------------------------------------------------
def lab_fw(c, r):
    return L("Top-down furniture", "Beds, tables, dressers (bright/clean set).", q=True)
section(0,53,5,55, "White furniture (top-down)",
        "Clean top-down beds, tables and dressers.",
        "Interior/house tiles for a bright top-down game.", lab_fw)

# 28. Blueprint -------------------------------------------------------------
def lab_bp(c, r):
    return L("Blueprint / floor-plan", "White line-art room/wall schematic.", q=True)
section(0,50,11,52, "Blueprint tiles",
        "White floor-plan line-art (rooms, walls as schematic lines).",
        "Minimap, level editor, blueprint/hacking screens.", lab_bp)

# 29. Terrain raw sets ------------------------------------------------------
def lab_terr(name):
    def f(c, r): return L(name, None, q=True)
    return f
section(0,20,15,23, "Colour panels (terrain)",
        "Green/pink/blue/yellow rounded panel frames (two shades each).",
        "UI panels, coloured floor zones, gift boxes/crates.", lab_terr("Colour panel piece"))
section(0,16,7,18, "Purple brick wall (terrain)",
        "Purple/pink brick wall tiles with a peephole.",
        "Alt dungeon wall theme (author a rule in Studio).", lab_terr("Purple brick tile"))
section(0,35,15,39, "Stone-brick wall set (terrain)",
        "Full navy/grey brick set incl. arch, window, gargoyle face — nine-slice source for wall_brick.",
        "Dungeon walls (the wall_brick.tileset draws from block 3-5,35-37) + door/feature tiles.", lab_terr("Stone-brick tile"))
section(0,44,15,46, "Temple / aztec wall (terrain)",
        "Navy wall with orange/purple tiki totems, gold banding, and a big idol face.",
        "Temple/jungle-ruin walls & an idol feature (decorative; not a clean nine-slice).", lab_terr("Temple wall tile"))
section(0,40,15,47, "Jungle grass & steps (terrain)",
        "Grass field with tiki columns, gold step border, a pink flower, and top friezes.",
        "Garden/jungle floors & borders (floor_grass draws from the plain centres).", lab_terr("Grass/step tile"))
section(0,25,4,28, "Cobblestone floors (terrain)",
        "Five-shade cobblestone floor textures (cream/grey/brown/navy/black).",
        "Dungeon floors (floor_cobble uses the grey shade); pick a shade per biome.", lab_terr("Cobble floor tile"))
section(48,33,63,39, "Terrain edges (terrain)",
        "Rings/holes, wave bands, sticks, mounds, and round rocks/boulders.",
        "Water/foam bands, pits, pebbles, hill tops (edge/detail tiles).", lab_terr("Edge/detail tile"))

# 36. Jewellery, chalices & regalia -----------------------------------------
# This block was in NO section until now, which is exactly why the game has no
# rings: the art was never surfaced to be labelled, so the ring items ended up
# pointed at the big cut gems in treasure_ore instead. Four metal rows -- white,
# gold, blue, red -- of goblets, torcs, medallions, plain bands and gemmed
# bands, plus earrings, a pendant necklace and a jewelled ring on the top row.
def lab_jewel(c, r):
    metal = {23: "silver", 24: "gold", 25: "blue", 26: "red"}.get(r, "?")
    kind = {16: "goblet", 17: "chalice", 18: "?", 19: "crossed blades",
            20: "crossed blades", 21: "?", 22: "torc / horn", 23: "torc (open)",
            24: "medallion", 25: "plain band", 26: "gemmed band",
            27: "earrings", 28: "pendant necklace", 29: "jewelled ring",
            30: "?"}.get(c, "?")
    return L(f"{metal} {kind}", "Ring / amulet / drinking vessel.", q=True)
section(16,23,30,26, "Jewellery, chalices & regalia",
        "Four metal rows of goblets, torcs, medallions, plain and gemmed bands, "
        "plus earrings, a necklace and a jewelled ring.",
        "Rings and amulets (TV_RING), and treasure props.", lab_jewel)

def render_section(c0, r0, c1, r1, title, blurb, use, labeler):
    cards = []
    nq = 0
    for r in range(r0, r1+1):
        for c in range(c0, c1+1):
            uri, ink = tile_datauri(c, r)
            if not ink: continue
            res = labeler(c, r)
            if res is None: continue
            name, tuse, q = res
            if q: nq += 1
            qcls = " q" if q else ""
            usehtml = f'<div class="use">{tuse}</div>' if tuse else ""
            cards.append(
                f'<figure class="card{qcls}">'
                f'<div class="thumb"><img src="{uri}" alt="{name}"></div>'
                f'<figcaption><span class="coord">{c},{r}</span>'
                f'<span class="name">{name}</span>{usehtml}</figcaption></figure>')
    if not cards: return "", 0, 0
    sid = title.split(" (")[0].lower().replace(" ", "-").replace("·","").replace("/","").replace("--","-")
    head = (f'<section id="{sid}"><header class="sec">'
            f'<div><p class="eyebrow">{c0},{r0} &ndash; {c1},{r1}</p>'
            f'<h2>{title}</h2><p class="blurb">{blurb}</p>'
            f'<p class="use-lead">Use: {use}</p></div>'
            f'<div class="count">{len(cards)}<span>tiles</span></div></header>'
            f'<div class="grid">{"".join(cards)}</div></section>')
    return head, len(cards), nq, sid, title

# font block (whole atlas as one image) -------------------------------------
def font_block():
    uri, _ = tile_datauri(48, 0, 16, 16)
    return (f'<section id="font"><header class="sec"><div>'
            f'<p class="eyebrow">48,0 &ndash; 63,15</p><h2>CP437 font</h2>'
            f'<p class="blurb">A complete 256-glyph 8&times;8 code-page-437 font: symbols, digits, '
            f'A&ndash;Z, a&ndash;z, accented letters, box-drawing, block elements. Baked two ways: a '
            f'<code>rogue8</code> MoteFont (proportional, via <code>text_font()</code>) and a '
            f'<code>font_cp437</code> tile atlas (monospace blit, char c &rarr; cell c%16,c//16).</p>'
            f'<p class="use-lead">Use: authentic ASCII-roguelike text, HUD labels, damage numbers.</p>'
            f'</div><div class="count">256<span>glyphs</span></div></header>'
            f'<div class="fontwrap"><img src="{uri}" alt="CP437 font atlas"></div></section>')

def build():
    body = []; nav = []; total = 0; totq = 0
    for sec in SECTIONS:
        html, n, nq, sid, title = render_section(*sec)
        if not html: continue
        body.append(html); total += n; totq += nq
        nav.append(f'<a href="#{sid}">{title.split(" (")[0]}</a>')
    body.append(font_block()); nav.append('<a href="#font">CP437 font</a>')

    css = """
:root{color-scheme:light dark;
 --bg:#f4f2ec;--panel:#fffdf8;--panel2:#efece3;--ink:#26242c;--muted:#6f6a7d;
 --line:#ddd8cc;--gold:#a9791a;--gold-ink:#7c580f;--torch:#c1471f;--cyan:#137a80;
 --chk-a:#d9d5cb;--chk-b:#c7c2b6;}
@media (prefers-color-scheme:dark){:root{
 --bg:#111219;--panel:#191b24;--panel2:#20222e;--ink:#e9e6df;--muted:#9a96ac;
 --line:#2b2d3a;--gold:#f2b83a;--gold-ink:#f4c85f;--torch:#e8663c;--cyan:#6fd3d8;
 --chk-a:#2a2c38;--chk-b:#22242f;}}
:root[data-theme="light"]{color-scheme:light;
 --bg:#f4f2ec;--panel:#fffdf8;--panel2:#efece3;--ink:#26242c;--muted:#6f6a7d;
 --line:#ddd8cc;--gold:#a9791a;--gold-ink:#7c580f;--torch:#c1471f;--cyan:#137a80;
 --chk-a:#d9d5cb;--chk-b:#c7c2b6;}
:root[data-theme="dark"]{color-scheme:dark;
 --bg:#111219;--panel:#191b24;--panel2:#20222e;--ink:#e9e6df;--muted:#9a96ac;
 --line:#2b2d3a;--gold:#f2b83a;--gold-ink:#f4c85f;--torch:#e8663c;--cyan:#6fd3d8;
 --chk-a:#2a2c38;--chk-b:#22242f;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
 font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;line-height:1.5}
.mono{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace}
header.top{position:sticky;top:0;z-index:10;background:color-mix(in srgb,var(--panel) 92%,transparent);
 backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:14px 20px}
header.top h1{margin:0;font-size:19px;letter-spacing:.02em;display:flex;align-items:baseline;gap:10px;flex-wrap:wrap}
header.top h1 b{color:var(--gold)}
header.top .sub{color:var(--muted);font-size:13px;font-weight:400}
.stats{display:flex;gap:18px;margin-top:6px;flex-wrap:wrap;font-size:12.5px;color:var(--muted)}
.stats b{color:var(--ink);font-family:ui-monospace,Menlo,monospace}
.legend{display:inline-flex;align-items:center;gap:6px}
.legend .dot{width:7px;height:7px;background:var(--torch);border-radius:50%}
nav{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
nav a{font-size:11.5px;text-decoration:none;color:var(--muted);border:1px solid var(--line);
 border-radius:999px;padding:3px 9px}
nav a:hover{color:var(--ink);border-color:var(--gold);background:var(--panel2)}
main{max-width:1180px;margin:0 auto;padding:8px 20px 80px}
section{margin-top:30px;scroll-margin-top:120px}
.sec{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;
 border-bottom:1px solid var(--line);padding-bottom:12px;margin-bottom:16px}
.eyebrow{margin:0 0 3px;font-family:ui-monospace,Menlo,monospace;font-size:11px;
 letter-spacing:.12em;text-transform:uppercase;color:var(--gold)}
.sec h2{margin:0 0 6px;font-size:22px;letter-spacing:-.01em;text-wrap:balance}
.blurb{margin:0 0 5px;max-width:62ch;color:var(--muted);font-size:14px}
.use-lead{margin:0;max-width:62ch;font-size:13px;color:var(--cyan)}
.count{flex:none;text-align:right;font-family:ui-monospace,Menlo,monospace;font-size:26px;color:var(--ink);line-height:1}
.count span{display:block;font-size:10px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.card{margin:0;background:var(--panel);border:1px solid var(--line);border-radius:8px;
 overflow:hidden;display:flex;flex-direction:column}
.card.q .coord::after{content:"?";margin-left:5px;color:var(--torch);font-weight:700}
.thumb{aspect-ratio:1/1;display:flex;align-items:center;justify-content:center;
 background-image:conic-gradient(var(--chk-a) 25%,var(--chk-b) 0 50%,var(--chk-a) 0 75%,var(--chk-b) 0);
 background-size:16px 16px}
.thumb img{width:76%;height:76%;object-fit:contain;image-rendering:pixelated}
figcaption{padding:8px 9px 10px;border-top:1px solid var(--line);display:flex;flex-direction:column;gap:3px}
.coord{font-family:ui-monospace,Menlo,monospace;font-size:11px;color:var(--gold-ink);letter-spacing:.02em}
.name{font-size:13px;font-weight:600;line-height:1.3}
.use{font-size:11.5px;color:var(--muted);line-height:1.35}
.fontwrap{background-image:conic-gradient(var(--chk-a) 25%,var(--chk-b) 0 50%,var(--chk-a) 0 75%,var(--chk-b) 0);
 background-size:16px 16px;border:1px solid var(--line);border-radius:8px;padding:16px;display:flex;justify-content:center}
.fontwrap img{width:min(512px,100%);image-rendering:pixelated}
code{font-family:ui-monospace,Menlo,monospace;font-size:.9em;background:var(--panel2);padding:1px 4px;border-radius:4px}
footer{max-width:1180px;margin:0 auto;padding:0 20px 60px;color:var(--muted);font-size:12.5px}
a.src{color:var(--cyan)}
"""
    html = f"""<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Roguemote — sprite catalogue</title>
<style>{css}</style>
<header class="top">
 <h1>Roguemote sprite catalogue <span class="sub">every tile, my read, and how we'd use it</span></h1>
 <div class="stats">
   <span>Source: <b>Simple Roguelike Tileset</b> v0.16 · Ink_Slime · CC0</span>
   <span><b>{total}</b> sprites catalogued</span>
   <span><b>256</b> font glyphs</span>
   <span class="legend"><span class="dot"></span> <span class="mono" style="color:var(--torch)">?</span> after a coord = family-level guess, correct me</span>
 </div>
 <nav>{''.join(nav)}</nav>
</header>
<main>
 <p style="color:var(--muted);max-width:70ch;margin:18px 0 0;font-size:14px">
  Each card shows the sprite on a transparency checkerboard, its source
  <span class="mono" style="color:var(--gold-ink)">col,row</span> in the 64&times;64 grid, my best-guess
  interpretation, and an in-game use idea. A <b style="color:var(--torch)">?</b> after the coordinate
  means the label is a family-level guess I'm not certain of &mdash; most of the dense enemy, item and
  trinket families are grouped rather than named per-tile. Tell me which reads are wrong (or which
  families you want split out) and I'll fix the names, regroup the sub-sheets, and adjust the extractor.</p>
 {''.join(body)}
</main>
<footer>
 Generated from <span class="mono">games/roguemote/authoring/source_tileset.png</span> ·
 sub-sheets in <span class="mono">assets/sheets</span>, rulesets in <span class="mono">tilesets/</span>.
 8&times;8 tiles shown at 76% of a 150px cell (pixel-perfect, no smoothing).
</footer>
"""
    out = "/tmp/roguemote_sprites.html"
    open(out, "w").write(html)
    print(f"wrote {out}: {total} sprites, {totq} flagged-uncertain, {len(SECTIONS)} sections")

if __name__ == "__main__":
    build()
