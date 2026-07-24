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


# --- 3. the nine configs a nine-slice can express ------------------------
# For each, which source tile the composite must equal, pixel for pixel.
NINESLICE_ANCHORS = {
    28:  "TL",   # E+SE+S           - N,W open  -> top-left corner of a region
    112: "TR",   # S+SW+W           - N,E open
    7:   "BL",   # N+NE+E           - S,W open
    193: "BR",   # N+W+NW           - S,E open
    124: "T",    # E+SE+S+SW+W      - N open
    199: "B",    # N+NE+E+W+NW      - S open
    31:  "L",    # N+NE+E+SE+S      - W open
    241: "R",    # N+S+SW+W+NW      - E open
    255: "C",    # fully surrounded
}


def check_anchors(name, spec, variants):
    print(f"\n3. nine-slice anchors composite back to the source art - {name}")
    for v, blk in enumerate(spec["blocks"]):
        ns = gt.load_nineslice(*blk)
        grids = variants[v]
        bad = []
        for mask, key in NINESLICE_ANCHORS.items():
            cell = blob47.LUT[mask]
            if grids[cell] != ns[key]:
                nd = sum(1 for y in range(TS) for x in range(TS)
                         if grids[cell][y][x] != ns[key][y][x])
                bad.append(f"mask {mask}->cell {cell} vs {key} ({nd}px)")
        check(not bad, f"variant {v}: all 9 anchors are pixel-exact", "; ".join(bad))


# --- 4/5/6. border + corner semantics ------------------------------------
def border_sides(grid, ns, m):
    """Which sides of this composed tile actually carry border art."""
    C = ns["C"]
    out = {}
    out["N"] = any(grid[y][x] != C[y][x] for y in range(m["top"]) for x in range(TS))
    out["S"] = any(grid[y][x] != C[y][x] for y in range(TS - m["bottom"], TS) for x in range(TS))
    out["W"] = any(grid[y][x] != C[y][x] for x in range(m["left"]) for y in range(TS))
    out["E"] = any(grid[y][x] != C[y][x] for x in range(TS - m["right"], TS) for y in range(TS))
    return out


