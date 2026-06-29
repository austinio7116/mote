#include "rogue_player.h"
#include "rogue_render.h"
#include "rogue_platform.h"
#include "rogue_dmgnum.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

#define PLAYER_SPEED   5.2f
#define PLAYER_RADIUS  0.30f
#define PLAYER_H       1.15f   /* body height for collision */
#define GRAVITY        26.0f
#define JUMP_VEL       9.6f    /* ~2-block jump apex */
#define MAX_FALL       32.0f
#define FALL_SAFE      4.5f    /* fall height before damage */
#define ATK_HITFRAME   0.11f   /* time into the swing the blow lands */
#define ATK_RECOVER    0.16f

/* Hero palette */
#define C_TUNIC   RGB(40, 90, 200)
#define C_TUNIC_D RGB(28, 64, 150)
#define C_SKIN    RGB(225, 175, 140)
#define C_HAIR    RGB(90, 55, 30)
#define C_BOOT    RGB(70, 50, 35)
#define C_BELT    RGB(120, 90, 40)
#define C_BLADE   RGB(210, 215, 225)

/* Sword is the LAST part so we can pose it during a swing. */
enum { P_SWORD = 11 };
static const RogueCuboid hero_parts[] = {
    { -0.11f, 0.14f, 0.0f,  0.07f, 0.14f, 0.08f, C_BOOT  },
    {  0.11f, 0.14f, 0.0f,  0.07f, 0.14f, 0.08f, C_BOOT  },
    {  0.00f, 0.55f, 0.0f,  0.17f, 0.22f, 0.11f, C_TUNIC },
    {  0.00f, 0.36f, 0.0f,  0.18f, 0.05f, 0.12f, C_BELT  },
    {  0.00f, 0.55f, 0.10f, 0.10f, 0.18f, 0.02f, C_TUNIC_D },
    { -0.21f, 0.55f, 0.0f,  0.05f, 0.18f, 0.06f, C_TUNIC_D },
    {  0.21f, 0.55f, 0.0f,  0.05f, 0.18f, 0.06f, C_TUNIC_D },
    { -0.21f, 0.36f, 0.04f, 0.05f, 0.05f, 0.05f, C_SKIN },
    {  0.21f, 0.36f, 0.04f, 0.05f, 0.05f, 0.05f, C_SKIN },
    {  0.00f, 0.92f, 0.0f,  0.12f, 0.13f, 0.12f, C_SKIN },
    {  0.00f, 1.02f, 0.0f,  0.13f, 0.06f, 0.13f, C_HAIR },
    {  0.27f, 0.55f, 0.06f, 0.02f, 0.30f, 0.02f, C_BLADE },  /* sword */
};
#define HERO_NPARTS ((int)(sizeof(hero_parts)/sizeof(hero_parts[0])))

void rogue_player_init(RoguePlayer *p, Vec3 spawn) {
    p->pos = spawn;
    p->knock = v3(0, 0, 0);
    p->yaw = 0.0f;
    p->move_phase = 0.0f;
    p->walk_blend = 0.0f;
    p->max_hp = 100;
    p->hp = 100;
    p->alive = true;
    p->atk_t = p->atk_cd = 0.0f;
    p->atk_hit_done = false;
    p->atk_hit_pending = false;
    p->dodge_t = p->dodge_cd = 0.0f;
    p->dodge_dx = p->dodge_dz = 0.0f;
    p->vy = 0.0f;
    p->on_ground = true;
    p->peak_y = spawn.y;
    p->jumped = false;
    p->invuln_t = 0.0f;
    p->hurt_flash = 0.0f;
    p->fire_pending = false;
    p->gold = 0;
    p->torch_fuel = 75.0f;
    for (int i = 0; i < SLOT_COUNT; i++) p->equip[i].kind = ITEM_NONE;
    RogueItem starter;
    rogue_item_starter(&starter);
    p->equip[SLOT_WEAPON] = starter;
    rogue_player_recompute(p);
    p->hp = p->max_hp;
}

