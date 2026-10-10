#!/usr/bin/env python3
"""test_results.py -- the relay's RESULT record (CueVR 7.3) against the ways
it could be abused. Starts its own relay on localhost with a fresh store,
plays the relay's protocol as two members of a room, and checks what is kept:

  * the same facts from both sides      -> results.jsonl, agreed
  * one side claiming another score     -> results_disputed.jsonl, not kept
  * a snooker break of 200              -> RESULT BAD, kept nowhere
  * frames past the match's length      -> RESULT BAD
  * a result with one player in the room -> RESULT BAD
  * a result never confirmed            -> results_unconfirmed.jsonl at close
  * more than 600 an hour from one address -> RESULT BUSY
  * a player's Meta proof (against a fake Meta here): a good one -> PROOF OK and
    the results carry the account, verified when both sides proved different
    accounts; a bad one -> PROOF BAD; the same account both sides -> not verified

  python3 test_results.py [--relay mote_relay.py]
"""
import argparse, json, os, socket, subprocess, sys, tempfile, threading, time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

APP, SECRET = "1036582382349280", "test-secret-not-real"
GOOD = {"goodnonceAAAA1111": "111", "goodnonceBBBB2222": "222", "goodnonceCCCC3333": "111"}


class FakeMeta(BaseHTTPRequestHandler):
    """graph.oculus.com/user_nonce_validate, as far as the relay can tell"""
    def do_POST(self):
        q = urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode())
        ok = (q.get("access_token", [""])[0] == f"OC|{APP}|{SECRET}" and
              GOOD.get(q.get("nonce", [""])[0]) == q.get("user_id", [""])[0])
        body = json.dumps({"is_valid": ok}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
GID = "1129661805"


def frame_out(to, payload):
    return (1 + len(payload)).to_bytes(2, "little") + bytes([to]) + payload


class Member:
    def __init__(self, port, line):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.sendall(line.encode())
        self.buf = b""
        while b"\n" not in self.buf:
            self.buf += self.s.recv(4096)
        head, self.buf = self.buf.split(b"\n", 1)
        self.seat = head.decode()

    def say(self, text):
        self.s.sendall(frame_out(0xFE, text.encode()))

    def replies(self, wait=0.4):
        """the relay's own words to this member since the last call"""
        self.s.settimeout(wait)
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d
        except socket.timeout:
            pass
        out = []
        while len(self.buf) >= 2:
            n = int.from_bytes(self.buf[:2], "little")
            if len(self.buf) < 2 + n:
                break
            body, self.buf = self.buf[2:2 + n], self.buf[2 + n:]
            if body[0] == 0xFE:
                out.append(body[5:].decode("ascii", "ignore"))
        return out

    def close(self):
        self.s.close()


SHOT = {"att": 14, "made": 9, "long_att": 4, "long_made": 2, "safeties": 5, "safe_ok": 3, "banks": 0,
        "breaks": 1, "breaks_ok": 0, "breaks_dry": 1, "breaks_foul": 0, "bnr": 0, "runouts": 0,
        "visits": 3, "vis_pts": 88, "b50": 1, "b100": 0, "obj": 0, "cue": 0, "random": 1, "series": 0}


def result(frame, me, score=(3, 2), frames=(1, 0), winner=0, game="SNOOKER", best=(57, 12), brk=([57, 31], []), shot=None):
    return "RESULT " + json.dumps({
        "v": 1, "t": "frame", "ver": "7.3", "game": game, "variant": "", "kind": 4, "mode": 4,
        "best_of": 3, "frame": frame, "me": me, "names": ["A", "B"], "score": list(score),
        "frames": list(frames), "winner": winner, "match_winner": -1, "secs": 300, "bnr": -1,
        "golden": 0, "clear": 0,
        "sides": [{"shots": 20, "pot_shots": 9, "potted": 9, "fouls": 1, "time": 120.0, "best": best[0], "breaks": brk[0],
                   "shot": dict(SHOT, **(shot or {}))},
                  {"shots": 18, "pot_shots": 3, "potted": 3, "fouls": 2, "time": 100.0, "best": best[1], "breaks": brk[1]}]})


def lines(store, name):
    p = os.path.join(store, name)
    return [json.loads(l) for l in open(p)] if os.path.exists(p) else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay", default=os.path.join(HERE, "mote_relay.py"))
    ap.add_argument("--port", type=int, default=42711)
    a = ap.parse_args()
    store = tempfile.mkdtemp(prefix="relay-results-")
    meta = HTTPServer(("127.0.0.1", 0), FakeMeta)
    threading.Thread(target=meta.serve_forever, daemon=True).start()
    secret = os.path.join(tempfile.mkdtemp(prefix="relay-secret-"), "meta.secret")
    open(secret, "w").write(SECRET + "\n")
    relay = subprocess.Popen([sys.executable, a.relay, "--addr", "127.0.0.1", "--port", str(a.port), "--store", store,
                              "--meta-secret-file", secret, "--meta-app-id", APP,
                              "--meta-verify-url", f"http://127.0.0.1:{meta.server_port}/"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    fails = 0

    def check(what, ok, detail=""):
        nonlocal fails
        print(f"  [{'ok ' if ok else 'BAD'}] {what}{('  ' + detail) if detail else ''}")
        fails += 0 if ok else 1

    try:
        host = Member(a.port, f"MOTE2 ROOMN {GID} HOST TEST PRIV 2\n")
        # one player in the room: nothing it says counts
        host.say(result(1, 0))
        check("a result with one player in the room is refused", "RESULT BAD" in host.replies())
        guest = Member(a.port, f"MOTE2 ROOMN {GID} JOIN TEST\n")
        host.say("START"); time.sleep(0.3); host.replies(); guest.replies()

        # proofs: a bad one, then good ones for two different accounts
        guest.say("PROOF 222 badnonceXXXX9999"); time.sleep(0.4); r = guest.replies()
        check("a bad Meta proof is refused", "PROOF BAD" in r, str(r))
        host.say("PROOF 111 goodnonceAAAA1111"); guest.say("PROOF 222 goodnonceBBBB2222"); time.sleep(0.5)
        r1, r2 = host.replies(), guest.replies()
        check("good Meta proofs are accepted", "PROOF OK" in r1 and "PROOF OK" in r2, f"{r1} {r2}")

        # the same facts from both sides: kept, agreed
        host.say(result(1, 0)); r1 = host.replies()
        guest.say(result(1, 1)); r2 = guest.replies()
        check("both sides agree -> kept", "RESULT HELD" in r1 and "RESULT KEPT" in r2, f"{r1} {r2}")

        # the guest claims it won frame 2; the host saw the host win it
        host.say(result(2, 0, frames=(2, 0), winner=0)); host.replies()
        guest.say(result(2, 1, frames=(1, 1), winner=1)); r = guest.replies()
        check("a side claiming another score -> disputed, not kept", "RESULT DISPUTED" in r, str(r))

        # a break no snooker table allows
        guest.say(result(3, 1, best=(200, 0), brk=([200], []))); r = guest.replies()
        check("a snooker break of 200 is refused", "RESULT BAD" in r, str(r))
        # frames past a best of 3
        guest.say(result(3, 1, frames=(5, 0))); r = guest.replies()
        check("frames past the match's length are refused", "RESULT BAD" in r, str(r))
        # stroke figures that cannot be: more pots made than attempted
        guest.say(result(3, 1, shot={"made": 20})); r = guest.replies()
        check("more pots made than attempted is refused", "RESULT BAD" in r, str(r))
        # not JSON at all
        guest.say("RESULT {not json"); r = guest.replies()
        check("a result that is not JSON is refused", "RESULT BAD" in r, str(r))

        # held for frame 3 by the host alone, then the room closes
        host.say(result(3, 0, frames=(2, 1), winner=0)); host.replies()
        guest.close(); host.close(); time.sleep(0.8)

        kept = lines(store, "results.jsonl")
        disp = lines(store, "results_disputed.jsonl")
        unconf = lines(store, "results_unconfirmed.jsonl")
        check("results.jsonl holds the one agreed frame", len(kept) == 1 and kept[0]["result"]["frame"] == 1, str(len(kept)))
        check("...with each side's stroke figures", kept and kept[0]["result"]["sides"][0].get("shot", {}).get("att") == 14)
        check("the kept result carries both proved accounts and is verified",
              kept and kept[0]["verified"] and {kept[0]["a"]["uid"], kept[0]["b"]["uid"]} == {"111", "222"}, str(kept[0].get("verified") if kept else None))
        check("the disputed frame is kept apart", len(disp) == 1 and not disp[0]["agreed"], str(len(disp)))
        check("the unconfirmed frame is kept apart at close", len(unconf) == 1, str(len(unconf)))
        check("no address is written, only its salted hash",
              all("127.0.0.1" not in json.dumps(x) for x in kept + disp + unconf))

        # one person, one account, both sides of a room: agreed, but never verified
        h3 = Member(a.port, f"MOTE2 ROOMN {GID} HOST TST3 PRIV 2\n")
        g3 = Member(a.port, f"MOTE2 ROOMN {GID} JOIN TST3\n")
        h3.say("START"); time.sleep(0.2)
        h3.say("PROOF 111 goodnonceAAAA1111")      # (a reused nonce: the fake does not expire them)
        g3.say("PROOF 111 goodnonceCCCC3333"); time.sleep(0.5); h3.replies(); g3.replies()
        h3.say(result(1, 0)); h3.replies(); g3.say(result(1, 1)); g3.replies()
        h3.close(); g3.close(); time.sleep(0.5)
        last = lines(store, "results.jsonl")[-1]
        check("the same account on both sides is never verified", last["room"] == "TST3" and last["agreed"] and not last["verified"])

        # a flood from one address
        h2 = Member(a.port, f"MOTE2 ROOMN {GID} HOST TST2 PRIV 2\n")
        g2 = Member(a.port, f"MOTE2 ROOMN {GID} JOIN TST2\n")
        h2.say("START"); time.sleep(0.2)
        for i in range(620):
            g2.say(result(1, 1))
        r = g2.replies(1.5)
        check("more than 600 an hour from one address -> BUSY", any("RESULT BUSY" in x for x in r), f"{len(r)} answers")
        h2.close(); g2.close()
        check("the secret is never written to the log or the store",
              all(SECRET not in open(os.path.join(store, f), errors="ignore").read() for f in os.listdir(store)))
    finally:
        relay.terminate()
        meta.shutdown()
    print("PASS" if not fails else f"FAIL ({fails})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
