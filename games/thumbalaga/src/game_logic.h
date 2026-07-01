/* Thumbalaga — gameplay logic, state machine and rendering.
 * Included at the bottom of game.c (after all data + helpers are declared). */
#ifndef THUMBALAGA_LOGIC_H
#define THUMBALAGA_LOGIC_H

static void add_score(int pts){ g.score+=pts; if(g.score>g.high_score)g.high_score=g.score; }

/* ── scoreboard persistence ───────────────────────────────────────────────── */
static void sb_load(void){
    int r=mote->kv_load("scores",&g.sb,sizeof g.sb);
    if(r<=0 || g.sb.n<0 || g.sb.n>5) g.sb.n=0;
    g.high_score = g.sb.n>0 ? g.sb.e[0].score : 0;
}
static void sb_save(void){ mote->kv_save("scores",&g.sb,sizeof g.sb); }
static int sb_is_high(void){
    if(g.score<=0) return 0;
    if(g.sb.n<5) return 1;
    return g.score > g.sb.e[4].score;
}
static void sb_insert(const char*name){
    HS ne; ne.score=g.score; ne.name[0]=name[0]; ne.name[1]=name[1]; ne.name[2]=name[2]; ne.name[3]=0;
    if(g.sb.n<5) g.sb.e[g.sb.n++]=ne; else g.sb.e[4]=ne;
    for(int i=0;i<g.sb.n;i++) for(int j=i+1;j<g.sb.n;j++)
        if(g.sb.e[j].score>g.sb.e[i].score){ HS t=g.sb.e[i]; g.sb.e[i]=g.sb.e[j]; g.sb.e[j]=t; }
    g.new_rank=-1;
    for(int i=0;i<g.sb.n;i++) if(g.sb.e[i].score==g.score && memcmp(g.sb.e[i].name,name,3)==0){ g.new_rank=i; break; }
    if(g.score>=g.high_score) g.high_score=g.score;
    sb_save();
}

/* ── player ───────────────────────────────────────────────────────────────── */
static void player_update(float dt,int left,int right){
    if(!g.p_alive) return;
    if(g.p_inv>0) g.p_inv-=dt;
    if(left)  g.px = fmaxf(-58.0f, g.px-PLAYER_SPEED*dt);
    if(right) g.px = fminf( 58.0f, g.px+PLAYER_SPEED*dt);
}
static void handle_fire(const MoteInput*in){
    if(mote_just_pressed(in,MOTE_BTN_A) && g.p_alive){
        int fired;
        if(g.dual){ fired=fire_player(g.px,g.py); fire_player(g.px+WING_OFF,g.py); }
        else fired=fire_player(g.px,g.py);
        if(fired){ g.shots_fired += g.dual?2:1; snd(&shoot_snd); }
    }
}
static void player_die(void){
    g.state=ST_DYING; g.state_timer=1.5f; g.p_alive=0;
    g.pexa=1; g.pext=0; g.pexx=g.px; g.pexy=g.py;
    snd(&player_die_snd); stop_beam();
}
static void check_extra_life(void){
    if(g.score>=g.next_extra_life){
        g.lives++; snd(&extra_life_snd);
        if(g.next_extra_life==EXTRA_LIFE_FIRST) g.next_extra_life=EXTRA_LIFE_FIRST+EXTRA_LIFE_INTERVAL;
        else g.next_extra_life += EXTRA_LIFE_INTERVAL;
    }
}

/* ── diving advance ───────────────────────────────────────────────────────── */
static int dive_step(int idx,float dt,float dive_speed){
    Enemy*e=&g.en[idx];
    e->dive_t += dt*dive_speed;
    int seg=e->dive_plen-1;
    if(e->dive_t>=1.0f){
        e->in_formation=1; e->dive_t=0; e->dive_path=NULL;
        float sx,sy; slot_pos(e->slot_col,e->slot_row,&sx,&sy); e->x=sx; e->y=sy;
        e->hflip=0; e->vflip=0;
        for(int i=0;i<e->nescorts;i++){ Enemy*esc=&g.en[e->escorts[i]];
            if(esc->alive){ esc->in_formation=1; esc->is_escort=0; esc->dive_path=NULL;
                float ex,ey; slot_pos(esc->slot_col,esc->slot_row,&ex,&ey); esc->x=ex; esc->y=ey; esc->hflip=0; esc->vflip=0; } }
        e->nescorts=0;
        return 1;
    }
    float ts=e->dive_t*seg; int si=(int)ts; float st=ts-si;
    if(si>=seg){ si=seg-1; st=1.0f; }
    const Pt*p=e->dive_path;
    float nx=e->dive_sx + p[si].x + (p[si+1].x-p[si].x)*st;
    float ny=e->dive_sy + p[si].y + (p[si+1].y-p[si].y)*st;
    if(nx<-60.0f)nx=-60.0f; else if(nx>60.0f)nx=60.0f;
    e->x=nx; e->y=ny; return 0;
}
static int maybe_trigger_dive(float dt,int beam_active){
    g.dive_timer-=dt;
    if(g.dive_timer>0) return -1;
    g.dive_timer=g.lv.dive_interval;
    int diving=0;
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(e->present&&e->alive&&!e->in_formation&&e->entry_done) diving++; }
    if(diving>=g.lv.max_divers) return -1;
    int cand[NSLOTS], nc=0;
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(e->present&&e->alive&&e->in_formation) cand[nc++]=i; }
    if(!nc) return -1;
    int idx=cand[randint(0,nc-1)];
    start_dive(idx,1,beam_active);
    return idx;
}
static void maybe_enemy_fire(int idx){
    if(rndf()<g.lv.fire_chance){
        int act=0; for(int i=0;i<MAXEB;i++) if(g.eba[i])act++;
        if(act<g.lv.max_eb){ Enemy*e=&g.en[idx]; fire_enemy(e->x,e->y); snd(&enemy_shot_snd); }
    }
}
/* first formation enemy hit by player bullet bi, or -1 */
static int pb_vs_formation(int bi){
    float bx=g.pbx[bi], by=g.pby[bi];
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(!e->present||!e->alive||!e->vis) continue;
        if(fabsf(bx-e->x)<EHALF+1 && fabsf(by-e->y)<EHALF+2) return i;
    }
    return -1;
}

