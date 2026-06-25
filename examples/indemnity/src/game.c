/*
 * indemnity — Indemnity Run (ThumbyElite), the bare-metal Elite-style space sim,
 * ported to Mote. The game owns every pixel (its r3d_* dual-core rasteriser runs in
 * render_band); the engine owns the loop, present, input, timing and memory.
 *
 * This file is the only new code — the Mote glue that replaces ThumbyElite's device
 * shell (device/elite_device_main.c):
 *   · render_band(fb,y0,y1) -> elite_game_render(fb,y0,y1)   (engine runs it dual-core)
 *   · update(dt)            -> input map + elite_game_tick + elite_game_render_begin
 *   · overlay(fb)           -> elite_game_draw_overlay
 *   · audio  -> elite_audio's sfx_* API reimplemented over mote->audio_note (synth dropped)
 *   · plat_* -> save/settings/rumble/controller-setup hooks stubbed (accepted gaps)
 *
 * The ~20k-line game (elite_*, r3d_*, ui_*, econ/events/galaxy/...) is lifted verbatim;
 * its entry points are forward-declared here so this TU doesn't pull the game's vec.h
 * (which defines Vec3/Mat3 like mote_vec.h and can't coexist).
 */
#include "mote_api.h"
#include "mote_build.h"
#include <stdint.h>

#include "craft_buttons.h"     /* CraftRawButtons (vec-free) */
#include "elite_audio.h"       /* the sfx_ and audio_ API we implement here */
#include "elite_platform.h"    /* plat_* hooks we stub here (vec-free) */

/* ThumbyElite entry points (forward-declared; none take a Vec3). */
void elite_game_init(uint32_t seed);
void elite_game_tick(const CraftRawButtons *btn, float dt);
void elite_game_render_begin(void);
void elite_game_render(uint16_t *fb, int y0, int y1);
void elite_game_draw_overlay(uint16_t *fb);
void elite_game_set_frame_ms(float ms);
/* FX particle pool relocated from module .bss to the arena (depth buffer is the
 * engine's now). */
#include <stddef.h>
size_t r3d_fx_parts_bytes(void);  void r3d_fx_set_parts(void *p);
#include "r3d_scene.h"            /* r3d_background (sky) — registered as the bg cb */

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* The engine jump table, shared with every Indemnity render file (elite_engine.h).
 * Set once at init so ships/planets/FX/sky all call the engine directly. */
const MoteApi *g_em;

/* ===================== audio: elite_audio API over BAKED SFX (flash, 0 arena) =====================
 * Every weapon + sound is an editable .sfx recipe (assets/*.sfx, Audio tab) baked to a
 * const PCM clip (<name>_snd, in flash) via wav2snd. We just play those — no per-sound
 * arena allocation. Tune a recipe in Studio and re-Save (which re-bakes its <name>_snd). */
#include "elite_snd_all.h"
/* Volume is the ENGINE master now (shared with the engine menu), not a private
 * per-SFX multiply — play() passes raw gain; master routes through the ABI. */
static void play(const MoteSound *s, float gain){ if(mote->audio_play && s && s->pcm) mote->audio_play(s, gain); }

void audio_init(void) {}
int  audio_render(int16_t *out, int n) { for (int i=0;i<n;i++) out[i]=0; return n; }   /* unused on Mote */
void  audio_set_master(float v){ if(mote->audio_set_master) mote->audio_set_master(v); }
float audio_get_master(void){ return mote->audio_get_master ? mote->audio_get_master() : 1.0f; }
void audio_engine_set(float throttle01, float speed01){ (void)throttle01; (void)speed01; }  /* continuous hum dropped */

void sfx_weapon(int wpn_type, float amp){ if(wpn_type>=0 && wpn_type<18) play(ELITE_WPN_SND[wpn_type], amp); }
void sfx_explosion(float amp, float big01){ (void)big01; play(&sfx_explosion_snd, amp); }
void sfx_hit_shield(void){ play(&sfx_hit_shield_snd, 0.8f); }
void sfx_enemy_shield_hit(void){ play(&sfx_enemy_shield_hit_snd, 0.6f); }
void sfx_lock_acquire(void){ play(&sfx_lock_acquire_snd, 0.7f); }
void sfx_lock_warn(void){ play(&sfx_lock_warn_snd, 0.8f); }
void sfx_hit_hull(void){ play(&sfx_hit_hull_snd, 0.9f); }
void sfx_ui_move(void){ play(&sfx_ui_move_snd, 0.5f); }
void sfx_ui_select(void){ play(&sfx_ui_select_snd, 0.6f); }
void sfx_ui_deny(void){ play(&sfx_ui_deny_snd, 0.6f); }
void sfx_scoop(void){ play(&sfx_scoop_snd, 0.6f); }
void sfx_jump(void){ play(&sfx_jump_snd, 0.9f); }
void sfx_sc_engage(void){ play(&sfx_sc_engage_snd, 0.8f); }
void sfx_charge_step(int step){ (void)step; play(&sfx_charge_step_snd, 0.5f); }
void sfx_chaff(void){ play(&sfx_chaff_snd, 0.7f); }
void sfx_dock(void){ play(&sfx_dock_snd, 0.8f); }
void sfx_klaxon(void){ play(&sfx_klaxon_snd, 0.7f); }

