/*
 * papermote — a paper.io-style territory game for the Thumby Color.
 *
 * Claim ground by closing loops back into your land; enclosing steals (and can
 * destroy) rivals. Cut a rival's trail and you take their whole base; cross your
 * own and you die. Five AI rivals (greedy / aggressive / cautious), three modes.
 *
 * Built entirely on the engine: render_band (dual-core scrolling+zooming raster
 * with claim-flash, bevelled edges, particles), overlay (HUD + minimap + danger
 * vignette), alloc (the grids), MoteSfx stings (size-scaled), a procedural bgm
 * bed that builds as you dominate, and save (best score).
 *
 * Controls — TITLE: D-pad set up your run, A start.  PLAY: D-pad steer,
 *            B restart (on the end screen), hold MENU 3s for the engine menu.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "claim.sfx.h"
#include "claimbig.sfx.h"
#include "combo.sfx.h"
#include "cut.sfx.h"
#include "die.sfx.h"
#include "spawn.sfx.h"
#include "slime.h"
#include "ghost.h"
#include "bug.h"
#include "fish.h"
#include "snake.h"
#include "chick.h"
#include "cat.h"
#include "alien.h"
#include "frog.h"
#include "robot.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ------------------------------------------------------------------ debug tap
 * Host-only, env-gated instrumentation for the 2P link (MOTE_PAPER_DEBUG=1):
 * per-step traces, trail-cut events, and causally-clocked FNV hashes of bot
 * state + territory so two instances' logs can be diffed to PROVE sync.
 * Extra test hooks: MOTE_PAPER_FPS (cap host fps so MOTE_KEYS frames map to
 * wall time), MOTE_PAPER_SCRIPT ("step:dir,..." cell-precise steering of the
 * local head), MOTE_PAPER_SLOWSTART (ms; delay the first LINKWAIT poll to
 * model a relay/CDC burst at match start), MOTE_PAPER_NOBOTS (2P sans bots
 * for the trail-crossing repro). None of it exists in the device build. */
#ifdef MOTE_HOST
#include <stdlib.h>   /* getenv — an implicit decl would truncate the pointer */
static int pdbg_on(void){ static int on=-1; if(on<0) on=getenv("MOTE_PAPER_DEBUG")!=0; return on; }
#define PDBG(...) do{ if(pdbg_on()){ fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); fflush(stderr);} }while(0)
static uint32_t pdbg_fnv(const void*d,int n){ const uint8_t*p=(const uint8_t*)d; uint32_t h=2166136261u;
    while(n--){ h^=*p++; h*=16777619u; } return h; }
#else
#define PDBG(...) do{}while(0)
#endif

/* ===================================================================== grid */
#define MW 100
#define MH 100
#define NCELL (MW*MH)
#define NP 6                       /* 1 human + 5 AI */
#define IDX(x,y) ((y)*MW+(x))

static uint8_t *owner;             /* 0 = empty, else player+1 */
static uint8_t *trail;             /* 0 = none,  else player+1 */
static uint8_t *vis;               /* flood-fill scratch */
static uint8_t *fresh;             /* per-cell claim-flash (255 -> 0) */
static int     *stk;               /* flood-fill stack */

/* selectable creatures — each a baked sprite + a theme colour (its territory). */
#define NCREA 10
static const struct { const MoteImage *img; const char *name; int r,g,b; } CREA[NCREA] = {
    {&slime_img,"SLIME", 70,200,110},{&ghost_img,"GHOST",120,180,250},{&bug_img,  "BUG",  230, 70, 70},
    {&fish_img, "FISH",  60,200,225},{&snake_img,"SNAKE",150,210, 60},{&chick_img,"CHICK",245,205, 70},
    {&cat_img,  "CAT",  240,150, 60},{&alien_img,"ALIEN",180,110,235},{&frog_img, "FROG",  80,190, 90},
    {&robot_img,"ROBOT",200, 90,160},
};
typedef struct { uint16_t terr, trail, head; uint8_t r,g,b; } Pal;
static Pal pal[NP];                /* index by entity */

/* ================================================================= entities */
typedef struct {
    int   cx, cy, px, py, dx, dy;
    float fx, fy;
    int   alive, ai, on_trail;
    int   home_x, home_y, trail_len, target_len;
    int   pend_dx, pend_dy;
    float respawn;                 /* >0 counting down · <0 = no respawn (LAST mode) */
    int   area, kills, creature;
    uint8_t persona;               /* 0 greedy · 1 aggressive · 2 cautious */
} Ent;
static Ent ent[NP];

/* ============================================================ particles */
typedef struct { float x,y,vx,vy,life,max; uint16_t col; } Part;
#define MAXPART 200
static Part part[MAXPART]; static int npart;
static void spawn_burst(float x,float y,uint16_t col,int n,float spd){
    for(int i=0;i<n&&npart<MAXPART;i++){ Part*q=&part[npart++];
        float a=(float)(rand()%628)*0.01f, s=spd*(0.3f+(rand()%100)*0.007f);
        q->x=x; q->y=y; q->vx=cosf(a)*s; q->vy=sinf(a)*s; q->max=q->life=0.5f+(rand()%100)*0.006f; q->col=col; } }

/* ============================================================ game state */
enum { S_TITLE, S_PLAY, S_DEAD, S_WIN, S_LINKWAIT };
enum { M_ENDLESS, M_TIMED, M_LAST, M_LINK };
static const char *MODE_L[4]={"ENDLESS","TIMED 90s","LAST STANDING","2P LINK"};
static const char *NAME_L[8]={"BLOB","NEON","VOID","DASH","FLUX","ACE","ZIP","NOVA"};
static int   state = S_TITLE, mode = M_ENDLESS;
static int   sel_creature=0, sel_name=0, sel_mode=0, menu_row=0;
static float step_acc, claim_flash, shake, danger, elapsed, game_t;
static float cam_fx, cam_fy, cellpx = 6.0f;
static int   score, best_score, combo; static float combo_t, msg_t; static char msg[28];
static int   milestone;            /* highest area-% milestone announced */
static float bgm_t; static int bgm_step;
static uint32_t rng = 0x1234567u;
static int irand(int n){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return (int)((rng>>1)%(unsigned)n); }

/* ---- 2P link duel state (shared 100x100 grid · fixed corners · victim-authoritative
 * deaths).  See the LINK section below for the wire protocol + replication model. ---- */
#define LK_MAGIC  0xA5
#define LK_PROTO  1
#define KO_TARGET 3                 /* first to 3 knockouts wins the duel */
static int      g_link, me, you;    /* link active · local / remote entity id (shared world) */
static uint16_t lk_my_nonce, lk_peer_nonce;
static int      lk_sent_hello, lk_got_hello;
static int      lk_ready;           /* peer confirmed IN-GAME (any 'S'/'P'/... received) — no
                                     * heads move before this, so no step can ever be sent
                                     * to a peer that isn't parsing in-game messages yet */
static float    lk_hello_t, lk_state_t, lk_rx_age, lk_respawn_t;
static int      lk_lost, my_kos, my_deaths, peer_kos;
static uint8_t  lk_msg[16]; static int lk_msg_len;

#define STEP_BASE 0.085f
#define WIN_PCT 60
#define TIMED_SECS 90.0f
static const int DIRS[4][2] = {{1,0},{0,1},{-1,0},{0,-1}};
static inline int in_b(int x,int y){ return x>=0&&y>=0&&x<MW&&y<MH; }

static MoteSound snd_claim, snd_claimbig, snd_combo, snd_cut, snd_die, snd_spawn;

static void say(const char*s,float t){ snprintf(msg,sizeof msg,"%.27s",s); msg_t=t; }

static void stamp_home(int p,int cx,int cy,int r){
    for(int y=cy-r;y<=cy+r;y++) for(int x=cx-r;x<=cx+r;x++)
        if(in_b(x,y)){ owner[IDX(x,y)]=p+1; trail[IDX(x,y)]=0; } }
static int count_area(int p){ int c=0; uint8_t v=p+1; for(int i=0;i<NCELL;i++) if(owner[i]==v)c++; return c; }
static void recount(void){ for(int p=0;p<NP;p++) ent[p].area=count_area(p); }

/* flood-fill claim: convert p's trail, fill everything enclosed, flash + return count */
static int do_claim(int p){
    uint8_t v=p+1; int gained=0; long sx=0,sy=0,sn=0;
    for(int i=0;i<NCELL;i++) if(trail[i]==v){ owner[i]=v; trail[i]=0; }
    memset(vis,0,NCELL); int sp=0;
    for(int x=0;x<MW;x++){ int a=IDX(x,0),b=IDX(x,MH-1);
        if(owner[a]!=v&&!vis[a]){vis[a]=1;stk[sp++]=a;} if(owner[b]!=v&&!vis[b]){vis[b]=1;stk[sp++]=b;} }
    for(int y=0;y<MH;y++){ int a=IDX(0,y),b=IDX(MW-1,y);
        if(owner[a]!=v&&!vis[a]){vis[a]=1;stk[sp++]=a;} if(owner[b]!=v&&!vis[b]){vis[b]=1;stk[sp++]=b;} }
    while(sp){ int c=stk[--sp]; int x=c%MW,y=c/MW;
        for(int d=0;d<4;d++){ int nx=x+DIRS[d][0],ny=y+DIRS[d][1]; if(!in_b(nx,ny))continue; int n=IDX(nx,ny);
            if(owner[n]!=v&&!vis[n]){ vis[n]=1; stk[sp++]=n; } } }
    for(int i=0;i<NCELL;i++){ if(owner[i]==v){ sx+=i%MW; sy+=i/MW; sn++; }
        if(owner[i]!=v&&!vis[i]){ owner[i]=v; fresh[i]=255; gained++; } }
    if(sn){ ent[p].home_x=(int)(sx/sn); ent[p].home_y=(int)(sy/sn); }   /* home = centroid */
    ent[p].trail_len=0; return gained;
}

