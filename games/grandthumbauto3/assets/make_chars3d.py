#!/usr/bin/env python3
"""
Generate side-view character sprites for the third-person camera.

The top-down game drew people from directly above: a head, two shoulders, and
nothing else. As a billboard that is a disc. These are the replacements --
back, front, left and right views with a walk cycle -- laid out as

    columns = facing  (0 back, 1 front, 2 left, 3 right)
    rows    = variant*nframes + frame

at 16x16 per cell, which is what draw_character in game.c expects.

The camera trails the player, so the BACK view is what is on screen almost all
the time; the other three exist for pedestrians and for the moments the camera
swings round. They are drawn as simple blocked figures at this size because a
16x16 person at 128x128 is about twelve pixels tall on screen -- detail below
that is noise. What matters at that size is silhouette: a wider head+shoulder
block than legs, a visible gap between the legs when they are apart, and a
leg that visibly swings across frames.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CELL = 16
# Transparent, NOT opaque magenta: bake_image only marks a pixel as the
# engine's colour key when its ALPHA is < 128 (see tools/mote bake_image) --
# an opaque (255,0,255,255) fill bakes as a literal solid magenta square,
# which is exactly the "billboard is a magenta box" bug this caught.
KEY = (255, 0, 255, 0)
EYE = (35, 28, 24, 255)

# (name, skin, shirt, trousers, hair, shoe, walk_frames)
# player: 4 walk frames + aim + fire (added as extra_poses below) = 6 rows.
CHARS = [
    ("player", (226, 190, 150), (60, 90, 170), (48, 48, 58), (60, 44, 32), (28, 26, 30), 4),
    ("ped",    (226, 190, 150), (170, 70, 70), (68, 64, 58), (30, 26, 22), (24, 22, 24), 4),
    ("cop",    (222, 184, 146), (36, 50, 96),  (34, 40, 58), (30, 26, 22), (20, 18, 22), 2),
]
# ped.png holds 4 variants side by side in the original; keep that by shifting
# the shirt + hair hue per variant so they read as different pedestrians.
PED_SHIRTS = [(170, 70, 70), (70, 150, 90), (180, 150, 60), (120, 80, 160)]
PED_HAIRS  = [(30, 26, 22), (50, 38, 30), (20, 20, 24), (90, 62, 40)]


def figure(d, ox, oy, facing, frame, nframes, skin, shirt, trousers, hair, shoe, pose=None):
    """One 16x16 figure. `pose` overrides the walk cycle for the player's
    aim (pose='aim') and muzzle-flash (pose='fire') frames -- those two are
    static poses, not part of the leg-swing cycle."""
    cx = ox + 7          # figure centre column (of 0..15)

    if pose in ("aim", "fire"):
        # feet planted, side-on stance, gun arm extended toward the camera
        d.rectangle([ox + 5, oy + 12, ox + 6, oy + 15], fill=trousers)
        d.rectangle([ox + 9, oy + 12, ox + 10, oy + 15], fill=trousers)
        d.rectangle([ox + 4, oy + 15, ox + 11, oy + 15], fill=shoe)
        d.rectangle([ox + 5, oy + 5, ox + 10, oy + 12], fill=shirt)
        # extended arm + gun, straight out toward the viewer
        d.rectangle([ox + 10, oy + 7, ox + 13, oy + 8], fill=shirt)
        d.rectangle([ox + 13, oy + 7, ox + 14, oy + 8], fill=(40, 40, 46, 255))
        d.rectangle([ox + 4, oy + 7, ox + 4, oy + 10], fill=shirt)
        if pose == "fire":
            d.point((ox + 15, oy + 7), fill=(255, 220, 120, 255))
            d.point((ox + 14, oy + 6), fill=(255, 200, 90, 255))
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 4], fill=skin)
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 2], fill=hair)
        d.point((ox + 9, oy + 3), fill=EYE)
        return

    # --- walk cycle over 4 phases: 0=left-fwd, 1=mid, 2=right-fwd, 3=mid.
    # A phase index instead of a continuous swing keeps every frame count
    # (2 or 4) landing on a distinct, readable pose. ---
    step = frame % 4
    # `lean`: +1 = left leg forward / right leg back, -1 = the reverse, 0 = level.
    lean = {0: 1, 1: 0, 2: -1, 3: 0}[step]

    if facing in (2, 3):                                # side view: legs fore/aft
        # In profile the near leg is the one the camera reads; swap which leg
        # is forward with `lean`, and drop the trailing leg's foot half a
        # pixel (rounds to the same row, but keeps the two legs as separate
        # rectangles so they don't fuse into one block).
        fwd = lean if facing == 3 else -lean
        front_x = ox + 8 + fwd
        back_x = ox + 6 - fwd
        d.rectangle([back_x, oy + 11, back_x + 1, oy + 14], fill=trousers)
        d.rectangle([front_x, oy + 11, front_x + 1, oy + 15], fill=trousers)
        d.rectangle([back_x, oy + 14, back_x + 1, oy + 14], fill=shoe)
        d.rectangle([front_x, oy + 15, front_x + 1, oy + 15], fill=shoe)
    else:                                                # front/back: legs apart, alternating hang
        # left leg lower when lean>0 (it's the planted/forward one), right
        # leg lower when lean<0; a real 2px GAP sits between them at x=8.
        ly = oy + 15 if lean >= 0 else oy + 13
        ry = oy + 15 if lean <= 0 else oy + 13
        d.rectangle([ox + 5, oy + 11, ox + 6, ly], fill=trousers)
        d.rectangle([ox + 9, oy + 11, ox + 10, ry], fill=trousers)
        d.rectangle([ox + 5, ly, ox + 6, ly], fill=shoe)
        d.rectangle([ox + 9, ry, ox + 10, ry], fill=shoe)

    # torso -- narrower than the old brief sketch so it doesn't swallow the
    # leg gap: same width as the head plus one pixel either side.
    d.rectangle([ox + 5, oy + 5, ox + 10, oy + 11], fill=shirt)

    # arms: swing opposite the legs so the walk reads as a walk, not a slide
    if facing in (2, 3):
        ax = ox + 4 if facing == 2 else ox + 11
        ay = oy + 7 if lean == 0 else oy + 6
        d.rectangle([ax, ay, ax, ay + 3], fill=shirt)
    else:
        al = oy + 6 if lean <= 0 else oy + 7
        ar = oy + 6 if lean >= 0 else oy + 7
        d.rectangle([ox + 3, al, ox + 4, al + 3], fill=shirt)
        d.rectangle([ox + 11, ar, ox + 12, ar + 3], fill=shirt)

    # head + hair; hair covers the whole head from behind, a fringe from the
    # front, a side-swept cap in profile -- the one cue that most separates
    # "back of a head" from "front of a head" at this resolution.
    d.rectangle([ox + 6, oy + 1, ox + 9, oy + 4], fill=skin)
    if facing == 0:                                     # back: full hair, no face
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 4], fill=hair)
    elif facing == 1:                                    # front: fringe + eyes
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 1], fill=hair)
        d.point((ox + 7, oy + 3), fill=EYE)
        d.point((ox + 8, oy + 3), fill=EYE)
    else:                                                 # side: cap + one eye
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 2], fill=hair)
        side = ox + 6 if facing == 2 else ox + 9
        d.point((side, oy + 3), fill=EYE)


def sheet(name, skin, shirt, trousers, hair, shoe, frames, variants=1, shirts=None, hairs=None,
          extra_poses=None):
    """extra_poses: list of pose names appended as extra rows after the walk
    cycle (used by the player for 'aim' and 'fire')."""
    extra_poses = extra_poses or []
    total_rows = frames + len(extra_poses)
    w, h = CELL * 4, CELL * total_rows * variants
    img = Image.new("RGBA", (w, h), KEY)
    d = ImageDraw.Draw(img)
    for v in range(variants):
        sh = shirts[v] if shirts else shirt
        hr = hairs[v] if hairs else hair
        for fr in range(frames):
            for fac in range(4):
                figure(d, fac * CELL, (v * total_rows + fr) * CELL,
                       fac, fr, frames, skin, sh, trousers, hr, shoe)
        for pi, pose in enumerate(extra_poses):
            row = frames + pi
            for fac in range(4):
                figure(d, fac * CELL, (v * total_rows + row) * CELL,
                       fac, 0, frames, skin, sh, trousers, hr, shoe, pose=pose)
    out = os.path.join(HERE, name + ".png")
    img.save(out)
    print("wrote %s (%dx%d)" % (out, w, h))


def main():
    for name, skin, shirt, trousers, hair, shoe, frames in CHARS:
        if name == "ped":
            sheet(name, skin, shirt, trousers, hair, shoe, frames,
                  variants=len(PED_SHIRTS), shirts=PED_SHIRTS, hairs=PED_HAIRS)
        elif name == "player":
            # 4 walk frames + aim + muzzle-flash = 6 rows, matching game.c's
            # draw_character(&player_img, ..., frame, 6) call.
            sheet(name, skin, shirt, trousers, hair, shoe, frames,
                  extra_poses=["aim", "fire"])
        else:
            sheet(name, skin, shirt, trousers, hair, shoe, frames)


if __name__ == "__main__":
    main()
