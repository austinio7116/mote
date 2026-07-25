/*
 * The turn engine, monster AI and melee combat.
 *
 * Energy model (Moria/Angband): every actor gains energy each tick from its
 * speed, and acts when it reaches 100. Speed 110 gains 10, so +10 speed is
 * exactly double actions -- which is what makes haste/slow dramatic rather
 * than a percentage nobody feels.
 */
#include "rl.h"

/* Angband's extract_energy curve, trimmed to the range we use. */
int rl_speed_gain(int speed) {
    static const uint8_t tbl[] = {
        /* 90..99  */  1,  1,  1,  1,  2,  2,  2,  2,  2,  3,
        /* 100..109*/  3,  3,  3,  3,  4,  4,  4,  4,  5,  5,
        /* 110..119*/ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        /* 120..129*/ 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    };
    int i = speed - 90;
    if (i < 0) i = 0;
    if (i >= (int)(sizeof tbl)) i = (int)(sizeof tbl) - 1;
    return tbl[i];
}

/* --- combat ------------------------------------------------------------- */
static int player_to_hit(void) { return 30 + g_pl.level * 3 + g_pl.stat[3]; }
static int player_damage(void) { return rl_dice(1, 6) + g_pl.stat[0] / 4; }

void rl_gain_xp(int32_t amount) {
    g_pl.xp += amount;
    /* a simple doubling curve; deep enough to feel earned, cheap to evaluate */
    int32_t need = 20;
    int lvl = 1;
    while (lvl < 50 && g_pl.xp >= need) { lvl++; need += need / 2 + 10; }
    if (lvl > g_pl.level) {
        int gained = lvl - g_pl.level;
        g_pl.level = (uint8_t)lvl;
        g_pl.mhp = (int16_t)(g_pl.mhp + gained * (4 + g_pl.stat[4] / 4));
        g_pl.msp = (int16_t)(g_pl.msp + gained * (1 + g_pl.stat[1] / 6));
        g_pl.hp = g_pl.mhp; g_pl.sp = g_pl.msp;
        rl_msgf("Welcome to level %d.", lvl);
    }
}

void rl_kill_mon(Mon *m) {
    const MonKind *mk = &g_mon_kind[m->kind];
    rl_msg2(mk->name, " dies.");
    m->hp = 0;
    rl_gain_xp((int32_t)mk->xp * (int32_t)(mk->lvl ? mk->lvl : 1) / 2 + mk->xp);
}

void rl_attack_mon(Mon *m) {
    const MonKind *mk = &g_mon_kind[m->kind];
    m->flags &= (uint8_t)~MF_ASLEEP;
    int roll = rl_range(100) + 1;
    if (roll > player_to_hit() - mk->ac * 2) {
        rl_msg("You miss.");
        return;
    }
    int dam = player_damage();
    m->hp = (int16_t)(m->hp - dam);
    if (m->hp <= 0) rl_kill_mon(m);
    else            rl_msgf("You hit for %d.", dam);
}

void rl_mon_attack_player(Mon *m) {
    const MonKind *mk = &g_mon_kind[m->kind];
    int roll = rl_range(100) + 1;
    if (roll > 40 + mk->lvl * 2) { rl_msg2(mk->name, " misses."); return; }
    int dam = rl_dice(mk->dam_d, mk->dam_s);
    g_pl.hp = (int16_t)(g_pl.hp - dam);
    rl_msgf("Hit for %d!", dam);
}

/* --- monster AI --------------------------------------------------------- */
/* A monster hunts on its OWN senses. Keying this off the player's field of
 * view (the obvious shortcut) means a monster only advances while you can
 * already see it, so nothing ever closes on you and the level feels deserted --
 * which is exactly how the first build played. */
static int mon_hunts(const Mon *m) {
    int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int d = dx > dy ? dx : dy;                  /* Chebyshev: 8-way movement */
    if (d <= 1) return 1;                       /* adjacent: always */
    if (g_lv.flags[m->y * MW + m->x] & CF_VISIBLE) return 1;   /* mutual sight */
    return d <= 14;                             /* hearing/scent */
}

static int step_toward(int from, int to) { return from < to ? 1 : (from > to ? -1 : 0); }

void rl_mon_turn(Mon *m) {
    const MonKind *mk = &g_mon_kind[m->kind];

    if (m->flags & MF_ASLEEP) {
        /* noise wakes them: closer means likelier, and it is never certain */
        int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
        int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d < 12 && rl_pct(30 - d * 2)) m->flags &= (uint8_t)~MF_ASLEEP;
        return;
    }
    if (mk->flags & MK_NEVER_MOVE) {
        int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) rl_mon_attack_player(m);
        return;
    }

    int tx = m->x, ty = m->y;
    int erratic = (mk->flags & MK_ERRATIC) && rl_pct(35);
    if (mon_hunts(m) && !erratic) {
        tx += step_toward(m->x, g_pl.x);
        ty += step_toward(m->y, g_pl.y);
    } else {
        tx += rl_range(3) - 1;
        ty += rl_range(3) - 1;
    }
    if (tx == g_pl.x && ty == g_pl.y) { rl_mon_attack_player(m); return; }
    if (!rl_walkable(tx, ty)) {
        if ((mk->flags & MK_OPEN_DOOR) && rl_ter(tx, ty) == T_DOOR_CLOSED) {
            g_lv.terrain[ty * MW + tx] = T_DOOR_OPEN;
            return;
        }
        return;
    }
    if (rl_mon_at(tx, ty)) return;
    m->x = (uint8_t)tx; m->y = (uint8_t)ty;
}

/* --- world tick --------------------------------------------------------- */
void rl_world_tick(void) {
    g_turn++;
    /* regeneration, slow and depth-agnostic; hunger keeps the clock honest */
    if ((g_turn & 15) == 0) {
        if (g_pl.hp < g_pl.mhp) g_pl.hp++;
        if (g_pl.sp < g_pl.msp) g_pl.sp++;
    }
    if ((g_turn & 7) == 0 && g_pl.food > 0) g_pl.food--;
    if (g_pl.food == 0 && (g_turn & 31) == 0) {
        g_pl.hp--;
        if ((g_turn & 127) == 0) rl_msg("You are starving!");
    }
}
