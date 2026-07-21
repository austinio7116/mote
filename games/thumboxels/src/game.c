// Falling-sand sandbox for the Mote engine (Thumby Color).
// Verified API: mote->rumble, mote->save/load, mote->kv_save/kv_load,
// mote->text_font(fb, mote->ui_font(...), s,x,y,col), MOTE_BTN_LB/RB.
//
// SIMPLIFICATIONS (read before reporting a "bug" on these):
//  - Bees/Ants have no pathfinding - Bees nudge nearby Plant growth, Ants
//    nudge nearby Seeds toward another empty spot. Not real foraging AI.
//  - Black Hole is a radial pull+consume approximation, not real gravity.
//  - Portal remembers only the MOST RECENTLY PLACED Portal1/Portal2 cell as
//    the active pair - placing a second Portal1 moves the entry point.
//  - Heat display is a fixed HOT/WARM/COLD label per element, not a real
//    diffusing temperature field.

#include "mote_api.h"
#include "mote_build.h"
#include <stdio.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define GW 128
#define GH 128
#define LIQUID_SPREAD 5

// ---------------------------------------------------------------------
// Elements
// ---------------------------------------------------------------------
enum {
    EMPTY = 0,
    STONE, SAND, DIRT, GRAVEL, CLAY, SNOW, ICE, MUD,                 // land 8
    WATER, OIL, LAVA, ACID, HONEY, TAR, MERCURY, SLIME,              // liquid 8
    PLANT, SEED, MOLD, VINE,                                          // life 4
    WOOD, GLASS, METAL, BRICK, PLASTIC, CONCRETE, CRYSTAL,           // solid 7
    FIRE, SPARK,                                                     // energy 2
    GUNPOWDER, ASH, SALT, CHARCOAL, SULFUR, CEMENT,                  // powder 6
    SMOKE, STEAM, METHANE, TOXIC, FOG,                               // gas 5
    BOMB, FIREWORK, BLACKHOLE, PORTAL1, PORTAL2,                     // weapons 5
    N_ELEMENTS
};

enum { CAT_EMPTY, CAT_STATIC, CAT_POWDER, CAT_LIQUID, CAT_GAS, CAT_FIRE };

static const uint8_t elem_category[N_ELEMENTS] = {
    [EMPTY] = CAT_EMPTY,
    [STONE] = CAT_STATIC, [SAND] = CAT_POWDER, [DIRT] = CAT_POWDER, [GRAVEL] = CAT_POWDER,
    [CLAY] = CAT_POWDER, [SNOW] = CAT_POWDER, [ICE] = CAT_STATIC, [MUD] = CAT_STATIC,
    [WATER] = CAT_LIQUID, [OIL] = CAT_LIQUID, [LAVA] = CAT_LIQUID, [ACID] = CAT_LIQUID,
    [HONEY] = CAT_LIQUID, [TAR] = CAT_LIQUID, [MERCURY] = CAT_LIQUID, [SLIME] = CAT_LIQUID,
    [PLANT] = CAT_STATIC, [SEED] = CAT_POWDER, [MOLD] = CAT_STATIC, [VINE] = CAT_STATIC,
    [WOOD] = CAT_STATIC, [GLASS] = CAT_STATIC, [METAL] = CAT_STATIC, [BRICK] = CAT_STATIC,
    [PLASTIC] = CAT_STATIC, [CONCRETE] = CAT_STATIC, [CRYSTAL] = CAT_STATIC,
    [FIRE] = CAT_FIRE, [SPARK] = CAT_GAS,
    [GUNPOWDER] = CAT_POWDER, [ASH] = CAT_POWDER, [SALT] = CAT_POWDER, [CHARCOAL] = CAT_POWDER,
    [SULFUR] = CAT_POWDER, [CEMENT] = CAT_POWDER,
    [SMOKE] = CAT_GAS, [STEAM] = CAT_GAS, [METHANE] = CAT_GAS, [TOXIC] = CAT_GAS, [FOG] = CAT_GAS,
    [BOMB] = CAT_POWDER, [FIREWORK] = CAT_POWDER, [BLACKHOLE] = CAT_STATIC,
    [PORTAL1] = CAT_STATIC, [PORTAL2] = CAT_STATIC,
};

static const int8_t liquid_density[N_ELEMENTS] = {
    [WATER] = 5, [OIL] = 3, [LAVA] = 9, [ACID] = 6,
    [HONEY] = 7, [TAR] = 8, [MERCURY] = 10, [SLIME] = 4,
};

static const uint8_t flammable[N_ELEMENTS] = {
    [WOOD] = 1, [PLANT] = 1, [OIL] = 1, [TAR] = 1,
    [MOLD] = 1, [VINE] = 1, [PLASTIC] = 1, [CHARCOAL] = 1, [SULFUR] = 1,
};

static const uint8_t explosive[N_ELEMENTS] = {
    [GUNPOWDER] = 1, [METHANE] = 1,
};

static const uint8_t acid_dissolvable[N_ELEMENTS] = {
    [SAND] = 1, [DIRT] = 1, [WOOD] = 1, [PLANT] = 1, [MUD] = 1,
    [SEED] = 1, [VINE] = 1, [MOLD] = 1,
};

static inline int spread_limit_for(uint8_t c) { return (c == HONEY || c == TAR) ? 1 : LIQUID_SPREAD; }

static const char *elem_name[N_ELEMENTS] = {
    [EMPTY] = "EMPTY",
    [STONE] = "STONE", [SAND] = "SAND", [DIRT] = "DIRT", [GRAVEL] = "GRAVEL",
    [CLAY] = "CLAY", [SNOW] = "SNOW", [ICE] = "ICE", [MUD] = "MUD",
    [WATER] = "WATER", [OIL] = "OIL", [LAVA] = "LAVA", [ACID] = "ACID",
    [HONEY] = "HONEY", [TAR] = "TAR", [MERCURY] = "MERCURY", [SLIME] = "SLIME",
    [PLANT] = "PLANT", [SEED] = "SEED", [MOLD] = "MOLD", [VINE] = "VINE",
    [WOOD] = "WOOD", [GLASS] = "GLASS", [METAL] = "METAL", [BRICK] = "BRICK",
    [PLASTIC] = "PLASTIC", [CONCRETE] = "CONCRETE", [CRYSTAL] = "CRYSTAL",
    [FIRE] = "FIRE", [SPARK] = "SPARK",
    [GUNPOWDER] = "GUNPOWDER", [ASH] = "ASH", [SALT] = "SALT", [CHARCOAL] = "CHARCOAL",
    [SULFUR] = "SULFUR", [CEMENT] = "CEMENT",
    [SMOKE] = "SMOKE", [STEAM] = "STEAM", [METHANE] = "METHANE", [TOXIC] = "TOXIC", [FOG] = "FOG",
    [BOMB] = "BOMB", [FIREWORK] = "FIREWORK", [BLACKHOLE] = "BLACK HOLE",
    [PORTAL1] = "PORTAL A", [PORTAL2] = "PORTAL B",
};

static const uint16_t elem_color[N_ELEMENTS] = {
    [EMPTY] = 0x0000,
    [STONE] = MOTE_RGB565(120, 120, 120), [SAND] = MOTE_RGB565(230, 200, 120),
    [DIRT] = MOTE_RGB565(100, 65, 35), [GRAVEL] = MOTE_RGB565(140, 140, 140),
    [CLAY] = MOTE_RGB565(180, 120, 90), [SNOW] = MOTE_RGB565(240, 240, 250),
    [ICE] = MOTE_RGB565(170, 220, 240), [MUD] = MOTE_RGB565(90, 70, 40),
    [WATER] = MOTE_RGB565(60, 110, 230), [OIL] = MOTE_RGB565(110, 90, 20),
    [LAVA] = MOTE_RGB565(220, 40, 10), [ACID] = MOTE_RGB565(140, 220, 40),
    [HONEY] = MOTE_RGB565(235, 180, 40), [TAR] = MOTE_RGB565(25, 20, 15),
    [MERCURY] = MOTE_RGB565(210, 210, 220), [SLIME] = MOTE_RGB565(110, 220, 90),
    [PLANT] = MOTE_RGB565(40, 160, 60), [SEED] = MOTE_RGB565(150, 120, 60),
    [MOLD] = MOTE_RGB565(90, 130, 60), [VINE] = MOTE_RGB565(50, 140, 50),
    [WOOD] = MOTE_RGB565(120, 80, 40), [GLASS] = MOTE_RGB565(180, 220, 230),
    [METAL] = MOTE_RGB565(180, 185, 195), [BRICK] = MOTE_RGB565(150, 60, 40),
    [PLASTIC] = MOTE_RGB565(220, 120, 180), [CONCRETE] = MOTE_RGB565(160, 160, 150),
    [CRYSTAL] = MOTE_RGB565(180, 140, 255),
    [FIRE] = MOTE_RGB565(250, 120, 30), [SPARK] = MOTE_RGB565(180, 220, 255),
    [GUNPOWDER] = MOTE_RGB565(60, 60, 60), [ASH] = MOTE_RGB565(100, 100, 90),
    [SALT] = MOTE_RGB565(245, 245, 240), [CHARCOAL] = MOTE_RGB565(35, 35, 35),
    [SULFUR] = MOTE_RGB565(220, 210, 60), [CEMENT] = MOTE_RGB565(160, 155, 140),
    [SMOKE] = MOTE_RGB565(90, 90, 90), [STEAM] = MOTE_RGB565(200, 200, 210),
    [METHANE] = MOTE_RGB565(140, 180, 120), [TOXIC] = MOTE_RGB565(90, 180, 60),
    [FOG] = MOTE_RGB565(210, 210, 215),
    [BOMB] = MOTE_RGB565(50, 50, 55), [FIREWORK] = MOTE_RGB565(255, 80, 150),
    [BLACKHOLE] = MOTE_RGB565(20, 5, 30), [PORTAL1] = MOTE_RGB565(60, 160, 255),
    [PORTAL2] = MOTE_RGB565(255, 140, 40),
};

