/* KILLER FOR 2 TO 8 PLAYERS.
 *
 * Two halves. The first proves the two-player game did not move: a long run of
 * strokes (made up, but every kind the resolver can be handed -- pots, dry
 * shots, air shots, scratches, balls off the table, a table potted dry, whole
 * matches with next_frame between them) is played through the rules and every
 * thing a player can see of the game -- whose shot, the lives, ball in hand, the
 * foul, the message and the status line, the frame and the match -- is folded
 * into one hash. The expected numbers come from the rules BEFORE N-player
 * killer (mote 6dd2a9c1), so a difference anywhere in the two-player game -- a
 * message, a life, a turn, a frame booked -- fails here.
 *
 * WHAT IS SEEN, NOT THE STRUCT'S BYTES. It used to hash every byte of CueRules
 * up to `conceded`, and every field another game added in front of that (the
 * sinuca's phase, the mesinha's choices) moved the bytes and failed it with
 * Killer playing exactly as before -- the frame counts never changed. A test
 * that fails on every unrelated change is one nobody reads. Recorded again
 * this way FROM 6dd2a9c1 itself (2026-10-02), so it still compares against
 * the old rules and not against whatever they are today.
 *
 * The second plays 3, 5 and 8 players: the drawn order, who shoots next, a
 * player going out and being skipped, lives never below zero, ball in hand to
 * the next one standing, the last one standing winning, the frame booked to
 * them, the break moving round the order, and the AI planning for whoever is
 * at the table against the field.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include "cue_ai.h"
#include "mote_arena.h"
#include "mote_phys.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    printf("  %-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && detail[0] ? "   " : "", detail ? detail : "");
    if (!cond) s_fail++;
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static int NB;

static void fresh(CueRules *r, CueGameKind k) {
    cue_table_init(&T, k);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    B[0].on = 1;
    cue_rules_init(r, &T, 0);
    r->ball_in_hand = 0;
}

static void play(CueRules *r, int first, int scratch, const int *pot, int np) {
    W.ntouch = 0;
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id == pot[k]) { B[i].on = 0; break; }
    r->n_off = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, 1, pot, np);
    if (r->rerack == 2) { NB = cue_table_rack(&T, B); B[0].on = 1; r->rerack = 0; }
}

/* ---- the two-player identity run ---------------------------------------- */

static uint32_t s_rng;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= 1099511628211ull; }
    return h;
}

/* What a player can see of the game, field by field -- every one of these has
 * been in CueRules since before N-player killer, so the same function builds
 * against the old rules to record and the new ones to compare. */
static uint64_t seen(uint64_t h, const CueRules *r) {
    const int v[] = { r->turn, r->score[0], r->score[1], r->frame_over, r->winner,
                      r->ball_in_hand, r->last_foul, r->break_shot,
                      r->frames[0], r->frames[1], r->match_over, r->match_winner };
    h = fnv(h, v, sizeof v);
    return fnv(h, r->msg, strnlen(r->msg, sizeof r->msg));
}

