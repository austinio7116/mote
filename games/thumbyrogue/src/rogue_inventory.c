#include "rogue_inventory.h"
#include "rogue_stats.h"
#include "craft_font.h"
#include "craft_types.h"
#include <stdio.h>
#include <string.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

static RogueItem s_bag[ROGUE_BAG_N];
static int  s_bag_n;
static bool s_open;
static int  s_cur;        /* 0..5 = paperdoll, 6.. = backpack item */

/* Item-detail sub-screen (opened with B on a selected item): a focused page
 * with Equip/Unequip, Socket-a-gem and Salvage. Salvage asks to confirm. */
static bool s_detail;     /* detail page open */
static int  s_dopt;       /* highlighted action on the detail page */
static bool s_confirm;    /* salvage confirmation pending */
static bool s_gempick;    /* gem-picker open (choosing a gem to socket) */
static int  s_gemcur;     /* cursor within the gem list */

/* Paperdoll display order (row-major, 2 cols × 3 rows). */
static const EquipSlot PD[6] = {
    SLOT_WEAPON, SLOT_HELM, SLOT_OFFHAND, SLOT_AMULET, SLOT_ARMOR, SLOT_RING
};

static void detail_reset(void) { s_detail = s_confirm = s_gempick = false; s_dopt = 0; }
void rogue_inventory_clear(void) { s_bag_n = 0; s_open = false; s_cur = 0; detail_reset(); }
int  rogue_inventory_count(void) { return s_bag_n; }
bool rogue_inventory_full(void)  { return s_bag_n >= ROGUE_BAG_N; }
bool rogue_inventory_is_open(void){ return s_open; }
bool rogue_inventory_detail_open(void) { return s_detail || s_gempick || s_confirm; }
void rogue_inventory_open(void)  { s_open = true; s_cur = 0; detail_reset(); }
void rogue_inventory_close(void) { s_open = false; detail_reset(); }

int rogue_inventory_export(RogueItem *out, int max) {
    int n = s_bag_n < max ? s_bag_n : max;
    for (int i = 0; i < n; i++) out[i] = s_bag[i];
    return n;
}
void rogue_inventory_import(const RogueItem *in, int n) {
    if (n > ROGUE_BAG_N) n = ROGUE_BAG_N;
    for (int i = 0; i < n; i++) s_bag[i] = in[i];
    s_bag_n = n;
}

bool rogue_inventory_add(const RogueItem *it) {
    if (s_bag_n >= ROGUE_BAG_N) return false;
    s_bag[s_bag_n++] = *it;
    return true;
}
static void bag_remove(int k) {
    for (int i = k; i < s_bag_n - 1; i++) s_bag[i] = s_bag[i + 1];
    s_bag_n--;
}

static int salvage_gold(const RogueItem *it) {
    return 4 + (int)it->rarity * 6 + it->n_affix * 2;
}

static bool edge(bool n, bool p) { return n && !p; }

/* Item currently selected in the grid (NULL for an empty paperdoll cell). */
static RogueItem *viewed(RoguePlayer *p) {
    if (s_cur < 6) { RogueItem *e = &p->equip[PD[s_cur]]; return (e->kind != ITEM_NONE) ? e : NULL; }
    int k = s_cur - 6;
    return (k < s_bag_n) ? &s_bag[k] : NULL;
}
static int gem_count(void) {
    int c = 0; for (int k = 0; k < s_bag_n; k++) if (s_bag[k].kind == ITEM_GEM) c++;
    return c;
}
static int gem_bag_index(int nth) {
    int c = 0; for (int k = 0; k < s_bag_n; k++)
        if (s_bag[k].kind == ITEM_GEM) { if (c == nth) return k; c++; }
    return -1;
}
static int item_free_sockets(const RogueItem *it) {
    int f = 0; for (int g = 0; g < it->sockets && g < 2; g++) if (it->gem[g] == GEM_NONE) f++;
    return f;
}

/* Actions offered on the detail page for the viewed item. */
enum { ACT_EQUIP, ACT_UNEQUIP, ACT_USE, ACT_SOCKET, ACT_SALVAGE, ACT_BACK };
static int build_actions(const RoguePlayer *p, int *acts) {
    int n = 0; bool eq = (s_cur < 6);
    const RogueItem *it = eq ? &p->equip[PD[s_cur]] : &s_bag[s_cur - 6];
    if (rogue_item_is_equip(it)) {
        acts[n++] = eq ? ACT_UNEQUIP : ACT_EQUIP;
        if (item_free_sockets(it) > 0) acts[n++] = ACT_SOCKET;
        acts[n++] = ACT_SALVAGE;
    } else if (it->kind == ITEM_POTION || it->kind == ITEM_TORCH) {
        acts[n++] = ACT_USE;
    } else if (it->kind == ITEM_GEM) {
        acts[n++] = ACT_SALVAGE;
    }
    acts[n++] = ACT_BACK;
    return n;
}

