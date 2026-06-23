/*
 * tanks — a Wii Play "Tanks!" minigame recreation, and the showcase for Mote's
 * 3D rigid-part animation (mote_anim3d.h).
 *
 * Every tank is one rig: BODY (root) -> TURRET (yaws to aim) -> BARREL (recoils
 * on fire). The turret rotation is PROCEDURAL (it tracks a target every frame),
 * the barrel recoil decays — so this exercises the animation runtime's
 * pose-override path, not just baked clips.
 *
 * Controls: D-pad drives (UP/DOWN forward/back, LEFT/RIGHT turn). The turret
 * auto-aims the nearest enemy; A fires (max 5 shells out). Shells ricochet off
 * walls once — your own bounce can kill you. Clear all enemies to win; B restarts.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_anim3d.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- box centred at an arbitrary model point (parts are modelled in place) ---- */
static const Mesh *box_at(const MoteApi *api, float hx, float hy, float hz, Vec3 c, uint16_t col) {
    MeshVert *v = (MeshVert *)api->alloc(8 * sizeof(MeshVert));
    MeshFace *f = (MeshFace *)api->alloc(12 * sizeof(MeshFace));
    Mesh *m = (Mesh *)api->alloc(sizeof(Mesh));
    if (!v || !f || !m) return 0;
    float ex = fabsf(c.x)+hx, ey = fabsf(c.y)+hy, ez = fabsf(c.z)+hz;
    float sc = ex > ey ? ex : ey; if (ez > sc) sc = ez; if (sc < 1e-4f) sc = 1e-4f;
    float X[8]={-hx,hx,hx,-hx,-hx,hx,hx,-hx}, Y[8]={-hy,-hy,-hy,-hy,hy,hy,hy,hy}, Z[8]={-hz,-hz,hz,hz,-hz,-hz,hz,hz};
    for (int i=0;i<8;i++){ v[i].x=(int8_t)((X[i]+c.x)/sc*127); v[i].y=(int8_t)((Y[i]+c.y)/sc*127); v[i].z=(int8_t)((Z[i]+c.z)/sc*127); }
    int q[6][4]={{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7},{4,5,6,7},{3,2,1,0}};
    int nf=0; for (int s=0;s<6;s++){ mote__face(v,f,&nf,q[s][0],q[s][2],q[s][1],col); mote__face(v,f,&nf,q[s][0],q[s][3],q[s][2],col); }
    *m=(Mesh){v,f,0,8,12,col,sc,sc*1.8f,0}; return m;
}

/* ---- the tank rig: body / turret / barrel ---- */
enum { P_BODY, P_TURRET, P_BARREL, P_COUNT };
static MoteRigPart s_tp[P_COUNT];
static MoteRig     s_tank;
#define TURRET_Y 0.50f
#define BARREL_LEN 0.95f

static void build_tank(void) {
    const Mesh *hull   = box_at(mote, 0.46f, 0.16f, 0.60f, v3(0, 0.22f, 0),       MOTE_RGB565(210,210,210));
    const Mesh *turret = box_at(mote, 0.30f, 0.15f, 0.34f, v3(0, TURRET_Y, 0.02f),MOTE_RGB565(235,235,235));
    const Mesh *barrel = box_at(mote, 0.06f, 0.06f, 0.42f, v3(0, TURRET_Y, 0.50f),MOTE_RGB565(120,120,130));
    s_tp[P_BODY]   = (MoteRigPart){ hull,   1, -1, v3(0,0,0) };
    s_tp[P_TURRET] = (MoteRigPart){ turret, 1,  P_BODY,   v3(0, TURRET_Y, 0.02f) };   /* yaw axis */
    s_tp[P_BARREL] = (MoteRigPart){ barrel, 1,  P_TURRET, v3(0, TURRET_Y, 0.14f) };   /* recoils along -Z */
    s_tank = (MoteRig){ s_tp, P_COUNT, P_COUNT * 12 };
}

