#!/usr/bin/env python3
"""mote_tourn.py -- the club's tournaments (CueVR 7.5), run by the relay.

The relay is the organiser: it keeps the tournaments, makes the draw, calls the
matches to the tables, follows the results both players' games agree, walks
over the absent and crowns the winner. The headsets show and play what it
decides; none of them is in charge (Mark, 2026-10-12, plan in the 7.5 doc).

Everything that happens is one line in <store>/tournaments.jsonl, appended and
never rewritten. The state is those lines replayed, so a relay that restarts in
the middle of a night picks it up where it was. Presence -- who is in the room,
who is at a table -- is not written: it is live, and after a restart everyone
comes back in.

No sockets here and no clock of its own: the relay passes the time in and turns
the returned announcements into lines to the tournament's room. That is what
lets relay/test_tourn.py run a whole night in a fraction of a second.

THE DECISIONS (Mark's, 2026-10-12): knockout, 4, 8 or 16 (16 added the same night); a race to 2, the final
to 3 (the tournament's own numbers when it says otherwise); a random draw from a
seed every entrant can check; at least 3 present or the night is called off; a
no-show has 10 minutes; a player who drops out of a match is waited for 5, then
walked over -- never handed to the computer; a disputed frame is replayed; the
times are UTC here and each headset shows its own.
"""
import hashlib
import json
import os
import random
import time

OPEN_BEFORE = 15 * 60       # the room opens this long before the start
NO_SHOW = 10 * 60           # an entrant not there this long after the start (or a call) is walked over
PAUSE = 5 * 60              # a player gone from a match in play is waited for this long
MIN_PRESENT = 3             # fewer at the start and the night is called off
TABLES = 4
MAX_ENTRANTS = 16         # (Mark, 10-12: "we might find we have more than 8 players on our first night")
# A TABLE'S ROOM CODE: four characters from the game's own alphabet
# (CueVR's CUEVR_CODE_ALPHABET, CUEVR_CODE_LEN 4) -- the game keeps no more,
# and "T1R1M1" cut to "T1R1" would have put every first-round match in one room
CODE_ALPHA = "23456789BCDFGHJKLMNPQRSTVWXYZ"


def bracket_size(n):
    return 4 if n <= 4 else 8 if n <= 8 else 16


class Match:
    """one match of the draw: a and b are draw numbers (1..size), or None for a
    place still to be decided or a bye"""
    __slots__ = ("mid", "rnd", "a", "b", "table", "code", "state", "score", "winner",
                 "why", "called_at", "paused_at", "paused_uid", "at_table", "frames", "best_of", "ready_at")

    def __init__(self, mid, rnd, best_of):
        self.mid, self.rnd, self.best_of = mid, rnd, best_of
        self.a = self.b = None
        self.table = 0
        self.code = ""
        self.state = "drawn"        # drawn / called / playing / paused / done
        self.score = [0, 0]
        self.winner = None          # a draw number
        self.why = ""               # "", "walkover", "bye", "default"
        self.called_at = None
        self.ready_at = None        # when both places were filled: a no-show is timed from here
        self.paused_at = None
        self.paused_uid = None
        self.at_table = set()       # uids in the table's room (live)
        self.frames = 0

    def view(self):
        return {"mid": self.mid, "rnd": self.rnd, "a": self.a, "b": self.b, "table": self.table,
                "state": self.state, "score": list(self.score), "winner": self.winner, "why": self.why,
                "best_of": self.best_of}


class Tournament:
    # A TOURNAMENT CARRIED OVER BY A RELOAD keeps its old __dict__ (the relay
    # swaps only the class), so a field added since reads from here: the
    # table was missing on the live one when it came in (2026-10-16)
    table = -1

    def __init__(self, ev):
        self.tid = ev["tid"]
        self.gid = str(ev.get("gid", ""))
        self.title = str(ev.get("title", ""))[:48]
        self.kind = int(ev.get("kind", 0))
        self.rules = int(ev.get("rules", 0))
        self.size = int(ev.get("size", 8))
        self.best_of = int(ev.get("best_of", 3))
        self.final_best_of = int(ev.get("final_best_of", 5))
        self.start = float(ev["start"])            # UTC seconds
        self.close = float(ev.get("close", self.start))
        self.ver = str(ev.get("ver", ""))
        self.venue = str(ev.get("venue", "club"))
        # THE STYLE, the same for everyone (Mark, 2026-10-12: "a fixed
        # tournament style and ball set -- defined in the tournament setup"):
        # the game's own indices -- cloth, timber, body (-1 the game's own),
        # ball set; and the hide and fittings (Mark: "make sure the
        # leather/metal is set on the style too"), each "RRGGBB" or "" for
        # the body's own
        self.style = clean_style(ev.get("style"))
        # THE TABLE, the same for every match (Mark, 2026-10-16: "Which table
        # is the tournament tonight? 6ft or 7ft? - this is something i need to
        # be able to set as tournament admin"): the game's bed number
        # (CUE_TAB_*: 1 the TOURNAMENT spec, 7 the 6 ft, ...), -1 the game's
        # own TOURNAMENT. Before this the host's own choice was played.
        self.table = clean_table(ev.get("table", -1))
        self.entrants = []                         # [{"uid", "name"}] in order of entry
        self.status = "open"                       # open / room / running / done / off
        self.draw = {}                             # draw number -> uid
        self.matches = []                          # in bracket order
        self.champion = None                       # a draw number
        self.present = set()                       # uids in the room (live)
        self.why_off = ""

    # -- who is who
    def name_of(self, uid):
        for e in self.entrants:
            if e["uid"] == uid:
                return e["name"]
        return "?"

    def num_of(self, uid):
        for n, u in self.draw.items():
            if u == uid:
                return n
        return None

    def entered(self, uid):
        return any(e["uid"] == uid for e in self.entrants)

    def view(self):
        """what the boards and the menus are told: everything public, no accounts"""
        return {"tid": self.tid, "title": self.title, "kind": self.kind, "rules": self.rules, "size": self.size,
                "best_of": self.best_of, "final_best_of": self.final_best_of, "start": int(self.start),
                "close": int(self.close), "ver": self.ver, "venue": self.venue, "status": self.status,
                "style": dict(self.style), "table": self.table,
                "entrants": [e["name"] for e in self.entrants],
                "draw": {str(n): self.name_of(u) for n, u in sorted(self.draw.items())},
                "present": sorted(n for n, u in self.draw.items() if u in self.present) if self.draw
                           else len([e for e in self.entrants if e["uid"] in self.present]),
                "matches": [m.view() for m in self.matches], "champion": self.champion, "why_off": self.why_off}


