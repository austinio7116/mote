#!/usr/bin/env python3
"""Capture the guide and gallery screenshots from a running world, reproducibly.

The first set was taken by hand, one env-var incantation at a time, and none of the
recipes were written down — so when the house sprites changed, nothing could be
re-shot without inventing the framing again. Every published picture of the game now
has a named recipe here.

    python3 authoring/shots.py                 # every shot
    python3 authoring/shots.py town farm       # just these
    python3 authoring/shots.py --list

Guide plates are 384x384 (128 x3) and gallery shots 256x256 (128 x2), which is what
docs/motebox-guide.html and docs/games.json already expect. Runs are headless and
must stay that way — see the note in CLAUDE.md about orphan hosts blocking audio.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
HOST = os.path.join(ROOT, "build_host", "mote_host")
MODULE = os.path.join(GAME, "build", "Motebox.so")
OUT = os.path.join(ROOT, "docs", "img", "gallery")

# name -> (env, MOTE_KEYS, frame, scale, filename)
#
# frame is an INPUT frame count, and the world is a live thing: an age can turn or an
# army arrive between two nearby frames, so the frame is part of the recipe, not a
# detail. Keys use the host's own syntax, "btn:from-to".
SHOTS = {
    # --- the world, from above ---
    "god":      (dict(SEED="5", YEARS="250"), "", 300, 3, "motebox-g-god.png"),
    "world":    (dict(SEED="5", YEARS="250"), "menu:300-302 down:320-322 a:340-342", 370, 3,
                 "motebox-g-world.png"),
    "chron":    (dict(SEED="5", YEARS="250"), "menu:300-302 a:320-322", 350, 3,
                 "motebox-g-chron.png"),
    "lens":     (dict(SEED="5", YEARS="300"), "menu:300-302", 330, 3, "motebox-g-lens.png"),
    # --- towns, close up: the shots the house designs are actually in ---
    "early":    (dict(SEED="21", YEARS="70",  MORTAL="1", CAM="v"), "", 300, 3,
                 "motebox-g-early.png"),
    "town":     (dict(SEED="3",  YEARS="220", MORTAL="1", CAM="v"), "", 420, 3,
                 "motebox-g-town.png"),
    "farm":     (dict(SEED="9",  YEARS="160", MORTAL="1", CAM="f"), "", 380, 3,
                 "motebox-g-farm.png"),
    "siege":    (dict(SEED="47514", YEARS="200", MORTAL="1", CAM="v"), "", 400, 3,
                 "motebox-g-siege.png"),
    # MOTEBOX_BATTLE, because wars are rare and their front lines wander: without it the
    # camera photographed a quiet coast four times out of four.
    "war":      (dict(SEED="7919", YEARS="260", MORTAL="1", CAM="v", BATTLE="1"), "", 360, 3,
                 "motebox-g-war.png"),
    # --- the industrial age, where the towns are blocks and towers ---
    "city":     (dict(SEED="11", YEARS="420", MORTAL="1", CAM="v"), "", 440, 3,
                 "motebox-g-city.png"),
    "coast":    (dict(SEED="23757", YEARS="400", MORTAL="1", CAM="v"), "", 430, 3,
                 "motebox-g-coast.png"),
    # --- screens ---
    "pick":     (dict(SEED="47514", YEARS="300", MORTAL="1", CAM="v"), "b:400-402", 430, 3,
                 "motebox-g-pick.png"),
    "pickland": (dict(SEED="3", YEARS="220", MORTAL="1", CAM="o"), "b:400-402", 430, 3,
                 "motebox-g-pickland.png"),
    # FIVE DOWNS is where the crowned line sits in this world at this frame — the list is
    # people first and the lord is one of them, so the count is part of the recipe. One down
    # got a dwarf farmer's page, which is the same screen about nobody in particular.
    "lord":     (dict(SEED="47514", YEARS="300", MORTAL="1", CAM="v"),
                 "b:400-402 down:410-411 down:416-417 down:422-423 down:428-429 "
                 "down:434-435 a:446-447", 470, 3, "motebox-g-lord.png"),
    "powers":   (dict(SEED="7", YEARS="40"), "lb:300-302", 330, 3, "motebox-g-powers.png"),
    "nuke":     (dict(SEED="5", YEARS="150", NUKE="1", MORTAL="1"), "", 520, 3,
                 "motebox-g-nuke.png"),
    # --- the three the gallery card uses, at 2x ---
    "shot1":    (dict(SEED="5", YEARS="250"), "", 300, 2, "motebox-1.png"),
    "shot2":    (dict(SEED="3", YEARS="220", MORTAL="1", CAM="v"), "", 420, 2, "motebox-2.png"),
    "shot3":    (dict(SEED="47514", YEARS="300", MORTAL="1", CAM="v"), "b:400-402", 430, 2,
                 "motebox-3.png"),
}


def capture(name, dest=None):
    env_extra, keys, frame, scale, fname = SHOTS[name]
    ppm = "/tmp/mb_shot_%s.ppm" % name
    env = dict(os.environ)
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", MOTE_AUTORUN="1",
               MOTE_SHOT=ppm, MOTE_SHOT_FRAME=str(frame))
    if keys:
        env["MOTE_KEYS"] = keys
    for k, v in env_extra.items():
        env["MOTEBOX_" + k] = v
    subprocess.run([HOST, MODULE], env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=600)
    from PIL import Image
    im = Image.open(ppm)
    out = os.path.join(dest or OUT, fname)
    im.resize((im.width * scale, im.height * scale), Image.NEAREST).save(out)
    print("  %-9s -> %s (%dx)" % (name, fname, scale))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if "--list" in sys.argv:
        for k, v in SHOTS.items():
            print("%-9s %s" % (k, v[4]))
        sys.exit(0)
    dest = None
    if "--dry" in sys.argv:                      # write somewhere harmless to compare first
        dest = "/tmp/mb_shots"
        os.makedirs(dest, exist_ok=True)
    for n in (args or SHOTS):
        capture(n, dest)
