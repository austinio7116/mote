/*
 * piano3d — a playable 3D piano, and the showcase for Mote's audio engine. Two
 * octaves of real 3D keys; move the hand with LEFT/RIGHT, A strikes the selected
 * key (it dips, lights up, and sounds a note via mote->audio_note). B auto-plays
 * a tune. Polyphonic — held notes ring and decay while you play the next.
 *
 * Controls: LEFT/RIGHT pick key · A play · UP/DOWN octave nudge · B demo tune
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NKEYS 24                 /* MIDI 60 (C4) .. 83 (B5) */
#define KW    0.62f              /* white-key width (world units) */

typedef struct {
    float x;        /* white-key index along the keyboard (blacks sit at -0.5) */
    int   black;    /* 1 if a black key */
    float freq;     /* pitch in Hz */
    float press;    /* 1.0 just struck, decays to 0 (drives the dip + glow) */
} Key;

static Key   key[NKEYS];
static int   nwhite;             /* number of white keys */
static int   cursor = 0;         /* currently selected key */
static int   a_armed;            /* debounce: A must release before re-striking */
static int   octave;             /* octave nudge for the demo tune (-1..0) */
static const Mesh *m_white, *m_black, *m_case;
static Vec3  cam_pos;
static Mat3  cam_basis;

/* "Twinkle Twinkle" as key indices (semitones above C4) + a step duration each */
static const int   TUNE[]  = {0,0,7,7,9,9,7, 5,5,4,4,2,2,0};
static const float TUNED[] = {.34f,.34f,.34f,.34f,.34f,.34f,.62f, .34f,.34f,.34f,.34f,.34f,.34f,.62f};
#define TUNEN 14
static int   tune_i;             /* next tune step index */
static float tune_t;             /* time left on the current tune step */
static int   playing;            /* demo tune running? */

/* strike key i: flag the press, move the cursor, and sound the note */
static void strike(int i){
    if(i < 0 || i >= NKEYS) return;
    key[i].press = 1.0f;
    cursor = i;
    mote->audio_note(key[i].freq, 0.85f);
}

/* world-space X of a key, centred on the keyboard */
static float keyx(int i){
    return (key[i].x - (nwhite - 1) * 0.5f) * KW;
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(26,20,36));
    mote->scene_set_sun(v3_norm(v3(0.3f,0.85f,-0.45f)));

    int wc = 0;
    for(int i = 0; i < NKEYS; i++){
        int midi = 60 + i;
        int pc = midi % 12;
        key[i].black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
        key[i].freq = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
        key[i].press = 0;
        if(key[i].black){
            key[i].x = wc - 0.5f;
        } else {
            key[i].x = wc;
            wc++;
        }
    }
    nwhite = wc;

    m_white = mote_mesh_box(mote, KW*0.45f, 0.18f, 1.7f,  MOTE_RGB565(238,238,232));
    m_black = mote_mesh_box(mote, KW*0.30f, 0.32f, 1.05f, MOTE_RGB565(28,28,34));
    m_case  = mote_mesh_box(mote, nwhite*KW*0.5f+0.3f, 0.4f, 0.35f, MOTE_RGB565(70,42,30));

    cam_pos   = v3(0,5.6f,-7.6f);
    cam_basis = mote_camera_look(cam_pos, v3(0,-0.4f,0.4f));
}

static void g_update(float dt){
    const MoteInput *in = mote->input();

    if(!mote_pressed(in,MOTE_BTN_A)) a_armed = 1;

    /* decay all currently-ringing keys */
    for(int i = 0; i < NKEYS; i++){
        if(key[i].press > 0) key[i].press -= dt * 3.0f;
    }

    /* B toggles the demo tune */
    if(mote_just_pressed(in,MOTE_BTN_B)){
        playing = !playing;
        tune_i = 0;
        tune_t = 0;
    }

    if(playing){
        tune_t -= dt;
        if(tune_t <= 0){
            if(tune_i < TUNEN){
                strike(TUNE[tune_i] + octave*12);
                tune_t = TUNED[tune_i];
                tune_i++;
            } else {
                playing = 0;
            }
        }
    } else {
        if(mote_just_pressed(in,MOTE_BTN_LEFT)  && cursor > 0)        cursor--;
        if(mote_just_pressed(in,MOTE_BTN_RIGHT) && cursor < NKEYS-1)  cursor++;
        if(mote_just_pressed(in,MOTE_BTN_UP)    && octave < 0)        octave++;
        if(mote_just_pressed(in,MOTE_BTN_DOWN)  && octave > -1)       octave--;
        if(a_armed && mote_just_pressed(in,MOTE_BTN_A)) strike(cursor);
    }

    /* ---- render (world coordinates; scene_camera subtracts the camera) ---- */
    mote->scene_camera(&cam_basis, cam_pos, 54.0f);

    mote_draw(mote, m_case, v3(0,-0.18f,1.0f));

    for(int pass = 0; pass < 2; pass++){        /* whites first, then blacks on top */
        for(int i = 0; i < NKEYS; i++){
            if(key[i].black != pass) continue;

            float dip = (key[i].press > 0 ? key[i].press : 0) * 0.16f;
            Vec3 p = key[i].black ? v3(keyx(i), 0.30f - dip, -0.42f)
                                  : v3(keyx(i), -dip, 0.0f);
            mote_draw(mote, key[i].black ? m_black : m_white, p);

            if(key[i].press > 0){
                /* glow sits just above the FRONT edge of the key (toward the camera,
                 * -z) so it isn't buried behind the long key tops */
                Vec3 glow = v3(keyx(i), key[i].black ? 0.58f : 0.24f, key[i].black ? -1.25f : -1.45f);
                mote->scene_add_sphere(glow, 0.18f + key[i].press*0.18f, MOTE_RGB565(120,220,255));
            }
        }
    }

    /* cursor marker hovering over the FRONT of the selected key (visible to the player) */
    if(!playing){
        mote->scene_add_sphere(v3(keyx(cursor), 0.7f, -1.45f), 0.16f, MOTE_RGB565(255,210,80));
    }
}

static void g_overlay(uint16_t *fb){
    static const char *NM[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    mote_ui_panel(fb,1,1,80,11,MOTE_RGB565(20,16,30),MOTE_RGB565(120,90,150));
    mote->text(fb,"PIANO",4,3,MOTE_RGB565(235,210,255));

    /* note name + octave of the selected key, e.g. "C4" */
    int midi = 60 + cursor;
    mote_textf(mote, fb, 52, 3, MOTE_RGB565(255,225,120), "%s%d", NM[midi%12], midi/12 - 1);

    mote->text(fb, playing ? "B  STOP" : "L/R PICK  A PLAY  B TUNE", 3, 118,
               playing ? MOTE_RGB565(120,235,150) : MOTE_RGB565(170,160,200));
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_tris=900, .max_spheres=40, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
