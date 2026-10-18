#!/usr/bin/env python3
"""
test_ranking.py -- the CueVR World Ranking in the tournament organiser (7.6.8):
sent by the stats app in parts, kept in ranking.json, served with the honours
as places only (never a score), the month's leader its winner when the month
turns, and that winner's own account told so (RMINE) -- nobody else's.

    python3 test_ranking.py
"""
import hashlib, os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mote_tourn

FAILS = []
def check(cond, what):
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    if not cond: FAILS.append(what)

store = tempfile.mkdtemp()
clock = [1792800000.0]                                   # 2026-10-23
o = mote_tourn.Organiser(store, now=lambda: clock[0])
h = lambda uid: hashlib.sha256(uid.encode()).hexdigest()[:12]
rows = [[h(str(1000 + i)), f"Player{i}"] for i in range(130)]
parts = [rows[i:i + 60] for i in range(0, len(rows), 60)]
for k, p in enumerate(parts):
    ans = o.admin("RANKING", {"part": k, "of": len(parts), "rows": p})
check(ans == "OK 130", f"three parts make one ranking of 130 ({ans})")
lines = o.champions_lines()
rk = [l for l in lines if l.startswith("RANK ")]
check(len(rk) == 130 and rk[0] == "RANK 1 Player0" and rk[129] == "RANK 130 Player129", "RANK lines in order, places and names")
check(all(len(l.split()) == 3 for l in rk), "...and nothing else on them: no score")
check(not [l for l in lines if l.startswith("RMONTH")], "no month won yet")
# a restart keeps it
o2 = mote_tourn.Organiser(store, now=lambda: clock[0])
check([l for l in o2.champions_lines() if l.startswith("RANK ")] == rk, "kept across a restart (ranking.json)")
# the month turns: the next push makes October's leader its winner
clock[0] = 1793500000.0                                  # 2026-11-01
o2.admin("RANKING", {"part": 0, "of": 1, "rows": [rows[1], rows[0]]})
mon = [l for l in o2.champions_lines() if l.startswith("RMONTH")]
check(mon == ["RMONTH 2026-10 Player0"], f"October won by the leader when November came in ({mon})")
check("RMINE 2026-10" in o2.wins_lines("1000"), "the winner's own account is told (RMINE)")
check(not [l for l in o2.wins_lines("1001") if l.startswith("RMINE")], "...and nobody else's")
check([l for l in o2.champions_lines() if l.startswith("RANK ")][:2] == ["RANK 1 Player1", "RANK 2 Player0"], "the new order served")
check(o2.admin("RANKING", {"part": 2, "of": 1, "rows": []}) == "BAD", "a nonsense part is refused")
print(f"\n{'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)")
sys.exit(1 if FAILS else 0)
