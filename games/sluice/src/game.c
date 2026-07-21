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
#include <stdlib.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "blonde_man.h"       /* player sprite sheet (32x32 cells, baked) */

MOTE_GAME_META("Sluice", "austinio7116");
MOTE_GAME_VERSION("0.3.0");

/* ============================================================ constants === */
#define W  128
#define H  128
#define N  (W * H)
#define LW 64
#define LH 64
#define LN (LW * LH)

enum {
    M_EMPTY = 0,   /* cave air */
    M_ROCK,        /* bedrock */
    M_DIRT,        /* earth */
    M_WALL,        /* (unused reserve) */
    M_OBSID,       /* hardened lava — solid rock */
    M_DRAIN,       /* pit floor — swallows lava */
    M_BUILD,       /* town building — protect it */
    M_LOG,         /* player-placed wooden deflector plank */
    M_SAND,        /* player-placed sandbag pile — granular, bakes to rock */
    M_LAVA,        /* thick molten/crusting lava */
    M_WATER        /* water */
};
#define IS_SOLID(m)    ((m)==M_ROCK||(m)==M_DIRT||(m)==M_WALL||(m)==M_OBSID||(m)==M_BUILD||(m)==M_LOG||(m)==M_SAND)

/* lava heat thresholds */
#define LAVA_HOT   255
#define LAVA_MOVE  40      /* below this the crust is too stiff to creep */
#define LAVA_FREEZE 16     /* below this it hardens to obsidian */

/* timed round */
#define READY_TIME 20.0f   /* planning countdown before the lava starts */
#define FLOW_TIME  16.0f   /* how long the lava pours once it starts */

enum { T_WATER = 0, T_LOG, T_DIG, T_SAND, T_BLAST, T_NTOOLS };
enum { ST_TITLE = 0, ST_READY, ST_PLAY, ST_WIN, ST_LOSE };

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

/* player (the little man) */
static float mx, my, mvx, mvy;
#define MW_ 8
#define MH_ 13
static int   grounded = 0, facing = 1, tool = T_WATER, anim_frame = 0;
static float anim_t = 0;
static float coyote_t = 0, jbuf_t = 0;   /* jump feel: coyote time + input buffer */
static float place_cd = 0;
static float log_angle = 0.35f;       /* radians — log placement angle */
static float aimx = 1, aimy = 0;      /* aim unit vector (grapple + log) */

/* grappling hook (Liero/Noita rope) */
static int   grap_state = 0;          /* 0 idle, 1 flying, 2 anchored */
static float grap_x, grap_y, grap_vx, grap_vy, rope_len;
#define GRAP_SPEED 300.0f
#define GRAP_MAX   96.0f
#define GRAP_REEL  40.0f

/* tool budgets + placed water sources */
static int   water_left, log_left, sand_left, blast_left;
#define MAXWSRC 8
static int   wsrc_x[MAXWSRC], wsrc_y[MAXWSRC], nwsrc = 0;

