/*
 * ThumbAtro — a Balatro-style poker roguelike, ported to the Mote engine from
 * the MicroPython original (TinyCircuits-Thumby-Color-Games/ThumbAtro).
 *
 * Pure 2D: a baked 13x6 card spritesheet (cards_img — rows 0..3 are the four
 * suits x thirteen ranks, rows 4..5 hold jokers / tarot / overlays / the hand
 * pointer) drawn cell-by-cell with the engine's colour-keyed blit. Everything
 * is painted immediate-mode in overlay(); no engine 3D/2D pools are used.
 */
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "mote_api.h"
#include "mote_build.h"
#include "mote_tween.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif
#include "icon.h"
#include "cards.h"        /* cards_img  (234x156, 13x6 grid) */
#include "background.h"   /* background_img (128x128) */
#include "title.h"        /* title_img (128x128) */
#include "munro.font.h"   /* `munro` — the original Munro Narrow pixel font, baked
                           * from munro-narrow_10.bmp via glyphs2font (mote->text_font) */
#include "select.h"       /* select_snd */
#include "play.h"         /* play_snd   */
#include "money.h"        /* money_snd  */
#include "shuffle.h"      /* shuffle_snd*/

#define CW 18          /* card cell width  (234/13) */
#define CH 26          /* card cell height (156/6)  */
#define KEY 0xF81F     /* magenta colour key the baker assigned */

/* ---- RNG (xorshift32, seeded in init) ------------------------------------ */
static uint32_t s_rng = 0xC0FFEEu;
static uint32_t rnd(void){ uint32_t x=s_rng; x^=x<<13; x^=x>>17; x^=x<<5; return s_rng=x; }
static int randint(int a,int b){ return a + (int)(rnd()%(uint32_t)(b-a+1)); }   /* inclusive */
static float rndf(void){ return (float)(rnd()>>8) / 16777216.0f; }
static int wchoice(const float *w,int n){ float t=0; for(int i=0;i<n;i++)t+=w[i]; float r=rndf()*t,u=0; for(int i=0;i<n;i++){ if(u+w[i]>=r) return i; u+=w[i]; } return n-1; }

/* ---- model --------------------------------------------------------------- */
typedef enum { M_NONE, M_BASE, M_MULT, M_RARE, M_WILD, M_STEEL, M_COIN,
               M_FACE, M_ACE, M_FIB, M_NONFACE, M_EVEN, M_ODD } ModType;
typedef struct { ModType type; int base, mult; int once; } Mod;

typedef enum { CK_NORMAL, CK_JOKER, CK_TAROT_RANK, CK_TAROT_SUIT, CK_TAROT_UPRANK,
               CK_UPGRADE } CardKind;

typedef struct {
    CardKind kind;
    int rank_x;        /* 0..12 sheet column; rank_value = (rank_x==0)?14:rank_x+1 */
    int suit_y;        /* 0..3 */
    int fx, fy;        /* sprite cell drawn (specials live on rows 4..5) */
    Mod mods[2]; int nmods;
    int param_rank, param_suit, level_bonus, hand_type;  /* tarot / upgrade */
    char rarity;       /* 'c' 'u' 'r' */
    /* runtime */
    int selected;
    float x, y, tx, ty;   /* centre position + tween target */
    int alive;            /* drawn/used */
} Card;

#define MAXCOL 90
static const char *HAND_NAMES[13] = {
    "Flush five","Flush house","5 of a Kind","Royal Flush","Straight Flush",
    "4 of a Kind","Full House","Flush","Straight","3 of a Kind","Two Pair",
    "One Pair","High Card" };

typedef enum { ST_INTRO, ST_PLAY, ST_JOKER, ST_BOOSTER_WAIT, ST_BOOSTER, ST_OVER } State;

/* score popup */
typedef struct { char s[12]; float x,y,tx,ty,t,life,scale; uint16_t col; int main; int alive; } Pop;
#define MAXPOP 24

static struct {
    State state;
    Card  col[MAXCOL]; int ncol;     /* persistent player collection (deck source) */
    int   deck[MAXCOL]; int ndeck, draw_ptr;  /* shuffled indices, draw from top */
    Card  hand[16]; int nhand;
    Card  jokers[2]; int njok;
    Card  booster[8]; int nboost;
    Card  played[8]; int nplayed;
    int   hand_lvl[13];
    int   score, target, hands_played, discard_limit, round;
    int   best_hand;
    int   cursor;                    /* index in hand or jokers or booster */
    int   extra_booster;
    float intro_t, over_t, played_t; /* timers (seconds) */
    int   booster_wait;
    float vol;
    char  msg[28];                   /* hand_type_text line */
    int   disp_base, disp_mult;      /* base/mult shown in the score boxes */
    Pop   pops[MAXPOP];
    float money_t; int money_left; float money_iv;  /* cascading coin sfx */
} g;

