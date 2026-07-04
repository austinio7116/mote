/* citygen.h — runtime procedural city generator (C port of tools/citygen.py v9).
 *
 * Generates a full city into a uint8 tile map (same tile chars the baked
 * city_map.h used), so EVERY NEW GAME gets a fresh random city:
 *   rivers with forks/islands/tributaries/estuaries · ragged shores ·
 *   ring road + jogged arterials (bridging water) · per-district BSP streets
 *   with L/T block merges · built-up map rim with a perimeter road inside ·
 *   1-3 bordered parks with lakes + pocket parks · straight harbour roads ·
 *   blocks packed with small buildings, open areas, verges/gardens/pools ·
 *   dead-end pruning and a connectivity pass (one road network, always).
 *
 * MEMORY: designed for the device — no scratch buffers at all. Noise is
 * hashed on the fly (no float grids), "distance to water/road" checks are
 * local window scans, and connectivity floods by in-place marker sweeps.
 * The only storage is the 65 KB map itself (caller-owned, static bss).
 *
 * Single-TU: #include from game.c. assets/city.png remains the reference
 * hand-made map (tools/citypng2map.py can still bake it if ever wanted).
 */
#ifndef CITYGEN_H
#define CITYGEN_H

#define CG_W 254
#define CG_H 256

#define T_ROAD  '.'
#define T_PAVE  ','
#define T_GRASS ' '
#define T_WATER '~'
#define T_BLO   '#'
#define T_BMID  'O'
#define T_BHI   'H'
#define T_BRIDGE 'B'

static uint8_t *cg;                     /* the map being generated */
static uint32_t cg_rng;

static uint32_t cg_rand(void){ cg_rng^=cg_rng<<13; cg_rng^=cg_rng>>17; cg_rng^=cg_rng<<5; return cg_rng; }
static float    cg_frand(void){ return (float)(cg_rand()&0xFFFFFF)/16777216.0f; }
static int      cg_ri(int lo,int hi){ return lo + (int)(cg_rand()%(uint32_t)(hi-lo)); }   /* [lo,hi) */
static float    cg_rf(float lo,float hi){ return lo + cg_frand()*(hi-lo); }

static inline uint8_t cg_at(int x,int y){ return (x<0||y<0||x>=CG_W||y>=CG_H)?T_WATER:cg[y*CG_W+x]; }
static inline void    cg_set(int x,int y,uint8_t c){ if(x>=0&&y>=0&&x<CG_W&&y<CG_H) cg[y*CG_W+x]=c; }

/* ---- hashed value noise (no storage): fbm of 3 octaves ---- */
static float cg_hash(int x,int y,uint32_t salt){
    uint32_t h = (uint32_t)x*374761393u + (uint32_t)y*668265263u + salt*2654435761u;
    h = (h^(h>>13))*1274126177u;
    return (float)((h^(h>>16))&0xFFFF)/65535.0f;
}
static float cg_vnoise(float fx,float fy,uint32_t salt){
    int ix=(int)fx, iy=(int)fy; if(fx<0)ix--; if(fy<0)iy--;
    float tx=fx-ix, ty=fy-iy;
    float a=cg_hash(ix,iy,salt),   b=cg_hash(ix+1,iy,salt);
    float c=cg_hash(ix,iy+1,salt), d=cg_hash(ix+1,iy+1,salt);
    return (a*(1-tx)+b*tx)*(1-ty) + (c*(1-tx)+d*tx)*ty;
}
static float cg_fbm(int x,int y,float base,uint32_t salt){
    float amp=1.0f, tot=0.0f, out=0.0f, f=base/(float)CG_W;
    for(int o=0;o<3;o++){ out+=amp*cg_vnoise(x*f,y*f,salt+o); tot+=amp; amp*=0.5f; f*=2.0f; }
    return out/tot;
}

/* ---- local window scans replace distance transforms ---- */
static int cg_near(int x,int y,uint8_t t,int r){       /* any tile t within Chebyshev r? */
    for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++)
        if(cg_at(x+dx,y+dy)==t) return 1;
    return 0;
}
static int cg_near_roadlike(int x,int y,int r){
    for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
        uint8_t c=cg_at(x+dx,y+dy);
        if(c==T_ROAD||c==T_BRIDGE) return 1;
    }
    return 0;
}

/* ---- primitives ---- */
static void cg_disc(float cx,float cy,float r,uint8_t ch){
    int x0=(int)(cx-r), x1=(int)(cx+r+1), y0=(int)(cy-r), y1=(int)(cy+r+1);
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)
        if((x-cx)*(x-cx)+(y-cy)*(y-cy)<=r*r) cg_set(x,y,ch);
}
/* angular blob: union of offset rects + ragged edge; only paints over `onlyN`
 * tile kinds listed in `only`. Bounded bbox work, no allocations. */
