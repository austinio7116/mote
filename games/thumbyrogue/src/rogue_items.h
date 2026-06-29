#ifndef ROGUE_ITEMS_H
#define ROGUE_ITEMS_H
/*
 * ThumbyRogue items — Diablo-4-lite. The hero wears a 6-slot paperdoll
 * (weapon/off-hand/helm/armor/amulet/ring); the WEAPON still defines the
 * attack style (melee/ranged/caster). Every equip rolls a rarity + affixes
 * (flat & % stats), legendaries carry a gameplay Aspect, and items may have
 * sockets for gems. All gear feeds an aggregated RogueStats.
 */
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SLOT_WEAPON, SLOT_OFFHAND, SLOT_HELM, SLOT_ARMOR, SLOT_AMULET, SLOT_RING,
    SLOT_COUNT
} EquipSlot;

typedef enum { WCLASS_MELEE, WCLASS_RANGED, WCLASS_CASTER } WeaponClass;

/* Concrete weapon bases — indexes WBASE[] in rogue_items.c. Each has its own
 * icon (draw_item_icon) and a distinct base-damage / reach / speed profile,
 * grouped melee → ranged → caster. */
typedef enum {
    WT_DAGGER, WT_SWORD, WT_GREATSWORD, WT_AXE, WT_MACE, WT_SPEAR, WT_WARHAMMER,
    WT_BOW, WT_CROSSBOW,
    WT_WAND, WT_SCEPTER, WT_STAFF,
    WT_COUNT
} WeaponType;
typedef enum { ITEM_NONE, ITEM_WEAPON, ITEM_GEAR, ITEM_GOLD, ITEM_POTION, ITEM_TORCH, ITEM_GEM } ItemKind;
typedef enum { RAR_COMMON, RAR_MAGIC, RAR_RARE, RAR_LEGENDARY, RAR_COUNT } Rarity;

/* Affixes — type + rolled magnitude. The elemental trio rolls on weapons
 * only and gives the weapon an element: extra burn damage (fire), a chilling
 * slow (frost) or a damage-over-time (poison) — and tints every impact and
 * projectile. (Appended at the end: affix ids live in saved items.) */
typedef enum {
    AFX_NONE, AFX_DMG, AFX_DMG_PCT, AFX_LIFE, AFX_ARMOR, AFX_CRIT,
    AFX_CRITDMG, AFX_ATKSPD, AFX_MOVESPD, AFX_LIFEONHIT, AFX_RESIST,
    AFX_FIRE, AFX_FROST, AFX_POISON, AFX_LIGHTNING, AFX_HOLY,
    AFX_SHADOW, AFX_VOID, AFX_ARCANE, AFX_COUNT
} AffixType;

/* Weapon element (from an elemental affix, or a gem socketed in the weapon:
 * ruby = fire, sapphire = frost, emerald = poison). */
typedef enum {
    ELEM_NONE, ELEM_FIRE, ELEM_FROST, ELEM_POISON,
    ELEM_LIGHTNING,   /* arcs to a second enemy nearby */
    ELEM_HOLY,        /* smites the undead (and demons) hard */
    ELEM_SHADOW,      /* drains life back to the hero */
    ELEM_VOID,        /* implodes — drags the victim toward the impact */
    ELEM_ARCANE,      /* force — a massive knockback shove */
} ElementId;
typedef struct { uint8_t type; int16_t val; } Affix;
#define MAX_AFFIX 3

/* Legendary aspects (gameplay powers). */
typedef enum {
    ASP_NONE, ASP_CHAIN, ASP_PIERCE, ASP_DARK, ASP_LIFESTEAL, ASP_THORNS,
    ASP_COUNT
} AspectId;

/* Socket gems. */
/* Socket gems. The classic four grant stats anywhere; the elemental trio
 * imbues a WEAPON with its element (and grants a little elemental warding
 * when socketed into armor instead). */
typedef enum {
    GEM_NONE, GEM_RUBY, GEM_SAPPHIRE, GEM_EMERALD, GEM_TOPAZ,
    GEM_FIREOPAL, GEM_GLACITE, GEM_VENOMSTONE, GEM_STORMCRYSTAL, GEM_SUNSTONE,
    GEM_NIGHTSTONE, GEM_VOIDPEARL, GEM_AETHERITE,
    GEM_COUNT
} GemType;

typedef struct {
    uint8_t  kind;        /* ItemKind */
    uint8_t  slot;        /* EquipSlot (equip kinds) */
    uint8_t  wclass;      /* WeaponClass (weapons) */
    uint8_t  wtype;       /* WeaponType  (weapons) — selects the icon */
    uint8_t  rarity;
    uint8_t  aspect;      /* AspectId (legendaries) */
    uint8_t  n_affix;
    Affix    affix[MAX_AFFIX];
    uint8_t  sockets;     /* number of gem sockets */
    uint8_t  gem[2];      /* GemType per socket */
    int16_t  base_dmg;    /* weapon base damage */
    int16_t  armor;       /* armor base (gear) */
    float    range, arc_cos, cooldown, proj_speed;  /* weapon attack */
    uint16_t color;
    int16_t  amount;      /* gold / potion heal / torch seconds */
    char     name[26];
} RogueItem;

uint16_t    rogue_rarity_color(Rarity r);
const char *rogue_slot_name(EquipSlot s);
const char *rogue_aspect_name(AspectId a);
const char *rogue_aspect_desc(AspectId a);
const char *rogue_gem_name(GemType g);
/* One-line affix label into buf (e.g. "+12 Life"). */
void        rogue_affix_label(char *buf, int n, const Affix *a);

static inline bool rogue_item_is_equip(const RogueItem *it) {
    return it->kind == ITEM_WEAPON || it->kind == ITEM_GEAR;
}

void rogue_item_make_gold(RogueItem *it, int amount);
void rogue_item_make_potion(RogueItem *it, int heal);
void rogue_item_make_torch(RogueItem *it, int seconds);
void rogue_item_make_gem(RogueItem *it, GemType g);
uint16_t rogue_gem_color(GemType g);
void rogue_item_starter(RogueItem *it);                       /* common dagger */
void rogue_item_roll_weapon(RogueItem *it, int depth, uint32_t seed);
void rogue_item_roll_gear(RogueItem *it, EquipSlot slot, int depth, uint32_t seed);
void rogue_item_roll_drop(RogueItem *it, int depth, uint32_t seed); /* random slot */

#endif /* ROGUE_ITEMS_H */
