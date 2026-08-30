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
#include <stdlib.h>
#include <stdio.h>

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
    case CUE_GAME_US8: case CUE_GAME_US9: case CUE_GAME_US10:
    case CUE_GAME_STRAIGHT: case CUE_GAME_ONEPOCKET:
    case CUE_GAME_BANKPOOL:
    case CUE_GAME_ROTATION: case CUE_GAME_ROTATION_PH:
    case CUE_GAME_FIFTEEN: case CUE_GAME_COWBOY:
    case CUE_GAME_HONOLULU: case CUE_GAME_SPEED:
    case CUE_GAME_BOWLLIARDS: case CUE_GAME_CRIBBAGE:   /* K-66, worsted */
        t->e_cush = 0.985f; t->cush_efall = 0.046f; break;
    case CUE_GAME_UK8:                          /* championship English, Northern rubber */
        t->e_cush = 0.965f; t->cush_efall = 0.052f; break;
    case CUE_GAME_CN8:                          /* built to English patterns */
        t->e_cush = 0.968f; t->cush_efall = 0.050f; break;
    case CUE_GAME_CAROM_STRAIGHT: case CUE_GAME_CAROM_2C:
    case CUE_GAME_CAROM_3C: case CUE_GAME_CAROM_4B:
    case CUE_GAME_CAROM_1C:
        /* K-55 on a heated table: the ball is meant to hold speed through
         * three rails, which no pool cushion is asked to do. */
        t->e_cush = 0.990f; t->cush_efall = 0.038f; break;
    case CUE_GAME_PAUL:
        /* A HOME TABLE'S RUBBER, and the one place in this file where the
         * numbers go DOWN rather than sideways. Everything else here is a
         * championship table and the spread between them is three points of
         * published resilience; this is a folding 6 ft that cost a fortnight's
         * pocket money, and its cushions were never Northern rubber. Deader at
         * a crawl and falling off faster, which is what makes the game played
         * on it a game of pots rather than of position. */
        t->e_cush = 0.930f; t->cush_efall = 0.062f; break;
    default:                                    /* snooker, and 6-red on the English bed */
        t->e_cush = 0.970f; t->cush_efall = 0.050f; break;
    }
    t->e_cush_min = 0.55f;                      /* Marlow's rails, as the floor */
}

