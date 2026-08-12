/* ThumbyCue pocket bench — one pocket, drawn straight down from above out of
 * the renderer's OWN mesh, with the circle the ball is taken on over the top.
 *
 * THE KNOBS ARE THE GAME'S OWN FIELDS, in the game's own units, so a number
 * dialled here goes back into the source as itself:
 *
 *   pr    cue_table_init  t->pr_corner   / t->pr_side     (xR)  the mouth
 *   gap   cue_table_init  t->gap_corner  / t->gap_side    (xR)  knuckle setback
 *   off   cue_table_init  t->off_corner  / t->off_side    (xR)  pocket centre
 *   back  cue_table_init  t->drop_back / t->drop_back_side (xR) how much
 *                         DEEPER than the pocket the drop circle is centred
 *   capm  cue_table.c build_world: the margin taken off pr to get the drop
 *                         circle — 0.30f, or side_m at a middle        (xR)
 *   set   cue_table_default_cut  CueCut.set                   (m)
 *   rad   cue_table_default_cut  CueCut.rad                         (x pr)
 *   roll  cue_table_default_cut  CueCut.roll                  (x pr)
 *
 * Called once per image by the web bench (pocketbench.py).
 */
#include "cue_table.h"
#include "cue_render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { CueGameKind k; const char *name; const char *label; const char *sym; } TB[] = {
    {CUE_GAME_SNK15,"snooker12","snooker 12ft",      "SNK15 + SNK10 (one block)"},
    {CUE_GAME_SNK10,"snooker10","snooker 10ft",      "SNK15 + SNK10 (one block)"},
    {CUE_GAME_UK8,  "uk7",      "UK8 7ft (+ 6-red)", "UK8 + SNK6 (one block)"},
    {CUE_GAME_US8,  "us9",      "US8 9ft (+ 9-ball)","US8 + US9 (one block)"},
    {CUE_GAME_CN8,  "chinese10","Chinese 8 10ft",    "CN8"},
};
#define NT 5


static CueTable T; static CueWorld W;

/* The mouth a ball actually goes through: the two knuckles beside the pocket
 * are circles, so it is their centre distance less both radii. */
static float jaw_sep(const CueWorld *w, int p) {
    int j1=-1,j2=-1; float d1=1e30f,d2=1e30f;
    for (int j=0;j<w->njaw;j++){
        float dx=w->jaw[j].x-w->pocket[p].x, dz=w->jaw[j].z-w->pocket[p].z;
        float dd=dx*dx+dz*dz;
        if(dd<d1){d2=d1;j2=j1;d1=dd;j1=j;} else if(dd<d2){d2=dd;j2=j;}
    }
    if(j1<0||j2<0) return 0.0f;
    float dx=w->jaw[j1].x-w->jaw[j2].x, dz=w->jaw[j1].z-w->jaw[j2].z;
    float s = sqrtf(dx*dx+dz*dz) - 2.0f*w->jaw_r;
    return s > 0.0f ? s : 0.0f;
}

typedef struct { float pr, gap, off, capm, back, set, rad, roll; } Knobs;

/* Built exactly the way the game builds it, with these numbers in place of the
 * shipped ones. Nothing here is a bench-only derivation: pr/gap/off go into
 * CueTable before build_world, capm reproduces build_world's own margin, and
 * set/rad/roll go through cue_table_derive_cut. */
static void build_tuned(int ti, int mid, const Knobs *k, CueTable *ot, CueWorld *ow) {
    CueTable t; cue_table_init(&t, TB[ti].k);
    if (mid) { t.pr_side   = k->pr*t.R;  t.gap_side   = k->gap*t.R;  t.off_side   = k->off*t.R;
               t.drop_back_side = k->back*t.R; t.cap_side = k->capm*t.R; }
    else     { t.pr_corner = k->pr*t.R;  t.gap_corner = k->gap*t.R;  t.off_corner = k->off*t.R;
               t.drop_back      = k->back*t.R; t.cap_corner = k->capm*t.R; }
    cue_table_build_world(&t, ow);
    int i = mid ? 1 : 0;
    ow->cut_set[i] = k->set; ow->cut_rad[i] = k->rad; ow->cut_roll[i] = k->roll;
    ow->cut_ref[i] = mid ? t.pr_side : t.pr_corner;
    cue_table_derive_cut(ow);
    *ot = t;
}

static int IW = 700, IH = 700;
static unsigned char *img;
static float *zb;
static float ox, oz, spm;
static void m2px(float x,float z,float*px,float*py){ *px=(x-ox)*spm+IW*0.5f; *py=(z-oz)*spm+IH*0.5f; }
static void put(int x,int y,int r,int g,int b){ if(x<0||y<0||x>=IW||y>=IH)return;
    unsigned char*q=img+((size_t)y*IW+x)*3; q[0]=r;q[1]=g;q[2]=b; }
