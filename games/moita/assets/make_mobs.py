#!/usr/bin/env python3
"""Author the fixed-shape creature + barrel spritesheet -> assets/mobs.png.
Editable source for the Studio Pixel Art / Anim tab; `mote bake` -> src/mobs.h.
16x16 cells, 4 animation frames across, one creature per row. Transparent bg."""
from PIL import Image
import math

CELL=16; COLS=4
ROWS=["bat","spitter","slime","wisp","crystal","ghost","magmaw","brood","keg_oil","keg_bomb"]
img=Image.new("RGBA",(CELL*COLS,CELL*len(ROWS)),(0,0,0,0))

def cellpx(row,frame):
    ox,oy=frame*CELL,row*CELL
    def put(x,y,c):
        if 0<=x<CELL and 0<=y<CELL:
            img.putpixel((ox+x,oy+y),(c[0],c[1],c[2],255))
    return put

# fire ramp (for magmaw), matches the in-game lava/fire feel
def fire(t):  # t 0..1
    stops=[(0,(70,8,0)),(0.3,(210,45,0)),(0.55,(255,110,18)),(0.8,(255,190,70)),(1,(255,250,222))]
    for i in range(len(stops)-1):
        a,ca=stops[i]; b,cb=stops[i+1]
        if t<=b:
            f=(t-a)/(b-a+1e-6); return tuple(int(ca[j]+(cb[j]-ca[j])*f) for j in range(3))
    return stops[-1][1]

