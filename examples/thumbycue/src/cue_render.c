/*
 * ThumbyCue — scene renderer. See cue_render.h.
 */
#include "cue_render.h"
#include "cue_types.h"
#include "r3d_raster.h"
#include "mote_api.h"      /* MoteApi / MoteSphereTex / scene_add_* (engine port) */
#include <math.h>
#include <string.h>
#include <stdint.h>

#define CUE_NEAR    0.05f
#define CUE_DEPTH_K (65535.0f * CUE_NEAR)

/* ---- static table mesh (world space) ---------------------------------- */
typedef struct { Vec3 v[3]; Vec3 nrm; uint16_t color; } CueTri;
#define MAX_TABLE_TRI 2200
#define MAX_STRI      3000     /* near-clipping can split a tri into two */
static CueTri  *s_tab;          /* arena-allocated (Mote) — see cue_render_set_buffers() */
static int      s_ntab;
static int      s_bed_ntab;   /* first s_bed_ntab tris are the flat cloth bed */
static int      s_lip_ntab;   /* s_tab[s_lip_ntab..s_ntab) are the pocket drop lips */
static uint16_t s_cloth, s_bg_top, s_bg_bot;
static uint16_t s_cloth_shadow;  /* dark cloth tint for ball shadow-side bounce */
static float    s_ballR = 0.0286f;
static int      s_is_snooker;   /* ids 1..15 mean reds, not solids/stripes */
static int      s_lip_mode = 1;  /* 0=none 1=tight 2=wide 3=deep (CUE_LIP env) */
static int      s_ball_set = 0;  /* 0 PRO, 1 UK Y/B, 2 UK Y/R, 3 dyna */

/* ---- per-frame projected lists ---------------------------------------- */
typedef struct { float x0,y0,x1,y1,x2,y2; uint16_t d0,d1,d2; uint16_t color; } STri;
static STri *s_stri; static int s_nstri;   /* arena-allocated (Mote) */
static int s_bed_nstri;   /* s_stri[0..s_bed_nstri) are the flat cloth bed */
static int s_lip_nstri;   /* s_stri[s_lip_nstri..s_nstri) are the pocket drop lips */

/* Mote: report the two big buffers' sizes and receive arena pointers for them, so they
 * live in the 280 KB arena rather than the module's 128 KB static RAM. Call before any
 * table build (i.e. before cue_game_init). */
size_t cue_render_tab_bytes(void)  { return (size_t)MAX_TABLE_TRI * sizeof(CueTri); }
size_t cue_render_stri_bytes(void) { return (size_t)MAX_STRI * sizeof(STri); }
void   cue_render_set_buffers(void *tab, void *stri) { s_tab = (CueTri *)tab; s_stri = (STri *)stri; }

typedef struct { float cx, cy, rad, viewz; Mat3 orient; uint8_t id; } Sprite;
static Sprite s_spr[CUE_MAX_BALLS]; static int s_nspr;
/* ground-plane shadow decal: centre + two screen-space axis vectors (the
 * projection of world +X and +Z offsets), so it foreshortens with the cloth */
static struct { float cx, cy, ux, uy, vx, vy; } s_shadow[CUE_MAX_BALLS];
static int s_nshadow;

#define MAX_DOTS 48
static struct { float x, y; uint16_t d; } s_dot[MAX_DOTS]; static int s_ndot;
static struct { float x, y; uint16_t d; } s_odot[MAX_DOTS]; static int s_nodot;
static struct { float tx,ty,bx,by; uint16_t color; int on; } s_cue;
static float s_cue_side, s_cue_vert, s_cue_elev;   /* tip offset + elevation for the stick */
static struct { float cx, cy, rad; uint16_t d; int on; } s_ghost;

/* View globals used by the per-pixel pass. */
static CueView s_view;
static float   s_focal;
static Vec3    s_light = { 0.10f, 0.975f, 0.20f };  /* nearly overhead (snooker lamps) */

/* ---- helpers ----------------------------------------------------------- */
static inline uint16_t shade565(uint16_t c, float s) {
    if (s < 0) s = 0;
    if (s > 1.999f) s = 1.999f;
    int r = (int)(((c >> 11) & 0x1F) * s); if (r > 31) r = 31;
    int g = (int)(((c >> 5) & 0x3F) * s);  if (g > 63) g = 63;
    int b = (int)((c & 0x1F) * s);         if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline uint16_t add565(uint16_t c, int ar, int ag, int ab) {
    int r = ((c >> 11) & 0x1F) + ar; if (r > 31) r = 31;
    int g = ((c >> 5) & 0x3F) + ag;  if (g > 63) g = 63;
    int b = (c & 0x1F) + ab;         if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
/* Blend a→b by t in [0,1]. */
static inline uint16_t mix565(uint16_t a, uint16_t b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a>>11)&0x1F, ag = (a>>5)&0x3F, ab = a&0x1F;
    int br = (b>>11)&0x1F, bg = (b>>5)&0x3F, bb = b&0x1F;
    int rr = ar + (int)((br-ar)*t), gg = ag + (int)((bg-ag)*t), bl = ab + (int)((bb-ab)*t);
    return (uint16_t)((rr<<11)|(gg<<5)|bl);
}

/* Ball lighting style (0=smooth/current, 1=hard spec, 2=toon, 3=gloss). */
static int s_light_mode = 1;
void cue_render_set_light_mode(int m) { s_light_mode = m; }
void cue_render_set_cue_tip(float side, float vert, float elev) {
    s_cue_side = side; s_cue_vert = vert; s_cue_elev = elev;
}

int cue_render_project(Vec3 world, float *sx, float *sy, uint16_t *d) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    if (d) {
        float dd = CUE_DEPTH_K * inv;
        *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
    }
    return 1;
}
/* project + return view-space z (for sphere radius / per-pixel depth). */
static int project_z(Vec3 world, float *sx, float *sy, float *vz) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    *vz = vv.z;
    return 1;
}

/* ---- table mesh build -------------------------------------------------- */
static void tri(Vec3 a, Vec3 b, Vec3 c, uint16_t col) {
    if (s_ntab >= MAX_TABLE_TRI) return;
    CueTri *t = &s_tab[s_ntab++];
    t->v[0] = a; t->v[1] = b; t->v[2] = c;
    t->nrm = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    t->color = col;
}
static void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t col) {
    tri(a, b, c, col); tri(a, c, d, col);
}
/* Ribbon quad a→b→c→d with a CHOSEN diagonal. The cushion strip is non-planar
 * (back verts use per-node normals), so the diagonal must follow the geometry,
 * not the vertex labels — otherwise a jaw renders mirror-broken on one side.
 * alt=0 splits a-c; alt=1 splits b-d. */
static void ribbon(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t col) {
    /* Split along the SHORTER diagonal — a pure distance test, so mirrored
     * geometry triangulates identically (the jaw was broken on one side
     * because a fixed-label diagonal is not mirror-invariant). */
    if (v3_len2(v3_sub(b, d)) < v3_len2(v3_sub(a, c))) {
        tri(a, b, d, col); tri(b, c, d, col);
    } else {
        tri(a, b, c, col); tri(a, c, d, col);
    }
}

/* Push a felt-boundary point OUT to the cushion back, but ONLY near the CORNER
 * pockets (faded to zero elsewhere) — the corner pocket is set back so its felt
 * needs to run under the cushions to frame it; the middle pockets are already
 * right and must be left untouched. */
static Vec3 jaw_pushed(const CueWorld *w, float pr_corner, float cw, Vec3 P) {
    float dmin = 1e9f;
    for (int p = 0; p < 4 && p < w->npocket; p++) {
        float dx = P.x - w->pocket[p].x, dz = P.z - w->pocket[p].z, d = sqrtf(dx*dx + dz*dz);
        if (d < dmin) dmin = d;
    }
    float thresh = 4.0f * pr_corner;
    float f = (dmin >= thresh) ? 0.0f : (1.0f - dmin/thresh);
    float push = cw * 1.7f * f;                 /* run the corner felt well under the frame */
    float l = sqrtf(P.x*P.x + P.z*P.z) + 1e-6f;
    return v3(P.x + P.x/l*push, 0, P.z + P.z/l*push);
}

/* The pocket-mouth edge as a true CIRCLE arc centred on the FUNCTIONAL drop
 * (`pc`, which moves with the pocket setback), radius `r`, swept the short way
 * between the two jaw tips a→b (so it faces the table). The endpoints are pinned
 * to the jaw tips so the felt still meets the cushion noses. Both the bed cut and
 * the lip use this, so they share one curve anchored to the drop — not the jaws. */
static void pocket_circ_arc(Vec3 pc, float r, Vec3 a, Vec3 b, Vec3 *arc, int N) {
    float a0 = atan2f(a.z - pc.z, a.x - pc.x);
    float a1 = atan2f(b.z - pc.z, b.x - pc.x);
    while (a1 - a0 >  3.14159265f) a1 -= 6.2831853f;
    while (a1 - a0 < -3.14159265f) a1 += 6.2831853f;
    for (int k = 0; k <= N; k++) {
        if (k == 0)      { arc[k] = v3(a.x, 0, a.z); continue; }
        if (k == N)      { arc[k] = v3(b.x, 0, b.z); continue; }
        float th = a0 + (a1 - a0) * (float)k / N;
        arc[k] = v3(pc.x + r*cosf(th), 0, pc.z + r*sinf(th));
    }
}

/* Baize lip (the drop): rolls the cloth down into each pocket throat. Emitted
 * AFTER the pocket voids so depth-test layers it OVER the void (no rim cutting
 * across it) while the raised cushions still occlude its sides. */
