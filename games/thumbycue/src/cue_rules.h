/*
 * ThumbyCue — rules & scoring for 8-ball and snooker. Ported (simplified) from
 * the 2D game's game.js. Driven by the shot-resolve step in cue_game.
 */
#ifndef CUE_RULES_H
#define CUE_RULES_H

#include "cue_physics.h"
#include "cue_table.h"

typedef struct {
    int kind;            /* 0 = pool (8/9-ball), 1 = snooker */
    int mode;            /* CueGameKind: UK8/US8/US9/SNK10/SNK15 */
    int cfoul[2];        /* 9-ball: consecutive fouls per player */
    int cpu;             /* player 1 (index 1) is the CPU */
    int turn;            /* 0 or 1 — whose shot */
    int score[2];        /* snooker points */
    int frame_over, winner;
    int ball_in_hand;    /* set on resolve; consumed by cue_game */
    float R;             /* ball radius (m) — for line-of-sight / snooker tests */
    /* Was the shot that just resolved a foul? Every code path already knows —
     * each resolver computes it and prices it — but none of them said so, and
     * "the opponent's score went up" is only a foul signal in snooker. A host
     * keeping records has no other way to count them. */
    int last_foul;
    /* And whether a MISS was called with it, which is a different call from the
     * referee and a different thing in the rules — a foul off a correct first
     * contact is not a miss. The host has no other way to know: the penalty is
     * the same number either way. */
    int last_miss;
    char msg[24];

    /* 8-ball */
    int group[2];        /* 0 = open, 1 = low(1-7), 2 = high(9-15) */
    int open;
    int break_shot;
    int shots_remaining; /* UK two-shot rule: shots left in this visit (1 or 2) */
    int two_shot;        /* opponent is on the two-shot carry from a foul */
    int free_shot;       /* first of the two shots — informational */

    /* snooker */
    int target;          /* 0 = red, 1 = a colour, 2 = clearance sequence */
    int seq;             /* clearance: value of the colour on (2..7) */
    /* Which colour the striker is on, as its value 2..7, or 0 for "not yet
     * nominated". Only meaningful while target == 1. Without it ANY colour was
     * legal after a red, so there was no wrong colour to pot and no nomination
     * for a foul to be priced against — a red then the black carried the same
     * risk as a red then the yellow. */
    int nominated;
    int reds_left;
    int brk;             /* current break points */
    Vec3 spot[8];        /* colour spots indexed by value 2..7 */
    float baulk_x, d_radius;  /* the D — a free ball after a scratch is judged
                               * from every position in it, so the rules need
                               * its geometry as well as the renderer. */

    /* 9-ball push-out (WPA) */
    int pushout_avail;   /* the next shot (first after the break) may be a push-out */
    int pushout_offer;   /* pending: ask the player at the table whether to push out */
    int is_pushout;      /* the shot just played / about to be played is a push-out */
    int pushout_resp;    /* pending: opponent decides play-from-here / pass-back */

    /* snooker foul-and-a-miss + free ball (WPBSA) */
    int was_snookered;   /* striker had NO clear ball-on before the shot (set by cue_game) */
    /* ---- what the HOST saw, set before cue_rules_resolve and cleared by it ----
     * Same contract as was_snookered above: the rules cannot see these for
     * themselves because they happen outside the settle. */
    int jumped;          /* the stroke was a JUMP SHOT by WPBSA Definition 20 —
                          * A foul in snooker — you may not jump a ball — and
                          * perfectly legal in pool, which is why it is a flag
                          * here rather than a decision the physics makes. */
    int n_off;           /* how many of the ids in `potted` were driven OFF THE
                          * TABLE rather than pocketed. They come through the
                          * potted list on purpose, so every consequence of that
                          * ball leaving still fires — a colour respots, the
                          * black off the table loses the frame, the 9 is
                          * spotted — and this only adds the one thing potting
                          * does not carry and going off the table always does,
                          * which is the foul. */
    int free_ball;       /* this shot is played under a free-ball award */
    int free_ball_id;    /* WHICH ball was nominated as it, or 0 for any */
    int cmiss[2];        /* consecutive misses per player (3 = frame forfeit) */
    int decision;        /* pending opponent decision after a snooker foul (CUE_DEC_*) */
    int dec_can_restore; /* a "miss" was called → opponent may force a replay */
    int dec_free_ball;   /* opponent is snookered → free ball available */
    int dec_scratch;     /* the foul was a scratch (cue potted) */
    int dec_offender;    /* player who committed the foul */
    int dec_penalty;     /* penalty already awarded (for restore re-apply) */

    /* ---- the match, not the frame ----
     * A frame is one rack; a match is the best of N of them. Everything above
     * resets per frame, everything here carries. */
    int frames[2];       /* frames won */
    int break_first;     /* who broke the FIRST frame — the alternation starts here */
    int best_of;         /* 1 = a single frame, else an odd number */
    int match_over, match_winner;
    int conceded;        /* the frame was given up rather than played out */
} CueRules;

