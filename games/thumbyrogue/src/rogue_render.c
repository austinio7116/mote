#include "rogue_render.h"
#include <math.h>

#ifndef CRAFT_MAX_DIST_FOR_ZBUF
#define CRAFT_MAX_DIST_FOR_ZBUF 60.0f
#endif

/* Per-face brightness (top brightest, bottom darkest) — matches the look of
 * craft_mobs so entities and world blocks shade consistently. */
static const uint16_t face_shade[6] = { 220, 220, 256, 150, 200, 170 };

static inline uint16_t shade565(uint16_t c, int m) {
    int r = ((c >> 11) & 0x1F) * m >> 8;
    int g = ((c >>  5) & 0x3F) * m >> 8;
    int b = ( c        & 0x1F) * m >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline bool ray_aabb(float ox, float oy, float oz,
                            float dx, float dy, float dz,
                            float bminx, float bminy, float bminz,
                            float bmaxx, float bmaxy, float bmaxz,
                            float *t_out, int *face_out) {
    float t_near = -1e30f, t_far = 1e30f;
    int nf = -1;
    if (dx > -1e-6f && dx < 1e-6f) { if (ox < bminx || ox > bmaxx) return false; }
    else {
        float inv = 1.0f / dx, t1 = (bminx - ox) * inv, t2 = (bmaxx - ox) * inv;
        int nfc = (dx > 0) ? 1 : 0;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; nfc ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = nfc; }
        if (t2 < t_far) t_far = t2;
        if (t_near > t_far) return false;
    }
    if (dy > -1e-6f && dy < 1e-6f) { if (oy < bminy || oy > bmaxy) return false; }
    else {
        float inv = 1.0f / dy, t1 = (bminy - oy) * inv, t2 = (bmaxy - oy) * inv;
        int nfc = (dy > 0) ? 3 : 2;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; nfc ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = nfc; }
        if (t2 < t_far) t_far = t2;
        if (t_near > t_far) return false;
    }
    if (dz > -1e-6f && dz < 1e-6f) { if (oz < bminz || oz > bmaxz) return false; }
    else {
        float inv = 1.0f / dz, t1 = (bminz - oz) * inv, t2 = (bmaxz - oz) * inv;
        int nfc = (dz > 0) ? 5 : 4;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; nfc ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = nfc; }
        if (t2 < t_far) t_far = t2;
        if (t_near > t_far) return false;
    }
    if (t_near < 0.0f) return false;
    *t_out = t_near;
    *face_out = nf;
    return true;
}

