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
static void resolve_pool(CueRules *r, CueBall *b, int n, int first_hit,
                         int scratch, int cushion, const int *potted, int np) {
    int grp = r->group[r->turn];
    int low = 0, high = 0, eight = 0;
    for (int k = 0; k < np; k++) {
        int g = pool_group(potted[k]);
        if (potted[k] == 8) eight = 1; else if (g == 1) low++; else if (g == 2) high++;
    }
    int my_potted = (grp == 1) ? low : high;   /* own group balls potted THIS shot */
    int legal_pot = r->open ? (low || high) : my_potted;
    /* "on the 8" only if the group was cleared BEFORE this shot — i.e. it's
     * empty now AND you didn't just pot a group ball this shot. Otherwise the
     * shot that pots your last group ball would wrongly read as must-hit-8. */
    int on_eight = !r->open && group_cleared(b, n, grp) && my_potted == 0;

    int foul = 0; const char *why = "";
    if (scratch)            { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0) { foul = 1; why = "NO BALL"; }
    else {
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
        (r->mode != CUE_GAME_UK8 || r->uk_intl)) { foul = 1; why = "NO RAIL"; }
    /* OFF THE TABLE. Last, so it names the foul when nothing worse did: a ball
     * driven off is a foul however good the contact was, and jumping is legal
     * here, so this is the only thing a clean jump shot can go wrong by. */
    if (r->n_off && !foul) { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    /* the 8 */
    if (eight) {
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
            /* legal win only if the group was clear BEFORE potting the 8 */
            int win = !foul && !scratch && on_eight;
            r->frame_over = 1; r->winner = win ? r->turn : (1 - r->turn);
            book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg, win ? "FRAME WON!" : "FOUL ON 8");
            return;
        }
    }

    if (r->open && !foul && !r->break_shot && (low || high)) {  /* assign */
        int g = (low && !high) ? 1 : (high && !low) ? 2 : pool_group(first_hit);
        if (g == 1 || g == 2) { r->group[r->turn] = g; r->group[1-r->turn] = (g==1)?2:1; r->open = 0; }
    }

    if (foul) {
        if (r->mode != CUE_GAME_UK8 || r->uk_intl) {
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
    int first = (bf + f0 + f1) & 1;
    cue_rules_init(r, t, cpu);
    if (tgt > 0) r->target_score = tgt;
    r->frames[0] = f0; r->frames[1] = f1; r->best_of = bo;
    r->match_over = mo; r->match_winner = mw;
    r->break_first = bf;
    r->turn = first;
}

void cue_rules_set_uk(CueRules *r, int ruleset) {
    if (!r) return;
    if (ruleset < CUE_UK_PUB || ruleset > CUE_UK_ULTIMATE) ruleset = CUE_UK_PUB;
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

static void resolve_snooker(CueRules *r, CueBall *b, int n, const CueWorld *w,
                            int first_hit,
                            int scratch, const int *potted, int np) {
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
        int miss_called = is_miss && !needs_snookers;
        r->last_miss = miss_called;

        /* 3-consecutive-miss forfeit (genuine, non-snookered misses only) */
        if (is_miss && !r->was_snookered) {
            if (++r->cmiss[off] >= 3) {
                r->frame_over = 1; r->winner = opp;
                book_frame(r, r->winner);
                snprintf(r->msg, sizeof r->msg, "3 MISSES - LOSS");
                return;
            }
        } else if (!is_miss) r->cmiss[off] = 0;

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
        if (miss_called || opp_snk) {
            /* a real choice exists → park for the opponent's decision */
            r->decision = CUE_DEC_PENDING;
            snprintf(r->msg, sizeof r->msg, miss_called ? "%sFOUL & MISS +%d"
                                                        : "%sFOUL +%d", why, fv);
        } else {
            r->turn = opp;
            if (scratch) r->ball_in_hand = 1;
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

static void resolve_9ball(CueRules *r, CueBall *b, int n, int first_hit,
                          int scratch, int cushion, const int *potted, int np) {
    int was_break = r->break_shot;
    const int money = rot_money(r);

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
    if (scratch)                      { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0)           { foul = 1; why = "NO BALL"; }
    else if (first_hit != lowest)     { foul = 1; why = "WRONG BALL"; }   /* must hit lowest first */
    else if (np == 0 && !cushion)     { foul = 1; why = "NO RAIL"; }      /* table scratch */
    if (r->n_off && !foul)            { foul = 1; why = "OFF THE TABLE"; }
    r->last_foul = foul;

    /* the 9: potted legally wins (incl. on the break); on a foul it respots */
    if (nine_potted) {
        if (!foul) { r->frame_over = 1; r->winner = r->turn; book_frame(r, r->winner);
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
    if (np > 0) r->msg[0] = 0;                   /* potted legally → carry on */
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
            snprintf(r->msg, sizeof r->msg, "LEVEL - THE BLACK GOES BACK UP");
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

    /* WHAT THE CUE BALL TOUCHED, and in what order. Only the two object balls
     * matter; a cushion between them changes nothing in billiards (it is
     * three-cushion that cares, and that is not this game). */
    int hit_red = 0, hit_white = 0, first = 0;   /* first: the id struck first */
    if (w) {
        for (int i = 0; i < w->ntouch; i++) {
            if (w->touch[i].what != CUE_TOUCH_BALL) continue;
            int id = w->touch[i].id;
            if (id == CUE_ID_BIL_RED) { if (!first) first = id; hit_red = 1; }
            else                      { if (!first) first = id; hit_white = 1; }
        }
    }
    /* A world that kept no account still has first_hit, which is enough for
     * everything but the cannon — better a game that scores the hazards than
     * one that refuses to run. */
    if (!first && first_hit >= 0) {
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
    const int in_off = scratch && first;
    const int cannon = hit_red && hit_white;

    /* ---- the fouls ---- */
    int foul = 0; const char *why = "";
    if (!first)                       { foul = 1; why = scratch ? "COUP" : "MISS"; }
    else if (scratch && !first)       { foul = 1; why = "COUP"; }
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
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
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
    if (!cleared && !maxed) {
        /* Still on the hole. The score IS the stroke count, so that is the
         * only thing worth saying. */
        snprintf(r->msg, sizeof r->msg, "%d", r->golf_strokes);
        return;
    }

    /* The hole is done, one way or the other. Rule 3 caps it at eight. */
    int score = r->golf_strokes;
    if (score > CUE_GOLF_MAX_STROKES) score = CUE_GOLF_MAX_STROKES;
    r->golf_card[me][r->golf_hole] = (uint8_t)score;
    r->golf_strokes = 0;

    const int par = CUE_GOLF_COURSE[r->golf_hole].par;
    if (!cleared)              snprintf(r->msg, sizeof r->msg, "%d - LIMIT", score);
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
    }
    const int first = cue_golf_first(r->golf_round);
    const int last  = cue_golf_last(r->golf_round);
    if (r->golf_hole >= last) {
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

int cue_rules_golf_total(const CueRules *r, int who, int from_hole, int to_hole) {
    if (!r || who < 0 || who > 1) return 0;
    int t = 0;
    for (int h = from_hole; h <= to_hole && h < CUE_GOLF_HOLES; h++)
        if (h >= 0) t += r->golf_card[who][h];
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

void cue_rules_billiards_swap(CueBall *b, int n) {
    if (!b || n < 3) return;
    int other = -1;
    for (int i = 1; i < n; i++)
        if (b[i].id == CUE_ID_BIL_WHITE || b[i].id == CUE_ID_BIL_YELLOW) { other = i; break; }
    if (other < 0) return;
    CueBall tmp = b[0]; b[0] = b[other]; b[other] = tmp;
}

static void resolve_straight(CueRules *r, CueBall *b, int n, int first_hit,
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
    if (scratch)                        { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0)             { foul = 1; why = was_break ? "BREAK" : "NO BALL"; }
    else if (np == 0 && !cushion)       { foul = 1; why = was_break ? "BREAK" : "NO RAIL"; }
    if (r->n_off && !foul)              { foul = 1; why = "OFF THE TABLE"; }
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
    if (!r || r->mode != CUE_GAME_STRAIGHT) return;
    r->nominated = (ball_id >= 1 && ball_id <= 15) ? ball_id : 0;
    r->called_pocket = r->nominated ? pocket : -1;
}

void cue_rules_set_target(CueRules *r, int points) {
    if (!r || points < 1) return;
    r->target_score = points;
}

void cue_rules_resolve(CueRules *r, CueBall *b, int n, const CueWorld *w,
                       int first_hit, int scratch, int cushion,
                       const int *potted, int np) {
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

    if (r->kind)                            resolve_snooker(r, b, n, w, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_PAUL)      resolve_paul(r, b, n, first_hit, scratch, cushion, potted, np);
    else if (CUE_GAME_IS_ROTATION(r->mode)) resolve_9ball(r, b, n, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_STRAIGHT)  resolve_straight(r, b, n, first_hit, scratch, cushion, potted, np);
    else if (CUE_GAME_IS_PYRAMID(r->mode))   resolve_pyramid(r, b, n, first_hit, scratch, cushion, potted, np);
    else if (r->mode == CUE_GAME_BILLIARDS)  resolve_billiards(r, b, n, w, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_BARBILLIARDS) resolve_barbilliards(r, b, n, w, first_hit, potted, np);
    else if (r->mode == CUE_GAME_GOLF) resolve_golf(r, b, n, scratch);
    else                                    resolve_pool(r, b, n, first_hit, scratch, cushion, potted, np);
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
    if (r->mode == CUE_GAME_STRAIGHT) return id >= 1 && id <= 15;
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
    if (r->mode == CUE_GAME_BILLIARDS) return id != CUE_ID_CUE;
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
    } else if (CUE_GAME_IS_ROTATION(r->mode)) {
        snprintf(buf, cap, "ON %d", r->seq ? r->seq : 1);
    } else if (r->mode == CUE_GAME_STRAIGHT) {
        /* The score IS the state in 14.1 — there is no ball on to report, so the
         * board carries the target and what has been called instead. */
        if (r->nominated)
            snprintf(buf, cap, "%d/%d  CALL %d", r->score[r->turn],
                     r->target_score, r->nominated);
        else
            snprintf(buf, cap, "%d/%d  SAFETY", r->score[r->turn], r->target_score);
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
         * which is half of knowing whose turn it is at a billiards table. */
        snprintf(buf, cap, "%d - %d   (%d)   %s", r->score[0], r->score[1],
                 r->target_score, r->bil_yellow ? "YELLOW" : "WHITE");
    } else if (CUE_GAME_IS_PYRAMID(r->mode)) {
        /* Balls, not points, and eight of them takes it. */
        snprintf(buf, cap, "%d - %d   (8 WINS)", r->score[0], r->score[1]);
    } else {
        int g = r->group[r->turn];
        const char *grp = r->open ? "OPEN" : g == 1 ? "SOLIDS" : "STRIPES";
        if (r->shots_remaining > 1) snprintf(buf, cap, "%s  2 SHOTS", grp);
        else                        snprintf(buf, cap, "%s", grp);
    }
}
