#include "rogue_items.h"
#include <stdio.h>
#include <string.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

static uint32_t xs(uint32_t *s){ *s^=*s<<13; *s^=*s>>17; *s^=*s<<5; return *s; }
static int rr(uint32_t *s, int lo, int hi){ return hi<=lo?lo:lo+(int)(xs(s)%(uint32_t)(hi-lo+1)); }

/* --- weapon bases (slot WEAPON) ---------------------------------- */
typedef struct {
    const char *name; WeaponClass wclass; int dmg;
    float range, arc_cos, cooldown, proj_speed; uint16_t color;
} WeaponBase;
/* Order MUST match enum WeaponType (the index is stored as it->wtype and
 * selects the icon). Damage spans 14 (fast dagger) → 52 (warhammer);
 * faster weapons hit lighter, slow ones hit hard. arc_cos: higher = a
 * narrower swing cone; range is in tiles; proj_speed only for ranged/caster. */
static const WeaponBase WBASE[WT_COUNT] = {
    [WT_DAGGER]     = { "Dagger",     WCLASS_MELEE,  14, 1.3f, 0.50f, 0.20f, 0,    RGB(200,200,210) },
    [WT_SWORD]      = { "Sword",      WCLASS_MELEE,  24, 1.8f, 0.40f, 0.32f, 0,    RGB(210,215,225) },
    [WT_GREATSWORD] = { "Greatsword", WCLASS_MELEE,  46, 2.1f, 0.25f, 0.60f, 0,    RGB(220,225,235) },
    [WT_AXE]        = { "Axe",        WCLASS_MELEE,  36, 1.8f, 0.15f, 0.46f, 0,    RGB(180,150,90)  },
    [WT_MACE]       = { "Mace",       WCLASS_MELEE,  30, 1.7f, 0.35f, 0.42f, 0,    RGB(150,150,160) },
    [WT_SPEAR]      = { "Spear",      WCLASS_MELEE,  28, 2.6f, 0.70f, 0.40f, 0,    RGB(190,180,150) },
    [WT_WARHAMMER]  = { "Warhammer",  WCLASS_MELEE,  52, 1.7f, 0.30f, 0.66f, 0,    RGB(170,160,150) },
    [WT_BOW]        = { "Bow",        WCLASS_RANGED, 22, 14.f, 0.0f,  0.40f, 26.f, RGB(150,110,60)  },
    [WT_CROSSBOW]   = { "Crossbow",   WCLASS_RANGED, 34, 16.f, 0.0f,  0.62f, 32.f, RGB(140,120,90)  },
    [WT_WAND]       = { "Wand",       WCLASS_CASTER, 16, 12.f, 0.0f,  0.28f, 24.f, RGB(90,200,200)  },
    [WT_SCEPTER]    = { "Scepter",    WCLASS_CASTER, 26, 12.f, 0.0f,  0.44f, 22.f, RGB(220,180,90)  },
    [WT_STAFF]      = { "Staff",      WCLASS_CASTER, 34, 13.f, 0.0f,  0.58f, 20.f, RGB(120,90,200)  },
};
#define N_WBASE WT_COUNT

/* --- gear bases (per non-weapon slot) ---------------------------- */
typedef struct { const char *name; int armor; uint16_t color; } GearBase;
static const GearBase OFFHAND[] = {
    {"Shield", 8, RGB(150,140,120)},   /* armour */
    {"Tome",   2, RGB(120,90,200)},    /* caster off-hand */
    {"Charm",  3, RGB(200,160,90)},    /* trinket */
};
static const GearBase HELM[]    = { {"Cap",3,RGB(140,120,90)}, {"Helm",6,RGB(170,170,180)}, {"Crown",4,RGB(230,200,80)} };
static const GearBase ARMORB[]  = { {"Tunic",5,RGB(120,90,60)}, {"Mail",9,RGB(150,150,160)}, {"Plate",14,RGB(180,185,200)} };
static const GearBase AMULET[]  = { {"Amulet",0,RGB(230,200,80)}, {"Pendant",0,RGB(200,120,220)} };
static const GearBase RINGB[]   = { {"Ring",0,RGB(230,200,80)}, {"Band",0,RGB(190,190,200)} };
static const GearBase *GBASE[SLOT_COUNT] = { 0, OFFHAND, HELM, ARMORB, AMULET, RINGB };
static const int GBASE_N[SLOT_COUNT] = { 0, 3, 3, 3, 2, 2 };

