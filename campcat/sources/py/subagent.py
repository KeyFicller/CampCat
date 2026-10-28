"""Who is working, and the lines a popup shows while it works.

One window: the innermost agent owns the title, and the lines before it stay,
so a region check inside a scripter run reads as part of that run. A model call
or a tool call several frames below the request loop reports here because the
transport is the only line back to the host.

Nothing is emitted while the operator works: `current` is None there, and the
operator's own text already streams through the parent graph.
"""

from __future__ import annotations

import contextlib
import contextvars
from typing import Iterator

import transport

current: contextvars.ContextVar[str | None] = contextvars.ContextVar(
    "subagent_current", default=None)

# Longest one line may be: the window is one line per event, and a tool's
# arguments can carry a region plus the expectation for it.
LIMIT = 200


def line(text: object, limit: int = LIMIT) -> str:
    """One flat line, cut at `limit`."""
    flat = " ".join(str(text or "").split())
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"


def event(kind: str, *, agent: str | None = None, **fields: object) -> None:
    """One line for the window. Silent when no agent is working."""
    who = agent or current.get()
    if not who:
        return
    msg: dict = {"type": "subagent", "id": transport.get_current_rid(),
                 "agent": who, "event": kind}
    msg.update(fields)
    transport.emit(msg)


class _Run:
    """What a `working` block can say about itself on the way out."""

    def __init__(self) -> None:
        self.summary = ""

    def settle(self, text: object) -> None:
        self.summary = line(text)


@contextlib.contextmanager
def working(agent: str, goal: object = "") -> Iterator[_Run]:
    """Report `agent` as working for the block, and finish it on every path.

    An exception, an interrupt included, still emits `end`: a window left
    saying "working" is worse than one that says why it stopped.
    """
    token = current.set(agent)
    event("start", agent=agent, text=line(goal))
    run = _Run()
    ok = True
    try:
        yield run
    except BaseException as exc:
        ok = False
        run.summary = line(f"{type(exc).__name__}: {exc}")
        raise
    finally:
        event("end", agent=agent, ok=ok, text=run.summary)
        current.reset(token)


def chunk_sink(is_answer: bool, delta: str) -> None:
    """The `on_chunk` that `models.streamed` takes: the model's words as they come."""
    event("text" if is_answer else "thinking", text=delta)