static void clamp_cur(void) {
    int total = 6 + s_bag_n;
    if (s_cur >= total) s_cur = total - 1;
    if (s_cur < 0) s_cur = 0;
}

#define BP_COLS 7
/* Directional grid navigation. The paperdoll is a 2x3 block (cur 0..5); the
 * backpack is a 7-wide grid (cur 6..). Moving down off the paperdoll drops
 * into the backpack top row and vice-versa; moves never land on empty bag
 * cells. dx/dy are -1/0/+1. */
static void grid_nav(int dx, int dy) {
    if (s_cur < 6) {                                  /* --- paperdoll --- */
        int col = s_cur & 1, row = s_cur >> 1;
        if (dx) { col += dx; if (col < 0 || col > 1) return; s_cur = row * 2 + col; return; }
        if (dy < 0) { if (row > 0) s_cur = (row - 1) * 2 + col; return; }
        if (dy > 0) {
            if (row < 2) { s_cur = (row + 1) * 2 + col; return; }
            int bp = (col == 0) ? 1 : 4;              /* drop into backpack top row */
            if (bp < s_bag_n) s_cur = 6 + bp;
            else if (s_bag_n > 0) s_cur = 6 + s_bag_n - 1;
            return;
        }
        return;
    }
    int k = s_cur - 6, col = k % BP_COLS, row = k / BP_COLS;  /* --- backpack --- */
    if (dx) {
        int nc = col + dx;
        if (nc < 0 || nc >= BP_COLS) return;
        int t = row * BP_COLS + nc;
        if (t < s_bag_n) s_cur = 6 + t;
        return;
    }
    if (dy < 0) {
        if (row > 0) { s_cur = 6 + (row - 1) * BP_COLS + col; return; }
        s_cur = (col < 4) ? 4 : 5;                    /* up into paperdoll bottom row */
        return;
    }
    if (dy > 0) {
        int t = (row + 1) * BP_COLS + col;
        if (t < s_bag_n) s_cur = 6 + t;
        return;
    }
}