// ---------------------------------------------------------------------
// UI sentinels: actions (settings/save) and tools live in separate ranges,
// well clear of the element id space (0-47), so nothing can collide.
// ---------------------------------------------------------------------
#define ACTION_BASE 100
#define ACTION_SAVE1              101
#define ACTION_SAVE2              102
#define ACTION_SAVE3              103
#define ACTION_SAVE4              104
#define ACTION_SENSITIVITY        110
#define ACTION_PIXEL_MODE         111
#define ACTION_AUTOSAVE           112
#define ACTION_SHOW_LABEL         113
#define ACTION_PAUSE_SIM          114
#define ACTION_CIRCLE_BRUSH       115
#define ACTION_MIRROR             116
#define ACTION_HOVER_DISPLAY      117
#define ACTION_MASTER_VOLUME      119
#define ACTION_MASTER_RUMBLE      120
#define ACTION_AUTOSAVE_INTERVAL  121

#define TOOL_BASE       150
#define TOOL_PLACE      150
#define TOOL_LINE       151
#define TOOL_EYEDROPPER 152
#define TOOL_DRAG       153
#define TOOL_MIX        154
#define TOOL_SMASH      155

static int sensitivity = 5;
static int autosave_enabled = 0;
static float autosave_timer = 0.0f;
static float autosave_notify_timer = 0.0f; // >0 while the "autosaving" indicator should show
static float autosave_interval = 60.0f;
static int show_label = 1;
static int pixel_mode = 0;
static int sim_paused = 0;
static int circle_brush = 0;
static int mirror_mode = 0;
static int hover_display = 0;
static int sound_enabled = 1;
static int rumble_enabled = 1;
static int current_tool = TOOL_PLACE;

static inline int is_action(uint8_t id) { return id >= ACTION_BASE && id < TOOL_BASE; }
static inline int is_tool(uint8_t id) { return id >= TOOL_BASE; }

