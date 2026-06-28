#!/usr/bin/env python3
"""Authoring step for MotoKart's editable source assets.

Writes the *editable* sources under assets/ (the kart model + the sprite PNGs).
These are what you open/tweak in Mote Studio; `mote bake` turns them into src/*.h.

The kart is ONE multi-part OBJ (`o hull / trim / wheel_*`) plus a `kart.rig`
sidecar, so it bakes to a single `MoteRig kart_rig` you can open in the Studio
Rig tab. At draw time the HULL part is tinted per racer (palette) while the
wheels spin + the front wheels steer.
"""
import os, math
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "assets"))
os.makedirs(ASSETS, exist_ok=True)

# ---------------------------------------------------------------------------
# RawMesh — arbitrary verts/tris (for the curved item models).  Faces are
# auto-wound OUTWARD (flipped to face away from the mesh centroid) so obj2mesh's
# normals + the engine's backface cull come out right for these convex-ish shapes.
# ---------------------------------------------------------------------------
class RawMesh:
    def __init__(self): self.v=[]; self.f=[]          # f: [mtl,(a,b,c)]
    def vert(self,x,y,z): self.v.append((x,y,z)); return len(self.v)-1
    def tri(self,a,b,c,mtl): self.f.append([mtl,(a,b,c)])
    def quad(self,a,b,c,d,mtl): self.tri(a,b,c,mtl); self.tri(a,c,d,mtl)
    def _wind_outward(self):
        cx=sum(p[0] for p in self.v)/len(self.v)
        cy=sum(p[1] for p in self.v)/len(self.v)
        cz=sum(p[2] for p in self.v)/len(self.v)
        for fc in self.f:
            a,b,c=fc[1]; A,B,C=self.v[a],self.v[b],self.v[c]
            ux,uy,uz=B[0]-A[0],B[1]-A[1],B[2]-A[2]
            vx,vy,vz=C[0]-A[0],C[1]-A[1],C[2]-A[2]
            nx,ny,nz=uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
            mx,my,mz=(A[0]+B[0]+C[0])/3-cx,(A[1]+B[1]+C[1])/3-cy,(A[2]+B[2]+C[2])/3-cz
            if nx*mx+ny*my+nz*mz < 0: fc[1]=(a,c,b)   # flip to face outward
    def write(self,objpath,mtlpath,mats):
        self._wind_outward()
        with open(mtlpath,"w") as fh:
            fh.write("# item materials\n")
            for n,(r,g,b) in mats.items(): fh.write("newmtl %s\nKd %.3f %.3f %.3f\n\n"%(n,r,g,b))
        with open(objpath,"w") as fh:
            fh.write("mtllib %s\n"%os.path.basename(mtlpath))
            for x,y,z in self.v: fh.write("v %.4f %.4f %.4f\n"%(x,y,z))
            cur=None
            for mtl,(a,b,c) in self.f:
                if mtl!=cur: fh.write("usemtl %s\n"%mtl); cur=mtl
                fh.write("f %d %d %d\n"%(a+1,b+1,c+1))

