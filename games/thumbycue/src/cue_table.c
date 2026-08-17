/*
 * ThumbyCue — table geometry & racks. See cue_table.h.
 *
 * The pocket-jaw model is the heart of the game. Each cushion is a straight
 * nose between two knuckle points, with a *facing* cut at each end running
 * back into the pocket throat. US pool uses straight mitred facings with
 * sharp points (corner cut 142°, side ~104°); snooker/UK uses short facings
 * with large rounded knuckles. Both the physics collision geometry (segments
 * + knuckle circles + capture points) and the 3D render mesh are generated
 * from this one description, so they can never disagree.
 */
#include "cue_table.h"

#ifndef CUE_JAW_SEGS
#define CUE_JAW_SEGS 3
#endif
#include "cue_types.h"
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#define DEG (3.14159265f / 180.0f)


/* ---- THE RAIL, PER TABLE -------------------------------------------------
 *
 * Restitution at a crawl, and how fast it falls with pace. Set against the only
 * measurements there are: Mathavan's high-speed imaging of a SNOOKER table gives
 * 0.910 for a ball barely moving and a 0.818 best fit across 0.28-3.5 m/s, so
 * snooker is the one table here with a source behind its rail and the others are
 * placed relative to it.
 *
 * Nobody publishes restitution by table type, so the spread is judgement and is
 * deliberately small. What IS documented is the profile and the cloth: American
 * tables run the pointed K-66 on napless worsted and are described everywhere as
 * the faster, livelier game; English and snooker use the flat L-shaped section
 * on napped wool. Published rubber-resilience figures differ by about three
 * points between profiles (Artemis K-66 ~72%, Klematch P59 ~75%), which is the
 * order of difference used here — not the chasm between a match table and a
 * tired coin-op, because these are all CHAMPIONSHIP tables. A worn pub cushion
 * would be a different and much deader thing, and is not what this is.
 *
 * Nose height, which is the variable that dominates all of this, is already
 * right and identical on every table: cushion_h = 1.27 R is 63.5% of the ball,
 * which is the WPA specification.
 *
 * These are the numbers going IN. What comes out is lower, because the friction
 * impulse at a contact above centre has to reverse the roll as well as the
 * travel — about six points at a crawl and more with pace. So they were tuned
 * against the MEASURED rebound, not set to the published figure and hoped for:
 * snooker lands on 83% at 2 m/s, 76% at 3.5 and 61% at 7, against Mathavan's
 * 0.818 across normal play and Marlow's 0.55 for a hard one. */
static void cue_table_rails(CueTable *t, CueGameKind kind) {
    switch (kind) {
    case CUE_GAME_US8: case CUE_GAME_US9:
    case CUE_GAME_STRAIGHT:                    /* K-66, worsted: the lively one */
        t->e_cush = 0.985f; t->cush_efall = 0.046f; break;
    case CUE_GAME_UK8:                          /* championship English, Northern rubber */
        t->e_cush = 0.965f; t->cush_efall = 0.052f; break;
    case CUE_GAME_CN8:                          /* built to English patterns */
        t->e_cush = 0.968f; t->cush_efall = 0.050f; break;
    default:                                    /* snooker, and 6-red on the English bed */
        t->e_cush = 0.970f; t->cush_efall = 0.050f; break;
    }
    t->e_cush_min = 0.55f;                      /* Marlow's rails, as the floor */
}

void cue_table_init(CueTable *t, CueGameKind kind) {
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    /* ENGLISH BILLIARDS IS PLAYED ON A SNOOKER TABLE, and this flag is about
     * the TABLE — the bed, the pockets, the D, the four spots and the ball
     * size. It is not about the rules: cue_rules_init asks the kind as well,
     * because billiards scores cannons and in-offs and resolve_snooker would
     * make nonsense of it. One table, two quite different games, exactly as
     * UK 8-ball and 6-red snooker already share a bed. */
    t->is_snooker = (kind == CUE_GAME_SNK10 || kind == CUE_GAME_SNK15 ||
                     kind == CUE_GAME_SNK6  || kind == CUE_GAME_BILLIARDS);

    if (kind == CUE_GAME_UK8 || kind == CUE_GAME_SNK6) {
        /* 7 ft UK pub 8-ball: 1.98 × 0.99 m, tight ROUNDED (curved) pockets. */
        t->half_len = 1.98f * 0.5f;
        t->half_wid = 0.99f * 0.5f;
        /* ENGLISH BALLS ON AN ENGLISH TABLE. This bed was carrying American
         * 2 1/4 in balls (57.15 mm, 170 g), which is the wrong ball for both
         * games played on it. English pool is 2.000 in ± 0.005 (50.8 mm) at
         * 4.5-5.0 oz under WEPF rules, and a 7 ft snooker set is the same size.
         *
         * Everything around it is written in BALL-RADII, so the pockets would
         * have followed the ball down. They are rescaled below to keep the cut
         * exactly the size it was: the ball changes, the table does not.
         *
         * Mass barely matters here and is set for completeness: equal masses
         * cancel out of the collision impulse, and the strike gives
         * omega = (r x J)/I with both J and I proportional to m. */
        t->R = 0.0254f; t->mass = 0.116f;
        /* AND THE WHITE IS SMALLER, which is the thing about an English table.
         * 47.6 mm and 94 g against the object balls' 50.8 and 116 — the
         * convention comes from coin-op ball returns needing to tell the cue
         * ball from the rest, and it is what a pub table has in it.
         *
         * It is not a cosmetic difference. A lighter cue ball comes back off a
         * contact faster and carries less through it, and a smaller one sits
         * lower against the cushion nose. Both fall out of the physics now that
         * a ball carries its own size and weight rather than borrowing the
         * set's. */
        t->cue_R = 0.0238f; t->cue_mass = 0.094f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.075f;
        t->pocket_round = 1;
        /* TIGHTER THAN THEY WERE, and about the size a 7 ft table's pockets
         * actually are. These are radii in ball-radii, so the mouth was 4.30
         * ball-radii across for a 2 in ball — 123 mm, where a real pub table
         * cuts about 89 mm. 1.72 took it to 98 mm and played a shade mean, so
         * it sits at 1.80 (103 mm) — still a pub pocket rather than the barn
         * door it was. Both games on this bed get it: one table, two rule sets. */
        /* THE POCKETS DO NOT MOVE. Every dimension here is written in ball-radii,
         * so shrinking the ball from 2 1/4 in to a proper 2 in English ball
         * would have shrunk the cut with it — 103 mm of corner mouth down to
         * 91 mm — and these were tuned by eye on the bench at the size they
         * are. The coefficients are therefore multiplied by 1.125, the ratio of
         * the old ball to the new one, so the hole a player sees is exactly the
         * hole that was there before and only the ball has changed size.
         *
         * That makes the pockets MORE generous relative to the ball, which is
         * what the bench tuning chose; the numbers are a description of that
         * table, not of the WEPF book. */
        t->pr_corner  = 2.025f * t->R; t->pr_side  = 1.8337f * t->R;
        /* knuckles: what pr_corner + 0.833R / + 1.083R used to give */
        t->gap_corner = 2.9621f * t->R; t->gap_side = 3.2434f * t->R;
        t->facing_len = 1.8754f * t->R;
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        /* Throat set back into the wood so the bore circle clears the (deepened)
         * cushion back and a proper wood ring is cut — see reach math in PLAN. */
        t->off_corner = 0.675f * t->R; t->off_side = 1.4062f * t->R;
        /* Tuned on the bench: the catch IS the hole, and it sits deeper in. */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.3375f * t->R; t->drop_back_side = 0.18f * t->R;
        t->jaw_r = 0.004f;
        t->cloth = RGB565C(22, 120, 70);
        t->rail = RGB565C(96, 54, 26); t->rail_top = RGB565C(128, 78, 38);
        t->spot = RGB565C(180, 180, 180); t->nballs = 16;
        /* UK 8-ball baulk line + D (white placed in the D after a foul). */
        t->baulk_x = -t->half_len * 0.6f; t->d_radius = t->half_wid * 0.35f;
        if (kind == CUE_GAME_SNK6) {
            /* 6-red snooker on the 7 ft UK table: same table geometry and ball
             * size as UK pool, but snooker balls/rules and snooker spots scaled
             * onto the small bed. */
            t->reds = 6;
            /* AND A MATCHED WHITE. The small cue ball is a POOL convention —
             * coin-op tables need to tell it from the object balls to return
             * it — and a snooker set has no such thing whatever bed it is
             * played on. It shares this table's geometry, not its ball box. */
            t->cue_R = 0.0f; t->cue_mass = 0.0f;
            t->cloth = RGB565C(4, 135, 21);            /* snooker green */
            t->spot = RGB565C(200, 200, 200);
            t->blue_x  = 0.0f;                          /* centre spot */
            t->pink_x  = t->half_len * 0.5f;            /* between centre and top */
            t->black_x = t->half_len - 0.135f;          /* ~scaled from full table */
            t->nballs  = 13;                            /* cue + 6 reds + 6 colours */
        }
    } else if (kind == CUE_GAME_US8 || kind == CUE_GAME_US9 ||
               kind == CUE_GAME_STRAIGHT) {
        /* 9 ft US table: 2.54 × 1.27 m, 2.25" balls, ANGLED straight-mitre
         * pockets (sharp points, more open than UK). Straight pool is played on
         * this same bed with the same fifteen balls — 14.1 is a rules game, not
         * a table game, which is why it costs a rack function and a resolver
         * and no geometry at all. */
        t->half_len = 2.54f * 0.5f;
        t->half_wid = 1.27f * 0.5f;
        t->R = 0.028575f; t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.080f;
        t->pocket_round = 0;                 /* straight mitred facings */
        /* TIGHTENED 20%, at the user's call: these were far and away the most
         * open pockets in the set — 2.75 R at the corner against snooker's 1.98
         * and the UK pub table's 2.15 — and they looked it next to the others.
         * Both numbers move together: `pr` is the mouth you see and `gap` is
         * where the jaw tips actually sit, so tightening one without the other
         * would draw a smaller pocket than the ball is allowed through.
         *     corner  2.75 -> 2.20 R      side  2.35 -> 1.88 R
         *     gap     3.20 -> 2.56 R      gap   2.95 -> 2.36 R
         * Still the widest mouths on any table here, which is right for an
         * American table; they are no longer in a class of their own. */
        t->pr_corner  = 2.20f * t->R; t->pr_side  = 1.88f * t->R;
        t->gap_corner = 2.56f * t->R; t->gap_side = 2.36f * t->R;
        t->facing_len = 1.55f * t->R;
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        t->off_corner = 1.30f * t->R; t->off_side = 1.20f * t->R;  /* corners set back into the pocket */
        /* Tuned on the bench: the catch IS the hole, and it sits deeper in. */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.28f * t->R; t->drop_back_side = 0.30f * t->R;
        t->jaw_r = 0.004f;
        t->cloth = RGB565C(18, 110, 120);    /* US tables often tournament blue-green */
        t->rail = RGB565C(70, 46, 30); t->rail_top = RGB565C(100, 66, 42);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = (kind == CUE_GAME_US9) ? 10 : 16;
    } else if (kind == CUE_GAME_CN8) {
        /* Chinese 8-ball: 10 ft table, full-size pool balls (solids/stripes),
         * but TIGHT ROUNDED ("Chinese template") pockets — closer to English
         * than American. Rack/break/spots as US 8-ball (foot spot, head string),
         * rules are WPA 8-ball (no UK two-shot). */
        t->half_len = 2.84f * 0.5f;
        t->half_wid = 1.42f * 0.5f;
        t->R = 0.028575f; t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.080f;
        t->pocket_round = 1;                 /* rounded jaws, tight */
        /* Tighter again on the bench — they were too loose next to the others. */
        t->pr_corner  = 2.03f * t->R; t->pr_side  = 1.88f * t->R;
        t->gap_corner = 2.675f * t->R; t->gap_side = 3.133f * t->R;
        t->facing_len = 1.667f * t->R;
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        t->off_corner = 0.60f * t->R; t->off_side = 1.25f * t->R;
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.31f * t->R; t->drop_back_side = 0.50f * t->R;
        t->jaw_r = 0.005f;
        t->cloth = RGB565C(22, 44, 155);     /* Chinese-8 royal blue cloth */
        t->rail = RGB565C(78, 48, 28); t->rail_top = RGB565C(112, 70, 36);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = 16;
    } else if (CUE_GAME_IS_PYRAMID(kind)) {
        /* G2 — RUSSIAN PYRAMID. A 12 ft bed and 68 mm balls, into pockets barely
         * wider than the ball: the official corner opening is 72-74 mm against a
         * 68 mm ball, so a pot has about two millimetres to spare on each side.
         * That is the whole character of the game, and it is why this is mostly
         * a table-parameters exercise rather than a rules one — the numbers the
         * workshop's bore, setback and drop-cap rows exist for are the numbers
         * that decide whether a pot is possible at all here.
         *
         * The bed is the 12 ft snooker bed, because it is: 3.55 x 1.78 m. What
         * differs is the ball, the mouths and the cloth. */
        /* ...on the tournament bed. The 7 ft is the same game on the bed a
         * house can hold: 198 x 99 cm with a 60 mm ball, which is the smallest
         * pairing the federation lists. Everything below is written off R and
         * the mouths, so the two sizes share the whole rest of this block. */
        const int home = (kind == CUE_GAME_PYRAMID7);
        /* THE BALL IS THE BOTTOM OF ITS RANGE, on both beds.
         *
         * The federation lists 67 mm at 255 g as the tournament ball and 63,
         * 60 and 57.15 as the smaller sets for smaller tables; 68 mm is the
         * OLD tournament size, which is what this shipped with. These balls
         * are enormous next to a snooker ball however you cut it — that is the
         * game — so where there is a choice it is made downwards. */
        if (home) {
            t->half_len = 1.980f * 0.5f;
            t->half_wid = 0.990f * 0.5f;
            /* 2 1/4 inch, the smallest set the federation lists, on the
             * smallest bed it lists. Same phenolic, so the mass follows the
             * cube of the diameter from the tournament ball's 255 g. */
            t->R = 0.0285750f; t->mass = 0.158f;
            t->rail_w = 0.070f;
        } else {
            t->half_len = 3.550f * 0.5f;
            t->half_wid = 1.775f * 0.5f;
            t->R = 0.0335f; t->mass = 0.255f;   /* 67 mm, and heavy with it */
            t->rail_w = 0.085f;
        }
        t->cushion_h = 1.20f * t->R;
        /* MITRED, not rounded. The federation's specification gives the openings
         * to the millimetre and says nothing about the jaw profile, but every
         * description of a Russian table calls the губки — the lips — SHARP,
         * and sharp is what a straight-cut facing is and what a snooker table's
         * rounded knuckle is precisely not. It is a workshop row either way, so
         * a player who disagrees can have the other one in two presses.
         *
         * Sources: ru.wikipedia "Пирамида (бильярд)" for the dimensions, which
         * are quoted in the numbers below. */
        t->pocket_round = 0;
        /* 72-73 mm at the corner and 82-83 mm in the middle, on a 68 mm ball —
         * three millimetres of total slack at a corner. The middle is the WIDER
         * of the two, which is the other way round from every other table here.
         *
         * AND THAT ONE NUMBER IS THE WHOLE PROBLEM WITH THE REST OF THEM. Every
         * pocket parameter on the mitred tables is written as a multiple of the
         * BALL — gap 2.56 R, facing 1.55 R, offset 1.30 R — and that works
         * because their mouth is always about 2.2 R, so "a multiple of the ball"
         * and "a multiple of the mouth" are the same statement. Here the mouth
         * is 1.07 R. Copying the ball-relative numbers made every one of them
         * roughly twice the size of the hole they were shaping: the cushion
         * ended 60 mm from a 73 mm pocket, the facings ran past the mouth
         * entirely, and the bore was wider than the hole it was boring.
         *
         * So they are written against the MOUTH, at the ratios the 9 ft
         * American — the mitred table this one is actually like — happens to
         * have. Those are: knuckle 1.16 and 1.26 of the mouth, facing 0.7-0.8,
         * offset 0.59 and 0.64, bore 0.86 and 0.94, cut 1.39 and 1.41 with a
         * setback of 0.52 and 0.57. */
        /* THE CLEARANCE, NOT THE OPENING, IS THE CONSTANT. 73 and 82.5 mm are
         * what the tournament ball's 68 comes to once you add the federation's
         * 5 mm at a corner and 14.5 in a middle; the 60 mm ball gets the same
         * two allowances, which lands it at 65 and 74.5 — inside the 4-5 and
         * 14-18 mm the smaller tables are quoted at. Written this way round so
         * a bigger or smaller ball cannot silently stop fitting. */
        t->pr_corner = t->R + 0.00250f;      /* 72.0 mm / 62.2 mm across */
        t->pr_side   = t->R + 0.00725f;      /* 81.5 mm / 71.7 mm across */
        /* THE KNUCKLE GAP IS THE POCKET, and pr_corner is not.
         *
         * pr_corner/pr_side drive the bore, the cut and the drop — how big the
         * hole in the slate is and how far in a ball must get. What a ball has
         * to physically SQUEEZE THROUGH is the distance between the two facing
         * TIPS, and that is gap_corner/gap_side. On a pool table the two are
         * within a few millimetres of each other and nobody could tell; here
         * the entire clearance is five millimetres, so getting them confused
         * built a 65 mm hole for a 68 mm ball. Balls did not stick on the lip
         * because the physics was wrong — they stuck because they did not fit,
         * and only ever went down when the capture radius reached out and took
         * them off the bed.
         *
         * These two are therefore set by MEASUREMENT, not by ratio: swept in
         * tools/probe_gap until the narrowest passage came out at the FBS
         * numbers of 73 mm at the corner and 82.5 mm at the middle, against a
         * 68 mm ball, and test_gap holds them there for both beds.
         *
         * The corner's squeeze is the two facing TIPS and scales with the
         * mouth; the middle's is the two jaw circles, and those sit a recess of
         * jaw_r + 0.15 R behind their tips, so that one goes off the BALL. Get
         * those the wrong way round and the size that was tuned comes out right
         * while the other is a millimetre out — which is a whole third of the
         * slack this game has. */
        t->gap_corner = 1.415f * t->pr_corner;   /* -> 73.0 / 65.0 across */
        t->gap_side   = t->R + 0.01136f;         /* -> 82.5 / 74.5 across */
        /* Against the CORNER's mouth, because the corner is where two facings
         * come at each other and a long one makes them meet before the pocket
         * does. One field serves both pockets, and this table is the first
         * where the two mouths differ enough for that to matter — its middle is
         * WIDER than its corner, where every other table is the other way
         * about. Sized for the tighter of the two; a facing that is short of a
         * middle pocket simply stops short, which is harmless. */
        t->facing_len = 0.705f * t->pr_corner;
        /* THE MIDDLE'S FACINGS RUN ALMOST PARALLEL. A pool middle splays at 70
         * degrees off the rail and the mouth tapers in like a funnel; the
         * photographs of a Russian table show the two cushion ends facing each
         * other across a slot, which is a splay of very nearly ninety. Left a
         * couple of degrees short of parallel because a cushion end always has
         * a little relief on it, and dead parallel reads as a machined slot. */
        t->ang_corner = 45.0f; t->ang_side = 88.0f;
        t->off_corner = 0.591f * t->pr_corner;
        t->off_side   = 0.638f * t->pr_side;
        /* THE CATCH IS THE ONE THING THAT SCALES WITH THE BALL, not the mouth:
         * it is about how far a BALL has to travel past the cushion line before
         * it has gone, and a 68 mm ball needs the same fraction of itself
         * whatever size the hole is. The American gets 0.90 and 0.68 of a ball
         * radius; at cap = 0 this table would get 0.44 of one, which is a ball
         * sitting on the lip. A negative cap makes the catch larger than the
         * mouth — see the note beside cap_corner in TAB_FIELDS. */
        t->cap_corner = t->pr_corner - t->off_corner - 0.90f * t->R;
        t->cap_side   = t->pr_side   - t->off_side   - 0.68f * t->R;
        t->drop_back  = 0.28f * t->R; t->drop_back_side = 0.30f * t->R;
        t->jaw_r = 0.004f;
        /* THE HOUSE, which is what a pyramid table has instead of a D: a line
         * across the baulk end with the cue ball played from behind it. Carried
         * in baulk_x + d_radius because that is the pair every renderer and the
         * placement clamp already read, and a house IS a D as far as both are
         * concerned — a region behind a line that the cue ball starts in. */
        t->baulk_x  = -t->half_len + (home ? 0.407f : 0.730f);
        t->d_radius =  home ? 0.162f : 0.290f;
        t->house    = 1;                      /* the whole width behind it */
        t->cloth = RGB565C(20, 105, 60);
        t->rail = RGB565C(70, 42, 24); t->rail_top = RGB565C(100, 60, 32);
        t->spot = RGB565C(200, 200, 200);
        t->nballs = 16;                       /* the white and fifteen */
    } else {
        /* Snooker — SNK10 (10 ft, 10 reds) or SNK15 (12 ft, 15 reds). Curved
         * jaws. Layout offsets scale with table length off the 12 ft master. */
        /* Billiards has no reds in the snooker sense — it has ONE red, and it
         * is an object ball rather than one of a pack. */
        t->reds = (kind == CUE_GAME_BILLIARDS) ? 0
                : (kind == CUE_GAME_SNK10) ? 10 : 15;
        float master = 3.569f * 0.5f;        /* 12 ft half-length */
        if (kind == CUE_GAME_SNK10) { t->half_len = 2.972f * 0.5f; t->half_wid = 1.483f * 0.5f; }
        else                        { t->half_len = 3.569f * 0.5f; t->half_wid = 1.778f * 0.5f; }
        float sc = t->half_len / master;     /* layout scale */
        t->R = 0.0262500f; t->mass = 0.142f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.085f;
        t->pocket_round = 1;
        t->pr_corner  = 1.98f * t->R; t->pr_side  = 1.82f * t->R;
        t->gap_corner = 2.813f * t->R; t->gap_side = 3.063f * t->R;
        t->facing_len = 1.25f * t->R;
        t->ang_corner = 60.0f; t->ang_side = 80.0f;
        /* Throat set well back into the wood: the small snooker pocket radius is
         * < the deepened cushion depth, so without this the bore circle never
         * reaches the wood and no cutaway is cut (the fall is realistically set
         * back behind the mouth anyway). */
        t->off_corner = 1.30f * t->R; t->off_side = 1.00f * t->R;  /* corners back, but not too far */
        /* Tuned on the bench: the catch IS the hole, and it sits deeper in. */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.29f * t->R; t->drop_back_side = 0.63f * t->R;
        t->jaw_r = 0.012f;
        t->baulk_x = -t->half_len + 0.737f * sc;
        t->d_radius = 0.292f * sc;
        t->blue_x = 0.0f;
        t->pink_x = t->half_len * 0.5f;
        t->black_x = t->half_len - 0.324f * sc;
        t->cloth = RGB565C(4, 135, 21);
        t->rail = RGB565C(74, 44, 22); t->rail_top = RGB565C(104, 62, 30);
        t->spot = RGB565C(200, 200, 200);
        /* THE STANDARD TABLE'S OWN FIGURES, and billiards uses every one of
         * them: the baulk-line 737 mm from the bottom cushion, a D of 292 mm,
         * the Spot 324 mm below the top cushion, the Centre Spot midway and the
         * Pyramid Spot midway between those two. They are already here because
         * snooker needs the same four marks — blue_x is the Centre Spot,
         * pink_x the Pyramid Spot and black_x the Spot. */
        t->nballs = (kind == CUE_GAME_BILLIARDS) ? 3
                  : (t->reds == 10) ? 17 : 22;
    }
    /* Drop-zone setback — how far the potted ball sinks BACK into the pocket
     * (past the cushion mouth) before it disappears. Scaled off each table's
     * pocket-mouth size so it tracks the official openings: a corner has a deep
     * fall (~0.6× its mouth radius), a middle pocket is shallow and the ball
     * must enter centrally, so it pulls straight back only a little (~0.3×). */
    /* The head string, for the US-style tables that have one instead of a D.
     * It has to live in `baulk_x` because that is the field every renderer asks
     * for the cross-bed line — leave it zero and the line is drawn down the
     * middle of the table, which is exactly what CueVR was doing. d_radius stays
     * zero: an American table has a head string and no D. */
    if (kind == CUE_GAME_US8 || kind == CUE_GAME_US9 || kind == CUE_GAME_CN8 ||
        kind == CUE_GAME_STRAIGHT)
        t->baulk_x = -t->half_len * 0.5f;

    /* THE HOLE IN THE TIMBER, dialled per table in tools/pocketbench.
     *
     * It began as the mouth radius, concentric with it, because it had never
     * been anything else — and where it does not reach the end of the cushion
     * there is a slot between the two that you can see straight out of the
     * table through. Every table wanted a different answer, and not all in the
     * same direction: the American mitres and the Chinese jaws want a SMALLER
     * hole (from outside, less hole is less to see through), the rounded jaws a
     * BIGGER one (the bore wall is full height, so widening it carries that
     * wall out to meet the cushion). The setback moves the same hole away from
     * the cloth instead of resizing it.
     *
     * Dialled on the bench, one table at a time, against the two views the
     * faults show in — outside on the corner diagonal, and inside over the
     * cloth at a middle pocket. */
    t->bore_corner = t->pr_corner;     /* the fallback, if a kind is added */
    t->bore_side   = t->pr_side;
    t->bore_set_corner = 0.0f;
    t->bore_set_side   = 0.0f;
    switch (kind) {
    case CUE_GAME_US8: case CUE_GAME_US9: case CUE_GAME_STRAIGHT:
        t->bore_corner = 1.8900f * t->R; t->bore_side = 1.7600f * t->R;
        t->bore_set_corner = 0.0000f * t->R; t->bore_set_side = 0.0000f * t->R;
        break;
    case CUE_GAME_CN8:
        t->bore_corner = 1.8700f * t->R; t->bore_side = 1.7800f * t->R;
        t->bore_set_corner = 0.5400f * t->R; t->bore_set_side = 0.4500f * t->R;
        break;
    case CUE_GAME_UK8: case CUE_GAME_SNK6:
        t->bore_corner = 2.0700f * t->R; t->bore_side = 1.8900f * t->R;
        t->bore_set_corner = 0.0000f * t->R; t->bore_set_side = 0.1800f * t->R;
        break;
    case CUE_GAME_SNK10:
        t->bore_corner = 2.1300f * t->R; t->bore_side = 1.7100f * t->R;
        t->bore_set_corner = 0.2700f * t->R; t->bore_set_side = 0.5300f * t->R;
        break;
    case CUE_GAME_PYRAMID: case CUE_GAME_PYRAMID7:
        /* Against the MOUTH, at the 9 ft American's ratios — the bore is a
         * little SMALLER than the hole it serves on every mitred table, and
         * concentric with it. The fallback makes it equal to the mouth, and a
         * guess of 1.35 R made it wider, which bores the timber out past the
         * cloth cut and leaves cloth lying over the hole. */
        t->bore_corner = 0.86f * t->pr_corner; t->bore_side = 0.94f * t->pr_side;
        /* ...EXCEPT THAT 0.86 OF THIS MOUTH IS SMALLER THAN THE BALL. See the
         * floor applied below: those ratios were tuned where the mouth is about
         * 2.2 R, and on a table whose mouth is 1.07 R they bore a 63 mm hole
         * under a 68 mm ball. The ball got through the cushions and sat on the
         * timber, which is what "it stops on the lip" looks like from above. */
        /* THE SETBACK IS NOT MOUTH-RELATIVE, and this is the one number where
         * that matters. The bore is a hole in the TIMBER, and the timber is
         * 85 mm of rail whatever size the mouth is — so a bore concentric with
         * a pocket that sits only 22 mm outside the cushion line never reaches
         * the wood, and the rail runs past the corner unbroken. The American's
         * pocket sits 37 mm out and its bore is concentric because it is
         * already in the wood; this one has to be pushed there. Dialled on the
         * bench against the 9 ft American's corner, which is the mitred pocket
         * this one is a small copy of. */
        t->bore_set_corner = 0.85f * t->R;
        /* THE MIDDLE SITS FURTHER BACK IN THE TIMBER than the corner does. The
         * setback runs toward the CLOTH, and at 0.55 R the middle's hole hung
         * out over the bed with the cushion line running across it, so the
         * pocket read as a hole cut in the cloth rather than an opening in the
         * rail. A Russian middle is a narrow slot between two nearly parallel
         * cushion ends with the drop behind it, and that is what it looks like
         * once the bore is pulled back level with the timber. */
        t->bore_set_side   = 0.30f * t->R;
        break;
    case CUE_GAME_BILLIARDS:
        /* The standard table, so the 12 ft snooker numbers to the digit. */
    case CUE_GAME_SNK15:
        /* The two snooker tables share every other pocket number and NOT these:
         * the 12 ft was dialled on its own and came out wanting a deeper set
         * back and a tighter middle. */
        t->bore_corner = 2.1100f * t->R; t->bore_side = 1.6500f * t->R;
        t->bore_set_corner = 0.2900f * t->R; t->bore_set_side = 0.8100f * t->R;
        break;
    default: break;
    }

    /* A BORE IS A HOLE A BALL FALLS THROUGH, whatever ratio it is written at.
     *
     * Every ratio above was chosen on a table whose mouth is about 2.2 R, where
     * 0.86 of the mouth is still comfortably wider than a ball and the question
     * never came up. Russian pyramid's mouth is 1.07 R. The same ratio bored a
     * 63 mm hole under a 68 mm ball: the ball squeezed past the cushions,
     * reached the pocket, and stopped on the timber — indistinguishable from
     * the jaws being too tight, and it survived the jaws being fixed.
     *
     * So there is a floor, and it is expressed against the BALL because that is
     * what has to fit. It cannot fire on any table whose bore is near 1.9 R,
     * which is all of them but this one. */
    {   float bmin = t->R + 0.0035f;       /* the ball, and room to fall */
        if (t->bore_corner < bmin) t->bore_corner = bmin;
        if (t->bore_side   < bmin) t->bore_side   = bmin; }

    cue_table_rails(t, kind);

    /* How far past the pocket the DROP is centred. Zero is concentric with the
     * hole, which is where it has always been; positive pushes it deeper, so a
     * ball has to get further in before it is down. Per pocket type because a
     * middle is a shallower thing than a corner. */
}

