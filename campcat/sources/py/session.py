"""The conversation's own state: its messages, its memory, its frames.

`graph.py` runs the flow; everything that outlives one round lives here. The
module holds a single conversation, as the sidecar serves one at a time.
"""

from __future__ import annotations

import base64
import io
import json
import re
import struct
import time
from uuid import uuid4

from langchain_core.messages import HumanMessage

try:                       # optional: without it the older frames are dropped
    from PIL import Image  # instead of scaled, which is what this did before
except ImportError:
    Image = None

# Tools hand back PNG only, so the mime type is fixed here. Add a protocol field
# if a tool ever returns another format.
MIME = "image/png"

# Pictures kept as images in a request; older ones keep their caption and lose
# the bytes. A request carries every picture still in the list and the provider
# answers 413 past its gateway's limit (~48 MiB, ~8 game screenshots), so this is
# what bounds a request's size. Beyond the newest, a picture is kept only as
# context, so it is worth `_THUMB_PX` instead of the full frame.
_KEEP_PICTURES = 3

# Small pictures (a crop, a template) kept at full size. Cleared in a batch,
# not one at a time: rewriting an earlier message invalidates the provider's
# prefix cache from there on, so a new crop must not touch the ones before it
# until a whole batch is due.
_KEEP_CROPS = 4
_CROP_HIGH = 8

CROP_TOOLS = frozenset({"view_crop", "view_template"})

# The longest side any context frame is worth, whatever the screen it came from.
# A request carries every picture still in the list and the provider answers 413
# past its gateway's limit (~48 MiB, ~8 game screenshots), so this is what bounds
# a request's size. It is also what says a frame has already been through here:
# demotion runs over the whole list every round, and nothing below is longer.
_THUMB_PX = 960

# Share of its long side a context frame keeps. Relative, because screens differ:
# one absolute target shrank a 2560-wide frame to 37% while leaving a 1280-wide
# one at 75%, so the frames that were already cheap were the ones shrunk least.
# `_THUMB_PX` now only caps the largest. Measured: the picture is 94% of a
# request's cost and that cost is bytes, not pixels, so a fifth of the bytes buys
# most of the speedup (spec 2026-10-12, §2.9). Resizing costs 91 ms a frame
# locally, against the 1.35 s each full one costs on the wire -- so this is the
# sidecar's own business, and the host sends only the full frame.
_THUMB_SCALE = 0.4

# DeepSeek writes tool-call syntax into the text content when it wants a tool it
# was not offered. Nothing else contains the token "DSML", so tags carrying it
# are markup rather than answer: a whole block goes first, then any orphan half.
_DSML_BLOCK = re.compile(r"<[^>]*DSML[^>]*>.*?</[^>]*DSML[^>]*>",
                         re.IGNORECASE | re.DOTALL)
_DSML_TAG = re.compile(r"</?[^>]*DSML[^>]*>", re.IGNORECASE)

# The frame number a caption names, e.g. "#9" in "Screenshot #9 (2560x1440 ...)".
# `_shot_caption` fixes that shape; nothing else in a caption contains "#".
_SHOT_NUM = re.compile(r"#(\d+)")


# Only the NDJSON read loop touches this, so it needs no lock.
_MESSAGES: list = []

# How many of _MESSAGES are already folded into the memory notes. Consolidation
# runs on a timer through a session's life, so it has to skip what it has already
# seen: re-summarizing the whole conversation every time would cost more each
# round and let the notes drift on every pass.
_MEMORIZED_UPTO = 0

# The most recent screenshot, only so it can ride along as its own user turn in
# `latest_frame`. Kept out of _MESSAGES on purpose: a picture in the history is
# re-sent on every later request and eventually summarized away. Every frame
# stays addressable by its `shot` number, but the host keeps those; older frames
# are not re-sent, only the last one is.
_LATEST_NUM = 0
_LATEST_B64 = ""
# When those bytes were captured, as a wall clock. The ride-along frame carries
# it so a later request can tell how stale the screen it is shown is. Not a
# delta: across turns the gap would be the user's typing, not the device's.
_LATEST_TIME = 0.0
# Monotonic time of the previous screenshot in the turn now being served, or
# None before its first one. Reset per turn, because only inside one turn are
# two frames separated by device time instead of by the user thinking.
_TURN_PREV_SHOT = None