static void respawn_ai(int p);
static void kill_ent(int v,int killer){     /* killer gains v's land (-1 = free it) */
    uint8_t m=v+1, k=killer>=0?(uint8_t)(killer+1):0;
    float wx=ent[v].fx, wy=ent[v].fy;
    for(int i=0;i<NCELL;i++){ if(trail[i]==m)trail[i]=0; if(owner[i]==m)owner[i]=k; }
    ent[v].on_trail=0; ent[v].trail_len=0;
    spawn_burst(wx,wy,pal[v].trail,18,9.0f);
    mote->audio_play(&snd_die, (v==me)?0.7f:0.25f);
    if(g_link){ ent[v].alive=0; return; }   /* link: ko/respawn/state handled by the LINK layer */
    if(v==0){ if(score>best_score){best_score=score; int b=best_score; mote->save(0,&b,sizeof b);} state=S_DEAD; }
    else { ent[v].alive=0; ent[v].respawn = (mode==M_LAST) ? -1.0f : 1.5f+irand(20)*0.1f; }
}

static void respawn_ai(int p){
    for(int tries=0;tries<80;tries++){
        int cx=6+irand(MW-12), cy=6+irand(MH-12), free=1;
        for(int y=cy-3;y<=cy+3&&free;y++) for(int x=cx-3;x<=cx+3;x++){ int c=IDX(x,y); if(owner[c]||trail[c]){free=0;break;} }
        if(free){ stamp_home(p,cx,cy,2); Ent*e=&ent[p];
            e->cx=e->px=cx; e->cy=e->py=cy; e->fx=cx+0.5f; e->fy=cy+0.5f;
            int d=irand(4); e->dx=DIRS[d][0]; e->dy=DIRS[d][1];
            e->alive=1; e->on_trail=0; e->trail_len=0; e->target_len=10+irand(26);
            e->home_x=cx; e->home_y=cy; e->respawn=0; spawn_burst(cx+0.5f,cy+0.5f,pal[p].head,10,5.0f);
            mote->audio_play(&snd_spawn,0.4f); return; } }
    ent[p].respawn=0.5f;
}

/* who currently leads on area (for aggressive AI to hunt) */
static int leader(void){ int b=0; for(int p=1;p<NP;p++) if(ent[p].area>ent[b].area)b=p; return b; }

/* ============================================================ AI steering */
/* from (nx,ny) arrived heading (dx,dy): is there at least one survivable onward
 * move? a wall counts (it deflects); only our OWN trail ahead is fatal. */
static int ai_safe_onward(int p,int nx,int ny,int dx,int dy){
    for(int d=0;d<4;d++){ if(DIRS[d][0]==-dx&&DIRS[d][1]==-dy)continue;
        int tx=nx+DIRS[d][0],ty=ny+DIRS[d][1];
        if(!in_b(tx,ty)) return 1;
        if(trail[IDX(tx,ty)]!=p+1) return 1; }
    return 0; }
static int ai_pick(int p){
    Ent*e=&ent[p]; int best=-1; float bs=-1e9f;
    int returning = (e->on_trail && e->trail_len>=e->target_len) || e->trail_len>=45;
    int threat=0;
    for(int q=0;q<NP&&!threat;q++){ if(q==p||!ent[q].alive)continue;
        if(abs((int)ent[q].fx-e->cx)+abs((int)ent[q].fy-e->cy)<=4) threat=1; }
    int tgt=-1; if(e->persona==1){ tgt=leader(); if(tgt==p)tgt=-1; }   /* aggressive -> hunt the leader */
    for(int d=0;d<4;d++){
        if(DIRS[d][0]==-e->dx&&DIRS[d][1]==-e->dy) continue;
        int nx=e->cx+DIRS[d][0], ny=e->cy+DIRS[d][1]; if(!in_b(nx,ny)) continue;
        int n=IDX(nx,ny); if(trail[n]==p+1) continue;                     /* NEVER our own trail */
        float s=(float)irand(100)*0.02f;
        if(!ai_safe_onward(p,nx,ny,DIRS[d][0],DIRS[d][1])) s-=1000.0f;     /* don't walk into a dead end */
        if(DIRS[d][0]==e->dx&&DIRS[d][1]==e->dy) s+=0.6f;                  /* prefer straight (avoid tight curls) */
        int dh=abs(nx-e->home_x)+abs(ny-e->home_y);
        if(returning || (threat && e->on_trail)) s -= dh*1.2f;            /* bank it / flee */
        else {
            s += dh*0.30f;
            if(owner[n]!=p+1) s+=2.0f;
            if(owner[n]&&owner[n]!=p+1) s+=1.6f;                          /* eat enemy land */
            if(e->persona==1 && trail[n] && trail[n]!=p+1) s+=7.0f;       /* cut a trail */
            if(tgt>=0) s -= (abs(nx-(int)ent[tgt].fx)+abs(ny-(int)ent[tgt].fy))*0.18f;
            if(e->persona==2 && e->trail_len>e->target_len/2) s-=2.5f;
        }
        if(s>bs){ bs=s; best=d; }
    }
    /* fallbacks (never onto our own trail): any open cell, else a wall to deflect off */
    if(best<0) for(int d=0;d<4;d++){ int nx=e->cx+DIRS[d][0],ny=e->cy+DIRS[d][1];
        if(!(DIRS[d][0]==-e->dx&&DIRS[d][1]==-e->dy)&&in_b(nx,ny)&&trail[IDX(nx,ny)]!=p+1){best=d;break;} }
    if(best<0) for(int d=0;d<4;d++){ if(DIRS[d][0]==-e->dx&&DIRS[d][1]==-e->dy)continue;
        if(!in_b(e->cx+DIRS[d][0],e->cy+DIRS[d][1])){best=d;break;} }     /* wall -> deflect, survivable */
    return best;
}

static void on_kill(int killer){            /* combo + score bookkeeping for a kill by `killer` */
    ent[killer].kills++;
    if(killer==0){ combo = combo_t>0 ? combo+1 : 1; combo_t=2.2f; score += 50*combo;
        if(combo>=2){ mote->audio_play(&snd_combo,0.5f); char b[28]; snprintf(b,sizeof b,"COMBO x%d!",combo); say(b,1.4f); } }
}

