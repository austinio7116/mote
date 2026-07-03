#!/usr/bin/env python3
"""Enemy sprites, redrawn (EDITABLE SOURCES; `mote bake` -> src/*.h).
  guard.png 96x40 — 4 frames of 24x40: idle / FIRE / pain / dead
  brute.png 112x44 — 4 frames of 28x44 (heavy armour, shoulder cannon)
Outlined, two-tone lit (key light from the LEFT), faces the camera.
rusher/commando/boss regenerate from these via make_extra.py hue shifts."""
from PIL import Image, ImageDraw

S = 8                                        # supersample

OUT=(22,20,26,255)
SKIN=(212,166,128,255); SKIND=(172,126,92,255)
BLOOD=(122,18,22,255); BLOOD2=(154,26,28,255)

def down(im,w,h): return im.resize((w,h), Image.LANCZOS)

def shaded_rrect(d, box, r, base, lite, dark, ow=1):
    """rounded block with left-light / right-shade + outline"""
    x0,y0,x1,y1=box
    d.rounded_rectangle(box, radius=r, fill=base, outline=OUT, width=ow*S//2)
    w=x1-x0
    def band(a,b,col):
        span=b-a
        if span < 2: return
        rr=min(r*0.7, span/2-1, (y1-y0)/2-1)
        if rr < 1: d.rectangle([a,y0+ow*S*0.6,b,y1-ow*S*0.6], fill=col)
        else: d.rounded_rectangle([a,y0+ow*S*0.6,b,y1-ow*S*0.6], radius=rr, fill=col)
    band(x0+ow*S*0.6, x0+w*0.32, lite)
    band(x1-w*0.26,  x1-ow*S*0.6, dark)

def guard_frame(pose):
    CW,CH=24,40
    im=Image.new("RGBA",(CW*S,CH*S),(0,0,0,0)); d=ImageDraw.Draw(im); cx=CW*S/2
    UNI=(52,74,152,255); UNIL=(78,104,190,255); UNID=(36,52,110,255)
    TRIM=(30,36,60,255); GM=(58,60,70,255)
    lean = 1.2*S if pose=="pain" else 0
    if pose=="dead":
        d.ellipse([2*S,30*S,22*S,39*S], fill=BLOOD)                       # pool
        d.ellipse([4.5*S,31.5*S,15*S,36.5*S], fill=BLOOD2)
        shaded_rrect(d,[6*S,28*S,20*S,34*S],2*S,UNI,UNIL,UNID)            # prone torso
        d.ellipse([1.8*S,28.5*S,7.4*S,34*S], fill=SKIN, outline=OUT, width=S//2)   # head
        d.pieslice([1.2*S,27.8*S,7.8*S,33*S],150,330, fill=TRIM)          # helmet
        d.rounded_rectangle([19*S,29.5*S,23*S,32.5*S], radius=S, fill=TRIM)        # boot
        return down(im,CW,CH)
    # legs + boots
    for sx in (-1,1):
        lx=cx+sx*3.1*S+lean*0.3
        shaded_rrect(d,[lx-2.2*S,26*S,lx+2.2*S,36.5*S],1.4*S,UNID,UNI,TRIM)
        d.rounded_rectangle([lx-2.5*S,36*S,lx+2.5*S,39.2*S], radius=S, fill=TRIM, outline=OUT, width=S//2)
    # torso
    shaded_rrect(d,[cx-6.4*S+lean,13*S,cx+6.4*S+lean,27.5*S],2.4*S,UNI,UNIL,UNID)
    d.rectangle([cx-6.2*S+lean,23.4*S,cx+6.2*S+lean,25.2*S], fill=TRIM)   # belt
    d.rectangle([cx-1.2*S+lean,23.2*S,cx+1.2*S+lean,25.4*S], fill=(168,140,70,255))  # buckle
    d.line([cx-5.6*S+lean,14.4*S,cx+2*S+lean,22.6*S], fill=UNID, width=S)  # chest strap
    if pose=="fire":
        # rifle levelled AT the viewer: hands together, dark muzzle disc centre-chest
        d.ellipse([cx-3.4*S,16.2*S,cx+3.4*S,23*S], fill=GM, outline=OUT, width=S//2)
        d.ellipse([cx-1.9*S,17.7*S,cx+1.9*S,21.5*S], fill=(12,12,16,255))
        d.ellipse([cx-4.8*S,20.4*S,cx-0.6*S,24.6*S], fill=SKIN, outline=SKIND, width=S//2)
        d.ellipse([cx+0.6*S,20.4*S,cx+4.8*S,24.6*S], fill=SKIN, outline=SKIND, width=S//2)
    else:
        # arms: idle by the sides holding the rifle low across / pain flared out
        spread = 3.4 if pose=="pain" else 0.0
        for sx in (-1,1):
            ax=cx+sx*(7.6+spread)*S+lean
            ang = 10 if pose!="pain" else 34
            d.rounded_rectangle([ax-1.8*S,14.5*S,ax+1.8*S,24*S], radius=1.4*S,
                                fill=UNI, outline=OUT, width=S//2)
            d.ellipse([ax-1.6*S,23*S,ax+1.6*S,26.4*S], fill=SKIN, outline=SKIND, width=S//2)
        if pose=="idle":
            d.rounded_rectangle([cx-8.4*S,21.6*S,cx+8.4*S,23.6*S], radius=S, fill=GM, outline=OUT, width=S//2)  # rifle across
            d.rectangle([cx+6.4*S,20.4*S,cx+8.4*S,22*S], fill=GM)
    # head + helmet
    hx=cx+lean
    d.ellipse([hx-4.4*S,3.6*S,hx+4.4*S,13.4*S], fill=SKIN, outline=OUT, width=S//2)
    d.pieslice([hx-4.9*S,2.2*S,hx+4.9*S,11.4*S],165,375, fill=TRIM, outline=OUT, width=S//2)   # helmet
    d.rectangle([hx-4.9*S,6.2*S,hx+4.9*S,7.2*S], fill=(64,74,110,255))                          # helmet band
    d.ellipse([hx-4.4*S,5*S,hx-1.4*S,7*S], fill=(88,100,150,120))                               # helm sheen
    ey = 9.2 if pose!="pain" else 8.8
    for sx in (-1,1):                                                                            # eyes
        d.ellipse([hx+sx*2.2*S-0.7*S,ey*S,hx+sx*2.2*S+0.7*S,(ey+1.4)*S], fill=(24,22,30,255))
    if pose=="pain": d.ellipse([hx-1.2*S,11.2*S,hx+1.2*S,12.8*S], fill=(70,26,26,255))          # ow
    d.line([hx+1.6*S,8*S,hx+3.6*S,12*S], fill=SKIND, width=S//2)                                # face shade
    return down(im,CW,CH)

def brute_frame(pose):
    CW,CH=28,44
    im=Image.new("RGBA",(CW*S,CH*S),(0,0,0,0)); d=ImageDraw.Draw(im); cx=CW*S/2
    AR=(96,72,48,255); ARL=(132,102,70,255); ARD=(64,48,32,255)          # bronze plate
    TRIM=(40,34,26,255); GM=(58,60,70,255)
    lean = 1.4*S if pose=="pain" else 0
    if pose=="dead":
        d.ellipse([2*S,33*S,26*S,43*S], fill=BLOOD)
        d.ellipse([5*S,35*S,18*S,41*S], fill=BLOOD2)
        shaded_rrect(d,[6*S,31*S,23*S,38*S],2.4*S,AR,ARL,ARD)
        d.ellipse([2*S,31.5*S,8.6*S,38*S], fill=ARD, outline=OUT, width=S//2)
        return down(im,CW,CH)
    # legs
    for sx in (-1,1):
        lx=cx+sx*4*S
        shaded_rrect(d,[lx-2.8*S,29*S,lx+2.8*S,40*S],1.6*S,ARD,AR,TRIM)
        d.rounded_rectangle([lx-3.2*S,39.4*S,lx+3.2*S,43.2*S], radius=S, fill=TRIM, outline=OUT, width=S//2)
    # massive torso plate
    shaded_rrect(d,[cx-8.6*S+lean,13.5*S,cx+8.6*S+lean,30.5*S],3*S,AR,ARL,ARD)
    d.line([cx+lean,15*S,cx+lean,29*S], fill=ARD, width=S)                # plate seam
    d.rectangle([cx-8.2*S+lean,26*S,cx+8.2*S+lean,28*S], fill=TRIM)      # girdle
    # pauldrons
    for sx in (-1,1):
        px2=cx+sx*9.6*S+lean
        d.ellipse([px2-3.8*S,12*S,px2+3.8*S,19.5*S], fill=ARD, outline=OUT, width=S//2)
        d.ellipse([px2-2.6*S,13*S,px2+0.6*S,16*S], fill=AR)
    if pose=="fire":
        # arm cannon levelled at the viewer
        d.ellipse([cx-4.2*S,17*S,cx+4.2*S,25.4*S], fill=GM, outline=OUT, width=S//2)
        d.ellipse([cx-2.4*S,18.9*S,cx+2.4*S,23.5*S], fill=(12,12,16,255))
        d.ellipse([cx+4.4*S,19.6*S,cx+8.8*S,24.4*S], fill=ARD, outline=OUT, width=S//2)  # supporting fist
    else:
        spread = 3.0 if pose=="pain" else 0.0
        for sx in (-1,1):
            ax=cx+sx*(10.2+spread)*S+lean
            d.rounded_rectangle([ax-2.2*S,17*S,ax+2.2*S,27*S], radius=1.6*S, fill=AR, outline=OUT, width=S//2)
            d.ellipse([ax-2.4*S,26*S,ax+2.4*S,30.4*S], fill=ARD, outline=OUT, width=S//2)   # slab fist
    # full helm, visor slit — no face
    hx=cx+lean
    d.ellipse([hx-4.9*S,3*S,hx+4.9*S,14*S], fill=ARD, outline=OUT, width=S//2)
    d.ellipse([hx-3.4*S,4*S,hx-0.4*S,7*S], fill=AR)                                       # helm sheen
    d.rectangle([hx-3.6*S,8.4*S,hx+3.6*S,10.2*S], fill=(16,14,18,255))                    # visor slit
    vis = (240,70,50,255) if pose!="pain" else (255,150,60,255)
    for sx in (-1,1):
        d.ellipse([hx+sx*1.8*S-0.6*S,8.7*S,hx+sx*1.8*S+0.6*S,9.9*S], fill=vis)            # glowing eyes
    d.polygon([(hx-4.9*S,7*S),(hx-6.4*S,4.4*S),(hx-4.4*S,5.4*S)], fill=ARD, outline=OUT) # horn stubs
    d.polygon([(hx+4.9*S,7*S),(hx+6.4*S,4.4*S),(hx+4.4*S,5.4*S)], fill=ARD, outline=OUT)
    return down(im,CW,CH)

AD="/home/maustin/thumby-color/mote/games/wolfmote/assets"
POSES=["idle","fire","pain","dead"]
g=Image.new("RGBA",(24*4,40),(0,0,0,0))
for i,p in enumerate(POSES): g.paste(guard_frame(p),(i*24,0))
g.save(f"{AD}/guard.png")
b=Image.new("RGBA",(28*4,44),(0,0,0,0))
for i,p in enumerate(POSES): b.paste(brute_frame(p),(i*28,0))
b.save(f"{AD}/brute.png")
print("wrote guard.png + brute.png (redrawn)")
