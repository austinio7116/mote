/*
 * ThumbyCue — rules & scoring for 8-ball and snooker. Ported (simplified) from
 * the 2D game's game.js. Driven by the shot-resolve step in cue_game.
 */
#ifndef CUE_RULES_H
#define CUE_RULES_H

#include "cue_physics.h"
/* CueTable, for the two games whose rules have to ask the table a question:
 * billiards for its four marks, bar billiards for its baulk arc. */
#include "cue_table.h"
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
    /* PAUL: how many points are still on the table. It decides the frame — a
     * lead bigger than this ends it — so it is kept where the board and the
     * planner can both read it rather than being recounted by each of them from
     * the ball array, which is how two answers to one question get about. */
    int paul_left;
    Vec3 spot[8];        /* colour spots indexed by value 2..7 */
    /* WHICH WAY IS UP THE TABLE at the top spots, so a spot that is occupied
     * can be walked away from along the table rather than along world +x. On a
     * rectangle those are the same direction and this is (1,0,0); on an L the
     * layout turns the corner, and +x from the black's spot walks into the
     * missing quadrant — which is a respotted colour placed inside the cushion,
     * or off the cloth entirely. Set from the table at init, because the rules
     * hold no table afterwards. */
    Vec3 spot_up;
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

    /* WPA 10-BALL IS A CALL-SHOT GAME, and this switches that half on. Off (the
     * default, and the pre-2.0 behaviour) slop counts and the guide says so.
     * On: every stroke after the break carries a called ball and pocket, a
     * stroke whose call was not made passes the table with everything staying
     * down except the 10 — which respots, WPA 9.5 — and the 10 itself only
     * wins AS CALLED. The break is never a called shot. */
    /* AND THE 8-BALL GAMES CALL TOO, at two strengths, because the codes
     * differ: WPA 8-ball calls ball and pocket on every stroke after the
     * break, while Chinese 8-ball exempts the obvious shots and asks only
     * that the BLACK be called — which is also what most pub tables play.
     *   0  no calling
     *   1  the black only
     *   2  every stroke after the break
     * Ten-ball uses 2 or nothing; the level is the same field so one host
     * row can drive it. */
    int call_shot_on;
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
    /* ...AND WHICH ONES, WHERE IT MATTERS.
     *
     * The count alone is Russian pyramid's question, because its balls are
     * interchangeable and giving one back is giving any back. It is the wrong
     * question everywhere else: cue_table_respot_one hands back the LOWEST id
     * that is off the table, and in One Pocket, bank pool and Honolulu that is
     * very often a ball the striker legitimately SCORED — pot the 3 into your
     * own pocket and the 5 into a neutral one, and the count alone brings the
     * 3 back and leaves the 5 down.
     *
     * So the resolvers that know which ball name it. Zero means "any", which
     * is pyramid's answer and cowboy's, where every potted ball comes back
     * anyway. Cleared by resolve like bb_hole. */
    unsigned char respot_id[8];
    int pyr_free;        /* CUE_PYR_* — see below */

    /* ---- G5: ENGLISH BILLIARDS ------------------------------------------ *
     *
     * Scored in points and played with three balls, two of which are cue
     * balls. Index 0 of the ball array is whichever of them is being struck;
     * `bil_yellow` says which side that is, so the host can exchange the two
     * balls at a change of turn and a scoreboard can say who is on.
     *
     * The two counters are the official limits, and they are limits on a
     * SEQUENCE rather than on a total, which is why they are state:
     * Section 3 Rule 9 caps consecutive cannons at seventy-five, Rule 10 caps
     * consecutive hazards at fifteen strokes, and exceeding either is a foul
     * (Rule 14(j) and (k)). Both reset the moment a stroke scores the other
     * kind, because the rules count each "not in conjunction with" the other.
     *
     * `bil_spot_pots` is Rule 8(b)/(c): the red goes back on the Spot twice,
     * then the Centre Spot once, for CONTINUED pots of the red not in
     * conjunction with another score. It counts pots of the red in the current
     * break and nothing else resets it but a stroke that scores otherwise. */
    int bil_yellow;      /* the striker is playing with the yellow */
    /* SNOOKER SHOOTOUT: the one-frame TV format, as a switch on the snooker
     * games. Rules-side it changes three things: every stroke must pot or
     * reach a cushion; every foul is ball in hand (anywhere, not the D); and
     * there is no miss rule and no foul decisions. The clocks and the
     * blue-ball tie-break are the host's. */
    int snk_shootout;
    int bil_cannons;     /* consecutive cannons, not with a hazard */
    int bil_hazards;     /* consecutive hazard strokes, not with a cannon */
    int bil_spot_pots;   /* consecutive pots of the red off a spot */
    /* Set on resolve and consumed by the host, exactly as ball_in_hand is: the
     * rules cannot place a ball because they hold no table. 0 = nothing to do.
     * CUE_BIL_SPOT_* says WHICH mark the red goes back on, because the rules
     * know the sequence and only the table knows where the marks are. */
    int bil_respot_red;
    /* ...and the same for the object white, which after a foul is placed on
     * the Centre Spot if the next player takes that option (Rule 15(c)(ii)). */
    int bil_respot_white;

    /* ---- G6: BAR BILLIARDS ---------------------------------------------- *
     *
     * Scored in points off nine holes and played against a clock rather than
     * to a target. What makes it its own game is the PENALTY structure: almost
     * every foul costs the break rather than a couple of points, and one of
     * them costs everything you have.
     *
     *   AEBBA Rule 110  loss of the break score: failing to hit a ball, a
     *                   ball coming back over the baulk line or into the D, a
     *                   ball leaving the table, knocking a WHITE skittle over,
     *                   and a cue ball that neither reaches the black peg's
     *                   line nor strikes anything
     *   AEBBA Rule 111  loss of the ENTIRE score: knocking the BLACK skittle
     *                   over
     *   AEBBA Rule 112  and if both go over, whichever fell FIRST decides
     *
     * `bb_break` is the score made since the break started; a foul under 110
     * takes it back off, which is why the running score and the break are kept
     * apart. `bb_time` is what is left on the clock and `bb_barred` is the bar
     * having dropped — after which potted balls do not come back and the game
     * plays itself out. */
    int bb_break;        /* points made in the break in progress */
    float bb_time;       /* seconds left before the bar drops */
    int bb_barred;       /* the bar has dropped: no more balls return */
    int bb_from_break;   /* the next shot is played from the break position */
    int bb_both_potted;  /* consecutive strokes potting both from the break */
    int bb_last_ball;    /* one ball left: the last-ball shot (Rule 108) */
    /* Set on resolve and consumed by the host: how many balls to feed back out
     * of the trough, and whether the red is among them. */
    int bb_return;
    /* HOW MANY BALLS ARE STILL IN THE GAME. Eight at the start. While the bar
     * is up a potted ball drops into the trough and comes back out; once it
     * has dropped they are swallowed, and the game ends when the last one is
     * (Rule 108's premise, and what "the bar drops" is FOR). */
    int bb_left;
    /* ---- what the HOST saw, set before cue_rules_resolve and cleared by it.
     * Same contract as was_snookered: the rules cannot see these for
     * themselves. `bb_hole` is which hole each entry of `potted` went down, in
     * the same order — the scoring IS which hole, and a list of ball ids
     * cannot say, least of all here where seven of the eight balls are
     * identical whites. */
    int bb_hole[8];   /* also One Pocket's: WHICH pocket each pot went down */
    /* ---- ONE POCKET ------------------------------------------------------
     *
     * Each player owns one of the two FOOT corner pockets and scores only into
     * that one; a ball in the other player's pocket scores for THEM, and a ball
     * in any of the other four is spotted. First to eight of the fifteen.
     *
     * `op_hole` is which pocket index belongs to each seat. The rules cannot
     * work it out — a pocket array is in the order the cloth boundary walks it,
     * which is not a fixed numbering and on a workshop table is not a
     * rectangle's order at all — so the host sets it at the rack from
     * cue_table_foot_pockets, the same contract as bb_hole.
     *
     * `op_owed` is the foul debt. A foul costs a ball: one of yours comes back
     * out of the pocket and onto the table, and if you have none scored yet you
     * OWE one, which is taken from the first ball you do score. */
    int op_hole[2];
    int op_owed[2];
    /* ---- SPEED POOL ------------------------------------------------------
     *
     * Not a frame: two attempts at the same task, and the CLOCK is the score.
     * Each player clears a full rack and the lower time takes it, so there is
     * no turn to win and nothing to defend — the opponent is a number.
     *
     * `sp_cs` is each attempt's time in hundredths, written by the HOST before
     * resolve because the rules hold no clock any more than they hold a table.
     * `sp_done` is who has had their rack. Same contract as bb_hole. */
    int sp_cs[2];
    int sp_done[2];
    int bb_in_baulk;     /* a ball came to rest on or inside the baulk arc */
    int bb_short;        /* the cue ball struck nothing and never reached the
                          * line through the black peg (Rule 110(o)) */
    /* ---- BILLIARDS GOLF -------------------------------------------------
     *
     * Not a frame. There is no opponent to take the table from you and no
     * scoring shot: you play the hole until the reds are gone, the STROKES are
     * the score, and low wins. Each player plays the same hole out in turn —
     * that is what a golf card is a record of — and only then does the course
     * move on.
     *
     *   Rule 2  the cue ball down a hole costs a stroke and goes back to the
     *           starting point. It is a penalty, not a foul: nothing changes
     *           hands, because nothing can.
     *   Rule 3  eight strokes is the most a hole can cost. A player who has
     *           not cleared it by then takes an 8 and the other plays.
     *   Rule 4  lowest total over eighteen wins.
     */
    int golf_hole;               /* 0..17, the hole being played */
    int golf_strokes;            /* strokes on it so far, by the player up */
    /* A hole cannot cost more than eight (Rule 3), so a byte holds it and the
     * whole card is 36 of them. It rides inside CueRules over the wire, and
     * four bytes apiece for a number that never exceeds eight is most of a
     * packet spent on nothing. */
    uint8_t golf_card[2][CUE_GOLF_HOLES];  /* 0 = not played yet */
    int golf_done;               /* both have played this hole: move on */
    int golf_rack;               /* set on resolve, consumed by the host: set
                                  * the hole out again for the next player */
    int golf_reset_cue;          /* ...and put the cue ball back on its spot */
    /* A ROUND ON YOUR OWN is a real way to play a course, and the only game
     * here that has one — every other mode always has a second seat even if a
     * person is sitting in both. Set by the host before the first stroke. */
    int golf_solo;
    /* THE HONOUR. In golf the player who took fewest on the last hole plays
     * first on the next, and a tie leaves it where it was — so it is a thing
     * the round REMEMBERS, not something derivable from the card. On the first
     * hole it is drawn, exactly as the break is in every other game here. */
    int golf_honour;
    /* WHICH HOLES THIS ROUND IS: all eighteen, the front nine or the back
     * nine. Nine holes is a real round of golf and a much shorter sitting,
     * which on a table where a hole takes four shots matters more than it does
     * on grass. */
    int golf_round;              /* CUE_GOLF_* below */

    /* 9-ball push-out (WPA) */
    int pushout_avail;   /* the next shot (first after the break) may be a push-out */
    int pushout_offer;   /* pending: ask the player at the table whether to push out */
    int is_pushout;      /* the shot just played / about to be played is a push-out */
    int pushout_resp;    /* pending: opponent decides play-from-here / pass-back */

    /* snooker foul-and-a-miss + free ball (WPBSA) */
    int was_snookered;   /* striker had NO clear ball-on before the shot (set by cue_game) */

    /* ---- FOUL AND A MISS, JUDGED --------------------------------------------
     *
     * WPBSA Section 3 Rule 14 does not call a miss on every failure to hit the
     * ball on: the referee judges whether the striker made a GOOD ENOUGH
     * ATTEMPT. miss_level is the standard that judgement is held to:
     *
     *   0  OFF       every failure to hit is a miss — the pre-2.0 behaviour,
     *                and what the handheld still gets, since it never sets this
     *   1  AMATEUR   a genuine try is enough: within three ball widths, and
     *                most of the pace to get there
     *   2  CLUB      within a ball and a half, at nearly full pace
     *   3  PRO       almost every failure is called, as on television: within
     *                half a ball, and the cue ball must have had the legs to
     *                PASS the ball on
     *
     * The tolerance is not flat: a snookered escape earns three times the room
     * (the referee judges the attempt, not the outcome), and every metre of
     * pre-shot distance earns more again — a long roll up the table is judged
     * kinder than a miss from a foot away.
     *
     * The attempt itself is measured by the two bookends below, on the same
     * host-sets-it contract as was_snookered: cue_rules_attempt_begin before
     * the stroke, cue_rules_attempt_end after the settle, and resolve consumes
     * what they left. A resolve with no attempt data falls back to level 0. */
    int   miss_level;
    float att_gap;       /* how far the nearest ball-on was missed by, ball widths */
    float att_pace;      /* cue-ball path length / pre-shot distance to that ball */
    float att_dist;      /* that pre-shot distance, metres */
    int   att_have;      /* both bookends ran for this stroke */
    float att_pre[CUE_MAX_BALLS];   /* begin: pre-shot surface distance per index */
    unsigned char att_on[CUE_MAX_BALLS]; /* begin: index was a legal ball-on */
    /* ---- what the HOST saw, set before cue_rules_resolve and cleared by it ----
     * Same contract as was_snookered above: the rules cannot see these for
     * themselves because they happen outside the settle. */
    /* C2b: WHICH BALL THE TIP ACTUALLY HIT, by id.
     *
     * The cue is not steered onto the cue ball any more. It goes down the line
     * the player is holding, and if another ball stands on that line the tip
     * reaches that one first — so THAT is the ball that gets struck, which is
     * what really happens when you cue through a ball you had not noticed.
     *
     * 0 for the ordinary case, and for free practice, where it is simply a
     * shot on another ball and nobody is keeping score. Otherwise the id of
     * whatever the tip found, and cue_rules_resolve turns it into that game's
     * own foul: the striker's cue ball was never struck, so no ball can have
     * been struck BY it, and every game already knows what that costs. */
    int cued_id;

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

