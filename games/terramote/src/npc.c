/* TerraMote — enemies, boss, drops, projectiles, spawning. */
#include "terra.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Enemy animation clips — authored in anims/<name>.anims, editable in Studio's
 * Anim tab, driven here by a MoteAnimPlayer per enemy (like the player). */
#include "slime.anim.h"        /* slime_sheet · slime_idle / slime_jump */
#include "slime_blue.anim.h"
#include "slime_lava.anim.h"
#include "zombie.anim.h"       /* zombie_sheet · zombie_walk */
#include "skeleton.anim.h"     /* skeleton_sheet · skeleton_walk */
#include "eye.anim.h"          /* eye_sheet · eye_fly */
#include "bat.anim.h"          /* bat_sheet · bat_fly */
#include "eoc.anim.h"          /* eoc_sheet · eoc_p1 / eoc_p2 */

Enemy g_en[MAX_ENEMIES];
Drop  g_drops[MAX_DROPS];
Proj  g_proj[MAX_PROJ];

typedef struct {
    const MoteImage *img;
    uint8_t fw, fh;        /* frame size */
    uint8_t hw, hh;        /* half-extent hitbox (from center) */
    int16_t hp;
    uint8_t dmg;
    uint8_t coins;
} EnemyDef;

static const EnemyDef k_edef[E_COUNT] = {
    [E_SLIME_GREEN] = { 0, 16, 12, 6, 4, 14, 6, 1 },
    [E_SLIME_BLUE]  = { 0, 16, 12, 6, 4, 26, 8, 2 },
    [E_SLIME_LAVA]  = { 0, 16, 12, 6, 4, 45, 14, 3 },
    [E_ZOMBIE]      = { 0, 12, 16, 4, 7, 46, 13, 3 },
    [E_EYE]         = { 0, 16, 16, 6, 6, 40, 16, 3 },
    [E_BAT]         = { 0, 16, 10, 5, 3, 16, 11, 2 },
    [E_SKELETON]    = { 0, 12, 16, 4, 7, 70, 18, 5 },
    [E_BOSS_EOC]    = { 0, 40, 40, 14, 14, 1400, 15, 40 },
};
static const MoteAnimSheet *edef_sheet(uint8_t kind) {
    switch (kind) {
    case E_SLIME_GREEN: return &slime_sheet;
    case E_SLIME_BLUE:  return &slime_blue_sheet;
    case E_SLIME_LAVA:  return &slime_lava_sheet;
    case E_ZOMBIE:      return &zombie_sheet;
    case E_EYE:         return &eye_sheet;
    case E_BAT:         return &bat_sheet;
    case E_SKELETON:    return &skeleton_sheet;
    case E_BOSS_EOC:    return &eoc_sheet;
    }
    return &slime_sheet;
}

/* the contextually-correct clip for an enemy's current state */
static const MoteAnimClip *edef_clip(const Enemy *e) {
    switch (e->kind) {
    case E_SLIME_GREEN: return e->on_ground ? &slime_idle      : &slime_jump;
    case E_SLIME_BLUE:  return e->on_ground ? &slime_blue_idle : &slime_blue_jump;
    case E_SLIME_LAVA:  return e->on_ground ? &slime_lava_idle : &slime_lava_jump;
    case E_ZOMBIE:      return &zombie_walk;
    case E_SKELETON:    return &skeleton_walk;
    case E_EYE:         return &eye_fly;
    case E_BAT:         return &bat_fly;
    case E_BOSS_EOC:    return e->phase ? &eoc_p2 : &eoc_p1;
    }
    return &slime_idle;
}

static float s_spawn_t;
static int s_boss_idx = -1;

/* co-op: enemies chase / spawn around whichever player is CLOSER (the host
 * sims for both; solo this collapses to the local player) */
static void nearest_pos(float ex, float ey, float *ox, float *oy) {
    float ax = g_pl.x, ay = g_pl.y;
    float px, py;
    if (net_peer_pos(&px, &py)) {
        float a2 = (ax - ex) * (ax - ex) + ((ay - 8) - ey) * ((ay - 8) - ey);
        float b2 = (px - ex) * (px - ex) + ((py - 8) - ey) * ((py - 8) - ey);
        if (b2 < a2) { ax = px; ay = py; }
    }
    *ox = ax; *oy = ay;
}
static void nearest_pl(float ex, float ey, float *dx, float *dy) {
    float x, y;
    nearest_pos(ex, ey, &x, &y);
    *dx = x - ex; *dy = (y - 8) - ey;
}

