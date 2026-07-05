/*
 * thumbycue — ThumbyCue (accurate 3D snooker & pool) ported to Mote.
 *
 * 100% parity: ThumbyCue's renderer/physics/rules/AI/HUD are lifted verbatim from its
 * game/ tree (src/cue_*.c, r3d_*.c). This file is the only new code — the Mote glue
 * that replaces ThumbyCue's device/ layer:
 *   · render_band(fb,y0,y1) -> cue_game_render(fb,y0,y1)  (the engine runs it dual-core)
 *   · update(dt)            -> input map + cue_game_tick + cue_game_render_begin (core0)
 *   · overlay(fb)           -> cue_game_draw_overlay
 *   · audio                 -> cue_audio_sfx() routed to mote->audio_play (synth dropped)
 * The engine owns the loop, dual-core dispatch, framebuffer, present, input and memory;
 * the game owns every pixel. config has no 3D pools (max_tris/depth=0) — ThumbyCue's own
 * rasteriser fills the whole frame, so the full arena is free for its buffers.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <stdint.h>
#include <stddef.h>

#include <string.h>
#include "cue_audio.h"        /* CUE_SFX_* enum + the cue_audio_* API we implement here */
#include "craft_buttons.h"    /* CraftRawButtons */
#include "craft_font.h"       /* craft_font_draw / _width — LINK status overlay */
#include "cue_game.h"         /* cue_game_init/tick/render/overlay + cue_game_link_* */
#include "cue_render.h"       /* cue_render_tab_bytes/stri_bytes/set_buffers */
#include "r3d_raster.h"       /* r3d_depth_bytes/set_depth */
#include "cue_clack_pcm.h"
#include "cue_cueshot_pcm.h"
#include "cue_cushion_pcm.h"
#include "cue_pot_pcm.h"
#include "cue_softpot_pcm.h"
#include "cue_hardpot_pcm.h"

/* ThumbyCue and the engine now share Vec3/Mat3 via mote_vec.h (cue_physics.h includes
 * it), so this TU can include the real ThumbyCue headers directly — no forward-decl
 * shim, and the math/types are the engine's. */

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- audio: ThumbyCue's cue_audio_* API, re-implemented over mote->audio_play.
 * The hit sounds are the original PCM clips; the per-hit pitch wobble, >4 voices and
 * the procedural UI synth are not reproduced (accepted gaps). ---- */
static const MoteSound s_strike  = { cue_cueshot_pcm,  CUE_CUESHOT_PCM_LEN };
static const MoteSound s_clack   = { cue_clack_pcm,    CUE_CLACK_LEN };
static const MoteSound s_cushion = { cue_cushion_pcm,  CUE_CUSHION_PCM_LEN };
static const MoteSound s_pot     = { cue_pot_pcm,      CUE_POT_LEN };
static const MoteSound s_softpot = { cue_softpot_pcm,  CUE_SOFTPOT_LEN };
static const MoteSound s_hardpot = { cue_hardpot_pcm,  CUE_HARDPOT_LEN };
static int s_snooker_audio;

void cue_audio_init(void) {}
void cue_audio_set_volume(int v) {                       /* 0..20 -> engine master 0..1 */
    if (mote->audio_set_master) mote->audio_set_master((float)v / 20.0f);
}
void cue_audio_set_snooker(int on) { s_snooker_audio = on; }
void cue_audio_tick(float dt) { (void)dt; }
void cue_audio_render(int16_t *out, int n) { for (int i = 0; i < n; i++) out[i] = 0; }  /* unused */
void cue_audio_sfx(int which, float intensity) {
    float g = intensity < 0 ? 0 : intensity > 1 ? 1 : intensity;
    switch (which) {
        case CUE_SFX_STRIKE:  mote->audio_play(&s_strike,  0.6f + 0.4f * g); break;
        case CUE_SFX_CLACK:   mote->audio_play(&s_clack,   0.3f + 0.7f * g); break;
        case CUE_SFX_CUSHION: mote->audio_play(&s_cushion, 0.3f + 0.7f * g); break;
        case CUE_SFX_POT:     mote->audio_play(s_snooker_audio ? (g > 0.5f ? &s_hardpot : &s_softpot)
                                                               : &s_pot, 0.7f + 0.3f * g); break;
        case CUE_SFX_UI:      mote->audio_note(660.0f, 0.15f); break;
    }
}