/* timed round */
static float phase_t = 0;             /* READY: counts down to 0 */
static float flow_t  = 0;             /* PLAY: lava pours while > 0 */
static int   drained;
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
            if(m==M_SAND){                          /* granular: falls + piles, no spread */
                int below=i+W;
                if(air(below)){ mat[below]=M_SAND; mat[i]=M_EMPTY; moved[below]=1; continue; }
                int dl=below-1,dr=below+1,d1=dir?dr:dl,d2=dir?dl:dr;
                if(air(d1)){mat[d1]=M_SAND;mat[i]=M_EMPTY;moved[d1]=1;continue;}
                if(air(d2)){mat[d2]=M_SAND;mat[i]=M_EMPTY;moved[d2]=1;continue;}
                continue;
            }
            if(m!=M_LAVA) continue;

            int below=i+W;
            if(mat[below]==M_DRAIN){ drained++; mat[i]=M_EMPTY; heat[i]=0; continue; }

            /* only a frozen crust stops moving */
            if(heat[i] < LAVA_MOVE) continue;

            /* fall RELIABLY — this cohesion is what reads as a liquid, not a spray */
            if(air(below)){ mat[below]=M_LAVA; heat[below]=heat[i]; mat[i]=M_EMPTY; heat[i]=0; moved[below]=1; continue; }
            int d1=dir?below+1:below-1, d2=dir?below-1:below+1;
            if(air(d1)){ mat[d1]=M_LAVA; heat[d1]=heat[i]; mat[i]=M_EMPTY; heat[i]=0; moved[d1]=1; continue; }
            if(air(d2)){ mat[d2]=M_LAVA; heat[d2]=heat[i]; mat[i]=M_EMPTY; heat[i]=0; moved[d2]=1; continue; }
            /* viscous lateral spread: fills pools + overflows, every other step */
            if((framestep&1)==0){
                int s1=dir?i+1:i-1, s2=dir?i-1:i+1;
                if(air(s1)){ mat[s1]=M_LAVA; heat[s1]=heat[i]; mat[i]=M_EMPTY; heat[i]=0; moved[s1]=1; continue; }
                if(air(s2)){ mat[s2]=M_LAVA; heat[s2]=heat[i]; mat[i]=M_EMPTY; heat[i]=0; moved[s2]=1; continue; }
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
            int cool = (framestep%3==0) ? 1 : 0;              /* stays molten far longer */
            if(exposed>=3 && (framestep&7)==0) cool++;         /* only very exposed skin cools faster */
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
        } else if(m==M_SAND){                   /* sand touching lava bakes to rock */
            if((mat[i-1]==M_LAVA||mat[i+1]==M_LAVA||mat[i-W]==M_LAVA||mat[i+W]==M_LAVA) && (rnd()&3)==0){
                mat[i]=M_OBSID; heat[i]=120;
            }
        }
    }

    /* a FEW gentle embers off the very hottest exposed lava (no spray) */
    if((framestep&3)==0){
        for(int t=0;t<6;t++){
            int i=W+(int)(rnd()%(N-2*W));
            if(mat[i]==M_LAVA && heat[i]>215 && mat[i-W]==M_EMPTY){
                spawn_part(i%W+0.5f, i/W-0.5f, (rndf()-0.5f)*8, -12-rndf()*12, 0.4f+rndf()*0.4f, 0);
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
static void carve_ellipse(int cx,int cy,int rx,int ry){
    if(rx<1)rx=1; if(ry<1)ry=1;
    for(int y=cy-ry;y<=cy+ry;y++)for(int x=cx-rx;x<=cx+rx;x++){
        if(x<2||x>=W-2||y<2||y>=H-2)continue;
        float dx=(float)(x-cx)/rx, dy=(float)(y-cy)/ry;
        if(dx*dx+dy*dy<=1.0f) mat[y*W+x]=M_EMPTY;
    }
}
static void carve_line(int x0,int y0,int x1,int y1,int r){
    int adx=x1-x0; if(adx<0)adx=-adx; int ady=y1-y0; if(ady<0)ady=-ady;
    int steps=(adx>ady?adx:ady)+1;
    for(int s=0;s<=steps;s++){ float t=(float)s/steps;
        carve_ellipse((int)(x0+(x1-x0)*t),(int)(y0+(y1-y0)*t),r,r); }
}

static void gen_level(void){
    memset(mat,M_EMPTY,N); memset(heat,0,N);
    uint32_t seed = (uint32_t)(level*2654435761u) ^ (rng|1u);

    /* crack veins in basalt (static, organic iso-lines of value noise) */
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        float n=vnoise(x*0.11f+3, y*0.11f+7, 51);
        float f=n*5.0f; f-=floorf(f);
        crackmap[y*W+x] = (fabsf(f-0.5f)<0.11f) ? 255 : 0;
    }

    /* --- Noita-style ENCLOSED cave: fill the world SOLID, then carve open
     * chambers. Lava pours in at the top and cascades down through the cavern. */
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        uint8_t m = M_DIRT;
        if(vnoise(x*0.16f, y*0.16f, seed^3) > 0.72f) m = M_ROCK;     /* rock veins */
        if(x<2||x>=W-2||y<2||y>=H-2) m = M_ROCK;                     /* enclosing shell */
        mat[y*W+x]=m;
    }

    /* --- NATURAL organic cave: carve open space from a biased fractal-noise
     * field, then relax it with cellular-automata smoothing passes so the walls
     * read as natural rock (no circles or lines). A wandering vertical "spine"
     * bias keeps a connected descending path for the lava. --- */
    int floory=H-15;
    float spine[H];
    for(int y=0;y<H;y++) spine[y]=W*0.5f + (vnoise(y*0.055f,0.0f,seed^11)-0.5f)*52.0f;
    for(int y=4;y<H-4;y++)for(int x=3;x<W-3;x++){
        float n = vnoise(x*0.075f,y*0.075f,seed)*0.55f
                + vnoise(x*0.155f,y*0.155f,seed^5)*0.30f
                + vnoise(x*0.31f, y*0.31f, seed^9)*0.15f;      /* fbm */
        float dxs=x-spine[y]; if(dxs<0)dxs=-dxs;
        float bias=1.0f-dxs/23.0f; if(bias<0)bias=0;           /* open near the spine */
        if(n + bias*0.42f > 0.55f) mat[y*W+x]=M_EMPTY;
    }
    for(int pass=0;pass<4;pass++){                             /* CA smoothing */
        for(int y=2;y<H-2;y++)for(int x=2;x<W-2;x++){
            int solid=0; for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++) if(mat[(y+dy)*W+(x+dx)]!=M_EMPTY) solid++;
            moved[y*W+x]=(solid>=5)?1:0;
        }
        for(int y=2;y<H-2;y++)for(int x=2;x<W-2;x++){
            if(moved[y*W+x]){ if(mat[y*W+x]==M_EMPTY) mat[y*W+x]=(vnoise(x*0.16f,y*0.16f,seed^3)>0.72f)?M_ROCK:M_DIRT; }
            else mat[y*W+x]=M_EMPTY;
        }
    }
    for(int x=0;x<W;x++){ mat[x]=mat[W+x]=M_ROCK; mat[(H-1)*W+x]=mat[(H-2)*W+x]=M_ROCK; }
    for(int y=0;y<H;y++){ mat[y*W]=mat[y*W+1]=M_ROCK; mat[y*W+W-1]=mat[y*W+W-2]=M_ROCK; }

    /* --- wide, connected bottom cavern (holds the town + both drains) --- */
    carve_ellipse(W/2, floory+1, 56, 10);

    /* --- a single lava SOURCE at a point on the TOP edge --- */
    nsrc = 1;
    int sx = mote_clampi((int)spine[6] + (int)((rndf()-0.5f)*40), 18, W-18);
    src_x[0]=sx; src_y[0]=3;
    /* just open the top a little at the source so it pours in (the channel below
     * connects it into the cave) — no carved starting box */
    for(int y=2;y<7;y++)for(int dx=-1;dx<=1;dx++){ int x=sx+dx; if(x>1&&x<W-1) mat[y*W+x]=M_EMPTY; }

    /* --- a guaranteed descending channel from the source down to the bottom
     * cavern, curving toward centre, organic radius so it blends with the cave.
     * This ensures the lava can ALWAYS pass down the main channel. --- */
    for(int y=6;y<floory-6;y++){
        float t=(float)(y-6)/(floory-12);
        float cx = sx + (W*0.5f - sx)*t*t + (vnoise(y*0.14f,2,seed)-0.5f)*12;
        int r = 4 + (int)(vnoise(y*0.2f,4,seed)*3);
        for(int dx=-r;dx<=r;dx++){ int x=(int)cx+dx; if(x>2&&x<W-2) mat[y*W+x]=M_EMPTY; }
    }

    /* --- a big open ARENA in the middle where the player starts --- */
    int mcx=W/2, mcy=H/2;
    for(int y=mcy-18;y<=mcy+14;y++)for(int x=mcx-25;x<=mcx+25;x++){
        if(x<3||x>=W-3||y<3||y>=H-3)continue;
        float ex=(x-mcx)/25.0f, ey=(y-mcy)/17.0f;
        if(ex*ex+ey*ey<=1.0f && mat[y*W+x]!=M_BUILD) mat[y*W+x]=M_EMPTY;
    }

    /* --- town platform + buildings (safe zone) --- */
    int platx = W/2-16, platy = floory+2;
    for(int y=platy;y<platy+3;y++)for(int x=platx;x<platx+32;x++)
        if(x>2&&x<W-2&&y<H-2) mat[y*W+x]=M_ROCK;
    nbld=0; town_int=100;
    int nb = 3 + (level>1?1:0);
    for(int b=0;b<nb && nbld<MAXBLD;b++){
        int w=5+(int)(rndf()*3), hh=7+(int)(rndf()*5);
        int bx=platx+2+b*(30/nb), by=platy-hh;
        for(int y=by;y<platy;y++)for(int x=bx;x<bx+w;x++) if(x>2&&x<W-2&&y>2) mat[y*W+x]=M_BUILD;
        bld_x[nbld]=bx; bld_y[nbld]=by; bld_w[nbld]=w; bld_h[nbld]=hh; nbld++;
    }

    /* --- drains at the bottom corners (route the flow here to win) --- */
    for(int x=4;x<22;x++)     mat[(H-3)*W+x]=M_DRAIN;
    for(int x=W-22;x<W-4;x++) mat[(H-3)*W+x]=M_DRAIN;

    /* --- CONNECTIVITY: flood-fill the open space reachable from the source, then
     * solidify every pocket that isn't reachable — so all open holes connect. --- */
    memset(moved,0,N);
    moved[(src_y[0]+3)*W+sx]=1;
    for(int pass=0;pass<300;pass++){ int changed=0;
        for(int i=W;i<N-W;i++){ if(moved[i]||mat[i]!=M_EMPTY)continue;
            if(moved[i-1]||moved[i+1]||moved[i-W]||moved[i+W]){moved[i]=1;changed=1;} }
        for(int i=N-W-1;i>=W;i--){ if(moved[i]||mat[i]!=M_EMPTY)continue;
            if(moved[i-1]||moved[i+1]||moved[i-W]||moved[i+W]){moved[i]=1;changed=1;} }
        if(!changed) break;
    }
    for(int i=0;i<N;i++) if(mat[i]==M_EMPTY && !moved[i]) mat[i]=M_DIRT;

    /* drop the player in the middle of the arena */
    mx = mcx - MW_/2; my = mcy - 4;
    if(test_mode){ mx=6; my=floory-MH_; }
    mvx=mvy=0; grap_state=0;

    nwsrc=0; drained=0; np=0;
    water_left = 4 + level;               /* placeable water sources */
    log_left   = 8 + level;               /* placeable logs */
    sand_left  = 6 + level;               /* sandbag drops */
    blast_left = 2 + level/2;             /* blast charges */
    phase_t = READY_TIME; flow_t = FLOW_TIME;
    tool = T_WATER; place_cd = 0; facing = 1; log_angle = 0.35f;
}

