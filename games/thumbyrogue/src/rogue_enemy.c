#include "rogue_enemy.h"
#include "rogue_render.h"
#include "rogue_band.h"
#include "rogue_dmgnum.h"
#include "rogue_particle.h"
#include <stdio.h>
#include "craft_world.h"
#include "craft_blocks.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

typedef enum { AI_WANDER, AI_CHASE, AI_WINDUP, AI_STRIKE } AIState;

typedef struct {
    bool      alive;
    EnemyType type;
    Vec3      pos;
    float     yaw;
    int       hp;
    AIState   state;
    float     state_t;     /* time in current state */
    float     wander_dx, wander_dz;
    float     hurt_flash;
    float     atk_cd;
    bool      champion;     /* band-end mini-boss: bigger + tougher */
    float     anim;         /* steady per-enemy clock — gait cycles + limb swing */
    float     sp;           /* per-type special timer (demon charge, strafe flips) */
    bool      moving;       /* displaced this tick → drive the walk animation */
    bool      calm;         /* shopkeeper at his post — no AI until provoked */
    float     slow_t;       /* frost chill — movement at 45% while > 0 */
    float     dot_t, dot_tick;  /* poison — ticking damage while dot_t > 0 */
    int       dot_pow;      /* total poison damage left to deal */
    float     lava_tick;    /* burn cadence while standing in lava */
} Enemy;

static Enemy s_en[ROGUE_MAX_ENEMIES];

/* Depth-scaled DAMAGE (set at spawn). HP always scaled with depth but damage
 * never did — past the first floors enemies tickled while your weapon kept
 * growing. Now they hit ~50% harder from depth 1 and keep climbing at the
 * same rate as their HP, so armor and lifesteal stay relevant all the way
 * down. */
static float s_dmg_scale = 1.5f;

/* Element on the player's current strike (see header). */
static int s_strike_elem, s_strike_pow;
void rogue_enemies_set_strike_element(int elem, int power) {
    s_strike_elem = elem; s_strike_pow = power;
}
/* Shadow drain accumulator — the game harvests this once per frame and
 * heals the hero by it. */
static int s_drain_heal;
int rogue_enemies_take_drain(void) { int h = s_drain_heal; s_drain_heal = 0; return h; }

/* Death-event ring: combat records where/what died so the game can drop
 * loot without the enemy module knowing about items. */
static Vec3      s_death_pos[ROGUE_MAX_ENEMIES];
static EnemyType s_death_type[ROGUE_MAX_ENEMIES];
static int       s_death_n;

/* Enemy projectiles (skeleton archers, fire sprites). */
typedef struct { bool alive; Vec3 pos; float vx, vz, life; int dmg; uint16_t col; } EShot;
#define MAX_ESHOT 12
static EShot s_eshot[MAX_ESHOT];
static void eshot_fire(Vec3 from, float tx, float tz, int dmg, uint16_t col) {
    float dx = tx - from.x, dz = tz - from.z;
    float l = sqrtf(dx*dx + dz*dz); if (l < 0.001f) l = 1.0f;
    for (int i = 0; i < MAX_ESHOT; i++) {
        if (s_eshot[i].alive) continue;
        EShot *s = &s_eshot[i];
        s->alive = true; s->pos = from; s->pos.y += 0.6f;
        s->vx = dx/l * 9.0f; s->vz = dz/l * 9.0f;
        s->life = 2.0f; s->dmg = dmg; s->col = col;
        return;
    }
}

/* --- per-type stats ---------------------------------------------- */
typedef struct {
    float speed, aggro, atk_range, windup, dmg_mul;
    int   base_hp, base_dmg;
    float radius, height;
    uint8_t ranged, flyer, loot;
} EnemyDef;

static const EnemyDef DEFS[EN_TYPE_COUNT] = {
    /* speed aggro range windup dmgmul hp  dmg radius height  rng fly loot */
    [EN_RAT]       = { 3.6f, 7.0f,  0.9f, 0.30f, 1.0f, 14,  5, 0.30f, 0.45f, 0,0, LOOT_GOLD   },
    [EN_SLIME]     = { 2.4f, 8.0f,  1.0f, 0.45f, 1.0f, 22,  7, 0.42f, 0.55f, 0,0, LOOT_POTION },
    [EN_SKELETON]  = { 3.0f, 11.f,  1.3f, 0.55f, 1.2f, 30, 10, 0.34f, 1.55f, 0,0, LOOT_GEAR   },
    [EN_SPIDER]    = { 4.2f, 10.f,  1.1f, 0.35f, 1.0f, 20,  8, 0.50f, 0.45f, 0,0, LOOT_GOLD   },
    [EN_BAT]       = { 5.2f, 9.0f,  0.8f, 0.20f, 0.8f, 10,  5, 0.28f, 0.40f, 0,1, LOOT_GOLD   },
    [EN_KOBOLD]    = { 4.0f, 10.f,  1.0f, 0.30f, 1.0f, 16,  7, 0.28f, 0.85f, 0,0, LOOT_GEAR   },
    [EN_GOBLIN]    = { 3.4f, 11.f,  1.2f, 0.40f, 1.1f, 34, 12, 0.32f, 1.05f, 0,0, LOOT_GEAR   },
    [EN_ZOMBIE]    = { 1.8f, 9.0f,  1.1f, 0.55f, 1.2f, 52, 14, 0.34f, 1.45f, 0,0, LOOT_POTION },
    [EN_ARCHER]    = { 2.6f, 13.f,  8.5f, 0.55f, 1.0f, 26,  9, 0.32f, 1.50f, 1,0, LOOT_GEAR   },
    [EN_FIRESPRITE]= { 3.0f, 11.f,  8.0f, 0.45f, 1.0f, 18, 10, 0.30f, 0.60f, 1,1, LOOT_GEM    },
    [EN_DEMON]     = { 2.9f, 12.f,  1.5f, 0.55f, 1.3f, 84, 20, 0.58f, 1.85f, 0,0, LOOT_RARE   },
    /* The merchant turned battle wizard: tanky, hard-hitting triple spell
     * bolts, and he BLINKS away when cornered. Provoke at your peril. */
    [EN_SHOPKEEPER]= { 3.2f, 14.f,  7.5f, 0.50f, 1.2f,110, 15, 0.34f, 1.45f, 1,0, LOOT_RARE   },
};
static const char *NAMES[EN_TYPE_COUNT] = {
    "Rat","Slime","Skeleton","Spider","Bat","Kobold","Goblin","Zombie",
    "Skeleton Archer","Fire Sprite","Demon","Shopkeeper"
};
EnemyLoot rogue_enemy_loot(int t){ return (t>=0&&t<EN_TYPE_COUNT)?(EnemyLoot)DEFS[t].loot:LOOT_GOLD; }
const char *rogue_enemy_name(int t){ return (t>=0&&t<EN_TYPE_COUNT)?NAMES[t]:"?"; }