/* ── tractor beam update (returns 1 if player caught this frame) ───────────── */
static int update_beam(float dt){
    if(g.beam_phase==BEAM_OFF) return 0;
    if(g.beam_boss<0 || !g.en[g.beam_boss].alive){ stop_beam(); return 0; }
    Enemy*b=&g.en[g.beam_boss];
    g.beam_timer+=dt;
    if(g.beam_phase==BEAM_EXPAND){
        g.beam_reveal=fminf(g.beam_timer/1.5f,1.0f);
        if(g.beam_reveal>=1.0f){ g.beam_phase=BEAM_ACTIVE; g.beam_timer=0; }
    } else if(g.beam_phase==BEAM_ACTIVE){
        g.beam_reveal=1.0f;
        if(g.beam_timer>=2.0f){ g.beam_phase=BEAM_RETRACT; g.beam_timer=0; }
        else if(g.p_alive && g.p_inv<=0){
            float bx=b->x, by=b->y, dx=fabsf(g.px-bx), dy=g.py-by, max_len=PLAYER_Y-by;
            int fan = max_len>0 ? 2+(int)((dy*10.0f)/max_len) : 12;
            if(dx<fan+4 && dy>0 && dy<max_len+5){
                g.beam_phase=BEAM_CAPTURE; g.beam_timer=0; g.beam_cap_y=g.py;
                g.cap_op=1.0f; g.cap_red=1; g.cap_x=b->x; g.cap_y=g.beam_cap_y;
                g.p_alive=0;
                return 1;
            }
        }
    } else if(g.beam_phase==BEAM_CAPTURE){
        float t=fminf(g.beam_timer/1.5f,1.0f);
        float target_y=b->y-12.0f;
        g.cap_y=g.beam_cap_y+(target_y-g.beam_cap_y)*t; g.cap_x=b->x; g.cap_rot=t*6.28f*2.0f;
        if(t>=1.0f){ g.cap_rot=0; capture_player_ship(g.beam_boss); g.beam_phase=BEAM_RETURN; g.beam_timer=0; g.beam_reveal=0; snd(&rescue_snd); }
    } else if(g.beam_phase==BEAM_RETRACT){
        g.beam_reveal=fmaxf(0.0f,1.0f-g.beam_timer/0.5f);
        if(g.beam_reveal<=0){ g.beam_phase=BEAM_RETURN; g.beam_timer=0; }
    } else if(g.beam_phase==BEAM_RETURN){
        b->y -= 80.0f*dt;
        if(g.capturing_boss==g.beam_boss){ g.cap_x=b->x; g.cap_y=b->y-12.0f; }
        if(b->y<-70.0f){
            float sx,sy; slot_pos(b->slot_col,b->slot_row,&sx,&sy); b->x=sx; b->y=sy; b->in_formation=1;
            if(g.capturing_boss==g.beam_boss){ g.cap_x=sx; g.cap_y=sy-12.0f; }
            stop_beam();
        }
    }
    return 0;
}

/* ── rescue + hostile fighter ─────────────────────────────────────────────── */
static void update_rescue(float dt){
    if(!g.rescue_active) return;
    g.rescue_timer+=dt; float t=fminf(g.rescue_timer/1.5f,1.0f);
    float tx=g.px+WING_OFF, ty=g.py;
    g.cap_x=g.rescue_sx+(tx-g.rescue_sx)*t; g.cap_y=g.rescue_sy+(ty-g.rescue_sy)*t; g.cap_op=1.0f;
    if(t>=1.0f){ g.rescue_active=0; g.cap_op=0; g.cap_x=-200; g.dual=1; }
}
static void update_hostile(float dt){
    if(!g.hf_active||!g.hf_alive) return;
    if(g.hf_path==NULL){
        g.hf_fire_timer-=dt; if(g.hf_fire_timer<=0) hf_start_dive();
        g.cap_x=g.hf_x; g.cap_y=g.hf_y; return;
    }
    g.hf_dive_t += dt*0.5f; int seg=g.hf_plen-1;
    if(g.hf_dive_t>=1.0f){ g.hf_active=0; g.hf_carry=1; g.cap_op=0; g.cap_x=-200; return; }
    float ts=g.hf_dive_t*seg; int si=(int)ts; float st=ts-si; if(si>=seg){si=seg-1;st=1.0f;}
    const Pt*p=g.hf_path;
    g.hf_x=g.hf_sx + p[si].x + (p[si+1].x-p[si].x)*st;
    g.hf_y=g.hf_sy + p[si].y + (p[si+1].y-p[si].y)*st;
    if(g.hf_x<-60.0f)g.hf_x=-60.0f; else if(g.hf_x>60.0f)g.hf_x=60.0f;
    g.cap_x=g.hf_x; g.cap_y=g.hf_y;
    g.hf_fire_timer-=dt; if(g.hf_fire_timer<=0){ g.hf_fire_timer=0.8f+rndf()*0.5f; fire_enemy(g.hf_x,g.hf_y); }
}
static void update_hostile_entry(float dt){
    if(g.hf_entry_boss<0) return;
    g.hf_entry_timer-=dt; if(g.hf_entry_timer>0) return;
    Enemy*boss=&g.en[g.hf_entry_boss];
    if(!boss->alive){ g.hf_entry_boss=-1; return; }
    float t=fminf((-g.hf_entry_timer)/1.5f,1.0f);
    float tx=boss->x, ty=boss->y-12.0f;
    g.cap_op=1.0f; g.cap_red=1; g.cap_x=tx*t; g.cap_y=-70.0f+(ty+70.0f)*t;
    if(t>=1.0f){ g.capturing_boss=g.hf_entry_boss; g.cap_x=tx; g.cap_y=ty; g.hf_entry_boss=-1; }
}