/* ============================================================== tools === */
/* the placement reticle: a spot in front of the man, at foot height */
static void reticle(int *rx,int *ry){
    *rx = (int)(mx + MW_*0.5f) + facing*10;
    *ry = (int)(my + MH_) - 3;
}
/* never drop a solid cell on the player (would trap them) */
static int cell_in_player(int x,int y){
    return x>=(int)mx-1 && x<=(int)mx+MW_ && y>=(int)my-1 && y<=(int)my+MH_;
}
static void place_water(void){
    if(water_left<=0 || nwsrc>=MAXWSRC) return;
    int rx,ry; reticle(&rx,&ry);
    rx=mote_clampi(rx,2,W-3); ry=mote_clampi(ry,2,H-3);
    wsrc_x[nwsrc]=rx; wsrc_y[nwsrc]=ry; nwsrc++;
    water_left--;
}
/* stamp a thick angled plank of M_LOG centred on the reticle */
static void place_log(void){
    if(log_left<=0) return;
    int rx,ry; reticle(&rx,&ry);
    float ca=cosf(log_angle), sa=sinf(log_angle);
    int placed=0, HALF=10;
    for(int t=-HALF;t<=HALF;t++){
        int lx=(int)(rx+ca*t), ly=(int)(ry+sa*t);
        for(int th=-2;th<=2;th++){                 /* 5px thick, perpendicular */
            int px=(int)(lx - sa*th), py=(int)(ly + ca*th);
            if(px<2||px>=W-2||py<2||py>=H-2)continue;
            if(cell_in_player(px,py)) continue;
            uint8_t m=mat[py*W+px];
            if(m==M_EMPTY||m==M_WATER){ mat[py*W+px]=M_LOG; placed=1; }
        }
    }
    if(placed) log_left--;
}
/* pickaxe: carve diggable terrain (earth / hardened lava / logs / sand) at the
 * reticle — cut channels to steer the flow. Bedrock (M_ROCK) is too hard. */
