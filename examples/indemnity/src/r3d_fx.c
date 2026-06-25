/*
 * ThumbyElite — particle / beam effects.
 */
#include "r3d_fx.h"
#include "r3d_scene.h"
#include "elite_types.h"
#include <string.h>

#define MAX_PARTICLES 1024  /* human fire model + lance rails: big pool ~37KB, ample slot headroom */
#define MAX_BEAMS     24   /* many ships fire hitscan at once now */

typedef struct {
    Vec3  pos, vel;
    float life, max_life;
    uint16_t color0, color1;   /* lerp colour over life (hot -> cool) */
} Particle;

typedef struct {
    Vec3  a, b;
    float life;
    uint16_t color;
} Beam;

/* Mote: the particle pool (36 KB) lives in the load-time arena, not module .bss —
 * handed in via r3d_fx_set_parts() before the first frame. */
static Particle *s_parts;
size_t r3d_fx_parts_bytes(void) { return (size_t)MAX_PARTICLES * sizeof(Particle); }
void   r3d_fx_set_parts(void *p) { s_parts = (Particle *)p; }
static Beam     s_beams[MAX_BEAMS];
static int      s_nparts;
static float    s_trail_accum;

static uint32_t s_rng = 0xC0FFEE11u;
static uint32_t frnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)(frnd() & 0xFFFF) * (1.0f / 65535.0f);
}
static Vec3 rnd_dir(void) {
    Vec3 d = v3(frand(-1, 1), frand(-1, 1), frand(-1, 1));
    return (v3_len2(d) < 1e-4f) ? v3(0, 1, 0) : v3_norm(d);
}

void fx_init(void) {
    memset(s_parts, 0, r3d_fx_parts_bytes());
    memset(s_beams, 0, sizeof s_beams);
    s_nparts = 0;
    s_trail_accum = 0;
}

/* Defined below fireball storage; cleared via dur<=t sentinel at init —
 * static zero init means t==dur==0, i.e. inactive. */

int fx_alive_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) if (s_parts[i].life > 0) n++;
    return n;
}

static Particle *alloc_part(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        int k = (s_nparts + i) % MAX_PARTICLES;
        if (s_parts[k].life <= 0) { s_nparts = k + 1; return &s_parts[k]; }
    }
    return NULL;   /* pool full: drop, never steal (explosions stay whole) */
}

static void spawn(Vec3 pos, Vec3 vel, float life, uint16_t c0, uint16_t c1) {
    Particle *p = alloc_part();
    if (!p) return;
    p->pos = pos; p->vel = vel;
    p->life = p->max_life = life;
    p->color0 = c0; p->color1 = c1;
}

/* --- Fireballs: expanding depth-tested discs — the BIG readable part of
 * an explosion at any distance (debris particles alone vanish at range). */
#define MAX_FIREBALLS 12
typedef struct {
    Vec3  pos, vel;
    float t, dur;        /* age / duration */
    float r_max;         /* world-space radius at peak, meters */
    uint8_t kind;        /* 0 = fire (orange), 1 = shield (blue) */
} Fireball;
static Fireball s_fire[MAX_FIREBALLS];

static void fireball_k(Vec3 pos, Vec3 vel, float r_max, float dur,
                       uint8_t kind) {
    int oldest = 0;
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        if (s_fire[i].t >= s_fire[i].dur) { oldest = i; break; }
        if (s_fire[i].t > s_fire[oldest].t) oldest = i;
    }
    s_fire[oldest].pos = pos;
    s_fire[oldest].vel = vel;
    s_fire[oldest].t = 0;
    s_fire[oldest].dur = dur;
    s_fire[oldest].r_max = r_max;
    s_fire[oldest].kind = kind;
}
static void fireball(Vec3 pos, Vec3 vel, float r_max, float dur) {
    fireball_k(pos, vel, r_max, dur, 0);
}

