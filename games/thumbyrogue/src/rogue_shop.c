#include "rogue_shop.h"
#include "rogue_inventory.h"
#include "rogue_gen.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_font.h"
#include "rogue_level.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define N_STOCK 4

static bool      s_has_pad, s_open;
static Vec3      s_pad;       /* centre of the counter's customer side */
static float     s_fdx, s_fdz; /* counter front direction (toward customer) */
static float     s_px, s_pz;   /* counter axis (perpendicular) */
static RogueItem s_stock[N_STOCK];
static int       s_price[N_STOCK];
static bool      s_sold[N_STOCK];
static bool      s_upgraded;  /* weapon upgrade is once per shop */
static int       s_depth;
static uint32_t  s_rng;
static int       s_cur;       /* 0..N_STOCK-1 items, then gamble/reroll/upgrade */

#define OPT_GAMBLE  (N_STOCK + 0)
#define OPT_REROLL  (N_STOCK + 1)
#define OPT_UPGRADE (N_STOCK + 2)
#define N_OPT       (N_STOCK + 3)

static uint32_t xs(void){ s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng; }

static int price_of(const RogueItem *it, int depth) {
    int base[RAR_COUNT] = { 15, 35, 75, 150 };
    return base[it->rarity] + depth * (8 + it->rarity * 6);
}

/* The generator builds the physical stall (counter / shopkeeper alcove /
 * shelf wall); this just rolls the stock and remembers where the counter's
 * customer side is so walking up to it opens the trade screen. */
void rogue_shop_place(const RogueLevelInfo *lv, int depth, uint32_t seed) {
    s_has_pad = false; s_open = false; s_cur = 0; s_depth = depth;
    s_upgraded = false;
    s_rng = seed ^ 0x5409u ^ (uint32_t)(depth * 40503u);
    if (!s_rng) s_rng = 1;
    if (lv->has_shop) {
        s_fdx = (float)lv->shop_dx; s_fdz = (float)lv->shop_dz;
        s_px  = (float)lv->shop_dz; s_pz  = (float)lv->shop_dx;
        s_pad = v3(lv->shop_x + 0.5f + s_fdx, (float)lv->floor_y,
                   lv->shop_z + 0.5f + s_fdz);
        s_has_pad = true;
    }
    for (int i = 0; i < N_STOCK; i++) {
        rogue_item_roll_drop(&s_stock[i], depth + 1, xs());
        s_price[i] = price_of(&s_stock[i], depth);
        s_sold[i] = false;
    }
}

/* True while the hero stands at the counter's customer side — anywhere
 * along the 3-cell front, hugging the counter. */
bool rogue_shop_pad_near(float x, float y, float z) {
    if (!s_has_pad) return false;
    float dx = x - s_pad.x, dz = z - s_pad.z;
    float along = dx * s_px + dz * s_pz;      /* down the counter's length */
    float out   = dx * s_fdx + dz * s_fdz;    /* away from the counter face */
    return fabsf(along) < 1.5f && out > -0.5f && out < 0.65f &&
           fabsf(y - s_pad.y) < 1.2f;
}
/* Pop one unsold stock item (marks it sold). Used when the shopkeeper is
 * slain: his wares spill onto the floor as ordinary pickups. */
int rogue_shop_take_stock(RogueItem *out) {
    for (int i = 0; i < N_STOCK; i++) {
        if (s_sold[i]) continue;
        *out = s_stock[i];
        s_sold[i] = true;
        return 1;
    }
    return 0;
}

bool rogue_shop_is_open(void){ return s_open; }
void rogue_shop_open(void){ s_open = true; s_cur = 0; }
void rogue_shop_close(void){ s_open = false; }

static bool edge(bool n, bool p){ return n && !p; }