static void emit_pocket_lips(const CueTable *t, const CueWorld *w) {
    if (!s_lip_mode) return;
    int nb = w->njaw;
    for (int i = 0; i < nb; i++) {
        if (!(i & 1)) continue;                 /* only pocket-mouth edges */
        /* same corner-only push as the bed so the lip's outer edge meets the
         * felt and covers the new corner baize */
        float cw = t->rail_w * 0.63f;
        Vec3 a = jaw_pushed(w, t->pr_corner, cw, w->jaw[i]);
        Vec3 b = jaw_pushed(w, t->pr_corner, cw, w->jaw[(i + 1) % nb]);
        Vec3 m = v3((a.x + b.x) * 0.5f, 0, (a.z + b.z) * 0.5f);
        int pidx = 0; float bestp = 1e9f;
        for (int q = 0; q < w->npocket; q++) {
            float dx = w->pocket[q].x - m.x, dz = w->pocket[q].z - m.z;
            float dd = dx*dx + dz*dz;
            if (dd < bestp) { bestp = dd; pidx = q; }
        }
        Vec3 pc = w->pocket[pidx];
        float pr = (pidx < 4) ? t->pr_corner : t->pr_side;
        float fd = w->pocket_r[pidx];            /* the FUNCTIONAL drop circle (= void) */
        /* the bed-edge circle around the drop (matches the bed cut) — the lip
         * rolls from here down to the drop fd */
        const int N = 6;
        Vec3 arc[N + 1];
        pocket_circ_arc(pc, fd * 1.35f, a, b, arc, N);
        int M; float ld;
        switch (s_lip_mode) {
            case 2:  M = 6; ld = 0.55f*pr; break;
            case 3:  M = 7; ld = 0.80f*pr; break;
            default: M = 5; ld = 0.45f*pr; break;
        }
        /* The bed already runs in close to the drop (mouth_cloth_ctrl), so the lip
         * just rolls smoothly from that edge down to the functional drop circle —
         * a short, multi-ring (smooth) curved roll that ends ON the red line. */
        Vec3 ring0[N + 1]; for (int k = 0; k <= N; k++) ring0[k] = arc[k];
        for (int s = 1; s <= M; s++) {
            float phi = (float)s / M * 1.5707963f;
            float tn = sinf(phi), yy = -ld * (1.0f - cosf(phi));
            uint16_t col = shade565(t->cloth, 1.0f - 0.92f*(1.0f - cosf(phi)));
            Vec3 ring1[N + 1];
            for (int k = 0; k <= N; k++) {
                float dx = arc[k].x - pc.x, dz = arc[k].z - pc.z;
                float Rk = sqrtf(dx*dx + dz*dz) + 1e-6f;
                float r_full = Rk + (fd - Rk) * tn;     /* arc edge → drop */
                float taper = sinf(3.14159265f * (float)k / N) * 2.2f;
                if (taper > 1.0f) taper = 1.0f;
                float r = Rk + (r_full - Rk) * taper;
                ring1[k] = v3(pc.x + dx/Rk*r, yy*taper, pc.z + dz/Rk*r);
            }
            for (int k = 0; k < N; k++)
                quad(ring0[k], ring0[k+1], ring1[k+1], ring1[k], col);
            for (int k = 0; k <= N; k++) ring0[k] = ring1[k];
        }
    }
}

/* Fill the wood ring around one pocket bore inside the notch box [x0,x1]×[z0,z1].
 *
 * The plank's wood lies entirely on ONE side of the pocket (the rail side); the
 * mouth opens to the bed on the opposite side. A radial (centre-based) fill
 * leaves slivers because the pocket centre sits OUTSIDE the box (behind the
 * cushion). Instead we fill in fine columns ALONG the rail — each column a
 * trapezoid whose inner edge follows the analytic circle (smooth, gap-free) and
 * a vertical wall dropping to `ybot`.
 *
 *   axis    : 0 = wood spans Z (long rails, columns run along X)
 *             1 = wood spans X (short rails, columns run along Z)
 *   rail_hi : 1 = wood is toward the LARGER coord (mouth at the smaller box edge)
 *             0 = wood toward the smaller coord (mouth at the larger box edge) */
static void bore_fill(float cx, float cz, float r, float x0, float x1, float z0, float z1,
                      float ytop, float ybot, uint16_t top, uint16_t wall,
                      int axis, int rail_hi) {
    const int N = 16;
    for (int k = 0; k < N; k++) {
        if (axis == 0) {                       /* columns along X, depth along Z */
            float u0 = x0 + (x1-x0)*k/N, u1 = x0 + (x1-x0)*(k+1)/N;
            float d0 = r*r-(u0-cx)*(u0-cx), d1 = r*r-(u1-cx)*(u1-cx);
            d0 = d0 > 0 ? sqrtf(d0) : 0; d1 = d1 > 0 ? sqrtf(d1) : 0;
            float zt0, zt1, wa, wb;            /* rim z, wood far edge */
            if (rail_hi) { zt0 = cz+d0; zt1 = cz+d1; wa = wb = z1; }   /* wood toward +z */
            else         { zt0 = cz-d0; zt1 = cz-d1; wa = wb = z0; }   /* wood toward -z */
            if (zt0 < z0) zt0 = z0; if (zt0 > z1) zt0 = z1;
            if (zt1 < z0) zt1 = z0; if (zt1 > z1) zt1 = z1;
            quad(v3(u0,ytop,zt0), v3(u1,ytop,zt1), v3(u1,ytop,wb), v3(u0,ytop,wa), top);
            quad(v3(u0,ytop,zt0), v3(u1,ytop,zt1), v3(u1,ybot,zt1), v3(u0,ybot,zt0), wall);
        } else {                               /* columns along Z, depth along X */
            float u0 = z0 + (z1-z0)*k/N, u1 = z0 + (z1-z0)*(k+1)/N;
            float d0 = r*r-(u0-cz)*(u0-cz), d1 = r*r-(u1-cz)*(u1-cz);
            d0 = d0 > 0 ? sqrtf(d0) : 0; d1 = d1 > 0 ? sqrtf(d1) : 0;
            float xt0, xt1, wa, wb;
            if (rail_hi) { xt0 = cx+d0; xt1 = cx+d1; wa = wb = x1; }   /* wood toward +x */
            else         { xt0 = cx-d0; xt1 = cx-d1; wa = wb = x0; }   /* wood toward -x */
            if (xt0 < x0) xt0 = x0; if (xt0 > x1) xt0 = x1;
            if (xt1 < x0) xt1 = x0; if (xt1 > x1) xt1 = x1;
            quad(v3(xt0,ytop,u0), v3(xt1,ytop,u1), v3(wb,ytop,u1), v3(wa,ytop,u0), top);
            quad(v3(xt0,ytop,u0), v3(xt1,ytop,u1), v3(xt1,ybot,u1), v3(xt0,ybot,u0), wall);
        }
    }
}

/* A wood rail plank [xa,xb]×[za,zb] with a clean round bore at each pocket: cut a
 * rectangular notch (the pocket's clipped bounding box) from the plank top, then
 * fill it with bore_fill so the visible cut edge is the smooth circle curve. */
static void wood_plank_bored(float xa, float xb, float za, float zb,
                             float ytop, float ybot, uint16_t top, uint16_t wall,
                             const float *hx, const float *hz, const float *hr, int nh,
                             int axis, int rail_hi, float ylow, uint16_t lip) {
    /* notches (clipped pocket bounding boxes) on this plank — note TWO pockets
     * can share the same x-range (the two corners of a short rail). */
    float nx0[CUE_MAX_POCKET], nx1[CUE_MAX_POCKET], nz0[CUE_MAX_POCKET], nz1[CUE_MAX_POCKET];
    int   pid[CUE_MAX_POCKET]; int ni = 0;
    for (int h = 0; h < nh; h++) {
        if (hz[h]+hr[h] <= za || hz[h]-hr[h] >= zb) continue;
        float a = hx[h]-hr[h], b = hx[h]+hr[h];
        if (a < xa) a = xa; if (b > xb) b = xb;
        if (b <= a + 1e-5f) continue;
        float c = hz[h]-hr[h], d = hz[h]+hr[h];
        if (c < za) c = za; if (d > zb) d = zb;
        nx0[ni]=a; nx1[ni]=b; nz0[ni]=c; nz1[ni]=d; pid[ni]=h; ni++;
    }
    /* wood top = plank minus the notch rectangles, split into x-columns at every
     * notch edge (so overlapping-x notches are both subtracted). */
    float ex[2*CUE_MAX_POCKET + 2]; int ne = 0;
    ex[ne++] = xa; ex[ne++] = xb;
    for (int i = 0; i < ni; i++) { ex[ne++] = nx0[i]; ex[ne++] = nx1[i]; }
    for (int i = 1; i < ne; i++) {                              /* sort x-edges */
        float e = ex[i]; int j = i-1;
        while (j >= 0 && ex[j] > e) { ex[j+1] = ex[j]; j--; }
        ex[j+1] = e;
    }
    for (int c = 0; c < ne-1; c++) {
        float cx0 = ex[c], cx1 = ex[c+1];
        if (cx1 <= cx0 + 1e-5f) continue;
        float mx = 0.5f*(cx0+cx1);
        float lo[8], hi[8]; int ns = 1; lo[0] = za; hi[0] = zb;
        for (int i = 0; i < ni; i++) {                          /* subtract active notches */
            if (mx < nx0[i]-1e-5f || mx > nx1[i]+1e-5f) continue;
            float clo = nz0[i], chi = nz1[i];
            float nlo[8], nhi[8]; int nn = 0;
            for (int s = 0; s < ns && nn < 7; s++) {
                if (chi <= lo[s] || clo >= hi[s]) { nlo[nn]=lo[s]; nhi[nn]=hi[s]; nn++; continue; }
                if (clo > lo[s]) { nlo[nn]=lo[s]; nhi[nn]=clo; nn++; }
                if (chi < hi[s] && nn < 8) { nlo[nn]=chi; nhi[nn]=hi[s]; nn++; }
            }
            ns = nn; for (int s = 0; s < ns; s++) { lo[s]=nlo[s]; hi[s]=nhi[s]; }
        }
        int inner_col = (axis == 1) && (rail_hi ? (cx0 <= xa+1e-4f) : (cx1 >= xb-1e-4f));
        float ix = rail_hi ? xa : xb;          /* x-plank inner (mouth) edge */
        for (int s = 0; s < ns; s++) {
            if (hi[s]-lo[s] <= 1e-4f) continue;
            quad(v3(cx0,ytop,lo[s]), v3(cx1,ytop,lo[s]), v3(cx1,ytop,hi[s]), v3(cx0,ytop,hi[s]), top);
            /* inner-edge riser — only where wood actually reaches the mouth edge,
             * so pocket mouths stay open (no wood line across the side pockets). */
            if (ylow < ytop) {
                if (axis == 0 && rail_hi && lo[s] <= za+1e-4f)
                    quad(v3(cx0,ytop,za), v3(cx1,ytop,za), v3(cx1,ylow,za), v3(cx0,ylow,za), lip);
                else if (axis == 0 && !rail_hi && hi[s] >= zb-1e-4f)
                    quad(v3(cx0,ytop,zb), v3(cx1,ytop,zb), v3(cx1,ylow,zb), v3(cx0,ylow,zb), lip);
                else if (inner_col)
                    quad(v3(ix,ytop,lo[s]), v3(ix,ytop,hi[s]), v3(ix,ylow,hi[s]), v3(ix,ylow,lo[s]), lip);
            }
        }
    }
    for (int i = 0; i < ni; i++)                                /* bore EACH pocket */
        bore_fill(hx[pid[i]], hz[pid[i]], hr[pid[i]], nx0[i], nx1[i], nz0[i], nz1[i],
                  ytop, ybot, top, wall, axis, rail_hi);
}