/* --- cuboid models (feet at y=0, face +Z) ------------------------ */
static const RogueCuboid M_RAT[] = {
    { 0.0f, 0.16f, 0.0f, 0.18f, 0.12f, 0.26f, RGB(135,108,88) },   /* body */
    { 0.0f, 0.20f, 0.28f, 0.10f, 0.09f, 0.10f, RGB(160,128,104) }, /* head */
    { 0.0f, 0.14f,-0.30f, 0.02f, 0.02f, 0.12f, RGB(200,170,155) }, /* tail */
    {-0.13f,0.22f,0.30f, 0.04f,0.05f,0.02f, RGB(90,70,60) },       /* ears */
    { 0.13f,0.22f,0.30f, 0.04f,0.05f,0.02f, RGB(90,70,60) },
};
static const RogueCuboid M_SLIME[] = {
    { 0.0f, 0.22f, 0.0f, 0.38f, 0.22f, 0.38f, RGB(70,200,90) },    /* blob */
    {-0.14f,0.30f,0.32f, 0.05f,0.05f,0.03f, RGB(20,40,20) },       /* eye */
    { 0.14f,0.30f,0.32f, 0.05f,0.05f,0.03f, RGB(20,40,20) },
    { 0.0f, 0.12f,0.34f, 0.16f,0.02f,0.02f, RGB(20,40,20) },       /* mouth */
};
static const RogueCuboid M_SKELETON[] = {
    { 0.0f, 1.40f, 0.0f, 0.14f, 0.13f, 0.13f, RGB(220,220,205) },  /* skull */
    {-0.07f,1.44f,0.11f, 0.04f,0.04f,0.03f, RGB(20,20,20) },       /* eyes */
    { 0.07f,1.44f,0.11f, 0.04f,0.04f,0.03f, RGB(20,20,20) },
    { 0.0f, 1.05f, 0.0f, 0.15f, 0.22f, 0.08f, RGB(200,200,185) },  /* ribcage */
    { 0.0f, 1.02f, 0.10f,0.10f, 0.16f, 0.02f, RGB(160,160,148) },  /* sternum */
    {-0.20f,0.95f, 0.0f, 0.04f, 0.26f, 0.04f, RGB(210,210,195) },  /* arms */
    { 0.20f,0.95f, 0.0f, 0.04f, 0.26f, 0.04f, RGB(210,210,195) },
    {-0.08f,0.32f, 0.0f, 0.05f, 0.32f, 0.05f, RGB(210,210,195) },  /* legs */
    { 0.08f,0.32f, 0.0f, 0.05f, 0.32f, 0.05f, RGB(210,210,195) },
};
static const RogueCuboid M_SPIDER[] = {
    { 0.0f, 0.20f, -0.05f,0.26f, 0.16f, 0.30f, RGB(78,54,95) },    /* abdomen */
    { 0.0f, 0.20f, 0.32f, 0.16f, 0.13f, 0.16f, RGB(95,68,112) },   /* head */
    {-0.10f,0.30f,0.42f, 0.04f,0.04f,0.03f, RGB(235,50,50) },      /* eyes */
    { 0.10f,0.30f,0.42f, 0.04f,0.04f,0.03f, RGB(235,50,50) },
    {-0.34f,0.16f,0.10f, 0.18f,0.03f,0.03f, RGB(60,42,74) },       /* legs */
    { 0.34f,0.16f,0.10f, 0.18f,0.03f,0.03f, RGB(60,42,74) },
    {-0.34f,0.16f,-0.20f,0.18f,0.03f,0.03f, RGB(60,42,74) },
    { 0.34f,0.16f,-0.20f,0.18f,0.03f,0.03f, RGB(60,42,74) },
};
static const RogueCuboid M_BAT[] = {
    { 0.0f, 0.50f, 0.0f, 0.10f, 0.09f, 0.13f, RGB(96,72,118) },    /* body (hovers) */
    {-0.26f,0.52f, 0.0f, 0.16f, 0.02f, 0.10f, RGB(70,52,92) },     /* wings */
    { 0.26f,0.52f, 0.0f, 0.16f, 0.02f, 0.10f, RGB(70,52,92) },
    {-0.05f,0.55f,0.11f, 0.03f,0.03f,0.02f, RGB(235,70,70) },      /* eyes */
    { 0.05f,0.55f,0.11f, 0.03f,0.03f,0.02f, RGB(235,70,70) },
};
static const RogueCuboid M_KOBOLD[] = {
    { 0.0f, 0.35f, 0.0f, 0.13f, 0.18f, 0.10f, RGB(120,150,90) },   /* body */
    { 0.0f, 0.60f, 0.04f,0.10f, 0.09f, 0.10f, RGB(140,170,100) },  /* head */
    { 0.0f, 0.62f, 0.18f,0.05f, 0.04f, 0.05f, RGB(150,180,110) },  /* snout */
    {-0.09f,0.74f, 0.0f, 0.03f, 0.06f, 0.02f, RGB(110,140,80) },   /* ears */
    { 0.09f,0.74f, 0.0f, 0.03f, 0.06f, 0.02f, RGB(110,140,80) },
    {-0.07f,0.10f, 0.0f, 0.04f, 0.10f, 0.05f, RGB(90,120,70) },    /* legs */
    { 0.07f,0.10f, 0.0f, 0.04f, 0.10f, 0.05f, RGB(90,120,70) },
    { 0.16f,0.45f, 0.0f, 0.02f, 0.26f, 0.02f, RGB(150,120,80) },   /* spear */
};
static const RogueCuboid M_GOBLIN[] = {
    { 0.0f, 0.55f, 0.0f, 0.17f, 0.20f, 0.12f, RGB(80,140,70) },    /* body */
    { 0.0f, 0.82f, 0.02f,0.13f, 0.11f, 0.12f, RGB(95,160,80) },    /* head */
    {-0.16f,0.84f, 0.0f, 0.06f, 0.05f, 0.03f, RGB(70,120,60) },    /* big ears */
    { 0.16f,0.84f, 0.0f, 0.06f, 0.05f, 0.03f, RGB(70,120,60) },
    {-0.07f,0.80f,0.12f, 0.03f,0.03f,0.02f, RGB(230,210,40) },     /* eyes */
    { 0.07f,0.80f,0.12f, 0.03f,0.03f,0.02f, RGB(230,210,40) },
    {-0.22f,0.55f, 0.0f, 0.05f, 0.16f, 0.05f, RGB(80,140,70) },    /* arms */
    { 0.22f,0.55f, 0.0f, 0.05f, 0.16f, 0.05f, RGB(80,140,70) },
    {-0.10f,0.16f, 0.0f, 0.05f, 0.16f, 0.05f, RGB(70,120,60) },    /* legs */
    { 0.10f,0.16f, 0.0f, 0.05f, 0.16f, 0.05f, RGB(70,120,60) },
    { 0.30f,0.40f, 0.0f, 0.05f, 0.05f, 0.05f, RGB(110,90,60) },    /* club head */
};
static const RogueCuboid M_ZOMBIE[] = {
    { 0.0f, 0.70f, 0.0f, 0.16f, 0.28f, 0.11f, RGB(90,120,80) },    /* torso */
    { 0.0f, 1.06f, 0.0f, 0.12f, 0.12f, 0.12f, RGB(110,140,95) },   /* head */
    {-0.06f,1.08f,0.11f, 0.03f,0.03f,0.02f, RGB(40,60,40) },       /* eyes */
    { 0.06f,1.08f,0.11f, 0.03f,0.03f,0.02f, RGB(40,60,40) },
    {-0.22f,0.78f,0.18f, 0.05f, 0.06f, 0.20f, RGB(95,125,82) },    /* arms out */
    { 0.22f,0.78f,0.18f, 0.05f, 0.06f, 0.20f, RGB(95,125,82) },
    {-0.09f,0.20f, 0.0f, 0.06f, 0.20f, 0.06f, RGB(80,110,72) },    /* legs */
    { 0.09f,0.20f, 0.0f, 0.06f, 0.20f, 0.06f, RGB(80,110,72) },
};
static const RogueCuboid M_ARCHER[] = {
    { 0.0f, 1.36f, 0.0f, 0.13f, 0.12f, 0.12f, RGB(225,222,205) },  /* skull */
    {-0.06f,1.40f,0.10f, 0.03f,0.03f,0.03f, RGB(20,20,20) },       /* eyes */
    { 0.06f,1.40f,0.10f, 0.03f,0.03f,0.03f, RGB(20,20,20) },
    { 0.0f, 1.02f, 0.0f, 0.13f, 0.20f, 0.07f, RGB(200,200,185) },  /* ribs */
    {-0.18f,0.92f, 0.0f, 0.04f, 0.24f, 0.04f, RGB(210,210,195) },  /* arms */
    { 0.18f,0.92f, 0.0f, 0.04f, 0.24f, 0.04f, RGB(210,210,195) },
    {-0.08f,0.30f, 0.0f, 0.05f, 0.30f, 0.05f, RGB(210,210,195) },  /* legs */
    { 0.08f,0.30f, 0.0f, 0.05f, 0.30f, 0.05f, RGB(210,210,195) },
    { 0.26f,0.95f, 0.0f, 0.02f, 0.30f, 0.02f, RGB(140,100,55) },   /* bow */
};
static const RogueCuboid M_FIRESPRITE[] = {
    { 0.0f, 0.55f, 0.0f, 0.16f, 0.18f, 0.16f, RGB(255,140,30) },   /* fiery core (hovers) */
    { 0.0f, 0.62f, 0.0f, 0.10f, 0.12f, 0.10f, RGB(255,220,110) },  /* hot centre */
    {-0.10f,0.58f,0.13f, 0.03f,0.03f,0.02f, RGB(255,255,200) },    /* eyes */
    { 0.10f,0.58f,0.13f, 0.03f,0.03f,0.02f, RGB(255,255,200) },
    {-0.18f,0.45f, 0.0f, 0.05f, 0.10f, 0.05f, RGB(255,90,20) },    /* flame wisps */
    { 0.18f,0.45f, 0.0f, 0.05f, 0.10f, 0.05f, RGB(255,90,20) },
};
static const RogueCuboid M_DEMON[] = {
    { 0.0f, 0.95f, 0.0f, 0.28f, 0.34f, 0.18f, RGB(150,30,30) },    /* hulking body */
    { 0.0f, 1.42f, 0.0f, 0.17f, 0.15f, 0.16f, RGB(170,40,35) },    /* head */
    {-0.12f,1.62f, 0.0f, 0.04f, 0.10f, 0.04f, RGB(60,20,20) },     /* horns */
    { 0.12f,1.62f, 0.0f, 0.04f, 0.10f, 0.04f, RGB(60,20,20) },
    {-0.08f,1.46f,0.13f, 0.04f,0.04f,0.03f, RGB(255,200,30) },     /* glowing eyes */
    { 0.08f,1.46f,0.13f, 0.04f,0.04f,0.03f, RGB(255,200,30) },
    {-0.36f,0.95f, 0.0f, 0.07f, 0.30f, 0.07f, RGB(150,30,30) },    /* arms */
    { 0.36f,0.95f, 0.0f, 0.07f, 0.30f, 0.07f, RGB(150,30,30) },
    {-0.14f,0.30f, 0.0f, 0.08f, 0.30f, 0.08f, RGB(120,25,25) },    /* legs */
    { 0.14f,0.30f, 0.0f, 0.08f, 0.30f, 0.08f, RGB(120,25,25) },
};

