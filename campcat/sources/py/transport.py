"""NDJSON transport shared by the request loop and the tool bridge.

A tool call is made from inside a graph node, several frames below
`handle_request`, and it has to read the host's answer off the same stdin the
request loop reads. Both therefore go through this module instead of threading
a stream and a request id down through every call.

One thread owns stdin. A model call blocks for as long as the provider takes to
think, so nothing in the request path is reading the host while it generates;
without that thread an `interrupt` from the host would sit in the pipe until
the generation it was meant to stop had finished on its own.
"""

from __future__ import annotations

import queue
import sys
import threading
from typing import Any, TextIO

import protocol


class TurnInterrupted(Exception):
    """The host stopped this turn: no result is coming for what was in flight."""


# Messages for the request loop, put there by the reader thread. `None` means
# stdin closed, and is put there exactly once.
_QUEUE: "queue.Queue[dict | None]" = queue.Queue()
# Messages read while a tool call was waiting for its own reply, put back for
# the request loop. Nothing else touches stdout, so this needs no lock.
_PENDING: list[dict] = []
# Requests the host has stopped. An interrupt arrives while a model call is on
# the wire, so it cannot be delivered as a message: the generation loop polls
# this instead.
_STOP_IDS: set = set()
# True while `call_cpp` waits for its own reply. An interrupt then belongs to
# that loop, not to whatever reads the queue next.
_TOOL_WAIT = False
_CLOSED = False
_CURRENT_RID: Any = None


def set_current_rid(rid: Any) -> None:
    """Record the id of the request being served; tool calls echo it."""
    global _CURRENT_RID
    _CURRENT_RID = rid


def get_current_rid() -> Any:
    return _CURRENT_RID


def emit(msg: dict) -> None:
    sys.stdout.write(protocol.encode(msg) + "\n")
    sys.stdout.flush()


def _pump(stream: TextIO) -> None:
    """Read `stream` until EOF, sorting messages between the two readers."""
    while True:
        line = stream.readline()
        if not line:
            _QUEUE.put(None)
            return
        msg = protocol.decode(line)
        if msg is None:
            continue
        if msg.get("type") == "interrupt":
            # Recorded either way: the request it names may be generating,
            # in which case nothing else is going to look at the message.
            _STOP_IDS.add(msg.get("id"))
            if _TOOL_WAIT:
                _QUEUE.put(msg)
            continue
        _QUEUE.put(msg)


def start_reader() -> None:
    """Start the thread that owns stdin. Call once, before the first request."""
    threading.Thread(target=_pump, args=(sys.stdin,), daemon=True).start()


def set_tool_wait(on: bool) -> None:
    """Say whether `call_cpp` is waiting, so an interrupt reaches it."""
    global _TOOL_WAIT
    _TOOL_WAIT = on


def stop_requested() -> bool:
    """True once the host has stopped the request being served."""
    return get_current_rid() in _STOP_IDS


def read_message() -> dict | None:
    """One decoded message, or None once stdin is closed.

    Malformed and blank lines are skipped: a stray line is not worth failing a
    request over.
    """
    global _CLOSED
    while True:
        if _PENDING:
            msg = _PENDING.pop(0)
        elif _CLOSED:
            return None
        else:
            msg = _QUEUE.get()
            if msg is None:
                _CLOSED = True
                return None
        if msg.get("type") == "interrupt" and not _TOOL_WAIT:
            # Its stop is already recorded, and it is not a request: answering
            # it in the request loop would clear the provider selection. A tool
            # call that ended before it was read leaves it behind.
            continue
        return msg


def push_back(msg: dict) -> None:
    """Hand a message back to the request loop, preserving arrival order."""
    _PENDING.insert(0, msg)
