"""NDJSON message encoding/decoding for the CampCat LLM sidecar."""

from __future__ import annotations

import json

PROTOCOL_VERSION = 1


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


def build_result(rid: object, ok: bool, text: str = "", error: str = "") -> dict:
    return {"type": "result", "id": rid, "ok": ok, "text": text, "error": error}


def build_error_result(rid: object, error: str) -> dict:
    return build_result(rid, False, "", error)
