#!/usr/bin/env python3
"""CampCat LLM sidecar: NDJSON over stdin/stdout.

Owns all model configuration; the C++ host only sends an image and reads back
text.

Usage:
  python3 llm/main.py           # persistent: print `ready`, then serve requests
  python3 llm/main.py --once    # read one request line, print one result, exit
"""

from __future__ import annotations

import pathlib
import sys
import traceback

import graph
import models
import protocol


def handle_request(req: dict) -> dict:
    """Turn one decoded request into one response message."""
    rtype = req.get("type")
    rid = req.get("id")

    if rtype == "ping":
        return protocol.build_result(rid, True, "pong")
    if rtype != "describe":
        return protocol.build_log("warn", f"ignoring unknown message type: {rtype}")

    try:
        out = graph.run_describe({"image_b64": req.get("image_b64") or ""})
    except Exception as exc:  # noqa: BLE001 - never let a request kill the loop
        traceback.print_exc(file=sys.stderr)
        return protocol.build_error_result(rid, str(exc))

    if out.get("error"):
        return protocol.build_error_result(rid, str(out["error"]))
    return protocol.build_result(rid, True, str(out.get("text", "")))


def emit(msg: dict) -> None:
    sys.stdout.write(protocol.encode(msg) + "\n")
    sys.stdout.flush()


def main(argv: list[str]) -> int:
    once = "--once" in argv

    applied = models.load_env_file(pathlib.Path(__file__).resolve().parent / ".env")
    if applied:
        print(f"[sidecar] loaded {applied} var(s) from llm/.env", file=sys.stderr)

    if not once:
        emit(protocol.build_ready())

    for line in sys.stdin:
        req = protocol.decode(line)
        if req is None:
            continue
        emit(handle_request(req))
        if once:
            break

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except BaseException:  # noqa: BLE001 - fatal: report on stderr only
        traceback.print_exc(file=sys.stderr)
        raise SystemExit(1)