void fx_spawn_explosion(Vec3 pos, Vec3 base_vel) {
    /* Core flash + fireball + secondary pops. */
    fireball(pos, base_vel, 14.0f, 0.9f);
    fireball(v3_add(pos, v3_scale(rnd_dir(), 4.0f)),
             base_vel, 7.0f, 0.65f);
    fireball(v3_add(pos, v3_scale(rnd_dir(), 6.0f)),
             base_vel, 5.0f, 1.1f);
    /* Debris: fast, plentiful, long-lived. */
    for (int i = 0; i < 48; i++) {
        Vec3 v = v3_add(base_vel, v3_scale(rnd_dir(), frand(8, 45)));
        float life = frand(0.6f, 1.8f);
        uint16_t hot = (i & 3) ? RGB565C(255, 200, 80) : RGB565C(255, 255, 220);
        spawn(pos, v, life, hot, RGB565C(90, 30, 10));
    }
}

void fx_flak_burst(Vec3 pos, Vec3 base_vel) {
    /* A flak AIRBURST — a sharp frag puff, not a ship explosion: one
     * quick flash + a radial star of fast frag embers fading to smoke,
     * sized to the burst radius. */
    fireball(pos, base_vel, 6.0f, 0.30f);
    for (int i = 0; i < 24; i++) {
        Vec3 v = v3_add(base_vel, v3_scale(rnd_dir(), frand(28, 70)));
        spawn(pos, v, frand(0.25f, 0.55f),
              RGB565C(255, 210, 120), RGB565C(120, 120, 130));
    }
}

void fx_spawn_spark(Vec3 pos, Vec3 base_vel) {
    /* A visible impact: brief flash disc + a spray of embers. */
    fireball(pos, base_vel, 2.6f, 0.18f);
    for (int i = 0; i < 10; i++) {
        Vec3 v = v3_add(base_vel, v3_scale(rnd_dir(), frand(6, 20)));
        spawn(pos, v, frand(0.2f, 0.45f),
              RGB565C(255, 240, 160), RGB565C(180, 80, 30));
    }
}

/* SHIELD ENVELOPE (user): a full blue bubble flicker around the WHOLE
 * ship + a shell of blue motes — reads instantly at any range as
 * 'shields holding'. */
void fx_shield_envelope(Vec3 center, Vec3 vel, float radius) {
    fireball_k(center, vel, radius * 1.15f, 0.20f, 1);
    for (int i = 0; i < 26; i++) {
        Vec3 dir = rnd_dir();
        Vec3 p = v3_add(center, v3_scale(dir, radius * frand(0.95f, 1.18f)));
        spawn(p, v3_add(vel, v3_scale(dir, frand(3, 10))),
              frand(0.12f, 0.26f),
              RGB565C(160, 210, 255), RGB565C(40, 90, 210));
    }
}

/* HULL BURST (user): a real fireball on hull damage, scaled by the blow
 * so it's visible from a distance — bigger hits, bigger flash. */
void fx_hull_burst(Vec3 pos, Vec3 vel, float scale) {
    if (scale < 0.15f) scale = 0.15f;
    if (scale > 1.0f) scale = 1.0f;
    fireball(pos, vel, 3.5f + 6.0f * scale, 0.26f + 0.10f * scale);
    int n = 12 + (int)(20.0f * scale);
    for (int i = 0; i < n; i++) {
        Vec3 v = v3_add(vel, v3_scale(rnd_dir(), frand(10, 25 + 35 * scale)));
        spawn(pos, v, frand(0.3f, 0.7f),
              RGB565C(255, 220, 130), RGB565C(170, 65, 25));
    }
}

/* SYSTEM BREAK (user): even bigger — a component just blew. */
void fx_break_blast(Vec3 pos, Vec3 vel) {
    fireball(pos, vel, 11.0f, 0.55f);
    fireball(v3_add(pos, v3_scale(rnd_dir(), 4.0f)), vel, 6.0f, 0.40f);
    for (int i = 0; i < 30; i++) {
        Vec3 v = v3_add(vel, v3_scale(rnd_dir(), frand(20, 65)));
        spawn(pos, v, frand(0.4f, 1.1f),
              (i & 1) ? RGB565C(255, 240, 180) : RGB565C(255, 170, 70),
              RGB565C(120, 40, 15));
    }
}

