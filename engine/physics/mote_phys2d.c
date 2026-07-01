/*
 * Mote — 2D rigid-body solver (see mote_phys2d.h). Sequential-impulse with
 * restitution + friction, one rotational DOF, AABB broad phase, positional
 * depenetration. Single-point manifolds (deepest contact) — ideal for arcade
 * vehicles that bump and spin rather than stack.
 */
#include "mote_phys2d.h"
#include "mote_arena.h"
#include <math.h>

#define SUB_DT (1.0f/120.0f)   /* fixed substep */

/* one resolved contact (solver-internal; the pool is arena-allocated) */
typedef struct { int a, b; float nx, ny, depth, px, py; } MoteContact2;

/* Contact scratch is ARENA-allocated at load — the engine holds only a pointer,
 * no fixed .bss pool (mirrors mote_phys_configure). */
static MoteContact2 *s_ct2;
static int s_max2c, s_max2b;

int mote_phys2d_configure(struct MoteArena *arena, int max_bodies, int max_contacts) {
    if (max_bodies <= 0) { s_max2b = 0; s_ct2 = 0; return 1; }
    s_max2b = max_bodies;
    s_max2c = max_contacts > 0 ? max_contacts : max_bodies * 4;
    s_ct2 = mote_arena_alloc(arena, (size_t)s_max2c * sizeof(MoteContact2));
    return s_ct2 != 0;
}

static inline float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }

static inline void to_local(const MoteBody2D *B, float wx, float wy, float *lx, float *ly){
    float c=cosf(B->angle), s=sinf(B->angle), dx=wx-B->x, dy=wy-B->y;
    *lx =  c*dx + s*dy;  *ly = -s*dx + c*dy;
}
static inline void dir_to_world(const MoteBody2D *B, float lx, float ly, float *wx, float *wy){
    float c=cosf(B->angle), s=sinf(B->angle);
    *wx = c*lx - s*ly;  *wy = s*lx + c*ly;
}