/* ---- input: MoteInput -> ThumbyCue's CraftRawButtons ---- */
static void map_buttons(const MoteInput *in, CraftRawButtons *b) {
    b->up    = mote_pressed(in, MOTE_BTN_UP);
    b->down  = mote_pressed(in, MOTE_BTN_DOWN);
    b->left  = mote_pressed(in, MOTE_BTN_LEFT);
    b->right = mote_pressed(in, MOTE_BTN_RIGHT);
    b->a     = mote_pressed(in, MOTE_BTN_A);
    b->b     = mote_pressed(in, MOTE_BTN_B);
    b->lb    = mote_pressed(in, MOTE_BTN_LB);
    b->rb    = mote_pressed(in, MOTE_BTN_RB);
    b->menu  = mote_pressed(in, MOTE_BTN_MENU);
}

/* ==== 2P LINK (ABI v43): transport + session over mote->link_* ============
 * CROSS-PLATFORM: the two units may differ in architecture (Studio x86 vs ARM
 * device); libm floats (sinf/cosf/sqrtf) diverge across them. So the SHOT-TAKER
 * is authoritative and TRANSMITS ball positions — we NEVER run the same physics
 * on both sides and hope they match. cue_game.c owns the game-state
 * (de)serialisation; this layer is pure transport: handshake, framing,
 * keepalive, timeout. Gated behind mote->abi_version >= 43.
 *
 * Wire: every message is [0xA5][type][len16][payload]. The parser resyncs on
 * the magic byte and drops frames with an absurd length.
 *   'H' proto nonce16  — hello; resent ~0.5 s. Higher nonce = player 0 (breaks,
 *                        picks the game type) — works over the Studio LAN bridge
 *                        where link_is_host() is 0 on BOTH ends.
 *   'S' kind           — player 0 announces the game type.
 *   'C' aim/cue        — cue-line stream while aiming/placing (~10 Hz).
 *   'B' balls          — ball positions while moving (~15 Hz).
 *   'E' final          — authoritative settle (layout + rules + turn).
 *   'A'                — keepalive (~1 Hz when nothing else is sent).
 *   'Q'                — orderly quit.                                          */
#define LK_MAGIC 0xA5
#define LK_PROTO 1
enum { LK_OFF = 0, LK_HS, LK_PLAY };
static int      lk_state;
static uint16_t lk_my_nonce, lk_peer_nonce;
static int      lk_sent_hello, lk_got_hello, lk_got_sel, lk_kind;
static float    lk_hello_t, lk_send_t, lk_rx_age;
static int      lk_frame_ctr;
static uint8_t  lk_buf[CUE_LINK_MAXMSG + 8]; static int lk_len;   /* inbound frame accumulator */

static int  lk_ok(void) { return mote->abi_version >= 44; }   /* net_lobby */
static void lk_new_nonce(void) {
    lk_my_nonce = (uint16_t)(mote->micros() * 2654435761u >> 8);
    if (!lk_my_nonce) lk_my_nonce = 1;
}
/* link_send may accept < len; loop (bounded) so a frame goes out whole. On the
 * host socket the first call takes it all. */
static void lk_raw(const uint8_t *d, int n) {
    int off = 0, tries = 0;
    while (off < n && tries < 128) {
        int s = mote->link_send(d + off, n - off);
        if (s <= 0) { tries++; continue; }
        off += s; tries = 0;
    }
}
static void lk_frame(uint8_t type, const uint8_t *payload, int len) {
    static uint8_t out[CUE_LINK_MAXMSG + 8];
    out[0] = LK_MAGIC; out[1] = type; out[2] = (uint8_t)len; out[3] = (uint8_t)(len >> 8);
    if (len > 0) memcpy(out + 4, payload, (size_t)len);
    lk_raw(out, len + 4);
    lk_send_t = 0;
}
static void lk_send_hello(void) {
    uint8_t p[3] = { LK_PROTO, (uint8_t)lk_my_nonce, (uint8_t)(lk_my_nonce >> 8) };
    lk_frame('H', p, 3);
}

