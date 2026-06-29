#ifndef ROGUE_ENEMY_H
#define ROGUE_ENEMY_H
/*
 * ThumbyRogue enemies — a fixed pool of cuboid creatures with simple but
 * readable AI: wander → chase → telegraphed wind-up → strike. Models are
 * adapted from ThumbyCraft's mob cuboids (the "start with the enemies we
 * already have" roster: rat, slime, skeleton, spider). Stats scale with
 * depth. Combat is fair: every hit is preceded by a visible wind-up.
 */
#include <stdint.h>
#include <stdbool.h>
#include "craft_types.h"
#include "craft_render.h"
#include "rogue_player.h"

#define ROGUE_MAX_ENEMIES 20

typedef enum {
    EN_RAT, EN_SLIME, EN_SKELETON, EN_SPIDER,
    EN_BAT, EN_KOBOLD, EN_GOBLIN, EN_ZOMBIE,
    EN_ARCHER, EN_FIRESPRITE, EN_DEMON,
    EN_SHOPKEEPER,   /* the merchant — peaceful until attacked, then a wizard */
    EN_TYPE_COUNT
} EnemyType;

/* What a slain enemy tends to drop (the game weights its loot roll by this). */
typedef enum { LOOT_GOLD, LOOT_GEAR, LOOT_GEM, LOOT_POTION, LOOT_RARE } EnemyLoot;
EnemyLoot rogue_enemy_loot(int type);
const char *rogue_enemy_name(int type);

void rogue_enemies_clear(void);
void rogue_enemies_set_dark(bool dark);   /* torch-out danger modifier */

/* Populate the level with depth-scaled enemies, avoiding the up-stairs
 * room. `rooms` are candidate room centres; `n` how many. */
void rogue_enemies_spawn(const int16_t *room_cx, const int16_t *room_cz,
                         int n_rooms, int up_x, int up_z,
                         int floor_y, int depth, uint32_t seed);

/* Advance AI + movement; apply telegraphed melee hits to the player. */
void rogue_enemies_update(RoguePlayer *p, float dt, int floor_y);

/* Element carried by the player's NEXT hits this tick (fire = burn bonus,
 * frost = chilling slow, poison = damage over time). Reset automatically at
 * the start of each enemy update so it never leaks into thorns/retaliation. */
void rogue_enemies_set_strike_element(int elem, int power);
/* Life drained by shadow-element hits since last call (heals the hero). */
int  rogue_enemies_take_drain(void);

/* Player melee: damage + knock every live enemy inside the facing arc.
 * Returns the number hit (for SFX/juice). */
int rogue_enemies_hit_arc(Vec3 origin, float yaw, float range,
                          float arc_cos, int dmg);

/* Point/projectile hit: damage the first live enemy within `radius` of
 * (x,z). Returns 1 if it hit something. */
int rogue_enemies_hit_point(float x, float z, float radius, int dmg);
/* AoE: damage ALL live enemies within `radius` (cleave / chain). */
int rogue_enemies_hit_radius(float x, float z, float radius, int dmg);

/* Drain one enemy death event (position + EnemyType). False when empty.
 * The game uses this to drop loot. */
bool rogue_enemies_pop_death(Vec3 *pos, int *type);

void rogue_enemies_draw(const CraftCamera *cam, uint16_t *fb);

int rogue_enemies_alive_count(void);

/* Nearest live enemy to (x,z); false if none. (Debug/autopilot helper.) */
bool rogue_enemies_nearest(float x, float z, float *ex, float *ez);

/* --- mid-level suspend snapshot --------------------------------------- *
 * Capture/restore the live enemy pool so quitting to the lobby and resuming
 * puts every creature back where it was (position + hp), not freshly spawned. */
typedef struct {
    uint8_t type, champ, calm;
    int16_t hp;
    float   x, y, z, yaw;
} RogueEnemySave;
int  rogue_enemies_export(RogueEnemySave *out, int max);   /* count of live enemies */
void rogue_enemies_import(const RogueEnemySave *in, int n);

/* The shopkeeper: spawned at the stall, peaceful (stands at his post,
 * untargeted by nothing — but any hit turns him into a blink-casting
 * battle wizard). state: 0 = none/dead, 1 = calm, 2 = hostile. */
void rogue_enemies_add_shopkeeper(float x, float y, float z, float yaw,
                                  bool calm, int depth);
int  rogue_enemies_shopkeeper_state(void);

/* Debug: pose one enemy for animation-sheet captures. */
void rogue_enemies_debug_showcase(int type, float x, float y, float z,
                                  float anim, int moving);

#endif /* ROGUE_ENEMY_H */