static const char *action_name(uint8_t id) {
    switch (id) {
        case ACTION_SAVE1: return "SAVE 1";
        case ACTION_SAVE2: return "SAVE 2";
        case ACTION_SAVE3: return "SAVE 3";
        case ACTION_SAVE4: return "SAVE 4";
        case ACTION_SENSITIVITY: return "SENSITIVITY";
        case ACTION_PIXEL_MODE: return "PIXEL MOVE";
        case ACTION_AUTOSAVE: return "AUTOSAVE";
        case ACTION_SHOW_LABEL: return "LABEL";
        case ACTION_PAUSE_SIM: return "PAUSE SIM";
        case ACTION_CIRCLE_BRUSH: return "CIRCLE BRUSH";
        case ACTION_MIRROR: return "MIRROR";
        case ACTION_HOVER_DISPLAY: return "HOVER NAME";
        case ACTION_MASTER_VOLUME: return "SOUND";
        case ACTION_MASTER_RUMBLE: return "RUMBLE";
        case ACTION_AUTOSAVE_INTERVAL: return "AUTOSAVE SEC";
    }
    return "?";
}
static uint16_t action_color(uint8_t id) {
    if (id >= ACTION_SAVE4 && id <= ACTION_SAVE1) return MOTE_RGB565(60, 150, 90);
    if (id == ACTION_SENSITIVITY) return MOTE_RGB565(80, 150, 220);
    if (id == ACTION_AUTOSAVE_INTERVAL) return MOTE_RGB565(80, 150, 220);
    int on =
        (id == ACTION_AUTOSAVE && autosave_enabled) ||
        (id == ACTION_SHOW_LABEL && show_label) ||
        (id == ACTION_PIXEL_MODE && pixel_mode) ||
        (id == ACTION_PAUSE_SIM && sim_paused) ||
        (id == ACTION_CIRCLE_BRUSH && circle_brush) ||
        (id == ACTION_MIRROR && mirror_mode) ||
        (id == ACTION_HOVER_DISPLAY && hover_display) ||
        (id == ACTION_MASTER_VOLUME && sound_enabled) ||
        (id == ACTION_MASTER_RUMBLE && rumble_enabled);
    return on ? MOTE_RGB565(60, 200, 90) : MOTE_RGB565(90, 90, 90);
}
static const char *tool_name(uint8_t id) {
    switch (id) {
        case TOOL_PLACE: return "PLACE";
        case TOOL_LINE: return "LINE";
        case TOOL_EYEDROPPER: return "EYEDROP";
        case TOOL_DRAG: return "DRAG";
        case TOOL_MIX: return "MIX";
        case TOOL_SMASH: return "SMASH";
    }
    return "?";
}
static uint16_t tool_color(uint8_t id) {
    switch (id) {
        case TOOL_PLACE: return MOTE_RGB565(200, 200, 200);
        case TOOL_LINE: return MOTE_RGB565(120, 180, 255);
        case TOOL_EYEDROPPER: return MOTE_RGB565(255, 180, 60);
        case TOOL_DRAG: return MOTE_RGB565(180, 120, 255);
        case TOOL_MIX: return MOTE_RGB565(255, 120, 180);
        case TOOL_SMASH: return MOTE_RGB565(255, 90, 60);
    }
    return MOTE_RGB565(150, 150, 150);
}
static const char *display_name(uint8_t id) { return is_action(id) ? action_name(id) : elem_name[id]; }
static uint16_t display_color(uint8_t id) {
    if (is_action(id)) return action_color(id);
    return (id == EMPTY) ? MOTE_RGB565(55, 55, 55) : elem_color[id];
}
static char label_buf[32];
static const char *format_display_name(uint8_t id) {
    if (id == ACTION_SENSITIVITY) { snprintf(label_buf, sizeof label_buf, "SENSITIVITY %d", sensitivity); return label_buf; }
    if (id == ACTION_AUTOSAVE_INTERVAL) { snprintf(label_buf, sizeof label_buf, "AUTOSAVE %ds", (int)autosave_interval); return label_buf; }
    if (id == ACTION_AUTOSAVE) { snprintf(label_buf, sizeof label_buf, "AUTOSAVE %s", autosave_enabled ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_SHOW_LABEL) { snprintf(label_buf, sizeof label_buf, "LABEL %s", show_label ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_PIXEL_MODE) { snprintf(label_buf, sizeof label_buf, "PIXEL MOVE %s", pixel_mode ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_PAUSE_SIM) { snprintf(label_buf, sizeof label_buf, "PAUSE SIM %s", sim_paused ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_CIRCLE_BRUSH) { snprintf(label_buf, sizeof label_buf, "CIRCLE BRUSH %s", circle_brush ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_MIRROR) { snprintf(label_buf, sizeof label_buf, "MIRROR %s", mirror_mode ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_HOVER_DISPLAY) { snprintf(label_buf, sizeof label_buf, "HOVER NAME %s", hover_display ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_MASTER_VOLUME) { snprintf(label_buf, sizeof label_buf, "SOUND %s", sound_enabled ? "ON" : "OFF"); return label_buf; }
    if (id == ACTION_MASTER_RUMBLE) { snprintf(label_buf, sizeof label_buf, "RUMBLE %s", rumble_enabled ? "ON" : "OFF"); return label_buf; }
    return display_name(id);
}

// ---------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------
static const uint8_t TAB_LAND[]    = { STONE, SAND, DIRT, GRAVEL, CLAY, SNOW, ICE, MUD };
static const uint8_t TAB_LIQUID[]  = { WATER, OIL, LAVA, ACID, HONEY, TAR, MERCURY, SLIME };
static const uint8_t TAB_LIFE[]    = { PLANT, SEED, MOLD, VINE };
static const uint8_t TAB_SOLID[]   = { WOOD, GLASS, METAL, BRICK, PLASTIC, CONCRETE, CRYSTAL };
static const uint8_t TAB_ENERGY[]  = { FIRE, SPARK };
static const uint8_t TAB_POWDER[]  = { GUNPOWDER, ASH, SALT, CHARCOAL, SULFUR, CEMENT };
static const uint8_t TAB_GAS[]     = { SMOKE, STEAM, METHANE, TOXIC, FOG };
static const uint8_t TAB_WEAPONS[] = { BOMB, FIREWORK, BLACKHOLE, PORTAL1, PORTAL2 };
static const uint8_t TAB_TOOLS[]   = { TOOL_PLACE, TOOL_LINE, TOOL_EYEDROPPER, TOOL_DRAG, TOOL_MIX, TOOL_SMASH };
static const uint8_t TAB_SPECIAL[] = { ACTION_SAVE1, ACTION_SAVE2, ACTION_SAVE3, ACTION_SAVE4 };
static const uint8_t TAB_SETTINGS[] = {
    ACTION_SENSITIVITY, ACTION_PIXEL_MODE, ACTION_AUTOSAVE, ACTION_AUTOSAVE_INTERVAL,
    ACTION_SHOW_LABEL, ACTION_PAUSE_SIM, ACTION_CIRCLE_BRUSH, ACTION_MIRROR,
    ACTION_HOVER_DISPLAY, ACTION_MASTER_VOLUME, ACTION_MASTER_RUMBLE,
};

#define N_TABS 11
static const uint8_t *tab_items[N_TABS] = {
    TAB_LAND, TAB_LIQUID, TAB_LIFE, TAB_SOLID, TAB_ENERGY, TAB_POWDER, TAB_GAS,
    TAB_WEAPONS, TAB_TOOLS, TAB_SPECIAL, TAB_SETTINGS,
};
static const int tab_count[N_TABS] = { 8, 8, 4, 7, 2, 6, 5, 5, 6, 4, 11 };
static const char *tab_abbrev[N_TABS - 1] = {
    "LAND", "LIQ", "LIFE", "SLD", "NRG", "PWD", "GAS", "WPN", "TLS", "SPC",
};
#define SETTINGS_TAB (N_TABS - 1)
#define SPECIAL_TAB 9
#define TOOLS_TAB 8

#define THUMB_N 16
static uint16_t thumb[4][THUMB_N * THUMB_N];
static int thumb_has_data[4] = {0, 0, 0, 0};

// ---------------------------------------------------------------------
// Grid state
// ---------------------------------------------------------------------
static uint8_t grid[GW * GH];
static uint8_t life[GW * GH];
static uint8_t tint[GW * GH];

static uint32_t rng_state = 0xC0FFEEu;
static inline uint32_t xrand(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static inline int chance(int percent) { return (int)(xrand() % 100) < percent; }

static inline void sfx(float freq, float vol) {
    if (!sound_enabled) return;
    vol *= 2.6f; if (vol > 1.0f) vol = 1.0f;
    mote->audio_note(freq, vol);
}
static inline void do_rumble(float intensity, int ms) {
    if (!rumble_enabled) return;
    mote->rumble(intensity, ms);
}

// ---------------------------------------------------------------------
// Cursor / tool state
// ---------------------------------------------------------------------
static float cursor_fx = 64.0f;
static float cursor_fy = 4.0f;
static int cursor_x = 64;
static int cursor_y = 4;

static int current_elem = SAND;
static int brush_radius = 0;
static float brush_hold_time = 0.0f;
static float brush_repeat_timer = 0.0f;
static unsigned frame_count = 0;
static float cursor_speed = 45.0f;

static int menu_open = 0;
static int cat_idx = 0;
static int item_idx = 0;

static float pixel_move_timer = 0.0f;
static float pixel_move_delay = 0.05f;
static float pixel_hold_time = 0.0f;
static int pixel_fast_repeat = 0;
#define PIXEL_FAST_THRESHOLD 0.5f
static int fast_repeat_active = 0;

static int hold_timer = 0;
static float place_throttle = 0.0f;

static int line_active = 0, line_start_x = 0, line_start_y = 0;
static int drag_prev_x = -1, drag_prev_y = -1;

static int portal1_x = -1, portal1_y = -1, portal2_x = -1, portal2_y = -1;

// ---------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------
static inline void put_px(uint16_t *fb, int x, int y, uint16_t col) {
    if (x < 0 || x >= GW || y < 0 || y >= GH) return;
    fb[y * GW + x] = col;
}
static void fill_rect(uint16_t *fb, int x0, int y0, int w, int h, uint16_t col) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_px(fb, x, y, col);
}
static void outline_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t col) {
    for (int x = x0; x <= x1; x++) { put_px(fb, x, y0, col); put_px(fb, x, y1, col); }
    for (int y = y0; y <= y1; y++) { put_px(fb, x0, y, col); put_px(fb, x1, y, col); }
}
static inline int text_w_est(const char *s) { int n = 0; for (; *s; s++) n++; return n * 7; }
static inline void draw_label(uint16_t *fb, int x, int y, const char *s, uint16_t col) {
    mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), s, x, y, col);
}
static inline uint32_t hash2(int x, int y, unsigned f) {
    uint32_t h = (uint32_t)(x * 374761393) ^ (uint32_t)(y * 668265263) ^ (uint32_t)(f * 2654435761u);
    h ^= h >> 15; h *= 0x2c1b3c6dU; h ^= h >> 12; h *= 0x297a2d39U; h ^= h >> 15;
    return h;
}
static inline uint16_t shade(uint16_t base, uint8_t t) {
    int d = (int)(t >> 6) - 2; // narrower range: -2..1 (was -8..7) - much subtler grain
    int r = (base >> 11) & 0x1F, g = (base >> 5) & 0x3F, b = base & 0x1F;
    r += d / 2; if (r < 0) r = 0; if (r > 31) r = 31;
    g += d;     if (g < 0) g = 0; if (g > 63) g = 63;
    b += d / 2; if (b < 0) b = 0; if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
// stronger grain used only for powders - gives the "individual visible grains"
// speckled look from the reference image, while liquids/solids/gas stay smooth
// via the subtler shade() above.
static inline uint16_t shade_grainy(uint16_t base, uint8_t t) {
    int d = (int)(t >> 4) - 8; // wide range: -8..7
    int r = (base >> 11) & 0x1F, g = (base >> 5) & 0x3F, b = base & 0x1F;
    r += d / 2; if (r < 0) r = 0; if (r > 31) r = 31;
    g += d;     if (g < 0) g = 0; if (g > 63) g = 63;
    b += d / 2; if (b < 0) b = 0; if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static void fill_squircle(uint16_t *fb, int x0, int y0, int size, uint16_t base, int textured) {
    int cut = size / 4; if (cut < 1) cut = 1;
    for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
            int cx = (dx < cut) ? (cut - dx - 1) : (dx >= size - cut ? dx - (size - cut) : -1);
            int cy = (dy < cut) ? (cut - dy - 1) : (dy >= size - cut ? dy - (size - cut) : -1);
            if (cx >= 0 && cy >= 0 && (cx + cy) >= cut) continue;
            uint16_t col = base;
            if (textured) { uint32_t h = hash2(x0 + dx, y0 + dy, 0); col = shade(base, (uint8_t)(h & 0xFF)); }
            put_px(fb, x0 + dx, y0 + dy, col);
        }
    }
}
static void draw_gear(uint16_t *fb, int cx, int cy, int r, uint16_t col, uint16_t hole_col) {
    int inner = r - 2; if (inner < 1) inner = 1;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            if (d2 <= inner * inner) put_px(fb, cx + dx, cy + dy, hole_col);
            else put_px(fb, cx + dx, cy + dy, col);
        }
    }
    static const int tdx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int tdy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    for (int k = 0; k < 8; k++) {
        put_px(fb, cx + tdx[k] * (r + 1), cy + tdy[k] * (r + 1), col);
        put_px(fb, cx + tdx[k] * (r + 2), cy + tdy[k] * (r + 2), col);
    }
    put_px(fb, cx, cy, col);
}

// ---------------------------------------------------------------------
// Grid helpers
// ---------------------------------------------------------------------
static inline uint8_t get_cell(int x, int y) {
    if (x < 0 || x >= GW || y < 0 || y >= GH) return STONE;
    return grid[y * GW + x];
}
static inline void set_cell(int x, int y, uint8_t v) {
    int i = y * GW + x;
    grid[i] = v; life[i] = 0; tint[i] = (uint8_t)xrand();
}
static inline void set_cell_life(int x, int y, uint8_t v, uint8_t l) {
    int i = y * GW + x;
    grid[i] = v; life[i] = l; tint[i] = (uint8_t)xrand();
}
static inline void swap_cells(int x1, int y1, int x2, int y2) {
    int i1 = y1 * GW + x1, i2 = y2 * GW + x2;
    uint8_t tg = grid[i1], tl = life[i1], tt = tint[i1];
    grid[i1] = grid[i2]; life[i1] = life[i2]; tint[i1] = tint[i2];
    grid[i2] = tg;       life[i2] = tl;       tint[i2] = tt;
}
static inline int liquid_can_enter(uint8_t self, uint8_t other) {
    if (other == EMPTY) return 1;
    if (elem_category[other] == CAT_GAS) return 1;
    if (elem_category[other] == CAT_LIQUID) return liquid_density[other] < liquid_density[self];
    return 0;
}
static void explode(int cx, int cy) {
    do_rumble(1.0f, 220);
    sfx(70.0f, 1.0f);
    const int R = 4;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > R * R) continue;
            int tx = cx + dx, ty = cy + dy;
            if (tx < 0 || tx >= GW || ty < 0 || ty >= GH) continue;
            if (get_cell(tx, ty) == STONE) continue;
            if (d2 <= (R - 1) * (R - 1)) set_cell(tx, ty, EMPTY);
            else set_cell_life(tx, ty, FIRE, 20 + (xrand() % 20));
        }
    }
}
static void try_ignite(int x, int y) {
    uint8_t c = get_cell(x, y);
    if (explosive[c] && chance(15)) { explode(x, y); return; }
    if (flammable[c] && chance(10)) set_cell_life(x, y, FIRE, 40 + (xrand() % 50));
}
static int liquid_scan(uint8_t self, int x, int y, int dx, int limit) {
    int best = 0;
    for (int d = 1; d <= limit; d++) {
        if (!liquid_can_enter(self, get_cell(x + dx * d, y))) break;
        best = d;
    }
    return best;
}
static void clear_board(void) {
    for (int i = 0; i < GW * GH; i++) { grid[i] = EMPTY; life[i] = 0; tint[i] = 0; }
    portal1_x = portal1_y = portal2_x = portal2_y = -1;
}
static uint8_t smash_result(uint8_t c) {
    switch (c) {
        case STONE: return GRAVEL;
        case BRICK: return GRAVEL;
        case CONCRETE: return GRAVEL;
        case GLASS: return SAND;
        case ICE: return SNOW;
        case CRYSTAL: return SAND;
    }
    return c;
}

