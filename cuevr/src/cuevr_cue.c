/*
 * CueVR — two controllers, one cue.
 *
 * The whole of "natural" cueing is that nothing is assisted. The cue is the
 * line between your two hands: the left is the bridge it rests on, the right is
 * the butt. Raise your back hand and the cue elevates. Move the bridge closer
 * to the ball and you shorten up. Aim by pointing the line, put side on by
 * moving the line off the ball's centre, and play the shot by pushing the cue
 * through — the power is the speed the tip is doing when it arrives, exactly as
 * on a real table. There is no aim line, no power bar and no snapping.
 *
 * Which means all of it is geometry, and geometry is testable: see
 * test_cue.c, which plays scripted strokes and asserts the shot that comes out.
 *
 * The output is the argument list cue_phys_strike_elev already takes, because
 * the physics was written for a real cue before any of this existed.
 */
#include "cuevr.h"

#include <math.h>
#include <string.h>

void cuevr_cue_init(CueVrCue *c) {
    memset(c, 0, sizeof *c);
    /* A snooker cue is ~1.45 m and you bridge maybe 25 cm behind the tip. That
     * is the distance from the bridge hand to the tip, which is all the
     * geometry needs — the butt is wherever your other hand actually is. */
    c->bridge_len = 0.28f;
}

/* Room space -> the table's own frame (metres, X length, Z width, Y up from
 * the cloth), which is the only frame the physics knows about. */
MoteVrV3 cuevr_room_to_table(const CueVrPlacement *p, MoteVrV3 r) {
    MoteVrV3 d = mv3_sub(r, p->pos);
    float cs = cosf(-p->yaw), sn = sinf(-p->yaw);
    return mv3(d.x * cs - d.z * sn, d.y, d.x * sn + d.z * cs);
}

MoteVrV3 cuevr_table_to_room(const CueVrPlacement *p, Vec3 t) {
    float cs = cosf(p->yaw), sn = sinf(p->yaw);
    return mv3(p->pos.x + t.x * cs - t.z * sn,
               p->pos.y + t.y,
               p->pos.z + t.x * sn + t.z * cs);
}

