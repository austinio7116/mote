"""mote_stats_model.py -- everything mote_stats.py shows that is worked out from
the relay's files and its log: players by Meta account, matches with their
frames, the honours (and this month's race), the integrity queue, activity and
the server's health. Read only: nothing here ever writes to the relay's store.
"""
import hashlib, json, os, re, socket, subprocess, time

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
    def __init__(self, store, relay, gid, journal_file="", backups=""):
        self.store, self.relay, self.gid, self.journal_file, self.backups = store, relay, gid, journal_file, backups
        self._key = None
        self._model = None
        self._hon = (0.0, None)
        self._srv = (0.0, None)

    # ---- the files, reloaded only when one changes -------------------------
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

    def get(self):
        k = self._mtimes()
        if k != self._key or self._model is None:
            self._model = self._build()
            self._key = k
        m = dict(self._model)
        m["honours"] = self.honours(m)
        return m

    # ---- the model ---------------------------------------------------------
    def _build(self):
        P = lambda n: os.path.join(self.store, n)
        rows = sorted(_jsonl(P("results.jsonl")), key=lambda d: str(d.get("time", "")))
        disputed = _jsonl(P("results_disputed.jsonl"))
        unconf = _jsonl(P("results_unconfirmed.jsonl"))
        acc, matches, frames_pend, notable = {}, [], {}, []
        month_now = time.strftime("%Y-%m", time.gmtime())

        def A(i, name, t):
            a = acc.get(i)
            if a is None:
                a = acc[i] = {"id": i, "proved": i.startswith("a:"), "team": i.startswith("t:"), "names": {}, "frames": 0, "fwon": 0,
                              "matches": 0, "mwon": 0, "games": {}, "best": {}, "shot": {k: 0 for k in SHOT_SUM},
                              "pot_shots": 0, "potted": 0, "fouls": 0, "shots": 0, "table_secs": 0.0,
                              "first": t, "last": t, "versions": {}, "left": 0, "stayed_won": 0, "disputes": 0,
                              "form": [], "opp": {}, "unverified": 0, "b50": 0, "b100": 0}
            a["names"][name] = max(a["names"].get(name, ""), t)
            a["first"] = min(a["first"], t); a["last"] = max(a["last"], t)
            return a

        for n, line in enumerate(rows):
            r = line.get("result") or {}
            t = str(line.get("time", ""))
            if r.get("t") not in ("frame", "match"):
                continue
            names = [_clean(x, 30) for x in (r.get("names") or ["?", "?"])][:2] + ["?", "?"]
            names = names[:2]
            ids = _ids(line, names)
            sides = r.get("sides") or [{}, {}]
            rep = _report(line, 0)
            verified = bool(line.get("verified"))
            doubles = any(" & " in x for x in names)
            for who in ("a", "b"):           # each sender's version, against their own account
                x = line.get(who)
                if isinstance(x, dict) and isinstance(x.get("r"), dict) and x.get("side") in (0, 1):
                    a = A(ids[x["side"]], names[x["side"]], t)
                    v = _clean(x["r"].get("ver"), 12)
                    if v:
                        a["versions"][v] = max(a["versions"].get(v, ""), t)
            if r["t"] == "frame":
                fr = {"time": t, "frame": r.get("frame"), "game": _clean(r.get("game")), "variant": _clean(r.get("variant")),
                      "kind": r.get("kind"), "score": r.get("score"), "winner": r.get("winner"),
                      "secs": rep.get("secs"), "bnr": r.get("bnr"), "golden": r.get("golden"),
                      "forfeit": line.get("forfeit"), "figures": line.get("figures", True),
                      "sides": [{k: (sides[i] or {}).get(k) for k in RES_SIDE} for i in (0, 1)],
                      "time_at": [((rep.get("sides") or [{}, {}])[i] or {}).get("time") for i in (0, 1)]}
                frames_pend.setdefault((line.get("gid"), line.get("room")), []).append(fr)
                for i in (0, 1):
                    a = A(ids[i], names[i], t)
                    sd = sides[i] if i < len(sides) and isinstance(sides[i], dict) else {}
                    won = r.get("winner") == i
                    a["frames"] += 1; a["fwon"] += 1 if won else 0
                    a["unverified"] += 0 if verified else 1
                    g = a["games"].setdefault(fr["game"] or "?", {"f": 0, "w": 0, "best": 0})
                    g["f"] += 1; g["w"] += 1 if won else 0
                    b = int(sd.get("best") or 0)
                    g["best"] = max(g["best"], b)
                    if b > a["best"].get(fr["game"], 0):
                        a["best"][fr["game"]] = b
                    for k in ("pot_shots", "potted", "fouls", "shots"):
                        a[k] += int(sd.get(k) or 0)
                    for k, v in (sd.get("shot") or {}).items():
                        if k in a["shot"]:
                            a["shot"][k] += int(v or 0)
                    ta = fr["time_at"][i]
                    if isinstance(ta, (int, float)):
                        a["table_secs"] += ta
                    o = a["opp"].setdefault(ids[1 - i], {"f": 0, "fw": 0, "m": 0, "mw": 0})
                    o["f"] += 1; o["fw"] += 1 if won else 0
                # worth a look: a whole frame's pots and no misses, a quick frame
                for i in (0, 1):
                    sh = (sides[i] or {}).get("shot") or {}
                    if int(sh.get("att") or 0) >= 20 and sh.get("att") == sh.get("made"):
                        notable.append({"time": t, "what": f"{names[i]} potted {sh.get('made')} of {sh.get('att')}",
                                        "game": fr["game"], "room": line.get("room")})
                if isinstance(fr["secs"], (int, float)) and 0 < fr["secs"] < 45 and not line.get("forfeit"):
                    notable.append({"time": t, "what": f"a {int(fr['secs'])} s frame, {names[0]} v {names[1]}",
                                    "game": fr["game"], "room": line.get("room")})
            else:
                key = (line.get("gid"), line.get("room"))
                fl = frames_pend.pop(key, [])
                mid = hashlib.sha1(f"{t}|{line.get('room')}|{names}".encode()).hexdigest()[:10]
                mw = r.get("match_winner")
                m = {"id": mid, "time": t, "room": _clean(line.get("room"), 8), "game": _clean(r.get("game")),
                     "variant": _clean(r.get("variant")), "names": names, "ids": [ids[0], ids[1]],
                     "frames": r.get("frames"), "best_of": r.get("best_of"), "winner": mw, "verified": verified,
                     "forfeit": bool(line.get("forfeit")) or "left" in line, "left": _clean(line.get("left"), 30) if "left" in line else "",
                     "figures": line.get("figures", True), "repaired": _clean(line.get("repaired"), 400),
                     "room_secs": line.get("room_secs"), "doubles": doubles, "frame_list": fl,
                     "ver": [_clean((line.get(w) or {}).get("r", {}).get("ver") if isinstance(line.get(w), dict) else "", 12) for w in ("a", "b")]}
                matches.append(m)
                for i in (0, 1):
                    a = A(ids[i], names[i], t)
                    won = mw == i
                    a["matches"] += 1; a["mwon"] += 1 if won else 0
                    a["form"].append({"id": mid, "won": won, "time": t})
                    o = a["opp"].setdefault(ids[1 - i], {"f": 0, "fw": 0, "m": 0, "mw": 0})
                    o["m"] += 1; o["mw"] += 1 if won else 0
                if "left" in line:
                    for i in (0, 1):
                        if names[i] == line.get("left"):
                            A(ids[i], names[i], t)["left"] += 1
                        elif mw == i:
                            A(ids[i], names[i], t)["stayed_won"] += 1

        # ---- integrity -----------------------------------------------------
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

        # the disputed lines a repair took up ("results_disputed.jsonl lines 1b + 3a", "line 9")
        fixed = set()
        for x in rows:
            for g in re.finditer(r"results_disputed\.jsonl lines? (\d+)[ab]?(?: \+ (\d+)[ab]?)?", str(x.get("repaired", ""))):
                fixed.update(int(n) for n in g.groups() if n)
        dlist = []
        for no, line in sorted(enumerate(disputed, 1), key=lambda nd: str(nd[1].get("time", "")), reverse=True):
            a, b = line.get("a") or {}, line.get("b") or {}
            ra, rb = a.get("r") or {}, b.get("r") or {}
            names = [_clean(x, 30) for x in (ra.get("names") or rb.get("names") or ["?", "?"])][:2]
            ids = _ids(line, names + ["?", "?"])
            for i in (0, 1):
                if ids.get(i) in acc:
                    acc[ids[i]]["disputes"] += 1
            dlist.append({"time": line.get("time"), "room": _clean(line.get("room"), 8), "t": ra.get("t"),
                          "frame": ra.get("frame"), "game": _clean(ra.get("game")), "names": names,
                          "senders": [a.get("side"), b.get("side")], "verified": bool(line.get("verified")),
                          "diff": diff(ra, rb, names), "repaired": no in fixed, "same_addr": bool(a.get("addr")) and a.get("addr") == b.get("addr")})
        ulist = []
        for line in sorted(unconf, key=lambda d: str(d.get("time", "")), reverse=True):
            a = line.get("a") or {}
            ra = a.get("r") or {}
            nm = [_clean(x, 30) for x in (ra.get("names") or ["?", "?"])][:2]
            ulist.append({"time": line.get("time"), "room": _clean(line.get("room"), 8), "t": ra.get("t"),
                          "frame": ra.get("frame"), "game": _clean(ra.get("game")), "names": nm,
                          "sender": nm[a["side"]] if a.get("side") in (0, 1) and len(nm) == 2 else "?",
                          "stale": bool(line.get("stale")), "strokes": sum(int((s or {}).get("shots") or 0) for s in (ra.get("sides") or []))})
        same_addr = []
        for line in rows:
            a, b = line.get("a"), line.get("b")
            if isinstance(a, dict) and isinstance(b, dict) and a.get("addr") and a.get("addr") == b.get("addr"):
                r = line.get("result") or {}
                same_addr.append({"time": line.get("time"), "names": [_clean(x, 30) for x in (r.get("names") or [])],
                                  "game": _clean(r.get("game")), "room": _clean(line.get("room"), 8), "t": r.get("t")})
        # one pair, one day, many frames, nearly all one way
        pairday = {}
        for line in rows:
            r = line.get("result") or {}
            if r.get("t") != "frame":
                continue
            nm = [_clean(x, 30) for x in (r.get("names") or ["?", "?"])][:2]
            ids = _ids(line, nm + ["?", "?"])
            k = (str(line.get("time", ""))[:10],) + tuple(sorted((ids[0], ids[1])))
            p = pairday.setdefault(k, {"day": k[0], "names": {}, "n": 0, "w": {}})
            p["n"] += 1
            for i in (0, 1):
                p["names"][ids[i]] = nm[i]
            if r.get("winner") in (0, 1):
                w = ids[r["winner"]]
                p["w"][w] = p["w"].get(w, 0) + 1
        lopsided = []
        for p in pairday.values():
            top = max(p["w"].values()) if p["w"] else 0
            if p["n"] >= 10 and top >= 0.9 * p["n"]:
                lopsided.append({"day": p["day"], "names": list(p["names"].values()), "frames": p["n"], "top": top})
        forfeits = [m for m in reversed(matches) if m["forfeit"]]
        unverified = [m for m in reversed(matches) if not m["verified"]]
        reports = []
        try:
            with open(P("reports.tsv"), encoding="ascii", errors="replace") as f:
                for l in f:
                    c = l.rstrip("\n").split("\t")
                    reports.append({"time": c[0] if c else "", "fields": [_clean(x, 60) for x in c[1:] if not re.fullmatch(r"[0-9a-f]{16,}", x)]})
        except OSError:
            pass

        # ---- players out ---------------------------------------------------
        def disp(a):
            return max(a["names"].items(), key=lambda kv: kv[1])[0] if a["names"] else "?"
        name_of = {i: disp(a) for i, a in acc.items()}
        players = []
        for i, a in acc.items():
            p = dict(a)
            p["name"] = name_of[i]
            p["aka"] = [n for n, _ in sorted(a["names"].items(), key=lambda kv: kv[1], reverse=True) if n != p["name"]]
            p["version"] = max(a["versions"].items(), key=lambda kv: kv[1])[0] if a["versions"] else ""
            p["opp"] = sorted([{"id": o, "name": name_of.get(o, o[2:] if o.startswith("n:") else "?"), **v}
                               for o, v in a["opp"].items()], key=lambda x: -(x["f"] + x["m"]))
            p["form"] = a["form"][-10:]
            p["games"] = sorted([{"game": g, **v} for g, v in a["games"].items()], key=lambda x: -x["f"])
            p["shown_id"] = "account " + i[2:8] if a["proved"] else "doubles team" if a["team"] else "name only"
            del p["names"], p["versions"]
            players.append(p)
        players.sort(key=lambda p: (p["team"], -p["frames"], p["name"]))

        # ---- activity ------------------------------------------------------
        today = time.time()
        days = [time.strftime("%Y-%m-%d", time.gmtime(today - k * 86400)) for k in range(29, -1, -1)]
        perday = {d: {"day": d, "frames": 0, "matches": 0, "players": set(), "new": 0} for d in days}
        hours = [0] * 24
        games_all, games_7 = {}, {}
        frame_secs = {}
        seen_first = {}
        d7 = time.strftime("%Y-%m-%d", time.gmtime(today - 7 * 86400))
        for line in rows:
            r = line.get("result") or {}
            t = str(line.get("time", ""))
            if r.get("t") not in ("frame", "match"):
                continue
            nm = [_clean(x, 30) for x in (r.get("names") or ["?", "?"])][:2] + ["?", "?"]
            ids = _ids(line, nm)
            d = perday.get(t[:10])
            for i in (0, 1):
                seen_first.setdefault(ids[i], t[:10])
            if r["t"] == "match":
                if d:
                    d["matches"] += 1
                continue
            g = _clean(r.get("game")) or "?"
            games_all[g] = games_all.get(g, 0) + 1
            if t[:10] >= d7:
                games_7[g] = games_7.get(g, 0) + 1
            if d:
                d["frames"] += 1; d["players"].update((ids[0], ids[1]))
            try:
                hours[int(t[11:13])] += 1
            except ValueError:
                pass
            s = _report(line, 0).get("secs")
            if isinstance(s, (int, float)) and s > 0:
                frame_secs.setdefault(g, []).append(s)
        for i, d0 in seen_first.items():
            if d0 in perday:
                perday[d0]["new"] += 1
        med = lambda v: sorted(v)[len(v) // 2] if v else 0
        room_secs = [m["room_secs"] for m in matches if isinstance(m["room_secs"], (int, float))]
        vers = {}
        cut14 = time.strftime("%Y-%m-%d", time.gmtime(today - 14 * 86400))
        for p in players:
            if p["version"] and p["last"][:10] >= cut14:
                vers[p["version"]] = vers.get(p["version"], 0) + 1
        activity = {"days": [{**d, "players": len(d["players"])} for d in perday.values()],
                    "hours_utc": hours,
                    "games": sorted(games_all.items(), key=lambda kv: -kv[1]),
                    "games7": sorted(games_7.items(), key=lambda kv: -kv[1]),
                    "frame_secs": sorted([(g, round(med(v)), len(v)) for g, v in frame_secs.items()], key=lambda x: -x[2]),
                    "room_secs_median": round(med(room_secs)), "versions": sorted(vers.items(), key=lambda kv: kv[0], reverse=True),
                    "totals": {"frames": sum(1 for x in rows if (x.get("result") or {}).get("t") == "frame"),
                               "matches": len(matches), "players": sum(1 for p in players if p["frames"] and not p["team"]),
                               "accounts": sum(1 for p in players if p["proved"] and p["frames"]),
                               "since": rows[0]["time"] if rows else ""}}

        return {"built": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC",
                "players": players, "matches": list(reversed(matches))[:1000], "activity": activity,
                "integrity": {"disputed": dlist, "unconfirmed": ulist, "forfeits": forfeits[:200],
                              "unverified": unverified[:200], "same_addr": same_addr, "lopsided": lopsided,
                              "notable": list(reversed(notable))[:100], "reports": list(reversed(reports))[:100],
                              "left_counts": sorted([(p["name"], p["left"]) for p in players if p["left"]], key=lambda x: -x[1])},
                "month_race": self._month_race(rows, month_now)}

    # ---- honours: the relay's own answer, and this month so far ------------
    def _month_race(self, rows, month):
        P, name_of = {}, {}
        for d in rows:
            r = d.get("result") or {}
            if not d.get("verified") or r.get("t") != "frame" or str(d.get("time", ""))[:7] != month:
                continue
            names, sides = r.get("names") or [], r.get("sides") or []
            if any(" & " in str(n) for n in names):
                continue
            ids = _ids(d, [_clean(x, 30) for x in names] + ["?", "?"])
            for side in (0, 1):
                uid = ids.get(side)
                if not uid or not uid.startswith("a:") or side >= len(names) or side >= len(sides):
                    continue
                name_of[uid] = _clean(names[side], 23) or "?"
                sd = sides[side] or {}
                sh = sd.get("shot") or {}
                p = P.setdefault(uid, {"f": 0, "w": 0, "bnr": 0, "clr": 0, "hi": {}})
                p["f"] += 1; p["w"] += 1 if r.get("winner") == side else 0
                p["bnr"] += int(sh.get("bnr") or 0); p["clr"] += int(sh.get("runouts") or 0)
                b = HON_KIND.get(int(r.get("kind", -1)))
                best = int(sd.get("best") or 0)
                if b is not None and best > p["hi"].get(b, 0):
                    p["hi"][b] = best
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
