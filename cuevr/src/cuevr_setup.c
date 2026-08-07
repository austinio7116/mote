/*
 * CueVR — putting the table in your room.
 *
 * Every session starts here, and it is not a nicety. The point of playing cue
 * sports in passthrough is that the cloth lands on a surface you can actually
 * lean on — a real table, a desk, the end of a bed — so that when you drop your
 * bridge hand it meets something. Get the height wrong by five centimetres and
 * your hand goes through the table or hovers above it, and the illusion is
 * gone. So height is a control of its own, it is shown in centimetres while you
 * set it, and it is the first thing you do.
 *
 * Both sticks work about the CUE BALL rather than the table's centre. The cue
 * ball is where you are standing and what you are about to hit; keeping it
 * still while the table swings and slides underneath is what lets you bring any
 * shot on the table to where your body already is — which is the whole answer
 * to a twelve-foot table in a room that is not twelve feet long.
 *
 *   left stick    slide the table (in your own view frame: push it away and it
 *                 goes away from you, not along some world axis you cannot see)
 *   right stick X yaw the table about the cue ball
 *   right stick Y raise and lower the cloth
 *   A / trigger   done
 *
 * Pure maths over the tracking struct, so test_setup.c can drive it.
 */
#include "cuevr.h"

#include <math.h>
#include <string.h>

#define DEADZONE     0.18f
#define SLIDE_RATE  0.90f    /* m/s at full deflection */
#define YAW_RATE     0.95f    /* rad/s (~54 deg/s) */
#define HEIGHT_RATE  0.22f    /* m/s */

/* A pub table is ~0.80 m to the cloth, a dining table ~0.75, a desk ~0.73.
 * Start at snooker height and let the player find their own surface. */
#define HEIGHT_DEFAULT 0.85f
#define HEIGHT_MIN     0.25f
#define HEIGHT_MAX     1.35f

static int s_lefty;
void cuevr_setup_left_handed(int on) { s_lefty = on ? 1 : 0; }

void cuevr_setup_init(CueVrSetup *s, float floor_y) {
    memset(s, 0, sizeof *s);
    s->place.yaw = 0.0f;
    s->place.height = floor_y + HEIGHT_DEFAULT;
    s->place.pos = mv3(0.0f, s->place.height, 0.0f);
    s->last_height = s->place.height;
    s->active = 1;
}

static float dead(float v) {
    if (v > DEADZONE)  return (v - DEADZONE) / (1.0f - DEADZONE);
    if (v < -DEADZONE) return (v + DEADZONE) / (1.0f - DEADZONE);
    return 0.0f;
}

/* The sticks, on their own. Called every frame of setup AND every frame you
 * are down on a shot, because bringing the table to you is not a thing you
 * only need once. */
/* Turn the table about a point that must not move — the cue ball, always.
 *
 * This is a two-line rotation that has now been got wrong twice in two separate
 * places, so it lives in one place. The trap is that the table's own transform
 * rotates its CONTENTS by -yaw (cuevr_table_to_room: x' = x*cos + z*sin), so
 * orbiting the table's origin by +dyaw while adding +dyaw to the yaw applies
 * the rotation twice in opposite senses and the "fixed" point is not fixed at
 * all: the ball swings away on a circle and the table appears to turn about
 * somewhere else entirely — measured at 205 mm of travel for a quarter radian.
 * The orbit has to use the SAME sense the yaw means. */
void cuevr_setup_yaw_about(CueVrSetup *s, MoteVrV3 pivot_room, float dyaw) {
    if (dyaw == 0.0f) return;
    float cs = cosf(dyaw), sn = sinf(dyaw);
    MoteVrV3 rel = mv3_sub(s->place.pos, pivot_room);
    s->place.pos = mv3(pivot_room.x + rel.x * cs + rel.z * sn,
                       s->place.pos.y,
                       pivot_room.z - rel.x * sn + rel.z * cs);
    s->place.yaw += dyaw;
}

void cuevr_setup_adjust(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 ball_room,
                        int allow_height) {
    float dt = t->dt > 0.0f && t->dt < 0.25f ? t->dt : 1.0f / 72.0f;

    /* Sliding is the OFF hand's stick and turning is the DOMINANT hand's, and
     * for a left-hander both are the other controller — the whole layout is a
     * mirror, not just the cue. Set from the same menu row. */
    const MoteVrHand *L  = &t->hand[s_lefty ? MOTE_VR_RIGHT : MOTE_VR_LEFT];
    const MoteVrHand *Rh = &t->hand[s_lefty ? MOTE_VR_LEFT  : MOTE_VR_RIGHT];

    /* ---- slide, in the player's own frame ------------------------------- *
     * Forward is where the head is looking, flattened onto the floor. Pushing
     * the stick away from you moves the table away from you whichever way you
     * happen to be facing, which is the only mapping that needs no thought. */
    float sx = dead(L->stick_x), sy = dead(L->stick_y);
    if (sx != 0.0f || sy != 0.0f) {
        MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
        fwd.y = 0.0f;
        if (mv3_len(fwd) < 1e-3f) fwd = mv3(0, 0, -1);
        fwd = mv3_norm(fwd);
        MoteVrV3 right = mv3_norm(mv3_cross(mv3(0, 1, 0), mv3_scale(fwd, -1.0f)));
        MoteVrV3 d = mv3_add(mv3_scale(fwd, sy * SLIDE_RATE * dt),
                             mv3_scale(right, sx * SLIDE_RATE * dt));
        s->place.pos = mv3_add(s->place.pos, d);
    }

    /* ---- yaw, about the cue ball ---------------------------------------- *
     * Rotating the placement alone would swing the table about its own centre
     * and take the cue ball with it. Orbit the table's origin about the ball by
     * the same angle and the ball stays exactly where it is, under your bridge
     * hand, while the table turns beneath it. */
    float rx = dead(Rh->stick_x);
    if (rx != 0.0f) cuevr_setup_yaw_about(s, ball_room, rx * YAW_RATE * dt);

    /* ---- height ---------------------------------------------------------- *
     * Setup only. Leaving it on a live stick during play meant any thumb
     * pressure walked the cloth up or down while you were lining up, so the
     * table you had carefully matched to a real surface would not stay put.
     * Sliding and turning stay live — those you want on every shot — but the
     * height is set once and then left alone. */
    float ry = allow_height ? dead(Rh->stick_y) : 0.0f;
    if (ry != 0.0f) {
        float h = s->place.height + ry * HEIGHT_RATE * dt;
        if (h < HEIGHT_MIN) h = HEIGHT_MIN;
        if (h > HEIGHT_MAX) h = HEIGHT_MAX;
        s->place.height = h;
        s->place.pos.y  = h;
        s->last_height  = h;
    }

}

int cuevr_setup_update(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 ball_room) {
    if (!s->active) return 0;
    cuevr_setup_adjust(s, t, ball_room, 1);
    /* Done: A only. The right trigger is the cue stroke now, so it cannot also
     * mean "yes" — confirming setup with it would immediately arm a shot. */
    if (t->hand[s_lefty ? MOTE_VR_LEFT : MOTE_VR_RIGHT].btn_lower) {
        s->active = 0;
        s->confirmed = 1;
        return 0;
    }
    return 1;
}
