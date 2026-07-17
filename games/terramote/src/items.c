/* TerraMote — item definitions + crafting recipes (the tech tree). */
#include "terra.h"

/* icon = cell index into the items sheet (assets/items.png, 12px grid).
 * Icons are laid out by make_sprites.py in ITEM ID ORDER, so icon == id. */
#define IC(id) (id)

const ItemDef g_items[I_COUNT] = {
    [I_NONE]        = { "",             IK_NONE,     0, 0, 0, 0, 0, 0 },
    /* blocks */
    [I_DIRT]        = { "DIRT",         IK_BLOCK, IC(I_DIRT),      T_DIRT,      0, 0, 99, 8 },
    [I_STONE]       = { "STONE",        IK_BLOCK, IC(I_STONE),     T_STONE,     0, 0, 99, 8 },
    [I_WOOD]        = { "WOOD",         IK_BLOCK, IC(I_WOOD),      T_WOOD,      0, 0, 99, 8 },
    [I_SAND]        = { "SAND",         IK_BLOCK, IC(I_SAND),      T_SAND,      0, 0, 99, 8 },
    [I_SNOW]        = { "SNOW",         IK_BLOCK, IC(I_SNOW),      T_SNOW,      0, 0, 99, 8 },
    [I_EBON]        = { "EBONSTONE",    IK_BLOCK, IC(I_EBON),      T_EBON,      0, 0, 99, 8 },
    [I_CLAY]        = { "CLAY",         IK_BLOCK, IC(I_CLAY),      T_CLAY,      0, 0, 99, 8 },
    [I_ASH]         = { "ASH",          IK_BLOCK, IC(I_ASH),       T_ASH,       0, 0, 99, 8 },
    [I_HELLSTONE]   = { "HELLSTONE",    IK_BLOCK, IC(I_HELLSTONE), T_HELLSTONE, 0, 0, 99, 8 },
    [I_OBSIDIAN]    = { "OBSIDIAN",     IK_BLOCK, IC(I_OBSIDIAN),  T_OBSIDIAN,  0, 0, 99, 8 },
    [I_TORCH]       = { "TORCH",        IK_BLOCK, IC(I_TORCH),     T_TORCH,     0, 0, 99, 6 },
    [I_PLATFORM]    = { "PLATFORM",     IK_BLOCK, IC(I_PLATFORM),  T_PLATFORM,  0, 0, 99, 6 },
    [I_WORKBENCH]   = { "WORKBENCH",    IK_BLOCK, IC(I_WORKBENCH), T_WORKBENCH, 0, 0, 99, 8 },
    [I_FURNACE]     = { "FURNACE",      IK_BLOCK, IC(I_FURNACE),   T_FURNACE,   0, 0, 99, 8 },
    [I_ANVIL]       = { "ANVIL",        IK_BLOCK, IC(I_ANVIL),     T_ANVIL,     0, 0, 99, 8 },
    [I_CHEST]       = { "CHEST",        IK_BLOCK, IC(I_CHEST),     T_CHEST,     0, 0, 99, 8 },
    [I_DOOR]        = { "DOOR",         IK_BLOCK, IC(I_DOOR),      T_DOOR_C,    0, 0, 99, 8 },
    [I_ACORN]       = { "ACORN",        IK_BLOCK, IC(I_ACORN),     T_SAPLING,   0, 0, 99, 8 },
    /* materials */
    [I_GEL]         = { "GEL",          IK_MATERIAL, IC(I_GEL),       0, 0, 0, 99, 0 },
    [I_LENS]        = { "LENS",         IK_MATERIAL, IC(I_LENS),      0, 0, 0, 99, 0 },
    [I_MUSHROOM]    = { "GLOWSHROOM",   IK_MATERIAL, IC(I_MUSHROOM),  0, 0, 0, 99, 0 },
    [I_COIN]        = { "GOLD COIN",    IK_MATERIAL, IC(I_COIN),      0, 0, 0, 99, 0 },
    [I_COPPER_ORE]  = { "COPPER ORE",   IK_MATERIAL, IC(I_COPPER_ORE),0, 0, 0, 99, 0 },
    [I_IRON_ORE]    = { "IRON ORE",     IK_MATERIAL, IC(I_IRON_ORE),  0, 0, 0, 99, 0 },
    [I_GOLD_ORE]    = { "GOLD ORE",     IK_MATERIAL, IC(I_GOLD_ORE),  0, 0, 0, 99, 0 },
    [I_DEMONITE_ORE]= { "DEMONITE ORE", IK_MATERIAL, IC(I_DEMONITE_ORE),0,0,0, 99, 0 },
    [I_COPPER_BAR]  = { "COPPER BAR",   IK_MATERIAL, IC(I_COPPER_BAR),0, 0, 0, 99, 0 },
    [I_IRON_BAR]    = { "IRON BAR",     IK_MATERIAL, IC(I_IRON_BAR),  0, 0, 0, 99, 0 },
    [I_GOLD_BAR]    = { "GOLD BAR",     IK_MATERIAL, IC(I_GOLD_BAR),  0, 0, 0, 99, 0 },
    [I_DEMONITE_BAR]= { "DEMONITE BAR", IK_MATERIAL, IC(I_DEMONITE_BAR),0,0,0, 99, 0 },
    [I_HELL_BAR]    = { "HELLSTONE BAR",IK_MATERIAL, IC(I_HELL_BAR),  0, 0, 0, 99, 0 },
    /* picks: power gates tiles, speed = swing frames */
    [I_PICK_WOOD]     = { "WOOD PICK",      IK_PICK, IC(I_PICK_WOOD),     0,  35, 4, 1, 22 },
    [I_PICK_COPPER]   = { "COPPER PICK",    IK_PICK, IC(I_PICK_COPPER),   0,  50, 5, 1, 19 },
    [I_PICK_IRON]     = { "IRON PICK",      IK_PICK, IC(I_PICK_IRON),     0,  65, 6, 1, 16 },
    [I_PICK_GOLD]     = { "GOLD PICK",      IK_PICK, IC(I_PICK_GOLD),     0,  80, 7, 1, 13 },
    [I_PICK_NIGHTMARE]= { "NIGHTMARE PICK", IK_PICK, IC(I_PICK_NIGHTMARE),0, 110, 9, 1, 10 },
    [I_AXE_WOOD]      = { "WOOD AXE",       IK_AXE,  IC(I_AXE_WOOD),      0,  35, 5, 1, 20 },
    [I_AXE_IRON]      = { "IRON AXE",       IK_AXE,  IC(I_AXE_IRON),      0,  70, 7, 1, 15 },
    /* swords */
    [I_SWORD_WOOD]   = { "WOOD SWORD",   IK_SWORD, IC(I_SWORD_WOOD),   0, 0,  8, 1, 16 },
    [I_SWORD_COPPER] = { "COPPER SWORD", IK_SWORD, IC(I_SWORD_COPPER), 0, 0, 11, 1, 15 },
    [I_SWORD_IRON]   = { "IRON SWORD",   IK_SWORD, IC(I_SWORD_IRON),   0, 0, 14, 1, 14 },
    [I_SWORD_GOLD]   = { "GOLD SWORD",   IK_SWORD, IC(I_SWORD_GOLD),   0, 0, 18, 1, 13 },
    [I_SWORD_BANE]   = { "LIGHTS BANE",  IK_SWORD, IC(I_SWORD_BANE),   0, 0, 23, 1, 12 },
    [I_SWORD_VOLCANO]= { "VOLCANO",      IK_SWORD, IC(I_SWORD_VOLCANO),0, 0, 32, 1, 12 },
    /* bows + ammo */
    [I_BOW_WOOD]   = { "WOOD BOW",   IK_BOW, IC(I_BOW_WOOD),   0, 0,  7, 1, 18 },
    [I_BOW_GOLD]   = { "GOLD BOW",   IK_BOW, IC(I_BOW_GOLD),   0, 0, 12, 1, 15 },
    [I_BOW_MOLTEN] = { "MOLTEN FURY", IK_BOW, IC(I_BOW_MOLTEN),0, 0, 19, 1, 13 },
    [I_ARROW]       = { "ARROW",        IK_AMMO, IC(I_ARROW),       0, 0, 3, 99, 0 },
    [I_ARROW_FLAME] = { "FLAME ARROW",  IK_AMMO, IC(I_ARROW_FLAME), 0, 0, 6, 99, 0 },
    /* armor: power = defense */
    [I_HELM_COPPER] = { "COPPER HELM",  IK_ARMOR_HEAD, IC(I_HELM_COPPER), 0, 1, 0, 1, 0 },
    [I_MAIL_COPPER] = { "COPPER MAIL",  IK_ARMOR_BODY, IC(I_MAIL_COPPER), 0, 2, 0, 1, 0 },
    [I_LEGS_COPPER] = { "COPPER GREAVES",IK_ARMOR_LEGS,IC(I_LEGS_COPPER), 0, 1, 0, 1, 0 },
    [I_HELM_IRON]   = { "IRON HELM",    IK_ARMOR_HEAD, IC(I_HELM_IRON),   0, 2, 0, 1, 0 },
    [I_MAIL_IRON]   = { "IRON MAIL",    IK_ARMOR_BODY, IC(I_MAIL_IRON),   0, 3, 0, 1, 0 },
    [I_LEGS_IRON]   = { "IRON GREAVES", IK_ARMOR_LEGS, IC(I_LEGS_IRON),   0, 2, 0, 1, 0 },
    [I_HELM_GOLD]   = { "GOLD HELM",    IK_ARMOR_HEAD, IC(I_HELM_GOLD),   0, 3, 0, 1, 0 },
    [I_MAIL_GOLD]   = { "GOLD MAIL",    IK_ARMOR_BODY, IC(I_MAIL_GOLD),   0, 4, 0, 1, 0 },
    [I_LEGS_GOLD]   = { "GOLD GREAVES", IK_ARMOR_LEGS, IC(I_LEGS_GOLD),   0, 3, 0, 1, 0 },
    [I_HELM_MOLTEN] = { "MOLTEN HELM",  IK_ARMOR_HEAD, IC(I_HELM_MOLTEN), 0, 5, 0, 1, 0 },
    [I_MAIL_MOLTEN] = { "MOLTEN MAIL",  IK_ARMOR_BODY, IC(I_MAIL_MOLTEN), 0, 6, 0, 1, 0 },
    [I_LEGS_MOLTEN] = { "MOLTEN GREAVES",IK_ARMOR_LEGS,IC(I_LEGS_MOLTEN), 0, 5, 0, 1, 0 },
    /* consumables */
    [I_POTION_HEAL]    = { "HEAL POTION",   IK_CONSUME, IC(I_POTION_HEAL),    0, 50, 0, 30, 20 },
    [I_SUSPICIOUS_EYE] = { "SUSPICIOUS EYE",IK_CONSUME, IC(I_SUSPICIOUS_EYE), 0,  0, 0,  3, 20 },
    [I_LIFE_CRYSTAL]   = { "LIFE CRYSTAL",  IK_CONSUME, IC(I_LIFE_CRYSTAL),   0, 20, 0,  9, 20 },
    [I_GRAPPLE]        = { "GRAPPLING HOOK",IK_GRAPPLE, IC(I_GRAPPLE),        0,  0, 0,  1, 0 },
    [I_TABLE]          = { "TABLE",         IK_BLOCK, IC(I_TABLE),     T_TABLE,     0, 0, 99, 8 },
    [I_CHAIR]          = { "CHAIR",         IK_BLOCK, IC(I_CHAIR),     T_CHAIR,     0, 0, 99, 8 },
    [I_LANTERN]        = { "LANTERN",       IK_BLOCK, IC(I_LANTERN),   T_LANTERN,   0, 0, 99, 8 },
    [I_FIREPLACE]      = { "FIREPLACE",     IK_BLOCK, IC(I_FIREPLACE), T_FIREPLACE, 0, 0, 99, 8 },
    [I_CHAIN]          = { "CHAIN",         IK_BLOCK, IC(I_CHAIN),     T_CHAIN,     0, 0, 99, 8 },
    [I_WALL_WOOD]      = { "WOOD BACK WALL",     IK_WALL,  IC(I_WALL_WOOD),  W_WOOD,     0, 0, 99, 6 },
    [I_WALL_STONE]     = { "STONE BACK WALL",    IK_WALL,  IC(I_WALL_STONE), W_STONE,    0, 0, 99, 6 },
    [I_ROOF]           = { "ROOF",          IK_BLOCK, IC(I_ROOF),       T_ROOF,     0, 0, 99, 8 },
    [I_BEAM]           = { "WOOD BEAM",     IK_BLOCK, IC(I_BEAM),       T_BEAM,     0, 0, 99, 8 },
    [I_BRICK_CLAY]     = { "CLAY BRICK",    IK_BLOCK, IC(I_BRICK_CLAY), T_BRICK_CLAY, 0, 0, 99, 8 },
    [I_BRICK_STONE]    = { "STONE BRICK",   IK_BLOCK, IC(I_BRICK_STONE),T_BRICK_STONE,0, 0, 99, 8 },
    [I_WALL_CLAYBRICK] = { "CLAY BRICK BACK WALL", IK_WALL, IC(I_WALL_CLAYBRICK), W_CLAYBRICK, 0, 0, 99, 6 },
    [I_WALL_STONEBRICK]= { "STONE BRICK BACK WALL",IK_WALL, IC(I_WALL_STONEBRICK),W_STONEBRICK,0, 0, 99, 6 },
    [I_WALL_GLASS]     = { "GLASS BACK WALL",    IK_WALL,  IC(I_WALL_GLASS), W_GLASS,    0, 0, 99, 6 },
#include "weapon_defs.inc"                              /* GENERATED weapon variants */
};