/* ---- arena ---- */
#define AX 5.0f
#define AZ 3.4f
typedef struct { float cx, cz, hx, hz; } Block;
static Block s_blocks[5];
static const Mesh *s_block_mesh[5];
static int   s_nblocks;
static const Mesh *s_floor, *s_wall[4];

static void build_arena(void) {
    uint16_t wc = MOTE_RGB565(120,110,95);
    s_floor   = box_at(mote, AX, 0.05f, AZ, v3(0,-0.05f,0), MOTE_RGB565(70,92,70));
    /* 4 long border walls (cheap — not tiled segments) */
    s_wall[0] = box_at(mote, AX+0.25f, 0.45f, 0.25f, v3(0,0.4f, AZ+0.25f), wc);
    s_wall[1] = box_at(mote, AX+0.25f, 0.45f, 0.25f, v3(0,0.4f,-AZ-0.25f), wc);
    s_wall[2] = box_at(mote, 0.25f, 0.45f, AZ+0.25f, v3(-AX-0.25f,0.4f,0), wc);
    s_wall[3] = box_at(mote, 0.25f, 0.45f, AZ+0.25f, v3( AX+0.25f,0.4f,0), wc);
    s_nblocks = 0;
    s_blocks[s_nblocks++] = (Block){ -2.0f,  0.0f, 0.5f, 1.2f };
    s_blocks[s_nblocks++] = (Block){  2.0f,  0.0f, 0.5f, 1.2f };
    s_blocks[s_nblocks++] = (Block){  0.0f, -1.7f, 1.0f, 0.45f };
    s_blocks[s_nblocks++] = (Block){  0.0f,  1.7f, 1.0f, 0.45f };
    for (int i = 0; i < s_nblocks; i++)
        s_block_mesh[i] = box_at(mote, s_blocks[i].hx, 0.45f, s_blocks[i].hz, v3(0,0.45f,0), MOTE_RGB565(150,138,120));
}

/* ---- tanks ---- */
typedef struct {
    float x, z, yaw, aim, recoil, fire_cd, think_cd, wander;
    int   alive, is_player;
    uint16_t color;
} Tank;
#define MAXT 6
static Tank s_t[MAXT];
static int  s_nt;

/* ---- shells ---- */
typedef struct { float x, z, vx, vz, life; int bounces, owner, alive; } Shell;
#define MAXS 24
static Shell s_s[MAXS];

/* ---- explosions (impostor spheres) ---- */
typedef struct { float x,y,z, r, life; uint16_t col; } Boom;
#define MAXB 24
static Boom s_b[MAXB];

static int s_state;            /* 0 play, 1 cleared, 2 destroyed */
enum { ST_PLAY, ST_WIN, ST_LOSE };

/* ---------------------------------------------------------------- helpers */
static float anglerp(float a, float target, float maxstep) {
    float d = target - a;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    if (d >  maxstep) d =  maxstep;
    if (d < -maxstep) d = -maxstep;
    return a + d;
}
/* push a circle out of every block + the arena bounds */
static void collide_circle(float *x, float *z, float r) {
    if (*x < -AX + r) *x = -AX + r; if (*x > AX - r) *x = AX - r;
    if (*z < -AZ + r) *z = -AZ + r; if (*z > AZ - r) *z = AZ - r;
    for (int i = 0; i < s_nblocks; i++) {
        Block *b = &s_blocks[i];
        float nx = mote_clampf(*x, b->cx - b->hx, b->cx + b->hx);
        float nz = mote_clampf(*z, b->cz - b->hz, b->cz + b->hz);
        float dx = *x - nx, dz = *z - nz, d2 = dx*dx + dz*dz;
        if (d2 >= r*r) continue;
        if (d2 > 1e-5f) { float d = sqrtf(d2), p = (r - d) / d; *x += dx*p; *z += dz*p; }
        else { /* centre inside the block: eject along the nearest face */
            float px = (b->cx+b->hx) - *x, mx = *x - (b->cx-b->hx);
            float pz = (b->cz+b->hz) - *z, mz = *z - (b->cz-b->hz);
            float minx = px<mx?px:mx, minz = pz<mz?pz:mz;
            if (minx < minz) *x += (px<mx ? px+r : -(mx+r));
            else             *z += (pz<mz ? pz+r : -(mz+r));
        }
    }
}

