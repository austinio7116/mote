/*
 * Indemnity Run — 1v1 LINK ARENA (PvP). See elite_pvp.h for the design.
 */
#include "elite_pvp.h"
#include "elite_engine.h"       /* g_em: the Mote ABI (link_*, set_fps_limit) */
#include "elite_entity.h"
#include "elite_ships.h"
#include "elite_weapons.h"
#include "elite_player.h"
#include "elite_flight.h"
#include "elite_input.h"
#include "elite_combat.h"
#include "elite_proj.h"
#include "elite_rocks.h"
#include "elite_loot.h"
#include "elite_save.h"
#include "elite_audio.h"
#include "elite_types.h"
#include "craft_font.h"
#include "r3d_fx.h"
#include <string.h>
#include <math.h>
#ifdef MOTE_HOST
#include <stdio.h>
#include <stdlib.h>
#define PVPDBG(...) do{ if(getenv("MOTE_PVP_DEBUG")) fprintf(stderr, "[PVP] " __VA_ARGS__); }while(0)
#else
#define PVPDBG(...) do{}while(0)
#endif

/* Configures elite_game.c's statics for the empty-space arena (anchor off,
 * HUD target locked to the remote). Defined in elite_game.c. */
void elite_game_pvp_prep(void);

/* Load the save + build our outgoing identity (heavy; done once at begin). */
static void id_build_mine(void);

/* ---- wire protocol (all frames start 0xA5) ------------------------------
 * 'H' proto8 nonce16                    handshake hello (also the tie-break)
 * 'I' <identity>                        ship + loadout (see id_encode)
 * 'P' <state>                           pos/orient/vel/hp @ ~18 Hz (keepalive)
 * 'F' wtype8                            fired (peer shows muzzle FX + sound)
 * 'D' dmg16 wtype8                      I hit you for dmg (victim applies)
 * 'K'                                   I died (peer: VICTORY)
 * 'Q'                                   I quit                                */
#define PVP_PROTO   1
#define PVP_SEP     1600.0f      /* ~1.6 km apart at spawn (user spec 1.5-2.5) */

/* End states. */
enum { END_NONE = 0, END_VICTORY, END_DEFEAT, END_LINKLOST };

typedef struct {
    uint8_t  hull_id;
    uint32_t hull_seed;
    uint8_t  nw, wpn[MAX_HARDPOINTS];
    uint8_t  shv, sht, arv, art;
    uint16_t hull_max, shield_max;
} PvpIdent;

static int      s_active;             /* arena running */
static int      s_waiting;            /* link-wait screen up */
static uint16_t s_my_nonce, s_peer_nonce;
static int      s_sent_hello, s_got_hello, s_got_ident;
static float    s_hello_t;
static PvpIdent s_my_id, s_peer_id;

static float    s_send_t, s_rx_age;
static int      s_end;                /* END_* */
static int      s_death_sent;

/* Replicated remote target (packet -> smoothed onto the pool entity). */
static Vec3     s_rp_pos;
static Vec3     s_rp_fwd, s_rp_up;
static Vec3     s_rp_vel;

/* Fire-edge detection for 'F'. */
static float    s_prev_firecool;

/* Frame reassembly. */
static uint8_t  s_msg[40];
static int      s_msg_len;

/* ---- little helpers ----------------------------------------------------- */
static void put16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t get16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static void putf(uint8_t *p, float f){ memcpy(p, &f, 4); }   /* IEEE754 LE both ends */
static float getf(const uint8_t *p){ float f; memcpy(&f, p, 4); return f; }

static void link_tx(const void *b, int n){ if (g_em && g_em->link_send) g_em->link_send(b, n); }