/* per-weapon combat properties (element / knockback / reach / multishot / spread).
 * Anything unset is a plain weapon. */
const WeaponFx g_wfx[I_COUNT] = {
    [I_SWORD_GOLD]    = { EL_NONE,    150, 1, 0, 0 },
    [I_SWORD_BANE]    = { EL_DEMONIC, 140, 1, 0, 0 },   /* lifesteal */
    [I_SWORD_VOLCANO] = { EL_FIRE,    165, 2, 0, 0 },   /* burn DoT + big knockback + reach */
    [I_BOW_GOLD]      = { EL_NONE,      0, 0, 2, 14 },  /* double shot */
    [I_BOW_MOLTEN]    = { EL_FIRE,      0, 0, 3, 26 },  /* triple fire shot */
#include "weapon_fx.inc"                                /* GENERATED weapon variants */
};

/* ---------------------------------------------------------------- tiles ----
 * solid: 0 pass, 1 solid, 2 one-way platform.
 * hardness: mine-time scale (0 = unbreakable). min_power: pick gate.
 * axe: chopped by axes instead of picks. light: emitted 0..15. */
const TileDef g_tiles[T_COUNT] = {
    /*                 solid hard minpow drop           light axe */
    [T_AIR]        = { 0, 0,   0, I_NONE,         0, 0 },
    [T_DIRT]       = { 1, 2,   0, I_DIRT,         0, 0 },
    [T_GRASS]      = { 1, 2,   0, I_DIRT,         0, 0 },
    [T_STONE]      = { 1, 5,   0, I_STONE,        0, 0 },
    [T_WOOD]       = { 1, 3,   0, I_WOOD,         0, 0 },
    [T_TRUNK]      = { 0, 4,   0, I_WOOD,         0, 1 },
    [T_LEAF]       = { 0, 1,   0, I_NONE,         0, 1 },
    [T_SAND]       = { 1, 2,   0, I_SAND,         0, 0 },
    [T_SNOW]       = { 1, 2,   0, I_SNOW,         0, 0 },
    [T_EBON]       = { 1, 7,  65, I_EBON,         0, 0 },
    [T_CLAY]       = { 1, 2,   0, I_CLAY,         0, 0 },
    [T_COPPER]     = { 1, 5,   0, I_COPPER_ORE,   0, 0 },
    [T_IRON]       = { 1, 6,   0, I_IRON_ORE,     0, 0 },
    [T_GOLD]       = { 1, 6,  50, I_GOLD_ORE,     1, 0 },
    [T_DEMONITE]   = { 1, 7,  65, I_DEMONITE_ORE, 2, 0 },
    [T_ASH]        = { 1, 2,   0, I_ASH,          0, 0 },
    [T_HELLSTONE]  = { 1, 7, 100, I_HELLSTONE,    5, 0 },
    [T_OBSIDIAN]   = { 1, 8,  80, I_OBSIDIAN,     0, 0 },
    [T_TORCH]      = { 0, 1,   0, I_TORCH,       14, 1 },   /* axe removes it */
    [T_WORKBENCH]  = { 0, 3,   0, I_WORKBENCH,    0, 1 },
    [T_FURNACE]    = { 0, 4,   0, I_FURNACE,      6, 1 },
    [T_ANVIL]      = { 0, 4,   0, I_ANVIL,        0, 1 },
    [T_CHEST]      = { 0, 3,   0, I_CHEST,        0, 1 },
    [T_DOOR_C]     = { 1, 3,   0, I_DOOR,         0, 1 },
    [T_DOOR_O]     = { 0, 3,   0, I_DOOR,         0, 1 },
    [T_PLATFORM]   = { 2, 1,   0, I_PLATFORM,     0, 1 },
    [T_ALTAR]      = { 0, 0,   0, I_NONE,         3, 0 },
    [T_MUSH]       = { 0, 1,   0, I_MUSHROOM,     7, 0 },
    [T_FLOWER]     = { 0, 1,   0, I_NONE,         0, 0 },
    [T_SAPLING]    = { 0, 1,   0, I_ACORN,        0, 1 },
    [T_TABLE]      = { 0, 2,   0, I_TABLE,        0, 1 },
    [T_CHAIR]      = { 0, 2,   0, I_CHAIR,        0, 1 },
    [T_LANTERN]    = { 0, 1,   0, I_LANTERN,     13, 1 },
    [T_FIREPLACE]  = { 0, 3,   0, I_FIREPLACE,   14, 1 },
    [T_CHAIN]      = { 0, 1,   0, I_CHAIN,        0, 1 },   /* axe removes it (was unremovable) */
    [T_ROOF]       = { 1, 2,   0, I_ROOF,         0, 1 },   /* axe-minable shingles */
    [T_BEAM]       = { 0, 1,   0, I_BEAM,         0, 1 },   /* walk-through support post */
    [T_BRICK_CLAY] = { 1, 3,   0, I_BRICK_CLAY,   0, 0 },
    [T_BRICK_STONE]= { 1, 3,   0, I_BRICK_STONE,  0, 0 },
};

