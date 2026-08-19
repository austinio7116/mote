/*
 * ThumbyCue — table dimensions, collision geometry and ball racks for both
 * 8-ball pool (7 ft) and snooker (12 ft). One dimension table feeds both the
 * physics world (cushion segments / jaws / pockets) and the renderer, so the
 * felt you see and the felt the balls bounce off are guaranteed identical.
 *
 * Coordinates: world metres, X = table length, Z = width, Y up. The playing
 * area runs x∈[−half_len,half_len], z∈[−half_wid,half_wid] to the cushion
 * NOSE. Baulk end is −X, top (foot/black) end is +X.
 */
#ifndef CUE_TABLE_H
#define CUE_TABLE_H

#include "cue_physics.h"

typedef enum {
    CUE_GAME_UK8 = 0,   /* 7ft,  curved pockets, UK 8-ball */
    CUE_GAME_US8,       /* 9ft,  angled (straight-mitre) pockets, US 8-ball */
    CUE_GAME_US9,       /* 9ft,  angled pockets, US 9-ball */
    CUE_GAME_CN8,       /* 10ft, tight rounded pockets, Chinese 8-ball (WPA rules) */
    CUE_GAME_SNK15,     /* 12ft, curved pockets, full snooker */
    CUE_GAME_SNK10,     /* 10ft, curved pockets, 10-red snooker */
    CUE_GAME_SNK6,      /* 7ft UK pool table, curved pockets, 6-red snooker */
    /* ---- VR only from here down (see CUE_GAME_VR_FIRST) ---- */
    CUE_GAME_STRAIGHT,  /* 9ft, angled pockets, straight pool (14.1 continuous) */
    /* G2: RUSSIAN PYRAMID. A 12 ft bed with 68 mm balls and pockets barely
     * wider than they are — the game the table workshop's bore, setback and
     * drop-cap numbers exist for, because on this table whether a pot is
     * possible at all is decided by a couple of millimetres. */
    CUE_GAME_PYRAMID,
    /* The same game on the size of table people actually own. The federation
     * lists four beds — 7, 8, 9 and 12 ft — and the ball shrinks with them:
     * 68 mm on the 9 and 12, 60 mm on the 7 and 8. The CLEARANCE does not
     * shrink, because that is about a ball fitting a hole and not about the
     * room it is in, so this table keeps the tournament's 5 mm at the corner
     * and 14.5 in the middle around a smaller ball. */
    CUE_GAME_PYRAMID7,
    /* G5: ENGLISH BILLIARDS. Three balls on a full-size snooker table — two
     * cue balls and a red — scored by cannons, pots and in-offs. It is the
     * oldest game in the building and the only one here whose scoring asks
     * what the cue ball touched in ORDER rather than merely what it hit
     * first, which is what the shot-contact log exists for. */
    CUE_GAME_BILLIARDS,
    /* G6: BAR BILLIARDS. The odd one out in every way: no pockets on the
     * rails at all, nine holes in the BED scoring from ten to two hundred,
     * three wooden skittles standing among them that cost you your break or
     * your whole score, play from one end only, and a clock that ends the
     * game rather than a rack that runs out. */
    CUE_GAME_BARBILLIARDS,
    /* G7: BILLIARDS GOLF. Eighteen holes on a pool table, from the Billiard &
     * Golf scoreboard: each hole is a fixed arrangement of reds and a par, you
     * count the strokes it takes to clear them, and the lowest total over the
     * round wins. The only game here that is not a frame at all — there is no
     * opponent to take the table from you, and no scoring shot: the SHOTS are
     * the score, and fewer is better. */
    CUE_GAME_GOLF,
    CUE_GAME_COUNT
} CueGameKind;
/* Both pyramid beds, wherever the game rather than the size is what matters. */
#define CUE_GAME_IS_PYRAMID(k) \
    ((k) == CUE_GAME_PYRAMID || (k) == CUE_GAME_PYRAMID7)
/* legacy coarse aliases (kept so existing call sites read cleanly) */
#define CUE_GAME_POOL    CUE_GAME_UK8
#define CUE_GAME_SNOOKER CUE_GAME_SNK15
/* first snooker variant in enum order — everything >= this is snooker */
#define CUE_GAME_FIRST_SNK CUE_GAME_SNK15

