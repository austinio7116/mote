"""mote_stats_model.py -- everything mote_stats.py shows that is worked out from
the relay's files and its log: players by Meta account, matches with their
frames, the honours (and this month's race), the integrity queue, activity and
the server's health. Read only: nothing here ever writes to the relay's store.
"""
import collections, hashlib, json, math, os, re, socket, subprocess, time

# walk-outs before this (UTC) win nothing -- the same moment as mote_relay.py's
# WALKOUTS_COUNT_FROM, and changed with it
WALKOUTS_COUNT_FROM = "2026-10-18 06:06:21"

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


# ---- CUELO (2026-10-18; Mark: "a new improved ELO measure that works perfectly
# for this casual mixed cuesports game ... varying K depending on the confidence
# of the result ... avoid people manipulating by playing the same weak player
# over and over"). Glicko-style, a frame at a time, in the order played:
#   * each player a rating and an UNCERTAINTY (RD): a newcomer moves fast, a
#     regular slowly; time away (CUELO_C a day) makes one less certain again
#   * each GAME its own weight, fitted from the results: how surely the better
#     player wins a frame of it. A game of coin-tosses moves nobody much; a
#     frame of 15-red tells a lot. Fewer than CUELO_GAME_MIN frames: 1
#   * the same opponent again within CUELO_WEEK: the k-th frame counts 1/sqrt(k)
#   * an opponent with few different opponents of their own counts for less --
#     a new second account set up to lose is worth almost nothing
#   * both ends from one internet address: left out (one person, two accounts)
#   * ranked on the CAUTIOUS rating, r - 2 RD; provisional until CUELO_MIN_F
#     frames against CUELO_MIN_OPP different players
# TUNED ON THE RESULTS (787 rated frames to 2026-10-18), by how well each
# frame was predicted before it was played (mean log loss; a coin toss 0.6931):
# Elo K 30 0.6874 (K 12 at best, 0.6817); CUELO as below 0.6777. A start of 350,
# Glicko's own, swung newcomers too far on a game this close to a coin toss
# (0.6911); the week's repeat discount helps rather than costs (none: 0.6971)
CUELO_R0, CUELO_RD0, CUELO_RDMIN = 1500.0, 120.0, 70.0
CUELO_C = 10.0               # RD regained a day apart, squared-root law
CUELO_WEEK = 7 * 86400
CUELO_FREE = 0              # frames a week against one opponent at full weight before the discount
CUELO_GAME_MIN = 40
CUELO_MIN_F, CUELO_MIN_OPP = 20, 5
_Q = math.log(10.0) / 400.0


def _cuelo_t(t):
    try:
        return time.mktime(time.strptime(t[:19], "%Y-%m-%d %H:%M:%S"))
    except (ValueError, TypeError):
        return 0.0