/* ------------------------------------------------------------- recipes ----
 * The tech tree. Station ST_NONE = craftable anywhere ("by hand"). */
const Recipe g_recipes[] = {
    /* hand */
    { ST_NONE,      I_WORKBENCH, 1, { { I_WOOD, 10 } } },
    { ST_NONE,      I_TORCH, 3,      { { I_WOOD, 1 }, { I_GEL, 1 } } },
    /* workbench: wood age */
    { ST_WORKBENCH, I_PICK_WOOD, 1,  { { I_WOOD, 8 } } },
    { ST_WORKBENCH, I_AXE_WOOD, 1,   { { I_WOOD, 9 } } },
    { ST_WORKBENCH, I_SWORD_WOOD, 1, { { I_WOOD, 7 } } },
    { ST_WORKBENCH, I_BOW_WOOD, 1,   { { I_WOOD, 10 } } },
    { ST_WORKBENCH, I_ARROW, 5,      { { I_WOOD, 1 }, { I_STONE, 1 } } },
    { ST_WORKBENCH, I_PLATFORM, 2,   { { I_WOOD, 1 } } },
    { ST_WORKBENCH, I_DOOR, 1,       { { I_WOOD, 6 } } },
    { ST_WORKBENCH, I_CHEST, 1,      { { I_WOOD, 8 } } },
    { ST_WORKBENCH, I_FURNACE, 1,    { { I_STONE, 20 }, { I_WOOD, 4 }, { I_TORCH, 3 } } },
    { ST_WORKBENCH, I_POTION_HEAL, 1,{ { I_MUSHROOM, 1 }, { I_GEL, 2 } } },
    /* workbench: furniture */
    { ST_WORKBENCH, I_TABLE, 1,      { { I_WOOD, 8 } } },
    { ST_WORKBENCH, I_CHAIR, 1,      { { I_WOOD, 4 } } },
    { ST_WORKBENCH, I_LANTERN, 1,    { { I_WOOD, 2 }, { I_TORCH, 1 } } },
    { ST_WORKBENCH, I_FIREPLACE, 1,  { { I_STONE, 10 }, { I_WOOD, 4 }, { I_TORCH, 1 } } },
    { ST_ANVIL,     I_CHAIN, 3,      { { I_IRON_BAR, 1 } } },
    /* furnace: smelting */
    { ST_FURNACE,   I_COPPER_BAR, 1, { { I_COPPER_ORE, 3 } } },
    { ST_FURNACE,   I_IRON_BAR, 1,   { { I_IRON_ORE, 3 } } },
    { ST_FURNACE,   I_GOLD_BAR, 1,   { { I_GOLD_ORE, 4 } } },
    { ST_FURNACE,   I_DEMONITE_BAR, 1, { { I_DEMONITE_ORE, 3 } } },
    { ST_FURNACE,   I_HELL_BAR, 1,   { { I_HELLSTONE, 3 }, { I_OBSIDIAN, 1 } } },
    { ST_FURNACE,   I_ANVIL, 1,      { { I_IRON_BAR, 5 } } },
    /* anvil: the metal ages */
    { ST_ANVIL, I_GRAPPLE, 1,      { { I_IRON_BAR, 3 },    { I_GEL, 3 } } },   /* replace a lost hook */
    { ST_ANVIL, I_PICK_COPPER, 1,  { { I_COPPER_BAR, 8 },  { I_WOOD, 3 } } },
    { ST_ANVIL, I_SWORD_COPPER, 1, { { I_COPPER_BAR, 6 } } },
    { ST_ANVIL, I_HELM_COPPER, 1,  { { I_COPPER_BAR, 8 } } },
    { ST_ANVIL, I_MAIL_COPPER, 1,  { { I_COPPER_BAR, 10 } } },
    { ST_ANVIL, I_LEGS_COPPER, 1,  { { I_COPPER_BAR, 9 } } },
    { ST_ANVIL, I_PICK_IRON, 1,    { { I_IRON_BAR, 8 },    { I_WOOD, 3 } } },
    { ST_ANVIL, I_AXE_IRON, 1,     { { I_IRON_BAR, 7 },    { I_WOOD, 3 } } },
    { ST_ANVIL, I_SWORD_IRON, 1,   { { I_IRON_BAR, 6 } } },
    { ST_ANVIL, I_HELM_IRON, 1,    { { I_IRON_BAR, 8 } } },
    { ST_ANVIL, I_MAIL_IRON, 1,    { { I_IRON_BAR, 10 } } },
    { ST_ANVIL, I_LEGS_IRON, 1,    { { I_IRON_BAR, 9 } } },
    { ST_ANVIL, I_PICK_GOLD, 1,    { { I_GOLD_BAR, 8 },    { I_WOOD, 3 } } },
    { ST_ANVIL, I_SWORD_GOLD, 1,   { { I_GOLD_BAR, 6 } } },
    { ST_ANVIL, I_BOW_GOLD, 1,     { { I_GOLD_BAR, 6 } } },
    { ST_ANVIL, I_HELM_GOLD, 1,    { { I_GOLD_BAR, 8 } } },
    { ST_ANVIL, I_MAIL_GOLD, 1,    { { I_GOLD_BAR, 10 } } },
    { ST_ANVIL, I_LEGS_GOLD, 1,    { { I_GOLD_BAR, 9 } } },
    { ST_ANVIL, I_SWORD_BANE, 1,   { { I_DEMONITE_BAR, 8 } } },
    { ST_ANVIL, I_PICK_NIGHTMARE, 1, { { I_DEMONITE_BAR, 10 }, { I_WOOD, 4 } } },
    { ST_ANVIL, I_SWORD_VOLCANO, 1,  { { I_HELL_BAR, 10 } } },
    { ST_ANVIL, I_BOW_MOLTEN, 1,     { { I_HELL_BAR, 8 } } },
    { ST_ANVIL, I_ARROW_FLAME, 5,    { { I_ARROW, 5 }, { I_HELLSTONE, 1 } } },
    { ST_ANVIL, I_HELM_MOLTEN, 1,  { { I_HELL_BAR, 8 } } },
    { ST_ANVIL, I_MAIL_MOLTEN, 1,  { { I_HELL_BAR, 10 } } },
    { ST_ANVIL, I_LEGS_MOLTEN, 1,  { { I_HELL_BAR, 9 } } },
    /* demon altar */
    { ST_ALTAR, I_SUSPICIOUS_EYE, 1, { { I_LENS, 6 } } },
    /* building set */
    { ST_WORKBENCH, I_WALL_WOOD,  4, { { I_WOOD, 1 } } },
    { ST_WORKBENCH, I_WALL_STONE, 4, { { I_STONE, 1 } } },
    { ST_WORKBENCH, I_ROOF,       4, { { I_WOOD, 2 } } },
    { ST_WORKBENCH, I_BEAM,       2, { { I_WOOD, 1 } } },
    { ST_FURNACE, I_BRICK_CLAY,   1, { { I_CLAY, 2 } } },
    { ST_FURNACE, I_BRICK_STONE,  1, { { I_STONE, 2 } } },
    { ST_WORKBENCH, I_WALL_CLAYBRICK,  4, { { I_BRICK_CLAY, 1 } } },
    { ST_WORKBENCH, I_WALL_STONEBRICK, 4, { { I_BRICK_STONE, 1 } } },
    { ST_FURNACE, I_WALL_GLASS,   4, { { I_SAND, 1 } } },
#include "weapon_recipes.inc"                            /* GENERATED weapon variants */
};
const int g_nrecipes = (int)(sizeof(g_recipes) / sizeof(g_recipes[0]));
