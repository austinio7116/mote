#!/usr/bin/env python3
"""citygen — procedural GrandThumbAuto city maps in the game palette.

Generates a city.png-compatible map (1 px = 1 tile, same 8 colours) with the
structural grammar of the hand-made original:
  · 1-2 meandering diagonal RIVERS with lakes, stepped shores and grass banks
  · a jogging ARTERIAL ring/grid (3 wide) that BRIDGES water, then dense
    local streets subdividing every district into small blocks
  · blocks = pavement speckled with building slabs; district noise clusters
    building types (brick / office / tower) so neighbourhoods feel coherent
  · a central park (pond + paths) plus scattered pocket parks
  · a connectivity pass guarantees the whole road network is one component

    python3 tools/citygen.py [seed] [out.png ...]
    python3 tools/citygen.py 7 /tmp/city7.png

NEVER writes over assets/city.png — the hand-made map stays untouched.
"""
import sys, random, math
import numpy as np
from PIL import Image

W, H = 254, 256
ROAD, PAVE, GRASS, WATER, BLO, BMID, BHI, BRIDGE = '.',',',' ','~','#','O','H','B'
COL = {ROAD:(70,70,78), PAVE:(142,140,128), GRASS:(94,130,51), WATER:(58,150,168),
       BLO:(150,80,70), BMID:(100,110,130), BHI:(118,118,128), BRIDGE:(120,92,64)}

def fbm(w, h, rng, oct=4, base=8):
    """cheap value-noise fBm in [0,1]"""
    out = np.zeros((h, w))
    amp, tot = 1.0, 0.0
    for o in range(oct):
        gw, gh = base*(2**o)+2, base*(2**o)+2
        g = rng.random((gh, gw))
        ys = np.linspace(0, gh-1.001, h); xs = np.linspace(0, gw-1.001, w)
        y0 = ys.astype(int); x0 = xs.astype(int)
        fy = (ys-y0)[:,None]; fx = (xs-x0)[None,:]
        a = g[y0][:,x0]; b = g[y0][:,x0+1]; c = g[y0+1][:,x0]; d = g[y0+1][:,x0+1]
        out += amp*((a*(1-fx)+b*fx)*(1-fy) + (c*(1-fx)+d*fx)*fy)
        tot += amp; amp *= 0.5
    return out/tot