#define MN(a) (int)(sizeof(a)/sizeof(RogueCuboid))
static const RogueCuboid M_SHOPKEEPER[] = {
    { 0.0f,  0.50f,  0.0f,  0.26f, 0.50f, 0.20f, RGB(122, 40, 46) },   /* robe body */
    { 0.0f,  0.60f,  0.08f, 0.20f, 0.30f, 0.05f, RGB(214, 178, 84) },  /* gold apron */
    { 0.0f,  1.18f,  0.0f,  0.14f, 0.15f, 0.13f, RGB(224, 178, 138) }, /* head */
    { 0.0f,  1.36f,  0.0f,  0.16f, 0.04f, 0.15f, RGB(70, 28, 32) },    /* flat cap */
    { -0.30f,0.66f,  0.10f, 0.06f, 0.22f, 0.06f, RGB(122, 40, 46) },   /* arms forward */
    {  0.30f,0.66f,  0.10f, 0.06f, 0.22f, 0.06f, RGB(122, 40, 46) },
    { -0.30f,0.46f,  0.17f, 0.055f,0.05f, 0.055f, RGB(224, 178, 138) },/* hands */
    {  0.30f,0.46f,  0.17f, 0.055f,0.05f, 0.055f, RGB(224, 178, 138) },
};

static const RogueCuboid *MODEL[EN_TYPE_COUNT] = {
    M_RAT, M_SLIME, M_SKELETON, M_SPIDER, M_BAT, M_KOBOLD, M_GOBLIN,
    M_ZOMBIE, M_ARCHER, M_FIRESPRITE, M_DEMON, M_SHOPKEEPER
};
static const int MODEL_N[EN_TYPE_COUNT] = {
    MN(M_RAT), MN(M_SLIME), MN(M_SKELETON), MN(M_SPIDER), MN(M_BAT),
    MN(M_KOBOLD), MN(M_GOBLIN), MN(M_ZOMBIE), MN(M_ARCHER), MN(M_FIRESPRITE),
    MN(M_DEMON), MN(M_SHOPKEEPER)
};

/* --- RNG (local, for spawn jitter / wander) ---------------------- */
static bool s_dark = false;   /* torch out → enemies bolder + hit harder */
void rogue_enemies_set_dark(bool dark) { s_dark = dark; }

static uint32_t s_rng = 0x1234567u;
static uint32_t rng(void){ s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng; }
static float frand(void){ return (float)(rng() & 0xFFFF) / 65536.0f; }

void rogue_enemies_clear(void) {
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) s_en[i].alive = false;
    for (int i = 0; i < MAX_ESHOT; i++) s_eshot[i].alive = false;
    s_death_n = 0;
}

/* Debug: clear the pool and POSE a single enemy of `type` at (x,y,z) with a
 * given gait-clock + moving flag — used to capture animation sheets. */
void rogue_enemies_debug_showcase(int type, float x, float y, float z,
                                  float anim, int moving) {
    rogue_enemies_clear();
    Enemy *e = &s_en[0];
    e->alive = true;
    e->type = (EnemyType)(type % EN_TYPE_COUNT);
    e->pos = v3(x, y, z);
    e->yaw = 3.14159265f;            /* face the camera */
    e->hp = 10;
    e->state = AI_WANDER; e->state_t = 0.0f;
    e->wander_dx = e->wander_dz = 0.0f;
    e->hurt_flash = 0.0f; e->atk_cd = 0.0f;
    e->champion = false;
    e->anim = anim;
    e->sp = 2.0f;
    e->moving = moving != 0;
    e->calm = false;
    e->slow_t = 0; e->dot_t = 0; e->dot_tick = 0; e->dot_pow = 0;
}