uint16_t rogue_rarity_color(Rarity r) {
    switch (r) {
        case RAR_MAGIC:     return RGB(90, 140, 255);
        case RAR_RARE:      return RGB(240, 220, 70);
        case RAR_LEGENDARY: return RGB(220, 130, 40);
        default:            return RGB(220, 220, 220);
    }
}
const char *rogue_slot_name(EquipSlot s) {
    static const char *N[SLOT_COUNT] = { "Weapon","Off-hand","Helm","Armor","Amulet","Ring" };
    return (s < SLOT_COUNT) ? N[s] : "?";
}
const char *rogue_aspect_name(AspectId a) {
    switch (a) {
        case ASP_CHAIN:     return "Chaining";
        case ASP_PIERCE:    return "Piercing";
        case ASP_DARK:      return "Nightstalker";
        case ASP_LIFESTEAL: return "Vampiric";
        case ASP_THORNS:    return "Thorns";
        default:            return "";
    }
}
const char *rogue_aspect_desc(AspectId a) {
    switch (a) {
        case ASP_CHAIN:     return "melee hits chain";
        case ASP_PIERCE:    return "shots pierce";
        case ASP_DARK:      return "+dmg in the dark";
        case ASP_LIFESTEAL: return "heal on hit";
        case ASP_THORNS:    return "reflect damage";
        default:            return "";
    }
}
const char *rogue_gem_name(GemType g) {
    switch (g) {
        case GEM_RUBY:     return "Ruby";
        case GEM_SAPPHIRE: return "Sapphire";
        case GEM_EMERALD:  return "Emerald";
        case GEM_TOPAZ:    return "Topaz";
        case GEM_FIREOPAL:   return "Fire Opal";
        case GEM_GLACITE:    return "Glacite";
        case GEM_VENOMSTONE: return "Venomstone";
        case GEM_STORMCRYSTAL: return "Stormcrystal";
        case GEM_SUNSTONE:     return "Sunstone";
        case GEM_NIGHTSTONE:   return "Nightstone";
        case GEM_VOIDPEARL:    return "Voidpearl";
        case GEM_AETHERITE:    return "Aetherite";
        default:           return "-";
    }
}