/* ================================================================= 2P LINK ==
 * A territory duel over the ABI-v43 byte pipe: YOU vs the PEER only (no AI).
 *
 * SHARED WORLD, NO ARENA SYNC.  papermote's board starts empty save two 7x7
 * spawn bases, and world setup uses no trig/sqrt — so there is nothing
 * procedural to diverge across x86 Studio vs ARM device.  Both units place
 * entity id 0 at the FIXED top-left corner and id 1 at the FIXED bottom-right,
 * in ONE shared coordinate system; the nonce winner is id 0.  Camera/HUD follow
 * the LOCAL id (`me`); the peer is `you`.  Zero seed handshake needed.
 *
 * REPLICATION — per-step, not batched.  Each unit simulates ONLY its own head
 * from input; on every local grid step it sends the FINAL direction it moved
 * ('S' dir).  The peer replays that step onto the SHARED grid with the exact
 * same movement/trail/claim code (lk_move) for the remote id.  Grid steps run
 * up to ~22 Hz — faster than the 15 Hz keepalive — but the byte pipe is
 * in-order and lossless, so one 3-byte 'S' per step guarantees no skipped cell
 * and exact turns + loop-close fills.  (~66 B/s; trivially inside the RX budget.)
 *
 * DEATHS — VICTIM-AUTHORITATIVE, to dodge the interleave race (both heads cut
 * each other in the same window).  A kill is APPLIED only where the victim is
 * the LOCAL player: each unit detects its own death (own-trail hit, being cut
 * by the peer's replayed step, or being enclosed to 0 area) and broadcasts
 * 'K' victim killer.  Cutting the PEER is deferred — the peer detects it and
 * sends 'K'; on receipt both units run kill_ent(victim,killer) so the cutter
 * takes the base identically.  Mutual cuts => both send 'K' => fair double-KO.
 *
 * WIN — first to KO_TARGET(3) knockouts (a KO for me == a death for the peer,
 * so both units agree).  Death => respawn at your own fixed corner after ~1.2s.
 *
 * AI BOTS — AUTHORITY-SIMULATED, INPUT-REPLAYED.  The nonce winner (id 0) is
 * the AUTHORITY: it alone runs ai_pick for the four bots (ids 2..5) and, on
 * each bot grid step, streams the FINAL direction taken ('A' bot dir) — the
 * same per-step replication the players use, replayed through the same
 * lk_move/do_claim integer code, so bot positions AND their territory claims
 * are identical on both units (no float, no local RNG on the peer).  Bot steps
 * tick with the shared STEP_BASE cadence (~12 Hz — inside the 10-15 Hz budget;
 * 4 bots = ~16 B per step tick).  Bot DEATHS are decided by the authority only
 * ('K' with a bot victim): the peer defers (its local cut of a bot trail sends
 * the absolute cell in 'C'; the authority verifies against its bot-fresh
 * grid).  Bot respawns are authority-picked and broadcast absolute ('E').
 * Bot kills never count toward the KO race — players only.
 *
 * TRAIL-CUT RECONCILIATION ('C') — WHY: the deferred cut used to rely purely
 * on the victim replaying the cutter's *relative* dir-steps; any head drift
 * (e.g. steps lost in a start burst, see lk_handle 'H') made the replayed
 * ghost cross somewhere else and the kill silently missed.  Now the cutter
 * ALSO reports the ABSOLUTE cell it crossed ('C' x y).  Deaths stay
 * VICTIM-AUTHORITATIVE: the trail's owner — who always has the freshest copy
 * of its own trail — verifies trail[cell] and only then dies (and broadcasts
 * 'K' as before).  If the cell is no longer its trail (the loop was banked
 * or it already died before the report arrived), the cut is VOID: banking
 * beats a racing cut — that tie-break is deliberate and consistent on both
 * units, because only the owner ever judges its own trail.
 *
 * WIRE (after 0xA5 magic; fixed-length, parser resyncs on a bad type):
 *   'H' proto nonce16  hello (resent ~0.5s; higher nonce = id 0 / authority)
 *   'S' dir            one grid step, dir = index into DIRS (0..3)
 *   'K' victim killer reason   a death (victim 0..5, killer 255 = none/self)
 *   'R' cx cy dir      respawn at a corner (absolute resync of the head)
 *   'P' alive kos      ~15 Hz keepalive + peer's KO count (3s silence = LOST)
 *   'Q'                orderly quit -> peer sees OPPONENT LEFT
 *   'A' bot dir        authority: one bot grid step (bot 2..5, replayed)
 *   'E' bot cx cy dir  authority: bot respawn (absolute)
 *   'C' x y            cutter: my head crossed a trail at this cell — the
 *                      trail's owner verifies and applies its own death
 * -------------------------------------------------------------------------- */
static const int LK_SX[2]={16,82}, LK_SY[2]={16,82};   /* id 0 / id 1 spawn+respawn corners */
#define NBOT 4                                          /* 2P bots: entity ids 2..5 */
static const int LK_BX[NBOT]={82,16,49,49}, LK_BY[NBOT]={16,82,12,86}, LK_BD[NBOT]={2,0,1,3};
static int lk_bots;                                     /* bots enabled this match */
static int lk_auth(void){ return me==0; }               /* nonce winner simulates the bots */
#ifdef MOTE_HOST
static uint32_t lk_botsteps;                            /* causal clock: applied bot steps */
#endif
static void apply_palette(void);                        /* defined in the lifecycle section */

static int dir_index(int dx,int dy){ for(int d=0;d<4;d++) if(DIRS[d][0]==dx&&DIRS[d][1]==dy) return d; return 0; }
static void lk_new_nonce(void){ lk_my_nonce=(uint16_t)(mote->micros()*2654435761u>>8); }
static void lk_send(const void*b,int n){ mote->link_send(b,n); }
static void lk_send_hello(void){ uint8_t m[5]={LK_MAGIC,'H',LK_PROTO,(uint8_t)lk_my_nonce,(uint8_t)(lk_my_nonce>>8)}; lk_send(m,5); }
static void lk_send_step(int dir){ uint8_t m[3]={LK_MAGIC,'S',(uint8_t)dir}; lk_send(m,3); }
static void lk_send_kill(int v,int k,int r){ uint8_t m[5]={LK_MAGIC,'K',(uint8_t)v,(uint8_t)k,(uint8_t)r}; lk_send(m,5); }
static void lk_send_respawn(int cx,int cy,int dir){ uint8_t m[5]={LK_MAGIC,'R',(uint8_t)cx,(uint8_t)cy,(uint8_t)dir}; lk_send(m,5); }
static void lk_send_state(void){ uint8_t m[4]={LK_MAGIC,'P',(uint8_t)ent[me].alive,(uint8_t)my_kos}; lk_send(m,4); }
static void lk_send_bye(void){ uint8_t m[2]={LK_MAGIC,'Q'}; lk_send(m,2); }
static void lk_send_bot_step(int b,int dir){ uint8_t m[4]={LK_MAGIC,'A',(uint8_t)b,(uint8_t)dir}; lk_send(m,4); }
static void lk_send_bot_spawn(int b,int cx,int cy,int dir){ uint8_t m[6]={LK_MAGIC,'E',(uint8_t)b,(uint8_t)cx,(uint8_t)cy,(uint8_t)dir}; lk_send(m,6); }
static void lk_send_cut(int cx,int cy){ uint8_t m[4]={LK_MAGIC,'C',(uint8_t)cx,(uint8_t)cy}; lk_send(m,4); }

/* my head just died — transfer land (kill_ent), tally, tell the peer, start respawn.
 * Only a kill BY THE PEER advances the KO race (bot kills cost land + a respawn,
 * nothing else — otherwise my_deaths could hit 3 without the peer's my_kos ever
 * reaching 3 and the two units would end in different states). */
static void lk_local_death(int killer,int reason){
    if(!ent[me].alive) return;
    PDBG("[P%d] DIE me=%d killer=%d reason=%d at (%d,%d)",me,me,killer,reason,ent[me].cx,ent[me].cy);
    kill_ent(me, killer);                 /* g_link path: just clears my land+trail & alive */
    if(killer==you) my_deaths++;
    lk_send_kill(me, killer, reason);
    lk_respawn_t = 1.2f; shake=0.5f;
}

/* authority verdict: a bot died (cut / enclosed).  Applied locally + broadcast;
 * the peer applies it from 'K'.  Respawn is scheduled here (authority-only). */
static void lk_bot_die(int v,int k){
    if(v<2||v>=NP||!ent[v].alive) return;
    PDBG("[P%d] BOTDIE v=%d killer=%d at (%d,%d)",me,v,k,ent[v].cx,ent[v].cy);
    kill_ent(v, k);
    ent[v].respawn = 1.5f + irand(20)*0.1f;
    if(k==me){ score+=25; mote->audio_play(&snd_cut,0.5f); }
    lk_send_kill(v, k<0?255:k, 0);
}

/* one grid step for entity p (e->dx/e->dy already set).  Mirrors do_step's per-entity
 * body but movement is strictly ONE CELL per call (steps are replicated per cell —
 * no frame-endpoint sampling, so a 1-cell trail can never be tunnelled over) and
 * every death has ONE owner: players are victim-authoritative (only `me` applies
 * its own death here), bots are authority-authoritative (only the nonce winner
 * calls lk_bot_die; the peer defers and/or reports the cell via 'C').
 * allow_deflect: 1 for a locally simulated head (RNG wall-bounce ok); 0 for a
 * replayed remote step (dir is already post-deflect — if it's OOB the grids
 * drifted, so skip).  Returns 1 if the head actually advanced. */
static int lk_move(int p, int allow_deflect){
    Ent*e=&ent[p]; if(!e->alive) return 0;
    int nx=e->cx+e->dx, ny=e->cy+e->dy;
    if(!in_b(nx,ny)){
        if(!allow_deflect) return 0;
        int ax=e->dx?0:1, ay=e->dx?1:0, r=irand(2)?1:-1, turned=0;
        for(int s=0;s<2;s++){ int ndx=ax*r,ndy=ay*r,tx=e->cx+ndx,ty=e->cy+ndy;
            if(in_b(tx,ty)&&trail[IDX(tx,ty)]!=p+1){ e->dx=ndx; e->dy=ndy; nx=tx; ny=ty; turned=1; break; } r=-r; }
        if(!turned) return 0;
    }
    int n=IDX(nx,ny); uint8_t tr=trail[n];
    if(tr){ int victim=tr-1;
        PDBG("[P%d] CROSS p=%d hits trail of %d at (%d,%d)",me,p,victim,nx,ny);
        if(victim==p){                        /* own trail */
            if(p==me){ lk_local_death(you,1); return 0; }  /* my self-cross: peer gets the KO */
            if(p>=2 && lk_auth()) lk_bot_die(p,-1);        /* bot self-cross (AI avoids it) */
            return 0;                          /* replayed peer self-cross: their call — hold */
        }
        if(victim==me){                        /* someone cut MY trail -> I die, I broadcast */
            lk_local_death(p, 0);
        } else if(victim==you){                /* the PEER's trail: the peer is the judge */
            if(p==me) lk_send_cut(nx,ny);      /* defer, but report the ABSOLUTE cell (see 'C') */
        } else {                               /* a BOT's trail: the authority is the judge */
            if(lk_auth()) lk_bot_die(victim, p);
            else if(p==me) lk_send_cut(nx,ny); /* report the cell; authority verifies + 'K's */
        }
    }
    if(!e->alive) return 0;
    if(e->on_trail && owner[IDX(e->cx,e->cy)]!=p+1) trail[IDX(e->cx,e->cy)]=p+1;
    e->px=e->cx; e->py=e->cy; e->cx=nx; e->cy=ny;
    if(owner[n]==p+1){
        if(e->on_trail){ int g=do_claim(p); recount();
#ifdef MOTE_HOST
            PDBG("[P%d] CLAIM p=%d gained=%d ownhash=%08x",me,p,g,pdbg_fnv(owner,NCELL));
#endif
            if(ent[me].alive && ent[me].area==0) lk_local_death(p==me?you:p, 2);  /* enclosed to nothing */
            if(lk_auth()) for(int q=2;q<NP;q++) if(ent[q].alive&&ent[q].area==0) lk_bot_die(q,p);
            if(p==me){ claim_flash=1.0f; score+=g; mote->audio_play(g>140?&snd_claimbig:&snd_claim,0.55f); }
            else mote->audio_play(&snd_claim,0.16f); }
        e->on_trail=0;
    } else { e->on_trail=1; e->trail_len++; }
    return 1;
}

