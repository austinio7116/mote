/*
 * CueVR — online play, with the same options the Mote lobby offers.
 *
 * The transport is studio/link_net.c: a non-blocking TCP pipe with either
 * zero-config LAN discovery or an Internet relay, pumped once a frame. CueVR
 * cannot call MoteApi's net_lobby() — that belongs to the Mote OS runtime and
 * this is a standalone APK with its own main() — but link_net sits BELOW the
 * lobby and is ordinary C, so it links straight in.
 *
 * Worth being clear about which side of the fence this is. Mote's on-device
 * lobby proxies everything through the Studio over the MN1 control protocol,
 * because a Thumby has no IP stack. A Quest has one. So CueVR does not imitate
 * the device lobby, it takes the STUDIO's role and drives link_net itself — which
 * is why the options can match exactly rather than approximately:
 *
 *   LAN       Host · Join            (UDP discovery, so no address to type)
 *   Internet  Quick Match · Host Room · Join Code · Browse Rooms
 *
 * Codes are four characters from the same alphabet the device lobby uses, so a
 * code read off a Thumby screen can be typed into a headset and vice versa.
 * Browse exists because picking a room from a list beats spelling one out with a
 * thumbstick, and Quick Match exists because most of the time you just want an
 * opponent.
 *
 * The match model is deterministic lockstep, which the physics makes nearly free:
 * cue_physics.c integrates at a fixed 2 kHz from a fixed state, so both machines
 * applying the same strike to the same table get the same result to the bit. Only
 * the STRIKE crosses the wire — direction, speed, tip offsets, elevation — never
 * ball positions. Two dozen bytes a shot, no drift, and a dropped link loses the
 * match rather than silently corrupting it.
 */
#ifndef CUEVR_NET_H
#define CUEVR_NET_H

#include <stdint.h>

enum { CUEVR_NET_OFF = 0, CUEVR_NET_SEARCHING, CUEVR_NET_LIVE, CUEVR_NET_LOST };

/* The alphabet room codes are drawn from — the device lobby's, deliberately.
 * No vowels and no 0/O/1/I, because these get read aloud and squinted at. */
#define CUEVR_CODE_ALPHABET "23456789BCDFGHJKLMNPQRSTVWXYZ"
#define CUEVR_CODE_LEN 4

/* Matches CUE_MAX_BALLS; kept here so the wire format does not include a game
 * header just to learn its own size. */
#define CUEVR_NET_MAXBALLS 22

/* One shot, as it goes over the wire. Fixed width; both ends are IEEE-754 and
 * the same endianness, so this needs no serialiser. */
typedef struct {
    float dirx, dirz;      /* aim, table space (normalised) */
    float speed;
    float side, vert;      /* tip offsets in ball radii */
    float elev;            /* cue elevation, radians */
    /* WHERE THE WHITE WAS. Lockstep only works from the same starting position,
     * and ball in hand is a position only ONE side knows: the striker walks the
     * cue ball wherever they like and the other end never hears about it. So
     * every break — every frame begins with one — was played from two different
     * spots, and the far end watched the same aim and speed sail past a pack it
     * was never pointed at. Sent with every shot, not just the placed ones,
     * because it costs eight bytes and removes the question. */
    float cuex, cuez;      /* cue ball, table space, at the moment of the strike */
    /* WHAT THEY DECLARED. Snooker scores and fouls against the ball the striker
     * NOMINATED, and nomination happens by aiming — a purely local act the far
     * end never saw. Two ends judging the same shot against different nominated
     * colours award different points and different fouls, and from there the
     * scores drift apart while the balls still agree, which is the confusing
     * kind of desync. Rides with the shot because it is part of the shot. */
    int   nominated;       /* colour value 2..7, or 0 */
    int   free_ball_id;    /* the ball named as a free ball, or 0 */
} CueVrNetShot;

/* What each end tells the other once, when the room goes live: which game the
 * HOST is playing and which cue each is holding. Neither was exchanged at all —
 * both ends called start_frame() on their own menu selection, so two players who
 * had not happened to pick the same game were racking different tables. */
typedef struct {
    int  seat;             /* 0 = host */
    int  kind;             /* CueGameKind — the host's is the one that counts */
    int  cue_idx;          /* the cue they are holding, so it can be drawn */
    /* WHO BREAKS. It is drawn at random now rather than always being seat 0, so
     * both ends have to be told the same answer before either racks — it decides
     * the whole frame and there is no later packet that could correct it. The
     * host's is the one that counts, like the kind. */
    int  first;
    /* HOW LONG THE MATCH IS. Each end was setting this from its OWN menu, so a
     * host on best of 5 and a joiner on best of 1 played the same frames and
     * disagreed about whether the match had ended — one went back to the menu
     * while the other racked. The host's number, like everything else here. */
    int  best_of;
} CueVrNetHello;

/* WHERE THEIR CUE IS, live. Not part of the lockstep — nothing here changes the
 * simulation — purely so the other player is a person standing at the table
 * holding a stick rather than a scoreboard entry that occasionally moves the
 * balls. Table space, so it lands correctly however each end has placed and
 * turned its own table in its own room. Sent at a fraction of frame rate and
 * dropped if it stops arriving. */
