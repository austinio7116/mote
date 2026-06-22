/*
 * stl2mesh — STL (binary or ASCII) -> a Mote mesh header.
 *
 * A real STL has no shared vertices and far more triangles than the Mote mesh
 * format allows (int8 verts, uint8 face indices => 255 verts per mesh). So this:
 *   1. parses the STL (binary or ASCII),
 *   2. WELDS + DECIMATES by vertex clustering (snap to a grid, average each cell)
 *      — binary-searching the grid resolution to land near a triangle budget,
 *   3. CHUNKS the result into <=255-vertex sub-meshes,
 *   4. quantises to int8, computes per-face normals, and emits a header with a
 *      `<name>_chunks[]` array of Mesh + `<name>_NCHUNKS`.
 *
 * A game renders the model by drawing every chunk at one transform.
 *
 * Usage: stl2mesh <name> <in.stl> <out.h> [tri_budget] [0xRRGGBB]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, z; } V3;
static V3 *RV; static int NRT;            /* raw triangle soup: 3 verts per tri */

static V3  v3sub(V3 a, V3 b){ V3 r={a.x-b.x,a.y-b.y,a.z-b.z}; return r; }
static V3  v3cross(V3 a, V3 b){ V3 r={a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; return r; }
static float v3dot(V3 a, V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float v3len(V3 a){ return sqrtf(v3dot(a,a)); }

/* ---- STL loading ---- */
static int load_stl(const char *path){
    FILE *f = fopen(path,"rb"); if(!f){ perror(path); return 0; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char hdr[84];
    if(sz>=84 && fread(hdr,1,84,f)==84){
        uint32_t cnt; memcpy(&cnt,hdr+80,4);
        if((long)84 + 50L*cnt == sz && cnt>0){            /* binary STL */
            NRT=cnt; RV=malloc((size_t)cnt*3*sizeof(V3));
            for(uint32_t i=0;i<cnt;i++){
                float b[12]; unsigned char attr[2];
                if(fread(b,4,12,f)!=12 || fread(attr,1,2,f)!=2){ fclose(f); return 0; }
                RV[i*3+0]=(V3){b[3],b[4],b[5]}; RV[i*3+1]=(V3){b[6],b[7],b[8]}; RV[i*3+2]=(V3){b[9],b[10],b[11]};
            }
            fclose(f); return 1;
        }
    }
    /* ASCII STL: scan "vertex x y z", 3 per facet */
    fseek(f,0,SEEK_SET);
    int cap=1024, n=0; V3 *vs=malloc(cap*sizeof(V3));
    char line[256];
    while(fgets(line,sizeof line,f)){
        char *p=line; while(*p==' '||*p=='\t') p++;
        if(strncmp(p,"vertex",6)==0){
            V3 v; if(sscanf(p+6," %f %f %f",&v.x,&v.y,&v.z)==3){
                if(n==cap){ cap*=2; vs=realloc(vs,cap*sizeof(V3)); } vs[n++]=v; }
        }
    }
    fclose(f);
    if(n<3 || n%3){ fprintf(stderr,"stl2mesh: %s parsed %d verts (not a multiple of 3)\n",path,n); free(vs); return 0; }
    NRT=n/3; RV=vs; return 1;
}

/* ---- vertex-cluster decimation at grid resolution G (cells on longest axis) ----
 * Outputs a welded, indexed, decimated mesh. Returns triangle count. */
static V3   bbmin, bbmax;
static V3  *OV; static int ONV;                 /* output verts (averaged per cell) */
static int *OT; static int ONF;                 /* output tris (3 indices each) */

#define HSZ (1<<21)
static int     *Hidx;   static int64_t *Hkey;   /* cell-hash -> new vertex index */
static V3      *Hsum;   static int     *Hcnt;

static int cluster(int G){
    float ext = bbmax.x-bbmin.x;
    if(bbmax.y-bbmin.y>ext) ext=bbmax.y-bbmin.y;
    if(bbmax.z-bbmin.z>ext) ext=bbmax.z-bbmin.z;
    if(ext<1e-9f) ext=1.0f;
    float cs = ext/(float)G;                     /* cell size */
    memset(Hidx,0xFF,HSZ*sizeof(int));
    ONV=0;
    int *map = malloc((size_t)NRT*3*sizeof(int));
    for(int i=0;i<NRT*3;i++){
        V3 v=RV[i];
        long ix=(long)floorf((v.x-bbmin.x)/cs), iy=(long)floorf((v.y-bbmin.y)/cs), iz=(long)floorf((v.z-bbmin.z)/cs);
        int64_t key=((int64_t)ix*73856093) ^ ((int64_t)iy*19349663) ^ ((int64_t)iz*83492791);
        uint32_t h=(uint32_t)(key*2654435761u) & (HSZ-1);
        int idx=-1;
        while(Hidx[h]!=-1){ if(Hkey[h]==key){ idx=Hidx[h]; break; } h=(h+1)&(HSZ-1); }
        if(idx<0){ idx=ONV++; Hidx[h]=idx; Hkey[h]=key; Hsum[idx]=(V3){0,0,0}; Hcnt[idx]=0; }
        Hsum[idx].x+=v.x; Hsum[idx].y+=v.y; Hsum[idx].z+=v.z; Hcnt[idx]++;
        map[i]=idx;
    }
    for(int i=0;i<ONV;i++){ OV[i]=(V3){Hsum[i].x/Hcnt[i], Hsum[i].y/Hcnt[i], Hsum[i].z/Hcnt[i]}; }
    ONF=0;
    for(int t=0;t<NRT;t++){
        int a=map[t*3],b=map[t*3+1],c=map[t*3+2];
        if(a==b||b==c||a==c) continue;            /* collapsed -> drop */
        OT[ONF*3]=a; OT[ONF*3+1]=b; OT[ONF*3+2]=c; ONF++;
    }
    free(map);
    return ONF;
}

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage: %s <name> <in.stl> <out.h> [tri_budget] [0xRRGGBB]\n",argv[0]); return 1; }
    const char *name=argv[1], *in=argv[2], *out=argv[3];
    int budget = argc>4 ? atoi(argv[4]) : 1500;
    long rgb = argc>5 ? strtol(argv[5],0,16) : 0xA8AEB8;   /* default steel grey */
    if(!load_stl(in)) return 1;

    bbmin=(V3){1e30f,1e30f,1e30f}; bbmax=(V3){-1e30f,-1e30f,-1e30f};
    V3 centre={0,0,0};
    for(int i=0;i<NRT*3;i++){ V3 v=RV[i];
        if(v.x<bbmin.x)bbmin.x=v.x; if(v.y<bbmin.y)bbmin.y=v.y; if(v.z<bbmin.z)bbmin.z=v.z;
        if(v.x>bbmax.x)bbmax.x=v.x; if(v.y>bbmax.y)bbmax.y=v.y; if(v.z>bbmax.z)bbmax.z=v.z;
        centre.x+=v.x; centre.y+=v.y; centre.z+=v.z; }
    centre.x/=NRT*3; centre.y/=NRT*3; centre.z/=NRT*3;

    OV=malloc((size_t)NRT*3*sizeof(V3)); OT=malloc((size_t)NRT*3*sizeof(int));
    Hidx=malloc(HSZ*sizeof(int)); Hkey=malloc(HSZ*sizeof(int64_t));
    Hsum=malloc((size_t)NRT*3*sizeof(V3)); Hcnt=malloc((size_t)NRT*3*sizeof(int));

    /* binary-search grid resolution so tris land just under the budget */
    int lo=4, hi=512, bestG=lo;
    while(lo<=hi){ int G=(lo+hi)/2; int t=cluster(G);
        if(t<=budget){ bestG=G; lo=G+1; } else hi=G-1; }
    cluster(bestG);
    fprintf(stderr,"[stl2mesh] %s: %d raw tris -> grid %d -> %d verts %d tris\n", name, NRT, bestG, ONV, ONF);

    /* re-centre on the model centroid, quantise by the max extent from centre */
    float maxc=0, bound_r=0;
    for(int i=0;i<ONV;i++){ OV[i].x-=centre.x; OV[i].y-=centre.y; OV[i].z-=centre.z;
        float ax=fabsf(OV[i].x),ay=fabsf(OV[i].y),az=fabsf(OV[i].z);
        if(ax>maxc)maxc=ax; if(ay>maxc)maxc=ay; if(az>maxc)maxc=az;
        float l=v3len(OV[i]); if(l>bound_r)bound_r=l; }
    if(maxc<1e-6f) maxc=1.0f;
    float q=127.0f/maxc;
    uint16_t col=(uint16_t)((((rgb>>16&0xFF)&0xF8)<<8)|(((rgb>>8&0xFF)&0xFC)<<3)|((rgb&0xFF)>>3));

    FILE *h=fopen(out,"w"); if(!h){ perror(out); return 1; }
    fprintf(h,"/* GENERATED by stl2mesh from %s — do not edit. */\n",in);
    fprintf(h,"#ifndef MOTE_MESH_%s_H\n#define MOTE_MESH_%s_H\n#include \"mote_mesh.h\"\n\n",name,name);

    /* greedy chunking: pack tris into <=255-vertex sub-meshes */
    int *local=malloc(ONV*sizeof(int));            /* global vert -> local index in current chunk */
    int *stamp=malloc(ONV*sizeof(int)); for(int i=0;i<ONV;i++) stamp[i]=-1;
    int chunk=0, ti=0, total_v=0, total_f=0;
    int *cv=malloc(256*sizeof(int)), cf=0;
    /* face buffer for the chunk */
    int (*cface)[3]=malloc((size_t)ONF*3*sizeof(int));
    char chunklist[8192]=""; int cl=0;
    while(ti<ONF){
        int nv=0; cf=0;
        int start=ti;
        for(; ti<ONF; ti++){
            int g[3]={OT[ti*3],OT[ti*3+1],OT[ti*3+2]};
            int need=0; for(int k=0;k<3;k++) if(stamp[g[k]]!=chunk) need++;
            if(nv+need>255) break;                  /* this chunk is full */
            int li[3];
            for(int k=0;k<3;k++){ if(stamp[g[k]]!=chunk){ stamp[g[k]]=chunk; local[g[k]]=nv; cv[nv++]=g[k]; } li[k]=local[g[k]]; }
            cface[cf][0]=li[0]; cface[cf][1]=li[1]; cface[cf][2]=li[2]; cf++;
        }
        if(ti==start){ ti++; continue; }            /* safety: skip a lone bad tri */
        /* emit this chunk's verts + faces */
        fprintf(h,"static const MeshVert %s_v%d[%d]={",name,chunk,nv);
        for(int i=0;i<nv;i++){ V3 v=OV[cv[i]];
            fprintf(h,"{%d,%d,%d},",(int)lrintf(v.x*q),(int)lrintf(v.y*q),(int)lrintf(v.z*q)); }
        fprintf(h,"};\n");
        fprintf(h,"static const MeshFace %s_f%d[%d]={\n",name,chunk,cf);
        for(int i=0;i<cf;i++){
            V3 a=OV[cv[cface[i][0]]], b=OV[cv[cface[i][1]]], c=OV[cv[cface[i][2]]];
            V3 n=v3cross(v3sub(b,a),v3sub(c,a)); float l=v3len(n);
            if(l<1e-9f){ n=(V3){0,0,1}; l=1; } n.x/=l; n.y/=l; n.z/=l;
            fprintf(h,"  {%d,%d,%d, %d,%d,%d, 0x%04X},\n",
                cface[i][0],cface[i][1],cface[i][2],
                (int)lrintf(n.x*127),(int)lrintf(n.y*127),(int)lrintf(n.z*127), col);
        }
        fprintf(h,"};\n");
        cl+=snprintf(chunklist+cl,sizeof(chunklist)-cl,
            "  {%s_v%d,%s_f%d,%d,%d,%.6ff,%.6ff,0},\n",name,chunk,name,chunk,nv,cf,maxc,bound_r);
        total_v+=nv; total_f+=cf; chunk++;
    }
    fprintf(h,"static const Mesh %s_chunks[%d]={\n%s};\n",name,chunk,chunklist);
    fprintf(h,"#define %s_NCHUNKS %d\n#define %s_TRIS %d\n",name,chunk,name,total_f);
    fprintf(h,"static const MoteModel %s = { %s_chunks, %s_NCHUNKS, %s_TRIS };  /* draw with mote_model_draw(mote, &%s, pos) */\n\n#endif\n",
            name,name,name,name,name);
    fclose(h);
    printf("[stl2mesh] %s: %d verts %d tris in %d chunks scale=%.3f r=%.3f -> %s\n",
           name,total_v,total_f,chunk,maxc,bound_r,out);
    return 0;
}
