/*
 * kp_rooms.h — hand-authored Spelunky-style room chunks (16x8 tiles each).
 *
 * The floor generator snakes a solution path across the room grid and stamps
 * one chunk per path cell: START (entrance door) -> SIDE/DROP rooms -> EXIT
 * (or BOSSEXIT every third floor). Chunks are plain text — edit freely, keep
 * every row exactly 16 chars.
 *
 * Legend:
 *   #  solid castle wall            .  room interior (pink brick bg)
 *   =  thin wooden plank (one-way)  -  thick beam, metal ends (one-way)
 *   O  drop hole (floor row only — punched through to the room below)
 *   d  small diamond   D  big diamond   h  big heart
 *   B  crate spot (stack 1-2 high, sometimes a bomb on top, sometimes a
 *      hiding pig from depth 2)     b  unlit bomb resting on the floor
 *   E  enemy spot (type scales with depth; may stay empty on early floors)
 *   C  cannon + match pig (only spawns from depth 4)
 *   W  window            F  hanging banner (flag)      S  wall shelf + loot
 *   e  entrance door     x  exit door
 *
 * Conventions every chunk must keep:
 *   - row 0 and row 7 are wall bands ('#', or 'O' holes in row 7)
 *   - rows 4-6 stay open ('.') at cols 0 and 15 — the standard corridor mouth
 *     (a neighbouring path room lines up with it; a dead side just becomes a
 *     small alcove in the wall)
 *   - interiors traversable left<->right with a 2.5-tile jump: platforms max
 *     2 tiles above a standing surface; no pit deeper than 2 tiles
 */
#ifndef KP_ROOMS_H
#define KP_ROOMS_H

#define KP_ROOM_W 16
#define KP_ROOM_H 8

typedef struct { const char *r[KP_ROOM_H]; } KpRoom;

static const KpRoom kp_start[] = {
    {{ "################",
       "#..F.......W...#",
       "#..............#",
       "#........---...#",
       "..........d.....",
       "....e......B....",
       "................",
       "################" }},
    {{ "################",
       "#...W.....F....#",
       "#..............#",
       "#.d..........d.#",
       "................",
       "......e.....b...",
       "................",
       "################" }},
};

static const KpRoom kp_exit[] = {
    {{ "################",
       "#....W......F..#",
       "#..............#",
       "#....===.......#",
       ".....d.d........",
       "...B......x.....",
       "................",
       "################" }},
    {{ "################",
       "#..F......W....#",
       "#...-----......#",
       "#...d.D.d......#",
       "................",
       "....x.....E..B..",
       "................",
       "################" }},
};

static const KpRoom kp_bossexit[] = {
    {{ "################",
       "#..F........F..#",
       "#..............#",
       "#..d.........d.#",
       "................",
       "..B....E.....x..",
       "................",
       "################" }},
};

static const KpRoom kp_side[] = {
    {{ "################",
       "#...W......W...#",
       "#..............#",
       "#..======......#",
       "...d..d.........",
       ".....E......B...",
       "................",
       "################" }},
    {{ "################",
       "#......F...W...#",
       "#..............#",
       "#.....-----....#",
       "......d.S.d.....",
       "..B.....E....C..",
       "................",
       "################" }},
    {{ "################",
       "#..W........W..#",
       "#..............#",
       "#...==....==...#",
       "....d..##..d....",
       "....E..##..E....",
       "................",
       "################" }},
    {{ "################",
       "#.....W....F...#",
       "#..............#",
       "#.S........===.#",
       "............d...",
       "..d...##.....E..",
       "......##B.b.....",
       "################" }},
    {{ "################",
       "#..F.......W...#",
       "#..............#",
       "#...-----......#",
       "....d.d.d..##...",
       ".C...E.....##.d.",
       "................",
       "################" }},
    {{ "################",
       "#..W.......F...#",
       "#..............#",
       "#..==........=.#",
       "...d.......d....",
       "...B..B....E....",
       "................",
       "################" }},
    {{ "################",
       "#....F....W....#",
       "#..=........=..#",
       "#...d..dD..d...#",
       "......####......",
       "..E...####...E..",
       "................",
       "################" }},
    {{ "################",
       "#.W..........W.#",
       "#..............#",
       "#.---......---.#",
       "..d..........d..",
       "....E.....E.....",
       ".B............B.",
       "################" }},
};

static const KpRoom kp_drop[] = {
    {{ "################",
       "#...W.....F....#",
       "#..............#",
       "#.....====.....#",
       "......d..d......",
       "...E........B...",
       "................",
       "#####OO#########" }},
    {{ "################",
       "#..F.......W...#",
       "#..............#",
       "#..-----....S..#",
       "...d...d........",
       "....B....E......",
       "................",
       "#########OO#####" }},
    {{ "################",
       "#......W.......#",
       "#..............#",
       "#..=........=..#",
       "...d...##...d...",
       "..E..h.##....E..",
       "................",
       "##OO############" }},
};

#endif /* KP_ROOMS_H */
