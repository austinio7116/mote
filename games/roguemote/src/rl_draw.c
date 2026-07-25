/*
 * Rendering: map, actors, items, HUD, message log.
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
#include "wall_plaster.tiles.h"
#include "floor_cobble.tiles.h"
#include "floor_jungle.tiles.h"
#include "floor_grass.tiles.h"

#include "animals.h"
#include "monsters.h"
#include "bosses.h"
#include "characters.h"
#include "weapons_potions.h"
#include "tools_wands.h"
#include "weapons_elemental.h"
#include "treasure_ore.h"
#include "crowns_fx.h"
#include "food.h"
#include "runes.h"
#include "loot_furniture.h"
#include "trinkets.h"
#include "props_light.h"
#include "doors_gems_banners.h"
#include "chests.h"
#include "boulders_mountains.h"
#include "fx_mono.h"
#include "dungeon_mono.h"
#include "rogue8.font.h"

/* Order must match the SH_* enum in rl.h. */
static const MoteImage *const s_sheet[SH_COUNT] = {
    &animals_img, &monsters_img, &bosses_img, &characters_img,
    &weapons_potions_img, &tools_wands_img, &weapons_elemental_img,
    &treasure_ore_img, &crowns_fx_img, &food_img, &runes_img,
    &loot_furniture_img, &trinkets_img, &props_light_img,
    &doors_gems_banners_img, &chests_img, &boulders_mountains_img,
    &fx_mono_img, &dungeon_mono_img,
};

const MoteImage *rl_sheet(int id) {
    return (id >= 0 && id < SH_COUNT) ? s_sheet[id] : s_sheet[0];
}

/* Feature sprites, named so the intent survives a re-bake.
 *
 * Stairs come from the monochrome dungeon band: (4,49) is a frame around two
 * descending risers, (5,49) around three ascending ones. The doors sheet's
 * wooden ladders were standing in for these, and at 8px a ladder reads as a
 * fence, not as a way down. Doors stay in the doors sheet (source cols 11-14).*/
#define SPR_STAIR_DOWN   36     /* dungeon_mono (4,49) */
#define SPR_STAIR_UP     37     /* dungeon_mono (5,49) */
#define SPR_DOOR_CLOSED   4
#define SPR_DOOR_OPEN     5
/* The town marker and the shopfronts come from labels_human.json, not from a
 * guess: (1,9) is the mushroom-roofed cottage, and (13,1)..(13,5) are the five
 * coloured house/shop fronts the annotator pass identified. props_light 32 --
 * what the town used before -- is the igloo. */
#define SPR_HOUSE        31     /* props_light (1,9): cottage */
/* The mine entrance is the same framed stairs-down glyph used underground.
 * boulders_mountains cell 6 -- what this was -- is one quadrant of a 2x3
 * boulder, which on its own is a flat grey square and reads as a bug. Reusing
 * the stairs glyph also means the player learns one symbol, not two. */
#define SPR_CAVE_MOUTH   SPR_STAIR_DOWN

/* Wall identity per depth band -- descending should LOOK like descending.
 * See DESIGN.md section 3. */
static const MoteAutotile *wall_for_depth(int d) {
    if (d <= 8)  return &wall_brick_at;
    if (d <= 16) return &wall_bone_at;
    if (d <= 24) return &wall_marble_at;
    if (d <= 32) return &wall_aztec_at;
    return &wall_plaster_at;
}
/* One floor for the whole dungeon, and a dark one. The walls are busy textures
 * at every depth, so anything but a near-black floor leaves the room boundary
 * unresolved and a monster with nothing to read against -- which is what the
 * first two attempts at this got wrong. The wall changes with depth; that is
 * enough of a progression without the ground moving too. */
static const MoteAutotile *floor_for_depth(int d) {
    (void)d;
    return &floor_cobble_at;
}

/* --- tiny text helpers --------------------------------------------------
 * Body text uses the engine's built-in 3x5 font, not the tileset's rogue8.
 * rogue8 is a genuine 8x8 CP437 face and advances 6-9px per glyph, which caps a
 * full-width line at about 17 characters -- not enough for "studded leather
 * +3 (Frost)" or a shop row with a price. rogue8 stays for headings, where the
 * weight is worth the width. */
void rl_text(uint16_t *fb, const char *s, int x, int y, uint16_t col) {
    g_api->text(fb, s, x, y, col);
}

void rl_text_big(uint16_t *fb, const char *s, int x, int y, uint16_t col) {
    g_api->text_font(fb, &rogue8, s, x, y, col);
}

