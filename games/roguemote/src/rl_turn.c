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

/* --- monster stat accessors ---------------------------------------------
 * A boss's stats come from g_boss_kind, an ordinary monster's from
 * g_mon_kind. Every read goes through these so `m->kind` is never dereferenced
 * for a boss (whose kind field is meaningless). */
static const char *mon_name(const Mon *m) {
    return m->boss ? g_boss_kind[m->boss - 1].name : g_mon_kind[m->kind].name;
}
static int mon_ac(const Mon *m) {
    return m->boss ? g_boss_kind[m->boss - 1].ac : g_mon_kind[m->kind].ac;
}
static int mon_lvl(const Mon *m) {
    return m->boss ? g_boss_kind[m->boss - 1].depth : g_mon_kind[m->kind].lvl;
}
static int mon_flags(const Mon *m) {
    return m->boss ? (MK_EVIL | MK_OPEN_DOOR) : g_mon_kind[m->kind].flags;
}
static void mon_dam_dice(const Mon *m, int *d, int *s) {
    if (m->boss) { *d = g_boss_kind[m->boss - 1].dam_d; *s = g_boss_kind[m->boss - 1].dam_s; }
    else         { *d = g_mon_kind[m->kind].dam_d;      *s = g_mon_kind[m->kind].dam_s; }
}
static int32_t mon_xp(const Mon *m) {
    if (m->boss) return g_boss_kind[m->boss - 1].xp;
    const MonKind *mk = &g_mon_kind[m->kind];
    return (int32_t)mk->xp * (int32_t)(mk->lvl ? mk->lvl : 1) / 2 + mk->xp;
}

/* --- combat ------------------------------------------------------------- */
static int player_to_hit(void) {
    int h = 30 + g_pl.level * 3 + g_pl.stat[3];
    if (g_pl.inv_wield >= 0) h += g_pl.inv[g_pl.inv_wield].to_hit * 3;
    return h;
}

/* Weapon dice + STR, then the ego multiplier if the target qualifies. Moria's
 * "slay" multipliers are what make a +2 Holy weapon feel different from a +4
 * plain one at the same enchantment. */
static int player_damage(const Mon *m) {
    int d, s, bonus;
    rl_player_weapon_dice(&d, &s, &bonus);
    int dam = rl_dice(d, s) + bonus + g_pl.stat[0] / 4;

    if (g_pl.inv_wield >= 0) {
        const Item *w = &g_pl.inv[g_pl.inv_wield];
        if (w->ego) {
            const EgoKind *e = &g_ego_kind[w->ego];
            int mf = mon_flags(m);
            int applies =
                (e->flags & EGO_SLAY_EVIL) ? (mf & MK_EVIL) :
                (e->flags & EGO_CURSED)    ? 1 :
                (e->flags & (EGO_FIRE | EGO_COLD | EGO_ELEC)) ? 1 :
                (e->mult > 0);
            if (applies && e->mult > 0) dam = dam * e->mult / 3;
            if (e->flags & EGO_CURSED)  dam = dam / 2 + 1;
        }
    }
    return dam < 1 ? 1 : dam;
}

/* Number of blows per turn: DEX and an "of Attacks" ego buy extra swings. */
static int player_blows(void) {
    int n = 1 + g_pl.stat[3] / 12;
    if (g_pl.inv_wield >= 0 && (g_ego_kind[g_pl.inv[g_pl.inv_wield].ego].flags & EGO_XATTACK)) n++;
    return n > 4 ? 4 : n;
}

void rl_gain_xp(int32_t amount) {
    g_pl.xp += amount;
    /* a simple compounding curve; deep enough to feel earned, cheap to evaluate */
    int32_t need = 20;
    int lvl = 1;
    while (lvl < 50 && g_pl.xp >= need) { lvl++; need += need / 2 + 10; }
    if (lvl > g_pl.level) {
        int gained = lvl - g_pl.level;
        const ClassKind *ck = &g_class[g_pl.cls];
        g_pl.level = (uint8_t)lvl;
        g_pl.mhp = (int16_t)(g_pl.mhp + gained * (ck->hp_bonus + g_pl.stat[4] / 4));
        g_pl.msp = (int16_t)(g_pl.msp + gained * (ck->sp_bonus + g_pl.stat[1] / 6));
        g_pl.hp = g_pl.mhp; g_pl.sp = g_pl.msp;
        rl_msgf("Welcome to level %d.", lvl);
    }
}

