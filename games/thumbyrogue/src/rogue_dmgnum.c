#include "rogue_dmgnum.h"
#include "craft_font.h"
#include <stdio.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define MAX_DN 18
#define DN_LIFE 0.75f
#define DN_RISE 1.7f      /* world units/sec the number floats upward */

typedef struct {
    bool  alive;
    Vec3  pos;
    float t;
    int   value;
    bool  taken;
    float jx, jz;         /* small spawn offset so stacked hits don't overlap */
} DmgNum;

static DmgNum   s_dn[MAX_DN];
static uint32_t s_seed = 0x1234567u;

static float frnd(void) {
    s_seed ^= s_seed << 13; s_seed ^= s_seed >> 17; s_seed ^= s_seed << 5;
    return (float)(s_seed & 0xFFFF) / 65535.0f - 0.5f;   /* -0.5..0.5 */
}

void rogue_dmgnum_clear(void) {
    for (int i = 0; i < MAX_DN; i++) s_dn[i].alive = false;
}

void rogue_dmgnum_spawn(Vec3 pos, int value, bool taken) {
    if (value <= 0) return;
    int idx = -1; float oldest = -1.0f;
    for (int i = 0; i < MAX_DN; i++) {
        if (!s_dn[i].alive) { idx = i; break; }
        if (s_dn[i].t > oldest) { oldest = s_dn[i].t; idx = i; }   /* recycle the oldest */
    }
    DmgNum *d = &s_dn[idx];
    d->alive = true;
    d->pos = pos; d->pos.y += 0.55f;
    d->t = 0; d->value = value; d->taken = taken;
    d->jx = frnd() * 0.5f; d->jz = frnd() * 0.5f;
}

void rogue_dmgnum_update(float dt) {
    for (int i = 0; i < MAX_DN; i++) {
        DmgNum *d = &s_dn[i];
        if (!d->alive) continue;
        d->t += dt;
        d->pos.y += DN_RISE * dt;
        if (d->t >= DN_LIFE) d->alive = false;
    }
}

void rogue_dmgnum_draw(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < MAX_DN; i++) {
        DmgNum *d = &s_dn[i];
        if (!d->alive) continue;
        Vec3 p = d->pos; p.x += d->jx; p.z += d->jz;
        int sx, sy; uint8_t depth; float dist;
        if (!craft_render_project(cam, p, &sx, &sy, &depth, &dist)) continue;
        /* fade out by darkening over life (no alpha in the font path) */
        float k = 1.0f - d->t / DN_LIFE;       /* 1 -> 0 */
        if (k < 0.0f) k = 0.0f;
        int r, g, b;
        if (d->taken) { r = 255; g = 80; b = 70; }   /* damage taken — red   */
        else          { r = 90;  g = 240; b = 90; }  /* damage dealt — green */
        r = (int)(r * k); g = (int)(g * k); b = (int)(b * k);
        char buf[8];
        snprintf(buf, sizeof buf, "%d", d->value);
        int w = craft_font_width(buf);
        craft_font_draw(fb, buf, sx - w / 2, sy - 3, RGB(r, g, b));
    }
}