static void cg_patch(int cx,int cy,int size,uint8_t ch,const uint8_t*only,int onlyN){
    int x0=cx-size, y0=cy-size, x1=cx+size, y1=cy+size;
    /* paint marker value 1 into a local stencil via second colour trick:
     * we paint directly but remember bbox for the ragged pass */
    int nr = 3 + size/12;
    for(int k=0;k<nr;k++){
        int w2=cg_ri(size/3>2?size/3:2, (int)(size*0.8f)>3?(int)(size*0.8f):3);
        int h2=cg_ri(size/3>2?size/3:2, (int)(size*0.8f)>3?(int)(size*0.8f):3);
        int ox=cx+cg_ri(-size/2,size/2+1)-w2/2, oy=cy+cg_ri(-size/2,size/2+1)-h2/2;
        for(int y=oy;y<oy+h2;y++)for(int x=ox;x<ox+w2;x++){
            uint8_t c=cg_at(x,y); int ok=0;
            for(int i=0;i<onlyN;i++) if(c==only[i]) ok=1;
            if(ok) cg_set(x,y,ch);
        }
    }
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){       /* ragged edge */
        if(cg_at(x,y)!=ch) continue;
        int n=(cg_at(x+1,y)==ch)+(cg_at(x-1,y)==ch)+(cg_at(x,y+1)==ch)+(cg_at(x,y-1)==ch);
        if(n<=2 && cg_frand()<0.45f){
            uint8_t back = (onlyN>0)?only[0]:T_PAVE;
            cg_set(x,y,back);
        }
    }
}

/* ================================================================= water == */
static void cg_channel(float x,float y,float ex,float ey,float w0,float w1,
                       int steps,int forks,int tribs,int taper_end);
static void cg_channel(float x,float y,float ex,float ey,float w0,float w1,
                       int steps,int forks,int tribs,int taper_end){
    float ang=atan2f(ey-y,ex-x), turn=0, off=0, woff=cg_frand()*7.0f;
    int hold=0;
    for(int i=0;i<steps;i++){
        float t=(float)i/steps;
        if(!(-30<x&&x<CG_W+30&&-30<y&&y<CG_H+30)) break;
        float w=(w0+(w1-w0)*t)*(0.70f+0.55f*fabsf(sinf(t*9+woff)));
        if(taper_end && t>0.65f){ float k=1.0f-(t-0.65f)/0.32f; w*= k>0?k:0; }
        if(w<0.9f) break;
        float px2=-sinf(ang), py2=cosf(ang);
        cg_disc(x+px2*off,y+py2*off,w,T_WATER);
        if(cg_frand()<0.030f){                              /* one-sided cove */
            float side=cg_frand()<0.5f?-1.f:1.f;
            cg_disc(x+px2*(off+side*w*0.85f),y+py2*(off+side*w*0.85f),w*cg_rf(0.45f,0.75f),T_WATER);
        }
        if(hold>0) hold--;
        else { turn=(cg_frand()<0.5f?-1.f:1.f)*cg_rf(0.15f,0.45f); hold=cg_ri(10,26); }
        float bearing=atan2f(ey-y,ex-x);
        float d=fmodf(bearing-ang+3*(float)M_PI,2*(float)M_PI)-(float)M_PI;
        ang += d*0.045f + turn*0.055f + cg_rf(-0.02f,0.02f);
        if(t>0.1f&&t<0.85f){                                /* shy from borders */
            float cx2=x<CG_W-x?x:CG_W-x, cy2=y<CG_H-y?y:CG_H-y;
            if(cx2<26||cy2<26){
                float toc=atan2f(CG_H/2.f-y,CG_W/2.f-x);
                float dd2=fmodf(toc-ang+3*(float)M_PI,2*(float)M_PI)-(float)M_PI;
                ang+=dd2*0.06f;
            }
        }
        off+=cg_rf(-0.35f,0.35f); if(off<-4)off=-4; if(off>4)off=4;
        x+=cosf(ang)*2.0f; y+=sinf(ang)*2.0f;
        if(forks&&cg_frand()<0.012f&&t>0.15f&&t<0.7f){      /* fork -> island */
            forks--;
            float sep=cg_rf(9,24); int dur=cg_ri(35,110);
            float bx=x+px2*sep, by=y+py2*sep, bang=ang;
            for(int j=0;j<dur;j++){
                float fw=w*0.6f*(0.8f+0.4f*sinf(j*0.2f));
                cg_disc(bx,by,fw>2.5f?fw:2.5f,T_WATER);
                float tgt=atan2f((y+sinf(ang)*2*(j+5))-by,(x+cosf(ang)*2*(j+5))-bx);
                float dd=fmodf(tgt-bang+3*(float)M_PI,2*(float)M_PI)-(float)M_PI;
                bang+=dd*(j>dur*0.5f?0.14f:0.02f)+cg_rf(-0.04f,0.04f);
                bx+=cosf(bang)*2.0f; by+=sinf(bang)*2.0f;
            }
        }
        if(tribs&&cg_frand()<0.018f&&t>0.25f){              /* tapering tributary */
            tribs--;
            cg_disc(x,y,w*1.25f,T_WATER);
            float tang=ang+(float)M_PI+(cg_frand()<0.5f?-1.f:1.f)*cg_rf(0.5f,1.1f);
            int ts=cg_ri(40,110);
            cg_channel(x,y,x+cosf(tang)*ts*2.2f,y+sinf(tang)*ts*2.2f,w*0.55f,1.4f,ts,0,0,1);
        }
    }
}