#ifdef MOTE_HOST
/* MOTE_PAPER_SCRIPT="6:d,50:r,..." — set the pend dir at local step N (test rig) */
static void pdbg_script(Ent*e,int step){
    static int parsed; static struct{int at;int dx,dy;} sc[64]; static int nsc;
    if(!parsed){ parsed=1; const char*s=getenv("MOTE_PAPER_SCRIPT");
        while(s&&*s&&nsc<64){ while(*s==','||*s==' ')s++; if(!*s)break;
            int at=atoi(s); while(*s&&*s!=':')s++; if(*s!=':')break; s++;
            int dx=0,dy=0; if(*s=='l')dx=-1; else if(*s=='r')dx=1; else if(*s=='u')dy=-1; else if(*s=='d')dy=1;
            sc[nsc].at=at; sc[nsc].dx=dx; sc[nsc].dy=dy; nsc++; while(*s&&*s!=',')s++; } }
    for(int i=0;i<nsc;i++) if(sc[i].at==step){ e->pend_dx=sc[i].dx; e->pend_dy=sc[i].dy; }
}
#endif

/* advance MY head one step (from input) and broadcast the direction taken */
static void lk_local_step(void){
    Ent*e=&ent[me]; if(!e->alive) return;
#ifdef MOTE_HOST
    { static int lstep; lstep++; if(pdbg_on()) pdbg_script(e,lstep); }
#endif
    if((e->pend_dx||e->pend_dy)&&!(e->pend_dx==-e->dx&&e->pend_dy==-e->dy)){ e->dx=e->pend_dx; e->dy=e->pend_dy; }
    if(lk_move(me,1) && e->alive){ lk_send_step(dir_index(e->dx,e->dy));
        PDBG("[P%d] LSTEP me=%d (%d,%d)",me,me,e->cx,e->cy); }
}

#ifdef MOTE_HOST
/* causal bot-state hash: logged every 64 APPLIED bot steps on both units.  All
 * bot mutations ride ONE in-order stream (the authority's 'A'/'E'/'K'), so at
 * an equal applied-step count the hashes MUST match — that is the sync proof.
 * (home_x/home_y/area are derived from the owner grid and excluded: they can
 * lag by an in-flight player event without the bots being wrong.) */
static void pdbg_bot_clock(void){
    if(!pdbg_on()) return;
    if((++lk_botsteps&63)!=0) return;
    int st[NBOT*7];
    for(int b=0;b<NBOT;b++){ Ent*e=&ent[2+b]; int*o=st+b*7;
        o[0]=e->cx;o[1]=e->cy;o[2]=e->dx;o[3]=e->dy;o[4]=e->alive;o[5]=e->on_trail;o[6]=e->trail_len; }
    uint32_t bh=pdbg_fnv(st,(int)sizeof st);
    /* bot-owned territory view: owner cells held by bots (players' claims can
     * straddle the window; bot cells are driven by the same single stream) */
    uint32_t oh=2166136261u, th=2166136261u;
    for(int i=0;i<NCELL;i++){ if(owner[i]>=3){ oh^=(uint32_t)i; oh*=16777619u; oh^=owner[i]; oh*=16777619u; }
                              if(trail[i]>=3){ th^=(uint32_t)i; th*=16777619u; th^=trail[i]; th*=16777619u; } }
    PDBG("[P%d] BOTHASH #%u state=%08x own=%08x trail=%08x pos=(%d,%d)(%d,%d)(%d,%d)(%d,%d)",
         me,(unsigned)lk_botsteps,bh,oh,th,
         ent[2].cx,ent[2].cy,ent[3].cx,ent[3].cy,ent[4].cx,ent[4].cy,ent[5].cx,ent[5].cy);
}
#endif

/* authority only: think + advance one bot, broadcast the final direction */
static void lk_bot_step(int b){
    Ent*e=&ent[b]; if(!e->alive) return;
    int d=ai_pick(b); if(d>=0){ e->dx=DIRS[d][0]; e->dy=DIRS[d][1]; }
    if(lk_move(b,1)){
        lk_send_bot_step(b, dir_index(e->dx,e->dy));
#ifdef MOTE_HOST
        pdbg_bot_clock();
#endif
    }
}

static void on_kill_link(void){        /* combo/score/FX when I confirm a KO of the peer */
    combo=combo_t>0?combo+1:1; combo_t=2.2f; score+=50*combo;
    mote->audio_play(&snd_cut,0.6f); shake=0.45f;
    if(combo>=2){ mote->audio_play(&snd_combo,0.5f); char b[28]; snprintf(b,sizeof b,"COMBO x%d!",combo); say(b,1.4f); } }

/* a 'K' arrived (or my deferred cut resolves): apply the death both units share.
 * victim may be the peer OR a bot (bot 'K's only ever come from the authority);
 * my own death is never applied from the wire — I am the only judge of it. */
static void lk_apply_kill(int victim,int killer){
    if(victim<0||victim>=NP||victim==me||!ent[victim].alive) return;   /* dedupe / not my call */
    kill_ent(victim, killer);
    if(victim==you && killer==me){ my_kos++; on_kill_link(); }   /* I took the peer down */
    else if(victim>=2 && killer==me){ score+=25; mote->audio_play(&snd_cut,0.5f); }  /* my bot KO, authority-confirmed */
}

static void lk_respawn_at(int p,int cx,int cy,int dir){
    Ent*e=&ent[p]; e->cx=e->px=cx; e->cy=e->py=cy; e->fx=cx+0.5f; e->fy=cy+0.5f;
    e->dx=DIRS[dir][0]; e->dy=DIRS[dir][1]; e->alive=1; e->on_trail=0; e->trail_len=0;
    e->home_x=cx; e->home_y=cy; stamp_home(p,cx,cy,3); recount();
    spawn_burst(cx+0.5f,cy+0.5f,pal[p].head,10,5.0f);
}
static void lk_respawn_me(void){
    int d=irand(4); lk_respawn_at(me, LK_SX[me], LK_SY[me], d);
    mote->audio_play(&snd_spawn,0.4f);
    lk_send_respawn(LK_SX[me], LK_SY[me], d);
}

/* authority only: pick a clear 7x7 spot for a dead bot, apply + broadcast it */
static void lk_bot_respawn(int b){
    for(int tries=0;tries<80;tries++){
        int cx=6+irand(MW-12), cy=6+irand(MH-12), free=1;
        for(int y=cy-3;y<=cy+3&&free;y++) for(int x=cx-3;x<=cx+3;x++){ int c=IDX(x,y); if(owner[c]||trail[c]){free=0;break;} }
        if(free){ int d=irand(4);
            lk_respawn_at(b,cx,cy,d); ent[b].target_len=10+irand(26);
            lk_send_bot_spawn(b,cx,cy,d);
            PDBG("[P%d] BOTSPAWN b=%d at (%d,%d) dir=%d",me,b,cx,cy,d);
            return; } }
    ent[b].respawn=0.5f;               /* board too busy: retry shortly */
}

