#!/usr/bin/env python3
"""test_honours.py -- the relay's HONOURS answer (7.4): the club's nine boards
and the completed months' winners, from a made-up results store. Its own temp
store and port; never the server's.

    python3 relay/test_honours.py
"""
import json, os, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 47977


def frame(t, a, b, win, kind, best=(0, 0), bnr=(0, 0), runs=(0, 0), verified=True):
    side = lambda i: {"best": best[i], "shot": {"bnr": bnr[i], "runouts": runs[i]}}
    return {"time": t, "verified": verified, "result": {"t": "frame", "names": [a, b], "winner": win, "kind": kind,
                                                        "sides": [side(0), side(1)]}}


def main():
    store = tempfile.mkdtemp()
    rows = []
    # last month: Ann wins 20 of 22 frames at straight pool (run 40); Bob a 147 at snooker
    for i in range(22):
        rows.append(frame("2026-09-12 20:00:00", "Ann", "Bob", 0 if i < 20 else 1, 7, best=(40 if i == 0 else 5, 3)))
    rows.append(frame("2026-09-20 21:00:00", "Bob", "Cat", 0, 4, best=(147, 20), bnr=(1, 0), runs=(1, 0)))
    # this month: Cat a 98 at 10-red, an unverified frame that must not count
    rows.append(frame(time.strftime("%Y-%m-01 10:00:00", time.gmtime()), "Cat", "Dan", 0, 5, best=(98, 0)))
    rows.append(frame(time.strftime("%Y-%m-01 11:00:00", time.gmtime()), "Eve", "Dan", 0, 4, best=(155, 0), verified=False))
    rows.append(frame("2026-09-21 21:00:00", "Ann & Bob", "Cat & Dan", 0, 1))   # doubles: counts nowhere
    rows.append(frame("2026-09-22 21:00:00", "Dan", "Fay", 0, 23, best=(51, 8)))   # 3-red
    with open(os.path.join(store, "results.jsonl"), "w") as f:
        for r in rows: f.write(json.dumps(r) + "\n")
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--port", str(PORT), "--store", store],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                s = socket.create_connection(("127.0.0.1", PORT), timeout=2); break
            except OSError:
                time.sleep(0.1)
        s.sendall(b"MOTE2 HONOURS 1129661807\n"); buf = b""
        while b"END\n" not in buf:
            d = s.recv(8192)
            if not d: break
            buf += d
        lines = buf.decode().splitlines()
    finally:
        p.terminate()
    ok = 0; bad = 0
    def check(cond, what):
        nonlocal ok, bad
        print(("  [ok ] " if cond else "  [BAD] ") + what); ok += cond; bad += not cond
    check("HON 0 1 20 Ann" in lines, "most frames won: Ann 20")
    check("HON 1 1 91% Ann" in lines, "win rate, 20 frames or more: Ann 91% (Bob has 23 frames, 3 won)")
    check(any(l.startswith("HON 1 ") and l.endswith(" Cat") for l in lines) is False, "Cat (2 frames) not on the win-rate board")
    check("HON 2 1 1 Bob" in lines and "HON 3 1 1 Bob" in lines, "break-and-runs and clearances: Bob 1")
    check("HON 4 1 40 Ann" in lines, "straight pool high run: Ann 40")
    check("HON 5 1 147 Bob" in lines, "snooker highest break: Bob 147")
    check("HON 6 1 98 Cat" in lines, "10-red: Cat 98")
    check(not any("Eve" in l for l in lines), "an unverified result counts nowhere")
    check("MONTH 5 2026-09 147 Bob" in lines and "MONTH 0 2026-09 20 Ann" in lines, "September's winners")
    check(not any(l.startswith("MONTH 6 ") for l in lines), "this month is not a trophy yet")
    check(not any(" & " in l for l in lines), "a doubles frame counts nowhere")
    check("HON 9 1 51 Dan" in lines and "MONTH 9 2026-09 51 Dan" in lines, "3-red highest break: Dan 51 (board 9), and September's")
    check(lines and lines[-1] == "END", "the answer ends with END")
    print(f"{ok} ok, {bad} bad"); sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