/* ---- identity ----------------------------------------------------------- */
static void id_snapshot(void) {          /* build s_my_id from the live player */
    const Ship *p = &g_ships[PLAYER];
    s_my_id.hull_id   = g_player.hull_id;
    s_my_id.hull_seed = g_player.hull_seed;
    s_my_id.nw = p->n_weapons > MAX_HARDPOINTS ? MAX_HARDPOINTS : p->n_weapons;
    for (int i = 0; i < MAX_HARDPOINTS; i++)
        s_my_id.wpn[i] = (i < s_my_id.nw) ? p->weapons[i] : 0;
    s_my_id.shv = p->shield_var;  s_my_id.sht = g_player.shield_eq.tier;
    s_my_id.arv = p->armor_var;   s_my_id.art = g_player.armor_eq.tier;
    float hm = p->hull_max, sm = p->shield_max;
    s_my_id.hull_max   = (uint16_t)(hm   < 0 ? 0 : hm   > 65535 ? 65535 : hm);
    s_my_id.shield_max = (uint16_t)(sm   < 0 ? 0 : sm   > 65535 ? 65535 : sm);
}
static void id_send(void) {
    uint8_t m[21]; int k = 0;
    m[k++] = 0xA5; m[k++] = 'I';
    m[k++] = s_my_id.hull_id;
    m[k++] = (uint8_t)s_my_id.hull_seed;       m[k++] = (uint8_t)(s_my_id.hull_seed >> 8);
    m[k++] = (uint8_t)(s_my_id.hull_seed >> 16); m[k++] = (uint8_t)(s_my_id.hull_seed >> 24);
    m[k++] = s_my_id.nw;
    m[k++] = s_my_id.wpn[0]; m[k++] = s_my_id.wpn[1]; m[k++] = s_my_id.wpn[2];
    m[k++] = s_my_id.shv; m[k++] = s_my_id.sht; m[k++] = s_my_id.arv; m[k++] = s_my_id.art;
    put16(m + k, s_my_id.hull_max);   k += 2;
    put16(m + k, s_my_id.shield_max); k += 2;
    link_tx(m, k);
}
static void id_decode(const uint8_t *m) {   /* m points at the 0xA5 */
    const uint8_t *b = m + 2;
    s_peer_id.hull_id   = b[0];
    s_peer_id.hull_seed = (uint32_t)b[1] | ((uint32_t)b[2] << 8) |
                          ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 24);
    s_peer_id.nw = b[5] > MAX_HARDPOINTS ? MAX_HARDPOINTS : b[5];
    s_peer_id.wpn[0] = b[6]; s_peer_id.wpn[1] = b[7]; s_peer_id.wpn[2] = b[8];
    s_peer_id.shv = b[9]; s_peer_id.sht = b[10]; s_peer_id.arv = b[11]; s_peer_id.art = b[12];
    s_peer_id.hull_max   = get16(b + 13);
    s_peer_id.shield_max = get16(b + 15);
}

/* ---- state packet ------------------------------------------------------- */
static void state_send(void) {
    const Ship *p = &g_ships[PLAYER];
    uint8_t m[30]; int k = 0;
    m[k++] = 0xA5; m[k++] = 'P';
    putf(m + k, p->pos.x); k += 4; putf(m + k, p->pos.y); k += 4; putf(m + k, p->pos.z); k += 4;
    Vec3 f = p->basis.r[2], u = p->basis.r[1];
    m[k++] = (uint8_t)(int8_t)(f.x * 127.0f); m[k++] = (uint8_t)(int8_t)(f.y * 127.0f); m[k++] = (uint8_t)(int8_t)(f.z * 127.0f);
    m[k++] = (uint8_t)(int8_t)(u.x * 127.0f); m[k++] = (uint8_t)(int8_t)(u.y * 127.0f); m[k++] = (uint8_t)(int8_t)(u.z * 127.0f);
    int16_t vx = (int16_t)(p->vel.x * 10.0f), vy = (int16_t)(p->vel.y * 10.0f), vz = (int16_t)(p->vel.z * 10.0f);
    put16(m + k, (uint16_t)vx); k += 2; put16(m + k, (uint16_t)vy); k += 2; put16(m + k, (uint16_t)vz); k += 2;
    m[k++] = (uint8_t)(p->throttle * 255.0f);
    float hp = p->hull_max   > 0 ? p->hull   / p->hull_max   : 0;
    float sp = p->shield_max > 0 ? p->shield / p->shield_max : 0;
    m[k++] = (uint8_t)(hp < 0 ? 0 : hp > 1 ? 255 : hp * 255.0f);
    m[k++] = (uint8_t)(sp < 0 ? 0 : sp > 1 ? 255 : sp * 255.0f);
    m[k++] = (uint8_t)((p->boost_t > 0 ? 1 : 0) | (!p->alive ? 2 : 0));
    link_tx(m, k);
}
static void state_decode(const uint8_t *m) {
    const uint8_t *b = m + 2;
    s_rp_pos = v3(getf(b), getf(b + 4), getf(b + 8));
    s_rp_fwd = v3((int8_t)b[12] / 127.0f, (int8_t)b[13] / 127.0f, (int8_t)b[14] / 127.0f);
    s_rp_up  = v3((int8_t)b[15] / 127.0f, (int8_t)b[16] / 127.0f, (int8_t)b[17] / 127.0f);
    int16_t vx = (int16_t)get16(b + 18), vy = (int16_t)get16(b + 20), vz = (int16_t)get16(b + 22);
    s_rp_vel = v3(vx / 10.0f, vy / 10.0f, vz / 10.0f);
    /* b[24] throttle (unused); b[25]/b[26] hp/shield %, b[27] flags */
    Ship *r = &g_ships[PVP_REMOTE];
    if (r->alive) {
        r->hull   = (b[25] / 255.0f) * r->hull_max;
        r->shield = (b[26] / 255.0f) * r->shield_max;
    }
    s_rx_age = 0;
}

