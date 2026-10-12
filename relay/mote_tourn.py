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

THE DECISIONS (Mark's, 2026-10-12): knockout, 4 or 8; a race to 2, the final
to 3 (the tournament's own numbers when it says otherwise); a random draw from a
seed every entrant can check; at least 3 present or the night is called off; a
no-show has 10 minutes; a player who drops out of a match is waited for 5, then
walked over -- never handed to the computer; a disputed frame is replayed; the
times are UTC here and each headset shows its own.
"""
import json
import os
import random
import time

OPEN_BEFORE = 15 * 60       # the room opens this long before the start
NO_SHOW = 10 * 60           # an entrant not there this long after the start (or a call) is walked over
PAUSE = 5 * 60              # a player gone from a match in play is waited for this long
MIN_PRESENT = 3             # fewer at the start and the night is called off
TABLES = 4
MAX_ENTRANTS = 8


def bracket_size(n):
    return 4 if n <= 4 else 8


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
                "entrants": [e["name"] for e in self.entrants],
                "draw": {str(n): self.name_of(u) for n, u in sorted(self.draw.items())},
                "present": sorted(n for n, u in self.draw.items() if u in self.present) if self.draw
                           else len([e for e in self.entrants if e["uid"] in self.present]),
                "matches": [m.view() for m in self.matches], "champion": self.champion, "why_off": self.why_off}


class Organiser:
    def __init__(self, store, now=time.time, log=lambda *a: None):
        self.path = os.path.join(store, "tournaments.jsonl") if store else None
        self.now = now
        self.log = log
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
        elif e == "champion":
            t.status, t.champion = "done", ev["num"]
            self.say(t.tid, f"CHAMPION {t.champion}")

    # ---- THE DRAW ---------------------------------------------------------
    def build(self, t, at):
        """the empty bracket for the draw, then the first round from it"""
        size = max(t.draw) if t.draw else 0
        size = 4 if size <= 4 else 8
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
    def seeded_draw(tid, uids):
        """the draw: the entrants shuffled by a generator seeded from the
        tournament's id, so anyone holding the entry list can make it again.
        Byes go one to a match, never two together."""
        size = bracket_size(len(uids))
        order = list(uids)
        rng = random.Random(f"cuevr-tournament-{tid}")
        rng.shuffle(order)
        byes = size - len(order)
        matches = list(range(size // 2))
        rng.shuffle(matches)
        bye_m = set(matches[:byes])
        draw, it = {}, iter(order)
        for i in range(size // 2):
            draw[2 * i + 1] = next(it)
            if i not in bye_m:
                draw[2 * i + 2] = next(it)
        return draw

    # ---- WHAT PLAYERS AND ADMINS ASK --------------------------------------
    def create(self, spec):
        """a new tournament from the admin's spec; its id or an error word"""
        try:
            start = float(spec["start"])
            kind, size = int(spec["kind"]), int(spec.get("size", 8))
            bo, fbo = int(spec.get("best_of", 3)), int(spec.get("final_best_of", 5))
        except (KeyError, TypeError, ValueError):
            return None, "BAD"
        if size not in (4, 8) or not (1 <= bo <= 35 and bo % 2) or not (1 <= fbo <= 35 and fbo % 2):
            return None, "BAD"
        if start < self.now() + 60:
            return None, "PAST"
        tid = 1 + max(self.t, default=0)
        ev = {"e": "created", "tid": tid, "gid": str(spec.get("gid", ""))[:16],
              "title": str(spec.get("title", ""))[:48], "kind": kind, "rules": int(spec.get("rules", 0)),
              "size": size, "best_of": bo, "final_best_of": fbo, "start": start,
              "close": float(spec.get("close", start - OPEN_BEFORE)), "ver": str(spec.get("ver", ""))[:16],
              "venue": str(spec.get("venue", "club"))[:16]}
        self.record(ev)
        self.log(f"tournament {tid}: created, {ev['title']!r} kind {kind} size {size} at {time.strftime('%Y-%m-%d %H:%M', time.gmtime(start))} UTC")
        return tid, "OK"

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
        if self.now() > t.close or t.status != "open":
            return "CLOSED"
        if t.ver and ver != t.ver:
            return "VERSION"
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

    def listing(self, gid):
        """the tournaments a board shows: not finished more than a day ago"""
        now = self.now()
        return [t.view() for t in sorted(self.t.values(), key=lambda t: t.start)
                if (not gid or t.gid == gid) and (t.status not in ("done", "off") or now - t.start < 86400)]

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
        if m is None or m.state == "done":
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
        draw = self.seeded_draw(t.tid, [e["uid"] for e in t.entrants])
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
                    code = f"T{t.tid}{m.mid}"[:8]
                    self.record({"e": "called", "tid": t.tid, "mi": i, "table": table, "code": code})