void cuevr_cue_update(CueVrCue *c, const MoteVrTracking *t,
                      const CueVrPlacement *p, MoteVrV3 ball_room, float R,
                      CueVrShot *out)
{
    memset(out, 0, sizeof *out);

    const MoteVrHand *bridge_hand = &t->hand[MOTE_VR_LEFT];
    const MoteVrHand *butt_hand   = &t->hand[MOTE_VR_RIGHT];
    if (!bridge_hand->tracked || !butt_hand->tracked) {
        c->have_prev = 0;
        c->on_ball = 0;
        c->tracked = 0;
        return;
    }

    c->bridge = bridge_hand->pose.p;
    c->butt   = butt_hand->pose.p;

    MoteVrV3 along = mv3_sub(c->bridge, c->butt);
    if (mv3_len(along) < 0.05f) {       /* hands together: no direction to hold */
        c->have_prev = 0;
        c->on_ball = 0;
        c->tracked = 0;
        return;
    }
    c->tracked = 1;
    c->axis = mv3_norm(along);
    c->tip  = mv3_add(c->bridge, mv3_scale(c->axis, c->bridge_len));

    /* Elevation is the cue's own tilt: the axis runs butt -> tip, so cueing
     * down on the ball (butt raised) points it below horizontal. The physics
     * wants that as a positive angle. */
    float e = asinf(-c->axis.y < -1.0f ? -1.0f : (-c->axis.y > 1.0f ? 1.0f : -c->axis.y));
    c->elev = e > 0.0f ? e : 0.0f;

    /* Aim is the axis flattened onto the cloth. */
    MoteVrV3 flat = mv3(c->axis.x, 0.0f, c->axis.z);
    c->aim_dir = mv3_len(flat) > 1e-4f ? mv3_norm(flat) : mv3(1, 0, 0);

    /* ---- where the line meets the ball ---------------------------------- *
     * Ray from the tip along the axis against a sphere of radius R. If it
     * misses, there is no shot to play and the cue simply passes by. */
    MoteVrV3 to_ball = mv3_sub(ball_room, c->tip);
    float along_axis = mv3_dot(to_ball, c->axis);
    float perp2 = mv3_dot(to_ball, to_ball) - along_axis * along_axis;
    float r2 = R * R;

    if (perp2 > r2) {
        /* The cue line misses the ball. That is a perfectly normal state — it is
         * most of the time, while you are lining up — so the cue still exists
         * and is still drawn; only the strike is unavailable. An earlier version
         * drew nothing until the line happened to cross the ball, which meant
         * you could not see the cue you were trying to aim. */
        c->on_ball = 0;
        c->have_prev = 0;
        c->gap = along_axis - R;
        return;
    }
    c->on_ball = 1;

    /* Entry point on the near face of the sphere. */
    float half_chord = sqrtf(r2 - perp2);
    float t_enter = along_axis - half_chord;
    MoteVrV3 hit = mv3_add(c->tip, mv3_scale(c->axis, t_enter));
    MoteVrV3 off = mv3_sub(hit, ball_room);          /* length R */

    /* Resolve the contact offset in the cue's own frame: "side" is horizontal,
     * square to the cue; "vert" is the remaining perpendicular. These are the
     * fractions of R the physics calls tip_side / tip_vert. */
    MoteVrV3 side = mv3_cross(mv3(0, 1, 0), c->axis);
    if (mv3_len(side) < 1e-4f) side = mv3(1, 0, 0);  /* cue pointing straight down */
    side = mv3_norm(side);
    MoteVrV3 vert = mv3_norm(mv3_cross(c->axis, side));

    c->tip_side = mv3_dot(off, side) / R;
    c->tip_vert = mv3_dot(off, vert) / R;

    /* Distance from the tip to the ball's surface, measured along the cue.
     * Negative once the tip is inside the ball, which is the moment of the
     * strike. */
    c->gap = t_enter;

    /* ---- the stroke ------------------------------------------------------ *
     * Closing speed along the cue's own axis. Taking it from the gap rather
     * than from the hand's velocity means a stroke delivered at an angle
     * contributes only what is actually going into the ball, which is what
     * makes a poor cue action play like a poor cue action. */
    if (c->have_prev && t->dt > 1e-5f) {
        float closing = (c->prev_gap - c->gap) / t->dt;
        c->speed = closing;
        if (c->prev_gap > 0.0f && c->gap <= 0.0f && closing > 0.05f) {
            out->struck   = 1;
            out->speed    = closing;
            out->tip_side = c->tip_side;
            out->tip_vert = c->tip_vert;
            out->elev     = c->elev;

            /* Aim in TABLE space: the physics knows nothing about the room. */
            float cs = cosf(-p->yaw), sn = sinf(-p->yaw);
            out->dir.x = c->aim_dir.x * cs - c->aim_dir.z * sn;
            out->dir.y = 0.0f;
            out->dir.z = c->aim_dir.x * sn + c->aim_dir.z * cs;

            /* A tip that lands too far off centre slides off the ball instead
             * of driving it. Real, and the reason nobody puts maximum side on
             * a power shot. */
            float r_off = sqrtf(c->tip_side * c->tip_side + c->tip_vert * c->tip_vert);
            if (r_off > CUEVR_MISCUE_LIMIT) {
                out->miscue = 1;
                out->speed *= 0.35f;                /* most of it is lost */
                out->tip_side *= 0.5f;
                out->tip_vert *= 0.5f;
            }
            c->have_prev = 0;                       /* one strike per approach */
            return;
        }
    } else {
        c->speed = 0.0f;
    }

    c->prev_gap = c->gap;
    c->have_prev = 1;
}