/* WHERE THE HANDHELD STOPS READING.
 *
 * The kinds are an enum, and CUE_GAME_COUNT sizes arrays that the DEVICE build
 * compiles too — cue_game.c's k_mode_name among them — while its own mode picker
 * cycles modulo that count. So a kind added for VR does not merely go unused on
 * the handheld: it appears in the handheld's menu and can be selected.
 *
 * Everything below this line plays on both. Everything at or above it is VR
 * only, and the device's picker stops here. The two are equal today because
 * nothing is VR-only yet — the point of pinning it NOW is that the first such
 * kind is appended after SNK6 and is excluded by construction, rather than
 * needing five kinds' worth of indices audited later. */
#define CUE_GAME_VR_FIRST 7
/* ...and a static check that the pin has not drifted past the end. */
typedef char cue_vr_first_in_range[(CUE_GAME_VR_FIRST <= CUE_GAME_COUNT) ? 1 : -1];

/* Ball id conventions (shared by physics, render, rules).
 * Pool:    0 = cue, 1..7 solids, 8 = black, 9..15 stripes.
 * Snooker: 0 = cue, 1..15 reds, then the six colours below. */
enum {
    CUE_ID_CUE = 0,
    CUE_ID_YELLOW = 20, CUE_ID_GREEN, CUE_ID_BROWN,
    CUE_ID_BLUE, CUE_ID_PINK, CUE_ID_BLACK,
};

/* ENGLISH BILLIARDS' THREE BALLS, in the ids the rest of the engine already
 * has. The red is a snooker red (id 1) because that is what it is and it
 * renders as one; the two cue balls are the white and the yellow.
 *
 * Index 0 of the ball array is THE CUE BALL as far as the physics and the
 * camera are concerned, and in billiards the two sides play with different
 * balls — so at a change of turn the contents of index 0 and the other white
 * are exchanged, ids and all. Index 0 therefore carries whichever of the two
 * is being struck, and it draws in that ball's own colour without anything
 * downstream needing to know that the striker changed. */
#define CUE_ID_BIL_RED    1
#define CUE_ID_BIL_WHITE  CUE_ID_CUE
#define CUE_ID_BIL_YELLOW CUE_ID_YELLOW

/* ---- BILLIARDS GOLF: the course ----------------------------------------
 *
 * Eighteen holes, each a par and an arrangement of reds. Read off the
 * Billiard & Golf scoreboard, and cross-checked three ways: the nines total
 * 38 and 35 exactly as the board prints them, and par is one more than the
 * ball count on every hole without exception.
 *
 * A layout is given in the RACK TRIANGLE'S own frame — u across, 0 at the
 * left corner and 1 at the right, v down, 0 at the apex ball and 1 at the
 * base row — so it survives any table size or ball diameter. Most balls sit
 * on one of the fifteen standard rack positions; the board also uses two
 * arrangements that do not, and only on the holes named here:
 *
 *   a COLUMN straight down from the apex, balls touching (6, 8, 12, 13, 18).
 *     Four is the most that fits: a fifth would sit past the base row.
 *   a PAIR CENTRED on the back row, straddling the centre line and so
 *     between two standard positions (hole 10).
 */
/* WHICH HOLES A ROUND COVERS. Nine is a real round of golf and half the
 * sitting, which matters more on a table — where a hole is a handful of shots
 * and a full eighteen is a long session — than it does on grass. */
enum { CUE_GOLF_18 = 0, CUE_GOLF_FRONT9, CUE_GOLF_BACK9, CUE_GOLF_ROUNDS };
#define CUE_GOLF_HOLES 18
#define CUE_GOLF_MAX_BALLS 4
#define CUE_GOLF_MAX_STROKES 8      /* Rule 3: "limit 8" */
typedef struct {
    uint8_t par;
    uint8_t n;
    float   u[CUE_GOLF_MAX_BALLS];  /* across the triangle, 0..1 */
    float   v[CUE_GOLF_MAX_BALLS];  /* apex to base row, 0..1   */
} CueGolfHole;
extern const CueGolfHole CUE_GOLF_COURSE[CUE_GOLF_HOLES];
/* Par for the front nine, the back nine and the round. */
int cue_golf_par(int from_hole, int to_hole);
/* The first and last hole of a round — one place, so nothing has to remember
 * that the back nine starts at ten. */
