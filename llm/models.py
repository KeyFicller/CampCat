"""Model defaults, `.env` loading, and LangChain chat-model construction.

All model configuration lives here. The C++ side sends only an image and
receives text; nothing about the provider crosses the protocol.
"""

from __future__ import annotations

import os
from pathlib import Path

DEFAULT_PROVIDER = "deepseek"
DEFAULT_SYSTEM_PROMPT = "You describe what is shown in the screenshot concisely."
DEFAULT_MAX_TOKENS = 1024
DEFAULT_CONTEXT_TOKENS = 32000
# Measured: a 1080x2400 emulator screenshot costs ~985 input tokens. The
# summarization counter never sees the image bytes, so it needs this constant.
TOKENS_PER_IMAGE = 950

# Per-provider fallbacks for the settings that differ between them.
PROVIDER_DEFAULTS = {
    "deepseek": {
        "model": "deepseek-flash",
        "base_url": "https://api.deepseek.com",
    },
    "ollama": {
        "model": "llava",
        "base_url": "http://localhost:11434",
    },
}


def load_env_file(path: Path) -> int:
    """Load `KEY=VALUE` lines into os.environ, without clobbering real env vars.

    The shell wins over the file, so `CAMPCAT_LLM_MODEL=... python3 main.py`
    behaves as expected. Returns the number of variables applied.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return 0

    applied = 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip().strip("'\"")
        if not key or key in os.environ:
            continue
        os.environ[key] = value
        applied += 1
    return applied


def _env(name: str) -> str:
    return os.environ.get(name, "").strip()


def _setting(name: str, provider_default: str) -> str:
    return _env(name) or provider_default


def system_prompt() -> str:
    return _env("CAMPCAT_LLM_SYSTEM_PROMPT") or DEFAULT_SYSTEM_PROMPT


def context_tokens() -> int:
    """Input-token budget for the conversation.

    Older turns are summarized only once the request exceeds this. Falls back
    to the default when the value is missing, non-numeric, or implausibly
    small, so a bad env var can never disable summarization entirely.
    """
    raw = _env("CAMPCAT_LLM_CONTEXT_TOKENS")
    if raw:
        try:
            value = int(raw)
        except ValueError:
            return DEFAULT_CONTEXT_TOKENS
        if value >= 1000:
            return value
    return DEFAULT_CONTEXT_TOKENS


def build_chat_model():
    """Build the chat model named by the environment.

    Raises ValueError for an unknown provider and RuntimeError when a hosted
    provider has no API key; the caller reports both as a failed result.
    """
    provider = (_env("CAMPCAT_LLM_PROVIDER") or DEFAULT_PROVIDER).lower()
    if provider not in PROVIDER_DEFAULTS:
        raise ValueError(f"unknown provider: {provider}")

    preset = PROVIDER_DEFAULTS[provider]
    model = _setting("CAMPCAT_LLM_MODEL", preset["model"])
    base_url = _setting("CAMPCAT_LLM_BASE_URL", preset["base_url"])
    max_tokens = int(_env("CAMPCAT_LLM_MAX_TOKENS") or DEFAULT_MAX_TOKENS)

    if provider == "deepseek":
        api_key = _env("CAMPCAT_LLM_API_KEY")
        if not api_key:
            raise RuntimeError(
                "CAMPCAT_LLM_API_KEY is not set (put it in llm/.env)"
            )
        from langchain_deepseek import ChatDeepSeek

        return ChatDeepSeek(
            model=model,
            api_key=api_key,
            api_base=base_url,
            max_tokens=max_tokens,
        )

    from langchain_ollama import ChatOllama

    return ChatOllama(
        model=model,
        base_url=base_url,
        num_predict=max_tokens,
    )
