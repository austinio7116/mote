#!/usr/bin/env python3
"""Author the people + pickup sprites (EDITABLE SOURCES; `mote bake` -> src/*.h).

Top-down characters face UP (image-up = heading; the game rotates the quad).
  ped.png     32x64 — 2 walk frames x 4 civilian variants, 16x16 cells
  player.png  64x16 — 4 walk frames, 16x16 cells
  pickups.png 80x16 — cash / pistol / smg / shotgun / medkit, 16x16 cells
"""
from PIL import Image, ImageDraw

S = 6                      # supersample factor

def canvas(): return Image.new("RGBA", (16*S, 16*S), (0,0,0,0))
def down(im): return im.resize((16,16), Image.LANCZOS)

def person(shirt, hair, skin, stride, jacket=None):
    """Top-down walker facing up. stride -1..1 swings arms/feet."""
    im = canvas(); d = ImageDraw.Draw(im); cx = 8*S
    arm  = shirt if jacket is None else jacket
    sw   = int(2.9*S*stride)                       # arm/foot swing in px
    # feet (under everything): stride apart, dark shoes
    d.ellipse([cx-3.2*S, 10.5*S - sw*0.5, cx-0.8*S, 13.5*S - sw*0.5], fill=(24,22,26,255))
    d.ellipse([cx+0.8*S, 10.5*S + sw*0.5, cx+3.2*S, 13.5*S + sw*0.5], fill=(24,22,26,255))
    # torso/shoulders
    body = shirt if jacket is None else jacket
    d.ellipse([cx-4.6*S, 4.5*S, cx+4.6*S, 12.0*S], fill=body)
    if jacket is not None:                         # open jacket: shirt shows BELOW the head only
        d.rectangle([cx-1.3*S, 8.4*S, cx+1.3*S, 11.2*S], fill=shirt)
    # arms: swing opposite to feet, hands = skin
    d.ellipse([cx-5.6*S, 6.5*S + sw, cx-3.2*S, 10.0*S + sw], fill=arm)
    d.ellipse([cx+3.2*S, 6.5*S - sw, cx+5.6*S, 10.0*S - sw], fill=arm)
    d.ellipse([cx-5.1*S, 6.2*S + sw, cx-3.7*S, 8.0*S + sw], fill=skin)
    d.ellipse([cx+3.7*S, 6.2*S - sw, cx+5.1*S, 8.0*S - sw], fill=skin)
    # head: skin sliver at the front (face), hair cap behind
    d.ellipse([cx-2.9*S, 1.2*S, cx+2.9*S, 7.2*S], fill=skin)
    d.ellipse([cx-2.9*S, 2.0*S, cx+2.9*S, 7.8*S], fill=hair)
    return im

SKINS = [(224,178,142,255), (196,148,110,255), (150,104,70,255), (232,190,158,255)]
HAIRS = [(50,38,30,255), (24,22,24,255), (150,120,60,255), (90,90,96,255)]

# ---- ped.png: 4 variants (rows) x 2 frames (cols) ----
VAR = [((172,52,44,255)),(( 70,132,60,255)),((198,172,60,255)),((110,112,122,255))]
sheet = Image.new("RGBA", (32,64), (0,0,0,0))
for v in range(4):
    for f in range(2):
        img = down(person(VAR[v], HAIRS[v], SKINS[v], 1.0 if f==0 else -1.0))
        sheet.paste(img, (f*16, v*16))
sheet.save("ped.png"); print("wrote ped.png 32x64")

# ---- player.png: leather jacket + white tee — 4 walk frames + AIM + FIRE ----
def player_pose(fire):
    im = canvas(); d = ImageDraw.Draw(im); cx = 8*S
    JK=(34,32,38,255); SKN=SKINS[0]; GUN=(50,52,60,255)
    d.ellipse([cx-3.2*S, 10.5*S, cx-0.8*S, 14*S], fill=(24,22,26,255))   # feet planted
    d.ellipse([cx+0.8*S, 10.5*S, cx+3.2*S, 14*S], fill=(24,22,26,255))
    d.ellipse([cx-4.6*S, 5.5*S, cx+4.6*S, 12.5*S], fill=JK)              # torso
    d.rectangle([cx-1.3*S, 9.2*S, cx+1.3*S, 11.6*S], fill=(52,50,58,255))
    d.ellipse([cx-2.5*S, 1.6*S, cx-0.5*S, 6.6*S], fill=JK)               # arms out front
    d.ellipse([cx+0.5*S, 1.6*S, cx+2.5*S, 6.6*S], fill=JK)
    d.ellipse([cx-1.3*S, 1.0*S, cx+1.3*S, 3.0*S], fill=SKN)              # hands
    d.rectangle([cx-0.6*S, 0.0*S, cx+0.6*S, 1.6*S], fill=GUN)            # pistol
    if fire:
        d.polygon([(cx,-1.8*S),(cx-1.5*S,0.2*S),(cx+1.5*S,0.2*S)], fill=(255,230,120,255))
        d.ellipse([cx-0.9*S,-1.0*S,cx+0.9*S,0.4*S], fill=(255,180,60,255))
    d.ellipse([cx-2.9*S, 3.0*S, cx+2.9*S, 9.0*S], fill=SKN)              # head back a touch
    d.ellipse([cx-2.9*S, 3.8*S, cx+2.9*S, 9.6*S], fill=(58,40,26,255))
    return down(im)
