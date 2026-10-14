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
        if l in ("END", "") or not l.startswith(("NOW", "T ", "TL ", "TE ", "TS ", "MINE ", "TR ")):
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
        self.styles = []
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
                        if l.startswith("TS STYLE "):
                            self.styles.append(l)
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
                              "--store", store, "--admin-token-file", tokf, "--tourn-trust", "--no-voice",
                              # its players send nothing while they wait for the start, as a real
                              # game in the room (a pose fifteen times a second) never does
                              "--club-quiet", "600",
                              "--latest-game", GID],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)
        start = int(time.time()) + 62
        spec = "\n" + json.dumps({"start": start, "kind": 0, "rules": 0, "size": 4, "best_of": 3, "final_best_of": 3, "ver": "7.5",
                                   "close": start - 1, "title": "DESK KNOCKOUT",
                                   "style": {"cloth": 9, "frame": 7, "body": -1, "set": 2, "leather": "6B3A1E", "metal": [0.8, 0.7, 0.3]}})
        check(ask(port, f"MOTE2 TOURN {GID} CREATE wrong-token{spec}")[0] == "CREATE DENIED", "a wrong admin token makes nothing")
        ans = ask(port, f"MOTE2 TOURN {GID} CREATE {TOKEN}{spec}")[0]
        check(ans.startswith("CREATE OK"), f"the admin makes a tournament: {ans}")
        tid = int(ans.split()[2])
        uids = ["201", "202", "203", "204"]
        for i, u in enumerate(uids):
            a = ask(port, f"MOTE2 TOURN {GID} ENTER {tid} {u} desknonce{u} 7.5 DESK{i + 1}")[0]
            check(a == "ENTER OK", f"entrant {i + 1} signs up: {a}")
        # ADMIN EDIT: the rules changed by the admin, its spec on the next line
        check(ask(port, f"MOTE2 TOURN {GID} ADMIN wrong-token EDIT\n" + json.dumps({"tid": tid, "rules": 1}))[0] == "ADMIN DENIED",
              "an edit needs the admin token")
        check(ask(port, f"MOTE2 TOURN {GID} ADMIN {TOKEN} EDIT\n" + json.dumps({"tid": tid, "rules": 1}))[0] == "ADMIN OK",
              "the admin changes the tournament's rules")
        check(ask(port, f"MOTE2 TOURN {GID} LIST")[1].split()[6] == "1", "the listing has the new rules")
        # NO VERSION AT SIGN-UP (2026-10-13): an older game's entry gets as far as FULL
        check(ask(port, f"MOTE2 TOURN {GID} ENTER {tid} 299 desknonce299 7.4.5 OLDGAME")[0] == "ENTER FULL",
              "an entrant on another version is not turned away for it (here: full)")
        check(ask(port, f"MOTE2 TOURN {GID} REFUSED wrong-token")[0] == "REFUSED DENIED", "the refusals need the admin token")
        check(ask(port, f"MOTE2 TOURN {GID} REFUSED {TOKEN}")[:2] == [f"TR {tid} FULL 1", "END"], "the refusal is counted, with its reason")
        # ...and a later game number still sees the tournament
        check(any(l.startswith(f"TL {tid} ") for l in ask(port, f"MOTE2 TOURN {int(GID) + 1} LIST")), "a later CueVR is shown the tournament")
        lst = ask(port, f"MOTE2 TOURN {GID} LIST 201")
        check(lst[0].startswith("NOW ") and lst[1].startswith(f"TL {tid} open ") and lst[1].endswith("DESK KNOCKOUT")
              and [l[len(f"TE {tid} "):] for l in lst if l.startswith("TE ")] == ["DESK1", "DESK2", "DESK3", "DESK4"]
              and f"MINE {tid}" in lst and not any("201" in l.split() for l in lst[1:] if not l.startswith("MINE")),
              f"the listing shows the tournament, its four names and that 201 entered it -- no accounts: {lst}")
        time.sleep(2.5)                       # the clock opens the room (the start is under 15 minutes away)
        # THE VERSION IS ASKED AT THE DOOR: an out-of-date game is turned away
        old = conn(port, f"MOTE2 CLUB {int(GID) - 1} TOURN {tid} 201 desknonce201")
        check(readline(old) == "OLD", "an out-of-date game is turned away at the room's door")
        old.close()
        stranger = conn(port, f"MOTE2 CLUB {GID} TOURN {tid} 298 desknonce298")
        check(readline(stranger) == "NOTYOU", "an account not entered is not let in")
        stranger.close()
        ms = [Member(port, tid, u) for u in uids]
        check(all(m.seat.startswith("SEAT") for m in ms), f"all four walk into the room: {[m.seat for m in ms]}")
        # RELOAD: mote_tourn.py again, under the night, nobody dropped
        check(ask(port, f"MOTE2 TOURN {GID} RELOAD wrong-token")[0] == "RELOAD DENIED", "a reload needs the admin token")
        ans = ask(port, f"MOTE2 TOURN {GID} RELOAD {TOKEN}")[0]
        check(ans.startswith("RELOAD OK"), f"the tournaments' code is reloaded in place: {ans}")
        clubs = ask(port, f"MOTE2 LIST {GID}")
        check(not any("TOURNAMENT" in l or "CLUB 0" in l for l in clubs), "the tournament's room is not in the club listing")
        print(f"         waiting for the start ({start - int(time.time())} s)...")
        while time.time() < start + 4:
            time.sleep(0.5)
        calls = [l for l in ms[0].lines if l.startswith("T CALL ")]
        check(len(calls) == 2 and all(m.state and m.state["status"] == "running" for m in ms),
              f"at the start everyone hears the draw and two calls: {calls}")
        style = [l for l in ms[0].styles]
        check(style and style[-1] == "TS STYLE 9 7 -1 2 6B3A1E CCB24C", f"the tournament's style, hide and fittings reach the room: {style[-1:]}")
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
            # A FINALIST'S LINK DROPS AND THEIR GAME IS STARTED AGAIN: HELD says
            # which tournament and which match, so the game goes back into the
            # tournament's room and to the table, not to a bare match
            _, _, mid, tbl, a, b, code = fin[0].split()
            ua, ub = uid_of(int(a)), uid_of(int(b))
            sa, _ = table(port, code, ua, True); time.sleep(0.2)
            sb, _ = table(port, code, ub, False); time.sleep(0.4)
            st = lambda me: "STARTED " + json.dumps({"v": 1, "t": "start", "frame": 1, "kind": 0, "me": me, "best_of": 3,
                                                     "names": ["A", "B"], "game": "UK 8-BALL"})
            for x, me in ((sa, 0), (sb, 1)):
                send_frame(x, 0xFE, b"RESUME"); send_frame(x, 0xFE, st(me).encode())
            time.sleep(0.4)
            sb.close(); time.sleep(0.5)
            h = ask(port, f"MOTE2 ROOMN {GID} HELD {ub}")[0].split()
            check(len(h) == 6 and h[0] == "HELD" and h[1] == code and h[3] == "T" and h[4] == str(tid) and h[5] == mid,
                  f"the dropped finalist's place is held, and HELD names the tournament and the match: {h}")
            back = conn(port, f"MOTE2 ROOMN {GID} REJOIN {code} {ub} desknonce{ub}")
            check(readline(back).startswith("SEAT 1 2 BACK"), "...and they come back to it")
            for f in (1, 2):
                result(sa, 0, f, 0); result(back, 1, f, 0)
                time.sleep(0.3)
            socks = [sa, back]
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
