#!/usr/bin/env python3
"""bench_tourn_watch.py -- what watchers cost a tournament's room, measured on
the desk (NEVER the relay server: a load test froze it once, 2026-10-11).

A local relay; eight players in a tournament's room each sending a 58-byte
pose 15 times a second (as the game does, cuevr_club_send_pose); four of them
also sending a table view of 163 bytes (16 balls) 15 times a second (balls
moving at every table -- the worst case); then 0, 10, 20, 30, 40 watchers
reading everything. For each, the relay's CPU (its own /proc ticks) and bytes
out a second.

    python3 relay/bench_tourn_watch.py [--secs 15]
"""
import argparse, os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port, conn, readline, send_frame  # noqa: E402

GID, TOKEN = "1129661807", "bench-token"


def cpu_ticks(pid):
    f = open(f"/proc/{pid}/stat").read().split(")")[1].split()
    return int(f[11]) + int(f[12])          # utime + stime


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--secs", type=float, default=15.0)
    a = ap.parse_args()
    store = tempfile.mkdtemp()
    tokf = os.path.join(store, "t"); open(tokf, "w").write(TOKEN)
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--admin-token-file", tokf, "--tourn-trust", "--no-voice", "--tourn-watchers", "60", *os.environ.get("BENCH_ARGS", "").split()],
                             stdout=open(os.environ.get("BENCH_LOG", os.devnull), "w"), stderr=subprocess.STDOUT)
    stop = threading.Event()
    got = [0]
    lock = threading.Lock()
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)
        st = int(time.time()) + 600
        s = conn(port, f"MOTE2 TOURN {GID} CREATE {TOKEN} {st} 0 0 8 3 5 7.5 {st - 1} BENCH")
        tid = int(readline(s).split()[2]); s.close()
        uids = [str(400 + i) for i in range(8)]
        for u in uids:
            s = conn(port, f"MOTE2 TOURN {GID} ENTER {tid} {u} n{u}nonce 7.5 P{u}"); readline(s); s.close()
        time.sleep(2.5)
        players = []
        for u in uids:
            s = conn(port, f"MOTE2 CLUB {GID} TOURN {tid} {u} n{u}nonce")
            seat = readline(s)
            if not seat.startswith("SEAT"):
                sys.exit(f"player {u} was not let in: {seat}")
            players.append(s)

        def drain(sock):
            sock.settimeout(1.0)
            while not stop.is_set():
                try:
                    b = sock.recv(65536)
                    if not b:
                        return
                    with lock:
                        got[0] += len(b)
                except socket.timeout:
                    continue
                except OSError:
                    return

        def sender(sock, table):
            pose, view = b"P" + bytes(58), b"T" + bytes(163)
            nxt = time.monotonic()
            while not stop.is_set():
                try:
                    send_frame(sock, 0xFF, pose)
                    if table:
                        send_frame(sock, 0xFF, view)
                except OSError as e:
                    print(f"  a player's link failed: {e}", flush=True)
                    return
                nxt += 1 / 15
                time.sleep(max(0.0, nxt - time.monotonic()))

        for i, s in enumerate(players):
            threading.Thread(target=drain, args=(s,), daemon=True).start()
            threading.Thread(target=sender, args=(s, i < 4), daemon=True).start()
        watchers = []
        print(f"{'watchers':>9} {'relay CPU':>10} {'out KB/s':>9}   (8 players at 15 poses/s, 4 tables at 15 views/s)")
        for want in (0, 10, 20, 30, 40):
            while len(watchers) < want:
                w = conn(port, f"MOTE2 CLUB {GID} TOURN {tid} WATCH")
                readline(w)
                threading.Thread(target=drain, args=(w,), daemon=True).start()
                watchers.append(w)
            time.sleep(2.0)
            c0, t0 = cpu_ticks(relay.pid), time.monotonic()
            with lock:
                g0 = got[0]
            time.sleep(a.secs)
            c1, t1 = cpu_ticks(relay.pid), time.monotonic()
            with lock:
                g1 = got[0]
            hz = os.sysconf("SC_CLK_TCK")
            print(f"{want:>9} {100 * (c1 - c0) / hz / (t1 - t0):>9.1f}% {(g1 - g0) / 1024 / (t1 - t0):>9.0f}", flush=True)
    finally:
        stop.set()
        relay.terminate()
        relay.wait(timeout=5)
        shutil.rmtree(store)


if __name__ == "__main__":
    main()