/* ---- helpers ------------------------------------------------------------- */
static int rankval(const Card *c){ return c->rank_x==0 ? 14 : c->rank_x+1; }
static void say(const char *s){ size_t i=0; for(;s[i]&&i<sizeof g.msg-1;i++) g.msg[i]=s[i]; g.msg[i]=0; }
static int has_mod(const Card *c, ModType t){ for(int i=0;i<c->nmods;i++) if(c->mods[i].type==t) return 1; return 0; }

static Card mk_normal(int rank_x,int suit_y){
    Card c; memset(&c,0,sizeof c); c.kind=CK_NORMAL; c.rank_x=rank_x; c.suit_y=suit_y;
    c.fx=rank_x; c.fy=suit_y; c.rarity='c'; c.x=150; c.y=80; c.tx=150; c.ty=80; c.alive=1; return c;
}

static int cond(ModType t,int rv){
    switch(t){
        case M_FACE:    return rv>10 && rv<14;
        case M_ACE:     return rv==14;
        case M_FIB:     return rv==2||rv==3||rv==5||rv==8||rv==13;
        case M_NONFACE: return rv<=10;
        case M_EVEN:    return rv%2==0;
        case M_ODD:     return rv%2!=0;
        default:        return 1;
    }
}

/* one modifier's contribution, mirroring ScoreBonusModifier.apply_bonus */
static void mod_bonus(const Card *card,const Mod *m,const Card *const*sel,int nsel,
                      int card_in_sel,int card_in_hand,int *ob,int *om){
    *ob=0; *om=0;
    int is_joker = card->kind==CK_JOKER;
    if(m->type==M_WILD || m->type==M_COIN || m->type==M_NONE) return;
    if(m->type==M_STEEL){ if(card_in_hand){ *ob=m->base; *om=m->mult; } return; }
    /* ScoreBonus family */
    if(!is_joker && !card_in_sel) return;
    if(m->once || !is_joker){ *ob=m->base; *om=m->mult; return; }
    /* joker, per-card */
    int tb=0,tm=0;
    for(int i=0;i<nsel;i++) if(cond(m->type, rankval(sel[i]))){ tb+=m->base; tm+=m->mult; }
    *ob=tb; *om=tm;
}
static void card_bonus(const Card *card,const Card *const*sel,int nsel,
                       int card_in_sel,int card_in_hand,int *ob,int *om){
    int tb=0,tm=0;
    for(int i=0;i<card->nmods;i++){ int b,mu; mod_bonus(card,&card->mods[i],sel,nsel,card_in_sel,card_in_hand,&b,&mu); tb+=b; tm+=mu; }
    *ob=tb; *om=tm;
}

/* ---- hand evaluation ----------------------------------------------------- */
static int is_straight(const int *rv,int n){
    if(n!=5) return 0;
    int s[5]; memcpy(s,rv,sizeof s);
    for(int i=0;i<5;i++) for(int j=i+1;j<5;j++) if(s[j]<s[i]){int t=s[i];s[i]=s[j];s[j]=t;}
    for(int i=1;i<5;i++) if(s[i]==s[i-1]) return 0;     /* need distinct */
    if(s[4]-s[0]==4) return 1;
    if(s[0]==2&&s[1]==3&&s[2]==4&&s[3]==5&&s[4]==14) return 1;  /* wheel */
    return 0;
}
static int is_flush(const Card *const*sel,int nsel){
    int wild=0, suit=-1, nonwild=0;
    for(int i=0;i<nsel;i++){
        if(has_mod(sel[i],M_WILD)) wild++;
        else { if(suit<0) suit=sel[i]->suit_y; else if(sel[i]->suit_y!=suit) return 0; nonwild++; }
    }
    return (nonwild+wild)==5;
}
/* returns hand index 0..12, fills base/mult (after level upgrades) */
static int score_hand(const Card *const*sel,int nsel,int *obase,int *omult){
    int rv[5]; for(int i=0;i<nsel&&i<5;i++) rv[i]=rankval(sel[i]);
    int cnt[15]={0}; for(int i=0;i<nsel;i++) cnt[rankval(sel[i])]++;
    int n2=0,n3=0,n4=0,n5=0,pairs=0;
    for(int r=2;r<=14;r++){ if(cnt[r]==2){n2=1;pairs++;} if(cnt[r]==3)n3=1; if(cnt[r]==4)n4=1; if(cnt[r]>=5)n5=1; }
    int flush=is_flush(sel,nsel), straight=is_straight(rv,nsel);
    int hi_for_royal=0; for(int i=0;i<nsel;i++){ int v=rankval(sel[i]); if(v==12||v==14) hi_for_royal=1; }
    int idx=12, base=5, mult=1;
    struct { int i,b,m,ok; } C[] = {
        {0,160,16, flush&&n5},
        {1,140,14, flush&&n3&&n2},
        {2,120,12, n5},
        {3,100,10, flush&&straight&&hi_for_royal},
        {4,100,8,  flush&&straight},
        {5,60,7,   n4},
        {6,40,4,   n3&&n2},
        {7,35,4,   flush},
        {8,30,4,   straight},
        {9,30,3,   n3},
        {10,20,2,  pairs==2},
        {11,10,2,  n2},
    };
    for(unsigned k=0;k<sizeof C/sizeof C[0];k++) if(C[k].ok){ idx=C[k].i; base=C[k].b; mult=C[k].m; break; }
    int lvl=g.hand_lvl[idx];
    if(lvl){ base+=lvl*10; mult+=lvl; }
    *obase=base; *omult=mult; return idx;
}

