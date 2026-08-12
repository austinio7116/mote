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
    CUE_GAME_COUNT
} CueGameKind;
/* legacy coarse aliases (kept so existing call sites read cleanly) */
#define CUE_GAME_POOL    CUE_GAME_UK8
#define CUE_GAME_SNOOKER CUE_GAME_SNK15
/* first snooker variant in enum order — everything >= this is snooker */
#define CUE_GAME_FIRST_SNK CUE_GAME_SNK15

/* Ball id conventions (shared by physics, render, rules).
 * Pool:    0 = cue, 1..7 solids, 8 = black, 9..15 stripes.
 * Snooker: 0 = cue, 1..15 reds, then the six colours below. */
enum {
    CUE_ID_CUE = 0,
    CUE_ID_YELLOW = 20, CUE_ID_GREEN, CUE_ID_BROWN,
    CUE_ID_BLUE, CUE_ID_PINK, CUE_ID_BLACK,
};

/* How far the woodwork carries on past the rail cap. CueVR's frame builder
 * (its own repository) makes the
 * body's outer face rail_w + this (SURR_X/SURR_Z) and the apron runs out to
 * meet it, so it is real table that a ball can be on top of — and without it
 * the landing strip for a jumped ball is barely one ball wide. Kept here rather
 * than in the physics because it is a fact about the table, and both the
 * boundary and the frame builder should read it from the same place. */
#define CUE_FRAME_OUT 0.055f

typedef struct {
    CueGameKind kind;
    int   is_snooker;           /* snooker ball set / rules vs pool */
    int   reds;                 /* snooker: number of reds (10 or 15) */
    float half_len, half_wid;   /* to cushion nose (m) */
    float R, mass;
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
    float gap_corner, gap_side; /* knuckle setback from corner / from centre (m) */
    float facing_len;           /* facing length (m) */
    float ang_corner, ang_side; /* facing splay from the rail line (deg) */
    float off_corner, off_side; /* pocket-centre offset beyond the boundary (m) */
    float jaw_r;                /* small knuckle rounding radius (m) */
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

    /* Snooker layout (ignored for pool). */
    float baulk_x, d_radius, blue_x, pink_x, black_x;
    uint16_t cloth, rail, rail_top, spot;
    int nballs;
} CueTable;

void cue_table_init(CueTable *t, CueGameKind kind);

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

/* Cue-ball home (centre of the D / behind the head string) for placement. */
Vec3 cue_table_cue_home(const CueTable *t);

/* A six-ball triangle on the foot spot plus the white at home, for the
 * practice challenges. Returns the ball count (7). */
int cue_table_rack_six(const CueTable *t, CueBall *balls);

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
float cue_table_min_elev(const CueTable *t, const CueBall *balls, int n,
                         Vec3 tip, float aim);

#endif
