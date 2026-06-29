#include "rogue_particle.h"
#include "rogue_render.h"
#include <math.h>

#define MAX_PART 72

typedef struct {
    bool  alive;
    Vec3  pos;
    float vx, vy, vz, grav;
    float life, max_life;
    uint16_t col;
    float size;
} Particle;

static Particle s_p[MAX_PART];
static int s_next;
static uint32_t s_rng = 0x51F00D1u;
static uint32_t xr(void){ s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng; }
static float fr(void){ return (float)(xr() & 0xFFFF) / 65536.0f; }

void rogue_particle_clear(void) {
    for (int i = 0; i < MAX_PART; i++) s_p[i].alive = false;
    s_next = 0;
}

void rogue_particle_spawn(Vec3 pos, float vx, float vy, float vz,
                          float life, uint16_t col, float size, float grav) {
    /* ring buffer: oldest gets recycled if the pool is full */
    Particle *p = &s_p[s_next];
    s_next = (s_next + 1) % MAX_PART;
    p->alive = true; p->pos = pos;
    p->vx = vx; p->vy = vy; p->vz = vz; p->grav = grav;
    p->life = p->max_life = life;
    p->col = col; p->size = size;
}

void rogue_particle_burst(Vec3 pos, int n, float speed, float life,
                          uint16_t col, float size) {
    for (int i = 0; i < n; i++) {
        float a = fr() * 6.2831853f;
        float up = 0.3f + fr() * 1.2f;
        float s = speed * (0.4f + fr() * 0.8f);
        rogue_particle_spawn(pos, cosf(a) * s, up * speed * 0.6f, sinf(a) * s,
                             life * (0.6f + fr() * 0.6f), col, size, 9.0f);
    }
}

int rogue_particle_debug_count(float *ymin, float *ymax) {
    int n = 0; *ymin = 1e9f; *ymax = -1e9f;
    for (int i = 0; i < MAX_PART; i++) {
        if (!s_p[i].alive) continue;
        n++;
        if (s_p[i].pos.y < *ymin) *ymin = s_p[i].pos.y;
        if (s_p[i].pos.y > *ymax) *ymax = s_p[i].pos.y;
    }
    return n;
}

void rogue_particle_update(float dt) {
    for (int i = 0; i < MAX_PART; i++) {
        Particle *p = &s_p[i];
        if (!p->alive) continue;
        p->life -= dt;
        if (p->life <= 0) { p->alive = false; continue; }
        p->pos.x += p->vx * dt;
        p->pos.y += p->vy * dt;
        p->pos.z += p->vz * dt;
        p->vy -= p->grav * dt;
    }
}

void rogue_particle_draw(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < MAX_PART; i++) {
        Particle *p = &s_p[i];
        if (!p->alive) continue;
        float k = p->life / p->max_life;       /* 1 → 0 */
        float sz = p->size * (0.4f + 0.6f * k); /* shrink as it fades */
        RogueCuboid m[1] = { { 0.0f, 0.0f, 0.0f, sz, sz, sz, p->col } };
        /* a little self-glow so sparks pop in the dark */
        rogue_render_model(cam, fb, p->pos, 0.0f, m, 1, sz + 0.05f, sz * 2.0f, 0.35f * k, 256);
    }
}
