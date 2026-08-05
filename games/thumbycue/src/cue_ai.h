/*
 * ThumbyCue — computer opponent.
 *
 * A port of the 2dpool ai.js opponent: simulation-driven shot selection with
 * positional play, across a roster of personas with distinct skill levels.
 *
 * The planner works in "JS pixel" space internally (scale = 12 / ball_radius)
 * so the original ai.js scoring constants port verbatim; aim is scale-free and
 * power is mapped to the engine's 0..1 strike scale. Candidate leaves are
 * ranked analytically, then the top few are refined with the REAL engine
 * (cue_phys_step) run headless — the same physics the shot will use.
 *
 * Cut-induced throw compensation from the original is intentionally dropped:
 * the ThumbyCue engine pots cleanly, so the ghost-ball aim is the true aim.
 */
#ifndef CUE_AI_H
#define CUE_AI_H

#include "cue_physics.h"
#include "cue_table.h"
#include "cue_rules.h"
#include <stdint.h>

enum { CUE_SEL_RANDOM = 0, CUE_SEL_TOP3, CUE_SEL_OPTIMAL };

typedef struct {
    const char *name;
    int   elo;
    float line_acc;     /* aim error, degrees (half of the ± range) */
    float power_acc;    /* power error, fraction (half of the ± range) */
    float safety_bias;  /* risk tolerance: +ve = cautious, -ve = aggressive */
    float power_bias;   /* >1 favours harder shots, <1 favours soft */
    float spin_ability; /* 0..1 max |follow/draw| the persona will use */
    float freeing;      /* 0..1 willingness/skill at disturbing stuck balls */
    int   shot_select;  /* CUE_SEL_* */
    float position;     /* 0..1 weight on positional play vs pure potting */
    int   think_ms;     /* pre-shot "thinking" delay */
} CuePersona;

#define CUE_NUM_PERSONAS 8
extern const CuePersona CUE_PERSONAS[CUE_NUM_PERSONAS];

typedef struct {
    float aim;        /* world aim angle, atan2(dz, dx) */
    float power01;    /* 0..1 strike power (× MAX_STRIKE_SPEED) */
    float tip_side;   /* english, fraction of R */
    float tip_vert;   /* follow(+)/draw(-), fraction of R */
    int   safe;       /* 1 = this is a safety, not a pot attempt */
    int   valid;      /* 0 = no legal shot found at all (rare) */
    float score;      /* confidence of the chosen pot (~0..100, higher = easier);
                       * 0 on a safety. Used by the foul / push-out decision AI. */
    /* WHICH ball this shot is on, as a ball id, or -1. The caller needs it to
     * nominate: a snooker colour has to be named before the shot, and the
     * planner is the only thing that knows which one it picked. Without it the
     * CPU was the one player at the table exempt from nominating. */
    int   target_id;
    /* The best POT confidence that was on the table when this shot was chosen,
     * or -1 if the planner could not find a pot at all. Independent of what it
     * actually played, which is the point: on a safety, `score` is 0 and tells
     * you nothing about whether the safety was forced or elective. This
     * separates "played safe because nothing was on" from "played safe with a
     * frame ball sitting there", which are opposite diagnoses. */
    float best_pot;
    /* Breakbuilding telemetry. `breakout` is the signed bonus this shot earned
     * for opening (or burying) the pack, `freed_sim` how many target balls the
     * simulation says it frees, `brk_decided` whether removing that bonus would
     * have picked a different shot — the only figure that says the behaviour is
     * doing anything. `next_pot` is the RAW difficulty of the easiest pot the
     * leave offers, with no angle/value/option extras folded in. */
    float breakout;
    int   freed_sim;
    int   brk_decided;
    float next_pot;
} CueAIShot;

/* What the CALLER's full power is, in m/s — whatever it multiplies the
 * returned power01 by before striking. The planner simulates at 8.5 m/s, which
 * is what its scoring was tuned against, and converts on the way out; so a
 * front-end whose maximum is anything else MUST say so or every shot it plays
 * will be harder or softer than the one that was planned. Defaults to 8.5, so
 * the handheld needs no call. */