void npc_clear_mobs(void) {
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (g_en[i].kind && g_en[i].kind != E_BOSS_EOC) g_en[i].kind = E_NONE;
}

void npc_reset(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) g_en[i].kind = E_NONE;
    for (int i = 0; i < MAX_DROPS; i++) g_drops[i].item = I_NONE;
    for (int i = 0; i < MAX_PROJ; i++) g_proj[i].kind = PR_NONE;
    s_boss_idx = -1;
    s_spawn_t = 2.0f;
}

/* ------------------------------------------------------------------ drops ----
 * In co-op every drop is HOST-owned: the guest's spawns become requests (its
 * world-op predictions suppress them entirely — the host's echo is the truth),
 * and every host spawn replicates to the guest's mirror slot. */
void drops_add_v(uint8_t item, int n, float x, float y, float vx, float vy) {
    if (!item || n <= 0) return;
    if (net_guest()) {
        if (!g_net_nodrops) net_ev_drop_req(item, n, x, y);
        return;
    }
    for (int i = 0; i < MAX_DROPS; i++) {
        if (g_drops[i].item) continue;
        g_drops[i] = (Drop){ item, (uint8_t)(n > 99 ? 99 : n), x, y, vx, vy, 0 };
        net_drop_spawned(i);
        return;
    }
}
void drops_add(uint8_t item, int n, float x, float y) {
    drops_add_v(item, n, x, y, mote_randf(-25, 25), -60.0f);
}

static void drops_tick(float dt) {
    int guest = net_guest();
    float qx = 0, qy = 0;
    int have_peer = net_peer_pos(&qx, &qy);
    for (int i = 0; i < MAX_DROPS; i++) {
        Drop *d = &g_drops[i];
        if (!d->item) continue;
        d->t += dt;
        /* magnet + collection track the NEAREST player (host arbitrates) */
        float px = g_pl.x, py = g_pl.y - 10;
        int peer_closer = 0;
        float dx = px - d->x, dy = py - d->y;
        float dist2 = dx * dx + dy * dy;
        if (have_peer) {
            float bx = qx - d->x, by = (qy - 10) - d->y;
            float b2 = bx * bx + by * by;
            if (b2 < dist2) { dx = bx; dy = by; dist2 = b2; peer_closer = 1; }
        }
        if (d->t > 1.1f && dist2 < 26 * 26) {            /* magnet (after a beat, so drops are SEEN) */
            float inv = 1.0f / sqrtf(dist2 + 1.0f);
            d->vx = dx * inv * 130.0f; d->vy = dy * inv * 130.0f;
        } else {
            d->vy += 480.0f * dt;
            if (d->vy > 220) d->vy = 220;
            if (world_solid_px((int)d->x, (int)(d->y + 4)))
                { d->vy = 0; d->vx *= 0.8f; }
            if (BG_LIQ(bg_at(px_c(d->x), (int)d->y / TILE)) >= 4) { d->vy *= 0.7f; d->vx *= 0.9f; }
        }
        float nx = d->x + d->vx * dt, ny2 = d->y + d->vy * dt;
        if (!world_solid_px((int)nx, (int)d->y)) d->x = nx;
        if (!world_solid_px((int)d->x, (int)ny2)) d->y = ny2;
        if (guest) continue;                             /* mirror only: host resolves pickups */
        if (d->t > 0.4f && dist2 < 9 * 9) {              /* collect */
            if (peer_closer) {                           /* the friend picked it up */
                net_drop_taken(i, 1);
                d->item = I_NONE;
                continue;
            }
            int left = inv_add(d->item, d->count);
            audio_sfx(d->item == I_COIN ? SFX_COIN : SFX_TICK, d->item == I_COIN ? 0.9f : 0.45f);
            if (left) d->count = (uint8_t)left;
            else { d->item = I_NONE; net_drop_taken(i, 0); }
        }
        if (d->t > 90.0f) { d->item = I_NONE; net_drop_taken(i, 0); }
    }
}

static void drops_draw(void) {
    for (int i = 0; i < MAX_DROPS; i++) {
        Drop *d = &g_drops[i];
        if (!d->item) continue;
        MoteSprite spr = {
            .img = g_items_sheet, .x = (int16_t)(d->x - 8), .y = (int16_t)(d->y - 8),
            .fx = (uint16_t)((d->item % 8) * 16), .fy = (uint16_t)((d->item / 8) * 16),
            .fw = 16, .fh = 16, .layer = 8, .flags = 0,   /* items.png is a 16px grid */
        };
        mote->scene2d_add(&spr);
    }
}

