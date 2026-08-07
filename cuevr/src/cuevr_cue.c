/*
 * CueVR — two controllers, one cue.
 *
 * The whole of "natural" cueing is that nothing is assisted. The cue is the
 * line between your two hands: the left is the bridge it rests on, the right is
 * the butt. Raise your back hand and the cue elevates. Move the bridge closer
 * to the ball and you shorten up. Aim by pointing the line, put side on by
 * moving the line off the ball's centre, and play the shot by pushing the cue
 * through — the power is the speed the tip is doing when it arrives, exactly as
 * on a real table. There is no aim line, no power bar and no snapping.
 *
 * Which means all of it is geometry, and geometry is testable: see
 * test_cue.c, which plays scripted strokes and asserts the shot that comes out.
 *
 * The output is the argument list cue_phys_strike_elev already takes, because
 * the physics was written for a real cue before any of this existed.
 */
#include "cuevr.h"
#include "cue_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_lefty;
void cuevr_cue_left_handed(int on) { s_lefty = on ? 1 : 0; }

void cuevr_cue_init(CueVrCue *c) {
    memset(c, 0, sizeof *c);
    /* A snooker player grips maybe 20 cm up from the butt end. With the bridge
     * hand the usual 85-95 cm in front of that, the tip lands ~30 cm past the
     * bridge — which is where it should be. */
    c->grip = 0.20f;
    c->rest = mv3(0.0f, CUEVR_REST_LIFT_DEFAULT, 0.0f); /* on top of the hand, not through it */
}

/* ---- preferences -------------------------------------------------------- *
 * A plain text file of KEY VALUE lines. Being text means it can be read, edited
 * and diffed when something looks wrong, which on a headset — where there is no
 * shell and no file browser — is most of how a preferences bug gets diagnosed. */
/* cuevr_cue.c is otherwise pure maths and is linked into the unit tests, which
 * have no logger. Preferences are the one part that touches the filesystem, and
 * a silent failure there is exactly what "it never saved anything" looks like. */
#if defined(__ANDROID__)
#include <android/log.h>
#define CUEVR_PREFS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#include <stdio.h>
#define CUEVR_PREFS_LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

static char s_prefs_path[512];

void cuevr_prefs_dir(const char *dir) {
    /* A relative path here is not a harmless fallback: an Android process starts
     * with "/" as its working directory and that is read-only, so every save
     * would fail and nothing would ever persist — silently, because nobody
     * checks the return of a preferences write. If there is no usable directory
     * we say so once, plainly, instead of pretending. */
    if (dir && dir[0]) {
        snprintf(s_prefs_path, sizeof s_prefs_path, "%s/cuevr.cfg", dir);
    } else {
        snprintf(s_prefs_path, sizeof s_prefs_path, "cuevr.cfg");
        CUEVR_PREFS_LOG("[cuevr] no data directory — preferences will not persist");
    }
    CUEVR_PREFS_LOG("[cuevr] preferences: %s", s_prefs_path);
}

/* Loading and saving.
 *
 * KEY VALUE, one per line, because the positional format this replaces had got
 * to "eleven fields and it stays eleven, slot 3 is a dead field nobody can
 * remove" — and the next option after that is the one that shifts everything
 * and quietly loads a lighting mode into a cue index. Named fields can be added,
 * reordered and retired, and an unknown key is skipped rather than fatal.
 *
 * Files written by the old build are still read: they have no key on the first
 * line, which is unambiguous, so they are parsed positionally and rewritten in
 * the new form the first time anything changes. Losing a table height that
 * somebody matched to their real kitchen table would be a poor way to ship a
 * file format.
 */
/* Which record a game belongs to. Kept here beside the file format so a new
 * table cannot be added to the menu and silently share another one's record. */
int cuevr_stat_snk_slot(int kind) {
    switch (kind) {
    case CUE_GAME_SNK6:  return 0;
    case CUE_GAME_SNK10: return 1;
    case CUE_GAME_SNK15: return 2;
    default:             return -1;
    }
}
int cuevr_stat_pool_slot(int kind) {
    switch (kind) {
    case CUE_GAME_UK8: return 0;
    case CUE_GAME_US8: return 1;
    case CUE_GAME_US9: return 2;
    case CUE_GAME_CN8: return 3;
    default:           return -1;
    }
}
const char *cuevr_stat_snk_name(int slot) {
    static const char *N[CUEVR_STAT_SNK] = { "SNOOKER 6-RED", "SNOOKER 10-RED", "SNOOKER 12FT" };
    return (slot >= 0 && slot < CUEVR_STAT_SNK) ? N[slot] : "?";
}
const char *cuevr_stat_pool_name(int slot) {
    static const char *N[CUEVR_STAT_POOL] = { "UK 8-BALL", "US 8-BALL", "9-BALL", "CHINESE 8" };
    return (slot >= 0 && slot < CUEVR_STAT_POOL) ? N[slot] : "?";
}

void cuevr_prefs_defaults(CueVrPrefs *p) {
    memset(p, 0, sizeof *p);
    p->table_height = 0.85f;
    p->rest  = mv3(0.0f, CUEVR_REST_LIFT_DEFAULT, 0.0f);
    p->grip  = 0.20f;
    p->opp   = 1;               /* vs CPU */
    p->body  = -1;              /* whichever suits the table */
    p->cue_spots = 1;           /* the white shows what it is doing */
    /* Measured on a Quest 3 against the real controller in a real hand, which
     * is the only way this number can be arrived at. Zero was never right: the
     * baked fallback model is somebody else's measurement of somebody else's
     * hardware, and it sat nose-down by about this much. A default that is
     * right for the hardware everyone is holding beats a default that is
     * merely tidy. */
    p->ctrl_rot[0] = -50.0f;
}

