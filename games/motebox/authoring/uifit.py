"""Does every string on the information screens actually FIT?

The mockups for these screens were laid out with a six-pixel monospace advance. rogue8 is
PROPORTIONAL and its capitals are eight or nine pixels wide, so the first live build truncated
"SHAPE" to "SHAP", "GLADDEN" to "GLADDE" and "hunger" to "hunge", and ran a family name into a
place name. Eyeballing screenshots found those; it would not have found the ones that only
truncate for a long name.

So this measures every mb_ui_text / mb_ui_text_r literal in the screens against the real glyph
advances out of rogue8.font.h, and against the panel's real interior width. Run it after
touching a screen.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
INTERIOR = 114          # 128 less the double frame and a margin either side

def advances():
    s = open(os.path.join(GAME, "src", "rogue8.font.h")).read()
    blk = s[s.index("rogue8_glyphs[128] = {"):]
    blk = blk[:blk.index("};")]
    return [int(a) for a in re.findall(r"\{(\d+),", blk)]

ADV = advances()

def width(t):
    return sum(ADV[ord(c)] if ord(c) < len(ADV) else 4 for c in t)

def main():
    src = open(os.path.join(GAME, "src", "game.c")).read()
    bad = 0
    # mb_ui_text(fb, x, y, "literal", col, maxw)
    for m in re.finditer(r'mb_ui_text\(fb,\s*(\d+),\s*(\d+),\s*"((?:[^"\\]|\\.)*)"\s*,'
                         r'\s*[A-Za-z_0-9]+\s*,\s*(\d+)\)', src):
        x, y, txt, maxw = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))
        w = width(txt)
        room = min(maxw, 121 - x)
        if w > room:
            bad += 1
            print("TRUNCATES  y%-4s x%-4s %3dpx > %3dpx  %r" % (y, x, w, room, txt))
    # mb_ui_text_r(fb, x2, y, "literal", col) — right aligned, must not run off the left
    for m in re.finditer(r'mb_ui_text_r\(fb,\s*(\d+),\s*(\d+),\s*"((?:[^"\\]|\\.)*)"', src):
        x2, y, txt = int(m.group(1)), int(m.group(2)), m.group(3)
        w = width(txt)
        if x2 - w < 6:
            bad += 1
            print("OVERRUNS   y%-4s r%-4s %3dpx  %r" % (y, x2, w, txt))
    # UNDER THE ACTION BAR. The bar owns y 112..121 on every screen, so a row placed at 105
    # with an 8-pixel glyph is clipped by it — which is how a spouse's name went missing.
    for m in re.finditer(r'mb_ui_(?:text|text_r|icon|meter|hearts|traits)\(fb,[^;]*?,\s*(\d{2,3})\s*[,)]',
                         src):
        pass    # positions vary by widget; the y check below reads the common two-arg forms
    for m in re.finditer(r'mb_ui_text\(fb,\s*\d+,\s*(\d+),', src):
        y = int(m.group(1))
        if 104 < y < 112:
            bad += 1
            print("CLIPPED    y%-4s sits under the action bar (owns 112..121)" % y)

    # AND EVERY OTHER LITERAL ON A SCREEN. The worst offenders reach mb_ui_text through a
    # ternary or a helper return ("WILL STRIKE FIRST" is 141px), which no call-site pattern
    # sees. Any literal in a ui_draw_* function that is wider than the interior is wrong
    # however it gets there, so check them all and let format strings through.
    for fm in re.finditer(r'\nstatic void ui_draw_\w+\(uint16_t \*fb\)\n\{', src):
        body = src[fm.end():]
        end = body.find("\n}\n")
        body = body[:end if end > 0 else len(body)]
        for lm in re.finditer(r'"((?:[^"\\]|\\.)*)"', body):
            txt = lm.group(1)
            if "%" in txt or len(txt) < 4:
                continue
            w = width(txt)
            if w > INTERIOR:
                bad += 1
                print("TOO WIDE   %3dpx > %3dpx  %r" % (w, INTERIOR, txt))
    if bad:
        print("\n%d string(s) will not fit. The interior is %dpx." % (bad, INTERIOR))
    else:
        print("all screen strings fit (interior %dpx)" % INTERIOR)
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