int rogue_enemies_export(RogueEnemySave *out, int max) {
    int n = 0;
    for (int i = 0; i < ROGUE_MAX_ENEMIES && n < max; i++) {
        if (!s_en[i].alive) continue;
        out[n].type  = (uint8_t)s_en[i].type;
        out[n].champ = s_en[i].champion ? 1 : 0;
        out[n].hp    = (int16_t)s_en[i].hp;
        out[n].calm  = s_en[i].calm ? 1 : 0;
        out[n].x = s_en[i].pos.x; out[n].y = s_en[i].pos.y; out[n].z = s_en[i].pos.z;
        out[n].yaw = s_en[i].yaw;
        n++;
    }
    return n;
}

void rogue_enemies_import(const RogueEnemySave *in, int n) {
    rogue_enemies_clear();
    if (n > ROGUE_MAX_ENEMIES) n = ROGUE_MAX_ENEMIES;
    for (int i = 0; i < n; i++) {
        Enemy *e = &s_en[i];
        e->alive = true;
        e->type = (EnemyType)in[i].type;
        e->champion = in[i].champ != 0;
        e->hp = in[i].hp;
        e->pos = v3(in[i].x, in[i].y, in[i].z);
        e->yaw = in[i].yaw;
        e->state = AI_WANDER; e->state_t = 0.0f;
        e->wander_dx = 0.0f; e->wander_dz = 0.0f;
        e->hurt_flash = 0.0f; e->atk_cd = 0.0f;
        e->anim = (float)i * 0.41f;        /* desync the gaits */
        e->sp = 2.0f;
        e->moving = false;
        e->calm = in[i].calm != 0;
        e->slow_t = 0; e->dot_t = 0; e->dot_tick = 0; e->dot_pow = 0;
    }
}

/* Apply damage + knockback to one enemy; record a death event if it dies. */
static void en_apply_damage(Enemy *e, int dmg, float fromx, float fromz) {
    if (e->calm) { e->calm = false; e->state = AI_CHASE; e->state_t = 0; }
    float knock = 0.30f;
    switch (s_strike_elem) {
    case ELEM_FIRE:   dmg += s_strike_pow; break;            /* burn bonus */
    case ELEM_FROST:  dmg += s_strike_pow / 2;               /* chill + slow */
                      e->slow_t = 1.4f; break;
    case ELEM_POISON: e->dot_t = 3.0f; e->dot_tick = 0.5f;   /* venom over time */
                      e->dot_pow = s_strike_pow; break;
    case ELEM_LIGHTNING: {
        /* the bolt ARCS to the nearest other enemy in range */
        dmg += s_strike_pow / 2;
        Enemy *t = NULL; float best = 3.0f * 3.0f;
        for (int k = 0; k < ROGUE_MAX_ENEMIES; k++) {
            Enemy *o = &s_en[k];
            if (!o->alive || o == e || o->calm) continue;
            float ax = o->pos.x - e->pos.x, az = o->pos.z - e->pos.z;
            float d2 = ax*ax + az*az;
            if (d2 < best) { best = d2; t = o; }
        }
        if (t) {
            t->hp -= s_strike_pow;
            t->hurt_flash = 0.22f;
            rogue_dmgnum_spawn(t->pos, s_strike_pow, false);
            if (t->state == AI_WANDER) { t->state = AI_CHASE; t->state_t = 0; }
            /* jagged spark line between the two victims */
            for (int k = 0; k < 6; k++) {
                float f = (k + 1) / 7.0f;
                Vec3 sp = v3(e->pos.x + (t->pos.x - e->pos.x) * f,
                             e->pos.y + 0.55f + ((k & 1) ? 0.14f : -0.10f),
                             e->pos.z + (t->pos.z - e->pos.z) * f);
                rogue_particle_spawn(sp, 0, 0, 0, 0.16f, RGB(255,250,150), 0.05f, 0.0f);
            }
            if (t->hp <= 0) {
                t->alive = false;
                if (s_death_n < ROGUE_MAX_ENEMIES) {
                    s_death_pos[s_death_n] = t->pos;
                    s_death_type[s_death_n] = t->type;
                    s_death_n++;
                }
            }
        }
        break; }
    case ELEM_HOLY: {
        /* radiance SMITES the unholy; everyone else just gets singed */
        bool unholy = (e->type == EN_SKELETON || e->type == EN_ZOMBIE ||
                       e->type == EN_ARCHER   || e->type == EN_DEMON);
        dmg += unholy ? s_strike_pow * 2 : s_strike_pow / 2;
        if (unholy) {
            Vec3 gp = e->pos; gp.y += 0.7f;
            rogue_particle_burst(gp, 8, 3.0f, 0.40f, RGB(255,230,110), 0.06f);
        }
        break; }
    case ELEM_SHADOW:                                        /* drain life */
        dmg += s_strike_pow / 2;
        s_drain_heal += s_strike_pow / 2;
        break;
    case ELEM_VOID:                                          /* implode — drag in */
        dmg += s_strike_pow / 3;
        knock = -0.55f;
        break;
    case ELEM_ARCANE:                                        /* force — launch them */
        dmg += s_strike_pow / 3;
        knock = 1.2f;
        break;
    }
    e->hp -= dmg;
    e->hurt_flash = 0.22f;
    rogue_dmgnum_spawn(e->pos, dmg, false);   /* green — damage you dealt */
    float dx = e->pos.x - fromx, dz = e->pos.z - fromz;
    float l = sqrtf(dx*dx + dz*dz);
    if (l > 0.001f) { e->pos.x += dx/l * knock; e->pos.z += dz/l * knock; }
    if (e->hp <= 0) {
        e->alive = false;
        if (s_death_n < ROGUE_MAX_ENEMIES) {
            s_death_pos[s_death_n] = e->pos;
            s_death_type[s_death_n] = e->type;
            s_death_n++;
        }
    } else if (e->state == AI_WANDER) {
        e->state = AI_CHASE; e->state_t = 0;
    }
}

bool rogue_enemies_pop_death(Vec3 *pos, int *type) {
    if (s_death_n <= 0) return false;
    s_death_n--;
    *pos = s_death_pos[s_death_n];
    *type = (int)s_death_type[s_death_n];
    return true;
}

/* Valid spawn cell: open at body height (floor + head), flat SOLID ground
 * underneath — never inside walls/scenery, never over a pit/lava/water. */
static bool spawn_ok(float fx, float fz, int floor_y) {
    int x = (int)floorf(fx), z = (int)floorf(fz);
    if (craft_block_solid(craft_world_get(x, floor_y,     z))) return false;
    if (craft_block_solid(craft_world_get(x, floor_y + 1, z))) return false;
    return craft_block_solid(craft_world_get(x, floor_y - 1, z));
}