static void dig_brush(void){
    int rx,ry; reticle(&rx,&ry); int r=3;
    for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
        int x=rx+dx, y=ry+dy; if(x<2||x>=W-2||y<2||y>=H-2)continue;
        if(dx*dx+dy*dy>r*r)continue; uint8_t m=mat[y*W+x];
        if(m==M_DIRT||m==M_OBSID||m==M_LOG||m==M_SAND) mat[y*W+x]=M_EMPTY;
    }
}
/* sandbags: drop a blob of granular sand — it falls and piles, dams the flow, and
 * bakes to solid rock where the lava touches it. */
static void place_sand(void){
    if(sand_left<=0) return;
    int rx,ry; reticle(&rx,&ry); int placed=0;
    for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
        int x=rx+dx, y=ry+dy; if(x<2||x>=W-2||y<2||y>=H-2)continue;
        if(dx*dx+dy*dy>5)continue;
        if(cell_in_player(x,y)) continue;
        if(mat[y*W+x]==M_EMPTY){ mat[y*W+x]=M_SAND; placed=1; }
    }
    if(placed) sand_left--;
}
/* blast charge: an explosion that craters even BEDROCK — open a new channel or
 * blow a pit for the lava to drain into. Spares the town buildings. */
static void place_blast(void){
    if(blast_left<=0) return;
    int rx,ry; reticle(&rx,&ry); int r=8;
    for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
        int x=rx+dx, y=ry+dy; if(x<2||x>=W-2||y<2||y>=H-2)continue;
        if(dx*dx+dy*dy>r*r)continue;
        if(mat[y*W+x]!=M_BUILD){ mat[y*W+x]=M_EMPTY; heat[y*W+x]=0; }
    }
    for(int k=0;k<26;k++){ float a=k/26.0f*6.2831853f, sp=20+rndf()*40;
        spawn_part(rx+0.5f, ry+0.5f, cosf(a)*sp, sinf(a)*sp-10, 0.4f+rndf()*0.5f, k&1); }
    blast_left--;
    mote->rumble(0.9f, 220);
}
static int pred_solid(int m){ return IS_SOLID(m); }
static int pred_lava(int m){ return m==M_LAVA; }
static int aabb_hits(float px,float py,int(*pred)(int)){
    int x0=(int)px,x1=(int)(px+MW_-1),y0=(int)py,y1=(int)(py+MH_-1);
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
        if(x<0||x>=W||y<0||y>=H)return 1; if(pred(mat[y*W+x]))return 1; }
    return 0;
}
static inline float pcx(void){ return mx+MW_*0.5f; }
static inline float pcy(void){ return my+MH_*0.5f; }

