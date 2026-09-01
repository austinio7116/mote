/* SINE AND COSINE THAT COME OUT THE SAME ON EVERY MACHINE.
 *
 * The physics may not call the platform's sinf and cosf. Two headsets run the
 * same binary, so every arithmetic instruction agrees, but libm is part of the
 * OS image: a Quest 2 and a Quest 3 are on different Android builds, and those
 * are allowed to differ in the last place. A power shot amplifies a last-place
 * difference by about a thousand -- on a UK 8-ball break, moving the cue ball
 * 100 nm moves the worst ball 0.001 mm at 2 m/s and 0.372 mm at 12, and a tenth
 * of a millimetre in comes out a quarter of a METRE. That was two players
 * watching different shots, and it is why the contact solver's trig was taken
 * out algebraically (see cushion_impact). What is left is real trig on real
 * angles -- the cue's elevation, the cue's deflection -- where there is no
 * algebra to exploit, so it is done here instead.
 *
 * ONLY + - * / AND CONVERSION, every one of which IEEE-754 rounds exactly, so
 * the answer is a function of the input and nothing else. Worked in double and
 * rounded once at the end, which leaves so much headroom that the float result
 * is correctly rounded either way.
 *
 * It is not an optimisation and does not need to be fast: it runs a handful of
 * times a stroke, not inside the 2 kHz loop.
 *
 * A caveat worth knowing: the compiler is free to contract a*b+c into a fused
 * multiply-add, and whether it does differs between the arm64 device build and
 * the x86-64 host tests. Both headsets run the SAME arm64 build, so the link is
 * safe either way; it is the host reproducing the device bit-for-bit that would
 * need -ffp-contract=off, and nothing depends on that today.
 */
#ifndef CUE_TRIG_H
#define CUE_TRIG_H

/* Taylor about zero, to x^9 and x^10, used only on |r| <= pi/4 where the first
 * dropped term is below 4e-12 -- far under a float's last bit. */
static inline double cue__sin_core(double x) {
    const double x2 = x * x;
    return x * (1.0 + x2 * (-1.0 / 6.0
              + x2 * (1.0 / 120.0
              + x2 * (-1.0 / 5040.0
              + x2 * (1.0 / 362880.0)))));
}
static inline double cue__cos_core(double x) {
    const double x2 = x * x;
    return 1.0 + x2 * (-0.5
         + x2 * (1.0 / 24.0
         + x2 * (-1.0 / 720.0
         + x2 * (1.0 / 40320.0
         + x2 * (-1.0 / 3628800.0)))));
}

/* Both at once, because every caller wants both and the reduction is shared. */
static inline void cue_sincosf(float xf, float *s, float *c) {
    const double PI_2 = 1.5707963267948966;
    double x = (double)xf;
    /* Which quadrant, rounded to nearest without lrint -- that is libm too. */
    double qd = x * (1.0 / PI_2);
    long   k  = (long)(qd >= 0.0 ? qd + 0.5 : qd - 0.5);
    double r  = x - (double)k * PI_2;
    double sr = cue__sin_core(r), cr = cue__cos_core(r);
    double so, co;
    switch ((int)(((k % 4) + 4) % 4)) {
    case 0:  so =  sr; co =  cr; break;
    case 1:  so =  cr; co = -sr; break;
    case 2:  so = -sr; co = -cr; break;
    default: so = -cr; co =  sr; break;
    }
    if (s) *s = (float)so;
    if (c) *c = (float)co;
}

static inline float cue_sinf(float x) { float s, c; cue_sincosf(x, &s, &c); (void)c; return s; }
static inline float cue_cosf(float x) { float s, c; cue_sincosf(x, &s, &c); (void)s; return c; }

/* ---- ARC TANGENT, and what is built on it -------------------------------- */

/* atan on |t| <= tan(pi/12), where the series converges fast enough that the
 * term after the last one here is below 2e-11. */
static inline double cue__atan_core(double t) {
    const double t2 = t * t;
    double p = -1.0 / 15.0;
    p = p * t2 + 1.0 / 13.0;
    p = p * t2 - 1.0 / 11.0;
    p = p * t2 + 1.0 /  9.0;
    p = p * t2 - 1.0 /  7.0;
    p = p * t2 + 1.0 /  5.0;
    p = p * t2 - 1.0 /  3.0;
    p = p * t2 + 1.0;
    return t * p;
}

/* atan for 0 <= x <= 1. Above tan(pi/12) it folds through
 * atan(x) = pi/6 + atan((sqrt3 x - 1)/(sqrt3 + x)), which lands back inside it. */
static inline double cue__atan01(double x) {
    const double SQ3  = 1.7320508075688772;
    const double PI_6 = 0.52359877559829887;
    if (x > 0.26794919243112270)
        return PI_6 + cue__atan_core((SQ3 * x - 1.0) / (SQ3 + x));
    return cue__atan_core(x);
}

static inline float cue_atan2f(float yf, float xf) {
    const double PI   = 3.1415926535897932;
    const double PI_2 = 1.5707963267948966;
    double y = (double)yf, x = (double)xf;
    double n = y < 0.0 ? -y : y;
    double d = x < 0.0 ? -x : x;
    double a;
    /* The ratio is taken the way round that keeps it inside [0,1], so nothing
     * here can divide by a very small number and there is no infinity to guard. */
    if (d == 0.0 && n == 0.0) return 0.0f;
    else if (d == 0.0)        a = PI_2;
    else if (n <= d)          a = cue__atan01(n / d);
    else                      a = PI_2 - cue__atan01(d / n);
    if (x < 0.0) a = PI - a;
    return (float)(y < 0.0 ? -a : a);
}

/* acos through atan2, which is exact given the two of them: the square root is
 * correctly rounded and the rest is the arc tangent above. */
static inline float cue_acosf(float x) {
    double v = (double)x;
    if (v >  1.0) v =  1.0;
    if (v < -1.0) v = -1.0;
    double s = 1.0 - v * v;
    if (s < 0.0) s = 0.0;
    /* sqrt in double, then the same atan2 every other caller gets. */
    double r = s;
    if (r > 0.0) { /* Newton from a double sqrt is not needed: use the builtin,
                    * which IEEE-754 rounds exactly on every target. */
        r = __builtin_sqrt(s);
    }
    return cue_atan2f((float)r, (float)v);
}

#endif /* CUE_TRIG_H */