static void new_link_game(void){
    g_link=1; mode=M_LINK;
    me = (lk_my_nonce>lk_peer_nonce)?0:1; you=1-me;        /* higher nonce = id 0 (authority) */
    memset(owner,0,NCELL); memset(trail,0,NCELL); memset(fresh,0,NCELL);
    for(int p=0;p<NP;p++){ Ent*e=&ent[p]; int cr=e->creature; memset(e,0,sizeof *e); e->creature=cr; e->alive=0; }
    ent[me].creature=sel_creature;                        /* you pick yours; peer gets a distinct one */
    ent[you].creature=(sel_creature==0)?1:0;
    for(int id=0;id<2;id++){ Ent*e=&ent[id]; int cx=LK_SX[id],cy=LK_SY[id];
        e->cx=e->px=cx; e->cy=e->py=cy; e->fx=cx+0.5f; e->fy=cy+0.5f;
        int d=(id==0)?0:2; e->dx=DIRS[d][0]; e->dy=DIRS[d][1];   /* face into the board */
        e->alive=1; e->home_x=cx; e->home_y=cy; stamp_home(id,cx,cy,3); }
    /* AI bots (ids 2..5): FIXED corners/dirs in the shared coordinate system —
     * both units stamp the same bases; only the authority ever *thinks* for them */
    lk_bots=1;
#ifdef MOTE_HOST
    if(getenv("MOTE_PAPER_NOBOTS")) lk_bots=0;             /* test rig: clean 1v1 board */
    lk_botsteps=0;
#endif
    if(lk_bots){
        int usedc[NCREA]={0}; usedc[ent[me].creature]=1; usedc[ent[you].creature]=1;
        for(int b=0;b<NBOT;b++){ int id=2+b; Ent*e=&ent[id]; int cx=LK_BX[b],cy=LK_BY[b];
            e->cx=e->px=cx; e->cy=e->py=cy; e->fx=cx+0.5f; e->fy=cy+0.5f;
            e->dx=DIRS[LK_BD[b]][0]; e->dy=DIRS[LK_BD[b]][1];
            e->ai=1; e->alive=1; e->persona=(uint8_t)(id%3);
            e->home_x=cx; e->home_y=cy; e->target_len=12+irand(24);   /* authority-only field */
            int c=0; while(usedc[c])c++; usedc[c]=1; e->creature=c;   /* local cosmetics */
            stamp_home(id,cx,cy,2); }
    }
    apply_palette(); recount();
    npart=0; combo=0; combo_t=0; score=0; milestone=0; msg_t=0; danger=0; elapsed=0;
    my_kos=my_deaths=peer_kos=0; lk_respawn_t=0; lk_state_t=0; lk_rx_age=0; lk_lost=0; lk_ready=0;
    cam_fx=ent[me].fx; cam_fy=ent[me].fy; cellpx=6.0f; step_acc=claim_flash=shake=0; state=S_PLAY;
}

static void lk_handle(const uint8_t*m){
    switch(m[1]){
        case 'H':
            lk_got_hello=1; lk_peer_nonce=(uint16_t)(m[3]|(m[4]<<8));
            /* START THE MATCH *INSIDE* THE PARSER.  The old flow started it after
             * lk_poll() returned — so any 'S' steps that arrived in the SAME rx
             * chunk as the hello (routine over a relayed/CDC pipe, which delivers
             * coalesced bursts) were still gated on g_link==0 and SILENTLY
             * DROPPED, leaving the remote head permanently offset: replayed
             * crossings then landed on the wrong cells and trail cuts missed.
             * Starting here means every byte after the hello lands in-game. */
            if(state==S_LINKWAIT){
                if(lk_peer_nonce==lk_my_nonce){ lk_new_nonce(); lk_got_hello=0; lk_send_hello(); }   /* tie: re-draw */
                else { lk_send_hello(); new_link_game();
                       PDBG("[P%d] START me=%d you=%d auth=%d",me,me,you,lk_auth()); }
            }
            /* a hello while WE are already playing = the peer hasn't started yet
             * (our start-hello can be eaten by the tail of its lobby handshake).
             * Re-answer until its first in-game packet proves it's in. */
            else if(g_link && state==S_PLAY && !lk_ready) lk_send_hello();
            break;
        case 'S': if(g_link&&state==S_PLAY){ int d=m[2]&3; ent[you].dx=DIRS[d][0]; ent[you].dy=DIRS[d][1];
                      if(lk_move(you,0)) PDBG("[P%d] RSTEP you=%d (%d,%d)",me,you,ent[you].cx,ent[you].cy);
                      else PDBG("[P%d] RSTEP-HELD you=%d dir=%d at (%d,%d)",me,you,d,ent[you].cx,ent[you].cy); }
                  else PDBG("[P%d] RSTEP-DROP glink=%d state=%d",me,g_link,state);
                  lk_rx_age=0; break;
        case 'K': PDBG("[P%d] K-RX victim=%d killer=%d",me,(int8_t)m[2],(int8_t)m[3]);
                  if(g_link) lk_apply_kill((int8_t)m[2],(int8_t)m[3]); lk_rx_age=0; break;
        case 'R': if(g_link) lk_respawn_at(you,m[2],m[3],m[4]&3); lk_rx_age=0; break;
        case 'P': if(g_link){ peer_kos=m[3]; } lk_rx_age=0; break;
        case 'Q': if(g_link&&state==S_PLAY){ state=S_WIN; say("OPPONENT LEFT",2.0f); } break;
        case 'A': if(g_link&&state==S_PLAY&&!lk_auth()){ int b=m[2]; int d=m[3]&3;   /* a bot step from the authority */
                      if(b>=2&&b<NP){ ent[b].dx=DIRS[d][0]; ent[b].dy=DIRS[d][1];
                          if(lk_move(b,0)){
#ifdef MOTE_HOST
                              pdbg_bot_clock();
#endif
                          } } }
                  lk_rx_age=0; break;
        case 'E': if(g_link&&state==S_PLAY&&!lk_auth()){ int b=m[2];                 /* bot respawn (absolute) */
                      if(b>=2&&b<NP) lk_respawn_at(b,m[3],m[4],m[5]&3); }
                  lk_rx_age=0; break;
        case 'C': if(g_link&&state==S_PLAY){ int cx=m[2],cy=m[3];                    /* peer: "my head crossed a trail HERE" */
                      if(in_b(cx,cy)){ uint8_t t2=trail[IDX(cx,cy)];
                          PDBG("[P%d] C-RX cell=(%d,%d) trail=%d",me,cx,cy,t2);
                          if(t2==(uint8_t)(me+1)) lk_local_death(you,3);             /* my FRESH trail confirms: I die */
                          else if(t2>=3 && lk_auth()) lk_bot_die(t2-1, you);         /* bot trail: authority verdict */
                          /* t2==0: banked/cleared before the report — cut is void by design */
                      } }
                  lk_rx_age=0; break;
    }
}
static void lk_poll(void){
    uint8_t chunk[64]; int n;
    while((n=mote->link_recv(chunk,(int)sizeof chunk))>0){
        for(int i=0;i<n;i++){ uint8_t b=chunk[i];
            if(lk_msg_len==0){ if(b==LK_MAGIC) lk_msg[lk_msg_len++]=b; continue; }
            lk_msg[lk_msg_len++]=b; int t=lk_msg[1];
            int want = t=='H'?5 : t=='S'?3 : t=='K'?5 : t=='R'?5 : t=='P'?4 : t=='Q'?2
                     : t=='A'?4 : t=='E'?6 : t=='C'?4 : -1;
            if(want<0){ lk_msg_len=0; continue; }      /* bad type -> drop, resync on next magic */
            if(lk_msg_len<want) continue;
            lk_msg_len=0;
            if(g_link && t!='H' && t!='Q' && !lk_ready){ lk_ready=1;   /* first in-game packet: peer is in */
                PDBG("[P%d] READY (first in-game packet '%c')",me,t); }
            lk_handle(lk_msg);
        }
    }
}
static void lk_start_wait(void){
    /* engine lobby: transport pick + connect + authority (2 beats 1) */
    int host=0;
    MoteNetCfg cfg={"PaperMote",LK_PROTO,MOTE_NET_ALL};
    if (mote->net_lobby(&cfg,&host)!=MOTE_NET_CONNECTED){ state=S_TITLE; return; }
    g_link=0; lk_my_nonce=(uint16_t)(host?2:1);
#ifdef MOTE_HOST
    /* test rig: pin the authority so scripted 2-instance runs are role-stable */
    { const char*nv=getenv("MOTE_PAPER_NONCE"); if(nv) lk_my_nonce=(uint16_t)atoi(nv); }
#endif
    lk_sent_hello=lk_got_hello=0; lk_msg_len=0; lk_hello_t=0; lk_lost=0;
    state=S_LINKWAIT;
}
static void lk_teardown_to_title(void){
    lk_send_bye(); mote->link_stop(); g_link=0; state=S_TITLE;
}

