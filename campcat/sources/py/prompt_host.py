"""Langfuse Prompt Management, with the in-repo text as the fallback.

The client is the one `observability` already builds. No second setup, and
nothing here runs at import: the first caller fetches. No credentials, a
missing name, or a failed fetch returns the fallback. A fetch failure is
logged once per name per session and never raised into a turn.

Templates are cached until the next Langfuse session
(`observability.new_session`, which a host reset starts). `{{var}}` is the
Langfuse placeholder and `{var}` is the one the in-repo text already uses.
Only the names passed in are filled, so JSON examples stay as they are.

The label is `CAMPCAT_LANGFUSE_PROMPT_LABEL`, or `production` when that is
unset. `CAMPCAT_LLM_SYSTEM_PROMPT` is not handled here; the operator and solo
prompts still treat it as a full local override.
"""

from __future__ import annotations

import os
import re
import sys
import threading

import observability

# Names published to Langfuse. Create each as a text prompt.
OPERATOR = "campcat/operator"
SOLO = "campcat/solo"
SCRIPTER = "campcat/scripter"
REGION_REVIEW = "campcat/region-review"          # {{extra}}
REGION_PICK = "campcat/region-pick"
REGION_TEMPLATE_PICK = "campcat/region-template-pick"
DIAGNOSE = "campcat/diagnose"
CONSULT = "campcat/consult"
MEMORY_OPERATOR = "campcat/memory-operator"      # entry_chars, max_ops
MEMORY_SCRIPTER = "campcat/memory-scripter"      # entry_chars, max_ops
MEMORY_FRAME = "campcat/memory-frame"
SCRIPTER_MEMORY_FRAME = "campcat/scripter-memory-frame"
REFLECTOR = "campcat/reflector"                  # max_chars

# One attempt. The SDK's default retries with backoff, and a down host would
# then stall the first turn that asks for a prompt.
_FETCH_TIMEOUT_SECONDS = 3
_MUSTACHE = re.compile(r"\{\{\s*(\w+)\s*\}\}")

_cache: dict[tuple[str, str], str | None] = {}
_lock = threading.Lock()


def _langfuse():
    """The tracing client, or None when Langfuse is off."""
    return observability._client_or_none()


def reset() -> None:
    """Drop cached templates. A new Langfuse session calls this."""
    with _lock:
        _cache.clear()


def label() -> str:
    """The prompt label this process asks Langfuse for."""
    raw = os.environ.get("CAMPCAT_LANGFUSE_PROMPT_LABEL", "").strip()
    return raw or "production"


def text(name: str, fallback: str, **variables: object) -> str:
    """Prompt `name` at the current label, or `fallback`, with variables filled."""
    template = _template(name)
    if template is None:
        template = fallback
    return _fill(template, variables)


def _template(name: str) -> str | None:
    client = _langfuse()
    if client is None:
        return None
    key = (name, label())
    with _lock:
        if key not in _cache:
            _cache[key] = _fetch(client, name, key[1])
        return _cache[key]


def _fetch(client, name: str, prompt_label: str) -> str | None:
    try:
        got = client.get_prompt(
            name,
            label=prompt_label,
            type="text",
            cache_ttl_seconds=0,
            max_retries=0,
            fetch_timeout_seconds=_FETCH_TIMEOUT_SECONDS,
        )
    except Exception as exc:  # noqa: BLE001 - a prompt miss never fails a turn
        print(
            f"[sidecar] langfuse prompt {name!r} ({prompt_label}) "
            f"unavailable: {exc}; using in-repo text",
            file=sys.stderr,
        )
        return None
    body = getattr(got, "prompt", None)
    if not isinstance(body, str) or not body.strip():
        # A chat prompt is a list of messages. This sidecar only has a slot
        # for one system string, so that shape is a miss, same as an empty one.
        why = "is empty" if isinstance(body, str) else "is not a text prompt"
        print(
            f"[sidecar] langfuse prompt {name!r} ({prompt_label}) "
            f"{why}; using in-repo text",
            file=sys.stderr,
        )
        return None
    return body


def _fill(template: str, variables: dict[str, object]) -> str:
    """Fill `{{name}}` and `{name}`.

    Not `str.format`: several prompts contain JSON examples, and every brace
    in those would be read as a field. Only the names the caller passed are
    replaced. Mustache runs first so `{{name}}` does not collapse to `{value}`.
    """
    if not variables:
        return template

    def mustache(match: re.Match) -> str:
        key = match.group(1)
        if key not in variables:
            return match.group(0)
        return _as_text(variables[key])

    template = _MUSTACHE.sub(mustache, template)
    for key in sorted(variables, key=len, reverse=True):
        template = template.replace("{" + key + "}", _as_text(variables[key]))
    return template


def _as_text(value: object) -> str:
    if value is None:
        return ""
    return str(value)