/* ---- THE TABLE AS A VALUE ------------------------------------------------ *
 * See cue_table.h. ONE description of the fields; pack, unpack, hash and
 * validate all read it, so a field added to CueTable is added here once and all
 * four learn about it together. */

enum { TF_F32, TF_I32, TF_U16 };
enum {
    TF_LOOK = 0,   /* colour only: packed and range-checked, never hashed */
    TF_SIM  = 1    /* moves a cushion, a pocket or a ball: goes in the hash */
};

typedef struct {
    const char *name;
    unsigned short off;
    unsigned char  type, sim;
    float lo, hi;              /* the sensible range, in the field's own units */
} CueTabField;

#define TF(f, ty, s, lo, hi) { #f, (unsigned short)offsetof(CueTable, f), ty, s, lo, hi }

static const CueTabField TAB_FIELDS[] = {
    TF(kind,            TF_I32, TF_SIM,  0.0f, (float)CUE_GAME_COUNT - 1.0f),
    TF(is_snooker,      TF_I32, TF_SIM,  0.0f, 1.0f),
    TF(reds,            TF_I32, TF_SIM,  0.0f, 15.0f),
    /* The bed. At least a metre of play, at most a 12 ft snooker table with a
     * little room to grow — and the WIDTH now reaches as far as the length,
     * because a square bed is the shape an L-shaped table wants and the old
     * ceiling of 1.10 m refused it outright on anything bigger than a 9 ft.
     * Nothing here says a table has to be longer than it is wide; that was a
     * range picked from the tables that existed. */
    TF(half_len,        TF_F32, TF_SIM,  0.40f, 2.00f),
    TF(half_wid,        TF_F32, TF_SIM,  0.20f, 2.00f),
    /* F2: the bed's shape. SIM, obviously — it is the wall a ball bounces off.
     * The notch is bounded by the bed itself and validated against it below,
     * because a bite deeper than the table is not a shape. */
    TF(bed_shape,       TF_I32, TF_SIM,  0.0f, 1.0f),
    /* SIM: which way the L turns is the wall a ball bounces off. */
    TF(bed_hand,        TF_I32, TF_SIM,  0.0f, 1.0f),
    TF(notch_x,         TF_F32, TF_SIM,  0.0f, 3.20f),
    TF(notch_z,         TF_F32, TF_SIM,  0.0f, 3.20f),
    /* The set. 34 mm is a Russian pyramid ball and 24 mm a small snooker one,
     * and those were the whole range — every ball anybody actually plays with
     * and nothing else. The workshop exists for the table nobody plays on, so
     * this reaches from 20 mm to 160 mm across. It stays honest because the
     * checks below are the real constraint: a ball that will not pass the
     * pockets, or that leaves no room to rack, is refused with a reason
     * whatever this range allows. */
    TF(R,               TF_F32, TF_SIM,  0.010f, 0.080f),
    TF(mass,            TF_F32, TF_SIM,  0.010f, 2.000f),
    /* Zero is legal and means "the same as the rest" — see CueTable. */
    TF(cue_R,           TF_F32, TF_SIM,  0.000f, 0.080f),
    TF(cue_mass,        TF_F32, TF_SIM,  0.000f, 2.000f),
    TF(cushion_h,       TF_F32, TF_SIM,  0.010f, 0.060f),
    TF(rail_w,          TF_F32, TF_SIM,  0.020f, 0.200f),
    TF(pocket_round,    TF_I32, TF_SIM,  0.0f, 1.0f),
    TF(pr_corner,       TF_F32, TF_SIM,  0.020f, 0.150f),
    TF(pr_side,         TF_F32, TF_SIM,  0.020f, 0.150f),
    TF(bore_corner,     TF_F32, TF_SIM,  0.020f, 0.200f),
    TF(bore_side,       TF_F32, TF_SIM,  0.020f, 0.200f),
    TF(bore_set_corner, TF_F32, TF_SIM, -0.050f, 0.100f),
    TF(bore_set_side,   TF_F32, TF_SIM, -0.050f, 0.100f),
    TF(gap_corner,      TF_F32, TF_SIM,  0.000f, 0.200f),
    TF(gap_side,        TF_F32, TF_SIM,  0.000f, 0.200f),
    TF(facing_len,      TF_F32, TF_SIM,  0.000f, 0.200f),
    TF(ang_corner,      TF_F32, TF_SIM,  0.0f, 90.0f),
    TF(ang_side,        TF_F32, TF_SIM,  0.0f, 90.0f),
    TF(off_corner,      TF_F32, TF_SIM,  0.000f, 0.150f),
    TF(off_side,        TF_F32, TF_SIM,  0.000f, 0.150f),
    TF(jaw_r,           TF_F32, TF_SIM,  0.000f, 0.050f),
    /* The rail. Livelier than 0.98 at a crawl is a trampoline; one that never
     * falls below 0.20 under pace is a dead cushion. The US tables ship at
     * 0.985, so the ceiling is above that: a range that excludes a table the
     * game already ships is the range being wrong, not the table. */
    TF(e_cush,          TF_F32, TF_SIM,  0.20f, 0.99f),
    TF(cush_efall,      TF_F32, TF_SIM,  0.00f, 0.50f),
    TF(e_cush_min,      TF_F32, TF_SIM,  0.10f, 0.99f),
    /* NEGATIVE IS LEGAL, and it has to be. The drop radius is (pr - cap), so a
     * positive cap makes the catch SMALLER than the mouth — right for a pocket
     * whose mouth is nearly twice the ball. Russian pyramid's mouth is 73 mm to
     * a 68 mm ball: a catch of at most 36.5 mm, centred outside the cushion
     * line, is one a ball cannot reach without being most of the way down the
     * throat, so balls sat on the lip and stayed up and no frame ever finished.
     *
     * The mouth and the catch are two different things: the mouth is what the
     * ball has to thread, and the catch is when it has gone. Tying the second
     * to be no bigger than the first was an assumption, not a rule. Widening a
     * range is backward-compatible on the wire — every value that used to pack
     * still validates. */
    TF(cap_corner,      TF_F32, TF_SIM, -0.060f, 0.060f),
    TF(cap_side,        TF_F32, TF_SIM, -0.060f, 0.060f),
    TF(drop_back,       TF_F32, TF_SIM,  0.000f, 0.100f),
    TF(drop_back_side,  TF_F32, TF_SIM,  0.000f, 0.100f),
    /* Snooker layout. Ignored for pool, but still part of the table. */
    TF(baulk_x,         TF_F32, TF_SIM, -2.00f, 2.00f),
    TF(d_radius,        TF_F32, TF_SIM,  0.000f, 0.600f),
    TF(house,           TF_I32, TF_SIM,  0,      1),
    TF(blue_x,          TF_F32, TF_SIM, -2.00f, 2.00f),
    TF(pink_x,          TF_F32, TF_SIM, -2.00f, 2.00f),
    TF(black_x,         TF_F32, TF_SIM, -2.00f, 2.00f),
    /* Cosmetics: packed, so a shared table arrives looking like itself; never
     * hashed, so two people who disagree about cloth colour can still play. */
    TF(cloth,           TF_U16, TF_LOOK, 0.0f, 65535.0f),
    TF(rail,            TF_U16, TF_LOOK, 0.0f, 65535.0f),
    TF(rail_top,        TF_U16, TF_LOOK, 0.0f, 65535.0f),
    TF(spot,            TF_U16, TF_LOOK, 0.0f, 65535.0f),
    TF(nballs,          TF_I32, TF_SIM,  2.0f, (float)CUE_MAX_BALLS),
};
#define TAB_NFIELD ((int)(sizeof TAB_FIELDS / sizeof TAB_FIELDS[0]))

