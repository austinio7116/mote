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
    r->kind = t->is_snooker;
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
    if (r->kind) {
        r->target = 0; r->reds_left = t->reds ? t->reds : 15;
        /* colour spots by value 2..7 */
        /* SWAPPED. Standing behind the D looking up the table, the order
         * reading left to right is green, brown, yellow — "God Bless You".
         * These were the other way about: the comments said right and left and
         * the coordinates said the opposite, and the render agreed with the
         * coordinates. +Z is the player's RIGHT from baulk (facing +X with +Y
         * up), so yellow is +d_radius. Checked against a render from behind
         * the baulk cushion rather than against the handedness argument, which
         * is exactly the sort of reasoning that put them here. */
        r->spot[2] = v3(t->baulk_x, t->R, +t->d_radius);   /* yellow — right of the D */
        r->spot[3] = v3(t->baulk_x, t->R, -t->d_radius);   /* green  — left of the D  */
        r->spot[4] = v3(t->baulk_x, t->R, 0.0f);           /* brown  */
        r->spot[5] = v3(t->blue_x,  t->R, 0.0f);           /* blue   */
        r->spot[6] = v3(t->pink_x,  t->R, 0.0f);           /* pink   */
        r->spot[7] = v3(t->black_x, t->R, 0.0f);           /* black  */
    } else {
        /* foot spot — respot for the 9 (US9) or an illegally broken-in 8 */
        r->spot[0] = v3(t->half_len * 0.5f, t->R, 0.0f);
        if (t->kind == CUE_GAME_US9) r->seq = 1;           /* lowest ball on (HUD) */
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
            /* Up the centre line from its own spot, toward the top cushion. */
            p = r->spot[v];
            for (int step = 1; step <= 60; step++) {
                Vec3 t = p; t.x += (float)step * r->R * 0.5f;
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
    int first = (bf + f0 + f1) & 1;
    cue_rules_init(r, t, cpu);
    r->frames[0] = f0; r->frames[1] = f1; r->best_of = bo;
    r->match_over = mo; r->match_winner = mw;
    r->break_first = bf;
    r->turn = first;
}

void cue_rules_set_uk(CueRules *r, int international) {
    if (r) r->uk_intl = international ? 1 : 0;
}

void cue_rules_respot(CueRules *r, CueBall *b, int n, int id) {
    if (!r->kind || !is_colour(id)) return;
    respot_colour(r, b, n, id);
}

void cue_rules_set_break(CueRules *r, int who) {
    r->break_first = who ? 1 : 0;
    r->turn = r->break_first;
}

void cue_rules_nominate(CueRules *r, int value) {
    if (!r->kind || r->target != 1) return;
    r->nominated = (value >= 2 && value <= 7) ? value : 0;
}

void cue_rules_nominate_free(CueRules *r, int id) {
    if (!r->kind || !r->free_ball) return;
    r->free_ball_id = id;
}

/* Full-ball line of sight from `from` to a target ball at `to` (XZ plane): both
 * extreme edges of the target must be reachable without a blocker in the way.
 * Ported from 2dpool hasClearPath(). rad = ball radius (all equal in snooker). */
static int clear_path(Vec3 from, Vec3 to, float rad,
                      const CueBall *b, int n, int target_idx) {
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
    }
    return 1;
}

int cue_rules_is_snookered(const CueRules *r, const CueBall *b, int n) {
    if (!r->kind || !b[0].on) return 0;       /* snooker only; cue must be on */
    int any_target = 0;
    for (int i = 1; i < n; i++) {
        if (!b[i].on || !snk_on(r, b[i].id)) continue;
        any_target = 1;
        if (clear_path(b[0].pos, b[i].pos, r->R, b, n, i)) return 0;  /* one is visible */
    }
    return any_target;                        /* all targets blocked → snookered */
}

/* Would the incoming player be snookered wherever in the D they put the ball?
 * Sampled rather than solved: the D is a half-disc and the answer only has to be
 * as good as a referee's eye. A ring of positions round the arc plus the middle
 * covers it — if any one of them can see a ball on, there is no free ball. */
static int snookered_from_whole_d(CueRules *r, CueBall *b, int n) {
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
            if (!cue_rules_is_snookered(r, b, n)) snookered = 0;
        }
    }
    if (snookered) {                       /* and the centre of the D */
        b[0].pos = v3(r->baulk_x, keep.y, 0.0f);
        if (!cue_rules_is_snookered(r, b, n)) snookered = 0;
    }
    b[0].pos = keep; b[0].on = keep_on;
    return snookered;
}

