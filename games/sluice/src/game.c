/*
 * SLUICE — a lava-diversion puzzle for the Thumby Color.
 *
 * A fissure has opened on the ridge above a town. Thick, glowing lava oozes
 * down the hillside toward the houses. You are an engineer with a headlamp:
 * dig diversion channels into the earth, throw up berms, and spray coolant to
 * freeze the flow into stone walls — steering the ooze into the ravine and
 * AROUND the town before it burns it down. (Heimaey, 1973.)
 *
 * ARCHITECTURE (see the mote skill):
 *   - The world is a 128x128 per-pixel material grid. Lava is a THICK VISCOUS
 *     fluid: only molten cells creep; heat conducts along the flow so the front
 *     stays bright while the trail cools into a dark basalt CRUST shot with
 *     glowing cracks, and finally hardens to solid obsidian rock. update()
 *     (core0) steps the sim on a fixed clock and rebuilds a warm LIGHT FIELD.
 *   - render_band() owns the whole framebuffer on BOTH cores: dusk sky, lit
 *     terrain, crust-cracked lava, the town, and the flowing warm light — so
 *     the landscape re-lights every frame as the lava moves.
 *   - overlay() draws the HUD.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

MOTE_GAME_META("Sluice", "austinio7116");
MOTE_GAME_VERSION("0.1.0");

/* ============================================================ constants === */
#define W  128
#define H  128
#define N  (W * H)
#define LW 64
#define LH 64
#define LN (LW * LH)

enum {
    M_EMPTY = 0,   /* sky / air */
    M_ROCK,        /* bedrock — undiggable */
    M_DIRT,        /* earth — diggable */
    M_WALL,        /* player berm */
    M_OBSID,       /* hardened lava — solid rock */
    M_DRAIN,       /* ravine floor — swallows lava */
    M_BUILD,       /* town building — protect it */
    M_LAVA,        /* thick molten/crusting lava */
    M_WATER        /* coolant */
};
#define IS_SOLID(m)    ((m)==M_ROCK||(m)==M_DIRT||(m)==M_WALL||(m)==M_OBSID||(m)==M_BUILD)
#define IS_DIGGABLE(m) ((m)==M_DIRT||(m)==M_OBSID)

/* lava heat thresholds */
#define LAVA_HOT   255
#define LAVA_MOVE  96      /* below this the crust is too stiff to creep */
#define LAVA_FREEZE 22     /* below this it hardens to obsidian */

enum { T_DIG = 0, T_DAM, T_COOL };
enum { ST_TITLE = 0, ST_PLAY, ST_WIN, ST_LOSE };

/* ============================================================== globals === */
static uint8_t  mat[N];
static uint8_t  heat[N];       /* lava molten-ness / obsidian cool timer */
static uint8_t  moved[N];
static uint8_t  crackmap[N];   /* static basalt crack veins (0..255) */
static uint16_t light[LN], light2[LN];

static uint16_t lava_lut[256]; /* molten ramp */
static uint16_t obs_lut[256];  /* cooling basalt ramp */
static uint16_t sky_lut[H];    /* dusk sky gradient by row */

#define MAXSRC 3
static int src_x[MAXSRC], src_y[MAXSRC], nsrc = 0;

/* town buildings double as the safe zone */
#define MAXBLD 8
static int bld_x[MAXBLD], bld_y[MAXBLD], bld_w[MAXBLD], bld_h[MAXBLD], nbld = 0;
static float town_int = 100;              /* town integrity */

#define MAXP 200
typedef struct { float x, y, vx, vy, life, max; uint8_t kind; } Part; /* 0 ember 1 steam 2 ash */
static Part parts[MAXP];
static int  np = 0;

/* engineer */
static float mx, my, mvx, mvy;
#define MW_ 5
#define MH_ 8
static int   grounded = 0, facing = 1, aim = 0, tool = T_DIG;
static float tool_cd = 0;

static float coolant, dam_charges;
static int   surge_left, surge_max, drained;
static float src_acc;
static int   level = 1, state = ST_TITLE;
static float state_t = 0;

static uint32_t rng = 0x1234abcd;
static float    ca_acc = 0;
static uint32_t framestep = 0;
static int      lose_cooked = 0;   /* 0 = town burned, 1 = engineer melted */
static int      test_mode = 0;     /* SLUICE_TEST: park engineer, no death (flow capture) */

/* =============================================================== utils === */
static inline uint32_t rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static inline float    rndf(void){ return (float)(rnd()&0xFFFFFF)/(float)0x1000000; }

static inline uint16_t lerp565(uint16_t a, uint16_t b, float t){
    int ar=a>>11, ag=(a>>5)&63, ab=a&31, br=b>>11, bg=(b>>5)&63, bb=b&31;
    int r=ar+(int)((br-ar)*t), g=ag+(int)((bg-ag)*t), bl=ab+(int)((bb-ab)*t);
    return (uint16_t)((r<<11)|(g<<5)|bl);
}
static inline uint16_t add565(uint16_t d, int ar, int ag, int ab){
    int r=(d>>11)+ar; if(r>31)r=31; int g=((d>>5)&63)+ag; if(g>63)g=63; int b=(d&31)+ab; if(b>31)b=31;
    return (uint16_t)((r<<11)|(g<<5)|b);
}