/* ---- cloth markings (baulk line / D / spots) -------------------------- */
#define MARK_Y 0.0015f      /* a hair above the cloth so markings sit on top */
static void cloth_line(float ax, float az, float bx, float bz, float w, uint16_t col) {
    float dx = bx-ax, dz = bz-az, l = sqrtf(dx*dx+dz*dz);
    if (l < 1e-6f) return;
    float px = -dz/l*w*0.5f, pz = dx/l*w*0.5f;
    quad(v3(ax+px,MARK_Y,az+pz), v3(bx+px,MARK_Y,bz+pz),
         v3(bx-px,MARK_Y,bz-pz), v3(ax-px,MARK_Y,az-pz), col);
}
static void cloth_disc(float cx, float cz, float r, uint16_t col) {
    const int N = 8; Vec3 c = v3(cx, MARK_Y, cz);
    for (int k = 0; k < N; k++) {
        float a0 = k*(6.2831853f/N), a1 = (k+1)*(6.2831853f/N);
        tri(c, v3(cx+r*cosf(a0),MARK_Y,cz+r*sinf(a0)),
               v3(cx+r*cosf(a1),MARK_Y,cz+r*sinf(a1)), col);
    }
}
static void cloth_arc(float cx, float cz, float r, float a0, float a1, float w, uint16_t col) {
    const int N = 14;
    for (int k = 0; k < N; k++) {
        float t0 = a0 + (a1-a0)*k/N, t1 = a0 + (a1-a0)*(k+1)/N;
        cloth_line(cx+r*cosf(t0), cz+r*sinf(t0), cx+r*cosf(t1), cz+r*sinf(t1), w, col);
    }
}
/* Baulk line + D (snooker & UK8), the six colour spots (snooker), or the foot
 * spot (US pool). Drawn in the bed layer so balls/cushions/shadows occlude them. */
static void emit_table_markings(const CueTable *t) {
    uint16_t lc = shade565(t->cloth, 1.65f);     /* lighter cloth line */
    uint16_t sc = RGB565C(220, 220, 205);        /* spot — off-white */
    float hw = t->half_wid, hl = t->half_len, R = t->R;
    float lw = R * 0.22f, sr = R * 0.42f;
    if (t->is_snooker || t->kind == CUE_GAME_UK8) {
        float bx = t->baulk_x, dr = t->d_radius;
        cloth_line(bx, -(hw-R*0.5f), bx, hw-R*0.5f, lw, lc);        /* baulk line */
        cloth_arc(bx, 0.0f, dr, 1.5707963f, 4.7123890f, lw, lc);   /* the D (bulges to baulk) */
    }
    if (t->is_snooker) {
        cloth_disc(t->baulk_x,  t->d_radius, sr, sc);   /* yellow */
        cloth_disc(t->baulk_x, -t->d_radius, sr, sc);   /* green  */
        cloth_disc(t->baulk_x,  0.0f,        sr, sc);   /* brown  */
        cloth_disc(t->blue_x,   0.0f,        sr, sc);   /* blue   */
        cloth_disc(t->pink_x,   0.0f,        sr, sc);   /* pink   */
        cloth_disc(t->black_x,  0.0f,        sr, sc);   /* black  */
    } else {
        cloth_disc(hl * 0.5f, 0.0f, sr, sc);            /* foot spot (rack apex) */
        /* US-style tables (US 8/9-ball, Chinese 8-ball) break from behind the
         * head string ("kitchen line") — a line across the bed at -hl/2 with a
         * head spot. UK8 uses the baulk line + D drawn above instead. */
        if (t->kind == CUE_GAME_US8 || t->kind == CUE_GAME_US9 ||
            t->kind == CUE_GAME_CN8) {
            float hx = -hl * 0.5f;
            cloth_line(hx, -(hw-R*0.5f), hx, hw-R*0.5f, lw, lc);   /* head string */
            cloth_disc(hx, 0.0f, sr, sc);                          /* head spot */
        }
    }
}

