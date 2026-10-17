#!/usr/bin/env python3
"""repair_disputes.py -- disputed frames whose RESULT both games agree on, put
into results.jsonl as repaired lines (Mark, 2026-10-17: "there are 64 new
disputed games in the server logs - we need to sort them out!!!", and yes to:
each player's figures from their own game's report; a score that differs taken
from the winner's game). results_disputed.jsonl is never touched: a repaired
line names the disputed line it came from, and the stats app counts it in place
of the dispute. Lines already repaired are skipped, so it can be run again.

    repair_disputes.py <store> [--write]

Without --write it only says what it would add. With it, it APPENDS to
<store>/results.jsonl -- back that file up first.
"""
import json, re, sys, time

SHARED = ("t", "game", "variant", "kind", "mode", "best_of", "frame",
          "score", "frames", "winner", "match_winner", "bnr", "golden")
SIDE = ("shots", "pot_shots", "potted", "fouls", "best", "breaks", "shot")
OUTCOME = ("t", "frame", "frames", "winner", "match_winner", "best_of")


def main():
    store = sys.argv[1]
    write = "--write" in sys.argv[2:]
    disputed = [json.loads(l) for l in open(f"{store}/results_disputed.jsonl") if l.strip()]
    done = set()
    for l in open(f"{store}/results.jsonl"):
        for g in re.finditer(r"results_disputed\.jsonl lines? (\d+)[ab]?(?: \+ (\d+)[ab]?)?", l):
            done.update(int(n) for n in g.groups() if n)
    today = time.strftime("%Y-%m-%d", time.gmtime())
    out, skipped = [], []
    for no, d in enumerate(disputed, 1):
        if no in done:
            continue
        a, b = d["a"]["r"], d["b"]["r"]
        if any(a.get(k) != b.get(k) for k in OUTCOME) or len(a.get("sides", [])) != 2 or len(b.get("sides", [])) != 2:
            skipped.append((no, "the games disagree on the result itself"))
            continue
        # whose report is whose side: "me" is the reporter's own side
        by_side = {}
        for rep in (a, b):
            if rep.get("me") in (0, 1):
                by_side[rep["me"]] = rep
        if len(by_side) != 2:
            skipped.append((no, "cannot tell whose report is whose"))
            continue
        w = a.get("winner") if a.get("winner") in (0, 1) else a.get("match_winner")
        base = by_side.get(w, a) if w in (0, 1) else a        # the winner's game for anything shared that differs
        result = {k: base.get(k) for k in SHARED}
        result["names"] = base.get("names")
        result["sides"] = [{k: by_side[i]["sides"][i].get(k) for k in SIDE} for i in range(2)]
        why = "the result agreed, stroke counts differed"
        if a.get("score") != b.get("score"):
            why = f"the result agreed, the score differed ({a.get('score')} v {b.get('score')}: the winner's game's kept)"
        line = {k: d[k] for k in ("time", "gid", "room", "room_secs", "players", "a", "b", "verified") if k in d}
        line["agreed"] = True
        line["result"] = result
        line["repaired"] = (f"{today} from results_disputed.jsonl line {no}: {why}; "
                            "each player's figures from their own game")
        out.append((no, line))
    for no, line in out:
        r = line["result"]
        print(f"line {no:3d}  {line['time']}  {r.get('game')}  {r.get('t')}  {r.get('names')}  score {r.get('score')}  "
              f"frames {r.get('frames')}  best {[s.get('best') for s in r['sides']]}")
    for no, why in skipped:
        print(f"line {no:3d}  SKIPPED: {why}")
    print(f"{len(out)} to add, {len(skipped)} skipped, {len(done)} already repaired")
    if write and out:
        with open(f"{store}/results.jsonl", "a", encoding="utf-8") as f:
            for _, line in out:
                f.write(json.dumps(line, separators=(", ", ": "), sort_keys=True) + "\n")
        print(f"appended {len(out)} lines to {store}/results.jsonl")


if __name__ == "__main__":
    main()