/* ---- popups -------------------------------------------------------------- */
static void pop_add(const char *s,float x,float y,float tx,float ty,uint16_t col,int main,float scale){
    for(int i=0;i<MAXPOP;i++) if(!g.pops[i].alive){
        Pop *p=&g.pops[i]; memset(p,0,sizeof *p); p->alive=1; p->main=main; p->scale=scale;
        p->x=x;p->y=y;p->tx=tx;p->ty=ty;p->col=col;p->life=main?1.6f:1.2f;
        size_t k=0; for(;s[k]&&k<sizeof p->s-1;k++)p->s[k]=s[k]; p->s[k]=0; return;
    }
}

/* ---- deck / hand --------------------------------------------------------- */
static void deck_build(void){
    g.ndeck=g.ncol; for(int i=0;i<g.ncol;i++) g.deck[i]=i;
    for(int i=g.ndeck-1;i>0;i--){ int j=randint(0,i); int t=g.deck[i];g.deck[i]=g.deck[j];g.deck[j]=t; }
    g.draw_ptr=0;
}
static int deck_remaining(void){ return g.ndeck - g.draw_ptr; }
/* draw n cards into hand (as copies of collection templates); 0 on empty */
static int draw_into_hand(int n){
    for(int k=0;k<n;k++){
        if(g.draw_ptr>=g.ndeck) return 0;
        Card c = g.col[g.deck[g.draw_ptr++]];
        c.x=150; c.y=80; c.tx=150; c.ty=80; c.selected=0; c.alive=1;
        g.hand[g.nhand++]=c;
    }
    return 1;
}
static void layout_hand(void){
    for(int i=0;i<g.nhand;i++){ g.hand[i].tx=7+i*16; g.hand[i].ty=103; g.hand[i].selected=0; }
}
static void layout_jokers(void){
    for(int i=0;i<g.njok;i++){ g.jokers[i].tx=7+i*19; g.jokers[i].ty=23; }
}
static int nsel_hand(void){ int n=0; for(int i=0;i<g.nhand;i++) if(g.hand[i].selected)n++; return n; }

static void end_game(void);

static void draw_hand(void){
    g.nhand=0;
    if(!draw_into_hand(8)){ end_game(); return; }
    layout_hand();
    g.cursor=0;
}

/* ---- audio --------------------------------------------------------------- */
static void snd(const MoteSound *s){ if(g.vol>0) mote->audio_play(s, g.vol*4.0f); }

/* ---- scoring / play ------------------------------------------------------ */
static void update_score_display(void){
    int n=nsel_hand();
    if(!n){ say("Play/Discard 5 cards"); return; }
    const Card *sel[5]; int ns=0;
    for(int i=0;i<g.nhand&&ns<5;i++) if(g.hand[i].selected) sel[ns++]=&g.hand[i];
    int b,m; int idx=score_hand(sel,ns,&b,&m);
    say(HAND_NAMES[idx]); g.disp_base=b; g.disp_mult=m;
}

