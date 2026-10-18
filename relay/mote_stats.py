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
    GET /api/tourn?t=<token>     the relay's tournaments (its TOURN LIST)
    POST /api/tourn?t=<token>    a new tournament: the spec as JSON, passed to the
                                 relay's TOURN CREATE with its admin token
                                 (--tourn-token-file; without one, 503)
    POST /api/tourn/cancel?t=<token>&tid=<n>   called off
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
VERSIONS = [("7.6.5-7.6.7", 0x43554575), ("7.6.4", 0x43554574), ("7.6.2-7.6.3", 0x43554573), ("7.6-7.6.1", 0x43554572), ("7.5.5", 0x43554571), ("7.5-7.5.4", 0x43554570), ("7.3.2-7.4.x", 0x4355456F), ("7.3", 0x4355456E), ("7.2.1", 0x4355456D)]
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


def seeder():
    """THE OVERALL RATINGS TO THE RELAY, every two minutes, for seeding a
    tournament's draw (Mark, 2026-10-13: "secretly use the overall ELO rating
    to seed players"): each proved account's by the short hash the relay can
    match, in pieces small enough for one request; kept by the relay in memory
    only. Nothing on any page says so."""
    time.sleep(15)
    while True:
        try:
            tok = tourn_token()
            if tok:
                with LOCK:
                    m = MODEL.get()
                r = {row["id"][2:]: row["r"] for row in m["ratings"]["all"] if str(row["id"]).startswith("a:")}
                items = list(r.items())
                for i in range(0, len(items), 100):
                    relay_ask(f"MOTE2 TOURN {A.tourn_gid} ADMIN {tok} SEEDS", json.dumps({"r": dict(items[i:i + 100])}))
                # THE CUEVR WORLD RANKING (7.6.8): the ClaudeELO order, as
                # places -- account hash and name, NEVER the score (Mark: "we
                # must NEVER show the actual ClaudeELO rating scores - just the
                # ranking") -- in parts under the relay's 4000-byte line
                rk = [[row["id"][2:], row["name"]] for row in (m.get("claude") or {}).get("rows", [])
                      if str(row["id"]).startswith("a:")]
                parts = [rk[i:i + 60] for i in range(0, len(rk), 60)] or [[]]
                for k, part in enumerate(parts):
                    relay_ask(f"MOTE2 TOURN {A.tourn_gid} ADMIN {tok} RANKING", json.dumps({"part": k, "of": len(parts), "rows": part}))
        except Exception as e:                  # a bad turn is skipped, never the end of it
            print(f"seeder: {type(e).__name__}: {e}", flush=True)
        time.sleep(120)


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
            tp = tourn_present()
            if tp is not None:
                row["tourn"], row["tn"] = tp
            day = time.strftime("%Y-%m-%d", time.gmtime(now))
            with open(os.path.join(hdir, day + ".jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps(row, separators=(",", ":")) + "\n")
        except Exception as e:                  # a bad minute is skipped, never the end of it
            print(f"history: {type(e).__name__}: {e}", flush=True)


HCACHE = {}                                      # closed days: date -> rows


def tourn_present():
    """players in a tournament's room now (the organiser's PRESENT, by the
    admin token): (count, names), or None when it cannot be asked"""
    tok = tourn_token()
    if not tok:
        return None
    try:
        ans = relay_ask(f"MOTE2 TOURN {A.tourn_gid} ADMIN {tok} PRESENT", "{}").strip()
    except OSError:
        return None
    p = ans.split(" ", 3)
    if len(p) < 3 or p[0] != "ADMIN" or p[1] != "OK":
        return None
    try:
        n = int(p[2])
    except ValueError:
        return None
    names = [x for x in (p[3].split("|") if len(p) > 3 else []) if x and x != "-"]
    return n, names


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
            tourn = int(r.get("tourn", 0) or 0)                  # in a tournament's room (blue)
            d = per_day.setdefault(day, {"day": day, "peak": 0, "peak_t": 0, "names": set(), "samples": 0})
            d["samples"] += 1
            if club + match + tourn > d["peak"]:
                d["peak"], d["peak_t"] = club + match + tourn, t
            for x in r.get("v", {}).values():
                d["names"].update(x.get("names", []))
            d["names"].update(r.get("tn", []))
            if t < t0:
                continue
            b = (t // bucket) * bucket
            cur = B.get(b)
            if cur is None or club + match + tourn > cur["club"] + cur["match"] + cur["tourn"]:
                B[b] = {"t": b, "club": club, "match": match, "tourn": tourn}
    series = [B[k] for k in sorted(B)]
    daily = [{"day": d["day"], "peak": d["peak"], "peak_t": d["peak_t"], "players": len(d["names"]),
              "samples": d["samples"]} for d in sorted(per_day.values(), key=lambda d: d["day"], reverse=True)
             if d["day"] >= time.strftime("%Y-%m-%d", time.gmtime(t0))]
    return {"days": days, "bucket": bucket, "from": t0, "to": now, "series": series, "daily": daily}


# ---- TOURNAMENTS (7.5): Mark makes them here, the relay runs them ----------
def relay_ask(line, body=None, until=b"\n"):
    host, port = A.relay.rsplit(":", 1)
    s = socket.create_connection((host, int(port)), timeout=6)
    s.sendall(line.encode() + b"\n" + ((body.encode() + b"\n") if body is not None else b""))
    s.settimeout(6)
    buf = b""
    try:
        while until not in buf:
            d = s.recv(8192)
            if not d:
                break
            buf += d
    finally:
        s.close()
    return buf.decode("utf-8", "replace")


def tourn_list():
    out, by = [], {}
    for l in relay_ask(f"MOTE2 TOURN {A.tourn_gid} LIST", until=b"END\n").splitlines():
        p = l.split(" ")
        if p[0] == "TL" and len(p) >= 12:
            t = {"tid": int(p[1]), "status": p[2], "start": int(float(p[3])), "close": int(float(p[4])), "kind": int(p[5]),
                 "rules": int(p[6]), "size": int(p[7]), "best_of": int(p[8]), "final_best_of": int(p[9]),
                 "nent": int(p[10]), "title": " ".join(p[11:]), "entrants": []}
            out.append(t); by[t["tid"]] = t
        elif p[0] == "TE" and len(p) >= 3 and int(p[1]) in by:
            by[int(p[1])]["entrants"].append(" ".join(p[2:]))
        elif p[0] == "TS" and len(p) >= 8 and int(p[1]) in by:
            by[int(p[1])]["style"] = {"cloth": int(p[2]), "frame": int(p[3]), "body": int(p[4]), "set": int(p[5]),
                                      "leather": p[6], "metal": p[7]}
    # THE TABLE, which LIST does not carry (a change to it needs a relay
    # restart): the last one set for each, from tournaments.jsonl, read only
    # (Mark, 2026-10-16: "my attempts in the app to save the 6ft table are not
    # working - it keeps reverting to tournament" -- it was saved; the page
    # could not see it)
    if out:
        tables = {}
        try:
            with open(os.path.join(A.store, "tournaments.jsonl")) as f:
                for line in f:
                    if '"table"' not in line:
                        continue
                    try:
                        ev = json.loads(line)
                        tables[int(ev["tid"])] = int(ev["table"])
                    except (ValueError, KeyError, TypeError):
                        pass
        except OSError:
            pass
        for t in out:
            t["table"] = tables.get(t["tid"], -1)
    # SIGN-UPS TURNED AWAY, and why (2026-10-13: a night's refusals went
    # unseen) -- the relay's count since it started; it needs the admin token
    tok = tourn_token()
    if tok and out:
        try:
            for l in relay_ask(f"MOTE2 TOURN {A.tourn_gid} REFUSED {tok}", until=b"END\n").splitlines():
                p = l.split(" ")
                if p[0] == "TR" and len(p) == 4 and int(p[1]) in by:
                    by[int(p[1])].setdefault("refused", {})[p[2]] = int(p[3])
        except (OSError, ValueError):
            pass
    return out


def tourn_accounts():
    """every proved account the results know (by the app's short hash of it,
    never the number), by the name it last played under -- who the admin can enter by hand (an account, so the room's door
    knows them on the night)"""
    try:
        with LOCK:
            m = MODEL.get()
        return sorted(({"acct": p["id"][2:], "name": p["name"]} for p in m["players"]
                       if p.get("proved") and str(p["id"]).startswith("a:")),
                      key=lambda a: a["name"].lower())
    except Exception:
        return []


# each game's rulesets (CueVR rules_stops): UK 8-ball 5, 9-ball 1, snooker 5
TOURN_RULES_N = {0: 5, 2: 1, 6: 5}


def tourn_token():
    try:
        with open(A.tourn_token_file) as f:
            return f.read().strip()
    except OSError:
        return ""


def tourn_create(spec):
    tok = tourn_token()
    if not tok:
        return 503, {"error": "creating tournaments is off on this server (no admin token)"}
    try:
        start = int(spec["start"]); kind = int(spec["kind"]); size = int(spec["size"])
        bo = int(spec["best_of"]); fbo = int(spec["final_best_of"]); close = int(spec.get("close", start - 60))
        title = str(spec.get("title", "")).strip()[:48]
        st = spec.get("style") or {}
        # THE BODY too (Mark, 2026-10-16: "I need to be able to set the body type too (cabinet in
        # this case)"): CueVR's CUEVR_FRAMES index, -1 the game's own
        body = int(st.get("body", -1))
        if not -1 <= body < 16:
            body = -1
        style = {"cloth": int(st.get("cloth", 9)), "frame": int(st.get("frame", 7)), "body": body, "set": int(st.get("set", 0))}
        for k in ("leather", "metal"):
            v = str(st.get(k, "") or "").lstrip("#")
            if len(v) == 6 and all(c in "0123456789abcdefABCDEF" for c in v):
                style[k] = v.upper()
    except (KeyError, TypeError, ValueError):
        return 400, {"error": "the form is not complete"}
    if not 0 <= int(spec.get("rules", 0)) < TOURN_RULES_N.get(kind, 1):
        return 400, {"error": "that game has no such rules"}
    try:
        table = int(spec.get("table", -1))      # the bed every match is played on; -1 the game's TOURNAMENT
    except (TypeError, ValueError):
        return 400, {"error": "the form is not complete"}
    if not -1 <= table < 32:
        return 400, {"error": "that is not a table"}
    if not title:
        return 400, {"error": "give it a title"}
    if start < time.time() + 120:
        return 400, {"error": "the start must be at least two minutes from now"}
    body = json.dumps({"start": start, "kind": kind, "rules": int(spec.get("rules", 0)), "size": size, "best_of": bo,
                       "final_best_of": fbo, "close": close, "title": title, "ver": str(spec.get("ver", A.tourn_ver)),
                       "gid": str(A.tourn_gid), "style": style, "table": table})
    ans = relay_ask(f"MOTE2 TOURN {A.tourn_gid} CREATE {tok}", body).strip()
    if ans.startswith("CREATE OK"):
        return 200, {"tid": int(ans.split()[2]), "answer": ans}
    return 400, {"error": {"CREATE DENIED": "the relay refused the admin token", "CREATE PAST": "that start has passed",
                           "CREATE BAD": "the relay did not accept those settings"}.get(ans, ans or "no answer from the relay")}


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

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path not in ("/api/tourn", "/api/tourn/cancel", "/api/tourn/edit", "/api/tourn/add", "/api/tourn/remove"):
            return self._send(404, "text/plain", b"not here\n")
        if not self._ok(q):
            return self._send(403, "text/plain", b"no\n")
        try:
            n = min(int(self.headers.get("Content-Length") or 0), 8192)
            raw = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        except (ValueError, OSError):
            raw = ""
        if u.path == "/api/tourn":
            try:
                spec = json.loads(raw or "{}")
            except ValueError:
                spec = {}
            code, out = tourn_create(spec if isinstance(spec, dict) else {})
            return self._send(code, "application/json", json.dumps(out).encode())
        tok = tourn_token()
        if not tok:
            return self._send(503, "application/json", b'{"error":"no admin token"}')
        try:
            tid = int((q.get("tid") or ["0"])[0])
        except ValueError:
            tid = 0
        if u.path in ("/api/tourn/add", "/api/tourn/remove"):
            # A PLAYER ENTERED OR TAKEN OFF BY HAND: added by account (its
            # name the results' latest), taken off by place and name
            try:
                spec = json.loads(raw or "{}")
                if u.path.endswith("add"):
                    acct = str(spec["acct"])
                    acc = next(a for a in tourn_accounts() if a["acct"] == acct)
                    verb, ed = "ADD", {"tid": tid, "acct": acct, "name": acc["name"]}
                else:
                    verb, ed = "REMOVE", {"tid": tid, "n": int(spec["n"]), "name": str(spec["name"])}
            except (StopIteration, KeyError, TypeError, ValueError):
                return self._send(400, "application/json", b'{"error":"pick a player"}')
            ans = relay_ask(f"MOTE2 TOURN {A.tourn_gid} ADMIN {tok} {verb}", json.dumps(ed)).strip()
            why = {"ADMIN OK": "", "ADMIN ALREADY": "they are already in it", "ADMIN FULL": "it is full",
                   "ADMIN NOTOPEN": "the draw has been made", "ADMIN CHANGED": "the list changed: look again",
                   "ADMIN BAD": "that did not make sense", "ADMIN NOACCOUNT": "the server cannot find their account"}
            return self._send(200 if ans == "ADMIN OK" else 400, "application/json",
                              json.dumps({"answer": ans, "error": why.get(ans, ans)}).encode())
        if u.path == "/api/tourn/edit":
            # CHANGED while it takes entries: the rules, the table, the body and the number of places
            try:
                spec = json.loads(raw or "{}")
                kind = next(t["kind"] for t in tourn_list() if t["tid"] == tid)
                ed = {"tid": tid}
                if "rules" in spec:
                    ed["rules"] = int(spec["rules"])
                    if not 0 <= ed["rules"] < TOURN_RULES_N.get(kind, 1):
                        return self._send(400, "application/json", b'{"error":"that game has no such rules"}')
                if "size" in spec:
                    ed["size"] = int(spec["size"])
                if "body" in spec:      # the body: the style as it is, with this body
                    st = dict(next(t["style"] for t in tourn_list() if t["tid"] == tid))
                    st["body"] = int(spec["body"])
                    if not -1 <= st["body"] < 16:
                        return self._send(400, "application/json", b'{"error":"that is not a body"}')
                    ed["style"] = st
                if "table" in spec:
                    ed["table"] = int(spec["table"])
                    if not -1 <= ed["table"] < 32:
                        return self._send(400, "application/json", b'{"error":"that is not a table"}')
            except (StopIteration, KeyError, TypeError, ValueError, OSError):
                return self._send(400, "application/json", b'{"error":"the change is not complete"}')
            ans = relay_ask(f"MOTE2 TOURN {A.tourn_gid} ADMIN {tok} EDIT", json.dumps(ed)).strip()
            why = {"ADMIN OK": "", "ADMIN TOOSMALL": "more have signed up than that",
                   "ADMIN NOTOPEN": "it has stopped taking entries", "ADMIN BAD": "the change is not complete"}
            return self._send(200 if ans == "ADMIN OK" else 400, "application/json",
                              json.dumps({"answer": ans, "error": why.get(ans, ans)}).encode())
        ans = relay_ask(f"MOTE2 TOURN {A.tourn_gid} CANCEL {tok} {tid}").strip()
        return self._send(200 if ans.endswith("OK") else 400, "application/json", json.dumps({"answer": ans}).encode())

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path not in ("/", "/api", "/api/history", "/api/model", "/api/server", "/api/player", "/api/match", "/api/tourn", "/api/tourn/view"):
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
        if u.path == "/api/tourn":
            try:
                return self._send(200, "application/json", json.dumps({"now": int(time.time()), "list": tourn_list(),
                                                                       "can_create": bool(tourn_token()),
                                                                       "accounts": tourn_accounts()}).encode())
            except OSError as e:
                return self._send(502, "application/json", json.dumps({"error": f"the relay did not answer ({e})"}).encode())
        if u.path == "/api/tourn/view":
            # ONE TOURNAMENT'S DRAW (Mark, 2026-10-17: "show tournament results
            # tree"): the organiser's VIEW, names only
            tok = tourn_token()
            try:
                tid = int((q.get("tid") or ["0"])[0])
                ans = relay_ask(f"MOTE2 TOURN {A.tourn_gid} ADMIN {tok} VIEW", json.dumps({"tid": tid})).strip() if tok else ""
            except (ValueError, OSError) as e:
                return self._send(502, "application/json", json.dumps({"error": f"the relay did not answer ({e})"}).encode())
            if not ans.startswith("ADMIN OK "):
                return self._send(404, "application/json", json.dumps({"error": ans or "no admin token"}).encode())
            return self._send(200, "application/json", ans[len("ADMIN OK "):].encode())
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
    ap.add_argument("--forfeits-from", default=None, help="walk-outs count from this UTC time on (default: the relay's WALKOUTS_COUNT_FROM)")
    ap.add_argument("--state", default="", help="keep a sample a minute here (history)")
    ap.add_argument("--journal-file", default="", help="read the relay's log from this file, not journalctl (tests)")
    ap.add_argument("--backups", default="", help="the daily snapshots' folder, listed on the Server page")
    ap.add_argument("--tourn-token-file", default="", help="the relay's tournament admin token (its --admin-token-file): makes the Tournaments form work")
    ap.add_argument("--tourn-gid", type=int, default=0x43554575, help="the game id tournaments are made for (7.6.5: CUEu; a tournament is shown to every version of its game)")
    ap.add_argument("--tourn-ver", default="7.5", help="the game version entrants must be on")
    A = ap.parse_args()
    MODEL = Model(A.store, A.relay, GID, A.journal_file, A.backups, forfeits_from=A.forfeits_from)
    with open(A.token_file, "rb") as f:
        TOKEN = f.read().strip()
    if len(TOKEN) < 24:
        raise SystemExit("the token is too short (24 characters at least)")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(A.cert, A.key)
    # THE HANDSHAKE ON THE CONNECTION'S OWN THREAD, with a time limit
    # (2026-10-17, Mark: "My android app isn't loading anything any more"): the
    # listening socket was wrapped, so every TLS handshake ran on the one thread
    # that accepts, with no timeout -- a phone that opened a connection and went
    # quiet held it for ever, the queue filled and nothing else got in
    class TLSServer(ThreadingHTTPServer):
        daemon_threads = True
        def finish_request(self, request, client_address):
            request.settimeout(15)
            try:
                tls = ctx.wrap_socket(request, server_side=True)
            except (OSError, ssl.SSLError):
                try: request.close()
                except OSError: pass
                return
            self.RequestHandlerClass(tls, client_address, self)
    H.timeout = 30
    srv = TLSServer((A.bind, A.port), H)
    if A.state:
        threading.Thread(target=sampler, daemon=True).start()
    threading.Thread(target=seeder, daemon=True).start()
    print(f"mote-stats on {A.bind}:{A.port}, results from {A.store}, relay {A.relay}, history {A.state or 'off'}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