// portal teleport check - call at the top of a mover's turn; returns 1 if it
// got relocated this tick (caller should skip the rest of its movement)
static int try_portal(int x, int y) {
    static const int nx[4] = {1, -1, 0, 0}, ny[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; k++) {
        uint8_t n = get_cell(x + nx[k], y + ny[k]);
        int tx = -1, ty = -1;
        if (n == PORTAL1 && portal2_x >= 0) { tx = portal2_x; ty = portal2_y; }
        else if (n == PORTAL2 && portal1_x >= 0) { tx = portal1_x; ty = portal1_y; }
        if (tx < 0) continue;
        for (int j = 0; j < 4; j++) {
            int ox = tx + nx[j], oy = ty + ny[j];
            if (get_cell(ox, oy) == EMPTY) {
                int si = y * GW + x, di = oy * GW + ox;
                grid[di] = grid[si]; life[di] = life[si]; tint[di] = tint[si];
                grid[si] = EMPTY; life[si] = 0; tint[si] = 0;
                return 1;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------
// Save/load
// ---------------------------------------------------------------------
typedef struct { uint8_t g[GW * GH]; uint8_t l[GW * GH]; uint8_t t[GW * GH]; } SaveBlob;
static SaveBlob save_buf;

static void do_save(int slot) {
    for (int i = 0; i < GW * GH; i++) { save_buf.g[i] = grid[i]; save_buf.l[i] = life[i]; save_buf.t[i] = tint[i]; }
    mote->save(slot, &save_buf, sizeof save_buf);
}
static void do_load(int slot) {
    if (mote->load(slot, &save_buf, sizeof save_buf)) {
        for (int i = 0; i < GW * GH; i++) { grid[i] = save_buf.g[i]; life[i] = save_buf.l[i]; tint[i] = save_buf.t[i]; }
    }
}
static void make_thumbnail(int slot) {
    const int stride = GW / THUMB_N;
    for (int ty = 0; ty < THUMB_N; ty++)
        for (int tx = 0; tx < THUMB_N; tx++)
            thumb[slot][ty * THUMB_N + tx] = elem_color[get_cell(tx * stride + stride / 2, ty * stride + stride / 2)];
    thumb_has_data[slot] = 1;
}
static void make_thumbnail_from_blob(int slot, const SaveBlob *blob) {
    const int stride = GW / THUMB_N;
    for (int ty = 0; ty < THUMB_N; ty++) {
        for (int tx = 0; tx < THUMB_N; tx++) {
            int gx = tx * stride + stride / 2, gy = ty * stride + stride / 2;
            thumb[slot][ty * THUMB_N + tx] = elem_color[blob->g[gy * GW + gx]];
        }
    }
    thumb_has_data[slot] = 1;
}
static void do_autosave(void) {
    for (int i = 0; i < GW * GH; i++) { save_buf.g[i] = grid[i]; save_buf.l[i] = life[i]; save_buf.t[i] = tint[i]; }
    mote->kv_save("autosave", &save_buf, sizeof save_buf);
}

// ---------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------
static void step_gravity(int ltr) {
    for (int y = GH - 2; y >= 0; y--) {
        for (int xi = 0; xi < GW; xi++) {
            int x = ltr ? xi : (GW - 1 - xi);
            uint8_t c = get_cell(x, y);
            int cat = elem_category[c];

            if ((cat == CAT_POWDER || cat == CAT_LIQUID) && try_portal(x, y)) continue;

            if (cat == CAT_POWDER) {
                static const int nx[4] = {1, -1, 0, 0}, ny[4] = {0, 0, 1, -1};
                if (c == BOMB) {
                    uint8_t l = life[y * GW + x];
                    if (l == 0) l = 60;
                    if (l <= 1) { explode(x, y); continue; }
                    life[y * GW + x] = l - 1;
                }
                if (c == FIREWORK) {
                    uint8_t l = life[y * GW + x];
                    if (l == 0) l = 50;
                    if (l <= 1) {
                        static const int fx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, fy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
                        for (int i = 0; i < 10; i++) {
                            int ang = xrand() % 8;
                            int tx = x + fx[ang] * (1 + (xrand() % 3)), ty = y + fy[ang] * (1 + (xrand() % 3));
                            if (get_cell(tx, ty) == EMPTY) set_cell_life(tx, ty, SPARK, 8 + (xrand() % 10));
                        }
                        do_rumble(0.8f, 80); sfx(500.0f + (float)(xrand() % 400), 0.6f);
                        set_cell(x, y, EMPTY);
                        continue;
                    }
                    life[y * GW + x] = l - 1;
                }
                if (c == SAND || c == DIRT) {
                    int reacted = 0;
                    for (int k = 0; k < 4 && !reacted; k++) {
                        uint8_t n = get_cell(x + nx[k], y + ny[k]);
                        if (n == WATER && chance(3)) { set_cell(x, y, MUD); reacted = 1; }
                        else if ((n == FIRE || n == LAVA) && c == SAND && chance(4)) { set_cell(x, y, GLASS); reacted = 1; }
                    }
                    if (reacted) continue;
                }
                if (c == SNOW) {
                    int melted = 0;
                    for (int k = 0; k < 4 && !melted; k++) {
                        uint8_t n = get_cell(x + nx[k], y + ny[k]);
                        if ((n == FIRE || n == LAVA) && chance(8)) { set_cell(x, y, WATER); melted = 1; }
                    }
                    if (melted) continue;
                }
                if (c == SEED) {
                    uint8_t below = get_cell(x, y + 1);
                    if ((below == DIRT || below == SAND || below == MUD || below == STONE) && chance(5)) {
                        set_cell(x, y, PLANT); continue;
                    }
                }
                if (c == SALT) {
                    int dissolved = 0;
                    for (int k = 0; k < 4 && !dissolved; k++) {
                        uint8_t n = get_cell(x + nx[k], y + ny[k]);
                        if (n == WATER && chance(10)) { set_cell(x, y, EMPTY); dissolved = 1; }
                        else if ((n == PLANT || n == MOLD) && chance(8)) set_cell(x + nx[k], y + ny[k], EMPTY);
                    }
                    if (dissolved) continue;
                }
                if (c == CEMENT) {
                    int hardened = 0;
                    for (int k = 0; k < 4 && !hardened; k++)
                        if (get_cell(x + nx[k], y + ny[k]) == WATER && chance(8)) { set_cell(x, y, CONCRETE); hardened = 1; }
                    if (hardened) continue;
                }
                if (c == ASH) {
                    // life[] doubles as a decay countdown, but ONLY for ash created
                    // from burnt-out fire (step_fire seeds it with a nonzero value).
                    // Ash placed directly by the user comes from set_cell(), which
                    // always resets life to 0 - life==0 now means "permanent", so
                    // we no longer lazily re-seed it here.
                    uint8_t l = life[y * GW + x];
                    if (l > 0) {
                        if (l <= 1) { set_cell(x, y, EMPTY); continue; }
                        life[y * GW + x] = l - 1;
                    }
                }

                // small chance to sit out this tick - desyncs neighboring grains so a
                // placed block visibly crumbles/scatters as it falls (matches the
                // reference: a solid square breaking into an uneven, speckled heap)
                // instead of the whole block sliding down in perfect unison.
                if (!chance(92)) continue;

                uint8_t below = get_cell(x, y + 1);
                if (below == EMPTY || elem_category[below] == CAT_GAS) { swap_cells(x, y, x, y + 1); continue; }
                int dx = ltr ? -1 : 1;
                uint8_t d1 = get_cell(x + dx, y + 1);
                if (d1 == EMPTY || elem_category[d1] == CAT_GAS) { swap_cells(x, y, x + dx, y + 1); continue; }
                uint8_t d2 = get_cell(x - dx, y + 1);
                if (d2 == EMPTY || elem_category[d2] == CAT_GAS) swap_cells(x, y, x - dx, y + 1);
            }
            else if (cat == CAT_LIQUID) {
                static const int nx[4] = {1, -1, 0, 0}, ny[4] = {0, 0, 1, -1};
                if (c == LAVA) {
                    // cools into stone after enough sustained water contact (life[] reused as a contact counter)
                    for (int k = 0; k < 4; k++) {
                        uint8_t n = get_cell(x + nx[k], y + ny[k]);
                        if (n == WATER) {
                            set_cell_life(x + nx[k], y + ny[k], STEAM, 30 + (xrand() % 40));
                            int idx = y * GW + x;
                            if (life[idx] < 200) life[idx]++;
                            if (life[idx] >= 3) { set_cell(x, y, STONE); }
                        } else if (flammable[n] || explosive[n]) {
                            try_ignite(x + nx[k], y + ny[k]);
                        }
                    }
                    if (get_cell(x, y) != LAVA) continue;
                }
                if (c == ACID) {
                    for (int k = 0; k < 4; k++) {
                        int tx = x + nx[k], ty = y + ny[k];
                        uint8_t n = get_cell(tx, ty);
                        if (acid_dissolvable[n] && chance(6)) {
                            set_cell(tx, ty, EMPTY);
                            if (chance(20)) set_cell(x, y, EMPTY);
                        } else if (n == METAL && chance(1)) {
                            set_cell(tx, ty, EMPTY); // metal corrodes much slower
                        }
                    }
                    if (get_cell(x, y) != ACID) continue;
                }

                // small chance to sit out this tick, same trick as powders - keeps a
                // falling/spreading blob of liquid from moving as one solid mass,
                // giving it a scattered, splashy look while in motion. Kept light
                // (95%, vs powder's 92%) so liquids still flatten/pool quickly.
                if (!chance(95)) continue;

                uint8_t below = get_cell(x, y + 1);
                if (liquid_can_enter(c, below)) { swap_cells(x, y, x, y + 1); continue; }
                int dx = ltr ? -1 : 1;
                uint8_t d1 = get_cell(x + dx, y + 1);
                if (liquid_can_enter(c, d1)) { swap_cells(x, y, x + dx, y + 1); continue; }
                uint8_t d2 = get_cell(x - dx, y + 1);
                if (liquid_can_enter(c, d2)) { swap_cells(x, y, x - dx, y + 1); continue; }

                int lim = spread_limit_for(c);
                int best = liquid_scan(c, x, y, dx, lim);
                if (best > 0) { swap_cells(x, y, x + dx * best, y); continue; }
                best = liquid_scan(c, x, y, -dx, lim);
                if (best > 0) swap_cells(x, y, x - dx * best, y);
            }
            else if (cat == CAT_STATIC) {
                static const int nx[4] = {1, -1, 0, 0}, ny[4] = {0, 0, 1, -1};
                if (c == ICE) {
                    int melted = 0;
                    for (int k = 0; k < 4; k++) {
                        uint8_t n = get_cell(x + nx[k], y + ny[k]);
                        if ((n == FIRE || n == LAVA) && chance(6)) { set_cell(x, y, WATER); melted = 1; break; }
                    }
                    if (!melted) {
                        for (int k = 0; k < 4; k++)
                            if (get_cell(x + nx[k], y + ny[k]) == WATER && chance(2)) set_cell(x + nx[k], y + ny[k], ICE);
                    }
                }
                else if (c == PLANT) {
                    // grows upward but caps out (life[] reused as a height counter)
                    uint8_t h = life[y * GW + x];
                    if (h < 20 && get_cell(x, y - 1) == EMPTY && chance(2)) set_cell_life(x, y - 1, PLANT, h + 1);
                }
                else if (c == MOLD) {
                    int wet = 0;
                    for (int k = 0; k < 4; k++) if (get_cell(x + nx[k], y + ny[k]) == WATER) wet = 1;
                    if (wet) {
                        for (int k = 0; k < 4; k++) {
                            uint8_t n = get_cell(x + nx[k], y + ny[k]);
                            if ((n == WOOD || n == PLANT) && chance(2)) set_cell(x + nx[k], y + ny[k], MOLD);
                        }
                    }
                }
                else if (c == VINE) {
                    static const int vx[6] = {1, -1, 0, 1, -1, 0}, vy[6] = {0, 0, -1, -1, -1, 1};
                    int k = xrand() % 6;
                    int tx = x + vx[k], ty = y + vy[k];
                    if (get_cell(tx, ty) == EMPTY && chance(3)) set_cell(tx, ty, VINE);
                }
                else if (c == CRYSTAL) {
                    uint8_t h = life[y * GW + x];
                    if (h < 15 && chance(3)) {
                        static const int gx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, gy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                        int k = xrand() % 8;
                        int tx = x + gx[k], ty = y + gy[k];
                        if (get_cell(tx, ty) == EMPTY) set_cell_life(tx, ty, CRYSTAL, h + 1);
                    }
                }
                else if (c == BLACKHOLE) {
                    const int PULL_R = 6;
                    for (int dy = -PULL_R; dy <= PULL_R; dy++) {
                        for (int dx = -PULL_R; dx <= PULL_R; dx++) {
                            int d2 = dx * dx + dy * dy;
                            if (d2 == 0 || d2 > PULL_R * PULL_R) continue;
                            int tx = x + dx, ty = y + dy;
                            uint8_t tc = get_cell(tx, ty);
                            if (tc == EMPTY || tc == STONE || tc == BLACKHOLE) continue;
                            if (d2 <= 4) {
                                if (chance(40)) set_cell(tx, ty, EMPTY);
                            } else if (chance(6)) {
                                int sx = (dx > 0) ? -1 : (dx < 0 ? 1 : 0);
                                int sy = (dy > 0) ? -1 : (dy < 0 ? 1 : 0);
                                int nxp = tx + sx, nyp = ty + sy;
                                if (get_cell(nxp, nyp) == EMPTY) swap_cells(tx, ty, nxp, nyp);
                            }
                        }
                    }
                }
            }
            // NOTE: fire is no longer handled here - it has its own step_fire()
            // pass below so it can float/wander instead of sitting as a static block
        }
    }
}

static void step_gas(void) {
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            uint8_t c = grid[y * GW + x];
            if (elem_category[c] != CAT_GAS) continue;

            if (try_portal(x, y)) continue;

            static const int nx[4] = {1, -1, 0, 0}, ny[4] = {0, 0, 1, -1};

            if (c == SPARK) {
                int near_metal = 0;
                for (int k = 0; k < 4; k++) if (get_cell(x + nx[k], y + ny[k]) == METAL) near_metal = 1;
                for (int k = 0; k < 4; k++) try_ignite(x + nx[k], y + ny[k]);
                if (near_metal) {
                    // "conducts" by hopping toward another metal-adjacent empty cell
                    for (int attempt = 0; attempt < 6; attempt++) {
                        int rx = x + (int)(xrand() % 9) - 4, ry = y + (int)(xrand() % 9) - 4;
                        if (get_cell(rx, ry) != EMPTY) continue;
                        int adj_metal = 0;
                        for (int k = 0; k < 4; k++) if (get_cell(rx + nx[k], ry + ny[k]) == METAL) adj_metal = 1;
                        if (adj_metal) { swap_cells(x, y, rx, ry); break; }
                    }
                    uint8_t l = life[y * GW + x];
                    if (l == 0) l = 10 + (xrand() % 15);
                    if (l <= 1) { set_cell(x, y, EMPTY); continue; }
                    life[y * GW + x] = l - 1;
                    continue; // skip the generic float-up movement below
                }
            }
            if (c == TOXIC) {
                for (int k = 0; k < 4; k++) {
                    uint8_t n = get_cell(x + nx[k], y + ny[k]);
                    if ((n == PLANT || n == MOLD) && chance(8)) set_cell(x + nx[k], y + ny[k], EMPTY);
                }
            }

            uint8_t l = life[y * GW + x];
            if (l == 0) {
                if (c == FOG) l = 80 + (xrand() % 70);
                else l = 40 + (xrand() % 60);
            }
            if (l <= 1) {
                if (c == STEAM && chance(40)) set_cell(x, y, WATER); // condenses back to water
                else set_cell(x, y, EMPTY);
                continue;
            }
            life[y * GW + x] = l - 1;

            uint8_t above = get_cell(x, y - 1);
            if (above == EMPTY) { swap_cells(x, y, x, y - 1); continue; }
            int dx = (xrand() & 1) ? 1 : -1;
            uint8_t d1 = get_cell(x + dx, y - 1);
            if (d1 == EMPTY) { swap_cells(x, y, x + dx, y - 1); continue; }
            uint8_t side = get_cell(x + dx, y);
            if (side == EMPTY) swap_cells(x, y, x + dx, y);
        }
    }
}

// Fire gets its own pass, top-to-bottom like gas - moving upward in the same
// bottom-to-top pass used for falling things would let it jump several rows
// in one frame (the same bug gas had before it got split out). This also
// means placed fire immediately starts wandering instead of sitting as a
// static filled block.
static void step_fire(void) {
    static const int nx[4] = {1, -1, 0, 0}, ny[4] = {0, 0, 1, -1};
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            if (grid[y * GW + x] != FIRE) continue;

            uint8_t l = life[y * GW + x];
            if (l == 0) l = 40 + (xrand() % 50);
            if (l <= 1) {
                if (chance(40)) set_cell_life(x, y, SMOKE, 30 + (xrand() % 40));
                else set_cell_life(x, y, ASH, 120 + (xrand() % 80)); // marks it as decaying residue
                continue;
            }
            life[y * GW + x] = l - 1;

            int extinguished = 0;
            for (int k = 0; k < 4; k++) {
                int tx = x + nx[k], ty = y + ny[k];
                uint8_t n = get_cell(tx, ty);
                if (n == WATER) {
                    set_cell_life(tx, ty, STEAM, 30 + (xrand() % 40));
                    set_cell(x, y, EMPTY);
                    extinguished = 1;
                    break;
                } else {
                    try_ignite(tx, ty);
                }
            }
            if (extinguished) continue;

            // wandering flicker: mostly drifts up, sometimes sideways, not every tick -
            // gives it a floating/dancing look instead of a rigid placed shape
            int wig = (int)(xrand() % 3) - 1; // -1, 0, or 1
            uint8_t above = get_cell(x, y - 1);
            if (above == EMPTY && chance(55)) { swap_cells(x, y, x, y - 1); continue; }
            if (wig != 0) {
                uint8_t diag = get_cell(x + wig, y - 1);
                if (diag == EMPTY && chance(40)) { swap_cells(x, y, x + wig, y - 1); continue; }
                uint8_t side = get_cell(x + wig, y);
                if (side == EMPTY && chance(20)) swap_cells(x, y, x + wig, y);
            }
        }
    }
}