def _cuelo_pass(ev, beta):
    """one pass over the frames; beta: kind -> weight. Returns the players and
    every frame's prediction (for the game fit and the scoring)"""
    P = {}                                    # id -> [r, rd, last_t, frames, set(opps)]
    recent = {}                               # (a, b) -> [times]
    preds = []
    for t, _, kind, wa, la, same in sorted(ev):
        if same or wa == la:
            continue
        tt = _cuelo_t(t)
        for i in (wa, la):
            p = P.setdefault(i, [CUELO_R0, CUELO_RD0, tt, 0, set()])
            if p[3]:                          # time away: less certain again
                days = max(0.0, (tt - p[2]) / 86400.0)
                p[1] = min(CUELO_RD0, math.sqrt(p[1] ** 2 + CUELO_C ** 2 * days))
        A, B = P[wa], P[la]
        b = beta.get(kind, 1.0)
        key = (min(wa, la), max(wa, la))
        hist = [x for x in recent.get(key, []) if tt - x < CUELO_WEEK]
        over = len(hist) + 1 - CUELO_FREE      # past the week's free frames against this player
        rep = 1.0 / math.sqrt(over) if over > 1 else 1.0
        hist.append(tt); recent[key] = hist
        g = lambda rd: 1.0 / math.sqrt(1.0 + 3.0 * _Q * _Q * rd * rd / (math.pi ** 2))
        ea = 1.0 / (1.0 + 10.0 ** (-g(B[1]) * b * (A[0] - B[0]) / 400.0))
        preds.append((kind, b * (A[0] - B[0]), A[1], B[1], ea))
        for me, op, s in ((A, B, 1.0), (B, A, 0.0)):
            nopp = len(op[4])
            w = b * rep * min(1.0, (nopp + 1) / float(CUELO_MIN_OPP))
            gg = g(op[1]) * b
            e = 1.0 / (1.0 + 10.0 ** (-gg * (me[0] - op[0]) / 400.0))
            d2 = 1.0 / (_Q * _Q * gg * gg * e * (1.0 - e) + 1e-12)
            inv = 1.0 / me[1] ** 2 + w / d2
            me.append((_Q / inv) * w * gg * (s - e))          # applied after both are worked out
            me.append(max(CUELO_RDMIN, math.sqrt(1.0 / inv)))
        for me, op in ((A, la), (B, wa)):
            dr, rd = me.pop(-2), me.pop(-1)
            me[0] += dr; me[1] = rd; me[2] = tt; me[3] += 1; me[4].add(op)
    return P, preds


def _cuelo_fit(preds):
    """each game's weight: the scale on the rating gap that best predicts its
    frames, pulled toward 1 by CUELO_GAME_MIN frames' worth of prior"""
    by = {}
    for kind, gap, _, _, _ in preds:
        by.setdefault(kind, []).append(gap)
    beta = {}
    for kind, gaps in by.items():
        if len(gaps) < 10:
            continue
        best, bl = 1.0, None
        for k in range(10, 201, 5):           # 0.10 .. 2.00
            bb = k / 100.0
            ll = sum(-math.log(1.0 / (1.0 + 10.0 ** (-bb * gp / 400.0)) + 1e-9) for gp in gaps)
            ll += CUELO_GAME_MIN * 0.5 * (bb - 1.0) ** 2 * 2.0     # the prior
            if bl is None or ll < bl:
                bl, best = ll, bb
        n = len(gaps)
        beta[kind] = (best * n + 1.0 * CUELO_GAME_MIN) / (n + CUELO_GAME_MIN) if n < 4 * CUELO_GAME_MIN else best
    return beta


