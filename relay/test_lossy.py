#!/usr/bin/env python3
"""
test_lossy.py -- a member who stops reading is not thrown out of a room for
the movement the others keep sending (7.6.5).

0IronSkull0 was dropped from club tables eight seconds after joining, every
time: four people's poses at 72 a second filled the relay's queue for him while
his game paused, and the relay threw him out at the backlog cap. Here three
members play; one stops reading altogether while the other two stream poses
and hands at a headset's rate, well past the cap's worth, with a stroke and a
table among them. That member must still be in the room, and when it reads
again every stroke and table must be there, in order; only movement may be
missing.

    python3 test_lossy.py [--relay path/to/mote_relay.py]

Its own relay on a free localhost port -- never a live one.
"""
import argparse
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import check, start_relay, conn, readline, send_frame, recv_frame, FAILS, GID  # noqa: E402

POSE, HANDS, SHOT, STATE = 0xC3, 0xB3, 0xC5, 0xC1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay", default=os.path.join(HERE, "mote_relay.py"))
    a = ap.parse_args()
    relay, port = start_relay(a.relay, ["--room-backlog", str(256 << 10)])
    try:
        h = conn(port, f"MOTE2 ROOMN {GID} HOST TEST PUB 3 BENCH ROOM")
        check("host gets place 0", readline(h) == "SEAT 0 3")
        p1 = conn(port, f"MOTE2 ROOMN {GID} JOIN TEST")
        check("a player gets place 1", readline(p1) == "SEAT 1 3")
        # the one who stops reading: a small receive buffer, as a headset's
        # filled socket is, so the relay's queue is what fills
        slow = conn(port, f"MOTE2 ROOMN {GID} JOIN TEST", rcvbuf=4096)
        check("the newcomer gets place 2", readline(slow) == "SEAT 2 3")
        send_frame(h, 0xFE, b"START")
        time.sleep(0.3)
        for s in (h, p1):            # what the two who keep reading are sent, drained as it comes
            s.setblocking(False)

        def drain(s):
            try:
                while s.recv(65536):
                    pass
            except (BlockingIOError, socket.timeout):
                pass

        # two players, 1 MB of movement between them -- four times the cap --
        # with a stroke and the table it settled on part way through
        pose = bytes([POSE]) + bytes(130)
        hands = bytes([HANDS]) + bytes(182)
        sent_ordered = []
        N = 30000
        for i in range(N):
            send_frame(h, 0xFF, pose)
            send_frame(p1, 0xFF, pose)
            if i % 3 == 0:
                send_frame(p1, 0xFF, hands)
            if i == N // 3:
                send_frame(h, 0xFF, bytes([SHOT]) + b"stroke-1" + bytes(60))
                sent_ordered.append(SHOT)
            if i == N // 3 + 100:
                send_frame(h, 0xFF, bytes([STATE]) + b"table-1" + bytes(800))
                sent_ordered.append(STATE)
            if i % 50 == 0:
                drain(h); drain(p1)
                time.sleep(0.002)
        send_frame(h, 0xFF, bytes([SHOT]) + b"stroke-2" + bytes(60))
        sent_ordered.append(SHOT)
        time.sleep(1.0)

        # the newcomer reads again: is it still in, and is everything that matters there?
        slow.settimeout(3)
        got, movement = [], 0
        try:
            while True:
                frm, seq, body = recv_frame(slow)
                if frm == 0xFE:
                    continue
                if body[0] in (POSE, HANDS):
                    movement += 1
                else:
                    got.append(body[0])
                if len(got) == len(sent_ordered):
                    break
        except (socket.timeout, EOFError, OSError):
            pass
        check("the member who stopped reading is still in the room", len(got) > 0, f"{len(got)} records read")
        check("every stroke and table reached it, in order", got == sent_ordered, f"{got} v {sent_ordered}")
        check("and some movement was skipped rather than queued", movement < N * 2, f"{movement} movement frames of {N * 2 + N // 3}")
    finally:
        relay.terminate()
        out = relay.stdout.read().decode(errors="replace") if relay.stdout else ""
        skipped = [l for l in out.splitlines() if "movement frames skipped" in l or "backlog" in l]
        for l in skipped[:4]:
            print("   relay:", l)
        check("the relay never dropped anyone for backlog", not any("backlog" in l and "dropped" in l for l in out.splitlines()))
    print(f"\n{'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