/* Where a potted red goes back. The order is Section 3 Rule 8: the Spot, and
 * if that is occupied the Pyramid Spot, and if both are occupied the Centre
 * Spot — except under the continued-pots sequence of 8(b), where the third
 * one in a row goes on the Centre Spot instead. The rules name the mark; the
 * host asks the table where it is and applies the occupied walk. */
enum { CUE_BIL_SPOT_NONE = 0, CUE_BIL_SPOT_SPOT, CUE_BIL_SPOT_CENTRE,
       CUE_BIL_SPOT_PYRAMID };

/* ---- BAR BILLIARDS, the things only a host with a table can answer -------
 *
 * The rules judge the stroke; these look at where the balls stopped and what
 * the cue ball managed, which is a question about geometry. Call them after
 * the table settles and BEFORE cue_rules_resolve, exactly as the snooker host
 * sets was_snookered.
 *
 * A ball is in baulk when its centre is on or inside the arc struck about the
 * break spot (Rule 77), or anywhere in the D — Rule 110(c) and (d) make either
 * a foul and send the ball back to the rack. */
int  cue_rules_bb_in_baulk(const CueRules *r, const CueTable *t,
                           const CueBall *b, int n);
/* Rule 110(o): the cue ball must either strike something or reach the line
 * through the black peg. `reached` is how far up the table it got. */
