/*
 * ThumbyCue — rules & scoring. See cue_rules.h. Faithful-but-simplified port
 * of 2dpool/js/game.js: UK-style 8-ball (numeric groups, ball-in-hand fouls,
 * black last) and snooker (red→colour alternation, values, colour respotting,
 * foul = max(4,…), clearance sequence, frame end on the black).
 */
#include "cue_rules.h"
#include "cue_types.h"
#include <string.h>
#include <stdio.h>

static void book_frame(CueRules *r, int winner);

/* ---- ball classification --------------------------------------------- */
static int pool_group(int id) {            /* 1 low, 2 high, 0 = the 8 */
    if (id >= 1 && id <= 7) return 1;
    if (id >= 9 && id <= 15) return 2;
    return 0;                              /* id == 8 */
}
static int is_red(int id)    { return id >= 1 && id <= 15; }
static int is_colour(int id) { return id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK; }
static int snk_value(int id) {
    if (is_red(id)) return 1;
    switch (id) {
        case CUE_ID_YELLOW: return 2; case CUE_ID_GREEN: return 3;
        case CUE_ID_BROWN:  return 4; case CUE_ID_BLUE:  return 5;
        case CUE_ID_PINK:   return 6; case CUE_ID_BLACK: return 7;
    }
    return 0;
}
static int colour_id_for_value(int v) {
    switch (v) {
        case 2: return CUE_ID_YELLOW; case 3: return CUE_ID_GREEN;
        case 4: return CUE_ID_BROWN;  case 5: return CUE_ID_BLUE;
        case 6: return CUE_ID_PINK;   case 7: return CUE_ID_BLACK;
    }
    return -1;
}

void cue_rules_init(CueRules *r, const CueTable *t, int cpu) {
    memset(r, 0, sizeof(*r));
    /* THE TABLE IS A SNOOKER TABLE; THE GAME MIGHT NOT BE. `kind` here means
     * "score this as snooker", and English billiards is played on the same bed
     * with the same balls and is not snooker by any reading — three balls, no
     * colours to nominate, and scoring by cannons and in-offs. */
    /* AND PAUL IS NOT SNOOKER EITHER. It is played with the snooker set on a
     * snooker bed, so t->is_snooker is quite right about the TABLE — but its
     * scoring is nothing like snooker's, there is no ball on and no order, and
     * `kind` here means "score this as snooker". Left in, every red would have
     * needed a colour after it. */
    r->kind = t->is_snooker && t->kind != CUE_GAME_BILLIARDS &&
              t->kind != CUE_GAME_PAUL;
    r->mode = t->kind;
    r->R = t->R;
    r->cpu = cpu;
    r->turn = 0; r->winner = -1; r->open = 1; r->break_shot = 1;
    r->shots_remaining = 1; r->two_shot = 0; r->free_shot = 0;
    /* Pub rules unless the caller says otherwise: it is the game most people
     * mean by UK 8-ball, and cue_rules_set_uk() switches it before the break. */
    r->uk_intl = 0;
    r->baulk_x = t->baulk_x; r->d_radius = t->d_radius;
    r->best_of = 1;
    if (t->kind == CUE_GAME_GOLF) {
        /* A ROUND, NOT A FRAME. There is no target score and nothing to run
         * out: the course ends it after eighteen holes, and low wins. The card
         * starts empty and a zero on it means "not played yet", which is why
         * nothing here has to say how many holes are still to come. */
        r->target_score = 0;
        r->golf_hole = cue_golf_first(r->golf_round);
        r->golf_strokes = 0;
        r->golf_done = 0;
        r->golf_honour = r->turn;
        for (int p = 0; p < 2; p++)
            for (int h = 0; h < CUE_GOLF_HOLES; h++) r->golf_card[p][h] = 0;
        cue_table_golf_set_hole(r->golf_hole);
    } else if (t->kind == CUE_GAME_BARBILLIARDS) {
        /* Rule 92: the game opens from the break position, and Rule 94 sends
         * it back there whenever the table empties. A coin buys about
         * seventeen minutes (Rule 71's tables are coin-operated). */
        r->bb_time = CUE_BB_TIME;
        r->bb_from_break = 1;
        r->bb_left = t->nballs ? t->nballs : 8;
        r->target_score = 0;          /* the clock ends it, not a number */
        for (int i = 0; i < 8; i++) r->bb_hole[i] = -1;
    } else if (t->kind == CUE_GAME_BILLIARDS) {
        /* The four marks of the standard table, in the same slots snooker uses
         * for its colours, so a host that already asks r->spot[] for a place to
         * put a ball needs no second table. Only three of them are used. */
        #define BSPOT(i_, x_) do { Vec3 q_ = cue_table_lay(t, (x_), 0.0f, NULL); \
                                   q_.y = t->R; r->spot[i_] = q_; } while (0)
        BSPOT(CUE_BIL_SPOT_SPOT,    t->black_x);   /* the Spot */
        BSPOT(CUE_BIL_SPOT_CENTRE,  t->blue_x);    /* the Centre Spot */
        BSPOT(CUE_BIL_SPOT_PYRAMID, t->pink_x);    /* the Pyramid Spot */
        #undef BSPOT
        { Vec3 up; cue_table_lay(t, t->black_x, 0.0f, &up); r->spot_up = up; }
        /* Rule 5(d): a game is to an agreed number of points. 100 is a short
         * one and a VR frame wants to end inside a session; the tournament
         * numbers are hundreds more and cue_rules_set_target sets them. */
        r->target_score = 100;
        /* Rule 2(b) has the first player in hand, and rack_billiards has
         * already put his ball in the D — exactly as snooker's rack does with
         * the same rule. Setting the flag as well would send the host into a
         * placement it does not need and leave the table looking empty. */
    } else if (CUE_GAME_IS_CAROM(t->kind)) {
        /* CAROM: race to a target of cannons. The numbers are the customary
         * club distances, scaled to how hard each game's point is: straight
         * rail runs long, three-cushion is won in the teens. */
        r->target_score = t->kind == CUE_GAME_CAROM_STRAIGHT ? 30
                        : t->kind == CUE_GAME_CAROM_1C       ? 20
                        : t->kind == CUE_GAME_CAROM_2C       ? 15
                        : t->kind == CUE_GAME_CAROM_3C       ? 10 : 15;
        r->bil_yellow = 0;
    } else if (t->kind == CUE_GAME_SPEED) {
        /* No target: the score is a time, and lower is better. */
        r->target_score = 0;
        r->sp_cs[0] = r->sp_cs[1] = 0;
        r->sp_done[0] = r->sp_done[1] = 0;
    } else if (t->kind == CUE_GAME_BOWLLIARDS) {
        /* THE TARGET IS THE PERFECT GAME, and it is a display number rather
         * than something to reach: nobody wins by getting to three hundred, the
         * higher of two ten-frame cards wins. It is here because it is the one
         * figure that says what the card is for, and because a game whose
         * maximum is unreachable is the mistake this game is easiest to make —
         * see resolve_bowlliards on the foul penalty we do not apply. */
        r->target_score = 300;
        r->called_pocket = -1;
        /* Every stroke but the break names a ball and a pocket. */
        r->call_shot_on = 2;
        for (int p = 0; p < 2; p++) {
            r->bw_frame[p] = 0;
            r->bw_sd[p] = 0xFF;
            for (int i = 0; i < (int)sizeof r->bw_pins[p]; i++)
                r->bw_pins[p][i] = 0xFF;       /* every delivery unplayed */
        }
        r->bw_inning = 1;
        /* The first frame opens the way every other one does: a rack, a free
         * break, and the cue ball behind the head string for it. */
        r->break_shot = 1;
        r->ball_in_hand = 1;
    } else if (t->kind == CUE_GAME_CRIBBAGE) {
        /* FIVE CRIBBAGES, and a target rather than a count of balls: the score
         * is pairs made, so a board that already draws two numbers draws these.
         * Five is a majority of the eight a rack holds, which is what makes it
         * the number — and why the eighth, the 15 on its own, has to exist for
         * a level game to be decided at all. */
        r->target_score = 5;
        r->called_pocket = -1;
        /* Ball and pocket on every stroke but the break. */
        r->call_shot_on = 2;
        r->cr_nowed = 0;
        for (int i = 0; i < 8; i++) r->cr_owed[i] = 0;
        /* An OPEN break, and unlike bowlliards' free one it is an ordinary
         * stroke of the breaker's inning — so it is set up the same way any
         * other US game's is, from in hand behind the head string. */
        r->break_shot = 1;
        r->ball_in_hand = 1;
    } else if (t->kind == CUE_GAME_HONOLULU) {
        r->target_score = 8;
    } else if (t->kind == CUE_GAME_COWBOY) {
        r->target_score = 101;
        /* FROM BEHIND THE HEAD STRING, and it has to find the 3 first.
         * "Starting player must place the cue ball behind the head string and
         * cause the cue ball to contact the 3 ball first." Neither was set, so
         * the break could be played from wherever the rack left the white, at
         * whatever it liked. */
        r->break_shot = 1;
        r->ball_in_hand = 1;
        r->cow_inning = 0;
    } else if (CUE_GAME_IS_ROT61(t->kind)) {
        /* 120 on the table and 61 takes it — the majority, so the frame is
         * decided the moment one player cannot be caught. */
        r->target_score = 61;
        /* the 1 is the ball on off the rack — except at fifteen-ball, where
         * there is no ball on at all and the board says so */
        r->seq = (t->kind == CUE_GAME_FIFTEEN) ? 0 : 1;
    } else if (t->kind == CUE_GAME_BANKPOOL) {
        /* Eight of the fifteen, as One Pocket is, and the foul debt with it. */
        r->target_score = 8;
        r->op_owed[0] = r->op_owed[1] = 0;
    } else if (t->kind == CUE_GAME_ONEPOCKET) {
        /* Eight of the fifteen, which leaves one ball that cannot decide it —
         * the game's own margin, and why a match can hang on a single safety. */
        r->target_score = 8;
        /* Both unowned until the breaker has chosen — see op_pick. The host
         * knows WHICH two pockets the table has; the rules know only that
         * neither belongs to anybody yet. */
        r->op_hole[0] = r->op_hole[1] = -1;
        r->op_owed[0] = r->op_owed[1] = 0;
        r->op_pick = 0;
    } else if (CUE_GAME_IS_KILLER(t->kind)) {
        /* KILLER: the score IS the lives. Three each, counting down; the
         * frame ends when somebody has none. */
        r->score[0] = r->score[1] = 3;
        r->target_score = 0;
        r->break_shot = 1;
        r->ball_in_hand = 1;
    } else if (r->kind || t->kind == CUE_GAME_PAUL) {
        /* THE FOUR SPOTS AND THE D, for snooker AND for Paul. Paul scores
         * nothing like snooker, but the table has the marks printed on it and
         * one of them does real work: a level table with nothing left re-spots
         * the black, and the black's spot has to exist for that to be possible.
         * The rest are scenery — nothing is ever spotted in Paul. */
        r->target = 0; r->reds_left = t->reds ? t->reds : 15;
        /* WHAT IS ON THE TABLE, before a ball has been struck — twenty-nine on
         * a full set. Counted from the table's own red count rather than
         * assumed, so a Paul frame on a bed built for six reds is right too. */
        if (t->kind == CUE_GAME_PAUL)
            r->paul_left = (t->reds ? t->reds : 15) + 5 * 2 + 4;
        /* colour spots by value 2..7 */
        /* SWAPPED. Standing behind the D looking up the table, the order
         * reading left to right is green, brown, yellow — "God Bless You".
         * These were the other way about: the comments said right and left and
         * the coordinates said the opposite, and the render agreed with the
         * coordinates. +Z is the player's RIGHT from baulk (facing +X with +Y
         * up), so yellow is +d_radius. Checked against a render from behind
         * the baulk cushion rather than against the handedness argument, which
         * is exactly the sort of reasoning that put them here. */
        /* LAID ALONG THE SPINE, exactly as the rack is. These were raw x and z,
         * which is the table's long axis written into the respot — and on an L
         * the long axis runs through the notch, so a potted colour came back
         * inside a cushion or off the cloth. cue_table_lay is the same function
         * that puts them there at the start of the frame, so the spot a colour
         * is respotted on is the spot it was racked on. */
        #define SPOT_AT(x_, a_) do { Vec3 q_ = cue_table_lay(t, (x_), (a_), NULL); \
                                     q_.y = t->R; r->spot[SPOT_I] = q_; } while (0)
        { const int SPOT_I = 2; SPOT_AT(t->baulk_x, +t->d_radius); }  /* yellow */
        { const int SPOT_I = 3; SPOT_AT(t->baulk_x, -t->d_radius); }  /* green  */
        { const int SPOT_I = 4; SPOT_AT(t->baulk_x, 0.0f); }          /* brown  */
        { const int SPOT_I = 5; SPOT_AT(t->blue_x,  0.0f); }          /* blue   */
        { const int SPOT_I = 6; SPOT_AT(t->pink_x,  0.0f); }          /* pink   */
        { const int SPOT_I = 7; SPOT_AT(t->black_x, 0.0f); }          /* black  */
        #undef SPOT_AT
        /* and which way is "up the table" where the top colours live */
        { Vec3 up; cue_table_lay(t, t->black_x, 0.0f, &up); r->spot_up = up; }
    } else {
        /* foot spot — respot for the 9 (US9), an illegally broken-in 8, or any
         * ball spotted in straight pool, which does far more of it than either */
        /* the foot spot, laid along the spine like everything else — and the
         * direction the long string runs from it, for anything spotted up */
        {   Vec3 up; Vec3 f = cue_table_foot_spot_dir(t, &up);
            r->spot[0] = v3(f.x, t->R, f.z);
            r->spot_up = up; }
        if (CUE_GAME_IS_ROTATION(t->kind)) r->seq = 1;     /* lowest ball on (HUD) */
        if (t->kind == CUE_GAME_STRAIGHT) {
            r->called_pocket = -1;
            r->target_score = 50;
            r->racks = 0;
        }
    }
}

static int group_cleared(const CueBall *b, int n, int grp) {
    for (int i = 1; i < n; i++)
        if (b[i].on && pool_group(b[i].id) == grp) return 0;
    return 1;
}

static CueBall *find_ball(CueBall *b, int n, int id) {
    for (int i = 0; i < n; i++) if (b[i].id == id) return &b[i];
    return NULL;
}
/* re-spot the 8 (illegally potted on the break). Must move it back to the
 * foot spot — otherwise it's resurrected underground in the pocket (on=1 but
 * invisible), where the asleep-ball skip leaves it forever and the frame can
 * never end. Mirrors respot_colour. */
static void respot_eight(CueRules *r, CueBall *b, int n) {
    CueBall *q = find_ball(b, n, 8);
    if (!q) return;
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0); q->drop = 0.0f;
    q->pos = r->spot[0];           /* foot spot (occupancy not checked) */
    q->orient = m3_identity();
}
/* Is any OTHER ball sitting on this point? */
static int spot_taken(const CueBall *b, int n, Vec3 p, int self_id, float R) {
    for (int i = 0; i < n; i++) {
        if (!b[i].on || b[i].id == self_id) continue;
        float dx = b[i].pos.x - p.x, dz = b[i].pos.z - p.z;
        if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R)) return 1;
    }
    return 0;
}

/* Respot a colour, WPBSA rule 3.6: its own spot; if that is occupied, the
 * highest-value spot that is free; if every spot is occupied, as near as
 * possible to its own spot on the centre line, above it, without touching.
 *
 * The old version dropped the ball on its own spot and said so in a comment —
 * "occupancy not checked, good enough". It is not: two balls in the same place
 * is a physics explosion the moment either is touched, and with practice mode
 * respotting a colour after every pot it would happen constantly. */
static void respot_colour(CueRules *r, CueBall *b, int n, int id) {
    CueBall *q = find_ball(b, n, id);
    if (!q) return;
    int v = snk_value(id);
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0);
    q->orient = m3_identity();

    Vec3 p = r->spot[v];
    if (spot_taken(b, n, p, id, r->R)) {
        int found = 0;
        for (int k = 7; k >= 2 && !found; k--) {          /* highest free spot */
            if (!spot_taken(b, n, r->spot[k], id, r->R)) { p = r->spot[k]; found = 1; }
        }
        if (!found) {
            /* Up the table from its own spot, toward the top cushion — ALONG
             * THE SPINE, which on an L is not +x. */
            p = r->spot[v];
            for (int step = 1; step <= 60; step++) {
                float d = (float)step * r->R * 0.5f;
                Vec3 t = p;
                t.x += r->spot_up.x * d; t.z += r->spot_up.z * d;
                if (!spot_taken(b, n, t, id, r->R)) { p = t; break; }
            }
        }
    }
    q->pos = p;
}

/* ---- 8-ball ---------------------------------------------------------- */
/* Off the table is a foul in pool too — jumping is not. */
/* ---- WAS THE BREAK A LEGAL ONE? -----------------------------------------
 *
 * Almost every rule book asks the same shape of question — the break must DO
 * something, or it is not a break — and then asks it with different numbers and
 * pays for it in a different currency. The numbers are per game and quoted at
 * each call; these two only measure.
 *
 * DRIVEN TO A RAIL is CueWorld::cush and NOT rails[]. rails[] only counts a
 * contact that turned the ball more than fifteen degrees, because that is what
 * bank pool means by coming off a cushion; a ball that runs nearly parallel
 * into a rail and comes away gently has still been driven to one, and counting
 * it the bank way would foul legal breaks. Index 0 is the cue ball, which 14.1
 * asks about separately.
 *
 * CROSSED THE LINE is CueWorld::brk_cross, whose bit per ball is set when the
 * whole ball has passed the line between the middle pockets. Note that the
 * books do not agree on WHICH line: blackball and Ultimate Pool use that one,
 * 9-ball's Regulation 16 uses the head string, and the pyramid uses the centre
 * line with its own rule about the ball's centre. Only the middle-pocket line
 * is measured here, so only the games that use it may ask. */
/* NO WORLD MEANS UNKNOWN, AND UNKNOWN IS NOT A FOUL.
 *
 * Every break rule here is of the form "fewer than N balls reached a rail". The
 * three counters below are the only things that can answer it, and they answer
 * it from CueWorld — which a caller is allowed not to have. cue_rules_resolve
 * takes the world as a pointer and test_wrongball passes NULL, because it is
 * testing which ball was cued and has no opinion about cushions.
 *
 * These returned zero in that case, so "I cannot tell" was scored as "nothing
 * touched a rail" and every such stroke was a break foul. Nothing in the game
 * saw it — the app always has a world — but the first harness to resolve a
 * break without one found it, and a rule that manufactures a foul out of
 * missing information is wrong whoever is asking.
 *
 * So they report SATISFIED instead: a count nothing can be short of, and a cue
 * ball that did find a cushion. A break can only be faulted on evidence. */
#define BRK_PLENTY (CUE_MAX_BALLS + 1)

static int brk_rails(const CueWorld *w, int n) {
    int c = 0;
    if (!w) return BRK_PLENTY;
    for (int i = 1; i < n && i < CUE_MAX_BALLS; i++) if (w->cush[i]) c++;
    return c;
}
static int brk_cue_rail(const CueWorld *w) { return w ? (w->cush[0] != 0) : 1; }
static int brk_crossed(const CueWorld *w) {
    if (!w) return BRK_PLENTY;
    int c = 0;
    uint32_t m = w->brk_cross;
    while (m) { c += (int)(m & 1u); m >>= 1; }
    return c;
}

static void resolve_pool(CueRules *r, CueBall *b, int n, const CueWorld *w,
                         int first_hit,
                         int scratch, int cushion, const int *potted, int np) {
    /* THE CALL, where the rule set asks for one. Level 1 asks only for the
     * black — Chinese 8-ball's own exemption for the obvious shots, and the
     * pub convention everywhere else; level 2 is WPA's every-stroke call.
     * Never on the break in either. The call belongs to this stroke however
     * it turns out, so it is read and cleared here. */
    const int pool_call = (r->mode == CUE_GAME_US8 || r->mode == CUE_GAME_CN8)
                        ? r->call_shot_on : 0;
    const int called_id  = r->nominated;
    const int called_pkt = r->called_pocket;
    r->nominated = 0; r->called_pocket = -1;
    int made_call = 0;
    for (int k = 0; k < np && called_id; k++) {
        if (potted[k] != called_id) continue;
        const CueBall *q = find_ball(b, n, potted[k]);
        if (!q || q->pocket == CUE_OFF_TABLE) continue;   /* driven off, not potted */
        if (called_pkt < 0 || (int)q->pocket == called_pkt) made_call = 1;
    }
    /* WPA Blackball Rules 2005, where the English game differs from the pub
     * and WPA-international readings that share this resolver. */
    const int bb = (r->mode == CUE_GAME_UK8 && r->uk_intl == CUE_UK_BLACKBALL);
    /* Rule 6b: on the first shot after a foul — and only that shot — any ball
     * may be struck and any ball potted without penalty. */
    const int was_free = bb && r->free_shot && r->shots_remaining >= 2;
    int grp = r->group[r->turn];
    int low = 0, high = 0, eight = 0;
    for (int k = 0; k < np; k++) {
        int g = pool_group(potted[k]);
        if (potted[k] == 8) eight = 1; else if (g == 1) low++; else if (g == 2) high++;
    }
    int my_potted = (grp == 1) ? low : high;   /* own group balls potted THIS shot */
    int legal_pot = (r->open || was_free) ? (low || high) : my_potted;
    /* WPA 8-ball is a call-shot game: a ball potted that was not the one
     * called ends the visit. It is NOT a foul and nothing comes back up — the
     * table simply passes, exactly as ten-ball's does. The break and the free
     * shot are exempt, and so is level 1, which asks only for the black. */
    const int slopped = (pool_call >= 2 && !r->break_shot && !was_free &&
                         legal_pot && !made_call);
    if (slopped) legal_pot = 0;
    /* "on the 8" only if the group was cleared BEFORE this shot — i.e. it's
     * empty now AND you didn't just pot a group ball this shot. Otherwise the
     * shot that pots your last group ball would wrongly read as must-hit-8. */
    int on_eight = !r->open && group_cleared(b, n, grp) && my_potted == 0;

    int foul = 0; const char *why = "";
    if (scratch)            { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0) { foul = 1; why = "NO BALL"; }
    else if (!was_free) {
        int fg = pool_group(first_hit);
        if (!r->open) {
            if (on_eight) { if (first_hit != 8) { foul = 1; why = "MUST HIT 8"; } }
            else if (fg != grp) { foul = 1; why = "WRONG BALL"; }   /* incl. hitting the 8 early */
        } else if (first_hit == 8)        { foul = 1; why = "HIT 8 FIRST"; }
    }
    /* THE CUSHION RULE, where the rule set has one. US and Chinese 8-ball are
     * played under WPA, which requires a ball to be pocketed or some ball to
     * reach a cushion after the contact; the pub game does not, and a soft
     * nudge that touches nothing is legal there. UK international takes the
     * WPA requirement with the rest of it. */
    if (!foul && np == 0 && !cushion && first_hit >= 0 &&
        (r->mode != CUE_GAME_UK8 || r->uk_intl) &&
        !(bb && r->was_snookered)) { foul = 1; why = "NO RAIL"; }
    /* Blackball 5e: a jump shot is a foul — the one pool reading here where
     * leaving the bed is illegal in itself. */
    if (bb && r->jumped && !foul) { foul = 1; why = "JUMP"; }
    /* Blackball 4b: a legal break pots a ball or sends two object balls fully
     * over the line between the middle pockets. The physics keeps the
     * crossing account (CueWorld.brk_cross); anything already foul stands. */
    /* ---- THE BREAK HAS TO DO SOMETHING ---------------------------------
     *
     * Four rule books meet on this resolver and none of them asks the same
     * question, so none of them shares an answer.
     *
     *   BLACKBALL 8.5(b) — a ball potted, or at least TWO object balls across
     *   the Center String (2.1(d): the line between the two side pockets). No
     *   rail test at all, and a single pot satisfies it outright. A foul, which
     *   under 8.13 hands over a free shot.
     *
     *   ULTIMATE POOL / IEPF 4f — no rail test either, and not a foul: THREE
     *   POINTS, counting one for every ball potted and one for every unpotted
     *   ball wholly past the centre-pocket line. Short of three is a mandatory
     *   re-rack.
     *
     *   WPA 8-BALL 4.3 and HEYBALL 6(c) — a ball potted, or FOUR object balls
     *   driven to a rail. Explicitly not a foul in either book and explicitly
     *   NOT ball in hand at heyball: the incoming player is given a choice of
     *   three, of which the one taken here is a re-rack with the break passing
     *   to them.
     *
     *   THE PUB GAME asks for nothing, which is what makes it the pub game. */
    int bad_break = 0;             /* a LOCAL, not a value smuggled in rerack */
    if (r->break_shot && !foul) {
        const int up = (r->mode == CUE_GAME_UK8 && r->uk_intl == CUE_UK_ULTIMATE);
        if (bb) {
            if (np == 0 && brk_crossed(w) < 2) { foul = 1; why = "BREAK"; }
        } else if (up) {
            if (np + brk_crossed(w) < 3) bad_break = 1;
        } else if (r->mode == CUE_GAME_US8 || r->mode == CUE_GAME_CN8 ||
                   (r->mode == CUE_GAME_UK8 && r->uk_intl == CUE_UK_INTL)) {
            if (np == 0 && brk_rails(w, n) < 4) bad_break = 1;
        }
    }
    /* A RE-RACK IS NOT A FOUL, and the difference is the whole of these rules:
     * nothing is owed, nothing is in hand, the balls simply go back and the
     * other player breaks. Marked above and acted on here so it cannot be
     * confused with the foul path below, which pays in a different currency. */
    if (bad_break) {
        r->rerack = 2; r->racks++;
        r->last_foul = 0;
        r->break_shot = 1;
        r->turn = 1 - r->turn;         /* "you break it, then" */
        r->ball_in_hand = 1;
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        snprintf(r->msg, sizeof r->msg, "ILLEGAL BREAK - RE-RACK");
        return;
    }
    /* OFF THE TABLE. Last, so it names the foul when nothing worse did: a ball
     * driven off is a foul however good the contact was, and jumping is legal
     * here, so this is the only thing a clean jump shot can go wrong by. */
    if (r->n_off && !foul) { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    /* the 8 */
    if (eight) {
        if (bb && r->break_shot) {
            /* Rule 4d: the black off any break — with or without other balls,
             * the cue ball included — is a re-rack and the same player breaks
             * again. No penalty of any kind. */
            r->last_foul = 0;
            r->rerack = 2; r->racks++;
            r->break_shot = 1;
            r->ball_in_hand = 1;             /* the re-break is from baulk */
            r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
            snprintf(r->msg, sizeof r->msg, "RE-RACK");
            return;
        }
        if (r->break_shot && r->mode == CUE_GAME_UK8 &&
            r->uk_intl == CUE_UK_ULTIMATE) {
            /* THE GOLDEN BREAK, which the Ultimate Pool Group play and nobody
             * else does: the black off the break wins the frame outright, and
             * the black WITH the cue ball — or with any other foul — loses it.
             * The golden duck. */
            r->frame_over = 1;
            r->winner = foul ? (1 - r->turn) : r->turn;
            book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg, "%s", foul ? "GOLDEN DUCK" : "GOLDEN BREAK");
            return;
        }
        if (r->break_shot) {                       /* re-spot, no result */
            respot_eight(r, b, n);
        } else {
            /* legal win only if the group was clear BEFORE potting the 8 —
             * and, where the rules call, only in the pocket that was called.
             * The black in the wrong pocket is loss of frame, WPA 3.15 and
             * every pub table in the country. Both call levels ask for this
             * one: level 1 asks for nothing else. */
            const int eight_called = !pool_call || !on_eight ||
                                     (called_id == 8 && made_call);
            int win = !foul && !scratch && on_eight && eight_called;
            r->frame_over = 1; r->winner = win ? r->turn : (1 - r->turn);
            book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg,
                     win ? "FRAME WON!"
                     : (!foul && !scratch && on_eight) ? "NOT AS CALLED - 8"
                                                       : "FOUL ON 8");
            return;
        }
    }

    /* WPA 8.3: with call shot on, the table stays open until a player pockets
     * the ball they CALLED — slop leaves it open. */
    if (pool_call >= 2 && !r->break_shot && !made_call) { /* nothing assigned */ }
    else if (r->open && !foul && !r->break_shot && !was_free && (low || high)) {  /* assign (4e/4f) */
        int g = (low && !high) ? 1 : (high && !low) ? 2 : pool_group(first_hit);
        if (g == 1 || g == 2) { r->group[r->turn] = g; r->group[1-r->turn] = (g==1)?2:1; r->open = 0; }
    }

    if (foul) {
        if (bb) {
            /* Rules 5/6a/6c: the offender loses the next visit — a free shot
             * plus one visit to the opponent — and the cue ball is played from
             * where it lies, or from baulk after an in-off. (The lie-or-baulk
             * choice on every foul, 6c, is not offered yet: as it lies, and
             * baulk when it must be.) */
            r->turn = 1 - r->turn;
            r->two_shot = 1; r->shots_remaining = 2; r->free_shot = 1;
            r->ball_in_hand = scratch ? 1 : 0;
            snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);   /* HUD: FREE SHOT */
        } else if (r->mode != CUE_GAME_UK8 || r->uk_intl) {
            /* WPA — US, Chinese, and UK international: any foul gives the
             * opponent ball in hand anywhere on the table. */
            r->turn = 1 - r->turn; r->ball_in_hand = 1;
            r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
            snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        } else {
            /* UK two-shot rule: opponent gets two visits; the cue ball stays put
             * unless it was potted (scratch → ball in hand behind the line). */
            r->turn = 1 - r->turn;
            r->two_shot = 1; r->shots_remaining = 2; r->free_shot = 1;
            r->ball_in_hand = scratch ? 1 : 0;
            snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);   /* HUD shows 2 SHOTS */
        }
    } else if (legal_pot) {
        /* potting your own ball cancels any two-shot advantage carried in */
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        r->msg[0] = 0;                              /* same player continues */
    } else if (slopped) {
        /* the ball went down, just not the one that was called */
        r->turn = 1 - r->turn;
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        snprintf(r->msg, sizeof r->msg, "NOT AS CALLED");
    } else if (r->shots_remaining > 1) {
        /* missed but still holding a shot from the carry — play on, same player */
        r->shots_remaining--; r->free_shot = 0;
        snprintf(r->msg, sizeof r->msg, "2ND SHOT");
    } else {
        r->turn = 1 - r->turn;
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        r->msg[0] = 0;
    }
    r->break_shot = 0;
}