static inline int cue_golf_first(int round) { return round == CUE_GOLF_BACK9 ? 9 : 0; }
static inline int cue_golf_last(int round)  { return round == CUE_GOLF_FRONT9 ? 8 : 17; }
extern const char *const CUE_GOLF_ROUND_NAME[CUE_GOLF_ROUNDS];
/* Which hole the next cue_table_rack() sets out. The rack takes no argument
 * for it and every caller already goes through that one function. */
void cue_table_golf_set_hole(int hole);
int  cue_table_golf_hole(void);

/* How far the woodwork carries on past the rail cap. CueVR's frame builder
 * (its own repository) makes the
 * body's outer face rail_w + this (SURR_X/SURR_Z) and the apron runs out to
 * meet it, so it is real table that a ball can be on top of — and without it
 * the landing strip for a jumped ball is barely one ball wide. Kept here rather
 * than in the physics because it is a fact about the table, and both the
 * boundary and the frame builder should read it from the same place. */
#define CUE_FRAME_OUT 0.055f

/* THE SHAPE OF THE BED (F2).
 *
 * RECT is every table ever shipped and stays the default, so a table built by
 * anything that predates this is a rectangle without saying so.
 *
 * L is that rectangle with a bite taken out of the +x/+z corner, `notch_x` long
 * and `notch_z` deep. One number each, and it is enough: an L-shaped table is a
 * rectangle with a corner missing, and describing it as two overlapping
 * rectangles rather than as six vertices is what keeps every boundary test two
 * compares wide. */
/* And N-GON is the regular family: an equilateral bed with a pocket at every
 * corner. Triangle, square, pentagon, hexagon, heptagon, octagon — and a round
 * table is the same object with enough sides to read as a curve and a pocket
 * only every so many of them.
 *
 * A SQUARE HERE IS NOT A RECTANGLE WITH EQUAL SIDES. A rectangle carries six
 * pockets, four at the corners and two halfway down the long rails; a square
 * bed carries four, one per corner, and that is a different table to play on
 * rather than a differently-proportioned one.
 *
 * Every one of them is CONVEX, which is why this costs so much less than the L
 * did: there is no reflex corner anywhere, so none of the elbow machinery is
 * needed and every boundary test is "inside all N edges". */
enum { CUE_BED_RECT = 0, CUE_BED_L = 1, CUE_BED_NGON = 2 };
enum { CUE_HAND_RIGHT = 0, CUE_HAND_LEFT = 1 };

