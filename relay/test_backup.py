#!/usr/bin/env python3
"""test_backup.py -- players' backups (CueVR 7.6) over real sockets: what goes
up comes back byte for byte; the newest three are kept; one account never sees
another's; too big, too many a day, no backup folder and no proof are each
refused in so many words. The relay runs with --tourn-trust, so a desk proof
("desk<account>") stands in for Meta's; one more relay without it says PROOF."""
import os, shutil, socket, subprocess, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port  # noqa: E402
import mote_backup  # noqa: E402

ok = bad = 0
GID = 1129661809


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


def start(extra):
    store = tempfile.mkdtemp()
    port = free_port()
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                          "--store", store, "--no-voice"] + extra, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError:
            time.sleep(0.05)
    return p, port


class Session:
    def __init__(self, port, uid, proof=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rb")
        self.s.sendall(f"MOTE2 BACKUP {GID} OPEN {uid} {proof or 'desk' + str(uid)}\n".encode())
        self.first = self.line()

    def line(self):
        return self.f.readline().decode().strip()

    def put(self, data):
        self.s.sendall(f"PUT {len(data)}\n".encode() + data)
        return self.line()

    def listing(self):
        self.s.sendall(b"LIST\n")
        rows = []
        while True:
            l = self.line()
            if l == "END" or not l:
                return rows
            w = l.split()
            rows.append((int(w[1]), int(w[2]), int(w[3])))

    def get(self, bid):
        self.s.sendall(f"GET {bid}\n".encode())
        l = self.line()
        if not l.startswith("DATA "):
            return l
        return self.f.read(int(l.split()[1]))

    def close(self):
        try:
            self.s.sendall(b"BYE\n")
        except OSError:
            pass
        self.s.close()


def main():
    bdir = tempfile.mkdtemp()
    relay, port = start(["--tourn-trust", "--backup-dir", bdir])
    try:
        a = Session(port, 780)
        check(a.first == "BACKUP OK", f"a proved account opens its backups: {a.first}")
        check(a.listing() == [], "none to begin with")
        blobs = [os.urandom(1000 + 70000 * i) for i in range(4)]
        ids = []
        for b in blobs:
            r = a.put(b)
            ids.append(int(r.split()[2]) if r.startswith("BACKUP SAVED") else -1)
            time.sleep(0.01)
        check(all(i > 0 for i in ids), f"four backups go up: {ids}")
        rows = a.listing()
        check([r[0] for r in rows] == sorted(ids, reverse=True)[:3], f"the newest three are kept, newest first: {rows}")
        check(rows[0][2] == len(blobs[3]), "each listed with its size")
        check(a.get(ids[3]) == blobs[3] and a.get(ids[2]) == blobs[2], "what went up comes back byte for byte")
        check(a.get(ids[0]) == "BACKUP NONE", "the oldest is gone")
        a.close()
        b = Session(port, 781)
        check(b.listing() == [] and b.get(ids[3]) == "BACKUP NONE", "another account sees none of them")
        b.s.sendall(f"PUT {mote_backup.MAX_BYTES + 1}\n".encode())         # refused on the size, before a byte is sent
        check(b.line() == "BACKUP TOOBIG", "a backup over the limit is refused on its size")
        b.close()
        c = Session(port, 782)
        rs = [c.put(b"y" * 100) for _ in range(mote_backup.PER_DAY)]
        check(all(r.startswith("BACKUP SAVED") for r in rs) and len(rs) == mote_backup.PER_DAY, f"{mote_backup.PER_DAY} in a day are taken")
        c.close()
        c = Session(port, 782)
        check(c.put(b"y" * 100) == "BACKUP DAY", "...and one more that day is refused")
        c.close()
        names = os.listdir(bdir)
        check(all(len(n) == 32 and not n.isdigit() for n in names), f"the folders are a hash of the account, never its number: {names}")
    finally:
        relay.terminate(); relay.communicate(timeout=10)
    relay, port = start([])
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(f"MOTE2 BACKUP {GID} OPEN 780 desk780\n".encode())
        check(s.makefile().readline().strip() == "BACKUP OFF", "a relay without a backup folder says OFF")
    finally:
        relay.terminate(); relay.communicate(timeout=10)
    relay, port = start(["--backup-dir", tempfile.mkdtemp()])
    try:
        x = Session(port, 780)
        check(x.first == "BACKUP PROOF", "without a proof Meta accepts, nothing is opened")
    finally:
        relay.terminate(); relay.communicate(timeout=10)
    shutil.rmtree(bdir, ignore_errors=True)
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