/* ------------------------------------------------------------- projectiles ---- */
void proj_add(uint8_t kind, float x, float y, float vx, float vy, int dmg, int hostile, uint8_t element) {
    for (int i = 0; i < MAX_PROJ; i++) {
        if (g_proj[i].kind) continue;
        g_proj[i] = (Proj){ kind, x, y, vx, vy, 0, (int16_t)dmg, (uint8_t)hostile, element, 0 };
        return;
    }
}

/* the FRIEND's arrow: flies and sparkles here, but the damage happens on the
 * shooter's own sim (melee/arrows are sender-authoritative via 'd') */
void proj_add_net(uint8_t kind, float x, float y, float vx, float vy, uint8_t element) {
    for (int i = 0; i < MAX_PROJ; i++) {
        if (g_proj[i].kind) continue;
        g_proj[i] = (Proj){ kind, x, y, vx, vy, 0, 0, 0, element, 1 };
        return;
    }
}

static void proj_tick(float dt) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &g_proj[i];
        if (!p->kind) continue;
        p->t += dt;
        p->vy += 240.0f * dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        if (p->t > 4.0f || world_solid_px((int)p->x, (int)p->y)) {
            if (!p->hostile && !p->net && (mote_rand() & 1))
                drops_add(p->kind == PR_ARROW_FLAME ? I_ARROW_FLAME : I_ARROW, 1, p->x - p->vx * dt, p->y - p->vy * dt);
            part_burst(p->x, p->y, rgb(180, 170, 150), 3, 30);
            p->kind = PR_NONE;
            continue;
        }
        if (p->kind == PR_ARROW_FLAME && (mote_rand() % 3) == 0)
            part_burst(p->x, p->y, rgb(255, 150, 40), 1, 8);
        else if (p->element && (mote_rand() % 2) == 0)          /* elemental arrow trail */
            part_element(p->x, p->y, p->element, 1, 12);
        if (p->net) continue;                                   /* peer's arrow: cosmetic */
        if (p->hostile) {
            if (fabsf(p->x - g_pl.x) < 5 && fabsf(p->y - (g_pl.y - 10)) < 11) {
                player_damage(p->dmg, p->vx > 0 ? 80.0f : -80.0f);
                p->kind = PR_NONE;
            }
        } else {
            for (int e = 0; e < MAX_ENEMIES; e++) {
                Enemy *en = &g_en[e];
                if (!en->kind) continue;
                const EnemyDef *ed = &k_edef[en->kind];
                if (fabsf(p->x - en->x) < ed->hw + 2 && fabsf(p->y - en->y) < ed->hh + 2) {
                    npc_damage_at(en->x, en->y, 1, 1, p->dmg, p->vx > 0 ? 90.0f : -90.0f,
                                  p->kind == PR_ARROW_FLAME ? EL_FIRE : p->element);
                    p->kind = PR_NONE;
                    break;
                }
            }
        }
    }
}

/* projectiles draw in screen space with smooth rotation (overlay path) */
void proj_draw(uint16_t *fb);
void proj_draw(uint16_t *fb) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &g_proj[i];
        if (!p->kind) continue;
        float ang = atan2f(p->vy, p->vx);             /* arrow art points right */
        int cell = p->kind == PR_ARROW_FLAME ? 8 : 7;
        mote->blit_ex(fb, g_ui_sheet, p->x - g_cam_x, p->y - g_cam_y,
                      (cell % 9) * 12, 0, 12, 12, ang, 1.0f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
    }
}

/* --------------------------------------------------------------- enemies ---- */
static int en_free(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) if (!g_en[i].kind) return i;
    return -1;
}
static int en_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) if (g_en[i].kind && g_en[i].kind != E_BOSS_EOC) n++;
    return n;
}

uint16_t element_color(uint8_t el) {
    switch (el) {
    case EL_FIRE:    return rgb(255, 140, 40);
    case EL_ICE:     return rgb(140, 220, 255);
    case EL_POISON:  return rgb(120, 220, 80);
    case EL_HOLY:    return rgb(255, 240, 170);
    case EL_DEMONIC: return rgb(190, 90, 240);
    case EL_ARCANE:  return rgb(230, 120, 255);
    case EL_BLOOD:   return rgb(220, 50, 60);
    case EL_NATURE:  return rgb(110, 210, 90);
    }
    return 0;
}

