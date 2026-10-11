#!/usr/bin/env python3
"""test_results_db.py -- results.db (the index of results.jsonl) gives the same
honours, byte for byte, as reading the whole file; keeps up with appended
lines; waits for a half-written one; and rebuilds itself when the file is not
the one it indexed. Temp folders only, never the server's store.

    python3 relay/test_results_db.py [a results.jsonl to check as well]
"""
import argparse, json, os, shutil, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mote_relay  # noqa: E402

mote_relay.log = lambda *a, **k: None


def relay_on(store):
    r = mote_relay.Relay.__new__(mote_relay.Relay)
    r.args = argparse.Namespace(store=store)
    r.results_db = mote_relay.ResultsDB(store, r.HON_KIND)
    return r


def both(store):
    """the honours from the index, and from the file scan, for one store"""
    r = relay_on(store)
    month_now = time.strftime("%Y-%m", time.gmtime())
    assert r.results_db.sync(), "the index would not sync"
    db = r.honours_render(0, month_now, *r.results_db.honours_parts(month_now))
    scan = r.honours_render(0, month_now, *r.honours_scan(os.path.join(store, "results.jsonl"), month_now))
    return db, scan


def frame(t, a, b, win, kind, ua, ub, best=(0, 0), bnr=(0, 0), runs=(0, 0), verified=True):
    side = lambda i: {"best": best[i], "shot": {"bnr": bnr[i], "runouts": runs[i]}}
    return {"time": t, "verified": verified, "a": {"side": 0, "uid": ua}, "b": {"side": 1, "uid": ub},
            "result": {"t": "frame", "names": [a, b], "winner": win, "kind": kind, "sides": [side(0), side(1)]}}