void cue_table_init(CueTable *t, CueGameKind kind) {
    /* KILLER borrows its base game's whole table — bed, pockets, rubber, ball
     * size — and only the rules differ; the kind is re-stamped so the rules
     * know what they are refereeing. The same trick the snookers play by
     * being three kinds, done by delegation instead of duplication. */
    if (CUE_GAME_IS_KILLER(kind)) {
        cue_table_init(t, kind == CUE_GAME_KILLER_UK ? CUE_GAME_UK8
                        : kind == CUE_GAME_KILLER_US ? CUE_GAME_US8
                                                     : CUE_GAME_CN8);
        t->kind = kind;
        return;
    }
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    /* ENGLISH BILLIARDS IS PLAYED ON A SNOOKER TABLE, and this flag is about
     * the TABLE — the bed, the pockets, the D, the four spots and the ball
     * size. It is not about the rules: cue_rules_init asks the kind as well,
     * because billiards scores cannons and in-offs and resolve_snooker would
     * make nonsense of it. One table, two quite different games, exactly as
     * UK 8-ball and 6-red snooker already share a bed. */
    /* PAUL IS ON THIS LIST because it is played with the snooker set — fifteen
     * reds and six colours — and this flag is about the TABLE and its balls,
     * not about the rules. Its scoring is nothing like snooker's. */
    t->is_snooker = (kind == CUE_GAME_SNK10 || kind == CUE_GAME_SNK15 ||
                     kind == CUE_GAME_SNK6  || kind == CUE_GAME_SNK3 ||
                     kind == CUE_GAME_BILLIARDS ||
                     kind == CUE_GAME_PAUL);

    /* THE ROUNDED JAW'S SHAPE, for every table, before any of them speak.
     * See CueTable::jaw_p0 for what the four points are. A STARTING POINT, in
     * millimetres and the same on every table, so the shape can be looked at
     * and compared before anything is tuned per table. */
    /* 70 mm, AND NOT RAISED, though raising it would be tidier in the abstract.
     *
     * How far out the facing leaves the rail decides how sharp its bend has to
     * be, and a tight bend bulges inside its own chord — so the narrowest part
     * of a pocket ends up being the curve rather than its ends. Past about
     * 110 mm that stops happening on every shape tried, and at 70 the shipped
     * corners are pinched by about 5%.
     *
     * They are left pinched deliberately. Their pocket sizes were tuned against
     * the openings that come out AT THIS VALUE, and those openings are what the
     * game plays like now; raising this would widen every shipped corner —
     * snooker from 1.60 ball widths to 1.68 — and re-tuning six numbers to undo
     * a change nobody asked for is how a table that plays well stops playing
     * well. A shape that actually needs a longer run says so for itself: see
     * the polygon presets in the workshop, where a triangle asks for 140. */
    t->jaw_p0  = 0.070f;   /* 70 mm out along the rail */
    /* The same as the corner, so every shipped table is exactly what it was.
     * See CueTable::jaw_p0_m for why they are separate at all. */
    t->jaw_p0_m = 0.070f;
    t->jaw_h1  = 0.030f;   /* 30 mm of rail tangent */
    t->jaw_h2  = 0.030f;   /* 30 mm of pocket-axis tangent */
    /* ZERO IS STRAIGHT DOWN THE POCKET'S OWN CENTRE LINE, at a corner and at a
     * middle alike. It used to be 45 at a corner because the centre line was
     * being reconstructed from the rail rather than read off the pocket, and 45
     * is what a rectangle's happens to be; now that the pocket carries its own
     * axis the number means what it says. */
    t->jaw_ang_c = 0.0f;
    /* TEN DEGREES OFF SQUARE, which is 80 and 100 to the rail on the two
     * sides — read off a photograph of a real middle pocket, where the throat
     * is plainly a slight funnel rather than a parallel slot. The two jaws
     * mirror about the pocket, so one number sets both. */
    t->jaw_ang_m = 10.0f;

    if (kind == CUE_GAME_UK8 || kind == CUE_GAME_SNK6 ||
        kind == CUE_GAME_SNK3 || kind == CUE_GAME_GOLF) {
        /* 7 ft UK pub 8-ball: 1.98 × 0.99 m, tight ROUNDED (curved) pockets.
         * Billiards golf is played on this bed too — it is a pub-table game
         * and the board it comes from is a home table's. */
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
        /* MILLIMETRES, NOT BALL RADII. A pocket is a hole of a given size;
         * how big the ball is decides whether it FITS, not how big the hole
         * is. Written in ball radii these numbers made every table a special
         * case and made a custom table impossible to author. The values are
         * the ones the table already had, to the micron, so nothing moves. */
        /* SET FROM THE OPENING A BALL ACTUALLY HAS TO PASS, in ball widths.
         *
         * The pocket size is the only authored number here and the opening is a
         * consequence of it, so the way to size a pocket is to say how wide the
         * opening should be and work back. Ball widths, because that is what
         * decides how much angular margin a pot has, and because the published
         * figures are measured at different places on the cushion and comparing
         * them as raw millimetres invites the wrong answer.
         *
         * Every corner on this engine measured 1.91-1.97 ball widths, and every
         * published specification puts a corner between 1.49 and 1.69. A quarter
         * of a ball too wide is a great deal of margin, and it played like it.
         *
         * 1.60 ball widths at both, which is the WEPF rule exactly: pocket
         * opening = ball x 1.6. And it needs no interpreting — the federation
         * measures it BETWEEN THE ENDS OF EACH CUSHION, which is where this
         * engine measures too. 81.3 mm on a 50.8 mm ball. */
        t->pr_corner  = 0.0411700f; t->pr_side  = 0.0412300f;   /* 41.17 / 41.23 mm */
        /* knuckles: what pr_corner + 0.833R / + 1.083R used to give */
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        /* Throat set back into the wood so the bore circle clears the (deepened)
         * cushion back and a proper wood ring is cut — see reach math in PLAN. */
        /* MILLIMETRES. Where the pocket sits into the corner, and how much
         * of the hole is not catch — both were ball radii, so a custom table
         * could not author them and the shipped tables were not examples of
         * anything. Values unchanged, to the micron. */
        t->off_corner = 0.0171450f; t->off_side = 0.0357175f;  /* 17.15 / 35.72 mm */
        /* Tuned on the bench: the catch IS the hole, and it sits deeper in. */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.0085725f; t->drop_back_side = 0.0045720f;  /* 8.57 / 4.57 mm */
        t->jaw_r = 0.004f;
        t->cloth = RGB565C(22, 120, 70);
        t->rail = RGB565C(96, 54, 26); t->rail_top = RGB565C(128, 78, 38);
        t->spot = RGB565C(180, 180, 180); t->nballs = 16;
        /* UK 8-ball baulk line + D (white placed in the D after a foul). */
        t->baulk_x = -t->half_len * 0.6f; t->d_radius = t->half_wid * 0.35f;
        /* GOLF never has more than the cue ball and four reds on the bed — the
         * biggest hole is a par 5 — so it does not carry a rack it will never
         * use. Everything else about the table is the UK 7 ft's. */
        if (kind == CUE_GAME_GOLF) t->nballs = 1 + CUE_GOLF_MAX_BALLS;
        if (kind == CUE_GAME_SNK6 || kind == CUE_GAME_SNK3) {
            /* 6-red snooker on the 7 ft UK table: same table geometry and ball
             * size as UK pool, but snooker balls/rules and snooker spots scaled
             * onto the small bed. THREE-RED is the same table again with a
             * two-row triangle — a shorter frame on the same bed, which is the
             * whole of the difference. */
            t->reds = (kind == CUE_GAME_SNK3) ? 3 : 6;
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
               kind == CUE_GAME_US10 || kind == CUE_GAME_STRAIGHT ||
               kind == CUE_GAME_ONEPOCKET || kind == CUE_GAME_BANKPOOL ||
               CUE_GAME_IS_ROT61(kind) || kind == CUE_GAME_COWBOY ||
               kind == CUE_GAME_HONOLULU || kind == CUE_GAME_SPEED ||
               kind == CUE_GAME_BOWLLIARDS || kind == CUE_GAME_CRIBBAGE) {
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
        /* MILLIMETRES, NOT BALL RADII. A pocket is a hole of a given size;
         * how big the ball is decides whether it FITS, not how big the hole
         * is. Written in ball radii these numbers made every table a special
         * case and made a custom table impossible to author. The values are
         * the ones the table already had, to the micron, so nothing moves. */
        t->pr_corner  = 0.0628650f; t->pr_side  = 0.0537210f;   /* 62.87 / 53.72 mm */
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        /* MILLIMETRES. Where the pocket sits into the corner, and how much
         * of the hole is not catch — both were ball radii, so a custom table
         * could not author them and the shipped tables were not examples of
         * anything. Values unchanged, to the micron. */
        t->off_corner = 0.0371475f; t->off_side = 0.0342900f;  /* 37.15 / 34.29 mm */
        /* Tuned on the bench: the catch IS the hole, and it sits deeper in. */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.0080010f; t->drop_back_side = 0.0085725f;  /* 8.00 / 8.57 mm */
        t->jaw_r = 0.004f;
        t->cloth = RGB565C(18, 110, 120);    /* US tables often tournament blue-green */
        t->rail = RGB565C(70, 46, 30); t->rail_top = RGB565C(100, 66, 42);
        t->spot = RGB565C(180, 180, 180);
        /* Golf never has more than the cue ball and four reds on the bed —
         * the hole with the most is a par 5 — so it does not carry a rack it
         * will never use. */
        /* Bowlliards racks the 1 to the 10, which is ten-ball's count and not
         * ten-ball's rack — the number of balls is the number of pins, and it
         * is the only thing the two games have in common. Cribbage pool takes
         * the whole fifteen and so wants the sixteen the fall-through already
         * gives it; a row of its own would only restate the default. */
        t->nballs = (kind == CUE_GAME_US9)  ? 10
                  : (kind == CUE_GAME_US10) ? 11
                  : (kind == CUE_GAME_BOWLLIARDS) ? 11
                  : (kind == CUE_GAME_COWBOY) ? 4 : 16;   /* cowboy: 1, 3, 5 */
    } else if (kind == CUE_GAME_PAUL) {
        /* PAUL: a 6 ft home snooker table, which is a real object and a
         * particular one — the folding kind that lives on top of a dining table
         * or on its own thin legs, and the reason two children could invent a
         * game on it at all.
         *
         * 6 ft by 3 ft of cloth, which is the size these are sold as, and the
         * numbers that come with it are all consequences:
         *
         *   42 mm BALLS. A home set is not a snooker set shrunk to fit; 42 mm
         *   is what comes in the box, and it is small enough that fifteen reds
         *   and six colours scattered over a 6 ft bed is a crowded table rather
         *   than an empty one. That crowding IS the game.
         *
         *   THIN CUSHIONS. A 1.8 m table cannot carry an 85 mm rail and still
         *   have a bed worth playing on: the rail is 45 mm, which is what these
         *   tables have, and it makes the pockets look and play tight because
         *   there is very little timber for the jaw to curve through.
         *
         *   A 26.7 mm NOSE, because the nose height is not a style choice. The
         *   WPA specification is 63.5% of the ball's diameter on every table
         *   there is, and 63.5% of 42 mm is 26.7. A cushion set to a full-size
         *   table's height would stand nearly two thirds of the way up a 42 mm
         *   ball and there would be no such thing as a follow shot.
         *
         *   AND 54 mm POCKETS, cut below. That is 1.29 ball widths against a
         *   snooker table's 1.60 — tighter in proportion than anything here
         *   except Russian pyramid, and on a bed this small every pot is a
         *   pot at a pocket you can see the far side of. */
        t->half_len = 1.829f * 0.5f;
        t->half_wid = 0.914f * 0.5f;
        t->R = 0.021f;                              /* 42 mm across */
        t->mass = 0.110f;                           /* a 42 mm phenolic ball */
        t->cue_R = 0.0f; t->cue_mass = 0.0f;        /* a matched white */
        t->cushion_h = t->R * 2.0f * 0.635f;        /* the WPA fraction, always */
        t->rail_w = 0.045f;
        t->pocket_round = 1;                        /* curved, like a snooker jaw */
        t->reds = 15;
        t->nballs = 22;                             /* the white and the full set */
        t->cloth = RGB565C(4, 135, 21);
        t->rail  = RGB565C(96, 62, 34);
        t->rail_top = RGB565C(120, 80, 44);
        t->spot = RGB565C(200, 200, 200);
        /* THE SPOTS AND THE D EXIST because the table has them printed on it,
         * and because an in-off has to put the white somewhere. Nothing is ever
         * spotted in Paul — a potted ball is gone — so the four spots are
         * scenery, and only the baulk line and the D do any work. Laid out in
         * the same proportions as the 7 ft snooker bed. */
        t->baulk_x  = -t->half_len * 0.6f;
        t->d_radius =  t->half_wid * 0.35f;
        t->blue_x   =  0.0f;
        t->pink_x   =  t->half_len * 0.5f;
        t->black_x  =  t->half_len * 0.82f;
        /* THE POCKET. Everything but the radius is the 12 ft snooker table's,
         * scaled by the ball rather than copied in millimetres — 0.800, which
         * is 21 mm over 26.25 — because these numbers are all about where a
         * ball sits in a hole and a smaller ball wants a smaller everything.
         * The radius itself is solved for a 54 mm MOUTH, which is the figure
         * that was asked for; see cue_table_openings for why a mouth cannot be
         * authored directly.
         *
         * THE BALL WENT UP FROM 40 mm TO 42 AND THE POCKET FROM 50 TO 54. The
         * ball-scaled numbers all moved by 21/20, and the radius had to be
         * RE-SOLVED rather than nudged: the opening is a consequence of the jaw
         * geometry AND the ball, so leaving the old radius alone under a
         * different ball moves the mouth on its own. cue_table_cut_to is what
         * lands it on the number asked for, every time it changes. */
        /* SOLVED FOR A 54 mm MOUTH at both, by cue_table_cut_to, and then
         * written down — the same way every other pocket in this file was
         * arrived at. The middle wants a SMALLER radius than the corner for the
         * same opening, which looks wrong and is not: a middle's two jaws face
         * each other across the pocket where a corner's meet at its axis, so
         * the same hole leaves a wider gap between them. Snooker's middles are
         * wider than its corners for the mirror-image reason. */
        t->pr_corner = 0.0337712f; t->pr_side = 0.0276264f;
        t->ang_corner = 60.0f; t->ang_side = 80.0f;
        t->off_corner = 0.0273000f; t->off_side = 0.0210000f;
        t->cap_corner = 0.0f;      t->cap_side = 0.0f;
        t->drop_back  = 0.0060900f; t->drop_back_side = 0.0132300f;
        t->jaw_r = 0.0095550f;
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
        /* MILLIMETRES, NOT BALL RADII. A pocket is a hole of a given size;
         * how big the ball is decides whether it FITS, not how big the hole
         * is. Written in ball radii these numbers made every table a special
         * case and made a custom table impossible to author. The values are
         * the ones the table already had, to the micron, so nothing moves. */
        /* SET FROM THE OPENING A BALL ACTUALLY HAS TO PASS, in ball widths.
         *
         * The pocket size is the only authored number here and the opening is a
         * consequence of it, so the way to size a pocket is to say how wide the
         * opening should be and work back. Ball widths, because that is what
         * decides how much angular margin a pot has, and because the published
         * figures are measured at different places on the cushion and comparing
         * them as raw millimetres invites the wrong answer.
         *
         * Every corner on this engine measured 1.91-1.97 ball widths, and every
         * published specification puts a corner between 1.49 and 1.69. A quarter
         * of a ball too wide is a great deal of margin, and it played like it.
         *
         * 1.50 ball widths, from heyball's quoted 3.84 in (85 mm) on a 57.1 mm
         * ball. Chinese 8 is the tightest of the three by some way, which is the
         * game: snooker pockets and cloth under American balls. 85.7 mm. */
        t->pr_corner  = 0.0441100f; t->pr_side  = 0.0428600f;   /* 44.11 / 42.86 mm */
        t->ang_corner = 45.0f; t->ang_side = 70.0f;
        /* MILLIMETRES. Where the pocket sits into the corner, and how much
         * of the hole is not catch — both were ball radii, so a custom table
         * could not author them and the shipped tables were not examples of
         * anything. Values unchanged, to the micron. */
        t->off_corner = 0.0171450f; t->off_side = 0.0357188f;  /* 17.15 / 35.72 mm */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.0088583f; t->drop_back_side = 0.0142875f;  /* 8.86 / 14.29 mm */
        t->jaw_r = 0.005f;
        t->cloth = RGB565C(22, 44, 155);     /* Chinese-8 royal blue cloth */
        t->rail = RGB565C(78, 48, 28); t->rail_top = RGB565C(112, 70, 36);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = 16;
    } else if (CUE_GAME_IS_CAROM(kind)) {
        /* G11 — CAROM. The international match table: 2.84 m by 1.42 m inside
         * the cushions, 61.5 mm balls at about 210 g, no pockets anywhere.
         * The cloth is heated and fast and the rubber (K-55) is the liveliest
         * in the building — the game is played off the rails. */
        t->half_len = 2.840f * 0.5f;
        t->half_wid = 1.420f * 0.5f;
        t->R = 0.0615f * 0.5f; t->mass = 0.210f;
        t->cushion_h = 1.27f * t->R; t->rail_w = 0.090f;
        /* no rail pockets: the jaw fields describe a hole never cut, exactly
         * as bar billiards does, because a zero mouth fails validation */
        t->pocket_round = 1;
        t->pr_corner = t->pr_side = 0.0309400f;
        t->jaw_r = 0.005f;
        /* no baulk line and no D: the line is parked at the cushion where the
         * chalk cannot draw it, and a zero radius is already "no D" */
        t->baulk_x = -t->half_len;
        t->d_radius = 0.0f;
        t->cloth = RGB565C(30, 120, 60);        /* tournament green-blue */
        t->rail = RGB565C(70, 42, 24); t->rail_top = RGB565C(100, 62, 32);
        t->spot = RGB565C(200, 200, 200);
        t->nballs = (kind == CUE_GAME_CAROM_4B) ? 4 : 3;
    } else if (kind == CUE_GAME_BARBILLIARDS) {
        /* G6 — BAR BILLIARDS, and almost nothing above applies to it.
         *
         * AEBBA Rule 71: the playing area is 138.4 to 143.5 cm long and at
         * least 78.7 cm wide, inside the cushions. There are NO pockets on the
         * rails — the rails are four plain cushions all the way round — and the
         * scoring is nine holes bored through the bed, which cue_table_build_world
         * lays out because their arrangement is a constant of the game rather
         * than anything a player would dial.
         *
         * Rule 91: every shot is played with the cue ball in the D at the near
         * end. Rule 79: one red and seven white balls. */
        t->half_len = 1.420f * 0.5f;
        t->half_wid = 0.790f * 0.5f;
        t->R = 0.0238f; t->mass = 0.120f;      /* 1 7/8 in, and light with it */
        t->cushion_h = 1.20f * t->R; t->rail_w = 0.055f;
        /* There are no pocket jaws to shape, but the fields are read all over
         * and a zero mouth would fail validation. They describe a hole that is
         * never cut on a rail: the pockets this table has are in the bed. */
        t->pocket_round = 1;
        t->pr_corner = t->pr_side = 0.0309400f;   /* 30.94 mm; no rail pockets */
        t->ang_corner = 45.0f; t->ang_side = 80.0f;
        t->off_corner = t->off_side = 0.0238000f;   /* 23.80 mm */
        t->cap_corner = t->cap_side = 0.0f;
        t->drop_back  = t->drop_back_side = 0.0f;
        t->jaw_r = 0.006f;
        /* Rule 75: a "D" of about 4 cm radius at the centre of the base, its
         * centre the break spot. Rule 76: the red spot 17.1 to 17.9 cm up the
         * table from it. Rule 77: the baulk arc out to the side cushions. */
        t->baulk_x  = -t->half_len + 0.060f;   /* the break spot */
        t->d_radius = 0.040f;
        t->baulk_arc = 155.0f;      /* degrees between the two baulk lines */
        /* The red spot rides in blue_x, which is the field every renderer and
         * every rack already asks for a mark on the centre line. */
        t->blue_x  = t->baulk_x + 0.175f;
        t->pink_x  = 0.0f;
        /* The 200 hole, which is at the PLAYER'S end — see the hole table in
         * cue_table_build_world. It was written as half_len - 0.070, the far
         * end, which is where the 200 used to be wrongly put. */
        t->black_x = -t->half_len + 0.760f;   /* the 200 hole */
        t->cloth = RGB565C(24, 96, 52);
        t->rail = RGB565C(64, 40, 24); t->rail_top = RGB565C(92, 58, 30);
        t->spot = RGB565C(215, 215, 200);
        t->nballs = 8;                         /* Rule 79: a red and seven whites */
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
        /* MILLIMETRES, NOT BALL RADII. A pocket is a hole of a given size;
         * how big the ball is decides whether it FITS, not how big the hole
         * is. Written in ball radii these numbers made every table a special
         * case and made a custom table impossible to author. The values are
         * the ones the table already had, to the micron, so nothing moves. */
        /* SET AGAINST THE FEDERATION'S OPENINGS, which is the only pyramid
         * number a player would recognise: 72-74 mm at a corner and 82-84 in
         * a middle. The opening is not authored — the link puts the cushions
         * on the bore — so these were swept until the measured mouth landed
         * in those bands: 36.0 gives 73.5 and 47.0 gives 83.0, against a 67 mm
         * ball. Change the depth or the splay and they want re-sweeping, which
         * is what the workshop's opening readout is for. */
        /* PER BED, because the two do not share a ball. The 12 ft plays a
         * 67 mm ball and the 7 ft a 57.15 mm one, so the same hole is a
         * different pocket on each — the small bed had 14 mm of corner slack
         * where the big one had 5. The federation quotes the smaller tables
         * their own openings for exactly this reason. */
        if (home) { t->pr_corner = 0.0310000f;   /* 31.0 mm radius */
                    t->pr_side   = 0.0391000f; } /* 39.1 mm radius */
        else      { t->pr_corner = 0.0370000f;   /* 37.0 mm radius */
                    t->pr_side   = 0.0463000f; } /* 46.3 mm radius */
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
        /* AUTHORED, LIKE EVERY OTHER TABLE. This block used to derive its own
         * way — gap, facing and offset as ratios of pr_corner, the middle's
         * gap off the ball — which made pyramid a third convention beside the
         * ball-relative tables and the millimetre ones. It is millimetres now
         * and the values are the ones it had.
         *
         * The gap is not authored at all any more: the link decides where a
         * cushion ends, from the bore, on every table. The facing follows from
         * the mitre angle and the cushion depth. Both are gone rather than
         * left to look meaningful. */
        t->ang_corner = 45.0f;
        /* THE MIDDLE'S FACINGS RUN ALMOST PARALLEL. A pool middle splays at 70
         * degrees off the rail and the mouth tapers in like a funnel; the
         * photographs of a Russian table show the two cushion ends facing each
         * other across a slot, which is a splay of very nearly ninety. Left a
         * couple of degrees short of parallel because a cushion end always has
         * a little relief on it, and dead parallel reads as a machined slot. */
        t->ang_side   = 88.0f;
        t->off_corner = 0.0212760f;   /* 21.28 mm */
        t->off_side   = 0.0259990f;   /* 26.00 mm */
        /* THE CATCH IS THE ONE THING THAT SCALES WITH THE BALL, not the mouth:
         * it is about how far a BALL has to travel past the cushion line before
         * it has gone, and a 68 mm ball needs the same fraction of itself
         * whatever size the hole is. The American gets 0.90 and 0.68 of a ball
         * radius; at cap = 0 this table would get 0.44 of one, which is a ball
         * sitting on the lip. A negative cap makes the catch larger than the
         * mouth — see the note beside cap_corner in TAB_FIELDS. */
        /* ZERO, LIKE EVERY OTHER TABLE. This carried a negative catch — the
         * drop circle swollen to 51.4 mm behind a 36 mm bore — so the hole a
         * ball fell into was half as big again as the hole in the timber. It
         * was doing the job the opening should do: reaching out past the jaws
         * to take a ball that could not really fit. With the pocket sizes set
         * from the federation's openings the ball fits for real, and the drop
         * can be the bore like everywhere else.
         *
         * This was the last of pyramid's private conventions. */
        t->cap_corner = 0.0f;
        t->cap_side   = 0.0f;
        t->drop_back  = 0.0093800f; t->drop_back_side = 0.0100500f;  /* 9.38 / 10.05 mm */
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
        /* THE POCKET IS THE 1.7 BORE. Now that the drop, the timber's hole and
         * the cushion ends are one circle, the size of that circle is the one
         * number left to pick — and 1.7's BORE is the number that was tuned by
         * hand in the headset, not its pr. The corners were dialled wider than
         * the drop (2.11 and 2.13 against 1.98) and the middles narrower (1.65
         * and 1.71 against 1.82), so this widens a corner and tightens a
         * middle, which is what those numbers say the table should be.
         *
         * Per table, because 1.7 tuned them apart: the 10ft's corner is a
         * touch wider than the 12ft's and its middle wider too. */
        /* SET FROM THE OPENING A BALL ACTUALLY HAS TO PASS, in ball widths.
         *
         * The pocket size is the only authored number here and the opening is a
         * consequence of it, so the way to size a pocket is to say how wide the
         * opening should be and work back. Ball widths, because that is what
         * decides how much angular margin a pot has, and because the published
         * figures are measured at different places on the cushion and comparing
         * them as raw millimetres invites the wrong answer.
         *
         * Every corner on this engine measured 1.91-1.97 ball widths, and every
         * published specification puts a corner between 1.49 and 1.69. A quarter
         * of a ball too wide is a great deal of margin, and it played like it.
         *
         * CORNER 1.60 ball widths = 84.0 mm, which is the tournament band the
         * WPBSA templates are quoted at (3.25-3.35 in) whichever end of the
         * cushion it is measured from. MIDDLE 1.73 = 90.8 mm, and it is WIDER
         * than the corner — the templates give 3.5 in at a corner and 4 in at a
         * middle, because a ball arrives at a middle across the pocket rather
         * than down its axis and needs the room. This engine had it the other
         * way round, a 1.91 corner against a 1.60 middle.
         *
         * The 10 ft and the 12 ft take the same numbers: same ball, same rail,
         * so the same pocket. 1.7 had them a touch apart and there is no reason
         * in the specification for it.
 */
        t->pr_corner = 0.0452900f; t->pr_side = 0.0466700f;  /* 45.29 / 46.67 mm */
        t->ang_corner = 60.0f; t->ang_side = 80.0f;
        /* Throat set well back into the wood: the small snooker pocket radius is
         * < the deepened cushion depth, so without this the bore circle never
         * reaches the wood and no cutaway is cut (the fall is realistically set
         * back behind the mouth anyway). */
        /* MILLIMETRES. Where the pocket sits into the corner, and how much
         * of the hole is not catch — both were ball radii, so a custom table
         * could not author them and the shipped tables were not examples of
         * anything. Values unchanged, to the micron. */
        t->off_corner = 0.0341250f; t->off_side = 0.0262500f;  /* 34.13 / 26.25 mm */
        /* Tuned on the bench: the catch IS the hole, and it sits deeper in. */
        t->cap_corner = 0.0f;         t->cap_side = 0.0f;
        t->drop_back  = 0.0076125f; t->drop_back_side = 0.0165375f; /* 7.61 / 16.54 mm */
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
        kind == CUE_GAME_US10 || kind == CUE_GAME_STRAIGHT ||
        kind == CUE_GAME_ONEPOCKET || kind == CUE_GAME_BANKPOOL ||
        CUE_GAME_IS_ROT61(kind) || kind == CUE_GAME_COWBOY ||
        kind == CUE_GAME_HONOLULU || kind == CUE_GAME_SPEED ||
        kind == CUE_GAME_BOWLLIARDS || kind == CUE_GAME_CRIBBAGE)
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
    case CUE_GAME_US8: case CUE_GAME_US9: case CUE_GAME_US10:
    case CUE_GAME_STRAIGHT: case CUE_GAME_ONEPOCKET:
    case CUE_GAME_BANKPOOL:
    case CUE_GAME_ROTATION: case CUE_GAME_ROTATION_PH:
    case CUE_GAME_FIFTEEN: case CUE_GAME_COWBOY: case CUE_GAME_HONOLULU:
    case CUE_GAME_SPEED: case CUE_GAME_BOWLLIARDS:
    case CUE_GAME_CRIBBAGE:
        t->bore_corner = 1.8900f * t->R; t->bore_side = 1.7600f * t->R;
        t->bore_set_corner = 0.0000f * t->R; t->bore_set_side = 0.0000f * t->R;
        break;
    case CUE_GAME_CN8:
        t->bore_corner = 1.8700f * t->R; t->bore_side = 1.7800f * t->R;
        t->bore_set_corner = 0.5400f * t->R; t->bore_set_side = 0.4500f * t->R;
        break;
    case CUE_GAME_UK8: case CUE_GAME_SNK6: case CUE_GAME_SNK3:
        t->bore_corner = 2.0700f * t->R; t->bore_side = 1.8900f * t->R;
        t->bore_set_corner = 0.0000f * t->R; t->bore_set_side = 0.1800f * t->R;
        break;
    case CUE_GAME_PAUL:
        /* Concentric, and equal to the hole. There is 45 mm of rail to work in
         * and the pocket is 34 mm across the radius, so there is no room to set
         * the bore back into timber that is barely there. */
        t->bore_corner = t->pr_corner; t->bore_side = t->pr_side;
        t->bore_set_corner = 0.0f; t->bore_set_side = 0.0f;
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

    /* THE GAP AND THE FACING ARE NOT AUTHORED. Nine tables used to carry a
     * gap_corner/gap_side and a facing_len apiece, and not one of them
     * survived the build: cue_table_link_gap decides where a cushion ends,
     * from the bore, and a mitre's facing follows from its angle and the
     * cushion's depth. They were rows a table designer could move with no
     * effect, which is worse than no row at all — so they are gone, and what
     * is left here is a SEED, enough for the first build the link then reads
     * its jaw tips from.
     *
     * Off the pocket, not off the ball, so a seed cannot reintroduce the thing
     * this whole scheme removes. */
    t->gap_corner = 2.60f * t->pr_corner;
    t->gap_side   = 2.60f * t->pr_side;
    t->facing_len = 0.80f * t->pr_corner;

    /* ---- WHAT THIS TABLE IS DRESSED IN --------------------------------- *
     *
     * The fittings, as opposed to the shape. A pub table has chrome castings on
     * its corners and dots on its rails; an American one has black mouldings
     * and diamonds; a match snooker table has neither, because it has string
     * pockets and a collector rail instead and neither of those is built yet.
     *
     * Written here rather than being offered as a preference, because it is
     * what those tables ARE. The workshop lets you dress one you built however
     * you like; a preset carries a whole CueTable, so it keeps what you chose.
     *
     * A LINER ON EVERYTHING WITH A DROP, for now. Cheap tables and dear ones
     * all line the hole with something; what changes is what the corner casting
     * is made of. Bar billiards is bored through open cloth with no rail round
     * the holes, and carom has no pockets at all, so both get nothing. */
    {   const int carom = (kind >= CUE_GAME_CAROM_STRAIGHT &&
                           kind <= CUE_GAME_CAROM_4B) ||
                          kind == CUE_GAME_CAROM_1C;
        if (!carom && kind != CUE_GAME_BARBILLIARDS) {
            t->furniture = CUE_FURN_LINER;
            switch (kind) {
            /* THE UK PUB TABLE and everything played on its bed: chrome on the
             * corners, round dots on the rails. */
            case CUE_GAME_UK8: case CUE_GAME_KILLER_UK:
            case CUE_GAME_GOLF:
                t->furniture |= CUE_FURN_CORNERCAP | CUE_FURN_SIGHTS;
                break;
            /* THE AMERICAN BED and its long list of games: a black moulding on
             * the corners, and diamonds rather than dots. */
            case CUE_GAME_US8: case CUE_GAME_US9: case CUE_GAME_US10:
            case CUE_GAME_KILLER_US: case CUE_GAME_KILLER_CN:
            case CUE_GAME_STRAIGHT:
            case CUE_GAME_ONEPOCKET: case CUE_GAME_BANKPOOL:
            case CUE_GAME_ROTATION: case CUE_GAME_ROTATION_PH:
            case CUE_GAME_FIFTEEN: case CUE_GAME_COWBOY:
            case CUE_GAME_HONOLULU: case CUE_GAME_SPEED:
            case CUE_GAME_BOWLLIARDS: case CUE_GAME_CRIBBAGE:
                t->furniture |= CUE_FURN_CORNERCAP | CUE_FURN_CAP_BLACK
                              | CUE_FURN_SIGHTS | CUE_FURN_DIAMONDS;
                break;
            /* THE FULL-SIZE SNOOKER BEDS wear what a club table wears: string
             * bags hung on leather-wrapped pocket plates, and the wire
             * collector each drop returns its ball along. NO LINER -- that is a
             * moulded plastic lip for a drop that ends in a tray, and there is
             * nothing here for one to line. */
            /* AND THE SMALL SNOOKER TABLES TOO. Six-red and three-red share
             * the 7 ft bed with UK pool, and they were wearing the pub table's
             * chrome and dots because of it -- but the bed is the only thing
             * they share. It is a snooker table: string bags on leather pocket
             * plates and a collector, at 7 ft instead of 12. Nothing else about
             * the table changes, because the furniture is dressing. */
            /* CHINESE 8-BALL IS ON THIS LIST because it is built like a
             * snooker table: a 10 ft bed with rounded pockets, a heavy timber
             * cabinet on turned legs and string pockets over a collector. It
             * was wearing the American bed's black mouldings and diamonds
             * because it shared that branch, and it shares none of the table.
             * A heyball rail is plain too -- the marked rail is an American
             * convention and this table no more follows it than snooker does. */
            case CUE_GAME_SNK15: case CUE_GAME_SNK10:
            case CUE_GAME_SNK6:  case CUE_GAME_SNK3:
            case CUE_GAME_CN8:
            case CUE_GAME_BILLIARDS:
                t->furniture = CUE_FURN_NETS | CUE_FURN_COLLECTOR;
                break;
            /* Billiards and pyramid keep the liner and nothing else until the
             * furniture they actually wear exists. */
            default: break;
            }
        } }

    cue_table_normalise(t);
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
    TF(bed_shape,       TF_I32, TF_SIM,  0.0f, 2.0f),
    /* SIM: which way the L turns is the wall a ball bounces off. */
    TF(bed_hand,        TF_I32, TF_SIM,  0.0f, 1.0f),
    TF(notch_x,         TF_F32, TF_SIM,  0.0f, 3.20f),
    TF(notch_z,         TF_F32, TF_SIM,  0.0f, 3.20f),
    /* SIM, obviously: how many cushions there are is the shape of the table.
     * Three is the fewest bed anyone can play on; the upper bound is headroom
     * for a round one, which is this same construction with enough sides to
     * read as a curve. */
    TF(bed_sides,       TF_I32, TF_SIM,  0.0f, 64.0f),
    TF(bed_pocket_every,TF_I32, TF_SIM,  0.0f, 64.0f),
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
    TF(baulk_arc,       TF_F32, TF_SIM,  0.000f, 180.0f),
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
    /* THE ROUNDED JAW'S SHAPE. SIM, because the curve these two describe is
     * the cushion a ball bounces off — the renderer and the collision world
     * are built from the same segments. Appended at the END so a block written
     * before they existed still reads: cue_table_unpack fills what the block
     * carries and leaves the rest, and build_world treats a zero bulge as
     * "never authored" rather than "dead straight". */
    /* THE ROUNDED JAW'S FOUR POINTS, of which three are numbers. All SIM: the
     * curve they describe IS the cushion a ball bounces off, and the renderer
     * and the collision world are built from the same segments, so two ends
     * that disagree about it are playing pockets of different widths. */
    TF(jaw_p0,          TF_F32, TF_SIM,  0.010f, 0.300f),
    /* Zero is legal here and means "follow the corner" — see jaw_p0_m. */
    TF(jaw_p0_m,        TF_F32, TF_SIM,  0.000f, 0.300f),
    TF(jaw_h1,          TF_F32, TF_SIM,  0.000f, 0.200f),
    TF(jaw_h2,          TF_F32, TF_SIM,  0.000f, 0.200f),
    TF(jaw_ang_c,       TF_F32, TF_SIM, -40.0f, 60.0f),
    TF(jaw_ang_m,       TF_F32, TF_SIM, -60.0f, 60.0f),
    /* HOW FAST THE CLOTH IS. SIM without question — it decides where every ball
     * comes to rest, so two ends that disagree about it are not playing the
     * same frame. Zero is legal and means the engine's own 0.010, which is what
     * every table saved before this field reads back as. The range is about
     * half to three times that: new worsted on a match table at one end, a
     * tired napped club cloth at the other. */
    TF(mu_r,            TF_F32, TF_SIM,  0.000f, 0.040f),
    /* WHAT THE TABLE IS DRESSED IN. LOOK, not SIM: a liner in the drop and a
     * casting on the corner are fittings, and no ball touches any of them, so
     * this must not enter the hash two identical beds are matched by. Appended,
     * like every field before it, which is what lets a file written without it
     * still be read -- see cue_table_unpack. */
    TF(furniture,       TF_I32, TF_LOOK, 0.0f, 255.0f),
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

static int tab_bytes_n(int nf) {
    int n = 4;
    if (nf > TAB_NFIELD) nf = TAB_NFIELD;
    for (int i = 0; i < nf; i++)
        n += (TAB_FIELDS[i].type == TF_U16) ? 2 : 4;
    return n;
}
static int tab_bytes(void) { return tab_bytes_n(TAB_NFIELD); }

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
    /* A FILE WRITTEN BEFORE A FIELD WAS ADDED STILL DESCRIBES A TABLE.
     *
     * Every field this format has ever gained was APPENDED, so a shorter block
     * is a PREFIX of a longer one: read what is there and leave the rest at the
     * zero cue_table_init would have given it. Refusing outright -- which is
     * what an exact count did -- threw away every table a player had built the
     * first time the list grew, which is a high price for a field about what
     * colour a pocket liner is.
     *
     * A LONGER block is refused, because that is a file from a future this code
     * knows nothing about and guessing at it is worse than saying no. */
    const int nf = (int)in[1];
    if (nf > TAB_NFIELD) return 0;
    if (len < tab_bytes_n(nf)) return 0;

    CueTable tmp;
    memset(&tmp, 0, sizeof tmp);
    int at = 4;
    for (int i = 0; i < nf; i++) {
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

    /* BAR BILLIARDS IS A RECTANGLE OR IT IS NOTHING.
     *
     * It is the one game here whose pockets are not cut into the boundary at
     * all: nine holes bored through the open cloth at coordinates fixed by the
     * AEBBA layout, four plain cushions round them, and no rail pocket
     * anywhere. cue_table_build_world takes its own branch for that — a branch
     * which sits BEFORE the polygon and L ones and so is the only one it ever
     * takes — so its cushions are always the four sides of a rectangle at
     * +-half_len/+-half_wid whatever bed_shape says, and its holes are always
     * where a 1.42 m by 0.79 m table put them.
     *
     * Ask for a hexagonal one and you get a square of cushions inside a
     * hexagonal frame; ask for a deep L and the cushions run straight through
     * the missing corner with two of the holes stranded in it. Neither is a
     * table, and the honest place to say so is here rather than in the four
     * separate places that would each have to cope. The workshop does not offer
     * the game at all, and this is what makes that a fact about the table
     * rather than a fact about the menu. */
    if (t->kind == CUE_GAME_BARBILLIARDS && t->bed_shape != CUE_BED_RECT)
        return tab_fail(msg, msgcap,
                        "bar billiards has its holes in the bed, so its bed must be a rectangle");
    /* CAROM IS ONE FIXED MATCH TABLE. Its cushions are four plain rails built
     * as a rectangle, and its variant list offers no shapes — but validate is
     * a separate gate, and without this a hexagon or a triangle could be built
     * with rectangular rails on a bed that is not one. Caught by test_frame,
     * which builds every frame design on every shape and found a face wound
     * against its own normal on a triangular carom table. */
    if (CUE_GAME_IS_CAROM(t->kind) && t->bed_shape != CUE_BED_RECT)
        return tab_fail(msg, msgcap,
                        "carom is played on one fixed rectangular table");

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
/* `ax,az` is the pocket's own centre line, pointing OUT of the pocket — see
 * CueWorld::paxis. Every caller has it to hand because it is the direction the
 * pocket was offset from its corner along. */
static void add_pocket(CueWorld *w, float x, float z, float cap, int mid,
                       float ax, float az) {
    if (w->npocket >= CUE_MAX_POCKET) return;
    int i = w->npocket++;
    w->pocket[i] = v3(x, 0, z);
    w->pocket_r[i] = cap;
    w->pocket_mid[i] = (unsigned char)(mid ? 1 : 0);
    const float l = sqrtf(ax*ax + az*az);
    w->paxis[i] = (l > 1e-6f) ? v3(ax/l, 0.0f, az/l) : v3(0.0f, 0.0f, 1.0f);
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
/* THE STRAIGHTS KISS THE CIRCLE, AND THE ARC BETWEEN THEM IS THE KNUCKLE.
 *
 * The rattle circle was dropped near the vertex and then recessed behind the
 * nose (jaw_r + 0.15 R) to keep it out of the throat — which put it somewhere a
 * ball can never touch. At a convex corner the two flats always reach a ball
 * first, so whatever the circle's radius, the faces shield it. Measured on the
 * 9 ft American middle: the circle sat 8.3 mm behind the knuckle and a ball
 * walked straight at it stopped 33-37 mm away against a 32.6 mm contact radius.
 * It never once registered, and what the ball met instead was the FACING, whose
 * normal looks into the mouth — so a ball onto the point was deflected IN
 * rather than thrown out.
 *
 * The circle only becomes real when the flats STOP at it. So it is placed where
 * it is tangent to both faces — the incircle of the corner, centre on the
 * bisector at r/sin(half) — and each straight is cut back to its own tangent
 * point, r/tan(half) from the vertex. The arc between those two points is then
 * exposed, and it is the outermost thing at the knuckle: a ball meets the
 * circle, and the existing jaw-circle collision does the rest.
 *
 * THE FACE LINES DO NOT MOVE. Only their ENDS do — every trimmed endpoint stays
 * exactly on the line it was already on, so the angle a ball rebounds at, and
 * the opening the pocket presents, are the ones the table was cut to. */
static void mitre_kiss(Vec3 v, Vec3 toward_a, Vec3 toward_b, float r,
                       Vec3 *ta, Vec3 *tb, Vec3 *c) {
    Vec3 ua = v3_norm(v3(toward_a.x - v.x, 0.0f, toward_a.z - v.z));
    Vec3 ub = v3_norm(v3(toward_b.x - v.x, 0.0f, toward_b.z - v.z));
    float dot = ua.x*ub.x + ua.z*ub.z;
    if (dot >  0.9999f) dot =  0.9999f;
    if (dot < -0.9999f) dot = -0.9999f;
    const float half = acosf(dot) * 0.5f;
    const float th = tanf(half), sh = sinf(half);
    float d = (th > 1e-4f) ? r / th : 0.0f;
    /* A tangent point can never run past the end of the face it is on. */
    const float la = v3_len(v3(toward_a.x-v.x, 0.0f, toward_a.z-v.z));
    const float lb = v3_len(v3(toward_b.x-v.x, 0.0f, toward_b.z-v.z));
    const float lim = 0.45f * ((la < lb) ? la : lb);
    if (d > lim) d = lim;
    *ta = v3(v.x + ua.x*d, 0.0f, v.z + ua.z*d);
    *tb = v3(v.x + ub.x*d, 0.0f, v.z + ub.z*d);
    Vec3 bis = v3_norm(v3(ua.x + ub.x, 0.0f, ua.z + ub.z));
    const float cd = (sh > 1e-4f) ? (d * th) / sh : 0.0f;
    *c = v3(v.x + bis.x*cd, 0.0f, v.z + bis.z*cd);
}

/* The exposed arc, drawn on the circle the straights were cut back to. Sized by
 * its own length — about a millimetre a step — because CUE_JAW_SEGS is ten and
 * ten steps round three millimetres of arc is a chain full of 0.3 mm segments
 * for nothing. */
static void add_arc_between(CueWorld *w, Vec3 c, Vec3 a0, Vec3 a1) {
    const float r = sqrtf((a0.x-c.x)*(a0.x-c.x) + (a0.z-c.z)*(a0.z-c.z));
    if (r <= 1e-5f) return;
    float s0 = atan2f(a0.z - c.z, a0.x - c.x);
    float s1 = atan2f(a1.z - c.z, a1.x - c.x);
    float d = s1 - s0;
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    int n = (int)((r * (d < 0.0f ? -d : d)) / 0.0010f + 0.5f);
    if (n < 3) n = 3;
    if (n > 6) n = 6;
    Vec3 prev = a0;
    for (int i = 1; i <= n; i++) {
        const float a = s0 + d * (float)i / (float)n;
        Vec3 p = v3(c.x + r*cosf(a), 0.0f, c.z + r*sinf(a));
        add_seg(w, prev, p, 1);
        prev = p;
    }
}

/* ONE MITRED POCKET RUN — facing, nose, facing — with each knuckle rounded onto
 * its own jaw circle. kn_a/kn_b say whether that end is a knuckle at all: an
 * n-gon vertex carrying no pocket, and the L's reflex corner, run straight on
 * with no facing and no circle.
 *
 * THIS IS THE ONLY PLACE A MITRED JAW IS BUILT. A rectangle arrives through
 * add_chain and every other bed shape through add_run, and that is the whole
 * point of the function existing: the knuckle was fitted to the rectangle and
 * the octagon never got it, because add_run emitted its own facings inline and
 * dropped a RECESSED circle at the vertex instead. That circle sits 8.3 mm
 * behind the point, where no ball can reach it — which is the fault Mark hit on
 * a middle pocket: the ball met the FACING, whose normal points into the mouth,
 * and was deflected in rather than rebounding off the point. Same jaw, same
 * numbers, one code path. */
static void add_mitred(CueWorld *w, Vec3 P1, Vec3 P2, Vec3 P3, Vec3 P4,
                       int kn_a, int kn_b, Vec3 nin) {
    const float r = w->jaw_r;
    if (r <= 1e-5f) {                    /* no radius authored: the old corner */
        if (kn_a) add_seg(w, P1, P2, 1);
        add_seg(w, P2, P3, 0);
        if (kn_b) add_seg(w, P3, P4, 1);
        if (kn_a) add_jaw_recessed(w, P2, nin);
        if (kn_b) add_jaw_recessed(w, P3, nin);
        return;
    }
    Vec3 a2, b2, c2, a3, b3, c3;
    Vec3 ns = P2, ne = P3;                     /* where the rail nose really runs */
    if (kn_a) { mitre_kiss(P2, P1, P3, r, &a2, &b2, &c2); ns = b2; }
    if (kn_b) { mitre_kiss(P3, P2, P4, r, &a3, &b3, &c3); ne = a3; }
    /* IN BOUNDARY ORDER, and it has to be: the renderer joins pieces by testing
     * whether one segment's b is the next one's a, walking the array and looking
     * at s-1 and s+1 and nothing else. */
    if (kn_a) { add_seg(w, P1, a2, 1); add_arc_between(w, c2, a2, b2); }
    add_seg(w, ns, ne, 0);
    if (kn_b) { add_arc_between(w, c3, a3, b3); add_seg(w, b3, P4, 1); }
    /* THE CIRCLE STAYS, AND THE ARC IS DRAWN ON IT.
     *
     * The arc is what the RENDERER needs — a chain with a gap in it draws a
     * notch at the knuckle, which is what a trimmed pair of straights looks
     * like with nothing between them. The circle is what the PHYSICS has always
     * used for a rattle, and it is the same locus as the arc, so a ball pushed
     * out by an arc segment sits exactly ON the circle and the circle then
     * finds no penetration to resolve. One shape, described twice, for two
     * readers. */
    if (kn_a) add_jaw(w, c2);
    if (kn_b) add_jaw(w, c3);
}

static void add_chain(CueWorld *w, Vec3 P1, Vec3 P2, Vec3 P3, Vec3 P4) {
    add_mitred(w, P1, P2, P3, P4, 1, 1, inward_n(P2.x, P2.z, P3.x, P3.z));
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
    /* THE FACING'S LENGTH IS NOT A BALL PROPERTY. facing_len is authored as a
     * multiple of the ball (or of the pocket), but what a mitre has to do is
     * reach the timber: it goes out by sin(ang) per unit length, so arriving
     * at a frame edge cw deep takes cw / sin(ang) and nothing else comes into
     * it. The authored value stays as the fallback for a world built without a
     * cushion depth. Same unlinking as the rectangle mitres, which is where it
     * was done first; this is the L and n-gon path. */
    const float sl_auth = t->facing_len;
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
            nIn = w->jaw_segs;
        }
        if (end_b != LEND_REFLEX) {
            jaw_tip(w, P3, v3(-d.x, 0, -d.z), out, end_b == LEND_MIDDLE, &T4, &aOut);
            nOut = w->jaw_segs;
        }
        add_curved_chain_e(w, T1, P2, P3, T4, aIn, aOut, nIn, nOut,
                           end_a != LEND_REFLEX, end_b != LEND_REFLEX);
        return;
    }

    /* THE MITRE, through the same builder the rectangle uses — so the knuckle
     * radius, the tangent jaw circle and the boundary order are the shape's,
     * not this function's. What used to be here was a second copy of all three,
     * and the copy is what left every non-rectangular bed with a sharp point
     * and a circle behind it. */
    Vec3 P1 = P2, P4 = P3;
    if (end_a != LEND_REFLEX) {
        float ang = (end_a == LEND_MIDDLE) ? t->ang_side : t->ang_corner;
        float c = cosf(ang*DEG), s2 = sinf(ang*DEG);
        const float sl = (w->cush_depth > 1e-6f && s2 > 1e-4f)
                       ? (w->cush_depth / s2) : sl_auth;
        P1 = v3(P2.x - d.x*(c*sl) + out.x*(s2*sl), 0,
                P2.z - d.z*(c*sl) + out.z*(s2*sl));
    }
    if (end_b != LEND_REFLEX) {
        float ang = (end_b == LEND_MIDDLE) ? t->ang_side : t->ang_corner;
        float c = cosf(ang*DEG), s2 = sinf(ang*DEG);
        const float sl = (w->cush_depth > 1e-6f && s2 > 1e-4f)
                       ? (w->cush_depth / s2) : sl_auth;
        P4 = v3(P3.x + d.x*(c*sl) + out.x*(s2*sl), 0,
                P3.z + d.z*(c*sl) + out.z*(s2*sl));
    }
    add_mitred(w, P1, P2, P3, P4,
               end_a != LEND_REFLEX, end_b != LEND_REFLEX, nin);
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

/* ---- S2: THE REGULAR BEDS ------------------------------------------------ *
 *
 * Triangle, square, pentagon, hexagon, heptagon, octagon — and a round table,
 * which is the same construction with enough sides to read as a curve.
 *
 * All of them are ONE object: N corners on a circumcircle with a pocket every
 * so many of them. That is why this costs a fraction of what an arbitrary
 * polygon would: every one is convex, so there is no reflex corner, none of the
 * elbow machinery is needed, and "on the bed" is "inside all N edges".
 *
 * THE EDGE THAT MATTERS IS THE ONE THE PACK FACES. Vertices are placed so an
 * edge is centred on +x, which is where the rack goes: break into a flat
 * cushion, never into a pocket. On an even-sided bed that puts a flat edge at
 * BOTH ends, so the baulk gets one too and nothing is given up. On an odd one
 * it cannot — three, five and seven corners cannot face flat both ways — and
 * the pack end wins, leaving a corner pocket at the baulk end. That is the
 * right way round: a pack sitting in the jaws of a pocket is a broken table,
 * where a D beside a corner is merely a different table.
 *
 * A SQUARE HERE IS NOT A RECTANGLE WITH EQUAL SIDES. It has four pockets, one
 * per corner, where a rectangle has six. Different table, not a differently
 * proportioned one. */
/* THE HALF-LENGTH ALONG THE SPINE — the distance from the middle of the table
 * to the cushion the rack sits in front of.
 *
 * On a rectangle and an L that is half_len, and every marking on a table is
 * already written as a fraction of it: the baulk line at six tenths, the foot
 * spot at a half, the pink halfway to the top cushion. On a REGULAR bed
 * half_len is the circumradius, which reaches the CORNERS — and there is a
 * pocket at every corner, so it is the one distance no marking should ever be
 * measured against. The cushion in front of the rack is at the apothem.
 *
 * Feeding the apothem through the same fractions is the whole of "mapped
 * traditional": the table keeps each game's own proportions and simply stops
 * measuring them to a place where there is no cushion. */
float cue_table_axis(const CueTable *t) {
    if (!t) return 0.0f;
    if (t->bed_shape != CUE_BED_NGON) return t->half_len;
    return t->half_len * cosf(3.14159265f / (float)cue_table_ngon_sides(t));
}

/* ...and across it. A regular bed is as wide as it is long, and its narrowest
 * half-width is the same apothem, so anything laid out across the spine uses
 * that rather than half_wid — which on these beds is the circumradius again. */
float cue_table_across(const CueTable *t) {
    if (!t) return 0.0f;
    return (t->bed_shape == CUE_BED_NGON) ? cue_table_axis(t) : t->half_wid;
}

int cue_table_ngon_sides(const CueTable *t) {
    int n = t ? t->bed_sides : 0;
    if (n < 3) n = 3;
    if (n > CUE_MAX_BEDV) n = CUE_MAX_BEDV;
    return n;
}

Vec3 cue_table_ngon_vert(const CueTable *t, int i) {
    const int n = cue_table_ngon_sides(t);
    const float r = t->half_len;
    const float a = 3.14159265f / (float)n + 6.2831853f * (float)i / (float)n;
    return v3(r * cosf(a), 0.0f, r * sinf(a));
}

static void build_ngon(CueWorld *w, const CueTable *t) {
    const int n = cue_table_ngon_sides(t);
    int every = t->bed_pocket_every < 1 ? 1 : t->bed_pocket_every;
    if (every > n) every = n;

    /* THE GAP IS A PULL-BACK, AND A PULL-BACK IS NOT A MOUTH.
     *
     * gap_corner says how far each run stops short of the vertex. The OPENING
     * that leaves is the distance between the two stopped ends, which for an
     * interior angle A is 2*g*sin(A/2) — so the same number means a different
     * mouth at every angle. Tuned on a rectangle's 90 degrees, it gave:
     *
     *     triangle 80.8   square 109.5   hexagon 130.1   round 143.1 mm
     *
     * for one 50.8 mm ball. A triangle you could not get a ball into beside a
     * round table you could not miss.
     *
     * Scaling the pull-back by sin(45)/sin(A/2) puts the mouth back where the
     * rectangle has it, and the factor is exactly 1 at 90 degrees — so a square
     * bed is untouched to the last bit, and so is the L, whose corners are all
     * right angles and whose elbow takes no gap at all. Measured after: 111.9,
     * 109.5, 106.2, 99.2. The residue is the bezier knuckle's own shape, which
     * does not sit exactly on the pull-back point and drifts as the corner
     * opens; closing that as well means solving for the mouth rather than
     * scaling toward it, and is worth doing only if the eye can see it.
     *
     * Applied by copying the table rather than by teaching add_run about
     * angles: a run knows its own two ends and nothing about the shape they
     * belong to, and it should stay that way. */
    CueTable tt = *t;
    {   const float A = 3.14159265f * (float)(n - 2) / (float)n;   /* interior */
        const float k = sinf(0.7853982f) / sinf(A * 0.5f);
        tt.gap_corner *= k;
        tt.gap_side   *= k;
    }
    t = &tt;

    /* THE CUSHION RUNS. One per edge, and the outward normal falls out of the
     * winding — (dz, -dx) for the order these vertices come in — so there is no
     * table of hand-written normals to get wrong the way the L had.
     *
     * A corner that carries no pocket is not an end at all: the runs either
     * side of it want to meet, not to grow a pair of facings pointing at
     * nothing. That is what LEND_REFLEX already means to add_run — no gap, no
     * facing, no jaw — so a round bed's plain corners borrow it. */
    /* THE SEGMENT BUDGET, SHARED OUT BEFORE ANYTHING IS BUILT. Each run costs
     * two jaws and a nose, so n runs want n*(2*JAW+1); when that exceeds the
     * array the tail of the loop silently gets nothing. Trim the jaw instead,
     * and keep at least one segment so a knuckle is still a knuckle. */
    {   /* MEASURED, not assumed: a jaw of js steps emits js+2 segments, and
         * only the corners that CARRY a pocket grow one — a bed with a pocket
         * every `every` corners has n/every of them, each with two. So
         *
         *     n + 2 * npocket * (js + 2)  <=  the array
         *
         * A first cut at this counted js per jaw and n runs with jaws on both
         * ends, which over-spent on a twelve-gon (still two rails bare) and
         * under-spent on a round bed, where fifty-four of the sixty corners
         * are plain and cost one segment each. */
        int npk = 0;
        for (int i = 0; i < n; i++) if (!(i % every)) npk++;
        int js = CUE_JAW_SEGS;
        if (npk > 0) {
            /* Three quarters of the array, not all of it. The cost of a jaw
             * is js+2 by measurement, but only roughly — the chain adds and
             * drops segments at its ends depending on what it meets — and
             * budgeting to the last segment left a sixteen-gon one rail short
             * and a thirty-gon two. Headroom is cheaper than an exact model of
             * something that does not need to be exact, and what it buys is
             * that no rail can ever come out bare. */
            js = (((CUE_MAX_SEG * 3) / 4 - n) / (2 * npk)) - 2;
            if (js > CUE_JAW_SEGS) js = CUE_JAW_SEGS;
            if (js < 1) js = 1;
        }
        w->jaw_segs = js;
    }
    /* BEFORE THE RUNS: a rounded jaw finishes at its pocket's bore, so the
     * pocket has to exist first. See the same note in build_L. */
    /* THE POCKETS, in outline order, each pushed out along its own bisector —
     * which for a regular polygon is simply the radial direction, by symmetry. */
    const float oc = t->off_corner;
    const float capc = t->pr_corner - t->cap_corner;
    for (int i = 0; i < n; i++) {
        if (i % every) continue;
        const Vec3 v = cue_table_ngon_vert(t, i);
        const float l = sqrtf(v.x*v.x + v.z*v.z);
        if (l < 1e-5f) continue;
        /* A regular bed's bisector is simply the radial, by symmetry — and it
         * is 180/n degrees off a rail's normal, not 45. */
        add_pocket(w, v.x + v.x / l * oc, v.z + v.z / l * oc, capc, 0,
                   v.x / l, v.z / l);
    }

    for (int i = 0; i < n; i++) {
        const Vec3 a = cue_table_ngon_vert(t, i);
        const Vec3 b = cue_table_ngon_vert(t, (i + 1) % n);
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float l = sqrtf(dx*dx + dz*dz);
        if (l < 1e-5f) continue;
        const Vec3 out = v3(dz / l, 0.0f, -dx / l);
        add_run(w, t, a, b, out,
                (i % every)             ? LEND_REFLEX : LEND_CORNER,
                ((i + 1) % n) % every   ? LEND_REFLEX : LEND_CORNER);
    }
#ifdef MOTE_HOST
    /* CUE_NGONDUMP=1 — does the cushion actually REACH the pocket it is cut
     * for? A jaw that stops short leaves bare cloth in the mouth and a bore
     * with nothing running into it, which is what a round bed was reported
     * for. Printed as the closest cushion point to each bore's rim: zero or
     * less means the cushion arrives, positive is the gap. */
    {   const char *e = getenv("CUE_NGONDUMP");
        if (e) {
            float edge = 0.0f;
            {   const Vec3 a0 = cue_table_ngon_vert(t, 0);
                const Vec3 b0 = cue_table_ngon_vert(t, 1 % n);
                edge = sqrtf((b0.x-a0.x)*(b0.x-a0.x) + (b0.z-a0.z)*(b0.z-a0.z)); }
            for (int q = 0; q < w->npocket; q++) {
                float best = 1e9f;
                for (int sgi = 0; sgi < w->nseg; sgi++) {
                    const Vec3 P[2] = { w->seg[sgi].a, w->seg[sgi].b };
                    for (int e2 = 0; e2 < 2; e2++) {
                        const float dx2 = P[e2].x - w->pocket[q].x;
                        const float dz2 = P[e2].z - w->pocket[q].z;
                        const float d2 = sqrtf(dx2*dx2 + dz2*dz2) - w->pocket_r[q];
                        if (d2 < best) best = d2;
                    }
                }
                printf("NGON n %d edge %.4f jaw_segs %d | pocket %d rim gap %+.4f"
                       " | pr %.4f\n",
                       n, edge, w->jaw_segs, q, best, w->pocket_r[q]);
            }
        }
    }
#endif

    /* ...and the outline itself, so the physics can say what is cloth. */
    w->nbedv = n;
    for (int i = 0; i < n; i++) {
        const Vec3 v = cue_table_ngon_vert(t, i);
        w->bedv_x[i] = v.x; w->bedv_z[i] = v.z;
    }
}

int cue_table_L_outline(const CueTable *t, Vec3 *v, int *reflex) {
    if (!t || !v || t->bed_shape != CUE_BED_L) return 0;
    const float hl = t->half_len, hw = t->half_wid;
    const float nx = t->notch_x,  nz = t->notch_z;
    if (cue_table_hand(t) >= 0.0f) {
        /* the bite out of the +x,+z corner */
        v[0] = v3(-hl,      0, -hw);
        v[1] = v3( hl,      0, -hw);
        v[2] = v3( hl,      0,  hw - nz);
        v[3] = v3( hl - nx, 0,  hw - nz);            /* the reflex corner */
        v[4] = v3( hl - nx, 0,  hw);
        v[5] = v3(-hl,      0,  hw);
        if (reflex) *reflex = 3;
    } else {
        /* ...and out of the +x,-z one. Walked the same way round, so the
         * winding — and every normal that falls out of it — still holds; the
         * inside corner simply arrives a vertex earlier. */
        v[0] = v3(-hl,      0, -hw);
        v[1] = v3( hl - nx, 0, -hw);
        v[2] = v3( hl - nx, 0, -hw + nz);            /* the reflex corner */
        v[3] = v3( hl,      0, -hw + nz);
        v[4] = v3( hl,      0,  hw);
        v[5] = v3(-hl,      0,  hw);
        if (reflex) *reflex = 2;
    }
    return 6;
}

static void build_L(CueWorld *w, const CueTable *t) {
    Vec3 V[6]; int rf = 3;
    if (!cue_table_L_outline(t, V, &rf)) return;
    const float hl = t->half_len, hw = t->half_wid;
    const float nx = t->notch_x, nz = t->notch_z;

    /* WHICH TWO RAILS GET A MIDDLE POCKET: the two that run the WHOLE way.
     *
     * On a rectangle the long rails are obvious. On an L four of the six are
     * shortened by the notch, and a middle on a shortened rail is a pocket
     * halfway along an arm too short to want one, sitting a few inches from a
     * corner. The two full ones are simply the two longest, which is true
     * whichever way the table turns and needs no table of indices. */
    float len[6]; int lo0 = 0, lo1 = 1;
    for (int i = 0; i < 6; i++) {
        Vec3 a = V[i], b = V[(i + 1) % 6];
        len[i] = sqrtf((b.x-a.x)*(b.x-a.x) + (b.z-a.z)*(b.z-a.z));
    }
    for (int i = 0; i < 6; i++) {
        if (len[i] > len[lo0]) { lo1 = lo0; lo0 = i; }
        else if (i != lo0 && len[i] > len[lo1]) lo1 = i;
    }

    /* THE ELBOW'S RADIUS. Both runs into the inside corner stop short of it by
     * this and an arc joins them, so the chain is continuous and the corner is
     * a curve rather than a knife edge a ball could squeeze through. A ball and
     * a half is what a cushion bends to, clamped so a shallow notch cannot ask
     * for more than the runs it comes out of. */
    float er = 1.5f * t->R;
    {   float lim = 0.35f * (nx < nz ? nx : nz);
        if (er > lim) er = lim;
        if (er < 0.0f) er = 0.0f; }

    /* ROUND THE OUTLINE. The outward normal of an edge falls out of the
     * winding — (dz, -dx) for a counter-clockwise walk — exactly as a regular
     * bed's does, so there is no table of hand-written normals to get wrong and
     * nothing here asks which way the table turns. */
    /* BEFORE THE RUNS, because a rounded jaw is built against its pocket: the
     * curve finishes at the yellow point, where the bore circle crosses the
     * frame's inner face, and the bore circle IS the pocket. Built the other
     * way round the jaws found no pocket and came out as bare mitres — which is
     * exactly what test_lshape caught. */
    /* THE POCKETS, in the order the boundary meets them — five outer corners
     * and the two middles — each pushed out along the bisector of its own two
     * outward normals, which is NOT always away from the table centre. The
     * corner where the far rail meets the underside of the notch has cloth to
     * -x and -z of it, so it belongs pushed out INTO the notch; reading the
     * direction off the sign of the coordinate, which every rectangle here can
     * get away with, put it back inside the playing area.
     *
     * There is deliberately NO pocket at the elbow: on a real L the inside
     * corner is solid timber and the two cushions meet there.
     *
     * IN OUTLINE ORDER, not corners-then-middles: the cloth boundary is a walk
     * round the table and wants them in the order it meets them. A rectangle
     * can sort six pockets back into order from their coordinates; an L
     * cannot. See build_bed_boundary_L, which relies on this. */
    const float oc = t->off_corner, os = t->off_side;
    const float capc = t->pr_corner - t->cap_corner;
    const float caps = t->pr_side   - t->cap_side;
    for (int i = 0; i < 6; i++) {
        const int ia = i, ib = (i + 1) % 6, ip = (i + 5) % 6;
        Vec3 a = V[ia], b = V[ib], pv = V[ip];
        /* THE CORNER AT THIS EDGE'S START comes before this edge's middle, so
         * the array runs in the order the boundary walks it. */
        if (ia != rf && len[ip] > 1e-5f && len[i] > 1e-5f) {
            float p1x = (a.x - pv.x) / len[ip], p1z = (a.z - pv.z) / len[ip];
            float p2x = (b.x - a.x) / len[i],   p2z = (b.z - a.z) / len[i];
            /* the bisector of the two edges' outward normals */
            float bx = p1z + p2z, bz = -p1x - p2x;
            float bl = sqrtf(bx*bx + bz*bz);
            if (bl > 1e-6f)
                add_pocket(w, a.x + bx/bl * oc, a.z + bz/bl * oc, capc, 0,
                           bx/bl, bz/bl);
        }
        if ((i == lo0 || i == lo1) && len[i] > 1e-5f) {
            const float dx = b.x - a.x, dz = b.z - a.z;
            const Vec3 out = v3(dz / len[i], 0.0f, -dx / len[i]);
            add_pocket(w, (a.x + b.x) * 0.5f + out.x * os,
                          (a.z + b.z) * 0.5f + out.z * os, caps, 1,
                          out.x, out.z);
        }
    }

    for (int i = 0; i < 6; i++) {
        const int ia = i, ib = (i + 1) % 6;
        Vec3 a = V[ia], b = V[ib];
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float l = len[i];
        if (l < 1e-5f) continue;
        const Vec3 out = v3(dz / l, 0.0f, -dx / l);
        const int enda = (ia == rf) ? LEND_REFLEX : LEND_CORNER;
        const int endb = (ib == rf) ? LEND_REFLEX : LEND_CORNER;
        if (i == lo0 || i == lo1) {
            /* split at the middle, and put a pocket there */
            Vec3 m = v3((a.x + b.x) * 0.5f, 0, (a.z + b.z) * 0.5f);
            add_run(w, t, a, m, out, enda,        LEND_MIDDLE);
            add_run(w, t, m, b, out, LEND_MIDDLE, endb);
        } else if (ib == rf && er > 0.0f) {
            /* stop short of the inside corner; the arc below turns it */
            add_run(w, t, a, v3(b.x - dx/l*er, 0, b.z - dz/l*er), out,
                    enda, LEND_REFLEX);
        } else if (ia == rf && er > 0.0f) {
            add_run(w, t, v3(a.x + dx/l*er, 0, a.z + dz/l*er), b, out,
                    LEND_REFLEX, endb);
        } else {
            add_run(w, t, a, b, out, enda, endb);
        }
        if (ib == rf && er > 0.0f) {
            /* the arc itself, between this edge's normal and the next one's */
            Vec3 c = V[(ib + 1) % 6];
            float ex2 = c.x - b.x, ez2 = c.z - b.z;
            float l2 = sqrtf(ex2*ex2 + ez2*ez2);
            if (l2 > 1e-5f)
                add_elbow(w, b, out, v3(ez2/l2, 0.0f, -ex2/l2), er);
        }
    }

}

/* Sample nseg+1 points along a quadratic-bezier curve from s to e with a
 * perpendicular bulge (matches the 2D game's generateCurvePoints), inclusive of
 * both ends. Returns the point count. */
/* Which pocket a point belongs to, by nearest. The same rule the cushion link
 * uses, and it saves threading a pocket index through four call sites that do
 * not otherwise care. */
static int near_pocket(const CueWorld *w, Vec3 q) {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < w->npocket; i++) {
        const float dx = q.x - w->pocket[i].x, dz = q.z - w->pocket[i].z;
        const float d = dx*dx + dz*dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* THE YELLOW POINT: where the bore circle crosses the frame's inner face.
 *
 * The frame's inner face is the cushion face pushed out by the cushion's depth,
 * so it is a line parallel to the rail. The bore is a circle. Their crossing is
 * where the rubber meets the wood, and therefore where a jaw has to finish — it
 * is not a shape decision, it is what the timber already decided.
 *
 * `rd` must point AWAY from the pocket along the rail. There are two crossings
 * and the far one is round the back of the pocket, so the larger root is the
 * one the cushion can actually reach.
 *
 * Returns 0 when the bore does not reach the face at all, which is a pocket too
 * small to build against that depth of cushion rather than a fault to paper
 * over — see cue_table_warnings. */
static int jaw_yellow(Vec3 nose, Vec3 rd, Vec3 outn, float cush,
                      Vec3 bore_c, float bore_r, Vec3 *out) {
    const Vec3 f0 = v3(nose.x + outn.x*cush, 0.0f, nose.z + outn.z*cush);
    const float ax = f0.x - bore_c.x, az = f0.z - bore_c.z;
    const float b = rd.x*ax + rd.z*az;
    const float c = ax*ax + az*az - bore_r*bore_r;
    const float disc = b*b - c;
    if (disc < 0.0f) return 0;
    const float t = -b + sqrtf(disc);
    *out = v3(f0.x + rd.x*t, 0.0f, f0.z + rd.z*t);
    return 1;
}

/* THE ROUNDED JAW, as a cubic with both ends pinned.
 *
 * See CueTable::jaw_p0 for what the four points are. Here they are assembled:
 * P3 is handed in already solved, P0 sits jaw_p0 along the rail from P3's foot
 * on the cushion face, and the two control points slide along the two tangents
 * — the rail at one end, the pocket's axis at the other.
 *
 * Points come out P0 first and P3 last, so the caller gets rail-end to
 * mouth-end and can reverse it for the outgoing half of a rail. */
static int jaw_curve(const CueWorld *w, Vec3 p0, Vec3 end, Vec3 axis, Vec3 rd,
                     int nseg, Vec3 *out) {
    /* The tangent at P0 is the rail, running back towards the pocket; the
     * tangent at the far end is the pocket's own axis, running back out to the
     * bed. The two control points slide along those, and that is the shape. */
    const Vec3 c1 = v3(p0.x - rd.x*w->jaw_h1, 0.0f, p0.z - rd.z*w->jaw_h1);
    const Vec3 c2 = v3(end.x - axis.x*w->jaw_h2, 0.0f, end.z - axis.z*w->jaw_h2);
    if (nseg < 1) nseg = 1;
    out[0] = p0;
    for (int i = 1; i <= nseg; i++) {
        const float t = (float)i/(float)nseg, o = 1.0f - t;
        const float b0 = o*o*o, b1 = 3.0f*o*o*t, b2 = 3.0f*o*t*t, b3 = t*t*t;
        out[i] = v3(b0*p0.x + b1*c1.x + b2*c2.x + b3*end.x, 0.0f,
                    b0*p0.z + b1*c1.z + b2*c2.z + b3*end.z);
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
/* THE FACING'S TANGENT AT THE MOUTH: the pocket's own centre line, turned by an
 * authored angle towards the pocket.
 *
 * The centre line comes off the world now (CueWorld::paxis) rather than being
 * guessed as "45 degrees off the rail's normal". That guess is exactly right on
 * a rectangle and a square and wrong on every other polygon — the true bisector
 * at an n-gon corner is 180/n, so a triangle's was 15 degrees out and its
 * pockets measured 1.19 ball widths against a square's 1.60.
 *
 * `rd` runs AWAY from the pocket along this rail, so turning towards -rd turns
 * into the throat; and because the two ends of a rail get opposite rd, the pair
 * mirrors about the pocket for free. Zero leaves the facing running straight
 * down the centre line, which is the parallel-sided throat.
 */
static Vec3 jaw_tangent(Vec3 axis, Vec3 rd, float ang) {
    /* the part of -rd square to the axis: the direction to turn towards */
    const float d = -(rd.x*axis.x + rd.z*axis.z);
    Vec3 t = v3(-rd.x - axis.x*d, 0.0f, -rd.z - axis.z*d);
    const float tl = sqrtf(t.x*t.x + t.z*t.z);
    if (tl < 1e-6f) return axis;                  /* rail along the axis */
    t = v3(t.x/tl, 0.0f, t.z/tl);
    const float c = cosf(ang * DEG), s2 = sinf(ang * DEG);
    return v3_norm(v3(axis.x*c + t.x*s2, 0.0f, axis.z*c + t.z*s2));
}

/* ONE END OF A RAIL, as the four points of its jaw.
 *
 * `ok` is 0 when the bore never reaches the frame's inner face: there is then
 * no yellow point, so no curve, and the caller runs the straight nose out to
 * the old knuckle instead of inventing a shape. cue_table_warnings reports it.
 */
typedef struct { Vec3 p0, y, nose, nrm, axis, centre, pk; int ok; } CueJawEnd;

static CueJawEnd jaw_end(const CueWorld *w, Vec3 k, Vec3 rd, Vec3 outn) {
    CueJawEnd e; e.ok = 0; e.p0 = k; e.y = k; e.nose = k; e.nrm = outn;
    e.axis = outn; e.centre = outn; e.pk = k;
    /* WHICH POCKET THIS END RUNS INTO: the nearest, which is the same rule the
     * cushion link uses. The pockets are added before the chains now precisely
     * so this can be asked. */
    const int p = near_pocket(w, k);
    if (p < 0) return e;
    e.pk = w->pocket[p];
    const int mid = w->pocket_mid[p];
    /* TWO DIFFERENT AXES, and running them together was wrong.
     *
     * The pocket's CENTRE LINE is geometry: the bisector of two square rails at
     * a corner, square-on to the rail at a middle. The drop is set back along
     * THAT, and so is the bore, because cue_table_normalise puts the hole in the
     * timber on the hole the ball falls into.
     *
     * The FACING'S TANGENT at the yellow point is a choice, and at a middle it
     * is not the centre line — a real middle's facings are cut a few degrees off
     * square. Using the tangent for the setback as well pushed the bore centre
     * sideways off the pocket's own line, which put the yellow point off the
     * drop circle entirely: the cushion ended inside the hole. Caught by
     * drawing it. */
    /* THE POCKET'S OWN, recorded when it was placed. Falls back to the rail's
     * normal for a pocket that never got one, which is a pocket with no rails. */
    Vec3 centre = w->paxis[p];
    if (centre.x*centre.x + centre.z*centre.z < 0.25f) centre = outn;
    e.axis = jaw_tangent(centre, rd, mid ? w->jaw_ang_m : w->jaw_ang_c);
    e.centre = centre;
    const float bset = mid ? w->drop_back_side : w->drop_back;
    const Vec3 bc = v3(w->pocket[p].x + centre.x*bset, 0.0f,
                       w->pocket[p].z + centre.z*bset);
    Vec3 y;
    if (!jaw_yellow(k, rd, outn, w->cush_depth, bc, w->pocket_r[p], &y)) return e;
    e.y = y;
    /* THE CURVE ENDS ON THE YELLOW POINT, and the cushion's back running on
     * into the rail behind it is correct — that is what the timber is for. The
     * only thing that must not happen is cushion showing INSIDE the bore, and
     * that is dealt with where the mesh is built (cue_render: bore clipping)
     * rather than by moving the curve. `nrm` is the curve's outward normal at
     * that end, kept for placing the cap. */
    Vec3 nv = v3(y.x - w->pocket[p].x, 0.0f, y.z - w->pocket[p].z);
    {   const float al = nv.x*centre.x + nv.z*centre.z;
        nv = v3(nv.x - centre.x*al, 0.0f, nv.z - centre.z*al);
        const float nl = sqrtf(nv.x*nv.x + nv.z*nv.z);
        nv = (nl > 1e-6f) ? v3(nv.x/nl, 0.0f, nv.z/nl) : outn;
    }
    e.nrm  = nv;
    e.nose = y;
    /* P0: back along the rail from the yellow point's foot on the cushion face. */
    const Vec3 foot = v3(y.x - outn.x*w->cush_depth, 0.0f, y.z - outn.z*w->cush_depth);
    /* A MIDDLE'S CURVE STARTS ON ITS OWN NUMBER — see CueTable::jaw_p0_m. It
     * turns through nearly twice a corner's angle, so the same run does the
     * opposite thing to it. */
    const float run = mid ? w->jaw_p0_m : w->jaw_p0;
    e.p0 = v3(foot.x + rd.x*run, 0.0f, foot.z + rd.z*run);
    e.ok = 1;
    return e;
}

static void add_curved_chain_e(CueWorld *w, Vec3 tipIn, Vec3 kIn, Vec3 kMid,
                               Vec3 tipMid, float aIn, float aOut,
                               int nIn, int nOut, int jawIn, int jawMid) {
    /* Sized for the jaw itself, not for CUE_MAX_SEG: these are stack arrays and
     * the device has very little of it. CUE_JAW_SEGS is 3 there and 10 in VR. */
    #define CUE_JAW_MAXPTS 34
    Vec3 in[CUE_JAW_MAXPTS], out[CUE_JAW_MAXPTS];
    if (nIn  > CUE_JAW_MAXPTS - 2) nIn  = CUE_JAW_MAXPTS - 2;
    if (nOut > CUE_JAW_MAXPTS - 2) nOut = CUE_JAW_MAXPTS - 2;
    (void)tipIn; (void)tipMid; (void)aIn; (void)aOut;
    const float railLen = sqrtf((kMid.x-kIn.x)*(kMid.x-kIn.x) +
                                (kMid.z-kIn.z)*(kMid.z-kIn.z));
    if (railLen < 1e-5f) return;
    const Vec3 rdir = v3((kMid.x-kIn.x)/railLen, 0.0f, (kMid.z-kIn.z)/railLen);
    /* Outward: away from the cloth, which is where the timber is. */
    const Vec3 nin  = inward_n(kIn.x, kIn.z, kMid.x, kMid.z);
    const Vec3 outn = v3(-nin.x, 0.0f, -nin.z);

    /* Each end's rail runs away from its own pocket, so they get opposite rd. */
    const CueJawEnd A = (nIn  > 0) ? jaw_end(w, kIn,  rdir, outn)
                                   : (CueJawEnd){kIn, kIn, outn, outn, kIn, 0};
    const CueJawEnd B = (nOut > 0) ? jaw_end(w, kMid, v3(-rdir.x,0.0f,-rdir.z), outn)
                                   : (CueJawEnd){kMid, kMid, outn, outn, kMid, 0};

    /* THE STRAIGHT NOSE RUNS BETWEEN THE TWO P0s. If a jaw could not be built
     * its end keeps the old knuckle, so the rail still closes. And if the two
     * P0s have crossed over — a jaw_p0 longer than half the rail — the nose is
     * dropped rather than emitted backwards, which would face the wrong way. */
    const Vec3 q1 = A.ok ? A.p0 : kIn;
    const Vec3 q2 = B.ok ? B.p0 : kMid;

    int ni = 0, no = 0;
    if (A.ok) {
        /* Built P0-first and reversed: the chain runs mouth -> rail -> rail ->
         * mouth, so this half arrives at the nose rather than leaving it. */
        Vec3 tmp[CUE_JAW_MAXPTS];
        const int n = jaw_curve(w, A.p0, A.y, A.axis, rdir, nIn, tmp);
        for (int i = 0; i < n; i++) in[i] = tmp[n-1-i];
        ni = n;
    }
    if (B.ok)
        no = jaw_curve(w, B.p0, B.y, B.axis, v3(-rdir.x,0.0f,-rdir.z), nOut, out);

    /* NO BLEND ARC ANY MORE. It existed because the old curve met the rail at
     * whatever angle its bulge left it, and that crease had to be rounded off
     * in the geometry. The tangent at P0 IS the rail now, by construction, so
     * the junction is already smooth and there is nothing to hide. */
    if (ni > 1) add_poly(w, in, ni, 1);
    if (sqrtf((q2.x-q1.x)*(q2.x-q1.x) + (q2.z-q1.z)*(q2.z-q1.z)) > 1e-4f &&
        ((q2.x-q1.x)*rdir.x + (q2.z-q1.z)*rdir.z) > 0.0f)
        add_seg(w, q1, q2, 0);
    if (no > 1) add_poly(w, out, no, 1);

    /* THE CAP GOES AT THE MOUTH, because that is where the chain now ends.
     *
     * A segment only pushes a ball out from its FRONT, and a polyline has open
     * ends — so without a circle at the last vertex a ball arriving round the
     * end of the rubber is in front of nothing and meets nothing. The circle is
     * omnidirectional and closes it, and it is the rounded nose the rubber
     * actually has. Recessed along the curve's own normal there, which is
     * across the pocket axis, so its surface sits on the curve instead of
     * bulging through it. */
    for (int e = 0; e < 2; e++) {
        const CueJawEnd *E = e ? &B : &A;
        if (!(e ? jawMid : jawIn)) continue;
        if (!E->ok) { add_jaw_recessed(w, e ? kMid : kIn, nin); continue; }
        /* BEHIND the nose, and OUT OF THE THROAT.
         *
         * The direction is across the pocket's own centre line, from the line
         * out to this jaw — which is the curve's outward normal at its end,
         * because the tangent there is the axis. Taken from the POCKET POINT
         * rather than from the rail's normal: at a middle the axis and the
         * rail's normal are the same vector, so a perpendicular-to-axis test
         * against the rail's normal reads zero and the recess went sideways
         * ALONG the rail instead of backwards. That narrowed a snooker middle
         * to 56 mm against a 52.5 mm ball. */
        const float off = w->jaw_r + 0.15f * w->R;
        add_jaw(w, v3(E->y.x + E->nrm.x*off, 0.0f, E->y.z + E->nrm.z*off));
    }
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
    /* OUT TO THE FRAME'S INNER EDGE, not to a multiple of the ball. This read
     * `mid ? ml : cl*0.7f` — 1.6R and 1.4R — while the timber starts at
     * rail_w*0.63, so the jaw curve ended short of the wood by whatever the
     * difference happened to be: 11.7mm at a UK corner, 6.6mm at a middle,
     * both confirmed by measuring the built mesh. The cushion has to arrive AT
     * the frame, so it is built out to it. */
    const float rise = (w->cush_depth > 1e-6f) ? w->cush_depth
                                               : (mid ? ml : cl*0.7f);
    *tip = v3(k.x - u.x*back + out.x*rise, 0, k.z - u.z*back + out.z*rise);
    *arc = mid ? 0.7f : 0.6f;
}

/* ---- P1: the cushions meet the frame at the bore -------------------------
 * See cue_table.h. Rectangles only: the jaw tip expressions below are the six
 * add_curved_chain calls in build_world, and an L or an n-gon builds its runs
 * through add_run instead. Those are left alone rather than approximated. */
static int link_edge_x(const CueWorld *w, int p, float br, float bset,
                       float line_z, int want_pos, float *out_x) {
    const float cx = w->pocket[p].x + w->pmnorm[p].x*bset;
    const float cz = w->pocket[p].z + w->pmnorm[p].z*bset;
    const float perp = line_z - cz;
    const float disc = br*br - perp*perp;
    if (disc < 0.0f) return 0;              /* the bore never reaches the edge */
    const float rt = sqrtf(disc);
    *out_x = want_pos ? (cx + rt) : (cx - rt);
    return 1;
}

/* THE SAME IDEA ON A BED THAT IS NOT A RECTANGLE.
 *
 * An L or an n-gon builds its runs through add_run, whose rails point in any
 * direction, so the rectangle's closed forms — written in x and z — do not
 * apply. The argument does, though, and in a form that needs no axes:
 *
 *   - the jaw tip now lies on the frame's inner edge by construction, because
 *     both jaw styles are built out by cush_depth;
 *   - `gap` slides the whole end treatment along its rail, one for one, so the
 *     tip moves along u and nothing else changes;
 *   - so asking for |tip + u*d - C| = bore is a quadratic in d with a closed
 *     root, exactly as it was on a rectangle.
 *
 * The tip is found structurally rather than by position: in add_run the facing
 * is emitted as (tip, knuckle) and the nose as (knuckle, ...), so the tip is
 * the kind-1 endpoint that no kind-0 segment shares. That holds for a mitre
 * and a bezier alike and does not care which way the rail runs. */
/* EVERY FREE JAW END, EACH ANSWERING TO THE POCKET IT IS NEAREST.
 *
 * This was written per pocket, gathering the tips within a fixed 350mm. That
 * radius is an assumption about how far apart pockets are, and it fails from
 * both ends: on a 16-gon one pocket found a single tip, and on an 18-gon each
 * found FOUR — its own two plus its neighbours' — so the mean was taken over
 * tips that wanted different answers and the link either skewed or gave up.
 *
 * A tip belongs to the pocket it is closest to. That is true at any size, on
 * any polygon, with pockets as close together as the shape allows, and it
 * needs no radius to be chosen. */
static void link_accumulate(const CueTable *t, const CueWorld *w,
                            float *acc, int *cnt) {
    /* THE MEDIAN, NOT THE MEAN.
     *
     * Every free jaw end on a regular bed wants the SAME correction, so any
     * spread is error rather than information — and a mean has no defence
     * against it. On a sixteen-gon one end came back with a delta of -334mm
     * against the others' +27, from a tip on the far side of the table that
     * the nearest-pocket rule had honestly assigned to this pocket. One such
     * value is enough: the sixteen- and twenty-gons came out 28mm and 21mm off
     * the point while every shape below them was inside half a millimetre.
     *
     * A median cannot be dragged by a few, and where the ends genuinely agree
     * it IS the mean. Nothing is thrown away silently — the spread is what the
     * bench's link-error readout shows. */
    enum { DMAX = 4096 };
    static float dv[2][DMAX]; static int dn[2];
    dn[0] = dn[1] = 0;
    for (int i = 0; i < w->nseg; i++) {
        if (w->seg[i].kind != 1) continue;
        const Vec3 e[2] = { w->seg[i].a, w->seg[i].b };
        for (int q = 0; q < 2; q++) {
            /* free: the end of the CHAIN, shared with no other segment */
            int shared = 0;
            for (int j = 0; j < w->nseg && !shared; j++) {
                if (j == i) continue;
                const Vec3 f[2] = { w->seg[j].a, w->seg[j].b };
                for (int r = 0; r < 2; r++) {
                    const float ax = f[r].x - e[q].x, az = f[r].z - e[q].z;
                    if (ax*ax + az*az < 1e-8f) { shared = 1; break; }
                }
            }
            if (shared) continue;
            /* the pocket it is nearest — no radius, no assumption */
            int p = -1; float pd = 1e30f;
            for (int k = 0; k < w->npocket; k++) {
                const float dx = w->pocket[k].x - e[q].x;
                const float dz = w->pocket[k].z - e[q].z;
                const float dd = dx*dx + dz*dz;
                if (dd < pd) { pd = dd; p = k; }
            }
            if (p < 0) continue;
            const int m = w->pocket_mid[p] ? 1 : 0;
            const float br   = m ? t->bore_side : t->bore_corner;
            const float bset = m ? t->bore_set_side : t->bore_set_corner;
            const float cx = w->pocket[p].x + w->pmnorm[p].x*bset;
            const float cz = w->pocket[p].z + w->pmnorm[p].z*bset;
            /* the rail it slides along, pointing AWAY from the pocket */
            float ux = 0, uz = 0, best = 1e30f;
            for (int j = 0; j < w->nseg; j++) {
                if (w->seg[j].kind != 0) continue;
                const float mx = 0.5f*(w->seg[j].a.x + w->seg[j].b.x) - e[q].x;
                const float mz = 0.5f*(w->seg[j].a.z + w->seg[j].b.z) - e[q].z;
                const float dd = mx*mx + mz*mz;
                if (dd >= best) continue;
                const float gx = w->seg[j].b.x - w->seg[j].a.x;
                const float gz = w->seg[j].b.z - w->seg[j].a.z;
                const float gl = sqrtf(gx*gx + gz*gz);
                if (gl < 1e-6f) continue;
                best = dd; ux = gx/gl; uz = gz/gl;
            }
            if (best > 1e29f) continue;
            if ((e[q].x - w->pocket[p].x)*ux + (e[q].z - w->pocket[p].z)*uz < 0.0f)
                { ux = -ux; uz = -uz; }
            const float qx = e[q].x - cx, qz = e[q].z - cz;
            const float qu = qx*ux + qz*uz;
            const float perp2 = (qx*qx + qz*qz) - qu*qu;
            const float disc = br*br - perp2;
            if (disc < 0.0f) continue;    /* the bore never reaches this edge */
            const float rt = sqrtf(disc);
            const float d1 = -qu + rt, d2 = -qu - rt;
            {   const float dd = (fabsf(d1) < fabsf(d2)) ? d1 : d2;
                if (dn[m] < DMAX) dv[m][dn[m]++] = dd;
            }
        }
    }
    for (int m = 0; m < 2; m++) {
        if (!dn[m]) continue;
        for (int a2 = 1; a2 < dn[m]; a2++) {      /* insertion sort, small */
            const float v2 = dv[m][a2]; int b2 = a2 - 1;
            while (b2 >= 0 && dv[m][b2] > v2) { dv[m][b2+1] = dv[m][b2]; b2--; }
            dv[m][b2+1] = v2;
        }
        acc[m] = dv[m][dn[m] / 2];
        cnt[m] = 1;                                /* already the answer */
    }
}

/* Returns 1 when both pocket kinds found their point, 0 when one fell back.
 *
 * IT HAS TO SAY. Below about 1.2 ball radii of pocket the bore stops reaching
 * the plank's inner edge, there is no crossing to put a cushion on, and the
 * gap was left at the seed — which on a UK 7ft reported a 192mm opening for a
 * 25mm pocket. Silently plausible and completely wrong, which is the worst
 * thing a measurement can be. */
int cue_table_link_gap(CueTable *t, const CueWorld *w) {
    /* AN L OR AN N-GON takes the general route: same argument, no axes. */
    if (t->notch_x > 0.0f || t->notch_z > 0.0f || t->bed_shape != CUE_BED_RECT) {
        float acc[2] = { 0.0f, 0.0f }; int cnt[2] = { 0, 0 };
        link_accumulate(t, w, acc, cnt);
        /* One gap per KIND, so ends that disagree get their mean — exact where
         * they are alike by symmetry, and the honest compromise where an L's
         * seven pockets are not. */
        int got = 0;
        if (!cnt[0]) got |= 1;   /* no pockets of that kind is not a failure */
        else { float g = t->gap_corner + acc[0]/(float)cnt[0];
               if (g > 0.005f && g < 0.30f) { t->gap_corner = g; got |= 1; } }
        if (!cnt[1]) got |= 2;
        else { float g = t->gap_side + acc[1]/(float)cnt[1];
               if (g > 0.005f && g < 0.30f) { t->gap_side = g; got |= 2; } }
        return got;
    }
    const float R = t->R;
    const float cl = 2.0f*R, ml = 1.6f*R, e3 = 0.25f*R;
    const float cw = t->rail_w * 0.63f;
    const float hw = t->half_wid, hl = t->half_len;
    const float line_z = hw + cw;           /* the +z rail's frame inner edge */
    int got = 3, seen_c = 0, seen_m = 0;

    /* THE CORNER. Its jaw comes along the +z rail from -x, so it answers to
     * the crossing on the far side of the bore centre from the table middle —
     * the smaller x of the two. */
    for (int p = 0; p < w->npocket; p++) {
        if (w->pocket_mid[p]) continue;
        if (w->pocket[p].x <= 0.0f || w->pocket[p].z <= 0.0f) continue;
        float yx;
        seen_c = 1; got &= ~1;
        if (!link_edge_x(w, p, t->bore_corner, t->bore_set_corner, line_z, 0, &yx))
            break;
        /* Rounded:  tip.x = hl - cgap + cl*0.7f + e3
         * Mitred:    tip.x = hl - cgap + cw/tan(ang_corner)
         * A mitre's run from the corner to the edge depends on its own facing
         * angle, so the two styles need their own arithmetic; sharing one was
         * the reason the mitred tables were left out. */
        const float sc2 = sinf(t->ang_corner*DEG);
        const float reach_c = t->pocket_round
            ? (cl*0.7f + e3)
            : ((sc2 > 1e-4f) ? (cw * cosf(t->ang_corner*DEG) / sc2) : 0.0f);
        float g = hl + reach_c - yx;
        if (g > 0.02f && g < 0.30f) { t->gap_corner = g; got |= 1; }
        break;
    }
    /* THE MIDDLE, on the same rail and the same edge line, approached from +x:
     * tip.x = (gap_side - 0.583R) - ml*0.3f - e3. */
    for (int p = 0; p < w->npocket; p++) {
        if (!w->pocket_mid[p]) continue;
        if (w->pocket[p].z <= 0.0f) continue;
        float yx;
        seen_m = 1; got &= ~2;
        if (!link_edge_x(w, p, t->bore_side, t->bore_set_side, line_z, 1, &yx))
            break;
        /* Rounded:  tip.x = gap_side - 0.583R - ml*0.3f - e3
         * Mitred:    tip.x = gap_side - cw/tan(ang_side) */
        const float ss2 = sinf(t->ang_side*DEG);
        const float reach_m = t->pocket_round
            ? (0.583f*R + ml*0.3f + e3)
            : ((ss2 > 1e-4f) ? (cw * cosf(t->ang_side*DEG) / ss2) : 0.0f);
        float g = yx + reach_m;
        if (g > 0.02f && g < 0.30f) { t->gap_side = g; got |= 2; }
        break;
    }
    (void)hl; (void)seen_c; (void)seen_m;
    return got;
}

/* See cue_table.h. The bore is the drop: same radius, same centre.
 *
 * They were two authored numbers and they disagreed on every table — the
 * American middles by 3.4mm of radius AND 8.6mm of centre — so the hole in the
 * timber did not sit on the circle a ball is caught by, and on the mitred
 * American middle you could see it. The drop is the gameplay; the timber's
 * hole is the same hole.
 *
 * Re-runnable, and that matters. The link places the cushions against the
 * bore, so a pocket size edited without this is a table whose cushions still
 * stand where the OLD size put them — and a workshop asking for the opening
 * would be told the opening of the table it started with. */
void cue_table_normalise(CueTable *t) {
    if (!t) return;
    t->bore_corner     = t->pr_corner - t->cap_corner;
    t->bore_side       = t->pr_side   - t->cap_side;
    t->bore_set_corner = t->drop_back;
    t->bore_set_side   = t->drop_back_side;
}

static void smooth_seg_normals(CueWorld *w) {
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
    /* the same number cue_render uses for the timber's inner edge (rw * 0.63) */
    w->cush_depth = t->rail_w * 0.63f;
    w->jaw_segs   = CUE_JAW_SEGS;   /* build_ngon trims it for a many-sided bed */
    /* Zero is a MEANINGFUL angle — the facing running straight down the
     * A table written before these existed unpacks them as zero, and a zero
     * P0 puts the curve's start on top of its end. So they fall back, and only
     * the shape is affected — the yellow point is derived either way. */
    w->jaw_p0  = (t->jaw_p0 > 0.0f) ? t->jaw_p0 : 0.070f;
    /* ZERO FOLLOWS THE CORNER. A table saved before this field existed reads
     * back as zero, and that has to mean "as it was", not "no curve at all". */
    w->jaw_p0_m = (t->jaw_p0_m > 0.0f) ? t->jaw_p0_m : w->jaw_p0;
    w->jaw_h1  = (t->jaw_h1 > 0.0f) ? t->jaw_h1 : 0.030f;
    w->jaw_h2  = (t->jaw_h2 > 0.0f) ? t->jaw_h2 : 0.030f;
    /* Both are offsets from the pocket's own centre line now, and zero is a
     * meaningful one — straight down it — so neither gets a fallback. */
    w->jaw_ang_c = t->jaw_ang_c;
    w->jaw_ang_m = t->jaw_ang_m;
    w->jaw_r = t->jaw_r;
    w->e_cush     = t->e_cush;
    w->cush_efall = t->cush_efall;
    w->e_cush_min = t->e_cush_min;
    /* Zero is "the engine's own", not "a frictionless cloth" — see CueTable. */
    if (t->mu_r > 0.0f) w->mu_r = t->mu_r;
    w->drop_back = t->drop_back;
    w->drop_back_side = t->drop_back_side;

    const float hl = t->half_len, hw = t->half_wid, R = t->R;

    /* THE POCKETS FIRST, because the jaw curve is built against them.
     *
     * These used to be added after the cushion chains, which was fine while a
     * jaw was a shape in its own right. It is not any more: a rounded jaw now
     * FINISHES at the yellow point — where the bore circle crosses the frame's
     * inner face — and the bore circle is this pocket. So the pocket has to
     * exist before the rail that runs into it is built.
     *
     * Only the position and the capture radius are set here; pmnorm and drop_c
     * still come later, off the finished jaws, and the curve does not need them
     * — it works out the pocket's axis from the rail it is on. */
    const float d = 0.70710678f, oc = t->off_corner, os = t->off_side;
    /* Drop-capture radius (independent of the visible mouth/pr_side), and PER
     * TABLE — see cap_corner / cap_side. It used to be one literal here, 0.3 R
     * with 0.15 R for a UK middle, which is why no table could be given a drop
     * of its own without moving every other table's with it. */
    float capc = t->pr_corner - t->cap_corner, caps = t->pr_side - t->cap_side;
    if (t->bed_shape == CUE_BED_RECT && t->kind != CUE_GAME_BARBILLIARDS &&
        !CUE_GAME_IS_CAROM(t->kind)) {
        /* The axis a corner was offset along IS its centre line: two rails
         * meeting square bisect at 45 degrees, which is what d is. */
        add_pocket(w, -hl - oc*d, -hw - oc*d, capc, 0, -d, -d);
        add_pocket(w,  hl + oc*d, -hw - oc*d, capc, 0,  d, -d);
        add_pocket(w,  hl + oc*d,  hw + oc*d, capc, 0,  d,  d);
        add_pocket(w, -hl - oc*d,  hw + oc*d, capc, 0, -d,  d);
        add_pocket(w, 0.0f, -hw - os, caps, 1, 0.0f, -1.0f);
        add_pocket(w, 0.0f,  hw + os, caps, 1, 0.0f,  1.0f);
    }

    /* CAROM IS PART OF THE CHAIN BELOW, not a block before it. Written as
     * its own if, the four plain cushions went in and then control FELL INTO
     * the pocketed-rectangle else-chain, which happily built a second, six-
     * pocket cushion set on top of them — overlapping rails, and the wrap
     * corner no longer shared, which is the square hole reported in one
     * corner. One chain, one set of cushions. */
    if (CUE_GAME_IS_CAROM(t->kind)) {
        add_seg(w, v3(-hl, 0, -hw), v3( hl, 0, -hw), 0);
        add_seg(w, v3( hl, 0, -hw), v3( hl, 0,  hw), 0);
        add_seg(w, v3( hl, 0,  hw), v3(-hl, 0,  hw), 0);
        add_seg(w, v3(-hl, 0,  hw), v3(-hl, 0, -hw), 0);
    } else if (t->kind == CUE_GAME_BARBILLIARDS) {
        /* ---- BAR BILLIARDS: four plain cushions and nine holes in the bed --
         *
         * There are no pockets on the rails, so the rails are four unbroken
         * runs and there is nothing to shape. Everything that scores is bored
         * through the cloth, and the arrangement of those holes is a constant
         * of the game — a player no more dials it than he dials where the
         * black goes on a snooker table — so it is written out here rather
         * than carried as twenty fields nobody would ever turn.
         *
         * FIVE IN A ROW AT THE FAR END AND A DIAMOND OF FOUR AT THE PLAYER'S,
         * which is the AEBBA board and was not what this was.
         *
         * From the D, going away: the 200 at the front of the diamond with the
         * black skittle standing in front of it, the two 50s out at its sides,
         * the 100 at its back between the two whites, and then the row of five
         * across in front of the top cushion reading 30, 20, 10, 20, 30.
         *
         * WHY THE 200 IS THE NEAREST HOLE, which is the part that makes the
         * board make sense: every stroke is played from the D at this end, so
         * you are always shooting AWAY from the near holes. Reaching the 200
         * means bringing a ball back down the table off a cushion or off another
         * ball, past a skittle you must not topple. That is what earns two
         * hundred. The far row is the easy scoring and is worth 10 to 30.
         *
         * IT WAS BUILT THE OTHER WAY ROUND AND INTERLEAVED: the 100 nearest, the
         * 50s next, then the row of five, then the 200 furthest of all. So there
         * was no diamond at all, the black guarded the hole at the BACK, and the
         * 100 sat unguarded in front of the player — a hundred a visit for
         * nothing, which is what gave it away.
         *
         * The x positions are chosen; the rules give the values, the order and
         * the skittles, not the coordinates. These keep the old footprint — the
         * scoring area in the far two thirds with a long run-up from the D — and
         * put the four of the diamond and the five of the row where the board
         * has them. */
        add_seg(w, v3(-hl, 0, -hw), v3( hl, 0, -hw), 0);
        add_seg(w, v3( hl, 0, -hw), v3( hl, 0,  hw), 0);
        add_seg(w, v3( hl, 0,  hw), v3(-hl, 0,  hw), 0);
        add_seg(w, v3(-hl, 0,  hw), v3(-hl, 0, -hw), 0);

        /* A hole is barely wider than a ball, which is what makes a 200 worth
         * two hundred. The capture radius is the ball's centre reaching it. */
        const float hr = 1.26f * R;
        static const struct { float x, z; int v; } HOLE[] = {
            /* the diamond, nearest the player first */
            { 0.050f,  0.000f, 200 },
            { 0.235f, -0.235f,  50 }, { 0.235f,  0.235f,  50 },
            { 0.420f,  0.000f, 100 },
            /* and the row of five across the far end */
            { 0.615f, -0.290f,  30 }, { 0.615f,  0.290f,  30 },
            { 0.615f, -0.145f,  20 }, { 0.615f,  0.145f,  20 },
            { 0.615f,  0.000f,  10 },
        };
        for (int i = 0; i < (int)(sizeof HOLE / sizeof HOLE[0]); i++) {
            /* IN THE BED, not on a rail: no cushion runs into it, so the axis
             * is only there to be well formed. Radial from the middle. */
            add_pocket(w, HOLE[i].x, HOLE[i].z, hr, 0,
                       HOLE[i].x, HOLE[i].z);
            w->pocket_score[w->npocket - 1] = (int16_t)HOLE[i].v;
            /* Cloth all the way round it: a circle, not a cut in the edge. */
            w->pocket_bed[w->npocket - 1] = 1;
        }

        /* Rule 74. The skittles are 15 to 18 mm across; the black stands 6 mm
         * clear of the front edge of the 200 hole, the two whites level with the
         * 100 and 178 mm either side of it.
         *
         * READ OFF THE BOARD, not written out again. Both skittle positions were
         * literal coordinates copied from the hole table, so when the holes were
         * re-laid the skittles stayed where they were: the black went on guarding
         * whatever now sat at the far end and the 200 was left open. A skittle is
         * defined by the hole it stands at, so it is found by that. */
        w->skittle_r = 0.0085f;
        w->nskittle = 0;
        Vec3 h200 = v3(0,0,0), h100 = v3(0,0,0);
        int got200 = 0, got100 = 0;
        for (int i = 0; i < w->npocket; i++) {
            if (w->pocket_score[i] == 200) { h200 = w->pocket[i]; got200 = 1; }
            if (w->pocket_score[i] == 100) { h100 = w->pocket[i]; got100 = 1; }
        }
        #define SKITTLE(x_, z_, black_) do { \
            int i_ = w->nskittle++; \
            w->skittle[i_] = v3((x_), 0.0f, (z_)); \
            w->skittle_spot[i_] = w->skittle[i_];   /* where it is replaced */ \
            w->skittle_black[i_] = (black_); } while (0)
        if (got100) {
            SKITTLE(h100.x, h100.z - 0.178f, 0);
            SKITTLE(h100.x, h100.z + 0.178f, 0);
        }
        /* IN FRONT OF THE 200, and "in front" is the side the player is on —
         * which is baulk, at -x. The black is what makes the nearest hole the
         * hardest one on the table. */
        if (got200) SKITTLE(h200.x - hr - 0.006f - w->skittle_r, h200.z, 1);
        #undef SKITTLE
        /* Rule 74's pin: 114 mm tall and about twelve grams of light wood.
         * Stood up as rigid bodies in a world whose floor is the bed and whose
         * walls are the cushions. */
        w->skittle_len  = 0.114f;
        /* THE MASS IS THE TIMBER, not a guess. Rule 74 gives the size and says
         * nothing about the weight, so it comes from the shape: the turned
         * profile sweeps 66.7 cm3, and a skittle is turned hardwood — beech at
         * about 720 kg/m3. That is 48 g. The 12 g it had before is balsa, and
         * it showed: a pin that light barely troubled a ball. */
        w->skittle_mass = 0.048f;
        cue_phys_skittles_init(w, hl, hw);
    } else if (t->bed_shape == CUE_BED_NGON) {
        /* A regular bed brings its own chain, its own pockets and its own
         * outline, so it skips the rectangle's rail construction entirely. */
        build_ngon(w, t);
    } else if (t->bed_shape == CUE_BED_L) {
        /* The L brings its own chain AND its own pockets, so it skips both the
         * rectangle's rail construction and the six-pocket block below. */
        build_L(w, t);
    } else if (!t->pocket_round) {
        /* US pool: straight mitred facings. */
        const float g = t->gap_corner, sg = t->gap_side, sl = t->facing_len;
        const float cc = cosf(t->ang_corner*DEG), sc = sinf(t->ang_corner*DEG);
        const float cs = cosf(t->ang_side*DEG),   ss = sinf(t->ang_side*DEG);
        /* OUT TO THE FRAME, the mitre's way. A rounded jaw reaches out by a
         * multiple of the ball; a mitre reaches out by facing_len * sin(ang),
         * so the length needed to arrive at the timber is cw / sin(ang) and
         * the corner-to-edge run along the rail is cw / tan(ang) — a different
         * distance for every facing angle, which is why one constant could
         * never have served both jaw styles. Falls back to the authored facing
         * where no depth is known. */
        const float slc = (w->cush_depth > 1e-6f && sc > 1e-4f)
                        ? (w->cush_depth / sc) : sl;
        const float sls = (w->cush_depth > 1e-6f && ss > 1e-4f)
                        ? (w->cush_depth / ss) : sl;
        add_chain(w, v3(-hl+g - cc*slc, 0, -hw - sc*slc), v3(-hl+g, 0, -hw),
                     v3(-sg, 0, -hw),                   v3(-sg + cs*sls, 0, -hw - ss*sls));
        add_chain(w, v3(sg - cs*sls, 0, -hw - ss*sls),    v3(sg, 0, -hw),
                     v3(hl-g, 0, -hw),                  v3(hl-g + cc*slc, 0, -hw - sc*slc));
        add_chain(w, v3(hl + sc*slc, 0, -hw+g - cc*slc),  v3(hl, 0, -hw+g),
                     v3(hl, 0, hw-g),                   v3(hl + sc*slc, 0, hw-g + cc*slc));
        add_chain(w, v3(hl-g + cc*slc, 0, hw + sc*slc),   v3(hl-g, 0, hw),
                     v3(sg, 0, hw),                     v3(sg - cs*sls, 0, hw + ss*sls));
        add_chain(w, v3(-sg + cs*sls, 0, hw + ss*sls),    v3(-sg, 0, hw),
                     v3(-hl+g, 0, hw),                  v3(-hl+g - cc*slc, 0, hw + sc*slc));
        add_chain(w, v3(-hl - sc*slc, 0, hw-g + cc*slc),  v3(-hl, 0, hw-g),
                     v3(-hl, 0, -hw+g),                 v3(-hl - sc*slc, 0, -hw+g - cc*slc));
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
        /* HOW FAR THE JAW REACHES OUT — to the frame's inner edge, not to a
         * multiple of the ball.
         *
         * These were cl*0.7f at a corner and ml at a middle: 1.4R and 1.6R,
         * struck off the BALL. cue_render puts the timber's inner edge at the
         * cushion back, rail_w * 0.63, struck off the RAIL. Two unrelated
         * numbers, so the jaw curve ended short of the wood by whatever the
         * difference happened to be — 11.7mm at a UK corner, 6.6mm at a middle
         * — and that shortfall is the gap beside every pocket. Measured on the
         * built mesh at 10.5 and 6.6mm, which is how it was found.
         *
         * A cushion has to arrive AT the frame it sits against, so it is built
         * out to it. Falls back to the old reach if the depth is unset, so a
         * world built without one is unchanged. */
        const float co = (w->cush_depth > 1e-6f) ? w->cush_depth : cl*0.7f;
        const float mo = (w->cush_depth > 1e-6f) ? w->cush_depth : ml;
        /* C1 top-left */
        add_curved_chain(w, v3(-hl+cgap - cl*0.7f - e3,0,-hw - co), v3(-hl+cgap,0,-hw),
                            v3(-mgap,0,-hw), v3(-bg + ml*0.3f + e3,0,-hw - mo), ca, ma, nc, nm);
        /* C2 top-right */
        add_curved_chain(w, v3(bg - ml*0.3f - e3,0,-hw - mo), v3(mgap,0,-hw),
                            v3(hl-cgap,0,-hw), v3(hl-cgap + cl*0.7f + e3,0,-hw - co), ma, ca, nm, nc);
        /* C3 right */
        add_curved_chain(w, v3(hl + co,0,-hw+cgap - cl*0.7f - e3), v3(hl,0,-hw+cgap),
                            v3(hl,0,hw-cgap), v3(hl + co,0,hw-cgap + cl*0.7f + e3), ca, ca, nc, nc);
        /* C4 bottom-right */
        add_curved_chain(w, v3(hl-cgap + cl*0.7f + e3,0,hw + co), v3(hl-cgap,0,hw),
                            v3(mgap,0,hw), v3(bg - ml*0.3f - e3,0,hw + mo), ca, ma, nc, nm);
        /* C5 bottom-left */
        add_curved_chain(w, v3(-bg + ml*0.3f + e3,0,hw + mo), v3(-mgap,0,hw),
                            v3(-hl+cgap,0,hw), v3(-hl+cgap - cl*0.7f - e3,0,hw + co), ma, ca, nm, nc);
        /* C6 left */
        add_curved_chain(w, v3(-hl - co,0,hw-cgap + cl*0.7f + e3), v3(-hl,0,hw-cgap),
                            v3(-hl,0,-hw+cgap), v3(-hl - co,0,-hw+cgap - cl*0.7f - e3), ca, ca, nc, nc);
    }

    smooth_seg_normals(w);


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

    /* ---- NOTHING OF A CUSHION INSIDE A POCKET --------------------------- *
     *
     * The nose is built to finish on the yellow point, and on every rectangle,
     * L and mitred table it does — measured, zero vertices inside a bore. A
     * POLYGON bed is the case that does not: its rails meet at angles the
     * corner construction was not written for, and an octagon put sixteen nose
     * vertices up to 3.3 mm inside the hole. That is cushion a ball can hit
     * after it has passed the mouth, which is a rattle out of a pocket the ball
     * had already gone into.
     *
     * So every vertex is pushed out to the bore's edge if it is inside it. On
     * the tables that were already clear this changes nothing at all; where it
     * bites, it stops the cushion at the hole instead of carrying on across it.
     * The normals are re-smoothed afterwards because moving an endpoint changes
     * the faces either side of it — and shared endpoints move identically, so
     * the chain stays welded. */
    {   int moved = 0;
        for (int s = 0; s < w->nseg; s++) {
            moved |= cue_table_clear_bore(w, &w->seg[s].a.x, &w->seg[s].a.z);
            moved |= cue_table_clear_bore(w, &w->seg[s].b.x, &w->seg[s].b.z);
        }
        if (moved) smooth_seg_normals(w);
    }

    w->cut_ref[0] = t->pr_corner; w->cut_ref[1] = t->pr_side;
    for (int m = 0; m < 2; m++) {
        CueCut c; cue_table_default_cut(t->kind, m, &c);
        w->cut_set[m] = c.set; w->cut_rad[m] = c.rad;
        w->cut_roll[m] = c.roll; w->cut_arc[m] = c.arc;
    }
    cue_table_derive_cut(w);

    /* THE CUSHIONS FOLLOW THE HOLE, on the shapes that need it.
     *
     * cue_table_link_gap was reachable only from the bench, so the game built
     * every table with the authored gap and none of this applied in the
     * headset. It applies here now — but only where the bed is NOT a plain
     * rectangle.
     *
     * The rectangles are left alone deliberately: their gaps were dialled in
     * the headset with a ball rolling at the pocket, and linking moves their
     * mouths a long way (snooker's corner 103.0 -> 87.6, the American 9ft
     * 107.2 -> 114.8). That is a decision about how those tables PLAY and it
     * is the author's, not a side effect of a geometry fix. A polygon or an L
     * has no such tuning to lose, and it is exactly where the cushions were
     * standing still while the hole moved.
     *
     * Guarded against re-entry because the link needs a built world to read
     * the jaw tips from and then wants the world built again — twice more,
     * because the pocket's normal is worked out from those same tips, so
     * moving them moves the target a little. */
    {   static int linking = 0;
        /* ON FOR EVERY SHAPE NOW, rectangles included, at the author's word.
         * It was held back from them because their gaps were dialled by hand
         * in the headset and linking moves their mouths; that is a judgement
         * about how they PLAY, and it is being made deliberately rather than
         * arrived at by accident. */
        if (!linking) {
            linking = 1;
            CueTable tt = *t;
            int got = 3;
            for (int pass = 0; pass < 3; pass++) {
                got = cue_table_link_gap(&tt, w);
                cue_table_build_world(&tt, w);
            }
            linking = 0;
            w->linked = got;      /* after the rebuilds, which clear it */
        }
    }
}


/* ---- what the pockets actually came out at ------------------------------- */

/* The narrowest passage at one pocket: the two jaw circles nearest it, less
 * their radii. That is the rule a ball has to pass, and it is the same measure
 * the bench reports as "mouth". */
/* Is this point nearer pocket p than any other? */
static int owns_pocket(const CueWorld *w, int p, float x, float z) {
    float bd = 1e30f; int bp = -1;
    for (int q = 0; q < w->npocket; q++) {
        const float dx = x - w->pocket[q].x, dz = z - w->pocket[q].z;
        const float d = dx*dx + dz*dz;
        if (d < bd) { bd = d; bp = q; }
    }
    return bp == p;
}

/* Distance between two line segments in the plane. */
static float seg_seg_dist(float ax, float az, float bx, float bz,
                          float cx, float cz, float dx_, float dz_) {
    /* Four point-to-segment distances is exact for non-crossing segments, and
     * the two sides of a pocket mouth never cross. */
    float best = 1e30f;
    const float ex1 = bx-ax, ez1 = bz-az, L1 = ex1*ex1 + ez1*ez1;
    const float ex2 = dx_-cx, ez2 = dz_-cz, L2 = ex2*ex2 + ez2*ez2;
    const float px[4] = { cx, dx_, ax, bx }, pz[4] = { cz, dz_, az, bz };
    for (int i = 0; i < 4; i++) {
        float sx, sz, ex, ez, L;
        if (i < 2) { sx = ax; sz = az; ex = ex1; ez = ez1; L = L1; }
        else       { sx = cx; sz = cz; ex = ex2; ez = ez2; L = L2; }
        float tt = (L > 1e-12f) ? ((px[i]-sx)*ex + (pz[i]-sz)*ez) / L : 0.0f;
        if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
        const float qx = px[i] - (sx + ex*tt), qz = pz[i] - (sz + ez*tt);
        const float d = sqrtf(qx*qx + qz*qz);
        if (d < best) best = d;
    }
    return best;
}

/* Distance from a point to a segment. */
static float pt_seg_dist(float px, float pz,
                         float ax, float az, float bx, float bz) {
    const float ex = bx-ax, ez = bz-az, L = ex*ex + ez*ez;
    float tt = (L > 1e-12f) ? ((px-ax)*ex + (pz-az)*ez) / L : 0.0f;
    if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
    const float qx = px - (ax + ex*tt), qz = pz - (az + ez*tt);
    return sqrtf(qx*qx + qz*qz);
}

/* THE NARROWEST PASSAGE INTO ONE POCKET.
 *
 * This used to be the two nearest jaw CIRCLES less their radii, which is right
 * on a rounded English pocket — the knuckles are the tightest point — and
 * wrong on a mitred one, where the tightest point is the two facing TIPS and
 * the circles sit behind them. On pyramid it read 76.4 mm where a ball
 * actually had 71.6, and 5 mm is that game's entire clearance.
 *
 * So it is measured rather than assumed: everything belonging to this pocket
 * is split into the two sides of its mouth, and the answer is the closest
 * approach between anything on one side and anything on the other. Segments
 * against segments, circles against both. That is the same quantity test_gap
 * finds by flooding a grid, at a fraction of the cost, and it does not care
 * which style of jaw built it. */
int cue_table_clear_bore_m(const CueWorld *w, float *x, float *z, float margin) {
    if (!w || !x || !z) return 0;
    int moved = 0;
    /* EVERY pocket, not the nearest: a middle and a corner on a short rail can
     * both reach the same stretch of cushion, and clearing one can push a point
     * into the other. Looped until it is outside all of them, with a low cap
     * because two circles cannot argue for long. */
    for (int pass = 0; pass < 4; pass++) {
        int hit = 0;
        for (int p = 0; p < w->npocket; p++) {
            const float r = w->pocket_r[p] + margin;
            if (r <= 0.0f) continue;
            const float dx = *x - w->drop_c[p].x, dz = *z - w->drop_c[p].z;
            const float d = sqrtf(dx*dx + dz*dz);
            if (d >= r) continue;
            if (d < 1e-6f) {                 /* dead centre: no direction to use */
                *x = w->drop_c[p].x + r; hit = 1; moved = 1; continue;
            }
            *x = w->drop_c[p].x + dx / d * r;
            *z = w->drop_c[p].z + dz / d * r;
            hit = 1; moved = 1;
        }
        if (!hit) break;
    }
    return moved;
}

int cue_table_clear_bore(const CueWorld *w, float *x, float *z) {
    return cue_table_clear_bore_m(w, x, z, 0.0f);
}

int cue_table_hide_bore(const CueWorld *w, float *x, float *z,
                        float ux, float uz, float margin, float reach) {
    if (!w || !x || !z) return 0;
    const float ul = sqrtf(ux*ux + uz*uz);
    if (ul < 1e-6f) return 0;
    ux /= ul; uz /= ul;
    int moved = 0;
    for (int p = 0; p < w->npocket; p++) {
        const float r = w->pocket_r[p] + margin;
        if (r <= 0.0f) continue;
        const float ax = *x - w->drop_c[p].x, az = *z - w->drop_c[p].z;
        const float d2 = ax*ax + az*az;
        if (d2 >= r*r) continue;                  /* already clear of this one */
        /* |P + s*u - C| = r, taking the forward root. u is a unit vector, so
         * s = -(a.u) + sqrt((a.u)^2 - |a|^2 + r^2). */
        const float b = ax*ux + az*uz;
        const float disc = b*b - d2 + r*r;
        if (disc < 0.0f) continue;                /* cannot get out this way */
        float sfar = -b + sqrtf(disc);
        if (sfar <= 0.0f) continue;
        if (sfar > reach) sfar = reach;           /* never fold further than told */
        *x += ux * sfar; *z += uz * sfar;
        moved = 1;
    }
    return moved;
}

float cue_table_ray_bore_limit(const CueWorld *w, float ox, float oz,
                               float dx, float dz, float smax) {
    if (!w) return smax;
    const float dl = sqrtf(dx*dx + dz*dz);
    if (dl < 1e-9f) return smax;
    dx /= dl; dz /= dl;
    float lim = smax;
    for (int p = 0; p < w->npocket; p++) {
        const float r = w->pocket_r[p];
        if (r <= 0.0f) continue;
        const float ax = ox - w->drop_c[p].x, az = oz - w->drop_c[p].z;
        const float b = ax*dx + az*dz;
        const float c = ax*ax + az*az - r*r;
        const float disc = b*b - c;
        if (disc < 0.0f) continue;              /* the ray misses this bore */
        const float rt = sqrtf(disc);
        const float s0 = -b - rt;               /* where it goes IN */
        /* STARTING ON THE EDGE is the case that matters, and c is then zero, so
         * s0 is either 0 or -2b. Heading inward means b < 0, and the ray enters
         * at once: the limit is nothing at all, which is the whole point — the
         * cushion stops at the yellow point instead of crossing the hole. */
        if (s0 > 0.0f) { if (s0 < lim) lim = s0; }
        else if (b < 0.0f && c <= 1e-9f) lim = 0.0f;
    }
    return lim < 0.0f ? 0.0f : lim;
}

float cue_table_mouth_at(const CueWorld *w, int p) {
    const float ox = w->pocket[p].x, oz = w->pocket[p].z;
    /* The mouth's axis: from the pocket out towards the bed. drop_c is set
     * back INTO the pocket, so the outward normal points from it to the
     * pocket point. Sides are then simply left and right of that axis. */
    float nx = ox - w->drop_c[p].x, nz = oz - w->drop_c[p].z;
    const float nl = sqrtf(nx*nx + nz*nz);
    if (nl < 1e-6f) return 0.0f;
    nx /= nl; nz /= nl;
    /* Perpendicular, which is what "which side" is measured along. */
    const float tx = -nz, tz = nx;
    /* NEAR THIS POCKET AND NO OTHER. A middle and a corner on the same rail
     * are close enough that a fixed radius picks up both, so everything is
     * assigned to its nearest pocket — the same rule the link uses. */
    #define OWNS(qx, qz) owns_pocket(w, p, (qx), (qz))
    float best = 1e30f;
    /* segment against segment */
    for (int i = 0; i < w->nseg; i++) {
        const CueSeg *a = &w->seg[i];
        const float amx = (a->a.x + a->b.x)*0.5f, amz = (a->a.z + a->b.z)*0.5f;
        if (!OWNS(amx, amz)) continue;
        const float sa = (amx-ox)*tx + (amz-oz)*tz;
        for (int j = i+1; j < w->nseg; j++) {
            const CueSeg *b = &w->seg[j];
            const float bmx = (b->a.x + b->b.x)*0.5f, bmz = (b->a.z + b->b.z)*0.5f;
            if (!OWNS(bmx, bmz)) continue;
            const float sb = (bmx-ox)*tx + (bmz-oz)*tz;
            if (sa * sb >= 0.0f) continue;          /* same side of the mouth */
            const float d = seg_seg_dist(a->a.x, a->a.z, a->b.x, a->b.z,
                                         b->a.x, b->a.z, b->b.x, b->b.z);
            if (d < best) best = d;
        }
    }
    /* circle against circle, and circle against segment */
    for (int i = 0; i < w->njaw; i++) {
        if (!OWNS(w->jaw[i].x, w->jaw[i].z)) continue;
        const float sa = (w->jaw[i].x-ox)*tx + (w->jaw[i].z-oz)*tz;
        for (int j = i+1; j < w->njaw; j++) {
            if (!OWNS(w->jaw[j].x, w->jaw[j].z)) continue;
            const float sb = (w->jaw[j].x-ox)*tx + (w->jaw[j].z-oz)*tz;
            if (sa * sb >= 0.0f) continue;
            const float dx = w->jaw[i].x - w->jaw[j].x;
            const float dz = w->jaw[i].z - w->jaw[j].z;
            const float d = sqrtf(dx*dx + dz*dz) - 2.0f * w->jaw_r;
            if (d < best) best = d;
        }
        for (int j = 0; j < w->nseg; j++) {
            const CueSeg *b = &w->seg[j];
            const float bmx = (b->a.x + b->b.x)*0.5f, bmz = (b->a.z + b->b.z)*0.5f;
            if (!OWNS(bmx, bmz)) continue;
            const float sb = (bmx-ox)*tx + (bmz-oz)*tz;
            if (sa * sb >= 0.0f) continue;
            const float d = pt_seg_dist(w->jaw[i].x, w->jaw[i].z,
                                        b->a.x, b->a.z, b->b.x, b->b.z) - w->jaw_r;
            if (d < best) best = d;
        }
    }
    #undef OWNS
    if (best > 1e29f) return 0.0f;
    return best > 0.0f ? best : 0.0f;
}

void cue_table_openings(const CueTable *t, float *corner, float *middle) {
    if (corner) *corner = 0.0f;
    if (middle) *middle = 0.0f;
    if (!t) return;
    /* STATIC, not stack: a CueWorld is a big structure and this is called from
     * a menu, on a device whose stack is not. */
    static CueWorld w;
    /* A COPY, normalised: the caller may have edited a pocket field without
     * putting the bore back in step, and reporting the old table's opening is
     * the one answer this must never give. */
    CueTable tt = *t;
    cue_table_normalise(&tt);
    cue_table_build_world(&tt, &w);
    for (int p = 0; p < w.npocket; p++) {
        const float m = cue_table_mouth_at(&w, p);
        if (m <= 0.0f) continue;
        /* -1, NOT the number: an unlinked kind's cushions are still standing
         * at the seed, so the gap between them is not this pocket's mouth and
         * saying so would be worse than saying nothing. */
        if (w.pocket_mid[p]) { if (middle && *middle == 0.0f)
                                  *middle = (w.linked & 2) ? m : -1.0f; }
        else                 { if (corner && *corner == 0.0f)
                                  *corner = (w.linked & 1) ? m : -1.0f; }
    }
}

/* ---- THE SPEC A STANDARD TABLE IS CUT TO ---------------------------------
 * See cue_table.h for what a spec is and why tournament is a no-op.
 *
 * WHERE THE NUMBERS COME FROM. Each row is an opening across the pocket in
 * millimetres, and the tournament row is not authored at all — it is what the
 * shipped table measures, read off cue_table_openings, so it can be checked
 * against the game rather than trusted. The two others are placed relative to
 * it by the amount a real fitter's templates differ:
 *
 *   SNOOKER. Tournament templates are cut around 3 3/8 in, and the shipped
 *   table measures 84.0 mm at the corner and 90.8 at the middle. A practice
 *   table cut for a professional goes tighter still, and 79 mm — a tenth of a
 *   ball width off — is about where that lands; the club table is the 3 3/4 in
 *   pocket almost everybody actually learned on, which is 95 mm.
 *
 *   AMERICAN POOL. The shipped 9 ft measures 111 mm, between the 4 1/2 in
 *   (114 mm) that home and bar tables come with and the 4 in (102 mm) pro cut
 *   that tournaments are played on. So pro takes the 4 in and club takes a
 *   generous bar-box 120 mm.
 *
 *   ENGLISH POOL AND CHINESE 8-BALL are already cut tight — 81 mm to a 50.8 mm
 *   ball, and 86 mm to a 57 mm one — so their spread is narrower in absolute
 *   terms and about the same in ball widths.
 *
 * AND THE CLOTH, which is the half of this that is not the pocket. Rolling
 * resistance: 0.010 is the engine's own and is what every table has played on,
 * so tournament leaves it alone. New worsted on a match table runs noticeably
 * further, and a napped club cloth with some years in it noticeably less; a
 * factor of about 0.85 and 1.35 either side is the spread, which is the
 * difference between a safety that reaches baulk and one that stops short.
 *
 * The rubber goes with it. A club table's cushions are the same profile and
 * ten years older, so a couple of points off the crawl restitution and a
 * steeper fall with pace — a deader rail, which is what makes a slow table feel
 * slow off the cushion as well as along the cloth.
 *
 * A zero opening in a row means "leave this pocket type alone", which is what
 * bar billiards and the golf table want and what a middle-less bed needs. */
const char *const CUE_SPEC_NAME[CUE_SPEC_COUNT] = { "PRO", "TOURNAMENT", "CLUB" };

/* Which family of standard table a game belongs to, for the spec table below.
 * A game with no entry has no specs. */
enum { SPEC_FAM_NONE = 0, SPEC_FAM_SNOOKER, SPEC_FAM_ENGLISH,
       SPEC_FAM_AMERICAN, SPEC_FAM_CHINESE, SPEC_FAM_COUNT };

static int spec_family(CueGameKind kind) {
    switch (kind) {
    case CUE_GAME_SNK15: case CUE_GAME_SNK10:
    case CUE_GAME_BILLIARDS:                       return SPEC_FAM_SNOOKER;
    /* 6-RED IS ON THE ENGLISH 7 FT BED, pockets and all — the same bed UK
     * 8-ball plays on, which is why cue_table_rails already groups it there.
     * Filed under snooker it came out with a PRO spec whose MIDDLES were wider
     * than the shipped ones, because the snooker row is written against a 12 ft
     * table's 90.8 mm middle and this bed's is 81.3. A spec that loosens a
     * pocket on the way to "pro" is the family being wrong. */
    /* AND GOLF, which plays on the English 7 ft bed and whose whole score is
     * how many strokes a hole took — so a pocket cut tighter or wider changes
     * the game as directly as it changes any other. It was excluded along with
     * bar billiards, on the grounds that its course is a fixed set of
     * arrangements; but the course is laid out in the RACK TRIANGLE'S own frame,
     * in fractions rather than millimetres, which is exactly what makes it
     * survive a change of table. Measured: all eighteen holes rack with every
     * ball on the cloth, on all three specs and all four shapes. */
    case CUE_GAME_UK8: case CUE_GAME_SNK6: case CUE_GAME_SNK3:
    case CUE_GAME_GOLF:
    case CUE_GAME_KILLER_UK:                       return SPEC_FAM_ENGLISH;
    case CUE_GAME_US8: case CUE_GAME_US9: case CUE_GAME_US10:
    case CUE_GAME_STRAIGHT: case CUE_GAME_ONEPOCKET:
    case CUE_GAME_BANKPOOL:
    case CUE_GAME_ROTATION: case CUE_GAME_ROTATION_PH:
    case CUE_GAME_FIFTEEN: case CUE_GAME_COWBOY: case CUE_GAME_HONOLULU:
    case CUE_GAME_SPEED: case CUE_GAME_BOWLLIARDS:
    case CUE_GAME_CRIBBAGE:
    case CUE_GAME_KILLER_US:                       return SPEC_FAM_AMERICAN;
    case CUE_GAME_CN8:
    case CUE_GAME_KILLER_CN:                       return SPEC_FAM_CHINESE;
    /* Russian pyramid's pockets are barely wider than its ball and that IS the
     * game; cutting them to a spec would be cutting the game up. Bar billiards
     * has no rail pockets at all, and golf's table is a prop for a course. */
    default:                                       return SPEC_FAM_NONE;
    }
}

typedef struct {
    float corner_mm, middle_mm;   /* 0 = leave that pocket type alone */
    float mu_r;                   /* 0 = the engine's own */
    float e_cush_d, efall_d;      /* added to the table's own rail numbers */
} SpecRow;

/* [family][spec], tightest first. The PRO column's openings are all ZERO — "leave
 * this pocket alone" — because pro IS the table the game shipped with, and the
 * shipped pockets were already the hard end of anything worth offering. It gets
 * the fast cloth, which is the other half of a professional table and the half
 * a pocket size cannot express.
 *
 * TOURNAMENT is a few millimetres more generous on the shipped cloth, and it is
 * the DEFAULT — so the game as it comes is slightly kinder than it was, which is
 * a deliberate change and the reason this ladder was turned round. CLUB is more
 * generous again, on a slow cloth over tired cushions. */
static const SpecRow SPEC[SPEC_FAM_COUNT][CUE_SPEC_COUNT] = {
    /* NONE */
    { {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0} },
    /* SNOOKER — shipped 84.0 / 90.8. Tournament templates are quoted around
     * 3 3/8 in and the shipped table is at the tight end of that band, so the
     * step up is a few millimetres rather than a re-cut; club is the 3 3/4 in
     * pocket almost everybody actually learned on. */
    { {  0.0f,   0.0f, 0.0085f,  0.0f,   0.0f },
      { 87.0f,  94.0f, 0.0f,     0.0f,   0.0f },
      { 95.0f, 105.0f, 0.0135f, -0.020f, 0.008f } },
    /* ENGLISH POOL — shipped 81.3 / 81.3, which is already 1.60 ball widths. */
    { {  0.0f,   0.0f, 0.0085f,  0.0f,   0.0f },
      { 84.0f,  86.0f, 0.0f,     0.0f,   0.0f },
      { 89.0f,  92.0f, 0.0135f, -0.020f, 0.008f } },
    /* AMERICAN POOL — shipped 111.1 / 106.4. Tournament takes the 4 1/2 in
     * (114.3 mm) that home and bar tables come with; club a generous 120. */
    { {   0.0f,   0.0f, 0.0085f,  0.0f,   0.0f },
      { 114.3f, 110.0f, 0.0f,     0.0f,   0.0f },
      { 120.0f, 116.0f, 0.0135f, -0.020f, 0.008f } },
    /* CHINESE 8-BALL — shipped 85.7 / 85.7, and cut tight on purpose: 1.50 ball
     * widths is what that game is, so its whole ladder is narrower. */
    { {  0.0f,   0.0f, 0.0085f,  0.0f,   0.0f },
      { 88.0f,  90.0f, 0.0f,     0.0f,   0.0f },
      { 92.0f,  95.0f, 0.0135f, -0.020f, 0.008f } },
};

int cue_table_spec_applies(CueGameKind kind) {
    return spec_family(kind) != SPEC_FAM_NONE;
}

const char *cue_table_spec_blurb(CueGameKind kind, int spec) {
    const int fam = spec_family(kind);
    if (fam == SPEC_FAM_NONE) return "ONE TABLE, AS BUILT";
    if (spec < 0 || spec >= CUE_SPEC_COUNT) spec = CUE_SPEC_TOURNAMENT;
    switch (spec) {
    case CUE_SPEC_PRO:
        return "THE TIGHTEST POCKETS AND THE FASTEST CLOTH";
    case CUE_SPEC_CLUB:
        return "GENEROUS POCKETS, SLOW CLOTH, TIRED CUSHIONS";
    default:
        return "A LITTLE MORE ROOM THAN A PRO TABLE GIVES";
    }
}

/* THE SOLVE. See cue_table.h.
 *
 * Bisection and not a formula, because the opening is the narrowest passage
 * between two tessellated jaws that are built against the bore, and there is no
 * closed form for it — cue_table_openings has to build the world to find out.
 * It is monotonic in the pocket radius (measured across every shipped table, a
 * clean near-linear rise), so bisection converges and cannot land on the wrong
 * root.
 *
 * THE BORE GOES WITH IT, which is not an implementation detail: the bore is the
 * hole cut in the timber, and a fitter opening up a pocket cuts the wood as
 * well as moving the facings. Leaving the bore behind would open the mouth and
 * leave a slot between the end of the cushion and the frame that you can see
 * through — which is the exact fault the bore field was added to close. */
/* The opening this pocket radius gives, or a negative number for "the cushions
 * cannot meet round a hole this size", or exactly 0 for "there is no pocket of
 * this kind on this bed". */
static float cut_measure(CueTable *t, float *pr, float *bore, int middle, float v) {
    *pr = v; *bore = v;
    float c = 0.0f, m = 0.0f;
    cue_table_openings(t, &c, &m);
    return middle ? m : c;
}

static int cut_one(CueTable *t, int middle, float target) {
    if (target <= 0.0f) return 1;
    float *pr   = middle ? &t->pr_side       : &t->pr_corner;
    float *bore = middle ? &t->bore_side     : &t->bore_corner;
    const float pr0 = *pr;
    /* WIDE ENOUGH FOR A CHANGE OF OUTLINE, not just for a spec. A spec moves a
     * pocket by a tenth of a ball width and 0.55 to 1.80 held every one of them
     * comfortably; a shape moves it further, and a range that cannot reach the
     * answer makes this return its closest miss with a straight face. */
    float lo0 = pr0 * 0.35f, hi0 = pr0 * 2.20f;
    /* AND INSIDE WHAT A POCKET CAN BE. The relative range is about how far a
     * solve may travel from where it started; this is about where it may end
     * up, and they are different questions. cue_table_cut_to runs three passes
     * and each starts from where the last left off, so a relative range alone
     * compounds: asked for a 220 mm corner on a snooker table — four ball
     * widths — the radius walked 45 -> 100 -> 137 mm over three passes and
     * reported success on a pocket the validator's own range (20..150 mm) would
     * only just admit, and a fourth pass would have taken it past.
     *
     * A solver that can hand back a table the validator refuses is worse than
     * one that says it could not get there. These are TF(pr_corner) and
     * TF(pr_side)'s own limits; if those move, these follow. */
    if (lo0 < 0.020f) lo0 = 0.020f;
    if (hi0 > 0.150f) hi0 = 0.150f;
    if (hi0 <= lo0)   { return 0; }

    /* SCAN FIRST, THEN BISECT — because the reading is not monotonic
     * everywhere, and a bisection that meets a discontinuity walks confidently
     * into it and stays there.
     *
     * The opening rises cleanly with the radius over the range that matters,
     * but below some radius the cushions can no longer be brought round the
     * hole and the measurement stops being a mouth at all: on Russian pyramid's
     * L it JUMPS to 136 mm, a larger number than the answer, so a bisection
     * reads "too wide" and halves the radius again — away from the answer,
     * every time. Measured: the corner was asked for 71.7 mm and returned 97.5,
     * a third again as wide, on the one table in the game whose whole character
     * is a pocket barely wider than its ball.
     *
     * A coarse sweep cannot be fooled by that. It finds the sample nearest the
     * target and the interval around it, and the bisection then runs inside an
     * interval known to be well-behaved. Sixteen plus twenty builds against the
     * old forty, so it costs nothing, and it is at rack time either way. */
    /* IS THERE A POCKET OF THIS KIND AT ALL? Asked at the radius the table
     * actually has, once, and nowhere else. Zero means the bed carries no
     * pocket of this kind — a regular bed has no middles, bar billiards has no
     * rail pockets — and there is nothing to solve.
     *
     * It must NOT be asked of the scan's samples. A radius at the far end of
     * the range breaks the geometry rather than removing the pocket, and it
     * reads as zero too: asking a snooker table for a 220 mm corner pushed the
     * radius to 99 mm, the measurement went to zero, and this reported success
     * on a table it had not touched. An impossible target has to come back as
     * an impossible target. */
    if (cut_measure(t, pr, bore, middle, pr0) == 0.0f) {
        *pr = pr0; *bore = pr0; return 1;
    }

    const int NS = 16;
    float best = pr0, best_err = 1e9f;
    int   best_i = -1;
    float got_at[NS + 1];
    for (int i = 0; i <= NS; i++) {
        const float v = lo0 + (hi0 - lo0) * (float)i / (float)NS;
        const float got = cut_measure(t, pr, bore, middle, v);
        got_at[i] = got;
        if (got <= 0.0f) continue;         /* not a mouth at this radius */
        const float err = fabsf(got - target);
        if (err < best_err) { best_err = err; best = v; best_i = i; }
    }
    if (best_i < 0) { *pr = pr0; *bore = pr0; return 0; }        /* nothing measurable */

    /* The neighbours of the best sample, skipping any that did not measure, so
     * the bracket is a stretch the reading behaved over. */
    float lo = best, hi = best;
    const float step = (hi0 - lo0) / (float)NS;
    if (best_i > 0  && got_at[best_i - 1] > 0.0f) lo = best - step;
    if (best_i < NS && got_at[best_i + 1] > 0.0f) hi = best + step;

    for (int i = 0; i < 20 && hi > lo; i++) {
        const float mid = 0.5f * (lo + hi);
        const float got = cut_measure(t, pr, bore, middle, mid);
        if (got <= 0.0f) break;
        const float err = got - target;
        if (fabsf(err) < best_err) { best_err = fabsf(err); best = mid; }
        if (fabsf(err) < 0.00002f) break;
        if (err < 0.0f) lo = mid; else hi = mid;
    }
    *pr = best; *bore = best;
    return best_err <= 0.0001f;      /* a tenth of a millimetre */
}

void cue_table_spec(CueTable *t, int spec) {
    if (!t) return;
    const int fam = spec_family(t->kind);
    if (fam == SPEC_FAM_NONE) return;
    /* NO SHORTCUT FOR INDEX 0 ANY MORE. It used to be tournament and a no-op, so
     * "spec 0, do nothing" was both true and quick; index 0 is PRO now and PRO
     * has a cloth of its own, so every spec has something to apply. */
    if (spec < 0 || spec >= CUE_SPEC_COUNT) return;
    const SpecRow *r = &SPEC[fam][spec];
    cue_table_cut_to(t, r->corner_mm * 0.001f, r->middle_mm * 0.001f);
    if (r->mu_r > 0.0f) t->mu_r = r->mu_r;
    if (r->e_cush_d != 0.0f) {
        t->e_cush += r->e_cush_d;
        /* The validator's range, honoured here rather than discovered there. */
        if (t->e_cush < 0.20f) t->e_cush = 0.20f;
        if (t->e_cush > 0.99f) t->e_cush = 0.99f;
    }
    if (r->efall_d != 0.0f) {
        t->cush_efall += r->efall_d;
        if (t->cush_efall < 0.0f)  t->cush_efall = 0.0f;
        if (t->cush_efall > 0.50f) t->cush_efall = 0.50f;
    }
}

/* ---- THE GAME ON SOMEBODY ELSE'S TABLE -----------------------------------
 * See cue_table.h. */
void cue_table_set_game(CueTable *t, CueGameKind kind) {
    if (!t) return;
    if (kind < 0 || kind >= CUE_GAME_COUNT) return;

    /* What this game is, asked of the game itself rather than written out here
     * a second time. cue_table_init already decides all four, per kind, and a
     * second copy of that decision is a second thing to keep in step. */
    CueTable std;
    cue_table_init(&std, kind);

    t->kind       = kind;
    t->is_snooker = std.is_snooker;
    t->reds       = std.reds;
    t->nballs     = std.nballs;

    if (std.is_snooker) {
        /* IN ORDER DOWN THE TABLE, or laid out afresh. The four spots have no
         * rows of their own anywhere — nothing dials blue, pink or black — so
         * either they came from a snooker table and are right, or they came
         * from a pool table and are all sitting at zero. */
        if (!(t->baulk_x < t->blue_x && t->blue_x < t->pink_x &&
              t->pink_x < t->black_x)) {
            t->baulk_x = -t->half_len * 0.6f;
            t->blue_x  =  0.0f;
            t->pink_x  =  t->half_len * 0.5f;
            t->black_x =  t->half_len * 0.82f;
        }
        /* A snooker frame is played from the D, so there has to be one. */
        if (t->d_radius <= 0.0f) t->d_radius = t->half_wid * 0.35f;
        t->house = std.house;
    } else if (std.baulk_x != 0.0f || std.d_radius == 0.0f) {
        /* A HEAD STRING AND NO D, which is what a snooker table's baulk line is
         * not. Only where the game says so: Russian pyramid plays from a HOUSE
         * and keeps its d_radius, which is why this asks the standard table
         * rather than assuming every non-snooker game is American pool. */
        if (std.d_radius <= 0.0f && t->d_radius > 0.0f) {
            t->baulk_x  = -t->half_len * 0.5f;
            t->d_radius = 0.0f;
        }
        t->house = std.house;
    }
    cue_table_normalise(t);
}

/* ---- WHICH TABLE THE GAME IS ON ------------------------------------------
 * See cue_table.h. */
const char *const CUE_TAB_NAME[CUE_TAB_COUNT] = {
    "PRO", "TOURNAMENT", "CLUB", "L-SHAPED", "HEXAGON", "OCTAGON", "ROUND"
};

/* The shape rows. `sides` of 0 means the L. */
static const struct { int sides, every; } TAB_SHAPE[CUE_TAB_COUNT] = {
    { 0, 0 }, { 0, 0 }, { 0, 0 },      /* the three specs are not shapes */
    { 0, 0 },                          /* L */
    { 6, 1 },                          /* hexagon: a pocket at every corner */
    { 8, 1 },                          /* octagon */
    /* ROUND is the same construction with enough sides to read as a curve and a
     * pocket only every tenth of them, so it gets six pockets rather than
     * sixty. Sixty pockets is not a table, it is a colander. */
    { 60, 10 },
};

int cue_table_variant_ok(CueGameKind kind, int variant) {
    if (variant < 0 || variant >= CUE_TAB_COUNT) return 1;
    if (variant <= CUE_TAB_CLUB) return cue_table_spec_applies(kind);
    /* THE SHAPES. What rules a game out is knowing where things are on the
     * cloth in absolute terms.
     *
     * BAR BILLIARDS is nine holes bored at fixed coordinates from the AEBBA
     * rules, three skittles among them, and play from one end: on a hexagon the
     * holes stay where a 1.42 m rectangle put them and several are off the
     * cloth. It is a fixed table and always was — the workshop refuses it as a
     * starting point for the same reason.
     *
     * GOLF is eighteen fixed arrangements read off a scoreboard and scored by
     * how many strokes a hole took. A hole laid out on a round table is not
     * that hole, so the round means nothing.
     *
     * ENGLISH BILLIARDS wants the four spots and the D, which are positions
     * down a rectangle's spine.
     *
     * Everything else racks against the foot spot and plays what is in front of
     * it, which survives a change of outline. */
    switch (kind) {
    /* BAR BILLIARDS is nine holes bored at fixed COORDINATES from the AEBBA
     * rules, three skittles among them and play from one end: on a hexagon the
     * holes stay where a 1.42 m rectangle put them and several are off the
     * cloth. ENGLISH BILLIARDS wants the four spots and the D, which are
     * positions down a rectangle's spine.
     *
     * GOLF USED TO BE HERE AND SHOULD NOT HAVE BEEN. Its course looked like the
     * same objection — eighteen fixed arrangements read off a scoreboard — and
     * it is the opposite: every hole is given in the rack triangle's own frame,
     * u across and v down and both of them fractions, precisely so it survives
     * any table size or ball diameter. Measured on all four shapes and all
     * eighteen holes, every ball lands on the cloth. */
    case CUE_GAME_CAROM_STRAIGHT: case CUE_GAME_CAROM_2C:
    case CUE_GAME_CAROM_3C: case CUE_GAME_CAROM_4B:
    case CUE_GAME_CAROM_1C:
    case CUE_GAME_BARBILLIARDS: case CUE_GAME_BILLIARDS:
        return 0;
    default: return 1;
    }
}

void cue_table_variant(CueTable *t, int variant) {
    if (!t) return;
    if (variant < 0 || variant >= CUE_TAB_COUNT) return;
    if (!cue_table_variant_ok(t->kind, variant)) return;
    if (variant <= CUE_TAB_CLUB) { cue_table_spec(t, variant); return; }

    /* THE POCKETS THIS TABLE HAS, measured before the outline moves, because
     * they are what the shape's pockets will be cut back to.
     *
     * A change of outline changes every corner ANGLE, and the mouth that comes
     * out of a pull-back depends on the angle it is pulled back from —
     * build_ngon already scales the pull-back by sin(45)/sin(A/2) to put it
     * back where a rectangle has it and gets within a few percent, the residue
     * being the bezier knuckle's own shape, which does not sit on the pull-back
     * point and drifts as the corner opens. Measured on the shipped code, a
     * 9 ft American table goes 111.1 -> 118.9 (hexagon) -> 122.4 (octagon) ->
     * 125.9 (round): 1.94 ball widths becoming 2.20, which is a pocket getting
     * a quarter of a ball more generous every time the bed gains sides.
     *
     * Solving for the mouth closes that, and cue_table_cut_to solves for the
     * mouth. So a hexagonal snooker table's corners are a snooker table's
     * corners, and the shape is the only thing that changed — which is the
     * whole point of offering it as the SAME GAME on a different table. */
    float want_c = 0.0f, want_m = 0.0f;
    cue_table_openings(t, &want_c, &want_m);
    if (want_c < 0.0f) want_c = 0.0f;
    if (want_m < 0.0f) want_m = 0.0f;

    /* AS LONG AS THE TABLE IT CAME FROM, which is the table workshop's own
     * rule: the bed's half-length becomes the shape's circumradius and the
     * width follows it. A hexagonal snooker table is then the length of a
     * twelve-footer, which is what somebody asking for one means, and it is a
     * shape the workshop has been building since the bed stopped being a
     * rectangle.
     *
     * IT HELD THE CLOTH AREA CONSTANT FOR A WHILE — solved from
     * (n/2) R^2 sin(2 pi/n) so a frame on a round table was the same size of
     * frame. Defensible, and it cost two invented geometry bugs, because it
     * makes the bed smaller and a many-sided bed's edges are short to begin
     * with. The workshop's rule has none of that and the workshop's polygons
     * already work. A fun table is not worth a second sizing rule. */
    const int sides = TAB_SHAPE[variant].sides;
    const float R = t->half_len;
    if (sides <= 0) {
        t->bed_shape = CUE_BED_L;
        t->bed_hand  = CUE_HAND_RIGHT;
        t->half_wid  = R;
        /* EVEN ARMS — a bite half the width each way, which is two arms of the
         * same width meeting at a square elbow. The workshop's own L, and the
         * shape people mean by one. */
        t->notch_x = t->notch_z = R;
    } else {
        /* A POLYGON BED HAS A LARGEST SIZE, and the big tables come down to
         * it rather than every table shrinking by a fixed fraction.
         *
         * The workshop's rule — circumradius = the bed's half-length — makes a
         * bed as WIDE as the table is long. On a twelve-footer that is 3569 mm
         * across against the rectangle's 3569 x 1778: nearly twice the cloth in
         * the short direction, and it plays as big as it looks. The small beds
         * have no such problem, so a flat fraction is the wrong shape of rule —
         * and it does real damage down there, because the spots scale and the
         * balls do not: at a flat two thirds, six-red snooker put the cue ball
         * 4.6 mm inside the brown.
         *
         * So there is a cap. NGON_MAX_R is set to put the full-size snooker
         * table on three quarters, which is the size that was asked for; a
         * ten-foot bed comes down rather less, a nine-foot barely, and seven
         * feet and under are left exactly where they were. One number, and it
         * says what it means: the biggest a round table gets.
         *
         * The spots come with it — cue_table_init laid them against the table's
         * ORIGINAL length, and this is the only variant that changes that
         * length rather than just its shape. Left behind they sit off the
         * cloth: measured, seventeen of twenty-two balls outside the cushions.
         */
        t->bed_shape = CUE_BED_NGON;
        t->bed_sides = sides;
        t->bed_pocket_every = TAB_SHAPE[variant].every;
        t->notch_x = t->notch_z = 0.0f;
        #define NGON_MAX_R 1.3384f          /* 3/4 of a 12 ft snooker's half-length */
        if (t->half_len > NGON_MAX_R) {
            const float k = NGON_MAX_R / t->half_len;
            t->half_len  = NGON_MAX_R;
            t->baulk_x  *= k;
            t->d_radius *= k;
            t->blue_x   *= k;
            t->pink_x   *= k;
            t->black_x  *= k;
        }
        #undef NGON_MAX_R
        t->half_wid = t->half_len;
    }
    cue_table_normalise(t);
    cue_table_cut_to(t, want_c, want_m);
}

int cue_table_cut_to(CueTable *t, float corner_m, float middle_m) {
    if (!t) return 0;
    /* CORNER, MIDDLE, AND ROUND AGAIN, because on some beds they DO interact.
     *
     * On a rectangle they do not: each solves on its own field, and sweeping
     * either across every shipped table leaves the other where it stood. That
     * was checked, and it was checked on rectangles only.
     *
     * AN L HAS SEVEN POCKETS AND NO SYMMETRY, and cue_table_link_gap answers
     * with one gap per KIND — "ends that disagree get their mean", exact where
     * they are alike and a compromise where they are not. So moving the middles
     * moves the mean the corners were placed against, and the corner solve that
     * ran first is undone by the middle solve that ran second.
     *
     * Three rounds, each starting from where the last left off, so it converges
     * rather than oscillates. Cheap: the solve is at rack time, not in a frame. */
    int ok = 0;
    for (int pass = 0; pass < 3; pass++) {
        ok  = cut_one(t, 0, corner_m);
        ok &= cut_one(t, 1, middle_m);
    }
    cue_table_normalise(t);
    return ok;
}

int cue_table_warnings(const CueTable *t, char *msg, int msgcap) {
    if (msg && msgcap > 0) msg[0] = 0;
    if (!t) return 0;
    const float R  = t->R;
    const float cR = (t->cue_R > 0.0f) ? t->cue_R : R;
    const float big = ((cR > R) ? cR : R) * 2.0f;      /* the widest ball ACROSS */
    float mc = 0.0f, mm = 0.0f;
    cue_table_openings(t, &mc, &mm);
    int n = 0;
    char buf[128];
    /* Zero means there are no rail pockets to measure, which is bar billiards
     * and not a fault; only a pocket that EXISTS can be too small. */
    if (mc < 0.0f) {
        snprintf(buf, sizeof buf,
                 "the corner pockets are too small for the cushions to meet\n");
        if (msg && msgcap > 0) { strncat(msg, buf, (size_t)msgcap - strlen(msg) - 1); }
        n++;
    }
    if (mm < 0.0f) {
        snprintf(buf, sizeof buf,
                 "the middle pockets are too small for the cushions to meet\n");
        if (msg && msgcap > 0) { strncat(msg, buf, (size_t)msgcap - strlen(msg) - 1); }
        n++;
    }
    if (mc > 0.0f && mc < big) {
        snprintf(buf, sizeof buf,
                 "the corner pockets are %.0fmm across and the ball is %.0fmm\n",
                 (double)(mc*1000.0f), (double)(big*1000.0f));
        if (msg && msgcap > 0) { strncat(msg, buf, (size_t)msgcap - strlen(msg) - 1); }
        n++;
    }
    if (mm > 0.0f && mm < big) {
        snprintf(buf, sizeof buf,
                 "the middle pockets are %.0fmm across and the ball is %.0fmm\n",
                 (double)(mm*1000.0f), (double)(big*1000.0f));
        if (msg && msgcap > 0) { strncat(msg, buf, (size_t)msgcap - strlen(msg) - 1); }
        n++;
    }
    return n;
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
        /* CN8   */ { 0.0170f, 1.3550f, 0.2200f,  90.0f },
        /* SNK15 */ { 0.0145f, 1.3550f, 0.2150f,  90.0f },
        /* SNK10 */ { 0.0145f, 1.3550f, 0.2150f,  90.0f },
        /* SNK6  */ { 0.0265f, 1.3550f, 0.2200f,  90.0f },
        /* STRT  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* PYRA — the American's cut, with the SETBACK scaled to this mouth
         * (0.517 of it) rather than copied in millimetres. */
        /* PYRA  */ { 0.0189f, 1.3900f, 0.2200f,  90.0f },
        /* PYRA7 — the same cut with the setback scaled to the smaller mouth */
        /* PYRA7 */ { 0.0168f, 1.3900f, 0.2200f,  90.0f },
        /* BILL — the standard table, so the 12 ft snooker cut exactly */
        /* BILL  */ { 0.0145f, 1.3550f, 0.2150f,  90.0f },
        /* BARB — the holes are in the bed and cut their own cloth. `roll` is
         * how far the cloth turns over the edge, and it is NOT only a drawing
         * number: cue_physics reads lip_d to decide how a dropping ball is
         * gathered to the pocket's axis. Wound up to 0.35 for looks, the ball
         * stopped being taken at all — five of the nine holes simply refused
         * it. 0.22 is the roll a pool pocket has and the most this one will
         * take. */
        /* BARB  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* GOLF — the UK 7 ft bed, so the UK 7 ft corner cut, exactly */
        /* GOLF  */ { 0.0265f, 1.3550f, 0.2200f,  90.0f },
        /* US10 — the same 9 ft American bed as 9-ball, so its cut exactly */
        /* US10  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },
        /* PAUL — the snooker cut, with the SETBACK scaled to this small mouth
         * rather than copied in millimetres: 14.5 mm on a 45 mm snooker pocket
         * is a third of it, and a third of Paul's is 8.4. */
        /* PAUL  */ { 0.0084f, 1.3550f, 0.2150f,  90.0f },
        /* KILLER — the base tables' own cuts, exactly */
        /* K-UK  */ { 0.0265f, 1.3550f, 0.2200f,  90.0f },
        /* K-US  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },
        /* K-CN  */ { 0.0170f, 1.3550f, 0.2200f,  90.0f },
        /* CAROM has no pockets to cut — five rows of nothing, like BARB */
        /* C-SR  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-2C  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-3C  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-4B  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-1C  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* SNK3  */ { 0.0265f, 1.3550f, 0.2200f,  90.0f },   /* the SNK6 cut */
        /* 1POC  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* BANK  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* ROT   */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* ROTPH */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* 15BAL */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* COWBY */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* HONOL */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* SPEED */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
        /* BOWLL */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },
        /* CRIB  */ { 0.0325f, 1.3900f, 0.2200f,  90.0f },   /* the US 9 ft cut */
    };
    static const CueCut mid[] = {
        /* UK8   */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },
        /* US8   */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* US9   */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* CN8   */ { 0.0285f, 1.4437f, 0.2250f, 180.0f },
        /* SNK15 */ { 0.0285f, 1.4437f, 0.2150f, 180.0f },
        /* SNK10 */ { 0.0285f, 1.4437f, 0.2150f, 180.0f },
        /* SNK6  */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },
        /* STRT  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* PYRA  */ { 0.0234f, 1.4100f, 0.2200f, 180.0f },   /* ...and the middle */
        /* PYRA7 */ { 0.0211f, 1.4100f, 0.2200f, 180.0f },
        /* BILL  */ { 0.0285f, 1.4437f, 0.2150f, 180.0f },
        /* BARB  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* GOLF  */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },
        /* US10  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* PAUL  */ { 0.0100f, 1.4437f, 0.2150f, 180.0f },
        /* K-UK  */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },
        /* K-US  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* K-CN  */ { 0.0285f, 1.4437f, 0.2250f, 180.0f },
        /* C-SR  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-2C  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-3C  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-4B  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* C-1C  */ { 0.0000f, 1.0000f, 0.2200f, 360.0f },
        /* SNK3  */ { 0.0250f, 1.4437f, 0.2200f, 180.0f },   /* the SNK6 cut */
        /* 1POC  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* BANK  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* ROT   */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* ROTPH */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* 15BAL */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* COWBY */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* HONOL */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* SPEED */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
        /* BOWLL */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },
        /* CRIB  */ { 0.0305f, 1.4150f, 0.2200f, 180.0f },   /* the US 9 ft cut */
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

        /* THE ROLL CANNOT BE DEEPER THAN THE POCKET IS WIDE.
         *
         * The cloth turns over the edge of the slate in a quarter circle of
         * radius lip_d, so a ball riding that turn has its centre on an arc of
         * radius lip_d + R — and it is only released, with nothing under it at
         * last, once it is that far out over the cut. On a pool or snooker
         * pocket there is room to spare. On a Russian pyramid corner there is
         * not: the mouth is 72 mm for a 67 mm ball, and the deepest a ball can
         * ever get is the middle of the pocket, where it was 0.9 MILLIMETRES
         * short of the release. So it sat there. Dead centre in the pocket, at
         * cloth height, held up by a roll it could not reach the end of, with
         * the shot never settling and the frame unable to continue.
         *
         * 42 shots in 1890 on the 12 ft and 37 on the 7 ft, and not one on any
         * other table — because no other table's ball fills its pocket like
         * that. Pyramid's ball is 0.65 of the pocket radius where snooker's is
         * 0.50, and the roll was a fraction of the pocket that never had to
         * answer to the ball.
         *
         * So it answers to it here: whatever the roll would like to be, it is
         * no deeper than lets a ball clear it before it reaches the deepest
         * point it can physically get to. `cut_out` at the drop centre is that
         * deepest point, exactly, and it is the same function the drop reads —
         * so the two cannot drift apart. Tables with room to spare are not
         * touched at all; this only ever bites where the arithmetic had already
         * become impossible. */
        {   float avail = cue_phys_cut_out(w, p, w->drop_c[p].x, w->drop_c[p].z);
            /* Clear it with something in hand, so the ball is falling freely
             * before it arrives rather than being let go at the last instant. */
            float cap = avail - w->R - 0.15f * w->R;
            if (cap < 0.001f) cap = 0.001f;
            if (w->lip_d[p] > cap) w->lip_d[p] = cap;
        }
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