int         cue_table_field_count(void) { return TAB_NFIELD; }
const char *cue_table_field_name(int i) {
    return (i >= 0 && i < TAB_NFIELD) ? TAB_FIELDS[i].name : "";
}

/* Read a field as a float whatever it is stored as, so the range check and the
 * hash do not each need a type switch of their own. */
static float tf_get(const CueTable *t, const CueTabField *f) {
    const unsigned char *p = (const unsigned char *)t + f->off;
    if (f->type == TF_F32) { float v; memcpy(&v, p, 4); return v; }
    if (f->type == TF_I32) { int   v; memcpy(&v, p, 4); return (float)v; }
    { unsigned short v; memcpy(&v, p, 2); return (float)v; }
}

static int tab_bytes(void) {
    int n = 4;
    for (int i = 0; i < TAB_NFIELD; i++)
        n += (TAB_FIELDS[i].type == TF_U16) ? 2 : 4;
    return n;
}

int cue_table_pack(const CueTable *t, unsigned char *out, int cap) {
    int need = tab_bytes();
    if (!t || !out || cap < need) return 0;
    out[0] = CUE_TABLE_SPEC_VERSION;
    out[1] = (unsigned char)TAB_NFIELD;   /* so a reader can refuse a short block */
    out[2] = out[3] = 0;
    int at = 4;
    for (int i = 0; i < TAB_NFIELD; i++) {
        const unsigned char *p = (const unsigned char *)t + TAB_FIELDS[i].off;
        if (TAB_FIELDS[i].type == TF_U16) {
            unsigned short v; memcpy(&v, p, 2);
            out[at++] = (unsigned char)v;
            out[at++] = (unsigned char)(v >> 8);
        } else {
            unsigned int v; memcpy(&v, p, 4);   /* float bits or int, verbatim */
            out[at++] = (unsigned char)v;
            out[at++] = (unsigned char)(v >> 8);
            out[at++] = (unsigned char)(v >> 16);
            out[at++] = (unsigned char)(v >> 24);
        }
    }
    return at;
}

int cue_table_unpack(CueTable *t, const unsigned char *in, int len) {
    if (!t || !in || len < 4) return 0;
    if (in[0] != CUE_TABLE_SPEC_VERSION) return 0;
    if (in[1] != (unsigned char)TAB_NFIELD) return 0;
    if (len < tab_bytes()) return 0;

    CueTable tmp;
    memset(&tmp, 0, sizeof tmp);
    int at = 4;
    for (int i = 0; i < TAB_NFIELD; i++) {
        unsigned char *p = (unsigned char *)&tmp + TAB_FIELDS[i].off;
        if (TAB_FIELDS[i].type == TF_U16) {
            unsigned short v = (unsigned short)(in[at] | (in[at+1] << 8));
            at += 2; memcpy(p, &v, 2);
        } else {
            unsigned int v = (unsigned int)in[at] | ((unsigned int)in[at+1] << 8)
                           | ((unsigned int)in[at+2] << 16) | ((unsigned int)in[at+3] << 24);
            at += 4; memcpy(p, &v, 4);
        }
    }
    /* The boundary is where refusal belongs: a spec that cannot be played must
     * not become the table and fail somewhere further in, where the reason for
     * it is no longer to hand. */
    if (!cue_table_validate(&tmp, 0, 0)) return 0;
    *t = tmp;
    return 1;
}

uint32_t cue_table_hash(const CueTable *t) {
    unsigned int h = 2166136261u;             /* FNV-1a */
    if (!t) return 0;
    for (int i = 0; i < TAB_NFIELD; i++) {
        if (!TAB_FIELDS[i].sim) continue;     /* a colour moves no ball */
        const unsigned char *p = (const unsigned char *)t + TAB_FIELDS[i].off;
        int n = (TAB_FIELDS[i].type == TF_U16) ? 2 : 4;
        for (int b = 0; b < n; b++) { h ^= p[b]; h *= 16777619u; }
    }
    return (uint32_t)h;
}

static int tab_fail(char *msg, int cap, const char *why) {
    if (msg && cap > 0) {
        int i = 0;
        while (why[i] && i < cap - 1) { msg[i] = why[i]; i++; }
        msg[i] = 0;
    }
    return 0;
}

int cue_table_validate(const CueTable *t, char *msg, int msgcap) {
    if (!t) return tab_fail(msg, msgcap, "there is no table");
    if (msg && msgcap > 0) msg[0] = 0;

    /* Every field inside its own range. A NaN fails both comparisons, which is
     * exactly right: a NaN anywhere in the geometry is an unplayable table. */
    for (int i = 0; i < TAB_NFIELD; i++) {
        float v = tf_get(t, &TAB_FIELDS[i]);
        if (!(v >= TAB_FIELDS[i].lo && v <= TAB_FIELDS[i].hi)) {
            char buf[96];
            snprintf(buf, sizeof buf, "%s is %.4f, outside %.3f..%.3f",
                     TAB_FIELDS[i].name, (double)v,
                     (double)TAB_FIELDS[i].lo, (double)TAB_FIELDS[i].hi);
            return tab_fail(msg, msgcap, buf);
        }
    }

    /* ...and then the combinations: pairs of individually sensible numbers that
     * together make a table nobody can play. */
    float R  = t->R;
    float cR = (t->cue_R > 0.0f) ? t->cue_R : R;
    float big = (cR > R) ? cR : R;            /* the widest ball on the cloth */

    if (t->half_wid > t->half_len)
        return tab_fail(msg, msgcap, "the table is wider than it is long");
    if (t->half_wid < big * 4.0f)
        return tab_fail(msg, msgcap, "the bed is too narrow for the balls on it");

    /* F2: AND THE SHAPE HAS TO BE A SHAPE.
     *
     * The notch is two individually sensible numbers that together make
     * nonsense far more easily than any pocket dimension does — a bite as deep
     * as the table leaves no table, and one a ball's width from an edge leaves
     * a channel nothing can be played down. Both look perfectly reasonable in
     * their own row, which is exactly the class of fault this section is for. */
    if (t->bed_shape == CUE_BED_L) {
        if (t->notch_x <= 0.0f || t->notch_z <= 0.0f)
            return tab_fail(msg, msgcap, "an L-shaped bed needs a notch with two sides");
        /* What is left of the short leg, and of the arm beside it. Four balls
         * wide is the same floor the whole bed is held to above — a leg
         * narrower than that is not a leg, it is a gutter. */
        float leg_x = 2.0f * t->half_len - t->notch_x;   /* the band below */
        float leg_z = 2.0f * t->half_wid - t->notch_z;   /* the column beside */
        if (leg_z < big * 8.0f)
            return tab_fail(msg, msgcap, "the notch is so deep the long arm is a gutter");
        if (leg_x < big * 8.0f)
            return tab_fail(msg, msgcap, "the notch is so wide the short arm is a gutter");
        /* And the notch has to take a bite worth having. A sliver is a
         * rectangle with a defect in it, and it puts two cushions and a reflex
         * corner within a ball of each other. */
        if (t->notch_x < big * 4.0f || t->notch_z < big * 4.0f)
            return tab_fail(msg, msgcap, "the notch is too small to be a corner");
    } else if (t->notch_x != 0.0f || t->notch_z != 0.0f) {
        return tab_fail(msg, msgcap, "a rectangular bed cannot have a notch");
    }

    /* A pocket has to admit a ball, and so does the DROP — they are different
     * radii and the drop is the smaller. This is the check the workshop exists
     * for: one thumbstick movement can express a pocket a ball will not fit. */
    if (t->pr_corner <= big) return tab_fail(msg, msgcap, "the corner pocket is narrower than the ball");
    if (t->pr_side   <= big) return tab_fail(msg, msgcap, "the middle pocket is narrower than the ball");
    if (t->pr_corner - t->cap_corner <= big)
        return tab_fail(msg, msgcap, "the corner drop is too small to take the ball");
    if (t->pr_side - t->cap_side <= big)
        return tab_fail(msg, msgcap, "the middle drop is too small to take the ball");

    /* The bore is the hole cut in the timber. Too small for the mouth it serves
     * and there is a slot beside the cushion you can see out of the table
     * through — which is the fault the bore was made dialable to close. */
    if (t->bore_corner < t->pr_corner * 0.70f)
        return tab_fail(msg, msgcap, "the corner bore is too small for the mouth it serves");
    if (t->bore_side < t->pr_side * 0.70f)
        return tab_fail(msg, msgcap, "the middle bore is too small for the mouth it serves");

    /* A real rail is livelier the more gently it is touched. Inverting that
     * gives a cushion that gains energy the harder it is hit. */
    if (t->e_cush_min > t->e_cush)
        return tab_fail(msg, msgcap, "the cushion gets livelier the harder it is hit");

    /* The nose is a fraction of a ball up the front face. At the ball's centre
     * or above it cannot be struck properly; at the cloth there is no cushion. */
    if (t->cushion_h >= big * 2.0f || t->cushion_h <= big * 0.2f)
        return tab_fail(msg, msgcap, "the cushion is the wrong height for the ball");

    /* Snooker spots have to be on the table, and in their order down it. */
    if (t->is_snooker) {
        if (t->baulk_x < -t->half_len || t->black_x > t->half_len)
            return tab_fail(msg, msgcap, "a spot is off the end of the table");
        if (!(t->baulk_x < t->blue_x && t->blue_x < t->pink_x && t->pink_x < t->black_x))
            return tab_fail(msg, msgcap, "the spots are out of order down the table");
        if (t->d_radius > t->half_wid)
            return tab_fail(msg, msgcap, "the D is wider than the table");
    }
    return 1;
}

/* Inward unit normal of segment a→b. The cushion boundary is built as one
 * closed loop traversed in a consistent sense (top rail +x, right rail +z, …),
 * so the inward normal is simply the edge direction rotated +90°: (-dz, dx).
 * (The earlier "point toward the origin" heuristic flipped corner-FACING
 * segments the wrong way — facings splay outward past the rail line so their
 * midpoint-to-centre direction is not the surface normal. That single flip
 * spiked the render jaws AND gave balls a wrong cushion normal off the jaws.) */
static Vec3 inward_n(float ax, float az, float bx, float bz) {
    float dx = bx - ax, dz = bz - az;
    return v3_norm(v3(-dz, 0, dx));
}
static void add_seg(CueWorld *w, Vec3 a, Vec3 b, uint8_t kind) {
    if (w->nseg >= CUE_MAX_SEG) return;
    CueSeg *s = &w->seg[w->nseg++];
    s->a = v3(a.x, w->R, a.z);
    s->b = v3(b.x, w->R, b.z);
    s->n = inward_n(a.x, a.z, b.x, b.z);
    s->kind = kind;
}
static void add_jaw(CueWorld *w, Vec3 k) {
    if (w->njaw >= CUE_MAX_SEG) return;
    w->jaw[w->njaw++] = v3(k.x, w->R, k.z);
}
static void add_pocket(CueWorld *w, float x, float z, float cap, int mid) {
    if (w->npocket >= CUE_MAX_POCKET) return;
    int i = w->npocket++;
    w->pocket[i] = v3(x, 0, z);
    w->pocket_r[i] = cap;
    w->pocket_mid[i] = (unsigned char)(mid ? 1 : 0);
}

/* Straight cushion chain (US pool): facing-tip → knuckle → knuckle →
 * facing-tip. P2,P3 are pushed into w->jaw in boundary order so the renderer
 * can fan the bed off them. */
/* Recess a jaw (rattle) circle so its PLAYABLE edge sits flush with the rail
 * nose instead of poking past it: shift the centre outward (−nose inward normal)
 * by its radius + a small margin. A ball hugging the rail then clears it
 * cleanly; a ball entering the pocket mouth still rattles. */
static void add_jaw_recessed(CueWorld *w, Vec3 k, Vec3 nin) {
    float off = w->jaw_r + 0.15f * w->R;
    add_jaw(w, v3(k.x - nin.x * off, 0, k.z - nin.z * off));
}
static void add_chain(CueWorld *w, Vec3 P1, Vec3 P2, Vec3 P3, Vec3 P4) {
    add_seg(w, P1, P2, 1);
    add_seg(w, P2, P3, 0);
    add_seg(w, P3, P4, 1);
    Vec3 nin = inward_n(P2.x, P2.z, P3.x, P3.z);   /* nose inward normal */
    add_jaw_recessed(w, P2, nin);
    add_jaw_recessed(w, P3, nin);
}

/* THE REFLEX CORNER AS A RADIUS RATHER THAN A POINT.
 *
 * The elbow was two cushions meeting at a right angle with a rattle circle
 * dropped on the vertex to stop a ball squeezing through the join. That circle
 * does the physics and nothing else: the DRAWN corner is still a knife edge, and
 * a knife edge is not a thing a table can have — every real inside corner is a
 * radius, because that is what a cushion rubber will bend to and what the timber
 * behind it is cut to.
 *
 * So the corner is rounded in the geometry, like the jaw-to-rail junctions above
 * and for the same reason: a normal is not a silhouette, and in a headset you
 * are looking straight down the rail at a lit edge.
 *
 * The arc is tangent to both faces, which fixes its centre completely — one
 * radius along each face's outward normal from the vertex, the only point a
 * circle of radius r can sit and touch both. Nothing here knows which corner of
 * which shape it is; it is given two normals and it turns between them, so the
 * next shape's reflex corners need no new code.
 *
 * It gives a sliver of cloth BACK at the elbow, about 0.4 r deep at the deepest,
 * which cue_table_bed_rects still calls off-bed. That is fine and deliberately
 * not chased: 0.4 r is smaller than a ball's radius, so no ball's CENTRE can
 * ever be in the sliver, and the centre is what the containment test is asked
 * about. */
static void add_elbow(CueWorld *w, Vec3 v, Vec3 na, Vec3 nb, float r) {
    if (r <= 1e-5f) { add_jaw(w, v); return; }
    const Vec3 c  = v3(v.x + (na.x + nb.x) * r, 0, v.z + (na.z + nb.z) * r);
    const Vec3 a0 = v3(c.x - na.x * r, 0, c.z - na.z * r);
    const Vec3 a1 = v3(c.x - nb.x * r, 0, c.z - nb.z * r);
    float s0 = atan2f(a0.z - c.z, a0.x - c.x);
    float s1 = atan2f(a1.z - c.z, a1.x - c.x);
    float d  = s1 - s0;                          /* the short way round */
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    int n = CUE_JAW_SEGS;
    if (n < 3) n = 3;
    Vec3 prev = a0;
    for (int i = 1; i <= n; i++) {
        float a = s0 + d * (float)i / (float)n;
        Vec3 p = v3(c.x + r * cosf(a), 0, c.z + r * sinf(a));
        /* kind 1, so the vertex smoothing averages ALONG the arc and leaves the
         * junctions with the two straight noses crisp — the same rule the bezier
         * jaws are built under. */
        add_seg(w, prev, p, 1);
        prev = p;
    }
}

/* The rounded jaw, built below with the rest of the bezier machinery. The L
 * needs it and is written above it, because the L's own commentary belongs
 * beside add_run. */
static void add_curved_chain_e(CueWorld *w, Vec3 tipIn, Vec3 kIn, Vec3 kMid,
                               Vec3 tipMid, float aIn, float aOut,
                               int nIn, int nOut, int jawIn, int jawMid);
static void jaw_tip(const CueWorld *w, Vec3 k, Vec3 u, Vec3 out, int mid,
                    Vec3 *tip, float *arc);

/* ---- S1: THE L-SHAPED BED ------------------------------------------------
 *
 * An L is six vertices: five that turn the way every cushion here has ever
 * turned, and ONE THAT TURNS THE OTHER WAY — the reflex corner, pointing into
 * the playing area. That vertex is the whole difficulty of the shape.
 *
 * Two things go wrong there and both are handled below rather than hoped about.
 *
 * The vertex-averaged normals that keep the chain smooth are meaningless across
 * a 270 degree turn: averaging the two faces gives a normal pointing into the
 * timber, and a ball arriving on it is pushed the wrong side of the wall. The
 * existing smoothing already refuses to average across a sharp corner — the
 * SMOOTH_COS test — and the reflex corner is a right angle, so it is refused by
 * the same rule that refuses a US mitre. It is worth saying explicitly because
 * it is the one place where that guard is load-bearing rather than tidy.
 *
 * And a knife-edge corner is not a thing a table can have. A real one is a
 * rounded nose, and the engine already has exactly that in the jaw circle — an
 * immovable circle a ball rebounds from. One at the reflex vertex gives the
 * corner a radius, stops a ball squeezing through the join between the two
 * segments, and is what the woodwork would actually look like.
 *
 * The pockets are cut the way the TABLE'S OWN pocket_round says — a mitre on an
 * American bed, a bezier knuckle on a UK or snooker one. These were mitred
 * either way at first, on the reasoning that an L is a novelty and the mitre is
 * the simpler jaw. That is a difference you can SEE, because cue_render cuts the
 * timber behind a pocket to the jaw it is given: a straight facing leaving the
 * knuckle at ang_side does not arrive where a rounded pocket back is, so there
 * is a slot beside the cushion at every pocket on a UK L — which is exactly what
 * it was reported as. The reflex corner needs none of that machinery: it has no
 * pocket, so it has no jaw and no rattle circle, and add_curved_chain_e takes
 * that as an end treatment rather than having it worked around. */

/* One straight rail, from `a` to `b`, with an end treatment at each end.
 * `out` is the outward unit normal — away from the cloth. */
enum { LEND_CORNER = 0, LEND_MIDDLE = 1, LEND_REFLEX = 2 };

