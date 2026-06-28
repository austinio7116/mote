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
} CueAIShot;

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
                  int restrict_d, uint32_t *rng);

/* 9-ball push-out shot: deliberately roll the cue ball to a spot that leaves
 * the opponent the WORST possible shot on the ball-on (no must-hit constraint —
 * that's the whole point of a push-out). Returns a safety-flagged shot. */
CueAIShot cue_ai_pushout(const CueWorld *w, const CueTable *t, const CueRules *r,
                         const CueBall *balls, int n, const CuePersona *p,
                         uint32_t *rng);

/* Debug: the object-ball aim point the planner would use to pot `target` into
 * pocket pk (jaw-aware). Exposed for the diagram/diagnostic tools. */
Vec3 cue_ai_pocket_aim(const CueWorld *w, const CueTable *t, int pk, Vec3 target);

#endif
