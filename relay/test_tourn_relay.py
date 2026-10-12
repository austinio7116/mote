#!/usr/bin/env python3
"""test_tourn_relay.py -- the club's tournaments through a real relay on a
local port (never the server): an admin makes one a minute ahead, four
entrants sign up and walk into its room (checking in), the draw and the calls
come to the room, each called pair meets in its table's room, their two games
agree the frames, and a champion is announced. Also: a wrong admin token,
an entrant on the wrong version, a stranger turned away from the room, and
the room kept out of the club listing. About 75 seconds (the start is a
minute ahead -- nothing can be made sooner).

    python3 relay/test_tourn_relay.py
"""
import json, os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port, conn, readline, send_frame, recv_frame  # noqa: E402

GID = "1129661807"
TOKEN = "desk-admin-token"
ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


def ask(port, line):
    s = conn(port, line)
    out = []
    while True:
        l = readline(s)
        out.append(l)
        if l in ("END", "") or not l.startswith(("NOW", "T ")):
            break
    s.close()
    return out


def parse_state(lines):
    """the TS lines back into the few things the test asks of them"""
    st = {"draw": {}, "matches": []}
    for l in lines:
        w = l.split(" ")
        if w[1] == "BEGIN":
            st["status"] = w[3]
        elif w[1] == "DRAW":
            st["draw"][w[2]] = " ".join(w[4:])
        elif w[1] == "M":
            st["matches"].append({"mid": w[2], "state": w[7]})
    return st


class Member:
    """one entrant in the tournament's room, collecting its control lines"""
    def __init__(self, port, tid, uid):
        self.uid = uid
        self.s = conn(port, f"MOTE2 CLUB {GID} TOURN {tid} {uid} desknonce{uid}")
        self.seat = readline(self.s)
        self.lines = []
        self.state = None
        self.ts = []
        self.s.settimeout(None)
        if self.seat.startswith("SEAT"):
            threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        try:
            while True:
                frm, _, payload = recv_frame(self.s)
                if frm == 0xFE:
                    l = payload.decode("ascii", "replace")
                    if l.startswith("TS "):
                        self.ts.append(l)
                        if l == "TS END":
                            self.state, self.ts = parse_state(self.ts), []
                    else:
                        self.lines.append(l)
        except Exception:
            pass


def table(port, code, uid, host):
    """one player's game at a called table: HOST or JOIN the code, then PROOF"""
    s = conn(port, f"MOTE2 ROOMN {GID} {'HOST ' + code + ' PRIV 2 TOURNAMENT' if host else 'JOIN ' + code}")
    seat = readline(s)
    send_frame(s, 0xFE, f"PROOF {uid} desknonce{uid}".encode())
    return s, seat


def result(s, me, frame, winner):
    side = {"shots": 5, "pot_shots": 2, "potted": 3, "fouls": 0, "best": 3, "breaks": []}
    r = {"v": 1, "t": "frame", "game": "UK 8-BALL", "kind": 0, "mode": 0, "best_of": 3, "frame": frame,
         "winner": winner, "match_winner": -1, "bnr": -1, "golden": 0, "me": me, "score": [1, 0], "frames": [0, 0],
         "names": ["A", "B"], "sides": [dict(side), dict(side)], "ver": "7.5"}
    send_frame(s, 0xFE, ("RESULT " + json.dumps(r)).encode())