def check_borders(name, spec, variants, mets):
    print(f"\n4. borders appear on exactly the open sides - {name}")
    for v, blk in enumerate(spec["blocks"]):
        ns, m, grids = gt.load_nineslice(*blk), mets[v], variants[v]
        bad = []
        ir = m["inner"]
        for i, mask in enumerate(blob47.CANON):
            has = border_sides(grids[i], ns, m)
            for bit, side in ((N, "N"), (E, "E"), (S, "S"), (W, "W")):
                open_ = not (mask & bit)
                if open_ and not has[side]:
                    bad.append(f"cell {i} (mask {mask}) missing {side} border")
            # And the other direction: a CLOSED side must carry no border of its
            # own. Two things legitimately intrude into that side's band and must
            # be excluded first, or the check reports art that belongs to a
            # neighbouring feature: a concave notch (inner_radius), and the
            # border of a PERPENDICULAR side that is open -- with hedge's 3px
            # borders the left band reaches well into the top band.
            C = ns["C"]
            oN_, oE_ = not (mask & N), not (mask & E)
            oS_, oW_ = not (mask & S), not (mask & W)
            x_lo = m["left"] if oW_ else ir
            x_hi = TS - (m["right"] if oE_ else ir)
            y_lo = m["top"] if oN_ else ir
            y_hi = TS - (m["bottom"] if oS_ else ir)
            spans = {
                "N": [(x, y) for y in range(m["top"]) for x in range(x_lo, x_hi)],
                "S": [(x, y) for y in range(TS - m["bottom"], TS) for x in range(x_lo, x_hi)],
                "W": [(x, y) for x in range(m["left"]) for y in range(y_lo, y_hi)],
                "E": [(x, y) for x in range(TS - m["right"], TS) for y in range(y_lo, y_hi)],
            }
            for bit, side in ((N, "N"), (E, "E"), (S, "S"), (W, "W")):
                if mask & bit:      # this side is same-terrain: no border allowed
                    if any(grids[i][y][x] != C[y][x] for x, y in spans[side]):
                        bad.append(f"cell {i} (mask {mask}) has a {side} border it should not")
        check(not bad, f"variant {v}: borders on open sides only, none on closed sides",
              "; ".join(bad[:4]) + (" ..." if len(bad) > 4 else ""))

    print(f"\n5. concave (inner) corners - {name}")
    for v, blk in enumerate(spec["blocks"]):
        ns, m, grids = gt.load_nineslice(*blk), mets[v], variants[v]
        ir = m["inner"]
        bad, n_inner = [], 0
        for i, mask in enumerate(blob47.CANON):
            # the same config with every corner filled in = no concave corners
            plain = gt.compose(ns, m, mask | NE | SE | SW | NW, ir)
            corners = {"NW": (NW, range(ir), range(ir)),
                       "NE": (NE, range(TS - ir, TS), range(ir)),
                       "SW": (SW, range(ir), range(TS - ir, TS)),
                       "SE": (SE, range(TS - ir, TS), range(TS - ir, TS))}
            for cname, (bit, xs, ys) in corners.items():
                need = {"NW": (N, W), "NE": (N, E), "SW": (S, W), "SE": (S, E)}[cname]
                concave = bool((mask & need[0]) and (mask & need[1]) and not (mask & bit))
                differs = any(grids[i][y][x] != plain[y][x] for y in ys for x in xs)
                # iff, not just if: a notch in a corner that is NOT concave is
                # just as wrong as a missing one, and only checking one direction
                # would let "notch every corner" pass.
                if concave:
                    n_inner += 1
                    if not differs:
                        bad.append(f"cell {i} mask {mask}: missing {cname} notch")
                elif differs:
                    bad.append(f"cell {i} mask {mask}: spurious {cname} notch")
            outside = [(x, y) for y in range(TS) for x in range(TS)
                       if grids[i][y][x] != plain[y][x]
                       and not (min(x, TS-1-x) < ir and min(y, TS-1-y) < ir)]
            if outside:
                bad.append(f"cell {i} mask {mask}: changed {len(outside)}px outside corner boxes")
        check(not bad, f"variant {v}: {n_inner} concave corners all notched, nothing else touched",
              "; ".join(bad[:4]) + (" ..." if len(bad) > 4 else ""))

    print(f"\n6. opposite-edge configs a nine-slice cannot express - {name}")
    # 1-wide vertical spur: W and E both open. 1-wide horizontal: N and S both open.
    for v, blk in enumerate(spec["blocks"]):
        ns, m, grids = gt.load_nineslice(*blk), mets[v], variants[v]
        cases = {
            "isolated (no neighbours)":            0,
            "vertical spur (N+S, no E/W)":         N | S,
            "horizontal spur (E+W, no N/S)":       E | W,
            "column top (S only)":                 S,
            "column bottom (N only)":              N,
            "row left end (E only)":               E,
            "row right end (W only)":              W,
        }
        bad = []
        for label, mask in cases.items():
            g = grids[blob47.LUT[mask]]
            has = border_sides(g, ns, m)
            want = {"N": not (mask & N), "E": not (mask & E),
                    "S": not (mask & S), "W": not (mask & W)}
            for side in "NESW":
                if want[side] and not has[side]:
                    bad.append(f"{label}: no {side} border")
        check(not bad, f"variant {v}: spurs and stubs carry every open border",
              "; ".join(bad))


# --- 7. determinism ------------------------------------------------------
def check_interior(name, spec, variants):
    """The fully-surrounded cell is drawn as the bare centre tile, so it must be
    solid: no border colour, no transparency. This is the check that caught two
    blocks whose centres carry a bright post in each corner -- they would have
    scattered dots through solid walls."""
    print(f"\n7. interior cells are clean fill - {name}")
    interior = blob47.LUT[255]
    for v, blk in enumerate(spec["blocks"]):
        ns = gt.load_nineslice(*blk)
        ok, bad = gt.fill_is_clean(ns)
        check(ok, f"variant {v}: source centre tile is solid fill",
              f"border colour {bad} found inside it")
        g = variants[v][interior]
        outline = set(ns["T"][0]) - {gt.TRANSPARENT}
        stray = [(x, y) for y in range(TS) for x in range(TS) if g[y][x] in outline]
        check(not stray, f"variant {v}: cell {interior} (mask 255) has no border pixels",
              f"{len(stray)} stray px at {stray[:4]}")
        holes = [(x, y) for y in range(TS) for x in range(TS) if g[y][x] == gt.TRANSPARENT]
        check(not holes, f"variant {v}: cell {interior} is fully opaque",
              f"{len(holes)} transparent px")