/* See cue_table.h. The renderer builds the rail plank to the first and bores it
 * to the second; anything fitted to the table asks for them here. */
float cue_table_rail_top(const CueTable *t) {
    if (!t) return 0.0f;
    return t->cushion_h * 1.30f + 0.085f * t->R;   /* rail_h + frame_lift */
}
float cue_table_bore_bot(void) { return -0.002f; }

Vec3 cue_table_cue_home(const CueTable *t) {
    const float CUE_Y = (t->cue_R > 0.0f) ? t->cue_R : t->R;
    /* All games start OFF the centre line so a break naturally strikes the pack
     * at an angle (a dead-straight break into the apex splits poorly). Snooker &
     * UK8 break from one side of the D; US pool from the side of the kitchen.
     * Laid out along the spine, so on an L it is on the baulk arm and the pack
     * is round the corner from it. */
    Vec3 p;
    if (t->kind == CUE_GAME_BARBILLIARDS)
        /* The centre of the D: the break spot itself (Rule 75), and where the
         * last-ball shot must play from (Rule 108). A 4 cm D has no room for
         * the off-centre courtesy the bigger tables get. */
        p = cue_table_lay(t, t->baulk_x, 0.0f, NULL);
    else if (t->is_snooker || t->kind == CUE_GAME_UK8 || CUE_GAME_IS_PYRAMID(t->kind))
        p = cue_table_lay(t, t->baulk_x, -t->d_radius * 0.55f, NULL);
    else
        p = cue_table_lay(t, -cue_table_axis(t) * 0.5f,
                             cue_table_across(t) * 0.40f, NULL);
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
    /* A REGULAR BED IS NOT A UNION OF RECTANGLES, so this can only bound it.
     * That is right for the callers that want a bound — the timber has to cover
     * the cloth, the lamps have to light it — and wrong for anything asking
     * "is this point ON the cloth", which must use cue_world_on_bed instead.
     * The physics does; nothing else asks. */
    if (!t || !out || cap < 1) return 0;
    if (t->bed_shape == CUE_BED_NGON) {
        float r = t->half_len;
        out[0].x0 = -r - g; out[0].x1 = r + g;
        out[0].z0 = -r - g; out[0].z1 = r + g;
        return 1;
    }
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
    /* A REGULAR BED IS NOT A UNION OF RECTANGLES, so this can only bound it.
     * That is right for the callers that want a bound — the timber has to cover
     * the cloth, the lamps have to light it — and wrong for anything asking
     * "is this point ON the cloth", which must use cue_world_on_bed instead.
     * The physics does; nothing else asks. */
    if (!t || !out || cap < 1) return 0;
    if (t->bed_shape == CUE_BED_NGON) {
        float r = t->half_len;
        out[0].x0 = -r; out[0].x1 = r;
        out[0].z0 = -r; out[0].z1 = r;
        return 1;
    }
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
        int rows = (t->reds <= 3) ? 2 : (t->reds <= 6) ? 3
                 : (t->reds <= 10) ? 4 : 5;
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
    Vec3 p = cue_table_lay(t, cue_table_axis(t) * 0.5f, 0.0f, dir);
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
    /* BAULK, THE AREA — Blackball's in-hand region (rules 4c/4h): the full
     * width of the table behind the baulk line, centre of the ball on the
     * line counting as in (4c note). Region 2 from cue_rules_in_hand_anywhere. */
    if (anywhere == 2) {
        const float x0 = -t->half_len + R, x1 = t->baulk_x;
        const float z1 = t->half_wid - R;
        if (p.x < x0) p.x = x0; else if (p.x > x1) p.x = x1;
        if (p.z < -z1) p.z = -z1; else if (p.z > z1) p.z = z1;
        return p;
    }
    /* BALL IN HAND MEANS THE WHOLE TABLE, where the rules say so. The English
     * table is the D under pub rules and the whole cloth under International
     * and Ultimate Pool, so the region cannot be decided by the table alone —
     * the caller passes what the rules of the frame allow. Snooker is always
     * the D. */
    /* ...AND "SNOOKER IS ALWAYS THE D" IS NOT TRUE OF EVERY SNOOKER FRAME.
     *
     * This threw the caller's answer away on any snooker bed, which is right
     * for the frame game and exactly wrong for the two formats that exist to
     * be different: the Shoot Out and THE 900 both give ball in hand ANYWHERE
     * after a foul, and cue_rules_in_hand_anywhere has always said so. The
     * table overruled it, so every placement — the player's and, far more
     * visibly, the AI's, whose whole candidate sweep is filtered through this
     * clamp — collapsed back into the D. A machine that answers every foul by
     * putting the white in the D on a table where it may go anywhere is
     * playing a much smaller game than the one in the rule book.
     *
     * WHICH region is a table question; WHETHER the whole cloth is allowed is a
     * rules question, and the caller is the one holding the rules. */
    if (anywhere) {
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
    /* BAR BILLIARDS IS PLAYED FROM THE D, EVERY STROKE (Rule 91), and it was
     * not in this list. cue_rules_in_hand_anywhere already said "not anywhere"
     * for it, correctly — but that only decides whether the whole cloth is
     * allowed, and WHICH region is decided here. Missing from both branches it
     * fell through to the US pool case at the bottom, which clamps to the cloth
     * and nothing else: the ball could be put down anywhere on the table. Rule
     * 75 gives it a D of about 4 cm at the centre of the base, and that is the
     * same shape snooker's is, so it is the same code. */
    if (t->is_snooker || t->kind == CUE_GAME_UK8 || CUE_GAME_IS_PYRAMID(t->kind) ||
        t->kind == CUE_GAME_BARBILLIARDS) {
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

/* CAROM'S OPENING. Three-ball games: the red on the foot spot, the
 * opponent's ball on the head spot, the breaker's ball on the head string a
 * hand's width to the side — the standard lag-winner's position. Four-ball:
 * the two reds take the foot and head spots and the cue balls sit either
 * side of the head string; the coordinates are chosen the way bar
 * billiards' holes are, and the rules give everything else. */
static int rack_carom(const CueTable *t, CueBall *b) {
    const float R = t->R;
    const float head = -t->half_len * 0.5f, foot = t->half_len * 0.5f;
    set_ball(&b[0], CUE_ID_CUE,        head, -0.1825f, R);   /* the striker */
    if (t->kind == CUE_GAME_CAROM_4B) {
        set_ball(&b[1], CUE_ID_BIL_RED,    foot,  0.0f, R);
        set_ball(&b[2], 2,                 head,  0.0f, R);  /* the second red */
        set_ball(&b[3], CUE_ID_BIL_YELLOW, head,  0.1825f, R);
        return 4;
    }
    set_ball(&b[1], CUE_ID_BIL_RED,    foot, 0.0f, R);
    set_ball(&b[2], CUE_ID_BIL_YELLOW, head, 0.0f, R);
    return 3;
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

/* ROTATION: the same triangle, racked to rotation's own rule — 1 at the apex on
 * the foot spot, 2 and 3 in the two back corners, 15 in the centre, and the
 * rest wherever they fall. That arrangement is not decoration: the 1 is the
 * ball you must hit and it is the one nearest you, the two next-lowest are the
 * furthest away, and the biggest score sits in the middle of the pack where it
 * cannot be had cheaply. A standard eight-ball triangle would put the low balls
 * at the back and make the opening entirely different. */
static int rack_rotation(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float footx = foot.x, footz = foot.z;
    const float dx = R * 1.7320508f;
    #define ROT_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                           (footz + up.z*(r_) + side.z*(o_))
    static const int rows[5][5] = {
        { 1 }, { 9, 12 }, { 10, 15, 4 }, { 6, 13, 11, 8 }, { 2, 7, 14, 5, 3 },
    };
    int n = 1;
    for (int row = 0; row < 5; row++)
        for (int k = 0; k <= row; k++)
            set_ball(&b[n++], rows[row][k],
                     ROT_AT(row * dx, -(row) * R + k * 2.0f * R), R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef ROT_AT
    return n;
}

/* COWBOY POOL: the 1, the 3 and the 5, and nothing else on the table.
 *
 * THE 1 ON THE HEAD SPOT, THE 3 ON THE FOOT SPOT, THE 5 IN THE CENTRE. That is
 * the rule as written, and this had all three of them wrong — 1 on the foot, 3
 * in the centre, 5 on the head — which is the same three spots with the balls
 * rotated round them. It looks identical until you play it: the ball you must
 * cannon off FIRST for the hundred-and-first point is the 1, and having it at
 * the wrong end of the table changes the whole shape of the endgame.
 *
 * Spread the length of the bed, because a game whose last eleven points are
 * cannons wants its balls apart rather than racked.
 *
 * WHERE EACH ONE LIVES IS ASKED TWICE — once to lay the table out and once to
 * put a potted ball back — so it is answered in one place. Two copies of this
 * is exactly how the balls came to be racked on one set of spots and respotted
 * onto another. */
Vec3 cue_table_cowboy_spot(const CueTable *t, int id) {
    switch (id) {
    case 1:  return cue_table_lay(t, t->baulk_x, 0.0f, NULL);   /* head spot */
    case 5:  return cue_table_lay(t, 0.0f, 0.0f, NULL);         /* centre    */
    default: return cue_table_foot_spot(t);                     /* the 3     */
    }
}

static int rack_cowboy(const CueTable *t, CueBall *b) {
    const float R = t->R;
    const Vec3 one   = cue_table_cowboy_spot(t, 1);
    const Vec3 three = cue_table_cowboy_spot(t, 3);
    const Vec3 five  = cue_table_cowboy_spot(t, 5);
    set_ball(&b[1], 1, one.x,   one.z,   R);
    set_ball(&b[2], 3, three.x, three.z, R);
    set_ball(&b[3], 5, five.x,  five.z,  R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    return 4;
}

/* BOWLLIARDS: the 1 to the 10 in a FOUR-ROW triangle — one, two, three, four
 * from the apex — with the apex on the foot spot. Ten balls make a four-row
 * triangle and fifteen make a five-row one, which is the whole reason the game
 * is played with ten: the rack is the set of pins.
 *
 * WHICH ball sits where is not a rule. The BCA rack for this designates no
 * position at all — unlike rotation, where the 1 at the apex is the ball you
 * are obliged to hit, and unlike nine-ball, where the 9 in the middle is the
 * money. Bowlliards calls its ball every stroke and any of the ten will do, so
 * the numbers carry no meaning past telling one ball from another when you name
 * it. They go in numerically because a rack has to be SOME arrangement and a
 * reproducible one is worth having in a test; nothing reads it.
 *
 * The ten-ball rack next door is a DIAMOND of the same ten balls and is not
 * this: a diamond has a ball at the back point and this has a flat back row, so
 * the two break quite differently. Borrowing rack_10ball would have been the
 * obvious saving and it is the wrong shape. */
static int rack_bowlliards(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float footx = foot.x, footz = foot.z;
    const float dx = R * 1.7320508f;
    #define BWL_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                           (footz + up.z*(r_) + side.z*(o_))
    static const int rows[4][4] = {
        { 1 }, { 2, 3 }, { 4, 5, 6 }, { 7, 8, 9, 10 },
    };
    int n = 1;
    for (int row = 0; row < 4; row++)
        for (int k = 0; k <= row; k++)
            set_ball(&b[n++], rows[row][k],
                     BWL_AT(row * dx, -(row) * R + k * 2.0f * R), R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef BWL_AT
    return n;
}

/* CRIBBAGE POOL: the ordinary fifteen-ball triangle with two things fixed in
 * it, and only one of them looks like a rule.
 *
 * THE 15 GOES IN THE MIDDLE, where the black sits in eight-ball — the centre of
 * the third row, the one position with a ball on every side of it. It is the
 * ball that decides a level game, so the rack buries it.
 *
 * NO TWO OF THE THREE CORNERS MAY TOTAL FIFTEEN, which is the odd one and is
 * worth knowing what it is for. The corners are the three balls a break can
 * reach cleanly, and a pair of them adding to fifteen is a cribbage sitting in
 * the open before anybody has played a stroke — the breaker takes both off the
 * corners and is a fifth of the way to the game for nothing. So the rack is
 * required to break up any such pair before the balls go down. The apex, the
 * left back corner and the right back corner here are the 1, the 2 and the 3,
 * which totals three, four and five between them and cannot offend.
 *
 * THE REST ARE "PLACED AT RANDOM" IN THE BOOK AND ARE NOT PLACED AT RANDOM
 * HERE. A rack crosses the wire in a match and both ends have to lay out the
 * same triangle from it, and a test that wants to read a position back needs
 * one to read; every other rack in this file is fixed for the same two reasons.
 * So the eleven free balls go in numerically, in the order the positions fall,
 * which is a reproducible arrangement rather than a meaningful one. Nothing
 * reads it — unlike rotation next door, where the apex is the ball you are
 * obliged to hit. The arrangement happens to put the 4 and the 11 side by side
 * in the second row; that is a consequence of counting, not a favour.
 *
 * Note that this is NOT rack_rotation, which also carries the 1 at the apex,
 * the 15 in the centre and the 2 and 3 in the back corners. The two arrived
 * there down different roads — rotation's is about which ball is on, this one
 * is about which pairs are exposed — and sharing a function would tie two racks
 * together that have no reason to move together. */
static int rack_cribbage(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float footx = foot.x, footz = foot.z;
    const float dx = R * 1.7320508f;
    #define CRB_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                           (footz + up.z*(r_) + side.z*(o_))
    static const int rows[5][5] = {
        { 1 }, { 4, 5 }, { 6, 15, 7 }, { 8, 9, 10, 11 }, { 2, 12, 13, 14, 3 },
    };
    int n = 1;
    for (int row = 0; row < 5; row++)
        for (int k = 0; k <= row; k++)
            set_ball(&b[n++], rows[row][k],
                     CRB_AT(row * dx, -(row) * R + k * 2.0f * R), R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef CRB_AT
    return n;
}

/* FIFTEEN-BALL: the 15 at the apex on the foot spot, the 13 and 14 in the back
 * corners — the mirror of rotation's rack, and for the mirror reason. Here the
 * biggest prize is the ball nearest you and the next two are furthest away, so
 * the opening is a shot at something worth having rather than a toll. */
static int rack_fifteen(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float footx = foot.x, footz = foot.z;
    const float dx = R * 1.7320508f;
    #define F15_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                           (footz + up.z*(r_) + side.z*(o_))
    static const int rows[5][5] = {
        { 15 }, { 7, 4 }, { 6, 1, 11 }, { 10, 3, 9, 5 }, { 13, 8, 2, 12, 14 },
    };
    int n = 1;
    for (int row = 0; row < 5; row++)
        for (int k = 0; k <= row; k++)
            set_ball(&b[n++], rows[row][k],
                     F15_AT(row * dx, -(row) * R + k * 2.0f * R), R);
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef F15_AT
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

/* TEN-BALL: the triangle, and the three balls in it that are not free.
 *
 * WPA 3.3: "the 1 ball at the apex, the 10 ball in the middle of the rack, and
 * the 2 and 3 balls on the two back corners. The remaining balls are placed at
 * random." The middle of a ten-ball triangle is the centre of the THIRD row —
 * the only position with a ball on every side of it, which is what makes the 10
 * hard to get at and the game what it is.
 *
 * The rest are placed in ascending order rather than at random. A rack the game
 * lays out the same way every time is one a player can learn, which is how
 * every other rack here works, and randomising it would make the break a
 * lottery rather than a shot.
 *
 * TEN BALLS, NOT NINE, is most of the difference from rack_9ball above: the
 * diamond has open lanes off the second ball from the moment it is struck and
 * this does not. */
static int rack_10ball(const CueTable *t, CueBall *b) {
    const float R = t->R;
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float footx = foot.x, fz = foot.z;
    const float dx = R * 1.7320508f;              /* row pitch: the triangle's */
    #define RACK_AT(r_, o_) (footx + up.x*(r_) + side.x*(o_)), \
                            (fz    + up.z*(r_) + side.z*(o_))
    set_ball(&b[ 1],  1, RACK_AT(0.0f,    0.0f),  R);   /* apex */
    set_ball(&b[ 2],  4, RACK_AT(dx,     -R),     R);
    set_ball(&b[ 3],  5, RACK_AT(dx,      R),     R);
    set_ball(&b[ 4],  6, RACK_AT(2*dx,   -2*R),   R);
    set_ball(&b[ 5], 10, RACK_AT(2*dx,    0.0f),  R);   /* the middle of the rack */
    set_ball(&b[ 6],  7, RACK_AT(2*dx,    2*R),   R);
    set_ball(&b[ 7],  2, RACK_AT(3*dx,   -3*R),   R);   /* back corner */
    set_ball(&b[ 8],  8, RACK_AT(3*dx,   -R),     R);
    set_ball(&b[ 9],  9, RACK_AT(3*dx,    R),     R);
    set_ball(&b[10],  3, RACK_AT(3*dx,    3*R),   R);   /* and the other */
    { Vec3 h = cue_table_cue_home(t); set_ball(&b[0], CUE_ID_CUE, h.x, h.z, R); }
    #undef RACK_AT
    return 11;
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

/* BAR BILLIARDS: the break position, and the rest of the balls in the rack.
 *
 * AEBBA Rule 92: "The red ball shall be placed by the hand on the red spot and
 * a white ball placed on the break spot." That is the whole opening — two
 * balls on the table and the other six waiting in the trough. Rule 94 sends
 * play back to it whenever the table empties.
 *
 * The waiting balls are OFF, and the host feeds them out one at a time as the
 * player calls for them. Index 0 is the ball being struck, as everywhere. */
static int rack_barbilliards(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    /* THE SEVEN WHITES CARRY SEVEN IDS, even though they are interchangeable.
     *
     * The engine tells balls apart by id — the renderer colours by it, the
     * rules read it, and the planner skips the CUE BALL by it. Give every
     * white the cue ball's id and the planner can see no object ball at all
     * except the red; the easiest "pot" on the table becomes rolling the ball
     * in its hand straight down a hole, which is a foul, and it played five
     * hundred of them in a row. So index 0 is the ball in hand and keeps id 0,
     * and the rest are 2..7 — all drawn the same white by the set, because on
     * the table they ARE the same. */
    set_ball(&b[n++], CUE_ID_CUE, t->baulk_x, 0.0f, R);
    /* The red on the red spot, 175 mm up the table (Rule 76). */
    set_ball(&b[n++], CUE_ID_BIL_RED, t->blue_x, 0.0f, R);
    /* Six more whites, in the trough. Rule 79: eight balls in all. */
    for (int i = 0; i < 6; i++) {
        set_ball(&b[n], (uint8_t)(2 + i), t->baulk_x, 0.0f, R);
        b[n].on = 0;
        n++;
    }
    return n;
}

/* ---- PAUL: THE WHOLE SET, THROWN ON ---------------------------------------
 *
 * Every other rack in this file is a shape somebody can learn: a triangle, a
 * diamond, four spots down a spine. This one is deliberately not. The set goes
 * on at random and differently every game, which is the whole of Paul's opening
 * — there is nothing to break and nothing to memorise, only a table you have to
 * read from scratch every time.
 *
 * REJECTION SAMPLING, and it is the right tool: the constraints are "on the
 * cloth", "not touching another ball" and "not over a pocket", none of which
 * compose into a formula, and all of which are cheap to test. Twenty-two balls
 * on a 6 ft bed is about a tenth of the cloth covered, so a candidate is
 * accepted far more often than not and the whole layout costs a few hundred
 * tries.
 *
 * A BUDGET, AND A FALLBACK. A loop that samples until it succeeds is a loop
 * that can run for ever, and this one runs at the start of a frame in a headset.
 * So each ball gets a fixed number of attempts and then settles for the best
 * candidate it saw — the one furthest from its nearest neighbour. That can
 * produce a touching pair on a table crowded by bad luck, which is a legal
 * position and a perfectly ordinary thing to find on a real table somebody has
 * thrown the balls onto.
 *
 * DETERMINISTIC, from cue_table_paul_set_seed. See the header: a layout that
 * cannot be reproduced cannot be tested, photographed twice, or sent to the
 * other end of a link.
 *
 * CLEAR OF THE POCKETS by a ball's radius past the drop, because a ball resting
 * inside a pocket's catch is potted the instant the physics runs and the frame
 * would start by scoring for nobody. */
static uint32_t s_paul_seed = 1u;
void     cue_table_paul_set_seed(uint32_t seed) { s_paul_seed = seed ? seed : 1u; }
uint32_t cue_table_paul_seed(void) { return s_paul_seed; }

/* xorshift32, which is all this needs: the layout has to be reproducible and
 * unremarkable, not cryptographic. */
static uint32_t paul_rand(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*st = x);
}
static float paul_frand(uint32_t *st) {
    return (float)(paul_rand(st) & 0xFFFFFFu) / 16777216.0f;
}

static int rack_paul(const CueTable *t, CueBall *b) {
    const float R = t->R;
    /* THE SEED IS SCRAMBLED BEFORE IT IS USED, and it has to be. xorshift's
     * first few outputs are strongly correlated with a small state, so seeds 1
     * to 10 — which is exactly what a caller counting frames hands over — laid
     * the white down in almost the same place every time: measured, x marched
     * from -854 mm to -598 in even steps while z alternated between two values.
     * Ten "random" tables with the cue ball on a line.
     *
     * A multiply-xor finaliser (splitmix32's) spreads a counter across the whole
     * word before the shift register ever sees it, so consecutive seeds are as
     * unalike as distant ones. */
    uint32_t st = s_paul_seed;
    st += 0x9E3779B9u;
    st = (st ^ (st >> 16)) * 0x85EBCA6Bu;
    st = (st ^ (st >> 13)) * 0xC2B2AE35u;
    st ^= st >> 16;
    if (!st) st = 1u;

    /* THE SET, in the order it is laid down: the white FIRST, so it gets the
     * pick of the table and the frame always has a cue ball that is not jammed
     * in a corner by twenty-one balls placed before it. Then the black, then
     * the colours, then the reds — most valuable first, for the same reason.
     * The reds are what end up wedged, and there are fifteen of them. */
    int id[CUE_MAX_BALLS], n = 0;
    id[n++] = CUE_ID_CUE;
    id[n++] = CUE_ID_BLACK;
    for (int v = CUE_ID_YELLOW; v < CUE_ID_BLACK; v++) id[n++] = v;
    for (int i = 0; i < 15; i++) id[n++] = 1;      /* the reds all share id 1 */

    /* The window balls may land in: a ball's width inside the cushion nose all
     * round, so nothing starts touching the rubber. */
    const float mx = t->half_len - R * 1.6f;
    const float mz = t->half_wid - R * 1.6f;

    /* A CueWorld to ask about the cloth and the pockets. Static because it is
     * far too big for a stack this deep, and there is one rack at a time. */
    static CueWorld w;
    cue_table_build_world((CueTable *)t, &w);

    for (int i = 0; i < n; i++) {
        float bx = 0.0f, bz = 0.0f, best_gap = -1.0f;
        for (int tries = 0; tries < 120; tries++) {
            const float x = (paul_frand(&st) * 2.0f - 1.0f) * mx;
            const float z = (paul_frand(&st) * 2.0f - 1.0f) * mz;
            /* THE WHOLE BALL, not its centre. The window above is a
             * rectangle inset by a ball and a half, which is the entire guard
             * on a rectangular bed and none at all on a polygon — there the
             * cushion line cuts diagonally across that window, and a centre
             * passing the point test can still be half buried in the rubber. */
            if (!cue_world_ball_on_bed(&w, x, z, R * 1.15f)) continue;
            /* Clear of every pocket's catch, with a ball's radius to spare. */
            int in_pocket = 0;
            for (int p = 0; p < w.npocket && !in_pocket; p++) {
                const float dx = x - w.drop_c[p].x, dz = z - w.drop_c[p].z;
                const float clear = w.pocket_r[p] + R;
                if (dx*dx + dz*dz < clear*clear) in_pocket = 1;
            }
            if (in_pocket) continue;
            /* And how far it is from its nearest neighbour, which is both the
             * test and the tie-breaker for the fallback. */
            float gap = 1e30f;
            for (int k = 0; k < i; k++) {
                const float dx = x - b[k].pos.x, dz = z - b[k].pos.z;
                const float d = sqrtf(dx*dx + dz*dz) - 2.0f * R;
                if (d < gap) gap = d;
            }
            if (gap > best_gap) { best_gap = gap; bx = x; bz = z; }
            if (gap > R * 0.25f) break;            /* good enough: take it */
        }
        set_ball(&b[i], id[i], bx, bz, R);
    }
    /* The white is index 0 by construction — see the order above — and gets its
     * own size stamped on it by cue_table_rack like every other game's. */
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
    /* reds triangle: 2 rows (3), 3 rows (6), 4 rows (10) or 5 rows (15), apex behind pink,
     * and laid out in the PINK'S own frame so that on an L it grows up the arm
     * the pink is on rather than off the side of it. */
    int rows = (t->reds <= 3) ? 2 : (t->reds <= 6) ? 3
             : (t->reds <= 10) ? 4 : 5;
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
    const float footx = cue_table_axis(t) * 0.5f;
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
    const float footx = cue_table_axis(t) * 0.5f;
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

/* ...AND ONE NAMED BALL, for a ball that has been driven off the table. Same
 * foot-spot-and-up rule; see the header for why it cannot just be respot_one. */
/* THE TWO FOOT-CORNER POCKETS — see the header for why by position. */
int cue_table_foot_pockets(const CueTable *t, const CueWorld *w,
                           int *left, int *right) {
    if (!t || !w || w->npocket < 2) return 0;
    int l = -1, r = -1;
    for (int i = 0; i < w->npocket; i++) {
        if (w->pocket_mid[i]) continue;                 /* corners only */
        if (w->pocket[i].x <= 0.0f) continue;           /* the FOOT half */
        if (w->pocket[i].z < 0.0f) {
            if (l < 0 || w->pocket[i].x > w->pocket[l].x) l = i;
        } else {
            if (r < 0 || w->pocket[i].x > w->pocket[r].x) r = i;
        }
    }
    if (l < 0 || r < 0) return 0;
    if (left)  *left  = l;
    if (right) *right = r;
    return 1;
}

int cue_table_respot_ball(const CueTable *t, CueBall *b, int n, int idx) {
    if (!t || !b || idx < 0 || idx >= n) return 0;
    Vec3 up; Vec3 foot = cue_table_foot_spot_dir(t, &up);
    /* COWBOY'S THREE BALLS EACH GO BACK TO THEIR OWN SPOT.
     *
     * Everywhere else a spotted ball goes to the foot spot, and that is right
     * because everywhere else there is only one spot to go to. Cowboy has
     * three and the balls are not interchangeable — they are worth one, three
     * and five — so sending every one of them to the foot spot slowly collected
     * all three at that end of the table, which is not the game.
     *
     * The direction to walk if the spot is taken stays the same: up the table
     * from wherever the ball belongs. */
    if (t->kind == CUE_GAME_COWBOY) foot = cue_table_cowboy_spot(t, b[idx].id);
    const float R = t->R;
    for (int step = 0; step < 60; step++) {
        Vec3 p = v3(foot.x + up.x * (float)step * 2.05f * R, R,
                    foot.z + up.z * (float)step * 2.05f * R);
        if (!cue_table_on_bed(t, p.x, p.z)) continue;
        int clash = 0;
        for (int j = 0; j < n && !clash; j++) {
            if (j == idx || !b[j].on) continue;
            float dx = b[j].pos.x - p.x, dz = b[j].pos.z - p.z;
            if (dx*dx + dz*dz < (2.0f*R)*(2.0f*R) * 0.98f) clash = 1;
        }
        if (clash) continue;
        b[idx].pos = p;
        b[idx].vel = v3(0,0,0);
        b[idx].w   = v3(0,0,0);
        b[idx].drop = 0.0f;
        b[idx].pocket = 0;
        b[idx].on = 1;
        b[idx].orient = rand_orient();
        return 1;
    }
    return 0;
}

/* THE COURSE, off the Billiard & Golf board. See cue_table.h for the frame. */
const CueGolfHole CUE_GOLF_COURSE[CUE_GOLF_HOLES] = {
    /*  1 */ { 4, 3, { 0.5000f, 0.1000f, 0.9000f, 0.0f },
                     { 0.0000f, 1.0000f, 1.0000f, 0.0f } },   /* apex + both base corners */
    /*  2 */ { 3, 2, { 0.5000f, 0.5000f, 0.0f, 0.0f },
                     { 0.0000f, 1.0000f, 0.0f, 0.0f } },   /* apex + base centre */
    /*  3 */ { 5, 4, { 0.5000f, 0.4000f, 0.6000f, 0.5000f },
                     { 0.0000f, 0.2500f, 0.2500f, 0.5000f } },   /* a diamond */
    /*  4 */ { 4, 3, { 0.5000f, 0.4000f, 0.6000f, 0.0f },
                     { 0.0000f, 0.2500f, 0.2500f, 0.0f } },   /* a 1-2 triangle */
    /*  5 */ { 5, 4, { 0.5000f, 0.4000f, 0.3000f, 0.2000f },
                     { 0.0000f, 0.2500f, 0.5000f, 0.7500f } },   /* the left edge */
    /*  6 */ { 4, 3, { 0.5000f, 0.5000f, 0.9000f, 0.0f },
                     { 0.0000f, 0.2887f, 1.0000f, 0.0f } },   /* a column of two + the far corner */
    /*  7 */ { 3, 2, { 0.5000f, 0.6000f, 0.0f, 0.0f },
                     { 0.0000f, 0.2500f, 0.0f, 0.0f } },   /* apex + one to its right */
    /*  8 */ { 5, 4, { 0.5000f, 0.5000f, 0.1000f, 0.9000f },
                     { 0.0000f, 0.2887f, 1.0000f, 1.0000f } },   /* a column of two + both base corners */
    /*  9 */ { 5, 4, { 0.5000f, 0.4000f, 0.6000f, 0.3000f },
                     { 0.0000f, 0.2500f, 0.2500f, 0.5000f } },   /* apex, the row-2 pair, then row-3 left */
    /* 10 */ { 3, 2, { 0.4000f, 0.6000f, 0.0f, 0.0f },
                     { 1.0000f, 1.0000f, 0.0f, 0.0f } },   /* a pair CENTRED on the back row */
    /* 11 */ { 4, 3, { 0.5000f, 0.6000f, 0.7000f, 0.0f },
                     { 0.0000f, 0.2500f, 0.5000f, 0.0f } },   /* the right edge */
    /* 12 */ { 4, 3, { 0.5000f, 0.5000f, 0.5000f, 0.0f },
                     { 0.0000f, 0.2887f, 0.5774f, 0.0f } },   /* a column of three */
    /* 13 */ { 3, 2, { 0.5000f, 0.5000f, 0.0f, 0.0f },
                     { 0.0000f, 0.2887f, 0.0f, 0.0f } },   /* a column of two */
    /* 14 */ { 5, 4, { 0.5000f, 0.6000f, 0.7000f, 0.1000f },
                     { 0.0000f, 0.2500f, 0.5000f, 1.0000f } },   /* the right edge + the far base corner */
    /* 15 */ { 4, 3, { 0.3000f, 0.5000f, 0.7000f, 0.0f },
                     { 1.0000f, 1.0000f, 1.0000f, 0.0f } },   /* three along the base */
    /* 16 */ { 3, 2, { 0.1000f, 0.9000f, 0.0f, 0.0f },
                     { 1.0000f, 1.0000f, 0.0f, 0.0f } },   /* the two base corners */
    /* 17 */ { 4, 3, { 0.5000f, 0.1000f, 0.5000f, 0.0f },
                     { 0.0000f, 1.0000f, 1.0000f, 0.0f } },   /* apex + two on the base */
    /* 18 */ { 5, 4, { 0.5000f, 0.5000f, 0.5000f, 0.5000f },
                     { 0.0000f, 0.2887f, 0.5774f, 0.8660f } },   /* a column of four */
};

const char *const CUE_GOLF_ROUND_NAME[CUE_GOLF_ROUNDS] = {
    "18 HOLES", "FRONT NINE", "BACK NINE",
    "MATCHPLAY 18", "MATCHPLAY FRONT 9", "MATCHPLAY BACK 9",
};

int cue_golf_par(int from_hole, int to_hole) {
    int p = 0;
    for (int h = from_hole; h <= to_hole && h < CUE_GOLF_HOLES; h++)
        if (h >= 0) p += CUE_GOLF_COURSE[h].par;
    return p;
}

/* ---- BILLIARDS GOLF: set a hole out -------------------------------------
 *
 * The layout is in the rack triangle's frame, so putting it on the cloth is
 * the same sum every game already does for a rack: the apex ball goes on the
 * spot the pack is racked from, u runs across the table and v runs away from
 * the player. A five-ball base is ten radii across and the four row-steps are
 * 1.732 radii each, which is what makes the standard positions come out
 * touching.
 *
 * The cue ball goes on the baulk spot — the board's "Starting Point" — and it
 * goes back there whenever it is potted (Rule 2), so it is one place. */
static int rack_golf(const CueTable *t, CueBall *b, int hole) {
    const float R = t->R;
    int n = 0;
    { Vec3 q = cue_table_lay(t, t->baulk_x, 0.0f, NULL);
      set_ball(&b[n++], CUE_ID_CUE, q.x, q.z, R); }
    if (hole < 0) hole = 0;
    if (hole >= CUE_GOLF_HOLES) hole = CUE_GOLF_HOLES - 1;
    const CueGolfHole *g = &CUE_GOLF_COURSE[hole];
    /* IN THE FOOT SPOT'S OWN FRAME, exactly as rack_pool builds its triangle:
     * `up` runs away from the player and `side` across, so an L-shaped bed
     * turns the layout with the spine instead of laying it across a cushion. */
    Vec3 up; const Vec3 foot = cue_table_foot_spot_dir(t, &up);
    const Vec3 side = v3(-up.z, 0.0f, up.x);
    const float baseW = 10.0f * R;               /* five balls across */
    const float triH  = 4.0f * 1.7320508f * R;   /* four row-steps down */
    for (int i = 0; i < g->n; i++) {
        float across = (g->u[i] - 0.5f) * baseW;
        float along  = g->v[i] * triH;
        set_ball(&b[n++], (uint8_t)(1 + i),
                 foot.x + up.x*along + side.x*across,
                 foot.z + up.z*along + side.z*across, R);   /* reds 1.. */
    }
    return n;
}

/* The hole the next rack sets out. Held here because cue_table_rack takes no
 * argument for it and every caller in the game already goes through it. */
static int s_golf_hole = 0;
void cue_table_golf_set_hole(int hole) {
    s_golf_hole = (hole < 0) ? 0 : (hole >= CUE_GOLF_HOLES ? CUE_GOLF_HOLES-1 : hole);
}
int cue_table_golf_hole(void) { return s_golf_hole; }

/* ---- THE RACK'S OWN TOLERANCE -------------------------------------------
 *
 * A rack is laid out on a perfect lattice with every ball exactly touching its
 * neighbours, and that is the one arrangement a break cannot open up. Measured
 * on the 9 ft table: fired straight into a perfect pack at any pace from 6 to
 * 12 m/s, exactly EIGHT of the fifteen balls ever exceed half a metre a second.
 * The same eight every time, whatever the pace, and wherever the cue ball is
 * aimed -- a symmetric lattice struck down its own axis has no lateral
 * component anywhere in it to give the rest. Seven balls sit in the triangle,
 * and the complaint is that they always do.
 *
 * IT IS NOT ENERGY. Three quarters of the cue ball's kinetic energy arrives in
 * the pack either way, and the figure goes UP with pace. Nor is it the solver's
 * step rate: the travel figures are identical from 60 Hz to 4000 Hz.
 *
 * AND OPENING THE RACK UP IS THE WRONG FIX. A uniform gap measures WORSE at
 * every pace below a full-blooded break -- the apex has to cross the gap before
 * it touches anything, and the energy is gone by the time it does. A tight rack
 * really is the best rack, exactly as the trade has always said.
 *
 * What is missing is asymmetry, not slack. Every ball in the pack is nudged by
 * up to CUE_RACK_TOL of its own radius, which on a pool ball is under a
 * millimetre -- tighter than any rack a human has ever set with a triangle --
 * and that is enough: mean travel rises by a quarter, total ball movement by
 * the same, and the balls left sitting where they were racked halve.
 *
 * A SEED, AND NOT A CALL TO RANDOM, for the reasons already set out above
 * Paul's scatter: a rack has to be reproducible, has to be testable, and has to
 * be sendable to the other end of a link in a packet. Seed 0 is the perfect
 * lattice this always built, so nothing that does not ask for a tolerance gets
 * one and every existing test still sees the rack it was written against. */
static unsigned s_rack_seed = 0;
void cue_table_rack_set_seed(unsigned seed) { s_rack_seed = seed; }
unsigned cue_table_rack_seed(void) { return s_rack_seed; }

static void rack_tolerance(const CueTable *t, CueBall *b, int n) {
    if (!s_rack_seed) return;
    unsigned r = s_rack_seed * 2654435761u + 0x9E3779B9u;
    for (int i = 1; i < n; i++) {
        if (!b[i].on) continue;
        /* A SPOTTED BALL IS ON ITS SPOT. Snooker's colours are placed by the
         * rules, not by the triangle, and a colour a millimetre off its spot is
         * a colour that will not respot to the same place. Only the pack moves. */
        if (b[i].id >= CUE_ID_YELLOW) continue;
        const float rad = (b[i].r > 0.0f) ? b[i].r : t->R;
        for (int k = 0; k < 2; k++) {
            r = r * 1664525u + 1013904223u;
            const float u = (float)((r >> 8) & 0xFFFFu) / 65535.0f - 0.5f;
            const float d = u * 2.0f * CUE_RACK_TOL * rad;
            if (k == 0) b[i].pos.x += d; else b[i].pos.z += d;
        }
    }
}

int cue_table_rack(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    int n;
    if (t->kind == CUE_GAME_GOLF) n = rack_golf(t, balls, s_golf_hole);
    else if (CUE_GAME_IS_CAROM(t->kind)) n = rack_carom(t, balls);
    else if (t->kind == CUE_GAME_BARBILLIARDS) n = rack_barbilliards(t, balls);
    else if (t->kind == CUE_GAME_BILLIARDS) n = rack_billiards(t, balls);
    /* PAUL BEFORE is_snooker, because it IS a snooker table by that flag —
     * same set, same colours — and it is the one game here that does not rack.
     * Behind the flag it got a snooker rack and the scatter never ran. */
    else if (t->kind == CUE_GAME_PAUL) n = rack_paul(t, balls);
    else if (t->is_snooker)      n = rack_snooker(t, balls);
    else if (t->kind == CUE_GAME_COWBOY)  n = rack_cowboy(t, balls);
    else if (t->kind == CUE_GAME_BOWLLIARDS) n = rack_bowlliards(t, balls);
    else if (t->kind == CUE_GAME_CRIBBAGE) n = rack_cribbage(t, balls);
    else if (t->kind == CUE_GAME_FIFTEEN) n = rack_fifteen(t, balls);
    else if (CUE_GAME_IS_ROT61(t->kind)) n = rack_rotation(t, balls);
    else if (t->kind == CUE_GAME_US9)  n = rack_9ball(t, balls);
    else if (t->kind == CUE_GAME_US10) n = rack_10ball(t, balls);
    else if (CUE_GAME_IS_PYRAMID(t->kind)) n = rack_pyramid(t, balls);
    else                         n = rack_pool(t, balls);   /* UK8 + US8 + CN8 + KILLER */
    /* ONE PLACE STAMPS THE CUE BALL. Every rack builds balls[0] as the white,
     * and every game but English pool wants it the same size as the rest — so
     * the exception is applied here rather than in three racks, and it sits at
     * the height its own radius asks for. */
    cue_table_set_cue_ball(t, &balls[0]);
    /* ...and the tolerance, on THE FOUR GAMES IT WAS MEASURED ON and no others.
     *
     * A LIST AND NOT AN EXCLUSION. Written as "everything except" it would
     * quietly reach every game added afterwards, including the ones that do not
     * rack at all, and the first anybody would know is a ball off its spot. The
     * four here are the pool triangles the spread was measured on; snooker is
     * left alone (its triangle is set tight by hand against the pink, and its
     * break is not the shot this fixes), and so is everything else until it has
     * been measured too. */
    if (t->kind == CUE_GAME_UK8 || t->kind == CUE_GAME_US8 ||
        t->kind == CUE_GAME_US9 || t->kind == CUE_GAME_CN8)
        rack_tolerance(t, balls, n);
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

/* IS (x, z) ON THE BED, grown outward by `g`? The one place that knows every
 * shape a bed can be, kept on the TABLE because the cue asks this before there
 * is a world to ask.
 *
 * A regular bed is convex and centred, so a point is inside it exactly when its
 * projection onto the NEAREST face normal is within the apothem — the nearest
 * normal is the one that maximises that projection, so checking it is checking
 * all of them. The normals sit at even steps of 2*pi/n starting at +x, so the
 * nearest is found by rounding rather than by searching.
 *
 * Everything else is a union of rectangles and cue_table_bed_rects already
 * grows one correctly. */
static int bed_contains(const CueTable *t, float x, float z, float g) {
    if (t->bed_shape == CUE_BED_NGON) {
        const int n = cue_table_ngon_sides(t);
        const float ap = t->half_len * cosf(3.14159265f / (float)n) + g;
        const float r = sqrtf(x*x + z*z);
        if (r < 1e-6f) return 1;
        const float th = atan2f(z, x);
        const float step = 6.2831853f / (float)n;
        const float phi = step * floorf(th / step + 0.5f);
        return r * cosf(th - phi) <= ap;
    }
    CueRect rr[CUE_MAX_RECT];
    int nr = cue_table_bed_rects(t, g, rr, CUE_MAX_RECT);
    return cue_rects_contain(rr, nr, x, z);
}

float cue_table_surface(const CueTable *t, float x, float z) {
    /* THE BED'S OWN SHAPE, not its bounding box.
     *
     * This asked |x| <= half_len && |z| <= half_wid, which is the right question
     * for a rectangle and a lie about everything else. On a regular bed
     * half_len and half_wid are both the CIRCUMRADIUS, so the whole bounding
     * square answered "open cloth" — two thirds of a triangle table, a third of
     * a hexagon — and the cue passed straight through the cushions and the
     * timber because as far as this function was concerned there was nothing
     * there. An L had it too: its missing quadrant sits inside the bounding
     * rectangle, so the cue went through the notch.
     *
     * Reported from play on a triangle, which is simply where the hole is
     * biggest. */
    if (bed_contains(t, x, z, 0.0f)) return 0.0f;              /* open cloth */
    if (!bed_contains(t, x, z, t->rail_w)) return -1.0e9f;     /* past the frame */

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