void rogue_enemies_spawn(const int16_t *room_cx, const int16_t *room_cz,
                         int n_rooms, int up_x, int up_z,
                         int floor_y, int depth, uint32_t seed) {
    rogue_enemies_clear();
    s_rng = seed ^ (0xABCD1234u * (uint32_t)(depth + 1));
    if (!s_rng) s_rng = 1;

    const RogueBand *band = rogue_band_get(depth);
    int rn = band->roster_n > 0 ? band->roster_n : 1;
    bool boss_floor = rogue_band_is_boss_floor(depth);

    int want = 5 + depth;                 /* population grows steadily with depth */
    if (want > ROGUE_MAX_ENEMIES) want = ROGUE_MAX_ENEMIES;

    int placed = 0;
    for (int attempt = 0; attempt < want * 12 && placed < want; attempt++) {
        int r = (int)(frand() * n_rooms);
        if (r >= n_rooms) r = n_rooms - 1;
        if (room_cx[r] == up_x && room_cz[r] == up_z) continue;  /* never spawn on start */
        /* Candidate cell MUST be valid standing ground: open at body height
         * (not inside a wall / scenery cube / pillar) with flat solid floor
         * underneath (not a pit, lava or water). Rooms now carry set-pieces
         * near their centres, so a blind centre-jitter could land inside a
         * crystal cluster or sarcophagus — retry instead. */
        float fx = room_cx[r] + 0.5f + (frand()-0.5f)*3.0f;
        float fz = room_cz[r] + 0.5f + (frand()-0.5f)*3.0f;
        if (!spawn_ok(fx, fz, floor_y)) continue;
        Enemy *e = &s_en[placed];
        e->alive = true;
        /* Pick from this band's roster. */
        EnemyType t = (EnemyType)band->roster[(int)(frand() * rn) % rn];
        e->type = t;
        e->pos = v3(fx, (float)floor_y, fz);
        e->yaw = frand() * 6.28f;
        float scale = 1.0f + 0.18f * depth;
        e->hp = (int)(DEFS[t].base_hp * scale);
        s_dmg_scale = 1.5f + 0.18f * (float)(depth - 1);
        e->state = AI_WANDER;
        e->state_t = frand() * 1.5f;
        e->wander_dx = e->wander_dz = 0.0f;
        e->hurt_flash = 0.0f;
        e->atk_cd = 0.0f;
        e->anim = frand() * 3.0f;          /* desync the gaits */
        e->sp = 1.5f + frand() * 2.0f;
        e->moving = false;
        e->calm = false;
        e->slow_t = 0; e->dot_t = 0; e->dot_tick = 0; e->dot_pow = 0;
        /* The first enemy on a band-end floor is a champion (mini-boss). */
        e->champion = (boss_floor && placed == 0);
        if (e->champion) e->hp *= 3;
        placed++;
    }
}

static bool cell_solid(int wx, int wy, int wz) {
    return craft_block_solid(craft_world_get(wx, wy, wz));
}
/* A cell an enemy may occupy: never inside anything solid (walls, scenery
 * cubes, pillars), and — for walkers — only with real footing beneath
 * (solid ground or wadeable water; never a pit or lava). Flyers skip the
 * footing rule but still can't pass through solids. */
static bool en_passable(int x, int floor_y, int z, bool flyer) {
    if (cell_solid(x, floor_y, z)) return false;
    if (flyer) return true;
    uint8_t below = (uint8_t)craft_world_get(x, floor_y - 1, z);
    return craft_block_solid((BlockId)below) || craft_is_water_id(below);
}

static void en_move(Enemy *e, float dx, float dz, float step, int floor_y) {
    bool flyer = DEFS[e->type].flyer != 0;
    float nx = e->pos.x + dx * step;
    if (en_passable((int)floorf(nx), floor_y, (int)floorf(e->pos.z), flyer)) e->pos.x = nx;
    float nz = e->pos.z + dz * step;
    if (en_passable((int)floorf(e->pos.x), floor_y, (int)floorf(nz), flyer)) e->pos.z = nz;
}

/* Straight-line sight test at stand height — ~3 samples per cell. Ranged
 * enemies may only open fire when this is clear, so nobody shoots through
 * walls or furniture. */
static bool en_los(Vec3 a, Vec3 b, int floor_y) {
    float dx = b.x - a.x, dz = b.z - a.z;
    float dist = sqrtf(dx*dx + dz*dz);
    int steps = (int)(dist * 3.0f) + 1;
    for (int s = 1; s < steps; s++) {
        float t = (float)s / (float)steps;
        if (cell_solid((int)floorf(a.x + dx * t), floor_y, (int)floorf(a.z + dz * t)))
            return false;
    }
    return true;
}