static void build_luts(void){
    struct { int at; uint16_t c; } lk[] = {              /* molten lava */
        {  0, MOTE_RGB565(120, 20,  4) },
        { 90, MOTE_RGB565(210, 55,  8) },
        {150, MOTE_RGB565(250,110, 18) },
        {200, MOTE_RGB565(255,170, 50) },
        {235, MOTE_RGB565(255,215,110) },
        {255, MOTE_RGB565(255,246,205) },
    };
    for(int h=0;h<256;h++){ int s=0; while(s<4&&h>lk[s+1].at)s++;
        int a=lk[s].at,b=lk[s+1].at; float t=(b>a)?(float)(h-a)/(b-a):0; lava_lut[h]=lerp565(lk[s].c,lk[s+1].c,t); }
    struct { int at; uint16_t c; } ok[] = {              /* cooling basalt */
        {  0, MOTE_RGB565( 22, 18, 26) },
        { 50, MOTE_RGB565( 40, 26, 30) },
        {110, MOTE_RGB565( 74, 34, 30) },
        {170, MOTE_RGB565(140, 54, 24) },
        {255, MOTE_RGB565(210, 90, 30) },
    };
    for(int h=0;h<256;h++){ int s=0; while(s<3&&h>ok[s+1].at)s++;
        int a=ok[s].at,b=ok[s+1].at; float t=(b>a)?(float)(h-a)/(b-a):0; obs_lut[h]=lerp565(ok[s].c,ok[s+1].c,t); }
    /* dusk volcanic sky: deep indigo up top -> smoky maroon at the horizon */
    for(int y=0;y<H;y++){ float t=(float)y/(H-1);
        sky_lut[y]=lerp565(MOTE_RGB565(10,8,22), MOTE_RGB565(46,18,26), t*t); }
}

/* smooth value noise 0..1 from a coarse random lattice */
static float vnoise(float x, float y, uint32_t seed){
    int xi=(int)floorf(x), yi=(int)floorf(y); float fx=x-xi, fy=y-yi;
    fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy);
    #define HH(a,b) ({ uint32_t h=(uint32_t)((a)*374761393u+(b)*668265263u+seed*362437u); \
                       h=(h^(h>>13))*1274126177u; (float)((h>>8)&0xFFFF)/65535.0f; })
    float a=HH(xi,yi), b=HH(xi+1,yi), c=HH(xi,yi+1), d=HH(xi+1,yi+1);
    #undef HH
    return a+(b-a)*fx + (c-a)*fy + (a-b-c+d)*fx*fy;
}

/* ========================================================= particles === */
static void spawn_part(float x,float y,float vx,float vy,float life,int kind){
    if(np>=MAXP){ int w=0; float wl=1e9f; for(int i=0;i<np;i++) if(parts[i].life<wl){wl=parts[i].life;w=i;}
        parts[w]=(Part){x,y,vx,vy,life,life,(uint8_t)kind}; return; }
    parts[np++]=(Part){x,y,vx,vy,life,life,(uint8_t)kind};
}
static void tick_particles(float dt){
    for(int i=0;i<np;){ Part*p=&parts[i]; p->life-=dt;
        if(p->life<=0){ parts[i]=parts[--np]; continue; }
        p->x+=p->vx*dt; p->y+=p->vy*dt;
        if(p->kind==0){ p->vy-=24*dt; p->vx+=(rndf()-0.5f)*40*dt; }
        else if(p->kind==1){ p->vy-=20*dt; p->vx*=0.96f; }
        else { p->vy-=4*dt; p->vx+=(rndf()-0.5f)*8*dt; }   /* ash drifts */
        int cx=(int)p->x, cy=(int)p->y;
        if(cx<1||cx>=W-1||cy<1||cy>=H-1){ parts[i]=parts[--np]; continue; }
        if(p->kind!=2 && IS_SOLID(mat[cy*W+cx])){ parts[i]=parts[--np]; continue; }
        i++;
    }
}

/* =========================================================== lava CA === */
/* Only MOLTEN lava creeps, and slowly (viscosity). Heat conducts up the flow so
 * the front stays hot; the trail cools, crusts, and hardens to obsidian. */
static inline int air(int t){ return mat[t]==M_EMPTY; }

