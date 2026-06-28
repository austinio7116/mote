/*
 * ThumbyElite — flight model.
 */
#include "elite_flight.h"
#include "elite_player.h"

#define BOOST_TIME   2.2f
#define BOOST_MULT   1.8f
#define THROTTLE_RATE 0.9f   /* full range per ~1.1s of held throttle */

/* Turn ramp: first touch turns gently, full rate after ~0.45s held —
 * fine aim on tap, fast slew on hold (user request). */
static float s_ramp_pitch, s_ramp_yaw, s_ramp_roll;
static float ramp(float *t, float active, float dt) {
    if (active != 0.0f) *t += dt; else *t = 0.0f;
    float k = *t / 0.45f;
    if (k > 1.0f) k = 1.0f;
    return 0.01f + 0.99f * k;
}

/* Steering momentum (user request): the actual turn rate CHASES the
 * commanded rate — fast attack, slower release — so alternating UP /
 * RIGHT taps blend into a diagonal arc the 4-way d-pad can't command
 * directly. Release tail ~0.25s: enough to blend, not floaty. */
static float s_vel_pitch, s_vel_yaw, s_vel_roll;
static float chase(float *vel, float target, float dt) {
    float k = (target != 0.0f) ? 14.0f : 4.5f;
    float a = k * dt;
    if (a > 1.0f) a = 1.0f;
    *vel += (target - *vel) * a;
    if (target == 0.0f && *vel < 0.02f && *vel > -0.02f) *vel = 0.0f;
    return *vel;
}

void flight_apply_input(const FlightInput *in, float dt) {
    Ship *p = &g_ships[PLAYER];
    if (!p->alive) return;

    float tr = p->turn_rate * turn_envelope(p) * dt;
    float rp = chase(&s_vel_pitch,
                     in->pitch * ramp(&s_ramp_pitch, in->pitch, dt), dt);
    float ry = chase(&s_vel_yaw,
                     in->yaw * ramp(&s_ramp_yaw, in->yaw, dt), dt);
    float rr = chase(&s_vel_roll,
                     in->roll * ramp(&s_ramp_roll, in->roll, dt), dt);
    /* Pitch about the ship's right axis, yaw about up, roll about forward.
     * Flight-stick convention (user-confirmed): d-pad UP = nose DOWN
     * ("push the stick forward"). An un-inverted option can join the
     * pause-menu settings later. */
    if (rp != 0.0f) m3_rotate_local(&p->basis, 0, tr * rp);
    if (ry != 0.0f) m3_rotate_local(&p->basis, 1, tr * ry);
    if (rr != 0.0f) m3_rotate_local(&p->basis, 2, tr * 1.5f * rr);
    m3_orthonormalize(&p->basis);

    if (in->throttle_abs >= 0.0f)
        p->throttle = in->throttle_abs;            /* HOTAS lever: absolute */
    else
        p->throttle += in->throttle_delta * THROTTLE_RATE * dt;
    if (p->throttle < 0.0f) p->throttle = 0.0f;
    if (p->throttle > 1.0f) p->throttle = 1.0f;

    if (in->assist_toggle) p->assist = !p->assist;
    if (in->boost && p->boost_t <= 0.0f)
        p->boost_t = player_has_util(EQ_TANK) ? 4.0f : BOOST_TIME;
}

static void ship_physics(Ship *s, float dt) {
    float max_v = s->max_speed, acc = s->accel;
    if (s->engine_drag_t > 0.0f) { max_v *= 0.5f; acc *= 0.5f; }
    if (s->boost_t > 0.0f) {
        s->boost_t -= dt;
        max_v *= BOOST_MULT;
        acc *= BOOST_MULT;
        if (s->throttle < 1.0f) s->throttle = 1.0f;   /* boost floors it */
    }

    Vec3 fwd = s->basis.r[2];
    if (s->assist) {
        /* Velocity chases throttle * nose direction. */
        Vec3 desired = v3_scale(fwd, s->throttle * max_v);
        Vec3 dv = v3_sub(desired, s->vel);
        float dl = v3_len(dv);
        float step = acc * dt;
        s->vel = (dl <= step || dl < 1e-5f)
                     ? desired
                     : v3_add(s->vel, v3_scale(dv, step / dl));
    } else {
        /* Drift: thrust along the nose only; speed capped, never damped. */
        s->vel = v3_add(s->vel, v3_scale(fwd, s->throttle * acc * dt));
        float v2 = v3_len2(s->vel);
        float cap = max_v * 1.15f;
        if (v2 > cap * cap) s->vel = v3_scale(v3_norm(s->vel), cap);
    }

    s->pos = v3_add(s->pos, v3_scale(s->vel, dt));
}

void flight_tick(float dt) {
    for (int i = 0; i < MAX_SHIPS; i++)
        if (g_ships[i].alive) ship_physics(&g_ships[i], dt);
}