static void evaluate_hand(void){
    const Card *sel[5]; int ns=0;
    for(int i=0;i<g.nhand&&ns<5;i++) if(g.hand[i].selected) sel[ns++]=&g.hand[i];
    if(!ns) return;
    int base,mult; int idx=score_hand(sel,ns,&base,&mult);
    for(int i=0;i<ns;i++) if(has_mod(sel[i],M_COIN)) g.extra_booster++;

    char buf[12];
    /* selected cards: rank value + their own modifiers */
    for(int i=0;i<g.nhand;i++){
        Card *c=&g.hand[i]; if(!c->selected) continue;
        int b,m; card_bonus(c,sel,ns,1,1,&b,&m); b += rankval(c);
        if(b>0){ base+=b; snprintf(buf,sizeof buf,"+%d",b); pop_add(buf,c->x,c->y-10,51,24,0x04FF,0,1); }
        if(m>0){ mult+=m; snprintf(buf,sizeof buf,"x%d",m); pop_add(buf,c->x,c->y+10,85,24,0xFAEA,0,1); }
    }
    /* non-selected cards left in hand (steel etc.) */
    for(int i=0;i<g.nhand;i++){
        Card *c=&g.hand[i]; if(c->selected) continue;
        int b,m; card_bonus(c,sel,ns,0,1,&b,&m);
        if(b>0){ base+=b; snprintf(buf,sizeof buf,"+%d",b); pop_add(buf,c->x,c->y-10,51,24,0x04FF,0,1); }
        if(m>0){ mult+=m; snprintf(buf,sizeof buf,"x%d",m); pop_add(buf,c->x,c->y+10,85,24,0xFAEA,0,1); }
    }
    /* jokers */
    for(int i=0;i<g.njok;i++){
        Card *j=&g.jokers[i]; int b,m; card_bonus(j,sel,ns,0,0,&b,&m);
        if(b>0){ base+=b; snprintf(buf,sizeof buf,"+%d",b); pop_add(buf,j->x,j->y-8,51,24,0x04FF,0,1); }
        if(m>0){ mult+=m; snprintf(buf,sizeof buf,"x%d",m); pop_add(buf,j->x,j->y+8,85,24,0xFAEA,0,1); }
    }
    int final = base*mult;
    if(final>g.best_hand) g.best_hand=final;
    snprintf(buf,sizeof buf,"%d",final);
    /* main score popup grows with magnitude */
    float sc = 1.0f; { int v=final; while(v>=10){ sc+=1.0f; v/=10; } }
    pop_add(buf,64,82,64,82,0xFFFF,1,sc);
    g.disp_base=base; g.disp_mult=mult; say(HAND_NAMES[idx]);
    g.score+=final;
    /* cascade coin sfx, longer for bigger scores */
    g.money_left = (int)(sc*3.0f)+2; g.money_iv=0.0f; g.money_t=0;
    snd(&money_snd);
}

static void play_hand(void){
    int ns=nsel_hand(); if(!ns) return;
    evaluate_hand();
    g.hands_played++;
    /* move selected -> played row (slide-in), drop from hand */
    g.nplayed=0; int px=0;
    Card keep[16]; int nk=0;
    for(int i=0;i<g.nhand;i++){
        if(g.hand[i].selected){
            Card c=g.hand[i]; c.tx=10+px*18+(px?1:0); c.ty=65; c.selected=0;
            g.played[g.nplayed++]=c; px++;
        } else keep[nk++]=g.hand[i];
    }
    memcpy(g.hand,keep,nk*sizeof(Card)); g.nhand=nk;
    g.played_t=0;

    if(g.hands_played>=4 || g.score>=g.target){
        /* clear the rest of the hand off-screen */
        for(int i=0;i<g.nhand;i++){ g.hand[i].tx=-100; }
        if(g.score>=g.target){ g.nhand=0; g.booster_wait=1; g.state=ST_BOOSTER_WAIT; }
        else end_game();
    } else {
        if(!draw_into_hand(ns)){ end_game(); return; }
        layout_hand();
    }
    snd(&play_snd);
}

static void discard_and_draw(void){
    int ns=nsel_hand(); if(!ns||g.discard_limit<=0) return;
    Card keep[16]; int nk=0;
    for(int i=0;i<g.nhand;i++) if(!g.hand[i].selected) keep[nk++]=g.hand[i];
    memcpy(g.hand,keep,nk*sizeof(Card)); g.nhand=nk;
    if(!draw_into_hand(ns)){ end_game(); return; }
    layout_hand(); g.discard_limit--; snd(&play_snd);
}

static void end_game(void){
    g.state=ST_OVER; g.over_t=0;
    snprintf(g.msg,sizeof g.msg,"Game Over!");
}

/* ---- booster packs ------------------------------------------------------- */
static Card mk_joker(char rarity){
    Card c; memset(&c,0,sizeof c); c.kind=CK_JOKER; c.rarity=rarity;
    c.x=150;c.y=80;c.tx=150;c.ty=80;c.alive=1; c.fx=0; c.fy=4;
    /* one modifier, weighted by rarity (mirrors generate_joker_card) */
    static const ModType cond_mods[6]={M_EVEN,M_ODD,M_FIB,M_ACE,M_NONFACE,M_FACE};
    Mod m; memset(&m,0,sizeof m);
    if(rarity=='c'){
        const float w[6]={0.3f,0.3f,0.3f,0.2f,0.2f,0.2f}; int k=wchoice(w,6);
        m.type=cond_mods[k]; m.base=randint(10,20); m.mult=0;
    } else if(rarity=='u'){
        const float w[9]={.15f,.15f,.15f,.15f,.15f,.15f,.15f,.25f,.15f}; int k=wchoice(w,9);
        if(k<6){ m.type=cond_mods[k]; m.base=0; m.mult=randint(1,3); }
        else if(k==6){ m.type=M_BASE; m.base=randint(10,20); m.once=1; }
        else if(k==7){ m.type=M_MULT; m.mult=randint(1,4); m.once=1; }
        else { m.type=M_RARE; m.base=randint(10,20); m.mult=randint(1,3); m.once=1; }
    } else {
        const float w[7]={.15f,.15f,.15f,.15f,.15f,.15f,.15f}; int k=wchoice(w,7);
        if(k<6){ m.type=cond_mods[k]; m.base=randint(5,10); m.mult=randint(1,3); }
        else { m.type=M_RARE; m.base=randint(20,50); m.mult=randint(3,5); m.once=1; }
    }
    c.mods[c.nmods++]=m;
    /* sheet frame for the modifier kind */
    int fx=0,fy=4;
    switch(m.type){ case M_EVEN:fx=2;fy=5;break; case M_ODD:fx=1;fy=5;break;
        case M_FIB:fx=5;fy=5;break; case M_ACE:fx=0;fy=5;break; case M_NONFACE:fx=3;fy=5;break;
        case M_FACE:fx=4;fy=5;break; case M_BASE:fx=0;fy=4;break; case M_MULT:fx=1;fy=4;break;
        case M_RARE:fx=2;fy=4;break; default:break; }
    c.fx=fx; c.fy=fy; return c;
}

