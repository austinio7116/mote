#include "mote_splat.h"
#include "mote_config.h"
#include <math.h>
#include <string.h>

#define SPLAT_MAX  6144     /* per-frame visible cap (RAM for the sort arrays) */
#define NB         512      /* depth buckets for the back-to-front sort */
#define BLUR       0.30f    /* low-pass: min screen variance so tiny splats show */
#define RMAX       30.0f    /* clamp a splat's screen radius (perf guard) */

static float    s_vz[SPLAT_MAX];
static int      s_src[SPLAT_MAX];   /* original splat index */
static int      s_order[SPLAT_MAX]; /* sorted far->near, holds positions in s_vz */
static int      s_cnt[NB];

/* exp() lookup over power in [-9,0] (beyond 3 sigma the weight is negligible). */
static float s_exp[256];
static int   s_exp_ready = 0;
static void exp_init(void) {
    for (int i = 0; i < 256; i++) s_exp[i] = expf(-9.0f * (float)i / 255.0f);
    s_exp_ready = 1;
}
static inline float fexp(float power) {       /* power <= 0 */
    if (power <= -9.0f) return 0.0f;
    int idx = (int)(-power * (255.0f / 9.0f));
    return s_exp[idx];
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, float a) {
    int br = (bg >> 11) & 31, bgr = (bg >> 5) & 63, bb = bg & 31;
    int fr = (fg >> 11) & 31, fgr = (fg >> 5) & 63, fb = fg & 31;
    int r = br + (int)((fr - br) * a + 0.5f);
    int g = bgr + (int)((fgr - bgr) * a + 0.5f);
    int b = bb + (int)((fb - bb) * a + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

int mote_splat_render(uint16_t *fb, const MoteSplat *splats, int n,
                      const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg) {
    if (!s_exp_ready) exp_init();
    float f = (MOTE_FB_W * 0.5f) / tanf(fov_deg * (3.14159265f/180.0f) * 0.5f);
    Vec3 R0 = cam_basis->r[0], R1 = cam_basis->r[1], R2 = cam_basis->r[2];

    /* pass 1: cull behind camera, record view-z, find range */
    int m = 0;
    float zmin = 1e30f, zmax = -1e30f;
    for (int i = 0; i < n && m < SPLAT_MAX; i++) {
        Vec3 d = v3_sub(splats[i].pos, cam_pos);
        float vz = v3_dot(d, R2);
        if (vz <= 0.15f) continue;
        s_vz[m] = vz; s_src[m] = i;
        if (vz < zmin) zmin = vz; if (vz > zmax) zmax = vz;
        m++;
    }
    if (m == 0) return 0;

    /* counting sort by view-z, placed far->near */
    float scale = (NB - 1) / (zmax - zmin + 1e-4f);
    for (int b = 0; b < NB; b++) s_cnt[b] = 0;
    for (int k = 0; k < m; k++) { int b = (int)((s_vz[k]-zmin)*scale); s_cnt[b]++; }
    int acc = 0;                                  /* offsets, highest bucket first */
    static int off[NB];
    for (int b = NB-1; b >= 0; b--) { off[b] = acc; acc += s_cnt[b]; }
    for (int k = 0; k < m; k++) { int b = (int)((s_vz[k]-zmin)*scale); s_order[off[b]++] = k; }

    /* pass 2: project + blend, far -> near */
    for (int t = 0; t < m; t++) {
        const MoteSplat *sp = &splats[s_src[s_order[t]]];
        Vec3 d = v3_sub(sp->pos, cam_pos);
        float vx = v3_dot(d, R0), vy = v3_dot(d, R1), vz = v3_dot(d, R2);
        float inv = 1.0f / vz;

        /* 3D covariance in view space: Sv_ij = Ri^T Sigma Rj (symmetric) */
        const float *c = sp->cov;
        #define SXV(w) v3(c[0]*(w).x + c[1]*(w).y + c[2]*(w).z, \
                          c[1]*(w).x + c[3]*(w).y + c[4]*(w).z, \
                          c[2]*(w).x + c[4]*(w).y + c[5]*(w).z)
        Vec3 SR0 = SXV(R0), SR1 = SXV(R1), SR2 = SXV(R2);
        float A = v3_dot(R0,SR0), B = v3_dot(R0,SR1), C = v3_dot(R0,SR2);
        float D = v3_dot(R1,SR1), E = v3_dot(R1,SR2), F = v3_dot(R2,SR2);
        #undef SXV

        /* projection Jacobian and 2D covariance Sigma' = J Sv J^T */
        float j00 = f*inv, j02 = -f*vx*inv*inv;
        float j11 = -f*inv, j12 =  f*vy*inv*inv;
        float r0_0 = j00*A + j02*C, r0_1 = j00*B + j02*E, r0_2 = j00*C + j02*F;
        float r1_1 = j11*D + j12*E, r1_2 = j11*E + j12*F;
        float a = r0_0*j00 + r0_2*j02 + BLUR;
        float b = r0_1*j11 + r0_2*j12;
        float cc = r1_1*j11 + r1_2*j12 + BLUR;
        float det = a*cc - b*b;
        if (det < 1e-6f) continue;
        float idet = 1.0f/det, ia = cc*idet, ib = -b*idet, ic = a*idet;

        /* 3-sigma screen radius from the larger eigenvalue */
        float mid = 0.5f*(a+cc), dif = 0.5f*(a-cc);
        float lmax = mid + sqrtf(dif*dif + b*b);
        float rad = 3.0f*sqrtf(lmax);
        if (rad < 0.6f) continue;
        if (rad > RMAX) rad = RMAX;

        float sx = (MOTE_FB_W*0.5f) + f*vx*inv;
        float sy = (MOTE_FB_H*0.5f) - f*vy*inv;
        int x0 = (int)(sx-rad), x1 = (int)(sx+rad)+1;
        int y0 = (int)(sy-rad), y1 = (int)(sy+rad)+1;
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > MOTE_FB_W) x1 = MOTE_FB_W; if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;
        if (x0 >= x1 || y0 >= y1) continue;

        float op = sp->opacity; uint16_t col = sp->color;
        for (int py = y0; py < y1; py++) {
            float dy = (float)py - sy;
            uint16_t *row = fb + py*MOTE_FB_W;
            float bdy = ib*dy, half = 0.5f*ic*dy*dy;   /* per-row constants */
            for (int px = x0; px < x1; px++) {
                float dx = (float)px - sx;
                float power = -(0.5f*ia*dx*dx + bdy*dx + half);
                if (power <= -9.0f) continue;
                float al = op * fexp(power);
                if (al < 0.004f) continue;
                if (al > 1.0f) al = 1.0f;
                row[px] = blend565(row[px], col, al);
            }
        }
    }
    return m;
}
