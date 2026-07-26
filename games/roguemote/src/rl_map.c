/*
 * Dungeon generation and field of view.
 *
 * Moria's generator, because it is the right one for this game: rooms placed by
 * rejection sampling, L-shaped corridors between them, stairs at both ends.
 * Levels are NOT persistent -- going back up regenerates, which is what makes
 * the descent feel one-way.
 */
#include "rl.h"

const MoteApi *g_api;
Player   g_pl;
Level    g_lv;
uint32_t g_seed = 1;
uint32_t g_turn = 0;

uint32_t rl_rand(void) {
    uint32_t x = g_seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (g_seed = x);
}

int rl_walkable(int x, int y) {
    switch (rl_ter(x, y)) {
    case T_FLOOR: case T_DOOR_OPEN: case T_STAIR_DOWN: case T_STAIR_UP:
    case T_TREE:  case T_HILL:      case T_TOWN:
    case T_DUNGEON_MOUTH: case T_SHOP:
    case T_CHEST: case T_CHEST_OPEN:
    case T_ROAD:  case T_INN:  case T_TOWER:
    /* a lever is stood on and pulled; a locked door is not walked through */
    case T_LEVER_R: case T_LEVER_B: case T_LEVER_D:
    case T_LEVER_G: case T_LEVER_W:
    /* a locked chest is stood on, like any other chest */
    case T_CBOX_R:  case T_CBOX_B:  case T_CBOX_D:
    case T_CBOX_G:  case T_CBOX_W:
        return 1;
    default:
        return 0;
    }
}

int rl_opaque(int x, int y) {
    uint8_t t = rl_ter(x, y);
    return t == T_WALL || t == T_DOOR_CLOSED || t == T_RUBBLE || t == T_MOUNTAIN ||
           t == T_TOWN_WALL;
}

Mon *rl_mon_at(int x, int y) {
    for (int i = 0; i < g_lv.n_mon; i++)
        if (g_lv.mon[i].hp > 0 && g_lv.mon[i].x == x && g_lv.mon[i].y == y)
            return &g_lv.mon[i];
    return 0;
}

/* --- generation --------------------------------------------------------- */
#define MAX_ROOM 20
enum { RM_PLAIN = 0, RM_L, RM_PILLARED, RM_VAULT };
typedef struct { uint8_t x, y, w, h, kind, dark, bud; } Room;
static Room  s_room[MAX_ROOM];
static int   s_nroom;

static void carve_rect(int x, int y, int w, int h, uint8_t t) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (rl_in(i, j)) g_lv.terrain[j * MW + i] = t;
}

static int room_overlaps(int x, int y, int w, int h) {
    for (int i = 0; i < s_nroom; i++) {
        Room *r = &s_room[i];
        if (x - 1 < r->x + r->w + 1 && x + w + 1 > r->x - 1 &&
            y - 1 < r->y + r->h + 1 && y + h + 1 > r->y - 1) return 1;
    }
    return 0;
}

/* Room shapes. Rectangles-only was the single biggest reason one floor looked
 * like the last: nine boxes, all lit, chained in a line. Four shapes now, rolled
 * per room, with the odds shifting toward the awkward ones as you descend.
 *
 *   PLAIN     a rectangle, still the common case
 *   L         a rectangle with a corner bitten out, so sightlines break
 *   PILLARED  a hall with a grid of columns -- cover, and a monster you lose
 *   VAULT     a room with a sealed inner chamber; see stock_vault below
 */
static int roll_room_kind(int depth) {
    int r = rl_range(100);
    if (r < 8 + depth)      return RM_VAULT;
    if (r < 30 + depth)     return RM_PILLARED;
    if (r < 55 + depth)     return RM_L;
    return RM_PLAIN;
}

static void carve_room(Room *rm) {
    carve_rect(rm->x, rm->y, rm->w, rm->h, T_FLOOR);
    switch (rm->kind) {
    case RM_L: {
        /* bite a corner out, up to half the room in each axis */
        int cw = 1 + rl_range(rm->w / 2), ch = 1 + rl_range(rm->h / 2);
        int cx = rl_pct(50) ? rm->x : rm->x + rm->w - cw;
        int cy = rl_pct(50) ? rm->y : rm->y + rm->h - ch;
        carve_rect(cx, cy, cw, ch, T_WALL);
        break;
    }
    case RM_PILLARED:
        for (int j = rm->y + 1; j < rm->y + rm->h - 1; j += 2)
            for (int i = rm->x + 1; i < rm->x + rm->w - 1; i += 2)
                g_lv.terrain[j * MW + i] = T_WALL;
        break;
    case RM_VAULT: {
        /* an inner chamber with one closed door -- what is in it is placed by
         * stock_vault once the monsters and items exist */
        int ix = rm->x + 1, iy = rm->y + 1, iw = rm->w - 2, ih = rm->h - 2;
        if (iw < 3 || ih < 3) { rm->kind = RM_PLAIN; break; }
        for (int j = iy; j < iy + ih; j++)
            for (int i = ix; i < ix + iw; i++)
                if (i == ix || i == ix + iw - 1 || j == iy || j == iy + ih - 1)
                    g_lv.terrain[j * MW + i] = T_WALL;
        /* the door, on a random side, never in a corner */
        if (rl_pct(50)) g_lv.terrain[(rl_pct(50) ? iy : iy + ih - 1) * MW
                                     + ix + 1 + rl_range(iw - 2)] = T_DOOR_CLOSED;
        else            g_lv.terrain[(iy + 1 + rl_range(ih - 2)) * MW
                                     + (rl_pct(50) ? ix : ix + iw - 1)] = T_DOOR_CLOSED;
        break;
    }
    default: break;
    }
}

