/*
 * CueVR — four rigs. See cuevr_light.h for why they are data.
 *
 * The numbers are the real ones where a real one exists. A match-table light is
 * a bar of shades about 800 mm above the bed on 850 mm centres, and the shades
 * are wide: the size of a highlight is the size of the SOURCE, so a small bright
 * lamp high up mirrors in a 52 mm ball as a speck and a big low one gives the
 * long streaks you see down a televised table. Domestic downlights are 60-90 mm
 * across and two and a half metres up, which is why a front room gives a ball
 * several small hard highlights and several faint shadows instead of one of
 * each. A window is one source metres wide, at head height, off to the side.
 */
#include "cuevr_light.h"

#include <math.h>

/* Overridable so a sweep is one -D away rather than an edit per frame. */
#ifndef ROOM_SPOT
#define ROOM_SPOT 2.6f
#endif
#include <string.h>

static const char *NAMES[CUEVR_LIGHT_N] = {
    "MATCH", "SIX SHADE", "ROOM", "WINDOW"
};

const char *cuevr_light_name(int mode) {
    if (mode < 0 || mode >= CUEVR_LIGHT_N) return "?";
    return NAMES[mode];
}

static void set3(float *d, float a, float b, float c) { d[0]=a; d[1]=b; d[2]=c; }

/* A row of n fixtures down the middle of the table, evenly spaced over `span`
 * metres. The layout every hanging rig uses; only the counts and sizes differ. */
static void bar(CueVrLightRig *r, int n, float span, float h,
                float hx, float hz, float gain) {
    if (n > CUEVR_MAX_LAMPS) n = CUEVR_MAX_LAMPS;
    r->nlamp = n;
    for (int i = 0; i < n; i++) {
        float tx = ((float)i + 0.5f) / (float)n * span - span * 0.5f;
        set3(r->lamp[i].c, tx, h, 0.0f);
        set3(r->lamp[i].ax, hx, 0.0f, 0.0f);     /* a shade faces down */
        set3(r->lamp[i].az, 0.0f, 0.0f, hz);
        r->lamp[i].gain = gain;
    }
}

/* A grid of cols x rows over an area larger than the table — a ceiling, not a
 * bar. The table is somewhere under it rather than the reason for it, which is
 * the whole difference between a snooker hall and a living room. */
static void grid(CueVrLightRig *r, int cols, int rows, float spanx, float spanz,
                 float h, float rad, float gain) {
    int n = cols * rows;
    if (n > CUEVR_MAX_LAMPS) { rows = CUEVR_MAX_LAMPS / cols; n = cols * rows; }
    r->nlamp = 0;
    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            CueVrLamp *l = &r->lamp[r->nlamp++];
            float tx = ((float)i + 0.5f) / (float)cols * spanx - spanx * 0.5f;
            float tz = rows == 1 ? 0.0f
                     : ((float)j + 0.5f) / (float)rows * spanz - spanz * 0.5f;
            set3(l->c, tx, h, tz);
            set3(l->ax, rad, 0.0f, 0.0f);
            set3(l->az, 0.0f, 0.0f, rad);
            l->gain = gain;
        }
    }
}

