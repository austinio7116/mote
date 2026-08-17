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
    /* And what it cost, in snooker, where a foul has a price: 4 to 7 depending
     * on the ball on and what was hit or potted. The referee reads it out after
     * the call — "Foul. Five." — and there was no way to know the number from
     * outside, because the same foul flag covers a four and a seven. Zero in
     * the games where a foul is paid for in ball-in-hand rather than points. */
    int last_foul_pts;
    char msg[24];

    /* 8-ball */
    int group[2];        /* 0 = open, 1 = low(1-7), 2 = high(9-15) */
    int open;
    int break_shot;
    int shots_remaining; /* UK two-shot rule: shots left in this visit (1 or 2) */
    int two_shot;        /* opponent is on the two-shot carry from a foul */
    int free_shot;       /* first of the two shots — informational */
    /* WHICH UK 8-BALL IS BEING PLAYED. 0 = the pub game (WEPF world rules): a
     * foul gives the opponent TWO shots and the cue ball stays where it lies,
     * and there is no obligation to reach a cushion. 1 = international
     * (blackball): a foul gives ball in hand anywhere, and a shot that pots
     * nothing must send some ball to a cushion.
     *
     * It lives in CueRules rather than CueTable because it is a rule, not a
     * table — the same 7 ft bed plays both — and because the whole struct
     * crosses the wire in a match, so putting it here syncs it for free. */
    int uk_intl;         /* CUE_UK_* — see below. Kept as a plain int so the
                          * struct still crosses the wire unchanged. */

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

    /* ---- straight pool, 14.1 continuous ----
     * `nominated` carries the CALLED BALL here — its id, 1..15 — rather than a
     * snooker colour value, which is why the field is reused rather than a
     * second one added: it is the same act, naming what you are going for. Zero
     * means nothing was called, and in 14.1 that is not an oversight but a
     * DECLARED SAFETY: play on, score nothing, hand the table over. */
    int called_pocket;   /* pocket index called with the ball, or -1 for none */
    int target_score;    /* points that take the frame. 50 here, because a VR
                          * frame wants to end inside a session; 100 and 150 are
                          * the tournament numbers and cue_rules_set_target sets
                          * them. */
    int racks;           /* reracks so far this frame — the HUD says "RACK 3" */
    /* Set on resolve, consumed by the host, exactly as ball_in_hand is: the
     * rules can see that only one object ball is left but cannot lay a triangle
     * out, because they hold no CueTable. 1 = rack the fourteen and leave the
     * apex empty, 2 = rack all fifteen (the table was cleared outright, or a
     * third consecutive foul). */
    int rerack;

    /* ---- G2: RUSSIAN PYRAMID -------------------------------------------- *
     *
     * Scored in BALLS rather than points, and the count is the whole game: any
     * of the fifteen may be potted, in any order, and the first player to eight
     * has more than half of them and cannot be caught. `score` carries it, so a
     * host that already draws two numbers draws these.
     *
     * The penalty is what makes it a different game from pool with no groups: a
     * foul RETURNS ONE OF THE OFFENDER'S OWN POTTED BALLS to the table, so a
     * frame can go backwards and a careless player hands his opponent a ball to
     * shoot at. `respot` is set on resolve and consumed by the host, exactly as
     * ball_in_hand and rerack are — the rules cannot place a ball because they
     * hold no table.
     *
     * `pyr_free` is the variant flag, and it is here for the same reason
     * uk_intl is: it is a rule rather than a table, and the whole struct crosses
     * the wire. 0 is CLASSIC, the game shipped first. */
    int respot;          /* put this many of the striker's potted balls back */
    int pyr_free;        /* CUE_PYR_* — see below */
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

/* Which pyramid. CLASSIC is the white-cue-ball game: pot the objects, eight
 * wins, and the cue ball down a pocket is a foul. COMBAT also scores a cue ball
 * potted OFF an object ball (a "свой"), which is the shot the game is famous
 * for. FREE lets any ball on the table be played as the cue ball, which breaks
 * an assumption balls[0] carries through the rules, the AI and the wire — so it
 * is named here and not yet implemented, rather than pretended about. */
enum { CUE_PYR_CLASSIC = 0, CUE_PYR_COMBAT = 1, CUE_PYR_FREE = 2 };

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
/* The three UK 8-ball rule sets.
 *
 * PUB is the pub game: a foul hands over two visits and the cue ball stays
 * where it lies, and a shot need not reach a cushion.
 *
 * INTERNATIONAL is the International 8-Ball ruleset — one visit with ball in
 * hand anywhere, and a shot that pots nothing must reach a cushion. It is what
 * the WEPF and the EPA have both adopted.
 *
 * ULTIMATE is that same ruleset as the Ultimate Pool Group play it, plus their
 * tournament addition: the GOLDEN BREAK. Pot the black off the break and the
 * frame is won there and then; pot the black and the cue ball, or foul while
 * doing it, and it is lost — the golden duck. Everything else is identical to
 * International, which is why it is a flag on top of it rather than a third
 * body of rules. */
enum { CUE_UK_PUB = 0, CUE_UK_INTL = 1, CUE_UK_ULTIMATE = 2 };

/* Pick which UK 8-ball is being played, before the break. 0 = pub (two shots
 * on a foul, no cushion requirement), 1 = international (ball in hand, and a
 * shot that pots nothing must reach a cushion). Ignored by every other game. */
void cue_rules_set_uk(CueRules *r, int ruleset);   /* CUE_UK_* */

/* May the cue ball be placed anywhere on the table, or only in the D?
 * Snooker is always the D. The English table follows its rule set: the D under
 * pub rules, the whole cloth under International and Ultimate Pool. Every other
 * pool game is the whole cloth. */
static inline int cue_rules_in_hand_anywhere(const CueRules *r) {
    if (!r || r->kind) return 0;                    /* snooker: the D */
    if (r->mode == CUE_GAME_UK8) return r->uk_intl != CUE_UK_PUB;
    return 1;
}
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

/* ---- straight pool ---------------------------------------------------- *
 * CALL THE SHOT: the ball you are going for and the pocket you are putting it
 * in. Both are required for a scoring stroke — pot the called ball in the
 * called pocket and every ball that went down on that stroke counts, one point
 * each, and you stay at the table.
 *
 * Calling nothing (ball_id 0) is a DECLARED SAFETY and a legal, ordinary thing
 * to do: the stroke must still reach a cushion or pocket a ball, nothing scores,
 * anything potted is spotted, and the table passes. It is not a foul, and it is
 * how most of a real 14.1 frame is played.
 *
 * The call lasts one stroke and is cleared by the resolve, whatever happened. */
void cue_rules_call_shot(CueRules *r, int ball_id, int pocket);

/* Points that win the frame. 14.1 is played to a target across as many racks as
 * it takes — 150 in a world championship, 100 commonly, 50 here by default. */
void cue_rules_set_target(CueRules *r, int points);
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