void rl_num(uint16_t *fb, int32_t v, int x, int y, uint16_t col) {
    char b[12]; int o = 0, digits[10], n = 0;
    int32_t u = v;
    if (u < 0) { b[o++] = '-'; u = -u; }
    do { digits[n++] = (int)(u % 10); u /= 10; } while (u && n < 10);
    while (n-- > 0) b[o++] = (char)('0' + digits[n]);
    b[o] = 0;
    rl_text(fb, b, x, y, col);
}

/* Direct framebuffer blit of one sheet cell -- used by menus and the FX layer,
 * which draw in overlay() where the scene list is no longer accepting sprites. */
void rl_blit_cell(uint16_t *fb, int sheet, int cell, int x, int y) {
    const MoteImage *img = rl_sheet(sheet);
    int cols = img->w / TS;
    if (cols < 1) cols = 1;
    g_api->blit(fb, img, x, y, (cell % cols) * TS, (cell / cols) * TS, TS, TS, 0, 0, MOTE_FB_H);
}

/* --- message log -------------------------------------------------------- */
#define MSG_N 3
#define MSG_LEN 31   /* the 3x5 font advances 4px, so ~31 chars span the screen */
static char s_msg[MSG_N][MSG_LEN];

void rl_msg(const char *s) {
    for (int i = MSG_N - 1; i > 0; i--)
        for (int j = 0; j < MSG_LEN; j++) s_msg[i][j] = s_msg[i - 1][j];
    int j = 0;
    while (s && s[j] && j < MSG_LEN - 1) { s_msg[0][j] = s[j]; j++; }
    s_msg[0][j] = 0;
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
static void add_cell(int sheet, int cell, int wx, int wy, int layer) {
    const MoteImage *img = rl_sheet(sheet);
    int cols = img->w / TS;
    if (cols < 1) cols = 1;
    MoteSprite s = { img, (int16_t)wx, (int16_t)wy,
                     (uint16_t)((cell % cols) * TS), (uint16_t)((cell / cols) * TS),
                     TS, TS, (uint8_t)layer, 0 };
    g_api->scene2d_add(&s);
}

/* Overworld elevation, three stacked blob47 layers:
 *
 *   bit0  floor_grass   flat ground everywhere
 *   bit1  floor_jungle  HILLS -- a vegetated rim with a gold terraced
 *                       escarpment along its lower edge
 *   bit2  wall_aztec    MOUNTAINS -- a dark plateau wall with orange steps
 *
 * A mountain cell sets the hill bit too, because the wall bands' interior cell
 * is transparent by design (they are facades, not fills -- gen_terrain declares
 * it as hole 46). Stacking them puts the hill fill inside the mountain rim, so
 * a range reads as a plateau with cliff faces rather than an empty outline.
 *
 * Forest stays a sprite: the hedge ruleset is a hedge, whose interior cells are
 * open ground, so a smoothed forest mass through it comes out as a hollow green
 * rectangle. Woodland wants individual canopies.
 *
 * There is no water layer -- the source tileset has no water anywhere. Rows
 * 34-43 cols 48-63 are the monochrome VFX strips, and none of the eight
 * blob47/EDGE16 bands is a water set. */
static void build_layers_overworld(void) {
    for (int i = 0; i < MW * MH; i++) {
        uint8_t t = g_lv.terrain[i], v = 1;
        if (t == T_HILL)     v |= 2;
        if (t == T_MOUNTAIN) v |= 2 | 4;
        g_lv.layer[i] = v;
    }
}

static unsigned pos_hash(int x, int y) {
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u;
    h ^= h >> 13; h *= 1274126177u;
    return h;
}

/* Three canopies picked by position, so a wood is not a stamped pattern.
 * trinkets 70/71 are the round and fir canopies, 72 the bare trunk -- weighted
 * so a wood is mostly alive with the odd dead tree in it. */
static int tree_cell(int x, int y) {
    static const uint8_t k[8] = { 70, 71, 70, 71, 70, 71, 71, 72 };
    return k[(pos_hash(x, y) >> 11) & 7];
}

/* Ground detail on open plain. (4,42) is the ONLY tile in the source's grass
 * band made purely of the two greens, so the fill has exactly one variant and
 * an open plain is a flat colour field. The texture has to come from what is
 * scattered on it: flowers, tufts, reeds and stones from the trinkets sheet, at
 * about one tile in eight, chosen by position so it is stable frame to frame. */
static int decor_cell(int x, int y) {
    static const uint8_t k[8] = { 48, 49, 52, 55, 57, 66, 67, 69 };
    unsigned h = pos_hash(x, y);
    if ((h & 7) != 0) return -1;              /* ~12% of open ground */
    return k[(h >> 9) & 7];
}

static void build_layers_dungeon(void) {
    for (int i = 0; i < MW * MH; i++) {
        uint8_t v = 0;
        if (g_lv.flags[i] & CF_KNOWN) {
            uint8_t t = g_lv.terrain[i];
            v = 1;                                    /* floor under everything */
            if (t == T_WALL || t == T_RUBBLE) v |= 2;
        }
        g_lv.layer[i] = v;
    }
}

void rl_draw_scene(void) {
    /* camera centres on the player, clamped so the view never leaves the map */
    int cx = g_pl.x * TS + TS / 2 - (VIEW_W * TS) / 2;
    int cy = g_pl.y * TS + TS / 2 - (VIEW_H * TS) / 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx > MW * TS - VIEW_W * TS) cx = MW * TS - VIEW_W * TS;
    if (cy > MH * TS - VIEW_H * TS) cy = MH * TS - VIEW_H * TS;

    static const MoteAutotile *tiles[3];
    int n_layer;
    if (g_pl.depth == 0) {
        build_layers_overworld();
        tiles[0] = &floor_grass_at;
        tiles[1] = &floor_jungle_at;
        tiles[2] = &wall_aztec_at;
        n_layer = 3;
    } else {
        build_layers_dungeon();
        tiles[0] = floor_for_depth(g_pl.depth);
        tiles[1] = wall_for_depth(g_pl.depth);
        n_layer = 2;
    }

    g_api->scene2d_begin(cx, cy);
    g_api->scene2d_set_autotile_layers(g_lv.layer, MW, MH, tiles, n_layer);

    /* Only the visible window is worth walking: at 64x48 the full sweep is
     * 3072 cells per frame for at most 16x13 of visible output. */
    int x0 = cx / TS, y0 = cy / TS;
    int x1 = x0 + VIEW_W + 1, y1 = y0 + VIEW_H + 1;
    if (x1 > MW) x1 = MW;
    if (y1 > MH) y1 = MH;

    /* features that are sprites rather than terrain */
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int i = y * MW + x;
            if (!(g_lv.flags[i] & CF_KNOWN)) continue;
            int sheet = SH_DOORS, cell = -1;
            switch (g_lv.terrain[i]) {
            case T_STAIR_DOWN:   sheet = SH_DUNGEON; cell = SPR_STAIR_DOWN; break;
            case T_STAIR_UP:     sheet = SH_DUNGEON; cell = SPR_STAIR_UP;   break;
            case T_DOOR_CLOSED:  cell = SPR_DOOR_CLOSED; break;
            case T_DOOR_OPEN:    cell = SPR_DOOR_OPEN;   break;
            case T_TREE:         sheet = SH_TRINKETS; cell = tree_cell(x, y); break;
            case T_TOWN:         sheet = SH_PROPS;    cell = SPR_HOUSE;      break;
            case T_SHOP:         sheet = SH_DOORS;    cell = g_shop_cell[0]; break;
            case T_DUNGEON_MOUTH:sheet = SH_DUNGEON;  cell = SPR_CAVE_MOUTH; break;
            case T_FLOOR:
                if (g_pl.depth == 0) { sheet = SH_TRINKETS; cell = decor_cell(x, y); }
                break;
            default: break;
            }
            if (cell >= 0) add_cell(sheet, cell, x * TS, y * TS, 8);
        }
    }

    /* items on the floor: remembered cells show them even out of sight, which
     * is how you plan a route back to the thing you could not carry */
    for (int i = 0; i < g_lv.n_item; i++) {
        Item *it = &g_lv.item[i];
        if (!it->qty) continue;
        if (!(g_lv.flags[it->y * MW + it->x] & CF_KNOWN)) continue;
        const ItemKind *ik = &g_item_kind[it->kind];
        add_cell(ik->sheet, ik->cell, it->x * TS, it->y * TS, 12);
    }

    /* monsters, then the player: layer order gives the stacking */
    for (int i = 0; i < g_lv.n_mon; i++) {
        Mon *m = &g_lv.mon[i];
        if (m->hp <= 0) continue;
        if (!(g_lv.flags[m->y * MW + m->x] & CF_VISIBLE)) continue;
        if (m->boss) {
            /* a boss is one 16x16 frame from the top-left of its 2x2 block,
             * drawn up-and-left so its feet stay on its own tile */
            const BossKind *bk = &g_boss_kind[m->boss - 1];
            const MoteImage *img = rl_sheet(SH_BOSSES);
            int cols = img->w / TS;
            MoteSprite s = { img, (int16_t)(m->x * TS - TS), (int16_t)(m->y * TS - TS),
                             (uint16_t)((bk->cell % cols) * TS),
                             (uint16_t)((bk->cell / cols) * TS),
                             TS * 2, TS * 2, 22, 0 };
            g_api->scene2d_add(&s);
        } else {
            const MonKind *mk = &g_mon_kind[m->kind];
            add_cell(mk->sheet, mk->cell, m->x * TS, m->y * TS, 20);
        }
    }

    add_cell(SH_CHARACTERS, g_class[g_pl.cls].cell, g_pl.x * TS, g_pl.y * TS, 30);
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

    /* Row 1: hp/sp bars with their numbers, then level, depth and state flags.
     * 24px of HUD is three 3x5 rows, so the log gets the bottom two. */
    bar(fb, 2,  HUD_Y + 2, 30, g_pl.hp, g_pl.mhp, MOTE_RGB565(200, 40, 60));
    rl_num(fb, g_pl.hp, 34, HUD_Y + 2, MOTE_RGB565(230, 130, 140));
    bar(fb, 50, HUD_Y + 2, 22, g_pl.sp, g_pl.msp, MOTE_RGB565(60, 110, 220));
    rl_num(fb, g_pl.sp, 74, HUD_Y + 2, MOTE_RGB565(130, 170, 240));

    /* 90, not 86: a three-digit SP total reaches x=86 and collides with the L */
    rl_text(fb, "L", 90, HUD_Y + 2, MOTE_RGB565(150, 146, 170));
    rl_num(fb, g_pl.level, 95, HUD_Y + 2, MOTE_RGB565(220, 214, 200));

    if (g_pl.depth == 0) rl_text(fb, "TOWN", 104, HUD_Y + 2, MOTE_RGB565(150, 220, 160));
    else {
        rl_text(fb, "DL", 104, HUD_Y + 2, MOTE_RGB565(150, 146, 170));
        rl_num(fb, g_pl.depth, 113, HUD_Y + 2, MOTE_RGB565(220, 214, 200));
    }

    /* starvation and haste are the two states worth a glyph */
    if (g_pl.food < 200)  rl_text(fb, "*", 124, HUD_Y + 2, MOTE_RGB565(230, 120, 60));
    else if (g_pl.haste)  rl_text(fb, ">", 124, HUD_Y + 2, MOTE_RGB565(250, 220, 90));

    rl_draw_msgs(fb);
}