int npc_damage_at(float x, float y, float hw, float hh, int dmg, float kx, uint8_t element) {
    if (net_guest()) {
        /* enemies live on the host: flash the hit locally, send the strike up */
        int hits = 0;
        for (int i = 0; i < MAX_ENEMIES; i++) {
            Enemy *e = &g_en[i];
            if (!e->kind) continue;
            const EnemyDef *d = &k_edef[e->kind];
            if (fabsf(x - e->x) > hw + d->hw || fabsf(y - e->y) > hh + d->hh) continue;
            e->hurt_t = 0.15f;
            ftext_add(e->x, e->y - d->hh - 6, dmg, rgb(255, 200, 90));   /* optimistic number */
            if (element) part_element(e->x, e->y - 2, element, 6, 55);
            hits++;
        }
        net_ev_dmg(x, y, hw, hh, dmg, kx, element);
        return hits;
    }
    int hits = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g_en[i];
        if (!e->kind) continue;
        const EnemyDef *d = &k_edef[e->kind];
        if (fabsf(x - e->x) > hw + d->hw || fabsf(y - e->y) > hh + d->hh) continue;
        e->hp -= dmg;
        e->hurt_t = 0.15f;
        ftext_add(e->x, e->y - d->hh - 6, dmg, rgb(255, 200, 90));
        if (e->kind != E_BOSS_EOC) { e->vx = kx; e->vy = -70.0f; }
        /* on-hit elemental status + a flare that BEHAVES like the element */
        if (element) part_element(e->x, e->y - 2, element, 6, 55);
        switch (element) {
        case EL_FIRE:   e->dot_t = 3.0f; e->dot_dps = 5 + dmg / 4; e->status_el = EL_FIRE;   break;
        case EL_POISON: e->dot_t = 5.0f; e->dot_dps = 3 + dmg / 6; e->status_el = EL_POISON; break;
        case EL_BLOOD:  e->dot_t = 2.5f; e->dot_dps = 4 + dmg / 5; e->status_el = EL_BLOOD;  break;
        case EL_ICE:    e->slow_t = 2.5f; e->status_el = EL_ICE;    break;
        case EL_NATURE: e->slow_t = 1.5f; e->status_el = EL_NATURE; break;
        default: break;
        }
        hits++;
        if (e->hp <= 0) {
            const EnemyDef *ed = d;
            part_burst(e->x, e->y, rgb(180, 40, 50), 10, 70);
            audio_sfx(SFX_KILL, 0.9f);
            switch (e->kind) {
            case E_SLIME_GREEN: drops_add(I_GEL, 1 + (mote_rand() % 2), e->x, e->y); break;
            case E_SLIME_BLUE:  drops_add(I_GEL, 1 + (mote_rand() % 3), e->x, e->y); break;
            case E_SLIME_LAVA:  drops_add(I_GEL, 2, e->x, e->y); break;
            case E_ZOMBIE: if ((mote_rand() % 5) == 0) drops_add(I_LENS, 1, e->x, e->y); break;
            case E_EYE:    drops_add(I_LENS, 1 + (mote_rand() % 2), e->x, e->y); break;
            case E_SKELETON: if ((mote_rand() % 4) == 0) drops_add(I_POTION_HEAL, 1, e->x, e->y); break;
            case E_BOSS_EOC:
                drops_add(I_DEMONITE_ORE, 28 + (mote_rand() % 8), e->x, e->y);
                drops_add(I_LENS, 4, e->x - 10, e->y);
                drops_add(I_POTION_HEAL, 3, e->x + 10, e->y);
                drops_add(I_LIFE_CRYSTAL, 1, e->x, e->y - 10);
                g_boss_down = 1;
                s_boss_idx = -1;
                ui_toast("THE EYE OF CTHULHU IS DEFEATED");
                audio_sfx(SFX_ROAR, 1.0f);
                net_ev_boss_state(1);
                break;
            }
            if (ed->coins) drops_add(I_COIN, ed->coins + (int)(mote_rand() % (ed->coins + 1)), e->x, e->y - 4);
            if (e->kind != E_BOSS_EOC && (mote_rand() % 18) == 0) drops_add(I_POTION_HEAL, 1, e->x, e->y);
            e->kind = E_NONE;
        }
    }
    return hits;
}