void rogue_render_model(const CraftCamera *cam, uint16_t *fb,
                        Vec3 pos, float yaw,
                        const RogueCuboid *parts, int n_parts,
                        float radius, float height,
                        float flash, int tint_q8) {
    float cy_c = cosf(cam->yaw),  sy_c = sinf(cam->yaw);
    float cp_c = cosf(cam->pitch), sp_c = sinf(cam->pitch);
    Vec3 fwd   = v3(sy_c * cp_c, sp_c, cy_c * cp_c);
    Vec3 right = v3(cy_c, 0.0f, -sy_c);
    Vec3 up    = v3(fwd.y * right.z - fwd.z * right.y,
                    fwd.z * right.x - fwd.x * right.z,
                    fwd.x * right.y - fwd.y * right.x);
    float tan_h  = tanf(cam->fov * 0.5f);
    float aspect = (float)CRAFT_FB_W / (float)CRAFT_FB_H;
    float focal_h = (CRAFT_FB_H * 0.5f) / tan_h;
    float focal_v = (CRAFT_FB_H * 0.5f) / tan_h;

    /* Project the model's world AABB to a screen bbox. */
    float wbmin_x = pos.x - radius, wbmax_x = pos.x + radius;
    float wbmin_y = pos.y,          wbmax_y = pos.y + height;
    float wbmin_z = pos.z - radius, wbmax_z = pos.z + radius;
    int sx_min = CRAFT_FB_W, sx_max = -1, sy_min = CRAFT_FB_H, sy_max = -1;
    bool any_in_front = false;
    for (int corner = 0; corner < 8; corner++) {
        float cx  = (corner & 1) ? wbmax_x : wbmin_x;
        float cyw = (corner & 2) ? wbmax_y : wbmin_y;
        float cz  = (corner & 4) ? wbmax_z : wbmin_z;
        float rx = cx - cam->pos.x, ry = cyw - cam->pos.y, rz = cz - cam->pos.z;
        float zf = rx * fwd.x + ry * fwd.y + rz * fwd.z;
        if (zf <= 0.05f) continue;
        any_in_front = true;
        float xs = (rx * right.x + ry * right.y + rz * right.z) / zf;
        float ys = (rx * up.x    + ry * up.y    + rz * up.z   ) / zf;
        int sx = (int)(CRAFT_FB_W * 0.5f + xs * focal_h);
        int sy = (int)(CRAFT_FB_H * 0.5f - ys * focal_v);
        if (sx < sx_min) sx_min = sx;
        if (sx > sx_max) sx_max = sx;
        if (sy < sy_min) sy_min = sy;
        if (sy > sy_max) sy_max = sy;
    }
    if (!any_in_front) return;
    sx_min--; sy_min--; sx_max++; sy_max++;
    if (sx_min < 0) sx_min = 0;
    if (sy_min < 0) sy_min = 0;
    if (sx_max >= CRAFT_FB_W) sx_max = CRAFT_FB_W - 1;
    if (sy_max >= CRAFT_FB_H) sy_max = CRAFT_FB_H - 1;
    if (sx_min > sx_max || sy_min > sy_max) return;

    /* Camera position in model-local frame. The game's forward convention is
     * (sin yaw, cos yaw) — the same one movement, the melee hit-arc and
     * projectiles use — so local +z must map to that world direction. (This
     * only spins each model about its own centre; screen position comes from
     * the world-space AABB above, so nothing moves left/right.) */
    float my_c = cosf(yaw), my_s = sinf(yaw);
    float rel_x = cam->pos.x - pos.x;
    float rel_y = cam->pos.y - pos.y;
    float rel_z = cam->pos.z - pos.z;
    float lo_x = rel_x * my_c - rel_z * my_s;
    float lo_y = rel_y;
    float lo_z = rel_x * my_s + rel_z * my_c;

    int flash_q8 = (int)(flash * 256.0f);
    if (flash_q8 < 0) flash_q8 = 0;
    if (flash_q8 > 256) flash_q8 = 256;

    for (int sy = sy_min; sy <= sy_max; sy++) {
        float ndc_y = -((float)(sy * 2 - CRAFT_FB_H + 1) / (float)CRAFT_FB_H);
        float vy = ndc_y * tan_h;
        for (int sx = sx_min; sx <= sx_max; sx++) {
            float ndc_x = ((float)(sx * 2 - CRAFT_FB_W + 1) / (float)CRAFT_FB_W);
            float vx = ndc_x * tan_h * aspect;
            float wdx = fwd.x + right.x * vx + up.x * vy;
            float wdy = fwd.y + right.y * vx + up.y * vy;
            float wdz = fwd.z + right.z * vx + up.z * vy;
            float ldx = wdx * my_c - wdz * my_s;
            float ldy = wdy;
            float ldz = wdx * my_s + wdz * my_c;

            float best_t = 1e30f;
            int best_face = 0;
            uint16_t best_color = 0;
            for (int p = 0; p < n_parts; p++) {
                const RogueCuboid *part = &parts[p];
                float t; int face;
                if (ray_aabb(lo_x, lo_y, lo_z, ldx, ldy, ldz,
                             part->cx - part->hx, part->cy - part->hy, part->cz - part->hz,
                             part->cx + part->hx, part->cy + part->hy, part->cz + part->hz,
                             &t, &face)) {
                    if (t < best_t) { best_t = t; best_face = face; best_color = part->color; }
                }
            }
            if (best_t >= 1e29f) continue;

            if (flash_q8 > 0) {
                int r = ((best_color >> 11) & 0x1F);
                int g = ((best_color >>  5) & 0x3F);
                int b = ( best_color        & 0x1F);
                r = r + ((31 - r) * flash_q8 >> 8);
                g = g - (g * flash_q8 >> 9);
                b = b - (b * flash_q8 >> 9);
                if (r > 31) r = 31; if (g < 0) g = 0; if (b < 0) b = 0;
                best_color = (uint16_t)((r << 11) | (g << 5) | b);
            }

            float dl2 = wdx*wdx + wdy*wdy + wdz*wdz;
            float dl  = (dl2 > 1.0001f) ? sqrtf(dl2) : 1.0f;
            float world_dist = best_t * dl;
            int q = (int)(world_dist * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
            if (q < 0) q = 0;
            if (q > 254) q = 254;
            int idx = sy * CRAFT_FB_W + sx;
            if (craft_zbuf[idx] > (uint8_t)q) {
                int sh = face_shade[best_face] * tint_q8 >> 8;
                fb[idx] = shade565(best_color, sh);
                craft_zbuf[idx] = (uint8_t)q;
            }
        }
    }
}