def clean_table(v):
    """a bed number the game knows (CUE_TAB_*), or -1 for the game's own TOURNAMENT"""
    try:
        v = int(v)
    except (TypeError, ValueError):
        return -1
    return v if 0 <= v < 32 else -1


def same_game(a, b):
    """the same game across its versions: a tournament made under one CueVR
    is shown to every later one (the game number's family, its top 24 bits)"""
    try:
        return (int(a) >> 8) == (int(b) >> 8)
    except (TypeError, ValueError):
        return str(a) == str(b)


def hexcol(v):
    """a colour as RRGGBB, from "RRGGBB"/"#RRGGBB" or [r, g, b] 0..1; "" for none (the body's own)"""
    if isinstance(v, (list, tuple)) and len(v) == 3:
        try:
            return "".join(f"{max(0, min(255, round(float(x) * 255))):02X}" for x in v)
        except (TypeError, ValueError):
            return ""
    s = str(v or "").lstrip("#").upper()
    return s if len(s) == 6 and all(ch in "0123456789ABCDEF" for ch in s) else ""


def clean_style(st):
    st = st if isinstance(st, dict) else {}
    out = {}
    for k, d in (("cloth", 9), ("frame", 7), ("body", -1), ("set", 0)):
        try:
            out[k] = int(st.get(k, d))
        except (TypeError, ValueError):
            out[k] = d
    out["leather"], out["metal"] = hexcol(st.get("leather")), hexcol(st.get("metal"))
    return out


def word(s):
    """a name or reason as one line's tail, printable and never empty"""
    s = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in str(s)).strip()
    return s or "-"


def state_lines(t):
    """THE STATE FOR THE GAME, as short lines it reads with sscanf (it has no
    JSON reader, and the club cuts a line at 200 characters): BEGIN, the
    title, the entrants (before the draw) or the draw, every match, END.
    Numbers are 0 where there is none; a reason is one word."""
    v = t.view()
    out = [f"TS BEGIN {t.tid} {t.status} {t.kind} {t.rules} {t.size} {t.best_of} {t.final_best_of} "
           f"{int(t.start)} {t.champion or 0} {word(t.why_off).replace(' ', '_')}",
           f"TS TITLE {word(t.title)}",
           f"TS STYLE {t.style['cloth']} {t.style['frame']} {t.style['body']} {t.style['set']} "
           f"{t.style['leather'] or '-'} {t.style['metal'] or '-'}",
           f"TS TABLE {t.table}"]
    if t.draw:
        for n, u in sorted(t.draw.items()):
            out.append(f"TS DRAW {n} {1 if u in t.present else 0} {word(t.name_of(u))[:23]}")
    else:
        for e in t.entrants:
            out.append(f"TS ENT {1 if e['uid'] in t.present else 0} {word(e['name'])[:23]}")
    for m in v["matches"]:
        out.append(f"TS M {m['mid']} {m['rnd']} {m['a'] or 0} {m['b'] or 0} {m['table']} {m['state']} "
                   f"{m['score'][0]} {m['score'][1]} {m['winner'] or 0} {m['why'] or '-'} {m['best_of']}")
    out.append("TS END")
    return out