/* ============================================================ one game step */
static void do_step(void){
    for(int p=0;p<NP;p++){
        Ent*e=&ent[p]; if(!e->alive) continue;
        if(p==0){ if((e->pend_dx||e->pend_dy)&&!(e->pend_dx==-e->dx&&e->pend_dy==-e->dy)){ e->dx=e->pend_dx; e->dy=e->pend_dy; } }
        else { int d=ai_pick(p); if(d>=0){ e->dx=DIRS[d][0]; e->dy=DIRS[d][1]; } }
        int nx=e->cx+e->dx, ny=e->cy+e->dy;
        if(!in_b(nx,ny)){                                   /* wall: deflect along it, don't die */
            int ax=e->dx?0:1, ay=e->dx?1:0, r=irand(2)?1:-1, turned=0;
            for(int s=0;s<2;s++){ int ndx=ax*r, ndy=ay*r, tx=e->cx+ndx, ty=e->cy+ndy;
                if(in_b(tx,ty)&&trail[IDX(tx,ty)]!=p+1){ e->dx=ndx; e->dy=ndy; nx=tx; ny=ty; turned=1; break; } r=-r; }
            if(!turned) continue;                           /* cornered: hold this step */
        }
        int n=IDX(nx,ny); uint8_t tr=trail[n];
        if(tr){ if(tr==p+1){ if(p==0){ kill_ent(p,-1); continue; } else continue; }  /* player dies on own trail; AI never self-kills (holds) */
            else { int victim=tr-1; kill_ent(victim,p); on_kill(p); recount();
                   mote->audio_play(&snd_cut, p==0?0.6f:0.3f); if(p==0) shake=0.5f; } }
        if(!e->alive) continue;
        /* lay trail on the cell we're LEAVING (now fully covered) so it appears
         * BEHIND the head as it slides on, not in front of it (no flicker) */
        if(e->on_trail && owner[IDX(e->cx,e->cy)]!=p+1) trail[IDX(e->cx,e->cy)]=p+1;
        e->px=e->cx; e->py=e->cy; e->cx=nx; e->cy=ny;
        if(owner[n]==p+1){
            if(e->on_trail){ int g=do_claim(p); recount();
                for(int q=0;q<NP;q++) if(q!=p&&ent[q].alive&&ent[q].area==0){ kill_ent(q,p); on_kill(p); }
                if(p==0){ claim_flash=1.0f; score+=g;
                          mote->audio_play(g>140?&snd_claimbig:&snd_claim, 0.55f); }
                else mote->audio_play(&snd_claim,0.16f); }
            e->on_trail=0;
        } else { e->on_trail=1; e->trail_len++; }
    }
    for(int p=1;p<NP;p++) if(!ent[p].alive&&ent[p].respawn>=0){ ent[p].respawn-=STEP_BASE; if(ent[p].respawn<=0) respawn_ai(p); }

    if(state!=S_PLAY) return;
    int mine=ent[0].area*100/NCELL;                                     /* milestone popups */
    int ms = mine>=50?50:mine>=25?25:mine>=10?10:0;
    if(ms>milestone){ milestone=ms; char b[28]; snprintf(b,sizeof b,"%d%% OF THE BOARD!",ms); say(b,1.5f); }
    if(mode==M_ENDLESS){ if(mine>=WIN_PCT) state=S_WIN; }
    else if(mode==M_LAST){ int al=0; for(int p=1;p<NP;p++) if(ent[p].alive)al++; if(al==0) state=S_WIN; }
}

/* ============================================================ lifecycle */
static void apply_palette(void){          /* each entity's colours come from its creature */
    for(int p=0;p<NP;p++){ int c=ent[p].creature; if(c<0||c>=NCREA)c=0;
        pal[p].terr =MOTE_RGB565((int)(CREA[c].r*0.55f),(int)(CREA[c].g*0.55f),(int)(CREA[c].b*0.55f));
        pal[p].trail=MOTE_RGB565(CREA[c].r,CREA[c].g,CREA[c].b);
        pal[p].head =MOTE_RGB565(255,255,255);
        pal[p].r=CREA[c].r; pal[p].g=CREA[c].g; pal[p].b=CREA[c].b; }
}
static void new_game(void){
    mode=sel_mode; g_link=0; me=0; you=1;    /* solo: local player is always ent[0] */
    memset(owner,0,NCELL); memset(trail,0,NCELL); memset(fresh,0,NCELL);
    int sx[NP]={16,82,16,82,49,49}, sy[NP]={16,16,82,82,12,86};
    /* you get your chosen creature; the AIs take distinct others */
    int usedc[NCREA]={0}; ent[0].creature=sel_creature; usedc[sel_creature]=1;
    for(int p=1;p<NP;p++){ int c; do{ c=irand(NCREA); }while(usedc[c]); usedc[c]=1; ent[p].creature=c; }
    for(int p=0;p<NP;p++){ Ent*e=&ent[p]; int cr=e->creature; memset(e,0,sizeof *e); e->creature=cr;
        e->cx=e->px=sx[p]; e->cy=e->py=sy[p]; e->fx=sx[p]+0.5f; e->fy=sy[p]+0.5f;
        e->ai=(p!=0); e->alive=1; e->persona=(uint8_t)(p%3);
        int d=irand(4); e->dx=DIRS[d][0]; e->dy=DIRS[d][1];
        e->home_x=sx[p]; e->home_y=sy[p]; e->target_len=12+irand(24);
        stamp_home(p,sx[p],sy[p],3); }
    apply_palette();
    recount(); npart=0; combo=0; combo_t=0; score=0; milestone=0; msg_t=0; danger=0; elapsed=0;
    game_t=TIMED_SECS; bgm_t=0; bgm_step=0;
    cam_fx=ent[0].fx; cam_fy=ent[0].fy; cellpx=6.0f; step_acc=claim_flash=shake=0; state=S_PLAY;
}

static void g_init(void){
    owner=mote->alloc(NCELL); trail=mote->alloc(NCELL); vis=mote->alloc(NCELL);
    fresh=mote->alloc(NCELL); stk=mote->alloc(NCELL*sizeof(int));
    rng ^= (uint32_t)mote->micros()|1u; srand(rng);
    mote->scene_set_background(MOTE_RGB565(18,20,28));   /* unused by render_band, harmless */
    snd_claim=mote_sfx_bake(mote,&claim_sfx); snd_claimbig=mote_sfx_bake(mote,&claimbig_sfx);
    snd_combo=mote_sfx_bake(mote,&combo_sfx); snd_cut=mote_sfx_bake(mote,&cut_sfx);
    snd_die=mote_sfx_bake(mote,&die_sfx); snd_spawn=mote_sfx_bake(mote,&spawn_sfx);
    int b=0; if(mote->load(0,&b,sizeof b)==(int)sizeof b && b>=0) best_score=b;
    new_game(); state=S_TITLE;
}

/* procedural bgm bed: a bass arp that adds a lead + tempo as you dominate */
static void bgm_tick(float dt){
    float dom = ent[me].area/(float)(NCELL*0.5f); if(dom>1)dom=1;         /* 0..1 toward 50% */
    float beat = 0.26f - 0.07f*dom;                                       /* speeds up */
    bgm_t += dt;
    if(bgm_t>=beat){ bgm_t-=beat; int s=(bgm_step++)&7;
        static const int SC[8]={0,3,5,7,5,3,0,-2};
        float root=98.0f, amp=0.07f+0.10f*dom;
        mote->audio_note(root*powf(2.0f,SC[s]/12.0f), amp);
        if(dom>0.30f && (s&1)==0) mote->audio_note(root*2.0f*powf(2.0f,SC[(s+4)&7]/12.0f), amp*0.55f); }
}

