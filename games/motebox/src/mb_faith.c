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

/* THE CEILING, in one place. It was computed inline in the step, which was fine while nothing
 * else could add Faith; the refund below is a second source and two copies of a cap drift. */
static int s_temples;               /* what the last step counted */
int mb_faith_temples(void) { return s_temples; }

int mb_faith_cap(int temples)
{
    int32_t cap = 700 + temples * 120;
    return cap > 6000 ? 6000 : (int)cap;
}

/* GIVE IT BACK. A cast that reached nobody has done nothing, and the Faith has to be spent
 * before the cast because most powers cannot know their own reach until they run — so the
 * refund is the honest half of that arrangement. Never above the ceiling the religion has
 * earned, so a refund cannot be used to bank more than the cap allows. */
void mb_faith_grant(int amount)
{
    s_faith += amount;
    int cap = mb_faith_cap(mb_faith_temples());
    if (s_faith > cap) s_faith = cap;
}

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
        /* EVERY WORSHIPPER TITHES SOMETHING. This was happy/24 alone, which is zero for a
         * contented villager and only pays out for the ecstatic — so measured income was
         * +1 A YEAR at year 100 with two hundred people alive, and the whole Pantheon mode
         * was a menu of powers nobody could afford. A god's power is what its followers give
         * it, and a follower gives something. */
        int v = 1 + u->happy / 24;
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
    /* SEVEN HUNDRED, not four. The ceiling was 400 and the ANGEL costs 550, the SKULL TITAN
     * 500, the REAPER 460 and the PHOENIX 420 — so four of the eight beasts could not be cast
     * at any income, ever, and the wheel offered them anyway. Measured: cap 400 with ZERO
     * temples standing at year 100, 400 and 700 across a world, because a temple needs gold
     * and gold did not exist until the mines were fixed. The base now clears the dearest
     * power with room to spare, and temples still mean a bigger religion holds more. */
    s_temples = (int)temples;
    int32_t cap = mb_faith_cap((int)temples);


    s_income = gain;
    s_faith += gain;
    if (s_faith > cap) s_faith = cap;
}
