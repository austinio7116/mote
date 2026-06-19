#include "mote_phys.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
int main(void){
    MoteWorld w; mote_phys_world_defaults(&w);
    w.bmin=v3(-1.8f,-1.4f,-1.8f); w.bmax=v3(1.8f,5.0f,1.8f);
    w.substep=1.0f/120.0f; w.max_substeps=4; w.restitution=0.55f;
    int N=48; MoteBody b[48]; memset(b,0,sizeof b);
    // tight 4x4x3 cluster, slightly overlapping spacing to provoke simultaneous deep contacts
    int per=4; float sp=0.50f;  // < 0.52 diameter -> initial overlap, worst case
    for(int i=0;i<N;i++){
        b[i].inv_mass=1.0f/0.3f; b[i].orient=m3_identity();
        if(i&1){b[i].shape=MOTE_SHAPE_SPHERE;b[i].radius=0.26f;}
        else{b[i].shape=MOTE_SHAPE_BOX;b[i].half=v3(0.24f,0.24f,0.24f);b[i].radius=0.26f;}
        int cell=i%(per*per),layer=i/(per*per),gx=cell%per,gz=cell/per;
        b[i].pos=v3((gx-1.5f)*sp,1.0f+layer*sp,(gz-1.5f)*sp);
    }
    float maxspeed=0,maxabs=0;
    for(int f=0;f<1500;f++){
        mote_phys_step(&w,b,N,1.0f/60.0f);
        if(f>300){ // after initial settle, watch for explosions
            for(int i=0;i<N;i++){
                float sp2=v3_len(b[i].vel); if(sp2>maxspeed)maxspeed=sp2;
                float a=fmaxf(fmaxf(fabsf(b[i].pos.x),fabsf(b[i].pos.y)),fabsf(b[i].pos.z));
                if(a>maxabs)maxabs=a;
            }
        }
    }
    int escaped=0; for(int i=0;i<N;i++){ if(b[i].pos.y<-2.0f||b[i].pos.y>6.0f||fabsf(b[i].pos.x)>2.5f||fabsf(b[i].pos.z)>2.5f) escaped++; }
    printf("after-settle max speed=%.2f m/s, max |coord|=%.2f, escaped=%d\n",maxspeed,maxabs,escaped);
    printf("RESULT: %s\n",(maxspeed<3.0f && escaped==0)?"STABLE (no explosion)":"EXPLOSION DETECTED");
    return 0;
}
