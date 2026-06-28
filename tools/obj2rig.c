/*
 * obj2rig — a multi-object Wavefront OBJ -> a Mote rigged model header (MoteRig).
 *
 * Each OBJ object/group (`o name` / `g name`) becomes one rig PART; its faces +
 * the verts they use are emitted as an independent <=255-vert Mesh (quantised
 * int8 per part, so parts stay in model space "in place"). A `.rig` sidecar
 * gives each part its PARENT and PIVOT (the joint it rotates about); without one,
 * the first part is the root and the rest parent to it with a centroid pivot.
 *
 *   # tank.rig
 *   part body   parent -1     pivot 0 0 0
 *   part turret parent body   pivot 0 0.30 0.02
 *   part barrel parent turret pivot 0 0.30 0.10
 *
 * Emits `static const MoteRig <name>_rig` (+ its parts/meshes) the game includes;
 * animate it with mote_anim3d.h. Parts must be listed root-first (parents before
 * children) — the editor / a root-first OBJ guarantees this.
 *
 * Usage: obj2rig <name> <in.obj> <out.h> [rig.rig]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "tex_embed.h"      /* PNG -> RGB565 MoteImage (ABI v35 textured meshes) */

#define MAX_V 8192
#define MAX_VT 16384
#define MAX_F 16384
#define MAX_MTL 64
#define MAX_PART 32

typedef struct { float x, y, z; } V3;
typedef struct { float u, v; } V2;
typedef struct { int a, b, c, mtl, part; int ta, tb, tc; } Face;
typedef struct { char name[64]; float r, g, b; char map_Kd[256]; } Mtl;

static V3   verts[MAX_V];
static V2   texc[MAX_VT];
static Face faces[MAX_F];
static Mtl  mtls[MAX_MTL];
static char partname[MAX_PART][64];
static int  nv, nvt, nf, nmtl, npart;
static char g_objdir[400];

/* .rig sidecar: parent name + pivot per part */
static char rig_parent[MAX_PART][64];
static V3   rig_pivot[MAX_PART];
static int  rig_has[MAX_PART];

static int mtl_find(const char *n){ for(int i=0;i<nmtl;i++) if(!strcmp(mtls[i].name,n)) return i; return -1; }
static int part_find(const char *n){ for(int i=0;i<npart;i++) if(!strcmp(partname[i],n)) return i; return -1; }
static void sanitize(char *s){ for(;*s;s++){ char c=*s; if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))) *s='_'; } }

static void load_mtl(const char *objpath, const char *mtlname){
    char path[512]; const char *sl=strrchr(objpath,'/');
    if(sl) snprintf(path,sizeof path,"%.*s/%s",(int)(sl-objpath),objpath,mtlname); else snprintf(path,sizeof path,"%s",mtlname);
    FILE *f=fopen(path,"r"); if(!f) return; char line[256]; Mtl *cur=NULL;
    while(fgets(line,sizeof line,f)){ char name[64],tex[256]; float r,g,b;
        if(sscanf(line,"newmtl %63s",name)==1){ if(nmtl<MAX_MTL){ cur=&mtls[nmtl++]; snprintf(cur->name,sizeof cur->name,"%s",name); cur->r=cur->g=cur->b=0.6f; cur->map_Kd[0]=0; } }
        else if(cur && sscanf(line,"Kd %f %f %f",&r,&g,&b)==3){ cur->r=r; cur->g=g; cur->b=b; }
        else if(cur && sscanf(line,"map_Kd %255[^\r\n]",tex)==1){ char *t=tex; while(*t==' '||*t=='\t')t++; snprintf(cur->map_Kd,sizeof cur->map_Kd,"%s",t); } }
    fclose(f);
}
static void resolve_tex(const char *rel,char *out,size_t outsz){
    if(rel[0]=='/'||g_objdir[0]==0) snprintf(out,outsz,"%s",rel); else snprintf(out,outsz,"%s/%s",g_objdir,rel);
}
/* parse "a","a/t","a//n","a/t/n" -> vertex index in *vi, texcoord in *ti (-1 if none). */
static void parse_ref(const char *t,int *vi,int *ti){ int a=atoi(t); if(a<0)a=nv+1+a; *vi=a-1; *ti=-1;
    const char *s=strchr(t,'/'); if(s&&s[1]&&s[1]!='/'){ int x=atoi(s+1); if(x<0)x=nvt+1+x; *ti=x-1; } }