# The opening message of each turn carries this id prefix, so `size()` can count
# turns without guessing from the rounds in between, and a compaction that folds
# whole turns away lowers the count on its own. `Message.id` never goes to the
# provider, so the mark costs nothing on the wire.
_TURN_MARK = "turn"
_TURN_SEQ = 0

# The scripter's own conversation. Kept out of `_MESSAGES` so a handoff's
# dozens of tool rounds never enter the operator's prompt or its cache prefix.
_SCRIPTER_MESSAGES: list = []
_SCRIPTER_TURN_MARK = "scripter-turn"


def reset() -> None:
    """Drop every turn. The next request starts a fresh conversation."""
    global _LATEST_NUM, _LATEST_B64, _LATEST_TIME, _MEMORIZED_UPTO, _TURN_PREV_SHOT
    global _TURN_SEQ
    import agent.memorier as memorier
    _MESSAGES.clear()
    _SCRIPTER_MESSAGES.clear()
    _LATEST_NUM = 0
    _LATEST_B64 = ""
    _LATEST_TIME = 0.0
    _TURN_PREV_SHOT = None
    memorier._MW = None
    memorier._SCRIPTER_MW = None
    _MEMORIZED_UPTO = 0
    _TURN_SEQ = 0


def size() -> int:
    """Turns currently kept verbatim; a summary does not count as a turn."""
    return sum(1 for m in _MESSAGES
               if isinstance(m.id, str) and m.id.startswith(_TURN_MARK))


def history() -> list:
    """The conversation, for the request being assembled.

    A copy: the caller extends it into its own list, so nothing outside this
    module ends up holding the live one.
    """
    return list(_MESSAGES)


def start_turn() -> None:
    """Begin a turn, whose first screenshot has no previous one to measure from."""
    global _TURN_PREV_SHOT
    _TURN_PREV_SHOT = None


def append_turn(text: str, raw: list) -> None:
    """Add one turn to the conversation, as it really happened.

    `raw` is the turn's own messages: the model's calls and the host's own
    results. Keeping those lets a later turn read what actually came back,
    which a written-up line per call could not. Pictures lose their bytes --
    only the newest frame is re-sent, by `latest_frame` -- but keep their
    captions, and a caption is where the frame number lives.
    """
    global _TURN_SEQ
    _TURN_SEQ += 1
    _MESSAGES.append(HumanMessage(content=text, id=f"{_TURN_MARK}:{_TURN_SEQ}"))
    _MESSAGES.extend(_detach_picture(m) if _is_picture(m) else m for m in raw)


def scripter_history() -> list:
    """The scripter's conversation. A copy, for the same reason as `history`."""
    return list(_SCRIPTER_MESSAGES)


def append_scripter_turn(brief: str, raw: list) -> None:
    """One handoff, kept where the operator's history cannot see it.

    Same rule as `append_turn`: pictures lose their bytes and keep the caption
    that names the frame. The id prefix is not `turn`, so `size()` ignores it.
    """
    global _TURN_SEQ
    _TURN_SEQ += 1
    _SCRIPTER_MESSAGES.append(
        HumanMessage(content=brief, id=f"{_SCRIPTER_TURN_MARK}:{_TURN_SEQ}"))
    _SCRIPTER_MESSAGES.extend(
        _detach_picture(m) if _is_picture(m) else m for m in raw)



