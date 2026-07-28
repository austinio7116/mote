/*
 * Motebox — Faith, and the two modes.
 *
 * The one thing WorldBox does not have, and the design's answer to its honest
 * weakness (DESIGN.md 11): every power is free there, so the optimal play is
 * meteor-spam and the civilisation is scenery.
 *
 * Here a power costs FAITH, and Faith comes from worshippers: the sum over living
 * people of happiness × piety, plus temples, minus heresy. Rain costs 1. A meteor
 * costs 120. A kaiju costs 500. To afford spectacle you must first grow a
 * civilisation that loves you — so nurturing and destroying stop being two
 * unrelated toys and become a trade-off, and the growth sim you built becomes the
 * resource engine for the disaster sim.
 *
 * SANDBOX mode turns it all off, because sometimes you just want the meteor.
 */
#include "mb.h"

static int32_t s_faith;
static int     s_mode = MODE_PANTHEON;
static int32_t s_income;          /* per year, for the HUD */

void mb_faith_init(void)
{
    s_faith = 120;                /* enough for rain, a forest and a fire */
    s_income = 0;
}

int  mb_faith(void)        { return (int)s_faith; }
int  mb_faith_income(void) { return (int)s_income; }
int  mb_mode(void)         { return s_mode; }
void mb_mode_set(int m)    { s_mode = m; }
void mb_faith_set(int v)   { s_faith = v; }

int mb_faith_afford(int cost)
{
    if (s_mode == MODE_SANDBOX) return 1;
    return s_faith >= cost;
}

void mb_faith_spend(int cost)
{
    if (s_mode == MODE_SANDBOX) return;
    s_faith -= cost;
    if (s_faith < 0) s_faith = 0;
}

/* Income, once a year. Yearly rather than per tick so the number on the HUD means
 * something you can plan against, and so a single bad week does not read as your
 * religion collapsing. */
void mb_faith_step(void)
{
    if ((mb_w.tick % 52) != 0) return;

    int32_t gain = 0, temples = 0;
    for (int i = 0; i < mb_nu; i++) {
        const Unit *u = &mb_u[i];
        if (!u->alive || u->sp >= SP_CIV_N) continue;
        /* A happy worshipper tithes; a miserable one is a net loss, which is the
         * whole feedback loop in one line. Piety doubles it either way. */
        int v = u->happy / 24;
        if (u->traits & TR_PIOUS)   v *= 2;
        if (u->traits & TR_BLESSED) v += 2;
        if (u->traits & TR_CURSED)  v -= 2;
        if (u->traits & TR_MADNESS) v -= 3;     /* the mad do not pray */
        gain += v;
    }
    /* temples are the standing infrastructure of belief */
    for (int i = 0; i < NC; i++) if (mb_w.obj[i] == O_TEMPLE) temples++;
    gain += temples * 10;

    gain = gain * (100 + mb_age_faith_mod()) / 100;

    /* A FLOOR, deliberately: a player who nukes their own worshippers to zero
     * would otherwise be locked out of every power with no way back, and a sandbox
     * you can softlock is a bug however logical the rule. */
    if (gain < 1) gain = 1;

    /* AND A CEILING ON THE RESERVE, which matters more than the rate. Uncapped, a
     * 500-year world banked 39,000 Faith and every power in the game was free
     * forever — the entire trade-off the mode exists for evaporated. A god's power
     * is what its followers give it NOW, not a hoard: the cap rises with the
     * temples you inspired, so a bigger religion really does hold more. */
    int32_t cap = 400 + temples * 120;
    if (cap > 6000) cap = 6000;

    s_income = gain;
    s_faith += gain;
    if (s_faith > cap) s_faith = cap;
}
