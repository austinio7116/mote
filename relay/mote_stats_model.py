"""mote_stats_model.py -- everything mote_stats.py shows that is worked out from
the relay's files and its log: players by Meta account, matches with their
frames, the honours (and this month's race), the integrity queue, activity and
the server's health. Read only: nothing here ever writes to the relay's store.
"""
import collections, hashlib, json, os, re, socket, subprocess, time

# the shared facts of a result, as the relay compares them (mote_relay.py)
RES_SHARED = ("t", "game", "variant", "kind", "mode", "best_of", "frame",
              "score", "frames", "winner", "match_winner", "bnr", "golden")
RES_SIDE = ("shots", "pot_shots", "potted", "fouls", "best", "breaks", "shot")
# the relay's honours rules (mote_relay.py HON_KIND / HON_MIN_FRAMES)
HON_KIND = {7: 4, 4: 5, 5: 6, 6: 7, 10: 8, 23: 9}
HON_MIN_FRAMES = 20
HON_TITLE = ["Frames won", "Win rate", "Break-and-runs", "Clearances", "Straight pool high run",
             "Snooker highest break", "10-red highest break", "6-red highest break",
             "Billiards highest break", "3-red highest break"]
SHOT_SUM = ("att", "made", "long_att", "long_made", "safeties", "safe_ok", "banks", "breaks",
            "breaks_ok", "breaks_dry", "breaks_foul", "bnr", "runouts", "visits", "vis_pts",
            "b20", "b30", "b50", "b100", "strokes", "scoring", "innings", "cannons", "pots", "inoffs", "points")
FILES = ("results.jsonl", "results_disputed.jsonl", "results_unconfirmed.jsonl", "results_forfeits.jsonl")


def _jsonl(path):
    out = []
    try:
        with open(path, encoding="ascii", errors="replace") as f:
            for l in f:
                try:
                    out.append(json.loads(l))
                except ValueError:
                    pass
    except OSError:
        pass
    return out


def _clean(s, n=40):
    return "".join(c if 32 <= ord(c) < 127 else "?" for c in str(s if s is not None else ""))[:n]


def _ids(line, names):
    """each side's identity: its proved Meta account, else its name ("n:<name>");
    a doubles side ("A & B") is the team ("t:<names>"), never the account of
    whichever of the two sent the report"""
    ids = {i: "t:" + _clean(names[i], 60) for i in (0, 1) if i < len(names) and " & " in str(names[i])}
    for who in ("a", "b"):
        x = line.get(who)
        if isinstance(x, dict) and x.get("uid") and x.get("side") in (0, 1) and x["side"] not in ids:
            ids[x["side"]] = str(x["uid"])
    for i, u in enumerate(line.get("uids") or []):
        if u and i not in ids and i < 2:
            ids[i] = str(u)
    for i in (0, 1):
        if i not in ids:
            ids[i] = "n:" + _clean(names[i] if i < len(names) else "?", 30)
        elif ids[i][:2] != "t:":          # an account by a short one-way hash: the page never needs the number
            ids[i] = "a:" + hashlib.sha256(ids[i].encode()).hexdigest()[:12]
    return ids


def _report(line, side):
    """the sender's own full report for a side, if one was kept (secs, time, ver)"""
    for who in ("a", "b"):
        x = line.get(who)
        if isinstance(x, dict) and isinstance(x.get("r"), dict):
            return x["r"]
    return {}