int  cue_rules_bb_short(const CueTable *t, float furthest_x, int hit_something);
/* Rules 110(c),(d): balls over the baulk line or on the D go back to the rack.
 * Called by the host AFTER resolve has read bb_in_baulk. Returns how many. */
int  cue_rules_bb_baulk_return(const CueRules *r, const CueTable *t,
                               CueBall *b, int n);

/* ---- billiards golf ----------------------------------------------------- */
/* A player's total so far, over holes [from..to]; 0 for holes not yet played. */
int  cue_rules_golf_total(const CueRules *r, int who, int from_hole, int to_hole);
/* How the round stands: -1 nobody yet, 0/1 the leader, 2 level. */
int  cue_rules_golf_leader(const CueRules *r);
/* Which holes this round covers — CUE_GOLF_18 / _FRONT9 / _BACK9. Sets the
 * starting hole with it, so nothing else has to know the back nine starts at
 * ten. Call after cue_rules_init, as cue_rules_set_uk is called. */
void cue_rules_set_golf_round(CueRules *r, int round);

/* Matchplay's score: holes up from seat 0's side, + for seat 0 and - for seat 1,
 * counting only holes both players have finished. */
int  cue_rules_golf_holes_up(const CueRules *r);
/* Run the clock down. When it reaches zero the bar drops and potted balls stop
 * coming back (Rule 108's premise); the game is over when the balls run out. */
