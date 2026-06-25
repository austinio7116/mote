/*
 * ThumbyElite — asteroid fields + mining.
 *
 * Rocks are tumbling deformed-sphere meshes (two variants generated at
 * init, instanced with per-rock scale). Hitscan weapons chip them —
 * the MINING laser at 4x rate — and every 20 damage of chipping spills
 * an ore canister (minerals / metals / rare gems) for the scoop.
 * Cracked rocks die in a dust burst with a final ore spill.
 */
#include "elite_rocks.h"
#include "elite_types.h"
#include "r3d_scene.h"
#include "r3d_fx.h"
#include "elite_loot.h"
#include <math.h>

#define MAX_ROCKS 8

typedef struct {
    Vec3  pos, vel;
    float spin, radius;     /* render scale, metres */
    float hp, chip;         /* chip accumulates toward ore spills */
    uint8_t alive, variant;
} Rock;

static Rock s_rocks[MAX_ROCKS];

/* --- rock meshes (two variants, generated once) ------------------------*/
#define ROCK_V 26
#define ROCK_F 48
static MeshVert s_rv[2][ROCK_V];
static MeshFace s_rf[2][ROCK_F];
static uint16_t s_rc[2][ROCK_F];     /* engine per-face colours */
static Mesh     s_rm[2];
static int      s_meshes_ready;

static uint32_t s_rng = 0x60CCu;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float rndf(float lo, float hi) {
    return lo + (hi - lo) * (float)(rnd() & 0xFFFF) * (1.0f / 65535.0f);
}

static void rock_mesh_build(int variant) {
    /* Three jittered octagonal rings + poles — a lumpy potato. */
    MeshVert *v = s_rv[variant];
    MeshFace *f = s_rf[variant];
    uint16_t *fc = s_rc[variant];
    int nv = 0, nf = 0;
    int rings[3][8];
    float zs[3] = { -0.55f, 0.0f, 0.55f };
    float rs[3] = { 0.62f, 1.0f, 0.62f };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 8; i++) {
            float a = (float)i * (6.2831853f / 8.0f) + (float)k * 0.26f;
            float jr = rs[k] * rndf(0.72f, 1.18f);
            v[nv].x = (int8_t)(cosf(a) * jr * 100.0f);
            v[nv].y = (int8_t)(sinf(a) * jr * 100.0f);
            v[nv].z = (int8_t)(zs[k] * rndf(0.8f, 1.25f) * 100.0f);
            rings[k][i] = nv++;
        }
    int south = nv;
    v[nv].x = (int8_t)rndf(-15, 15); v[nv].y = (int8_t)rndf(-15, 15);
    v[nv].z = -95; nv++;
    int north = nv;
    v[nv].x = (int8_t)rndf(-15, 15); v[nv].y = (int8_t)rndf(-15, 15);
    v[nv].z = 95; nv++;

    uint16_t c0 = RGB565C(120, 108, 96), c1 = RGB565C(88, 80, 72);
    #define RF(a2, b2, c2, col) do { \
        f[nf].a = (uint8_t)(a2); f[nf].b = (uint8_t)(b2); \
        f[nf].c = (uint8_t)(c2); fc[nf] = (col); \
        /* normal from the verts */ \
        float ux = v[f[nf].b].x - v[f[nf].a].x, \
              uy = v[f[nf].b].y - v[f[nf].a].y, \
              uz = v[f[nf].b].z - v[f[nf].a].z; \
        float wx = v[f[nf].c].x - v[f[nf].a].x, \
              wy = v[f[nf].c].y - v[f[nf].a].y, \
              wz = v[f[nf].c].z - v[f[nf].a].z; \
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, \
              nz = ux * wy - uy * wx; \
        float l = sqrtf(nx * nx + ny * ny + nz * nz); \
        if (l < 1e-6f) l = 1; \
        f[nf].nx = (int8_t)(nx / l * 127.0f); \
        f[nf].ny = (int8_t)(ny / l * 127.0f); \
        f[nf].nz = (int8_t)(nz / l * 127.0f); \
        nf++; \
    } while (0)
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 8; i++) {
            int j = (i + 1) & 7;
            RF(rings[k][i], rings[k][j], rings[k + 1][j],
               (i & 1) ? c0 : c1);
            RF(rings[k][i], rings[k + 1][j], rings[k + 1][i], c0);
        }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        RF(rings[0][i], south, rings[0][j], c1);
        RF(rings[2][i], rings[2][j], north, c0);
    }
    #undef RF
    s_rm[variant].verts = v;
    s_rm[variant].faces = f;
    s_rm[variant].face_colors = fc;
    s_rm[variant].color = 0;
    s_rm[variant].nverts = (uint16_t)nv;
    s_rm[variant].nfaces = (uint16_t)nf;
    s_rm[variant].scale = 1.27f;       /* pipe: scale = metres at vert 127;
                                          verts span ~±100 -> unit radius,
                                          per-instance os = world radius */
    s_rm[variant].bound_r = 1.25f;
    s_rm[variant].lod_lo = 0;
}

