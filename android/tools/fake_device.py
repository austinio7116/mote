#!/usr/bin/env python3
"""A stand-in for a docked Thumby Color, for testing the Android app's dock service
without any hardware.

The dock (os/android/mote_android_dock.c) talks to a handheld over USB-CDC on a
phone; on the desktop build it talks to a Unix socket instead (MOTE_DOCK_SOCK).
This script is the other end of that socket, speaking the same MN1 lines the real
device's lobby and gallery screens send (os/device/lobby_main.c), and checking the
replies.

    python3 android/tools/fake_device.py --sock /tmp/mote_dock.sock gallery
    python3 android/tools/fake_device.py --sock /tmp/mote_dock.sock host
"""
import argparse
import os
import socket
import sys
import time


class Dev:
    def __init__(self, path):
        if os.path.exists(path):
            os.unlink(path)
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(1)
        self.buf = b""
        print("fake-device: listening on %s" % path)

    def accept(self, timeout=30):
        self.srv.settimeout(timeout)
        self.c, _ = self.srv.accept()
        self.c.settimeout(30)
        print("fake-device: dock connected")

    def send(self, s):
        print("  dev -> %s" % s.strip())
        self.c.sendall(s.encode())

    def _fill(self):
        d = self.c.recv(65536)
        if not d:
            raise EOFError("dock closed the pipe")
        self.buf += d

    def line(self, timeout=20):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            if time.time() > end:
                raise TimeoutError("no line (buffered %r)" % self.buf[:80])
            self._fill()
        i = self.buf.index(b"\n")
        ln, self.buf = self.buf[:i], self.buf[i + 1:]
        s = ln.decode("utf-8", "replace").strip()
        print("  dev <- %s" % s)
        return s

    def bytes(self, n, timeout=60):
        end = time.time() + timeout
        while len(self.buf) < n:
            if time.time() > end:
                raise TimeoutError("wanted %d bytes, have %d" % (n, len(self.buf)))
            self._fill()
        d, self.buf = self.buf[:n], self.buf[n:]
        return d


def expect(got, want):
    if not got.startswith(want):
        sys.exit("FAIL: expected %r, got %r" % (want, got))


def gallery(d):
    """What the device's gallery screen does: list, thumbnail, describe, install."""
    d.send("MN1 GMANIFEST\n")
    expect(d.line(), "MN1 OK")
    games = []
    while True:
        l = d.line(timeout=90)
        if l.startswith("MN1 GEND"):
            break
        if l.startswith("MN1 GAME "):
            games.append(l)
        elif l.startswith("MN1 GERR"):
            sys.exit("FAIL: %s" % l)
    print("\n  parsed %d games" % len(games))
    if not games:
        sys.exit("FAIL: no games in the manifest")
    # the device parses: idx abi mp nshots size ver fname|author|name
    for g in games[:3]:
        body = g[len("MN1 GAME "):]
        head, _, tail = body.partition("|")
        f = head.split()
        assert len(f) == 7, "bad GAME line: %r" % g
        print("    idx=%s abi=%s mp=%s shots=%s size=%s ver=%s file=%s  %s"
              % (f[0], f[1], f[2], f[3], f[4], f[5], f[6], tail))

    d.send("MN1 GTHUMB 0 0\n")
    expect(d.line(), "MN1 OK")
    hd = d.line(timeout=90)
    expect(hd, "MN1 GTHUMB ")
    p = hd.split()
    n = int(p[-1])
    px = d.bytes(n)
    nz = sum(1 for i in range(0, len(px), 2) if px[i] or px[i + 1])
    print("  thumbnail %sx%s, %d bytes, %d non-black pixels" % (p[4], p[5], len(px), nz))
    if nz < 64:
        sys.exit("FAIL: thumbnail is essentially blank")

    d.send("MN1 GDESC 0\n")
    expect(d.line(), "MN1 OK")
    hd = d.line()
    expect(hd, "MN1 GDESC ")
    n = int(hd.split()[-1])
    desc = d.bytes(n).decode("utf-8", "replace") if n else ""
    print("  description %d bytes: %.70s..." % (n, desc))
    if n < 40:
        sys.exit("FAIL: description too short")

    d.send("MN1 GFETCH 0\n")
    expect(d.line(), "MN1 OK")
    hd = d.line(timeout=180)
    expect(hd, "MN1 GDATA ")
    n = int(hd.split()[-1])
    blob = d.bytes(n, timeout=180)
    magic = blob[:4]
    print("  .mote %d bytes, magic %r" % (len(blob), magic))
    if magic != b"MOTE":
        sys.exit("FAIL: not a .mote image (magic %r)" % magic)
    want = int(games[0][len("MN1 GAME "):].split()[4])
    if n != want:
        sys.exit("FAIL: manifest said %d bytes, got %d" % (want, n))
    print("\nPASS: gallery service is correct")


def room(d, verb):
    """What the lobby does for an online match: a room verb, then wait for GO."""
    gid = 1234567
    d.send("MN1 %s %d\n" % (verb, gid))
    expect(d.line(), "MN1 OK")
    got_code = None
    while True:
        l = d.line(timeout=40)
        if l.startswith("MN1 CODE "):
            got_code = l.split()[-1]
        elif l.startswith("MN1 GO"):
            print("\nPASS: paired, pipe is transparent%s"
                  % (" (room %s)" % got_code if got_code else ""))
            return
        elif l.startswith("MN1 ERR"):
            print("\n%s — expected when no relay/peer is reachable" % l)
            return


def browse(d):
    d.send("MN1 LIST 1234567\n")
    expect(d.line(), "MN1 OK")
    n = 0
    while True:
        l = d.line(timeout=40)
        if l.startswith("MN1 ENDROOMS"):
            break
        if l.startswith("MN1 ROOM "):
            n += 1
    print("\nPASS: browse returned %d rooms + ENDROOMS" % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["gallery", "host", "quick", "join", "browse", "lanhost"])
    ap.add_argument("--sock", default="/tmp/mote_dock.sock")
    a = ap.parse_args()

    d = Dev(a.sock)
    d.accept()
    if a.mode == "gallery":
        gallery(d)
    elif a.mode == "browse":
        browse(d)
    elif a.mode == "join":
        room(d, "JOIN 1234567 ABCD")
    elif a.mode == "lanhost":
        room(d, "LANHOST")
    else:
        room(d, a.mode.upper())


if __name__ == "__main__":
    main()
