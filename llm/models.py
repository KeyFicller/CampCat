"""Model defaults, `.env` loading, and LangChain chat-model construction.

All model configuration lives here. The C++ side sends only an image and
receives text; nothing about the provider crosses the protocol.
"""

from __future__ import annotations

import os
from pathlib import Path

DEFAULT_PROVIDER = "deepseek"
DEFAULT_SYSTEM_PROMPT = (
    "You operate an Android device through the provided tools. Take a screenshot "
    "first, since you cannot see the screen otherwise, and look at the result "
    "before deciding the next step. When the user only asks a question, answer it "
    "instead of acting. Write replies in Markdown, limited to what the viewer "
    "renders: '#'/'##'/'###' plus a space at the start of a line for headings; "
    "'**bold**' with a space before the opening marker; '  * ' (two spaces, "
    "asterisk, space) for list items, indented two more spaces per level; '***' "
    "alone on a line for a divider. Do not use tables, code fences, ordered "
    "lists, '-' bullets, images or single-'*' italics; the viewer does not "
    "render them.\n"
    "\n"
    "Example of a correctly formatted reply:\n"
    "\n"
    "## Current screen\n"
    "I can see the **WeChat** main screen, with the chats tab selected.\n"
    "\n"
    "  * A search box at the top\n"
    "  * Four tabs along the bottom\n"
    "  * Two unread messages\n"
    "\n"
    "***\n"
    "Shall I open one of the tabs?\n"
    "\n"
    "A reply written the wrong way keeps its markers on screen. This:\n"
    "\n"
    "- Wait 1 second\n"
    "1. Tap the button\n"
    "```tap(540, 1200)```\n"
    "| Step | Action |\n"
    "## **Bold** heading\n"
    "Tap it.**Then wait.**\n"
    "\n"
    "shows the dashes, numbers, backticks, pipes and asterisks as plain text.\n"
    "Write the same content as:\n"
    "\n"
    "## Next steps\n"
    "  * Wait **1** second\n"
    "  * Tap the button at (540, 1200)\n"
    "  * Tap it. **Then wait.**\n"
)
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


_CCAT_CHEATSHEET = (
    "You can also write a .ccat script and run it with `run_script`. Call `ccat_help` "
    "for the full language reference. The parts that are easy to get wrong:\n"
    "  * Comments are `//`; semicolons are optional. There are no expressions and no "
    "runtime variables: `defs` values are substituted while parsing.\n"
    "  * `tap_at(x, y)` / `swipe_at(...)` take coordinates normalized to [0,1] against "
    "the screenshot size, not pixels; out of range is an error.\n"
    "  * Actions: tap(\"a.png\"), tap_at(x, y), tap_offset(\"a.png\", dx, dy), "
    "swipe(\"a.png\", \"b.png\"), wait(ms), wait_until(\"a.png\", ms) (a timeout is an "
    "error), log(\"msg\"), home, run(\"other.ccat\").\n"
    "  * Control flow: if (\"a.png\") { } else { }, retry(n) { } (retries until it "
    "succeeds), loop(n) { }, do { } while (\"a.png\") (stops hard at 50000 iterations), "
    "break, return.\n"
    "  * A failing statement stops the script and comes back to you as an error. "
    "Statements that need a PNG (`tap`, `tap_offset`, `swipe`, `wait_until`) or a `.ccat` "
    "(`run`) fail when the file is missing; an `if` with a missing template is just false."
)


def system_prompt() -> str:
    # Appended after the override, not replaced by it: the language reference is
    # what makes the script tools usable at all, not a stylistic choice.
    return f"{_env('CAMPCAT_LLM_SYSTEM_PROMPT') or DEFAULT_SYSTEM_PROMPT}\n\n{_CCAT_CHEATSHEET}"


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
        api_key = _env("DEEPSEEK_API_KEY")
        if not api_key:
            raise RuntimeError(
                "DEEPSEEK_API_KEY is not set (put it in llm/.env)"
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