static void ca_step(void){
    memset(moved,0,N);
    int dir = framestep&1; framestep++;

    /* --- movement (thick, viscous) --- */
    for(int y=H-2;y>=1;y--){
        for(int xi=0;xi<W;xi++){
            int x = dir?(W-1-xi):xi;
            int i = y*W+x; uint8_t m = mat[i];
            if(moved[i]) continue;

            if(m==M_WATER){
                int below=i+W;
                if(mat[below]==M_DRAIN){ mat[i]=M_EMPTY; continue; }
                if(air(below)){ mat[below]=M_WATER; mat[i]=M_EMPTY; moved[below]=1; continue; }
                int dl=below-1,dr=below+1,d1=dir?dr:dl,d2=dir?dl:dr;
                if(air(d1)){mat[d1]=M_WATER;mat[i]=M_EMPTY;moved[d1]=1;continue;}
                if(air(d2)){mat[d2]=M_WATER;mat[i]=M_EMPTY;moved[d2]=1;continue;}
                int s1=dir?i+1:i-1,s2=dir?i-1:i+1;
                if(air(s1)){mat[s1]=M_WATER;mat[i]=M_EMPTY;moved[s1]=1;continue;}
                if(air(s2)){mat[s2]=M_WATER;mat[i]=M_EMPTY;moved[s2]=1;continue;}
                continue;
            }
            if(m!=M_LAVA) continue;

            int below=i+W;
            if(mat[below]==M_DRAIN){ drained++; mat[i]=M_EMPTY; heat[i]=0; continue; }

            /* stiff crust barely moves */
            if(heat[i] < LAVA_MOVE) continue;

            /* gravity: fall if unsupported (still viscous — not every step) */
            if(air(below) && (rnd()&1)){ mat[below]=M_LAVA; heat[below]=heat[i]; mat[i]=M_EMPTY; heat[i]=0; moved[below]=1; continue; }
            int dl=below-1,dr=below+1,d1=dir?dr:dl,d2=dir?dl:dr;
            if(air(d1)&&(rnd()&3)==0){mat[d1]=M_LAVA;heat[d1]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[d1]=1;continue;}
            if(air(d2)&&(rnd()&3)==0){mat[d2]=M_LAVA;heat[d2]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[d2]=1;continue;}
            /* lateral ooze — lets pools spread, thicken and overflow while hot */
            if(heat[i] > 130 && (rnd()&3)==0){
                int s1=dir?i+1:i-1,s2=dir?i-1:i+1;
                if(air(s1)){mat[s1]=M_LAVA;heat[s1]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[s1]=1;continue;}
                if(air(s2)){mat[s2]=M_LAVA;heat[s2]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[s2]=1;continue;}
            }
        }
    }

    /* --- thermal: conduction keeps the front hot, trail crusts & hardens --- */
    for(int i=W;i<N-W;i++){
        uint8_t m=mat[i];
        if(m==M_LAVA){
            /* quench on contact with water */
            if(mat[i-1]==M_WATER||mat[i+1]==M_WATER||mat[i-W]==M_WATER||mat[i+W]==M_WATER){
                int wn = mat[i-1]==M_WATER?i-1:mat[i+1]==M_WATER?i+1:mat[i-W]==M_WATER?i-W:i+W;
                mat[wn]=M_EMPTY; mat[i]=M_OBSID; heat[i]=210;
                spawn_part(i%W+0.5f, i/W-0.5f, (rndf()-0.5f)*10, -22-rndf()*24, 1.0f+rndf()*0.7f, 1);
                continue;
            }
            /* Lava stays molten a LONG time so it actually flows down the hill;
             * it only crusts once it settles. Cool slowly, and a little faster
             * where exposed to air. Conduction lets a connected body share heat
             * so the live flow keeps glowing while the still trail hardens. */
            int exposed = air(i-1)+air(i+1)+air(i-W)+air(i+W);
            int cool = (framestep&1) ? 1 : 0;                 /* ~0.5/step: stays molten seconds */
            if(exposed && (framestep&3)==0) cool++;            /* exposed skin cools a touch faster */
            int nh=0;                       /* hottest lava neighbour */
            if(mat[i-1]==M_LAVA&&heat[i-1]>nh)nh=heat[i-1];
            if(mat[i+1]==M_LAVA&&heat[i+1]>nh)nh=heat[i+1];
            if(mat[i-W]==M_LAVA&&heat[i-W]>nh)nh=heat[i-W];
            if(mat[i+W]==M_LAVA&&heat[i+W]>nh)nh=heat[i+W];
            int nv = heat[i]-cool;
            if(nh-14 > nv) nv = nh-14;      /* conduction from a hotter neighbour */
            if(nv <= LAVA_FREEZE){ mat[i]=M_OBSID; heat[i]=150; }  /* harden to rock */
            else heat[i]=(uint8_t)(nv>255?255:nv);
        } else if(m==M_OBSID && heat[i]>0){
            heat[i] -= (heat[i]>2?1:heat[i]);   /* residual glow fades slowly */
        }
    }

    /* surface embers off the hottest exposed lava */
    if((framestep&1)==0){
        for(int t=0;t<32;t++){
            int i=W+(int)(rnd()%(N-2*W));
            if(mat[i]==M_LAVA && heat[i]>190 && mat[i-W]==M_EMPTY){
                spawn_part(i%W+0.5f, i/W-0.5f, (rndf()-0.5f)*14, -22-rndf()*24, 0.5f+rndf()*0.7f, 0);
            }
        }
    }
}