/* ── transforms ───────────────────────────────────────────────────────────── */
static const Pt* const TPATH[3]={TRANSFORM_PATH_LEFT,TRANSFORM_PATH_RIGHT,TRANSFORM_PATH_CENTER};
static const int   TPLEN[3]={TRANSFORM_PATH_LEFT_N,TRANSFORM_PATH_RIGHT_N,TRANSFORM_PATH_CENTER_N};
static int transforms_active(void){ int c=0; for(int i=0;i<MAXTG;i++) if(g.tg[i].active)c++; return c; }
static int do_morph(int bee_idx,int ttype){
    Enemy*bee=&g.en[bee_idx]; float sx=bee->x, sy=bee->y; enemy_kill(bee);
    int slot=-1; for(int i=0;i<MAXTG;i++) if(!g.tg[i].active){ slot=i; break; }
    if(slot<0) return 0;
    TGroup*t=&g.tg[slot]; memset(t,0,sizeof *t);
    int pi=randint(0,2); t->path=TPATH[pi]; t->plen=TPLEN[pi];
    t->sx=sx; t->sy=sy; t->t=0; t->active=1; t->kills=0; t->bonus=transform_bonus(g.stage);
    t->speed=0.45f; t->fire_timer=rndf()*0.5f+0.3f; t->etype=ttype;
    int ia,ib; idle_frames(ttype,&ia,&ib);
    for(int i=0;i<3;i++){ t->e[i].frame_y=ttype; t->e[i].frame_x=ia; t->e[i].alive=1;
        t->e[i].x=sx+(i-1)*TRANSFORM_SPREAD; t->e[i].y=sy; }
    return 1;
}
static int try_morph(float dt){
    int ttype=transform_type(g.stage);
    if(ttype<0) return 0;
    if(g.premorph>=0){
        Enemy*bee=&g.en[g.premorph];
        if(!bee->alive){ g.premorph=-1; return 0; }
        g.premorph_timer-=dt;
        if(g.premorph_timer>0){
            int pt_row = (bee->orig_type==EBUTTER)?EBUTTER_PRE:EBEE_PRE;
            int frame=(int)((PREMORPH_DUR-g.premorph_timer)/PREMORPH_DUR*PREMORPH_FRAMES);
            if(frame>PREMORPH_FRAMES-1) frame=PREMORPH_FRAMES-1;
            bee->frame_y=pt_row; bee->frame_x=frame;
            return 0;
        }
        int bi=g.premorph; g.premorph=-1; return do_morph(bi,ttype);
    }
    g.morph_timer-=dt;
    if(g.morph_timer>0) return 0;
    g.morph_timer=8.0f;
    int src=-1;
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i]; if(e->present&&e->alive&&e->in_formation&&e->type==EBEE){src=i;break;} }
    if(src<0) for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i]; if(e->present&&e->alive&&e->in_formation&&e->type==EBUTTER){src=i;break;} }
    if(src<0) return 0;
    g.premorph=src; g.premorph_timer=PREMORPH_DUR;
    return 0;
}
static void transforms_update(float dt){
    for(int gi=0;gi<MAXTG;gi++){ TGroup*t=&g.tg[gi]; if(!t->active) continue;
        t->t += dt*t->speed; int seg=t->plen-1;
        if(t->t>=1.0f){ t->active=0; for(int i=0;i<3;i++) t->e[i].alive=0; continue; }
        for(int i=0;i<3;i++){ if(!t->e[i].alive) continue;
            float et=t->t - i*0.06f; if(et<0)et=0;
            float ts=et*seg; int si=(int)ts; float st=ts-si; if(si>=seg){si=seg-1;st=1.0f;}
            const Pt*p=t->path;
            float cx=t->sx + p[si].x + (p[si+1].x-p[si].x)*st;
            float cy=t->sy + p[si].y + (p[si+1].y-p[si].y)*st;
            float spread=(i-1)*TRANSFORM_SPREAD*(0.5f+t->t);
            t->e[i].x=cx+spread; t->e[i].y=cy;
        }
        t->fire_timer-=dt;
        if(t->fire_timer<=0){ t->fire_timer=rndf()*0.8f+0.4f;
            int al[3],na=0; for(int i=0;i<3;i++) if(t->e[i].alive)al[na++]=i;
            if(na){ int s=al[randint(0,na-1)]; fire_enemy(t->e[s].x,t->e[s].y); }
        }
    }
}

