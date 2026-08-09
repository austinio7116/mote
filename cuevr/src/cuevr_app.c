/*
 * CueVR — the game itself.
 *
 * The four callbacks platform/xr calls, and the flow between them. Everything
 * that makes it a game rather than a tech demo is borrowed wholesale from the
 * handheld: cue_table lays the rack, cue_physics runs the shot, cue_rules
 * scores it and calls the fouls, cue_ai plays the other side. This file is the
 * wiring, the HUD and the menu.
 *
 * The state machine is small on purpose:
 *
 *   MENU    pick one of the seven tables
 *   SETUP   put it in your room, at the height of a real surface
 *   AIM     your shot — the cue is live, the physics is asleep
 *   ROLL    the balls are running; nothing you do matters until they stop
 *   THINK   the CPU is planning (resumably, so the frame never stalls)
 *   OVER    frame finished
 *
 * Holding the left menu button for a second drops back into SETUP from
 * anywhere, because the first thing anyone does with a table in their room is
 * decide it is in the wrong place.
 */
#include "cuevr.h"
#include "cuevr_render.h"
#include "cuevr_frame.h"
#include "cuevr_career.h"
#include "cuevr_drill.h"
#include "cuevr_text.h"
#include "cuevr_font_md.h"
#include "cuevr_font_lg.h"
#include "cuevr_font_xl.h"
#include "cuevr_net.h"
#include <SDL.h>   /* the VR compat shim: threads only, no video */
#include "cue_theme.h"
#include "cue_ai.h"
#include "cue_render.h"
#include "craft_font.h"
#include "cue_audio.h"
#include "cuevr_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv, free — the render-model poll */
#include <string.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
/* Through the host's logger so logcat and the SideQuest-readable file both get
 * it, in one order. */
/* Off in a normal build — see MOTE_LOG in platform/xr/mote_xr.c. The sink
 * returns immediately anyway; this stops the arguments being assembled too. */
#if defined(MOTE_LOG) && MOTE_LOG
#define LOGI(...) mote_xr_logv(__VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif
#else
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

/* The handheld's ceiling was 8.5 m/s, which is what its power bar reached at
 * 100%. In VR you are not moving a bar, you are moving an arm: a firm stroke
 * puts the tip through at 6-7 m/s, which becomes 8-9 m/s of ball, so every
 * committed shot was landing on the clamp and coming out identical. Raised
 * enough that only a genuinely violent swing reaches it. */
#define MAX_STRIKE_SPEED 12.0f

/* Cloth height above the floor. A match table is 2 ft 10 in; a pub table is
 * near enough the same. The player adjusts it in setup to match whatever real
 * surface they are standing at. */
#define CUEVR_TABLE_HEIGHT 0.85f

/* Cue tip speed -> ball speed.
 *
 * A one-dimensional impact of a cue (mass M, speed V) on a stationary ball
 * (mass m) with restitution e leaves the ball at
 *
 *     v = V * M(1 + e) / (M + m)
 *
 * A leather tip on phenolic measures e = 0.71-0.75 (drdavepoolinfo.com's
 * property tables), and a cue is 17-19 oz. That works out at about 1.33 for a
 * 5.5 oz pool ball and 1.35 for a lighter snooker ball — so the ball always
 * leaves faster than the tip arrives, and passing the tip's speed straight
 * through as the ball's, which is what this did, under-read every shot by a
 * third. Derived from the table's own ball mass rather than hard-coded, since
 * snooker and pool balls differ and CueTable knows which is on the cloth. */
#define CUEVR_CUE_MASS 0.52f    /* 18 oz */
#define CUEVR_TIP_E    0.73f    /* leather on phenolic */

/* A feel trim on top of the derived transfer, and labelled as exactly that: the
 * physics above says how fast the ball leaves, but how hard a stroke FEELS in a
 * headset depends on how much your arm is really moving, and a centre-ball hit
 * was landing slightly heavy. One number, no pretence that it is derived. Taken
 * from 0.90 to 0.82 to 0.70 after play, each time on the same report: still
 * heavy. A real stroke is slower than it feels — in a headset your arm swings
 * freely with no cue weight and no cloth to push against, so the tip arrives
 * faster than the same-looking stroke would on a table. */
#define CUEVR_POWER_TRIM 0.70f

enum { ST_MENU = 0, ST_SETUP, ST_AIM, ST_ROLL, ST_THINK, ST_CPUCUE, ST_PLACE,
       ST_DECIDE, ST_OVER, ST_PAUSE, ST_LOBBY, ST_APPEAR, ST_STATS,
       ST_CONTROLS,
       /* Career: choosing the season's tables, the hub you come back to
        * between matches, a league table, and what you have won. */
       ST_CARSETUP, ST_CAREER, ST_CARTABLE, ST_CARACH,
       /* Choosing the cloth off a grid of swatches, which is how a
        * cloth is chosen: twenty-three names cycled one at a time is
        * a list you have to remember rather than a card you look at. */
       ST_CLOTH,
       /* Practice: the list of saved drills, and the editor where you set the
        * balls out by hand. */
       ST_DRILLS, ST_LAYOUT, ST_DRILLSET,
       /* Tuning the pocket cut with a ball on the table, which is the only
        * place it can honestly be judged. */
       ST_POCKETS };

/* Who you are playing. PRACTICE is not "vs nobody" — it is its own mode: the
 * table never changes hands, so a missed pot leaves you to carry on, and undo is
 * available because the whole point is to play the same shot again. */
/* CAREER is a way of ARRANGING matches, not a kind of opponent: a career match
 * is played against the CPU like any other, so it sets S.opp to OPP_CPU and
 * remembers where the result has to go. Keeping it out of this enum keeps every
 * `S.opp == OPP_CPU` test in the file correct for career play — records,
 * concede, undo, the lot — instead of needing a second case each. */
/* CHALLENGES IS A MODE, NOT A CORNER OF PRACTICE. A saved position with
 * something to do on it is a different activity from knocking balls about:
 * one keeps records and applies the rules, the other deliberately does
 * neither. Appended rather than inserted so an existing preferences file
 * still means what it said. */
enum { OPP_PRACTICE = 0, OPP_CPU, OPP_ONLINE, OPP_CAREER, OPP_CHALLENGE, OPP_N };
static const char *OPP_NAME[OPP_N] = { "FREE PRACTICE", "VS CPU", "ONLINE",
                                       "CAREER", "CHALLENGES" };

/* The menu is rows of options rather than a list of games, because there are now
 * six things to choose and a list only chooses one. Up/down picks the row,
 * left/right changes it, A activates. */
/* Eleven rows was a wall of text you had to read past to find START. The six
 * that only decide how the table LOOKS now live on their own screen, reachable
 * from here and from the pause menu — so they can also be changed mid-frame,
 * which is when you actually notice you dislike the cloth. */
enum { MR_GAME = 0, MR_OPP, MR_FRAMES, MR_STRENGTH, MR_REFVOICE,
       MR_CONTROLS, MR_APPEAR, MR_STATS, MR_START, MR_N };

/* The controls page. Handedness sits here rather than on the main menu because
 * it is the same kind of thing as the rest of these: a preference about how you
 * work the game, set once and forgotten, not a choice about the frame you are
 * about to play. */
enum { CR_HAND = 0, CR_STICKS, CR_INVSLIDE, CR_INVTURN, CR_RESET, CR_BACK, CR_N };

/* The appearance screen's own rows. */
/* THE POCKET SHAPE IS BEING TUNED AGAIN, so its screen is back on.
 *
 * It was switched off once the four numbers were found, on the grounds that
 * they were settled and the same for everybody. Then the 7 ft mouths were
 * tightened, which moves the cut arcs with them — they are ratios of each
 * table's own drop circle — and the lip wants judging again. That judging can
 * only honestly happen in the headset with a ball rolling at a pocket, which
 * is exactly why this screen exists. APPEARANCE -> POCKET SHAPE.
 *
 * It writes cuevr_pockets.txt next to the preferences and logs the four #define
 * lines ready to paste into cue_render.c, so a session's tuning does not have
 * to be transcribed by hand or remembered.
 *
 * Build with -DCUEVR_TUNE_POCKETS=0 to take it back out when they settle. */
#ifndef CUEVR_TUNE_POCKETS
#define CUEVR_TUNE_POCKETS 1
#endif

enum { AR_CLOTH = 0, AR_FRAME, AR_BODY, AR_LIGHT, AR_BALLS, AR_SPOTS, AR_CUE,
       AR_SURROUND,
#if CUEVR_TUNE_POCKETS
       AR_POCKETS,
#endif
       AR_BACK, AR_N };
/* Where you play. PASSTHROUGH is the app as shipped — the table in your own
 * room. The other two paint a world over the cameras: the plain dark room the
 * screenshots have always shown, or the snooker-theatre arena. */
static const char *SURROUND_NAME[3] = { "PASSTHROUGH", "DARK ROOM", "ARENA" };
/* Match lengths. Odd numbers only — a best-of-even can be drawn, and there is
 * nothing here to play off a draw with. */
static const int MATCH_LEN[] = { 1, 3, 5, 7, 9, 11 };
#define MATCH_LEN_N ((int)(sizeof MATCH_LEN / sizeof MATCH_LEN[0]))

/* The pause menu, on the MENU button. */
/* The lobby, matching the Mote lobby's own shape: pick a transport, then an
 * action. USB is absent because two headsets are not cabled together. */
enum { LB_TRANSPORT = 0, LB_ACTION, LB_CODE, LB_BROWSE, LB_WAIT };
enum { TR_LAN = 0, TR_NET, TR_N };
static const char *TR_NAME[TR_N] = { "LAN (WI-FI)", "INTERNET" };
enum { ACT_LANHOST = 0, ACT_LANJOIN, ACT_QUICK, ACT_HOST, ACT_JOIN, ACT_BROWSE };

/* The pause menu carries the render toggles: finding what is slow means turning
 * things off ONE AT A TIME while wearing the headset and watching the frame
 * rate, and an environment variable cannot be reached from in there. */
/* Actions only. The five render toggles that used to hang off the end of this
 * made the pause menu overfull — and they are settings a player changes once if
 * ever, against defaults that were chosen by measurement. cuevr_render_fx_set()
 * is still there for the preview harness to drive. */
enum { PS_RESUME = 0, PS_UNDO, PS_PICKUP, PS_RERACK, PS_PLACE, PS_NOMINATE,
       PS_FREEBALL, PS_RESPOT, PS_MINI, PS_DRILLS, PS_PRACTICE,
       PS_AGAIN, PS_ENDCHAL,
       PS_CONCEDE, PS_APPEAR, PS_CONTROLS,
       PS_STATS, PS_QUIT, PS_N };
/* THE LAYOUT EDITOR'S OWN SCREEN.
 *
 * It did not have one. You went into it and the panel carried on showing the
 * scoreboard, so there was nothing to say you were in an editor, nothing to
 * say which ball was in your hand, and — the part that actually lost work — no
 * way to accept what you had set out: B saved a NEW challenge and, on an
 * existing one, quietly threw the changes away and started a frame.
 *
 * Taking a ball off and putting one back were both real and both invisible:
 * carry a ball past the cushions to remove it, reach into empty cloth to bring
 * one home. Good gestures, and nobody could be expected to guess either, so
 * they are rows here as well. */
enum { LAY_DONE = 0, LAY_TAKEOFF, LAY_BACKON, LAY_RACK, LAY_CLEAR, LAY_CANCEL,
       LAY_N };

static const char *COLOUR_NAME[8] = {
    "", "", "YELLOW", "GREEN", "BROWN", "BLUE", "PINK", "BLACK" };


/* What a frame looked like, per player.
 *
 * The RECORDS screen keeps career bests; this is the other half — what just
 * happened, while it is still worth reading. A frame used to end with two words
 * over the scoreboard, and two words tell you nothing about how it went:
 * whether you missed everything and won on a fluke, or made a fifty and lost on
 * the black.
 *
 * `tally` counts the visit in progress by ball id; `best_tally` is the visit
 * that made `best_break`, kept so the screen can SHOW the break instead of
 * naming it. "56" is a number you have to take on trust; five reds, four blacks
 * and a pink is the break itself. */
#define CUEVR_TALLY_N 26          /* ball ids 0..25: reds 1..15, colours 20..25 */
typedef struct {
    int   shots;        /* strokes played */
    int   pot_shots;    /* strokes that potted something legally */
    int   potted;       /* object balls legally potted */
    int   fouls;
    float time;         /* seconds spent at the table, addressing shots */
    /* ONE figure, not two. Snooker calls a visit's haul a break and counts it
     * in points; pool calls it a run and counts it in balls. They are the same
     * statistic — balls potted without giving the table up — so showing both
     * put "BEST BREAK 56" next to "LONGEST RUN 9" and invited exactly the
     * question of how one frame produced both. */
    int   best_break;   /* points (snooker) or balls (pool) in one visit */
    int   brk;          /* the visit in progress */
    uint8_t tally[CUEVR_TALLY_N], best_tally[CUEVR_TALLY_N];
} CueVrPlayStat;

static const struct { CueGameKind kind; const char *name; } MENU[] = {
    { CUE_GAME_UK8,   "UK 8-BALL 7FT" },
    { CUE_GAME_US8,   "US 8-BALL 9FT" },
    { CUE_GAME_US9,   "9-BALL 9FT"    },
    { CUE_GAME_CN8,   "CHINESE 8 10FT"},
    { CUE_GAME_SNK6,  "SNOOKER 6-RED" },
    { CUE_GAME_SNK10, "SNOOKER 10-RED"},
    { CUE_GAME_SNK15, "SNOOKER 12FT"  },
};
#define MENU_N ((int)(sizeof MENU / sizeof MENU[0]))

static struct {
    int state;
    int menu_sel, menu_latch, menu_row;
    /* The A button gets its OWN latch, separate from the stick's.
     *
     * They shared one, and the main menu clears it whenever the STICK is
     * centred — so leaving a sub-screen with A still held cleared the latch on
     * the very next frame and the still-held button opened the sub-screen
     * again, immediately, for ever. A latch another control can clear is not a
     * latch. */
    int btn_latch;
    int menu_updown;             /* latch for row movement */
    int opp;                     /* OPP_* */
    int cloth_idx, frame_idx;    /* cue_theme.h palettes */
    int ref_voice;               /* CUEVR_REF_* — the referee's break calls */
    int cloth_hov;               /* the swatch under the pointer, or -1 */
    int light_idx;               /* the lighting rig, cuevr_light.h */
    float menu_cue_roll;         /* the display cue turning in the menu */
    /* Frame timing, on screen. "It hitches sometimes" cannot be acted on; a
     * worst-frame number over the last second can. The MINIMUM is the figure
     * that matters — a mean of 72 with one 40 ms frame in it still reads as a
     * lurch, and the mean will happily hide it. */
    float fps_acc, fps_worst, fps_win;
    int   fps_n;
    float fps_show, fps_low;
    SDL_Thread *ai_th;           /* the opponent, thinking off the render thread */
    volatile int ai_done;
    int body_idx;                /* the frame model; -1 = the one that suits */
    int cue_idx;                 /* which cue off the rack */
    int levelled;                /* the table has been sited once this session */
    int dbg_frame;

    /* Records. `stat_me` is which seat the human is in; `visit_full` is set
     * when a visit begins with every one of my object balls still on the table,
     * which is what makes winning that visit a clearance rather than merely the
     * last few balls. */
    CueVrPrefs stats;
    int stat_visit_owner;
    int stat_visit_full;
    int stat_counted;          /* this frame's result already recorded */
    int stat_prev_brk;         /* the break before this shot, for tier crossings */
    int stat_dirty;
    int stat_page;         /* 0 = vs CPU, 1 = online */
    int stat_scroll, stat_len;   /* the records list is taller than the panel */
    /* This frame's play, and the match's. Not saved: they describe the game you
     * are in, and the game you are in is over by the time a save would matter. */
    CueVrPlayStat fstat[2], mstat[2];
    float shot_clock;      /* how long the striker has been at the table */
    int   stat_folded;     /* this frame is already summed into the match */
    int dec_sel;           /* highlighted decision row */
    int lb_click;          /* the laser clicked a lobby row */
    int can_repick;        /* the ball is down but the stroke is not played */
    int lefty;             /* bridges with the right hand */
    int stick_swap, inv_slide, inv_turn;
    int cue_spots;
    int cut_cr, cut_cs, cut_mr, cut_ms;   /* the pocket cut, tuned in the headset */
    int surround;                /* 0 passthrough, 1 dark room, 2 arena */
    /* The six-ball clearance challenge. `mini` is on; `mini_t` is the clock,
     * which starts on the first strike and stops on the last ball; `mini_done`
     * freezes it so the final time stays on the board. */
    int   mini, mini_done, mini_beat;
    float mini_t;
    int   mini_best[CUE_GAME_COUNT];

    /* TOASTS. Records and achievements used to happen in silence and turn up
     * later in a menu, which is no way to be told you have just made your best
     * break. A short queue, because two can land on the same shot — a 50 break
     * that is also a new personal best — and they must not overwrite each
     * other. Drawn in the message zone, where the game already speaks. */
    struct { char title[28], body[34]; int kind; } toast[4];
    int   toast_n;
    float toast_t;

    /* The career. `in_career` is set for the duration of a career MATCH, so
     * the result knows where to go when the last frame ends. */
    CueVrCareer career;
    int   in_career, car_league, car_row, car_view, car_scroll;
    int   car_pick[CUE_GAME_COUNT];
    char  car_path[512];
    int prac_respot;             /* practice snooker: colours go back on */

    /* ---- practice drills: a table you set up, and something to do on it ----
     * See cuevr_drill.h. `drill` is the slot being played, -1 for none; the
     * clock runs from the first strike; score and pots are the VISIT, because
     * every goal is judged over one. `edit` is the ball currently in your hand
     * in the layout editor, -1 for none. */
    CueVrDrills drills;
    char  drill_path[512];
    int   drill;                 /* slot being played, or -1 */
    int   drill_done, drill_won, drill_beat;
    float drill_t;
    int   drill_score, drill_pots;
    /* WHICH OF THE ASKED-FOR BALLS HAVE GONE DOWN this visit, as the same bit
     * per id that CueVrDrill.need uses. A POT challenge was decided by
     * comparing each potted ball against `ball`, the LEGACY single id — so a
     * challenge set to "the black" through the ball grid was judged against
     * whatever `ball` happened to hold, and potting the black did nothing. */
    uint32_t drill_got;
    int   edit_ball;             /* the ball being carried in the editor, or -1 */
    int   edit_latch;
    int   drill_row;             /* the slot the drills screen is on */
    int   drill_scroll;          /* first row shown — the list outgrows a panel */
    int   edit_new;              /* the editor is building a NEW challenge */
    int   edit_slot;             /* which slot it will be saved into, -1 = first free */
    int   dset_row, dset_ball;   /* the challenge-setup screen */
    int   lay_row;               /* the layout editor's own row */
    /* THE EDITOR'S OPTIONS ARE BEHIND MENU. With them always on the panel, the
     * trigger had two jobs at once — pick a ball up, and click a row — and the
     * rows were walked with the stick, which is not how anything else in the
     * game works and is not what the sticks do here (they move the table). Shut
     * by default: the panel says what is in your hand, and MENU brings up the
     * list to point at, exactly like the pause menu. */
    int   lay_menu;
    int break_first;             /* who breaks this frame (rules player index) */
    float undo_hold;             /* B held down, seconds — practice take-back */
    /* The pointer: where the right controller's ray meets the panel, in the
     * HUD's own layout coordinates (0..HW across, 0..rows down). */
    int   ptr_ok;
    float ptr_x, ptr_y;
    int   ptr_latch;
    int appear_from;       /* the state to return to */
    CueVrShot idle_shot;   /* this frame's cue update, wherever we are */
    int pause_sel, pause_latch, pause_click;

    /* Controller alignment: six channels the player steps through, the values
     * they are editing, and where they came from so CANCEL means something. */
    /* The controller alignment. There is no screen for it any more — the
     * measured default is right and nobody needed to change it twice — but the
     * VALUES stay: they are what puts the drawn controller where the real one
     * is, they load from preferences, and an existing file may carry a figure
     * somebody set by hand. */
    float cal_pos[3], cal_rot[3];
    int net_me;                  /* our player index in an online match */
    /* The opponent's cue, smoothed. Their poses arrive unevenly however fast
     * they are sent, and a cue drawn on the raw arrivals stutters. */
    Vec3 ocue_tip, ocue_butt;
    int  ocue_have;
    int lb_screen, lb_sel, lb_tr, lb_act, lb_latch, lb_ud;
    int lb_cur;                  /* which code character is being edited */
    char lb_code[CUEVR_CODE_LEN + 1];

    /* Undo. A snapshot of everything a shot changes, taken the instant before
     * the strike: the balls, and the rules state that scores them. Practice
     * only, by design — undo in a match is just cheating with extra steps. */
    CueBall  snap_balls[CUE_MAX_BALLS];
    int      snap_n;
    CueRules snap_rules;
    int      have_snap;
    int persona;                 /* CPU difficulty */
    int ballset;                 /* cue_render's authored sets */

    CueTable  tab;
    CueWorld  world;
    CueBall   balls[CUE_MAX_BALLS];
    int       nballs;
    CueRules  rules;
    CueVrSetup setup;
    CueVrCue   cue;

    uint8_t   was_on[CUE_MAX_BALLS];
    uint32_t  shot_events;
    uint32_t  rng;

    float     menu_hold;         /* left menu button held, seconds */
    int       hud_dirty;
    int       dec_latch;
    /* The press that got you INTO a placement must not also end it. The
     * A that confirms the table placement carried straight through the
     * break placement in one frame — SETUP, MENU, PLACE, AIM on one
     * button-down — so the ball was never actually placeable. */
    int       place_latch;
    /* What the MENU tap interrupted. Resuming went to ST_AIM whatever it
     * was, so pausing during a shot froze the balls mid-flight, never
     * resolved it, and left you aiming a shot that was already in the
     * air — the table was stuck for good. */
    int       pause_from;
    /* The player named the colour from the menu, so aiming must stop
     * overriding it — you nominate a cushion-first shot precisely when
     * the cue is NOT pointing at the ball you mean. Cleared when the
     * nomination itself is (turn change, or off a colour). */
    int       nom_manual;
    int       match_idx;      /* index into MATCH_LEN */
    int       over_latch;
    int       over_hov;          /* the pointer is on the CONTINUE row */
    /* Ball in hand just started: bring the table to the player so the ball
     * is under their bridge hand instead of wherever the last shot left
     * it — which on a 12 ft table can be three metres away, and you
     * cannot walk there. Acted on where the tracking is to hand. */
    int       recentre;
    int       sited;             /* the table has been put somewhere real */
    float     pref_height;      /* the height they set last time */

    /* The CPU's shot, once it has decided on it. It gets a cue and takes the
     * shot with it rather than the ball simply leaving: you should be able to
     * see what it is about to do, and hear it played. */
    CueAIShot cpu_shot;
    float     cpu_t;            /* seconds into its stroke */
    MoteVrV3  cpu_tip, cpu_butt;
    uint16_t  hud[CUEVR_HUD_W * CUEVR_HUD_H];
    char      msg[32];
    float     msg_time;

    CueVrScene scene;
} S;

/* THE DOMINANT HAND is the one on the butt: it holds the cue, it carries the
 * ball, it holds the laser, and every button and stick a menu reads belongs to
 * it. For a left-hander that is the physical LEFT controller, so none of this
 * can be written as MOTE_VR_RIGHT.
 *
 * Two things stay physical because they are about the hardware and not the
 * player: the MENU button, which only the left controller has, and the hand
 * poses handed to the renderer, which must stay what they are or the controller
 * models swap sides. */
/* A AND B DO NOT MOVE. They are letters printed on the right controller, and
 * every prompt in the game says "A". Swapping them for a left-hander meant the
 * screen asked for A while the button that answered was X — the label and the
 * key disagreeing is worse than reaching across.
 *
 * What DOES follow the cue hand is the pointing: the laser and its trigger, and
 * the trigger that drops the ball. Those are held, not read off a legend. */
#define DOMH (S.lefty ? MOTE_VR_LEFT  : MOTE_VR_RIGHT)
#define OFFH (S.lefty ? MOTE_VR_RIGHT : MOTE_VR_LEFT)

/* The rest the player has set. Only the scripted preview harness wants this:
 * its fake bridge hand has to sit rest_lift below the aim line to put the cue on
 * the ball, exactly as a real hand does without being told. Hardcoding the
 * default here meant the harness went quiet the moment a saved rest was
 * loaded — a stroke that never lands looks identical to a test that passes. */
MoteVrV3 cuevr_app_rest(void) { return S.cue.rest; }
int cuevr_app_aiming(void) { return S.state == ST_AIM; }

/* ---- what a two-instance test has to be able to see ----------------------
 *
 * The old online test drove two instances into a room together and checked they
 * agreed on the game and who broke. They did, every time — and the shot AFTER
 * the break was dead in the water for want of turn routing, for as long as the
 * test existed. Pairing is not playing. These are the numbers a back-and-forth
 * test asserts on: which state each end is sitting in, whose turn each end
 * thinks it is, and what each end has on the table. Two ends that disagree
 * about any of them have desynced, whatever the screen looks like. */
const char *cuevr_app_state_name(void) {
    /* IN THE ENUM'S ORDER, and it has to be kept that way — a screen added to
     * the enum without a name here does not break anything visibly, it just
     * makes every trace of it read as the WRONG state, which is worse. Three
     * screens were added and this was not, so a challenge list logged itself as
     * the pocket tuner. */
    static const char *N[] = {
        "MENU","SETUP","AIM","ROLL","THINK","CPUCUE","PLACE","DECIDE","OVER",
        "PAUSE","LOBBY","APPEAR","STATS","CONTROLS","CARSETUP","CAREER",
        "CARTABLE","CARACH","CLOTH","DRILLS","LAYOUT","DRILLSET","POCKETS" };
    _Static_assert(sizeof N / sizeof N[0] == ST_POCKETS + 1,
                   "a state was added without a name");
    return (S.state >= 0 && S.state < (int)(sizeof N / sizeof N[0]))
         ? N[S.state] : "?";
}
/* Why a scripted stroke did or did not land. A harness that cannot see inside
 * the cue can only report "nothing happened", which is the same thing a dead
 * network link reports. */
void cuevr_app_cue_probe(int *tracked, int *stroking, int *on_ball,
                         float *gap, float *speed, int *n) {
    if (tracked)  *tracked  = S.cue.tracked;
    if (stroking) *stroking = S.cue.stroking;
    if (on_ball)  *on_ball  = S.cue.on_ball;
    if (gap)      *gap      = S.cue.gap;
    if (speed)    *speed    = S.cue.speed;
    if (n)        *n        = S.cue.speed_n;
}
/* The cue's real line, so a scripted stroke can be corrected ONTO the ball
 * rather than aimed at it by arithmetic that has to reproduce the bridge rest,
 * the hand orientation and the elevation clamp — three things that move. The
 * harness translates both hands by the perpendicular miss and is on the ball
 * next frame, whatever the geometry does. */
void cuevr_app_cue_line(MoteVrV3 *tip, MoteVrV3 *axis) {
    if (tip)  *tip  = S.cue.tip;
    if (axis) *axis = S.cue.axis;
}
int cuevr_app_table_kind(void) { return (int)S.tab.kind; }
int cuevr_app_net_turn(void) { return S.rules.turn; }
int cuevr_app_net_seat(void) { return S.net_me; }
int cuevr_app_score(int i)   { return S.rules.score[i & 1]; }
int cuevr_app_frames(int i)  { return S.rules.frames[i & 1]; }
int cuevr_app_best_of(void)  { return S.rules.best_of; }
int cuevr_app_balls_on(void) {
    int n = 0;
    for (int i = 0; i < S.nballs; i++) if (S.balls[i].on) n++;
    return n;
}
/* A cheap fingerprint of the whole table, so two ends can be compared in one
 * number rather than by eye. Quantised to a tenth of a millimetre because
 * lockstep is not expected to be bit-identical — the host's correction is what
 * makes it agree, and this is here to prove that it does. */
unsigned cuevr_app_table_hash(void) {
    unsigned h = 2166136261u;
    #define MIX(v) do { unsigned _v = (unsigned)(v); h ^= _v; h *= 16777619u; } while (0)
    for (int i = 0; i < S.nballs; i++) {
        MIX(S.balls[i].on);
        MIX((int)(S.balls[i].pos.x * 10000.0f));
        MIX((int)(S.balls[i].pos.z * 10000.0f));
    }
    MIX(S.rules.turn); MIX(S.rules.score[0]); MIX(S.rules.score[1]);
    MIX(S.rules.target); MIX(S.rules.seq); MIX(S.rules.reds_left);
    MIX(S.rules.group[0]); MIX(S.rules.group[1]); MIX(S.rules.open);
    MIX(S.rules.frames[0]); MIX(S.rules.frames[1]); MIX(S.rules.frame_over);
    #undef MIX
    return h;
}
/* The same, split, so a mismatch says WHICH half moved. One combined number
 * tells you there is a desync and nothing about where to look. Balls only,
 * excluding the cue ball — the striker holds that one and the far end does not
 * hear where until the shot goes. */
unsigned cuevr_app_object_hash(void) {
    unsigned h = 2166136261u;
    #define MIX(v) do { unsigned _v = (unsigned)(v); h ^= _v; h *= 16777619u; } while (0)
    for (int i = 1; i < S.nballs; i++) {
        MIX(S.balls[i].on);
        MIX((int)(S.balls[i].pos.x * 10000.0f));
        MIX((int)(S.balls[i].pos.z * 10000.0f));
    }
    #undef MIX
    return h;
}
unsigned cuevr_app_rules_hash(void) {
    unsigned h = 2166136261u;
    #define MIX(v) do { unsigned _v = (unsigned)(v); h ^= _v; h *= 16777619u; } while (0)
    MIX(S.rules.turn); MIX(S.rules.score[0]); MIX(S.rules.score[1]);
    MIX(S.rules.target); MIX(S.rules.seq); MIX(S.rules.reds_left);
    MIX(S.rules.group[0]); MIX(S.rules.group[1]); MIX(S.rules.open);
    MIX(S.rules.frames[0]); MIX(S.rules.frames[1]); MIX(S.rules.frame_over);
    MIX(S.rules.shots_remaining); MIX(S.rules.two_shot); MIX(S.rules.free_ball);
    MIX(S.rules.cfoul[0]); MIX(S.rules.cfoul[1]); MIX(S.rules.break_shot);
    #undef MIX
    return h;
}
float cuevr_app_cue_x(void) { return S.balls[0].pos.x; }
float cuevr_app_cue_z(void) { return S.balls[0].pos.z; }

/* CUEVR_SCREEN=appear|stats — jump straight to a menu screen for a capture.
 * The scripted stick-walk cannot reach them reliably (it is frame-timed and the
 * row counts change), and a screen nobody can photograph is a screen nobody
 * checks. */
/* A coin toss that is not the same coin toss every launch.
 *
 * S.rng is seeded to a constant so the CPU's shot selection is reproducible
 * across runs, which is what the AI measurements need — and that is exactly the
 * wrong property for deciding who breaks. This one is seeded from the clock and
 * used for nothing else. */
static uint32_t s_toss = 0;
static int coin_toss(void) {
    if (!s_toss) {
        s_toss = (uint32_t)time(NULL) * 2654435761u + 0x9E3779B9u;
        if (!s_toss) s_toss = 0xA5A5A5A5u;
    }
    s_toss ^= s_toss << 13; s_toss ^= s_toss >> 17; s_toss ^= s_toss << 5;
    return (int)((s_toss >> 16) & 1u);
}

static void stat_frame_reset(void);
static void restyle_table(void);
#if CUEVR_TUNE_POCKETS
static void pockets_write(void);
static void pockets_log(void);
#endif
enum { TOAST_RECORD = 0, TOAST_ACH };
static void toast_push(int kind, const char *title, const char *body);
static uint16_t cloth_colour(int i);
static void arm_shot(void);
static void drill_capture(CueVrDrill *d);
static void drill_start(int slot);
static void rerack(void);
static void hand_over(void);
static void start_frame(CueGameKind kind);
static int coin_toss(void);
static void mini_start(void);
static void mini_stop(void);
static void stat_match_reset(void);
static void stat_frame_into_match(void);

void cuevr_app_force_screen(const char *name) {
    if (!name) return;
    if (!strcmp(name, "appear")) {
        S.appear_from = ST_MENU; S.menu_row = AR_CLOTH; S.state = ST_APPEAR;
    } else if (!strcmp(name, "drillswitch")) {
        /* THE TABLE A DRILL WAS MADE ON. Capture a position on THIS table,
         * then move the game to a different one, then start the drill — which
         * must bring the right table back with it. Doing that by hand needs two
         * menus and a good memory; doing it here is one line and it is checked
         * every build. */
        S.opp = OPP_PRACTICE;
        drill_capture(&S.drills.slot[0]);
        S.drills.slot[0].goal = CUEVR_GOAL_SETUP;
        S.drill_path[0] = 0;
        fprintf(stderr, "[drillswitch] saved on kind %d\n", (int)S.drills.slot[0].kind);
        {   /* somewhere else entirely: the first table that is not this one */
            int other = ((int)S.tab.kind == CUE_GAME_UK8) ? CUE_GAME_SNK15 : CUE_GAME_UK8;
            for (int i = 0; i < MENU_N; i++)
                if ((int)MENU[i].kind == other) S.menu_sel = i;
            start_frame((CueGameKind)other);
            fprintf(stderr, "[drillswitch] moved to kind %d\n", (int)S.tab.kind);
        }
        drill_start(0);
        fprintf(stderr, "[drillswitch] playing on kind %d\n", (int)S.tab.kind);
    } else if (!strcmp(name, "drill")) {
        /* Straight into a playable one: a clearance of what is on the table,
         * which is the goal a scripted stroke can actually make progress
         * against. CUEVR_SCREEN=drill. */
        S.opp = OPP_PRACTICE;
        drill_capture(&S.drills.slot[0]);
        S.drills.slot[0].goal = CUEVR_GOAL_CLEAR;
        S.drill_path[0] = 0;          /* a test must not write over real drills */
        drill_start(0);
    } else if (!strcmp(name, "chalstart")) {
        /* THE REAL ROUTE: sit on the main menu in CHALLENGES with START under
         * the pointer, so the test presses the same button a player does. The
         * screen hooks that jump straight to a state cannot catch a START that
         * does nothing, which is exactly what shipped. */
        S.opp = OPP_CHALLENGE;
        S.state = ST_MENU;
        S.menu_row = MR_START;
    } else if (!strcmp(name, "dset")) {
        S.opp = OPP_CHALLENGE;
        drill_capture(&S.drills.slot[0]);
        S.drills.slot[0].goal = CUEVR_GOAL_POT;
        S.drills.slot[0].need = (1u << CUE_ID_BLACK) | (1u << CUE_ID_BLUE) | 2u;
        S.drills.slot[0].ball = CUE_ID_BLACK;
        S.drill_path[0] = 0;
        S.edit_slot = 0; S.dset_row = 0; S.dset_ball = -1;
        S.state = ST_DRILLSET;
    } else if (!strcmp(name, "drills")) {
        /* With a couple of slots filled, so the screen shows what it looks like
         * in use rather than eight rows of EMPTY. */
        S.opp = OPP_PRACTICE;
        drill_capture(&S.drills.slot[0]);
        S.drills.slot[0].goal = CUEVR_GOAL_POT;
        S.drills.slot[0].ball = CUE_ID_BLACK;
        S.drills.slot[0].need = 1u << CUE_ID_BLACK;
        S.drills.slot[0].timed = 1;
        S.drills.slot[0].best = 842;
        drill_capture(&S.drills.slot[1]);
        S.drills.slot[1].goal = CUEVR_GOAL_CLEAR;
        S.drills.slot[1].wins = 3; S.drills.slot[1].tries = 11;
        drill_capture(&S.drills.slot[2]);
        S.drills.slot[2].goal = CUEVR_GOAL_SCORE;
        S.drills.slot[2].target = 40;
        drill_capture(&S.drills.slot[3]);
        S.drills.slot[3].goal = CUEVR_GOAL_SETUP;
        /* A slot on ANOTHER table, so the gallery is seen doing the thing it
         * exists for: the card draws the table the position was set out on, not
         * the one that happens to be up. */
        drill_capture(&S.drills.slot[4]);
        S.drills.slot[4].kind = (uint8_t)((int)S.tab.kind == CUE_GAME_UK8
                                          ? CUE_GAME_SNK15 : CUE_GAME_UK8);
        S.drills.slot[4].goal = CUEVR_GOAL_POT;
        S.drill_path[0] = 0;          /* a test must not write over real drills */
        S.drill_row = 0; S.drill_scroll = 0;
        S.opp = OPP_CHALLENGE;
        S.state = ST_DRILLS;
    } else if (!strcmp(name, "layout")) {
        /* The editor, mid-edit: a racked table with a ball in the hand, which
         * is the state the screen exists to explain. */
        S.opp = OPP_CHALLENGE;
        S.nballs = cue_table_rack(&S.tab, S.balls);
        cue_rules_init(&S.rules, &S.tab, 0);
        S.rules.turn = 0; S.rules.ball_in_hand = 0;
        for (int i = 1; i < 4 && i < S.nballs; i++) S.balls[i].on = 0;
        S.edit_new = 1; S.edit_slot = -1;
        S.edit_ball = 5 < S.nballs ? 5 : -1;
        S.lay_row = LAY_DONE;
        S.lay_menu = getenv("CUEVR_LAYMENU") ? 1 : 0;
        S.drill_path[0] = 0;
        S.state = ST_LAYOUT;
    } else if (!strcmp(name, "cloth")) {
        S.appear_from = ST_MENU; S.state = ST_CLOTH; S.cloth_hov = -1;
    } else if (!strcmp(name, "controls")) {
        S.appear_from = ST_MENU; S.menu_row = CR_HAND; S.state = ST_CONTROLS;
    } else if (!strcmp(name, "stats")) {
        S.appear_from = ST_MENU; S.state = ST_STATS; S.stat_scroll = 0;
    } else if (!strcmp(name, "stats2")) {
        S.appear_from = ST_MENU; S.state = ST_STATS; S.stat_scroll = 68;
    } else if (!strcmp(name, "menu")) {
        S.state = ST_MENU;
        S.menu_row = MR_GAME;
    } else if (!strcmp(name, "pause")) {
        /* The pause menu mid-practice-frame, which is where the longest list
         * lives and the only place AUTO RESPOT appears. */
        S.opp = OPP_PRACTICE;
        S.pause_from = ST_AIM;
        S.pause_sel = 0;
        S.have_snap = 1;              /* so UNDO SHOT is on the list too */
        S.state = ST_PAUSE;
    } else if (!strcmp(name, "mini")) {
        /* The clearance challenge mid-run, so its board can be photographed —
         * it cannot be reached from a script any other way. */
        S.opp = OPP_PRACTICE;
        mini_start();
        S.have_snap = 1;                 /* i.e. the clock is running */
        S.mini_t = 27.44f;
        S.mini_best[(int)S.tab.kind] = 3162;
        for (int i = 1; i < S.nballs && i <= 2; i++) S.balls[i].on = 0;
        S.state = ST_AIM;
    } else if (!strncmp(name, "career", 6)) {
        /* A career several matches in, so the hub, the table and the
         * achievements can all be photographed — none of them is reachable
         * from a script otherwise, and a season is forty matches long. */
        int kinds[3] = { CUE_GAME_UK8, CUE_GAME_SNK15, CUE_GAME_US9 };
        cuevr_career_new(&S.career, kinds, 3);
        for (int i = 0; i < 5; i++) {
            const CueVrCarFixture *fx;
            int l = cuevr_career_next(&S.career, &fx);
            if (l < 0) break;
            cuevr_career_record(&S.career, l, (i != 2), (i != 2) ? 2 : 1,
                                (i != 2) ? 1 : 2, i == 1 ? 57 : 24);
        }
        S.opp = OPP_CAREER;
        S.car_row = 0; S.car_view = 0; S.car_scroll = 0;
        S.state = !strcmp(name, "careertab") ? ST_CARTABLE
                : !strcmp(name, "careerach") ? ST_CARACH
                : !strcmp(name, "careernew") ? ST_CARSETUP : ST_CAREER;
        if (S.state == ST_CARSETUP) {
            for (int i = 0; i < CUE_GAME_COUNT; i++) S.car_pick[i] = (i == 0 || i == 4);
        }
    } else if (!strncmp(name, "toast", 5)) {
        /* Both kinds, queued, so the band and the "+1" can be photographed —
         * a record only happens when it happens. */
        S.state = ST_AIM;
        S.rules.score[0] = 63; S.rules.score[1] = 41; S.rules.brk = 57;
        if (name[5] == 'a')
            toast_push(TOAST_ACH, cuevr_car_ach_name(CAR_ACH_BRK50),
                       cuevr_car_ach_how(CAR_ACH_BRK50));
        else {
            toast_push(TOAST_RECORD, "BREAK OF 57", "your best on this table");
            toast_push(TOAST_RECORD, "50 BREAK", "3 made");
        }
    } else if (!strcmp(name, "freeball")) {
        /* On a red with a free ball awarded, the case the user hit: the pause
         * menu has to offer a COLOUR to stand in for it. */
        S.state = ST_PAUSE;
        S.pause_from = ST_AIM;
        S.pause_sel = 0;
        S.rules.free_ball = 1;
        S.rules.free_ball_id = CUE_ID_BROWN;
        S.rules.target = 0;
#if CUEVR_TUNE_POCKETS
    } else if (!strcmp(name, "pockets")) {
        S.appear_from = ST_MENU; S.state = ST_POCKETS; S.menu_row = 0;
        pockets_write();          /* so the harness can check the file it writes */
#endif
    } else if (!strcmp(name, "board")) {
        /* The in-play scoreboard, with a frame's worth of score on it. The
         * ahead/behind figure only exists once somebody has scored, and no
         * script can drive a headset game far enough into a frame to produce
         * one — so the numbers are put there and the LAYOUT is what is being
         * looked at. */
        S.state = ST_AIM;
        S.rules.score[0] = 47; S.rules.score[1] = 31;
        S.rules.brk = 12;
        S.rules.target = 0;
        for (int i = 1; i < S.nballs; i++)
            if (S.balls[i].id >= 1 && S.balls[i].id <= 6) S.balls[i].on = 0;
    } else if (!strcmp(name, "over") || !strcmp(name, "match")) {
        /* The end-of-frame and end-of-match screens, with the play invented for
         * them. Both are only reachable by playing a frame — or five — right
         * out, which no capture can do, and a screen nobody can photograph is a
         * screen nobody checks. The FIGURES are made up; the layout is real.
         *
         * "match" is the same screen with match_over set: match totals instead
         * of the frame's, the frames tally in the band, and the break titled as
         * the best of the match. */
        int mt = !strcmp(name, "match");
        S.state = ST_OVER;
        S.rules.frame_over = 1;
        S.rules.winner = 0;
        stat_frame_reset();
        stat_match_reset();
        if (mt) {
            S.rules.best_of = 5;
            S.rules.frames[0] = 3; S.rules.frames[1] = 1;
            S.rules.match_over = 1; S.rules.match_winner = 0;
        } else {
            /* A single frame IS the match, and book_frame sets match_over when
             * it ends — so the capture has to as well or it photographs a state
             * the game never reaches. */
            S.rules.best_of = 1;
            S.rules.frames[0] = 1;
            S.rules.match_over = 1; S.rules.match_winner = 0;
        }
        int fold_after = !mt;   /* the single frame IS the match: fold it once */
        for (int p = 0; p < 2; p++) {
            CueVrPlayStat *st = &S.fstat[p];
            st->shots = p ? 19 : 24;
            st->pot_shots = p ? 9 : 16;
            st->potted = p ? 11 : 23;
            st->fouls = p ? 4 : 1;
            st->time = (float)st->shots * (p ? 9.4f : 12.6f);
            if (S.tab.is_snooker) {
                /* The BREAK IS ADDED UP FROM THE BALLS, not typed in beside
                 * them. A hand-written 56 next to five reds and four blacks is
                 * a screen that contradicts itself in the one place it is
                 * asking to be believed. */
                if (p) { st->best_tally[1] = 3; st->best_tally[CUE_ID_BLUE] = 2;
                         st->best_tally[CUE_ID_PINK] = 1; }
                else   { st->best_tally[1] = 7; st->best_tally[CUE_ID_BLACK] = 5;
                         st->best_tally[CUE_ID_PINK] = 1;
                         st->best_tally[CUE_ID_BLUE] = 1; }
                int v = 0;
                for (int i = 1; i <= 15; i++) v += st->best_tally[i];        /* a red is 1 */
                for (int c = CUE_ID_YELLOW; c <= CUE_ID_BLACK; c++)
                    v += st->best_tally[c] * (c - 18);
                st->best_break = v;
            } else {
                st->best_break = p ? 3 : 5;
                for (int i = 0; i < st->best_break; i++)
                    st->best_tally[(p ? 9 : 1) + i] = 1;
            }
        }
        /* Four frames of it, folded the way the real thing folds: the counts
         * sum and the best break is a maximum, not a total. */
        if (fold_after) { stat_frame_into_match(); S.stat_folded = 1; }
        if (mt) {
            for (int f = 0; f < 4; f++) stat_frame_into_match();
            for (int p = 0; p < 2; p++) {
                CueVrPlayStat *m = &S.mstat[p];
                if (S.tab.is_snooker) {
                    memset(m->best_tally, 0, sizeof m->best_tally);
                    if (p) { m->best_tally[1] = 4; m->best_tally[CUE_ID_BLACK] = 2;
                             m->best_tally[CUE_ID_PINK] = 1;
                             m->best_tally[CUE_ID_BLUE] = 1; }
                    else   { m->best_tally[1] = 9; m->best_tally[CUE_ID_BLACK] = 8;
                             m->best_tally[CUE_ID_PINK] = 1; }
                    int v = 0;
                    for (int i = 1; i <= 15; i++) v += m->best_tally[i];
                    for (int c = CUE_ID_YELLOW; c <= CUE_ID_BLACK; c++)
                        v += m->best_tally[c] * (c - 18);
                    m->best_break = v;
                }
            }
            S.stat_folded = 1;         /* already counted; do not fold again */
        }
    }
    S.hud_dirty = 1;
}

/* A corner pocket, in ROOM space. Only the preview wants this: aiming its camera
 * by hand meant guessing where the table had been placed, and every guess landed
 * somewhere in the middle of the cloth. Ask the app instead. */
MoteVrV3 cuevr_app_pocket_room(void) {
    Vec3 t = { S.tab.half_len, 0.0f, S.tab.half_wid };
    return cuevr_table_to_room(&S.setup.place, t);
}

/* Any point in TABLE space, in room space — as fractions of the half-extents,
 * so one camera position means the same thing on a 7 ft pub table and a 12 ft
 * match table. The pocket accessor above is this with (1,0,1), and everything
 * else I wanted to photograph needed the same translation done by hand. */
MoteVrV3 cuevr_app_table_room(float fx, float y, float fz) {
    Vec3 t = { fx * S.tab.half_len, y, fz * S.tab.half_wid };
    return cuevr_table_to_room(&S.setup.place, t);
}
float cuevr_app_table_yaw(void) { return S.setup.place.yaw; }
/* Where the panel is, in room space. Menu captures were framed by guessing
 * table-space fractions at it, and it MOVES — it hangs past whichever end of the
 * table the player is not standing at. */
MoteVrV3 cuevr_app_hud_room(void) { return S.scene.hud_pos; }

/* Which end of the table the panel hangs past, as a multiplier on the length.
 *
 * "Whichever end is away from you" flipped the moment the head crossed the
 * middle of the table — and the middle of a table is exactly where a player
 * stands. Shifting your weight threw the board from one end of the room to the
 * other. It stays where it is until you are plainly in the other half.
 *
 * The lock is for the harness: a preview camera that flies to the panel pushes
 * the panel to the far end, which moves the camera, which moves the panel, and
 * the two chase each other round the table for ever — so a capture of any menu
 * screen photographs a postage stamp 4 m away. */
static int s_hud_end_lock;
void cuevr_app_lock_hud_end(int sign) { s_hud_end_lock = sign; }

static float hud_end_sign(float head_x) {
    static int side;                       /* +1 = the head is at the +x end */
    if (s_hud_end_lock) return (float)s_hud_end_lock;
    if      (head_x >  0.25f) side =  1;
    else if (head_x < -0.25f) side = -1;
    else if (!side)           side = (head_x >= 0.0f) ? 1 : -1;
    return side > 0 ? -1.0f : 1.0f;        /* the panel goes to the OTHER end */
}

/* Forcing a rig or a body from the harness. After the preferences load, because
 * otherwise the saved value would immediately win and the capture would be of
 * whatever was last played rather than of what was asked for. */
void cuevr_app_force_light(int i) {
    S.light_idx = i;
    cuevr_render_set_light(i);
}
/* Where the display cue is, in room space. The preview cannot frame it from
 * table-space guesses — it moves with the table size and with which end the butt
 * is at — so it asks. */
MoteVrV3 cuevr_app_cue_mid(void) {
    /* Biased hard onto the butt: the splice, the veneer, the badge and the cap
     * are all in the last third, and that is what anyone framing the cue wants
     * to see. 0.72 still left the badge off the bottom of the picture. */
    return mv3_add(mv3_scale(S.scene.cue_butt, 0.90f),
                   mv3_scale(S.scene.cue_tip,  0.10f));
}
void cuevr_app_force_cue(int i) {
    S.cue_idx = i;
    cuevr_render_set_cue(i);
}
void cuevr_app_force_body(int i) {
    S.body_idx = i;
    cuevr_render_set_body(i);
    cuevr_render_set_table(&S.tab, &S.world);
}
/* And the timber, by palette index — the harness's only way to photograph the
 * bodies in ebony without walking the appearance menu. */
void cuevr_app_force_surround(int i) {
    S.surround = (i >= 0 && i <= 2) ? i : 0;
    cuevr_render_set_surround(S.surround);
}
void cuevr_app_force_framecol(int i) {
    if (i < 0 || i >= CUE_NFRAME) return;
    S.frame_idx = i;
    restyle_table();
}
float cuevr_app_grip(void)      { return S.cue.grip; }

/* ---- what just happened -------------------------------------------------- */

static void stat_frame_reset(void) {
    memset(S.fstat, 0, sizeof S.fstat);
    S.shot_clock = 0.0f;
    S.stat_folded = 0;
}
static void stat_match_reset(void) { memset(S.mstat, 0, sizeof S.mstat); }

/* Fold the frame into the match. Everything sums except the two bests, which
 * are maxima: a match's best break is the best break in it, not the total of
 * each frame's. */
static void stat_frame_into_match(void) {
    for (int p = 0; p < 2; p++) {
        const CueVrPlayStat *f = &S.fstat[p];
        CueVrPlayStat *m = &S.mstat[p];
        m->shots     += f->shots;
        m->pot_shots += f->pot_shots;
        m->potted    += f->potted;
        m->fouls     += f->fouls;
        m->time      += f->time;
        if (f->best_break > m->best_break) {
            m->best_break = f->best_break;
            memcpy(m->best_tally, f->best_tally, sizeof m->best_tally);
        }
    }
}

/* The frame is over, however it ended — potted out, conceded, three misses,
 * lost on the black. Every one of those used to set the state by hand, and the
 * match totals only ever saw the ones that went through resolve_shot. */
static void enter_over(void) {
    /* "Frame." — the referee's call, and it belongs HERE rather than beside any
     * one of the ways a frame can end: potted out, conceded, forfeited on three
     * misses, the opponent leaving. Every one of them arrives through this
     * function, and a call wired to only the tidy ending would be missing from
     * exactly the endings that need explaining. */
    if (S.rules.kind && !S.stat_folded) cuevr_refcall_say(CUEVR_SAY_FRAME);
    if (!S.stat_folded) { S.stat_folded = 1; stat_frame_into_match(); }
    S.state = ST_OVER;
    S.hud_dirty = 1;
}

/* ---- table setup -------------------------------------------------------- */

static void think_start(void);
static void think_join(void);

static void start_frame(CueGameKind kind) {
    /* A re-rack while the opponent is mid-plan would move every ball out from
     * under the thread reading them. */
    think_join();
    S.stat_counted = 0;
    S.stat_visit_owner = -1;
    S.stat_visit_full = 0;
    stat_frame_reset();
    stat_match_reset();      /* a new game, not the next frame of this one */
    /* AND IT IS NOT THE CHALLENGE. Leaving a clearance through the pause menu
     * set the state back to the main menu but left S.mini standing, so the
     * challenge scoreboard — clock, balls left and all — stayed up over the
     * next frame of snooker. Every new frame ends the challenge; there is no
     * path where a rack and a running clearance are both true. */
    S.mini = S.mini_done = S.mini_beat = 0;
    S.mini_t = 0.0f;
    /* A CHALLENGE IS NOT A FRAME EITHER, and for exactly the same reason: a new
     * rack ends it, and leaving it standing put the challenge board over the
     * game that replaced it. drill_start racks through here on purpose, so it
     * sets S.drill AFTER calling this. */
    S.drill = -1;
    S.drill_done = S.drill_beat = S.drill_won = 0;
    S.drill_t = 0.0f;
    cue_table_init(&S.tab, kind);
    /* The player's two table colours, from the authored palettes in cue_theme.h
     * — the same values the handheld offers, not a second set invented here. */
    S.tab.cloth    = cloth_colour(S.cloth_idx);
    S.tab.rail     = k_frame_rail[S.frame_idx];
    S.tab.rail_top = k_frame_top[S.frame_idx];

    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    /* Only VS CPU has a CPU. Practice and online are both "player 1 is a person",
     * and in practice that person is also you. */
    cue_rules_init(&S.rules, &S.tab, S.opp == OPP_CPU);
    S.rules.best_of = MATCH_LEN[S.match_idx];
    /* WHO BREAKS IS DRAWN, not assumed. It was player 0 every time — you against
     * the CPU, the host online, and in a single-frame match that is the whole
     * game decided before a ball is struck. Practice is the exception: there is
     * nobody to lose the toss to. */
    cue_rules_set_break(&S.rules, S.opp == OPP_PRACTICE ? 0 : S.break_first);
    S.rules.ball_in_hand = 1;              /* you break from the D / the string */
    cuevr_render_set_table(&S.tab, &S.world);
    /* Re-racking the balls does not re-make the player. The bridge height and
     * the grip are things a player HAS, not things a frame has — and wiping
     * them here was what made the rest snap back to default. */
    {
        MoteVrV3 keep_rest = S.cue.rest; float keep_grip = S.cue.grip;
        cuevr_cue_init(&S.cue);
        S.cue.rest      = keep_rest;
        S.cue.grip      = keep_grip;
    }
    S.have_snap = 0;
    S.shot_events = 0;
    snprintf(S.msg, sizeof S.msg, "%s", MENU[S.menu_sel].name);
    S.msg_time = 3.0f;
    S.hud_dirty = 1;
}

/* The menu previews itself on the real table.
 *
 * Choosing a 12 ft snooker table, claret cloth and ebony frame from a text list
 * and only finding out what you picked after the break is a poor way to choose.
 * Every change re-racks the table in front of you, so the menu IS the preview.
 * Only on change — this rebuilds meshes and re-bakes the ball atlas. */
/* Re-dress the table that is already there. menu_preview() re-RACKS, which is
 * exactly what the main menu wants and exactly what a live frame does not: the
 * appearance screen can be opened mid-break, and changing the cloth must not
 * put the balls back. Colours and the render's copy of the table only. */
static void restyle_table(void) {
    S.tab.cloth    = cloth_colour(S.cloth_idx);
    S.tab.rail     = k_frame_rail[S.frame_idx];
    S.tab.rail_top = k_frame_top[S.frame_idx];
    cuevr_render_set_table(&S.tab, &S.world);
    S.hud_dirty = 1;
}

static void menu_preview(void) {
    cue_table_init(&S.tab, MENU[S.menu_sel].kind);
    S.tab.cloth    = cloth_colour(S.cloth_idx);
    S.tab.rail     = k_frame_rail[S.frame_idx];
    S.tab.rail_top = k_frame_top[S.frame_idx];
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    cue_render_set_ball_set(S.ballset);
    cuevr_render_set_table(&S.tab, &S.world);
    S.hud_dirty = 1;
}

/* Whose turn it is, in one place. Three modes used to mean three copies of
 * "(rules.cpu && turn == 1) ? THINK : AIM" scattered about, which is how a mode
 * ends up almost working: practice never hands the table over at all, and online
 * waits on the wire rather than on the planner. */
/* Put the cue ball in somebody's hand: on its home spot, legal by construction,
 * for them to walk about before playing. Returns 1 if the table is now waiting
 * on YOUR placement; the CPU and the far end place for themselves.
 *
 * Every frame starts here, because every one of these games breaks with the ball
 * in hand — from the D on a snooker or UK table, from behind the head string on
 * an American one. Racking straight into AIM skipped that, so the break was
 * always played from wherever cue_table_rack happened to leave the ball. */
static int take_ball_in_hand(void) {
    S.balls[0].pos = cue_table_cue_home(&S.tab);
    S.balls[0].vel = (Vec3){0, 0, 0};
    S.balls[0].w   = (Vec3){0, 0, 0};
    S.balls[0].on  = 1;
    S.rules.ball_in_hand = 0;
    if (S.opp == OPP_ONLINE && S.rules.turn != S.net_me) return 0;
    if (S.rules.cpu && S.rules.turn == 1) {
        /* Not on the break. cue_ai_place is a foul-recovery search — it looks
         * for the best cut at an object ball, and a full rack has no such thing.
         * The home spot IS the break position. */
        if (!S.rules.break_shot)
            S.balls[0].pos = cue_ai_place(&S.world, &S.tab, &S.rules, S.balls,
                                          S.nballs, &CUE_PERSONAS[S.persona],
                                          &S.rng);
        return 0;
    }
    S.state = ST_PLACE;
    S.place_latch = 1;
    S.recentre = 1;
    S.hud_dirty = 1;
    return 1;
}

/* Back to whatever the MENU tap interrupted — the shot that is still rolling,
 * the opponent that is still thinking, the ball you were still placing. */
static void unpause(void) {
    int back = S.pause_from;
    if (S.rules.frame_over) back = ST_OVER;
    /* RESUME PUTS YOU AT THE TABLE, never back on a screen. Every menu belongs
     * in this list, and the ones added since did not get into it — so pausing
     * from the pocket tuner and resuming returned you straight to the pocket
     * tuner, round and round, with no way back to a game. */
    else if (back == ST_PAUSE || back == ST_MENU || back == ST_SETUP
             || back == ST_LOBBY
             || back == ST_APPEAR || back == ST_STATS || back == ST_CLOTH
             || back == ST_DRILLS || back == ST_LAYOUT || back == ST_DRILLSET
             || back == ST_CONTROLS || back == ST_POCKETS
             || back == ST_CARSETUP || back == ST_CAREER
             || back == ST_CARTABLE || back == ST_CARACH) back = ST_AIM;
    /* and the press that resumed must not also put the ball down */
    if (back == ST_PLACE) S.place_latch = 1;
    S.state = back;
    S.hud_dirty = 1;
}

/* The colour the striker is aiming at, as its value 2..7, or 0.
 *
 * A snooker player names their colour out loud; here you name it by pointing the
 * cue at it, which is the same act and costs no menu. Cast the aim line from the
 * cue ball and take the first ball it would reach — that IS the nomination,
 * because it is the ball the cue ball is about to hit. Manual nomination stays
 * in the pause menu for the shot you mean to play off a cushion. */
/* The first ball the cue ball would reach along the aim line, as a ball id, or
 * -1. aimed_colour() below narrows that to a colour value. */
static int aimed_ball(void) {
    MoteVrV3 d = cuevr_room_dir_to_table(&S.setup.place, S.cue.aim_dir);
    float dx = d.x, dz = d.z;
    float l = sqrtf(dx*dx + dz*dz);
    if (l < 1e-4f) return 0;
    dx /= l; dz /= l;
    const float R = S.tab.R;
    float best_t = 1e9f; int best = 0;
    for (int i = 1; i < S.nballs; i++) {
        if (!S.balls[i].on) continue;
        float ox = S.balls[i].pos.x - S.balls[0].pos.x;
        float oz = S.balls[i].pos.z - S.balls[0].pos.z;
        float tt = ox*dx + oz*dz;                    /* along the aim */
        if (tt <= 0.0f) continue;                    /* behind the cue ball */
        float px = ox - dx*tt, pz = oz - dz*tt;      /* perpendicular miss */
        if (px*px + pz*pz > (2.0f*R)*(2.0f*R)) continue;
        if (tt < best_t) { best_t = tt; best = S.balls[i].id; }
    }
    return best ? best : -1;
}

static int aimed_colour(void) {
    if (!S.rules.kind || S.rules.target != 1) return 0;
    int id = aimed_ball();
    if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) return id - CUE_ID_YELLOW + 2;
    return 0;
}

static void hand_over(void) {
    if (!S.rules.nominated && !S.rules.free_ball_id) S.nom_manual = 0;
    if (S.rules.frame_over) { enter_over(); return; }
    /* Before whose-turn routing: nobody can aim until the ball is down. */
    if (S.rules.ball_in_hand && take_ball_in_hand()) return;
    if (S.opp == OPP_PRACTICE) {
        /* A free table. Fouls are still called and still shown, but the table
         * never changes hands — you are here to play the shot again, not to be
         * punished for it. */
        S.rules.turn = 0;
        S.state = ST_AIM;
    } else if (S.opp == OPP_ONLINE) {
        S.state = (S.rules.turn == S.net_me) ? ST_AIM : ST_THINK;
    } else if (S.rules.cpu && S.rules.turn == 1) {
        /* Whether it concedes is think_start's question, not this one — see
         * there. It lived here, and here is a place an ordinary shot never
         * reaches. */
        S.state = ST_THINK;
        think_start();
    } else {
        S.state = ST_AIM;
    }
    S.hud_dirty = 1;
}

/* Everything a shot changes, saved the instant before it happens. */
static void snap_take(void) {
    if (getenv("CUEVR_SNAPLOG"))
        LOGI("[cuevr] snapshot taken for player %d's shot", S.rules.turn);
    memcpy(S.snap_balls, S.balls, sizeof S.balls);
    S.snap_n = S.nballs;
    S.snap_rules = S.rules;
    S.have_snap = 1;
}
/* Put the BALLS back where they were before the shot, and nothing else.
 *
 * "Play it again" after a foul and a miss rewinds the TABLE, not the frame: the
 * penalty stands, the score stands, the decision has been applied. snap_restore
 * below takes the rules back with it, which would undo the foul itself — and
 * the human's replay was applying the decision without restoring anything at
 * all, so the striker was made to "play again" from wherever the balls had
 * finished up. */
static void snap_restore_balls(void) {
    think_join();
    if (!S.have_snap) return;
    memcpy(S.balls, S.snap_balls, sizeof S.balls);
    S.nballs = S.snap_n;
    for (int i = 0; i < S.nballs; i++) {
        S.balls[i].vel = v3(0, 0, 0);
        S.balls[i].w   = v3(0, 0, 0);
    }
}

static int snap_restore(void) {
    think_join();          /* undo rewrites the balls; see think_start */
    if (!S.have_snap) return 0;
    memcpy(S.balls, S.snap_balls, sizeof S.balls);
    S.nballs = S.snap_n;
    S.rules = S.snap_rules;
    S.have_snap = 0;             /* one level of undo, and it is spent */
    for (int i = 0; i < S.nballs; i++) {
        S.balls[i].vel = v3(0, 0, 0);
        S.balls[i].w = v3(0, 0, 0);
    }
    return 1;
}

/* ---- toasts -------------------------------------------------------------- */

static void toast_push(int kind, const char *title, const char *body) {
    /* "New record." — said HERE, once, rather than beside each of the six
     * things that can set one. Every record in the game already funnels through
     * this call to get its banner — the break best, the tier counts, the pool
     * clearance, a drill, the six-ball challenge — so hooking the banner is
     * what makes "whenever a record is broken in anything" true by
     * construction instead of true until somebody adds a seventh.
     *
     * QUEUED, not spoken over the top: a record break in snooker has just had
     * its total called, and interrupting "fifty seven" to say "new record"
     * loses the number that made it one. Behind it, they are a sentence. */
    if (kind == TOAST_RECORD) {
        /* Once per shot, however many records it set. Beating your best break
         * AND passing a tier is two records off one pot, and a referee does not
         * say "new record, new record" — the banners already show both. */
        static int said_frame = -1;
        if (said_frame != S.dbg_frame) {
            said_frame = S.dbg_frame;
            cuevr_refcall_say_after(CUEVR_SAY_RECORD);
        }
    }
    if (S.toast_n >= (int)(sizeof S.toast / sizeof S.toast[0])) return;
    int i = S.toast_n++;
    snprintf(S.toast[i].title, sizeof S.toast[i].title, "%s", title);
    snprintf(S.toast[i].body,  sizeof S.toast[i].body,  "%s", body ? body : "");
    S.toast[i].kind = kind;
    if (S.toast_n == 1) S.toast_t = 0.0f;
    /* Felt as well as seen: you may be looking at the table, not the board. */
    mote_xr_haptic(0.55f, 90);
    cue_audio_sfx(CUE_SFX_POT, 0.7f);
    S.hud_dirty = 1;
}

/* How long each one stays up, and how they retire. */
#define CUEVR_TOAST_SECS 4.0f
static void toast_tick(float dt) {
    if (!S.toast_n) return;
    S.toast_t += dt;
    if (S.toast_t >= CUEVR_TOAST_SECS) {
        S.toast_n--;
        for (int i = 0; i < S.toast_n; i++) S.toast[i] = S.toast[i + 1];
        S.toast_t = 0.0f;
    }
    S.hud_dirty = 1;
}

/* A ball's name in a few characters, for a menu row that has to say WHICH ball
 * without room for a sentence. */
static const char *cue_ball_short_name(int id) {
    static const char *C[6] = { "YELLOW", "GREEN", "BROWN", "BLUE", "PINK", "BLACK" };
    static char num[8];
    if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) return C[id - CUE_ID_YELLOW];
    if (id >= 1 && id <= 15) {
        /* On a snooker table 1..15 are all reds; on a pool one they are
         * numbered balls and the number is the name. */
        if (S.tab.is_snooker) return "RED";
        snprintf(num, sizeof num, "%d", id);
        return num;
    }
    return "-";
}

/* ---- career ------------------------------------------------------------- */

#if CUEVR_TUNE_POCKETS
/* WRITE THE POCKET NUMBERS OUT, so they can be got off the headset and into
 * the source. Next to the preferences — internalDataPath on Android — and the
 * exact lines the code wants, not a report about them: the point is that they
 * can be pasted straight in without anybody transcribing a decimal. */
static void pockets_write(void) {
    if (!S.car_path[0]) return;
    char path[560];
    snprintf(path, sizeof path, "%s", S.car_path);
    char *slash = strrchr(path, '/');
    if (slash) slash[1] = 0; else path[0] = 0;
    strncat(path, "cuevr_pockets.txt", sizeof path - strlen(path) - 1);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "CueVR pocket cut, tuned in the headset\n\n");
    fprintf(f, "corner size     %d%%\n", S.cut_cr);
    fprintf(f, "corner set back %d mm\n", S.cut_cs);
    fprintf(f, "middle size     %d%%\n", S.cut_mr);
    fprintf(f, "middle set back %d mm\n\n", S.cut_ms);
    fprintf(f, "-- paste into games/thumbycue/src/cue_render.c --\n");
    fprintf(f, "#define CUE_CUT_CORNER  %.3ff\n", S.cut_cr / 100.0f);
    fprintf(f, "#define CUE_CUT_MIDDLE  %.3ff\n", S.cut_mr / 100.0f);
    fprintf(f, "#define CUE_COR_SETBACK %.4ff\n", S.cut_cs / 1000.0f);
    fprintf(f, "#define CUE_MID_SETBACK %.4ff\n", S.cut_ms / 1000.0f);
    fclose(f);
    LOGI("[cuevr] pocket numbers written to %s", path);
}

/* And into the log, every time they change. The log is the thing that gets sent
 * back; a file on the headset is one more step for somebody to take. */
static void pockets_log(void) {
    LOGI("[cuevr] POCKET CUT  corner %d%% set back %d mm   middle %d%% set back %d mm"
         "   (CUE_CUT_CORNER %.3ff CUE_COR_SETBACK %.4ff"
         " CUE_CUT_MIDDLE %.3ff CUE_MID_SETBACK %.4ff)",
         S.cut_cr, S.cut_cs, S.cut_mr, S.cut_ms,
         (double)(S.cut_cr/100.0f), (double)(S.cut_cs/1000.0f),
         (double)(S.cut_mr/100.0f), (double)(S.cut_ms/1000.0f));
}

#endif

static void career_save(void) {
    if (S.career.active && S.car_path[0]) cuevr_career_save(&S.career, S.car_path);
}

/* Begin the fixture that is next in the queue: the league's table, the CPU at
 * the persona you are drawn against, the division's match length. */
static void career_play(void) {
    const CueVrCarFixture *fx;
    int l = cuevr_career_next(&S.career, &fx);
    if (l < 0 || !fx) return;
    int opp = cuevr_career_opponent(fx);
    int bo  = cuevr_career_bestof(&S.career, l);

    S.opp = OPP_CPU;                    /* it IS a match against the CPU */
    S.persona = (opp >= 0 && opp < CUE_NUM_PERSONAS) ? opp : 0;
    for (int i = 0; i < MATCH_LEN_N; i++) if (MATCH_LEN[i] == bo) S.match_idx = i;
    for (int i = 0; i < MENU_N; i++)
        if ((int)MENU[i].kind == S.career.league[l].kind) S.menu_sel = i;
    S.in_career = 1;
    S.car_league = l;
    S.break_first = coin_toss();
    cue_render_set_ball_set(S.ballset);
    start_frame(MENU[S.menu_sel].kind);
    hand_over();
}

/* The match is over and the ledger wants it. */
static void career_finish(void) {
    if (!S.in_career) return;
    S.in_career = 0;
    int me = 0;
    int mine = S.rules.frames[me], theirs = S.rules.frames[1 - me];
    int won  = mine > theirs;
    /* Only snooker has a break worth recording; a pool "break" is a ball count
     * and would sit in the same column meaning something else. */
    int hb = S.tab.is_snooker ? S.mstat[me].best_break : 0;
    uint32_t before = S.career.ach;
    cuevr_career_record(&S.career, S.car_league, won, mine, theirs, hb);
    /* Whatever lit up during that, said out loud. Diffing the mask beats
     * threading a callback through the ledger, and it cannot miss one. */
    for (int i = 0; i < CAR_ACH_N; i++)
        if (!(before & (1u << i)) && (S.career.ach & (1u << i)))
            toast_push(TOAST_ACH, cuevr_car_ach_name(i), cuevr_car_ach_how(i));
    career_save();
    S.opp = OPP_CAREER;
    S.state = ST_CAREER;
    S.car_row = 0;
    S.hud_dirty = 1;
}

/* ---- the six-ball clearance --------------------------------------------- *
 *
 * Practice with a scoreboard on it: six balls in a small triangle, a clock that
 * starts on your first strike and stops on the last ball down, and the best
 * time you have ever set on THIS table sitting next to it to be beaten.
 *
 * The rules engine is stepped aside for the duration (see resolve_shot): six
 * reds and no colours is not a position snooker has an opinion about, and a
 * challenge that called half your pots fouls would be a worse game than no
 * challenge at all. Any ball down counts; the white comes back if you lose it.
 */
static void mini_start(void) {
    think_join();
    S.nballs = cue_table_rack_six(&S.tab, S.balls);
    cue_rules_init(&S.rules, &S.tab, 0);
    S.rules.turn = 0;
    /* Place the white ONCE, to break with — and clear the flag here rather than
     * leaving it set, which is what put the ball back in your hand after every
     * single shot of a run. You get it back only if you pot it. */
    S.rules.ball_in_hand = 0;
    S.balls[0].pos = cue_table_cue_home(&S.tab);
    S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);
    S.balls[0].on = 1;
    S.mini = 1;
    S.mini_done = 0;
    S.mini_beat = 0;
    S.mini_t = 0.0f;
    S.have_snap = 0;
    S.shot_events = 0;
    stat_frame_reset();
    snprintf(S.msg, sizeof S.msg, "CLEAR SIX - GO");
    S.msg_time = 2.5f;
    S.state = ST_PLACE;
    S.place_latch = 1;
    S.recentre = 1;
    S.hud_dirty = 1;
}

static void mini_stop(void) {
    S.mini = 0;
    S.mini_done = 0;
    rerack();
    hand_over();
}

/* THE CLOTH, LIFTED.
 *
 * The palette is measured off a photograph of the manufacturer's swatch card,
 * which is honest and comes out dull: cloth is a nap, photographed under a
 * light, and the numbers that describe a patch of it on paper are not the
 * numbers that make a table look like that cloth. Saturation up and a gamma
 * lift on the value — gamma rather than a multiply so the dark ones move most
 * and the bright ones barely at all, which is where the dullness actually is.
 *
 * CUEVR_CLOTHLIFT=sat,gamma overrides it so the strengths can be rendered and
 * compared rather than argued about. CHAMPIONSHIP is left alone: it was not
 * measured off the card and it already reads right. */
#ifndef CUEVR_CLOTH_SAT
#define CUEVR_CLOTH_SAT   1.25f
#endif
#ifndef CUEVR_CLOTH_GAMMA
#define CUEVR_CLOTH_GAMMA 0.82f
#endif

static uint16_t cloth_colour(int i) {
    if (i < 0 || i >= CUE_NCLOTH) i = 0;
    uint16_t c = k_cloth[i];
    if (i == 9) return c;                       /* CHAMPIONSHIP, as authored */
    float sat = CUEVR_CLOTH_SAT, gam = CUEVR_CLOTH_GAMMA;
    { const char *v = getenv("CUEVR_CLOTHLIFT");
      if (v) { float a, b; if (sscanf(v, "%f,%f", &a, &b) == 2) { sat = a; gam = b; } } }
    if (sat == 1.0f && gam == 1.0f) return c;
    float r = (float)((c >> 11) & 31) / 31.0f;
    float g = (float)((c >>  5) & 63) / 63.0f;
    float b = (float)( c        & 31) / 31.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float v2 = powf(mx, gam);
    /* Saturation about the maximum channel, which is the same as an HSV
     * saturate without the round trip through hue. */
    float s2 = (mx > 1e-5f) ? (mx - mn) / mx * sat : 0.0f;
    if (s2 > 1.0f) s2 = 1.0f;
    float lo = v2 * (1.0f - s2);
    float sc = (mx - mn > 1e-5f) ? (v2 - lo) / (mx - mn) : 0.0f;
    r = lo + (r - mn) * sc; g = lo + (g - mn) * sc; b = lo + (b - mn) * sc;
    #define Q(x, n) ((uint16_t)((x) < 0 ? 0 : (x) > 1 ? (n) : (int)((x) * (n) + 0.5f)))
    return (uint16_t)((Q(r,31) << 11) | (Q(g,63) << 5) | Q(b,31));
    #undef Q
}

/* ---- practice drills ---------------------------------------------------- */

/* What a ball is worth. Snooker prices its colours; in pool every ball is one
 * ball, and a drill that says SCORE 5 on a pool table means five of them. */
static int drill_value(int id) {
    if (!S.rules.kind) return 1;
    if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) return id - CUE_ID_YELLOW + 2;
    return (id >= 1 && id <= 15) ? 1 : 0;
}

/* WHICH BIT OF `need` A POTTED BALL SATISFIES.
 *
 * The ball grid offers ONE entry for the reds, because a drill that wants a red
 * wants any of them and fifteen identical buttons would be a worse screen — so
 * the mask carries bit 1 for "a red" and any red id has to answer to it. Every
 * other ball is its own id. Ids above 31 cannot be held in a 32-bit mask and
 * there are none: pool runs to 15 and snooker's colours stop at 25. */
static uint32_t drill_need_bit(int id) {
    if (S.tab.is_snooker && id >= 1 && id <= 15) return 1u << 1;
    return (id >= 0 && id < 32) ? (1u << id) : 0u;
}

/* The table as it stands, into a slot. Table space, so it survives the table
 * being moved or turned or set up in another room. */
static void drill_capture(CueVrDrill *d) {
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->kind = (uint8_t)S.tab.kind;
    d->n = (uint8_t)(S.nballs > CUEVR_DRILL_MAXBALLS ? CUEVR_DRILL_MAXBALLS : S.nballs);
    for (int i = 0; i < d->n; i++) {
        d->id[i] = (uint8_t)S.balls[i].id;
        d->on[i] = (uint8_t)S.balls[i].on;
        d->x[i]  = S.balls[i].pos.x;
        d->z[i]  = S.balls[i].pos.z;
    }
    d->goal = CUEVR_GOAL_SETUP;
    d->target = 20;
    /* A sensible ball to nominate if the goal is later set to POT: the dearest
     * one still on the table, which is the one anybody would pick. */
    int best = 0;
    for (int i = 1; i < d->n; i++)
        if (d->on[i] && d->id[i] > best) best = d->id[i];
    d->ball = (uint8_t)best;
}

/* THE TABLE THE POSITION WAS SET OUT ON.
 *
 * A snooker position means nothing on a pub table, and it is a kindness to
 * switch rather than refuse. This has to happen wherever a saved layout is put
 * back — PLAYING one did it, EDITING one did not, so opening a snooker
 * challenge to move its balls while a UK8 table was up laid twenty-two balls
 * out to snooker's dimensions on a table two-thirds the size: everything
 * bunched into the middle with the outer ones jammed in the cushions. */
static void drill_use_table(const CueVrDrill *d) {
    if ((int)S.tab.kind == (int)d->kind) return;
    for (int i = 0; i < MENU_N; i++)
        if ((int)MENU[i].kind == (int)d->kind) S.menu_sel = i;
    start_frame((CueGameKind)d->kind);
}

/* And back onto the cloth. The rack is rebuilt first so the ball ARRAY is the
 * right shape for the table — a saved 22-ball snooker layout dropped onto a
 * 16-ball pool table would otherwise index past the end of it. */
static void drill_restore(const CueVrDrill *d) {
    S.nballs = cue_table_rack(&S.tab, S.balls);
    int n = d->n < S.nballs ? d->n : S.nballs;
    for (int i = 0; i < n; i++) {
        S.balls[i].on  = d->on[i];
        S.balls[i].pos = v3(d->x[i], S.tab.R, d->z[i]);
        S.balls[i].vel = v3(0,0,0);
        S.balls[i].w   = v3(0,0,0);
    }
    /* Anything the layout did not mention is off: a saved position is the WHOLE
     * table, not a patch on top of a fresh rack. */
    for (int i = n; i < S.nballs; i++) S.balls[i].on = 0;
    S.balls[0].on = 1;
}

/* Start a drill: the layout back as it was, the clock at zero, the visit
 * empty. A drill you have to reset by hand is a drill you play once. */
static void drill_start(int slot) {
    if (slot < 0 || slot >= CUEVR_DRILL_SLOTS || !S.drills.slot[slot].used) return;
    const CueVrDrill *d = &S.drills.slot[slot];
    think_join();
    drill_use_table(d);
    cue_rules_init(&S.rules, &S.tab, 0);
    S.rules.turn = 0;
    S.rules.ball_in_hand = 0;
    drill_restore(d);
    S.mini = 0; S.mini_done = 0;
    S.drill = slot;
    S.drill_done = S.drill_won = S.drill_beat = 0;
    S.drill_t = 0.0f;
    S.drill_score = S.drill_pots = 0;
    S.drill_got = 0;
    S.have_snap = 0;
    S.shot_events = 0;
    stat_frame_reset();
    {   char nm[24]; cuevr_drill_name(d, nm, sizeof nm);
        snprintf(S.msg, sizeof S.msg, "%s", nm); }
    S.msg_time = 2.5f;
    arm_shot();
    S.state = ST_AIM;
    S.hud_dirty = 1;
}

/* Put it back for another go, which is what practice is. */
static void drill_again(void) {
    if (S.drill < 0) return;
    int slot = S.drill;
    S.drill = -1;
    drill_start(slot);
}

static void drill_stop(void) {
    S.drill = -1;
    S.drill_done = 0;
    rerack();
    hand_over();
}

/* How many of the six are still up. */
static int mini_left(void) {
    int n = 0;
    for (int i = 1; i < S.nballs; i++) if (S.balls[i].on) n++;
    return n;
}

/* Re-rack without moving the table: the frame starts again, the room does not. */
static void rerack(void) {
    S.nballs = cue_table_rack(&S.tab, S.balls);
    cue_rules_init(&S.rules, &S.tab, S.opp == OPP_CPU);
    S.rules.ball_in_hand = 1;
    S.have_snap = 0;
    S.shot_events = 0;
    stat_frame_reset();
    snprintf(S.msg, sizeof S.msg, "RE-RACKED");
    S.msg_time = 2.0f;
    S.hud_dirty = 1;
}

/* ---- the HUD ------------------------------------------------------------ */

#define HW 128   /* layout space — NOT CUEVR_HUD_W, which is 4x this */

/* THE CLOTH CARD. One set of numbers, used by the drawing and by the pointer
 * hit test, because two sets drift and a swatch you cannot press is worse than
 * no grid at all. Twenty-three swatches, six to a row of four, in the card's
 * own order. */
#define CLOTH_COLS 4
#define CLOTH_ROWS ((CUE_NCLOTH + CLOTH_COLS - 1) / CLOTH_COLS)
#define CLOTH_X0   6
#define CLOTH_Y0   12
#define CLOTH_CW   30
#define CLOTH_CH   12
/* The name line, then the way out. A grid with no way off it is a grid you
 * leave by guessing at a button — B works and always did, but a screen that
 * only answers to a button nothing on it mentions is a screen people get stuck
 * on. Below the swatches, where you already are once you have picked one.
 *
 * The whole thing has to land inside CUEVR_HUD_LH, which is 112 rows: the first
 * version of this laid out to 129, hud_height clamped it, and the BACK row was
 * simply off the bottom of the panel — present, pressable by pointer, and
 * invisible. Twenty-four swatches, a name and a way out fit in 112 with these
 * numbers and not with much bigger ones. */
#define CLOTH_NAME_Y (CLOTH_Y0 + CLOTH_ROWS * CLOTH_CH + 3)
/* The back row's own "index", so one hover variable covers the whole
 * screen and there is no second piece of state to get out of step. */
#define CLOTH_BACK   (-2)

/* A GALLERY, NOT A LIST.
 *
 * A challenge IS a position. Its name — "SNK POT BLACK", "UK8 POSITION (7)" —
 * is a label somebody had to invent for it, and it cannot say the one thing you
 * want to know when you are looking for the one you meant: where the balls are.
 * So each is a card with the table drawn on it, and you pick the picture.
 *
 * The chevrons are gone with the list. They meant "click the middle to play,
 * click the edge to set it up", which is a thing you have to be told and then
 * remember, on a row that gives no sign of it. A card OPENS — and PLAY is the
 * first thing on the page it opens, so playing is still one tap and a press
 * rather than a hunt. */
#define DRILL_COLS  2
#define DRILL_VROWS 2           /* card rows on screen at once */
#define DRILL_VIS   (DRILL_COLS * DRILL_VROWS)
#define DRILL_X0    4
#define DRILL_Y0    13
#define DRILL_CW    60          /* card pitch */
#define DRILL_CH    42
#define DRILL_ROW_TIMED  0
#define DRILL_ROW_SLOT0  1
/* The setup screen's rows, and the grid of balls under them. */
enum { DSET_GOAL = 0, DSET_TARGET, DSET_TIMED, DSET_PLAY, DSET_EDIT, DSET_DEL,
       DSET_BACK, DSET_N };
#define DSET_BX     12
#define DSET_BW     14
#define DSET_BH     14
#define DSET_BCOLS  8
/* Only the ones that EXIST, plus the built-in and the way to make another.
 * Padding the list out to eight EMPTY rows advertises a limit nobody asked
 * about and buries the one row that does something. */
static int drill_slot_at(int row);
static int drill_rows(void) {
    int n = DRILL_ROW_SLOT0;
    for (int i = 0; i < CUEVR_DRILL_SLOTS; i++) if (S.drills.slot[i].used) n++;
    return n + 1;                                  /* ...and MAKE A CHALLENGE */
}
/* Which card a slot is, the inverse of drill_slot_at. NOT slot + 1: the gallery
 * only shows slots that EXIST, so slot 5 is the second card if 0..4 are empty.
 * Adding the offset directly put the cursor on somebody else's challenge. */
static int drill_card_of(int slot) {
    int k = DRILL_ROW_SLOT0;
    for (int i = 0; i < CUEVR_DRILL_SLOTS && i < slot; i++)
        if (S.drills.slot[i].used) k++;
    return k;
}
/* Which slot a card is showing, or -1 for the built-in and the maker. */
static int drill_slot_at(int row) {
    if (row < DRILL_ROW_SLOT0) return -1;
    int k = row - DRILL_ROW_SLOT0;
    for (int i = 0; i < CUEVR_DRILL_SLOTS; i++)
        if (S.drills.slot[i].used && k-- == 0) return i;
    return -1;
}
#define CLOTH_BACK_Y (CLOTH_NAME_Y + 13)
/* How many rows the screen being drawn is using. It is NOT a constant: the
 * scoreboard is 16:9 and the list screens are taller (see CUEVR_HUD_LH). This
 * was hard-coded at 128 while the texture was only 72 rows deep, so every
 * screen's help line — and, once the menu grew, START GAME itself — was being
 * drawn past the bottom edge and simply never appeared. */
static int s_hud_rows = CUEVR_HUD_LH;
#define HH s_hud_rows

/* The HUD's layout is written in 128-space and drawn at CUEVR_HUD_SS times that.
 * Text is real Audiowide at the size it is actually seen, not the 3x5 handheld
 * font magnified — there is no detail in a 3x5 glyph to enlarge. The two baked
 * sizes stand in for the old 1x and 2x calls. */
#define HS CUEVR_HUD_SS

static int hud_text(const char *t, int x, int y, uint16_t c) {
    /* y was a 6px-cell top-left in 128-space; Audiowide's box is taller, so nudge
     * it up a touch to keep the old baselines looking right. */
    return cuevr_text_draw(S.hud, CUEVR_HUD_W, CUEVR_HUD_H, &cuevr_font_md,
                           t, x * HS, y * HS - 2, c) / HS;
}
static int hud_text_2x(const char *t, int x, int y, uint16_t c) {
    return cuevr_text_draw(S.hud, CUEVR_HUD_W, CUEVR_HUD_H, &cuevr_font_lg,
                           t, x * HS, y * HS - 4, c) / HS;
}
static int hud_text_w(const char *t)    { return cuevr_text_width(&cuevr_font_md, t) / HS; }
static int hud_text_w_2x(const char *t) { return cuevr_text_width(&cuevr_font_lg, t) / HS; }

/* The ball graphics are drawn analytically from a centre and a radius, so they
 * come out at whatever size they are asked for — 4x here costs nothing but the
 * pixels. Wrappers so the call sites keep their 128-space numbers. */
static void cue_render_onball_icon_hs(int cx, int cy, int r, int tgt, int seq)
    { cue_render_onball_icon(S.hud, cx*HS, cy*HS, r*HS, tgt, seq); }
static void cue_render_group_icon_hs(int cx, int cy, int r, int g)
    { cue_render_group_icon(S.hud, cx*HS, cy*HS, r*HS, g); }
static void cue_render_ball_icon_hs(int cx, int cy, int r, int id)
    { cue_render_ball_icon(S.hud, cx*HS, cy*HS, r*HS, id); }

static void hud_face(int cx, int cy, int size, int persona)
    { cue_render_face(S.hud, cx*HS, cy*HS, size*HS, persona); }
static void cue_render_set_preview_hs(int cx, int cy, int r, int a, int b)
    { cue_render_set_preview(S.hud, cx*HS, cy*HS, r*HS, a, b); }

static void hud_clear(uint16_t c) {
    for (int i = 0; i < CUEVR_HUD_W * CUEVR_HUD_H; i++) S.hud[i] = c;
}

/* How tall the screen about to be drawn is, in layout rows. The renderer shapes
 * the panel to it, so a screen only has to say what it needs. */
static void hud_height(int rows) {
    if (rows < 16) rows = 16;
    if (rows > CUEVR_HUD_LH) rows = CUEVR_HUD_LH;
    s_hud_rows = rows;
    S.scene.hud_rows = rows;
}

static void hud_rect(int x, int y, int w, int h, uint16_t c) {
    x *= HS; y *= HS; w *= HS; h *= HS;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= CUEVR_HUD_H) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= CUEVR_HUD_W) continue;
            S.hud[j * CUEVR_HUD_W + i] = c;
        }
    }
}

static int hud_text_xl(const char *t, int x, int y, uint16_t c) {
    return cuevr_text_draw(S.hud, CUEVR_HUD_W, CUEVR_HUD_H, &cuevr_font_xl,
                           t, x * HS, y * HS - 5, c) / HS;
}
static int hud_text_w_xl(const char *t) { return cuevr_text_width(&cuevr_font_xl, t) / HS; }

/* Right-align helpers. A scoreboard's numbers line up on their right edge; a
 * scoreboard whose numbers jump left when a score passes 99 is a spreadsheet. */
static void hud_text_r(const char *t, int rx, int y, uint16_t c) {
    hud_text(t, rx - hud_text_w(t), y, c);
}
static void hud_text_r_xl(const char *t, int rx, int y, uint16_t c) {
    hud_text_xl(t, rx - hud_text_w_xl(t), y, c);
}

/* One option row of the menu. */
/* Which row the pointer is over, for a list drawn at y0 with `pitch` between
 * rows. -1 for none. Rows are drawn from y0-1 to y0+pitch-1, so the test uses
 * the same numbers hud_opt() draws with rather than a second set that can
 * drift from them. */
/* One step of a list per flick of a stick, either hand. Edge-triggered on the
 * way out of the dead zone, so a held stick moves one row and waits — a list
 * that scrolls at frame rate under your thumb is a list you cannot land on. */
static int stick_step(const MoteVrTracking *t) {
    static int held;
    float y = t->hand[MOTE_VR_LEFT].stick_y + t->hand[MOTE_VR_RIGHT].stick_y;
    if (y > 0.5f)  { if (held == 1) return 0; held = 1; return -1; }
    if (y < -0.5f) { if (held == 2) return 0; held = 2; return +1; }
    held = 0;
    return 0;
}

static int ptr_row_at(int y0, int pitch, int n) {
    if (!S.ptr_ok) return -1;
    float r = (S.ptr_y - (float)(y0 - 1)) / (float)pitch;
    if (r < 0.0f) return -1;
    int i = (int)r;
    return (i >= 0 && i < n) ? i : -1;
}

/* Where across a row the pointer sits: -1 left third, +1 right third, 0 middle.
 * The chevrons hud_opt draws are at 46 and HW-18, so those are the zones. */
static int ptr_zone(void) {
    if (!S.ptr_ok) return 0;
    if (S.ptr_x < 46.0f + 8.0f)      return -1;
    if (S.ptr_x > (float)HW - 26.0f) return +1;
    return 0;
}

/* A trigger pull, once. The trigger is the Quest select and this is the only
 * place that reads it as a button. */
static int ptr_click(const MoteVrTracking *t) {
    int down = t->hand[DOMH].trigger > 0.55f;
    if (!down) { S.ptr_latch = 0; return 0; }
    if (S.ptr_latch) return 0;
    S.ptr_latch = 1;
    return 1;
}

static void hud_opt(int row, const char *label, const char *value, int sel,
                    int enabled, uint16_t TXT, uint16_t DIM, uint16_t HI) {
    int y = 12 + row * 8;
    if (sel) hud_rect(1, y - 1, HW - 2, 8, RGB565C(30, 46, 72));
    hud_text(label, 4, y, sel ? HI : DIM);
    /* The right-hand 18 columns are the icon gutter — an avatar or a ball rack
     * lives there — so values are right-aligned to its edge rather than to the
     * panel's. Without the gutter the face landed on top of the persona's name. */
    if (value)
        hud_text_r(value, HW - 20, y, enabled ? (sel ? TXT : DIM)
                                              : RGB565C(70, 78, 92));
    if (sel && value) {
        hud_text("<", 46, y, HI);
        hud_text(">", HW - 18, y, HI);
    }
}

/* Paint the screen, then put the pointer on top of it.
 *
 * hud_paint returns early from every screen it draws, so the cursor cannot go
 * at its end — it would only ever appear on the last one. It goes in a wrapper.
 *
 * Drawn INTO the HUD bitmap rather than as geometry in the world: the panel is
 * already a textured quad, so this costs a handful of pixels where a laser and
 * a puck would be two more meshes and two more draws every frame. Seeing where
 * you point is the whole job. */
static void hud_paint(void);

/* A row that OPENS something, drawn so it cannot be mistaken for one that
 * CHANGES something.
 *
 * hud_opt puts a chevron either side of the value on the selected row, which is
 * the whole vocabulary of "left and right do things here". Giving a submenu a
 * value and letting it inherit that told the player they could scroll through
 * appearances, which they cannot. This has no chevrons, no value, a green
 * highlight rather than blue — the same green the START row uses for the other
 * thing on this screen that ACTS rather than adjusts — and the word OPEN where
 * a value would sit. */
/* EVERY choice the rules actually offer at this decision point, in one list.
 *
 * The HUD used to name two of them on A and B and the input used to act on two
 * of them, and which two depended on separate chains of if/else that had to
 * agree by hand. They did not: "play again from where they lie" — the commonest
 * answer to a foul — appeared in neither, and the B option after a miss restored
 * the whole layout while being labelled REPLAY, which reads as "play it again"
 * and does considerably more than that.
 *
 * Both the drawing and the acting walk THIS array. */
typedef struct { int dec; const char *label; const char *note; } DecOpt;

/* THE PAUSE MENU, only as long as it needs to be.
 *
 * Eleven fixed rows at nine apart put the last one at 104 with the help text at
 * 106 — the bottom of the list ran through the footer. Half of them do not
 * apply at any given moment either: NOMINATE and CONCEDE are snooker, UNDO is
 * practice, PICK UP is only live between placing the ball and playing. A menu
 * that lists what you cannot do, and overflows doing it, is worse than a short
 * one. Built from what actually applies, and both the drawing and the acting
 * walk it. */
typedef struct { int id; const char *label; } PsRow;

static int pause_rows(PsRow *o, int max) {
    int n = 0;
    #define ADD(i,l) do { if (n < max) o[n++] = (PsRow){ (i), (l) }; } while (0)
    ADD(PS_RESUME, "RESUME");
    /* THE BOARD SAYS "MENU - ANOTHER GO", so the menu has to have one. It said
     * that from the day the challenges were written and there was never a row
     * to press: MENU opened the pause list, the list had nothing about the
     * challenge on it, and the only way out of one was BACK TO MENU. And that
     * left the challenge running, so the clock stayed on the board over every
     * frame you played afterwards. */
    if (S.drill >= 0 || S.mini) {
        ADD(PS_AGAIN,   "ANOTHER GO");
        ADD(PS_ENDCHAL, "END CHALLENGE");
    }
    /* THE PRACTICE TOOLS LIVE ON THEIR OWN SCREEN. Undo, auto respot, the
     * timed clearance and the drills were four rows here, on a menu that fits
     * ten — adding the drills row pushed practice snooker to fourteen and the
     * last four rows were drawn straight through the help line. They belong
     * together anyway: they are the tools of a mode, not actions of a pause. */
    /* RE-RACK is not offered online: it rebuilds this end's table and the far
     * end would carry on with the old one. Undo is practice-only already, and
     * for the same reason. */
    if (S.can_repick)                          ADD(PS_PICKUP, "PICK UP BALL");
    /* Nominating is the STRIKER's act. Online both ends have target == 1 at the
     * same moment, so this was offered to the player who was not at the table —
     * and taking it set a colour in their copy of the rules that the striker had
     * never named. */
    int at_table = (S.opp != OPP_ONLINE) || (S.rules.turn == S.net_me);
    if (S.rules.kind && !S.rules.frame_over && S.rules.target == 1 && at_table)
        ADD(PS_NOMINATE, "NOMINATE");
    /* A FREE BALL IS NOMINATED TOO, and the row above cannot do it: that one is
     * gated on being on a COLOUR, and a free ball is usually awarded while you
     * are on a RED — so after taking one there was no way to name the ball from
     * this menu at all. Aiming at it always worked, but a mechanism nothing on
     * screen mentions is not a mechanism the player has. */
    if (S.rules.kind && !S.rules.frame_over && S.rules.free_ball && at_table)
        ADD(PS_FREEBALL, "FREE BALL");
    if (S.opp == OPP_PRACTICE) ADD(PS_PRACTICE, "PRACTICE");
    if (S.opp != OPP_ONLINE) ADD(PS_RERACK, "RE-RACK");
    ADD(PS_PLACE,  "PLACE TABLE");
    ADD(PS_APPEAR, "APPEARANCE");
    ADD(PS_CONTROLS, "CONTROLS");
    ADD(PS_STATS,  "RECORDS");
    /* NOT in practice: there is nobody to concede to, and it dumped you on a
     * frame-over screen for a frame nobody was contesting. */
    if (S.rules.kind && !S.rules.frame_over && S.opp != OPP_PRACTICE)
        ADD(PS_CONCEDE, "CONCEDE FRAME");
    ADD(PS_QUIT,   "BACK TO MENU");
    #undef ADD
    return n;
}

static int decision_options(DecOpt *o, int max) {
    int n = 0;
    if (S.rules.pushout_offer) {
        if (n < max) o[n++] = (DecOpt){ CUE_DEC_PLAY,  "PUSH OUT",  "play a free shot" };
        if (n < max) o[n++] = (DecOpt){ CUE_DEC_AGAIN, "PLAY ON",   "play it normally" };
        return n;
    }
    if (n < max) o[n++] = (DecOpt){ CUE_DEC_PLAY,  "TAKE THE SHOT",
                                    "you play the balls where they lie" };
    if (n < max) o[n++] = (DecOpt){ CUE_DEC_AGAIN, "MAKE THEM PLAY",
                                    "they shoot again from here" };
    if (S.rules.dec_can_restore && n < max)
        o[n++] = (DecOpt){ CUE_DEC_REPLAY, "PUT THE BALLS BACK",
                           "they replay the same shot" };
    if (S.rules.dec_free_ball && n < max)
        o[n++] = (DecOpt){ CUE_DEC_FREEBALL, "FREE BALL",
                           "you play, and name any ball" };
    return n;
}

static void hud_link(int row, const char *label, const char *act, int sel,
                     uint16_t DIM, uint16_t HI, uint16_t LIVE) {
    int y = 12 + row * 8;
    if (sel) hud_rect(1, y - 1, HW - 2, 8, RGB565C(28, 58, 40));
    hud_text(label, 4, y, sel ? HI : DIM);
    hud_text_r(act, HW - 20, y, sel ? LIVE : DIM);
}

static void hud_build(void) {
    hud_paint();
    /* No cursor painted into the panel: the laser and its bead are real
     * objects in the room now, which is what a pointer should be. A square
     * target stamped into the texture read as part of the interface rather
     * than as something you were holding. */
}

/* A name that fits the room it has been given.
 *
 * The stats screen has two number columns and the opponent's is only as wide as
 * the widest figure in it; "Professor Pete" ran straight through "YOU" in the
 * next column along. Every persona is a two-word name, and the second word is
 * the one people use — Pete, Hank, Nina — so a name that will not fit whole
 * falls back to that before it falls back to being chopped mid-word. */
static const char *hud_fit_name(char *dst, size_t n, const char *name, int room) {
    if (hud_text_w(name) <= room) return name;
    const char *last = strrchr(name, ' ');
    if (last && hud_text_w(last + 1) <= room) return last + 1;
    const char *src = last ? last + 1 : name;
    size_t k = strlen(src);
    if (k > n - 1) k = n - 1;
    memcpy(dst, src, k); dst[k] = 0;
    while (k > 1 && hud_text_w(dst) > room) dst[--k] = 0;
    return dst;
}

/* A break drawn as the balls that made it.
 *
 * This is how the 2D game showed one and it is how a player thinks about one:
 * "56" is a number you take on trust; five reds, four blacks and a pink IS the
 * break — you can read its shape, whether it was the black off the spot over
 * and over or a scramble round the colours.
 *
 * The COUNT is what is written, and it is written in the ball's own colour, so
 * the row reads without a legend. Snooker's fifteen reds are one entry: they
 * are one ball as far as a break is concerned. Pool has no repeats to count, so
 * it draws the balls themselves, which is the same information.
 *
 * Snooker's black is very nearly the panel's own background, so dark balls are
 * lifted until they can be read — a legend nobody can see is not a legend. */
/* The number goes INSIDE the ball, the way a pool ball carries its own. So the
 * ink has to be whatever reads on that ball — white on a red or a black, dark
 * on a yellow — which is a decision about the ball's brightness, not a palette.
 */
/* THE GROUND BALLS ARE DRAWN ON.
 *
 * The panel's own background is very nearly black, and the snooker black on it
 * is not a ball — it is a hole with a highlight on it, and the blue and the
 * brown are barely better. The frame-over screen already solved this with a
 * bluer plate under its break rows; this is that colour, named, so every place
 * that draws a ball can stand it on the same ground instead of one screen
 * having the fix and the rest not. */
#define HUD_BALLBG RGB565C(22, 32, 52)

static uint16_t hud_ball_ink(int id) {
    uint16_t c = cue_render_ball_colour(id);
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    int lum = (r * 4 + g * 5 + b * 2) / 11;          /* 0..63 */
    return lum > 34 ? RGB565C(16, 16, 20) : RGB565C(250, 250, 250);
}

/* A TABLE, SMALL, SEEN FROM ABOVE.
 *
 * The gallery's whole argument is that a position is recognised and not read,
 * so this has to be recognisable at sixty columns: the cloth in the colour the
 * player chose, the six pockets, and the balls where they are. Ball positions
 * are true to the table; ball SIZE is not — at this scale a snooker ball is
 * two-fifths of a layout pixel, so they are drawn about two and a half times
 * over-size. That is what a diagram is for, and an accurate dot you cannot see
 * the colour of tells you nothing about which ball it is.
 *
 * Returns the ball radius it used, so a caller wanting to mark one can match. */
static int hud_mini_table(int x, int y, int w, int h, int kind,
                          const uint8_t *id, const uint8_t *on,
                          const float *bx, const float *bz, int n)
{
    CueTable t;
    cue_table_init(&t, (CueGameKind)kind);
    /* Fit the cloth in the box at the table's own proportions — a snooker
     * position squeezed to a pub table's shape is a different position. */
    float ar = t.half_len / t.half_wid;
    int cw = w - 2, ch = h - 2;
    if ((float)cw > (float)ch * ar) cw = (int)((float)ch * ar);
    else                            ch = (int)((float)cw / ar);
    if (cw < 4) cw = 4;
    if (ch < 3) ch = 3;
    int cx = x + (w - cw) / 2, cy = y + (h - ch) / 2;

    hud_rect(cx - 1, cy - 1, cw + 2, ch + 2, RGB565C(58, 38, 20));   /* the frame */
    hud_rect(cx, cy, cw, ch, S.tab.cloth);

    /* THE POCKETS, ROUND. They were hud_rect, which is a square, and six black
     * squares on a green rectangle is not a billiard table — the shape of a
     * pocket is most of what makes the card read as one at a glance. Drawn in
     * the HUD's real pixels rather than layout space so the circle has enough
     * of them to be a circle. */
    { int pr = (cw > 40) ? 2 : 1;
      const int px[6] = { 0, cw / 2, cw, 0, cw / 2, cw };
      const int py[6] = { 0, 0, 0, ch, ch, ch };
      const uint16_t hole = RGB565C(10, 12, 16);
      for (int p = 0; p < 6; p++) {
          int ox = (cx + px[p]) * HS, oy = (cy + py[p]) * HS, r = pr * HS;
          for (int j = -r; j <= r; j++)
              for (int i = -r; i <= r; i++) {
                  if (i * i + j * j > r * r) continue;
                  int X = ox + i, Y = oy + j;
                  if (X < 0 || X >= CUEVR_HUD_W || Y < 0 || Y >= CUEVR_HUD_H) continue;
                  S.hud[Y * CUEVR_HUD_W + X] = hole;
              }
      } }

    int br = cw / 26; if (br < 1) br = 1;
    for (int i = 0; i < n; i++) {
        if (!on[i]) continue;
        /* Table space is centred on the middle of the cloth; x runs the length. */
        float fx = bx[i] / t.half_len * 0.5f + 0.5f;
        float fz = bz[i] / t.half_wid * 0.5f + 0.5f;
        if (fx < 0.0f) fx = 0.0f; else if (fx > 1.0f) fx = 1.0f;
        if (fz < 0.0f) fz = 0.0f; else if (fz > 1.0f) fz = 1.0f;
        cue_render_ball_icon_hs(cx + (int)(fx * (float)cw),
                                cy + (int)(fz * (float)ch), br, id[i]);
    }
    return br;
}

/* The built-in timed clearance has no saved layout — it builds one when you
 * start it. Its card shows the rack it builds, which is the honest picture. */
static void hud_mini_six(int x, int y, int w, int h, int kind) {
    CueTable t;
    CueBall b[CUE_MAX_BALLS];
    uint8_t id[8], on[8];
    float bx[8], bz[8];
    cue_table_init(&t, (CueGameKind)kind);
    int n = cue_table_rack_six(&t, b);
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        id[i] = (uint8_t)b[i].id; on[i] = (uint8_t)b[i].on;
        bx[i] = b[i].pos.x; bz[i] = b[i].pos.z;
    }
    hud_mini_table(x, y, w, h, kind, id, on, bx, bz, n);
}

static void hud_break_row(int y, const char *who, int brk, const uint8_t *tal,
                          uint16_t TXT, uint16_t DIM, uint16_t HI)
{
    char b[16];
    (void)TXT; (void)HI;
    hud_text(who, 4, y + 2, DIM);
    if (!brk) { hud_text("-", 26, y + 2, DIM); return; }

    /* Smaller, and NO white rims. The rims were meant to lift the snooker
     * black off the dark panel and instead put a thick white ring around every
     * ball on the board — black on dark reads better than that. The count
     * inside drops to the small face to fit the smaller disc. */
    const int rr = 4, step = 11, cy = y + 4;
    int x = 28;
    const int xmax = HW - rr - 2;
    if (S.tab.is_snooker) {
        /* All fifteen reds are one entry: to a break they are one ball, and the
         * count is the whole of what you want to know about them. */
        int reds = 0;
        for (int i = 1; i <= 15; i++) reds += tal[i];
        for (int v = 1; v <= 7 && x <= xmax; v++) {
            int id = (v == 1) ? 1 : CUE_ID_YELLOW + (v - 2);
            int n  = (v == 1) ? reds : tal[id];
            if (!n) continue;
            cue_render_ball_icon_hs(x, cy, rr, id);
            snprintf(b, sizeof b, "%d", n);
            hud_text(b, x - hud_text_w(b) / 2, cy - 2, hud_ball_ink(id));
            x += step;
        }
    } else {
        /* Pool balls carry their own numbers already, and a run pots each of
         * them once — so there is nothing to count, only which ones went. */
        for (int id = 1; id < CUEVR_TALLY_N && x <= xmax; id++) {
            if (!tal[id]) continue;
            cue_render_ball_icon_hs(x, cy, rr, id);
            x += step;
        }
    }
}

/* A toast, painted across the message zone.
 *
 * It goes THERE and not in a corner: that band is where the game already
 * speaks, so a player's eye is trained on it, and giving a record its own
 * quiet corner would repeat the mistake of hiding it. It takes priority over
 * an ordinary message for its four seconds — "FOUL: WRONG BALL" will still be
 * true afterwards, a hundred break happens once. */
static int hud_toast(uint16_t TXT, uint16_t DIM) {
    if (!S.toast_n) return 0;
    int ach = (S.toast[0].kind == TOAST_ACH);
    uint16_t band = ach ? RGB565C(38, 30, 62) : RGB565C(24, 46, 34);
    uint16_t edge = ach ? RGB565C(150, 120, 250) : RGB565C(40, 190, 90);
    /* Fading out at the end, so it leaves rather than blinks off. */
    float k = (CUEVR_TOAST_SECS - S.toast_t) / 0.6f;
    if (k > 1.0f) k = 1.0f;
    if (k < 0.0f) k = 0.0f;
    hud_rect(0, 50, HW, 24, band);
    hud_rect(0, 50, HW, 1, edge);
    hud_rect(0, 73, HW, 1, edge);
    hud_text(ach ? "ACHIEVEMENT" : "NEW RECORD", 4, 52, k > 0.5f ? edge : DIM);
    hud_text_2x(S.toast[0].title, 4, 58, TXT);
    hud_text(S.toast[0].body, 4, 68, DIM);
    /* More waiting behind it. */
    if (S.toast_n > 1) {
        char q[8];
        snprintf(q, sizeof q, "+%d", S.toast_n - 1);
        hud_text_r(q, HW - 4, 52, DIM);
    }
    return 1;
}

static void hud_paint(void) {
    const uint16_t BG   = RGB565C(8, 11, 18);
    const uint16_t BAND = RGB565C(14, 20, 32);
    const uint16_t LINE = RGB565C(52, 96, 160);
    const uint16_t TXT  = RGB565C(232, 238, 246);
    const uint16_t DIM  = RGB565C(118, 138, 164);
    const uint16_t HI   = RGB565C(250, 205, 60);
    const uint16_t LIVE = RGB565C(40, 190, 90);

    /* The scoreboard is the 16:9 one; any screen that is a list overrides this
     * before it draws. */
    hud_height(CUEVR_HUD_BOARD_LH);
    hud_clear(BG);


    /* ---- the menu ---- */
    if (S.state == ST_MENU) {
        /* AS TALL AS IT NEEDS. Every list screen asked for the full 112 rows,
         * so a nine-row menu hung as a half-empty board with the help line
         * marooned at the bottom of it. */
        hud_height(12 + MR_N * 8 + 20);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("CUEVR", 4, 1, HI);
        /* The build, where both players can read it off each other's menu. */
        hud_text_r("v" CUEVR_VERSION, HW - 4, 3, DIM);
        hud_rect(0, 10, HW, 1, LINE);

        char v[40];
        hud_opt(MR_GAME, "GAME", MENU[S.menu_sel].name, S.menu_row == MR_GAME, 1, TXT, DIM, HI);
        hud_opt(MR_OPP, "OPPONENT", OPP_NAME[S.opp], S.menu_row == MR_OPP, 1, TXT, DIM, HI);
        {   char mv[16];
            if (MATCH_LEN[S.match_idx] == 1) snprintf(mv, sizeof mv, "1 FRAME");
            else snprintf(mv, sizeof mv, "BEST OF %d", MATCH_LEN[S.match_idx]);
            hud_opt(MR_FRAMES, "MATCH", mv, S.menu_row == MR_FRAMES, 1, TXT, DIM, HI); }
        snprintf(v, sizeof v, "%s %d", CUE_PERSONAS[S.persona].name,
                 CUE_PERSONAS[S.persona].elo);
        hud_opt(MR_STRENGTH, "STRENGTH", v, S.menu_row == MR_STRENGTH,
                S.opp == OPP_CPU, TXT, DIM, HI);
        /* Who you are about to play, with their face — the portraits have been
         * sitting in cue_faces.h all along. */
        if (S.opp == OPP_CPU) hud_face(HW - 9, 12 + MR_STRENGTH * 8 + 3, 9, S.persona);
        /* The referee. Only he speaks, and only in snooker, so the row says
         * which voice rather than pretending to be a general volume. */
        hud_opt(MR_REFVOICE, "REF VOICE", cuevr_refcall_voice_name(S.ref_voice),
                S.menu_row == MR_REFVOICE, 1, TXT, DIM, HI);
        hud_link(MR_CONTROLS, "CONTROLS", "OPEN", S.menu_row == MR_CONTROLS, DIM, HI, LIVE);
        hud_link(MR_APPEAR, "APPEARANCE", "OPEN", S.menu_row == MR_APPEAR, DIM, HI, LIVE);
        hud_link(MR_STATS,  "RECORDS",    "OPEN", S.menu_row == MR_STATS,  DIM, HI, LIVE);

        {   int y = 12 + MR_START * 8;
            if (S.menu_row == MR_START) hud_rect(1, y - 1, HW - 2, 9, RGB565C(30, 60, 40));
            hud_text_2x(S.opp == OPP_ONLINE ? "FIND A MATCH"
                        : S.opp == OPP_CAREER ? (S.career.active ? "CONTINUE CAREER"
                                                                 : "START A CAREER")
                        : "BREAK OFF",
                        4, y - 1, S.menu_row == MR_START ? LIVE : DIM);
        }
        cue_render_set_preview_hs(-1, -1, 0, 0, 0);   /* the balls preview lives on APPEARANCE now */
        /* The footer said "STICK" long after the menu became a pointer — the
         * same wrong-instruction-in-the-corner the records page had. */
        hud_text("TRIGGER SELECT   < > CHANGE", 4, HH - 6, DIM);
        return;
    }

    /* ---- placing the table ---- */
    if (S.state == ST_SETUP) {
        hud_height(76);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x(S.levelled ? "PLACE TABLE" : "LEVEL THE TABLE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        char b[40];
        int cm = (int)(S.setup.place.height * 100.0f + 0.5f);
        snprintf(b, sizeof b, "%d CM", cm);
        hud_text_xl(b, 4, 16, TXT);
        hud_text(S.levelled ? "MATCH THE HEIGHT TO YOUR REAL TABLE."
                            : "SET THE TABLE TO THE HEIGHT OF YOUR",
                 4, 36, DIM);
        if (!S.levelled) hud_text("REAL TABLE, DESK OR BED.", 4, 42, DIM);
        hud_text("LEFT STICK MOVE    RIGHT STICK TURN", 4, 52, TXT);
        hud_text("RIGHT STICK UP/DOWN HEIGHT    A DONE", 4, 58, HI);
        /* Something you can act on, rather than a note about the pivot. The
         * pivot is a fact about the maths; what a player needs here is how to
         * know when the number is right, and the answer is to put a hand on the
         * real thing and see whether the cloth agrees with it. */
        hud_text("PUT A HAND ON YOUR REAL TABLE AND", 4, HH - 12, DIM);
        hud_text("LINE THE CLOTH UP WITH IT.", 4, HH - 6, DIM);
        return;
    }

    /* ---- the lobby: same options as the Mote lobby ---- */
    if (S.state == ST_LOBBY) {
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("ONLINE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);

        if (S.lb_screen == LB_TRANSPORT) {
            hud_text("HOW DO YOU WANT TO CONNECT?", 4, 14, DIM);
            for (int i = 0; i < TR_N; i++) {
                int y = 24 + i * 11;
                if (i == S.lb_sel) hud_rect(1, y - 1, HW - 2, 11, RGB565C(30, 46, 72));
                hud_text_2x(TR_NAME[i], 8, y, i == S.lb_sel ? HI : DIM);
            }
            hud_text("POINT AND CLICK      B BACK", 4, HH - 6, DIM);
            return;
        }
        if (S.lb_screen == LB_ACTION) {
            static const char *LAN_A[2] = { "HOST", "JOIN" };
            static const char *NET_A[4] = { "QUICK MATCH", "HOST ROOM", "JOIN CODE", "BROWSE ROOMS" };
            int n = (S.lb_tr == TR_LAN) ? 2 : 4;
            hud_text(S.lb_tr == TR_LAN ? "LAN - NO ADDRESS TO TYPE"
                                       : "INTERNET - VIA THE MOTE RELAY", 4, 13, DIM);
            for (int i = 0; i < n; i++) {
                int y = 21 + i * 10;
                if (i == S.lb_sel) hud_rect(1, y - 1, HW - 2, 10, RGB565C(30, 46, 72));
                hud_text_2x(S.lb_tr == TR_LAN ? LAN_A[i] : NET_A[i], 8, y,
                            i == S.lb_sel ? HI : DIM);
            }
            hud_text("POINT AND CLICK      B BACK", 4, HH - 6, DIM);
            return;
        }
        if (S.lb_screen == LB_CODE) {
            hud_text("ENTER THE HOST\'S CODE", 4, 14, DIM);
            for (int i = 0; i < CUEVR_CODE_LEN; i++) {
                int cx = 26 + i * 20;
                char c[2] = { S.lb_code[i], 0 };
                if (i == S.lb_cur) hud_rect(cx - 8, 24, 17, 20, RGB565C(34, 54, 88));
                hud_text_xl(c, cx - hud_text_w_xl(c) / 2, 26,
                            i == S.lb_cur ? TXT : DIM);
            }
            hud_text("STICK LEFT/RIGHT PICK   UP/DOWN CHANGE", 4, 50, DIM);
            hud_text("POINT AND CLICK      B BACK", 4, HH - 6, HI);
            return;
        }
        if (S.lb_screen == LB_BROWSE) {
            if (!cuevr_net_browse_done()) {
                hud_text_2x("LOOKING FOR ROOMS...", 4, 22, TXT);
            } else if (cuevr_net_browse_count() == 0) {
                hud_text_2x("NO ROOMS OPEN", 4, 22, DIM);
                hud_text("HOST ONE, OR TRY QUICK MATCH.", 4, 38, DIM);
            } else {
                int n = cuevr_net_browse_count();
                for (int i = 0; i < n && i < 5; i++) {
                    int y = 13 + i * 9;
                    if (i == S.lb_sel) hud_rect(1, y - 1, HW - 2, 9, RGB565C(30, 46, 72));
                    hud_text_2x(cuevr_net_browse_code(i), 6, y - 1, i == S.lb_sel ? HI : DIM);
                    hud_text(cuevr_net_browse_label(i), 40, y, i == S.lb_sel ? TXT : DIM);
                }
            }
            hud_text("POINT AND CLICK      B BACK", 4, HH - 6, HI);
            return;
        }
        /* LB_WAIT */
        {
            int st = cuevr_net_state();
            hud_text_2x(st == CUEVR_NET_LIVE ? "MATCHED" :
                        st == CUEVR_NET_LOST ? "NO OPPONENT" : "WAITING...",
                        4, 16, st == CUEVR_NET_LIVE ? LIVE : TXT);
            if (cuevr_net_code()[0]) {
                hud_text("YOUR CODE", 4, 32, DIM);
                hud_text_xl(cuevr_net_code(), 46, 30, HI);
            }
            hud_text(cuevr_net_info(), 4, 50, DIM);
            hud_text("B CANCEL", 4, HH - 6, HI);
        }
        return;
    }

    /* ---- practice drills --------------------------------------------------
     *
     * Eight slots. A slot holds a table and, if you want one, something to do
     * on it — so the same screen is where you keep positions to play from and
     * where you keep challenges, because they are the same object with the goal
     * left off. Chevrons on a row change what that drill ASKS; the middle of
     * the row plays it. */
    /* ---- what a challenge ASKS ---------------------------------------------
     *
     * The goal on its own is not enough for a pot: "pot a ball" has to say
     * WHICH, and often which ones — the last three reds is a drill, so is the
     * colours. So the balls are a set you tick, drawn as the balls themselves
     * rather than named, because a row of ball glyphs is read at a glance and a
     * list of words is not. Reds are one entry: a drill that wants a red wants
     * any of them, and fifteen identical buttons would be a worse screen. */
    if (S.state == ST_DRILLSET) {
        int slot = S.edit_slot;
        if (slot < 0 || slot >= CUEVR_DRILL_SLOTS) slot = 0;
        CueVrDrill *d = &S.drills.slot[slot];
        char v[40], nm[24];
        cuevr_drill_name(d, nm, sizeof nm);
        int show_balls = (d->goal == CUEVR_GOAL_POT);
        uint8_t ids[24];
        int nb = show_balls ? cuevr_drill_ball_choices((int)d->kind, ids, 24) : 0;
        int brows = show_balls ? (nb + DSET_BCOLS - 1) / DSET_BCOLS : 0;
        int by = 12 + DSET_N * 8 + 6;
        hud_height(by + (show_balls ? 8 + brows * DSET_BH : 0) + 10);
        hud_rect(0, 0, HW, 10, BAND);
        /* ONE WORD. "THE CHALLENGE" at this size is 100 columns wide and the
         * name was right-aligned into the same band, so the two ran through
         * each other. The name goes at the foot, where there was already room
         * doing nothing. */
        hud_text_2x("CHALLENGE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        hud_opt(DSET_GOAL, "GOAL", cuevr_goal_name(d->goal),
                S.dset_row == DSET_GOAL, 1, TXT, DIM, HI);
        if (d->goal == CUEVR_GOAL_SCORE) snprintf(v, sizeof v, "%d POINTS", (int)d->target);
        else                             snprintf(v, sizeof v, "-");
        hud_opt(DSET_TARGET, "TARGET", v, S.dset_row == DSET_TARGET,
                d->goal == CUEVR_GOAL_SCORE, TXT, DIM, HI);
        /* AGAINST THE CLOCK is the challenge's own business, and only a
         * challenge with a goal can be raced — there is nothing to be quick
         * about in a position you are just playing on from. */
        hud_opt(DSET_TIMED, "AGAINST THE CLOCK", d->timed ? "YES" : "NO",
                S.dset_row == DSET_TIMED, d->goal != CUEVR_GOAL_SETUP, TXT, DIM, HI);
        hud_link(DSET_PLAY, "PLAY IT", "GO", S.dset_row == DSET_PLAY, DIM, HI, LIVE);
        hud_link(DSET_EDIT, "MOVE THE BALLS", "EDIT", S.dset_row == DSET_EDIT, DIM, HI, LIVE);
        hud_link(DSET_DEL,  "DELETE", "REMOVE", S.dset_row == DSET_DEL, DIM, HI, LIVE);
        hud_link(DSET_BACK, "BACK", "DONE", S.dset_row == DSET_BACK, DIM, HI, LIVE);
        if (show_balls) {
            hud_rect(0, by - 2, HW, 10 + brows * DSET_BH, HUD_BALLBG);
            hud_rect(0, by - 3, HW, 1, LINE);
            hud_text("WHICH BALLS", 4, by - 1, DIM);
            for (int i = 0; i < nb; i++) {
                int cx = DSET_BX + (i % DSET_BCOLS) * DSET_BW;
                int cy = by + 8 + (i / DSET_BCOLS) * DSET_BH;
                int on = (d->need >> ids[i]) & 1u;
                if (S.dset_ball == i) hud_rect(cx - 6, cy - 6, 12, 12, HI);
                if (on) hud_rect(cx - 5, cy - 5, 10, 10, LIVE);
                cue_render_ball_icon_hs(cx, cy, 4, ids[i]);
            }
        }
        hud_text(nm, 4, HH - 6, DIM);
        return;
    }

    if (S.state == ST_LAYOUT) {
        char b3[40];
        int on = 0, off = 0;
        for (int i = 1; i < S.nballs; i++) { if (S.balls[i].on) on++; else off++; }
        int rows = S.lay_menu ? LAY_N : 0;
        hud_height(12 + rows * 8 + 30);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x(S.edit_new ? "SET THE BALLS OUT" : "MOVE THE BALLS", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);

        if (S.lay_menu) {
            hud_link(LAY_DONE, S.edit_new ? "SAVE THIS POSITION" : "SAVE THE CHANGES",
                     "DONE", S.lay_row == LAY_DONE, DIM, HI, LIVE);
            hud_link(LAY_TAKEOFF, "TAKE A BALL OFF",
                     S.edit_ball > 0 ? "THIS ONE" : "REACH FIRST",
                     S.lay_row == LAY_TAKEOFF, DIM, HI, LIVE);
            snprintf(b3, sizeof b3, off ? "%d OFF" : "NONE OFF", off);
            hud_link(LAY_BACKON, "PUT ONE BACK", b3, S.lay_row == LAY_BACKON, DIM, HI, LIVE);
            hud_link(LAY_RACK,  "START FROM A FULL RACK", "RACK",
                     S.lay_row == LAY_RACK, DIM, HI, LIVE);
            hud_link(LAY_CLEAR, "TAKE THEM ALL OFF", "CLEAR",
                     S.lay_row == LAY_CLEAR, DIM, HI, LIVE);
            hud_link(LAY_CANCEL, "THROW THE CHANGES AWAY", "CANCEL",
                     S.lay_row == LAY_CANCEL, DIM, HI, LIVE);
        }

        /* WHAT IS IN YOUR HAND, because the gesture gives no other sign of it —
         * the ball follows the controller whether you meant to pick it up or
         * not, and a ball you did not know you were holding is how a position
         * gets wrecked one grab at a time. */
        {   int y = 12 + rows * 8 + 4;
            hud_rect(0, y - 2, HW, 28, HUD_BALLBG);
            hud_rect(0, y - 3, HW, 1, LINE);
            if (S.edit_ball >= 0) {
                cue_render_ball_icon_hs(11, y + 8, 6, S.balls[S.edit_ball].id);
                hud_text(S.edit_ball == 0 ? "THE CUE BALL, IN YOUR HAND"
                                          : "IN YOUR HAND", 22, y + 2, HI);
                hud_text("LET GO OF THE TRIGGER TO PUT IT DOWN", 22, y + 10, DIM);
            } else {
                snprintf(b3, sizeof b3, "%d BALLS ON THE TABLE", on);
                hud_text(b3, 4, y + 2, TXT);
                hud_text("HOLD THE TRIGGER NEAR ONE TO PICK IT UP", 4, y + 10, DIM);
            }
            hud_text(S.lay_menu ? "MENU  -  BACK TO THE BALLS"
                                : "MENU  -  SAVE, REMOVE, RACK, CLEAR",
                     4, y + 19, S.lay_menu ? DIM : HI);
        }
        return;
    }

    if (S.state == ST_DRILLS) {
        char v[40], nm[24];
        int n = drill_rows();
        int nrow = (n + DRILL_COLS - 1) / DRILL_COLS;
        int vrow = nrow < DRILL_VROWS ? nrow : DRILL_VROWS;
        hud_height(DRILL_Y0 + vrow * DRILL_CH + 10);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("CHALLENGES", 4, 1, HI);
        if (nrow > DRILL_VROWS) {
            snprintf(v, sizeof v, "%d/%d", S.drill_row + 1, n);
            hud_text_r(v, HW - 4, 3, DIM);
        }
        hud_rect(0, 10, HW, 1, LINE);

        for (int k = 0; k < DRILL_VIS; k++) {
            int i = S.drill_scroll * DRILL_COLS + k;
            if (i >= n) break;
            int cx = DRILL_X0 + (k % DRILL_COLS) * DRILL_CW;
            int cy = DRILL_Y0 + (k / DRILL_COLS) * DRILL_CH;
            int sel = (S.drill_row == i);
            int cw = DRILL_CW - 4, ch = DRILL_CH - 3;

            /* The card. Selected gets a lit ground and a rule under it rather
             * than an outline, so the picture keeps its own edge. */
            hud_rect(cx - 1, cy - 1, cw + 2, ch + 2,
                     sel ? RGB565C(30, 46, 72) : RGB565C(14, 19, 30));

            const char *sub;
            char sb[40];
            if (i == DRILL_ROW_TIMED) {
                hud_mini_six(cx, cy, cw, ch - 15, (int)S.tab.kind);
                snprintf(nm, sizeof nm, "TIMED SIX");
                int b = S.mini_best[(int)S.tab.kind];
                if (b) { snprintf(sb, sizeof sb, "BEST %d.%02d", b / 100, b % 100); sub = sb; }
                else     sub = "AGAINST THE CLOCK";
            } else if (i == n - 1) {
                /* THE ONE CARD THAT IS NOT A POSITION. An empty table with a
                 * plus on it: it is the same shape as the rest so it sits in
                 * the grid, and plainly not one of them. */
                int mx = cx + cw / 2, my = cy + (ch - 15) / 2;
                hud_rect(cx, cy, cw, ch - 15, RGB565C(20, 28, 44));
                hud_rect(mx - 7, my - 1, 15, 3, HI);
                hud_rect(mx - 1, my - 7, 3, 15, HI);
                snprintf(nm, sizeof nm, "MAKE ONE");
                sub = "SET THE BALLS OUT";
            } else {
                const CueVrDrill *d = &S.drills.slot[drill_slot_at(i)];
                hud_mini_table(cx, cy, cw, ch - 15, (int)d->kind,
                               d->id, d->on, d->x, d->z, d->n);
                cuevr_drill_name(d, nm, sizeof nm);
                /* A SCORE challenge's whole character is the NUMBER, and the
                 * goal's name does not carry it: two cards both reading
                 * "SCORE" are two cards saying nothing. */
                char gl[24];
                if (d->goal == CUEVR_GOAL_SCORE)
                    snprintf(gl, sizeof gl, "SCORE %d", (int)d->target);
                else snprintf(gl, sizeof gl, "%s", cuevr_goal_name(d->goal));

                if (d->timed && d->best)
                    snprintf(sb, sizeof sb, "BEST %d.%02d", d->best / 100, d->best % 100);
                else if (d->goal == CUEVR_GOAL_SETUP)
                    snprintf(sb, sizeof sb, "%s", "PLAY ON FROM HERE");
                else if (d->wins)
                    snprintf(sb, sizeof sb, "%s  %d/%d", gl, d->wins, d->tries);
                else
                    snprintf(sb, sizeof sb, "%s", gl);
                sub = sb;
            }
            hud_text(nm, cx + 1, cy + ch - 14, sel ? HI : TXT);
            hud_text(sub, cx + 1, cy + ch - 7, DIM);
        }
        hud_text("POINT AND CLICK TO OPEN      B BACK", 4, HH - 6, DIM);
        return;
    }

    /* ---- the cloth, as a card of swatches ---------------------------------
     *
     * Twenty-three cloths cannot be chosen by stepping a value one at a time:
     * you would have to remember what OLIVE looked like six presses ago. This
     * is the manufacturer's card — the colour itself, at a size you can judge,
     * in the order it is printed — so picking a cloth is looking at cloth
     * rather than reading a list of words.
     *
     * The layout lives in one place because the drawing and the pointer have
     * to agree about where a swatch is; they were two sets of numbers on the
     * decision screen and they drifted. */
    if (S.state == ST_CLOTH) {
        hud_height(CLOTH_BACK_Y + 12);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("CLOTH", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        for (int i = 0; i < CUE_NCLOTH; i++) {
            int cx = CLOTH_X0 + (i % CLOTH_COLS) * CLOTH_CW;
            int cy = CLOTH_Y0 + (i / CLOTH_COLS) * CLOTH_CH;
            int sel = (i == S.cloth_idx), hov = (i == S.cloth_hov);
            /* The swatch, and a surround that says which one is yours and
             * which one you are over — two different things, because the one
             * under the pointer is not chosen until it is pressed. */
            if (sel)      hud_rect(cx - 2, cy - 2, CLOTH_CW - 2, CLOTH_CH - 2, HI);
            else if (hov) hud_rect(cx - 2, cy - 2, CLOTH_CW - 2, CLOTH_CH - 2, LINE);
            hud_rect(cx - 1, cy - 1, CLOTH_CW - 4, CLOTH_CH - 4, RGB565C(10,10,12));
            hud_rect(cx, cy, CLOTH_CW - 6, CLOTH_CH - 6, cloth_colour(i));
        }
        /* The name of whichever one is under the pointer, or of yours when it
         * is nowhere: a grid of colours with no names is a grid you cannot ask
         * anybody else for. */
        {   int nameof = (S.cloth_hov >= 0) ? S.cloth_hov : S.cloth_idx;
            hud_rect(0, CLOTH_NAME_Y - 3, HW, 1, LINE);
            /* The hint sits BESIDE the name, not under it: hud_text_2x is
             * eleven rows tall and a small line nine below it lands inside the
             * capitals. */
            hud_text_2x(k_cloth_name[nameof], 4, CLOTH_NAME_Y, HI);
            hud_text_r(nameof == S.cloth_idx ? "ON THE TABLE" : "TRIGGER TO FIT",
                       HW - 4, CLOTH_NAME_Y + 3, DIM);
        }
        {   int on = (S.cloth_hov == CLOTH_BACK);
            if (on) hud_rect(1, CLOTH_BACK_Y - 1, HW - 2, 12, RGB565C(30, 60, 40));
            hud_text_2x("BACK", 6, CLOTH_BACK_Y, on ? HI : DIM);
            hud_text_r("DONE", HW - 4, CLOTH_BACK_Y + 3, on ? LIVE : DIM);
        }
        return;
    }

    /* ---- appearance ---- *
     * The same six controls whether you opened them from the main menu or from
     * the middle of a frame, so there is one screen and one piece of input
     * handling rather than two that drift. */
    if (S.state == ST_APPEAR) {
        char v[48];
        hud_height(12 + AR_N * 8 + 14);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("APPEARANCE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        hud_link(AR_CLOTH, "CLOTH", k_cloth_name[S.cloth_idx],
                 S.menu_row == AR_CLOTH, DIM, HI, LIVE);
        hud_opt(AR_FRAME, "FRAME", k_frame_name[S.frame_idx], S.menu_row == AR_FRAME, 1, TXT, DIM, HI);
        if (S.body_idx < 0)
            snprintf(v, sizeof v, "AUTO (%s)",
                     cuevr_render_body_name(cuevr_frame_default(&S.tab)));
        else
            snprintf(v, sizeof v, "%s", cuevr_render_body_name(S.body_idx));
        hud_opt(AR_BODY, "TABLE", v, S.menu_row == AR_BODY, 1, TXT, DIM, HI);
        hud_opt(AR_LIGHT, "LIGHTING", cuevr_render_light_name(S.light_idx),
                S.menu_row == AR_LIGHT, 1, TXT, DIM, HI);
        hud_opt(AR_BALLS, "BALLS", k_ballset_name[S.ballset], S.menu_row == AR_BALLS, 1, TXT, DIM, HI);
        hud_opt(AR_SPOTS, "CUE BALL SPOTS", S.cue_spots ? "ON" : "OFF",
                S.menu_row == AR_SPOTS, 1, TXT, DIM, HI);
        hud_opt(AR_CUE, "CUE", cuevr_render_cue_name(S.cue_idx), S.menu_row == AR_CUE, 1, TXT, DIM, HI);
        hud_opt(AR_SURROUND, "SURROUNDINGS", SURROUND_NAME[S.surround],
                S.menu_row == AR_SURROUND, 1, TXT, DIM, HI);
#if CUEVR_TUNE_POCKETS
        hud_link(AR_POCKETS, "POCKET SHAPE", "TUNE",
                 S.menu_row == AR_POCKETS, DIM, HI, LIVE);
#endif
        /* An action, so it wears the action colour rather than the value one —
         * the same distinction the main menu now draws. */
        hud_link(AR_BACK, "BACK", "DONE", S.menu_row == AR_BACK, DIM, HI, LIVE);
        hud_rect(HW - 18, 12 + AR_BALLS * 8 - 1, 18, 8, HUD_BALLBG);
        cue_render_set_preview_hs(HW - 9, 12 + AR_BALLS * 8 + 3, 2,
                                  S.ballset, S.tab.kind >= CUE_GAME_FIRST_SNK);
        hud_text("< > CHANGE   A BACK", 4, HH - 6, DIM);
        return;
    }

    /* ---- controls ---- */
    if (S.state == ST_CONTROLS) {
        hud_height(12 + CR_N * 8 + 20);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("CONTROLS", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        hud_opt(CR_HAND, "CUE HAND", S.lefty ? "LEFT" : "RIGHT",
                S.menu_row == CR_HAND, 1, TXT, DIM, HI);
        hud_opt(CR_STICKS, "STICKS",
                S.stick_swap ? "LEFT TURNS  RIGHT MOVES"
                             : "LEFT MOVES  RIGHT TURNS",
                S.menu_row == CR_STICKS, 1, TXT, DIM, HI);
        hud_opt(CR_INVSLIDE, "INVERT SLIDE", S.inv_slide ? "ON" : "OFF",
                S.menu_row == CR_INVSLIDE, 1, TXT, DIM, HI);
        hud_opt(CR_INVTURN, "INVERT TURN", S.inv_turn ? "ON" : "OFF",
                S.menu_row == CR_INVTURN, 1, TXT, DIM, HI);
        /* The way out when the cue has ended up somewhere impossible. It used
         * to live on the alignment screen, which has gone; the escape hatch
         * should not go with it. */
        hud_link(CR_RESET, "RESET CUE", "RESET", S.menu_row == CR_RESET, DIM, HI, LIVE);
        hud_link(CR_BACK, "BACK", "DONE", S.menu_row == CR_BACK, DIM, HI, LIVE);
        hud_text("A AND B STAY ON THE RIGHT CONTROLLER", 4, HH - 12, DIM);
        hud_text("TRIGGER SELECT", 4, HH - 6, DIM);
        return;
    }

    /* ---- records ---- */
    if (S.state == ST_STATS) {
        char v[48];
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("RECORDS", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        /* A ROW YOU CAN SEE AND CLICK, not a word in the corner. The footer
         * said left/right while the only thing that worked was clicking that
         * word — two different instructions, neither of them signposted. Both
         * of these are rows now, and both are hit-tested where they are drawn. */
        {
            int on = (S.menu_row == 0);
            if (on) hud_rect(1, 12, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text("SHOWING", 4, 14, on ? HI : DIM);
            hud_text_r(S.stat_page ? "ONLINE" : "VS CPU", HW - 6, 14, LIVE);
        }

        int md = S.stat_page ? 1 : 0;
        /* MORE THAN FITS. The page grew a breaks section and there is no taller
         * panel to put it on, so it scrolls: everything is drawn at an offset
         * and clipped to the window between the SHOWING row and the footer,
         * with one clickable row at the bottom that pages up and down. */
        const int Y0 = 24, Y1 = HH - 22;
        int y = Y0 - S.stat_scroll;
        #define ROW(lbl, val, lit) do {                                        \
                if (y >= Y0 && y <= Y1) {                                      \
                    hud_text((lbl), 8, y, TXT);                                \
                    hud_text_r((val), HW - 6, y, (lit) ? HI : DIM);            \
                }                                                              \
                y += 7;                                                        \
            } while (0)
        #define HEAD(lbl) do {                                                 \
                if (y >= Y0 && y <= Y1) hud_text((lbl), 4, y, LIVE);           \
                y += 7;                                                        \
            } while (0)

        HEAD("HIGHEST BREAK");
        for (int a = 0; a < CUEVR_STAT_SNK; a++) {
            snprintf(v, sizeof v, "%d", S.stats.snk_best[a][md]);
            ROW(cuevr_stat_snk_name(a), v, S.stats.snk_best[a][md]);
        }
        y += 3;
        HEAD("BREAKS MADE");
        {
            static const char *TN[CUEVR_BRK_TIERS] = { "20+", "30+", "50+", "100+" };
            for (int a = 0; a < CUEVR_BRK_TIERS; a++) {
                snprintf(v, sizeof v, "%d", S.stats.brk_tier[a][md]);
                ROW(TN[a], v, S.stats.brk_tier[a][md]);
            }
        }
        y += 3;
        HEAD("CLEARANCES");
        for (int a = 0; a < CUEVR_STAT_POOL; a++) {
            snprintf(v, sizeof v, "%d", S.stats.pool_clear[a][md]);
            ROW(cuevr_stat_pool_name(a), v, S.stats.pool_clear[a][md]);
        }
        y += 3;
        HEAD("SIX BALL CLEARANCE");
        for (int a = 0; a < CUE_GAME_COUNT; a++) {
            int best = S.mini_best[a];
            if (!best) continue;
            snprintf(v, sizeof v, "%d.%02d", best / 100, best % 100);
            ROW(cuevr_stat_table_name(a), v, 1);
        }
        y += 3;
        snprintf(v, sizeof v, "%d of %d", S.stats.frames_won[md], S.stats.frames_played[md]);
        ROW("FRAMES WON", v, 1);
        #undef ROW
        #undef HEAD

        /* How far the list actually runs, so the scroll cannot go past its end
         * and the arrow only appears when there is something below. */
        S.stat_len = (y + S.stat_scroll) - Y0;
        int page = Y1 - Y0;
        int maxs = S.stat_len - page;
        if (maxs < 0) maxs = 0;
        if (S.stat_scroll > maxs) S.stat_scroll = maxs;

        if (maxs > 0) {
            int on = (S.menu_row == 2);
            if (on) hud_rect(1, HH - 20, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text(S.stat_scroll < maxs ? "MORE" : "BACK TO THE TOP",
                     4, HH - 18, on ? HI : DIM);
            hud_text_r(S.stat_scroll < maxs ? "DOWN" : "UP", HW - 6, HH - 18, LIVE);
        }

        {
            int on = (S.menu_row == 1);
            if (on) hud_rect(1, HH - 11, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text("BACK", 4, HH - 9, on ? HI : DIM);
            hud_text_r("DONE", HW - 6, HH - 9, on ? LIVE : DIM);
        }
        return;
    }

    /* ---- paused ---- */
    if (S.state == ST_PAUSE) {
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("PAUSED", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        {
            PsRow row[16];
            int n = pause_rows(row, 16);
            if (S.pause_sel >= n) S.pause_sel = 0;
            for (int i = 0; i < n; i++) {
                int y = 12 + i * 9, on = (i == S.pause_sel);
                if (on) hud_rect(1, y - 1, HW - 2, 9, RGB565C(30, 46, 72));
                char lb[40];
                const char *txt = row[i].label;
                if (row[i].id == PS_NOMINATE) {
                    snprintf(lb, sizeof lb, "NOMINATE %s",
                             (S.rules.nominated >= 2 && S.rules.nominated <= 7)
                             ? COLOUR_NAME[S.rules.nominated] : "-");
                    txt = lb;
                } else if (row[i].id == PS_FREEBALL) {
                    snprintf(lb, sizeof lb, "FREE BALL %s",
                             S.rules.free_ball_id
                             ? cue_ball_short_name(S.rules.free_ball_id) : "-");
                    txt = lb;
                } else if (row[i].id == PS_RESPOT) {
                    snprintf(lb, sizeof lb, "AUTO RESPOT %s",
                             S.prac_respot ? "ON" : "OFF");
                    txt = lb;
                }
                hud_text_2x(txt, 6, y - 1, on ? HI : DIM);
            }
        }
        /* The help line only when there is room for it. A menu that runs its
         * last rows through the footer is worse than one with no footer, and
         * which rows are offered depends on the mode and the moment. */
        {   PsRow row[16]; int n = pause_rows(row, 16);
            if (12 + n * 9 <= HH - 8)
                hud_text("TRIGGER SELECT   MENU RESUME", 4, HH - 6, DIM); }
        return;
    }

    /* ---- the frame is over ----------------------------------------------- *
     * The scoreboard said "YOU WIN" over the top of itself and that was the
     * whole of it. Who won is the one thing you already know — you were there.
     * What you cannot see from the table is how it went: whether the eighty you
     * lost by was one visit or forty of them, whether you were potting and
     * fouling or neither.
     *
     * A frame gets the frame's figures. The last frame of a match gets the
     * match's, because by then the individual rack matters less than the hour
     * of play it finished. */
    if (S.state == ST_OVER) {
        /* A ONE-FRAME GAME IS A MATCH TOO. It ended on the frame screen and
         * never showed this one at all, which meant the commonest way to play
         * — one rack, see how it went — was the one that got the lesser
         * summary. `multi` is what separates the wording that only makes sense
         * across several frames (the tally, "of the match") from the totals,
         * which are the same numbers either way. */
        int match = S.rules.match_over;
        int multi = (S.rules.best_of > 1);
        int done  = (S.rules.best_of == 1 || S.rules.match_over);
        const CueVrPlayStat *sp = match ? S.mstat : S.fstat;
        int me = (S.opp == OPP_ONLINE) ? S.net_me : 0;
        int two = (S.opp != OPP_PRACTICE);
        char nm[24], b[48];
        /* Two number columns, right-aligned on their own edges so a 3 and a 147
         * line up. Only one of them when there is nobody in the other chair. The
         * labels need 40 and the figures never pass four characters, so the
         * columns are as wide as they can be without crowding either. */
        const int c0 = two ? 86 : HW - 6, c1 = HW - 6;
        const char *them = hud_fit_name(nm, sizeof nm,
                                        (S.opp == OPP_ONLINE) ? "OPPONENT"
                                        : CUE_PERSONAS[S.persona].name,
                                        c1 - c0 - 4);

        /* ONE headline, not two. "FRAME OVER" on the band with "FRAME WON"
         * underneath it said the same thing twice and cost ten rows the balls
         * needed — and a result belongs at the top of a results screen. */
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 11, BAND);
        {
            const char *v;
            int won = (S.rules.winner == me) || S.opp == OPP_PRACTICE;
            if (S.opp == OPP_PRACTICE)     v = "TABLE CLEARED";
            else if (S.rules.winner == me) v = match ? "MATCH WON" : "FRAME WON";
            else                           v = match ? "MATCH LOST" : "FRAME LOST";
            hud_text_2x(v, 4, 1, won ? LIVE : RGB565C(230, 80, 60));
            if (two && multi) {
                snprintf(b, sizeof b, "%d - %d", S.rules.frames[me],
                         S.rules.frames[1 - me]);
                hud_text_2x(b, HW - 6 - hud_text_w_2x(b), 1, TXT);
            } else if (S.rules.conceded) {
                hud_text_r("CONCEDED", HW - 6, 4, DIM);
            }
        }
        hud_rect(0, 11, HW, 1, LINE);

        /* Column heads. */
        hud_text_r("YOU", c0, 15, HI);
        if (two) hud_text_r(them, c1, 15, TXT);
        hud_rect(0, 22, HW, 1, LINE);

        /* Five figures, in the order you would ask for them: what you made, what
         * your best visit was worth, how often the ball went in, what it cost
         * you, and how long you took over it.
         *
         * The visit figure has ONE name, and which name depends on the game:
         * snooker counts a visit in points and calls it a break, pool counts it
         * in balls and calls it a run. */
        {
            const char *LBL[5];
            LBL[0] = "POTTED";
            LBL[1] = S.tab.is_snooker ? "BEST BREAK" : "LONGEST RUN";
            LBL[2] = "POT %"; LBL[3] = "FOULS"; LBL[4] = "AVG SHOT";
            for (int i = 0; i < 5; i++) {
                int y = 25 + i * 8;
                hud_text(LBL[i], 4, y, DIM);
                for (int col = 0; col < (two ? 2 : 1); col++) {
                    const CueVrPlayStat *st = &sp[col == 0 ? me : 1 - me];
                    switch (i) {
                    case 0: snprintf(b, sizeof b, "%d", st->potted); break;
                    case 1: snprintf(b, sizeof b, "%d", st->best_break); break;
                    /* Pots per stroke. Rounded, not truncated: 19 pots from 20
                     * shots is 95%, and a floor would call it 94. */
                    case 2: if (st->shots)
                                snprintf(b, sizeof b, "%d%%",
                                         (st->pot_shots * 200 + st->shots) / (st->shots * 2));
                            else snprintf(b, sizeof b, "-");
                            break;
                    case 3: snprintf(b, sizeof b, "%d", st->fouls); break;
                    case 4: if (st->shots)
                                snprintf(b, sizeof b, "%.1fs",
                                         (double)(st->time / (float)st->shots));
                            else snprintf(b, sizeof b, "-");
                            break;
                    default: b[0] = 0;
                    }
                    hud_text_r(b, col == 0 ? c0 : c1, y, col == 0 ? HI : TXT);
                }
            }
        }

        /* And that best visit, drawn out ball by ball — on a slightly bluer
         * ground than the panel's near-black, so the dark balls read against
         * it. The snooker black on the bare background was a number floating
         * in nothing. */
        hud_rect(0, 66, HW, HH - 66 - 11, HUD_BALLBG);
        hud_rect(0, 65, HW, 1, LINE);
        {
            const char *t2 = S.tab.is_snooker ? "BEST BREAK" : "LONGEST RUN";
            if (match) { snprintf(b, sizeof b, "%s OF THE MATCH", t2);
                         hud_text(b, 4, 67, LIVE); }
            else       hud_text(t2, 4, 67, LIVE);
        }
        hud_break_row(77, "YOU", sp[me].best_break, sp[me].best_tally, TXT, DIM, HI);
        if (two) {
            /* Its own, shorter fit: the break rows give a name 24 columns
             * before the balls start, where the stats columns gave it 32 —
             * "OPPONENT" cleared one and ran under the other. */
            char n2[24];
            const char *t2 = hud_fit_name(n2, sizeof n2,
                                          (S.opp == OPP_ONLINE) ? "THEM"
                                          : CUE_PERSONAS[S.persona].name, 24);
            hud_break_row(89, t2, sp[1 - me].best_break, sp[1 - me].best_tally,
                          TXT, DIM, HI);
        }

        {
            int hov = (S.ptr_ok && S.ptr_y >= (float)(HH - 9));
            if (hov) hud_rect(1, HH - 9, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text(done ? "A    BACK TO THE MENU" : "A    NEXT FRAME",
                     4, HH - 7, hov ? HI : TXT);
        }
        return;
    }

    /* ---- a foul decision ---------------------------------------------------
     * Its own screen, BEFORE the scoreboard paints. It used to live after it,
     * drawing a band and rows over a board that was already there — two screens
     * through each other, unreadable exactly when the game was asking you a
     * question. Early-return screens do not share a canvas. */
    if (S.state == ST_DECIDE) {
        DecOpt o[6];
        int n = decision_options(o, 6);
        /* Online it is one player's call, and the other one's screen must say so
         * rather than offering a menu that does nothing when pressed. */
        int theirs = 0;
        if (S.opp == OPP_ONLINE) {
            int decider = S.rules.pushout_offer ? S.rules.turn
                                                : 1 - S.rules.dec_offender;
            theirs = (decider != S.net_me);
        }
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x(theirs ? (S.rules.pushout_offer ? "THEIR PUSH OUT" : "YOUR FOUL - THEIR CALL")
                           : (S.rules.pushout_offer ? "PUSH OUT?" : "THEIR FOUL - YOUR CALL"),
                    4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        if (S.rules.dec_can_restore) hud_text("A MISS WAS CALLED", 4, 13, LIVE);
        for (int i = 0; i < n; i++) {
            int y = 22 + i * 14, sel = (!theirs && S.dec_sel == i);
            if (sel) hud_rect(1, y - 1, HW - 2, 13, RGB565C(28, 58, 40));
            hud_text_2x(o[i].label, 6, y - 1, sel ? HI : DIM);
            hud_text(o[i].note, 8, y + 8, DIM);
        }
        hud_text(theirs ? "WAITING FOR THEM TO CHOOSE" : "TRIGGER SELECT", 4, HH - 6, DIM);
        return;
    }

#if CUEVR_TUNE_POCKETS
    /* ---- the pocket cut, with a ball on the table ------------------------- */
    if (S.state == ST_POCKETS) {
        char v[40];
        hud_height(12 + 5 * 9 + 30);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("POCKET SHAPE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        static const char *L[4] = { "CORNER SIZE", "CORNER SET BACK",
                                    "MIDDLE SIZE", "MIDDLE SET BACK" };
        int val[4] = { S.cut_cr, S.cut_cs, S.cut_mr, S.cut_ms };
        for (int i = 0; i < 4; i++) {
            if (i & 1) snprintf(v, sizeof v, "%d MM", val[i]);
            else       snprintf(v, sizeof v, "%d%%", val[i]);
            hud_opt(i, L[i], v, S.menu_row == i, 1, TXT, DIM, HI);
        }
        hud_link(4, "BACK", "DONE", S.menu_row == 4, DIM, HI, LIVE);
        hud_text("ROLL A BALL IN AND WATCH WHERE IT DROPS", 4, HH - 18, DIM);
        hud_text("< > CHANGE   THE TABLE REDRAWS AS YOU GO", 4, HH - 12, DIM);
        hud_text("A OR B LEAVES   SAVES TO CUEVR_POCKETS.TXT", 4, HH - 6, LIVE);
        return;
    }

#endif

    /* ---- career: choosing the season's tables ----------------------------- */
    if (S.state == ST_CARSETUP) {
        hud_height(12 + (CUE_GAME_COUNT + 2) * 8 + 20);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("NEW CAREER", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        int n = 0;
        for (int i = 0; i < CUE_GAME_COUNT; i++) {
            hud_opt(i, cuevr_stat_table_name(i), S.car_pick[i] ? "IN" : "-",
                    S.car_row == i, 1, TXT, DIM, HI);
            if (S.car_pick[i]) n++;
        }
        {
            char v[32];
            snprintf(v, sizeof v, n == 1 ? "%d LEAGUE" : "%d LEAGUES", n);
            hud_link(CUE_GAME_COUNT, "START CAREER", n ? v : "PICK ONE",
                     S.car_row == CUE_GAME_COUNT, DIM, HI, n ? LIVE : DIM);
        }
        hud_link(CUE_GAME_COUNT + 1, "BACK", "CANCEL",
                 S.car_row == CUE_GAME_COUNT + 1, DIM, HI, LIVE);
        hud_text("ONE LEAGUE PER TABLE, EVERY SEASON", 4, HH - 12, DIM);
        hud_text("TRIGGER SELECT", 4, HH - 6, DIM);
        return;
    }

    /* ---- career: the hub -------------------------------------------------- */
    if (S.state == ST_CAREER) {
        char v[48];
        const CueVrCareer *C = &S.career;
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 11, BAND);
        snprintf(v, sizeof v, "SEASON %d", C->season);
        hud_text_2x(v, 4, 1, HI);
        snprintf(v, sizeof v, "%d", C->elo);
        hud_text_2x(v, HW - 6 - hud_text_w_2x(v), 1, TXT);
        hud_rect(0, 11, HW, 1, LINE);

        snprintf(v, sizeof v, "WON %d   LOST %d", C->wins, C->losses);
        hud_text(v, 4, 14, DIM);
        if (C->best_break > 0) {
            snprintf(v, sizeof v, "BEST BREAK %d", C->best_break);
            hud_text_r(v, HW - 6, 14, DIM);
        }
        hud_rect(0, 21, HW, 1, RGB565C(26, 40, 62));

        /* The next fixture, which is the whole point of the screen. */
        const CueVrCarFixture *fx = NULL;
        int l = cuevr_career_next(C, &fx);
        if (l >= 0 && fx) {
            const CueVrCarLeague *L = &C->league[l];
            hud_text("NEXT MATCH", 4, 24, LIVE);
            hud_text_2x(cuevr_stat_table_name(L->kind), 4, 31, TXT);
            int opp = cuevr_career_opponent(fx);
            snprintf(v, sizeof v, "%s  %d", CUE_PERSONAS[opp].name, C->ai_elo[opp]);
            hud_text_2x(v, 4, 42, HI);
            snprintf(v, sizeof v, "%s DIVISION   BEST OF %d",
                     L->division ? "PRO" : "AMATEUR", cuevr_career_bestof(C, l));
            hud_text(v, 4, 54, DIM);
            hud_face(HW - 12, 40, 18, opp);
        } else {
            hud_text_2x("SEASON COMPLETE", 4, 34, LIVE);
        }
        hud_rect(0, 61, HW, 1, RGB565C(26, 40, 62));

        {
            static const char *ROWS[4] = { "PLAY THE MATCH", "LEAGUE TABLES",
                                           "ACHIEVEMENTS", "BACK TO THE MENU" };
            for (int i = 0; i < 4; i++) {
                int y = 65 + i * 10, on = (S.car_row == i);
                if (on) hud_rect(1, y - 1, HW - 2, 10, RGB565C(28, 58, 40));
                int live = (i != 0) || (l >= 0);
                hud_text_2x(ROWS[i], 6, y, on ? HI : (live ? DIM : RGB565C(70,78,92)));
            }
        }
        hud_text("TRIGGER SELECT", 4, HH - 6, DIM);
        return;
    }

    /* ---- career: a league table ------------------------------------------ */
    if (S.state == ST_CARTABLE) {
        char v[48];
        CueVrCareer *C = &S.career;
        int l = (S.car_view >= 0 && S.car_view < C->nleague) ? S.car_view : 0;
        const CueVrCarLeague *L = &C->league[l];
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("LEAGUE", 4, 1, HI);
        hud_text_r(L->division ? "PRO" : "AMATEUR", HW - 6, 3, LIVE);
        hud_rect(0, 10, HW, 1, LINE);

        {   /* the league picker, when there is more than one */
            int on = (S.car_row == 0);
            if (on) hud_rect(1, 12, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text("TABLE", 4, 14, on ? HI : DIM);
            hud_text_r(cuevr_stat_table_name(L->kind), HW - 6, 14,
                       C->nleague > 1 ? LIVE : DIM);
        }

        hud_text("P", 74, 24, DIM);
        hud_text("W", 88, 24, DIM);
        hud_text("F", 102, 24, DIM);
        hud_text("PTS", HW - 6 - hud_text_w("PTS"), 24, DIM);
        hud_rect(0, 31, HW, 1, RGB565C(26, 40, 62));
        for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) {
            const CueVrCarStand *st = &L->tab[i];
            int y = 34 + i * 10;
            int you = (st->id == CUEVR_CAR_PLAYER);
            if (you) hud_rect(0, y - 2, HW, 10, RGB565C(20, 34, 54));
            snprintf(v, sizeof v, "%d", i + 1);
            hud_text(v, 4, y, DIM);
            {   char nm[24];
                const char *who = you ? "YOU" : CUE_PERSONAS[st->id].name;
                hud_text(hud_fit_name(nm, sizeof nm, who, 58), 12, y,
                         you ? HI : TXT);
            }
            snprintf(v, sizeof v, "%d", st->played); hud_text(v, 74, y, DIM);
            snprintf(v, sizeof v, "%d", st->won);    hud_text(v, 88, y, DIM);
            snprintf(v, sizeof v, "%d", st->ff - st->fa);
            hud_text(v, 102, y, DIM);
            snprintf(v, sizeof v, "%d", st->points);
            hud_text_r(v, HW - 6, y, you ? HI : TXT);
        }
        if (L->complete) hud_text("SEASON FINISHED", 4, 88, LIVE);
        {
            int on = (S.car_row == 1);
            if (on) hud_rect(1, HH - 11, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text("BACK", 4, HH - 9, on ? HI : DIM);
            hud_text_r("DONE", HW - 6, HH - 9, on ? LIVE : DIM);
        }
        return;
    }

    /* ---- career: what you have won --------------------------------------- */
    if (S.state == ST_CARACH) {
        const CueVrCareer *C = &S.career;
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("ACHIEVEMENTS", 4, 1, HI);
        {
            char v[24]; int got = 0;
            for (int i = 0; i < CAR_ACH_N; i++) if (C->ach & (1u << i)) got++;
            snprintf(v, sizeof v, "%d/%d", got, CAR_ACH_N);
            hud_text_r(v, HW - 6, 3, LIVE);
        }
        hud_rect(0, 10, HW, 1, LINE);
        const int Y0 = 14, Y1 = HH - 22;
        int y = Y0 - S.car_scroll;
        for (int i = 0; i < CAR_ACH_N; i++) {
            int got = (C->ach & (1u << i)) != 0;
            /* +12, not +6: an entry is TWO lines and the second one is what
             * was running under the MORE row. */
            if (y >= Y0 && y + 12 <= Y1) {
                hud_text(cuevr_car_ach_name(i), 4, y, got ? HI : DIM);
                hud_text(got ? "WON" : cuevr_car_ach_how(i), 4, y + 6,
                         got ? LIVE : RGB565C(70, 78, 92));
            }
            y += 14;
        }
        {
            int total = CAR_ACH_N * 14, page = Y1 - Y0;
            int maxs = total - page; if (maxs < 0) maxs = 0;
            if (S.car_scroll > maxs) S.car_scroll = maxs;
            if (maxs > 0) {
                int on = (S.car_row == 1);
                if (on) hud_rect(1, HH - 20, HW - 2, 9, RGB565C(28, 58, 40));
                hud_text(S.car_scroll < maxs ? "MORE" : "BACK TO THE TOP",
                         4, HH - 18, on ? HI : DIM);
                hud_text_r(S.car_scroll < maxs ? "DOWN" : "UP", HW - 6, HH - 18, LIVE);
            }
        }
        {
            int on = (S.car_row == 0);
            if (on) hud_rect(1, HH - 11, HW - 2, 9, RGB565C(28, 58, 40));
            hud_text("BACK", 4, HH - 9, on ? HI : DIM);
            hud_text_r("DONE", HW - 6, HH - 9, on ? LIVE : DIM);
        }
        return;
    }

    /* ---- the six-ball clearance ------------------------------------------ *
     * Its own board, because none of a frame's furniture applies: there is no
     * opponent, no score and no ball on — there is a clock, six balls, and the
     * time to beat. */
    /* A DRILL IN PLAY. The same board as the clearance challenge, because it is
     * the same question: what am I chasing, how am I doing, and what is the best
     * I have done. A goalless position gets no clock and no target — it is a
     * place to play from, and saying "0.00 SECONDS" over it would be inventing
     * a competition nobody entered. */
    if (S.drill >= 0) {
        const CueVrDrill *d = &S.drills.slot[S.drill];
        char b2[48], nm[24];
        cuevr_drill_name(d, nm, sizeof nm);
        hud_height(CUEVR_HUD_BOARD_LH);
        hud_rect(0, 0, HW, 9, BAND);
        hud_text(nm, 4, 2, HI);
        hud_text_r(MENU[S.menu_sel].name, HW - 4, 2, DIM);
        hud_rect(0, 9, HW, 1, LINE);

        if (d->goal == CUEVR_GOAL_SETUP) {
            hud_text_2x("PLAY ON FROM HERE", 4, 16, TXT);
            hud_text("MENU  -  START AGAIN", 4, 30, DIM);
        } else {
            /* THE CLOCK, IF THIS ONE IS AGAINST THE CLOCK. Otherwise the top
             * of the board carries what you are chasing and how you have got
             * on with it before, which is what an untimed challenge has
             * instead of a time. */
            if (d->timed) {
                int cs = (int)(S.drill_t * 100.0f + 0.5f);
                snprintf(b2, sizeof b2, "%d.%02d", cs / 100, cs % 100);
                hud_text_xl(b2, 4, 14, S.drill_done ? (S.drill_beat ? LIVE : TXT) : TXT);
                hud_text("SECONDS", 4, 36, DIM);
                hud_text_r("BEST", HW - 6, 14, DIM);
                if (d->best > 0) snprintf(b2, sizeof b2, "%d.%02d", d->best / 100, d->best % 100);
                else             snprintf(b2, sizeof b2, "-");
                hud_text_r(b2, HW - 6, 22, d->best ? HI : DIM);
            } else if (d->goal == CUEVR_GOAL_POT) {
                /* THE BALLS IT WANTS, drawn as the balls. The clock's slot used
                 * to be filled with the goal's NAME and its how-to line — both
                 * of which the block below already says, so the board carried
                 * two "pot a ball" messages and never once said WHICH. That is
                 * the only thing a pot challenge needs to tell you. */
                hud_text("POT THESE", 4, 14, DIM);
                hud_rect(0, 21, HW, 18, HUD_BALLBG);
                uint8_t ids[24];
                int nb = cuevr_drill_ball_choices((int)d->kind, ids, 24), x = 10;
                for (int i = 0; i < nb && x < HW - 24; i++) {
                    if (!((d->need >> ids[i]) & 1u)) continue;
                    int done = (S.drill_got >> ids[i]) & 1u;
                    if (done) hud_rect(x - 7, 23, 14, 14, LIVE);
                    cue_render_ball_icon_hs(x, 30, 5, ids[i]);
                    x += 15;
                }
                if (d->tries) {
                    snprintf(b2, sizeof b2, "%d/%d", d->wins, d->tries);
                    hud_text_r(b2, HW - 6, 14, HI);
                }
            } else {
                hud_text_2x(cuevr_goal_name(d->goal), 4, 16, TXT);
                if (d->tries) {
                    snprintf(b2, sizeof b2, "%d OF %d", d->wins, d->tries);
                    hud_text_r(b2, HW - 6, 18, HI);
                    hud_text_r("DONE", HW - 6, 28, DIM);
                }
            }

            hud_rect(0, 45, HW, 1, LINE);
            if (S.drill_done && S.drill_won) {
                hud_text_2x(S.drill_beat ? "NEW RECORD" : "DONE", 4, 48, HI);
                hud_text("MENU  -  ANOTHER GO", 4, 62, DIM);
            } else if (!S.have_snap) {
                hud_text_2x(d->timed ? "PLAY TO START THE CLOCK" : "PLAY WHEN READY",
                            4, 48, HI);
                hud_text(cuevr_goal_how(d->goal), 4, 62, DIM);
            } else {
                switch (d->goal) {
                case CUEVR_GOAL_SCORE:
                    snprintf(b2, sizeof b2, "%d OF %d", S.drill_score, (int)d->target);
                    break;
                case CUEVR_GOAL_CLEAR: {
                    int left = 0;
                    for (int i = 1; i < S.nballs; i++) if (S.balls[i].on) left++;
                    snprintf(b2, sizeof b2, left == 1 ? "%d BALL LEFT" : "%d BALLS LEFT", left);
                    break;
                }
                default: {
                    /* How many of the asked-for balls are still standing —
                     * "POT IT" was the whole of it before, which is no help at
                     * all on a challenge that wants three. */
                    int want = 0, got = 0;
                    for (int i = 0; i < 32; i++) {
                        if (!((d->need >> i) & 1u)) continue;
                        want++;
                        if ((S.drill_got >> i) & 1u) got++;
                    }
                    if (want > 1) snprintf(b2, sizeof b2, "%d OF %d POTTED", got, want);
                    else          snprintf(b2, sizeof b2, "%s", "POT IT");
                    break;
                }
                }
                hud_text_2x(b2, 4, 48, TXT);
                hud_text(cuevr_goal_how(d->goal), 4, 62, DIM);
            }
        }
        return;
    }

    if (S.mini) {
        char b2[40];
        hud_height(CUEVR_HUD_BOARD_LH);
        hud_rect(0, 0, HW, 9, BAND);
        hud_text("SIX BALL CLEARANCE", 4, 2, HI);
        hud_text_r(MENU[S.menu_sel].name, HW - 4, 2, DIM);
        hud_rect(0, 9, HW, 1, LINE);

        /* The clock, big, and green the moment it is a record. */
        int cs = (int)(S.mini_t * 100.0f + 0.5f);
        snprintf(b2, sizeof b2, "%d.%02d", cs / 100, cs % 100);
        hud_text_xl(b2, 4, 14, S.mini_done ? (S.mini_beat ? LIVE : TXT) : TXT);
        hud_text("SECONDS", 4, 36, DIM);

        /* The record for THIS table. */
        {
            int k = (int)S.tab.kind;
            int best = (k >= 0 && k < CUE_GAME_COUNT) ? S.mini_best[k] : 0;
            hud_text_r("BEST", HW - 6, 14, DIM);
            if (best > 0) snprintf(b2, sizeof b2, "%d.%02d", best / 100, best % 100);
            else          snprintf(b2, sizeof b2, "-");
            hud_text_r(b2, HW - 6, 22, best ? HI : DIM);
        }

        hud_rect(0, 45, HW, 1, LINE);
        if (S.mini_done) {
            hud_text_2x(S.mini_beat ? "NEW RECORD" : "CLEARED", 4, 48, HI);
            hud_text("MENU FOR ANOTHER GO", 4, 62, DIM);
        } else if (!S.have_snap) {
            hud_text_2x("BREAK TO START THE CLOCK", 4, 48, HI);
        } else {
            int left = mini_left();
            snprintf(b2, sizeof b2, left == 1 ? "%d BALL LEFT" : "%d BALLS LEFT", left);
            hud_text_2x(b2, 4, 48, TXT);
        }

        /* The balls themselves, so the count is a glance and not a number. */
        {
            const int ry = 75, rr = 4, step = 10;
            int x = 8;
            hud_rect(0, 70, HW, HH - 70, HUD_BALLBG);
            hud_rect(0, 69, HW, 1, RGB565C(26, 40, 62));
            for (int i = 1; i < S.nballs; i++) {
                if (!S.balls[i].on) continue;
                cue_render_ball_icon_hs(x, ry, rr, S.balls[i].id);
                x += step;
            }
        }
        /* AFTER the balls, not before. Taking the early return at the top of
         * this board meant a record toast hid the two things you are actually
         * watching during a run — how many are left, and which — at the very
         * moment you pot the last one. It takes the message line, like it does
         * on the scoreboard, and nothing else. */
        hud_toast(TXT, DIM);
        return;
    }

    /* ---- in play: the scoreboard ----------------------------------------- *
     * Laid out like a television board rather than a handheld screen: a title
     * strip, then one wide row per player with the name on the left and the
     * score large on the right, and the striker's row picked out. That is the
     * shape because it is the shape that reads at a glance from across a table,
     * which is the only place you will ever be standing. */
    const char *me   = S.opp == OPP_ONLINE ? "YOU" : "YOU";
    const char *them = S.opp == OPP_PRACTICE ? "-" :
                       S.opp == OPP_ONLINE ? "OPPONENT" : CUE_PERSONAS[S.persona].name;
    char b[48];

    hud_rect(0, 0, HW, 9, BAND);
    hud_text(S.tab.is_snooker ? "SNOOKER" : "POOL", 4, 2, HI);
    if (S.opp == OPP_PRACTICE) hud_text("PRACTICE", 40, 2, LIVE);
    if (S.opp == OPP_ONLINE)
        hud_text(cuevr_net_state() == CUEVR_NET_LIVE ? "ONLINE" : "LINK LOST",
                 40, 2, cuevr_net_state() == CUEVR_NET_LIVE ? LIVE : RGB565C(230,80,60));
    if (S.rules.brk > 0) {
        snprintf(b, sizeof b, "BREAK %d", S.rules.brk);
        hud_text_r(b, HW - 4, 2, HI);
    }
    hud_rect(0, 9, HW, 1, LINE);

    /* Two player rows. */
    for (int p = 0; p < 2; p++) {
        int y = 11 + p * 19;
        int striker = (S.rules.turn == p);
        if (S.opp == OPP_PRACTICE && p == 1) {
            /* Nobody to play against — say so rather than showing a dead 0. */
            hud_text_2x("FREE TABLE", 8, y + 4, RGB565C(70, 78, 92));
            continue;
        }
        if (striker) {
            hud_rect(0, y, HW, 18, RGB565C(20, 34, 54));
            hud_rect(0, y, 3, 18, HI);           /* the striker's flash */
        }
        /* A face on each row, the way a broadcast board carries a headshot. You
         * get the cue ball, since the one thing every player has in common is
         * that they are the one down on the white. */
        if (p == 1 && S.opp == OPP_CPU) hud_face(11, y + 9, 16, S.persona);
        else                            cue_render_ball_icon_hs(11, y + 9, 7, CUE_ID_CUE);
        /* THE NAME FITS, OR IT SHRINKS, OR IT IS CUT. It was drawn at 2x from a
         * fixed column with the frame tally and the score at fixed columns of
         * their own, so a long one — "Professor Pete" — ran straight through
         * both. A scoreboard whose name overlaps its numbers is unreadable
         * exactly when you want to read it.
         *
         * The name column ends where the right-hand furniture begins, and that
         * differs between snooker and pool, so it is computed rather than
         * assumed. Big if it fits, small if it does not, cut if even that will
         * not do — in that order, because the size is worth more than the last
         * few letters. */
        {
            const char *nm = (p == 0) ? me : them;
            int x0 = 22;
            int right = S.tab.is_snooker
                      ? (S.rules.best_of > 1 ? HW - 36 : HW - 20)
                      : (S.rules.best_of > 1 ? HW - 46 : HW - 28);
            int room = right - x0;
            char cut[32];
            if (hud_text_w_2x(nm) <= room) {
                hud_text_2x(nm, x0, y + 3, striker ? TXT : DIM);
            } else if (hud_text_w(nm) <= room) {
                hud_text(nm, x0, y + 6, striker ? TXT : DIM);
            } else {
                size_t n2 = strlen(nm);
                if (n2 > sizeof cut - 1) n2 = sizeof cut - 1;
                memcpy(cut, nm, n2); cut[n2] = 0;
                while (n2 > 1 && hud_text_w(cut) > room) cut[--n2] = 0;
                hud_text(cut, x0, y + 6, striker ? TXT : DIM);
            }
        }
        if (S.tab.is_snooker) {
            snprintf(b, sizeof b, "%d", S.rules.score[p]);
            hud_text_r_xl(b, HW - 5, y + 1, striker ? TXT : DIM);
            /* Frames won, left of the points, only when there is a match to
             * count — a single frame has no tally worth the room. */
            if (S.rules.best_of > 1) {
                snprintf(b, sizeof b, "(%d)", S.rules.frames[p]);
                hud_text_r(b, HW - 34, y + 7, striker ? TXT : DIM);
            }
        } else {
            /* Pool has no running score, so the board shows what it does have:
             * which group you are on, and how many of it is left. */
            int left = 0;
            for (int i = 1; i < S.nballs; i++)
                if (S.balls[i].on && cue_rules_ball_legal(&S.rules, S.balls, S.nballs,
                                                          S.balls[i].id)) left++;
            if (S.rules.group[p]) {
                cue_render_group_icon_hs(HW - 13, y + 9, 8, S.rules.group[p]);
                if (striker) {
                    snprintf(b, sizeof b, "%d", left);
                    hud_text_r_xl(b, HW - 26, y + 1, TXT);
                }
            } else {
                hud_text_r("OPEN", HW - 6, y + 7, striker ? TXT : DIM);
            }
            if (S.rules.best_of > 1) {
                snprintf(b, sizeof b, "(%d)", S.rules.frames[p]);
                hud_text_r(b, HW - 44, y + 7, striker ? TXT : DIM);
            }
        }
    }

    /* Footer. Two lines and no more: a small status line, then ONE bigger line
     * that is either what the game is telling you or what the controls are. The
     * first version stacked a message on top of a prompt and the descenders of
     * one ran through the other. */
    hud_rect(0, 49, HW, 1, LINE);
    cue_rules_status(&S.rules, b, sizeof b);
    hud_text(b, 4, 51, TXT);

    if (S.tab.is_snooker) {
        /* Points still on the table, which is the number a snooker board always
         * carries: every red is worth itself plus the colour that follows it,
         * and once the reds are gone it is just the colours that are left. */
        int reds = 0, colours = 0;
        for (int i = 1; i < S.nballs; i++) {
            if (!S.balls[i].on) continue;
            int id = S.balls[i].id;
            /* Snooker ids: 1..15 are the reds, 20..25 the colours, and a colour's
             * value is its id less eighteen — yellow 20 is 2, black 25 is 7. */
            if (id >= 1 && id <= 15) reds++;
            else if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) colours += id - 18;
        }
        int rem = reds * 8 + colours;
        /* The other number a snooker board always carries: how far in front you
         * are. It is the figure that decides what shot to play — thirty ahead
         * with forty left is a safety game, thirty behind with twenty left is
         * over — and the board made you do the subtraction yourself, off two
         * scores at opposite ends of it. Green in front, red behind, from YOUR
         * side of the table. */
        int mine = (S.opp == OPP_ONLINE) ? S.net_me : 0;
        int diff = S.rules.score[mine] - S.rules.score[1 - mine];
        char d[16];
        snprintf(d, sizeof d, "%+d", diff);
        /* Nothing to say before anybody has scored: at the break it would read
         * "+0", which is not information, it is furniture. */
        int dw = (S.opp == OPP_PRACTICE ||
                  (S.rules.score[0] == 0 && S.rules.score[1] == 0))
               ? 0 : hud_text_w(d) + 5;
        if (dw)
            hud_text_r(d, HW - 42, 51,
                       diff > 0 ? LIVE : diff < 0 ? RGB565C(230, 80, 60) : DIM);
        snprintf(b, sizeof b, "REM %d", rem);
        /* Clear of the SPIN indicator, which lives at x 89..107 on this line and
         * was being drawn straight over this readout — the ball-on disc at
         * HW-22 was measured against, but the spin ball sits further left again
         * and is only there while you are down on the shot, which is exactly
         * when you are looking at the board. */
        hud_text_r(b, HW - 42 - dw, 51, DIM);
    }

    /* WHAT IS LEFT, as balls rather than as a number. "REM 43" is a fact you
     * have to decode; a row of the actual balls is the same fact read at a
     * glance, and it is what every real board and the 2D game show. Small, low,
     * and clear of the spin indicator's corner.
     *
     * Snooker: one red with a count, because fifteen identical discs is a smear,
     * then each colour still on in value order. Pool: the striker's own group,
     * one disc each, which is exactly the thing you are counting down. */
    {
        /* Its own band under everything else. It used to sit at row 63, which
         * is inside the one big message line — so "BALL IN HAND" was printed
         * straight through the balls it was talking about. */
        /* WHAT YOU ARE ON, THEN WHAT IS LEFT — one band, read left to right.
         *
         * The ball-on used to hang at (HW-14, 55), which is neither on the
         * status line nor in the balls band but straddling the rule between
         * them and half off the right edge. It is a ball; it belongs with the
         * balls, and it belongs FIRST, because "what am I on" is the question
         * you ask before "what is left". */
        /* Small. The balls are a reference strip, not the news — at radius 4
         * with white rims they dominated the whole board, and the rims read as
         * thick white rings around everything. Black on dark is better than a
         * ring: the coloured disc carries all the information there is. */
        /* Bottom band, BELOW the message zone. The strip sat at row 75 with
         * the help line at 70, and "CARRY IT WITH YOUR HAND" printed straight
         * through the rack it was standing over. Rows 51-73 belong to text,
         * 75 down belongs to the balls, and the divider is the fence. */
        const int ry = 79, rr = 3, step = 8, x0 = 28, xmax = HW - 8;
        int x = x0;
        hud_rect(0, 75, HW, HH - 75, HUD_BALLBG);
        hud_rect(0, 74, HW, 1, RGB565C(26, 40, 62));
        if (S.tab.is_snooker) {
            /* A nominated colour draws as THAT ball, not as the multicolour
             * "any colour" disc — the disc is only right before a nomination
             * exists. Reuse the clearance path by handing it the value. */
            if (S.rules.target == 1 && S.rules.nominated)
                cue_render_onball_icon_hs(13, ry, 4, 2, S.rules.nominated);
            else
                cue_render_onball_icon_hs(13, ry, 4, S.rules.target, S.rules.seq);
            hud_rect(24, 76, 1, 7, RGB565C(40, 60, 92));
        } else if (S.tab.kind == CUE_GAME_US9) {
            cue_render_ball_icon_hs(13, ry, 4, S.rules.seq > 0 ? S.rules.seq : 1);
            hud_rect(24, 76, 1, 7, RGB565C(40, 60, 92));
        } else x = 6;
        if (S.tab.is_snooker) {
            int reds = 0;
            for (int i = 1; i < S.nballs; i++)
                if (S.balls[i].on && S.balls[i].id >= 1 && S.balls[i].id <= 15) reds++;
            if (reds > 0) {
                cue_render_ball_icon_hs(x, ry, rr, 1);
                char rb[8]; snprintf(rb, sizeof rb, "x%d", reds);
                hud_text(rb, x + rr + 2, ry - 2, DIM);
                x += step + 8;
            }
            for (int v = 2; v <= 7 && x < xmax; v++) {
                int id = CUE_ID_YELLOW + (v - 2), on = 0;
                for (int i = 1; i < S.nballs; i++)
                    if (S.balls[i].on && S.balls[i].id == id) { on = 1; break; }
                if (!on) continue;
                cue_render_ball_icon_hs(x, ry, rr, id);
                x += step;
            }
        } else {
            int me = (S.opp == OPP_ONLINE) ? S.net_me : 0;
            int grp = S.rules.group[me];
            for (int i = 1; i < S.nballs && x < xmax; i++) {
                if (!S.balls[i].on) continue;
                int id = S.balls[i].id;
                if (S.tab.kind != CUE_GAME_US9) {
                    int g = (id >= 1 && id <= 7) ? 1 : (id >= 9 && id <= 15) ? 2 : 0;
                    if (grp && g != grp && id != 8) continue;
                    if (!grp && id == 8) continue;      /* open table: not yours yet */
                }
                cue_render_ball_icon_hs(x, ry, rr, id);
                x += step;
            }
        }
    }

    /* NO SPIN READOUT. On the handheld it was the only way to know where the tip
     * was going to land, because there was no tip — you set a number. Here you
     * are holding the cue and looking down it at the ball: the contact point is
     * the thing in front of your eyes, drawn at full size on the actual ball.
     * A second, smaller, mirror-imaged copy of it on a board at the far end of
     * the room is not a readout, it is a distraction that disagrees. */

    /* The one big line. */
    if (S.state == ST_PLACE) {
        hud_text_2x("BALL IN HAND", 4, 55, HI);
        hud_text("MOVE YOUR HAND TO POSITION IT", 4, 63, DIM);
        hud_text("TRIGGER TO PLACE", 4, 69, HI);
        return;
    }
    /* BEFORE the ordinary message, not after. A toast was losing to whatever
     * the game happened to be saying — "SNOOKER 12FT" beat a new personal
     * best, which is precisely the hiding-it-away this was built to stop.
     * FOUL: WRONG BALL will still be true in four seconds; a hundred break
     * happens once. */
    if (hud_toast(TXT, DIM)) return;

    if (S.msg_time > 0.0f) { hud_text_2x(S.msg, 4, 57, HI); return; }

    /* A FREE BALL IS A THING YOU HAVE BEEN GIVEN, and the board never said so:
     * you took one from the foul menu and the game went quiet about it. It says
     * which ball is named and that aiming names another. */
    if (S.rules.free_ball && (S.state == ST_AIM || S.state == ST_PLACE)) {
        char fb[40];
        snprintf(fb, sizeof fb, "FREE BALL: %s",
                 S.rules.free_ball_id ? cue_ball_short_name(S.rules.free_ball_id)
                                      : "AIM TO NAME ONE");
        hud_text_2x(fb, 4, 57, LIVE);
        hud_text("AIM AT A BALL TO NAME IT   MENU TO PICK", 4, 68, DIM);
        return;
    }
    if (S.state == ST_THINK)       hud_text_2x("OPPONENT THINKING...", 4, 57, DIM);
    else if (S.state == ST_CPUCUE) hud_text_2x("OPPONENT CUEING...", 4, 57, HI);
    else if (S.state == ST_ROLL)   hud_text_2x("...", 4, 57, DIM);
    else if (S.state == ST_AIM) {
        /* No "cue is off the ball" prompt: you can see whether you are on it,
         * and a panel telling you so is noise. */
        if (S.cue.stroking)       hud_text_2x("PUSH THROUGH THE BALL", 4, 57, HI);
        else if (S.cue.adjusting) hud_text_2x("MOVING YOUR BRIDGE HAND", 4, 57, HI);
        else if (S.opp == OPP_PRACTICE && S.have_snap && S.undo_hold > 0.0f) {
            hud_text_2x("HOLD B TO RETRY THE SHOT", 4, 57, HI);
            int w = (int)((float)(HW - 8) * (S.undo_hold / CUEVR_UNDO_HOLD));
            if (w > HW - 8) w = HW - 8;
            hud_rect(4, 67, w, 2, LIVE);
        }
        else if (S.opp == OPP_PRACTICE && S.have_snap)
            hud_text("TRIGGER AIM   HOLD B RETRY   MENU OPTIONS", 4, 60, DIM);
        else hud_text("TRIGGER AIM   GRIP MOVE HAND   MENU OPTIONS", 4, 60, DIM);
    }
}

/* ---- shots -------------------------------------------------------------- */

/* The striker's shot is about to start. cue_rules expects the host to have
 * decided whether they were snookered BEFORE it, because foul-and-a-miss turns
 * on it: a miss is only a miss if there was a ball on to be hit. cue_game does
 * this and I had not, which quietly disabled the whole rule. */
/* HOW FAST THE BALL LEAVES THE BED, or zero.
 *
 * A jump is not "the cue was elevated". The table FORCES the cue up — near a
 * cushion cue_table_min_elev asks for around thirty degrees so the shaft clears
 * the rail — so a jump measured off absolute elevation launches the white on
 * every shot played off a cushion, with no intent from anybody. What counts is
 * the elevation the player ADDED beyond what the table demanded.
 *
 * And it is a deadband, not a scale. Below the threshold this returns exactly
 * zero, so the shot is bit-for-bit the planar one it has always been: a hop of
 * a fraction of a millimetre would still suspend cloth friction for its whole
 * flight, and cloth friction is where draw, follow and stun come from. Losing
 * that intermittently, on an elevation difference nobody can see, would make
 * every shot in the game feel unreliable.
 *
 * Subtracting the threshold rather than testing against it keeps it continuous:
 * at the boundary the ball leaves the bed at nothing, and grows from there.
 * There is no angle at which one more degree turns nothing into a leap.
 *
 * IT DEPENDS ON THE VERTICAL MOMENTUM AND ON NOTHING ELSE. An earlier version
 * also demanded the tip be above centre, on the reasoning that a jump is a high
 * shot and anything under the ball is a scoop. That is wrong about the physics
 * and wrong about the shot. The ball is squeezed between the descending tip and
 * the slate, and how hard it is driven into the bed is speed x sin(elevation) —
 * where on the FACE the tip lands decides the spin it leaves with, not whether
 * it leaves at all. Cue down steeply on the lower half and you get a jump with
 * screw on it, which is a real and useful shot. A scoop is the tip going under
 * the ball's equator and lifting it, which is a different thing entirely and
 * not something an elevated stroke into the face can do. */
#define CUEVR_JUMP_MIN_VY 0.50f   /* m/s of bed rebound before it is a jump */
/* How much of the down-stroke the bed gives back. Raised from 0.50: a real
 * jump shot takes a firm stroke and a steep cue, but not as much of either as
 * this was asking for, and the shot was landing outside what a player can
 * comfortably swing in a headset. Thirty percent more return, which also drops
 * the force needed to cross the threshold at all by about a quarter — the two
 * are the same number and it was the launch that felt wrong, not the deadband. */
#define CUEVR_JUMP_BED_E  0.65f

static float jump_launch(float speed, float elev, float min_elev) {
    float deliberate = elev - min_elev;
    if (deliberate <= 0.0f) return 0.0f;       /* the table's angle, not yours */
    float raw = speed * sinf(deliberate) * CUEVR_JUMP_BED_E;
    return raw > CUEVR_JUMP_MIN_VY ? raw - CUEVR_JUMP_MIN_VY : 0.0f;
}

static void arm_shot(void) {
    S.rules.was_snookered = cue_rules_is_snookered(&S.rules, S.balls, S.nballs);
}

static void begin_shot(void) {
    /* THE TABLE AS THE STRIKER FOUND IT — whoever the striker is.
     *
     * This snapshot was taken in the human's strike path only, so after the
     * OPPONENT fouled, "PUT THE BALLS BACK" restored the last position the
     * HUMAN had played from — the shot before the one that laid the snooker.
     * The offender was sent back to a table two shots stale, and the snooker
     * the foul was committed against simply vanished. Reported, and I argued
     * with it instead of reading this function: every path — human, CPU and
     * the far end of a link — reaches begin_shot(), and none of the other two
     * ever took a snapshot at all.
     *
     * Safe here rather than before the strike: cue_phys_strike_elev writes
     * vel and w and never pos, and snap_restore_balls zeroes both. */
    snap_take();
    S.can_repick = 0;      /* the stroke is away: the ball is no longer in hand */
    for (int i = 0; i < S.nballs; i++) S.was_on[i] = S.balls[i].on;
    S.world.first_hit = -1;
    S.world.first_hit_idx = -1;
    S.world.jump_over = 0; S.world.jump_over_id = 0;
    S.world.jmp_pending = 0; S.world.jmp_idx = -1;
    S.world.jmp_hit_it = 0; S.world.jmp_bounced = 0;
    S.world.first_hit_idx = -1;
    S.shot_events = 0;
    S.state = ST_ROLL;
}

/* Which seat the human occupies, and whether this frame counts at all.
 * Practice is excluded: a table you can rearrange, undo on and re-rack at will
 * is not a table anyone sets a record on. */
static int stat_me(void)   { return (S.opp == OPP_ONLINE) ? S.net_me : 0; }
static int stat_mode(void) { return (S.opp == OPP_ONLINE) ? 1 : 0; }
static int stat_active(void) { return S.opp == OPP_CPU || S.opp == OPP_ONLINE; }

/* How many of MY object balls are still on. Snooker does not use this. */
static int stat_my_balls_left(void) {
    int n = 0;
    for (int i = 1; i < S.nballs; i++) {
        if (!S.balls[i].on) continue;
        int id = S.balls[i].id;
        if (S.rules.mode == CUE_GAME_US9) { n++; continue; }
        int g = (id >= 1 && id <= 7) ? 1 : (id >= 9 && id <= 15) ? 2 : 0;
        if (id == 8) { n++; continue; }             /* the 8 is always mine to pot */
        int mine = S.rules.group[stat_me()];
        if (mine == 0 || g == mine) n++;
    }
    return n;
}

/* A visit is starting. Remember whether the table is untouched from my point of
 * view, because that is what separates a clearance from a tidy-up. */
static void stat_visit_begins(int who) {
    S.stat_visit_owner = who;
    S.stat_visit_full = 0;
    if (!stat_active() || who != stat_me() || S.rules.kind) return;
    int full = (S.rules.mode == CUE_GAME_US9) ? 9
             : 8;                                   /* seven of a group plus the 8 */
    S.stat_visit_full = (stat_my_balls_left() >= full);
}

/* One stroke, recorded — for the frame screen, not the career records.
 *
 * Takes the shot's own facts rather than reading them back off the rules,
 * because by the time this runs the table may already have changed hands and
 * `turn` is somebody else. */
static void stat_shot(int p, const int *potted, int np) {
    if (p < 0 || p > 1) return;
    CueVrPlayStat *st = &S.fstat[p];
    int foul   = S.rules.last_foul;
    int scored = (!foul && np > 0);

    st->shots++;
    st->time += S.shot_clock;
    S.shot_clock = 0.0f;
    if (foul) st->fouls++;

    if (scored) {
        st->pot_shots++;
        st->potted += np;
        for (int k = 0; k < np; k++) {
            int id = potted[k];
            if (id > 0 && id < CUEVR_TALLY_N && st->tally[id] < 250) st->tally[id]++;
        }
    }

    /* The break. Snooker counts points and cue_rules already keeps that number
     * — including zeroing it on a foul or a miss — so take theirs rather than
     * keep a second one that can disagree with the scoreboard. Pool has no
     * running score, so a break there is what a pool player means by one: balls
     * in a row without giving the table up. */
    if (S.rules.kind) st->brk = S.rules.brk;
    else if (scored)  st->brk += np;
    else              st->brk = 0;

    if (st->brk > st->best_break) {
        st->best_break = st->brk;
        memcpy(st->best_tally, st->tally, sizeof st->best_tally);
    }
    if (st->brk == 0) memset(st->tally, 0, sizeof st->tally);
}

static void stat_after_shot(void);

/* CUEVR_BRKTEST=1 — drive the REAL tier counter through a scripted frame and
 * print what it made of it. A break of 57 built over six shots, then the
 * opponent's 45, is the whole question: three crossings for me and none for
 * them. Reading the loop was not settling it, and a counter that is wrong on
 * a real table is wrong here too. */
void cuevr_app_break_selftest(void) {
    static const struct { int brk, turn; const char *what; } SEQ[] = {
        {  1, 0, "red" },      {  8, 0, "black" },   { 15, 0, "red+black" },
        { 22, 0, "past 20" },  { 35, 0, "past 30" }, { 57, 0, "past 50" },
        {  0, 1, "I miss, table over" },
        { 10, 1, "them" },     { 25, 1, "them past 20" },
        { 45, 1, "them past 40" },
        {  0, 0, "they miss, back to me" },
        { 12, 0, "me again" }, { 21, 0, "past 20 again" },
    };
    S.opp = OPP_CPU;
    S.rules.kind = 1;
    S.stat_counted = 0;
    S.stat_prev_brk = 0;
    memset(S.stats.brk_tier, 0, sizeof S.stats.brk_tier);
    memset(S.stats.snk_best, 0, sizeof S.stats.snk_best);
    for (unsigned i = 0; i < sizeof SEQ / sizeof SEQ[0]; i++) {
        S.rules.brk = SEQ[i].brk;
        S.rules.turn = SEQ[i].turn;
        int b20 = S.stats.brk_tier[0][0];
        stat_after_shot();
        fprintf(stderr, "[brktest] brk=%-3d turn=%d  %-22s  20+=%d 30+=%d 50+=%d 100+=%d%s\n",
                SEQ[i].brk, SEQ[i].turn, SEQ[i].what,
                S.stats.brk_tier[0][0], S.stats.brk_tier[1][0],
                S.stats.brk_tier[2][0], S.stats.brk_tier[3][0],
                S.stats.brk_tier[0][0] != b20 ? "   <- counted" : "");
    }
    fprintf(stderr, "[brktest] expected 20+=2 30+=1 50+=1 100+=0 "
                    "(my 57, my 21; their 45 must not count)\n");
}

/* CUEVR_CONCEDETEST=1 — does the opponent actually shake hands?
 *
 * cue_rules_should_concede was right the whole time and its unit test passed
 * the whole time; the opponent still played every frame out to the last black.
 * The rule was being ASKED in hand_over(), which a rack goes through and an
 * ordinary shot does not — so the only moment the question ever got put was
 * before a ball had been struck, when the answer is always no.
 *
 * A unit test on the rule cannot see that. This drives the app's own state
 * through the app's own entry point: the frame is hopeless, the CPU is asked
 * to think, and it must be over before it thinks. */
void cuevr_app_concede_selftest(void) {
    struct { const char *what; int mine, theirs, reds, seq; int expect; } CASE[] = {
        { "on the colours, 40 behind with 27 on",     40 + 40, 40,  0, 2, 1 },
        { "on the colours, 30 behind with 27 on",     30 + 40, 40,  0, 2, 0 },
        { "on the black, 20 behind with 7 on",        20 + 40, 40,  0, 7, 1 },
        { "reds still up, 50 behind",                 50 + 40, 40,  8, 0, 0 },
        { "reds still up, hopeless",                 200 + 40, 40,  2, 0, 1 },
        { "in front",                                      40, 90,  0, 2, 0 },
    };
    int fail = 0;
    for (unsigned i = 0; i < sizeof CASE / sizeof CASE[0]; i++) {
        S.opp = OPP_CPU;
        cue_table_init(&S.tab, CUE_GAME_SNK15);
        cue_table_build_world(&S.tab, &S.world);
        S.nballs = cue_table_rack(&S.tab, S.balls);
        cue_rules_init(&S.rules, &S.tab, 1);
        S.rules.turn = 1;
        S.rules.score[0] = CASE[i].mine;
        S.rules.score[1] = CASE[i].theirs;
        S.rules.reds_left = CASE[i].reds;
        S.rules.seq = CASE[i].seq;
        S.rules.frame_over = 0;
        S.rules.conceded = 0;
        S.stat_folded = 1;              /* no match bookkeeping from a test */
        S.state = ST_THINK;             /* exactly what the callers do */
        think_start();
        think_join();
        int gave_up = (S.rules.conceded && S.state == ST_OVER);
        int good = (gave_up == CASE[i].expect);
        if (!good) fail++;
        fprintf(stderr, "[concede] %-42s  %-6s  %s\n", CASE[i].what,
                gave_up ? "shook" : "played",
                good ? "ok" : (CASE[i].expect ? "FAIL - should have conceded"
                                              : "FAIL - should have played on"));
    }
    fprintf(stderr, "[concede] %s\n", fail ? "FAILED" : "PASSED");
}

/* Called after every shot resolves, and once more when the frame ends. */
static void stat_after_shot(void) {
    if (!stat_active() || S.stat_counted) return;
    int me = stat_me(), md = stat_mode();
    /* SNOOKER: the highest break, per table size. r->brk is the live break and
     * it is still standing at the moment the shot resolves. */
    if (S.rules.kind && S.rules.turn == me) {
        int slot = cuevr_stat_snk_slot((int)S.tab.kind);
        if (slot >= 0 && S.rules.brk > S.stats.snk_best[slot][md]) {
            /* Only once the break is worth remarking on. Every frame starts by
             * beating a stored zero, and a toast for a break of 3 devalues the
             * one for a 70. */
            int was = S.stats.snk_best[slot][md];
            S.stats.snk_best[slot][md] = S.rules.brk;
            S.stat_dirty = 1;
            if (S.rules.brk >= 20 && was > 0) {
                char t[28];
                snprintf(t, sizeof t, "BREAK OF %d", S.rules.brk);
                toast_push(TOAST_RECORD, t, "your best on this table");
            }
        }
        /* Tier counts, on the CROSSING. A break only ever grows within a visit,
         * so a 57 passes 20, 30 and 50 exactly once each and counts once for
         * each — which is what "how many fifties have you made" means. Reading
         * the final figure at the end of a visit would have needed a visit-end
         * hook that does not exist; watching it go past does not. */
        static const int TIER[CUEVR_BRK_TIERS] = { 20, 30, 50, 100 };
        for (int a2 = 0; a2 < CUEVR_BRK_TIERS; a2++)
            if (S.stat_prev_brk < TIER[a2] && S.rules.brk >= TIER[a2]) {
                S.stats.brk_tier[a2][md]++;
                if (getenv("CUEVR_BRKDBG"))
                    fprintf(stderr, "[brk] tier %d++ -> %d   brk=%d prev=%d "
                            "turn=%d me=%d md=%d score=%d/%d\n",
                            TIER[a2], S.stats.brk_tier[a2][md], S.rules.brk,
                            S.stat_prev_brk, S.rules.turn, me, md,
                            S.rules.score[0], S.rules.score[1]);
                S.stat_dirty = 1;
                /* The fifty and the century are the ones worth announcing. A
                 * twenty is a good visit, not an event. */
                if (TIER[a2] >= 50) {
                    char t[28], b3[34];
                    snprintf(t, sizeof t, "%d BREAK", TIER[a2]);
                    snprintf(b3, sizeof b3, "%d made",
                             S.stats.brk_tier[a2][md]);
                    toast_push(TOAST_RECORD, t, b3);
                }
            }
    }
    S.stat_prev_brk = (S.rules.turn == me) ? S.rules.brk : 0;
    if (!S.rules.frame_over) return;
    S.stat_counted = 1;
    S.stats.frames_played[md]++;
    if (S.rules.winner == me) {
        S.stats.frames_won[md]++;
        /* POOL: a frame won in one visit from a full table. */
        if (!S.rules.kind && S.stat_visit_owner == me && S.stat_visit_full) {
            int slot = cuevr_stat_pool_slot((int)S.tab.kind);
            if (slot >= 0) {
                char b3[34];
                S.stats.pool_clear[slot][md]++;
                snprintf(b3, sizeof b3, "%d on this table",
                         S.stats.pool_clear[slot][md]);
                toast_push(TOAST_RECORD, "CLEARANCE", b3);
            }
        }
    }
    S.stat_dirty = 1;
}

/* The two things a main-menu row can do, so the pointer and the button cannot
 * end up doing different things on the same row. */
static void menu_change(int d) {
            switch (S.menu_row) {
            case MR_GAME:
                S.menu_sel = (S.menu_sel + d + MENU_N) % MENU_N;
                /* A grouped two-colour set cannot be used for 9-ball, and
                 * cue_theme knows which sets those are. */
                if (!cue_ballset_ok((int)MENU[S.menu_sel].kind, S.ballset)) {
                    for (int i = 0; i < CUE_NBALLSET; i++)
                        if (cue_ballset_ok((int)MENU[S.menu_sel].kind, i)) { S.ballset = i; break; }
                }
                break;
            case MR_OPP:      S.opp = (S.opp + d + OPP_N) % OPP_N; break;
            case MR_FRAMES:   S.match_idx = (S.match_idx + d + MATCH_LEN_N) % MATCH_LEN_N; break;
            case MR_STRENGTH: S.persona = (S.persona + d + CUE_NUM_PERSONAS) % CUE_NUM_PERSONAS; break;
            case MR_REFVOICE:
                S.ref_voice = (S.ref_voice + d + CUEVR_REF_N) % CUEVR_REF_N;
                /* Load it now, not at the next pot: six megabytes arriving in
                 * the middle of a frame is a dropped frame in a headset, and
                 * choosing a voice you cannot hear until you pot something is
                 * not choosing a voice. Say a number so the choice is audible
                 * from the menu. */
                cuevr_refcall_set_voice(S.ref_voice);
                if (S.ref_voice != CUEVR_REF_OFF) cuevr_refcall_say(147);
                break;
            /* Cloth, frame, table, lighting, balls and cue moved to the
             * APPEARANCE screen, which owns them for both entry points. */
            default: break;
            }
    /* Show it, do not just name it. */
    if (S.menu_row == MR_GAME) menu_preview();
    S.hud_dirty = 1;
}

static void menu_activate(void) {
    /* CAREER is not an opponent you rack against — it is the fixture list. */
    if (S.menu_row == MR_START && S.opp == OPP_CAREER) {
        if (S.career.active) { S.state = ST_CAREER; S.car_row = 0; }
        else {
            for (int i = 0; i < CUE_GAME_COUNT; i++) S.car_pick[i] = 0;
            S.car_pick[CUE_GAME_UK8] = 1;
            S.car_pick[CUE_GAME_SNK15] = 1;
            S.car_row = 0;
            S.state = ST_CARSETUP;
        }
        S.hud_dirty = 1;
        return;
    }
    if (S.menu_row == MR_APPEAR) {
        S.appear_from = ST_MENU;
        S.menu_row = AR_CLOTH;
        S.state = ST_APPEAR;
        S.hud_dirty = 1;
    } else if (S.menu_row == MR_STATS) {
        S.stat_scroll = 0;
        S.appear_from = ST_MENU;
        S.state = ST_STATS;
        S.hud_dirty = 1;
    } else if (S.menu_row == MR_CONTROLS) {
        S.appear_from = ST_MENU;
        S.menu_row = CR_HAND;
        S.state = ST_CONTROLS;
        S.hud_dirty = 1;
    } else if (S.menu_row == MR_START) {
        cue_render_set_ball_set(S.ballset);
        if (S.opp == OPP_CHALLENGE) {
            /* A challenge is CHOSEN, not started. There is no frame to rack
             * until you have said which one, so START opens the list. */
            S.state = ST_DRILLS;
            S.drill_row = 0; S.drill_scroll = 0;
            S.ptr_latch = 1;
            S.hud_dirty = 1;
        } else if (S.opp == OPP_ONLINE) {
            /* The lobby first — there is no frame to start until there
             * is somebody to play. */
            /* WHAT WE ARE, BEFORE THE LINK CAN GO LIVE.
             *
             * The hello is sent from inside cuevr_net_task the instant the two
             * ends connect, and that runs before any of the lobby's own code
             * gets a look in. Filling it in when the match STARTS meant the
             * packet had already gone out carrying kind 0 — so the joiner
             * dutifully adopted "game zero" and racked pool against a host
             * playing snooker. Set it here, where the choice is made. */
            /* Draw for the break here, and tell the other end. Whoever turns
             * out to be the host, their answer is the one both sides rack to —
             * there is no later packet that could correct it. */
            S.break_first = coin_toss();
            cuevr_net_set_hello((int)MENU[S.menu_sel].kind, S.cue_idx,
                                S.break_first, MATCH_LEN[S.match_idx]);
            S.lb_screen = LB_TRANSPORT;
            S.lb_sel = 0;
            /* The press that opened the lobby must not also answer its
             * first question — otherwise the transport screen flashes
             * past and you are on a transport you never chose. */
            S.lb_latch = 1;
            S.state = ST_LOBBY;
        } else {
            S.break_first = coin_toss();
            start_frame(MENU[S.menu_sel].kind);
            hand_over();
        }
        S.hud_dirty = 1;
    }
    S.hud_dirty = 1;
}

/* One step of a shot in flight, with its sounds. Pulled out of ST_ROLL so a
 * menu opened mid-shot does not STOP the shot.
 *
 * It used to live inside the ST_ROLL case, so opening appearance while the
 * balls were running froze them mid-roll: the state machine was somewhere else
 * and nobody was stepping the physics. Coming back resumed a shot that had been
 * standing still for however long you were choosing a cloth, with the audio
 * clock and the event bits from before still pending — which is what "goes
 * weird" was.
 *
 * Now the balls run to a stop whatever screen is in front of you, and ST_ROLL's
 * "settled, so resolve" fires the moment you come back to it. */
static int roll_step(float dt) {
    uint32_t ev = 0;
    int moving = cue_phys_step(&S.world, S.balls, S.nballs, dt, &ev);
    S.shot_events |= ev;
    if (ev & CUE_EV_BALL_HIT) {
        float i = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
        cue_audio_sfx(CUE_SFX_CLACK, i > 0.05f ? i : 0.5f);
        mote_xr_haptic(0.18f, 25);
    }
    if (ev & CUE_EV_CUSHION) {
        float i = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
        cue_audio_sfx(CUE_SFX_CUSHION, i);
    }
    if (ev & CUE_EV_JAW) cue_audio_sfx(CUE_SFX_CUSHION, 0.55f);
    /* A jumped ball coming down on the slate. Nearer a clack than a cushion —
     * it is a hard thing landing on a hard thing through a thin cloth. */
    /* A ball coming down on the slate is NOT two balls meeting. The clack is
     * the most recognisable sound in the game and hearing it while the white is
     * still in the air reads as a phantom contact — reported as "a ball
     * clacking sound as it flies through the air". The cushion sample is the
     * nearest honest thing there is: a ball against something solid and damped
     * rather than against another ball. */
    if (ev & CUE_EV_BED) cue_audio_sfx(CUE_SFX_CUSHION, 0.4f);
    if (ev & CUE_EV_POCKET) {
        float i = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
        cue_audio_sfx(CUE_SFX_POT, i > 0.1f ? i : 0.45f);
        mote_xr_haptic(0.5f, 60);
    }
    cue_audio_tick(dt);
    return moving;
}

/* THE HOST SAYS WHERE EVERYTHING IS — the balls AND the rules, whole.
 *
 * Lockstep assumes both machines get the same answer from the same numbers, and
 * floating point across two chips does not promise that. But the worse problem
 * was never the floats: it was that this used to send nine hand-picked rule
 * fields, and every field NOT picked was a desync nobody had thought of yet —
 * the 8-ball groups, the open table, the UK two-shot carry, the 9-ball foul
 * count, the free ball, the called-miss tallies, the frames won, the match. So
 * the whole struct goes. Three hundred bytes, once a shot, and no field left to
 * forget.
 *
 * Sent from every point that CHANGES the table without a shot as well —
 * decisions, concessions, a new rack — because those are exactly the moments a
 * missing field used to bite. */
_Static_assert(sizeof(CueRules) <= CUEVR_NET_RULES_MAX,
               "CueRules outgrew the state packet — raise CUEVR_NET_RULES_MAX");

static void net_push_state(void) {
    if (S.opp != OPP_ONLINE || S.net_me != 0) return;
    if (cuevr_net_state() != CUEVR_NET_LIVE) return;
    CueVrNetState st;
    memset(&st, 0, sizeof st);
    st.n = (uint8_t)(S.nballs > CUEVR_NET_MAXBALLS ? CUEVR_NET_MAXBALLS : S.nballs);
    for (int i = 0; i < st.n; i++) {
        st.on[i] = (uint8_t)S.balls[i].on;
        st.x[i]  = S.balls[i].pos.x;
        st.z[i]  = S.balls[i].pos.z;
    }
    st.rules_len = (uint16_t)sizeof(CueRules);
    memcpy(st.rules, &S.rules, sizeof(CueRules));
    if (getenv("CUEVR_NETDBG"))
        fprintf(stderr, "[netdbg] f%d SEND state turn=%d on0=%d state=%s\n",
                S.dbg_frame, S.rules.turn, S.balls[0].on, cuevr_app_state_name());
    cuevr_net_send_state(&st);
}

static void resolve_shot(void) {
    int potted[CUE_MAX_BALLS], np = 0, scratch = 0, n_off = 0;
    for (int i = 0; i < S.nballs; i++) {
        if (S.was_on[i] && !S.balls[i].on) {
            /* DOWN A POCKET OR OFF THE TABLE — not the same thing, and they
             * arrive here looking identical. A ball driven off the table is a
             * foul in every game; a potted one very often is not. The ids still
             * go through the potted list because every OTHER consequence is the
             * same (a colour respots, the black is a lost frame, the 9 is
             * spotted); n_off is what adds the foul on top. */
            int off = (S.balls[i].pocket == CUE_OFF_TABLE);
            if (S.balls[i].id == CUE_ID_CUE) scratch = 1;
            else { potted[np++] = S.balls[i].id; if (off) n_off++; }
        }
    }
    S.rules.n_off = n_off;
    /* AND WHETHER IT WAS A JUMP SHOT, as snooker defines one — which is not
     * "did the white leave the bed". WPBSA Definition 20 is about passing over
     * a ball, and it excepts three quite ordinary ways of doing that, so only
     * the integrator can answer it: it is a question about the order things
     * happened in between the strike and the settle. This flag used to be set
     * at the strike from vy alone, which fouled a hop over open cloth and
     * fouled all three exceptions. */
    S.rules.jumped = S.world.jump_over;
    LOGI("[cuevr] settle: cue at %.2f,%.2f  first_hit %d  potted %d  scratch %d",
         (double)S.balls[0].pos.x, (double)S.balls[0].pos.z, S.world.first_hit, np, scratch);
    /* A DRILL IS NOT A FRAME EITHER, and for the same reason: a saved position
     * is whatever you set out, and the rules of the game have no opinion about
     * a table with three balls on it in the wrong places. Every goal is judged
     * over ONE VISIT — the attempt ends when the table would have changed hands
     * — because that is what makes it practice and not a frame you can grind.
     *
     * A failed attempt puts the balls straight back. A drill you have to reset
     * by hand is a drill nobody plays twice. */
    if (S.drill >= 0 && !S.drill_done) {
        CueVrDrill *d = &S.drills.slot[S.drill];
        int pot_value = 0, cleared = 1;
        for (int k = 0; k < np; k++) {
            pot_value += drill_value(potted[k]);
            S.drill_got |= drill_need_bit(potted[k]);
        }
        for (int i = 1; i < S.nballs; i++) if (S.balls[i].on) cleared = 0;
        S.drill_score += pot_value;
        S.drill_pots  += np;

        int won = 0, lost = 0;
        switch (d->goal) {
        /* EVERY BALL IT ASKED FOR, which is what the ball grid sets. This
         * compared each potted ball against `d->ball` — the single legacy id
         * kept only so old files still read — so a challenge built by ticking
         * the black was judged against whatever that field happened to hold,
         * and potting the black did nothing at all. */
        case CUEVR_GOAL_POT:
            won = d->need ? ((S.drill_got & d->need) == d->need) : 0;
            break;
        case CUEVR_GOAL_SCORE: won = (S.drill_score >= d->target); break;
        case CUEVR_GOAL_CLEAR: won = cleared; break;
        default: break;
        }
        /* The visit is over the moment nothing goes down, or the white does.
         * Off the table counts as nothing going down, which it is. */
        if (!won && d->goal != CUEVR_GOAL_SETUP && (np == 0 || scratch || n_off))
            lost = 1;

        if (won || lost) {
            S.drill_done = 1;
            S.drill_won = won;
            d->tries++;
            if (won) {
                d->wins++;
                /* A RECORD ONLY EXISTS IF THERE IS A CLOCK. An untimed
                 * challenge's record is that you did it, and how often — a
                 * "best time" for one is a number about nothing. */
                if (d->timed) {
                    int cs = (int)(S.drill_t * 100.0f + 0.5f);
                    if (d->best == 0 || cs < d->best) {
                        int was = d->best;
                        d->best = cs;
                        S.drill_beat = 1;
                        char t2[28], b3[36];
                        snprintf(t2, sizeof t2, "%d.%02d SECONDS", cs / 100, cs % 100);
                        if (was) snprintf(b3, sizeof b3, "beat %d.%02d", was / 100, was % 100);
                        else     snprintf(b3, sizeof b3, "first time");
                        toast_push(TOAST_RECORD, t2, b3);
                    }
                }
                snprintf(S.msg, sizeof S.msg, S.drill_beat ? "NEW RECORD" : "DONE");
            } else {
                snprintf(S.msg, sizeof S.msg, scratch ? "IN OFF - AGAIN"
                                            : n_off   ? "OFF THE TABLE - AGAIN"
                                                      : "MISSED - AGAIN");
            }
            S.msg_time = 3.0f;
            if (S.drill_path[0]) cuevr_drills_save(&S.drills, S.drill_path);
            S.hud_dirty = 1;
            /* Straight back to the position, win or lose. The record screen is
             * where results are looked at; the table is where you practise. */
            if (!won) { drill_again(); return; }
        }
        /* Still in the visit — or a goalless position, which just plays on. */
        if (scratch) {
            S.balls[0].pos = cue_table_cue_home(&S.tab);
            S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);
            S.balls[0].on = 1;
        }
        if (S.rules.ball_in_hand) {
            S.rules.ball_in_hand = 0;
            if (!S.drill_done) { S.state = ST_PLACE; S.place_latch = 1; S.recentre = 1; return; }
        }
        arm_shot();
        S.state = ST_AIM;
        S.hud_dirty = 1;
        return;
    }

    /* THE CHALLENGE IS NOT A FRAME. Six reds with no colours is not a position
     * the snooker rules have an opinion about, and running them here would call
     * half the pots fouls. Count what went down, put the white back if it went
     * with them, and stop the clock on the last one. */
    if (S.mini) {
        if (scratch) {
            S.balls[0].pos = cue_table_cue_home(&S.tab);
            S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);
            S.balls[0].on  = 1;
            S.rules.ball_in_hand = 1;
        }
        int left = mini_left();
        if (np > 0) {
            snprintf(S.msg, sizeof S.msg, left ? "%d TO GO" : "CLEARED", left);
            S.msg_time = 1.5f;
        }
        if (left == 0 && !S.mini_done) {
            S.mini_done = 1;
            int cs = (int)(S.mini_t * 100.0f + 0.5f);
            int k = (int)S.tab.kind;
            if (k >= 0 && k < CUE_GAME_COUNT &&
                (S.mini_best[k] == 0 || cs < S.mini_best[k])) {
                int was = S.mini_best[k];
                S.mini_best[k] = cs;
                S.mini_beat = 1;
                S.stat_dirty = 1;
                {
                    char t[28], b3[34];
                    snprintf(t, sizeof t, "%d.%02d SECONDS", cs / 100, cs % 100);
                    if (was) snprintf(b3, sizeof b3, "beat %d.%02d", was / 100, was % 100);
                    else     snprintf(b3, sizeof b3, "first time on this table");
                    toast_push(TOAST_RECORD, t, b3);
                }
            }
            snprintf(S.msg, sizeof S.msg, S.mini_beat ? "NEW RECORD" : "CLEARED");
            S.msg_time = 4.0f;
        }
        S.hud_dirty = 1;
        if (S.rules.ball_in_hand) {
            S.rules.ball_in_hand = 0;
            if (!S.mini_done) { S.state = ST_PLACE; S.place_latch = 1; S.recentre = 1; return; }
        }
        arm_shot();
        S.state = ST_AIM;
        return;
    }

    /* FREE PRACTICE HAS NO RULES.
     *
     * A practice table is not a frame. Running the rules on one meant a knock
     * about was scored, penalised and had the table taken off you for missing —
     * on a table with nobody at the other end of it. You would be told you had
     * fouled for not hitting a red, on a table where you had put four balls out
     * to practise a cut.
     *
     * So the rules only run when there is something being judged: a drill with
     * an actual goal, or the timed clearance. A drill that is "just the
     * position" is a place to play from, which is free play with the balls
     * arranged — the user's own distinction, and the right one.
     *
     * Everything that is left is the physical part: colours go back if that is
     * switched on, the white comes back if it went down, and it stays your
     * shot because there is nobody to hand it to. */
    int judged = (S.drill >= 0 && S.drills.slot[S.drill].goal != CUEVR_GOAL_SETUP)
              || S.mini;
    if (S.opp == OPP_PRACTICE && !judged) {
        if (S.rules.kind && S.prac_respot) {
            for (int i = 1; i < S.nballs; i++) {
                int id = S.balls[i].id;
                if (S.balls[i].on) continue;
                if (id < CUE_ID_YELLOW || id > CUE_ID_BLACK) continue;
                cue_rules_respot(&S.rules, S.balls, S.nballs, id);
            }
        }
        S.rules.turn = 0;
        S.rules.last_foul = 0;
        S.rules.last_miss = 0;
        S.rules.decision = CUE_DEC_NONE;
        S.rules.free_ball = 0;
        S.rules.brk = 0;
        S.rules.n_off = 0; S.rules.jumped = 0;
        if (np > 0) {
            snprintf(S.msg, sizeof S.msg, np == 1 ? "POTTED" : "%d DOWN", np);
            S.msg_time = 1.2f;
        }
        S.hud_dirty = 1;
        net_push_state();
        if (scratch || n_off) {
            S.balls[0].pos = cue_table_cue_home(&S.tab);
            S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);
            S.balls[0].on = 1;
            S.state = ST_PLACE; S.place_latch = 1; S.recentre = 1;
            return;
        }
        arm_shot();
        S.state = ST_AIM;
        return;
    }

    int cushion = (S.shot_events & CUE_EV_CUSHION) != 0;
    int was_turn = S.rules.turn;
    int cpu_played = (S.rules.cpu && was_turn == 1);
    int score_before = S.rules.score[1 - was_turn];
    cue_rules_resolve(&S.rules, S.balls, S.nballs, &S.world,
                      S.world.first_hit, scratch, cushion, potted, np);
    /* Tell the planner whether ITS shot fouled, so it stops offering the same
     * one. A penalty landing on the other player is the only reliable signal
     * from out here that a foul was given. */
    if (cpu_played) {
        if (S.rules.score[1 - was_turn] > score_before) {
            /* which ball it actually hit matters as much as which it meant to:
             * the planner steers away from the offender, not just the target */
            int hit = (S.world.first_hit >= 0 && S.world.first_hit < S.nballs)
                    ? S.balls[S.world.first_hit].id : -1;
            cue_ai_note_foul(S.cpu_shot.target_id, hit);
        }
        else
            cue_ai_clear_fouls();
    }
    snprintf(S.msg, sizeof S.msg, "%s", S.rules.msg);
    S.msg_time = 2.5f;
    S.hud_dirty = 1;

    /* PRACTICE, SNOOKER: keep the colours on the table.
     *
     * The rules respot a colour whenever the shot was a foul or the striker was
     * not in the clearance sequence, which is right for a frame — but a practice
     * table is not being played out, it is being practised on, and there is no
     * reason for it to strip down to a handful of reds with nothing to take
     * after them. With this on, every colour goes straight back while reds
     * remain, whatever the sequence said. Off, practice follows the frame rules.
     *
     * After cue_rules_resolve, so it is the last word — and only in practice, so
     * nothing here can touch a match or a lockstep frame. */
    if (S.opp == OPP_PRACTICE && S.rules.kind && S.prac_respot &&
        S.rules.reds_left > 0) {
        for (int i = 1; i < S.nballs; i++) {
            int id = S.balls[i].id;
            if (S.balls[i].on) continue;
            if (id < CUE_ID_YELLOW || id > CUE_ID_BLACK) continue;
            cue_rules_respot(&S.rules, S.balls, S.nballs, id);
        }
    }

    /* THE REFEREE CALLS THE BREAK, which is most of what a snooker frame
     * sounds like. After every pot, before the turn is routed — r->brk is the
     * break the striker has just added to and it is about to be reset if the
     * table changes hands.
     *
     * Only on a legal pot: a referee calls a foul, he does not call a total
     * that has just stopped counting, and the fouls have their own line on the
     * panel. Snooker only, because a break total is a snooker idea — potting
     * five reds in eight-ball is not a break of five of anything. */
    if (S.rules.kind && np > 0 && !S.rules.last_foul && S.rules.brk > 0)
        cuevr_refcall_say(S.rules.brk);

    /* AND THE CALLS THAT ARE NOT NUMBERS. A frame does not sound officiated
     * because the totals are read out — it sounds officiated because the fouls
     * are called. Snooker only, like the totals: the other games have no
     * referee and no misses.
     *
     * The warning goes BEHIND the foul call rather than instead of it, which is
     * what the queue in the mixer is for: "Foul and a miss. Two consecutive
     * fouls, a third loses the frame." is one breath from an official and two
     * separate recordings here. */
    if (S.rules.kind && S.rules.last_foul) {
        int off = S.rules.dec_offender;
        cuevr_refcall_say(S.rules.last_miss ? CUEVR_SAY_FOUL_MISS
                                            : CUEVR_SAY_FOUL);
        /* Two on the board and a third ends it. Said once, as it happens —
         * announcing it every shot afterwards would be nagging. */
        if (off >= 0 && off < 2 && S.rules.cmiss[off] == 2)
            cuevr_refcall_say_after(CUEVR_SAY_TWO_FOULS);
    }

    /* Records, before the turn is routed: r->brk is this visit's break and it
     * is about to be reset if the table changes hands. */
    stat_shot(was_turn, potted, np);
    stat_after_shot();
    if (S.rules.turn != was_turn) stat_visit_begins(S.rules.turn);

    /* THE STATE GOES OUT WHEN THE TABLE IS FINISHED WITH, at every one of this
     * function's exits — not straight after cue_rules_resolve, which is where it
     * was and which is a table nobody ever plays from.
     *
     * A scratch is the case that showed it. resolve() leaves the white potted
     * and sets ball_in_hand; the block further down is what puts it back on the
     * cloth. Sending between the two told the far end the cue ball was off the
     * table — arriving just after the striker there had already picked it up, so
     * they aimed and played a shot with the white flagged as potted. The balls
     * agreed, the scores agreed, and the one number that did not was the one
     * that decides whether the cue ball gets drawn at all. */
    if (S.rules.frame_over) { net_push_state(); enter_over(); return; }

    /* A pending decision is the rules engine asking a question — after a
     * snooker foul (play on / make them play again / free ball) or before the
     * first shot of a 9-ball frame (push out?). It waits for an answer, and
     * never answering is not neutral: the frame simply plays on under the wrong
     * assumption. The player gets asked; the CPU decides for itself. */
    if (S.rules.pushout_offer || S.rules.decision == CUE_DEC_PENDING) {
        /* WHO answers is not the same question for the two offers.
         *
         * A push-out belongs to the player about to play, so that one follows
         * the turn. A snooker foul does not: the choice is the fouled-AGAINST
         * player's, and cue_rules deliberately leaves r->turn sitting on the
         * offender until the decision is applied (it only moves in
         * cue_rules_apply_decision). Testing the turn therefore handed every
         * foul decision to whoever committed the foul — so you were asked to
         * choose after your own foul, and the CPU quietly chose for itself
         * after its own. Exactly backwards, both ways round. */
        int decider = S.rules.pushout_offer ? S.rules.turn
                                            : 1 - S.rules.dec_offender;
        /* Reported because a missed snooker escape was seen NOT to offer the
         * replay, and the rules path that decides it reads correctly — so the
         * next time it happens this says which link broke: whether the miss was
         * called at all, whether a restore was on offer, and who was asked. */
        LOGI("[cuevr] foul decision: offender %d decider %d  can_restore %d "
             "free_ball %d scratch %d pushout %d cpu %d",
             S.rules.dec_offender, decider, S.rules.dec_can_restore,
             S.rules.dec_free_ball, S.rules.dec_scratch,
             S.rules.pushout_offer, S.rules.cpu);
        if (S.rules.cpu && decider == 1) {
            if (S.rules.pushout_offer) {
                CueAIShot p = cue_ai_pushout(&S.world, &S.tab, &S.rules,
                                             S.balls, S.nballs,
                                             &CUE_PERSONAS[S.persona], &S.rng);
                S.rules.is_pushout = p.valid;
                S.rules.pushout_offer = 0;
                S.rules.pushout_avail = 0;
            } else {
                /* The CPU is the fouled-against player here, always. It weighs
                 * the three answers against the table it is actually looking at
                 * — cue_ai_decide is the same code the handheld uses. This was a
                 * one-liner that took the replay whenever one was on offer, so
                 * the table came straight back to you after every foul. */
                int dec = cue_ai_decide(&S.world, &S.tab, &S.rules, S.balls,
                                        S.nballs, &CUE_PERSONAS[S.persona], &S.rng);
                if (dec == CUE_DEC_REPLAY) snap_restore_balls();
                cue_rules_apply_decision(&S.rules, dec);
                if (S.rules.free_ball) cuevr_refcall_say(CUEVR_SAY_FREE_BALL);
                snprintf(S.msg, sizeof S.msg, "%s",
                         dec == CUE_DEC_REPLAY   ? "PLAY THE SHOT AGAIN"
                       : dec == CUE_DEC_FREEBALL ? "FREE BALL"
                                                 : "OPPONENT PLAYS ON");
                S.msg_time = 3.0f;
            }
        } else {
            S.state = ST_DECIDE;
            S.hud_dirty = 1;
            net_push_state();
            return;
        }
    }

    /* WHOSE TABLE IT IS NOW.
     *
     * Online this routing is the whole match, and it was not being done here at
     * all. `cpu` is 0 in an online frame, so the tail below always took its else
     * branch and put BOTH ends into ST_AIM after every shot. ST_THINK is the
     * only state that reads an incoming shot, so the next stroke — whichever end
     * played it — arrived at a socket nobody was listening to: it latched, was
     * never consumed, no balls moved, no resolve ran, and so the host never sent
     * the state that would have corrected anything. The frame stopped dead and
     * stayed stopped. What you saw from the far end was their cue swinging
     * through the white, because the non-striker's `mine` test in ST_AIM throws
     * the strike away.
     *
     * The BREAK worked because a rack routes through hand_over(), which is the
     * one place that asked whose seat it is. Every shot after it came through
     * here. */
    int mine = (S.opp != OPP_ONLINE) || (S.rules.turn == S.net_me);

    if (S.rules.ball_in_hand) {
        /* Ball in hand. Start it on its home spot, legal by construction, and
         * let the player walk it about with the left stick before playing. */
        S.balls[0].pos = cue_table_cue_home(&S.tab);
        S.balls[0].vel = (Vec3){0, 0, 0};
        S.balls[0].w   = (Vec3){0, 0, 0};
        S.balls[0].on  = 1;
        S.rules.ball_in_hand = 0;
        if (!mine) {
            /* Theirs to place, and only they know where. It waits on the home
             * spot until their shot arrives carrying the spot they chose. */
        } else if (!(S.rules.cpu && S.rules.turn == 1)) {
            S.state = ST_PLACE;
            S.place_latch = 1;
            S.recentre = 1;
            S.hud_dirty = 1;
            net_push_state();
            return;
        } else {
            /* The CPU places for itself. */
            S.balls[0].pos = cue_ai_place(&S.world, &S.tab, &S.rules, S.balls,
                                          S.nballs, &CUE_PERSONAS[S.persona],
                                          &S.rng);
        }
    }

    arm_shot();
    if (S.opp == OPP_ONLINE) {
        S.state = mine ? ST_AIM : ST_THINK;
    } else if (S.rules.cpu && S.rules.turn == 1) {
        S.state = ST_THINK;
        think_start();
    } else {
        S.state = ST_AIM;
    }
    net_push_state();
}

/* CUEVR_DRILLTEST=1 — does potting the ball a challenge ASKED FOR finish it?
 *
 * Reported: "with one set up to pot just blacks, I potted the black and
 * nothing happened". It did nothing because the win test compared each potted
 * ball against `d->ball`, the single legacy id kept only so files written
 * before the ball GRID existed still load. The grid sets `need`, a mask, and
 * nothing was reading it — so the challenge you built was judged against a
 * field you had never touched.
 *
 * A unit test on the mask would have proved the mask works. This plays the
 * shot: a real strike, the real solver, and the app's own resolve_shot, so
 * what is being asked is "does potting it finish the challenge" rather than
 * "does this expression evaluate the way I just wrote it".
 */
void cuevr_app_drill_selftest(void) {
    /* The third is the reported failure, and it is the one that matters.
     *
     * The ball grid offers RED first, so `d->ball` — which is set to the first
     * ticked id in grid order — lands on RED the moment a red is among them,
     * or simply keeps whatever it was set to when the goal was chosen. Judging
     * on that field while the grid says BLACK is a challenge that cannot be
     * completed by doing what it shows you. Potting the black did nothing, and
     * nothing is what it looked like: not a win, not a miss, no message. */
    struct { const char *what; int nballs_needed; int stale_ball; int expect; } CASE[] = {
        { "pot the black, asked for the black",     1, 0, 1 },
        { "pot the black, asked for black AND blue", 2, 0, 0 },
        { "pot the black, grid says black, ball says red", 1, 1, 1 },
    };
    int fail = 0;
    for (unsigned c = 0; c < sizeof CASE / sizeof CASE[0]; c++) {
        S.opp = OPP_CHALLENGE;
        cue_table_init(&S.tab, CUE_GAME_SNK15);
        cue_table_build_world(&S.tab, &S.world);
        S.nballs = cue_table_rack(&S.tab, S.balls);
        cue_rules_init(&S.rules, &S.tab, 0);
        S.rules.turn = 0; S.rules.ball_in_hand = 0;
        S.drill_path[0] = 0;

        /* A challenge that wants the black — and, in the second case, the blue
         * as well, so "all of them" is tested and not just "one of them". */
        CueVrDrill *d = &S.drills.slot[0];
        drill_capture(d);
        d->goal = CUEVR_GOAL_POT;
        d->need = (1u << CUE_ID_BLACK);
        if (CASE[c].nballs_needed > 1) d->need |= (1u << CUE_ID_BLUE);
        d->ball = CASE[c].stale_ball ? 1 : CUE_ID_BLACK;   /* 1 = a red */
        d->timed = 0;
        drill_start(0);

        /* Everything off but the white and the black, the black hanging over a
         * top pocket with the white straight behind it. */
        int ib = -1;
        for (int i = 1; i < S.nballs; i++) {
            S.balls[i].on = 0;
            if (S.balls[i].id == CUE_ID_BLACK) ib = i;
        }
        if (ib < 0) { fprintf(stderr, "[drilltest] no black?\n"); fail++; continue; }
        Vec3 pk = S.world.pocket[1];                 /* a top corner */
        S.balls[ib].on = 1;
        S.balls[ib].pos = v3(pk.x - 0.16f, S.tab.R, pk.z + 0.16f);
        S.balls[ib].vel = v3(0,0,0); S.balls[ib].w = v3(0,0,0);
        S.balls[0].on = 1;
        S.balls[0].pos = v3(pk.x - 0.62f, S.tab.R, pk.z + 0.62f);
        S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);

        Vec3 aim = v3(pk.x - S.balls[0].pos.x, 0, pk.z - S.balls[0].pos.z);
        float l = sqrtf(aim.x*aim.x + aim.z*aim.z);
        aim.x /= l; aim.z /= l;

        begin_shot();
        cue_phys_strike(&S.world, &S.balls[0], aim, 2.6f, 0.0f, 0.0f);
        uint32_t ev = 0;
        for (int it = 0; it < 3000; it++)
            if (!cue_phys_step(&S.world, S.balls, S.nballs, 1.0f/120.0f, &ev)) break;

        int black_down = !S.balls[ib].on;
        resolve_shot();
        int finished = (S.drill_done && S.drill_won);
        int good = black_down && (finished == CASE[c].expect);
        if (!good) fail++;
        fprintf(stderr, "[drilltest] %-38s black %s, challenge %s   %s\n",
                CASE[c].what, black_down ? "potted" : "MISSED",
                finished ? "complete" : "still running",
                good ? "ok" : "FAIL");
    }
    fprintf(stderr, "[drilltest] %s\n", fail ? "FAILED" : "PASSED");
}

/* ---- the callbacks ------------------------------------------------------ */

/* Test hook: park the cue ball at a chosen table-space spot after the rack, so
 * a situation that is otherwise a matter of luck — the white tight under a
 * cushion, a ball parked right behind it — can be staged for a screenshot or a
 * regression run. CUEVR_CUEBALL="x,z" in metres. No effect unless set. */
static void stage_cue_ball(void) {
    const char *e = getenv("CUEVR_CUEBALL");
    if (!e) return;
    float x = 0.0f, z = 0.0f;
    if (sscanf(e, "%f,%f", &x, &z) != 2) return;
    S.balls[0].pos = v3(x, S.tab.R, z);
    S.balls[0].on = 1;
    S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);
    LOGI("[cuevr] CUEVR_CUEBALL staged white at %.3f, %.3f", (double)x, (double)z);
}

static int app_gl_init(void *u) {
    (void)u;
    memset(&S, 0, sizeof S);
    S.rng = 0x1234567u;
    S.persona = 3;
    /* Levelling comes first — see ST_SETUP. */
    S.state = ST_SETUP;
    S.setup.active = 1;
    S.levelled = 0;
    S.menu_sel = 0;
    S.opp = OPP_CPU;
    S.body_idx = -1;             /* the body that suits the table, until told otherwise */
    cue_table_init(&S.tab, CUE_GAME_UK8);
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    stage_cue_ball();
    /* The planner simulates every candidate at its own 8.5 m/s and hands back a
     * FRACTION of full power; we multiply that by 12.0, because a real stroke can
     * be swung harder than a slider can be dragged. Unless it is told, every CPU
     * shot lands 41% harder than the one that was planned — worth a highest break
     * of 22 instead of 67 over 40 measured frames. */
    cue_ai_set_max_speed(MAX_STRIKE_SPEED);

    /* Before init: it decides which shader gets compiled. */
    cuevr_render_set_multiview(mote_xr_multiview());
    if (cuevr_render_init(&S.tab, &S.world, mote_xr_target_is_srgb()) != 0) return -1;
    /* Without this every ball icon and avatar is clipped away at x >= 128. */
    cue_render_icon_target(CUEVR_HUD_W, CUEVR_HUD_H);
    cuevr_audio_open();
    cuevr_setup_init(&S.setup, 0.0f);
    cuevr_cue_init(&S.cue);

    /* Whatever the player set last time. Height and rest especially: they
     * matched the cloth to a real surface and made a bridge that suits them,
     * and being asked to do both again every session is the kind of small
     * insult that stops people playing. */
    {
        CueVrPrefs pr;
        cuevr_prefs_defaults(&pr);
        pr.table_height = S.setup.place.height;
        pr.rest = S.cue.rest;
        pr.grip = S.cue.grip;
        pr.table_kind = (int)S.tab.kind;
        pr.ballset = S.ballset; pr.persona = S.persona;
        pr.opp = S.opp; pr.cue = S.cue_idx;
        pr.light = S.light_idx; pr.body = S.body_idx;
        /* CLOTH, FRAME AND REF VOICE ARE NOT PRE-SEEDED FROM S, for the same
         * reason the calibration below is not — and they were, which quietly
         * broke every one of their defaults.
         *
         * The rows above hold something meaningful by now (S.opp is set a few
         * lines up, the cue and light are zero and mean it). These three do
         * not: S is zero-initialised, so copying it in here overwrote the
         * shipped default with zero every time. The EBONY frame that was asked
         * for arrived as walnut on a fresh install, the cloth default could
         * never be anything but the first swatch, and the referee shipped
         * silent. Let cuevr_prefs_defaults stand and let the file override it
         * if there is one — which is the whole point of having defaults. */
        cuevr_prefs_load(&pr);

        S.cue.rest = pr.rest; S.cue.grip = pr.grip;
        S.ballset = pr.ballset; S.persona = pr.persona;
        S.cloth_idx = pr.cloth; S.frame_idx = pr.frame;
        S.opp = pr.opp; S.cue_idx = pr.cue;
        S.light_idx = pr.light; S.body_idx = pr.body;
        S.ref_voice = pr.refvoice;
        cuevr_refcall_set_voice(S.ref_voice);
        for (int i = 0; i < 3; i++) {
            S.cal_pos[i] = pr.ctrl_pos[i];
            S.cal_rot[i] = pr.ctrl_rot[i];
        }
        cuevr_render_set_ctrl_cal(S.cal_pos, S.cal_rot);
        S.stats = pr;                      /* the records ride in the same file */
        S.lefty = pr.lefty;
        S.stick_swap = pr.stick_swap; S.inv_slide = pr.inv_slide; S.inv_turn = pr.inv_turn;
        cuevr_cue_left_handed(S.lefty);
        cuevr_setup_left_handed(S.lefty);
        cuevr_setup_sticks(S.stick_swap, S.inv_slide, S.inv_turn);
        cuevr_render_set_cue(S.cue_idx);
        cuevr_render_set_light(S.light_idx);
        cuevr_render_set_body(S.body_idx);
        S.setup.place.height = pr.table_height;
        S.pref_height = pr.table_height;
        for (int i = 0; i < MENU_N; i++)
            if ((int)MENU[i].kind == pr.table_kind) S.menu_sel = i;
        cue_render_set_ball_set(S.ballset);
        S.cue_spots = pr.cue_spots;
        S.prac_respot = pr.prac_respot;
        S.surround = pr.surround;
#if CUEVR_TUNE_POCKETS
        S.cut_cr = pr.cut_cr; S.cut_cs = pr.cut_cs;
        S.cut_mr = pr.cut_mr; S.cut_ms = pr.cut_ms;
        cue_render_set_pocket_cut(S.cut_cr/100.0f, S.cut_cs/1000.0f,
                                  S.cut_mr/100.0f, S.cut_ms/1000.0f);
#else
        /* The shipped shape wins. A saved file from a tuning build must not
         * quietly give one player a different pocket from everybody else. */
        cue_render_get_pocket_cut(NULL, NULL, NULL, NULL);
#endif
        memcpy(S.mini_best, pr.mini_best, sizeof S.mini_best);
        /* The career lives beside the preferences, in its own file: it is a
         * different size and shape of thing and a hundred fixture lines have no
         * business in a settings file. */
        {
            /* Beside the preferences, wherever those really are. This used to
             * derive from CUEVR_PREFS_DIR, which is a host-only convenience and
             * unset inside an APK — so on the headset the career was being
             * written to "./cuevr_career.txt" against a read-only working
             * directory and never saved at all. */
            snprintf(S.car_path, sizeof S.car_path, "%s", cuevr_prefs_path());
            char *sl = strrchr(S.car_path, '/');
            if (sl) sl[1] = 0; else S.car_path[0] = 0;
            strncat(S.car_path, "cuevr_career.txt",
                    sizeof S.car_path - strlen(S.car_path) - 1);
            LOGI("[cuevr] career file: %s", S.car_path);
            cuevr_career_load(&S.career, S.car_path);

            /* And the drills, in the same place and for the same reason. */
            snprintf(S.drill_path, sizeof S.drill_path, "%s", cuevr_prefs_path());
            char *ds = strrchr(S.drill_path, '/');
            if (ds) ds[1] = 0; else S.drill_path[0] = 0;
            strncat(S.drill_path, "cuevr_drills.txt",
                    sizeof S.drill_path - strlen(S.drill_path) - 1);
            cuevr_drills_load(&S.drills, S.drill_path);
            S.drill = -1;
            S.edit_ball = -1;
        }
        cuevr_render_set_surround(S.surround);
        mote_xr_show_passthrough(S.surround == 0);
        cue_render_set_cue_spots(S.cue_spots);
        /* Build the table the player last chose, right now, before the
         * levelling screen shows it. The table used to be racked with whatever
         * cue_table_init leaves behind and only re-dressed when the main menu
         * opened, so the first thing anyone saw was a green pub table that
         * changed colour, size and furniture the moment they confirmed the
         * height. Nothing was wrong; it just looked as though something had
         * been. Level the table you are going to play on. */
        menu_preview();
    }

    S.hud_dirty = 1;
    return 0;
}

/* ---- the opponent thinks on its own thread -------------------------------- *
 *
 * The planner simulates up to 160 candidate shots with the real engine, each one
 * run to a true rest. Measured over real positions that is 95 ms of work for an
 * average shot and 434 ms for a hard one, on a desktop — and it was being done
 * on the RENDER thread, ten sims at a time, so a single frame could spend 172 ms
 * inside cue_ai_plan_tick(). A frame at 72 Hz is 13.9 ms. In a headset that is
 * not a dropped frame, it is the world lurching, and it happened every time the
 * opponent got to the table.
 *
 * Ticking it more finely does not fix it — the work is real and it has to happen
 * before the CPU can play. So it happens somewhere else. The render loop polls a
 * flag and never touches the planner.
 *
 * What makes this safe is that nothing moves while it runs: the balls are at
 * rest, the rules are settled, and ST_THINK's only other job is letting you
 * nudge the TABLE around, which the planner never reads. The one shared piece of
 * mutable state in the physics is the substep, and the planner now integrates at
 * exactly the live rate, so setting it is a no-op. Every other cue_ai entry point
 * (place, pushout) is called from states this thread cannot be alive in. */
static int ai_worker(void *unused) {
    (void)unused;
    while (!cue_ai_plan_tick()) { }
    S.ai_done = 1;
    return 0;
}

/* Start thinking. Replaces cue_ai_plan_start at every call site so there is no
 * way to start a plan that stays on the render thread by accident. */
static void think_start(void) {
    /* A SNOOKER PLAYER WHO CANNOT CATCH UP SHAKES HANDS.
     *
     * This is the only place it can be asked, because it is the only place the
     * CPU begins to plan a shot. It used to be asked in hand_over() — which a
     * RACK goes through and an ordinary shot does not, since resolve_shot ends
     * by routing the turn itself. So the opponent could concede on the break
     * and at no other moment in the frame, which is to say never: a player is
     * not hopelessly behind before a ball has been struck.
     *
     * The caller has already set ST_THINK by the time this runs; enter_over()
     * sets ST_OVER over the top of it, so conceding wins the race by being
     * second. */
    if (cue_rules_should_concede(&S.rules, 1)) {
        cue_rules_concede(&S.rules, 1);
        snprintf(S.msg, sizeof S.msg, "OPPONENT CONCEDES");
        S.msg_time = 4.0f;
        enter_over();
        return;
    }
    if (S.ai_th) { SDL_WaitThread(S.ai_th, NULL); S.ai_th = NULL; }
    S.ai_done = 0;
    cue_ai_plan_start(&S.world, &S.tab, &S.rules, S.balls, S.nballs,
                      &CUE_PERSONAS[S.persona], &S.rng);
    S.ai_th = SDL_CreateThread(ai_worker, "cuevr-ai", NULL);
    if (!S.ai_th) {
        /* No thread: fall back to thinking here. A hitch is better than a game
         * that never takes its shot. */
        while (!cue_ai_plan_tick()) { }
        S.ai_done = 1;
    }
}

/* The planner reads the ball array, so ANY path that disturbs it — a re-rack, an
 * undo, quitting to the menu — has to wait for the thread first. */
static void think_join(void) {
    if (!S.ai_th) return;
    SDL_WaitThread(S.ai_th, NULL);
    S.ai_th = NULL;
    S.ai_done = 0;
}

/* Where the cue ball is, in the room. */
static MoteVrV3 cue_ball_room(void) {
    return cuevr_table_to_room(&S.setup.place, S.balls[0].pos);
}

MoteVrV3 cuevr_app_cue_ball_room(void) { return cue_ball_room(); }

static void app_update(void *u, const MoteVrTracking *t) {
    (void)u;
    float dt = t->dt > 0.0f && t->dt < 0.25f ? t->dt : 1.0f / 72.0f;
    S.dbg_frame++;

    /* Put the table somewhere real, the first time we know where the player is.
     *
     * The height comes off the FLOOR: the app asks the OpenXR host for a
     * floor-relative world (STAGE), so y = 0 is the actual floor of the room
     * and a table stands 0.85 m above it, like a table. Where a runtime cannot
     * give us that, LOCAL's origin is the HEADSET — and "0.85 m up" in LOCAL is
     * 0.85 m above the player's EYES, which is the ceiling, with the HUD
     * hanging off the table and going up there with it. So the fallback derives
     * the floor from eye level instead.
     *
     * Left-right and facing always come from the head: centred in front, length
     * running away, so the player starts at the baulk end. */
    if (!S.sited) {
        MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
        fwd.y = 0.0f;
        fwd = mv3_len(fwd) > 1e-3f ? mv3_norm(fwd) : mv3(0, 0, -1);
        /* The height they set last time, if they have ever set one — that is the
         * whole point of remembering it. Otherwise a match table's height above
         * the real floor, or eye level less three quarters of a metre where the
         * runtime cannot tell us where the floor is. */
        float dflt = mote_xr_floor_relative() ? CUEVR_TABLE_HEIGHT
                                              : t->head.p.y - 0.75f;
        S.setup.place.height = S.pref_height > 0.25f ? S.pref_height : dflt;
        S.setup.place.pos = mv3_add(t->head.p,
                                    mv3_scale(fwd, S.tab.half_len + 0.35f));
        S.setup.place.pos.y = S.setup.place.height;
        S.setup.place.yaw = atan2f(fwd.z, fwd.x);   /* +X, the length, points away */
        S.setup.last_height = S.setup.place.height;
        S.sited = 1;
        S.hud_dirty = 1;
    }
    if (S.msg_time > 0.0f) { S.msg_time -= dt; if (S.msg_time <= 0.0f) S.hud_dirty = 1; }

    /* The shot clock. It runs while the striker is at the table — thinking,
     * walking round it, lining up, answering a foul — and stops the instant the
     * cue makes contact, because balls rolling is not time anybody spent on the
     * shot. Paused, and in the menus, it does not run at all: choosing a cloth
     * mid-frame should not read as a four-minute deliberation. */
    if (S.state == ST_AIM || S.state == ST_PLACE || S.state == ST_THINK ||
        S.state == ST_CPUCUE || S.state == ST_DECIDE)
        S.shot_clock += dt;

    toast_tick(dt);

    /* The challenge clock. It runs from the first strike to the last ball —
     * including while the balls are rolling, because the time a shot takes to
     * settle is part of the shot you chose. It does not run while paused or in
     * a menu: the challenge is your cueing, not your reading speed. */
    if (S.mini && !S.mini_done && S.have_snap &&
        (S.state == ST_AIM || S.state == ST_ROLL || S.state == ST_PLACE))
        S.mini_t += dt;
    /* A drill's clock runs on the same terms: from the first strike, through
     * the roll-out, and not while a menu is open — and only if the challenge
     * asked for a clock at all. Running one on every challenge made a race out
     * of "pot the black off its spot", which is not a race, and left a
     * stopwatch on the board afterwards. */
    if (S.drill >= 0 && S.drills.slot[S.drill].timed &&
        !S.drill_done && S.have_snap &&
        (S.state == ST_AIM || S.state == ST_ROLL || S.state == ST_PLACE))
        S.drill_t += dt;

    /* Online runs whether or not anyone is looking at the lobby. */
    if (S.opp == OPP_ONLINE) cuevr_net_task();

    /* MENU: tap for the options, hold to go straight to placing the table.
     * The hold shortcut stays because it was the only way in before and muscle
     * memory is worth keeping; the tap is there because "hold a button for a
     * second to find out what your options are" is not a discoverable design. */
    if (t->hand[MOTE_VR_LEFT].menu) {
        S.menu_hold += dt;
        if (S.menu_hold > 1.0f && S.state != ST_SETUP && S.state != ST_MENU
            && S.state != ST_LOBBY) {
            S.setup.active = 1;
            S.state = ST_SETUP;
            S.menu_hold = -2.0f;                    /* don't retrigger on release */
            S.hud_dirty = 1;
        }
    } else {
        if (S.menu_hold > 0.02f && S.menu_hold <= 1.0f) {
            /* A tap. */
            if (S.state == ST_PAUSE) {
                unpause();
            } else if (S.state == ST_LAYOUT) {
                /* The editor has its own options and MENU is what shows them.
                 * The pause list is not the right one here — half of it acts on
                 * a frame that is not being played. */
                S.lay_menu = !S.lay_menu;
                S.ptr_latch = 1;
                S.hud_dirty = 1;
            } else if (S.state != ST_SETUP && S.state != ST_MENU && S.state != ST_LOBBY) {
                S.pause_sel = PS_RESUME;
                S.pause_from = S.state;
                S.state = ST_PAUSE;
                S.hud_dirty = 1;
            }
        }
        if (S.menu_hold > -1.0f) S.menu_hold = 0.0f;
    }

    /* ---- the pointer ---------------------------------------------------- *
     * A ray out of the right controller onto the HUD panel, exactly as every
     * Quest menu works. The AIM pose is the runtime's own answer to which way
     * the controller points (-Z along the ray), so there is nothing to
     * calibrate and nothing to argue about.
     *
     * The panel's pose is computed at the END of the frame, so this reads last
     * frame's — one frame of lag on a thing a hand moves slowly, which nobody
     * can see, and it avoids ordering the whole update around the HUD. */
    {
        S.ptr_ok = 0;
        S.scene.ptr_visible = 0;
        /* ONLY WHERE THERE IS SOMETHING TO POINT AT. The panel in play is the
         * scoreboard: it is the same HUD quad, so the ray found it and drew a
         * laser across the table at a board with nothing on it to press. A
         * pointer that appears when there is nothing to point at is not a
         * pointer, it is a laser lying on the cloth. */
        int pointing = (S.state == ST_MENU || S.state == ST_PAUSE ||
                        S.state == ST_APPEAR || S.state == ST_STATS ||
                        S.state == ST_CLOTH || S.state == ST_DRILLS || S.state == ST_DRILLSET || S.state == ST_DRILLS ||
                        S.state == ST_LOBBY || S.state == ST_DECIDE ||
                        S.state == ST_CONTROLS || S.state == ST_OVER ||
                        S.state == ST_CARSETUP || S.state == ST_CAREER ||
                        S.state == ST_CARTABLE || S.state == ST_CARACH ||
                        S.state == ST_POCKETS);
        /* The pointer lives in the hand that holds the BUTT, which is the left
         * one for a left-hander. Hard-coding the right would have put the laser
         * in their bridge hand — the one lying on the cloth. */
        const MoteVrHand *rh = &t->hand[S.lefty ? MOTE_VR_LEFT : MOTE_VR_RIGHT];
        if (pointing && rh->aim_tracked && S.scene.hud_visible && S.scene.hud_w > 0.01f) {
            MoteVrV3 o = rh->aim.p;
            MoteVrV3 d = mq_rot(rh->aim.q, mv3(0, 0, -1));
            MoteVrV3 px = mq_rot(S.scene.hud_rot, mv3(1, 0, 0));
            MoteVrV3 py = mq_rot(S.scene.hud_rot, mv3(0, 1, 0));
            MoteVrV3 pn = mq_rot(S.scene.hud_rot, mv3(0, 0, 1));
            float dn = mv3_dot(d, pn);
            if (fabsf(dn) > 1.0e-4f) {
                float k = mv3_dot(mv3_sub(S.scene.hud_pos, o), pn) / dn;
                if (k > 0.02f && k < 8.0f) {
                    MoteVrV3 hit = mv3_sub(mv3_add(o, mv3_scale(d, k)), S.scene.hud_pos);
                    float hw_m = S.scene.hud_w;
                    int rows = S.scene.hud_rows ? S.scene.hud_rows : CUEVR_HUD_LH;
                    float hh_m = hw_m * (float)rows / (float)HW;
                    float u = mv3_dot(hit, px) / hw_m + 0.5f;   /* 0..1 left→right */
                    float v = 0.5f - mv3_dot(hit, py) / hh_m;   /* 0..1 top→bottom */
                    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                        S.ptr_x = u * (float)HW;
                        S.ptr_y = v * (float)rows;
                        S.ptr_ok = 1;
                    }
                    /* The beam is drawn from the SAME intersection, so what you
                     * see and what the menu reads can never disagree. It stops
                     * at the panel when it lands on it and runs on past when it
                     * does not, which is how you find the panel again. */
                    S.scene.ptr_visible = 1;
                    S.scene.ptr_from = o;
                    S.scene.ptr_hit = S.ptr_ok;
                    S.scene.ptr_to = S.ptr_ok ? mv3_add(o, mv3_scale(d, k))
                                              : mv3_add(o, mv3_scale(d, 2.5f));
                }
            }
        }
    }

    /* THE CUE FOLLOWS YOUR HANDS, ALWAYS. It is a rigid object you are
     * holding, so its pose is a function of where your hands are and nothing
     * else — not of whose turn it is.
     *
     * This used to run only inside ST_AIM, which was invisible while the cue
     * was only DRAWN in ST_AIM: the two wrongs cancelled. Drawing it the rest
     * of the time exposed it immediately — the cue hung in the air wherever it
     * had been when you struck, while your hands moved out from under it. The
     * side triggers are part of the same call, so they were dead outside
     * aiming too.
     *
     * The stroke it detects is only ACTED on in ST_AIM. You can pull the
     * trigger and cue through while the balls are still running, and it does
     * what it does on a real table: nothing at all. */
    {
        int in_menu = (S.state == ST_MENU || S.state == ST_SETUP ||
                       S.state == ST_LOBBY ||
                       S.state == ST_STATS || S.state == ST_CONTROLS);
        if (!in_menu) {
            /* How steeply the cue is FORCED to sit for this aim, worked out
             * BEFORE the cue is read because the cue is what consumes it. It is
             * a table-space question (which cushion, which balls) and the cue
             * works in room space, so it is answered here and handed down. The
             * aim it uses is last frame's — the two are mutually dependent and
             * one frame of lag at 72 Hz is invisible and self-correcting.
             *
             * It has to stay immediately before the update: moving the update
             * out of ST_AIM and leaving this behind would have put a second
             * frame of lag on it, silently. */
            MoteVrV3 td = cuevr_room_dir_to_table(&S.setup.place, S.cue.aim_dir);
            float aim_t = atan2f(td.z, td.x);
            Vec3 cb = S.balls[0].pos; float Rr = S.tab.R;
            Vec3 tp = v3(cb.x - cosf(aim_t)*Rr, Rr*(1.0f + S.cue.tip_vert),
                         cb.z - sinf(aim_t)*Rr);
            S.cue.min_elev = getenv("CUEVR_NOELEV") ? 0.0f
                : cue_table_min_elev(&S.tab, S.balls, S.nballs, tp, aim_t);

            cuevr_cue_update(&S.cue, t, &S.setup.place, cue_ball_room(),
                             S.tab.R, &S.idle_shot);
        }
        else
            memset(&S.idle_shot, 0, sizeof S.idle_shot);
    }

    /* THEY LEFT — from wherever we happen to be standing.
     *
     * This lived inside ST_THINK, so it only ever fired if the link dropped
     * while you were WAITING for them. Lose it on your own turn — the far more
     * likely moment, since that is when the other player is the one with
     * nothing to do — and nothing said a word: you aimed and played into a dead
     * socket, for ever. Anywhere the frame is live counts. */
    if (S.opp == OPP_ONLINE && cuevr_net_state() == CUEVR_NET_LOST &&
        !S.rules.frame_over &&
        (S.state == ST_AIM || S.state == ST_ROLL || S.state == ST_THINK ||
         S.state == ST_PLACE || S.state == ST_DECIDE || S.state == ST_PAUSE)) {
        think_join();
        /* WHY it went, not just that it did. link_net knows — peer closed the
         * pipe, a send error, a backlog — and without it a dropped match is a
         * mystery every single time. */
        LOGI("[cuevr] link lost: %s", cuevr_net_info());
        fprintf(stderr, "[cuevr] link lost: %s\n", cuevr_net_info());
        snprintf(S.msg, sizeof S.msg, "OPPONENT LEFT");
        S.msg_time = 4.0f;
        S.rules.frame_over = 1;
        S.rules.winner = S.net_me;
        enter_over();
    }

    /* THE HOST'S TABLE WINS. Taken only when nothing is moving: applied
     * mid-roll it would teleport balls out from under a shot that is still
     * being simulated, which is a worse desync than the one it is here to
     * mend. The host never applies its own. */
    if (S.opp == OPP_ONLINE && S.net_me != 0 && S.state != ST_ROLL) {
        CueVrNetState st;
        if (cuevr_net_recv_state(&st)) {
            int n = st.n < S.nballs ? st.n : S.nballs;
            for (int i = 0; i < n; i++) {
                S.balls[i].on  = st.on[i];
                S.balls[i].pos = v3(st.x[i], S.tab.R, st.z[i]);
                S.balls[i].vel = v3(0,0,0);
                S.balls[i].w   = v3(0,0,0);
            }
            /* The rules entire, not a chosen few fields. `cpu` is 0 at both
             * ends of an online frame and the table geometry in here is built
             * from the same kind, so there is nothing in the struct that is
             * legitimately local — which is exactly why it can be taken whole
             * and why doing so is safer than a list somebody has to maintain. */
            if (st.rules_len == (uint16_t)sizeof(CueRules))
                memcpy(&S.rules, st.rules, sizeof(CueRules));
            if (getenv("CUEVR_NETDBG"))
                fprintf(stderr, "[netdbg] f%d TAKE state turn=%d on0=%d len=%d "
                        "was %s\n", S.dbg_frame, S.rules.turn, st.on[0],
                        (int)st.rules_len, cuevr_app_state_name());
            /* AND THE STATE MACHINE MOVES WITH THE TURN. Taking st.turn as a
             * number and leaving S.state where it was is how a corrected turn
             * becomes a dead frame: ST_THINK is the only state that reads an
             * incoming shot, so an end left thinking while the table is its own
             * waits for a shot nobody will play, and an end left aiming while
             * the table is theirs is deaf to the shot they do play. Only from a
             * settled state — mid-roll, placing or deciding are all mid-flow and
             * have their own routing at the end of them. */
            if (S.rules.frame_over) enter_over();
            else if (S.state == ST_AIM || S.state == ST_THINK) {
                int want = (S.rules.turn == S.net_me) ? ST_AIM : ST_THINK;
                if (S.state != want) { arm_shot(); S.state = want; }
            }
            S.hud_dirty = 1;
        }
    }

    /* A choice from the far end: apply it exactly as if we had made it.
     *
     * NOT while our own copy of the shot is still rolling. Every branch below is
     * conditional on the rules state the shot is about to produce — a pending
     * decision, a push-out offer — and none of those exist yet mid-roll, so a
     * call that overtook the local simulation fell through every branch and was
     * thrown away. The far end then waited for a player who had silently never
     * been asked. It stays latched until there is something for it to answer. */
    if (S.opp == OPP_ONLINE && S.state != ST_ROLL) {
        CueVrNetCall c;
        while (cuevr_net_recv_call(&c)) {
            if (c.code == CUEVR_NET_CONCEDE) {
                think_join();
                cue_rules_concede(&S.rules, c.who);
                snprintf(S.msg, sizeof S.msg, "FRAME CONCEDED");
                S.msg_time = 3.0f;
                enter_over();
            } else if (S.rules.pushout_offer) {
                S.rules.is_pushout = (c.code == CUE_DEC_PLAY) ? 1 : 0;
                S.rules.pushout_offer = 0;
                S.rules.pushout_avail = 0;
                arm_shot(); hand_over();
            } else if (S.rules.decision == CUE_DEC_PENDING) {
                if (c.code == CUE_DEC_REPLAY) snap_restore_balls();
                cue_rules_apply_decision(&S.rules, c.code);
                if (S.rules.free_ball) cuevr_refcall_say(CUEVR_SAY_FREE_BALL);
                arm_shot(); hand_over();
            }
            /* A decision moves the balls (a replay puts them all back) and moves
             * the turn, so the host says where everything is afterwards — the
             * same guarantee a shot gets. */
            net_push_state();
            S.hud_dirty = 1;
        }
    }

    /* Ours, out to them, EVERY frame.
     *
     * This went at every fourth — nominally 18 Hz — on the reasoning that a
     * stick moves at human speed. It does, but a cue tip travels the length of
     * a stroke in about a tenth of a second, and four frames of hold at 72 Hz is
     * a visible step every time: the opponent's cue arrived in jerks. 25 bytes a
     * frame is 1.8 KB/s, which is less than a hundredth of what the state packet
     * costs over a frame of snooker. Only while we are actually holding it. */
    if (S.opp == OPP_ONLINE && cuevr_net_state() == CUEVR_NET_LIVE &&
        S.cue.tracked) {
        MoteVrV3 tp = cuevr_room_to_table(&S.setup.place, S.cue.tip);
        MoteVrV3 bp = cuevr_room_to_table(&S.setup.place, S.cue.butt);
        CueVrNetPose np = { tp.x, tp.y, tp.z, bp.x, bp.y, bp.z };
        cuevr_net_send_pose(&np);
    }

    /* A SHOT IN FLIGHT KEEPS RUNNING behind a menu. Opening one used to stop
     * the balls dead, because the stepping lived inside ST_ROLL and the state
     * machine was somewhere else — you came back to a shot that had been
     * standing still while you chose a cloth. The balls settle whether or not
     * anybody is watching, and ST_ROLL resolves it the moment you return. */
    if (S.pause_from == ST_ROLL &&
        (S.state == ST_PAUSE || S.state == ST_APPEAR ||
         S.state == ST_CLOTH ||
         S.state == ST_CONTROLS || S.state == ST_STATS))
        roll_step(dt);

    switch (S.state) {
    case ST_MENU: {
        /* POINTED AT, not scrolled to. The sticks belong to the table on every
         * screen in this game, so a menu cannot borrow one: you point at a row
         * and pull the trigger, the way everything else on the headset works.
         *
         * A row with a value has three zones — the two chevrons change it, the
         * middle acts on it — which is why hud_opt draws those chevrons where
         * it does and why ptr_zone reads the same numbers. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            int hov = ptr_row_at(12, 8, MR_N);
            if (hov >= 0 && hov != S.menu_row) { S.menu_row = hov; S.hud_dirty = 1; }
            int click = ptr_click(t);
            if (hov >= 0 && click) {
                int z = ptr_zone();
                if (z == 0 || S.menu_row == MR_APPEAR || S.menu_row == MR_STATS
                           || S.menu_row == MR_CONTROLS || S.menu_row == MR_START)
                    menu_activate();
                else
                    menu_change(z);
            }
        }
        /* A still works, on whatever the pointer last picked out — a button is
         * a kinder thing to find than a ray when you are not looking at the
         * panel. */
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1;
            menu_activate();
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }

    case ST_LOBBY: {
        /* Pairing completes on its own schedule, so this is checked every frame
         * rather than behind the input latch — otherwise the match only starts
         * when you happen to touch a stick. */
        if (S.lb_screen == LB_WAIT && cuevr_net_state() == CUEVR_NET_LIVE) {
            S.net_me = cuevr_net_me();
            /* THE HOST'S GAME IS THE GAME. Both ends used to call start_frame()
             * on their OWN menu selection, so two players who had not happened
             * to pick the same one racked different tables and every shot after
             * that was nonsense. Wait for their hello before racking; the host
             * needs nobody's permission and racks at once. */
            if (S.net_me == 0) {
                start_frame(MENU[S.menu_sel].kind);
            } else {
                CueVrNetHello ph;
                if (!cuevr_net_peer(&ph)) break;   /* not yet — ask again next frame */
                for (int i = 0; i < MENU_N; i++)
                    if ((int)MENU[i].kind == ph.kind) S.menu_sel = i;
                cuevr_render_set_opp_cue(ph.cue_idx);
                /* THEIR toss, not ours. Both ends drew one; only the host's
                 * counts, exactly as with the game kind. */
                S.break_first = ph.first ? 1 : 0;
                start_frame((CueGameKind)ph.kind);
                /* AND THEIR MATCH LENGTH. start_frame() takes it from OUR menu,
                 * so a host on best of 5 and a joiner on best of 1 played the
                 * same frames and disagreed about when it was over: one went
                 * back to the menu, the other racked and waited for a shot from
                 * somebody who had left. After start_frame, which sets it. */
                if (ph.best_of > 0) S.rules.best_of = ph.best_of;
            }
            LOGI("[cuevr] online: seat %d playing %s, %s breaks", S.net_me,
                 MENU[S.menu_sel].name, S.break_first ? "joiner" : "host");
            snprintf(S.msg, sizeof S.msg,
                     S.rules.turn == S.net_me ? "YOU BREAK" : "THEY BREAK");
            S.msg_time = 3.0f;
            hand_over();
            break;
        }
        if (S.lb_screen == LB_BROWSE && cuevr_net_browse_done()) S.hud_dirty = 1;

        /* Either stick, so it does not matter which hand — the sum is the same
         * whichever way round the player is, and writing it handed would only
         * suggest it mattered. */
        {
            /* POINTED AT, like every other list. This was the one screen still
             * driven by a stick — exactly the inconsistency that makes an
             * interface feel improvised: you point at everything else, arrive
             * here, and nothing moves. The rows the pointer tests are the rows
             * each screen draws, at the same coordinates, so they cannot drift.
             * LB_CODE keeps its stick: spinning a letter is a dial, not a list,
             * and there is nothing on screen to point at. */
            int hov = -1;
            if (S.lb_screen == LB_TRANSPORT)   hov = ptr_row_at(24, 11, TR_N);
            else if (S.lb_screen == LB_ACTION) hov = ptr_row_at(21, 10,
                                                    (S.lb_tr == TR_LAN) ? 2 : 4);
            else if (S.lb_screen == LB_BROWSE && cuevr_net_browse_done()) {
                int nb = cuevr_net_browse_count();
                hov = ptr_row_at(13, 9, nb < 5 ? nb : 5);
            }
            if (hov >= 0 && hov != S.lb_sel) { S.lb_sel = hov; S.hud_dirty = 1; }
            if (hov >= 0 && ptr_click(t)) S.lb_click = 1;
        }
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        float ly = (S.lb_screen == LB_CODE)
                 ? t->hand[MOTE_VR_LEFT].stick_y + t->hand[MOTE_VR_RIGHT].stick_y : 0.0f;
        float lx = (S.lb_screen == LB_CODE)
                 ? t->hand[MOTE_VR_LEFT].stick_x + t->hand[MOTE_VR_RIGHT].stick_x : 0.0f;
        int a = t->hand[MOTE_VR_RIGHT].btn_lower || S.lb_click;
        int bb = t->hand[MOTE_VR_RIGHT].btn_upper;

        /* Edge-triggered, and it has to be written this way round. The first
         * version cleared the latch on a neutral frame and then immediately
         * re-took it in the same frame, so by the time a button was actually down
         * the latch was already held and every press was swallowed. Two peers sat
         * in the lobby forever and nothing was logged, because nothing ran. */
        int any = (fabsf(ly) > 0.4f) || (fabsf(lx) > 0.4f) || a || bb;
        if (!any)                      { S.lb_latch = 0; break; }
        if (S.lb_latch && !S.lb_click) break;
        S.lb_latch = 1;
        S.lb_click = 0;                /* a click is one event, not a held state */

        if (bb) {
            /* Back, one screen at a time; out of the lobby means out of online. */
            if (S.lb_screen == LB_WAIT || S.lb_screen == LB_CODE || S.lb_screen == LB_BROWSE) {
                cuevr_net_stop();
                S.lb_screen = LB_ACTION; S.lb_sel = 0;
            } else if (S.lb_screen == LB_ACTION) {
                S.lb_screen = LB_TRANSPORT; S.lb_sel = 0;
            } else {
                cuevr_net_stop();
                S.state = ST_MENU;
            }
            S.hud_dirty = 1;
            break;
        }

        if (S.lb_screen == LB_TRANSPORT) {
            if (fabsf(ly) > 0.4f) S.lb_sel = (S.lb_sel + (ly < 0 ? 1 : TR_N - 1)) % TR_N;
            if (a) { S.lb_tr = S.lb_sel; S.lb_screen = LB_ACTION; S.lb_sel = 0; }
            S.hud_dirty = 1;
            break;
        }
        if (S.lb_screen == LB_ACTION) {
            int n = (S.lb_tr == TR_LAN) ? 2 : 4;
            if (fabsf(ly) > 0.4f) S.lb_sel = (S.lb_sel + (ly < 0 ? 1 : n - 1)) % n;
            if (a) {
                S.lb_act = (S.lb_tr == TR_LAN) ? (S.lb_sel == 0 ? ACT_LANHOST : ACT_LANJOIN)
                         : (S.lb_sel == 0 ? ACT_QUICK : S.lb_sel == 1 ? ACT_HOST
                          : S.lb_sel == 2 ? ACT_JOIN : ACT_BROWSE);
                switch (S.lb_act) {
                case ACT_LANHOST: cuevr_net_lan_host(); S.lb_screen = LB_WAIT; break;
                case ACT_LANJOIN: cuevr_net_lan_join(); S.lb_screen = LB_WAIT; break;
                case ACT_QUICK:   cuevr_net_quick();    S.lb_screen = LB_WAIT; break;
                case ACT_HOST: {
                    char c[CUEVR_CODE_LEN + 1];
                    cuevr_net_make_code(c);
                    cuevr_net_host(c);
                    S.lb_screen = LB_WAIT;
                    break;
                }
                case ACT_JOIN:
                    for (int i = 0; i < CUEVR_CODE_LEN; i++)
                        S.lb_code[i] = CUEVR_CODE_ALPHABET[0];
                    S.lb_code[CUEVR_CODE_LEN] = 0;
                    S.lb_cur = 0;
                    S.lb_screen = LB_CODE;
                    break;
                case ACT_BROWSE:
                    cuevr_net_browse_start();
                    S.lb_sel = 0;
                    S.lb_screen = LB_BROWSE;
                    break;
                }
            }
            S.hud_dirty = 1;
            break;
        }
        if (S.lb_screen == LB_CODE) {
            const char *AL = CUEVR_CODE_ALPHABET;
            int NA = (int)strlen(AL);
            if (lx > 0.4f)  S.lb_cur = (S.lb_cur + 1) % CUEVR_CODE_LEN;
            if (lx < -0.4f) S.lb_cur = (S.lb_cur + CUEVR_CODE_LEN - 1) % CUEVR_CODE_LEN;
            if (fabsf(ly) > 0.4f) {
                const char *pp = strchr(AL, S.lb_code[S.lb_cur]);
                int i = pp ? (int)(pp - AL) : 0;
                i += (ly > 0.0f) ? 1 : (NA - 1);
                S.lb_code[S.lb_cur] = AL[i % NA];
            }
            if (a) { cuevr_net_join(S.lb_code); S.lb_screen = LB_WAIT; }
            S.hud_dirty = 1;
            break;
        }
        if (S.lb_screen == LB_WAIT) {
            /* handled below, outside the input latch */
        }
        if (S.lb_screen == LB_BROWSE) {
            int n = cuevr_net_browse_count();
            if (n > 0 && fabsf(ly) > 0.4f) S.lb_sel = (S.lb_sel + (ly < 0 ? 1 : n - 1)) % n;
            if (a && n > 0) {
                cuevr_net_join(cuevr_net_browse_code(S.lb_sel));
                S.lb_screen = LB_WAIT;
            }
            S.hud_dirty = 1;
            break;
        }
        break;
    }

    case ST_PAUSE: {
        /* Pointed at, like the rest. The rows are drawn at 14 + i*9. */
        {
            PsRow row[16];
            int nrow = pause_rows(row, 16);
            if (S.pause_sel >= nrow) S.pause_sel = 0;
            int hov = ptr_row_at(14, 9, nrow);
            if (hov >= 0 && hov != S.pause_sel) { S.pause_sel = hov; S.hud_dirty = 1; }
            if (hov >= 0 && ptr_click(t)) S.pause_click = 1;
        }
        /* The sticks belong to the table here too. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        int a = t->hand[MOTE_VR_RIGHT].btn_lower;
        if (!a && !S.pause_click) S.pause_latch = 0;
        else if (!S.pause_latch || S.pause_click) {
            S.pause_latch = 1;
            S.pause_click = 0;
            {
                PsRow row2[16];
                int n2 = pause_rows(row2, 16);
                /* The ROW'S OWN id, not its index. The list is built from
                 * what applies, so index 3 is a different action depending on
                 * whether you are playing snooker, in practice, or holding the
                 * ball. */
                int sel = (S.pause_sel < n2) ? row2[S.pause_sel].id : PS_RESUME;
                switch (sel) {
                case PS_RESUME:
                    unpause();
                    break;
                case PS_UNDO:
                    /* Practice only, and only if there is a shot to take back. */
                    if (S.opp == OPP_PRACTICE && snap_restore()) {
                        snprintf(S.msg, sizeof S.msg, "SHOT UNDONE");
                        S.msg_time = 2.0f;
                        S.state = ST_AIM;
                    }
                    break;
                case PS_AGAIN:
                    /* The position back as it was, the clock at zero. Which is
                     * the whole of what "let me try that again" means, and the
                     * commonest thing anybody wants from a challenge. */
                    if (S.drill >= 0)   { unpause(); drill_again(); }
                    else if (S.mini)    { unpause(); mini_start(); }
                    break;
                case PS_ENDCHAL:
                    if (S.drill >= 0) drill_stop();
                    else if (S.mini)  mini_stop();
                    unpause();
                    break;
                case PS_MINI:
                    if (S.mini) mini_stop();
                    else        mini_start();
                    break;
                case PS_DRILLS:
                    if (S.drill >= 0) drill_stop();
                    else {
                        S.state = ST_DRILLS;
                        S.ptr_latch = 1;
                        S.hud_dirty = 1;
                    }
                    break;
                case PS_RESPOT:
                    /* Toggles in place — you are looking at the table it acts
                     * on, so closing the menu to see the effect is backwards. */
                    S.prac_respot = !S.prac_respot;
                    S.hud_dirty = 1;
                    break;
                case PS_RERACK:
                    rerack();
                    hand_over();          /* which puts the ball back in hand */
                    break;
                case PS_PLACE:
                    S.setup.active = 1;
                    S.state = ST_SETUP;
                    break;
                case PS_NOMINATE: {
                    /* Steps through the colours still on the table rather than
                     * all six: nominating a ball that has been potted is not a
                     * choice, it is a mistake waiting to be made. */
                    if (!S.rules.kind || S.rules.target != 1) break;
                    int v = S.rules.nominated;
                    for (int k = 0; k < 6; k++) {
                        v = (v < 2 || v >= 7) ? 2 : v + 1;
                        int id = CUE_ID_YELLOW + (v - 2), on = 0;
                        for (int i = 1; i < S.nballs; i++)
                            if (S.balls[i].on && S.balls[i].id == id) { on = 1; break; }
                        if (on) break;
                    }
                    cue_rules_nominate(&S.rules, v);
                    S.nom_manual = 1;
                    break;
                }
                case PS_FREEBALL: {
                    /* WHICH COLOUR STANDS IN FOR THE BALL ON. That is the
                     * choice a free ball actually is, and the first version
                     * stepped through every ball on the table in id order —
                     * which on a snooker table means fourteen reds before you
                     * reach a colour. The candidates are the colours, plus a
                     * red when you are on a colour and a red is what you would
                     * nominate, and never the ball that is already on. */
                    if (!S.rules.free_ball) break;
                    int cand[8], nc = 0;
                    int on_col = (S.rules.target == 1) ? S.rules.nominated
                               : (S.rules.target == 2) ? S.rules.seq : 0;
                    for (int v = 2; v <= 7; v++) {
                        if (v == on_col) continue;          /* that one IS the ball on */
                        int id = CUE_ID_YELLOW + (v - 2);
                        for (int i = 1; i < S.nballs; i++)
                            if (S.balls[i].on && S.balls[i].id == id) { cand[nc++] = id; break; }
                    }
                    if (S.rules.target != 0)                /* on a colour: a red will do */
                        for (int i = 1; i < S.nballs; i++)
                            if (S.balls[i].on && S.balls[i].id >= 1 && S.balls[i].id <= 15) {
                                cand[nc++] = S.balls[i].id; break;
                            }
                    if (!nc) break;
                    int at = -1;
                    for (int i = 0; i < nc; i++) if (cand[i] == S.rules.free_ball_id) at = i;
                    cue_rules_nominate_free(&S.rules, cand[(at + 1) % nc]);
                    S.nom_manual = 1;
                    S.hud_dirty = 1;
                    break;
                }
                case PS_CONCEDE:
                    /* Snooker only, and it ends the frame there and then — that
                     * is what conceding is. The match tally moves with it. */
                    if (S.rules.kind && !S.rules.frame_over) {
                        think_join();
                        if (S.opp == OPP_ONLINE) {
                            CueVrNetCall c = { CUEVR_NET_CONCEDE, S.net_me };
                            cuevr_net_send_call(&c);
                        }
                        cue_rules_concede(&S.rules, S.opp == OPP_ONLINE ? S.net_me : 0);
                        snprintf(S.msg, sizeof S.msg, "FRAME CONCEDED");
                        S.msg_time = 3.0f;
                        enter_over();
                        net_push_state();
                    }
                    break;
                case PS_PICKUP:
                    if (S.can_repick) {
                        S.state = ST_PLACE;
                        S.place_latch = 1;      /* the press that got here does not drop it */
                        S.hud_dirty = 1;
                    }
                    break;
                case PS_APPEAR:
                    S.btn_latch = 1;      /* the press that opened it is still down */
                    S.appear_from = ST_PAUSE;
                    S.menu_row = AR_CLOTH;
                    S.state = ST_APPEAR;
                    break;
                case PS_CONTROLS:
                    S.btn_latch = 1;
                    S.appear_from = ST_PAUSE;
                    S.menu_row = CR_HAND;
                    S.state = ST_CONTROLS;
                    break;
                case PS_STATS:
                    S.btn_latch = 1;
                    S.appear_from = ST_PAUSE;
                    S.state = ST_STATS;
                    break;
                case PS_QUIT:
                    if (S.opp == OPP_ONLINE) cuevr_net_stop();
                    /* Belt and braces with start_frame: the board must go back
                     * to normal the moment you leave, not on the next rack. */
                    S.mini = S.mini_done = S.mini_beat = 0;
                    /* AND THE CHALLENGE. This cleared the clearance and not the
                     * drill, so leaving a challenge through BACK TO MENU left
                     * S.drill standing — and the drill board wins the HUD over
                     * every screen after it. Start a timed challenge, leave,
                     * play a frame of snooker, and there was still a stopwatch
                     * where the scoreboard should be, with no way to shift it. */
                    S.drill = -1;
                    S.drill_done = S.drill_beat = S.drill_won = 0;
                    S.drill_t = 0.0f;
                    S.in_career = 0;
                    S.state = ST_MENU;
                    S.menu_row = MR_GAME;
                    menu_preview();
                    break;
                default: break;
                }
            }
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_SETUP: {
        int h0 = (int)(S.setup.place.height * 1000.0f);
        if (!cuevr_setup_update(&S.setup, t, cue_ball_room())) {
            /* They have settled on a height — that is the one to remember, and
             * the one the next frame should site itself at. */
            S.pref_height = S.setup.place.height;
            if (!S.levelled) {
                /* The first thing that happens in a session is getting the cloth
                 * onto a real surface, because every shot afterwards depends on
                 * it and nobody thinks to go looking for it behind a menu. Once
                 * it is done, the menu. */
                S.levelled = 1;
                S.state = ST_MENU;
                menu_preview();
            } else {
                hand_over();
            }
            S.hud_dirty = 1;
        }
        if ((int)(S.setup.place.height * 1000.0f) != h0) S.hud_dirty = 1;
        break;
    }

    case ST_AIM: {
        /* The sticks keep working. "Move the table, not yourself" is the whole
         * answer to a twelve-foot table in a small room, and it is no use only
         * during setup — the shot you want has to be brought to you on every
         * visit. Nothing else in aiming uses them. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);

        /* HOLD B TO PLAY IT AGAIN. Practice is for playing the same shot over
         * until it goes, and the take-back was three presses down a pause menu
         * — by which time you have lost the picture of what you just did.
         *
         * Held, not tapped: B is a hair from the trigger hand's grip and a
         * stray press must not silently rewind the table. The bar on the panel
         * says how long is left. */
        if (S.opp == OPP_PRACTICE && S.have_snap) {
            if (t->hand[MOTE_VR_RIGHT].btn_upper) {
                float was = S.undo_hold;
                S.undo_hold += dt;
                if (was < CUEVR_UNDO_HOLD && S.undo_hold >= CUEVR_UNDO_HOLD) {
                    if (snap_restore()) {
                        snprintf(S.msg, sizeof S.msg, "SHOT UNDONE");
                        S.msg_time = 2.0f;
                        mote_xr_haptic(0.5f, 60);
                    }
                    S.undo_hold = 0.0f;
                }
                S.hud_dirty = 1;
            } else if (S.undo_hold > 0.0f) {
                S.undo_hold = 0.0f;
                S.hud_dirty = 1;
            }
        } else S.undo_hold = 0.0f;

        /* Online: you may only strike on your own turn. Everything else about
         * aiming still works, so you can line up while they are playing. */
        int mine = (S.opp != OPP_ONLINE) || (S.rules.turn == S.net_me);

        CueVrShot shot = S.idle_shot;   /* already updated for this frame */
        /* The tip readout moves every frame, but a full software raster and
         * a ~400 KB texture upload at 72 Hz is a tax the Quest notices. The
         * numbers on it are read by a human: 12 Hz is indistinguishable, and
         * anything that matters (state, scores, menus) still dirties the HUD
         * immediately from its own site. */
        {   static int hud_tick;
            if (++hud_tick >= 6) { hud_tick = 0; S.hud_dirty = 1; } }

        /* Nominate by aiming. Only while the cue is actually pointing at a
         * colour — swinging past one on the way to another must not un-nominate
         * the one you meant, so a null result leaves the last nomination alone
         * and the pause menu can override it. */
        if (mine && !S.nom_manual) {
            int c = aimed_colour();
            if (c && c != S.rules.nominated) {
                cue_rules_nominate(&S.rules, c);
                S.hud_dirty = 1;
            }
        }
        /* And the free ball, if one was awarded — same act, same ray, unless
         * you named one from the menu. */
        if (mine && S.rules.free_ball && !S.nom_manual) {
            int id = aimed_ball();
            if (id > 0 && id != S.rules.free_ball_id) {
                cue_rules_nominate_free(&S.rules, id);
                S.hud_dirty = 1;
            }
        }
        if (shot.struck && !mine) shot.struck = 0;
        if (shot.struck) {
            /* The ball leaves faster than the tip arrives. A cue is heavier
             * than a ball (~0.55 kg against 0.16) and the contact is fairly
             * elastic, so for a centre-ball hit the ball departs at roughly 1.35
             * times the tip's speed. Passing the tip speed straight through as
             * the ball's — which is what it did — under-reads every shot by
             * about a third, consistently, which is exactly how it felt. */
            float transfer = CUEVR_CUE_MASS * (1.0f + CUEVR_TIP_E) /
                             (CUEVR_CUE_MASS + S.tab.mass);
            float sp = shot.speed * transfer * CUEVR_POWER_TRIM;
            if (sp > MAX_STRIKE_SPEED) sp = MAX_STRIKE_SPEED;
            LOGI("[cuevr] strike tip %.2f -> ball %.2f m/s  [%d fr, %.1f mm in %.1f ms]  side %+.2f vert %+.2f  elev %.1f deg%s",
                 (double)shot.speed, (double)sp,
                 S.cue.m_frames, (double)(S.cue.m_dist * 1000.0f),
                 (double)(S.cue.m_time * 1000.0f), (double)shot.tip_side, (double)shot.tip_vert,
                 (double)(shot.elev * 180.0f / 3.14159265f),
                 "");
            /* Did it leave the bed? Measured against the elevation the TABLE
             * forced on us, so being made to raise the cue by a cushion never
             * jumps the ball.
             *
             * Against lock_elev, not min_elev: the lock is the figure frozen at
             * trigger-down and played down for the whole delivery, while
             * min_elev is recomputed every frame from a tip that is travelling.
             * They are nearly the same and "nearly" is the wrong word for the
             * number that decides whether the ball leaves the table. */
            float vy = jump_launch(sp, shot.elev, S.cue.lock_elev);

            /* Online: send the strike, not the outcome. Both machines integrate
             * the same 2 kHz physics from the same state, so the same six numbers
             * produce the same table on both sides. */
            if (S.opp == OPP_ONLINE) {
                CueVrNetShot ns;
                ns.dirx = shot.dir.x; ns.dirz = shot.dir.z;
                ns.speed = sp; ns.side = shot.tip_side; ns.vert = shot.tip_vert;
                ns.elev = shot.elev;
                /* And where the white actually is. After ball in hand only this
                 * end knows. */
                ns.cuex = S.balls[0].pos.x; ns.cuez = S.balls[0].pos.z;
                /* And what we declared. Nomination happens by aiming, which the
                 * far end cannot see. */
                ns.nominated = S.rules.nominated;
                ns.free_ball_id = S.rules.free_ball_id;
                /* WHETHER THE BALL LEFT THE BED, decided here and sent, not
                 * re-derived there. The far end would have to reconstruct
                 * min_elev from our tip and our aim to get the same answer, and
                 * an end that reconstructs it a hair differently does not jump
                 * when we did — which is a divergence no amount of position
                 * correction repairs, because the two tables are then playing
                 * different shots. */
                ns.vy = vy;
                cuevr_net_send_shot(&ns);
            }
            cue_phys_strike_jump(&S.world, &S.balls[0], shot.dir, sp,
                                 shot.tip_side, shot.tip_vert, shot.elev, vy);
            /* Power relative to the hardest shot there is, so a delicate safety
             * whispers and a break cracks. */
            cue_audio_sfx(CUE_SFX_STRIKE, sp / MAX_STRIKE_SPEED);
            mote_xr_haptic(0.75f, 70);
            begin_shot();
        }
        break;
    }

    case ST_ROLL: {
        /* Keep the sticks live: a shot takes several seconds to settle and that
         * is exactly when you want to be lining up the next one. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        if (!roll_step(dt)) resolve_shot();
        break;
    }

    case ST_THINK: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        if (S.opp == OPP_ONLINE) {
            /* Their shot arrives as six numbers and we play it ourselves. */
            CueVrNetShot ns;
            /* Their cue design may arrive after the rack — the host sends its
             * hello the same instant it starts — so keep taking it. */
            {   CueVrNetHello ph;
                if (cuevr_net_peer(&ph)) cuevr_render_set_opp_cue(ph.cue_idx); }
            if (cuevr_net_recv_shot(&ns)) {
                Vec3 dir = v3(ns.dirx, 0.0f, ns.dirz);
                float l = v3_len(dir);
                if (l > 1e-4f) dir = v3_scale(dir, 1.0f / l);
                /* Put the white where THEY had it before striking. Ball in hand
                 * is a position only the striker knows, and starting the same
                 * shot from a different spot is how the far end watched every
                 * break sail past the pack. */
                S.balls[0].pos = v3(ns.cuex, S.tab.R, ns.cuez);
                S.balls[0].vel = v3(0,0,0); S.balls[0].w = v3(0,0,0);
                S.balls[0].on = 1;
                /* Judge their shot against what THEY declared, or the scores
                 * drift apart while the balls still agree. */
                if (ns.nominated) cue_rules_nominate(&S.rules, ns.nominated);
                if (ns.free_ball_id) cue_rules_nominate_free(&S.rules, ns.free_ball_id);
                cue_audio_sfx(CUE_SFX_STRIKE, ns.speed / MAX_STRIKE_SPEED);
                cue_phys_strike_jump(&S.world, &S.balls[0], dir, ns.speed,
                                     ns.side, ns.vert, ns.elev, ns.vy);
                begin_shot();
            }
            break;
        }
        if (S.ai_done) {
            think_join();
            S.cpu_shot = cue_ai_plan_result();
            /* The CPU names its colour, like anybody else at the table. Without
             * this it was the one player exempt from the nomination rule: any
             * colour stayed legal for it while you were bound to the one you
             * pointed at. */
            if (S.rules.kind && S.rules.target == 1 &&
                S.cpu_shot.target_id >= CUE_ID_YELLOW &&
                S.cpu_shot.target_id <= CUE_ID_BLACK)
                cue_rules_nominate(&S.rules, S.cpu_shot.target_id - CUE_ID_YELLOW + 2);
            if (S.rules.free_ball && S.cpu_shot.target_id > 0)
                cue_rules_nominate_free(&S.rules, S.cpu_shot.target_id);
            S.cpu_t = 0.0f;
            S.state = ST_CPUCUE;
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_CPUCUE: {
        /* The CPU cues its shot instead of the ball simply departing. It
         * addresses the ball, draws back, and comes through — and the strike is
         * played on contact, so the cue-shot and the clack arrive when you can
         * see what caused them. Cosmetic, but a ball that moves with no cue and
         * no sound reads as the game glitching rather than as an opponent. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        S.cpu_t += dt;

        const float ADDRESS = 0.35f, BACK = 0.30f, THROUGH = 0.16f;
        float travel;                       /* tip distance behind the ball */
        if (S.cpu_t < ADDRESS)                       travel = 0.045f;
        else if (S.cpu_t < ADDRESS + BACK) {
            float k = (S.cpu_t - ADDRESS) / BACK;
            travel = 0.045f + 0.13f * sinf(k * 1.5708f);
        } else {
            float k = (S.cpu_t - ADDRESS - BACK) / THROUGH;
            if (k > 1.0f) k = 1.0f;
            travel = (0.045f + 0.13f) * (1.0f - k * k);   /* accelerating through */
        }

        /* Lay the cue along the aim it chose, behind the cue ball — and up at
         * whatever elevation this shot forces, which is the same number its own
         * ranking sims used. Two reasons it cannot stay level: the shaft would
         * lie through the cushion in plain view, and the strike below would put
         * the ball on a different line from the one the planner simulated. */
        Vec3 aim_t = { cosf(S.cpu_shot.aim), 0.0f, sinf(S.cpu_shot.aim) };
        float cpu_elev;
        {   Vec3 cb = S.balls[0].pos; float Rr = S.tab.R;
            Vec3 ctp = v3(cb.x - cosf(S.cpu_shot.aim)*Rr,
                          Rr*(1.0f + S.cpu_shot.tip_vert),
                          cb.z - sinf(S.cpu_shot.aim)*Rr);
            cpu_elev = cue_table_min_elev(&S.tab, S.balls, S.nballs, ctp,
                                          S.cpu_shot.aim); }
        MoteVrV3 ball = cue_ball_room();
        MoteVrV3 aim_r = mv3_norm(cuevr_table_dir_to_room(&S.setup.place, aim_t));
        { /* tilt the drawn shaft up about the tip, in the room's vertical */
            float ce = cosf(cpu_elev), se = sinf(cpu_elev);
            MoteVrV3 shaft = mv3(aim_r.x * ce, -se, aim_r.z * ce);
            /* Draw back ALONG THE CUE, not along the floor. The tip was being
             * pulled straight back horizontally while the shaft sat at an
             * angle, so an elevated cue slid sideways through its own line
             * instead of stroking down it — which looks exactly as wrong as it
             * is. One direction for the shaft and the travel, always. */
            /* THE TIP GOES WHERE IT IS ACTUALLY STRIKING, not through the
             * middle of the ball.
             *
             * This put the tip on the line through the ball's CENTRE whatever
             * the planner had chosen, and the planner's whole answer to a ball
             * tight under a cushion is to strike HIGHER rather than raise the
             * butt — ai_shot_elev walks the tip up the ball precisely so the
             * cue can stay down. So on exactly those shots the elevation came
             * back near zero, correctly, and the cue was then drawn flat
             * through the centre of the ball: below the cushion top, straight
             * through it. Sometimes it lifted, sometimes it went through, and
             * the difference was whether the planner had solved the clearance
             * with the butt or with the tip.
             *
             * Put the tip on the ball's surface where the shot says: back along
             * the shot line, offset up by tip_vert and across by tip_side, on
             * the sphere. Then the shaft runs back from THERE. */
            MoteVrV3 upv = mv3(0.0f, 1.0f, 0.0f);
            MoteVrV3 sidev = mv3_cross(upv, aim_r);
            {   float sl = mv3_len(sidev);
                sidev = sl > 1e-4f ? mv3_scale(sidev, 1.0f / sl) : mv3(0,0,1); }
            float tv = S.cpu_shot.tip_vert, ts = S.cpu_shot.tip_side;
            float ax = 1.0f - tv*tv - ts*ts;
            ax = ax > 0.0f ? sqrtf(ax) : 0.0f;
            MoteVrV3 nb = mv3_add(mv3_add(mv3_scale(aim_r, -ax),
                                          mv3_scale(upv, tv)),
                                  mv3_scale(sidev, ts));
            {   float nl = mv3_len(nb);
                nb = nl > 1e-4f ? mv3_scale(nb, 1.0f / nl) : mv3_scale(aim_r, -1.0f); }
            S.cpu_tip  = mv3_sub(mv3_add(ball, mv3_scale(nb, S.tab.R + CUEVR_TIP_R)),
                                 mv3_scale(shaft, travel));
            S.cpu_butt = mv3_sub(S.cpu_tip, mv3_scale(shaft, CUEVR_CUE_LEN));
        }

        if (S.cpu_t >= ADDRESS + BACK + THROUGH) {
            if (S.cpu_shot.valid) {
                float sp = S.cpu_shot.power01 * MAX_STRIKE_SPEED;
                cue_audio_sfx(CUE_SFX_STRIKE, S.cpu_shot.power01);
                cue_phys_strike_elev(&S.world, &S.balls[0], aim_t, sp,
                                     S.cpu_shot.tip_side, S.cpu_shot.tip_vert,
                                     cpu_elev);
                LOGI("[cuevr] cpu strike %.2f m/s  side %+.2f vert %+.2f  elev %.1f deg%s",
                     (double)sp, (double)S.cpu_shot.tip_side,
                     (double)S.cpu_shot.tip_vert,
                     (double)(cpu_elev * 180.0f / 3.14159265f),
                     S.cpu_shot.safe ? "  (safety)" : "");
            }
            begin_shot();
        }
        break;
    }

    case ST_PLACE: {
        /* Brought to you, once, the moment the ball goes in your hand. The table
         * slides — it does not turn — so the shot you were looking at is still
         * laid out the way you were looking at it, only now the cue ball is
         * within reach. Translation only: rotating the room under someone in a
         * headset to save them a step is how you make them ill. */
        if (S.recentre) {
            S.recentre = 0;
            MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
            fwd.y = 0.0f;
            if (mv3_len(fwd) > 1e-3f) {
                fwd = mv3_norm(fwd);
                MoteVrV3 have = cue_ball_room();
                /* Out where the shot IS, not right under your chin. A bridge
                 * hand's length put the ball 42 cm away, which is correct for a
                 * player already down on the shot and wrong for one standing up
                 * about to walk in — it arrived in your face with the table
                 * behind it. Nearly a metre puts it where you would stand to
                 * look at it, and you step in from there. */
                const float REACH = 0.90f;
                S.setup.place.pos.x += (t->head.p.x + fwd.x * REACH) - have.x;
                S.setup.place.pos.z += (t->head.p.z + fwd.z * REACH) - have.z;
                S.hud_dirty = 1;
            }
        }
        /* POINT AT THE SPOT WITH THE CUE. Both sticks belong to the table —
         * slide and turn, the same everywhere in the game — so placing the ball
         * cannot have one of them. It used to walk the ball with the left
         * stick, which meant that during ball in hand the one control you use
         * constantly did something else.
         *
         * The pointer is the CUE, not a laser out of the controller. The cue's
         * line is already computed, already trusted, and already visible in
         * your hands: extend it past the tip to where it crosses the cloth and
         * put the ball there. Nothing new has to be calibrated, and there is no
         * question about which way a controller "points", because you can see
         * exactly where the stick is aimed.
         *
         * Clamped against the OTHER BALLS as well as the legal region, so the
         * ball slides round an obstruction instead of being parked inside one
         * and fired out again on the first tick of the shot. */
        /* THE BALL IS IN YOUR HAND, not on the end of a pointer. You are
         * holding it: it follows the controller, and the trigger puts it down.
         * The cue is not in your hand while it is — you cannot hold both, and a
         * cue lying across the table you are trying to place on is in the way.
         *
         * Sticks are untouched by any of this, which is the point: they slide
         * and turn the table here exactly as they do everywhere else.
         *
         * Where it will LAND is the legal position directly under your hand,
         * clamped against the region and the other balls, so an illegal
         * placement is impossible rather than merely discouraged — and the ball
         * is drawn at your hand's height so you can see you are carrying it. */
        {
            /* THE BALL IS ON THE CONTROLLER, and on nothing else.
             *
             * It used to be clamped to the legal region while you carried it,
             * with only its height free — so it hung in the air refusing to
             * follow your hand until you brought it near the table, then
             * snapped about inside the D. You were not holding it; you were
             * dragging a thing that was still attached to the cloth.
             *
             * Now it sits just above the controller and goes exactly where the
             * controller goes, in the room, unbounded. The rules apply when you
             * let go and not before, which is what "in hand" means. */
            /* Carried in the cueing hand, whichever that is. */
            const MoteVrHand *rh = &t->hand[S.lefty ? MOTE_VR_LEFT : MOTE_VR_RIGHT];
            if (rh->tracked) {
                /* OUT IN FRONT, along where the controller points.
                 *
                 * Above the hand put it inside the controller model, which both
                 * looks wrong and hides the thing you are trying to place. Held
                 * out ahead it is clear of the model, you can see it and the
                 * cloth under it at the same time, and steering it is the same
                 * motion as pointing — which is what makes it quick to put
                 * somewhere exact.
                 *
                 * Along the AIM pose, because that is the runtime's own answer
                 * to where the controller points; the grip pose's axes are not
                 * it, and the fifty degrees of pitch the controller model needed
                 * is the measure of how far out a guess would be. Grip is the
                 * fallback for a runtime with no aim pose, where a hand's width
                 * forward of the grip origin is at least in the right region. */
                MoteVrV3 held;
                if (rh->aim_tracked) {
                    MoteVrV3 fwd = mq_rot(rh->aim.q, mv3(0.0f, 0.0f, -1.0f));
                    held = mv3_add(rh->aim.p, mv3_scale(fwd, 0.13f));
                } else {
                    MoteVrV3 fwd = mq_rot(rh->pose.q, mv3(0.0f, 0.0f, -1.0f));
                    held = mv3_add(rh->pose.p, mv3_scale(fwd, 0.13f));
                }
                MoteVrV3 tp = cuevr_room_to_table(&S.setup.place, held);
                S.balls[0].pos = v3(tp.x, tp.y, tp.z);
                S.hud_dirty = 1;
            }
        }

        /* Both sticks do what they do everywhere else: slide the table and
         * turn it about the cue ball. Placement is the one time you genuinely
         * need to see a spot from the other side, and in a room the size anyone
         * plays this in you cannot simply walk round to it — so the turn matters
         * here more than anywhere, which is another reason not to spend a stick
         * on walking the ball. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);

        /* The TRIGGER drops it — the button your finger is already on while
         * you carry something. A stays out of this so it keeps meaning "yes"
         * on the panels. */
        if (t->hand[DOMH].trigger < 0.4f) S.place_latch = 0;
        else if (!S.place_latch) {
            /* Let go: NOW the rules apply. Straight down onto the cloth from
             * wherever you were holding it, then clamped into the legal region
             * and out of anything already standing there. */
            Vec3 p = v3(S.balls[0].pos.x, S.tab.R, S.balls[0].pos.z);
            S.balls[0].pos = cue_table_clamp_placement_balls(
                &S.tab, p, S.balls, S.nballs, S.rules.break_shot);
            S.can_repick = 1;      /* until the stroke is played */
            arm_shot();
            S.state = ST_AIM;
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_DRILLS: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            int n = drill_rows();
            int nrow = (n + DRILL_COLS - 1) / DRILL_COLS;
            /* WHICH CARD THE POINTER IS ON, from the same numbers the gallery
             * draws with. Between the cards is no card rather than the nearest
             * one — opening a challenge you did not mean to is worse than a
             * click that does nothing. */
            int hov = -1;
            if (S.ptr_ok) {
                float fx = (S.ptr_x - (float)DRILL_X0) / (float)DRILL_CW;
                float fy = (S.ptr_y - (float)DRILL_Y0) / (float)DRILL_CH;
                int gx = (int)fx, gy = (int)fy;
                if (fx >= 0.0f && fy >= 0.0f && gx < DRILL_COLS && gy < DRILL_VROWS) {
                    int i = (S.drill_scroll + gy) * DRILL_COLS + gx;
                    if (i < n) hov = i;
                }
            }
            if (hov >= 0 && hov != S.drill_row) { S.drill_row = hov; S.hud_dirty = 1; }
            /* The stick walks the grid: up and down by a row of cards, which is
             * how a grid is walked. */
            int ud = stick_step(t);
            if (ud) {
                int r2 = S.drill_row / DRILL_COLS + ud, c2 = S.drill_row % DRILL_COLS;
                if (r2 < 0) r2 = nrow - 1; else if (r2 >= nrow) r2 = 0;
                int i = r2 * DRILL_COLS + c2;
                if (i >= n) i = n - 1;
                S.drill_row = i;
                S.hud_dirty = 1;
            }
            {   int r2 = S.drill_row / DRILL_COLS;
                if (r2 < S.drill_scroll) S.drill_scroll = r2;
                if (r2 >= S.drill_scroll + DRILL_VROWS)
                    S.drill_scroll = r2 - DRILL_VROWS + 1;
                if (S.drill_scroll > nrow - DRILL_VROWS)
                    S.drill_scroll = nrow - DRILL_VROWS;
                if (S.drill_scroll < 0) S.drill_scroll = 0; }

            int fire = (hov >= 0 && ptr_click(t));
            if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
            else if (!S.btn_latch) { S.btn_latch = 1; fire = 1; }
            /* B leaves, back to the menu that sent you here. */
            if (!t->hand[MOTE_VR_RIGHT].btn_upper) S.dec_latch = 0;
            else if (!S.dec_latch) {
                S.dec_latch = 1;
                S.state = ST_MENU; S.menu_row = MR_START;
                S.btn_latch = 1; S.ptr_latch = 1; S.hud_dirty = 1;
                break;
            }
            if (!fire) break;

            if (S.drill_row == DRILL_ROW_TIMED) {
                /* The six-ball clearance, which is a challenge like any other —
                 * it just builds its own layout each time instead of loading
                 * one, which is the whole of what makes it a different sort. */
                mini_start();
                break;
            }
            if (S.drill_row == n - 1) {
                /* MAKE ONE. The table you have chosen, racked, with the balls
                 * in your hands: setting a position out is the act, and it
                 * starts from a full table because taking balls off is quicker
                 * than putting them on. */
                S.edit_new = 1;
                S.edit_slot = -1;
                S.nballs = cue_table_rack(&S.tab, S.balls);
                cue_rules_init(&S.rules, &S.tab, 0);
                S.rules.turn = 0; S.rules.ball_in_hand = 0;
                S.drill = -1;
                S.edit_ball = -1; S.edit_latch = 1; S.lay_menu = 0;
                S.state = ST_LAYOUT;
                S.hud_dirty = 1;
                snprintf(S.msg, sizeof S.msg, "SET THE BALLS OUT");
                S.msg_time = 3.0f;
                break;
            }
            int slot = drill_slot_at(S.drill_row);
            if (slot >= 0) {
                /* A CARD OPENS. Playing it is the first thing on the page it
                 * opens, so playing is a tap and a press — and every other
                 * thing you might want to do to a challenge is visible instead
                 * of being a chevron on the edge of a row that nothing on
                 * screen mentions. */
                S.edit_slot = slot; S.dset_row = DSET_PLAY; S.dset_ball = -1;
                S.state = ST_DRILLSET; S.ptr_latch = 1;
            }
            break;
        }
    }

    case ST_DRILLSET: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            int slot = S.edit_slot;
            if (slot < 0 || slot >= CUEVR_DRILL_SLOTS) slot = 0;
            CueVrDrill *d = &S.drills.slot[slot];
            uint8_t ids[24];
            int nb = (d->goal == CUEVR_GOAL_POT)
                   ? cuevr_drill_ball_choices((int)d->kind, ids, 24) : 0;

            /* The SAME pitch hud_opt draws at. It was 9 here and 8 there, so
             * every row below the second was hit-tested a row high. */
            int hov = ptr_row_at(12, 8, DSET_N);
            if (hov >= 0 && hov != S.dset_row) { S.dset_row = hov; S.hud_dirty = 1; }
            /* A ball under the pointer, if the grid is showing. */
            int bhov = -1;
            if (nb && S.ptr_ok) {
                int y0 = 12 + DSET_N * 8 + 6 + 8;
                float fx = (S.ptr_x - (DSET_BX - 6)) / (float)DSET_BW;
                float fy = (S.ptr_y - (y0 - 6)) / (float)DSET_BH;
                int cxi = (int)fx, cyi = (int)fy;
                if (fx >= 0 && fy >= 0 && cxi < DSET_BCOLS) {
                    int i = cyi * DSET_BCOLS + cxi;
                    if (i >= 0 && i < nb) bhov = i;
                }
            }
            if (bhov != S.dset_ball) { S.dset_ball = bhov; S.hud_dirty = 1; }

            int click = ptr_click(t);
            if (bhov >= 0 && click) {
                d->need ^= 1u << ids[bhov];
                /* Keep the legacy single id pointing at something sensible so a
                 * name still reads right. */
                for (int i = 0; i < nb; i++)
                    if ((d->need >> ids[i]) & 1u) { d->ball = ids[i]; break; }
                if (S.drill_path[0]) cuevr_drills_save(&S.drills, S.drill_path);
                S.hud_dirty = 1;
                break;
            }
            int zone = ptr_zone();
            if (hov >= 0 && click) {
                switch (S.dset_row) {
                case DSET_GOAL:
                    d->goal = (uint8_t)((d->goal + (zone ? zone : 1) + CUEVR_GOAL_N)
                                        % CUEVR_GOAL_N);
                    if (d->goal == CUEVR_GOAL_POT && d->need == 0) {
                        /* Something ticked to start with, or the drill asks for
                         * nothing and can never be finished. */
                        int best = 0;
                        for (int i = 1; i < S.nballs; i++)
                            if (S.balls[i].on && S.balls[i].id > best) best = S.balls[i].id;
                        if (best) { d->need = 1u << best; d->ball = (uint8_t)best; }
                    }
                    break;
                case DSET_TIMED:
                    /* Nothing to race in a position with no goal. */
                    if (d->goal != CUEVR_GOAL_SETUP) d->timed = (uint8_t)!d->timed;
                    break;
                case DSET_TARGET:
                    if (d->goal == CUEVR_GOAL_SCORE) {
                        int step = (zone < 0) ? -5 : 5;
                        int tv = d->target + step;
                        if (tv < 5) tv = 5;
                        if (tv > 147) tv = 147;
                        d->target = (int16_t)tv;
                    }
                    break;
                case DSET_PLAY:   drill_start(slot); S.btn_latch = 1; break;
                case DSET_EDIT:
                    S.edit_new = 0;
                    /* The table it was made on, FIRST. Restoring onto whatever
                     * table happened to be up put the balls at another table's
                     * dimensions — see drill_use_table. */
                    drill_use_table(d);
                    cue_rules_init(&S.rules, &S.tab, 0);
                    S.rules.turn = 0; S.rules.ball_in_hand = 0;
                    drill_restore(d);
                    S.edit_ball = -1; S.edit_latch = 1; S.lay_menu = 0;
                    S.state = ST_LAYOUT; S.btn_latch = 1;
                    break;
                case DSET_DEL:
                    memset(d, 0, sizeof *d);
                    if (S.drill_path[0]) cuevr_drills_save(&S.drills, S.drill_path);
                    S.state = ST_DRILLS; S.btn_latch = 1;
                    break;
                default:
                    S.state = ST_DRILLS; S.btn_latch = 1;
                    break;
                }
                if (S.drill_path[0]) cuevr_drills_save(&S.drills, S.drill_path);
                S.hud_dirty = 1;
                break;
            }
            if (!t->hand[MOTE_VR_RIGHT].btn_upper) S.dec_latch = 0;
            else if (!S.dec_latch) {
                S.dec_latch = 1;
                S.state = ST_DRILLS; S.ptr_latch = 1; S.hud_dirty = 1;
            }
        }
        break;
    }

    case ST_LAYOUT: {
        /* SET THE BALLS OUT BY HAND.
         *
         * The same act as placing the cue ball, which the game already does
         * well: the ball sits out ahead of the controller where you can see it
         * and the cloth under it at once, and the trigger puts it down. The only
         * new thing is choosing WHICH ball, and that is done by reaching for it
         * — the nearest one to your hand, which is how you would pick a ball off
         * a table.
         *
         * A ball let go beyond the cushions is off the table, so taking balls
         * away needs no button of its own: you put it down where a ball cannot
         * be, which is exactly what you would do with your hand. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        const MoteVrHand *rh = &t->hand[S.lefty ? MOTE_VR_LEFT : MOTE_VR_RIGHT];
        MoteVrV3 held = rh->pose.p;
        if (rh->tracked) {
            MoteVrV3 fwd = rh->aim_tracked ? mq_rot(rh->aim.q, mv3(0,0,-1))
                                           : mq_rot(rh->pose.q, mv3(0,0,-1));
            held = mv3_add(rh->aim_tracked ? rh->aim.p : rh->pose.p,
                           mv3_scale(fwd, 0.13f));
        }
        MoteVrV3 tp = cuevr_room_to_table(&S.setup.place, held);

        /* ONE JOB FOR THE TRIGGER AT A TIME. With the options up it clicks the
         * panel; with them down it picks balls up. Both at once meant reaching
         * for a ball also fired whichever row the ray happened to cross. */
        int down = !S.lay_menu && (t->hand[DOMH].trigger > 0.55f);
        if (S.lay_menu && S.edit_ball >= 0) {
            /* Opening the options while carrying one puts it down first. */
            S.balls[S.edit_ball].pos.y = S.tab.R;
            S.edit_ball = -1;
            S.hud_dirty = 1;
        }
        if (!down) {
            if (S.edit_ball >= 0) {
                /* PUT IT DOWN. On the cloth if it can go there, off the table
                 * if you have carried it past the cushions. */
                CueBall *b = &S.balls[S.edit_ball];
                if (tp.x >  S.tab.half_len || tp.x < -S.tab.half_len ||
                    tp.z >  S.tab.half_wid || tp.z < -S.tab.half_wid) {
                    if (S.edit_ball == 0) {
                        /* except the white, which has to be somewhere */
                        b->pos = cue_table_cue_home(&S.tab);
                        b->on = 1;
                    } else {
                        b->on = 0;
                        snprintf(S.msg, sizeof S.msg, "TAKEN OFF");
                        S.msg_time = 1.2f;
                    }
                } else {
                    /* ANYWHERE ON THE CLOTH. The placement clamp is the RULES'
                     * idea of where a cue ball may go — the D, or behind the
                     * head string — and setting a position out is not playing a
                     * shot. A drill whose white can only start in the D cannot
                     * be most of the positions worth practising, and the object
                     * balls were never subject to it in the first place.
                     *
                     * Still off the other balls and inside the cushions, because
                     * those are physical rather than legal. */
                    float lx = S.tab.half_len - S.tab.R;
                    float lz = S.tab.half_wid - S.tab.R;
                    Vec3 p = v3(tp.x >  lx ?  lx : tp.x < -lx ? -lx : tp.x,
                                S.tab.R,
                                tp.z >  lz ?  lz : tp.z < -lz ? -lz : tp.z);
                    const float sep = 2.0f * S.tab.R + 0.0004f;
                    for (int pass = 0; pass < 6; pass++) {
                        int moved = 0;
                        for (int i = 0; i < S.nballs; i++) {
                            if (i == S.edit_ball || !S.balls[i].on) continue;
                            float dx = p.x - S.balls[i].pos.x;
                            float dz = p.z - S.balls[i].pos.z;
                            float d2 = dx*dx + dz*dz;
                            if (d2 >= sep*sep) continue;
                            float d = sqrtf(d2);
                            if (d < 1e-5f) { dx = 1.0f; dz = 0.0f; d = 1.0f; }
                            p.x = S.balls[i].pos.x + dx / d * sep;
                            p.z = S.balls[i].pos.z + dz / d * sep;
                            moved = 1;
                        }
                        if (p.x >  lx) p.x =  lx;
                        if (p.x < -lx) p.x = -lx;
                        if (p.z >  lz) p.z =  lz;
                        if (p.z < -lz) p.z = -lz;
                        if (!moved) break;
                    }
                    b->pos = p;
                    b->on = 1;
                }
                b->vel = v3(0,0,0); b->w = v3(0,0,0);
                S.edit_ball = -1;
                S.hud_dirty = 1;
            }
            S.edit_latch = 0;
        } else if (!S.edit_latch) {
            S.edit_latch = 1;
            /* REACH FOR ONE. The nearest ball to the hand within a hand's
             * width; balls that are off the table count, so anything you have
             * taken away can be brought back by reaching where it went. */
            int best = -1; float bd = 0.18f * 0.18f;
            for (int i = 0; i < S.nballs; i++) {
                float dx = S.balls[i].pos.x - tp.x, dz = S.balls[i].pos.z - tp.z;
                float d2 = dx*dx + dz*dz;
                if (S.balls[i].on && d2 < bd) { bd = d2; best = i; }
            }
            if (best < 0) {
                /* Nothing there: bring back the first ball that is off. That
                 * is how a ball comes home — reach into empty cloth and one
                 * arrives, rather than hunting a menu for it. */
                for (int i = 1; i < S.nballs; i++)
                    if (!S.balls[i].on) { best = i; break; }
                if (best >= 0) { S.balls[best].on = 1; }
            }
            S.edit_ball = best;
            S.hud_dirty = 1;
        }
        if (S.edit_ball >= 0 && rh->tracked) {
            S.balls[S.edit_ball].pos = v3(tp.x, tp.y, tp.z);
            S.hud_dirty = 1;
        }

        /* THE PANEL'S OWN ROWS. B still finishes, because the act of finishing
         * IS the save and a hand already on the controller should not have to
         * find a menu — but every one of these is a thing somebody has to be
         * able to SEE they can do, and none of them was visible before. */
        {
            /* POINT AT IT, like every other screen. The stick was walking these
             * rows, which is not a thing the game does anywhere else — and here
             * the sticks belong to the table, which is what they do in the
             * editor as much as at the table. */
            int act = -1;
            if (S.lay_menu) {
                int hov = ptr_row_at(12, 8, LAY_N);
                if (hov >= 0 && hov != S.lay_row) { S.lay_row = hov; S.hud_dirty = 1; }
                if (hov >= 0 && ptr_click(t)) act = hov;
            }
            /* B finishes from either side of the menu: the act of finishing IS
             * the save, and a hand on the controller should not have to go
             * looking for a row to say so. */
            if (!t->hand[MOTE_VR_RIGHT].btn_upper) S.btn_latch = 0;
            else if (!S.btn_latch) { S.btn_latch = 1; act = LAY_DONE; }
            if (act < 0) break;
            S.lay_menu = 0;

            /* Whatever is in the hand goes down on the cloth first: an action
             * taken while carrying a ball must not leave it in mid-air. */
            if (act != LAY_TAKEOFF && S.edit_ball >= 0) {
                S.balls[S.edit_ball].pos.y = S.tab.R;
                S.edit_ball = -1;
            }
            S.hud_dirty = 1;

            switch (act) {
            case LAY_TAKEOFF:
                /* The ball in your hand, off — except the white, which has to
                 * be somewhere for the position to be playable at all. */
                if (S.edit_ball > 0) {
                    S.balls[S.edit_ball].on = 0;
                    S.balls[S.edit_ball].vel = v3(0,0,0);
                    S.balls[S.edit_ball].w = v3(0,0,0);
                    S.edit_ball = -1;
                    snprintf(S.msg, sizeof S.msg, "TAKEN OFF");
                } else {
                    snprintf(S.msg, sizeof S.msg, "HOLD A BALL FIRST");
                }
                S.msg_time = 1.5f;
                break;
            case LAY_BACKON: {
                /* One of the ones you took off, back — on its own spot if it
                 * has one and the spot is free, otherwise somewhere clear. */
                int got = -1;
                for (int i = 1; i < S.nballs; i++) if (!S.balls[i].on) { got = i; break; }
                if (got < 0) { snprintf(S.msg, sizeof S.msg, "THEY ARE ALL ON");
                               S.msg_time = 1.5f; break; }
                S.balls[got].on = 1;
                S.balls[got].vel = v3(0,0,0); S.balls[got].w = v3(0,0,0);
                S.balls[got].pos = cue_table_clamp_placement_balls(
                    &S.tab, v3(0.0f, S.tab.R, 0.0f), S.balls, S.nballs, 0);
                /* Straight into your hand, so it can be put where you want it
                 * without hunting for where it landed. */
                S.edit_ball = got;
                S.edit_latch = 1;
                snprintf(S.msg, sizeof S.msg, "BACK ON - PUT IT DOWN");
                S.msg_time = 2.0f;
                break;
            }
            case LAY_RACK:
                S.nballs = cue_table_rack(&S.tab, S.balls);
                S.edit_ball = -1;
                snprintf(S.msg, sizeof S.msg, "RACKED");
                S.msg_time = 1.5f;
                break;
            case LAY_CLEAR:
                for (int i = 1; i < S.nballs; i++) S.balls[i].on = 0;
                S.balls[0].on = 1;
                S.balls[0].pos = cue_table_cue_home(&S.tab);
                S.edit_ball = -1;
                snprintf(S.msg, sizeof S.msg, "CLEARED");
                S.msg_time = 1.5f;
                break;
            case LAY_CANCEL:
                S.edit_new = 0;
                S.state = ST_DRILLS;
                S.ptr_latch = 1;
                break;
            default: {
                /* DONE — and it SAVES, for an existing challenge as much as a
                 * new one. Editing an old one used to drop you into a frame
                 * with the changes discarded, which is the worst possible
                 * answer: it looks like it worked until you come back. */
                int slot = S.edit_slot;
                if (slot < 0 || slot >= CUEVR_DRILL_SLOTS ||
                    (S.edit_new && S.drills.slot[slot].used)) {
                    slot = -1;
                    for (int i = 0; i < CUEVR_DRILL_SLOTS; i++)
                        if (!S.drills.slot[i].used) { slot = i; break; }
                }
                if (slot < 0) {
                    snprintf(S.msg, sizeof S.msg, "NO FREE SLOTS");
                    S.msg_time = 3.0f;
                    S.state = ST_DRILLS;
                } else {
                    /* The goal and the clock survive an edit: you came here to
                     * move the balls, not to be asked what it is for again. */
                    CueVrDrill keep = S.drills.slot[slot];
                    drill_capture(&S.drills.slot[slot]);
                    if (!S.edit_new) {
                        S.drills.slot[slot].goal   = keep.goal;
                        S.drills.slot[slot].need   = keep.need;
                        S.drills.slot[slot].target = keep.target;
                        S.drills.slot[slot].timed  = keep.timed;
                        S.drills.slot[slot].best   = keep.best;
                        S.drills.slot[slot].tries  = keep.tries;
                        S.drills.slot[slot].wins   = keep.wins;
                    }
                    if (S.drill_path[0]) cuevr_drills_save(&S.drills, S.drill_path);
                    S.edit_slot = slot;
                    S.drill_row = drill_card_of(slot);
                    snprintf(S.msg, sizeof S.msg, "SAVED");
                    S.msg_time = 2.0f;
                    /* Straight on to what it ASKS. A saved position with no goal
                     * is half a challenge, and the moment you have just finished
                     * setting it out is the moment you know what it is for. */
                    S.state = ST_DRILLSET;
                    S.dset_row = DSET_PLAY;
                }
                S.edit_new = 0;
                S.ptr_latch = 1;
                break;
            }
            }
        }
        break;
    }

    case ST_CLOTH: {
        /* The sticks still belong to the table, here as everywhere. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            /* Which swatch the pointer is over, from the SAME numbers the
             * drawing uses. The gaps between swatches count as no swatch
             * rather than as the nearest one: a colour you did not mean to
             * pick is worse than a press that does nothing. */
            int hov = -1;
            if (S.ptr_ok) {
                float fx = (S.ptr_x - (float)CLOTH_X0) / (float)CLOTH_CW;
                float fy = (S.ptr_y - (float)CLOTH_Y0) / (float)CLOTH_CH;
                int cx = (int)fx, cy = (int)fy;
                if (fx >= 0.0f && fy >= 0.0f && cx < CLOTH_COLS && cy < CLOTH_ROWS) {
                    int i = cy * CLOTH_COLS + cx;
                    if (i < CUE_NCLOTH) hov = i;
                }
                if (S.ptr_y >= (float)(CLOTH_BACK_Y - 2) &&
                    S.ptr_y <  (float)(CLOTH_BACK_Y + 11)) hov = CLOTH_BACK;
            }
            if (hov != S.cloth_hov) { S.cloth_hov = hov; S.hud_dirty = 1; }

            if (hov == CLOTH_BACK && ptr_click(t)) {
                S.state = ST_APPEAR;
                S.menu_row = AR_CLOTH;
                S.btn_latch = 1;
                S.hud_dirty = 1;
                break;
            }
            if (hov >= 0 && ptr_click(t)) {
                S.cloth_idx = hov;
                /* On the table at once, not on the way out: the whole point of
                 * a card is seeing the cloth on your own table before you
                 * commit to it. */
                S.tab.cloth = cloth_colour(S.cloth_idx);
                cuevr_render_set_table(&S.tab, &S.world);
                cue_audio_sfx(CUE_SFX_UI, 0.4f);
                S.hud_dirty = 1;
                break;
            }
            /* B, or A anywhere off the grid, goes back — a grid with a BACK row
             * under it puts the row where a swatch should be. */
            int a = t->hand[MOTE_VR_RIGHT].btn_lower;
            int b = t->hand[MOTE_VR_RIGHT].btn_upper;
            if (!a && !b) S.btn_latch = 0;
            else if (!S.btn_latch) {
                S.btn_latch = 1;
                S.state = ST_APPEAR;
                S.menu_row = AR_CLOTH;
                S.ptr_latch = 1;
                S.hud_dirty = 1;
            }
        }
        break;
    }

    case ST_APPEAR: {
        /* Same shape as the main menu: point, and the chevrons on the row you
         * are over change the value. BACK has no value, so anywhere on it
         * leaves. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            int hov = ptr_row_at(12, 8, AR_N);
            if (hov >= 0 && hov != S.menu_row) { S.menu_row = hov; S.hud_dirty = 1; }
            int click = ptr_click(t);
            if (hov >= 0 && click) {
                if (S.menu_row == AR_BACK) {
                    S.state = S.appear_from;
                    S.menu_row = (S.appear_from == ST_MENU) ? MR_APPEAR : 0;
                    if (S.appear_from == ST_PAUSE) S.pause_sel = PS_APPEAR;
                    S.btn_latch = 1;
                    S.hud_dirty = 1;
                    break;
                }
                if (S.menu_row == AR_CLOTH) {
                    S.state = ST_CLOTH;
                    S.cloth_hov = -1;
                    S.ptr_latch = 1;
                    S.hud_dirty = 1;
                    break;
                }
#if CUEVR_TUNE_POCKETS
                if (S.menu_row == AR_POCKETS) {
                    S.state = ST_POCKETS;
                    S.menu_row = 0;
                    S.ptr_latch = 1;
                    S.hud_dirty = 1;
                    break;
                }
#endif
                int d = ptr_zone();
                if (d == 0) d = 1;              /* the middle steps forward */
                switch (S.menu_row) {
                case AR_CLOTH: break;   /* handled above: it opens the card */
                case AR_FRAME: S.frame_idx = (S.frame_idx + d + CUE_NFRAME) % CUE_NFRAME; break;
                case AR_BODY: {
                    /* -1 is AUTO and sits before the first design, so the list runs
                     * AUTO, REGENCY, CABINET, ... and wraps. */
                    int n = cuevr_render_body_count() + 1;
                    int i2 = S.body_idx + 1;
                    i2 = (i2 + d + n) % n;
                    S.body_idx = i2 - 1;
                    cuevr_render_set_body(S.body_idx);
                    break;
                }
                case AR_LIGHT:
                    S.light_idx = (S.light_idx + d + cuevr_render_light_count())
                                    % cuevr_render_light_count();
                    cuevr_render_set_light(S.light_idx);
                    break;
                case AR_BALLS: {
                    int b0 = S.ballset;
                    do { b0 = (b0 + d + CUE_NBALLSET) % CUE_NBALLSET; }
                    while (!cue_ballset_ok((int)S.tab.kind, b0) && b0 != S.ballset);
                    S.ballset = b0;
                    cue_render_set_ball_set(S.ballset);
                    break;
                }
                case AR_SPOTS:
                    S.cue_spots = !S.cue_spots;
                    cue_render_set_cue_spots(S.cue_spots);
                    break;
                case AR_CUE: {
                    int n = cuevr_render_cue_count();
                    S.cue_idx = (S.cue_idx + d + n) % n;
                    cuevr_render_set_cue(S.cue_idx);
                    break;
                }
                case AR_SURROUND:
                    S.surround = (S.surround + d + 3) % 3;
                    cuevr_render_set_surround(S.surround);
                    /* No point compositing a camera feed the environment is
                     * about to paint over. */
                    mote_xr_show_passthrough(S.surround == 0);
                    break;
                default: break;
                }
                restyle_table();
                S.hud_dirty = 1;
            }
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1;
            S.state = S.appear_from;
            S.menu_row = (S.appear_from == ST_MENU) ? MR_APPEAR : 0;
            if (S.appear_from == ST_PAUSE) S.pause_sel = PS_APPEAR;
            S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }


    case ST_CONTROLS: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            int hov = ptr_row_at(12, 8, CR_N);
            if (hov >= 0 && hov != S.menu_row) { S.menu_row = hov; S.hud_dirty = 1; }
            if (hov >= 0 && ptr_click(t)) {
                if (S.menu_row == CR_BACK) {
                    S.state = S.appear_from;
                    S.menu_row = (S.appear_from == ST_MENU) ? MR_CONTROLS : 0;
                    if (S.appear_from == ST_PAUSE) S.pause_sel = PS_CONTROLS;
                    S.btn_latch = 1;
                    S.hud_dirty = 1;
                    break;
                }
                if (S.menu_row == CR_RESET) {
                    CueVrPrefs d; cuevr_prefs_defaults(&d);
                    S.cue.rest = d.rest;
                    S.cue.grip = d.grip;
                    S.cue.adj_have0 = 0;
                    for (int i = 0; i < 3; i++) {
                        S.cal_pos[i] = d.ctrl_pos[i];
                        S.cal_rot[i] = d.ctrl_rot[i];
                    }
                    cuevr_render_set_ctrl_cal(S.cal_pos, S.cal_rot);
                    snprintf(S.msg, sizeof S.msg, "CUE RESET");
                    S.msg_time = 2.5f;
                    S.hud_dirty = 1;
                    break;
                }
                switch (S.menu_row) {
                case CR_HAND:     S.lefty = !S.lefty; break;
                case CR_STICKS:   S.stick_swap = !S.stick_swap; break;
                case CR_INVSLIDE: S.inv_slide = !S.inv_slide; break;
                case CR_INVTURN:  S.inv_turn = !S.inv_turn; break;
                default: break;
                }
                cuevr_cue_left_handed(S.lefty);
                cuevr_setup_left_handed(S.lefty);
                cuevr_setup_sticks(S.stick_swap, S.inv_slide, S.inv_turn);
                S.hud_dirty = 1;
            }
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1;
            S.state = S.appear_from;
            S.menu_row = (S.appear_from == ST_MENU) ? MR_CONTROLS : 0;
            if (S.appear_from == ST_PAUSE) S.pause_sel = PS_CONTROLS;
            S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }

#if CUEVR_TUNE_POCKETS
    case ST_POCKETS: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        int hov = ptr_row_at(12, 8, 5);
        if (hov >= 0 && hov != S.menu_row) { S.menu_row = hov; S.hud_dirty = 1; }
        if (hov >= 0 && ptr_click(t)) {
            if (hov == 4) {
                pockets_write();
                S.state = ST_APPEAR; S.menu_row = AR_POCKETS;
            } else {
                int z = ptr_zone();
                if (z) {
                    int *p2 = (hov == 0) ? &S.cut_cr : (hov == 1) ? &S.cut_cs
                            : (hov == 2) ? &S.cut_mr : &S.cut_ms;
                    int lo = (hov & 1) ? -30 : 40, hi = (hov & 1) ? 80 : 320;
                    int st = (hov & 1) ? 1 : 3;
                    *p2 += z * st;
                    if (*p2 < lo) *p2 = lo;
                    if (*p2 > hi) *p2 = hi;
                    /* Redraw the table right now — the whole point is to watch
                     * it change with a ball sitting next to the pocket. */
                    cue_render_set_pocket_cut(S.cut_cr/100.0f, S.cut_cs/1000.0f,
                                              S.cut_mr/100.0f, S.cut_ms/1000.0f);
                    cuevr_render_set_table(&S.tab, &S.world);
                    pockets_log();
                    S.stat_dirty = 1;
                }
            }
            S.hud_dirty = 1;
        }
        /* A or B both leave, and the MENU button pauses out of it — three ways
         * out, because a tuning screen you cannot leave costs somebody their
         * frame. */
        if ((t->hand[MOTE_VR_RIGHT].btn_lower || t->hand[MOTE_VR_RIGHT].btn_upper)
            && !S.btn_latch) {
            S.btn_latch = 1; pockets_write();
            S.state = ST_APPEAR; S.menu_row = AR_POCKETS;
            S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower && !t->hand[MOTE_VR_RIGHT].btn_upper)
            S.btn_latch = 0;
        break;
    }

#endif

    case ST_CARSETUP: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        int n = CUE_GAME_COUNT + 2;
        int hov = ptr_row_at(12, 8, n);
        if (hov >= 0 && hov != S.car_row) { S.car_row = hov; S.hud_dirty = 1; }
        if (hov >= 0 && ptr_click(t)) {
            if (hov < CUE_GAME_COUNT) S.car_pick[hov] = !S.car_pick[hov];
            else if (hov == CUE_GAME_COUNT) {
                int kinds[CUE_GAME_COUNT], k = 0;
                for (int i = 0; i < CUE_GAME_COUNT; i++)
                    if (S.car_pick[i]) kinds[k++] = i;
                if (k && cuevr_career_new(&S.career, kinds, k)) {
                    career_save();
                    S.state = ST_CAREER;
                    S.car_row = 0;
                }
            } else { S.state = ST_MENU; S.menu_row = MR_START; }
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1; S.state = ST_MENU; S.menu_row = MR_START; S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }

    case ST_CAREER: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        int hov = ptr_row_at(65, 10, 4);
        if (hov >= 0 && hov != S.car_row) { S.car_row = hov; S.hud_dirty = 1; }
        if (hov >= 0 && ptr_click(t)) {
            if (hov == 0) {
                if (cuevr_career_next(&S.career, NULL) >= 0) career_play();
            } else if (hov == 1) {
                S.car_view = 0; S.car_row = 0; S.state = ST_CARTABLE;
            } else if (hov == 2) {
                S.car_scroll = 0; S.car_row = 0; S.state = ST_CARACH;
            } else {
                S.opp = OPP_CAREER; S.state = ST_MENU; S.menu_row = MR_START;
            }
            S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }

    case ST_CARTABLE: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        int hov = -1;
        if (S.ptr_ok) {
            if (S.ptr_y >= 12.0f && S.ptr_y < 21.0f)  hov = 0;
            else if (S.ptr_y >= (float)(HH - 11))      hov = 1;
        }
        if (hov >= 0 && hov != S.car_row) { S.car_row = hov; S.hud_dirty = 1; }
        if (hov >= 0 && ptr_click(t)) {
            if (hov == 0 && S.career.nleague > 1)
                S.car_view = (S.car_view + 1) % S.career.nleague;
            else if (hov == 1) { S.state = ST_CAREER; S.car_row = 1; }
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1; S.state = ST_CAREER; S.car_row = 1; S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }

    case ST_CARACH: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        int hov = -1;
        if (S.ptr_ok) {
            if (S.ptr_y >= (float)(HH - 11))            hov = 0;
            else if (S.ptr_y >= (float)(HH - 20))       hov = 1;
        }
        if (hov >= 0 && hov != S.car_row) { S.car_row = hov; S.hud_dirty = 1; }
        if (hov >= 0 && ptr_click(t)) {
            if (hov == 1) {
                int page = (HH - 22) - 14, total = CAR_ACH_N * 14;
                int maxs = total - page; if (maxs < 0) maxs = 0;
                S.car_scroll = (S.car_scroll < maxs)
                             ? (S.car_scroll + page > maxs ? maxs : S.car_scroll + page)
                             : 0;
            } else { S.state = ST_CAREER; S.car_row = 2; }
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1; S.state = ST_CAREER; S.car_row = 2; S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }

    case ST_STATS: {
        /* Two rows, both drawn, both hit-tested where they are drawn: the page
         * at the top and the way out at the bottom. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        {
            int hov = -1;
            if (S.ptr_ok) {
                if (S.ptr_y >= 12.0f && S.ptr_y < 21.0f)          hov = 0;
                else if (S.ptr_y >= (float)(HH - 11))              hov = 1;
                else if (S.ptr_y >= (float)(HH - 20) &&
                         S.ptr_y <  (float)(HH - 11))              hov = 2;
            }
            if (hov >= 0 && hov != S.menu_row) { S.menu_row = hov; S.hud_dirty = 1; }
            if (hov >= 0 && ptr_click(t)) {
                if (hov == 0) { S.stat_page ^= 1; S.stat_scroll = 0; }
                else if (hov == 2) {
                    /* One page at a time, and round to the top at the bottom —
                     * a scroll with no way back up is a trap. */
                    int page = (HH - 22) - 24;
                    int maxs = S.stat_len - page;
                    if (maxs < 0) maxs = 0;
                    S.stat_scroll = (S.stat_scroll < maxs)
                                  ? (S.stat_scroll + page > maxs ? maxs : S.stat_scroll + page)
                                  : 0;
                }
                else {
                    S.state = S.appear_from;
                    S.menu_row = (S.appear_from == ST_MENU) ? MR_STATS : 0;
                    if (S.appear_from == ST_PAUSE) S.pause_sel = PS_STATS;
                    S.btn_latch = 1;
                }
                S.hud_dirty = 1;
            }
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.btn_latch) {
            S.btn_latch = 1;
            S.state = S.appear_from;
            S.menu_row = (S.appear_from == ST_MENU) ? MR_STATS : 0;
            if (S.appear_from == ST_PAUSE) S.pause_sel = PS_STATS;
            S.hud_dirty = 1;
        }
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.btn_latch = 0;
        break;
    }


    case ST_DECIDE: {
        /* Sticks stay the table's here too. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        /* ONLINE, THE CHOICE BELONGS TO ONE OF US. Both ends reach this state —
         * they have to, because both are holding a frame that cannot go on
         * until it is answered — but only the fouled-AGAINST player may answer.
         * Without this both players got a live menu after every foul and the
         * offender could answer their own, whichever end pressed first, and the
         * two ends then applied different decisions to the same frame. We wait
         * and take theirs off the wire. A push-out belongs to the player at the
         * table, a foul decision to the one who was fouled against — the same
         * split the CPU path makes. */
        if (S.opp == OPP_ONLINE) {
            int decider = S.rules.pushout_offer ? S.rules.turn
                                                : 1 - S.rules.dec_offender;
            if (decider != S.net_me) break;
        }
        /* Pointed at, and driven off the SAME list the HUD draws — so every
         * choice the rules offer is a choice you can make, and none of them can
         * be named on screen without being wired up or wired up without being
         * named. */
        {
            DecOpt o[6];
            int n = decision_options(o, 6);
            if (S.dec_sel >= n) S.dec_sel = 0;
            int hov = ptr_row_at(22, 14, n);
            if (hov >= 0 && hov != S.dec_sel) { S.dec_sel = hov; S.hud_dirty = 1; }

            int fire = 0;
            if (hov >= 0 && ptr_click(t)) fire = 1;
            int a = t->hand[MOTE_VR_RIGHT].btn_lower;
            if (!a) S.dec_latch = 0;
            else if (!S.dec_latch) { S.dec_latch = 1; fire = 1; }
            if (!fire) break;

            int dec = o[S.dec_sel].dec;
            /* The far end is sitting on the same pending decision and cannot
             * see which row we picked. */
            if (S.opp == OPP_ONLINE) {
                CueVrNetCall c = { dec, S.net_me };
                cuevr_net_send_call(&c);
            }
            if (S.rules.pushout_offer) {
                S.rules.is_pushout = (dec == CUE_DEC_PLAY) ? 1 : 0;
                S.rules.pushout_offer = 0;
                S.rules.pushout_avail = 0;
            } else {
                if (dec == CUE_DEC_REPLAY) snap_restore_balls();
                cue_rules_apply_decision(&S.rules, dec);
                /* "Free ball." Only when one is actually awarded — the option
                 * being on the list is not the call. */
                if (S.rules.free_ball) cuevr_refcall_say(CUEVR_SAY_FREE_BALL);
            }
        }
        /* Through hand_over() whether or not the ball is in hand, because the
         * FAR end applies this same decision through hand_over() and the two
         * have to land in the same place. Placing inline here instead put the
         * decider in ST_PLACE by a route that never asked whose seat it was —
         * the same omission that froze the frame after the break. hand_over()
         * puts the white on its home spot for the player who owns it and sends
         * everyone else to wait. */
        arm_shot();
        hand_over();
        net_push_state();
        S.hud_dirty = 1;
        break;
    }

    case ST_OVER: {
        /* A won frame in an unfinished match leads to the next frame, not back
         * to the main menu — otherwise "best of 7" is seven trips through the
         * table setup. A won MATCH goes back. */
        int go = 0;
        if (!t->hand[MOTE_VR_RIGHT].btn_lower) S.over_latch = 0;
        else if (!S.over_latch) { S.over_latch = 1; go = 1; }
        /* And by pointing at it, because every other screen is pointed at. The
         * prompt still says A: A is what is printed on the button, and this is
         * the one place the game asks for a button by name. */
        int hov = (S.ptr_ok && S.ptr_y >= (float)(CUEVR_HUD_LH - 9));
        if (hov != S.over_hov) { S.over_hov = hov; S.hud_dirty = 1; }
        if (hov && ptr_click(t)) go = 1;
        if (go) {
            if (S.rules.best_of > 1 && !S.rules.match_over) {
                think_join();
                cue_rules_next_frame(&S.rules, &S.tab);
                S.nballs = cue_table_rack(&S.tab, S.balls);
                stage_cue_ball();
                S.have_snap = 0;
                S.shot_events = 0;
                S.nom_manual = 0;
                /* A NEW FRAME IS A NEW FRAME. Neither of these was cleared, so
                 * from the second frame of a match on, `stat_counted` was still
                 * set from the first — and every frame after it went unrecorded
                 * in the career figures. */
                S.stat_counted = 0;
                S.stat_visit_owner = -1;
                S.stat_visit_full = 0;
                stat_frame_reset();
                snprintf(S.msg, sizeof S.msg, "FRAME %d",
                         S.rules.frames[0] + S.rules.frames[1] + 1);
                S.msg_time = 3.0f;
                S.rules.ball_in_hand = 1;
                hand_over();
                /* A NEW RACK IS A NEW TABLE, and each end reaches this on its
                 * own press of A rather than together, so the host says what
                 * the new frame looks like as soon as it has one. The rack and
                 * the break alternation are both deterministic, so this is a
                 * belt to the braces — but it is the cheap half of the pair. */
                net_push_state();
            } else if (S.in_career) {
                career_finish();
                S.btn_latch = 1;
                S.ptr_latch = 1;
            } else {
                S.state = ST_MENU;
                /* THE PRESS THAT LEFT THIS SCREEN IS SPENT. The menu keeps its
                 * row from last time — usually START GAME — and A was still
                 * down when it arrived, so the same physical press started a
                 * whole new game before the menu was even seen. Both latches,
                 * because the click that got here may have been the trigger. */
                S.btn_latch = 1;
                S.ptr_latch = 1;
                S.menu_row = MR_GAME;
            }
            S.hud_dirty = 1;
        }
        break;
    }
    }

    /* ---- describe the scene ---- */
    S.scene.place   = &S.setup.place;
    S.scene.balls   = S.balls;
    S.scene.nballs  = S.nballs;
    /* Visible whenever you are holding it, not only when it happens to be
     * lined up. */
    if (getenv("CUEVR_CUESHOT")) {
        /* Stage the cue for a design capture. Headless has no tracked hands, so
         * the cue was simply never drawn and every cue design shipped on
         * arithmetic and hope — three passes of the forced elevation reached a
         * headset before anyone could look at it. Lay it along the table, level
         * and above the cloth, where a whole cue is in frame. */
        S.scene.cue_visible = 1;
        S.scene.cue_tip  = cuevr_table_to_room(&S.setup.place, v3(-1.45f, 0.16f, 0.0f));
        S.scene.cue_butt = cuevr_table_to_room(&S.setup.place, v3( 0.00f, 0.16f, 0.0f));
        S.scene.cue_roll = 0.0f;
        S.scene.ocue_visible = 0;
    } else {
        /* YOUR CUE IS ALWAYS IN YOUR HANDS. It used to appear for ST_AIM and
         * ST_PLACE and vanish for everything else — so it blinked out the
         * moment you struck, stayed gone while the balls ran, and was absent
         * for the whole of the opponent's visit. Real cues do not do that. You
         * are holding it: it is drawn.
         *
         * Menus are the exception, and only the ones that put a panel in your
         * face: a cue through the middle of the list you are reading helps
         * nobody. Everything else in play keeps it. */
        int in_menu = (S.state == ST_MENU || S.state == ST_SETUP ||
                       S.state == ST_LOBBY || S.state == ST_PAUSE ||
                       S.state == ST_STATS || S.state == ST_OVER ||
                       S.state == ST_APPEAR || S.state == ST_CLOTH ||
                       S.state == ST_DRILLS || S.state == ST_LAYOUT || S.state == ST_DRILLSET ||
                       S.state == ST_CARSETUP ||
                       S.state == ST_CAREER || S.state == ST_CARTABLE ||
                       S.state == ST_CARACH || S.state == ST_POCKETS ||
                       S.state == ST_PLACE || S.state == ST_CONTROLS);
        /* APPEARANCE hides the HELD cue and lays the display one on the cloth
         * instead, the way the main menu does. That screen is where you pick a
         * cue, and a cue you are gripping is pointing wherever you are pointing
         * the laser — swinging about at arm's length, mostly end-on, with the
         * butt in your face. On the table it lies still, side on, whole. */
        S.scene.cue_visible = !in_menu && S.cue.tracked;
        S.scene.cue_butt = S.cue.butt;
        S.scene.cue_tip  = S.cue.tip;
        S.scene.cue_roll = 0.0f;

        /* And the opponent's, on its own slot. The CPU while it is down on the
         * shot; a live opponent whenever their cue is arriving, so you can
         * watch them walk round the table and line one up rather than seeing a
         * stick appear for the stroke and vanish again. */
        S.scene.ocue_visible = (S.state == ST_CPUCUE);
        S.scene.ocue_tip  = S.cpu_tip;
        S.scene.ocue_butt = S.cpu_butt;
        if (S.opp == OPP_ONLINE) {
            CueVrNetPose pp;
            if (cuevr_net_peer_pose(&pp)) {
                /* Table space on the wire, so it lands right however each end
                 * has put its own table down in its own room. */
                Vec3 tip  = v3(pp.tipx, pp.tipy, pp.tipz);
                Vec3 butt = v3(pp.bttx, pp.btty, pp.bttz);
                /* SMOOTHED TOWARDS, not snapped to.
                 *
                 * Sending at 72 Hz instead of 18 fixes the sender's share of the
                 * judder, but the packets do not ARRIVE evenly — a relay across
                 * the Internet delivers three in a burst and then nothing for
                 * two frames, and a cue drawn exactly where the last packet said
                 * shows every bit of that as a stutter. Chasing the target at a
                 * fixed rate per frame turns arrival jitter into a cue that
                 * always moves smoothly and is at most a few milliseconds
                 * behind, which no one can see and everyone can see the
                 * alternative to. */
                if (!S.ocue_have) { S.ocue_tip = tip; S.ocue_butt = butt; S.ocue_have = 1; }
                else {
                    float k = dt * 30.0f;             /* ~1/3 of the gap a frame at 72 Hz */
                    if (k > 1.0f) k = 1.0f;
                    S.ocue_tip  = v3_add(S.ocue_tip,  v3_scale(v3_sub(tip,  S.ocue_tip),  k));
                    S.ocue_butt = v3_add(S.ocue_butt, v3_scale(v3_sub(butt, S.ocue_butt), k));
                }
                S.scene.ocue_tip  = cuevr_table_to_room(&S.setup.place, S.ocue_tip);
                S.scene.ocue_butt = cuevr_table_to_room(&S.setup.place, S.ocue_butt);
                S.scene.ocue_visible = 1;
            } else S.ocue_have = 0;
        }
    }

    /* Where the panel goes depends on what it is for.
     *
     * While the game is asking you something — the table menu, placing the
     * table, placing the cue ball, answering a foul — it sits in front of your
     * face, at arm's length, because a panel you cannot find is a game you
     * cannot start. Hanging it off the table was how a mis-sited table took the
     * whole interface with it.
     *
     * In play it is a scoreboard on the wall past the far end, where you can
     * glance at it without it being in the way of a shot. */
    {
        int asking = (S.state == ST_MENU || S.state == ST_SETUP ||
                      S.state == ST_PLACE || S.state == ST_DECIDE ||
                      S.state == ST_OVER || S.state == ST_PAUSE ||
                      S.state == ST_LOBBY ||
                      S.state == ST_APPEAR || S.state == ST_STATS ||
                      S.state == ST_CLOTH ||
                      S.state == ST_CONTROLS || S.state == ST_CARSETUP ||
                      S.state == ST_CAREER || S.state == ST_CARTABLE ||
                      S.state == ST_CARACH || S.state == ST_POCKETS);
        MoteVrV3 pos;
        if (asking) {
            /* BEHIND THE TABLE, not stuck to your face.
             *
             * It used to hang 80 cm along your heading, so it followed you
             * everywhere: turn to look at something and the menu came too, and
             * a panel you cannot look away from is a panel you cannot look at.
             * It also meant the table you were choosing a cloth for was behind
             * the thing describing it.
             *
             * The same rule the scoreboard uses: past whichever end is away
             * from you, so it is never behind your head, and standing in the
             * room rather than on your nose. You look at it, choose, and look
             * back at the table — and the table is right there in front of it
             * while you do. */
            MoteVrV3 ht = cuevr_room_to_table(&S.setup.place, t->head.p);
            float end = hud_end_sign(ht.x) * (S.tab.half_len + 0.42f);
            pos = cuevr_table_to_room(&S.setup.place, (Vec3){ end, 0.0f, 0.0f });
            /* Size it FIRST, because how tall it is decides how high it has to
             * hang. Measured along the floor, so raising the panel cannot feed
             * back into its own width. It subtends the same angle wherever you
             * stand, exactly as the scoreboard does — from the far end of a 12
             * ft table a fixed width is unreadable. */
            int rows = S.scene.hud_rows ? S.scene.hud_rows : CUEVR_HUD_LH;
            float d;
            {
                MoteVrV3 flat = mv3_sub(t->head.p, pos);
                flat.y = 0.0f;
                d = mv3_len(flat);
                float w = d * 0.46f;
                /* A LIST IS TALL. 112 rows at 1.6 m wide is a 1.4 m billboard
                 * you have to crane at; the cap is on the HEIGHT, so the wide
                 * 72-row scoreboard is untouched and only the list screens are
                 * held in. */
                float wmax = 0.95f * (float)HW / (float)rows;
                if (w > wmax) w = wmax;
                S.scene.hud_w = w < 0.50f ? 0.50f : w;
            }
            float ph = S.scene.hud_w * (float)rows / (float)HW;

            /* HIGH ENOUGH THAT THE TABLE CANNOT CUT IT.
             *
             * The panel hangs PAST the end of the table, so from where you
             * stand the table is in front of it. Centred on eye level, half of
             * it fell below the rail and the bottom of every list was simply
             * not there.
             *
             * The rule is the one that cannot go wrong: the whole panel lives
             * ABOVE THE PLANE OF THE RAIL. A player's eye is always above the
             * rail — you are standing at a table — so a sightline from the eye
             * to any part of the panel passes over the rail and nothing on the
             * table can be in front of it, from any position, on any table
             * size. (The alternative, projecting the eye-over-rail line out to
             * the panel, is exact and also depends on eye height, which moves
             * every time the player does.) */
            {
                float rail = S.setup.place.pos.y + S.tab.cushion_h * 1.30f;
                pos.y = rail + 0.04f + ph * 0.5f;
            }
        } else if (S.surround == 2) {
            /* IN THE ARENA, THE BOARD IS ON THE WALL.
             *
             * The entrance end is a flat dark face from floor to ceiling and it
             * carries a screen — a real one, with a casing, built into the room
             * — so the scoreboard goes ON it rather than hovering in the air
             * somewhere near the table. It does not follow you and it does not
             * turn to face you: it is bolted to a wall, and you look at it.
             *
             * Arena space is the table's, rotated by the table's yaw and
             * standing on the room's floor, so the same transform the arena
             * mesh is drawn with places the panel. */
            float sx, sy, sw;
            cuevr_arena_screen(&sx, &sy, &sw);
            pos = cuevr_table_to_room(&S.setup.place, (Vec3){ sx, 0.0f, 0.0f });
            pos.y = (S.setup.place.pos.y - S.setup.place.height) + sy;
            S.scene.hud_w = sw;
            /* Facing back down the room: the wall's own normal, turned with the
             * table, and level — a screen on a wall is not tilted at you. */
            {
                MoteVrV3 n = mq_rot(mq_axis_angle(mv3(0,1,0), S.setup.place.yaw),
                                    mv3(-1, 0, 0));
                n.y = 0.0f;
                n = mv3_len(n) > 1e-4f ? mv3_norm(n) : mv3(-1, 0, 0);
                MoteVrV3 x = mv3_norm(mv3_cross(mv3(0, 1, 0), n));
                S.scene.hud_rot = mq_from_axes(x, mv3_cross(n, x), n);
                S.scene.hud_pos = pos;
                S.scene.hud_visible = 1;
                /* the shared tail below re-derives rot from the head, so this
                 * branch finishes the job itself */
                goto hud_done;
            }
        } else {
            /* High and well back. At 45 cm above the cloth just past the rail it
             * sat in the line of any shot played up the table — you were cueing
             * through the scoreboard. Above head height when you are down on the
             * ball, and half a metre clear of the cushion, it cannot be in the
             * way of anything and you glance up for it. */
            /* Past whichever end is AWAY from you, not a fixed end. A board
             * bolted to one end of the table is behind your head for half the
             * frame, and "always visible" has to mean always: walk round to the
             * far end and the board moves to the end you are now looking at, the
             * way you would turn a real one round. */
            MoteVrV3 ht = cuevr_room_to_table(&S.setup.place, t->head.p);
            float end = hud_end_sign(ht.x) * (S.tab.half_len + 0.55f);
            pos = cuevr_table_to_room(&S.setup.place, (Vec3){ end, 0.0f, 0.0f });
            pos.y += 0.95f;
            /* Sized by how far away it is, so it subtends the same angle from
             * anywhere on the table. A fixed 44 cm board is comfortable from the
             * baulk end of a 7 ft table and unreadable from the far end of a 12
             * ft one — and the whole point of a scoreboard is that a glance is
             * enough. Bounded so it neither becomes a postage stamp nor fills the
             * room. */
            float d = mv3_len(mv3_sub(t->head.p, pos));
            float w = d * 0.34f;
            S.scene.hud_w = w < 0.45f ? 0.45f : (w > 1.7f ? 1.7f : w);
        }
        S.scene.hud_pos = pos;
        MoteVrV3 to_head = mv3_sub(t->head.p, pos);
        if (!asking) to_head.y = 0.0f;
        MoteVrV3 z = mv3_len(to_head) > 1e-3f ? mv3_norm(to_head) : mv3(0, 0, 1);
        MoteVrV3 x = mv3_norm(mv3_cross(mv3(0, 1, 0), z));
        if (mv3_len(x) < 0.05f) x = mv3(1, 0, 0);
        x = mv3_norm(x);
        S.scene.hud_rot = mq_from_axes(x, mv3_cross(z, x), z);
        S.scene.hud_visible = 1;
        hud_done: ;


        /* The cue you are choosing, LYING ON THE TABLE.
         *
         * Picking one off a list of names with nothing to look at is choosing
         * blind: the rack differs in the shaft timber, the four-point splice,
         * the veneer flash and the butt, and none of that is in the words
         * "ASH & EBONY".
         *
         * It used to hang in the air below the panel, which read as a cue
         * floating in your room. On the cloth is where a cue between frames
         * actually is — laid along the table, butt at the baulk end, off the
         * cushions — and it is somewhere you are already looking, at a distance
         * that shows the whole length of it.
         *
         * Still turning slowly: all the asymmetry is on one face, so a static
         * cue hides half of what you are choosing between.
         *
         * Placed in TABLE space and carried out to the room, so it lies with the
         * table wherever the table has been put. */
        if (S.state == ST_MENU || S.state == ST_APPEAR || S.state == ST_CLOTH) {
            float hl = S.tab.half_len, hw = S.tab.half_wid;
            /* Clear of the baulk cushion by a comfortable hand's width, and off
             * to one side so it is not lying across the D and the spots. */
            float bx = -hl + 0.16f;
            float tx = bx + CUEVR_CUE_LEN;
            if (tx > hl - 0.10f) {          /* a short table: centre it instead */
                float over = tx - (hl - 0.10f);
                bx -= over * 0.5f; tx -= over * 0.5f;
            }
            float z  = hw * 0.55f;
            /* Resting ON the cloth: the butt is the thick end, so the axis sits
             * a butt-radius up rather than at y = 0. */
            const float LIE = 0.016f;
            /* BUTT TOWARD THE PLAYER. The table is sited with +X pointing away
             * (see the yaw taken from the head direction at setup), so the butt
             * goes at the far end of the run and the tip points back down the
             * table — which puts the thick end, the splice and the badge, the
             * parts you are actually choosing between, nearest your eye. */
            Vec3 bt = { tx, LIE, z }, tp = { bx, LIE, z };
            S.scene.cue_visible = 1;
            S.scene.cue_butt = cuevr_table_to_room(&S.setup.place, bt);
            S.scene.cue_tip  = cuevr_table_to_room(&S.setup.place, tp);
            S.menu_cue_roll += dt * 0.55f;
            if (S.menu_cue_roll > 6.2831853f) S.menu_cue_roll -= 6.2831853f;
            S.scene.cue_roll = S.menu_cue_roll;
        }
    }

    S.scene.hands_valid = t->hand[MOTE_VR_LEFT].tracked && t->hand[MOTE_VR_RIGHT].tracked;
    S.scene.hand[0] = t->hand[MOTE_VR_LEFT].pose;
    /* PHYSICAL. This is where the controller MODELS are drawn, and a model has
     * to appear on the controller it is a model of. Swept up with the input
     * reads, it put both models on the dominant hand and a left-hander watched
     * two controllers stack up in one hand. */
    S.scene.hand[1] = t->hand[MOTE_VR_RIGHT].pose;
    /* NO BRIDGE MARKER. It was a small pale quad sitting at the point the cue
     * rests on the bridge hand, put there so the rest adjustment had something
     * to show for itself. In the headset it reads as a square of nothing
     * floating past the controller while you cue, and the cue lying in your
     * hands already shows you where the bridge is. */
    S.scene.rest_visible = 0;
    S.scene.rest_pos = S.cue.bridge;

    /* Frame timing. Accumulated here rather than in the renderer because this is
     * the whole frame — tracking, planning, physics and draw — which is what the
     * headset actually has to deliver inside 13.9 ms. */
    if (dt > 0.0f) {
        S.fps_acc += dt;
        S.fps_n++;
        if (dt > S.fps_worst) S.fps_worst = dt;
        S.fps_win += dt;
        if (S.fps_win >= 0.5f) {
            S.fps_show = S.fps_n > 0 ? (float)S.fps_n / S.fps_acc : 0.0f;
            S.fps_low  = S.fps_worst > 1e-6f ? 1.0f / S.fps_worst : 0.0f;
            S.fps_acc = S.fps_win = S.fps_worst = 0.0f;
            S.fps_n = 0;
            S.hud_dirty = 1;
        }
    }

    /* Ask the runtime for its controller models until it has them.
     *
     * Not once at start-up: a model does not exist until the runtime has seen
     * that controller, and on a headset whose controllers wake on motion that
     * can be seconds after the app is up. mote_xr gives up on its own after
     * enough refusals, so this costs nothing on a runtime that will never
     * answer, and CUEVR_NO_RENDER_MODEL keeps the baked proxies for comparing
     * the two on hardware. */
    {
        static int rm_off = -1;
        if (rm_off < 0) rm_off = getenv("CUEVR_NO_RENDER_MODEL") ? 1 : 0;
        if (!rm_off) {
            for (int h = 0; h < 2; h++) {
                if (cuevr_render_has_ctrl_model(h)) continue;
                uint32_t n = 0;
                void *b = mote_xr_render_model_take(h, &n);
                if (b) {
                    cuevr_render_set_ctrl_model(h, b, n);
                    free(b);
                }
            }
        }
    }

    /* Save what the player has set, when it changes and no more often — the
     * cloth height they matched to a real surface, the bridge they make, where
     * they grip, and what they chose to play. All of it is a property of the
     * player rather than of the frame, so none of it should have to be redone. */
    {
        static CueVrPrefs last; static float since;
        /* FROM THE RECORDS, not from an uninitialised local.
         *
         * This was a bare `CueVrPrefs now;` filled in field by field, and it
         * missed one: brk_tier was never assigned, so every save wrote whatever
         * was on the stack as the player's 20/30/50/100 break counts. They were
         * not summed or double-counted — they were never counts at all, which
         * is why they looked absurd.
         *
         * S.stats IS the loaded record set, so starting from it carries every
         * statistic across by construction and the settings below overwrite the
         * few fields that are settings. A field added to CueVrPrefs from now on
         * is saved whether or not anybody remembers to add a line here, which
         * is the actual bug: a list you have to maintain by hand. */
        CueVrPrefs now = S.stats;
        now.table_height = S.setup.place.height;
        now.rest = S.cue.rest;
        now.grip = S.cue.grip;
        now.table_kind = (int)S.tab.kind;
        now.ballset = S.ballset; now.persona = S.persona;
        now.cloth = S.cloth_idx; now.frame = S.frame_idx;
        now.refvoice = S.ref_voice;
        now.opp = S.opp; now.cue = S.cue_idx;
        now.light = S.light_idx; now.body = S.body_idx;
        for (int i = 0; i < 3; i++) {
            now.ctrl_pos[i] = S.cal_pos[i]; now.ctrl_rot[i] = S.cal_rot[i];
        }
        for (int a = 0; a < CUEVR_STAT_SNK;  a++)
            for (int b = 0; b < 2; b++) now.snk_best[a][b] = S.stats.snk_best[a][b];
        for (int a = 0; a < CUEVR_STAT_POOL; a++)
            for (int b = 0; b < 2; b++) now.pool_clear[a][b] = S.stats.pool_clear[a][b];
        for (int b = 0; b < 2; b++) {
            now.frames_won[b] = S.stats.frames_won[b];
            now.frames_played[b] = S.stats.frames_played[b];
        }
        now.lefty = S.lefty;
        now.cue_spots = S.cue_spots;
        now.prac_respot = S.prac_respot;
        now.surround = S.surround;
        now.cut_cr = S.cut_cr; now.cut_cs = S.cut_cs;
        now.cut_mr = S.cut_mr; now.cut_ms = S.cut_ms;
        memcpy(now.mini_best, S.mini_best, sizeof now.mini_best);
        now.stick_swap = S.stick_swap;
        now.inv_slide = S.inv_slide; now.inv_turn = S.inv_turn;
        /* Floats compared with a tolerance, not bit-for-bit: the table height is
         * being recomputed every frame while the player is levelling, and an
         * exact compare would rewrite the file once a second for ever on a value
         * that had not really moved. */
        int changed =
            fabsf(now.table_height - last.table_height) > 0.0005f ||
            fabsf(now.rest.x - last.rest.x) > 0.0005f ||
            fabsf(now.rest.y - last.rest.y) > 0.0005f ||
            fabsf(now.rest.z - last.rest.z) > 0.0005f ||
            fabsf(now.grip - last.grip) > 0.0005f ||
            now.table_kind != last.table_kind || now.ballset != last.ballset ||
            now.persona != last.persona || now.cloth != last.cloth ||
            now.frame != last.frame || now.opp != last.opp ||
            now.cue != last.cue || now.light != last.light ||
            now.body != last.body || now.lefty != last.lefty ||
            now.stick_swap != last.stick_swap || now.cue_spots != last.cue_spots ||
            now.inv_slide != last.inv_slide || now.inv_turn != last.inv_turn ||
            fabsf(now.ctrl_pos[0] - last.ctrl_pos[0]) > 0.0002f ||
            fabsf(now.ctrl_pos[1] - last.ctrl_pos[1]) > 0.0002f ||
            fabsf(now.ctrl_pos[2] - last.ctrl_pos[2]) > 0.0002f ||
            fabsf(now.ctrl_rot[0] - last.ctrl_rot[0]) > 0.02f ||
            fabsf(now.ctrl_rot[1] - last.ctrl_rot[1]) > 0.02f ||
            fabsf(now.ctrl_rot[2] - last.ctrl_rot[2]) > 0.02f ||
            S.stat_dirty;
        since += dt;
        if (changed && since > 1.0f) {
            cuevr_prefs_save(&now);
            last = now;
            since = 0.0f;
            S.stat_dirty = 0;
        }
    }

    /* One line whenever the flow moves. Cheap, and the scripted harness is the
     * only eyes this build has on the state machine. */
    {
        static int last_state = -1;
        if (S.state != last_state) {
            /* cuevr_app_state_name(), not a SECOND list of the same names.
             * There were two, and only one of them had the assert that stops a
             * new screen being added without a name — so this one silently went
             * five screens stale and logged the challenge gallery as "?", while
             * the guarded one a few thousand lines up was perfectly correct.
             * Two copies of a table like this is one copy too many by
             * definition: the duplicate exists only to go wrong. */
            LOGI("[cuevr] f%d state -> %s", S.dbg_frame, cuevr_app_state_name());
            last_state = S.state;
        }
    }

    /* No per-frame repaint any more: the pointer is a beam in the room, drawn
     * fresh every frame by the renderer, and the panel only changes when the
     * highlighted row does — which dirties it from its own site. Repainting a
     * 400 KB texture at 72 Hz to move a cursor was a tax on the one thing the
     * Quest is short of. */
    if (S.hud_dirty) { hud_build(); cuevr_render_hud(S.hud); S.hud_dirty = 0; }
}

/* Multiview: one pass, both eyes. The XR host calls this instead of app_draw_eye
 * when the runtime has GL_OVR_multiview2 — half the draw calls and half the vertex
 * work, which on a Quest is the single biggest structural win available. */
static void app_draw_views(void *u, const float *view2, const float *proj2, int draw_room) {
    (void)u;
    cuevr_render_views(view2, proj2, &S.scene, draw_room);
}

static void app_draw_eye(void *u, const float *view, const float *proj, int draw_room) {
    (void)u;
    cuevr_render_eye(view, proj, &S.scene, draw_room);
}

static void app_gl_shutdown(void *u) { (void)u; cuevr_audio_close(); cuevr_render_shutdown(); }

/* CUEVR_START=<CueGameKind> — rack and play, straight away.
 *
 * The scripted stick-walk through the main menu stopped working the moment the
 * menu became a pointer, and a harness that silently photographs the menu
 * instead of a frame is worse than no harness. This does not go through the
 * menu at all, so it cannot drift with it. */
/* CUEVR_NET=host|join, without the menu.
 *
 * The scripted stick-walk into the lobby stopped working the moment the menus
 * became pointers — the same failure MR_START_STEPS had, for the same reason:
 * a script that drives the UI drifts with the UI. Two ends that have never
 * talked to each other is not networking, it is hope, so the test has to keep
 * working. */
void cuevr_app_force_net(int join) {
    S.opp = OPP_ONLINE;
    S.break_first = coin_toss();
    /* CUEVR_BREAK=0|1 pins the toss, so a two-instance test can run BOTH ways
     * round rather than whichever way the coin happened to land. The host's is
     * the one that counts, so setting it on the host is enough — but a test that
     * sets it on both is testing what it thinks it is. */
    { const char *v = getenv("CUEVR_BREAK"); if (v) S.break_first = atoi(v) ? 1 : 0; }
    /* CUEVR_GAME=<CueGameKind> picks the table for an online test. The host's
     * choice is the one both ends rack, so a test that sets it on the JOINER as
     * well is checking that too. Snooker matters here because it is the only
     * game with nomination, free balls and a foul decision — three things that
     * cross the wire and nothing else exercises. */
    { const char *v = getenv("CUEVR_GAME");
      if (v) { int k = atoi(v); for (int i = 0; i < MENU_N; i++)
                   if ((int)MENU[i].kind == k) S.menu_sel = i; } }
    /* CUEVR_MATCH=n forces this end's match length, so a mismatched pair proves
     * the host's number is the one both play to. */
    { const char *v = getenv("CUEVR_MATCH");
      if (v) { int m = atoi(v); for (int i = 0; i < (int)(sizeof MATCH_LEN / sizeof MATCH_LEN[0]); i++)
                   if (MATCH_LEN[i] == m) S.match_idx = i; } }
    cuevr_net_set_hello((int)MENU[S.menu_sel].kind, S.cue_idx, S.break_first,
                        MATCH_LEN[S.match_idx]);
    S.lb_screen = LB_WAIT;
    S.state = ST_LOBBY;
    if (join) cuevr_net_lan_join(); else cuevr_net_lan_host();
    S.hud_dirty = 1;
}

void cuevr_app_force_start(int kind) {
    for (int i = 0; i < MENU_N; i++)
        if ((int)MENU[i].kind == kind) S.menu_sel = i;
    cue_render_set_ball_set(S.ballset);
    /* A capture wants the same frame every time, so the toss is fixed — and
     * CUEVR_BREAK=1 hands it to the CPU, which is the only way from a script
     * to watch the opponent play a shot. */
    S.break_first = 0;
    { const char *v = getenv("CUEVR_BREAK"); if (v) S.break_first = atoi(v) ? 1 : 0; }
    start_frame((CueGameKind)kind);
    hand_over();
}

void cuevr_app_describe(MoteXrApp *out) {
    memset(out, 0, sizeof *out);
    out->name        = "CueVR";
    out->floor_relative = 1;      /* a table stands on the floor */
    out->gl_init     = app_gl_init;
    out->update      = app_update;
    out->draw_eye    = app_draw_eye;
    out->draw_views  = app_draw_views;
    out->gl_shutdown = app_gl_shutdown;
    /* The GPU is the wall on this app (measured on device), and the eye render
     * scale is the cheapest lever on it — see CUEVR_RENDER_SCALE in cuevr.h.
     *
     * NOT an env var: an Android app inherits its environment from zygote, so
     * every CUEVR_* getenv in this codebase is host-preview only and reads as
     * unset in the APK. Device knobs have to be constants or menu items.
     *
     * NO foveated rendering, at the user's direction (twice): in cue sports
     * you sight along the cue to balls at the edge of the view — the
     * periphery is precisely where the eye goes, so FFR blurs the one thing
     * that must stay sharp. The host plumbing exists but this app never asks. */
    out->render_scale = CUEVR_RENDER_SCALE;
    out->foveation    = 0;
}