/* Every field is range-checked on the way in: a corrupt or hand-edited file
 * must not put the table through the ceiling or the cue inside your hand. */
static void prefs_put(CueVrPrefs *p, const char *k, double v) {
    int i = (int)v;
    if      (!strcmp(k, "height"))  { if (v > 0.25 && v < 1.4) p->table_height = (float)v; }
    else if (!strcmp(k, "rest_x"))  p->rest.x = (float)v;
    else if (!strcmp(k, "rest_y"))  p->rest.y = (float)v;
    else if (!strcmp(k, "rest_z"))  p->rest.z = (float)v;
    else if (!strcmp(k, "grip"))    { if (v >= CUEVR_GRIP_MIN && v <= CUEVR_GRIP_MAX) p->grip = (float)v; }
    else if (!strcmp(k, "table"))   { if (i >= 0 && i < CUE_GAME_COUNT) p->table_kind = i; }
    else if (!strcmp(k, "balls"))   { if (i >= 0 && i < 8)  p->ballset = i; }
    else if (!strcmp(k, "persona")) { if (i >= 0 && i < 32) p->persona = i; }
    else if (!strcmp(k, "cloth"))   { if (i >= 0 && i < CUE_NCLOTH) p->cloth = i; }
    else if (!strcmp(k, "frame"))   { if (i >= 0 && i < CUE_NFRAME) p->frame = i; }
    else if (!strcmp(k, "opp"))     { if (i >= 0 && i < 3)  p->opp = i; }
    else if (!strcmp(k, "cue"))     { if (i >= 0 && i < 32) p->cue = i; }
    else if (!strcmp(k, "light"))   { if (i >= 0 && i < 16) p->light = i; }
    else if (!strcmp(k, "body"))    { if (i >= -1 && i < 16) p->body = i; }
    /* Bounded so a hand-edited file cannot put the controller across the room:
     * a quarter of a metre is already far more than any real mismatch. */
    else if (!strcmp(k, "cpx"))     { if (v > -0.25 && v < 0.25) p->ctrl_pos[0] = (float)v; }
    else if (!strcmp(k, "cpy"))     { if (v > -0.25 && v < 0.25) p->ctrl_pos[1] = (float)v; }
    else if (!strcmp(k, "cpz"))     { if (v > -0.25 && v < 0.25) p->ctrl_pos[2] = (float)v; }
    else if (!strcmp(k, "crx"))     { if (v > -180.0 && v <= 180.0) p->ctrl_rot[0] = (float)v; }
    else if (!strcmp(k, "cry"))     { if (v > -180.0 && v <= 180.0) p->ctrl_rot[1] = (float)v; }
    else if (!strcmp(k, "crz"))     { if (v > -180.0 && v <= 180.0) p->ctrl_rot[2] = (float)v; }
    /* Records. Keys are "sb<table><mode>" and "pc<game><mode>", built the same
     * way on the way in and on the way out so a slot cannot be written under
     * one name and read under another. Bounded because a hand-edited 147000
     * would sit at the top of the table for ever. */
    else if (k[0] == 's' && k[1] == 'b' && k[2] && k[3] && !k[4]) {
        int a = k[2] - '0', b = k[3] - '0';
        if (a >= 0 && a < CUEVR_STAT_SNK && (b == 0 || b == 1) && v >= 0 && v <= 200)
            p->snk_best[a][b] = i;
    }
    else if (k[0] == 'p' && k[1] == 'c' && k[2] && k[3] && !k[4]) {
        int a = k[2] - '0', b = k[3] - '0';
        if (a >= 0 && a < CUEVR_STAT_POOL && (b == 0 || b == 1) && v >= 0 && v <= 1000000)
            p->pool_clear[a][b] = i;
    }
    else if (!strcmp(k, "fw0")) { if (v >= 0) p->frames_won[0] = i; }
    else if (!strcmp(k, "fw1")) { if (v >= 0) p->frames_won[1] = i; }
    else if (!strcmp(k, "fp0")) { if (v >= 0) p->frames_played[0] = i; }
    else if (!strcmp(k, "fp1")) { if (v >= 0) p->frames_played[1] = i; }
    else if (!strcmp(k, "lefty")) p->lefty = i ? 1 : 0;
    else if (!strcmp(k, "spots"))   p->cue_spots  = i ? 1 : 0;
    else if (!strcmp(k, "swapstk")) p->stick_swap = i ? 1 : 0;
    else if (!strcmp(k, "invsld"))  p->inv_slide  = i ? 1 : 0;
    else if (!strcmp(k, "invturn")) p->inv_turn   = i ? 1 : 0;
    /* anything else: a field from a newer build, or a typo. Skip it. */
}