/* ── challenge stage ──────────────────────────────────────────────────────── */
static void ch_init(int stage){
    int ci=((stage-3)/4)%8; int etype=CHALLENGE_TYPE[ci]; const CWave*cfg=CFG[ci];
    g.nce=0; g.ch_time=0; g.ch_kills=0; g.ch_perfect=0;
    for(int i=0;i<CH_WAVES;i++){ g.wave_kills[i]=0; g.wave_done[i]=0; }
    int ni=0;
    for(int w=0;w<CH_WAVES;w++){ const CWave*wv=&cfg[w]; float ws=w*CH_WAVE_DELAY;
        for(int i=0;i<CH_PER_WAVE;i++){ if(ni>=CH_TOTAL) break;
            CEnemy*c=&g.ce[ni]; memset(c,0,sizeof *c);
            int tt=(w==1 && i%2==0)?EBOSS:etype;
            c->etype=tt; c->hp=EHP[tt]; c->alive=1; c->wave_idx=w; c->vis=0; c->x=-200; c->y=-200;
            int ia,ib; idle_frames(tt,&ia,&ib); c->frame_y=tt; c->frame_x=ia;
            if(wv->split){ if(i<4){c->path=wv->a;c->plen=wv->al;} else {c->path=wv->b;c->plen=wv->bl;}
                c->start_time=ws+(i%4)*CH_SPACING; }
            else { c->path=wv->a; c->plen=wv->al; c->start_time=ws+i*CH_SPACING; }
            ni++;
        }
    }
    g.nce=ni;
}
static int update_challenge(float dt){
    g.ch_time+=dt; int all=1;
    for(int k=0;k<g.nce;k++){ CEnemy*c=&g.ce[k];
        if(c->finished) continue;
        if(!c->alive){ c->finished=1; continue; }
        all=0;
        if(g.ch_time<c->start_time) continue;
        float et=g.ch_time-c->start_time, t=et/CH_PATH_DUR;
        if(t>=1.0f){ c->finished=1; c->active=0; c->vis=0; c->x=-200; c->y=-200; continue; }
        if(!c->active){ c->active=1; c->vis=1; c->last_x=c->path[0].x; c->last_y=c->path[0].y; }
        float px,py; path_interp(c->path,c->plen,t,&px,&py); c->x=px; c->y=py;
        float dx=px-c->last_x, dy=py-c->last_y; c->last_x=px; c->last_y=py;
        if(c->etype==ESAT){ c->frame_x=(int)(et*6)%3; c->hflip=0; continue; }
        int maxf=(c->etype<20)?EFC[c->etype]:8; int ia,ib; idle_frames(c->etype,&ia,&ib);
        float adx=fabsf(dx), ady=fabsf(dy);
        if(adx<0.2f && ady<0.2f){ c->frame_x=ia; continue; }
        float ratio=(ady>0.01f)?adx/ady:99.0f; int frame;
        if(ratio<0.13f)frame=6; else if(ratio<0.41f)frame=5; else if(ratio<0.77f)frame=4;
        else if(ratio<1.33f)frame=3; else if(ratio<2.5f)frame=2; else if(ratio<5.0f)frame=1; else frame=0;
        if(frame>maxf-1)frame=maxf-1;
        if(dx<-0.2f)c->hflip=1; else if(dx>0.2f)c->hflip=0;
        if(dy<-0.2f)c->vflip=1; else if(dy>0.2f)c->vflip=0;
        c->frame_x=frame;
    }
    return all;
}

/* ── dying-flash sweep (all states) ───────────────────────────────────────── */
static void flash_sweep(float dt){
    int died=0;
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(e->present && e->hit_flash>0){ e->hit_flash-=dt;
            if(e->hit_flash<=0){ e->hit_flash=0; enemy_kill(e); died=1; } } }
    if(died) count_alive();
}

