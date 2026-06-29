#ifndef ROGUE_PROJ_H
#define ROGUE_PROJ_H
/*
 * ThumbyRogue projectiles — one distinct look per ranged/caster weapon type
 * (arrow, bolt, wand spark, scepter mote, staff orb). A small pool; each flies
 * straight, damages the first enemy it touches, and dies on walls or range.
 */
#include <stdint.h>
#include "craft_types.h"
#include "craft_render.h"

/* Visual style of a player projectile — one per ranged/caster weapon type. */
typedef enum {
    PROJ_ARROW,    /* bow — thin gold-wood streak */
    PROJ_BOLT,     /* crossbow — heavy steel bolt */
    PROJ_WAND,     /* wand — small fast cyan spark */
    PROJ_SCEPTER,  /* scepter — golden holy mote */
    PROJ_STAFF,    /* staff — big purple arcane orb */
    PROJ_KIND_COUNT
} ProjKind;

void rogue_proj_clear(void);
void rogue_proj_fire(Vec3 pos, float yaw, float speed, int dmg,
                     int kind, float max_range, int pierce,
                     int elem, int elem_pow);   /* ElementId tint + on-hit effect */
void rogue_proj_update(float dt, int floor_y);
void rogue_proj_draw(const CraftCamera *cam, uint16_t *fb);

#endif /* ROGUE_PROJ_H */
