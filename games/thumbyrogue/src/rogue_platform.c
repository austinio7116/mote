#include "rogue_platform.h"
#include "rogue_render.h"
#include <math.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define MAX_PLAT 6
#define PLAT_HX  0.95f
#define PLAT_HZ  0.95f
#define PLAT_TH  0.30f   /* slab half-thickness */
#define PLAYER_R 0.30f

typedef struct {
    bool  used;
    Vec3  a, b;          /* endpoints (top-surface position) */
    float t, dir, speed;
    Vec3  pos, prev;     /* current / previous top-surface position */
} Plat;

static Plat s_p[MAX_PLAT];

void rogue_platform_clear(void) {
    for (int i = 0; i < MAX_PLAT; i++) s_p[i].used = false;
}

static uint32_t hh(uint32_t x){ x^=x<<13; x^=x>>17; x^=x<<5; return x; }

void rogue_platform_place(const int16_t *room_cx, const int16_t *room_cz,
                          int n_rooms, int up_x, int up_z,
                          int floor_y, int depth, uint32_t seed,
                          const int16_t *chasm_x, const int16_t *chasm_z,
                          int n_chasm) {
    rogue_platform_clear();
    uint32_t r = seed ^ 0x9143u ^ (uint32_t)(depth * 2654435761u);
    float y = (float)floor_y;          /* top surface at normal walk height */
    int placed = 0;

    /* One platform per chasm, ferrying from the bridge OUT to the marooned
     * bonus-chest island — the only way to reach that loot. */
    for (int c = 0; c < n_chasm && placed < MAX_PLAT; c++) {
        int cx = chasm_x[c], cz = chasm_z[c];
        Plat *p = &s_p[placed++];
        p->used = true;
        p->a = v3((float)cx,        y, (float)(cz - 4));  /* cross-arm boarding point */
        p->b = v3((float)(cx - 4),  y, (float)(cz - 4));  /* the marooned island */
        r = hh(r);
        p->t = (float)(r & 0xFF) / 255.0f;
        p->dir = 1.0f;
        p->speed = 0.45f + 0.04f * depth;
        if (p->speed > 0.75f) p->speed = 0.75f;
        p->pos = p->prev = p->a;
    }

    /* Platforms ONLY span lava chasms — over open lava the path is clear, so
     * they never clip through walls/scenery and they always have a purpose
     * (an alternate route across the lake). No more random room platforms. */
    (void)room_cx; (void)room_cz; (void)n_rooms; (void)up_x; (void)up_z;
}

void rogue_platform_update(float dt) {
    for (int i = 0; i < MAX_PLAT; i++) {
        Plat *p = &s_p[i];
        if (!p->used) continue;
        p->t += p->dir * p->speed * dt;
        if (p->t >= 1.0f) { p->t = 1.0f; p->dir = -1.0f; }
        if (p->t <= 0.0f) { p->t = 0.0f; p->dir =  1.0f; }
        p->prev = p->pos;
        p->pos.x = p->a.x + (p->b.x - p->a.x) * p->t;
        p->pos.y = p->a.y + (p->b.y - p->a.y) * p->t;
        p->pos.z = p->a.z + (p->b.z - p->a.z) * p->t;
    }
}

bool rogue_platform_support(float x, float z, float feet,
                            float *top, float *dx, float *dy, float *dz) {
    float best = -1e30f; int bi = -1;
    for (int i = 0; i < MAX_PLAT; i++) {
        Plat *p = &s_p[i];
        if (!p->used) continue;
        if (fabsf(x - p->pos.x) > PLAT_HX + PLAYER_R) continue;
        if (fabsf(z - p->pos.z) > PLAT_HZ + PLAYER_R) continue;
        float t = p->pos.y;                 /* top surface */
        if (t > feet + 0.35f) continue;     /* above the player's feet */
        if (t < feet - 0.7f) continue;      /* too far below to land on */
        if (t > best) { best = t; bi = i; }
    }
    if (bi < 0) return false;
    *top = best;
    *dx = s_p[bi].pos.x - s_p[bi].prev.x;
    *dy = s_p[bi].pos.y - s_p[bi].prev.y;
    *dz = s_p[bi].pos.z - s_p[bi].prev.z;
    return true;
}

void rogue_platform_draw(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < MAX_PLAT; i++) {
        Plat *p = &s_p[i];
        if (!p->used) continue;
        /* Carved stone slab with a warm metal-trim top edge — reads clearly
         * over the dark lava without the odd purple. */
        RogueCuboid m[3] = {
            { 0.0f, -PLAT_TH, 0.0f, PLAT_HX,        PLAT_TH, PLAT_HZ,        RGB(96, 92, 86)  },
            { 0.0f, -0.04f,   0.0f, PLAT_HX,        0.04f,   PLAT_HZ,        RGB(150,145,135) },
            { 0.0f, -0.04f,   0.0f, PLAT_HX*0.9f,   0.05f,   PLAT_HZ*0.9f,   RGB(196,150, 70) },
        };
        rogue_render_model(cam, fb, p->pos, 0.0f, m, 3, PLAT_HX + 0.1f, 0.4f, 0.0f, 256);
    }
}