static int load_obj(const char *path){
    nv=nvt=nf=nmtl=npart=0;
    const char *sl0=strrchr(path,'/'); if(sl0) snprintf(g_objdir,sizeof g_objdir,"%.*s",(int)(sl0-path),path); else g_objdir[0]=0;
    FILE *f=fopen(path,"r"); if(!f){ fprintf(stderr,"error: cannot open %s\n",path); return 0; }
    char line[512]; int cur_mtl=-1, cur_part=-1;
    while(fgets(line,sizeof line,f)){
        if(!strncmp(line,"mtllib ",7)){ char name[256]; if(sscanf(line,"mtllib %255s",name)==1) load_mtl(path,name); }
        else if(!strncmp(line,"usemtl ",7)){ char name[64]; if(sscanf(line,"usemtl %63s",name)==1) cur_mtl=mtl_find(name); }
        else if((line[0]=='o'||line[0]=='g') && line[1]==' '){
            char name[64]; if(sscanf(line+2,"%63s",name)==1){ int p=part_find(name);
                if(p<0 && npart<MAX_PART){ p=npart; snprintf(partname[npart++],64,"%s",name); } cur_part=p; } }
        else if(line[0]=='v' && line[1]=='t'){ if(nvt<MAX_VT){ sscanf(line+3,"%f %f",&texc[nvt].u,&texc[nvt].v); nvt++; } }
        else if(line[0]=='v' && line[1]==' '){ if(nv<MAX_V){ sscanf(line+2,"%f %f %f",&verts[nv].x,&verts[nv].y,&verts[nv].z); nv++; } }
        else if(line[0]=='f' && line[1]==' '){
            if(cur_part<0 && npart<MAX_PART){ cur_part=npart; snprintf(partname[npart++],64,"part0"); }   /* faces before any o/g */
            int idx[16],tdx[16],n=0; char *tok=strtok(line+2," \t\r\n");
            while(tok && n<16){ parse_ref(tok,&idx[n],&tdx[n]); n++; tok=strtok(NULL," \t\r\n"); }
            for(int i=2;i<n;i++){ if(nf<MAX_F){ faces[nf].a=idx[0]; faces[nf].b=idx[i-1]; faces[nf].c=idx[i];
                faces[nf].ta=tdx[0]; faces[nf].tb=tdx[i-1]; faces[nf].tc=tdx[i];
                faces[nf].mtl=cur_mtl; faces[nf].part=cur_part; nf++; } } }
    }
    fclose(f); return 1;
}
static int load_rig(const char *path){
    FILE *f=fopen(path,"r"); if(!f) return 0; char line[256];
    while(fgets(line,sizeof line,f)){ char nm[64],par[64]; V3 pv={0,0,0};
        if(line[0]=='#') continue;
        if(sscanf(line,"part %63s parent %63s pivot %f %f %f",nm,par,&pv.x,&pv.y,&pv.z)>=2){
            int p=part_find(nm); if(p<0) continue; snprintf(rig_parent[p],64,"%s",par); rig_pivot[p]=pv; rig_has[p]=1; } }
    fclose(f); return 1;
}

