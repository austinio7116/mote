#!/usr/bin/env python3
"""Software-render each decimated OBJ (textured, lit) into a contact sheet."""
import os, math, glob
from PIL import Image, ImageDraw, ImageFont

SP = os.path.dirname(os.path.abspath(__file__))
ASSETS = '/home/maustin/thumby-color/mote/games/galaxyswarm/assets'
CELL = 200

def load_obj(path):
    vs, vts, faces = [], [], []
    for line in open(path):
        p = line.split()
        if not p: continue
        if p[0] == 'v': vs.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == 'vt': vts.append((float(p[1]), float(p[2])))
        elif p[0] == 'f':
            idx = []
            for w in p[1:]:
                sub = w.split('/')
                vi = int(sub[0]); vi = vi - 1 if vi > 0 else len(vs) + vi
                ti = -1
                if len(sub) > 1 and sub[1]:
                    ti = int(sub[1]); ti = ti - 1 if ti > 0 else len(vts) + ti
                idx.append((vi, ti))
            for j in range(1, len(idx) - 1):
                faces.append((idx[0], idx[j], idx[j+1]))
    return vs, vts, faces

def render(vs, vts, faces, tex, yaw=0.6, pitch=0.45):
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    def xf(v):
        x, y, z = v
        x, z = x*cy + z*sy, -x*sy + z*cy
        y, z = y*cp - z*sp, y*sp + z*cp
        return (x, y, z)
    tvs = [xf(v) for v in vs]
    img = Image.new('RGB', (CELL, CELL), (16, 18, 30))
    px = img.load()
    zbuf = [[-1e9]*CELL for _ in range(CELL)]
    tw = th = 0
    tpx = None
    if tex:
        tpx = tex.load(); tw, th = tex.size
    L = (0.45, 0.7, -0.55)
    ll = math.sqrt(sum(c*c for c in L)); L = tuple(c/ll for c in L)
    S = CELL*0.42; OX = OY = CELL/2
    for f in faces:
        p = [tvs[f[k][0]] for k in range(3)]
        # normal (screen-space cull via z of cross)
        ux, uy, uz = (p[1][i]-p[0][i] for i in range(3))
        wx, wy, wz = (p[2][i]-p[0][i] for i in range(3))
        n = (uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx)
        nl = math.sqrt(sum(c*c for c in n)) or 1
        n = tuple(c/nl for c in n)
        if n[2] >= 0: continue
        lit = max(0.25, min(1.0, 0.25 + 0.85*max(0.0, -(n[0]*L[0]+n[1]*L[1]+n[2]*L[2]))))
        scr = [(OX + q[0]*S, OY - q[1]*S, q[2]) for q in p]
        uv = []
        for k in range(3):
            ti = f[k][1]
            uv.append(vts[ti] if 0 <= ti < len(vts) else (0, 0))
        x0 = max(0, int(min(s[0] for s in scr))); x1 = min(CELL-1, int(max(s[0] for s in scr))+1)
        y0 = max(0, int(min(s[1] for s in scr))); y1 = min(CELL-1, int(max(s[1] for s in scr))+1)
        ax, ay = scr[0][0], scr[0][1]
        bx, by = scr[1][0], scr[1][1]
        cx2, cy2 = scr[2][0], scr[2][1]
        den = (by-cy2)*(ax-cx2) + (cx2-bx)*(ay-cy2)
        if abs(den) < 1e-9: continue
        for yy in range(y0, y1+1):
            for xx in range(x0, x1+1):
                l0 = ((by-cy2)*(xx-cx2) + (cx2-bx)*(yy-cy2)) / den
                l1 = ((cy2-ay)*(xx-cx2) + (ax-cx2)*(yy-cy2)) / den
                l2 = 1 - l0 - l1
                if l0 < -0.001 or l1 < -0.001 or l2 < -0.001: continue
                z = l0*scr[0][2] + l1*scr[1][2] + l2*scr[2][2]
                if z <= zbuf[yy][xx]: continue
                zbuf[yy][xx] = z
                if tpx:
                    u = l0*uv[0][0] + l1*uv[1][0] + l2*uv[2][0]
                    v = l0*uv[0][1] + l1*uv[1][1] + l2*uv[2][1]
                    tu = min(tw-1, max(0, int((u % 1.0)*tw)))
                    tv = min(th-1, max(0, int(((1-v) % 1.0)*th)))
                    c = tpx[tu, tv]
                else:
                    c = (150, 155, 165)
                px[xx, yy] = (int(c[0]*lit), int(c[1]*lit), int(c[2]*lit))
    return img

names = sorted(os.path.basename(f)[:-4] for f in glob.glob(os.path.join(ASSETS, '*.obj')))
cols = 5
rows = (len(names) + cols - 1) // cols
sheet = Image.new('RGB', (cols*CELL, rows*(CELL+18)), (8, 8, 14))
d = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    vs, vts, faces = load_obj(os.path.join(ASSETS, n + '.obj'))
    texp = os.path.join(ASSETS, n + '.png')
    tex = Image.open(texp).convert('RGB') if os.path.exists(texp) else None
    im = render(vs, vts, faces, tex)
    x, y = (i % cols)*CELL, (i // cols)*(CELL+18)
    sheet.paste(im, (x, y))
    d.text((x+6, y+CELL+2), '%s (%dt%s)' % (n, len(faces), '' if tex else ' flat'), fill=(220, 220, 220))
out = os.path.join(SP, 'preview_models.png')
sheet.save(out)
print(out)