void cue_render_build_table(const CueTable *t, const CueWorld *w) {
    /* Lip roll is scaled to each pocket's mouth radius (pr), so mode 1 already
     * gives a proportionate cloth fall on every table — the snooker drop only
     * looked hard earlier because the lips were being dropped (buffer overflow),
     * not because the roll was too shallow. */
    s_lip_mode = 1;
    { extern char *getenv(const char*); const char *e = getenv("CUE_LIP"); if (e) s_lip_mode = e[0]-'0'; }
    { extern char *getenv(const char*); const char *e2 = getenv("CUE_BALLSET"); if (e2) s_ball_set = e2[0]-'0'; }
    s_ntab = 0;
    s_cloth = t->cloth;
    s_ballR = t->R;
    s_is_snooker = t->is_snooker;
    s_cloth_shadow = shade565(t->cloth, 0.42f);   /* cloth bounce tint */
    s_bg_top = RGB565C(24, 26, 36);
    s_bg_bot = RGB565C(6, 7, 12);
    const float hl = t->half_len, hw = t->half_wid;
    const float rw = t->rail_w;
    const float cw = rw * 0.63f;        /* cushion depth (nose → cushion back); +50% for a beefier rail */
    const float nose_h = t->cushion_h;       /* nose contact line (bottom of the front face) */
    const float flat_h = nose_h * 1.30f;     /* top of the small VERTICAL nose front face */
    const float rail_h = flat_h;             /* flat cushion top & wood top, level at flat_h */
    uint16_t wood = t->rail, woodt = t->rail_top;

    /* Cloth bed — fanned from the centre over the knuckle boundary (w->jaw is
     * stored in boundary order), so the felt edge follows the cushion noses
     * and the pocket MOUTHS are real gaps (the angled jaws stay visible). */
    int nb = w->njaw;
    for (int i = 0; i < nb; i++) {
        /* Run the felt boundary out under the cushions near the corner pockets
         * (jaw_pushed fades to zero elsewhere) so the corners get felt framing them. */
        Vec3 a = jaw_pushed(w, t->pr_corner, cw, w->jaw[i]);
        Vec3 b = jaw_pushed(w, t->pr_corner, cw, w->jaw[(i + 1) % nb]);
        /* Edges within a chain (i even) are the straight nose; edges ACROSS a
         * pocket (i odd) are the mouth — cut it as a CURVED arc bulging toward
         * the table so the pocket drop is rounded, not a straight chord. */
        if (i & 1) {
            /* Mouth edge: the felt is cut on a CIRCLE around the FUNCTIONAL drop
             * (so it accounts for the pocket setback, not the jaw position) — the
             * bed runs in close to the drop and the lip finishes from there. */
            Vec3 m = v3((a.x + b.x) * 0.5f, 0, (a.z + b.z) * 0.5f);
            int pidx = 0; float best = 1e9f;
            for (int q = 0; q < w->npocket; q++) {
                float dx = w->pocket[q].x - m.x, dz = w->pocket[q].z - m.z, dd = dx*dx + dz*dz;
                if (dd < best) { best = dd; pidx = q; }
            }
            const int N = 6;
            Vec3 arc[N + 1];
            pocket_circ_arc(w->pocket[pidx], w->pocket_r[pidx] * 1.35f, a, b, arc, N);
            for (int k = 1; k <= N; k++)
                tri(v3(0, 0, 0), arc[k-1], arc[k], t->cloth);
            /* the baize lip (drop) is emitted AFTER the pocket voids — see
             * emit_pocket_lips() below — so the void can't draw its rim across it */
        } else {
            tri(v3(0, 0, 0), a, b, t->cloth);
        }
    }
    emit_table_markings(t);   /* baulk line / D / spots — part of the bed layer */
    s_bed_ntab = s_ntab;   /* everything after here is raised (cushions/frame/voids) */

    /* Cushions from the chain segments: steep cloth playing face up to the
     * nose, then a cloth top sloping back to the cushion back. The facings
     * (which splay outward) shape the jaws automatically. */
    /* Cushion cross-section (K66-ish): from the bed it leans FORWARD up to the
     * protruding nose (the contact line at ~nose_h), a small vertical flat just
     * above the nose, then the cloth top slopes back to the rail. The base is
     * set back from the nose by `ub` so the nose overhangs (the "cut in below"). */
    uint16_t fdark = shade565(t->cloth, 0.55f);   /* undercut face (in shadow) */
    uint16_t face  = shade565(t->cloth, 0.72f);   /* the vertical nose front face */
    uint16_t ctop  = shade565(t->cloth, 0.92f);   /* cloth top to the rail */
    const float ub = 0.45f * t->R;                /* undercut / overhang */
    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *sg = &w->seg[s];
        /* Per-NODE back normal: average with the neighbouring segment when they
         * share an endpoint, so adjacent cushion tops share their back vertices
         * — a continuous strip with no V-gaps (the "holes in the top"). */
        Vec3 pa = sg->a, pb = sg->b, na = sg->n, nb = sg->n;
        int sharedA = 0, sharedB = 0;
        if (s > 0) {
            const CueSeg *pr = &w->seg[s-1];
            if (v3_len2(v3_sub(pr->b, sg->a)) < 1e-8f) { na = v3_norm(v3_add(sg->n, pr->n)); sharedA = 1; }
        }
        if (s < w->nseg - 1) {
            const CueSeg *nx = &w->seg[s+1];
            if (v3_len2(v3_sub(sg->b, nx->a)) < 1e-8f) { nb = v3_norm(v3_add(sg->n, nx->n)); sharedB = 1; }
        }
        /* Pocket facing: extend the free-tip NOSE along its own (mitre/tangent)
         * direction — CONTINUING THE SAME ANGLE — to STOP exactly at the frame
         * line (the wood inner edge ±(hw|hl)+cw), NOT past it. Overshooting
         * tucked the cushion under the raised wood and z-fought on device. */
        if (sg->kind == 1 && (!sharedA || !sharedB)) {
            int afree = !sharedA;
            Vec3 kn = afree ? sg->b : sg->a;     /* shared knuckle (toward the rail) */
            Vec3 tp = afree ? sg->a : sg->b;     /* free tip (at the pocket mouth) */
            Vec3 M = v3_norm(v3_sub(tp, kn));    /* the facing's own direction */
            float t = 0.0f;
            if (hw - fabsf(kn.z) < hl - fabsf(kn.x)) {   /* knuckle on a z-rail */
                float target = (kn.z > 0 ? hw + cw : -(hw + cw));
                if (fabsf(M.z) > 1e-4f) t = (target - tp.z) / M.z;
            } else {                                     /* knuckle on an x-rail */
                float target = (kn.x > 0 ? hl + cw : -(hl + cw));
                if (fabsf(M.x) > 1e-4f) t = (target - tp.x) / M.x;
            }
            if (t > 0.0f) { Vec3 e = v3_add(tp, v3_scale(M, t)); if (afree) pa = e; else pb = e; }
        }
        float uba = sharedA ? ub : 0.0f, ubb = sharedB ? ub : 0.0f;
        /* Back-vertex depth. Shared ends reach the full depth cw; a FREE tip
         * collapses to 0 because the nose was already extended along its tangent
         * to the rail plane above — the facing continues at the same angle and
         * comes to a clean point there (US mitre and curved jaws alike). */
        float cwa = sharedA ? cw : 0.0f;
        float cwb = sharedB ? cw : 0.0f;
        Vec3 ba = v3(pa.x - na.x*uba, 0, pa.z - na.z*uba);
        Vec3 bb = v3(pb.x - nb.x*ubb, 0, pb.z - nb.z*ubb);
        Vec3 an = v3(pa.x, nose_h, pa.z), bn = v3(pb.x, nose_h, pb.z);
        Vec3 af = v3(pa.x, flat_h, pa.z), bf = v3(pb.x, flat_h, pb.z);
        /* straight rail nose (kind 0): clean perpendicular back at depth cw (a
         * straight edge at ±(hw|hl)+cw) so the wood inner edge can touch it
         * exactly. Facings keep the averaged normal for top continuity. */
        Vec3 bka = (sg->kind == 0) ? sg->n : na;
        Vec3 bkb = (sg->kind == 0) ? sg->n : nb;
        Vec3 ar = v3(pa.x - bka.x*cwa, rail_h, pa.z - bka.z*cwa);
        Vec3 br = v3(pb.x - bkb.x*cwb, rail_h, pb.z - bkb.z*cwb);
        ribbon(ba, bb, bn, an, fdark);      /* undercut face (leans to nose) */
        quad(an, bn, bf, af, face);            /* small flat (planar) */
        ribbon(af, bf, br, ar, ctop);       /* cloth top → rail */
    }

    /* Wood rail frame: full rectangular ring (the pocket caps punch holes
     * through it, flush with the rail). */
    const float fw = rw + 0.055f;       /* wider wood frame to balance the deeper cushions */
    const float ox = hl + fw, oz = hw + fw;
    /* Wood inner edge WIDENED inward to meet the cushion back: the averaged
     * corner-normal pulls the cushion back to ~0.82·cw, so set the wood inset a
     * touch inside that (0.78·cw) — the wood reaches the cushion (no gap), the
     * cushion (drawn after, at rail_h) cleanly covers the tiny overlap. */
    const float ibx = hl + cw, ibz = hw + cw;   /* wood inner edge EXACTLY at the cushion back */
    /* Raise the wood top a hair above the flat cushion so the frame OVERLAPS and
     * hides the cushion back that now tucks under it (jaw_back runs the cushion
     * past the inner edge). A short inner riser closes the step down to rail_h. */
    const float frame_lift = 0.085f * t->R;
    const float plank_y = rail_h + frame_lift;
    float hx[CUE_MAX_POCKET], hz[CUE_MAX_POCKET], hr[CUE_MAX_POCKET];
    for (int p = 0; p < w->npocket; p++) {
        hx[p] = w->pocket[p].x; hz[p] = w->pocket[p].z;
        hr[p] = (p < 4) ? t->pr_corner : t->pr_side;
    }
    int nh = w->npocket;
    uint16_t wbore = shade565(woodt, 0.42f);   /* internal bore wall (in shadow) */
    const float bore_bot = -0.002f;            /* bore wall reaches the bed; throat continues below */
    /* Inner-edge risers (the short wood lip dropping from the raised plank top to
     * rail_h along the mouth edge) are drawn INSIDE wood_plank_bored per wood
     * column, so they skip the pocket mouths (no wood line across the side
     * pockets). rail_h is passed as the riser bottom. */
    uint16_t wlip = shade565(woodt, 0.80f);
    wood_plank_bored(-ox, ox,  ibz,  oz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 1, rail_h, wlip); /* +z */
    wood_plank_bored(-ox, ox, -oz, -ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 0, rail_h, wlip); /* -z */
    wood_plank_bored(ibx, ox, -ibz, ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, rail_h, wlip); /* +x */
    wood_plank_bored(-ox,-ibx,-ibz, ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 0, rail_h, wlip); /* -x */
    quad(v3(-ox,plank_y,oz), v3(ox,plank_y,oz), v3(ox,0,oz), v3(-ox,0,oz), wood);
    quad(v3(ox,plank_y,-oz), v3(-ox,plank_y,-oz), v3(-ox,0,-oz), v3(ox,0,-oz), wood);
    quad(v3(ox,plank_y,oz), v3(ox,plank_y,-oz), v3(ox,0,-oz), v3(ox,0,oz), wood);
    quad(v3(-ox,plank_y,-oz), v3(-ox,plank_y,oz), v3(-ox,0,oz), v3(-ox,0,-oz), wood);

    /* Pockets = circular VOIDS you look down into. The bed is already cut at
     * the mouth, so a downward cone gives the recess. The OUTWARD half of each
     * pocket (the half sitting over the wood frame) gets a flush rail-level cap
     * + a frame-thickness wall to punch the hole through the wood; the inward
     * (mouth) half is left open so nothing floats above the playing surface. */
    /* Snooker: a deeper, dark-olive "net bag" pouch the potted ball drops into.
     * Pool: a shallow near-black void. */
    uint16_t pk_floor = s_is_snooker ? RGB565C(34, 30, 20) : RGB565C(3, 4, 4);
    uint16_t pk_net   = s_is_snooker ? RGB565C(22, 20, 13) : RGB565C(6, 7, 7);
    const float floor_y = s_is_snooker ? -0.105f : -0.055f;
    for (int p = 0; p < w->npocket; p++) {
        float cx = w->pocket[p].x, cz = w->pocket[p].z;
        float r = w->pocket_r[p];     /* void = the functional drop (matches the red line) */
        Vec3 floor_c = v3(cx, floor_y, cz);
        const int N = 20;
        float base = atan2f(cz, cx);
        for (int k = 0; k < N; k++) {
            float a0 = base + k * (6.2831853f / N), a1 = base + (k + 1) * (6.2831853f / N);
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            Vec3 bed0 = v3(cx + r*c0, -0.002f, cz + r*s0);
            Vec3 bed1 = v3(cx + r*c1, -0.002f, cz + r*s1);
            if (s_is_snooker) {                              /* two-tone net pouch */
                float midy = -0.05f, midr = r * 0.62f;
                Vec3 m0 = v3(cx+midr*c0, midy, cz+midr*s0);
                Vec3 m1 = v3(cx+midr*c1, midy, cz+midr*s1);
                quad(bed0, bed1, m1, m0, pk_floor);
                tri(floor_c, m0, m1, pk_net);
            } else {
                tri(floor_c, bed0, bed1, pk_floor);          /* shallow dark void */
            }
        }
    }

    s_lip_ntab = s_ntab;      /* lips drawn last + depth-write OFF so balls cover them */
    emit_pocket_lips(t, w);   /* drop lip last → layers over the voids cleanly */
}

/* ---- per-frame build --------------------------------------------------- */
static void project_view(Vec3 v, float *sx, float *sy, uint16_t *d) {
    if (v.z < CUE_NEAR) v.z = CUE_NEAR;
    float inv = 1.0f / v.z;
    *sx = 64.0f + s_focal * v.x * inv;
    *sy = 64.0f - s_focal * v.y * inv;
    float dd = CUE_DEPTH_K * inv;
    *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
}
/* Push one screen triangle, ordering verts for positive area (double-sided
 * so a table face is never culled by winding). */
static void push_stri(float ax, float ay, uint16_t da, float bx, float by,
                      uint16_t db, float cx, float cy, uint16_t dc, uint16_t col) {
    if (s_nstri >= MAX_STRI) return;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    STri *st = &s_stri[s_nstri++];
    st->color = col;
    if (area >= 0) {
        st->x0 = ax; st->y0 = ay; st->d0 = da;
        st->x1 = bx; st->y1 = by; st->d1 = db;
        st->x2 = cx; st->y2 = cy; st->d2 = dc;
    } else {
        st->x0 = ax; st->y0 = ay; st->d0 = da;
        st->x1 = cx; st->y1 = cy; st->d1 = dc;
        st->x2 = bx; st->y2 = by; st->d2 = db;
    }
}
/* Transform to view space, clip against the near plane (Sutherland-Hodgman,
 * one plane → 1 or 2 tris), shade and emit. This is what keeps the bed/rails
 * visible when the camera sits on the table (near corners behind it). */
static void add_stri(Vec3 a, Vec3 b, Vec3 c, Vec3 nrm, uint16_t base) {
    Vec3 va = m3_mul_v3_t(&s_view.basis, v3_sub(a, s_view.pos));
    Vec3 vb = m3_mul_v3_t(&s_view.basis, v3_sub(b, s_view.pos));
    Vec3 vc = m3_mul_v3_t(&s_view.basis, v3_sub(c, s_view.pos));
    float ndl = v3_dot(nrm, s_light); if (ndl < 0) ndl = -ndl;
    uint16_t col = shade565(base, 0.32f + 0.68f * ndl);

    Vec3 poly[3] = { va, vb, vc };
    Vec3 out[4]; int no = 0;
    for (int i = 0; i < 3; i++) {
        Vec3 p = poly[i], q = poly[(i + 1) % 3];
        int pin = p.z > CUE_NEAR, qin = q.z > CUE_NEAR;
        if (pin) out[no++] = p;
        if (pin != qin) {
            float t = (CUE_NEAR - p.z) / (q.z - p.z);
            out[no++] = v3_lerp(p, q, t);
        }
    }
    if (no < 3) return;
    float ox[4], oy[4]; uint16_t od[4];
    for (int i = 0; i < no; i++) project_view(out[i], &ox[i], &oy[i], &od[i]);
    push_stri(ox[0], oy[0], od[0], ox[1], oy[1], od[1], ox[2], oy[2], od[2], col);
    if (no == 4)
        push_stri(ox[0], oy[0], od[0], ox[2], oy[2], od[2], ox[3], oy[3], od[3], col);
}