static void cg_edge_pt(int e,float*px,float*py){
    int p=cg_ri(20,(e<2?CG_W:CG_H)-20);
    switch(e){ case 0:*px=p;*py=-8; break; case 1:*px=p;*py=CG_H+8; break;
               case 2:*px=-8;*py=p; break; default:*px=CG_W+8;*py=p; }
}

static void cg_rivers(void){
    int e_in=cg_ri(0,4), e_out=(e_in+cg_ri(1,4))%4;
    float sx,sy,ex,ey;
    cg_edge_pt(e_in,&sx,&sy); cg_edge_pt(e_out,&ex,&ey);
    cg_channel(sx,sy,ex,ey,cg_rf(3.5f,5),cg_rf(11,15),250,2,2,0);
    if(cg_frand()<0.6f){
        int e2=(e_out+2)%4;
        cg_edge_pt(e2,&sx,&sy); cg_edge_pt((e2+2)%4,&ex,&ey);
        cg_channel(sx,sy,ex,ey,cg_rf(3,4.5f),cg_rf(7,10),200,1,1,0);
    }
    int nl=cg_ri(0,3);                                      /* angular lakes */
    for(int i=0;i<nl;i++){
        static const uint8_t any[4]={T_PAVE,T_GRASS,T_ROAD,T_WATER};
        cg_patch(cg_ri(30,CG_W-30),cg_ri(30,CG_H-30),cg_ri(10,18),T_WATER,any,4);
    }
    for(int pass=0;pass<2;pass++){                          /* ragged shores (in place) */
        for(int y=1;y<CG_H-1;y++)for(int x=1;x<CG_W-1;x++){
            int n=(cg_at(x+1,y)==T_WATER)+(cg_at(x-1,y)==T_WATER)
                 +(cg_at(x,y+1)==T_WATER)+(cg_at(x,y-1)==T_WATER);
            uint8_t c=cg_at(x,y);
            if(c!=T_WATER&&n>=2&&cg_frand()<0.28f) cg_set(x,y,T_WATER);
            else if(c==T_WATER&&n<=2&&cg_frand()<0.20f) cg_set(x,y,T_PAVE);
        }
    }
    int isl=cg_ri(2,5);                                     /* lumpy islands in open water */
    for(int i=0;i<isl;i++){
        for(int tr=0;tr<80;tr++){
            int x=cg_ri(12,CG_W-12), y=cg_ri(12,CG_H-12);
            int deep=1;                                     /* water all around? */
            for(int dy=-6;dy<=6&&deep;dy+=3)for(int dx=-6;dx<=6&&deep;dx+=3)
                if(cg_at(x+dx,y+dy)!=T_WATER) deep=0;
            if(!deep) continue;
            static const uint8_t onlyw[1]={T_WATER};
            int big = cg_frand()<0.35f;
            cg_patch(x,y,big?cg_ri(13,24):cg_ri(6,11),T_GRASS,onlyw,1);
            break;
        }
    }
}

static void cg_banks(void){                                 /* clumped bank grass */
    for(int y=0;y<CG_H;y++)for(int x=0;x<CG_W;x++){
        if(cg_at(x,y)!=T_PAVE) continue;
        float cl=cg_fbm(x,y,7,0xBA9C);
        if(cg_near(x,y,T_WATER,2)&&cl>0.52f&&cg_frand()<0.9f) cg_set(x,y,T_GRASS);
        else if(cg_near(x,y,T_WATER,4)&&cl>0.62f&&cg_frand()<0.55f) cg_set(x,y,T_GRASS);
    }
}

/* ================================================================= roads == */
static void cg_stamp(int x,int y,int wid){
    for(int dy=0;dy<wid;dy++)for(int dx=0;dx<wid;dx++){
        uint8_t c=cg_at(x+dx,y+dy);
        if(c==T_WATER) cg_set(x+dx,y+dy,T_BRIDGE);
        else if(x+dx>=0&&y+dy>=0&&x+dx<CG_W&&y+dy<CG_H) cg_set(x+dx,y+dy,T_ROAD);
    }
}
static void cg_jog(int vertical,int pos,int wid){
    int p=pos,t=0,lim=vertical?CG_H:CG_W;
    while(t<lim){
        int seg=cg_ri(40,90);
        for(int s2=0;s2<seg&&t<lim;s2++,t++){
            if(vertical) cg_stamp(p,t,wid); else cg_stamp(t,p,wid);
        }
        if(t<lim){
            int jog=cg_ri(-14,15);
            int lo=p<p+jog?p:p+jog, hi=(p>p+jog?p:p+jog)+wid-1;
            for(int q=lo;q<=hi;q++){ if(vertical) cg_stamp(q,t,wid); else cg_stamp(t,q,wid); }
            t+=wid;
            p+=jog;
            int mx=(vertical?CG_W:CG_H)-wid-10;
            if(p<10)p=10; if(p>mx)p=mx;
        }
    }
}
#define CG_MAXART 4
static int cg_axv[CG_MAXART], cg_naxv, cg_axh[CG_MAXART], cg_naxh;   /* base arterial lines (for districts) */
static void cg_arterials(void){
    int inset=cg_ri(26,44);                                 /* the ring road */
    int x0=inset+cg_ri(-8,9), y0=inset+cg_ri(-8,9);
    int x1=CG_W-inset+cg_ri(-8,9), y1=CG_H-inset+cg_ri(-8,9);
    for(int x=x0;x<x1;x++){ cg_stamp(x,y0,4); cg_stamp(x,y1-4,4); }
    for(int y=y0;y<y1;y++){ cg_stamp(x0,y,4); cg_stamp(x1-4,y,4); }
    cg_naxv=cg_ri(2,4); cg_naxh=cg_ri(2,4);
    for(int i=0;i<cg_naxv;i++){ cg_axv[i]=CG_W*(i+1)/(cg_naxv+1)+cg_ri(-20,21); cg_jog(1,cg_axv[i],4); }
    for(int i=0;i<cg_naxh;i++){ cg_axh[i]=CG_H*(i+1)/(cg_naxh+1)+cg_ri(-20,21); cg_jog(0,cg_axh[i],4); }
    cg_axv[cg_naxv]=x0; cg_axh[cg_naxh]=y0;                 /* ring counts as boundaries too */
}