void cue_rules_bb_tick(CueRules *r, float dt);
/* Set the table up for the next stroke.
 *
 * Rule 91: every shot is played from the D, so there is always a white to
 * place. Rule 92 and 94: with no object ball on the table the stroke is played
 * from the BREAK POSITION — a white on the break spot and the red on the red
 * spot — and play returns there whenever the table empties. While the bar is
 * up a potted ball comes back out of the trough; once it has dropped they stay
 * down and the game plays itself out.
 *
 * Returns 1 if it placed anything. Call before each stroke; the rules cannot,
 * because they hold no table. */
int  cue_rules_bb_setup(CueRules *r, const CueTable *t, CueBall *b, int n);

/* Put the red back where the rules just said, following Rule 8's sequence when
 * that mark is occupied, and clear the request. The rules name the mark and
 * hold its position; only this knows whether a ball is standing on it. Returns
 * 1 if a ball was placed. */
int cue_rules_billiards_respot(CueRules *r, const CueTable *t,
                               CueBall *b, int n);
/* THE STRIKER'S BALL IS ALWAYS INDEX 0. In billiards the two sides play with
 * different balls, so at a change of turn the two whites exchange places in
 * the array — contents, ids and all — and everything downstream that knows
 * index 0 is the cue ball goes on being right. Call it when `turn` changes. */
