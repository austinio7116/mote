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
    /* A snooker player grips maybe 20 cm up from the butt end. With the bridge
     * hand the usual 85-95 cm in front of that, the tip lands ~30 cm past the
     * bridge — which is where it should be. */
    c->grip = 0.20f;
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

    const MoteVrHand *Lh = &t->hand[MOTE_VR_LEFT];    /* bridge */
    const MoteVrHand *Rh = &t->hand[MOTE_VR_RIGHT];   /* grip on the butt */
    if (!Lh->tracked || !Rh->tracked) {
        c->tracked = c->on_ball = c->have_prev = c->have_hand = c->stroking = 0;
        return;
    }

    c->bridge = Lh->pose.p;

    /* Aim: the line from your grip hand through your bridge hand. Once the
     * stroke is under way it is frozen — the bridge is a pivot during a
     * delivery, not a steering wheel. */
    MoteVrV3 along = mv3_sub(c->bridge, Rh->pose.p);
    if (mv3_len(along) < 0.10f && !c->stroking) {
        c->tracked = c->on_ball = c->have_prev = 0;
        return;
    }
    c->tracked = 1;
    MoteVrV3 live_axis = mv3_len(along) > 1e-4f ? mv3_norm(along) : mv3(1, 0, 0);

    /* ---- sliding a hand along the cue ----------------------------------- *
     * Either side trigger, and each hand does the thing that hand does. The
     * shared rule is the physical one: sliding your hand ALONG a cue does not
     * move the cue, so while a side trigger is held the aim is pinned and only
     * the hand travels.
     *
     *   right (grip)   changes how much cue is in front of the bridge — this is
     *                  how you shorten up for a tight shot.
     *   left (bridge)  repositions your bridge along the shaft without steering,
     *                  which is otherwise impossible: the bridge hand IS the aim,
     *                  so moving it normally swings the cue.
     *
     * The first version accepted either trigger but only ever measured the RIGHT
     * hand's motion, so squeezing the left did nothing at all. */
    int adj_r = Rh->squeeze > 0.5f && !c->stroking;
    int adj_l = Lh->squeeze > 0.5f && !c->stroking;
    int adjusting = adj_r || adj_l;
    if (adjusting && !c->adjusting) c->adj_axis = live_axis;
    if (adjusting && c->have_hand && adj_r) {
        float d = mv3_dot(mv3_sub(Rh->pose.p, c->prev_hand[MOTE_VR_RIGHT]), c->adj_axis);
        c->grip += d;
        if (c->grip < CUEVR_GRIP_MIN) c->grip = CUEVR_GRIP_MIN;
        if (c->grip > CUEVR_GRIP_MAX) c->grip = CUEVR_GRIP_MAX;
    }
    c->adjusting = adjusting;
    c->prev_hand[MOTE_VR_LEFT]  = Lh->pose.p;
    c->prev_hand[MOTE_VR_RIGHT] = Rh->pose.p;
    c->have_hand = 1;
    if (adjusting) live_axis = c->adj_axis;   /* the cue holds still while you slide */

    /* ---- the stroke ----------------------------------------------------- */
    int want_stroke = Rh->trigger > 0.45f;
    if (want_stroke && !c->stroking) {
        c->stroking = 1;
        c->lock_axis = live_axis;
        c->lock_bridge = c->bridge;
        c->lock_butt0 = Rh->pose.p;
        c->lock_tip0 = mv3_add(Rh->pose.p,
                               mv3_scale(live_axis, CUEVR_CUE_LEN - c->grip));
        c->have_prev = 0;
        c->struck = 0;
    } else if (!want_stroke) {
        c->stroking = 0;
    }

    if (c->stroking) {
        /* Only motion ALONG the cue counts. Wobble across it is not delivery,
         * and a real bridge would absorb it. */
        c->axis = c->lock_axis;
        float travel = mv3_dot(mv3_sub(Rh->pose.p, c->lock_butt0), c->lock_axis);
        c->tip = mv3_add(c->lock_tip0, mv3_scale(c->lock_axis, travel));
        c->butt = mv3_sub(c->tip, mv3_scale(c->lock_axis, CUEVR_CUE_LEN));
        c->bridge = c->lock_bridge;
    } else {
        c->axis = live_axis;
        c->tip  = mv3_add(Rh->pose.p, mv3_scale(c->axis, CUEVR_CUE_LEN - c->grip));
        c->butt = mv3_sub(c->tip, mv3_scale(c->axis, CUEVR_CUE_LEN));
    }

    /* Elevation is the cue's own tilt: the axis runs butt -> tip, so cueing
     * down on the ball (butt raised) points it below horizontal, and the
     * physics wants that as a positive angle. */
    float ay = c->axis.y < -1.0f ? -1.0f : (c->axis.y > 1.0f ? 1.0f : c->axis.y);
    c->elev = -ay > 0.0f ? asinf(-ay) : 0.0f;

    MoteVrV3 flat = mv3(c->axis.x, 0.0f, c->axis.z);
    c->aim_dir = mv3_len(flat) > 1e-4f ? mv3_norm(flat) : mv3(1, 0, 0);

    /* ---- where the line meets the ball ---------------------------------- */
    /* The tip is a 5 mm object, so contact is its surface against the ball's,
     * not an infinitely thin line through the ball's centre. It widens the
     * target by the tip's own radius — which is what a real tip does — and it is
     * the physically right test regardless. */
    MoteVrV3 to_ball = mv3_sub(ball_room, c->tip);
    float along_axis = mv3_dot(to_ball, c->axis);
    float perp2 = mv3_dot(to_ball, to_ball) - along_axis * along_axis;
    float reach = R + CUEVR_TIP_R;
    float r2 = reach * reach;

    if (perp2 > r2) {
        /* Missing the ball is the normal state while you line up, so the cue
         * still exists and is still drawn — only the strike is unavailable. */
        c->on_ball = 0;
        c->have_prev = 0;
        c->gap = along_axis - reach;
        return;
    }
    c->on_ball = 1;

    float half_chord = sqrtf(r2 - perp2);
    float t_enter = along_axis - half_chord;
    MoteVrV3 hit = mv3_add(c->tip, mv3_scale(c->axis, t_enter));
    MoteVrV3 off = mv3_sub(hit, ball_room);

    MoteVrV3 side = mv3_cross(mv3(0, 1, 0), c->axis);
    if (mv3_len(side) < 1e-4f) side = mv3(1, 0, 0);
    side = mv3_norm(side);
    MoteVrV3 vert = mv3_norm(mv3_cross(c->axis, side));
    c->tip_side = mv3_dot(off, side) / R;
    c->tip_vert = mv3_dot(off, vert) / R;
    c->gap = t_enter;

    /* Contact.
     *
     * Only a stroke can play a shot — shuffling about while lining up must never
     * send the ball away — and a stroke plays at most one.
     *
     * The condition is "at or inside the surface, moving forward" rather than
     * "crossed from outside to inside this frame". The stricter form also fails
     * when the tip is already touching at the moment the trigger goes down,
     * which is how you address a ball, and there is no reason to require a
     * backswing that the player may already have made. */
    if (c->stroking && !c->struck && c->have_prev && t->dt > 1e-5f) {
        float closing = (c->prev_gap - c->gap) / t->dt;
        c->speed = closing;
        if (c->gap <= 0.0f && closing > 0.12f) {
            c->struck = 1;
            out->struck   = 1;
            out->speed    = closing;
            out->tip_side = c->tip_side;
            out->tip_vert = c->tip_vert;
            out->elev     = c->elev;
            float cs = cosf(-p->yaw), sn = sinf(-p->yaw);
            out->dir.x = c->aim_dir.x * cs - c->aim_dir.z * sn;
            out->dir.y = 0.0f;
            out->dir.z = c->aim_dir.x * sn + c->aim_dir.z * cs;
            float r_off = sqrtf(c->tip_side * c->tip_side + c->tip_vert * c->tip_vert);
            if (r_off > CUEVR_MISCUE_LIMIT) {
                out->miscue = 1;
                out->speed *= 0.35f;
                out->tip_side *= 0.5f;
                out->tip_vert *= 0.5f;
            }
            return;
        }
    } else if (!c->stroking) {
        c->speed = 0.0f;
    }
    c->prev_gap = c->gap;
    c->have_prev = c->stroking;
}
