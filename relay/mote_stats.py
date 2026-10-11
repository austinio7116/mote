#!/usr/bin/env python3
"""mote_stats.py -- CueVR's online play at a glance, for one private phone app.

    python3 mote_stats.py --store /data --token-file T --cert C --key K [--port 8443] [--relay 127.0.0.1:443]

A separate program from the relay, beside it on the same box. It changes
nothing of the relay's: it reads <store>'s results files (kept, disputed,
unconfirmed, forfeits, reports), the relay's log (journalctl -u mote-relay),
and asks the relay its public LIST and HONOURS -- the same questions the game
asks. The working-out is in mote_stats_model.py, the page in mote_stats.html.

    GET /?t=<token>              the app: Now, Players, Matches, Honours,
                                 Integrity, Activity, Server
    GET /api?t=<token>           the clubs and rooms of each CueVR version now
    GET /api/model?t=<token>     players by Meta account (a short hash, never the
                                 number), matches with their frames, the
                                 honours and this month's race, the integrity
                                 queue, activity
    GET /api/player?t=<token>&id=<id>   one player in full: head to head, recent matches
    GET /api/match?t=<token>&id=<id>    one match with its frames
    GET /api/server?t=<token>    the relay's log over 7 days (addresses taken
                                 out), the box's health, the data files
    GET /api/history?t=<token>&days=1|7|30
                                 people online over time, and each day's peak
                                 and the players seen -- from a sample a minute

With --state DIR it also keeps that history: one line a minute in
DIR/history/<UTC date>.jsonl -- its own folder, never the relay's.

HTTPS with a self-signed certificate; the app pins it, so no domain is needed.
Without the token: 403, and a wrong token waits a second before it is told so.
"""
import argparse, hmac, json, os, socket, ssl, threading, time
from mote_stats_model import Model
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# the CueVR versions worth showing: the game id each plays online with
VERSIONS = [("7.3.2-7.4.x", 0x4355456F), ("7.3", 0x4355456E), ("7.2.1", 0x4355456D)]
GID = 0x4355456F                 # the honours are asked of the current one

A = None
TOKEN = b""
LOCK = threading.Lock()
CACHE = {"live_at": 0.0, "live": []}
MODEL = None


def relay_list(gid):
    host, port = A.relay.rsplit(":", 1)
    s = socket.create_connection((host, int(port)), timeout=5)
    s.sendall(f"MOTE2 LIST {gid}\n".encode())
    s.settimeout(5)
    buf = b""
    try:
        while b"END\n" not in buf:
            d = s.recv(8192)
            if not d:
                break
            buf += d
    finally:
        s.close()
    clubs, rooms, live = {}, [], []
    for l in buf.decode(errors="ignore").splitlines():
        p = l.split(" ")
        if p[0] == "CLUB" and len(p) >= 4 and "/" in p[2]:
            have, mx = p[2].split("/")
            clubs[p[1]] = {"n": p[1], "have": int(have), "max": int(mx), "tables": int(p[3]), "who": []}
        elif p[0] == "CLUBWHO" and len(p) >= 3 and p[1] in clubs:
            clubs[p[1]]["who"].append(" ".join(p[2:]))
        elif p[0] == "ROOM":
            rooms.append(" ".join(p[1:]))
        elif p[0] == "LIVE":
            live.append(" ".join(p[1:]))
    return {"clubs": list(clubs.values()), "rooms": rooms, "live": live}


def live_now():
    with LOCK:
        if time.time() - CACHE["live_at"] < 4:
            return CACHE["live"]
    out = []
    for name, gid in VERSIONS:
        try:
            out.append({"version": name, **relay_list(gid)})
        except OSError as e:
            out.append({"version": name, "error": type(e).__name__, "clubs": [], "rooms": [], "live": []})
    with LOCK:
        CACHE["live"], CACHE["live_at"] = out, time.time()
    return out


def count_list(v):
    """one version's LIST, as numbers: people in clubs, players and watchers in
    listed matches, rooms waiting, and the names to be seen (every club member;
    a listed match shows only its host -- a club table's match is a private
    room, its players counted in the club)"""
    club = sum(c["have"] for c in v["clubs"])
    names = set(n for c in v["clubs"] for n in c["who"])
    mp = mw = 0
    for m in v["live"]:
        t = m.split(" ")
        if len(t) >= 3 and "/" in t[-2]:
            try:
                mp += int(t[-2].split("/")[0]); mw += int(t[-1])
            except ValueError:
                pass
        if len(t) >= 2:
            names.add(t[1])
    rooms = len(v["rooms"])
    for r in v["rooms"]:
        t = r.split(" ")
        if len(t) >= 2:
            names.add(t[1])
    return {"club": club, "match": mp, "watch": mw, "matches": len(v["live"]), "rooms": rooms, "names": sorted(names)}


