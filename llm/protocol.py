"""NDJSON message encoding/decoding for the CampCat LLM sidecar."""

from __future__ import annotations

import json

PROTOCOL_VERSION = 4


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
                 turns: int | None = None) -> dict:
    """`turns` is omitted from the JSON when None, so messages that carry no
    conversation state keep their original shape."""
    msg = {"type": "result", "id": rid, "ok": ok, "text": text, "error": error}
    if turns is not None:
        msg["turns"] = turns
    return msg


def build_error_result(rid: object, error: str) -> dict:
    return build_result(rid, False, "", error)


def build_tool_call(rid: object, call_id: str, name: str, args: dict,
                    image_b64: str = "") -> dict:
    """Ask the host to run one tool. The host answers with `tool_result`.

    `image_b64` (raw base64, no data URL) carries a screenshot kept from earlier
    in the session, for the tools whose arguments name one. Omitted when empty,
    so every other call keeps its original shape.
    """
    msg = {"type": "tool_call", "id": rid, "call_id": call_id, "name": name,
           "args": args}
    if image_b64:
        msg["image_b64"] = image_b64
    return msg