/* ---- snooker --------------------------------------------------------- */
static int snk_on(const CueRules *r, int id) {
    if (r->target == 0) return is_red(id);
    /* On a colour: the NOMINATED one, once there is one. Before it is named any
     * colour is still "the ball on" — that is what lets the striker declare it
     * by aiming at it rather than being forced to choose from a menu first. */
    if (r->target == 1)
        return is_colour(id) && (!r->nominated || snk_value(id) == r->nominated);
    return is_colour(id) && snk_value(id) == r->seq;   /* clearance */
}

/* Should this player give the frame up? Straight from the 2D game: they are
 * behind, and further behind than the balls left can retrieve by a clear margin
 * — fifteen points while there are still reds to be had, twelve once it is down
 * to the colours, because a colours clearance is worth less and comes sooner. */
int cue_rules_should_concede(const CueRules *r, int player) {
    if (!r->kind || r->frame_over) return 0;
    int deficit = r->score[1 - player] - r->score[player];
    if (deficit <= 0) return 0;
    int remaining;
    if (r->reds_left > 0) remaining = r->reds_left * 8 + 27;
    else { remaining = 0; for (int v = (r->seq < 2 ? 2 : r->seq); v <= 7; v++) remaining += v; }
    /* MORE THAN TWELVE POINTS OF SNOOKERS. Once the deficit is past what is
     * left on the table, the difference is what has to come from snookers, and
     * at four points each twelve is three of them — past that no player carries
     * on, they shake hands. The threshold used to be 15 with reds still up and
     * 12 on the colours; one number is right for both, because the arithmetic
     * that makes it hopeless is the same arithmetic. */
    return (deficit - remaining) > 12;
}

/* A finished frame, booked into the match. Called from every place a frame can
 * end — potted out, forfeited on three misses, or conceded — so the tally cannot
 * be updated in some of them and not others. */
static void book_frame(CueRules *r, int winner) {
    if (winner < 0 || winner > 1) return;
    r->frames[winner]++;
    int need = (r->best_of > 1) ? (r->best_of / 2 + 1) : 1;
    if (r->frames[winner] >= need) { r->match_over = 1; r->match_winner = winner; }
}

void cue_rules_concede(CueRules *r, int player) {
    if (r->frame_over) return;
    r->frame_over = 1;
    r->conceded = 1;
    r->winner = 1 - player;
    r->brk = 0;
    snprintf(r->msg, sizeof r->msg, "FRAME CONCEDED");
    book_frame(r, r->winner);
}

/* WHAT SURVIVES THE TABLE BEING LAID OUT AGAIN.
 *
 * cue_rules_init memsets the struct, so everything chosen before the frame
 * started comes back as its default -- and uk_intl's default is 0, which is
 * CUE_UK_PUB, the two-shot game. That was reported once already from an online
 * best of three ("it suddenly changed the rules to 2 shots") and fixed here in
 * next_frame by writing the list out by hand.
 *
 * It was then reported a SECOND time, from practice: pressing RE-RACK put UK
 * 8-ball back to pub rules. rerack() calls cue_rules_init directly and had no
 * such list, because the list was in next_frame rather than anywhere it could
 * be shared.
 *
 * So it lives here once and both callers use it. A setting that belongs to the
 * MATCH rather than to the frame goes in this struct and nowhere else. */
typedef struct {
    int   uk, call, miss, shoot, solo;
    int   cpu, best_of, f0, f1, break_first, match_over, match_winner;
    int   target;
    float bil_len;
} RulesKeep;

static void rules_keep_take(const CueRules *r, RulesKeep *k) {
    k->uk = r->uk_intl;        k->call = r->call_shot_on; k->miss  = r->miss_level;
    k->shoot = r->snk_shootout; k->solo = r->golf_solo;   k->cpu   = r->cpu;
    k->best_of = r->best_of;   k->f0 = r->frames[0];      k->f1    = r->frames[1];
    k->break_first = r->break_first;
    k->match_over = r->match_over; k->match_winner = r->match_winner;
    k->target = r->target_score;   k->bil_len = r->bil_time_len;
}

static void rules_keep_put(CueRules *r, const RulesKeep *k) {
    if (k->target > 0)    r->target_score = k->target;
    if (k->bil_len > 0.0f) cue_rules_bil_set_time(r, k->bil_len);
    r->uk_intl = k->uk;  r->call_shot_on = k->call; r->miss_level = k->miss;
    r->snk_shootout = k->shoot; r->golf_solo = k->solo;
    r->best_of = k->best_of;
    r->frames[0] = k->f0; r->frames[1] = k->f1;
    r->break_first = k->break_first;
    r->match_over = k->match_over; r->match_winner = k->match_winner;
}

/* THE SAME FRAME, LAID OUT AGAIN. Not the next frame: nothing is scored, the
 * break does not alternate and the match tally does not move. Everything the
 * player chose before the frame started stays chosen. */
void cue_rules_rerack(CueRules *r, const CueTable *t) {
    RulesKeep k;
    if (!r) return;
    rules_keep_take(r, &k);
    cue_rules_init(r, t, k.cpu);
    rules_keep_put(r, &k);
}

void cue_rules_next_frame(CueRules *r, const CueTable *t) {
    int f0 = r->frames[0], f1 = r->frames[1], bo = r->best_of, cpu = r->cpu;
    int mo = r->match_over, mw = r->match_winner;
    /* Alternate the break, as a match does — FROM WHOEVER BROKE THE FIRST ONE.
     * This counted frames alone, so with a random first break the second frame
     * could hand it back to the same player. */
    int bf = r->break_first;
    /* The target is a property of the MATCH, like best_of — a 100-up frame is
     * followed by another 100-up frame, not by the default. */
    int tgt = r->target_score;
    /* ...and so does the CLOCK, for exactly the same reason: a ten-minute game
     * is followed by another ten-minute game, not by the default points target.
     * Its LENGTH, not what is left of it — the next frame gets a full clock. */
    float btm = r->bil_time_len;
    /* AND SO DOES THE RULE BOOK, which is the one this had been missing.
     *
     * Reported from an online match: "A best of 3 in 8 ball. It was supposed to
     * be international rules, but in the second frame it suddenly changed the
     * rules to 2 shots." It did. cue_rules_init memsets the whole struct and
     * uk_intl comes back as 0 — which is CUE_UK_PUB, and PUB is the two-shot
     * game. Every frame after the first was played to a code nobody chose.
     *
     * It is not only the UK row. Everything the host sets up at start_frame and
     * does not set again here went the same way: the snooker miss standard, the
     * shootout format, whether the shot is called, and golf's solo card. A
     * best-of-seven of call-shot ten-ball was one frame of call-shot followed
     * by six of ordinary ten-ball, and a snooker match set to the professional
     * miss standard dropped to the default after frame one. None of it showed
     * unless you knew what you had picked.
     *
     * The principle is already written three lines above: the target is a
     * property of the MATCH, like best_of. So is the code being played to.
     *
     * ONLINE THIS IS NOT A DESYNC, which is why no harness caught it: both ends
     * call this on their own press of A and both reset identically, so the two
     * agree perfectly about the wrong rules. Only a player who knew what they
     * had chosen could see it, which is exactly who reported it. */
    /* ...and all of it through ONE list now -- see RulesKeep. Written out by
     * hand here, it was missing from rerack() entirely, and the same bug was
     * reported a second time from practice. */
    RulesKeep k;
    rules_keep_take(r, &k);
    int first = (bf + f0 + f1) & 1;
    cue_rules_init(r, t, cpu);
    rules_keep_put(r, &k);
    r->frames[0] = f0; r->frames[1] = f1; r->best_of = bo;
    r->match_over = mo; r->match_winner = mw;
    r->break_first = bf;
    r->turn = first;
}

void cue_rules_set_uk(CueRules *r, int ruleset) {
    if (!r) return;
    if (ruleset < CUE_UK_PUB || ruleset > CUE_UK_BLACKBALL) ruleset = CUE_UK_PUB;
    r->uk_intl = ruleset;
}

void cue_rules_respot(CueRules *r, CueBall *b, int n, int id) {
    if (!r->kind || !is_colour(id)) return;
    respot_colour(r, b, n, id);
}

void cue_rules_set_break(CueRules *r, int who) {
    r->break_first = who ? 1 : 0;
    r->turn = r->break_first;
    /* ...and at golf that same draw is the first hole's honour. Every hole
     * after it is earned; the first one has nothing to earn it with. */
    r->golf_honour = r->break_first;
}

void cue_rules_set_golf_round(CueRules *r, int round) {
    if (!r) return;
    if (round < 0 || round >= CUE_GOLF_ROUNDS) round = CUE_GOLF_18;
    r->golf_round = round;
    r->golf_hole = cue_golf_first(round);
    r->golf_strokes = 0;
    cue_table_golf_set_hole(r->golf_hole);
}

void cue_rules_nominate(CueRules *r, int value) {
    if (!r->kind || r->target != 1) return;
    r->nominated = (value >= 2 && value <= 7) ? value : 0;
}

void cue_rules_nominate_free(CueRules *r, int id) {
    if (!r->kind || !r->free_ball) return;
    r->free_ball_id = id;
}

/* Does the line a->b cross the line c->d, in the XZ plane? Ends excluded, so a
 * ray that merely touches a segment's endpoint is not a crossing. */
static int xz_cross(float ax, float az, float bx, float bz,
                    float cx, float cz, float dx, float dz) {
    float rx = bx - ax, rz = bz - az;
    float sx = dx - cx, sz = dz - cz;
    float den = rx * sz - rz * sx;
    if (den > -1e-9f && den < 1e-9f) return 0;              /* parallel */
    float qx = cx - ax, qz = cz - az;
    float t = (qx * sz - qz * sx) / den;
    float u = (qx * rz - qz * rx) / den;
    return t > 0.0f && t < 1.0f && u > 0.0f && u < 1.0f;
}

/* Full-ball line of sight from `from` to a target ball at `to` (XZ plane): both
 * extreme edges of the target must be reachable without a blocker in the way.
 * Ported from 2dpool hasClearPath(). rad = ball radius (all equal in snooker).
 *
 * AND THE CUSHIONS, WHICH THE RULE BOOK DOES NOT MENTION.
 *
 * Section 2 defines snookered as obstructed "by a ball or balls not On". Balls.
 * Only balls — and that is not an oversight, it is a fact that never had to be
 * written down: on a rectangular table the bed is CONVEX, so the straight line
 * between two balls resting on it cannot leave it, and no cushion can ever come
 * between them. The rule did not exclude cushions; it never had to consider
 * them.
 *
 * Custom beds break the assumption underneath the sentence. An L-shaped table's
 * inner corner cuts straight across the line between two balls that can see
 * each other on no rectangular table in the world, and the game noticed in the
 * worst way: an opponent laid a genuine snooker behind the elbow, the sight
 * test said "not snookered" because no BALL was in the way, three honest
 * failures to escape were counted as three misses, and the frame was forfeited.
 *
 * So the definition is extended, not corrected: obstructed by anything the cue
 * ball could not pass through. On a rectangular table that is the same sentence
 * — provably, by convexity, and test_sight asserts it table by table — and on
 * every other shape it is the one the rule would have written if it had had to.
 *
 * A pocket mouth is not a wall: there is no segment across it, so a ray through
 * one crosses nothing. The facings beside it ARE segments and do count, which
 * is right — they are timber, and a ball sent along that line would hit them. */
static int clear_path(Vec3 from, Vec3 to, float rad,
                      const CueBall *b, int n, int target_idx,
                      const CueWorld *w) {
    float dx = to.x - from.x, dz = to.z - from.z;
    float dist = sqrtf(dx*dx + dz*dz);
    if (dist < 1e-4f) return 1;
    float nx = dx / dist, nz = dz / dist;
    float px = -nz, pz = nx;                 /* perpendicular unit */
    float clr = rad + rad;                   /* cue radius + blocker radius */
    for (int e = -1; e <= 1; e += 2) {       /* left / right extreme edge */
        float ex = to.x + px * rad * e, ez = to.z + pz * rad * e;
        float edx = ex - from.x, edz = ez - from.z;
        float ed = sqrtf(edx*edx + edz*edz);
        if (ed < 1e-4f) continue;
        float enx = edx / ed, enz = edz / ed;
        for (int i = 0; i < n; i++) {
            if (i == target_idx || i == 0 || !b[i].on) continue;   /* skip cue + target */
            float tx = b[i].pos.x - from.x, tz = b[i].pos.z - from.z;
            float proj = tx * enx + tz * enz;
            if (proj < 0.0f || proj > ed) continue;                /* behind / beyond */
            float cxp = from.x + enx * proj, czp = from.z + enz * proj;
            float ddx = b[i].pos.x - cxp, ddz = b[i].pos.z - czp;
            if (sqrtf(ddx*ddx + ddz*ddz) < clr) return 0;          /* blocked */
        }
        if (!w) continue;
        /* The same ray against the cushion chain. Pulled in a quarter of a ball
         * at each end, because both ends of it sit on a ball that may be frozen
         * to a cushion — and a cushion a ball is already touching is not
         * something standing between it and anywhere. */
        {   float k = rad * 0.25f;
            float px0 = from.x + enx * k,        pz0 = from.z + enz * k;
            float px1 = from.x + enx * (ed - k), pz1 = from.z + enz * (ed - k);
            float rlox = px0 < px1 ? px0 : px1, rhix = px0 < px1 ? px1 : px0;
            float rloz = pz0 < pz1 ? pz0 : pz1, rhiz = pz0 < pz1 ? pz1 : pz0;
            for (int sg = 0; sg < w->nseg; sg++) {
                const CueSeg *g = &w->seg[sg];
                float slox = g->a.x < g->b.x ? g->a.x : g->b.x;
                float shix = g->a.x < g->b.x ? g->b.x : g->a.x;
                float sloz = g->a.z < g->b.z ? g->a.z : g->b.z;
                float shiz = g->a.z < g->b.z ? g->b.z : g->a.z;
                if (slox > rhix || shix < rlox) continue;   /* nowhere near */
                if (sloz > rhiz || shiz < rloz) continue;
                if (xz_cross(px0, pz0, px1, pz1,
                             g->a.x, g->a.z, g->b.x, g->b.z)) return 0;
            }
        }
    }
    return 1;
}

int cue_rules_pool_snookered(const CueRules *r, const CueBall *b, int n,
                             const CueWorld *w) {
    if (!r || r->kind || !b || !b[0].on) return 0;
    int any_target = 0;
    for (int i = 1; i < n; i++) {
        if (!b[i].on || !cue_rules_ball_legal(r, b, n, b[i].id)) continue;
        any_target = 1;
        if (clear_path(b[0].pos, b[i].pos, r->R, b, n, i, w)) return 0;
    }
    return any_target;
}

int cue_rules_is_snookered(const CueRules *r, const CueBall *b, int n,
                           const CueWorld *w) {
    if (!r->kind || !b[0].on) return 0;       /* snooker only; cue must be on */
    int any_target = 0;
    for (int i = 1; i < n; i++) {
        if (!b[i].on || !snk_on(r, b[i].id)) continue;
        any_target = 1;
        if (clear_path(b[0].pos, b[i].pos, r->R, b, n, i, w)) return 0; /* one is visible */
    }
    return any_target;                        /* all targets blocked → snookered */
}

/* Would the incoming player be snookered wherever in the D they put the ball?
 * Sampled rather than solved: the D is a half-disc and the answer only has to be
 * as good as a referee's eye. A ring of positions round the arc plus the middle
 * covers it — if any one of them can see a ball on, there is no free ball. */
static int snookered_from_whole_d(CueRules *r, CueBall *b, int n,
                                  const CueWorld *w) {
    if (r->d_radius <= 0.0f) return 0;
    Vec3 keep = b[0].pos; int keep_on = b[0].on;
    b[0].on = 1;
    int snookered = 1;
    const int RINGS = 3, ARC = 7;
    for (int ring = 1; ring <= RINGS && snookered; ring++) {
        float rr = r->d_radius * ((float)ring / RINGS) * 0.92f;
        for (int k = 0; k < ARC && snookered; k++) {
            float a = 1.5707963f + 3.14159265f * ((float)k / (ARC - 1));
            b[0].pos = v3(r->baulk_x + rr * cosf(a), keep.y, rr * sinf(a));
            if (!cue_rules_is_snookered(r, b, n, w)) snookered = 0;
        }
    }
    if (snookered) {                       /* and the centre of the D */
        b[0].pos = v3(r->baulk_x, keep.y, 0.0f);
        if (!cue_rules_is_snookered(r, b, n, w)) snookered = 0;
    }
    b[0].pos = keep; b[0].on = keep_on;
    return snookered;
}

/* ---- THE MISS JUDGEMENT'S BOOKENDS ---------------------------------------- *
 * See cue_rules.h. The set of legal balls-on is decided BEFORE the stroke —
 * the same set resolve_snooker will judge against — and the free ball is read
 * without being consumed, because consuming it is resolve's job. */
void cue_rules_attempt_begin(CueRules *r, const CueBall *b, int n) {
    if (!r) return;
    r->att_have = 0;
    /* ENGLISH BILLIARDS TAKES ITS BAULK PICTURE HERE, before the snooker gate
     * below turns this call away (billiards has kind 0 — it is scored in points
     * and it is not snooker). Rule 6 and Rule 16 both turn on where the object
     * balls were WHEN THE STROKE WAS PLAYED, and by the time resolve runs they
     * have been hit; this is the one call the host already makes at the right
     * moment, with the right balls, for every striker including the machine. */
    if (r->mode == CUE_GAME_BILLIARDS && b && n >= 2) {
        r->bil_red_baulk = r->bil_wht_baulk = 0;
        for (int i = 1; i < n && i < CUE_MAX_BALLS; i++) {
            if (!b[i].on) continue;
            /* Section 2 Definition 14: "A ball is in Baulk when it rests on the
             * Baulk-line or between that line and the bottom cushion." The
             * baulk end is -x, so that is everything at or below baulk_x. */
            const int in_b = (b[i].pos.x <= r->baulk_x);
            if (b[i].id == CUE_ID_BIL_RED) r->bil_red_baulk = in_b;
            else                           r->bil_wht_baulk = in_b;
        }
    }
    if (!r->kind || !b || n < 2 || !b[0].on) return;
    const int fb = r->free_ball, fb_id = r->free_ball_id;
    for (int i = 0; i < n && i < CUE_MAX_BALLS; i++) {
        r->att_on[i] = 0;
        if (i == 0 || !b[i].on) continue;
        const int on = snk_on(r, b[i].id) ||
                       (fb && (fb_id == 0 || b[i].id == fb_id));
        if (!on) continue;
        r->att_on[i] = 1;
        r->att_pre[i] = v3_len(v3_sub(b[i].pos, b[0].pos));
    }
    r->att_have = 1;              /* half: end() must still run */
}

void cue_rules_attempt_end(CueRules *r, const CueWorld *w, const CueBall *b, int n) {
    (void)b;
    if (!r || !w || !r->att_have || !r->kind) { if (r) r->att_have = 0; return; }
    /* The ball the attempt came NEAREST is the one the referee judges it
     * against — a stroke aimed at one red is not marked down for the fourteen
     * it ignored. */
    float best_gap = 1.0e9f, best_pre = 0.0f;
    for (int i = 1; i < n && i < CUE_MAX_BALLS; i++) {
        if (!r->att_on[i]) continue;
        if (w->att_min[i] >= 1.0e8f) continue;          /* never sampled */
        const float gap = w->att_min[i] - 2.0f * w->R;  /* surface to surface */
        if (gap < best_gap) { best_gap = gap; best_pre = r->att_pre[i]; }
    }
    if (best_gap >= 1.0e8f) { r->att_have = 0; return; } /* no legal ball existed */
    if (best_gap < 0.0f) best_gap = 0.0f;                /* touched it */
    r->att_gap  = best_gap / (2.0f * w->R);
    r->att_dist = best_pre;
    const float need = best_pre > 0.05f ? best_pre : 0.05f;
    r->att_pace = w->att_path / need;
    r->att_have = 2;              /* both halves: resolve may judge */
}

/* Was this failure to hit a good enough attempt for the standard in force?
 * The numbers are the product of the tuning in test_missrule.c — change them
 * there first, where every scenario is asserted at all three standards. */
static int miss_attempt_ok(const CueRules *r) {
    static const float GAP[4]  = { -1.0f, 3.00f, 1.50f, 0.30f };  /* ball widths */
    static const float PACE[4] = {  0.0f, 0.60f, 0.90f, 1.05f };
    const int lvl = r->miss_level;
    if (lvl <= 0 || lvl > 3) return 0;          /* OFF: every failure is a miss */
    if (r->att_have != 2)    return 0;          /* no data: judge as before 2.0 */
    /* THE TOLERANCE GROWS WITH THE SHOT. Three times the room out of a snooker,
     * because the referee judges the attempt and an escape off three cushions
     * that lands a ball-width away IS a good attempt; and almost half a ball
     * width more per metre of shot, because nobody is called for drift on a
     * twelve-foot roll that a foot-long tap would be called for. */
    float allow = GAP[lvl];
    if (r->was_snookered) allow *= 3.0f;
    allow *= 1.0f + 0.45f * r->att_dist;
    return r->att_gap <= allow && r->att_pace >= PACE[lvl];
}

