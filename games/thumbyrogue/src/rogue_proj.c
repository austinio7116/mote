#include "rogue_proj.h"
#include "rogue_render.h"
#include "rogue_enemy.h"
#include "rogue_particle.h"
#include "rogue_items.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include <math.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define MAX_PROJ 10

/* Per-kind appearance: body colour/size, trail colour, render glow, trail
 * motes per tick, and impact burst size/count. Indexed by ProjKind. */
typedef struct {
    uint16_t body, trail;
    float    hx, hy, hz, glow;
    int      trail_n, impact_n;
    float    impact_sz;
} ProjStyle;
static const ProjStyle PSTYLE[PROJ_KIND_COUNT] = {
    /* ARROW   */ { RGB(230,210,120), RGB(210,185,110), 0.05f,0.04f,0.18f, 0.20f, 1,  6, 0.07f },
    /* BOLT    */ { RGB(200,208,222), RGB(150,160,175), 0.06f,0.05f,0.22f, 0.30f, 1,  8, 0.08f },
    /* WAND    */ { RGB(120,230,255), RGB(150,220,255), 0.08f,0.08f,0.10f, 0.75f, 2,  9, 0.08f },
    /* SCEPTER */ { RGB(255,220,120), RGB(255,235,170), 0.09f,0.09f,0.11f, 0.75f, 2, 10, 0.09f },
    /* STAFF   */ { RGB(190,120,255), RGB(170,110,240), 0.12f,0.12f,0.13f, 0.85f, 3, 14, 0.11f },
};
static const ProjStyle *style_of(int kind) {
    if (kind < 0 || kind >= PROJ_KIND_COUNT) kind = PROJ_ARROW;
    return &PSTYLE[kind];
}

typedef struct {
    bool  alive;
    Vec3  pos;
    float vx, vz;
    float travelled, max_range;
    int   dmg;
    int   kind;       /* ProjKind — selects PSTYLE */
    int   pierce;     /* aspect: pass through enemies */
    float hit_cd;     /* re-hit interval while piercing */
    float spin;       /* magic motes jitter the trail for a sparkly look */
    int   elem;       /* ElementId — recolours body/trail/impacts + on-hit */
    int   elem_pow;
} Proj;

static Proj s_proj[MAX_PROJ];

void rogue_proj_clear(void) {
    for (int i = 0; i < MAX_PROJ; i++) s_proj[i].alive = false;
}

/* Elemental palettes — body + trail per element. */
static uint16_t elem_body_col(int elem, uint16_t def) {
    switch (elem) {
    case ELEM_FIRE:      return RGB(255,120,40);
    case ELEM_FROST:     return RGB(130,210,255);
    case ELEM_POISON:    return RGB(120,235,90);
    case ELEM_LIGHTNING: return RGB(255,250,140);
    case ELEM_HOLY:      return RGB(255,225,100);
    case ELEM_SHADOW:    return RGB(150,70,210);
    case ELEM_VOID:      return RGB(105,95,240);
    case ELEM_ARCANE:    return RGB(255,95,235);
    default:             return def;
    }
}
static uint16_t elem_trail_col(int elem, uint16_t def) {
    switch (elem) {
    case ELEM_FIRE:      return RGB(255,185,70);
    case ELEM_FROST:     return RGB(185,235,255);
    case ELEM_POISON:    return RGB(175,255,135);
    case ELEM_LIGHTNING: return RGB(205,225,255);
    case ELEM_HOLY:      return RGB(255,245,185);
    case ELEM_SHADOW:    return RGB(110,55,160);
    case ELEM_VOID:      return RGB(70,60,185);
    case ELEM_ARCANE:    return RGB(255,160,245);
    default:             return def;
    }
}

void rogue_proj_fire(Vec3 pos, float yaw, float speed, int dmg,
                     int kind, float max_range, int pierce,
                     int elem, int elem_pow) {
    for (int i = 0; i < MAX_PROJ; i++) {
        if (s_proj[i].alive) continue;
        Proj *p = &s_proj[i];
        p->alive = true;
        p->pos = pos; p->pos.y += 0.55f;
        p->vx = sinf(yaw) * speed;
        p->vz = cosf(yaw) * speed;
        p->travelled = 0;
        p->max_range = max_range;
        p->dmg = dmg;
        p->kind = kind;
        p->pierce = pierce;
        p->hit_cd = 0;
        p->spin = 0;
        p->elem = elem;
        p->elem_pow = elem_pow;
        return;
    }
}

