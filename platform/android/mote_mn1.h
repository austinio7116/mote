/*
 * Mote — the MN1 room-action verb table, shared by both places the Android app
 * answers it.
 *
 * MN1 is the control protocol the handheld's lobby (os/mote_lobby.c) speaks to
 * whatever is on the other end of its byte pipe, to get a match set up. On the
 * handheld that peer is a docked Studio; in this app it is answered twice:
 *
 *   · platform/android/mote_link_android.c — for a game running IN the app, over
 *     internal rings, as a non-blocking state machine driven once per frame
 *   · os/android/mote_android_dock.c       — for a real handheld on the USB
 *     cable, on its own thread, where blocking is fine
 *
 * The two have genuinely different concurrency, but the verbs, the link_net
 * calls and the replies are identical, so that part lives here. The one verb
 * that cannot be shared outright is LIST: link_net_list() blocks for ~2.5s,
 * which the dock thread can simply wait for and the per-frame state machine
 * cannot. dispatch() therefore hands LIST back to the caller.
 */
#ifndef MOTE_MN1_H
#define MOTE_MN1_H

/* What the caller must do next. */
enum {
    MOTE_MN1_DONE = 0,   /* fully handled (or rejected) — nothing to wait for */
    MOTE_MN1_PENDING,    /* a room action is in flight: wait for link_net to pair */
    MOTE_MN1_LIST,       /* run link_net_list() and emit ROOM lines + ENDROOMS */
    MOTE_MN1_CANCEL,     /* the peer cancelled: drop any pending action */
};

/* Sink for replies back to the peer. */
typedef void (*MoteMn1Reply)(void *ud, const char *line);

/* Act on one '\n'-stripped MN1 line. `line` must start "MN1 "; anything else is
 * MOTE_MN1_DONE with no reply. `label` is the room label shown in Browse.
 * `room_code` (>= 6 bytes) receives the code for HOST, or "" otherwise. */
int mote_mn1_dispatch(const char *line, const char *label,
                      MoteMn1Reply reply, void *ud, char *room_code);

/* Emit the "CODE LABEL\n"* buffer link_net_list() filled as MN1 ROOM lines,
 * followed by ENDROOMS. `n` is link_net_list's return. */
void mote_mn1_emit_rooms(const char *buf, int n, MoteMn1Reply reply, void *ud);

#endif /* MOTE_MN1_H */