class Model:
    """the stats, kept as running totals: each line results.jsonl gains is
    folded in once, and only the lines added since the last look are read
    (2026-10-11 -- re-reading and holding every line would have outgrown the
    service's 200 MB at about 14,000 lines). A file that is not the one being
    followed (shorter, or its first line changed) starts the totals again."""
    MATCHES_KEPT = 2000           # the newest matches, with their frames, for the Matches page

    def __init__(self, store, relay, gid, journal_file="", backups=""):
        self.store, self.relay, self.gid, self.journal_file, self.backups = store, relay, gid, journal_file, backups
        self._key = None
        self._model = None
        self._hon = (0.0, None)
        self._srv = (0.0, None)
        self._reset()

    def _reset(self):
        self.off, self.head = 0, None
        self.acc = {}
        self.matches = collections.deque(maxlen=self.MATCHES_KEPT)
        self.frames_pend = {}
        self.notable = collections.deque(maxlen=200)
        self.same_addr = collections.deque(maxlen=200)
        self.pairday = {}
        self.perday = {}
        self.hours = [0] * 24
        self.games_all, self.games_day = {}, {}
        self.frame_secs = {}
        self.seen_first = {}
        self.room_secs = collections.deque(maxlen=2000)
        self.monthp, self.month_names = {}, {}
        self.fixed = set()
        self.seen_lines = {}
        self.n_frames = self.n_matches = 0
        self.since = ""

    # ---- following results.jsonl --------------------------------------------
    def _follow(self):
        path = os.path.join(self.store, "results.jsonl")
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as f:
                head = hashlib.sha256(f.readline()).hexdigest()
                if self.head is not None and (head != self.head or size < self.off):
                    self._reset()
                self.head = head
                f.seek(self.off)
                while self.off < size:
                    raw = f.readline(size - self.off)
                    if not raw.endswith(b"\n"):          # a line still being written waits
                        break
                    self.off += len(raw)
                    try:
                        line = json.loads(raw)
                    except ValueError:
                        continue
                    if isinstance(line, dict):
                        self._fold(line)
        except OSError:
            pass

    def _acc(self, i, name, t):
        a = self.acc.get(i)
        if a is None:
            a = self.acc[i] = {"id": i, "proved": i.startswith("a:"), "team": i.startswith("t:"), "names": {},
                               "frames": 0, "fwon": 0, "matches": 0, "mwon": 0, "games": {}, "best": {},
                               "shot": {k: 0 for k in SHOT_SUM}, "pot_shots": 0, "potted": 0, "fouls": 0, "shots": 0,
                               "table_secs": 0.0, "first": t, "last": t, "versions": {}, "left": 0, "stayed_won": 0,
                               "form": collections.deque(maxlen=20), "opp": {}, "unverified": 0}
        a["names"][name] = max(a["names"].get(name, ""), t)
        a["first"] = min(a["first"], t); a["last"] = max(a["last"], t)
        return a

    def _fold(self, line):
        """one line of results.jsonl into every total"""
        for g in re.finditer(r"results_disputed\.jsonl lines? (\d+)[ab]?(?: \+ (\d+)[ab]?)?", str(line.get("repaired", ""))):
            self.fixed.update(int(n) for n in g.groups() if n)
        r = line.get("result") if isinstance(line.get("result"), dict) else {}
        t = str(line.get("time", ""))
        if r.get("t") not in ("frame", "match"):
            return
        names = ([_clean(x, 30) for x in (r.get("names") or [])] + ["?", "?"])[:2]
        # ONE FRAME, ONCE: a doubles frame is reported by both pairs of seats and
        # kept twice, the same frame, result and time -- counted the once
        dup = json.dumps([r["t"], line.get("gid"), line.get("room"), t, r.get("frame"), r.get("frames"),
                          r.get("score"), r.get("winner"), r.get("match_winner"), names])
        if dup in self.seen_lines:
            return
        self.seen_lines[dup] = None
        while len(self.seen_lines) > 2000:
            self.seen_lines.pop(next(iter(self.seen_lines)))
        ids = _ids(line, names)
        sides = r.get("sides") if isinstance(r.get("sides"), list) else [{}, {}]
        side = lambda i: sides[i] if i < len(sides) and isinstance(sides[i], dict) else {}
        rep = _report(line, 0)
        verified = bool(line.get("verified"))
        day = t[:10]
        self.since = min(self.since, t) if self.since else t
        for i in (0, 1):
            self.seen_first.setdefault(ids[i], day)
        for who in ("a", "b"):           # each sender's version, against their own account
            x = line.get(who)
            if isinstance(x, dict) and isinstance(x.get("r"), dict) and x.get("side") in (0, 1):
                v = _clean(x["r"].get("ver"), 12)
                if v:
                    acc = self._acc(ids[x["side"]], names[x["side"]], t)
                    acc["versions"][v] = max(acc["versions"].get(v, ""), t)
        a_, b_ = line.get("a"), line.get("b")
        if isinstance(a_, dict) and isinstance(b_, dict) and a_.get("addr") and a_.get("addr") == b_.get("addr"):
            self.same_addr.append({"time": t, "names": names, "game": _clean(r.get("game")),
                                   "room": _clean(line.get("room"), 8), "t": r.get("t")})
        pd = self.perday.setdefault(day, {"day": day, "frames": 0, "matches": 0, "players": set()})
        if r["t"] == "frame":
            self.n_frames += 1
            game = _clean(r.get("game")) or "?"
            fr = {"time": t, "frame": r.get("frame"), "game": game, "variant": _clean(r.get("variant")),
                  "kind": r.get("kind"), "score": r.get("score"), "winner": r.get("winner"),
                  "secs": rep.get("secs"), "bnr": r.get("bnr"), "golden": r.get("golden"),
                  "forfeit": line.get("forfeit"), "figures": line.get("figures", True),
                  "sides": [{k: side(i).get(k) for k in RES_SIDE} for i in (0, 1)],
                  "time_at": [((rep.get("sides") or [{}, {}])[i] or {}).get("time") if i < len(rep.get("sides") or []) else None for i in (0, 1)]}
            pend = self.frames_pend.setdefault((line.get("gid"), line.get("room")), [])
            pend.append(fr)
            del pend[:-64]                               # a room that never sends its match line cannot grow without end
            for i in (0, 1):
                acc = self._acc(ids[i], names[i], t)
                sd = side(i)
                won = r.get("winner") == i
                acc["frames"] += 1; acc["fwon"] += 1 if won else 0
                acc["unverified"] += 0 if verified else 1
                g = acc["games"].setdefault(game, {"f": 0, "w": 0, "best": 0})
                g["f"] += 1; g["w"] += 1 if won else 0
                g["best"] = max(g["best"], int(sd.get("best") or 0))
                for k in ("pot_shots", "potted", "fouls", "shots"):
                    acc[k] += int(sd.get(k) or 0)
                for k, v in (sd.get("shot") or {}).items():
                    if k in acc["shot"]:
                        acc["shot"][k] += int(v or 0)
                ta = fr["time_at"][i]
                if isinstance(ta, (int, float)):
                    acc["table_secs"] += ta
                o = acc["opp"].get(ids[1 - i])            # [frames, won, matches, won]: four numbers, not a dict each
                if o is None:
                    o = acc["opp"][ids[1 - i]] = [0, 0, 0, 0]
                o[0] += 1; o[1] += 1 if won else 0
            # worth a look: a whole frame's pots and no misses, a quick frame
            for i in (0, 1):
                sh = side(i).get("shot") or {}
                if int(sh.get("att") or 0) >= 20 and sh.get("att") == sh.get("made"):
                    self.notable.append({"time": t, "what": f"{names[i]} potted {sh.get('made')} of {sh.get('att')}",
                                         "game": game, "room": line.get("room")})
            if isinstance(fr["secs"], (int, float)) and 0 < fr["secs"] < 45 and not line.get("forfeit"):
                self.notable.append({"time": t, "what": f"a {int(fr['secs'])} s frame, {names[0]} v {names[1]}",
                                     "game": game, "room": line.get("room")})
            # one pair, one day, many frames, nearly all one way
            k = (day,) + tuple(sorted((ids[0], ids[1])))
            if k not in self.pairday and len(self.pairday) % 500 == 0:     # only the last 60 days are flagged
                cut = time.strftime("%Y-%m-%d", time.gmtime(time.time() - 60 * 86400))
                for old_k in [x for x in self.pairday if x[0] < cut]:
                    del self.pairday[old_k]
            p = self.pairday.setdefault(k, {"day": day, "names": {}, "n": 0, "w": {}})
            p["n"] += 1
            for i in (0, 1):
                p["names"][ids[i]] = names[i]
            if r.get("winner") in (0, 1):
                w = ids[r["winner"]]
                p["w"][w] = p["w"].get(w, 0) + 1
            # activity
            self.games_all[game] = self.games_all.get(game, 0) + 1
            gd = self.games_day.setdefault(day, {})
            gd[game] = gd.get(game, 0) + 1
            pd["frames"] += 1; pd["players"].update((ids[0], ids[1]))
            try:
                self.hours[int(t[11:13])] += 1
            except ValueError:
                pass
            if isinstance(fr["secs"], (int, float)) and fr["secs"] > 0:
                fs = self.frame_secs.setdefault(game, [collections.deque(maxlen=1000), 0])
                fs[0].append(fr["secs"]); fs[1] += 1
            # this month's race, by the relay's honours rules
            if verified and not any(" & " in x for x in names):
                mon = t[:7]
                for i in (0, 1):
                    if not ids[i].startswith("a:"):
                        continue
                    sd = side(i); sh = sd.get("shot") or {}
                    self.month_names.setdefault(mon, {})[ids[i]] = _clean(names[i], 23) or "?"
                    mp = self.monthp.setdefault(mon, {}).setdefault(ids[i], {"f": 0, "w": 0, "bnr": 0, "clr": 0, "hi": {}})
                    mp["f"] += 1; mp["w"] += 1 if r.get("winner") == i else 0
                    mp["bnr"] += int(sh.get("bnr") or 0); mp["clr"] += int(sh.get("runouts") or 0)
                    try:
                        bd = HON_KIND.get(int(r.get("kind", -1)))
                    except (TypeError, ValueError):
                        bd = None
                    best = int(sd.get("best") or 0)
                    if bd is not None and best > mp["hi"].get(bd, 0):
                        mp["hi"][bd] = best
        else:
            self.n_matches += 1
            pd["matches"] += 1
            fl = self.frames_pend.pop((line.get("gid"), line.get("room")), [])
            mid = hashlib.sha1(f"{t}|{line.get('room')}|{names}".encode()).hexdigest()[:10]
            mw = r.get("match_winner")
            m = {"id": mid, "time": t, "room": _clean(line.get("room"), 8), "game": _clean(r.get("game")),
                 "variant": _clean(r.get("variant")), "names": names, "ids": [ids[0], ids[1]],
                 "frames": r.get("frames"), "best_of": r.get("best_of"), "winner": mw, "verified": verified,
                 "forfeit": bool(line.get("forfeit")) or "left" in line, "left": _clean(line.get("left"), 30) if "left" in line else "",
                 "figures": line.get("figures", True), "repaired": _clean(line.get("repaired"), 400),
                 "room_secs": line.get("room_secs"), "doubles": any(" & " in x for x in names), "frame_list": fl,
                 "ver": [_clean((line.get(w) or {}).get("r", {}).get("ver") if isinstance(line.get(w), dict) else "", 12) for w in ("a", "b")]}
            self.matches.append(m)
            if isinstance(m["room_secs"], (int, float)):
                self.room_secs.append(m["room_secs"])
            for i in (0, 1):
                acc = self._acc(ids[i], names[i], t)
                won = mw == i
                acc["matches"] += 1; acc["mwon"] += 1 if won else 0
                acc["form"].append({"id": mid, "won": won, "time": t})
                o = acc["opp"].get(ids[1 - i])
                if o is None:
                    o = acc["opp"][ids[1 - i]] = [0, 0, 0, 0]
                o[2] += 1; o[3] += 1 if won else 0
            if "left" in line:
                for i in (0, 1):
                    if names[i] == line.get("left"):
                        self._acc(ids[i], names[i], t)["left"] += 1
                    elif mw == i:
                        self._acc(ids[i], names[i], t)["stayed_won"] += 1

    # ---- the model out ---------------------------------------------------------
    def _mtimes(self):
        k = []
        for n in FILES + ("reports.tsv",):
            try:
                st = os.stat(os.path.join(self.store, n))
                k.append((n, st.st_mtime, st.st_size))
            except OSError:
                k.append((n, 0, 0))
        k.append(time.strftime("%Y-%m-%d", time.gmtime()))     # the month and the days move on too
        return tuple(k)

    def _fresh(self):
        k = self._mtimes()
        if k != self._key or self._model is None:
            self._follow()
            self._model = self._out()
            self._slim = self._slimmed(self._model)
            self._key = k

    @staticmethod
    def _slimmed(m):
        """what the page loads every minute: each player without their whole
        head to head, each match without its frames -- those come with
        player() and match() when one is opened"""
        out = dict(m)
        out["players"] = [{**{k: v for k, v in p.items() if k != "opp"}, "nopp": len(p["opp"])} for p in m["players"]]
        slim_m = lambda x: {**{k: v for k, v in x.items() if k != "frame_list"}, "nframes": len(x["frame_list"])}
        out["matches"] = [slim_m(x) for x in m["matches"]]
        integ = dict(m["integrity"])
        for k in ("forfeits", "unverified"):
            integ[k] = [slim_m(x) for x in integ[k]]
        out["integrity"] = integ
        return out

    def get(self):
        self._fresh()
        m = dict(self._slim)
        m["honours"] = self.honours(m)
        return m

    def player(self, pid):
        self._fresh()
        p = next((x for x in self._model["players"] if x["id"] == pid), None)
        if p is None:
            return None
        ms = [x for x in self.matches if pid in x["ids"]]
        ms.sort(key=lambda x: x["time"], reverse=True)
        return {**p, "recent": [{k: v for k, v in x.items() if k != "frame_list"} for x in ms[:30]]}

    def match(self, mid):
        self._fresh()
        return next((x for x in self.matches if x["id"] == mid), None)

    def _out(self):
        P = lambda n: os.path.join(self.store, n)
        disputed = _jsonl(P("results_disputed.jsonl"))
        unconf = _jsonl(P("results_unconfirmed.jsonl"))
        acc = self.acc

        def diff(ra, rb, names):
            out = []
            for k in RES_SHARED:
                if ra.get(k) != rb.get(k):
                    out.append(f"{k}: {json.dumps(ra.get(k))} v {json.dumps(rb.get(k))}")
            sa, sb = ra.get("sides") or [{}, {}], rb.get("sides") or [{}, {}]
            for i in (0, 1):
                xa = sa[i] if i < len(sa) and isinstance(sa[i], dict) else {}
                xb = sb[i] if i < len(sb) and isinstance(sb[i], dict) else {}
                for k in RES_SIDE:
                    if k == "shot":
                        ha, hb = xa.get("shot") or {}, xb.get("shot") or {}
                        for s in sorted(set(ha) | set(hb)):
                            if ha.get(s, 0) != hb.get(s, 0):
                                out.append(f"{names[i] if i < len(names) else i} {s}: {ha.get(s, 0)} v {hb.get(s, 0)}")
                    elif xa.get(k) != xb.get(k):
                        out.append(f"{names[i] if i < len(names) else i} {k}: {json.dumps(xa.get(k))} v {json.dumps(xb.get(k))}")
            return out

        disputes_of = {}
        dlist = []
        for no, line in sorted(enumerate(disputed, 1), key=lambda nd: str(nd[1].get("time", "")), reverse=True):
            a, b = line.get("a") or {}, line.get("b") or {}
            ra, rb = a.get("r") or {}, b.get("r") or {}
            names = [_clean(x, 30) for x in (ra.get("names") or rb.get("names") or ["?", "?"])][:2]
            ids = _ids(line, names + ["?", "?"])
            for i in (0, 1):
                disputes_of[ids.get(i)] = disputes_of.get(ids.get(i), 0) + 1
            dlist.append({"time": line.get("time"), "room": _clean(line.get("room"), 8), "t": ra.get("t"),
                          "frame": ra.get("frame"), "game": _clean(ra.get("game")), "names": names,
                          "senders": [a.get("side"), b.get("side")], "verified": bool(line.get("verified")),
                          "diff": diff(ra, rb, names), "repaired": no in self.fixed,
                          "same_addr": bool(a.get("addr")) and a.get("addr") == b.get("addr")})
        ulist = []
        for line in sorted(unconf, key=lambda d: str(d.get("time", "")), reverse=True):
            a = line.get("a") or {}
            ra = a.get("r") or {}
            nm = [_clean(x, 30) for x in (ra.get("names") or ["?", "?"])][:2]
            ulist.append({"time": line.get("time"), "room": _clean(line.get("room"), 8), "t": ra.get("t"),
                          "frame": ra.get("frame"), "game": _clean(ra.get("game")), "names": nm,
                          "sender": nm[a["side"]] if a.get("side") in (0, 1) and len(nm) == 2 else "?",
                          "stale": bool(line.get("stale")), "strokes": sum(int((s or {}).get("shots") or 0) for s in (ra.get("sides") or []))})
        lopsided = []
        for p in self.pairday.values():
            top = max(p["w"].values()) if p["w"] else 0
            if p["n"] >= 10 and top >= 0.9 * p["n"]:
                lopsided.append({"day": p["day"], "names": list(p["names"].values()), "frames": p["n"], "top": top})
        lopsided.sort(key=lambda x: x["day"], reverse=True)
        matches = sorted(self.matches, key=lambda m: m["time"], reverse=True)
        reports = []
        try:
            with open(P("reports.tsv"), encoding="ascii", errors="replace") as f:
                for l in f:
                    c = l.rstrip("\n").split("\t")
                    reports.append({"time": c[0] if c else "", "fields": [_clean(x, 60) for x in c[1:] if not re.fullmatch(r"[0-9a-f]{16,}", x)]})
                    del reports[:-100]
        except OSError:
            pass

        # ---- players out ---------------------------------------------------
        def disp(a):
            return max(a["names"].items(), key=lambda kv: kv[1])[0] if a["names"] else "?"
        name_of = {i: disp(a) for i, a in acc.items()}
        players = []
        for i, a in acc.items():
            p = {k: v for k, v in a.items() if k not in ("names", "versions", "form", "opp", "games")}
            p["name"] = name_of[i]
            p["aka"] = [n for n, _ in sorted(a["names"].items(), key=lambda kv: kv[1], reverse=True) if n != p["name"]]
            p["version"] = max(a["versions"].items(), key=lambda kv: kv[1])[0] if a["versions"] else ""
            p["opp"] = sorted([{"id": o, "name": name_of.get(o, o[2:] if o.startswith("n:") else "?"),
                                "f": v[0], "fw": v[1], "m": v[2], "mw": v[3]}
                               for o, v in a["opp"].items()], key=lambda x: -(x["f"] + x["m"]))[:100]
            p["form"] = sorted(a["form"], key=lambda f: f["time"])[-10:]
            p["games"] = sorted([{"game": g, **v} for g, v in a["games"].items()], key=lambda x: -x["f"])
            p["shot"] = dict(a["shot"])
            p["disputes"] = disputes_of.get(i, 0)
            p["shown_id"] = "account " + i[2:8] if a["proved"] else "doubles team" if a["team"] else "name only"
            players.append(p)
        players.sort(key=lambda p: (p["team"], -p["frames"], p["name"]))

        # ---- activity ------------------------------------------------------
        today = time.time()
        days = [time.strftime("%Y-%m-%d", time.gmtime(today - k * 86400)) for k in range(29, -1, -1)]
        new_on = {}
        for d0 in self.seen_first.values():
            new_on[d0] = new_on.get(d0, 0) + 1
        perday = [{"day": d, "frames": self.perday.get(d, {}).get("frames", 0), "matches": self.perday.get(d, {}).get("matches", 0),
                   "players": len(self.perday.get(d, {}).get("players", ())), "new": new_on.get(d, 0)} for d in days]
        d7 = time.strftime("%Y-%m-%d", time.gmtime(today - 7 * 86400))
        games_7 = {}
        for d, gd in self.games_day.items():
            if d >= d7:
                for g, n in gd.items():
                    games_7[g] = games_7.get(g, 0) + n
        med = lambda v: sorted(v)[len(v) // 2] if v else 0
        vers = {}
        cut14 = time.strftime("%Y-%m-%d", time.gmtime(today - 14 * 86400))
        for p in players:
            if p["version"] and p["last"][:10] >= cut14:
                vers[p["version"]] = vers.get(p["version"], 0) + 1
        activity = {"days": perday, "hours_utc": list(self.hours),
                    "games": sorted(self.games_all.items(), key=lambda kv: -kv[1]),
                    "games7": sorted(games_7.items(), key=lambda kv: -kv[1]),
                    "frame_secs": sorted([(g, round(med(v[0])), v[1]) for g, v in self.frame_secs.items()], key=lambda x: -x[2]),
                    "room_secs_median": round(med(self.room_secs)), "versions": sorted(vers.items(), key=lambda kv: kv[0], reverse=True),
                    "totals": {"frames": self.n_frames, "matches": self.n_matches,
                               "players": sum(1 for p in players if p["frames"] and not p["team"]),
                               "accounts": sum(1 for p in players if p["proved"] and p["frames"]), "since": self.since}}

        return {"built": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC",
                "players": players, "matches": matches[:1000], "activity": activity,
                "integrity": {"disputed": dlist, "unconfirmed": ulist,
                              "forfeits": [m for m in matches if m["forfeit"]][:200],
                              "unverified": [m for m in matches if not m["verified"]][:200],
                              "same_addr": list(reversed(self.same_addr)), "lopsided": lopsided[:100],
                              "notable": list(reversed(self.notable))[:100], "reports": list(reversed(reports)),
                              "left_counts": sorted([(p["name"], p["left"]) for p in players if p["left"]], key=lambda x: -x[1])},
                "month_race": self._month_race(time.strftime("%Y-%m", time.gmtime()))}

    # ---- honours: the relay's own answer, and this month so far ------------
    def _month_race(self, month):
        P, name_of = self.monthp.get(month, {}), self.month_names.get(month, {})
        v = [{} for _ in range(10)]
        for uid, p in P.items():
            if p["w"]: v[0][uid] = p["w"]
            if p["f"] >= HON_MIN_FRAMES: v[1][uid] = round(100 * p["w"] / p["f"])
            if p["bnr"]: v[2][uid] = p["bnr"]
            if p["clr"]: v[3][uid] = p["clr"]
            for b, best in p["hi"].items():
                if best: v[b][uid] = best
        out = []
        for b, x in enumerate(v):
            rows_ = sorted(x.items(), key=lambda kv: (-kv[1], name_of.get(kv[0], "")))[:3]
            out.append({"board": b, "title": HON_TITLE[b],
                        "top": [{"name": name_of.get(u, "?"), "value": f"{val}{'%' if b == 1 else ''}"} for u, val in rows_]})
        near = sorted([(name_of.get(u, "?"), p["f"]) for u, p in P.items() if p["f"] < HON_MIN_FRAMES], key=lambda x: -x[1])[:5]
        return {"month": month, "boards": out, "win_rate_near": near}

    def honours(self, m):
        at, val = self._hon
        if val is not None and time.time() - at < 60:
            return val
        out = {"boards": [{"board": b, "title": HON_TITLE[b], "rows": []} for b in range(10)], "months": [], "error": ""}
        try:
            host, port = self.relay.rsplit(":", 1)
            s = socket.create_connection((host, int(port)), timeout=5)
            s.sendall(f"MOTE2 HONOURS {self.gid}\n".encode())
            s.settimeout(5)
            buf = b""
            try:
                while b"END\n" not in buf:
                    d = s.recv(16384)
                    if not d:
                        break
                    buf += d
            finally:
                s.close()
            for l in buf.decode(errors="ignore").splitlines():
                p = l.split(" ")
                if p[0] == "HON" and len(p) >= 5 and p[1].isdigit() and int(p[1]) < 10:
                    out["boards"][int(p[1])]["rows"].append({"rank": p[2], "value": p[3], "name": _clean(" ".join(p[4:]), 24)})
                elif p[0] == "MONTH" and len(p) >= 5 and p[1].isdigit() and int(p[1]) < 10:
                    out["months"].append({"board": int(p[1]), "title": HON_TITLE[int(p[1])], "month": p[2], "value": p[3],
                                          "name": _clean(" ".join(p[4:]), 24)})
        except OSError as e:
            out["error"] = type(e).__name__
        out["months"].sort(key=lambda x: (x["month"], x["board"]), reverse=True)
        self._hon = (time.time(), out)
        return out

    # ---- the server: its log and its health --------------------------------
    ADDR = re.compile(r"\s*\(\(['\"][0-9a-fA-F:.]+['\"],\s*\d+\)\)|\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b")
    LINE = re.compile(r"^(\S+) \S+ (\S+?)(?:\[\d+\])?: (?:\d{4}-\d\d-\d\d \d\d:\d\d:\d\d )?(.*)$")

    def _journal(self):
        if self.journal_file:
            try:
                with open(self.journal_file, encoding="utf-8", errors="replace") as f:
                    return f.read().splitlines()
            except OSError:
                return []
        try:
            p = subprocess.run(["journalctl", "-u", "mote-relay", "--since", "-7 days", "-o", "short-iso", "--no-pager", "-q"],
                               capture_output=True, text=True, timeout=20)
            return p.stdout.splitlines() if p.returncode == 0 else ["ERR " + (p.stderr or "journalctl failed").strip()[:200]]
        except (OSError, subprocess.SubprocessError) as e:
            return ["ERR " + type(e).__name__]

    def server(self):
        at, val = self._srv
        if val is not None and time.time() - at < 30:
            return val
        lines = self._journal()
        err = lines[0][4:] if lines and lines[0].startswith("ERR ") else ""
        days = {}
        recent, restarts, leaves, proofs = [], [], {}, {"good": 0, "bad": 0, "unchecked": 0}
        hon = {"good": 0, "bad": 0, "unchecked": 0}
        for l in lines:
            m = self.LINE.match(l)
            if not m:
                continue
            ts, src, msg = m.group(1), m.group(2), self.ADDR.sub("", m.group(3))
            day = ts[:10]
            d = days.setdefault(day, {"day": day, "hosted": 0, "clubs": 0, "joins": 0, "leaves": 0, "agreed": 0,
                                      "disputed": 0, "forfeits": 0, "proof_bad": 0, "errors": 0, "restarts": 0})
            kind = ""
            if "Started mote-relay.service" in msg:
                d["restarts"] += 1; restarts.append(ts); kind = "restart"
            elif "hosted," in msg or "quick-hosted" in msg:
                d["hosted"] += 1
            elif ": opened" in msg and msg.startswith("club"):
                d["clubs"] += 1
            elif re.search(r"member \d+ in", msg):
                d["joins"] += 1
            elif " left (" in msg and "member" in msg:
                d["leaves"] += 1
                why = re.search(r"left \(([^)]*)\)", msg)
                w = re.sub(r"\d+", "N", why.group(1)) if why else "?"
                leaves[w] = leaves.get(w, 0) + 1
            elif re.search(r"(frame|match) \d+ agreed", msg):
                d["agreed"] += 1
            elif "DISPUTED" in msg:
                d["disputed"] += 1; kind = "dispute"
            elif "forfeit" in msg or "the other having left" in msg or "left frame" in msg:
                d["forfeits"] += 1; kind = "forfeit"
            elif "proof good" in msg and not msg.startswith("honours"):
                proofs["good"] += 1
            elif "proof BAD" in msg and not msg.startswith("honours"):
                proofs["bad"] += 1; d["proof_bad"] += 1; kind = "proof"
            elif "proof could not be checked" in msg and not msg.startswith("honours"):
                proofs["unchecked"] += 1; kind = "proof"
            if msg.startswith("honours:"):
                kind = "honours"
                if "proof good" in msg: hon["good"] += 1; kind = ""
                elif "proof BAD" in msg: hon["bad"] += 1
                elif "could not" in msg: hon["unchecked"] += 1
            if re.search(r"Traceback|Exception|Error(?! \()|error:|failed", msg) and " left (" not in msg:
                d["errors"] += 1; kind = kind or "error"
            if "backlog" in msg:
                kind = kind or "backlog"
            if kind:
                recent.append({"time": ts[:19].replace("T", " "), "kind": kind, "msg": msg[:200]})
        sysinfo = {}
        try:
            sysinfo["uptime_s"] = float(open("/proc/uptime").read().split()[0])
            sysinfo["load"] = open("/proc/loadavg").read().split()[:3]
            mi = {l.split(":")[0]: int(l.split()[1]) for l in open("/proc/meminfo") if len(l.split()) >= 2}
            sysinfo["mem_total_mb"] = mi.get("MemTotal", 0) // 1024
            sysinfo["mem_avail_mb"] = mi.get("MemAvailable", 0) // 1024
        except (OSError, ValueError, IndexError):
            pass
        try:
            st = os.statvfs(self.store)
            sysinfo["disk_free_gb"] = round(st.f_bavail * st.f_frsize / 1e9, 1)
            sysinfo["disk_total_gb"] = round(st.f_blocks * st.f_frsize / 1e9, 1)
        except OSError:
            pass
        files, backups = [], []
        try:
            for n in sorted(os.listdir(self.store)):
                if n in ("meta.secret", "reports.salt"):
                    continue
                p = os.path.join(self.store, n)
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                if os.path.isdir(p):
                    try:
                        cnt = len(os.listdir(p))
                    except OSError:
                        cnt = -1
                    files.append({"name": n + "/", "size": cnt, "mtime": st.st_mtime, "dir": True})
                elif ".bak" in n:
                    backups.append({"name": n, "size": st.st_size, "mtime": st.st_mtime})
                else:
                    lines_n = None
                    if n.endswith(".jsonl") or n.endswith(".tsv"):
                        try:
                            with open(p, "rb") as f:
                                lines_n = sum(1 for _ in f)
                        except OSError:
                            pass
                    files.append({"name": n, "size": st.st_size, "mtime": st.st_mtime, "lines": lines_n})
        except OSError:
            pass
        snaps = []
        if self.backups:
            try:
                for n in sorted(os.listdir(self.backups), reverse=True):
                    if n.startswith("mote-relay-") and n.endswith(".tar.gz"):
                        st = os.stat(os.path.join(self.backups, n))
                        snaps.append({"name": n, "size": st.st_size, "mtime": st.st_mtime})
            except OSError:
                pass
        code = []
        here = os.path.dirname(os.path.abspath(__file__))
        for n in ("mote_relay.py", "mote_stats.py", "mote_stats_model.py", "mote_stats.html"):
            p = os.path.join(here, n)
            try:
                code.append({"name": n, "sha": hashlib.sha256(open(p, "rb").read()).hexdigest()[:12],
                             "mtime": os.stat(p).st_mtime})
            except OSError:
                pass
        val = {"at": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC", "journal_error": err,
               "days": sorted(days.values(), key=lambda d: d["day"], reverse=True), "leaves": sorted(leaves.items(), key=lambda kv: -kv[1]),
               "proofs": proofs, "honours_proofs": hon, "restarts": restarts[-20:][::-1],
               "relay_up_since": restarts[-1][:19].replace("T", " ") if restarts else "",
               "recent": recent[-80:][::-1], "sys": sysinfo, "files": files, "backups": backups, "snapshots": snaps, "code": code}
        self._srv = (time.time(), val)
        return val