static void g_update(float dt){
    const MoteInput*in=mote->input();
#ifdef MOTE_HOST
    /* test rig: cap the host frame rate so MOTE_KEYS frame windows map to wall time */
    { static int fpsdone; if(!fpsdone){ fpsdone=1; const char*f=getenv("MOTE_PAPER_FPS"); if(f) mote->set_fps_limit(atoi(f)); } }
#endif
    if(claim_flash>0)claim_flash-=dt*2.0f; if(shake>0)shake-=dt*3.0f;
    if(combo_t>0){ combo_t-=dt; if(combo_t<=0)combo=0; } if(msg_t>0)msg_t-=dt;
    /* fade claim-flash freshness */
    int df=(int)(dt*520.0f); if(df<1)df=1; for(int i=0;i<NCELL;i++){ int f=fresh[i]; if(f){ f-=df; fresh[i]=f<0?0:f; } }
    /* particles */
    for(int i=0;i<npart;){ Part*q=&part[i]; q->life-=dt; if(q->life<=0){ *q=part[--npart]; continue; }
        q->x+=q->vx*dt; q->y+=q->vy*dt; q->vx*=0.92f; q->vy*=0.92f; i++; }


    if(state==S_TITLE){
        int nmode = mote->abi_version>=44 ? 4 : 3;           /* 2P LINK needs net_lobby (v44) */
        if(mote_just_pressed(in,MOTE_BTN_UP))   menu_row=(menu_row+2)%3;
        if(mote_just_pressed(in,MOTE_BTN_DOWN)) menu_row=(menu_row+1)%3;
        int dl=mote_just_pressed(in,MOTE_BTN_LEFT)?-1:mote_just_pressed(in,MOTE_BTN_RIGHT)?1:0;
        if(dl){ if(menu_row==0)sel_creature=(sel_creature+NCREA+dl)%NCREA;
                else if(menu_row==1)sel_name=(sel_name+8+dl)%8; else sel_mode=(sel_mode+nmode+dl)%nmode; }
        if(mote_just_pressed(in,MOTE_BTN_A)){ if(sel_mode==M_LINK) lk_start_wait(); else new_game(); }
        return;
    }
    if(state==S_LINKWAIT){                                   /* handshake: exchange hellos, then start */
        lk_hello_t-=dt;
        if(mote->link_status()!=MOTE_LINK_CONNECTED){ lk_sent_hello=lk_got_hello=0; lk_msg_len=0; }
        else {
            if(!lk_sent_hello||lk_hello_t<=0){ lk_send_hello(); lk_sent_hello=1; lk_hello_t=0.5f; }
#ifdef MOTE_HOST
            /* test rig: model a bursty pipe by holding the first poll back — the
             * peer's hello AND its first steps then arrive as ONE chunk */
            { static float ss=-1.0f; if(ss<0){ const char*e=getenv("MOTE_PAPER_SLOWSTART"); ss=e?atoi(e)*0.001f:0.0001f;
                  if(ss>0.001f) PDBG("[P?] SLOWSTART armed %.0fms",ss*1000.0f); }
              if(ss>0){ ss-=dt;
                  if(ss<=0) PDBG("[P?] SLOWSTART over — polling the burst now");
                  else { if(mote_just_pressed(in,MOTE_BTN_B)){ mote->link_stop(); state=S_TITLE; } return; } } }
#endif
            lk_poll();       /* a peer hello starts the match INSIDE the parser (see lk_handle 'H') */
        }
        if(state==S_LINKWAIT && mote_just_pressed(in,MOTE_BTN_B)){ mote->link_stop(); state=S_TITLE; }
        return;
    }
    if(state==S_DEAD||state==S_WIN){
        if(g_link){ if(mote_just_pressed(in,MOTE_BTN_B)) lk_teardown_to_title(); return; }
        if(score>best_score){best_score=score;int b=best_score;mote->save(0,&b,sizeof b);}
        if(mote_just_pressed(in,MOTE_BTN_A)) new_game();
        else if(mote_just_pressed(in,MOTE_BTN_B)) state=S_TITLE; return; }

    /* ---- play ---- */
    Ent*e=&ent[me];
    if(mote_just_pressed(in,MOTE_BTN_RIGHT)){e->pend_dx=1;e->pend_dy=0;}
    else if(mote_just_pressed(in,MOTE_BTN_LEFT)){e->pend_dx=-1;e->pend_dy=0;}
    else if(mote_just_pressed(in,MOTE_BTN_DOWN)){e->pend_dx=0;e->pend_dy=1;}
    else if(mote_just_pressed(in,MOTE_BTN_UP)){e->pend_dx=0;e->pend_dy=-1;}

    float step;
    if(g_link){                                             /* ---- linked duel ---- */
        bgm_tick(dt);
        lk_poll();
        if(mote->link_status()!=MOTE_LINK_CONNECTED || mote->net_health()==MOTE_NET_LOST){ lk_lost=1; state=S_DEAD; }   /* v45 */
        lk_rx_age+=dt;
        step=STEP_BASE;                                     /* constant speed — a fair duel */
        step_acc+=dt;
        if(!lk_ready) step_acc=0;                           /* nobody moves until the peer is in-game */
        while(step_acc>=step){ step_acc-=step;
            lk_local_step();
            /* the AUTHORITY thinks + moves the bots on the same step clock and
             * streams each step ('A'); the peer only ever replays them */
            if(lk_bots && lk_auth() && state==S_PLAY) for(int b=2;b<NP;b++) lk_bot_step(b);
            if(state!=S_PLAY)break; lk_poll(); }
        if(lk_respawn_t>0 && state==S_PLAY){ lk_respawn_t-=dt; if(lk_respawn_t<=0) lk_respawn_me(); }
        if(lk_bots && lk_auth() && state==S_PLAY)           /* authority: revive dead bots */
            for(int b=2;b<NP;b++) if(!ent[b].alive && ent[b].respawn>0){
                ent[b].respawn-=dt; if(ent[b].respawn<=0) lk_bot_respawn(b); }
        lk_state_t+=dt; if(lk_state_t>=1.0f/15.0f){ lk_state_t-=1.0f/15.0f; lk_send_state(); }
        if(my_kos>=KO_TARGET) state=S_WIN; else if(my_deaths>=KO_TARGET && state==S_PLAY) state=S_DEAD;
#ifdef MOTE_HOST
        if(pdbg_on()){ static float t; t+=dt; if(t>=1.0f){ t-=1.0f;
            PDBG("[P%d] AREA a=%d,%d,%d,%d,%d,%d kos=%d deaths=%d own=%08x trail=%08x",
                 me,ent[0].area,ent[1].area,ent[2].area,ent[3].area,ent[4].area,ent[5].area,
                 my_kos,my_deaths,pdbg_fnv(owner,NCELL),pdbg_fnv(trail,NCELL)); } }
#endif
    } else {                                                /* ---- solo (endless/timed/last) ---- */
        elapsed+=dt; bgm_tick(dt);
        if(mode==M_TIMED){ game_t-=dt; if(game_t<=0){ game_t=0; int rank=1; for(int p=1;p<NP;p++) if(ent[p].area>ent[0].area)rank++;
            state = rank==1 ? S_WIN : S_DEAD; } }
        step = STEP_BASE / (1.0f + (elapsed/180.0f)*0.5f);  /* difficulty: speeds up */
        if(step<0.045f)step=0.045f;
        step_acc+=dt;
        while(step_acc>=step){ step_acc-=step; do_step(); if(state!=S_PLAY)break; }
    }

    /* danger vignette: rises with exposed trail length + a nearby rival */
    float want = e->on_trail ? (e->trail_len*0.012f) : 0.0f;
    for(int p=0;p<NP;p++) if(p!=me && ent[p].alive && abs((int)ent[p].fx-e->cx)+abs((int)ent[p].fy-e->cy)<6 && e->on_trail) want+=0.4f;
    if(want>1)want=1; danger += (want-danger)*mote_clampf(dt*4.0f,0,1);

    float f=step_acc/step; if(f>1)f=1;
    for(int p=0;p<NP;p++){ Ent*q=&ent[p]; q->fx=(q->px+0.5f)+(q->cx-q->px)*f; q->fy=(q->py+0.5f)+(q->cy-q->py)*f; }
    float tgtcell = 6.0f - 3.0f*mote_clampf(ent[me].area/(float)(NCELL*0.40f),0,1);
    cellpx += (tgtcell-cellpx)*mote_clampf(dt*2.5f,0,1);
    cam_fx += (ent[me].fx-cam_fx)*mote_clampf(dt*6.0f,0,1);
    cam_fy += (ent[me].fy-cam_fy)*mote_clampf(dt*6.0f,0,1);
}

/* ============================================================ render_band */
static inline uint16_t lerp565(uint16_t a,uint16_t b,float t){
    int ar=(a>>11)&31,ag=(a>>5)&63,ab=a&31, br=(b>>11)&31,bg=(b>>5)&63,bb=b&31;
    int r=ar+(int)((br-ar)*t),g=ag+(int)((bg-ag)*t),bl=ab+(int)((bb-ab)*t);
    return (uint16_t)((r<<11)|(g<<5)|bl); }
static inline uint16_t shd(uint16_t c,int n){ int r=((c>>11)&31),g=((c>>5)&63),b=(c&31);
    r-=r*n/8;g-=g*n/8;b-=b*n/8; return (uint16_t)((r<<11)|(g<<5)|b); }
static inline uint16_t lit(uint16_t c,int n){ int r=((c>>11)&31)+n,g=((c>>5)&63)+n*2,b=(c&31)+n;
    if(r>31)r=31;if(g>63)g=63;if(b>31)b=31; return (uint16_t)((r<<11)|(g<<5)|b); }

/* draw a creature sprite centred at (cx,cy), scaled to sz px, colour-keyed, band-clipped */
static void draw_creature(uint16_t*fb,int cx,int cy,int sz,const MoteImage*im,int y0,int y1){
    if(!im||sz<2)return; int x0=cx-sz/2, ytop=cy-sz/2, iw=im->w, ih=im->h; uint16_t key=im->key;
    for(int dy=0;dy<sz;dy++){ int yy=ytop+dy; if(yy<y0||yy>=y1||yy<0||yy>=128)continue;
        const uint16_t*srow=im->pixels+(dy*ih/sz)*iw;
        for(int dx=0;dx<sz;dx++){ int xx=x0+dx; if(xx<0||xx>=128)continue;
            uint16_t c=srow[dx*iw/sz]; if(c!=key) fb[yy*128+xx]=c; } } }