def latest_frame() -> HumanMessage | None:
    """The newest screenshot as its own user turn, or None before any.

    It opens the request after a turn full of taps, so the model still sees the
    screen it last looked at. Only the newest: older frames are not re-sent, and
    the prefix stays byte-identical for the provider's prompt cache.
    """
    if not _LATEST_NUM:
        return None
    return HumanMessage(content=_shot_content(
        _shot_caption(_LATEST_NUM, "The last screenshot you took:",
                      _LATEST_B64, _LATEST_TIME), _LATEST_B64))


def screenshot_frame(shot: object, image: str) -> HumanMessage | None:
    """Record a fresh capture as the current frame, and return its user turn.

    None when the host gave no usable number: a picture nobody can name later is
    worse than no picture, because a cut box would have no frame to be checked
    against.
    """
    global _LATEST_NUM, _LATEST_B64, _LATEST_TIME, _TURN_PREV_SHOT
    if not isinstance(shot, int) or isinstance(shot, bool) or shot <= 0:
        return None
    now = time.monotonic()
    since = None if _TURN_PREV_SHOT is None else now - _TURN_PREV_SHOT
    _TURN_PREV_SHOT = now
    _LATEST_NUM, _LATEST_B64 = shot, image
    _LATEST_TIME = time.time()
    return HumanMessage(content=_shot_content(
        _shot_caption(shot, "Screenshot", image, _LATEST_TIME, since), image))


def recall_frame(shot: object, image: str) -> HumanMessage:
    """A look back at an older frame, as its own user turn.

    It must not become the current screen -- tap boxes are validated against
    that one, and a stale frame would move them without saying so. No capture
    time either: the host keeps frames without one.
    """
    size = png_size(image)
    dims = f" ({size[0]}x{size[1]} pixels)" if size else ""
    return HumanMessage(content=_shot_content(
        f"Screenshot #{shot} again{dims}; this is an earlier "
        f"screen, not the current one.", image))


def picture_of(name: str, image: str, args: dict | None = None) -> HumanMessage:
    """A marked-up copy some other tool returned, for the user to look at.

    Not stored as a frame: its box and label would be baked into any template
    cut from it. `args` is the call that produced it, so a picture later dropped
    can be asked for again by repeating that call. A small crop or template is
    marked, which is how `demote_pictures` counts it apart from screenshots.
    """
    if args is None:
        caption = f"Image after {name}."
    else:
        caption = (
            f"Image after {name}"
            f"({json.dumps(args, ensure_ascii=False, separators=(',', ':'))})."
        )
    message = HumanMessage(content=_shot_content(caption, image))
    size = png_size(image)
    if name in CROP_TOOLS and size is not None and max(size) <= _THUMB_PX:
        message.id = f"crop:{uuid4().hex}"
    return message


def png_size(png_b64: str) -> tuple[int, int] | None:
    """Width and height of a base64 PNG, read from its fixed-position IHDR.

    The model names tap coordinates in these pixels, so it needs the exact size;
    the image alone does not state its own resolution.
    """
    # Signature (8) + chunk length (4) + "IHDR" (4) + width (4) + height (4) = 24
    # bytes, so the first 32 base64 characters already cover what we read.
    try:
        head = base64.b64decode(png_b64[:32])
    except ValueError:  # binascii.Error, for a payload that is not base64
        return None
    if len(head) < 24 or not head.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    return struct.unpack(">II", head[16:24])


def _shot_caption(shot: int, lead: str, image: str, captured: float = 0.0,
                  since_previous: float | None = None) -> str:
    """`lead` plus the number, size and time, which is what names this frame later.

    `save_template`'s `shot` is this number and its region is in these pixels, so
    both have to be on screen next to the picture. The time is what lets a wait
    be reasoned about instead of guessed: `since_previous` is how long passed
    between this frame and the one before it in the same turn.

    `image` is passed rather than read off `_LATEST_B64` because a look back at an
    old frame has to state that frame's size, which need not be the newest one's.
    """
    size = png_size(image)
    parts = [f"#{shot}" + (f" ({size[0]}x{size[1]} pixels)" if size else "")]
    if captured:
        stamp = time.strftime("%H:%M:%S", time.localtime(captured))
        parts.append(f"taken {stamp}.{int(captured * 1000) % 1000:03d}")
    if since_previous is not None:
        parts.append(f"+{since_previous:.1f}s since the previous screenshot")
    return f"{lead} {', '.join(parts)}."


