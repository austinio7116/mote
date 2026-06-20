#include "golf_gen.h"
#include <math.h>

#define BASE_Y   4.0f
#define GREEN_R  7.0f
#define FAIR_HALF 6.0f
#define TEE_R    3.5f

/* ---- VENDORED from ThumbyGolf src/golf_terrain.c ---- */
static int32_t hash32(int x, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x*0x9E3779B9u ^ (uint32_t)z*0x85EBCA77u ^ seed;
    h ^= h>>16; h *= 0x7feb352du; h ^= h>>15; h *= 0x846ca68bu; h ^= h>>16;
    return (int32_t)h;
}
static float lat(int gx, int gz, uint32_t s){ return (float)hash32(gx,gz,s)*(1.0f/2147483648.0f); }
static float ss(float t){ return t*t*(3.0f-2.0f*t); }
static float noise2d(float wx, float wz, float p, uint32_t s){
    float u=wx/p, v=wz/p; int x0=(int)floorf(u), z0=(int)floorf(v);
    float tx=ss(u-x0), tz=ss(v-z0);
    float a=lat(x0,z0,s)+(lat(x0+1,z0,s)-lat(x0,z0,s))*tx;
    float b=lat(x0,z0+1,s)+(lat(x0+1,z0+1,s)-lat(x0,z0+1,s))*tx;
    return a+(b-a)*tz;
}
static float noise_aniso(float wx, float wz, float p, float ca, float sa, float st, uint32_t s){
    float u=(ca*wx+sa*wz)/p, v=(-sa*wx+ca*wz)/(p*st);
    int x0=(int)floorf(u), z0=(int)floorf(v); float tx=ss(u-x0), tz=ss(v-z0);
    float a=lat(x0,z0,s)+(lat(x0+1,z0,s)-lat(x0,z0,s))*tx;
    float b=lat(x0,z0+1,s)+(lat(x0+1,z0+1,s)-lat(x0,z0+1,s))*tx;
    return a+(b-a)*tz;
}
typedef struct { float hp,ha,mp,ma,ip,ia,lp,la,aniso,ridge; } StyleNoise;
/* Cranked for DRAMA: hero wavelength ~hole-length so a hill + valley fit INSIDE
 * the hole, with big amplitudes (ThumbyGolf's gentle "playable" tuning read flat
 * over 60m). The fairway/green still mow toward the low-freq contour so the
 * relief shows but the green stays puttable. */
static const StyleNoise STY[3] = {
    {46,14.0f,21,5.0f,10,1.0f,5,0.24f,2.2f,0.5f},    /* Links: big dunes */
    {52,11.0f,23,4.0f,11,0.8f,6,0.18f,1.0f,0.0f},    /* Parkland: rolling */
    {44,13.0f,19,4.5f,10,0.9f,5,0.22f,1.5f,0.30f},   /* Heathland: heaving heath */
};
static float natural_h(uint32_t seed, int style, float x, float z){
    const StyleNoise *s=&STY[style]; float ca=0.7071f, sa=0.7071f, h=BASE_Y;
    float hero = (s->aniso>1.01f) ? noise_aniso(x,z,s->hp,ca,sa,s->aniso,seed)
                                  : noise2d(x,z,s->hp,seed);
    if (s->ridge>0.01f) hero = hero*(1.0f-s->ridge) + (1.0f-fabsf(hero))*s->ridge;
    h += hero*s->ha;
    h += noise2d(x,z,s->mp,seed^0x1111u)*s->ma;
    h += noise2d(x,z,s->ip,seed^0x2222u)*s->ia;
    h += noise2d(x,z,s->lp,seed^0x3333u)*s->la;
    return h;
}
/* low-frequency contour only (hero+macro) — the "mown" surface a fairway/green
 * sits on: keeps the land's broad roll, drops the bumpy detail. */