void rogue_player_recompute(RoguePlayer *p) {
    rogue_stats_compute(&p->stats, p->equip);
    const RogueItem *w = &p->equip[SLOT_WEAPON];
    p->wpn_class = w->wclass;
    p->wpn_type = w->wtype;
    p->wpn_range = w->range;
    p->wpn_arc_cos = w->arc_cos;
    p->wpn_proj_speed = w->proj_speed;
    /* attack speed shortens the swing/cooldown */
    float spd = 1.0f - p->stats.atk_spd * 0.01f;
    if (spd < 0.4f) spd = 0.4f;
    p->wpn_dur = w->cooldown * spd;
    /* effective damage = (base + flat) * (1 + dmg%) */
    int dmg = (w->base_dmg + p->stats.flat_dmg);
    dmg = dmg + dmg * p->stats.dmg_pct / 100;
    if (dmg < 1) dmg = 1;
    p->wpn_dmg = dmg;
    int old_max = p->max_hp;
    p->max_hp = p->stats.max_life;
    if (old_max > 0 && p->hp > p->max_hp) p->hp = p->max_hp;
}

void rogue_player_equip(RoguePlayer *p, const RogueItem *it) {
    if (!rogue_item_is_equip(it)) return;
    p->equip[it->slot] = *it;
    rogue_player_recompute(p);
}

static bool solid_cell(int x, int y, int z) {
    if (y < 0 || y >= CRAFT_WORLD_Y) return false;
    return craft_block_solid(craft_world_get(x, y, z));
}
/* Any solid cell in the footprint at integer height y? */
static bool foot_solid_y(float x, float z, int y) {
    float r = PLAYER_RADIUS;
    int x0 = (int)floorf(x - r), x1 = (int)floorf(x + r);
    int z0 = (int)floorf(z - r), z1 = (int)floorf(z + r);
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++)
            if (solid_cell(cx, y, cz)) return true;
    return false;
}
/* Solid anywhere in the body's vertical span standing at (x,z,feet)? */
static bool body_blocked(float x, float z, float feet) {
    int ylo = (int)floorf(feet + 0.15f);
    int yhi = (int)floorf(feet + PLAYER_H - 0.05f);
    for (int y = ylo; y <= yhi; y++)
        if (foot_solid_y(x, z, y)) return true;
    return false;
}
/* Top surface of the highest solid at/below `feet` under the footprint. */
static float ground_top(float x, float z, float feet) {
    for (int y = (int)floorf(feet + 0.01f); y >= 0; y--)
        if (foot_solid_y(x, z, y)) return (float)(y + 1);
    return 0.0f;
}
static void move_h(RoguePlayer *p, float mx, float mz, float step) {
    float nx = p->pos.x + mx * step;
    if (!body_blocked(nx, p->pos.z, p->pos.y)) p->pos.x = nx;
    float nz = p->pos.z + mz * step;
    if (!body_blocked(p->pos.x, nz, p->pos.y)) p->pos.z = nz;
}