void rocks_init(void) {
    for (int i = 0; i < MAX_ROCKS; i++) s_rocks[i].alive = 0;
    if (!s_meshes_ready) {
        rock_mesh_build(0);
        rock_mesh_build(1);
        s_meshes_ready = 1;
    }
}

void rocks_spawn_field(uint32_t seed, int n) {
    /* The belt is a visible CLUMP, not a scatter (user req): one
     * cluster centre 400-700m out, rocks packed within ~220m of it —
     * you can SEE the belt hanging in one part of the sky. One or two
     * rocks are proper boulders (up to 42m); the rest medium. */
    s_rng = seed | 1u;
    if (n > MAX_ROCKS) n = MAX_ROCKS;
    float ca = rndf(0, 6.2831853f);
    float cd = rndf(400, 700);
    Vec3 centre = v3(cosf(ca) * cd, rndf(-120, 120), sinf(ca) * cd);
    for (int i = 0; i < n; i++) {
        Rock *r = &s_rocks[i];
        float oa = rndf(0, 6.2831853f);
        float od = rndf(20, 220);
        r->pos = v3_add(centre, v3(cosf(oa) * od, rndf(-90, 90),
                                   sinf(oa) * od));
        r->vel = v3(rndf(-1.2f, 1.2f), rndf(-0.8f, 0.8f),
                    rndf(-1.2f, 1.2f));
        r->spin = rndf(0.08f, 0.4f);
        r->radius = (i < 2) ? rndf(26, 42)       /* the boulders */
                            : rndf(10, 22);
        r->hp = 40.0f + r->radius * 3.0f;
        r->chip = 0;
        r->variant = (uint8_t)(rnd() & 1);
        r->alive = 1;
    }
}

void rocks_tick(float dt) {
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &s_rocks[i];
        if (!r->alive) continue;
        r->pos = v3_add(r->pos, v3_scale(r->vel, dt));
    }
}

void rocks_render(Vec3 cam_pos, float t) {
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &s_rocks[i];
        if (!r->alive) continue;
        MoteObject obj; obj.color = 0;
        obj.mesh = &s_rm[r->variant];
        obj.basis = m3_identity();
        m3_rotate_local(&obj.basis, 1, t * r->spin);
        m3_rotate_local(&obj.basis, 0, t * r->spin * 0.6f + (float)i);
        obj.pos = v3_sub(r->pos, cam_pos);
        g_em->scene_add_object_scaled(&obj, r->radius);
    }
}

/* Ray test for hitscan weapons: returns rock index or -1; *t_out is the
 * hit distance along dir. */
int rocks_ray(Vec3 o, Vec3 dir, float max_t, float *t_out) {
    int best = -1;
    float bt = max_t;
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &s_rocks[i];
        if (!r->alive) continue;
        Vec3 oc = v3_sub(r->pos, o);
        float tca = v3_dot(oc, dir);
        if (tca < 0) continue;
        float d2 = v3_len2(oc) - tca * tca;
        float rr = r->radius * 1.1f;
        if (d2 > rr * rr) continue;
        float t = tca - sqrtf(rr * rr - d2);
        if (t >= 0 && t < bt) { bt = t; best = i; }
    }
    if (best >= 0 && t_out) *t_out = bt;
    return best;
}

/* Chip a rock: ore spills as chip damage accumulates; destruction
 * spills a bonus. yield_mult is the tool economics (user req: lasers
 * mined nearly as well as the mining laser): crude blasting VAPORIZES
 * ore — standard weapons recover ~45% per chip, the MINING laser 100%.
 * Rocks are finite, so yield is income. Returns true if it died. */
bool rocks_damage(int idx, float dmg, float yield_mult, Vec3 hit_pos) {
    Rock *r = &s_rocks[idx];
    if (!r->alive) return false;
    r->hp -= dmg;
    r->chip += dmg * yield_mult;
    fx_spawn_spark(hit_pos, r->vel);
    while (r->chip >= 24.0f) {
        r->chip -= 24.0f;
        loot_spawn_ore(v3_add(r->pos,
                              v3_scale(v3_sub(hit_pos, r->pos), 1.15f)),
                       r->vel);
    }
    if (r->hp <= 0.0f) {
        r->alive = 0;
        fx_spawn_explosion(r->pos, r->vel);
        loot_spawn_ore(r->pos, r->vel);
        loot_spawn_ore(r->pos, v3_add(r->vel, v3(2, 1, -1)));
        return true;
    }
    return false;
}

int rocks_get(int idx, Vec3 *pos, float *radius) {
    if (idx < 0 || idx >= MAX_ROCKS || !s_rocks[idx].alive) return 0;
    if (pos) *pos = s_rocks[idx].pos;
    if (radius) *radius = s_rocks[idx].radius;
    return 1;
}

int rocks_positions(Vec3 *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_ROCKS && n < max; i++)
        if (s_rocks[i].alive) out[n++] = s_rocks[i].pos;
    return n;
}