uint32_t frnd_pub(void) { return frnd(); }

void fx_chaff_burst(Vec3 pos, Vec3 base_vel) {
    for (int i = 0; i < 22; i++) {
        Vec3 v = v3_add(base_vel, v3_scale(rnd_dir(), frand(10, 35)));
        spawn(pos, v, frand(0.5f, 1.2f),
              RGB565C(230, 230, 240), RGB565C(90, 90, 100));
    }
}

void fx_spawn_shield_flash(Vec3 pos, Vec3 base_vel, int ion) {
    /* Shield impact: a blue shimmer burst — reads instantly as 'their
     * shields are still up' (vs orange hull sparks). */
    int n = ion ? 12 : 7;
    for (int i = 0; i < n; i++) {
        Vec3 v = v3_add(base_vel, v3_scale(rnd_dir(), frand(4, 16)));
        spawn(pos, v, frand(0.12f, 0.30f),
              ion ? RGB565C(200, 230, 255) : RGB565C(110, 170, 255),
              RGB565C(40, 70, 180));
    }
}

void fx_spawn_crackle(Vec3 pos, Vec3 base_vel, float r) {
    /* Ion scramble: brief blue-white arcs dancing on the hull. */
    for (int i = 0; i < 3; i++) {
        Vec3 off = v3_scale(rnd_dir(), r * frand(0.5f, 1.0f));
        spawn(v3_add(pos, off), v3_add(base_vel, v3_scale(rnd_dir(), 6.0f)),
              frand(0.10f, 0.22f),
              RGB565C(180, 220, 255), RGB565C(60, 90, 200));
    }
}