/* ============================================================ lighting === */
static void build_light(void){
    memset(light,0,sizeof light);
    for(int y=1;y<H-1;y++){ int ly=y>>1;
        const uint8_t*mr=&mat[y*W]; const uint8_t*hr=&heat[y*W];
        for(int x=1;x<W-1;x++){ uint8_t m=mr[x]; int add=0;
            if(m==M_LAVA) add = 30 + (hr[x]>>1);
            else if(m==M_OBSID && hr[x]>40) add=(hr[x]-40)>>2;
            if(add){ uint16_t*L=&light[ly*LW+(x>>1)]; int v=*L+add; *L=(uint16_t)(v>4000?4000:v); }
        }
    }
    { /* headlamp */
        int lx=((int)(mx+MW_*0.5f+facing*4))>>1, ly=((int)(my+2))>>1;
        for(int dy=-3;dy<=3;dy++)for(int dx=-3;dx<=3;dx++){ int gx=lx+dx,gy=ly+dy;
            if(gx<0||gx>=LW||gy<0||gy>=LH)continue; int d2=dx*dx+dy*dy; if(d2>12)continue;
            uint16_t*L=&light[gy*LW+gx]; int v=*L+(1400-d2*100); *L=(uint16_t)(v>4000?4000:v); }
    }
    for(int i=0;i<np;i++){ if(parts[i].kind!=0)continue;
        int gx=(int)parts[i].x>>1,gy=(int)parts[i].y>>1;
        if(gx<1||gx>=LW-1||gy<1||gy>=LH-1)continue;
        uint16_t*L=&light[gy*LW+gx]; int v=*L+260; *L=(uint16_t)(v>4000?4000:v); }
    for(int pass=0;pass<2;pass++){ uint16_t*s=pass?light2:light,*d=pass?light:light2;
        for(int y=0;y<LH;y++){ int y0=y>0?y-1:0,y1=y<LH-1?y+1:LH-1;
            for(int x=0;x<LW;x++){ int x0=x>0?x-1:0,x1=x<LW-1?x+1:LW-1;
                int t=s[y0*LW+x0]+s[y0*LW+x]+s[y0*LW+x1]+s[y*LW+x0]+s[y*LW+x]+s[y*LW+x1]
                     +s[y1*LW+x0]+s[y1*LW+x]+s[y1*LW+x1];
                d[y*LW+x]=(uint16_t)(t/9); } } }
}

/* ========================================================= landscape === */
static void fill_column_solid(int x, int top){
    for(int y=top;y<H;y++){
        if(x<1||x>=W-1||y>=H-1){ if(y>=H-1) mat[y*W+x]=M_ROCK; continue; }
        /* deep = bedrock, shallow = diggable earth, with rocky veins */
        int depth=y-top;
        uint8_t m = (depth>26) ? M_ROCK : M_DIRT;
        if(depth>6 && vnoise(x*0.18f, y*0.18f, 99)>0.74f) m=M_ROCK;
        mat[y*W+x]=m;
    }
}