static V3 sub(V3 a,V3 b){ V3 r={a.x-b.x,a.y-b.y,a.z-b.z}; return r; }
static V3 cross(V3 a,V3 b){ V3 r={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; return r; }
static float dot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float len(V3 a){ return sqrtf(dot(a,a)); }
static uint16_t kd565(int mtl){ float r=0.6f,g=0.6f,b=0.65f; if(mtl>=0){ r=mtls[mtl].r; g=mtls[mtl].g; b=mtls[mtl].b; }
    int ri=(int)(r*255+0.5f),gi=(int)(g*255+0.5f),bi=(int)(b*255+0.5f); if(ri>255)ri=255; if(gi>255)gi=255; if(bi>255)bi=255;
    return (uint16_t)(((ri&0xF8)<<8)|((gi&0xFC)<<3)|(bi>>3)); }

int main(int argc, char **argv){
    if(argc<4){ fprintf(stderr,"usage: %s <name> <in.obj> <out.h> [rig.rig]\n",argv[0]); return 1; }
    const char *name=argv[1], *in=argv[2], *out=argv[3], *rig=argc>4?argv[4]:NULL;
    if(!load_obj(in)) return 1;
    if(nv==0||nf==0||npart==0){ fprintf(stderr,"error: %s has no geometry/objects\n",in); return 1; }
    if(rig) load_rig(rig);

    FILE *h=fopen(out,"w"); if(!h){ perror("output"); return 1; }
    fprintf(h,"/* GENERATED by obj2rig from %s — do not edit. */\n",in);
    fprintf(h,"#ifndef MOTE_RIG_%s_H\n#define MOTE_RIG_%s_H\n#include \"mote_anim3d.h\"\n\n",name,name);

    /* Textured? If ANY material names a map_Kd, embed the FIRST one once and give
     * every part .texture + per-corner UVs (faces without a vt get 0,0). */
    int tex_mtl=-1; for(int i=0;i<nmtl;i++) if(mtls[i].map_Kd[0]){ tex_mtl=i; break; }
    int textured=0, tex_avg=0;
    if(tex_mtl>=0){ char texpath[600]; resolve_tex(mtls[tex_mtl].map_Kd,texpath,sizeof texpath);
        int tw,th; if(tex_embed(h,name,texpath,&tw,&th,&tex_avg)){ textured=1; fprintf(h,"\n"); }
        else fprintf(stderr,"warn: %s map_Kd '%s' failed to load; emitting flat colour\n",name,texpath); }

    int total_tris=0;
    int loc[MAX_V];                       /* global vert -> local index, per part */
    for(int p=0;p<npart;p++){
        char pn[80]; snprintf(pn,sizeof pn,"%.40s_%.30s",name,partname[p]); sanitize(pn);
        for(int i=0;i<nv;i++) loc[i]=-1;
        /* collect this part's verts + faces */
        static int lv[256]; int nlv=0, pf=0;
        for(int i=0;i<nf;i++) if(faces[i].part==p){ pf++;
            int *I[3]={&faces[i].a,&faces[i].b,&faces[i].c};
            for(int k=0;k<3;k++){ int g=*I[k]; if(loc[g]<0){ if(nlv<256) { loc[g]=nlv; lv[nlv++]=g; } else loc[g]=255; } } }
        if(pf==0) continue;
        if(nlv>255){ fprintf(stderr,"error: part '%s' has %d verts (>255) — split it\n",partname[p],nlv); fclose(h); return 1; }
        /* per-part quantisation scale = max |coord| of this part's verts */
        float maxc=1e-6f; for(int i=0;i<nlv;i++){ V3 v=verts[lv[i]];
            float ax=fabsf(v.x),ay=fabsf(v.y),az=fabsf(v.z); if(ax>maxc)maxc=ax; if(ay>maxc)maxc=ay; if(az>maxc)maxc=az; }
        float q=127.0f/maxc, bound_r=0;
        fprintf(h,"static const MeshVert %s_v[%d] = {\n",pn,nlv);
        for(int i=0;i<nlv;i++){ V3 v=verts[lv[i]]; float l=len(v); if(l>bound_r)bound_r=l;
            fprintf(h,"    {%4d,%4d,%4d},%s",(int)lrintf(v.x*q),(int)lrintf(v.y*q),(int)lrintf(v.z*q),(i%4==3||i==nlv-1)?"\n":""); }
        fprintf(h,"};\n");
        /* uniform colour? */
        uint16_t c0=0; int first=1, uniform=1;
        for(int i=0;i<nf;i++) if(faces[i].part==p){ uint16_t c=kd565(faces[i].mtl); if(first){c0=c;first=0;} else if(c!=c0){uniform=0;break;} }
        fprintf(h,"static const MeshFace %s_f[%d] = {\n",pn,pf);
        for(int i=0;i<nf;i++) if(faces[i].part==p){
            V3 a=verts[faces[i].a], b=verts[faces[i].b], cc=verts[faces[i].c];
            V3 nrm=cross(sub(b,a),sub(cc,a)); float l=len(nrm); if(l<1e-9f){ nrm.x=0;nrm.y=0;nrm.z=1;l=1; } nrm.x/=l;nrm.y/=l;nrm.z/=l;
            fprintf(h,"    {%3d,%3d,%3d, %4d,%4d,%4d},\n",loc[faces[i].a],loc[faces[i].b],loc[faces[i].c],
                    (int)lrintf(nrm.x*127),(int)lrintf(nrm.y*127),(int)lrintf(nrm.z*127)); }
        fprintf(h,"};\n");
        if(!uniform){ fprintf(h,"static const uint16_t %s_fc[%d] = {\n",pn,pf);
            for(int i=0;i<nf;i++) if(faces[i].part==p) fprintf(h,"0x%04X,",kd565(faces[i].mtl)); fprintf(h,"\n};\n"); }
        if(textured){   /* per-corner UVs (u0,v0,..), OBJ bottom-left -> engine top-left (v flipped) */
            fprintf(h,"static const uint8_t %s_uv[%d] = {\n",pn,pf*6);
            for(int i=0;i<nf;i++) if(faces[i].part==p){ int ti[3]={faces[i].ta,faces[i].tb,faces[i].tc};
                for(int k=0;k<3;k++){ int ub=0,vb=0; if(ti[k]>=0&&ti[k]<nvt){ float u=texc[ti[k]].u,v=texc[ti[k]].v;
                    if(u<0.0f||u>1.0f)u-=floorf(u); if(v<0.0f||v>1.0f)v-=floorf(v); ub=(int)lrintf(u*255.0f); if(ub<0)ub=0; if(ub>255)ub=255;
                    vb=255-(int)lrintf(v*255.0f); if(vb<0)vb=0; if(vb>255)vb=255; }
                    fprintf(h,"%d,%d,%s",ub,vb,k==2?"\n":""); } }
            fprintf(h,"};\n"); }
        if(textured)    fprintf(h,"static const Mesh %s_m = { %s_v, %s_f, 0, %d, %d, 0x%04X, %.6ff, %.6ff, 0, &%s_tex, %s_uv };\n\n",pn,pn,pn,nlv,pf,tex_avg,maxc,bound_r,name,pn);
        else if(uniform) fprintf(h,"static const Mesh %s_m = { %s_v, %s_f, 0, %d, %d, 0x%04X, %.6ff, %.6ff, 0 };\n\n",pn,pn,pn,nlv,pf,c0,maxc,bound_r);
        else        fprintf(h,"static const Mesh %s_m = { %s_v, %s_f, %s_fc, %d, %d, 0, %.6ff, %.6ff, 0 };\n\n",pn,pn,pn,pn,nlv,pf,maxc,bound_r);
        total_tris += pf;
    }

    /* the parts table (parent + pivot) */
    fprintf(h,"static const MoteRigPart %s_parts[%d] = {\n",name,npart);
    for(int p=0;p<npart;p++){
        char pn[80]; snprintf(pn,sizeof pn,"%.40s_%.30s",name,partname[p]); sanitize(pn);
        int parent=-1; V3 pivot={0,0,0};
        if(rig_has[p]){ pivot=rig_pivot[p]; if(strcmp(rig_parent[p],"-1")!=0){ parent=part_find(rig_parent[p]); } }
        else if(p>0){ parent=0; V3 c={0,0,0}; int n=0;        /* default: parent root, centroid pivot */
            for(int i=0;i<nf;i++) if(faces[i].part==p){ c.x+=verts[faces[i].a].x+verts[faces[i].b].x+verts[faces[i].c].x;
                c.y+=verts[faces[i].a].y+verts[faces[i].b].y+verts[faces[i].c].y; c.z+=verts[faces[i].a].z+verts[faces[i].b].z+verts[faces[i].c].z; n+=3; }
            if(n){ pivot.x=c.x/n; pivot.y=c.y/n; pivot.z=c.z/n; } }
        if(parent>=p) fprintf(stderr,"warn: part '%s' parent is not earlier in the list (need root-first)\n",partname[p]);
        fprintf(h,"    { &%s_m, 1, %d, {%.4ff,%.4ff,%.4ff} },  /* %s */\n",pn,parent,pivot.x,pivot.y,pivot.z,partname[p]);
    }
    fprintf(h,"};\n");
    fprintf(h,"static const MoteRig %s_rig = { %s_parts, %d, %d };\n\n#endif\n",name,name,npart,total_tris);
    fclose(h);
    printf("[obj2rig] %s: %d parts, %d tris -> %s\n",name,npart,total_tris,out);
    return 0;
}