/* The rest offset is checked as a whole, not per-axis: it is a length. */
static void prefs_fix_rest(CueVrPrefs *p) {
    /* STRICTLY greater was the whole problem. The check rejected an offset
     * LONGER than the leash, and the leash length is exactly what a saved
     * offset settles at when something has gone wrong — a clamp writes the
     * limit itself, not a value past it. So the one number this guard existed
     * to catch was the one number it let through, and a cue stranded at the
     * limit came back stranded every launch, surviving reinstalls because
     * preferences live in internal storage. Reject anything AT the limit too,
     * with a little margin for the rounding a %.4f round trip introduces. */
    float lim = CUEVR_REST_MAXLEN * 0.98f;
    float l2 = p->rest.x*p->rest.x + p->rest.y*p->rest.y + p->rest.z*p->rest.z;
    if (l2 >= lim * lim)
        p->rest = mv3(0.0f, CUEVR_REST_LIFT_DEFAULT, 0.0f);
}

void cuevr_prefs_load(CueVrPrefs *p) {
    if (!s_prefs_path[0]) cuevr_prefs_dir(NULL);
    FILE *f = fopen(s_prefs_path, "r");
    if (!f) return;

    char line[128];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }

    /* Legacy: the first character of the first line is a number. */
    if (line[0] == '-' || line[0] == '.' || (line[0] >= '0' && line[0] <= '9')) {
        float a = 0, rx = 0, ry = 0, rz = 0, d = 0;
        int k = 0, bs = 0, ps = 0, cl = 0, fr = 0, op = 1, cu = 0;
        int got = sscanf(line, "%f %f %f %f %f %d %d %d %d %d %d %d",
                         &a, &rx, &ry, &rz, &d, &k, &bs, &ps, &cl, &fr, &op, &cu);
        if (got >= 8) {
            prefs_put(p, "height", a);
            p->rest = mv3(rx, ry, rz);
            prefs_put(p, "grip", d);
            prefs_put(p, "table", k);
            prefs_put(p, "balls", bs);
            prefs_put(p, "persona", ps);
            if (got >= 11) {
                prefs_put(p, "cloth", cl);
                prefs_put(p, "frame", fr);
                prefs_put(p, "opp", op);
            }
            if (got >= 12) prefs_put(p, "cue", cu);
        }
        prefs_fix_rest(p);
        fclose(f);
        CUEVR_PREFS_LOG("[cuevr] preferences read in the old format; will rewrite");
        return;
    }

    do {
        char key[32];
        double val;
        if (sscanf(line, "%31s %lf", key, &val) == 2) prefs_put(p, key, val);
    } while (fgets(line, sizeof line, f));
    prefs_fix_rest(p);
    fclose(f);
}

void cuevr_prefs_save(const CueVrPrefs *p) {
    if (!s_prefs_path[0]) cuevr_prefs_dir(NULL);
    FILE *f = fopen(s_prefs_path, "w");
    if (!f) { CUEVR_PREFS_LOG("[cuevr] cannot write %s", s_prefs_path); return; }
    fprintf(f,
            "height %.4f\nrest_x %.4f\nrest_y %.4f\nrest_z %.4f\ngrip %.4f\n"
            "table %d\nballs %d\npersona %d\ncloth %d\nframe %d\nopp %d\n"
            "cue %d\nlight %d\nbody %d\n"
            "cpx %.4f\ncpy %.4f\ncpz %.4f\ncrx %.2f\ncry %.2f\ncrz %.2f\n",
            (double)p->table_height, (double)p->rest.x, (double)p->rest.y,
            (double)p->rest.z, (double)p->grip,
            p->table_kind, p->ballset, p->persona, p->cloth, p->frame, p->opp,
            p->cue, p->light, p->body,
            (double)p->ctrl_pos[0], (double)p->ctrl_pos[1], (double)p->ctrl_pos[2],
            (double)p->ctrl_rot[0], (double)p->ctrl_rot[1], (double)p->ctrl_rot[2]);
    for (int a = 0; a < CUEVR_STAT_SNK; a++)
        for (int b = 0; b < 2; b++)
            fprintf(f, "sb%d%d %d\n", a, b, p->snk_best[a][b]);
    for (int a = 0; a < CUEVR_STAT_POOL; a++)
        for (int b = 0; b < 2; b++)
            fprintf(f, "pc%d%d %d\n", a, b, p->pool_clear[a][b]);
    fprintf(f, "fw0 %d\nfw1 %d\nfp0 %d\nfp1 %d\n",
            p->frames_won[0], p->frames_won[1],
            p->frames_played[0], p->frames_played[1]);
    fprintf(f, "lefty %d\nswapstk %d\ninvsld %d\ninvturn %d\nspots %d\n",
            p->lefty, p->stick_swap, p->inv_slide, p->inv_turn, p->cue_spots);
    fclose(f);
}

/* Room space <-> the table's own frame (metres, X length, Z width, Y up from
 * the cloth), which is the only frame the physics knows about.
 *
 * These MUST agree with the rotation the renderer's model matrix applies, and
 * for a long time they did not: table_to_room used the transpose, so with any
 * non-zero table yaw — and the table is always sited at a yaw taken from the
 * player's head direction — every ball was drawn in one place and tested in
 * another, mirrored across the yaw. The cue tip went through the ball it could
 * see and missed the ball it was tested against, and the on-ball indicator
 * never lit because it was telling the truth.
 *
 * The renderer builds its transform from a quaternion about +Y, which for a
 * point p gives  x' = x·cos + z·sin,  z' = -x·sin + z·cos.  That is the
 * definition these follow now, and test_cue.c checks them against a matrix
 * built the same way the renderer builds it rather than against my arithmetic. */
