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
/* A direction from table space out into the room — the CPU's aim, so its cue
 * can be laid along it. */
MoteVrV3 cuevr_table_dir_to_room(const CueVrPlacement *p, Vec3 v);
/* ...and the other way. Exported because open-coding it is how two separate
 * places ended up applying the TRANSPOSE — a rotation by -yaw instead of +yaw,
 * which is silently correct only when the table happens to be square to the
 * room. The stick walked the ball in hand mirrored about the table's axis, and
 * the colour nomination named whatever ball lay along a line 2*yaw away from
 * where the cue was actually pointing. */
MoteVrV3 cuevr_room_dir_to_table(const CueVrPlacement *p, MoteVrV3 v);

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
void cuevr_setup_adjust(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 cue_ball_room,
                        int allow_height);

/* How many (position, time) samples of the delivery to keep. Power is measured
 * over the longest run of forward motion inside this window, up to ~110 ms at
 * 72 Hz, which is most of a real delivery. */
/* Samples kept, and how many of them power is measured over. The window is
 * FIXED because a real delivery accelerates: a variable-length baseline reports
 * a different part of the stroke every time, which is a lottery, whereas a fixed
 * one always reports the arrival speed. Three is ~42 ms at 72 Hz — long enough
 * that tracking noise cannot dominate, short enough to be the speed the tip was
 * doing as it arrived rather than the average of the whole swing. */
#define CUEVR_SPEED_N       12
#define CUEVR_SPEED_WINDOW  3

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

    /* Where the bridge sits relative to the CENTRE of your left controller.
     * A stored 3D offset, nothing cleverer: hold the left side trigger, move
     * your hand, and the offset moves with it, so you place the cue where your
     * bridge actually is and it stays there.
     *
     * The first version subtracted the motion instead of adding it, on the idea
     * that a hand slides along a stationary cue. That inverts the control: the
     * instinctive gesture for "lift the cue" is to raise your hand, and raising
     * your hand drove the offset to ZERO — putting the cue through the middle of
     * the controller, every time, no matter how carefully it was aligned. It
     * also froze the drawn cue while the trigger was held, so none of it was
     * visible until you let go. Direct, and live, is what this wants to be. */
    MoteVrV3 rest;

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
    /* The floor under `elev`: the cue is a solid stick, so it cannot lie level
     * through a cushion or through a ball sitting behind the white. The app
     * fills this each frame from cue_table_min_elev (using the previous
     * frame's aim — one frame of lag at 72 Hz, and self-correcting), and the
     * update raises BOTH the played elevation and the drawn shaft to meet it.
     * Drawing the cue where the hand is while playing it elevated would show
     * the shaft passing through the rail, so the two move together. */
    float min_elev;
    int   elev_forced;   /* 1 while min_elev is what is actually in effect */
    MoteVrV3 aim_dir;    /* horizontal unit aim direction */

    /* stroke tracking */
    float prev_gap;
    int   have_prev;
    float speed;         /* closing speed along the axis (m/s) */
    /* The delivery, as (gap, elapsed) samples rather than per-frame speeds.
     * Dividing each frame's movement by that frame's dt and then averaging
     * multiplies the timing noise in; measuring one distance over one span of
     * time does not. */
    float gap_hist[CUEVR_SPEED_N];
    float t_hist[CUEVR_SPEED_N];
    int   speed_n;
    float t_accum;
    /* what the last strike was measured from — for the log, so a bad reading on
     * hardware can be diagnosed instead of theorised about */
    int   m_frames;
    float m_dist, m_time;
    MoteVrV3 prev_hand[2];   /* for sliding a hand along the cue */
    int   have_hand;
    int   struck;            /* this stroke has already made contact */
    int   adjusting;         /* a side trigger is held: the aim is held still */
    MoteVrV3 adj_axis;
} CueVrCue;

/* A real cue is 1.45 m. Fixed — a cue that stretches between your hands is the
 * fastest way to stop believing in it. */
#define CUEVR_CUE_LEN  1.45f
/* The leather tip's radius: contact is its surface, not a line. */
#define CUEVR_TIP_R    0.005f
#define CUEVR_REST_LIFT_DEFAULT 0.030f  /* a knuckle's worth of bridge */
#define CUEVR_REST_MAXLEN 0.30f         /* the cue stays within reach of the hand */
#define CUEVR_REST_MIN  -0.02f
#define CUEVR_REST_MAX   0.14f

#define CUEVR_GRIP_MIN 0.06f
#define CUEVR_GRIP_MAX 0.55f

void cuevr_cue_init(CueVrCue *c);

/* Player preferences that outlive a frame and a session: the cloth height they
 * matched to a real surface, the bridge they make, and everything they chose to
 * play with. Stored beside the app's own data; set the directory once at
 * start-up. A struct and a named-field file, because the positional argument
 * list this replaces had reached ten parameters of which seven were int. */
typedef struct {
    float    table_height;
    MoteVrV3 rest;
    float    grip;
    int      table_kind;
    int      ballset;
    int      persona;
    int      cloth;
    int      frame;      /* the frame COLOUR, from cue_theme.h */
    int      opp;
    int      cue;
    int      light;      /* the lighting rig — see cuevr_light.h */
    int      body;       /* the frame MODEL; -1 = whichever suits the table */
} CueVrPrefs;

void cuevr_prefs_dir(const char *dir);
void cuevr_prefs_defaults(CueVrPrefs *p);
void cuevr_prefs_load(CueVrPrefs *p);      /* leaves untouched what the file omits */
void cuevr_prefs_save(const CueVrPrefs *p);

/* A struck shot, as the physics wants it. */
typedef struct {
    int   struck;
    Vec3  dir;
    float speed;
    float tip_side, tip_vert, elev;
} CueVrShot;

/* Update the cue from the hands and decide whether it has just hit the ball.
 * `ball_room` is the cue ball centre in room space, R its radius. */
void cuevr_cue_update(CueVrCue *c, const MoteVrTracking *t,
                      const CueVrPlacement *p, MoteVrV3 ball_room, float R,
                      CueVrShot *out);

#endif /* CUEVR_H */