static void gen_level(void){
    for(int i=0;i<N;i++){ mat[i]=M_EMPTY; heat[i]=0; }

    /* crack veins in basalt (static, organic iso-lines of value noise) */
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        float n=vnoise(x*0.11f+3, y*0.11f+7, 51);
        float f=n*5.0f; f-=floorf(f);
        crackmap[y*W+x] = (fabsf(f-0.5f)<0.11f) ? 255 : 0;
    }

    /* --- heightfield: the vent sits on the PEAK at the left; the hillside then
     * descends steadily to the right (toward the ravine and the town) with only
     * gentle rolling bumps, so the lava reliably oozes downhill across the whole
     * landscape. (height = surface row; larger y = lower elevation.) --- */
    int height[W];
    int ventx = 12 + (int)(rndf()*6);
    int peak  = 28;
    for(int x=0;x<W;x++){
        float base;
        if(x <= ventx) base = peak - (ventx - x)*1.2f;          /* a CLIFF rising to the left */
        else           base = peak + (x - ventx)*0.52f;         /* long downhill to the right */
        float bumps = (vnoise(x*0.07f, 3, 11)-0.5f)*3.0f;       /* subtle rolling texture */
        int h=(int)(base+bumps); height[x]=mote_clampi(h,14,H-14);
    }
    /* town terrace: flatten a stretch on the lower-right */
    int townx = 84 + (int)(rndf()*8), townw = 34;
    int towny = height[townx];
    for(int x=townx-2;x<townx+townw+2 && x<W-1;x++) if(x>0) height[x]=towny;
    /* spillway: guarantee the columns just right of the vent never rise, so the
     * flow always starts downhill (no basin at the source). */
    for(int x=ventx+1;x<ventx+16 && x<W-1;x++)
        if(height[x] < height[x-1]) height[x] = height[x-1];

    for(int x=0;x<W;x++) fill_column_solid(x, height[x]);
    /* the left cliff backing the vent is solid bedrock (no leftward escape) */
    for(int x=1;x<ventx;x++) for(int y=height[x];y<height[x]+10 && y<H-1;y++) mat[y*W+x]=M_ROCK;
    for(int x=0;x<W;x++){ mat[(H-1)*W+x]=M_ROCK; }
    for(int y=0;y<H;y++){ mat[y*W]=M_ROCK; mat[y*W+W-1]=M_ROCK; }

    /* --- lava fissures on the ridge --- */
    nsrc = 1 + (level>2?1:0);
    if(nsrc>MAXSRC) nsrc=MAXSRC;
    for(int s=0;s<nsrc;s++){
        int sx = ventx + s*6;
        sx=mote_clampi(sx,4,W-5);
        src_x[s]=sx; src_y[s]=height[sx]-1;
        /* a small chimney ABOVE the surface only — never a pit that traps lava */
        for(int y=height[sx]-3;y<height[sx];y++)for(int dx=-1;dx<=1;dx++){
            int xx=sx+dx; if(xx>0&&xx<W-1&&y>0&&y<H-1) mat[y*W+xx]=M_EMPTY; }
    }

    /* --- a ravine (drain) between the vent and the town, capped by a thin
     * diggable DIRT LID so the surface stays continuous: by default the lava
     * flows straight OVER it toward the town, and the player must dig the lid to
     * tip the flow down into the ravine (where it drains away safely). --- */
    int ravx = 50 + (int)(rndf()*12);
    for(int x=ravx-3;x<=ravx+3;x++){ if(x<1||x>=W-1)continue;
        for(int y=height[x]+3;y<H-1;y++) mat[y*W+x]=M_EMPTY;   /* open shaft below the lid */
        mat[(H-2)*W+x]=M_DRAIN; }
    for(int x=ravx-4;x<=ravx+4;x++) if(x>0&&x<W-1) mat[(H-2)*W+x]=M_DRAIN;

    /* --- the town: buildings on the terrace = the safe zone --- */
    nbld=0; town_int=100;
    int bx=townx, bw=townw;
    int nb = 3 + (level>1?1:0) + (level>3?1:0);
    int step=bw/nb;
    for(int b=0;b<nb && nbld<MAXBLD;b++){
        int w = 5 + (int)(rndf()*4);
        int hh = 8 + (int)(rndf()*8);
        int px = bx + b*step + 1;
        int py = towny - hh;
        if(px+w>=W-1) continue;
        for(int y=py;y<towny;y++)for(int x=px;x<px+w;x++)
            if(x>0&&x<W-1&&y>0&&y<H-1) mat[y*W+x]=M_BUILD;
        bld_x[nbld]=px; bld_y[nbld]=py; bld_w[nbld]=w; bld_h[nbld]=hh; nbld++;
    }

    /* --- a reservoir the player can tap: a small water pocket uphill --- */
    for(int k=0;k<1+level/3;k++){
        int cx=30+(int)(rndf()*40), cy=height[mote_clampi(cx,1,W-2)]+8+(int)(rndf()*20);
        for(int y=cy-2;y<=cy+2;y++)for(int x=cx-3;x<=cx+3;x++)
            if(x>0&&x<W-1&&y>0&&y<H-1 && (x-cx)*(x-cx)/2+(y-cy)*(y-cy)<=6){
                if(mat[y*W+x]!=M_ROCK) mat[y*W+x]=M_WATER; }
    }

    /* engineer spawns a little clear of the vent's downhill path */
    mx = ventx+12; my = height[mote_clampi(ventx+12,1,W-2)]-MH_-1; mvx=mvy=0;
    if(test_mode){ mx = 2; my = H-MH_-2; }   /* park in the corner for flow capture */

    surge_max = surge_left = 2400 + level*400;
    coolant     = 300 + level*20;
    dam_charges = 160 + level*10;
    drained=0; src_acc=0; np=0;
    tool=T_DIG; tool_cd=0; facing=1; aim=0;
}