/* contact normal points from A to B. returns 1 if overlapping. */
static int collide(const MoteBody2D *A, const MoteBody2D *B, MoteContact2 *c){
    if (A->shape==MOTE_C2D_CIRCLE && B->shape==MOTE_C2D_CIRCLE){
        float dx=B->x-A->x, dy=B->y-A->y, d2=dx*dx+dy*dy, r=A->radius+B->radius;
        if (d2>=r*r) return 0;
        float d=sqrtf(d2); float nx = d>1e-6f?dx/d:1.0f, ny = d>1e-6f?dy/d:0.0f;
        c->nx=nx; c->ny=ny; c->depth=r-d; c->px=A->x+nx*A->radius; c->py=A->y+ny*A->radius; return 1;
    }
    /* circle vs box: orient so circle=cir, box=bx, then fix normal sign */
    if (A->shape!=B->shape){
        const MoteBody2D *cir = A->shape==MOTE_C2D_CIRCLE?A:B;
        const MoteBody2D *bx  = A->shape==MOTE_C2D_CIRCLE?B:A;
        float lx,ly; to_local(bx, cir->x, cir->y, &lx,&ly);
        float qx=clampf(lx,-bx->hx,bx->hx), qy=clampf(ly,-bx->hy,bx->hy);
        float ddx=lx-qx, ddy=ly-qy, d2=ddx*ddx+ddy*ddy;
        float nlx, nly, depth;
        if (d2>1e-9f){ float d=sqrtf(d2); if(d>=cir->radius) return 0; nlx=ddx/d; nly=ddy/d; depth=cir->radius-d; }
        else { float ox=bx->hx-fabsf(lx), oy=bx->hy-fabsf(ly);   /* centre inside box */
               if (ox<oy){ nlx=lx<0?-1.0f:1.0f; nly=0; depth=cir->radius+ox; }
               else      { nlx=0; nly=ly<0?-1.0f:1.0f; depth=cir->radius+oy; } }
        float wnx,wny; dir_to_world(bx, nlx,nly, &wnx,&wny);      /* box surface -> circle */
        float pxL=qx, pyL=qy, pwx,pwy; dir_to_world(bx, pxL,pyL, &pwx,&pwy);
        c->depth=depth; c->px=bx->x+pwx; c->py=bx->y+pwy;
        /* normal must point A->B: box->circle is (wnx,wny) */
        if (A->shape==MOTE_C2D_CIRCLE){ c->nx=-wnx; c->ny=-wny; } else { c->nx=wnx; c->ny=wny; }
        return 1;
    }
    /* box vs box: SAT for the axis, then clip the incident face against the reference
     * face's side planes → up to TWO contact points. Two points give a stable resting
     * contact (no corner sink-through / rocking) when cars lie edge-to-edge. */
    {
        float Aux=cosf(A->angle), Auy=sinf(A->angle), Avx=-Auy, Avy=Aux;   /* A local axes (world) */
        float Bux=cosf(B->angle), Buy=sinf(B->angle), Bvx=-Buy, Bvy=Bux;
        float dx=B->x-A->x, dy=B->y-A->y;
        float nxs[4]={Aux,Avx,Bux,Bvx}, nys[4]={Auy,Avy,Buy,Bvy};
        float best=1e30f, bnx=1,bny=0; int bi=0;
        for (int i=0;i<4;i++){
            float nx=nxs[i], ny=nys[i];
            float rA=fabsf(Aux*nx+Auy*ny)*A->hx + fabsf(Avx*nx+Avy*ny)*A->hy;
            float rB=fabsf(Bux*nx+Buy*ny)*B->hx + fabsf(Bvx*nx+Bvy*ny)*B->hy;
            float dist=fabsf(dx*nx+dy*ny);
            float overlap=rA+rB-dist;
            if (overlap<0) return 0;
            if (overlap<best-1e-4f){ best=overlap; bnx=nx; bny=ny; bi=i; }
        }
        if (dx*bnx+dy*bny<0){ bnx=-bnx; bny=-bny; }      /* normal points A -> B */

        /* reference box owns the best axis; ref normal points from ref toward incident */
        int refIsA=(bi<2);
        const MoteBody2D *R=refIsA?A:B, *I=refIsA?B:A;
        float rnx=refIsA?bnx:-bnx, rny=refIsA?bny:-bny;
        float Rux=cosf(R->angle), Ruy=sinf(R->angle), Rvx=-Ruy, Rvy=Rux;
        float Iux=cosf(I->angle), Iuy=sinf(I->angle), Ivx=-Iuy, Ivy=Iux;

        /* incident face = the I edge whose outward normal most opposes rn; its endpoints */
        float dU=Iux*rnx+Iuy*rny, dV=Ivx*rnx+Ivy*rny;
        float e0x,e0y,e1x,e1y;
        if (fabsf(dU) > fabsf(dV)){
            float s = dU>0?-1.0f:1.0f, fx=I->x+s*I->hx*Iux, fy=I->y+s*I->hx*Iuy;
            e0x=fx+I->hy*Ivx; e0y=fy+I->hy*Ivy; e1x=fx-I->hy*Ivx; e1y=fy-I->hy*Ivy;
        } else {
            float s = dV>0?-1.0f:1.0f, fx=I->x+s*I->hy*Ivx, fy=I->y+s*I->hy*Ivy;
            e0x=fx+I->hx*Iux; e0y=fy+I->hx*Iuy; e1x=fx-I->hx*Iux; e1y=fy-I->hx*Iuy;
        }

        /* reference face plane + tangent extent; clip incident edge to the side planes */
        float tx=-rny, ty=rnx;
        float rsup=fabsf(Rux*rnx+Ruy*rny)*R->hx + fabsf(Rvx*rnx+Rvy*rny)*R->hy;
        float rfx=R->x+rnx*rsup, rfy=R->y+rny*rsup;          /* a point on the reference face */
        float rhw=fabsf(Rux*tx+Ruy*ty)*R->hx + fabsf(Rvx*tx+Rvy*ty)*R->hy;
        float p0=(e0x-rfx)*tx+(e0y-rfy)*ty, p1=(e1x-rfx)*tx+(e1y-rfy)*ty;
        float t0=0.0f, t1=1.0f, d=p1-p0;
        if (fabsf(d)>1e-6f){
            float ta=(-rhw-p0)/d, tb=(rhw-p0)/d, tmin=ta<tb?ta:tb, tmax=ta<tb?tb:ta;
            if (tmin>t0) t0=tmin; if (tmax<t1) t1=tmax;
            if (t0>t1){ t0=0.0f; t1=1.0f; }
        }
        /* the two clipped points; keep those behind the reference face (penetrating) */
        int nout=0;
        float ts[2]={t0,t1};
        for (int k=0;k<2;k++){
            float qx=e0x+ts[k]*(e1x-e0x), qy=e0y+ts[k]*(e1y-e0y);
            float sep=(qx-rfx)*rnx+(qy-rfy)*rny;             /* <0 => penetrating */
            if (sep<=0.001f){
                c[nout].nx=bnx; c[nout].ny=bny; c[nout].depth=(sep<0?-sep:0.0f);
                c[nout].px=qx; c[nout].py=qy; nout++;
            }
        }
        if (nout==0){ c[0].nx=bnx; c[0].ny=bny; c[0].depth=best;
                      c[0].px=0.5f*(e0x+e1x); c[0].py=0.5f*(e0y+e1y); nout=1; }
        return nout;
    }
}