static Card gen_booster_card(void){
    const char *types_dummy=0; (void)types_dummy;
    const float tw[8]={0.15f,0.1f,0.1f,0.25f,0.1f,0.1f,0.1f,0.1f}; /* joker,wild,tarot,bonus,upgrade,steel,coin,normal */
    int t=wchoice(tw,8);
    if(t==0){ const float jw[3]={0.6f,0.3f,0.1f}; int r=wchoice(jw,3); return mk_joker("cur"[r]); }
    if(t==1){ Card c=mk_normal(randint(0,12),randint(0,3)); c.mods[c.nmods++]=(Mod){M_WILD,0,0,0}; return c; }
    if(t==2){ /* tarot */
        const float tt[3]={0.5f,0.3f,0.2f}; int k=wchoice(tt,3);
        const float rr[2]={0.7f,0.3f}; int lvl = wchoice(rr,2)? 4:2;
        Card c; memset(&c,0,sizeof c); c.x=150;c.y=80;c.tx=150;c.ty=80;c.alive=1; c.level_bonus=lvl; c.rarity=lvl>2?'r':'c';
        if(k==0){ c.kind=CK_TAROT_RANK; c.fx=4; c.fy=4; c.param_rank=randint(0,12); }
        else if(k==1){ c.kind=CK_TAROT_SUIT; c.fx=11; c.fy=5; c.param_suit=randint(0,3); }
        else { c.kind=CK_TAROT_UPRANK; c.fx=12; c.fy=5; c.param_rank=randint(1,12); }
        return c;
    }
    if(t==3){ /* bonus card */
        const float bw[3]={0.7f,0.2f,0.1f}; int r=wchoice(bw,3);
        Card c=mk_normal(randint(0,12),randint(0,3));
        if(r==0) c.mods[c.nmods++]=(Mod){M_BASE,randint(5,20),0,0};
        else if(r==1) c.mods[c.nmods++]=(Mod){M_MULT,0,randint(2,4),0};
        else c.mods[c.nmods++]=(Mod){M_RARE,randint(10,20),randint(2,5),0};
        return c;
    }
    if(t==4){ /* hand-type upgrade */
        Card c; memset(&c,0,sizeof c); c.kind=CK_UPGRADE; c.x=150;c.y=80;c.tx=150;c.ty=80;c.alive=1; c.fx=3; c.fy=4;
        const float hw[13]={1,1,1,2,2,3,3,4,4,5,5,5,4}; c.hand_type=wchoice(hw,13);
        const float rr[2]={0.7f,0.3f}; c.level_bonus = wchoice(rr,2)?2:1; return c;
    }
    if(t==5){ Card c=mk_normal(randint(0,12),randint(0,3)); c.mods[c.nmods++]=(Mod){M_STEEL,randint(10,100),0,0}; return c; }
    if(t==6){ Card c=mk_normal(randint(0,12),randint(0,3)); c.mods[c.nmods++]=(Mod){M_COIN,0,0,0}; return c; }
    return mk_normal(randint(0,12),randint(0,3));
}

static void open_booster(void){
    g.booster_wait=0; g.state=ST_BOOSTER;
    int size = 4 + (g.hands_played<4?1:0) + (g.discard_limit>0?1:0) + g.extra_booster;
    if(size>7) size=7;
    g.nboost=size;
    for(int i=0;i<size;i++){ g.booster[i]=gen_booster_card(); g.booster[i].tx=7+i*18+(i?1:0); g.booster[i].ty=103; g.booster[i].selected=0; }
    g.cursor=0; say("Select 1 or 2");
}

static void start_new_round(void){
    g.score=0; g.target+=500; g.hands_played=0; g.discard_limit=4; g.round++;
    deck_build(); draw_hand(); layout_jokers();
    snd(&shuffle_snd);
    g.state=ST_PLAY;
}

