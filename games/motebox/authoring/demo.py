#!/usr/bin/env python3
"""Record a demo reel of the game, headlessly and reproducibly.

Each scene is one headless run with the host's frame recorder (MOTE_REC dumps every
frame as a PPM and quits after MOTE_REC_N of them). MOTE_DT_MS pins the timestep so a
clip is the same length every time and plays back at real speed. The frames are scaled
up with nearest-neighbour, captioned, and stitched with ffmpeg.

    python3 authoring/demo.py                # -> /tmp/motebox_demo.mp4
    python3 authoring/demo.py --gif          # + a smaller gif

Scenes come from the same test hooks the screenshots use, because the interesting
things — a battle, a crossing, the bomb — are rare enough that a camera pointed at a
running world records an empty field.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
HOST = os.path.join(ROOT, "build_host", "mote_host")
MODULE = os.path.join(GAME, "build", "Motebox.so")
WORK = "/tmp/motebox_demo_frames"
OUT = "/tmp/motebox_demo.mp4"

FPS = 30
# THE REEL IS SHOT IN THE CHASSIS. The frames are composited into the screen of Studio's
# product photo, at the same rectangle Studio uses (studio/assets/screen.cfg, calibrated by
# `mote studio calibrate`), so the clip looks like the handheld running rather than like a
# window. --plain gives the older captioned version instead.
CHASSIS = "studio/assets/thumby_color.png"
SCREEN_CFG = "studio/assets/screen.cfg"
OUT_W = 1280                  # the chassis, scaled down; the screen lands at about 396 px
BACKDROP = (11, 13, 20)       # the product photo is cut out; this is behind it
CAPTION_H = 34
ZOOM = 4                      # --plain only: 128 -> 512

# name, caption, env, keys, frames, skip (frames recorded then dropped from the front)
SCENES = [
    ("world", "A world already running: four peoples, their borders, three centuries in",
     dict(SEED="5", YEARS="250", NOFOLLOW="1"), "", 150, 0),
    ("town", "Close up, every villager is drawn: farmers, soldiers, an elder, a lord",
     dict(SEED="3", YEARS="220", MORTAL="1", CAM="v", NOFOLLOW="1"), "", 210, 0),
    ("farm", "Fields are sown and cut one cell at a time, by hand",
     dict(SEED="9", YEARS="160", MORTAL="1", CAM="f", NOFOLLOW="1"), "", 180, 0),
    # CAM=s keeps the camera on whatever is at sea, and the first ninety frames are dropped:
    # the traveller walks through the town to the quay first, which is honest but is not the
    # shot.
    ("ship", "A dock, and a crossing: colonists and expeditions go by sea",
     dict(SEED="23757", YEARS="200", MORTAL="1", SAIL="1", CAM="s", NOFOLLOW="1"), "", 300, 90),
    ("war", "Kingdoms fight over iron, and take each other's towns",
     dict(SEED="7919", YEARS="260", MORTAL="1", CAM="v", BATTLE="1", NOFOLLOW="1"), "", 210, 0),
    ("powers", "Forty-eight powers across six pages",
     dict(SEED="7", YEARS="60", NOFOLLOW="1"),
     "lb:20-22 rb:70-72 rb:120-122 rb:170-172 rb:220-222", 260, 0),
    ("volcano", "A volcano leaves a mountain where it erupted",
     dict(SEED="5", YEARS="200", MORTAL="1", CAM="47,59", CAST="VOLCANO@50,59",
          SANDBOX="1", NOFOLLOW="1"), "", 210, 0),
    ("nuke", "And a kingdom that finishes the tech tree can build the bomb",
     dict(SEED="5", YEARS="150", NUKE="1", MORTAL="1", NOFOLLOW="1"), "", 330, 0),
]


def record(name, env_extra, keys, frames):
    d = os.path.join(WORK, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    env = dict(os.environ)
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", MOTE_AUTORUN="1",
               MOTE_REC=d, MOTE_REC_N=str(frames), MOTE_DT_MS=str(1000.0 / FPS))
    if keys:
        env["MOTE_KEYS"] = keys
    for k, v in env_extra.items():
        env["MOTEBOX_" + k] = v
    subprocess.run([HOST, MODULE], env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=900)
    got = sorted(f for f in os.listdir(d) if f.endswith(".ppm"))
    print("  %-8s %d frames" % (name, len(got)))
    return d, got


def chassis_plate():
    """The product photo at OUT_W, and where the screen sits in it."""
    from PIL import Image
    shot = Image.open(os.path.join(ROOT, CHASSIS)).convert("RGBA")
    im = Image.new("RGB", shot.size, BACKDROP)      # the photo is cut out, so give it a room
    im.paste(shot, (0, 0), shot)
    sx, sy, ss = 1011.2, 319.6, 888.8
    try:
        with open(os.path.join(ROOT, SCREEN_CFG)) as f:
            sx, sy, ss = (float(v) for v in f.read().split()[:3])
    except IOError:
        pass                                     # the defaults compiled into studio/main.c
    k = OUT_W / float(im.width)
    h = int(round(im.height * k))
    h -= h & 1                                   # even, or yuv420p will not have it
    plate = im.resize((OUT_W, h), Image.LANCZOS)
    return plate, (int(round(sx * k)), int(round(sy * k)), int(round(ss * k)))


def caption(im, text, draw_cls, font=None):
    from PIL import Image
    W = im.width
    out = Image.new("RGB", (W, im.height + CAPTION_H), (12, 14, 22))
    out.paste(im, (0, 0))
    d = draw_cls.Draw(out)
    w = d.textlength(text, font=font) if font else d.textlength(text)
    d.text(((W - w) / 2, im.height + (CAPTION_H - 14) / 2), text,
           fill=(214, 220, 240), font=font)
    return out


def main():
    from PIL import Image, ImageDraw, ImageFont
    font = None
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"):
        if os.path.exists(p):
            font = ImageFont.truetype(p, 15)
            break
    plain = "--plain" in sys.argv
    plate = screen = None
    if not plain:
        plate, screen = chassis_plate()
        print("chassis %dx%d, screen %dx%d at %d,%d"
              % (plate.width, plate.height, screen[2], screen[2], screen[0], screen[1]))
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    seq = os.path.join(WORK, "seq")
    os.makedirs(seq)
    n = 0
    for name, cap, env, keys, frames, skip in SCENES:
        d, got = record(name, env, keys, frames)
        for f in got[skip:]:
            im = Image.open(os.path.join(d, f)).convert("RGB")
            if plain:
                im = im.resize((im.width * ZOOM, im.height * ZOOM), Image.NEAREST)
                out = caption(im, cap, ImageDraw, font)
            else:
                out = plate.copy()
                sx, sy, ss = screen
                out.paste(im.resize((ss, ss), Image.NEAREST), (sx, sy))
            out.save(os.path.join(seq, "%05d.png" % n))
            n += 1
        shutil.rmtree(d, ignore_errors=True)
    print("stitching %d frames (%.1f s)" % (n, n / float(FPS)))
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS),
                    "-i", os.path.join(seq, "%05d.png"),
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", OUT],
                   check=True)
    print("wrote", OUT)
    if "--gif" in sys.argv:
        gif = OUT.replace(".mp4", ".gif")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", OUT,
                        "-vf", "fps=15,scale=384:-1:flags=neighbor", gif], check=True)
        print("wrote", gif)


if __name__ == "__main__":
    main()
