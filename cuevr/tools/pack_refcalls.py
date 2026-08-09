#!/usr/bin/env python3
"""Pack the referee's break calls into one file per voice.

153 numbers x 2 voices is about 12 MB of PCM. Baking that into a C header the
way the clacks are baked would put 12 MB of int16 literals through the compiler
and 12 MB in .rodata for audio that is only touched after a pot, so it ships as
an APK asset instead and only the chosen voice is ever resident.

One file per voice rather than 153, because opening 153 assets on a headset is
153 chances to stall a frame, and because the index makes "call number N" a
lookup rather than a filename.

    magic   "CUEREF01"      8 bytes
    count   uint32          how many calls, always 153
    rate    uint32          22050, checked at load rather than assumed
    index   count x (uint32 offset, uint32 samples)   offsets from the PCM start
    pcm     int16 mono, little endian

Call n is index n-1: there is no "zero" break to announce.

    ./pack_refcalls.py [source_dir] [out_dir]
"""
import os, struct, sys, wave

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "explainer", "refcalls")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(__file__), "..", "app", "src", "main", "assets")
RATE, COUNT = 22050, 153

def pack(sub, name):
    pcm, index = bytearray(), []
    for n in range(1, COUNT + 1):
        path = os.path.join(SRC, sub, "%03d.wav" % n)
        with wave.open(path) as w:
            if (w.getnchannels(), w.getframerate(), w.getsampwidth()) != (1, RATE, 2):
                raise SystemExit(
                    "%s is %dch %dHz %dbit — the mixer takes mono 22050 s16 and "
                    "resampling at runtime is exactly what these were generated "
                    "to avoid" % (path, w.getnchannels(), w.getframerate(),
                                  w.getsampwidth() * 8))
            frames = w.getnframes()
            index.append((len(pcm), frames))
            pcm += w.readframes(frames)
    hdr = b"CUEREF01" + struct.pack("<II", COUNT, RATE)
    hdr += b"".join(struct.pack("<II", o, n) for o, n in index)
    os.makedirs(OUT, exist_ok=True)
    dst = os.path.join(OUT, name)
    with open(dst, "wb") as f:
        f.write(hdr); f.write(pcm)
    print("%-18s %3d calls, %5.1f s, %5.2f MB -> %s"
          % (sub, COUNT, len(pcm) / 2 / RATE, len(hdr) + len(pcm) >> 20 or
             (len(hdr) + len(pcm)) / 1e6, dst))

pack("wav", "refcalls_m.bin")
pack("wav_female", "refcalls_f.bin")