void rl_kill_mon(Mon *m) {
    rl_msg2(mon_name(m), " dies.");
    m->hp = 0;
    g_pl.kills++;
    rl_gain_xp(mon_xp(m));

    if (m->boss) {
        const BossKind *bk = &g_boss_kind[m->boss - 1];
        g_pl.bosses_slain++;
        g_pl.gold += 200 + bk->depth * 60;
        if (g_lv.n_item < MAX_ITEM) {
            Item *it = &g_lv.item[g_lv.n_item++];
            rl_make_item_kind(it, bk->drop, bk->depth + 10);   /* well enchanted */
            it->x = m->x; it->y = m->y;
        }
        rl_msg2(bk->name, " falls!");
        return;
    }
    /* ordinary monsters sometimes leave coin or a trinket */
    if (rl_pct(22)) g_pl.gold += 2 + rl_range(4 + mon_lvl(m) * 3);
    else if (rl_pct(10) && g_lv.n_item < MAX_ITEM) {
        Item *it = &g_lv.item[g_lv.n_item++];
        rl_make_item(it, mon_lvl(m));
        it->x = m->x; it->y = m->y;
    }
}

void rl_attack_mon(Mon *m) {
    m->flags &= (uint8_t)~MF_ASLEEP;
    int blows = player_blows(), hits = 0, total = 0;
    for (int b = 0; b < blows && m->hp > 0; b++) {
        int roll = rl_range(100) + 1;
        if (roll > player_to_hit() - mon_ac(m) * 2) continue;
        int dam = player_damage(m);
        total += dam; hits++;
        m->hp = (int16_t)(m->hp - dam);
    }
    if (!hits) { rl_msg("You miss."); return; }
    if (m->hp <= 0) rl_kill_mon(m);
    else            rl_msgf("You hit for %d.", total);
}

void rl_mon_attack_player(Mon *m) {
    int roll = rl_range(100) + 1;
    if (roll > 45 + mon_lvl(m) * 2 - rl_player_ac()) { rl_msg2(mon_name(m), " misses."); return; }
    int d, s; mon_dam_dice(m, &d, &s);
    int dam = rl_dice(d, s);
    /* armour soaks a slice of every landed blow, so AC is never wasted */
    dam -= rl_player_ac() / 6;
    if (dam < 1) dam = 1;
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
    int mf = mon_flags(m);

    if (m->flags & MF_ASLEEP) {
        int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
        int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        /* a mimic is not asleep, it is waiting, and it does not give itself away
         * to a noise across the room -- it springs when you are within reach */
        if (mf & MK_MIMIC) {
            if (d <= 1) {
                m->flags &= (uint8_t)~MF_ASLEEP;
                rl_msg("The chest has teeth!");
                rl_mon_attack_player(m);
            }
            return;
        }
        /* noise wakes them: closer means likelier, and it is never certain */
        if (d < 12 && rl_pct(30 - d * 2)) m->flags &= (uint8_t)~MF_ASLEEP;
        return;
    }
    if (mf & MK_NEVER_MOVE) {
        int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) rl_mon_attack_player(m);
        return;
    }

    int tx = m->x, ty = m->y;
    int erratic = (mf & MK_ERRATIC) && rl_pct(35);
    if (mon_hunts(m) && !erratic) {
        tx += step_toward(m->x, g_pl.x);
        ty += step_toward(m->y, g_pl.y);
    } else {
        tx += rl_range(3) - 1;
        ty += rl_range(3) - 1;
    }
    if (tx == g_pl.x && ty == g_pl.y) { rl_mon_attack_player(m); return; }
    if (!rl_walkable(tx, ty)) {
        if ((mf & MK_OPEN_DOOR) && rl_ter(tx, ty) == T_DOOR_CLOSED)
            g_lv.terrain[ty * MW + tx] = T_DOOR_OPEN;
        return;
    }
    if (rl_mon_at(tx, ty)) return;
    m->x = (uint8_t)tx; m->y = (uint8_t)ty;
}

/* --- world tick --------------------------------------------------------- */
void rl_world_tick(void) {
    g_turn++;

    /* haste expires; a ring of speed sets haste absurdly high so it never does */
    if (g_pl.haste > 0 && --g_pl.haste == 0) {
        g_pl.speed = SPEED_NORMAL;
        rl_msg("You slow down.");
    }

    int regen = 16;
    if (g_pl.inv_ring >= 0 && g_item_kind[g_pl.inv[g_pl.inv_ring].kind].eff == EF_R_REGEN) regen = 5;
    if ((g_turn % (uint32_t)regen) == 0) {
        if (g_pl.hp < g_pl.mhp) g_pl.hp++;
        if (g_pl.sp < g_pl.msp) g_pl.sp++;
    }

    /* hunger keeps the clock honest; regeneration burns food faster */
    if ((g_turn & 7) == 0 && g_pl.food > 0) g_pl.food -= (regen == 5) ? 2 : 1;
    if (g_pl.food < 0) g_pl.food = 0;
    if (g_pl.food == 0 && (g_turn & 31) == 0) {
        g_pl.hp--;
        if ((g_turn & 127) == 0) rl_msg("You are starving!");
    }
}
