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
    # THE GIFT LIST, scrolled down far enough to show UNDYING at the bottom of it. SANDBOX
    # makes casting free so the Faith column is not all red.
    "gifts":    (dict(SEED="3", YEARS="220", MORTAL="1", CAM="v", NOFOLLOW="1", SANDBOX="1"),
                 "b:200-202 a:220-222 a:250-252 down:270-272 down:280-282 down:290-292 "
                 "down:300-302 down:310-312 down:320-322 down:330-332", 350, 3,
                 "motebox-g-gifts.png"),
    # MOTEBOX_PAGE OPENS AN INFO SCREEN DIRECTLY. These used to press B and then guess how many
    # DOWNs reached the crowned line of a list whose contents change with the world — so the
    # moment the world changed, the recipe quietly produced a picture of a patch of grass, and
    # that is what the guide shipped.
    "townscr":  (dict(SEED="3", YEARS="220", MORTAL="1", CAM="v", NOFOLLOW="1", PAGE="town"),
                 "", 40, 3, "motebox-g-townscr.png"),
    "lord":     (dict(SEED="3", YEARS="220", MORTAL="1", CAM="v", NOFOLLOW="1", PAGE="lord"),
                 "", 40, 3, "motebox-g-lord.png"),
    # THE SIX PAGES OF POWERS. One shot each, because the wheel is a third of what the god
    # actually does and the guide had a single picture of one page of it. RB turns the page.
    "powers":   (dict(SEED="7", YEARS="40"), "lb:300-302", 330, 3, "motebox-g-powers.png"),
    "pw_life":  (dict(SEED="7", YEARS="40"), "lb:300-302 rb:320-322", 340, 3,
                 "motebox-g-pw-life.png"),
    "pw_bless": (dict(SEED="7", YEARS="40"), "lb:300-302 rb:320-322 rb:330-332", 350, 3,
                 "motebox-g-pw-bless.png"),
    "pw_curse": (dict(SEED="7", YEARS="40"),
                 "lb:300-302 rb:320-322 rb:330-332 rb:340-342", 360, 3,
                 "motebox-g-pw-curse.png"),
    "pw_wrath": (dict(SEED="7", YEARS="40"),
                 "lb:300-302 rb:320-322 rb:330-332 rb:340-342 rb:350-352", 370, 3,
                 "motebox-g-pw-wrath.png"),
    "pw_beast": (dict(SEED="7", YEARS="40"),
                 "lb:300-302 rb:320-322 rb:330-332 rb:340-342 rb:350-352 rb:360-362", 380, 3,
                 "motebox-g-pw-beasts.png"),
    # AND FOUR OF THEM DOING SOMETHING. A menu of forty-eight icons says nothing about what
    # any of them is like; MOTEBOX_CAST fires one at a named cell so the guide can show it.
    "cast_volcano": (dict(SEED="5", YEARS="200", MORTAL="1", CAM="47,59",
                          CAST="VOLCANO@50,59", SANDBOX="1"), "", 90, 3,
                     "motebox-g-cast-volcano.png"),
    "cast_meteor":  (dict(SEED="3", YEARS="200", MORTAL="1", CAM="115,69",
                          CAST="METEOR@116,70", SANDBOX="1"), "", 40, 3,
                     "motebox-g-cast-meteor.png"),
    "cast_dragon":  (dict(SEED="47514", YEARS="200", MORTAL="1", CAM="65,15",
                          CAST="DRAGON@68,15", SANDBOX="1"), "", 120, 3,
                     "motebox-g-cast-dragon.png"),
    # RAIN was the fourth of these and it does not photograph: it douses fires and waters
    # crops, which is a change of state rather than a picture. Lightning leaves a bolt and a
    # scorch mark.
    "cast_light":   (dict(SEED="5", YEARS="200", MORTAL="1", CAM="47,59",
                          CAST="LIGHTNING@49,58", SANDBOX="1"), "", 26, 3,
                     "motebox-g-cast-lightning.png"),
    "nuke":     (dict(SEED="5", YEARS="150", NUKE="1", MORTAL="1"), "", 520, 3,
                 "motebox-g-nuke.png"),
    # A SHIP AT SEA. MOTEBOX_SAIL sends one, because a crossing lasts a few dozen ticks out of
    # a town's eight-year settling cycle and a blind camera photographs empty water.
    "ship":     (dict(SEED="23757", YEARS="200", MORTAL="1", SAIL="1"), "", 70, 3,
                 "motebox-g-ship.png"),
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
