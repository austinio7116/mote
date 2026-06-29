#include "rogue_loot.h"
#include "rogue_render.h"
#include "rogue_inventory.h"
#include "rogue_particle.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include <math.h>

#include <stdio.h>
void rogue_game_toast(const char *msg);   /* announce pickups/chests */

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define MAX_GROUND ROGUE_MAX_GROUND   /* shared with the suspend snapshot */
#define MAX_CHEST  8
#define PICKUP_R   1.4f    /* gold/potion vacuum radius */
#define INTERACT_R 1.3f

typedef struct { bool alive; RogueItem item; Vec3 pos; float spin; } Ground;
typedef struct { bool used, opened; Vec3 pos; } Chest;

static Ground s_g[MAX_GROUND];
static Chest  s_c[MAX_CHEST];
static uint32_t s_rng = 0x2468ace0u;
static uint32_t xs(void){ s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng; }
static float frand(void){ return (float)(xs() & 0xFFFF) / 65536.0f; }

void rogue_loot_clear(void) {
    for (int i = 0; i < MAX_GROUND; i++) s_g[i].alive = false;
    for (int i = 0; i < MAX_CHEST; i++)  s_c[i].used = false;
}

void rogue_loot_drop(const RogueItem *it, Vec3 pos) {
    for (int i = 0; i < MAX_GROUND; i++) {
        if (s_g[i].alive) continue;
        s_g[i].alive = true;
        s_g[i].item = *it;
        s_g[i].pos = pos;
        s_g[i].spin = frand() * 6.28f;
        return;
    }
}

void rogue_loot_place_chests(const int16_t *room_cx, const int16_t *room_cz,
                             int n_rooms, int up_x, int up_z,
                             int floor_y, int depth, uint32_t seed) {
    for (int i = 0; i < MAX_CHEST; i++) s_c[i].used = false;
    s_rng = seed ^ (0x51ED2700u * (uint32_t)(depth + 1));
    if (!s_rng) s_rng = 1;
    int want = 3 + depth / 2;
    if (want > MAX_CHEST) want = MAX_CHEST;
    int placed = 0;
    for (int a = 0; a < want * 5 && placed < want; a++) {
        int r = (int)(frand() * n_rooms);
        if (r >= n_rooms) r = n_rooms - 1;
        if (room_cx[r] == up_x && room_cz[r] == up_z) continue;
        s_c[placed].used = true;
        s_c[placed].opened = false;
        int cx = room_cx[r], cz = room_cz[r];
        float cy = (float)floor_y;
        /* ~45% of chests sit on a pedestal you must JUMP onto to reach. */
        if ((xs() % 100u) < 45u) {
            craft_world_set_byte(cx, floor_y, cz, BLK_COBBLE);  /* 1-high pedestal */
            cy = (float)(floor_y + 1);
        }
        s_c[placed].pos = v3(cx + 0.5f, cy, cz + 0.5f);
        placed++;
    }
}

void rogue_loot_update(RoguePlayer *p, float dt) {
    for (int i = 0; i < MAX_GROUND; i++) {
        Ground *g = &s_g[i];
        if (!g->alive) continue;
        g->spin += dt * 2.4f;
        float dx = g->pos.x - p->pos.x, dz = g->pos.z - p->pos.z;
        if (dx*dx + dz*dz > PICKUP_R*PICKUP_R) continue;
        uint16_t sc = rogue_item_is_equip(&g->item) ? rogue_rarity_color(g->item.rarity) : g->item.color;
        if (g->item.kind == ITEM_GOLD) {
            p->gold += g->item.amount;
            g->alive = false;
        } else if (g->item.kind == ITEM_TORCH) {
            p->torch_fuel += g->item.amount;
            if (p->torch_fuel > 90.0f) p->torch_fuel = 90.0f;
            g->alive = false;
        } else if (rogue_item_is_equip(&g->item) || g->item.kind == ITEM_GEM ||
                   g->item.kind == ITEM_POTION) {
            /* gear + gems + potions auto-collect into the backpack; potions are
             * carried and quaffed manually from the inventory (USE), not drunk
             * instantly on pickup. */
            if (rogue_inventory_add(&g->item)) {
                char msg[48]; snprintf(msg, sizeof msg, "Got %s", g->item.name);
                rogue_game_toast(msg);
                g->alive = false;
            }
        }
        if (!g->alive) {   /* just collected → a little sparkle */
            Vec3 sp = g->pos; sp.y += 0.3f;
            rogue_particle_burst(sp, 6, 3.0f, 0.30f, sc, 0.06f);
        }
    }
}

bool rogue_loot_weapon_near(float x, float z, RogueItem *out, int *out_index) {
    float best = INTERACT_R * INTERACT_R; int bi = -1;
    for (int i = 0; i < MAX_GROUND; i++) {
        if (!s_g[i].alive || !rogue_item_is_equip(&s_g[i].item)) continue;
        float dx = s_g[i].pos.x - x, dz = s_g[i].pos.z - z;
        float d = dx*dx + dz*dz;
        if (d < best) { best = d; bi = i; }
    }
    if (bi < 0) return false;
    *out = s_g[bi].item; *out_index = bi;
    return true;
}