static void spawn_boom(float x, float z, uint16_t col) {
    for (int n = 0; n < 8; n++)
        for (int i = 0; i < MAXB; i++) if (s_b[i].life <= 0) {
            s_b[i].x = x + mote_randf(-0.3f,0.3f); s_b[i].y = 0.3f + mote_randf(0,0.5f); s_b[i].z = z + mote_randf(-0.3f,0.3f);
            s_b[i].r = mote_randf(0.12f,0.26f); s_b[i].life = mote_randf(0.25f,0.5f); s_b[i].col = col; break;
        }
}

static void fire(Tank *t) {
    if (t->fire_cd > 0) return;
    int live = 0; for (int i = 0; i < MAXS; i++) if (s_s[i].alive && s_s[i].owner == (int)(t - s_t)) live++;
    int cap = t->is_player ? 5 : 2;
    if (live >= cap) return;
    for (int i = 0; i < MAXS; i++) if (!s_s[i].alive) {
        float dx = sinf(t->aim), dz = cosf(t->aim);
        s_s[i].x = t->x + dx * (BARREL_LEN); s_s[i].z = t->z + dz * (BARREL_LEN);
        s_s[i].vx = dx * 5.2f; s_s[i].vz = dz * 5.2f;
        s_s[i].bounces = 1; s_s[i].owner = (int)(t - s_t); s_s[i].life = 4.0f; s_s[i].alive = 1;
        t->recoil = 1.0f; t->fire_cd = t->is_player ? 0.45f : 1.4f;
        mote->audio_note(t->is_player ? 220.0f : 150.0f, 0.7f);
        spawn_boom(s_s[i].x, s_s[i].z, MOTE_RGB565(250,210,90));
        break;
    }
}

static void reset_level(void) {
    for (int i = 0; i < MAXS; i++) s_s[i].alive = 0;
    for (int i = 0; i < MAXB; i++) s_b[i].life = 0;
    s_nt = 0;
    /* player (blue) */
    s_t[s_nt++] = (Tank){ -3.9f, 0, 0, 0, 0,0,0,0, 1, 1, MOTE_RGB565(80,150,255) };
    /* enemies — high-contrast vs the green floor / tan blocks; staggered first shot */
    s_t[s_nt++] = (Tank){  3.7f,  1.8f, 3.14f, 3.14f, 0, mote_randf(0.6f,1.8f), mote_randf(0.3f,1.0f), 0, 1,0, MOTE_RGB565(235,70,55)  };
    s_t[s_nt++] = (Tank){  3.7f, -1.8f, 3.14f, 3.14f, 0, mote_randf(0.6f,1.8f), mote_randf(0.3f,1.0f), 0, 1,0, MOTE_RGB565(240,200,70) };
    s_t[s_nt++] = (Tank){  0.0f,  0.0f, 3.14f, 3.14f, 0, mote_randf(0.6f,1.8f), mote_randf(0.3f,1.0f), 0, 1,0, MOTE_RGB565(180,120,235) };
    s_state = ST_PLAY;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(28, 34, 52));
    mote->scene_set_sun(v3_norm(v3(-0.35f, 0.9f, 0.4f)));
    mote_rand_seed(0x7A4E12u);
    build_tank();
    build_arena();
    reset_level();
}