static void dot(float fx,float fy,int rad,int r,int g,int b){
    for(int dy=-rad;dy<=rad;dy++)for(int dx=-rad;dx<=rad;dx++)
        if(dx*dx+dy*dy<=rad*rad) put((int)(fx+dx),(int)(fy+dy),r,g,b); }
static void circ_m(float cx,float cz,float rm,int r,int g,int b,int th,int dash){
    if(rm<=0) return;
    int n=(int)(rm*spm*8.0f)+360; if(n>20000)n=20000;
    for(int i=0;i<n;i++){
        if(dash && ((i*24/n)&1)) continue;
        float a=i*6.2831853f/n, p,q;
        m2px(cx+rm*cosf(a),cz+rm*sinf(a),&p,&q); dot(p,q,th,r,g,b); }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--defaults")) {
        printf("{\n");
        for (int i = 0; i < NT; i++) {
            cue_table_init(&T, TB[i].k); cue_table_build_world(&T, &W);
            printf("  \"%s\": {\"label\": \"%s\", \"sym\": \"%s\", \"ball\": %.4f",
                   TB[i].name, TB[i].label, TB[i].sym, (double)(W.R*1000.0f));
            for (int m = 0; m < 2; m++) {
                int p = m ? 5 : 2;
                CueCut c; cue_table_get_cut(&W, m, &c);
                printf(", \"%s\": {\"pr\": %.4f, \"gap\": %.4f, \"off\": %.4f, "
                       "\"capm\": %.4f, \"back\": %.4f, "
                       "\"set\": %.5f, \"rad\": %.4f, \"roll\": %.4f}",
                    m?"middle":"corner",
                    (double)((m ? T.pr_side  : T.pr_corner ) / T.R),
                    (double)((m ? T.gap_side : T.gap_corner) / T.R),
                    (double)((m ? T.off_side : T.off_corner) / T.R),
                    (double)((m ? T.cap_side : T.cap_corner) / T.R),
                    (double)((m ? T.drop_back_side : T.drop_back) / T.R),
                    (double)c.set, (double)c.rad, (double)c.roll);
            }
            printf("}%s\n", i+1<NT ? "," : "");
        }
        printf("}\n");
        return 0;
    }

    const char *tbl="snooker12", *ty="corner", *out="/tmp/pk.ppm";
    Knobs k = {-1,-1,-1,-1,-1,-1,-1,-1};
    float zoom=5.0f;
    for (int i=1;i<argc-1;i++){
        if(!strcmp(argv[i],"--table")) tbl=argv[++i];
        else if(!strcmp(argv[i],"--type")) ty=argv[++i];
        else if(!strcmp(argv[i],"--out"))  out=argv[++i];
        else if(!strcmp(argv[i],"--pr"))   k.pr  =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--gap"))  k.gap =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--off"))  k.off =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--capm")) k.capm=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--back")) k.back=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--set"))  k.set =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--rad"))  k.rad =(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--roll")) k.roll=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--zoom")) zoom=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--size")) { IW=IH=atoi(argv[++i]); }
    }
    int ti=0; for(int i=0;i<NT;i++) if(!strcmp(tbl,TB[i].name)) ti=i;
    int mid = (ty[0]=='m');
    /* anything not given falls back to what the game ships */
    {   CueTable t0; cue_table_init(&t0, TB[ti].k);
        CueWorld w0; cue_table_build_world(&t0, &w0);
        CueCut c0;   cue_table_get_cut(&w0, mid, &c0);
        if(k.pr  <0) k.pr  = (mid?t0.pr_side :t0.pr_corner )/t0.R;
        if(k.gap <0) k.gap = (mid?t0.gap_side:t0.gap_corner)/t0.R;
        if(k.off <0) k.off = (mid?t0.off_side:t0.off_corner)/t0.R;
        if(k.capm<0) k.capm= (mid?t0.cap_side:t0.cap_corner)/t0.R;
        if(k.back<0) k.back= (mid?t0.drop_back_side:t0.drop_back)/t0.R;
        if(k.set <0) k.set = c0.set;
        if(k.rad <0) k.rad = c0.rad;
        if(k.roll<0) k.roll= c0.roll;
    }
    build_tuned(ti, mid, &k, &T, &W);

    img=malloc((size_t)IW*IH*3); zb=malloc(sizeof(float)*(size_t)IW*IH);
    cue_render_set_buffers(malloc(cue_render_tab_bytes()), malloc(cue_render_stri_bytes()));
    cue_render_build_table(&T,&W);
    const CueTri *tri; int bd=0,lp=0; int ntri=cue_render_table_tris(&tri,&bd,&lp);

    int p = mid ? 5 : 2;
    ox=W.pocket[p].x; oz=W.pocket[p].z;
    float span = zoom*T.pr_corner; spm = IW/span;
    ox -= W.pmnorm[p].x*span*0.16f; oz -= W.pmnorm[p].z*span*0.16f;
    memset(img,12,(size_t)IW*IH*3);
    for(size_t i=0;i<(size_t)IW*IH;i++) zb[i]=-1e9f;
    for(int t=0;t<ntri;t++){
        const CueTri *q=&tri[t];
        float px0,py0,px1,py1,px2,py2;
        m2px(q->v[0].x,q->v[0].z,&px0,&py0);
        m2px(q->v[1].x,q->v[1].z,&px1,&py1);
        m2px(q->v[2].x,q->v[2].z,&px2,&py2);
        int lx=(int)floorf(fminf(px0,fminf(px1,px2))), hx=(int)ceilf(fmaxf(px0,fmaxf(px1,px2)));
        int ly=(int)floorf(fminf(py0,fminf(py1,py2))), hy=(int)ceilf(fmaxf(py0,fmaxf(py1,py2)));
        if(hx<0||hy<0||lx>=IW||ly>=IH) continue;
        if(lx<0)lx=0; if(ly<0)ly=0; if(hx>=IW)hx=IW-1; if(hy>=IH)hy=IH-1;
        float dd=(py1-py2)*(px0-px2)+(px2-px1)*(py0-py2); if(fabsf(dd)<1e-9f) continue;
        int R=(q->color>>11&31)*255/31, G=(q->color>>5&63)*255/63, B=(q->color&31)*255/31;
        for(int y=ly;y<=hy;y++)for(int x=lx;x<=hx;x++){
            float fx=x+0.5f, fy=y+0.5f;
            float w0=((py1-py2)*(fx-px2)+(px2-px1)*(fy-py2))/dd;
            float w1=((py2-py0)*(fx-px2)+(px0-px2)*(fy-py2))/dd;
            float w2=1.0f-w0-w1;
            if(w0<0||w1<0||w2<0) continue;
            float hyt=w0*q->v[0].y+w1*q->v[1].y+w2*q->v[2].y;
            size_t o=(size_t)y*IW+x;
            if(hyt<=zb[o]) continue;
            zb[o]=hyt; unsigned char*c=img+o*3; c[0]=R;c[1]=G;c[2]=B;
        }
    }
    Vec3 pc=W.drop_c[p], cc=W.cut_c[p], n=W.pmnorm[p];
    circ_m(cc.x,cc.z,W.cut_r[p],            80,200,255,0,0);
    circ_m(cc.x,cc.z,W.cut_r[p]-W.lip_d[p], 30,120,210,0,1);
    circ_m(pc.x,pc.z,W.pocket_r[p],        255, 45, 45,1,0);
    circ_m(pc.x-n.x*W.pocket_r[p], pc.z-n.z*W.pocket_r[p], W.R, 255,255,255,0,0);
    float q0,q1; m2px(pc.x,pc.z,&q0,&q1); dot(q0,q1,3,255,45,45);
    m2px(cc.x,cc.z,&q0,&q1); dot(q0,q1,3,80,200,255);
    FILE *o=fopen(out,"wb"); fprintf(o,"P6\n%d %d\n255\n",IW,IH);
    fwrite(img,1,(size_t)IW*IH*3,o); fclose(o);

    /* the millimetres the page shows beside the sliders — derived, never dialled */
    fprintf(stderr, "{\"mouth\": %.2f, \"drop\": %.2f, \"edge\": %.2f, "
                    "\"thick\": %.2f, \"ball\": %.2f, \"gap_to_drop\": %.2f}\n",
        (double)(jaw_sep(&W,p)*1000.0f), (double)(W.pocket_r[p]*1000.0f),
        (double)(W.cut_r[p]*1000.0f), (double)(W.lip_d[p]*1000.0f),
        (double)(W.R*2000.0f),
        (double)(( (W.cut_r[p] - sqrtf((W.cut_c[p].x-W.drop_c[p].x)*(W.cut_c[p].x-W.drop_c[p].x)
                                     + (W.cut_c[p].z-W.drop_c[p].z)*(W.cut_c[p].z-W.drop_c[p].z)))
                   - W.pocket_r[p]) * 1000.0f));
    return 0;
}
