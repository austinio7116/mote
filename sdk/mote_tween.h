/*
 * mote_tween.h — header-only lerp / approach / easing helpers.
 *
 * Pure math, compiled INTO the game (every function is `static inline`): no ABI
 * surface, no resident-engine RAM/flash — the same discipline as mote_anim.h
 * and the inline helpers in mote_build.h. The engine ABI is reserved for things
 * only the resident engine can do (framebuffer, audio, input, save, render
 * pools); interpolation needs none of that, so it lives here.
 *
 *   pos.x = mote_approach(pos.x, target.x, 9.0f, dt);   // frame-rate independent
 *   yaw   = mote_approach_angle(yaw, target_yaw, 6.0f, dt);
 *   MoteTween tw; mote_tween_start(&tw, 0, score, 1.5f, mote_ease_out_cubic);
 *   float shown = mote_tween_step(&tw, dt);             // one-shot eased value
 *
 * The easing set mirrors the original Thumby engine's Tween (EASE_*), so a
 * faithful port can match its feel exactly.
 */
#ifndef MOTE_TWEEN_H
#define MOTE_TWEEN_H

#include <math.h>
#include "mote_vec.h"   /* Vec3, v3_lerp */

#define MOTE_PI   3.14159265358979f
#define MOTE_TAU  6.28318530717959f

static inline float mote_lerp(float a, float b, float t)   { return a + (b - a) * t; }
static inline float mote_clamp01(float t)                  { return t < 0 ? 0 : (t > 1 ? 1 : t); }

/* Frame-rate-independent exponential approach toward `target`. `rate` is the
 * responsiveness in 1/seconds (larger = snappier); stable for any dt. This is
 * the correct form of the common `x += (target-x)*k` smoothing — k here is
 * derived from dt so the motion is identical at 30 or 60 fps. */
static inline float mote_approach(float cur, float target, float rate, float dt) {
    return cur + (target - cur) * (1.0f - expf(-rate * dt));
}
static inline Vec3 mote_approach3(Vec3 cur, Vec3 target, float rate, float dt) {
    return v3_lerp(cur, target, 1.0f - expf(-rate * dt));
}
/* Shortest-arc angle lerp by factor t (radians) — never spins the long way. */
static inline float mote_lerp_angle(float a, float b, float t) {
    float d = b - a;
    while (d >  MOTE_PI) d -= MOTE_TAU;
    while (d < -MOTE_PI) d += MOTE_TAU;
    return a + d * t;
}
/* Frame-rate-independent shortest-arc angle approach toward `target`. */
static inline float mote_approach_angle(float cur, float target, float rate, float dt) {
    return mote_lerp_angle(cur, target, 1.0f - expf(-rate * dt));
}

/* ---- easing functions, t in [0,1] (Penner set; matches Thumby EASE_*) ----- */
static inline float mote_ease_linear(float t)      { return t; }
static inline float mote_ease_in_sine(float t)     { return 1.0f - cosf(t * 1.57079633f); }
static inline float mote_ease_out_sine(float t)    { return sinf(t * 1.57079633f); }
static inline float mote_ease_inout_sine(float t)  { return -(cosf(MOTE_PI * t) - 1.0f) * 0.5f; }
static inline float mote_ease_in_quad(float t)     { return t * t; }
static inline float mote_ease_out_quad(float t)    { float u = 1.0f - t; return 1.0f - u * u; }
static inline float mote_ease_inout_quad(float t)  { return t < 0.5f ? 2.0f*t*t : 1.0f - (-2.0f*t+2.0f)*(-2.0f*t+2.0f)*0.5f; }
static inline float mote_ease_in_cubic(float t)    { return t * t * t; }
static inline float mote_ease_out_cubic(float t)   { float u = 1.0f - t; return 1.0f - u*u*u; }
static inline float mote_ease_inout_cubic(float t) { return t < 0.5f ? 4.0f*t*t*t : 1.0f - powf(-2.0f*t+2.0f, 3.0f)*0.5f; }
static inline float mote_ease_out_back(float t)    { const float c1=1.70158f, c3=c1+1.0f; float u=t-1.0f; return 1.0f + c3*u*u*u + c1*u*u; }
static inline float mote_ease_out_elastic(float t) { if (t<=0) return 0; if (t>=1) return 1; const float c4=MOTE_TAU/3.0f; return powf(2.0f,-10.0f*t)*sinf((t*10.0f-0.75f)*c4)+1.0f; }
static inline float mote_ease_out_bounce(float t)  { const float n1=7.5625f, d1=2.75f;
    if (t < 1.0f/d1)        return n1*t*t;
    else if (t < 2.0f/d1)   { t -= 1.5f/d1;   return n1*t*t + 0.75f; }
    else if (t < 2.5f/d1)   { t -= 2.25f/d1;  return n1*t*t + 0.9375f; }
    else                    { t -= 2.625f/d1; return n1*t*t + 0.984375f; } }

/* ---- one-shot eased tween of a scalar (mirrors Thumby Tween.start) -------- */
typedef float (*MoteEaseFn)(float);
typedef struct { float from, to, t, dur; MoteEaseFn ease; int done; } MoteTween;

static inline void  mote_tween_start(MoteTween *tw, float from, float to, float dur, MoteEaseFn ease) {
    tw->from = from; tw->to = to; tw->t = 0; tw->dur = dur > 0 ? dur : 0.0001f;
    tw->ease = ease ? ease : mote_ease_linear; tw->done = 0;
}
static inline float mote_tween_step(MoteTween *tw, float dt) {
    tw->t += dt; float p = tw->t / tw->dur; if (p >= 1.0f) { p = 1.0f; tw->done = 1; }
    return tw->from + (tw->to - tw->from) * tw->ease(p);
}
static inline int   mote_tween_done(const MoteTween *tw) { return tw->done; }

#endif /* MOTE_TWEEN_H */