def _shot_content(caption: str, image: str) -> list[dict]:
    """One screenshot as user-turn content, for the model to look at.

    A user turn, never a tool result: ChatDeepSeek json.dumps()es list content
    on a `tool` message, so a data URL there is billed as text. One 1080x2400
    screenshot cost 2.3M tokens that way, against ~1k here.
    """
    return [
        {"type": "text", "text": caption},
        {"type": "image_url",
         "image_url": {"url": f"data:{MIME};base64,{image}"}},
    ]


def _is_picture(m: object) -> bool:
    """True for an attached-picture message: user-turn content with an image."""
    if not isinstance(m, HumanMessage) or not isinstance(m.content, list):
        return False
    return any(isinstance(p, dict) and p.get("type") == "image_url"
               for p in m.content)


def _detach_picture(m: HumanMessage) -> HumanMessage:
    """The same content with the image dropped, plus one line on getting it back.

    The caption stays verbatim: its number, size and time-since-previous are what
    the model reasons about, and the base64 is the only part worth megabytes. The
    frame itself is still on the C++ side, so its number is enough to ask again.
    """
    caption = " ".join(p["text"] for p in m.content if p.get("type") == "text")
    found = _SHOT_NUM.search(caption)
    if found:
        note = ("[image not attached; call view_shot(" + found[1]
                + ") to look again]")
    elif caption.startswith("Image after "):
        note = "[image not attached; make the same call again to look at it]"
    else:
        note = "[image not attached]"
    return HumanMessage(
        content=[p for p in m.content if p.get("type") != "image_url"]
        + [{"type": "text", "text": note}]
    )


def _downscale(url: str, size: tuple[int, int]) -> tuple[str, int, int] | None:
    """A smaller copy of one `data:` PNG, as (url, width, height).

    None when there is no Pillow or the bytes are not a PNG it can read, which
    leaves the caller to drop the picture instead. Bilinear: these frames are
    read for which screen is on and roughly where things are, not edge detail.
    """
    if Image is None:
        return None
    try:
        im = Image.open(io.BytesIO(base64.b64decode(url.partition(",")[2])))
        im.load()
        long_side = min(_THUMB_PX, round(max(size) * _THUMB_SCALE))
        scale = long_side / max(size)
        small = im.resize((max(1, round(size[0] * scale)),
                           max(1, round(size[1] * scale))), Image.BILINEAR)
        buf = io.BytesIO()
        small.save(buf, "PNG", compress_level=6)
    except Exception:  # a frame the host sent and Pillow still cannot read
        return None
    return (f"data:{MIME};base64,{base64.b64encode(buf.getvalue()).decode()}",
            small.width, small.height)


def _shrink_picture(m: HumanMessage) -> HumanMessage:
    """The same frame at `_THUMB_SCALE` of its long side, capped at `_THUMB_PX`.

    The full picture is already in this message, so a smaller one costs only
    local CPU. The caption keeps the frame's real size -- that is the size a box
    for it has to be in -- and gains a line saying the picture is scaled and
    where the unscaled one is.
    """
    caption = " ".join(p["text"] for p in m.content if p.get("type") == "text")
    found = _SHOT_NUM.search(caption)
    part = next((p for p in m.content if p.get("type") == "image_url"), None)
    if found is None or part is None:
        return _detach_picture(m)      # `mark`'s picture carries no number
    size = png_size(part["image_url"]["url"].partition(",")[2])
    if size is None:
        return _detach_picture(m)
    if max(size) <= _THUMB_PX:
        return m                       # at or under the ceiling: already dealt with
    small = _downscale(part["image_url"]["url"], size)
    if small is None:
        return _detach_picture(m)
    url, w, h = small
    note = (f"[shown scaled to {w}x{h}; call view_shot({found[1]}) before "
            f"cutting a template from this frame]")
    return HumanMessage(content=[
        p for p in m.content if p.get("type") != "image_url"
    ] + [
        {"type": "text", "text": note},
        {"type": "image_url", "image_url": {"url": url}},
    ])


