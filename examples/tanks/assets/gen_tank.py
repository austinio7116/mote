#!/usr/bin/env python3
"""Generate a detailed multi-part tank.obj for Mote's rig pipeline.

Objects (== rig parts, root-first): body, tracks, turret, barrel.
The tank faces +z; the barrel points +z. After each primitive we orient its
triangles so the winding-derived normal points away from the primitive centre
(the engine backface-culls on that normal) — so no face can go missing.
Re-run, then `mote bake examples/tanks`.
"""
import math

V = []                      # (x,y,z)
FACES = []                  # [objname, a, b, c]  (1-based vert indices)
_cur = [None]

def vert(x, y, z):
    V.append((x, y, z)); return len(V)
def obj(name): _cur[0] = name
def tri(a, b, c): FACES.append([_cur[0], a, b, c])
def quad(a, b, c, d): tri(a, b, c); tri(a, c, d)

def _cross(A, B, C):
    ux,uy,uz=B[0]-A[0],B[1]-A[1],B[2]-A[2]; vx,vy,vz=C[0]-A[0],C[1]-A[1],C[2]-A[2]
    return (uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx)

def orient(start, cx, cy, cz):
    """Flip any triangle whose normal points toward (cx,cy,cz) so all face out."""
    for k in range(start, len(FACES)):
        _,a,b,c = FACES[k]; A,B,C = V[a-1],V[b-1],V[c-1]
        n = _cross(A,B,C); fx=(A[0]+B[0]+C[0])/3; fy=(A[1]+B[1]+C[1])/3; fz=(A[2]+B[2]+C[2])/3
        if n[0]*(fx-cx)+n[1]*(fy-cy)+n[2]*(fz-cz) < 0: FACES[k][2],FACES[k][3]=c,b

def box(cx, cy, cz, hx, hy, hz):
    s=len(FACES); x0,x1=cx-hx,cx+hx; y0,y1=cy-hy,cy+hy; z0,z1=cz-hz,cz+hz
    p=[vert(x0,y0,z0),vert(x1,y0,z0),vert(x1,y0,z1),vert(x0,y0,z1),
       vert(x0,y1,z0),vert(x1,y1,z0),vert(x1,y1,z1),vert(x0,y1,z1)]
    for a,b,c,d in [(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7),(4,5,6,7),(3,2,1,0)]:
        quad(p[a],p[b],p[c],p[d])
    orient(s,cx,cy,cz)

def extrude_x(profile_zy, x0, x1):
    """Extrude a (z,y) profile loop across x -> a prism with caps; auto-oriented."""
    s=len(FACES); cz=sum(p[0] for p in profile_zy)/len(profile_zy); cy=sum(p[1] for p in profile_zy)/len(profile_zy)
    L=[vert(x0,y,z) for (z,y) in profile_zy]; R=[vert(x1,y,z) for (z,y) in profile_zy]
    n=len(profile_zy)
    for i in range(n):
        j=(i+1)%n; quad(L[i],R[i],R[j],L[j])                       # side walls
    lc=vert(x0,cy,cz); rc=vert(x1,cy,cz)
    for i in range(n):
        j=(i+1)%n; tri(lc,L[i],L[j]); tri(rc,R[i],R[j])            # end caps (fans)
    orient(s,(x0+x1)/2,cy,cz)

def prism_y(cx,cy,cz, rx,rz, y0,y1, n, rot=0.0):
    s=len(FACES); bot=[]; top=[]
    for i in range(n):
        a=rot+2*math.pi*i/n; x=cx+rx*math.cos(a); z=cz+rz*math.sin(a)
        bot.append(vert(x,y0,z)); top.append(vert(x,y1,z))
    cb=vert(cx,y0,cz); ct=vert(cx,y1,cz)
    for i in range(n):
        j=(i+1)%n; quad(bot[i],bot[j],top[j],top[i]); tri(cb,bot[i],bot[j]); tri(ct,top[i],top[j])
    orient(s,cx,(y0+y1)/2,cz)

