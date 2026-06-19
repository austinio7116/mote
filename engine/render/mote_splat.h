/*
 * mote_splat — 3D Gaussian-splat renderer (a 4th render path beside raster /
 * raycast / 2D). Each splat is an anisotropic 3D Gaussian (centre + 3x3
 * covariance + colour + opacity); rendering projects each to a screen-space 2D
 * Gaussian (EWA), depth-sorts back-to-front, and alpha-blends soft ellipses into
 * the RGB565 framebuffer. Tuned for the small 128x128 screen + a few-thousand
 * splat budget on the RP2350.
 */
#ifndef MOTE_SPLAT_H
#define MOTE_SPLAT_H

#include <stdint.h>
#include "mote_vec.h"

typedef struct {
    Vec3     pos;      /* world-space centre */
    float    cov[6];   /* symmetric 3D covariance: xx,xy,xz,yy,yz,zz */
    uint16_t color;    /* RGB565 */
    float    opacity;  /* 0..1 peak alpha */
} MoteSplat;

/* Build a splat from an anisotropic scale (the 3 std-devs) + an orthonormal
 * basis `rot` (rows = the splat's local axes). Flatten one axis for disc-like
 * surface splats. Header-inline (pure math) so game modules need no engine link.
 * Sigma = sum_k scale_k^2 * outer(axis_k, axis_k). */
static inline MoteSplat mote_splat_make(Vec3 pos, Vec3 scale, Mat3 rot,
                                        uint16_t color, float opacity) {
    Vec3 a0 = rot.r[0], a1 = rot.r[1], a2 = rot.r[2];
    float s0 = scale.x*scale.x, s1 = scale.y*scale.y, s2 = scale.z*scale.z;
    MoteSplat sp;
    sp.pos = pos; sp.color = color; sp.opacity = opacity;
    sp.cov[0] = s0*a0.x*a0.x + s1*a1.x*a1.x + s2*a2.x*a2.x; /* xx */
    sp.cov[1] = s0*a0.x*a0.y + s1*a1.x*a1.y + s2*a2.x*a2.y; /* xy */
    sp.cov[2] = s0*a0.x*a0.z + s1*a1.x*a1.z + s2*a2.x*a2.z; /* xz */
    sp.cov[3] = s0*a0.y*a0.y + s1*a1.y*a1.y + s2*a2.y*a2.y; /* yy */
    sp.cov[4] = s0*a0.y*a0.z + s1*a1.y*a1.z + s2*a2.y*a2.z; /* yz */
    sp.cov[5] = s0*a0.z*a0.z + s1*a1.z*a1.z + s2*a2.z*a2.z; /* zz */
    return sp;
}

/* Render `n` splats into fb (128x128 RGB565), back-to-front, OVER existing
 * pixels. Camera-relative (centres are world; cam_pos subtracted internally).
 * Returns the number actually drawn (after culling / the internal cap). */
int mote_splat_render(uint16_t *fb, const MoteSplat *splats, int n,
                      const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg);

#endif /* MOTE_SPLAT_H */
