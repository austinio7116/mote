/*
 * ThumbyElite — weapons fire, damage, death.
 */
#ifndef ELITE_COMBAT_H
#define ELITE_COMBAT_H

#include "elite_entity.h"
#include "vec.h"
#include <stdbool.h>

void combat_init(void);
void combat_tick(float dt);

/* Fire the shooter's ACTIVE weapon along its nose (+- spread radians).
 * target feeds homing seekers (-1 = none -> dumbfire). Returns the
 * entity hit for hitscan weapons, else -1. */
int combat_fire(int shooter, float spread, int target);
/* PC dedicated buttons: fire a specific player mount (slot) on its own
 * cooldown, independent of the active-weapon trigger. */
int combat_player_fire_slot(int slot, int target);

bool combat_can_fire(const Ship *s);

/* Damage entry points (also used by the projectile pool). */
int  combat_pkiller(void);
int  combat_pkiller_env(void);
void combat_note_env_hit(int kind);
void combat_finalize_kill(int shooter, int victim);
int player_turret_gunner_tier(void);
int turret_cal_for_seed(uint32_t seed);
void combat_direct_damage(int shooter, int victim, float dmg, Vec3 hit_pos);
void combat_explosion_damage(int shooter, Vec3 centre, float radius,
                             float dmg);

/* Stats / HUD feedback. */
int   combat_kills(void);
void  combat_set_kills(int n);
/* Instant per-kill bounty since last call (player kills only). */
int   combat_take_kill_pay(void);
void  combat_set_shot_type(int wt);   /* proj impact tagging */
void  combat_crit_cooldown_tick(float dt);
void  combat_set_player_target(int t);  /* auto-turret target feed */
float combat_hitmarker(void);
float combat_killmarker(void);

#endif