void rogue_enemies_update(RoguePlayer *p, float dt, int floor_y) {
    s_strike_elem = ELEM_NONE; s_strike_pow = 0;   /* strike elements never linger */
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) {
        Enemy *e = &s_en[i];
        if (!e->alive) continue;
        const EnemyDef *d = &DEFS[e->type];
        if (e->hurt_flash > 0) e->hurt_flash -= dt;
        if (e->atk_cd > 0) e->atk_cd -= dt;
        e->state_t += dt;

        /* Lava burns whoever ends up standing in it (knockback can shove
         * anyone in) — except fire creatures and flyers. Heavy, fast burn:
         * the lake is lethal terrain for them just like for the hero. */
        if (!d->flyer && e->type != EN_FIRESPRITE && e->type != EN_DEMON) {
            int lx = (int)floorf(e->pos.x), lz = (int)floorf(e->pos.z);
            uint8_t b1 = (uint8_t)craft_world_get(lx, floor_y - 1, lz);
            uint8_t b2 = (uint8_t)craft_world_get(lx, floor_y - 2, lz);
            /* lava directly underfoot, or one deeper with nothing solid in
             * between (knocked out over a chasm — no bridge under them) */
            if (craft_is_lava_id(b1) ||
                (!craft_block_solid((BlockId)b1) && craft_is_lava_id(b2))) {
                e->lava_tick -= dt;
                if (e->lava_tick <= 0.0f) {
                    e->lava_tick = 0.4f;
                    int burn = (int)(8.0f * s_dmg_scale);
                    e->hp -= burn;
                    e->hurt_flash = 0.18f;
                    rogue_dmgnum_spawn(e->pos, burn, false);
                    Vec3 fp = e->pos; fp.y += 0.3f;
                    rogue_particle_burst(fp, 5, 2.5f, 0.45f, RGB(255,140,30), 0.07f);
                    if (e->hp <= 0) {
                        e->alive = false;
                        if (s_death_n < ROGUE_MAX_ENEMIES) {
                            s_death_pos[s_death_n] = e->pos;
                            s_death_type[s_death_n] = e->type;
                            s_death_n++;
                        }
                        continue;
                    }
                }
            } else e->lava_tick = 0.0f;
        }

        /* --- elemental status -------------------------------------- */
        if (e->slow_t > 0) {
            e->slow_t -= dt;
            if (((int)(e->anim * 5.0f) & 1) == 0) {       /* icy motes drift up */
                Vec3 fp = e->pos; fp.y += 0.3f + 0.4f * frand();
                fp.x += (frand() - 0.5f) * 0.5f; fp.z += (frand() - 0.5f) * 0.5f;
                rogue_particle_spawn(fp, 0, 0.5f, 0, 0.30f, RGB(170,225,255), 0.04f, 0.0f);
            }
        }
        if (e->dot_t > 0) {
            e->dot_t -= dt; e->dot_tick -= dt;
            if (e->dot_tick <= 0.0f) {                    /* venom tick */
                e->dot_tick = 0.5f;
                int tick = e->dot_pow / 6; if (tick < 1) tick = 1;
                e->hp -= tick;
                rogue_dmgnum_spawn(e->pos, tick, false);
                Vec3 gp = e->pos; gp.y += 0.5f;
                rogue_particle_burst(gp, 3, 1.6f, 0.35f, RGB(140,235,90), 0.05f);
                if (e->hp <= 0) {
                    e->alive = false;
                    if (s_death_n < ROGUE_MAX_ENEMIES) {
                        s_death_pos[s_death_n] = e->pos;
                        s_death_type[s_death_n] = e->type;
                        s_death_n++;
                    }
                    continue;
                }
            }
        }

        float dx = p->pos.x - e->pos.x, dz = p->pos.z - e->pos.z;
        float dist = sqrtf(dx*dx + dz*dz);
        float nx = dist > 0.001f ? dx/dist : 0, nz = dist > 0.001f ? dz/dist : 0;
        float aggro = d->aggro * (s_dark ? 1.7f : 1.0f);   /* sense you in the dark */
        float pre_x = e->pos.x, pre_z = e->pos.z;          /* movement → animation */
        e->anim += dt;                                     /* steady gait clock */

        if (e->calm) { e->moving = false; continue; }      /* shopkeeper at his post */

        EnemyDef dslow;
        if (e->slow_t > 0) { dslow = *d; dslow.speed *= 0.45f; d = &dslow; }

#ifdef ROGUE_KEEPER_DEBUG
        if (e->type == EN_SHOPKEEPER) {
            static int dbg;
            if ((dbg++ % 30) == 0)
                printf("[keeper] st=%d sp=%.2f dist=%.2f cd=%.2f pos=(%.1f,%.1f)\n",
                       (int)e->state, e->sp, dist, e->atk_cd, e->pos.x, e->pos.z);
        }
#endif
        switch (e->state) {
        case AI_WANDER:
            if (p->alive && dist < aggro) { e->state = AI_CHASE; e->state_t = 0; break; }
            if (e->state_t > 1.2f) {
                e->state_t = 0;
                float a = frand() * 6.28f;
                e->wander_dx = sinf(a); e->wander_dz = cosf(a);
                if (frand() < 0.4f) e->wander_dx = e->wander_dz = 0;  /* pause */
            }
            if (e->wander_dx || e->wander_dz) {
                e->yaw = atan2f(e->wander_dx, e->wander_dz);
                en_move(e, e->wander_dx, e->wander_dz, d->speed * 0.4f * dt, floor_y);
            }
            break;
        case AI_CHASE:
            if (!p->alive || dist > aggro * 1.4f) { e->state = AI_WANDER; e->state_t = 0; break; }
            e->yaw = atan2f(nx, nz);
            if (d->ranged) {
                /* Kite: hold mid-range, back off if the hero closes — and
                 * STRAFE sideways while in the band (flipping every ~1.6s) so
                 * archers/sprites are moving targets with a readable rhythm.
                 * No line of sight → never fire; advance to regain it. */
                bool los = en_los(e->pos, p->pos, floor_y);
                if (e->type == EN_SHOPKEEPER) {
                    /* BLINK: cornered (hero close) or sightline blocked (his
                     * own counter!) → teleport to open ground a few cells
                     * out, with a purple poof at both ends. */
                    e->sp -= dt;
                    if (e->sp <= 0.0f && (dist < 2.5f || !los)) {
                        /* pick a spot that can actually SEE the hero (else he
                         * blinks blind forever); fall back to any open cell */
                        float bx = 0, bz = 0; int found = 0;
                        for (int t = 0; t < 20; t++) {
                            float a = frand() * 6.28f;
                            float r = 2.6f + frand() * 1.7f;   /* stay inside small rooms */
                            float tx = p->pos.x + sinf(a) * r;
                            float tz = p->pos.z + cosf(a) * r;
                            if (!en_passable((int)floorf(tx), floor_y,
                                             (int)floorf(tz), false)) continue;
                            if (!found) { bx = tx; bz = tz; found = 1; }
                            Vec3 vp = v3(tx, e->pos.y, tz);
                            if (en_los(vp, p->pos, floor_y)) {
                                bx = tx; bz = tz; found = 2;
                                break;
                            }
                        }
                        if (found) {
                            Vec3 poof = e->pos; poof.y += 0.7f;
                            rogue_particle_burst(poof, 10, 3.5f, 0.4f, RGB(190,90,255), 0.08f);
                            e->pos.x = bx; e->pos.z = bz;
                            poof = e->pos; poof.y += 0.7f;
                            rogue_particle_burst(poof, 10, 3.5f, 0.4f, RGB(190,90,255), 0.08f);
                            e->sp = 2.6f;
                            e->atk_cd = 0.30f;     /* spell follows the blink fast */
                            los = en_los(e->pos, p->pos, floor_y);
                            dx = p->pos.x - e->pos.x; dz = p->pos.z - e->pos.z;
                            dist = sqrtf(dx*dx + dz*dz);
                            nx = dist > 0.001f ? dx/dist : 0; nz = dist > 0.001f ? dz/dist : 0;
                            e->yaw = atan2f(nx, nz);
                        }
                    }
                }
                /* the wizard holds his ground much closer than archers do */
                float backoff = d->atk_range * (e->type == EN_SHOPKEEPER ? 0.28f : 0.45f);
                if (dist < backoff)                   en_move(e, -nx, -nz, d->speed * dt, floor_y);
                else if (dist > d->atk_range || !los) en_move(e, nx, nz, d->speed * dt, floor_y);
                else {
                    float sgn = ((i & 1) ? 1.0f : -1.0f) *
                                (fmodf(e->anim, 3.2f) < 1.6f ? 1.0f : -1.0f);
                    en_move(e, -nz * sgn, nx * sgn, d->speed * 0.55f * dt, floor_y);
                }
                if (dist <= d->atk_range && e->atk_cd <= 0 && los) { e->state = AI_WINDUP; e->state_t = 0; }
            } else {
                if (dist <= d->atk_range && e->atk_cd <= 0) { e->state = AI_WINDUP; e->state_t = 0; }
                else {
                    /* Per-type locomotion — each creature closes in with its
                     * own DISTINCT, PREDICTABLE pattern. */
                    float mx = nx, mz = nz, spd = d->speed;
                    float px = -nz, pz = nx;            /* strafe axis */
                    switch (e->type) {
                    case EN_RAT: {                      /* dart — pause — dart */
                        float c = fmodf(e->anim, 0.75f);
                        if (c < 0.45f) spd *= 1.9f; else spd = 0.0f;
                        break; }
                    case EN_SLIME: {                    /* discrete hops */
                        float c = fmodf(e->anim, 0.95f);
                        spd = (c >= 0.40f && c < 0.85f) ? spd * 2.4f : 0.0f;
                        break; }
                    case EN_BAT: {                      /* swooping weave */
                        float w = sinf(e->anim * 3.4f);
                        mx = nx + px * 0.8f * w; mz = nz + pz * 0.8f * w;
                        float l = sqrtf(mx*mx + mz*mz);
                        if (l > 0.01f) { mx /= l; mz /= l; }
                        e->yaw = atan2f(mx, mz);
                        break; }
                    case EN_SPIDER:                     /* circle close, then lunge */
                        if (dist < 3.4f) {
                            float sgn = (i & 1) ? 1.0f : -1.0f;
                            mx = px * sgn + nx * 0.25f; mz = pz * sgn + nz * 0.25f;
                            float l = sqrtf(mx*mx + mz*mz);
                            if (l > 0.01f) { mx /= l; mz /= l; }
                        }
                        break;
                    case EN_KOBOLD: case EN_GOBLIN: {   /* weaving zigzag */
                        float w = sinf(e->anim * 2.6f) * 0.7f;
                        mx = nx + px * w; mz = nz + pz * w;
                        float l = sqrtf(mx*mx + mz*mz);
                        if (l > 0.01f) { mx /= l; mz /= l; }
                        e->yaw = atan2f(mx, mz);
                        break; }
                    case EN_ZOMBIE:                     /* relentless lurch */
                        spd *= 0.55f + 0.75f * fmaxf(0.0f, sinf(e->anim * 2.4f));
                        break;
                    case EN_DEMON:                      /* paw the ground, then CHARGE */
                        e->sp -= dt;
                        if (e->sp <= -0.55f) e->sp = 3.0f;     /* charge spent */
                        if (e->sp <= 0.0f)      spd *= 2.6f;   /* charging! */
                        else if (e->sp < 0.45f) spd = 0.0f;    /* the tell */
                        break;
                    default: break;
                    }
                    en_move(e, mx, mz, spd * dt, floor_y);
                }
            }
            break;
        case AI_WINDUP:
            e->yaw = atan2f(nx, nz);     /* track during the tell */
            if (e->state_t >= d->windup) { e->state = AI_STRIKE; e->state_t = 0; }
            break;
        case AI_STRIKE:
            if (e->state_t == 0.0f || e->state_t < dt + 0.0001f) {
                int dmg = (int)(d->base_dmg * d->dmg_mul * s_dmg_scale) * (e->champion ? 2 : 1);
                if (s_dark) dmg = dmg * 3 / 2;     /* the dark bites harder */
                if (d->ranged) {
                    if (e->type == EN_SHOPKEEPER) {
                        /* a fan of three arcane bolts — dodge the spread */
                        float bx = p->pos.x - e->pos.x, bz = p->pos.z - e->pos.z;
                        for (int b = -1; b <= 1; b++) {
                            float a = 0.30f * (float)b;
                            float ca = cosf(a), sa = sinf(a);
                            eshot_fire(e->pos, e->pos.x + bx * ca - bz * sa,
                                       e->pos.z + bx * sa + bz * ca,
                                       dmg, RGB(190, 90, 255));
                        }
                    } else {
                        /* loose a projectile toward the hero */
                        uint16_t col = (e->type == EN_FIRESPRITE) ? RGB(255,140,30) : RGB(230,230,210);
                        eshot_fire(e->pos, p->pos.x, p->pos.z, dmg, col);
                    }
                } else if (p->alive && dist <= d->atk_range + (e->champion ? 0.8f : 0.4f)) {
                    if (rogue_player_damage(p, dmg, e->pos) &&
                        (p->stats.aspects & (1u << ASP_THORNS)))
                        en_apply_damage(e, 6 + dmg / 2, p->pos.x, p->pos.z);
                }
            }
            if (e->state_t >= 0.18f) {
                e->state = AI_CHASE;
                e->atk_cd = d->ranged ? 1.4f : 0.8f;
            }
            break;
        }
        e->moving = (fabsf(e->pos.x - pre_x) + fabsf(e->pos.z - pre_z)) > 0.0008f;
    }

    /* Advance enemy projectiles → hit the player / walls / expire. */
    for (int i = 0; i < MAX_ESHOT; i++) {
        EShot *s = &s_eshot[i];
        if (!s->alive) continue;
        s->pos.x += s->vx * dt; s->pos.z += s->vz * dt;
        s->life -= dt;
        if (s->life <= 0) { s->alive = false; continue; }
        if (craft_block_solid(craft_world_get((int)floorf(s->pos.x), floor_y,
                                               (int)floorf(s->pos.z)))) { s->alive = false; continue; }
        float dx = p->pos.x - s->pos.x, dz = p->pos.z - s->pos.z;
        if (p->alive && dx*dx + dz*dz < 0.45f*0.45f) {
            rogue_player_damage(p, s->dmg, s->pos);
            s->alive = false;
        }
    }
}