/* ===== Mote engine port: emit the scene through the engine ABI instead of
 * the in-game r3d rasteriser. cue_render_build now feeds scene_add_tri (table),
 * scene_add_sphere_tex (balls), scene_add_point/line (aim/cue) and a background
 * callback; the engine owns projection, depth, dual-core raster and present. */
static const MoteApi *s_api;
void cue_render_set_api(const MoteApi *api) { s_api = api; }

static uint16_t ball_base(uint8_t id);                       /* defined below */
static uint16_t ball_sample(uint8_t id, Vec3 nb, uint16_t base);

/* per-frame ball specular half-vectors (overhead 4-lamp cluster + single H) */
static Vec3 s_ballH, s_ballHl[4];
/* one MoteSphereTex per ball this frame (ud carries the ball id). */
static MoteSphereTex s_balltex[CUE_MAX_BALLS];

/* Per-pixel ball shading — ported verbatim from the old draw_ball inner loop.
 * The engine reconstructs the normal/depth and rotates it into ball space; we
 * return the final colour from the ball-local (nb) + world (nw) normals. */
static uint16_t cue_ball_shade(Vec3 nb, Vec3 nw, float de, float se, float nz, void *ud) {
    (void)de; (void)se;
    uint8_t id = (uint8_t)(uintptr_t)ud;
    uint16_t base = ball_base(id);
    float diff = v3_dot(nw, s_light); if (diff < 0) diff = 0;
    float s = v3_dot(nw, s_ballH);    if (s < 0) s = 0;
    float down = -nw.y;               if (down < 0) down = 0;
    uint16_t bc = ball_sample(id, nb, base);
    uint16_t col;
    switch (s_light_mode) {
    case 0:
        col = shade565(bc, (0.30f + 0.70f*diff) * (0.78f + 0.22f*nz));
        { float ss = s; ss*=ss; ss*=ss; ss*=ss; int hi=(int)(ss*26.0f);
          if (hi>0) col = add565(col, hi, hi*2, hi); }
        break;
    case 2:
        col = shade565(bc, diff>0.62f?1.0f : diff>0.30f?0.74f : 0.52f);
        col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.22f);
        if (s > 0.82f) col = RGB565C(250,250,250);
        break;
    case 3:
        col = shade565(bc, 0.30f + 0.70f*diff);
        col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.50f + down*0.40f);
        if (s > 0.60f) { float h=(s-0.60f)*2.5f; h*=h*h; int hi=(int)(h*30.0f);
          if (hi>0) col = add565(col, hi, hi, hi); }
        break;
    case 4: case 5: default: {
        float thr = (s_light_mode==5) ? 0.93f : (s_light_mode==4) ? 0.955f : 0.975f;
        float gain = (s_light_mode==5) ? 0.85f : 1.0f;
        col = shade565(bc, 0.46f + 0.54f*diff);
        col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.42f);
        float refl = 0.0f;
        for (int li = 0; li < 4; li++) {
            float si = v3_dot(nw, s_ballHl[li]);
            if (si > thr) { float h = (si - thr) / (1.0f - thr); refl += h*h; }
        }
        if (refl > 1.0f) refl = 1.0f;
        if (refl > 0.0f) col = mix565(col, RGB565C(255,255,255), refl * gain);
        break;
    }
    }
    return col;
}

/* Background vertical gradient — registered with the engine; runs per band. */
void cue_render_bg(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        float t = (float)y / 128.0f;
        int r = (int)(((s_bg_top >> 11) & 31) * (1 - t) + ((s_bg_bot >> 11) & 31) * t);
        int g = (int)(((s_bg_top >> 5) & 63) * (1 - t) + ((s_bg_bot >> 5) & 63) * t);
        int b = (int)(((s_bg_top) & 31) * (1 - t) + ((s_bg_bot) & 31) * t);
        uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
        uint16_t *row = fb + y * 128;
        for (int x = 0; x < 128; x++) row[x] = c;
    }
}

void cue_render_build(const CueView *v, const CueBall *balls, int n,
                      int aim_active, int aim_ball, Vec3 aim_dir,
                      float power, int aim_level) {
    s_view = *v;
    s_focal = 64.0f / tanf(v->fov_deg * (3.14159265f / 180.0f) * 0.5f);
    if (!s_api) return;
    s_api->scene_camera(&v->basis, v->pos, v->fov_deg);   /* resets the draw list */

    /* ball specular half-vectors for cue_ball_shade (4 overhead lamps + one H) */
    Vec3 vcam = v3_scale(v->basis.r[2], -1.0f);
    s_ballH = v3_norm(v3_add(s_light, vcam));
    const float lx = 0.42f, lz = 0.28f;
    s_ballHl[0] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z+lz)), vcam));
    s_ballHl[1] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z+lz)), vcam));
    s_ballHl[2] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z-lz)), vcam));
    s_ballHl[3] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z-lz)), vcam));

    /* Table: shade per tri (light is static), emit as world tris. Pocket-lip
     * tris (>= s_lip_ntab) are depth-tested but not depth-writing so the balls
     * paint over them. scene_add_tri is double-sided, matching the old build. */
    for (int i = 0; i < s_ntab; i++) {
        const CueTri *t = &s_tab[i];
        float ndl = v3_dot(t->nrm, s_light); if (ndl < 0) ndl = -ndl;
        uint16_t col = shade565(t->color, 0.32f + 0.68f * ndl);
        uint32_t fl = (i >= s_lip_ntab) ? MOTE_DRAW_NO_DEPTH_WRITE : 0;
        s_api->scene_add_tri(t->v[0], t->v[1], t->v[2], col, fl);
    }

    /* Ball shadows: soft ground-shadow decals on the cloth under each ball
     * (the engine darkens the felt with a radial falloff, like the original). */
    for (int i = 0; i < n; i++) {
        const CueBall *b = &balls[i];
        if (!b->on || b->drop > 0.0f) continue;
        s_api->scene_add_shadow(v3(b->pos.x, 0.0f, b->pos.z), s_ballR * 1.55f, 0.5f);
    }

    /* Balls → textured/oriented sphere impostors (the engine shades per pixel
     * via cue_ball_shade, rotated by the ball's spin orientation). */
    for (int i = 0; i < n && i < CUE_MAX_BALLS; i++) {
        const CueBall *b = &balls[i];
        if (!b->on) continue;
        s_balltex[i] = (MoteSphereTex){ .shade_mode = MOTE_SHADE_CUSTOM,
                                        .shade = cue_ball_shade,
                                        .ud = (void *)(uintptr_t)b->id };
        s_api->scene_add_sphere_tex(b->pos, s_ballR, &b->orient, &s_balltex[i]);
    }

    /* Aim line, ghost ball, object-ball line, cue stick. */
    if (aim_active && aim_ball >= 0 && aim_ball < n && balls[aim_ball].on) {
        Vec3 cuepos = balls[aim_ball].pos;
        Vec3 dir = v3_norm(v3(aim_dir.x, 0, aim_dir.z));
        const float twoR = 2.0f * s_ballR;
        const float step = s_ballR * 2.2f;
        int hit = -1;
        if (aim_level >= 1) {
            float bests = 1e9f;
            for (int i = 0; i < n; i++) {
                if (i == aim_ball || !balls[i].on) continue;
                Vec3 d = v3_sub(balls[i].pos, cuepos); d.y = 0;
                float along = v3_dot(d, dir);
                if (along <= 0) continue;
                float perp2 = (d.x * d.x + d.z * d.z) - along * along;
                if (perp2 < twoR * twoR) {
                    float s = along - sqrtf(twoR * twoR - perp2);
                    if (s > 0 && s < bests) { bests = s; hit = i; }
                }
            }
            float linelen = (hit >= 0) ? bests : 1.4f;
            int ndots = (int)(linelen / step);
            if (ndots > MAX_DOTS) ndots = MAX_DOTS;
            for (int k = 1; k <= ndots; k++)
                s_api->scene_add_point(v3(cuepos.x + dir.x*(k*step), s_ballR,
                                          cuepos.z + dir.z*(k*step)),
                                       RGB565C(240,240,160), 1);
            if (hit >= 0) {
                Vec3 ghost = v3(cuepos.x + dir.x*bests, s_ballR, cuepos.z + dir.z*bests);
                if (aim_level >= 3) {
                    Vec3 odir = v3_norm(v3(balls[hit].pos.x - ghost.x, 0,
                                           balls[hit].pos.z - ghost.z));
                    for (int k = 1; k <= 10; k++)
                        s_api->scene_add_point(v3(balls[hit].pos.x + odir.x*(k*step), s_ballR,
                                                  balls[hit].pos.z + odir.z*(k*step)),
                                               RGB565C(120,230,235), 1);
                }
                if (aim_level >= 2)   /* ghost ball: camera-facing ring at the contact point */
                    s_api->scene_add_ring(ghost, s_ballR, RGB565C(230, 230, 230));
            }
        }
        /* Cue stick: a tapered quad resting at the english-shifted contact point,
         * running back along the elevated cue axis (two world tris, double-sided). */
        Vec3 up = v3(0,1,0);
        Vec3 rightv = v3_norm(v3_cross(up, dir));
        float ce = cosf(s_cue_elev), se = sinf(s_cue_elev);
        Vec3 cdir = v3(dir.x*ce, -se, dir.z*ce);
        Vec3 contact = v3(cuepos.x + rightv.x*s_cue_side*s_ballR,
                          s_ballR     + s_cue_vert*s_ballR,
                          cuepos.z + rightv.z*s_cue_side*s_ballR);
        float gap = 0.015f + power * 0.18f;
        Vec3 tip  = v3(contact.x - cdir.x*gap, contact.y - cdir.y*gap, contact.z - cdir.z*gap);
        Vec3 butt = v3(tip.x - cdir.x*0.55f, tip.y - cdir.y*0.55f, tip.z - cdir.z*0.55f);
        const float wt = 0.004f, wb = 0.013f;   /* world half-widths: tip, butt */
        Vec3 t0 = v3(tip.x + rightv.x*wt,  tip.y,  tip.z + rightv.z*wt);
        Vec3 t1 = v3(tip.x - rightv.x*wt,  tip.y,  tip.z - rightv.z*wt);
        Vec3 b0 = v3(butt.x + rightv.x*wb, butt.y, butt.z + rightv.z*wb);
        Vec3 b1 = v3(butt.x - rightv.x*wb, butt.y, butt.z - rightv.z*wb);
        uint16_t cue_col = RGB565C(214, 176, 104);
        s_api->scene_add_tri(t0, t1, b1, cue_col, 0);
        s_api->scene_add_tri(t0, b1, b0, cue_col, 0);
    }
}