static void resolve_snooker(CueRules *r, CueBall *b, int n, const CueWorld *w,
                            int first_hit,
                            int scratch, int cushion, const int *potted, int np) {
    r->break_shot = 0;            /* the opening break is over once it's resolved */
    int target_before = r->target;
    /* Reds remaining = what's actually on the table (post-shot). Tracking a
     * counter drifted when a red was potted on a foul: it was removed from the
     * table but never decremented, so reds_left stayed >0 and the state was
     * stuck ON RED after the last red. Count the table instead. */
    int reds_left = 0;
    for (int i = 0; i < n; i++) if (b[i].on && is_red(b[i].id)) reds_left++;
    r->reds_left = reds_left;
    /* Free ball (awarded when the incoming player was snookered): for this one
     * shot, ANY ball may be struck/potted as the ball-on, scoring the ball-on's
     * value. Consumed whether the shot is legal or a foul. */
    int fb = r->free_ball; r->free_ball = 0;
    int fb_id = r->free_ball_id; r->free_ball_id = 0;
    /* THE NOMINATION IS THE WHOLE OF A FREE BALL. WPBSA Section 3 Rule 12: the
     * striker names a ball and for that stroke the NAMED ball is the ball on.
     * Any other is not, and hitting or potting one is an ordinary foul.
     *
     * This id was read and voided, so "a free ball is in play" meant "every
     * ball that is not the ball on is in play": you could nominate the brown,
     * pot the blue, score for it, and watch the frame move on to the next ball
     * while the brown you named stood there untouched. Which is exactly how it
     * was reported.
     *
     * Nothing nominated stays permissive, deliberately. A free ball with no
     * name attached is a state the rules do not produce, and refusing every
     * ball would leave the striker unable to play at all — much worse than the
     * laxity it would be fixing. */
    #define FB_OK(id) (fb && (fb_id == 0 || (id) == fb_id))
    int nominated_before = r->nominated;
    int bon_val = (target_before == 2) ? r->seq : 1;   /* value of the red/clearance ball-on */
    int legal_pots = 0, illegal_pot = 0, maxpot = 0, reds_potted = 0;
    /* Did the ball ON itself go down, as opposed to a free ball standing in for
     * it? In the clearance the difference is the whole frame: a free ball
     * SCORES the ball-on's value but the ball on is still sitting there. */
    int on_potted = 0;
    for (int k = 0; k < np; k++) {
        int on = snk_on(r, potted[k]);
        int as_fb = !on && FB_OK(potted[k]);
        if (on)        { legal_pots += snk_value(potted[k]); on_potted = 1; }
        else if (as_fb)  legal_pots += bon_val;        /* free-ball pot scores the ball-on */
        else             illegal_pot = 1;
        if (on ? is_red(potted[k]) : (as_fb && target_before == 0)) reds_potted++;
        if (snk_value(potted[k]) > maxpot) maxpot = snk_value(potted[k]);
    }
    int foul = 0;
    if (scratch || first_hit < 0 || illegal_pot ||
        (!snk_on(r, first_hit) && !FB_OK(first_hit))) foul = 1;
    /* A JUMP SHOT IS A FOUL — WPBSA Section 3, Rule 11(a)(x), "playing a jump
     * shot", at the value of the ball on. Whether the shot WAS one is Section 2,
     * Definition 20, and it is not "the cue ball left the bed": it is passing
     * over part of an object ball, with three exceptions for doing so after a
     * contact. Only the integrator can see that, so it arrives as a verdict
     * (CueWorld.jump_over) rather than being decided here.
     *
     * The fv calculation below already prices it correctly: for a jump whose
     * first contact was legal, max(4, ball-on, first-hit, potted) reduces to
     * the value of the ball on, minimum four. */
    if (r->jumped) foul = 1;
    /* And so is putting a ball off the table, in every game there is. */
    if (r->n_off) foul = 1;
    /* SHOOTOUT: every stroke must pot a ball or drive one to a cushion —
     * the format has no place to hide. Judged only when nothing else already
     * fouled the stroke, so the message below can name it. */
    int shoot_norail = 0;
    if (r->snk_shootout && !foul && np == 0 && !cushion) { foul = 1; shoot_norail = 1; }
    r->last_foul = foul;

    /* Respot every potted colour unless it was legally cleared IN SEQUENCE — a
     * free-ball colour always comes back, even in the clearance phase.
     *
     * "Was it the ball on" rather than "was a free ball in play": with a free
     * ball up, `fb` is true for the whole stroke, so a shot that potted the
     * free ball AND the actual ball on put the ball on back on its spot too,
     * and the clearance could not be finished. snk_on still reads the PRE-shot
     * target here, which is the question being asked. */
    for (int k = 0; k < np; k++)
        if (is_colour(potted[k]) &&
            (foul || target_before != 2 || !snk_on(r, potted[k])))
            respot_colour(r, b, n, potted[k]);

    if (foul) {
        int off = r->turn, opp = 1 - off;
        /* "Miss" = failed to HIT a ball-on (air shot or wrong first ball); an
         * illegal pot off a correct first contact is a foul but NOT a miss.
         * Evaluated against the pre-shot target (r->target still == target_before). */
        int is_miss = (first_hit < 0) || (!fb && !snk_on(r, first_hit));

        /* Max(4, value of the ball ON, value of the first ball hit, value of any
         * ball potted). The ball-on's value while on a colour is the NOMINATED
         * colour — and 7 if none was named, because an unnominated colour could
         * have been any of them and the rule prices the striker's uncertainty
         * against them. This read 1 before, so every foul on a colour cost the
         * minimum 4 however dear the colour was. */
        int fv = 4;
        int tv = (target_before == 2) ? r->seq
               : (target_before == 1) ? (nominated_before ? nominated_before : 7)
               : 1;
        if (tv > fv) fv = tv;
        if (first_hit >= 0 && snk_value(first_hit) > fv) fv = snk_value(first_hit);
        if (maxpot > fv) fv = maxpot;
        r->score[opp] += fv;
        r->last_foul_pts = fv;   /* what the referee reads out after the call */
        r->brk = 0;

        int target_after = (r->reds_left > 0) ? 0 : 2;
        int seq_after = (target_after == 2 && r->seq < 2) ? 2 : r->seq;

        /* Snookers-needed exemption: a player who can no longer catch up on the
         * balls left (deficit beyond what's still on the table) is exempt from
         * the miss rule — no "miss" is called, but 3 misses still forfeits. */
        int remaining;
        if (r->reds_left > 0) remaining = r->reds_left * 8 + 27;   /* reds(+black) + colours */
        else { remaining = 0; for (int v = (seq_after < 2 ? 2 : seq_after); v <= 7; v++) remaining += v; }
        int deficit = r->score[opp] - r->score[off];
        int needs_snookers = deficit > remaining;
        /* NOT conditioned on having been snookered. Failing to escape a snooker
         * is the commonest foul and a miss there is; suppressing the call there
         * meant the opponent was never once offered the replay. Only the
         * snookers-needed exemption suppresses it, per WPBSA. */
        /* THE JUDGEMENT. A failure to hit is only CALLED a miss when the
         * attempt was not good enough for the standard in force — see
         * miss_attempt_ok. At level 0 (the handheld, and any stroke with no
         * attempt data) every failure is called, exactly as before. */
        int miss_called = is_miss && !needs_snookers && !miss_attempt_ok(r);
        /* SHOOTOUT has no miss rule and no replays: a foul is ball in hand to
         * the opponent — anywhere on the table — and play goes on. The clock
         * is the pressure; the decision menu would just burn it. */
        if (r->snk_shootout) miss_called = 0;
        r->last_miss = miss_called;

        /* 3-consecutive-miss forfeit (genuine, non-snookered misses only) */
        /* CALLED misses, not raw failures: an attempt the referee accepted
         * must not count towards losing the frame. */
        if (miss_called && !r->was_snookered) {
            if (++r->cmiss[off] >= 3) {
                r->frame_over = 1; r->winner = opp;
                book_frame(r, r->winner);
                snprintf(r->msg, sizeof r->msg, "3 MISSES - LOSS");
                return;
            }
        } else if (!miss_called) r->cmiss[off] = 0;

        /* Is the incoming player snookered on the post-foul ball-on? → free ball.
         * (Skipped after a scratch — the cue is replaced in the D.) */
        int opp_snk = 0;
        {
            int sv_t = r->target, sv_s = r->seq, sv_n = r->nominated;
            r->target = target_after; r->seq = seq_after; r->nominated = 0;
            /* After a scratch the incoming player picks their own spot in the D,
             * so they are only snookered if they are snookered from EVERY spot
             * in it. This case was skipped entirely, so a free ball that was due
             * after a scratch was never awarded. */
            opp_snk = scratch ? snookered_from_whole_d(r, b, n, w)
                              : cue_rules_is_snookered(r, b, n, w);
            r->target = sv_t; r->seq = sv_s; r->nominated = sv_n;
        }

        r->target = target_after; r->seq = seq_after; r->nominated = 0;
        r->dec_offender = off; r->dec_penalty = fv; r->dec_scratch = scratch;
        /* ...and after a scratch played from a snooker: the opponent may prefer
         * to put the striker back in the trouble they were in than to take the
         * table with the cue ball in the D. */
        r->dec_can_restore = miss_called || (scratch && r->was_snookered);
        r->dec_free_ball = opp_snk;

        /* Name the two the striker cannot possibly work out from the score.
         * Every other snooker foul is legible from the table; "you may not do
         * that at all" is not, and a penalty with no stated reason reads as the
         * game being broken. */
        const char *why = r->jumped ? "JUMP " : r->n_off ? "OFF TABLE " : "";
        if ((miss_called || opp_snk) && !r->snk_shootout) {
            /* a real choice exists → park for the opponent's decision */
            r->decision = CUE_DEC_PENDING;
            snprintf(r->msg, sizeof r->msg, miss_called ? "%sFOUL & MISS +%d"
                                                        : "%sFOUL +%d", why, fv);
        } else {
            r->turn = opp;
            if (scratch) r->ball_in_hand = 1;
            if (r->snk_shootout) {
                r->ball_in_hand = 1;             /* every foul: in hand */
                r->decision = CUE_DEC_NONE;
                snprintf(r->msg, sizeof r->msg, "%sFOUL +%d%s", why, fv,
                         shoot_norail ? " (NO RAIL)" : "");
            } else
            snprintf(r->msg, sizeof r->msg, "%sFOUL +%d", why, fv);
        }
        return;
    }

    /* legal */
    r->score[r->turn] += legal_pots;
    r->brk += legal_pots;
    if (legal_pots > 0) r->cmiss[r->turn] = 0;     /* a pot resets the miss counter */

    if (target_before == 0) {                 /* was on a red */
        if (reds_potted > 0) r->target = 1;   /* now a colour */
    } else if (target_before == 1) {          /* was on a colour */
        if (r->reds_left > 0) r->target = 0;
        else { r->target = 2; r->seq = 2; }   /* clearance from yellow */
    } else {                                  /* clearance */
        /* THE SEQUENCE MOVES ON WHEN THE BALL ON IS POTTED, and not merely when
         * something scored. WPBSA Section 3 Rule 12: a free ball is potted, it
         * is SPOTTED, and the value of the ball on is scored — the ball on has
         * not been potted and is still the ball on.
         *
         * This advanced on `legal_pots > 0`, so taking a free ball in the
         * clearance skipped a colour permanently. Reported exactly: on the blue
         * with the pink as a free ball, the pink went down, the pink came back,
         * and the frame moved on to the pink and the black with the blue still
         * standing on its spot and no way ever to be on it again. */
        if (on_potted) {
            r->seq++;
            if (r->seq > 7) {
                /* Level after the black is not a win for whoever the array puts
                 * first: the black goes back on its spot and the frame is played
                 * for it. The old code handed the frame to player 0. */
                if (r->score[0] == r->score[1]) {
                    respot_colour(r, b, n, CUE_ID_BLACK);
                    r->seq = 7; r->target = 2; r->nominated = 0;
                    r->turn = 1 - r->turn;    /* the non-potter plays first */
                    r->brk = 0;
                    r->ball_in_hand = 1;      /* ...from in hand, WPBSA */
                    snprintf(r->msg, sizeof r->msg, "RESPOTTED BLACK");
                    return;
                }
                r->frame_over = 1;
                r->winner = (r->score[0] > r->score[1]) ? 0 : 1;
                book_frame(r, r->winner);
                snprintf(r->msg, sizeof r->msg, "FRAME OVER");
                return;
            }
        }
    }
    /* The nomination belongs to one visit to a colour and nothing else. */
    if (r->target != 1) r->nominated = 0;

    if (legal_pots > 0) { snprintf(r->msg, sizeof r->msg, "BREAK %d", r->brk); }
    else {
        r->cmiss[r->turn] = 0;          /* a legal shot (good safety) clears misses */
        r->brk = 0; r->turn = 1 - r->turn;
        r->target = (r->reds_left > 0) ? 0 : 2;
        if (r->target == 2 && r->seq < 2) r->seq = 2;
        r->nominated = 0;
        r->msg[0] = 0;
    }
}

/* ---- THE ROTATION GAMES: 9-BALL AND 10-BALL --------------------------- *
 *
 * One resolver, because they are one game. Lowest ball first, three consecutive
 * fouls and you have lost, a push-out after the break, and one ball that ends
 * the frame when it goes in legally. What differs is how many balls there are
 * and which one is the money ball — nine out of a diamond, or ten out of a
 * triangle — so those are the two things parameterised and nothing else is.
 *
 * They were not one function to begin with, and the money ball was the literal
 * 9 in five places. Adding 10-ball by copying it would have meant five more,
 * and the interesting bug in a copied resolver is the line you forgot to change:
 * a 10-ball frame that ends when the NINE goes in reads as the game working
 * until somebody pots them out of order. */
static int rot_money(const CueRules *r) {
    return CUE_GAME_MONEY_BALL(r->mode);
}
static int rot_lowest(const CueRules *r, const CueBall *b, int n) {
    const int top = rot_money(r);
    int lo = 99;
    for (int i = 0; i < n; i++)
        if (b[i].on && b[i].id >= 1 && b[i].id <= top && b[i].id < lo) lo = b[i].id;
    return lo == 99 ? 0 : lo;
}
static void respot_money(CueRules *r, CueBall *b, int n) {
    CueBall *q = find_ball(b, n, rot_money(r));
    if (!q) return;
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0);
    q->pos = r->spot[0]; q->orient = m3_identity();
}

static void resolve_9ball(CueRules *r, CueBall *b, int n, const CueWorld *w,
                          int first_hit,
                          int scratch, int cushion, const int *potted, int np) {
    int was_break = r->break_shot;
    const int money = rot_money(r);
    /* The call, read and consumed whatever happens next — a call outlives
     * nothing, exactly as at straight pool. */
    const int called_id  = r->nominated;
    const int called_pkt = r->called_pocket;
    r->nominated = 0; r->called_pocket = -1;
    /* Call-shot is in force for this stroke: 10-ball with the option on, after
     * the break. WPA 9.1: the break is not a called shot. */
    const int callshot = r->call_shot_on && r->mode == CUE_GAME_US10 &&
                         !was_break && !r->is_pushout;
    /* Was the call made — the called ball, down the called pocket? A call with
     * no pocket (the app could not infer one and none was given) can never be
     * made, which is the incentive to call. */
    int made_call = 0;
    if (callshot && called_id) {
        for (int k = 0; k < np; k++) {
            if (potted[k] != called_id) continue;
            const CueBall *q = find_ball(b, n, called_id);
            if (!q || q->pocket == CUE_OFF_TABLE) continue;
            if (called_pkt >= 0 && (int)q->pocket == called_pkt) made_call = 1;
        }
    }

    /* Push-out (WPA): the shot carries no obligation to hit the lowest ball or
     * drive a ball to a rail — the ONLY foul is pocketing the cue ball. A potted
     * 9 is spotted (no win). The opponent then chooses to play from here or
     * pass the shot back. */
    if (r->is_pushout) {
        r->is_pushout = 0; r->pushout_avail = 0; r->break_shot = 0;
        for (int k = 0; k < np; k++) if (potted[k] == money) respot_money(r, b, n);
        r->seq = rot_lowest(r, b, n);
        if (scratch) {                              /* the one push-out foul */
            r->last_foul = 1;
            r->cfoul[r->turn]++;
            r->turn = 1 - r->turn; r->ball_in_hand = 1;
            snprintf(r->msg, sizeof r->msg, "PUSH-OUT FOUL");
            return;
        }
        r->turn = 1 - r->turn;                      /* opponent decides */
        r->pushout_resp = 1; r->msg[0] = 0;
        return;
    }

    /* lowest ball at the START of the shot = min(still-on, potted-this-shot) */
    int lowest = rot_lowest(r, b, n);
    int nine_potted = 0;
    for (int k = 0; k < np; k++) {
        if (potted[k] == money) nine_potted = 1;
        if (lowest == 0 || potted[k] < lowest) lowest = potted[k];
    }
    if (lowest == 0) lowest = 1;

    int foul = 0; const char *why = "";
    /* WPA 5.3 (nine-ball) and 6.3 (ten-ball): the break must pocket a ball or
     * drive FOUR object balls to a rail. A plain foul in both books, and 5.7 /
     * 6.9 hand the incoming player the cue ball in hand anywhere — which is
     * what the foul path below already does, so it need only be named.
     *
     * Regulation 16's other test — three object balls past the HEAD STRING —
     * is deliberately not here. It is a Regulation and not a Rule (Reg 1: "the
     * Rules have priority"), it is a tournament option rather than the game,
     * and the head string is a different line from the one brk_crossed
     * measures. */
    if (was_break && np == 0 && brk_rails(w, n) < 4) { foul = 1; why = "BREAK"; }
    if (scratch)                      { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0)           { foul = 1; why = "NO BALL"; }
    else if (first_hit != lowest)     { foul = 1; why = "WRONG BALL"; }   /* must hit lowest first */
    else if (np == 0 && !cushion)     { foul = 1; why = "NO RAIL"; }      /* table scratch */
    if (r->n_off && !foul)            { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    /* the 9: potted legally wins (incl. on the break); on a foul it respots.
     * Under call-shot the money only wins AS CALLED — potted legally any other
     * way it is spotted, WPA 9.5, and the stroke is judged like any other. */
    if (nine_potted) {
        const int wins = !foul && (!callshot || (called_id == money && made_call));
        if (wins) { r->frame_over = 1; r->winner = r->turn; book_frame(r, r->winner);
                    snprintf(r->msg, sizeof r->msg, "%d-BALL!", money); return; }
        respot_money(r, b, n);
    }

    if (foul) {
        r->cfoul[r->turn]++;
        if (r->cfoul[r->turn] >= 3) {           /* three consecutive fouls = loss */
            r->frame_over = 1; r->winner = 1 - r->turn; book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg, "3 FOULS - LOSS"); return;
        }
        r->turn = 1 - r->turn; r->ball_in_hand = 1;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        r->break_shot = 0; r->seq = rot_lowest(r, b, n); return;
    }
    r->cfoul[r->turn] = 0;
    /* NOT AS CALLED is not a foul — the table simply passes, with everything
     * staying down (the 10 has already gone back). The other player takes it
     * as it lies: no ball in hand, WPA. */
    if (callshot && np > 0 && !made_call) {
        r->turn = 1 - r->turn;
        snprintf(r->msg, sizeof r->msg, "NOT AS CALLED");
    }
    else if (np > 0) r->msg[0] = 0;              /* potted legally → carry on */
    else { r->turn = 1 - r->turn; r->msg[0] = 0; }
    r->break_shot = 0; r->seq = rot_lowest(r, b, n);

    /* After the opening break, the player now at the table may push out. */
    if (was_break && !r->frame_over) { r->pushout_avail = 1; r->pushout_offer = 1; }
}

/* ---- PAUL ---------------------------------------------------------------- *
 *
 * A game two friends invented on a 6 ft home snooker table, and the only one in
 * this file whose rules came from a person. Written down as they were described,
 * which means the odd corner of it is left odd rather than tidied into whatever
 * a federation would have done — a rule set that has been played is worth more
 * than one that is consistent.
 *
 *   THE WHOLE SET GOES ON AT RANDOM. See rack_paul. There is nothing to break.
 *
 *   THE "BREAK" IS A STROKE THAT MUST TOUCH NOTHING. Tap the white, and if it
 *   reaches a ball or a cushion that is a foul and the opponent has two shots.
 *   It is the exact inverse of every other opening stroke in this file, where
 *   failing to reach something is the offence — and it is the rule that makes
 *   the random scatter a problem to be solved rather than a picture: the white
 *   lands where it lands, and finding it room is the first thing you have to do.
 *
 *   THEN ALTERNATE VISITS AND POT WHAT YOU CAN REACH. No ball on, no order, no
 *   nomination. A red is one, a colour two, the black four.
 *
 *   AN IN-OFF IS THE FOUL, and it costs two shots. That is the only penalty
 *   named, so it is the only one here: missing everything simply ends the visit,
 *   as it would on a table with nobody refereeing. A ball driven off the table
 *   goes with the in-off, because a ball on the carpet has to be a foul in any
 *   game or it becomes a tactic.
 *
 *   NOTHING EVER COMES BACK. No spotting, so the points on the table only fall,
 *   and the frame ends the moment one player's lead is larger than what is
 *   left. Level with an empty table re-spots the black and it is played for.
 *
 * TWENTY-NINE POINTS on the table at the start: fifteen reds, five colours at
 * two, and the black at four. */
static int paul_value(int id) {
    if (is_red(id)) return 1;
    if (id == CUE_ID_BLACK) return 4;
    if (is_colour(id)) return 2;
    return 0;
}

/* What is still to play for. */
static int paul_left(const CueBall *b, int n) {
    int pts = 0;
    for (int i = 1; i < n; i++) if (b[i].on) pts += paul_value(b[i].id);
    return pts;
}

/* Is this frame over, and who won it? The only ending condition is a lead
 * bigger than what is left — and, once nothing is left, a lead at all. */
static void paul_maybe_over(CueRules *r, CueBall *b, int n) {
    const int left = paul_left(b, n);
    r->paul_left = left;
    const int lead = r->score[0] - r->score[1];
    const int a = lead < 0 ? -lead : lead;
    if (a > left) {
        r->frame_over = 1;
        r->winner = (lead > 0) ? 0 : 1;
        book_frame(r, r->winner);
        snprintf(r->msg, sizeof r->msg, "%d AHEAD WITH %d LEFT",
                 a, left);
        return;
    }
    if (left == 0 && a == 0) {
        /* LEVEL AND EMPTY. Snooker's answer, because it is the only one that
         * does not decide a frame with something other than a stroke: the black
         * goes back up and it is played for. It is worth four, so the moment it
         * is potted the lead is larger than the nothing that is left and the
         * test above ends the frame on its own. */
        CueBall *q = find_ball(b, n, CUE_ID_BLACK);
        if (q) {
            q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0);
            q->drop = 0.0f; q->pocket = 0; q->orient = m3_identity();
            q->pos = r->spot[7];
            /* AND THE COUNT GOES BACK WITH IT. This was set from the empty
             * table above, so the board said nothing was left while a black
             * stood on its spot — and the frame's only ending condition is a
             * comparison against that number. */
            r->paul_left = paul_left(b, n);
            snprintf(r->msg, sizeof r->msg, "LEVEL - BLACK RESPOTTED");
        }
    }
}

static void resolve_paul(CueRules *r, CueBall *b, int n, int first_hit,
                         int scratch, int cushion, const int *potted, int np)
{
    const int was_break = r->break_shot;
    r->break_shot = 0;

    /* ---- what it was worth ---- */
    int pts = 0, potted_any = 0;
    for (int k = 0; k < np; k++) {
        if (potted[k] == CUE_ID_CUE) continue;         /* the white is not a score */
        pts += paul_value(potted[k]);
        potted_any = 1;
    }

    /* ---- and whether it was a foul ---- */
    int foul = 0; const char *why = "";
    if (was_break) {
        /* THE ONE STROKE THAT MUST REACH NOTHING. Either contact fouls it, and
         * so does potting anything with it — a ball down off a stroke that was
         * supposed to touch nothing is a ball that was touched. */
        /* SHORT ENOUGH FOR THE BOARD. CueRules::msg is 24 characters because it
         * is one HUD line, and "FOUL: " takes six of them — so a reason of more
         * than seventeen is silently cut off mid-word. RAIL rather than CUSHION
         * for the same reason, and it is the word the other resolvers here
         * already use. */
        if (first_hit >= 0) { foul = 1; why = "BREAK HIT A BALL"; }
        else if (cushion)   { foul = 1; why = "BREAK HIT A RAIL"; }
        else if (potted_any || scratch) { foul = 1; why = "BREAK POTTED"; }
    }
    if (!foul && scratch)   { foul = 1; why = "IN OFF"; }
    if (!foul && r->n_off)  { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    /* NOTHING SCORES ON A FOUL, and what went down stays down — there is no
     * spotting in this game, so a ball potted on a foul is simply lost to
     * whoever might have had it. That is harsher than snooker and softer than
     * bar billiards, and it is what "nothing ever comes back" means. */
    if (foul) {
        r->brk = 0;
        r->turn = 1 - r->turn;
        /* TWO SHOTS, on the same machinery UK 8-ball's pub rules use, so the
         * board's "2 SHOTS" and the shot counting are the ones already tested. */
        r->two_shot = 1; r->shots_remaining = 2; r->free_shot = 1;
        /* The white comes back in hand only if it went down. Otherwise it lies
         * where it stopped, like every other two-shot game here. */
        r->ball_in_hand = scratch ? 1 : 0;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        paul_maybe_over(r, b, n);
        return;
    }

    if (pts > 0) {
        r->score[r->turn] += pts;
        r->brk += pts;
        /* A pot cancels any two-shot advantage carried into the visit, exactly
         * as it does under pub rules: the carry is compensation for being put in
         * a bad place, and a scoring stroke says you were not. */
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        snprintf(r->msg, sizeof r->msg, "%d", r->brk);
        paul_maybe_over(r, b, n);
        return;
    }

    /* A miss. NOT A FOUL — the only penalty this game names is the in-off, and
     * a table with nobody refereeing does not fine you for trying something and
     * failing. It just ends the visit, unless a shot is still owed. */
    if (r->shots_remaining > 1) {
        r->shots_remaining--; r->free_shot = 0;
        snprintf(r->msg, sizeof r->msg, "2ND SHOT");
        return;
    }
    r->brk = 0;
    r->turn = 1 - r->turn;
    r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
    r->msg[0] = 0;
    paul_maybe_over(r, b, n);
}

/* ---- straight pool (14.1 continuous) --------------------------------- *
 *
 * The scoring game, and the one whose shape is unlike everything else here:
 * there is no ball you must hit, no group, no order. Every ball is fair game and
 * worth exactly one point, and the whole of the skill is in saying beforehand
 * which one is going where. A frame runs across as many racks as the target
 * takes — fourteen balls go back on the table each time one is left, and the
 * fifteenth stays where it lies to be broken off.
 *
 * WPA 4.14 straight pool, with the interference cases left out — see
 * cue_table_rack_14 for what that means and why. */

static int straight_left(const CueBall *b, int n) {   /* object balls still up */
    int c = 0;
    for (int i = 1; i < n; i++)
        if (b[i].on && b[i].id >= 1 && b[i].id <= 15) c++;
    return c;
}

/* Spot a ball on the foot spot, or as near behind it as it will go — the long
 * string runs from the foot spot toward the foot cushion, which on a rectangle
 * is +x and on an L is wherever the spine points there. Every illegally potted
 * ball in 14.1 comes back this way, and there are a lot of them: an uncalled
 * pot, anything that went down on a foul, anything potted on a safety. */
static void spot_straight(CueRules *r, CueBall *b, int n, int id) {
    CueBall *q = find_ball(b, n, id);
    if (!q) return;
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0); q->drop = 0.0f;
    q->pocket = 0; q->orient = m3_identity();
    Vec3 p = r->spot[0];
    if (spot_taken(b, n, p, id, r->R)) {
        for (int step = 1; step <= 80; step++) {
            float d = (float)step * r->R * 0.5f;
            Vec3 c = p;
            c.x += r->spot_up.x * d; c.z += r->spot_up.z * d;
            if (!spot_taken(b, n, c, id, r->R)) { p = c; break; }
        }
    }
    q->pos = p;
}

/* ---- G2: RUSSIAN PYRAMID ------------------------------------------------ *
 *
 * The simplest scoring in the file and the hardest table in it. Any of the
 * fifteen is a legal ball, in any order, and eight of them takes the frame —
 * more than half, so it cannot be caught. What makes it a game rather than pool
 * with the groups switched off is the pockets, which are 73 mm to a 68 mm ball,
 * and the penalty: a foul PUTS ONE OF YOUR OWN BALLS BACK, so a frame can go
 * backwards and a careless visit hands your opponent something to shoot at.
 *
 * CLASSIC first, which is the white-cue-ball game. COMBAT scores the cue ball
 * potted off an object ball as well — the "свой" the game is known for — and is
 * a two-line difference from here. FREE, where any ball on the table may be
 * played as the cue ball, is not a variant of this function: balls[0] is the
 * white throughout the rules, the AI and the wire, and that is a real assumption
 * to break rather than a flag to add. */
static void resolve_pyramid(CueRules *r, CueBall *b, int n, int first_hit,
                            int scratch, int cushion, const int *potted, int np) {
    (void)b; (void)n;
    const int me = r->turn, you = 1 - r->turn;
    r->break_shot = 0;
    r->respot = 0;

    /* A "свой": the cue ball potted AFTER a legal contact. A foul in Classic and
     * a scored ball in Combat, and the same event either way. */
    const int svoy = scratch && first_hit >= 0;

    int scored = 0;
    for (int k = 0; k < np; k++) if (potted[k] >= 1 && potted[k] <= 15) scored++;
    if (svoy && r->pyr_free == CUE_PYR_COMBAT) scored++;

    int foul = 0; const char *why = "";
    if (first_hit < 0)                    { foul = 1; why = "NO BALL"; }
    else if (scratch && !(r->pyr_free == CUE_PYR_COMBAT))
                                          { foul = 1; why = "SCRATCH"; }
    else if (scored == 0 && !cushion)     { foul = 1; why = "NO RAIL"; }
    if (r->n_off)                         { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    if (!foul) {
        r->score[me] += scored;
        if (r->score[me] >= 8) {
            r->frame_over = 1; r->winner = me; book_frame(r, me);
            snprintf(r->msg, sizeof r->msg, "PYRAMID!");
            return;
        }
        if (scored) { snprintf(r->msg, sizeof r->msg, "%d BALL%s", scored,
                               scored == 1 ? "" : "S"); return; }
        r->turn = you; r->msg[0] = 0;
        return;
    }

    /* The penalty. One ball back for the offender — and only if he has one to
     * give, because a player who has potted nothing cannot pay. Anything potted
     * on the foul shot itself does NOT count first: it is scored and then given
     * back, which nets to nothing and is the same answer by a shorter road. */
    if (r->score[me] > 0) { r->score[me]--; r->respot = 1; }
    r->cfoul[me]++;
    r->turn = you;
    /* Ball in hand from the house, which is where a pyramid foul puts it —
     * behind the line, not anywhere on the table. clamp_region knows the house
     * from the D: the table carries `house`, and the region behind the line is
     * the WHOLE width rather than an arc of d_radius. It did treat the two as
     * the same thing, which chalked a snooker D on a Russian table and then
     * confined the ball to a third of the area the rules give it. */
    r->ball_in_hand = 1;
    snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
}

/* ---- G6: BAR BILLIARDS --------------------------------------------------
 *
 * Nine holes in the bed worth ten to two hundred, three skittles standing
 * among them, and a clock. The scoring is trivial — the value of the hole, and
 * double for the red (AEBBA Rule 97) — and the game is entirely in the
 * penalties, which do not take points off you but take your BREAK off you:
 *
 *   Rule 110  loss of the break score, for failing to hit a ball, for a ball
 *             coming to rest over the baulk line or in the D, for a ball
 *             leaving the table, for knocking a WHITE skittle over, and for a
 *             cue ball that neither reaches the black peg's line nor hits
 *             anything
 *   Rule 111  loss of the ENTIRE score, for knocking the BLACK skittle over
 *   Rule 112  and where both went over, whichever fell FIRST decides which
 *
 * That last one is why the physics records the order the skittles fell in
 * rather than merely that they did.
 *
 * A break runs until the striker fails to pot or fouls (Rule 98). There is no
 * opponent to hand over to in the sense the other games mean it — the players
 * take turns at a table that keeps its own score — so the turn passes at the
 * end of a break exactly as it does everywhere else here.
 */
static void resolve_barbilliards(CueRules *r, CueBall *b, int n, const CueWorld *w,
                                 int first_hit, const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    r->break_shot = 0;
    r->bb_return = 0;

    /* ---- what the skittles did (Rules 110(f), 111(a), 112) ---- */
    int white_down = 0, black_down = 0, white_first = 0;
    if (w) {
        int wo = 0, bo = 0;
        for (int k = 0; k < w->nskittle; k++) {
            if (!w->skittle_down[k]) continue;
            if (w->skittle_black[k]) { black_down = 1; bo = w->skittle_order[k]; }
            else                     { white_down = 1; if (!wo || w->skittle_order[k] < wo)
                                                           wo = w->skittle_order[k]; }
        }
        /* Rule 112: whichever fell first decides the penalty. */
        white_first = (white_down && (!black_down || (wo && bo && wo < bo)));
    }

    /* ---- what went down ---- */
    int pts = 0, potted_any = 0, red_potted = 0;
    for (int k = 0; k < np; k++) {
        int hole = (k < 8) ? r->bb_hole[k] : -1;
        int val = (hole >= 0 && w && hole < w->npocket) ? w->pocket_score[hole] : 0;
        /* Rule 97: the red doubles the value of its hole. */
        if (potted[k] == CUE_ID_BIL_RED) { val *= 2; red_potted = 1; }
        pts += val;
        potted_any = 1;
    }

    /* ---- Rule 108: the last-ball shot plays by its own book ----------------
     *
     * One ball left in the game, played from the centre of the D into the 100
     * or the 200 OFF ONE SIDE CUSHION, with the white skittles standing in the
     * 50 holes. Strike a skittle and the score does not count. The players
     * alternate until the ball is potted or the black peg goes down — so the
     * only ways out of here are GAME, or the turn passing with the ball back
     * on the centre spot. Rules 110(b) and 110(o) explicitly do not apply. */
    if (r->bb_last_ball) {
        int white_touched = 0;
        if (w) for (int k = 0; k < w->nskittle; k++)
            if (!w->skittle_black[k] &&
                (w->skittle_down[k] || w->skittle_nudged[k])) white_touched = 1;
        if (black_down && !white_first) {
            /* Rule 111 still stands, and 108 says the black falling ENDS it. */
            r->score[me] = 0; r->bb_break = 0; r->brk = 0;
            r->frame_over = 1;
            r->winner = (r->score[0] == r->score[1]) ? -1
                      : (r->score[0] > r->score[1]) ? 0 : 1;
            if (r->winner >= 0) book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg, "THE BLACK - SCORE LOST - GAME");
            return;
        }
        if (np > 0 && r->bb_hole[0] >= 0) {
            /* Down a hole: the game is over either way. It scores only if the
             * hole was the 100 or the 200, a side cushion came first, and no
             * skittle in a 50 hole was touched on the way. */
            int val = (w && r->bb_hole[0] < w->npocket)
                    ? w->pocket_score[r->bb_hole[0]] : 0;
            int legal = (val == 100 || val == 200) &&
                        (w && w->side_cushion) && !white_touched;
            if (legal) {
                if (potted[0] == CUE_ID_BIL_RED) val *= 2;
                r->score[me] += val;
                snprintf(r->msg, sizeof r->msg, "%d - GAME", val);
            } else snprintf(r->msg, sizeof r->msg, "NO SCORE - GAME");
            r->bb_left = 0;
            r->frame_over = 1;
            r->winner = (r->score[0] == r->score[1]) ? -1
                      : (r->score[0] > r->score[1]) ? 0 : 1;
            if (r->winner >= 0) book_frame(r, r->winner);
            return;
        }
        /* Not potted — off the table included, Rule 110(e)'s return being how
         * the ball gets back to the D. The other player has the next attempt. */
        r->turn = you;
        snprintf(r->msg, sizeof r->msg, "LAST BALL");
        return;
    }

    /* ---- the fouls ---- */
    int foul = 0, fatal = 0; const char *why = "";
    if (black_down && !white_first)   { fatal = 1; why = "THE BLACK"; }
    else if (white_down)              { foul = 1; why = "A WHITE SKITTLE"; }
    if (!foul && !fatal) {
        if (first_hit < 0)            { foul = 1; why = "HIT NOTHING"; }   /* 110(b) */
        else if (r->n_off)            { foul = 1; why = "OFF THE TABLE"; } /* 110(e) */
        else if (r->bb_in_baulk)      { foul = 1; why = "BACK OVER THE LINE"; } /* 110(c),(d) */
        else if (r->bb_short)         { foul = 1; why = "SHORT OF THE BLACK"; }  /* 110(o) */
        else if (r->bb_from_break && potted_any && np >= 2 &&
                 r->bb_both_potted + 1 > CUE_BB_MAX_BOTH)
                                      { foul = 1; why = "BOTH, FOUR TIMES"; }    /* 110(a) */
    }
    r->last_foul = foul || fatal;

    if (fatal) {
        /* Rule 111: the whole score, not merely the break. */
        r->score[me] = 0;
        r->bb_break = 0;
        r->brk = 0;
        r->cfoul[me]++;
        r->turn = you;
        r->bb_from_break = 1;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s - SCORE LOST", why);
        return;
    }
    if (foul) {
        /* Rule 110: the break comes off, and only the break. */
        r->score[me] -= r->bb_break;
        if (r->score[me] < 0) r->score[me] = 0;
        r->bb_break = 0;
        r->brk = 0;
        r->cfoul[me]++;
        r->turn = you;
        r->bb_from_break = 1;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s - BREAK LOST", why);
        return;
    }

    /* ---- the score ---- */
    if (!potted_any) {
        /* Rule 98: a break runs until the striker fails to pot. */
        r->bb_break = 0;
        r->brk = 0;
        r->turn = you;
        r->bb_from_break = 1;
        r->msg[0] = 0;
        return;
    }
    r->score[me] += pts;
    r->bb_break += pts;
    r->brk = r->bb_break;
    /* Rule 110(a): potting both from the break position, counted so the fourth
     * consecutive one is a foul. Anything else resets it. */
    if (r->bb_from_break && np >= 2) r->bb_both_potted++;
    else                             r->bb_both_potted = 0;
    /* Rule 94: an empty table sends play back to the break position; and until
     * the bar drops, a potted ball comes back out of the trough (Rule 115's
     * premise). The host counts what is on the table and feeds them. */
    r->bb_return = r->bb_barred ? 0 : np;
    /* Once the bar is down they do not come back. */
    if (r->bb_barred) { r->bb_left -= np; if (r->bb_left < 0) r->bb_left = 0; }
    (void)red_potted; (void)b; (void)n;
    /* Rule 116(e): after the THIRD consecutive both-pot from the break the
     * scorer must clearly warn the player to leave one ball up — without the
     * warning the fourth could not be penalised, so the warning is part of
     * the rule, not a courtesy. */
    if (r->bb_from_break && r->bb_both_potted >= CUE_BB_MAX_BOTH)
        snprintf(r->msg, sizeof r->msg, "%d - LEAVE ONE UP", pts);
    else
        snprintf(r->msg, sizeof r->msg, "%d", pts);
}