/* decision codes. CUE_DEC_PENDING is parked in r->decision after a snooker foul
 * that offers a choice; the host then passes a PLAY/REPLAY/FREEBALL back. */
/* PLAY     — I play the balls as they lie.
 * AGAIN    — you play again, from where they lie. Available after ANY foul,
 *            and the option that was missing: REPLAY restores the layout,
 *            which is a different thing and only offered after a called miss.
 * REPLAY   — put the balls back and play the stroke again. Miss only.
 * FREEBALL — I play, and I am snookered, so I may nominate a free ball. */
enum { CUE_DEC_NONE = 0, CUE_DEC_PENDING, CUE_DEC_PLAY, CUE_DEC_AGAIN,
       CUE_DEC_REPLAY, CUE_DEC_FREEBALL };

void cue_rules_init(CueRules *r, const CueTable *t, int cpu);
/* Re-rack for the next frame of the same match: the frame state resets, the
 * frame tally and the match length do not. */
void cue_rules_next_frame(CueRules *r, const CueTable *t);

/* Give the frame up. The opponent takes it, and the match tally moves with it —
 * which is the whole reason a snooker player concedes rather than potting out a
 * frame they cannot win. */
void cue_rules_concede(CueRules *r, int player);

/* Should `player` concede? Ported from the 2D game: they need snookers, and
 * there are not enough of them left on the table to get. */
int  cue_rules_should_concede(const CueRules *r, int player);

/* Nominate the colour `value` (2..7) as the ball on. Ignored unless the striker
 * is on a colour in the reds phase. */
/* Who breaks the first frame. The break used to be player 0's, always — the
 * human's against the CPU, the host's online, every frame of every match. Set
 * this after cue_rules_init and the per-frame alternation follows from it. */
void cue_rules_set_break(CueRules *r, int who);

/* Put a potted colour back on, by the respot rule. Public because a practice
 * table wants the colours kept on it whatever the sequence says, and the host
 * has no other way to ask for the same placement logic. */
void cue_rules_respot(CueRules *r, CueBall *b, int n, int id);

void cue_rules_nominate(CueRules *r, int value);
/* Name the ball being taken as the free ball. Ignored unless one is awarded. */
void cue_rules_nominate_free(CueRules *r, int id);

/* True if the player to strike has NO full-ball clear path to any ball-on
 * (used pre-shot to flag snookers for the miss / free-ball rules). */
int  cue_rules_is_snookered(const CueRules *r, const CueBall *b, int n);

/* Apply the opponent's choice after a snooker foul (CUE_DEC_*). On CUE_DEC_REPLAY
 * the caller must restore the pre-shot ball layout first. Returns the next
 * player to shoot. */
int  cue_rules_apply_decision(CueRules *r, int decision);

/* Resolve a completed shot. balls[]/n is the post-shot table state (potted
 * balls have on=0). first_hit = id of the first object ball the cue contacted
 * (-1 if none). potted[] = ids potted this shot. May respot snooker colours
 * (sets balls[].on=1 + position). */
void cue_rules_resolve(CueRules *r, CueBall *balls, int n, const CueWorld *w,
                       int first_hit, int cue_scratch, int cushion_seen,
                       const int *potted, int npotted);

/* Is `id` a legal ball to go for right now (used by the CPU planner)? Needs the
 * ball array so the 8 is only legal once the shooter's group is cleared. */
int  cue_rules_ball_legal(const CueRules *r, const CueBall *b, int n, int id);

/* Short status line for the HUD (group / ball-on). */
void cue_rules_status(const CueRules *r, char *buf, int cap);

#endif