static void confirm_booster(void){
    int nsel=0; for(int i=0;i<g.nboost;i++) if(g.booster[i].selected)nsel++;
    if(!nsel) return;
    for(int i=0;i<g.nboost;i++){
        Card *c=&g.booster[i]; if(!c->selected) continue;
        switch(c->kind){
            case CK_UPGRADE: g.hand_lvl[c->hand_type]+=c->level_bonus; break;
            case CK_TAROT_RANK: { int rem=c->level_bonus>2?4:2;
                for(int k=0;k<g.ncol && rem>0;){ if(g.col[k].rank_x==c->param_rank){ g.col[k]=g.col[--g.ncol]; rem--; } else k++; } } break;
            case CK_TAROT_SUIT: { int rem=c->level_bonus>2?4:2;
                for(int k=0;k<g.ncol && rem>0;){ if(g.col[k].suit_y==c->param_suit){ g.col[k]=g.col[--g.ncol]; rem--; } else k++; } } break;
            case CK_TAROT_UPRANK: { int up=c->level_bonus;
                for(int k=0;k<g.ncol && up>0;k++){ if(g.col[k].rank_x==c->param_rank){ g.col[k].rank_x=(g.col[k].rank_x+1)%13; g.col[k].fx=g.col[k].rank_x; up--; } } } break;
            case CK_JOKER:
                if(g.njok<2) g.jokers[g.njok++]=*c;
                else { g.jokers[0]=g.jokers[1]; g.jokers[1]=*c; }
                break;
            default: /* normal/bonus/wild/steel/coin → into the deck */
                if(g.ncol<MAXCOL) g.col[g.ncol++]=*c; break;
        }
    }
    g.nboost=0; g.extra_booster=0;
    start_new_round();
    layout_jokers();
}

/* ---- input --------------------------------------------------------------- */
static void move_cursor(int *cur,int n,int d){ if(n>0){ *cur=((*cur+d)%n+n)%n; } }

static void handle_play(const MoteInput *in){
    if((mote_just_pressed(in,MOTE_BTN_LB)||mote_just_pressed(in,MOTE_BTN_RB)) && g.njok>0){
        g.state=ST_JOKER; g.cursor = mote_just_pressed(in,MOTE_BTN_LB)?0:g.njok-1;
        return;
    }
    if(mote_just_pressed(in,MOTE_BTN_LEFT))  move_cursor(&g.cursor,g.nhand,-1);
    if(mote_just_pressed(in,MOTE_BTN_RIGHT)) move_cursor(&g.cursor,g.nhand,+1);
    if(mote_just_pressed(in,MOTE_BTN_UP)){
        if(g.cursor<g.nhand && !g.hand[g.cursor].selected && nsel_hand()<5){ g.hand[g.cursor].selected=1; update_score_display(); snd(&select_snd); }
    }
    if(mote_just_pressed(in,MOTE_BTN_DOWN)){
        if(g.cursor<g.nhand && g.hand[g.cursor].selected){ g.hand[g.cursor].selected=0; update_score_display(); snd(&select_snd); }
    }
    if(mote_just_pressed(in,MOTE_BTN_A)) play_hand();
    if(mote_just_pressed(in,MOTE_BTN_B)) discard_and_draw();
}

static void handle_joker(const MoteInput *in){
    if(mote_just_pressed(in,MOTE_BTN_LB)||mote_just_pressed(in,MOTE_BTN_RB)){ g.state= (g.nboost>0)?ST_BOOSTER:ST_PLAY; return; }
    if(mote_just_pressed(in,MOTE_BTN_LEFT))  move_cursor(&g.cursor,g.njok,-1);
    if(mote_just_pressed(in,MOTE_BTN_RIGHT)) move_cursor(&g.cursor,g.njok,+1);
}

static void handle_booster(const MoteInput *in){
    if((mote_just_pressed(in,MOTE_BTN_LB)||mote_just_pressed(in,MOTE_BTN_RB)) && g.njok>0){
        g.state=ST_JOKER; g.cursor = mote_just_pressed(in,MOTE_BTN_LB)?0:g.njok-1; return;
    }
    if(mote_just_pressed(in,MOTE_BTN_LEFT))  move_cursor(&g.cursor,g.nboost,-1);
    if(mote_just_pressed(in,MOTE_BTN_RIGHT)) move_cursor(&g.cursor,g.nboost,+1);
    int nsel=0; for(int i=0;i<g.nboost;i++) if(g.booster[i].selected)nsel++;
    if(mote_just_pressed(in,MOTE_BTN_UP)){
        Card *c=&g.booster[g.cursor];
        int seljok=0; for(int i=0;i<g.nboost;i++) if(g.booster[i].selected&&g.booster[i].kind==CK_JOKER)seljok++;
        if(!(c->kind==CK_JOKER && g.njok+seljok>=2) && !c->selected && nsel<2){ c->selected=1; snd(&select_snd); }
    }
    if(mote_just_pressed(in,MOTE_BTN_DOWN)){
        if(g.booster[g.cursor].selected){ g.booster[g.cursor].selected=0; snd(&select_snd); }
    }
    if(mote_just_pressed(in,MOTE_BTN_A)) confirm_booster();
}

