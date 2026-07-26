#!/usr/bin/env python3
"""
Regenerate the guide's screenshots from the CURRENT build.

They were hand-placed PNGs, which means they age silently: the guide went on
showing a 64x48 world map, a four-slot gear screen and a town with nobody in it
long after none of those were true. This drives the host emulator's frame
recorder to reach each screen and saves the frame.

Every shot is one entry in SHOTS: where to start (depth, character level, gear),
which buttons to press on which frames, and which frame to keep. Add a screen by
adding a row.

Usage:  python3 authoring/gen_shots.py            # all of them
        python3 authoring/gen_shots.py pack gear  # just these
"""
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
OUT = os.path.join(HERE, "guide_shots")

# A is pressed at 5-8 to leave the title and at 20-23 to commit the class, so
# every script below starts already in play unless it says otherwise.
ENTER = "a:5-8 a:20-23"

SHOTS = {
    # name          env                                    keys                       frame
    "title":       (dict(),                                "",                            4),
    "class":       (dict(),                                "a:5-8",                      16),
    "overworld":   (dict(LEVEL="8"),                       ENTER + " right:30-44",       50),
    "town":        (dict(LEVEL="8"),                       ENTER,                        30),
    "worldmap":    (dict(LEVEL="8"),                       ENTER + " menu:30-33 rb:40-42 rb:47-49 rb:54-56 rb:61-63", 70),
    "dungeon":     (dict(DEPTH="5",  LEVEL="12"),          ENTER,                        30),
    "levelmap":    (dict(DEPTH="5",  LEVEL="12", REVEAL="1"), ENTER + " menu:30-33 rb:40-42 rb:47-49 rb:54-56 rb:61-63", 70),
    "pack":        (dict(DEPTH="4",  LEVEL="10"),          ENTER + " menu:30-33",        40),
    "gear":        (dict(DEPTH="4",  LEVEL="10", GEAR="bow"), ENTER + " menu:30-33 rb:40-42", 50),
    # class 5 is the Mage -- a Warrior's spell page reads "You know no magic",
    # which is true and useless as an illustration of the spell page
    "spells":      (dict(DEPTH="4",  LEVEL="18", CLASS="5"), ENTER + " menu:30-33 rb:40-42 rb:47-49", 56),
    "character":   (dict(DEPTH="4",  LEVEL="18", CLASS="5"), ENTER + " menu:30-33 rb:40-42 rb:47-49 rb:54-56", 62),
    # the effect strips are greyscale and tinted per spell; catch one mid-flight
    "nova":        (dict(DEPTH="6",  LEVEL="30", CLASS="5"),
                    ENTER + " menu:26-29 rb:34-36 rb:41-43 down:48-64 a:70-73", 78),
    # Magic Missile is spell 0, Fireball is row 8 -- both bolts/balls, and the
    # point of the pair is that they are the SAME frames in two colours.
    "missile":     (dict(DEPTH="6",  LEVEL="30", CLASS="5"),
                    ENTER + " menu:26-29 rb:34-36 rb:41-43 a:50-53", 56),
    "fireball":    (dict(DEPTH="6",  LEVEL="30", CLASS="5"),
                    ENTER + " menu:26-29 rb:34-36 rb:41-43 down:48-72 a:78-81", 86),
    "shop":        (dict(LEVEL="6"),                       ENTER + " a:30-33",           40),
}


def shoot(name, env, keys, frame):
    tmp = tempfile.mkdtemp(prefix="shot_")
    e = dict(os.environ)
    e.update(MOTE_AUTORUN="1", MOTE_REC=tmp, MOTE_REC_N=str(frame + 6),
             MOTE_DT_MS="33", MOTE_KEYS=keys)
    for k, v in env.items():
        e["MOTE_RL_" + k] = v
    subprocess.run([os.path.join(ROOT, "tools", "mote"), "run",
                    "games/roguemote"], cwd=ROOT, env=e,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    src = os.path.join(tmp, "%05d.ppm" % frame)
    if not os.path.exists(src):
        shutil.rmtree(tmp, ignore_errors=True)
        return "no frame %d" % frame
    Image.open(src).convert("RGB").save(os.path.join(OUT, name + ".png"))
    shutil.rmtree(tmp, ignore_errors=True)
    return None


want = sys.argv[1:] or list(SHOTS)
os.makedirs(OUT, exist_ok=True)
bad = 0
for name in want:
    if name not in SHOTS:
        print("  ?  %-11s no such shot" % name)
        bad += 1
        continue
    env, keys, frame = SHOTS[name]
    err = shoot(name, env, keys, frame)
    print("  %s  %-11s %s" % ("FAIL" if err else "ok  ", name, err or ""))
    bad += bool(err)
print("%d shot(s), %d failed" % (len(want), bad))
sys.exit(1 if bad else 0)