static void step_sim(void) {
    int ltr = (frame_count & 1) == 0;
    frame_count++;
    step_gravity(ltr);
    step_gas();
    step_fire();
}

// ---------------------------------------------------------------------
// Placement helpers (brush shape + mirror, shared by Place tool and Line tool)
// ---------------------------------------------------------------------
static void place_brush(int cx, int cy, uint8_t v) {
    for (int by = -brush_radius; by <= brush_radius; by++) {
        for (int bx = -brush_radius; bx <= brush_radius; bx++) {
            if (circle_brush && (bx * bx + by * by) > brush_radius * brush_radius) continue;
            int tx = cx + bx, ty = cy + by;
            if (tx < 0 || tx >= GW || ty < 0 || ty >= GH) continue;
            set_cell(tx, ty, v);
            if (v == PORTAL1) { portal1_x = tx; portal1_y = ty; }
            else if (v == PORTAL2) { portal2_x = tx; portal2_y = ty; }
        }
    }
    if (mirror_mode) {
        int mx = GW - 1 - cx;
        for (int by = -brush_radius; by <= brush_radius; by++) {
            for (int bx = -brush_radius; bx <= brush_radius; bx++) {
                if (circle_brush && (bx * bx + by * by) > brush_radius * brush_radius) continue;
                int tx = mx + bx, ty = cy + by;
                if (tx < 0 || tx >= GW || ty < 0 || ty >= GH) continue;
                set_cell(tx, ty, v);
            }
        }
    }
}
static void draw_line(int x0, int y0, int x1, int y1, uint8_t v) {
    int dxr = x1 - x0; if (dxr < 0) dxr = -dxr;
    int dyr = y1 - y0; if (dyr < 0) dyr = -dyr;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dxr - dyr;
    int gx = x0, gy = y0;
    for (;;) {
        place_brush(gx, gy, v);
        if (gx == x1 && gy == y1) break;
        int e2 = 2 * err;
        if (e2 > -dyr) { err -= dyr; gx += sx; }
        if (e2 < dxr) { err += dxr; gy += sy; }
    }
}