bool rogue_loot_take(int index, RogueItem *out) {
    if (index < 0 || index >= MAX_GROUND || !s_g[index].alive) return false;
    *out = s_g[index].item;
    s_g[index].alive = false;
    return true;
}

bool rogue_loot_chest_near(float x, float y, float z, int *out_index) {
    for (int i = 0; i < MAX_CHEST; i++) {
        if (!s_c[i].used || s_c[i].opened) continue;
        float dx = s_c[i].pos.x - x, dz = s_c[i].pos.z - z;
        if (fabsf(s_c[i].pos.y - y) > 0.8f) continue;  /* must stand at its level */
        if (dx*dx + dz*dz <= INTERACT_R*INTERACT_R) { *out_index = i; return true; }
    }
    return false;
}

void rogue_loot_add_chest_at(float x, float y, float z) {
    for (int i = 0; i < MAX_CHEST; i++) {
        if (s_c[i].used) continue;
        s_c[i].used = true; s_c[i].opened = false;
        s_c[i].pos = v3(x, y, z);
        return;
    }
}

void rogue_loot_open_chest(int index, int depth, uint32_t seed) {
    if (index < 0 || index >= MAX_CHEST || !s_c[index].used || s_c[index].opened) return;
    s_c[index].opened = true;
    s_rng = seed ^ (0xC4E57u * (uint32_t)(index + 1) * (uint32_t)(depth + 2));
    if (!s_rng) s_rng = 1;
    Vec3 base = s_c[index].pos;
    /* Always a piece of gear + gold; sometimes a potion. Spills as visible
     * (beamed) drops, and announce the gear so you know what you found. */
    RogueItem it;
    rogue_item_roll_drop(&it, depth + 1, xs());   /* chest gear skews better */
    char msg[48]; snprintf(msg, sizeof msg, "Chest: %s", it.name);
    rogue_game_toast(msg);
    Vec3 a = base; a.x += 0.6f; rogue_loot_drop(&it, a);
    int g = 8 + (int)(frand() * (12 + depth * 6));
    rogue_item_make_gold(&it, g);
    Vec3 b = base; b.x -= 0.6f; rogue_loot_drop(&it, b);
    if (frand() < 0.5f) {
        rogue_item_make_potion(&it, 35);
        Vec3 c = base; c.z += 0.6f; rogue_loot_drop(&it, c);
    }
}

