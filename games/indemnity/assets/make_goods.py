#!/usr/bin/env python3
"""Generate goods.png — a 16x16 commodity-icon spritesheet (10 cols x 2 rows,
20 icons) for Indemnity Run's market. Edit this or the PNG in Mote Studio.
Icon order matches k_goods[] in econ.c."""
from PIL import Image, ImageDraw

CW = CH = 16
COLS, ROWS = 10, 2
N = 20
img = Image.new("RGBA", (CW * COLS, CH * ROWS), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

def cell(i):
    return (i % COLS) * CW, (i // COLS) * CH

def P(i, x, y, c):
    ox, oy = cell(i)
    if 0 <= x < CW and 0 <= y < CH:
        img.putpixel((ox + x, oy + y), c)

def rect(i, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            P(i, x, y, c)

def line(i, x0, y0, x1, y1, c):
    ox, oy = cell(i)
    d.line((ox + x0, oy + y0, ox + x1, oy + y1), fill=c)

def ellipse(i, x0, y0, x1, y1, c, outline=None):
    ox, oy = cell(i)
    d.ellipse((ox + x0, oy + y0, ox + x1, oy + y1), fill=c, outline=outline)

def poly(i, pts, c, outline=None):
    ox, oy = cell(i)
    d.polygon([(ox + x, oy + y) for x, y in pts], fill=c, outline=outline)

# palette
GOLD=(230,190,70,255); WHEAT=(210,170,90,255)
VIOL=(170,120,220,255); VIOL2=(120,80,170,255)
BLUE=(80,150,240,255); AMBER=(220,150,60,255); AMBER2=(150,90,30,255)
RED=(230,80,70,255); WHITE=(230,235,245,255)
TEAL=(70,200,210,255); GREEN=(110,220,130,255); GREEN2=(70,160,90,255)
STEEL=(150,160,180,255); STEEL2=(100,110,130,255)
SILVER=(190,200,215,255); BRONZE=(200,140,80,255)
GREY=(140,145,160,255); CYAN=(120,235,235,255)
MAG=(230,110,220,255); ORANGE=(240,150,60,255)
DARK=(70,75,90,255)

def i_grain(i):  # 0 FOOD
    line(i,8,4,8,13,WHEAT)
    for k,y in enumerate(range(4,12,2)):
        P(i,7,y,GOLD); P(i,6,y+1,GOLD); P(i,9,y,GOLD); P(i,10,y+1,GOLD)
    P(i,8,3,GOLD)

def i_cloth(i):  # 1 TEXTILES
    rect(i,3,5,12,11,VIOL)
    rect(i,3,5,4,11,VIOL2)
    line(i,6,5,4,11,WHITE); line(i,9,5,7,11,WHITE)

def i_drop(i,col):  # droplet
    poly(i,[(8,3),(12,10),(10,13),(6,13),(4,10)],col)
    ellipse(i,5,8,11,13,col)
    P(i,7,7,WHITE)

def i_bottle(i):  # 3 LIQUOR
    rect(i,7,3,9,6,AMBER2)          # neck
    poly(i,[(6,6),(10,6),(11,13),(5,13)],AMBER)
    rect(i,5,9,11,11,AMBER2)        # label

def i_gem(i,col,br=WHITE):  # diamond
    poly(i,[(8,3),(13,7),(8,14),(3,7)],col)
    line(i,3,7,13,7,br); line(i,8,3,8,14,br)

def i_cross(i):  # 5 MEDICINE
    ellipse(i,2,2,13,13,WHITE)
    rect(i,7,5,8,10,RED); rect(i,5,7,10,8,RED)

def i_chip(i,col):  # computers/electronics
    rect(i,4,4,11,11,col)
    rect(i,6,6,9,9,DARK)
    for x in (5,8,10):
        P(i,x,2,STEEL2); P(i,x,3,STEEL2); P(i,x,12,STEEL2); P(i,x,13,STEEL2)
    for y in (5,8,10):
        P(i,2,y,STEEL2); P(i,3,y,STEEL2); P(i,12,y,STEEL2); P(i,13,y,STEEL2)

def i_gear(i):  # 8 MACHINERY
    ellipse(i,3,3,12,12,STEEL)
    ellipse(i,6,6,9,9,DARK)
    for a in ((8,1),(8,14),(1,8),(14,8),(3,3),(12,3),(3,12),(12,12)):
        P(i,a[0],a[1],STEEL2);
    rect(i,7,1,8,3,STEEL); rect(i,7,12,8,14,STEEL)
    rect(i,1,7,3,8,STEEL); rect(i,12,7,14,8,STEEL)

def i_wrench(i):  # 9 TOOLS
    line(i,4,12,11,5,STEEL); line(i,5,12,12,5,STEEL2)
    poly(i,[(10,3),(13,4),(12,7),(9,6)],STEEL)
    poly(i,[(3,10),(6,11),(5,14),(2,13)],STEEL)

def i_ingot(i,col):  # 10/11 metals
    poly(i,[(4,7),(12,7),(14,12),(2,12)],col)
    line(i,4,7,12,7,WHITE)

def i_crystal(i):  # 12 MINERALS
    poly(i,[(5,13),(4,7),(7,5),(8,13)],GREY)
    poly(i,[(8,13),(9,6),(12,8),(11,13)],STEEL)
    P(i,7,5,WHITE); P(i,9,6,WHITE)

def i_battery(i):  # 13 FUELCELLS
    rect(i,3,5,12,12,GREEN2)
    rect(i,4,6,11,11,GREEN)
    rect(i,6,3,9,5,STEEL2)
    P(i,7,8,DARK); P(i,8,8,DARK); rect(i,6,8,10,8,DARK); rect(i,8,6,8,10,DARK)

def i_flask(i):  # 14 HYDROGEN
    rect(i,7,3,9,6,CYAN)
    poly(i,[(6,6),(10,6),(13,13),(3,13)],CYAN)
    line(i,3,13,13,13,WHITE)

def i_pills(i):  # 16 NARCOTICS
    ellipse(i,3,6,9,12,MAG)
    ellipse(i,7,4,13,10,WHITE,outline=MAG)
    line(i,7,7,13,7,MAG)

def i_bullet(i):  # 17 WEAPONS
    poly(i,[(8,2),(11,6),(11,12),(5,12),(5,6)],ORANGE)
    rect(i,5,12,11,14,AMBER2)
    P(i,8,4,WHITE)

def i_figure(i):  # 18 SLAVES
    ellipse(i,6,3,10,7,GREY)
    poly(i,[(5,8),(11,8),(12,14),(4,14)],GREY)
    line(i,3,9,4,11,STEEL2); line(i,13,9,12,11,STEEL2)  # chains

def i_crate(i):  # 19 CONTRABAND
    rect(i,3,4,12,13,BRONZE)
    line(i,3,4,12,13,AMBER2); line(i,12,4,3,13,AMBER2)
    rect(i,7,6,8,10,RED); P(i,7,12,RED); P(i,8,12,RED)

i_grain(0)
i_cloth(1)
i_drop(2,BLUE)
i_bottle(3)
i_gem(4,GOLD)
i_cross(5)
i_chip(6,TEAL)
i_chip(7,GREEN)
i_gear(8)
i_wrench(9)
i_ingot(10,SILVER)
i_ingot(11,BRONZE)
i_crystal(12)
i_battery(13)
i_flask(14)
i_gem(15,CYAN)
i_pills(16)
i_bullet(17)
i_figure(18)
i_crate(19)

import os
out = os.path.join(os.path.dirname(__file__), "goods.png")
img.save(out)
print("wrote", out, img.size)