/* ── initials entry ───────────────────────────────────────────────────────── */
static const char IC[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
static int ic_index(char c){ for(int i=0;IC[i];i++) if(IC[i]==c) return i; return 0; }
static void start_initials(void){ g.initials[0]=g.initials[1]=g.initials[2]='A'; g.init_pos=0; g.init_flash=0; }
static int initials_input(const MoteInput*in,float dt){
    g.init_flash+=dt; int pos=g.init_pos; int ci=ic_index(g.initials[pos]); int N=(int)strlen(IC);
    if(mote_just_pressed(in,MOTE_BTN_UP)){ ci=(ci-1+N)%N; g.initials[pos]=IC[ci]; g.init_flash=0; }
    else if(mote_just_pressed(in,MOTE_BTN_DOWN)){ ci=(ci+1)%N; g.initials[pos]=IC[ci]; g.init_flash=0; }
    else if(mote_just_pressed(in,MOTE_BTN_A)){
        if(pos<2){ g.init_pos++; g.init_flash=0; }
        else { sb_insert(g.initials); return 1; }
    }
    else if(mote_just_pressed(in,MOTE_BTN_B)){ if(pos>0){ g.init_pos--; g.init_flash=0; } }
    return 0;
}

/* ════════════════════════ UPDATE ════════════════════════════════════════════ */
static void g_update(float dt){
    const MoteInput*in=mote->input();
    if(dt>0.05f) dt=0.05f;
    if(mote_just_pressed(in,MOTE_BTN_MENU)){ g.mute=!g.mute; mote->audio_set_master(g.mute?0.0f:1.0f); }
    stars_update(dt);

    switch(g.state){
    case ST_TITLE:
        g.title_t+=dt;
        if(mote_just_pressed(in,MOTE_BTN_A)){ reset_game(); g.state=ST_STAGE_INTRO; g.state_timer=1.5f; snd(&level_start_snd); }
        break;

    case ST_STAGE_INTRO:
        level_set(g.stage);
        g.state_timer-=dt;
        if(g.state_timer<=0){
            if(is_challenge_stage(g.stage)){ hide_all_enemies(); ch_init(g.stage); bullets_clear(); g.state=ST_CHALLENGE; }
            else { build_entry(g.stage); start_stage(); g.state=ST_ENTRY; }
        }
        break;

    case ST_ENTRY: {
        formation_update(dt); update_enemy_frames(dt);
        if(update_entry(dt)){ g.state=ST_PLAYING; g.dive_timer=g.lv.dive_interval; }
        for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
            if(e->present&&e->alive&&e->entry_done&&!e->in_formation&&e->dive_path){
                if(!dive_step(i,dt,g.lv.dive_speed)) maybe_enemy_fire(i); } }
        player_update(dt,mote_pressed(in,MOTE_BTN_LEFT),mote_pressed(in,MOTE_BTN_RIGHT));
        handle_fire(in);
        bullets_update(dt);
        for(int bi=0;bi<MAXPB;bi++){ if(!g.pba[bi])continue; int ei=pb_vs_formation(bi); if(ei<0)continue;
            Enemy*e=&g.en[ei]; g.pba[bi]=0; g.hits++; e->hp--;
            if(e->hp<=0){ enemy_start_dying(e); explosion_spawn(e->x,e->y); snd(explode_sfx(e->type));
                add_score(POINTS[e->type][0]); count_alive(); }
            else if(e->type==EBOSS){ e->frame_y=EBOSSHIT; e->type=EBOSSHIT; snd(&explode3_snd); } }
        explosions_update(dt);
        break; }

    case ST_PLAYING: {
        player_update(dt,mote_pressed(in,MOTE_BTN_LEFT),mote_pressed(in,MOTE_BTN_RIGHT));
        handle_fire(in);
        formation_update(dt); update_enemy_frames(dt);
        if(g.capturing_boss>=0 && g.en[g.capturing_boss].alive){
            g.cap_x=g.en[g.capturing_boss].x; g.cap_y=g.en[g.capturing_boss].y-12.0f; }
        int diver=maybe_trigger_dive(dt, g.beam_phase!=BEAM_OFF);
        if(diver>=0) snd(&dive_snd);
        for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
            if(!(e->present&&e->alive&&!e->in_formation&&e->entry_done)) continue;
            if(i==g.beam_boss) continue;
            if(!dive_step(i,dt,g.lv.dive_speed)){
                int isb=IS_BOSS(e->type);
                if(isb&&e->will_beam&&g.beam_phase==BEAM_OFF&&e->y>10.0f&&e->y<30.0f){ start_beam(i); snd(&beam_snd); }
                else if(!isb) maybe_enemy_fire(i);
            } }
        if(g.beam_phase!=BEAM_OFF){ if(update_beam(dt)){ snd(&beam_capture_snd); g.state=ST_DYING; g.state_timer=2.5f; } }
        bullets_update(dt);
        /* player bullets vs formation enemies */
        for(int bi=0;bi<MAXPB;bi++){ if(!g.pba[bi])continue; int ei=pb_vs_formation(bi); if(ei<0)continue;
            Enemy*e=&g.en[ei]; g.pba[bi]=0; g.hits++; e->hp--;
            if(e->hp<=0){
                int diving=!e->in_formation; int isb=IS_BOSS(e->type); int pts;
                enemy_start_dying(e); explosion_spawn(e->x,e->y); snd(explode_sfx(e->type));
                if(isb&&diving&&e->nescorts>0){ pts=(e->nescorts==1)?800:1600; popup_int(pts,e->x,e->y); }
                else if(isb&&diving){ pts=400; popup_int(pts,e->x,e->y); }
                else pts=POINTS[e->type][diving?1:0];
                add_score(pts);
                if(isb && g.capturing_boss==ei && diving){ start_rescue(); snd(&rescue_snd); }
                else if(isb && g.capturing_boss==ei && !diving){ float hx=g.cap_x,hy=g.cap_y; g.capturing_boss=-1; hf_activate(hx,hy); }
                count_alive();
            } else if(e->type==EBOSS){ e->frame_y=EBOSSHIT; e->type=EBOSSHIT; snd(&explode3_snd); }
        }
        /* enemy bullets vs player */
        if(g.p_alive && g.p_inv<=0){
            for(int i=0;i<MAXEB;i++){ if(!g.eba[i])continue;
                if(fabsf(g.ebx[i]-g.px)<PHALFW && fabsf(g.eby[i]-g.py)<PHALFH){ g.eba[i]=0; player_die(); break; } }
        }
        /* divers vs player */
        if(g.state==ST_PLAYING && g.p_alive && g.p_inv<=0){
            for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
                if(!(e->present&&e->alive&&!e->in_formation&&e->entry_done)) continue;
                if(fabsf(e->x-g.px)<EHALF+PHALFW-2 && fabsf(e->y-g.py)<EHALF+PHALFH-2){ player_die(); break; } }
        }
        /* transforms */
        if(try_morph(dt)) snd(&transform_snd);
        transforms_update(dt);
        if(g.state==ST_PLAYING){
            for(int bi=0;bi<MAXPB;bi++){ if(!g.pba[bi])continue; float bx=g.pbx[bi],by=g.pby[bi]; int hit=0;
                for(int gi=0;gi<MAXTG&&!hit;gi++){ TGroup*t=&g.tg[gi]; if(!t->active)continue;
                    for(int e=0;e<3;e++){ if(!t->e[e].alive)continue;
                        if(fabsf(bx-t->e[e].x)<EHALF+1 && fabsf(by-t->e[e].y)<EHALF+2){
                            g.pba[bi]=0; g.hits++; float ex=t->e[e].x,ey=t->e[e].y;
                            t->e[e].alive=0; t->kills++;
                            int anyalive=0; for(int q=0;q<3;q++) if(t->e[q].alive)anyalive=1; if(!anyalive)t->active=0;
                            explosion_spawn(ex,ey); snd(&explode_boss_snd); add_score(TRANSFORM_PTS);
                            if(t->kills>=3){ add_score(t->bonus); popup_int(t->bonus,ex,ey); }
                            hit=1; break;
                        } } } }
        }
        /* transforms vs player */
        if(g.state==ST_PLAYING && g.p_alive && g.p_inv<=0){
            int hit=0;
            for(int gi=0;gi<MAXTG&&!hit;gi++){ TGroup*t=&g.tg[gi]; if(!t->active)continue;
                for(int e=0;e<3;e++){ if(!t->e[e].alive)continue;
                    if(fabsf(g.px-t->e[e].x)<EHALF+PHALFW-2 && fabsf(g.py-t->e[e].y)<EHALF+PHALFH-2){ player_die(); hit=1; break; } } }
        }
        /* hostile fighter */
        if(g.hf_active && g.hf_alive){
            update_hostile(dt);
            if(g.hf_active){
                for(int bi=0;bi<MAXPB;bi++){ if(!g.pba[bi])continue;
                    if(fabsf(g.pbx[bi]-g.hf_x)<PHALFW && fabsf(g.pby[bi]-g.hf_y)<PHALFH){
                        g.pba[bi]=0; g.hits++; explosion_spawn(g.hf_x,g.hf_y); snd(&explode_boss_snd);
                        add_score(1000); popup_int(1000,g.hf_x,g.hf_y); hf_kill(); break; } }
            }
            if(g.hf_active && g.p_alive && g.p_inv<=0){
                if(fabsf(g.px-g.hf_x)<PHALFW+4 && fabsf(g.py-g.hf_y)<PHALFH+4) player_die();
            }
        }
        check_extra_life();
        if(g.alive_count<=0 && transforms_active()==0 && !(g.hf_active&&g.hf_alive)){
            g.state=ST_STAGE_CLEAR; g.state_timer=1.5f; stop_beam(); bullets_clear();
        }
        explosions_update(dt);
        break; }

    case ST_CHALLENGE: {
        player_update(dt,mote_pressed(in,MOTE_BTN_LEFT),mote_pressed(in,MOTE_BTN_RIGHT));
        handle_fire(in);
        if(update_challenge(dt)){
            g.state=ST_STAGE_CLEAR; g.state_timer=3.0f;
            if(g.ch_kills>=CH_TOTAL){ g.ch_perfect=1; add_score(CH_PERFECT_BONUS); popup_spawn("PERFECT!",0,-10); popup_int(CH_PERFECT_BONUS,0,0); }
            char b[16]; snprintf(b,sizeof b,"HITS:%d",g.ch_kills); popup_spawn(b,0,10);
        }
        bullets_update(dt);
        for(int bi=0;bi<MAXPB;bi++){ if(!g.pba[bi])continue; float bx=g.pbx[bi],by=g.pby[bi]; int hit=0;
            for(int k=0;k<g.nce&&!hit;k++){ CEnemy*c=&g.ce[k]; if(!c->active||!c->alive)continue;
                if(fabsf(bx-c->x)<EHALF+1 && fabsf(by-c->y)<EHALF+2){
                    g.pba[bi]=0; g.hits++; c->hp--; float ex=c->x,ey=c->y;
                    if(c->hp<=0){ c->alive=0; c->active=0; c->vis=0; explosion_spawn(ex,ey); snd(&explode_snd);
                        g.ch_kills++; g.wave_kills[c->wave_idx]++; add_score(CH_HIT_PTS);
                        if(g.wave_kills[c->wave_idx]>=CH_PER_WAVE && !g.wave_done[c->wave_idx]){
                            g.wave_done[c->wave_idx]=1; int cn=(g.stage-3)/4+1;
                            int wb = cn<=2?1000 : cn<=4?1500 : cn<=6?2000 : 3000;
                            add_score(wb); popup_int(wb,ex,ey); }
                    } else { c->frame_y=EBOSSHIT; c->etype=EBOSSHIT; snd(&explode3_snd); }
                    hit=1;
                } } }
        explosions_update(dt);
        break; }

    case ST_DYING:
        g.state_timer-=dt;
        formation_update(dt); update_enemy_frames(dt);
        bullets_update(dt); explosions_update(dt);
        if(g.beam_phase!=BEAM_OFF) update_beam(dt);
        if(g.capturing_boss>=0 && g.en[g.capturing_boss].alive){
            g.cap_x=g.en[g.capturing_boss].x; g.cap_y=g.en[g.capturing_boss].y-12.0f; }
        if(g.state_timer<=0){
            g.pexa=0; g.lives--;
            if(g.lives<=0){ g.state=ST_GAME_OVER; g.state_timer=1.0f; hide_all_enemies(); bullets_clear(); stop_beam(); release_captured(); }
            else {
                player_reset(); bullets_clear();
                for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
                    if(e->present&&e->alive&&!e->in_formation&&i!=g.beam_boss){
                        e->in_formation=1; e->dive_path=NULL; e->nescorts=0;
                        float sx,sy; slot_pos(e->slot_col,e->slot_row,&sx,&sy); e->x=sx; e->y=sy; } }
                g.state=ST_PLAYING;
            }
        }
        break;

    case ST_STAGE_CLEAR:
        g.state_timer-=dt; explosions_update(dt);
        if(g.state_timer<=0){
            int prev_ch=is_challenge_stage(g.stage);
            hide_all_enemies(); g.stage++; g.state=ST_STAGE_INTRO; g.state_timer=1.5f;
            int next_ch=is_challenge_stage(g.stage);
            if(prev_ch) snd(&level_start_snd);
            else if(next_ch) snd(&challenge_start_snd);
            else snd(&level_start_snd);
        }
        break;

    case ST_GAME_OVER:
        if(g.state_timer>0) g.state_timer-=dt;
        explosions_update(dt);
        if(g.state_timer<=0 && mote_just_pressed(in,MOTE_BTN_A)){ hide_all_enemies(); g.state=ST_RESULTS; g.state_timer=0.5f; }
        break;

    case ST_RESULTS:
        if(g.state_timer>0) g.state_timer-=dt;
        if(g.state_timer<=0 && mote_just_pressed(in,MOTE_BTN_A)){
            if(sb_is_high()){ start_initials(); g.state=ST_INITIALS; } else g.state=ST_SCOREBOARD;
        }
        break;

    case ST_INITIALS:
        if(initials_input(in,dt)) g.state=ST_SCOREBOARD;
        break;

    case ST_SCOREBOARD:
        if(mote_just_pressed(in,MOTE_BTN_A)){ g.title_t=0; g.state=ST_TITLE; }
        break;
    }

    /* ── always ── */
    if(g.hf_entry_boss>=0) update_hostile_entry(dt);
    flash_sweep(dt);
    popups_update(dt);
    if(g.rescue_active) update_rescue(dt);
}