/* Where a corridor should aim for when linking this room. Normally the centre;
 * for a vault the centre is INSIDE the sealed chamber, and tunnel() converts
 * wall to floor, so aiming there drills a second entrance and the vault stops
 * being a vault. Aim at the outer ring instead. */
static int link_x(const Room *r) { return r->kind == RM_VAULT ? r->x : r->x + r->w / 2; }
static int link_y(const Room *r) { return r->kind == RM_VAULT ? r->y : r->y + r->h / 2; }

/* Plain rectangle intersection. room_overlaps() inflates by one cell on every
 * side, which is right for scattered rooms and wrong for a budded one: it is
 * placed exactly one wall apart on purpose, and the fat test calls that a
 * collision. */
static int room_free(int x, int y, int w, int h) {
    for (int i = 0; i < s_nroom; i++) {
        Room *r = &s_room[i];
        if (x < r->x + r->w && r->x < x + w &&
            y < r->y + r->h && r->y < y + h) return 0;
    }
    return 1;
}

/* Two rooms with exactly one wall cell between them can be joined by knocking
 * that cell through, which is a doorway rather than a corridor. WolfMote builds
 * whole levels this way and has no corridors at all; here it is only ever some
 * of the joins, because a corridor you walk down is still worth having -- it is
 * the level made ENTIRELY of them that reads wrong.
 *
 * Returns 1 if the two share a wall and a door was punched. */
static int bud_door(const Room *a, const Room *b) {
    int lo, hi;
    /* vertical shared wall: b to the right of a, or the reverse */
    for (int k = 0; k < 2; k++) {
        const Room *l = k ? b : a, *r = k ? a : b;
        if (l->x + l->w != r->x - 1) continue;
        lo = l->y > r->y ? l->y : r->y;
        hi = (l->y + l->h < r->y + r->h ? l->y + l->h : r->y + r->h) - 1;
        if (hi < lo) continue;
        int y = lo + rl_range(hi - lo + 1);
        g_lv.terrain[y * MW + l->x + l->w] = rl_pct(45) ? T_DOOR_CLOSED : T_FLOOR;
        return 1;
    }
    /* horizontal shared wall */
    for (int k = 0; k < 2; k++) {
        const Room *t = k ? b : a, *u = k ? a : b;
        if (t->y + t->h != u->y - 1) continue;
        lo = t->x > u->x ? t->x : u->x;
        hi = (t->x + t->w < u->x + u->w ? t->x + t->w : u->x + u->w) - 1;
        if (hi < lo) continue;
        int x = lo + rl_range(hi - lo + 1);
        g_lv.terrain[(t->y + t->h) * MW + x] = rl_pct(45) ? T_DOOR_CLOSED : T_FLOOR;
        return 1;
    }
    return 0;
}

/* Try to seat a room flush against an existing one, one wall cell apart, so it
 * can be joined by a door instead of a corridor. Fills rm on success. */
static int place_budded(Room *rm, int w, int h) {
    if (!s_nroom) return 0;
    for (int t = 0; t < 30; t++) {
        int oi = rl_range(s_nroom);
        const Room *o = &s_room[oi];
        int side = rl_range(4), x, y;
        if (side == 0) { x = o->x + o->w + 1;  y = o->y - h + 2 + rl_range(o->h + h - 3); }
        else if (side == 1) { x = o->x - w - 1; y = o->y - h + 2 + rl_range(o->h + h - 3); }
        else if (side == 2) { y = o->y + o->h + 1; x = o->x - w + 2 + rl_range(o->w + w - 3); }
        else                { y = o->y - h - 1;    x = o->x - w + 2 + rl_range(o->w + w - 3); }
        if (x < 1 || y < 1 || x + w >= DUN_W - 1 || y + h >= DUN_H - 1) continue;
        if (!room_free(x, y, w, h)) continue;
        rm->x = (uint8_t)x; rm->y = (uint8_t)y;
        rm->w = (uint8_t)w; rm->h = (uint8_t)h;
        rm->bud = (uint8_t)(oi + 1);
        return 1;
    }
    return 0;
}

