#!/usr/bin/env python3
"""test_merged.py -- one player on two Meta accounts counted as one (2026-10-20,
UKIRAMPAGE / UKIIIRAMPAGE): <store>/accounts_merged.json read by the relay's
honours index (re-read when it changes, the index rebuilt) and by the stats
app's player ids. results.jsonl is never written to."""
import json, os, shutil, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mote_relay
import mote_stats_model

ok = bad = 0


def check(c, what):
    global ok, bad
    print(("  ok    " if c else "  [BAD] ") + what)
    if c: ok += 1
    else: bad += 1


OLD, NEW, OTHER = "111", "222", "333"


def frame(t, names, uids, winner):
    side = {"shots": 5, "pot_shots": 3, "potted": 3, "fouls": 0, "best": 3, "breaks": [], "shot": {}}
    return {"time": t, "gid": 1, "room": "TEST", "verified": True, "agreed": True,
            "a": {"uid": uids[0], "side": 0}, "b": {"uid": uids[1], "side": 1},
            "result": {"t": "frame", "kind": 0, "game": "UK 8-BALL", "names": names, "winner": winner,
                       "frames": [1, 0], "best_of": 1, "frame": 1, "sides": [dict(side), dict(side)]}}


d = tempfile.mkdtemp()
try:
    lines = [frame("2026-10-14 20:00:0%d" % i, ["RAMPAGE", "OPP"], [OLD, OTHER], 0) for i in range(3)]
    lines += [frame("2026-10-19 20:00:0%d" % i, ["RAMPAGE3", "OPP"], [NEW, OTHER], 0) for i in range(2)]
    with open(os.path.join(d, "results.jsonl"), "w") as f:
        f.writelines(json.dumps(l) + "\n" for l in lines)
    before = open(os.path.join(d, "results.jsonl"), "rb").read()
    db = mote_relay.ResultsDB(d, mote_relay.Relay.HON_KIND)
    month = time.strftime("%Y-%m", time.gmtime())
    check(db.sync(), "the index builds")
    allp, _, name_of = db.honours_parts(month)
    check(allp.get(OLD, {}).get("w") == 3 and allp.get(NEW, {}).get("w") == 2, "no merge: two accounts, 3 and 2 frames won")

    with open(os.path.join(d, "accounts_merged.json"), "w") as f:
        json.dump({OLD: NEW}, f)
    os.utime(os.path.join(d, "accounts_merged.json"), (time.time() + 5, time.time() + 5))
    check(db.sync(), "merged: the index rebuilds")
    allp, _, name_of = db.honours_parts(month)
    check(OLD not in allp and allp.get(NEW, {}).get("w") == 5 and allp[NEW].get("f") == 5,
          "merged: one account, 5 frames, 5 won (%s)" % allp.get(NEW))
    check(name_of.get(NEW) == "RAMPAGE3", "merged: shown under the latest name (%s)" % name_of.get(NEW))
    check(allp.get(OTHER, {}).get("f") == 5, "the opponent is untouched: 5 frames")

    m = mote_relay.Merged(d).load()
    check(m.canon(OLD) == NEW and m.canon(NEW) == NEW and m.canon(OTHER) == OTHER, "canon: old -> new, the rest as they are")

    # the file scan (when the index cannot be used) counts the same
    class A: store = d; forfeits_from = mote_relay.WALKOUTS_COUNT_FROM
    r = mote_relay.Relay.__new__(mote_relay.Relay)
    r.args, r.merged = A, mote_relay.Merged(d).load()
    allp2, _, _ = r.honours_scan(os.path.join(d, "results.jsonl"), month)
    check(OLD not in allp2 and allp2.get(NEW, {}).get("f") == 5, "the file scan merges the same")

    # the stats app's ids
    mote_stats_model.merged_load(d)
    a = mote_stats_model._ids(lines[0], ["RAMPAGE", "OPP"])
    b = mote_stats_model._ids(lines[3], ["RAMPAGE3", "OPP"])
    check(a[0] == b[0] and a[0].startswith("a:"), "stats app: the old and the new account are one player")
    check(a[1] == b[1], "stats app: the opponent is one player as before")

    # taking it out undoes it
    os.remove(os.path.join(d, "accounts_merged.json"))
    check(db.sync(), "unmerged: the index rebuilds")
    allp, _, _ = db.honours_parts(month)
    check(allp.get(OLD, {}).get("w") == 3 and allp.get(NEW, {}).get("w") == 2, "unmerged: two accounts again")
    check(open(os.path.join(d, "results.jsonl"), "rb").read() == before, "results.jsonl never written to")
finally:
    shutil.rmtree(d)
print(f"{ok} ok, {bad} bad")
sys.exit(1 if bad else 0)
