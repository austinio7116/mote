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
    float drop_back;            /* CORNER drop pulled this far further INTO the pocket (m) */
    float drop_back_side;       /* MIDDLE drop pulled straight back into the pocket (m, shallower) */

    /* Snooker layout (ignored for pool). */
    float baulk_x, d_radius, blue_x, pink_x, black_x;
    uint16_t cloth, rail, rail_top, spot;
    int nballs;
} CueTable;

void cue_table_init(CueTable *t, CueGameKind kind);

/* Fill a physics world with this table's constants + collision geometry. */
void cue_table_build_world(const CueTable *t, CueWorld *w);

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