/* per-district BSP streets: districts are the rects between arterial base lines */
static void cg_bsp(int x0,int y0,int x1,int y1,int depth){
    int w=x1-x0,h=y1-y0;
    int cx=(x0+x1)/2, cy=(y0+y1)/2;
    float dense=cg_fbm(cx,cy,4,0xDE45);
    int stop=11+(int)(11*(1.0f-dense));
    if(w<stop&&h<stop) return;
    int horiz = h>w ? 1 : (w>h ? 0 : (cg_rand()&1));
    if(horiz&&h<8) horiz=0;
    if(!horiz&&w<8) return;
    if(horiz){
        int cut=y0+cg_ri(4,h-4>5?h-4:5);
        for(int x=x0;x<=x1;x++)for(int d2=0;d2<2;d2++){
            uint8_t c=cg_at(x,cut+d2);
            if(c==T_PAVE||c==T_GRASS) cg_set(x,cut+d2,T_ROAD);
        }
        cg_bsp(x0,y0,x1,cut,depth+1); cg_bsp(x0,cut+2,x1,y1,depth+1);
    } else {
        int cut=x0+cg_ri(4,w-4>5?w-4:5);
        for(int y=y0;y<=y1;y++)for(int d2=0;d2<2;d2++){
            uint8_t c=cg_at(cut+d2,y);
            if(c==T_PAVE||c==T_GRASS) cg_set(cut+d2,y,T_ROAD);
        }
        cg_bsp(x0,y0,cut,y1,depth+1); cg_bsp(cut+2,y0,x1,y1,depth+1);
    }
}
static void cg_sort(int*v,int n){ for(int i=1;i<n;i++){ int k=v[i],j=i-1;
    while(j>=0&&v[j]>k){ v[j+1]=v[j]; j--; } v[j+1]=k; } }
static void cg_streets(void){
    int xs[CG_MAXART+3], ys[CG_MAXART+3], nx=0, ny=0;
    xs[nx++]=5; ys[ny++]=5;
    for(int i=0;i<cg_naxv+1;i++) xs[nx++]=cg_axv[i];
    for(int i=0;i<cg_naxh+1;i++) ys[ny++]=cg_axh[i];
    xs[nx++]=CG_W-5; ys[ny++]=CG_H-5;
    cg_sort(xs,nx); cg_sort(ys,ny);
    for(int i=0;i+1<nx;i++)for(int j=0;j+1<ny;j++)
        cg_bsp(xs[i]+4,ys[j]+4,xs[i+1]-2,ys[j+1]-2,0);
    /* merge blocks into L/T shapes: delete random LOCAL street segments */
    int cuts=cg_ri(10,18);
    for(int c2=0;c2<cuts;c2++){
        for(int tr=0;tr<60;tr++){
            int x=cg_ri(9,CG_W-9), y=cg_ri(9,CG_H-9);
            if(cg_at(x,y)!=T_ROAD) continue;
            int horiz = (cg_at(x-1,y)==T_ROAD||cg_at(x+1,y)==T_ROAD);
            int wide = horiz ? (cg_at(x,y-1)==T_ROAD)+(cg_at(x,y+1)==T_ROAD)
                             : (cg_at(x-1,y)==T_ROAD)+(cg_at(x+1,y)==T_ROAD);
            if(wide>1) continue;                            /* arterial: leave it */
            int ln=cg_ri(5,12);
            for(int i=0;i<ln;i++){
                int xx=horiz?x+i:x, yy=horiz?y:y+i;
                if(cg_at(xx,yy)==T_ROAD) cg_set(xx,yy,T_PAVE);
                if(horiz&&cg_at(xx,yy+1)==T_ROAD) cg_set(xx,yy+1,T_PAVE);
                if(!horiz&&cg_at(xx+1,yy)==T_ROAD) cg_set(xx+1,yy,T_PAVE);
            }
            break;
        }
    }
}