def patch(m, rng, cx, cy, size, ch, only=None):
    """an ANGULAR organic blob (union of offset rectangles + ragged edge) — the
    hand map never draws circles"""
    cells=set()
    nr = 3 + int(size//12)
    for _ in range(nr):
        w2 = int(rng.integers(max(2,size//3), max(3,int(size*0.8))))
        h2 = int(rng.integers(max(2,size//3), max(3,int(size*0.8))))
        ox = int(cx + rng.integers(-size//2, size//2+1) - w2//2)
        oy = int(cy + rng.integers(-size//2, size//2+1) - h2//2)
        for y in range(oy, oy+h2):
            for x in range(ox, ox+w2):
                if 0<=x<W and 0<=y<H: cells.add((x,y))
    for x,y in list(cells):                                # ragged edge
        n = sum((nx,ny) in cells for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)))
        if n<=2 and rng.random()<0.45: cells.discard((x,y))
    for x,y in cells:
        if only is None or m[y][x] in only: m[y][x]=ch
    return cells

def disc(m, cx, cy, r, ch):
    x0,x1 = max(0,int(cx-r)), min(W,int(cx+r+1))
    y0,y1 = max(0,int(cy-r)), min(H,int(cy+r+1))
    for y in range(y0,y1):
        for x in range(x0,x1):
            if (x-cx)**2+(y-cy)**2 <= r*r: m[y][x]=ch

# ---------------------------------------------------------------- water ----
def carve_rivers(m, rng):
    """v3 water with river PERSONALITY:
      · main channels flow edge-to-edge with intent (steered at an exit point),
        held angular headings, and a width that swells from a narrow upper
        reach to a broad ESTUARY mouth
      · lateral drift + one-sided COVES break the brush-stroke symmetry
      · tapered TRIBUTARIES join at widened confluence pools
      · forks split around real islands and rejoin
      · an erosion pass steps the banks"""
    import math

    def channel(x, y, exit_pt, w0, w1, steps, forks=1, tribs=2, taper_end=False):
        ang = math.atan2(exit_pt[1]-y, exit_pt[0]-x)
        hold = 0; turn = 0.0; off = 0.0; woff = rng.random()*7
        for i in range(steps):
            t = i/steps
            if not (-30 < x < W+30 and -30 < y < H+30): break
            # width: upper reach -> mouth, with slow noise swells and pinches
            w = (w0 + (w1-w0)*t) * (0.70 + 0.55*abs(math.sin(t*9 + woff)))
            if taper_end and t > 0.65: w *= max(0.0, 1.0 - (t-0.65)/0.32)  # dies to a POINT
            if w < 0.9: break
            px2, py2 = -math.sin(ang), math.cos(ang)
            disc(m, x + px2*off, y + py2*off, w, WATER)
            if rng.random() < 0.030:                              # one-sided COVE
                side = rng.choice([-1,1])
                disc(m, x + px2*(off + side*w*0.85), y + py2*(off + side*w*0.85),
                     w*rng.uniform(0.45, 0.75), WATER)
            # angular heading: hold, then turn a chunk; always re-aim at the exit
            if hold > 0: hold -= 1
            else:
                turn = rng.choice([-1,1]) * rng.uniform(0.15, 0.45)
                hold = int(rng.integers(10, 26))
            bearing = math.atan2(exit_pt[1]-y, exit_pt[0]-x)
            d = (bearing - ang + math.pi) % (2*math.pi) - math.pi
            ang += d*0.045 + turn*0.055 + rng.uniform(-0.02, 0.02)
            if 0.1 < t < 0.85:                                    # mid-course: shy from the borders
                cx2 = min(x, W-x); cy2 = min(y, H-y)
                if cx2 < 26 or cy2 < 26:
                    toc = math.atan2(H/2-y, W/2-x)
                    dd2 = (toc - ang + math.pi) % (2*math.pi) - math.pi
                    ang += dd2*0.06
            off = max(-4, min(4, off + rng.uniform(-0.35, 0.35)))  # bank drift
            x += math.cos(ang)*2.0; y += math.sin(ang)*2.0
            # FORK around an island, rejoining downstream
            if forks and rng.random() < 0.012 and 0.15 < t < 0.7:
                forks -= 1
                sep = rng.uniform(9, 24); dur = int(rng.integers(35, 110))
                bx, by = x + px2*sep, y + py2*sep
                bang = ang
                for j in range(dur):
                    fw = w*0.6 * (0.8 + 0.4*math.sin(j*0.2))
                    disc(m, bx, by, max(2.5, fw), WATER)
                    tgt = math.atan2((y+math.sin(ang)*2*(j+5))-by, (x+math.cos(ang)*2*(j+5))-bx)
                    dd = (tgt - bang + math.pi) % (2*math.pi) - math.pi
                    bang += dd*(0.14 if j > dur*0.5 else 0.02) + rng.uniform(-0.04,0.04)
                    bx += math.cos(bang)*2.0; by += math.sin(bang)*2.0
            # TRIBUTARY: joins here at a confluence pool, tapers away upstream
            if tribs and rng.random() < 0.018 and t > 0.25:
                tribs -= 1
                disc(m, x, y, w*1.25, WATER)                      # confluence pool
                tang = ang + math.pi + rng.choice([-1,1])*rng.uniform(0.5, 1.1)
                tx, ty = x, y
                tsteps = int(rng.integers(40, 110))
                texit = (tx + math.cos(tang)*tsteps*2.2, ty + math.sin(tang)*tsteps*2.2)
                channel(tx, ty, texit, w*0.55, 1.4, tsteps, forks=0, tribs=0, taper_end=True)
        return x, y

    edges = lambda: rng.choice([0,1,2,3])
    def edge_pt(e, lo=20, hi=None):
        hi = hi or (W-20 if e in (0,1) else H-20)
        p = int(rng.integers(lo, hi))
        return {0:(p,-8), 1:(p,H+8), 2:(-8,p), 3:(W+8,p)}[e]
    e_in = edges(); e_out = (e_in + rng.choice([1,2,3])) % 4
    start, exitp = edge_pt(e_in), edge_pt(e_out)
    channel(start[0], start[1], exitp, rng.uniform(3.5,5), rng.uniform(11,15), 250, forks=2, tribs=2)
    if rng.random() < 0.6:                                        # a second, lesser river
        e2 = (e_out + 2) % 4
        s2, x2 = edge_pt(e2), edge_pt((e2+2)%4)
        channel(s2[0], s2[1], x2, rng.uniform(3,4.5), rng.uniform(7,10), 200, forks=1, tribs=1)
    for _ in range(int(rng.integers(0,3))):                       # lakes: angular, never round
        lx,ly = int(rng.integers(30,W-30)), int(rng.integers(30,H-30))
        patch(m, rng, lx, ly, int(rng.integers(10,18)), WATER)
    # RAGGED SHORES
    for _ in range(2):
        wm = [[m[y][x]==WATER for x in range(W)] for y in range(H)]
        for y in range(1,H-1):
            for x in range(1,W-1):
                n = wm[y-1][x]+wm[y+1][x]+wm[y][x-1]+wm[y][x+1]
                if not wm[y][x] and n>=2 and rng.random()<0.28: m[y][x]=WATER
                elif wm[y][x] and n<=2 and rng.random()<0.20:  m[y][x]=PAVE
    # lumpy islands inside open water
    from scipy import ndimage
    wm = np.array([[m[y][x]==WATER for x in range(W)] for y in range(H)])
    dist = ndimage.distance_transform_edt(wm)
    ys,xs = np.where(dist > 6)
    for _ in range(int(rng.integers(2,5))):
        if len(ys)==0: break
        k = int(rng.integers(0,len(ys)))
        cx,cy = int(xs[k]), int(ys[k])
        big = rng.random() < 0.35
        patch(m, rng, cx, cy, int(rng.integers(13,24)) if big else int(rng.integers(6,11)),
              GRASS, only=(WATER,))

def grass_banks(m, rng):
    """CLUMPED bank grass — the original pours pavement right to the water and
    greens only stretches of shore; a noise gate picks which stretches."""
    wm = np.array([[m[y][x]==WATER for x in range(W)] for y in range(H)])
    from scipy import ndimage
    d = ndimage.distance_transform_edt(~wm)
    clump = fbm(W, H, rng, oct=3, base=7)
    for y in range(H):
        for x in range(W):
            if wm[y][x] or m[y][x] != PAVE: continue
            if d[y][x] < 2.5 and clump[y][x] > 0.52 and rng.random()<0.9: m[y][x]=GRASS
            elif d[y][x] < 4 and clump[y][x] > 0.62 and rng.random()<0.55: m[y][x]=GRASS

def esplanades(m, rng):
    """roads that trace the riverbanks — the original's grid meets the water
    with streets running along the shore."""
    from scipy import ndimage
    wm = np.array([[m[y][x]==WATER for x in range(W)] for y in range(H)])
    d = ndimage.distance_transform_edt(~wm)
    band = (d >= 4.0) & (d < 6.2)
    for y in range(H):
        for x in range(W):
            if band[y][x] and m[y][x] in (PAVE,GRASS): m[y][x]=ROAD

# ---------------------------------------------------------------- roads ----
def stamp_road(m, x, y, wid, bridging):
    for dy in range(wid):
        for dx in range(wid):
            xx,yy = x+dx, y+dy
            if 0<=xx<W and 0<=yy<H:
                if m[yy][xx]==WATER:
                    if bridging: m[yy][xx]=BRIDGE
                else: m[yy][xx]=ROAD

def jog_line(m, rng, vertical, pos, wid=4):
    p = pos; t = 0
    while t < (H if vertical else W):
        seg = int(rng.integers(40, 90))
        for s in range(seg):
            if t >= (H if vertical else W): break
            if vertical: stamp_road(m, p, t, wid, True)
            else:        stamp_road(m, t, p, wid, True)
            t += 1
        if t < (H if vertical else W):
            jog = int(rng.integers(-14, 15))
            lo, hi = (min(p, p+jog), max(p, p+jog+wid-1))
            for q in range(lo, hi+1):
                if vertical: stamp_road(m, q, t, wid, True)
                else:        stamp_road(m, t, q, wid, True)
            t += wid
            p = max(10, min((W if vertical else H)-wid-10, p+jog))

def ring_road(m, rng):
    """the original's strongest signature: a thick rectangular ring with jogged
    corners enclosing the city core"""
    inset = int(rng.integers(26, 44))
    x0,y0 = inset+int(rng.integers(-8,9)), inset+int(rng.integers(-8,9))
    x1,y1 = W-inset+int(rng.integers(-8,9)), H-inset+int(rng.integers(-8,9))
    wid=4
    for x in range(x0, x1): stamp_road(m, x, y0, wid, True); stamp_road(m, x, y1-wid, wid, True)
    for y in range(y0, y1): stamp_road(m, x0, y, wid, True); stamp_road(m, x1-wid, y, wid, True)

def arterials(m, rng):
    ring_road(m, rng)
    nv = int(rng.integers(2, 4)); nh = int(rng.integers(2, 4))
    xs = sorted(int(W*(i+1)/(nv+1) + rng.integers(-20,21)) for i in range(nv))
    ys = sorted(int(H*(i+1)/(nh+1) + rng.integers(-20,21)) for i in range(nh))
    for x in xs: jog_line(m, rng, True, x)
    for y in ys: jog_line(m, rng, False, y)

def local_streets(m, rng):
    """per-district BSP: irregular block sizes, denser downtown, sparser edges"""
    dense = fbm(W, H, rng, oct=3, base=4)
    def split(cset, x0,y0,x1,y1, depth):
        w,h = x1-x0, y1-y0
        cx,cy = (x0+x1)//2, (y0+y1)//2
        stop = 13 + int(14*(1.0-dense[min(cy,H-1)][min(cx,W-1)]))  # 13..27
        if depth >= 1 and rng.random() < 0.16: return              # leave a SUPERBLOCK
        if w < stop and h < stop: return
        horiz = h > w if abs(h-w) > 3 else rng.random()<0.5
        wid = 2
        if horiz and h < 8: horiz=False
        if not horiz and w < 8: return
        if horiz:
            cut = y0 + int(rng.integers(4, max(5, h-4)))
            for x in range(x0, x1+1):
                for dq in range(wid):
                    if (x,cut+dq) in cset: m[cut+dq][x]=ROAD
            split(cset,x0,y0,x1,cut,depth+1); split(cset,x0,cut+wid,x1,y1,depth+1)
        else:
            cut = x0 + int(rng.integers(4, max(5, w-4)))
            for y in range(y0, y1+1):
                for dq in range(wid):
                    if (cut+dq,y) in cset: m[y][cut+dq]=ROAD
            split(cset,x0,y0,cut,y1,depth+1); split(cset,cut+wid,y0,x1,y1,depth+1)
    seen=[[False]*W for _ in range(H)]
    for sy in range(H):
        for sx in range(W):
            if seen[sy][sx] or m[sy][sx] != PAVE: continue
            stack=[(sx,sy)]; seen[sy][sx]=True; cells=[]
            while stack:
                x,y=stack.pop(); cells.append((x,y))
                for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
                    if 0<=nx<W and 0<=ny<H and not seen[ny][nx] and m[ny][nx]==PAVE:
                        seen[ny][nx]=True; stack.append((nx,ny))
            if len(cells) < 100: continue
            xs=[c[0] for c in cells]; ys=[c[1] for c in cells]
            split(set(cells), min(xs),min(ys),max(xs),max(ys), 0)

# ---------------------------------------------------------------- blocks ---
def fill_blocks(m, rng):
    """blocks packed with many SMALL individual buildings (1-4 x 1-3), abutting
    each other with pavement seams where the packing misses; each building is a
    single type, district noise setting the local red/grey/blue mix."""
    kind  = fbm(W, H, rng, oct=3, base=5)
    dense = fbm(W, H, rng, oct=3, base=4)
    rm = np.array([[m[y][x] in (ROAD,BRIDGE) for x in range(W)] for y in range(H)])
    from scipy import ndimage
    droad = ndimage.distance_transform_edt(~rm)
    anchors = [(x,y) for y in range(H) for x in range(W)]
    rng.shuffle(anchors)
    for x,y in anchors:
        if m[y][x] != PAVE or droad[y][x] < 2: continue
        d = dense[y][x]
        if rng.random() > 0.60 + 0.35*d: continue
        bw,bh = int(rng.integers(1,5)), int(rng.integers(1,4))
        cand=[(x+i,y+j) for i in range(bw) for j in range(bh)]
        ok=True
        for cx,cy in cand:
            if not (0<=cx<W and 0<=cy<H) or m[cy][cx]!=PAVE or droad[cy][cx]<2:
                ok=False; break
        if not ok: continue
        k = kind[y][x] + rng.uniform(-0.18,0.18)
        ch = BHI if k>0.60 else BMID if k>0.42 else BLO
        for cx,cy in cand: m[cy][cx]=ch
    # infill: the little 1-2 tile buildings that crowd the leftover gaps
    for x,y in anchors:
        if m[y][x] != PAVE or droad[y][x] < 2: continue
        d = dense[y][x]
        if rng.random() > 0.45 + 0.40*d: continue
        k = kind[y][x] + rng.uniform(-0.22,0.22)
        ch = BHI if k>0.60 else BMID if k>0.42 else BLO
        m[y][x]=ch
        if rng.random()<0.5:
            nx,ny = x+int(rng.integers(0,2)), y+int(rng.integers(0,2))
            if 0<=nx<W and 0<=ny<H and m[ny][nx]==PAVE and droad[ny][nx]>=2: m[ny][nx]=ch

def micro_details(m, rng):
    """the hand map's small-scale life: 1-wide grass VERGES along streets,
    little GARDEN squares inside blocks, and tiny POOLS in the suburbs."""
    rm = np.array([[m[y][x] in (ROAD,BRIDGE) for x in range(W)] for y in range(H)])
    from scipy import ndimage
    droad = ndimage.distance_transform_edt(~rm)
    dense = fbm(W, H, rng, oct=3, base=4)
    # verges: short grass strips hugging a street edge
    placed=0
    for _ in range(1600):
        if placed >= int(rng.integers(55, 90)): break
        x,y = int(rng.integers(2,W-2)), int(rng.integers(2,H-2))
        if m[y][x]!=PAVE or not (1.0 <= droad[y][x] < 2.0): continue
        horiz = rm[y-1][x] or rm[y+1][x]
        ln = int(rng.integers(2,7))
        for i in range(ln):
            xx,yy = (x+i,y) if horiz else (x,y+i)
            if 0<=xx<W and 0<=yy<H and m[yy][xx]==PAVE and droad[yy][xx]>=1.0:
                m[yy][xx]=GRASS
        placed+=1
    # gardens: 2x2..3x3 grass squares in the block interior
    for _ in range(int(rng.integers(18, 32))):
        for _try in range(30):
            x,y = int(rng.integers(2,W-5)), int(rng.integers(2,H-5))
            gw,gh = int(rng.integers(2,4)), int(rng.integers(2,4))
            cand=[(x+i,y+j) for i in range(gw) for j in range(gh)]
            if all(m[cy][cx]==PAVE and droad[cy][cx]>=1.5 for cx,cy in cand):
                for cx,cy in cand: m[cy][cx]=GRASS
                break
    # pools: 1x1..2x2 water dots, never adjacent to a road (keeps the dock off them)
    for _ in range(int(rng.integers(10, 20))):
        for _try in range(40):
            x,y = int(rng.integers(3,W-4)), int(rng.integers(3,H-4))
            if dense[y][x] > 0.5: continue                       # pools live in the suburbs
            pw,ph = int(rng.integers(1,3)), int(rng.integers(1,3))
            cand=[(x+i,y+j) for i in range(pw) for j in range(ph)]
            if all(m[cy][cx]==PAVE and droad[cy][cx]>=2.5 for cx,cy in cand):
                for cx,cy in cand: m[cy][cx]=WATER
                break

def parks(m, rng):
    """the central park is an ANGULAR patch with an angular pond and a 2-wide
    path ring; pocket parks green ENTIRE small blocks so they sit naturally in
    the street grid — no circles anywhere."""
    cx,cy = W//2+int(rng.integers(-40,41)), H//2+int(rng.integers(-40,41))
    blob = patch(m, rng, cx, cy, int(rng.integers(20,28)), GRASS,
                 only=(PAVE,ROAD))                        # roads inside the park are ERASED
    blob = {(x,y) for x,y in blob if m[y][x]==GRASS}
    if blob:
        border=set()                                      # 2-wide road BORDER around the park
        for x,y in blob:
            for dx in (-1,0,1):
                for dy in (-1,0,1):
                    n=(x+dx,y+dy)
                    if n not in blob and 0<=n[0]<W and 0<=n[1]<H: border.add(n)
        ring2=set(border)
        for x,y in border:
            for dx in (-1,0,1):
                for dy in (-1,0,1):
                    n=(x+dx,y+dy)
                    if n not in blob and 0<=n[0]<W and 0<=n[1]<H: ring2.add(n)
        for x,y in ring2:
            if m[y][x] not in (WATER,BRIDGE): m[y][x]=ROAD
        inner=[(x,y) for x,y in blob
               if all((nx,ny) in blob for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)))]
        for _ in range(1 + (rng.random()<0.4)):           # lake(s) inside
            if not inner: break
            px2,py2 = inner[int(rng.integers(0,len(inner)))]
            patch(m, rng, px2, py2, int(rng.integers(5,9)), WATER, only=(GRASS,))
    # pocket parks: whole small BLOCKS become greens
    seen=[[False]*W for _ in range(H)]
    blocks=[]
    for sy in range(H):
        for sx in range(W):
            if seen[sy][sx] or m[sy][sx]!=PAVE: continue
            stack=[(sx,sy)]; seen[sy][sx]=True; cells=[]
            while stack:
                x,y=stack.pop(); cells.append((x,y))
                for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
                    if 0<=nx<W and 0<=ny<H and not seen[ny][nx] and m[ny][nx]==PAVE:
                        seen[ny][nx]=True; stack.append((nx,ny))
            if 25 <= len(cells) <= 140: blocks.append(cells)
    rng.shuffle(blocks)
    for cells in blocks[:int(rng.integers(4,8))]:
        for x,y in cells: m[y][x]=GRASS

# ---------------------------------------------------------- connectivity ---
def connect_roads(m, rng):
    lab = [[0]*W for _ in range(H)]
    comps=[]
    for sy in range(H):
        for sx in range(W):
            if lab[sy][sx] or m[sy][sx] not in (ROAD,BRIDGE): continue
            comps.append([])
            stack=[(sx,sy)]; lab[sy][sx]=len(comps)
            while stack:
                x,y=stack.pop(); comps[-1].append((x,y))
                for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
                    if 0<=nx<W and 0<=ny<H and not lab[ny][nx] and m[ny][nx] in (ROAD,BRIDGE):
                        lab[ny][nx]=len(comps); stack.append((nx,ny))
    if not comps: return
    comps.sort(key=len, reverse=True)
    main=set(comps[0])
    for comp in comps[1:]:
        if len(comp) < 14:
            for x,y in comp: m[y][x]=PAVE
            continue
        sx,sy = comp[len(comp)//2]
        best,bd=None,1<<30
        for x,y in list(main)[::9]:
            dd=(x-sx)**2+(y-sy)**2
            if dd<bd: bd,best=dd,(x,y)
        if not best: continue
        x,y=sx,sy
        while (x,y)!=best:
            if   x<best[0]: x+=1
            elif x>best[0]: x-=1
            elif y<best[1]: y+=1
            else: y-=1
            for dq in (0,1):
                xx,yy=(min(W-1,x+dq),y) if abs(best[1]-sy)>=abs(best[0]-sx) else (x,min(H-1,y+dq))
                m[yy][xx] = BRIDGE if m[yy][xx]==WATER else ROAD
        main |= set(comp)

# -------------------------------------------------------------------- gen --
def generate(seed):
    rng = np.random.default_rng(seed)
    m = [[PAVE]*W for _ in range(H)]
    carve_rivers(m, rng)
    grass_banks(m, rng)
    esplanades(m, rng)
    arterials(m, rng)
    local_streets(m, rng)
    B = 5                                                  # built-up map RIM: erase streets
    for y in range(H):                                     # in the border band, buildings
        for x in range(W):                                 # will pack it in the fill pass
            if (x < B or x >= W-B or y < B or y >= H-B) and m[y][x] == ROAD:
                m[y][x] = PAVE
    for _ in range(8):                                     # prune dead-end stubs the rim cut
        changed=0
        for y in range(1,H-1):
            for x in range(1,W-1):
                if m[y][x]!=ROAD: continue
                n = sum(m[ny][nx] in (ROAD,BRIDGE) for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)))
                if n <= 1: m[y][x]=PAVE; changed+=1
        if not changed: break
    parks(m, rng)
    fill_blocks(m, rng)
    micro_details(m, rng)
    connect_roads(m, rng)
    return m

def save(m, path):
    im = Image.new("RGB",(W,H))
    px = im.load()
    for y in range(H):
        for x in range(W): px[x,y]=COL[m[y][x]]
    im.save(path)
    from collections import Counter
    c=Counter(ch for row in m for ch in row)
    names={ROAD:'road',PAVE:'pave',GRASS:'grass',WATER:'water',BLO:'b-lo',BMID:'b-mid',BHI:'b-hi',BRIDGE:'bridge'}
    stats=" ".join(f"{names[k]}={100*v/(W*H):.0f}%" for k,v in c.most_common())
    print(f"{path}: {stats}")

if __name__=="__main__":
    seed = int(sys.argv[1]) if len(sys.argv)>1 else 1
    outs = sys.argv[2:] or [f"/tmp/citygen_{seed}.png"]
    for i,o in enumerate(outs):
        save(generate(seed+i), o)
