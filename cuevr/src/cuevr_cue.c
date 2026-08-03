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

/* Room space <-> the table's own frame (metres, X length, Z width, Y up from
 * the cloth), which is the only frame the physics knows about.
 *
 * These MUST agree with the rotation the renderer's model matrix applies, and
 * for a long time they did not: table_to_room used the transpose, so with any
 * non-zero table yaw — and the table is always sited at a yaw taken from the
 * player's head direction — every ball was drawn in one place and tested in
 * another, mirrored across the yaw. The cue tip went through the ball it could
 * see and missed the ball it was tested against, and the on-ball indicator
 * never lit because it was telling the truth.
 *
 * The renderer builds its transform from a quaternion about +Y, which for a
 * point p gives  x' = x·cos + z·sin,  z' = -x·sin + z·cos.  That is the
 * definition these follow now, and test_cue.c checks them against a matrix
 * built the same way the renderer builds it rather than against my arithmetic. */
MoteVrV3 cuevr_table_to_room(const CueVrPlacement *p, Vec3 t) {
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(p->pos.x + t.x * c + t.z * s,
               p->pos.y + t.y,
               p->pos.z - t.x * s + t.z * c);
}

MoteVrV3 cuevr_room_to_table(const CueVrPlacement *p, MoteVrV3 r) {
    MoteVrV3 d = mv3_sub(r, p->pos);
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(d.x * c - d.z * s, d.y, d.x * s + d.z * c);
}