static void cg_rim(void){
    const int B=5;
    for(int y=0;y<CG_H;y++)for(int x=0;x<CG_W;x++)
        if((x<B||x>=CG_W-B||y<B||y>=CG_H-B)&&cg_at(x,y)==T_ROAD) cg_set(x,y,T_PAVE);
    for(int t=B;t<CG_W-B;t++)for(int d2=0;d2<2;d2++){        /* perimeter road inside the rim */
        if(cg_at(t,B+d2)==T_PAVE) cg_set(t,B+d2,T_ROAD);
        if(cg_at(t,CG_H-B-1-d2)==T_PAVE) cg_set(t,CG_H-B-1-d2,T_ROAD);
    }
    for(int t=B;t<CG_H-B;t++)for(int d2=0;d2<2;d2++){
        if(cg_at(B+d2,t)==T_PAVE) cg_set(B+d2,t,T_ROAD);
        if(cg_at(CG_W-B-1-d2,t)==T_PAVE) cg_set(CG_W-B-1-d2,t,T_ROAD);
    }
}

static void cg_prune(void){                                 /* dead-end stubs -> pavement */
    for(int pass=0;pass<8;pass++){
        int changed=0;
        for(int y=1;y<CG_H-1;y++)for(int x=1;x<CG_W-1;x++){
            if(cg_at(x,y)!=T_ROAD) continue;
            int n=0;
            uint8_t c;
            c=cg_at(x+1,y); n+=(c==T_ROAD||c==T_BRIDGE);
            c=cg_at(x-1,y); n+=(c==T_ROAD||c==T_BRIDGE);
            c=cg_at(x,y+1); n+=(c==T_ROAD||c==T_BRIDGE);
            c=cg_at(x,y-1); n+=(c==T_ROAD||c==T_BRIDGE);
            if(n<=1){ cg_set(x,y,T_PAVE); changed=1; }
        }
        if(!changed) break;
    }
}

/* ================================================================= parks == */
static void cg_make_park(int cx,int cy,int size){
    static const uint8_t pr[2]={T_PAVE,T_ROAD};
    cg_patch(cx,cy,size,T_GRASS,pr,2);
    /* a PAVEMENT margin around the park (the original rings its parks with
     * pavement + a few buildings, not tarmac): push any street within 2 cells
     * of the green back to pavement — the fill pass then dots it with buildings */
    int x0=cx-size-3,y0=cy-size-3,x1=cx+size+3,y1=cy+size+3;
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
        if(cg_at(x,y)!=T_ROAD) continue;
        if(cg_near(x,y,T_GRASS,2)) cg_set(x,y,T_PAVE);
    }
    int lakes=1+(cg_frand()<0.4f);                           /* lake(s) inside */
    for(int l=0;l<lakes;l++){
        for(int tr=0;tr<40;tr++){
            int x=cx+cg_ri(-size/2,size/2+1), y=cy+cg_ri(-size/2,size/2+1);
            int inner=1;
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)
                if(cg_at(x+dx,y+dy)!=T_GRASS) inner=0;
            if(!inner) continue;
            static const uint8_t g1[1]={T_GRASS};
            cg_patch(x,y,cg_ri(5,9),T_WATER,g1,1);
            break;
        }
    }
}
static void cg_parks(void){
    int px[3],py[3],np=0;
    px[np]=CG_W/2+cg_ri(-40,41); py[np]=CG_H/2+cg_ri(-40,41); np++;
    int extra=cg_ri(0,3);
    for(int i=0;i<extra&&np<3;i++){ px[np]=cg_ri(35,CG_W-35); py[np]=cg_ri(35,CG_H-35); np++; }
    for(int i=0;i<np;i++){
        int ok=1;
        for(int j=0;j<i;j++)
            if((px[i]-px[j])*(px[i]-px[j])+(py[i]-py[j])*(py[i]-py[j])<60*60) ok=0;
        if(ok) cg_make_park(px[i],py[i],cg_ri(16,26));
    }
    int pockets=cg_ri(4,8);                                  /* whole-block pocket parks */
    for(int p2=0;p2<pockets;p2++){
        for(int tr=0;tr<50;tr++){
            int x=cg_ri(10,CG_W-10), y=cg_ri(10,CG_H-10);
            if(cg_at(x,y)!=T_PAVE||cg_near_roadlike(x,y,1)) continue;
            /* bounded flood of this block; abort if too big */
            static uint16_t st[520]; int sp=0, n=0; static uint16_t cells[420];
            st[sp++]=(uint16_t)(y*CG_W+x); cg_set(x,y,1);    /* 1 = temp mark */
            int overflow=0;
            while(sp>0){
                uint16_t idx=st[--sp]; if(n>=420){overflow=1;break;}
                cells[n++]=idx;
                int xx=idx%CG_W, yy=idx/CG_W;
                static const int DX[4]={1,-1,0,0},DY[4]={0,0,1,-1};
                for(int k=0;k<4;k++){
                    int nx2=xx+DX[k],ny2=yy+DY[k];
                    if(cg_at(nx2,ny2)==T_PAVE&&sp<518){ cg_set(nx2,ny2,1); st[sp++]=(uint16_t)(ny2*CG_W+nx2); }
                }
            }
            uint8_t final = (!overflow&&n>=25&&n<=140)?T_GRASS:T_PAVE;
            for(int i=0;i<n;i++) cg[cells[i]]=final;
            /* clear any temp marks left on the stack when we overflowed */
            if(overflow){ for(int i=0;i<CG_W*CG_H;i++) if(cg[i]==1) cg[i]=T_PAVE; }
            if(final==T_GRASS) break;
        }
    }
}

