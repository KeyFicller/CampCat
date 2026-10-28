"""The conversation's own state: its messages, its memory, its frames.

`graph.py` runs the flow; everything that outlives one round lives here. The
module holds a single conversation, as the sidecar serves one at a time.
"""

from __future__ import annotations

import base64
import io
import json
import os
import re
import struct
import sys
import time
from uuid import uuid4

from langchain_core.messages import HumanMessage, SystemMessage

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

# Frames asked for again with `view_shot`, which are a class of their own. They
# used to compete with the live screen for the `_KEEP_PICTURES` slots, so the
# first picture of every batch was dropped before the model could see it, the
# caption said to call `view_shot(n)` again, and the model did -- the same frame
# seven times, because asking for four at once guarantees the oldest is lost.
_KEEP_LOOKBACKS = 6

CROP_TOOLS = frozenset({"view_crop", "view_template"})

# Marks a looked-back frame, the way `crop:` marks a small cut. Not a screenshot:
# it is older than the screen and must never take the screen's place.
_LOOKBACK = "look:"

# The longest side any context frame is worth, whatever the screen it came from.
# A request carries every picture still in the list and the provider answers 413
# past its gateway's limit (~48 MiB, ~8 game screenshots), so this is what bounds
# a request's size. It is also what says a frame has already been through here:
# demotion runs over the whole list every round, and nothing below is longer.
_THUMB_PX = 960

# What a frame's caption claims about its own pixels. Declared once because the
# shrinker has to take it back out again: a picture that is scaled cannot also
# be at original size, and a message saying both is one the model argues with.
_ORIGINAL_SIZE = "就是原尺寸；坐标直接用这张图的像素"

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
# What `_detach_picture` writes where the picture was. Used to tell a frame's
# own message from prose that happens to name a frame number.
_DETACHED = "[图片未附上"
# Appended to the text of every request a model gets -- the user's own words and
# the brief after a handoff -- so its reasoning stays Chinese. It rides on the
# message, not the system prompt, and it is committed with the turn: the next
# request's prefix then matches what the provider already cached.
ASK_IN_CHINESE = "\n\n用中文思考和回答。"

# Ends the system prompt, the last thing read before the model starts
# reasoning; ASK_IN_CHINESE on its own only nudges. Naming the two habits that
# actually show up -- an English preamble and `Hmm`/`Wait` hedging -- is what
# holds: on DeepSeek's region check it took Chinese thinking from 0/6 to 5/6,
# and the scripter from 1/6 to 4/6. Allowing English identifiers here would
# undo that, so they are not mentioned.
THINK_IN_CHINESE = ("\n\n系统：你的思考过程（reasoning）必须全程使用简体中文，"
                    "禁止英文和 Hmm/Wait。最终答复也用简体中文。")


def think_in_chinese(messages: list) -> list:
    """The messages with THINK_IN_CHINESE on the system prompt.

    Every model call is routed through here rather than each prompt module, so
    an agent added later cannot forget it. A new list comes back: the caller's
    is the conversation, which gets committed and must not keep the note.
    """
    if not messages or not isinstance(messages[0], SystemMessage):
        return messages
    system = SystemMessage(content=(messages[0].content or "") + THINK_IN_CHINESE)
    return [system, *messages[1:]]


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

# Status-bar counters for this conversation. Reset with it. `_CONTEXT` stays
# None until a conversation response reports input_tokens; a missing report
# must not look like zero.
_LLM_CALLS = 0
_TOOL_CALLS = 0
_CONTEXT: int | None = None

# The scripter's own conversation. Kept out of `_MESSAGES` so a handoff's
# dozens of tool rounds never enter the operator's prompt or its cache prefix.
_SCRIPTER_MESSAGES: list = []
_SCRIPTER_TURN_MARK = "scripter-turn"
# How many of _SCRIPTER_MESSAGES are already folded into the scripter notes.
_SCRIPTER_MEMORIZED_UPTO = 0
# Pitfalls the host sent with this turn. Empty until a turn sets them.
_SCRIPTER_MEMORY = ""
# What the handoffs in this conversation taught, newest first. Never read off
# disk. Capped because it is prepended to every scripter prompt: a note that
# outlives its lesson costs tokens on every round of every later handoff.
LEDGER_MAX_CHARS = 1500
_LESSONS: list[str] = []


def set_scripter_memory(text: str) -> None:
    """Remember the scripter notes the host sent with this turn."""
    global _SCRIPTER_MEMORY
    _SCRIPTER_MEMORY = text


def scripter_memory() -> str:
    """The scripter notes for the turn now being served."""
    return _SCRIPTER_MEMORY


