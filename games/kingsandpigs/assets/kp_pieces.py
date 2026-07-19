#!/usr/bin/env python3
"""Hand-authored room-piece library (starter set) for the Kings & Pigs floor
generator. Every piece passes assets/proto_check.check_piece: two-way
reachability (you can climb back up), art clearance, floor shape variety.

Ports on edges:  ^ top hole   v bottom hole   < left door   > right door
Interiors shaped with SOLID BRICK (# stairs/pillars/blocks) + planks (=-S) +
markers (d D h b B E C W F e x). Doors are 2 tall at floor level; every top
hole has a climb-ladder up to it.

Run `python3 assets/kp_pieces.py` to validate the whole library.
"""
PIECES = {
 "start":       {"kind": "start", "rows": [
    "#########","#.......#","#..--...#","#.d...d.#","#.e..##.#","####vv###"]},
 "exit":        {"kind": "exit", "rows": [
    "####^^####","#...--...#","#..d.....#","#...##...#","#.##.E.x.#","##########"]},
 "spine_climb": {"kind": "side", "rows": [
    "####^^####","#...--...#","#..d.....#","<....##..>","<.E..#..d>","####vv####"]},
 "cross_pit":   {"kind": "side", "rows": [
    "####^^####","#....-...#","#..==....#","<...d..E.>","<.#....#.>","#####vv###"]},
 "twin_step":   {"kind": "side", "rows": [
    "###^^#####","#..--....#","#.....d..#","<..#..##.>","<.dE.#..E>","####vv####"]},
 "ledge_hall":  {"kind": "side", "rows": [
    "####^^####","#...--...#","#.d......#","<...##..d>","<.E..#..E>","#####vv###"]},
 "pillar_pit":  {"kind": "side", "rows": [
    "####^^####","#...--...#","#......d.#","<..d.##..>","<.#E.#..E>","####vv####"]},
 "vault":       {"kind": "side", "rows": [
    "########","#..dd..#","#.S..S.#","<...h..#","<.B..#.#","########"]},
 "vault_hi":    {"kind": "side", "rows": [
    "#########","#.d...d.#","<..SS...#","<...h.##.","#..B..#d.","#########"]},
}

if __name__ == "__main__":
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import proto_check as C
    bad = 0
    for n, d in PIECES.items():
        e = C.check_piece(d["rows"])
        print(f"[{n}]", "OK" if not e else f"{len(e)}: " + "; ".join(e[:3]))
        bad += bool(e)
    print("ALL OK" if not bad else f"{bad} pieces bad")
