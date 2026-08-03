/*
 * CueVR — ThumbyCue on a headset: a real table, in your room, with a real cue.
 *
 * A separate app from Mote VR (its own APK, its own icon, its own applicationId)
 * that shares two things with it: the OpenXR host in platform/xr, and — the
 * reason this is worth doing at all — the billiard physics that already exists.
 *
 * games/thumbycue/src is a 128x128 handheld game, but its guts were written in
 * SI units against real tables: cue_physics.c integrates at 2 kHz with full
 * three-axis angular velocity per ball, impulse collisions with Coulomb
 * friction (so throw and cushion english fall out rather than being faked), and
 * a strike entry point that already takes a tip offset and a cue elevation:
 *
 *     cue_phys_strike_elev(w, ball, dir, speed, tip_side, tip_vert, elev)
 *
 * That is exactly the call a two-handed VR cue wants to make. cue_table.c has
 * seven real tables from a 7 ft pub table to a 12 ft snooker table with true
 * cushion, jaw and pocket geometry; cue_rules.c has 8-ball, 9-ball and snooker
 * including the UK two-shot carry, push-out, free ball and foul-and-a-miss;
 * cue_ai.c is an opponent. All four are pure C with no engine dependency, so
 * they compile into this app unchanged.
 *
 * What is new here is only what a headset needs:
 *
 *   cuevr_cue.c     two controllers -> a cue -> a strike. Pure maths, tested.
 *   cuevr_setup.c   putting the table in your room, and at the height of the
 *                   real surface you are standing at. Pure maths, tested.
 *   cuevr_render.c  GLES3: cloth, rails, pockets, balls, cue, HUD.
 *   cuevr_app.c     the four callbacks platform/xr calls, and the game flow.
 */
#ifndef CUEVR_H
#define CUEVR_H

#include "mote_xr.h"
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_rules.h"

/* The physics works in table space: metres, X along the length, Z across, Y up
 * from the cloth at Y=0. The table then sits in the room under a pose. Keeping
 * those two apart is what lets you pick the table up and put it on your actual
 * kitchen table without the physics ever knowing. */
typedef struct {
    MoteVrV3 pos;        /* where the centre of the cloth is, in room space */
    float    yaw;        /* rotation about vertical */
    float    height;     /* cloth height above the room floor (== pos.y) */
} CueVrPlacement;

/* Converting between the two. */
MoteVrV3 cuevr_table_to_room(const CueVrPlacement *p, Vec3 t);
MoteVrV3 cuevr_room_to_table(const CueVrPlacement *p, MoteVrV3 r);

/* ---- setup ------------------------------------------------------------- *
 * Every session starts here, because the point of playing in passthrough is
 * that the cloth lands on a surface you can actually lean on — a real table,
 * a desk, a bed. Height is the whole game, so it is a control of its own and
 * it is shown in centimetres while you set it.
 *
 * Both sticks work about the CUE BALL rather than about the table's centre:
 * the cue ball is where you are standing and what you are about to hit, so
 * keeping it still while the table swings and slides underneath is what lets
 * you bring any shot on the table to where your body already is. */
typedef struct {
    CueVrPlacement place;
    int   active;             /* in setup rather than playing */
    int   confirmed;          /* has been through setup at least once */
    float last_height;        /* remembered across re-entry within a session */
} CueVrSetup;

void cuevr_setup_init(CueVrSetup *s, float floor_y);

/* One frame of setup. `cue_ball_room` is where the cue ball currently is, in
 * room space, and is the pivot for everything. Returns 1 while setup should
 * continue, 0 once the player has confirmed. */
int  cuevr_setup_update(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 cue_ball_room);

/* Just the sticks — slide, turn about the cue ball, height. Live during
 * setup and during aiming, because the shot has to be brought to you. */
void cuevr_setup_adjust(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 cue_ball_room);

/* ---- the cue ------------------------------------------------------------ *
 * "Natural" cueing, as in Unlimited Snooker: the left hand is the bridge the
 * cue rests on and the right hand is the butt. The two together are the cue —
 * its direction is the line between them, so raising your back hand elevates
 * the cue exactly as it does on a real table, and the tip goes where the line
 * goes. Nothing is snapped to the ball and no aim line is drawn for you.
 *
 * The stroke is a real stroke: the tip's closing speed along its own axis at
 * the moment it reaches the ball. Pull back and push through. */
typedef struct {
    /* Where your grip hand sits on the butt, measured from the butt end. This —
     * not a fixed bridge-to-tip length — is what decides how much cue is in
     * front of the bridge, exactly as on a real cue: the butt end is behind
     * your grip hand, the tip is CUE_LEN away from it, and your bridge hand is
     * wherever you have put it in between. Hold the side trigger and slide your
     * hand along the cue to change it. */
    float grip;

    /* The stroke. Pulling the right trigger locks the aim and hands the cue to
     * your grip hand: the bridge becomes a fixed pivot and the cue slides
     * through it, forward and back, as your back hand moves. That is what a
     * cue action is — the bridge does not move during the delivery. */
    int      stroking;
    MoteVrV3 lock_axis;      /* aim, frozen at the moment the trigger went down */
    MoteVrV3 lock_tip0;      /* where the tip was then */
    MoteVrV3 lock_butt0;     /* and where the grip hand was */
    MoteVrV3 lock_bridge;

    /* live, recomputed each frame */
    MoteVrV3 butt, bridge, tip;
    MoteVrV3 axis;       /* unit, butt -> tip */
    int   tracked;       /* both hands are held apart: there is a cue */
    int   on_ball;       /* the cue line actually meets the cue ball */
    float gap;           /* tip to ball surface along the axis (m), <0 = through */
    float tip_side;      /* contact offset, fractions of R (+ = right english) */
    float tip_vert;      /* + = top/follow, - = bottom/draw */
    float elev;          /* radians above horizontal (butt raised) */
    MoteVrV3 aim_dir;    /* horizontal unit aim direction */

    /* stroke tracking */
    float prev_gap;
    int   have_prev;
    float speed;         /* closing speed along the axis (m/s) */
    MoteVrV3 prev_hand[2];   /* for sliding a hand along the cue */
    int   have_hand;
    int   adjusting;         /* a side trigger is held: the aim is held still */
    MoteVrV3 adj_axis;
} CueVrCue;

/* A real cue is 1.45 m. Fixed — a cue that stretches between your hands is the
 * fastest way to stop believing in it. */
#define CUEVR_CUE_LEN  1.45f
#define CUEVR_GRIP_MIN 0.06f
#define CUEVR_GRIP_MAX 0.55f

void cuevr_cue_init(CueVrCue *c);

/* A struck shot, as the physics wants it. */
typedef struct {
    int   struck;
    Vec3  dir;
    float speed;
    float tip_side, tip_vert, elev;
    int   miscue;        /* contact too far off centre — a real cue would slip */
} CueVrShot;

/* Update the cue from the hands and decide whether it has just hit the ball.
 * `ball_room` is the cue ball centre in room space, R its radius. */
void cuevr_cue_update(CueVrCue *c, const MoteVrTracking *t,
                      const CueVrPlacement *p, MoteVrV3 ball_room, float R,
                      CueVrShot *out);

/* Beyond this fraction of the radius a real tip slides off the ball. */
#define CUEVR_MISCUE_LIMIT 0.55f

#endif /* CUEVR_H */
