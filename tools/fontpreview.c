/*
 * fontpreview — render representative Mote UI panels with the currently-baked
 * medium/large UI fonts, so we can compare candidate fonts side by side.
 *
 * Links the REAL mote_ui + mote_font_draw_aa, so what it draws is exactly what
 * the launcher/gallery would render. Emits a 128 x 260 BMP: a game-picker panel
 * on top and a gallery-hero panel below, separated by a 4px gap.
 *
 *   cc fontpreview.c ../os/mote_ui.c ../engine/render/mote_font.c \
 *      -I../os -I../engine/core -I../engine/render -lm -o fontpreview
 *   ./fontpreview out.bmp
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "mote_ui.h"
#include "mote_config.h"

#define W MOTE_FB_W
#define H MOTE_FB_H

static void rect(uint16_t *fb,int x,int y,int w,int h,uint16_t c){
    for(int j=y;j<y+h;j++){ if((unsigned)j>=H)continue;
        for(int i=x;i<x+w;i++){ if((unsigned)i>=W)continue; fb[j*W+i]=c; } } }

/* ---- panel 1: the game picker (list) ------------------------------------- */
static void panel_picker(uint16_t *fb){
    static const char *games[]={"WolfMote","Grand Thumb Auto","ThumbyCue","MotoKart","DeepThumb","PaperMote"};
    mote_ui_ground(fb);
    mote_ui_header(fb,"MOTE",2,6);
    mote_ui_list(fb,games,6,1,0,21);
    mote_ui_footer(fb,"A PLAY   RB GALLERY");
}
/* ---- panel 2: the gallery hero (thumbnail + big title + details) --------- */
static void panel_gallery(uint16_t *fb){
    mote_ui_ground(fb);
    mote_ui_header(fb,"GALLERY",7,20);
    const int TS=64, tx=(W-TS)/2, ty=20;                            /* matches gg_draw */
    for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){                     /* fake screenshot gradient */
        int r=(x*255/TS)>>3, g=(y*255/TS)>>2, b=((x+y)*128/TS)>>3;
        fb[(ty+y)*W+tx+x]=(uint16_t)((r<<11)|(g<<5)|b); }
    rect(fb,tx-2,ty-2,TS+4,2,0x18E3); rect(fb,tx-2,ty+TS,TS+4,2,0x18E3);
    rect(fb,tx-2,ty-2,2,TS+4,0x18E3); rect(fb,tx+TS,ty-2,2,TS+4,0x18E3);
    /* chips outside the box */
    rect(fb,1,ty,mote_ui_text_w("UPD")+4,13,0x8200); mote_ui_text(fb,"UPD",3,ty+1,0xFE60);
    { int bw=mote_ui_text_w("2P")+4; rect(fb,W-bw-1,ty,bw,13,0x2A4A); mote_ui_text(fb,"2P",W-bw+1,ty+1,0x8FF4); }
    /* forced-large title (wrap to 2 lines) */
    const char *nm="Grand Thumb Auto"; char l1[40],l2[40]; int nl=1;
    if(mote_ui_title_w(nm)<=W-4){ snprintf(l1,sizeof l1,"%s",nm); l2[0]=0; }
    else { const char *sp=strrchr(nm,' '); int c=sp?(int)(sp-nm):(int)strlen(nm);
           memcpy(l1,nm,c); l1[c]=0; snprintf(l2,sizeof l2,"%s",sp?sp+1:""); nl=2; }
    int y0=nl==1?87:85, lh=mote_ui_title_h();
    mote_ui_title(fb,l1,(W-mote_ui_title_w(l1))/2,y0,0xFFFF);
    if(nl==2) mote_ui_title(fb,l2,(W-mote_ui_title_w(l2))/2,y0+lh,0xFFFF);
    int yb=y0+nl*lh;
    if(nl==1) mote_ui_text(fb,"v1.1.0   386 KB",(W-mote_ui_text_w("v1.1.0   386 KB"))/2,yb+1,0x9CD3);
    mote_ui_text(fb,"A install  LB info  B",(W-mote_ui_text_w("A install  LB info  B"))/2,116,0x8410);
}

/* ---- panel 3: the engine overlay menu (widened for Audiowide) ------------ */
static void bar3(uint16_t*fb,int x,int y,int w,int h,int pct,uint16_t fg){
    rect(fb,x,y,w,h,0x1CE7); rect(fb,x,y,pct*w/100,h,fg); }