static void g_band(uint16_t*fb,int y0,int y1){
    if(state==S_TITLE||state==S_LINKWAIT){               /* clean backdrop behind the title / link-wait menu */
        for(int sy=y0;sy<y1;sy++){ uint16_t*row=fb+sy*128;
            for(int sx=0;sx<128;sx++) row[sx]=(((sx>>3)^(sy>>3))&1)?MOTE_RGB565(22,24,34):MOTE_RGB565(18,20,30); }
        return; }
    float cpx=cellpx, inv=1.0f/cpx;
    float ox=(shake>0)?(irand(3)-1)*shake*1.5f:0, oy=(shake>0)?(irand(3)-1)*shake*1.5f:0;
    float wx0=cam_fx+ox-64.0f*inv, wy0=cam_fy+oy-64.0f*inv;
    for(int sy=y0;sy<y1;sy++){
        float wy=wy0+sy*inv; int cy=(int)floorf(wy); float fyp=wy-cy;
        uint16_t*row=fb+sy*128;
        for(int sx=0;sx<128;sx++){
            float wx=wx0+sx*inv; int cx=(int)floorf(wx); uint16_t col;
            if(!in_b(cx,cy)) col=MOTE_RGB565(12,12,16);
            else{ int c=IDX(cx,cy); uint8_t o=owner[c],t=trail[c];
                if(t) col=pal[t-1].trail;
                else if(o){ col=pal[o-1].terr; uint8_t fr=fresh[c]; if(fr) col=lerp565(col,0xFFFF,fr*0.78f/255.0f);
                    if(cpx>=4.0f){ float fxp=wx-cx;                                  /* bevel: top/left lit, bottom/right shaded */
                        int top=(cy<=0||owner[IDX(cx,cy-1)]!=o), lft=(cx<=0||owner[IDX(cx-1,cy)]!=o);
                        int bot=(cy>=MH-1||owner[IDX(cx,cy+1)]!=o), rgt=(cx>=MW-1||owner[IDX(cx+1,cy)]!=o);
                        if((fyp<0.14f&&top)||(fxp<0.14f&&lft)) col=lit(col,4);
                        else if((fyp>0.86f&&bot)||(fxp>0.86f&&rgt)) col=shd(col,3); } }
                else col=((cx^cy)&1)?MOTE_RGB565(30,32,42):MOTE_RGB565(26,28,38); }
            row[sx]=col;
        }
    }
    /* particles (world -> screen), drawn over the grid */
    for(int i=0;i<npart;i++){ Part*q=&part[i]; int psx=(int)((q->x-(cam_fx+ox))*cpx+64.0f), psy=(int)((q->y-(cam_fy+oy))*cpx+64.0f);
        if(psy<y0||psy>=y1||psx<0||psx>=128)continue; uint16_t c=lerp565(MOTE_RGB565(20,20,28),q->col,q->life/q->max);
        fb[psy*128+psx]=c; if(psx+1<128)fb[psy*128+psx+1]=c; }
    /* heads */
    for(int p=0;p<NP;p++){ Ent*e=&ent[p]; if(!e->alive)continue;
        float hsx=(e->fx-(cam_fx+ox))*cpx+64.0f, hsy=(e->fy-(cam_fy+oy))*cpx+64.0f;
        int sz=(int)(cpx*2.2f); if(sz<13)sz=13; if(sz>20)sz=20;          /* always readable */
        /* rotate the sprite to face travel (sprites are drawn facing up) */
        float ang=atan2f((float)e->dx,(float)-e->dy);
        mote->blit_ex(fb, CREA[e->creature].img, hsx, hsy, 0,0,0,0, ang, sz/16.0f, MOTE_BLEND_NONE, y0, y1); }
}

/* ============================================================ HUD overlay */
static void vignette(uint16_t*fb,int t,int rr,int gg,int bb){ uint16_t c=MOTE_RGB565(rr,gg,bb);
    mote->draw_rect(fb,0,0,128,t,c,1,0,128); mote->draw_rect(fb,0,128-t,128,t,c,1,0,128);
    mote->draw_rect(fb,0,0,t,128,c,1,0,128); mote->draw_rect(fb,128-t,0,t,128,c,1,0,128); }

static void g_overlay(uint16_t*fb){
    const uint16_t wht=MOTE_RGB565(240,244,250), dim=MOTE_RGB565(150,170,210);
    if(state==S_TITLE){
        mote_textf(mote,fb,30,10,pal[0].trail,"PAPERMOTE");
        mote->text(fb,"CLAIM THE BOARD",26,22,dim);
        struct{const char*k;const char*v;} rows[3]={
            {"CREATURE",CREA[sel_creature].name},{"NAME",NAME_L[sel_name]},{"MODE",MODE_L[sel_mode]} };
        int yy=40; for(int i=0;i<3;i++){ uint16_t fg = i==menu_row?MOTE_RGB565(255,230,120):wht;
            if(i==menu_row){ mote->draw_rect(fb,8,yy-2,112,13,MOTE_RGB565(30,36,54),1,0,128);
                mote->text(fb,"<",10,yy,fg); mote->text(fb,">",116,yy,fg); }
            mote->text(fb,rows[i].k,16,yy,fg); mote->text(fb,rows[i].v,72,yy,fg); yy+=15; }
        /* preview your chosen creature, big, in its theme colour */
        mote->draw_rect(fb,52,90,24,24,MOTE_RGB565((int)(CREA[sel_creature].r*0.4f),(int)(CREA[sel_creature].g*0.4f),(int)(CREA[sel_creature].b*0.4f)),1,0,128);
        draw_creature(fb,64,102,22,CREA[sel_creature].img,0,128);
        mote->text(fb,"D-PAD SET  A START",18,120,dim);
        mote_textf(mote,fb,40,2,dim,"BEST %d",best_score);
        return;
    }
    if(state==S_LINKWAIT){
        mote_textf(mote,fb,36,14,pal[0].trail,"2P LINK");
        int conn=mote->link_status()==MOTE_LINK_CONNECTED;
        draw_creature(fb,64,58,26,CREA[sel_creature].img,0,128);
        mote->text(fb, conn?"HANDSHAKE...":"SEARCHING...", 26,84, conn?MOTE_RGB565(255,230,120):dim);
        mote->text(fb,"LINK A 2ND UNIT",22,98,dim);
        mote->text(fb,"B  CANCEL",40,116,dim);
        return;
    }
    if(claim_flash>0){ int a=(int)(claim_flash*26); if(a>18)a=18;
        for(int i=0;i<128*128;i++){ uint16_t c=fb[i]; int r=((c>>11)&31)+a/4,g=((c>>5)&63)+a/3,b=(c&31)+a/4;
            if(r>31)r=31;if(g>63)g=63;if(b>31)b=31; fb[i]=(uint16_t)((r<<11)|(g<<5)|b);} }
    if(danger>0.05f){ int t=(int)(danger*9.0f); if(t>9)t=9; vignette(fb,t,200,40,20); }   /* danger glow */

    int mine=ent[me].area*100/NCELL;
    mote->draw_rect(fb,0,0,128,9,MOTE_RGB565(14,16,24),1,0,128);
    mote_textf(mote,fb,2,1,pal[me].trail,"%d%%",mine);
    if(g_link){                                          /* opp KOs == my deaths (always local-accurate) */
        mote_textf(mote,fb,34,1,pal[me].trail,"YOU %d",my_kos);
        mote_textf(mote,fb,84,1,pal[you].trail,"OPP %d",my_deaths);
    } else {
        int rank=1; for(int p=1;p<NP;p++) if(ent[p].area>ent[0].area)rank++;
        mote_textf(mote,fb,34,1,wht,"R%d/%d",rank,NP);
        mote_textf(mote,fb,64,1,MOTE_RGB565(245,205,70),"%d",score);
        if(mode==M_TIMED) mote_textf(mote,fb,104,1,wht,"%d",(int)(game_t+0.99f));
        else mote_textf(mote,fb,104,1,dim,"K%d",ent[0].kills);
    }
    if(combo>=2&&combo_t>0) mote_textf(mote,fb,46,11,MOTE_RGB565(255,210,90),"x%d",combo);

    /* minimap (whole board) */
    int MM=40,mx=128-MM-3,my=128-MM-3;
    mote->draw_rect(fb,mx-2,my-2,MM+4,MM+4,MOTE_RGB565(10,12,18),1,0,128);
    for(int j=0;j<MM;j++) for(int i=0;i<MM;i++){ int wx=i*MW/MM,wy=j*MH/MM,c2=IDX(wx,wy);
        int o=owner[c2],t=trail[c2]; mote->draw_pixel(fb,mx+i,my+j,t?pal[t-1].trail:o?pal[o-1].terr:MOTE_RGB565(28,30,40)); }
    for(int p=0;p<NP;p++){ if(!ent[p].alive)continue; int dxp=mx+(int)(ent[p].fx*MM/MW),dyp=my+(int)(ent[p].fy*MM/MW);
        if(dxp>=mx&&dxp<mx+MM&&dyp>=my&&dyp<my+MM) mote->draw_pixel(fb,dxp,dyp,pal[p].head); }
    mote->draw_rect(fb,mx-1,my-1,MM+2,MM+2,MOTE_RGB565(70,80,110),0,0,128);

    if(msg_t>0) mote_textf(mote,fb,18,116,MOTE_RGB565(255,225,90),"%s",msg);

    if(state==S_DEAD||state==S_WIN){ int win=state==S_WIN;
        mote->draw_rect(fb,14,50,100,30,win?MOTE_RGB565(18,34,20):MOTE_RGB565(36,16,16),1,0,128);
        if(g_link){
            const char*hdr = lk_lost?"LINK LOST":win?"YOU WIN!":"DEFEATED";
            mote_textf(mote,fb,lk_lost?38:win?30:36,55,lk_lost?MOTE_RGB565(240,90,90):win?MOTE_RGB565(120,240,130):MOTE_RGB565(240,90,90),"%s",hdr);
            mote_textf(mote,fb,34,65,wht,"KO  %d - %d",my_kos,my_deaths);
            mote->text(fb,"B  MENU",42,74,dim);
        } else {
            int rank=1; for(int p=1;p<NP;p++) if(ent[p].area>ent[0].area)rank++;
            mote_textf(mote,fb,win?30:42,55,win?MOTE_RGB565(120,240,130):MOTE_RGB565(240,90,90),"%s",win?"YOU WIN!":"DEAD");
            mote_textf(mote,fb,22,65,wht,"%d%% R%d  SCORE %d",mine,rank,score);
            mote->text(fb,"A REPLAY  B MENU",24,74,dim);
        } }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=g_band, .overlay=g_overlay, .config={ 0 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
