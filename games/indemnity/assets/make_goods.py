#!/usr/bin/env python3
"""Generate goods.png — a 12x12 commodity-icon spritesheet (10 cols x 2 rows,
20 icons) for Indemnity Run's market. Icons are drawn NATIVELY at 12px (not
downscaled). Edit this or the PNG in Mote Studio. Order matches k_goods[]."""
from PIL import Image, ImageDraw

CW = CH = 12
COLS, ROWS = 10, 2
img = Image.new("RGBA", (CW * COLS, CH * ROWS), (0, 0, 0, 0))

def cell(i): return (i % COLS) * CW, (i // COLS) * CH

def P(i, x, y, c):
    ox, oy = cell(i)
    if 0 <= x < CW and 0 <= y < CH: img.putpixel((ox + x, oy + y), c)
def rect(i, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1): P(i, x, y, c)
def line(i, x0, y0, x1, y1, c):
    ox, oy = cell(i); ImageDraw.Draw(img).line((ox+x0, oy+y0, ox+x1, oy+y1), fill=c)
def ell(i, x0, y0, x1, y1, c, o=None):
    ox, oy = cell(i); ImageDraw.Draw(img).ellipse((ox+x0, oy+y0, ox+x1, oy+y1), fill=c, outline=o)
def poly(i, pts, c, o=None):
    ox, oy = cell(i); ImageDraw.Draw(img).polygon([(ox+x, oy+y) for x, y in pts], fill=c, outline=o)

GOLD=(232,192,72,255); WHEAT=(206,166,88,255)
VIOL=(176,126,224,255); VIOL2=(120,80,170,255)
BLUE=(84,152,240,255); AMBER=(222,150,60,255); AMBER2=(150,92,34,255)
RED=(232,80,70,255); WHITE=(232,236,246,255)
TEAL=(72,202,212,255); GREEN=(112,222,132,255); GREEN2=(64,150,84,255)
STEEL=(154,164,184,255); STEEL2=(104,112,132,255)
SILVER=(196,206,220,255); BRONZE=(202,142,82,255)
GREY=(146,150,166,255); CYAN=(124,236,236,255)
MAG=(232,112,222,255); ORANGE=(242,152,62,255); DARK=(60,64,78,255)

def grain(i):
    line(i,6,3,6,10,WHEAT)
    for y in (3,5,7): P(i,5,y,GOLD); P(i,7,y,GOLD)
    for y in (4,6,8): P(i,6,y,GOLD)
    P(i,6,2,GOLD)
def cloth(i):
    rect(i,2,4,9,9,VIOL); rect(i,2,4,3,9,VIOL2)
    line(i,5,4,3,9,WHITE); line(i,8,4,6,9,WHITE)
def drop(i,c):
    poly(i,[(6,2),(9,7),(8,10),(4,10),(3,7)],c); P(i,5,5,WHITE)
def bottle(i):
    rect(i,5,2,6,4,AMBER2); poly(i,[(4,4),(7,4),(8,10),(3,10)],AMBER); rect(i,3,7,8,8,AMBER2)
def gem(i,c):
    poly(i,[(6,2),(10,5),(6,11),(2,5)],c); line(i,2,5,10,5,WHITE); line(i,6,2,6,11,WHITE)
def cross(i):
    ell(i,1,1,10,10,WHITE); rect(i,5,3,6,8,RED); rect(i,3,5,8,6,RED)
def chip(i,c):
    rect(i,3,3,8,8,c); rect(i,5,5,6,6,DARK)
    for x in (4,7): P(i,x,1,STEEL2); P(i,x,2,STEEL2); P(i,x,9,STEEL2); P(i,x,10,STEEL2)
    for y in (4,7): P(i,1,y,STEEL2); P(i,2,y,STEEL2); P(i,9,y,STEEL2); P(i,10,y,STEEL2)
def gear(i):
    ell(i,2,2,9,9,STEEL); ell(i,4,4,7,7,DARK)
    rect(i,5,0,6,2,STEEL); rect(i,5,9,6,11,STEEL); rect(i,0,5,2,6,STEEL); rect(i,9,5,11,6,STEEL)
    P(i,2,2,STEEL); P(i,9,2,STEEL); P(i,2,9,STEEL); P(i,9,9,STEEL)
def wrench(i):
    line(i,3,9,8,4,STEEL); line(i,4,9,9,4,STEEL2)
    poly(i,[(7,2),(10,3),(9,6),(6,5)],STEEL); poly(i,[(2,7),(5,8),(4,11),(1,10)],STEEL)
def ingot(i,c):
    poly(i,[(3,5),(8,5),(10,9),(1,9)],c); line(i,3,5,8,5,WHITE)
def crystal(i):
    poly(i,[(4,10),(3,5),(6,3),(6,10)],GREY); poly(i,[(6,10),(7,4),(9,6),(8,10)],STEEL)
    P(i,6,3,WHITE); P(i,7,4,WHITE)
def battery(i):
    rect(i,2,4,9,10,GREEN2); rect(i,3,5,8,9,GREEN); rect(i,5,2,7,4,STEEL2)
    rect(i,5,7,7,7,DARK); rect(i,6,6,6,8,DARK)
def flask(i):
    rect(i,5,2,6,4,CYAN); poly(i,[(4,4),(7,4),(9,10),(2,10)],CYAN); line(i,2,10,9,10,WHITE)
def pills(i):
    ell(i,1,5,6,10,MAG); ell(i,5,2,10,7,WHITE,MAG); line(i,5,7,9,7,MAG)
def bullet(i):
    poly(i,[(6,1),(9,5),(9,9),(3,9),(3,5)],ORANGE); rect(i,3,9,9,10,AMBER2); P(i,6,3,WHITE)
def figure(i):
    ell(i,4,2,7,5,GREY); poly(i,[(3,6),(8,6),(9,11),(2,11)],GREY)
def crate(i):
    rect(i,2,3,9,10,BRONZE); line(i,2,3,9,10,AMBER2); line(i,9,3,2,10,AMBER2)
    rect(i,5,5,6,8,RED); P(i,5,9,RED); P(i,6,9,RED)

grain(0); cloth(1); drop(2,BLUE); bottle(3); gem(4,GOLD); cross(5)
chip(6,TEAL); chip(7,GREEN); gear(8); wrench(9); ingot(10,SILVER); ingot(11,BRONZE)
crystal(12); battery(13); flask(14); gem(15,CYAN); pills(16); bullet(17); figure(18); crate(19)

import os
out = os.path.join(os.path.dirname(__file__), "goods.png")
img.save(out); print("wrote", out, img.size)