static const char *afx_noun(AffixType t) {
    switch (t) {
        case AFX_DMG: case AFX_DMG_PCT: return "Wrath";
        case AFX_LIFE:        return "Vigor";
        case AFX_ARMOR: case AFX_RESIST: return "Warding";
        case AFX_CRIT:        return "Precision";
        case AFX_CRITDMG:     return "Ruin";
        case AFX_ATKSPD:      return "Haste";
        case AFX_MOVESPD:     return "the Wind";
        case AFX_LIFEONHIT:   return "Leeching";
        case AFX_FIRE:        return "Flames";
        case AFX_FROST:       return "the Frost";
        case AFX_POISON:      return "Venom";
        case AFX_LIGHTNING:   return "the Storm";
        case AFX_HOLY:        return "Radiance";
        case AFX_SHADOW:      return "the Night";
        case AFX_VOID:        return "the Void";
        case AFX_ARCANE:      return "Force";
        default:              return "Power";
    }
}
void rogue_affix_label(char *buf, int n, const Affix *a) {
    switch (a->type) {
        case AFX_DMG:       snprintf(buf,n,"+%d Dmg", a->val); break;
        case AFX_DMG_PCT:   snprintf(buf,n,"+%d%% Dmg", a->val); break;
        case AFX_LIFE:      snprintf(buf,n,"+%d Life", a->val); break;
        case AFX_ARMOR:     snprintf(buf,n,"+%d Armor", a->val); break;
        case AFX_CRIT:      snprintf(buf,n,"+%d%% Crit", a->val); break;
        case AFX_CRITDMG:   snprintf(buf,n,"+%d%% CritDmg", a->val); break;
        case AFX_ATKSPD:    snprintf(buf,n,"+%d%% Atk Spd", a->val); break;
        case AFX_MOVESPD:   snprintf(buf,n,"+%d%% Move", a->val); break;
        case AFX_LIFEONHIT: snprintf(buf,n,"+%d Life/Hit", a->val); break;
        case AFX_RESIST:    snprintf(buf,n,"+%d%% Resist", a->val); break;
        case AFX_FIRE:      snprintf(buf,n,"+%d Fire", a->val); break;
        case AFX_FROST:     snprintf(buf,n,"+%d Frost", a->val); break;
        case AFX_POISON:    snprintf(buf,n,"+%d Poison", a->val); break;
        case AFX_LIGHTNING: snprintf(buf,n,"+%d Lightning", a->val); break;
        case AFX_HOLY:      snprintf(buf,n,"+%d Holy", a->val); break;
        case AFX_SHADOW:    snprintf(buf,n,"+%d Shadow", a->val); break;
        case AFX_VOID:      snprintf(buf,n,"+%d Void", a->val); break;
        case AFX_ARCANE:    snprintf(buf,n,"+%d Force", a->val); break;
        default:            snprintf(buf,n,"-"); break;
    }
}

void rogue_item_make_gold(RogueItem *it, int amount) {
    memset(it,0,sizeof *it); it->kind=ITEM_GOLD; it->amount=amount;
    it->color=RGB(240,210,60); snprintf(it->name,sizeof it->name,"%d Gold",amount);
}
void rogue_item_make_potion(RogueItem *it, int heal) {
    memset(it,0,sizeof *it); it->kind=ITEM_POTION; it->amount=heal;
    it->color=RGB(230,60,90); snprintf(it->name,sizeof it->name,"Potion +%d",heal);
}
void rogue_item_make_torch(RogueItem *it, int seconds) {
    memset(it,0,sizeof *it); it->kind=ITEM_TORCH; it->amount=seconds;
    it->color=RGB(255,170,40); snprintf(it->name,sizeof it->name,"Torch");
}
uint16_t rogue_gem_color(GemType g) {
    switch (g) {
        case GEM_RUBY:     return RGB(220,40,60);
        case GEM_SAPPHIRE: return RGB(60,110,230);
        case GEM_EMERALD:  return RGB(40,210,110);
        case GEM_TOPAZ:    return RGB(240,210,60);
        case GEM_FIREOPAL:   return RGB(255,120,40);
        case GEM_GLACITE:    return RGB(150,215,255);
        case GEM_VENOMSTONE: return RGB(120,235,90);
        case GEM_STORMCRYSTAL: return RGB(250,245,130);
        case GEM_SUNSTONE:     return RGB(255,215,80);
        case GEM_NIGHTSTONE:   return RGB(150,70,210);
        case GEM_VOIDPEARL:    return RGB(105,95,240);
        case GEM_AETHERITE:    return RGB(255,95,235);
        default:           return RGB(200,200,200);
    }
}
void rogue_item_make_gem(RogueItem *it, GemType g) {
    memset(it,0,sizeof *it); it->kind=ITEM_GEM; it->amount=(int16_t)g;
    it->color=rogue_gem_color(g); snprintf(it->name,sizeof it->name,"%s",rogue_gem_name(g));
}