static bool cell_solid(int wx, int wy, int wz) {
    return craft_block_solid(craft_world_get(wx, wy, wz));
}

/* Magic detonation: an expanding circular shock-ring + a hot core burst,
 * and splash damage around the impact (walls and creatures alike). The
 * staff blows the biggest crater; element tints the whole display. */
static void proj_explode(const Proj *p, const ProjStyle *st) {
    uint16_t core = elem_body_col(p->elem, st->body);
    uint16_t ring = elem_trail_col(p->elem, st->trail);
    float scale = (p->kind == PROJ_STAFF) ? 1.35f : (p->kind == PROJ_SCEPTER) ? 1.1f : 1.0f;
    /* the WAVE: a flat ring of sparks racing outward */
    int n = (int)(14 * scale);
    for (int k = 0; k < n; k++) {
        float a = (float)k / (float)n * 6.2831853f;
        rogue_particle_spawn(p->pos, sinf(a) * 6.5f * scale, 0.6f, cosf(a) * 6.5f * scale,
                             0.32f, ring, 0.065f, 0.0f);
    }
    /* hot core */
    rogue_particle_burst(p->pos, st->impact_n + 4, 4.5f, 0.40f, core, st->impact_sz + 0.02f);
    /* splash — half damage to everything near the blast */
    rogue_enemies_set_strike_element(p->elem, p->elem_pow);
    rogue_enemies_hit_radius(p->pos.x, p->pos.z, 1.5f * scale, p->dmg / 2);
    rogue_enemies_set_strike_element(ELEM_NONE, 0);
}

void rogue_proj_update(float dt, int floor_y) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (!p->alive) continue;
        const ProjStyle *st = style_of(p->kind);
        float dx = p->vx * dt, dz = p->vz * dt;
        p->pos.x += dx; p->pos.z += dz;
        p->travelled += sqrtf(dx*dx + dz*dz);
        p->spin += dt * 22.0f;
        /* Trail — magic kinds emit a couple of jittered sparks for a glittery
         * arcane look; arrows/bolts leave a single tight streak mote. */
        for (int t = 0; t < st->trail_n; t++) {
            float jx = 0, jy = 0, jz = 0;
            if (st->trail_n > 1) {
                jx = sinf(p->spin + t * 2.1f) * 0.06f;
                jy = cosf(p->spin * 1.3f + t) * 0.06f;
                jz = cosf(p->spin + t * 2.1f) * 0.06f;
            }
            Vec3 tp = p->pos; tp.x += jx; tp.y += jy; tp.z += jz;
            rogue_particle_spawn(tp, 0, 0, 0, 0.18f,
                                 elem_trail_col(p->elem, st->trail), 0.05f, 0.0f);
        }
        if (p->travelled > p->max_range) { p->alive = false; continue; }
        if (cell_solid((int)floorf(p->pos.x), floor_y, (int)floorf(p->pos.z))) {
            if (p->kind >= PROJ_WAND) proj_explode(p, st);
            else rogue_particle_burst(p->pos, st->impact_n - 2, 4.0f, 0.30f,
                                      elem_body_col(p->elem, st->body), st->impact_sz);
            p->alive = false; continue;
        }
        if (p->hit_cd > 0) p->hit_cd -= dt;
        if (p->hit_cd <= 0) {
            rogue_enemies_set_strike_element(p->elem, p->elem_pow);
            int hit = rogue_enemies_hit_point(p->pos.x, p->pos.z, 0.35f, p->dmg);
            rogue_enemies_set_strike_element(ELEM_NONE, 0);
            if (hit) {
                if (p->kind >= PROJ_WAND) proj_explode(p, st);
                else rogue_particle_burst(p->pos, st->impact_n, 5.0f, 0.35f,
                                          elem_body_col(p->elem, st->body), st->impact_sz);
                if (p->pierce) p->hit_cd = 0.12f;   /* keep flying, re-hit periodically */
                else           p->alive = false;
            }
        }
    }
}

void rogue_proj_draw(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (!p->alive) continue;
        const ProjStyle *st = style_of(p->kind);
        RogueCuboid body[1] = {
            { 0.0f, 0.0f, 0.0f, st->hx, st->hy, st->hz,
              elem_body_col(p->elem, st->body) }
        };
        float yaw = atan2f(p->vx, p->vz);
        rogue_render_model(cam, fb, p->pos, yaw, body, 1,
                           st->hz + 0.04f, 0.12f, st->glow, 256);
    }
}
