#!/usr/bin/env python3
"""test_newer.py -- the relay telling an out-of-date CueVR so (7.3): an old
game id is still let into its own club, its SEAT line carries NEWER and its
LIST ends with a NEWER line; the current id hears neither; a different game's
ids are never compared; and <store>/latest_games is read again when it changes.

  python3 test_newer.py [--relay mote_relay.py]
"""
import argparse, os, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
CUR, OLD, OTHER = 0x4355456E, 0x4355456D, 0x4D4F5401   # 'CUEn', 'CUEm', another game


def line(port, text):
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(text.encode())
    s.settimeout(2.0)
    buf = b""
    try:
        while b"END\n" not in buf and not (text.split()[1] == "CLUB" and b"\n" in buf):
            d = s.recv(4096)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    return s, buf.decode("ascii", "ignore")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay", default=os.path.join(HERE, "mote_relay.py"))
    ap.add_argument("--port", type=int, default=42713)
    a = ap.parse_args()
    store = tempfile.mkdtemp(prefix="relay-newer-")
    relay = subprocess.Popen([sys.executable, a.relay, "--addr", "127.0.0.1", "--port", str(a.port),
                              "--store", store, "--latest-game", "CUEn"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    fails = 0

    def check(what, ok, detail=""):
        nonlocal fails
        print(f"  [{'ok ' if ok else 'BAD'}] {what}{('  ' + detail) if detail else ''}")
        fails += 0 if ok else 1

    try:
        s1, r = line(a.port, f"MOTE2 CLUB {OLD} JOIN\n")
        check("an old version is still let into its club", r.startswith("SEAT "), r.strip())
        check("...and its SEAT says NEWER", r.split("\n")[0].split()[-1] == "NEWER", r.strip())
        check("...in a line an old reader still reads as three numbers",
              len(r.split()[1:4]) == 3 and all(x.isdigit() for x in r.split()[1:4]))
        s2, r = line(a.port, f"MOTE2 CLUB {CUR} JOIN\n")
        check("the current version's SEAT does not", r.startswith("SEAT ") and "NEWER" not in r, r.strip())
        _, r = line(a.port, f"MOTE2 LIST {OLD}\n")
        check("an old version's LIST carries NEWER", "NEWER\n" in r and "CLUB 1 1/16" in r, repr(r))
        _, r = line(a.port, f"MOTE2 LIST {CUR}\n")
        check("the current version's LIST does not", "NEWER" not in r and "CLUB 1 1/16" in r, repr(r))
        _, r = line(a.port, f"MOTE2 LIST {OTHER}\n")
        check("another game is never compared with CueVR", "NEWER" not in r, repr(r))
        # a release: 'CUEo' in the store's file, no restart
        with open(os.path.join(store, "latest_games"), "w") as f:
            f.write("# the current CueVR\nCUEo\n")
        _, r = line(a.port, f"MOTE2 LIST {CUR}\n")
        check("the store's latest_games is read again without a restart", "NEWER\n" in r, repr(r))
        s1.close(); s2.close()
    finally:
        relay.terminate()
    print("PASS" if not fails else f"FAIL ({fails})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