/* Roll one affix appropriate to the slot. */
static Affix roll_affix(uint32_t *s, EquipSlot slot, int depth, Rarity rar) {
    static const uint8_t WPOOL[] = { AFX_DMG, AFX_DMG_PCT, AFX_CRIT, AFX_CRITDMG, AFX_ATKSPD, AFX_LIFEONHIT,
                                     AFX_FIRE, AFX_FROST, AFX_POISON,
                                     AFX_LIGHTNING, AFX_HOLY,
                                     AFX_SHADOW, AFX_VOID, AFX_ARCANE };
    static const uint8_t APOOL[] = { AFX_LIFE, AFX_ARMOR, AFX_RESIST, AFX_LIFE, AFX_MOVESPD };
    static const uint8_t JPOOL[] = { AFX_DMG_PCT, AFX_CRIT, AFX_CRITDMG, AFX_RESIST, AFX_LIFE, AFX_MOVESPD };
    const uint8_t *pool; int pn;
    if (slot == SLOT_WEAPON)      { pool = WPOOL; pn = sizeof WPOOL; }
    else if (slot == SLOT_AMULET || slot == SLOT_RING) { pool = JPOOL; pn = sizeof JPOOL; }
    else                          { pool = APOOL; pn = sizeof APOOL; }
    Affix a; a.type = pool[xs(s) % (uint32_t)pn];
    float sc = 1.0f + 0.30f * (int)rar + 0.09f * depth;
    int v;
    switch (a.type) {
        case AFX_DMG:       v = rr(s,3,8); break;
        case AFX_DMG_PCT:   v = rr(s,4,10); break;
        case AFX_LIFE:      v = rr(s,8,18); break;
        case AFX_ARMOR:     v = rr(s,4,10); break;
        case AFX_CRIT:      v = rr(s,2,6); break;
        case AFX_CRITDMG:   v = rr(s,8,18); break;
        case AFX_ATKSPD:    v = rr(s,3,8); break;
        case AFX_MOVESPD:   v = rr(s,3,7); break;
        case AFX_LIFEONHIT: v = rr(s,1,4); break;
        case AFX_RESIST:    v = rr(s,4,10); break;
        case AFX_FIRE: case AFX_FROST: case AFX_POISON:
        case AFX_LIGHTNING: case AFX_HOLY:
        case AFX_SHADOW: case AFX_VOID: case AFX_ARCANE: v = rr(s,3,7); break;
        default:            v = 1; break;
    }
    /* % stats scale gently with rarity; flats scale with depth too. */
    if (a.type==AFX_DMG || a.type==AFX_LIFE || a.type==AFX_ARMOR || a.type==AFX_LIFEONHIT ||
        a.type >= AFX_FIRE)
        v = (int)(v * sc);
    else
        v = (int)(v * (1.0f + 0.18f * (int)rar));
    a.val = (int16_t)v;
    return a;
}

/* Rarity is deliberately stingy early so the descent feels like progression.
 * Per-mille chances that climb with depth (capped):
 *   depth 1  -> legendary ~0.5%, rare ~2.9%, magic ~21%
 *   depth 5  -> legendary ~1.7%, rare ~6.5%, magic ~27%
 *   depth 10 -> legendary ~3.2%, rare ~11%,  magic ~34%
 *   depth 20 -> legendary ~6.2%, rare ~18%,  magic ~48% */
static Rarity roll_rarity(uint32_t *s, int depth) {
    int roll = (int)(xs(s) % 1000);
    int leg  = 2  + depth * 3;   if (leg  > 90)  leg  = 90;
    int rare = 20 + depth * 9;   if (rare > 240) rare = 240;
    int mag  = 200 + depth * 14; if (mag  > 520) mag  = 520;
    if (roll < leg)             return RAR_LEGENDARY;
    if (roll < leg + rare)      return RAR_RARE;
    if (roll < leg + rare + mag) return RAR_MAGIC;
    return RAR_COMMON;
}

static void finish_name(RogueItem *it, const char *base) {
    char tmp[26];
    if (it->rarity == RAR_LEGENDARY && it->aspect)
        snprintf(tmp,sizeof tmp,"%s %s", rogue_aspect_name((AspectId)it->aspect), base);
    else if (it->n_affix > 0)
        snprintf(tmp,sizeof tmp,"%s of %s", base, afx_noun((AffixType)it->affix[0].type));
    else
        snprintf(tmp,sizeof tmp,"%s", base);
    memcpy(it->name, tmp, sizeof it->name); it->name[sizeof it->name-1]=0;
}

