/*
 * Rendering: map, actors, HUD, message log.
 *
 * The map goes through the engine's autotile layers, so the terrain is only
 * ever a logical byte map -- no resolved tilemap is stored. Fog of war works by
 * rebuilding a DISPLAY layer map from terrain + the KNOWN flag, so unknown
 * cells simply have no layer bit set and draw as background.
 */
#include "rl.h"

#include "wall_brick.tiles.h"
#include "wall_bone.tiles.h"
#include "wall_marble.tiles.h"
#include "wall_aztec.tiles.h"
#include "floor_cobble.tiles.h"
#include "floor_jungle.tiles.h"

#include "animals.h"
#include "monsters.h"
#include "bosses.h"
#include "characters.h"
#include "props_light.h"
#include "doors_gems_banners.h"
#include "ui_status_emotes.h"
#include "rogue8.font.h"

static const MoteImage *const s_sheet[] = {
    &animals_img, &monsters_img, &bosses_img, &characters_img,
};
#define SHEET_COLS 16

/* Wall identity per depth band -- descending should LOOK like descending.
 * See DESIGN.md section 3. */
static const MoteAutotile *wall_for_depth(int d) {
    if (d <= 10) return &wall_brick_at;
    if (d <= 20) return &wall_bone_at;
    if (d <= 30) return &wall_marble_at;
    return &wall_aztec_at;
}
static const MoteAutotile *floor_for_depth(int d) {
    return (d == 0) ? &floor_jungle_at : &floor_cobble_at;
}

/* --- message log -------------------------------------------------------- */
#define MSG_N 3
#define MSG_LEN 20   /* rogue8 is proportional; 20 chars is the safe fit */
static char s_msg[MSG_N][MSG_LEN];
static int  s_msg_age[MSG_N];

void rl_msg(const char *s) {
    for (int i = MSG_N - 1; i > 0; i--) {
        for (int j = 0; j < MSG_LEN; j++) s_msg[i][j] = s_msg[i - 1][j];
        s_msg_age[i] = s_msg_age[i - 1];
    }
    int j = 0;
    while (s && s[j] && j < MSG_LEN - 1) { s_msg[0][j] = s[j]; j++; }
    s_msg[0][j] = 0;
    s_msg_age[0] = 0;
}

/* Join two strings into one log line ("kobold" + " dies.") -- combat lines are
 * mostly a monster name plus a fixed suffix. */
void rl_msg2(const char *a, const char *b) {
    char buf[MSG_LEN]; int o = 0;
    for (int i = 0; a[i] && o < MSG_LEN - 1; i++) buf[o++] = a[i];
    for (int i = 0; b[i] && o < MSG_LEN - 1; i++) buf[o++] = b[i];
    buf[o] = 0;
    rl_msg(buf);
}

/* tiny int-substituting formatter: the one '%d' in `fmt` is replaced by `a`.
 * Avoids pulling in printf for a handful of combat lines. */
void rl_msgf(const char *fmt, int a) {
    char buf[MSG_LEN]; int o = 0;
    for (int i = 0; fmt[i] && o < MSG_LEN - 1; i++) {
        if (fmt[i] == '%' && fmt[i + 1] == 'd') {
            int v = a, digits[8], n = 0;
            if (v < 0) { buf[o++] = '-'; v = -v; }
            do { digits[n++] = v % 10; v /= 10; } while (v && n < 8);
            while (n-- > 0 && o < MSG_LEN - 1) buf[o++] = (char)('0' + digits[n]);
            i++;
        } else buf[o++] = fmt[i];
    }
    buf[o] = 0;
    rl_msg(buf);
}

