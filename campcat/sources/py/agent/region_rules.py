"""How a region sits on the control it names.

Drawn from the 80 redrawn samples in `training_data` (browse them with
`campcat/sources/py/view_samples.py`). Three facts decided the text below:

* 12 of the 21 shape changes were a wide diamond redrawn as a box. Every
  diamond a human kept was within 1.12 of square; every one replaced was at
  least 1.86 times wider than tall. The model was naming the shape it meant,
  not the shape of the control.
* A redrawn region was a median 0.69 of the proposal's area: the proposal
  carries margin the control does not have.
* The proposals that stayed boxes were flush at the top and spilled on the
  right (median 17px) and left (8px).

Both agents that propose a region, and the check that adjusts one, read this
text, so they cannot disagree about what a good region is.
"""

REGION_RULES = (
    "What decides whether a region is right, in this order: the text's own "
    "boundary when the control is text, then its shape, then its size against "
    "the control. Everything else is a detail of those. Text comes first: "
    "crop the glyphs, not the thing they sit on. Otherwise shape first -- a "
    "right-shaped region a few pixels off still matches; a wrong-shaped one "
    "cannot, however tight its box is.\n"
    "  * Text boundary first, when the control is text. A label, a menu row or "
    "a button caption stands for its own ink: the region's edges sit on the "
    "first and the last stroke of it, and it is a box around exactly those "
    "pixels. The button, the pill or the panel the text sits on is not part of "
    "the control, and neither is a margin for legibility -- the matcher "
    "compares pixels, not meaning.\n"
    "    Right: the caption \"开始游戏\" gives a box whose four edges touch the "
    "glyphs.\n"
    "    Wrong: the whole red button around that caption. Its fill and border "
    "change with state -- hover, disabled, a different theme -- while the "
    "glyphs do not.\n"
    "  * Shape follows the control's outline, not what it means. Ask which "
    "outline the control actually has, and pick the one shape that matches: "
    "four straight sides -> box; an unbroken round rim -> circle; a square "
    "standing on one corner -> diamond.\n"
    "    Right: a rotated square gets a diamond -- four vertices on its four "
    "corners, width and height within a third of each other.\n"
    "    Right: a round control gets a circle, and a rectangle, a pill, a bar "
    "or a label gets a box.\n"
    "    Wrong: a 240x60 horizontal bar written as a diamond. Its corners are "
    "the bar's own ends, so the diamond throws them away. Long and flat is a "
    "box.\n"
    "    Wrong: a round control kept as a box, or a small icon with straight "
    "sides kept as a circle -- the shape writes an alpha mask, so the wrong "
    "one drops the corners the control does have, or keeps the background it "
    "does not.\n"
    "  * Size is the control's own extent: the region covers all of it and "
    "nothing but it. Every edge sits on the control's own pixels. Do not "
    "carry over the margin a first guess had: a redrawn region is typically "
    "0.7 of the area of the proposal it corrects, and the giving-away edge "
    "is the right or the bottom one.\n"
    "    Right: each edge lands on the control's own border, with no band of "
    "background between them.\n"
    "    Wrong: an edge floating outside the control so the region is nicer "
    "to look at. The matcher scores that background too.\n"
    "  * Neither is negotiable, and they are not traded against each other.\n"
    "    Never cut into the control: a pixel of background on one side is "
    "cheaper than a shaved corner.\n"
    "    Right: when the edge is between two pixels, take the one that keeps "
    "the control whole.\n"
    "    Wrong: shrinking until the control's outer ring is gone, so the "
    "template matches only the part that never moves.\n"
)
