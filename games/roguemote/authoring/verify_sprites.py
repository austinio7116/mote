#!/usr/bin/env python3
"""Fail if any content table points at a blank sprite cell.

The sheets have gaps -- `monsters` is populated at cells 0..18 and 48.., with
nothing in between -- and a table entry that lands in a gap draws an invisible
monster. That is exactly the bug this catches: six entries in the first draft of
rl_data.c pointed into the monsters gap, and the only symptom was "no monsters
appear", which is very easy to misread as an AI or spawn bug.

Parses the C tables directly rather than duplicating them, so it cannot drift.

    python3 verify_sprites.py
"""
import os, re, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SHEETS = os.path.join(GAME, "assets", "sheets")

# macro name in rl_data.c -> sheet file
MACRO_SHEET = {"A": "animals", "M": "monsters", "B": "bosses", "C": "characters"}


def occupancy(name):
    im = Image.open(os.path.join(SHEETS, name + ".png")).convert("RGBA")
    px, cols = im.load(), im.width // 8
    n = cols * (im.height // 8)
    return cols, [any(px[(i % cols) * 8 + x, (i // cols) * 8 + y][3]
                      for y in range(8) for x in range(8)) for i in range(n)]


def main():
    src = open(os.path.join(GAME, "src", "rl_data.c")).read()
    occ = {k: occupancy(v) for k, v in MACRO_SHEET.items()}

    bad, checked = [], 0
    for line in src.split("\n"):
        # { "name", A(12), ... }
        m = re.search(r'\{\s*"([^"]+)"\s*,\s*([ABMC])\((\d+)\)', line)
        if not m:
            continue
        name, macro, cell = m.group(1), m.group(2), int(m.group(3))
        cols, filled = occ[macro]
        checked += 1
        if cell >= len(filled):
            bad.append(f'{name}: {MACRO_SHEET[macro]} cell {cell} is past the end of the sheet')
        elif not filled[cell]:
            bad.append(f'{name}: {MACRO_SHEET[macro]} cell {cell} '
                       f'(col {cell % cols}, row {cell // cols}) is BLANK')

    print(f"checked {checked} content-table sprite references")
    for s in bad:
        print("  FAIL " + s)
    if bad:
        print(f"\n{len(bad)} table entr{'y' if len(bad) == 1 else 'ies'} "
              f"point at a blank cell -- they would draw nothing")
        sys.exit(1)
    print("all reference a non-empty cell")


if __name__ == "__main__":
    main()
