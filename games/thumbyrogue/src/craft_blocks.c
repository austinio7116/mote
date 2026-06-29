/*
 * ThumbyCraft — block table + procedural texture atlas.
 *
 * Textures generated at startup so we don't burn flash on baked
 * pixel data. Patterns are intentionally chunky/painterly — the
 * raycaster samples at 16×16 native then any distance gives a
 * smooth Notch-ish look at 128px.
 */
#include "craft_blocks.h"
#include <string.h>
#include <math.h>

/* Atlas storage. In baked mode the bulk lives in flash (.rodata) and
 * the only writable bytes are the two slots that animate_water mutates
 * every frame. */
#ifdef CRAFT_TEXTURES_BAKED
extern const uint16_t craft_textures_baked[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];
/* Two pre-rendered water frames, [frame][face: 0=top,1=side]. The
 * frames are visibly distinct (different jitter + band offset) so the
 * alternation reads as obvious surface motion. animate_water just
 * toggles which one craft_block_texture returns. */
static uint16_t craft_water_frames[2][2 * CRAFT_TEX_PIXELS];
static int      craft_water_frame_idx;
static bool     craft_water_frames_built;
/* Lava uses the same 2-frame animated-tile scheme as water (top + side
 * share one tile each). Toggled at ~2 Hz — lava crawls slower than
 * water. */
static uint16_t craft_lava_frames[2][CRAFT_TEX_PIXELS];
static int      craft_lava_frame_idx;
/* Portal — swirling purple, two animated frames like lava. */
static uint16_t craft_portal_frames[2][CRAFT_TEX_PIXELS];
static int      craft_portal_frame_idx;
#else
uint16_t craft_textures[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];
#endif

/* Map (block, face) → slot index in the atlas. Slots layout:
 *   3 slots per block — 0 = top, 1 = side, 2 = bottom.
 * Most blocks reuse the side texture for all six faces; grass + wood
 * + sand differentiate top/side/bottom. */
static int slot_for(BlockId blk, Face face) {
    int base = blk * 3;
    switch (face) {
        case FACE_PY: return base + 0;     /* top    */
        case FACE_NY: return base + 2;     /* bottom */
        default:      return base + 1;     /* side (all four) */
    }
}

const uint16_t *craft_block_texture(BlockId blk, Face face) {
    /* Map state-variant IDs to the atlas slot of their base. All
     * water levels share BLK_WATER_L0's animated 2-frame slot;
     * redstone _ON variants currently share their _OFF texture
     * (per-state visuals are a follow-up — the IDs are already in
     * place, just the per-variant baking still TBD). */
    BlockId tex_blk = blk;
    if (craft_is_water_id((uint8_t)blk))    tex_blk = BLK_WATER_L0;
    else if (craft_is_lava_id((uint8_t)blk)) tex_blk = BLK_LAVA;
    else if (blk == BLK_OBSERVER_ON)        tex_blk = BLK_OBSERVER;
    else if (blk == BLK_NOTE_BLOCK_ON)      tex_blk = BLK_NOTE_BLOCK;
    else if (blk == BLK_NOT_GATE_ON)        tex_blk = BLK_NOT_GATE;
    else if (blk == BLK_DELAY_ON)           tex_blk = BLK_DELAY;
    else if (blk == BLK_DISPENSER_ON)       tex_blk = BLK_DISPENSER;
    else if (blk == BLK_TARGET_ON)          tex_blk = BLK_TARGET;
    /* Sticky pistons now have their own (green-faced) texture — no longer
     * aliased to the regular piston, so they differ in the inventory bar
     * and held-item viewport. */
    int s = slot_for(tex_blk, face);
#ifdef CRAFT_TEXTURES_BAKED
    if (tex_blk == BLK_WATER_L0) {
        int fr = craft_water_frame_idx & 1;
        if (face == FACE_PY) return &craft_water_frames[fr][0 * CRAFT_TEX_PIXELS];
        if (face != FACE_NY) return &craft_water_frames[fr][1 * CRAFT_TEX_PIXELS];
    }
    if (tex_blk == BLK_LAVA)   return craft_lava_frames[craft_lava_frame_idx & 1];
    if (tex_blk == BLK_PORTAL) return craft_portal_frames[craft_portal_frame_idx & 1];
    return &craft_textures_baked[s * CRAFT_TEX_PIXELS];
#else
    return &craft_textures[s * CRAFT_TEX_PIXELS];
#endif
}

/* Direct atlas-slot accessor (slot 0/1/2). Used by the tall-grass
 * cutout renderer, which packs three sprite variants into a block's
 * three slots. Ordinary baked blocks only — not animated water/lava. */
const uint16_t *craft_block_texture_slot(BlockId blk, int slot) {
#ifdef CRAFT_TEXTURES_BAKED
    return &craft_textures_baked[(blk * 3 + slot) * CRAFT_TEX_PIXELS];
#else
    return &craft_textures[(blk * 3 + slot) * CRAFT_TEX_PIXELS];
#endif
}

const char *craft_block_name(BlockId blk) {
    switch (blk) {
        case BLK_AIR:           return "air";
        case BLK_STONE:         return "stone";
        case BLK_DIRT:          return "dirt";
        case BLK_GRASS:         return "grass";
        case BLK_SAND:          return "sand";
        case BLK_WOOD:          return "wood";
        case BLK_LEAVES:        return "leaves";
        case BLK_WATER_L0:      return "water";
        case BLK_COBBLE:        return "cobble";
        case BLK_PLANK:         return "plank";
        case BLK_GLASS:         return "glass";
        case BLK_COAL_ORE:      return "coal ore";
        case BLK_TORCH:         return "torch";
        case BLK_IRON_ORE:      return "iron ore";
        case BLK_STICK:         return "stick";
        case BLK_IRON_INGOT:    return "iron";
        case BLK_PICKAXE_WOOD:  return "wood pick";
        case BLK_PICKAXE_STONE: return "stone pick";
        case BLK_PICKAXE_IRON:  return "iron pick";
        case BLK_SWORD_WOOD:    return "wood sword";
        case BLK_SWORD_STONE:   return "stone sword";
        case BLK_SWORD_IRON:    return "iron sword";
        case BLK_BOW:           return "bow";
        case BLK_ARROW:         return "arrow";
        case BLK_FURNACE:       return "furnace";
        case BLK_CHEST:         return "chest";
        case BLK_SILVER_ORE:    return "silver ore";
        case BLK_GOLD_ORE:      return "gold ore";
        case BLK_DIAMOND_ORE:   return "diamond ore";
        case BLK_REDSTONE_ORE:  return "redstone ore";
        case BLK_SILVER_INGOT:  return "silver";
        case BLK_GOLD_INGOT:    return "gold";
        case BLK_DIAMOND:       return "diamond";
        case BLK_REDSTONE:      return "redstone";
        case BLK_PICKAXE_SILVER:  return "silver pick";
        case BLK_PICKAXE_GOLD:    return "gold pick";
        case BLK_PICKAXE_DIAMOND: return "diamond pick";
        case BLK_SWORD_SILVER:    return "silver sword";
        case BLK_SWORD_GOLD:      return "gold sword";
        case BLK_SWORD_DIAMOND:   return "diamond sword";
        case BLK_SILVER_BLOCK:    return "silver block";
        case BLK_GOLD_BLOCK:      return "gold block";
        case BLK_DIAMOND_BLOCK:   return "diamond block";
        case BLK_REDSTONE_BLOCK:  return "redstone block";
        case BLK_LEVER_OFF:       return "lever";
        case BLK_LEVER_ON:        return "lever (on)";
        case BLK_REDSTONE_WIRE:
        case BLK_REDSTONE_WIRE_ON: return "wire";
        case BLK_LADDER:        return "ladder";
        case BLK_TRAPDOOR_OFF:
        case BLK_TRAPDOOR_ON:   return "trapdoor";
        case BLK_DOOR_OFF:
        case BLK_DOOR_ON:       return "door";
        case BLK_PRESSURE_PAD:  return "pressure pad";
        case BLK_PISTON_OFF:
        case BLK_PISTON_ON:     return "piston";
        case BLK_PISTON_ARM:    return "piston arm";
        case BLK_STICKY_PISTON_OFF:
        case BLK_STICKY_PISTON_ON: return "sticky piston";
        case BLK_TNT:           return "TNT";
        case BLK_TNT_FUSED:     return "TNT!";
        case BLK_OBSERVER:
        case BLK_OBSERVER_ON:   return "observer";
        case BLK_NOTE_BLOCK:
        case BLK_NOTE_BLOCK_ON: return "note block";
        case BLK_LAMP:
        case BLK_LAMP_ON:       return "lamp";
        case BLK_NOT_GATE:
        case BLK_NOT_GATE_ON:   return "NOT gate";
        case BLK_DELAY:
        case BLK_DELAY_ON:      return "delay";
        case BLK_DISPENSER:
        case BLK_DISPENSER_ON:  return "dispenser";
        case BLK_TARGET:
        case BLK_TARGET_ON:     return "target";
        case BLK_SLIME_BLOCK:   return "slime block";
        case BLK_SLIMEBALL:     return "slimeball";
        case BLK_SNOW:          return "snow";
        case BLK_SNOWY_ROCK:    return "snowy rock";
        case BLK_ICE:           return "ice";
        case BLK_LAVA:          return "lava";
        case BLK_LAVA_L1:
        case BLK_LAVA_L2:
        case BLK_LAVA_L3:       return "lava";
        case BLK_OBSIDIAN:      return "obsidian";
        case BLK_GRAVEL:        return "gravel";
        case BLK_FLINT:         return "flint";
        case BLK_PORTAL:        return "portal";
        case BLK_SANDSTONE:     return "sandstone";
        case BLK_CACTUS:        return "cactus";
        case BLK_VINE:          return "vine";
        case BLK_LILY_PAD:      return "lily pad";
        case BLK_WATER_L1:
        case BLK_WATER_L2:
        case BLK_WATER_L3:
        case BLK_WATER_L4:
        case BLK_WATER_L5:
        case BLK_WATER_L6:
        case BLK_WATER_L7:      return "water";
        case BLK_BOOKCASE:      return "bookcase";
        case BLK_BARREL:        return "barrel";
        case BLK_CRATE:         return "crate";
        case BLK_SARCOPHAGUS:   return "sarcophagus";
        case BLK_CRYSTAL:       return "crystal";
        case BLK_BONES:         return "bones";
        case BLK_RUBBLE:        return "rubble";
        case BLK_SHARDS:        return "shards";
        case BLK_FUNGI:         return "fungi";
        case BLK_COBWEB:        return "cobweb";
        case BLK_BARRIER:       return "barrier";
        case BLK_RIVERBED:      return "riverbed";
        case BLK_SHOPCOUNTER:   return "shop counter";
        case BLK_SHOPSHELF:     return "shop shelf";
        default:                return "?";
    }
}

/* xorshift32 with a deterministic seed so each (block, face, pixel)
 * gets the same colour every boot — required for the renderer to
 * not shimmer between frames. */
static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

static void fill_solid(uint16_t *dst, uint16_t c) {
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) dst[i] = c;
}

/* Add ±jitter to each channel, clamped, write back. */
static void speckle(uint16_t *dst, uint32_t seed, int r, int g, int b, int jit) {
    uint32_t s = seed;
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) {
        int j = ((int)(xs32(&s) & 0xff) - 128) * jit / 128;
        dst[i] = rgb565(r + j, g + j, b + j);
    }
}

/* ThumbyRogue dungeon flagstone: mortared slabs with a soft top-left bevel,
 * per-slab tone variation and the odd crack — clean at iso distance. `gsize`
 * sets slab size and `tone` shifts brightness, so several variants can be
 * scattered across the floor for tessellating variety. */
static void flagstone_pattern(uint16_t *dst, uint32_t seed, int gsize, int tone) {
    uint32_t s = seed;
    int half = gsize / 2;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int lx = x % gsize, ly = y % gsize;
            int sxx = x / gsize, syy = y / gsize;
            uint32_t sh = (uint32_t)(sxx * 73856093) ^ (uint32_t)(syy * 19349663) ^ seed;
            int slabtone = (int)(sh % 22u) - 11;           /* per-slab shade */
            int c;
            if (lx == 0 || ly == 0) {                      /* recessed mortar */
                c = 54 + (int)(xs32(&s) & 7);
            } else {
                c = 118 + tone + slabtone;
                c += (half - lx) + (half - ly);            /* top-left bevel */
                if (lx == gsize - 1 || ly == gsize - 1) c -= 12;
                c += ((int)(xs32(&s) & 7) - 3);            /* faint grain */
                if (((sh >> (lx & 7)) & 0x3Fu) == 0u) c -= 34;  /* occasional crack */
            }
            if (c < 0) c = 0;
            if (c > 205) c = 205;
            int b = c - 4; if (b < 0) b = 0;
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c + 6, c + 2, b);  /* faintly warm */
        }
    }
}

/* --- ThumbyRogue band blocks ------------------------------------- */

/* Caverns wall: warm tan rugged rock with dark pits + the odd ochre
 * mineral fleck — gives the caverns an earthy, NON-grey feel. */
static void cave_rock_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++)
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            uint32_t h = (uint32_t)((x+1)*73856093) ^ (uint32_t)((y+1)*19349663) ^ seed;
            h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            int r = 150 + j, g = 124 + j, b = 92 + j;               /* tan base */
            if ((h & 7u) == 0u)        { r -= 54; g -= 48; b -= 40; }   /* dark pit */
            else if ((h % 29u) == 0u)  { r = 188; g = 140; b = 52; }    /* ochre fleck */
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(r, g, b);
        }
}

/* Fungal floor: a luminous JADE moss mat — bright and fairly uniform so the
 * hero, enemies and drops read clearly (the old dark purple was muddy and
 * unplayable). Soft lighter patches give it life; a few bright spore nodes
 * glow without cluttering. */
static void mycelium_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++)
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            /* Large 4x4 soft patches (block-level, not per-pixel) keep the mat
             * calm and uniform instead of speckly. */
            uint32_t cm = (uint32_t)(((x >> 2) + 1) * 26597) ^ (uint32_t)(((y >> 2) + 1) * 53401) ^ seed;
            cm ^= cm >> 13; cm *= 0x9E3779B1u; cm ^= cm >> 16;
            int patch = ((cm & 3u) == 0u) ? -10 : ((cm & 3u) == 1u) ? 8 : 0;
            int j = ((int)(xs32(&s) & 0x7) - 4);                    /* very gentle grain */
            int r = 44 + j + patch, g = 100 + j + patch, b = 80 + j + patch; /* calm jade */
            /* sparse, deliberate bright spore nodes — the only high-contrast bits */
            uint32_t h = (uint32_t)((x+3)*40503) ^ (uint32_t)((y+7)*12289) ^ seed;
            h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
            if ((h % 73u) == 0u) { r = 150; g = 245; b = 205; }
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(r, g, b);
        }
}

/* Fungal wall: a solid, organic mat of CONNECTED VINES — winding tube-shaded
 * tendrils (3 vertical + 2 horizontal, sinusoidal so the 16px tile loops
 * seamlessly) covering a dark loamy base, knotted where they cross, with a
 * few glowing pods. Reads as living overgrowth rather than glowy rock. */