static void cg_harbour(void){                                /* straight dockside roads */
    int laid=0;
    for(int y=6;y<CG_H-6&&laid<4;y++){
        int run=0,start=0;
        for(int x=6;x<CG_W-6;x++){
            uint8_t c=cg_at(x,y);
            int shore=(c==T_PAVE||c==T_GRASS)&&cg_near(x,y,T_WATER,2)&&!cg_near(x,y,T_WATER,1);
            if(shore){ if(!run)start=x; run++; }
            else{
                if(run>=14){
                    for(int xx=start;xx<start+run;xx++)for(int d2=0;d2<2;d2++){
                        uint8_t cc=cg_at(xx,y-d2);
                        if(cc==T_PAVE||cc==T_GRASS) cg_set(xx,y-d2,T_ROAD);
                    }
                    laid++; if(laid>=4)break;
                }
                run=0;
            }
        }
    }
    for(int x=6;x<CG_W-6&&laid<4;x++){
        int run=0,start=0;
        for(int y=6;y<CG_H-6;y++){
            uint8_t c=cg_at(x,y);
            int shore=(c==T_PAVE||c==T_GRASS)&&cg_near(x,y,T_WATER,2)&&!cg_near(x,y,T_WATER,1);
            if(shore){ if(!run)start=y; run++; }
            else{
                if(run>=14){
                    for(int yy=start;yy<start+run;yy++)for(int d2=0;d2<2;d2++){
                        uint8_t cc=cg_at(x-d2,yy);
                        if(cc==T_PAVE||cc==T_GRASS) cg_set(x-d2,yy,T_ROAD);
                    }
                    laid++; if(laid>=4)break;
                }
                run=0;
            }
        }
    }
}

/* ================================================================= blocks == */
static void cg_fill(void){
    /* pseudo-random permutation over all cells: idx = (a*i+b) mod N, a coprime N */
    const uint32_t N=CG_W*CG_H;
    uint32_t a=10007u; while((N%a)==0) a+=2;                 /* coprime with N=65024 */
    uint32_t b=cg_rand()%N;
    /* dress OPEN areas first: trees + rare pools in the no-build noise zones */
    for(uint32_t i=0;i<N;i++){
        uint32_t idx=(uint32_t)(((uint64_t)a*i+b)%N);
        int x=idx%CG_W, y=idx/CG_W;
        if(cg[idx]!=T_PAVE) continue;
        float open=cg_fbm(x,y,6,0x09E4);
        if(open<=0.63f||cg_near_roadlike(x,y,1)) continue;
        float r=cg_frand();
        if(r<0.045f) cg[idx]=T_GRASS;
        else if(r<0.052f&&!cg_near_roadlike(x,y,2)) cg[idx]=T_WATER;
    }
    for(uint32_t i=0;i<N;i++){                               /* building rects */
        uint32_t idx=(uint32_t)(((uint64_t)a*i+b+7)%N);
        int x=idx%CG_W, y=idx/CG_W;
        if(cg[idx]!=T_PAVE||cg_near_roadlike(x,y,1)) continue;
        float open=cg_fbm(x,y,6,0x09E4);
        if(open>0.63f) continue;
        float dense=cg_fbm(x,y,4,0xDE45);
        if(cg_frand()>0.60f+0.35f*dense) continue;
        int bw=cg_ri(1,5), bh=cg_ri(1,4), ok=1;
        for(int j=0;j<bh&&ok;j++)for(int i2=0;i2<bw&&ok;i2++){
            int cx=x+i2, cy=y+j;
            if(cg_at(cx,cy)!=T_PAVE||cg_near_roadlike(cx,cy,1)) ok=0;
        }
        if(!ok) continue;
        float k=cg_fbm(x,y,5,0x4B1D)+cg_rf(-0.18f,0.18f);
        uint8_t ch=k>0.60f?T_BHI:(k>0.42f?T_BMID:T_BLO);
        for(int j=0;j<bh;j++)for(int i2=0;i2<bw;i2++) cg_set(x+i2,y+j,ch);
    }
    for(uint32_t i=0;i<N;i++){                               /* tiny infill buildings */
        uint32_t idx=(uint32_t)(((uint64_t)a*i+b+13)%N);
        int x=idx%CG_W, y=idx/CG_W;
        if(cg[idx]!=T_PAVE||cg_near_roadlike(x,y,1)) continue;
        float open=cg_fbm(x,y,6,0x09E4);
        if(open>0.63f) continue;
        float dense=cg_fbm(x,y,4,0xDE45);
        if(cg_frand()>0.45f+0.40f*dense) continue;
        float k=cg_fbm(x,y,5,0x4B1D)+cg_rf(-0.22f,0.22f);
        uint8_t ch=k>0.60f?T_BHI:(k>0.42f?T_BMID:T_BLO);
        cg[idx]=ch;
        if(cg_frand()<0.5f){
            int nx=x+cg_ri(0,2), ny=y+cg_ri(0,2);
            if(cg_at(nx,ny)==T_PAVE&&!cg_near_roadlike(nx,ny,1)) cg_set(nx,ny,ch);
        }
    }
}