def build_banana():
    m=RawMesh(); N=5; Rarc=0.24
    prof=[(-1.15,0.012),(-0.6,0.06),(0.0,0.085),(0.6,0.06),(1.15,0.012)]  # (arc angle, tube radius)
    rings=[]; centers=[]
    for ang,r in prof:
        c=(Rarc*math.sin(ang),0.0,Rarc*math.cos(ang)); centers.append(c)
        Tx,Tz=math.cos(ang),-math.sin(ang)                 # tangent in XZ
        Rx,Ry,Rz=(0*0-(-0)*Tz, 0,  Tx*0-0)                 # cross(T,up): up=(0,1,0)
        Rx,Ry,Rz=( -Tz*1.0, 0.0, Tx*1.0 )                  # = cross((Tx,0,Tz),(0,1,0))
        l=math.hypot(Rx,Rz) or 1; Rx/=l; Rz/=l
        ring=[]
        for k in range(N):
            th=2*math.pi*k/N; ca,sa=math.cos(th),math.sin(th)
            ring.append(m.vert(c[0]+r*ca*Rx, c[1]+r*sa, c[2]+r*ca*Rz))
        rings.append(ring)
    for i in range(len(rings)-1):
        A,B=rings[i],rings[i+1]
        mtl="btip" if (i==0 or i==len(rings)-2) else "banana"
        for k in range(N):
            k2=(k+1)%N; m.quad(A[k],A[k2],B[k2],B[k],mtl)
    c0=m.vert(*centers[0]); cL=m.vert(*centers[-1])
    for k in range(N):
        k2=(k+1)%N
        m.tri(c0,rings[0][k],rings[0][k2],"btip")
        m.tri(cL,rings[-1][k],rings[-1][k2],"btip")
    m.write(os.path.join(ASSETS,"banana.obj"),os.path.join(ASSETS,"banana.mtl"),
            {"banana":(0.95,0.82,0.12),"btip":(0.35,0.22,0.08)})
    print("banana.obj: %d verts %d tris"%(len(m.v),len(m.f)))

def build_shell():
    m=RawMesh(); N=8; R=0.30
    base=[]; phis=[15,40,65]
    for k in range(N):
        th=2*math.pi*k/N; base.append(m.vert(R*math.cos(th),0.0,R*math.sin(th)))
    rings=[base]
    for phi in phis:
        rr=R*math.cos(math.radians(phi)); h=R*math.sin(math.radians(phi)); ring=[]
        for k in range(N):
            th=2*math.pi*k/N; ring.append(m.vert(rr*math.cos(th),h,rr*math.sin(th)))
        rings.append(ring)
    apex=m.vert(0,R,0); bc=m.vert(0,0,0)
    for i in range(len(rings)-1):
        A,B=rings[i],rings[i+1]
        mtl="rim" if i==0 else "shell"
        for k in range(N):
            k2=(k+1)%N; m.quad(A[k],A[k2],B[k2],B[k],mtl)
    top=rings[-1]
    for k in range(N):
        k2=(k+1)%N; m.tri(apex,top[k],top[k2],"shell"); m.tri(bc,base[k],base[k2],"rim")
    # NB: model is named "kshell" (not "shell") so its baked src/kshell.h can't
    # collide with the shell SOUND header (shell.wav/shell.sfx -> src/shell.*)
    m.write(os.path.join(ASSETS,"kshell.obj"),os.path.join(ASSETS,"kshell.mtl"),
            {"shell":(0.30,0.78,0.32),"rim":(0.94,0.92,0.80)})
    print("kshell.obj: %d verts %d tris"%(len(m.v),len(m.f)))

# ---------------------------------------------------------------------------
# Kart geometry (boxes).  Local frame: +Z forward, +Y up, metres.
# ---------------------------------------------------------------------------
CORNERS = [(-1,-1,-1),( 1,-1,-1),( 1, 1,-1),(-1, 1,-1),
           (-1,-1, 1),( 1,-1, 1),( 1, 1, 1),(-1, 1, 1)]
QUADS = [(4,5,6,7),(1,0,3,2),(1,2,6,5),(0,4,7,3),(7,6,2,3),(0,1,5,4)]