static void fungal_wall_pattern(uint16_t *dst, uint32_t seed) {
    const float TAU = 6.2831853f;
    float ps = (float)(seed & 7) * 0.4f;          /* per-band phase jitter */
    for (int y = 0; y < CRAFT_TEX_SIZE; y++)
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            float fx = (float)x, fy = (float)y;
            /* nearest distance to any vine strand (with toroidal wrap) */
            float best = 99.0f;
            for (int i = 0; i < 3; i++) {                          /* vertical vines */
                float cx = (2.5f + i * 5.0f) + 3.0f * sinf(fy * TAU / 16.0f + i * 2.1f + ps);
                float d = fabsf(fx - cx); if (d > 8.0f) d = 16.0f - d;
                if (d < best) best = d;
            }
            for (int i = 0; i < 2; i++) {                          /* horizontal vines */
                float cy = (4.0f + i * 8.0f) + 2.5f * sinf(fx * TAU / 16.0f + i * 1.7f + ps);
                float d = fabsf(fy - cy); if (d > 8.0f) d = 16.0f - d;
                if (d < best) best = d;
            }
            int r, g, b;
            if (best < 2.3f) {                                     /* on a vine — tube shade */
                float t = best / 2.3f;                             /* 0 core .. 1 rim */
                r = 46 - (int)(t * 22);
                g = 168 - (int)(t * 96);                           /* bright green core → dark rim */
                b = 74 - (int)(t * 34);
                uint32_t h = (uint32_t)((x+5)*83492791) ^ (uint32_t)((y+2)*19349663) ^ seed;
                h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
                if (best < 1.0f && (h % 23u) == 0u) { r = 130; g = 245; b = 205; } /* glowing pod */
            } else {                                               /* dark loam between vines */
                uint32_t cm = (uint32_t)(((x >> 2) + 1) * 26597) ^ (uint32_t)(((y >> 2) + 1) * 53401) ^ seed;
                cm ^= cm >> 13; cm *= 0x9E3779B1u; cm ^= cm >> 16;
                int patch = ((cm & 3u) == 0u) ? -6 : ((cm & 3u) == 1u) ? 5 : 0;
                r = 26 + patch; g = 40 + patch; b = 32 + patch;
            }
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(r, g, b);
        }
}

/* Fungal pillar: a pale, luminous toadstool stalk — cool blue-white with
 * soft vertical fibres and a few glowing teal pores. Bright so the pillars
 * stand out against the dark spore-colony walls. */
static void mushroom_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++)
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int fibre = ((x & 3) == 0) ? -16 : 0;                   /* vertical fibres */
            int j = ((int)(xs32(&s) & 0xf) - 8);
            int r = 198 + j + fibre, g = 220 + j + fibre, b = 222 + j + fibre; /* pale blue-white */
            uint32_t h = (uint32_t)((x+1)*40503) ^ (uint32_t)((y+1)*12289) ^ seed;
            if ((h % 29u) == 0u) { r = 110; g = 235; b = 210; }     /* glowing teal pore */
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(r, g, b);
        }
}

/* --- ThumbyRogue room scenery -----------------------------------------
 * Each takes a slot (0=top, 1=side, 2=bottom) so faces can differ — a
 * bookcase shows shelves on the sides but planks on top, a barrel shows
 * a lid on top but staves on the side, and so on. */
#define S CRAFT_TEX_SIZE

/* Bookcase: dark wood frame; sides carry horizontal shelves packed with
 * varied book spines, top/bottom are plain planks. */
static void bookcase_pattern(uint16_t *dst, int slot, uint32_t seed) {
    if (slot != 1) {                       /* top / bottom — plank grain */
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int band = (y >> 2) & 1;
                int c = band ? 96 : 84;
                uint32_t h = (uint32_t)((x+1)*92821) ^ (uint32_t)((y+1)*68917) ^ seed;
                c += (int)(h % 11u) - 5;
                if ((y & 3) == 0) c -= 22;
                dst[y * S + x] = rgb565(c, (c * 11) >> 4, (c * 6) >> 4);
            }
        return;
    }
    for (int y = 0; y < S; y++) {
        int shelf = (y % 5 == 0) || y == S - 1;        /* shelf plank rows */
        for (int x = 0; x < S; x++) {
            int frame = (x == 0 || x == S - 1);
            if (shelf || frame) {                       /* dark wood frame */
                int c = 60 - ((y & 1) ? 0 : 6);
                dst[y * S + x] = rgb565(c, (c * 10) >> 4, (c * 5) >> 4);
                continue;
            }
            /* a book: colour keyed to its (column,shelf) so spines vary */
            uint32_t bk = (uint32_t)((x / 1) * 2654435761u) ^
                          (uint32_t)((y / 5) * 40503u) ^ seed;
            bk ^= bk >> 13; bk *= 0x9E3779B1u; bk ^= bk >> 16;
            int hue = (int)(bk % 6u);
            int sh  = 150 + (int)((bk >> 8) % 60u);     /* per-book brightness */
            int r, g, b;
            switch (hue) {
                case 0: r = sh;        g = sh*2/5;     b = sh*2/5;     break; /* red */
                case 1: r = sh*2/5;    g = sh*2/3;     b = sh;         break; /* blue */
                case 2: r = sh*2/5;    g = sh;         b = sh*2/5;     break; /* green */
                case 3: r = sh;        g = sh*3/4;     b = sh*2/5;     break; /* gold */
                case 4: r = sh*3/4;    g = sh*2/5;     b = sh;         break; /* purple */
                default:r = sh*3/4;    g = sh*3/4;     b = sh*3/4;     break; /* tan */
            }
            if (x == S - 2) { r = r*3/4; g = g*3/4; b = b*3/4; }   /* edge shade */
            dst[y * S + x] = rgb565(r, g, b);
        }
    }
}

/* Shop counter: rich polished planks on top with a gold inlay rim (the
 * trading surface), panelled dark-wood front with a brass stud line. */
static void shop_counter_pattern(uint16_t *dst, int slot, uint32_t seed) {
    if (slot == 0) {                       /* top -- polished planks, gold rim */
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                if (x == 0 || x == S-1 || y == 0 || y == S-1) {     /* dark edge */
                    dst[y * S + x] = rgb565(58, 38, 20);
                    continue;
                }
                if (x == 1 || x == S-2 || y == 1 || y == S-2) {     /* gold inlay */
                    int g = ((x + y) & 1) ? 0 : 14;                 /* subtle sparkle */
                    dst[y * S + x] = rgb565(208 + g, 168 + g, 56);
                    continue;
                }
                int band = (y >> 2) & 1;                            /* plank bands */
                int cb = band ? 132 : 116;
                uint32_t h = (uint32_t)((x+1)*92821) ^ (uint32_t)((y+1)*68917) ^ seed;
                cb += (int)(h % 9u) - 4;
                if ((y & 3) == 0) cb -= 18;                         /* plank seam */
                if (((x * 7 + y * 3) % 29) == 0) cb += 26;          /* polish glint */
                dst[y * S + x] = rgb565(cb + 28, (cb * 10) >> 4, (cb * 4) >> 4);
            }
        return;
    }
    if (slot == 2) {                       /* bottom -- plain dark wood */
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int cb = 64 + (((x ^ y) & 3) ? 0 : 8);
                dst[y * S + x] = rgb565(cb, (cb * 10) >> 4, (cb * 5) >> 4);
            }
        return;
    }
    /* side -- counter lip on top, raised door-style panels below */
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (y < 3) {                                            /* lip matches top */
                int cb = 124 + ((y == 2) ? -20 : 0);
                dst[y * S + x] = rgb565(cb + 28, (cb * 10) >> 4, (cb * 4) >> 4);
                continue;
            }
            if (y == 3) {                                           /* brass stud line */
                dst[y * S + x] = ((x & 3) == 1) ? rgb565(212, 172, 64)
                                                : rgb565(70, 46, 24);
                continue;
            }
            /* two raised panels per face */
            int px = (x < 8) ? x : x - 8;                           /* panel-local 0..7 */
            int border = (px == 0 || px == 7 || y == 4 || y == S-1);
            int cb = border ? 58 : 92;
            if (!border && (px == 1 || y == 5)) cb = 104;           /* bevel highlight */
            if (!border && (px == 6 || y == S-2)) cb = 76;          /* bevel shadow */
            uint32_t h = (uint32_t)((x+3)*92821) ^ (uint32_t)((y+5)*68917) ^ seed;
            cb += (int)(h % 7u) - 3;
            dst[y * S + x] = rgb565(cb + 14, (cb * 10) >> 4, (cb * 5) >> 4);
        }
}

/* Shop shelf: dark wood frame packed with wares -- potion bottles with
 * bright caps, coin pouches and small boxes -- so the wall behind the
 * merchant reads instantly as a stocked store. Top/bottom are planks. */
static void shop_shelf_pattern(uint16_t *dst, int slot, uint32_t seed) {
    if (slot != 1) {                       /* top / bottom -- plank grain */
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int band = (y >> 2) & 1;
                int cb = band ? 92 : 80;
                uint32_t h = (uint32_t)((x+1)*92821) ^ (uint32_t)((y+1)*68917) ^ seed;
                cb += (int)(h % 11u) - 5;
                if ((y & 3) == 0) cb -= 20;
                dst[y * S + x] = rgb565(cb, (cb * 11) >> 4, (cb * 6) >> 4);
            }
        return;
    }
    for (int y = 0; y < S; y++) {
        int shelf = (y % 5 == 0) || y == S - 1;        /* shelf plank rows */
        for (int x = 0; x < S; x++) {
            int frame = (x == 0 || x == S - 1);
            if (shelf || frame) {                       /* dark wood frame */
                int cb = 64 - ((y & 1) ? 0 : 6);
                dst[y * S + x] = rgb565(cb, (cb * 10) >> 4, (cb * 5) >> 4);
                continue;
            }
            /* ware slots: 3 shelves x ~4 wares; each ware keyed to its
             * (slot, shelf) so the stock varies cube to cube */
            int sy = y / 5;                             /* shelf index 0..2 */
            int ly = y % 5;                             /* row inside shelf 1..4 */
            int wx = (x - 1) / 4;                       /* ware slot 0..3 */
            int lx = (x - 1) % 4;                       /* col inside slot 0..3 */
            uint32_t wk = (uint32_t)((wx + 1) * 2654435761u) ^
                          (uint32_t)((sy + 1) * 40503u) ^ seed;
            wk ^= wk >> 13; wk *= 0x9E3779B1u; wk ^= wk >> 16;
            int kind = (int)(wk % 5u);
            int r = 24, g = 18, b = 14;                 /* dark alcove behind wares */
            if (kind <= 1) {
                /* potion bottle: 2px-wide body rows 2..4, bright cap row 1 */
                int hue = (int)((wk >> 8) % 4u);
                int body = (lx == 1 || lx == 2);
                if (body && ly >= 2) {
                    int sh = 150 + (int)((wk >> 16) % 50u);
                    switch (hue) {
                        case 0:  r = sh;      g = sh/4;    b = sh/4;   break; /* red */
                        case 1:  r = sh/4;    g = sh/2;    b = sh;     break; /* blue */
                        case 2:  r = sh/4;    g = sh;      b = sh/3;   break; /* green */
                        default: r = sh*3/4;  g = sh/4;    b = sh;     break; /* purple */
                    }
                    if (lx == 1 && ly == 2) { r += 60; g += 60; b += 60; } /* glint */
                } else if (body && ly == 1) {
                    r = 200; g = 196; b = 188;          /* stopper cap */
                }
            } else if (kind == 2) {
                /* coin pouch: tan mound rows 2..4, gold tie row 1 */
                int mound = (ly >= 2) && (lx >= (ly == 2 ? 1 : 0)) && (lx <= (ly == 2 ? 2 : 3));
                if (mound) { r = 168; g = 124; b = 62; }
                if (ly == 1 && (lx == 1 || lx == 2)) { r = 224; g = 184; b = 60; }
            } else if (kind == 3) {
                /* small box with a strap */
                if (ly >= 1) {
                    r = 120; g = 84; b = 44;
                    if (lx == 1 || lx == 2) { r = 88; g = 60; b = 30; }   /* strap */
                    if (ly == 1) { r += 22; g += 16; b += 8; }            /* lid */
                }
            }
            /* kind 4: empty slot -- bare alcove */
            dst[y * S + x] = rgb565(r, g, b);
        }
    }
}

/* Barrel: top is a planked lid (concentric); sides are vertical staves
 * with two dark iron hoops. */
static void barrel_pattern(uint16_t *dst, int slot, uint32_t seed) {
    if (slot == 0) {                       /* lid — concentric planks */
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int dx = x - 8, dy = y - 8;
                int rr = dx*dx + dy*dy;
                int ring = (rr / 14) & 1;
                int c = ring ? 120 : 104;
                if (rr > 60) c -= 28;                  /* dark rim hoop */
                uint32_t h = (uint32_t)((x+3)*92821) ^ (uint32_t)((y+5)*68917) ^ seed;
                c += (int)(h % 9u) - 4;
                dst[y * S + x] = rgb565(c + 24, (c * 11) >> 4, (c * 6) >> 4);
            }
        return;
    }
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int stave = (x >> 1) & 1;                  /* alternating staves */
            int c = stave ? 128 : 110;
            int edge = (x % 4 == 0);                    /* stave seam */
            if (edge) c -= 30;
            if (y == 2 || y == 3 || y == 12 || y == 13) /* two iron hoops */
                { c = 70; dst[y * S + x] = rgb565(c, c, c + 6); continue; }
            uint32_t h = (uint32_t)((x+1)*92821) ^ (uint32_t)((y+7)*68917) ^ seed;
            c += (int)(h % 11u) - 5;
            dst[y * S + x] = rgb565(c + 30, (c * 11) >> 4, (c * 5) >> 4);
        }
}

/* Crate: planks with a lighter outer frame and corner nails (same on
 * every face). */
static void crate_pattern(uint16_t *dst, int slot, uint32_t seed) {
    (void)slot;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int frame = (x < 2 || x >= S - 2 || y < 2 || y >= S - 2);
            int band  = (y >> 2) & 1;
            int c = band ? 138 : 120;
            if (frame) c += 24;                         /* lighter border beam */
            uint32_t h = (uint32_t)((x+1)*92821) ^ (uint32_t)((y+1)*68917) ^ seed;
            c += (int)(h % 11u) - 5;
            if ((y & 3) == 0 && !frame) c -= 18;        /* plank seams */
            /* corner nails */
            if (((x == 2 || x == S - 3) && (y == 2 || y == S - 3))) c = 80;
            dst[y * S + x] = rgb565(c, (c * 12) >> 4, (c * 7) >> 4);
        }
}

/* Sarcophagus: top is a carved effigy lid (a pale figure down the
 * centre on stone); sides are recessed stone panels. */
static void sarcophagus_pattern(uint16_t *dst, int slot, uint32_t seed) {
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            uint32_t h = (uint32_t)((x+2)*73856093) ^ (uint32_t)((y+9)*19349663) ^ seed;
            h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
            int c = 120 + (int)(h % 17u) - 8;          /* mottled stone */
            if (slot == 0) {
                /* Coffin lid: an elongated hexagon outline (the classic
                 * tapered-shoulder casket), a carved head disc up top and a
                 * folded-arms cross on the chest — reads clearly top-down. */
                int dx = x - 8;
                int adx = dx < 0 ? -dx : dx;
                /* hexagon half-width: widest at the shoulders (y~5), tapering
                 * to the head (top) and the foot (bottom). */
                int hw;
                if (y < 4)      hw = 2 + y;             /* head taper */
                else if (y < 7) hw = 6;                 /* shoulders */
                else            hw = 6 - (y - 7) / 2;   /* foot taper */
                int inside = (y >= 1 && y <= 14 && adx <= hw);
                int edge   = inside && (adx >= hw - 1 || y == 1 || y == 14);
                if (!inside) { c -= 30; }               /* recessed margin */
                else if (edge) { c = 150 + (int)(h % 9u); }   /* raised lid rim */
                else {
                    c = 96 + (int)(h % 8u);             /* sunken lid panel */
                    int head = (y >= 2 && y <= 5 && dx*dx + (y-4)*(y-4) <= 4);
                    int armV = (y >= 7 && y <= 12 && adx <= 1);          /* body line */
                    int armH = (y == 8 && adx <= 3);                     /* folded arms */
                    if (head || armV || armH) c = 190 + (int)(h % 12u);  /* pale effigy */
                }
            } else {
                int border = (x < 2 || x >= S - 2 || y < 2 || y >= S - 2);
                if (border) c -= 30;                    /* recessed panel */
            }
            dst[y * S + x] = rgb565(c, c, c + 6);       /* faintly cool */
        }
}