def main():
    store = tempfile.mkdtemp()
    tokf = os.path.join(store, "admin.token")
    open(tokf, "w").write(TOKEN + "\n")
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--admin-token-file", tokf, "--tourn-trust", "--no-voice"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)
        start = int(time.time()) + 62
        spec = f"{start} 0 0 4 3 3 7.5 {start - 1} DESK KNOCKOUT"     # start kind rules size best_of final ver close title
        check(ask(port, f"MOTE2 TOURN {GID} CREATE wrong-token {spec}")[0] == "CREATE DENIED", "a wrong admin token makes nothing")
        ans = ask(port, f"MOTE2 TOURN {GID} CREATE {TOKEN} {spec}")[0]
        check(ans.startswith("CREATE OK"), f"the admin makes a tournament: {ans}")
        tid = int(ans.split()[2])
        uids = ["201", "202", "203", "204"]
        for i, u in enumerate(uids):
            a = ask(port, f"MOTE2 TOURN {GID} ENTER {tid} {u} desknonce{u} 7.5 DESK{i + 1}")[0]
            check(a == "ENTER OK", f"entrant {i + 1} signs up: {a}")
        check(ask(port, f"MOTE2 TOURN {GID} ENTER {tid} 299 desknonce299 7.4.5 OLDGAME")[0] == "ENTER VERSION", "an entrant on 7.4.5 is turned away")
        lst = ask(port, f"MOTE2 TOURN {GID} LIST")
        t = json.loads(lst[1][2:])
        check(lst[0].startswith("NOW ") and t["entrants"] == ["DESK1", "DESK2", "DESK3", "DESK4"] and "201" not in lst[1],
              "the listing shows the four names, no accounts")
        time.sleep(2.5)                       # the clock opens the room (the start is under 15 minutes away)
        stranger = conn(port, f"MOTE2 CLUB {GID} TOURN {tid} 298 desknonce298")
        check(readline(stranger) == "NOTYOU", "an account not entered is not let in")
        stranger.close()
        ms = [Member(port, tid, u) for u in uids]
        check(all(m.seat.startswith("SEAT") for m in ms), f"all four walk into the room: {[m.seat for m in ms]}")
        clubs = ask(port, f"MOTE2 LIST {GID}")
        check(not any("TOURNAMENT" in l or "CLUB 0" in l for l in clubs), "the tournament's room is not in the club listing")
        print(f"         waiting for the start ({start - int(time.time())} s)...")
        while time.time() < start + 4:
            time.sleep(0.5)
        calls = [l for l in ms[0].lines if l.startswith("T CALL ")]
        check(len(calls) == 2 and all(m.state and m.state["status"] == "running" for m in ms),
              f"at the start everyone hears the draw and two calls: {calls}")
        state = ms[0].state
        draw = {int(n): nm for n, nm in state["draw"].items()}
        num = {f"DESK{i + 1}": u for i, u in enumerate(uids)}
        uid_of = lambda n: num[draw[n]]
        yous = {}
        for i, m in enumerate(ms):
            y = [l for l in m.lines if l.startswith("T YOU ")]
            yous[i] = int(y[-1].split()[2]) if y else 0
        check(all(draw.get(yous[i]) == f"DESK{i + 1}" for i in range(4)), f"each player is told their own draw number: {yous}")

        def play_called(call):
            _, _, mid, tbl, a, b, code = call.split()
            ua, ub = uid_of(int(a)), uid_of(int(b))
            sa, _ = table(port, code, ua, True)
            time.sleep(0.2)
            sb, seat_b = table(port, code, ub, False)
            time.sleep(0.5)
            for f in (1, 2):                  # a wins two frames, both games say so
                result(sa, 0, f, 0); result(sb, 1, f, 0)
                time.sleep(0.3)
            return sa, sb

        socks = []
        for c in calls:
            socks += play_called(c)
        time.sleep(3)
        won = [l for l in ms[0].lines if l.startswith("T WON ")]
        check(len(won) == 2, f"both semi-finals are won on agreed frames: {won}")
        fin = [l for l in ms[0].lines if l.startswith("T CALL R2M1")]
        check(len(fin) == 1 and fin[0].split()[3] == "1", f"the final is called to table 1: {fin}")
        for s in socks:
            s.close()
        if fin:
            socks = play_called(fin[0])
            time.sleep(3)
        champ = [l for l in ms[0].lines if l.startswith("T CHAMPION ")]
        check(len(champ) == 1 and ms[0].state["status"] == "done", f"the champion is announced to the room: {champ}")
        rec = open(os.path.join(store, "tournaments.jsonl")).read().splitlines()
        check(any('"e": "champion"' in l for l in rec), f"tournaments.jsonl holds the night ({len(rec)} lines)")
    finally:
        relay.terminate()
        try:
            out = relay.communicate(timeout=5)[0].decode("ascii", "replace")
        except Exception:
            out = ""
        if bad:
            print(out[-3000:])
        shutil.rmtree(store)
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