typedef struct {
    CueGameKind kind;
    int   is_snooker;           /* snooker ball set / rules vs pool */
    int   reds;                 /* snooker: number of reds (10 or 15) */
    /* F2: the bed's shape, and the corner an L is missing. Zero everywhere for
     * a rectangle, which is what memset in cue_table_init already gives. */
    int   bed_shape;            /* CUE_BED_* */
    float notch_x, notch_z;     /* the bite, measured in from +x and the hand's z */
    /* S2: the regular bed. `bed_sides` is how many, `bed_pocket_every` says a
     * pocket sits at every Nth corner — one for the polygons, and six-sides'-
     * worth for a round table so it gets six pockets rather than sixty. The
     * bed's size is half_len used as the CIRCUMRADIUS; half_wid follows it, so
     * everything that wants a bounding box still gets a true one. */
    int   bed_sides;
    int   bed_pocket_every;
    /* WHICH WAY THE L TURNS. An L has a handedness and only ever had one of
     * them: the bite came out of the +x/+z corner, so every L-shaped table in
     * the game was the same table. RIGHT keeps that; LEFT mirrors it in z, so
     * the short arm comes off the other side and the shot round the corner is
     * the other way about.
     *
     * A FIELD rather than a signed notch_z. The sign would have cost nothing on
     * the wire and it is exactly the kind of cleverness this shape has already
     * punished five times: every L-shaped bug so far has been a fact hidden in
     * a coordinate sign, and hiding one more there deliberately is asking for
     * the sixth. Zero is RIGHT, so a table built before this is unchanged and a
     * memset one is too. */
    int   bed_hand;             /* CUE_HAND_* */
    float half_len, half_wid;   /* to cushion nose (m) */
    float R, mass;
    /* THE CUE BALL, WHERE IT IS NOT ONE OF THE SET.
     *
     * English pool is played with a cue ball smaller and lighter than the
     * object balls — 47.6 mm and 94 g against 50.8 mm and 116 g — because
     * coin-op tables have to separate it to return it. Zero means "the same as
     * the rest", which is every other game here. */
    float cue_R, cue_mass;
    float cushion_h;            /* nose height above cloth (m) */
    float rail_w;               /* rail/frame width, render only (m) */

    /* Pocket-jaw model (faithful to the 2D game's geometry). Each cushion is a
     * 4-point chain: facing-tip → knuckle → knuckle → facing-tip, the facings
     * splaying OUTWARD (away from the playing area) at the pocket angle. The
     * pocket itself is a circle of radius pr_* centred just outside the rail.
     *   pocket_round = 0 → US pool (45° corner / 70° side facings)
     *   pocket_round = 1 → snooker/UK (tighter, more rounded). */
    int   pocket_round;
    float pr_corner, pr_side;   /* pocket hole radius (m) */
    /* THE HOLE CUT IN THE TIMBER, which is not the same measurement as the
     * mouth the ball goes through even though it started life equal to it. The
     * rail's inner face only exists where the timber does, so where the bore is
     * too small to reach the end of the cushion there is a slot between the two
     * that you can see out of the table through — reported at the middle
     * pockets of the 7 ft table and behind the mitred jaws of the American and
     * Chinese ones. Dial it with tools/pocketbench. Defaults to pr_*, which is
     * exactly what the code did when the radius was not a field. */
    float bore_corner, bore_side;
    /* And how far OUT from the pocket centre that hole is cut, along the
     * pocket's own outward normal. Positive pushes the timber's hole away from
     * the cloth, which is the other way of closing the same slot: shrink the
     * hole, or set it back. Zero is concentric, which is what the code did
     * before either was a field. */
    float bore_set_corner, bore_set_side;
    float gap_corner, gap_side; /* knuckle setback from corner / from centre (m) */
    float facing_len;           /* facing length (m) */
    float ang_corner, ang_side; /* facing splay from the rail line (deg) */
    float off_corner, off_side; /* pocket-centre offset beyond the boundary (m) */
    float jaw_r;                /* small knuckle rounding radius (m) */
    /* The rail: restitution at a crawl, how fast it falls with pace, and the
     * floor. See cue_table_rails for where the numbers come from. */
    float e_cush, cush_efall, e_cush_min;
    /* HOW MUCH SMALLER THE DROP CIRCLE IS THAN THE HOLE, per pocket type (m).
     * Taken off pr to get the radius a ball's centre must be inside before it
     * is down. It was a literal in build_world — 0.3 R, and 0.15 R for a UK
     * middle — which meant it could not be set per table at all. */
    float cap_corner, cap_side;
    /* ...and HOW MUCH DEEPER THAN THE POCKET that circle is centred (m). Zero
     * is concentric with the hole. The two together are the whole of what a
     * pocket takes: how big the catch is, and how far in it sits. */
    float drop_back;            /* CORNER drop pushed this far deeper (m) */
    float drop_back_side;       /* MIDDLE drop pushed this far deeper (m) */

    /* THE BAULK LINES, which only bar billiards has.
 *
 * AEBBA Rule 77: they are "drawn on the table RADIATING from the centre of the
 * base of the playing area TO THE SIDE CUSHIONS so as to form an arc of not
 * less than 150 degrees and not more than 160". Two straight lines out of the
 * break spot, then, with that angle BETWEEN them — a shallow V a little short
 * of square across the table — and the baulk is the wedge behind them.
 *
 * Read first as a circle about the break spot, which put the whole near third
 * of the table in baulk and the RED SPOT with it: the red is placed 175 mm up
 * the table and an arc that reaches the side cushions is 405 mm across, so
 * every stroke fouled under Rule 110(c) before it was played. Two lines at
 * 155 degrees leave it comfortably clear, which is the point of them.
 *
 * Held as the angle between the two lines, in degrees. Zero on every other
 * table, which has no such thing. */
    float baulk_arc;

/* Snooker layout (ignored for pool). */
    float baulk_x, d_radius, blue_x, pink_x, black_x;
    /* WHAT SHAPE THE REGION BEHIND THE BAULK LINE IS.
     *
     * 0: a D — the half-disc of radius d_radius that snooker and UK 8-ball
     *    play from, and the shape everything here assumed there was only one
     *    of. 1: a HOUSE — Russian pyramid's дом, which is the WHOLE width of
     *    the table behind the line and not an arc at all.
     *
     * Pyramid arrived carrying its house in baulk_x + d_radius on the grounds
     * that a house is a D as far as a clamp is concerned. It is not: it left a
     * semicircle chalked on a Russian table and confined the cue ball to it,
     * which is a third of the area the rules give you. d_radius still means
     * something under a house — it is where yellow and green would sit, and it
     * is the only sensible width for the AI to spread its break-off over. */
    int house;
    uint16_t cloth, rail, rail_top, spot;
    int nballs;
} CueTable;

