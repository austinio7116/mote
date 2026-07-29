"""Motebox — the terrain palette: the artist's hues, with room between them.

The CC0 master is drawn in PICO-8's sixteen, and for a long time this project treated
that as a hard limit — "there is no olive", "PICO-8 has only two blues", "a shadow only
reads as shading if it is a step and not a drop". None of that was true. It was a
DECISION, made because the sprites are his art, and then reasoned from as if it were a
wall. The screen is RGB565 and every 4bpp tileset carries sixteen palette entries; the
baker already emits sixteen-colour palettes for the town and monster sheets while the
terrain sheets were using two to four.

So the rule is his hue FAMILIES, not his exact swatches: tones are interpolated between
the colours he used, which keeps generated ground sitting under hand-drawn creatures
without pretending three steps is all a gradient can have.
"""

BLACK   = (0, 0, 0)
NAVY    = (29, 43, 83)
PLUM    = (126, 37, 83)
DKGREEN = (0, 135, 81)
BROWN   = (171, 82, 54)
DKGREY  = (95, 87, 79)
LTGREY  = (194, 195, 199)
WHITE   = (255, 241, 232)
RED     = (255, 0, 77)
ORANGE  = (255, 163, 0)
YELLOW  = (255, 236, 39)
GREEN   = (0, 228, 54)
BLUE    = (41, 173, 255)
SLATE   = (131, 118, 156)
PINK    = (255, 119, 168)
PEACH   = (255, 204, 170)


def mix(a, b, t):
    """A tone t of the way from a to b. The whole point of this module."""
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def ramp(body, toward, n=5):
    """`n` tones from `body` toward `toward`, body first.

    Five is the mark: three gives a bright lip against the field and reads as a rim,
    seven goes soft and loses its edge. Rendered all three side by side to pick it.
    """
    if n <= 1:
        return [body]
    return [mix(body, toward, i / float(n - 1) * 0.75) for i in range(n)]