static void player_update(float dt){
    const MoteInput*in=mote->input();
    /* --- aim from the d-pad (drives grapple + log angle) --- */
    float dax=0,day=0;
    if(mote_pressed(in,MOTE_BTN_LEFT)) { dax-=1; facing=-1; }
    if(mote_pressed(in,MOTE_BTN_RIGHT)){ dax+=1; facing= 1; }
    if(tool==T_LOG){
        /* UP/DOWN simply dial the log's angle (LEFT/RIGHT still walk) */
        if(mote_pressed(in,MOTE_BTN_UP))   log_angle -= 2.2f*dt;
        if(mote_pressed(in,MOTE_BTN_DOWN)) log_angle += 2.2f*dt;
        aimx=facing; aimy=-0.9f;
    } else {
        if(mote_pressed(in,MOTE_BTN_UP))   day-=1;
        if(mote_pressed(in,MOTE_BTN_DOWN)) day+=1;
        if(dax||day){ aimx=dax; aimy=day; } else { aimx=facing; aimy=-0.9f; }
    }
    { float l=sqrtf(aimx*aimx+aimy*aimy); if(l>0.001f){ aimx/=l; aimy/=l; } }

    /* --- fast, snappy horizontal movement --- */
    if(dax!=0){ anim_t+=dt*9; anim_frame=((int)anim_t)&3; } else anim_frame=0;
    mvx += dax*1000*dt; mvx*=0.82f; if(mvx>98)mvx=98; if(mvx<-98)mvx=-98;

    /* --- jump: coyote time + input buffer + release-to-shorten = consistent feel --- */
    if(grounded) coyote_t=0.09f; else if(coyote_t>0) coyote_t-=dt;
    if(mote_just_pressed(in,MOTE_BTN_A)) jbuf_t=0.10f; else if(jbuf_t>0) jbuf_t-=dt;
    if(jbuf_t>0 && coyote_t>0){ mvy=-205; grounded=0; coyote_t=0; jbuf_t=0; }   /* always full jump */
    if(!mote_pressed(in,MOTE_BTN_A) && mvy<-80) mvy=-80;                        /* release early = short hop */
    mvy += 540*dt; if(mvy>250)mvy=250;

    /* --- grappling hook (RB) --- */
    if(mote_just_pressed(in,MOTE_BTN_RB) && grap_state==0){
        grap_x=pcx(); grap_y=pcy(); grap_vx=aimx*GRAP_SPEED; grap_vy=aimy*GRAP_SPEED; grap_state=1;
    }
    if(grap_state && !mote_pressed(in,MOTE_BTN_RB)) grap_state=0;   /* release lets go */
    if(grap_state==1){                                             /* hook in flight */
        for(int s=0;s<8 && grap_state==1;s++){
            grap_x+=grap_vx*dt/8; grap_y+=grap_vy*dt/8;
            int gx=(int)grap_x, gy=(int)grap_y;
            if(gx<1||gx>=W-1||gy<1||gy>=H-1){ grap_state=0; break; }
            if(IS_SOLID(mat[gy*W+gx])){ grap_state=2;
                rope_len=sqrtf((grap_x-pcx())*(grap_x-pcx())+(grap_y-pcy())*(grap_y-pcy())); break; }
            float ddx=grap_x-pcx(), ddy=grap_y-pcy();
            if(ddx*ddx+ddy*ddy > GRAP_MAX*GRAP_MAX){ grap_state=0; break; }
        }
    }

    /* --- integrate, PIXEL-STEPPED so the player slides flush to walls/ceilings
     * (a single whole-velocity AABB test would stop a frame-step short — the
     * "block-based" feel + clipped jumps). --- */
    { float d=mvx*dt, st=d<0?-1.0f:1.0f, r=d<0?-d:d;
      while(r>=1.0f){ if(aabb_hits(mx+st,my,pred_solid)){ mvx=0; break; } mx+=st; r-=1.0f; }
      if(r>0 && mvx!=0){ float f=st*r; if(!aabb_hits(mx+f,my,pred_solid)) mx+=f; else mvx=0; } }
    { float d=mvy*dt, st=d<0?-1.0f:1.0f, r=d<0?-d:d;
      while(r>=1.0f){ if(aabb_hits(mx,my+st,pred_solid)){ mvy=0; break; } my+=st; r-=1.0f; }
      if(r>0 && mvy!=0){ float f=st*r; if(!aabb_hits(mx,my+f,pred_solid)) my+=f; else mvy=0; } }

    /* --- rope constraint: pendulum swing + reel-in while held --- */
    if(grap_state==2){
        rope_len -= GRAP_REEL*dt; if(rope_len<7)rope_len=7;
        float dx=pcx()-grap_x, dy=pcy()-grap_y, d=sqrtf(dx*dx+dy*dy);
        if(d>rope_len && d>0.01f){
            float k=(d-rope_len)/d, nmx=mx-dx*k, nmy=my-dy*k;
            if(!aabb_hits(nmx,my,pred_solid)) mx=nmx;
            if(!aabb_hits(mx,nmy,pred_solid)) my=nmy;
            float ux=dx/d, uy=dy/d, vd=mvx*ux+mvy*uy; if(vd>0){ mvx-=ux*vd; mvy-=uy*vd; }
        }
    }

    grounded = aabb_hits(mx,my+1.0f,pred_solid)&&!aabb_hits(mx,my,pred_solid);
    if(mx<1){mx=1;mvx=0;} if(mx>W-1-MW_){mx=W-1-MW_;mvx=0;} if(my<1){my=1;mvy=0;}
    if(!test_mode && aabb_hits(mx,my,pred_lava)){ state=ST_LOSE; lose_cooked=1; state_t=0; mote->rumble(0.8f,300); grap_state=0; }
}