/* ---- small sends -------------------------------------------------------- */
static void send1(uint8_t t){ uint8_t m[2] = { 0xA5, t }; link_tx(m, 2); }
static void send_hello(void){
    uint8_t m[5] = { 0xA5, 'H', PVP_PROTO, (uint8_t)s_my_nonce, (uint8_t)(s_my_nonce >> 8) };
    link_tx(m, 5);
}

/* ---- receive ------------------------------------------------------------ */
static void handle(const uint8_t *m) {
    switch (m[1]) {
    case 'H': s_got_hello = 1; s_peer_nonce = get16(m + 3); break;
    case 'I': s_got_ident = 1; id_decode(m); break;
    case 'P': if (s_active) state_decode(m); break;
    case 'F': if (s_active) {                         /* remote muzzle FX + sound */
                  Ship *r = &g_ships[PVP_REMOTE];
                  if (r->alive) {
                      int wt = m[2];
                      Vec3 muz = v3_add(r->pos, v3_scale(r->basis.r[2],
                                        r->mesh ? r->mesh->bound_r : 6.0f));
                      fx_beam(muz, v3_add(muz, v3_scale(r->basis.r[2], 40.0f)),
                              k_weapons[wt < WPN_COUNT ? wt : 0].color);
                      float d = v3_len(v3_sub(r->pos, g_ships[PLAYER].pos));
                      sfx_weapon(wt, 0.5f - d / 1400.0f);
                  }
              } break;
    case 'D': if (s_active && s_end == END_NONE && g_ships[PLAYER].alive) {
                  float dmg = (float)get16(m + 2);
                  int wt = m[4];
                  combat_set_shot_type(wt);
                  combat_direct_damage(PVP_REMOTE, PLAYER, dmg, g_ships[PLAYER].pos);
              } break;
    case 'K': if (s_active && s_end == END_NONE) {     /* peer died -> VICTORY */
                  Ship *r = &g_ships[PVP_REMOTE];
                  if (r->alive) { fx_spawn_explosion(r->pos, r->vel);
                                  sfx_explosion(1.0f, r->mesh ? r->mesh->bound_r / 15.0f : 0.5f);
                                  r->alive = false; }
                  s_end = END_VICTORY;
              } break;
    case 'Q': if (s_active && s_end == END_NONE) s_end = END_LINKLOST; break;
    }
}
static void poll(void) {
    uint8_t chunk[64]; int n;
    if (!g_em || !g_em->link_recv) return;
    while ((n = g_em->link_recv(chunk, (int)sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (s_msg_len == 0) { if (b == 0xA5) s_msg[s_msg_len++] = b; continue; }
            if (s_msg_len < (int)sizeof s_msg) s_msg[s_msg_len++] = b;
            int t = s_msg[1];
            int want = t == 'P' ? 30 : t == 'I' ? 19 : t == 'H' ? 5
                     : t == 'D' ? 5 : t == 'F' ? 3
                     : (t == 'K' || t == 'Q') ? 2 : -1;
            if (want < 0) { s_msg_len = 0; continue; }   /* junk: resync */
            if (s_msg_len < want) continue;
            handle(s_msg);
            s_msg_len = 0;
        }
    }
}

/* ---- ship construction -------------------------------------------------- */
static void face_toward(Ship *s, Vec3 from, Vec3 to) {
    Vec3 fwd = v3_norm(v3_sub(to, from));
    Vec3 up0 = v3(0, 1, 0);
    s->basis.r[2] = fwd;
    s->basis.r[0] = v3_norm(v3_cross(up0, fwd));
    s->basis.r[1] = v3_cross(fwd, s->basis.r[0]);
    m3_orthonormalize(&s->basis);
}

/* Load the player's saved commander (their ship + loadout). Falls back to a
 * stock combat fit when no save exists so PvP is playable headless / fresh. */
static void load_my_ship(void) {
    int slot = -1, mx = save_max_slots();
    for (int s = 0; s < mx; s++) {
        SavePeek pk;
        if (save_peek(s, &pk) && pk.valid) { slot = s; break; }
    }
    if (slot >= 0) {
        save_set_slot(slot);
        SaveMeta meta;
        if (save_load(&meta)) return;      /* g_player now holds their ship */
    }
    /* STOCK FALLBACK: a REAVER (fast-turning, 3 hardpoints) with a decent
     * mid-grade fit — stated in the design as the no-save loadout. */
    player_init();
    g_player.hull_id   = 4;                 /* REAVER */
    g_player.hull_seed = 0x51EED04u;
    g_player.difficulty = 1;                /* MEDIUM */
    for (int i = 0; i < HULL_SLOTS; i++) { g_player.mounts[i].in_use = 0; g_player.ammo[i] = -1; }
    g_player.mounts[0] = (WeaponInst){ .type = WPN_PULSE_M,    .quality = Q_STANDARD, .integrity = 100, .in_use = 1 };
    g_player.mounts[1] = (WeaponInst){ .type = WPN_AUTOCANNON, .quality = Q_STANDARD, .integrity = 100, .in_use = 1 };
    g_player.shield_eq = (WeaponInst){ .type = EQ_SHIELD, .quality = Q_STANDARD, .integrity = 100, .in_use = 1, .tier = 2 };
    g_player.armor_eq  = (WeaponInst){ .type = EQ_ARMOR,  .quality = Q_STANDARD, .integrity = 100, .in_use = 1, .tier = 2 };
}

/* The arena build runs ONE STEP PER FRAME (s_build_step 1..8) with the step
 * number painted on the sync card — if a device wedges mid-build, the frozen
 * number names the culprit; and the frame loop keeps running between steps,
 * so hellos/keepalives still flow and the peer can't time out on a SLOW step. */
static int s_build_step;
static void build_arena_step(void) {
    int i_won = s_my_nonce > s_peer_nonce;    /* both compute both spawn sides */
    float sep = PVP_SEP;
#ifdef MOTE_HOST
    if (getenv("MOTE_PVP_SEP")) sep = (float)atof(getenv("MOTE_PVP_SEP"));  /* test hook */
#endif
    Vec3 mypos = v3(i_won ? -sep * 0.5f :  sep * 0.5f, 0, 0);
    Vec3 rmpos = v3(i_won ?  sep * 0.5f : -sep * 0.5f, 0, 0);

    switch (s_build_step) {
    case 1: ships_init(); fx_init(); break;
    case 2: proj_init(); rocks_init(); break;  /* empty space: no belt/traffic/salvage */
    case 3: loot_init(); break;
    case 4: combat_init(); combat_set_kills(0); elite_input_reset(); break;
    case 5: {

    /* g_player was loaded + snapshotted in pvp_begin BEFORE the lobby opened —
     * never touch the save filesystem here (USB link is live). */
        Ship *p = &g_ships[PLAYER];
        p->alive = true;
        p->pos = mypos;
        p->vel = v3(0, 0, 0);
        p->throttle = 0.0f;
        p->assist = true;
        p->boost_t = 0; p->heat = 0; p->fire_cool = 0;
        p->team = TEAM_PLAYER;
        player_apply_to_ship();               /* hull/shield/turn from g_player */
        p->turret_type = 0;                    /* no auto-aim help in a duel */
        p->hull = p->hull_max;
        p->shield = p->shield_max;
        face_toward(p, mypos, rmpos);
        id_snapshot();
        break; }
    case 6: {

    /* --- the peer (entity PVP_REMOTE) --- */
        Ship *r = &g_ships[PVP_REMOTE];
        memset(r, 0, sizeof *r);
        r->alive = true;
        r->mesh = hull_mesh(s_peer_id.hull_seed, s_peer_id.hull_id);
        r->team = TEAM_HOSTILE;
        r->ai_state = AI_NONE;                  /* driven by packets, never local AI */
        r->hull_max   = s_peer_id.hull_max   > 0 ? s_peer_id.hull_max   : 100;
        r->shield_max = s_peer_id.shield_max;
        r->hull = r->hull_max; r->shield = r->shield_max;
        r->shield_var = s_peer_id.shv; r->armor_var = s_peer_id.arv;
        r->max_speed = 500.0f; r->accel = 300.0f; r->turn_rate = 2.5f;
        r->assist = false; r->throttle = 0.0f; /* -> ship_physics dead-reckons on vel */
        r->n_weapons = s_peer_id.nw;
        for (int i = 0; i < MAX_HARDPOINTS; i++) r->weapons[i] = s_peer_id.wpn[i];
        r->cls = s_peer_id.hull_id;
        r->pos = rmpos;
        face_toward(r, rmpos, mypos);
        s_rp_pos = rmpos; s_rp_fwd = r->basis.r[2]; s_rp_up = r->basis.r[1]; s_rp_vel = v3(0,0,0);
        break; }
    case 7: elite_game_pvp_prep(); break;       /* anchor off, HUD locks the remote */
    case 8: {
        s_active = 1; s_waiting = 0;
        s_end = END_NONE; s_death_sent = 0;
        s_send_t = 0; s_rx_age = 0;
        s_prev_firecool = 0;
        Ship *p = &g_ships[PLAYER]; Ship *r = &g_ships[PVP_REMOTE];
        PVPDBG("ARENA built: i_won=%d my(nonce=%u hull=%.0f shd=%.0f) peer(hull_id=%d hull=%.0f shd=%.0f) mypos=(%.0f,%.0f,%.0f) rmpos=(%.0f,%.0f,%.0f)\n",
               i_won, s_my_nonce, p->hull_max, p->shield_max, s_peer_id.hull_id,
               r->hull_max, r->shield_max, p->pos.x, p->pos.y, p->pos.z, r->pos.x, r->pos.y, r->pos.z);
        break; }
    }
    (void)i_won;
}
int pvp_build_step(void) { return s_build_step; }   /* sync-card diagnostic */

/* ---- public API --------------------------------------------------------- */
int pvp_active(void)      { return s_active; }
int pvp_waiting(void)     { return s_waiting; }
int pvp_remote_slot(void) { return PVP_REMOTE; }

int pvp_begin(void) {
    /* engine lobby: transport pick + connect + authority (2 beats 1) */
    int host = 0;
    MoteNetCfg cfg = { "IndemnityRun", PVP_PROTO, MOTE_NET_ALL };
    if (!g_em || g_em->abi_version < 44 ||
        g_em->net_lobby(&cfg, &host) != MOTE_NET_CONNECTED) return 0;
    s_my_nonce = (uint16_t)(host ? 2 : 1);
    s_sent_hello = s_got_hello = s_got_ident = 0;
    s_hello_t = 0; s_msg_len = 0;
    s_build_step = 0;
    s_active = 0; s_waiting = 1;
    return 1;
}

int pvp_wait_tick(const CraftRawButtons *btn, float dt) {
    static int prev_b;
    int b_edge = btn->b && !prev_b; prev_b = btn->b;
    if (b_edge) { pvp_end(); return PVP_CANCEL; }

    int st = (g_em && g_em->link_status) ? g_em->link_status() : 0;
    if (st != MOTE_LINK_CONNECTED) {           /* (re)connect restarts handshake */
        s_sent_hello = s_got_hello = s_got_ident = 0;
        s_hello_t = 0; s_msg_len = 0;
        return PVP_WAIT;
    }
    s_hello_t -= dt;
    if (!s_sent_hello || s_hello_t <= 0) {
        send_hello();
        if (!s_got_ident) id_send();       /* resend cached identity until acked */
        s_sent_hello = 1; s_hello_t = 0.4f;
    }
    poll();
    if (s_got_hello && s_peer_nonce == s_my_nonce) {   /* 1-in-65536 tie: re-roll */
        s_my_nonce ^= 0x5A5Au; s_got_hello = 0; send_hello();
    }
    if (s_got_hello && s_got_ident) {
        if (s_build_step == 0) s_build_step = 1;
        build_arena_step();
        if (s_build_step >= 8) { s_build_step = 0; return PVP_START; }
        s_build_step++;
        return PVP_WAIT;
    }
    return PVP_WAIT;
}

int pvp_arena_tick(const CraftRawButtons *btn, float dt) {
    static int prev_b;
    int b_edge = btn->b && !prev_b; prev_b = btn->b;

    poll();
    s_rx_age += dt;
    if (g_em && g_em->net_health && g_em->net_health() == MOTE_NET_LOST && s_end == END_NONE) s_end = END_LINKLOST;   /* v45 */

    if (s_end != END_NONE) {                    /* end card: world frozen, FX play */
        fx_tick(dt);
        if (b_edge) { send1('Q'); return PVP_EXIT; }
        return PVP_WAIT;
    }

    Ship *p = &g_ships[PLAYER];
    if (p->alive) {
        FlightInput in; elite_input_update(btn, dt, &in);
        flight_apply_input(&in, dt);
        if (in.secondary && p->n_weapons > 1)
            p->active_w = (uint8_t)((p->active_w + 1) % p->n_weapons);
        /* Fire the active weapon; detect an actual shot (cooldown reset) to
         * replicate the muzzle FX to the peer. RAILGUN charge-on-release is
         * treated as hold-fire here (kept simple for the duel). */
        s_prev_firecool = p->fire_cool;
        if (in.fire) combat_fire(PLAYER, 0.0f, PVP_REMOTE);
        if (p->fire_cool > s_prev_firecool + 1e-4f) {
            uint8_t wt = p->weapons[p->active_w];
            uint8_t m[3] = { 0xA5, 'F', wt };
            link_tx(m, 3);
        }
    }

    /* Advance the local sim: physics for both ships (the remote dead-reckons
     * on its replicated velocity, throttle 0 / assist off), projectiles +
     * shield regen + fx. NO ai_tick — the remote is never locally piloted. */
    Ship *r = &g_ships[PVP_REMOTE];
    if (r->alive) { r->assist = false; r->throttle = 0.0f; r->vel = s_rp_vel;
                    r->basis.r[2] = s_rp_fwd; r->basis.r[1] = s_rp_up;
                    m3_orthonormalize(&r->basis); }
    flight_tick(dt);
    combat_tick(dt);
    fx_tick(dt);
    if (r->alive) {                             /* smooth position toward the target */
        float k = dt * 8.0f; if (k > 1) k = 1;
        r->pos = v3_lerp(r->pos, s_rp_pos, k);
    }

    /* Stream my state (also the keepalive). */
    s_send_t -= dt;
    if (s_send_t <= 0) { state_send(); s_send_t = 1.0f / 18.0f; }

    /* My death -> tell the peer, show DEFEAT. */
    if (!p->alive && !s_death_sent) {
        send1('K'); s_death_sent = 1; s_end = END_DEFEAT;
    }
    return PVP_WAIT;
}

void pvp_report_damage(float dmg, int wtype) {
    if (!s_active || s_end != END_NONE) return;
    int d = (int)(dmg + 0.5f); if (d < 0) d = 0; if (d > 65535) d = 65535;
    uint8_t m[5] = { 0xA5, 'D', (uint8_t)d, (uint8_t)(d >> 8), (uint8_t)wtype };
    link_tx(m, 5);
}

void pvp_end(void) {
    if (s_active) send1('Q');
    if (g_em && g_em->link_stop) g_em->link_stop();
    s_active = 0; s_waiting = 0; s_end = END_NONE; s_msg_len = 0;
}

/* Build our outgoing identity before the arena exists (wait screen). Loads the
 * save so the peer sees the real ship; loadout stats need the live player
 * entity, so we apply into slot 0 here — harmless, build_arena redoes it. */
static void id_build_mine(void) {
    load_my_ship();
    player_apply_to_ship();
    id_snapshot();
}

/* ---- overlay ------------------------------------------------------------ */
static void ctext(uint16_t *fb, const char *s, int y, uint16_t c) {
    craft_font_draw(fb, s, (ELITE_FB_W - craft_font_width(s)) / 2, y, c);
}
static void ctext2x(uint16_t *fb, const char *s, int y, uint16_t c) {
    craft_font_draw_2x(fb, s, (ELITE_FB_W - craft_font_width_2x(s)) / 2, y, c);
}

void pvp_draw_overlay(uint16_t *fb) {
    if (s_waiting) {
        ctext2x(fb, "LINK ARENA", 28, RGB565C(150, 200, 255));
        int st = (g_em && g_em->link_status) ? g_em->link_status() : 0;
        if (st == MOTE_LINK_CONNECTED) {
            ctext(fb, "PEER FOUND - SYNCING", 60, RGB565C(120, 255, 140));
            /* sync self-diagnosis: exactly which leg is missing, on screen —
             * TX> hello sent · HELO peer hello rx'd · SHIP peer identity rx'd.
             * A stall names its missing piece instead of hanging mutely. */
            { char d[20]; int k = 0;
              d[k++]='T'; d[k++]='X'; d[k++]=s_sent_hello?'+':'-'; d[k++]=' ';
              d[k++]='H'; d[k++]='E'; d[k++]='L'; d[k++]=s_got_hello?'+':'-'; d[k++]=' ';
              d[k++]='S'; d[k++]='H'; d[k++]='P'; d[k++]=s_got_ident?'+':'-'; d[k]=0;
              ctext(fb, d, 74, RGB565C(150, 170, 200)); }
            { int nh = (g_em && g_em->net_health) ? g_em->net_health() : 0;
              if (nh == MOTE_NET_STALLED) ctext(fb, "LINK STALLED...", 86, RGB565C(255, 200, 120));
              else if (nh == MOTE_NET_LOST) ctext(fb, "PEER SILENT 20S+", 86, RGB565C(255, 120, 90)); }
            if (s_build_step > 0) {                 /* staged arena build: the frozen
                                                       number names a wedged step */
                char bs[16]; int k = 0;
                bs[k++]='A'; bs[k++]='R'; bs[k++]='E'; bs[k++]='N'; bs[k++]='A'; bs[k++]=' ';
                bs[k++]=(char)('0'+s_build_step); bs[k++]='/'; bs[k++]='8'; bs[k]=0;
                ctext(fb, bs, 98, RGB565C(150, 220, 255));
            }
        }
        else {
            ctext(fb, "CONNECT USB CABLE", 56, RGB565C(210, 214, 222));
            ctext(fb, "WAITING FOR PEER", 68, RGB565C(150, 160, 180));
        }
        ctext(fb, "B: CANCEL", 108, RGB565C(150, 160, 185));
        return;
    }
    if (!s_active) return;
    if (s_end == END_VICTORY) {
        ctext2x(fb, "VICTORY", 40, RGB565C(120, 255, 140));
        ctext(fb, "B: TITLE", 108, RGB565C(180, 200, 220));
    } else if (s_end == END_DEFEAT) {
        ctext2x(fb, "DEFEAT", 40, RGB565C(255, 110, 90));
        ctext(fb, "B: TITLE", 108, RGB565C(180, 200, 220));
    } else if (s_end == END_LINKLOST) {
        ctext2x(fb, "LINK LOST", 40, RGB565C(240, 200, 90));
        ctext(fb, "B: TITLE", 108, RGB565C(180, 200, 220));
    }
}