static void panel_menu(uint16_t *fb){
    for(int i=0;i<W*H;i++) fb[i]=0x2124;                 /* stand-in for a dimmed game */
    static const char*rows[]={"PERF","BRIGHT","VOLUME","USB LOGS","EXIT TO LOBBY","RESUME"};
    const int PX=4,PY=8,PW=120,ROW_H=14,ROW_Y=PY+22,M_N=6,PH=22+M_N*ROW_H+12;
    rect(fb,PX,PY,PW,PH,0x10A6); rect(fb,PX,PY,PW,18,0x1567);
    rect(fb,PX,PY,PW,1,MOTE_UI_ACCENT); rect(fb,PX,PY+PH-1,PW,1,MOTE_UI_ACCENT);
    rect(fb,PX,PY,1,PH,MOTE_UI_ACCENT); rect(fb,PX+PW-1,PY,1,PH,MOTE_UI_ACCENT);
    rect(fb,PX,PY+17,PW,1,MOTE_UI_ACCENT);
    mote_ui_text(fb,"ENGINE MENU",PX+8,PY+3,MOTE_UI_GOLD);
    int sel=3;
    for(int i=0;i<M_N;i++){ int y=ROW_Y+i*ROW_H;
        if(i==sel){ rect(fb,PX+2,y-1,PW-4,13,0x2251); rect(fb,PX+2,y-1,2,13,0x663F); }
        uint16_t tc=(i==sel)?0xFFFF:MOTE_UI_TEXT; int vx=PX+72;
        mote_ui_text(fb,rows[i],PX+8,y,tc);
        if(i==0) mote_ui_text(fb,"FULL",vx,y,0x9764);
        else if(i==1) bar3(fb,vx,y+1,42,8,80,0xEE6B);
        else if(i==2) bar3(fb,vx,y+1,42,8,60,0x6DDF);
        else if(i==3) mote_ui_text(fb,"ON",vx,y,0x9764);
    }
    mote_ui_text(fb,"L/R set   B close",(W-mote_ui_text_w("L/R set   B close"))/2,PY+PH-12,MOTE_UI_DIM);
}

/* ---- panel 4: the gallery details modal (standard Mote palette) ---------- */
static void panel_modal(uint16_t *fb){
    mote_ui_ground(fb);
    rect(fb,0,0,W,18,MOTE_UI_BAR); rect(fb,0,16,W,1,MOTE_UI_ACCENT);
    mote_ui_text(fb,"Grand Thumb Auto",4,3,MOTE_UI_GOLD);
    mote_ui_text(fb,"by austinio7116",4,20,MOTE_UI_DIM);
    const char*lines[]={"An open-world driving","game with a living city,","traffic, cops and","total mayhem across","eight districts.","Steal any car."};
    int lh=mote_ui_read_h()+2;
    for(int r=0;r<6;r++) mote_ui_read(fb,lines[r],4,32+r*lh,MOTE_UI_TEXT);
    int fy=H-15; rect(fb,0,fy,W,15,MOTE_UI_BAR); rect(fb,0,fy,W,1,MOTE_UI_ACCENT);
    mote_ui_text(fb,"^v scroll  B close",(W-mote_ui_text_w("^v scroll  B close"))/2,fy+2,MOTE_UI_DIM);
}
static void put16(FILE*f,int v){fputc(v&255,f);fputc((v>>8)&255,f);}
static void put32(FILE*f,long v){for(int i=0;i<4;i++)fputc((v>>(i*8))&255,f);}
static void write_bmp(const char*path,const uint16_t*img,int w,int h){
    FILE*f=fopen(path,"wb"); if(!f){perror(path);return;}
    int row=(w*3+3)&~3, sz=54+row*h;
    fputc('B',f);fputc('M',f); put32(f,sz); put16(f,0);put16(f,0); put32(f,54);
    put32(f,40); put32(f,w); put32(f,h); put16(f,1); put16(f,24); put32(f,0);
    put32(f,row*h); put32(f,2835);put32(f,2835);put32(f,0);put32(f,0);
    unsigned char*line=calloc(1,row);
    for(int y=h-1;y>=0;y--){ for(int x=0;x<w;x++){ uint16_t c=img[y*w+x];
        line[x*3+0]=(unsigned char)((c&31)<<3); line[x*3+1]=(unsigned char)(((c>>5)&63)<<2); line[x*3+2]=(unsigned char)(((c>>11)&31)<<3); }
        fwrite(line,1,row,f); }
    free(line); fclose(f);
}

int main(int argc,char**argv){
    const char*out=argc>1?argv[1]:"fontpreview.bmp";
    static uint16_t a[W*H], b[W*H], c[W*H], d[W*H];
    panel_picker(a); panel_gallery(b); panel_menu(c); panel_modal(d);
    uint16_t*P[4]={a,b,c,d}; int GAP=4, TH=H*4+GAP*3;
    uint16_t *img=malloc(sizeof(uint16_t)*W*TH);
    for(int i=0;i<W*TH;i++) img[i]=0x0000;
    for(int p=0;p<4;p++) memcpy(img + W*(p*(H+GAP)), P[p], sizeof a);
    write_bmp(out,img,W,TH); free(img);
    printf("wrote %s (%dx%d)\n",out,W,TH);
    return 0;
}
