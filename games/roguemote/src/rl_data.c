/*
 * Content tables: monsters, bosses, player classes.
 *
 * Sprite cells are (row * sheet_cols + col) into the baked subsheets, and the
 * column count differs per sheet -- rl_blit_cell() resolves it from img->w, so
 * a cell index here is always "the nth non-empty-looking slot in reading order".
 *
 * Every cell below was chosen by eye from a 9x magnified contact sheet of the
 * baked art, and authoring/verify_sprites.py fails the build if any entry lands
 * on a fully transparent cell -- the failure mode that once looked exactly like
 * a broken spawner (DESIGN.md section 14).
 */
#include "rl.h"

#define A(c) SH_ANIMALS,  (c)
#define M(c) SH_MONSTERS, (c)
#define G(c) SH_REGALIA,  (c)   /* crowns_fx: dummies and blobs, not crowns */

#define GRP  MK_GROUP
#define ERR  MK_ERRATIC
#define STIL MK_NEVER_MOVE
#define DOOR MK_OPEN_DOOR
#define EVIL MK_EVIL
#define UNDD (MK_UNDEAD | MK_EVIL)

/* name                sheet,cell  lvl spd  hp_d,hp_s  ac  dam  xp    flags */
const MonKind g_mon_kind[] = {
/* --- the surface and the first floors: the animals sheet ----------------- */
  { "jackal",            A(0),    1, 115, 1, 4,  8, 1, 3,     1, GRP },
  { "wild dog",          A(1),    2, 115, 1, 5, 10, 1, 4,     3, GRP },
  { "hunting hound",     A(2),    3, 115, 2, 5, 12, 1, 5,     6, GRP },
  { "red stag",          A(3),    4, 115, 3, 6, 14, 1, 6,    10, 0 },
  { "great elk",         A(4),    6, 110, 5, 7, 18, 2, 5,    20, 0 },
  { "wild pig",          A(5),    2, 110, 2, 5, 11, 1, 4,     4, 0 },
  { "tusked boar",       A(6),    4, 110, 4, 6, 16, 2, 4,    12, 0 },
  { "chick",             A(7),    1, 120, 1, 2,  6, 1, 1,     1, ERR },
  { "cockerel",          A(8),    2, 120, 1, 4, 10, 1, 3,     3, ERR },
  { "cow",               A(9),    3, 100, 4, 6, 12, 1, 4,     6, 0 },
  { "sheep",             A(10),   2, 105, 2, 5, 10, 1, 3,     3, 0 },
  { "mountain goat",     A(11),   3, 115, 2, 6, 13, 1, 4,     7, 0 },
  { "canary",            A(12),   1, 125, 1, 2,  8, 1, 1,     1, ERR },
  { "grey goose",        A(13),   2, 115, 1, 5, 10, 1, 3,     3, ERR },
  { "white dove",        A(14),   1, 125, 1, 2,  9, 1, 1,     1, ERR },
  { "hornet",            A(15),   3, 125, 1, 5, 14, 1, 4,     8, ERR },
  { "wasp swarm",        A(16),   4, 125, 2, 5, 15, 2, 3,    12, ERR | GRP },
  { "killer bee",        A(17),   4, 125, 2, 5, 14, 1, 6,    12, GRP },
  { "giant wasp",        A(18),   5, 120, 3, 6, 16, 2, 4,    18, 0 },
  { "green lizard",      A(19),   2, 105, 2, 5, 12, 1, 4,     4, 0 },
  { "grey bat",          A(20),   2, 120, 1, 5, 12, 1, 3,     4, ERR },
  { "vampire bat",       A(21),   5, 125, 3, 5, 16, 1, 6,    18, ERR },
  { "yellow finch",      A(22),   1, 125, 1, 2,  8, 1, 1,     1, ERR },
  { "wild cockerel",     A(23),   2, 120, 1, 4, 10, 1, 3,     3, ERR },
  { "brood hen",         A(24),   1, 115, 1, 3,  8, 1, 2,     2, 0 },
  { "sow",               A(25),   2, 110, 2, 5, 10, 1, 4,     4, 0 },
  { "shadow bat",        A(26),   6, 125, 3, 6, 18, 1, 7,    26, ERR },
  { "brown bat",         A(27),   3, 120, 2, 5, 13, 1, 4,     7, ERR },
  { "cave beetle",       A(28),   3, 105, 3, 5, 16, 1, 5,     8, 0 },
  { "violet fungus",     A(29),   4, 110, 4, 5, 12, 1, 6,    10, STIL },
  { "purple fungus",     A(30),   4, 110, 4, 5, 12, 1, 6,    10, STIL },
  { "fire ant",          A(31),   5, 115, 3, 6, 18, 2, 4,    16, GRP },
  { "sand crab",         A(32),   4, 105, 4, 6, 20, 1, 6,    12, 0 },
  { "carnivorous plant", A(33),   6, 110, 5, 7, 16, 2, 5,    24, STIL },
  { "floating eye",      A(34),   6, 100, 4, 7, 14, 1, 8,    26, STIL },
  { "log worm",          A(35),   3, 100, 3, 5, 14, 1, 5,     7, 0 },
  { "aurochs",           A(36),   6, 105, 6, 7, 20, 2, 6,    24, 0 },
  { "brown cow",         A(37),   3, 100, 4, 6, 12, 1, 4,     6, 0 },
  { "spotted cow",       A(38),   3, 100, 4, 6, 12, 1, 4,     6, 0 },
  { "white bull",        A(39),   7, 110, 7, 7, 22, 2, 7,    34, 0 },
  { "grey mould",        A(40),   2, 110, 2, 6, 10, 1, 4,     3, STIL },
  { "swamp lizard",      A(41),   4, 110, 3, 6, 16, 1, 6,    12, 0 },
  { "giant gecko",       A(42),   5, 115, 4, 6, 18, 2, 4,    18, 0 },
  { "red crab",          A(43),   5, 100, 5, 6, 22, 1, 7,    18, 0 },
  { "pink crab",         A(44),   5, 100, 5, 6, 22, 1, 7,    18, 0 },
  { "cave roach",        A(45),   3, 115, 2, 5, 14, 1, 4,     7, GRP },
  { "grey roach",        A(46),   3, 115, 2, 5, 14, 1, 4,     7, GRP },
  { "rust beetle",       A(47),   4, 105, 4, 6, 20, 1, 5,    12, 0 },
  { "sewer rat",         A(48),   1, 110, 1, 3,  6, 1, 2,     1, GRP },
  { "grey rat",          A(49),   1, 110, 1, 4,  8, 1, 3,     1, GRP },
  { "plague rat",        A(50),   2, 115, 1, 5, 10, 1, 4,     4, GRP },
  { "white rat",         A(51),   1, 110, 1, 4,  8, 1, 3,     1, GRP },
  { "albino rat",        A(52),   2, 115, 2, 4, 10, 1, 4,     4, GRP },
  { "pale rat",          A(53),   2, 115, 2, 4, 10, 1, 4,     4, GRP },
  { "shadow rat",        A(54),   3, 120, 2, 5, 13, 1, 5,     8, GRP },
  { "giant worm",        A(55),   3, 100, 3, 5, 14, 1, 5,     7, 0 },
  { "sand snake",        A(56),   4, 105, 3, 6, 16, 1, 6,    12, 0 },
  { "green frog",        A(57),   2, 110, 2, 5, 12, 1, 4,     4, 0 },
  { "giant frog",        A(58),   4, 110, 4, 5, 15, 1, 6,    12, 0 },
  { "crow",              A(59),   3, 125, 2, 5, 12, 1, 5,     8, ERR },
  { "blood tick",        A(60),   4, 105, 3, 6, 18, 1, 6,    12, 0 },
  { "snowy owl",         A(61),   4, 125, 3, 5, 16, 1, 6,    14, ERR },
  { "grey owl",          A(62),   4, 125, 3, 5, 16, 1, 6,    14, ERR },
  { "night moth",        A(63),   3, 125, 2, 4, 14, 1, 4,     8, ERR },
  { "yellow jacket",     A(64),   5, 125, 3, 6, 17, 2, 4,    18, GRP },
  { "red wasp",          A(65),   6, 125, 4, 6, 18, 2, 5,    24, GRP },
  { "gold hornet",       A(66),   6, 125, 4, 6, 18, 2, 5,    24, GRP },
  { "grey wasp",         A(67),   5, 125, 3, 6, 17, 2, 4,    18, GRP },
  { "cave fish",         A(68),   3, 110, 2, 5, 14, 1, 4,     7, ERR },
  { "shrieker",          A(69),   4, 100, 4, 5, 10, 1, 3,     8, STIL },
  { "bull frog",         A(70),   5, 110, 5, 6, 16, 2, 4,    18, 0 },
  { "tree frog",         A(71),   3, 115, 2, 5, 13, 1, 4,     8, 0 },
  { "mushroom man",      A(96),   5, 105, 5, 6, 18, 2, 4,    18, 0 },
  { "cap fungus",        A(97),   4, 100, 4, 6, 12, 1, 5,    10, STIL },
  { "white shrieker",    A(98),   5, 100, 5, 6, 12, 1, 5,    14, STIL },
  { "red mushroom man",  A(99),   6, 105, 6, 6, 19, 2, 5,    24, 0 },
  { "violet mushroom",   A(100),  7, 105, 7, 6, 20, 2, 6,    32, 0 },
  { "shadow fungus",     A(101),  7, 100, 7, 6, 16, 2, 5,    28, STIL },
  { "green mushroom man",A(102),  6, 105, 6, 6, 19, 2, 5,    24, 0 },
  { "moss fungus",       A(103),  5, 100, 5, 6, 14, 1, 6,    14, STIL },
  { "crawling vine",     A(104),  6, 105, 5, 7, 18, 2, 5,    26, 0 },
  { "blood vine",        A(105),  8, 105, 8, 7, 22, 2, 7,    44, 0 },
  { "strangle weed",     A(106),  9, 100,10, 7, 24, 3, 5,    56, STIL },

/* --- the Mines proper: the monsters sheet -------------------------------- */
  { "kobold",            M(0),    3, 110, 3, 5, 14, 1, 6,     8, DOOR | GRP | EVIL },
  { "large kobold",      M(1),    4, 110, 4, 6, 16, 1, 7,    14, DOOR | GRP | EVIL },
  { "kobold chieftain",  M(2),    6, 110, 6, 7, 20, 2, 5,    28, DOOR | EVIL },
  { "rubble mimic",      M(3),    6, 100, 8, 6, 26, 2, 5,    24, STIL },
  { "clay mimic",        M(4),    7, 100, 9, 6, 28, 2, 6,    32, STIL },
  { "shadow lurker",     M(5),    8, 115, 6, 7, 22, 2, 7,    44, ERR | EVIL },
  { "gold serpent",      M(6),    7, 105, 6, 7, 24, 2, 6,    34, 0 },
  { "crimson slug",      M(7),    5, 100, 6, 6, 18, 1, 8,    16, 0 },
  { "mud crawler",       M(8),    4, 105, 4, 6, 18, 1, 6,    12, 0 },
  { "blood slug",        M(9),    6, 100, 7, 6, 20, 2, 5,    24, 0 },
  { "green worm mass",   M(10),   5, 100, 5, 6, 16, 1, 7,    16, GRP },
  { "pink worm mass",    M(11),   5, 100, 5, 6, 16, 1, 7,    16, GRP },
  { "white serpent",     M(12),   8, 105, 7, 7, 24, 2, 7,    44, 0 },
  { "jade serpent",      M(13),   9, 105, 8, 7, 26, 2, 8,    56, 0 },
  { "amber serpent",     M(14),  10, 105, 9, 8, 28, 3, 6,    70, 0 },
  { "rose serpent",      M(15),  10, 105, 9, 8, 28, 3, 6,    70, 0 },
  { "ice serpent",       M(16),  12, 105,11, 8, 32, 3, 7,   100, 0 },
  { "flame serpent",     M(17),  13, 105,12, 8, 34, 3, 8,   120, 0 },
  { "cave serpent",      M(18),   9, 105, 8, 7, 26, 2, 8,    56, 0 },
  { "white jackal",      M(48),   4, 118, 3, 6, 16, 1, 6,    12, GRP },
  { "pale jackal",       M(49),   4, 118, 3, 6, 16, 1, 6,    12, GRP },
  { "grey jackal",       M(50),   5, 118, 4, 6, 18, 2, 4,    18, GRP },
  { "winter wolf",       M(51),  10, 120, 9, 7, 28, 2, 8,    70, GRP },
  { "snow wolf",         M(52),  11, 120,10, 7, 30, 3, 6,    82, GRP },
  { "dire wolf",         M(53),  12, 120,11, 8, 32, 3, 7,    98, GRP },
  { "white warg",        M(54),  13, 120,12, 8, 34, 3, 8,   118, GRP | EVIL },
  { "wolf cub",          M(55),   3, 118, 2, 5, 13, 1, 5,     7, GRP },
  { "fire hound",        M(56),  14, 120,12, 8, 34, 3, 9,   140, GRP | EVIL },
  { "gold jackal",       M(57),   6, 118, 5, 6, 20, 2, 5,    24, GRP },
  { "sand jackal",       M(58),   6, 118, 5, 6, 20, 2, 5,    24, GRP },
  { "desert hound",      M(59),   7, 118, 6, 7, 22, 2, 6,    34, GRP },
  { "amber hound",       M(60),   8, 118, 7, 7, 24, 2, 7,    44, GRP },
  { "gold warg",         M(61),  10, 120, 9, 8, 28, 3, 6,    70, GRP | EVIL },
  { "sun hound",         M(62),  11, 120,10, 8, 30, 3, 6,    82, EVIL },
  { "gold lynx",         M(63),   9, 122, 7, 8, 26, 2, 8,    56, 0 },
  { "gold panther",      M(64),  12, 122,10, 9, 32, 3, 7,    98, 0 },
  { "gold tiger",        M(65),  14, 122,12, 9, 36, 3, 9,   140, 0 },
  { "sabre cat",         M(66),  15, 122,13, 9, 38, 4, 7,   160, 0 },
  { "striped hound",     M(67),   9, 118, 8, 7, 26, 2, 7,    56, GRP },
  { "yellow warg",       M(68),  11, 120,10, 8, 30, 3, 6,    82, GRP | EVIL },
  { "great tiger",       M(69),  17, 122,15,10, 42, 4, 8,   210, 0 },
  { "tiger lord",        M(70),  19, 124,17,10, 46, 4, 9,   270, 0 },
  { "green hound",       M(71),   6, 118, 5, 6, 20, 2, 5,    24, GRP },
  { "moss hound",        M(72),   7, 118, 6, 7, 22, 2, 6,    34, GRP },
  { "swamp hound",       M(73),   8, 118, 7, 7, 24, 2, 7,    44, GRP },
  { "jade hound",        M(74),   9, 118, 8, 7, 26, 2, 7,    56, GRP },
  { "forest warg",       M(75),  11, 120,10, 8, 30, 3, 6,    82, GRP | EVIL },
  { "green stalker",     M(76),  12, 118,11, 8, 32, 3, 7,    98, EVIL },
  { "thicket beast",     M(77),  13, 112,13, 8, 36, 3, 8,   118, 0 },
  { "bog beast",         M(78),  14, 110,15, 9, 38, 3, 9,   140, 0 },
  { "jungle warg",       M(79),  15, 120,14, 9, 38, 4, 7,   160, GRP | EVIL },
  { "moss beast",        M(80),  12, 110,12, 8, 34, 3, 7,    98, 0 },
  { "vine beast",        M(81),  14, 110,15, 9, 38, 3, 9,   140, 0 },
  { "swamp horror",      M(82),  16, 112,17,10, 42, 4, 8,   190, EVIL },
  { "poltergeist",       M(83),   8, 130, 5, 7, 20, 1, 8,    44, ERR | UNDD },
  { "grey ghost",        M(84),  13, 118,10, 8, 30, 2, 9,   118, ERR | UNDD },
  { "imp",               M(112), 12, 122, 9, 8, 30, 2, 8,    98, ERR | EVIL },
  { "lesser demon",      M(113), 14, 115,13, 9, 36, 3, 8,   140, EVIL | DOOR },
  { "chaos beast",       M(114), 22, 122,18,10, 52, 4, 9,   420, ERR | EVIL },
  { "blood imp",         M(115), 13, 122,10, 8, 32, 2, 9,   118, ERR | EVIL },
  { "crimson demon",     M(116), 16, 115,15, 9, 40, 3, 9,   190, EVIL | DOOR },
  { "horned demon",      M(117), 18, 115,17,10, 44, 4, 8,   250, EVIL | DOOR },
  { "demon warrior",     M(118), 20, 115,19,11, 48, 4, 9,   330, EVIL | DOOR },
  { "pit fiend",         M(119), 24, 118,22,12, 56, 5, 8,   520, EVIL | DOOR },
  { "greater demon",     M(120), 26, 118,24,12, 60, 5, 9,   640, EVIL | DOOR },
  { "dark wight",        M(121), 15, 110,12, 9, 34, 3, 7,   160, UNDD | DOOR },
  { "grave wight",       M(122), 17, 110,14, 9, 38, 3, 8,   210, UNDD | DOOR },
  { "bone lich",         M(123), 26, 115,22,12, 62, 5, 8,   660, UNDD | DOOR },
  { "pale lich",         M(124), 28, 115,24,13, 66, 5, 9,   780, UNDD | DOOR },
  { "night hag",         M(125), 20, 118,17,10, 46, 4, 8,   330, EVIL | ERR },
  { "shadow hag",        M(126), 22, 118,19,11, 50, 4, 9,   420, EVIL | ERR },
  { "dark troll",        M(127), 18, 108,20,10, 46, 4, 8,   250, EVIL | DOOR },

/* --- the crowns_fx sheet ------------------------------------------------
 * Filed under "crowns" by the extract pass and used as body armour by the item
 * table, which was wrong twice over: source row 27 is a rank of training
 * dummies and rows 29 and 31 are blob enemies. They are monsters, and this is
 * where they belong. The dummies never move, which is what a dummy does. */
  { "straw dummy",       G(27),   2, 100, 3, 6, 14, 1, 4,     4, STIL },
  { "bone dummy",        G(28),   5, 100, 6, 7, 20, 1, 6,    16, STIL | UNDD },
  { "leather dummy",     G(29),   3, 100, 4, 6, 16, 1, 5,     8, STIL },
  { "iron dummy",        G(30),   8, 100, 9, 8, 30, 2, 5,    40, STIL },
  { "green blob",        G(45),   3, 100, 4, 6, 14, 1, 5,     8, 0 },
  { "gold blob",         G(46),   5, 100, 6, 6, 18, 1, 7,    18, 0 },
  { "crimson blob",      G(47),   7, 100, 8, 7, 22, 2, 5,    32, 0 },
  { "violet blob",       G(48),   9, 100,10, 7, 26, 2, 6,    52, 0 },
  { "azure blob",        G(49),  11, 100,12, 8, 30, 2, 7,    76, 0 },
  { "clay mound",        G(50),   6, 100, 9, 6, 24, 2, 4,    24, STIL },
  { "shadow mound",      G(51),  10, 100,12, 7, 30, 2, 7,    64, STIL },
  { "moss mound",        G(52),   8, 100,10, 7, 26, 2, 5,    40, STIL },
  { "pale ooze",         G(63),   4, 105, 5, 6, 16, 1, 6,    12, 0 },
  { "violet ooze",       G(64),   9, 105,10, 7, 26, 2, 6,    52, 0 },
  { "acid ooze",         G(65),  12, 105,13, 8, 32, 3, 5,    98, 0 },
};
const int g_mon_kind_n = (int)(sizeof g_mon_kind / sizeof g_mon_kind[0]);

