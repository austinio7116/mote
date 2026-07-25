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
        return 1;
    default:
        return 0;
    }
}

int rl_opaque(int x, int y) {
    uint8_t t = rl_ter(x, y);
    return t == T_WALL || t == T_DOOR_CLOSED || t == T_RUBBLE || t == T_MOUNTAIN;
}

Mon *rl_mon_at(int x, int y) {
    for (int i = 0; i < g_lv.n_mon; i++)
        if (g_lv.mon[i].hp > 0 && g_lv.mon[i].x == x && g_lv.mon[i].y == y)
            return &g_lv.mon[i];
    return 0;
}

/* --- generation --------------------------------------------------------- */
#define MAX_ROOM 20
typedef struct { uint8_t x, y, w, h; } Room;
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

/* An L-shaped corridor: horizontal then vertical, or the reverse. Doors are
 * punched where a corridor meets a room wall. */
static void tunnel(int x0, int y0, int x1, int y1) {
    int x = x0, y = y0;
    int horiz_first = rl_pct(50);
    for (;;) {
        if (rl_in(x, y) && g_lv.terrain[y * MW + x] == T_WALL)
            g_lv.terrain[y * MW + x] = T_FLOOR;
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
        if (g_mon_kind[k].lvl <= eff) { best = k; if (rl_pct(60)) break; }
    }
    return best;
}

static void spawn_monsters(int depth) {
    int n = 10 + rl_range(8) + depth / 2;
    if (n > MAX_MON - 4) n = MAX_MON - 4;
    for (int i = 0; i < n; i++) {
        for (int tries = 0; tries < 60; tries++) {
            Room *r = &s_room[rl_range(s_nroom)];
            int x = r->x + rl_range(r->w), y = r->y + rl_range(r->h);
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (rl_mon_at(x, y)) continue;
            if (x == g_pl.x && y == g_pl.y) continue;
            int k = pick_mon(depth);
            const MonKind *mk = &g_mon_kind[k];
            Mon *m = &g_lv.mon[g_lv.n_mon++];
            m->x = (uint8_t)x; m->y = (uint8_t)y; m->kind = (uint8_t)k;
            m->mhp = m->hp = (int16_t)rl_dice(mk->hp_d, mk->hp_s);
            m->speed = mk->speed; m->energy = (int16_t)rl_range(100);
            m->flags = rl_pct(60) ? MF_ASLEEP : 0;
            m->boss = 0;
            break;
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

void rl_gen_level(int depth) {
    for (int i = 0; i < MW * MH; i++) { g_lv.terrain[i] = T_WALL; g_lv.flags[i] = 0; }
    g_lv.n_mon = 0; g_lv.n_item = 0;
    s_nroom = 0;

    int want = 8 + rl_range(6);
    for (int tries = 0; tries < 300 && s_nroom < want && s_nroom < MAX_ROOM; tries++) {
        int w = 4 + rl_range(7), h = 3 + rl_range(5);
        int x = 1 + rl_range(MW - w - 2), y = 1 + rl_range(MH - h - 2);
        if (room_overlaps(x, y, w, h)) continue;
        carve_rect(x, y, w, h, T_FLOOR);
        s_room[s_nroom].x = (uint8_t)x; s_room[s_nroom].y = (uint8_t)y;
        s_room[s_nroom].w = (uint8_t)w; s_room[s_nroom].h = (uint8_t)h;
        s_nroom++;
    }
    if (s_nroom == 0) {                       /* degenerate: one guaranteed room */
        carve_rect(MW / 2 - 4, MH / 2 - 3, 8, 6, T_FLOOR);
        s_room[0].x = (uint8_t)(MW / 2 - 4); s_room[0].y = (uint8_t)(MH / 2 - 3);
        s_room[0].w = 8; s_room[0].h = 6; s_nroom = 1;
    }
    /* mark room cells so lit-room FOV can reveal a whole chamber at once */
    for (int i = 0; i < s_nroom; i++)
        for (int j = s_room[i].y; j < s_room[i].y + s_room[i].h; j++)
            for (int k = s_room[i].x; k < s_room[i].x + s_room[i].w; k++)
                g_lv.flags[j * MW + k] |= CF_ROOM;

    for (int i = 1; i < s_nroom; i++)
        tunnel(s_room[i - 1].x + s_room[i - 1].w / 2, s_room[i - 1].y + s_room[i - 1].h / 2,
               s_room[i].x + s_room[i].w / 2,         s_room[i].y + s_room[i].h / 2);

    place_stairs();

    /* the player arrives on the up-stair (or the first room if it failed) */
    g_pl.x = g_lv.up_x ? g_lv.up_x : (uint8_t)(s_room[0].x + s_room[0].w / 2);
    g_pl.y = g_lv.up_y ? g_lv.up_y : (uint8_t)(s_room[0].y + s_room[0].h / 2);

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
    int r = g_pl.light;
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