void cuevr_light_build(int mode, const CueTable *t, CueVrLightRig *out) {
    memset(out, 0, sizeof *out);
    out->spot = 1.0f;               /* honest unless a rig says otherwise */
    float L = t->half_len * 2.0f;
    float W = t->half_wid * 2.0f;

    switch (mode) {
    default:
    case CUEVR_LIGHT_MATCH:
        out->soft = 0.10f; out->dark = 0.72f;
        /* What the table has had all along, kept exactly: two to four shades
         * on ~850 mm centres, 620 mm up, in a dark hall. Changing this while
         * adding the others would have meant the mode everyone plays in shifted
         * under them for no reason they asked for. */
        out->name = NAMES[mode];
        set3(out->key, 0.10f, 0.975f, 0.20f);
        set3(out->keyc, 1.0f, 1.0f, 1.0f);
        out->fill = 0.0f;
        out->round = 0;
        {   int n = (int)(L / 0.85f + 0.5f);
            if (n < 2) n = 2;
            if (n > 4) n = 4;
            bar(out, n, L, 0.62f, 0.30f, 0.17f, 1.0f);
        }
        break;

    case CUEVR_LIGHT_SIX:
        out->soft = 0.14f; out->dark = 0.66f;
        /* The full match rig: six smaller shades instead of three big ones, so
         * a ball carries six distinct streaks and the cloth is lit evenly from
         * baulk to black. Hung a little higher because six of them at 620 mm
         * would be in the way of a cue. */
        out->name = NAMES[mode];
        set3(out->key, 0.05f, 0.99f, 0.13f);
        set3(out->keyc, 1.0f, 0.995f, 0.97f);
        out->fill = 0.06f;
        out->round = 0;
        /* THREE BY TWO, not a line of six. A six-shade rig is two rows down the
         * table, which is what puts a highlight on both flanks of a ball
         * instead of six in a stripe along the top of it. */
        grid(out, 3, 2, L * 0.94f, W * 0.62f, 0.70f, 0.0f, 0.82f);
        for (int i = 0; i < out->nlamp; i++) {
            out->lamp[i].ax[0] = 0.185f; out->lamp[i].ax[2] = 0.0f;
            out->lamp[i].az[0] = 0.0f;   out->lamp[i].az[2] = 0.150f;
        }
        break;

    case CUEVR_LIGHT_ROOM:
        out->soft = 0.30f; out->dark = 0.55f;
        /* An ordinary room with the ceiling lights on. Six downlights on a
         * 3 x 2 grid over an area half again the size of the table, 1.75 m
         * above the cloth — a 2.4 m ceiling over a 0.85 m table. Small discs,
         * so the highlights are small and hard; warm, because domestic lamps
         * are; and a lot of fill, because the walls are close and everything
         * bounces. This is the mode where the underside of the cushions and
         * the inside of the pockets stop being black. */
        out->name = NAMES[mode];
        set3(out->key, 0.0f, 1.0f, 0.0f);
        set3(out->keyc, 1.0f, 0.945f, 0.86f);
        out->fill = 0.42f;
        out->round = 1;
        /* The one rig that needs help. Six 110 mm discs at 1.75 m reflect as
         * specks a couple of pixels across in a 26 mm ball — accurate, and it
         * reads as dirt rather than as a lit room. Drawn half again over life
         * they become six small round highlights you can count, which is what
         * tells you where you are standing. Kept well short of the point where
         * neighbours touch: they must stay six, not one. */
        out->spot = ROOM_SPOT;
        /* 110 mm across, not 45. A domestic downlight has a wide diffuser and a
         * ball mirrors the SOURCE — at 45 mm the reflections were specks, which
         * is not what a lit room looks like in a polished ball. */
        grid(out, 3, 2, L * 1.15f, W * 1.9f, 1.75f, 0.110f, 0.62f);
        break;

    case CUEVR_LIGHT_WINDOW:
        out->soft = 0.70f; out->dark = 0.48f;
        /* Daylight from one side. One source, metres across, at head height and
         * out beyond the long rail — so the key is raked rather than overhead,
         * every ball throws one long shadow across the cloth, and the far side
         * of every cushion goes into shade. Cool, and a strong sky fill from
         * above because the ceiling is being lit by the same window.
         *
         * A big low source is the one case where the reflection is not a spot
         * at all: it is a broad wash down one flank of each ball, which is what
         * makes it read as daylight rather than as a very large lamp. */
        out->name = NAMES[mode];
        set3(out->key, -0.16f, 0.44f, -0.88f);
        set3(out->keyc, 0.93f, 0.965f, 1.0f);
        out->fill = 0.30f;
        out->round = 0;
        out->nlamp = 1;
        set3(out->lamp[0].c, -L * 0.10f, 0.42f, -(W * 0.5f + 1.30f));
        set3(out->lamp[0].ax, 0.95f, 0.0f, 0.0f);   /* a pane stands up */
        set3(out->lamp[0].az, 0.0f, 0.75f, 0.0f);
        out->lamp[0].gain = 0.85f;
        break;
    }

    /* Normalise the key here rather than in the shader, where it would be done
     * once per fragment for a value that changes once per frame. */
    float k = sqrtf(out->key[0]*out->key[0] + out->key[1]*out->key[1]
                  + out->key[2]*out->key[2]);
    if (k > 1e-6f) { out->key[0] /= k; out->key[1] /= k; out->key[2] /= k; }
    out->name = NAMES[(mode < 0 || mode >= CUEVR_LIGHT_N) ? 0 : mode];
}