void rogue_shop_input(RoguePlayer *p, const CraftRawButtons *btn,
                      const CraftRawButtons *prev) {
    if (edge(btn->left,prev->left)||edge(btn->up,prev->up))     s_cur--;
    if (edge(btn->right,prev->right)||edge(btn->down,prev->down)) s_cur++;
    if (s_cur < 0) s_cur = N_OPT - 1;
    if (s_cur >= N_OPT) s_cur = 0;

    if (!edge(btn->a, prev->a)) return;
    if (s_cur < N_STOCK) {
        if (!s_sold[s_cur] && p->gold >= s_price[s_cur] && !rogue_inventory_full()) {
            if (rogue_inventory_add(&s_stock[s_cur])) {
                p->gold -= s_price[s_cur]; s_sold[s_cur] = true;
            }
        }
    } else if (s_cur == OPT_GAMBLE) {
        int cost = 30 + s_depth * 8;
        if (p->gold >= cost && !rogue_inventory_full()) {
            p->gold -= cost;
            RogueItem it; rogue_item_roll_drop(&it, s_depth + 1, xs());
            rogue_inventory_add(&it);
        }
    } else if (s_cur == OPT_REROLL) {
        int cost = 25 + s_depth * 5;
        if (p->gold >= cost) {
            p->gold -= cost;
            /* reroll the equipped weapon's affixes at the same rarity */
            RogueItem nw; rogue_item_roll_weapon(&nw, s_depth + 1, xs());
            nw.rarity = p->equip[SLOT_WEAPON].rarity;  /* keep tier feel */
            p->equip[SLOT_WEAPON] = nw;
            rogue_player_recompute(p);
        }
    } else if (s_cur == OPT_UPGRADE) {
        /* Once per shop, and a modest bump (+5% +1). The old unlimited
         * +12.5%+2 compounded into an exponential damage pump. */
        int cost = 50 + s_depth * 10;
        if (!s_upgraded && p->gold >= cost) {
            p->gold -= cost;
            p->equip[SLOT_WEAPON].base_dmg += p->equip[SLOT_WEAPON].base_dmg / 20 + 1;
            rogue_player_recompute(p);
            s_upgraded = true;
        }
    }
}

static void fr(uint16_t *fb,int x,int y,int w,int h,uint16_t c){
    for(int j=y;j<y+h;j++){ if((unsigned)j>=CRAFT_FB_H)continue;
        for(int i=x;i<x+w;i++) if((unsigned)i<CRAFT_FB_W) fb[j*CRAFT_FB_W+i]=c; }
}

/* Draw one selectable row with a clear highlight bar + cursor. If `icon` is
 * non-NULL its item glyph is drawn before the label (merchant stock rows). */
static void shop_row(uint16_t *fb, int y, bool sel, bool dim,
                     const char *label, uint16_t lc, int price,
                     const RogueItem *icon) {
    if (sel) {
        fr(fb, 0, y - 1, CRAFT_FB_W, 9, RGB(60, 52, 18));   /* highlight bar */
        fr(fb, 0, y - 1, 2, 9, RGB(240, 210, 60));          /* gold edge */
        craft_font_draw(fb, ">", 4, y, RGB(255, 255, 255));
    }
    int lx = 11;
    if (icon) {
        rogue_item_draw_icon(fb, 11, y - 1, icon, dim ? RGB(110,105,95) : lc);
        lx = 25;                                            /* leave room for the glyph */
    }
    uint16_t c = dim ? RGB(95, 90, 80) : (sel ? RGB(255, 255, 255) : lc);
    craft_font_draw(fb, label, lx, y, c);
    if (price >= 0) {
        char pb[12]; snprintf(pb, sizeof pb, "%d", price);
        uint16_t pc = dim ? RGB(120,70,60) : RGB(240, 210, 60);
        craft_font_draw(fb, pb, CRAFT_FB_W - craft_font_width(pb) - 4, y, pc);
    }
}