void fx_gauss_helix(Vec3 prev, Vec3 cur, Vec3 dir, float traveled) {
    /* Spawn a helix point-pair every SPACING metres of flight; the pool
     * holds ~100 live pairs at gauss speed, fading white -> deep blue.
     * Slight outward drift makes the corkscrew visibly unwind. */
    const float SPACING = 12.0f;
    float seg = v3_len(v3_sub(cur, prev));
    if (seg < 1e-3f) return;
    Vec3 ref = (dir.y < 0.9f && dir.y > -0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
    Vec3 e1 = v3_norm(v3_cross(dir, ref));
    Vec3 e2 = v3_cross(dir, e1);
    float t0 = traveled - seg;
    float s2 = ((float)(int)(t0 / SPACING) + 1.0f) * SPACING;
    for (; s2 <= traveled; s2 += SPACING) {
        Vec3 base = v3_sub(cur, v3_scale(dir, traveled - s2));
        float ang = s2 * 0.55f;
        Vec3 off = v3_add(v3_scale(e1, cosf(ang)), v3_scale(e2, sinf(ang)));
        spawn(v3_add(base, off), v3_scale(off, 2.2f), 0.55f,
              RGB565C(225, 245, 255), RGB565C(30, 50, 130));
        spawn(v3_sub(base, off), v3_scale(off, -2.2f), 0.55f,
              RGB565C(160, 200, 255), RGB565C(25, 40, 110));
    }
}

void fx_engine_trail(Vec3 rear_pos, Vec3 ship_vel, float throttle, float dt) {
    if (throttle < 0.15f) return;
    /* Emission rate scales with throttle; accumulate fractional spawns. */
    s_trail_accum += throttle * 40.0f * dt;
    while (s_trail_accum >= 1.0f) {
        s_trail_accum -= 1.0f;
        Vec3 jitter = v3_scale(rnd_dir(), 0.25f);
        spawn(v3_add(rear_pos, jitter),
              v3_add(v3_scale(ship_vel, 0.2f), v3_scale(rnd_dir(), 1.5f)),
              frand(0.25f, 0.5f),
              RGB565C(120, 190, 255), RGB565C(30, 50, 110));
    }
}

/* PLASMA LANCE (user): a thick purple shaft with STRAIGHT parallel
 * particle streaks running alongside it — railgun heft, no corkscrew.
 * The beam itself is the core; four lines of motes ring the axis at a
 * fixed small radius, drifting gently outward as they fade. */
void fx_lance(Vec3 from, Vec3 to, uint16_t color) {
    fx_beam(from, to, color);                    /* the bright core */
    Vec3 axis = v3_sub(to, from);
    float len = v3_len(axis);
    if (len < 1e-3f) return;
    Vec3 dir = v3_scale(axis, 1.0f / len);
    Vec3 ref = (dir.y < 0.9f && dir.y > -0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
    Vec3 e1 = v3_norm(v3_cross(dir, ref));
    Vec3 e2 = v3_cross(dir, e1);
    const float SPACING = 9.0f, RADIUS = 1.6f;   /* dense rails; pool is big now */
    for (float d = 0.0f; d <= len; d += SPACING) {
        Vec3 base = v3_add(from, v3_scale(dir, d));
        for (int k = 0; k < 4; k++) {            /* 4 parallel rails */
            float ang = (float)k * 1.5707963f;   /* 90 deg apart, FIXED */
            Vec3 off = v3_add(v3_scale(e1, cosf(ang) * RADIUS),
                              v3_scale(e2, sinf(ang) * RADIUS));
            spawn(v3_add(base, off), v3_scale(off, 1.4f), 0.45f,
                  RGB565C(220, 180, 255), RGB565C(80, 30, 150));
        }
    }
}

void fx_beam(Vec3 from, Vec3 to, uint16_t color) {
    for (int i = 0; i < MAX_BEAMS; i++) {
        if (s_beams[i].life > 0) continue;
        s_beams[i].a = from;
        s_beams[i].b = to;
        s_beams[i].life = 0.07f;
        s_beams[i].color = color;
        return;
    }
}

void fx_tick(float dt) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &s_parts[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->pos = v3_add(p->pos, v3_scale(p->vel, dt));
    }
    for (int i = 0; i < MAX_BEAMS; i++)
        if (s_beams[i].life > 0) s_beams[i].life -= dt;
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        Fireball *f = &s_fire[i];
        if (f->t >= f->dur) continue;
        f->t += dt;
        f->pos = v3_add(f->pos, v3_scale(f->vel, dt));
    }
}

static uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* --- Space dust --------------------------------------------------------
 * Two wrapping shells around the camera give translation cues the
 * at-infinity starfield can't:
 *   near shell: bright motes, +-60m, drawn as velocity streaks at speed
 *   far shell:  dim points, +-500m — distant scenery that drifts slowly
 * Positions wrap per-axis so the cloud always surrounds the player. */
#define DUST_NEAR_N 30
#define DUST_FAR_N  20
#define DUST_NEAR_R 60.0f
#define DUST_FAR_R  500.0f
static Vec3 s_dust_near[DUST_NEAR_N];
static Vec3 s_dust_far[DUST_FAR_N];
static int  s_dust_seeded;

static void dust_seed(Vec3 c) {
    for (int i = 0; i < DUST_NEAR_N; i++)
        s_dust_near[i] = v3_add(c, v3(frand(-DUST_NEAR_R, DUST_NEAR_R),
                                      frand(-DUST_NEAR_R, DUST_NEAR_R),
                                      frand(-DUST_NEAR_R, DUST_NEAR_R)));
    for (int i = 0; i < DUST_FAR_N; i++)
        s_dust_far[i] = v3_add(c, v3(frand(-DUST_FAR_R, DUST_FAR_R),
                                     frand(-DUST_FAR_R, DUST_FAR_R),
                                     frand(-DUST_FAR_R, DUST_FAR_R)));
    s_dust_seeded = 1;
}