typedef struct {
    float tipx, tipy, tipz;
    float bttx, btty, bttz;
} CueVrNetPose;

/* A CHOICE, rather than a stroke. After a foul the fouled-AGAINST player picks
 * from the decision list, and that pick changes the rules state on both ends —
 * so it has to cross the wire like a shot does. Without it the deciding player
 * carried on and the other sat on a pending decision for ever. Also carries a
 * concession, which is the other thing one player can do that the other has to
 * be told about. */
typedef struct {
    int code;              /* CUE_DEC_*, or CUEVR_NET_CONCEDE */
    int who;               /* the seat that made it */
} CueVrNetCall;
#define CUEVR_NET_CONCEDE 100

void cuevr_net_send_call(const CueVrNetCall *c);
int  cuevr_net_recv_call(CueVrNetCall *out);

/* THE TABLE, AS THE HOST HAS IT, after every shot.
 *
 * Lockstep assumes both machines compute the same answer from the same numbers,
 * and floating point across two different chips, drivers and compilers does not
 * promise that. The physics runs at 2 kHz with dozens of contacts a shot, so a
 * single last-place bit in one collision compounds into a different table — and
 * it would do it SILENTLY and permanently, which is the failure nobody notices
 * until the frame makes no sense.
 *
 * So the host says where everything is at the end of every shot and the other
 * end takes it. Not a checksum and a repair request: the full layout is about
 * two hundred bytes once a shot, which is nothing over this link, and it cannot
 * fail to converge the way a request-and-wait can. Cheap enough to do always,
 * so it never needs deciding whether this is the moment to bother.
 *
 * The scores ride with it because a divergence in the balls becomes a
 * divergence in the score the instant one end thinks a ball went in.
 *
 * AND THE RULES GO WHOLE, not field by field. This carried nine hand-picked
 * numbers — score, turn, target, seq, reds left, nomination — and every one of
 * the ones NOT on that list was a silent desync waiting to be found: which
 * player is on lows and which on highs, whether the group is still open, the UK
 * two-shot carry, the 9-ball consecutive-foul count, the free-ball award, the
 * called-miss tallies, the frames won, whether the match is over. Picking
 * fields by hand means picking wrongly at some point, and the failure is
 * invisible until the frame stops making sense.
 *
 * So the whole CueRules crosses as an opaque blob. It costs about three hundred
 * bytes once a shot, which is nothing over this link, and there is no longer
 * any field that CAN be forgotten. It is opaque because this header is compiled
 * into the Mote shell, which does not have the game's headers on its include
 * path; the app memcpys its own struct in and out and static-asserts that it
 * fits. */
#define CUEVR_NET_RULES_MAX 512
typedef struct {
    uint8_t  n;                      /* balls in play */
    uint8_t  on[CUEVR_NET_MAXBALLS];
    float    x[CUEVR_NET_MAXBALLS];
    float    z[CUEVR_NET_MAXBALLS];
    uint16_t rules_len;              /* 0 if the sender had none to give */
    uint8_t  rules[CUEVR_NET_RULES_MAX];
} CueVrNetState;

void cuevr_net_send_state(const CueVrNetState *st);
int  cuevr_net_recv_state(CueVrNetState *out);

void cuevr_net_send_pose(const CueVrNetPose *p);
/* 1 if a pose has arrived recently enough to draw. */
int  cuevr_net_peer_pose(CueVrNetPose *out);

/* The last hello received, if any. Returns 1 when one has arrived. */
int  cuevr_net_peer(CueVrNetHello *out);
/* Ours, sent when the room goes live. */
void cuevr_net_set_hello(int kind, int cue_idx, int first, int best_of);

/* ---- starting a session ------------------------------------------------- */
void cuevr_net_lan_host(void);
void cuevr_net_lan_join(void);              /* discovers on the LAN, no address */
void cuevr_net_quick(void);
void cuevr_net_host(const char *code);      /* public room under `code` */
void cuevr_net_join(const char *code);
void cuevr_net_stop(void);

/* Browsing public rooms. link_net_list blocks for ~2.5 s, which on the frame
 * thread would stall the compositor and make the headset lurch, so it runs on its
 * own thread: kick it off, poll, then read. */
void cuevr_net_browse_start(void);
int  cuevr_net_browse_done(void);           /* 1 once the list is ready */
int  cuevr_net_browse_count(void);
const char *cuevr_net_browse_code(int i);   /* 4 chars, NUL-terminated */
const char *cuevr_net_browse_label(int i);

/* ---- while it runs ------------------------------------------------------ */
void cuevr_net_task(void);                  /* once a frame */
int  cuevr_net_state(void);                 /* CUEVR_NET_* */
int  cuevr_net_me(void);                    /* local player index, 0 breaks */
const char *cuevr_net_info(void);           /* a line for the HUD */
const char *cuevr_net_code(void);           /* the code we are hosting, or "" */

void cuevr_net_send_shot(const CueVrNetShot *s);
int  cuevr_net_recv_shot(CueVrNetShot *s);  /* 1 if one arrived */

/* A fresh random code to host under. */
void cuevr_net_make_code(char *out5);

#endif /* CUEVR_NET_H */