static uint64_t identity_run(CueGameKind k, uint32_t seed, int strokes, int *nframes) {
    CueRules r;
    s_rng = seed;
    fresh(&r, k);
    r.best_of = 5;
    cue_rules_set_break(&r, (int)(rnd() & 1));
    uint64_t h = 1469598103934665603ull;
    int frames = 0;
    for (int s = 0; s < strokes; s++) {
        const uint32_t d = rnd() % 100;
        int pot[CUE_MAX_BALLS], np = 0, first = 1, scratch = 0, off = 0;
        int on[CUE_MAX_BALLS], non = 0;
        for (int i = 1; i < NB; i++) if (B[i].on) on[non++] = B[i].id;
        if (d < 8)        first = -1;                          /* an air shot */
        else if (d < 18)  scratch = 1;                         /* in-off */
        if (d >= 18 && d < 55 && non) {                        /* one, two or three down */
            int want = 1 + (int)(rnd() % 3);
            for (int k2 = 0; k2 < want && non; k2++) {
                int j = (int)(rnd() % (uint32_t)non);
                pot[np++] = on[j]; on[j] = on[--non];
            }
        }
        if (d >= 55 && d < 58) {                               /* the table potted dry */
            for (int j = 0; j < non; j++) pot[np++] = on[j];
            non = 0;
        }
        if (d >= 58 && d < 62 && np == 0 && non) { pot[np++] = on[0]; off = 1; }
        if (scratch && non && (rnd() & 1)) pot[np++] = on[0];  /* a scratch that also pots */
        W.ntouch = 0;
        for (int q = 0; q < np; q++)
            for (int i = 1; i < NB; i++)
                if (B[i].on && B[i].id == pot[q]) { B[i].on = 0; break; }
        r.n_off = off;
        cue_rules_resolve(&r, B, NB, &W, first, scratch, 1, pot, np);
        h = seen(h, &r);
        char st[64]; cue_rules_status(&r, st, sizeof st);
        h = fnv(h, st, strlen(st));
        if (r.rerack == 2) { NB = cue_table_rack(&T, B); B[0].on = 1; }
        r.rerack = 0;
        if (r.frame_over) {
            frames++;
            if (r.match_over) {
                fresh(&r, k); r.best_of = 5;
                cue_rules_set_break(&r, (int)(rnd() & 1));
            } else {
                cue_rules_next_frame(&r, &T);
                NB = cue_table_rack(&T, B); B[0].on = 1;
            }
            h = seen(h, &r);
        } else if (d == 99) {                                  /* now and then, give it up */
            cue_rules_concede(&r, r.turn);
            h = seen(h, &r);
        }
    }
    if (nframes) *nframes = frames;
    return h;
}

/* ---- N players ----------------------------------------------------------- */
#ifndef KILLER_RECORD_ONLY
static int first_is_foul(int first, int scratch) { return first < 0 || scratch; }


static int alive_count(const CueRules *r) {
    int a = 0;
    for (int p = 0; p < cue_rules_killer_players(r); p++) a += !cue_rules_killer_out(r, p);
    return a;
}

/* Play a whole frame of N-player killer with made-up strokes and check the
 * invariants on every one of them. Returns the winner. */
static int random_frame(CueRules *r, int n, uint32_t seed, int *bad, int *outs, int *nouts) {
    s_rng = seed;
    *nouts = 0;
    for (int s = 0; s < 2000 && !r->frame_over; s++) {
        const int who = cue_rules_killer_shooter(r);
        const int before = cue_rules_killer_lives(r, who);
        const int was_break = r->break_shot;
        if (cue_rules_killer_out(r, who)) (*bad)++;         /* an eliminated player at the table */
        const uint32_t d = rnd() % 100;
        int pot[1], np = 0, first = 1, scratch = 0;
        if (d < 10) first = -1; else if (d < 20) scratch = 1;
        else if (d < 45) for (int i = 1; i < NB; i++) if (B[i].on) { pot[np++] = B[i].id; break; }
        play(r, first, scratch, pot, np);
        const int after = cue_rules_killer_lives(r, who);
        const int made = !first_is_foul(first, scratch) && np > 0;
        (void)was_break;
        if (after < 0) (*bad)++;
        if (made && after != before) (*bad)++;
        if (after < before - 1) (*bad)++;
        if (after == 0 && before > 0) outs[(*nouts)++] = who;
        /* the next shooter is always standing, and is not the one who just
         * played while anybody else is */
        if (!r->frame_over) {
            const int nx = cue_rules_killer_shooter(r);
            if (cue_rules_killer_out(r, nx)) (*bad)++;
            if (nx == who) (*bad)++;
            if (scratch && !r->ball_in_hand) (*bad)++;
        }
        /* the view: the shooter's lives against the best of the field */
        {   int best = 0; const int cur = cue_rules_killer_shooter(r);
            for (int p = 0; p < n; p++)
                if (p != cur && cue_rules_killer_lives(r, p) > best) best = cue_rules_killer_lives(r, p);
            if (!r->frame_over && (r->score[r->turn] != cue_rules_killer_lives(r, cur) ||
                                   r->score[1 - r->turn] != best)) (*bad)++;
            if (r->turn < 0 || r->turn > 1) (*bad)++;
        }
        if (alive_count(r) < 1) (*bad)++;
    }
    return r->frame_over ? cue_rules_killer_winner(r) : -2;
}

#endif