def main():
    ok = bad = 0

    def check(cond, what):
        nonlocal ok, bad
        print(("  [ok ] " if cond else "  [BAD] ") + what)
        ok += bool(cond); bad += not cond

    store = tempfile.mkdtemp()
    path = os.path.join(store, "results.jsonl")
    rows = []
    for i in range(30):      # last month: a run of frames, a rename, a doubles frame, a forfeit's uids
        rows.append(frame("2026-09-%02d 20:00:00" % (1 + i % 28), "Ann" if i < 20 else "Annie", "Bob", i % 3 != 0,
                          [7, 4, 5, 6, 10, 23][i % 6], "101", "102", best=(i * 3, i), bnr=(i % 2, 0), runs=(0, i % 4)))
    rows.append(frame("2026-09-28 21:00:00", "Ann & Bob", "Cat & Dan", 0, 1, "101", "103"))
    rows.append(frame("2026-09-28 22:00:00", "Eve", "Bob", 0, 4, "105", "102", best=(155, 1), verified=False))
    f = frame("2026-09-29 21:00:00", "Cat", "Dan", 1, 23, None, None, best=(12, 40))
    f["a"], f["b"], f["uids"] = None, None, ["103", "104"]
    rows.append(f)
    rows.append(frame(time.strftime("%Y-%m-02 10:00:00", time.gmtime()), "Cat", "Dan", 0, 5, "103", "104", best=(98, 0)))
    with open(path, "w") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")
    db, scan = both(store)
    check(db == scan and db[0], f"the index's honours equal the file scan's ({db[0].count(chr(10))} lines)")
    check("MONTH" in db[0] and db[1], "monthly winners and each account's own wins come through")

    # appended lines, and one half written
    with open(path, "a") as fh:
        fh.write(json.dumps(frame("2026-09-30 23:00:00", "Dan", "Ann", 0, 6, "104", "101", best=(77, 0))) + "\n")
        fh.write('{"time": "2026-09-30 23:59:00", "verified": tr')
    db, scan = both(store)
    check(db == scan and "77 Dan" in db[0], "an appended line is taken in; the half-written one waits")
    with open(path, "a") as fh:
        fh.write('ue, "a": {"side": 0, "uid": "104"}, "b": {"side": 1, "uid": "101"}, "result": {"t": "frame", '
                 '"names": ["Dan", "Ann"], "winner": 0, "kind": 6, "sides": [{"best": 99}, {"best": 0}]}}\n')
    db, scan = both(store)
    check(db == scan and "99 Dan" in db[0], "once the line is whole it is taken in, once")

    # the file replaced by another: the index rebuilds rather than mixing two files
    with open(path, "w") as fh:
        fh.write(json.dumps(frame("2026-09-05 20:00:00", "Zed", "Yan", 0, 23, "201", "202", best=(64, 3))) + "\n")
    db, scan = both(store)
    check(db == scan and "Zed" in db[0] and "Ann" not in db[0], "a different file: the index is rebuilt from it")

    # the index deleted: built again
    os.remove(os.path.join(store, "results.db"))
    db, scan = both(store)
    check(db == scan and "Zed" in db[0], "the index deleted: built again from the file")
    shutil.rmtree(store)

    # A GOLDEN BREAK is not a break-and-run (2026-10-11): one stroke, the other side none
    store = tempfile.mkdtemp()
    g = frame("2026-09-10 20:00:00", "Gil", "Hal", 0, 2, "301", "302", bnr=(1, 0))
    g["result"]["sides"][0]["shots"], g["result"]["sides"][1]["shots"] = 1, 0
    run = frame("2026-09-11 20:00:00", "Ivy", "Hal", 0, 2, "303", "302", bnr=(1, 0))
    run["result"]["sides"][0]["shots"], run["result"]["sides"][1]["shots"] = 6, 0
    with open(os.path.join(store, "results.jsonl"), "w") as fh:
        fh.write(json.dumps(g) + "\n" + json.dumps(run) + "\n")
    db, scan = both(store)
    check(db == scan and "HON 2 1 1 Ivy" in db[0] and "Gil" not in "".join(l for l in db[0].splitlines() if l.startswith("HON 2")),
          "a golden break is left off the break-and-runs board; a real break-and-run is on it")
    shutil.rmtree(store)
    # ...and two games that differ only in a golden break's bnr agree on the frame
    r = mote_relay.Relay.__new__(mote_relay.Relay)
    old_end = {"t": "frame", "sides": [{"shots": 1, "shot": {"bnr": 1, "att": 1}}, {"shots": 0, "shot": {"bnr": 0}}]}
    new_end = {"t": "frame", "sides": [{"shots": 1, "shot": {"bnr": 0, "att": 1}}, {"shots": 0, "shot": {"bnr": 0}}]}
    check(r.result_key(old_end) == r.result_key(new_end), "a 7.4.1 and a 7.4.2 account of a golden break agree")
    other = {"t": "frame", "sides": [{"shots": 5, "shot": {"bnr": 1}}, {"shots": 0, "shot": {"bnr": 0}}]}
    other2 = {"t": "frame", "sides": [{"shots": 5, "shot": {"bnr": 0}}, {"shots": 0, "shot": {"bnr": 0}}]}
    check(r.result_key(other) != r.result_key(other2), "a real break-and-run's bnr is still compared")

    # 7.4.4 JUDGES STROKES DIFFERENTLY: two versions agree on a frame whose
    # attempts and safeties they counted differently; one version still must not
    a3 = {"t": "frame", "ver": "7.4.3", "sides": [{"shots": 9, "shot": {"att": 5, "made": 4, "safeties": 4, "safe_ok": 1, "bnr": 0, "random": 1}}, {"shots": 8, "shot": {"att": 6, "made": 3}}]}
    a4 = {"t": "frame", "ver": "7.4.4", "sides": [{"shots": 9, "shot": {"att": 7, "made": 4, "safeties": 2, "safe_ok": 1, "bnr": 0, "random": 0}}, {"shots": 8, "shot": {"att": 7, "made": 3}}]}
    check(r.result_key(a3, True) == r.result_key(a4, True), "a 7.4.3 and a 7.4.4 account that judged strokes differently agree")
    check(r.result_key(a3) != r.result_key(a4), "...but two games of one version that differ still do not")
    a4b = dict(a4); a4b["sides"] = [{"shots": 10, "shot": a4["sides"][0]["shot"]}, a4["sides"][1]]
    check(r.result_key(a3, True) != r.result_key(a4b, True), "...and a different stroke count is still a dispute between versions")
    check(max((a3, a4), key=r.ver_key) is a4 and max((a4, a3), key=r.ver_key) is a4, "the newer game's account is the one kept")

    # a real file, if given (a copy -- this test writes results.db beside it)
    if len(sys.argv) > 1:
        store = tempfile.mkdtemp()
        shutil.copy(sys.argv[1], os.path.join(store, "results.jsonl"))
        t = time.perf_counter(); db, scan = both(store); dt = time.perf_counter() - t
        check(db == scan, f"{sys.argv[1]}: the index's honours equal the file scan's ({db[0].count(chr(10))} lines, {dt:.2f} s)")
        shutil.rmtree(store)
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