/* ---- G11: CAROM ------------------------------------------------------------
 *
 * Every point is a cannon: the cue ball contacts both object balls in one
 * stroke. What the stroke must do FIRST is the whole difference between the
 * games — nothing (straight rail), two cushions, three — and the cue ball's
 * own touch log holds the answer in order, cushions and balls alike. The
 * count that matters is cushions BEFORE the second object ball is first
 * contacted; rails after the cannon is made decorate nothing.
 *
 * Four-ball is the Korean and Japanese game: the objects are the two REDS,
 * and touching the opponent's cue ball at any point ends the turn scoreless.
 *
 * A cannon keeps the striker at the table; anything else passes it. There are
 * no pockets to scratch into; a ball off the table is the one foul the game
 * has, and it goes back on its opening spot.
 */
static void resolve_carom(CueRules *r, CueBall *b, int n, const CueWorld *w,
                          int first_hit)
{
    const int me = r->turn, you = 1 - r->turn;
    const int fourb = (r->mode == CUE_GAME_CAROM_4B);
    r->break_shot = 0;

    /* the two objects: the reds in four-ball, the red and the OTHER cue ball
     * in the three-ball games (index 0 is always the striker's ball — the
     * host exchanges them the way billiards does) */
    const int objA = CUE_ID_BIL_RED;
    const int objB = fourb ? 2
                   : (r->bil_yellow ? CUE_ID_BIL_WHITE : CUE_ID_BIL_YELLOW);
    const int oppw = fourb ? (r->bil_yellow ? CUE_ID_BIL_WHITE
                                            : CUE_ID_BIL_YELLOW) : -1;

    int hitA = 0, hitB = 0, cush = 0, before = -1, touched_opp = 0;
    if (w) {
        for (int i = 0; i < w->ntouch; i++) {
            if (w->touch[i].what == CUE_TOUCH_CUSHION) { cush++; continue; }
            const int id = w->touch[i].id;
            if (fourb && id == oppw) touched_opp = 1;
            const int a = (id == objA), bb2 = (id == objB);
            if (!a && !bb2) continue;
            if (a) { if (!hitA) { hitA = 1; if (hitB && before < 0) before = cush; } }
            else   { if (!hitB) { hitB = 1; if (hitA && before < 0) before = cush; } }
        }
    }
    /* `before` is the cushion count when the SECOND object was first reached;
     * a stroke that never got there scores nothing whatever it did. */
    const int need = r->mode == CUE_GAME_CAROM_1C ? 1
                   : r->mode == CUE_GAME_CAROM_2C ? 2
                   : r->mode == CUE_GAME_CAROM_3C ? 3 : 0;
    const int cannon = hitA && hitB && before >= need && !touched_opp;

    /* the one foul: a ball leaving the table. Back on its opening spot. */
    if (r->n_off) {
        CueTable ot; cue_table_init(&ot, (CueGameKind)r->mode);
        CueBall home[CUE_MAX_BALLS]; const int hn = cue_table_rack(&ot, home);
        for (int i = 0; i < n; i++) {
            if (b[i].on) continue;
            /* BY ID, NOT BY SLOT. The two cue balls are exchanged every time
             * the turn passes so that index 0 is always the ball being struck
             * — so after an odd number of visits slot 0 holds the YELLOW while
             * the fresh rack's slot 0 is the white's spot, and restoring by
             * index put whichever ball went off onto the other one's mark. */
            for (int k = 0; k < hn; k++)
                if (home[k].id == b[i].id) { b[i].pos = home[k].pos; break; }
            b[i].on = 1;
            b[i].vel = v3(0, 0, 0); b[i].w = v3(0, 0, 0);
            b[i].pocket = 0; b[i].drop = 0.0f;
        }
        r->last_foul = 1;
        r->turn = you; r->bil_yellow = !r->bil_yellow;
        snprintf(r->msg, sizeof r->msg, "FOUL: OFF THE TABLE");
        return;
    }
    r->last_foul = 0;

    if (cannon) {
        r->score[me] += 1;
        r->brk += 1;
        if (r->target_score > 0 && r->score[me] >= r->target_score) {
            r->frame_over = 1; r->winner = me; book_frame(r, me);
            snprintf(r->msg, sizeof r->msg, "GAME");
            return;
        }
        snprintf(r->msg, sizeof r->msg, "1");
        return;                                   /* the striker plays on */
    }

    r->brk = 0;
    r->turn = you;
    r->bil_yellow = !r->bil_yellow;
    if (touched_opp) snprintf(r->msg, sizeof r->msg, "TOUCHED WHITE");
    else r->msg[0] = 0;
    (void)first_hit;
}

/* ---- ROTATION, THE FILIPINO GAME, AND FIFTEEN-BALL ----------------------
 *
 * Fifteen balls, the LOWEST always the one on, and a ball is worth its own
 * NUMBER. The numbers 1 to 15 come to 120, so 61 wins it — and the frame can
 * be over with balls still on the table, which is the whole shape of the game:
 * the 15 alone is worth a quarter of what you need, and a player who has taken
 * the low balls cheaply can still lose to one who took the high ones.
 *
 * It is the oldest of the rotation family and the reason the family has that
 * name. The rack puts the 1 nearest you and the 2 and 3 in the far corners, so
 * the opening is a long shot at the ball you are obliged to hit.
 *
 * THE FILIPINO GAME is the same board with two rules changed, and they are the
 * two that decide how it feels:
 *
 *   BALL IN HAND ANYWHERE after a foul, where the classic game sends the
 *   incoming player behind the head string. That single line is most of the
 *   difference: a foul in the classic game leaves you a long way from a pack
 *   at the foot end, and in the Filipino game it hands the table over.
 *
 *   THREE CONSECUTIVE FOULS LOSE THE FRAME, which the classic game does not
 *   have at all — so a player who cannot reach the ball on cannot simply keep
 *   fouling and handing it back.
 *
 * (House rules on the Filipino game vary and the jump-cue and snooker
 * conventions are not modelled. These two are the ones every account agrees on.)
 */
static void resolve_rotation(CueRules *r, CueBall *b, int n, const CueWorld *w,
                             int first_hit,
                             int scratch, int cushion, const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    const int ph = (r->mode == CUE_GAME_ROTATION_PH);
    const int was_break = r->break_shot;
    r->break_shot = 0;

    /* THE BALL ON, read from the table as it was BEFORE the stroke — which is
     * what the striker was obliged to hit, and the potted balls are already
     * off by the time this runs. */
    int lowest = 99;
    for (int i = 1; i < n; i++)
        if (b[i].on && b[i].id >= 1 && b[i].id <= 15 && b[i].id < lowest)
            lowest = b[i].id;
    for (int k = 0; k < np; k++)
        if (potted[k] >= 1 && potted[k] <= 15 && potted[k] < lowest) lowest = potted[k];
    if (lowest == 99) lowest = 0;

    /* FIFTEEN-BALL LETS YOU SHOOT AT ANYTHING, which is the whole of what
     * separates it from rotation: the same fifteen balls, the same number for a
     * score and the same 61 to win, but nobody tells you which one is on. So
     * the obligation below is the rotation family's alone. */
    const int must_hit_lowest = CUE_GAME_IS_ROTATION(r->mode);
    int foul = 0; const char *why = "";
    /* ROTATION FOLLOWS NINE-BALL, which is Mark's decision and not a rule book:
     * the WPA has no Rotation ruleset at all — it is in neither the current
     * book nor the 2016 one — so there is nothing to be faithful to. Nine-ball
     * is the nearest game that IS published (lowest ball first, same rack, same
     * table) and its break rule is the one adopted here. */
    if (was_break && np == 0 && brk_rails(w, n) < 4) { foul = 1; why = "BREAK"; }
    if (first_hit < 0)                    { foul = 1; why = "NO BALL HIT"; }
    else if (must_hit_lowest && lowest && first_hit != lowest)
                                          { foul = 1; why = "WRONG BALL FIRST"; }
    else if (scratch)                     { foul = 1; why = "SCRATCH"; }
    else if (r->n_off)                    { foul = 1; why = "OFF THE TABLE"; }
    else if (!np && !cushion)             { foul = 1; why = "NO CUSHION"; }

    /* WHAT WENT DOWN IS WORTH ITS NUMBER — but only on a legal stroke. A foul
     * scores nothing however many balls dropped, and they STAY down: rotation
     * does not spot them, so the 120 on the table simply gets smaller and the
     * player who fouled has given the points away rather than banked them. */
    int gained = 0;
    if (!foul) for (int k = 0; k < np; k++)
        if (potted[k] >= 1 && potted[k] <= 15) gained += potted[k];

    if (foul) {
        r->last_foul = 1;
        r->cfoul[me]++;
        r->ball_in_hand = 1;
        r->turn = you;
        r->brk = 0;
        /* THREE IN A ROW IS THE FRAME, in the Filipino game only. */
        if (ph && r->cfoul[me] >= 3) {
            r->frame_over = 1; r->winner = you; book_frame(r, you);
            snprintf(r->msg, sizeof r->msg, "THREE FOULS");
            return;
        }
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        return;
    }
    r->last_foul = 0;
    r->cfoul[me] = 0;
    r->score[me] += gained;
    r->seq = 0;
    for (int i = 1; i < n; i++)
        if (b[i].on && b[i].id >= 1 && b[i].id <= 15 &&
            (r->seq == 0 || b[i].id < r->seq)) r->seq = b[i].id;

    if (r->score[me] >= 61) {
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }
    /* AND THE TABLE CAN RUN OUT WITH NOBODY AT 61 — 120 points shared and the
     * higher total takes it, which is the only way a frame ends short. */
    if (r->seq == 0) {
        r->frame_over = 1;
        r->winner = (r->score[0] == r->score[1]) ? -1
                  : (r->score[0] > r->score[1]) ? 0 : 1;
        if (r->winner >= 0) book_frame(r, r->winner);
        snprintf(r->msg, sizeof r->msg, "TABLE CLEARED");
        return;
    }

    if (gained) {
        r->brk += gained;
        snprintf(r->msg, sizeof r->msg, "%d", gained);
        return;                                  /* the striker plays on */
    }
    r->brk = 0;
    r->turn = you;
    r->msg[0] = 0;
}

/* ---- SPEED POOL ----------------------------------------------------------
 *
 * Not a frame. Two attempts at the same task: clear a full rack, and the CLOCK
 * is the score. Lowest time wins, so there is no turn to take from anybody and
 * nothing to defend — your opponent is a number on a board, and the only
 * question on every shot is how fast you can be at the next one.
 *
 * That makes it the odd one out here in a way worth stating: every other game
 * in this file is about denying the other player something. This one cannot be
 * played badly on purpose.
 *
 * ANY BALL, ANY POCKET, and a scratch is not a foul in the usual sense — the
 * cue ball comes back in hand and the clock never stopped, which is the whole
 * of the penalty and a heavier one than a lost turn. Time is the only currency.
 */
static void resolve_speed(CueRules *r, CueBall *b, int n, int first_hit,
                          int scratch, const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    (void)first_hit; (void)potted; (void)np;
    r->break_shot = 0;
    r->last_foul = 0;

    /* A scratch costs you the seconds it takes to place the ball again. */
    if (scratch) r->ball_in_hand = 1;

    int left = 0;
    for (int i = 1; i < n; i++) if (b[i].on) left++;
    if (left > 0) {
        /* Still clearing. The board is the clock and nothing else. */
        r->msg[0] = 0;
        return;
    }

    /* THE RACK IS CLEAR. The host has been writing the elapsed time in all
     * along, so it is already here. */
    r->sp_done[me] = 1;
    if (!r->sp_done[you]) {
        r->turn = you;                 /* their go at the same task */
        r->rerack = 2;                 /* a fresh full rack */
        r->ball_in_hand = 1;
        snprintf(r->msg, sizeof r->msg, "%d.%02d - NOW THEM",
                 r->sp_cs[me] / 100, r->sp_cs[me] % 100);
        return;
    }

    /* Both have gone: the lower time takes it. */
    r->frame_over = 1;
    if (r->sp_cs[0] == r->sp_cs[1]) r->winner = -1;
    else r->winner = (r->sp_cs[0] < r->sp_cs[1]) ? 0 : 1;
    if (r->winner >= 0) book_frame(r, r->winner);
    snprintf(r->msg, sizeof r->msg, "%d.%02d TO %d.%02d",
             r->sp_cs[0] / 100, r->sp_cs[0] % 100,
             r->sp_cs[1] / 100, r->sp_cs[1] % 100);
}

/* ---- HONOLULU ------------------------------------------------------------
 *
 * A straight-in pot scores NOTHING. Every scoring ball has to arrive the hard
 * way — off a bank, off a kick, out of a combination, or by a carom — so the
 * one shot every other game on this table is built around is the only shot
 * this game does not have. Fifteen balls, first to eight.
 *
 * WHAT COUNTS AS "THE HARD WAY", and this is the whole rule:
 *
 *   THE OBJECT BALL BANKED     it found a cushion before it dropped.
 *   THE CUE BALL KICKED        it found one before it reached the object ball,
 *                              which the cue ball's own touch log says in
 *                              order.
 *   A COMBINATION OR CAROM     the ball that dropped was moved by, or moved
 *                              through, another ball.
 *
 * The first is CueWorld::rails, which bank pool needed. The third is
 * CueWorld::balls_hit, which is new: the cue ball's touch log follows the cue
 * ball, and a ball set off by a collision knows that about itself and nothing
 * else does. Between the three there is no straight pot that reads as legal
 * and no legal shot that reads as straight.
 *
 * A ball potted straight is not a foul — it is simply no score, and it goes
 * back on the table like a bank pool ball that never banked.
 */
static void resolve_honolulu(CueRules *r, CueBall *b, int n, const CueWorld *w,
                             int first_hit, int scratch, int cushion,
                             const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    r->break_shot = 0;

    /* DID THE CUE BALL FIND A CUSHION BEFORE IT FOUND A BALL? Read from its
     * own log, in order, which is the only thing that can say "before". */
    int kicked = 0;
    if (w) for (int i = 0; i < w->ntouch; i++) {
        if (w->touch[i].what == CUE_TOUCH_CUSHION) { kicked = 1; break; }
        if (w->touch[i].what == CUE_TOUCH_BALL) break;
    }

    int scored = 0, straight = 0;
    for (int k = 0; k < np; k++) {
        int idx = -1;
        for (int i = 1; i < n; i++) if (b[i].id == potted[k]) { idx = i; break; }
        if (idx < 0 || idx >= CUE_MAX_BALLS) {
            if (straight < 8) r->respot_id[straight] = (unsigned char)potted[k];
            straight++; continue;
        }
        const int banked = w && w->rails[idx] > 0;
        /* CAME THROUGH ANOTHER BALL, which is two different strokes wearing one
         * name. Neither of them is a COUNT of contacts, and both were written
         * as one first time.
         *
         *   A COMBINATION: the cue ball never touched this one at all. Some
         *   other ball did, and that is the whole test — a ball with a single
         *   contact that was not the white came off a combination, and counting
         *   contacts read it as a straight pot.
         *
         *   A CAROM: the white came to this ball OFF ANOTHER ONE. That is a
         *   fact about the white's journey and not about this ball, which is
         *   where the second reading went wrong: "hit by the white and touched
         *   more than once" is false of the ordinary carom, where the white
         *   glances off one ball and pots this with the only contact it has —
         *   Mark's shot exactly, refused every time. And it is TRUE of a plain
         *   straight pot whose object ball happens to brush another ball on its
         *   way to the hole, which should score nothing at all.
         *
         * The white keeps its own log, in order, and that is the only thing
         * that can answer "before" — the same log the kick above is read from.
         */
        const int combo = w && w->balls_hit[idx] > 0 && !w->hit_by_cue[idx];
        int carom = 0;
        if (w && w->hit_by_cue[idx]) {
            int off_a_ball = 0;
            for (int i = 0; i < w->ntouch; i++) {
                if (w->touch[i].what != CUE_TOUCH_BALL) continue;
                if (w->touch[i].idx == idx) { carom = off_a_ball; break; }
                off_a_ball = 1;                /* the white had already found one */
            }
        }
        if (banked || kicked || combo || carom) scored++;
        else {
            if (straight < 8) r->respot_id[straight] = (unsigned char)potted[k];
            straight++;
        }
    }

    int foul = 0; const char *why = "";
    if (first_hit < 0)        { foul = 1; why = "NO BALL HIT"; }
    else if (scratch)         { foul = 1; why = "SCRATCH"; }
    else if (r->n_off)        { foul = 1; why = "OFF THE TABLE"; }
    else if (!np && !cushion) { foul = 1; why = "NO CUSHION"; }

    r->score[me] += scored;
    r->respot     = straight;          /* a straight pot goes back on */

    if (foul) {
        r->last_foul = 1;
        r->cfoul[me]++;
        r->ball_in_hand = 1;
        r->turn = you;
        r->brk = 0;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        return;
    }
    r->last_foul = 0;
    r->cfoul[me] = 0;

    if (r->score[me] >= 8) {
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }
    /* A LEGAL SCORE KEEPS THE TABLE, whatever else went down with it. The
     * straight one is spotted and costs you nothing but itself — it is not a
     * foul, so there is nothing to hand the table over for. Mark asked, and the
     * old reading gave you the point and took the visit, which is a penalty
     * with no rule behind it. */
    if (scored) {
        r->brk += scored;
        if (straight) snprintf(r->msg, sizeof r->msg, "%d - ONE SPOTTED", scored);
        else          snprintf(r->msg, sizeof r->msg, "%d", scored);
        return;                                   /* the striker plays on */
    }
    r->brk = 0;
    r->turn = you;
    if (straight) snprintf(r->msg, sizeof r->msg, "STRAIGHT IN - NO SCORE");
    else          r->msg[0] = 0;
}

/* ---- BOWLLIARDS ----------------------------------------------------------
 *
 * Pocket billiards kept on a ten-pin bowling card, and the second half of that
 * sentence is the whole game: the potting is ordinary call-shot pool, and every
 * decision a player makes comes from the SCORING, which is a bowler's and not a
 * pool player's. It is in the BCA rulebook and the WPA has never sanctioned it,
 * which is why nothing here reads like the other American games on this table.
 *
 * Ten object balls, the 1 to the 10, racked as a four-row triangle. Ten frames.
 * Each frame allows two INNINGS, an inning being a visit to the table that ends
 * when the striker fails to pot the ball he called, or fouls, or clears all ten.
 * Clear the ten in the first inning and it is a STRIKE; clear them across the
 * two and it is a SPARE; anything else is an OPEN frame worth its pinfall. The
 * bonuses are the bowler's exactly — a spare is ten and the next inning, a
 * strike is ten and the next two — so three hundred is the perfect game and it
 * is reachable, which matters more than it sounds and is argued below.
 *
 * A WORD ABOUT THE WORD FRAME, because there are two of them in this file and
 * they are not the same thing. CueRules calls a rack a frame — frame_over,
 * book_frame, cue_rules_next_frame — and bowlliards calls one tenth of a game a
 * frame. The engine's frame is the whole ten of the game's, so r->frame_over
 * means the GAME is decided and bw_frame is the bowling one. Anything that
 * reads only one of the two names will be wrong about which.
 *
 * WHAT THE BREAK IS FOR, which is nothing. The break is not an inning, scores
 * nothing, and every ball it pots is spotted before scoring play begins; a
 * scratch or a jumped cue ball on it carries no penalty at all, and there is no
 * balls-to-cushion requirement. So it is a free spread of the rack and the
 * striker then starts the first inning with the cue ball in hand behind the
 * head string. Scoring the break instead would put an eleventh delivery on a
 * ten-delivery card with no box to write it in.
 *
 * A FOUL COSTS NO POINTS. It ends the INNING and, in the first inning, nothing
 * else — the striker takes the second one from in hand behind the head string
 * and the frame carries on. The BCA rulebook, bowlliards.com and Virtual Pool 4
 * all read this way. bowlliards.net and a number of leagues take a point off
 * per foul instead, and that is a real disagreement rather than a misreading;
 * we follow the majority because a point penalty makes 300 UNATTAINABLE. A
 * perfect game needs twelve consecutive clearances and a card whose maximum
 * cannot be reached is not a bowling card. A house rule that costs a point is
 * cheap to add on top of this; a maximum that has been quietly moved is not
 * something a player can see.
 *
 * CALL SHOT: ball and pocket, and the pot has to be made as called for the ball
 * to count. Kisses, caroms, combinations and banks need not be called — you say
 * where the ball is going, not how it gets there. Anything else that drops on
 * the same stroke is spotted and scores nothing, and crucially DOES NOT END THE
 * INNING: it is not a foul, so there is nothing to hand the table over for. That
 * is the one place this differs from straight pool next door, where the
 * accidental extra counts a point.
 *
 * HOW IT ENDS. The two players take a frame each in turn, as bowlers on one
 * lane do, so a fresh rack goes up between every frame and each of them carries
 * its own free break. Ten frames each, and the higher card wins — level, and
 * they play further frames alternately until one is better, which is the only
 * part of this with no rule book behind it and is argued where it is written.
 */

/* THE CARD, one nibble to a delivery — see the block comment on bw_pins in
 * cue_rules.h for why it is packed rather than a byte each. The slot number is
 * the delivery number a bowling sheet prints: frame f owns 2f and 2f+1, and the
 * tenth owns 18, 19 and 20. Fifteen is "not delivered", which is a different
 * thing from a nought that was shot for and missed. */
#define BW_SLOT(f_, d_)  ((f_) * 2 + (d_))

static int bw_get(const CueRules *r, int who, int slot) {
    if (!r || who < 0 || who > 1 || slot < 0 || slot > 20) return -1;
    const int v = (slot & 1) ? (r->bw_pins[who][slot >> 1] & 0x0F)
                             : (r->bw_pins[who][slot >> 1] >> 4);
    return (v == 0x0F) ? -1 : v;
}

static void bw_put(CueRules *r, int who, int slot, int pins) {
    if (!r || who < 0 || who > 1 || slot < 0 || slot > 20) return;
    if (pins < 0) pins = 0x0F; else if (pins > 10) pins = 10;
    unsigned char *p = &r->bw_pins[who][slot >> 1];
    if (slot & 1) *p = (unsigned char)((*p & 0xF0) | pins);
    else          *p = (unsigned char)((*p & 0x0F) | (pins << 4));
}

/* THE RUNNING TOTAL, scored the way a card is scored: over the DELIVERIES in
 * the order they were made rather than frame by frame, because a strike's frame
 * holds one delivery and reaches two frames forward for its bonus, and a frame
 * loop cannot say "the next two" without unpicking that again at every step.
 *
 * A frame whose bonus has not been delivered yet is not scored at all and the
 * walk stops there, which is why a bowler's box stays blank after a strike
 * until two more balls have been thrown. It is not an approximation waiting to
 * be corrected; there is genuinely no number to put in it. */
static int bw_score(const CueRules *r, int who, int through) {
    int roll[21], nr = 0;
    for (int s = 0; s <= 20; s++) {
        const int p = bw_get(r, who, s);
        /* A strike leaves its frame's second slot empty and the frame after it
         * fills the next one, so skipping the gaps keeps the deliveries in the
         * order they were played. */
        if (p >= 0) roll[nr++] = p;
    }
    int score = 0, i = 0;
    for (int f = 0; f < 10 && f <= through; f++) {
        if (i >= nr) break;
        if (roll[i] == 10) {                       /* a strike: ten and the next two */
            if (i + 2 >= nr) break;
            score += 10 + roll[i + 1] + roll[i + 2];
            i += 1;
        } else {
            if (i + 1 >= nr) break;                /* the frame is half played */
            const int pinfall = roll[i] + roll[i + 1];
            if (pinfall == 10) {                   /* a spare: ten and the next one */
                if (i + 2 >= nr) break;
                score += 10 + roll[i + 2];
            } else score += pinfall;
            i += 2;
        }
    }
    return score;
}

/* HOW MANY OF THE TEN IN FRONT OF THE STRIKER ARE ALREADY DOWN.
 *
 * Not the frame's pinfall, which is a different number in the tenth: a delivery
 * that clears the rack is followed by a fresh one, so the count goes back to
 * nought and the frame's pinfall carries on past ten. Walking the frame's
 * deliveries and resetting on each clearance is the same rule the tenth frame
 * is written in — "re-rack only after a delivery that clears" — read off the
 * card rather than kept as a second copy of it in a field of its own.
 *
 * Counted from the card rather than from the table on purpose. Balls potted
 * without being called are SPOTTED and come back, and the spotting is the
 * host's and happens after this runs, so the ball array here says the rack is
 * emptier than it is about to be. */
