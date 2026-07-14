#!/usr/bin/env python3
"""TerraMote CHARACTER / ENEMY / ITEM art generator.

Writes editable PNGs under assets/ (+ .anims clip sources under anims/):
  player.png + anims/player.anims  — 16x24 cells, RESERVED palette slots so the
                                     game recolours skin/hair/shirt/pants at
                                     runtime (character builder)
  hair.png                          — 6 hair styles, 16x14 cells (same slots)
  slime.png / slime_blue.png / slime_lava.png — 16x12 x3 frames (identical
                                     layout, whole-palette variants)
  zombie.png, skeleton.png          — 16x24 walk cycles (pose renderer)
  eye.png (demon eye 16x16 x2), bat.png (16x10 x2), eoc.png (40x40 x4)
  items.png                         — 12x12 icon PER ITEM ID (see items.c)
  ui.png                            — hearts, slot frames, cursor, bubbles
Bake: `mote bake games/terramote` (Studio Save does the same).
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

# ---- RESERVED player palette slots (the game rewrites these at runtime) ----
OUTLINE  = (26, 20, 26)
SKIN     = (232, 190, 150); SKIN_SH  = (188, 140, 104)
HAIR     = (140, 88, 40);   HAIR_SH  = (94, 58, 28)
SHIRT    = (196, 64, 60);   SHIRT_SH = (140, 40, 40)
PANTS    = (64, 84, 180);   PANTS_SH = (40, 56, 128)
BOOTS    = (72, 48, 32)
EYE_W    = (240, 240, 248); EYE_P    = (40, 60, 148)

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3)

class Canvas:
    def __init__(self, w, h):
        self.im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    def px(self, x, y, c):
        if 0 <= x < self.im.width and 0 <= y < self.im.height and c is not None:
            s = snap(c)
            self.im.putpixel((x, y), (s[0], s[1], s[2], 255))
    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)
    def outline_all(self, c=OUTLINE):
        """1px dark outline around every opaque region."""
        w, h = self.im.size
        src = self.im.copy()
        for y in range(h):
            for x in range(w):
                if src.getpixel((x, y))[3]:
                    continue
                near = False
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < w and 0 <= yy < h and src.getpixel((xx, yy))[3]:
                        near = True; break
                if near:
                    s = snap(c)
                    self.im.putpixel((x, y), (s[0], s[1], s[2], 255))

# ------------------------------------------------------------------ player ----
# 16x24 cell; facing RIGHT; feet baseline y=23.
# frames: 0 idle · 1-6 walk · 7 jump · 8-10 swing (arm back->up->fwd)
CW, CH = 16, 24
HEAD_X, HEAD_Y = 5, 3          # head box 7 wide, 7 tall (x5..11,y3..9)

def draw_humanoid(c: Canvas, ox, skin, skin_sh, shirt, shirt_sh, pants, pants_sh,
                  boots, leg_pose=0, arm="side", eye=True, ragged=False):
    """One pose into canvas at x-offset ox. leg_pose: 0 stand, 1..4 walk,
    5 jump. arm: side|fwd|up|back|zombie."""
    X = lambda x: ox + x
    # --- legs (pants) : two 2px-wide legs, hip y=17, feet y=22 ---
    hip = 17
    poses = {
        0: ((7, 0), (9, 0)),           # x of back/front foot, spread
        1: ((5, 0), (10, 0)),
        2: ((6, -1), (9, 0)),
        3: ((8, 0), (8, 0)),
        4: ((10, 0), (6, -1)),
        5: ((6, -1), (10, -2)),        # jump: tucked
    }
    (bx, blift), (fx, flift) = poses.get(leg_pose, poses[0])
    for (lx, lift, sh) in ((bx, blift, True), (fx, flift, False)):
        col = pants_sh if sh else pants
        top = hip
        bot = 22 + lift
        # slanted leg from hip x=8 to foot x=lx
        for y in range(top, bot + 1):
            t = (y - top) / max(1, bot - top)
            x = int(8 + (lx - 8) * t + 0.5)
            c.px(X(x), y, col); c.px(X(x + 1), y, col)
        c.px(X(lx), bot + 1, boots); c.px(X(lx + 1), bot + 1, boots)
        c.px(X(lx + 2 if not sh else lx + 1), bot + 1, boots)
    # --- torso (shirt): y 11..16 ---
    c.rect(X(6), 11, X(10), 16, shirt)
    c.rect(X(6), 14, X(7), 16, shirt_sh)
    if ragged:
        for x, y in ((6, 16), (9, 15), (10, 13)):
            c.px(X(x), y, None)
    # --- arms ---
    if arm == "side":
        c.rect(X(9), 11, X(10), 15, shirt_sh)
        c.px(X(9), 16, skin); c.px(X(10), 16, skin)
    elif arm == "fwd":
        c.rect(X(10), 12, X(13), 13, shirt_sh)
        c.px(X(13), 13, skin); c.px(X(14), 13, skin)
    elif arm == "up":
        c.rect(X(10), 6, X(11), 11, shirt_sh)
        c.px(X(11), 5, skin); c.px(X(11), 4, skin)
    elif arm == "back":
        c.rect(X(4), 11, X(5), 14, shirt_sh)
        c.px(X(4), 15, skin)
    elif arm == "zombie":
        c.rect(X(10), 12, X(14), 12, skin)
        c.rect(X(10), 13, X(13), 13, skin_sh)
    # --- head: skin block + jaw shade ---
    c.rect(X(HEAD_X), HEAD_Y, X(HEAD_X + 6), HEAD_Y + 6, skin)
    c.rect(X(HEAD_X), HEAD_Y + 5, X(HEAD_X + 1), HEAD_Y + 6, skin_sh)
    if eye:
        c.px(X(HEAD_X + 4), HEAD_Y + 3, EYE_W)
        c.px(X(HEAD_X + 5), HEAD_Y + 3, EYE_P)

def make_player():
    frames = [
        (0, "side"), (1, "side"), (2, "side"), (3, "side"), (4, "side"), (2, "side"), (3, "side"),
        (5, "side"),
        (0, "back"), (0, "up"), (0, "fwd"),
    ]  # 0 idle, 1-6 walk, 7 jump, 8-10 swing
    c = Canvas(CW * len(frames), CH)
    for i, (leg, arm) in enumerate(frames):
        draw_humanoid(c, i * CW, SKIN, SKIN_SH, SHIRT, SHIRT_SH, PANTS, PANTS_SH, BOOTS,
                      leg_pose=leg, arm=arm)
    c.outline_all()
    c.im.save(os.path.join(HERE, "player.png"))
    os.makedirs(os.path.join(GAME, "anims"), exist_ok=True)
    with open(os.path.join(GAME, "anims", "player.anims"), "w") as f:
        f.write("sheet assets/player.png\ntile %d %d\nclips 4\n" % (CW, CH))
        f.write("clip idle 1 2 0 0 1\nf 0 500 -\n")
        f.write("clip walk 1 10 0 0 6\n")
        for i in range(1, 7): f.write("f %d 90 -\n" % i)
        f.write("clip jump 1 8 0 0 1\nf 7 200 -\n")
        f.write("clip swing 0 12 0 0 3\nf 8 70 -\nf 9 70 -\nf 10 90 -\n")
    print("[sprites] player.png (%d frames) + player.anims" % len(frames))

def make_hair():
    """6 styles, cells 16x14, aligned to the body cell's head box."""
    styles = 6
    c = Canvas(CW * styles, 14)
    HX, HY = HEAD_X, HEAD_Y
    for s in range(styles):
        ox = s * CW
        X = lambda x: ox + x
        if s == 0:      # short spiky
            c.rect(X(HX - 1), HY - 1, X(HX + 6), HY + 1, HAIR)
            for x in (HX - 1, HX + 1, HX + 3, HX + 5):
                c.px(X(x), HY - 2, HAIR)
            c.rect(X(HX - 1), HY + 2, X(HX), HY + 3, HAIR_SH)
        elif s == 1:    # long flowing (down the back)
            c.rect(X(HX - 1), HY - 1, X(HX + 6), HY + 1, HAIR)
            c.rect(X(HX - 2), HY, X(HX - 1), HY + 9, HAIR)
            c.rect(X(HX - 1), HY + 2, X(HX), HY + 7, HAIR_SH)
            c.px(X(HX + 6), HY + 2, HAIR_SH)
        elif s == 2:    # ponytail
            c.rect(X(HX - 1), HY - 1, X(HX + 6), HY + 1, HAIR)
            c.rect(X(HX - 3), HY + 1, X(HX - 2), HY + 6, HAIR)
            c.px(X(HX - 3), HY + 7, HAIR_SH)
            c.px(X(HX - 1), HY + 2, HAIR_SH)
        elif s == 3:    # bowl cut
            c.rect(X(HX - 1), HY - 1, X(HX + 6), HY + 2, HAIR)
            c.rect(X(HX - 1), HY + 3, X(HX), HY + 4, HAIR)
            c.rect(X(HX + 6), HY + 3, X(HX + 6), HY + 4, HAIR_SH)
        elif s == 4:    # mohawk
            c.rect(X(HX + 1), HY - 3, X(HX + 4), HY - 1, HAIR)
            c.px(X(HX + 2), HY - 4, HAIR_SH); c.px(X(HX + 3), HY - 4, HAIR_SH)
        elif s == 5:    # buzz
            c.rect(X(HX), HY - 1, X(HX + 5), HY, HAIR_SH)
        # tiny outline pass per style area is skipped: hair sits over the head
    c.im.save(os.path.join(HERE, "hair.png"))
    with open(os.path.join(HERE, "hair.sheet"), "w") as f:
        f.write("cell %d 14\n" % CW)
    print("[sprites] hair.png (%d styles)" % styles)