/* +1 for a right-handed L and -1 for a left-handed one: the factor every piece
 * of shape arithmetic multiplies its z by. ONE place says what the hand means
 * and every builder reads it from here, rather than each deciding for itself —
 * which is how a shape fact ends up written six different ways. */
/* THE L'S OUTLINE, in the order the cloth boundary walks it, for EITHER hand.
 *
 * There is one shape here and it turns two ways, and for a long time that cost
 * six separate mirrorings — the world reversed segment and pocket arrays and
 * recomputed every normal, the renderer's boundary walk undid that with an
 * index macro and redid it with a coordinate one, and the frame planks did
 * their own. Six places multiplying z by the hand, each an opportunity to get
 * it wrong, and one of them was.
 *
 * A left-handed L is not a right-handed one drawn backwards: mirroring z
 * reverses the winding, which moves the inside corner from the fourth vertex
 * to the third. So the vertices are laid out for the hand asked for, once, and
 * everything downstream reads them and needs to know nothing about hands at
 * all. Six vertices, always counter-clockwise so an edge's outward normal
 * falls out of its own direction; `reflex` is the inside corner.
 *
 * Returns 6, or 0 if this is not an L. */
int cue_table_L_outline(const CueTable *t, Vec3 *v6, int *reflex);

static inline float cue_table_hand(const CueTable *t) {
    return (t && t->bed_hand == CUE_HAND_LEFT) ? -1.0f : 1.0f;
}

void cue_table_init(CueTable *t, CueGameKind kind);

/* ---- THE TABLE AS A VALUE ------------------------------------------------ *
 *
 * A CueTable has always been a fine parameter block — it is why the pocket bench
 * can dial a bore. What it has never been is ADDRESSABLE: every table in the
 * game arrives through cue_table_init() with an enum, and the numbers are
 * literals inside a switch. Nothing can save one, send one, or say whether one
 * is playable.
 *
 * Four operations fix that, and they all read ONE description of the fields
 * (cue_table_fields in the .c). That is deliberate. cuevr_net.h records what
 * happens when a struct crosses a boundary field by hand-picked field: "picking
 * fields by hand means picking wrongly at some point, and the failure is
 * invisible until the frame stops making sense." A field added to the struct and
 * forgotten in the packer is exactly that bug. Here there is one list, and
 * adding a row to it teaches all four at once. */

#define CUE_TABLE_SPEC_VERSION 1
#define CUE_TABLE_SPEC_MAX 256      /* a packed table is comfortably under this */

/* Pack into `out`, returning the bytes written, or 0 if it would not fit.
 * Version-stamped and explicitly little-endian, so a spec written by one build
 * is readable by another. */
int cue_table_pack(const CueTable *t, unsigned char *out, int cap);

/* And back. Returns 1 on success, 0 on a bad length, an unknown version, or a
 * block that does not validate. A spec that fails here must not be played:
 * unpacking is the boundary, so it is where refusal belongs. */
int cue_table_unpack(CueTable *t, const unsigned char *in, int len);

/* A stable 32-bit hash of the table's PLAYING numbers, for the room handshake:
 * hash in the hello, the spec on request, refuse if the two cannot agree.
 *
 * COSMETICS ARE NOT IN IT, and that is the whole point of having a hash rather
 * than comparing structs. Cloth colour, rail colour and the spot colour change
 * nothing about where a ball goes, so two players whose tables differ only in
 * how they look must still be able to play each other. Anything that moves a
 * cushion, a pocket or a ball is in. */