def sampler():
    """a sample a minute, on the minute, into the history"""
    hdir = os.path.join(A.state, "history")
    os.makedirs(hdir, exist_ok=True)
    while True:
        time.sleep(60 - (time.time() % 60) + 0.5)
        try:
            with LOCK:
                CACHE["live_at"] = 0.0           # a fresh LIST, not the page's
            live = live_now()
            now = time.time()
            row = {"t": int(now), "v": {v["version"]: count_list(v) for v in live if not v.get("error")}}
            day = time.strftime("%Y-%m-%d", time.gmtime(now))
            with open(os.path.join(hdir, day + ".jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps(row, separators=(",", ":")) + "\n")
        except Exception as e:                  # a bad minute is skipped, never the end of it
            print(f"history: {type(e).__name__}: {e}", flush=True)


HCACHE = {}                                      # closed days: date -> rows


def history_rows(day):
    path = os.path.join(A.state, "history", day + ".jsonl")
    today = time.strftime("%Y-%m-%d", time.gmtime())
    if day != today and day in HCACHE:
        return HCACHE[day]
    rows = []
    try:
        with open(path, encoding="utf-8") as f:
            for l in f:
                try:
                    rows.append(json.loads(l))
                except ValueError:
                    pass
    except OSError:
        pass
    if day != today:
        HCACHE[day] = rows
    return rows


def history(days):
    """people online over the last `days`, in buckets (the most at once in each),
    and per day: the peak and the players seen"""
    days = 1 if days <= 1 else 7 if days <= 7 else 30
    bucket = {1: 300, 7: 3600, 30: 4 * 3600}[days]
    now = int(time.time())
    t0 = now - days * 86400
    B = {}
    per_day = {}
    for k in range(days, -1, -1):
        day = time.strftime("%Y-%m-%d", time.gmtime(now - k * 86400))
        for r in history_rows(day):
            t = r.get("t", 0)
            club = sum(x.get("club", 0) for x in r.get("v", {}).values())
            match = sum(x.get("match", 0) for x in r.get("v", {}).values())
            d = per_day.setdefault(day, {"day": day, "peak": 0, "peak_t": 0, "names": set(), "samples": 0})
            d["samples"] += 1
            if club + match > d["peak"]:
                d["peak"], d["peak_t"] = club + match, t
            for x in r.get("v", {}).values():
                d["names"].update(x.get("names", []))
            if t < t0:
                continue
            b = (t // bucket) * bucket
            cur = B.get(b)
            if cur is None or club + match > cur["club"] + cur["match"]:
                B[b] = {"t": b, "club": club, "match": match}
    series = [B[k] for k in sorted(B)]
    daily = [{"day": d["day"], "peak": d["peak"], "peak_t": d["peak_t"], "players": len(d["names"]),
              "samples": d["samples"]} for d in sorted(per_day.values(), key=lambda d: d["day"], reverse=True)
             if d["day"] >= time.strftime("%Y-%m-%d", time.gmtime(t0))]
    return {"days": days, "bucket": bucket, "from": t0, "to": now, "series": series, "daily": daily}


def snapshot():
    return {"at": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC", "live": live_now()}


def page():
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mote_stats.html"), "rb") as f:
        return f.read()


class H(BaseHTTPRequestHandler):
    server_version = "mote-stats"

    def _ok(self, q):
        t = (q.get("t") or [""])[0].encode()
        if TOKEN and hmac.compare_digest(t, TOKEN):
            return True
        time.sleep(1.0)          # a wrong guess waits
        return False

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path not in ("/", "/api", "/api/history", "/api/model", "/api/server", "/api/player", "/api/match"):
            return self._send(404, "text/plain", b"not here\n")
        if not self._ok(q):
            return self._send(403, "text/plain", b"no\n")
        if u.path == "/api":
            return self._send(200, "application/json", json.dumps(snapshot()).encode())
        if u.path == "/api/model":
            with LOCK:
                m = MODEL.get()
            return self._send(200, "application/json", json.dumps(m, separators=(",", ":")).encode())
        if u.path in ("/api/player", "/api/match"):
            key = (q.get("id") or [""])[0][:80]
            with LOCK:
                v = MODEL.player(key) if u.path == "/api/player" else MODEL.match(key)
            if v is None:
                return self._send(404, "application/json", b'{"error":"not found"}')
            return self._send(200, "application/json", json.dumps(v, separators=(",", ":")).encode())
        if u.path == "/api/server":
            return self._send(200, "application/json", json.dumps(MODEL.server(), separators=(",", ":")).encode())
        if u.path == "/api/history":
            if not A.state:
                return self._send(404, "text/plain", b"no history kept\n")
            try:
                days = int((q.get("days") or ["1"])[0])
            except ValueError:
                days = 1
            return self._send(200, "application/json", json.dumps(history(days)).encode())
        return self._send(200, "text/html; charset=utf-8", page())

    def log_message(self, fmt, *args):
        pass                     # no addresses kept


def main():
    global A, TOKEN, MODEL
    ap = argparse.ArgumentParser()
    ap.add_argument("--store", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--relay", default="127.0.0.1:443")
    ap.add_argument("--state", default="", help="keep a sample a minute here (history)")
    ap.add_argument("--journal-file", default="", help="read the relay's log from this file, not journalctl (tests)")
    ap.add_argument("--backups", default="", help="the daily snapshots' folder, listed on the Server page")
    A = ap.parse_args()
    MODEL = Model(A.store, A.relay, GID, A.journal_file, A.backups)
    with open(A.token_file, "rb") as f:
        TOKEN = f.read().strip()
    if len(TOKEN) < 24:
        raise SystemExit("the token is too short (24 characters at least)")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(A.cert, A.key)
    srv = ThreadingHTTPServer((A.bind, A.port), H)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    if A.state:
        threading.Thread(target=sampler, daemon=True).start()
    print(f"mote-stats on {A.bind}:{A.port}, results from {A.store}, relay {A.relay}, history {A.state or 'off'}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
