"""NDJSON message encoding/decoding for the CampCat LLM sidecar."""

from __future__ import annotations

import json

PROTOCOL_VERSION = 9


def encode(obj: dict) -> str:
    """Serialize one message to a single JSON line (no trailing newline)."""
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def decode(line: str) -> dict | None:
    """Parse one line into a dict; None for blank or malformed input."""
    if not line or not line.strip():
        return None
    try:
        obj = json.loads(line)
    except (ValueError, TypeError):
        return None
    return obj if isinstance(obj, dict) else None


def build_ready() -> dict:
    return {"type": "ready", "protocol": PROTOCOL_VERSION}


def build_log(level: str, message: str) -> dict:
    return {"type": "log", "level": level, "message": message}


def build_chunk(rid: object, text: str = "", thinking: bool = False) -> dict:
    """One streaming delta, or a reasoning heartbeat when `thinking`."""
    msg = {"type": "chunk", "id": rid}
    if text:
        msg["text"] = text
    if thinking:
        msg["thinking"] = True
    return msg


def build_result(rid: object, ok: bool, text: str = "", error: str = "",
                 turns: int | None = None, interrupted: bool = False,
                 llm_calls: int | None = None, tool_calls: int | None = None,
                 context: int | None = None) -> dict:
    """`turns` is omitted from the JSON when None, so messages that carry no
    conversation state keep their original shape. `interrupted` is omitted
    when false: the host reads a missing field as false. The usage fields are
    omitted the same way: a missing field means "this message does not carry
    the number", not zero."""
    msg = {"type": "result", "id": rid, "ok": ok, "text": text, "error": error}
    if turns is not None:
        msg["turns"] = turns
    if interrupted:
        msg["interrupted"] = True
    if llm_calls is not None:
        msg["llm_calls"] = llm_calls
    if tool_calls is not None:
        msg["tool_calls"] = tool_calls
    if context is not None:
        msg["context"] = context
    return msg


def build_error_result(rid: object, error: str, **usage) -> dict:
    return build_result(rid, False, "", error, **usage)


def build_tool_call(rid: object, call_id: str, name: str, args: dict,
                    llm_calls: int | None = None, tool_calls: int | None = None,
                    context: int | None = None) -> dict:
    """Ask the host to run one tool. The host answers with `tool_result`.

    A `shot` number inside `args` names a screenshot the host keeps itself, so
    nothing is attached here: the host looks the frame up.
    """
    msg = {"type": "tool_call", "id": rid, "call_id": call_id, "name": name,
           "args": args}
    if llm_calls is not None:
        msg["llm_calls"] = llm_calls
    if tool_calls is not None:
        msg["tool_calls"] = tool_calls
    if context is not None:
        msg["context"] = context
    return msg