/* ===================== platform hooks: stubbed (accepted gaps) ===================== */
static void put_dash(char *out, int cap){ if(cap>0){ out[0]='-'; if(cap>1) out[1]=0; } }

static int s_slot;   /* active save slot (set via plat_save_slot) */
void plat_rumble(float intensity, float seconds){ if(mote->rumble) mote->rumble(intensity, (int)(seconds*1000.f)); }
int  plat_save(const uint8_t *data, int len){ return mote->save ? mote->save(s_slot, data, len) : 0; }
int  plat_load(uint8_t *data, int max_len){ return mote->load ? mote->load(s_slot, data, max_len) : 0; }
void plat_save_slot(int slot){ s_slot = slot<0?0:slot; }
void plat_save_remove(int slot){ if(mote->save) mote->save(slot, 0, 0); }   /* len 0 clears the slot */
int  plat_save_max_slots(void){ return mote->save_slots ? mote->save_slots() : 1; }
/* which 0 = volume (0..20, drives the audio master), 1 = brightness (no Mote ABI to set
 * the backlight from a game — accepted no-op), >=2 = analog sens (no device input). */
int  plat_setting_get(int which){
    if(which==0) return (int)(audio_get_master()*20.0f + 0.5f);
    if(which==1) return 200;
    return 10;
}
void plat_setting_set(int which, int value){
    if(which==0) audio_set_master((float)value/20.0f);   /* applied to every SFX in play() */
    /* brightness / analog: no game-facing Mote ABI yet */
}

int  plat_ctrl_present(void){ return 0; }
int  plat_ctrl_editable(void){ return 0; }
const char *plat_ctrl_device_name(void){ return "Thumby"; }
void plat_ctrl_axis_label(CtrlAxis ax, char *out, int cap){ (void)ax; put_dash(out,cap); }
void plat_ctrl_btn_label(CtrlButton b, char *out, int cap){ (void)b; put_dash(out,cap); }
void plat_ctrl_capture_begin(int kind, int which){ (void)kind; (void)which; }
int  plat_ctrl_capture_poll(void){ return -1; }   /* never binds */
void plat_ctrl_capture_cancel(void){}
void plat_ctrl_axis_invert(CtrlAxis ax){ (void)ax; }
void plat_ctrl_clear(int kind, int which){ (void)kind; (void)which; }
void plat_ctrl_save(void){}
void plat_ctrl_monitor(void){}
const char *plat_ctrl_last_input(void){ return ""; }
const char *plat_menu_btn(int action){
    switch(action){ case MB_A: return "A"; case MB_B: return "B"; case MB_INFO: return "LB"; default: return "MENU"; }
}

/* ===================== Mote vtable ===================== */
static void map_buttons(const MoteInput *in, CraftRawButtons *b){
    b->up=mote_pressed(in,MOTE_BTN_UP);   b->down=mote_pressed(in,MOTE_BTN_DOWN);
    b->left=mote_pressed(in,MOTE_BTN_LEFT); b->right=mote_pressed(in,MOTE_BTN_RIGHT);
    b->a=mote_pressed(in,MOTE_BTN_A);     b->b=mote_pressed(in,MOTE_BTN_B);
    b->lb=mote_pressed(in,MOTE_BTN_LB);   b->rb=mote_pressed(in,MOTE_BTN_RB);
    b->menu=mote_pressed(in,MOTE_BTN_MENU);
}

static void g_init(void){
    g_em = mote;                                      /* share the engine with all render files */
    r3d_fx_set_parts(mote->alloc(r3d_fx_parts_bytes()));   /* particle pool in the arena */
    if (mote->audio_set_master) mote->audio_set_master(0.7f);   /* default volume (engine master) */
    mote->set_background_cb(r3d_background);           /* starfield/nebula/galaxies/dust */
    elite_game_init((uint32_t)mote->micros() | 1u);   /* SFX are baked flash clips — no arena */
}
static void g_update(float dt){
    CraftRawButtons b; map_buttons(mote->input(), &b);
    elite_game_tick(&b, dt);
    elite_game_render_begin();              /* core0 submits the scene to the engine */
    elite_game_set_frame_ms(dt*1000.f);
}
static void g_overlay(uint16_t *fb){ elite_game_draw_overlay(fb); }

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=0, .overlay=g_overlay,
    /* Renders through the built-in engine: ships/stations -> scene_add_object,
     * planets/suns -> scene_add_sphere_tex, FX -> point/line/disc, sky -> the
     * background callback. Pools sized for the busiest combat scene. */
    .config={ .max_tris=1500, .max_tex_spheres=8, .max_points=256,
              .max_lines=48, .max_discs=16, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
