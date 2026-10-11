#!/usr/bin/env python3
"""test_honours.py -- the relay's HONOURS answer (7.4): the club's ten boards
and the completed months' winners, tallied BY META ACCOUNT, and the MINE lines
a player gets back only with a proof of their account. A made-up results
store and a stand-in for Meta, its own temp store and port; never the server's.

    python3 relay/test_honours.py
"""
import json, os, socket, subprocess, sys, tempfile, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 47977
APP, SECRET = "1036582382349280", "test-secret-not-real"
UID = {"Ann": "101", "Bob": "102", "Cat": "103", "Dan": "104", "Fay": "106", "Eve": "105", "Ann2": "107"}
GOOD = {"proofANN": "101", "proofBOB": "102"}


class FakeMeta(BaseHTTPRequestHandler):
    def do_POST(self):
        q = urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode())
        ok = (q.get("access_token", [""])[0] == f"OC|{APP}|{SECRET}" and
              GOOD.get(q.get("nonce", [""])[0]) == q.get("user_id", [""])[0])
        body = json.dumps({"is_valid": ok}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def frame(t, a, b, win, kind, best=(0, 0), bnr=(0, 0), runs=(0, 0), verified=True, ua=None, ub=None):
    """a kept frame; each side's account its player's unless given (ua/ub)"""
    side = lambda i: {"best": best[i], "shot": {"bnr": bnr[i], "runouts": runs[i]}}
    acc = lambda n: UID.get(n.split(" & ")[0], "999")
    return {"time": t, "verified": verified,
            "a": {"side": 0, "uid": ua or acc(a)}, "b": {"side": 1, "uid": ub or acc(b)},
            "result": {"t": "frame", "names": [a, b], "winner": win, "kind": kind, "sides": [side(0), side(1)]}}


def ask(line):
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=4); break
        except OSError:
            time.sleep(0.1)
    s.sendall(line.encode()); buf = b""
    while b"END\n" not in buf:
        d = s.recv(8192)
        if not d: break
        buf += d
    s.close()
    return buf.decode().splitlines()


def main():
    store = tempfile.mkdtemp()
    rows = []
    # last month: Ann wins 20 of 22 frames at straight pool (run 40); Bob a 147 at snooker
    for i in range(22):
        rows.append(frame("2026-09-12 20:00:00", "Ann", "Bob", 0 if i < 20 else 1, 7, best=(40 if i == 0 else 5, 3)))
    rows.append(frame("2026-09-20 21:00:00", "Bob", "Cat", 0, 4, best=(147, 20), bnr=(1, 0), runs=(1, 0)))
    rows.append(frame("2026-09-21 21:00:00", "Ann & Bob", "Cat & Dan", 0, 1))   # doubles: counts nowhere
    rows.append(frame("2026-09-22 21:00:00", "Dan", "Fay", 0, 23, best=(51, 8)))   # 3-red
    # BY ACCOUNT: Ann plays on as "Annie" (her account 101), and a different
    # player (account 107) also calls themself "Ann"
    rows.append(frame("2026-09-25 21:00:00", "Annie", "Cat", 0, 7, best=(12, 0), ua="101"))
    rows.append(frame("2026-09-26 21:00:00", "Ann", "Cat", 0, 10, best=(77, 0), ua="107"))
    # this month: Cat a 98 at 10-red, an unverified frame that must not count
    rows.append(frame(time.strftime("%Y-%m-01 10:00:00", time.gmtime()), "Cat", "Dan", 0, 5, best=(98, 0)))
    rows.append(frame(time.strftime("%Y-%m-01 11:00:00", time.gmtime()), "Eve", "Dan", 0, 4, best=(155, 0), verified=False))
    with open(os.path.join(store, "results.jsonl"), "w") as f:
        for r in rows: f.write(json.dumps(r) + "\n")
    meta = HTTPServer(("127.0.0.1", 0), FakeMeta)
    threading.Thread(target=meta.serve_forever, daemon=True).start()
    secret = os.path.join(tempfile.mkdtemp(), "meta.secret")
    open(secret, "w").write(SECRET + "\n")
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--port", str(PORT), "--store", store,
                          "--meta-secret-file", secret, "--meta-app-id", APP,
                          "--meta-verify-url", f"http://127.0.0.1:{meta.server_port}/"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        lines = ask("MOTE2 HONOURS 1129661807\n")
        ann = ask("MOTE2 HONOURS 1129661807 101 proofANN\n")
        bad = ask("MOTE2 HONOURS 1129661807 101 proofBOB\n")
        steal = ask("MOTE2 HONOURS 1129661807 102 proofANN\n")
    finally:
        p.terminate()
    ok = 0; bad_n = 0
    def check(cond, what):
        nonlocal ok, bad_n
        print(("  [ok ] " if cond else "  [BAD] ") + what); ok += cond; bad_n += not cond
    check("HON 0 1 21 Annie" in lines, "most frames won by ACCOUNT: Ann's 20 and Annie's 1 are one player, shown as her latest name")
    check("HON 8 1 77 Ann" in lines, "the other player called Ann (account 107) keeps their own record (billiards 77)")
    check("HON 1 1 91% Annie" in lines, "win rate, 20 frames or more: account 101 21 of 23")
    check(not any(l.startswith("HON 1 ") and l.endswith(" Cat") for l in lines), "Cat (5 frames) not on the win-rate board")
    check("HON 2 1 1 Bob" in lines and "HON 3 1 1 Bob" in lines, "break-and-runs and clearances: Bob 1")
    check("HON 4 1 40 Annie" in lines, "straight pool high run: account 101's 40")
    check("HON 5 1 147 Bob" in lines, "snooker highest break: Bob 147")
    check("HON 6 1 98 Cat" in lines, "10-red: Cat 98")
    check("HON 9 1 51 Dan" in lines and "MONTH 9 2026-09 51 Dan" in lines, "3-red: Dan 51, and September's")
    check(not any("Eve" in l for l in lines), "an unverified result counts nowhere")
    check(not any(" & " in l for l in lines), "a doubles frame counts nowhere")
    check("MONTH 5 2026-09 147 Bob" in lines and "MONTH 0 2026-09 21 Annie" in lines, "September's winners")
    check(not any(l.startswith("MONTH 6 ") for l in lines), "this month is not a trophy yet")
    check(not any(l.startswith("MINE ") for l in lines), "no MINE lines without a proof")
    check("MINE 0 2026-09 21" in ann and "MINE 4 2026-09 40" in ann and "MINE 1 2026-09 91%" in ann,
          "a proved account gets its own monthly wins back (Ann: frames, straight pool, win rate)")
    check(not any(l.startswith("MINE ") for l in bad), "a proof that is not that account's: no MINE lines")
    check(not any(l.startswith("MINE ") for l in steal), "someone else's account with your proof: nothing of theirs to claim")
    check(lines and lines[-1] == "END", "the answer ends with END")
    print(f"{ok} ok, {bad_n} bad"); sys.exit(1 if bad_n else 0)


if __name__ == "__main__":
    main()