void rogue_shop_draw(uint16_t *fb, const RoguePlayer *p) {
    fr(fb,0,0,CRAFT_FB_W,CRAFT_FB_H,RGB(14,12,8));
    char buf[40];
    craft_font_draw(fb,"MERCHANT",4,2,RGB(240,220,120));
    snprintf(buf,sizeof buf,"G %d",p->gold);
    craft_font_draw(fb,buf,CRAFT_FB_W-craft_font_width(buf)-4,2,RGB(240,210,60));
    fr(fb,0,11,CRAFT_FB_W,1,RGB(60,52,30));

    int y = 16;
    for (int i = 0; i < N_STOCK; i++) {
        bool sel = (s_cur == i);
        if (s_sold[i]) {
            shop_row(fb, y, sel, true, "- sold -", RGB(90,90,90), -1, NULL);
        } else {
            bool dim = p->gold < s_price[i];
            uint16_t lc = rogue_item_is_equip(&s_stock[i])
                        ? rogue_rarity_color(s_stock[i].rarity) : s_stock[i].color;
            shop_row(fb, y, sel, dim, s_stock[i].name, lc, s_price[i], &s_stock[i]);
        }
        y += 10;
    }
    y += 4;
    fr(fb,0,y-3,CRAFT_FB_W,1,RGB(60,52,30));
    struct { int id; const char *t; int cost; } opt[3] = {
        { OPT_GAMBLE,  "Gamble random", 30 + s_depth*8 },
        { OPT_REROLL,  "Reroll weapon", 25 + s_depth*5 },
        { OPT_UPGRADE, "Upgrade weapon",50 + s_depth*10 },
    };
    for (int i = 0; i < 3; i++) {
        bool sel = (s_cur == opt[i].id);
        if (opt[i].id == OPT_UPGRADE && s_upgraded) {
            shop_row(fb, y, sel, true, "Upgraded", RGB(90,90,90), -1, NULL);
        } else {
            bool dim = p->gold < opt[i].cost;
            shop_row(fb, y, sel, dim, opt[i].t, RGB(200,200,210), opt[i].cost, NULL);
        }
        y += 10;
    }

    /* Detail panel for the selected item — stats + affixes as you scroll. */
    int dy = CRAFT_FB_H - 28;
    fr(fb, 0, dy - 1, CRAFT_FB_W, 19, RGB(8, 7, 5));
    if (s_cur < N_STOCK && !s_sold[s_cur]) {
        const RogueItem *it = &s_stock[s_cur];
        if (it->kind == ITEM_WEAPON)
            snprintf(buf, sizeof buf, "DMG %d  %s", it->base_dmg, rogue_slot_name((EquipSlot)it->slot));
        else
            snprintf(buf, sizeof buf, "ARM %d  %s", it->armor, rogue_slot_name((EquipSlot)it->slot));
        craft_font_draw(fb, buf, 4, dy, rogue_rarity_color(it->rarity));
        char line[44]; line[0] = 0; int col = 4;
        for (int a = 0; a < it->n_affix; a++) {
            char ab[24]; rogue_affix_label(ab, sizeof ab, &it->affix[a]);
            craft_font_draw(fb, ab, col, dy + 8, RGB(150,200,150));
            col += craft_font_width(ab) + 6;
        }
        if (it->aspect) {
            const char *ad = rogue_aspect_desc((AspectId)it->aspect);
            craft_font_draw(fb, ad, CRAFT_FB_W - craft_font_width(ad) - 4, dy, RGB(220,130,40));
        }
    } else if (s_cur == OPT_GAMBLE) {
        craft_font_draw(fb, "buy a random item", 4, dy, RGB(180,180,190));
    } else if (s_cur == OPT_REROLL) {
        craft_font_draw(fb, "re-roll equipped weapon affixes", 4, dy, RGB(180,180,190));
    } else if (s_cur == OPT_UPGRADE) {
        craft_font_draw(fb, s_upgraded ? "already upgraded at this shop"
                                       : "raise weapon damage (once per shop)",
                        4, dy, RGB(180,180,190));
    }
    craft_font_draw(fb,"A buy/use    MENU leave",4,CRAFT_FB_H-9,RGB(150,150,160));
}
