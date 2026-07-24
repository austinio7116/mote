#!/usr/bin/env python3
"""Mutation test: prove verify_terrain.py actually catches things.

A suite that passes tells you nothing unless it can fail. This deliberately
breaks the generator in five plausible ways -- each a mistake that is easy to
make for real -- and asserts the verifier rejects every one.

    python3 mutate_terrain.py
"""
import sys, io, contextlib

import blob47
import gen_terrain as gt
import verify_terrain as vt
from blob47 import N, NE, E, SE, S, SW, W, NW

MUTANTS = {}


def mutant(label):
    def deco(fn):
        MUTANTS[label] = fn
        return fn
    return deco


@mutant("LUT: swap the mapping of two masks")
def m_lut():
    orig = list(blob47.LUT)
    blob47.LUT[28], blob47.LUT[112] = blob47.LUT[112], blob47.LUT[28]
    return lambda: blob47.LUT.__setitem__(slice(None), orig)


@mutant("compose: never draw the concave (inner) corner notch")
def m_inner():
    orig = gt.compose

    def patched(ns, m, mask, inner_radius):
        # fill every corner bit first, so no corner is ever seen as concave
        return orig(ns, m, mask | NE | SE | SW | NW, inner_radius)
    gt.compose = patched
    return lambda: setattr(gt, "compose", orig)


@mutant("compose: nine-slice fallback, so opposite edges can't both draw")
def m_opposite():
    orig = gt.compose

    def patched(ns, m, mask, inner_radius):
        # classic nine-slice column/row pick: when BOTH sides are open it
        # collapses to the middle, losing one of the two borders
        oN, oE = not (mask & N), not (mask & E)
        oS, oW = not (mask & S), not (mask & W)
        if oW and oE:
            mask |= W                      # pretend the left side is closed
        if oN and oS:
            mask |= N
        return orig(ns, m, mask, inner_radius)
    gt.compose = patched
    return lambda: setattr(gt, "compose", orig)


@mutant("compose: put the notch in the wrong corner (NW <-> NE)")
def m_swapcorner():
    orig = gt.compose

    def patched(ns, m, mask, inner_radius):
        g = orig(ns, m, mask, inner_radius)
        ir = inner_radius
        for y in range(ir):                # mirror the two top corner boxes
            for x in range(ir):
                a, b = g[y][x], g[y][gt.TS - 1 - x]
                g[y][x], g[y][gt.TS - 1 - x] = b, a
        return g
    gt.compose = patched
    return lambda: setattr(gt, "compose", orig)


@mutant("terrain: accept a block whose centre is not clean fill")
def m_dirtyfill():
    orig = dict(gt.TERRAINS["wall_brick"])
    gt.TERRAINS["wall_brick"] = dict(orig, blocks=[(3, 35), (0, 35)])
    ocheck = gt.fill_is_clean
    gt.fill_is_clean = lambda ns: (True, [])      # disable the build-time guard
    def restore():
        gt.TERRAINS["wall_brick"] = orig
        gt.fill_is_clean = ocheck
    return restore


def run_verifier():
    """-> (passed?, failure labels). Silences the verifier's own output."""
    vt.FAILS = []
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            vt.check_lut()
            for name, spec in gt.TERRAINS.items():
                sheet, variants, mets = gt.build(name, spec)
                vt.check_geometry(name, sheet, len(variants))
                vt.check_anchors(name, spec, variants)
                vt.check_borders(name, spec, variants, mets)
                vt.check_interior(name, spec, variants)
    except SystemExit as e:
        return False, [f"build refused: {e}"]
    except Exception as e:
        return False, [f"raised {type(e).__name__}: {e}"]
    return not vt.FAILS, list(vt.FAILS)


def main():
    clean_ok, _ = run_verifier()
    print(f"baseline (unmutated): {'passes' if clean_ok else 'FAILS -- fix before mutating'}")
    if not clean_ok:
        sys.exit(1)

    bad = []
    for label, make in MUTANTS.items():
        restore = make()
        try:
            ok, fails = run_verifier()
        finally:
            restore()
        if ok:
            print(f"  NOT CAUGHT  {label}")
            bad.append(label)
        else:
            first = fails[0][:78] if fails else "?"
            print(f"  caught      {label}\n                -> {first}")

    again_ok, _ = run_verifier()
    print(f"\nrestored cleanly: {'yes' if again_ok else 'NO'}")
    if bad or not again_ok:
        print(f"\n{len(bad)} mutation(s) slipped through")
        sys.exit(1)
    print(f"all {len(MUTANTS)} mutations caught")


if __name__ == "__main__":
    main()