/* ════════════════════════ RENDER ════════════════════════════════════════════ */
static void draw_formation(uint16_t*fb){
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(e->present && e->vis) spr(fb,&enemies_img,e->x,e->y,e->frame_x*12,e->frame_y*12,12,12,e->hflip,e->vflip); }
}
static void draw_transforms(uint16_t*fb){
    for(int gi=0;gi<MAXTG;gi++){ TGroup*t=&g.tg[gi]; if(!t->active)continue;
        for(int e=0;e<3;e++){ if(!t->e[e].alive)continue;
            spr(fb,&enemies_img,t->e[e].x,t->e[e].y,t->e[e].frame_x*12,t->e[e].frame_y*12,12,12,t->e[e].hflip,t->e[e].vflip); } }
}
static void draw_challenge(uint16_t*fb){
    for(int k=0;k<g.nce;k++){ CEnemy*c=&g.ce[k];
        if(c->vis) spr(fb,&enemies_img,c->x,c->y,c->frame_x*12,c->frame_y*12,12,12,c->hflip,c->vflip); }
}
static void draw_captured(uint16_t*fb){
    if(g.cap_op<=0) return;
    const MoteImage*im=g.cap_red?&player_captured_img:&player_img;
    mote->blit_ex(fb,im,(float)SX(g.cap_x),(float)SX(g.cap_y),0,0,12,12,g.cap_rot,1.0f,MOTE_BLEND_NONE,0,128);
}
static int player_visible(void){
    if(!g.p_alive) return 0;
    if(g.p_inv<=0) return 1;
    return ((int)(g.p_inv*10))%2==0;
}
static void draw_player(uint16_t*fb){
    if(!player_visible()) return;
    spr(fb,&player_img,g.px,g.py,0,0,12,12,0,0);
    if(g.dual) spr(fb,&player_img,g.px+WING_OFF,g.py,0,0,12,12,0,0);
}
static void draw_beam(uint16_t*fb){
    if(g.beam_phase==BEAM_OFF || g.beam_boss<0) return;
    float bx=g.en[g.beam_boss].x, by=g.en[g.beam_boss].y;
    int cx=SX(bx), cy=SX(by)+6; float max_len=PLAYER_Y-by;
    if(max_len<=0) return; int beam_len=(int)(max_len*g.beam_reveal); if(beam_len<=0) return;
    for(int dy=0;dy<beam_len;dy++){ int half=2+(int)((dy*10.0f)/max_len); int y=cy+dy;
        if(y<0||y>=128) continue;
        uint16_t col = dy%3==0?BEAM_C1 : dy%3==1?BEAM_C2 : BEAM_C3;
        int xs=cx-half; if(xs<0)xs=0; int xe=cx+half; if(xe>127)xe=127;
        if(xs<=xe) hline(fb,xs,y,xe-xs+1,col); }
}