static void add_run(CueWorld *w, const CueTable *t, Vec3 a, Vec3 b, Vec3 out,
                    int end_a, int end_b) {
    float dx = b.x - a.x, dz = b.z - a.z;
    float len = sqrtf(dx*dx + dz*dz);
    if (len < 1e-5f) return;
    Vec3 d = v3(dx/len, 0, dz/len);
    const float sl = t->facing_len;
    const float ga = (end_a == LEND_REFLEX) ? 0.0f
                   : (end_a == LEND_MIDDLE) ? t->gap_side : t->gap_corner;
    const float gb = (end_b == LEND_REFLEX) ? 0.0f
                   : (end_b == LEND_MIDDLE) ? t->gap_side : t->gap_corner;
    /* A gap at each end has to leave a nose between them. Two pockets closer
     * together than their own mouths is a rail that does not exist, and it
     * would be emitted as a segment pointing backwards — an inward normal
     * facing out, and a cushion that sucks balls through it. */
    if (ga + gb >= len - 1e-4f) return;

    Vec3 P2 = v3(a.x + d.x*ga, 0, a.z + d.z*ga);
    Vec3 P3 = v3(b.x - d.x*gb, 0, b.z - d.z*gb);
    Vec3 nin = inward_n(P2.x, P2.z, P3.x, P3.z);

    /* A ROUNDED TABLE GETS ROUNDED JAWS HERE TOO.
     *
     * This used to mitre the L's pockets straight whatever pocket_round said,
     * which was written down as a deliberate simplification and is the reported
     * fault: on a UK table — the one the L is actually played on — every other
     * pocket on every other table is a bezier knuckle, and cue_render cuts the
     * timber behind a pocket to match the jaw it is given. A straight facing
     * leaving the knuckle at ang_side does not arrive where a rounded pocket
     * back is, so there is a slot beside the cushion. Same jaw, same numbers,
     * same code path as the six rails a rectangle is built from. */
    if (t->pocket_round) {
        Vec3 T1 = P2, T4 = P3;
        float aIn = 0.6f, aOut = 0.6f;
        int nIn = 0, nOut = 0;
        if (end_a != LEND_REFLEX) {
            /* `d` runs a → b, so into the rail from this end */
            jaw_tip(w, P2, d, out, end_a == LEND_MIDDLE, &T1, &aIn);
            nIn = CUE_JAW_SEGS;
        }
        if (end_b != LEND_REFLEX) {
            jaw_tip(w, P3, v3(-d.x, 0, -d.z), out, end_b == LEND_MIDDLE, &T4, &aOut);
            nOut = CUE_JAW_SEGS;
        }
        add_curved_chain_e(w, T1, P2, P3, T4, aIn, aOut, nIn, nOut,
                           end_a != LEND_REFLEX, end_b != LEND_REFLEX);
        return;
    }

    /* IN BOUNDARY ORDER — facing, nose, facing — and it has to be.
     *
     * The renderer works out which cushion pieces are joined by testing whether
     * one segment's b is the next segment's a, walking the array in order. It
     * is not a search: it looks at s-1 and s+1 and nothing else. Emitting the
     * nose first and then its two facings, which is the order that reads
     * naturally here, leaves every facing looking FREE AT BOTH ENDS — so each
     * one was run out to the rail on an assumption about which end was the
     * knuckle that was simply wrong, and none of them shared vertices with the
     * nose it belongs to. That is the wedge of cushion hanging below the timber
     * at each pocket. add_chain has always emitted in this order; this is the
     * same contract, and it is a contract rather than a preference. */
    if (end_a != LEND_REFLEX) {
        float ang = (end_a == LEND_MIDDLE) ? t->ang_side : t->ang_corner;
        float c = cosf(ang*DEG), s = sinf(ang*DEG);
        Vec3 P1 = v3(P2.x - d.x*(c*sl) + out.x*(s*sl), 0,
                     P2.z - d.z*(c*sl) + out.z*(s*sl));
        add_seg(w, P1, P2, 1);
    }
    add_seg(w, P2, P3, 0);                       /* the nose */
    if (end_b != LEND_REFLEX) {
        float ang = (end_b == LEND_MIDDLE) ? t->ang_side : t->ang_corner;
        float c = cosf(ang*DEG), s = sinf(ang*DEG);
        Vec3 P4 = v3(P3.x + d.x*(c*sl) + out.x*(s*sl), 0,
                     P3.z + d.z*(c*sl) + out.z*(s*sl));
        add_seg(w, P3, P4, 1);
    }
    /* the jaw circles after, so they do not interleave with the chain */
    if (end_a != LEND_REFLEX) add_jaw_recessed(w, P2, nin);
    if (end_b != LEND_REFLEX) add_jaw_recessed(w, P3, nin);
}

/* The whole L: six vertices, seven pockets, and a jaw circle on the reflex. */
/* A LEFT-HANDED L IS THE MIRROR IMAGE OF A RIGHT-HANDED ONE, so it is built as
 * one and reflected, rather than written out a second time with the signs
 * changed. Every L-shaped bug in this file has been a fact about the shape
 * hidden in a coordinate sign; a second copy of the outline would be eleven more
 * places for the sixth one to hide.
 *
 * Mirroring z REVERSES the winding, and the winding is what makes every inward
 * normal point at the cloth — so the chain is walked backwards as well, which
 * puts it back. Each segment therefore swaps its own two ends, and the array is
 * reversed so that seg[s-1].b is still seg[s].a: the renderer finds its
 * neighbours by walking the array in order and nothing else. The pockets are
 * reversed for the same reason — build_bed_boundary_L takes them in the order
 * the boundary meets them.
 *
 * The jaw circles need only their z: a circle has no winding. */
static void mirror_world_z(CueWorld *w) {
    for (int i = 0, j = w->nseg - 1; i < j; i++, j--) {
        CueSeg s = w->seg[i]; w->seg[i] = w->seg[j]; w->seg[j] = s;
    }
    for (int i = 0; i < w->nseg; i++) {
        Vec3 a = w->seg[i].a, b = w->seg[i].b;
        w->seg[i].a = v3(b.x, b.y, -b.z);
        w->seg[i].b = v3(a.x, a.y, -a.z);
        w->seg[i].n = inward_n(w->seg[i].a.x, w->seg[i].a.z,
                               w->seg[i].b.x, w->seg[i].b.z);
    }
    for (int i = 0; i < w->njaw; i++) w->jaw[i].z = -w->jaw[i].z;
    for (int i = 0, j = w->npocket - 1; i < j; i++, j--) {
        Vec3 p = w->pocket[i];            w->pocket[i] = w->pocket[j];         w->pocket[j] = p;
        float r = w->pocket_r[i];         w->pocket_r[i] = w->pocket_r[j];     w->pocket_r[j] = r;
        unsigned char m = w->pocket_mid[i];
        w->pocket_mid[i] = w->pocket_mid[j]; w->pocket_mid[j] = m;
    }
    for (int i = 0; i < w->npocket; i++) w->pocket[i].z = -w->pocket[i].z;
}

static void build_L(CueWorld *w, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid;
    const float nx = t->notch_x, nz = t->notch_z;

    const Vec3 V0 = v3(-hl,      0, -hw);
    const Vec3 V1 = v3( hl,      0, -hw);
    const Vec3 V2 = v3( hl,      0,  hw - nz);
    const Vec3 V3 = v3( hl - nx, 0,  hw - nz);      /* the reflex corner */
    const Vec3 V4 = v3( hl - nx, 0,  hw);
    const Vec3 V5 = v3(-hl,      0,  hw);
    /* THE MIDDLES GO ON THE TWO OUTER RAILS, and only those.
     *
     * On a rectangle the long rails are the two of length 2*half_len and it is
     * obvious which they are. On an L the two rails that run the WHOLE way are
     * the outside of the two arms — here the one at z = -half_wid and the one
     * at x = -half_len — and the other four are all shortened by the notch. A
     * middle on a shortened rail is a pocket halfway along an arm that is not
     * long enough to want one, sitting a few inches from a corner pocket, which
     * is what putting one on the short leg's top rail produced. */
    const Vec3 M0 = v3(0.0f, 0, -hw);    /* the long arm's outer rail */
    const Vec3 M1 = v3(-hl,  0,  0.0f);  /* the short arm's outer rail */

    const Vec3 OUT_Z0 = v3(0,0,-1), OUT_Z1 = v3(0,0,1);
    const Vec3 OUT_X1 = v3(1,0,0),  OUT_X0 = v3(-1,0,0);

    /* Round the outline, in the order that makes every inward normal point at
     * the cloth: V0 -> V1 -> V2 -> V3 -> V4 -> V5 -> V0. */
    add_run(w, t, V0, M0, OUT_Z0, LEND_CORNER, LEND_MIDDLE);   /* outer rail, half */
    add_run(w, t, M0, V1, OUT_Z0, LEND_MIDDLE, LEND_CORNER);   /* ...and the rest */
    add_run(w, t, V1, V2, OUT_X1, LEND_CORNER, LEND_CORNER);   /* the far end */
    /* OUT_Z1, not OUT_Z0: the rail under the notch has the cloth below it and
     * the missing corner above, so its outside is +z. Facing it the other way
     * splays this rail's pocket facings back INTO the playing area — a wedge of
     * cushion standing on the cloth beside the pocket, which is what it drew.
     * The nose is unaffected either way, because add_seg takes the inward
     * normal from the chain's own direction; only the facings read this. */
    /* THE ELBOW'S RADIUS. Both runs stop short of the vertex by it and the arc
     * joins them, so the chain is continuous and the corner is a curve rather
     * than a point. A ball and a half across is what a cushion bends to; it is
     * clamped so a shallow notch cannot ask for a radius longer than the runs
     * it has to be taken out of. */
    float er = 1.5f * t->R;
    {   float lim = 0.35f * (nx < nz ? nx : nz);
        if (er > lim) er = lim;
        if (er < 0.0f) er = 0.0f; }
    add_run(w, t, V2, v3(V3.x + er, 0, V3.z), OUT_Z1,
            LEND_CORNER, LEND_REFLEX);                         /* under the notch */
    add_elbow(w, V3, OUT_Z1, OUT_X1, er);                      /* ...round the corner */
    add_run(w, t, v3(V3.x, 0, V3.z + er), V4, OUT_X1,
            LEND_REFLEX, LEND_CORNER);                         /* beside the notch */
    add_run(w, t, V4, V5, OUT_Z1, LEND_CORNER, LEND_CORNER);   /* the short arm's top */
    add_run(w, t, V5, M1, OUT_X0, LEND_CORNER, LEND_MIDDLE);   /* the other outer rail */
    add_run(w, t, M1, V0, OUT_X0, LEND_MIDDLE, LEND_CORNER);

    /* The elbow needed a rattle circle dropped on the vertex, because two
     * cushions meeting at a knife edge leave a join a ball can squeeze through.
     * add_elbow above turns the corner into a real arc of shared-endpoint
     * segments instead, so there is no join left to squeeze through and no
     * circle needed — it keeps one only when the radius has been clamped away to
     * nothing, which a notch a few millimetres deep would do. */

    const float dg = 0.70710678f, oc = t->off_corner, os = t->off_side;
    const float capc = t->pr_corner - t->cap_corner;
    const float caps = t->pr_side   - t->cap_side;
    /* THE FIVE OUTER CORNERS, each pushed out along the bisector of its own two
     * outward normals — which is NOT always away from the table centre.
     *
     * V2 is the corner where the right rail meets the underside of the notch.
     * The cloth there lies to -x and -z of it, so the pocket belongs pushed
     * +x AND +z, out into the notch. Reading the direction off the sign of the
     * coordinate instead — which is what every rectangle here can get away with
     * — put it at +x,-z, back INSIDE the playing area, where it drew a facing
     * jutting into the cloth and offered a pocket in the middle of the table.
     *
     * There is deliberately NO pocket at the elbow: on a real L the inside
     * corner is solid timber, and the two cushions meet there.
     *
     * IN OUTLINE ORDER, not corners-then-middles. The cloth boundary is a walk
     * round the table and it wants the pockets in the order it meets them; a
     * rectangle can sort six pockets back into order from their coordinates,
     * and an L cannot. See build_bed_boundary_L, which relies on this. */
    add_pocket(w, V0.x - oc*dg, V0.z - oc*dg, capc, 0);   /* 0 */
    add_pocket(w, M0.x,         M0.z - os,    caps, 1);   /* 1 */
    add_pocket(w, V1.x + oc*dg, V1.z - oc*dg, capc, 0);   /* 2 */
    add_pocket(w, V2.x + oc*dg, V2.z + oc*dg, capc, 0);   /* 3 */
    add_pocket(w, V4.x + oc*dg, V4.z + oc*dg, capc, 0);   /* 4 */
    add_pocket(w, V5.x - oc*dg, V5.z + oc*dg, capc, 0);   /* 5 */
    add_pocket(w, M1.x - os,    M1.z,         caps, 1);   /* 6 */

    /* ...and if this table turns the other way, that was the right-handed L and
     * this is its reflection. */
    if (cue_table_hand(t) < 0.0f) mirror_world_z(w);
}

/* Sample nseg+1 points along a quadratic-bezier curve from s to e with a
 * perpendicular bulge (matches the 2D game's generateCurvePoints), inclusive of
 * both ends. Returns the point count. */
static int curve_pts(Vec3 s, Vec3 e, float dir, int nseg, Vec3 *out) {
    float dx = e.x - s.x, dz = e.z - s.z;
    float len = sqrtf(dx*dx + dz*dz);
    out[0] = s;
    if (len < 1e-6f || nseg < 1) return 1;
    float px = -dz/len, pz = dx/len, depth = len * 0.4f * dir;
    float cx = (s.x+e.x)*0.5f + px*depth, cz = (s.z+e.z)*0.5f + pz*depth;
    for (int i = 1; i <= nseg; i++) {
        float t = (float)i/nseg, o = 1.0f - t;
        out[i] = v3(o*o*s.x + 2*o*t*cx + t*t*e.x, 0,
                    o*o*s.z + 2*o*t*cz + t*t*e.z);
    }
    return nseg + 1;
}

/* Append a polyline as facing segments. */
static void add_poly(CueWorld *w, const Vec3 *p, int n, uint8_t kind) {
    for (int i = 1; i < n; i++) add_seg(w, p[i-1], p[i], kind);
}

/* ---- THE JAW-TO-RAIL BLEND ----------------------------------------------- *
 *
 * WHERE THE CURVE MEETS THE STRAIGHT, and why it needed saying at all.
 *
 * The chain is bezier jaw → straight nose → bezier jaw, and at each junction the
 * curve's tangent and the rail's direction are simply different. That is a
 * corner. The normal field papers over it — the vertex-averaging below gives a
 * continuous normal along the jaw run — but a normal is not a silhouette: in a
 * headset you are looking down the rail at a lit edge, and the geometry creases
 * however smoothly it is shaded.
 *
 * So the corner is rounded in the GEOMETRY, over a length measured in ball
 * radii rather than in segments. Two things follow from doing it that way:
 *
 *   It is cheaper the longer it gets. The blend CONSUMES the curve points it
 *   spans, so a longer blend replaces more vertices than it inserts. Rounding
 *   over a segment or two — which is what trimming by a fraction of the
 *   neighbouring segment amounts to — crams all the curvature into a couple of
 *   millimetres and still reads as a corner, because it IS one, just a smaller
 *   one.
 *
 *   It stays kind=1, and so changes no physics. A quadratic through
 *   P → (control at the old corner) → Q leaves P along the chord into the corner
 *   and arrives at Q ALONG THE RAIL. Tangent-continuous at both ends: the normal
 *   step across the junction is now a fraction of a degree, so the rail keeps
 *   its own crisp normal (the run-kind rule below is untouched, and with it the
 *   protection against a facing tilting the rail's normal into the throat) and
 *   gets a smooth silhouette anyway.
 *
 * And because the blend is applied HERE, to the collision chain, the drawn
 * cushion and the played cushion are one line by construction — cue_render
 * builds its mesh from w->seg. There is no second copy to drift. */
#ifndef CUE_JAW_BLEND
#define CUE_JAW_BLEND 0.30f      /* blend half-length, in ball radii */
#endif
#ifndef CUE_JAW_BLEND_PTS
#define CUE_JAW_BLEND_PTS 2      /* interior vertices inserted across it */
#endif

/* Walk `n` points from index `from` toward index `to` until `len` of arc has
 * been used. Returns the index landed on, and writes the exact cut point. */
static int walk_back(const Vec3 *p, int from, int to, float len, Vec3 *cut) {
    int step = (to > from) ? 1 : -1;
    float used = 0.0f;
    int i = from;
    while (i != to) {
        int j = i + step;
        float d = sqrtf((p[j].x-p[i].x)*(p[j].x-p[i].x) + (p[j].z-p[i].z)*(p[j].z-p[i].z));
        if (used + d >= len) {
            float f = (len - used) / (d > 1e-9f ? d : 1.0f);
            *cut = v3(p[i].x + (p[j].x-p[i].x)*f, 0, p[i].z + (p[j].z-p[i].z)*f);
            return j;   /* p[j] and everything past it toward `to` is consumed */
        }
        used += d; i = j;
    }
    *cut = p[to];
    return to;
}

/* The blend arc itself: P → corner → Q as a quadratic, interior points only. */
static void blend_arc(CueWorld *w, Vec3 P, Vec3 corner, Vec3 Q) {
    Vec3 prev = P;
    for (int i = 1; i <= CUE_JAW_BLEND_PTS + 1; i++) {
        float t = (float)i / (CUE_JAW_BLEND_PTS + 1), o = 1.0f - t;
        Vec3 p = v3(o*o*P.x + 2*o*t*corner.x + t*t*Q.x, 0,
                    o*o*P.z + 2*o*t*corner.z + t*t*Q.z);
        add_seg(w, prev, p, 1);
        prev = p;
    }
}

/* Curved cushion chain (snooker/UK): bezier jaw → straight nose → bezier jaw.
 * The curves bulge into the pocket, giving rounded knuckles, and each junction
 * with the nose is rounded off per the note above. */
/* `nIn`/`nOut` of 0 means THAT END HAS NO JAW — the rail simply begins at the
 * knuckle point. A rectangle never needs it: both ends of all six rails run into
 * a pocket. An L's reflex elbow is the case that does, and `jawIn`/`jawMid` go
 * with it, because a rattle circle belongs to a pocket and the elbow has none.
 *
 * The two ends were also blended as a pair — either both junctions were rounded
 * or neither was — which is the same answer whenever both ends carry a jaw, and
 * every rectangular rail does, so no shipped table moves by a micron. Per-end is
 * what a rail with a jaw at one end only needs. */