/* ============================================================== tools === */
static void apply_brush(int dig,int dam,int cool){
    float cx=mx+MW_*0.5f, cy=my+MH_*0.5f; int bx,by;
    if(aim>0){ bx=(int)cx; by=(int)my+MH_+4; } else { bx=(int)cx+facing*5; by=(int)cy; }
    int r=3;
    for(int y=by-r;y<=by+r;y++)for(int x=bx-r;x<=bx+r;x++){
        if(x<1||x>=W-1||y<1||y>=H-1)continue;
        int dx=x-bx,dy=y-by; if(dx*dx+dy*dy>r*r)continue;
        int i=y*W+x; uint8_t m=mat[i];
        if(dig && IS_DIGGABLE(m)) mat[i]=M_EMPTY;
        else if(dam && m==M_EMPTY && dam_charges>0){ mat[i]=M_WALL; dam_charges-=1; }
        else if(cool && m==M_EMPTY && coolant>0){ mat[i]=M_WATER; coolant-=1; }
    }
}
static int pred_solid(int m){ return IS_SOLID(m); }
static int pred_lava(int m){ return m==M_LAVA; }
static int aabb_hits(float px,float py,int(*pred)(int)){
    int x0=(int)px,x1=(int)(px+MW_-1),y0=(int)py,y1=(int)(py+MH_-1);
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
        if(x<0||x>=W||y<0||y>=H)return 1; if(pred(mat[y*W+x]))return 1; }
    return 0;
}
static void move_miner(float dt){
    const MoteInput*in=mote->input();
    float ax=0;
    if(mote_pressed(in,MOTE_BTN_LEFT)){ax-=1;facing=-1;}
    if(mote_pressed(in,MOTE_BTN_RIGHT)){ax+=1;facing=1;}
    mvx+=ax*520*dt; mvx*=0.80f; if(mvx>62)mvx=62; if(mvx<-62)mvx=-62;
    aim = mote_pressed(in,MOTE_BTN_DOWN)?1:0;
    if(grounded && mote_just_pressed(in,MOTE_BTN_UP)){ mvy=-155; grounded=0; }
    else if(mote_pressed(in,MOTE_BTN_UP)&&!grounded&&mvy<0) mvy-=120*dt;
    mvy+=470*dt; if(mvy>210)mvy=210;
    float nx=mx+mvx*dt; if(!aabb_hits(nx,my,pred_solid))mx=nx; else mvx=0;
    float ny=my+mvy*dt; if(!aabb_hits(mx,ny,pred_solid))my=ny; else mvy=0;
    grounded = aabb_hits(mx,my+1.0f,pred_solid)&&!aabb_hits(mx,my,pred_solid);
    if(mx<1){mx=1;mvx=0;} if(mx>W-1-MW_){mx=W-1-MW_;mvx=0;} if(my<1){my=1;mvy=0;}
    if(!test_mode && aabb_hits(mx,my,pred_lava)){ state=ST_LOSE; lose_cooked=1; state_t=0; mote->rumble(0.8f,300); }
}

/* ============================================================== flow === */
static void emit_sources(float dt){
    if(surge_left<=0) return;
    src_acc+=dt; const float rate=0.03f;
    while(src_acc>=rate && surge_left>0){ src_acc-=rate;
        for(int s=0;s<nsrc&&surge_left>0;s++){
            /* fatten the stream: a couple of cells across the mouth */
            for(int k=0;k<2 && surge_left>0;k++){
                int sx=src_x[s]+(int)((rndf()-0.5f)*4);
                int i=(src_y[s])*W+mote_clampi(sx,1,W-2);
                if(mat[i]==M_EMPTY){ mat[i]=M_LAVA; heat[i]=LAVA_HOT; surge_left--; }
            }
        }
    }
}
static void check_town(float dt){
    int hurt=0;
    for(int b=0;b<nbld;b++){
        /* lava touching the footprint of a building */
        for(int y=bld_y[b]-1;y<bld_y[b]+bld_h[b]+1;y++)
            for(int x=bld_x[b]-1;x<bld_x[b]+bld_w[b]+1;x++){
                if(x<0||x>=W||y<0||y>=H)continue;
                if(mat[y*W+x]==M_LAVA) hurt++;
            }
    }
    if(hurt>0){ town_int -= hurt*dt*3.0f; if(town_int<0)town_int=0; }
    if(town_int<=0){ state=ST_LOSE; lose_cooked=0; state_t=0; }
}
static int lava_alive(void){ for(int i=0;i<N;i++) if(mat[i]==M_LAVA) return 1; return 0; }

/* =============================================================== init === */
#include <stdlib.h>
static void g_init(void){
    rng=(uint32_t)mote->micros()|1u;
    test_mode = getenv("SLUICE_TEST") != 0;
    build_luts(); mote->set_fps_limit(60);
    state=ST_TITLE; state_t=0; gen_level(); build_light();
}

static void step_sim(float dt){
    emit_sources(dt);
    ca_acc+=dt; int st=0;
    while(ca_acc>=(1.0f/75.0f) && st<4){ ca_step(); ca_acc-=1.0f/75.0f; st++; }
    tick_particles(dt);
    /* occasional ash from the vent */
    if((framestep&7)==0 && nsrc>0){
        int s=rnd()%nsrc;
        spawn_part(src_x[s]+(rndf()-0.5f)*4, src_y[s]-2, (rndf()-0.5f)*8, -14-rndf()*10, 2.0f+rndf()*2, 2);
    }
}

