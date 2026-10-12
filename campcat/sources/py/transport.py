"""NDJSON transport shared by the request loop and the tool bridge.

A tool call is made from inside a graph node, several frames below
`handle_request`, and it has to read the host's answer off the same stdin the
request loop reads. Both therefore go through this module instead of threading
a stream and a request id down through every call.
"""

from __future__ import annotations

import sys
from typing import Any

import protocol

# Messages read while a tool call was waiting for its own reply, put back for
# the request loop. Nothing else touches stdout, so this needs no lock.
_PENDING: list[dict] = []
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


def read_message() -> dict | None:
    """One decoded message, or None once stdin is closed.

    Malformed and blank lines are skipped: a stray line is not worth failing a
    request over.
    """
    if _PENDING:
        return _PENDING.pop(0)
    while True:
        line = sys.stdin.readline()
        if not line:
            return None
        msg = protocol.decode(line)
        if msg is not None:
            return msg


def push_back(msg: dict) -> None:
    """Hand a message back to the request loop, preserving arrival order."""
    _PENDING.insert(0, msg)