/* Fold a mote into the [c-r, c+r] box around the camera. Modular, not
 * single-step: a single-step wrap (+-2r once) never catches up after a
 * teleport or a fast cruise, leaving every mote thousands of Mm behind
 * (the 'supercruise has no particles' bug). */
static float wrap1(float v, float c, float r) {
    float d = fmodf(v - c + r, 2.0f * r);
    if (d < 0) d += 2.0f * r;
    return c + d - r;
}
static void dust_wrap(Vec3 *p, Vec3 c, float r) {
    p->x = wrap1(p->x, c.x, r);
    p->y = wrap1(p->y, c.y, r);
    p->z = wrap1(p->z, c.z, r);
}

static void dust_emit(Vec3 cam_pos, Vec3 cam_vel) {
    if (!s_dust_seeded) dust_seed(cam_pos);
    float speed = v3_len(cam_vel);
    /* Streak = where this mote appeared a short moment ago, relative to
     * us — i.e. offset by +vel*dt_streak (we moved, the dust didn't). */
    Vec3 streak = v3_scale(cam_vel, 0.045f);
    int do_streaks = speed > 25.0f;
    uint16_t cn = RGB565C(150, 160, 175);
    for (int i = 0; i < DUST_NEAR_N; i++) {
        dust_wrap(&s_dust_near[i], cam_pos, DUST_NEAR_R);
        Vec3 rel = v3_sub(s_dust_near[i], cam_pos);
        if (do_streaks) g_em->scene_add_line(rel, v3_add(rel, streak), cn);
        else            g_em->scene_add_point(rel, cn, 1);
    }
    uint16_t cf = RGB565C(90, 95, 110);
    for (int i = 0; i < DUST_FAR_N; i++) {
        dust_wrap(&s_dust_far[i], cam_pos, DUST_FAR_R);
        g_em->scene_add_point(v3_sub(s_dust_far[i], cam_pos), cf, 1);
    }
}

/* Fireball colour ramp: white flash -> yellow -> orange -> dark ember. */
static uint16_t fireball_color(float t01, uint8_t kind) {
    if (kind == 1) {                  /* shield bubble: blue flicker */
        if (t01 < 0.25f) return RGB565C(200, 230, 255);
        if (t01 < 0.55f) return RGB565C(90, 165, 255);
        return RGB565C(30, 80, 210);
    }
    if (t01 < 0.15f) return RGB565C(255, 255, 235);
    if (t01 < 0.40f) return RGB565C(255, 215, 90);
    if (t01 < 0.70f) return RGB565C(245, 130, 40);
    return RGB565C(140, 50, 18);
}

static void fireballs_emit(Vec3 cam_pos) {
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        const Fireball *f = &s_fire[i];
        if (f->t >= f->dur) continue;
        float t01 = f->t / f->dur;
        /* Fast expansion, brief hold, slight shrink as it gutters out. */
        float grow = t01 < 0.35f ? (t01 / 0.35f)
                                 : 1.0f - 0.35f * ((t01 - 0.35f) / 0.65f);
        /* World-radius disc — the engine projects it to a screen circle. */
        g_em->scene_add_disc(v3_sub(f->pos, cam_pos), f->r_max * grow,
                             fireball_color(t01, f->kind));
    }
}

int g_dbg_dust[2];
float g_dbg_dustf[4];

/* --- Supercruise debris -------------------------------------------------
 * Mm-scale wrapping motes. The wrap box grows with speed so there is
 * always *something* streaming past; streaks stretch with velocity. */
#define SC_DUST_N 48
static Vec3 s_sc_dust[SC_DUST_N];
static int  s_sc_seeded;
static float s_scd_flow, s_scd_speed;
static int   s_scd_on;   /* drawn only on frames the game actually emits (supercruise) */