static int bw_rack_down(const CueRules *r, int who) {
    const int f = r->bw_frame[who];
    if (f >= 10) return (r->bw_sd[who] == 0xFF) ? 0 : r->bw_sd[who];
    int down = 0;
    for (int d = 0; d < 3; d++) {
        const int p = bw_get(r, who, BW_SLOT(f, d));
        if (p < 0) break;
        down += p;
        if (down >= 10) down = 0;          /* that one cleared it: a fresh rack */
    }
    return down;
}

/* Set the table out again and hand the striker the free break that comes with
 * it. Every fresh rack in this game is broken, and the break is never an
 * inning — so the two always travel together and there is nowhere a rack goes
 * up without one. */
static void bw_fresh_rack(CueRules *r) {
    r->rerack = 2;
    r->break_shot = 1;
    r->ball_in_hand = 1;
    r->respot = 0;                 /* the rack replaces every ball on the cloth */
    for (int i = 0; i < 8; i++) r->respot_id[i] = 0;
}

static void resolve_bowlliards(CueRules *r, CueBall *b, int n, const CueWorld *w,
                               int first_hit, int scratch, int cushion,
                               const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    const int was_break = r->break_shot;
    const int called_id = r->nominated, called_pkt = r->called_pocket;
    (void)w;
    r->nominated = 0; r->called_pocket = -1;
    r->break_shot = 0;

    /* Everything down that did not score goes back on the table. Collected in
     * one place because the break spots the lot and a scoring stroke spots all
     * but one, and the two were the same loop written twice first time. */
    int back = 0;
    #define BW_SPOT(id_) do { if (back < 8) r->respot_id[back] = (unsigned char)(id_); \
                              back++; } while (0)

    if (was_break) {
        /* THE FREE BREAK. No cushion requirement, no penalty for the cue ball
         * going down or off, and nothing on it scores. There is no foul to
         * find, so none is looked for. */
        for (int k = 0; k < np; k++)
            if (potted[k] != CUE_ID_CUE) BW_SPOT(potted[k]);
        r->respot = back;
        r->last_foul = 0;
        r->cfoul[me] = 0;
        r->ball_in_hand = 1;                    /* in hand behind the head string */
        snprintf(r->msg, sizeof r->msg, "BREAK");
        return;
    }

    int foul = 0; const char *why = "";
    if (first_hit < 0)        { foul = 1; why = "NO BALL HIT"; }
    else if (scratch)         { foul = 1; why = "SCRATCH"; }
    else if (r->n_off)        { foul = 1; why = "OFF THE TABLE"; }
    else if (!np && !cushion) { foul = 1; why = "NO CUSHION"; }

    /* WAS THE CALL MADE? CueBall.pocket carries the pocket the ball fell in,
     * which is what makes calling a pocket enforceable rather than an honour
     * system. A pocket of -1 is a stroke nobody called a pocket for — the CPU's
     * safeties arrive that way — and the ball then counts wherever it went. */
    int made = 0;
    for (int k = 0; k < np && called_id && !foul; k++) {
        if (potted[k] != called_id) continue;
        const CueBall *q = find_ball(b, n, potted[k]);
        if (!q || q->pocket == CUE_OFF_TABLE) continue;   /* driven off, not potted */
        if (called_pkt < 0 || (int)q->pocket == called_pkt) made = 1;
    }

    for (int k = 0; k < np; k++) {
        if (potted[k] == CUE_ID_CUE) continue;    /* the white is replaced, not spotted */
        if (made && potted[k] == called_id) continue;
        BW_SPOT(potted[k]);
    }
    r->respot = back;
    #undef BW_SPOT

    const int scored = made ? 1 : 0;
    const int f = r->bw_frame[me];
    const int sudden = (f >= 10);            /* the tie-break: no card to write to */
    const int before = bw_rack_down(r, me);
    const int cleared = (before + scored) >= 10;

    if (sudden) {
        r->bw_sd[me] = (unsigned char)((r->bw_sd[me] == 0xFF ? 0 : r->bw_sd[me])
                                       + scored);
    } else {
        const int slot = BW_SLOT(f, r->bw_inning - 1);
        const int sofar = bw_get(r, me, slot);
        bw_put(r, me, slot, (sofar < 0 ? 0 : sofar) + scored);
    }
    r->score[0] = bw_score(r, 0, 9);
    r->score[1] = bw_score(r, 1, 9);

    /* A FOUL COSTS NOTHING BUT THE INNING, so the consecutive-foul counter every
     * other pool game here keeps has nothing to count towards and is left at
     * nought rather than being allowed to accumulate towards a penalty that
     * does not exist in this game. */
    r->last_foul = foul;
    r->cfoul[me] = 0;

    /* THE STRIKER PLAYS ON as long as he keeps making the call and there is
     * still something in front of him. An uncalled ball down with it is spotted
     * and costs him nothing — it is not a foul, so there is nothing to hand the
     * table over for. */
    if (made && !cleared) {
        r->brk += scored;
        r->ball_in_hand = 0;
        /* Both clamped to what they can actually be — nought to ten off a rack
         * of ten — because the compiler cannot see that from an int and warns
         * that eleven digits will not fit in a message of twenty-four. */
        const int down = (before + scored) > 10 ? 10
                       : (before + scored) < 0  ? 0 : (before + scored);
        const int spot = back > 10 ? 10 : back < 0 ? 0 : back;
        if (spot) snprintf(r->msg, sizeof r->msg, "%d DOWN - %d SPOTTED", down, spot);
        else      snprintf(r->msg, sizeof r->msg, "%d DOWN", down);
        return;
    }

    /* ---- the inning is over. How many does this frame get? ---------------
     *
     * THE TENTH FRAME IS THE TRAP, and getting it wrong silently costs the
     * maximum rather than throwing anything. Within it, follow bowling to the
     * letter: a strike earns two further deliveries and a spare one, and the
     * rack is set out again ONLY after a delivery that cleared it. A bonus
     * delivery that does not clear leaves the balls where they are and the next
     * delivery shoots what is left — so a tenth of ten, four, six is a legal
     * twenty and not a mistake. Re-racking after every bonus delivery instead
     * would give a striker ten fresh balls to clear in one visit twice over,
     * and re-racking never would make the second bonus delivery unplayable
     * after a first that cleared. Both read as a working game and both put a
     * different number at the bottom of the card. */
    int allowed = 2;
    if (!sudden && f == 9) {
        const int d0 = bw_get(r, me, BW_SLOT(9, 0));
        const int d1 = bw_get(r, me, BW_SLOT(9, 1));
        if (d0 == 10) allowed = 3;                          /* strike: two more */
        else if (d0 >= 0 && d1 >= 0 && d0 + d1 == 10) allowed = 3;  /* spare: one */
    }
    const int frame_done = (!sudden && f == 9) ? (r->bw_inning >= allowed)
                                               : (cleared || r->bw_inning >= 2);

    if (!frame_done) {
        r->bw_inning++;
        if (cleared) bw_fresh_rack(r);      /* the tenth only: a new rack, a new break */
        else {
            /* THE SECOND INNING IS PLAYED FROM WHERE THE BALLS LIE. No re-rack,
             * and no ball in hand either unless the inning ended on a foul —
             * missing a pot is not an offence and does not buy the striker a
             * better cue ball than the one he left himself. */
            r->ball_in_hand = foul ? 1 : 0;
        }
        r->brk += scored;
        if (foul) snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        else if (cleared) {
            /* Only the tenth ever gets here. The delivery just made is
             * bw_inning - 2, and clearing a rack the striker did not himself
             * open is a strike again rather than a spare — which is how twelve
             * strikes on one card is a thing that can happen. */
            const int d = r->bw_inning - 2;
            const int strike = (d == 0) || (bw_get(r, me, BW_SLOT(9, d - 1)) == 10);
            snprintf(r->msg, sizeof r->msg, "%s", strike ? "STRIKE" : "SPARE");
        }
        else snprintf(r->msg, sizeof r->msg, "MISS");
        return;
    }

    /* ---- the frame is closed -------------------------------------------- */
    if (sudden) {
        if (cleared) snprintf(r->msg, sizeof r->msg, "CLEARED");
        else         snprintf(r->msg, sizeof r->msg, "%d PINS", (int)r->bw_sd[me]);
    } else if (f == 9) {
        /* The tenth is worth up to thirty and its mark is the whole story of
         * three deliveries, so the board gets the number instead. */
        int pins = 0;
        for (int d = 0; d < 3; d++) {
            const int p = bw_get(r, me, BW_SLOT(9, d));
            if (p >= 0) pins += p;
        }
        snprintf(r->msg, sizeof r->msg, "TENTH: %d", pins);
    } else if (cleared) {
        snprintf(r->msg, sizeof r->msg, (r->bw_inning == 1) ? "STRIKE" : "SPARE");
    } else {
        snprintf(r->msg, sizeof r->msg, "OPEN: %d", bw_rack_down(r, me));
    }
    r->brk = 0;
    r->bw_inning = 1;
    if (!sudden) r->bw_frame[me] = (unsigned char)(f + 1);
    r->score[0] = bw_score(r, 0, 9);
    r->score[1] = bw_score(r, 1, 9);

    /* SUDDEN DEATH: tied on ten frames, so both play further frames alternately
     * and the first superior one takes it. Superior means PINFALL and nothing
     * else: a strike and a spare are both ten pins and neither is better than
     * the other here, because the bonus that separates them on a card is a
     * claim on innings that this frame does not have and never will. Level
     * again, and they go round once more. */
    if (sudden) {
        if (r->bw_sd[you] == 0xFF) {          /* their answer to it */
            r->turn = you;
            bw_fresh_rack(r);
            return;
        }
        const int a = r->bw_sd[0], c = r->bw_sd[1];
        if (a != c) {
            r->frame_over = 1;
            r->winner = (a > c) ? 0 : 1;
            book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg, "%d - %d", r->score[0], r->score[1]);
            return;
        }
        /* AND IT CANNOT GO ON FOR EVER. Two players who cannot pot a ball
         * between them tie every sudden-death frame at nought, and the rule as
         * written then never lets go of the table — not a hypothetical, since
         * that is exactly what a pair of idle seats does. So after twenty of
         * them it is called a draw and the game ends without a winner, which
         * speed pool already does when two clearances take the same time and
         * which the host already knows how to show.
         *
         * This is OURS and not the BCA's: the book says play until one frame is
         * better, full stop. A cap is a smaller departure from it than a table
         * nobody can leave. */
        r->bw_frame[0]++; r->bw_frame[1]++;
        if (r->bw_frame[me] >= 30) {
            r->frame_over = 1;
            r->winner = -1;
            snprintf(r->msg, sizeof r->msg, "DRAWN");
            return;
        }
        r->bw_sd[0] = r->bw_sd[1] = 0xFF;
        r->turn = you;
        bw_fresh_rack(r);
        snprintf(r->msg, sizeof r->msg, "LEVEL - AGAIN");
        return;
    }

    /* THE TWO ALTERNATE FRAMES, so the striker hands over whenever the other
     * player still has this frame to play — which, one behind or level, is
     * whenever their card is not full. */
    if (r->bw_frame[you] < 10) {
        r->turn = you;
        bw_fresh_rack(r);
        return;
    }
    if (r->bw_frame[me] < 10) {               /* they have finished; play yours out */
        bw_fresh_rack(r);
        return;
    }

    /* Both cards are full. */
    if (r->score[0] != r->score[1]) {
        r->frame_over = 1;
        r->winner = (r->score[0] > r->score[1]) ? 0 : 1;
        book_frame(r, r->winner);
        snprintf(r->msg, sizeof r->msg, "%d - %d", r->score[0], r->score[1]);
        return;
    }
    r->bw_sd[0] = r->bw_sd[1] = 0xFF;
    r->turn = you;
    bw_fresh_rack(r);
    snprintf(r->msg, sizeof r->msg, "TIED - SUDDEN DEATH");
}

/* What the card says, for a board that wants to draw one and for a test that
 * wants to read one back. `inning` is nought-based, so the tenth frame's bonus
 * deliveries are 1 and 2; -1 for an inning that has not been played. */
int cue_rules_bw_pins(const CueRules *r, int who, int frame, int inning) {
    if (!r || frame < 0 || frame > 9 || inning < 0 || inning > 2) return -1;
    if (inning == 2 && frame != 9) return -1;
    return bw_get(r, who, BW_SLOT(frame, inning));
}

/* AND A CARD SET DIRECTLY, for a harness that wants to LOOK at one.
 *
 * A card is the one thing in this game that cannot be reached by playing: a
 * full ten frames is a hundred-odd strokes of simulation, and the layout that
 * draws it has to be checked against strikes, spares, an open tenth and a
 * frame still waiting on its bonus — positions that take a particular run of
 * luck to reach. So the deliveries can be written straight in.
 *
 * Deliberately NOT the way the game scores: this writes a delivery and asks no
 * questions, where resolve_bowlliards decides whose inning it is, whether the
 * frame is closed and what the pinfall was. Nothing in the game calls this and
 * nothing should — it is here for the card's own sake, the way bw_pins is here
 * for the board's. */
void cue_rules_bw_set(CueRules *r, int who, int frame, int inning, int pins) {
    if (!r || who < 0 || who > 1 || frame < 0 || frame > 9) return;
    if (inning < 0 || inning > 2 || (inning == 2 && frame != 9)) return;
    bw_put(r, who, BW_SLOT(frame, inning), pins);
    r->score[0] = bw_score(r, 0, 9);
    r->score[1] = bw_score(r, 1, 9);
}

/* The running total through `frame` (nought-based; pass 9 for the whole card).
 * A frame still waiting on a strike's or a spare's bonus is not counted, which
 * is why this can go backwards relative to the pins already down. */
int cue_rules_bw_score(const CueRules *r, int who, int frame) {
    if (!r || who < 0 || who > 1) return 0;
    return bw_score(r, who, frame < 0 ? 0 : (frame > 9 ? 9 : frame));
}

/* ---- CRIBBAGE POOL --------------------------------------------------------
 *
 * Fifteen balls whose numbers are worth nothing on their own. What scores is a
 * CRIBBAGE — two balls totalling fifteen, potted in succession inside one
 * inning — and there are exactly seven of them in a rack: 1+14, 2+13, 3+12,
 * 4+11, 5+10, 6+9 and 7+8. Five cribbages take the game and the rack is not
 * played out once somebody has them. BCA rulebook 1992, pages 75 and 76;
 * Shamos and the Wikipedia article agree with it on everything below that is
 * not marked otherwise. The WPA has never sanctioned the game, which is why
 * nothing here reads like the American games it sits beside.
 *
 * IN SUCCESSION IS THE WHOLE GAME. Pot the 4 and you are ON A CRIBBAGE: the
 * next stroke must pot the 11, and there is no option to leave it and come back
 * later. Fail and it is a foul — the 4 is spotted, the inning ends, and the pair
 * has to be made again from nothing. Cribbages already completed are not
 * touched; only the unpaired ball goes back. So a run here is short by
 * construction, and the decision worth making is which ball to OPEN a pair
 * with rather than which ball to pot.
 *
 * Both balls of a pair on one stroke is a cribbage as well — succession does
 * not mean two strokes, it means nothing else got in between. Several balls on
 * one stroke leave several pairs open at once, and the striker may take the
 * companions in whichever order he likes so long as he keeps taking them;
 * anything he pots on the way joins the list. See cr_owed in cue_rules.h.
 *
 * THE 15 IS THE BALL THE GAME TURNS ON, and it is worth being exact about why.
 * It pairs with nothing — 15 plus anything is more than fifteen — so it can
 * never start a cribbage, and potted while any other ball is still up it is
 * simply spotted again with no penalty at all. Once the other fourteen are
 * gone it is a cribbage on its own, which makes EIGHT in a rack rather than
 * seven.
 *
 * That eighth is not a curiosity, it is the only thing that keeps the game
 * finishable. Seven pairs split between two players cannot reach five and five;
 * the best they can do is four and three. The 15 then takes the trailing player
 * to four, and at four and four with an empty table neither player can ever
 * score again — a rack that is not lost, not drawn and not playable, which is
 * the failure this game invites and which looks from outside exactly like the
 * frame having stopped for no reason. So an empty table with no winner spots
 * the 15 and plays it again as the deciding cribbage, as often as it takes.
 * It cannot go round for ever: with nothing else on the cloth the striker is on
 * a cribbage by definition and must pot it, so a player who cannot fouls, and
 * three fouls in a row loses the game.
 *
 * THE BREAK is an open break — a ball potted, or four object balls driven to a
 * rail — and it is otherwise an ordinary stroke inside the breaker's own
 * inning: what it pots counts and opens cribbages, and a scratch on it is a
 * foul like any other. This is the opposite of bowlliards next door, where the
 * break is free and scores nothing, and the two games are otherwise near
 * neighbours; the difference is easy to carry across by accident.
 *
 * WHERE THE BOOK GIVES A CHOICE AND THIS CODE CANNOT ASK. Two places, both
 * marked again where they happen. A failed open break lets the incoming player
 * either re-rack and break himself or make the offender break again; a foul
 * lets him either play the table as it lies or take the cue ball in hand in the
 * kitchen. There is no mechanism here for putting a question to a player
 * between strokes outside snooker's foul decisions, so one branch of each is
 * taken and said so — the same thing resolve_pool does with heyball's choice of
 * three.
 */

/* THE MATE THAT MAKES FIFTEEN, or nought for the ball that has none. Only the
 * 15 has none, and that is the whole of what is special about it. */
static int cr_mate(int id) { return (id >= 1 && id <= 14) ? 15 - id : 0; }

/* The most balls one stroke can ever ask to have spotted is the fifteen, since
 * a ball already off the table cannot also have been potted by this stroke. */
#define CR_MAX_SPOT 16

/* Name a ball to be spotted, keeping the list in ascending numerical order.
 * That order is the rule book's and not a tidiness: spotted balls go on the
 * foot spot and then one behind another up the long string, so the order they
 * are named in decides which of them ends up where. */
static void cr_spot(unsigned char *list, int *n, int id) {
    if (id < 1 || id > 15) return;
    if (*n >= CR_MAX_SPOT) { (*n)++; return; }
    int i = *n;
    while (i > 0 && list[i - 1] > (unsigned char)id) { list[i] = list[i - 1]; i--; }
    list[i] = (unsigned char)id;
    (*n)++;
}

/* Was `id` on the cloth when the striker addressed the ball? Still on it, or
 * taken off by this very stroke — which is the same question and has to be
 * asked of the two together, because the ball array has already been emptied by
 * the time a resolver sees it. */
static int cr_was_up(const CueBall *b, int n, const int *potted, int np, int id) {
    for (int i = 1; i < n; i++)
        if (b[i].id == id && b[i].on) return 1;
    for (int k = 0; k < np; k++) if (potted[k] == id) return 1;
    return 0;
}

static void resolve_cribbage(CueRules *r, CueBall *b, int n, const CueWorld *w,
                             int first_hit, int scratch, int cushion,
                             const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    const int was_break = r->break_shot;
    const int called_id = r->nominated, called_pkt = r->called_pocket;
    r->break_shot = 0;
    r->nominated = 0; r->called_pocket = -1;

    unsigned char spot[CR_MAX_SPOT]; int nspot = 0;

    /* WHAT HE CAME TO THE TABLE OWING, kept before the stroke is allowed to
     * change it. The obligation is judged against the list he was ON, and the
     * live list grows as this stroke's own pots are worked through it — so
     * reading the obligation off the live list afterwards would let a stroke
     * discharge a debt it had itself just created. */
    unsigned char owed0[8];
    const int nowed0 = (r->cr_nowed > 8) ? 8 : r->cr_nowed;
    for (int i = 0; i < nowed0; i++) owed0[i] = r->cr_owed[i];

    /* THE LONE 15 IS AN OBLIGATION TOO. With nothing else on the cloth there is
     * nothing else to shoot at and the 15 is a cribbage in its own right, so
     * the striker is on a cribbage whether he opened one or not. Without this
     * the deciding stroke of a level game would be the one stroke in the game a
     * player could miss for nothing, and two players missing it in turn is the
     * deadlock coming back in through another door. */
    int nup = 0;
    for (int i = 1; i < n; i++) {
        if (b[i].id < 1 || b[i].id > 15) continue;
        if (cr_was_up(b, n, potted, np, b[i].id)) nup++;
    }
    const int lone15 = (nup == 1 && cr_was_up(b, n, potted, np, 15));

    /* How many OBJECT balls went down, which is not np: the white is in that
     * list too and it is replaced rather than spotted. */
    int npo = 0;
    for (int k = 0; k < np; k++) if (potted[k] != CUE_ID_CUE) npo++;

    int foul = 0; const char *why = "";
    if (first_hit < 0)         { foul = 1; why = "NO BALL HIT"; }
    else if (scratch)          { foul = 1; why = "SCRATCH"; }
    else if (r->n_off)         { foul = 1; why = "OFF THE TABLE"; }
    else if (!npo && !cushion) { foul = 1; why = "NO CUSHION"; }

    /* THE OPEN BREAK: a ball potted, or four object balls driven to a rail.
     * Asked only of a break that was otherwise clean, exactly as the eight-ball
     * codes ask it — a scratch on the break is already a foul and is priced as
     * one, and a stroke cannot be both re-racked and paid for.
     *
     * NOT A FOUL, and the difference is the whole of the rule: nothing is owed,
     * nobody's foul count moves, the balls simply go back. The book gives the
     * incoming player a choice of re-racking and breaking himself or making the
     * offender break again; the first is taken here because it is the one that
     * cannot leave a player who has already shown he cannot open the rack
     * trying to again, and there is nothing to ask him with. */
    if (was_break && !foul && npo == 0 && brk_rails(w, n) < 4) {
        r->rerack = 2; r->racks++;
        r->last_foul = 0;
        r->break_shot = 1;
        r->turn = you;
        r->ball_in_hand = 1;
        r->cr_nowed = 0;
        r->respot = 0;
        r->brk = 0;
        snprintf(r->msg, sizeof r->msg, "ILLEGAL BREAK - RE-RACK");
        return;
    }

    if (foul) {
        /* EVERY BALL THIS STROKE PUT DOWN WAS ILLEGALLY POTTED, so none of them
         * counts and all of them go back on. That is the one place the general
         * foul and the cribbage foul part company: a general foul makes the
         * whole stroke illegal, while failing to pot a companion is a failure
         * to do something in a stroke that was otherwise legal, and what such a
         * stroke legally potted stands. */
        for (int k = 0; k < np; k++)
            if (potted[k] != CUE_ID_CUE) cr_spot(spot, &nspot, potted[k]);
        for (int i = 0; i < (int)r->cr_nowed && i < 8; i++)
            cr_spot(spot, &nspot, cr_mate(r->cr_owed[i]));
        r->cr_nowed = 0;
        goto penalty;
    }

    /* WAS THE CALL MADE? Ball and pocket, read the way bowlliards reads it: the
     * ball has to be in the pocket it was named for, and a pocket of -1 is a
     * stroke nobody named one for — the CPU's safeties arrive that way — so the
     * ball then counts wherever it went.
     *
     * THE BREAK IS NOT CALLED, in this game as in every other that calls, and
     * what drops on it counts and opens cribbages like any other stroke. Left
     * out, a break that pots the 4 would spot it again and the open break would
     * have satisfied itself by doing something the rules then undid. */
    const int callshot = !was_break;
    int made = 0;
    for (int k = 0; k < np && called_id && !made; k++) {
        if (potted[k] != called_id) continue;
        const CueBall *q = find_ball(b, n, potted[k]);
        if (!q || q->pocket == CUE_OFF_TABLE) continue;   /* driven off, not potted */
        if (called_pkt < 0 || (int)q->pocket == called_pkt) made = 1;
    }
    /* A CALL ON THE 15 TOO EARLY IS NOT A CALL. The ball cannot count however
     * cleanly it is potted, so a stroke whose only named ball was that one has
     * named nothing, and the balls that came down with it are as uncalled as
     * the ball itself. */
    if (called_id == 15 && !lone15) made = 0;
    const int counts = !callshot || made;

    /* ---- what actually counted, lowest first ---------------------------- */
    int cnt[16], ncnt = 0;
    for (int k = 0; k < np; k++) {
        const int id = potted[k];
        if (id == CUE_ID_CUE) continue;
        if (id == 15 && !lone15) { cr_spot(spot, &nspot, 15); continue; }
        if (!counts)             { cr_spot(spot, &nspot, id); continue; }
        int i = ncnt;
        while (i > 0 && cnt[i - 1] > id) { cnt[i] = cnt[i - 1]; i--; }
        cnt[i] = id; ncnt++;
    }

    /* ---- and what it did to the debt ------------------------------------ */
    int scored = 0;
    for (int i = 0; i < ncnt; i++) {
        const int id = cnt[i];
        if (id == 15) { scored++; continue; }        /* a cribbage on its own */
        int at = -1;
        for (int j = 0; j < (int)r->cr_nowed && j < 8; j++)
            if (r->cr_owed[j] == id) { at = j; break; }
        if (at >= 0) {
            for (int j = at; j + 1 < (int)r->cr_nowed; j++)
                r->cr_owed[j] = r->cr_owed[j + 1];
            r->cr_nowed--;
            scored++;
        } else if (r->cr_nowed < 8) {
            r->cr_owed[r->cr_nowed++] = (unsigned char)cr_mate(id);
        }
    }
    r->score[me] += scored;

    /* FIVE TAKES IT, THE MOMENT IT IS REACHED. The rack is not played out and
     * the balls still on the cloth are not looked at — which is what makes the
     * fifth cribbage worth setting up for rather than arriving at. */
    if (r->score[me] >= r->target_score) {
        r->respot = 0;
        r->cr_nowed = 0;
        r->last_foul = 0;
        r->cfoul[me] = 0;
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }

    /* DID HE POT WHAT HE WAS ON? Any one of the companions he owed will do —
     * the striker chooses the order when several are open — but one of them has
     * to arrive, and a cribbage completed out of a pair he opened on this very
     * stroke does not answer for a pair he opened on the last one. */
    if (nowed0 > 0 || lone15) {
        int got = 0;
        for (int i = 0; i < ncnt && !got; i++) {
            /* Owing nothing and still under an obligation means the lone 15,
             * and then the 15 is the only answer there is. */
            if (nowed0 == 0) { if (cnt[i] == 15) got = 1; continue; }
            for (int j = 0; j < nowed0; j++)
                if (cnt[i] == owed0[j]) { got = 1; break; }
        }
        if (!got) {
            /* THE UNPAIRED BALLS GO BACK, all of them: the inning ends here, so
             * every pair still open is broken, including any this stroke opened
             * itself. The ball to spot is the one already down, which is the
             * MATE of what is owed — kept as one list rather than two, because
             * two lists of the same seven pairs is two chances to disagree. */
            for (int i = 0; i < (int)r->cr_nowed && i < 8; i++)
                cr_spot(spot, &nspot, cr_mate(r->cr_owed[i]));
            r->cr_nowed = 0;
            foul = 1; why = "NO CRIBBAGE";
            goto penalty;
        }
    }

    /* ---- THE TIE-BREAK: an empty table that nobody has won ---------------
     *
     * Four and four, every pair made and the 15 with them, and not a ball left
     * to play at. Spot the 15 and it is the deciding cribbage — see the block
     * comment above for why this cannot be left to sort itself out. The striker
     * keeps the table, because he can only have got here by scoring. */
    {   int left = 0;
        for (int i = 1; i < n; i++)
            if (b[i].on && b[i].id >= 1 && b[i].id <= 15) left++;
        if (left == 0 && nspot == 0) cr_spot(spot, &nspot, 15);
    }

    r->last_foul = 0;
    r->cfoul[me] = 0;
    r->ball_in_hand = 0;
    r->respot = nspot;
    for (int i = 0; i < 8; i++) r->respot_id[i] = (i < nspot) ? spot[i] : 0;

    if (ncnt > 0) {
        /* THE STRIKER PLAYS ON. He has either completed a cribbage or opened
         * one, and either way there is something in front of him he must now
         * do. An early 15 spotted alongside costs him nothing — it is not a
         * foul, so there is nothing to hand the table over for. */
        r->brk += scored;
        if (r->cr_nowed == 1)
            snprintf(r->msg, sizeof r->msg, "ON THE %d", r->cr_owed[0]);
        else if (r->cr_nowed > 1)
            snprintf(r->msg, sizeof r->msg, "%d TO PAIR", (int)r->cr_nowed);
        else
            snprintf(r->msg, sizeof r->msg, "CRIBBAGE - %d", r->score[me]);
        return;
    }

    /* NOTHING DOWN AND NOTHING OWED: an ordinary miss, and the table passes
     * without a penalty. The 15 taken early on its own arrives here, which is
     * a decision rather than a reading — the books say it is spotted and there
     * is no penalty, and are silent about the table. Keeping it would let a
     * striker pot the same spotted ball for ever and never have to leave, which
     * is not a rule anybody wrote down but is what "he keeps the table" comes
     * to when the ball always comes back. */
    r->brk = 0;
    r->turn = you;
    r->msg[0] = 0;
    if (npo && !counts)     snprintf(r->msg, sizeof r->msg, "NOT AS CALLED");
    else if (npo)           snprintf(r->msg, sizeof r->msg, "THE 15 - SPOTTED");
    return;

penalty:
    /* A FOUL COSTS THE INNING AND NOTHING ELSE — no points come off, which is
     * why this game has a three-foul rule at all: without one there would be no
     * price for a player who simply could not reach the ball he owed.
     *
     * THE CUE BALL GOES TO THE KITCHEN. After a scratch or a ball off the table
     * the book gives the incoming player no choice about it; after any other
     * foul he may take that or play the table as it lies, and there is nothing
     * here to ask him with. In hand is taken for both, which is what a player
     * takes in most positions — but it is genuinely the weaker of the two when
     * the balls are all at the foot end, so this is a choice made rather than a
     * rule followed. */
    r->respot = nspot;
    for (int i = 0; i < 8; i++) r->respot_id[i] = (i < nspot) ? spot[i] : 0;
    r->last_foul = 1;
    r->brk = 0;
    r->cfoul[me]++;
    if (r->cfoul[me] >= 3) {
        r->frame_over = 1; r->winner = you; book_frame(r, you);
        snprintf(r->msg, sizeof r->msg, "3 FOULS - LOSS");
        return;
    }
    r->turn = you;
    r->ball_in_hand = 1;
    snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
}