static void roll_affixes_and_extras(RogueItem *it, EquipSlot slot, int depth, uint32_t *s) {
    int n = (it->rarity==RAR_MAGIC)?1 : (it->rarity>=RAR_RARE)?rr(s,2,MAX_AFFIX) : 0;
    it->n_affix = (uint8_t)n;
    for (int i=0;i<n;i++) it->affix[i] = roll_affix(s, slot, depth, (Rarity)it->rarity);
    if (it->rarity == RAR_LEGENDARY) it->aspect = (uint8_t)rr(s,1,ASP_COUNT-1);
    /* Sockets: rare 0-1, legendary 1-2. */
    if (it->rarity==RAR_RARE)        it->sockets = (xs(s)&1)?1:0;
    else if (it->rarity==RAR_LEGENDARY) it->sockets = (uint8_t)rr(s,1,2);
}

void rogue_item_starter(RogueItem *it) {
    memset(it,0,sizeof *it);
    const WeaponBase *b=&WBASE[WT_DAGGER];
    it->kind=ITEM_WEAPON; it->slot=SLOT_WEAPON; it->wclass=b->wclass; it->wtype=WT_DAGGER;
    it->rarity=RAR_COMMON; it->base_dmg=b->dmg; it->range=b->range;
    it->arc_cos=b->arc_cos; it->cooldown=b->cooldown; it->proj_speed=b->proj_speed;
    it->color=b->color; snprintf(it->name,sizeof it->name,"%s",b->name);
}

void rogue_item_roll_weapon(RogueItem *it, int depth, uint32_t seed) {
    memset(it,0,sizeof *it);
    uint32_t s = seed?seed:1;
    int wt = (int)(xs(&s)%N_WBASE);
    const WeaponBase *b=&WBASE[wt];
    it->kind=ITEM_WEAPON; it->slot=SLOT_WEAPON; it->wclass=b->wclass; it->wtype=(uint8_t)wt;
    it->rarity=roll_rarity(&s, depth);
    float sc=1.0f+0.22f*(int)it->rarity+0.07f*depth;
    it->base_dmg=(int16_t)(b->dmg*sc);
    it->range=b->range; it->arc_cos=b->arc_cos; it->cooldown=b->cooldown;
    it->proj_speed=b->proj_speed; it->color=b->color;
    roll_affixes_and_extras(it, SLOT_WEAPON, depth, &s);
    finish_name(it, b->name);
}

void rogue_item_roll_gear(RogueItem *it, EquipSlot slot, int depth, uint32_t seed) {
    if (slot==SLOT_WEAPON) { rogue_item_roll_weapon(it,depth,seed); return; }
    memset(it,0,sizeof *it);
    uint32_t s = seed?seed:1;
    const GearBase *b=&GBASE[slot][xs(&s)%(uint32_t)GBASE_N[slot]];
    it->kind=ITEM_GEAR; it->slot=(uint8_t)slot;
    it->rarity=roll_rarity(&s, depth);
    float sc=1.0f+0.25f*(int)it->rarity+0.08f*depth;
    it->armor=(int16_t)(b->armor*sc);
    it->color=b->color;
    roll_affixes_and_extras(it, slot, depth, &s);
    finish_name(it, b->name);
}

void rogue_item_roll_drop(RogueItem *it, int depth, uint32_t seed) {
    uint32_t s = seed?seed:1;
    /* weapons a bit more common than any single gear slot */
    int roll = (int)(xs(&s) % 100);
    EquipSlot slot;
    if (roll < 34) slot = SLOT_WEAPON;
    else if (roll < 48) slot = SLOT_OFFHAND;
    else if (roll < 62) slot = SLOT_HELM;
    else if (roll < 76) slot = SLOT_ARMOR;
    else if (roll < 88) slot = SLOT_AMULET;
    else slot = SLOT_RING;
    rogue_item_roll_gear(it, slot, depth, s ? s : seed + 1);
}