int main(void) {
    printf("killer_n\n");

    /* ---- 1. two players, byte for byte what they were ---- */
    {   /* recorded from the rules before N-player killer: mote 6dd2a9c1, built with
         * -DKILLER_RECORD_ONLY and run with KILLER_RECORD=1 */
        static const struct { CueGameKind k; uint32_t seed; uint64_t want; int frames; } G[] = {
            { CUE_GAME_KILLER_UK, 1u,        0xced90973e4159d49ull, 728 },
            { CUE_GAME_KILLER_US, 12345u,    0x99fcc6f68e648993ull, 745 },
            { CUE_GAME_KILLER_CN, 987654u,   0x6120f0b5f157e357ull, 743 },
            { CUE_GAME_KILLER_UK, 55555u,    0x4de5aa8981acbb42ull, 734 },
        };
        const int record = getenv("KILLER_RECORD") != NULL;
        for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
            int fr = 0;
            uint64_t h = identity_run(G[i].k, G[i].seed, 6000, &fr);
            char d[96];
            snprintf(d, sizeof d, "kind %d seed %u: %d frames, hash %016llx",
                     (int)G[i].k, G[i].seed, fr, (unsigned long long)h);
            if (record) { printf("  REC  { %d, %uu, 0x%016llxull, %d },\n",
                                 (int)G[i].k, G[i].seed, (unsigned long long)h, fr); continue; }
            ok(h == G[i].want && fr == G[i].frames,
               "two-player killer: 6000 strokes identical to before", d);
        }
        if (record) return 0;
    }