/* Crystal cluster: bright faceted gem (cyan→violet), luminous since it
 * emits light. Diagonal facet banding + a few specular sparkles. */
static void crystal_pattern(uint16_t *dst, int slot, uint32_t seed) {
    (void)slot;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int facet = ((x + y) >> 2) & 1;             /* diagonal facets */
            int facet2 = ((x - y + 16) >> 2) & 1;
            uint32_t h = (uint32_t)((x+1)*92821) ^ (uint32_t)((y+1)*68917) ^ seed;
            h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
            int base = facet ? 210 : 150;
            base += facet2 ? 22 : -22;
            base += (int)(h % 13u) - 6;
            int r = base * 2 / 5, g = base * 4 / 5, b = base;   /* cyan-violet */
            if ((h % 23u) == 0u) { r = 255; g = 255; b = 255; }  /* sparkle */
            /* dark seams between cluster shards */
            if (((x + 2*y) % 7) == 0) { r = r/2; g = g/2; b = b/2; }
            dst[y * S + x] = rgb565(r, g, b);
        }
}

/* --- 2D cross-sprite scenery -------------------------------------------
 * Filled magenta (transparent) then a small ground-hugging silhouette is
 * stamped in the lower rows; the DDA cutout path traces through magenta.
 * tv runs 0 (cell top) .. 15 (cell bottom) so floor clutter lives high in
 * tv (bottom of the cell). Same art on every face. */
#define MAG rgb565(255, 0, 255)
static inline void spr_clear(uint16_t *dst) {
    for (int i = 0; i < S * S; i++) dst[i] = MAG;
}
static inline void spr_disc(uint16_t *dst, int cx, int cy, int rr, uint16_t c) {
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx*dx + dy*dy <= rr) dst[y * S + x] = c;
        }
}

/* Scattered bones: a small skull + two crossed long bones, ivory. */
static void bones_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;
    spr_clear(dst);
    uint16_t bone = rgb565(224, 218, 196), shade = rgb565(168, 160, 140);
    /* two crossed long bones (diagonals with knobbed ends) */
    for (int i = -4; i <= 4; i++) {
        int x1 = 8 + i, y1 = 11 - i;     /* "/" bone */
        int x2 = 8 + i, y2 = 11 + i;     /* "\" bone */
        if (x1 >= 0 && x1 < S && y1 >= 6 && y1 < S) dst[y1 * S + x1] = bone;
        if (x2 >= 0 && x2 < S && y2 >= 6 && y2 < S) dst[y2 * S + x2] = bone;
    }
    spr_disc(dst, 3, 7,  1, bone); spr_disc(dst, 13, 7,  1, bone); /* bone knobs */
    spr_disc(dst, 3, 15, 1, bone); spr_disc(dst, 13, 15, 1, bone);
    /* skull, bottom-left */
    spr_disc(dst, 6, 13, 5, bone);
    dst[12 * S + 5] = rgb565(40, 36, 30);      /* eye sockets */
    dst[12 * S + 8] = rgb565(40, 36, 30);
    dst[14 * S + 6] = shade; dst[14 * S + 7] = shade;  /* jaw shadow */
}

/* Broken rock debris: a couple of overlapping grey boulders, bottom. */
static void rubble_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    spr_clear(dst);
    int cx[3] = { 5, 10, 8 }, cy[3] = { 13, 12, 14 }, rd[3] = { 9, 7, 5 };
    for (int k = 0; k < 3; k++)
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int dx = x - cx[k], dy = y - cy[k];
                if (dx*dx + dy*dy > rd[k]) continue;
                int j = (int)(xs32(&s) & 0xf) - 8;
                int base = 116 + j - (k * 6);          /* each boulder a shade */
                if (dy <= -1) base += 22;              /* top-lit */
                dst[y * S + x] = rgb565(base, base, base + 4);
            }
}

/* Small crystal shards: a few bright spikes rising from the floor. */
static void shards_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;
    spr_clear(dst);
    /* three spikes: (apex_x, apex_y, base_y, half-width-at-base) */
    int ax[3] = { 5, 8, 11 }, ay[3] = { 6, 3, 7 }, hw[3] = { 2, 2, 2 };
    for (int k = 0; k < 3; k++)
        for (int y = ay[k]; y < S; y++) {
            float f = (float)(y - ay[k]) / (float)(S - 1 - ay[k]);
            int w = (int)(hw[k] * f + 0.5f);
            for (int dx = -w; dx <= w; dx++) {
                int x = ax[k] + dx;
                if (x < 0 || x >= S) continue;
                int edge = (dx == -w || dx == w);
                int tip  = (y <= ay[k] + 1);
                uint16_t c = tip ? rgb565(235, 248, 255)
                           : edge ? rgb565(40, 120, 180)
                                  : rgb565(90, 190, 235);
                dst[y * S + x] = c;
            }
        }
}

/* Little mushroom cluster: cream stalks + warm caps. */
static void fungi_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;
    spr_clear(dst);
    uint16_t stalk = rgb565(222, 212, 184);
    int mx[3] = { 4, 9, 12 }, capw[3] = { 3, 4, 2 }, capy[3] = { 9, 6, 11 };
    uint16_t cap[3] = { rgb565(200, 70, 70), rgb565(210, 120, 60), rgb565(170, 90, 190) };
    for (int k = 0; k < 3; k++) {
        for (int y = capy[k] + 1; y < S; y++)              /* stalk */
            { if (mx[k] >= 0 && mx[k] < S) dst[y * S + mx[k]] = stalk; }
        for (int dx = -capw[k]; dx <= capw[k]; dx++) {     /* dome cap */
            int x = mx[k] + dx; if (x < 0 || x >= S) continue;
            int top = capy[k] - (capw[k] - (dx < 0 ? -dx : dx));
            for (int y = top; y <= capy[k]; y++)
                if (y >= 0) dst[y * S + x] = cap[k];
        }
    }
}

/* Riverbed: rounded pebbles for the floor of water pools — overlapping stone
 * discs in grey / brown / slate tones on dark silt, top-lit, wrapped so the
 * tile is seamless. Bright enough to read through the water blend. */
static void riverbed_pattern(uint16_t *dst, int slot, uint32_t seed) {
    (void)slot;
    uint32_t s = seed;
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) {           /* dark silt base */
        int j = (int)(xs32(&s) & 0x7) - 4;
        dst[i] = rgb565(40 + j, 36 + j, 32 + j);
    }
    static const int px[9] = { 2, 7, 12, 4, 10, 15, 1, 8, 13 };
    static const int py[9] = { 2, 1, 3, 7, 6, 8, 12, 11, 13 };
    for (int k = 0; k < 9; k++) {
        uint32_t h = (uint32_t)((k + 1) * 2654435761u) ^ seed;
        h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
        int rr = 2 + (int)(h % 2u);                        /* radius 2..3 */
        int br = 108, bg = 102, bb = 96;                   /* grey */
        if ((h % 3u) == 1u)      { br = 116; bg = 96; bb = 74; }   /* brown */
        else if ((h % 3u) == 2u) { br = 88;  bg = 96; bb = 106; }  /* slate */
        for (int dy = -rr; dy <= rr; dy++)
            for (int dx = -rr; dx <= rr; dx++) {
                if (dx*dx + dy*dy > rr*rr) continue;
                int x = (px[k] + dx + 16) & 15, y = (py[k] + dy + 16) & 15;
                int lt = (dy < 0) ? 16 : (dy > 0 ? -12 : 0);   /* top-lit dome */
                dst[y * CRAFT_TEX_SIZE + x] = rgb565(br + lt, bg + lt, bb + lt);
            }
    }
}

/* Corner cobweb: faint threads anchored in the top-left corner, radiating
 * down/right and braced by a couple of quarter-ring arcs. Only the corner
 * region is webbed; the rest of the cell stays transparent. */
static void cobweb_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;
    spr_clear(dst);
    uint16_t web = rgb565(208, 208, 218);
    const int R = 11;                 /* web only reaches ~11px from the corner */
    /* four spokes fanning out from (0,0) */
    for (int t = 0; t <= R; t++) {
        dst[t * S + t]               = web;          /* 45° */
        if (t / 2 < S) dst[(t / 2) * S + t] = web;   /* shallow (toward +x) */
        if (t / 2 < S) dst[t * S + (t / 2)] = web;   /* steep  (toward +y) */
        if (t == 0) dst[0] = web;
    }
    dst[0] = web; for (int x = 0; x <= R; x++) dst[x] = web;        /* top edge anchor */
    for (int y = 0; y <= R; y++) dst[y * S] = web;                 /* left edge anchor */
    /* two quarter-ring arcs catching the spokes */
    int rings[2] = { 5, 9 };
    for (int k = 0; k < 2; k++) {
        int rr = rings[k];
        for (int x = 0; x <= rr; x++) {
            int y = (int)(0.5f + sqrtf((float)(rr*rr - x*x)));
            if (y >= 0 && y < S && x < S) dst[y * S + x] = web;
        }
    }
}
#undef MAG
#undef S

/* Brick-like mortar pattern at the grid lines — used by cobble. */
static void cobble_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int rx = x % 8, ry = y % 4;
            int row = (y / 4) & 1;
            if (row) rx = (x + 4) % 8;
            int border = (ry == 0 || rx == 0);
            int base = border ? 70 : 130;
            int j = ((int)(xs32(&s) & 0x3f) - 32);
            int c = base + j;
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c, c, c);
        }
    }
}

/* Horizontal plank stripes. */
static void plank_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        int band = y / 4;
        int base_r = 160 - band * 8;
        int base_g = 110 - band * 6;
        int base_b = 70  - band * 4;
        int edge = (y % 4 == 0);
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            int c_r = base_r + j - (edge ? 30 : 0);
            int c_g = base_g + j - (edge ? 25 : 0);
            int c_b = base_b + j - (edge ? 20 : 0);
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c_r, c_g, c_b);
        }
    }
}

/* Vertical wood-grain rings. */
static void wood_side_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int xx = x - 8;
            int ring = (xx * xx);
            int base_r = 110 + (ring & 0x1f);
            int base_g = 70  + (ring & 0xf);
            int base_b = 35;
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            dst[y * CRAFT_TEX_SIZE + x] =
                rgb565(base_r + j, base_g + j / 2, base_b + j / 4);
        }
    }
}

/* Water — striped pattern that suggests gentle wave bands. */
static void water_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        int band = (y / 2) & 1 ? 20 : 0;
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            dst[y * CRAFT_TEX_SIZE + x] =
                rgb565(30 + j / 2, 90 + j / 2, 180 + band + j / 2);
        }
    }
}

/* Bake one water frame into out_top + out_side. Variant 0 / 1 produce
 * two visibly distinct patterns (different band offset + different
 * jitter seed) that alternate to give clear surface motion. */
static void bake_water_frame(uint16_t *out_top, uint16_t *out_side, int variant) {
    /* Murky teal-blue dungeon water (the old version was stripy and far too
     * green). Two seamless diagonal ripple waves — integer wave-vectors over
     * the 16px tile so it wraps — drift between the two animation frames,
     * plus sparse moving glints. Reads as slow, deep water. */
    uint32_t s = 0xC0FFEEu ^ (variant ? 0x9E3779B9u : 0u);
    const float TAU = 6.2831853f;
    float ph = variant ? 1.6f : 0.0f;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            float w1 = sinf((float)(x + 2*y) * (TAU / 16.0f) + ph);
            float w2 = sinf((float)(2*x - y) * (TAU / 16.0f) - ph * 0.7f);
            int lift = (int)(w1 * 8.0f + w2 * 5.0f);
            int j = ((int)(xs32(&s) & 0xf) - 8) / 2;
            int r = 13 + lift / 3 + j;
            int g = 46 + lift + j;
            int b = 68 + lift + j;
            uint32_t h = (uint32_t)((x + 1 + variant * 7) * 92821) ^ (uint32_t)((y + 3) * 68917);
            h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
            if ((h % 67u) == 0u) { r += 55; g += 65; b += 65; }   /* glint */
            uint16_t c = rgb565(r, g, b);
            out_side[y * CRAFT_TEX_SIZE + x] = c;
            out_top [y * CRAFT_TEX_SIZE + x] = c;
        }
    }
}

/* Molten lava tile — cracked basalt crust: dark cooled plates split by
 * a glowing network of cracks (Voronoi cell boundaries). High contrast
 * (near-black crust vs white-hot cracks), connected crack lines rather
 * than stripes or scattered spots. Seeds wrap across the 16px edge so
 * the pattern tiles seamlessly between adjacent lava cells. The variant
 * nudges seed positions + crack glow so the cracks shimmer/crawl
 * between the two animation frames. One tile serves every face. */
static void bake_lava_frame(uint16_t *out, int variant) {
    enum { LAVA_SEEDS = 13 };
    float sx[LAVA_SEEDS], sy[LAVA_SEEDS];
    uint32_t s = 0x1A1AF0u ^ (variant ? 0x9E3779B9u : 0u);
    for (int i = 0; i < LAVA_SEEDS; i++) {
        sx[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
        sy[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
    }
    const float F = 6.2831853f / 16.0f;   /* base freq — periodic over 16px */
    float ph   = variant ? 1.7f : 0.0f;   /* frame phase → the swirl crawls */
    int   glow = variant ? 12 : 0;        /* crack brightness pulse */
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            /* Domain warp — two octaves of sinusoidal swirl. All terms
             * are periodic over 16px, so the tile still wraps seamlessly
             * between adjacent lava cells, but the Voronoi boundaries
             * bend into organic, varied curves instead of straight
             * polygon edges. */
            float wx = x + 3.2f * sinf(F * 2.0f * y + ph)
                         + 1.7f * sinf(F * 3.0f * y - ph * 0.5f);
            float wy = y + 3.2f * cosf(F * 2.0f * x - ph)
                         + 1.7f * cosf(F * 3.0f * x + ph * 0.5f);
            float d1 = 1e9f, d2 = 1e9f;   /* nearest + second-nearest seed */
            for (int i = 0; i < LAVA_SEEDS; i++) {
                float dx = wx - sx[i], dy = wy - sy[i];
                dx -= 16.0f * floorf(dx / 16.0f + 0.5f);   /* toroidal wrap */
                dy -= 16.0f * floorf(dy / 16.0f + 0.5f);
                float dd = dx * dx + dy * dy;
                if (dd < d1)      { d2 = d1; d1 = dd; }
                else if (dd < d2) { d2 = dd; }
            }
            float edge = d2 - d1;         /* small → on a plate boundary */
            /* Low-frequency "temperature" field — some patches of crust
             * glow warmer than others, breaking up flat dark plates. */
            float warm = 0.5f + 0.5f * sinf(F * (x + y) + ph * 1.3f);
            uint32_t n = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663);
            int j = (int)(n & 7);         /* faint per-pixel break-up */
            int rr, gg, bb;
            if (edge < 16.0f) {           /* glowing crack */
                int hot = (int)(16.0f - edge);   /* 0..16, hottest at seam */
                rr = 188 + hot * 4 + glow;       /* → ~255 white-hot */
                gg = 55  + hot * 9 + glow;       /* → ~200 */
                bb = 8   + hot * 4;              /* → ~72 */
            } else {                      /* dark cooled crust */
                int crust = d1 > 26.0f ? 26 : (int)d1;
                rr = 28 + (int)(warm * 26.0f) + crust / 2 + j;  /* ~28..70 */
                gg = 6  + (int)(warm * 8.0f)  + crust / 3;      /* ~6..20 */
                bb = 3;
            }
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            out[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, bb);
        }
    }
}

