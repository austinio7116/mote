# Pocket bench

Dial the ThumbyCue pocket shapes and watch them change, live.

    tools/pocketbench/pocketbench.py      # then open http://127.0.0.1:8765

Every image is drawn by `cue_render_build_table()` itself, straight down from
above — so what you are adjusting is the game's own mesh, not a picture of it.
The stub is recompiled from `games/thumbycue/src` each time the server starts,
so a change to the renderer or the table shows up on the next run.

## What the circles mean

| | |
|---|---|
| red | the drop. The ball's centre inside it and the ball is down — since the jaw-tip line came out, the only thing that decides a pot. |
| white | a ball sitting exactly on the drop, on the mouth's centre line |
| cyan | the lip's outer edge: where flat cloth stops and the roll begins |
| dashed | the bottom of the roll, where the cloth turns vertical |

`edge − drop` under each picture is the gap between the two: how far the ball
travels past the edge of the drawn cloth before it is taken. Zero is the two
agreeing.

## The sliders are the game's own fields

Nothing here is a bench-only invention. Each slider is a field in the source,
in the units the source writes it in, so a number dialled here goes back as
itself. **Amber sliders change how the table plays**; the rest only move the
cut and the lip.

| slider | what it does | field | units |
|---|---|---|---|
| **mouth width** | how far apart the knuckles sit — the opening the ball goes through. **The cushions move with it.** | `t->gap_corner` / `t->gap_side` | ×R |
| **hole size** | the size of the hole itself: the drawn bore, and what the drop and the lip are measured from | `t->pr_corner` / `t->pr_side` | ×R |
| **pocket depth** | how far the pocket centre sits back beyond the cushion line | `t->off_corner` / `t->off_side` | ×R |
| **drop inset** | how far inside the hole the ball must get before it is down. **Bigger means a smaller drop.** | the margin taken off `pr` in `build_world` (`0.30f`, or `side_m` at a middle) | ×R |
| **cut setback** | slides the whole cut in and out | `CueCut.set` | m |
| **lip outer edge** | where flat cloth stops and the roll begins | `CueCut.rad` | × pr |
| **lip thickness** | how wide the roll is, from that edge inwards and down | `CueCut.roll` | × pr |

Each one moves exactly one thing. **mouth width** and **hole size** used to be
the same knob: on the rounded tables both knuckle gaps were derived from
`pr_corner`, so one field placed every jaw on the table and a middle pocket
could not be sized at all. **drop inset** and **lip outer edge** were also
locked together, because `CueCut.rad` was a multiple of the drop rather than of
the hole, so shrinking one shrank the other and the pocket only ever looked
self-similar.

The millimetres in the readout are derived and read-only — the mouth the ball
actually goes through is the knuckle centres less both knuckle radii.

## Tables

Five blocks, because two pairs share a bed: snooker 12ft, snooker 10ft,
UK8 7ft (with 6-red), US8 9ft (with 9-ball), Chinese 8 10ft.

## Saving

**save** writes `pockets.json` beside the script and prints the source lines to
paste back. The session is reloaded next time the server starts, so tuning can
be picked up where it was left.