/* Cleared by the game each frame before building the scene; fx_sc_dust_emit
 * re-arms it. So normal flight (no emit) shows no dust instead of frozen motes. */
void fx_sc_dust_off(void) { s_scd_on = 0; }

void fx_sc_dust_emit(Vec3 cam_pos_mm, Vec3 vel_mms) {
    s_scd_on = 1;
    /* Screen-space starline flow (same z-cycling technique as the
     * hyperspace tunnel, throttled by real speed). The old free-space
     * motes crossed their wrap box in ~3 frames at cruise speed and
     * read as flickering "pulsing lines" (user report) — this flows.
     * Travel direction is always the nose in SC, so the vanishing
     * point is screen centre. */
    (void)cam_pos_mm;
    /* Screen-space tunnel streaks: state updated here each frame, DRAWN in the
     * background pass (fx_sc_dust_draw) since they have no world position. */
    s_scd_flow += v3_len(vel_mms) * (1.0f / 30.0f) * 0.0035f;
    s_scd_speed = v3_len(vel_mms);
}

/* Plot screen-space dust streaks into the framebuffer band [yb0,yb1). Called
 * from the background pass (before geometry; these are distant motes). */
void fx_sc_dust_draw(uint16_t *fb, int yb0, int yb1) {
    if (!s_scd_on) return;          /* only when the game emitted dust this frame */
    float speed = s_scd_speed;
    float k = speed * (1.0f / 2200.0f);
    if (k > 1.0f) k = 1.0f;
    int count = (int)(44.0f * sqrtf(k));
    if (count < 1) return;
    float s_flow = s_scd_flow;
    for (int i = 0; i < count; i++) {
        uint32_t h = 0x5CD0u ^ (uint32_t)(i * 2654435761u);
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        float ang = (float)(h & 0x3FF) * (6.2831853f / 1024.0f);
        float sm = 0.55f + (float)((h >> 10) & 0xFF) * (1.0f / 255.0f);
        float z0 = (float)((h >> 18) & 0x3FF) * (2.4f / 1024.0f);
        float zz = z0 - s_flow * sm;
        zz = zz - 2.4f * floorf(zz / 2.4f);
        zz += 0.14f;
        float r1 = 9.5f / zz;
        if (r1 < 4.5f) continue;                 /* far hole: keep clear */
        if (r1 > 96.0f) r1 = 96.0f;
        float dz = 0.045f * k * sm;
        float r0 = 9.5f / (zz + dz);
        float ca = cosf(ang), sa = sinf(ang) * 0.92f;
        float x0 = 64.0f + ca * r0, y0 = 60.0f + sa * r0;
        float x1 = 64.0f + ca * r1, y1 = 60.0f + sa * r1;
        uint16_t c = (zz < 0.5f) ? RGB565C(170, 185, 215)
                   : (zz < 1.2f) ? RGB565C(110, 125, 160)
                                 : RGB565C(60, 72, 100);
        g_em->draw_line(fb, (int)x0, (int)y0, (int)x1, (int)y1, c, yb0, yb1);
    }
}

void fx_emit_all(Vec3 cam_pos, Vec3 cam_vel) {
    dust_emit(cam_pos, cam_vel);
    fireballs_emit(cam_pos);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &s_parts[i];
        if (p->life <= 0) continue;
        float t = 1.0f - p->life / p->max_life;
        Vec3 rel = v3_sub(p->pos, cam_pos);
        /* Near particles get a 2x2 block (closer than ~40 m). */
        uint8_t size = v3_len(rel) < 40.0f ? 2 : 1;
        g_em->scene_add_point(rel, lerp565(p->color0, p->color1, t), size);
    }
    for (int i = 0; i < MAX_BEAMS; i++) {
        const Beam *bm = &s_beams[i];
        if (bm->life <= 0) continue;
        g_em->scene_add_line(v3_sub(bm->a, cam_pos), v3_sub(bm->b, cam_pos), bm->color);
    }
}
