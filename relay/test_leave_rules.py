#!/usr/bin/env python3
"""test_leave_rules.py -- who leaves, who is held and who is charged (7.6.9; Mark:
"only actively selecting to end game or leave the club should ever leave a
club/room"; "this is SO critical to get right as the integrity of the game rests
entirely on things like this"). The relay's half of the agreed rules, one check a
row (the rows are the memory note online-leave-drop-rules):

  3   a room of two, LEAVE mid-frame: LEFT at once, the walk-out the leaver's
  4   a room of two between frames of a match still going: a drop is held
  5   ...gone for good between frames: the match to the one who stayed if ahead
      (at the score as it stands), nothing if level or behind
  8   a room of three, at the table, LEAVE mid-frame: the walk-out the leaver's
  12  a room of three, in the queue, LEAVE: LEFT at once, nothing written
  15  the host in the queue, not back in time: no walk-out for the two at the table
  16  the host at the table, LEAVE mid-frame: the room closes, the walk-out the host's
  17  the host in the queue, LEAVE: the room closes, NOBODY's walk-out (G9YJ)

A real relay on a local port with short clocks; temp folders only.

    python3 relay/test_leave_rules.py
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port, conn  # noqa: E402
from test_resume import P, GID  # noqa: E402

ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


NAMES = ["HOSTY", "GUESTY"]


def start(me, frame=1, best_of=1, names=NAMES):
    return "STARTED " + json.dumps({"v": 1, "t": "start", "frame": frame, "kind": 7, "me": me, "best_of": best_of,
                                    "names": names, "game": "STRAIGHT POOL"})


def result(me, t, frame, winner, frames, best_of, match_winner, names=NAMES):
    side = {"shots": 9, "pot_shots": 8, "potted": 8, "fouls": 0, "best": 8, "breaks": [8]}
    r = {"v": 1, "t": t, "game": "STRAIGHT POOL", "kind": 7, "mode": 0, "best_of": best_of, "frame": frame,
         "winner": winner, "match_winner": match_winner, "bnr": -1, "golden": 0, "me": me, "score": [8, 2],
         "frames": frames, "names": names, "sides": [dict(side), dict(side, potted=2, best=2, breaks=[])], "ver": "7.6.9"}
    return "RESULT " + json.dumps(r)


def lines(store, f, room):
    p = os.path.join(store, f)
    return [l for l in (json.loads(x) for x in open(p)) if l.get("room") == room] if os.path.exists(p) else []


def two(port, code):
    h = P(port, f"MOTE2 ROOMN {GID} HOST {code} PRIV 2 TEST", "711")
    g = P(port, f"MOTE2 ROOMN {GID} JOIN {code}", "722")
    h.say("RESUME"); g.say("RESUME")
    h.say("START"); time.sleep(0.2)
    h.say("PROOF 711 desk711"); g.say("PROOF 722 desk722"); time.sleep(0.3)
    return h, g


def three(port, code, at_table):
    """host 0, players 1 and 2; at_table: the two places that start the frame"""
    h = P(port, f"MOTE2 ROOMN {GID} HOST {code} PRIV 3 TEST", "711")
    g = P(port, f"MOTE2 ROOMN {GID} JOIN {code}", "722")
    q = P(port, f"MOTE2 ROOMN {GID} JOIN {code}", "733")
    ps = {0: h, 1: g, 2: q}
    for x in ps.values():
        x.say("RESUME")
    h.say("PLAYING"); time.sleep(0.2)
    h.say("PROOF 711 desk711"); g.say("PROOF 722 desk722"); q.say("PROOF 733 desk733"); time.sleep(0.3)
    nm = [f"P{at_table[0]}", f"P{at_table[1]}"]
    ps[at_table[0]].say(start(0, names=nm)); ps[at_table[1]].say(start(1, names=nm)); time.sleep(0.3)
    return h, g, q


def main():
    store = tempfile.mkdtemp()
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--tourn-trust", "--no-voice", "--resume-wait", "3", "--forfeit-wait", "2",
                              "--room-backlog", "6000"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)

        # ---- 3: a room of two, LEAVE mid-frame
        h, g = two(port, "LV3")
        h.say(start(0)); g.say(start(1)); time.sleep(0.3)
        g.say("LEAVE"); time.sleep(0.1); g.cut(); time.sleep(0.4)
        check("LEFT 1" in h.ctrl and not any(l.startswith("AWAY") for l in h.ctrl), f"3: LEAVE mid-frame: LEFT at once, not held: {h.ctrl[-2:]}")
        h.say(result(0, "frame", 1, 0, [1, 0], 1, 0)); time.sleep(2.6)
        f = lines(store, "results_forfeits.jsonl", "LV3")
        check(f and f[0].get("left") == "GUESTY" and f[0]["result"]["winner"] == 0, f"3: the walk-out is the leaver's: {[(x.get('left'), x['result'].get('winner')) for x in f]}")
        h.cut(); time.sleep(0.3)

        # ---- 4: a room of two, between frames of a match still going: a drop is held
        h, g = two(port, "LV4")
        h.say(start(0, 1, 3)); g.say(start(1, 1, 3)); time.sleep(0.3)
        h.say(result(0, "frame", 1, 0, [1, 0], 3, -1)); g.say(result(1, "frame", 1, 0, [1, 0], 3, -1)); time.sleep(0.4)
        g.cut(); time.sleep(0.4)
        check(any(l.startswith("AWAY 1 ") for l in h.ctrl), f"4: dropped between frames of a match still going: held, AWAY 1: {h.ctrl[-2:]}")
        g2 = P(port, f"MOTE2 ROOMN {GID} REJOIN LV4 722 desk722", "722"); time.sleep(0.3)
        check(g2.seat == "SEAT 1 2 BACK", f"4: ...and back: {g2.seat}")
        h.cut(); g2.cut(); time.sleep(0.3)

        # ---- 5: gone between frames -- ahead, level/behind, and not back in time
        for code, frames, who_goes, note, want in (("LV5", [1, 0], "g", "the one who stayed ahead 1-0: the match is theirs", True),
                                                   ("LV6", [0, 1], "g", "the one who stayed behind 0-1: no winner", False),
                                                   ("LV7", [1, 1], "g", "level 1-1: no winner", False)):
            h, g = two(port, code)
            w = 0 if frames[0] >= frames[1] else 1
            h.say(start(0, 1, 5)); g.say(start(1, 1, 5)); time.sleep(0.3)
            h.say(result(0, "frame", 1, w, frames, 5, -1)); g.say(result(1, "frame", 1, w, frames, 5, -1)); time.sleep(0.4)
            g.say("LEAVE"); time.sleep(0.1); g.cut(); time.sleep(0.5)
            ms = [l for l in lines(store, "results.jsonl", code) if l.get("result", {}).get("t") == "match"]
            if want:
                check(ms and ms[0].get("left") == "GUESTY" and ms[0]["result"]["match_winner"] == 0 and ms[0]["result"]["frames"] == frames,
                      f"5: LEAVE between frames, {note}: {[(x.get('left'), x['result'].get('match_winner'), x['result'].get('frames')) for x in ms]}")
            else:
                check(not ms, f"5: LEAVE between frames, {note}: {len(ms)} match lines")
            h.cut(); time.sleep(0.3)
        h, g = two(port, "LV8")
        h.say(start(0, 1, 5)); g.say(start(1, 1, 5)); time.sleep(0.3)
        h.say(result(0, "frame", 1, 0, [1, 0], 5, -1)); g.say(result(1, "frame", 1, 0, [1, 0], 5, -1)); time.sleep(0.4)
        g.cut(); time.sleep(4.0)
        ms = [l for l in lines(store, "results.jsonl", "LV8") if l.get("result", {}).get("t") == "match"]
        check("GONE 1" in h.ctrl and ms and ms[0]["result"]["match_winner"] == 0,
              f"5: dropped between frames and not back in time, the one who stayed ahead: GONE and the match theirs: {h.ctrl[-2:]}, {len(ms)}")
        h.cut(); time.sleep(0.3)

        # ...and a later frame DISPUTED: the score is not known, so no match line
        h, g = two(port, "LVE")
        h.say(start(0, 1, 5)); g.say(start(1, 1, 5)); time.sleep(0.3)
        h.say(result(0, "frame", 1, 0, [1, 0], 5, -1)); g.say(result(1, "frame", 1, 0, [1, 0], 5, -1)); time.sleep(0.4)
        h.say(start(0, 2, 5)); g.say(start(1, 2, 5)); time.sleep(0.3)
        h.say(result(0, "frame", 2, 0, [2, 0], 5, -1)); g.say(result(1, "frame", 2, 1, [1, 1], 5, -1)); time.sleep(0.4)
        g.say("LEAVE"); time.sleep(0.1); g.cut(); time.sleep(0.5)
        ms = [l for l in lines(store, "results.jsonl", "LVE") if l.get("result", {}).get("t") == "match"]
        check(not ms, f"5: a later frame disputed, then LEAVE between frames: no match line on a score not known ({len(ms)})")
        h.cut(); time.sleep(0.3)

        # ---- 8: a room of three, at the table, LEAVE mid-frame
        h, g, q = three(port, "LV9", (0, 1))
        g.say("LEAVE"); time.sleep(0.1); g.cut(); time.sleep(0.4)
        check("LEFT 1" in h.ctrl and not any(l.startswith("AWAY") for l in h.ctrl), f"8: at the table, LEAVE mid-frame: LEFT at once: {h.ctrl[-2:]}")
        h.say(result(0, "frame", 1, 0, [1, 0], 1, 0, ["P0", "P1"])); time.sleep(2.6)
        f = lines(store, "results_forfeits.jsonl", "LV9")
        check(f and f[0].get("left") == "P1" and f[0]["result"]["winner"] == 0, f"8: the walk-out is the leaver's: {[(x.get('left'), x['result'].get('winner')) for x in f]}")
        h.cut(); q.cut(); time.sleep(0.3)

        # ---- 12: a room of three, in the queue, LEAVE
        h, g, q = three(port, "LVA", (0, 1))
        q.say("LEAVE"); time.sleep(0.1); q.cut(); time.sleep(0.4)
        check("LEFT 2" in h.ctrl and not any(l.startswith("AWAY") for l in h.ctrl), f"12: in the queue, LEAVE: LEFT at once: {h.ctrl[-2:]}")
        time.sleep(2.5)
        check(not lines(store, "results_forfeits.jsonl", "LVA"), "12: ...and nothing written")
        h.cut(); g.cut(); time.sleep(0.3)

        # ---- 15: the host in the queue, not back in time: no walk-out for the two at the table
        h, g, q = three(port, "LVB", (1, 2))
        h.cut(); time.sleep(4.5)
        check(any(l.startswith("AWAY 0 ") for l in g.ctrl) and "GONE 0" in g.ctrl, f"15: the host dropped from the queue: held, then GONE 0: {g.ctrl[-3:]}")
        time.sleep(2.5)
        check(not lines(store, "results_forfeits.jsonl", "LVB"), "15: ...and no walk-out for either of the two at the table")
        g.cut(); q.cut(); time.sleep(0.3)

        # ---- 16: the host at the table, LEAVE mid-frame: closed, the host's walk-out
        h, g, q = three(port, "LVC", (0, 1))
        h.say("LEAVE"); time.sleep(0.1); h.cut(); time.sleep(0.4)
        check("CLOSED" in g.ctrl, f"16: the host at the table leaves: the room closes: {g.ctrl[-2:]}")
        g.say(result(1, "frame", 1, 1, [0, 1], 1, 1, ["P0", "P1"])); time.sleep(2.6)
        f = lines(store, "results_forfeits.jsonl", "LVC")
        check(f and f[0].get("left") == "P0", f"16: the walk-out is the host's: {[x.get('left') for x in f]}")
        g.cut(); q.cut(); time.sleep(0.3)

        # ---- 17: the host in the queue, LEAVE: closed, NOBODY's walk-out (G9YJ, 2026-10-18)
        h, g, q = three(port, "LVD", (1, 2))
        h.say("LEAVE"); time.sleep(0.1); h.cut(); time.sleep(0.4)
        check("CLOSED" in g.ctrl, f"17: the host in the queue leaves: the room closes: {g.ctrl[-2:]}")
        g.cut(); q.cut(); time.sleep(2.6)
        check(not lines(store, "results_forfeits.jsonl", "LVD"), "17: ...and nobody at the table is charged with the walk-out")
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0].decode("ascii", "replace")
        if bad:
            print(out[-5000:])
        shutil.rmtree(store)
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