/* ── HUD ──────────────────────────────────────────────────────────────────── */
static void draw_badges(uint16_t*fb){
    struct { const MoteImage*img; int fx,fw,fh; } badges[16]; int nb=0;
    int s=g.stage; if(s<=0) return;
    while(s>=50 && nb<16){ badges[nb++]=(typeof(badges[0])){&badge_shields_img,3*8,8,8}; s-=50; }
    if(s>=30){ badges[nb++]=(typeof(badges[0])){&badge_shields_img,2*8,8,8}; s-=30; }
    if(s>=20){ badges[nb++]=(typeof(badges[0])){&badge_shields_img,1*8,8,8}; s-=20; }
    if(s>=10){ badges[nb++]=(typeof(badges[0])){&badge_shields_img,0*8,8,8}; s-=10; }
    if(s>=5){ badges[nb++]=(typeof(badges[0])){&badge_narrow_img,1*5,5,10}; s-=5; }
    while(s>0 && nb<16){ badges[nb++]=(typeof(badges[0])){&badge_narrow_img,0*5,5,10}; s--; }
    int x=126;
    for(int i=nb-1;i>=0;i--){ x-=badges[i].fw+1; if(x<50) break;
        int y=128-badges[i].fh; mote->blit(fb,badges[i].img,x,y,badges[i].fx,0,badges[i].fw,badges[i].fh,0,0,128); }
}
static void hud_draw(uint16_t*fb){
    char b[24];
    snprintf(b,sizeof b,"%d",g.score); mote->text(fb,b,2,2,COL_WHITE);
    snprintf(b,sizeof b,"%d",g.high_score);
    int w=(int)strlen(b)*4; int xn=126-w; mote->text(fb,"HI",xn-9,2,COL_RED); mote->text(fb,b,xn,2,COL_WHITE);
    int show=g.lives-1; if(show<0)show=0; if(show>8)show=8;
    for(int i=0;i<show;i++) mote->blit(fb,&life_icon_img,2+i*8,121,0,0,6,6,0,0,128);
    draw_badges(fb);
}

