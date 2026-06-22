#ifndef MOTE_ANIM_H
#define MOTE_ANIM_H
/*
 * Mote sprite-animation runtime — header-only, no engine ABI.
 *
 * You author clips in Mote Studio's Anim tab; it bakes them to src/<sheet>.anim.h as
 * const data (so they live in flash, 0 SRAM). At runtime you keep a tiny MoteAnimPlayer
 * per animated sprite, call mote_anim_tick() each frame, and read the current cell to
 * drive a MoteSprite's source rect. Nothing here touches the engine — it works on any
 * firmware.
 *
 *   #include "hero.anim.h"        // MoteAnimSheet hero_sheet; clips hero_idle, hero_walk...
 *   static MoteAnimPlayer p;
 *   mote_anim_play(&p, &hero_walk);
 *   // each frame:
 *   mote_anim_tick(&p, dt);                       // dt in seconds
 *   MoteSprite s = { hero_sheet.image, x, y,
 *                    mote_anim_fx(&p,&hero_sheet), mote_anim_fy(&p,&hero_sheet),
 *                    hero_sheet.tile_w, hero_sheet.tile_h, layer, flags };
 *   mote->scene2d_add(&s);
 *   if (p.event) { ... }                          // a frame-tagged event fired this tick
 */
#include <stdint.h>
#include "mote_2d.h"

enum { MOTE_ANIM_ONCE = 0, MOTE_ANIM_LOOP = 1, MOTE_ANIM_PINGPONG = 2 };

/* One frame of a clip: a cell index into the sheet grid, how long it shows, and an
 * optional event tag fired when the frame becomes current (0 = none). */
typedef struct {
    uint16_t    cell;     /* grid cell index (row-major, cols = sheet->w/tile_w) */
    uint16_t    dur_ms;   /* display time; baked >= 1 */
    const char *event;    /* fires once on entry, else 0 */
} MoteAnimFrame;

/* A named animation: an ORDERED list of frames + loop behaviour + a pivot (origin) the
 * game can use to line up clips whose art sits differently in the cell. */
typedef struct {
    const char        *name;
    const MoteAnimFrame *frames;
    uint16_t           count;
    uint8_t            loop;      /* MOTE_ANIM_* */
    int16_t            pivot_x, pivot_y;
} MoteAnimClip;

/* The atlas the clips index into (a grid of tile_w x tile_h cells). */
typedef struct {
    const MoteImage *image;
    uint16_t         tile_w, tile_h;
} MoteAnimSheet;

/* Per-sprite playback cursor. */
typedef struct {
    const MoteAnimClip *clip;
    uint16_t            i;       /* index within clip->frames */
    int8_t              dir;     /* +1 / -1, for ping-pong */
    uint8_t             done;    /* 1 when a non-looping clip has finished */
    uint32_t            t_ms;    /* time spent on the current frame */
    const char         *event;   /* event tag that fired THIS tick (0 if none) */
} MoteAnimPlayer;

static inline void mote_anim_play(MoteAnimPlayer *p, const MoteAnimClip *c) {
    p->clip = c; p->i = 0; p->dir = 1; p->done = 0; p->t_ms = 0;
    p->event = (c && c->count) ? c->frames[0].event : 0;
}

/* Advance the cursor by dt seconds, honouring per-frame durations and the loop mode.
 * Sets p->event to the tag of any frame entered this tick (the last, if several). */
static inline void mote_anim_tick(MoteAnimPlayer *p, float dt) {
    p->event = 0;
    const MoteAnimClip *c = p->clip;
    if (!c || c->count == 0 || p->done) return;
    if (c->count == 1) return;                       /* a single-frame clip never advances */
    int32_t ms = (int32_t)(dt * 1000.0f + 0.5f);
    if (ms < 0) ms = 0;
    p->t_ms += (uint32_t)ms;
    for (int guard = 0; guard < 4096; guard++) {     /* guard: never spin on a 0ms frame */
        uint16_t dur = c->frames[p->i].dur_ms;
        if (dur == 0) dur = 1;
        if (p->t_ms < dur) break;
        p->t_ms -= dur;
        if (c->loop == MOTE_ANIM_PINGPONG) {
            int32_t ni = (int32_t)p->i + p->dir;
            if (ni >= c->count) { p->dir = -1; ni = c->count - 2; }
            else if (ni < 0)    { p->dir =  1; ni = 1; }
            p->i = (uint16_t)ni;
        } else if (p->i + 1 >= c->count) {
            if (c->loop == MOTE_ANIM_LOOP) p->i = 0;
            else { p->done = 1; break; }
        } else {
            p->i++;
        }
        p->event = c->frames[p->i].event;
    }
}

static inline int  mote_anim_cell(const MoteAnimPlayer *p) {
    return (p->clip && p->clip->count) ? p->clip->frames[p->i].cell : 0;
}
static inline int  mote_anim_cols(const MoteAnimSheet *s) {
    return s->image->w / (s->tile_w ? s->tile_w : 1);
}
static inline int  mote_anim_fx(const MoteAnimPlayer *p, const MoteAnimSheet *s) {
    return (mote_anim_cell(p) % mote_anim_cols(s)) * s->tile_w;
}
static inline int  mote_anim_fy(const MoteAnimPlayer *p, const MoteAnimSheet *s) {
    return (mote_anim_cell(p) / mote_anim_cols(s)) * s->tile_h;
}
static inline int  mote_anim_done(const MoteAnimPlayer *p) { return p->done; }

#endif /* MOTE_ANIM_H */