void cue_ai_set_max_speed(float mps);

/* Whether the ranking sims apply the forced cue elevation (cue_table_min_elev)
 * that the front ends impose on the real shot. ON by default and it must stay
 * on in a game: with it off the planner simulates a level cue, then the game
 * tilts it and the ball leaves at cos(elev) of the planned pace, curving. The
 * setter exists so the measurement harness can disable BOTH halves together. */
void cue_ai_force_elev(int on);

/* Target balls with a clear line to a pocket — how much of the frame is
 * actually available. Exposed so a harness can measure what a shot really
 * freed, rather than trusting the planner's simulation of it. */
int cue_ai_open_targets(const CueWorld *w, const CueTable *t, const CueRules *r,
                        const CueBall *balls, int n);

/* Plan a shot for the current table state. `rng` is an xorshift state advanced
 * in place (so persona error/selection are reproducible from a seed). */
CueAIShot cue_ai_plan(const CueWorld *w, const CueTable *t, const CueRules *r,
                      const CueBall *balls, int n, const CuePersona *p,
                      uint32_t *rng);

/* Resumable form: call _start once, then _tick() once per frame until it
 * returns 1 (done), then read _result(). The render loop stays live in between
 * so a "thinking" indicator can animate instead of the game freezing. The ball
 * array / world must not change between _start and the final _tick. */
void      cue_ai_plan_start(const CueWorld *w, const CueTable *t, const CueRules *r,
                            const CueBall *balls, int n, const CuePersona *p, uint32_t *rng);
int       cue_ai_plan_tick(void);     /* returns 1 when planning is complete */
CueAIShot cue_ai_plan_result(void);

/* Ball-in-hand: choose where to place the cue ball. `restrict_d` = confine to
 * the D / behind the head string (placement already clamped by the caller). */
Vec3 cue_ai_place(const CueWorld *w, const CueTable *t, const CueRules *r,
                  const CueBall *balls, int n, const CuePersona *p,
                  uint32_t *rng);

/* 9-ball push-out shot: deliberately roll the cue ball to a spot that leaves
 * the opponent the WORST possible shot on the ball-on (no must-hit constraint —
 * that's the whole point of a push-out). Returns a safety-flagged shot. */
/* Which of PLAY / REPLAY / FREEBALL to take after the opponent's snooker foul,
 * chosen on MERIT: what can actually be potted from where the balls lie, what a
 * free ball would be worth, and whether snookers are needed. Returns a CUE_DEC_*.
 *
 * Shared because CueVR had a one-line stand-in — "restore if you can, else
 * play" — which took the replay on every foul that offered one and never once
 * looked at the table. The handheld had the real thing; now there is one of it.
 *
 * On CUE_DEC_REPLAY the CALLER must restore the pre-shot layout before applying
 * the decision, exactly as with a human's choice. */
/* Remember that a shot ON THIS BALL fouled, so the planner stops offering it
 * while anything else is available; and forget it all after a clean shot. The
 * planner keeps no state between shots, so the caller has to tell it. */
void cue_ai_note_foul(int target_id, int hit_id);
void cue_ai_clear_fouls(void);

int cue_ai_decide(const CueWorld *w, const CueTable *t, const CueRules *r,
                  const CueBall *balls, int n, const CuePersona *p,
                  uint32_t *rng);

CueAIShot cue_ai_pushout(const CueWorld *w, const CueTable *t, const CueRules *r,
                         const CueBall *balls, int n, const CuePersona *p,
                         uint32_t *rng);

/* Debug: the object-ball aim point the planner would use to pot `target` into
 * pocket pk (jaw-aware). Exposed for the diagram/diagnostic tools. */
Vec3 cue_ai_pocket_aim(const CueWorld *w, const CueTable *t, int pk, Vec3 target);

#endif