MoteVrV3 cuevr_table_to_room(const CueVrPlacement *p, Vec3 t) {
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(p->pos.x + t.x * c + t.z * s,
               p->pos.y + t.y,
               p->pos.z - t.x * s + t.z * c);
}

MoteVrV3 cuevr_room_to_table(const CueVrPlacement *p, MoteVrV3 r) {
    MoteVrV3 d = mv3_sub(r, p->pos);
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(d.x * c - d.z * s, d.y, d.x * s + d.z * c);
}

MoteVrV3 cuevr_table_dir_to_room(const CueVrPlacement *p, Vec3 v) {
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

/* A direction only — the same rotation, no translation. */
MoteVrV3 cuevr_room_dir_to_table(const CueVrPlacement *p, MoteVrV3 v) {
    float c = cosf(p->yaw), s = sinf(p->yaw);
    return mv3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
}

void cuevr_cue_update(CueVrCue *c, const MoteVrTracking *t,
                      const CueVrPlacement *p, MoteVrV3 ball_room, float R,
                      CueVrShot *out)
{
    memset(out, 0, sizeof *out);

    /* Which hand is which. A left-hander bridges with the right and grips with
     * the left; swapping the two pointers here is the whole of it, because
     * everything below is written in terms of "the bridge hand" and "the grip
     * hand" rather than in terms of left and right. */
    const MoteVrHand *Lh = &t->hand[s_lefty ? MOTE_VR_RIGHT : MOTE_VR_LEFT];
    const MoteVrHand *Rh = &t->hand[s_lefty ? MOTE_VR_LEFT  : MOTE_VR_RIGHT];
    if (!Lh->tracked || !Rh->tracked) {
        c->tracked = c->on_ball = c->have_prev = c->have_hand = c->stroking = 0;
        /* AND DROP THE REPOSITION ANCHOR. This did not, and that is how the
         * bridge got "out of sync with the controller" out of nowhere.
         *
         * The bridge hand spends the whole game down on the cloth under the
         * player's own body, which is precisely where a headset cannot see it,
         * so the left controller drops tracking constantly. If the grip button
         * happened to be held — and a hand wrapped round a controller rests on
         * that button — the anchor survived the dropout while the hand did not.
         * Tracking comes back somewhere else entirely, the offset is recomputed
         * against an anchor from before, and the cue is suddenly a hand's width
         * from where it belongs. Nothing the player did explains it, because
         * nothing the player did caused it.
         *
         * Forgetting the anchor means the next tracked frame re-anchors where
         * the hand actually is: the cue stays where it was, which is the whole
         * point of the gesture, and nothing jumps. */
        c->adj_have0 = 0;
        c->adjusting = 0;
        return;
    }

    /* THE OFFSET IS IN THE LEFT CONTROLLER'S OWN FRAME, not the room's.
     *
     * It used to be a world-space vector added to the controller, which is
     * right only while you stand where you were standing when you set it. Walk
     * round to the other side of the table and "five centimetres that way"
     * still means the same direction in the ROOM — so it now sits five
     * centimetres the wrong side of your hand, and the cue points somewhere you
     * did not put it. Nothing had gone wrong with the controller, which is
     * exactly why it looked inexplicable.
     *
     * The controller's OWN frame and not the cue's, because the cue is derived
     * from the bridge — taking the frame from the cue would define this in
     * terms of the thing it determines. It is not the two hands either: the
     * grip hand has no business moving the bridge, and a frame built from both
     * lets it.
     *
     * So the offset is bolted to the left controller. It rotates with your
     * wrist, which is what a cue lying across a hand actually does, and it is
     * rigid: the bridge is wherever that controller is, plus a fixed few
     * centimetres in the controller's own axes. Nothing else can move it. */
    c->rest_world = mq_rot(Lh->pose.q, c->rest);
    c->bridge = mv3_add(Lh->pose.p, c->rest_world);

    /* Aim: the line from your grip hand through your bridge hand. Once the
     * stroke is under way it is frozen — the bridge is a pivot during a
     * delivery, not a steering wheel. */
    MoteVrV3 along = mv3_sub(c->bridge, Rh->pose.p);
    if (mv3_len(along) < 0.10f && !c->stroking) {
        /* Hands together: there is no cue line to speak of. Same reasoning as
         * the tracking drop above — do not carry an anchor across a gap in
         * which the hands were free to go anywhere. */
        c->tracked = c->on_ball = c->have_prev = 0;
        c->adj_have0 = 0;
        c->adjusting = 0;
        return;
    }
    c->tracked = 1;
    MoteVrV3 live_axis = mv3_len(along) > 1e-4f ? mv3_norm(along) : mv3(1, 0, 0);

    /* ---- sliding a hand along the cue ----------------------------------- *
     * Either side trigger, and each hand does the thing that hand does. The
     * shared rule is the physical one: sliding your hand ALONG a cue does not
     * move the cue, so while a side trigger is held the aim is pinned and only
     * the hand travels.
     *
     *   right (grip)   changes how much cue is in front of the bridge — this is
     *                  how you shorten up for a tight shot.
     *   left (bridge)  repositions your bridge along the shaft without steering,
     *                  which is otherwise impossible: the bridge hand IS the aim,
     *                  so moving it normally swings the cue.
     *
     * The first version accepted either trigger but only ever measured the RIGHT
     * hand's motion, so squeezing the left did nothing at all. */
    int adj_r = Rh->squeeze > 0.5f && !c->stroking;
    int adj_l = Lh->squeeze > 0.5f && !c->stroking;
    int adjusting = adj_r || adj_l;
    if (adjusting && !c->adjusting) c->adj_axis = live_axis;
    if (adjusting && c->have_hand && adj_r) {
        float d = mv3_dot(mv3_sub(Rh->pose.p, c->prev_hand[MOTE_VR_RIGHT]), c->adj_axis);
        c->grip += d;
        if (c->grip < CUEVR_GRIP_MIN) c->grip = CUEVR_GRIP_MIN;
        if (c->grip > CUEVR_GRIP_MAX) c->grip = CUEVR_GRIP_MAX;
    }
    if (adjusting && c->have_hand && adj_l) {
        /* Hold the trigger and the cue STAYS WHERE IT IS while your hand moves
         * under it — the offset absorbs the motion exactly, all three axes. Let
         * go and that relationship is what you keep. This is a hand sliding on a
         * stationary cue, which is the real thing being modelled.
         *
         * The clamp is a generous sphere rather than the tight vertical band it
         * started as. That band was ±2 cm to +14 cm, so a hand raised more than
         * about five centimetres hit the floor of it and simply stopped
         * tracking — which is what "it snaps back to the same place whatever I
         * do" actually was. A limit you can reach during normal use is not a
         * safety rail, it is a bug. */
        /* ABSOLUTE, not integrated. This accumulated the hand's per-frame
         * delta into the offset — an integrator, so every frame of tracking
         * jitter, every dropped or duplicated pose, was added permanently and
         * never corrected. Over a session the bridge wandered to somewhere
         * unrelated, and it looked like it had failed to save. Anchor the
         * bridge once when the trigger goes down and derive the offset from
         * that anchor every frame: the cue still stands still while the hand
         * moves under it, but nothing can drift. */
        if (!c->adj_have0) {
            /* c->bridge, not hand + rest. `rest` lives in the CONTROLLER's
             * frame now, so adding it straight to a world position treats a
             * local vector as a world one and anchors the cue somewhere it
             * never was — which is why taking hold of it made it jump instead
             * of picking it up where it lay. c->bridge is that same offset
             * already resolved, three lines above. */
            c->adj_bridge0 = c->bridge;
            c->adj_have0 = 1;
        }
        MoteVrV3 off = mv3_sub(c->adj_bridge0, Lh->pose.p);
        float len = mv3_len(off);
        if (len > CUEVR_REST_MAXLEN) {
            /* DRAG THE ANCHOR, do not merely shorten the offset.
             *
             * Shortening it alone left a dead zone the size of however far the
             * hand had strayed: with the anchor fixed and the hand a metre out,
             * the leash is taut, and the player could bring their hand back
             * seventy centimetres before anything moved at all. That is what
             * "virtually impossible to reposition" was — not a stiff control, a
             * frozen one, and it froze exactly when someone had pushed it far
             * enough to want it back.
             *
             * Pulling the anchor along keeps the response immediate at the
             * limit while still refusing to accumulate: tracking jitter never
             * makes the leash taut, so it never moves the anchor, which is the
             * whole point of anchoring in the first place. */
            off = mv3_scale(off, CUEVR_REST_MAXLEN / len);
            c->adj_bridge0 = mv3_add(Lh->pose.p, off);
        }
        /* Back into the controller's own frame to store it. */
        c->rest = mq_rot(mq_conj(Lh->pose.q), off);
        c->rest_world = off;
        c->bridge = mv3_add(Lh->pose.p, off);
    }
    if (!adjusting) c->adj_have0 = 0;   /* re-anchor on the next hold */
    c->adjusting = adjusting;
    /* These two slots are BRIDGE and GRIP, not left and right — they are read
     * back against Lh and Rh, which are already whichever way round the player
     * is handed. The enum names are the storage, not the meaning. */
    c->prev_hand[MOTE_VR_LEFT]  = Lh->pose.p;
    c->prev_hand[MOTE_VR_RIGHT] = Rh->pose.p;
    c->have_hand = 1;
    if (adj_r && !adj_l) live_axis = c->adj_axis;   /* a grip slide does not steer */
    if (adj_l) {
        /* Steer from the butt hand to the bridge — but ONLY while that still
         * describes a cue. A 30 cm offset can put the bridge behind the grip
         * hand, and the moment it crosses over, this vector reverses: the tip
         * swings through ~180 degrees and the player is holding a cue pointing
         * back at themselves, with no obvious way to undo it because the
         * control that got them there now steers backwards too. Below a hand's
         * width apart the direction is noise anyway, so keep the last good
         * line rather than invent a new one. */
        MoteVrV3 v = mv3_sub(c->bridge, Rh->pose.p);
        float vl = mv3_len(v);
        if (vl > 0.08f && mv3_dot(mv3_scale(v, 1.0f / vl), live_axis) > 0.0f)
            live_axis = mv3_scale(v, 1.0f / vl);
    }

    /* ---- the stroke ----------------------------------------------------- *
     * Hysteresis on the trigger, and a wide band of it: pull past 0.55 to take
     * hold, and it does not let go until you release below 0.15.
     *
     * A single threshold is what made the power a lottery. An index trigger is
     * analogue and your whole arm is moving during a delivery, so finger
     * pressure dips — and one dip below the threshold disarmed the stroke, the
     * next frame re-armed it, and re-arming resets the sample window, the clock
     * and the locked tip. The measurement restarted from nothing with a
     * one-frame baseline, at a random point in the delivery.
     *
     * And it was invisible. The cue is drawn from the same hand positions
     * either way, and the re-locked axis differs by however far the hands moved
     * in one frame, which is nothing. So the cue looked perfectly smooth while
     * the number behind it was being thrown away and rebuilt mid-stroke. */
    int want_stroke = c->stroking ? (Rh->trigger > 0.15f) : (Rh->trigger > 0.55f);
    if (want_stroke && !c->stroking) {
        c->stroking = 1;
        /* Freeze the forced elevation with everything else at trigger-down.
         * It cannot be live: the geometry it comes from is measured from the
         * tip, the tip travels back and forth through the delivery, so a live
         * value would swing the cue line while you were stroking through the
         * ball. One angle, decided when you commit to the shot, held. */
        c->lock_elev = c->min_elev;
        c->lock_axis = live_axis;
        /* THE DELIVERY RUNS ALONG THE CUE, INCLUDING ITS ELEVATION.
         *
         * The lock axis is the line the tip travels down for the whole stroke.
         * Taking it from the hands alone meant that whenever the cue was forced
         * up to clear a cushion, the shaft was DRAWN elevated while the tip
         * still slid along the flat hand line — the cue skated forward at an
         * angle instead of striking down through it, which is the same fault
         * the CPU's cue had before its travel was put on the shaft direction.
         *
         * So if the forced angle is steeper than the one the hands are making,
         * the locked line takes the forced angle: elevation is part of the
         * stroke, not a pose applied to it. */
        {   float lay = c->lock_axis.y < -1.0f ? -1.0f
                      : (c->lock_axis.y > 1.0f ? 1.0f : c->lock_axis.y);
            float e_hand = asinf(-lay);
            if (c->lock_elev > e_hand) {
                MoteVrV3 fl = mv3(c->lock_axis.x, 0.0f, c->lock_axis.z);
                if (mv3_len(fl) > 1e-4f) {
                    fl = mv3_norm(fl);
                    float ce = cosf(c->lock_elev), se = sinf(c->lock_elev);
                    c->lock_axis = mv3(fl.x * ce, -se, fl.z * ce);
                }
            }
        }
        c->lock_bridge = c->bridge;
        c->lock_butt0 = Rh->pose.p;
        c->lock_tip0 = mv3_add(Rh->pose.p,
                               mv3_scale(live_axis, CUEVR_CUE_LEN - c->grip));
        c->have_prev = 0;
        c->struck = 0;
        c->speed_n = 0;
        c->t_accum = 0.0f;
    } else if (!want_stroke) {
        c->stroking = 0;
    }

    if (c->stroking) {
        /* Only motion ALONG the cue counts. Wobble across it is not delivery,
         * and a real bridge would absorb it. */
        c->axis = c->lock_axis;
        float travel = mv3_dot(mv3_sub(Rh->pose.p, c->lock_butt0), c->lock_axis);
        c->tip = mv3_add(c->lock_tip0, mv3_scale(c->lock_axis, travel));
        c->butt = mv3_sub(c->tip, mv3_scale(c->lock_axis, CUEVR_CUE_LEN));
        c->bridge = c->lock_bridge;
    } else {
        c->axis = live_axis;
        c->tip  = mv3_add(Rh->pose.p, mv3_scale(c->axis, CUEVR_CUE_LEN - c->grip));
        c->butt = mv3_sub(c->tip, mv3_scale(c->axis, CUEVR_CUE_LEN));
    }

    /* Elevation is the cue's own tilt: the axis runs butt -> tip, so cueing
     * down on the ball (butt raised) points it below horizontal, and the
     * physics wants that as a positive angle. */
    float ay = c->axis.y < -1.0f ? -1.0f : (c->axis.y > 1.0f ? 1.0f : c->axis.y);
    /* SIGNED. Clamping this at zero threw away the one case that matters for
     * the bed: a cue angled down runs its shaft into the cloth behind the tip,
     * and reporting that as "level" meant the floor below had nothing to push
     * against, so you could cue clean through the felt. Negative is butt below
     * tip; the forcing lifts it back to at least the required angle, which is
     * never less than level, so what reaches the physics is still >= 0. */
    c->elev = asinf(-ay);

    MoteVrV3 flat = mv3(c->axis.x, 0.0f, c->axis.z);
    c->aim_dir = mv3_len(flat) > 1e-4f ? mv3_norm(flat) : mv3(1, 0, 0);

    /* Forced elevation. You cannot cue through the cushion or through a ball
     * behind the white, so where the geometry demands it the cue comes up —
     * and the DRAWN shaft comes up with it, pivoting about the tip, which is
     * the one point that must not move (it is on the ball). The alternative,
     * playing elevated while drawing the cue along the hand, puts the shaft
     * visibly through the rail on exactly the shots where the player is most
     * carefully watching it.
     *
     * Cueing higher on the ball lowers the requirement, so the cue settles back
     * down of its own accord as the tip is raised — which is the real
     * technique, discovered rather than explained.
     *
     * Note what is deliberately NOT changed: c->axis stays the line the player
     * is actually holding, because everything below derives the contact point
     * on the ball from it. Where you hit the white is your business; how steeply
     * the stick has to sit to get there is the table's. Only the played
     * elevation and the drawn shaft pivot, both about the tip. */
    /* Only while actually shooting. Outside the stroke the cue is just a stick
     * in your hands and nothing should move it — the correction exists to make
     * a SHOT playable, not to police where you carry the cue. */
    c->elev_forced = 0;
    if (c->stroking && c->elev < c->lock_elev) {
        c->elev = c->lock_elev;
        c->elev_forced = 1;
        float ce = cosf(c->elev), se = sinf(c->elev);
        MoteVrV3 shown = mv3(c->aim_dir.x * ce, -se, c->aim_dir.z * ce);
        c->butt = mv3_sub(c->tip, mv3_scale(shown, CUEVR_CUE_LEN));
    }

    /* ---- where the line meets the ball ---------------------------------- */
    /* The tip is a 5 mm object, so contact is its surface against the ball's,
     * not an infinitely thin line through the ball's centre. It widens the
     * target by the tip's own radius — which is what a real tip does — and it is
     * the physically right test regardless. */
    MoteVrV3 to_ball = mv3_sub(ball_room, c->tip);
    float along_axis = mv3_dot(to_ball, c->axis);
    float perp2 = mv3_dot(to_ball, to_ball) - along_axis * along_axis;
    float reach = R + CUEVR_TIP_R;
    float r2 = reach * reach;

    if (perp2 > r2) {
        /* Missing the ball is the normal state while you line up, so the cue
         * still exists and is still drawn — only the strike is unavailable. */
        c->on_ball = 0;
        c->have_prev = 0;
        c->gap = along_axis - reach;
        return;
    }
    c->on_ball = 1;

    float half_chord = sqrtf(r2 - perp2);
    float t_enter = along_axis - half_chord;
    MoteVrV3 hit = mv3_add(c->tip, mv3_scale(c->axis, t_enter));
    MoteVrV3 off = mv3_sub(hit, ball_room);

    MoteVrV3 side = mv3_cross(mv3(0, 1, 0), c->axis);
    if (mv3_len(side) < 1e-4f) side = mv3(1, 0, 0);
    side = mv3_norm(side);
    MoteVrV3 vert = mv3_norm(mv3_cross(c->axis, side));
    /* Divide by REACH, not by R.
     *
     * `off` runs from the ball's centre to the point where the TIP's surface
     * meets it, so its length is R + the tip's radius — and dividing that by R
     * scaled every offset up by 19%. The physics reads these as a fraction of
     * the ball's radius, so it saw contacts at 1.19 where the ball ends at 1.0,
     * and the consequences were both severe: the drive term is
     * sqrt(1 - side^2 - vert^2), which clamps to ZERO once the pair exceeds one,
     * so a merely off-centre contact gave the ball full spin and no speed at all
     * — the tip visibly striking and the ball barely leaving. And the miscue
     * threshold of 0.55 was tripping at a true offset of 0.46, docking two
     * thirds of the power off ordinary side and screw.
     *
     * Normalised by reach, dead centre is 0 and the edge of the ball is 1, which
     * is what tip_side and tip_vert are defined to mean. */
    c->tip_side = mv3_dot(off, side) / reach;
    c->tip_vert = mv3_dot(off, vert) / reach;
    c->gap = t_enter;

    /* Contact.
     *
     * Only a stroke can play a shot — shuffling about while lining up must never
     * send the ball away — and a stroke plays at most one.
     *
     * The condition is "at or inside the surface, moving forward" rather than
     * "crossed from outside to inside this frame". The stricter form also fails
     * when the tip is already touching at the moment the trigger goes down,
     * which is how you address a ball, and there is no reason to require a
     * backswing that the player may already have made. */
    /* Sample from the FIRST frame of the stroke, not the second. Waiting for a
     * previous frame leaves a two-frame delivery — a hard shot played from close
     * to the ball — with a single sample and therefore no baseline to measure
     * over at all, so it silently would not fire. */
    if (c->stroking && !c->struck && t->dt > 1e-5f) {
        /* Power from a SMOOTHED stroke speed, not from one frame of it.
         *
         * A single frame at 72 Hz is 14 ms, and a millimetre of tracking noise
         * over that reads as 0.07 m/s — so a difference of two or three
         * millimetres between consecutive frames is the difference between a
         * safety and a power shot. That is what made the power feel random. A
         * 35 ms attack averages two or three frames: fast enough to follow a
         * real delivery, slow enough that jitter cannot dominate it. */
        /* Power: one distance over one span of time.
         *
         * Every previous attempt computed a speed per frame — this frame's
         * movement divided by this frame's dt — and then combined those. That
         * multiplies the frame-timing noise straight into the answer: a
         * predicted display time that lands a millisecond early, or a dropped
         * frame that reports one interval's dt for two intervals' motion, and
         * the same physical stroke reads as a tap or a smash. At 72 Hz a dt
         * that is 30% out is a power reading 30% out, and dt at the head of a
         * stroke is exactly where a runtime's prediction is least settled.
         *
         * So keep the raw (gap, elapsed) samples and take a single finite
         * difference across the longest run of FORWARD motion in the window:
         * total distance travelled divided by the total time it took. Individual
         * dt errors cancel because the same frames' times are summed, and a
         * ~100 ms baseline over a real delivery leaves tracking jitter nowhere
         * to hide. Walking back only while the motion is forward means the
         * baseline stops at the turnaround, so a backswing cannot dilute it.
         */
        c->t_accum += t->dt;
        for (int i = CUEVR_SPEED_N - 1; i > 0; i--) {
            c->gap_hist[i] = c->gap_hist[i-1];
            c->t_hist[i]   = c->t_hist[i-1];
        }
        c->gap_hist[0] = c->gap;
        c->t_hist[0]   = c->t_accum;
        if (c->speed_n < CUEVR_SPEED_N) c->speed_n++;

        /* The start of the delivery is the FURTHEST BACK the cue went inside the
         * window — the largest gap. Not "walk back while each step is forward":
         * a millimetre of tracking wobble makes one step non-monotonic and
         * truncates the baseline to two or three frames at random, which is
         * precisely how the same stroke came out as a tap or a smash. Taking the
         * maximum is immune to that and finds the turnaround of the backswing on
         * its own.
         *
         * Ties resolve to the NEWEST of the equal samples, and they have to: the
         * gap is constant while you address the ball, so the window fills with
         * equal values, and reaching past them would drag all that stationary
         * time into the baseline and dilute the speed towards zero. The cost is
         * that the first frame of a delivery has only itself to measure over —
         * the baseline lengthens as the stroke proceeds — so a shot that connects
         * within a frame or two of leaving the address is measured over a short
         * span. The log reports that span for exactly this reason. */
        /* Measure over a FIXED short baseline ending at contact — the speed the
         * tip was doing as it arrived — not from the start of the delivery.
         *
         * This is the whole bug. A real delivery ACCELERATES, and once it does,
         * "average over the delivery so far" and "speed at contact" are
         * different numbers; which one you get depends on how many frames happen
         * to be in the window, and that varied with where the contact landed and
         * with anything that reset the window. Same stroke, different answer,
         * every time. Every test I wrote used a constant-velocity delivery, which
         * measures identically over any baseline, so all of them passed while the
         * thing was a lottery in the hand.
         *
         * A fixed window is consistent by construction: it always reports the
         * same part of the stroke. Three samples is ~42 ms — long enough that
         * tracking noise cannot dominate, short enough to be the arrival speed
         * rather than the whole swing. */
        /* The speed at CONTACT, from a second-order backward difference.
         *
         * A trailing average — over the delivery, or over a fixed window — is
         * biased low by however much the cue accelerated inside it, and that
         * bias grows with the stroke: soft shots came out right and hard ones
         * came out at three quarters. Every earlier test missed it because they
         * all delivered at constant velocity, which has no acceleration to be
         * biased by and measures identically over any baseline.
         *
         *   v ≈ (-3·g0 + 4·g1 - g2) / 2h
         *
         * is exact for constant acceleration, so it reports the arrival speed
         * rather than the average of the swing, and being a three-point estimate
         * it still averages tracking noise. Sampled two frames apart so h is
         * wide enough that noise does not get amplified by the differencing. */
        const int SP = 2;                    /* frames between the three samples */
        int start = 2 * SP;
        float dgap, dtime;
        if (c->speed_n > 2 * SP) {
            float g0 = c->gap_hist[0], g1 = c->gap_hist[SP], g2 = c->gap_hist[2*SP];
            float h = (c->t_hist[0] - c->t_hist[2*SP]) * 0.5f;
            /* Only when the whole span is forward motion; a backswing inside it
             * would make the fit meaningless. */
            if (h > 1e-4f && g1 > g0 && g2 > g1) {
                c->speed = (-3.0f * g0 + 4.0f * g1 - g2) / (2.0f * h);
                dgap = g2 - g0; dtime = h * 2.0f;
            } else {
                start = 1;
                dgap = c->gap_hist[1] - g0;
                dtime = c->t_hist[0] - c->t_hist[1];
                c->speed = dtime > 1e-4f ? dgap / dtime : 0.0f;
            }
        } else {
            start = c->speed_n > 1 ? 1 : 0;
            dgap = c->gap_hist[start] - c->gap_hist[0];
            dtime = c->t_hist[0] - c->t_hist[start];
            c->speed = (dtime > 1e-4f && dgap > 0.0f) ? dgap / dtime : 0.0f;
        }
        if (c->speed < 0.0f) c->speed = 0.0f;
        c->m_frames = start;
        c->m_dist = dgap;
        c->m_time = dtime;

        if (c->speed_n >= 2 && c->gap <= 0.0f && c->speed > 0.12f) {
            c->struck = 1;
            out->struck   = 1;
            out->speed    = c->speed;
            out->tip_side = c->tip_side;
            out->tip_vert = c->tip_vert;
            out->elev     = c->elev;
            MoteVrV3 td = cuevr_room_dir_to_table(p, c->aim_dir);
            out->dir.x = td.x;
            out->dir.y = 0.0f;
            out->dir.z = td.z;
            /* No pace penalty for striking off centre, and no miscue at all.
             *
             * There was a cliff here — past 0.55 of the radius the pace was cut
             * to a flat 35% — and then a gentler version of the same idea. Both
             * were wrong. Half a ball of side is an ordinary shot, and a hard
             * step is a poor model of anything physical even where the effect is
             * real. These are good cues with chalk on them: a tip does not slide
             * off, so nothing here needs to pretend it might.
             *
             * Off centre gives you the spin, and the degree and a half of squirt
             * cue_phys applies, at the pace you played it. That is the whole of
             * it, and the tip cannot reach past the edge of the ball anyway —
             * the contact test is a sphere against a sphere, so there is no
             * region beyond the ball to need a rule for. */
            return;
        }
    } else if (!c->stroking) {
        c->speed = 0.0f;
    }
    c->prev_gap = c->gap;
    c->have_prev = c->stroking;
}