/* grounded walker physics shared by slimes/zombies/skeletons */
static void walker_phys(Enemy *e, const EnemyDef *d, float dt) {
    e->vy += 620.0f * dt;
    if (e->vy > 260) e->vy = 260;
    if (BG_LIQ(bg_at(px_c(e->x), (int)e->y / TILE)) >= 4 && e->kind != E_SLIME_LAVA) e->vy *= 0.92f;
    float nx = e->x + e->vx * dt * (e->slow_t > 0 ? 0.45f : 1.0f);   /* ice/nature slow */
    float edge = nx + (e->vx > 0 ? d->hw : -d->hw);
    if (!world_solid_px((int)edge, (int)(e->y + d->hh - 2)) &&
        !world_solid_px((int)edge, (int)(e->y - d->hh + 2)))
        e->x = nx;
    else if (e->on_ground) {
        /* try a step-up, else jump/turn */
        if (!world_solid_px((int)edge, (int)(e->y - d->hh - 6)) &&
            (e->kind == E_ZOMBIE || e->kind == E_SKELETON))
            e->vy = -175.0f;
        else e->vx = 0;
    }
    float ny = e->y + e->vy * dt;
    e->on_ground = 0;
    if (e->vy >= 0) {
        if (world_solid_px((int)(e->x - d->hw + 1), (int)(ny + d->hh)) ||
            world_solid_px((int)(e->x + d->hw - 1), (int)(ny + d->hh))) {
            ny = (float)(((int)(ny + d->hh) / TILE) * TILE) - d->hh;
            e->vy = 0; e->on_ground = 1;
        }
    } else {
        if (world_solid_px((int)(e->x - d->hw + 1), (int)(ny - d->hh)) ||
            world_solid_px((int)(e->x + d->hw - 1), (int)(ny - d->hh))) {
            e->vy = 0; ny = e->y;
        }
    }
    e->y = ny;
}

static void en_spawn(uint8_t kind, float x, float y) {
    int i = en_free();
    if (i < 0) return;
    g_en[i] = (Enemy){ 0 };
    g_en[i].kind = kind;
    g_en[i].x = x; g_en[i].y = y;
    g_en[i].hp = k_edef[kind].hp;
    g_en[i].facing = x > g_pl.x ? -1 : 1;
    g_en[i].t = mote_randf(0, 1.0f);
    mote_anim_play(&g_en[i].anim, edef_clip(&g_en[i]));
}

void npc_spawn_boss(void) {
    int i = en_free();
    if (i < 0) { for (i = 0; i < MAX_ENEMIES; i++) if (g_en[i].kind != E_BOSS_EOC) { g_en[i].kind = E_NONE; break; } }
    en_spawn(E_BOSS_EOC, g_pl.x - 140, g_pl.y - 120);
    for (int k = 0; k < MAX_ENEMIES; k++) if (g_en[k].kind == E_BOSS_EOC) s_boss_idx = k;
    ui_toast("THE EYE OF CTHULHU HAS AWOKEN!");
    audio_sfx(SFX_ROAR, 1.0f);
    mote->rumble(0.8f, 400);
    net_ev_boss_state(0);
}

int npc_boss_hp(int *max);
int npc_boss_hp(int *max) {
    *max = k_edef[E_BOSS_EOC].hp;
    if (s_boss_idx >= 0 && g_en[s_boss_idx].kind == E_BOSS_EOC) return g_en[s_boss_idx].hp;
    return 0;
}

static void spawn_try(void) {
    /* the night RAMPS: dusk starts gentle (cap 3), deep night packs up to 8 */
    int cap = 3;                                   /* day: a sparse world */
    if (IS_NIGHT()) {
        float np = (g_time - 0.60f) / 0.40f;
        cap = 2 + (int)(np * 4.0f + 0.5f);         /* dusk 2 -> deep night 6 */
        if (cap > 6) cap = 6;
    }
    if (en_count() >= cap) return;
    /* co-op: half the spawns happen around the friend instead */
    float ax = g_pl.x, ay = g_pl.y;
    int peer_anchor = 0;
    {
        float px, py;
        if (net_peer_pos(&px, &py) && (mote_rand() & 1)) { ax = px; ay = py; peer_anchor = 1; }
    }
    int side = (mote_rand() & 1) ? 1 : -1;
    int pc = px_c(ax);
    int c = pc + side * (18 + (int)(mote_rand() % 8));
    if ((unsigned)c >= WCOLS - 2 || c < 2) return;
    int pr = (int)ay / TILE;
    int r = pr + (int)(mote_rand() % 13) - 6;
    if ((unsigned)r >= WROWS - 3) return;
    /* find a free 2-tall air spot near, with ground below (fliers skip the
     * floor requirement) */
    int ok = 0, air_ok = 0;
    for (int k = 0; k < 8; k++, r++) {
        if ((unsigned)r >= WROWS - 3) break;
        if (fg_at(c, r) == T_AIR && fg_at(c, r - 1) == T_AIR) {
            air_ok = 1;
            if (g_tiles[fg_at(c, r + 1)].solid == 1) { ok = 1; break; }
        }
    }
    if (!ok && !air_ok) return;
    float x = c * TILE + 4.0f, y = r * TILE + 4.0f;
    int depth = r;
    uint8_t kind = 0;
    int night = IS_NIGHT();
    if (depth >= ROW_HELL - 6) {
        kind = E_SLIME_LAVA;
    } else if (depth > 150) {
        kind = (mote_rand() % 3 == 0) ? E_BAT : ((mote_rand() & 1) ? E_SKELETON : E_SLIME_BLUE);
    } else if (depth > ROW_DIRT_END) {
        kind = (mote_rand() & 1) ? E_BAT : E_SLIME_BLUE;
    } else {
        /* surface-ish */
        if (BG_LIQ(bg_at(c, r)) >= 4) return;
        if (night) kind = (mote_rand() % 3 == 0) ? E_EYE : E_ZOMBIE;
        else kind = (mote_rand() % 4 == 0) ? E_SLIME_BLUE : E_SLIME_GREEN;
        /* day surface slimes only in the open (the light window only covers
         * OUR camera, so skip the check for spawns anchored on the friend) */
        if (!night && !peer_anchor && fx_light_at(c, r) < 6) kind = E_BAT;
    }
    if (!ok && kind != E_EYE && kind != E_BAT) return;   /* walkers need a floor */
    if (kind == E_EYE || kind == E_BAT) y -= 8;
    en_spawn(kind, x, y);
}