static void add_curved_chain_e(CueWorld *w, Vec3 tipIn, Vec3 kIn, Vec3 kMid,
                               Vec3 tipMid, float aIn, float aOut,
                               int nIn, int nOut, int jawIn, int jawMid) {
    /* Sized for the jaw itself, not for CUE_MAX_SEG: these are stack arrays and
     * the device has very little of it. CUE_JAW_SEGS is 3 there and 10 in VR. */
    #define CUE_JAW_MAXPTS 34
    Vec3 in[CUE_JAW_MAXPTS], out[CUE_JAW_MAXPTS];
    if (nIn  > CUE_JAW_MAXPTS - 2) nIn  = CUE_JAW_MAXPTS - 2;
    if (nOut > CUE_JAW_MAXPTS - 2) nOut = CUE_JAW_MAXPTS - 2;
    int ni = nIn  > 0 ? curve_pts(tipIn, kIn,    aIn,  nIn,  in)  : 0;
    int no = nOut > 0 ? curve_pts(kMid,  tipMid, aOut, nOut, out) : 0;

    float railLen = sqrtf((kMid.x-kIn.x)*(kMid.x-kIn.x) + (kMid.z-kIn.z)*(kMid.z-kIn.z));
    float L = CUE_JAW_BLEND * w->R;
    /* Never eat more than the rail can spare, nor a whole jaw curve. */
    if (L > railLen * 0.45f) L = railLen * 0.45f;
    if (railLen < 1e-5f) return;
    Vec3 rd = v3_norm(v3_sub(kMid, kIn));
    /* BOTH ends of the nose are settled before ANY of it is emitted. Working
     * them out as the chain is written looks equivalent and is not: the far
     * knuckle's blend starts where the nose STOPS, so a nose emitted before its
     * far end is known runs all the way to the knuckle and the blend after it
     * collapses to a point. That is a jaw with no curve on its outgoing side —
     * invisible in a picture of the table and worth 500 balls a run lost through
     * the cushions in test_edge, which is how it was caught. */
    const int blend_in  = (L > 1e-5f && ni > 1);
    const int blend_out = (L > 1e-5f && no > 1);
    Vec3 Q1 = blend_in  ? v3(kIn.x  + rd.x * L, 0, kIn.z  + rd.z * L) : kIn;
    Vec3 Q2 = blend_out ? v3(kMid.x - rd.x * L, 0, kMid.z - rd.z * L) : kMid;

    if (blend_in) {
        Vec3 P1;
        int c1 = walk_back(in, ni - 1, 0, L, &P1);   /* back along the incoming curve */
        add_poly(w, in, c1 + 1, 1);             /* curve, up to the last kept point */
        add_seg(w, in[c1], P1, 1);              /* ...then the part-segment to the cut */
        blend_arc(w, P1, kIn, Q1);              /* rounded junction */
    } else if (ni > 1) {
        add_poly(w, in, ni, 1);
    }
    add_seg(w, Q1, Q2, 0);                      /* the rail nose */
    if (blend_out) {
        Vec3 P2;
        int c2 = walk_back(out, 0, no - 1, L, &P2);  /* on along the outgoing curve */
        blend_arc(w, Q2, kMid, P2);
        add_seg(w, P2, out[c2], 1);
        add_poly(w, out + c2, no - c2, 1);      /* curve, on to the tip */
    } else if (no > 1) {
        add_poly(w, out, no, 1);
    }

    Vec3 nin = inward_n(kIn.x, kIn.z, kMid.x, kMid.z);   /* nose inward normal */
    if (jawIn)  add_jaw_recessed(w, kIn, nin);
    if (jawMid) add_jaw_recessed(w, kMid, nin);
}

/* Curved cushion chain (snooker/UK): bezier jaw → straight nose → bezier jaw. */
static void add_curved_chain(CueWorld *w, Vec3 tipIn, Vec3 kIn, Vec3 kMid,
                             Vec3 tipMid, float aIn, float aOut, int nIn, int nOut) {
    add_curved_chain_e(w, tipIn, kIn, kMid, tipMid, aIn, aOut, nIn, nOut, 1, 1);
}

/* THE JAW A POCKET END IS CUT WITH, in the rail's own frame.
 *
 * The six rectangular rails above spell each of their twelve jaw tips out as a
 * literal coordinate pair, and reading the shape back out of them is the whole
 * of what follows. Every tip is the same two steps from its knuckle: BACK along
 * the rail away from the nose, and OUT past the rail line — so a jaw is two
 * lengths and an arc parameter, and nothing about which rail or which sign.
 *
 * That is what the L needs. It was straight-mitred whatever the table's
 * pocket_round said — written down as a deliberate simplification and reported,
 * correctly, as the pockets not looking like the pockets: a UK L had mitred jaws
 * where every other UK table has curved ones, and a straight facing leaving the
 * knuckle at ang_side does not meet the rounded pocket back that cue_render cuts
 * for a rounded table. That is the gap in the rail.
 *
 * `mid` picks the middle pocket's jaw over a corner's. The numbers are the
 * rectangle's own, recovered from those literals: a corner tip stands 0.7 of a
 * jaw length out and 0.7 plus a hair back; a middle's stands a full jaw length
 * out and is pulled forward by 0.583 R, which is what made `bg` a separate
 * control point from the knuckle gap. */
static void jaw_tip(const CueWorld *w, Vec3 k, Vec3 u, Vec3 out, int mid,
                    Vec3 *tip, float *arc) {
    const float R = w->R;
    const float cl = 2.0f*R, ml = 1.6f*R, e3 = 0.25f*R;
    const float back = mid ? (0.583f*R + 0.3f*ml + e3) : (cl*0.7f + e3);
    const float rise = mid ? ml : cl*0.7f;
    *tip = v3(k.x - u.x*back + out.x*rise, 0, k.z - u.z*back + out.z*rise);
    *arc = mid ? 0.7f : 0.6f;
}

void cue_table_build_world(const CueTable *t, CueWorld *w) {
    cue_world_defaults(w, t->R, t->mass);
    w->cush_tilt = asinf((t->cushion_h - t->R) / t->R);
    /* The height a ball has to be above the cloth to be over the rail rather
     * than bouncing off it. The table knows it; the physics only needed telling. */
    w->cushion_nose = t->cushion_h;
    /* THE FRAME IS A SURFACE, AND IT IS WIDER THAN THE RAIL CAP.
     *
     * rail_w alone is 75-85 mm and a ball is 52-57 mm across, so a ball that
     * cleared a cushion landed on the cap, rolled about one ball's width and
     * fell off the outer edge — reported as vanishing the moment it bounced.
     * The woodwork does not stop there: CueVR's frame builds the body's outer
     * face at rail_w + 55 mm (SURR_X/SURR_Z) and the apron runs out to it, so
     * there is another 55 mm of table to land on and it should behave like it.
     *
     * That roughly doubles the width a jumped ball has to run along, which is
     * the difference between "it fell off immediately" and a ball that rattles
     * along the top and may still drop into a pocket. */
    w->bound_x = t->half_len + t->rail_w + CUE_FRAME_OUT;
    w->bound_z = t->half_wid + t->rail_w + CUE_FRAME_OUT;
    w->play_x  = t->half_len;
    w->play_z  = t->half_wid;
    /* ...and the shape itself, which for every rectangular table says exactly
     * what the four half-extents above already said. The half-extents remain
     * the BOUNDING box, so they stay a valid first reject for any shape. */
    w->nplay  = cue_table_bed_rects(t, 0.0f, w->play_r, CUE_MAX_RECT);
    w->nbound = cue_table_bed_rects(t, t->rail_w + CUE_FRAME_OUT,
                                    w->bound_r, CUE_MAX_RECT);
    /* The same height cue_table_surface reports and cue_render draws: the
     * cushion top and the wood cap are one surface, not a step. */
    w->rail_top = t->cushion_h * 1.30f;
    w->jaw_r = t->jaw_r;
    w->e_cush     = t->e_cush;
    w->cush_efall = t->cush_efall;
    w->e_cush_min = t->e_cush_min;
    w->drop_back = t->drop_back;
    w->drop_back_side = t->drop_back_side;

    const float hl = t->half_len, hw = t->half_wid, R = t->R;

    if (t->bed_shape == CUE_BED_L) {
        /* The L brings its own chain AND its own pockets, so it skips both the
         * rectangle's rail construction and the six-pocket block below. */
        build_L(w, t);
    } else if (!t->pocket_round) {
        /* US pool: straight mitred facings. */
        const float g = t->gap_corner, sg = t->gap_side, sl = t->facing_len;
        const float cc = cosf(t->ang_corner*DEG), sc = sinf(t->ang_corner*DEG);
        const float cs = cosf(t->ang_side*DEG),   ss = sinf(t->ang_side*DEG);
        add_chain(w, v3(-hl+g - cc*sl, 0, -hw - sc*sl), v3(-hl+g, 0, -hw),
                     v3(-sg, 0, -hw),                   v3(-sg + cs*sl, 0, -hw - ss*sl));
        add_chain(w, v3(sg - cs*sl, 0, -hw - ss*sl),    v3(sg, 0, -hw),
                     v3(hl-g, 0, -hw),                  v3(hl-g + cc*sl, 0, -hw - sc*sl));
        add_chain(w, v3(hl + sc*sl, 0, -hw+g - cc*sl),  v3(hl, 0, -hw+g),
                     v3(hl, 0, hw-g),                   v3(hl + sc*sl, 0, hw-g + cc*sl));
        add_chain(w, v3(hl-g + cc*sl, 0, hw + sc*sl),   v3(hl-g, 0, hw),
                     v3(sg, 0, hw),                     v3(sg - cs*sl, 0, hw + ss*sl));
        add_chain(w, v3(-sg + cs*sl, 0, hw + ss*sl),    v3(-sg, 0, hw),
                     v3(-hl+g, 0, hw),                  v3(-hl+g - cc*sl, 0, hw + sc*sl));
        add_chain(w, v3(-hl - sc*sl, 0, hw-g + cc*sl),  v3(-hl, 0, hw-g),
                     v3(-hl, 0, -hw+g),                 v3(-hl - sc*sl, 0, -hw+g - cc*sl));
    } else {
        /* Snooker/UK: bezier-curved jaws bulging into the pocket (rounded
         * knuckles), matching the 2D game's createCurvedRailChains. */
        /* WHERE THE KNUCKLES SIT, and it is its own number now.
         *
         * These used to be derived from pr_corner — bg = pr_corner + 0.5R, and
         * both gaps off that — which meant one field placed every jaw on the
         * table: widening a corner walked the middles out with it, and there
         * was no way at all to size a middle pocket on a rounded table. It also
         * left gap_corner/gap_side sitting in CueTable read by nothing but the
         * straight-mitre branch, so a bench slider on them moved nothing.
         *
         * Now they are what their names say, and the shipped values below are
         * exactly what the old derivation produced, so no table changed shape
         * when this went in. */
        const float cgap = t->gap_corner, mgap = t->gap_side;
        const float bg = mgap - 0.583f*R;   /* the middle jaw's far control point */
        const float cl = 2.0f*R, ml = 1.6f*R, e3 = 0.25f*R;
        /* Bezier steps per jaw. Three is right for a 128x128 screen and reads as
         * blocky knuckles when the pocket is 30 cm from your eye, so the VR build
         * raises it. More steps approximate the SAME intended curve more closely,
         * so this refines the collision geometry rather than changing it — but it
         * is a compile-time knob so the handheld's physics stays bit-identical. */
        const float ca = 0.6f, ma = 0.7f;
        const int nc = CUE_JAW_SEGS, nm = CUE_JAW_SEGS;
        /* C1 top-left */
        add_curved_chain(w, v3(-hl+cgap - cl*0.7f - e3,0,-hw - cl*0.7f), v3(-hl+cgap,0,-hw),
                            v3(-mgap,0,-hw), v3(-bg + ml*0.3f + e3,0,-hw - ml), ca, ma, nc, nm);
        /* C2 top-right */
        add_curved_chain(w, v3(bg - ml*0.3f - e3,0,-hw - ml), v3(mgap,0,-hw),
                            v3(hl-cgap,0,-hw), v3(hl-cgap + cl*0.7f + e3,0,-hw - cl*0.7f), ma, ca, nm, nc);
        /* C3 right */
        add_curved_chain(w, v3(hl + cl*0.7f,0,-hw+cgap - cl*0.7f - e3), v3(hl,0,-hw+cgap),
                            v3(hl,0,hw-cgap), v3(hl + cl*0.7f,0,hw-cgap + cl*0.7f + e3), ca, ca, nc, nc);
        /* C4 bottom-right */
        add_curved_chain(w, v3(hl-cgap + cl*0.7f + e3,0,hw + cl*0.7f), v3(hl-cgap,0,hw),
                            v3(mgap,0,hw), v3(bg - ml*0.3f - e3,0,hw + ml), ca, ma, nc, nm);
        /* C5 bottom-left */
        add_curved_chain(w, v3(-bg + ml*0.3f + e3,0,hw + ml), v3(-mgap,0,hw),
                            v3(-hl+cgap,0,hw), v3(-hl+cgap - cl*0.7f - e3,0,hw + cl*0.7f), ma, ca, nm, nc);
        /* C6 left */
        add_curved_chain(w, v3(-hl - cl*0.7f,0,hw-cgap + cl*0.7f + e3), v3(-hl,0,hw-cgap),
                            v3(-hl,0,-hw+cgap), v3(-hl - cl*0.7f,0,-hw+cgap - cl*0.7f - e3), ca, ca, nc, nc);
    }

    /* Smooth vertex normals: at each endpoint shared with a neighbouring
     * segment, average the two face normals so the collision normal can be
     * interpolated continuously along GENTLE curves (snooker rounded knuckles,
     * bezier jaw steps) — no kink there. But ONLY when the junction is actually
     * smooth: a sharp CONVEX corner (the US straight-pocket rail↔facing mitre,
     * which juts into play) must NOT be averaged, or the rail's endpoint normal
     * gets pulled toward the facing and a ball gliding along the rail bounces
     * off "the back of the knuckle" instead of glancing off the flat rail. The
     * jaw circle handles contact at those sharp knuckles. Threshold ≈ 37°. */
    const float SMOOTH_COS = 0.80f;
    for (int s = 0; s < w->nseg; s++) {
        Vec3 ns = w->seg[s].n;
        Vec3 na = ns, nb = ns;
        for (int o = 0; o < w->nseg; o++) {
            if (o == s) continue;
            /* Only smooth WITHIN a run of the same kind: the bezier jaw (all
             * kind=1) stays a continuous rounded knuckle, but a facing must NEVER
             * pull the straight rail nose's (kind=0) endpoint normal. Snooker's
             * rail→curve junction is gentle enough to pass the dot test below, so
             * without this a ball hugging the rail near the knuckle saw a normal
             * tilted toward the pocket throat — funnelling it into the pocket and
             * spinning it up off the rail. The jaw circle handles the junction. */
            if (w->seg[o].kind != w->seg[s].kind) continue;
            Vec3 no = w->seg[o].n;
            if (ns.x*no.x + ns.z*no.z < SMOOTH_COS) continue;   /* sharp corner: keep crisp */
            if (v3_len2(v3_sub(w->seg[o].b, w->seg[s].a)) < 1e-8f ||
                v3_len2(v3_sub(w->seg[o].a, w->seg[s].a)) < 1e-8f)
                na = v3_add(na, no);
            if (v3_len2(v3_sub(w->seg[o].a, w->seg[s].b)) < 1e-8f ||
                v3_len2(v3_sub(w->seg[o].b, w->seg[s].b)) < 1e-8f)
                nb = v3_add(nb, no);
        }
        w->seg[s].na = v3_norm(na);
        w->seg[s].nb = v3_norm(nb);
    }

    /* Pocket circles: centre offset just beyond the boundary; drop-capture
     * when the ball centre is within (radius − 0.3R), matching the 2D game. */
    const float d = 0.70710678f, oc = t->off_corner, os = t->off_side;
    /* Drop-capture radius (independent of the visible mouth/pr_side), and PER
     * TABLE — see cap_corner / cap_side. It used to be one literal here, 0.3 R
     * with 0.15 R for a UK middle, which is why no table could be given a drop
     * of its own without moving every other table's with it. */
    float capc = t->pr_corner - t->cap_corner, caps = t->pr_side - t->cap_side;
    if (t->bed_shape != CUE_BED_L) {
        add_pocket(w, -hl - oc*d, -hw - oc*d, capc, 0);
        add_pocket(w,  hl + oc*d, -hw - oc*d, capc, 0);
        add_pocket(w,  hl + oc*d,  hw + oc*d, capc, 0);
        add_pocket(w, -hl - oc*d,  hw + oc*d, capc, 0);
        add_pocket(w, 0.0f, -hw - os, caps, 1);
        add_pocket(w, 0.0f,  hw + os, caps, 1);
    }

    /* ---- each pocket's mouth, from the two jaw tips beside it ------------ */
    for (int p = 0; p < w->npocket; p++) {
        int j1 = -1, j2 = -1; float d1 = 1e30f, d2 = 1e30f;
        for (int j = 0; j < w->njaw; j++) {
            float dx = w->jaw[j].x - w->pocket[p].x;
            float dz = w->jaw[j].z - w->pocket[p].z;
            float dd = dx*dx + dz*dz;
            if (dd < d1) { d2 = d1; j2 = j1; d1 = dd; j1 = j; }
            else if (dd < d2) { d2 = dd; j2 = j; }
        }
        if (j1 < 0 || j2 < 0) {                 /* no jaws: fall back to radial */
            float l = sqrtf(w->pocket[p].x*w->pocket[p].x + w->pocket[p].z*w->pocket[p].z);
            if (l < 1e-6f) l = 1.0f;
            w->pmnorm[p] = v3(w->pocket[p].x/l, 0, w->pocket[p].z/l);
            w->pmouth[p] = w->pocket[p];
            continue;
        }
        Vec3 a = w->jaw[j1], b2 = w->jaw[j2];
        w->pmouth[p] = v3((a.x + b2.x) * 0.5f, 0, (a.z + b2.z) * 0.5f);
        /* Normal of the jaw-to-jaw line, pointing away from the table centre. */
        float ex = b2.x - a.x, ez = b2.z - a.z;
        float el = sqrtf(ex*ex + ez*ez); if (el < 1e-6f) el = 1.0f;
        Vec3 nrm = v3(-ez/el, 0, ex/el);
        if (nrm.x * w->pocket[p].x + nrm.z * w->pocket[p].z < 0.0f)
            nrm = v3(-nrm.x, 0, -nrm.z);
        w->pmnorm[p] = nrm;
    }

    for (int p = 0; p < w->npocket; p++) {
        float back = w->pocket_mid[p] ? w->drop_back_side : w->drop_back;
        w->drop_c[p] = v3(w->pocket[p].x + w->pmnorm[p].x * back, 0,
                          w->pocket[p].z + w->pmnorm[p].z * back);
    }

    w->cut_ref[0] = t->pr_corner; w->cut_ref[1] = t->pr_side;
    for (int m = 0; m < 2; m++) {
        CueCut c; cue_table_default_cut(t->kind, m, &c);
        w->cut_set[m] = c.set; w->cut_rad[m] = c.rad;
        w->cut_roll[m] = c.roll; w->cut_arc[m] = c.arc;
    }
    cue_table_derive_cut(w);
}

/* ---- the cloth cut, which both the renderer and the physics obey ---------- *
 *
 * Four numbers per pocket type per table; see CueCut in cue_table.h for what
 * each one moves. These are the shipped shape. They were set in the headset
 * with a ball rolling at the pocket, which is the only place a pocket can
 * honestly be judged, and they are per table size because a 12 ft snooker
 * pocket and a 9 ft American one are not the same cut scaled. */
