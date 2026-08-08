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
/* What build this is, printed on the menu.
 *
 * Two headsets that disagree about the rules or the physics play two different
 * frames, and the only way anyone can answer "are we on the same build?" is if
 * the build says so where they can both see it. Keep in step with versionName
 * in cuevr/app/build.gradle. */
#define CUEVR_VERSION "0.2"

/* How long B is held to take a practice shot back. Long enough that a knuckle
 * brushing it cannot rewind the table, short enough that it is not a chore
 * when you are playing the same shot for the tenth time. */
#define CUEVR_UNDO_HOLD 0.6f

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

/* Mirror the setup controls for a left-hander — see cuevr_cue_left_handed. */
void cuevr_setup_left_handed(int on);

/* Stick layout: which stick slides and which turns, and which way round each
 * goes. Taste, so it is a setting and not a decision made here. */
void cuevr_setup_sticks(int swap, int inv_slide, int inv_turn);

/* One frame of setup. `cue_ball_room` is where the cue ball currently is, in
 * room space, and is the pivot for everything. Returns 1 while setup should
 * continue, 0 once the player has confirmed. */
int  cuevr_setup_update(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 cue_ball_room);

/* Just the sticks — slide, turn about the cue ball, height. Live during
 * setup and during aiming, because the shot has to be brought to you. */
void cuevr_setup_adjust(CueVrSetup *s, const MoteVrTracking *t, MoteVrV3 cue_ball_room,
                        int allow_height);

/* Turn the table about a point that must stay put. Shared, because getting the
 * sense of this rotation wrong leaves the "fixed" point orbiting instead — and
 * that has happened once in each place that open-coded it. */
void cuevr_setup_yaw_about(CueVrSetup *s, MoteVrV3 pivot_room, float dyaw);

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
    /* IN THE LEFT CONTROLLER'S OWN FRAME, not the room's — see cuevr_cue.c.
     * Bolted to the controller, so the bridge is always that controller plus a
     * fixed few centimetres and nothing else can move it. */
    MoteVrV3 rest;
    MoteVrV3 rest_world;   /* the same offset in room axes, for this frame */

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
    float lock_elev;     /* min_elev as it was at trigger-down, held for the stroke */
    int   elev_forced;   /* 1 while the forced angle is what is actually in effect */
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
    /* Where the bridge WAS when the reposition began. The offset is derived
     * from this absolutely each frame rather than integrated, so tracking
     * noise cannot accumulate into it. */
    MoteVrV3 adj_bridge0;
    int      adj_have0;
    MoteVrV3 prev_hand[2];   /* for sliding a hand along the cue */
    int   have_hand;
    int   struck;            /* this stroke has already made contact */
    int   adjusting;         /* a side trigger is held: the aim is held still */
    MoteVrV3 adj_axis;
} CueVrCue;

/* A real cue is 1.45 m. Fixed — a cue that stretches between your hands is the
 * fastest way to stop believing in it. */
/* EYE RENDER SCALE — multiplies the runtime's recommended eye resolution, so
 * it multiplies fragment work by its square. 1.25 was the long-standing value
 * (1.56x the fragments of 1.0), bought for text crispness; 1.10 is 1.21x and
 * the cheapest real GPU saving available. Quest Games Optimizer can override
 * this per app, so treat it as the app's own default rather than the last
 * word. One number, one rebuild. */
#define CUEVR_RENDER_SCALE 1.25f

#define CUEVR_CUE_LEN  1.45f
/* The leather tip's radius: contact is its surface, not a line. */
#define CUEVR_TIP_R    0.005f
#define CUEVR_REST_LIFT_DEFAULT 0.030f  /* a knuckle's worth of bridge */
/* How far the cue may sit from the bridge hand. 150 mm, not 300.
 *
 * A bridge offset is the gap between a controller's centre and where the shaft
 * lies across your hand — a few centimetres, and never a hand-span. 300 mm was
 * not a safety rail, it was a cliff: the bridge point is a LEVER, so with the
 * hands ~500 mm apart and the tip 1250 mm beyond, a 300 mm offset throws the
 * TIP about 700 mm out of place. The cue was not slightly off, it was in
 * another part of the room. */
#define CUEVR_REST_MAXLEN 0.15f
#define CUEVR_REST_MIN  -0.02f
#define CUEVR_REST_MAX   0.14f

#define CUEVR_GRIP_MIN 0.06f
/* 0.97 m from the butt, which is two thirds up a 1.45 m cue.
 *
 * It was 0.55 — a shade under halfway — and half a cue is not far enough for
 * the shots this exists for: reaching over a ball, cueing off the rail, playing
 * from under a cushion. A player shortens right up for those, and the limit was
 * stopping them at exactly the point the technique starts being useful. */