/* --- the map screen -----------------------------------------------------
 * The whole level at 2px a cell (64x48 -> 128x96). On the surface this is the
 * only way to find a cave mouth without walking the coastline; underground it
 * is the classic full-level map, showing only what you have seen. */
void rl_draw_map(uint16_t *fb, int y0) {
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            int i = y * MW + x;
            uint16_t c;
            if (g_pl.depth && !(g_lv.flags[i] & CF_KNOWN)) continue;
            switch (g_lv.terrain[i]) {
            case T_TREE:          c = MOTE_RGB565(0, 105, 60);   break;
            case T_HILL:          c = MOTE_RGB565(110, 205, 105);break;
            case T_MOUNTAIN:      c = MOTE_RGB565(150, 110, 60); break;
            case T_TOWN:          c = MOTE_RGB565(255, 210, 90); break;
            case T_DUNGEON_MOUTH: c = MOTE_RGB565(240, 90, 60);  break;
            case T_WALL:
            case T_RUBBLE:        c = MOTE_RGB565(64, 60, 84);   break;
            case T_STAIR_DOWN:    c = MOTE_RGB565(240, 90, 60);  break;
            case T_STAIR_UP:      c = MOTE_RGB565(90, 200, 240); break;
            case T_DOOR_CLOSED:
            case T_DOOR_OPEN:     c = MOTE_RGB565(180, 130, 60); break;
            default: c = g_pl.depth ? MOTE_RGB565(150, 146, 160)
                                    : MOTE_RGB565(0, 135, 81);   break;
            }
            g_api->draw_rect(fb, x * 2, y0 + y * 2, 2, 2, c, 1, 0, MOTE_FB_H);
        }
    }
    /* the player last, so nothing paints over it */
    g_api->draw_rect(fb, g_pl.x * 2 - 1, y0 + g_pl.y * 2 - 1, 4, 4,
                     MOTE_RGB565(255, 255, 255), 1, 0, MOTE_FB_H);
}

void rl_draw_msgs(uint16_t *fb) {
    if (s_msg[0][0]) rl_text(fb, s_msg[0], 2, HUD_Y + 10, MOTE_RGB565(235, 230, 215));
    if (s_msg[1][0]) rl_text(fb, s_msg[1], 2, HUD_Y + 17, MOTE_RGB565(128, 124, 152));
}