uint32_t cue_table_hash(const CueTable *t);

/* Is this a table anybody can play on? Returns 1 if so. Otherwise 0, and `msg`
 * (if given) receives a one-line reason in the player's terms — "the pocket is
 * narrower than the ball", not "pr_corner out of range".
 *
 * This is the product, not paperwork. Every number below is reachable from the
 * table workshop, and a pocket dialled narrower than a ball is expressible in
 * one thumbstick movement. Per-field ranges catch the wild values; the
 * cross-field checks after them catch the combinations that are individually
 * sensible and together unplayable. */
int cue_table_validate(const CueTable *t, char *msg, int msgcap);

/* How many fields the description carries, and the name of one — so a tuning
 * screen can walk the table without a second list of its own. */
int         cue_table_field_count(void);
const char *cue_table_field_name(int i);

/* Fill a physics world with this table's constants + collision geometry. */
void cue_table_build_world(const CueTable *t, CueWorld *w);

/* ---- THE POCKET CUT ------------------------------------------------------- *
 *
 * The shape the cloth is cut away in around each pocket: an arc around the
 * pocket with a straight tangent leg out to each slate edge — a quarter of a
 * circle at a corner, a half at a middle, which is what a slate cutter leaves.
 * The radii are multiples of the ball's drop circle; the setbacks push the arc
 * centre away from the table, into the frame.
 *
 * ONE definition, because the edge of the cut is where the ball tips over. If
 * the renderer and the physics each kept their own the ball would fall through
 * cloth at one pocket and hang in mid air at another, which is exactly what
 * they did while they did. */
/* FOUR NUMBERS PER POCKET TYPE PER TABLE, and nothing else decides the shape:
 *
 *   set   how far the arc's centre sits back from the pocket, along the line
 *         out through the mouth. Metres. This is what slides the whole cut in
 *         and out, so it is what puts the cloth edge on the drop line.
 *   rad   the arc's radius, as a multiple of the pocket's drop radius. This is
 *         how WIDE the opening is.
 *   roll  the lip: how far the cloth rolls over the edge before it turns
 *         vertical, same units. Its thickness.
 *   arc   how much of the cut is arc rather than straight leg, in degrees. 90
 *         at a corner and 180 at a middle is the shape a slate cutter leaves;
 *         less arc means longer straight runs into the same opening.
 *
 * Corner and middle are separate, and each table size has its own pair. UK8
 * shares with 6-red (same bed) and US8 with 9-ball (same bed). */
typedef struct { float set, rad, roll, arc; } CueCut;

void cue_table_default_cut(CueGameKind kind, int middle, CueCut *out);
void cue_table_set_cut(CueWorld *w, int middle, CueCut c);
void cue_table_get_cut(const CueWorld *w, int middle, CueCut *out);

void cue_table_derive_cut(CueWorld *w);

/* Lay out the opening rack / spots. Returns the number of balls placed.
 * balls[0] is always the cue ball. orient set to identity. */
int cue_table_rack(const CueTable *t, CueBall *balls);
/* Give this ball the cue ball's own size and weight for this table (a no-op
 * where the set is matched). Racking does it; anything that re-creates the
 * white — a respot, ball in hand — should too. */
void cue_table_set_cue_ball(const CueTable *t, CueBall *cue);

/* Ball-in-hand placement. The _balls form keeps the old behaviour (the D on
 * snooker and the English table, the whole cloth elsewhere); _any takes the
 * region explicitly; the caller asks the RULES which it
 * is — see cue_rules_in_hand_anywhere — because the English table places in the
 * D under pub rules and anywhere under International and Ultimate Pool. */
Vec3 cue_table_clamp_placement_any(const CueTable *t, Vec3 p,
                                   const CueBall *balls, int n, int breaking,
                                   int anywhere);

/* Cue-ball home (centre of the D / behind the head string) for placement. */
Vec3 cue_table_cue_home(const CueTable *t);

