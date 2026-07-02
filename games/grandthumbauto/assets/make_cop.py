#!/usr/bin/env python3
"""Top-down police officer sprite — 4 cells of 16x16 (64x16 sheet):
  0/1 walk stride   2 AIM (arms out front, pistol)   3 FIRE (muzzle flash)
Faces UP. Editable source; `mote bake` -> src/cop.h (cop_img)."""
from PIL import Image, ImageDraw

SK=(222,170,135,255)      # skin
NAVY=(38,52,120,255)      # uniform
CAP=(20,28,66,255)        # cap
VIS=(10,12,22,255)        # visor / shoes
BADGE=(220,222,232,255)   # silver badge
GUN=(50,52,60,255)
S=6

def frame(pose, step=0):
    im=Image.new("RGBA",(16*S,16*S),(0,0,0,0)); d=ImageDraw.Draw(im)
    cx=8*S
    if pose=="walk":
        la = 3 if step==0 else -2
        lb = -2 if step==0 else 3
        d.ellipse([cx-4*S, (10.5+la*0.4)*S, cx-1*S, (14.5+la*0.4)*S], fill=VIS)
        d.ellipse([cx+1*S, (10.5+lb*0.4)*S, cx+4*S, (14.5+lb*0.4)*S], fill=VIS)
    else:
        d.ellipse([cx-4*S, 10.5*S, cx-1*S, 14.5*S], fill=VIS)
        d.ellipse([cx+1*S, 10.5*S, cx+4*S, 14.5*S], fill=VIS)
    d.ellipse([cx-5*S, 6*S, cx+5*S, 13*S], fill=NAVY)          # torso
    if pose=="walk":
        sw = int(2.6*S)*(1 if step==0 else -1)
        d.ellipse([cx-6*S, 7*S+sw, cx-3*S, 10.5*S+sw], fill=NAVY)   # arms swing
        d.ellipse([cx+3*S, 7*S-sw, cx+6*S, 10.5*S-sw], fill=NAVY)
        d.ellipse([cx-5.4*S, 6.7*S+sw, cx-3.8*S, 8.5*S+sw], fill=SK)
        d.ellipse([cx+3.8*S, 6.7*S-sw, cx+5.4*S, 8.5*S-sw], fill=SK)
    else:                                                      # AIM: both arms out front
        d.ellipse([cx-2.6*S, 1.8*S, cx-0.6*S, 7*S], fill=NAVY)
        d.ellipse([cx+0.6*S, 1.8*S, cx+2.6*S, 7*S], fill=NAVY)
        d.ellipse([cx-1.4*S, 1.2*S, cx+1.4*S, 3.2*S], fill=SK) # joined hands
        d.rectangle([cx-0.6*S, 0.2*S, cx+0.6*S, 1.8*S], fill=GUN)   # pistol
        if pose=="fire":                                        # muzzle flash
            d.polygon([(cx,-1.6*S),(cx-1.4*S,0.4*S),(cx+1.4*S,0.4*S)], fill=(255,230,120,255))
            d.ellipse([cx-0.8*S,-0.8*S,cx+0.8*S,0.6*S], fill=(255,180,60,255))
    d.ellipse([cx-1*S, 8*S, cx+1*S, 10*S], fill=BADGE)          # badge
    hy = 3.0 if pose=="walk" else 4.6                           # head sits back when aiming
    d.ellipse([cx-2.9*S, (hy-1.8)*S, cx+2.9*S, (hy+4.2)*S], fill=SK)
    d.ellipse([cx-2.9*S, (hy-1.0)*S, cx+2.9*S, (hy+4.8)*S], fill=CAP)
    d.rectangle([cx-2.9*S, (hy-1.0)*S, cx+2.9*S, (hy+1.0)*S], fill=VIS)
    return im.resize((16,16), Image.LANCZOS)

sheet=Image.new("RGBA",(64,16),(0,0,0,0))
sheet.paste(frame("walk",0),(0,0));  sheet.paste(frame("walk",1),(16,0))
sheet.paste(frame("aim"),(32,0));    sheet.paste(frame("fire"),(48,0))
sheet.save("/home/maustin/thumby-color/mote/games/grandthumbauto/assets/cop.png")
print("wrote assets/cop.png 64x16 (walk x2, aim, fire)")