/* --- scene -------------------------------------------------------------- */
void rl_draw_scene(void) {
    /* camera centres on the player, clamped so the view never leaves the map */
    int cx = g_pl.x * TS + TS / 2 - (VIEW_W * TS) / 2;
    int cy = g_pl.y * TS + TS / 2 - (VIEW_H * TS) / 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx > MW * TS - VIEW_W * TS) cx = MW * TS - VIEW_W * TS;
    if (cy > MH * TS - VIEW_H * TS) cy = MH * TS - VIEW_H * TS;

    /* Rebuild the display layer map: bit0 = floor, bit1 = wall. Only KNOWN
     * cells get bits, so undiscovered map is simply absent. */
    for (int i = 0; i < MW * MH; i++) {
        uint8_t v = 0;
        if (g_lv.flags[i] & CF_KNOWN) {
            uint8_t t = g_lv.terrain[i];
            v = 1;                                    /* floor under everything */
            if (t == T_WALL || t == T_RUBBLE) v |= 2;
        }
        g_lv.layer[i] = v;
    }

    static const MoteAutotile *tiles[2];
    tiles[0] = floor_for_depth(g_pl.depth);
    tiles[1] = wall_for_depth(g_pl.depth);

    g_api->scene2d_begin(cx, cy);
    g_api->scene2d_set_autotile_layers(g_lv.layer, MW, MH, tiles, 2);

    /* features that are sprites rather than terrain */
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            int i = y * MW + x;
            if (!(g_lv.flags[i] & CF_KNOWN)) continue;
            uint8_t t = g_lv.terrain[i];
            const MoteImage *img = 0; int cell = 0;
            if (t == T_STAIR_DOWN)      { img = &props_light_img;       cell = 0; }
            else if (t == T_STAIR_UP)   { img = &props_light_img;       cell = 1; }
            else if (t == T_DOOR_CLOSED){ img = &doors_gems_banners_img; cell = 4; }
            else if (t == T_DOOR_OPEN)  { img = &doors_gems_banners_img; cell = 5; }
            if (!img) continue;
            int cols = img->w / TS;
            MoteSprite s = { img, (int16_t)(x * TS), (int16_t)(y * TS),
                             (uint16_t)((cell % cols) * TS), (uint16_t)((cell / cols) * TS),
                             TS, TS, 8, 0 };
            g_api->scene2d_add(&s);
        }
    }

    /* items, then monsters, then the player: layer order gives the stacking */
    for (int i = 0; i < g_lv.n_mon; i++) {
        Mon *m = &g_lv.mon[i];
        if (m->hp <= 0) continue;
        if (!(g_lv.flags[m->y * MW + m->x] & CF_VISIBLE)) continue;
        const MonKind *mk = &g_mon_kind[m->kind];
        const MoteImage *img = s_sheet[mk->sheet];
        MoteSprite s = { img, (int16_t)(m->x * TS), (int16_t)(m->y * TS),
                         (uint16_t)((mk->cell % SHEET_COLS) * TS),
                         (uint16_t)((mk->cell / SHEET_COLS) * TS),
                         TS, TS, 20, 0 };
        g_api->scene2d_add(&s);
    }

    MoteSprite p = { &characters_img, (int16_t)(g_pl.x * TS), (int16_t)(g_pl.y * TS),
                     0, 0, TS, TS, 30, 0 };   /* cell 0 = the '@' glyph */
    g_api->scene2d_add(&p);
}

/* --- HUD ---------------------------------------------------------------- */
static void bar(uint16_t *fb, int x, int y, int w, int cur, int max, uint16_t col) {
    if (max <= 0) max = 1;
    int fill = (cur * w) / max;
    if (fill < 0) fill = 0;
    if (fill > w) fill = w;
    g_api->draw_rect(fb, x, y, w, 4, MOTE_RGB565(40, 40, 56), 1, 0, MOTE_FB_H);
    if (fill) g_api->draw_rect(fb, x, y, fill, 4, col, 1, 0, MOTE_FB_H);
}

void rl_draw_hud(uint16_t *fb) {
    g_api->draw_rect(fb, 0, HUD_Y, MOTE_FB_W, MOTE_FB_H - HUD_Y,
                     MOTE_RGB565(16, 14, 24), 1, 0, MOTE_FB_H);
    g_api->draw_line(fb, 0, HUD_Y, MOTE_FB_W - 1, HUD_Y, MOTE_RGB565(90, 86, 120), 0, MOTE_FB_H);

    /* Row 1: two bars and the depth, sized so nothing reaches the right edge.
     * 24px of HUD is about two and a half rogue8 rows, so the log gets one. */
    bar(fb, 2,  HUD_Y + 3, 40, g_pl.hp, g_pl.mhp, MOTE_RGB565(200, 40, 60));
    bar(fb, 46, HUD_Y + 3, 40, g_pl.sp, g_pl.msp, MOTE_RGB565(60, 110, 220));

    char b[12]; int o = 0, v = g_pl.depth;
    b[o++] = 'D'; b[o++] = 'L';
    if (v >= 10) b[o++] = (char)('0' + v / 10);
    b[o++] = (char)('0' + v % 10); b[o] = 0;
    g_api->text_font(fb, &rogue8, b, 92, HUD_Y + 1, MOTE_RGB565(220, 214, 200));

    rl_draw_msgs(fb);
}

void rl_draw_msgs(uint16_t *fb) {
    uint16_t col[MSG_N] = { MOTE_RGB565(235, 230, 215),
                            MOTE_RGB565(150, 146, 170),
                            MOTE_RGB565(100, 98, 120) };
    for (int i = 0; i < MSG_N; i++) {
        if (!s_msg[i][0]) continue;
        g_api->text_font(fb, &rogue8, s_msg[i], 2, HUD_Y + 11, col[i]);
        break;                       /* one line of log; older lines fade out */
    }
}