void cue_rules_billiards_swap(CueBall *b, int n);

/* THE SCORES, from Section 3 Rule 4. A cannon, a pot white and an in-off
 * white are two each; a pot red and an in-off red are three. */
#define CUE_BIL_CANNON 2
#define CUE_BIL_WHITE  2
#define CUE_BIL_RED    3
/* Section 3 Rule 15(c): every foul is two, and never more than two in one
 * stroke however many rules the stroke broke. */
#define CUE_BIL_FOUL   2
/* Section 3 Rules 9 and 10. */
#define CUE_BIL_MAX_CANNONS 75
#define CUE_BIL_MAX_HAZARDS 15

/* ---- bar billiards ------------------------------------------------------
 * Rule 97: a white scores the value of its hole and the red scores double.
 * A coin buys between fifteen and twenty minutes; seventeen is the usual. */
#define CUE_BB_TIME 1020.0f
/* Rule 110(a): potting both balls from the break position four consecutive
 * times is a foul — the player is warned after three and must leave one up. */
#define CUE_BB_MAX_BOTH 3

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
enum { CUE_UK_PUB = 0, CUE_UK_INTL = 1, CUE_UK_ULTIMATE = 2,
       CUE_UK_BLACKBALL = 3 };   /* WPA Blackball Rules 2005 */

