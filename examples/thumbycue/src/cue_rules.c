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
    if (r->kind) {
        r->target = 0; r->reds_left = t->reds ? t->reds : 15;
        /* colour spots by value 2..7 */
        r->spot[2] = v3(t->baulk_x, t->R, -t->d_radius);   /* yellow = right of the D */
        r->spot[3] = v3(t->baulk_x, t->R, +t->d_radius);   /* green  = left of the D */
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
static void respot_colour(CueRules *r, CueBall *b, int n, int id) {
    CueBall *q = find_ball(b, n, id);
    if (!q) return;
    int v = snk_value(id);
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0);
    q->pos = r->spot[v];           /* (occupancy not checked — good enough) */
    q->orient = m3_identity();
}

/* ---- 8-ball ---------------------------------------------------------- */
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
    (void)cushion;

    /* the 8 */
    if (eight) {
        if (r->break_shot) {                       /* re-spot, no result */
            respot_eight(r, b, n);
        } else {
            /* legal win only if the group was clear BEFORE potting the 8 */
            int win = !foul && !scratch && on_eight;
            r->frame_over = 1; r->winner = win ? r->turn : (1 - r->turn);
            snprintf(r->msg, sizeof r->msg, win ? "FRAME WON!" : "FOUL ON 8");
            return;
        }
    }

    if (r->open && !foul && !r->break_shot && (low || high)) {  /* assign */
        int g = (low && !high) ? 1 : (high && !low) ? 2 : pool_group(first_hit);
        if (g == 1 || g == 2) { r->group[r->turn] = g; r->group[1-r->turn] = (g==1)?2:1; r->open = 0; }
    }

    if (foul) {
        if (r->mode != CUE_GAME_UK8) {
            /* US / Chinese 8-ball (WPA): any foul → opponent ball-in-hand. */
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
    if (r->target == 1) return is_colour(id);
    return is_colour(id) && snk_value(id) == r->seq;   /* clearance */
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
    int bon_val = (target_before == 2) ? r->seq : 1;   /* value of the red/clearance ball-on */
    int legal_pots = 0, illegal_pot = 0, maxpot = 0, reds_potted = 0;
    for (int k = 0; k < np; k++) {
        int on = snk_on(r, potted[k]);
        if (on)       legal_pots += snk_value(potted[k]);
        else if (fb)  legal_pots += bon_val;           /* free-ball pot scores the ball-on */
        else          illegal_pot = 1;
        if (on ? is_red(potted[k]) : (fb && target_before == 0)) reds_potted++;
        if (snk_value(potted[k]) > maxpot) maxpot = snk_value(potted[k]);
    }
    int foul = 0;
    if (scratch || first_hit < 0 || (!fb && !snk_on(r, first_hit)) || illegal_pot) foul = 1;

    /* respot every potted colour unless it was legally cleared in sequence
     * (a free-ball colour ALWAYS respots, even in the clearance phase) */
    for (int k = 0; k < np; k++)
        if (is_colour(potted[k]) && (foul || fb || target_before != 2))
            respot_colour(r, b, n, potted[k]);

    if (foul) {
        int off = r->turn, opp = 1 - off;
        /* "Miss" = failed to HIT a ball-on (air shot or wrong first ball); an
         * illegal pot off a correct first contact is a foul but NOT a miss.
         * Evaluated against the pre-shot target (r->target still == target_before). */
        int is_miss = (first_hit < 0) || (!fb && !snk_on(r, first_hit));

        int fv = 4;
        int tv = (target_before == 2) ? r->seq : 1;
        if (tv > fv) fv = tv;
        if (first_hit >= 0 && snk_value(first_hit) > fv) fv = snk_value(first_hit);
        if (maxpot > fv) fv = maxpot;
        r->score[opp] += fv;
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
        int miss_called = is_miss && !r->was_snookered && !needs_snookers;

        /* 3-consecutive-miss forfeit (genuine, non-snookered misses only) */
        if (is_miss && !r->was_snookered) {
            if (++r->cmiss[off] >= 3) {
                r->frame_over = 1; r->winner = opp;
                snprintf(r->msg, sizeof r->msg, "3 MISSES - LOSS");
                return;
            }
        } else if (!is_miss) r->cmiss[off] = 0;

        /* Is the incoming player snookered on the post-foul ball-on? → free ball.
         * (Skipped after a scratch — the cue is replaced in the D.) */
        int opp_snk = 0;
        if (!scratch) {
            int sv_t = r->target, sv_s = r->seq;
            r->target = target_after; r->seq = seq_after;
            opp_snk = cue_rules_is_snookered(r, b, n);
            r->target = sv_t; r->seq = sv_s;
        }

        r->target = target_after; r->seq = seq_after;
        r->dec_offender = off; r->dec_penalty = fv; r->dec_scratch = scratch;
        r->dec_can_restore = miss_called; r->dec_free_ball = opp_snk;

        if (miss_called || opp_snk) {
            /* a real choice exists → park for the opponent's decision */
            r->decision = CUE_DEC_PENDING;
            snprintf(r->msg, sizeof r->msg, miss_called ? "FOUL & MISS +%d" : "FOUL +%d", fv);
        } else {
            r->turn = opp;
            if (scratch) r->ball_in_hand = 1;
            snprintf(r->msg, sizeof r->msg, "FOUL +%d", fv);
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
        if (legal_pots > 0) {
            r->seq++;
            if (r->seq > 7) {
                r->frame_over = 1;
                r->winner = (r->score[0] >= r->score[1]) ? 0 : 1;
                snprintf(r->msg, sizeof r->msg, "FRAME OVER");
                return;
            }
        }
    }

    if (legal_pots > 0) { snprintf(r->msg, sizeof r->msg, "BREAK %d", r->brk); }
    else {
        r->cmiss[r->turn] = 0;          /* a legal shot (good safety) clears misses */
        r->brk = 0; r->turn = 1 - r->turn;
        r->target = (r->reds_left > 0) ? 0 : 2;
        if (r->target == 2 && r->seq < 2) r->seq = 2;
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

    /* the 9: potted legally wins (incl. on the break); on a foul it respots */
    if (nine_potted) {
        if (!foul) { r->frame_over = 1; r->winner = r->turn;
                     snprintf(r->msg, sizeof r->msg, "9-BALL!"); return; }
        respot_nine(r, b, n);
    }

    if (foul) {
        r->cfoul[r->turn]++;
        if (r->cfoul[r->turn] >= 3) {           /* three consecutive fouls = loss */
            r->frame_over = 1; r->winner = 1 - r->turn;
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
    if (r->kind)                       resolve_snooker(r, b, n, first_hit, scratch, potted, np);
    else if (r->mode == CUE_GAME_US9)  resolve_9ball(r, b, n, first_hit, scratch, cushion, potted, np);
    else                               resolve_pool(r, b, n, first_hit, scratch, cushion, potted, np);
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
    } else {
        r->turn = opp;
        r->ball_in_hand = r->dec_scratch ? 1 : 0;
        r->free_ball = (decision == CUE_DEC_FREEBALL && free_ball) ? 1 : 0;
    }
    return r->turn;
}

int cue_rules_ball_legal(const CueRules *r, const CueBall *b, int n, int id) {
    if (id == CUE_ID_CUE) return 0;
    if (r->kind) return r->free_ball ? 1 : snk_on(r, id);   /* free ball: any ball is on */
    if (r->mode == CUE_GAME_US9) return id == nine_lowest(b, n);  /* must hit lowest */
    if (r->open) return id != 8;                 /* open table: anything but the 8 */
    /* the 8 is legal ONLY once your own group is fully cleared */
    if (id == 8) return group_cleared(b, n, r->group[r->turn]);
    return pool_group(id) == r->group[r->turn];
}

void cue_rules_status(const CueRules *r, char *buf, int cap) {
    if (r->kind) {
        const char *on = r->target == 0 ? "RED" : r->target == 1 ? "COLOUR" :
            (r->seq == 2 ? "YELLOW" : r->seq == 3 ? "GREEN" : r->seq == 4 ? "BROWN" :
             r->seq == 5 ? "BLUE" : r->seq == 6 ? "PINK" : "BLACK");
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
