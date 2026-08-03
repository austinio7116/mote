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

/* One shot, as it goes over the wire. Fixed width; both ends are IEEE-754 and
 * the same endianness, so this needs no serialiser. */
typedef struct {
    float dirx, dirz;      /* aim, table space (normalised) */
    float speed;
    float side, vert;      /* tip offsets in ball radii */
    float elev;            /* cue elevation, radians */
} CueVrNetShot;

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
