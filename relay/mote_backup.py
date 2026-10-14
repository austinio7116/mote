"""mote_backup.py -- a player's settings and creations, kept for them (CueVR 7.6).

VR111: "the ability to back it up somewhere and somehow be able to load it
back -- to preserve and save hours and hours of work" (a headset dies, a new
headset, a friend's headset, a safety net). Mark chose: by the Meta account,
on the server.

Each account has a folder of its own under the backup directory -- a
directory APART from the relay's data, so keeping only the newest few never
deletes anything there, and the daily snapshot of the results stays small.
The folder is named by a one-way hash of the account, never the number.

A backup is whatever the game sends (its own packed, compressed bundle): the
relay neither opens nor reads it. Kept: the newest KEEP; the oldest go as a
new one lands. Refused: bigger than MAX_BYTES, more than PER_DAY a day from one
account, or when the disk is short of MIN_FREE.

    store = BackupStore("/var/lib/mote-backups")
    store.begin(uid) -> a Writer, or a word ("DAY", "DISK")
    writer.write(chunk) ... writer.finish() -> the new id
    store.list(uid)  -> [(id, unix time, bytes)], newest first
    store.path(uid, id) -> the file, or None
"""
import hashlib
import os
import shutil
import time

KEEP = 3
MAX_BYTES = 5 * 1024 * 1024
PER_DAY = 10
MIN_FREE = 2 * 1024 * 1024 * 1024


def account_dir_name(uid):
    return hashlib.sha256(("cuevr-backup:" + str(uid)).encode()).hexdigest()[:32]


class Writer:
    """one backup arriving: written to a part file, renamed into place whole"""
    def __init__(self, store, d, bid):
        self.store, self.d, self.bid = store, d, bid
        self.part = os.path.join(d, f"{bid}.part")
        self.f = open(self.part, "wb")
        self.n = 0

    def write(self, chunk):
        self.n += len(chunk)
        if self.n > MAX_BYTES:
            raise ValueError("too big")
        self.f.write(chunk)

    def abandon(self):
        try:
            self.f.close()
        except OSError:
            pass
        try:
            os.remove(self.part)
        except OSError:
            pass

    def finish(self):
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()
        os.replace(self.part, os.path.join(self.d, f"{self.bid}.bak"))
        self.store.rotate(self.d)
        return self.bid


class BackupStore:
    def __init__(self, root, now=time.time):
        self.root = root
        self.now = now
        self.puts = {}           # account folder -> the times of its uploads today (kept in memory)
        os.makedirs(root, exist_ok=True)

    def _dir(self, uid):
        return os.path.join(self.root, account_dir_name(uid))

    def list(self, uid):
        d = self._dir(uid)
        out = []
        try:
            names = os.listdir(d)
        except OSError:
            return out
        for n in names:
            if not n.endswith(".bak"):
                continue
            try:
                bid = int(n[:-4])
                out.append((bid, bid // 1000, os.path.getsize(os.path.join(d, n))))
            except (ValueError, OSError):
                continue
        out.sort(reverse=True)
        return out

    def path(self, uid, bid):
        try:
            bid = int(bid)
        except (TypeError, ValueError):
            return None
        p = os.path.join(self._dir(uid), f"{bid}.bak")
        return p if os.path.isfile(p) else None

    def begin(self, uid):
        """a Writer for a new backup, or why not: DAY (too many today), DISK"""
        try:
            if shutil.disk_usage(self.root).free < MIN_FREE:
                return "DISK"
        except OSError:
            return "DISK"
        d = self._dir(uid)
        os.makedirs(d, exist_ok=True)
        now = self.now()
        key = account_dir_name(uid)
        day = [x for x in self.puts.get(key, []) if now - x < 86400]
        if len(day) >= PER_DAY:
            self.puts[key] = day
            return "DAY"
        self.puts[key] = day + [now]
        bid = int(now * 1000)
        while os.path.exists(os.path.join(d, f"{bid}.bak")):
            bid += 1
        return Writer(self, d, bid)

    def rotate(self, d):
        """the newest KEEP stay; the rest, and any part file left by a cut
        connection more than an hour ago, go"""
        try:
            names = os.listdir(d)
        except OSError:
            return
        baks = sorted((n for n in names if n.endswith(".bak")), key=lambda n: int(n[:-4]) if n[:-4].isdigit() else 0,
                      reverse=True)
        for n in baks[KEEP:]:
            try:
                os.remove(os.path.join(d, n))
            except OSError:
                pass
        for n in names:
            if n.endswith(".part"):
                p = os.path.join(d, n)
                try:
                    if self.now() - os.path.getmtime(p) > 3600:
                        os.remove(p)
                except OSError:
                    pass