#define CUEVR_GRIP_MAX 0.97f

void cuevr_cue_init(CueVrCue *c);

/* Player preferences that outlive a frame and a session: the cloth height they
 * matched to a real surface, the bridge they make, and everything they chose to
 * play with. Stored beside the app's own data; set the directory once at
 * start-up. A struct and a named-field file, because the positional argument
 * list this replaces had reached ten parameters of which seven were int. */
/* Snooker table sizes and pool games that carry their own record. */
/* 20+, 30+, 50+, 100+ */
#define CUEVR_BRK_TIERS 4
#define CUEVR_STAT_SNK  3
#define CUEVR_STAT_POOL 4
int cuevr_stat_snk_slot(int kind);    /* CueGameKind -> 0..2, or -1 */
int cuevr_stat_pool_slot(int kind);   /* CueGameKind -> 0..3, or -1 */
const char *cuevr_stat_snk_name(int slot);
const char *cuevr_stat_pool_name(int slot);
const char *cuevr_stat_table_name(int kind);

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

    /* Where the drawn controller sits relative to the pose the runtime reports,
     * as a translation in the GRIP frame (metres) and an XYZ rotation (degrees).
     * Zero means "exactly on the reported pose", which is what it should be and
     * often is not: the baked fallback models come from somebody else's
     * measurement of somebody else's controller, and even the runtime's own
     * model can sit off if the pose being drawn at is not the one it was
     * authored against. Nobody can derive this — it has to be looked at with a
     * real controller in a real hand, so it is adjusted in the headset and kept
     * here. */
    float    ctrl_pos[3];
    float    ctrl_rot[3];

    /* ---- what you have actually done ----------------------------------- *
     * Kept beside the settings because they persist for the same reason and
     * through the same file: a highest break nobody recorded is a highest
     * break that did not happen.
     *
     * Indexed [table][mode]. Snooker holds the best break on each SIZE of
     * table, because a 147 on a 7 ft bed and a 147 on a 12 ft are not the same
     * thing and averaging them into one number tells you neither. Pool holds
     * the count of frames won in a single visit from a full table — the run
     * out — because pool has no break to speak of and clearing is the thing
     * worth counting.
     *
     * mode: 0 = against the CPU, 1 = against another player. Practice is not
     * recorded at all: a table you can rearrange is not a table you can set a
     * record on. */
    int      snk_best[CUEVR_STAT_SNK][2];    /* 6-red, 10-red, 12 ft */
    int      pool_clear[CUEVR_STAT_POOL][2]; /* UK8, US8, 9-ball, Chinese 8 */
    int      frames_won[2];
    int      frames_played[2];
    int      lefty;          /* bridge with the right hand */
    int      cue_spots;      /* the white's measles spots */
    /* PRACTICE, SNOOKER: put every potted colour straight back on its spot
     * while reds remain, so a practice table never strips itself down to a few
     * reds and nothing to follow them with. */
    int      prac_respot;
    int      surround;       /* 0 passthrough, 1 dark room, 2 arena */
    /* Best six-ball clearance, in hundredths of a second, per table kind.
     * 0 = never done. Per table because a 12 ft snooker table and a 7 ft pub
     * table are not the same challenge and one record for both is meaningless. */
    int      mini_best[CUE_GAME_COUNT];
    /* How many breaks of 20, 30, 50 and 100 you have made (vs CPU / online).
     * The COUNT, not the best — somebody who has made forty fifties is a
     * different player from somebody who made one lucky one, and a highest
     * break alone cannot tell them apart. */
    int      brk_tier[CUEVR_BRK_TIERS][2];
    /* The pocket cut, as tuned in the headset. Radii in percent of the ball's
     * drop circle; setbacks in millimetres into the frame. */
    int      cut_cr, cut_cs, cut_mr, cut_ms;
    int      stick_swap;     /* turn on the left stick, slide on the right */
    int      inv_slide, inv_turn;
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
/* LEFT-HANDED. A left-hander bridges with the RIGHT hand and grips with the
 * left — the mirror image of everything below, and the only thing that has to
 * change to get it. Set once from the menu; the cue then reads the hands the
 * other way round and nothing else in the game knows or cares. */
void cuevr_cue_left_handed(int on);

void cuevr_cue_update(CueVrCue *c, const MoteVrTracking *t,
                      const CueVrPlacement *p, MoteVrV3 ball_room, float R,
                      CueVrShot *out);

#endif /* CUEVR_H */