void cue_table_default_cut(CueGameKind kind, int middle, CueCut *out) {
    /*                       set(m)   rad(x pr)  roll(x pr)  arc(deg)
     *
     * `rad` used to be a multiple of the DROP circle, which meant the cut could
     * not be held still while the drop was moved — shrink the drop and the cut
     * shrank with it and the pocket only ever looked the same. Both are off the
     * mouth now, so they are independent, and the numbers below are what the
     * old ratios worked out to on each table. */
    static const CueCut corner[] = {
        /* UK8   */ { 0.0265f, 1.3550f, 0.2200f,  90.0f },
        /* US8   */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },
        /* US9   */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },
        /* CN8   */ { 0.0170f, 1.1450f, 0.2200f,  90.0f },
        /* SNK15 */ { 0.0145f, 1.1350f, 0.2150f,  90.0f },
        /* SNK10 */ { 0.0145f, 1.1350f, 0.2150f,  90.0f },
        /* SNK6  */ { 0.0265f, 1.3550f, 0.2200f,  90.0f },
        /* STRT  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* PYRA — the American's cut, with the SETBACK scaled to this mouth
         * (0.517 of it) rather than copied in millimetres. */
        /* PYRA  */ { 0.0189f, 1.3900f, 0.2200f,  90.0f },
        /* PYRA7 — the same cut with the setback scaled to the smaller mouth */
        /* PYRA7 */ { 0.0168f, 1.3900f, 0.2200f,  90.0f },
        /* BILL — the standard table, so the 12 ft snooker cut exactly */
        /* BILL  */ { 0.0145f, 1.1350f, 0.2150f,  90.0f },
    };
    static const CueCut mid[] = {
        /* UK8   */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },
        /* US8   */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* US9   */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* CN8   */ { 0.0285f, 1.2650f, 0.2250f, 180.0f },
        /* SNK15 */ { 0.0285f, 1.2500f, 0.2150f, 180.0f },
        /* SNK10 */ { 0.0285f, 1.2500f, 0.2150f, 180.0f },
        /* SNK6  */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },
        /* STRT  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* PYRA  */ { 0.0234f, 1.4100f, 0.2200f, 180.0f },   /* ...and the middle */
        /* PYRA7 */ { 0.0211f, 1.4100f, 0.2200f, 180.0f },
        /* BILL  */ { 0.0285f, 1.2500f, 0.2150f, 180.0f },
    };
    /* THE ROW COUNT IS THE KIND COUNT, checked rather than assumed. These are
     * sized by their initialisers, so adding a kind without adding a row here
     * reads off the end of the array — silently, and only for the new kind. */
    typedef char cut_rows_match_kinds[
        ((int)(sizeof corner / sizeof corner[0]) == CUE_GAME_COUNT &&
         (int)(sizeof mid    / sizeof mid[0])    == CUE_GAME_COUNT) ? 1 : -1];
    int i = (int)kind; if (i < 0 || i >= CUE_GAME_COUNT) i = 0;
    *out = middle ? mid[i] : corner[i];
}

void cue_table_set_cut(CueWorld *w, int middle, CueCut c) {
    int i = middle ? 1 : 0;
    w->cut_set[i] = c.set; w->cut_rad[i] = c.rad;
    w->cut_roll[i] = c.roll; w->cut_arc[i] = c.arc;
    cue_table_derive_cut(w);
}

void cue_table_get_cut(const CueWorld *w, int middle, CueCut *out) {
    int i = middle ? 1 : 0;
    out->set = w->cut_set[i]; out->rad = w->cut_rad[i];
    out->roll = w->cut_roll[i]; out->arc = w->cut_arc[i];
}

/* The derived shape: where each pocket's arc sits, how big it is, and how far
 * the lip rolls. The setback runs OUT THROUGH THE MOUTH — along the line the
 * ball leaves on — so winding `set` up slides the whole cut straight out over
 * the drop and down again, which is the one motion that puts the cloth edge on
 * the line the ball is taken at. */
void cue_table_derive_cut(CueWorld *w) {
    for (int p = 0; p < w->npocket; p++) {
        /* ALONG THE POCKET'S OWN NORMAL, and its own kind — not its index and
         * not the sign of its coordinates.
         *
         * This read `p < 4` for "is it a corner" and took the setback direction
         * from whether x and z were negative. Both are true of a rectangle
         * centred on the origin and neither survives an L: its pocket 1 is a
         * middle that the index calls a corner, its pocket 4 is a corner the
         * index calls a middle, and the two beside the notch sit at coordinates
         * whose signs say nothing about which way is out of the table.
         *
         * The drop circle was already set back along pmnorm — the normal worked
         * out from the pocket's own two jaw tips — so the cut and the drop were
         * being pushed in DIFFERENT directions, and the two circles came off
         * the line they are both supposed to lie on. That is visible on an L
         * and invisible on a rectangle, where the two happen to agree.
         *
         * Identical on every rectangle: at a corner the old diagonal was the
         * unit diagonal times `set`, which is what pmnorm times `set` is, and
         * at a middle both are straight out along the rail. */
        int i = w->pocket_mid[p] ? 1 : 0;
        Vec3 C = w->pocket[p], n = w->pmnorm[p];
        w->cut_c[p] = v3(C.x + n.x * w->cut_set[i], 0, C.z + n.z * w->cut_set[i]);
        w->cut_r[p] = w->cut_ref[i]  * w->cut_rad[i];
        w->lip_d[p] = w->cut_ref[i]  * w->cut_roll[i];
    }
}

/* ---- THE SPINE: the line a table is laid out ALONG -----------------------
 *
 * Everything about where a game is SET OUT — the baulk line, the D, the six
 * snooker spots, the foot spot, the rack, the head string — was an x coordinate
 * on the centre line, with the rack growing in +x and the balls spread in z.
 * That is the table's long axis written into every one of them, and on an L it
 * is nonsense: the long axis runs down one arm, straight through the notch, and
 * out into the corner that is not there. It is why snooker was refused on any L
 * with a real bite in it — the pink, the black and half the reds landed in the
 * missing quadrant — and why the pack sat awkwardly on the long arm with the
 * short one empty.
 *
 * So the layout is expressed along a SPINE: a line down the middle of the table
 * from the baulk end to the far end, which on a rectangle is straight and on an
 * L TURNS THE CORNER. The baulk and the D are at one end of it, on one arm; the
 * rack is at the other end, on the other arm, ninety degrees round; and the
 * middle of the spine — where a snooker table puts its blue — is the middle of
 * the bend. The break is then a shot round a corner, which is the whole point of
 * playing on a table this shape.
 *
 * A position is given as the x coordinate it would have on a rectangle, because
 * that is what the table already stores and what every ruleset already means by
 * it. On a rectangle it IS that coordinate, untouched, so nothing about the
 * seven shipped tables moves by a float. On an L it is read as the same fraction
 * of the way along the spine, and the spine is walked to find out where that
 * actually is.
 *
 * The two legs of an L's spine are the centre lines of its two arms:
 *   arm A, the full-length band below the notch, centred at z = -notch_z/2
 *   arm B, the full-width column beside it,     centred at x = -notch_x/2
 * They cross at the bend. The free end of A is +x and the free end of B is +z —
 * the shared corner of the two arms is at (-half_len, -half_wid), so neither of
 * those is the free end of anything. Baulk goes on A's free end and the rack on
 * B's, which is what puts them at right angles. */
typedef struct { float ax, az, bx, bz, len; } CueLeg;

static int spine_legs(const CueTable *t, CueLeg *g) {
    const float hl = t->half_len, hw = t->half_wid;
    if (t->bed_shape != CUE_BED_L || t->notch_x <= 0.0f || t->notch_z <= 0.0f) {
        g[0].ax = -hl; g[0].az = 0.0f; g[0].bx = hl; g[0].bz = 0.0f;
        g[0].len = 2.0f * hl;
        return 1;
    }
    const float h  = cue_table_hand(t);
    const float cA = -t->notch_z * 0.5f * h;  /* arm A's centre line, in z */
    const float cB = -t->notch_x * 0.5f;      /* arm B's centre line, in x */
    g[0].ax = hl; g[0].az = cA; g[0].bx = cB; g[0].bz = cA;   /* baulk end -> bend */
    g[0].len = hl - cB;
    g[1].ax = cB; g[1].az = cA; g[1].bx = cB; g[1].bz = hw * h; /* bend -> far end */
    g[1].len = hw - cA * h;
    return 2;
}

float cue_table_spine_len(const CueTable *t) {
    if (!t) return 0.0f;
    CueLeg g[2];
    int n = spine_legs(t, g);
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += g[i].len;
    return s;
}

Vec3 cue_table_lay(const CueTable *t, float x, float across, Vec3 *dir) {
    if (!t) { if (dir) *dir = v3(1,0,0); return v3(0,0,0); }
    /* A RECTANGLE IS ALREADY EXPRESSED IN ITS OWN AXIS. Returned untouched
     * rather than round-tripped through a fraction: the seven shipped tables
     * must not move by so much as a float's worth, and "it would come back the
     * same to six places" is not the same promise. */
    if (t->bed_shape != CUE_BED_L || t->notch_x <= 0.0f || t->notch_z <= 0.0f) {
        if (dir) *dir = v3(1, 0, 0);
        return v3(x, 0.0f, across);
    }
    CueLeg g[2];
    int n = spine_legs(t, g);
    float total = 0.0f;
    for (int i = 0; i < n; i++) total += g[i].len;
    /* WHERE THE MIDDLE OF THE TABLE GOES: the BEND.
     *
     * Stretching the rectangle's whole length evenly over the spine is the
     * obvious mapping and it is not the right one — the two legs of an L are
     * rarely the same length, so the centre spot lands somewhere up one of them
     * and the blue, which is the ball a snooker table puts on its centre, sits
     * on a straight run of cushion instead of in the crook of the shape. The
     * middle of the table IS the bend; a table's layout is really "how far from
     * baulk" and "how far from the top", and the bend is the join. So the two
     * halves are mapped to the two legs, each stretched to fit its own.
     *
     * On a rectangle the bend is the centre of a straight line and both halves
     * are the same scale, so this is the identity — which is why a rectangle
     * takes the early return above and never gets here at all. */
    const float hl = t->half_len;
    const float bend = (n > 1) ? g[0].len : total * 0.5f;
    float s;
    if (hl <= 1e-4f)   s = bend;
    else if (x <= 0.0f) s = (x + hl) / hl * bend;
    else                s = bend + (x / hl) * (total - bend);
    if (s < 0.0f) s = 0.0f;
    if (s > total) s = total;
    for (int i = 0; i < n; i++) {
        if (s > g[i].len && i + 1 < n) { s -= g[i].len; continue; }
        float dx = (g[i].bx - g[i].ax) / g[i].len;
        float dz = (g[i].bz - g[i].az) / g[i].len;
        if (dir) *dir = v3(dx, 0.0f, dz);
        /* +across is ninety degrees to the LEFT of the direction of travel,
         * which on a rectangle is +z — so yellow stays on the right of the D
         * and nothing about a shipped table's layout is mirrored. */
        return v3(g[i].ax + dx * s - dz * across, 0.0f,
                  g[i].az + dz * s + dx * across);
    }
    if (dir) *dir = v3(1, 0, 0);
    return v3(x, 0.0f, across);
}

Vec3 cue_table_cue_home(const CueTable *t) {
    const float CUE_Y = (t->cue_R > 0.0f) ? t->cue_R : t->R;
    /* All games start OFF the centre line so a break naturally strikes the pack
     * at an angle (a dead-straight break into the apex splits poorly). Snooker &
     * UK8 break from one side of the D; US pool from the side of the kitchen.
     * Laid out along the spine, so on an L it is on the baulk arm and the pack
     * is round the corner from it. */
    Vec3 p;
    if (t->is_snooker || t->kind == CUE_GAME_UK8 || CUE_GAME_IS_PYRAMID(t->kind))
        p = cue_table_lay(t, t->baulk_x, -t->d_radius * 0.55f, NULL);
    else
        p = cue_table_lay(t, -t->half_len * 0.5f, t->half_wid * 0.40f, NULL);
    /* On its own radius: the English white is smaller than the set. */
    return v3(p.x, CUE_Y, p.z);
}

/* Clamp a desired cue-ball placement to the legal ball-in-hand region:
 * inside the D (snooker / UK8) or behind the head string (US pool). Returns the
 * clamped XZ (y left to the caller). */
static Vec3 clamp_region(const CueTable *t, Vec3 p, int breaking, int anywhere);

/* Is this spot clear of every ball already on the table? */
static int placement_clear(const CueTable *t, Vec3 p, const CueBall *balls, int n) {
    const float sep = ((t->cue_R > 0.0f) ? (t->cue_R + t->R) : (2.0f * t->R)) + 0.0004f;
    for (int i = 1; i < n; i++) {
        if (!balls[i].on) continue;
        float dx = p.x - balls[i].pos.x, dz = p.z - balls[i].pos.z;
        if (dx*dx + dz*dz < sep*sep) return 0;
    }
    return 1;
}

/* ---- F2: the bed as a union of rectangles -------------------------------- */
int cue_table_bed_rects(const CueTable *t, float g, CueRect *out, int cap) {
    if (!t || !out || cap < 1) return 0;
    const float hl = t->half_len, hw = t->half_wid;
    if (t->bed_shape != CUE_BED_L || cap < 2 ||
        t->notch_x <= 0.0f || t->notch_z <= 0.0f) {
        out[0].x0 = -hl - g; out[0].x1 = hl + g;
        out[0].z0 = -hw - g; out[0].z1 = hw + g;
        return 1;
    }
    /* An L, as a band and a column rather than as two pieces meeting at a
     * corner. Both of these have four OUTSIDE edges, so growing them is the
     * same operation as growing a rectangle — which is the whole reason to cut
     * it up this way rather than the obvious way. Their overlap is the bottom
     * left, and overlap costs nothing: a point inside both is inside. */
    float nx = t->notch_x, nz = t->notch_z;
    if (nx > 2.0f * hl) nx = 2.0f * hl;
    if (nz > 2.0f * hw) nz = 2.0f * hw;
    /* the full-width band below the notch, and the full-height column beside
     * it — written for a RIGHT-handed L and mirrored in z for a left-handed
     * one, which is the whole of what the hand costs anywhere. */
    const float h = cue_table_hand(t);
    out[0].x0 = -hl - g;      out[0].x1 = hl + g;
    out[0].z0 = -hw - g;      out[0].z1 = hw - nz + g;
    out[1].x0 = -hl - g;      out[1].x1 = hl - nx + g;
    out[1].z0 = -hw - g;      out[1].z1 = hw + g;
    if (h < 0.0f) for (int i = 0; i < 2; i++) {
        float z0 = -out[i].z1, z1 = -out[i].z0;
        out[i].z0 = z0; out[i].z1 = z1;
    }
    return 2;
}

int cue_table_bed_strips(const CueTable *t, CueRect *out, int cap) {
    if (!t || !out || cap < 1) return 0;
    const float hl = t->half_len, hw = t->half_wid;
    if (t->bed_shape != CUE_BED_L || cap < 2 ||
        t->notch_x <= 0.0f || t->notch_z <= 0.0f) {
        out[0].x0 = -hl; out[0].x1 = hl;
        out[0].z0 = -hw; out[0].z1 = hw;
        return 1;
    }
    float nx = t->notch_x, nz = t->notch_z;
    if (nx > 2.0f * hl) nx = 2.0f * hl;
    if (nz > 2.0f * hw) nz = 2.0f * hw;
    /* below the notch line the bed runs the whole length; above it, only as far
     * as the notch — the same two pieces the union describes, cut where they
     * would otherwise have overlapped */
    const float h = cue_table_hand(t);
    out[0].x0 = -hl;  out[0].x1 = hl;
    out[0].z0 = -hw;  out[0].z1 = hw - nz;
    out[1].x0 = -hl;  out[1].x1 = hl - nx;
    out[1].z0 = hw - nz;  out[1].z1 = hw;
    if (h < 0.0f) for (int i = 0; i < 2; i++) {
        float z0 = -out[i].z1, z1 = -out[i].z0;
        out[i].z0 = z0; out[i].z1 = z1;
    }
    return 2;
}

int cue_table_game_ok(const CueTable *t, CueGameKind kind, int laid_out,
                      char *msg, int msgcap) {
    if (msg && msgcap > 0) msg[0] = 0;
    if (!t) return tab_fail(msg, msgcap, "there is no table");

    /* The cue ball has to have somewhere to be, whatever the game. */
    Vec3 home = cue_table_cue_home(t);
    if (!cue_table_on_bed(t, home.x, home.z))
        return tab_fail(msg, msgcap, "the cue ball's home is off the cloth");
    if (laid_out) return 1;      /* every ball placed by hand: nothing else to ask */

    const int snooker = (kind == CUE_GAME_SNK15 || kind == CUE_GAME_SNK10 ||
                         kind == CUE_GAME_SNK6);
    if (snooker) {
        /* SNOOKER NEEDS ITS SPOTS, and they are places on a table rather than
         * fractions of one: the three baulk colours on the D, the blue on the
         * centre, the pink and the black up the top. Every one of them has to
         * be on cloth, and on an L the top two are exactly where the corner is
         * missing. */
        const float sx[6] = { t->baulk_x, t->baulk_x, t->baulk_x,
                              t->blue_x,  t->pink_x,  t->black_x };
        const float sz[6] = { +t->d_radius, -t->d_radius, 0.0f, 0.0f, 0.0f, 0.0f };
        static const char *SN[6] = { "yellow", "green", "brown", "blue", "pink", "black" };
        for (int i = 0; i < 6; i++) {
            /* WHERE THE SPOT ACTUALLY IS, which on an L is not (x, z): the
             * layout runs along the spine and turns the corner with it. Asked
             * of the same function that puts the ball there, so the validator
             * and the rack can never disagree about whether a game fits. */
            Vec3 q = cue_table_lay(t, sx[i], sz[i], NULL);
            if (cue_table_on_bed(t, q.x, q.z)) continue;
            char buf[96];
            snprintf(buf, sizeof buf, "the %s spot is off the cloth", SN[i]);
            return tab_fail(msg, msgcap, buf);
        }
        /* ...and the triangle of reds behind the pink, whose back row is the
         * furthest thing up the table — in the pink's own frame, because that
         * is the frame it is racked in. */
        int rows = (t->reds <= 6) ? 3 : (t->reds <= 10) ? 4 : 5;
        float apexx = t->pink_x + 2.0f * t->R + 0.002f;
        float along = (float)(rows - 1) * t->R * 1.7320508f;
        float half  = (float)(rows - 1) * t->R;
        Vec3 up; Vec3 apex = cue_table_lay(t, apexx, 0.0f, &up);
        Vec3 side = v3(-up.z, 0.0f, up.x);
        for (int k = -1; k <= 1; k++) {
            float off = (float)k * half;
            float bx = apex.x + up.x * along + side.x * off;
            float bz = apex.z + up.z * along + side.z * off;
            if (!cue_table_on_bed(t, bx, bz))
                return tab_fail(msg, msgcap, "the reds do not fit behind the pink");
        }
        return 1;
    }

    /* The pool games rack on the foot spot, which does move with the bed — but
     * an L can still take the bite out of exactly where the pack goes. */
    {
        Vec3 up; Vec3 foot = cue_table_foot_spot_dir(t, &up);
        Vec3 side = v3(-up.z, 0.0f, up.x);
        int rows = 5;
        float along = (float)(rows - 1) * t->R * 1.7320508f;
        float half  = (float)(rows - 1) * t->R;
        if (!cue_table_on_bed(t, foot.x, foot.z))
            return tab_fail(msg, msgcap, "the rack does not fit on this bed");
        for (int k = -1; k <= 1; k++) {
            float off = (float)k * half;
            float bx = foot.x + up.x * along + side.x * off;
            float bz = foot.z + up.z * along + side.z * off;
            if (!cue_table_on_bed(t, bx, bz))
                return tab_fail(msg, msgcap, "the rack does not fit on this bed");
        }
    }
    return 1;
}