/* A direction only — the same rotation, no translation. */
static MoteVrV3 room_dir_to_table(const CueVrPlacement *p, MoteVrV3 v) {
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
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

    /* ---- the stroke ----------------------------------------------------- *
     * Hysteresis on the trigger, and a wide band of it: pull past 0.55 to take
     * hold, and it does not let go until you release below 0.15.
     *
     * A single threshold is what made the power a lottery. An index trigger is
     * analogue and your whole arm is moving during a delivery, so finger
     * pressure dips — and one dip below the threshold disarmed the stroke, the
     * next frame re-armed it, and re-arming resets the sample window, the clock
     * and the locked tip. The measurement restarted from nothing with a
     * one-frame baseline, at a random point in the delivery.
     *
     * And it was invisible. The cue is drawn from the same hand positions
     * either way, and the re-locked axis differs by however far the hands moved
     * in one frame, which is nothing. So the cue looked perfectly smooth while
     * the number behind it was being thrown away and rebuilt mid-stroke. */
    int want_stroke = c->stroking ? (Rh->trigger > 0.15f) : (Rh->trigger > 0.55f);
    if (want_stroke && !c->stroking) {
        c->stroking = 1;
        c->lock_axis = live_axis;
        c->lock_bridge = c->bridge;
        c->lock_butt0 = Rh->pose.p;
        c->lock_tip0 = mv3_add(Rh->pose.p,
                               mv3_scale(live_axis, CUEVR_CUE_LEN - c->grip));
        c->have_prev = 0;
        c->struck = 0;
        c->speed_n = 0;
        c->t_accum = 0.0f;
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
    /* Divide by REACH, not by R.
     *
     * `off` runs from the ball's centre to the point where the TIP's surface
     * meets it, so its length is R + the tip's radius — and dividing that by R
     * scaled every offset up by 19%. The physics reads these as a fraction of
     * the ball's radius, so it saw contacts at 1.19 where the ball ends at 1.0,
     * and the consequences were both severe: the drive term is
     * sqrt(1 - side^2 - vert^2), which clamps to ZERO once the pair exceeds one,
     * so a merely off-centre contact gave the ball full spin and no speed at all
     * — the tip visibly striking and the ball barely leaving. And the miscue
     * threshold of 0.55 was tripping at a true offset of 0.46, docking two
     * thirds of the power off ordinary side and screw.
     *
     * Normalised by reach, dead centre is 0 and the edge of the ball is 1, which
     * is what tip_side and tip_vert are defined to mean. */
    c->tip_side = mv3_dot(off, side) / reach;
    c->tip_vert = mv3_dot(off, vert) / reach;
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
    /* Sample from the FIRST frame of the stroke, not the second. Waiting for a
     * previous frame leaves a two-frame delivery — a hard shot played from close
     * to the ball — with a single sample and therefore no baseline to measure
     * over at all, so it silently would not fire. */
    if (c->stroking && !c->struck && t->dt > 1e-5f) {
        /* Power from a SMOOTHED stroke speed, not from one frame of it.
         *
         * A single frame at 72 Hz is 14 ms, and a millimetre of tracking noise
         * over that reads as 0.07 m/s — so a difference of two or three
         * millimetres between consecutive frames is the difference between a
         * safety and a power shot. That is what made the power feel random. A
         * 35 ms attack averages two or three frames: fast enough to follow a
         * real delivery, slow enough that jitter cannot dominate it. */
        /* Power: one distance over one span of time.
         *
         * Every previous attempt computed a speed per frame — this frame's
         * movement divided by this frame's dt — and then combined those. That
         * multiplies the frame-timing noise straight into the answer: a
         * predicted display time that lands a millisecond early, or a dropped
         * frame that reports one interval's dt for two intervals' motion, and
         * the same physical stroke reads as a tap or a smash. At 72 Hz a dt
         * that is 30% out is a power reading 30% out, and dt at the head of a
         * stroke is exactly where a runtime's prediction is least settled.
         *
         * So keep the raw (gap, elapsed) samples and take a single finite
         * difference across the longest run of FORWARD motion in the window:
         * total distance travelled divided by the total time it took. Individual
         * dt errors cancel because the same frames' times are summed, and a
         * ~100 ms baseline over a real delivery leaves tracking jitter nowhere
         * to hide. Walking back only while the motion is forward means the
         * baseline stops at the turnaround, so a backswing cannot dilute it.
         */
        c->t_accum += t->dt;
        for (int i = CUEVR_SPEED_N - 1; i > 0; i--) {
            c->gap_hist[i] = c->gap_hist[i-1];
            c->t_hist[i]   = c->t_hist[i-1];
        }
        c->gap_hist[0] = c->gap;
        c->t_hist[0]   = c->t_accum;
        if (c->speed_n < CUEVR_SPEED_N) c->speed_n++;

        /* The start of the delivery is the FURTHEST BACK the cue went inside the
         * window — the largest gap. Not "walk back while each step is forward":
         * a millimetre of tracking wobble makes one step non-monotonic and
         * truncates the baseline to two or three frames at random, which is
         * precisely how the same stroke came out as a tap or a smash. Taking the
         * maximum is immune to that and finds the turnaround of the backswing on
         * its own.
         *
         * Ties resolve to the NEWEST of the equal samples, and they have to: the
         * gap is constant while you address the ball, so the window fills with
         * equal values, and reaching past them would drag all that stationary
         * time into the baseline and dilute the speed towards zero. The cost is
         * that the first frame of a delivery has only itself to measure over —
         * the baseline lengthens as the stroke proceeds — so a shot that connects
         * within a frame or two of leaving the address is measured over a short
         * span. The log reports that span for exactly this reason. */
        /* Measure over a FIXED short baseline ending at contact — the speed the
         * tip was doing as it arrived — not from the start of the delivery.
         *
         * This is the whole bug. A real delivery ACCELERATES, and once it does,
         * "average over the delivery so far" and "speed at contact" are
         * different numbers; which one you get depends on how many frames happen
         * to be in the window, and that varied with where the contact landed and
         * with anything that reset the window. Same stroke, different answer,
         * every time. Every test I wrote used a constant-velocity delivery, which
         * measures identically over any baseline, so all of them passed while the
         * thing was a lottery in the hand.
         *
         * A fixed window is consistent by construction: it always reports the
         * same part of the stroke. Three samples is ~42 ms — long enough that
         * tracking noise cannot dominate, short enough to be the arrival speed
         * rather than the whole swing. */
        /* The speed at CONTACT, from a second-order backward difference.
         *
         * A trailing average — over the delivery, or over a fixed window — is
         * biased low by however much the cue accelerated inside it, and that
         * bias grows with the stroke: soft shots came out right and hard ones
         * came out at three quarters. Every earlier test missed it because they
         * all delivered at constant velocity, which has no acceleration to be
         * biased by and measures identically over any baseline.
         *
         *   v ≈ (-3·g0 + 4·g1 - g2) / 2h
         *
         * is exact for constant acceleration, so it reports the arrival speed
         * rather than the average of the swing, and being a three-point estimate
         * it still averages tracking noise. Sampled two frames apart so h is
         * wide enough that noise does not get amplified by the differencing. */
        const int SP = 2;                    /* frames between the three samples */
        int start = 2 * SP;
        float dgap, dtime;
        if (c->speed_n > 2 * SP) {
            float g0 = c->gap_hist[0], g1 = c->gap_hist[SP], g2 = c->gap_hist[2*SP];
            float h = (c->t_hist[0] - c->t_hist[2*SP]) * 0.5f;
            /* Only when the whole span is forward motion; a backswing inside it
             * would make the fit meaningless. */
            if (h > 1e-4f && g1 > g0 && g2 > g1) {
                c->speed = (-3.0f * g0 + 4.0f * g1 - g2) / (2.0f * h);
                dgap = g2 - g0; dtime = h * 2.0f;
            } else {
                start = 1;
                dgap = c->gap_hist[1] - g0;
                dtime = c->t_hist[0] - c->t_hist[1];
                c->speed = dtime > 1e-4f ? dgap / dtime : 0.0f;
            }
        } else {
            start = c->speed_n > 1 ? 1 : 0;
            dgap = c->gap_hist[start] - c->gap_hist[0];
            dtime = c->t_hist[0] - c->t_hist[start];
            c->speed = (dtime > 1e-4f && dgap > 0.0f) ? dgap / dtime : 0.0f;
        }
        if (c->speed < 0.0f) c->speed = 0.0f;
        c->m_frames = start;
        c->m_dist = dgap;
        c->m_time = dtime;

        if (c->speed_n >= 2 && c->gap <= 0.0f && c->speed > 0.12f) {
            c->struck = 1;
            out->struck   = 1;
            out->speed    = c->speed;
            out->tip_side = c->tip_side;
            out->tip_vert = c->tip_vert;
            out->elev     = c->elev;
            MoteVrV3 td = room_dir_to_table(p, c->aim_dir);
            out->dir.x = td.x;
            out->dir.y = 0.0f;
            out->dir.z = td.z;
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