/* Frozen ice tile — clean Voronoi plates: a few big cells of light blue
 * split by near-white crack lines. Few seeds (big plates, not speckle),
 * LINEAR-distance crack metric (d2-d1 in sqrt space) so seams are
 * continuous ~1px lines rather than scattered dots, and a subtle per-plate
 * brightness tier + interior sheen for variation. Seeds wrap toroidally so
 * the tile is seamless: the renderer applies a per-cell offset/flip (like
 * lava) for block-scale variety, plus a world-position value-noise
 * brightness for lake-scale variety (see ice_macro_q8 in craft_render). */
static void bake_ice_tile(uint16_t *out) {
    enum { ICE_SEEDS = 7 };
    float sx[ICE_SEEDS], sy[ICE_SEEDS];
    uint32_t s = 0x1CE5A2u;
    for (int i = 0; i < ICE_SEEDS; i++) {
        sx[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
        sy[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
    }
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            float d1 = 1e9f, d2 = 1e9f; int c1 = 0;
            for (int i = 0; i < ICE_SEEDS; i++) {
                float dx = (float)x - sx[i], dy = (float)y - sy[i];
                dx -= 16.0f * floorf(dx / 16.0f + 0.5f);   /* toroidal — seamless */
                dy -= 16.0f * floorf(dy / 16.0f + 0.5f);
                float dd = dx * dx + dy * dy;
                if (dd < d1)      { d2 = d1; d1 = dd; c1 = i; }
                else if (dd < d2) { d2 = dd; }
            }
            float edge = sqrtf(d2) - sqrtf(d1);   /* linear → connected ~1px seam */
            int rr, gg, bb;
            if (edge < 1.0f) {                    /* near-white crack line */
                rr = 248; gg = 252; bb = 255;
            } else {                              /* light-blue plate */
                uint32_t h = (uint32_t)c1 * 0x9E3779B1u;
                float lev = (float)(int)(h & 3) - 1.5f;     /* -1.5..1.5 tier */
                float sd1 = sqrtf(d1);
                float sheen = (1.0f - (sd1 > 8.0f ? 8.0f : sd1) / 8.0f) * 14.0f;
                rr = (int)(224.0f + lev * 7.0f + sheen);     /* near-white, faint blue */
                gg = (int)(236.0f + lev * 7.0f + sheen);
                bb = (int)(250.0f + lev * 6.0f + sheen * 0.6f);
            }
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            out[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, bb);
        }
    }
}

/* Portal — a swirling purple haze with bright sparkle flecks. Two
 * octaves of periodic sinusoidal warp (seamless over 16px) drive
 * violet brightness bands that swirl; the variant offsets the phase so
 * the swirl rotates, and the sparkles twinkle between frames. */
static void bake_portal_frame(uint16_t *out, int variant) {
    const float F = 6.2831853f / 16.0f;
    float ph = variant ? 2.1f : 0.0f;
    uint32_t s = 0x90A7A1u ^ (variant ? 0x9E3779B9u : 0u);
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            float wx = x + 3.0f * sinf(F * 2.0f * y + ph)
                         + 1.6f * sinf(F * 3.0f * y - ph);
            float wy = y + 3.0f * cosf(F * 2.0f * x - ph)
                         + 1.6f * cosf(F * 3.0f * x + ph);
            float swirl = sinf(F * (wx + wy)) * 0.5f
                        + sinf(F * 2.0f * wx - ph) * 0.5f;   /* -1..1 */
            int p = (int)(96.0f + 70.0f * swirl);            /* ~26..166 */
            if (p < 0) p = 0;
            int rr = p + 40;          /* violet: strong R + B, weak G */
            int gg = p / 4;
            int bb = p + 70;
            uint32_t r = xs32(&s);
            if ((r & 0x3f) == 0) {    /* sparse twinkle */
                rr = 235; gg = 200; bb = 255;
            }
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            out[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, bb);
        }
    }
}

void craft_blocks_animate_water(float t) {
#ifdef CRAFT_TEXTURES_BAKED
    /* Lazy first-time build — guard for the case where build_textures
     * hasn't run yet (shouldn't normally happen, but harmless). */
    if (!craft_water_frames_built) {
        bake_water_frame(&craft_water_frames[0][0 * CRAFT_TEX_PIXELS],
                         &craft_water_frames[0][1 * CRAFT_TEX_PIXELS], 0);
        bake_water_frame(&craft_water_frames[1][0 * CRAFT_TEX_PIXELS],
                         &craft_water_frames[1][1 * CRAFT_TEX_PIXELS], 1);
        bake_lava_frame(craft_lava_frames[0], 0);
        bake_lava_frame(craft_lava_frames[1], 1);
        bake_portal_frame(craft_portal_frames[0], 0);
        bake_portal_frame(craft_portal_frames[1], 1);
        craft_water_frames_built = true;
    }
    /* 4 Hz toggle — same cadence as classic Minecraft water. */
    craft_water_frame_idx = ((int)(t * 4.0f)) & 1;
    /* Lava crawls slower — 2 Hz. */
    craft_lava_frame_idx = ((int)(t * 2.0f)) & 1;
    /* Portal swirls at 3 Hz. */
    craft_portal_frame_idx = ((int)(t * 3.0f)) & 1;
#else
    /* Unbaked (host development) path — write the chosen frame
     * straight into the atlas slots used by the renderer. */
    uint16_t *top  = &craft_textures[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS];
    uint16_t *side = &craft_textures[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS];
    int variant = ((int)(t * 4.0f)) & 1;
    bake_water_frame(top, side, variant);
    int lvar = ((int)(t * 2.0f)) & 1;
    bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + 0) * CRAFT_TEX_PIXELS], lvar);
    bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + 1) * CRAFT_TEX_PIXELS], lvar);
    bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + 2) * CRAFT_TEX_PIXELS], lvar);
    int pvar = ((int)(t * 3.0f)) & 1;
    bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + 0) * CRAFT_TEX_PIXELS], pvar);
    bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + 1) * CRAFT_TEX_PIXELS], pvar);
    bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + 2) * CRAFT_TEX_PIXELS], pvar);
#endif
}

/* Glass — mostly transparent-feeling pale tile with a darker frame. */
static void glass_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int border = (x == 0 || y == 0 || x == 15 || y == 15);
            int c = border ? 150 : 220;
            int b = border ? 170 : 235;
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c, c, b);
        }
    }
}

/* Leaves — clumpy green with sparse darker pixels for depth. */
/* Leaf cutout: many small leaf-shaped stamps (a 2px teardrop, ~3px) on
 * a 2px grid, ~10% of cells skipped → ~33% open canopy. Magenta texels
 * are holes the DDA BCLASS_CUBE path traces through (see-through
 * "fancy leaves"). Toroidally wrapped so adjacent leaf blocks tile
 * seamlessly. Leaves are biome-tinted at render, so the base greens
 * just need contrast. CRAFT_LEAF_FILL = % of grid cells that get a leaf. */
#define CRAFT_LEAF_FILL 100
static inline uint32_t leaf_hash(int a, int b) {
    uint32_t n = (uint32_t)(a * 73856093) ^ (uint32_t)(b * 19349663);
    n ^= n >> 13; n *= 0x9E3779B1u; n ^= n >> 16; return n;
}
static void leaves_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;   /* deterministic so all three face slots match */
    const int Sz = CRAFT_TEX_SIZE;
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) dst[i] = rgb565(255, 0, 255);
    const uint16_t gr[4] = {
        rgb565(40, 150, 52), rgb565(30, 128, 44),
        rgb565(52, 165, 64), rgb565(22, 104, 36),
    };
    #define LPLOT(xx,yy,cc) dst[(((yy)%Sz+Sz)%Sz)*Sz + (((xx)%Sz+Sz)%Sz)] = (cc)
    for (int gy = 0; gy < Sz; gy += 2)
        for (int gx = 0; gx < Sz; gx += 2) {
            uint32_t hh = leaf_hash(gx + 1, gy + 1);
            if ((int)(hh % 100) >= CRAFT_LEAF_FILL) continue;   /* hole */
            uint16_t base = gr[hh & 3], tip = gr[3];
            int x = gx + (int)((hh >> 9) & 1), y = gy + (int)((hh >> 10) & 1);
            LPLOT(x,     y,     base);
            LPLOT(x + 1, y,     base);
            LPLOT(x,     y + 1, tip);
            if (hh & 0x10000) LPLOT(x + 1, y + 1, base);
        }
    #undef LPLOT
}

#ifdef CRAFT_TEXTURES_BAKED
void craft_blocks_build_textures(void) {
    /* Atlas lives in flash; we just need to bake the two animated
     * water frames once into the in-RAM frame pair. */
    bake_water_frame(&craft_water_frames[0][0 * CRAFT_TEX_PIXELS],
                     &craft_water_frames[0][1 * CRAFT_TEX_PIXELS], 0);
    bake_water_frame(&craft_water_frames[1][0 * CRAFT_TEX_PIXELS],
                     &craft_water_frames[1][1 * CRAFT_TEX_PIXELS], 1);
    bake_lava_frame(craft_lava_frames[0], 0);
    bake_lava_frame(craft_lava_frames[1], 1);
    bake_portal_frame(craft_portal_frames[0], 0);
    bake_portal_frame(craft_portal_frames[1], 1);
    craft_water_frames_built = true;
    craft_water_frame_idx = 0;
    craft_lava_frame_idx = 0;
    craft_portal_frame_idx = 0;
}
#else
/* Generate an ore tile: speckled-stone base with ~18 small material
 * flecks. Mirrored to all three faces. */
static void ore_with_flecks(BlockId blk, uint32_t seed,
                            int fr, int fg, int fb) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    speckle(side, seed, 130, 130, 130, 50);
    uint32_t s = seed ^ 0xABCD0001u;
    for (int n = 0; n < 18; n++) {
        int cx = (int)(xs32(&s) % CRAFT_TEX_SIZE);
        int cy = (int)(xs32(&s) % CRAFT_TEX_SIZE);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int x = cx + dx, y = cy + dy;
                if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                if ((xs32(&s) & 3) == 0) continue;
                int j = (int)(xs32(&s) & 0x1F);
                int r = fr + j, g = fg + j, b = fb + j;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                side[y * CRAFT_TEX_SIZE + x] = rgb565(r, g, b);
            }
        }
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Inventory item tile: dark backdrop + a horizontal "bar" of two
 * shades from row 6 to 9, cols 3..12. Used by ingots; pale+dark
 * gives a metallic look. */