#ifndef KILLER_RECORD_ONLY   /* built against the old rules: part 1 only */
    /* ---- 2. the draw ---- */
    {   uint8_t a[8], b2[8]; int same = 1, perm = 1, moved = 0;
        cue_rules_killer_draw(777u, 8, a); cue_rules_killer_draw(777u, 8, b2);
        int seen = 0;
        for (int i = 0; i < 8; i++) { same &= a[i] == b2[i]; seen |= 1 << a[i]; moved |= a[i] != i; }
        perm = seen == 0xFF;
        ok(same && perm && moved, "the draw: a shuffle of 0..7, the same from the same seed", "");
    }

    /* ---- 3. three players, stroke by stroke ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        const uint8_t order[3] = { 2, 0, 1 };
        cue_rules_killer_setup(&r, 3, 0, order);
        ok(cue_rules_killer_players(&r) == 3 && cue_rules_killer_lives(&r, 0) == 3 &&
           cue_rules_killer_lives(&r, 2) == 3 && cue_rules_killer_shooter(&r) == 2 &&
           cue_rules_killer_next(&r) == 0 && r.break_shot,
           "three players, three lives each, the first drawn breaks", "");
        int pot[1] = { 0 };
        play(&r, 1, 0, NULL, 0);                         /* 2: dry break, exempt */
        ok(cue_rules_killer_lives(&r, 2) == 3 && cue_rules_killer_shooter(&r) == 0,
           "a dry break costs nothing, and the table goes to the next in the order", r.msg);
        play(&r, 1, 0, NULL, 0);                         /* 0: dry: a life */
        ok(cue_rules_killer_lives(&r, 0) == 2 && cue_rules_killer_shooter(&r) == 1, "a dry shot is a life", r.msg);
        pot[0] = B[3].id; play(&r, 1, 0, pot, 1);        /* 1: pots: safe */
        ok(cue_rules_killer_lives(&r, 1) == 3 && cue_rules_killer_shooter(&r) == 2,
           "a pot is safe and the turn still moves on", r.msg);
        play(&r, 1, 1, NULL, 0);                         /* 2: scratch */
        ok(cue_rules_killer_lives(&r, 2) == 2 && r.ball_in_hand && cue_rules_killer_shooter(&r) == 0,
           "a scratch: a life, and ball in hand to the next player", r.msg);
        play(&r, -1, 0, NULL, 0);                        /* 0: air shot: 1 left */
        play(&r, 1, 0, NULL, 0);                         /* 1: dry: 2 left */
        play(&r, 1, 0, NULL, 0);                         /* 2: dry: 1 left */
        play(&r, 1, 0, NULL, 0);                         /* 0: dry: OUT */
        ok(cue_rules_killer_out(&r, 0) && cue_rules_killer_lives(&r, 0) == 0 && !r.frame_over &&
           cue_rules_killer_out_at(&r, 0) == 0 && cue_rules_killer_standing(&r) == 2,
           "the first one out is out, and the frame goes on", r.msg);
        ok(cue_rules_killer_shooter(&r) == 1 && cue_rules_killer_next(&r) == 2,
           "...and the order closes up behind them", "");
        play(&r, 1, 0, NULL, 0);                         /* 1: 1 left */
        ok(cue_rules_killer_shooter(&r) == 2, "2 follows 1", "");
        play(&r, 1, 1, NULL, 0);                         /* 2: scratch, OUT */
        ok(r.frame_over && cue_rules_killer_winner(&r) == 1 && cue_rules_killer_out_at(&r, 1) == 2 &&
           cue_rules_killer_frames(&r, 1) == 1 && r.frames[0] == 0 && r.frames[1] == 0,
           "the last one standing wins, and the frame is booked to them", r.msg);
        ok(r.score[r.turn] == 0 && r.score[1 - r.turn] == 1 && r.winner == 1 - r.turn,
           "the view: the one at the table has none, the field won it", "");
        ok(r.match_over && cue_rules_killer_match_winner(&r) == 1, "a single frame is the match", "");
    }

    /* ---- 4. going out skips a player for the ball in hand too ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_US);
        cue_rules_killer_setup(&r, 4, 1, NULL);          /* one life each */
        play(&r, 1, 0, NULL, 0);                         /* 0: dry break */
        play(&r, 1, 0, NULL, 0);                         /* 1: out */
        ok(cue_rules_killer_out(&r, 1) && cue_rules_killer_shooter(&r) == 2, "one life: out at once", r.msg);
        play(&r, 1, 0, NULL, 0);                         /* 2: out */
        ok(cue_rules_killer_shooter(&r) == 3, "...and the next, past the one who went out", "");
        play(&r, 1, 1, NULL, 0);                         /* 3: scratch, out: 0 wins */
        ok(r.frame_over && cue_rules_killer_winner(&r) == 0, "three out, the breaker wins", r.msg);
        CueRules q; fresh(&q, CUE_GAME_KILLER_US);
        cue_rules_killer_setup(&q, 4, 2, NULL);
        play(&q, 1, 0, NULL, 0);  /* 0 break */
        play(&q, 1, 0, NULL, 0);  /* 1: 1 */
        play(&q, 1, 0, NULL, 0);  /* 2: 1 */
        play(&q, 1, 0, NULL, 0);  /* 3: 1 */
        play(&q, 1, 0, NULL, 0);  /* 0: 1 */
        play(&q, 1, 0, NULL, 0);  /* 1: OUT */
        play(&q, 1, 1, NULL, 0);  /* 2: scratch, OUT -> in hand to 3, skipping nobody */
        ok(cue_rules_killer_shooter(&q) == 3 && q.ball_in_hand, "ball in hand goes to the next one standing", q.msg);
        play(&q, 1, 1, NULL, 0);  /* 3: scratch, OUT -> 0 wins */
        ok(q.frame_over && cue_rules_killer_winner(&q) == 0 && !q.ball_in_hand, "...and a scratch that ends the frame gives nobody the ball", q.msg);
    }

    /* ---- 5. a dry table with several standing goes back up; lives stay ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        cue_rules_killer_setup(&r, 5, 3, NULL);
        play(&r, 1, 0, NULL, 0);                          /* break */
        play(&r, 1, 0, NULL, 0);                          /* 1: a life */
        int pot[CUE_MAX_BALLS], np = 0;
        for (int i = 1; i < NB; i++) if (B[i].on) pot[np++] = B[i].id;
        W.ntouch = 0;
        for (int i = 1; i < NB; i++) B[i].on = 0;
        r.n_off = 0;
        cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, pot, np);
        ok(r.rerack == 2 && r.break_shot && !r.frame_over && cue_rules_killer_lives(&r, 1) == 2 &&
           cue_rules_killer_shooter(&r) == 3,
           "the table potted dry: racked again, lives kept, the next player breaks", r.msg);
    }

    /* ---- 6. 3, 5 and 8 players, whole frames, every stroke checked ---- */
    {   static const int NS[3] = { 3, 5, 8 };
        for (int t = 0; t < 3; t++) {
            const int n = NS[t];
            int bad = 0, frames = 0, wins_ok = 1, outs_ok = 1, seat_won[8] = { 0 };
            for (uint32_t seed = 1; seed <= 200; seed++) {
                CueRules r; fresh(&r, CUE_GAME_KILLER_CN);
                uint8_t order[8]; cue_rules_killer_draw(seed * 2654435761u, n, order);
                cue_rules_killer_setup(&r, n, 3, order);
                int outs[8], nouts = 0;
                const int w = random_frame(&r, n, seed, &bad, outs, &nouts);
                if (w < 0) { wins_ok = 0; continue; }
                frames++; seat_won[w]++;
                if (cue_rules_killer_out(&r, w) || cue_rules_killer_standing(&r) != 1) wins_ok = 0;
                if (nouts != n - 1) outs_ok = 0;
                for (int k = 0; k < nouts; k++)
                    if (cue_rules_killer_out_at(&r, k) != outs[k] || outs[k] == w) outs_ok = 0;
                if (cue_rules_killer_out_at(&r, n - 1) != -1) outs_ok = 0;
            }
            char d[96]; int spread = 0;
            for (int p = 0; p < n; p++) spread += seat_won[p] > 0;
            snprintf(d, sizeof d, "%d frames, %d bad strokes, winners from %d seats", frames, bad, spread);
            char what[64]; snprintf(what, sizeof what, "%d players: 200 random frames", n);
            ok(frames == 200 && bad == 0 && wins_ok && outs_ok && spread == n, what, d);
        }
    }

    /* ---- 7. a match: the break goes round the order, the tally is per player ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        r.best_of = 3;
        const uint8_t order[5] = { 4, 1, 3, 0, 2 };
        cue_rules_killer_setup(&r, 5, 1, order);
        /* frame 1: 4 breaks dry, then 1, 3, 0 go out: 2 wins */
        play(&r, 1, 0, NULL, 0); play(&r, 1, 0, NULL, 0); play(&r, 1, 0, NULL, 0);
        play(&r, 1, 0, NULL, 0);
        ok(!r.frame_over && cue_rules_killer_shooter(&r) == 2, "three out of five, one life each", r.msg);
        int pot[1] = { B[1].id }; play(&r, 1, 0, pot, 1);    /* 2: safe */
        play(&r, 1, 0, NULL, 0);                            /* 4: out, 2 wins */
        ok(r.frame_over && cue_rules_killer_winner(&r) == 2 && !r.match_over &&
           cue_rules_killer_frames(&r, 2) == 1, "frame one to player 2", r.msg);
        cue_rules_next_frame(&r, &T); NB = cue_table_rack(&T, B); B[0].on = 1;
        ok(!r.frame_over && cue_rules_killer_players(&r) == 5 && cue_rules_killer_shooter(&r) == 1 &&
           cue_rules_killer_standing(&r) == 5 && cue_rules_killer_lives(&r, 4) == 1 &&
           cue_rules_killer_frames(&r, 2) == 1 && r.break_shot && cue_rules_killer_winner(&r) == -1,
           "next frame: all back in, the break one place round (player 1)", "");
        /* frame 2: 1 breaks dry; 3, 0, 2, 4 go out -- 1 wins */
        for (int k = 0; k < 5; k++) play(&r, 1, 0, NULL, 0);
        ok(r.frame_over && cue_rules_killer_winner(&r) == 1 && !r.match_over, "frame two to player 1", r.msg);
        cue_rules_next_frame(&r, &T); NB = cue_table_rack(&T, B); B[0].on = 1;
        ok(cue_rules_killer_shooter(&r) == 3, "frame three broken by player 3", "");
        /* 3 breaks dry, 0 out, 2 pots, 4 out, 1 out, 3 out: 2 wins the match */
        play(&r, 1, 0, NULL, 0); play(&r, 1, 0, NULL, 0);
        pot[0] = B[2].id; play(&r, 1, 0, pot, 1);
        play(&r, 1, 0, NULL, 0); play(&r, 1, 0, NULL, 0); play(&r, 1, 0, NULL, 0);
        ok(r.frame_over && r.match_over && cue_rules_killer_match_winner(&r) == 2 &&
           cue_rules_killer_frames(&r, 2) == 2 && cue_rules_killer_frames(&r, 1) == 1,
           "two frames takes a best of three: player 2", r.msg);
        CueRules q = r; cue_rules_rerack(&q, &T);
        ok(cue_rules_killer_players(&q) == 5 && cue_rules_killer_shooter(&q) == 3,
           "a re-rack keeps the players and the breaker", "");
    }

    /* ---- 8. leaving ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        cue_rules_killer_setup(&r, 3, 3, NULL);
        play(&r, 1, 0, NULL, 0);                            /* 0 breaks; 1 to play */
        cue_rules_killer_retire(&r, 2);                     /* somebody else leaves */
        ok(cue_rules_killer_out(&r, 2) && cue_rules_killer_shooter(&r) == 1 && !r.frame_over,
           "a player not at the table leaves: the table stays where it is", r.msg);
        cue_rules_concede(&r, r.turn);                      /* the one at the table gives up */
        ok(r.frame_over && cue_rules_killer_winner(&r) == 0 && r.conceded &&
           cue_rules_killer_frames(&r, 0) == 1, "...and the last one left takes the frame", r.msg);
        ok(r.winner == 1 - r.turn, "...which the view calls the field", "");
        CueRules q; fresh(&q, CUE_GAME_KILLER_UK);
        cue_rules_killer_setup(&q, 3, 3, NULL);
        play(&q, 1, 0, NULL, 0);                            /* 1 to play */
        cue_rules_killer_retire(&q, 0); cue_rules_killer_retire(&q, 2);
        ok(q.frame_over && cue_rules_killer_winner(&q) == 1 && q.winner == q.turn,
           "the one at the table can be the one left", "");
    }

    /* ---- 9. two players through the new calls: the old game, the old fields ---- */
    /* every byte before the Killer fields -- both built in this run, so a
     * field added in front changes both alike */
    #define OLD_PREFIX (offsetof(CueRules, conceded) + sizeof(int))
    {   CueRules r, o; fresh(&r, CUE_GAME_KILLER_UK); o = r;
        const uint8_t order[2] = { 1, 0 };
        cue_rules_killer_setup(&r, 2, 0, order);
        cue_rules_set_break(&o, 1);
        ok(memcmp(&r, &o, OLD_PREFIX) == 0 && cue_rules_killer_shooter(&r) == 1 &&
           cue_rules_killer_lives(&r, 0) == 3 && cue_rules_killer_players(&r) == 2,
           "setup(2) is cue_rules_set_break and nothing else", "");
        play(&r, 1, 0, NULL, 0); play(&r, 1, 0, NULL, 0);
        ok(cue_rules_killer_lives(&r, 0) == 2 && cue_rules_killer_shooter(&r) == 1,
           "and the calls read score[] and turn", "");
    }

    /* ---- 10. the AI plans for whoever is at the table, against the field ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        cue_rules_killer_setup(&r, 6, 3, NULL);
        play(&r, 1, 0, NULL, 0);                             /* a dry break */
        play(&r, 1, 0, NULL, 0);                             /* 1 loses one */
        play(&r, 1, 0, NULL, 0);                             /* 2 loses one: 3 to play */
        /* the same table, as a two-player game seen from the same seat */
        CueRules two = r; two.kl_n = 2;     /* what a two-player game holds */
        uint32_t g1 = 42, g2 = 42;
        CueAIShot a = cue_ai_plan(&W, &T, &r,   B, NB, &CUE_PERSONAS[3], &g1);
        CueAIShot b = cue_ai_plan(&W, &T, &two, B, NB, &CUE_PERSONAS[3], &g2);
        char d[96];
        snprintf(d, sizeof d, "view %d-%d (turn %d), aim %.4f/%.4f pow %.3f/%.3f",
                 r.score[r.turn], r.score[1 - r.turn], r.turn, a.aim, b.aim, a.power01, b.power01);
        ok(r.score[r.turn] == 3 && r.score[1 - r.turn] == 3 &&
           a.valid && a.aim == b.aim && a.power01 == b.power01 && g1 == g2,
           "the plan from six players is the plan against one opponent with the field's lives", d);
    }

#endif
    printf("\n%s\n", s_fail ? "FAILED" : "all good");
    return s_fail != 0;
}