// ---------------------------------------------------------------------
// Game callbacks
// ---------------------------------------------------------------------
static void g_init(void) {
    clear_board();
    for (int slot = 0; slot < 4; slot++)
        if (mote->load(slot, &save_buf, sizeof save_buf)) make_thumbnail_from_blob(slot, &save_buf);
}

static void open_menu(void) {
    menu_open = 1;
    for (int t = 0; t < N_TABS; t++)
        for (int i = 0; i < tab_count[t]; i++)
            if (tab_items[t][i] == current_elem) { cat_idx = t; item_idx = i; return; }
    cat_idx = 0; item_idx = 0;
}

static void update_cursor_position(void) {
    if (cursor_fx < 0) cursor_fx = 0;
    if (cursor_fy < 0) cursor_fy = 0;
    if (cursor_fx >= GW) cursor_fx = GW - 1;
    if (cursor_fy >= GH) cursor_fy = GH - 1;
    cursor_x = (int)cursor_fx;
    cursor_y = (int)cursor_fy;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    autosave_timer += dt;
    if (autosave_enabled && autosave_timer >= autosave_interval) {
        do_autosave();
        autosave_timer = 0.0f;
        autosave_notify_timer = 2.5f; // shown briefly in the top-right corner
    }
    if (autosave_notify_timer > 0.0f) autosave_notify_timer -= dt;

    if (mote_just_pressed(in, MOTE_BTN_MENU)) {
        if (menu_open) { menu_open = 0; sfx(400.0f, 0.4f); } else { open_menu(); sfx(520.0f, 0.4f); }
    }

    if (menu_open) {
        if (mote_just_pressed(in, MOTE_BTN_LB)) { cat_idx = (cat_idx - 1 + N_TABS) % N_TABS; item_idx = 0; }
        if (mote_just_pressed(in, MOTE_BTN_RB)) { cat_idx = (cat_idx + 1) % N_TABS; item_idx = 0; }

        int n = tab_count[cat_idx];

        if (cat_idx == SETTINGS_TAB) {
            if (mote_just_pressed(in, MOTE_BTN_UP) && item_idx > 0) item_idx--;
            if (mote_just_pressed(in, MOTE_BTN_DOWN) && item_idx < n - 1) item_idx++;

            uint8_t sel = tab_items[cat_idx][item_idx];
            if (sel == ACTION_SENSITIVITY) {
                if (mote_just_pressed(in, MOTE_BTN_LEFT) && sensitivity > 1) sensitivity--;
                if (mote_just_pressed(in, MOTE_BTN_RIGHT) && sensitivity < 10) sensitivity++;
            } else if (sel == ACTION_AUTOSAVE_INTERVAL) {
                if (mote_just_pressed(in, MOTE_BTN_LEFT) || mote_just_pressed(in, MOTE_BTN_RIGHT)) {
                    if (autosave_interval <= 30.0f) autosave_interval = 60.0f;
                    else if (autosave_interval <= 60.0f) autosave_interval = 120.0f;
                    else autosave_interval = 30.0f;
                    autosave_timer = 0.0f;
                }
            }
            if (mote_just_pressed(in, MOTE_BTN_A)) {
                if (sel == ACTION_AUTOSAVE) { autosave_enabled = !autosave_enabled; autosave_timer = 0.0f; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_SHOW_LABEL) { show_label = !show_label; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_PIXEL_MODE) { pixel_mode = !pixel_mode; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_PAUSE_SIM) { sim_paused = !sim_paused; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_CIRCLE_BRUSH) { circle_brush = !circle_brush; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_MIRROR) { mirror_mode = !mirror_mode; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_HOVER_DISPLAY) { hover_display = !hover_display; sfx(440.0f, 0.5f); }
                else if (sel == ACTION_MASTER_VOLUME) { sound_enabled = !sound_enabled; if (sound_enabled) sfx(440.0f, 0.6f); }
                else if (sel == ACTION_MASTER_RUMBLE) { rumble_enabled = !rumble_enabled; sfx(440.0f, 0.5f); if (rumble_enabled) do_rumble(0.6f, 40); }
            }
        } else if (cat_idx == SPECIAL_TAB) {
            if (mote_just_pressed(in, MOTE_BTN_LEFT)) item_idx = (item_idx - 1 + n) % n;
            if (mote_just_pressed(in, MOTE_BTN_RIGHT)) item_idx = (item_idx + 1) % n;
            uint8_t sel = tab_items[cat_idx][item_idx];
            int slot = ACTION_SAVE1 - sel;
            if (mote_just_pressed(in, MOTE_BTN_UP)) { do_save(slot); make_thumbnail(slot); do_rumble(1.0f, 100); sfx(880.0f, 0.6f); }
            if (mote_just_pressed(in, MOTE_BTN_A)) { do_load(slot); do_rumble(0.7f, 60); sfx(660.0f, 0.6f); }
        } else if (cat_idx == TOOLS_TAB) {
            if (mote_just_pressed(in, MOTE_BTN_LEFT)) item_idx = (item_idx - 1 + n) % n;
            if (mote_just_pressed(in, MOTE_BTN_RIGHT)) item_idx = (item_idx + 1) % n;
            if (mote_just_pressed(in, MOTE_BTN_A)) { current_tool = tab_items[cat_idx][item_idx]; menu_open = 0; sfx(500.0f, 0.4f); }
        } else {
            if (mote_just_pressed(in, MOTE_BTN_LEFT)) item_idx = (item_idx - 1 + n) % n;
            if (mote_just_pressed(in, MOTE_BTN_RIGHT)) item_idx = (item_idx + 1) % n;
            if (mote_just_pressed(in, MOTE_BTN_A)) { current_elem = tab_items[cat_idx][item_idx]; menu_open = 0; }
        }

        if (mote_just_pressed(in, MOTE_BTN_B)) menu_open = 0;
        return;
    }

    // brush resize: single press moves by 1, holding accelerates (faster after
    // ~1.5s) so reaching the new max of 64 - big enough to cover the whole
    // 128x128 grid from any cursor position - doesn't take forever
    {
        int lb = mote_pressed(in, MOTE_BTN_LB);
        int rb = mote_pressed(in, MOTE_BTN_RB);
        int lb_first = mote_just_pressed(in, MOTE_BTN_LB);
        int rb_first = mote_just_pressed(in, MOTE_BTN_RB);

        if (!lb && !rb) {
            brush_hold_time = 0.0f;
            brush_repeat_timer = 0.0f;
        } else {
            brush_hold_time += dt;
        }

        if (lb_first) { brush_radius--; brush_repeat_timer = 0.35f; }
        if (rb_first) { brush_radius++; brush_repeat_timer = 0.35f; }

        if (!lb_first && !rb_first && (lb || rb)) {
            brush_repeat_timer -= dt;
            if (brush_repeat_timer <= 0.0f) {
                int step = (brush_hold_time > 1.5f) ? 4 : 1;
                float interval = (brush_hold_time > 1.5f) ? 0.05f : 0.12f;
                if (lb) brush_radius -= step;
                if (rb) brush_radius += step;
                brush_repeat_timer = interval;
            }
        }
        if (brush_radius < 0) brush_radius = 0;
        if (brush_radius > 64) brush_radius = 64;
    }

    // ---- cursor movement (unchanged from before) ----
    if (pixel_mode) {
        int any_dir = mote_pressed(in, MOTE_BTN_LEFT) || mote_pressed(in, MOTE_BTN_RIGHT) ||
                      mote_pressed(in, MOTE_BTN_UP) || mote_pressed(in, MOTE_BTN_DOWN);
        if (!any_dir) {
            pixel_hold_time = 0.0f; pixel_fast_repeat = 0; pixel_move_timer = 0.0f;
        } else {
            pixel_hold_time += dt;
            if (!pixel_fast_repeat && pixel_hold_time >= PIXEL_FAST_THRESHOLD) {
                pixel_fast_repeat = 1; pixel_move_timer = 0.0f; sfx(700.0f, 0.35f);
            }
        }
        fast_repeat_active = pixel_fast_repeat;
        if (pixel_fast_repeat) {
            pixel_move_timer -= dt;
            if (pixel_move_timer <= 0.0f) {
                pixel_move_timer = pixel_move_delay;
                if (mote_pressed(in, MOTE_BTN_LEFT))  cursor_fx -= 1.0f;
                if (mote_pressed(in, MOTE_BTN_RIGHT)) cursor_fx += 1.0f;
                if (mote_pressed(in, MOTE_BTN_UP))    cursor_fy -= 1.0f;
                if (mote_pressed(in, MOTE_BTN_DOWN))  cursor_fy += 1.0f;
            }
        } else {
            if (mote_just_pressed(in, MOTE_BTN_LEFT))  cursor_fx -= 1.0f;
            if (mote_just_pressed(in, MOTE_BTN_RIGHT)) cursor_fx += 1.0f;
            if (mote_just_pressed(in, MOTE_BTN_UP))    cursor_fy -= 1.0f;
            if (mote_just_pressed(in, MOTE_BTN_DOWN))  cursor_fy += 1.0f;
        }
    } else {
        fast_repeat_active = 0;
        float speed = cursor_speed + (sensitivity - 5) * 8.0f;
        if (speed < 10.0f) speed = 10.0f;
        if (mote_pressed(in, MOTE_BTN_LEFT))  cursor_fx -= speed * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) cursor_fx += speed * dt;
        if (mote_pressed(in, MOTE_BTN_UP))    cursor_fy -= speed * dt;
        if (mote_pressed(in, MOTE_BTN_DOWN))  cursor_fy += speed * dt;
    }
    update_cursor_position();

    // ---- tool behavior ----
    int dragging_now = 0;
    if (current_tool == TOOL_PLACE) {
        int placing = mote_pressed(in, MOTE_BTN_A);
        int erasing = mote_pressed(in, MOTE_BTN_B);
        if (placing || erasing) {
            int first = mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B);
            place_throttle -= dt;
            if (first || place_throttle <= 0.0f) {
                place_brush(cursor_x, cursor_y, placing ? (uint8_t)current_elem : EMPTY);
                place_throttle = 0.06f; // slower dropping while held, instead of every single frame
            }
            if (first) { do_rumble(1.0f, 90); sfx(placing ? 600.0f : 320.0f, 0.6f); hold_timer = 0; }
            else if (++hold_timer >= 8) { do_rumble(0.6f, 45); hold_timer = 0; }
        } else { hold_timer = 0; place_throttle = 0.0f; }
    }
    else if (current_tool == TOOL_LINE) {
        if (mote_just_pressed(in, MOTE_BTN_A)) { line_active = 1; line_start_x = cursor_x; line_start_y = cursor_y; }
        if (line_active && !mote_pressed(in, MOTE_BTN_A)) {
            draw_line(line_start_x, line_start_y, cursor_x, cursor_y, (uint8_t)current_elem);
            line_active = 0;
            do_rumble(0.6f, 60); sfx(700.0f, 0.5f);
        }
    }
    else if (current_tool == TOOL_EYEDROPPER) {
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            current_elem = get_cell(cursor_x, cursor_y);
            current_tool = TOOL_PLACE; // auto-switch back so the next press just places it
            sfx(900.0f, 0.4f);
        }
    }
    else if (current_tool == TOOL_DRAG) {
        // works on ANY element under the cursor - the swap below doesn't check
        // or care what type is there, it just moves whatever exists
        if (mote_pressed(in, MOTE_BTN_A)) {
            dragging_now = 1; // pauses gravity for this frame - see below
            if (drag_prev_x < 0) {
                // first frame of the hold: just grab, nothing to swap yet
                drag_prev_x = cursor_x; drag_prev_y = cursor_y;
            } else if (cursor_x != drag_prev_x || cursor_y != drag_prev_y) {
                // full swap regardless of what's at the destination or its type -
                // whatever it displaces goes back to where the dragged thing came from
                swap_cells(drag_prev_x, drag_prev_y, cursor_x, cursor_y);
                drag_prev_x = cursor_x; drag_prev_y = cursor_y;
            }
        } else { drag_prev_x = -1; drag_prev_y = -1; }
    }
    else if (current_tool == TOOL_MIX) {
        if (mote_pressed(in, MOTE_BTN_A)) {
            int r = brush_radius > 0 ? brush_radius : 3;
            for (int i = 0; i < 4; i++) {
                int x1 = cursor_x + (int)(xrand() % (2 * r + 1)) - r, y1 = cursor_y + (int)(xrand() % (2 * r + 1)) - r;
                int x2 = cursor_x + (int)(xrand() % (2 * r + 1)) - r, y2 = cursor_y + (int)(xrand() % (2 * r + 1)) - r;
                if (x1 >= 0 && x1 < GW && y1 >= 0 && y1 < GH && x2 >= 0 && x2 < GW && y2 >= 0 && y2 < GH)
                    swap_cells(x1, y1, x2, y2);
            }
        }
    }
    else if (current_tool == TOOL_SMASH) {
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            int r = brush_radius > 0 ? brush_radius : 3;
            for (int by = -r; by <= r; by++) {
                for (int bx = -r; bx <= r; bx++) {
                    int tx = cursor_x + bx, ty = cursor_y + by;
                    if (tx < 0 || tx >= GW || ty < 0 || ty >= GH) continue;
                    uint8_t c = get_cell(tx, ty);
                    uint8_t s = smash_result(c);
                    if (s != c) set_cell(tx, ty, s);
                }
            }
            do_rumble(0.8f, 70); sfx(150.0f, 0.6f);
        }
    }

    // NOTE: physics is skipped for this one frame while actively dragging.
    // Without this, gravity ran immediately after the swap above in the same
    // tick, and would yank a dragged powder/liquid straight back down before
    // you ever saw it move - which is exactly why dragging looked like it
    // "did nothing." Letting go resumes normal physics on the very next frame.
    if (!sim_paused && !dragging_now) step_sim();
}

