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

def disc(m, cx, cy, r, ch):
    x0,x1 = max(0,int(cx-r)), min(W,int(cx+r+1))
    y0,y1 = max(0,int(cy-r)), min(H,int(cy+r+1))
    for y in range(y0,y1):
        for x in range(x0,x1):
            if (x-cx)**2+(y-cy)**2 <= r*r: m[y][x]=ch

# ---------------------------------------------------------------- water ----
def carve_rivers(m, rng):
    """1-2 bold diagonal waterways: smooth quadratic paths edge-to-edge with
    width swells, occasional islands, plus a few lakes."""
    import math
    def path(p0, p1, ctrl, wmin, wmax):
        steps = 260
        for i in range(steps+1):
            t = i/steps
            x = (1-t)*(1-t)*p0[0] + 2*(1-t)*t*ctrl[0] + t*t*p1[0]
            y = (1-t)*(1-t)*p0[1] + 2*(1-t)*t*ctrl[1] + t*t*p1[1]
            w = wmin + (wmax-wmin)*0.5*(1+math.sin(t*math.pi*rng.uniform(1.5,3.0)+rng.random()))
            disc(m, x, y, w, WATER)
    corners = [((rng.integers(20,120),-6),(rng.integers(140,240),H+6)),
               ((-6,rng.integers(20,120)),(W+6,rng.integers(140,240))),
               ((rng.integers(140,240),-6),(rng.integers(20,120),H+6)),
               ((W+6,rng.integers(20,120)),(-6,rng.integers(140,240)))]
    a,b = corners[int(rng.integers(4))]
    ctrl = (W/2+rng.integers(-60,61), H/2+rng.integers(-60,61))
    path(a,b,ctrl, rng.uniform(5,7), rng.uniform(9,13))
    if rng.random() < 0.7:                                   # a second, thinner branch
        c,d = corners[int(rng.integers(4))]
        ctrl2 = (ctrl[0]+rng.integers(-50,51), ctrl[1]+rng.integers(-50,51))
        path(c,d,ctrl2, rng.uniform(4,5), rng.uniform(6,9))
    for _ in range(int(rng.integers(1,4))):                  # lakes off the channel
        lx,ly = rng.integers(30,W-30), rng.integers(30,H-30)
        for _ in range(int(rng.integers(3,6))):
            disc(m, lx+rng.integers(-7,8), ly+rng.integers(-6,7), rng.uniform(4,8), WATER)
    # islands: reclaim a few pockets inside wide water
    from scipy import ndimage
    wm = np.array([[m[y][x]==WATER for x in range(W)] for y in range(H)])
    dist = ndimage.distance_transform_edt(wm)
    ys,xs = np.where(dist > 7)
    for _ in range(int(rng.integers(1,4))):
        if len(ys)==0: break
        k = int(rng.integers(0,len(ys)))
        disc(m, xs[k], ys[k], rng.uniform(2.5,5), GRASS)

def grass_banks(m, rng):
    wm = np.array([[m[y][x]==WATER for x in range(W)] for y in range(H)])
    from scipy import ndimage
    d = ndimage.distance_transform_edt(~wm)
    for y in range(H):
        for x in range(W):
            if wm[y][x] or m[y][x] != PAVE: continue
            if d[y][x] < 2.5 and rng.random()<0.95: m[y][x]=GRASS
            elif d[y][x] < 4 and rng.random()<0.5: m[y][x]=GRASS

def esplanades(m, rng):
    """roads that trace the riverbanks — the original's grid meets the water
    with streets running along the shore."""
    from scipy import ndimage
    wm = np.array([[m[y][x]==WATER for x in range(W)] for y in range(H)])
    d = ndimage.distance_transform_edt(~wm)
    band = (d >= 4.0) & (d < 5.5)
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
            p = max(4, min((W if vertical else H)-wid-4, p+jog))

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
        stop = 7 + int(10*(1.0-dense[min(cy,H-1)][min(cx,W-1)]))   # 7..17
        if w < stop and h < stop: return
        horiz = h > w if abs(h-w) > 3 else rng.random()<0.5
        wid = 2 if depth < 1 else 1
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
    """the hand map fills blocks nearly SOLID with buildings, the type dithered
    in diagonal stripes between a district's two dominant kinds, threaded with
    pavement holes; suburbs thin out, downtown packs tight."""
    kind  = fbm(W, H, rng, oct=3, base=5)
    dense = fbm(W, H, rng, oct=3, base=4)
    holes = fbm(W, H, rng, oct=4, base=10)
    rm = np.array([[m[y][x] in (ROAD,BRIDGE) for x in range(W)] for y in range(H)])
    from scipy import ndimage
    droad = ndimage.distance_transform_edt(~rm)
    for y in range(H):
        for x in range(W):
            if m[y][x] != PAVE or droad[y][x] < 2: continue
            d = dense[y][x]
            hole = 0.30 - 0.22*d                              # downtown ~8% holes, suburbs ~30%
            if holes[y][x] < hole or rng.random() < 0.06: continue
            k = kind[y][x]
            if   k > 0.58: pri,sec = BHI,BMID
            elif k > 0.40: pri,sec = BMID,(BHI if k>0.5 else BLO)
            else:          pri,sec = BLO,BMID
            stripe = ((x + y) >> 1) % 3                       # diagonal dither, like the original
            m[y][x] = pri if stripe else sec

def parks(m, rng):
    cx,cy = W//2+int(rng.integers(-40,41)), H//2+int(rng.integers(-40,41))
    blob=set()
    for _ in range(int(rng.integers(9,14))):
        bx,by,r = cx+rng.integers(-13,14), cy+rng.integers(-11,12), rng.uniform(5,10)
        for y in range(max(0,int(by-r)),min(H,int(by+r+1))):
            for x in range(max(0,int(bx-r)),min(W,int(bx+r+1))):
                if (x-bx)**2+(y-by)**2<=r*r: blob.add((x,y))
    for x,y in blob:
        if m[y][x]==PAVE: m[y][x]=GRASS
    for x,y in blob:                                   # path RING around the park edge
        edge=any((nx,ny) not in blob for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)))
        if edge and m[y][x]==GRASS: m[y][x]=ROAD
    disc(m, cx+rng.integers(-3,4), cy+rng.integers(-3,4), rng.uniform(2.5,4.0), WATER)
    for _ in range(int(rng.integers(8,14))):
        px,py = rng.integers(12,W-12), rng.integers(12,H-12)
        if m[py][px]==PAVE: disc(m, px, py, rng.uniform(2,4.5), GRASS)

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
    parks(m, rng)
    fill_blocks(m, rng)
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