static void resolve_snooker(CueRules *r, CueBall *b, int n, int first_hit,
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
            opp_snk = scratch ? snookered_from_whole_d(r, b, n)
                              : cue_rules_is_snookered(r, b, n);
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

/* ---- US 9-ball ------------------------------------------------------- */
static int nine_lowest(const CueBall *b, int n) {     /* lowest 1..9 on table */
    int lo = 99;
    for (int i = 0; i < n; i++)
        if (b[i].on && b[i].id >= 1 && b[i].id <= 9 && b[i].id < lo) lo = b[i].id;
    return lo == 99 ? 0 : lo;
}
static void respot_nine(CueRules *r, CueBall *b, int n) {
    CueBall *q = find_ball(b, n, 9);
    if (!q) return;
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0);
    q->pos = r->spot[0]; q->orient = m3_identity();
}

static void resolve_9ball(CueRules *r, CueBall *b, int n, int first_hit,
                          int scratch, int cushion, const int *potted, int np) {
    int was_break = r->break_shot;

    /* Push-out (WPA): the shot carries no obligation to hit the lowest ball or
     * drive a ball to a rail — the ONLY foul is pocketing the cue ball. A potted
     * 9 is spotted (no win). The opponent then chooses to play from here or
     * pass the shot back. */
    if (r->is_pushout) {
        r->is_pushout = 0; r->pushout_avail = 0; r->break_shot = 0;
        for (int k = 0; k < np; k++) if (potted[k] == 9) respot_nine(r, b, n);
        r->seq = nine_lowest(b, n);
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
    int lowest = nine_lowest(b, n);
    int nine_potted = 0;
    for (int k = 0; k < np; k++) {
        if (potted[k] == 9) nine_potted = 1;
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
                     snprintf(r->msg, sizeof r->msg, "9-BALL!"); return; }
        respot_nine(r, b, n);
    }

    if (foul) {
        r->cfoul[r->turn]++;
        if (r->cfoul[r->turn] >= 3) {           /* three consecutive fouls = loss */
            r->frame_over = 1; r->winner = 1 - r->turn; book_frame(r, r->winner);
            snprintf(r->msg, sizeof r->msg, "3 FOULS - LOSS"); return;
        }
        r->turn = 1 - r->turn; r->ball_in_hand = 1;
        snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        r->break_shot = 0; r->seq = nine_lowest(b, n); return;
    }
    r->cfoul[r->turn] = 0;
    if (np > 0) r->msg[0] = 0;                   /* potted legally → carry on */
    else { r->turn = 1 - r->turn; r->msg[0] = 0; }
    r->break_shot = 0; r->seq = nine_lowest(b, n);

    /* After the opening break, the player now at the table may push out. */
    if (was_break && !r->frame_over) { r->pushout_avail = 1; r->pushout_offer = 1; }
}

void cue_rules_resolve(CueRules *r, CueBall *b, int n, const CueWorld *w,
                       int first_hit, int scratch, int cushion,
                       const int *potted, int np) {
    (void)w;
    r->ball_in_hand = 0;
    r->last_foul = 0;
    r->last_miss = 0;
    r->last_foul_pts = 0;
    if (r->kind)                       resolve_snooker(r, b, n, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_US9)  resolve_9ball(r, b, n, first_hit, scratch, cushion, potted, np);
    else                               resolve_pool(r, b, n, first_hit, scratch, cushion, potted, np);
    /* The host's observations are about the shot just resolved and nothing
     * else. Left set they would foul the NEXT one too. */
    r->jumped = 0;
    r->n_off = 0;
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
    if (r->mode == CUE_GAME_US9) return id == nine_lowest(b, n);  /* must hit lowest */
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
    } else if (r->mode == CUE_GAME_US9) {
        snprintf(buf, cap, "ON %d", r->seq ? r->seq : 1);
    } else {
        int g = r->group[r->turn];
        const char *grp = r->open ? "OPEN" : g == 1 ? "SOLIDS" : "STRIPES";
        if (r->shots_remaining > 1) snprintf(buf, cap, "%s  2 SHOTS", grp);
        else                        snprintf(buf, cap, "%s", grp);
    }
}