void rogue_player_update(RoguePlayer *p, const CraftRawButtons *btn,
                         bool atk_edge, bool jump_edge,
                         float dt, float cam_yaw, int floor_y) {
    if (!p->alive) return;

    (void)floor_y;
    p->jumped = false;
    if (p->atk_cd   > 0) p->atk_cd   -= dt;
    if (p->invuln_t > 0) p->invuln_t -= dt;
    if (p->hurt_flash > 0) p->hurt_flash -= dt;

    float fx = sinf(cam_yaw), fz = cosf(cam_yaw);
    float rx = cosf(cam_yaw), rz = -sinf(cam_yaw);
    float mx = 0, mz = 0;
    if (btn->up)    { mx += fx; mz += fz; }
    if (btn->down)  { mx -= fx; mz -= fz; }
    if (btn->right) { mx += rx; mz += rz; }
    if (btn->left)  { mx -= rx; mz -= rz; }
    float len = sqrtf(mx*mx + mz*mz);
    if (len > 0.0001f) { mx /= len; mz /= len; }

    /* Horizontal: walk (slowed mid-swing, hastened by move-speed) + knockback. */
    float sp = PLAYER_SPEED * (1.0f + p->stats.move_spd * 0.01f) * (p->atk_t > 0 ? 0.4f : 1.0f);
    /* Wading through water slows you down. */
    if (craft_is_water_id((uint8_t)craft_world_get((int)floorf(p->pos.x),
                          (int)floorf(p->pos.y), (int)floorf(p->pos.z))))
        sp *= 0.55f;
    bool walking = (len > 0.0001f) && p->on_ground;
    if (len > 0.0001f) {
        if (p->atk_t <= 0) p->yaw = atan2f(mx, mz);
        if (p->on_ground) p->move_phase += dt * 9.0f;
        move_h(p, mx, mz, sp * dt);
    }
    /* Ease the walk animation in/out so starting/stopping isn't a snap. */
    float wb_target = walking ? 1.0f : 0.0f;
    float wb_rate = 12.0f * dt; if (wb_rate > 1.0f) wb_rate = 1.0f;
    p->walk_blend += (wb_target - p->walk_blend) * wb_rate;
    if (p->knock.x != 0 || p->knock.z != 0) {
        move_h(p, p->knock.x, p->knock.z, dt);
        float decay = 1.0f - 9.0f * dt; if (decay < 0) decay = 0;
        p->knock.x *= decay; p->knock.z *= decay;
        if (fabsf(p->knock.x) < 0.05f && fabsf(p->knock.z) < 0.05f) p->knock = v3(0,0,0);
    }

    /* Jump. */
    if (jump_edge && p->on_ground) {
        p->vy = JUMP_VEL;
        p->on_ground = false;
        p->jumped = true;
    }

    /* Gravity + vertical resolve. */
    p->vy -= GRAVITY * dt;
    if (p->vy < -MAX_FALL) p->vy = -MAX_FALL;
    float ny = p->pos.y + p->vy * dt;
    if (p->vy <= 0.0f) {
        float g = ground_top(p->pos.x, p->pos.z, p->pos.y);
        /* A moving platform can be the higher support. */
        float ptop, pdx, pdy, pdz; bool on_plat = false;
        if (rogue_platform_support(p->pos.x, p->pos.z, p->pos.y, &ptop, &pdx, &pdy, &pdz)
            && ptop > g) { g = ptop; on_plat = true; }
        if (ny <= g) {
            if (!p->on_ground) {
                float fall = p->peak_y - g;
                if (fall > FALL_SAFE)
                    rogue_player_damage(p, (int)((fall - FALL_SAFE) * 6.0f), p->pos);
            }
            p->pos.y = g; p->vy = 0.0f; p->on_ground = true;
            if (on_plat) {            /* ride: carry the platform's motion */
                move_h(p, pdx, pdz, 1.0f);
                p->pos.y += pdy;
            }
        } else {
            p->pos.y = ny; p->on_ground = false;
        }
    } else {
        /* Rising: stop at a ceiling. */
        if (foot_solid_y(p->pos.x, p->pos.z, (int)floorf(ny + PLAYER_H))) {
            p->vy = 0.0f;
        } else {
            p->pos.y = ny;
        }
        p->on_ground = false;
    }
    if (p->on_ground) p->peak_y = p->pos.y;
    else if (p->pos.y > p->peak_y) p->peak_y = p->pos.y;

    /* Start an attack? */
    if (atk_edge && p->atk_cd <= 0 && p->atk_t <= 0) {
        p->atk_t = p->wpn_dur;
        p->atk_hit_done = false;
    }

    /* Advance swing; raise the hit flag at the strike frame. */
    if (p->atk_t > 0) {
        p->atk_t -= dt;
        float elapsed = p->wpn_dur - p->atk_t;
        if (!p->atk_hit_done && elapsed >= ATK_HITFRAME) {
            p->atk_hit_done = true;
            if (p->wpn_class == WCLASS_MELEE) p->atk_hit_pending = true;
            else                              p->fire_pending = true;
        }
        if (p->atk_t <= 0) p->atk_cd = ATK_RECOVER;
    }
}

bool rogue_player_damage(RoguePlayer *p, int dmg, Vec3 from) {
    if (!p->alive || p->invuln_t > 0) return false;
    /* armor + resist mitigation */
    float red = rogue_stats_reduction(p->stats.armor);
    dmg = (int)(dmg * (1.0f - red) * (1.0f - p->stats.resist * 0.01f));
    if (dmg < 1) dmg = 1;
    p->hp -= dmg;
    rogue_dmgnum_spawn(p->pos, dmg, true);   /* red — damage you took */
    p->hurt_flash = 0.30f;
    p->invuln_t = 0.45f;   /* brief mercy i-frames after a hit */
    float dx = p->pos.x - from.x, dz = p->pos.z - from.z;
    float l = sqrtf(dx*dx + dz*dz);
    if (l > 0.001f) { p->knock.x = dx / l * 6.0f; p->knock.z = dz / l * 6.0f; }
    if (p->hp <= 0) { p->hp = 0; p->alive = false; }
    return true;
}

