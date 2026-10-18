import sys, json, math, os, tempfile, time as _t
sys.path.insert(0, '/home/maustin/thumby-color/mote/relay')
import mote_stats_model as M
SP = sys.argv[1]
M.CL_MIN_FRAMES = 1
lines = []
for raw in open(SP + "/results.jsonl"):
    try: d = json.loads(raw)
    except ValueError: continue
    if isinstance(d, dict) and "time" in d: lines.append(d)
lines.sort(key=lambda d: str(d.get("time", "")))
tmp = tempfile.mkdtemp(); open(os.path.join(tmp, "results.jsonl"), "w").write("{}\n")
m = M.Model(tmp, "127.0.0.1:1", 0); m._follow()
# per-metric play z, by player, recomputed before each frame
def metric_z():
    return M.cl_metric_z(m.cl_play)[0]
def _old_metric_z():
    out = {}
    play = m.cl_play
    for fam in sorted({f for (_, f) in play}):
        P = {i: T for (i, f), T in play.items() if f == fam and T["frames"] >= 3}
        for key, label, val, nfn, sign, n0 in M._CL_MET:
            vals = {i: (val(T), nfn(T)) for i, T in P.items() if val(T) is not None and nfn(T) > 0}
            if len(vals) < 4: continue
            tot = sum(n for _, n in vals.values()); mean = sum(v * n for v, n in vals.values()) / tot
            sd = math.sqrt(sum(n * (v - mean) ** 2 for v, n in vals.values()) / tot) or 1e-9
            for i, (v, n) in vals.items():
                z = sign * (v - mean) / sd * (n / (n + n0)); w = P[i]["frames"]
                a = out.setdefault(i, {}).setdefault(key, [0.0, 0.0]); a[0] += z * w; a[1] += w
    return {i: {k: a[0] / a[1] for k, a in d.items()} for i, d in out.items()}
rows = []
t0 = _t.time()
for d in lines:
    n0 = len(m.cuelo_ev)
    # the state BEFORE this line
    pre = None
    r = d.get("result") if isinstance(d.get("result"), dict) else {}
    if r.get("t") == "frame":
        cu = {x["id"]: x for x in m._cuelo()["rows"]}
        mz = metric_z()
        pre = (cu, mz)
    m._fold(d)
    if len(m.cuelo_ev) > n0 and pre:
        e = m.cuelo_ev[-1]
        if e[5]: continue
        wa, la = e[3], e[4]; cu, mz = pre
        ca, cb = cu.get(wa), cu.get(la)
        f = {"t": e[0], "dc": (ca["rating"] if ca else 1500) - (cb["rating"] if cb else 1500),
             "rda": ca["rd"] if ca else 120, "rdb": cb["rd"] if cb else 120,
             "d_exp": math.log1p(ca["f"] if ca else 0) - math.log1p(cb["f"] if cb else 0),
             "d_opp": math.log1p(ca["opp"] if ca else 0) - math.log1p(cb["opp"] if cb else 0)}
        for key, *_ in M._CL_ALL:
            f["d_" + key] = mz.get(wa, {}).get(key, 0.0) - mz.get(la, {}).get(key, 0.0)
        rows.append(f)
json.dump(rows, open(SP + "/feat.json", "w"))
print(len(rows), "frames with features in %.0f s" % (_t.time() - t0))