/* ---- rendering ----------------------------------------------------------- */
static void draw_cell(uint16_t *fb,float cx,float cy,int fx,int fy){
    mote->blit_ex(fb,&cards_img, cx,cy, fx*CW, fy*CH, CW, CH, 0.0f, 1.0f, MOTE_BLEND_NONE, 0,128);
}
static int overlay_cell(const Card *c,int *ofx,int *ofy,int *alpha){
    *alpha=0;
    for(int i=0;i<c->nmods;i++){ ModType t=c->mods[i].type;
        /* rare = a shiny holographic tile (cols 9-10, rows 4-5). Pick the variant
         * DETERMINISTICALLY from the card so it stays put — the old per-frame
         * randint() (and rows 9-10, off the 6-row sheet) drew twitching garbage. */
        if(t==M_RARE){ int hsh=c->fx*7+c->fy*5+c->rank_x*3+c->suit_y; *ofx=9+(hsh&1); *ofy=4+((hsh>>1)&1); *alpha=1; return 1; }
        if(t==M_BASE){*ofx=7;*ofy=4;return 1;}
        if(t==M_MULT){*ofx=8;*ofy=4;return 1;}
        if(t==M_WILD){*ofx=6;*ofy=5;return 1;}
        if(t==M_STEEL){*ofx=12;*ofy=4;return 1;}
        if(t==M_COIN){*ofx=11;*ofy=4;return 1;}
    }
    return 0;
}
static void draw_card(uint16_t *fb,const Card *c){
    float cy = c->y - (c->selected?5:0);
    draw_cell(fb,c->x,cy,6,4);          /* card base */
    draw_cell(fb,c->x,cy,c->fx,c->fy);  /* face */
    int ofx,ofy,al; if(overlay_cell(c,&ofx,&ofy,&al))
        mote->blit_ex(fb,&cards_img,c->x,cy, ofx*CW,ofy*CH,CW,CH,0.0f,1.0f, al?MOTE_BLEND_ALPHA:MOTE_BLEND_NONE,0,128);
}
/* HUD text uses the baked 9px proportional `munro` font (mote->text_font), which
 * fills the background's value boxes far better than the tiny built-in 3x5.
 * Centre on (cx,cy) the way the original Thumby Text2DNode did. */
static int fw(const char *s){ int w=0; for(;*s;s++){ unsigned c=(unsigned char)*s;
    if(c<munro.first||c>=(unsigned)(munro.first+munro.count)) c=munro.first; w+=munro.glyphs[c-munro.first].adv; } return w; }
static void text_l(uint16_t *fb,const char *s,int x,int cy,uint16_t col){ mote->text_font(fb,&munro,s,x,cy-munro.line_h/2,col); }
static void text_c(uint16_t *fb,const char *s,int cx,int cy,uint16_t col){ text_l(fb,s,cx-fw(s)/2,cy,col); }
static void num_c(uint16_t *fb,int cx,int cy,uint16_t col,int v){ char b[16]; snprintf(b,sizeof b,"%d",v); text_c(fb,b,cx,cy,col); }

static void render(uint16_t *fb){
    mote->blit(fb,&background_img,0,0,0,0,128,128,0,0,128);

    /* jokers row */
    for(int i=0;i<g.njok;i++) draw_card(fb,&g.jokers[i]);
    /* played cards (during the 2s reveal) */
    for(int i=0;i<g.nplayed;i++) draw_card(fb,&g.played[i]);
    /* hand or booster row */
    if(g.state==ST_BOOSTER){ for(int i=0;i<g.nboost;i++) draw_card(fb,&g.booster[i]); }
    else { for(int i=0;i<g.nhand;i++) draw_card(fb,&g.hand[i]); }

    /* cursor pointer (cell 5,4) under the focused card */
    if(g.state==ST_PLAY && g.cursor<g.nhand) draw_cell(fb,g.hand[g.cursor].x,g.hand[g.cursor].y+19,5,4);
    else if(g.state==ST_JOKER && g.cursor<g.njok) draw_cell(fb,g.jokers[g.cursor].x,g.jokers[g.cursor].y+19,5,4);
    else if(g.state==ST_BOOSTER && g.cursor<g.nboost) draw_cell(fb,g.booster[g.cursor].x,g.booster[g.cursor].y+19,5,4);

    /* HUD text (positions mirror the original layout over the baked background) */
    uint16_t cyan=0x04FF, pink=0xFAEA, white=0xFFFF;
    /* centres measured from the baked background's value boxes (ink centre = cy) */
    { char b[24]; snprintf(b,sizeof b,"%d/%d",g.score,g.target); text_c(fb,b,68,11,white); }  /* top box */
    num_c(fb,51,24,cyan,g.disp_base);       /* base-score box */
    num_c(fb,85,24,pink,g.disp_mult);       /* mult box */
    text_c(fb,g.msg,80,38,white);           /* wide hand-type / prompt box */
    num_c(fb,103,59,white,4-g.hands_played);/* hands remaining */
    num_c(fb,117,59,white,g.discard_limit); /* discards left */
    num_c(fb,110,75,white,g.round);         /* round box */
    if(g.state==ST_OVER){ char b[24]; snprintf(b,sizeof b,"Best %d",g.best_hand); text_c(fb,b,64,52,white); }

    /* popups */
    for(int i=0;i<MAXPOP;i++){ Pop *p=&g.pops[i]; if(!p->alive) continue;
        if(p->main) mote->text_2x(fb,p->s,(int)p->x-8,(int)p->y-6,p->col);
        else text_c(fb,p->s,(int)p->x,(int)p->y,p->col);
    }

    if(g.state==ST_INTRO) mote->blit(fb,&title_img,0,0,0,0,128,128,0,0,128);
}