for row,name in enumerate(ROWS):
    for fr in range(COLS):
        P=cellpx(row,fr); cx,cy=8,8
        if name=="bat":
            b=(96,62,120); w=(150,105,170)
            fl=[2,1,0,1][fr]
            P(cx,cy,b); P(cx,cy-1,b)
            for s in (-1,1):
                P(cx+s,cy-fl+1,w); P(cx+2*s,cy-(fl-1),w); P(cx+3*s,cy-(2 if fl>1 else 0),w)
            P(cx,cy-2,(255,80,80))
        elif name=="spitter":
            for x in(-1,0,1): P(cx+x,cy+2,(50,110,40))
            P(cx,cy+1,(80,160,60)); P(cx-1,cy+1,(60,130,46)); P(cx+1,cy+1,(60,130,46))
            for x in(-2,-1,0,1,2): P(cx+x,cy-1,(150,60,190)); P(cx+x,cy-2,(120,44,160))
            op = fr in (1,2)
            P(cx,cy,(190,255,100) if op else (90,30,120))
            if op: P(cx-1,cy,(150,255,90)); P(cx+1,cy,(150,255,90))
        elif name=="slime":
            g=(92,200,68); gd=(58,140,42); hl=(160,255,140)
            sq=[0,1,1,0][fr]   # squash/stretch
            if sq:  # squashed wide
                for x in range(-3,4):
                    for y in range(0,3): P(cx+x,cy+y,g)
                for x in range(-3,4): P(cx+x,cy,hl if x<0 else g)
                P(cx-3,cy+2,gd);P(cx+3,cy+2,gd)
            else:   # tall
                for x in range(-2,3):
                    for y in range(-1,3): P(cx+x,cy+y,g)
                for x in range(-2,1): P(cx+x,cy-1,hl)
            P(cx-1,cy,(20,60,20)); P(cx+1,cy,(20,60,20))  # eyes
        elif name=="wisp":
            c=fire(0.9)
            P(cx,cy,(255,255,220))
            for dx,dy in((1,0),(-1,0),(0,1),(0,-1)): P(cx+dx,cy+dy,c)
            a=fr*1.57; ox=int(round(math.cos(a)*2.5)); oy=int(round(math.sin(a)*2.5))
            P(cx+ox,cy+oy,fire(0.7))
            P(cx+1,cy-2,(120,60,20))  # trailing ember
        elif name=="crystal":
            c1=(140,190,235); c2=(205,235,255); d=(60,90,130)
            P(cx,cy-2,c2); P(cx,cy-1,c2)
            P(cx-1,cy,c1); P(cx,cy,c2); P(cx+1,cy,c1)
            P(cx-1,cy+1,d); P(cx+1,cy+1,d); P(cx,cy+1,c1)
            if fr==0: P(cx,cy-2,(255,255,255))  # glint frame
            if fr==2: P(cx+1,cy,(255,255,255))
        elif name=="ghost":
            base=(210,215,235)
            for y in range(-3,2):
                for x in range(-2,3):
                    if x*x+((y+1)*(y+1))//2 <= 5: P(cx+x,cy+y,base)
            # wavy hem
            hem=[(-2,0),(0,1),(2,0)] if fr%2==0 else [(-1,1),(1,1)]
            for hx,hy in [(-2,2),(0,2),(2,2)]:
                if (hx//2 + fr)%2==0: P(cx+hx,cy+hy,base)
            P(cx-1,cy-1,(40,50,90)); P(cx+1,cy-1,(40,50,90))
        elif name=="magmaw":
            SPR=["....kkkkk....","..kkKKKKKkk..",".kKoooooooKk.",".koyyyyyyyok.",
                 "koyyeyyyeyyok","koyywwwwwyyok","koyywwwwwyyok",".koyywwwyyok.",
                 ".kKoyyyyyoKk.","..kKoooooKk..","...kK.o.Kk...","...K.....K..."]
            col={'K':(58,22,10),'k':(112,46,18),'e':(255,255,235)}
            churn=fr*0.9
            for r,rows in enumerate(SPR):
                for c,ch in enumerate(rows):
                    if ch=='.':continue
                    x=cx-6+c; y=cy-6+r
                    if ch in col: P(x,y,col[ch])
                    else:
                        base={'w':0.92,'y':0.72,'o':0.55}[ch]
                        t=base+0.12*math.sin(churn+x*0.6+y*0.8)
                        P(x,y,fire(max(0,min(1,t))))
        elif name=="brood":
            SPR=["...lgg..ggD...","..lhlggggggDd.",".lhhlgggggggDd","lhhllggggggggD",
                 "lhlllgEgggEgDD","llllggggggggDD","lgggggggggggDd","gggggggggggddd",
                 "DggggggggggdD.",".DDgggggggDD..","..dDD.DD.DDd.."]
            col={'D':(40,110,35),'d':(28,78,24),'g':(92,200,68),'l':(150,240,120),
                 'h':(205,255,186),'E':(16,50,16)}
            for r,rows in enumerate(SPR):
                off=int(round(math.sin(fr*1.4+r*0.55)*1.4*(1-r/11.0)))
                for c,ch in enumerate(rows):
                    if ch=='.':continue
                    P(cx-7+c+off, cy-5+r, col[ch])
            P(cx-4,cy-4,(232,255,222))  # gloss glint
        elif name in ("keg_oil","keg_bomb"):
            bomb = name=="keg_bomb"
            body=(150,44,38) if bomb else (70,78,48)
            hoop=(236,196,72) if bomb else (122,130,88)
            dark=(78,22,20) if bomb else (40,46,28)
            if fr in (2,3):  # fuse-lit flash frames
                k=0.6 if fr==2 else 0.3
                body=tuple(int(body[j]+(255-body[j])*k) for j in range(3))
            for y in range(-7,1):
                for x in range(-2,3):
                    c=body
                    if x in(-2,2) or y in(-7,0): c=dark
                    P(cx+x,cy+y,c)
            for x in range(-2,3): P(cx+x,cy-5,hoop); P(cx+x,cy-2,hoop)
            P(cx,cy-8,dark)
            P(cx-1,cy-4,tuple(int(body[j]+(255-body[j])*0.4) for j in range(3)))  # sheen
            if fr in (2,3): P(cx,cy-9,(255,230,120))  # fuse spark

img.save("/home/maustin/thumby-color/mote/games/moita/assets/mobs.png")
print("wrote mobs.png", img.size)
