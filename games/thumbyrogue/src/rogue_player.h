#ifndef ROGUE_PLAYER_H
#define ROGUE_PLAYER_H
/*
 * ThumbyRogue hero — movement, melee attack arc, dodge-roll with i-frames,
 * damage/knockback, and a posed multi-cuboid render (the sword swings on
 * attack). Walks a flat dungeon floor (Y = floor surface).
 */
#include <stdint.h>
#include <stdbool.h>
#include "craft_types.h"
#include "craft_render.h"
#include "craft_buttons.h"
#include "rogue_items.h"
#include "rogue_stats.h"

typedef struct {
    Vec3  pos;            /* feet position */
    Vec3  knock;          /* knockback velocity (decays) */
    float yaw;            /* facing, radians (0 = +Z) */
    float move_phase;     /* walk-cycle accumulator */
    float walk_blend;     /* 0..1 eased in/out as you start/stop moving */
    int   hp, max_hp;
    bool  alive;

    /* platforming physics */
    float vy;             /* vertical velocity */
    bool  on_ground;
    float peak_y;         /* highest y this airtime, for fall damage */
    bool  jumped;         /* set the frame a jump launches (for SFX) */

    /* combat timers */
    float atk_t;          /* >0 while swinging */
    float atk_cd;         /* recovery before next swing */
    bool  atk_hit_done;   /* hit-frame already applied this swing */
    bool  atk_hit_pending;/* set on the hit frame; game consumes to deal arc dmg */
    float dodge_t;        /* >0 while rolling */
    float dodge_cd;
    float dodge_dx, dodge_dz;
    float invuln_t;       /* i-frames remaining */
    float hurt_flash;     /* red wash on taking damage */
    bool  fire_pending;   /* ranged/caster: launch a projectile this frame */

    /* 6-slot paperdoll; the WEAPON slot defines the attack playstyle. */
    RogueItem   equip[SLOT_COUNT];
    RogueStats  stats;
    WeaponClass wpn_class;
    uint8_t     wpn_type;     /* WeaponType — selects the swing/impact/projectile FX */
    float wpn_range, wpn_arc_cos, wpn_dur, wpn_proj_speed;
    int   wpn_dmg;        /* effective per-hit damage (base + stats) */
    int   gold;
    float torch_fuel;     /* seconds of light left; 0 = darkness */
} RoguePlayer;

void rogue_player_init(RoguePlayer *p, Vec3 spawn);
/* Equip an item into its slot (item->slot); recomputes stats. */
void rogue_player_equip(RoguePlayer *p, const RogueItem *it);
/* Recompute aggregated stats + derived weapon params after any gear change. */
void rogue_player_recompute(RoguePlayer *p);

/* atk_edge / jump_edge are just-pressed edges computed by the caller.
 * `floor_y` is unused now that the hero has full gravity/Y-collision, but
 * kept for call-site compatibility. */
void rogue_player_update(RoguePlayer *p, const CraftRawButtons *btn,
                         bool atk_edge, bool jump_edge,
                         float dt, float cam_yaw, int floor_y);

/* Apply damage from a world point (knockback away from it). Respects
 * i-frames. Returns true if the hit landed. */
bool rogue_player_damage(RoguePlayer *p, int dmg, Vec3 from);

void rogue_player_draw(const RoguePlayer *p, const CraftCamera *cam,
                       uint16_t *fb, int tint_q8);

#endif /* ROGUE_PLAYER_H */