class PartObj:
    """Multi-part OBJ builder: each box is tagged with its part (an `o` group)."""
    def __init__(self): self.v=[]; self.faces=[]   # faces: (part, mtl, (idx..))
    def box(self, part, mtl, cx,cy,cz, hx,hy,hz):
        base=len(self.v)
        for sx,sy,sz in CORNERS: self.v.append((cx+sx*hx, cy+sy*hy, cz+sz*hz))
        for q in QUADS: self.faces.append((part, mtl, tuple(base+i for i in q)))
    def write(self, objpath, mtlpath, materials, part_order):
        with open(mtlpath,"w") as f:
            f.write("# MotoKart kart materials\n")
            for n,(r,g,b) in materials.items():
                f.write("newmtl %s\nKd %.3f %.3f %.3f\n\n"%(n,r,g,b))
        with open(objpath,"w") as f:
            f.write("# MotoKart go-kart — multi-part (+Z fwd, +Y up, metres)\n")
            f.write("mtllib %s\n"%os.path.basename(mtlpath))
            for x,y,z in self.v: f.write("v %.4f %.4f %.4f\n"%(x,y,z))
            for part in part_order:               # emit root-first, one `o` per part
                f.write("o %s\n"%part)
                cur=None
                for p,mtl,idx in self.faces:
                    if p!=part: continue
                    if mtl!=cur: f.write("usemtl %s\n"%mtl); cur=mtl
                    f.write("f %s\n"%" ".join(str(i+1) for i in idx))

KART_MAT = {
    "hull":  (0.80,0.80,0.84),   # neutral grey — palette tints it per racer
    "trim":  (0.96,0.82,0.10),
    "dark":  (0.11,0.11,0.14),
    "seat":  (0.18,0.18,0.22),
    "metal": (0.62,0.64,0.70),
    "tire":  (0.06,0.06,0.07),
    "hub":   (0.80,0.81,0.85),
}
# wheel axle centres = rig pivots (the joint each wheel spins / steers about)
WHEELS = { "wheel_fl":(-0.245,0.070, 0.30), "wheel_fr":( 0.245,0.070, 0.30),
           "wheel_rl":(-0.260,0.080,-0.26), "wheel_rr":( 0.260,0.080,-0.26) }
PART_ORDER = ["hull","trim","wheel_fl","wheel_fr","wheel_rl","wheel_rr"]  # root-first

def build_kart():
    o = PartObj()
    # ---- hull (tinted per racer) ----
    o.box("hull","hull", 0,0.075,0.00, 0.20,0.035,0.40)   # floor pan
    o.box("hull","hull", 0,0.105,0.36, 0.17,0.045,0.10)   # nose
    o.box("hull","hull", -0.215,0.105,-0.02, 0.035,0.060,0.26)  # pod L
    o.box("hull","hull",  0.215,0.105,-0.02, 0.035,0.060,0.26)  # pod R
    o.box("hull","hull", 0,0.150,-0.06, 0.13,0.075,0.13)  # cockpit hump
    # ---- trim (fixed colours: stripe, seat, engine, wing) ----
    o.box("trim","trim", 0,0.135,0.30, 0.10,0.030,0.06)   # nose stripe
    o.box("trim","seat", 0,0.165,-0.06, 0.115,0.030,0.115)# seat base
    o.box("trim","seat", 0,0.250,-0.20, 0.135,0.110,0.05) # seat back
    o.box("trim","dark", 0,0.150,0.12, 0.015,0.075,0.015) # column
    o.box("trim","dark", 0,0.220,0.135, 0.075,0.012,0.022)# steering wheel
    o.box("trim","dark", 0,0.150,-0.30, 0.10,0.075,0.075) # engine
    o.box("trim","metal", 0.10,0.110,-0.40, 0.028,0.028,0.06) # exhaust
    o.box("trim","dark", -0.17,0.270,-0.40, 0.018,0.075,0.016)# wing post L
    o.box("trim","dark",  0.17,0.270,-0.40, 0.018,0.075,0.016)# wing post R
    o.box("trim","trim", 0,0.345,-0.41, 0.235,0.014,0.060)    # wing plate
    # ---- wheels (own part each: tyre + hub, centred on the axle pivot) ----
    for name,(cx,cy,cz) in WHEELS.items():
        front = "f" in name
        hx,hy,hz = (0.045,0.070,0.070) if front else (0.055,0.085,0.085)
        o.box(name,"tire", cx,cy,cz, hx,hy,hz)
        o.box(name,"hub",  cx+ (0.005 if cx>0 else -0.005),cy,cz,
              hx*1.2,hy*0.42,hz*0.42)
    o.write(os.path.join(ASSETS,"kart.obj"), os.path.join(ASSETS,"kart.mtl"),
            KART_MAT, PART_ORDER)
    print("kart.obj: %d verts %d quads, %d parts"%(len(o.v),len(o.faces),len(PART_ORDER)))

    # ---- kart.rig sidecar (parent + pivot per part, root-first) ----
    with open(os.path.join(ASSETS,"kart.rig"),"w") as f:
        f.write("# MotoKart rig — hull root, wheels parented to it, pivot at each axle\n")
        f.write("part hull parent -1 pivot 0 0 0\n")
        f.write("part trim parent hull pivot 0 0 0\n")
        for name,(cx,cy,cz) in WHEELS.items():
            f.write("part %s parent hull pivot %.3f %.3f %.3f\n"%(name,cx,cy,cz))
    print("kart.rig written")