/* ---- COWBOY POOL ---------------------------------------------------------
 *
 * Three balls — the 1, the 3 and the 5 — and a hundred and one points to be had
 * off them three different ways at once. It is the strangest game on this list
 * and the only one that changes its own rules as it goes.
 *
 *   TO 90       everything scores. A ball pocketed is worth its NUMBER, a
 *               cannon off two balls is ONE, and an in-off is ONE.
 *   91 TO 100   CANNONS ONLY. Pocketing scores nothing at all from here, so a
 *               player who has spent the game potting has to find a different
 *               game to finish it in.
 *   THE 101st   a cannon off the 1 BALL FIRST, and nothing else will do.
 *
 * YOU CANNOT OVERSHOOT. A stroke worth more than the points left scores nothing
 * and the visit is over — so the run into the nineties has to be counted, and a
 * player on 88 cannot simply pot the 5. That is the whole tactical shape of the
 * endgame and the reason the game is played at all.
 *
 * The balls always come back: with three of them on a nine-foot table, a game
 * that removed them would be over in a minute. Pocketed, they go to their spots.
 */
static void resolve_cowboy(CueRules *r, CueBall *b, int n, const CueWorld *w,
                           int first_hit, int scratch, int cushion,
                           const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    /* READ BEFORE IT IS CLEARED, which is the whole of why every other
     * resolver here keeps its own copy: the line below wipes it, so a test
     * written further down sees 0 on the break like every other shot. */
    const int was_break = r->break_shot;
    r->break_shot = 0;

    /* WHAT THE CUE BALL TOUCHED, in order, so a cannon can be told from a
     * single contact — the same log carom is scored from. */
    int distinct = 0, seen[8]; int nseen = 0;
    if (w) for (int i = 0; i < w->ntouch; i++) {
        if (w->touch[i].what != CUE_TOUCH_BALL) continue;
        int dup = 0;
        for (int k = 0; k < nseen; k++) if (seen[k] == w->touch[i].id) dup = 1;
        if (!dup && nseen < 8) seen[nseen++] = w->touch[i].id;
    }
    distinct = nseen;

    /* HOW MANY OF THE THREE THE CUE BALL FOUND, which is the whole of what a
     * cannon is worth here — and it is worth more off all three.
     *
     * Two balls is one point and THREE IS TWO, which the game scored as one.
     * It is the only stroke in cowboy that pays double and it is the hardest
     * one on the table, so losing it lost the shot players aim for: a pot of
     * the 5 with a cannon off both the others is seven, and it was paying six.
     *
     * Worked out before the foul test because the foul test now needs it. */
    const int cannon  = (distinct >= 2);
    const int cannon2 = (distinct >= 3);

    const int have = r->score[me];

    int foul = 0; const char *why = "";
    if (first_hit < 0)        { foul = 1; why = "NO BALL HIT"; }
    else if (r->n_off)        { foul = 1; why = "OFF THE TABLE"; }
    /* THE BREAK MUST CONTACT THE 3 FIRST. */
    else if (was_break && first_hit != 3) { foul = 1; why = "BREAK MUST HIT THE 3"; }
    /* A SCRATCH IS A FOUL, and it is not a point.
     *
     * The first ninety are scored by pocketing an object ball, by a cannon off
     * two, or by a cannon off three -- an in-off is not on that list, and the
     * rules put the incoming player in the kitchen after one, which is the
     * penalty for a foul. This awarded the striker a point for it and let him
     * carry on, so the commonest foul in the game was the cheapest thing on the
     * table. The one exception is the last point, which IS a deliberate in-off
     * and is handled below. */
    else if (scratch && have < 100) { foul = 1; why = "SCRATCH"; }
    /* POCKETING IN THE CANNON PHASE IS A FOUL. From ninety the points may only
     * be scored by cannons, and putting a ball down is not merely worth nothing
     * -- it ends the inning and takes the inning's points with it. */
    else if (have >= 90 && have < 100 && np) { foul = 1; why = "CANNONS ONLY"; }
    /* A CANNON IS ITSELF THE STROKE, so it needs no cushion after it.
     *
     * The general rule — contact, then a pot or a rail — is the right one for
     * a potting game, and cowboy stops being a potting game at ninety. Applied
     * literally it fouled the shot the endgame is entirely made of: a delicate
     * cannon off two balls that leaves everything where it lies touches no
     * cushion by design, and calling that a foul made the last eleven points
     * unplayable as they are meant to be played. */
    else if (!np && !cushion && !scratch && !cannon) { foul = 1; why = "NO CUSHION"; }

    /* THE CEILING OF THE PHASE YOU ARE IN, and it is not 101 until the end.
     * Pocketing and in-offs may carry you to NINETY and no further — that is
     * what makes the run-in a counting problem rather than a potting one, and
     * why a player on 88 cannot simply take the 5. From ninety the cannons may
     * carry you to a hundred, and the hundred-and-first is its own shot. */
    const int cap  = (have >= 100) ? 101 : (have >= 90) ? 100 : 90;
    const int left = cap - have;

    /* WHAT THE STROKE WAS WORTH, by the phase the striker is in. */
    int gain = 0;
    /* One for two balls, two for all three — everywhere a cannon counts. */
    const int cannon_pts = cannon2 ? 2 : cannon ? 1 : 0;
    if (!foul) {
        if (have >= 100) {
            /* THE LAST POINT IS A LOSING HAZARD, not a cannon.
             *
             * "The final point necessary to reach 101 and the win must be made
             * by a losing hazard -- an intentional scratch made by caroming the
             * cue ball off the one ball", and it is a foul if the cue ball
             * fails to contact the 1 or contacts any other object ball.
             *
             * This asked for a CANNON off the 1, which is very nearly the
             * opposite: it handed the game to a player who cannoned off the 1
             * into another ball -- the one thing the rule explicitly fouls --
             * and gave nothing at all for the in-off the game is won with. */
            if (scratch && first_hit == 1 && distinct == 1) gain = 1;
        } else if (have >= 90) {
            gain = cannon_pts;                /* cannons only from ninety */
        } else {
            for (int k = 0; k < np; k++)
                if (potted[k] >= 1 && potted[k] <= 5) gain += potted[k];
            gain += cannon_pts;
            /* NO POINT FOR AN IN-OFF. It is a foul above; the only scoring
             * scratch in the game is the 101st. */
        }
    }

    /* THE BALLS GO BACK, always — three balls that stayed down would end the
     * game rather than the frame.
     *
     * AND BY NAME. An unnamed respot means "any", which sends the host to
     * respot_one — lowest id off the table, onto the foot spot. With one ball
     * down that happens to be right; with two it puts them both at the foot
     * end, and neither of them necessarily on its own spot. Cowboy's three
     * balls are worth one, three and five and each has a spot of its own, so
     * each is asked for by name and cue_table_respot_ball knows where it
     * lives. */
    r->respot = np;
    for (int k = 0; k < np && k < 8; k++)
        r->respot_id[k] = (unsigned char)potted[k];
    if (scratch) r->ball_in_hand = 1;

    if (foul) {
        /* AND IT COSTS THE WHOLE INNING.
         *
         * "All foul shots result in the player losing all points scored during
         * the inning (not just those on the fouled stroke)." That is the rule
         * that makes cowboy the game it is -- a run of eighty is not banked
         * until you leave the table -- and it was not implemented at all: a
         * foul simply ended the turn and every point already scored stood.
         * No deduction below zero and no penalty beyond it: the score goes back
         * to what the striker came to the table with. */
        r->score[me] -= r->cow_inning;
        if (r->score[me] < 0) r->score[me] = 0;
        r->last_foul = 1;
        r->cfoul[me]++;
        r->turn = you;
        r->brk = 0;
        if (r->cow_inning)
            snprintf(r->msg, sizeof r->msg, "FOUL: %s - LOST %d",
                     why, r->cow_inning);
        else
            snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        r->cow_inning = 0;
        return;
    }
    r->last_foul = 0;
    r->cfoul[me] = 0;

    /* NO OVERSHOOTING, AND IT IS A FOUL. "The 90th point must be reached
     * exactly, and the failure to do so is a foul resulting in a loss of turn"
     * -- so it costs the inning like any other foul, which it did not. */
    if (gain > left) {
        r->score[me] -= r->cow_inning;
        if (r->score[me] < 0) r->score[me] = 0;
        r->last_foul = 1;
        r->cfoul[me]++;
        r->turn = you;
        r->brk = 0;
        snprintf(r->msg, sizeof r->msg, "TOO MANY - %d NEEDED", left);
        r->cow_inning = 0;
        return;
    }

    r->score[me] += gain;
    r->cow_inning += gain;          /* at risk until the striker leaves */
    if (r->score[me] >= 101) {
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }
    if (gain) {
        r->brk += gain;
        snprintf(r->msg, sizeof r->msg, "%d", gain);
        return;                                   /* the striker plays on */
    }
    /* A LEGAL STROKE THAT SCORED NOTHING ends the inning -- and the points
     * banked in it are safe, which is the whole difference between leaving the
     * table and fouling at it. */
    r->cow_inning = 0;
    r->brk = 0;
    r->turn = you;
    if (have >= 100)     snprintf(r->msg, sizeof r->msg, "IN OFF THE 1");
    else if (have >= 90) snprintf(r->msg, sizeof r->msg, "CANNONS ONLY");
    else                 r->msg[0] = 0;
}

/* ---- ONE POCKET ----------------------------------------------------------
 *
 * The most tactical game on a pool table and unlike anything else here: each
 * player owns ONE of the two foot corner pockets and scores only into it.
 * Fifteen balls, first to eight, and a ball is only ever worth something to
 * whoever owns the hole it went down.
 *
 *   YOUR POCKET      one point, and you stay at the table.
 *   THEIR POCKET     one point TO THEM, and your visit is over. There is no
 *                    way to refuse it: a ball down is down.
 *   ANY OTHER        spotted, and your visit is over. The four neutral pockets
 *                    are hazards rather than targets, which is the whole
 *                    reason the game is played at a crawl.
 *
 * A FOUL COSTS A BALL. Not the turn only — one of your scored balls comes back
 * out and onto the table, and if you have none yet you OWE one, taken from the
 * first ball you do score. That is what makes a foul expensive enough to play
 * safe for, and it is why the game is famous for two players nudging balls a
 * millimetre at a time.
 *
 * The fouls are the ordinary pool ones: hit nothing, drive nothing to a cushion
 * when nothing is potted, pot the cue ball, or put a ball off the table. A
 * scratch also hands the cue ball over in hand behind the head string.
 *
 * WHICH POCKET IS WHOSE is the host's to say (see op_hole), because the rules
 * hold no table and a pocket array has no fixed numbering. */
static void resolve_onepocket(CueRules *r, CueBall *b, int n, const CueWorld *w,
                              int first_hit,
                              int scratch, int cushion, const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    const int was_break = r->break_shot;
    r->break_shot = 0;

    /* NOBODY OWNS A POCKET YET, so nothing on this stroke can score.
     *
     * The pockets are the breaker's choice and it is made AFTER the break —
     * you look at what the break left and take the end that suits it. Until
     * then a ball down belongs to nobody: it goes back on the table, and the
     * breaker is asked. */
    if (r->op_hole[me] < 0 || r->op_hole[you] < 0) {
        for (int k = 0; k < np && k < 8; k++)
            r->respot_id[k] = (unsigned char)potted[k];
        r->respot = np;
        r->op_pick = me + 1;
        r->last_foul = 0;
        r->brk = 0;
        if (was_break) snprintf(r->msg, sizeof r->msg, "CHOOSE YOUR POCKET");
        else           r->msg[0] = 0;
        return;
    }

    /* WHERE EVERYTHING WENT. bb_hole runs in step with potted, and -1 is a ball
     * driven off the table rather than down a hole. */
    int mine = 0, theirs = 0, neutral = 0;
    for (int k = 0; k < np && k < 8; k++) {
        const int h = r->bb_hole[k];
        /* NAMED, not counted: the ball that goes back is the one that went
         * down a hole nobody owns, and the striker may have scored on the same
         * stroke. See respot_id. */
        if (h < 0 || (h != r->op_hole[me] && h != r->op_hole[you])) {
            if (neutral < 8) r->respot_id[neutral] = (unsigned char)potted[k];
            neutral++;
            continue;
        }
        if (h == r->op_hole[me]) mine++; else theirs++;
    }

    /* ---- the foul, before anything is counted ---- */
    int foul = 0; const char *why = "";
    /* ONE POCKET TAKES ITS OWN RULE, and a much weaker one, on purpose.
     *
     * The two codes contradict each other: WPA 12.3 says "there are no special
     * requirements for the break shot", and OnePocket.org 2.2 asks that "the
     * cue ball or at least one object ball must be driven to a rail, or a ball
     * pocketed". This is the second, because "no requirement" is not a rule you
     * can implement and OnePocket.org is what tournaments actually run.
     *
     * And emphatically NOT the four-ball rule the eight-balls use. A one pocket
     * break is meant to be soft — it moves almost nothing, which is the whole
     * character of the game — so a four-ball requirement would foul nearly
     * every legitimate break in it. */
    if (was_break && np == 0 && brk_rails(w, n) < 1 && !brk_cue_rail(w))
        { foul = 1; why = "BREAK"; }
    if (first_hit < 0)          { foul = 1; why = "NO BALL HIT"; }
    else if (scratch)           { foul = 1; why = "SCRATCH"; }
    else if (r->n_off)          { foul = 1; why = "OFF THE TABLE"; }
    else if (!np && !cushion)   { foul = 1; why = "NO CUSHION"; }

    /* A BALL DOWN IS DOWN, whoever fouled. The pot still counts for whoever
     * owns the hole — a foul does not un-pot a ball — and the penalty is
     * charged on top. */
    if (mine)   r->score[me]  += mine;
    if (theirs) r->score[you] += theirs;

    /* ...and the neutral ones come back. The host does the placing, as it does
     * for every other spot in this file. */
    r->respot = neutral;

    if (foul) {
        /* THE PENALTY IS A BALL, and it is owed when there is none to give. */
        if (r->score[me] > 0) { r->score[me]--; r->respot++; }
        else                    r->op_owed[me]++;
        r->last_foul = 1;
        r->cfoul[me]++;
        if (scratch || r->n_off) r->ball_in_hand = 1;
        r->turn = you;
        r->brk = 0;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        return;
    }
    r->last_foul = 0;
    r->cfoul[me] = 0;

    /* A DEBT IS PAID OUT OF THE NEXT BALL SCORED, which is what "owing" means:
     * the ball goes in, comes straight back out, and the debt goes down. */
    while (r->op_owed[me] > 0 && r->score[me] > 0) {
        r->score[me]--; r->op_owed[me]--; r->respot++;
    }

    if (r->score[me] >= 8) {
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }
    if (r->score[you] >= 8) {
        r->frame_over = 1; r->winner = you; book_frame(r, you);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }

    if (mine && !theirs && !neutral) {
        r->brk += mine;
        snprintf(r->msg, sizeof r->msg, mine > 1 ? "%d" : "%d", mine);
        return;                              /* the striker plays on */
    }
    r->brk = 0;
    r->turn = you;
    if (theirs)      snprintf(r->msg, sizeof r->msg, "THEIR POCKET");
    else if (neutral) snprintf(r->msg, sizeof r->msg, "SPOTTED");
    else              r->msg[0] = 0;
}

/* ---- BANK POOL -----------------------------------------------------------
 *
 * Fifteen balls on the 9 ft table, first to eight, and EVERY scoring ball must
 * come off a cushion first. A ball potted without a bank does not count: it
 * goes back on the table and the visit is over. That one rule is the whole
 * game, and it makes a different player of you — the shots a pool player has
 * spent years learning are precisely the ones that score nothing here.
 *
 * WHETHER A BALL BANKED is a question about the OBJECT ball, and nothing in the
 * world could answer it until now: the touch log follows the cue ball, because
 * carom is a game about where the cue ball has been. CueWorld::rails counts
 * every ball's own cushions, and this reads it.
 *
 * A foul costs a ball, as it does at One Pocket and for the same reason: the
 * game is played slowly and deliberately, and a penalty that costs only the
 * turn is no penalty at all in a game where you were going to play safe
 * anyway. Owed when there is nothing to give.
 *
 * NOT MODELLED: the strict prohibition on combinations, caroms and kicks. Those
 * score nothing in a tournament, and here a banked ball counts however it got
 * there. It is on the list rather than pretended at. */
static void resolve_bank(CueRules *r, CueBall *b, int n, const CueWorld *w,
                         int first_hit, int scratch, int cushion,
                         const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    const int was_break = r->break_shot;
    r->break_shot = 0;

    /* WHICH OF THE POTTED BALLS ACTUALLY BANKED. potted carries ids; the rail
     * count is by index, so the ball is found by id. */
    int scored = 0, unbanked = 0;
    for (int k = 0; k < np; k++) {
        int idx = -1;
        for (int i = 1; i < n; i++) if (b[i].id == potted[k]) { idx = i; break; }
        const int banked = (w && idx > 0 && idx < CUE_MAX_BALLS && w->rails[idx] > 0);
        if (banked) scored++;
        else {
            if (unbanked < 8) r->respot_id[unbanked] = (unsigned char)potted[k];
            unbanked++;
        }
    }

    int foul = 0; const char *why = "";
    /* WPA 13.3: the break must pocket a ball or drive FOUR object balls to a
     * rail. The book gives the incoming player a choice — take the balls where
     * they lie, or make the breaker go again — and this game has no way to ask
     * for that, so it is charged in bank pool's own currency instead: a foul,
     * which here costs a ball. It is the same size of penalty the game uses for
     * everything else, which is the point. */
    if (was_break && np == 0 && brk_rails(w, n) < 4) { foul = 1; why = "BREAK"; }
    if (first_hit < 0)        { foul = 1; why = "NO BALL HIT"; }
    else if (scratch)         { foul = 1; why = "SCRATCH"; }
    else if (r->n_off)        { foul = 1; why = "OFF THE TABLE"; }
    else if (!np && !cushion) { foul = 1; why = "NO CUSHION"; }

    /* The banked ones count whatever else happened; the rest go back on. */
    r->score[me] += scored;
    r->respot     = unbanked;

    if (foul) {
        if (r->score[me] > 0) { r->score[me]--; r->respot++; }
        else                    r->op_owed[me]++;
        r->last_foul = 1;
        r->cfoul[me]++;
        if (scratch || r->n_off) r->ball_in_hand = 1;
        r->turn = you;
        r->brk = 0;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        return;
    }
    r->last_foul = 0;
    r->cfoul[me] = 0;

    while (r->op_owed[me] > 0 && r->score[me] > 0) {
        r->score[me]--; r->op_owed[me]--; r->respot++;
    }

    if (r->score[me] >= 8) {
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }

    /* Same as Honolulu: a banked ball is a legal score and keeps the table.
     * The unbanked one is spotted and is not a foul, so there is nothing to
     * give the table up for. */
    if (scored) {
        r->brk += scored;
        if (unbanked) snprintf(r->msg, sizeof r->msg, "%d - ONE SPOTTED", scored);
        else          snprintf(r->msg, sizeof r->msg, "%d", scored);
        return;                                  /* the striker plays on */
    }
    r->brk = 0;
    r->turn = you;
    if (unbanked) snprintf(r->msg, sizeof r->msg, "NOT BANKED");
    else          r->msg[0] = 0;
}

/* ---- G10: KILLER ---------------------------------------------------------
 *
 * One shot each, strictly alternating. Pot any object ball and you are safe;
 * fail to pot — or foul — and one of your three lives goes. A scratch is a
 * life AND ball in hand to the incoming player. The opening break is exempt:
 * nobody loses a life for a dry break, which is the pub's own custom. The
 * rack goes back on when the table runs dry with both players standing.
 */