def cuelo(ev, name_of, game_names):
    """the CUELO listing over the rated frames ev (time, seq, kind, winner, loser, same address)"""
    P1, pr1 = _cuelo_pass(ev, {})
    beta = _cuelo_fit(pr1)
    P, preds = _cuelo_pass(ev, beta)
    rows = []
    for i, p in P.items():
        cautious = p[0] - 2.0 * p[1]
        rows.append({"id": i, "name": name_of(i), "r": round(cautious), "rating": round(p[0]), "rd": round(p[1]),
                     "f": p[3], "opp": len(p[4]), "prov": p[3] < CUELO_MIN_F or len(p[4]) < CUELO_MIN_OPP})
    rows.sort(key=lambda x: (x["prov"], -x["r"]))
    # how well each rating predicted the frames it had not yet seen: the mean
    # log loss, lower is better (a coin toss is 0.693)
    ll = sum(-math.log(max(1e-9, x[4])) for x in preds) / max(1, len(preds))
    games = sorted(({"kind": k, "game": game_names.get(k, "?"), "weight": round(b, 2)} for k, b in beta.items()),
                   key=lambda g: -g["weight"])
    return {"rows": rows, "games": games, "frames": len(preds), "logloss": round(ll, 4),
            "skipped_same": sum(1 for e in ev if e[5]),
            "min_f": CUELO_MIN_F, "min_opp": CUELO_MIN_OPP, "week_days": CUELO_WEEK // 86400}


# ---- CLAUDEELO (2026-10-18; Mark: "look at everyone's results including their
# gameplay stats and make your own independent ranking ... make this method a
# new 3rd option - call it ClaudeELO" ... "just make ClaudeELO as good as it can
# be"). CUELO's rating plus a bonus for how you actually play:
#   ClaudeELO = CUELO + CL_BONUS x play
# play: the mean of three figures, each a z against everyone else who played
# the same FAMILY of game (8-ball, 9/10-ball, snooker, straight pool,
# billiards -- a pot rate means something different in each), shrunk toward
# average on a small sample and weighted by frames:
#     pot success, points a visit, fouls a shot (fewer is better)
# THE THREE THAT PREDICT. Tested frame by frame on the results (each frame
# predicted from everything before it): the better potter won 59% of frames,
# the bigger scorer a visit 59%, the cleaner player 56%; long pots and breaks
# or runs told nothing (50%), and a better safety rate went with LOSING (45%) --
# whoever needs the most safeties is the one being made to play them. Fitted
# on the first 60% of frames and scored on the last 40%: CUELO 0.6610 log
# loss, + 40 x play 0.6556, picking the winner 64% to 63%; more than ~60
# points a step and it gets worse again. On a thin early history the bonus
# cost a little (0.6900 -> 0.6941): it earns its keep as the figures fill up.
# On CUELO's scale, so a gap means what a CUELO gap means. 15 rated frames.
CL_MIN_FRAMES = 15
CL_BONUS = 40.0
_CL_MET = (("pot", "pot success", lambda T: T["made"] / T["att"] if T["att"] else None, lambda T: T["att"], 1, 60),
           ("fouls", "few fouls", lambda T: T["fouls"] / T["shots"] if T["shots"] else None, lambda T: T["shots"], -1, 80),
           ("visit", "points a visit", lambda T: T["vis_pts"] / T["visits"] if T["visits"] else None, lambda T: T["visits"], 1, 40))
# ...and every figure, for the trained model (ml_rank) and the strengths shown
_CL_ALL = _CL_MET + (
           ("long", "long pots", lambda T: T["long_made"] / T["long_att"] if T["long_att"] else None, lambda T: T["long_att"], 1, 25),
           ("safety", "safeties", lambda T: T["safe_ok"] / T["safeties"] if T["safeties"] else None, lambda T: T["safeties"], 1, 25),
           ("big", "breaks and runs", lambda T: (T["b30"] + T["bnr"] + T["runouts"]) / T["frames"] if T["frames"] else None, lambda T: T["frames"], 1, 10))


def _cl_family(g):
    g = (g or "").upper()
    if "SNOOKER" in g or "900" in g: return "snooker"
    if "9-BALL" in g or "10-BALL" in g: return "9/10-ball"
    if "8-BALL" in g or "ULTIMATE" in g: return "8-ball"
    if "STRAIGHT" in g: return "straight pool"
    if "BILLIARDS" in g and "BAR" not in g: return "billiards"
    return None


def cl_metric_z(play, mets=None):
    """each player's z for each figure, frame-weighted over the families they
    played: account -> key -> z (and the families' z's for the strengths)"""
    acc, detail = {}, {}
    for fam in sorted({f for (_, f) in play}):
        P = {i: T for (i, f), T in play.items() if f == fam and T["frames"] >= 3}
        for key, label, val, nfn, sign, n0 in (mets or _CL_ALL):
            vals = {i: (val(T), nfn(T)) for i, T in P.items() if val(T) is not None and nfn(T) > 0}
            if len(vals) < 4:
                continue
            tot = sum(n for _, n in vals.values())
            mean = sum(v * n for v, n in vals.values()) / tot
            sd = math.sqrt(sum(n * (v - mean) ** 2 for v, n in vals.values()) / tot) or 1e-9
            for i, (v, n) in vals.items():
                z = sign * (v - mean) / sd * (n / (n + n0))
                a = acc.setdefault(i, {}).setdefault(key, [0.0, 0.0]); a[0] += z * P[i]["frames"]; a[1] += P[i]["frames"]
                detail.setdefault(i, []).append((z, f"{fam} {label}"))
    return {i: {k: a[0] / a[1] for k, a in d.items()} for i, d in acc.items()}, detail


def claude_elo(play, cuelo_rows, name_of):
    mz, detail = cl_metric_z(play)
    out = []
    for c in cuelo_rows:
        if c["f"] < CL_MIN_FRAMES:
            continue
        z = mz.get(c["id"], {})
        pz = sum(z.get(k[0], 0.0) for k in _CL_MET) / len(_CL_MET)
        d = sorted(x for x in detail.get(c["id"], []) if any(x[1].endswith(m[1]) for m in _CL_MET))
        out.append({"id": c["id"], "name": name_of(c["id"]), "r": round(c["rating"] + CL_BONUS * pz),
                    "cuelo": c["rating"], "bonus": round(CL_BONUS * pz), "play": round(pz, 2), "f": c["f"], "opp": c["opp"],
                    "best": d[-1][1] if d else "", "worst": d[0][1] if d else ""})
    out.sort(key=lambda x: -x["r"])
    return {"rows": out, "min_f": CL_MIN_FRAMES, "bonus": CL_BONUS}


class Model:
    """the stats, kept as running totals: each line results.jsonl gains is
    folded in once, and only the lines added since the last look are read
    (2026-10-11 -- re-reading and holding every line would have outgrown the
    service's 200 MB at about 14,000 lines). A file that is not the one being
    followed (shorter, or its first line changed) starts the totals again."""
    MATCHES_KEPT = 2000           # the newest matches, with their frames, for the Matches page

    def __init__(self, store, relay, gid, journal_file="", backups="", forfeits_from=None):
        self.store, self.relay, self.gid, self.journal_file, self.backups = store, relay, gid, journal_file, backups
        # see _fold: a walk-out before this wins nothing (the relay's own cut-off unless given)
        self.forfeits_from = forfeits_from if forfeits_from is not None else WALKOUTS_COUNT_FROM
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
        self.pot_all, self.pot_month, self.pot_name = {}, {}, {}
        self.fixed = set()
        self.seen_lines = {}
        self.superseded = set()
        self.n_frames = self.n_matches = 0
        self.since = ""
        # RATINGS (Mark, 2026-10-12): Elo, everyone from 1500, K 30, a rating
        # over every game ("all") and one a game (by its kind), moved by each
        # verified singles frame between two proved accounts, in the order the
        # frames were played -- the whole history on the first read. key ->
        # account -> [rating, frames, peak, last change]
        self.elo = {}
        self.elo_game = {}
        # ...IN THE ORDER THE FRAMES WERE PLAYED (2026-10-18; Mark: "check the
        # elo calculations account for all the various cleanups"): a repair
        # appends its frames at the end of the file, so rating them as read
        # put a frame played on the 15th after the 18th's -- 4 places, up to
        # 32 points on one account. Each rated frame is kept here (time, then
        # file order) and the ratings are played through in that order when
        # they are asked for (_elo_replay).
        self.elo_ev = []
        self.elo_dirty = False
        self.cuelo_ev = []
        self.cl_play = {}            # ClaudeELO: (account, family) -> the frame figures, summed

    # ---- following results.jsonl --------------------------------------------
    def _follow(self):
        path = os.path.join(self.store, "results.jsonl")
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as f:
                head = hashlib.sha256(f.readline()).hexdigest()
                if self.head is not None and (head != self.head or size < self.off):
                    self._reset()
                if self.off == 0:          # a full read: which lines a repaired copy stands in for
                    for raw in f:
                        if b'"repaired"' in raw:
                            try:
                                self.superseded.add(self._dupkey(json.loads(raw)))
                            except ValueError:
                                pass
                    f.seek(0); f.readline()
                self.head = head
                f.seek(self.off)
                started = self.off
                while self.off < size:
                    raw = f.readline(size - self.off)
                    if not raw.endswith(b"\n"):          # a line still being written waits
                        break
                    # A REPAIR WHILE RUNNING IS A WHOLE NEW READ: its copy has
                    # the original's key, so read on from here it was passed
                    # over as a duplicate and the original kept until a restart
                    # -- and the read is made NOW: left to the next change of
                    # the file, the page was built from the emptied state and
                    # showed no one (2026-10-18, a repair at 20:38)
                    if started > 0 and b'"repaired"' in raw:
                        self._reset()
                        return self._follow()
                    self.off += len(raw)
                    try:
                        line = json.loads(raw)
                    except ValueError:
                        continue
                    if isinstance(line, dict):
                        self._fold(line)
        except OSError:
            pass

    @staticmethod
    def _dupkey(line):
        r = line.get("result") if isinstance(line.get("result"), dict) else {}
        names = ([_clean(x, 30) for x in (r.get("names") or [])] + ["?", "?"])[:2]
        return json.dumps([r.get("t"), line.get("gid"), line.get("room"), str(line.get("time", "")), r.get("frame"),
                           r.get("frames"), r.get("score"), r.get("winner"), r.get("match_winner"), names])

    def _acc(self, i, name, t):
        a = self.acc.get(i)
        if a is None:
            a = self.acc[i] = {"id": i, "proved": i.startswith("a:"), "team": i.startswith("t:"), "names": {},
                               "frames": 0, "fwon": 0, "matches": 0, "mwon": 0, "games": {}, "best": {},
                               "shot": {k: 0 for k in SHOT_SUM}, "pot_shots": 0, "potted": 0, "fouls": 0, "shots": 0,
                               "table_secs": 0.0, "first": t, "last": t, "versions": {}, "left": 0, "stayed_won": 0,
                               "form": collections.deque(maxlen=20), "opp": {}, "unverified": 0,
                               "carom_strokes": 0, "carom_cannons": 0}
        a["names"][name] = max(a["names"].get(name, ""), t)
        a["first"] = min(a["first"], t); a["last"] = max(a["last"], t)
        return a

    ELO_START, ELO_K = 1500.0, 30.0

    def _elo(self, key, wa, la):
        """one frame into the ratings under key: wa beat la"""
        R = self.elo.setdefault(key, {})
        a = R.setdefault(wa, [self.ELO_START, 0, self.ELO_START, 0.0])
        b = R.setdefault(la, [self.ELO_START, 0, self.ELO_START, 0.0])
        ea = 1.0 / (1.0 + 10.0 ** ((b[0] - a[0]) / 400.0))
        d = self.ELO_K * (1.0 - ea)
        a[0] += d; b[0] -= d
        a[1] += 1; b[1] += 1
        a[2] = max(a[2], a[0])
        a[3], b[3] = d, -d

    def _fold(self, line):
        """one line of results.jsonl into every total"""
        for g in re.finditer(r"results_disputed\.jsonl lines? (\d+)[ab]?(?: \+ (\d+)[ab]?)?", str(line.get("repaired", ""))):
            self.fixed.update(int(n) for n in g.groups() if n)
        r = line.get("result") if isinstance(line.get("result"), dict) else {}
        t = str(line.get("time", ""))
        if r.get("t") not in ("frame", "match"):
            return
        # A WALK-OUT BEFORE THE FIX WINS NOTHING (2026-10-18; Mark: "just
        # remove wins for dropping from the history and resume only once these
        # fixes are out"; "Noone is trying to cheat yet so we can assume all
        # drops were bugs"; "we want walk outs to count from the fix forward").
        # Players were thrown out of rooms by the relay's backlog cap, and the
        # frames were given to the one who stayed -- or, with the sides
        # reported the wrong way round, to the one who went. Such a frame stays
        # in the list, with no winner: no wins, form, head-to-head or rating.
        voided = False
        if t < self.forfeits_from and (line.get("forfeit") or r.get("forfeit") or "left" in line):
            r = dict(r, winner=None, match_winner=None)
            line = dict(line, result=r)
            # ...AND NO LOSS EITHER (2026-10-19; Mark: "Why are my online results
            # head to head Vs orms different to his vs mine?"). Counted as played
            # with nobody winning, it was a frame lost on BOTH pages -- lolly11luce
            # 3-13 against Orms16 on one, Orms16 11-5 on the other, the two
            # walk-outs of 10-17 the difference. Not played, then, by either.
            voided = True
        names = ([_clean(x, 30) for x in (r.get("names") or [])] + ["?", "?"])[:2]
        # ONE FRAME, ONCE: a doubles frame is reported by both pairs of seats and
        # kept twice, the same frame, result and time -- counted the once
        dup = self._dupkey(line)
        if dup in self.seen_lines:
            return
        # ...AND A REPAIRED COPY IN PLACE OF ITS ORIGINAL: a repair appends a
        # corrected copy (verified, the missing account added) further down
        # the file, so the original is passed over (7.4.5)
        if dup in self.superseded and not line.get("repaired"):
            return
        self.seen_lines[dup] = None
        while len(self.seen_lines) > 2000:
            self.seen_lines.pop(next(iter(self.seen_lines)))
        ids = _ids(line, names)
        # CLAUDEELO'S FIGURES (claude_elo): each proved player's own shot figures,
        # by the family of game, from verified frames that were played out
        if (r.get("t") == "frame" and line.get("verified") and not line.get("forfeit") and "left" not in line
                and r.get("winner") in (0, 1)):
            f = _cl_family(r.get("game"))
            sides_ = r.get("sides") if isinstance(r.get("sides"), list) else []
            for i in (0, 1) if f else ():
                if not ids[i].startswith("a:") or i >= len(sides_) or not isinstance(sides_[i], dict):
                    continue
                sd = sides_[i]; sh = sd.get("shot") if isinstance(sd.get("shot"), dict) else {}
                T = self.cl_play.setdefault((ids[i], f), collections.defaultdict(float))
                T["frames"] += 1
                T["fouls"] += sd.get("fouls") or 0; T["shots"] += sd.get("shots") or 0
                for k_ in ("att", "made", "long_att", "long_made", "safeties", "safe_ok", "visits", "vis_pts", "b30", "bnr", "runouts"):
                    try: T[k_] += float(sh.get(k_) or 0)
                    except (TypeError, ValueError): pass
        # THE RATINGS: a verified singles frame, both sides proved accounts,
        # a winner -- a forfeit's included, the one who stayed the winner
        if (r.get("t") == "frame" and line.get("verified") and r.get("winner") in (0, 1)
                and ids[0][:2] == "a:" and ids[1][:2] == "a:" and ids[0] != ids[1]):
            w = r["winner"]
            try:
                kind = int(r.get("kind", -1))
            except (TypeError, ValueError):
                kind = -1
            # snooker's sizes all report "SNOOKER": named by their kind (CueGameKind)
            self.elo_game[kind] = {4: "15-RED SNOOKER", 5: "10-RED SNOOKER", 6: "6-RED SNOOKER", 23: "3-RED SNOOKER"}.get(
                kind, _clean(r.get("game") or f"game {kind}", 30))
            self.elo_ev.append((t, len(self.elo_ev), kind, ids[w], ids[1 - w]))
            self.elo_dirty = True
            # ...and CUELO's own record of it: the same frame, and whether both
            # ends came from one internet address (cuelo())
            a_, b_ = line.get("a"), line.get("b")
            same = bool(isinstance(a_, dict) and isinstance(b_, dict) and a_.get("addr") and a_.get("addr") == b_.get("addr"))
            self.cuelo_ev.append((t, len(self.cuelo_ev), kind, ids[w], ids[1 - w], same))
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
                if voided:
                    self._acc(ids[i], names[i], t)
                    continue
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
                if fr["kind"] in (18, 19, 20, 21, 22):           # carom: every point is a cannon
                    sh0 = sd.get("shot") or {}
                    acc["carom_strokes"] += int(sh0.get("strokes") or 0); acc["carom_cannons"] += int(sh0.get("scoring") or 0)
                golden = sd.get("shots") == 1 and side(1 - i).get("shots") == 0
                for k, v in (sd.get("shot") or {}).items():
                    if k in acc["shot"] and not (golden and k == "bnr"):     # a golden break is not a break-and-run
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
                    sd = side(i); sh = dict(sd.get("shot") or {})
                    if sd.get("shots") == 1 and side(1 - i).get("shots") == 0:
                        sh.pop("bnr", None)
                    # ...AND THE REST OF THE RELAY'S RULES (mote_relay.py
                    # honours_shot -- keep the two in step): at 8-ball a run is
                    # eight of the runner's own, at 9- and 10-ball every object
                    # ball down. This month's race had only the golden-break
                    # rule, so it still showed the runs the boards had dropped
                    # (Mark, 2026-10-14: "my monthly honours lists in the admin
                    # app showing the clearances and break and runs that we have
                    # now filtered out")
                    try:
                        kind = int(r.get("kind", -1))
                    except (TypeError, ValueError):
                        kind = -1
                    own = {0: 8, 1: 8, 3: 8}.get(kind)
                    if own and int(sd.get("potted") or 0) < own:
                        sh.pop("bnr", None); sh.pop("runouts", None)
                    need = {2: 9, 13: 10}.get(kind)
                    if need and int(sd.get("potted") or 0) + int(side(1 - i).get("potted") or 0) < need:
                        sh.pop("bnr", None); sh.pop("runouts", None)
                    # pot success, for Mark's eyes only (the stats app's Honours tab; not a club board)
                    att, made = int(sh.get("att") or 0), int(sh.get("made") or 0)
                    for tally in (self.pot_all, self.pot_month.setdefault(mon, {})):
                        pt = tally.setdefault(ids[i], [0, 0])
                        pt[0] += att; pt[1] += made
                    self.pot_name[ids[i]] = _clean(names[i], 23) or "?"
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
                if voided:                                   # see above: not played by either
                    self._acc(ids[i], names[i], t)
                    continue
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

    def _elo_replay(self):
        """every rated frame through the ratings again, in the order played"""
        if not self.elo_dirty:
            return
        self.elo = {}
        for t, _, kind, wa, la in sorted(self.elo_ev):
            for key in ("all", kind):
                self._elo(key, wa, la)
        self.elo_dirty = False

    def _cuelo(self):
        """CUELO, the second listing: cuelo() over the same frames"""
        def name_of(i):
            a = self.acc.get(i)
            return max(a["names"].items(), key=lambda kv: kv[1])[0] if a and a["names"] else "?"
        return cuelo(self.cuelo_ev, name_of, self.elo_game)

    def _claude(self):
        """ClaudeELO, the third listing: claude_elo() over CUELO and the play figures"""
        def name_of(i):
            a = self.acc.get(i)
            return max(a["names"].items(), key=lambda kv: kv[1])[0] if a and a["names"] else "?"
        return claude_elo(self.cl_play, self._cuelo()["rows"], name_of)

    def _ratings(self):
        """the ratings, highest first: overall and a table a game"""
        self._elo_replay()
        def name_of(i):
            a = self.acc.get(i)
            return max(a["names"].items(), key=lambda kv: kv[1])[0] if a and a["names"] else "?"
        def rows(R):
            return [{"id": i, "name": name_of(i), "r": round(v[0]), "f": v[1], "peak": round(v[2]), "last": round(v[3], 1)}
                    for i, v in sorted(R.items(), key=lambda kv: -kv[1][0])]
        games = sorted(((k, self.elo_game.get(k, "?"), len(R)) for k, R in self.elo.items() if k != "all"),
                       key=lambda g: (-g[2], g[1]))
        return {"start": self.ELO_START, "k": self.ELO_K, "all": rows(self.elo.get("all", {})),
                "games": [{"kind": k, "game": g, "players": n, "rows": rows(self.elo[k])} for k, g, n in games]}

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

        # REPEAT WALK-OUTS (Mark, 10-12: forfeits are kept, to stop anyone
        # walking out of a losing frame to keep it off the record -- "if we
        # see a lot of these by the same players it is worth flagging"): over
        # the last 30 days, who keeps leaving frames under way, who keeps
        # winning that way, and the same pair again and again (a friend
        # walking out to hand someone wins). Three or more, or a pair twice.
        cut30 = time.strftime("%Y-%m-%d", time.gmtime(time.time() - 30 * 86400))
        lv, wn, pr = {}, {}, {}
        for m in matches:
            if not m["forfeit"] or not m["left"] or str(m["time"])[:10] < cut30:
                continue
            li = next((i for i in (0, 1) if m["names"][i] == m["left"]), None)
            if li is None:
                continue
            wi = 1 - li
            a = lv.setdefault(m["ids"][li], {"id": m["ids"][li], "name": m["names"][li], "n": 0, "bare": 0, "last": ""})
            a["n"] += 1; a["bare"] += 0 if m["figures"] else 1; a["last"] = max(a["last"], str(m["time"]))
            b = wn.setdefault(m["ids"][wi], {"id": m["ids"][wi], "name": m["names"][wi], "n": 0, "last": ""})
            b["n"] += 1; b["last"] = max(b["last"], str(m["time"]))
            c = pr.setdefault((m["ids"][li], m["ids"][wi]), {"left": m["names"][li], "won": m["names"][wi], "n": 0, "last": ""})
            c["n"] += 1; c["last"] = max(c["last"], str(m["time"]))
        walkout_flags = {"leavers": sorted([x for x in lv.values() if x["n"] >= 3], key=lambda x: -x["n"]),
                         "winners": sorted([x for x in wn.values() if x["n"] >= 3], key=lambda x: -x["n"]),
                         "pairs": sorted([x for x in pr.values() if x["n"] >= 2], key=lambda x: -x["n"])}

        return {"built": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC",
                "players": players, "matches": matches[:1000], "activity": activity,
                "integrity": {"disputed": dlist, "unconfirmed": ulist,
                              "forfeits": [m for m in matches if m["forfeit"]][:200],
                              "unverified": [m for m in matches if not m["verified"]][:200],
                              "same_addr": list(reversed(self.same_addr)), "lopsided": lopsided[:100],
                              "notable": list(reversed(self.notable))[:100], "reports": list(reversed(reports)),
                              "left_counts": sorted([(p["name"], p["left"]) for p in players if p["left"]], key=lambda x: -x[1]),
                              "walkout_flags": walkout_flags},
                "month_race": self._month_race(time.strftime("%Y-%m", time.gmtime())),
                "pot_board": self._pot_board(time.strftime("%Y-%m", time.gmtime())),
                "ratings": self._ratings(), "cuelo": self._cuelo(), "claude": self._claude()}

    POT_MIN_ALL, POT_MIN_MONTH = 100, 50       # pot attempts before a player is on the pot success board

    def _pot_board(self, month):
        """pot success by account, verified singles frames: all time and this
        month, each from a minimum of attempts, and who is still short of it"""
        def board(T, need):
            on = [(self.pot_name.get(u, "?"), m / a, a, m) for u, (a, m) in T.items() if a >= need]
            on.sort(key=lambda x: (-x[1], -x[2], x[0]))
            short = sorted([(self.pot_name.get(u, "?"), a) for u, (a, m) in T.items() if 0 < a < need], key=lambda x: -x[1])
            return {"need": need, "rows": [{"name": n, "pct": round(100 * r, 1), "att": a, "made": m} for n, r, a, m in on[:20]],
                    "short": short[:10]}
        return {"all": board(self.pot_all, self.POT_MIN_ALL), "month": board(self.pot_month.get(month, {}), self.POT_MIN_MONTH)}

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
