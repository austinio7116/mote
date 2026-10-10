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

  python3 test_results.py [--relay mote_relay.py]
"""
import argparse, json, os, socket, subprocess, sys, tempfile, time

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


def result(frame, me, score=(3, 2), frames=(1, 0), winner=0, game="SNOOKER", best=(57, 12), brk=([57, 31], [])):
    return "RESULT " + json.dumps({
        "v": 1, "t": "frame", "ver": "7.3", "game": game, "variant": "", "kind": 4, "mode": 4,
        "best_of": 3, "frame": frame, "me": me, "names": ["A", "B"], "score": list(score),
        "frames": list(frames), "winner": winner, "match_winner": -1, "secs": 300, "bnr": -1,
        "golden": 0, "clear": 0,
        "sides": [{"shots": 20, "pot_shots": 9, "potted": 9, "fouls": 1, "time": 120.0, "best": best[0], "breaks": brk[0]},
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
    relay = subprocess.Popen([sys.executable, a.relay, "--addr", "127.0.0.1", "--port", str(a.port), "--store", store],
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
        check("the disputed frame is kept apart", len(disp) == 1 and not disp[0]["agreed"], str(len(disp)))
        check("the unconfirmed frame is kept apart at close", len(unconf) == 1, str(len(unconf)))
        check("no address is written, only its salted hash",
              all("127.0.0.1" not in json.dumps(x) for x in kept + disp + unconf))

        # a flood from one address
        h2 = Member(a.port, f"MOTE2 ROOMN {GID} HOST TST2 PRIV 2\n")
        g2 = Member(a.port, f"MOTE2 ROOMN {GID} JOIN TST2\n")
        h2.say("START"); time.sleep(0.2)
        for i in range(620):
            g2.say(result(1, 1))
        r = g2.replies(1.5)
        check("more than 600 an hour from one address -> BUSY", any("RESULT BUSY" in x for x in r), f"{len(r)} answers")
        h2.close(); g2.close()
    finally:
        relay.terminate()
    print("PASS" if not fails else f"FAIL ({fails})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