/* THE BED AS RECTANGLES (F2). Writes the union that describes this table's
 * cloth — one rectangle for a plain table, two for an L — and returns how many.
 *
 * `grow` widens every OUTSIDE edge by that much, which is how one description
 * serves both the cloth and the frame edge a jumped ball is deleted at. It is
 * not a per-rectangle grow: the two rectangles of an L share an internal edge,
 * and growing that would push phantom cloth out into the notch. The
 * decomposition here is chosen so that growing each piece is right — a full
 * width band below the notch, and a full height column beside it. */
/* The i-th corner of a regular bed, in table space. Vertex 0 is placed so that
 * an EDGE is centred on -x: the baulk end of every one of these is a flat
 * cushion to lay a D against, rather than a pocket in the middle of it. */
/* The half-length along the table's SPINE, and across it: half_len/half_wid on
 * a rectangle and an L, and the APOTHEM on a regular bed, where half_len is the
 * circumradius and reaches the corners — which is where the pockets are, and so
 * the one distance no marking may be measured against. Every layout fraction on
 * a table goes through these. */
float cue_table_axis(const CueTable *t);
float cue_table_across(const CueTable *t);

Vec3 cue_table_ngon_vert(const CueTable *t, int i);
/* How many corners it actually has, clamped to what the world can hold. */
int  cue_table_ngon_sides(const CueTable *t);

int cue_table_bed_rects(const CueTable *t, float grow, CueRect *out, int cap);

/* THE SAME BED, CUT INTO PIECES THAT DO NOT OVERLAP. One for a rectangle, two
 * for an L, swept along z so that between two levels the run in x is constant.
 *
 * cue_table_bed_rects OVERLAPS on purpose: a point inside either piece is on the
 * cloth, and the overlap costs a containment test nothing. Anything that DIVIDES
 * the bed up rather than testing against it needs the pieces not to double —
 * hanging a lamp over each run, flooring the underside, counting area. Sharing
 * the overlapping version for that puts two lamps in the same place and two
 * coplanar faces where one belongs. */
int cue_table_bed_strips(const CueTable *t, CueRect *out, int cap);

/* Is this point on the cloth? The shape-aware form of "inside the half-extents",
 * and the question the placement clamp and the AI's sampler both need. */
int cue_table_on_bed(const CueTable *t, float x, float z);

/* ---- THE SPINE: the line a table is laid out ALONG -----------------------
 *
 * Where a game is SET OUT — the baulk line, the D, the six snooker spots, the
 * foot spot, the rack, the head string — used to be an x coordinate on the
 * centre line, with the rack growing in +x. That is the table's long axis
 * written into every one of them, and on an L it is nonsense: the long axis runs
 * down one arm, through the notch, and out into the corner that is not there.
 *
 * The spine is that line made shape-aware. On a rectangle it is the straight
 * centre line and NOTHING CHANGES — a position given as an x coordinate comes
 * back as that coordinate, untouched. On an L it turns the corner: the baulk end
 * and the D are at the free end of one arm, the rack is at the free end of the
 * other, ninety degrees round, and the middle of the spine — where a snooker
 * table puts its blue — is the middle of the bend.
 *
 * That is what makes every game possible on any L. It also makes the break a
 * shot round a corner, which is the entire point of a table this shape.
 *
 *   `x`      the position as a rectangle would express it, which is what the
 *            table already stores and what every ruleset already means
 *   `across` the offset to the side of the spine; +across is ninety degrees to
 *            the LEFT of the direction of travel, which on a rectangle is +z
 *   `dir`    out: which way is "up the table" there, so a rack can be laid out
 *            in the local frame rather than in world x */
Vec3  cue_table_lay(const CueTable *t, float x, float across, Vec3 *dir);
float cue_table_spine_len(const CueTable *t);

/* WHERE THE PACK GOES, and which way it grows from there. On a rectangle that is
 * a quarter of the way down the centre line, growing toward the top cushion; on
 * an L it is a quarter of the way along the spine FROM THE FAR END, which puts
 * it on the arm the baulk is not on.
 *
 * A custom table will be able to override this outright; this is what it does
 * when nobody has said otherwise. */
Vec3 cue_table_foot_spot_dir(const CueTable *t, Vec3 *dir);
Vec3 cue_table_foot_spot(const CueTable *t);

