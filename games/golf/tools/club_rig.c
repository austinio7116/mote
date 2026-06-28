/* Golf club rig — full 14-club bag. I set each club's loft + spin + target carry;
 * the rig solves the launch SPEED, then reports the shot SHAPE. Flight matches the
 * game (Magnus once/frame, gravity in substeps). Roll uses rolling resistance PLUS
 * a backspin brake that bleeds away — high-spin clubs check, the wedges spin BACK
 * (negative roll). yards = world*4. Read speeds straight into game.c CLUBS. */
#include <stdio.h>
#include <math.h>
#define YARD 4.0f
#define MAG   0.0030f      /* airborne Magnus (game flight term) */
#define BRAKE 0.0060f     /* backspin grab on the ground */
#define SDEC  2.6f         /* backspin bleed per second */
#define RD    3.2f         /* rolling resistance */
typedef struct { const char*name; float loft, spin, tgt; } Spec;
static Spec spec[13] = {
    {"DRIVER",18,0.40f,215},{"3 WOOD",21,0.55f,200},{"5 WOOD",23,0.65f,186},
    {"4 IRON",25,0.80f,174},{"5 IRON",28,0.95f,164},{"6 IRON",31,1.10f,152},
    {"7 IRON",34,1.25f,141},{"8 IRON",38,1.45f,130},{"9 IRON",42,1.65f,118},
    {"PW",47,1.95f,105},{"GW",51,2.25f,92},{"SW",56,2.60f,78},{"LW",62,3.00f,62},
};
static void sim(float speed,float loft,float spin,float*oc,float*oa,float*ot,float*od){
    float df=1.f/60,ds=1.f/240,g=9.8f; int sub=4;
    float lr=loft*(float)M_PI/180, vx=speed*cosf(lr), vy=speed*sinf(lr), W=28*spin;
    float x=0,y=0,apex=0,carry=-1,desc=0; int landed=0;
    for(int f=0;f<30000;f++){
        if(y>0.02f){ vx+=-W*vy*MAG*df; vy+=W*vx*MAG*df; }
        for(int s=0;s<sub;s++){ vy-=g*ds; x+=vx*ds; y+=vy*ds; if(y>apex)apex=y;
            if(y<=0.f){ if(!landed){landed=1;carry=x;desc=atan2f(-vy,vx)*180/(float)M_PI; W*=0.85f;} y=0;
                if(vy<-0.4f){ vy=-vy*0.26f; vx*=(0.52f-0.005f*desc); }
                else { vy=0; vx*=(1.f-RD*ds); vx-= (W*BRAKE)*ds; W*=(1.f-SDEC*ds);  /* backspin brake */ } } }
        if(landed && fabsf(vx)<0.04f && W<1.0f && y<=0.f) break;
    }
    *oc=carry*YARD; *oa=apex*YARD; *ot=x*YARD; *od=desc;
}
static float solve(Spec s){ float lo=4,hi=70; for(int i=0;i<44;i++){ float m=(lo+hi)/2,c,a,t,d;
    sim(m,s.loft,s.spin,&c,&a,&t,&d); if(c<s.tgt)lo=m; else hi=m;} return (lo+hi)/2; }
int main(void){
    printf("%-7s %5s %4s %5s | %5s %5s %6s %6s %4s\n","club","spd","lft","spin","carry","apex","total","roll","desc");
    for(int i=0;i<13;i++){ float sp=solve(spec[i]),c,a,t,d; sim(sp,spec[i].loft,spec[i].spin,&c,&a,&t,&d);
        printf("%-7s %5.1f %4.0f %5.2f | %4.0fy %4.0fy %5.0fy %+5.0fy %3.0f\n",
            spec[i].name,sp,spec[i].loft,spec[i].spin,c,a,t,t-c,d); }
    return 0;
}
