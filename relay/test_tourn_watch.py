#!/usr/bin/env python3
"""test_tourn_watch.py -- a tournament's watchers (7.5) are unseen, unnamed and
silent: nobody in the room is told one came or went, nothing a watcher sends
reaches anybody (a pose, a name), no player's voice reaches a watcher, and a
watcher sees everything else -- the players' frames, their names, the state.
A real relay on a local port, never the server.

    python3 relay/test_tourn_watch.py
"""
import asyncio, json, os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port, conn, readline, send_frame, recv_frame  # noqa: E402
import mote_relay  # noqa: E402

GID = "1129661807"
TOKEN = "desk-admin-token"
ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


def ask(port, line):
    s = conn(port, line)
    l = readline(s)
    s.close()
    return l


class Seat:
    """a connection into the tournament's room, keeping everything it is sent"""
    def __init__(self, port, tid, who):
        self.s = conn(port, f"MOTE2 CLUB {GID} TOURN {tid} {who}" + ("" if who == "WATCH" else f" desknonce{who}"))
        self.seat = readline(self.s)
        self.k = int(self.seat.split()[1]) if self.seat.startswith("SEAT") else -1
        self.ctrl, self.frames = [], []
        self.s.settimeout(None)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        try:
            while True:
                frm, _, payload = recv_frame(self.s)
                if frm == 0xFE:
                    self.ctrl.append(payload.decode("ascii", "replace"))
                elif frm != 0xFD:
                    self.frames.append((frm, payload))
        except Exception:
            pass


def voice_unit():
    """TournClub.voice, straight: a player's packet goes to the other player
    and never to a watcher; a watcher's goes nowhere"""
    async def go():
        club = mote_relay.TournClub(GID, 1)
        ms = {}
        for k in (0, 1, 16):
            m = mote_relay.Member(k, None, None, ("127.0.0.1", 1))
            m.vtok = bytes([k] * 8); m.vtcp = True
            club.members[k] = m; ms[k] = m
        club.voice(0, b"hello", None, 1 << 20)
        club.voice(16, b"from-a-watcher", None, 1 << 20)
        return ms[1].q.qsize(), ms[16].q.qsize(), ms[0].q.qsize(), ms[16].vtok is not None
    return asyncio.run(go())


def main():
    p1, w, p0, tok = voice_unit()
    check(p1 == 1 and w == 0, f"a player's voice reaches the other player and not the watcher ({p1}, {w})")
    check(p0 == 0 and tok, "a watcher's voice reaches nobody, and the watcher keeps its token")

    store = tempfile.mkdtemp()
    tokf = os.path.join(store, "admin.token"); open(tokf, "w").write(TOKEN)
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--admin-token-file", tokf, "--tourn-trust", "--no-voice", "--tourn-watchers", "3", *os.environ.get("TEST_ARGS", "").split()],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)
        start = int(time.time()) + 300
        tid = int(ask(port, f"MOTE2 TOURN {GID} CREATE {TOKEN} {start} 0 0 4 3 3 7.5 {start - 1} WATCH TEST").split()[2])
        for u in ("301", "302", "303"):
            ask(port, f"MOTE2 TOURN {GID} ENTER {tid} {u} desknonce{u} 7.5 P{u}")
        time.sleep(2.5)                                         # the clock opens the room
        a, b = Seat(port, tid, "301"), Seat(port, tid, "302")
        send_frame(a.s, 0xFE, b"NAME ALICE"); send_frame(b.s, 0xFE, b"NAME BOB")
        time.sleep(0.4)
        w = Seat(port, tid, "WATCH")
        time.sleep(0.4)
        check(w.seat.endswith("WATCH") and w.k >= 16, f"a watcher comes in, at a watcher's place: {w.seat}")
        check(any(l.startswith("MEMBERS ") and str(w.k) not in l.split()[1:] for l in w.ctrl) and
              any(l == "NAME 0 ALICE" for l in w.ctrl) and any(l.startswith("TS BEGIN ") for l in w.ctrl) and "TS END" in w.ctrl,
              "the watcher is told the players, their names and the state")
        check(not any(f" {w.k}" in l for l in a.ctrl + b.ctrl), f"no player hears of the watcher: {a.ctrl[-3:]}")
        c = Seat(port, tid, "303")
        time.sleep(0.4)
        mem = next((l for l in c.ctrl if l.startswith("MEMBERS ")), "")
        check(str(w.k) not in mem.split()[1:], f"a player who comes in later is not shown the watcher: {mem}")
        send_frame(a.s, 0xFF, b"pose-from-alice")
        send_frame(w.s, 0xFF, b"pose-from-watcher")
        send_frame(w.s, 0xFE, b"NAME SPY")
        time.sleep(0.4)
        check(any(p == b"pose-from-alice" for _, p in w.frames), "the watcher sees the players' frames")
        check(not any(p == b"pose-from-watcher" for _, p in a.frames + b.frames + c.frames), "nothing the watcher sends reaches a player")
        check(not any("SPY" in l for l in a.ctrl + b.ctrl + c.ctrl), "a watcher cannot give itself a name")
        more = [Seat(port, tid, "WATCH") for _ in range(3)]
        check([x.seat.split()[0] for x in more] == ["SEAT", "SEAT", "FULL"], f"watchers stop at the room's limit: {[x.seat for x in more]}")
        before = len(a.ctrl)
        w.s.shutdown(socket.SHUT_RDWR); w.s.close()
        time.sleep(0.4)
        check(not any(l.startswith("LEFT") for l in a.ctrl[before:]) and "watcher" not in "".join(a.ctrl[before:]),
              f"nobody is told a watcher left: {a.ctrl[before:]}")
        b.s.shutdown(socket.SHUT_RDWR); b.s.close()
        time.sleep(0.4)
        check(any(l == "LEFT 1" for l in a.ctrl), "a player leaving is still told, as in any club")
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0].decode("ascii", "replace")
        if bad:
            print(out[-3000:])
        shutil.rmtree(store)
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
