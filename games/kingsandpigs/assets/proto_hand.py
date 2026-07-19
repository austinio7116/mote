#!/usr/bin/env python3
"""Hand-built demo floor v4 — MANY SMALL chambers, no big rectangles. Each is
shaped with SOLID BRICK terrain (stairs, pillars, alcoves, raised blocks,
layered floors) so no floor is ever flat; wood planks add extra routes on top;
enemies populate the shapes. Real-tile render to lock the look.

Usage: python3 assets/proto_hand.py [out.png]
"""
import os, sys
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proto_layout as PL

CW, CH = 34, 40
cv = [[' '] * CW for _ in range(CH)]
def fill(x, y, w, h, ch='#'):
    for r in range(y, y+h):
        for c in range(x, x+w):
            if 0 <= r < CH and 0 <= c < CW: cv[r][c] = ch
def carve(x, y, w, h): fill(x, y, w, h, '.')
def rw(x, y, w, ch):
    for c in range(x, x+w):
        if 0 <= y < CH and 0 <= c < CW: cv[y][c] = ch
def solid(x, y, w, h): fill(x, y, w, h, '#')
def put(x, y, ch): cv[y][x] = ch
def stairs(x, floor, n, d=1):          # solid brick staircase, 1-tile steps
    for i in range(n):
        c = x + i*d
        solid(c, floor-1-i, 1, i+1)
def drop(x, y, w=2):                    # optional-drop plank over a hole
    rw(x, y, w, '='); rw(x, y+1, w, '.')

fill(0, 0, CW, CH, '#')                 # solid mass; carve small rooms out of it

# ============ R1 entry (top-left, small) — stepped floor + alcove =========
carve(2, 2, 8, 4)                       # rows 2-5, floor row 6 below
put(3, 2, 'W')                          # window
stairs(6, 6, 3, d=1)                    # brick stairs rising right
put(3, 5, 'e'); put(2, 5, 'B')          # door + crate on the low floor
carve(1, 4, 1, 2); put(1, 4, 'd')       # left alcove w/ gem
put(5, 5, 'E')
put(8, 3, 'd')
drop(8, 6)                              # drop to R2 (right of the stairs top)

# ============ R2 (below-left, small) — raised plinth + pillar ============
carve(2, 7, 9, 4)                       # rows 7-10, floor row 11
solid(5, 9, 2, 2)                       # central raised brick plinth
put(5, 8, 'd'); put(6, 8, 'd')          # loot on the plinth
solid(9, 8, 1, 3)                       # pillar splitting the room
put(3, 10, 'E'); put(8, 10, 'E')
rw(2, 8, 2, '=')                        # a wood shelf (upper route, left)
put(2, 7, 'F')                          # banner
drop(3, 11)                             # drop down-left to R4

# ============ R3 (top-right of R1, small) reached over the stairs ========
carve(11, 2, 8, 4)                      # rows 2-5
stairs(17, 6, 3, d=-1)                  # stairs rising left (mirror)
solid(11, 4, 2, 2)                      # corner brick block (alcove shape)
put(12, 3, 'd'); put(15, 5, 'E')
put(18, 2, 'W')
rw(13, 4, 3, '-')                       # beam route
drop(12, 6)                             # drop to R5

# doorway linking R1-top to R3 (walk across the stair tops)
carve(10, 5, 1, 1)

# ============ R4 (mid-left, small) — layered floor (two heights) =========
carve(2, 12, 8, 5)                      # rows 12-16
solid(2, 14, 3, 3)                      # left floor raised 2 (upper lane)
solid(6, 15, 4, 2)                      # right floor raised 1 (step down)
put(3, 13, 'd'); put(7, 14, 'E')
put(4, 13, 'h')
carve(9, 13, 1, 1)                      # doorway right -> R5
rw(2, 12, 3, '=')
drop(3, 16, 2)                          # actually keep floor; small hop

# ============ R5 (mid-right, small) — cannon nook + pillars ============
carve(11, 12, 9, 5)                     # rows 12-16
solid(14, 12, 1, 3)                     # hanging pillar from ceiling
solid(11, 15, 2, 2)                     # brick step
put(17, 16, 'C')                        # cannon on the floor
put(12, 14, 'E'); put(18, 13, 'b')
rw(15, 14, 4, '=')                      # plank shelf (upper route)
put(16, 13, 'd')
put(19, 12, 'W')
drop(15, 17)                            # drop to R7

# ============ R6 side vault (far right, small treasure) ================
carve(21, 12, 6, 4)
solid(21, 14, 2, 2); put(22, 13, 'D')
put(25, 13, 'd'); put(24, 15, 'd')
carve(20, 14, 1, 1)                     # door from R5 area

# ============ R7 (lower-centre, small) — staircase down + alcove ======
carve(9, 18, 9, 5)                      # rows 18-22
stairs(10, 22, 4, d=1)                  # big brick staircase descending
put(9, 21, 'B'); put(9, 20, 'B')
put(15, 19, 'E'); put(12, 21, 'd')
carve(8, 20, 1, 1)                      # door left -> R8
rw(14, 20, 3, '-')                      # beam
put(16, 18, 'F')
drop(15, 23)

# ============ R8 (lower-left, small) — pillars + pit ===================
carve(2, 18, 6, 5)                      # rows 18-22
solid(4, 18, 1, 3)                      # pillar
solid(2, 21, 2, 2)                      # raised block
put(3, 20, 'd'); put(6, 21, 'E')
put(2, 18, 'W')
rw(5, 20, 2, '=')

# ============ R9 (bottom, small) — exit with stepped approach =========
carve(9, 24, 11, 5)                     # rows 24-28
stairs(10, 28, 3, d=1)                  # steps down to the door pit
solid(16, 26, 2, 2)                     # raised brick block by the exit
put(17, 25, 'd')
put(13, 28, 'x')                        # exit doors
put(11, 27, 'E'); put(18, 27, 'B')
rw(9, 26, 3, '='); put(9, 24, 'F')

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/kp_proto_hand.png"
    PL.render(cv).save(out)
    print("wrote", out)

if __name__ == "__main__":
    main()