void rogue_inventory_input(RoguePlayer *p, const CraftRawButtons *btn,
                           const CraftRawButtons *prev) {
    bool up = edge(btn->up,prev->up), dn = edge(btn->down,prev->down);
    bool lf = edge(btn->left,prev->left), rt = edge(btn->right,prev->right);
    bool a = edge(btn->a,prev->a), b = edge(btn->b,prev->b);

    /* --- gem picker (socket a gem into the viewed item) --- */
    if (s_gempick) {
        int ng = gem_count();
        if (up || lf) s_gemcur--;
        if (dn || rt) s_gemcur++;
        if (ng <= 0) s_gemcur = 0;
        else { if (s_gemcur < 0) s_gemcur = ng - 1; if (s_gemcur >= ng) s_gemcur = 0; }
        if (a && ng > 0) {
            int gi = gem_bag_index(s_gemcur);
            RogueItem *it = viewed(p);
            if (gi >= 0 && it && rogue_item_is_equip(it)) {
                GemType g = (GemType)s_bag[gi].amount;
                bool done = false;
                for (int s = 0; s < it->sockets && s < 2; s++)
                    if (it->gem[s] == GEM_NONE) { it->gem[s] = (uint8_t)g; done = true; break; }
                if (done) {
                    bool eq = (s_cur < 6); int bi = eq ? -1 : s_cur - 6;
                    bag_remove(gi);
                    if (!eq && gi < bi) s_cur--;     /* viewed bag item shifted down */
                    if (eq) rogue_player_recompute(p);
                    s_gempick = false;
                }
            }
        }
        if (b) s_gempick = false;
        return;
    }

    /* --- salvage confirmation --- */
    if (s_confirm) {
        if (a) {
            if (s_cur < 6) { EquipSlot sl = PD[s_cur];
                p->gold += salvage_gold(&p->equip[sl]);
                p->equip[sl].kind = ITEM_NONE; rogue_player_recompute(p);
            } else { int k = s_cur - 6;
                p->gold += salvage_gold(&s_bag[k]); bag_remove(k);
            }
            s_confirm = false; s_detail = false; clamp_cur();
        }
        if (b) s_confirm = false;
        return;
    }

    /* --- detail page --- */
    if (s_detail) {
        if (!viewed(p)) { s_detail = false; return; }
        int acts[6]; int na = build_actions(p, acts);
        if (up || lf) s_dopt--;
        if (dn || rt) s_dopt++;
        if (s_dopt < 0) s_dopt = na - 1;
        if (s_dopt >= na) s_dopt = 0;
        if (a) {
            switch (acts[s_dopt]) {
            case ACT_EQUIP: { int k = s_cur - 6; RogueItem in = s_bag[k];
                RogueItem old = p->equip[in.slot]; bag_remove(k);
                rogue_player_equip(p, &in);
                if (rogue_item_is_equip(&old)) rogue_inventory_add(&old);
                s_detail = false; } break;
            case ACT_UNEQUIP: { EquipSlot sl = PD[s_cur];
                if (!rogue_inventory_full()) {
                    rogue_inventory_add(&p->equip[sl]);
                    p->equip[sl].kind = ITEM_NONE; rogue_player_recompute(p);
                }
                s_detail = false; } break;
            case ACT_USE: { int k = s_cur - 6;
                if (s_bag[k].kind == ITEM_POTION) {
                    p->hp += s_bag[k].amount; if (p->hp > p->max_hp) p->hp = p->max_hp;
                } else if (s_bag[k].kind == ITEM_TORCH) {
                    p->torch_fuel += s_bag[k].amount;
                }
                bag_remove(k); s_detail = false; } break;
            case ACT_SOCKET:  s_gempick = true; s_gemcur = 0; break;
            case ACT_SALVAGE: s_confirm = true; break;
            case ACT_BACK:    s_detail = false; break;
            }
            clamp_cur();
        }
        if (b) s_detail = false;
        return;
    }

    /* --- grid navigation (default): directional, not linear --- */
    if (up) grid_nav(0, -1);
    if (dn) grid_nav(0,  1);
    if (lf) grid_nav(-1, 0);
    if (rt) grid_nav( 1, 0);
    clamp_cur();

    /* A = quick equip / unequip / use (fast path). */
    if (a) {
        if (s_cur < 6) {
            EquipSlot sl = PD[s_cur];
            if (rogue_item_is_equip(&p->equip[sl]) && !rogue_inventory_full()) {
                rogue_inventory_add(&p->equip[sl]);
                p->equip[sl].kind = ITEM_NONE;
                rogue_player_recompute(p);
            }
        } else {
            int k = s_cur - 6;
            if (k < s_bag_n && rogue_item_is_equip(&s_bag[k])) {
                RogueItem in = s_bag[k];
                RogueItem old = p->equip[in.slot];
                bag_remove(k);
                rogue_player_equip(p, &in);
                if (rogue_item_is_equip(&old)) rogue_inventory_add(&old);
            } else if (k < s_bag_n && s_bag[k].kind == ITEM_POTION) {
                p->hp += s_bag[k].amount;
                if (p->hp > p->max_hp) p->hp = p->max_hp;
                bag_remove(k);
            }
        }
        clamp_cur();
    }
    /* B = open the detail page for the selected item (socket / salvage live there). */
    if (b && viewed(p)) { s_detail = true; s_dopt = 0; }
}

/* ---- drawing ---- */
static void fr(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= CRAFT_FB_H) continue;
        for (int i = x; i < x + w; i++)
            if ((unsigned)i < CRAFT_FB_W) fb[j * CRAFT_FB_W + i] = c;
    }
}
static void box(uint16_t *fb, int x, int y, int w, int h, uint16_t border, uint16_t fill) {
    fr(fb, x, y, w, h, border);
    fr(fb, x + 1, y + 1, w - 2, h - 2, fill);
}
static void px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < CRAFT_FB_W && (unsigned)y < CRAFT_FB_H) fb[y * CRAFT_FB_W + x] = c;
}
static void hrun(uint16_t *fb, int x0, int x1, int y, uint16_t c) {
    for (int x = x0; x <= x1; x++) px(fb, x, y, c);
}

/* Tiny per-kind glyph drawn inside a backpack cell. (ox,oy) is the top-left
 * of a ~12x9 icon box; `tint` is the rarity/item colour used to tone equip
 * gear and consumables so rarity still reads at a glance. */