void rogue_loot_draw(const CraftCamera *cam, uint16_t *fb) {
    /* chests */
    for (int i = 0; i < MAX_CHEST; i++) {
        if (!s_c[i].used) continue;
        if (!s_c[i].opened) {
            /* Clear treasure chest: wood body, dark iron bands, gold lock,
             * domed lid — plus a faint gold "openable" glow column. */
            RogueCuboid m[8] = {
                { 0.0f, 0.16f, 0.0f,  0.32f, 0.16f, 0.24f, RGB(140, 92, 44) },  /* body */
                {-0.26f,0.16f, 0.0f,  0.05f, 0.17f, 0.25f, RGB(70, 46, 22)  },  /* L iron band */
                { 0.26f,0.16f, 0.0f,  0.05f, 0.17f, 0.25f, RGB(70, 46, 22)  },  /* R iron band */
                { 0.0f, 0.38f, 0.0f,  0.33f, 0.09f, 0.25f, RGB(120, 78, 38) },  /* lid */
                {-0.26f,0.38f, 0.0f,  0.05f, 0.10f, 0.26f, RGB(70, 46, 22)  },  /* lid bands */
                { 0.26f,0.38f, 0.0f,  0.05f, 0.10f, 0.26f, RGB(70, 46, 22)  },
                { 0.0f, 0.28f, 0.25f, 0.05f, 0.05f, 0.03f, RGB(240, 205, 70) }, /* gold lock */
                { 0.0f, 0.56f, 0.0f,  0.05f, 0.08f, 0.05f, RGB(240, 205, 70) }, /* lid twinkle */
            };
            rogue_render_model(cam, fb, s_c[i].pos, 0.0f, m, 8, 0.4f, 1.3f, 0.0f, 256);
        } else {
            /* opened: lid flipped back, dark interior */
            RogueCuboid m[4] = {
                { 0.0f, 0.16f, 0.0f,  0.32f, 0.16f, 0.24f, RGB(120, 78, 38) },  /* body */
                { 0.0f, 0.30f, 0.0f,  0.27f, 0.03f, 0.19f, RGB(18, 12, 6)   },  /* dark inside */
                { 0.0f, 0.40f,-0.24f, 0.32f, 0.09f, 0.05f, RGB(100, 64, 30) },  /* open lid (back) */
                {-0.26f,0.16f, 0.0f,  0.05f, 0.17f, 0.25f, RGB(60, 40, 20)  },
            };
            rogue_render_model(cam, fb, s_c[i].pos, 0.0f, m, 4, 0.4f, 0.6f, 0.0f, 256);
        }
    }
    /* ground items: a spinning item cube + a Diablo-style vertical LOOT BEAM
     * coloured by rarity (equip) or type (gold/gem/potion/torch), so drops are
     * obvious and you can read their value from across the room. */
    for (int i = 0; i < MAX_GROUND; i++) {
        Ground *g = &s_g[i];
        if (!g->alive) continue;
        /* Gold is just coin(s) on the ground — a low spinning gold disc, NO
         * loot beam, so it doesn't read like a rare/legendary drop. */
        if (g->item.kind == ITEM_GOLD) {
            float bob = 0.05f + 0.025f * sinf(g->spin * 2.0f);
            Vec3 cp = g->pos; cp.y += bob;
            RogueCuboid coin[1] = { { 0.0f, 0.05f, 0.0f, 0.13f, 0.03f, 0.13f, RGB(245, 205, 55) } };
            rogue_render_model(cam, fb, cp, g->spin, coin, 1, 0.16f, 0.16f, 0.12f, 256);
            continue;
        }
        bool eq = rogue_item_is_equip(&g->item);
        if (!eq) {
            /* Non-equipment NEVER gets a light shaft — beams are the rarity
             * language for gear, and an orange torch / gold topaz beam reads
             * as a legendary drop from across the room. Small ground models
             * instead: a mini standing torch, or a bobbing spinning trinket
             * cube (potion flask / gem) in the item's colour. */
            if (g->item.kind == ITEM_TORCH) {
                RogueCuboid tm[3] = {
                    { 0.0f, 0.16f, 0.0f, 0.030f, 0.16f, 0.030f, RGB(110, 75, 40)  },
                    { 0.0f, 0.36f, 0.0f, 0.060f, 0.05f, 0.060f, RGB(255, 150, 30) },
                    { 0.0f, 0.43f, 0.0f, 0.042f, 0.04f, 0.042f, RGB(255, 225, 120) },
                };
                rogue_render_model(cam, fb, g->pos, 0.0f, tm, 3, 0.12f, 0.55f, 0.15f, 256);
            } else {
                float bob = 0.10f + 0.05f * sinf(g->spin * 1.7f);
                Vec3 pos = g->pos; pos.y += bob;
                RogueCuboid m[1] = { { 0.0f, 0.09f, 0.0f, 0.09f, 0.09f, 0.09f, g->item.color } };
                rogue_render_model(cam, fb, pos, g->spin, m, 1, 0.15f, 0.30f, 0.25f, 256);
            }
            continue;
        }
        uint16_t c = rogue_rarity_color(g->item.rarity);
        /* Shaft of light: a tall coloured outer glow with a near-white bright
         * core rising from the drop — taller for higher rarity. Rendered in
         * two passes so the core can use a much higher emissive flash. */
        float bh = 1.4f + 0.45f * g->item.rarity;
        RogueCuboid outer[1] = { { 0.0f, bh, 0.0f, 0.07f, bh, 0.07f, c } };
        rogue_render_model(cam, fb, g->pos, g->spin * 0.25f, outer, 1, 0.12f, bh * 2.0f, 0.40f, 256);
        RogueCuboid core[1]  = { { 0.0f, bh, 0.0f, 0.025f, bh, 0.025f, c } };
        rogue_render_model(cam, fb, g->pos, 0.0f, core, 1, 0.06f, bh * 2.0f, 0.92f, 256);
        float bob = 0.12f + 0.05f * sinf(g->spin * 1.7f);
        Vec3 pos = g->pos; pos.y += bob;
        RogueCuboid m[1] = { { 0.0f, 0.10f, 0.0f, 0.10f, 0.10f, 0.10f, c } };
        rogue_render_model(cam, fb, pos, g->spin, m, 1, 0.16f, 0.30f, 0.0f, 256);
    }
}

/* --- mid-level suspend snapshot ---------------------------------------- */
int rogue_loot_export_ground(RogueGroundSave *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_GROUND && n < max; i++) {
        if (!s_g[i].alive) continue;
        out[n].it = s_g[i].item;
        out[n].pos = s_g[i].pos;
        n++;
    }
    return n;
}

void rogue_loot_import_ground(const RogueGroundSave *in, int n) {
    for (int i = 0; i < MAX_GROUND; i++) s_g[i].alive = false;
    if (n > MAX_GROUND) n = MAX_GROUND;
    for (int i = 0; i < n; i++) {
        s_g[i].alive = true;
        s_g[i].item  = in[i].it;
        s_g[i].pos   = in[i].pos;
        s_g[i].spin  = 0.0f;
    }
}

uint32_t rogue_loot_chest_mask(void) {
    uint32_t m = 0;
    for (int i = 0; i < MAX_CHEST; i++)
        if (s_c[i].used && s_c[i].opened) m |= (1u << i);
    return m;
}

void rogue_loot_apply_chest_mask(uint32_t mask) {
    for (int i = 0; i < MAX_CHEST; i++)
        if (s_c[i].used && (mask & (1u << i))) s_c[i].opened = true;
}
