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
#include "floor_road.tiles.h"

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
#include "armour_set.h"
#include "trinkets.h"
#include "props_light.h"
#include "doors_gems_banners.h"
#include "chests.h"
#include "boulders_mountains.h"
#include "fx_mono.h"
#include "dungeon_mono.h"
#include "stairs.h"
#include "jewellery.h"
#include "ammo.h"
#include "devices.h"
#include "chest_wood.h"
#include "levers.h"
#include "rogue8.font.h"

/* Order must match the SH_* enum in rl.h. */
static const MoteImage *const s_sheet[SH_COUNT] = {
    &animals_img, &monsters_img, &bosses_img, &characters_img,
    &weapons_potions_img, &tools_wands_img, &weapons_elemental_img,
    &treasure_ore_img, &crowns_fx_img, &food_img, &runes_img,
    &armour_set_img, &trinkets_img, &props_light_img,
    &doors_gems_banners_img, &chests_img, &boulders_mountains_img,
    &fx_mono_img, &dungeon_mono_img, &stairs_img, &jewellery_img,
    &ammo_img, &devices_img, &chest_wood_img, &levers_img,
};

const MoteImage *rl_sheet(int id) {
    return (id >= 0 && id < SH_COUNT) ? s_sheet[id] : s_sheet[0];
}

/* Feature sprites, named so the intent survives a re-bake.
 *
 * Stairs are the four two-tile staircases on source row 0, cols 2-9 -- stone,
 * gold, blue and pink, each drawn as a flight of risers with the treads left
 * black so the floor shows through them. They replace the monochrome band's
 * framed glyphs, which were a frame with a couple of lines in it. Stone descends
 * and stone ascends read as the same material at different values; the gold
 * flight is loud enough to find on grass, so it marks the mine mouth. */
#define SPR_STAIR_DOWN    0     /* stairs (2,0): stone, dark risers */
#define SPR_STAIR_UP      1     /* stairs (3,0): stone, pale treads */
/* The grey/white door, source (11,5) and (12,5). The doors sheet is five
 * colours and this was on the first row, which is hot pink -- against navy brick
 * and a near-black floor that is the loudest thing on the screen and it is a
 * door. Grey timber-and-stone reads at 8px against every wall band. */
#define SPR_DOOR_CLOSED  20
#define SPR_DOOR_OPEN    21
/* The town marker and the shopfronts come from labels_human.json, not from a
 * guess: (1,9) is the mushroom-roofed cottage, and (13,1)..(13,5) are the five
 * coloured house/shop fronts the annotator pass identified. props_light 32 --
 * what the town used before -- is the igloo. */
#define SPR_HOUSE        31     /* props_light (1,9): cottage */
/* The inn is a bed, trinkets (0,9). props_light has no tavern, and of the two
 * honest options -- a second cottage nobody can tell from a house, or the thing
 * you actually go in for -- the bed is the one that reads at 8px. */
#define SPR_INN          16     /* trinkets (0,9): bed */
#define SPR_RUBBLE       69     /* trinkets (5,12): the loose grey stone */
/* The mine mouth is the gold flight (4,0). Stone stairs vanish into a green
 * field; gold does not, and a landmark you have to hunt for on a 16x13 viewport
 * is a landmark that does not work. */
#define SPR_CAVE_MOUTH    2     /* stairs (4,0): gold */
/* A tower is the other way underground. props_light (2,9) -- battlements over a
 * pale stone shaft with an archway cut in it -- reads as a doorway at 8px in a
 * way a hole in the ground does not, and it is tall enough to be a landmark
 * across a 128x96 continent. */
#define SPR_TOWER        32     /* props_light (2,9): stone tower */

/* Wall identity per depth band -- descending should LOOK like descending.
 * See DESIGN.md section 3. */
/* Four filled blob47 sets across the forty floors. The fifth band used to be
 * wall_plaster, which is EDGE16 line art -- sixteen cells of bars and junctions
 * for drawing a floor plan, handed to a consumer indexing forty-seven. Below
 * floor 33 the walls came out as unrelated fragments. A mass wall needs a mass
 * tileset; plaster stays in the atlas for line work. */