static void g_update(float dt){
    if(dt>0.05f)dt=0.05f;
    const MoteInput*in=mote->input(); state_t+=dt;

    if(state==ST_TITLE){ if(mote_just_pressed(in,MOTE_BTN_A)){state=ST_PLAY;state_t=0;} build_light(); return; }
    if(state==ST_WIN||state==ST_LOSE){
        step_sim(dt); build_light();
        if(mote_just_pressed(in,MOTE_BTN_A)){ if(state==ST_WIN)level++; gen_level(); state=ST_PLAY; state_t=0; }
        return;
    }
    /* ST_PLAY */
    if(mote_just_pressed(in,MOTE_BTN_LB)) tool=(tool+2)%3;
    if(mote_just_pressed(in,MOTE_BTN_RB)) tool=(tool+1)%3;
    move_miner(dt);
    tool_cd-=dt;
    if(mote_pressed(in,MOTE_BTN_A)){
        if(tool==T_DIG) apply_brush(1,0,0);
        else if(tool_cd<=0){ if(tool==T_DAM){apply_brush(0,1,0);tool_cd=0.03f;} else {apply_brush(0,0,1);tool_cd=0.02f;} }
    }
    if(mote_pressed(in,MOTE_BTN_B)) apply_brush(1,0,0);
    step_sim(dt); check_town(dt); build_light();
    if(state==ST_PLAY && surge_left<=0 && !lava_alive()){ state=ST_WIN; state_t=0; }
}

/* ============================================================= render === */
static inline void px_add(uint16_t*fb,int x,int y,int y0,int y1,int ar,int ag,int ab){
    if(x<0||x>=W||y<y0||y>=y1)return; fb[y*W+x]=add565(fb[y*W+x],ar,ag,ab);
}

static void render_band(uint16_t*fb,int y0,int y1){
    for(int y=y0;y<y1;y++){
        int ly=y>>1;
        const uint8_t*mr=&mat[y*W]; const uint8_t*hr=&heat[y*W]; const uint8_t*ck=&crackmap[y*W];
        uint16_t*fr=&fb[y*W]; uint16_t sky=sky_lut[y];
        for(int x=0;x<W;x++){
            uint8_t m=mr[x]; uint16_t base; int emissive=0;

            if(m==M_LAVA){
                int h=hr[x];
                if(h>150){ base=lava_lut[h]; }                    /* bright molten */
                else {                                            /* basalt crust w/ glowing cracks */
                    uint16_t crust=obs_lut[h];
                    if(ck[x]){
                        uint16_t glow=lava_lut[mote_clampi(h+90,0,255)];
                        crust=lerp565(crust,glow, 0.7f*(h/150.0f));
                    }
                    base=crust;
                }
                fr[x]=base; continue;
            }

            switch(m){
                case M_EMPTY: base=sky; break;
                case M_ROCK:  base=((x^y)&3)?MOTE_RGB565(30,28,40):MOTE_RGB565(38,34,48); break;
                case M_DIRT:  base=((x+y)&3)?MOTE_RGB565(52,40,30):MOTE_RGB565(44,32,24); break;
                case M_WALL:  base=((x^y)&1)?MOTE_RGB565(96,84,66):MOTE_RGB565(78,68,54); break;  /* earth berm */
                case M_OBSID: base=obs_lut[hr[x]]; if(ck[x]&&hr[x]>20) base=lerp565(base,lava_lut[mote_clampi(hr[x]+60,0,255)],0.4f*(hr[x]/150.0f)); break;
                case M_DRAIN: base=(x&1)?MOTE_RGB565(6,5,10):MOTE_RGB565(18,14,22); break;
                case M_BUILD: base=MOTE_RGB565(60,58,74); break;   /* recoloured below */
                case M_WATER: base=((x+y+(int)state_t)&3)?MOTE_RGB565(24,72,120):MOTE_RGB565(34,100,150); break;
                default:      base=sky; break;
            }

            /* terrain surface highlight: a lit rim where solid meets sky above */
            if((m==M_DIRT||m==M_ROCK) && y>0 && mr[x-0] && mat[(y-1)*W+x]==M_EMPTY)
                base=add565(base,4,3,2);

            /* sky stars up high */
            if(m==M_EMPTY && y<46){
                uint32_t hsh=(x*73856093u)^(y*19349663u); if((hsh&1023)<3) base=add565(base,8,8,10);
            }

            /* warm flowing light */
            int L=light[ly*LW+(x>>1)];
            if(L>0) base=add565(base, L>>5, L>>6, L>>8);
            fr[x]=base;
        }
    }

    /* --- buildings: roof + lit windows drawn over the M_BUILD blocks --- */
    for(int b=0;b<nbld;b++){
        int px=bld_x[b],py=bld_y[b],w=bld_w[b],hh=bld_h[b];
        uint16_t wall = town_int>30?MOTE_RGB565(58,54,72):MOTE_RGB565(70,44,44);
        uint16_t roof = town_int>30?MOTE_RGB565(120,60,54):MOTE_RGB565(90,40,38);
        uint16_t win  = town_int>30?MOTE_RGB565(255,210,110):MOTE_RGB565(120,60,30);
        for(int y=py;y<py+hh;y++){ if(y<y0||y>=y1)continue;
            for(int x=px;x<px+w;x++){ if(x<0||x>=W)continue; if(mat[y*W+x]!=M_BUILD)continue;
                uint16_t c = (y<py+2)?roof:wall;
                /* windows */
                if(y>=py+3 && ((x-px)&1)==0 && ((y-py)&1)==0 && x>px && x<px+w-1) c=win;
                fb[y*W+x]=c;
            }
        }
    }

    /* --- particles --- */
    for(int i=0;i<np;i++){ Part*p=&parts[i]; int x=(int)p->x,y=(int)p->y;
        if(y<y0||y>=y1)continue; float f=p->life/p->max;
        if(p->kind==0){ int e=(int)(f*28); px_add(fb,x,y,y0,y1,e,e*2/3,e/6); }
        else if(p->kind==1){ int e=(int)(f*12); px_add(fb,x,y,y0,y1,e,e,e+2); }
        else { int e=(int)(f*5); px_add(fb,x,y,y0,y1,e,e-1,e); }   /* ash: dim grey */
    }

    /* --- engineer --- */
    {
        int px=(int)mx,py=(int)my;
        uint16_t body=MOTE_RGB565(70,130,180), head=MOTE_RGB565(210,180,150), helm=MOTE_RGB565(240,210,80);
        for(int yy=0;yy<MH_;yy++){ int y=py+yy; if(y<y0||y>=y1)continue;
            for(int xx=0;xx<MW_;xx++){ int x=px+xx; if(x<0||x>=W)continue;
                uint16_t c = (yy<2)?helm : (yy<4)?head : body; fb[y*W+x]=c; } }
        int hx=px+(facing>0?MW_-1:0), hy=py+1;
        if(hy>=y0&&hy<y1&&hx>=0&&hx<W) fb[hy*W+hx]=MOTE_RGB565(255,245,180);
    }
}

