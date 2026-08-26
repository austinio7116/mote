/*
 * ThumbyCue — THE BREAK-OFF MUST NOT BE THE SAME BREAK-OFF EVERY TIME.
 *
 * The planner chooses a break by generating a grid of clip, power and side,
 * SHUFFLING it out of the caller's xorshift state, simulating as many of the
 * shuffled candidates as the sim budget allows, and then adding the persona's
 * aim and power error — also out of that state. Every one of those steps reads
 * the rng, so a break is a function of the seed and of nothing else the player
 * can see: same rack, same seed, same break, for ever.
 *
 * Which is fine, and is exactly what the AI measurements want. What is not fine
 * is the HOST handing it a constant. CueVR did, and got away with it for a long
 * time by accident: the stream is advanced by every think, so how the break came
 * out depended on how many draws had happened before it, and that count moved
 * with the frame timing. When the table bake moved to a worker thread and the
 * menu stopped hitching, the count stopped varying — and every break became the
 * same break. It was never random, it was unpredictable, and those are not the
 * same thing.
 *
 * This asserts the property the host depends on, in both directions: different
 * seeds must give visibly different break-offs, and the same seed must give the
 * same one, because reproducibility is the other half of the bargain.
 */
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_rules.h"
#include "cue_ai.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    if (!cond) { s_fail++; printf("  FAIL %s%s%s\n", what, detail ? " — " : "", detail ? detail : ""); }
    else printf("  ok   %s\n", what);
}

typedef struct { float aim, power, side, vert; } Shot;

static Shot plan_break(CueGameKind kind, uint32_t seed, int persona) {
    CueTable t;
    CueWorld w;
    CueBall b[CUE_MAX_BALLS];
    CueRules r;
    cue_table_init(&t, kind);
    cue_table_build_world(&t, &w);
    int n = cue_table_rack(&t, b);
    cue_rules_init(&r, &t, 0);
    r.break_shot = 1;
    uint32_t rng = seed;
    CueAIShot s = cue_ai_plan(&w, &t, &r, b, n, &CUE_PERSONAS[persona], &rng);
    Shot o = { s.aim, s.power01, s.tip_side, s.tip_vert };
    return o;
}

/* "Visibly different" is not "differs in the last bit". A break-off that moved
 * a thousandth of a degree is the same break-off to anybody watching, so the
 * test asks for a difference a player could actually see: a tenth of a degree
 * of aim, or two percent of power, or a real change of tip. */
static int differs(Shot a, Shot b) {
    return fabsf(a.aim   - b.aim)   > 0.1f * 3.14159265f / 180.0f
        || fabsf(a.power - b.power) > 0.02f
        || fabsf(a.side  - b.side)  > 0.05f
        || fabsf(a.vert  - b.vert)  > 0.05f;
}

int main(void) {
    /* The four the break planner has genuinely different code for: the pool
     * grid, snooker's clip off the outside red, and the two racks in between. */
    static const struct { CueGameKind k; const char *name; } GAME[] = {
        { CUE_GAME_UK8,   "UK 8-ball" },
        { CUE_GAME_US9,   "9-ball"    },
        { CUE_GAME_SNK15, "snooker"   },
        { CUE_GAME_US8,   "US 8-ball" },
    };
    /* Seeds a host would actually produce. The first is the constant CueVR used
     * to ship, kept deliberately: it is the one value most likely to be right
     * by accident. */
    static const uint32_t SEED[] = { 0x1234567u, 0xA5A5A5A5u, 0xDEADBEEFu,
                                     0x00000001u, 0x7F3E21C9u, 0xFFFFFFFFu };
    const int NS = (int)(sizeof SEED / sizeof SEED[0]);

    printf("the break-off varies with the seed\n");
    for (unsigned g = 0; g < sizeof GAME / sizeof GAME[0]; g++) {
        Shot s[8];
        for (int i = 0; i < NS; i++) s[i] = plan_break(GAME[g].k, SEED[i], 3);

        /* EVERY PAIR, not just neighbours. A generator that collapsed to two
         * outcomes would pass a neighbour-only check half the time. */
        int same = 0, pairs = 0;
        for (int i = 0; i < NS; i++)
            for (int j = i + 1; j < NS; j++) {
                pairs++;
                if (!differs(s[i], s[j])) same++;
            }
        char d[120];
        snprintf(d, sizeof d, "%d of %d seed pairs gave the same break "
                              "(aim %.2f..%.2f deg, power %.3f..%.3f)",
                 same, pairs,
                 (double)(s[0].aim * 180.0 / 3.14159265),
                 (double)(s[NS-1].aim * 180.0 / 3.14159265),
                 (double)s[0].power, (double)s[NS-1].power);
        /* Not "no pair may match": with a small candidate grid two seeds can
         * legitimately land on the same one. Most of them must differ. */
        ok(same * 2 < pairs, GAME[g].name, d);
    }

    /* AND IT MUST VARY FOR THE STRONGEST PLAYERS TOO, which is where it did
     * not and where the bug actually lived.
     *
     * The shuffle exists so that a WEAK player samples only a few candidates.
     * A strong one's budget is ncand * (0.18 + 0.82 * position), which at
     * position 1.0 is every candidate there is — so the shuffle reorders a list
     * that is then tried in full, the same candidate scores highest, and the
     * same break is played. The Machine is the pure case: position 1.0 AND
     * line_acc 0.00, so there was not even a hair of aim error between one
     * break and the next. Testing only a mid persona hid all of it.
     *
     * Personas 6 and 7 are Iron Nina and The Machine — see CUE_PERSONAS. */
    printf("\n...and for the players who try every candidate\n");
    {   static const int STRONG[] = { 6, 7 };
        for (unsigned pi = 0; pi < sizeof STRONG / sizeof STRONG[0]; pi++) {
            for (unsigned g = 0; g < sizeof GAME / sizeof GAME[0]; g++) {
                Shot s2[8];
                for (int i = 0; i < NS; i++)
                    s2[i] = plan_break(GAME[g].k, SEED[i], STRONG[pi]);
                int same = 0, pairs = 0;
                for (int i = 0; i < NS; i++)
                    for (int j = i + 1; j < NS; j++) {
                        pairs++;
                        if (!differs(s2[i], s2[j])) same++;
                    }
                char nm[64], d[120];
                snprintf(nm, sizeof nm, "%s, persona %d", GAME[g].name, STRONG[pi]);
                snprintf(d, sizeof d, "%d of %d seed pairs gave the same break",
                         same, pairs);
                ok(same * 2 < pairs, nm, d);
            }
        }
    }

    printf("\nand the same seed still gives the same break\n");
    for (unsigned g = 0; g < sizeof GAME / sizeof GAME[0]; g++) {
        Shot a = plan_break(GAME[g].k, 0x7F3E21C9u, 3);
        Shot b = plan_break(GAME[g].k, 0x7F3E21C9u, 3);
        char d[120];
        snprintf(d, sizeof d, "aim %.6f vs %.6f, power %.6f vs %.6f",
                 (double)a.aim, (double)b.aim, (double)a.power, (double)b.power);
        ok(!differs(a, b) && a.aim == b.aim && a.power == b.power,
           GAME[g].name, d);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