static void resolve(MoteBody2D *A, MoteBody2D *B, MoteContact2 *c, float inv_dt){
    if ((A->flags&MOTE_B2D_SENSOR) || (B->flags&MOTE_B2D_SENSOR)) return;
    float imA=A->inv_mass, imB=B->inv_mass, iiA=A->inv_inertia, iiB=B->inv_inertia;
    float sum=imA+imB; if (sum<=0) return;
    float nx=c->nx, ny=c->ny;
    /* contact-relative arms */
    float rax=c->px-A->x, ray=c->py-A->y, rbx=c->px-B->x, rby=c->py-B->y;
    /* relative velocity at contact */
    float rvx=(B->vx - B->avel*rby) - (A->vx - A->avel*ray);
    float rvy=(B->vy + B->avel*rbx) - (A->vy + A->avel*rax);
    float vn=rvx*nx+rvy*ny;
    if (vn>0) { /* separating: still depenetrate below */ }
    else {
        float rest=(A->restitution<B->restitution?A->restitution:B->restitution);
        float ranA=rax*ny-ray*nx, ranB=rbx*ny-rby*nx;
        float kn=sum + iiA*ranA*ranA + iiB*ranB*ranB;
        float jn=-(1.0f+rest)*vn/(kn>1e-8f?kn:1e-8f);
        float ix=jn*nx, iy=jn*ny;
        A->vx-=ix*imA; A->vy-=iy*imA; A->avel-=iiA*(rax*iy-ray*ix);
        B->vx+=ix*imB; B->vy+=iy*imB; B->avel+=iiB*(rbx*iy-rby*ix);
        /* friction */
        rvx=(B->vx - B->avel*rby) - (A->vx - A->avel*ray);
        rvy=(B->vy + B->avel*rbx) - (A->vy + A->avel*rax);
        float tx=-ny, ty=nx; float vt=rvx*tx+rvy*ty;
        float ratA=rax*ty-ray*tx, ratB=rbx*ty-rby*tx;
        float kt=sum + iiA*ratA*ratA + iiB*ratB*ratB;
        float jt=-vt/(kt>1e-8f?kt:1e-8f);
        float mu=sqrtf(A->friction*B->friction), lim=mu*jn;
        jt = jt> lim ?  lim : jt;
        jt = jt<-lim ? -lim : jt;
        float fx=jt*tx, fy=jt*ty;
        A->vx-=fx*imA; A->vy-=fy*imA; A->avel-=iiA*(rax*fy-ray*fx);
        B->vx+=fx*imB; B->vy+=fy*imB; B->avel+=iiB*(rbx*fy-rby*fx);
    }
    /* positional correction (split by inverse mass) */
    float slop=0.005f, perc=0.6f;
    float corr=(c->depth-slop>0?c->depth-slop:0)*perc/sum;
    A->x-=nx*corr*imA; A->y-=ny*corr*imA;
    B->x+=nx*corr*imB; B->y+=ny*corr*imB;
    (void)inv_dt;
}