/* CAN THIS GAME BE RACKED ON THIS TABLE?
 *
 * A custom bed can be any size and any of the two shapes, and most games do not
 * care — pool wants a foot spot and a head string and both move with the table.
 * Snooker does care, and it cares a lot: six colours on six named spots and a D
 * struck from the baulk line, none of which is a proportion of the bed. On an
 * L-shaped bed the pink and the black land in the missing corner, and the game
 * cannot be racked at all — so it is refused rather than racked into thin air.
 *
 * `laid_out` says the caller has a hand-placed position for every ball, which is
 * the one thing that makes any game playable on any shape: the spots are only
 * needed because nobody said where the balls go. Pass 1 and this only checks
 * that the cue ball has somewhere legal to be.
 *
 * Returns 1 if it can, 0 with a reason in `msg` if it cannot. */
int cue_table_game_ok(const CueTable *t, CueGameKind kind, int laid_out,
                      char *msg, int msgcap);

/* A six-ball triangle on the foot spot plus the white at home, for the
 * practice challenges. Returns the ball count (7). */
int cue_table_rack_six(const CueTable *t, CueBall *balls);

/* THE STRAIGHT-POOL RERACK: put every off-table object ball back as a triangle
 * with the apex space EMPTY, leaving whatever is still on the table — the break
 * ball and the cue ball — exactly where it lies. Returns how many were placed
 * (14 in the normal case). Does nothing to a table where nothing is potted. */
int cue_table_rack_14(const CueTable *t, CueBall *balls, int n);

/* Clamp a desired placement to the legal ball-in-hand region (the D for
 * snooker/UK8, behind the head string for US pool). */
Vec3 cue_table_clamp_placement(const CueTable *t, Vec3 p);
/* The same, but also pushed clear of every ball already on the table. Use
 * this wherever the live balls are to hand: region-only clamping lets the
 * player park the cue ball inside another one, and the solver then fires the
 * pair apart on the first tick of the shot. */
/* `breaking` = this is the placement for the OPENING BREAK, which is behind
 * the head string on a US-style table. Ball in hand after a foul is anywhere on
 * those tables; snooker and UK 8-ball use the D either way. */
/* PUT ONE BALL BACK ON THE TABLE, at the foot spot or as near behind it as is
 * clear. Russian pyramid's foul penalty returns one of the offender's own potted
 * balls, and the rules cannot do it themselves — they hold no table, which is
 * the same reason `rerack` and `ball_in_hand` are flags a host consumes.
 *
 * Returns the id it put back, or 0 if the striker had nothing to give back or
 * there was nowhere to put it. Takes the ball with the LOWEST id that is off, so
 * two hosts replaying the same frame put back the same ball — this crosses the
 * wire as a state packet, and "whichever one the loop happened to reach first"
 * is not a rule. */
int cue_table_respot_one(const CueTable *t, CueBall *b, int n);

Vec3 cue_table_clamp_placement_balls(const CueTable *t, Vec3 p,
                                     const CueBall *balls, int n, int breaking);

/* Minimum cue elevation (radians, butt raised) at which the CUE — a tapered
 * solid running back from `tip` along `aim` for its full length — clears the
 * bed, the rail band, the frame and every ball. `tip` is the 3-D position of
 * the tip ITSELF, not the cue ball: this asks where the stick is, not what the
 * shot would need, which is the only version that means anything when the cue
 * is being carried around a room. balls[0] is the cue ball and never blocks.
 *
 * Front ends play at fmaxf(their own elevation, this). The AI's ranking sims
 * must use it too — an elevated cue delivers cos(elev) of its pace and, with
 * side on, swerves, so a planner assuming a level cue picks shots that cannot
 * be played. At address, pass the contact point on the white. */
/* HOW HIGH THE TABLE IS AT A POINT, as the CUE sees it: cloth at zero, the
 * cushion top and rail cap above that, the six bores through the timber and
 * the cloth cut at the mouths counting as no surface at all, and anything past
 * the frame likewise. CUE_TABLE_NO_SURFACE means "nothing here".
 *
 * Exported because the host asks it too: a stroke may not begin with the tip
 * buried in the table (C2a), and "is the tip inside something" is this
 * question with the tip's own height compared against the answer. */
#define CUE_TABLE_NO_SURFACE (-1.0e9f)
float cue_table_surface(const CueTable *t, float x, float z);

float cue_table_min_elev(const CueTable *t, const CueBall *balls, int n,
                         Vec3 tip, float aim);

#endif
