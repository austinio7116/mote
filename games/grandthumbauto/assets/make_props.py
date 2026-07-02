#!/usr/bin/env python3
"""Author the world-prop sprites (props.png, 3 cells of 16x16, top-down):
  0 PHONEBOX — red telephone booth   1 GUNSHOP — pistol on a shop mat
  2 SPRAY    — paint splat + can (pay-n-spray bay)
Editable source; `mote bake` -> src/props.h (props_img)."""
from PIL import Image, ImageDraw

S = 6
def cell(fn):
    im = Image.new("RGBA", (16*S, 16*S), (0,0,0,0))
    fn(ImageDraw.Draw(im))
    return im.resize((16,16), Image.LANCZOS)

def phonebox(d):
    d.rounded_rectangle([2*S,2*S,14*S,14*S], radius=1.2*S, fill=(178,40,36,255), outline=(96,20,18,255), width=S)
    d.rectangle([4.4*S,4.4*S,11.6*S,11.6*S], fill=(150,190,205,255))            # glass roof pane
    d.line([8*S,4.4*S,8*S,11.6*S], fill=(178,40,36,255), width=S)               # glazing bars
    d.line([4.4*S,8*S,11.6*S,8*S], fill=(178,40,36,255), width=S)
    d.ellipse([7*S,7*S,9*S,9*S], fill=(240,220,140,255))                        # roof lamp

def gunshop(d):
    d.rounded_rectangle([1.5*S,3*S,14.5*S,13*S], radius=S, fill=(46,58,46,255), outline=(210,190,90,255), width=S)
    d.rounded_rectangle([4*S,6*S,12.5*S,8.4*S], radius=S//2, fill=(215,218,225,255))   # slide
    d.rectangle([5.2*S,8.2*S,7.4*S,11.4*S], fill=(215,218,225,255))                    # grip
    d.rectangle([11*S,6.6*S,12.5*S,7.4*S], fill=(150,150,160,255))                     # muzzle

def spray(d):
    d.ellipse([1.5*S,4*S,11*S,13.5*S], fill=(80,190,120,255))                   # paint splat
    d.ellipse([3.5*S,6*S,7*S,9.5*S], fill=(120,220,150,255))
    d.rounded_rectangle([9*S,2*S,13.5*S,10*S], radius=S, fill=(180,184,195,255), outline=(90,92,100,255), width=S//2)
    d.rectangle([10.4*S,1.2*S,12.1*S,2.4*S], fill=(60,62,70,255))               # nozzle

def dock(d):
    d.rounded_rectangle([1.5*S,1.5*S,14.5*S,14.5*S], radius=S, fill=(96,74,52,255), outline=(60,46,32,255), width=S//2)
    for y in (5,9,13):                                                          # pier planks
        d.line([2*S,y*S,14*S,y*S], fill=(74,58,40,255))
    d.ellipse([6.2*S,3.4*S,9.8*S,7*S], outline=(220,222,230,255), width=S)      # anchor ring
    d.line([8*S,6.4*S,8*S,12*S], fill=(220,222,230,255), width=S)               # shank
    d.arc([4.6*S,8.6*S,11.4*S,13.8*S], 20, 160, fill=(220,222,230,255), width=S) # flukes
sheet = Image.new("RGBA", (64,16), (0,0,0,0))
for i,fn in enumerate([phonebox, gunshop, spray, dock]):
    sheet.paste(cell(fn), (i*16,0))
sheet.save("/home/maustin/thumby-color/mote/games/grandthumbauto/assets/props.png")
print("wrote assets/props.png 64x16")