sheet = Image.new("RGBA", (96,16), (0,0,0,0))
CYCLE = [1.0, 0.25, -1.0, -0.25]
for f in range(4):
    img = down(person((52,50,58,255), (58,40,26,255), SKINS[0], CYCLE[f], jacket=(34,32,38,255)))
    sheet.paste(img, (f*16, 0))
sheet.paste(player_pose(False), (64, 0))
sheet.paste(player_pose(True),  (80, 0))
sheet.save("player.png"); print("wrote player.png 96x16 (walk x4, aim, fire)")

# ---- pickups.png: cash / pistol / smg / shotgun / medkit ----
def cell(draw_fn):
    im = canvas(); draw_fn(ImageDraw.Draw(im)); return down(im)
GM  = (108,114,128,255); GMD = (40,42,50,255)  # bright gunmetal + dark outline (reads at 16px)
def p_cash(d):
    d.rounded_rectangle([2*S,4.5*S,14*S,11.5*S], radius=1.5*S, fill=(52,120,58,255), outline=(24,60,30,255), width=S)
    d.rectangle([2*S,7.2*S,14*S,8.8*S], fill=(74,158,80,255))
    d.ellipse([6.4*S,5.8*S,9.6*S,10.2*S], fill=(210,224,190,255))
    d.line([8*S,5.2*S,8*S,10.8*S], fill=(30,80,40,255), width=max(1,S//2))
def p_pistol(d):
    d.rounded_rectangle([2.6*S,5*S,13.4*S,9*S], radius=S, fill=GM, outline=GMD, width=S)    # slide
    d.rectangle([3.8*S,8.6*S,7.6*S,13.4*S], fill=(70,52,38,255))                            # grip (wood)
    d.rectangle([3.8*S,8.6*S,7.6*S,9.6*S], fill=GMD)
def p_smg(d):
    d.rounded_rectangle([1.2*S,5.4*S,14.8*S,9.2*S], radius=S, fill=GM, outline=GMD, width=S)
    d.rectangle([6.2*S,9*S,9.2*S,14*S], fill=GMD)                                           # magazine
    d.rectangle([1.2*S,6.2*S,2.8*S,10.6*S], fill=GMD)                                       # stock
    d.rectangle([13.4*S,5.8*S,15.8*S,7.4*S], fill=GMD)                                      # barrel
def p_shotgun(d):
    d.rounded_rectangle([0.6*S,6.2*S,15.4*S,9.4*S], radius=S//2, fill=GM, outline=GMD, width=S)
    d.rectangle([2*S,5.6*S,6.4*S,10.2*S], fill=(140,92,52,255))                             # wood pump
    d.rectangle([12*S,5.6*S,15.4*S,10.2*S], fill=(140,92,52,255))                           # stock
def p_medkit(d):
    d.rounded_rectangle([2.4*S,3.6*S,13.6*S,12.4*S], radius=1.4*S, fill=(232,232,230,255), outline=(150,150,150,255), width=S//2)
    d.rectangle([6.6*S,5.4*S,9.4*S,10.6*S], fill=(200,44,44,255))
    d.rectangle([4.6*S,6.9*S,11.4*S,9.1*S], fill=(200,44,44,255))
def p_flamer(d):
    d.rounded_rectangle([3.4*S,3*S,9*S,13*S], radius=S, fill=(190,60,40,255), outline=(90,26,18,255), width=S//2)   # red tank
    d.rectangle([4.6*S,1.6*S,7.8*S,3*S], fill=(90,92,100,255))                     # valve
    d.rounded_rectangle([9.6*S,6.4*S,14.6*S,8.4*S], radius=S//2, fill=(108,114,128,255), outline=(40,42,50,255), width=S//2)  # nozzle arm
    d.polygon([(14.6*S,5.6*S),(16*S,7.4*S),(14.6*S,9.2*S)], fill=(255,170,60,255)) # pilot flame
def p_rocket(d):
    d.rounded_rectangle([1.2*S,6.2*S,12.6*S,9.8*S], radius=1.4*S, fill=(96,120,86,255), outline=(46,58,42,255), width=S//2)  # olive tube
    d.polygon([(12.6*S,5.4*S),(15.6*S,8*S),(12.6*S,10.6*S)], fill=(196,60,44,255))   # warhead
    d.rectangle([2.6*S,5*S,4*S,6.2*S], fill=(46,58,42,255))                          # sight
sheet = Image.new("RGBA", (112,16), (0,0,0,0))
for i, fn in enumerate([p_cash, p_pistol, p_smg, p_shotgun, p_medkit, p_flamer, p_rocket]):
    sheet.paste(cell(fn), (i*16, 0))
sheet.save("pickups.png"); print("wrote pickups.png 112x16")