static void resolve_killer(CueRules *r, CueBall *b, int n, int first_hit,
                           int scratch, const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    const int was_break = r->break_shot;
    r->break_shot = 0;
    r->rerack = 0;

    int foul = 0; const char *why = "";
    if (scratch)             { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0)  { foul = 1; why = "NO BALL"; }
    else if (r->n_off)       { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    /* a scratch still counts the other balls it sank; they stay down */
    const int made = !foul && np > 0;

    if (!made && !(was_break && !foul)) {
        r->score[me]--;
        if (r->score[me] <= 0) {
            r->score[me] = 0;
            r->frame_over = 1; r->winner = you;
            book_frame(r, you);
            snprintf(r->msg, sizeof r->msg, "OUT OF LIVES");
            return;
        }
        snprintf(r->msg, sizeof r->msg, foul ? "FOUL: %s - A LIFE" : "%sA LIFE",
                 foul ? why : "");
    } else if (made) {
        snprintf(r->msg, sizeof r->msg, "SAFE");
    } else {
        r->msg[0] = 0;                       /* a dry break: no harm done */
    }

    /* one shot each, whatever happened */
    r->turn = you;
    if (scratch) r->ball_in_hand = 1;

    /* the table ran dry with both standing: rack it again */
    int left = 0;
    for (int i = 1; i < n; i++) if (b[i].on) left++;
    if (left == 0) { r->rerack = 2; r->racks++; r->break_shot = 1; }
}

/* ---- G5: ENGLISH BILLIARDS ----------------------------------------------
 *
 * Three balls, two of them cue balls, and the whole game is in Section 3 Rules
 * 4, 8, 9, 10, 14 and 15 of the WPBSA book. Scoring:
 *
 *   cannon (the cue ball contacts BOTH object balls)          2
 *   pot the object white / in-off the object white            2
 *   pot the red / in-off the red                              3
 *
 * and Rule 4(c): if more than one hazard, or a combination of hazards and a
 * cannon, are made in the same stroke, ALL are scored. Rule 4(d) prices the
 * in-off by which ball was struck FIRST, not by which one it went in off — so
 * an in-off combined with a cannon is three if the red was contacted first and
 * two if the white was.
 *
 * This is the first game here that cannot be scored from `first_hit`. A cannon
 * is a question about the whole stroke, and the answer is in the cue ball's
 * own account of what it touched (CueWorld.touch), which exists for exactly
 * this and has said so in its comment since it was written.
 *
 * The break continues while the striker scores; it ends on a stroke that
 * scores nothing, and a foul both ends it and gives the opponent two.
 */
static void resolve_billiards(CueRules *r, CueBall *b, int n, const CueWorld *w,
                              int first_hit, int scratch, const int *potted, int np)
{
    const int me = r->turn, you = 1 - r->turn;
    r->break_shot = 0;
    r->bil_respot_red = CUE_BIL_SPOT_NONE;
    r->bil_respot_white = 0;
    /* CONSUMED HERE, whichever way this resolve goes out. The host sets it when
     * it places the ball; leaving it standing would make the NEXT stroke look
     * like one played from hand and put Rule 6 on a shot it does not bind. */
    const int from_hand = r->bil_from_hand;
    r->bil_from_hand = 0;

    /* WHAT THE CUE BALL TOUCHED, and in what order. Only the two object balls
     * matter; a cushion between them changes nothing in billiards (it is
     * three-cushion that cares, and that is not this game). */
    /* `first` is an ID, and one of the ids in this game is ZERO — the object
     * white wears CUE_ID_BIL_WHITE == CUE_ID_CUE when the yellow is the
     * striker's ball. Testing it for truth called every clean stroke the
     * yellow played onto the white a MISS, and priced a white-first in-off as
     * a red one. -1 means nothing struck; nothing else does. */
    int hit_red = 0, hit_white = 0, first = -1;  /* first: the id struck first */
    if (w) {
        for (int i = 0; i < w->ntouch; i++) {
            if (w->touch[i].what != CUE_TOUCH_BALL) continue;
            int id = w->touch[i].id;
            if (id == CUE_ID_BIL_RED) { if (first < 0) first = id; hit_red = 1; }
            else                      { if (first < 0) first = id; hit_white = 1; }
        }
    }
    /* A world that kept no account still has first_hit, which is enough for
     * everything but the cannon — better a game that scores the hazards than
     * one that refuses to run. */
    if (first < 0 && first_hit >= 0) {
        first = first_hit;
        if (first_hit == CUE_ID_BIL_RED) hit_red = 1; else hit_white = 1;
    }

    int pot_red = 0, pot_white = 0;
    for (int k = 0; k < np; k++) {
        if (potted[k] == CUE_ID_BIL_RED) pot_red = 1;
        else if (potted[k] != CUE_ID_CUE) pot_white = 1;
    }
    /* The cue ball is index 0 whatever colour it is wearing, so an in-off is
     * `scratch` — but only a scratch AFTER a contact is an in-off. Section 2
     * Definition 17: the striker in hand who pockets his cue ball having hit
     * nothing is running a coup, and that is a foul (Rule 14(r)). */
    const int in_off = scratch && first >= 0;
    const int cannon = hit_red && hit_white;

    /* ---- BAULK: what a player IN HAND may and may not do (Rule 6) --------
     *
     * Only from in-hand, because that is the only time Rule 6 applies — a ball
     * lying in baulk during ordinary play may be struck however you like.
     *
     * 6(f) is the one with teeth and the only one this can judge honestly. "If
     * an object ball is in Baulk, no part of its surface may be played on
     * directly from in-hand": so playing from the D straight onto a ball that
     * was in baulk, with nothing touched in between, is playing improperly from
     * in-hand and a foul under Rule 14(e). It is what makes the double baulk a
     * shot worth playing — with both object balls behind the line the striker
     * has to go up the table and come back off a cushion, and cannot simply
     * roll up and nudge one.
     *
     * WHAT WENT BEFORE IT decides it: the touch list is in order, so if a
     * cushion or the other object ball came first then this was not a direct
     * stroke and the rule is not engaged. That much is exactly right. What this
     * does NOT test is 6(d)'s further requirement that the cushion be one OUT
     * of baulk, because the touch record carries what was hit and not where —
     * so a stroke into the baulk cushion and back onto a ball in baulk is
     * allowed here and would be a foul at a real table. It is the rarer half of
     * the rule and it costs a per-contact position to judge; noted rather than
     * silently approximated. */
    int baulk_foul = 0;
    if (from_hand && first >= 0 && w) {
        const int first_in_baulk = (first == CUE_ID_BIL_RED) ? r->bil_red_baulk
                                                             : r->bil_wht_baulk;
        if (first_in_baulk) {
            /* `first` IS the first ball touched, so "directly" is settled by
             * whether anything at all came before it — and the only thing that
             * can is a cushion. */
            baulk_foul = (w->ntouch > 0 && w->touch[0].what == CUE_TOUCH_BALL);
        }
    }

    /* ---- the fouls ---- */
    int foul = 0; const char *why = "";
    int miss_only = 0;
    if (first < 0) {
        foul = 1; why = scratch ? "COUP" : "MISS";
        /* RULE 16, AND THE WHOLE POINT OF A DOUBLE BAULK.
         *
         * "If a miss is made, by other than a stroke made directly into a
         * pocket or off a shoulder of a pocket when the striker is in-hand with
         * no object ball out of Baulk, the referee shall call MISS. A penalty
         * of two points is incurred... Any other miss is a foul, and all direct
         * 'coups' are fouls."
         *
         * So the player who is double baulked, plays properly out of baulk and
         * fails to find anything has made a MISS: two points away, and that is
         * all. An ordinary failure to hit is a FOUL, which costs the same two
         * AND gives the incoming player Rule 15(c)(ii) — the balls spotted and
         * the table from hand. That difference is the entire reason for leaving
         * an opponent double baulked, and without it the tactic is free.
         *
         * A coup is excluded by name: running the cue ball into a pocket having
         * hit nothing is a foul from any position (Rule 14(r)). */
        if (!scratch && from_hand && !baulk_foul &&
            r->bil_red_baulk && r->bil_wht_baulk) { miss_only = 1; why = "MISS"; }
    }
    if (baulk_foul) { foul = 1; miss_only = 0; why = "BALL IN BAULK"; }
    if (r->n_off)                     { foul = 1; why = "OFF THE TABLE"; }
    /* Rules 9 and 10, checked on the stroke that would exceed them. A cannon
     * counts toward the cannon limit only when the stroke has no hazard in it,
     * and a hazard toward the hazard limit only with no cannon — which is what
     * "not in conjunction with" means, and why each resets the other. */
    const int hazard = (pot_red || pot_white || in_off);
    if (!foul) {
        if (cannon && !hazard && r->bil_cannons + 1 > CUE_BIL_MAX_CANNONS)
            { foul = 1; why = "75 CANNONS"; }
        if (hazard && !cannon && r->bil_hazards + 1 > CUE_BIL_MAX_HAZARDS)
            { foul = 1; why = "15 HAZARDS"; }
    }
    r->last_foul = foul;

    if (foul) {
        /* Rule 15(b): points already made in the break stand, but the stroke
         * called foul scores nothing. (c): two to the opponent, and never more
         * than two however many ways the stroke was foul. */
        r->score[you] += CUE_BIL_FOUL;
        r->last_foul_pts = CUE_BIL_FOUL;
        r->cfoul[me]++;
        r->brk = 0;
        r->bil_cannons = r->bil_hazards = r->bil_spot_pots = 0;
        /* The red always comes back; the white waits for its owner. */
        if (pot_red) r->bil_respot_red = CUE_BIL_SPOT_SPOT;
        r->turn = you;
        r->bil_yellow = !r->bil_yellow;
        if (scratch) r->ball_in_hand = 1;
        /* Rule 16 again, in the words the board uses. A MISS is not a foul and
         * saying FOUL for one is the referee getting it wrong out loud. */
        snprintf(r->msg, sizeof r->msg, miss_only ? "%s" : "FOUL: %s", why);
        /* Rule 5(a): the stroke that had been made has now been allowed to
         * finish, so a clock that ran out during it ends the game here. */
        if (r->bil_timeup) cue_rules_bil_expire(r);
        return;
    }

    /* ---- the score ---- */
    int pts = 0;
    if (cannon)    pts += CUE_BIL_CANNON;
    if (pot_red)   pts += CUE_BIL_RED;
    if (pot_white) pts += CUE_BIL_WHITE;
    /* Rule 4(d): the in-off is priced by the ball struck FIRST. */
    if (in_off)    pts += (first == CUE_ID_BIL_RED) ? CUE_BIL_RED : CUE_BIL_WHITE;

    if (pts == 0) {
        /* A legal stroke that scored nothing. The turn passes and the table is
         * played as it lies. */
        r->brk = 0;
        r->bil_cannons = r->bil_hazards = r->bil_spot_pots = 0;
        r->turn = you;
        r->bil_yellow = !r->bil_yellow;
        r->msg[0] = 0;
        if (r->bil_timeup) cue_rules_bil_expire(r);
        return;
    }

    r->score[me] += pts;
    r->brk += pts;

    /* The two sequences, each counted only while the other kind is absent. */
    if (cannon && !hazard) r->bil_cannons++; else r->bil_cannons = 0;
    if (hazard && !cannon) r->bil_hazards++; else r->bil_hazards = 0;

    /* Rule 8(b) and (c): the red goes back on the Spot twice and the Centre
     * Spot once, in sequence, for CONTINUED pots of the red not in conjunction
     * with another score. Anything else in the stroke resets the count. */
    if (pot_red) {
        int alone = !cannon && !pot_white && !in_off;
        if (alone) {
            r->bil_spot_pots++;
            r->bil_respot_red = (r->bil_spot_pots >= 3) ? CUE_BIL_SPOT_CENTRE
                                                        : CUE_BIL_SPOT_SPOT;
            if (r->bil_spot_pots >= 3) r->bil_spot_pots = 0;
        } else {
            r->bil_spot_pots = 0;
            r->bil_respot_red = CUE_BIL_SPOT_SPOT;
        }
    }
    /* An in-off leaves the striker in hand, and he plays on. Rule 3: the break
     * continues "from the position left or, after an in-off, from in-hand". */
    if (in_off) r->ball_in_hand = 1;

    if (r->target_score > 0 && r->score[me] >= r->target_score) {
        r->frame_over = 1; r->winner = me; book_frame(r, me);
        snprintf(r->msg, sizeof r->msg, "GAME");
        return;
    }
    snprintf(r->msg, sizeof r->msg, "%d", pts);
    /* "Any stroke that has been made shall be allowed to finish and any points
     * scored shall be added to the appropriate side" — added first, on the line
     * above, and only then is it TIME. */
    if (r->bil_timeup) cue_rules_bil_expire(r);
    (void)b; (void)n;
}

/* ---- G7: BILLIARDS GOLF -------------------------------------------------
 *
 * The simplest resolver in the building, because the game has almost no rules:
 * every stroke costs one, the cue ball down a hole costs one more and goes
 * back, and the hole ends when the reds are gone or eight have been played.
 * What it does NOT have is what everything else here is made of — there is no
 * foul, no turn to lose, no break to keep. The other player is not waiting for
 * you to miss; they are waiting for you to finish, and then they play the same
 * hole. That is what a golf card records.
 */
static void resolve_golf(CueRules *r, CueBall *b, int n, int scratch)
{
    const int me = r->turn;
    r->break_shot = 0;
    r->golf_rack = 0;
    r->golf_reset_cue = 0;
    r->last_foul = 0;

    r->golf_strokes++;                       /* the stroke itself */
    /* Rule 2: it is a PENALTY, not a foul. Nothing changes hands — there is
     * nothing to hand over — so the ball goes back and the player plays on. */
    if (scratch) {
        r->golf_strokes++;
        r->golf_reset_cue = 1;
    }

    int left = 0;
    for (int i = 1; i < n; i++) if (b[i].on) left++;

    const int cleared = (left == 0);
    const int maxed   = (r->golf_strokes >= CUE_GOLF_MAX_STROKES);
    /* MATCHPLAY: A HOLE YOU CANNOT TIE IS ALREADY LOST.
     *
     * The second player to a hole knows exactly what it takes: the opponent's
     * number. Once they have played that many and the hole is not cleared,
     * their next shot can only be worse, so nothing they do from here changes
     * the result — and in matchplay the size of the loss is worth nothing
     * either. They pick up. In strokeplay every shot still counts towards the
     * total, so this applies to the match rounds only. */
    int conceded = 0;
    if (!cleared && !r->golf_solo && CUE_GOLF_IS_MATCH(r->golf_round)) {
        const int theirs = r->golf_card[1 - me][r->golf_hole];
        if (theirs && r->golf_strokes >= theirs) conceded = 1;
    }
    if (!cleared && !maxed && !conceded) {
        /* Still on the hole. The score IS the stroke count, so that is the
         * only thing worth saying. */
        snprintf(r->msg, sizeof r->msg, "%d", r->golf_strokes);
        return;
    }

    /* The hole is done, one way or the other. Rule 3 caps it at eight. */
    int score = r->golf_strokes;
    if (score > CUE_GOLF_MAX_STROKES) score = CUE_GOLF_MAX_STROKES;
    if (conceded) score = CUE_GOLF_CONCEDED;
    r->golf_card[me][r->golf_hole] = (uint8_t)score;
    r->golf_strokes = 0;

    const int par = CUE_GOLF_COURSE[r->golf_hole].par;
    if (conceded)              snprintf(r->msg, sizeof r->msg, "PICK UP");
    else if (!cleared)         snprintf(r->msg, sizeof r->msg, "%d - LIMIT", score);
    else if (score == 1)       snprintf(r->msg, sizeof r->msg, "HOLE IN ONE");
    else if (score <= par - 2) snprintf(r->msg, sizeof r->msg, "%d - EAGLE", score);
    else if (score == par - 1) snprintf(r->msg, sizeof r->msg, "%d - BIRDIE", score);
    else if (score == par)     snprintf(r->msg, sizeof r->msg, "%d - PAR", score);
    else if (score == par + 1) snprintf(r->msg, sizeof r->msg, "%d - BOGEY", score);
    else                       snprintf(r->msg, sizeof r->msg, "%d", score);

    /* Has the other player still to play this hole? Two-handed only: against
     * nobody, the course simply moves on. */
    const int you = 1 - me;
    const int solo = r->golf_solo;
    if (!solo && r->golf_card[you][r->golf_hole] == 0) {
        r->turn = you;                       /* their turn at the same hole */
        r->golf_rack = 1;                    /* ...set out fresh for them */
        return;
    }

    /* Both have played it. THE HONOUR GOES TO THE LOWER SCORE — and a tie
     * leaves it where it was, which is why it has to be remembered rather than
     * read off the card. */
    r->golf_done = 1;
    if (!solo) {
        const int lo = r->golf_card[0][r->golf_hole];
        const int hi = r->golf_card[1][r->golf_hole];
        if (lo < hi)      r->golf_honour = 0;
        else if (hi < lo) r->golf_honour = 1;
        /* AND IN MATCHPLAY THE HOLE HAS A RESULT, which is the whole of the
         * scoring: strokes decide who won it and are then done with. Saying
         * "you took five" tells a matchplay player nothing they need. */
        if (CUE_GOLF_IS_MATCH(r->golf_round)) {
            const int up = cue_rules_golf_holes_up(r);
            const char *res = (lo == hi) ? "HOLE HALVED"
                            : (lo < hi)  ? "HOLE TO YOU" : "HOLE TO THEM";
            if (up == 0) snprintf(r->msg, sizeof r->msg, "%s - ALL SQUARE", res);
            else snprintf(r->msg, sizeof r->msg, "%s - %d UP", res,
                          up > 0 ? up : -up);
        }
    }
    const int first = cue_golf_first(r->golf_round);
    const int last  = cue_golf_last(r->golf_round);
    /* MATCHPLAY IS DECIDED HOLE BY HOLE, and can be over before the holes are.
     *
     * Each hole is won, lost or halved on its own and the totals are thrown
     * away — so a player who is more holes up than there are holes left has
     * won, whatever the cards say. That is the whole difference: a disastrous
     * hole costs you that hole and nothing else, where in strokeplay a nine
     * follows you round. On a table, where a bad hole can be a dozen shots,
     * that matters more than it does on grass. */
    if (!solo && CUE_GOLF_IS_MATCH(r->golf_round)) {
        const int up = cue_rules_golf_holes_up(r);
        const int left = last - r->golf_hole;
        if (up > left || r->golf_hole >= last) {
            r->frame_over = 1;
            r->winner = (up > 0) ? 0 : (up < 0) ? 1 : -1;
            if (r->winner >= 0) book_frame(r, r->winner);
            if (up != 0 && left > 0)
                snprintf(r->msg, sizeof r->msg, "%d AND %d",
                         up > 0 ? up : -up, left);
            else if (up != 0) snprintf(r->msg, sizeof r->msg, "%d UP",
                                       up > 0 ? up : -up);
            else snprintf(r->msg, sizeof r->msg, "HALVED");
            return;
        }
    } else if (r->golf_hole >= last) {
        r->frame_over = 1;
        int a = cue_rules_golf_total(r, 0, first, last);
        int c = cue_rules_golf_total(r, 1, first, last);
        r->winner = solo ? 0 : (a == c ? -1 : (a < c ? 0 : 1));   /* LOW wins */
        if (r->winner >= 0 && !solo) book_frame(r, r->winner);
        snprintf(r->msg, sizeof r->msg, "ROUND OVER");
        return;
    }
    r->golf_hole++;
    r->golf_rack = 1;
    if (!solo) r->turn = r->golf_honour;     /* the honour leads off */
}

/* HOLES UP, from seat 0's side: +2 means seat 0 is two holes to the good, -1
 * means seat 1 is one up, 0 is all square. Only holes BOTH have played count —
 * a hole in progress is nobody's yet. */
int cue_rules_golf_holes_up(const CueRules *r) {
    if (!r) return 0;
    const int first = cue_golf_first(r->golf_round);
    const int last  = cue_golf_last(r->golf_round);
    int up = 0;
    for (int h = first; h <= last && h < CUE_GOLF_HOLES; h++) {
        const int a = r->golf_card[0][h], b = r->golf_card[1][h];
        if (!a || !b) continue;                /* not finished by both */
        if (a < b) up++; else if (b < a) up--; /* LOW wins a hole */
    }
    return up;
}

int cue_rules_golf_total(const CueRules *r, int who, int from_hole, int to_hole) {
    if (!r || who < 0 || who > 1) return 0;
    int t = 0;
    for (int h = from_hole; h <= to_hole && h < CUE_GOLF_HOLES; h++) {
        if (h < 0) continue;
        /* A HOLE PICKED UP HAS NO STROKE COUNT — it is only ever a matchplay
         * thing and matchplay does not add up, but the sentinel must not be
         * allowed to land in a total as 255 wherever one is still drawn. It
         * counts as the limit, which is the worst a hole can cost. */
        const int v = r->golf_card[who][h];
        t += (v == CUE_GOLF_CONCEDED) ? CUE_GOLF_MAX_STROKES : v;
    }
    return t;
}

int cue_rules_golf_leader(const CueRules *r) {
    if (!r) return -1;
    int a = cue_rules_golf_total(r, 0, 0, CUE_GOLF_HOLES - 1);
    int c = cue_rules_golf_total(r, 1, 0, CUE_GOLF_HOLES - 1);
    if (!a && !c) return -1;
    return (a == c) ? 2 : (a < c ? 0 : 1);
}

int cue_rules_bb_in_baulk(const CueRules *r, const CueTable *t,
                          const CueBall *b, int n)
{
    if (!t || !b || t->baulk_arc <= 0.0f) return 0;
    const float R = r ? r->R : t->R;
    /* Half the angle between the lines, measured from the up-table axis. */
    const float th = t->baulk_arc * 0.5f * 3.14159265f / 180.0f;
    const float st = sinf(th), ct = cosf(th);
    for (int i = 0; i < n; i++) {
        if (!b[i].on) continue;
        float u = b[i].pos.x - t->baulk_x;      /* up the table from the break spot */
        float v = b[i].pos.z;
        /* Behind the V, and "obstructing" it counts as behind (Rule 110(c)):
         * a ball obscuring any part of the line is in, so its own radius is
         * allowed for. */
        if (u * st - fabsf(v) * ct <= R) return 1;
        /* Rule 110(d): and anything on the D itself. */
        if (u*u + v*v <= (t->d_radius + R) * (t->d_radius + R)) return 1;
    }
    return 0;
}

int cue_rules_bb_short(const CueTable *t, float furthest_x, int hit_something)
{
    if (!t || hit_something) return 0;
    /* The line through the black peg, parallel with the top cushion. The peg
     * stands just in front of the 200, which is black_x. */
    return furthest_x < t->black_x - 0.045f;
}

/* Rules 110(c) and (d) do not stop at calling the foul: the ball that came
 * back over the baulk line, or stopped on the D, "should be returned to the
 * rack". Same geometry as cue_rules_bb_in_baulk, acted on instead of merely
 * reported; the host calls it after the resolve has read the flag. */
int cue_rules_bb_baulk_return(const CueRules *r, const CueTable *t,
                              CueBall *b, int n)
{
    if (!t || !b || t->baulk_arc <= 0.0f) return 0;
    const float R = r ? r->R : t->R;
    const float th = t->baulk_arc * 0.5f * 3.14159265f / 180.0f;
    const float st = sinf(th), ct = cosf(th);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!b[i].on) continue;
        float u = b[i].pos.x - t->baulk_x, v = b[i].pos.z;
        if (u * st - fabsf(v) * ct <= R ||
            u*u + v*v <= (t->d_radius + R) * (t->d_radius + R)) {
            b[i].on = 0; b[i].pocket = 0; b[i].drop = 0.0f;
            b[i].vel = v3(0,0,0); b[i].w = v3(0,0,0);
            m++;
        }
    }
    return m;
}

int cue_rules_bb_setup(CueRules *r, const CueTable *t, CueBall *b, int n) {
    if (!r || !t || !b || n <= 0) return 0;
    if (r->frame_over) return 0;
    const float R = t->R;
    /* Which balls are up, and which of the ones that are not are still in the
     * game at all. Once the bar has dropped, a potted ball is swallowed. */
    int up = 0, red_up = -1;
    for (int i = 0; i < n; i++) if (b[i].on) { up++; if (b[i].id == CUE_ID_BIL_RED) red_up = i; }
    int in_play = r->bb_barred ? r->bb_left : n;
    if (in_play > n) in_play = n;

    int placed = 0;
    /* EVERY STROKE IS PLAYED FROM THE D (Rule 91), with a ball taken by hand
     * (Rule 96) — so index 0, the ball the engine strikes, must be one the
     * striker is entitled to lift. If the last shot left it out on the cloth
     * it STAYS there as an object ball and the striker takes another: a white
     * from the rack while the rack holds one (the red is optional as a cue
     * ball, Rule 95, so it is never forced on you), and when the rack is
     * empty, the ball furthest from the top cushion, nearest the centre line
     * on a tie (Rule 105). The swap keeps the struck ball at index 0, which
     * is what the physics, the camera and the cue all assume. */
    /* ...EXCEPT WHEN THE BALL AT INDEX 0 IS THE ONE ALREADY IN HAND, which is
     * how the break position is set out: Rule 92 puts a white ON THE BREAK
     * SPOT and that white is the striker's ball, not one stranded on the
     * cloth. Read as stranded, the rack handed him a second ball and stood it
     * on top of the first — two balls at the same point, which is a table the
     * physics cannot answer for and the planner cannot see a line through.
     *
     * A ball genuinely left at rest there is not a case: the break spot is the
     * centre of the D, and Rule 110(d) returns anything resting on the D to
     * the rack before this ever runs. */
    int in_hand = b[0].on &&
                  fabsf(b[0].pos.x - t->baulk_x) < R * 0.25f &&
                  fabsf(b[0].pos.z) < R * 0.25f;
    if (b[0].on && !in_hand) {
        int pick = -1;
        if (up < in_play) {
            for (int i = 1; i < n; i++)
                if (!b[i].on && b[i].id != CUE_ID_BIL_RED) { pick = i; break; }
            if (pick < 0)
                for (int i = 1; i < n; i++)
                    if (!b[i].on) { pick = i; break; }
        }
        if (pick < 0) {
            /* Rule 105 — over every ball ON the table, index 0 included, so
             * if the one already in hand is the furthest back it simply
             * stays. The top cushion is +x; furthest from it is least x. */
            float bx = 1e9f, bz = 1e9f; int best = 0;
            for (int i = 0; i < n; i++) {
                if (!b[i].on) continue;
                float az = fabsf(b[i].pos.z);
                if (b[i].pos.x < bx - 1e-6f ||
                    (b[i].pos.x < bx + 1e-6f && az < bz)) {
                    best = i; bx = b[i].pos.x; bz = az;
                }
            }
            if (best != 0) pick = best;
        }
        if (pick > 0) { CueBall tmp = b[0]; b[0] = b[pick]; b[pick] = tmp; }
    }
    if (!b[0].on && up < in_play) {
        b[0].on = 1; b[0].pocket = 0; b[0].drop = 0.0f;
        b[0].vel = v3(0,0,0); b[0].w = v3(0,0,0);
        up++; placed = 1;
    }
    /* Lifted, so it starts from the break spot — the centre of the D — and
     * the host lets the striker walk it about the D from there (Rule 96),
     * except on the two shots the rules pin down exactly: the break plays
     * from the spot itself (Rule 92) and the last ball from the centre of
     * the D (Rule 108). */
    if (b[0].on) {
        b[0].pos = v3(t->baulk_x, R, 0.0f);
        b[0].vel = v3(0,0,0); b[0].w = v3(0,0,0);
        placed = 1;
    }
    /* The break position: no object ball on the table, so the red goes back on
     * the red spot and the white on the break spot (Rules 92, 94, 95). */
    int objects = up - (b[0].on ? 1 : 0);
    r->bb_from_break = (objects <= 0);
    if (r->bb_from_break) {
        if (red_up < 0 && !r->bb_barred) {
            for (int i = 1; i < n; i++)
                if (!b[i].on && b[i].id == CUE_ID_BIL_RED) {
                    b[i].on = 1; b[i].pocket = 0; b[i].drop = 0.0f;
                    b[i].vel = v3(0,0,0); b[i].w = v3(0,0,0);
                    b[i].pos = v3(t->blue_x, R, 0.0f);   /* the red spot */
                    placed = 1; break;
                }
        }
        if (b[0].on) b[0].pos = v3(t->baulk_x, R, 0.0f);  /* the break spot */
    }
    /* ONCE THE BAR IS DOWN, WHAT IS LEFT IN THE GAME IS WHAT IS ON THE CLOTH.
     * A ball in the trough does not come back out of it (which is what the bar
     * dropping IS), and bb_left cannot tell a ball in the trough from one that
     * was never dealt — so the table is asked instead. Without it a game whose
     * objects were all swallowed sat with a ball in hand, nothing to play at,
     * and a count insisting six balls were still in play. */
    if (r->bb_barred) {
        int left = 0;
        for (int i = 0; i < n; i++) if (b[i].on) left++;
        r->bb_left = left;
    }
    /* Rule 108: with one ball left it is the last-ball shot, into the 100 or
     * the 200 off a side cushion. Flagged for the host and the scorer; the
     * shot itself is the player's problem. */
    r->bb_last_ball = (r->bb_barred && r->bb_left <= 1 && b[0].on);
    /* Rule 108: the break score is recorded BEFORE the last-ball shot is
     * played — nothing that happens from here can take it back off. */
    if (r->bb_last_ball) { r->bb_break = 0; r->brk = 0; }
    r->ball_in_hand = (!r->bb_from_break && !r->bb_last_ball && b[0].on);
    /* And the game is over when the last ball has been swallowed. */
    if (r->bb_barred && r->bb_left <= 0 && !r->frame_over) {
        r->frame_over = 1;
        r->winner = (r->score[0] == r->score[1]) ? -1
                  : (r->score[0] > r->score[1]) ? 0 : 1;
        if (r->winner >= 0) book_frame(r, r->winner);
        snprintf(r->msg, sizeof r->msg, "TIME");
    }
    return placed;
}

void cue_rules_bb_tick(CueRules *r, float dt) {
    if (!r || r->bb_barred) return;
    r->bb_time -= dt;
    if (r->bb_time <= 0.0f) { r->bb_time = 0.0f; r->bb_barred = 1; }
}

/* ---- ENGLISH BILLIARDS AGAINST THE CLOCK (Section 3 Rule 5) -------------- */

void cue_rules_bil_set_time(CueRules *r, float secs) {
    if (!r) return;
    r->bil_time     = secs > 0.0f ? secs : 0.0f;
    r->bil_time_len = r->bil_time;
    r->bil_timeup   = 0;
    /* A game is played to TIME or to POINTS and not to both — Rule 1(f) offers
     * them as alternatives. Leaving a target standing under a clock would end a
     * timed game early on a number nobody agreed to. */
    if (r->bil_time > 0.0f) r->target_score = 0;
}

void cue_rules_bil_tick(CueRules *r, float dt) {
    if (!r || r->bil_time <= 0.0f || r->bil_timeup || r->frame_over) return;
    r->bil_time -= dt;
    if (r->bil_time <= 0.0f) {
        r->bil_time = 0.0f;
        /* "the referee shall call TIME" — and nothing more, yet. Rule 5(a)
         * allows the stroke that has been made to finish and score. */
        r->bil_timeup = 1;
    }
}

void cue_rules_bil_expire(CueRules *r) {
    if (!r || r->frame_over || !r->bil_timeup) return;
    r->frame_over = 1;
    /* Rule 1(f)(i): most points in the stipulated time. Rule 5(c) allows the
     * scores to be level, and says the rules setting the time must provide for
     * a tie-break — so with none set this is a drawn game rather than a made-up
     * winner. book_frame refuses a winner of -1, which is what a draw is. */
    r->winner = (r->score[0] == r->score[1]) ? -1
              : (r->score[0] > r->score[1]) ? 0 : 1;
    if (r->winner >= 0) book_frame(r, r->winner);
    snprintf(r->msg, sizeof r->msg,
             r->winner < 0 ? "TIME - GAME DRAWN" : "TIME");
}

/* Rule 8(a): the Spot, then the Pyramid Spot, then the Centre Spot. Rule
 * 8(b): where the sequence sends it to the Centre Spot, the fallbacks are the
 * Pyramid Spot and then the Spot. Two orders, and which one applies depends on
 * which mark was asked for — so the order is written out rather than derived. */
int cue_rules_billiards_respot(CueRules *r, const CueTable *t,
                               CueBall *b, int n)
{
    if (!r || !t || !b) return 0;
    const int want = r->bil_respot_red;
    r->bil_respot_red = CUE_BIL_SPOT_NONE;
    if (want == CUE_BIL_SPOT_NONE) return 0;

    /* Which ball is the red, and is it actually off? */
    int idx = -1;
    for (int i = 0; i < n; i++) if (b[i].id == CUE_ID_BIL_RED) { idx = i; break; }
    if (idx < 0 || b[idx].on) return 0;

    static const int from_spot[3]   = { CUE_BIL_SPOT_SPOT, CUE_BIL_SPOT_PYRAMID,
                                        CUE_BIL_SPOT_CENTRE };
    static const int from_centre[3] = { CUE_BIL_SPOT_CENTRE, CUE_BIL_SPOT_PYRAMID,
                                        CUE_BIL_SPOT_SPOT };
    const int *order = (want == CUE_BIL_SPOT_CENTRE) ? from_centre : from_spot;

    const float R = r->R > 0.0f ? r->R : 0.02625f;
    for (int k = 0; k < 3; k++) {
        Vec3 p = r->spot[order[k]];
        p.y = R;
        int clash = 0;
        for (int j = 0; j < n && !clash; j++) {
            if (j == idx || !b[j].on) continue;
            float dx = b[j].pos.x - p.x, dz = b[j].pos.z - p.z;
            /* Section 2 Definition 19: occupied means a ball cannot be placed
             * there without touching another. */
            if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R) * 0.999f) clash = 1;
        }
        if (clash) continue;
        b[idx].pos = p; b[idx].vel = v3(0,0,0); b[idx].w = v3(0,0,0);
        b[idx].on = 1; b[idx].pocket = 0; b[idx].drop = 0.0f;
        return 1;
    }
    /* All three occupied. Walk up the table from the Spot, as every other
     * spotted ball in this engine does when its mark is taken. */
    {   Vec3 up = r->spot_up;
        if (up.x == 0.0f && up.z == 0.0f) up = v3(1,0,0);
        Vec3 base = r->spot[CUE_BIL_SPOT_SPOT];
        for (int step = 1; step < 60; step++) {
            Vec3 p = v3(base.x + up.x * (float)step * 2.05f * R, R,
                        base.z + up.z * (float)step * 2.05f * R);
            if (!cue_table_on_bed(t, p.x, p.z)) continue;
            int clash = 0;
            for (int j = 0; j < n && !clash; j++) {
                if (j == idx || !b[j].on) continue;
                float dx = b[j].pos.x - p.x, dz = b[j].pos.z - p.z;
                if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R) * 0.98f) clash = 1;
            }
            if (clash) continue;
            b[idx].pos = p; b[idx].vel = v3(0,0,0); b[idx].w = v3(0,0,0);
            b[idx].on = 1; b[idx].pocket = 0; b[idx].drop = 0.0f;
            return 1;
        } }
    return 0;
}

/* SHOOTOUT: the ten minutes are up. The scores decide it — or fail to, and
 * the host stages the blue-ball tie-break and reports its verdict back through
 * cue_rules_shootout_win. Both are host-called because only the host has a
 * clock; the frame bookkeeping is the rules' own. */
int cue_rules_shootout_time(CueRules *r) {
    if (!r || r->frame_over) return 1;
    if (r->score[0] == r->score[1]) return 0;      /* a tie: the blue decides */
    r->frame_over = 1;
    r->winner = (r->score[0] > r->score[1]) ? 0 : 1;
    book_frame(r, r->winner);
    snprintf(r->msg, sizeof r->msg, "TIME");
    return 1;
}

void cue_rules_shootout_win(CueRules *r, int winner) {
    if (!r || r->frame_over) return;
    r->frame_over = 1;
    r->winner = winner ? 1 : 0;
    book_frame(r, r->winner);
    snprintf(r->msg, sizeof r->msg, "SHOOTOUT");
}

void cue_rules_billiards_swap(CueBall *b, int n) {
    if (!b || n < 3) return;
    int other = -1;
    for (int i = 1; i < n; i++)
        if (b[i].id == CUE_ID_BIL_WHITE || b[i].id == CUE_ID_BIL_YELLOW) { other = i; break; }
    if (other < 0) return;
    CueBall tmp = b[0]; b[0] = b[other]; b[other] = tmp;
}