/* ---- guest side: enemies are PUPPETS driven by the host's 'E' snapshots ----
 * We smooth toward the reported positions, run animation locally, and decide
 * our own contact damage (victim-authoritative, matching the link convention). */
static float   s_ptx[MAX_ENEMIES], s_pty[MAX_ENEMIES];
static uint8_t s_seen[MAX_ENEMIES];

void npc_net_snapshot_begin(void) { memset(s_seen, 0, sizeof s_seen); }

void npc_net_apply(int idx, uint8_t kind, float x, float y, int hp, uint8_t flags) {
    if ((unsigned)idx >= MAX_ENEMIES || !kind || kind >= E_COUNT) return;
    Enemy *e = &g_en[idx];
    s_seen[idx] = 1;
    if (e->kind != kind) {
        memset(e, 0, sizeof *e);
        e->kind = kind;
        e->x = x; e->y = y;
        mote_anim_play(&e->anim, edef_clip(e));
        if (kind == E_BOSS_EOC) s_boss_idx = idx;            /* boss hp bar */
    }
    s_ptx[idx] = x; s_pty[idx] = y;
    if (fabsf(x - e->x) > 48 || fabsf(y - e->y) > 48) { e->x = x; e->y = y; }
    e->hp = (int16_t)hp;
    e->facing = (flags & 1) ? -1 : 1;
    e->on_ground = (uint8_t)((flags >> 1) & 1);
    e->phase = (uint8_t)((flags >> 2) & 1);
    if ((flags & 8) && e->hurt_t <= 0) e->hurt_t = 0.12f;
}

void npc_net_snapshot_end(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g_en[i];
        if (!e->kind || s_seen[i]) continue;
        /* gone on the host — a kill if it happened in our view */
        if (fabsf(e->x - g_pl.x) < 100 && fabsf(e->y - g_pl.y) < 100) {
            part_burst(e->x, e->y, rgb(180, 40, 50), 10, 70);
            audio_sfx(SFX_KILL, 0.7f);
        }
        if (e->kind == E_BOSS_EOC) s_boss_idx = -1;
        e->kind = E_NONE;
    }
}

static void npc_guest_tick(float dt) {
    drops_tick(dt);                    /* mirror physics; the host resolves pickups */
    proj_tick(dt);                     /* own arrows: hits go up as 'd' events */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g_en[i];
        if (!e->kind) continue;
        const EnemyDef *d = &k_edef[e->kind];
        if (e->hurt_t > 0) e->hurt_t -= dt;
        float k = mote_clampf(dt * 10.0f, 0, 1);
        e->x += (s_ptx[i] - e->x) * k;
        e->y += (s_pty[i] - e->y) * k;
        const MoteAnimClip *clip = edef_clip(e);
        if (e->anim.clip != clip) mote_anim_play(&e->anim, clip);
        mote_anim_tick(&e->anim, dt);
        /* contact damage: OUR device decides when WE get hit */
        if (fabsf(e->x - g_pl.x) < d->hw + 4 && fabsf(e->y - (g_pl.y - 8)) < d->hh + 7)
            player_damage(d->dmg, (g_pl.x > e->x ? 1 : -1) * 120.0f);
    }
}