/* Pick which UK 8-ball is being played, before the break. 0 = pub (two shots
 * on a foul, no cushion requirement), 1 = international (ball in hand, and a
 * shot that pots nothing must reach a cushion). Ignored by every other game. */
void cue_rules_set_uk(CueRules *r, int ruleset);   /* CUE_UK_* */

/* May the cue ball be placed anywhere on the table, or only in the D?
 * Snooker is always the D. The English table follows its rule set: the D under
 * pub rules, the whole cloth under International and Ultimate Pool. Every other
 * pool game is the whole cloth. */
static inline int cue_rules_in_hand_anywhere(const CueRules *r) {
    if (!r) return 0;
    if (r->kind) return r->snk_shootout;    /* snooker: the D — Shootout: anywhere */
    if (r->mode == CUE_GAME_BARBILLIARDS) return 0; /* Rule 91: the D, always */
    /* PAUL: the D, and only after an in-off. It is played on a snooker table
     * and the D is chalked on it, so that is where the white comes back from —
     * but `kind` above is 0 for Paul (it is not scored as snooker), so it fell
     * through to the pool answer and the white could be put down anywhere. */
    if (r->mode == CUE_GAME_PAUL) return 0;
    /* ROTATION: behind the head string, which is the classic game's penalty and
     * a real one — you may not shoot at anything in front of the line, so a
     * pack sitting at the foot end has to be reached the long way. The FILIPINO
     * game gives ball in hand anywhere instead, which is the single biggest
     * difference between the two and why the same rack plays like a different
     * sport. Region 2 is the full-width rectangle behind the line. */
    if (r->mode == CUE_GAME_ROTATION)    return 2;
    if (r->mode == CUE_GAME_ROTATION_PH) return 1;
    /* Blackball: baulk — the full-width rectangle behind the line, not the D
     * (WPA Blackball 4c/4h). The value 2 is that region to the clamp. */
    if (r->mode == CUE_GAME_UK8)
        return r->uk_intl == CUE_UK_PUB ? 0
             : r->uk_intl == CUE_UK_BLACKBALL ? 2 : 1;
    return 1;
}
/* Re-rack for the next frame of the same match: the frame state resets, the
 * frame tally and the match length do not. */
void cue_rules_next_frame(CueRules *r, const CueTable *t);

/* Give the frame up. The opponent takes it, and the match tally moves with it —
 * which is the whole reason a snooker player concedes rather than potting out a
 * frame they cannot win. */
void cue_rules_concede(CueRules *r, int player);
/* Snooker Shootout, the host's clock talking to the rules' book: time up
 * (returns 0 on a tie — stage the blue), and the tie-break's verdict. */
/* Is the striker snookered on every ball they may legally hit? The pool
 * games' version of cue_rules_is_snookered — Blackball rule 5g hangs on it. */
int  cue_rules_pool_snookered(const CueRules *r, const CueBall *b, int n,
                              const CueWorld *w);
int  cue_rules_shootout_time(CueRules *r);
void cue_rules_shootout_win(CueRules *r, int winner);

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
/* Is the striker snookered — no ball On visible at both its extreme edges?
 *
 * `w` carries the cushions, and they count: the rule book says "obstructed by a
 * ball or balls not On" because on a rectangular table no cushion CAN obstruct
 * (the bed is convex), not because a cushion should be ignored. Pass the world
 * the shot is being played on and custom beds answer correctly; pass NULL for
 * the letter of the rule book and balls only. See clear_path in cue_rules.c. */
/* The two bookends of the miss judgement — call around every snooker stroke.
 * begin records the straight distance to each legal ball-on; end reads the
 * integrator's attempt log out of the world. Harmless in any other game. */
void cue_rules_attempt_begin(CueRules *r, const CueBall *b, int n);
void cue_rules_attempt_end(CueRules *r, const CueWorld *w, const CueBall *b, int n);

int  cue_rules_is_snookered(const CueRules *r, const CueBall *b, int n,
                            const CueWorld *w);

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