/* ---- ball sets --------------------------------------------------------- */
/* 0 = PRO (per-number coloured solids/stripes), 1 = UK yellow/blue solids,
 * 2 = UK yellow/red solids, 3 = US "dyna" (yellow solids / maroon stripes). */
void cue_render_set_ball_set(int s) { s_ball_set = (s < 0 || s > 7) ? 0 : s; }

/* the standard pro per-number hues for ids 1..7 (9..15 reuse 1..7's hue) */
static const uint16_t k_prohue[8] = {
    0, RGB565C(235,200,40), RGB565C(30,80,200), RGB565C(200,40,40),
    RGB565C(120,40,160), RGB565C(230,120,30), RGB565C(20,130,50),
    RGB565C(120,30,40) };
#define BALL_YELLOW RGB565C(235,200,40)
#define BALL_GOLD   RGB565C(228,165,20)
#define BALL_BLUE   RGB565C(30,80,200)
#define BALL_RED    RGB565C(200,40,40)
#define BALL_MAROON RGB565C(120,22,42)
#define BALL_BLACK  RGB565C(20,20,22)
#define BALL_WHITE  RGB565C(235,235,225)
/* Pro Tournament per-number palette (from 2dpool "Pro Tournament" ballColors);
 * striped balls use the same hue as their +8 solid, on BLACK poles. */
static const uint16_t k_ptourhue[8] = {
    0, RGB565C(245,180,0),  RGB565C(0,55,237),  RGB565C(255,30,0),
    RGB565C(255,71,123),    RGB565C(154,46,255), RGB565C(0,227,155),
    RGB565C(128,50,11) };
/* SPACE per-number palette (2dpool "Space" ballColors). */
static const uint16_t k_spacehue[8] = {
    0, RGB565C(255,215,0),  RGB565C(0,0,205),   RGB565C(255,0,0),
    RGB565C(75,0,130),      RGB565C(255,140,0), RGB565C(0,100,0),
    RGB565C(128,0,0) };
/* VINTAGE per-number palette (2dpool "Vintage" ballColors — muted gold/orange). */
static const uint16_t k_vintagehue[8] = {
    0, RGB565C(184,135,0),  RGB565C(0,0,205),   RGB565C(255,0,0),
    RGB565C(75,0,130),      RGB565C(255,115,0), RGB565C(0,100,0),
    RGB565C(128,0,0) };
/* per-set "pole"/stripe-background colour for striped balls 9..15. */
#define BALL_GREY  RGB565C(148,143,143)   /* SPACE poles */
#define BALL_CREAM RGB565C(255,233,153)   /* VINTAGE poles */
#define BALL_PINK  RGB565C(255,0,221)     /* HOT PINK group2 */
#define BALL_INK   RGB565C(19,16,16)      /* HOT PINK group1 */
static uint16_t stripe_bg(void) {
    switch (s_ball_set) {
        case 4: return BALL_BLACK;   /* pro tournament — black poles */
        case 6: return BALL_GREY;    /* space */
        case 7: return BALL_CREAM;   /* vintage */
        default: return BALL_WHITE;  /* PRO / dyna */
    }
}
/* striped, numbered sets: 0 PRO, 3 dyna, 4 pro-tour, 6 space, 7 vintage. */
static int set_striped(void) {
    return s_ball_set==0||s_ball_set==3||s_ball_set==4||s_ball_set==6||s_ball_set==7;
}
/* numbered sets (show the number circle / digit). */
static int set_numbered(void) {
    return s_ball_set==0||s_ball_set==3||s_ball_set==4||s_ball_set==6||s_ball_set==7;
}
/* hue for the current set's ball id 1..7 (used by solids and stripes). */
static uint16_t set_hue(uint8_t i) {
    switch (s_ball_set) {
        case 4: return k_ptourhue[i];
        case 6: return k_spacehue[i];
        case 7: return k_vintagehue[i];
        default: return k_prohue[i];
    }
}

/* ---- ball texture ------------------------------------------------------ */
static uint16_t ball_base(uint8_t id) {
    switch (id) {
        case CUE_ID_CUE:    return RGB565C(245, 245, 235);
        case CUE_ID_YELLOW: return RGB565C(235, 200, 40);
        case CUE_ID_GREEN:  return RGB565C(20, 130, 50);
        case CUE_ID_BROWN:  return RGB565C(120, 70, 35);
        case CUE_ID_BLUE:   return RGB565C(30, 80, 200);
        case CUE_ID_PINK:   return RGB565C(235, 120, 150);
        case CUE_ID_BLACK:  return RGB565C(20, 20, 22);
    }
    if (s_is_snooker) return RGB565C(190, 30, 30);          /* reds 1..15 */
    if (id == 8) return (s_ball_set == 5) ? RGB565C(158,158,158) : BALL_BLACK;
    switch (s_ball_set) {
        case 1: return (id <= 7) ? BALL_YELLOW : BALL_BLUE;    /* UK yellow/blue */
        case 2: return (id <= 7) ? BALL_YELLOW : BALL_RED;     /* UK yellow/red  */
        case 5: return (id <= 7) ? BALL_INK    : BALL_PINK;    /* hot pink: ink solids / pink */
        case 3: return (id <= 7) ? BALL_GOLD   : stripe_bg();  /* pro league: gold / maroon-on-white */
        default:                                               /* PRO / tour / space / vintage */
            return (id <= 7) ? set_hue(id) : stripe_bg();      /* per-num solids / striped poles */
    }
}
/* 3x5 digit glyphs, packed top row first, 3 bits/row (MSB = left column). */
static const uint16_t k_digit3x5[10] = {
    0x7B6F, /* 0: 111 101 101 101 111 */
    0x2C97, /* 1: 010 110 010 010 111 */
    0x73E7, /* 2: 111 001 111 100 111 */
    0x73CF, /* 3: 111 001 111 001 111 */
    0x5BC9, /* 4: 101 101 111 001 001 */
    0x79CF, /* 5: 111 100 111 001 111 */
    0x79EF, /* 6: 111 100 111 101 111 */
    0x7292, /* 7: 111 001 010 010 010 */
    0x7BEF, /* 8: 111 101 111 101 111 */
    0x7BCF, /* 9: 111 101 111 001 111 */
};

/* Render the white number circle (and, for the dyna set, the dynasphere black
 * ring + three spoke radii) onto the +x pole cap. `us` selects numbered sets. */
static uint16_t number_patch(uint8_t id, Vec3 nb, uint16_t base, int us) {
    if (!us || nb.x <= 0.90f) return base;
    /* Map the pole cap to a unit disc (py,pz); edge of the patch -> r2 ~ 1. */
    float py = nb.y * 2.30f, pz = nb.z * 2.30f;
    float r2 = py * py + pz * pz;
    if (r2 > 1.0f) return base;
    const uint16_t WHT = RGB565C(245, 245, 245);
    const uint16_t INK = RGB565C(15, 15, 18);
    /* dynasphere-style number circle: black ring + N spoke radii.
     * set 3 (pro league) = 3 spokes; set 4 (pro tournament) = 2 spokes. */
    int nspoke = (s_ball_set == 3) ? 3 : (s_ball_set == 4) ? 2 : 0;
    if (nspoke) {
        if (r2 > 0.78f) return INK;        /* outer black ring */
        if (r2 > 0.30f) {                  /* spoke radii, evenly spaced */
            static const float dk[3][2] = {
                {1.0f, 0.0f}, {-0.5f, 0.86603f}, {-0.5f, -0.86603f} };
            /* 2-spoke set uses a vertical pair; 3-spoke uses the tripod above */
            static const float dk2[2][2] = { {0.0f, 1.0f}, {0.0f, -1.0f} };
            const float (*sp)[2] = (nspoke == 2) ? dk2 : dk;
            for (int k = 0; k < nspoke; k++) {
                float dot = py * sp[k][0] + pz * sp[k][1];
                if (dot <= 0.0f) continue;
                float cr = py * sp[k][1] - pz * sp[k][0];
                if (cr * cr < 0.018f * r2) return INK;
            }
        }
    }
    /* Digit(s): 1 cell for 1-9, two side-by-side cells for 10-15. */
    int two = id >= 10;
    float uw = two ? 0.78f : 0.40f;         /* half-width of the glyph area */
    float gx = (pz + uw) / (2.0f * uw) * (two ? 7.0f : 3.0f);
    float gy = (0.62f - py) / 1.24f * 5.0f;
    int col = (int)gx, row = (int)gy;
    if (gx < 0.0f || gy < 0.0f || row > 4) return WHT;
    int d, dc;
    if (!two) { d = id % 10; dc = col; if (col > 2) return WHT; }
    else if (col < 3) { d = id / 10; dc = col; }
    else if (col < 4) return WHT;           /* gap column */
    else { d = id % 10; dc = col - 4; if (dc > 2) return WHT; }
    int rowbits = (k_digit3x5[d] >> ((4 - row) * 3)) & 7;
    return ((rowbits >> (2 - dc)) & 1) ? INK : WHT;
}