def add_lessons(body: str) -> None:
    """What this handoff taught, for every later one in this conversation.

    Newest first. Over the cap the oldest note goes whole rather than losing
    its tail: half a lesson reads as an instruction.
    """
    global _LESSONS
    text = body.strip()
    if not text:
        return
    _LESSONS.insert(0, text)
    while len(_LESSONS) > 1 and sum(map(len, _LESSONS)) > LEDGER_MAX_CHARS:
        _LESSONS.pop()
    if len(_LESSONS[0]) > LEDGER_MAX_CHARS:
        _LESSONS[0] = _LESSONS[0][-LEDGER_MAX_CHARS:]


def lessons() -> list[str]:
    """The notes the handoffs left, newest first."""
    return list(_LESSONS)


def _review_turn() -> None:
    import agent.region_review as region_review
    region_review.begin_turn()


def reset() -> None:
    """Drop every turn. The next request starts a fresh conversation."""
    global _LATEST_NUM, _LATEST_B64, _LATEST_TIME, _MEMORIZED_UPTO, _TURN_PREV_SHOT
    global _SCRIPTER_MEMORIZED_UPTO, _SCRIPTER_MEMORY, _LESSONS
    global _TURN_SEQ, _LLM_CALLS, _TOOL_CALLS, _CONTEXT
    import agent.memorier as memorier
    _review_turn()
    _MESSAGES.clear()
    _SCRIPTER_MESSAGES.clear()
    _LATEST_NUM = 0
    _LATEST_B64 = ""
    _LATEST_TIME = 0.0
    _TURN_PREV_SHOT = None
    memorier._MW = None
    memorier._SCRIPTER_MW = None
    _MEMORIZED_UPTO = 0
    _SCRIPTER_MEMORIZED_UPTO = 0
    _SCRIPTER_MEMORY = ""
    _LESSONS.clear()
    _TURN_SEQ = 0
    _LLM_CALLS = 0
    _TOOL_CALLS = 0
    _CONTEXT = None


def note_llm(response: object | None, *, context: bool) -> None:
    """Count one chat completion. `context` updates the bar's token length.

    A missing response is not a call. `input_tokens` is the provider's count;
    `total_tokens` includes the completion and is not a stand-in. `bool` is an
    `int` in Python, and `True` is not a token count.
    """
    global _LLM_CALLS, _CONTEXT
    if response is None:
        return
    _LLM_CALLS += 1
    if not context:
        return
    meta = getattr(response, "usage_metadata", None)
    if not isinstance(meta, dict):
        return
    n = meta.get("input_tokens")
    if isinstance(n, int) and not isinstance(n, bool):
        _CONTEXT = n


def note_tool() -> None:
    """Count one tool the sidecar asked the host to run."""
    global _TOOL_CALLS
    _TOOL_CALLS += 1


def usage_fields() -> dict:
    """The counters a `result` or `tool_call` should carry.

    `context` is omitted until a provider has reported it, so a host that
    treats a missing field as "leave the display alone" does not zero it.
    """
    fields = {"llm_calls": _LLM_CALLS, "tool_calls": _TOOL_CALLS}
    if _CONTEXT is not None:
        fields["context"] = _CONTEXT
    return fields


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
    _review_turn()


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


def new_mark(prefix: str) -> str:
    """A unique id for the message that starts a turn."""
    global _TURN_SEQ
    _TURN_SEQ += 1
    return f"{prefix}:{_TURN_SEQ}"


def own_since(messages: list, mark: str | None) -> list:
    """The messages after the one carrying `mark`.

    Found by id, not by an index saved at build time: `demote_pictures` drops
    stale frames from the history as a turn goes on, so that index points past
    the end of a list which has since shrunk. One run sliced an assistant turn
    out and kept its tool results, and the provider then rejected every later
    request with "Messages with role 'tool' must be a response to a preceding
    message with 'tool_calls'".
    """
    if not mark:
        return list(messages)
    for i, m in enumerate(messages):
        if getattr(m, "id", None) == mark:
            return list(messages[i + 1:])
    return list(messages)


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
        _shot_caption(_LATEST_NUM, "你最近拍的一张截图：",
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
        _shot_caption(shot, "截图", image, _LATEST_TIME, since), image))