static void cg_micro(void){                                  /* verges, gardens, pools */
    int verges=cg_ri(55,90);
    for(int tr=0;tr<1600&&verges>0;tr++){
        int x=cg_ri(2,CG_W-2), y=cg_ri(2,CG_H-2);
        if(cg_at(x,y)!=T_PAVE) continue;
        int nx=cg_near_roadlike(x,y,1);
        if(!nx) continue;
        int horiz=(cg_at(x,y-1)==T_ROAD||cg_at(x,y+1)==T_ROAD);
        int ln=cg_ri(2,7);
        for(int i=0;i<ln;i++){
            int xx=horiz?x+i:x, yy=horiz?y:y+i;
            if(cg_at(xx,yy)==T_PAVE) cg_set(xx,yy,T_GRASS);
        }
        verges--;
    }
    int gardens=cg_ri(18,32);
    for(int g2=0;g2<gardens;g2++){
        for(int tr=0;tr<30;tr++){
            int x=cg_ri(2,CG_W-5), y=cg_ri(2,CG_H-5);
            int gw=cg_ri(2,4),gh=cg_ri(2,4),ok=1;
            for(int j=0;j<gh&&ok;j++)for(int i2=0;i2<gw&&ok;i2++)
                if(cg_at(x+i2,y+j)!=T_PAVE||cg_near_roadlike(x+i2,y+j,1)) ok=0;
            if(!ok) continue;
            for(int j=0;j<gh;j++)for(int i2=0;i2<gw;i2++) cg_set(x+i2,y+j,T_GRASS);
            break;
        }
    }
    int pools=cg_ri(10,20);
    for(int p2=0;p2<pools;p2++){
        for(int tr=0;tr<40;tr++){
            int x=cg_ri(3,CG_W-4), y=cg_ri(3,CG_H-4);
            if(cg_fbm(x,y,4,0xDE45)>0.5f) continue;
            int pw=cg_ri(1,3),ph=cg_ri(1,3),ok=1;
            for(int j=0;j<ph&&ok;j++)for(int i2=0;i2<pw&&ok;i2++)
                if(cg_at(x+i2,y+j)!=T_PAVE||cg_near_roadlike(x+i2,y+j,2)) ok=0;
            if(!ok) continue;
            for(int j=0;j<ph;j++)for(int i2=0;i2<pw;i2++) cg_set(x+i2,y+j,T_WATER);
            break;
        }
    }
}

/* ======================================================== connectivity ===== */
/* flood the road network in place with marker values, sweeping until stable;
 * then bridge each unreached road cluster toward the reached set. */
#define M_R 1   /* reached road */
#define M_B 2   /* reached bridge */
static void cg_sweep_flood(void){
    for(int pass=0;pass<200;pass++){
        int changed=0;
        for(int y=0;y<CG_H;y++)for(int x=0;x<CG_W;x++){
            uint8_t c=cg_at(x,y);
            if(c!=T_ROAD&&c!=T_BRIDGE) continue;
            int adj=0;
            uint8_t n;
            n=cg_at(x+1,y); adj|=(n==M_R||n==M_B);
            n=cg_at(x-1,y); adj|=(n==M_R||n==M_B);
            n=cg_at(x,y+1); adj|=(n==M_R||n==M_B);
            n=cg_at(x,y-1); adj|=(n==M_R||n==M_B);
            if(adj){ cg_set(x,y,c==T_ROAD?M_R:M_B); changed=1; }
        }
        for(int y=CG_H-1;y>=0;y--)for(int x=CG_W-1;x>=0;x--){
            uint8_t c=cg_at(x,y);
            if(c!=T_ROAD&&c!=T_BRIDGE) continue;
            int adj=0;
            uint8_t n;
            n=cg_at(x+1,y); adj|=(n==M_R||n==M_B);
            n=cg_at(x-1,y); adj|=(n==M_R||n==M_B);
            n=cg_at(x,y+1); adj|=(n==M_R||n==M_B);
            n=cg_at(x,y-1); adj|=(n==M_R||n==M_B);
            if(adj){ cg_set(x,y,c==T_ROAD?M_R:M_B); changed=1; }
        }
        if(!changed) break;
    }
}
static void cg_connect(void){
    /* seed: the first road cell */
    int seeded=0;
    for(int y=0;y<CG_H&&!seeded;y++)for(int x=0;x<CG_W&&!seeded;x++)
        if(cg_at(x,y)==T_ROAD){ cg_set(x,y,M_R); seeded=1; }
    if(!seeded) return;
    cg_sweep_flood();
    for(int round=0;round<24;round++){
        /* find an unreached road cell */
        int ux=-1,uy=-1;
        for(int y=0;y<CG_H&&ux<0;y++)for(int x=0;x<CG_W&&ux<0;x++)
            if(cg_at(x,y)==T_ROAD){ ux=x; uy=y; }
        if(ux<0) break;
        /* nearest reached cell */
        int bx=-1,by=-1; long long bd=(long long)1<<60;
        for(int y=0;y<CG_H;y++)for(int x=0;x<CG_W;x++){
            uint8_t c=cg_at(x,y);
            if(c!=M_R&&c!=M_B) continue;
            long long d=(long long)(x-ux)*(x-ux)+(long long)(y-uy)*(y-uy);
            if(d<bd){bd=d;bx=x;by=y;}
        }
        if(bx<0) break;
        int x=ux,y=uy;                                       /* L-walk, bridging water */
        while(x!=bx||y!=by){
            if(x<bx)x++; else if(x>bx)x--; else if(y<by)y++; else y--;
            static const int LX[3]={0,1,0}, LY[3]={0,0,1};   /* 2-wide-ish lane */
            for(int k=0;k<3;k++){
                uint8_t c=cg_at(x+LX[k],y+LY[k]);
                if(c==T_WATER) cg_set(x+LX[k],y+LY[k],T_BRIDGE);
                else if(c==T_PAVE||c==T_GRASS||c==T_BLO||c==T_BMID||c==T_BHI)
                    cg_set(x+LX[k],y+LY[k],T_ROAD);
            }
        }
        cg_set(ux,uy,M_R);
        cg_sweep_flood();
    }
    /* erase remaining unreached crumbs; restore markers */
    for(int i=0;i<CG_W*CG_H;i++){
        if(cg[i]==T_ROAD) cg[i]=T_PAVE;
        else if(cg[i]==T_BRIDGE) cg[i]=T_WATER;
        else if(cg[i]==M_R) cg[i]=T_ROAD;
        else if(cg[i]==M_B) cg[i]=T_BRIDGE;
    }
}