/* ============================================================ overlay === */
static const char*tool_name[3]={"DIG","BERM","COOL"};
static void g_overlay(uint16_t*fb){
    const MoteFont*fmed=(mote->abi_version>=47)?mote->ui_font(MOTE_FONT_MED):0;
    if(state==ST_TITLE){
        mote_dim_box(fb,0,0,128,128,0);
        if(fmed){
            mote->text_font(fb,mote->ui_font(MOTE_FONT_LARGE),"SLUICE",34,26,MOTE_RGB565(255,150,40));
            mote->text_font(fb,fmed,"The lava is coming.",14,54,MOTE_RGB565(230,220,225));
            mote->text_font(fb,fmed,"Divert it. Save",22,70,MOTE_RGB565(150,200,210));
            mote->text_font(fb,fmed,"the town.",40,82,MOTE_RGB565(150,200,210));
            mote->text_font(fb,fmed,"A: start",42,104,MOTE_RGB565(255,220,120));
        } else { mote->text_2x(fb,"SLUICE",34,40,MOTE_RGB565(255,150,40)); mote->text(fb,"A:START",44,80,MOTE_RGB565(255,220,120)); }
        return;
    }
    char buf[32];
    float sf=surge_max?(float)surge_left/surge_max:0;
    mote_ui_bar(fb,2,2,60,4,sf,MOTE_RGB565(255,120,20),MOTE_RGB565(40,20,10));
    mote_ui_bar(fb,66,2,60,4,town_int/100.0f, town_int>40?MOTE_RGB565(120,220,140):MOTE_RGB565(230,60,40),MOTE_RGB565(20,30,24));
    if(fmed){
        mote->text_font(fb,fmed,tool_name[tool],3,115,MOTE_RGB565(255,230,140));
        snprintf(buf,sizeof buf,"C%d B%d",(int)coolant,(int)dam_charges);
        mote->text_font(fb,fmed,buf,72,115,MOTE_RGB565(150,200,210));
    } else mote->text(fb,tool_name[tool],3,118,MOTE_RGB565(255,230,140));

    if(state==ST_WIN||state==ST_LOSE){
        mote_dim_box(fb,0,44,128,40,0);
        const char*t=state==ST_WIN?"TOWN SAVED":(lose_cooked?"MELTED":"BURNED");
        uint16_t c=state==ST_WIN?MOTE_RGB565(80,240,160):MOTE_RGB565(255,80,50);
        if(fmed){ mote->text_font(fb,mote->ui_font(MOTE_FONT_LARGE),t,26,52,c);
                  mote->text_font(fb,fmed,"A: continue",34,72,MOTE_RGB565(230,230,240)); }
        else mote->text_2x(fb,t,26,54,c);
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=render_band, .overlay=g_overlay,
    .config={ .max_points=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