/* Sample the ball's surface colour for a ball-local unit normal. */
static uint16_t ball_sample(uint8_t id, Vec3 nb, uint16_t base) {
    /* Cue ball: a "measles" spotted ball — six small red dots, one centred on
     * each axis pole (±x, ±y, ±z), so spin reads clearly however it rolls. */
    if (id == CUE_ID_CUE) {
        float ax = fabsf(nb.x), ay = fabsf(nb.y), az = fabsf(nb.z);
        float m = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
        if (m > 0.965f) return RGB565C(198, 58, 46);  /* small red pole dots — vibrant but not garish */
        return base;
    }
    if (s_is_snooker) return base;              /* snooker balls are unmarked */
    int us = set_numbered();
    if (id >= 9 && id <= 15) {
        float half = (s_ball_set == 4) ? 0.55f : 0.42f;   /* pro-tour wider band */
        if (set_striped() && fabsf(nb.y) < half) {
            /* don't paint the stripe over the number circle */
            if (!(us && nb.x > 0.90f)) {
                if (s_ball_set == 3) return BALL_MAROON;
                return set_hue(id - 8);            /* PRO / pro-tour: per-number band */
            }
        }
        return number_patch(id, nb, base, us);  /* UK: solid body, no stripe */
    }
    if (id >= 1 && id <= 8)                       /* solids + 8 */
        return number_patch(id, nb, base, us);
    return base;
}

/* ---- raster ------------------------------------------------------------ */
static void draw_ball(uint16_t *fb, uint16_t *depth, const Sprite *sp,
                      int y0, int y1) {
    int rad = (int)(sp->rad + 0.999f);
    float inv_rad = 1.0f / sp->rad;
    int icx = (int)(sp->cx + 0.5f), icy = (int)(sp->cy + 0.5f);
    uint16_t base = ball_base(sp->id);
    /* camera-to-surface dir (specular) and light, world space. */
    Vec3 vcam = v3_scale(s_view.basis.r[2], -1.0f);   /* toward camera */
    Vec3 H = v3_norm(v3_add(s_light, vcam));
    /* Overhead fixture = 4 lamps in a 2×2 cluster → 4 sharp reflection dots.
     * Each reflects where the surface normal ≈ that lamp's half-vector. */
    const float lx = 0.42f, lz = 0.28f;   /* wide enough to read as 4 dots */
    Vec3 Hl[4];
    Hl[0] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z+lz)), vcam));
    Hl[1] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z+lz)), vcam));
    Hl[2] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z-lz)), vcam));
    Hl[3] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z-lz)), vcam));
    float R = s_ballR;
    for (int py = icy - rad; py <= icy + rad; py++) {
        if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
        float v = (py - sp->cy) * inv_rad;
        uint16_t *frow = fb + py * R3D_FB_W;
        uint16_t *drow = depth + py * R3D_FB_W;
        for (int px = icx - rad; px <= icx + rad; px++) {
            if (px < 0 || px >= CUE_FB_W) continue;
            float u = (px - sp->cx) * inv_rad;
            float rr = u * u + v * v;
            if (rr > 1.0f) continue;
            float nz = sqrtf(1.0f - rr);
            /* per-pixel depth: nearer than centre by R*nz. */
            float zpix = sp->viewz - R * nz;
            if (zpix < CUE_NEAR) zpix = CUE_NEAR;
            uint16_t d = (uint16_t)(CUE_DEPTH_K / zpix);
            if (d <= drow[px]) continue;
            /* view-space normal (screen y down → view up = -v). */
            Vec3 Nv = v3(u, -v, -nz);
            Vec3 Nw = m3_mul_v3(&s_view.basis, Nv);     /* view→world */
            float diff = v3_dot(Nw, s_light); if (diff < 0) diff = 0;
            float s = v3_dot(Nw, H); if (s < 0) s = 0;  /* specular base */
            float down = -Nw.y; if (down < 0) down = 0; /* underside (faces cloth) */
            Vec3 Nb = m3_mul_v3_t(&sp->orient, Nw);     /* world→ball-local */
            uint16_t bc = ball_sample(sp->id, Nb, base);
            uint16_t col;
            switch (s_light_mode) {
            case 0:  /* SMOOTH (original soft look) */
                col = shade565(bc, (0.30f + 0.70f*diff) * (0.78f + 0.22f*nz));
                { float ss = s; ss*=ss; ss*=ss; ss*=ss; int hi=(int)(ss*26.0f);
                  if (hi>0) col = add565(col, hi, hi*2, hi); }
                break;
            case 2:  /* TOON: banded diffuse + cloth shadow + crisp dot */
                col = shade565(bc, diff>0.62f?1.0f : diff>0.30f?0.74f : 0.52f);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.22f);
                if (s > 0.82f) col = RGB565C(250,250,250);
                break;
            case 3:  /* GLOSS: smooth body, strong cloth tint, sharp hotspot */
                col = shade565(bc, 0.30f + 0.70f*diff);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.50f + down*0.40f);
                if (s > 0.60f) { float h=(s-0.60f)*2.5f; h*=h*h; int hi=(int)(h*30.0f);
                  if (hi>0) col = add565(col, hi, hi, hi); }
                break;
            case 4:  /* 4-DOT medium */
            case 5:  /* 4-DOT large/soft */
            default: /* 1 = 4-DOT sharp: polished ball reflecting the 4 overhead
                      * lamps as crisp bright dots; saturated body, cloth-tinted
                      * lower half. */
            {
                float thr = (s_light_mode==5) ? 0.93f : (s_light_mode==4) ? 0.955f : 0.975f;
                float gain = (s_light_mode==5) ? 0.85f : 1.0f;
                col = shade565(bc, 0.46f + 0.54f*diff);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.42f);
                float refl = 0.0f;
                for (int li = 0; li < 4; li++) {
                    float si = v3_dot(Nw, Hl[li]);
                    if (si > thr) { float h = (si - thr) / (1.0f - thr); refl += h*h; }
                }
                if (refl > 1.0f) refl = 1.0f;
                /* lamp reflections are neutral white, NOT a brighter shade of
                 * the ball — blend toward pure white so every ball shows the
                 * same white dots (no pink/coloured tinge). */
                if (refl > 0.0f) col = mix565(col, RGB565C(255,255,255), refl * gain);
                break;
            }
            }
            frow[px] = col;
            drow[px] = d;
        }
    }
}

