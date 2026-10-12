"""Langfuse assembly. Optional: no credentials, no client, no network.

Langfuse reads its credentials when the client is built, and `main` imports
this module before `load_env_file` runs, so nothing here may touch the client
at import time -- every entry point resolves it lazily.
"""

from __future__ import annotations

import contextlib
import os
import sys
import uuid
from typing import Iterator

_client = None       # The langfuse client, resolved once.
_disabled = False    # Credentials missing or the SDK refused: stop retrying.
_session_id = ""


def _client_or_none():
    global _client, _disabled
    if _client is not None or _disabled:
        return _client
    if not (os.environ.get("LANGFUSE_PUBLIC_KEY")
            and os.environ.get("LANGFUSE_SECRET_KEY")):
        _disabled = True
        return None
    try:
        from langfuse import get_client
        _client = get_client()
    except Exception as exc:  # noqa: BLE001 - tracing never breaks a turn
        print(f"[sidecar] langfuse disabled: {exc}", file=sys.stderr)
        _disabled = True
    return _client


def start() -> None:
    """Build the client and open the first session. Call after load_env_file."""
    if _client_or_none() is not None and not _session_id:
        new_session()


def new_session() -> str:
    """Start a new conversation id, so its turns group in Langfuse Sessions."""
    global _session_id
    _session_id = uuid.uuid4().hex
    return _session_id


@contextlib.contextmanager
def turn(name: str, input_text: str = "") -> Iterator[list]:
    """One named trace, with a handler bound to it. Yields the callbacks.

    The handler must be built inside `propagate_attributes`, or it does not
    inherit this trace and starts a second one.
    """
    client = _client_or_none()
    if client is None:
        yield []
        return
    from langfuse import propagate_attributes
    from langfuse.langchain import CallbackHandler
    with client.start_as_current_observation(as_type="span", name=name) as span:
        if input_text:
            span.update(input={"instruction": input_text})
        with propagate_attributes(session_id=_session_id):
            yield [CallbackHandler()]


def flush() -> None:
    """Send what is still buffered. Called once, on the way out."""
    client = _client_or_none()
    if client is None:
        return
    try:
        client.flush()
    except Exception as exc:  # noqa: BLE001 - never block shutdown
        print(f"[sidecar] langfuse flush failed: {exc}", file=sys.stderr)