static uint16_t fire_pixel_color(int x, int y, uint8_t life_val, unsigned fc) {
    uint16_t c0 = MOTE_RGB565(90, 10, 0);
    uint16_t c1 = MOTE_RGB565(200, 40, 10);
    uint16_t c2 = MOTE_RGB565(250, 110, 20);
    uint16_t c3 = MOTE_RGB565(255, 175, 50);
    uint16_t c4 = MOTE_RGB565(255, 225, 140);
    int bucket = life_val; if (bucket > 60) bucket = 60;
    int stop_idx = bucket / 13; if (stop_idx > 4) stop_idx = 4;
    uint16_t base;
    switch (stop_idx) {
        case 0: base = c0; break;
        case 1: base = c1; break;
        case 2: base = c2; break;
        case 3: base = c3; break;
        default: base = c4; break;
    }
    uint32_t h = hash2(x, y, fc >> 1);
    return shade(base, (uint8_t)(h & 0xFF));
}

static void g_render_band(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        uint16_t *row = fb + y * GW;
        for (int x = 0; x < GW; x++) {
            uint8_t c = get_cell(x, y);
            if (c == EMPTY) {
                int near_fire = get_cell(x-1,y)==FIRE || get_cell(x+1,y)==FIRE ||
                                 get_cell(x,y-1)==FIRE || get_cell(x,y+1)==FIRE;
                int near_lava = get_cell(x-1,y)==LAVA || get_cell(x+1,y)==LAVA ||
                                 get_cell(x,y-1)==LAVA || get_cell(x,y+1)==LAVA;
                row[x] = near_fire ? MOTE_RGB565(50,15,0) : (near_lava ? MOTE_RGB565(35,8,0) : 0x0000);
            } else if (c == FIRE) {
                row[x] = fire_pixel_color(x, y, life[y * GW + x], frame_count);
            } else if (elem_category[c] == CAT_POWDER || elem_category[c] == CAT_LIQUID) {
                row[x] = shade_grainy(elem_color[c], tint[y * GW + x]);
            } else {
                row[x] = shade(elem_color[c], tint[y * GW + x]);
            }
        }
    }
}