static float smooth_h(uint32_t seed, int style, float x, float z){
    const StyleNoise *s=&STY[style]; float ca=0.7071f, sa=0.7071f, h=BASE_Y;
    float hero = (s->aniso>1.01f) ? noise_aniso(x,z,s->hp,ca,sa,s->aniso,seed)
                                  : noise2d(x,z,s->hp,seed);
    if (s->ridge>0.01f) hero = hero*(1.0f-s->ridge) + (1.0f-fabsf(hero))*s->ridge;
    h += hero*s->ha;
    h += noise2d(x,z,s->mp,seed^0x1111u)*(s->ma*0.6f);
    return h;
}
/* ---- end vendored ---- */

static float fmin2(float a,float b){return a<b?a:b;} static float fmax2(float a,float b){return a>b?a:b;}

void golf_generate(GolfHole *h, uint32_t seed){
    h->seed=seed; h->style=(int)(seed%3u);
    h->tee_x=0; h->tee_z=0;
    float ang = 0.30f*((float)(hash32(7,7,seed)&255)/255.0f - 0.5f);   /* dogleg angle */
    h->length_m = 38.0f + 92.0f*((float)(hash32(3,9,seed)&255)/255.0f);  /* 38..130m: par 3/4/5 */
    h->cup_x = sinf(ang)*h->length_m;
    h->cup_z = cosf(ang)*h->length_m;
    h->bend_x = (h->tee_x+h->cup_x)*0.5f + 12.0f*((float)(hash32(5,5,seed)&255)/255.0f-0.5f);
    h->bend_z = (h->tee_z+h->cup_z)*0.5f;
    h->par = (h->length_m < 55.0f) ? 3 : (h->length_m < 92.0f ? 4 : 5);
    h->tee_h = smooth_h(seed,h->style,h->tee_x,h->tee_z);
    h->cup_h = smooth_h(seed,h->style,h->cup_x,h->cup_z);
    float pad=34.0f;   /* generous space around the hole for wayward shots */
    h->min_x = fmin2(h->tee_x,fmin2(h->cup_x,h->bend_x))-pad;
    h->max_x = fmax2(h->tee_x,fmax2(h->cup_x,h->bend_x))+pad;
    h->min_z = fmin2(h->tee_z,h->cup_z)-pad;
    h->max_z = fmax2(h->tee_z,h->cup_z)+pad;
    /* water: natural lows flood (higher level -> more visible water) */
    h->water_level = fmin2(h->tee_h,h->cup_h) - 2.2f;
    /* bunkers: greenside pair (from the approach direction) + a fairway bunker */
    float ax=h->cup_x-h->bend_x, az=h->cup_z-h->bend_z, al=sqrtf(ax*ax+az*az); if(al<1e-3f)al=1.0f; ax/=al; az/=al;
    float px=-az, pz=ax;
    h->n_bunker=0;
    h->bunker_x[0]=h->cup_x+px*9.0f-ax*3.0f; h->bunker_z[0]=h->cup_z+pz*9.0f-az*3.0f; h->bunker_r[0]=4.5f;
    h->bunker_x[1]=h->cup_x-px*8.0f-ax*2.0f; h->bunker_z[1]=h->cup_z-pz*8.0f-az*2.0f; h->bunker_r[1]=4.0f;
    h->bunker_x[2]=h->bend_x+px*7.0f;        h->bunker_z[2]=h->bend_z+pz*7.0f;        h->bunker_r[2]=5.0f;
    h->n_bunker=3;
}

static float dist_seg(float px,float pz,float ax,float az,float bx,float bz){
    float dx=bx-ax,dz=bz-az,l2=dx*dx+dz*dz;
    if(l2<1e-6f) return sqrtf((px-ax)*(px-ax)+(pz-az)*(pz-az));
    float t=((px-ax)*dx+(pz-az)*dz)/l2; if(t<0)t=0; if(t>1)t=1;
    float cx=ax+t*dx,cz=az+t*dz; return sqrtf((px-cx)*(px-cx)+(pz-cz)*(pz-cz));
}
float golf_route_dist(const GolfHole *h, float x, float z){
    float d1=dist_seg(x,z,h->tee_x,h->tee_z,h->bend_x,h->bend_z);
    float d2=dist_seg(x,z,h->bend_x,h->bend_z,h->cup_x,h->cup_z);
    return d1<d2?d1:d2;
}