void rogue_item_draw_icon(uint16_t *fb, int ox, int oy, const RogueItem *it, uint16_t tint) {
    const uint16_t SIL = RGB(205,205,215);   /* steel  */
    const uint16_t WD  = RGB(150,110,60);    /* wood   */
    const uint16_t GLD = RGB(235,200,70);    /* gold   */
    const uint16_t DK  = RGB(28,26,34);      /* shadow */
    #define P(i,j,c) px(fb, ox+(i), oy+(j), (c))
    #define H(a,b,j,c) hrun(fb, ox+(a), ox+(b), oy+(j), (c))
    switch (it->kind) {
    case ITEM_WEAPON:
        switch (it->wtype) {
        case WT_DAGGER:                               /* short blade + small guard */
            for (int j = 2; j < 6; j++) P(5,j,SIL);
            H(4,6,6,GLD);
            P(5,7,WD); P(5,8,WD);
            break;
        case WT_SWORD:                                /* medium blade + crossguard */
            for (int j = 0; j < 6; j++) P(5,j,SIL);
            P(6,1,SIL);
            H(3,7,6,WD); P(5,7,WD); P(5,8,WD);
            break;
        case WT_GREATSWORD:                           /* long 2-wide blade, gold guard */
            for (int j = 0; j < 6; j++) { P(5,j,SIL); P(6,j,SIL); }
            H(3,8,6,GLD);
            P(5,7,WD); P(6,7,WD); P(5,8,GLD); P(6,8,GLD);
            break;
        case WT_AXE:                                  /* haft + tapered beard-axe bit */
            for (int j = 0; j < 9; j++) P(4,j,WD);
            H(5,6,1,SIL); H(5,8,2,SIL); H(5,9,3,SIL); H(5,9,4,SIL);
            H(6,8,5,SIL); P(7,6,SIL);                 /* beard tapers to a point */
            break;
        case WT_MACE:                                 /* haft + round spiked ball */
            for (int j = 4; j < 9; j++) P(5,j,WD);
            H(4,6,0,SIL); H(3,7,1,SIL); H(3,7,2,SIL); H(4,6,3,SIL);  /* ball */
            P(2,1,SIL); P(8,1,SIL);                   /* side spikes */
            break;
        case WT_SPEAR:                                /* long shaft + leaf tip */
            for (int j = 3; j < 9; j++) P(5,j,WD);
            P(5,0,SIL); H(4,6,1,SIL); P(5,2,SIL);
            break;
        case WT_WARHAMMER:                            /* haft + big block head */
            for (int j = 3; j < 9; j++) P(5,j,WD);
            H(3,7,0,SIL); H(2,8,1,SIL); H(3,7,2,SIL);
            break;
        case WT_BOW:                                  /* a clean recurve bow (no arrow) */
            P(4,0,WD); P(5,1,WD); P(6,2,WD);          /* upper limb */
            P(6,3,WD); P(6,4,WD); P(6,5,WD);          /* riser belly */
            P(6,6,WD); P(5,7,WD); P(4,8,WD);          /* lower limb */
            for (int j = 1; j < 8; j++) P(4,j,SIL);   /* string chord across the tips */
            break;
        case WT_CROSSBOW:                             /* horizontal limbs + stock + bolt */
            H(1,9,3,WD); P(1,2,WD); P(9,2,WD);        /* bow arms + tips */
            for (int j = 3; j < 8; j++) P(5,j,WD);    /* stock */
            P(5,0,SIL); P(5,1,SIL); P(5,2,SIL);       /* loaded bolt */
            H(3,7,4,SIL);                             /* rail */
            break;
        case WT_WAND:                                 /* short rod + spark */
            for (int i = 0; i < 5; i++) P(3+i, 8-i, WD);
            P(8,3,tint); P(9,2,tint); P(8,2,RGB(255,255,255));
            break;
        case WT_SCEPTER:                              /* ornate gold rod + gem head */
            for (int j = 3; j < 9; j++) P(5,j,GLD);
            P(5,0,tint); H(4,6,1,tint); P(5,2,GLD);
            P(3,1,GLD); P(7,1,GLD);                   /* ornate arms */
            break;
        case WT_STAFF:                                /* diagonal shaft + orb */
        default:
            for (int i = 0; i < 6; i++) P(2+i, 8-i, WD);
            P(8,1,tint); P(9,1,tint); P(8,2,tint); P(9,2,tint);
            P(7,0,RGB(255,255,255));
            break;
        }
        break;
    case ITEM_GEAR:
        if (it->slot == SLOT_OFFHAND) {               /* shield */
            H(3,8,0,SIL); H(2,9,1,tint); H(2,9,2,tint); H(2,9,3,tint);
            H(3,8,4,tint); H(3,8,5,tint); H(4,7,6,tint); H(5,6,7,tint);
            P(5,2,DK); P(6,2,DK); P(5,3,DK); P(6,3,DK);   /* boss */
        } else if (it->slot == SLOT_HELM) {           /* helm */
            H(4,7,0,SIL); H(3,8,1,SIL); H(2,9,2,SIL);
            H(2,9,3,SIL); P(4,3,DK); P(5,3,DK); P(6,3,DK); P(7,3,DK); /* visor */
            H(1,10,4,SIL);                            /* brim */
        } else if (it->slot == SLOT_AMULET) {         /* amulet */
            P(3,0,GLD); P(4,1,GLD); P(5,2,GLD);       /* chain */
            P(8,0,GLD); P(7,1,GLD); P(6,2,GLD);
            P(6,3,tint); P(5,4,tint); P(6,4,tint); P(7,4,tint); P(6,5,tint);
        } else if (it->slot == SLOT_RING) {           /* ring + gem */
            P(5,2,tint); P(6,2,tint);                 /* gem */
            P(4,3,GLD); P(7,3,GLD); P(4,4,GLD); P(7,4,GLD);
            P(5,5,GLD); P(6,5,GLD);                   /* band */
        } else {                                      /* armour / chestplate */
            H(2,3,0,SIL); H(8,9,0,SIL);               /* pauldrons */
            H(1,10,1,SIL); H(2,9,2,tint); H(2,9,3,tint);
            H(3,8,4,tint); H(4,7,5,tint); H(5,6,6,tint);
            P(5,2,DK); P(6,2,DK);                     /* neckline */
        }
        break;
    case ITEM_GEM: {
        uint16_t g = rogue_gem_color((GemType)(it->amount % GEM_COUNT));
        H(5,6,0,g); H(4,7,1,g); H(3,8,2,g); H(4,7,3,g); H(5,6,4,g);
        P(4,1,RGB(255,255,255));                      /* facet glint */
        break; }
    case ITEM_POTION:
        P(5,0,WD); P(6,0,WD);                         /* cork */
        P(5,1,RGB(180,200,210)); P(6,1,RGB(180,200,210));
        H(4,7,2,RGB(180,200,210));
        H(3,8,3,tint); H(3,8,4,tint); H(3,8,5,tint); H(4,7,6,tint);
        P(4,3,RGB(255,255,255));                      /* glass highlight */
        break;
    case ITEM_TORCH:
        for (int j = 4; j <= 8; j++) P(5,j,WD);       /* handle */
        P(5,0,RGB(255,235,120));                      /* flame */
        H(4,6,1,RGB(255,180,40)); H(4,6,2,RGB(255,140,30)); P(5,3,RGB(255,120,20));
        break;
    case ITEM_GOLD:
        H(4,7,1,GLD); H(3,8,2,GLD); H(3,8,3,GLD); H(3,8,4,GLD); H(4,7,5,GLD);
        P(4,2,RGB(255,245,180));                      /* shine */
        break;
    default:
        fr(fb, ox + 2, oy + 1, 8, 7, tint);
        break;
    }
    #undef P
    #undef H
}