# ------------------------------------------------------------------ enemies ----
def make_slime(name, body, body_hi, body_dk):
    """3 frames: rest / squash / stretch. 16x12 cells."""
    c = Canvas(16 * 3, 12)
    shapes = [
        (2, 13, 4, 11),   # x0,x1,ytop,ybot: rest
        (1, 14, 6, 11),   # squash
        (3, 12, 2, 11),   # stretch (airborne)
    ]
    for i, (x0, x1, yt, yb) in enumerate(shapes):
        ox = i * 16
        for y in range(yt, yb + 1):
            t = (y - yt) / max(1, yb - yt)
            inset = int((1.0 - t) * 2.2) if y < yb else 0
            for x in range(x0 + inset, x1 - inset + 1):
                c.px(ox + x, y, body)
        # inner blob + shine
        c.px(ox + (x0 + x1) // 2 - 1, yt + 2, body_hi)
        c.px(ox + (x0 + x1) // 2, yt + 2, body_hi)
        c.rect(ox + x0 + 2, yb - 1, ox + x1 - 2, yb, body_dk)
        # eyes
        ey = yt + (3 if i != 1 else 2)
        c.px(ox + x1 - 4, ey, OUTLINE); c.px(ox + x1 - 2, ey, OUTLINE)
    c.outline_all(body_dk)
    c.im.save(os.path.join(HERE, name + ".png"))
    with open(os.path.join(HERE, name + ".sheet"), "w") as f:
        f.write("cell 16 12\n")
    print("[sprites]", name + ".png")

def make_zombie():
    ZSKIN = (110, 150, 90); ZSKIN_SH = (80, 112, 66)
    ZSHIRT = (120, 90, 60); ZSHIRT_SH = (88, 64, 42)
    ZPANTS = (70, 62, 50); ZPANTS_SH = (50, 44, 36)
    c = Canvas(CW * 4, CH)
    for i, leg in enumerate((1, 2, 3, 4)):
        draw_humanoid(c, i * CW, ZSKIN, ZSKIN_SH, ZSHIRT, ZSHIRT_SH, ZPANTS, ZPANTS_SH,
                      (40, 36, 30), leg_pose=leg, arm="zombie", eye=False, ragged=True)
        c.px(i * CW + HEAD_X + 4, HEAD_Y + 3, (220, 40, 40))   # red eye
    c.outline_all()
    small = c.im.resize((12 * 4, 16), Image.NEAREST)
    small.save(os.path.join(HERE, "zombie.png"))
    with open(os.path.join(HERE, "zombie.sheet"), "w") as f:
        f.write("cell 12 16\n")
    print("[sprites] zombie.png (12x16)")

def make_skeleton():
    BONE = (222, 218, 200); BONE_SH = (168, 162, 142)
    c = Canvas(CW * 4, CH)
    for i, leg in enumerate((1, 2, 3, 4)):
        ox = i * CW
        draw_humanoid(c, ox, BONE, BONE_SH, BONE, BONE_SH, BONE, BONE_SH,
                      BONE_SH, leg_pose=leg, arm="side", eye=False)
        # rib lines + eye socket
        for y in (12, 14, 16):
            for x in range(6, 11, 2):
                c.px(ox + x, y, BONE_SH)
        c.px(ox + HEAD_X + 4, HEAD_Y + 3, OUTLINE)
        c.px(ox + HEAD_X + 2, HEAD_Y + 3, OUTLINE)
    c.outline_all()
    c.im.save(os.path.join(HERE, "skeleton.png"))
    with open(os.path.join(HERE, "skeleton.sheet"), "w") as f:
        f.write("cell %d %d\n" % (CW, CH))
    print("[sprites] skeleton.png")

def make_eye():
    """demon eye 16x16, 2 wing frames."""
    W = (232, 228, 224); WS = (180, 172, 170); IRIS = (190, 40, 40)
    VEIN = (200, 120, 120); WING = (90, 60, 90)
    c = Canvas(16 * 2, 16)
    for i in range(2):
        ox = i * 16
        # eyeball
        for y in range(3, 13):
            for x in range(3, 13):
                dx, dy = x - 7.5, y - 7.5
                if dx * dx + dy * dy <= 22:
                    c.px(ox + x, y, W)
        c.rect(ox + 4, 10, ox + 11, 12, WS)
        # iris facing left (flies at the player)
        c.rect(ox + 3, 6, ox + 5, 9, IRIS)
        c.px(ox + 3, 7, (255, 120, 120))
        c.px(ox + 9, 5, VEIN); c.px(ox + 10, 8, VEIN); c.px(ox + 8, 10, VEIN)
        # wings flap
        if i == 0:
            c.rect(ox + 12, 2, ox + 14, 6, WING)
            c.px(ox + 13, 1, WING)
        else:
            c.rect(ox + 12, 8, ox + 14, 12, WING)
    c.outline_all()
    c.im.save(os.path.join(HERE, "eye.png"))
    with open(os.path.join(HERE, "eye.sheet"), "w") as f:
        f.write("cell 16 16\n")
    print("[sprites] eye.png")

def make_bat():
    BODY = (96, 74, 110); WING = (70, 50, 84); EYE = (240, 90, 60)
    c = Canvas(16 * 2, 10)
    for i in range(2):
        ox = i * 16
        c.rect(ox + 6, 4, ox + 9, 7, BODY)
        c.px(ox + 5, 3, BODY); c.px(ox + 10, 3, BODY)     # ears
        c.px(ox + 6, 5, EYE)
        if i == 0:
            c.rect(ox + 1, 2, ox + 5, 4, WING); c.rect(ox + 10, 2, ox + 14, 4, WING)
        else:
            c.rect(ox + 2, 6, ox + 5, 8, WING); c.rect(ox + 10, 6, ox + 13, 8, WING)
    c.outline_all()
    c.im.save(os.path.join(HERE, "bat.png"))
    with open(os.path.join(HERE, "bat.sheet"), "w") as f:
        f.write("cell 16 10\n")
    print("[sprites] bat.png")

def make_eoc():
    """Eye of Cthulhu 40x40: frames 0,1 = phase 1 (iris), 2,3 = phase 2 (mouth)."""
    W = (226, 220, 214); WS = (178, 168, 164); IRIS = (200, 40, 40)
    PUPIL = (60, 10, 10); VEIN = (196, 110, 110); TEND = (110, 60, 60)
    TEETH = (240, 236, 220); MAW = (120, 24, 24)
    c = Canvas(40 * 4, 40)
    for f in range(4):
        ox = f * 40
        cx, cy, R = 20, 20, 15
        for y in range(40):
            for x in range(40):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy <= R * R:
                    c.px(ox + x, y, W)
        # shading bottom
        for y in range(26, 36):
            for x in range(8, 33):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy <= R * R and dx * dx + dy * dy > (R - 4) * (R - 4):
                    c.px(ox + x, y, WS)
        # tendrils on the back (right side; sprite faces left)
        for k, (ty, tl) in enumerate(((8, 5), (14, 7), (22, 7), (28, 5))):
            wig = 1 if (f + k) % 2 == 0 else -1
            for t in range(tl):
                c.px(ox + 33 + t, ty + (t * wig) // 2, TEND)
                c.px(ox + 33 + t, ty + 1 + (t * wig) // 2, TEND)
        if f < 2:
            # phase 1: big iris + pupil, slight jitter between frames
            j = 0 if f == 0 else 1
            for y in range(-5, 6):
                for x in range(-5, 6):
                    if x * x + y * y <= 26:
                        c.px(ox + 10 + x + j, 20 + y, IRIS)
            for y in range(-2, 3):
                for x in range(-2, 3):
                    if x * x + y * y <= 5:
                        c.px(ox + 8 + x + j, 20 + y, PUPIL)
            c.px(ox + 12, 14, VEIN); c.px(ox + 16, 10, VEIN); c.px(ox + 15, 28, VEIN)
        else:
            # phase 2: gaping maw with teeth
            j = 0 if f == 2 else 1
            for y in range(-7, 8):
                x0 = -9 + abs(y) // 2
                for x in range(x0, 2):
                    c.px(ox + 12 + x + j, 20 + y, MAW)
            for k in range(-6, 7, 3):
                c.px(ox + 12 + (-8 + abs(k) // 2) + j, 20 + k, TEETH)
                c.px(ox + 12 + (-7 + abs(k) // 2) + j, 20 + k, TEETH)
            c.px(ox + 18, 8, VEIN); c.px(ox + 20, 32, VEIN)
    c.outline_all()
    c.im.save(os.path.join(HERE, "eoc.png"))
    with open(os.path.join(HERE, "eoc.sheet"), "w") as f:
        f.write("cell 40 40\n")
    print("[sprites] eoc.png")

# ------------------------------------------------------------------ items ----
def make_items():
    import importlib.util
    # item id order must match src/terra.h — keep this list in sync
    IDS = """NONE DIRT STONE WOOD SAND SNOW EBON CLAY ASH HELLSTONE OBSIDIAN TORCH
    PLATFORM WORKBENCH FURNACE ANVIL CHEST DOOR ACORN GEL LENS MUSHROOM COIN
    COPPER_ORE IRON_ORE GOLD_ORE DEMONITE_ORE COPPER_BAR IRON_BAR GOLD_BAR
    DEMONITE_BAR HELL_BAR PICK_WOOD PICK_COPPER PICK_IRON PICK_GOLD PICK_NIGHTMARE
    AXE_WOOD AXE_IRON SWORD_WOOD SWORD_COPPER SWORD_IRON SWORD_GOLD SWORD_BANE
    SWORD_VOLCANO BOW_WOOD BOW_GOLD BOW_MOLTEN ARROW ARROW_FLAME HELM_COPPER
    MAIL_COPPER LEGS_COPPER HELM_IRON MAIL_IRON LEGS_IRON HELM_GOLD MAIL_GOLD
    LEGS_GOLD HELM_MOLTEN MAIL_MOLTEN LEGS_MOLTEN POTION_HEAL SUSPICIOUS_EYE LIFE_CRYSTAL GRAPPLE
    TABLE CHAIR LANTERN FIREPLACE CHAIN""".split()
    CS = 12          # procedural painters draw at 12px...
    OUT = 16         # ...but the sheet grid is 16px cells (weapon art needs it)
    cols = 8
    rows = (len(IDS) + cols - 1) // cols
    c = Canvas(cols * CS, rows * CS)

    BLOCK_COL = {
        "DIRT": (128, 84, 50), "STONE": (116, 116, 124), "WOOD": (168, 122, 68),
        "SAND": (212, 192, 116), "SNOW": (222, 232, 244), "EBON": (104, 88, 128),
        "CLAY": (172, 106, 74), "ASH": (74, 66, 66), "HELLSTONE": (96, 42, 36),
        "OBSIDIAN": (46, 38, 66),
    }
    METAL = { "WOOD": (150, 108, 60), "COPPER": (198, 112, 42), "IRON": (176, 176, 186),
              "GOLD": (236, 196, 44), "NIGHTMARE": (120, 70, 190), "BANE": (120, 70, 190),
              "VOLCANO": (240, 110, 30), "MOLTEN": (240, 110, 30), "DEMONITE": (120, 70, 190),
              "HELL": (240, 110, 30) }

    def cell_origin(i):
        return (i % cols) * CS, (i // cols) * CS

    def block_icon(ox, oy, col):
        for y in range(2, 10):
            for x in range(2, 10):
                v = ((x * 7 + y * 13) % 5)
                cc = col if v > 1 else tuple(int(k * 0.8) for k in col)
                c.px(ox + x, oy + y, cc)
        for x in range(2, 10):
            c.px(ox + x, oy + 2, tuple(min(255, int(k * 1.25)) for k in col))
            c.px(ox + x, oy + 9, tuple(int(k * 0.6) for k in col))
        for y in range(2, 10):
            c.px(ox + 2, oy + y, tuple(int(k * 0.75) for k in col))
            c.px(ox + 9, oy + y, tuple(int(k * 0.75) for k in col))

    def pick_icon(ox, oy, m):
        hcol = (150, 108, 60)
        for i in range(6):
            c.px(ox + 3 + i, oy + 8 - i, hcol)      # handle
        for i in range(7):                          # curved head
            c.px(ox + 1 + i, oy + 2 + (0 if 1 < i < 5 else 1), m)
            if 1 < i < 5: c.px(ox + 1 + i, oy + 3, tuple(int(k*0.75) for k in m))
        c.px(ox + 1, oy + 4, m); c.px(ox + 7, oy + 4, m)

    def axe_icon(ox, oy, m):
        hcol = (150, 108, 60)
        for i in range(6):
            c.px(ox + 3 + i, oy + 9 - i, hcol)
        for y in range(2, 7):
            for x in range(2, 5):
                c.px(ox + x, oy + y, m if x > 2 else tuple(int(k*0.75) for k in m))

    def sword_icon(ox, oy, m):
        for i in range(6):
            c.px(ox + 8 - i, oy + 2 + i, m)
            c.px(ox + 9 - i, oy + 2 + i, tuple(min(255, int(k * 1.3)) for k in m))
        c.px(ox + 3, oy + 8, (150, 108, 60)); c.px(ox + 2, oy + 9, (150, 108, 60))
        c.px(ox + 2, oy + 7, (110, 80, 45)); c.px(ox + 4, oy + 9, (110, 80, 45))

    def bow_icon(ox, oy, m):
        for y in range(2, 10):
            x = 3 + (2 if 3 < y < 8 else (1 if y in (3, 8) else 0))
            c.px(ox + x, oy + y, m)
        for y in range(2, 10):
            c.px(ox + 3, oy + y, (220, 220, 226)) if False else None
        c.px(ox + 3, oy + 2, m); c.px(ox + 3, oy + 9, m)
        for y in range(3, 9):
            c.px(ox + 8, oy + y, (200, 200, 208))   # string

    def arrow_icon(ox, oy, flame=False):
        for i in range(6):
            c.px(ox + 5, oy + 3 + i, (150, 108, 60))
        tip = (240, 110, 30) if flame else (180, 180, 190)
        c.px(ox + 5, oy + 2, tip); c.px(ox + 4, oy + 3, tip); c.px(ox + 6, oy + 3, tip)
        c.px(ox + 4, oy + 9, (200, 200, 210)); c.px(ox + 6, oy + 9, (200, 200, 210))

    def bar_icon(ox, oy, m):
        for y in range(6, 9):
            for x in range(2, 10):
                c.px(ox + x, oy + y, m)
        for x in range(3, 11):
            c.px(ox + x, oy + 5, tuple(min(255, int(k * 1.25)) for k in m))
        c.px(ox + 2, oy + 6, tuple(min(255, int(k*1.25)) for k in m))

    def ore_icon(ox, oy, m):
        pts = ((4, 4), (6, 3), (7, 5), (5, 6), (3, 6), (6, 7))
        for x, y in pts:
            c.px(ox + x, oy + y, m)
            c.px(ox + x + 1, oy + y, tuple(int(k * 0.75) for k in m))
        c.px(ox + 5, oy + 5, tuple(min(255, int(k * 1.3)) for k in m))

    def helm_icon(ox, oy, m):
        for y in range(3, 8):
            for x in range(3, 9):
                if y == 3 and x in (3, 8): continue
                c.px(ox + x, oy + y, m)
        c.rect(ox + 4, oy + 6, ox + 7, oy + 6, tuple(int(k * 0.7) for k in m))

    def mail_icon(ox, oy, m):
        c.rect(ox + 3, oy + 3, ox + 8, oy + 8, m)
        c.rect(ox + 2, oy + 3, ox + 2, oy + 5, tuple(int(k*0.8) for k in m))
        c.rect(ox + 9, oy + 3, ox + 9, oy + 5, tuple(int(k*0.8) for k in m))
        c.rect(ox + 5, oy + 4, ox + 6, oy + 7, tuple(int(k * 0.7) for k in m))

    def legs_icon(ox, oy, m):
        c.rect(ox + 3, oy + 3, ox + 8, oy + 4, m)
        c.rect(ox + 3, oy + 5, ox + 4, oy + 9, m)
        c.rect(ox + 7, oy + 5, ox + 8, oy + 9, tuple(int(k * 0.8) for k in m))

    for i, name in enumerate(IDS):
        ox, oy = cell_origin(i)
        if name == "NONE": continue
        if name in BLOCK_COL: block_icon(ox, oy, BLOCK_COL[name]); continue
        if name == "TORCH":
            for y in range(4, 10): c.px(ox + 5, oy + y, (150, 108, 60)); c.px(ox + 6, oy + y, (120, 84, 46))
            c.px(ox + 5, oy + 3, (255, 200, 60)); c.px(ox + 6, oy + 3, (255, 160, 30))
            c.px(ox + 5, oy + 2, (255, 240, 160)); continue
        if name == "PLATFORM":
            c.rect(ox + 1, oy + 5, ox + 10, oy + 6, (172, 126, 70))
            for x in (3, 6, 9): c.px(ox + x, oy + 6, (104, 72, 38))
            continue
        if name == "WORKBENCH":
            c.rect(ox + 1, oy + 4, ox + 10, oy + 5, (176, 128, 72))
            c.rect(ox + 2, oy + 6, ox + 3, oy + 9, (128, 90, 48))
            c.rect(ox + 8, oy + 6, ox + 9, oy + 9, (128, 90, 48)); continue
        if name == "FURNACE":
            c.rect(ox + 2, oy + 3, ox + 9, oy + 9, (104, 100, 100))
            c.rect(ox + 4, oy + 6, ox + 7, oy + 8, (40, 36, 36))
            c.rect(ox + 5, oy + 7, ox + 6, oy + 8, (255, 150, 30)); continue
        if name == "ANVIL":
            c.rect(ox + 2, oy + 4, ox + 9, oy + 5, (152, 156, 168))
            c.rect(ox + 4, oy + 6, ox + 7, oy + 8, (110, 114, 126))
            c.rect(ox + 3, oy + 9, ox + 8, oy + 9, (72, 76, 88)); continue
        if name == "CHEST":
            c.rect(ox + 2, oy + 4, ox + 9, oy + 9, (178, 126, 64))
            c.rect(ox + 2, oy + 6, ox + 9, oy + 6, (240, 200, 70))
            c.px(ox + 5, oy + 7, (240, 200, 70)); c.px(ox + 6, oy + 7, (240, 200, 70)); continue
        if name == "DOOR":
            c.rect(ox + 3, oy + 2, ox + 8, oy + 10, (170, 122, 66))
            c.px(ox + 7, oy + 6, (240, 200, 80)); continue
        if name == "ACORN":
            c.rect(ox + 4, oy + 5, ox + 7, oy + 8, (150, 108, 60))
            c.rect(ox + 4, oy + 4, ox + 7, oy + 4, (94, 64, 38))
            c.px(ox + 5, oy + 3, (94, 64, 38)); c.px(ox + 5, oy + 9, (110, 80, 45)); continue
        if name == "GEL":
            for y in range(5, 10):
                for x in range(3, 9):
                    if (x + y) % 7: c.px(ox + x, oy + y, (90, 200, 110))
            c.px(ox + 4, oy + 6, (160, 240, 170)); continue
        if name == "LENS":
            for y in range(3, 9):
                for x in range(4, 8):
                    dx, dy = x - 5.5, y - 5.5
                    if dx*dx + dy*dy < 6: c.px(ox + x, oy + y, (220, 224, 234))
            c.px(ox + 5, oy + 5, (120, 140, 220)); continue
        if name == "MUSHROOM":
            c.rect(ox + 3, oy + 4, ox + 8, oy + 5, (94, 220, 255))
            c.rect(ox + 4, oy + 3, ox + 7, oy + 3, (52, 150, 210))
            c.rect(ox + 5, oy + 6, ox + 6, oy + 9, (200, 214, 230)); continue
        if name == "COIN":
            for y in range(3, 9):
                for x in range(3, 9):
                    dx, dy = x - 5.5, y - 5.5
                    if dx*dx + dy*dy < 7: c.px(ox + x, oy + y, (236, 196, 44))
            c.px(ox + 5, oy + 5, (255, 240, 140)); c.px(ox + 6, oy + 6, (190, 150, 30)); continue
        if name == "POTION_HEAL":
            c.rect(ox + 5, oy + 2, ox + 6, oy + 3, (180, 190, 200))
            c.rect(ox + 4, oy + 4, ox + 7, oy + 9, (220, 60, 70))
            c.px(ox + 4, oy + 4, (250, 130, 140)); continue
        if name == "LIFE_CRYSTAL":
            for y in range(3, 9):
                w = 3 - abs(y - 6) if y > 5 else y - 2
                for x in range(6 - w, 6 + w):
                    c.px(ox + x, oy + y, (235, 60, 90) if (x + y) % 3 else (255, 130, 150))
            c.px(ox + 5, oy + 4, (255, 190, 200)); continue
        if name == "SUSPICIOUS_EYE":
            for y in range(3, 9):
                for x in range(3, 9):
                    dx, dy = x - 5.5, y - 5.5
                    if dx*dx + dy*dy < 8: c.px(ox + x, oy + y, (226, 220, 214))
            c.rect(ox + 4, oy + 5, ox + 5, oy + 6, (200, 40, 40)); continue
        if name == "TABLE":
            c.rect(ox + 1, oy + 4, ox + 10, oy + 5, (176, 128, 72))       # top
            c.rect(ox + 2, oy + 6, ox + 2, oy + 9, (128, 90, 48))         # legs
            c.rect(ox + 9, oy + 6, ox + 9, oy + 9, (128, 90, 48)); continue
        if name == "CHAIR":
            c.rect(ox + 3, oy + 2, ox + 4, oy + 8, (176, 128, 72))        # backrest
            c.rect(ox + 3, oy + 6, ox + 8, oy + 7, (176, 128, 72))        # seat
            c.rect(ox + 7, oy + 8, ox + 8, oy + 9, (128, 90, 48)); continue
        if name == "LANTERN":
            c.rect(ox + 4, oy + 2, ox + 7, oy + 2, (128, 90, 48))         # cap
            c.rect(ox + 4, oy + 3, ox + 7, oy + 8, (255, 200, 70))        # glass/glow
            c.px(ox + 5, oy + 5, (255, 250, 200)); c.px(ox + 6, oy + 6, (255, 160, 40))
            c.rect(ox + 4, oy + 9, ox + 7, oy + 9, (128, 90, 48)); continue
        if name == "FIREPLACE":
            c.rect(ox + 1, oy + 2, ox + 10, oy + 3, (150, 92, 60))        # mantle
            c.rect(ox + 2, oy + 4, ox + 9, oy + 9, (80, 80, 90))          # stone
            c.rect(ox + 4, oy + 6, ox + 7, oy + 9, (40, 36, 36))          # hearth
            c.rect(ox + 5, oy + 7, ox + 6, oy + 9, (255, 150, 30)); continue
        if name == "CHAIN":
            for y in range(2, 10, 2):
                c.px(ox + 5, oy + y, (188, 190, 198)); c.px(ox + 6, oy + y, (120, 122, 132))
                c.px(ox + 5, oy + y + 1, (120, 122, 132)); c.px(ox + 6, oy + y + 1, (188, 190, 198))
            continue
        if name == "GRAPPLE":
            IRON = (188, 190, 198); DK = (120, 122, 132); ROPE = (150, 108, 60)
            for i in range(4):                       # rope trailing down-left
                c.px(ox + 3 + i, oy + 9 - i, ROPE)
            c.px(ox + 6, oy + 5, IRON); c.px(ox + 6, oy + 4, IRON)  # shank
            for dx, dy in ((5, 3), (7, 3), (4, 4), (8, 4)):         # three curved claws
                c.px(ox + dx, oy + dy, IRON)
            c.px(ox + 4, oy + 3, DK); c.px(ox + 8, oy + 3, DK)
            c.px(ox + 6, oy + 2, IRON); continue
        parts = name.split("_")
        if parts[0] == "PICK": pick_icon(ox, oy, METAL[parts[1]]); continue
        if parts[0] == "AXE": axe_icon(ox, oy, METAL[parts[1]]); continue
        if parts[0] == "SWORD": sword_icon(ox, oy, METAL[parts[1]]); continue
        if parts[0] == "BOW": bow_icon(ox, oy, METAL[parts[1]]); continue
        if parts[0] == "ARROW": arrow_icon(ox, oy, len(parts) > 1); continue
        if parts[1] == "ORE": ore_icon(ox, oy, METAL[parts[0]]); continue
        if parts[1] == "BAR": bar_icon(ox, oy, METAL[parts[0]]); continue
        if parts[0] == "HELM": helm_icon(ox, oy, METAL[parts[1]]); continue
        if parts[0] == "MAIL": mail_icon(ox, oy, METAL[parts[1]]); continue
        if parts[0] == "LEGS": legs_icon(ox, oy, METAL[parts[1]]); continue
    out = Image.new("RGBA", (cols * OUT, rows * OUT), (0, 0, 0, 0))
    for i in range(len(IDS)):
        cell = c.im.crop(((i % cols) * CS, (i // cols) * CS, (i % cols + 1) * CS, (i // cols + 1) * CS))
        out.paste(cell, ((i % cols) * OUT + 2, (i // cols) * OUT + 2), cell)
    out.save(os.path.join(HERE, "items.png"))
    with open(os.path.join(HERE, "items.sheet"), "w") as f:
        f.write("cell %d %d\n" % (OUT, OUT))
    print("[sprites] items.png (%d icons, %d cols, %dpx cells)" % (len(IDS), cols, OUT))

# ------------------------------------------------------------------ ui ----
def make_ui():
    """cells 12x12: 0 heart full, 1 heart half, 2 heart empty, 3 slot frame,
    4 slot frame selected, 5 cursor/reticle, 6 bubble, 7 arrow projectile,
    8 flame arrow projectile"""
    CS = 12
    c = Canvas(CS * 9, CS)
    def heart(ox, kind):
        RED = (220, 40, 60); DARK = (120, 20, 34); GREY = (70, 66, 74)
        col = RED if kind == 0 else GREY
        pts = [(2,3),(3,2),(4,2),(5,3),(6,2),(7,2),(8,3),(8,4),(8,5),(7,6),(6,7),(5,8),(4,7),(3,6),(2,5),(2,4)]
        for x, y in pts: c.px(ox + x, y, DARK if kind == 2 else tuple(int(k*0.7) for k in col))
        for y in range(3, 8):
            for x in range(3, 8):
                inside = (y <= 5 or abs(x - 5) <= (8 - y))
                if inside and kind != 2:
                    if kind == 1 and x > 5: continue
                    c.px(ox + x, y, col)
        c.px(ox + 3, 3, (255, 150, 160) if kind != 2 else GREY)
    heart(0, 0); heart(CS, 1); heart(CS * 2, 2)
    # slot frames
    for k in range(2):
        ox = CS * (3 + k)
        col = (210, 190, 120) if k else (110, 104, 96)
        bgc = (46, 42, 40)
        for y in range(CS):
            for x in range(CS):
                edge = x in (0, CS - 1) or y in (0, CS - 1)
                c.px(ox + x, y, col if edge else bgc)
    # reticle: corner ticks
    ox = CS * 5
    TICK = (255, 250, 200)
    for k in range(3):
        c.px(ox + k, 0, TICK); c.px(ox, k, TICK)
        c.px(ox + CS - 1 - k, 0, TICK); c.px(ox + CS - 1, k, TICK)
        c.px(ox + k, CS - 1, TICK); c.px(ox, CS - 1 - k, TICK)
        c.px(ox + CS - 1 - k, CS - 1, TICK); c.px(ox + CS - 1, CS - 1 - k, TICK)
    # bubble
    ox = CS * 6
    for y in range(3, 9):
        for x in range(3, 9):
            dx, dy = x - 5.5, y - 5.5
            if 4 < dx*dx + dy*dy < 8: c.px(ox + x, y, (170, 210, 250))
    # arrows (projectiles, pointing right)
    for k in range(2):
        ox = CS * (7 + k)
        for i in range(7): c.px(ox + 2 + i, 6, (150, 108, 60))
        tip = (240, 110, 30) if k else (200, 200, 210)
        c.px(ox + 9, 6, tip); c.px(ox + 8, 5, tip); c.px(ox + 8, 7, tip)
        c.px(ox + 2, 5, (210, 210, 220)); c.px(ox + 2, 7, (210, 210, 220))
    c.im.save(os.path.join(HERE, "ui.png"))
    with open(os.path.join(HERE, "ui.sheet"), "w") as f:
        f.write("cell %d %d\n" % (CS, CS))
    print("[sprites] ui.png")


def _normalize_sheets():
    """Rewrite every assets/*.sheet to the full Studio format (a `sheet <png>`
    line is REQUIRED for the Studio Sheet tab to find the image)."""
    import glob
    for f in glob.glob(os.path.join(HERE, "*.sheet")):
        nm = os.path.splitext(os.path.basename(f))[0]
        cw = ch = mg = sp = 0
        for ln in open(f):
            t = ln.split()
            if len(t) >= 3 and t[0] == "cell": cw, ch = int(t[1]), int(t[2])
            elif len(t) >= 2 and t[0] == "margin": mg = int(t[1])
            elif len(t) >= 2 and t[0] == "spacing": sp = int(t[1])
        open(f, "w").write("sheet assets/%s.png\ncell %d %d\nmargin %d\nspacing %d\n" % (nm, cw, ch, mg, sp))


if __name__ == "__main__":
    make_player()
    make_hair()
    make_slime("slime",      (70, 200, 90),  (150, 240, 160), (40, 140, 60))
    make_slime("slime_blue", (70, 130, 230), (150, 200, 250), (40, 80, 170))
    make_slime("slime_lava", (240, 120, 40), (255, 210, 90),  (170, 60, 20))
    make_zombie()
    make_skeleton()
    make_eye()
    make_bat()
    make_eoc()
    make_items()
    make_ui()
    print("done")
    _normalize_sheets()