/* --- bosses --------------------------------------------------------------
 * The `bosses` sheet holds seventeen 2x2 sprites in three bands: rows 1-2
 * (cells 16,18,...,30), rows 3-4 (48..56) and rows 5-6 (80..86). Cell here is
 * the block's TOP-LEFT, and rl_draw draws a 16x16 frame from it.
 *
 * One boss guards each of seventeen floors. It is not rolled -- when you take
 * the stairs to its depth it is there, in the deepest room, awake. `drop` is a
 * guaranteed item kind so the fight always pays. */
const BossKind g_boss_kind[] = {
/*  name                cell depth spd   hp   ac dam    xp   drop */
  { "Grishnakh",         26,   3, 112,   40,  22, 2, 6,   120, ITM_LEATHER_SHIELD }, /* skeleton champion */
  { "Bloat the Chief",   18,   5, 110,   65,  26, 2, 7,   240, ITM_WAR_HAMMER },     /* iron knight */
  { "Wormtongue",        28,   7, 118,   85,  28, 2, 8,   400, ITM_WAND_MISSILE },   /* crimson spider */
  { "Lagduf the Snaga",  22,   9, 110,  120,  32, 3, 6,   620, ITM_IRON_HELM },      /* floating horror */
  { "Medusa",            24,  11, 114,  150,  34, 3, 7,   900, ITM_RING_PROT },
  { "Boldor of the Yeek",20,  13, 112,  185,  36, 3, 8,  1250, ITM_CHAIN_MAIL },     /* the great maw */
  { "Ulfast",            16,  15, 114,  225,  40, 4, 6,  1700, ITM_BATTLE_AXE },     /* crimson demon lord */
  { "The Pale Shade",    30,  17, 122,  270,  42, 3,10,  2300, ITM_WAND_FIRE },      /* great ghost */
  { "Shagrat",           48,  19, 112,  330,  46, 4, 8,  3000, ITM_GILDED_BLADE },
  { "Golfimbul",         50,  21, 112,  400,  50, 4, 9,  3900, ITM_IRON_SHIELD },    /* gold knight */
  { "The Grim Reaper",   52,  23, 120,  480,  52, 4,10,  5000, ITM_WAND_FROST },     /* hooded reaper */
  { "Khamul",            54,  25, 116,  580,  56, 5, 8,  6400, ITM_PLATE_MAIL },     /* demon-armour */
  { "Shelob",            56,  27, 114,  700,  60, 5, 9,  8000, ITM_FROST_BRAND },    /* giant insect */
  { "The Seraph",        80,  29, 120,  850,  64, 5,10, 10000, ITM_RING_REGEN },
  { "Ancalagon",         82,  31, 116, 1050,  70, 6, 9, 13000, ITM_FLAME_TONGUE },
  { "The Radiant One",   84,  33, 122, 1300,  74, 6,10, 16000, ITM_AMULET_SPEED },
  { "Morgoth",           86,  35, 124, 1700,  82, 7,10, 25000, ITM_BLADE_OF_CHAOS },
};
const int g_boss_kind_n = (int)(sizeof g_boss_kind / sizeof g_boss_kind[0]);