static int nearest_enemy(Tank *self) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < s_nt; i++) if (s_t[i].alive && &s_t[i] != self && (self->is_player ? !s_t[i].is_player : s_t[i].is_player)) {
        float dx = s_t[i].x - self->x, dz = s_t[i].z - self->z, d = dx*dx+dz*dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void step_shells(float dt) {
    for (int i = 0; i < MAXS; i++) {
        Shell *s = &s_s[i]; if (!s->alive) continue;
        s->life -= dt; if (s->life <= 0) { s->alive = 0; continue; }
        s->x += s->vx * dt; s->z += s->vz * dt;
        float r = 0.10f;
        /* arena walls */
        if (s->x < -AX+r) { s->x = -AX+r; s->vx = -s->vx; if (--s->bounces < 0) { s->alive=0; continue; } }
        if (s->x >  AX-r) { s->x =  AX-r; s->vx = -s->vx; if (--s->bounces < 0) { s->alive=0; continue; } }
        if (s->z < -AZ+r) { s->z = -AZ+r; s->vz = -s->vz; if (--s->bounces < 0) { s->alive=0; continue; } }
        if (s->z >  AZ-r) { s->z =  AZ-r; s->vz = -s->vz; if (--s->bounces < 0) { s->alive=0; continue; } }
        /* blocks: reflect on the shallower-penetration axis */
        for (int b = 0; b < s_nblocks; b++) {
            Block *bl = &s_blocks[b];
            if (s->x > bl->cx-bl->hx-r && s->x < bl->cx+bl->hx+r && s->z > bl->cz-bl->hz-r && s->z < bl->cz+bl->hz+r) {
                float ox = (bl->hx + r) - fabsf(s->x - bl->cx);
                float oz = (bl->hz + r) - fabsf(s->z - bl->cz);
                if (ox < oz) { s->vx = -s->vx; s->x += (s->x < bl->cx ? -ox : ox); }
                else         { s->vz = -s->vz; s->z += (s->z < bl->cz ? -oz : oz); }
                if (--s->bounces < 0) { s->alive = 0; break; }
            }
        }
        if (!s->alive) continue;
        /* tank hits — only the opposing team (no friendly fire) */
        for (int k = 0; k < s_nt; k++) {
            Tank *t = &s_t[k]; if (!t->alive) continue;
            if (s_t[s->owner].is_player == t->is_player) continue;    /* same team */
            float dx = t->x - s->x, dz = t->z - s->z;
            if (dx*dx + dz*dz < 0.42f*0.42f) {
                t->alive = 0; s->alive = 0; spawn_boom(t->x, t->z, t->color);
                mote->audio_note(90.0f, 0.9f);
                break;
            }
        }
    }
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (s_state != ST_PLAY) {
        if (mote_just_pressed(in, MOTE_BTN_B)) reset_level();
    } else {
        Tank *p = &s_t[0];
        /* ---- player drive ---- */
        if (p->alive) {
            float turn = 2.4f * dt, spd = 3.2f;
            if (mote_pressed(in, MOTE_BTN_LEFT))  p->yaw -= turn;
            if (mote_pressed(in, MOTE_BTN_RIGHT)) p->yaw += turn;
            float mv = 0;
            if (mote_pressed(in, MOTE_BTN_UP))   mv += spd*dt;
            if (mote_pressed(in, MOTE_BTN_DOWN)) mv -= spd*dt;
            p->x += sinf(p->yaw)*mv; p->z += cosf(p->yaw)*mv;
            collide_circle(&p->x, &p->z, 0.5f);
            int e = nearest_enemy(p);
            if (e >= 0) p->aim = anglerp(p->aim, atan2f(s_t[e].x - p->x, s_t[e].z - p->z), 3.0f*dt);
            if (mote_pressed(in, MOTE_BTN_A)) fire(p);
        }
        /* ---- enemy AI ---- */
        for (int i = 1; i < s_nt; i++) {
            Tank *t = &s_t[i]; if (!t->alive) continue;
            float tx = p->x - t->x, tz = p->z - t->z, dist = sqrtf(tx*tx+tz*tz);
            t->aim = anglerp(t->aim, atan2f(tx, tz), 1.8f*dt);     /* track the player */
            t->think_cd -= dt;
            if (t->think_cd <= 0) { t->wander = mote_randf(-1.0f, 1.0f); t->think_cd = mote_randf(0.6f, 1.4f); }
            float want = atan2f(tx, tz) + t->wander + (dist < 3.0f ? 1.4f : 0.0f);  /* approach, but strafe / back off when close */
            t->yaw = anglerp(t->yaw, want, 1.6f*dt);
            float mv = (dist > 2.0f ? 1.7f : 0.8f) * dt;
            t->x += sinf(t->yaw)*mv; t->z += cosf(t->yaw)*mv;
            collide_circle(&t->x, &t->z, 0.5f);
            if (p->alive && dist < 8.5f) fire(t);
        }
        for (int i = 0; i < s_nt; i++) { if (s_t[i].fire_cd > 0) s_t[i].fire_cd -= dt; if (s_t[i].recoil > 0) s_t[i].recoil -= dt*4.0f; }
        step_shells(dt);
        /* win / lose */
        int enemies = 0; for (int i = 1; i < s_nt; i++) if (s_t[i].alive) enemies++;
        if (!s_t[0].alive) s_state = ST_LOSE;
        else if (enemies == 0) s_state = ST_WIN;
    }
    for (int i = 0; i < MAXB; i++) if (s_b[i].life > 0) s_b[i].life -= dt;

    /* ---- camera: fixed angled overview of the arena ---- */
    Vec3 eye = v3(0, 8.0f, -7.0f), tgt = v3(0, 0, 0.2f);
    Mat3 cam = mote_camera_look(eye, tgt);
    mote->scene_camera(&cam, eye, 50.0f);

    /* floor + 4 border walls + blocks */
    mote_draw(mote, s_floor, v3(0,0,0));
    for (int i = 0; i < 4; i++) mote_draw(mote, s_wall[i], v3(0,0,0));
    for (int i = 0; i < s_nblocks; i++)
        mote_draw(mote, s_block_mesh[i], v3(s_blocks[i].cx, 0, s_blocks[i].cz));
    /* tanks */
    for (int i = 0; i < s_nt; i++) {
        Tank *t = &s_t[i]; if (!t->alive) continue;
        Mat3 body = m3_identity(); m3_rotate_local(&body, 1, t->yaw);
        MoteRigLocal loc[P_COUNT];
        mote_rig_eval(&s_tank, 0, loc);
        loc[P_TURRET].rot = mote_quat_axis(v3(0,1,0), t->aim - t->yaw);
        loc[P_BARREL].pos = v3(0, 0, -t->recoil * 0.22f);
        mote_rig_draw_locals_tint(mote, &s_tank, loc, v3(t->x, 0, t->z), body, 1.0f, t->color);
    }
    /* shells + explosions as impostor spheres */
    for (int i = 0; i < MAXS; i++) if (s_s[i].alive)
        mote->scene_add_sphere(v3_sub(v3(s_s[i].x, 0.3f, s_s[i].z), eye), 0.12f, MOTE_RGB565(40,40,46));
    for (int i = 0; i < MAXB; i++) if (s_b[i].life > 0)
        mote->scene_add_sphere(v3_sub(v3(s_b[i].x, s_b[i].y, s_b[i].z), eye), s_b[i].r, s_b[i].col);
}

static void g_overlay(uint16_t *fb) {
    int enemies = 0; for (int i = 1; i < s_nt; i++) if (s_t[i].alive) enemies++;
    mote_textf(mote, fb, 4, 4, MOTE_RGB565(220,228,240), "ENEMIES %d", enemies);
    if (s_state == ST_WIN)  { mote->text_2x(fb, "CLEARED!", 30, 54, MOTE_RGB565(120,235,120)); mote->text(fb, "B  NEXT", 46, 80, MOTE_RGB565(210,220,235)); }
    if (s_state == ST_LOSE) { mote->text_2x(fb, "DESTROYED", 18, 54, MOTE_RGB565(245,120,110)); mote->text(fb, "B  RETRY", 46, 80, MOTE_RGB565(210,220,235)); }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 600, .max_spheres = 64, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