static const char *slot_abbrev(EquipSlot s) {
    static const char *A[SLOT_COUNT] = { "Wp","Of","Hl","Ar","Am","Rg" };
    return A[s];
}

static const char *gem_effect(GemType g) {
    static const char *gd[GEM_COUNT] = { "", "+20 Life", "+8% Resist", "+4% Crit", "+10 Armor" };
    return gd[g % GEM_COUNT];
}

/* The gem-picker overlay: choose which bag gem to drop into a free socket. */
static void draw_gem_pick(uint16_t *fb) {
    fr(fb, 0, 0, CRAFT_FB_W, CRAFT_FB_H, RGB(10, 9, 16));
    craft_font_draw(fb, "SOCKET A GEM", 4, 3, RGB(220, 200, 120));
    fr(fb, 0, 12, CRAFT_FB_W, 1, RGB(60, 52, 30));
    int y = 17, shown = 0;
    for (int k = 0; k < s_bag_n; k++) {
        if (s_bag[k].kind != ITEM_GEM) continue;
        bool sel = (shown == s_gemcur);
        if (sel) { fr(fb, 0, y - 1, CRAFT_FB_W, 13, RGB(40, 40, 60));
                   craft_font_draw(fb, ">", 2, y + 2, RGB(255,255,255)); }
        rogue_item_draw_icon(fb, 10, y, &s_bag[k],
                             rogue_gem_color((GemType)(s_bag[k].amount % GEM_COUNT)));
        char nm[40];
        snprintf(nm, sizeof nm, "%s  %s", s_bag[k].name,
                 gem_effect((GemType)(s_bag[k].amount % GEM_COUNT)));
        craft_font_draw(fb, nm, 26, y + 2, sel ? RGB(255,255,255) : RGB(190,190,200));
        y += 14; shown++;
    }
    if (shown == 0)
        craft_font_draw(fb, "No gems in your bag.", 6, 22, RGB(190,150,150));
    craft_font_draw(fb, "A socket   B back", 4, CRAFT_FB_H - 9, RGB(140,140,155));
}