void npc_tick(float dt) {
    /* dev metrics: TERRA_DBG=1 logs boss hp + enemy count once a second */
    static float s_dbg_t; static int s_dbg_on = -1;
    if (s_dbg_on < 0) s_dbg_on = getenv("TERRA_DBG") != 0;
    if (s_dbg_on) {
        s_dbg_t += dt;
        if (s_dbg_t > 1.0f) {
            s_dbg_t = 0;
            char b[80]; int bm, bh = npc_boss_hp(&bm);
            snprintf(b, 80, "dbg t=%.2f en=%d boss=%d php=%d py=%d grap=%d rl=%d",
                     (double)g_time, en_count(), bh, g_pl.hp, (int)g_pl.y, g_pl.grap, (int)g_pl.grap_len);
            mote->log(b);
        }
    }
    if (net_guest()) { npc_guest_tick(dt); return; }
    s_spawn_t -= dt;
    if (s_spawn_t <= 0) {
        /* spawn cadence ramps through the night: 2.6s at dusk -> 1.2s deep night */
        float np = IS_NIGHT() ? (g_time - 0.60f) / 0.40f : 0.0f;
        s_spawn_t = IS_NIGHT() ? (3.4f - 1.6f * np) : 2.4f;   /* longer breathers */
        spawn_try();
    }
    drops_tick(dt);
    proj_tick(dt);

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g_en[i];
        if (!e->kind) continue;
        const EnemyDef *d = &k_edef[e->kind];
        if (e->hurt_t > 0) e->hurt_t -= dt;
        /* elemental status: damage-over-time in discrete ticks, slow decays */
        if (e->dot_t > 0) {
            float prev = e->dot_t; e->dot_t -= dt;
            if (floorf(prev * 2.0f) != floorf(e->dot_t * 2.0f)) {   /* twice a second */
                int dd = e->dot_dps / 2; if (dd < 1) dd = 1;
                npc_damage_at(e->x, e->y, 0.1f, 0.1f, dd, 0, EL_NONE);
                part_burst(e->x, e->y, e->status_el == EL_POISON ? rgb(120, 220, 80)
                                     : e->status_el == EL_BLOOD ? rgb(200, 40, 60) : rgb(255, 140, 40), 2, 20);
                if (!e->kind) continue;                              /* died to the tick */
            }
        }
        if (e->slow_t > 0) {
            e->slow_t -= dt;
            if ((mote_rand() % 8) == 0)
                part_burst(e->x, e->y, e->status_el == EL_NATURE ? rgb(120, 220, 80) : rgb(150, 220, 255), 1, 10);
        }
        if (e->dot_t <= 0 && e->slow_t <= 0) e->status_el = EL_NONE;
        float dx, dy;
        nearest_pl(e->x, e->y, &dx, &dy);
        float dist = sqrtf(dx * dx + dy * dy) + 0.01f;

        /* despawn far away (never on screen) */
        if (e->kind != E_BOSS_EOC && dist > 340.0f) { e->kind = E_NONE; continue; }

        switch (e->kind) {
        case E_SLIME_GREEN: case E_SLIME_BLUE: case E_SLIME_LAVA:
            e->t += dt;
            if (e->on_ground) {
                e->vx = 0;
                float wait = e->kind == E_SLIME_LAVA ? 1.0f : 1.5f;
                if (e->t > wait && dist < 190) {
                    e->t = 0;
                    e->facing = dx > 0 ? 1 : -1;
                    e->vx = e->facing * (35.0f + (e->kind != E_SLIME_GREEN ? 14.0f : 0));
                    e->vy = -160.0f;
                }
            }
            walker_phys(e, d, dt);
            break;
        case E_ZOMBIE: case E_SKELETON: {
            float sp = e->kind == E_SKELETON ? 33.0f : 26.0f;
            e->facing = dx > 0 ? 1 : -1;
            e->vx = e->facing * sp;
            walker_phys(e, d, dt);
            e->t += dt;
            /* zombies dig in at dawn: despawn off-screen */
            if (e->kind == E_ZOMBIE && !IS_NIGHT() && dist > 150) e->kind = E_NONE;
            break;
        }
        case E_EYE: case E_BAT: {
            float sp = e->kind == E_EYE ? 55.0f : 42.0f;
            float ax = dx / dist * sp * 2.2f, ay = dy / dist * sp * 2.2f;
            if (e->kind == E_BAT) {
                ax += mote_randf(-140, 140); ay += mote_randf(-140, 140);
            } else {
                ay += sinf(e->t * 5.0f) * 60.0f;
            }
            e->t += dt;
            e->vx += ax * dt; e->vy += ay * dt;
            float vmax = sp + 25;
            float v = sqrtf(e->vx * e->vx + e->vy * e->vy);
            if (v > vmax) { e->vx = e->vx / v * vmax; e->vy = e->vy / v * vmax; }
            float nx = e->x + e->vx * dt, ny = e->y + e->vy * dt;
            if (world_solid_px((int)(nx + (e->vx > 0 ? d->hw : -d->hw)), (int)e->y)) e->vx = -e->vx * 0.7f;
            else e->x = nx;
            if (world_solid_px((int)e->x, (int)(ny + (e->vy > 0 ? d->hh : -d->hh)))) e->vy = -e->vy * 0.7f;
            else e->y = ny;
            e->facing = e->vx > 0 ? 1 : -1;
            if (e->kind == E_EYE && !IS_NIGHT() && dist > 150) e->kind = E_NONE;
            break;
        }
        case E_BOSS_EOC: {
            int ph2 = e->hp < (int)(k_edef[E_BOSS_EOC].hp * 0.4f);
            e->phase = (uint8_t)ph2;
            e->t += dt;
            float cycle = ph2 ? 2.2f : 3.2f;
            float tt = fmodf(e->t, cycle);
            if (tt < cycle - 1.1f) {
                /* hover above-left/right of the (nearest) player, keeping its distance */
                float txp, typ;
                nearest_pos(e->x, e->y, &txp, &typ);
                float hx = txp + (e->x > txp ? 95 : -95);
                float hy = typ - 85;
                e->vx += (hx - e->x) * 4.5f * dt; e->vy += (hy - e->y) * 4.5f * dt;
                e->vx *= 0.94f; e->vy *= 0.94f;
            } else if (tt - dt < cycle - 1.1f) {
                /* start a charge at the player */
                float sp = ph2 ? 250.0f : 190.0f;
                e->vx = dx / dist * sp; e->vy = dy / dist * sp;
                audio_sfx(SFX_ROAR, 0.5f);
            }
            e->x += e->vx * dt; e->y += e->vy * dt;
            if (e->y < 8) e->y = 8;
            /* flees at dawn */
            if (!IS_NIGHT()) {
                e->vy = -160; e->y += e->vy * dt;
                if (e->t > 6.0f || e->y < -80) { e->kind = E_NONE; s_boss_idx = -1; ui_toast("THE EYE FLEES..."); net_ev_boss_state(2); }
            }
            break;
        }
        }

        /* advance animation (switch clip when the enemy's state changes) */
        const MoteAnimClip *clip = edef_clip(e);
        if (e->anim.clip != clip) mote_anim_play(&e->anim, clip);
        mote_anim_tick(&e->anim, dt);

        /* contact damage */
        if (fabsf(e->x - g_pl.x) < d->hw + 4 && fabsf(e->y - (g_pl.y - 8)) < d->hh + 7) {
            player_damage(d->dmg, (g_pl.x > e->x ? 1 : -1) * 120.0f);
        }
        /* lava hurts non-lava enemies */
        if (e->kind != E_SLIME_LAVA && e->kind != E_BOSS_EOC &&
            BG_IS_LAVA(bg_at(px_c(e->x), (int)e->y / TILE)) && BG_LIQ(bg_at(px_c(e->x), (int)e->y / TILE)) >= 3) {
            npc_damage_at(e->x, e->y, 1, 1, 12, 0, EL_NONE);
        }
    }
}

void npc_draw(void) {
    drops_draw();                         /* item drops render with the scene */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g_en[i];
        if (!e->kind) continue;
        const EnemyDef *d = &k_edef[e->kind];
        const MoteAnimSheet *sh = edef_sheet(e->kind);
        MoteSprite s = {
            .img = sh->image,
            .x = (int16_t)(e->x - d->fw / 2),
            .y = (int16_t)(e->y - d->fh / 2),
            .fx = (uint16_t)mote_anim_fx(&e->anim, sh),
            .fy = (uint16_t)mote_anim_fy(&e->anim, sh),
            .fw = d->fw, .fh = d->fh,
            .layer = 9,
            .flags = e->facing < 0 ? 0 : MOTE_SPR_HFLIP,   /* art faces LEFT for eye/eoc */
        };
        /* humanoid + slime art faces RIGHT: flip the other way */
        if (e->kind != E_EYE && e->kind != E_BOSS_EOC)
            s.flags = e->facing < 0 ? MOTE_SPR_HFLIP : 0;
        if (e->hurt_t > 0 && ((int)(e->hurt_t * 30) & 1)) s.y -= 1;   /* hit shake */
        mote->scene2d_add(&s);
    }
}