Vec3 cue_table_foot_spot_dir(const CueTable *t, Vec3 *dir) {
    if (!t) { if (dir) *dir = v3(1,0,0); return v3(0,0,0); }
    Vec3 p = cue_table_lay(t, t->half_len * 0.5f, 0.0f, dir);
    return v3(p.x, t->R, p.z);
}
Vec3 cue_table_foot_spot(const CueTable *t) {
    return cue_table_foot_spot_dir(t, NULL);
}

int cue_table_on_bed(const CueTable *t, float x, float z) {
    CueRect r[CUE_MAX_RECT];
    int n = cue_table_bed_rects(t, 0.0f, r, CUE_MAX_RECT);
    return cue_rects_contain(r, n, x, z);
}

Vec3 cue_table_clamp_placement(const CueTable *t, Vec3 p) {
    return cue_table_clamp_placement_balls(t, p, NULL, 0, 0);
}
Vec3 cue_table_clamp_placement_balls(const CueTable *t, Vec3 p,
                                     const CueBall *balls, int n, int breaking) {
    return cue_table_clamp_placement_any(t, p, balls, n, breaking, 0);
}
Vec3 cue_table_clamp_placement_any(const CueTable *t, Vec3 p,
                                   const CueBall *balls, int n, int breaking,
                                   int anywhere) {
    const float R = t->R;
    /* The ball being placed is the CUE ball, which on an English table is the
     * small one — so the gap it needs from an object ball is the two radii, not
     * twice the set's. */
    const float sep = ((t->cue_R > 0.0f) ? (t->cue_R + R) : (2.0f * R)) + 0.0004f;
    for (int pass = 0; pass < 6; pass++) {
        p = clamp_region(t, p, breaking, anywhere);
        int moved = 0;
        for (int i = 1; i < n; i++) {
            if (!balls[i].on) continue;
            float dx = p.x - balls[i].pos.x, dz = p.z - balls[i].pos.z;
            float d2 = dx*dx + dz*dz;
            if (d2 >= sep*sep) continue;
            float d = sqrtf(d2);
            if (d < 1e-5f) {                   /* dead centre: pick a direction */
                dx = 1.0f; dz = 0.0f; d = 1.0f;
            }
            p.x = balls[i].pos.x + dx / d * sep;
            p.z = balls[i].pos.z + dz / d * sep;
            moved = 1;
        }
        if (!moved) break;
    }

    /* THE REGION IS A RULE; THE SEPARATION IS A COURTESY, and the loop above had
     * that the wrong way round. Its last act is a PUSH, not a clamp, so a ball
     * placed against one of the baulk colours was shoved clear of the colour and
     * returned there — outside the D, in front of the line, which is not a legal
     * position in any of these games. Reported as the cue ball ending up forward
     * of the D whenever something was in the way, and it happened to the CPU as
     * well because cue_ai_place asks this same function.
     *
     * So the region wins. Clamp, and if that lands on top of something, look
     * for the nearest spot that is BOTH legal and clear rather than accepting
     * one that is neither: rings of candidates outward from where the player
     * asked, each one clamped back into the region before it is judged. Half a
     * ball at a time, out to a couple of ball widths, is more than enough room
     * for the worst case there is — the cue ball wanted where the brown sits
     * with the green and yellow either side. */
    p = clamp_region(t, p, breaking, anywhere);
    if (n > 0 && !placement_clear(t, p, balls, n)) {
        const Vec3 want = p;
        for (int ring = 1; ring <= 6; ring++) {
            float rad = (float)ring * R;
            for (int a = 0; a < 16; a++) {
                float th = (float)a * (6.2831853f / 16.0f);
                Vec3 c = { want.x + cosf(th) * rad, want.y, want.z + sinf(th) * rad };
                c = clamp_region(t, c, breaking, anywhere);
                if (placement_clear(t, c, balls, n)) return c;
            }
        }
        /* Nowhere legal and clear — a table so full there is no room in the D.
         * The legal spot stands: overlapping is recoverable, illegal is not. */
    }
    return p;
}

static Vec3 clamp_region(const CueTable *t, Vec3 p, int breaking, int anywhere) {
    float R = t->R;
    /* BALL IN HAND MEANS THE WHOLE TABLE, where the rules say so. The English
     * table is the D under pub rules and the whole cloth under International
     * and Ultimate Pool, so the region cannot be decided by the table alone —
     * the caller passes what the rules of the frame allow. Snooker is always
     * the D. */
    if (anywhere && !t->is_snooker) {
        /* Ball in hand anywhere on the cloth — which is a SHAPE, not two
         * numbers. Clamping to the bounding half-extents put the ball in the
         * missing corner of an L: a legal-looking placement outside the table,
         * from which the first stroke sends it through the wall.
         *
         * Clamped into each rectangle in turn, keeping whichever answer moved
         * least. For one rectangle that is the same arithmetic as before; for
         * two it lands on the nearest real cloth, which is what a player
         * reaching for the far side of an L means. */
        CueRect r[CUE_MAX_RECT];
        int nr = cue_table_bed_rects(t, 0.0f, r, CUE_MAX_RECT);
        Vec3 best = p; float bd = -1.0f;
        for (int i = 0; i < nr; i++) {
            float x0 = r[i].x0 + R, x1 = r[i].x1 - R;
            float z0 = r[i].z0 + R, z1 = r[i].z1 - R;
            if (x1 < x0 || z1 < z0) continue;      /* a rail wider than the piece */
            Vec3 q = p;
            if (q.x < x0) q.x = x0; else if (q.x > x1) q.x = x1;
            if (q.z < z0) q.z = z0; else if (q.z > z1) q.z = z1;
            float dx = q.x - p.x, dz = q.z - p.z;
            float d = dx*dx + dz*dz;
            if (bd < 0.0f || d < bd) { bd = d; best = q; }
        }
        return (bd < 0.0f) ? p : best;
    }
    if (t->is_snooker || t->kind == CUE_GAME_UK8 || CUE_GAME_IS_PYRAMID(t->kind)) {
        /* The D — and the pyramid's HOUSE, which is the same thing as far as
         * this is concerned: a region behind a line that the cue ball is played
         * from. A half-disc of radius d_radius centred on (baulk_x, 0), bulging
         * toward the baulk cushion (−x).
         *
         * The CENTRE goes up to the line, not the ball's edge. A ball played
         * from hand is in the D if its centre is within the D or on the lines
         * bounding it — half of it may hang outside, and on a tight angle from
         * the D that half is the difference between having the shot and not.
         * This kept the whole ball inside, which quietly shrank the D by a ball
         * radius all the way round. */
        /* IN THE BAULK LINE'S OWN FRAME. On a rectangle `up` is +x and this is
         * the same clamp it has always been, to the float; on an L the baulk
         * line lies across one arm and the D bulges back down it, so a D
         * expressed in world x and z would be sideways on. */
        Vec3 up; Vec3 c = cue_table_lay(t, t->baulk_x, 0.0f, &up);
        Vec3 side = v3(-up.z, 0.0f, up.x);
        float along = (p.x - c.x) * up.x + (p.z - c.z) * up.z;
        float off   = (p.x - c.x) * side.x + (p.z - c.z) * side.z;
        if (along > 0.0f) along = 0.0f;                /* not past the baulk line */
        if (t->house) {
            /* THE HOUSE IS THE WHOLE WIDTH. Across, only the cushions stop it;
             * back, only the baulk cushion. No arc, so no radius. */
            float lim = t->half_wid - R;
            if (off >  lim) off =  lim;
            if (off < -lim) off = -lim;
            float back = -(t->baulk_x + t->half_len) + R;
            if (along < back) along = back;
        } else {
            float rmax = t->d_radius;
            float d = sqrtf(along*along + off*off);
            if (d > rmax && d > 1e-6f) { float k = rmax / d; along *= k; off *= k; }
        }
        p.x = c.x + up.x * along + side.x * off;
        p.z = c.z + up.z * along + side.z * off;
        return p;
    }
    /* US pool — 8-ball, 9-ball and Chinese 8: ball in hand is ANYWHERE ON THE
     * TABLE. Behind the head string is the rule for the BREAK, and only for the
     * break; this applied it to every foul as well, so a player fouled at the
     * black end and was marched back to baulk to place. Chinese 8 is where it
     * was noticed and all three were doing it.
     *
     * Snooker and UK 8-ball are different and are handled above: there the ball
     * really does go back in the D every time. */
    float lim = t->half_wid - R;
    float lenlim = t->half_len - R;
    if (!breaking) {
        if (p.x >  lenlim) p.x =  lenlim;
        if (p.x < -lenlim) p.x = -lenlim;
        if (p.z >  lim) p.z =  lim;
        if (p.z < -lim) p.z = -lim;
        return p;
    }
    /* BEHIND THE HEAD STRING, and "behind" is along the spine. On a rectangle
     * that is x < baulk_x and the two clamps below are the ones that were here;
     * on an L the head string runs across the baulk arm, and everything behind
     * it is the stub of that arm rather than a slab of the bounding box. */
    {   Vec3 up; Vec3 c = cue_table_lay(t, t->baulk_x, 0.0f, &up);
        Vec3 side = v3(-up.z, 0.0f, up.x);
        float along = (p.x - c.x) * up.x + (p.z - c.z) * up.z;
        float off   = (p.x - c.x) * side.x + (p.z - c.z) * side.z;
        if (along > -R) along = -R;
        float back = cue_table_spine_len(t) * 0.5f;    /* far enough to matter */
        if (along < -back) along = -back;
        p.x = c.x + up.x * along + side.x * off;
        p.z = c.z + up.z * along + side.z * off; }
    if (p.x >  lenlim) p.x =  lenlim;
    if (p.x < -lenlim) p.x = -lenlim;
    if (p.z >  lim) p.z =  lim;
    if (p.z < -lim) p.z = -lim;
    return p;
}

/* Per-rack RNG (render-only ball orientation; advances each ball + each rack so
 * the balls don't all face the same way and racks differ between frames). */
static uint32_t s_orient_rng = 0x2545F491u;
static float orient_rand(void) {
    s_orient_rng ^= s_orient_rng << 13; s_orient_rng ^= s_orient_rng >> 17; s_orient_rng ^= s_orient_rng << 5;
    return (float)(s_orient_rng & 0xFFFFu) * (1.0f / 65536.0f);
}
static Mat3 rand_orient(void) {
    Mat3 m = m3_identity();
    m3_rotate_local(&m, 0, orient_rand() * 6.2831853f);
    m3_rotate_local(&m, 1, orient_rand() * 6.2831853f);
    m3_rotate_local(&m, 2, orient_rand() * 6.2831853f);
    return m;
}

static void set_ball(CueBall *b, int id, float x, float z, float R) {
    b->pos = v3(x, R, z);
    b->vel = v3(0, 0, 0);
    b->w = v3(0, 0, 0);
    b->orient = rand_orient();      /* random facing so the rack isn't uniform */
    b->on = 1;
    b->id = (uint8_t)id;
    b->pocket = 0;
}

static int rack_pool(const CueTable *t, CueBall *b) {
    const float R = t->R;
    /* IN THE FOOT SPOT'S OWN FRAME, not in world x and z. On a rectangle `up` is
     * +x and `side` is +z and this is the rack it always was; on an L the spine
     * has turned the corner by here, so the triangle turns with it and points
     * back down the arm the cue ball is on. */
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    float footx = foot.x, footz = foot.z;
    float dx = R * 1.7320508f;
    #define RACK_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                            (footz + up.z*(r_) + side.z*(o_))
    /* Fixed arrangement matching 2dpool (RackPatterns.eightBall): 8 in the centre
     * of row 3, one solid + one stripe in the two back corners. */
    static const int rows[5][5] = {
        { 1 }, { 9, 2 }, { 3, 8, 10 }, { 11, 4, 5, 12 }, { 6, 13, 14, 7, 15 },
    };
    int n = 1;
    for (int row = 0; row < 5; row++) {
        for (int k = 0; k <= row; k++)
            set_ball(&b[n++], rows[row][k],
                     RACK_AT(row * dx, -(row) * R + k * 2.0f * R), R);
    }
    /* From cue_table_cue_home(), not from a repeat of the head-string number:
     * a UK table breaks from inside the D, and hard-coding -hl/2 here put the
     * cue ball a hand's breadth IN FRONT of its own baulk line — an illegal
     * break, on the centre line, on the one table whose rules this file
     * already knew. */
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef RACK_AT
    return n;
}

/* US 9-ball: diamond rack — 1 at the apex (foot spot), 9 in the centre. */
static int rack_9ball(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float footx = foot.x, fz = foot.z;
    float dx = R * 1.7320508f;
    #define RACK_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                            (fz    + up.z*(r_) + side.z*(o_))
    set_ball(&b[1], 1, RACK_AT(0.0f,     0.0f),   R);
    set_ball(&b[2], 2, RACK_AT(dx,      -R),      R);
    set_ball(&b[3], 3, RACK_AT(dx,       R),      R);
    set_ball(&b[4], 4, RACK_AT(2*dx,    -2*R),    R);
    set_ball(&b[5], 9, RACK_AT(2*dx,     0.0f),   R);   /* 9 in the middle */
    set_ball(&b[6], 5, RACK_AT(2*dx,     2*R),    R);
    set_ball(&b[7], 6, RACK_AT(3*dx,    -R),      R);
    set_ball(&b[8], 7, RACK_AT(3*dx,     R),      R);
    set_ball(&b[9], 8, RACK_AT(4*dx,     0.0f),   R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef RACK_AT
    return 10;
}

/* ENGLISH BILLIARDS: the red on the Spot, and both cue balls in hand.
 *
 * Section 3 Rule 2(b): "The red is placed on the Spot and the first player
 * plays from in-hand." Both whites start in hand — the non-striker's ball is
 * not on the table until he plays it — so only the red is placed.
 *
 * Index 0 is the ball being struck and carries its owner's colour; index 2 is
 * the other side's, off the table until its turn. The rules exchange the two
 * at a change of turn, which is why they are laid out as a pair here. */
static int rack_billiards(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    /* In hand, and placed in the D so a host that never asks for a placement
     * still has a legal opening shot. */
    { Vec3 q = cue_table_lay(t, t->baulk_x, -t->d_radius * 0.4f, NULL);
      set_ball(&b[n++], CUE_ID_BIL_WHITE, q.x, q.z, R); }
    { Vec3 q = cue_table_lay(t, t->black_x, 0.0f, NULL);
      set_ball(&b[n++], CUE_ID_BIL_RED, q.x, q.z, R); }
    /* The other side's ball: on the table only when it is that side's turn. */
    { Vec3 q = cue_table_lay(t, t->baulk_x, +t->d_radius * 0.4f, NULL);
      set_ball(&b[n], CUE_ID_BIL_YELLOW, q.x, q.z, R); b[n].on = 0; n++; }
    return n;
}

static int rack_snooker(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    /* Every spot along the SPINE. On a rectangle these are the x coordinates
     * they have always been; on an L the baulk colours are on one arm, the blue
     * is on the bend and the pink, the black and the reds are round the corner
     * on the other — which is the layout that makes snooker possible on the
     * shape at all, and makes the break a shot off a cushion. */
    { Vec3 q = cue_table_lay(t, t->baulk_x, -t->d_radius * 0.4f, NULL);
      set_ball(&b[n++], CUE_ID_CUE, q.x, q.z, R); }
    /* Green, brown, yellow reading LEFT TO RIGHT from behind the D — "God
     * Bless You". +Z is the player's right facing up the table, so yellow is
     * +d_radius. This and cue_rules.c's respot table were both mirrored, and
     * both carried a comment saying the opposite of what the code did. */
    { Vec3 q = cue_table_lay(t, t->baulk_x, +t->d_radius, NULL);   /* yellow — right of the D */
      set_ball(&b[n++], CUE_ID_YELLOW, q.x, q.z, R); }
    { Vec3 q = cue_table_lay(t, t->baulk_x, -t->d_radius, NULL);   /* green  — left of the D  */
      set_ball(&b[n++], CUE_ID_GREEN,  q.x, q.z, R); }
    { Vec3 q = cue_table_lay(t, t->baulk_x, 0.0f, NULL);
      set_ball(&b[n++], CUE_ID_BROWN,  q.x, q.z, R); }
    { Vec3 q = cue_table_lay(t, t->blue_x,  0.0f, NULL);
      set_ball(&b[n++], CUE_ID_BLUE,   q.x, q.z, R); }
    { Vec3 q = cue_table_lay(t, t->pink_x,  0.0f, NULL);
      set_ball(&b[n++], CUE_ID_PINK,   q.x, q.z, R); }
    { Vec3 q = cue_table_lay(t, t->black_x, 0.0f, NULL);
      set_ball(&b[n++], CUE_ID_BLACK,  q.x, q.z, R); }
    /* reds triangle: 3 rows (6), 4 rows (10) or 5 rows (15), apex behind pink,
     * and laid out in the PINK'S own frame so that on an L it grows up the arm
     * the pink is on rather than off the side of it. */
    int rows = (t->reds <= 6) ? 3 : (t->reds <= 10) ? 4 : 5;
    float apexx = t->pink_x + 2.0f * R + 0.002f;
    float dx = R * 1.7320508f;
    Vec3 up; Vec3 apex = cue_table_lay(t, apexx, 0.0f, &up);
    Vec3 side = v3(-up.z, 0.0f, up.x);
    int red_id = 1;
    for (int row = 0; row < rows; row++) {
        for (int k = 0; k <= row; k++) {
            float along = (float)row * dx, off = -(float)row * R + (float)k * 2.0f * R;
            set_ball(&b[n++], red_id++,
                     apex.x + up.x * along + side.x * off,
                     apex.z + up.z * along + side.z * off, R);
        }
    }
    return n;
}

/* A SIX-BALL RACK for the practice challenges: a small triangle on the foot
 * spot and the white at home. Ids 1..6, which are reds on a snooker table and
 * the low solids on a pool one — so the rack is always made of balls the table
 * you are on actually uses, without the caller having to know which. */
int cue_table_rack_six(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    const float R = t->R;
    const float footx = t->half_len * 0.5f;
    const float dx = R * 1.7320508f;
    int n = 1, id = 1;
    for (int row = 0; row < 3; row++) {
        float x = footx + row * dx;
        for (int k = 0; k <= row; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            set_ball(&balls[n++], id++, x, z, R);
        }
    }
    { Vec3 h = cue_table_cue_home(t); set_ball(&balls[0], CUE_ID_CUE, h.x, h.z, R); }
    return n;
}

/* THE 14.1 RERACK. Straight pool is played to a target score across as many
 * racks as it takes: when one object ball is left, the other fourteen come back
 * as a triangle with THE APEX SPACE EMPTY, and the fifteenth — the break ball —
 * stays exactly where it lies, as does the cue ball. The break ball and the
 * apex gap are the whole point: the incoming player breaks the new rack open
 * off a ball that is already in a usable position, which is why a 14.1 run
 * continues across racks instead of restarting.
 *
 * Only balls that are currently OFF the table are placed. Anything still on it
 * is left untouched, so the caller does not have to identify the break ball —
 * whatever survived is it.
 *
 * WHAT THIS DOES NOT DO: the interference cases. If the break ball or the cue
 * ball is standing inside the rack area, the real rules move specified balls to
 * specified spots (WPA 4.14). Here they simply stay, which can leave a ball
 * touching the back of the rack. Rare, legal-looking, and worth fixing when the
 * rest of 14.1 has been played enough to say how often it comes up. */
int cue_table_rack_14(const CueTable *t, CueBall *b, int n) {
    if (!t || !b || n <= 0) return 0;
    const float R = t->R;
    const float footx = t->half_len * 0.5f;
    const float dx = R * 1.7320508f;

    /* the fifteen triangle slots, apex first — the same geometry rack_pool
     * lays out, so a reracked pack sits exactly where the opening one did */
    Vec3 slot[15];
    int ns = 0;
    for (int row = 0; row < 5; row++) {
        float x = footx + row * dx;
        for (int k = 0; k <= row; k++)
            slot[ns++] = v3(x, R, (-(row) * R) + k * 2.0f * R);
    }

    /* A SLOT A BALL IS ALREADY STANDING IN IS NOT A FREE SLOT. The break ball
     * stays where it lies, and where it lies can be inside the rack area — the
     * commonest case being a ball that never moved from the pack. Placing on top
     * of it is a physics explosion the moment either is touched, so occupied
     * slots are skipped and the overflow goes up the long string behind the
     * rack, the same place a spotted ball goes. */
    int free_slot[15]; int nfree = 0;
    for (int s = 1; s < 15; s++) {                   /* slot 0 is the empty apex */
        int taken = 0;
        for (int i = 1; i < n && !taken; i++) {
            if (!b[i].on) continue;
            float dx = b[i].pos.x - slot[s].x, dz = b[i].pos.z - slot[s].z;
            if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R) * 0.98f) taken = 1;
        }
        if (!taken) free_slot[nfree++] = s;
    }

    int placed = 0, overflow = 0;
    for (int i = 1; i < n && placed < 14; i++) {
        if (b[i].on) continue;                       /* still up — the break ball */
        if (b[i].id < 1 || b[i].id > 15) continue;   /* object balls only */
        Vec3 p;
        if (placed < nfree) {
            p = slot[free_slot[placed]];
        } else {
            /* Behind the back row, on the centre line, stepping back until the
             * spot is clear of everything already down. */
            do {
                overflow++;
                p = v3(slot[14].x + (float)overflow * 2.05f * R, R, 0.0f);
                int clash = 0;
                for (int j = 1; j < n && !clash; j++) {
                    if (!b[j].on) continue;
                    float dx = b[j].pos.x - p.x, dz = b[j].pos.z - p.z;
                    if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R) * 0.98f) clash = 1;
                }
                if (!clash) break;
            } while (overflow < 40);
        }
        b[i].pos = p;
        b[i].vel = v3(0, 0, 0);
        b[i].w   = v3(0, 0, 0);
        b[i].drop = 0.0f;
        b[i].pocket = 0;
        b[i].on = 1;
        b[i].orient = rand_orient();
        placed++;
    }
    return placed;
}