/* ── menu / screens ───────────────────────────────────────────────────────── */
static void scoreboard_draw(uint16_t*fb,int press_a){
    text_c(fb,"- TOP SCORES -",64,8,COL_RED);
    for(int i=0;i<5;i++){ int y=26+i*14; char b[24];
        if(i<g.sb.n){ uint16_t col=(i==g.new_rank)?COL_YELLOW:COL_TEAL;
            snprintf(b,sizeof b,"%d.",i+1); mote->text(fb,b,12,y,col);
            mote->text(fb,g.sb.e[i].name,32,y,col);
            snprintf(b,sizeof b,"%d",g.sb.e[i].score); mote->text(fb,b,62,y,col);
        } else { snprintf(b,sizeof b,"%d.",i+1); mote->text(fb,b,12,y,COL_TEAL); mote->text(fb,"---",32,y,COL_TEAL); }
    }
    if(press_a) text_c(fb,"PRESS A",64,108,COL_WHITE);
}
static void results_draw(uint16_t*fb){
    char b[24];
    text_c(fb,"- RESULTS -",64,8,COL_RED);
    mote->text(fb,"SHOTS",14,24,COL_TEAL); snprintf(b,sizeof b,"%d",g.shots_fired); mote->text(fb,b,96,24,COL_WHITE);
    mote->text(fb,"HITS",14,36,COL_TEAL);  snprintf(b,sizeof b,"%d",g.hits); mote->text(fb,b,96,36,COL_WHITE);
    int ratio = g.shots_fired>0 ? (g.hits*100)/g.shots_fired : 0;
    mote->text(fb,"HIT RATIO",14,48,COL_TEAL); snprintf(b,sizeof b,"%d%%",ratio); mote->text(fb,b,96,48,COL_WHITE);
    mote->text(fb,"SCORE",14,64,COL_RED); snprintf(b,sizeof b,"%d",g.score); mote->text(fb,b,96,64,COL_YELLOW);
    if(sb_is_high()){ text_c(fb,"NEW HIGH SCORE!",64,84,COL_YELLOW); text_c(fb,"PRESS A",64,100,COL_WHITE); }
    else text_c(fb,"PRESS A",64,92,COL_WHITE);
}
static void initials_draw(uint16_t*fb){
    char b[24];
    text_c(fb,"ENTER INITIALS",64,15,COL_RED);
    snprintf(b,sizeof b,"SCORE %d",g.score); text_c(fb,b,64,30,COL_YELLOW);
    for(int i=0;i<3;i++){ int x=52+i*12, y=50; char ch[2]={g.initials[i],0};
        if(i==g.init_pos){ if(((int)(g.init_flash*4))%2==0) mote->text(fb,ch,x,y,COL_YELLOW); hline(fb,x,y+8,6,COL_YELLOW); }
        else { mote->text(fb,ch,x,y,COL_WHITE); hline(fb,x,y+8,6,COL_WHITE); } }
    text_c(fb,"UP/DN SELECT",64,76,COL_TEAL);
    text_c(fb,"A=NEXT B=BACK",64,88,COL_TEAL);
}
static void title_draw(uint16_t*fb){
    int phase=((int)(g.title_t/3.0f))%2;
    if(phase==0){ mote->blit(fb,&logo_img,0,0,0,0,128,128,0,0,128); text_c(fb,"PRESS A",64,116,COL_WHITE); }
    else scoreboard_draw(fb,1);
}

static void g_overlay(uint16_t*fb){
    for(int i=0;i<128*128;i++) fb[i]=COL_BLACK;
    stars_draw(fb);
    switch(g.state){
    case ST_TITLE: title_draw(fb); break;
    case ST_STAGE_INTRO:
        text_c(fb, is_challenge_stage(g.stage)?"CHALLENGE":"STAGE", 64,52,COL_CYAN);
        if(!is_challenge_stage(g.stage)){ char b[8]; snprintf(b,sizeof b,"%d",g.stage); text_c(fb,b,64,64,COL_WHITE); }
        break;
    case ST_ENTRY:
        draw_formation(fb); draw_player(fb); bullets_draw(fb); explosions_draw(fb); hud_draw(fb);
        break;
    case ST_PLAYING:
        draw_formation(fb); draw_captured(fb); draw_transforms(fb); draw_player(fb);
        bullets_draw(fb); draw_beam(fb); explosions_draw(fb); player_exp_draw(fb); hud_draw(fb);
        break;
    case ST_CHALLENGE:
        draw_challenge(fb); draw_player(fb); bullets_draw(fb); explosions_draw(fb); hud_draw(fb);
        break;
    case ST_DYING:
        draw_formation(fb); draw_captured(fb); bullets_draw(fb); draw_beam(fb);
        explosions_draw(fb); player_exp_draw(fb); hud_draw(fb);
        if(g.beam_phase==BEAM_CAPTURE || (g.beam_phase==BEAM_RETURN && g.capturing_boss>=0))
            text_c(fb,"FIGHTER CAPTURED",64,52,COL_RED);
        break;
    case ST_STAGE_CLEAR:
        explosions_draw(fb); hud_draw(fb);
        break;
    case ST_GAME_OVER:
        hud_draw(fb); text_c(fb,"GAME OVER",64,54,COL_RED);
        break;
    case ST_RESULTS: results_draw(fb); break;
    case ST_INITIALS: initials_draw(fb); break;
    case ST_SCOREBOARD: scoreboard_draw(fb,1); break;
    }
    popups_draw(fb);
}

/* ════════════════════════ INIT ══════════════════════════════════════════════ */
static void g_init(void){
    uint64_t t=mote->micros(); s_rng=(uint32_t)(t^(t>>32))|1u;
    memset(&g,0,sizeof g);
    g.state=ST_TITLE; g.lives=START_LIVES; g.stage=1; g.next_extra_life=EXTRA_LIFE_FIRST;
    g.capturing_boss=-1; g.beam_boss=-1; g.hf_entry_boss=-1; g.new_rank=-1; g.sway_dir=1;
    g.px=0; g.py=PLAYER_Y;
    stars_init();
    sb_load();
    mote->audio_set_master(1.0f);
}

#endif /* THUMBALAGA_LOGIC_H */