static void lk_dispatch(uint8_t type, const uint8_t *p, int len) {
    lk_rx_age = 0;
    switch (type) {
        case 'H': lk_got_hello = 1; if (len >= 3) lk_peer_nonce = (uint16_t)(p[1] | (p[2] << 8)); break;
        case 'S': if (len >= 1) { lk_kind = p[0]; lk_got_sel = 1; } break;
        case 'A': break;                                   /* keepalive */
        case 'Q': if (lk_state == LK_PLAY && cue_game_link_active()) {
                      cue_game_link_lost(1); mote->link_stop(); lk_state = LK_OFF;
                  } break;
        case 'C': cue_game_link_dec_aim(p, len);   break;
        case 'B': cue_game_link_dec_balls(p, len); break;
        case 'E': cue_game_link_dec_final(p, len); break;
    }
}
static void lk_poll(void) {
    uint8_t chunk[256]; int n;
    while ((n = mote->link_recv(chunk, (int)sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (lk_len == 0) { if (b == LK_MAGIC) lk_buf[lk_len++] = b; continue; }
            lk_buf[lk_len++] = b;
            if (lk_len < 4) continue;                       /* need the header */
            int len = lk_buf[2] | (lk_buf[3] << 8);
            if (len > CUE_LINK_MAXMSG) { lk_len = 0; continue; }   /* junk → resync */
            if (lk_len < 4 + len) continue;                 /* wait for the payload */
            lk_dispatch(lk_buf[1], lk_buf + 4, len);
            lk_len = 0;
        }
    }
}

static void lk_tick(float dt) {
    if (lk_state == LK_OFF) {
        if (lk_ok() && cue_game_link_pending()) {           /* user chose 2P LINK + START */
            /* engine lobby: transport pick + connect + authority (2 beats 1) */
            int host = 0;
            MoteNetCfg cfg = { "ThumbyCue", LK_PROTO, MOTE_NET_ALL };
            if (mote->net_lobby(&cfg, &host) == MOTE_NET_CONNECTED) {
                lk_sent_hello = lk_got_hello = lk_got_sel = 0; lk_len = 0;
                lk_hello_t = lk_send_t = lk_rx_age = 0;
                lk_my_nonce = (uint16_t)(host ? 2 : 1);
                lk_state = LK_HS;
            } else cue_game_link_abort();                   /* backed out of the lobby */
        }
        return;
    }
    if (lk_state == LK_HS) {
        if (!cue_game_link_pending()) { mote->link_stop(); lk_state = LK_OFF; return; }  /* cancelled */
        lk_hello_t -= dt;
        if (mote->link_status() != MOTE_LINK_CONNECTED) {   /* (re)connect restarts hello */
            lk_sent_hello = lk_got_hello = 0; lk_len = 0;
            return;
        }
        if (!lk_sent_hello || lk_hello_t <= 0) { lk_send_hello(); lk_sent_hello = 1; lk_hello_t = 0.5f; }
        lk_poll();
        if (lk_got_hello && lk_peer_nonce == lk_my_nonce) { /* 1-in-65536 tie → re-roll */
            lk_new_nonce(); lk_got_hello = 0; lk_send_hello(); return;
        }
        if (lk_got_hello) {
            if (lk_my_nonce > lk_peer_nonce) {              /* winner: player 0, picks + breaks */
                lk_send_hello();                            /* make sure the peer has ours */
                lk_kind = cue_game_link_kind();
                { uint8_t s = (uint8_t)lk_kind; lk_frame('S', &s, 1); }
                cue_game_link_begin(0, lk_kind);
                lk_state = LK_PLAY; lk_send_t = lk_rx_age = 0; lk_frame_ctr = 0;
            } else if (lk_got_sel) {                        /* loser: start on the host's 'S' */
                cue_game_link_begin(1, lk_kind);
                lk_state = LK_PLAY; lk_send_t = lk_rx_age = 0; lk_frame_ctr = 0;
            }
        }
        return;
    }

    /* LK_PLAY */
    if (!cue_game_link_active()) {                          /* user quit link back to a menu */
        lk_frame('Q', 0, 0); mote->link_stop(); lk_state = LK_OFF; return;
    }
    if (mote->link_status() != MOTE_LINK_CONNECTED) {
        cue_game_link_lost(0); mote->link_stop(); lk_state = LK_OFF; return;
    }
    lk_poll();
    if (lk_state != LK_PLAY) return;                        /* 'Q' ended it inside poll */
    lk_rx_age += dt;
    if (lk_rx_age > 5.0f) { cue_game_link_lost(0); mote->link_stop(); lk_state = LK_OFF; return; }

    static uint8_t pay[CUE_LINK_MAXMSG];
    if (cue_game_link_take_settled()) { int n = cue_game_link_enc_final(pay); lk_frame('E', pay, n); }
    lk_frame_ctr++;
    if (cue_game_link_my_turn()) {
        int ph = cue_game_link_sub();
        if      (ph == 0 && (lk_frame_ctr % 3) == 0) { int n = cue_game_link_enc_aim(pay);   lk_frame('C', pay, n); }
        else if (ph == 1 && (lk_frame_ctr % 2) == 0) { int n = cue_game_link_enc_balls(pay); lk_frame('B', pay, n); }
    }
    lk_send_t += dt;
    if (lk_send_t > 1.0f) lk_frame('A', 0, 0);             /* keepalive */
}

/* Live connection status over the link-wait screen (cue_game draws the frame). */
static void lk_overlay(uint16_t *fb) {
    if (lk_state == LK_OFF || !cue_game_link_pending()) return;
    const char *s = mote->link_status() != MOTE_LINK_CONNECTED ? "CONNECT 2ND UNIT"
                  : !lk_got_hello                              ? "HANDSHAKING..."
                  :                                              "STARTING...";
    uint16_t c = mote->link_status() != MOTE_LINK_CONNECTED ? MOTE_RGB565(255,255,255)
               : !lk_got_hello                              ? MOTE_RGB565(120,220,255)
               :                                              MOTE_RGB565(120,255,150);
    int w = craft_font_width(s);
    craft_font_draw(fb, s, 64 - w / 2, 72, c);
}

/* ---- Mote vtable ---- */
static void g_init(void) {
    /* The table mesh (geometry SOURCE) lives in the arena; the engine now owns the
     * depth buffer and the screen-tri list, so we no longer allocate those here.
     * Must run before cue_game_init (it builds the table into s_tab). */
    void *tab = mote->alloc(cue_render_tab_bytes());
    cue_render_set_buffers(tab, NULL);
    cue_render_set_api(mote);                  /* renderer emits via the engine ABI */
    mote->set_background_cb(cue_render_bg);     /* table backdrop gradient */
    mote->scene_set_near(0.05f);                /* small table — near plane in close */
    mote->audio_set_master(0.7f);               /* match cue_game's default VOLUME (14/20) */

    cue_audio_init();
    cue_game_init((uint32_t)mote->micros() | 1u);
}

static void g_update(float dt) {
    /* Clamp dt before the physics tick, exactly as the standalone ThumbyCue does
     * (device main capped at 0.1s). cue_phys_step runs dt*2000 fixed 2 kHz substeps;
     * without this cap a heavy break frame feeds a bigger dt -> more substeps ->
     * heavier frame -> spiral down to ~30fps. The cap bounds the substep pile-up
     * (brief physics slow-mo on a monster break instead of a framerate collapse). */
    if (dt > 0.1f) dt = 0.1f;
    CraftRawButtons b;
    map_buttons(mote->input(), &b);
    cue_game_tick(&b, dt);
    if (lk_ok()) {                    /* LINK: pump the session after the game tick */
        lk_tick(dt);
        if (lk_state != LK_OFF) mote->set_fps_limit(30);   /* steady pacing while linked */
    }
    cue_game_render_begin();          /* build the engine scene on core0 */
}

static void g_overlay(uint16_t *fb) { cue_game_draw_overlay(fb); lk_overlay(fb); }

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .render_band = 0, .overlay = g_overlay,
    /* Ported onto the built-in engine: table -> scene_add_tri, balls ->
     * scene_add_sphere_tex, aim/cue -> point/line, backdrop -> background_cb.
     * Pools: table tris (+ near-clip splits, cue, shadows), 22 ball impostors,
     * aim/ghost points + lines, and the depth buffer. */
    .config = { .max_tris = 2600, .max_tex_spheres = CUE_MAX_BALLS,
                .max_points = 2 * 48, .max_lines = 2, .max_rings = 2,
                .max_shadows = CUE_MAX_BALLS, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