float golf_surface(const GolfHole *h, float x, float z){
    float nat = natural_h(h->seed,h->style,x,z);        /* rough: full detail */
    float smo = smooth_h(h->seed,h->style,x,z);         /* mown low-freq contour */
    float ht  = nat;
    float rd = golf_route_dist(h,x,z);
    float fw = (rd < FAIR_HALF) ? 1.0f : (rd < FAIR_HALF+4.0f ? ss(1.0f-(rd-FAIR_HALF)*0.25f) : 0.0f);
    if (fw > 0.0f) ht = nat*(1.0f-fw) + smo*fw;          /* fairway mown to contour */
    float dg = sqrtf((x-h->cup_x)*(x-h->cup_x)+(z-h->cup_z)*(z-h->cup_z));
    float gw = (dg < GREEN_R) ? 1.0f : (dg < GREEN_R+2.5f ? ss(1.0f-(dg-GREEN_R)*0.4f) : 0.0f);
    if (gw > 0.0f) ht = ht*(1.0f-gw) + h->cup_h*gw;      /* green flat */
    float dt = sqrtf((x-h->tee_x)*(x-h->tee_x)+(z-h->tee_z)*(z-h->tee_z));
    float tw = (dt < TEE_R) ? 1.0f : (dt < TEE_R+1.5f ? ss(1.0f-(dt-TEE_R)*0.66f) : 0.0f);
    if (tw > 0.0f) ht = ht*(1.0f-tw) + h->tee_h*tw;      /* tee flat */
    for(int i=0;i<h->n_bunker;i++){                       /* bunker hollows */
        float dx=x-h->bunker_x[i], dz=z-h->bunker_z[i], d=sqrtf(dx*dx+dz*dz);
        if(d<h->bunker_r[i]) ht -= 1.3f*ss(1.0f-d/h->bunker_r[i]);
    }
    return ht;
}
float golf_height(const GolfHole *h, float x, float z){
    float s = golf_surface(h,x,z);
    return s < h->water_level ? h->water_level : s;       /* flat water surface */
}

int golf_lie(const GolfHole *h, float x, float z){
    float dg = sqrtf((x-h->cup_x)*(x-h->cup_x)+(z-h->cup_z)*(z-h->cup_z));
    if(dg<GREEN_R) return GOLF_GREEN;
    float dt = sqrtf((x-h->tee_x)*(x-h->tee_x)+(z-h->tee_z)*(z-h->tee_z));
    if(dt<TEE_R) return GOLF_TEE;
    for(int i=0;i<h->n_bunker;i++){
        float dx=x-h->bunker_x[i], dz=z-h->bunker_z[i];
        if(dx*dx+dz*dz < h->bunker_r[i]*h->bunker_r[i]) return GOLF_BUNKER;
    }
    if(golf_surface(h,x,z) < h->water_level) return GOLF_WATER;
    return (golf_route_dist(h,x,z)<FAIR_HALF) ? GOLF_FAIRWAY : GOLF_ROUGH;
}

int golf_tree(const GolfHole *h, float x, float z){
    if(golf_lie(h,x,z)!=GOLF_ROUGH) return 0;
    float rd = golf_route_dist(h,x,z);
    if(rd < FAIR_HALF+1.0f) return 0;
    float c = noise2d(x,z,20.0f,h->seed^0xABCDu)*0.5f+0.5f;     /* low-freq cluster */
    float dens = (h->style==GOLF_PARKLAND)?0.80f:(h->style==GOLF_HEATHLAND?0.60f:0.45f);
    if(rd < FAIR_HALF+7.0f && dens < 0.75f) dens = 0.75f;        /* always line the fairway */
    float roll = (float)(hash32((int)floorf(x),(int)floorf(z),h->seed^0x9999u)&255)/255.0f;
    return c*dens > roll;
}