/* The detail page for the viewed item: full stats + sockets + an action list
 * (Equip/Unequip, Socket, Salvage, Back). Salvage asks to confirm. */
static void draw_item_detail(uint16_t *fb, const RoguePlayer *p) {
    fr(fb, 0, 0, CRAFT_FB_W, CRAFT_FB_H, RGB(10, 9, 16));
    bool eq = (s_cur < 6);
    const RogueItem *it = eq ? &p->equip[PD[s_cur]] : &s_bag[s_cur - 6];
    bool gear = rogue_item_is_equip(it);
    uint16_t rc = gear ? rogue_rarity_color(it->rarity) : it->color;
    char line[48];

    /* header: icon box + name + type */
    box(fb, 4, 4, 22, 18, rc, RGB(20,18,28));
    rogue_item_draw_icon(fb, 9, 8, it, rc);
    craft_font_draw(fb, it->name, 30, 5, rc);
    static const char *RAR[RAR_COUNT] = { "Common","Magic","Rare","Legendary" };
    if (gear) snprintf(line, sizeof line, "%s %s", RAR[it->rarity % RAR_COUNT],
                       rogue_slot_name((EquipSlot)it->slot));
    else if (it->kind == ITEM_POTION) snprintf(line, sizeof line, "Potion");
    else if (it->kind == ITEM_GEM)    snprintf(line, sizeof line, "Gem");
    else if (it->kind == ITEM_TORCH)  snprintf(line, sizeof line, "Torch");
    else snprintf(line, sizeof line, "Item");
    craft_font_draw(fb, line, 30, 14, RGB(160,160,175));

    int y = 26;
    if (it->kind == ITEM_WEAPON) {
        snprintf(line, sizeof line, "Damage  %d", it->base_dmg);
        craft_font_draw(fb, line, 6, y, RGB(230,120,80)); y += 9;
    }
    if (it->armor > 0) {
        snprintf(line, sizeof line, "Armor  %d", it->armor);
        craft_font_draw(fb, line, 6, y, RGB(170,170,200)); y += 9;
    }
    for (int a = 0; a < it->n_affix; a++) {
        char ab[24]; rogue_affix_label(ab, sizeof ab, &it->affix[a]);
        craft_font_draw(fb, ab, 6, y, RGB(150,210,150)); y += 8;
    }
    if (gear && it->aspect) {
        snprintf(line, sizeof line, "%s: %s", rogue_aspect_name((AspectId)it->aspect),
                 rogue_aspect_desc((AspectId)it->aspect));
        craft_font_draw(fb, line, 6, y, RGB(220,130,40)); y += 9;
    }
    if (it->kind == ITEM_POTION) { snprintf(line,sizeof line,"Restores %d health", it->amount);
        craft_font_draw(fb, line, 6, y, RGB(150,210,150)); y += 9; }
    else if (it->kind == ITEM_GEM) { snprintf(line,sizeof line,"%s (socket into gear)",
        gem_effect((GemType)(it->amount % GEM_COUNT)));
        craft_font_draw(fb, line, 6, y, RGB(150,210,150)); y += 9; }
    else if (it->kind == ITEM_TORCH) { snprintf(line,sizeof line,"Relights torch +%ds", it->amount);
        craft_font_draw(fb, line, 6, y, RGB(150,210,150)); y += 9; }

    /* sockets row */
    if (gear && it->sockets > 0) {
        craft_font_draw(fb, "Sockets", 6, y, RGB(160,160,175));
        int sxp = 6 + craft_font_width("Sockets") + 5;
        for (int g = 0; g < it->sockets && g < 2; g++) {
            uint16_t gc = it->gem[g] ? rogue_gem_color((GemType)it->gem[g]) : RGB(36,34,44);
            box(fb, sxp + g * 12, y - 1, 9, 9, RGB(90,90,100), gc);
        }
        y += 11;
    }

    /* action list, stacked just above the bottom hint */
    int acts[6]; int na = build_actions(p, acts);
    int ay = CRAFT_FB_H - 11 - na * 10;
    for (int i = 0; i < na; i++) {
        const char *lbl = "Back";
        switch (acts[i]) {
        case ACT_EQUIP:   lbl = "Equip"; break;
        case ACT_UNEQUIP: lbl = "Unequip"; break;
        case ACT_USE:     lbl = (it->kind == ITEM_POTION) ? "Drink" : "Use"; break;
        case ACT_SOCKET:  lbl = "Socket a gem"; break;
        case ACT_SALVAGE: lbl = "Salvage"; break;
        case ACT_BACK:    lbl = "Back"; break;
        }
        bool sel = (i == s_dopt);
        uint16_t lc = (acts[i] == ACT_SALVAGE) ? RGB(230,120,110) : RGB(200,200,210);
        if (sel) { fr(fb, 0, ay - 1, CRAFT_FB_W, 10, RGB(60,52,18));
                   fr(fb, 0, ay - 1, 2, 10, RGB(240,210,60));
                   craft_font_draw(fb, ">", 4, ay, RGB(255,255,255)); }
        craft_font_draw(fb, lbl, 12, ay, sel ? RGB(255,255,255) : lc);
        ay += 10;
    }
    craft_font_draw(fb, "A select   B back", 4, CRAFT_FB_H - 9, RGB(120,120,135));

    /* salvage confirmation overlay */
    if (s_confirm) {
        int bx = 10, by = 46, bwc = CRAFT_FB_W - 20, bhc = 30;
        box(fb, bx, by, bwc, bhc, RGB(220,90,80), RGB(30,16,16));
        snprintf(line, sizeof line, "Salvage for %d gold?", salvage_gold(it));
        craft_font_draw(fb, line, bx + 6, by + 6, RGB(255,210,205));
        craft_font_draw(fb, "A  Yes        B  No", bx + 6, by + 17, RGB(255,255,255));
    }
}

