/*
 * ThumbyElite — weapon catalogue.
 *
 * Roles: pulse lasers = reliable energy DPS by size tier; beam = sustained
 * melt at high heat; photon = slow heavy slug you must lead; gauss =
 * sniper alpha; autocannon = close-range shredder; missiles = burst AoE,
 * homing trades damage for guidance.
 */
#include "elite_weapons.h"
#include "elite_types.h"

const WeaponDef k_weapons[WPN_COUNT] = {
    /*                name       dmg  cool  heat  speed  range turn aoe  sz ammo color */
    [WPN_PULSE_S]   = { "PULSE-S",  9, 0.16f, 4.5f,    0,  600,  0,   0, 1,  0, RGB565C(255,  80,  60) },
    [WPN_PULSE_M]   = { "PULSE-M", 16, 0.22f, 7.5f,    0,  700,  0,   0, 2,  0, RGB565C(255, 120,  50) },
    [WPN_PULSE_L]   = { "PULSE-L", 28, 0.30f, 12.0f,   0,  800,  0,   0, 3,  0, RGB565C(255, 160,  40) },
    [WPN_BEAM]      = { "BEAM",     5, 0.05f, 2.6f,    0,  500,  0,   0, 2,  0, RGB565C(255, 220, 120) },
    [WPN_PHOTON]    = { "PHOTON",  34, 0.55f, 14.0f, 260, 1100,  0,   0, 2,  0, RGB565C(120, 220, 255) },
    [WPN_GAUSS]     = { "GAUSS",   40, 0.90f, 6.0f, 1400, 1700,  0,   0, 2, 24, RGB565C(200, 230, 255) },
    [WPN_AUTOCANNON]= { "AUTOCAN",  5, 0.07f, 1.2f,  750,  900,  0,   0, 1,200, RGB565C(255, 210, 140) },
    [WPN_MISSILE]   = { "MISSILE", 45, 0.80f, 2.0f,  205, 2000,  0,  22, 1,  8, RGB565C(255, 170,  90) },
    [WPN_HOMING]    = { "HOMING",  32, 1.10f, 2.0f,  172, 2600, 1.0f, 18, 2, 10, RGB565C(255, 120, 200) },
    [WPN_FLAK]      = { "FLAK",    30, 0.65f, 5.0f, 1200,  300,  0,   0, 2, 60, RGB565C(255, 190, 110) },
    [WPN_RAILGUN]   = { "RAILGUN", 90, 1.60f, 16.0f, 2200, 2200, 0,   0, 3, 12, RGB565C(170, 240, 255) },
    [WPN_ION]       = { "ION",     30, 0.45f, 8.0f,  340,  700,  0,   0, 2,  0, RGB565C(110, 160, 255) },
    [WPN_MINE]      = { "MINE",    45, 1.20f, 1.0f,    0,    0,  0,  18, 1,  6, RGB565C(255, 140,  70) },
    [WPN_TRACTOR]   = { "TRACTOR",  0, 0.10f, 0.6f,    0,  300,  0,   0, 1,  0, RGB565C(255, 215, 120) },
    [WPN_MINING]    = { "MINING",   2, 0.09f, 1.4f,    0,  350,  0,   0, 1,  0, RGB565C(255, 200,  90) },
    [WPN_PLASMA]    = { "PLASMA",   7, 0.09f, 2.2f,  380,  700,  0,   0, 2,  0, RGB565C(120, 235, 255) },
    [WPN_LANCE]     = { "P.LANCE", 18, 0.60f, 9.0f,    0,  600,  0,   0, 3,  0, RGB565C(190, 140, 255) },
    [WPN_BLASTER]   = { "BLASTER", 11, 0.16f, 3.2f,  260,  700, 0.5f, 0, 2,  0, RGB565C(150, 200, 255) },
};

const AffixDef k_affixes[AFX_COUNT] = {
    /*                 name           tag    dmg    heat   cd     range  price */
    [AFX_NONE]        = { "",           "",   1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
    [AFX_OVERCLOCKED] = { "OVERCLOCKED","OC", 1.18f, 1.30f, 1.00f, 1.00f, 1.35f },
    [AFX_VENTED]      = { "VENTED",     "VN", 0.92f, 0.65f, 1.00f, 1.00f, 1.30f },
    [AFX_CALIBRATED]  = { "CALIBRATED", "CL", 1.00f, 1.00f, 1.00f, 1.30f, 1.30f },
    [AFX_RAPID]       = { "RAPID",      "RP", 0.88f, 1.00f, 0.80f, 1.00f, 1.35f },
    [AFX_SURPLUS]     = { "SURPLUS",    "SP", 1.00f, 1.00f, 1.00f, 1.00f, 0.70f },
    [AFX_TUNED]       = { "TUNED",      "TN", 1.10f, 1.00f, 1.00f, 1.00f, 1.80f },
};

const EquipDef k_equip[11] = {
    { "SHIELD", 1400 },
    { "ARMOR", 1100 },
    { "HEATSINK", 2200 },
    { "SCANNER+", 1500 },
    { "AB TANK", 1800 },
    { "FUELSCOOP", 2600 },
    { "TARGETCOMP", 3400 },
    { "CHAFF", 1200 },
    { "RPR DRONE", 3600 },
    { "CLOAK", 5800 },
    { "MANIFEST", 2200 },
};

const char *k_shield_var_names[4] = { "", "REGEN", "BULWARK", "PHASE" };
const char *k_armor_var_names[4] = { "", "REACTIVE", "ABLATIVE",
                                     "COMPOSITE" };

const char *item_name(int type) {
    if (type >= WPN_COUNT && type < ITEM_COUNT)
        return k_equip[type - WPN_COUNT].name;
    return k_weapons[type].name;
}