int rogue_enemies_hit_arc(Vec3 origin, float yaw, float range,
                          float arc_cos, int dmg) {
    float fx = sinf(yaw), fz = cosf(yaw);
    int hits = 0;
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) {
        Enemy *e = &s_en[i];
        if (!e->alive) continue;
        float dx = e->pos.x - origin.x, dz = e->pos.z - origin.z;
        float dist = sqrtf(dx*dx + dz*dz);
        if (dist > range + DEFS[e->type].radius) continue;
        if (dist > 0.001f) {
            float dot = (dx/dist) * fx + (dz/dist) * fz;
            if (dot < arc_cos) continue;     /* outside the swing arc */
        }
        en_apply_damage(e, dmg, origin.x, origin.z);
        hits++;
    }
    return hits;
}

int rogue_enemies_hit_radius(float x, float z, float radius, int dmg) {
    int hits = 0;
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) {
        Enemy *e = &s_en[i];
        if (!e->alive) continue;
        float dx = e->pos.x - x, dz = e->pos.z - z;
        float rr = radius + DEFS[e->type].radius;
        if (dx*dx + dz*dz > rr*rr) continue;
        en_apply_damage(e, dmg, x, z);
        hits++;
    }
    return hits;
}

int rogue_enemies_hit_point(float x, float z, float radius, int dmg) {
    int hits = 0;
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) {
        Enemy *e = &s_en[i];
        if (!e->alive) continue;
        float dx = e->pos.x - x, dz = e->pos.z - z;
        float rr = radius + DEFS[e->type].radius;
        if (dx*dx + dz*dz > rr*rr) continue;
        en_apply_damage(e, dmg, x, z);
        hits++;
        break;   /* a projectile hits one target */
    }
    return hits;
}

