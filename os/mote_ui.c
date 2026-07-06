/*
 * Mote OS — shared styled-UI kit. See mote_ui.h.
 */
#include "mote_ui.h"
#include "mote_config.h"
#include "mote_font.h"
#include "mote_ui_font_med.h"   /* baked Audiowide @11px -> mote_ui_med  */
#include "mote_ui_font_lg.h"    /* baked Audiowide @15px -> mote_ui_lg   */
#include "mote_ui_font_read.h"  /* baked Audiowide @13px -> mote_ui_rd (reading body) */
#include <string.h>

void mote_ui_text  (uint16_t *fb, const char *s, int x, int y, uint16_t c){ mote_font_draw_aa(fb,&mote_ui_med,s,x,y,c); }
void mote_ui_title (uint16_t *fb, const char *s, int x, int y, uint16_t c){ mote_font_draw_aa(fb,&mote_ui_lg ,s,x,y,c); }
void mote_ui_read  (uint16_t *fb, const char *s, int x, int y, uint16_t c){ mote_font_draw_aa(fb,&mote_ui_rd ,s,x,y,c); }
int  mote_ui_text_w (const char *s){ return mote_font_aa_width(&mote_ui_med,s); }
int  mote_ui_title_w(const char *s){ return mote_font_aa_width(&mote_ui_lg ,s); }
int  mote_ui_read_w (const char *s){ return mote_font_aa_width(&mote_ui_rd ,s); }
int  mote_ui_text_h (void){ return mote_ui_med.line_h; }
int  mote_ui_title_h(void){ return mote_ui_lg.line_h; }
int  mote_ui_read_h (void){ return mote_ui_rd.line_h; }

#define W MOTE_FB_W
#define H MOTE_FB_H

#define C_BAR    MOTE_RGB565(18, 22, 40)
#define C_ACCENT MOTE_RGB565(96, 176, 255)
#define C_TITLE  MOTE_RGB565(255, 206, 92)    /* gold */
#define C_SEL    MOTE_RGB565(36, 74, 138)
#define C_SELED  MOTE_RGB565(120, 200, 255)
#define C_TEXT   MOTE_RGB565(224, 230, 244)
#define C_DIM    MOTE_RGB565(120, 134, 162)

#define HDR_H 18
#define FTR_H 15
#define ROW_H 15

static void rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c){
    for(int j=y;j<y+h;j++){ if((unsigned)j>=H) continue;
        uint16_t *r=fb+j*W; for(int i=x;i<x+w;i++) if((unsigned)i<W) r[i]=c; }
}
static void tri(uint16_t *fb, int cx, int y, int up, uint16_t c){   /* small arrow */
    for(int r=0;r<3;r++){ int yy=up?y+r:y+(2-r); rect(fb, cx-r, yy, 2*r+1, 1, c); }
}

void mote_ui_dim(uint16_t *fb){
    for(int i=0;i<W*H;i++){ uint16_t c=fb[i];
        int r=(c>>11)&31,g=(c>>5)&63,b=c&31;
        fb[i]=(uint16_t)(((r/4)<<11)|((g/4)<<5)|(b/4)); }
}
void mote_ui_ground(uint16_t *fb){ for(int i=0;i<W*H;i++) fb[i]=MOTE_UI_BG; }

void mote_ui_header(uint16_t *fb, const char *title, int idx, int count){
    rect(fb, 0, 0, W, HDR_H, C_BAR);
    rect(fb, 0, HDR_H-2, W, 1, C_ACCENT);                 /* accent rule */
    rect(fb, 0, HDR_H-1, W, 1, MOTE_RGB565(20,40,70));
    mote_ui_title(fb, title, 4, 2, C_TITLE);
    if(count >= 0){
        char b[10]; int q=0, v=idx;
        char t[6]; int m=0; if(v==0)t[m++]='0'; while(v){t[m++]=(char)('0'+v%10);v/=10;}
        while(m) b[q++]=t[--m]; b[q++]='/';
        v=count; m=0; if(v==0)t[m++]='0'; while(v){t[m++]=(char)('0'+v%10);v/=10;}
        while(m) b[q++]=t[--m]; b[q]=0;
        mote_ui_text(fb, b, W - mote_ui_text_w(b) - 4, 4, C_ACCENT);
    }
}

void mote_ui_footer(uint16_t *fb, const char *hint){
    rect(fb, 0, H-FTR_H, W, FTR_H, C_BAR);
    rect(fb, 0, H-FTR_H, W, 1, C_ACCENT);
    if(hint){ int w=mote_ui_text_w(hint); mote_ui_text(fb, hint, (W-w)/2, H-FTR_H+3, C_DIM); }
}

int mote_ui_list(uint16_t *fb, const char *const *items, int n, int sel, int top, int y0){
    int rows = (H - FTR_H - y0) / ROW_H;
    if(rows < 1) rows = 1;
    if(sel < top) top = sel;
    if(sel >= top + rows) top = sel - rows + 1;
    if(top < 0) top = 0;
    for(int r=0; r<rows && top+r < n; r++){
        int i = top + r, y = y0 + r*ROW_H;
        if(i == sel){
            rect(fb, 2, y, W-4, ROW_H-1, C_SEL);
            rect(fb, 2, y, 2, ROW_H-1, C_SELED);          /* bright left edge */
            mote_ui_text(fb, items[i], 9, y+2, C_TEXT);
        } else {
            mote_ui_text(fb, items[i], 9, y+2, C_DIM);
        }
    }
    /* scroll arrows */
    if(top > 0)      tri(fb, W-6, y0+1, 1, C_ACCENT);
    if(top+rows < n) tri(fb, W-6, H-FTR_H-5, 0, C_ACCENT);
    return top;
}