/* ============================================================== flow === */
/* Emit one row of lava at the single top source each sim step (continuous feed =
 * a solid stream, no stripes). Gated by the caller so it only runs while pouring. */
static void emit_lava(void){
    for(int dx=-4;dx<=4;dx++){                      /* 9-wide thick stream (as before); pours for FLOW_TIME */
        int i=src_y[0]*W+mote_clampi(src_x[0]+dx,1,W-2);
        if(mat[i]==M_EMPTY){ mat[i]=M_LAVA; heat[i]=LAVA_HOT; }
    }
}
/* each placed water source drips water every sim step */
static void emit_water(void){
    for(int s=0;s<nwsrc;s++){
        int i=wsrc_y[s]*W+wsrc_x[s];
        if(mat[i]==M_EMPTY) mat[i]=M_WATER;
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
    int pour = (state==ST_PLAY && flow_t>0);
    ca_acc+=dt; int st=0;
    while(ca_acc>=(1.0f/45.0f) && st<3){
        if(pour) emit_lava();
        if(state==ST_PLAY) emit_water();     /* water releases WITH the lava, not during planning */
        ca_step(); ca_acc-=1.0f/45.0f; st++;
    }
    tick_particles(dt);
    if(pour && (framestep&7)==0)
        spawn_part(src_x[0]+(rndf()-0.5f)*4, src_y[0]-1, (rndf()-0.5f)*8, -14-rndf()*10, 1.5f+rndf()*2, 2);
}

static void g_update(float dt){
    if(dt>0.05f)dt=0.05f;
    const MoteInput*in=mote->input(); state_t+=dt;
    if(state==ST_TITLE){ if(mote_just_pressed(in,MOTE_BTN_A)){ state=ST_READY; state_t=0; } build_light(); return; }
    if(state==ST_WIN||state==ST_LOSE){
        step_sim(dt); build_light();
        if(mote_just_pressed(in,MOTE_BTN_A)){ if(state==ST_WIN)level++; gen_level(); state=ST_READY; state_t=0; }
        return;
    }

    /* ---- ST_READY (planning countdown) or ST_PLAY (lava pouring) ---- */
    if(mote_just_pressed(in,MOTE_BTN_LB)) tool = (tool+1)%T_NTOOLS;  /* LB cycles: WATER>LOG>DIG */
    player_update(dt);
    place_cd-=dt;
    if(tool==T_DIG){ if(mote_pressed(in,MOTE_BTN_B)) dig_brush(); }        /* continuous */
    else if(tool==T_BLAST){ if(mote_just_pressed(in,MOTE_BTN_B)) place_blast(); }  /* one-shot */
    else if(mote_pressed(in,MOTE_BTN_B) && place_cd<=0){                   /* rate-limited place */
        if(tool==T_WATER) place_water();
        else if(tool==T_LOG) place_log();
        else place_sand();
        place_cd = 0.14f;
    }

    if(state==ST_READY){ phase_t-=dt; if(phase_t<=0){ state=ST_PLAY; state_t=0; } }
    else if(flow_t>0)  { flow_t-=dt; }

    step_sim(dt);
    if(state==ST_PLAY) check_town(dt);
    build_light();

    /* survived the whole pour with the town still standing -> win */
    if(state==ST_PLAY && flow_t<=0 && !lava_alive()){ state=ST_WIN; state_t=0; }
}

/* ============================================================= render === */
static inline void px_add(uint16_t*fb,int x,int y,int y0,int y1,int ar,int ag,int ab){
    if(x<0||x>=W||y<y0||y>=y1)return; fb[y*W+x]=add565(fb[y*W+x],ar,ag,ab);
}

static void render_band(uint16_t*fb,int y0,int y1){
    for(int y=y0;y<y1;y++){
        int ly=y>>1;
        const uint8_t*mr=&mat[y*W]; const uint8_t*hr=&heat[y*W]; const uint8_t*ck=&crackmap[y*W];
        uint16_t*fr=&fb[y*W];
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
                case M_EMPTY: base=MOTE_RGB565(7,6,11); break;   /* dark cave air */
                case M_ROCK:  base=((x^y)&3)?MOTE_RGB565(30,28,40):MOTE_RGB565(38,34,48); break;
                case M_DIRT:  base=((x+y)&3)?MOTE_RGB565(52,40,30):MOTE_RGB565(44,32,24); break;
                case M_WALL:  base=((x^y)&1)?MOTE_RGB565(96,84,66):MOTE_RGB565(78,68,54); break;  /* earth berm */
                case M_OBSID: base=obs_lut[hr[x]]; if(ck[x]&&hr[x]>20) base=lerp565(base,lava_lut[mote_clampi(hr[x]+60,0,255)],0.4f*(hr[x]/150.0f)); break;
                case M_DRAIN: base=(x&1)?MOTE_RGB565(6,5,10):MOTE_RGB565(18,14,22); break;
                case M_SAND:  base=((x*3+y*5)&3)==0?MOTE_RGB565(210,180,110):(((x+y)&1)?MOTE_RGB565(190,158,96):MOTE_RGB565(170,140,84)); break; /* sandbags */
                case M_LOG: {   /* wooden log: dark bark rim + grain interior */
                    int bark = (mat[y*W+x-1]!=M_LOG)||(mat[y*W+x+1]!=M_LOG)||(mat[(y-1)*W+x]!=M_LOG)||(mat[(y+1)*W+x]!=M_LOG);
                    base = bark ? MOTE_RGB565(66,40,20)
                                : (((x+y)&3)==0 ? MOTE_RGB565(150,104,56) : MOTE_RGB565(120,80,42));
                    break; }
                case M_BUILD: base=MOTE_RGB565(60,58,74); break;   /* recoloured below */
                case M_WATER: base=((x+y+(int)state_t)&3)?MOTE_RGB565(24,72,120):MOTE_RGB565(34,100,150); break;
                default:      base=MOTE_RGB565(7,6,11); break;
            }

            /* cave-wall rim: a faint lit edge where rock meets open air above */
            if((m==M_DIRT||m==M_ROCK) && y>0 && mat[(y-1)*W+x]==M_EMPTY)
                base=add565(base,4,3,2);

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

    /* the player sprite + reticle are drawn in overlay() (on top, unclipped) */
}

/* ============================================================ overlay === */
static const char*tool_name[T_NTOOLS]={"WATER","LOG","DIG"};

static void draw_player(uint16_t*fb){
    /* Crop to the CHARACTER inside the 32x32 cell (opaque x9..23, y13..31) and
     * scale it to the collision box so head=box top, feet=box bottom — the sprite
     * fits the player's pixels exactly (no dead space above the head). */
    float cx=mx+MW_*0.5f, cy=my+MH_*0.5f;
    mote->blit_ex(fb,&blonde_man_img, cx,cy, anim_frame*32+9,13, 14,18, 0.0f, (float)MH_/18.0f, MOTE_BLEND_NONE, 0,128);
}
static void draw_reticle(uint16_t*fb){
    int rx,ry; reticle(&rx,&ry);
    if(tool==T_WATER){
        uint16_t c=MOTE_RGB565(80,200,255);
        mote->draw_line(fb,rx-2,ry,rx+2,ry,c,0,128); mote->draw_line(fb,rx,ry-2,rx,ry+2,c,0,128);
    } else if(tool==T_LOG){
        float ca=cosf(log_angle), sa=sinf(log_angle); uint16_t c=MOTE_RGB565(220,180,120);
        mote->draw_line(fb,(int)(rx-ca*10),(int)(ry-sa*10),(int)(rx+ca*10),(int)(ry+sa*10),c,0,128);
    } else if(tool==T_DIG){
        mote->draw_circle(fb,rx,ry,3,MOTE_RGB565(200,200,210),0,0,128);
    } else if(tool==T_SAND){
        mote->draw_circle(fb,rx,ry,2,MOTE_RGB565(220,190,120),0,0,128);
    } else { /* BLAST radius */
        mote->draw_circle(fb,rx,ry,8,MOTE_RGB565(255,110,40),0,0,128);
    }
}
static void draw_rope(uint16_t*fb){
    if(!grap_state) return;
    int px=(int)(mx+MW_*0.5f), py=(int)(my+MH_*0.5f);
    mote->draw_line(fb,px,py,(int)grap_x,(int)grap_y,MOTE_RGB565(170,150,110),0,128);
    uint16_t c = grap_state==2?MOTE_RGB565(230,230,240):MOTE_RGB565(200,180,120);
    mote->draw_rect(fb,(int)grap_x-1,(int)grap_y-1,2,2,c,1,0,128);
}
static void draw_water_marks(uint16_t*fb){
    for(int s=0;s<nwsrc;s++){
        int x=wsrc_x[s], y=wsrc_y[s];
        mote->draw_rect(fb,x-1,y-2,2,2,MOTE_RGB565(90,200,255),1,0,128);   /* nozzle */
        mote->draw_pixel(fb,x,y,MOTE_RGB565(150,230,255));
    }
}
static void draw_toolbar(uint16_t*fb,const MoteFont*f){
    static const char*sh[T_NTOOLS]={"WATER","LOG","DIG","SAND","BLAST"};
    static const char lt[T_NTOOLS]={'W','L','D','S','B'};
    uint16_t ic[T_NTOOLS]={MOTE_RGB565(70,180,255),MOTE_RGB565(150,100,50),MOTE_RGB565(170,175,185),
                           MOTE_RGB565(220,190,120),MOTE_RGB565(255,110,40)};
    int cnt[T_NTOOLS]={water_left,log_left,-1,sand_left,blast_left};
    /* 5 compact icon slots; the active tool's full name shown above the bar */
    for(int i=0;i<T_NTOOLS;i++){
        int bx=2+i*25, by=117, sel=(i==tool);
        mote_ui_panel(fb,bx,by,23,10, sel?MOTE_RGB565(48,48,66):MOTE_RGB565(16,16,24),
                      sel?MOTE_RGB565(255,214,110):MOTE_RGB565(48,48,62));
        mote->draw_rect(fb,bx+2,by+3,4,4,ic[i],1,0,128);
        char b[8];
        if(cnt[i]>=0) snprintf(b,sizeof b,"%c%d",lt[i],cnt[i]); else snprintf(b,sizeof b,"%c",lt[i]);
        mote->text(fb,b,bx+8,by+2, sel?MOTE_RGB565(255,240,190):MOTE_RGB565(150,150,165));
    }
    if(f) mote->text_font(fb,f,sh[tool],3,104,MOTE_RGB565(255,230,150));
}
static void draw_source_arrow(uint16_t*fb){
    if(((int)(state_t*3))&1) return;                  /* blink */
    int ax=src_x[0]; uint16_t o=MOTE_RGB565(255,150,30);
    mote->draw_pixel(fb,ax,2,o);                      /* small UP arrow at the top edge */
    for(int k=1;k<4;k++){ mote->draw_pixel(fb,ax-k,2+k,o); mote->draw_pixel(fb,ax+k,2+k,o); }
    mote->draw_rect(fb,ax-1,5,3,5,o,1,0,128);         /* short stem */
}

static void g_overlay(uint16_t*fb){
    const MoteFont*fmed=(mote->abi_version>=47)?mote->ui_font(MOTE_FONT_MED):0;
    if(state==ST_TITLE){
        mote_dim_box(fb,0,0,128,128,0);
        if(fmed){
            mote->text_font(fb,mote->ui_font(MOTE_FONT_LARGE),"SLUICE",34,22,MOTE_RGB565(255,150,40));
            mote->text_font(fb,fmed,"Lava is coming.",22,50,MOTE_RGB565(230,220,225));
            mote->text_font(fb,fmed,"Place water + logs",12,66,MOTE_RGB565(150,200,210));
            mote->text_font(fb,fmed,"to steer it away.",16,78,MOTE_RGB565(150,200,210));
            mote->text_font(fb,fmed,"A: start",42,104,MOTE_RGB565(255,220,120));
        } else { mote->text_2x(fb,"SLUICE",34,40,MOTE_RGB565(255,150,40)); }
        return;
    }

    draw_water_marks(fb);
    draw_rope(fb);
    draw_player(fb);
    if(state==ST_READY) draw_source_arrow(fb);
    if(state==ST_READY||state==ST_PLAY) draw_reticle(fb);

    /* HUD bars: flow/countdown (left) + town integrity (right) */
    char buf[32];
    if(state==ST_READY){
        int secs=(int)phase_t+1; if(secs<0)secs=0;
        uint16_t c=MOTE_RGB565(255,180,60);
        if(fmed){ snprintf(buf,sizeof buf,"LAVA IN %d",secs);
                  mote->text_font(fb,fmed,buf,20,2,c); }
        else mote->text(fb,"READY",2,2,c);
    } else {
        float ff=flow_t/FLOW_TIME; if(ff<0)ff=0;
        mote_ui_bar(fb,2,2,60,4,ff,MOTE_RGB565(255,120,20),MOTE_RGB565(40,20,10));
    }
    mote_ui_bar(fb,66,2,60,4,town_int/100.0f, town_int>40?MOTE_RGB565(120,220,140):MOTE_RGB565(230,60,40),MOTE_RGB565(20,30,24));

    draw_toolbar(fb,fmed);

    if(state==ST_WIN||state==ST_LOSE){
        mote_dim_box(fb,0,44,128,40,0);
        const char*t=state==ST_WIN?"TOWN SAVED":(lose_cooked?"YOU DIED":"BURNED");
        uint16_t c=state==ST_WIN?MOTE_RGB565(80,240,160):MOTE_RGB565(255,80,50);
        if(fmed){ mote->text_font(fb,mote->ui_font(MOTE_FONT_LARGE),t,24,52,c);
                  mote->text_font(fb,fmed,"A: continue",34,72,MOTE_RGB565(230,230,240)); }
        else mote->text_2x(fb,t,24,54,c);
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=render_band, .overlay=g_overlay,
    .config={ .max_points=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
