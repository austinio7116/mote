/* Golf club rig — I set each club's loft + spin (the shot SHAPE: apex, descent,
 * roll-check) and the target carry; the rig binary-searches the launch SPEED to
 * hit that carry, then reports the realised apex/roll/total/descent. Read the
 * solved speeds straight into game.c CLUBS. Flight matches the game (Magnus once
 * per frame, gravity in substeps); yards = world*4. */
#include <stdio.h>
#include <math.h>
#define YARD 4.0f
#define MAG  0.0030f
typedef struct { const char*name; float loft, spin, tgt_carry; } Spec;
static Spec spec[5] = {
    {"DRIVER", 18.0f, 0.40f, 215.0f}, {"3 WOOD", 21.0f, 0.60f, 200.0f},
    {"5 IRON", 28.0f, 1.00f, 165.0f}, {"8 IRON", 38.0f, 1.50f, 132.0f},
    {"WEDGE",  54.0f, 2.20f,  96.0f},
};
static float tgt_apex[5]={26,24,27,30,33};

static void sim(float speed,float loft,float spin,float*oc,float*oa,float*ot,float*od){
    float df=1.0f/60.0f, ds=1.0f/240.0f, g=9.8f; int sub=4;
    float lr=loft*(float)M_PI/180.0f, vx=speed*cosf(lr), vy=speed*sinf(lr), W=28.0f*spin;
    float x=0,y=0,apex=0,carry=-1,desc=0; int landed=0;
    for(int f=0;f<20000;f++){
        if(y>0.02f){ vx+=-W*vy*MAG*df; vy+=W*vx*MAG*df; }
        for(int s=0;s<sub;s++){ vy-=g*ds; x+=vx*ds; y+=vy*ds; if(y>apex)apex=y;
            if(y<=0.0f){ if(!landed){landed=1;carry=x;desc=atan2f(-vy,vx)*180.0f/(float)M_PI;} y=0;
                if(vy<-0.4f){ vy=-vy*0.30f; vx*=(0.5f-0.16f*spin); }
                else { vy=0; vx*=(1.0f-(2.2f+2.6f*spin)*ds); } } }
        if(landed && fabsf(vx)<0.12f && y<=0.0f) break;
    }
    *oc=carry*YARD; *oa=apex*YARD; *ot=x*YARD; *od=desc;
}
static float solve_speed(Spec s){           /* binary search speed for target carry */
    float lo=5,hi=70;
    for(int it=0;it<40;it++){ float m=(lo+hi)*0.5f,ca,ap,to,de; sim(m,s.loft,s.spin,&ca,&ap,&to,&de);
        if(ca<s.tgt_carry) lo=m; else hi=m; }
    return (lo+hi)*0.5f;
}
int main(void){
    printf("%-7s %5s %5s %5s | %6s %5s %6s %5s %4s\n","club","speed","loft","spin","carry","apex","total","roll","desc");
    for(int i=0;i<5;i++){ float sp=solve_speed(spec[i]); float ca,ap,to,de; sim(sp,spec[i].loft,spec[i].spin,&ca,&ap,&to,&de);
        printf("%-7s %5.1f %5.0f %5.2f | %5.0fy %4.0fy(t%2.0f) %5.0fy %4.0fy %3.0f\n",
            spec[i].name,sp,spec[i].loft,spec[i].spin,ca,ap,tgt_apex[i],to,to-ca,de); }
    return 0;
}