void rogue_inventory_draw(uint16_t *fb, const RoguePlayer *p) {
    if (s_gempick) { draw_gem_pick(fb); return; }
    if (s_detail)  { draw_item_detail(fb, p); return; }
    fr(fb, 0, 0, CRAFT_FB_W, CRAFT_FB_H, RGB(12, 10, 18));
    char buf[40];

    /* Paperdoll 2x3 at the very top (no title bar). Every cell ALWAYS shows
     * its slot label at the top so you know what goes where even before it's
     * filled; the equipped item's icon sits below the label. */
    int px0 = 4, py0 = 2, bw = 18, bh = 16, gap = 2;
    for (int i = 0; i < 6; i++) {
        int col = i % 2, row = i / 2;
        int x = px0 + col * (bw + gap), y = py0 + row * (bh + gap);
        const RogueItem *it = &p->equip[PD[i]];
        bool equipped = rogue_item_is_equip(it);
        bool sel = (s_cur == i);
        uint16_t bdr = sel ? RGB(255,255,255)
                     : equipped ? rogue_rarity_color(it->rarity) : RGB(70,70,80);
        if (sel) box(fb, x-1, y-1, bw+2, bh+2, RGB(240,210,60), RGB(60,52,18));  /* gold cursor frame */
        box(fb, x, y, bw, bh, bdr, sel ? RGB(48,42,24) : RGB(24,22,30));
        craft_font_draw(fb, slot_abbrev(PD[i]), x + 4, y + 1,
                        equipped ? RGB(175,175,190) : RGB(115,115,130));  /* always label the slot */
        if (equipped)
            rogue_item_draw_icon(fb, x + 3, y + 7, it, rogue_rarity_color(it->rarity));
    }

    /* Stat column (gold folded in as the final line). */
    int sx = px0 + 2 * (bw + gap) + 3, sy = 2;
    snprintf(buf, sizeof buf, "HP %d", p->max_hp);        craft_font_draw(fb, buf, sx, sy,    RGB(80,220,90));
    snprintf(buf, sizeof buf, "ARM %d", p->stats.armor);  craft_font_draw(fb, buf, sx, sy+8,  RGB(170,170,200));
    snprintf(buf, sizeof buf, "DMG %d", p->wpn_dmg);      craft_font_draw(fb, buf, sx, sy+16, RGB(230,120,80));
    snprintf(buf, sizeof buf, "CRT %d", p->stats.crit);   craft_font_draw(fb, buf, sx, sy+24, RGB(240,220,80));
    snprintf(buf, sizeof buf, "RES %d", p->stats.resist); craft_font_draw(fb, buf, sx, sy+32, RGB(120,200,220));
    snprintf(buf, sizeof buf, "G %d", p->gold);           craft_font_draw(fb, buf, sx, sy+40, RGB(240,210,60));

    /* Backpack 7xN — wider grid, with a clear gap below the paperdoll. */
    int gx0 = 3, gy0 = 56, cw = 15, ch = 13, gp = 2, cols = 7;
    for (int k = 0; k < ROGUE_BAG_N; k++) {
        int col = k % cols, row = k / cols;
        int x = gx0 + col * (cw + gp), y = gy0 + row * (ch + gp);
        bool has = k < s_bag_n;
        bool sel = (s_cur == 6 + k);
        uint16_t bdr = sel ? RGB(255,255,255)
                     : has ? (rogue_item_is_equip(&s_bag[k]) ? rogue_rarity_color(s_bag[k].rarity) : s_bag[k].color)
                           : RGB(50,48,58);
        if (sel) box(fb, x-1, y-1, cw+2, ch+2, RGB(240,210,60), RGB(60,52,18));  /* gold cursor frame */
        box(fb, x, y, cw, ch, bdr, sel ? RGB(48,42,24) : RGB(20,18,26));
        if (has) {
            uint16_t tint = rogue_item_is_equip(&s_bag[k]) ? rogue_rarity_color(s_bag[k].rarity) : s_bag[k].color;
            rogue_item_draw_icon(fb, x + 1, y + 2, &s_bag[k], tint);
        }
    }

    /* detail / compare for the selected cell */
    const RogueItem *sel = NULL; EquipSlot ssl = SLOT_WEAPON;
    if (s_cur < 6) { sel = &p->equip[PD[s_cur]]; ssl = PD[s_cur]; }
    else if (s_cur - 6 < s_bag_n) { sel = &s_bag[s_cur - 6]; ssl = (EquipSlot)sel->slot; }

    /* Three-line detail panel: name(+aspect), what-it-does (stats+affixes),
     * then the action/compare line. */
    int dy = CRAFT_FB_H - 22;
    fr(fb, 0, dy - 1, CRAFT_FB_W, 23, RGB(8, 7, 12));
    if (sel && sel->kind != ITEM_NONE) {
        uint16_t nc = rogue_item_is_equip(sel) ? rogue_rarity_color(sel->rarity) : sel->color;
        craft_font_draw(fb, sel->name, 3, dy, nc);
        if (rogue_item_is_equip(sel) && sel->aspect) {
            const char *ad = rogue_aspect_desc((AspectId)sel->aspect);
            craft_font_draw(fb, ad, CRAFT_FB_W - craft_font_width(ad) - 3, dy, RGB(220,130,40));
        }

        /* line 2 — what the item does */
        char info[48]; int n = 0; info[0] = 0;
        if (rogue_item_is_equip(sel)) {
            if (sel->kind == ITEM_WEAPON)
                n += snprintf(info + n, sizeof info - n, "DMG%d ", sel->base_dmg);
            if (sel->armor > 0)
                n += snprintf(info + n, sizeof info - n, "ARM%d ", sel->armor);
            for (int a = 0; a < sel->n_affix && n < (int)sizeof info - 12; a++) {
                char ab[20]; rogue_affix_label(ab, sizeof ab, &sel->affix[a]);
                n += snprintf(info + n, sizeof info - n, "%s ", ab);
            }
            if (sel->sockets)
                n += snprintf(info + n, sizeof info - n, "[%d sock]", sel->sockets);
        } else if (sel->kind == ITEM_POTION) {
            snprintf(info, sizeof info, "Restores %d health", sel->amount);
        } else if (sel->kind == ITEM_GEM) {
            static const char *gd[GEM_COUNT] = { "", "+20 Life", "+8% Resist", "+4% Crit", "+10 Armor" };
            snprintf(info, sizeof info, "Gem: %s (socket into gear)", gd[sel->amount % GEM_COUNT]);
        } else if (sel->kind == ITEM_TORCH) {
            snprintf(info, sizeof info, "Relights torch +%ds", sel->amount);
        }
        craft_font_draw(fb, info, 3, dy + 8, RGB(150, 210, 150));

        /* line 3 — action + compare */
        if (rogue_item_is_equip(sel)) {
            const RogueItem *eq = &p->equip[ssl];
            int sd = (sel->kind==ITEM_WEAPON? sel->base_dmg:0) + sel->armor;
            int ed = (eq->kind==ITEM_WEAPON? eq->base_dmg:0) + eq->armor;
            if (s_cur >= 6 && rogue_item_is_equip(eq)) {
                int d = sd - ed;
                snprintf(buf, sizeof buf, "A equip (%s%d %s)  B details",
                         d>=0?"+":"", d, rogue_slot_name(ssl));
            } else if (s_cur >= 6) {
                snprintf(buf, sizeof buf, "A equip %s  B details", rogue_slot_name(ssl));
            } else {
                snprintf(buf, sizeof buf, "A unequip   B details");
            }
            craft_font_draw(fb, buf, 3, dy + 16, RGB(200,200,210));
        } else if (sel->kind == ITEM_POTION) {
            craft_font_draw(fb, "A drink   B details", 3, dy + 16, RGB(200,200,210));
        } else if (sel->kind == ITEM_GEM) {
            craft_font_draw(fb, "B details (socket / salvage)", 3, dy + 16, RGB(200,200,210));
        }
    } else {
        craft_font_draw(fb, "MENU close   dpad move", 3, dy + 8, RGB(150,150,160));
    }
}