static void cg_fix_pool_roads(void){
    /* a small water blob (pool/fountain/pond crumb) must never touch a road:
     * if any cell of a blob <=32 cells is 4-adjacent to road, fill the blob */
    for(int y=1;y<CG_H-1;y++)for(int x=1;x<CG_W-1;x++){
        if(cg_at(x,y)!=T_WATER) continue;
        uint8_t n1=cg_at(x+1,y),n2=cg_at(x-1,y),n3=cg_at(x,y+1),n4=cg_at(x,y-1);
        if(n1!=T_ROAD&&n2!=T_ROAD&&n3!=T_ROAD&&n4!=T_ROAD) continue;
        /* bounded flood of this water blob */
        static uint16_t st[80], cells[64]; int sp=0,n=0,overflow=0;
        st[sp++]=(uint16_t)(y*CG_W+x); cg_set(x,y,1);
        while(sp>0){
            uint16_t idx=st[--sp];
            if(n>=64){overflow=1;break;}
            cells[n++]=idx;
            int xx=idx%CG_W, yy=idx/CG_W;
            static const int DX[4]={1,-1,0,0},DY[4]={0,0,1,-1};
            for(int k=0;k<4;k++){
                int nx2=xx+DX[k],ny2=yy+DY[k];
                if(cg_at(nx2,ny2)==T_WATER&&sp<78){ cg_set(nx2,ny2,1); st[sp++]=(uint16_t)(ny2*CG_W+nx2); }
            }
        }
        uint8_t final = overflow ? T_WATER : T_PAVE;       /* big water = a real river: leave it */
        for(int i=0;i<n;i++) cg[cells[i]]=final;
        if(overflow){ for(int i=0;i<CG_W*CG_H;i++) if(cg[i]==1) cg[i]=T_WATER; }
    }
}

static void cg_bridge_walkways(void){
    /* every bridge gets pavement flanks so pedestrians can cross the water */
    for(int y=1;y<CG_H-1;y++)for(int x=1;x<CG_W-1;x++){
        if(cg_at(x,y)!=T_BRIDGE) continue;
        static const int DX[4]={1,-1,0,0},DY[4]={0,0,1,-1};
        for(int k=0;k<4;k++)
            if(cg_at(x+DX[k],y+DY[k])==T_WATER) cg_set(x+DX[k],y+DY[k],T_PAVE);
    }
}

static void cg_seal_border(void){
    /* the outer border is NEVER walkable: any pavement/grass in the outer two
     * rings becomes building (water and the rare bridge stay) */
    for(int y=0;y<CG_H;y++)for(int x=0;x<CG_W;x++){
        if(x>=2&&x<CG_W-2&&y>=2&&y<CG_H-2) continue;
        uint8_t c=cg_at(x,y);
        if(c!=T_PAVE&&c!=T_GRASS&&c!=T_ROAD) continue;
        float k=cg_fbm(x,y,5,0x4B1D);
        cg_set(x,y, k>0.60f?T_BHI:(k>0.42f?T_BMID:T_BLO));
    }
}

/* ==================================================================== API == */
static void citygen(uint8_t *map, uint32_t seed){
    cg = map; cg_rng = seed?seed:0xC17E5EEDu;
    for(int i=0;i<CG_W*CG_H;i++) cg[i]=T_PAVE;
    cg_rivers();
    cg_banks();
    cg_arterials();
    cg_streets();
    cg_rim();
    cg_prune();
    cg_parks();
    cg_harbour();
    cg_prune();
    cg_fill();
    cg_micro();
    cg_connect();
    cg_prune();
    cg_bridge_walkways();
    cg_fix_pool_roads();
    cg_seal_border();
}

#endif /* CITYGEN_H */
