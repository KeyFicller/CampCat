"""The conversation's own state: its messages, its memory, its frames, its ledger.

`graph.py` runs the flow; everything that outlives one round lives here. The
module holds a single conversation, as the sidecar serves one at a time.
"""

from __future__ import annotations

import base64
import functools
import io
import re
import struct
import sys
import time

from langchain.agents.middleware import SummarizationMiddleware
from langchain.agents.middleware.summarization import count_tokens_approximately
from langchain_core.messages import (
    AIMessage,
    HumanMessage,
    RemoveMessage,
    SystemMessage,
)
from langgraph.runtime import Runtime

try:                       # optional: without it the older frames are dropped
    from PIL import Image  # instead of scaled, which is what this did before
except ImportError:
    Image = None

import models
import observability

# Tools hand back PNG only, so the mime type is fixed here. Add a protocol field
# if a tool ever returns another format.
MIME = "image/png"

# Pictures kept as images in a request; older ones keep their caption and lose
# the bytes. A request carries every picture still in the list and the provider
# answers 413 past its gateway's limit (~48 MiB, ~8 game screenshots), so this is
# what bounds a request's size. Beyond the newest, a picture is kept only as
# context, so it is worth `_THUMB_PX` instead of the full frame.
_KEEP_PICTURES = 3

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

# Tools that only read, or only draw for the human. Everything else that comes
# back ok is worth remembering: the ledger exists so a later turn can tell what
# already happened, and on which frame.
READ_ONLY_TOOLS = frozenset({
    "ccat_help", "list_scripts", "mark", "read_script", "view_crop",
    "view_shot", "view_template",
})

# ponytail: capped so a tool whose text grows cannot bloat the ledger. Raise it
# if a recorded tool starts returning something longer that matters.
_LEDGER_CHARS = 80

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

# Built lazily: both the budget and the model come from the environment, which
# main.py loads from llm/.env before the first request. A summary lands in
# _MESSAGES as the first HumanMessage, never as a SystemMessage: the summarizer
# drops those, so build_messages prepends the prompt instead.
_MW = None


def reset() -> None:
    """Drop every turn. The next request starts a fresh conversation."""
    global _MW, _LATEST_NUM, _LATEST_B64, _LATEST_TIME, _MEMORIZED_UPTO, _TURN_PREV_SHOT
    _MESSAGES.clear()
    _LATEST_NUM = 0
    _LATEST_B64 = ""
    _LATEST_TIME = 0.0
    _TURN_PREV_SHOT = None
    _MW = None
    _MEMORIZED_UPTO = 0


def size() -> int:
    """Turns currently kept verbatim; a summary does not count as a turn."""
    return sum(1 for m in _MESSAGES if isinstance(m, AIMessage))


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


def append_turn(text: str, reply: str, ledger: list) -> None:
    """Add the turn to the conversation, ledger folded into the stored reply.

    The ledger never comes back out: it is for the model's own history, while
    `reply` is what the UI shows, so the two stay apart.
    """
    if ledger:
        reply = (reply.rstrip() + "\n\n### Actions In Current Turn\n"
                 + "\n".join("- " + line for line in ledger))
    _MESSAGES.append(HumanMessage(content=text))
    _MESSAGES.append(AIMessage(content=reply))


def memorize(memory: str) -> str | None:
    """Fold the turns since the last call into `memory`, returning the new notes.

    Called between turns rather than during one, so the user is never waiting on
    it. Returns `memory` unchanged when there is nothing new -- no model call --
    and None when the call fails, which leaves the caller's file alone.
    """
    global _MEMORIZED_UPTO
    if _MEMORIZED_UPTO >= len(_MESSAGES):
        return memory
    try:
        model = models.build_memory_model()
        instructions = models.MEMORY_INSTRUCTIONS.format(
            max_chars=models.memory_max_chars()
        )
        with observability.turn("rewrite-memory") as callbacks:
            reply = model.invoke([
                SystemMessage(content=instructions),
                HumanMessage(content=(
                    "## Notes so far\n" + (memory.strip() or "(none)") +
                    "\n\n## Session transcript\n" +
                    _transcript(_MESSAGES[_MEMORIZED_UPTO:])
                )),
            ], config={"callbacks": callbacks})
    except Exception as exc:  # noqa: BLE001 - a missed update, never fatal
        print(f"[sidecar] memory not updated: {exc}", file=sys.stderr)
        return None
    text = strip_tool_markup(
        content_text(getattr(reply, "content", reply))
    ).strip()
    if not text:
        return None
    _MEMORIZED_UPTO = len(_MESSAGES)
    return text