void rogue_enemies_draw(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) {
        Enemy *e = &s_en[i];
        if (!e->alive) continue;
        const EnemyDef *d = &DEFS[e->type];
        float flash = (e->hurt_flash > 0) ? 0.8f : 0.0f;
        /* Telegraph: flash bright white-hot during the wind-up. */
        if (e->state == AI_WINDUP) {
            float k = e->state_t / d->windup;
            flash = 0.4f + 0.5f * k;
        }
        /* Every enemy renders 25% larger than its hitbox (readability — they
         * were easy to lose against busy floors); champions stay the hulking
         * outlier. A soft dark shadow slab under the feet anchors them. */
        const float sc = e->champion ? 1.9f : 1.25f;
        RogueCuboid big[16];
        int n = MODEL_N[e->type]; if (n > 16) n = 16;
        for (int k = 0; k < n; k++) {
            big[k] = MODEL[e->type][k];
            big[k].cx *= sc; big[k].cy *= sc; big[k].cz *= sc;
            big[k].hx *= sc; big[k].hy *= sc; big[k].hz *= sc;
        }

        /* --- body animation --------------------------------------------- *
         * Slimes squash-and-stretch through their hop; bats flap and bob;
         * fire sprites flicker; everything with legs strides them (and
         * counter-swings arms) while moving. Part indices per model. */
        float a = e->anim;
        float hop = 0.0f;
        if (e->type == EN_SLIME) {
            float c = fmodf(a, 0.95f), sy, sxz;
            if (c < 0.40f) {              /* squat before the hop */
                float k = sinf((c / 0.40f) * (float)M_PI);
                sy = 1.0f - 0.28f * k; sxz = 1.0f + 0.20f * k;
            } else if (c < 0.85f) {       /* airborne — stretched */
                float k = (c - 0.40f) / 0.45f;
                hop = 0.50f * sinf(k * (float)M_PI);
                sy = 1.18f; sxz = 0.88f;
            } else { sy = 0.86f; sxz = 1.10f; }   /* landing splat */
            for (int k = 0; k < n; k++) {
                big[k].cy *= sy;  big[k].hy *= sy;
                big[k].cx *= sxz; big[k].hx *= sxz;
                big[k].cz *= sxz; big[k].hz *= sxz;
            }
        } else if (e->type == EN_BAT) {
            hop = 0.10f * sinf(a * 6.0f);                 /* hover bob */
            float flap = 0.08f * sinf(a * 14.0f) * sc;
            big[1].cy += flap; big[2].cy += flap;         /* wings */
        } else if (e->type == EN_FIRESPRITE) {
            hop = 0.06f * sinf(a * 9.0f);                 /* flame flicker */
            big[4].cy += 0.05f * sinf(a * 11.0f) * sc;    /* wisps lick upward */
            big[5].cy += 0.05f * sinf(a * 11.0f + 2.1f) * sc;
        } else if (e->moving) {
            /* leg stride (+ arm counter-swing) — part indices per model */
            static const int8_t LEGA[EN_TYPE_COUNT] = { -1,-1, 7, 4,-1, 5, 8, 6, 6,-1, 8,-1 };
            static const int8_t LEGB[EN_TYPE_COUNT] = { -1,-1, 8, 5,-1, 6, 9, 7, 7,-1, 9,-1 };
            static const int8_t ARMA[EN_TYPE_COUNT] = { -1,-1, 5, 6,-1,-1, 6, 4, 4,-1, 6, 4 };
            static const int8_t ARMB[EN_TYPE_COUNT] = { -1,-1, 6, 7,-1,-1, 7, 5, 5,-1, 7, 5 };
            float rate = 7.0f + d->speed * 3.0f;
            float sw = sinf(a * rate) * 0.085f * sc;
            int la = LEGA[e->type], lb = LEGB[e->type];
            int aa = ARMA[e->type], ab = ARMB[e->type];
            if (la >= 0 && la < n) big[la].cz += sw;
            if (lb >= 0 && lb < n) big[lb].cz -= sw;
            if (aa >= 0 && aa < n) big[aa].cz -= sw;      /* arms counter-swing */
            if (ab >= 0 && ab < n) big[ab].cz += sw;
            /* gentle whole-body bob at double stride frequency */
            float bob = 0.025f * sc * (0.5f - 0.5f * cosf(a * rate * 2.0f));
            for (int k = 0; k < n; k++) big[k].cy += bob;
            /* archer legs double as spider mid-legs: skeleton-family already
             * covered above by the tables. Rats wiggle the tail instead. */
            if (e->type == EN_RAT && n > 2) big[2].cx += sinf(a * 10.0f) * 0.05f * sc;
        }

        if (e->champion && flash < 0.25f) flash = 0.25f;
        Vec3 dp = e->pos; dp.y += hop;
        float shr = d->radius * sc * 0.95f;
        RogueCuboid shadow[1] = { { 0.0f, 0.02f, 0.0f, shr, 0.012f, shr, RGB(15,13,17) } };
        rogue_render_model(cam, fb, e->pos, 0.0f, shadow, 1, shr + 0.05f, 0.06f, 0.0f, 256);
        rogue_render_model(cam, fb, dp, e->yaw, big, n,
                           d->radius * sc + 0.05f, d->height * sc + 0.6f, flash, 256);
    }
    /* enemy projectiles — bigger + hotter so incoming fire reads clearly */
    for (int i = 0; i < MAX_ESHOT; i++) {
        EShot *s = &s_eshot[i];
        if (!s->alive) continue;
        RogueCuboid m[1] = { { 0.0f, 0.0f, 0.0f, 0.11f, 0.09f, 0.11f, s->col } };
        rogue_render_model(cam, fb, s->pos, atan2f(s->vx, s->vz), m, 1, 0.15f, 0.14f, 0.7f, 256);
    }
}

int rogue_enemies_alive_count(void) {
    int n = 0;
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++)
        if (s_en[i].alive && !s_en[i].calm) n++;
    return n;
}

/* Spawn the merchant at his stall. Calm unless the player's reputation
 * precedes them (a shopkeeper was killed on an earlier floor). Takes a
 * free slot, or evicts the last regular enemy on a packed floor. */
void rogue_enemies_add_shopkeeper(float x, float y, float z, float yaw,
                                  bool calm, int depth) {
    int slot = -1;
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++)
        if (!s_en[i].alive) { slot = i; break; }
    if (slot < 0) {
        for (int i = ROGUE_MAX_ENEMIES - 1; i >= 0; i--)
            if (!s_en[i].champion) { slot = i; break; }
        if (slot < 0) return;
    }
    Enemy *e = &s_en[slot];
    e->alive = true;
    e->type = EN_SHOPKEEPER;
    e->pos = v3(x, y, z);
    e->yaw = yaw;
    e->hp = (int)(DEFS[EN_SHOPKEEPER].base_hp * (1.0f + 0.18f * depth) * 2.0f);
    e->state = calm ? AI_WANDER : AI_CHASE;
    e->state_t = 0.0f;
    e->wander_dx = e->wander_dz = 0.0f;
    e->hurt_flash = 0.0f; e->atk_cd = 0.0f;
    e->anim = 0.0f; e->sp = 0.0f;
    e->moving = false;
    e->champion = false;
    e->calm = calm;
}

int rogue_enemies_shopkeeper_state(void) {
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++)
        if (s_en[i].alive && s_en[i].type == EN_SHOPKEEPER)
            return s_en[i].calm ? 1 : 2;
    return 0;
}

bool rogue_enemies_nearest(float x, float z, float *ex, float *ez) {
    float best = 1e30f; bool found = false;
    for (int i = 0; i < ROGUE_MAX_ENEMIES; i++) {
        if (!s_en[i].alive || s_en[i].calm) continue;   /* never auto-target the calm shopkeeper */
        float dx = s_en[i].pos.x - x, dz = s_en[i].pos.z - z;
        float d = dx*dx + dz*dz;
        if (d < best) { best = d; *ex = s_en[i].pos.x; *ez = s_en[i].pos.z; found = true; }
    }
    return found;
}
