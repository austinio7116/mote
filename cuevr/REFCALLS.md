# Spoken break calls — WIRED IN

A referee's voice saying every break total from **1 to 153**, in two voices, both
of which are in the game. Chosen from REF VOICE on the main menu (OFF / MALE /
FEMALE) and called after every legal pot in snooker.

The three questions this note left open were answered as follows.

**Where the bytes live.** APK assets, one packed file per voice, built by
`cuevr/tools/pack_refcalls.py` into `cuevr/app/src/main/assets/`. Only the
chosen voice is resident (~6 MB). Rerun the packer if the wavs are
regenerated — the APK carries the packed files, not the wav directories.

**Voice-stealing.** Speech has its own mixer slot outside the pool of eight
(`cue_audio_speak`), so the clacks cannot reach it, at 0.55 gain against the
−1 dBFS the calls were normalised to.

**When it fires.** Every legal pot, snooker only, announcing the running break —
`resolve_shot` in `cuevr_app.c`, before the turn is routed, because `r->brk` is
about to be reset if the table changes hands.

## Where they are

```
../../explainer/refcalls/
  wav/001.wav … 153.wav          narrator — the ThumbyOne film's voice   6.3 MB
  wav_female/001.wav … 153.wav   female                                  5.9 MB
  master/, master_female/        24 kHz originals, for re-shaping without regenerating
  refcalls.json, refcalls_female.json    per-call duration, seed, attempts
  review_all.wav, review_all_female.wav  each set in order, for checking by ear
```

**Format is already CueVR's**: 22050 Hz mono s16, matching `RATE` in
`cuevr_audio.c`, so nothing is resampled and nothing is retuned. Peak-normalised
to −1 dBFS, silence trimmed off both ends so a call lands on the pot rather than
a fifth of a second after it. Narrator runs 0.40–1.59 s, female 0.41–1.45 s, so
the two are interchangeable at runtime and neither drags relative to the other.

Every clip was transcribed back with Whisper and had to return the number that
went in with nothing said after it, so the set does not need listening through
for correctness — only for taste.

## What is left to decide

**Where the bytes live.** 153 calls × 2 voices is ~12 MB of PCM. Baking that into
`cue_refcall_pcm.h` the way `cue_clack_pcm.h` is baked would put 12 MB in
`.rodata`, which is a lot to carry in the binary for audio that is only touched
after a pot. Loading them from APK assets and holding one voice's set in memory
is the obvious alternative. Both are fine; it is a build-size question, not an
audio one.

**Voice-stealing.** `play()` in `cuevr_audio.c` steals the quietest of 8 voices.
A break call is 1.5 s long and the clacks that follow it are loud, so on the
current rule a call will be cut off by the next shot. It probably wants its own
voice slot outside the pool, and its own gain — the calls are normalised to
−1 dBFS, which is louder than the sample gains the SFX use.

**When it fires.** The counter exists already: `best_break` and `best_tally` are
in the per-player state that `hud_break_row()` draws at `cuevr_app.c:1608`.
Whether the call comes on every pot or only at milestones is a design call, not
a technical one — 153 files means either works.

## Regenerating

Only needed if the delivery wants changing; the sets are complete.

```
cd ../../explainer/refcalls
./run_refcalls.sh                      # narrator
REFCALL_VOICE=female ./run_refcalls.sh # female
```

Resumable — delete the wavs you want redone and rerun. Needs
`/home/maustin/audiogen-venv` and `HF_HOME=/mnt/d/hf-cache` (the wrapper sets it).

**Do not re-tune the timing without reading the docstring in `gen_refcalls.py`.**
F5 truncates short lines: the generated length is
`ref_frames * gen_chars / ref_chars / speed`, which allots "One hundred." 0.62 s
— less than the phrase takes — and the word is simply cut off mid-syllable. It is
worked around with `fix_duration`, at the reference's own chars/sec plus 0.55 s.
More headroom is worse, not better: at +1.8 s F5 fills the spare room with a
second burst of invented speech. Lines under 10 bytes dodge the bug entirely via
a hidden `local_speed = 0.3` in `utils_infer.py`, which is why one-word numbers
can sound perfect while the multi-word ones are broken.

## Adding more calls

`refcall_text.py` turns a number into the words for it, British convention
("one hundred **and** forty seven"), and is where anything beyond bare numbers
would go — "foul", "free ball", century calls. `gen_refcalls.py` takes a range,
so extending is `./run_refcalls.sh 154 200` rather than a rerun.
