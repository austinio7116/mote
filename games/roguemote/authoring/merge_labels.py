#!/usr/bin/env python3
"""Merge the per-batch agent identifications (batches/out_*.json) into one
labels_ai.json keyed by "c,r". On overlap (props/trinkets, temple/jungle) keep
the higher-confidence entry. Validate categories against the annotator taxonomy."""
import json, glob, os
HERE = os.path.dirname(os.path.abspath(__file__))
B = os.path.join(HERE, "batches")
VALID = {"player","npc","enemy","boss","animal","weapon","armor","consumable","treasure",
         "key","tool","container","furniture","prop","door","floor","wall","feature",
         "ui","fx","font","unknown"}
CONF = {"high":3,"med":2,"medium":2,"low":1,"":0}

def main():
    merged = {}
    fixed = 0
    for f in sorted(glob.glob(os.path.join(B,"out_*.json"))):
        for e in json.load(open(f)):
            c,r = int(e["c"]), int(e["r"])
            cat = (e.get("category") or "unknown").strip().lower()
            if cat not in VALID: cat = "unknown"; fixed += 1
            conf = (e.get("confidence") or "").strip().lower()
            rec = {"cat":cat, "name":(e.get("name") or "").strip(), "conf":conf,
                   "src":os.path.basename(f)[4:-5]}
            k = f"{c},{r}"
            if k in merged and CONF.get(merged[k]["conf"],0) >= CONF.get(conf,0):
                continue                    # keep existing (>= confidence)
            merged[k] = rec
    json.dump(merged, open(os.path.join(HERE,"labels_ai.json"),"w"), indent=0)
    # stats
    from collections import Counter
    bycat = Counter(v["cat"] for v in merged.values())
    byconf = Counter(v["conf"] or "none" for v in merged.values())
    print(f"merged {len(merged)} unique tiles ({fixed} category coercions)")
    print("by category:", dict(sorted(bycat.items(), key=lambda x:-x[1])))
    print("by confidence:", dict(byconf))

if __name__ == "__main__":
    main()