def _is_crop(m: object) -> bool:
    """A small crop or template, counted apart from screenshots."""
    return _is_picture(m) and isinstance(getattr(m, "id", None), str) \
        and m.id.startswith("crop:")


def thumbnail_png(b64: str) -> str | None:
    """`b64` scaled the way a context frame is, or None when that cannot be done.

    A picture already within `_THUMB_PX` comes back unchanged, and that needs no
    Pillow. Anything larger does, so a missing Pillow or a file it cannot read
    is None rather than the original: the caller asked for a thumbnail.
    """
    size = png_size(b64)
    if size is None:
        return None
    if max(size) <= _THUMB_PX:
        return b64
    small = _downscale(f"data:{MIME};base64,{b64}", size)
    if small is None:
        return None
    return small[0].partition(",")[2]


def crop_png(b64: str, box: tuple[int, int, int, int]) -> str | None:
    """The rectangle `(x, y, w, h)` of a base64 PNG, or None when it cannot be cut."""
    if Image is None:
        return None
    x, y, w, h = box
    try:
        im = Image.open(io.BytesIO(base64.b64decode(b64)))
        im.load()
        cut = im.crop((x, y, x + w, y + h))
        buf = io.BytesIO()
        cut.save(buf, "PNG")
    except Exception:  # a frame the host sent and Pillow still cannot read
        return None
    return base64.b64encode(buf.getvalue()).decode()


def demote_pictures(messages: list, keep: int | None = None,
                    keep_crops: int | None = None) -> list:
    """Price each picture by what the model does with it.

    Screenshots and small crops are counted apart, so looking at a crop does
    not shrink the screenshot it was cut from. The newest screenshot stays full
    size: a tap's box is measured on it and validated at that resolution. The
    next few become thumbnails. The rest lose their image and keep their
    caption, which already names the number `view_shot` wants back.

    Crops stay whole until more than `_CROP_HIGH` of them still carry their
    picture, and then only the newest `keep_crops` do. Clearing them one at a
    time would rewrite an earlier message on every call and drop the provider's
    prefix cache from there on.

    `keep` and `keep_crops` default to the windows this module holds, resolved
    at the call rather than bound as a default argument, so the constants stay
    the knobs.
    """
    keep = _KEEP_PICTURES if keep is None else keep
    keep_crops = _KEEP_CROPS if keep_crops is None else keep_crops
    frames = [i for i, m in enumerate(messages)
              if _is_picture(m) and not _is_crop(m)]
    crops = [i for i, m in enumerate(messages) if _is_crop(m)]
    for n, i in enumerate(reversed(frames)):
        if n >= keep:
            messages[i] = _detach_picture(messages[i])
        elif n >= 1:
            messages[i] = _shrink_picture(messages[i])
    if len(crops) > _CROP_HIGH:
        for n, i in enumerate(reversed(crops)):
            if n >= keep_crops:
                messages[i] = _detach_picture(messages[i])
    return messages


def content_text(content: object) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, str):
                parts.append(block)
            elif isinstance(block, dict) and block.get("type") == "text":
                parts.append(str(block.get("text", "")))
        return "".join(parts)
    return str(content)


def strip_tool_markup(text: str) -> str:
    """Drop leaked tool-call markup, keeping whatever answer surrounds it."""
    cleaned = _DSML_TAG.sub("", _DSML_BLOCK.sub("", text))
    # A removed block leaves its own blank lines behind; one blank line is the
    # most an answer should show where the markup used to be.
    return re.sub(r"\n{3,}", "\n\n", cleaned).strip()
