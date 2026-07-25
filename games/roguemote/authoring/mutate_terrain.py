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
import map_blob47 as mp
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


@mutant("mapping: file two cells under each other's config")
def m_swapcells():
    orig = mp.classify

    def patched(t, fill, border, extra=()):
        m, d = orig(t, fill, border, extra)
        if m == 28: return 112, d          # TL art filed as TR
        if m == 112: return 28, d
        return m, d
    mp.classify = patched
    return lambda: setattr(mp, "classify", orig)


@mutant("classifier: ignore concave corner marks entirely")
def m_noinner():
    orig = mp.classify

    def patched(t, fill, border, extra=()):
        m, d = orig(t, fill, border, extra)
        if m is None: return m, d
        return blob47.reduce_mask(m | NE | SE | SW | NW), d
    mp.classify = patched
    return lambda: setattr(mp, "classify", orig)


@mutant("band: shift the source rect one column right")
def m_shift():
    orig = dict(mp.BANDS["wall_brick"])
    r = orig["rect"]
    mp.BANDS["wall_brick"] = dict(orig, rect=(r[0] + 1, r[1], r[2] + 1, r[3]))
    return lambda: mp.BANDS.__setitem__("wall_brick", orig)


@mutant("art: paint out the border on one cell's open edge")
def m_wipeborder():
    orig = mp.tile

    def patched(c, r):
        t = orig(c, r)
        if (c, r) == (4, 35):              # the top-edge tile
            t[0] = [mp.BANDS["wall_brick"]["fill"]] * mp.TS
        return t
    mp.tile = patched
    return lambda: setattr(mp, "tile", orig)


def run_verifier():
    """-> (passed?, failure labels). Silences the verifier's own output."""
    vt.FAILS = []
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            vt.check_lut()
            for name, spec in gt.TERRAINS.items():
                sheet, grids, where = gt.build(name, spec)
                vt.check_geometry(name, sheet)
                vt.check_mapping(name, spec, where)
                vt.check_roundtrip(name, spec, grids, where)
                vt.check_borders(name, spec, grids)
                vt.check_inner(name, spec, grids)
                vt.check_interior(name, spec, grids)
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