class Organiser:
    def __init__(self, store, now=time.time, log=lambda *a: None, taken=lambda code: False):
        self.path = os.path.join(store, "tournaments.jsonl") if store else None
        self.now = now
        self.log = log
        self.taken = taken      # is this code a room already (the relay's own rooms)
        self.rng = random.SystemRandom()
        self.t = {}             # tid -> Tournament
        self.out = []           # announcements waiting for the relay: (tid, line)
        self.replaying = False
        self.load()

    # ---- THE RECORD -------------------------------------------------------
    def load(self):
        if not self.path or not os.path.exists(self.path):
            return
        self.replaying = True
        n = 0
        with open(self.path, encoding="ascii", errors="replace") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue            # a half-written last line: the next write starts a fresh one
                if isinstance(ev, dict) and "e" in ev:
                    self.apply(ev)
                    n += 1
        self.replaying = False
        self.out.clear()
        # A RESTART DROPS EVERYONE: a match in play is waited for from now,
        # exactly as if both had dropped out together
        at = self.now()
        for t in self.t.values():
            for m in t.matches:
                if m.state == "playing":
                    m.state, m.paused_at, m.paused_uid = "paused", at, None
        if n:
            self.log(f"tournaments: {len(self.t)} from {n} events")

    def record(self, ev):
        ev.setdefault("at", round(self.now(), 1))
        if self.path:
            try:
                with open(self.path, "a", encoding="ascii", errors="replace") as f:
                    f.write(json.dumps(ev, sort_keys=True) + "\n")
            except OSError as e:
                self.log(f"tournaments: could not write ({type(e).__name__})")
        self.apply(ev)

    def say(self, tid, line):
        if not self.replaying:
            self.out.append((tid, line))

    def take(self):
        out, self.out = self.out, []
        return out

    # ---- EVERYTHING THAT HAPPENS, AS IT HAPPENS ---------------------------
    def apply(self, ev):
        e = ev["e"]
        if e == "created":
            self.t[ev["tid"]] = Tournament(ev)
            return
        t = self.t.get(ev.get("tid"))
        if t is None:
            return
        if e == "edited":
            # CHANGED BY THE ADMIN while it takes entries (Mark, 2026-10-13: "update the
            # current tournament to use ultimate pool rules and to increase the max player count to 16")
            for k in ("title", "rules", "size", "best_of", "final_best_of"):
                if k in ev:
                    setattr(t, k, ev[k])
            if "style" in ev:
                t.style = clean_style(ev["style"])
            if "table" in ev:
                t.table = clean_table(ev["table"])
            return
        if e == "entered":
            if not t.entered(ev["uid"]):
                t.entrants.append({"uid": ev["uid"], "name": ev["name"]})
        elif e == "withdrawn":
            t.entrants = [x for x in t.entrants if x["uid"] != ev["uid"]]
        elif e == "opened":
            t.status = "room"
            self.say(t.tid, "OPEN")
        elif e == "off":
            t.status, t.why_off = "off", ev.get("why", "")
            self.say(t.tid, f"OFF {t.why_off}")
        elif e == "drawn":
            t.status = "running"
            t.draw = {int(n): u for n, u in ev["draw"].items()}
            self.build(t, ev["at"])
            self.say(t.tid, "DRAWN")
        elif e == "called":
            m = t.matches[ev["mi"]]
            m.state, m.table, m.code, m.called_at = "called", ev["table"], ev["code"], ev["at"]
            self.say(t.tid, f"CALL {m.mid} {m.table} {m.a} {m.b} {m.code}")
        elif e == "playing":
            m = t.matches[ev["mi"]]
            m.state, m.paused_at, m.paused_uid = "playing", None, None
        elif e == "paused":
            m = t.matches[ev["mi"]]
            m.state, m.paused_at, m.paused_uid = "paused", ev["at"], ev.get("uid")
            self.say(t.tid, f"PAUSED {m.mid}")
        elif e == "frame":
            m = t.matches[ev["mi"]]
            m.score = list(ev["score"])
            m.frames += 1
            self.say(t.tid, f"SCORE {m.mid} {m.score[0]} {m.score[1]}")
        elif e == "replay":
            self.say(t.tid, f"REPLAY {t.matches[ev['mi']].mid} {ev.get('frame', 0)}")
        elif e == "won":
            m = t.matches[ev["mi"]]
            m.state, m.winner, m.why = "done", ev["winner"], ev.get("why", "")
            if ev.get("score"):
                m.score = list(ev["score"])
            self.say(t.tid, f"WON {m.mid} {m.winner} {m.why or 'played'}")
            self.advance(t, m, ev["at"])
        elif e == "undone":
            # A RESULT TAKEN BACK by the admin (the first night: Jimmy walked
            # over while REJOIN had him in his finished match's room): the
            # match waits for its call again, its no-show clock from now, and
            # whatever it sent on comes back out of the next round -- that
            # match back to waiting, its call withdrawn
            m = t.matches[ev["mi"]]
            nxt = [x for x in t.matches if x.rnd == m.rnd + 1]
            if nxt and m.winner is not None:
                to = nxt[(int(m.mid.split("M")[1]) - 1) // 2]
                if to.a == m.winner: to.a = None
                if to.b == m.winner: to.b = None
                if to.state in ("called", "playing", "paused"):
                    to.state, to.code, to.table, to.called_at, to.score, to.frames = "drawn", "", 0, None, [0, 0], 0
                    self.say(t.tid, f"UNCALLED {to.mid}")
            m.state, m.winner, m.why, m.score, m.frames = "drawn", None, "", [0, 0], 0
            m.code, m.table, m.called_at, m.ready_at = "", 0, None, ev["at"]
            m.paused_at, m.paused_uid = None, None
            self.say(t.tid, f"UNDONE {m.mid}")
        elif e == "champion":
            t.status, t.champion = "done", ev["num"]
            self.say(t.tid, f"CHAMPION {t.champion}")

    # ---- THE DRAW ---------------------------------------------------------
    def build(self, t, at):
        """the empty bracket for the draw, then the first round from it"""
        size = max(t.draw) if t.draw else 0
        size = 4 if size <= 4 else 8 if size <= 8 else 16
        t.matches = []
        r, n = 1, size // 2
        while n >= 1:
            for i in range(n):
                bo = t.final_best_of if n == 1 else t.best_of
                t.matches.append(Match(f"R{r}M{i + 1}", r, bo))
            r, n = r + 1, n // 2
        first = [m for m in t.matches if m.rnd == 1]
        for i, m in enumerate(first):
            a, b = 2 * i + 1, 2 * i + 2
            m.a = a if a in t.draw else None
            m.b = b if b in t.draw else None
            m.ready_at = at
        # A BYE: a first-round place with nobody in it sends the other through
        for m in first:
            if (m.a is None) != (m.b is None):
                m.state, m.winner, m.why = "done", m.a or m.b, "bye"
                self.advance(t, m, at)

    def advance(self, t, m, at):
        """the winner into the next round's place; the final's winner is champion"""
        nxt = [x for x in t.matches if x.rnd == m.rnd + 1]
        if not nxt:
            if not self.replaying and t.champion is None:
                self.record({"e": "champion", "tid": t.tid, "num": m.winner})
            return
        idx = int(m.mid.split("M")[1]) - 1
        to = nxt[idx // 2]
        if idx % 2 == 0:
            to.a = m.winner
        else:
            to.b = m.winner
        if to.a is not None and to.b is not None:
            to.ready_at = at

    @staticmethod
    def seeded_draw(tid, uids, rating=None):
        """the draw: SEEDED by rating (Mark, 2026-10-13: "secretly use the
        overall ELO rating to seed players ... using 1500 for players that have
        not yet played"). The entrants ranked by rating, equal ratings in a
        random order (never the order they signed up in); seeds placed as a
        knockout seeds them -- 1 and 2 can meet only in the final, the top four
        only in the semi-finals -- so the byes of a short field go to the top
        seeds, one to a match. And SECRETLY: the halves, the quarters and the
        two sides of every match flipped at random, so a draw number says
        nothing about a seed. Random from a generator seeded by the
        tournament's id, so the same entries and ratings make the same draw."""
        rating = rating or (lambda u: 1500.0)
        size = bracket_size(len(uids))
        rng = random.Random(f"cuevr-tournament-{tid}")
        order = list(uids)
        rng.shuffle(order)                                  # equal ratings: a random order
        order.sort(key=lambda u: -float(rating(u)))         # stable: the shuffle stands between equals
        seeds = [1, 2]
        while len(seeds) < size:                            # 1 v 4, 2 v 3; then 1 v 8, 4 v 5, 2 v 7, 3 v 6 ...
            n = 2 * len(seeds)
            seeds = [x for sd in seeds for x in (sd, n + 1 - sd)]
        seeds = seeds[:size]

        def flip(lst):
            if len(lst) == 1:
                return lst
            h = len(lst) // 2
            a, b = flip(lst[:h]), flip(lst[h:])
            return b + a if rng.random() < 0.5 else a + b
        seeds = flip(seeds)
        draw = {}
        for pos, sd in enumerate(seeds, start=1):
            if sd <= len(order):
                draw[pos] = order[sd - 1]
        return draw

    def rating_of(self, uid):
        """the overall rating the stats app last sent for this account (by its
        short hash, as the app knows it); 1500 for one it has none for"""
        h = hashlib.sha256(str(uid).encode()).hexdigest()[:12]
        return getattr(self, "ratings", {}).get(h, 1500.0)

    # ---- WHAT PLAYERS AND ADMINS ASK --------------------------------------
    def create(self, spec):
        """a new tournament from the admin's spec; its id or an error word"""
        try:
            start = float(spec["start"])
            kind, size = int(spec["kind"]), int(spec.get("size", 8))
            bo, fbo = int(spec.get("best_of", 3)), int(spec.get("final_best_of", 5))
        except (KeyError, TypeError, ValueError):
            return None, "BAD"
        if size not in (4, 8, 16) or not (1 <= bo <= 35 and bo % 2) or not (1 <= fbo <= 35 and fbo % 2):
            return None, "BAD"
        if start < self.now() + 60:
            return None, "PAST"
        tid = 1 + max(self.t, default=0)
        ev = {"e": "created", "tid": tid, "gid": str(spec.get("gid", ""))[:16],
              "title": str(spec.get("title", ""))[:48], "kind": kind, "rules": int(spec.get("rules", 0)),
              "size": size, "best_of": bo, "final_best_of": fbo, "start": start,
              "close": float(spec.get("close", start - OPEN_BEFORE)), "ver": str(spec.get("ver", ""))[:16],
              "venue": str(spec.get("venue", "club"))[:16],
              "style": clean_style(spec.get("style")), "table": clean_table(spec.get("table", -1))}
        self.record(ev)
        self.log(f"tournament {tid}: created, {ev['title']!r} kind {kind} size {size} at {time.strftime('%Y-%m-%d %H:%M', time.gmtime(start))} UTC")
        return tid, "OK"

    # ---- THE ADMIN'S OWN REQUESTS (TOURN ADMIN <token> <verb>, the spec as
    # JSON on the next line): here and not in the relay, so a new one goes
    # out by RELOAD with nobody dropped
    def admin(self, verb, spec):
        if verb == "EDIT":
            return self.edit(spec)
        if verb == "VIEW":
            # ONE TOURNAMENT WHOLE, for the stats page's draw (Mark, 2026-10-17:
            # "I also want the app to show tournament results tree"): its
            # view() as one line of JSON
            try:
                t = self.t.get(int(spec["tid"]))
            except (KeyError, TypeError, ValueError):
                return "BAD"
            if t is None:
                return "NONE"
            return "OK " + json.dumps(t.view(), separators=(",", ":"))
        if verb == "PRESENT":
            # WHO IS IN A TOURNAMENT'S ROOM NOW, for the stats page's graph
            # (Mark, 2026-10-17: "show tournament room players too in blue"):
            # the room is not browsed, so LIST never counts them -- "OK <n>
            # <name|name|...>"
            names = [word(t.name_of(u))[:23] for t in self.t.values()
                     if t.status in ("room", "running") for u in t.present]
            return f"OK {len(names)} {'|'.join(n.replace('|', '/') for n in names) or '-'}"
        if verb == "UNDO":
            # A RESULT TAKEN BACK: spec {"tid", "mid"}; only while the match it
            # sent its winner on to has not been played
            try:
                tid, mid = int(spec["tid"]), str(spec["mid"])
            except (KeyError, TypeError, ValueError):
                return "BAD"
            t = self.t.get(tid)
            m = next((x for x in t.matches if x.mid == mid), None) if t else None
            if m is None:
                return "NONE"
            if m.state != "done":
                return "NOTDONE"
            nxt = [x for x in t.matches if x.rnd == m.rnd + 1]
            if nxt:
                to = nxt[(int(m.mid.split("M")[1]) - 1) // 2]
                if to.state == "done" or to.score != [0, 0]:
                    return "PLAYEDON"
            self.record({"e": "undone", "tid": tid, "mi": t.matches.index(m), "by": "admin"})
            self.log(f"tournament {tid}: {mid} undone by the admin")
            return "OK"
        if verb == "RECALL":
            # A MATCH CALLED AGAIN, to its table with a fresh room code (the first
            # night, 2026-10-16: Jimmy's game went back into his finished
            # match's room by REJOIN, which replaced his call to the next one,
            # and nothing times out while both players are present): spec
            # {"tid", "mid"}; only a match still waiting at its call
            try:
                tid, mid = int(spec["tid"]), str(spec["mid"])
            except (KeyError, TypeError, ValueError):
                return "BAD"
            t = self.t.get(tid)
            m = next((x for x in t.matches if x.mid == mid), None) if t else None
            if m is None:
                return "NONE"
            if m.state != "called":
                return "NOTCALLED"
            code = self.new_code()
            self.record({"e": "called", "tid": tid, "mi": t.matches.index(m), "table": m.table, "code": code, "by": "admin"})
            self.log(f"tournament {tid}: {mid} called again by the admin, table {m.table}, code {code}")
            return f"OK {code}"
        if verb == "ADD":
            return self.admin_add(spec)
        if verb == "SEEDS":
            # THE OVERALL RATINGS, from the stats app every few minutes: kept
            # in memory for the draw, never written, never shown
            try:
                r = {str(k).lower(): float(v) for k, v in dict(spec["r"]).items()
                     if len(str(k)) == 12 and all(c in "0123456789abcdef" for c in str(k).lower()) and 0 < float(v) < 5000}
            except (KeyError, TypeError, ValueError):
                return "BAD"
            if not hasattr(self, "ratings"):
                self.ratings = {}
            n0 = len(self.ratings)
            self.ratings.update(r)
            if len(self.ratings) != n0:
                self.log(f"tournaments: ratings for {len(self.ratings)} accounts, for seeding")
            return f"OK {len(self.ratings)}"
        if verb == "REMOVE":
            return self.admin_remove(spec)
        return "UNKNOWN"

    # ENTERED BY THE ADMIN (Mark, 2026-10-13: "add players in the admin app in
    # case they cannot make it to the blackboard and message me directly"):
    # by account, as a sign-up is, so the room's door knows them on the night.
    # Up to the draw -- after sign-up closes too -- and never past the size.
    def account_of(self, acct):
        """the Meta account behind the stats app's short hash of it ("a:" +
        sha256(account)[:12] -- the app never holds the number): looked for in
        the relay's results index, read-only, then its results file"""
        acct = str(acct).lower()
        if not (len(acct) == 12 and all(c in "0123456789abcdef" for c in acct)) or not self.path:
            return None
        h = lambda u: hashlib.sha256(str(u).encode()).hexdigest()[:12]
        store = os.path.dirname(self.path)
        for t in self.t.values():
            for e in t.entrants:
                if h(e["uid"]) == acct:
                    return e["uid"]
        try:
            import sqlite3
            c = sqlite3.connect(f"file:{os.path.join(store, 'results.db')}?mode=ro", uri=True, timeout=5)
            try:
                for (u,) in c.execute("SELECT DISTINCT uid FROM hside"):
                    if u and h(u) == acct:
                        return str(u)
            finally:
                c.close()
        except Exception:
            pass
        try:
            with open(os.path.join(store, "results.jsonl"), encoding="utf-8", errors="replace") as f:
                for line in f:
                    if '"uid' not in line:
                        continue
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue
                    us = [x.get("uid") for x in (r.get("a"), r.get("b")) if isinstance(x, dict)] + list(r.get("uids") or [])
                    for u in us:
                        if u and h(u) == acct:
                            return str(u)
        except OSError:
            pass
        return None

    def admin_add(self, spec):
        try:
            tid = int(spec["tid"])
            uid = str(spec["uid"]) if "uid" in spec else self.account_of(spec.get("acct", ""))
            name = str(spec.get("name", ""))[:23] or "PLAYER"
        except (KeyError, TypeError, ValueError):
            return "BAD"
        if uid is None:
            return "NOACCOUNT"
        if not (uid.isdigit() and len(uid) <= 20):
            return "BAD"
        t = self.t.get(tid)
        if t is None or t.status not in ("open", "room"):
            return "NOTOPEN"
        if t.entered(uid):
            return "ALREADY"
        if len(t.entrants) >= min(t.size, MAX_ENTRANTS):
            return "FULL"
        self.record({"e": "entered", "tid": tid, "uid": uid, "name": name, "by": "admin"})
        self.log(f"tournament {tid}: entered by the admin")
        return "OK"

    # ...and taken off: by place in the list AND name, so a list that changed
    # under the admin's finger never takes off the wrong player
    def admin_remove(self, spec):
        try:
            tid, n, name = int(spec["tid"]), int(spec["n"]), str(spec["name"])
        except (KeyError, TypeError, ValueError):
            return "BAD"
        t = self.t.get(tid)
        if t is None or t.status not in ("open", "room"):
            return "NOTOPEN"
        if not (0 <= n < len(t.entrants)) or t.entrants[n]["name"] != name:
            return "CHANGED"
        self.record({"e": "withdrawn", "tid": tid, "uid": t.entrants[n]["uid"], "by": "admin"})
        self.log(f"tournament {tid}: an entrant taken off by the admin")
        return "OK"

    def edit(self, spec):
        """a tournament still taking entries changed: title, rules, size, the
        races and the style; never fewer places than have entered"""
        try:
            tid = int(spec["tid"])
        except (KeyError, TypeError, ValueError):
            return "BAD"
        t = self.t.get(tid)
        if t is None or t.status != "open":
            return "NOTOPEN"
        ev = {"e": "edited", "tid": tid}
        try:
            if "title" in spec:
                ev["title"] = str(spec["title"])[:48]
            if "rules" in spec:
                ev["rules"] = int(spec["rules"])
                if not 0 <= ev["rules"] < 32:
                    return "BAD"
            if "size" in spec:
                ev["size"] = int(spec["size"])
                if ev["size"] not in (4, 8, 16):
                    return "BAD"
                if ev["size"] < len(t.entrants):
                    return "TOOSMALL"
            for k in ("best_of", "final_best_of"):
                if k in spec:
                    ev[k] = int(spec[k])
                    if not (1 <= ev[k] <= 35 and ev[k] % 2):
                        return "BAD"
            if "style" in spec:
                ev["style"] = clean_style(spec["style"])
            if "table" in spec:
                ev["table"] = int(spec["table"])
                if not -1 <= ev["table"] < 32:
                    return "BAD"
        except (TypeError, ValueError):
            return "BAD"
        if len(ev) == 2:
            return "BAD"
        self.record(ev)
        self.log(f"tournament {tid}: edited ({', '.join(k for k in ev if k not in ('e', 'tid'))})")
        return "OK"

    def cancel(self, tid, why="cancelled"):
        t = self.t.get(tid)
        if t is None or t.status in ("done", "off"):
            return "NONE"
        self.record({"e": "off", "tid": tid, "why": why})
        return "OK"

    def enter(self, tid, uid, name, ver):
        t = self.t.get(tid)
        if t is None or t.status in ("done", "off"):
            return "NONE"
        if t.entered(uid):
            return "ALREADY"
        # SIGN-UP CLOSES AT ITS OWN TIME (by default as the room opens, 15
        # minutes before the start), not at a state: a tournament made inside
        # that window takes entries up to the close it was given
        if self.now() > t.close or t.status not in ("open", "room"):
            return "CLOSED"
        # NO VERSION AT SIGN-UP (Mark, 2026-10-13: "we might have multiple
        # game updates between signup and the tournament so the check doesn't
        # even make sense"). It was an exact match on the version's name, and
        # every 7.5.1 game was refused a 7.5 tournament. Which game can play is
        # asked on the night, at the room's door (the relay: the current one)
        if len(t.entrants) >= min(t.size, MAX_ENTRANTS):
            return "FULL"
        self.record({"e": "entered", "tid": tid, "uid": uid, "name": name[:23]})
        return "OK"

    def withdraw(self, tid, uid):
        t = self.t.get(tid)
        if t is None or not t.entered(uid):
            return "NONE"
        if t.status not in ("open", "room"):
            return "CLOSED"
        self.record({"e": "withdrawn", "tid": tid, "uid": uid})
        return "OK"

    # ---- A PLAYER'S TOURNAMENTS (Mark, 2026-10-16: "the trophy must go in
    # their trophy cabinet and a history of events entered must be available in
    # the records section. The plate must have the tournament name date and
    # winner"; one trophy per tournament name, engraved each win; a TOURNAMENT
    # WINNERS board in the club). Lines the game reads with sscanf, the free
    # text last after a "|".
    @staticmethod
    def stage(t, rnd):
        """the round as the game words it: F the final, S a semi-final, Q a
        quarter-final, else R<n>"""
        rounds = {4: 2, 8: 3, 16: 4}.get(t.size, max((m.rnd for m in t.matches), default=1))
        left = rounds - rnd
        return "F" if left == 0 else "S" if left == 1 else "Q" if left == 2 else f"R{rnd}"

    def history_lines(self, uid, limit=40):
        """HISTORY <uid>: every tournament this account entered, newest first --
        "TH <tid> <start> <kind> <rules> <status> <result> <num> <size>|<title>",
        then its matches as "THM <tid> <stage> <mine> <theirs> <won> <why|-> <best_of>|<opponent>".
        result: CHAMPION, RUNNERUP, the stage it went out in (S, Q, R<n>),
        IN (still in), ENTERED (not drawn yet), OFF (called off), OUT (never played)"""
        out = []
        mine = [t for t in self.t.values() if t.entered(uid) or uid in t.draw.values()]
        for t in sorted(mine, key=lambda t: -t.start)[:limit]:
            num = next((n for n, u in t.draw.items() if u == uid), 0)
            if t.status == "off":
                res = "OFF"
            elif not t.draw:
                res = "ENTERED"
            elif t.champion and t.champion == num:
                res = "CHAMPION"
            else:
                lost = next((m for m in t.matches if m.state == "done" and num in (m.a, m.b) and m.winner != num), None)
                if lost is not None:
                    res = "RUNNERUP" if self.stage(t, lost.rnd) == "F" else self.stage(t, lost.rnd)
                elif t.status == "done":
                    res = "OUT"
                else:
                    res = "IN"
            out.append(f"TH {t.tid} {int(t.start)} {t.kind} {t.rules} {t.status} {res} {num} {t.size}|{word(t.title)}")
            for m in t.matches:
                if not num or num not in (m.a, m.b) or m.a is None or m.b is None or m.state != "done":
                    continue
                me_a = m.a == num
                opp = m.b if me_a else m.a
                s_me, s_them = (m.score[0], m.score[1]) if me_a else (m.score[1], m.score[0])
                out.append(f"THM {t.tid} {self.stage(t, m.rnd)} {s_me} {s_them} {1 if m.winner == num else 0} "
                           f"{m.why or '-'} {m.best_of}|{word(t.name_of(t.draw.get(opp)))[:23]}")
        return out

    def wins_lines(self, uid):
        """this account's titles, for its cabinet: "TWON <tid> <start>|<title>" (sent
        only to a game that proved the account, with the honours)"""
        return [f"TWON {t.tid} {int(t.start)}|{word(t.title)}"
                for t in sorted(self.t.values(), key=lambda t: t.start)
                if t.status == "done" and t.champion and t.draw.get(t.champion) == uid]

    def champions_lines(self, limit=20):
        """the club's TOURNAMENT WINNERS board, newest first: TCHAMP <tid> <start> <champion>|<title>"""
        done = [t for t in self.t.values() if t.status == "done" and t.champion]
        return [f"TCHAMP {t.tid} {int(t.start)} {word(t.name_of(t.draw.get(t.champion)))[:23].replace(' ', '_')}|{word(t.title)}"
                for t in sorted(done, key=lambda t: -t.start)[:limit]]

    def player_answer(self, sub, a):
        """a player's request the relay does not know itself (so a new one goes
        out by RELOAD): HISTORY <uid>"""
        if sub == "STATE":
            # ONE TOURNAMENT'S WHOLE DRAW for the headset's Records (Mark,
            # 2026-10-17): the TS lines the room's board is painted from
            try:
                t = self.t.get(int(a[1]))
            except (IndexError, ValueError):
                return "BAD\n"
            if t is None:
                return "NONE\nEND\n"
            return "".join(l + "\n" for l in state_lines(t))
        if sub == "HISTORY":
            uid = a[1] if len(a) > 1 and a[1].isdigit() and len(a[1]) <= 20 else ""
            if not uid:
                return "BAD\n"
            return "".join(l + "\n" for l in self.history_lines(uid)) + "END\n"
        return None

    def listing(self, gid):
        """the tournaments a board shows: not finished more than a day ago"""
        now = self.now()
        return [t.view() for t in sorted(self.t.values(), key=lambda t: t.start)
                if (not gid or same_game(t.gid, gid)) and (t.status not in ("done", "off") or now - t.start < 86400)]

    def may_enter_room(self, tid, uid):
        """PLAYER: an entrant, the room open; WATCH: anyone, the room open; else None"""
        t = self.t.get(tid)
        if t is None or t.status not in ("room", "running"):
            return None
        return "PLAYER" if t.entered(uid) and uid else "WATCH"

    # ---- PRESENCE (live, never written) ------------------------------------
    def room_in(self, tid, uid):
        t = self.t.get(tid)
        if t and uid and t.entered(uid):
            t.present.add(uid)
            self.say(tid, f"HERE {t.num_of(uid) or 0} {t.name_of(uid)}")

    def room_out(self, tid, uid):
        t = self.t.get(tid)
        if t and uid in t.present:
            t.present.discard(uid)
            for m in t.matches:
                if uid in m.at_table:
                    self.table_out(tid, m.mid, uid)

    def match_by_code(self, code):
        for t in self.t.values():
            for i, m in enumerate(t.matches):
                if m.code and m.code == code and m.state != "done":
                    return t, i, m
        return None, None, None

    def table_in(self, tid, mid, uid):
        t = self.t.get(tid)
        m = next((x for x in t.matches if x.mid == mid), None) if t else None
        if m is None or m.state == "done" or t.num_of(uid) not in (m.a, m.b):
            return
        m.at_table.add(uid)
        both = {t.draw.get(m.a), t.draw.get(m.b)} <= m.at_table
        if both and m.state in ("called", "paused"):
            self.record({"e": "playing", "tid": tid, "mi": t.matches.index(m)})

    def table_out(self, tid, mid, uid):
        t = self.t.get(tid)
        m = next((x for x in t.matches if x.mid == mid), None) if t else None
        if m is None:
            return
        m.at_table.discard(uid)
        if m.state == "playing":
            self.record({"e": "paused", "tid": tid, "mi": t.matches.index(m), "uid": uid})

    # ---- RESULTS, AS BOTH GAMES AGREED THEM --------------------------------
    def frame_agreed(self, tid, mid, winner_uid):
        t = self.t.get(tid)
        m = next((x for x in t.matches if x.mid == mid), None) if t else None
        if m is None or m.state == "done" or m.a is None or m.b is None:
            return
        n = t.num_of(winner_uid)
        if n not in (m.a, m.b):
            return
        sc = list(m.score)
        sc[0 if n == m.a else 1] += 1
        mi = t.matches.index(m)
        self.record({"e": "frame", "tid": tid, "mi": mi, "score": sc})
        need = m.best_of // 2 + 1
        if max(sc) >= need:
            self.record({"e": "won", "tid": tid, "mi": mi, "winner": m.a if sc[0] > sc[1] else m.b, "score": sc})

    def frame_disputed(self, tid, mid, frame):
        t = self.t.get(tid)
        m = next((x for x in t.matches if x.mid == mid), None) if t else None
        if m is None or m.state == "done":
            return
        self.record({"e": "replay", "tid": tid, "mi": t.matches.index(m), "frame": frame})

    def new_code(self):
        """a room code no other match of any tournament holds and no room has"""
        held = {m.code for x in self.t.values() for m in x.matches if m.code and m.state != "done"}
        for _ in range(200):
            code = "".join(self.rng.choice(CODE_ALPHA) for _ in range(4))
            if code not in held and not self.taken(code):
                return code
        return "".join(self.rng.choice(CODE_ALPHA) for _ in range(4))

    # ---- THE CLOCK ---------------------------------------------------------
    def tick(self):
        now = self.now()
        for t in list(self.t.values()):
            if t.status == "open" and now >= t.start - OPEN_BEFORE:
                self.record({"e": "opened", "tid": t.tid})
            if t.status == "room" and now >= t.start:
                self.start(t)
            if t.status == "running":
                self.run(t, now)

    def start(self, t):
        present = [e["uid"] for e in t.entrants if e["uid"] in t.present]
        if len(present) < MIN_PRESENT:
            self.record({"e": "off", "tid": t.tid, "why": f"only {len(present)} here"})
            return
        # EVERY ENTRANT IS DRAWN: one who is late has NO_SHOW from the start
        # to come in before their first match is walked over
        draw = self.seeded_draw(t.tid, [e["uid"] for e in t.entrants], self.rating_of)
        self.record({"e": "drawn", "tid": t.tid, "draw": {str(n): u for n, u in draw.items()}})

    def run(self, t, now):
        busy = {m.table for m in t.matches if m.state in ("called", "playing", "paused")}
        for i, m in enumerate(t.matches):
            if m.state == "done" or m.a is None or m.b is None:
                continue
            ua, ub = t.draw.get(m.a), t.draw.get(m.b)
            # ABSENT: from the start, or from the call, NO_SHOW to turn up
            since = max(m.ready_at or t.start, m.called_at or 0)
            if m.state in ("drawn", "called") and now - since >= NO_SHOW:
                ha, hb = ua in t.present, ub in t.present
                if ha != hb:
                    self.record({"e": "won", "tid": t.tid, "mi": i, "winner": m.a if ha else m.b, "why": "walkover"})
                    busy.discard(m.table)
                    continue
                if not ha and not hb and now - since >= 2 * NO_SHOW:
                    # NEITHER CAME: the lower number goes through by default,
                    # so the draw is not stuck (and is told so)
                    self.record({"e": "won", "tid": t.tid, "mi": i, "winner": min(m.a, m.b), "why": "default"})
                    busy.discard(m.table)
                    continue
            # DROPPED OUT: waited for PAUSE, then the one who stayed goes through
            if m.state == "paused" and m.paused_at is not None and now - m.paused_at >= PAUSE:
                stay = [n for n, u in ((m.a, ua), (m.b, ub)) if u in m.at_table or (u in t.present and u != m.paused_uid)]
                if len(stay) == 1:
                    self.record({"e": "won", "tid": t.tid, "mi": i, "winner": stay[0], "why": "walkover"})
                    busy.discard(m.table)
                elif not stay and now - m.paused_at >= 2 * PAUSE:
                    lead = m.a if m.score[0] >= m.score[1] else m.b
                    self.record({"e": "won", "tid": t.tid, "mi": i, "winner": lead, "why": "default"})
                    busy.discard(m.table)
                continue
            # CALLED: both in the room and a table free (the final to table 1)
            if m.state == "drawn" and ua in t.present and ub in t.present:
                final = not any(x.rnd > m.rnd for x in t.matches)
                free = [k for k in range(1, TABLES + 1) if k not in busy]
                if final:
                    free = [1] if 1 not in busy else []
                if free:
                    table = free[0]
                    busy.add(table)
                    code = self.new_code()
                    self.record({"e": "called", "tid": t.tid, "mi": i, "table": table, "code": code})