static void ingot_bar_tex(BlockId blk, int r, int g, int b) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
    uint16_t bright = rgb565(r, g, b);
    int dr = r * 3 / 4, dg = g * 3 / 4, db = b * 3 / 4;
    uint16_t dark   = rgb565(dr, dg, db);
    for (int x = 3; x < 13; x++) {
        side[6 * CRAFT_TEX_SIZE + x] = dark;
        side[7 * CRAFT_TEX_SIZE + x] = bright;
        side[8 * CRAFT_TEX_SIZE + x] = bright;
        side[9 * CRAFT_TEX_SIZE + x] = dark;
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Inventory item tile: dark backdrop + a diamond-shaped gem. */
static void gem_tex(BlockId blk, int r, int g, int b) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
    uint16_t bright = rgb565(r, g, b);
    int dr = r * 3 / 4, dg = g * 3 / 4, db = b * 3 / 4;
    uint16_t dark   = rgb565(dr, dg, db);
    int hr = r + (255 - r) / 2, hg = g + (255 - g) / 2, hb = b + (255 - b) / 2;
    uint16_t hi = rgb565(hr, hg, hb);
    /* Diamond outline rows: spread = |row - 7|. */
    for (int y = 3; y < 13; y++) {
        int spread = (y >= 8) ? (12 - y) : (y - 3);
        for (int dx = -spread; dx <= spread; dx++) {
            int x = 7 + dx;
            if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
            uint16_t c = bright;
            if (dx == -spread || dx == spread) c = dark;
            if (dx == -spread + 1 && y >= 4 && y <= 7) c = hi;
            side[y * CRAFT_TEX_SIZE + x] = c;
        }
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Inventory item tile: dark backdrop + 8 scattered specks of `c`. */
static void dust_tex(BlockId blk, uint32_t seed, int r, int g, int b) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
    uint16_t c = rgb565(r, g, b);
    uint32_t s = seed;
    for (int n = 0; n < 14; n++) {
        int cx = 3 + (int)(xs32(&s) % 10);
        int cy = 3 + (int)(xs32(&s) % 10);
        side[cy * CRAFT_TEX_SIZE + cx] = c;
        if ((xs32(&s) & 1) && cx + 1 < CRAFT_TEX_SIZE)
            side[cy * CRAFT_TEX_SIZE + cx + 1] = c;
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Storage block (silver/gold/diamond/redstone block): solid colour
 * with a slightly-darker border so faces visibly separate. Same on
 * top / side / bottom. */
static void solid_block_tex(BlockId blk, int r, int g, int b) {
    uint16_t bright = rgb565(r, g, b);
    int dr = r * 5 / 6, dg = g * 5 / 6, db = b * 5 / 6;
    uint16_t mid    = rgb565(dr, dg, db);
    int br = r * 2 / 3, bg = g * 2 / 3, bb = b * 2 / 3;
    uint16_t dark   = rgb565(br, bg, bb);
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            uint16_t c = bright;
            if (x == 0 || y == 0 || x == 15 || y == 15) c = dark;
            else if ((x ^ y) & 1) c = mid;
            side[y * CRAFT_TEX_SIZE + x] = c;
        }
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

void craft_blocks_build_textures(void) {
    /* AIR slot is never sampled but zero-init the rows anyway. */
    fill_solid(&craft_textures[(BLK_AIR * 3 + 0) * CRAFT_TEX_PIXELS], 0);
    fill_solid(&craft_textures[(BLK_AIR * 3 + 1) * CRAFT_TEX_PIXELS], 0);
    fill_solid(&craft_textures[(BLK_AIR * 3 + 2) * CRAFT_TEX_PIXELS], 0);

    /* STONE — uniform speckled grey. */
    speckle(&craft_textures[(BLK_STONE * 3 + 0) * CRAFT_TEX_PIXELS], 0xC0FFEE, 130, 130, 130, 60);
    speckle(&craft_textures[(BLK_STONE * 3 + 1) * CRAFT_TEX_PIXELS], 0xC0FFEE, 130, 130, 130, 60);
    speckle(&craft_textures[(BLK_STONE * 3 + 2) * CRAFT_TEX_PIXELS], 0xC0FFEE, 130, 130, 130, 60);

    /* DIRT — earthy brown. */
    speckle(&craft_textures[(BLK_DIRT * 3 + 0) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);
    speckle(&craft_textures[(BLK_DIRT * 3 + 1) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);
    speckle(&craft_textures[(BLK_DIRT * 3 + 2) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);

    /* GRASS — green top, dirt-with-green-edge side, dirt bottom. */
    speckle(&craft_textures[(BLK_GRASS * 3 + 0) * CRAFT_TEX_PIXELS], 0x1EAF1E, 70, 160, 50, 40);
    /* Side: paint dirt then green over top 5 rows. */
    {
        uint16_t *side = &craft_textures[(BLK_GRASS * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xDABBED, 130, 90, 50, 50);
        uint32_t s = 0x6166;
        for (int y = 0; y < 5; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int j = ((int)(xs32(&s) & 0x3f) - 32);
                int gg = 150 + j / 2 - y * 8;
                int rr = 60 + j / 2;
                side[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, 40);
            }
        }
    }
    speckle(&craft_textures[(BLK_GRASS * 3 + 2) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);

    /* SAND — pale yellow. */
    speckle(&craft_textures[(BLK_SAND * 3 + 0) * CRAFT_TEX_PIXELS], 0x5A1, 220, 200, 130, 30);
    speckle(&craft_textures[(BLK_SAND * 3 + 1) * CRAFT_TEX_PIXELS], 0x5A1, 220, 200, 130, 30);
    speckle(&craft_textures[(BLK_SAND * 3 + 2) * CRAFT_TEX_PIXELS], 0x5A1, 220, 200, 130, 30);

    /* WOOD — ring on caps, grain on sides. */
    {
        uint16_t *top = &craft_textures[(BLK_WOOD * 3 + 0) * CRAFT_TEX_PIXELS];
        uint32_t s = 0xDEAD;
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                int r2 = dx * dx + dy * dy;
                int ring = (r2 / 4) & 3;
                int base = 110 - ring * 12;
                int j = ((int)(xs32(&s) & 0xf) - 8);
                top[y * CRAFT_TEX_SIZE + x] = rgb565(base + j, base * 7 / 10 + j, base * 4 / 10);
            }
        }
    }
    wood_side_pattern(&craft_textures[(BLK_WOOD * 3 + 1) * CRAFT_TEX_PIXELS], 0xBEEF);
    /* Bottom: same as top. */
    memcpy(&craft_textures[(BLK_WOOD * 3 + 2) * CRAFT_TEX_PIXELS],
           &craft_textures[(BLK_WOOD * 3 + 0) * CRAFT_TEX_PIXELS],
           sizeof(uint16_t) * CRAFT_TEX_PIXELS);

    /* LEAVES — clumpy green. */
    leaves_pattern(&craft_textures[(BLK_LEAVES * 3 + 0) * CRAFT_TEX_PIXELS], 0xACE);
    leaves_pattern(&craft_textures[(BLK_LEAVES * 3 + 1) * CRAFT_TEX_PIXELS], 0xACE);
    leaves_pattern(&craft_textures[(BLK_LEAVES * 3 + 2) * CRAFT_TEX_PIXELS], 0xACE);

    /* PALM FROND — feathery green cutout, 4-fold symmetric so it has no
     * directional grain. Biome-tinted at render like other leaves, so a
     * jungle palm reads vivid green and a savanna/desert one drier. */
    {
        const int S = CRAFT_TEX_SIZE;
        for (int f = 0; f < 3; f++) {
            uint16_t *t = &craft_textures[(BLK_PALM_LEAF * 3 + f) * CRAFT_TEX_PIXELS];
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++) {
                    /* 4-fold symmetric so the frond has no directional
                     * grain — fronds reading the same texture in every
                     * arm direction then don't all "point" one way. */
                    int fx = x < 8 ? x : 15 - x;
                    int fy = y < 8 ? y : 15 - y;
                    uint32_t h = (uint32_t)((fx + 1) * 73856093) ^
                                 (uint32_t)((fy + 1) * 19349663);
                    h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
                    int g = 140 + (int)(h & 25);          /* 140..165 */
                    if (((h >> 5) & 3) == 0)              /* ~25% airy gaps */
                        t[y * S + x] = rgb565(255, 0, 255);
                    else
                        t[y * S + x] = rgb565(40, g, 46);
                }
        }
    }

    /* WATER — blue stripes. */
    water_pattern(&craft_textures[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS], 0x10ADED);
    water_pattern(&craft_textures[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS], 0x10ADED);
    water_pattern(&craft_textures[(BLK_WATER * 3 + 2) * CRAFT_TEX_PIXELS], 0x10ADED);

    /* COBBLESTONE — brick mortar pattern. */
    cobble_pattern(&craft_textures[(BLK_COBBLE * 3 + 0) * CRAFT_TEX_PIXELS], 0xC0B);
    cobble_pattern(&craft_textures[(BLK_COBBLE * 3 + 1) * CRAFT_TEX_PIXELS], 0xC0B);
    cobble_pattern(&craft_textures[(BLK_COBBLE * 3 + 2) * CRAFT_TEX_PIXELS], 0xC0B);

    /* ROGUE FLAGSTONE FLOORS — four variants (slab size + tone) scattered
     * across the floor for tessellating variety. */
    struct { int blk; uint32_t seed; int gsize; int tone; } rfloor[4] = {
        { BLK_RFLOOR,  0xF1A65u, 8,   0 },   /* medium slabs */
        { BLK_RFLOOR2, 0x5AB12u, 8, -12 },   /* darker medium slabs */
        { BLK_RFLOOR3, 0x9C3E7u, 4,  +6 },   /* small tiles */
        { BLK_RFLOOR4, 0x2D71Fu, 16, -4 },   /* one big cracked slab */
    };
    for (int v = 0; v < 4; v++)
        for (int slot = 0; slot < 3; slot++)
            flagstone_pattern(&craft_textures[(rfloor[v].blk * 3 + slot) * CRAFT_TEX_PIXELS],
                              rfloor[v].seed, rfloor[v].gsize, rfloor[v].tone);

    /* ThumbyRogue band blocks: warm Caverns rock + the unique Fungal Deep set. */
    for (int slot = 0; slot < 3; slot++) {
        cave_rock_pattern  (&craft_textures[(BLK_CAVE_ROCK   * 3 + slot) * CRAFT_TEX_PIXELS], 0xCA7E0u);
        mycelium_pattern   (&craft_textures[(BLK_MYCELIUM    * 3 + slot) * CRAFT_TEX_PIXELS], 0x5907Eu);
        fungal_wall_pattern(&craft_textures[(BLK_FUNGAL_WALL * 3 + slot) * CRAFT_TEX_PIXELS], 0xF09611u);
        mushroom_pattern   (&craft_textures[(BLK_MUSHROOM    * 3 + slot) * CRAFT_TEX_PIXELS], 0x3057Eu);
    }

    /* ThumbyRogue room scenery — bulky cube props (top/side/bottom differ). */
    for (int slot = 0; slot < 3; slot++) {
        bookcase_pattern    (&craft_textures[(BLK_BOOKCASE    * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0xB00C5u);
        barrel_pattern      (&craft_textures[(BLK_BARREL      * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0xBA77Eu);
        crate_pattern       (&craft_textures[(BLK_CRATE       * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0xC8A7Eu);
        sarcophagus_pattern (&craft_textures[(BLK_SARCOPHAGUS * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0x5A8C0u);
        crystal_pattern     (&craft_textures[(BLK_CRYSTAL     * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0xC8957u);
        riverbed_pattern    (&craft_textures[(BLK_RIVERBED    * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0xB1BEDu);
        shop_counter_pattern(&craft_textures[(BLK_SHOPCOUNTER * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0x5C047u);
        shop_shelf_pattern  (&craft_textures[(BLK_SHOPSHELF   * 3 + slot) * CRAFT_TEX_PIXELS], slot, 0x5E1F5u);
    }
    /* ThumbyRogue cross-sprite scenery — same silhouette on every face. */
    for (int slot = 0; slot < 3; slot++) {
        bones_pattern (&craft_textures[(BLK_BONES  * 3 + slot) * CRAFT_TEX_PIXELS], 0xB04E5u);
        rubble_pattern(&craft_textures[(BLK_RUBBLE * 3 + slot) * CRAFT_TEX_PIXELS], 0x2BB1Eu);
        shards_pattern(&craft_textures[(BLK_SHARDS * 3 + slot) * CRAFT_TEX_PIXELS], 0x58A2Du);
        fungi_pattern (&craft_textures[(BLK_FUNGI  * 3 + slot) * CRAFT_TEX_PIXELS], 0xF0671u);
        cobweb_pattern(&craft_textures[(BLK_COBWEB * 3 + slot) * CRAFT_TEX_PIXELS], 0xC0BEBu);
    }

    /* PLANK — horizontal bands. */
    plank_pattern(&craft_textures[(BLK_PLANK * 3 + 0) * CRAFT_TEX_PIXELS], 0xFADE);
    plank_pattern(&craft_textures[(BLK_PLANK * 3 + 1) * CRAFT_TEX_PIXELS], 0xFADE);
    plank_pattern(&craft_textures[(BLK_PLANK * 3 + 2) * CRAFT_TEX_PIXELS], 0xFADE);

    /* GLASS — pale tile w/ darker frame. */
    glass_pattern(&craft_textures[(BLK_GLASS * 3 + 0) * CRAFT_TEX_PIXELS], 0);
    glass_pattern(&craft_textures[(BLK_GLASS * 3 + 1) * CRAFT_TEX_PIXELS], 0);
    glass_pattern(&craft_textures[(BLK_GLASS * 3 + 2) * CRAFT_TEX_PIXELS], 0);

    /* COAL ORE — speckled grey stone with dark clusters. */
    {
        uint16_t *side = &craft_textures[(BLK_COAL_ORE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xC0A1, 110, 110, 115, 50);
        /* Sprinkle ~6 coal clusters. */
        uint32_t s = 0xC0A100u;
        for (int n = 0; n < 24; n++) {
            int cx = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            int cy = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int x = cx + dx, y = cy + dy;
                    if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                    if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                    if ((xs32(&s) & 3) == 0) continue;
                    side[y * CRAFT_TEX_SIZE + x] = rgb565(20, 20, 25);
                }
            }
        }
        memcpy(&craft_textures[(BLK_COAL_ORE * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_COAL_ORE * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* TORCH — thin brown stem with a bright orange flame at the top.
     * Drawn the same on all 6 faces (cube approximation). */
    {
        uint16_t *side = &craft_textures[(BLK_TORCH * 3 + 1) * CRAFT_TEX_PIXELS];
        /* Dim grey background — torches are see-through-ish in real
         * Minecraft, but our raycaster doesn't do partial transparency. */
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(20, 18, 25);
        /* Brown stem 2 px wide in the centre, rows 8..14. */
        for (int y = 8; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = rgb565(110, 70, 30);
            side[y * CRAFT_TEX_SIZE + 8] = rgb565(140, 95, 45);
        }
        /* Flame: 3-wide warm gradient near the top. */
        uint16_t f_core = rgb565(255, 230, 100);
        uint16_t f_mid  = rgb565(255, 170, 40);
        uint16_t f_low  = rgb565(220, 80, 20);
        side[3 * CRAFT_TEX_SIZE + 8] = f_low;
        for (int y = 4; y < 8; y++) {
            side[y * CRAFT_TEX_SIZE + 6] = (y == 7) ? f_low : 0;
            side[y * CRAFT_TEX_SIZE + 7] = (y == 4) ? f_mid : (y == 7 ? f_core : f_mid);
            side[y * CRAFT_TEX_SIZE + 8] = (y == 7) ? f_core : f_core;
            side[y * CRAFT_TEX_SIZE + 9] = (y == 7) ? f_low : f_mid;
            side[y * CRAFT_TEX_SIZE + 10] = (y == 7) ? f_low : 0;
        }
        memcpy(&craft_textures[(BLK_TORCH * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_TORCH * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* IRON ORE — stone base with rusty orange flecks. */
    {
        uint16_t *side = &craft_textures[(BLK_IRON_ORE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1207E, 130, 130, 130, 50);
        uint32_t s = 0x1207EBu;
        for (int n = 0; n < 18; n++) {
            int cx = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            int cy = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int x = cx + dx, y = cy + dy;
                    if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                    if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                    if ((xs32(&s) & 3) == 0) continue;
                    int j = (int)(xs32(&s) & 0x1F);
                    side[y * CRAFT_TEX_SIZE + x] =
                        rgb565(190 + j, 110 + j / 2, 50 + j / 4);
                }
            }
        }
        memcpy(&craft_textures[(BLK_IRON_ORE * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_IRON_ORE * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* --- Inventory-only items — single-face icons, dark backdrop. */

    /* STICK — vertical thin brown line. */
    {
        uint16_t *side = &craft_textures[(BLK_STICK * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t a = rgb565(160, 110, 60), b = rgb565(120, 80, 40);
        for (int y = 2; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = a;
            side[y * CRAFT_TEX_SIZE + 8] = b;
        }
        memcpy(&craft_textures[(BLK_STICK * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_STICK * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* IRON INGOT — horizontal silver bar. */
    {
        uint16_t *side = &craft_textures[(BLK_IRON_INGOT * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t a = rgb565(220, 220, 230), b = rgb565(170, 170, 180);
        for (int x = 3; x < 13; x++) {
            side[6 * CRAFT_TEX_SIZE + x] = b;
            side[7 * CRAFT_TEX_SIZE + x] = a;
            side[8 * CRAFT_TEX_SIZE + x] = a;
            side[9 * CRAFT_TEX_SIZE + x] = b;
        }
        memcpy(&craft_textures[(BLK_IRON_INGOT * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_IRON_INGOT * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* Common tool icon — stone-style pickaxe with tunable head colour.
     * Non-static: rgb565 isn't a constant expression so a static
     * initializer wouldn't compile. Trivial stack init at startup. */
    const uint16_t TIER_PICK[6][2] = {
        { rgb565(155, 110, 60),  rgb565(115, 80, 40)   }, /* wood */
        { rgb565(130, 130, 135), rgb565(85, 85, 90)    }, /* stone */
        { rgb565(225, 225, 235), rgb565(170, 170, 180) }, /* iron */
        { rgb565(190, 210, 225), rgb565(140, 165, 185) }, /* silver */
        { rgb565(255, 215, 60),  rgb565(195, 155, 30)  }, /* gold */
        { rgb565(120, 240, 250), rgb565(70, 175, 200)  }, /* diamond */
    };
    BlockId pick_ids[6] = {
        BLK_PICKAXE_WOOD, BLK_PICKAXE_STONE, BLK_PICKAXE_IRON,
        BLK_PICKAXE_SILVER, BLK_PICKAXE_GOLD, BLK_PICKAXE_DIAMOND,
    };
    for (int tier = 0; tier < 6; tier++) {
        uint16_t *side = &craft_textures[(pick_ids[tier] * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t head   = TIER_PICK[tier][0];
        uint16_t head_d = TIER_PICK[tier][1];
        for (int x = 2; x < 14; x++) {
            side[2 * CRAFT_TEX_SIZE + x] = (x == 2 || x == 13) ? head_d : head;
            side[3 * CRAFT_TEX_SIZE + x] = head;
        }
        side[4 * CRAFT_TEX_SIZE + 7] = head_d;
        side[4 * CRAFT_TEX_SIZE + 8] = head_d;
        uint16_t wood = rgb565(150, 100, 50), wood_d = rgb565(110, 70, 35);
        for (int i = 0; i < 10; i++) {
            int x = 7 + i / 2;
            int y = 5 + i;
            if ((unsigned)x < CRAFT_TEX_SIZE && (unsigned)y < CRAFT_TEX_SIZE)
                side[y * CRAFT_TEX_SIZE + x] = (i & 1) ? wood : wood_d;
            x++;
            if ((unsigned)x < CRAFT_TEX_SIZE && (unsigned)y < CRAFT_TEX_SIZE)
                side[y * CRAFT_TEX_SIZE + x] = wood;
        }
        memcpy(&craft_textures[(pick_ids[tier] * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(pick_ids[tier] * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    const uint16_t TIER_SWORD[6][2] = {
        { rgb565(170, 130, 70),  rgb565(120, 90, 50)   }, /* wood */
        { rgb565(140, 140, 145), rgb565(95, 95, 100)   }, /* stone */
        { rgb565(230, 230, 240), rgb565(170, 170, 180) }, /* iron */
        { rgb565(195, 215, 230), rgb565(145, 170, 190) }, /* silver */
        { rgb565(255, 215, 60),  rgb565(195, 155, 30)  }, /* gold */
        { rgb565(130, 245, 255), rgb565(75, 180, 210)  }, /* diamond */
    };
    BlockId sword_ids[6] = {
        BLK_SWORD_WOOD, BLK_SWORD_STONE, BLK_SWORD_IRON,
        BLK_SWORD_SILVER, BLK_SWORD_GOLD, BLK_SWORD_DIAMOND,
    };
    for (int tier = 0; tier < 6; tier++) {
        uint16_t *side = &craft_textures[(sword_ids[tier] * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t blade = TIER_SWORD[tier][0], blade_d = TIER_SWORD[tier][1];
        /* Blade column, rows 2..10. */
        for (int y = 2; y < 11; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = blade_d;
            side[y * CRAFT_TEX_SIZE + 8] = blade;
        }
        /* Tip — taper at top. */
        side[1 * CRAFT_TEX_SIZE + 8] = blade;
        /* Hilt cross at row 11-12. */
        uint16_t hilt = rgb565(130, 95, 50);
        for (int x = 5; x < 11; x++) side[11 * CRAFT_TEX_SIZE + x] = hilt;
        /* Handle below hilt. */
        uint16_t handle = rgb565(115, 75, 40);
        side[12 * CRAFT_TEX_SIZE + 7] = handle;
        side[12 * CRAFT_TEX_SIZE + 8] = handle;
        side[13 * CRAFT_TEX_SIZE + 7] = handle;
        side[13 * CRAFT_TEX_SIZE + 8] = handle;
        memcpy(&craft_textures[(sword_ids[tier] * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(sword_ids[tier] * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* BOW — horizontal C-curve in wood with a vertical white string. */
    {
        uint16_t *side = &craft_textures[(BLK_BOW * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t wood = rgb565(140, 95, 45), wood_d = rgb565(100, 65, 30);
        uint16_t str  = rgb565(220, 220, 230);
        /* Curve arch — two arcs from rows 3..13, columns 4..6 (top) and
         * 9..11 (bottom), with a back rail at col 4 / 11. */
        for (int y = 3; y < 6; y++) {
            side[y * CRAFT_TEX_SIZE + 5] = wood;
            side[y * CRAFT_TEX_SIZE + 4] = wood_d;
        }
        for (int y = 11; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 5] = wood;
            side[y * CRAFT_TEX_SIZE + 4] = wood_d;
        }
        for (int y = 5; y < 12; y++) {
            side[y * CRAFT_TEX_SIZE + 3] = wood;
        }
        /* Bowstring — vertical white line on the inner side. */
        for (int y = 3; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = str;
        }
        memcpy(&craft_textures[(BLK_BOW * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_BOW * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* FURNACE — cobble-style sides + top, and a darker face panel
     * with a glowing mouth on the FRONT (we paint that into the
     * "side" slot since the renderer uses side for all four
     * horizontal faces — the mouth shows on every side, which is
     * fine without a directional facing bit). */
    {
        /* Top + bottom: stone-grey speckle, similar to cobble. */
        uint16_t *top = &craft_textures[(BLK_FURNACE * 3 + 0) * CRAFT_TEX_PIXELS];
        speckle(top, 0xF00DBEEFu, 120, 120, 130, 40);
        memcpy(&craft_textures[(BLK_FURNACE * 3 + 2) * CRAFT_TEX_PIXELS],
               top, sizeof(uint16_t) * CRAFT_TEX_PIXELS);

        /* Side: stone speckle + dark recessed face panel with a
         * 2-row glow at the bottom of the mouth. */
        uint16_t *side = &craft_textures[(BLK_FURNACE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xF00DCAFEu, 110, 110, 120, 35);
        /* Iron-frame rectangle around the mouth at rows 3..13, cols 2..13. */
        uint16_t frame_d = rgb565(60, 60, 70);
        uint16_t frame   = rgb565(95, 95, 110);
        for (int x = 2; x < 14; x++) {
            side[3  * CRAFT_TEX_SIZE + x] = frame;
            side[13 * CRAFT_TEX_SIZE + x] = frame_d;
        }
        for (int y = 3; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 2]  = frame;
            side[y * CRAFT_TEX_SIZE + 13] = frame_d;
        }
        /* Mouth interior — dark void with a warm fire glow at the
         * bottom 3 rows so it reads as "burning" at a glance. */
        uint16_t void_c = rgb565(15, 15, 20);
        uint16_t fire1  = rgb565(220, 100, 30);
        uint16_t fire2  = rgb565(255, 180, 60);
        for (int y = 4; y < 13; y++) {
            for (int x = 3; x < 13; x++) {
                side[y * CRAFT_TEX_SIZE + x] = void_c;
            }
        }
        for (int x = 3; x < 13; x++) {
            side[10 * CRAFT_TEX_SIZE + x] = fire1;
            side[11 * CRAFT_TEX_SIZE + x] = fire2;
            side[12 * CRAFT_TEX_SIZE + x] = fire1;
        }
    }

    /* CHEST — plank body with iron strapping and a small dark keyhole
     * front-and-centre. Top has a visible lid seam. */
    {
        uint16_t *top  = &craft_textures[(BLK_CHEST * 3 + 0) * CRAFT_TEX_PIXELS];
        uint16_t *side = &craft_textures[(BLK_CHEST * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t plank_a = rgb565(170, 115, 60);
        uint16_t plank_b = rgb565(140, 90, 45);
        uint16_t plank_d = rgb565(100, 65, 30);
        uint16_t iron    = rgb565(95,  95, 110);
        uint16_t iron_d  = rgb565(60,  60, 70);
        uint16_t hole    = rgb565(15, 15, 20);
        /* Top: alternating plank stripes + a central seam where the lid opens. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            uint16_t base = ((y / 3) & 1) ? plank_a : plank_b;
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                top[y * CRAFT_TEX_SIZE + x] = base;
            }
            /* Lid seam — horizontal dark line down the middle. */
            if (y == 7 || y == 8) {
                for (int x = 1; x < CRAFT_TEX_SIZE - 1; x++)
                    top[y * CRAFT_TEX_SIZE + x] = plank_d;
            }
        }
        /* Iron corner studs on top. */
        top[1 * CRAFT_TEX_SIZE + 1] = iron;
        top[1 * CRAFT_TEX_SIZE + 14] = iron;
        top[14 * CRAFT_TEX_SIZE + 1] = iron;
        top[14 * CRAFT_TEX_SIZE + 14] = iron;
        memcpy(&craft_textures[(BLK_CHEST * 3 + 2) * CRAFT_TEX_PIXELS],
               top, sizeof(uint16_t) * CRAFT_TEX_PIXELS);

        /* Side: plank stripes + vertical iron bands at the corners
         * + horizontal iron bar across the top (where the hinges
         * would be) + a small keyhole rectangle just below centre. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            uint16_t base = ((y / 3) & 1) ? plank_a : plank_b;
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                side[y * CRAFT_TEX_SIZE + x] = base;
            }
        }
        /* Top iron band — hinges. */
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            side[1 * CRAFT_TEX_SIZE + x] = iron;
            side[2 * CRAFT_TEX_SIZE + x] = iron_d;
        }
        /* Vertical corner bands. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 0]  = iron_d;
            side[y * CRAFT_TEX_SIZE + 15] = iron_d;
        }
        /* Keyhole — 2×2 dark cluster mid-front. */
        side[8  * CRAFT_TEX_SIZE + 7] = hole;
        side[8  * CRAFT_TEX_SIZE + 8] = hole;
        side[9  * CRAFT_TEX_SIZE + 7] = hole;
        side[9  * CRAFT_TEX_SIZE + 8] = hole;
        /* Iron lock-plate around the keyhole. */
        side[7  * CRAFT_TEX_SIZE + 6] = iron;
        side[7  * CRAFT_TEX_SIZE + 9] = iron;
        side[10 * CRAFT_TEX_SIZE + 6] = iron;
        side[10 * CRAFT_TEX_SIZE + 9] = iron;
    }

    /* ARROW — diagonal shaft with white flight + dark tip. */
    {
        uint16_t *side = &craft_textures[(BLK_ARROW * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t shaft = rgb565(150, 110, 70);
        uint16_t tip   = rgb565(80, 80, 90);
        uint16_t fletch= rgb565(230, 230, 230);
        /* Shaft running TL → BR. */
        for (int i = 2; i < 14; i++) {
            int x = i, y = i;
            if ((unsigned)x < CRAFT_TEX_SIZE && (unsigned)y < CRAFT_TEX_SIZE)
                side[y * CRAFT_TEX_SIZE + x] = shaft;
        }
        /* Tip cluster at top-left. */
        side[1 * CRAFT_TEX_SIZE + 1] = tip;
        side[2 * CRAFT_TEX_SIZE + 1] = tip;
        side[1 * CRAFT_TEX_SIZE + 2] = tip;
        /* Fletching cluster at bottom-right. */
        side[13 * CRAFT_TEX_SIZE + 14] = fletch;
        side[14 * CRAFT_TEX_SIZE + 13] = fletch;
        side[14 * CRAFT_TEX_SIZE + 14] = fletch;
        memcpy(&craft_textures[(BLK_ARROW * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_ARROW * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* Precious-metal ores — speckled stone with material flecks. */
    ore_with_flecks(BLK_SILVER_ORE,   0x511A011u, 190, 210, 225);
    ore_with_flecks(BLK_GOLD_ORE,     0x6011D11u, 220, 180,  40);
    ore_with_flecks(BLK_DIAMOND_ORE,  0xD1A1011u,  90, 200, 220);
    ore_with_flecks(BLK_REDSTONE_ORE, 0xED5701Cu, 200,  40,  40);

    /* Ingots / gem / dust. */
    ingot_bar_tex(BLK_SILVER_INGOT, 215, 225, 235);
    ingot_bar_tex(BLK_GOLD_INGOT,   245, 210,  55);
    gem_tex      (BLK_DIAMOND,      130, 240, 250);
    dust_tex     (BLK_REDSTONE,     0xDEADBEEFu, 230,  40,  40);

    /* Storage blocks — solid colour with hatched mid + dark border. */
    solid_block_tex(BLK_SILVER_BLOCK,   210, 220, 230);
    solid_block_tex(BLK_GOLD_BLOCK,     245, 210,  55);
    solid_block_tex(BLK_DIAMOND_BLOCK,  130, 240, 250);
    solid_block_tex(BLK_REDSTONE_BLOCK, 210,  40,  40);

    /* LEVER — high-contrast: dark stone base plate at the bottom,
     * a chunky wood handle running diagonally to a bright ball-tip.
     * ON state mirrors the handle and lights the tip bright red so
     * it reads as "pulled". Same texture on every face. */
    for (int on = 0; on < 2; on++) {
        BlockId blk = on ? BLK_LEVER_ON : BLK_LEVER_OFF;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        /* Dark stone backdrop. */
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(60, 60, 70);
        /* Mounting plate — solid grey block at the bottom centre. */
        uint16_t plate    = rgb565(160, 160, 170);
        uint16_t plate_d  = rgb565(95, 95, 105);
        for (int y = 10; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 3; x < 13; x++) {
                bool edge = (y == 10 || y == 15 || x == 3 || x == 12);
                side[y * CRAFT_TEX_SIZE + x] = edge ? plate_d : plate;
            }
        }
        /* Handle — 3-wide diagonal stalk to make it readable. */
        uint16_t wood   = rgb565(190, 130, 60);
        uint16_t wood_d = rgb565(110, 75, 35);
        uint16_t tip    = on ? rgb565(255, 80, 60) : rgb565(220, 220, 230);
        uint16_t tip_d  = on ? rgb565(170, 30, 20) : rgb565(150, 150, 160);
        for (int i = 0; i < 6; i++) {
            int yy = 10 - i;
            int xx = on ? (7 + i) : (7 - i);
            if (yy < 0 || yy >= CRAFT_TEX_SIZE) continue;
            for (int dx = -1; dx <= 1; dx++) {
                int x = xx + dx;
                if (x < 0 || x >= CRAFT_TEX_SIZE) continue;
                side[yy * CRAFT_TEX_SIZE + x] = (dx == 0) ? wood : wood_d;
            }
        }
        /* Ball tip — 3×3 cluster at the top of the handle. */
        int tx = on ? 12 : 2;
        int ty = 4;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int x = tx + dx, y = ty + dy;
                if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                bool centre = (dx == 0 && dy == 0);
                side[y * CRAFT_TEX_SIZE + x] = centre ? tip : tip_d;
            }
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LADDER — wooden rails + rungs on a dark backdrop (rendered as
     * a 2D vertical sprite via craft_torches, like wire). */
    {
        uint16_t *side = &craft_textures[(BLK_LADDER * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(255, 0, 255); /* cutout */
        uint16_t rail = rgb565(140, 90, 40);
        uint16_t rung = rgb565(170, 115, 55);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 3] = rail;
            side[y * CRAFT_TEX_SIZE + 12] = rail;
        }
        for (int ry = 1; ry < CRAFT_TEX_SIZE; ry += 4) {
            for (int x = 4; x < 12; x++) {
                side[ry * CRAFT_TEX_SIZE + x] = rung;
            }
        }
        memcpy(&craft_textures[(BLK_LADDER * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_LADDER * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* TRAPDOOR — horizontal wood planks with iron strapping. Open
     * variant uses the same texture (we only render closed cells as
     * cubes; open cells are passable + invisible for v1). */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_TRAPDOOR_OFF : BLK_TRAPDOOR_ON;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t plank = rgb565(150, 100, 50);
        uint16_t plank_d = rgb565(110, 70, 35);
        uint16_t iron = rgb565(90, 90, 100);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                uint16_t c = ((y / 3) & 1) ? plank : plank_d;
                side[y * CRAFT_TEX_SIZE + x] = c;
            }
        }
        for (int x = 1; x < 15; x++) {
            side[2  * CRAFT_TEX_SIZE + x] = iron;
            side[13 * CRAFT_TEX_SIZE + x] = iron;
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* DOOR — vertical plank face with hinge band on one side.
     * Closed = solid wall; open = passable (same texture either way). */
    /* DOOR — a classic framed wooden door: vertical stiles + a rail at
     * the top and bottom of each cell, with a recessed bevelled panel in
     * the middle. The two stacked cells of a 2-tall door tile into a
     * proper 2-panel door (the cell-seam rails read as the mid-rail),
     * and the full-height stiles stay continuous. No handle — any
     * localised detail would duplicate on the two-cell door, and the
     * frame + panels already read clearly as a door. */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_DOOR_OFF : BLK_DOOR_ON;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t wood   = rgb565(170, 116, 58);   /* plank face        */
        uint16_t grain  = rgb565(150, 100, 48);   /* grain line        */
        uint16_t frame  = rgb565(96, 60, 28);     /* stiles + rails    */
        uint16_t recess = rgb565(120, 78, 38);    /* panel shadow      */
        uint16_t panel  = rgb565(158, 106, 52);   /* panel face        */
        uint16_t bevel  = rgb565(202, 148, 84);   /* lit panel edge    */
        const int S = CRAFT_TEX_SIZE;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++)
                side[y * S + x] = ((x & 3) == 2) ? grain : wood;   /* vertical grain */
        /* Frame: 2px stiles (L/R, full height → continuous) + 1px rails. */
        for (int y = 0; y < S; y++) {
            side[y * S + 0] = frame; side[y * S + 1] = frame;
            side[y * S + 14] = frame; side[y * S + 15] = frame;
        }
        for (int x = 0; x < S; x++) { side[0 * S + x] = frame; side[15 * S + x] = frame; }
        /* Recessed panel [3..12] with a bevel: top/left lit, bottom/right shadow. */
        for (int y = 3; y <= 12; y++)
            for (int x = 3; x <= 12; x++) {
                if (y == 3 || x == 3)        side[y * S + x] = bevel;
                else if (y == 12 || x == 12) side[y * S + x] = recess;
                else                          side[y * S + x] = panel;
            }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* PRESSURE PAD — flat stone slab, rendered as 2D sprite via the
     * torch system (same path as wires). */
    {
        uint16_t *side = &craft_textures[(BLK_PRESSURE_PAD * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(255, 0, 255); /* cutout */
        uint16_t pad = rgb565(150, 150, 160);
        uint16_t pad_d = rgb565(110, 110, 120);
        for (int y = 4; y < 13; y++) {
            for (int x = 2; x < 14; x++) {
                bool edge = (y == 4 || y == 12 || x == 2 || x == 13);
                side[y * CRAFT_TEX_SIZE + x] = edge ? pad_d : pad;
            }
        }
        memcpy(&craft_textures[(BLK_PRESSURE_PAD * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_PRESSURE_PAD * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* PISTON — iron-banded stone block with a bright square on the
     * face that pushes (one side gets a different texture, but for
     * v1 every face shows the same — orientation goes through the
     * lever pipeline later). Active variant brightens the face. */
    for (int variant = 0; variant < 4; variant++) {
        /* 0/1 = regular off/on (brown face), 2/3 = sticky off/on (green
         * slime face) — so the two pistons read differently in the
         * inventory swatch and the held-item viewport, matching the
         * green slime cap the 3D world model now draws. */
        bool sticky = (variant >= 2);
        bool on     = (variant == 1 || variant == 3);
        BlockId blk = (variant == 0) ? BLK_PISTON_OFF :
                      (variant == 1) ? BLK_PISTON_ON :
                      (variant == 2) ? BLK_STICKY_PISTON_OFF : BLK_STICKY_PISTON_ON;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xD15700u + variant, 120, 120, 130, 35);
        uint16_t band = rgb565(85, 85, 100);
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            side[1 * CRAFT_TEX_SIZE + x]  = band;
            side[14 * CRAFT_TEX_SIZE + x] = band;
        }
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 1]  = band;
            side[y * CRAFT_TEX_SIZE + 14] = band;
        }
        uint16_t face = sticky ? (on ? rgb565(150, 230, 110) : rgb565(110, 185, 75))
                               : (on ? rgb565(220, 180, 90)  : rgb565(170, 130, 60));
        for (int y = 5; y < 11; y++) {
            for (int x = 5; x < 11; x++) {
                side[y * CRAFT_TEX_SIZE + x] = face;
            }
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* PISTON_ARM — thinner extension shape; same iron band. */
    {
        uint16_t *side = &craft_textures[(BLK_PISTON_ARM * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(140, 140, 150);
        uint16_t band = rgb565(85, 85, 100);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 0] = band;
            side[y * CRAFT_TEX_SIZE + 15] = band;
        }
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            side[0 * CRAFT_TEX_SIZE + x] = band;
            side[15 * CRAFT_TEX_SIZE + x] = band;
        }
        memcpy(&craft_textures[(BLK_PISTON_ARM * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_PISTON_ARM * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* TNT — red-and-white striped block. Fused variant adds a glowing
     * core and brighter stripes to read as "about to explode". */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_TNT : BLK_TNT_FUSED;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t red    = variant ? rgb565(255, 100, 80) : rgb565(200, 50, 40);
        uint16_t white  = variant ? rgb565(255, 255, 200) : rgb565(220, 220, 220);
        uint16_t letter = rgb565(20, 20, 25);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                bool stripe = (y >= 4 && y < 12);
                side[y * CRAFT_TEX_SIZE + x] = stripe ? red : white;
            }
        }
        /* "TNT" letters — 3 dark verticals on the red band. */
        for (int y = 6; y < 10; y++) {
            side[y * CRAFT_TEX_SIZE + 4]  = letter;
            side[y * CRAFT_TEX_SIZE + 8]  = letter;
            side[y * CRAFT_TEX_SIZE + 12] = letter;
        }
        side[5  * CRAFT_TEX_SIZE + 8] = letter;     /* T crossbar tops */
        side[5  * CRAFT_TEX_SIZE + 4] = letter;
        side[5  * CRAFT_TEX_SIZE + 12] = letter;
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* WIRE — solid red fill (off=dim, on=bright). The DDA draws the
     * dust as a flat floor slab and cuts the cross/arm SHAPE from the
     * live connection mask (craft_render BCLASS_PANEL wire path), so
     * this tile only needs to supply the colour, not the routing. */
    for (int on = 0; on < 2; on++) {
        BlockId blk = on ? BLK_REDSTONE_WIRE_ON : BLK_REDSTONE_WIRE;
        uint16_t col = on ? rgb565(235, 60, 48) : rgb565(150, 40, 34);
        for (int f = 0; f < 3; f++)
            for (int i = 0; i < CRAFT_TEX_PIXELS; i++)
                craft_textures[(blk * 3 + f) * CRAFT_TEX_PIXELS + i] = col;
    }

    /* OBSERVER — dark grey block, single bright "lens" eye on the
     * side and a tiny back vent. Direction marker is the lens. */
    {
        uint16_t *side = &craft_textures[(BLK_OBSERVER * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x0B5, 75, 75, 80, 30);
        /* Centred 4×4 cyan lens with brighter centre. */
        uint16_t lens   = rgb565(140, 220, 220);
        uint16_t pupil  = rgb565(40, 180, 220);
        for (int y = 6; y < 10; y++) {
            for (int x = 6; x < 10; x++) {
                side[y * CRAFT_TEX_SIZE + x] = lens;
            }
        }
        side[7 * CRAFT_TEX_SIZE + 7] = pupil;
        side[7 * CRAFT_TEX_SIZE + 8] = pupil;
        side[8 * CRAFT_TEX_SIZE + 7] = pupil;
        side[8 * CRAFT_TEX_SIZE + 8] = pupil;
        memcpy(&craft_textures[(BLK_OBSERVER * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_OBSERVER * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* NOTE BLOCK — plank-coloured cube with a black eighth-note glyph
     * for unmistakable identification. */
    {
        uint16_t *side = &craft_textures[(BLK_NOTE_BLOCK * 3 + 1) * CRAFT_TEX_PIXELS];
        plank_pattern(side, 0xF00D);
        uint16_t glyph = rgb565(20, 20, 25);
        /* Stem */
        for (int y = 4; y < 12; y++) side[y * CRAFT_TEX_SIZE + 9] = glyph;
        /* Filled note head */
        for (int y = 11; y < 14; y++) {
            for (int x = 6; x < 10; x++) {
                side[y * CRAFT_TEX_SIZE + x] = glyph;
            }
        }
        /* Flag */
        side[4 * CRAFT_TEX_SIZE + 9]  = glyph;
        side[4 * CRAFT_TEX_SIZE + 10] = glyph;
        side[5 * CRAFT_TEX_SIZE + 11] = glyph;
        side[6 * CRAFT_TEX_SIZE + 11] = glyph;
        memcpy(&craft_textures[(BLK_NOTE_BLOCK * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_NOTE_BLOCK * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LAMP — warm-glass cube, soft cross pattern. The lit variant is
     * produced live by the renderer's brightness path so we only
     * bake the off colour here. */
    {
        uint16_t *side = &craft_textures[(BLK_LAMP * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1A3D, 130, 100, 60, 25);
        uint16_t accent = rgb565(180, 140, 80);
        for (int x = 1; x < 15; x++) {
            side[3 * CRAFT_TEX_SIZE + x]  = accent;
            side[12 * CRAFT_TEX_SIZE + x] = accent;
        }
        for (int y = 1; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 3]  = accent;
            side[y * CRAFT_TEX_SIZE + 12] = accent;
        }
        memcpy(&craft_textures[(BLK_LAMP * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_LAMP * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LAMP_ON — lit: bright warm glow with a brighter cross. Distinct
     * from the dark off tile so the lamp visibly lights up; it's also
     * a light source (see craft_world_rebuild_lightmap). */
    {
        uint16_t *side = &craft_textures[(BLK_LAMP_ON * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1A3E, 255, 224, 150, 16);
        uint16_t accent = rgb565(255, 250, 215);
        for (int x = 1; x < 15; x++) {
            side[3 * CRAFT_TEX_SIZE + x]  = accent;
            side[12 * CRAFT_TEX_SIZE + x] = accent;
        }
        for (int y = 1; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 3]  = accent;
            side[y * CRAFT_TEX_SIZE + 12] = accent;
        }
        memcpy(&craft_textures[(BLK_LAMP_ON * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_LAMP_ON * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* NOT GATE — stone-coloured with a centred white "¬" glyph and a
     * direction arrow ridge. Output state is rendered as-is for now;
     * sweeping a state-aware texture variant in is a follow-up. */
    {
        uint16_t *side = &craft_textures[(BLK_NOT_GATE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1701, 120, 120, 130, 30);
        uint16_t glyph = rgb565(240, 240, 240);
        /* Horizontal bar */
        for (int x = 4; x < 12; x++) side[7 * CRAFT_TEX_SIZE + x] = glyph;
        /* Right-side drop tick (the "¬" hook). */
        side[8  * CRAFT_TEX_SIZE + 11] = glyph;
        side[9  * CRAFT_TEX_SIZE + 11] = glyph;
        /* Direction arrow on the bottom edge. */
        uint16_t arrow = rgb565(80, 200, 80);
        side[13 * CRAFT_TEX_SIZE + 6]  = arrow;
        side[13 * CRAFT_TEX_SIZE + 9]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 7]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 8]  = arrow;
        memcpy(&craft_textures[(BLK_NOT_GATE * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_NOT_GATE * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* DELAY — stone slab with a row of pip indicators showing the
     * current 1..4 tick setting. We bake the "1" variant; live re-
     * skin per cell uses the orient-hash byte to count pips. */
    {
        uint16_t *side = &craft_textures[(BLK_DELAY * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xDE1A, 140, 140, 145, 25);
        uint16_t pip = rgb565(220, 80, 60);
        /* Single lit pip at the centre (setting = 1). */
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                side[(8 + dy) * CRAFT_TEX_SIZE + (8 + dx)] = pip;
        /* Direction arrow on the bottom edge. */
        uint16_t arrow = rgb565(80, 200, 80);
        side[13 * CRAFT_TEX_SIZE + 6]  = arrow;
        side[13 * CRAFT_TEX_SIZE + 9]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 7]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 8]  = arrow;
        memcpy(&craft_textures[(BLK_DELAY * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_DELAY * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* DISPENSER — cobble body with a dark circular muzzle on every
     * face (close enough to read as "the hole shoots that way"). */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_DISPENSER * 3 + f) * CRAFT_TEX_PIXELS];
        cobble_pattern(t, 0xD15B + f);
        /* Dark muzzle disc centred at (8,8), radius ~4. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                if (dx*dx + dy*dy <= 16)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(25, 25, 28);
            }
    }

    /* TARGET — concentric red/white rings, dartboard style. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_TARGET * 3 + f) * CRAFT_TEX_PIXELS];
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                int r2 = dx*dx + dy*dy;
                uint16_t c;
                if      (r2 <= 4)   c = rgb565(220, 60, 50);
                else if (r2 <= 16)  c = rgb565(235, 230, 215);
                else if (r2 <= 36)  c = rgb565(220, 60, 50);
                else                c = rgb565(235, 230, 215);
                t[y * CRAFT_TEX_SIZE + x] = c;
            }
    }

    /* SLIME BLOCK — translucent-looking green with a darker inner
     * grid, a lighter speckle for the gel sheen. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_SLIME_BLOCK * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x511E0000u + (uint32_t)f, 110, 200, 110, 30);
        uint16_t grid = rgb565(70, 150, 70);
        for (int i = 0; i < CRAFT_TEX_SIZE; i++) {
            t[2 * CRAFT_TEX_SIZE + i] = grid;
            t[13 * CRAFT_TEX_SIZE + i] = grid;
            t[i * CRAFT_TEX_SIZE + 2] = grid;
            t[i * CRAFT_TEX_SIZE + 13] = grid;
        }
    }

    /* SLIMEBALL — item icon: a green blob with a highlight on a dark
     * backdrop (the engine has no texel transparency — every pixel
     * shows, so the tile is filled). */
    {
        uint16_t *t = &craft_textures[(BLK_SLIMEBALL * 3 + 1) * CRAFT_TEX_PIXELS];
        fill_solid(t, rgb565(28, 34, 28));
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                if (dx*dx + dy*dy <= 20)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(120, 210, 120);
                if (dx*dx + dy*dy <= 4)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(170, 240, 170);
            }
        memcpy(&craft_textures[(BLK_SLIMEBALL * 3 + 0) * CRAFT_TEX_PIXELS],
               t, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_SLIMEBALL * 3 + 2) * CRAFT_TEX_PIXELS],
               t, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* SNOW — snow over a dirt/grass band (white top, snow-capped dirt
     * sides). The "snow dusting on ground" look for the taiga. Mountain
     * caps use BLK_SNOWY_ROCK instead so they don't get a dirt band. */
    {
        uint16_t *top = &craft_textures[(BLK_SNOW * 3 + 0) * CRAFT_TEX_PIXELS];
        speckle(top, 0x5009u, 235, 240, 250, 12);
        uint16_t *side = &craft_textures[(BLK_SNOW * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x5102, 120, 90, 70, 18);   /* dirt base */
        for (int y = 0; y < 5; y++)               /* snow cap on top rows */
            for (int x = 0; x < CRAFT_TEX_SIZE; x++)
                side[y * CRAFT_TEX_SIZE + x] = rgb565(235, 240, 250);
        memcpy(&craft_textures[(BLK_SNOW * 3 + 2) * CRAFT_TEX_PIXELS],
               &craft_textures[(BLK_DIRT * 3 + 1) * CRAFT_TEX_PIXELS],
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);   /* dirt bottom */
    }

    /* SNOWY ROCK — mountain cap: white snow top, grey stone sides with
     * a white snow rim along the upper edge, stone bottom. Reads as
     * snow lying on rock (no dirt band). */
    {
        uint16_t *top = &craft_textures[(BLK_SNOWY_ROCK * 3 + 0) * CRAFT_TEX_PIXELS];
        speckle(top, 0x5A09u, 235, 240, 250, 12);
        uint16_t *side = &craft_textures[(BLK_SNOWY_ROCK * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x5A0Cu, 120, 122, 130, 22);   /* stone-grey base */
        for (int y = 0; y < 4; y++)                  /* snow rim on top rows */
            for (int x = 0; x < CRAFT_TEX_SIZE; x++)
                side[y * CRAFT_TEX_SIZE + x] = rgb565(232, 238, 248);
        memcpy(&craft_textures[(BLK_SNOWY_ROCK * 3 + 2) * CRAFT_TEX_PIXELS],
               &craft_textures[(BLK_STONE * 3 + 1) * CRAFT_TEX_PIXELS],
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);   /* stone bottom */
    }

    /* ICE — frozen sheet: warped-Voronoi cracked plates like the lava
     * tile but static and blue (mid-blue plates, bright frosty seams).
     * Per-cell offset/flip in the renderer keeps a lake from looking
     * tiled. Same tile on all faces. */
    {
        uint16_t *t0 = &craft_textures[(BLK_ICE * 3 + 0) * CRAFT_TEX_PIXELS];
        bake_ice_tile(t0);
        memcpy(&craft_textures[(BLK_ICE * 3 + 1) * CRAFT_TEX_PIXELS], t0,
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_ICE * 3 + 2) * CRAFT_TEX_PIXELS], t0,
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LAVA / PORTAL — initial (frame 0) tiles on all faces. The live
     * tiles are re-baked every frame by craft_blocks_animate_water; this
     * just seeds the slots so they're never uninitialised before the
     * first animate call. */
    for (int f = 0; f < 3; f++) {
        bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + f) * CRAFT_TEX_PIXELS], 0);
        bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + f) * CRAFT_TEX_PIXELS], 0);
    }

    /* GRAVEL — grey speckle with scattered darker pebbles, all faces. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_GRAVEL * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x6AA7E1u + f, 140, 138, 134, 40);
        uint32_t s = 0x6AA7E1u ^ (f * 0x1000u);
        for (int n = 0; n < 14; n++) {            /* darker pebbles */
            int cx = (int)(xs32(&s) & 15);
            int cy = (int)(xs32(&s) & 15);
            t[cy * CRAFT_TEX_SIZE + cx] = rgb565(92, 90, 86);
        }
    }

    /* FLINT — item icon only (never a world cell): dark grey shard. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_FLINT * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0xF11A7u + f, 60, 60, 66, 18);
    }

    /* OBSIDIAN — near-black with a faint purple sheen + sparse brighter
     * violet flecks, all faces alike. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_OBSIDIAN * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x0B51D1u + f, 26, 18, 38, 14);   /* dark purple-black */
        uint32_t s = 0x0B51D1u ^ (f * 0x1000u);
        for (int n = 0; n < 10; n++) {               /* sparse violet flecks */
            int cx = (int)(xs32(&s) & 15);
            int cy = (int)(xs32(&s) & 15);
            t[cy * CRAFT_TEX_SIZE + cx] = rgb565(78, 54, 110);
        }
    }

    /* SANDSTONE — tan with faint horizontal banding. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_SANDSTONE * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x5A5Du + f, 210, 195, 150, 14);
        for (int y = 0; y < CRAFT_TEX_SIZE; y += 5)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++)
                t[y * CRAFT_TEX_SIZE + x] = rgb565(185, 170, 128);
    }

    /* CACTUS — green with darker vertical ribs and a notched top. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_CACTUS * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0xCAC0u + f, 60, 130, 60, 16);
        if (f == 1) {                              /* side ribs */
            for (int x = 3; x < CRAFT_TEX_SIZE; x += 5)
                for (int y = 0; y < CRAFT_TEX_SIZE; y++)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(35, 90, 40);
        }
    }

    /* VINE — leafy cutout (two winding stems + leaves, magenta = holes),
     * vertically tiling so a multi-cell vine flows unbroken. Rendered by
     * the DDA CROSS path now (crossed quads), not the post-pass. */
    {
        const int Sv = CRAFT_TEX_SIZE;
        uint8_t tier[CRAFT_TEX_PIXELS];
        for (int i = 0; i < Sv * Sv; i++) tier[i] = 0;
        const float vcx[2] = { 4.0f, 11.0f }, vph[2] = { 0.0f, 2.0f };
        const float vamp = 1.5f;
        #define VPUT(xx,yy,vv) do { int _x=(xx), _y=(((yy)%Sv)+Sv)%Sv; \
            if (_x>=0 && _x<Sv && (vv) > tier[_y*Sv+_x]) tier[_y*Sv+_x]=(vv); } while(0)
        for (int s = 0; s < 2; s++) {
            for (int y = 0; y < Sv; y++) {
                float fx = vcx[s] + vamp * sinf(6.2831853f * (float)y / (float)Sv + vph[s]);
                int x = (int)(fx + 0.5f);
                VPUT(x, y, 2); VPUT(x, y + 1, 2);
            }
            for (int y = 0; y < Sv; y += 3) {
                float fx = vcx[s] + vamp * sinf(6.2831853f * (float)y / (float)Sv + vph[s]);
                int x = (int)(fx + 0.5f);
                int side = ((y / 3) & 1) ? 1 : -1, lx = x + side * 2;
                for (int dy = -2; dy <= 1; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int adx = dx < 0 ? -dx : dx, ady2 = (dy < 0 ? -dy : dy) * 2;
                        if (adx + ady2 <= 2) VPUT(lx + dx, y + dy, 1);
                    }
        }
        #undef VPUT
        }
        for (int f = 0; f < 3; f++) {
            uint16_t *t = &craft_textures[(BLK_VINE * 3 + f) * CRAFT_TEX_PIXELS];
            for (int i = 0; i < Sv * Sv; i++) {
                if (tier[i] == 0) { t[i] = rgb565(255, 0, 255); continue; }
                uint32_t h = (uint32_t)(((i % Sv) + 1) * 73856093) ^
                             (uint32_t)(((i / Sv) + 1) * 19349663);
                h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
                int j = (int)(h & 7) - 3, r, g, b;
                if (tier[i] == 2) { r = 40; g = 88; b = 34; }
                else              { r = 66; g = 138; b = 52; }
                r += j; g += j * 3; b += j;
                if (r < 0) r = 0; if (b < 0) b = 0;
                if (g < 0) g = 0; if (g > 200) g = 200;
                t[i] = rgb565(r, g, b);
            }
        }
    }

    /* LILY PAD — drawn in-world as a flat sprite slab; this tile is
     * the hotbar icon: a green disc on a water-blue backdrop. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_LILY_PAD * 3 + f) * CRAFT_TEX_PIXELS];
        fill_solid(t, rgb565(40, 80, 110));
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                if (dx*dx + dy*dy <= 42 && !(dx > 0 && dy > 0 && dx + dy > 6))
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(70, 150, 70);
            }
    }

    /* --- Cross-sprite plants (cutout) --------------------------------
     * Magenta (rgb565 255,0,255) texels are transparent: the DDA
     * cutout-CROSS path traces straight through them. Baked into all
     * three face slots identically (the renderer samples the side
     * slot). TALL_GRASS is neutral green and gets biome-tinted at
     * render time; flowers keep their own bloom colours. */
    {
        const uint16_t KEY = rgb565(255, 0, 255);
        const int S = CRAFT_TEX_SIZE;

        /* TALL GRASS — three blade-tuft variants baked into the 3 slots
         * (0 = light-tips, 1 = seed-heads, 2 = half-height). The cutout
         * renderer picks a slot per cell, weighted by biome temperature
         * (warmer → seedier). Clean single-px vertical blades that fan
         * out and brighten toward the tips; magenta = transparent. */
        {
            static const int vbx[3][6] = {{3,6,8,10,13,0},{4,6,8,10,12,0},{3,5,7,9,11,13}};
            static const int vln[3][6] = {{-2,-1,0,1,2,0},{-2,-1,0,1,2,0},{-3,-2,-1,1,2,3}};
            static const int vtp[3][6] = {{6,3,1,3,6,0},{7,4,3,4,7,0},{10,8,7,8,9,11}};
            static const int vnb[3]    = {5,5,6};
            for (int slot = 0; slot < 3; slot++) {
                uint16_t *t = &craft_textures[(BLK_TALL_GRASS * 3 + slot) * CRAFT_TEX_PIXELS];
                for (int i = 0; i < S * S; i++) t[i] = KEY;
                for (int b = 0; b < vnb[slot]; b++) {
                    int top = vtp[slot][b];
                    for (int y = S - 1; y >= top; y--) {
                        int up = (S - 1) - y;
                        int x = vbx[slot][b] + (vln[slot][b] * up) / 6;
                        if (x < 0 || x >= S) continue;
                        float f = 0.78f + 0.025f * up; if (f > 1.25f) f = 1.25f;
                        int g = (int)(140 * f); if (g > 200) g = 200;
                        t[y * S + x] = rgb565((int)(44 * f), g, (int)(40 * f));
                    }
                    /* seed head on the inner blades of variant 1 */
                    if (slot == 1 && b != 0 && b != 4) {
                        int x = vbx[slot][b] + (vln[slot][b] * (S - 1 - top)) / 6;
                        if (x >= 0 && x < S) {
                            uint16_t hi = rgb565(210,215,70), md = rgb565(180,190,60);
                            if (top-1 >= 0) t[(top-1)*S + x] = hi;
                            if (top-2 >= 0) t[(top-2)*S + x] = hi;
                            if (top-3 >= 0) t[(top-3)*S + x] = hi;
                            if (top-4 >= 0) t[(top-4)*S + x] = md;
                            if (x-1 >= 0 && top-2 >= 0) t[(top-2)*S + x-1] = md;
                            if (x+1 <  S && top-3 >= 0) t[(top-3)*S + x+1] = md;
                        }
                    }
                }
            }
        }

        /* FLOWERS — hand-placed, natural silhouettes (no lollypop discs).
         * RED is a tulip: a cup that flares into three lobed petals.
         * YELLOW is a daisy: petals radiating around an orange core with
         * magenta gaps between them. Both sit on a thin stem with two
         * small angled leaves, shaded with a few soft tones. */
        #define PX(tt,xx,yy,cc) do { if ((xx)>=0 && (xx)<S && (yy)>=0 && (yy)<S) \
                                       (tt)[(yy)*S+(xx)] = (cc); } while (0)
        {
            uint16_t stem  = rgb565(72, 122, 56), stemd = rgb565(52, 96, 44);
            uint16_t leaf  = rgb565(86, 144, 62), leafd = rgb565(60, 112, 50);
            /* Curved stems (x for each row 15..7) so flowers lean and
             * arc rather than standing as a dead-straight stick. The two
             * flowers curve opposite ways for variety. Index 0 = bottom
             * row (y=15) up to index 8 (y=7); the bloom sits at the top. */
            const int stemTulip[9] = { 8, 8, 7, 7, 6, 6, 6, 7, 7 }; /* arcs left */
            const int stemDaisy[9] = { 8, 8, 9, 9, 9, 8, 8, 8, 8 }; /* leans right */
            for (int pass = 0; pass < 2; pass++) {
                BlockId blk = pass ? BLK_FLOWER_YELLOW : BLK_FLOWER_RED;
                const int *sc = pass ? stemDaisy : stemTulip;
                for (int f = 0; f < 3; f++) {
                    uint16_t *t = &craft_textures[(blk * 3 + f) * CRAFT_TEX_PIXELS];
                    for (int i = 0; i < S * S; i++) t[i] = KEY;
                    /* Stem follows the curve; shade alternates for body. */
                    for (int i = 0; i < 9; i++) {
                        int y = 15 - i;
                        PX(t, sc[i], y, (i & 1) ? stem : stemd);
                    }
                    if (!pass) {
                        /* RED TULIP — cup centred on the stem top (x≈7),
                         * tilted, flaring into three lobes. Leaves tuck
                         * into the arc of the stem. */
                        PX(t, 8, 12, leaf); PX(t, 9, 12, leafd);   /* leaf out of the bend */
                        PX(t, 5, 9,  leaf); PX(t, 4, 9,  leafd);
                        uint16_t br = rgb565(238, 96, 98);
                        uint16_t md = rgb565(210, 58, 62);
                        uint16_t dp = rgb565(165, 36, 46);
                        PX(t,6,6,dp); PX(t,7,6,dp); PX(t,8,6,dp);                 /* base */
                        PX(t,5,5,dp); PX(t,6,5,md); PX(t,7,5,md); PX(t,8,5,md); PX(t,9,5,dp);
                        PX(t,5,4,md); PX(t,6,4,md); PX(t,7,4,md); PX(t,8,4,md); PX(t,9,4,md);
                        PX(t,5,3,md); PX(t,6,3,br); PX(t,7,3,md); PX(t,8,3,br); PX(t,9,3,md);
                        PX(t,5,2,br); PX(t,7,2,br); PX(t,9,2,br);                 /* 3 lobe tips */
                    } else {
                        /* YELLOW DAISY — radiating petals + orange core,
                         * sitting atop the right-leaning stem (x≈8). */
                        PX(t, 7, 12, leaf); PX(t, 6, 12, leafd);
                        PX(t, 10, 10, leaf); PX(t, 11, 10, leafd);
                        uint16_t pet = rgb565(250, 224, 92);
                        uint16_t tip = rgb565(255, 244, 156);
                        uint16_t c0  = rgb565(220, 146, 40);
                        uint16_t cd  = rgb565(156, 96, 24);
                        PX(t,8,1,tip);  PX(t,8,2,pet);                 /* N  */
                        PX(t,6,2,pet);  PX(t,10,2,pet);                /* NW NE */
                        PX(t,5,4,tip);  PX(t,6,4,pet);                 /* W  */
                        PX(t,10,4,pet); PX(t,11,4,tip);                /* E  */
                        PX(t,6,6,pet);  PX(t,10,6,pet);                /* SW SE */
                        PX(t,8,6,pet);                                 /* S  */
                        PX(t,8,3,pet);  PX(t,7,4,c0); PX(t,9,4,c0); PX(t,8,5,pet);
                        PX(t,8,4,cd);                                  /* core */
                    }
                }
            }
        }
        #undef PX
    }

    /* FLOWER_VINE — jungle dangling vine: two winding green stems +
     * leaves (biome-tinted at render) with white blossom blobs. The
     * cutout CROSS renderer recolours the white texels per-cluster to
     * one of four bloom colours (pink/red/purple/orange). Tiles
     * vertically so a hanging run flows unbroken. */
    {
        const uint16_t KEY = rgb565(255, 0, 255);
        const uint16_t WHT = rgb565(255, 255, 255);
        const int S = CRAFT_TEX_SIZE;
        const float vcx[2] = { 4.0f, 11.0f }, vph[2] = { 0.0f, 2.0f };
        const float vamp = 1.5f;
        for (int f = 0; f < 3; f++) {
            uint16_t *t = &craft_textures[(BLK_FLOWER_VINE * 3 + f) * CRAFT_TEX_PIXELS];
            for (int i = 0; i < S * S; i++) t[i] = KEY;
            #define FVP(xx,yy,cc) do { int _x=(xx), _y=((((yy))%S)+S)%S; \
                if (_x>=0 && _x<S) t[_y*S+_x]=(cc); } while(0)
            for (int s = 0; s < 2; s++) {
                for (int y = 0; y < S; y++) {
                    int x = (int)(vcx[s] + vamp * sinf(6.2831853f*(float)y/(float)S + vph[s]) + 0.5f);
                    FVP(x, y, rgb565(40,110,40)); FVP(x, y+1, rgb565(40,110,40));
                }
                for (int y = 1; y < S; y += 4) {
                    int x = (int)(vcx[s] + vamp * sinf(6.2831853f*(float)y/(float)S + vph[s]) + 0.5f);
                    int side = ((y/4)&1) ? 1 : -1;
                    FVP(x+side, y, rgb565(60,135,55)); FVP(x+side*2, y, rgb565(60,135,55));
                }
            }
            for (int s = 0; s < 2; s++)
                for (int y = 2; y < S; y += 5) {
                    int x = (int)(vcx[s] + vamp * sinf(6.2831853f*(float)y/(float)S + vph[s]) + 0.5f);
                    int bx = x + ((y&1) ? 2 : -2);
                    FVP(bx, y, WHT); FVP(bx+1, y, WHT); FVP(bx, y+1, WHT);
                    FVP(bx+1, y+1, WHT); FVP(bx-1, y, WHT); FVP(bx, y-1, WHT);
                }
            #undef FVP
        }
    }

    /* BLOSSOM_LEAVES — leafy green cutout (like leaves) with white
     * blossom clusters; the CUBE cutout renderer recolours white texels
     * per-tree to a bloom colour, green is biome-tinted. */
    {
        const uint16_t WHT = rgb565(255, 255, 255);
        const int S = CRAFT_TEX_SIZE;
        for (int f = 0; f < 3; f++) {
            uint16_t *t = &craft_textures[(BLK_BLOSSOM_LEAVES * 3 + f) * CRAFT_TEX_PIXELS];
            leaves_pattern(t, 0xB105u + (uint32_t)f);
            for (int k = 0; k < 5; k++) {
                uint32_t h = leaf_hash(k + 7, f * 13 + 3);
                int cx = (int)(h % (uint32_t)S), cy = (int)((h >> 8) % (uint32_t)S);
                #define BLP(xx,yy) do { int _x=((((xx))%S)+S)%S, _y=((((yy))%S)+S)%S; t[_y*S+_x]=WHT; } while(0)
                BLP(cx, cy); BLP(cx+1, cy); BLP(cx, cy+1);
                BLP(cx+1, cy+1); BLP(cx-1, cy); BLP(cx, cy-1);
                #undef BLP
            }
        }
    }
}
#endif /* CRAFT_TEXTURES_BAKED */
