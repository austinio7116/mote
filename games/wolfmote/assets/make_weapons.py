#!/usr/bin/env python3
"""First-person weapon art + per-weapon pickup icons (EDITABLE SOURCES).
  weapons.png  216x56 — three 72x56 cells: PISTOL / SHOTGUN / CHAINGUN, viewed
               from the shooter (gun + gripping hands), 3-tone steel + outlines.
  wpickup.png  60x20  — three 20x20 side-profile icons matching the weapons.
`mote bake` -> src/weapons.h + src/wpickup.h."""
from PIL import Image, ImageDraw, ImageFilter

S = 6
CW, CH = 72, 56

OUT=(26,24,30,255)          # outline
ST =(104,108,122,255)       # steel base
STH=(170,176,192,255)       # steel highlight
STD=(52,54,64,255)          # steel shade
BORE=(14,14,18,255)         # barrel bore
WOOD=(126,84,46,255); WOODH=(158,110,64,255); WOODD=(86,54,28,255)
SKIN=(214,166,128,255); SKIND=(176,128,94,255)

def cell():
    return Image.new("RGBA",(CW*S,CH*S),(0,0,0,0))
def down(im):
    return im.resize((CW,CH), Image.LANCZOS)

def hand(d, cx, cy, w, h, ang_fingers=True):
    """a gripping fist seen from behind"""
    d.rounded_rectangle([cx-w/2, cy-h/2, cx+w/2, cy+h/2], radius=w*0.3, fill=SKIN, outline=SKIND, width=S)
    if ang_fingers:
        for i in range(3):
            fy = cy-h*0.28+i*h*0.28
            d.line([cx-w/2+S, fy, cx+w/2-S, fy], fill=SKIND, width=S//2)

def pistol():
    im=cell(); d=ImageDraw.Draw(im); cx=CW*S/2
    # forearm hint
    d.polygon([(cx-9*S,CH*S),(cx+9*S,CH*S),(cx+6*S,44*S),(cx-6*S,44*S)], fill=SKIND)
    # grip + hand
    hand(d, cx, 44*S, 14*S, 11*S)
    # slide: perspective trapezoid running away from the eye
    d.polygon([(cx-8.5*S,40*S),(cx+8.5*S,40*S),(cx+5.5*S,8*S),(cx-5.5*S,8*S)], fill=ST, outline=OUT, width=S)
    d.polygon([(cx-8.5*S,40*S),(cx-5.5*S,8*S),(cx-3.4*S,8*S),(cx-5.6*S,40*S)], fill=STH)      # left glint
    d.polygon([(cx+5.2*S,40*S),(cx+8.5*S,40*S),(cx+5.5*S,8*S),(cx+3.6*S,8*S)], fill=STD)      # right shade
    # rear serrations
    for i in range(4):
        y=(34.5+i*1.7)*S
        d.line([cx-7.2*S,y,cx+7.2*S,y], fill=STD, width=S//2)
    # rear sight
    d.rectangle([cx-6.5*S,31*S,cx+6.5*S,33.4*S], fill=STD, outline=OUT, width=S//2)
    d.rectangle([cx-1.6*S,31*S,cx+1.6*S,33.4*S], fill=BORE)
    # muzzle + front sight
    d.rectangle([cx-5.5*S,6*S,cx+5.5*S,9*S], fill=STD, outline=OUT, width=S//2)
    d.ellipse([cx-2.6*S,6.2*S,cx+2.6*S,8.8*S], fill=BORE)
    d.rectangle([cx-0.9*S,3.4*S,cx+0.9*S,6.4*S], fill=ST, outline=OUT, width=S//2)
    return down(im)

def shotgun():
    im=cell(); d=ImageDraw.Draw(im); cx=CW*S/2
    # stock shoulder wedge
    d.polygon([(cx-16*S,CH*S),(cx+16*S,CH*S),(cx+11*S,40*S),(cx-11*S,40*S)], fill=WOOD, outline=WOODD, width=S)
    d.line([(cx-9*S),46*S,(cx+9*S),46*S], fill=WOODD, width=S//2)
    d.line([(cx-10*S),50*S,(cx+10*S),50*S], fill=WOODD, width=S//2)
    # receiver
    d.polygon([(cx-11*S,42*S),(cx+11*S,42*S),(cx+9*S,26*S),(cx-9*S,26*S)], fill=ST, outline=OUT, width=S)
    d.polygon([(cx-11*S,42*S),(cx-9*S,26*S),(cx-6.6*S,26*S),(cx-8*S,42*S)], fill=STH)
    d.rectangle([cx-2.2*S,36*S,cx+2.2*S,40*S], fill=STD, outline=OUT, width=S//2)   # top lever
    # twin barrels: big muzzles toward the eye-line top
    for sx in (-5.4, 5.4):
        bx=cx+sx*S
        d.ellipse([bx-5.4*S,10*S,bx+5.4*S,26*S], fill=ST, outline=OUT, width=S)     # barrel shroud
        d.ellipse([bx-4.4*S,12*S,bx+4.4*S,24*S], fill=STD)
        d.ellipse([bx-3.2*S,14*S,bx+3.2*S,22*S], fill=BORE)
        d.arc([bx-4.4*S,12*S,bx+4.4*S,24*S], 130, 230, fill=STH, width=S)           # rim glint
    # forend hand on the pump
    hand(d, cx, 33*S, 20*S, 11*S)
    return down(im)

def chaingun():
    im=cell(); d=ImageDraw.Draw(im); cx=CW*S/2
    # housing
    d.polygon([(cx-15*S,CH*S),(cx+15*S,CH*S),(cx+12*S,34*S),(cx-12*S,34*S)], fill=STD, outline=OUT, width=S)
    d.polygon([(cx-15*S,CH*S),(cx-12*S,34*S),(cx-9*S,34*S),(cx-11.6*S,CH*S)], fill=ST)
    # ammo feed to the right
    d.rounded_rectangle([cx+11*S,40*S,cx+22*S,50*S], radius=2*S, fill=ST, outline=OUT, width=S)
    for i in range(3):
        d.line([cx+(13+i*3)*S,41*S,cx+(13+i*3)*S,49*S], fill=(220,180,70,255), width=S)
    # rotary drum with six barrel bores
    R=13.5
    d.ellipse([cx-R*S,8*S,cx+R*S,(8+2*R)*S], fill=ST, outline=OUT, width=S)
    d.arc([cx-R*S,8*S,cx+R*S,(8+2*R)*S], 120, 250, fill=STH, width=S+2)
    import math
    ccy=(8+R)*S
    for k in range(6):
        a=k*math.pi/3 + 0.5
        bx=cx+math.cos(a)*7.4*S; by=ccy+math.sin(a)*7.4*S
        d.ellipse([bx-3.2*S,by-3.2*S,bx+3.2*S,by+3.2*S], fill=STD, outline=OUT, width=S//2)
        d.ellipse([bx-2*S,by-2*S,bx+2*S,by+2*S], fill=BORE)
    d.ellipse([cx-2.6*S,ccy-2.6*S,cx+2.6*S,ccy+2.6*S], fill=STD, outline=OUT, width=S//2)  # hub
    # two supporting hands
    hand(d, cx-11*S, 44*S, 14*S, 11*S)
    hand(d, cx+11*S, 44*S, 14*S, 11*S)
    return down(im)

sheet=Image.new("RGBA",(CW*3,CH),(0,0,0,0))
for i,fn in enumerate([pistol,shotgun,chaingun]):
    sheet.paste(fn(),(i*CW,0))
sheet.save("/home/maustin/thumby-color/mote/games/wolfmote/assets/weapons.png")

# ---- per-weapon pickup icons (side profiles, 20x20 cells) ----
P=6
def pk_cell(fn):
    im=Image.new("RGBA",(20*P,20*P),(0,0,0,0)); fn(ImageDraw.Draw(im))
    return im.resize((20,20), Image.LANCZOS)
def pk_pistol(d):
    d.rounded_rectangle([1.6*P,5*P,18*P,10.6*P], radius=P, fill=ST, outline=OUT, width=P//2)   # slide
    d.rectangle([2.6*P,6*P,17*P,7.2*P], fill=STH)
    d.rectangle([1.6*P,5*P,3.4*P,6.6*P], fill=STD)                                             # rear sight
    d.polygon([(4*P,10.6*P),(10.4*P,10.6*P),(9*P,17.5*P),(4*P,17.5*P)], fill=WOOD)             # grip
    d.line([5*P,12*P,8.6*P,12*P], fill=WOODD, width=P//2)
    d.rectangle([10.6*P,10.6*P,13*P,13.4*P], fill=STD, outline=OUT, width=P//3)                # guard
def pk_shotgun(d):
    d.rounded_rectangle([0.6*P,6.4*P,19.4*P,9.8*P], radius=P//2, fill=ST, outline=OUT, width=P//2)  # barrel
    d.rectangle([1.6*P,7.2*P,18*P,8*P], fill=STH)
    d.rounded_rectangle([4*P,9.4*P,10.4*P,13*P], radius=P//2, fill=WOOD, outline=WOODD, width=P//2) # pump
    d.line([5*P,10.8*P,9.4*P,10.8*P], fill=WOODD, width=P//2)
    d.polygon([(13.6*P,9.8*P),(19.4*P,9.8*P),(19.4*P,16*P),(15.6*P,14.4*P)], fill=WOOD)        # stock
    d.polygon([(13.6*P,9.8*P),(15.4*P,9.8*P),(16.6*P,13.2*P),(14.6*P,12.4*P)], fill=WOODH)
def pk_chaingun(d):
    for dy in (-2.2,0,2.2):
        d.rounded_rectangle([0.6*P,(7.6+dy)*P,12.4*P,(9.4+dy)*P], radius=P//2, fill=ST, outline=OUT, width=P//3)
        d.line([1.6*P,(8+dy)*P,11.6*P,(8+dy)*P], fill=STH, width=P//3)
    d.ellipse([10.4*P,3.6*P,19.6*P,13*P], fill=STD, outline=OUT, width=P//2)                    # drum
    d.arc([10.4*P,3.6*P,19.6*P,13*P], 120, 260, fill=STH, width=P//2)
    d.ellipse([13.6*P,6.8*P,16.4*P,9.8*P], fill=BORE)
    d.rectangle([13*P,13*P,16*P,17*P], fill=WOOD, outline=WOODD, width=P//3)                    # grip
sheet=Image.new("RGBA",(60,20),(0,0,0,0))
for i,fn in enumerate([pk_pistol,pk_shotgun,pk_chaingun]):
    sheet.paste(pk_cell(fn),(i*20,0))
sheet.save("/home/maustin/thumby-color/mote/games/wolfmote/assets/wpickup.png")
print("wrote weapons.png 216x56 + wpickup.png 60x20")