def _transcript(messages: list) -> str:
    """The conversation as plain text, for the memory model to read."""
    lines: list[str] = []
    for m in messages:
        if isinstance(m, HumanMessage):
            lines.append("User: " + content_text(m.content))
        elif isinstance(m, AIMessage):
            lines.append("Assistant: " + content_text(m.content))
    return "\n\n".join(lines)


def _middleware():
    """Summarizes older turns once the request approaches the budget.

    `keep` is a quarter of the budget. The history is text only -- screenshots
    live outside it and only the latest one is re-sent, by `build_messages` --
    so a quarter already covers many turns.
    """
    global _MW
    if _MW is None:
        counter = functools.partial(
            count_tokens_approximately, tokens_per_image=models.TOKENS_PER_IMAGE
        )
        budget = models.context_tokens()
        _MW = SummarizationMiddleware(
            model=models.build_chat_model(),
            trigger=("tokens", budget),
            keep=("tokens", budget // 4),
            token_counter=counter,
        )
    return _MW


def compact() -> None:
    """Fold the oldest turns into a rolling summary once past the budget.

    Summarizing rewrites the request prefix, which invalidates the provider's
    prompt cache from that point on, so the summarizer's own hysteresis is what
    keeps this rare. A failure here is skipped, never fatal: the request is
    still far below the model's real limit, so it goes out unsummarized.
    """
    if not _MESSAGES:
        return
    try:
        out = _middleware().before_model({"messages": list(_MESSAGES)}, Runtime())
    except Exception as exc:  # noqa: BLE001 - summarizing must not lose history
        print(f"[sidecar] summarization skipped: {exc}", file=sys.stderr)
        return
    if not out or "messages" not in out:
        return
    # Drop the RemoveMessage sentinel: it signals a reducer this list has none of.
    _MESSAGES[:] = [m for m in out["messages"] if not isinstance(m, RemoveMessage)]


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


def picture_of(name: str, image: str) -> HumanMessage:
    """A marked-up copy some other tool returned, for the user to look at.

    Not stored as a frame: its box and label would be baked into any template
    cut from it.
    """
    return HumanMessage(content=_shot_content(f"Image after {name}.", image))


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

    `save_template`'s `shot` is this number and its box is in these pixels, so
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
    note = ("[image not attached; call view_shot(" + found[1] + ") to look again]"
            if found else "[image not attached]")
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


def demote_pictures(messages: list, keep: int | None = None) -> list:
    """Price each picture by what the model does with it.

    The newest stays full size: a tap's box is measured on it and validated at
    that resolution. The next few become thumbnails. The rest lose their image
    and keep their caption, which already names the number `view_shot` wants
    back.

    `keep` defaults to the window this module holds, resolved at the call rather
    than bound as a default argument, so `_KEEP_PICTURES` stays the one knob.
    """
    keep = _KEEP_PICTURES if keep is None else keep
    pics = [i for i, m in enumerate(messages) if _is_picture(m)]
    for n, i in enumerate(reversed(pics)):
        if n >= keep:
            messages[i] = _detach_picture(messages[i])
        elif n >= 1:
            messages[i] = _shrink_picture(messages[i])
    return messages


def ledger_line(name: str, args: dict, reply: dict) -> str:
    """One ledger line for a tool that ran: what it did, and on which frame.

    Worded to be read without this file: the box names the same `x/y/w/h` the
    model's own call used, and `on #N` says which frame it acted on. Only
    frame-bound tools carry a number. A tap always lands on the screen the model
    last saw, which is the newest frame; `save_template` may name an older one.
    The rest are worth only the host's own short summary, because a tool's
    arguments can be a whole script.
    """
    if name == "screenshot":
        return f"screenshot #{reply.get('shot')}"
    if name == "tap":
        return (f"tap x={args.get('x')} y={args.get('y')} w={args.get('w')}"
                f" h={args.get('h')} on #{_LATEST_NUM}")
    if name == "save_template":
        shot = args.get("shot") or _LATEST_NUM
        return (f"save_template \"{args.get('name')}\" x={args.get('x')}"
                f" y={args.get('y')} w={args.get('w')} h={args.get('h')}"
                f" on #{shot}")
    text = " ".join(str(reply.get("text") or "").split())
    if len(text) > _LEDGER_CHARS:
        text = text[:_LEDGER_CHARS] + "..."
    return f"{name}: {text}" if text else name


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