static int in_any_room(int x, int y) {
    for (int i = 0; i < s_nroom; i++) {
        Room *r = &s_room[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return 1;
    }
    return 0;
}

/* A corridor cell that touches a room, with wall on both sides across the
 * corridor's run, is a doorway. The tunnel() comment has claimed doors were
 * punched here since the first commit; nothing ever punched them, so
 * MK_OPEN_DOOR and the whole T_DOOR_CLOSED path were dead code. */
static void place_doors(int depth) {
    for (int y = 1; y < DUN_H - 1; y++) {
        for (int x = 1; x < DUN_W - 1; x++) {
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (in_any_room(x, y)) continue;
            int horiz = rl_ter(x - 1, y) == T_WALL && rl_ter(x + 1, y) == T_WALL;
            int vert  = rl_ter(x, y - 1) == T_WALL && rl_ter(x, y + 1) == T_WALL;
            if (!horiz && !vert) continue;
            int touches = in_any_room(x - 1, y) || in_any_room(x + 1, y) ||
                          in_any_room(x, y - 1) || in_any_room(x, y + 1);
            if (touches && rl_pct(55 + depth)) g_lv.terrain[y * MW + x] = T_DOOR_CLOSED;
        }
    }
}

/* Fallen stone in the corridors. It blocks movement AND sight, so a passage you
 * remember can stop being a passage -- which is the cheapest way to make the
 * route back matter. Never in a room: a blocked room is a stuck player. */
static void place_rubble(int depth) {
    int n = depth / 2 + rl_range(4);
    if (n > 14) n = 14;
    for (int i = 0; i < n; i++) {
        for (int tries = 0; tries < 40; tries++) {
            int x = 1 + rl_range(DUN_W - 2), y = 1 + rl_range(DUN_H - 2);
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (in_any_room(x, y)) continue;
            if (x == g_pl.x && y == g_pl.y) continue;
            /* only where the corridor is one tile wide, so a rubble pile never
             * seals the only route by filling a junction */
            int open = 0;
            if (rl_walkable(x - 1, y)) open++;
            if (rl_walkable(x + 1, y)) open++;
            if (rl_walkable(x, y - 1)) open++;
            if (rl_walkable(x, y + 1)) open++;
            if (open > 2) continue;
            g_lv.terrain[y * MW + x] = T_RUBBLE;
            break;
        }
    }
}

/* An L-shaped corridor, horizontal then vertical or the reverse -- but not a
 * pipe. A one-tile-wide run of exactly equal width for its whole length is what
 * makes a level read as corridors with rooms attached; every third cell or so
 * gets a neighbour opened as well, so a passage bulges, narrows and occasionally
 * runs two wide. Same topology, none of the drainpipe. */
static void widen(int x, int y) {
    static const int8_t dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    int d = rl_range(4);
    int nx = x + dx[d], ny = y + dy[d];
    if (nx > 0 && nx < DUN_W - 1 && ny > 0 && ny < DUN_H - 1 &&
        g_lv.terrain[ny * MW + nx] == T_WALL)
        g_lv.terrain[ny * MW + nx] = T_FLOOR;
}

static void tunnel(int x0, int y0, int x1, int y1) {
    int x = x0, y = y0;
    int horiz_first = rl_pct(50);
    for (;;) {
        if (rl_in(x, y) && g_lv.terrain[y * MW + x] == T_WALL)
            g_lv.terrain[y * MW + x] = T_FLOOR;
        if (rl_pct(34)) widen(x, y);
        if (x == x1 && y == y1) break;
        if (horiz_first) {
            if (x != x1) x += (x1 > x) ? 1 : -1;
            else         y += (y1 > y) ? 1 : -1;
        } else {
            if (y != y1) y += (y1 > y) ? 1 : -1;
            else         x += (x1 > x) ? 1 : -1;
        }
    }
}

static void place_stairs(void) {
    for (int tries = 0; tries < 500; tries++) {
        Room *r = &s_room[rl_range(s_nroom)];
        int x = r->x + rl_range(r->w), y = r->y + rl_range(r->h);
        if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
        g_lv.terrain[y * MW + x] = T_STAIR_DOWN;
        g_lv.down_x = (uint8_t)x; g_lv.down_y = (uint8_t)y;
        break;
    }
    for (int tries = 0; tries < 500; tries++) {
        Room *r = &s_room[rl_range(s_nroom)];
        int x = r->x + rl_range(r->w), y = r->y + rl_range(r->h);
        if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
        g_lv.terrain[y * MW + x] = T_STAIR_UP;
        g_lv.up_x = (uint8_t)x; g_lv.up_y = (uint8_t)y;
        break;
    }
}

/* Depth-weighted pick from the monster table: natives first, with a small
 * chance of something out of depth -- Moria's "you should not be seeing this
 * yet" moment, which is most of the tension. */
static int pick_mon(int depth) {
    int eff = depth + (rl_pct(10) ? 1 + rl_range(5) : 0);
    int best = 0;
    for (int tries = 0; tries < 20; tries++) {
        int k = rl_range(g_mon_kind_n);
        /* mimics are placed by place_chests, never rolled -- one standing in
         * the open with no chest under it gives the whole joke away */
        /* a mimic is placed as a chest, and a villager belongs in a town */
        if (g_mon_kind[k].flags & (MK_MIMIC | MK_PEACEFUL)) continue;
        if (g_mon_kind[k].lvl <= eff) { best = k; if (rl_pct(60)) break; }
    }
    return best;
}

/* Chests, and the things that are pretending to be chests.
 *
 * One to four a floor, always inside a room, never on the stairs. Roughly one in
 * six is a mimic instead: the monster is placed on the tile and the tile is left
 * as an ordinary floor, so what you see is the mimic drawing itself as a shut
 * chest. Walk up and open it and it opens you.
 *
 * The mimic kind is chosen by depth so the shallow floors get the weak one. */
static void place_chests(int depth) {
    int n = 1 + rl_range(3) + depth / 8;
    if (n > 5) n = 5;
    for (int i = 0; i < n; i++) {
        for (int tries = 0; tries < 60; tries++) {
            Room *r = &s_room[rl_range(s_nroom)];
            int x = r->x + rl_range(r->w), y = r->y + rl_range(r->h);
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (rl_mon_at(x, y) || rl_item_at(x, y)) continue;
            if (x == g_pl.x && y == g_pl.y) continue;

            if (rl_pct(17) && g_lv.n_mon < MAX_MON) {
                int k = -1;
                for (int j = 0; j < g_mon_kind_n; j++)
                    if ((g_mon_kind[j].flags & MK_MIMIC) &&
                        (k < 0 || (g_mon_kind[j].lvl <= depth &&
                                   g_mon_kind[j].lvl > g_mon_kind[k].lvl)))
                        k = j;
                const MonKind *mk = &g_mon_kind[k];
                Mon *m = &g_lv.mon[g_lv.n_mon++];
                m->x = (uint8_t)x; m->y = (uint8_t)y; m->kind = (uint8_t)k;
                m->mhp = m->hp = (int16_t)rl_dice(mk->hp_d, mk->hp_s);
                m->speed = mk->speed; m->energy = (int16_t)rl_range(100);
                m->flags = MF_ASLEEP;              /* asleep == still disguised */
                m->boss = 0;
            } else {
                g_lv.terrain[y * MW + x] = T_CHEST;
            }
            break;
        }
    }
}

/* Put one monster of kind `k` at (x,y). Returns 0 if the tile was no good. */
static int put_mon(int k, int x, int y, int asleep) {
    if (g_lv.n_mon >= MAX_MON) return 0;
    if (!rl_in(x, y) || g_lv.terrain[y * MW + x] != T_FLOOR) return 0;
    if (rl_mon_at(x, y)) return 0;
    if (x == g_pl.x && y == g_pl.y) return 0;
    const MonKind *mk = &g_mon_kind[k];
    Mon *m = &g_lv.mon[g_lv.n_mon++];
    m->x = (uint8_t)x; m->y = (uint8_t)y; m->kind = (uint8_t)k;
    m->mhp = m->hp = (int16_t)rl_dice(mk->hp_d, mk->hp_s);
    m->speed = mk->speed; m->energy = (int16_t)rl_range(100);
    m->flags = (uint8_t)(asleep ? MF_ASLEEP : 0);
    m->boss = 0;
    return 1;
}

static void spawn_monsters(int depth) {
    int n = 10 + rl_range(8) + depth / 2;
    if (n > MAX_MON - 4) n = MAX_MON - 4;
    for (int i = 0; i < n && g_lv.n_mon < MAX_MON - 4; i++) {
        for (int tries = 0; tries < 60; tries++) {
            Room *r = &s_room[rl_range(s_nroom)];
            int x = r->x + rl_range(r->w), y = r->y + rl_range(r->h);
            int k = pick_mon(depth);
            int asleep = rl_pct(60);
            if (!put_mon(k, x, y, asleep)) continue;
            /* MK_GROUP has been on forty-five of the hundred and seventy-two
             * kinds since the table was written and nothing has ever read it.
             * Jackals come in packs; that is the entire point of a jackal. */
            if (g_mon_kind[k].flags & MK_GROUP) {
                int pack = 2 + rl_range(4);
                for (int j = 0; j < pack; j++) {
                    int ox = x + rl_range(5) - 2, oy = y + rl_range(5) - 2;
                    put_mon(k, ox, oy, asleep);
                }
                i += pack;                    /* a pack counts against the budget */
            }
            break;
        }
    }
}

/* What is behind the vault door: chests, and something awake standing over
 * them. A sealed room you have to open, with a reason to open it. */
static void stock_vaults(int depth) {
    for (int i = 0; i < s_nroom; i++) {
        Room *r = &s_room[i];
        if (r->kind != RM_VAULT) continue;
        int ix = r->x + 2, iy = r->y + 2, iw = r->w - 4, ih = r->h - 4;
        if (iw < 1 || ih < 1) continue;
        for (int j = iy; j < iy + ih; j++) {
            for (int k = ix; k < ix + iw; k++) {
                if (g_lv.terrain[j * MW + k] != T_FLOOR) continue;
                if (rl_pct(45)) g_lv.terrain[j * MW + k] = T_CHEST;
                else if (rl_pct(40)) put_mon(pick_mon(depth + 4), k, j, 0);
            }
        }
    }
}

/* The boss for this depth, if any. One boss per guarded floor, placed in the
 * room furthest from the up-stair and awake -- it is the reason to come down,
 * so it should not be something you can miss. */
static void spawn_boss(int depth) {
    int bi = -1;
    for (int i = 0; i < g_boss_kind_n; i++) if (g_boss_kind[i].depth == depth) { bi = i; break; }
    if (bi < 0 || g_lv.n_mon >= MAX_MON) return;

    int best = 0, bestd = -1;
    for (int i = 0; i < s_nroom; i++) {
        int rx = s_room[i].x + s_room[i].w / 2, ry = s_room[i].y + s_room[i].h / 2;
        int dx = rx - g_pl.x, dy = ry - g_pl.y;
        int d = dx * dx + dy * dy;
        if (d > bestd) { bestd = d; best = i; }
    }
    Room *r = &s_room[best];
    /* the 2x2 sprite hangs up-and-left, so keep it off the room's top-left edge */
    int x = r->x + 1 + rl_range(r->w > 2 ? r->w - 1 : 1);
    int y = r->y + 1 + rl_range(r->h > 2 ? r->h - 1 : 1);
    if (!rl_in(x, y) || g_lv.terrain[y * MW + x] != T_FLOOR) {
        x = r->x + r->w / 2; y = r->y + r->h / 2;
    }

    const BossKind *bk = &g_boss_kind[bi];
    Mon *m = &g_lv.mon[g_lv.n_mon++];
    m->x = (uint8_t)x; m->y = (uint8_t)y;
    m->kind = 0;                        /* unused for a boss; `boss` drives it */
    m->boss = (uint8_t)(bi + 1);
    m->mhp = m->hp = (int16_t)bk->hp;
    m->speed = bk->speed;
    m->energy = 0;
    m->flags = 0;                       /* bosses do not sleep */
}

/* --- coloured levers ------------------------------------------------------
 *
 * A lever and the door it opens are placed as a PAIR, always on the same floor,
 * and the colour lives in the terrain id so neither half can lose track of the
 * other. The door goes on a corridor cell that a room opens onto -- somewhere
 * you will walk into and be stopped -- and the lever goes in a room far enough
 * away that finding it is the puzzle.
 *
 * The rule that makes this safe: a locked door is only ever placed where an
 * ordinary door already was. The generator has already proved the level
 * connects through that cell, and if it were a chokepoint the lever behind it
 * could seal the stairs away. Locking a door that was optional cannot strand
 * anybody.
 */
static const int DIRX[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
static const int DIRY[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };

/* Everything reachable from (sx,sy) without passing through `block`, staged in
 * g_lv.layer -- which the draw code rebuilds from terrain every frame, so it is
 * free scratch during generation. */
static void reach_from(int sx, int sy, int bx, int by) {
    for (int i = 0; i < MW * MH; i++) g_lv.layer[i] = 0;
    /* a dungeon is DUN_W x DUN_H of the buffer, so the queue only ever needs
     * that many entries -- sized to MW * MH it was 24 KB of BSS doing nothing */
    static uint16_t q[DUN_W * DUN_H];
    int head = 0, tail = 0;
    g_lv.layer[sy * MW + sx] = 1;
    q[tail++] = (uint16_t)(sy * MW + sx);
    while (head < tail) {
        int i = q[head++], x = i % MW, y = i / MW;
        for (int d = 0; d < 8; d++) {
            int nx = x + DIRX[d], ny = y + DIRY[d];
            if (!rl_in(nx, ny)) continue;
            if (nx == bx && ny == by) continue;          /* the shut door */
            int j = ny * MW + nx;
            uint8_t nt = g_lv.terrain[j];
            if (g_lv.layer[j]) continue;
            /* a shut door opens and rubble is cleared by walking into it */
            if (!rl_walkable(nx, ny) && nt != T_DOOR_CLOSED && nt != T_RUBBLE)
                continue;
            g_lv.layer[j] = 1;
            if (tail < (int)(sizeof q / sizeof q[0])) q[tail++] = (uint16_t)j;
        }
    }
}

/* At most ONE pair a floor, and not on every floor.
 *
 * The cap is a safety property, not a taste one. Two pairs can interlock -- the
 * red lever behind the blue door and the blue lever behind the red -- and no
 * amount of per-pair checking sees that, because each pair is fine on its own.
 * With one pair there is nothing to interlock with, and a single reachability
 * check settles it outright. */
static void place_levers(int depth) {
    if (!rl_pct(28 + depth)) return;
    {
        int hue = rl_range(LEVER_N);

        /* Half the time the lever opens a CHEST rather than a door -- a
         * strongbox in its own colour, sitting where you can see it and cannot
         * touch it. A door is a route; a chest is a reward, and a floor that
         * only ever locks routes makes the lever feel like an obstacle. */
        int as_chest = rl_pct(50);
        int dx = -1, dy = -1;
        for (int t = 0; t < 900; t++) {
            int x = 1 + rl_range(DUN_W - 2), y = 1 + rl_range(DUN_H - 2);
            if (as_chest) {
                if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
                if (!(g_lv.flags[y * MW + x] & CF_ROOM)) continue;
            } else if (g_lv.terrain[y * MW + x] != T_DOOR_CLOSED) continue;
            dx = x; dy = y; break;
        }
        if (dx < 0) return;

        /* The lever must be reachable WITHOUT passing the door it opens, or
         * the floor locks itself. Flood from the up stair with the door shut
         * and take a cell the flood actually touched -- guessing at distance
         * (the first cut) puts the lever behind its own door often enough to
         * matter.
         *
         * A chest blocks nothing, so there is no door to shut -- but the flood
         * still runs, because a lever you cannot walk to is no better than one
         * behind its own door. */
        reach_from(g_lv.up_x, g_lv.up_y, as_chest ? -1 : dx,
                                         as_chest ? -1 : dy);

        int lx = -1, ly = -1;
        for (int t = 0; t < 1500; t++) {
            int x = 1 + rl_range(DUN_W - 2), y = 1 + rl_range(DUN_H - 2);
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (!(g_lv.flags[y * MW + x] & CF_ROOM)) continue;   /* in a room */
            if (!g_lv.layer[y * MW + x]) continue;               /* our side */
            int ax = x - dx, ay = y - dy;
            if (ax * ax + ay * ay < 200) continue;               /* not beside it */
            lx = x; ly = y; break;
        }
        if (lx < 0) return;

        g_lv.terrain[dy * MW + dx] =
            (uint8_t)((as_chest ? T_CBOX_R : T_LOCK_R) + hue);
        g_lv.terrain[ly * MW + lx] = (uint8_t)(T_LEVER_R + hue);
        g_lv.flags[ly * MW + lx] &= (uint8_t)~CF_ROOM;           /* unthrown */
    }
}

/* Throw it: every door of that colour on the floor opens. Returns 0 if the
 * lever has already been pulled, so the caller does not spend a turn twice. */
/* The five, in the order both bands are drawn in. Read off the sheet, not
 * guessed: index 2 is GOLD and index 3 is green, which is the way round the
 * first cut had them swapped. */
const char *rl_hue_name(int hue) {
    static const char *const n[LEVER_N] = { "red", "blue", "gold",
                                            "green", "grey" };
    return n[hue < 0 || hue >= LEVER_N ? 0 : hue];
}

int rl_pull_lever(int x, int y) {
    uint8_t t = rl_ter(x, y);
    if (!T_IS_LEVER(t)) return 0;
    int i = y * MW + x;
    if (g_lv.flags[i] & CF_ROOM) { rl_msg("It is already thrown."); return 0; }
    g_lv.flags[i] |= CF_ROOM;

    int opened = 0, hue = T_LEVER_HUE(t);
    uint8_t door = (uint8_t)(T_LOCK_R + hue), box = (uint8_t)(T_CBOX_R + hue);
    for (int k = 0; k < MW * MH; k++) {
        if (g_lv.terrain[k] == door) { g_lv.terrain[k] = T_DOOR_OPEN; opened++; }
        /* a freed strongbox becomes an ordinary chest -- you still have to walk
         * to it and open it, and it still rolls its trap */
        else if (g_lv.terrain[k] == box) { g_lv.terrain[k] = T_CHEST; opened++; }
    }

    if (opened) rl_msgf("Something opens. (%d)", opened);
    else        rl_msg("It clunks. Nothing here.");
    rl_fov();
    return 1;
}

/* --- what you have seen ---------------------------------------------------
 *
 * The layout of a floor is reproducible from its seed, but what you have WALKED
 * is not -- and a floor you have half-explored, left, and come back to should
 * still be half-explored. Regenerating it handed you a fresh blanket of fog
 * every time, which quietly undoes the point of the floor persisting at all.
 *
 * CF_KNOWN is one bit a cell, so a whole floor's exploration is 384 bytes and
 * every floor in the game is eighteen kilobytes. That is cheap enough that
 * there is no argument for not doing it. */
#define SEEN_BYTES  ((DUN_W * DUN_H + 7) / 8)
#define SEEN_DEPTHS 48
static uint8_t s_seen[SEEN_DEPTHS][SEEN_BYTES];

/* The store itself, so the save can carry it. It is a flat block of bits with
 * no pointers, so it goes into the blob verbatim. */
void *rl_seen_blob(int *bytes) { *bytes = (int)sizeof s_seen; return s_seen; }

void rl_seen_wipe(void) {
    for (int d = 0; d < SEEN_DEPTHS; d++)
        for (int i = 0; i < SEEN_BYTES; i++) s_seen[d][i] = 0;
}

/* Fold the current floor's explored map into the store. OR rather than
 * overwrite: arriving by a different stair should not forget the far end. */
void rl_seen_store(int depth) {
    if (depth <= 0 || depth >= SEEN_DEPTHS) return;
    for (int y = 0; y < DUN_H; y++)
        for (int x = 0; x < DUN_W; x++)
            if (g_lv.flags[y * MW + x] & CF_KNOWN) {
                int b = y * DUN_W + x;
                s_seen[depth][b >> 3] |= (uint8_t)(1u << (b & 7));
            }
}

void rl_seen_load(int depth) {
    if (depth <= 0 || depth >= SEEN_DEPTHS) return;
    for (int y = 0; y < DUN_H; y++)
        for (int x = 0; x < DUN_W; x++) {
            int b = y * DUN_W + x;
            if (s_seen[depth][b >> 3] & (1u << (b & 7)))
                g_lv.flags[y * MW + x] |= CF_KNOWN;
        }
}

void rl_gen_level(int depth) {
    /* The LAYOUT is a function of the world and the depth, and nothing else, so
     * floor seven is always the floor seven you remember: you can climb out,
     * restock, and come back down to the same rooms, the same corridors and the
     * same lever. Delving, surfacing and re-delving is the shape of the game,
     * and it does not work if the map is new every time.
     *
     * What is NOT deterministic is what is standing in it. Monsters, floor loot
     * and vault stock roll from the live sequence below, so returning is
     * re-entering a place you know rather than re-running a fight you already
     * won. */
    uint32_t live = g_seed;
    g_seed = g_world_seed * 2654435761u + (uint32_t)depth * 40503u + 17u;
    if (!g_seed) g_seed = 1;

    for (int i = 0; i < MW * MH; i++) { g_lv.terrain[i] = T_WALL; g_lv.flags[i] = 0; }
    g_lv.n_mon = 0; g_lv.n_item = 0;
    s_nroom = 0;

    /* Room count and size vary by floor mood, so a level is a warren of little
     * cells or a handful of halls rather than always the same nine boxes. */
    int warren = rl_pct(30);
    int want = warren ? 13 + rl_range(7) : 7 + rl_range(6);
    for (int tries = 0; tries < 400 && s_nroom < want && s_nroom < MAX_ROOM; tries++) {
        int w = warren ? 3 + rl_range(4) : 4 + rl_range(8);
        int h = warren ? 3 + rl_range(3) : 3 + rl_range(6);
        Room *rm = &s_room[s_nroom];
        rm->bud = 0;
        /* Half the rooms bud straight off a neighbour and are joined by a
         * doorway; the rest are scattered and reached by corridor. That ratio
         * is the whole fix -- every room connected by its own corridor is what
         * made a floor read as a pipe network with chambers bolted on. */
        if (!(rl_pct(55) && place_budded(rm, w, h))) {
            int x = 1 + rl_range(DUN_W - w - 2), y = 1 + rl_range(DUN_H - h - 2);
            if (room_overlaps(x, y, w, h)) continue;
            rm->x = (uint8_t)x; rm->y = (uint8_t)y;
            rm->w = (uint8_t)w; rm->h = (uint8_t)h;
        }
        rm->kind = (uint8_t)((w >= 6 && h >= 5) ? roll_room_kind(depth) : RM_PLAIN);
        /* An unlit room has to be walked with a torch. Deeper floors have more
         * of them, and the first room is always lit -- arriving blind on the
         * up-stair reads as a bug, not as atmosphere. */
        rm->dark = (uint8_t)(s_nroom > 0 && rl_pct(12 + depth * 2));
        carve_room(rm);
        s_nroom++;
    }
    if (s_nroom == 0) {                       /* degenerate: one guaranteed room */
        carve_rect(MW / 2 - 4, MH / 2 - 3, 8, 6, T_FLOOR);
        s_room[0].x = (uint8_t)(MW / 2 - 4); s_room[0].y = (uint8_t)(MH / 2 - 3);
        s_room[0].w = 8; s_room[0].h = 6;
        s_room[0].kind = RM_PLAIN; s_room[0].dark = 0; s_room[0].bud = 0; s_nroom = 1;
    }
    /* mark lit room cells so lit-room FOV can reveal a whole chamber at once */
    for (int i = 0; i < s_nroom; i++) {
        if (s_room[i].dark) continue;
        for (int j = s_room[i].y; j < s_room[i].y + s_room[i].h; j++)
            for (int k = s_room[i].x; k < s_room[i].x + s_room[i].w; k++)
                g_lv.flags[j * MW + k] |= CF_ROOM;
    }

    /* A budded room already touches its host, so it gets a door knocked through
     * the shared wall. Everything else is chained by corridor as before. */
    for (int i = 1; i < s_nroom; i++) {
        Room *rm = &s_room[i];
        if (rm->bud && rm->kind != RM_VAULT &&
            bud_door(&s_room[rm->bud - 1], rm)) continue;
        tunnel(link_x(&s_room[i - 1]), link_y(&s_room[i - 1]),
               link_x(rm),             link_y(rm));
    }

    /* Extra links between random pairs. The chain above makes the level a path:
     * one way in, one way out, every room a dead end but two. A couple of loops
     * turns "walk the corridor" into "which way round", and gives you somewhere
     * to run to when the thing behind you is faster than you are. */
    int loops = 1 + rl_range(3);
    for (int i = 0; i < loops && s_nroom > 2; i++) {
        int a = rl_range(s_nroom), b = rl_range(s_nroom);
        if (a == b) continue;
        tunnel(link_x(&s_room[a]), link_y(&s_room[a]),
               link_x(&s_room[b]), link_y(&s_room[b]));
    }

    place_doors(depth);
    place_stairs();

    /* the player arrives on the up-stair (or the first room if it failed) */
    g_pl.x = g_lv.up_x ? g_lv.up_x : (uint8_t)(s_room[0].x + s_room[0].w / 2);
    g_pl.y = g_lv.up_y ? g_lv.up_y : (uint8_t)(s_room[0].y + s_room[0].h / 2);

    /* the rest of the architecture, still on the layout seed */
    place_rubble(depth);
    place_levers(depth);
    place_chests(depth);          /* a mimic is furniture, and stays put */
    stock_vaults(depth);          /* a vault's strongbox is part of the vault */

    /* --- and from here, the living contents ----------------------------
     * Nothing below this line may touch terrain. stock_vaults sat here at
     * first and it lays chest TILES, so a handful of cells moved between
     * visits and the floor was almost, but not quite, the one you left. */
    g_seed = live;
    spawn_monsters(depth);
    spawn_boss(depth);
    rl_scatter_items(depth);
    rl_fov();
}

/* --- field of view ------------------------------------------------------ */
/* Symmetric shadowcasting would be nicer, but a bounded ray cast per boundary
 * cell is small, has no recursion, and at light radius <= 8 the artefacts are
 * not visible on a 16x13 viewport. */
static void cast_ray(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int n = adx > ady ? adx : ady;
    if (n == 0) return;
    int fx = (dx << 8) / n, fy = (dy << 8) / n;
    int px = x0 << 8, py = y0 << 8;
    for (int i = 0; i <= n; i++) {
        int cx = (px + 128) >> 8, cy = (py + 128) >> 8;
        if (!rl_in(cx, cy)) return;
        g_lv.flags[cy * MW + cx] |= CF_KNOWN | CF_VISIBLE;
        if (rl_opaque(cx, cy)) return;
        px += fx; py += fy;
    }
}

void rl_fov(void) {
    for (int i = 0; i < MW * MH; i++) g_lv.flags[i] &= (uint8_t)~CF_VISIBLE;
    int r = rl_player_light();
    if (r < 1) r = 1;
    int x0 = g_pl.x, y0 = g_pl.y;
    g_lv.flags[y0 * MW + x0] |= CF_KNOWN | CF_VISIBLE;
    for (int d = -r; d <= r; d++) {
        cast_ray(x0, y0, x0 + d, y0 - r);
        cast_ray(x0, y0, x0 + d, y0 + r);
        cast_ray(x0, y0, x0 - r, y0 + d);
        cast_ray(x0, y0, x0 + r, y0 + d);
    }
    /* standing in a lit room reveals the whole room, as in Moria */
    if (g_lv.flags[y0 * MW + x0] & CF_ROOM) {
        for (int i = 0; i < s_nroom; i++) {
            Room *rm = &s_room[i];
            if (x0 < rm->x || x0 >= rm->x + rm->w || y0 < rm->y || y0 >= rm->y + rm->h) continue;
            for (int j = rm->y - 1; j <= rm->y + rm->h; j++)
                for (int k = rm->x - 1; k <= rm->x + rm->w; k++)
                    if (rl_in(k, j)) g_lv.flags[j * MW + k] |= CF_KNOWN | CF_VISIBLE;
        }
    }
}