static void resolve_straight(CueRules *r, CueBall *b, int n, const CueWorld *w,
                             int first_hit,
                             int scratch, int cushion, const int *potted, int np) {
    const int was_break = r->break_shot;
    /* The call belongs to this stroke and no other, whatever becomes of it. */
    const int called_id  = r->nominated;
    const int called_pkt = r->called_pocket;
    r->nominated = 0; r->called_pocket = -1;
    r->break_shot = 0;

    /* Did the called ball go down the called pocket? CueBall.pocket carries the
     * pocket it fell in, which is what makes calling a pocket enforceable rather
     * than an honour system. */
    int made_call = 0;
    for (int k = 0; k < np && called_id; k++) {
        if (potted[k] != called_id) continue;
        const CueBall *q = find_ball(b, n, potted[k]);
        if (!q || q->pocket == CUE_OFF_TABLE) continue;   /* driven off, not potted */
        if (called_pkt < 0 || (int)q->pocket == called_pkt) made_call = 1;
    }

    /* THE OPENING BREAK has its own requirement and its own price: two object
     * balls to a cushion, or a called ball down, and any failure costs TWO
     * points rather than one. It is the only stroke in the game that costs more.
     *
     * `cushion` is "some ball reached a cushion", not a count of them, so the
     * two-ball part of the rule is enforced as one. The honest reading is that a
     * bad break is under-punished, never over-punished: every break this passes
     * did send a ball to a rail. Counting them wants the contact log F6 added,
     * and is worth doing when the AI plays this game.
     *
     * The break check cannot be a separate test after the ordinary ones — the
     * generic no-rail rule fires on exactly the same conditions and claimed the
     * foul first, so the break price never applied. It is the same foul at a
     * different price, so it is the same branch with a different name. */
    int foul = 0; const char *why = "";
    /* WPA 7.3(b), and the only break rule in the game that asks about the CUE
     * BALL: "If no called ball is pocketed, the cue-ball AND two object-balls
     * must each be driven to a rail after the cue-ball contacts the rack or the
     * shot is a breaking foul." Conjunctive — two object balls is not enough on
     * its own, and neither is the cue ball.
     *
     * It is priced differently too: TWO points off rather than the one a
     * standard foul costs, and 7.10 says that where both happen on one stroke
     * it counts as the breaking foul alone, so the two do not stack. 7.11 keeps
     * it out of the three-consecutive-fouls count. */
    if (was_break && np == 0 && (brk_rails(w, n) < 2 || !brk_cue_rail(w)))
        { foul = 1; why = "BREAK"; }
    if (scratch)                        { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0)             { foul = 1; why = was_break ? "BREAK" : "NO BALL"; }
    else if (np == 0 && !cushion)       { foul = 1; why = was_break ? "BREAK" : "NO RAIL"; }
    if (r->n_off && !foul)              { foul = 1; why = "OFF THE TABLE"; }
    /* ANY foul on the break costs two rather than one (7.3(b), 7.10) — which
     * includes the break's own rule above, so nothing extra is charged for it
     * and the two do not stack. */
    const int break_foul = (was_break && foul);
    r->last_foul = foul;

    /* SCORING. The called ball in the called pocket scores, and so does
     * everything else that went down with it — the accidental extra is a real
     * part of the game and always has been. Nothing scores otherwise. */
    int scored = 0;
    if (!foul && made_call) {
        for (int k = 0; k < np; k++) {
            const CueBall *q = find_ball(b, n, potted[k]);
            if (q && q->pocket != CUE_OFF_TABLE) scored++;
        }
    }

    /* Everything potted that did not score comes back on the long string. On a
     * scoring stroke that is nothing; on a foul, a safety or an uncalled pot it
     * is every ball that went down. */
    if (!scored) {
        for (int k = 0; k < np; k++) {
            if (potted[k] == CUE_ID_CUE) continue;        /* the white is replaced, not spotted */
            spot_straight(r, b, n, potted[k]);
        }
    }

    if (foul) {
        r->score[r->turn] -= break_foul ? 2 : 1;
        r->cfoul[r->turn]++;
        r->last_foul_pts = break_foul ? 2 : 1;
        if (r->cfoul[r->turn] >= 3) {
            /* THREE IN A ROW: fifteen more off the score and the whole table
             * comes back, with the offender breaking it. The heaviest penalty in
             * any game here, and it exists because without it a losing player
             * would simply foul safe forever. */
            r->score[r->turn] -= 15;
            r->cfoul[r->turn] = 0;
            r->rerack = 2; r->racks++;
            r->break_shot = 1;
            r->ball_in_hand = 1;
            snprintf(r->msg, sizeof r->msg, "3 FOULS -15");
            return;                       /* offender breaks the new rack */
        }
        r->turn = 1 - r->turn;
        r->ball_in_hand = scratch ? 1 : 0;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s -%d", why, break_foul ? 2 : 1);
    } else {
        r->cfoul[r->turn] = 0;
        if (scored) {
            r->score[r->turn] += scored;
            r->brk += scored;
            snprintf(r->msg, sizeof r->msg, "BREAK %d", r->brk);
        } else {
            /* A safety, or a called shot that missed. Either way the table goes
             * over and the run ends. */
            r->brk = 0;
            r->turn = 1 - r->turn;
            snprintf(r->msg, sizeof r->msg, "%s", called_id ? "MISSED CALL" : "SAFETY");
        }
    }

    /* THE RERACK, after the score is settled: one ball left and the fourteen go
     * back with the apex empty; none left and the whole fifteen do. Asked after
     * the spotting above, so a ball that came back off a foul is counted. */
    int left = straight_left(b, n);
    if (left <= 1 && !r->frame_over) {
        r->rerack = (left == 1) ? 1 : 2;
        r->racks++;
        /* Clearing the table outright is a fresh start, not a continuation:
         * all fifteen go back and the striker breaks them, from in hand behind
         * the head string, exactly as at the beginning of the frame. With one
         * ball left there is a break ball on the table and none of that applies
         * — the run simply carries on. */
        if (left == 0) { r->break_shot = 1; r->ball_in_hand = 1; }
    }

    /* THE TARGET, asked of both players rather than of whoever is at the table:
     * the turn has already changed hands by this point on a miss or a foul, so
     * "did the striker reach it" is a question about the wrong player half the
     * time. Nobody can reach it on a foul — a foul only ever subtracts. */
    for (int p = 0; p < 2 && !r->frame_over; p++) {
        if (r->score[p] < r->target_score) continue;
        r->frame_over = 1; r->winner = p;
        r->rerack = 0;                     /* the frame is over; do not lay a rack */
        book_frame(r, r->winner);
        snprintf(r->msg, sizeof r->msg, "FRAME WON!");
    }
}

void cue_rules_call_shot(CueRules *r, int ball_id, int pocket) {
    if (!r) return;
    if (r->mode == CUE_GAME_STRAIGHT) {
        r->nominated = (ball_id >= 1 && ball_id <= 15) ? ball_id : 0;
        r->called_pocket = r->nominated ? pocket : -1;
    } else if (r->mode == CUE_GAME_BOWLLIARDS) {
        /* Ten balls, so ten legal names. Unconditional like straight pool's
         * above rather than gated on call_shot_on: there is no bowlliards that
         * does not call, so a switch for it would only be a way of turning the
         * game off. */
        r->nominated = (ball_id >= 1 && ball_id <= 10) ? ball_id : 0;
        r->called_pocket = r->nominated ? pocket : -1;
    } else if (r->mode == CUE_GAME_CRIBBAGE) {
        /* All fifteen are nameable — including the 15, which cannot score early
         * but can perfectly well be shot at and named. Unconditional like
         * bowlliards' above rather than gated on call_shot_on: there is no
         * cribbage pool that does not call, so a switch for it would only be a
         * way of turning the game off. */
        r->nominated = (ball_id >= 1 && ball_id <= 15) ? ball_id : 0;
        r->called_pocket = r->nominated ? pocket : -1;
    } else if (r->mode == CUE_GAME_US10 && r->call_shot_on) {
        r->nominated = (ball_id >= 1 && ball_id <= 10) ? ball_id : 0;
        r->called_pocket = r->nominated ? pocket : -1;
    } else if ((r->mode == CUE_GAME_US8 || r->mode == CUE_GAME_CN8) &&
               r->call_shot_on) {
        r->nominated = (ball_id >= 1 && ball_id <= 15) ? ball_id : 0;
        r->called_pocket = r->nominated ? pocket : -1;
    }
}

void cue_rules_set_target(CueRules *r, int points) {
    if (!r || points < 1) return;
    r->target_score = points;
}

void cue_rules_resolve(CueRules *r, CueBall *b, int n, const CueWorld *w,
                       int first_hit, int scratch, int cushion,
                       const int *potted, int np) {
    /* NAMED RESPOTS ARE THIS STROKE'S, so they are cleared going IN. Cleared on
     * the way out instead, a resolver that sets none would inherit the last
     * one's names and spot a ball nobody potted. */
    for (int i = 0; i < 8; i++) r->respot_id[i] = 0;

    r->ball_in_hand = 0;
    r->last_foul = 0;
    r->last_miss = 0;
    r->last_foul_pts = 0;
    r->rerack = 0;                     /* the host consumed the last one */

    /* C2b: THE TIP FOUND THE WRONG BALL.
     *
     * Not a new penalty and deliberately not one. If the tip never reached the
     * striker's own cue ball, then that ball struck nothing — which is the
     * oldest foul there is, and one every game below already prices in its own
     * currency: four away at snooker, two at billiards, ball in hand at pool,
     * a ball back at pyramid, the break lost at bar billiards. Saying it once,
     * here, as the fact it is, gets all six right and keeps them right.
     *
     * What is NOT touched is anything that actually happened. Balls the wrong
     * ball knocked in are still potted and still go through each game's foul
     * path, because they are on the floor of the pocket either way and a rule
     * that pretended otherwise would leave the table disagreeing with itself.
     * Only the message is taken over afterwards, so the player is told what
     * they did rather than being told they missed. */
    const int wrong_ball = r->cued_id && b[0].on && r->cued_id != b[0].id;
    if (wrong_ball) first_hit = -1;

    if (r->kind)                            { resolve_snooker(r, b, n, w, first_hit, scratch, cushion, potted, np);
                                              r->att_have = 0; }
    else if (r->mode == CUE_GAME_PAUL)      resolve_paul(r, b, n, first_hit, scratch, cushion, potted, np);
    /* ROTATION FIRST, because IS_ROTATION now matches it: the family is "the
     * lowest ball is the one on", which rotation is the original of, but 9- and
     * 10-ball are won by potting ONE named ball and rotation is won on points.
     * Tested the other way round, rotation would be resolved as 9-ball and the
     * frame would end when the 15 went down. */
    else if (CUE_GAME_IS_ROT61(r->mode))
        resolve_rotation(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (CUE_GAME_IS_ROTATION(r->mode)) resolve_9ball(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_STRAIGHT)  resolve_straight(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (CUE_GAME_IS_PYRAMID(r->mode))   resolve_pyramid(r, b, n, first_hit, scratch, cushion, potted, np);
    else if (CUE_GAME_IS_CAROM(r->mode))     resolve_carom(r, b, n, w, first_hit);
    else if (CUE_GAME_IS_KILLER(r->mode))    resolve_killer(r, b, n, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_BILLIARDS)  resolve_billiards(r, b, n, w, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_BARBILLIARDS) resolve_barbilliards(r, b, n, w, first_hit, potted, np);
    else if (r->mode == CUE_GAME_SPEED)
        resolve_speed(r, b, n, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_HONOLULU)
        resolve_honolulu(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_BOWLLIARDS)
        resolve_bowlliards(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_CRIBBAGE)
        resolve_cribbage(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_COWBOY)
        resolve_cowboy(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_BANKPOOL)
        resolve_bank(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_ONEPOCKET)
        resolve_onepocket(r, b, n, w, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_GOLF) resolve_golf(r, b, n, scratch);
    else                                    resolve_pool(r, b, n, w, first_hit, scratch, cushion, potted, np);
    if (wrong_ball && r->last_foul && !r->frame_over)
        snprintf(r->msg, sizeof r->msg, "FOUL: WRONG BALL CUED");

    /* The host's observations are about the shot just resolved and nothing
     * else. Left set they would foul the NEXT one too. */
    r->cued_id = 0;
    r->jumped = 0;
    r->n_off = 0;
    r->bb_in_baulk = 0;
    r->bb_short = 0;
    for (int i = 0; i < 8; i++) r->bb_hole[i] = -1;
}

/* Apply the opponent's choice after a snooker foul that offered one (decision
 * was parked at CUE_DEC_PENDING). On CUE_DEC_REPLAY the host must have restored
 * the pre-shot ball layout + target/seq/reds_left from its own snapshot first;
 * the penalty already stands. Returns the next player to shoot. */
int cue_rules_apply_decision(CueRules *r, int decision) {
    int off = r->dec_offender, opp = 1 - off;
    int can_restore = r->dec_can_restore, free_ball = r->dec_free_ball;
    r->decision = CUE_DEC_NONE;
    r->dec_can_restore = r->dec_free_ball = 0;
    if (decision == CUE_DEC_REPLAY && can_restore) {
        r->turn = off;                        /* offender plays again from restored layout */
        r->ball_in_hand = 0; r->free_ball = 0;
    } else if (decision == CUE_DEC_AGAIN) {
        /* Play again from HERE. The commonest thing a player actually does with
         * a foul — hand it straight back on the mess the offender has left —
         * and it had no code at all: the only way to make them play again was
         * REPLAY, which also puts every ball back where it was and so gives
         * them the position they fouled out of. */
        r->turn = off;
        r->ball_in_hand = 0; r->free_ball = 0;
    } else {
        r->turn = opp;
        r->ball_in_hand = r->dec_scratch ? 1 : 0;
        r->free_ball = (decision == CUE_DEC_FREEBALL && free_ball) ? 1 : 0;
        r->free_ball_id = 0;      /* theirs to name */
    }
    return r->turn;
}

int cue_rules_ball_legal(const CueRules *r, const CueBall *b, int n, int id) {
    /* The generic guard cannot run first in billiards: the OBJECT WHITE wears
     * CUE_ID_CUE, and the guard was answering for it before the billiards rule
     * below ever saw the question. */
    if (r->mode == CUE_GAME_BILLIARDS)
        return id == CUE_ID_BIL_RED ||
               id == (r->bil_yellow ? CUE_ID_BIL_WHITE : CUE_ID_BIL_YELLOW);
    /* CAROM answers before the guard for billiards' own reason: when the
     * yellow is in, the object WHITE wears id zero. Four-ball's objects are
     * the two reds alone. */
    if (CUE_GAME_IS_CAROM(r->mode)) {
        if (r->mode == CUE_GAME_CAROM_4B) return id == CUE_ID_BIL_RED || id == 2;
        return id == CUE_ID_BIL_RED ||
               id == (r->bil_yellow ? CUE_ID_BIL_WHITE : CUE_ID_BIL_YELLOW);
    }
    if (id == CUE_ID_CUE) return 0;
    /* Free ball: the NOMINATED one, once named — a free ball is nominated in
     * snooker exactly as a colour is, and "any ball is on" was the striker
     * getting a choice they never had to declare. */
    if (r->kind) {
        if (r->free_ball)
            return !r->free_ball_id || id == r->free_ball_id;
        return snk_on(r, id);
    }
    if (CUE_GAME_IS_ROTATION(r->mode)) return id == rot_lowest(r, b, n);  /* lowest first */
    /* Straight pool: every object ball is legal to hit, always. The obligation
     * is to SAY which one, not to choose from a list — see cue_rules_call_shot. */
    /* FIFTEEN-BALL likewise, and there is not even a call: any ball, any
     * pocket, and its number is the score. */
    /* Cowboy: the three balls on the table are all of them and all legal. */
    if (r->mode == CUE_GAME_COWBOY) return id == 1 || id == 3 || id == 5;
    /* BOWLLIARDS: the rack is the 1 to the 10 and every one of them is on, in
     * any order — the obligation is to SAY which, not to be told. Stopping at
     * ten rather than fifteen matters more here than it looks: this function is
     * how the planner learns what it may shoot at, and the five balls that are
     * not in the game would otherwise be offered to it as targets that are not
     * on the table. */
    if (r->mode == CUE_GAME_BOWLLIARDS) return id >= 1 && id <= 10;
    /* CRIBBAGE POOL: WHAT IS ON DEPENDS ENTIRELY ON WHAT YOU HAVE ALREADY DONE.
     *
     * This is how the planner learns the game, and it is the whole of what it
     * is told. Having potted the 4 there is exactly one ball worth shooting at
     * and it is the 11 — leave every ball legal and the machine plays a
     * pleasant frame of pot-what-you-can, fouls on the second stroke of every
     * pair it opens, and never scores. So the debt is the answer: on a cribbage
     * only the companions owed are on, and with several owed the striker may
     * pick any of them, which is the rule as written rather than a convenience.
     *
     * Off a cribbage the 15 is the one ball NOT on. It cannot start a pair, so
     * a pot of it is a ball spotted and an inning thrown away — until it is the
     * only ball left, and then it is the only ball on. */
    if (r->mode == CUE_GAME_CRIBBAGE) {
        if (id < 1 || id > 15) return 0;
        if (r->cr_nowed) {
            for (int i = 0; i < (int)r->cr_nowed && i < 8; i++)
                if (r->cr_owed[i] == id) return 1;
            return 0;
        }
        if (id != 15) return 1;
        for (int i = 1; i < n; i++)
            if (b[i].on && b[i].id >= 1 && b[i].id <= 14) return 0;
        return 1;                       /* the deciding cribbage, on its own */
    }
    if (r->mode == CUE_GAME_STRAIGHT ||
        r->mode == CUE_GAME_FIFTEEN ||
        r->mode == CUE_GAME_HONOLULU ||
        r->mode == CUE_GAME_SPEED ||
        r->mode == CUE_GAME_ONEPOCKET ||
        r->mode == CUE_GAME_BANKPOOL) return id >= 1 && id <= 15;
    /* Pyramid: every one of the fifteen is on, always. */
    /* Billiards has no ball ON: Rule 16 makes it a miss only if the cue ball
     * fails to contact EITHER object ball, so both are legal to strike. */
    /* Bar billiards: every ball on the table is an object ball, and the one in
     * your hand is whichever white you picked up. */
    if (r->mode == CUE_GAME_BARBILLIARDS) return id != CUE_ID_CUE;
    /* PAUL: everything on the table is fair game and always was. No ball on, no
     * order, no nomination — you pot what you can reach. */
    if (r->mode == CUE_GAME_PAUL) return id != CUE_ID_CUE;
    /* GOLF: clear the reds. There is no order and no nominated ball — the only
     * thing you may not strike first is your own cue ball. */
    if (r->mode == CUE_GAME_GOLF) return id != CUE_ID_CUE;
    /* KILLER: pot what you like — any object ball, always. */
    if (CUE_GAME_IS_KILLER(r->mode)) return id != CUE_ID_CUE;
    if (CUE_GAME_IS_PYRAMID(r->mode))  return id >= 1 && id <= 15;
    if (r->open) return id != 8;                 /* open table: anything but the 8 */
    /* the 8 is legal ONLY once your own group is fully cleared */
    if (id == 8) return group_cleared(b, n, r->group[r->turn]);
    return pool_group(id) == r->group[r->turn];
}

void cue_rules_status(const CueRules *r, char *buf, int cap) {
    if (r->kind) {
        static const char *CN[8] = { "", "", "YELLOW", "GREEN", "BROWN",
                                     "BLUE", "PINK", "BLACK" };
        /* On a colour, say WHICH — once it has been nominated. "ON COLOUR" was
         * true of the rules as they were, when any colour would do; now that a
         * nomination binds, the board has to carry it. */
        const char *on = r->target == 0 ? "RED"
                       : r->target == 1 ? (r->nominated ? CN[r->nominated] : "COLOUR")
                       : CN[r->seq < 2 ? 2 : (r->seq > 7 ? 7 : r->seq)];
        snprintf(buf, cap, "ON %s", on);
    } else if (r->mode == CUE_GAME_PAUL) {
        /* THERE IS NO BALL ON, so the only thing worth a status line is what is
         * still to play for — which is also the only thing that ends the frame.
         * "22 LEFT" beside a four-point lead tells a player exactly how much
         * work is left, which is the whole tactical question in this game. */
        /* AND WHETHER TWO SHOTS ARE OWED, which the pool board says the same way
         * and which matters more here than it does there: with no penalty for
         * missing, the free shot is the only thing a foul actually buys. */
        if (r->shots_remaining > 1)
            snprintf(buf, cap, "%d LEFT  2 SHOTS", r->paul_left);
        else
            snprintf(buf, cap, "%d ON THE TABLE", r->paul_left);
    } else if (r->mode == CUE_GAME_SPEED) {
        if (r->sp_done[0] || r->sp_done[1]) {
            const int d = r->sp_done[0] ? 0 : 1;
            snprintf(buf, cap, "THEY DID %d.%02d", r->sp_cs[d] / 100,
                     r->sp_cs[d] % 100);
        } else snprintf(buf, cap, "CLEAR THE TABLE");
    } else if (r->mode == CUE_GAME_COWBOY) {
        /* The phase is the game, so the board says which one you are in. */
        const int have = r->score[r->turn];
        if (have >= 100)
            snprintf(buf, cap, "%d - %d   CANNON OFF THE 1", have,
                     r->score[1 - r->turn]);
        else if (have >= 90)
            snprintf(buf, cap, "%d - %d   CANNONS ONLY", have,
                     r->score[1 - r->turn]);
        else
            snprintf(buf, cap, "%d - %d  TO 101", have, r->score[1 - r->turn]);
    } else if (r->mode == CUE_GAME_FIFTEEN) {
        /* No ball on to report — that is the game — so the board is the race. */
        snprintf(buf, cap, "%d - %d  TO %d", r->score[r->turn],
                 r->score[1 - r->turn], r->target_score);
    } else if (CUE_GAME_IS_ROT61(r->mode)) {
        /* "ON 7" alone would leave out the only number that matters: rotation
         * is a race and the ball on is just the toll to be paid on the way. */
        snprintf(buf, cap, "ON %d   %d - %d  TO %d", r->seq ? r->seq : 1,
                 r->score[r->turn], r->score[1 - r->turn], r->target_score);
    } else if (CUE_GAME_IS_ROTATION(r->mode)) {
        snprintf(buf, cap, "ON %d", r->seq ? r->seq : 1);
    } else if (CUE_GAME_IS_CAROM(r->mode)) {
        snprintf(buf, cap, "%d - %d  TO %d", r->score[r->turn],
                 r->score[1 - r->turn], r->target_score);
    } else if (CUE_GAME_IS_KILLER(r->mode)) {
        /* the board is the lives — yours first, because it is your shot */
        snprintf(buf, cap, "LIVES %d - %d", r->score[r->turn],
                 r->score[1 - r->turn]);
    } else if (r->mode == CUE_GAME_STRAIGHT) {
        /* The score IS the state in 14.1 — there is no ball on to report, so the
         * board carries the target and what has been called instead. */
        if (r->nominated)
            snprintf(buf, cap, "%d/%d  CALL %d", r->score[r->turn],
                     r->target_score, r->nominated);
        else
            snprintf(buf, cap, "%d/%d  SAFETY", r->score[r->turn], r->target_score);
    } else if (r->mode == CUE_GAME_BOWLLIARDS) {
        /* WHICH FRAME, WHICH INNING, AND THE TWO CARDS. There is no ball on to
         * report — any of the ten will do — and the only thing a striker
         * decides on is how much of the frame he has left, which is the inning
         * he is in. The tenth's third delivery is a bonus and says so.
         *
         * The card total lags the pins on purpose: a frame waiting on a
         * strike's bonus has no score yet, exactly as the blank box on a
         * bowling sheet has none. */
        const int fr = r->bw_frame[r->turn];
        const char *inn = r->bw_inning >= 3 ? "BONUS"
                        : r->bw_inning == 2 ? "2ND" : "1ST";
        if (fr >= 10)
            snprintf(buf, cap, "SUDDEN DEATH %s  %d - %d", inn,
                     r->score[r->turn], r->score[1 - r->turn]);
        else
            snprintf(buf, cap, "FRAME %d %s   %d - %d", fr + 1, inn,
                     r->score[r->turn], r->score[1 - r->turn]);
    } else if (r->mode == CUE_GAME_CRIBBAGE) {
        /* WHAT YOU ARE ON, because in this game that is the next stroke
         * entirely: there is no choice about it and no safety to play instead,
         * so a board that showed only the score would be leaving out the one
         * thing the striker has to know. The race is carried with it — five
         * cribbages, and the number is small enough that the gap between the
         * two is the state of the game. */
        const int me = r->score[r->turn], them = r->score[1 - r->turn];
        if (r->cr_nowed == 1)
            snprintf(buf, cap, "%d - %d   ON THE %d", me, them,
                     (int)r->cr_owed[0]);
        else if (r->cr_nowed > 1)
            snprintf(buf, cap, "%d - %d   %d TO PAIR", me, them,
                     (int)r->cr_nowed);
        else
            snprintf(buf, cap, "%d - %d  TO %d", me, them, r->target_score);
    } else if (r->mode == CUE_GAME_GOLF) {
        /* WHAT IS STILL BEING PLAYED FOR ON THIS HOLE.
         *
         * Which hole it is, its par, and where the round stands are all things
         * a board can show standing still, so a one-line status that repeats
         * them says nothing. What changes with every stroke — and what a
         * player actually decides the next shot on — is what this stroke is
         * worth. Said the way a golfer says it: "3 FOR PAR", then "THIS FOR
         * PAR", then "THIS FOR BOGEY". */
        const int par = CUE_GOLF_COURSE[r->golf_hole].par;
        const int spare = par - r->golf_strokes;   /* strokes left for par */
        if (spare > 1)       snprintf(buf, cap, "HOLE %d   %d FOR PAR",
                                      r->golf_hole + 1, spare);
        else if (spare == 1) snprintf(buf, cap, "HOLE %d   THIS FOR PAR",
                                      r->golf_hole + 1);
        else if (spare == 0) snprintf(buf, cap, "HOLE %d   THIS FOR BOGEY",
                                      r->golf_hole + 1);
        else if (spare == -1) snprintf(buf, cap, "HOLE %d   THIS FOR DOUBLE",
                                      r->golf_hole + 1);
        else                 snprintf(buf, cap, "HOLE %d   %d OVER",
                                      r->golf_hole + 1, -spare);
    } else if (r->mode == CUE_GAME_BARBILLIARDS) {
        /* The break is the number that matters at a bar billiards table: it is
         * what you are about to lose. */
        int m = (int)(r->bb_time / 60.0f), sec = (int)r->bb_time % 60;
        snprintf(buf, cap, "%d - %d   BREAK %d   %d:%02d%s",
                 r->score[0], r->score[1], r->bb_break, m, sec,
                 r->bb_barred ? "  BAR DOWN" : "");
    } else if (r->mode == CUE_GAME_BILLIARDS) {
        /* Points, a target, and which ball the striker is on — the last of
         * which is half of knowing whose turn it is at a billiards table.
         *
         * PLAYED TO A CLOCK, the board says so instead of naming a target that
         * does not exist (Rule 1(f) — a game is to a time OR to a number of
         * points). What is left of it is the only number that matters then. */
        if (r->bil_time_len > 0.0f) {
            const int sec = (int)(r->bil_time + 0.999f);
            snprintf(buf, cap, "%d - %d   %d:%02d   %s", r->score[0], r->score[1],
                     sec / 60, sec % 60, r->bil_yellow ? "YELLOW" : "WHITE");
        } else
        snprintf(buf, cap, "%d - %d   (%d)   %s", r->score[0], r->score[1],
                 r->target_score, r->bil_yellow ? "YELLOW" : "WHITE");
    } else if (CUE_GAME_IS_PYRAMID(r->mode)) {
        /* Balls, not points, and eight of them takes it. */
        snprintf(buf, cap, "%d - %d   (8 WINS)", r->score[0], r->score[1]);
    } else if (r->mode == CUE_GAME_ONEPOCKET || r->mode == CUE_GAME_BANKPOOL ||
               r->mode == CUE_GAME_HONOLULU) {
        /* EIGHT OF THE FIFTEEN, AND NO GROUPS AT ALL.
         *
         * These three fell through to the eight-ball line below, which says
         * OPEN until a group has been decided — and one never is here, because
         * there are none. So the board read OPEN for the whole frame and the
         * score of a game that is a race to eight appeared nowhere on it.
         * Reported at Honolulu.
         *
         * One pocket carries what a foul left owed as well: until it is paid
         * the next ball you pot is not a point, and that is not visible from
         * the score. */
        const int me = r->score[r->turn], them = r->score[1 - r->turn];
        if (r->mode == CUE_GAME_ONEPOCKET && r->op_owed[r->turn] > 0)
            snprintf(buf, cap, "%d - %d  TO %d   OWES %d", me, them,
                     r->target_score, r->op_owed[r->turn]);
        else
            snprintf(buf, cap, "%d - %d  TO %d", me, them, r->target_score);
    } else {
        int g = r->group[r->turn];
        const char *grp = r->open ? "OPEN" : g == 1 ? "SOLIDS" : "STRIPES";
        if (r->shots_remaining > 1) snprintf(buf, cap, "%s  2 SHOTS", grp);
        else                        snprintf(buf, cap, "%s", grp);
    }
}