static void substep(MoteWorld2D *w, MoteBody2D *b, int n, float dt){
    int iters = w->iterations>0 ? w->iterations : 8;
    for (int i=0;i<n;i++){ MoteBody2D *B=&b[i]; if(B->inv_mass<=0) continue;
        B->vx += w->gx*dt; B->vy += w->gy*dt;
        if (B->lin_damp>0){ float f=expf(-B->lin_damp*dt); B->vx*=f; B->vy*=f; }
        if (B->ang_damp>0){ B->avel*=expf(-B->ang_damp*dt); }
    }
    MoteContact2 *ct = s_ct2; int nc=0;
    for (int i=0;i<n;i++) for (int j=i+1;j<n && nc<s_max2c;j++){
        MoteBody2D *A=&b[i], *B=&b[j];
        if (A->inv_mass<=0 && B->inv_mass<=0) continue;
        if (A->mask && B->mask && !(A->mask & B->mask)) continue;
        /* AABB reject */
        float ra=A->shape==MOTE_C2D_CIRCLE?A->radius:(A->hx>A->hy?A->hx:A->hy)*1.4142f;
        float rb=B->shape==MOTE_C2D_CIRCLE?B->radius:(B->hx>B->hy?B->hx:B->hy)*1.4142f;
        float dx=A->x-B->x, dy=A->y-B->y; float rr=ra+rb;
        if (dx*dx+dy*dy > rr*rr) continue;
        MoteContact2 cb[2];
        int m=collide(A,B,cb);
        for (int k=0;k<m && nc<s_max2c;k++){ cb[k].a=i; cb[k].b=j; ct[nc++]=cb[k]; }
    }
    for (int it=0; it<iters; it++)
        for (int k=0;k<nc;k++) resolve(&b[ct[k].a], &b[ct[k].b], &ct[k], 1.0f/dt);
    /* anisotropic (Coulomb-style) lateral friction: remove up to `lat_damp` m/s of the
     * velocity component perpendicular to the body's local axis, per second. A FIXED
     * rate (not a fraction) means a fast sideways slide isn't fully caught — it decays
     * over time, so bodies DRIFT and slide more the faster they're going sideways. A
     * driving game uses this as tyre grip; 0 = off (isotropic free body). */
    for (int i=0;i<n;i++){ MoteBody2D *B=&b[i];
        if (B->inv_mass<=0 || B->lat_damp<=0.0f) continue;
        float c=cosf(B->angle), s=sinf(B->angle);
        float vf = B->vx*c + B->vy*s;                 /* along local axis */
        float lx = B->vx - c*vf, ly = B->vy - s*vf;   /* perpendicular */
        float vlat = sqrtf(lx*lx + ly*ly);
        if (vlat > 1e-4f){
            float nv = vlat - B->lat_damp*dt; if (nv<0) nv=0;   /* friction-limited */
            float sc = nv/vlat; lx*=sc; ly*=sc;
            B->vx = c*vf + lx; B->vy = s*vf + ly;
        }
    }
    for (int i=0;i<n;i++){ MoteBody2D *B=&b[i]; if(B->inv_mass<=0) continue;
        B->x += B->vx*dt; B->y += B->vy*dt; B->angle += B->avel*dt;
        if (w->max_x>w->min_x){
            if (B->x<w->min_x){ B->x=w->min_x; if(B->vx<0)B->vx=-B->vx*B->restitution; }
            if (B->x>w->max_x){ B->x=w->max_x; if(B->vx>0)B->vx=-B->vx*B->restitution; }
            if (B->y<w->min_y){ B->y=w->min_y; if(B->vy<0)B->vy=-B->vy*B->restitution; }
            if (B->y>w->max_y){ B->y=w->max_y; if(B->vy>0)B->vy=-B->vy*B->restitution; }
        }
    }
}

uint32_t mote_phys2d_step(MoteWorld2D *w, MoteBody2D *b, int n, float dt){
    if (!s_ct2 || dt<=0) return 0;         /* not configured / no time */
    if (s_max2b && n>s_max2b) n=s_max2b;
    /* fixed substeps for stability at any frame rate */
    int steps = (int)(dt/SUB_DT); if (steps<1) steps=1; if (steps>6) steps=6;
    float sdt = dt/steps;
    for (int s=0;s<steps;s++) substep(w,b,n,sdt);
    return (uint32_t)n;
}