def recall_frame(shot: object, image: str) -> HumanMessage:
    """A look back at an older frame, as its own user turn.

    It must not become the current screen -- tap boxes are validated against
    that one, and a stale frame would move them without saying so. No capture
    time either: the host keeps frames without one. It is marked as a look-back
    so `demote_pictures` will not let it take the live screen's place.

    Its caption states the size the same way a screenshot does, `_ORIGINAL_SIZE`
    included, because `_shrink_picture` swaps that phrase for the multiplier: a
    picture that is scaled must not also claim to be at original size.
    """
    size = png_size(image)
    dims = f"（{size[0]}x{size[1]} 像素，{_ORIGINAL_SIZE}）" if size else ""
    message = HumanMessage(content=_shot_content(
        f"#{shot} 号截图再看一次{dims}；这是早先的一张"
        f"画面，不是当前画面。", image))
    message.id = _LOOKBACK + uuid4().hex
    return message


def picture_of(name: str, image: str, args: dict | None = None) -> HumanMessage:
    """A marked-up copy some other tool returned, for the user to look at.

    Not stored as a frame: its box and label would be baked into any template
    cut from it. `args` is the call that produced it, so a picture later dropped
    can be asked for again by repeating that call. A small crop or template is
    marked, which is how `demote_pictures` counts it apart from screenshots.
    """
    if args is None:
        caption = f"工具图：{name}。"
    else:
        caption = (
            f"工具图：{name} "
            f"{json.dumps(args, ensure_ascii=False, separators=(',', ':'))}。"
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
    parts = [f"#{shot}" + (f"（{size[0]}x{size[1]} 像素，{_ORIGINAL_SIZE}）"
                           if size else "")]
    if captured:
        stamp = time.strftime("%H:%M:%S", time.localtime(captured))
        parts.append(f"拍于 {stamp}.{int(captured * 1000) % 1000:03d}")
    if since_previous is not None:
        parts.append(f"距上一张截图 +{since_previous:.1f}s")
    return f"{lead} {', '.join(parts)}。"


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
        note = ("[图片未附上；要再看一次就调 view_shot(" + found[1]
                + ")]")
    elif caption.startswith("工具图："):
        note = "[图片未附上；把同一个调用再发一次就能看到]"
    else:
        note = "[图片未附上]"
    return HumanMessage(
        content=[p for p in m.content if p.get("type") != "image_url"]
        + [{"type": "text", "text": note}],
        id=getattr(m, "id", None),
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
    # The multiplier is what saves the arithmetic: reading a box off a picture
    # and rescaling it by hand was a large block of thinking in a smoke run. It
    # replaces the "original size" claim rather than joining it -- one message
    # saying both is a contradiction the model stops to argue with.
    shown = (f"按 {w}x{h} 显示，读到的数 ×{size[0] / w:.2f}；"
             f"坐标仍是帧像素 {size[0]}x{size[1]}；"
             f"要从这一帧裁模板先调 view_shot({found[1]})")
    content = []
    for p in m.content:
        if p.get("type") == "image_url":
            continue
        text = p.get("text") or ""
        content.append({"type": "text", "text": text.replace(_ORIGINAL_SIZE, shown)})
    content.append({"type": "image_url", "image_url": {"url": url}})
    # The id says which class this message is in, and demotion runs every round:
    # a shrunk look-back that came back unmarked would be read as a screenshot
    # from then on and evict the live screen.
    return HumanMessage(content=content, id=getattr(m, "id", None))


def _is_crop(m: object) -> bool:
    """A small crop or template, counted apart from screenshots."""
    return _is_picture(m) and isinstance(getattr(m, "id", None), str) \
        and m.id.startswith("crop:")


def _is_lookback(m: object) -> bool:
    """A frame asked for again with `view_shot`, counted apart from the screen."""
    return _is_picture(m) and isinstance(getattr(m, "id", None), str) \
        and m.id.startswith(_LOOKBACK)


def _frame_copy(m: object) -> str | None:
    """The frame number, for a message whose one job is to carry that frame.

    A picture of the frame, or the same message after its picture was detached
    (it keeps the note saying so). An assistant turn or a tool result that
    happens to mention "#15" is not a copy of frame 15, and a crop carries its
    frame's number without being a duplicate of it.
    """
    if not isinstance(m, HumanMessage):
        return None
    ident = getattr(m, "id", None)
    if isinstance(ident, str) and ident.startswith("crop:"):
        return None
    if not (_is_picture(m) or _DETACHED in content_text(m.content)):
        return None
    found = _SHOT_NUM.search(content_text(m.content))
    return None if found is None else found[1]


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
    size: a tap's box is measured on it, and proposals read off a 540x960
    thumbnail were corrected 72% of the time against 40% off a full frame. The
    next few become thumbnails, and each one's caption states its own
    multiplier -- one message claiming both sizes at once is what the model
    stops to argue with. The rest lose their image and keep their caption, which
    already names the number `view_shot` wants back.

    Crops stay whole until more than `_CROP_HIGH` of them still carry their
    picture, and then only the newest `keep_crops` do. Clearing them one at a
    time would rewrite an earlier message on every call and drop the provider's
    prefix cache from there on.

    Looked-back frames are their own class with their own window. Sharing the
    screenshot window meant a batch of four `view_shot`s lost its first picture
    every time, and the caption then sent the model right back to ask for it.

    `keep` and `keep_crops` default to the windows this module holds, resolved
    at the call rather than bound as a default argument, so the constants stay
    the knobs.
    """
    keep = _KEEP_PICTURES if keep is None else keep
    keep_crops = _KEEP_CROPS if keep_crops is None else keep_crops
    frames = [i for i, m in enumerate(messages)
              if _is_picture(m) and not _is_crop(m) and not _is_lookback(m)]
    crops = [i for i, m in enumerate(messages) if _is_crop(m)]
    looks = [i for i, m in enumerate(messages) if _is_lookback(m)]
    for n, i in enumerate(reversed(frames)):
        if n >= keep:
            messages[i] = _detach_picture(messages[i])
        elif n >= 1:
            messages[i] = _shrink_picture(messages[i])
    if len(crops) > _CROP_HIGH:
        for n, i in enumerate(reversed(crops)):
            if n >= keep_crops:
                messages[i] = _detach_picture(messages[i])
    for n, i in enumerate(reversed(looks)):
        if n >= _KEEP_LOOKBACKS:
            messages[i] = _detach_picture(messages[i])
        else:
            messages[i] = _shrink_picture(messages[i])
    # After the sizes are settled, so "already without its picture" is the
    # state the model will read.
    messages = _without_stale_lookbacks(messages)
    _trace_pictures(messages)
    return messages


def _without_stale_lookbacks(messages: list) -> list:
    """Only the newest message per frame number, among the ones without a picture.

    A second `view_shot(N)` appends another copy while the first, by then
    detached, still reads "[图片未附上；要再看一次就调 view_shot(N)]" -- so the
    model took its own earlier copy for a failed call and asked again: 42 calls
    for 10 frames in one smoke run, and a 47-message picture list in a round.

    The screenshot of that frame is such a copy too, once its picture is gone,
    so the newest per number wins across both classes then. A screenshot that
    still carries its picture is kept: it is the frame a tap's box is measured
    on, and only it stays at full size.

    Only a frame's own message is ever dropped. An assistant turn that mentions
    "#15" in passing and carries `tool_calls` is not a copy of frame 15, and
    dropping it orphans its tool result -- the provider then rejects the whole
    request. Crops are left alone too: several crops of one frame all carry its
    number and none of them is a duplicate of the frame.
    """
    newest: dict[str, object] = {}
    for m in messages:
        num = _frame_copy(m)
        if num:
            newest[num] = m
    kept = {id(m) for m in newest.values()}
    messages[:] = [
        m for m in messages
        if id(m) in kept
        or _frame_copy(m) is None
        or (_is_picture(m) and not _is_lookback(m))
    ]
    return messages


def _trace_pictures(messages: list) -> None:
    """One line per round: which pictures still carry bytes, and which only a caption.

    A detached frame and an attached one read the same in the log otherwise, so a
    model asking `view_shot(N)` again cannot be told from one that was never sent
    the picture -- `*` marks the ones it can actually see.
    """
    if not os.environ.get("CAMPCAT_LLM_TRACE"):
        return
    seen = []
    for m in messages:
        found = _SHOT_NUM.search(content_text(m.content))
        if found:
            seen.append("#" + found[1] + ("*" if _is_picture(m) else ""))
    print("[pics] " + (", ".join(seen) or "(none)"), file=sys.stderr, flush=True)


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


def reasoning_text(chunk: object) -> str:
    """The reasoning a streamed chunk carries, or "" when it has none.

    DeepSeek and Qwen both name the field `reasoning_content`; the difference
    is that `ChatOpenAI` drops it and `ChatDeepSeek` keeps it.
    """
    extra = getattr(chunk, "additional_kwargs", None) or {}
    return str(extra.get("reasoning_content") or "")


def strip_tool_markup(text: str) -> str:
    """Drop leaked tool-call markup, keeping whatever answer surrounds it."""
    cleaned = _DSML_TAG.sub("", _DSML_BLOCK.sub("", text))
    # A removed block leaves its own blank lines behind; one blank line is the
    # most an answer should show where the markup used to be.
    return re.sub(r"\n{3,}", "\n\n", cleaned).strip()