void rogue_player_draw(const RoguePlayer *p, const CraftCamera *cam,
                       uint16_t *fb, int tint_q8) {
    RogueCuboid parts[HERO_NPARTS];
    for (int i = 0; i < HERO_NPARTS; i++) parts[i] = hero_parts[i];

    /* Walk cycle: stride the legs (opposite phase), counter-swing the arms +
     * hands, and bob the upper body — scaled by walk_blend so it eases in and
     * out. Boots are parts 0/1, arms 5/6, hands 7/8; the upper body bobs while
     * the feet stay planted. */
    float wb = p->walk_blend;
    if (wb > 0.01f) {
        float sw  = sinf(p->move_phase);             /* left phase  */
        float swo = sinf(p->move_phase + (float)M_PI);/* right phase */
        float stride = 0.12f * wb;
        parts[0].cz += stride * sw;   parts[1].cz += stride * swo;   /* legs */
        if (sw  > 0) parts[0].cy += 0.03f * wb * sw;                 /* lift forward foot */
        if (swo > 0) parts[1].cy += 0.03f * wb * swo;
        parts[5].cz += stride * swo;  parts[7].cz += stride * swo;   /* left arm + hand */
        parts[6].cz += stride * sw;   parts[8].cz += stride * sw;    /* right arm + hand */
        /* upper-body bob (twice walk freq); keep boots planted */
        float bob = 0.035f * wb * (0.5f - 0.5f * cosf(p->move_phase * 2.0f));
        for (int i = 2; i < HERO_NPARTS; i++) parts[i].cy += bob;
    }

    /* Pose the weapon through the strike: a real SWEEP across the body for
     * swinging weapons (right→left arc, dipping at the middle), a straight
     * lunge for stabbing ones. */
    if (p->atk_t > 0) {
        float ph = 1.0f - (p->atk_t / p->wpn_dur);   /* 0..1 */
        float s = sinf(ph * (float)M_PI);             /* 0..1..0 */
        bool stab = (p->wpn_type == WT_DAGGER || p->wpn_type == WT_SPEAR);
        if (stab) {
            parts[P_SWORD].cz = 0.06f + 0.46f * s;    /* lunge forward */
            parts[P_SWORD].cy = 0.55f - 0.10f * s;
            parts[P_SWORD].hz = 0.02f + 0.20f * s;    /* lengthen toward target */
        } else {
            float ang = -1.0f + 2.0f * ph;            /* sweep right → left */
            parts[P_SWORD].cx = sinf(ang) * 0.36f;
            parts[P_SWORD].cz = 0.08f + cosf(ang) * 0.34f * s;
            parts[P_SWORD].cy = 0.62f - 0.14f * s;    /* dip through the middle */
            parts[P_SWORD].hy = 0.30f - 0.10f * s;    /* blade flattens into the cut */
            parts[P_SWORD].hz = 0.02f + 0.16f * s;
        }
    }

    float flash = (p->hurt_flash > 0) ? (p->hurt_flash / 0.30f) * 0.7f : 0.0f;
    /* Blink during i-frames (after a hit / while rolling). */
    if (p->invuln_t > 0 && ((int)(p->invuln_t * 30.0f) & 1)) return;

    /* Render ~18% larger than the hitbox (readability — matches the enemy
     * upscale), with a soft shadow slab anchoring the hero to the floor. */
    const float HSC = 1.18f;
    for (int i = 0; i < HERO_NPARTS; i++) {
        parts[i].cx *= HSC; parts[i].cy *= HSC; parts[i].cz *= HSC;
        parts[i].hx *= HSC; parts[i].hy *= HSC; parts[i].hz *= HSC;
    }
    {
        /* The shadow stays pinned to the GROUND under the hero (not pos.y —
         * it must not ride up during a jump), shrinking a touch with height. */
        float gy = ground_top(p->pos.x, p->pos.z, p->pos.y);
        float air = p->pos.y - gy;
        float ss = 1.0f - air * 0.25f;
        if (ss < 0.5f) ss = 0.5f;
        Vec3 sp = v3(p->pos.x, gy, p->pos.z);
        RogueCuboid hsh[1] = { { 0.0f, 0.02f, 0.0f, 0.34f * ss, 0.012f, 0.34f * ss, RGB(15,13,17) } };
        rogue_render_model(cam, fb, sp, 0.0f, hsh, 1, 0.40f, 0.06f, 0.0f, tint_q8);
    }
    rogue_render_model(cam, fb, p->pos, p->yaw, parts, HERO_NPARTS,
                       0.32f * HSC, 1.15f * HSC, flash, tint_q8);

    /* Readable melee swing FX, distinct per weapon type: thin quick stabs for
     * dagger/spear (a forward thrust line), a wide cyan crescent for swords, a
     * huge blue arc for the greatsword, a fiery cleave for the axe, and a
     * stubby blunt arc for mace/warhammer. (Ranged/caster show their
     * projectile instead.) */
    if (p->atk_t > 0 && p->wpn_class == WCLASS_MELEE) {
        float ph = 1.0f - (p->atk_t / p->wpn_dur);     /* 0..1 */
        if (ph < 0.8f) {
            uint16_t col = RGB(200,250,255);
            int   n = 5;
            float spread = 0.80f, seg = 0.07f, rmul = 1.0f;
            bool  thrust = false;
            switch (p->wpn_type) {
            case WT_DAGGER:     n=3; spread=0.30f; seg=0.05f; rmul=0.85f; thrust=true; col=RGB(225,235,255); break;
            case WT_SWORD:      n=5; spread=0.80f;                        col=RGB(200,250,255); break;
            case WT_GREATSWORD: n=7; spread=1.20f; seg=0.10f; rmul=1.20f; col=RGB(150,205,255); break;
            case WT_AXE:        n=6; spread=1.05f; seg=0.10f;             col=RGB(255,150,70);  break;
            case WT_MACE:       n=4; spread=0.55f; seg=0.11f; rmul=0.85f; col=RGB(225,225,215); break;
            case WT_SPEAR:      n=4; spread=0.10f; seg=0.06f; rmul=1.35f; thrust=true; col=RGB(220,232,245); break;
            case WT_WARHAMMER:  n=5; spread=0.70f; seg=0.12f; rmul=0.90f; col=RGB(212,202,182); break;
            default: break;
            }
            float r = p->wpn_range * 0.7f * rmul;
            RogueCuboid slash[26];
            int ns = 0;
            if (thrust) {
                /* Thrust: a solid lance of overlapping segments that stabs out
                 * and recovers, with a bright tip. */
                float reach = r * sinf(ph * (float)M_PI);
                int tn = n + 3;
                for (int k = 0; k < tn && ns < 24; k++) {
                    float d = reach * (k + 1) / tn;
                    float s2 = seg * (0.7f + 0.5f * k / tn);
                    slash[ns++] = (RogueCuboid){ 0.0f, 0.58f, 0.10f + d, s2, s2, s2 + 0.05f, col };
                }
                if (ns < 24)   /* white-hot tip */
                    slash[ns++] = (RogueCuboid){ 0.0f, 0.58f, 0.10f + reach, seg*1.2f, seg*1.2f, seg*1.2f, RGB(255,255,255) };
            } else {
                /* Sweeping crescent: a bright leading EDGE travels across the
                 * arc with a comet trail shrinking behind it — a solid slash
                 * ribbon, not dots. Two layers: coloured rim + white core. */
                float phN = ph / 0.8f;                       /* 0..1 over the visible swing */
                float lead = -spread + 2.2f * spread * phN;  /* leading edge angle */
                int   fan = 14;                              /* dense, overlapping */
                for (int k = 0; k < fan && ns < 24; k++) {
                    float a = -spread + (2.0f * spread) * k / (fan - 1);
                    if (a > lead) break;                     /* not swept yet */
                    float behind = (lead - a) / (spread * 1.6f);   /* 0 fresh .. 1 old */
                    if (behind > 1.0f) continue;             /* trail fully faded */
                    float s2 = seg * (2.8f - 1.5f * behind); /* fat edge, tapering tail */
                    float rr = r * (1.0f - 0.08f * behind);
                    slash[ns++] = (RogueCuboid){ sinf(a) * rr, 0.60f - 0.08f * behind,
                                                 cosf(a) * rr, s2, s2 * 0.6f, s2, col };
                    if (behind < 0.35f && ns < 24) {         /* white-hot core on the edge */
                        slash[ns++] = (RogueCuboid){ sinf(a) * rr * 0.84f, 0.62f,
                                                     cosf(a) * rr * 0.84f,
                                                     s2 * 0.65f, s2 * 0.4f, s2 * 0.65f,
                                                     RGB(255,255,255) };
                    }
                }
            }
            if (ns > 0)
                rogue_render_model(cam, fb, p->pos, p->yaw, slash, ns,
                                   r + 0.45f, 1.2f, 0.35f, 256);
        }
    }
}