def check_determinism(name, spec):
    print(f"\n8. determinism - {name}")
    a = gt.build(name, spec)[0].tobytes()
    b = gt.build(name, spec)[0].tobytes()
    check(a == b, "regenerating the sheet is byte-identical")


def check_baked(name, spec, nvar):
    """Close the loop: whatever `mote bake` wrote into src/<name>.tiles.h must
    still be the canonical LUT. Catches a stale header or a bake-format change."""
    import re, os
    p = os.path.join(gt.GAME, "src", name + ".tiles.h")
    print(f"\n9. baked header round-trip - {name}")
    if not os.path.exists(p):
        check(False, "src/%s.tiles.h exists" % name, "run: mote bake games/roguemote")
        return
    h = open(p).read()
    mm = re.search(r"_at = \{ &%s_img, (\d+), (\d+), \{(.*?)\}, (\d+), (\d+), \{" % name, h, re.S)
    if not mm:
        check(False, "can parse the baked MoteAutotile")
        return
    vals = [int(x) for x in mm.group(3).replace("\n", "").split(",") if x.strip()]
    check(int(mm.group(1)) == TS and int(mm.group(2)) == TS, "baked tile size is 8x8")
    check(vals == blob47.LUT, "baked LUT is byte-identical to the canonical blob47 LUT")
    check(int(mm.group(4)) == spec["edge"], f"baked edge_is_solid == {spec['edge']}")
    check(int(mm.group(5)) == nvar, f"baked nvar == {nvar}")


def check_against_c():
    """Compile sdk/mote_tile.h for real and ask it directly.

    Every other check re-derives the contract from a second transcription of the
    C -- but a transcription can be wrong the same way twice. This one compares
    against the header the engine actually compiles.
    """
    import subprocess, os
    print("\n10. cross-check against the compiled C header")
    root = os.path.dirname(os.path.dirname(gt.GAME))
    src = os.path.join(gt.HERE, "ctest_blob47.c")
    exe = "/tmp/ctest_blob47"
    r = subprocess.run(["cc", "-I" + os.path.join(root, "sdk"),
                        "-I" + os.path.join(root, "engine", "render"),
                        "-o", exe, src], capture_output=True, text=True)
    if r.returncode:
        check(False, "ctest_blob47.c compiles against sdk/mote_tile.h", r.stderr[:200])
        return
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
    for p in range(256):
        t = [0] * 9; t[4] = 1
        for b, (dx, dy) in enumerate([(0, -1), (1, -1), (1, 0), (1, 1),
                                      (0, 1), (-1, 1), (-1, 0), (-1, -1)]):
            if p & (1 << b):
                t[(1 + dy) * 3 + (1 + dx)] = 1
        py.append(pv.neighbour_mask(t, 3, 3, 1, 1, 1, 0))
    check(cmask == py, "C mote_autotile_mask agrees for all 256 neighbourhoods")

    el = [l for l in out.split("\n") if l.startswith("edge0")]
    if el:
        p = el[0].split()
        check((int(p[1]), int(p[3])) == (pv.neighbour_mask([1], 1, 1, 0, 0, 1, 0),
                                         pv.neighbour_mask([1], 1, 1, 0, 0, 1, 1)),
              "C edge_is_solid handling agrees")
    cv = [int(x) for x in kv.get("variants", "").split()]
    check(cv == [pv.variant_of(3, [1, 1, 2, 0, 0, 0, 0, 0], x, y)
                 for y in range(8) for x in range(8)],
          "C mote__at_variant (weighted) agrees for 64 cells")


def main():
    check_lut()
    check_against_c()
    for name, spec in gt.TERRAINS.items():
        sheet, variants, mets = gt.build(name, spec)
        check_geometry(name, sheet, len(variants))
        check_anchors(name, spec, variants)
        check_borders(name, spec, variants, mets)
        check_interior(name, spec, variants)
        check_determinism(name, spec)
        check_baked(name, spec, len(variants))
    print("\n" + "=" * 66)
    if FAILS:
        print(f"{len(FAILS)} CHECK(S) FAILED:")
        for f in FAILS:
            print("  -", f)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