static const MoteAutotile *wall_for_depth(int d) {
    if (d <= 10) return &wall_brick_at;
    if (d <= 20) return &wall_bone_at;
    if (d <= 30) return &wall_marble_at;
    return &wall_aztec_at;
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

/* Where the camera ended up last frame, so the overlay pass can place the
 * shadow under the player without recomputing the clamp and drifting from it. */
static int s_cam_x, s_cam_y;

/* --- the player's shadow -------------------------------------------------
 *
 * The character is one 8x8 cell drawn from the same sixteen colours as the
 * floor it stands on, inside a viewport that is nothing but texture, and at
 * 128x128 it simply vanishes -- especially on the busy deep wall bands.
 *
 * A pool of shadow under it, with the sprite re-drawn on top at full strength,
 * gives the one thing the scene itself cannot: local contrast that travels with
 * the player. It stays inside the player's own tile and a pixel or two beyond,
 * so it darkens the ground the character stands on rather than dimming whatever
 * is standing next to them.
 *
 * The blend is done by hand. scene2d sprites are colour-keyed with no alpha and
 * draw_rect/draw_circle are solid fills, so there is no engine path that puts
 * translucent black over what has already been rastered. */
#define SHADOW_R     6          /* radius in pixels; the tile is 8 across */
#define SHADOW_DARK 112         /* brightness at the centre, out of 256 */

static uint16_t dim565(uint16_t c, int num) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = r * num >> 8; g = g * num >> 8; b = b * num >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void rl_draw_player_shadow(uint16_t *fb) {
    int px = g_pl.x * TS - s_cam_x;         /* top-left of the player's cell */
    int py = g_pl.y * TS - s_cam_y;
    int ox = px + TS / 2, oy = py + TS / 2; /* and its centre */

    for (int y = oy - SHADOW_R; y <= oy + SHADOW_R; y++) {
        if (y < 0 || y >= HUD_Y) continue;  /* never bleed into the HUD */
        for (int x = ox - SHADOW_R; x <= ox + SHADOW_R; x++) {
            if (x < 0 || x >= MOTE_FB_W) continue;
            int dx = x - ox, dy = y - oy;
            int d2 = dx * dx + dy * dy;
            if (d2 > SHADOW_R * SHADOW_R) continue;
            /* darkest at the centre, easing back to untouched at the rim, so
             * the pool has no visible edge */
            int t = d2 * 256 / (SHADOW_R * SHADOW_R);
            int num = SHADOW_DARK + (256 - SHADOW_DARK) * t / 256;
            uint16_t *p = &fb[y * MOTE_FB_W + x];
            *p = dim565(*p, num);
        }
    }

    /* and the character back over the top, undimmed */
    rl_blit_cell(fb, SH_CHARACTERS, g_class[g_pl.cls].cell, px, py);
}

/* Blit a sheet cell TINTED. The fx_mono strips are drawn in greyscale on
 * purpose -- that is the whole reason they are grey -- so a frost bolt and a
 * fireball can be the same six frames in two colours instead of two sets of
 * art. Multiplying the source by the tint is exactly right for a grey source:
 * the grey level survives as brightness and the hue comes entirely from `col`.
 *
 * Hand-written because there is no engine path for it: blit() is colour-keyed
 * and copies pixels through unchanged. */
void rl_blit_cell_tint(uint16_t *fb, int sheet, int cell, int x, int y,
                       uint16_t col) {
    const MoteImage *img = rl_sheet(sheet);
    int cols = img->w / TS;
    if (cols < 1) cols = 1;
    int sx = (cell % cols) * TS, sy = (cell / cols) * TS;
    int tr = (col >> 11) & 31, tg = (col >> 5) & 63, tb = col & 31;

    for (int j = 0; j < TS; j++) {
        int py = y + j;
        if (py < 0 || py >= MOTE_FB_H) continue;
        for (int i = 0; i < TS; i++) {
            int px = x + i;
            if (px < 0 || px >= MOTE_FB_W) continue;
            /* the bake may hand back RGB565 or 4-/8-bit indexed, so sample
             * through the format rather than assuming pixels[] */
            int si = (sy + j) * img->w + sx + i;
            uint16_t c;
            if (img->format == 1)
                c = img->palette[(img->indices[si >> 1] >> ((si & 1) ? 0 : 4)) & 15];
            else if (img->format == 2)
                c = img->palette[img->indices[si]];
            else
                c = img->pixels[si];
            if (!img->opaque && c == img->key) continue;
            int r = ((c >> 11) & 31) * tr / 31;
            int g = ((c >> 5) & 63) * tg / 63;
            int b = (c & 31) * tb / 31;
            fb[py * MOTE_FB_W + px] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
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

/* Assemble a combat line: "<who> <verb> <whom>'s <part> (12)". Every piece is
 * optional, so the same builder serves "You graze the rat's leg (2)." and
 * "Your arrow punches the orc (14)!".
 *
 * Assembled rather than printf'd because there is no printf here, and because
 * a 31-character line has to be TRUNCATED at a sensible place -- the damage is
 * the part you must not lose, so it is written before the sentence is allowed
 * to run out of room. */
void rl_msg_hit(const char *who, const char *verb, const char *whom,
                const char *part, int dam, int bang) {
    char buf[MSG_LEN];
    int o = 0;
    #define ADD(str) do { const char *p_ = (str); \
        while (p_ && *p_ && o < MSG_LEN - 8) buf[o++] = *p_++; } while (0)
    ADD(who);
    ADD(" "); ADD(verb);
    if (whom) { ADD(" "); ADD(whom); }
    /* "the orc's leg", but "your leg" -- a possessive noun takes the apostrophe
     * and a possessive pronoun does not, and "your's" is what you get if the
     * builder decides that for itself */
    if (part) {
        if (whom && whom[0] != 'y') ADD("'s");
        ADD(" "); ADD(part);
    }
    if (dam >= 0) {
        ADD(" ");
        int v = dam, digits[6], n = 0;
        do { digits[n++] = v % 10; v /= 10; } while (v && n < 6);
        if (o < MSG_LEN - 3) buf[o++] = '(';
        while (n-- > 0 && o < MSG_LEN - 2) buf[o++] = (char)('0' + digits[n]);
        if (o < MSG_LEN - 2) buf[o++] = ')';
    }
    if (o < MSG_LEN - 1) buf[o++] = bang ? '!' : '.';
    buf[o] = 0;
    #undef ADD
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
        /* bit3 the town's brick wall, bit4 its cobbled streets. The streets run
         * under the buildings too, so a shopfront stands ON the road rather than
         * on a patch of grass the road happens to pass. */
        if (t == T_TOWN_WALL) v |= 8;
        if (t == T_ROAD || t == T_SHOP || t == T_INN || t == T_TOWN) v |= 16;
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
 * scattered on it, and it should read as grassland: trinkets 66 and 67 -- the
 * blades and the short tufts sitting just left of the trees on source row 12 --
 * carry thirteen of sixteen draws. A flower is an event, not a ground cover, so
 * two entries between them; one stone for the rest. Density is one tile in four,
 * chosen by position so the field is stable frame to frame. */
static int decor_cell(int x, int y) {
    static const uint8_t k[16] = { 66, 67, 66, 67, 66, 67, 66, 67,
                                   66, 67, 66, 67, 66, 48, 55, 69 };
    unsigned h = pos_hash(x, y);
    if ((h & 3) != 0) return -1;              /* ~25% of open ground */
    return k[(h >> 9) & 15];
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
    /* clamp to the LEVEL, not the buffer -- a dungeon occupies the top-left
     * 64x48 of it, and scrolling out into the unused rock is not a view */
    int lw = rl_level_w() * TS, lh = rl_level_h() * TS;
    if (cx > lw - VIEW_W * TS) cx = lw - VIEW_W * TS;
    if (cy > lh - VIEW_H * TS) cy = lh - VIEW_H * TS;
    s_cam_x = cx; s_cam_y = cy;

    static const MoteAutotile *tiles[5];
    int n_layer;
    if (g_pl.depth == 0) {
        build_layers_overworld();
        tiles[0] = &floor_grass_at;
        tiles[1] = &floor_jungle_at;
        tiles[2] = &wall_aztec_at;
        tiles[3] = &wall_brick_at;      /* town wall */
        tiles[4] = &floor_road_at;      /* streets: warm brown paving, not the
                                           near-black dungeon floor, which read
                                           as a hole cut in the grass */
        n_layer = 5;
    } else {
        build_layers_dungeon();
        tiles[0] = floor_for_depth(g_pl.depth);
        tiles[1] = wall_for_depth(g_pl.depth);
        n_layer = 2;
    }

    g_api->scene2d_begin(cx, cy);
    g_api->scene2d_set_autotile_layers(g_lv.layer, MW, MH, tiles, n_layer);

    /* Only the visible window is worth walking: over the full map the sweep is
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
            case T_STAIR_DOWN:   sheet = SH_STAIRS;  cell = SPR_STAIR_DOWN; break;
            case T_STAIR_UP:     sheet = SH_STAIRS;  cell = SPR_STAIR_UP;   break;
            case T_DOOR_CLOSED:  cell = SPR_DOOR_CLOSED; break;
            case T_DOOR_OPEN:    cell = SPR_DOOR_OPEN;   break;
            case T_TREE:         sheet = SH_TRINKETS; cell = tree_cell(x, y); break;
            case T_TOWN:         sheet = SH_PROPS;    cell = SPR_HOUSE;      break;
            case T_INN:          sheet = SH_TRINKETS; cell = SPR_INN;        break;
            case T_SHOP: {
                int s = rl_shop_at(x, y);
                if (s < 0) s = 0;
                sheet = g_shop_sheet[s]; cell = g_shop_cell[s];
                break;
            }
            case T_DUNGEON_MOUTH:sheet = SH_STAIRS;   cell = SPR_CAVE_MOUTH; break;
            case T_TOWER:       sheet = SH_PROPS;    cell = SPR_TOWER;      break;
            /* Rubble gets its own stone on top of whatever it is standing on:
             * underground the wall layer is drawn beneath it, on the surface it
             * is a fallen block on grass. Either way it must not look like plain
             * wall -- you can clear rubble, and the player has to be able to see
             * which blockage is worth a turn. */
            case T_RUBBLE:       sheet = SH_TRINKETS; cell = SPR_RUBBLE;        break;
            /* One chest, drawn closed or open. The five-colour set this used
             * to draw from is a set of BOXES; (16,5)/(17,5) is the one that
             * reads as treasure. The tier still varies -- it decides the loot
             * and the trap odds -- it just no longer changes the paint. */
            case T_CHEST:        sheet = SH_CHESTWOOD; cell = 0; break;
            case T_CHEST_OPEN:   sheet = SH_CHESTWOOD; cell = 1; break;
            /* levers: three positions across, five colours down; a thrown lever
             * shows its handle over, an unthrown one shows it back */
            case T_LEVER_R: case T_LEVER_B: case T_LEVER_D:
            case T_LEVER_G: case T_LEVER_W:
                sheet = SH_LEVERS;
                cell = T_LEVER_HUE(g_lv.terrain[i]) * 3
                     + ((g_lv.flags[i] & CF_ROOM) ? 2 : 0);
                break;
            case T_LOCK_R: case T_LOCK_B: case T_LOCK_D:
            case T_LOCK_G: case T_LOCK_W:
                sheet = SH_DOORS; cell = SPR_DOOR_CLOSED; break;
            /* A locked chest wears its colour, which is the whole clue. The
             * five-colour box set the wooden chest replaced is exactly this,
             * row for row against the lever band. */
            case T_CBOX_R: case T_CBOX_B: case T_CBOX_D:
            case T_CBOX_G: case T_CBOX_W:
                sheet = SH_CHESTS;
                cell = T_CBOX_HUE(g_lv.terrain[i]) * 2; break;
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
            int sheet = mk->sheet, cell = mk->cell;
            if (mk->flags & MK_MIMIC) {
                if (m->flags & MF_ASLEEP) {
                    /* still pretending: a real chest, in the colour this tile
                     * would have had if a chest had actually been put here */
                    sheet = SH_CHESTS;
                    cell = rl_chest_cell(m->x, m->y, 0);
                } else {
                    /* mk->cell is the shut frame; the next four are the lid
                     * coming up on eyes, tongue, teeth and the full snarl */
                    cell = mk->cell + 1 + (int)((g_turn >> 1) & 3);
                }
            }
            add_cell(sheet, cell, m->x * TS, m->y * TS, 20);
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

    /* the surface is two places, not one: inside the walls and everywhere else */
    if (g_pl.depth == 0)
        rl_text(fb, rl_in_town() ? "TOWN" : "WILD", 104, HUD_Y + 2,
                rl_in_town() ? MOTE_RGB565(150, 220, 160) : MOTE_RGB565(190, 180, 120));
    else {
        rl_text(fb, "DL", 104, HUD_Y + 2, MOTE_RGB565(150, 146, 170));
        rl_num(fb, g_pl.depth, 113, HUD_Y + 2, MOTE_RGB565(220, 214, 200));
    }

    /* starvation, haste and a blessing are the states worth a glyph. Haste is
     * read from rl_player_speed, not from the timer -- a ring of speed sets no
     * timer, so keying the glyph off g_pl.haste hid the one permanent source. */
    if (g_pl.food < 200)                        rl_text(fb, "*", 124, HUD_Y + 2, MOTE_RGB565(230, 120, 60));
    else if (rl_player_speed() > SPEED_NORMAL)  rl_text(fb, ">", 124, HUD_Y + 2, MOTE_RGB565(250, 220, 90));
    else if (g_pl.bless > 0)                    rl_text(fb, "+", 124, HUD_Y + 2, MOTE_RGB565(140, 200, 255));

    rl_draw_msgs(fb);
}

/* --- the map screen -----------------------------------------------------
 * The whole level at one pixel a cell -- 128x96, which is the framebuffer's
 * width exactly. On the surface this is the only way to find a cave mouth
 * without walking the coastline; underground it is the classic full-level map,
 * showing only what you have seen. */
void rl_draw_map(uint16_t *fb, int y0) {
    /* Only the part of the buffer this level actually uses -- and at whatever
     * scale fills the panel: the continent is 128 wide and takes one pixel a
     * cell, a dungeon floor is 64 and takes two. */
    int lw = rl_level_w(), lh = rl_level_h();
    int px = MOTE_FB_W / lw;
    if (px < 1) px = 1;
    for (int y = 0; y < lh; y++) {
        for (int x = 0; x < lw; x++) {
            int i = y * MW + x;
            uint16_t c;
            if (g_pl.depth && !(g_lv.flags[i] & CF_KNOWN)) continue;
            switch (g_lv.terrain[i]) {
            case T_TREE:          c = MOTE_RGB565(0, 105, 60);   break;
            case T_HILL:          c = MOTE_RGB565(110, 205, 105);break;
            case T_MOUNTAIN:      c = MOTE_RGB565(150, 110, 60); break;
            case T_TOWN:
            case T_INN:
            case T_SHOP:          c = MOTE_RGB565(255, 210, 90); break;
            case T_TOWN_WALL:     c = MOTE_RGB565(190, 190, 205); break;
            case T_ROAD:          c = MOTE_RGB565(140, 120, 100); break;
            case T_CHEST:         c = MOTE_RGB565(255, 160, 40); break;
            case T_LEVER_R: case T_LEVER_B: case T_LEVER_D:
            case T_LEVER_G: case T_LEVER_W:  c = MOTE_RGB565(255, 120, 200); break;
            case T_LOCK_R: case T_LOCK_B: case T_LOCK_D:
            case T_LOCK_G: case T_LOCK_W:    c = MOTE_RGB565(200, 60, 120); break;
            case T_DUNGEON_MOUTH: c = MOTE_RGB565(240, 90, 60);  break;
            /* cyan, and nothing else on the surface is: at one pixel a cell a
             * tower has to be findable by colour alone */
            case T_TOWER:         c = MOTE_RGB565(120, 230, 255); break;
            case T_WALL:
            case T_RUBBLE:        c = MOTE_RGB565(64, 60, 84);   break;
            case T_STAIR_DOWN:    c = MOTE_RGB565(240, 90, 60);  break;
            case T_STAIR_UP:      c = MOTE_RGB565(90, 200, 240); break;
            case T_DOOR_CLOSED:
            case T_DOOR_OPEN:     c = MOTE_RGB565(180, 130, 60); break;
            default: c = g_pl.depth ? MOTE_RGB565(150, 146, 160)
                                    : MOTE_RGB565(0, 135, 81);   break;
            }
            if (px == 1) g_api->draw_pixel(fb, x, y0 + y, c);
            else g_api->draw_rect(fb, x * px, y0 + y * px, px, px, c, 1, 0, MOTE_FB_H);
        }
    }
    /* the player last, so nothing paints over it -- and as a cross rather than
     * a dot, because one white pixel in a 128x96 field is not findable */
    int mx = g_pl.x * px, my = y0 + g_pl.y * px;
    uint16_t w = MOTE_RGB565(255, 255, 255);
    for (int d = -2; d <= 2; d++) {
        if (mx + d >= 0 && mx + d < MOTE_FB_W) g_api->draw_pixel(fb, mx + d, my, w);
        if (my + d >= y0 && my + d < MOTE_FB_H) g_api->draw_pixel(fb, mx, my + d, w);
    }
}

void rl_draw_msgs(uint16_t *fb) {
    if (s_msg[0][0]) rl_text(fb, s_msg[0], 2, HUD_Y + 10, MOTE_RGB565(235, 230, 215));
    if (s_msg[1][0]) rl_text(fb, s_msg[1], 2, HUD_Y + 17, MOTE_RGB565(128, 124, 152));
}