/* THE PYRAMID: fifteen balls in a triangle with the apex on the foot spot,
 * pointing back down the table at the house. Geometrically that is the pool
 * rack — apex nearest baulk, base toward the top cushion — so it is the same
 * triangle with the ids simply in order, because a pyramid's balls are
 * unnumbered and interchangeable and nothing reads them. */
static int rack_pyramid(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float dx = R * 1.7320508f;
    int n = 1, id = 1;
    for (int row = 0; row < 5; row++)
        for (int k = 0; k <= row; k++) {
            float along = (float)row * dx;
            float off   = -(float)row * R + (float)k * 2.0f * R;
            set_ball(&b[n++], id++,
                     foot.x + up.x * along + side.x * off,
                     foot.z + up.z * along + side.z * off, R);
        }
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    return n;
}

int cue_table_respot_one(const CueTable *t, CueBall *b, int n) {
    if (!t || !b || n <= 0) return 0;
    /* the lowest id that is off the table — deterministic, because both ends of
     * a match run this and have to agree */
    int pick = -1;
    for (int i = 1; i < n; i++)
        if (!b[i].on && b[i].id >= 1 && b[i].id <= 15 &&
            (pick < 0 || b[i].id < b[pick].id)) pick = i;
    if (pick < 0) return 0;

    /* the foot spot, then straight back up the table until it is clear. The
     * same rule a spotted ball follows everywhere else in this file. */
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const float R = t->R;
    for (int step = 0; step < 60; step++) {
        Vec3 p = v3(foot.x + up.x * (float)step * 2.05f * R, R,
                    foot.z + up.z * (float)step * 2.05f * R);
        if (!cue_table_on_bed(t, p.x, p.z)) continue;
        int clash = 0;
        for (int j = 0; j < n && !clash; j++) {
            if (j == pick || !b[j].on) continue;
            float dx = b[j].pos.x - p.x, dz = b[j].pos.z - p.z;
            if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R) * 0.98f) clash = 1;
        }
        if (clash) continue;
        b[pick].pos = p;
        b[pick].vel = v3(0,0,0);
        b[pick].w   = v3(0,0,0);
        b[pick].drop = 0.0f;
        b[pick].pocket = 0;
        b[pick].on = 1;
        b[pick].orient = rand_orient();
        return b[pick].id;
    }
    return 0;
}

int cue_table_rack(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    int n;
    if (t->kind == CUE_GAME_BILLIARDS) n = rack_billiards(t, balls);
    else if (t->is_snooker)      n = rack_snooker(t, balls);
    else if (t->kind == CUE_GAME_US9) n = rack_9ball(t, balls);
    else if (CUE_GAME_IS_PYRAMID(t->kind)) n = rack_pyramid(t, balls);
    else                         n = rack_pool(t, balls);   /* UK8 + US8 */
    /* ONE PLACE STAMPS THE CUE BALL. Every rack builds balls[0] as the white,
     * and every game but English pool wants it the same size as the rest — so
     * the exception is applied here rather than in three racks, and it sits at
     * the height its own radius asks for. */
    cue_table_set_cue_ball(t, &balls[0]);
    return n;
}

void cue_table_set_cue_ball(const CueTable *t, CueBall *cue) {
    if (!cue) return;
    cue->r = (t->cue_R > 0.0f)    ? t->cue_R    : 0.0f;
    cue->m = (t->cue_mass > 0.0f) ? t->cue_mass : 0.0f;
    if (cue->r > 0.0f) cue->pos.y = cue->r;
}

/* ---- forced cue elevation --------------------------------------------- *
 * The cue is a SOLID TAPERED STICK sitting somewhere in the world, and the only
 * question is whether it is inside something. Nothing here knows about the cue
 * ball, and that is the point: the previous version modelled the SHOT — a shaft
 * starting at the contact point on the white and running back — which is fine
 * on the handheld, where the cue exists only at address, and nonsense in VR the
 * moment you pick the cue up. It went on answering "the shot from that ball
 * needs thirty degrees" while the player stood a metre away with the cue in the
 * air, and the cue was dragged off their hands to match.
 *
 * So: take the tip where it actually is, run back along the aim for the cue's
 * length, and ask what the stick would pass through. The answer is the smallest
 * elevation that clears all of it, pivoting about the tip.
 *
 * What it can be inside:
 *   - the bed, cloth at y = 0 across the playing area;
 *   - the RAIL BAND, from the cushion nose out to the frame edge, solid to the
 *     rail top. Its absence is why a cue could be laid under the rail and
 *     through the table: a cushion height alone says nothing about the plank
 *     beside it;
 *   - the balls;
 *   - and past the frame edge, nothing at all — that is open air.
 *
 * Clearance follows the taper, so the fat end of the cue is given the room it
 * actually needs rather than being treated as a line down its axis. */

#define CUE_SHAFT_REACH 1.45f       /* == CUEVR_CUE_LEN */
#define CUE_TIP_R    0.00475f       /* 9.5 mm tip ferrule */
#define CUE_BUTT_R   0.01500f       /* 30 mm at the butt */
#define CUE_ELEV_MAX 1.30f          /* steep masse; past this it is not a shot */

/* Half-thickness of the cue `dd` back from the tip. */
static float cue_shaft_r(float dd) {
    /* THE TIP RADIUS, ALL THE WAY BACK. Clearing the true taper is honest and
     * plays badly: the fat end is a metre behind the ball and its half-inch of
     * timber was forcing the cue up on shots a player would simply make. What
     * a player actually judges is whether the BUSINESS END clears, and the
     * rest of the cue is somewhere out over the rail past the point anyone
     * cares. A deliberate compromise, in the direction of feeling right. */
    (void)dd;
    return CUE_TIP_R;
}

/* Height of whatever is solid under (x,z). Off the table entirely returns a
 * large negative, meaning "nothing below you here". */
/* WHERE THE POCKETS ARE, from the table alone — the six centres, the hole bored
 * through the timber at each, the cloth cut around it, and which way the mouth
 * faces. The cue asks this with no world in its hand, so it is worked out from
 * the table's own numbers; the cut can be tuned per world on the pocket screen
 * and this uses the defaults, which is what every table has unless somebody has
 * been in there moving it. */
typedef struct { Vec3 c, cut_c, out; float bore, cut_r; } CuePocketGap;
static int cue_table_pocket_gaps(const CueTable *t, CuePocketGap *g) {
    const float k = 0.70710678f;
    const float hl = t->half_len, hw = t->half_wid;
    CueCut cc, cm;
    cue_table_default_cut(t->kind, 0, &cc);
    cue_table_default_cut(t->kind, 1, &cm);
    const float cx = hl + t->off_corner*k, cz = hw + t->off_corner*k;
    const float sx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float sz[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int p = 0; p < 4; p++) {
        g[p].c     = v3(sx[p]*cx, 0, sz[p]*cz);
        g[p].cut_c = v3(g[p].c.x + sx[p]*cc.set*k, 0, g[p].c.z + sz[p]*cc.set*k);
        g[p].out   = v3(sx[p]*k, 0, sz[p]*k);
        g[p].bore  = t->pr_corner;
        g[p].cut_r = t->pr_corner * cc.rad;
    }
    for (int p = 0; p < 2; p++) {
        float s = p ? 1.0f : -1.0f;
        g[4+p].c     = v3(0, 0, s*(hw + t->off_side));
        g[4+p].cut_c = v3(0, 0, g[4+p].c.z + s*cm.set);
        g[4+p].out   = v3(0, 0, s);
        g[4+p].bore  = t->pr_side;
        g[4+p].cut_r = t->pr_side * cm.rad;
    }
    return 6;
}

static float cue_table_surface(const CueTable *t, float x, float z) {
    float px = t->half_len, pz = t->half_wid;              /* cushion nose */
    float ox = px + t->rail_w, oz = pz + t->rail_w;        /* outer frame edge */
    float ax = fabsf(x), az = fabsf(z);
    if (ax <= px && az <= pz) return 0.0f;                 /* open cloth */
    if (ax > ox || az > oz) return -1.0e9f;                /* past the frame */

    /* THE RAIL HAS SIX HOLES IN IT — AND ONLY SIX HOLES.
     *
     * Everything outside the cushion nose used to be rail at full height, all
     * the way round, which puts timber across every pocket mouth and builds an
     * imaginary square corner out of two rails that in fact stop short of each
     * other with a pocket between them. A cue laid into a corner was lifted
     * over a cushion that is not there, on exactly the shots where it is most
     * often ON the rail: the white in the jaws, cueing out through the mouth.
     *
     * The hole is the one the wood actually has. cue_render bores the rail with
     * a circle of pr_corner / pr_side on the pocket CENTRE and fills the ring
     * around it — so that circle, and nothing wider, is where the timber is
     * missing. The curved back of the pocket and the frame behind it are real
     * and still turn the cue up.
     *
     * The cloth cut is a second, larger circle set further out, and it counts
     * only on the playing side of the pocket: that is the mouth between the
     * jaws, where the ball falls and there is no timber either. Past the pocket
     * it is just the shape the slate was cut to, with wood under it.
     *
     * Both are taken in by a tip radius, so the shaft is not asked to thread
     * the eye of a hole it only just fits through. */
    {   CuePocketGap g[6];
        int np = cue_table_pocket_gaps(t, g);
        for (int p = 0; p < np; p++) {
            float dx = x - g[p].c.x, dz = z - g[p].c.z;
            float rb = g[p].bore - CUE_TIP_R;
            if (rb > 0.0f && dx*dx + dz*dz < rb*rb) return -1.0e9f;
            if (dx * g[p].out.x + dz * g[p].out.z >= 0.0f) continue;  /* past it */
            float cx2 = x - g[p].cut_c.x, cz2 = z - g[p].cut_c.z;
            float rc = g[p].cut_r - CUE_TIP_R;
            if (rc > 0.0f && cx2*cx2 + cz2*cz2 < rc*rc) return -1.0e9f;
        }
    }

    /* EXACTLY WHAT IS DRAWN, and nothing on top of it.
     *
     * cue_render.c builds the cushion top and the wood top level with each
     * other at cushion_h * 1.30 — the rail cap is NOT a step above the cushion,
     * they are one surface. (cushion_h itself is the ball-CONTACT line at 63.5%
     * of ball height, which is a different thing again and easy to mistake for
     * the top of the cushion.)
     *
     * What was wrong here was an extra 0.085R — 2.2 mm — added above that. It
     * had the cue clearing a rail 2.2 mm taller than the one on screen, and on
     * the one shot where every millimetre counts, the white frozen on the
     * cushion with the shaft laid across it, 2.2 mm is most of the window. The
     * cue is a cylinder resting on a surface: its centreline sits one shaft
     * radius above, which cue_elev_for already adds, and there is nothing left
     * to account for. */
    return t->cushion_h * 1.30f;
}

/* Elevation needed for the shaft to sit above `surf` at distance `dd` back. */
static float cue_elev_for(float tip_y, float dd, float surf) {
    if (surf < -1.0f || dd <= 1.0e-4f) return 0.0f;
    float want = surf + cue_shaft_r(dd);
    if (want <= tip_y) return 0.0f;
    return atan2f(want - tip_y, dd);
}

float cue_table_min_elev(const CueTable *t, const CueBall *balls, int n,
                         Vec3 tip, float aim) {
    const float R = t->R;
    float bx = -cosf(aim), bz = -sinf(aim);      /* the stick runs BACK from the tip */
    float need = 0.0f;

    /* Walk back along the cue. Sampling alone can step over the rail edge, which
     * is the one place the answer jumps, so the four boundary crossings are
     * measured exactly and added to the samples. */
    const int NS = 24;
    for (int k = 1; k <= NS; k++) {
        float dd = CUE_SHAFT_REACH * (float)k / (float)NS;
        float e = cue_elev_for(tip.y, dd,
                               cue_table_surface(t, tip.x + bx*dd, tip.z + bz*dd));
        if (e > need) need = e;
    }
    {   /* the nose lines and the frame edges, hit exactly */
        float edge[4];
        edge[0] = t->half_len; edge[1] = t->half_wid;
        edge[2] = t->half_len + t->rail_w; edge[3] = t->half_wid + t->rail_w;
        for (int q = 0; q < 4; q++) {
            float lim = edge[q];
            float cand[2]; int nc = 0;
            if (q == 0 || q == 2) {
                if (bx >  1.0e-4f) cand[nc++] = ( lim - tip.x) / bx;
                if (bx < -1.0e-4f) cand[nc++] = (-lim - tip.x) / bx;
            } else {
                if (bz >  1.0e-4f) cand[nc++] = ( lim - tip.z) / bz;
                if (bz < -1.0e-4f) cand[nc++] = (-lim - tip.z) / bz;
            }
            for (int m = 0; m < nc; m++) {
                float dd = cand[m];
                if (dd <= 1.0e-4f || dd > CUE_SHAFT_REACH) continue;
                /* sample a whisker either side: the tall side is what binds */
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    float d2 = dd + (float)sgn * 0.002f;
                    if (d2 <= 1.0e-4f || d2 > CUE_SHAFT_REACH) continue;
                    float e = cue_elev_for(tip.y, d2,
                                cue_table_surface(t, tip.x + bx*d2, tip.z + bz*d2));
                    if (e > need) need = e;
                }
            }
        }
    }

    /* Balls. You go over them, never through, and the stick's own thickness
     * counts here too. balls[0] is the cue ball and is never an obstacle. */
    for (int i = 1; i < n; i++) {
        if (!balls[i].on) continue;
        float dx = balls[i].pos.x - tip.x, dz = balls[i].pos.z - tip.z;
        float along = dx*bx + dz*bz;
        if (along <= 0.0f || along > CUE_SHAFT_REACH) continue;
        float perp2 = (dx*dx + dz*dz) - along*along;
        if (perp2 < 0.0f) perp2 = 0.0f;
        float room = R + cue_shaft_r(along);
        if (perp2 < room*room) {
            /* A BALL IS A SPHERE. This asked for clearance over 2R — the very
             * crown — for any shaft passing within a ball's width of the
             * centre, so a cue skimming the far edge of a ball was lifted as
             * high as one going straight over the middle of it. That is a
             * cylinder, or near enough a cube, and it is why the planner
             * thought whole classes of ordinary shot needed the butt in the air.
             *
             * The real condition is the obvious one: the shaft's axis must stay
             * at least (R + shaft radius) away from the ball's centre in THREE
             * dimensions. With the axis at horizontal offset `perp` and height
             * y, that distance squared is perp^2 + (y - R)^2, so
             *
             *     y >= R + sqrt((R + r)^2 - perp^2)
             *
             * which gives 2R + r straight over the middle — the old answer, in
             * the one case it was right for — and falls smoothly to R at the
             * edge, where a cue alongside a ball needs no lift at all. */
            float need_y = R + sqrtf(room*room - perp2);
            if (need_y > tip.y) {
                float e = atan2f(need_y - tip.y, along);
                if (e > need) need = e;
            }
        }
    }

    if (need > CUE_ELEV_MAX) need = CUE_ELEV_MAX;
    return need;
}