def prism_z(cx,cy, rx,ry, z0,z1, n, rot=0.0):
    s=len(FACES); a0=[]; a1=[]
    for i in range(n):
        a=rot+2*math.pi*i/n; x=cx+rx*math.cos(a); y=cy+ry*math.sin(a)
        a0.append(vert(x,y,z0)); a1.append(vert(x,y,z1))
    c0=vert(cx,cy,z0); c1=vert(cx,cy,z1)
    for i in range(n):
        j=(i+1)%n; quad(a0[i],a1[i],a1[j],a0[j]); tri(c0,a0[i],a0[j]); tri(c1,a1[i],a1[j])
    orient(s,cx,cy,(z0+z1)/2)

def prism_x(cx,cy,cz, ry,rz, x0,x1, n, rot=0.0):
    s=len(FACES); a0=[]; a1=[]
    for i in range(n):
        a=rot+2*math.pi*i/n; y=cy+ry*math.cos(a); z=cz+rz*math.sin(a)
        a0.append(vert(x0,y,z)); a1.append(vert(x1,y,z))
    c0=vert(x0,cy,cz); c1=vert(x1,cy,cz)
    for i in range(n):
        j=(i+1)%n; quad(a0[i],a1[i],a1[j],a0[j]); tri(c0,a0[i],a0[j]); tri(c1,a1[i],a1[j])
    orient(s,(x0+x1)/2,cy,cz)

# ---- body: hull box + sloped glacis nose + turret pedestal ----
obj("body")
box(0.0, 0.185, -0.02, 0.20, 0.085, 0.30)                          # hull
# glacis: a 6-pt side profile (sloped nose front, sloped tail rear), extruded across x
box(0.0, 0.30, 0.02, 0.205, 0.03, 0.235)                           # turret pedestal ring
glacis=[(-0.30,0.10),(0.30,0.10),(0.50,0.165),(0.46,0.27),(-0.40,0.27),(-0.46,0.20)]
extrude_x(glacis, -0.185, 0.185)                                   # upper hull shell w/ sloped nose+tail

# ---- tracks: angled side-profile run + road wheels (per side) ----
obj("tracks")
trk=[(-0.40,0.0),(0.40,0.0),(0.50,0.14),(0.42,0.20),(-0.42,0.20),(-0.50,0.14)]   # track loop profile
for sx in (-0.255, 0.255):
    extrude_x(trk, sx-0.065, sx+0.065)
    for wz in (-0.30,-0.10,0.10,0.30):                             # 4 road wheels
        prism_x(sx, 0.065, wz, 0.075, 0.075, sx-0.075, sx+0.075, 6, rot=math.pi/6)

# ---- turret: octagonal dome + front mantlet + commander hatch ----
obj("turret")
prism_y(0.0, 0.0, 0.02, 0.185, 0.215, 0.31, 0.43, 8, rot=math.pi/8)
box(0.0, 0.355, 0.235, 0.085, 0.055, 0.06)                         # mantlet
box(-0.02, 0.45, -0.07, 0.055, 0.025, 0.06)                        # commander hatch

# ---- barrel: octagonal gun + muzzle brake ----
obj("barrel")
prism_z(0.0, 0.355, 0.032, 0.032, 0.12, 0.70, 8, rot=math.pi/8)
box(0.0, 0.355, 0.70, 0.05, 0.05, 0.045)                           # muzzle brake

# ---- emit ----
out=["# generated by gen_tank.py — detailed tank (body/tracks/turret/barrel)","mtllib tank.mtl"]
for (x,y,z) in V: out.append(f"v {x:.4f} {y:.4f} {z:.4f}")
mtl={"body":"hull","tracks":"tracks","turret":"turret","barrel":"barrel"}
for name in ("body","tracks","turret","barrel"):
    out.append(f"o {name}"); out.append(f"usemtl {mtl[name]}")
    for fc in FACES:
        if fc[0]==name: out.append(f"f {fc[1]} {fc[2]} {fc[3]}")
open("tank.obj","w").write("\n".join(out)+"\n")
used={};
for fc in FACES: used.setdefault(fc[0],set()).update((fc[1],fc[2],fc[3]))
print("faces:", len(FACES), "| verts/part:", {k:len(v) for k,v in used.items()})