/* --- player classes ------------------------------------------------------
 * Twelve of the full-body figures from the `characters` sheet. `spells` is a
 * bitmask over g_spell[] so each class gets an arbitrary, thematic set instead
 * of whatever a contiguous window happens to contain.
 *
 * Bit -> spell:  0 Magic Missile  1 Detect Life   2 Frost Bolt   3 Light Room
 *                4 Cure Wounds    5 Lightning     6 Shock Ball   7 Bless
 *                8 Fireball       9 Great Heal   10 Haste Self  11 Ice Storm
 *               12 Chain Storm   13 Word of Pain 14 Annihilate  15 Mana Storm */
const ClassKind g_class[] = {
/*  name         cell  STR INT WIS DEX CON CHA  hp  sp  spells   weapon */
  { "Warrior",     58,  17, 8,  9, 14, 16,  9,  8,  0, 0x0000, ITM_SHORT_SWORD },
  { "Knight",      56,  16, 9, 11, 12, 16, 13,  7,  1, 0x0090, ITM_SHORT_SWORD },
  { "Paladin",     57,  15,10, 14, 11, 15, 14,  6,  3, 0x02D8, ITM_MACE },
  { "Ranger",      55,  14,11, 11, 17, 13, 10,  5,  2, 0x0425, ITM_SPEAR },
  { "Rogue",       59,  12,13, 10, 18, 12, 12,  4,  2, 0x040B, ITM_DAGGER },
  { "Mage",        62,   9,18,  9, 13, 10, 11,  2,  6, 0xDD6F, ITM_DAGGER },
  { "Sorcerer",    60,   9,17, 10, 12, 10, 14,  2,  7, 0xF967, ITM_DAGGER },
  { "Priest",      51,  11,10, 18, 10, 13, 13,  4,  5, 0x229A, ITM_WAR_HAMMER },
  { "Druid",       66,  12,12, 16, 12, 13, 11,  4,  5, 0x0A9E, ITM_MACE },
  { "Bard",        71,  11,14, 12, 15, 11, 18,  3,  4, 0x048B, ITM_DAGGER },
  { "Monk",        48,  14,12, 15, 17, 14, 11,  6,  3, 0x2490, ITM_DAGGER },
  /* "Berserk", not "Barbarian": the class grid gives a name 7 characters
   * before the third column runs off a 128px screen. */
  { "Berserk",     76,  18, 7,  8, 13, 18,  8,  9,  0, 0x0000, ITM_WAR_HAMMER },
};
const int g_class_n = (int)(sizeof g_class / sizeof g_class[0]);
