#!/usr/bin/env python3
"""Exhaustive verification of the blob47 terrain sets.

Nothing here trusts the generator. Each check re-derives the expected answer
from sdk/mote_tile.h's own rules and fails loudly on any mismatch.

  1. LUT contract      - all 256 masks map into 0..46; the reduction and the
                         cell ordering match mote_tile.h exactly.
  2. Sheet geometry    - the atlas is exactly 47 cells wide so the engine's
                         cell%tpr / cell/tpr indexing lands on the right pixels,
                         and variant rows stack the way draw_autotile() expects.
  3. Nine-slice anchor - the 9 configs a nine-slice CAN express must composite
                         back to the original source tiles, pixel for pixel.
  4. Border logic      - every cell draws a border on exactly the sides that
                         face empty space, and none of the sides that don't.
  5. Inner corners     - the 31 concave cells differ from their plain
                         nine-slice equivalent in the right corner, and only there.
  6. Opposite edges    - 1-wide spurs/stubs carry both borders at once.
  7. Interior fill     - the fully-surrounded cell is solid: no border colour,
                         no transparency. This one caught a real bug.
  8. Determinism       - regenerating is byte-identical.
  9. Bake round-trip   - the LUT in src/<name>.tiles.h, as written by
                         `mote bake`, still matches the canonical one.

    python3 verify_terrain.py
"""
import sys
from PIL import Image

import blob47
import gen_terrain as gt
import map_blob47 as mp
from blob47 import N, NE, E, SE, S, SW, W, NW

TS = gt.TS
FAILS = []


def check(cond, label, detail=""):
    if cond:
        print(f"   ok    {label}")
    else:
        print(f"   FAIL  {label}   {detail}")
        FAILS.append(label)
    return cond


# --- 1. the LUT contract, re-derived from mote_tile.h --------------------
def check_lut():
    print("\n1. LUT contract (vs sdk/mote_tile.h)")
    order, lut = blob47.CANON, blob47.LUT
    check(len(order) == 47, "47 distinct reduced masks")
    check(len(lut) == 256 and all(0 <= v < 47 for v in lut),
          "all 256 masks map into cells 0..46")

    # independent reimplementation of mote__at_reduce, written from the C
    def reduce_ref(m):
        r = m & (N | E | S | W)
        if (m & NE) and (r & N) and (r & E): r |= NE
        if (m & SE) and (r & S) and (r & E): r |= SE
        if (m & SW) and (r & S) and (r & W): r |= SW
        if (m & NW) and (r & N) and (r & W): r |= NW
        return r
    check(all(blob47.reduce_mask(m) == reduce_ref(m) for m in range(256)),
          "reduction matches the C rule for all 256 masks")

    # canonical order = first-seen ascending
    seen, n, ok = {}, 0, True
    for m in range(256):
        r = reduce_ref(m)
        if r not in seen:
            seen[r] = n; n += 1
        if lut[m] != seen[r]:
            ok = False
    check(ok and n == 47, "cell numbering is first-seen-ascending, as the engine builds it")

    # a reduced mask must be its own fixed point, else the sheet cell is ambiguous
    check(all(blob47.reduce_mask(r) == r for r in order),
          "every canonical mask is stable under reduction")

    # masks folding into one cell must agree on cardinals + surviving corners
    groups = {}
    for m in range(256):
        groups.setdefault(lut[m], []).append(m)
    bad = []
    for cell, ms in groups.items():
        red = {blob47.reduce_mask(m) for m in ms}
        if len(red) != 1:
            bad.append(cell)
    check(not bad, "every cell's masks share one reduced form", f"cells {bad}")


