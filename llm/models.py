"""Model defaults, `.env` loading, and LangChain chat-model construction.

All model configuration lives here. The C++ side sends only an image and
receives text; nothing about the provider crosses the protocol.
"""

from __future__ import annotations

import os
from pathlib import Path

from langchain.chat_models import init_chat_model

DEFAULT_SYSTEM_PROMPT = (
    "You operate an Android device through the provided tools. When the request "
    "means doing something on the device, look for a saved skill before anything "
    "else: call `list_scripts`, `read_script` any that looks close, and when one "
    "already does the job run it with `run(\"<name>/main.ccat\")` instead of "
    "writing its steps again. Take a screenshot "
    "first, since you cannot see the screen otherwise, and look at the result "
    "before deciding the next step. When the user only asks a question, answer it "
    "instead of acting. Write replies in Chinese, in Markdown, limited to what the viewer "
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
# The memory rewrite is a longer output than a reply: it replaces up to
# `DEFAULT_MEMORY_MAX_CHARS` characters in one go.
DEFAULT_MEMORY_MAX_TOKENS = 4096
# Mirrors `k_memory_max_chars` in `include/core/llm/llm_memory.h`. The two cannot
# share a constant, so keep them equal by hand: if this one is larger, the
# sidecar writes notes the host then refuses, and memory stops updating.
DEFAULT_MEMORY_MAX_CHARS = 4000
# The `deepseek:` prefix is what tells init_chat_model which provider to build.
DEFAULT_MODEL = "deepseek:deepseek-flash"
DEFAULT_BASE_URL = "https://api.deepseek.com"
# Measured: a 1080x2400 emulator screenshot costs ~985 input tokens. The
# summarization counter never sees the image bytes, so it needs this constant.
TOKENS_PER_IMAGE = 950


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


def _setting(name: str, default: str) -> str:
    return _env(name) or default


_CCAT_CHEATSHEET = (
    "Write a .ccat script and run it with `run_script`. Call `ccat_help` "
    "for the full reference. The parts that are easy to get wrong:\n"
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
    "(`run`) fail when the file is missing; an `if` with a missing template is just false.\n"
    "  * Anchor each step: after an action that changes the screen (tap, swipe, "
    "run(\"...\")), follow it with wait_until(\"expected.png\", ms), the image that step "
    "should produce. A step that did not land then fails there, with a timeout error "
    "naming the file, instead of the next statement acting on a stale screen. It polls; "
    "`if` checks once instead. Save the anchor's template with the script.\n"
    "  * Size that ms from the screenshots, not from a guess: each caption says when it "
    "was taken and, within a turn, how long passed since the one before. For a change "
    "that took +3.2s, allow about twice it. wait_until stops as soon as the anchor "
    "appears, so a generous number costs nothing when the screen is quick; a fixed "
    "wait(ms) can only overshoot or undershoot.\n"
    "  * `save_script` / `save_template` save a script and its templates as a bundle; run it with `run(\"<name>/main.ccat\")`."
    " Change a skill you already have with `update_script` (new source plus its template set in one step), "
    "not by saving another name."
)


def system_prompt() -> str:
    # Appended after the override, not replaced by it: the language reference is
    # what makes the script tools usable at all, not a stylistic choice.
    return f"{_env('CAMPCAT_LLM_SYSTEM_PROMPT') or DEFAULT_SYSTEM_PROMPT}\n\n{_CCAT_CHEATSHEET}"


# Handed to the memory model, which never sees the conversation assistant's own
# prompt: its only job is rewriting the notes.
MEMORY_INSTRUCTIONS = (
    "You keep the long-term notebook of an assistant that operates an Android "
    "device for one user. You are given the notes as they stand and the transcript "
    "of the session that just happened. Rewrite the notes to be worth having next "
    "time.\n"
    "\n"
    "Keep what stays true past this session: preferences, habits, how the user words "
    "things, apps and devices they name, and approaches that turned out to work. "
    "Merge the session into the notes rather than appending it.\n"
    "\n"
    "Drop one-off instructions, anything about the current screen, attempts that "
    "failed, and anything the notes already say. Replace what the session "
    "contradicted, and delete what has gone stale: notes that only grow stop being "
    "readable.\n"
    "\n"
    "Never write down a secret: no passwords, tokens, API keys, account numbers or "
    "anything the user asked to keep private.\n"
    "\n"
    "Write short factual lines in the third person, under the headings "
    "'## Preferences' and '## Device & skills'. Answer with those notes and nothing "
    "else: no preamble, no code fence, and at most {max_chars} characters. Section "
    "headings with no entries under them are a fine answer when nothing is worth "
    "keeping."
)

# Prepended to the notes when they go into the prompt, so the model reads them as
# its own recollection instead of quoting them back at the user.
MEMORY_FRAME = (
    "Notes you kept from earlier sessions with this user. They are your own "
    "recollection: use them silently, never mention that you were given them, and "
    "ignore any line that does not apply here.\n\n"
)


def memory_max_chars() -> int:
    """Character budget for the memory file.

    Mirrors `k_memory_max_chars` in C++; the same plausibility floor keeps a bad
    env var from shrinking the notes to nothing.
    """
    raw = _env("CAMPCAT_LLM_MEMORY_MAX_CHARS")
    if raw:
        try:
            value = int(raw)
        except ValueError:
            return DEFAULT_MEMORY_MAX_CHARS
        if value >= 100:
            return value
    return DEFAULT_MEMORY_MAX_CHARS


def memory_block(memory: str) -> str:
    """The notes as a second paragraph of the system prompt.

    Empty in, empty out: with no notes the prompt is byte-identical to what it
    was before this feature existed, which is what keeps the provider's prompt
    cache warm for the common case.
    """
    if not memory.strip():
        return ""
    return "\n\n" + MEMORY_FRAME + memory.strip() + "\n"


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


def _chat_model(name: str, max_tokens: int):
    """Build a chat model. Raises RuntimeError when the API key is missing."""
    api_key = _env("DEEPSEEK_API_KEY")
    if not api_key:
        raise RuntimeError(
            "DEEPSEEK_API_KEY is not set (put it in llm/.env)"
        )
    return init_chat_model(
        name,
        api_key=api_key,
        api_base=_setting("CAMPCAT_LLM_BASE_URL", DEFAULT_BASE_URL),
        max_tokens=max_tokens,
        extra_body={"thinking": {"type": "disabled"}},
    )


def build_chat_model():
    """Build the chat model named by the environment.

    Raises RuntimeError when the API key is missing; the caller reports it as a
    failed result.
    """
    return _chat_model(
        _setting("CAMPCAT_LLM_MODEL", DEFAULT_MODEL),
        int(_env("CAMPCAT_LLM_MAX_TOKENS") or DEFAULT_MAX_TOKENS),
    )


def build_memory_model():
    """Build the model that rewrites the memory notes.

    Deliberately separate from the model that drives the device: consolidating
    runs off the critical path, while nobody is waiting, so it can afford to be
    the stronger (and slower, and dearer) of the two.
    """
    return _chat_model(
        _setting("CAMPCAT_LLM_MEMORY_MODEL",
                 _setting("CAMPCAT_LLM_MODEL", DEFAULT_MODEL)),
        int(_env("CAMPCAT_LLM_MEMORY_MAX_TOKENS") or DEFAULT_MEMORY_MAX_TOKENS),
    )