static void g_overlay(uint16_t *fb) {
    const uint16_t white = MOTE_RGB565(255, 255, 255);

    if (!menu_open) {
        uint16_t cursor_col = fast_repeat_active ? MOTE_RGB565(255, 220, 60) : white;
        outline_rect(fb, cursor_x - brush_radius, cursor_y - brush_radius,
                         cursor_x + brush_radius, cursor_y + brush_radius, cursor_col);

        if (show_label) {
            char buf[40];
            if (current_tool != TOOL_PLACE) snprintf(buf, sizeof buf, "%s [%s]", elem_name[current_elem], tool_name(current_tool));
            else snprintf(buf, sizeof buf, "%s", elem_name[current_elem]);
            fill_rect(fb, 0, 0, text_w_est(buf) + 6, 13, MOTE_RGB565(20, 20, 20));
            draw_label(fb, 3, 1, buf, elem_color[current_elem] ? elem_color[current_elem] : white);
        }
        if (hover_display) {
            uint8_t hv = get_cell(cursor_x, cursor_y);
            const char *buf = elem_name[hv];
            int w = text_w_est(buf);
            fill_rect(fb, GW - w - 6, 0, w + 6, 13, MOTE_RGB565(20, 20, 20));
            draw_label(fb, GW - w - 3, 1, buf, white);
        }
        if (autosave_notify_timer > 0.0f) {
            const char *buf = "AUTOSAVING...";
            int w = text_w_est(buf);
            fill_rect(fb, GW - w - 6, 14, w + 6, 13, MOTE_RGB565(20, 20, 20));
            draw_label(fb, GW - w - 3, 15, buf, MOTE_RGB565(120, 220, 140));
        }
        return;
    }

    const int PX0 = 4, PY0 = 6, PX1 = 124, PY1 = 122;
    fill_rect(fb, PX0, PY0, PX1 - PX0, PY1 - PY0, MOTE_RGB565(18, 18, 24));
    outline_rect(fb, PX0, PY0, PX1, PY1, MOTE_RGB565(90, 90, 60));
    outline_rect(fb, PX0 + 1, PY0 + 1, PX1 - 1, PY1 - 1, MOTE_RGB565(50, 50, 40));

    const int TOTAL_SLOTS = N_TABS;
    const int WIN = 4;
    int win_start = cat_idx - WIN / 2;
    if (win_start < 0) win_start = 0;
    if (win_start > TOTAL_SLOTS - WIN) win_start = TOTAL_SLOTS - WIN;

    int strip_w = PX1 - PX0 - 2;
    int slot_w = strip_w / WIN;
    for (int s = 0; s < WIN; s++) {
        int idx = win_start + s;
        if (idx < 0 || idx >= TOTAL_SLOTS) continue;
        int cx0 = PX0 + 1 + s * slot_w;
        int selected = (idx == cat_idx);
        if (selected) fill_rect(fb, cx0 + 1, PY0 + 3, slot_w - 2, 12, MOTE_RGB565(70, 70, 40));

        if (idx == SETTINGS_TAB) {
            uint16_t hole = selected ? MOTE_RGB565(70, 70, 40) : MOTE_RGB565(18, 18, 24);
            draw_gear(fb, cx0 + slot_w / 2, PY0 + 9, 4,
                      selected ? MOTE_RGB565(255, 220, 100) : MOTE_RGB565(160, 160, 160), hole);
        } else {
            const char *ab = tab_abbrev[idx];
            int tw = text_w_est(ab);
            draw_label(fb, cx0 + (slot_w - tw) / 2, PY0 + 4, ab,
                       selected ? MOTE_RGB565(255, 220, 100) : MOTE_RGB565(160, 160, 160));
        }
    }
    outline_rect(fb, PX0 + 1, PY0 + 22, PX1 - 1, PY0 + 22, MOTE_RGB565(50, 50, 40));

    int cy0 = PY0 + 28;

    if (cat_idx == SETTINGS_TAB) {
        // scrolling window so the list can grow without overflowing the panel
        const int ROWS = 5;
        int n = tab_count[SETTINGS_TAB];
        int sw = item_idx - ROWS / 2;
        if (sw < 0) sw = 0;
        if (sw > n - ROWS) sw = n - ROWS;
        if (sw < 0) sw = 0;

        const int bw = PX1 - PX0 - 16, bx = PX0 + 8, bh = 14, gap = 3;
        for (int row = 0; row < ROWS; row++) {
            int i = sw + row;
            if (i >= n) break;
            uint8_t id = tab_items[SETTINGS_TAB][i];
            uint16_t col = display_color(id);
            int y = cy0 + row * (bh + gap);
            if (i == item_idx) outline_rect(fb, bx - 2, y - 2, bx + bw + 1, y + bh + 1, white);
            fill_rect(fb, bx, y, bw, bh, col);

            if (id == ACTION_SENSITIVITY) {
                draw_label(fb, bx + 3, y + 2, "SENS", MOTE_RGB565(15, 15, 15));
                int track_x = bx + 38, track_w = bw - 44, track_h = 7, track_y = y + 4;
                fill_rect(fb, track_x, track_y, track_w, track_h, MOTE_RGB565(15, 15, 15));
                fill_rect(fb, track_x, track_y, track_w * sensitivity / 10, track_h, MOTE_RGB565(255, 255, 255));
            } else {
                const char *label = format_display_name(id);
                int tw = text_w_est(label);
                draw_label(fb, bx + (bw - tw) / 2, y + (bh - 11) / 2 + 1, label, MOTE_RGB565(15, 15, 15));
            }
        }
        // small scroll hint
        if (n > ROWS) {
            char sbuf[8]; snprintf(sbuf, sizeof sbuf, "%d/%d", item_idx + 1, n);
            draw_label(fb, PX1 - 24, PY0 + 24, sbuf, MOTE_RGB565(150, 150, 150));
        }
    } else if (cat_idx == SPECIAL_TAB) {
        const int box = 22, gap = 3, pitch = box + gap;
        int n = tab_count[SPECIAL_TAB];
        int total_w = n * pitch - gap;
        int start_x = PX0 + (PX1 - PX0 - total_w) / 2;
        int sy = cy0 + 4;
        for (int i = 0; i < n; i++) {
            int x = start_x + i * pitch;
            uint8_t id = tab_items[SPECIAL_TAB][i];
            if (i == item_idx) outline_rect(fb, x - 2, sy - 2, x + box + 1, sy + box + 1, white);
            int slot = ACTION_SAVE1 - id;
            if (thumb_has_data[slot]) {
                for (int ty = 0; ty < THUMB_N; ty++)
                    for (int tx = 0; tx < THUMB_N; tx++)
                        put_px(fb, x + 3 + tx, sy + 3 + ty, thumb[slot][ty * THUMB_N + tx]);
                outline_rect(fb, x + 2, sy + 2, x + 2 + THUMB_N + 1, sy + 2 + THUMB_N + 1, MOTE_RGB565(80, 80, 80));
            } else {
                fill_squircle(fb, x + 2, sy + 2, THUMB_N + 2, MOTE_RGB565(40, 40, 40), 0);
            }
        }
        const char *name = format_display_name(tab_items[SPECIAL_TAB][item_idx]);
        draw_label(fb, PX0 + ((PX1 - PX0) - text_w_est(name)) / 2, sy + box + 6, name, white);
        const char *hint = "A=LOAD  UP=SAVE";
        draw_label(fb, PX0 + ((PX1 - PX0) - text_w_est(hint)) / 2, sy + box + 18, hint, MOTE_RGB565(170, 170, 170));
    } else if (cat_idx == TOOLS_TAB) {
        const int swatch = 14, gap = 3, pitch = swatch + gap;
        int n = tab_count[TOOLS_TAB];
        int total_w = n * pitch - gap;
        int start_x = PX0 + (PX1 - PX0 - total_w) / 2;
        int sy = cy0 + 6;
        for (int i = 0; i < n; i++) {
            int x = start_x + i * pitch;
            uint8_t id = tab_items[TOOLS_TAB][i];
            if (i == item_idx) outline_rect(fb, x - 2, sy - 2, x + swatch + 1, sy + swatch + 1, white);
            fill_squircle(fb, x, sy, swatch, tool_color(id), 0);
            if (id == current_tool) put_px(fb, x + swatch / 2, sy + swatch / 2, MOTE_RGB565(255, 255, 255));
        }
        const char *name = tool_name(tab_items[TOOLS_TAB][item_idx]);
        draw_label(fb, PX0 + ((PX1 - PX0) - text_w_est(name)) / 2, sy + swatch + 8, name, white);
    } else {
        const int swatch = 12, gap = 2, pitch = swatch + gap;
        int n = tab_count[cat_idx];
        int total_w = n * pitch - gap;
        int start_x = PX0 + (PX1 - PX0 - total_w) / 2;
        if (start_x < PX0 + 2) start_x = PX0 + 2;
        int sy = cy0 + 6;

        for (int i = 0; i < n; i++) {
            int x = start_x + i * pitch;
            uint8_t id = tab_items[cat_idx][i];
            if (i == item_idx) outline_rect(fb, x - 2, sy - 2, x + swatch + 1, sy + swatch + 1, white);
            fill_squircle(fb, x, sy, swatch, display_color(id), 1);
        }

        const char *name = format_display_name(tab_items[cat_idx][item_idx]);
        draw_label(fb, PX0 + ((PX1 - PX0) - text_w_est(name)) / 2, sy + swatch + 8, name, white);
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = g_render_band,
    .overlay = g_overlay,
    .config = { 0 },
};

static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("Thumboxels", "Prevolve");
MOTE_GAME_VERSION("1.0.0");