/* ---- vtbl ---------------------------------------------------------------- */
static void g_init(void){
    uint64_t t=mote->micros(); s_rng=(uint32_t)(t^(t>>32))|1u;
    memset(&g,0,sizeof g);
    g.state=ST_INTRO; g.vol=0.25f; g.target=300; g.discard_limit=4; g.round=1;
    /* 52-card collection */
    g.ncol=0; for(int s=0;s<4;s++) for(int r=0;r<13;r++) g.col[g.ncol++]=mk_normal(r,s);
    deck_build(); draw_hand(); layout_jokers();
    say("Play/Discard 5 cards");
    mote->audio_set_master(g.vol);
}

static void tween_to(Card *c,float dt){
    c->x = mote_approach(c->x, c->tx, 9.0f, dt);
    c->y = mote_approach(c->y, c->ty, 9.0f, dt);
}

static void g_update(float dt){
    const MoteInput *in=mote->input();
    if(mote_just_pressed(in,MOTE_BTN_MENU)){ g.vol = g.vol>0?0.0f:0.25f; mote->audio_set_master(g.vol); }

    /* tween everything toward targets */
    for(int i=0;i<g.nhand;i++)   tween_to(&g.hand[i],dt);
    for(int i=0;i<g.njok;i++)    tween_to(&g.jokers[i],dt);
    for(int i=0;i<g.nboost;i++)  tween_to(&g.booster[i],dt);
    for(int i=0;i<g.nplayed;i++) tween_to(&g.played[i],dt);
    for(int i=0;i<MAXPOP;i++){ Pop *p=&g.pops[i]; if(!p->alive)continue;
        p->x=mote_approach(p->x,p->tx,4.0f,dt); p->y=mote_approach(p->y,p->ty,4.0f,dt);
        p->t+=dt; if(p->t>=p->life) p->alive=0; }

    /* cascading coin sfx while a score tallies */
    if(g.money_left>0){ g.money_t-=dt; if(g.money_t<=0){ snd(&money_snd); g.money_left--; g.money_iv = g.money_iv*0.85f; if(g.money_iv<0.04f)g.money_iv=0.04f; g.money_t=g.money_iv+0.04f; } }

    switch(g.state){
        case ST_INTRO:
            g.intro_t+=dt;
            if(g.intro_t>0.6f && (mote_just_pressed(in,MOTE_BTN_A)||g.intro_t>2.5f)) g.state=ST_PLAY;
            break;
        case ST_PLAY:   handle_play(in);   break;
        case ST_JOKER:  handle_joker(in);  break;
        case ST_BOOSTER:handle_booster(in);break;
        case ST_BOOSTER_WAIT:
            g.played_t+=dt;
            if(g.played_t>1.2f){ g.nplayed=0; open_booster(); }
            break;
        case ST_OVER:
            g.over_t+=dt; /* stay on the summary; A restarts */
            if(mote_just_pressed(in,MOTE_BTN_A)) g_init();
            break;
    }
    /* clear played row after its reveal during normal play */
    if(g.nplayed && g.state==ST_PLAY){ g.played_t+=dt; if(g.played_t>1.4f){ for(int i=0;i<g.nplayed;i++) g.played[i].tx=g.played[i].x-120; if(g.played_t>2.2f) g.nplayed=0; } }
}

static void g_render_band(uint16_t *fb,int y0,int y1){ (void)fb;(void)y0;(void)y1; }
static void g_overlay(uint16_t *fb){ render(fb); }

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=g_render_band, .overlay=g_overlay,
    .config = { .max_sprites = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
MOTE_GAME_META("ThumbAtro", "austinio7116");
MOTE_GAME_VERSION("1.0.0");