# ---------------------------------------------------------------------------
# Sprites
# ---------------------------------------------------------------------------
def save_png(img,name): img.save(os.path.join(ASSETS,name)); print(name,img.size)

def driver_sheet():
    W,H,N=18,24,8
    img=Image.new("RGBA",(W*N,H),(0,0,0,0)); d=ImageDraw.Draw(img)
    hues=[(220,40,40),(40,90,220),(40,180,70),(230,180,30),
          (170,60,200),(240,130,30),(40,200,200),(230,230,235)]
    for i,(r,g,b) in enumerate(hues):
        ox=i*W; dk=(r*2//3,g*2//3,b*2//3)
        d.rounded_rectangle([ox+2,15,ox+15,23],3,fill=(r,g,b,255))
        d.rectangle([ox+7,13,ox+10,17],fill=dk+(255,))
        d.ellipse([ox+4,2,ox+13,14],fill=(r,g,b,255),outline=(20,20,24,255))
        d.ellipse([ox+5,3,ox+12,9],fill=(min(255,r+40),min(255,g+40),min(255,b+40),255))
        d.rectangle([ox+5,9,ox+12,11],fill=(30,30,40,255))
    save_png(img,"driver.png")

def tree_sheet():
    # 5 scenery kinds, 26x40 each: pine, round, cactus, palm, snowpine
    W,H,N=26,40,5
    img=Image.new("RGBA",(W*N,H),(0,0,0,0)); d=ImageDraw.Draw(img)
    trunk=(96,62,33,255)
    def pine(ox, g_lo, g_hi, snow=False):
        d.rectangle([ox+11,33,ox+15,40],fill=trunk)
        for k,(ay,by,hw) in enumerate([(2,16,6),(10,26,8),(18,34,11)]):
            col=(g_lo[0]+k*8, g_lo[1]+k*10, g_lo[2]+k*6,255)
            d.polygon([(ox+13,ay),(ox+13-hw,by),(ox+13+hw,by)],fill=col)
            if snow:  # white cap on each tier apex
                d.polygon([(ox+13,ay),(ox+13-hw//2,ay+5),(ox+13+hw//2,ay+5)],fill=(235,240,250,255))
    # 0 pine
    pine(0,(28,108,40),(50,160,60))
    # 1 round
    ox=W
    d.rectangle([ox+11,30,ox+16,40],fill=trunk)
    d.ellipse([ox+3,4,ox+23,28],fill=(34,128,46,255))
    d.ellipse([ox+6,2,ox+18,18],fill=(50,160,60,255))
    d.ellipse([ox+12,12,ox+22,26],fill=(28,108,40,255))
    # 2 cactus
    ox=2*W; cg=(46,140,60,255); cd=(34,110,48,255)
    d.rounded_rectangle([ox+10,8,ox+16,40],3,fill=cg)
    d.rounded_rectangle([ox+3,18,ox+9,40],3,fill=cg)
    d.rounded_rectangle([ox+17,14,ox+23,40],3,fill=cg)
    d.rounded_rectangle([ox+3,18,ox+6,28],2,fill=cd)
    # 3 palm
    ox=3*W
    d.polygon([(ox+12,40),(ox+10,12),(ox+15,12)],fill=trunk)
    for ang in (-1,-0.4,0.4,1):
        x2=ox+13+int(ang*13);
        d.line([ox+13,12,x2,4 if abs(ang)<0.6 else 8],fill=(40,150,60,255),width=3)
    d.ellipse([ox+10,9,ox+16,15],fill=(36,130,52,255))
    d.ellipse([ox+11,38,ox+15,42],fill=(120,80,40,255))
    # 4 snowpine
    pine(4*W,(30,96,52),(60,140,80),snow=True)
    save_png(img,"tree.png")

def build_items():
    """HUD power-up icon strip: 10 frames of 16x16, indexed by ITEM id-1
       (mushroom, x3, banana, x3, green shell, red shell, star, bullet, lightning, mega)."""
    N=10; W=16
    img=Image.new("RGBA",(W*N,W),(0,0,0,0)); d=ImageDraw.Draw(img)
    def mush(ox,cx,cy,rad,cap,spots=True):
        d.pieslice([ox+cx-rad,cy-rad,ox+cx+rad,cy+rad],180,360,fill=cap+(255,))
        d.rectangle([ox+cx-rad,cy-1,ox+cx+rad,cy+1],fill=cap+(255,))
        sw=max(2,rad//2)
        d.rounded_rectangle([ox+cx-sw,cy,ox+cx+sw,cy+rad+1],2,fill=(248,242,228,255))
        if spots:
            d.ellipse([ox+cx-1,cy-rad+2,ox+cx+1,cy-rad+4],fill=(255,255,255,255))   # small centre spot
            if rad>=5:                                                              # two tiny side spots
                d.ellipse([ox+cx-rad+2,cy-2,ox+cx-rad+3,cy-1],fill=(255,255,255,255))
                d.ellipse([ox+cx+rad-3,cy-2,ox+cx+rad-2,cy-1],fill=(255,255,255,255))
    def banana(ox,cx,cy,s,col=(245,215,40)):
        d.arc([ox+cx-s,cy-s,ox+cx+s,cy+s],35,205,fill=col+(255,),width=max(2,int(s*0.7)))
    def shell(ox,cx,cy,r,col):
        d.pieslice([ox+cx-r,cy-r,ox+cx+r,cy+r],180,360,fill=col+(255,),outline=(15,30,15,255))
        d.rectangle([ox+cx-r,cy,ox+cx+r,cy+2],fill=(238,232,205,255))
        d.line([ox+cx,cy-r+2,ox+cx,cy-1],fill=(20,50,20,200))
    def star(ox,cx,cy,ro,ri):
        pts=[]
        for k in range(10):
            a=-math.pi/2+k*math.pi/5; r=ro if k%2==0 else ri
            pts.append((ox+cx+r*math.cos(a),cy+r*math.sin(a)))
        d.polygon(pts,fill=(255,225,60,255),outline=(150,110,0,255))
    mush(0*W,8,9,6,(225,55,55))                                             # 1 mushroom
    ox=1*W; mush(ox-3,5,11,3,(225,55,55)); mush(ox+3,11,11,3,(225,55,55)); mush(ox,8,7,4,(225,55,55))  # 2 x3
    banana(2*W,8,8,6)                                                       # 3 banana
    ox=3*W; banana(ox,5,6,4); banana(ox,11,7,4); banana(ox,8,12,4)          # 4 x3 banana
    shell(4*W,8,10,6,(60,190,70))                                           # 5 green shell
    shell(5*W,8,10,6,(220,60,55))                                           # 6 red shell
    star(6*W,8,8,7,3)                                                       # 7 star
    ox=7*W                                                                  # 8 bullet bill
    d.rounded_rectangle([ox+2,5,ox+11,12],3,fill=(45,48,56,255),outline=(12,12,16,255))
    d.pieslice([ox+8,5,ox+15,12],270,90,fill=(45,48,56,255),outline=(12,12,16,255))
    d.ellipse([ox+4,6,ox+7,10],fill=(255,255,255,255)); d.ellipse([ox+5,7,ox+6,9],fill=(0,0,0,255))
    ox=8*W                                                                  # 9 lightning
    d.polygon([(ox+9,1),(ox+4,9),(ox+7,9),(ox+6,15),(ox+12,6),(ox+8,6)],fill=(255,225,60,255),outline=(190,150,0,255))
    mush(9*W,8,10,7,(245,170,40))                                           # 10 mega mushroom
    save_png(img,"items.png")

def itembox_sheet():
    W=20; img=Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(img)
    d.rounded_rectangle([2,2,17,17],4,fill=(40,180,230,235),outline=(240,250,255,255))
    d.rounded_rectangle([4,4,15,15],3,fill=(70,210,250,235))
    d.arc([6,5,13,12],200,90,fill=(255,255,255,255),width=2)
    d.line([10,10,10,12],fill=(255,255,255,255),width=2); d.point((10,15),fill=(255,255,255,255))
    save_png(img,"itembox.png")

def banner_sheet():
    W,Hh=64,16; img=Image.new("RGBA",(W,Hh),(0,0,0,0)); d=ImageDraw.Draw(img); sq=8
    for ry in range(0,Hh,sq):
        for rx in range(0,W,sq):
            on=((rx//sq)+(ry//sq))%2==0
            d.rectangle([rx,ry,rx+sq-1,ry+sq-1],fill=(20,20,24,255) if on else (235,235,240,255))
    save_png(img,"banner.png")

def sign_sheet():
    W=20; img=Image.new("RGBA",(W*2,W),(0,0,0,0)); d=ImageDraw.Draw(img)
    for i,flip in enumerate((False,True)):
        ox=i*W
        d.rounded_rectangle([ox+1,4,ox+18,15],3,fill=(235,200,40,255),outline=(40,30,10,255))
        if not flip: d.polygon([(ox+5,7),(ox+5,12),(ox+13,9.5)],fill=(40,30,10,255))
        else:        d.polygon([(ox+14,7),(ox+14,12),(ox+6,9.5)],fill=(40,30,10,255))
    save_png(img,"sign.png")

def icon_png():
    W=60; img=Image.new("RGBA",(W,W),(28,32,52,255)); d=ImageDraw.Draw(img)
    d.rectangle([0,38,59,59],fill=(80,84,92,255))
    d.rectangle([0,36,59,40],fill=(60,160,80,255))
    sq=5
    for ry in range(4,24,sq):
        for rx in range(4,29,sq):
            on=((rx//sq)+(ry//sq))%2==0
            d.rectangle([rx,ry,rx+sq-1,ry+sq-1],fill=(235,235,240,255) if on else (30,30,36,255))
    d.rounded_rectangle([20,40,48,50],3,fill=(220,40,40,255))
    d.rectangle([26,34,40,42],fill=(220,40,40,255))
    d.ellipse([22,47,30,55],fill=(20,20,24,255)); d.ellipse([40,47,48,55],fill=(20,20,24,255))
    d.ellipse([31,28,41,38],fill=(230,180,30,255))
    img.convert("RGB").save(os.path.join("..","icon.png") if False else os.path.join(os.path.dirname(ASSETS),"icon.png"))
    print("icon.png",(W,W))

if __name__=="__main__":
    build_kart(); build_banana(); build_shell()
    driver_sheet(); tree_sheet(); itembox_sheet(); banner_sheet(); sign_sheet(); build_items()
    icon_png()
    print("done ->",ASSETS)
