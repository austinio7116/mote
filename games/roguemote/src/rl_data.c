/*
 * Content tables.
 *
 * Sprite cells are (row * sheet_cols + col) into the baked subsheets. The
 * monster identities below follow the annotator pass, which is only partly
 * human-verified -- names here are provisional and should be re-checked against
 * labels_human.json as that pass lands (see DESIGN.md section 14).
 */
#include "rl.h"

/* The monsters sheet is non-empty at cells 0..18 and 48..; 19..47 is a gap,
 * so table entries stay inside the populated ranges. verify_sprites.py fails
 * the build if any entry points at a blank cell.
 *
 * sheet ids -- must match the s_sheet[] table in rl_draw.c */
enum { SH_ANIMALS = 0, SH_MONSTERS, SH_BOSSES, SH_CHARACTERS };

#define A(c) SH_ANIMALS, (c)
#define M(c) SH_MONSTERS, (c)

/* name              sheet,cell  lvl spd  hp    ac  dam    xp  flags */
const MonKind g_mon_kind[] = {
    /* --- the surface and the first few floors: animals ------------------ */
    { "giant rat",       A(0),    1, 110, 1, 4,  8, 1, 3,     1, MK_GROUP },
    { "sewer rat",       A(1),    1, 110, 1, 3,  6, 1, 2,     1, MK_GROUP },
    { "white bat",       A(16),   1, 120, 1, 4,  9, 1, 3,     2, MK_ERRATIC },
    { "grey mould",      A(2),    1, 110, 2, 6, 10, 1, 4,     3, MK_NEVER_MOVE },
    { "wild dog",        A(3),    2, 115, 2, 5, 12, 1, 5,     4, MK_GROUP },
    { "green snake",     A(17),   2, 105, 2, 6, 14, 1, 4,     4, 0 },
    { "giant frog",      A(18),   3, 110, 3, 5, 14, 1, 6,     6, 0 },
    { "cave spider",     A(19),   3, 120, 2, 5, 12, 1, 5,     6, MK_GROUP },
    { "wild boar",       A(4),    4, 110, 4, 6, 18, 2, 4,    10, 0 },
    { "rock lizard",     A(20),   4, 110, 3, 6, 16, 1, 6,     8, 0 },
    { "giant beetle",    A(21),   5, 105, 5, 6, 24, 1, 8,    14, 0 },
    { "crow",            A(5),    5, 125, 3, 5, 12, 1, 5,    12, MK_ERRATIC },
    { "wolf",            A(6),    6, 120, 5, 7, 20, 2, 5,    18, MK_GROUP },
    { "giant spider",    A(22),   7, 115, 6, 7, 24, 2, 6,    26, 0 },
    { "grizzly bear",    A(7),    9, 110, 9, 8, 30, 2, 8,    45, 0 },

    /* --- the Mines proper: humanoids and horrors ------------------------ */
    { "kobold",          M(0),    2, 110, 3, 5, 14, 1, 6,     5, MK_OPEN_DOOR | MK_GROUP },
    { "goblin",          M(1),    3, 110, 4, 6, 16, 1, 7,     8, MK_OPEN_DOOR | MK_GROUP },
    { "orc archer",      M(2),    5, 110, 5, 7, 20, 1, 8,    16, MK_OPEN_DOOR },
    { "cave orc",        M(3),    6, 110, 6, 8, 24, 2, 6,    22, MK_OPEN_DOOR | MK_GROUP },
    { "green slime",     M(16),   4, 100, 6, 8, 12, 1, 8,    12, 0 },
    { "black naga",      M(17),   7, 100, 8, 8, 30, 2, 8,    35, 0 },
    { "hill giant",      M(4),   10, 110,12, 9, 36, 3, 6,    70, MK_OPEN_DOOR },
    { "skeleton",        M(18),   8, 110, 7, 8, 28, 2, 7,    32, MK_OPEN_DOOR },
    { "zombie",          M(48),   8, 100, 9, 9, 24, 2, 6,    30, 0 },
    { "ghoul",           M(49),  11, 110,10, 9, 34, 2, 8,    60, MK_OPEN_DOOR },
    { "wraith",          M(50),  14, 115,11,10, 38, 3, 7,   110, 0 },
    { "imp",             M(5),   12, 120, 9, 9, 30, 2, 8,    80, MK_ERRATIC },
    { "fire demon",      M(6),   18, 115,14,11, 46, 3, 9,   220, MK_OPEN_DOOR },
    { "ice troll",       M(7),   16, 105,16,11, 50, 3, 8,   180, MK_OPEN_DOOR },
    { "stone golem",     M(51),  17, 100,18,12, 60, 3, 8,   200, MK_NEVER_MOVE },
    { "ghost",           M(52),  15, 115,10,10, 30, 2, 9,   140, MK_ERRATIC },
    { "vampire",         M(8),   22, 115,16,12, 54, 3,10,   340, MK_OPEN_DOOR },
    { "young dragon",    M(9),   24, 110,20,13, 70, 4, 9,   500, MK_OPEN_DOOR },
    { "chaos beast",     M(53),  27, 120,18,12, 60, 3,11,   620, MK_ERRATIC },
    { "iron lich",       M(10),  30, 115,20,14, 76, 4,10,   900, MK_OPEN_DOOR },
};
const int g_mon_kind_n = (int)(sizeof g_mon_kind / sizeof g_mon_kind[0]);