void cue_render_raster(uint16_t *fb, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > CUE_FB_H) y1 = CUE_FB_H;
    if (y0 >= y1) return;
    r3d_raster_set_fb(fb);
    uint16_t *depth = r3d_depth_buffer();

    /* background vertical gradient + depth clear */
    for (int y = y0; y < y1; y++) {
        float t = (float)y / (float)CUE_FB_H;
        int r = (int)(((s_bg_top >> 11) & 31) * (1 - t) + ((s_bg_bot >> 11) & 31) * t);
        int g = (int)(((s_bg_top >> 5) & 63) * (1 - t) + ((s_bg_bot >> 5) & 63) * t);
        int b = (int)(((s_bg_top) & 31) * (1 - t) + ((s_bg_bot) & 31) * t);
        uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
        uint16_t *row = fb + y * R3D_FB_W;
        for (int x = 0; x < R3D_FB_W; x++) row[x] = c;
    }
    r3d_depth_clear(y0, y1);

    /* table triangles — BED first (flat cloth), so shadows can paint over it
     * without the slate occluding them; the RAISED geometry (cushions, rails,
     * pocket voids) is drawn after the shadows and depth-tests over them. */
    for (int i = 0; i < s_bed_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* soft ground-plane shadow decals lying flat on the cloth. Each is an
     * ellipse C + s*U + t*V (|(s,t)|<=1) where U,V are the screen projections
     * of world +X/+Z offsets, so it foreshortens with the table and spreads
     * toward the camera (stays visible at the low aim-cam). We invert the 2×2
     * [U V] per pixel to recover (s,t) and fade darkness from centre to edge. */
    for (int i = 0; i < s_nshadow; i++) {
        float cx = s_shadow[i].cx, cy = s_shadow[i].cy;
        float ux = s_shadow[i].ux, uy = s_shadow[i].uy;
        float vx = s_shadow[i].vx, vy = s_shadow[i].vy;
        float det = ux * vy - uy * vx;
        if (det > -1e-4f && det < 1e-4f) continue;
        float inv = 1.0f / det;
        int bx = (int)(fabsf(ux) + fabsf(vx)) + 1;   /* screen bounding box */
        int by = (int)(fabsf(uy) + fabsf(vy)) + 1;
        int x0 = (int)cx - bx, x1b = (int)cx + bx;
        int yy0 = (int)cy - by, yy1 = (int)cy + by;
        for (int py = yy0; py <= yy1; py++) {
            if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
            uint16_t *frow = fb + py * R3D_FB_W;
            float ry = py - cy;
            for (int px = x0; px <= x1b; px++) {
                if (px < 0 || px >= CUE_FB_W) continue;
                float rx = px - cx;
                float s = ( rx * vy - ry * vx) * inv;
                float t = (-rx * uy + ry * ux) * inv;
                float r2 = s * s + t * t;
                if (r2 > 1.0f) continue;
                /* No depth test: shadows are drawn AFTER the cloth bed but
                 * BEFORE the raised geometry, so the slate never occludes them
                 * while cushions/rails (drawn next, depth-tested) paint over. */
                float k = 0.5f + 0.5f * r2 * r2;
                frow[px] = shade565(frow[px], k);
            }
        }
    }

    /* raised table geometry (cushions, rail frame, pocket voids) — depth-tested
     * over the shadows so a cushion/rail correctly hides a shadow behind it. */
    for (int i = s_bed_nstri; i < s_lip_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }
    /* Pocket drop lips: depth-TESTED (cushions/wood occlude them) but NOT
     * depth-WRITING, so the balls drawn afterwards always cover them. Uses the
     * no-write triangle (per-call flag, NOT a shared global) so the two cores
     * can raster concurrently — one core's lip pass no longer stops the other
     * core writing depth for the frame/cushions (that was the device flicker). */
    for (int i = s_lip_nstri; i < s_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri_nowrite(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                        t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* aim dots (cue path, pale yellow) + object-ball path (cyan) */
    for (int i = 0; i < s_ndot; i++)
        r3d_point((int)s_dot[i].x, (int)s_dot[i].y, 65000, RGB565C(240,240,160),
                  1, y0, y1);
    for (int i = 0; i < s_nodot; i++)
        r3d_point((int)s_odot[i].x, (int)s_odot[i].y, 65000, RGB565C(120,230,235),
                  1, y0, y1);

    /* ghost-ball ring */
    if (s_ghost.on) {
        int seg = 18;
        for (int k = 0; k < seg; k++) {
            float a0 = k * (6.2831853f / seg), a1 = (k + 1) * (6.2831853f / seg);
            r3d_line(s_ghost.cx + s_ghost.rad * cosf(a0),
                     s_ghost.cy + s_ghost.rad * sinf(a0), 65000,
                     s_ghost.cx + s_ghost.rad * cosf(a1),
                     s_ghost.cy + s_ghost.rad * sinf(a1), 65000,
                     RGB565C(230, 230, 230), y0, y1);
        }
    }

    /* balls */
    for (int i = 0; i < s_nspr; i++) draw_ball(fb, depth, &s_spr[i], y0, y1);

    /* cue stick (over the balls): a tapered shaded shaft drawn as three FULL-WIDTH
     * sections along its length — a 1px blue tip, a ~4px ivory ferrule, then the
     * wood shaft widening to the butt. Each section spans the full cue cross-
     * section (not a centre line), so the tip cap reads as the end of a cylinder. */
    if (s_cue.on) {
        float dx = s_cue.bx - s_cue.tx, dy = s_cue.by - s_cue.ty;
        float L = sqrtf(dx*dx + dy*dy);
        if (L > 1.0f) {
            float ux = dx/L, uy = dy/L;               /* along */
            float px = -uy, py = ux;                  /* perpendicular */
            float wt = 1.4f, wb = 4.3f;               /* half-width: tip → butt */
            float seg[4] = { 0.0f, 1.0f, 5.0f, L };   /* tip | ferrule | shaft */
            uint16_t col[3] = { RGB565C(70,90,180), RGB565C(238,234,212), s_cue.color };
            if (seg[1] > L) seg[1] = L;
            if (seg[2] > L) seg[2] = L;
            for (int s = 0; s < 3; s++) {
                float s0 = seg[s], s1 = seg[s+1]; if (s1 <= s0) continue;
                float w0 = wt + (wb-wt)*(s0/L), w1 = wt + (wb-wt)*(s1/L);
                float ax = s_cue.tx+ux*s0, ay = s_cue.ty+uy*s0;
                float bx2 = s_cue.tx+ux*s1, by2 = s_cue.ty+uy*s1;
                float aLx=ax+px*w0, aLy=ay+py*w0, aRx=ax-px*w0, aRy=ay-py*w0;
                float bLx=bx2+px*w1, bLy=by2+py*w1, bRx=bx2-px*w1, bRy=by2-py*w1;
                r3d_tri(aLx,aLy,65000, aRx,aRy,65000, bRx,bRy,65000, col[s], y0,y1);
                r3d_tri(aLx,aLy,65000, bRx,bRy,65000, bLx,bLy,65000, col[s], y0,y1);
            }
            /* sheen down the wood for a rounded look */
            float ss = seg[2];
            r3d_line(s_cue.tx+ux*ss, s_cue.ty+uy*ss, 65000, s_cue.bx, s_cue.by, 65000,
                     RGB565C(244,214,150), y0,y1);
        }
    }
}

/* Draw one flat sphere-shaded ball icon using the live ball_base/ball_sample.
 * face_x: 0 = +z toward viewer (stripe reads as a mid band; for HUD group hint);
 *         1 = +x toward viewer (the number circle faces out; for menu previews). */
static void draw_ball_icon(uint16_t *fb, int cx, int cy, int rad, uint8_t id, int face_x) {
    uint16_t base = ball_base(id);
    for (int dy = -rad; dy <= rad; dy++) {
        int y = cy + dy; if (y < 0 || y >= CUE_FB_H) continue;
        for (int dx = -rad; dx <= rad; dx++) {
            int x = cx + dx; if (x < 0 || x >= CUE_FB_W) continue;
            float u = (float)dx / rad, v = (float)dy / rad;
            float r2 = u * u + v * v; if (r2 > 1.0f) continue;
            float nz = sqrtf(1.0f - r2);
            Vec3 nb = face_x ? v3(nz, -v, u) : v3(u, -v, nz);
            uint16_t c = ball_sample(id, nb, base);
            float hl = -0.5f * u - 0.5f * v + 0.7f * nz;   /* top-left spec */
            if (hl > 0.95f) c = RGB565C(255, 255, 255);
            else            c = shade565(c, 0.45f + 0.55f * nz);
            fb[y * CUE_FB_W + x] = c;
        }
    }
}

/* HUD group hint. Pick boldly-distinct reps so the two sides never read the same:
 * low/solids = a red SOLID (3), high/stripes = a blue STRIPE (10). Avoids the
 * yellow solid-vs-stripe pair (1 / 9) which was hard to tell apart at icon size.
 * In UK sets (no stripes) these map to the two group colours anyway. */
void cue_render_group_icon(uint16_t *fb, int cx, int cy, int rad, int group) {
    draw_ball_icon(fb, cx, cy, rad, (group == 2) ? 10 : 3, 0);
}

/* HUD: draw a specific ball id (number circle facing out) with the live set —
 * used to show the 9-ball "ball to pot next". */
void cue_render_ball_icon(uint16_t *fb, int cx, int cy, int rad, int id) {
    draw_ball_icon(fb, cx, cy, rad, (uint8_t)id, 1);
}

/* Snooker "ball on" icon: target 0 = a RED ball, 2 = the sequence colour
 * (value `seq`, 2..7), 1 = "any colour" drawn as a 6-wedge multicolour ball. */
void cue_render_onball_icon(uint16_t *fb, int cx, int cy, int rad, int target, int seq) {
    int was = s_is_snooker; s_is_snooker = 1;
    if (target == 0) { draw_ball_icon(fb, cx, cy, rad, 1, 0); s_is_snooker = was; return; }
    if (target == 2) { draw_ball_icon(fb, cx, cy, rad, (uint8_t)(18 + seq), 0); s_is_snooker = was; return; }
    /* any colour → 6 angular wedges of the snooker colours */
    static const uint16_t cols[6] = {
        RGB565C(235,200,40), RGB565C(20,130,50), RGB565C(120,70,35),
        RGB565C(30,80,200), RGB565C(235,120,150), RGB565C(40,40,44) };
    for (int dy = -rad; dy <= rad; dy++) {
        int y = cy + dy; if (y < 0 || y >= CUE_FB_H) continue;
        for (int dx = -rad; dx <= rad; dx++) {
            int x = cx + dx; if (x < 0 || x >= CUE_FB_W) continue;
            float u = (float)dx/rad, v = (float)dy/rad;
            float r2 = u*u + v*v; if (r2 > 1.0f) continue;
            float nz = sqrtf(1.0f - r2);
            int w = (int)((atan2f(v, u) + 3.14159265f) * (6.0f / 6.2831853f));
            if (w < 0) w = 0; if (w > 5) w = 5;
            float hl = -0.5f*u - 0.5f*v + 0.7f*nz;
            uint16_t c = (hl > 0.95f) ? RGB565C(255,255,255) : shade565(cols[w], 0.45f + 0.55f*nz);
            fb[y*CUE_FB_W + x] = c;
        }
    }
    s_is_snooker = was;
}

/* 3D-shaded cue ball for the spin/aim HUD: a white sphere with a specular
 * highlight and the tip-contact marker drawn on its front face at (side,vert)
 * (fractions of R; +side = right english, +vert = top/follow). Replaces the old
 * flat 2D disc so the spin readout matches the game balls. */
void cue_render_spin_ball(uint16_t *fb, int cx, int cy, int rad,
                          float side, float vert) {
    const uint16_t body = RGB565C(238, 238, 228);
    const uint16_t spot = RGB565C(205, 45, 40);
    const uint16_t ring = RGB565C(120, 24, 22);
    float ms = 0.30f;                 /* marker radius (fraction of ball) */
    for (int dy = -rad; dy <= rad; dy++) {
        int y = cy + dy; if (y < 0 || y >= CUE_FB_H) continue;
        for (int dx = -rad; dx <= rad; dx++) {
            int x = cx + dx; if (x < 0 || x >= CUE_FB_W) continue;
            float u = (float)dx / rad, v = (float)dy / rad;
            float r2 = u * u + v * v; if (r2 > 1.0f) continue;
            float nz = sqrtf(1.0f - r2);
            float mu = u - side, mv = v + vert;       /* offset to contact point */
            float md = sqrtf(mu * mu + mv * mv);
            uint16_t c;
            if (md < ms * 0.62f)        c = spot;     /* contact dot */
            else if (md < ms)           c = ring;     /* dark rim around it */
            else {
                float hl = -0.5f * u - 0.5f * v + 0.7f * nz;
                c = (hl > 0.95f) ? RGB565C(255,255,255)
                                 : shade565(body, 0.5f + 0.5f * nz);
            }
            fb[y * CUE_FB_W + x] = c;
        }
    }
}

/* Ball-set preview row for the menu: a representative solid, stripe and the 8
 * (or red/colour/black for snooker), drawn with the given set so the player can
 * see what they're picking. Temporarily overrides the active set/snooker flag. */
void cue_render_set_preview(uint16_t *fb, int cx, int cy, int rad,
                            int ballset, int snooker) {
    int sb = s_ball_set, ss = s_is_snooker;
    s_ball_set = (ballset < 0 || ballset > 7) ? 0 : ballset;
    s_is_snooker = snooker;
    /* A small 6-ball triangle rack (rows of 1/2/3). Mixed solids + stripes so
     * each set reads clearly, with the BLACK (8) in the centre of the base row
     * (the rack's centre line). Snooker shows reds + colours instead. */
    /* distinct hues (per-number sets): yellow,blue,red,purple,green + black; idx4
     * is the rack centre. Mix of solids (1,3,6) and stripes (10,12). */
    static const uint8_t rack_pool[6] = { 1, 10, 3, 12, 8, 6 };
    static const uint8_t rack_snk[6]  = { 1, CUE_ID_YELLOW, CUE_ID_GREEN,
                                          CUE_ID_BROWN, CUE_ID_BLACK, CUE_ID_BLUE };
    const uint8_t *ids = snooker ? rack_snk : rack_pool;
    float dx = rad * 2.0f, dy = rad * 1.78f;
    int idx = 0;
    for (int row = 0; row < 3; row++) {
        float ry = cy + (row - 1) * dy;
        for (int j = 0; j <= row; j++) {
            float rx = cx + (j - row * 0.5f) * dx;
            draw_ball_icon(fb, (int)(rx + 0.5f), (int)(ry + 0.5f), rad, ids[idx++], 1);
        }
    }
    s_ball_set = sb; s_is_snooker = ss;
}
