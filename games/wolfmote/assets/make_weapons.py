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
    # two barrels running AWAY from the eye — foreshortened trapezoids, muzzles far/small
    for side in (-1, 1):
        nb, fb = 4.6, 2.6                     # near/far half-widths
        noff, foff = 5.2, 3.4                 # near/far centre offsets (converge slightly)
        x0=cx+side*noff*S; x1=cx+side*foff*S
        d.polygon([(x0-nb*S,38*S),(x0+nb*S,38*S),(x1+fb*S,7*S),(x1-fb*S,7*S)], fill=ST, outline=OUT, width=S)
        d.polygon([(x0-nb*S,38*S),(x1-fb*S,7*S),(x1-fb*S+1.6*S,7*S),(x0-nb*S+2.6*S,38*S)], fill=STH)  # top glint
        d.ellipse([x1-fb*S+0.6*S,5.6*S,x1+fb*S-0.6*S,8.6*S], fill=BORE, outline=OUT, width=S//2)      # far muzzle
    # mid barrel band
    d.polygon([(cx-10.6*S,22*S),(cx+10.6*S,22*S),(cx+11.4*S,25.5*S),(cx-11.4*S,25.5*S)], fill=STD, outline=OUT, width=S//2)
    # wood forend under the barrels + pump hand
    d.polygon([(cx-11*S,46*S),(cx+11*S,46*S),(cx+9.4*S,36*S),(cx-9.4*S,36*S)], fill=WOOD, outline=WOODD, width=S)
    d.line([cx-8.5*S,39*S,cx+8.5*S,39*S], fill=WOODH, width=S//2)
    d.line([cx-9*S,42.5*S,cx+9*S,42.5*S], fill=WOODD, width=S//2)
    hand(d, cx-2*S, 44*S, 15*S, 11*S)
    # receiver + forearm at the bottom edge
    d.polygon([(cx-8*S,CH*S),(cx+12*S,CH*S),(cx+10*S,47*S),(cx-6*S,47*S)], fill=SKIND)
    return down(im)

def chaingun():
    im=cell(); d=ImageDraw.Draw(im); cx=CW*S/2
    import math
    # barrel BUNDLE running away: three visible tubes foreshortened into the screen
    for off,nw,fw in ((-6.2,3.4,1.9),(6.2,3.4,1.9),(0,3.8,2.2)):
        x0=cx+off*S; x1=cx+off*0.55*S
        d.polygon([(x0-nw*S,34*S),(x0+nw*S,34*S),(x1+fw*S,7*S),(x1-fw*S,7*S)], fill=ST, outline=OUT, width=S)
        d.polygon([(x0-nw*S,34*S),(x1-fw*S,7*S),(x1-fw*S+1.3*S,7*S),(x0-nw*S+2.1*S,34*S)], fill=STH)
    # far muzzle cluster: small bores at the tip
    for offx,offy in ((-3.4,0.2),(3.4,0.2),(0,-1.2),(0,1.8)):
        bx=cx+offx*S; by=6.6*S+offy*S
        d.ellipse([bx-1.9*S,by-1.7*S,bx+1.9*S,by+1.7*S], fill=BORE, outline=OUT, width=S//2)
    # rotating collar mid-way
    d.polygon([(cx-9.4*S,20*S),(cx+9.4*S,20*S),(cx+10.4*S,24*S),(cx-10.4*S,24*S)], fill=STD, outline=OUT, width=S//2)
    # drum housing at the near end (seen from behind-top)
    d.rounded_rectangle([cx-13*S,34*S,cx+13*S,50*S], radius=4*S, fill=STD, outline=OUT, width=S)
    d.ellipse([cx-10*S,35.5*S,cx+10*S,42*S], fill=ST)                      # top face catches light
    d.arc([cx-10*S,35.5*S,cx+10*S,42*S], 160, 380, fill=STH, width=S)
    # ammo belt feeding the right side
    d.rounded_rectangle([cx+12*S,38*S,cx+22*S,47*S], radius=2*S, fill=ST, outline=OUT, width=S)
    for i in range(3):
        d.line([cx+(14+i*2.8)*S,39.4*S,cx+(14+i*2.8)*S,45.6*S], fill=(224,186,80,255), width=S)
    # both hands on the frame
    hand(d, cx-11.5*S, 48*S, 13*S, 10*S)
    hand(d, cx+9.5*S, 51*S, 13*S, 10*S)
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