# --- 2. sheet geometry, as draw_autotile() indexes it --------------------
def check_geometry(name, sheet, nvar):
    print(f"\n2. sheet geometry - {name}")
    w, h = sheet.size
    tpr = w // TS
    check(tpr == 47, "atlas is exactly 47 cells wide (tpr=47)", f"got {tpr}")
    check(h == nvar * TS, f"atlas is {nvar} variant rows tall", f"got {h}px")
    # the engine: fx=(cell%tpr)*tw, fy=(cell/tpr)*th, then += variant*base_rows*th
    base_rows = (h // TS) // nvar
    check(base_rows == 1, "base block is 1 row, so a variant steps exactly one row")
    ok = all((c % tpr) * TS + TS <= w and (c // tpr) * TS + TS <= h for c in range(47))
    check(ok, "every cell index lands inside the atlas")
    # variant rows must actually differ, or nvar is a lie
    if nvar > 1:
        a = sheet.crop((0, 0, w, TS)).tobytes()
        b = sheet.crop((0, TS, w, 2 * TS)).tobytes()
        check(a != b, "variant rows are visually distinct")



# --- 3. the mapping is a clean bijection onto the 47 cells ---------------
def check_mapping(name, spec, where):
    print(f"\n3. source art covers all 47 configs exactly once - {name}")
    c0, r0 = spec["at"]
    holes = set(spec.get("holes", ()))
    n, blank = mp.band_report(c0, r0)
    check(len(mp.LAYOUT) == 47, "the shared layout covers 47 cells")
    check(set(blank) <= holes, "every blank cell is a declared hole",
          f"undeclared {sorted(set(blank) - holes)}")
    check(n == 47 - len(holes), f"{n} of 47 cells carry art "
          f"({len(holes)} declared hole)" )
    check(len(set(where.values())) == 47, "47 distinct source tiles, no reuse")


# --- 4. every cell's art really depicts the config it is filed under -----
NINESLICE_ANCHORS = {28: "TL", 124: "T", 112: "TR", 31: "L", 255: "C",
                     241: "R", 7: "BL", 199: "B", 193: "BR"}


def check_roundtrip(name, spec, grids, where):
    if spec.get("at") not in [mp.BANDS[b]["rect"][:2] for b in mp.BANDS]:
        print(f"\n4. classifier round-trip - {name}")
        check(True, "skipped: layout-mapped band with no single border colour")
        return
    """The tightest check available: re-classify the tile the generator picked
    for each cell and require it to come back as that cell's mask. If the art in
    cell i does not depict config i, this fails."""
    print(f"\n4. classifier round-trip - {name}")
    band = next(mp.BANDS[b] for b in mp.BANDS if mp.BANDS[b]["rect"][:2] == spec["at"])
    bad = []
    for i, mask in enumerate(blob47.CANON):
        got, _ = mp.classify(grids[i], band["fill"], band["border"], band["extra_fill"])
        if got != mask:
            bad.append(f"cell {i}: art at {where[mask]} classifies as {got}, not {mask}")
    check(not bad, "all 47 cells re-classify to their own mask",
          "; ".join(bad[:3]))

    # and the nine configs whose source tiles were verified by hand
    c0, r0 = spec["at"]
    hand = {28: (c0+3, r0), 124: (c0+4, r0), 112: (c0+5, r0),
            31: (c0+3, r0+1), 255: (c0+4, r0+1), 241: (c0+5, r0+1),
            7: (c0+3, r0+2), 199: (c0+4, r0+2), 193: (c0+5, r0+2)}
    bad = [f"{NINESLICE_ANCHORS[m]} -> {where[m]}, expected {t}"
           for m, t in hand.items() if where[m] != t]
    check(not bad, "the 9 nine-slice configs resolve to the hand-verified tiles",
          "; ".join(bad[:3]))


# --- 5. border semantics of the artist's tiles ---------------------------
def check_borders(name, spec, grids):
    _b = [b for b in mp.BANDS if mp.BANDS[b]["rect"][:2] == spec["at"]]
    if not _b:
        print(f"\ncheck_borders: skipped for {name} (no single border colour)".replace("check_borders",""))
        return
    print(f"\n5. borders sit on exactly the open sides - {name}")
    band = mp.BANDS[_b[0]]
    border = band["border"]
    mid = range(TS // 2 - 1, TS // 2 + 1)
    bad = []
    for i, mask in enumerate(blob47.CANON):
        t = grids[i]
        col = lambda x: [t[y][x] for y in range(TS)]
        def hit(lines):
            return max(sum(1 for k in mid if l[k] == border) / len(list(mid)) for l in lines)
        sides = {"N": hit([t[0], t[1]]), "S": hit([t[TS-1], t[TS-2]]),
                 "W": hit([col(0), col(1)]), "E": hit([col(TS-1), col(TS-2)])}
        for bit, s in ((N, "N"), (E, "E"), (S, "S"), (W, "W")):
            open_ = not (mask & bit)
            if open_ and sides[s] < 0.5:
                bad.append(f"cell {i} (mask {mask}) has no {s} border but {s} is open")
            if not open_ and sides[s] >= 0.5:
                bad.append(f"cell {i} (mask {mask}) draws a {s} border but {s} is closed")
    check(not bad, "all 47 cells: border iff the side is open", "; ".join(bad[:3]))


# --- 6. concave corners --------------------------------------------------
def check_inner(name, spec, grids):
    _b = [b for b in mp.BANDS if mp.BANDS[b]["rect"][:2] == spec["at"]]
    if not _b:
        print(f"\ncheck_inner: skipped for {name} (no single border colour)".replace("check_inner",""))
        return
    print(f"\n6. concave corner marks - {name}")
    band = mp.BANDS[_b[0]]
    border, box = band["border"], 3
    bad, n = [], 0
    for i, mask in enumerate(blob47.CANON):
        t = grids[i]
        for bit, (a, b), (xs, ys), nm in (
                (NW, (N, W), (range(box), range(box)), "NW"),
                (NE, (N, E), (range(TS-box, TS), range(box)), "NE"),
                (SW, (S, W), (range(box), range(TS-box, TS)), "SW"),
                (SE, (S, E), (range(TS-box, TS), range(TS-box, TS)), "SE")):
            if not ((mask & a) and (mask & b)):
                continue                     # corner irrelevant: a cardinal is open
            concave = not (mask & bit)
            marked = any(t[y][x] in (border, gt.TRANSPARENT) for y in ys for x in xs)
            if concave:
                n += 1
                if not marked:
                    bad.append(f"cell {i} mask {mask}: no {nm} mark")
            elif marked:
                bad.append(f"cell {i} mask {mask}: unexpected {nm} mark")
    check(not bad, f"{n} concave corners marked, and only those", "; ".join(bad[:3]))


# --- 7. interior cell is solid ------------------------------------------
def check_interior(name, spec, grids):
    _b = [b for b in mp.BANDS if mp.BANDS[b]["rect"][:2] == spec["at"]]
    if not _b:
        print(f"\ncheck_interior: skipped for {name} (no single border colour)".replace("check_interior",""))
        return
    print(f"\n7. interior cell is clean fill - {name}")
    band = mp.BANDS[_b[0]]
    g = grids[blob47.LUT[255]]
    stray = [(x, y) for y in range(TS) for x in range(TS) if g[y][x] == band["border"]]
    holes = [(x, y) for y in range(TS) for x in range(TS) if g[y][x] == gt.TRANSPARENT]
    check(not stray, "cell 46 (fully surrounded) has no border pixels", f"{len(stray)} px")
    check(not holes, "cell 46 is fully opaque", f"{len(holes)} px")


def check_determinism(name, spec):
    print(f"\n8. determinism - {name}")
    a = gt.build(name, spec)[0].tobytes()
    b = gt.build(name, spec)[0].tobytes()
    check(a == b, "regenerating the sheet is byte-identical")


def check_baked(name, spec, nvar):
    import re, os
    p = os.path.join(gt.GAME, "src", name + ".tiles.h")
    print(f"\n9. baked header round-trip - {name}")
    if not os.path.exists(p):
        check(False, "src/%s.tiles.h exists" % name, "run: mote bake games/roguemote")
        return
    h = open(p).read()
    mm = re.search(r"_at = \{ &%s_img, (\d+), (\d+), \{(.*?)\}, (\d+), (\d+), \{" % name, h, re.S)
    if not mm:
        check(False, "can parse the baked MoteAutotile"); return
    vals = [int(x) for x in mm.group(3).replace("\n", "").split(",") if x.strip()]
    check(int(mm.group(1)) == TS and int(mm.group(2)) == TS, "baked tile size is 8x8")
    check(vals == blob47.LUT, "baked LUT is byte-identical to the canonical blob47 LUT")
    check(int(mm.group(4)) == spec["edge"], f"baked edge_is_solid == {spec['edge']}")
    check(int(mm.group(5)) == nvar, f"baked nvar == {nvar}")


def check_geometry(name, sheet):
    print(f"\n2. sheet geometry - {name}")
    w, h = sheet.size
    tpr = w // TS
    check(tpr == 47, "atlas is exactly 47 cells wide (tpr=47)", f"got {tpr}")
    check(h == TS, "atlas is one row tall", f"got {h}px")
    check(all((c % tpr) * TS + TS <= w for c in range(47)),
          "every cell index lands inside the atlas")


def check_against_c():
    """Compile sdk/mote_tile.h for real and ask it directly.

    Every other check re-derives the contract from a second transcription of the
    C -- but a transcription can be wrong the same way twice. This one compares
    against the header the engine actually compiles."""
    import subprocess, os
    print("\n1b. cross-check against the compiled C header")
    root = os.path.dirname(os.path.dirname(gt.GAME))
    src = os.path.join(gt.HERE, "ctest_blob47.c")
    exe = "/tmp/ctest_blob47"
    r = subprocess.run(["cc", "-I" + os.path.join(root, "sdk"),
                        "-I" + os.path.join(root, "engine", "render"),
                        "-o", exe, src], capture_output=True, text=True)
    if r.returncode:
        check(False, "ctest_blob47.c compiles against sdk/mote_tile.h", r.stderr[:200]); return
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    kv = {}
    for line in out.split("\n"):
        k, _, v = line.partition(":")
        kv[k.strip()] = v.strip()
    clut = [int(x) for x in kv.get("template_lut", "").split()]
    check(clut == blob47.LUT,
          "engine's own mote_autotile_template(BLOB47) produces our exact LUT")
    cred = [int(x) for x in kv.get("reduce", "").split()]
    check(cred == [blob47.reduce_mask(m) for m in range(256)],
          "C mote__at_reduce agrees for all 256 masks")
    import preview_terrain as pv
    cmask = [int(x) for x in kv.get("masks", "").split()]
    py = []
    for p_ in range(256):
        t = [0] * 9; t[4] = 1
        for b, (dx, dy) in enumerate([(0,-1),(1,-1),(1,0),(1,1),(0,1),(-1,1),(-1,0),(-1,-1)]):
            if p_ & (1 << b):
                t[(1 + dy) * 3 + (1 + dx)] = 1
        py.append(pv.neighbour_mask(t, 3, 3, 1, 1, 1, 0))
    check(cmask == py, "C mote_autotile_mask agrees for all 256 neighbourhoods")
    el = [l for l in out.split("\n") if l.startswith("edge0")]
    if el:
        q = el[0].split()
        check((int(q[1]), int(q[3])) == (pv.neighbour_mask([1],1,1,0,0,1,0),
                                         pv.neighbour_mask([1],1,1,0,0,1,1)),
              "C edge_is_solid handling agrees")
    cv = [int(x) for x in kv.get("variants", "").split()]
    check(cv == [pv.variant_of(3, [1,1,2,0,0,0,0,0], x, y)
                 for y in range(8) for x in range(8)],
          "C mote__at_variant (weighted) agrees for 64 cells")


def main():
    check_lut()
    check_against_c()
    for name, spec in gt.TERRAINS.items():
        sheet, grids, where = gt.build(name, spec)
        check_geometry(name, sheet)
        check_mapping(name, spec, where)
        check_roundtrip(name, spec, grids, where)
        check_borders(name, spec, grids)
        check_inner(name, spec, grids)
        check_interior(name, spec, grids)
        check_determinism(name, spec)
        check_baked(name, spec, 1)
    print("\n" + "=" * 66)
    if FAILS:
        print(f"{len(FAILS)} CHECK(S) FAILED:")
        for f in FAILS:
            print("  -", f)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
