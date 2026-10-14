"""Model defaults, `.env` loading, and LangChain chat-model construction.

All model configuration lives here. Which provider to build arrives with each
request -- the C++ host sends the name its settings selected -- and `.env`
holds the keys.
"""

from __future__ import annotations

import os
from pathlib import Path

from langchain.chat_models import init_chat_model

# Who the model is and how it works the device. Kept in its own block so it can
# be edited without touching the reply-format rules below, and vice versa.
PERSONA = (
    "You operate an Android device through the provided tools. When the request "
    "means doing something on the device, look for a saved skill before anything "
    "else: call `list_scripts`, `read_script` any that looks close, and when one "
    "already does the job run it with `run(\"<name>/main.ccat\")` instead of "
    "writing its steps again. An `@name` names that saved skill for you -- the "
    "user dropped it in from the list -- so read that one rather than searching. "
    "Take a screenshot "
    "first, since you cannot see the screen otherwise, and look at the result "
    "before deciding the next step. The emulator itself may be down, and a "
    "screenshot then fails and says so. `start_emulator` boots it and blocks "
    "until the instance is up, so give that one a round of its own: a "
    "screenshot batched with it runs first, while the boot is still going, and "
    "fails too. When the user only asks a question, answer it "
    "instead of acting."
)

# How a reply is written, one entry per part of the rule. Fed as Markdown with
# the key as a heading, so a part can be rewritten or dropped on its own.
RULES = {
    "autonomy": (
        "Act instead of asking. Propose the next step and take it, rather than "
        "ending the reply with a question you could answer by acting. Anything that "
        "changes the device or the disk is shown to the user and waits for their "
        "approval, so asking in prose first is the same question twice -- and a guess "
        "they disagree with costs a rejection, not a mistake."
    ),
    "format": (
        "Write replies in Chinese, in Markdown, limited to what the viewer "
        "renders: '#'/'##'/'###' plus a space at the start of a line for headings; "
        "'**bold**' with a space before the opening marker; '  * ' (two spaces, "
        "asterisk, space) for list items, indented two more spaces per level; '***' "
        "alone on a line for a divider. Do not use tables, code fences, ordered "
        "lists, '-' bullets, images or single-'*' italics; the viewer does not "
        "render them."
    ),
    "example_ok": (
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
        "Opening the chats tab."
    ),
    "example_bad": (
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
    ),
    "script_workflow": (
        "Writing or changing a script is this sequence, and you do not skip a step:\n"
        "\n"
        "1. `list_scripts` first. If a bundle already does the job, run it with "
        "`run(\"<name>/main.ccat\")`; if one is close, `read_script` it and build on it. "
        "Write a new script only when neither holds.\n"
        "2. `screenshot`, so you can see the screen you are scripting.\n"
        "3. Decide each step's anchor: the image that only appears when that step worked, "
        "taken from a fixed UI control -- a button, a tab, a label -- never from free-form "
        "text, which drifts between runs and makes the check meaningless. Write what that "
        "step expects beside it as a `//` comment (`wait_until(\"claim_taken.png\", 8000)"
        "   // the button turns grey once the claim lands`): a failing run then has an "
        "expectation to compare the screen against, and the approval reads.\n"
        "4. `save_script` to create the bundle. It writes main.ccat and runs nothing; a name "
        "that already exists is refused.\n"
        "5. `save_template`, once for every PNG the script names. Screenshot right before "
        "each cut -- it can only match as well as the picture it came from -- and crop tight "
        "around the part that stays put. `save_template(bundle, name, shot, box)` takes the "
        "box; for a round element use `save_template_circle(bundle, name, shot, circle)` "
        "instead, which writes the disc's alpha and leaves every pixel outside it out of "
        "the match.\n"
        "6. Run the bundle: `run_script` with the source `run(\"<name>/main.ccat\")`. Pass "
        "that `run(...)` call and not the script's own text -- a bare `run_script` resolves "
        "template names against the script root, so the PNGs you just saved into the bundle "
        "are invisible to it.\n"
        "7. Change what you have with `update_script(name, source, templates)`: the source "
        "and the whole template set in one step, under one approval. Each entry is "
        "`{\"name\":...,\"cut\":{\"x\":...,\"y\":...,\"w\":...,\"h\":...},\"shot\":...}` "
        "for a box or the same with `{\"cx\":...,\"cy\":...,\"r\":...}` for a circle, and "
        "`shot` omitted means the most recent screenshot. `save_script` refuses an "
        "existing name, and the answer is never a second bundle under a new name."
    ),
    "script_rules": (
        "  * A failing statement stops the script. That error is what you get back -- not a "
        "screen that looks wrong.\n"
        "  * Anchor every action. After a tap, swipe or `run(...)`, assert the screen it "
        "should have produced with `wait_until(\"expected.png\", ms)`. A step that did not "
        "land then fails there, naming the file, instead of the next statement acting on a "
        "stale screen. Save the anchor's template with the script.\n"
        "  * Ground every `log(...)` in an anchor: never print \"task complete\" on its own. "
        "A `log(...)` that declares a state sits right after the `wait_until` or `if` on a "
        "template that confirms that state.\n"
        "  * Size that `ms` from the screenshots, not from a guess. Each caption says when it "
        "was taken and, within a turn, how long passed since the one before; a change that "
        "took +3.2s gets about twice that. `wait_until` stops as soon as the anchor appears, "
        "so a generous number costs nothing. Both waits are capped, not rejected -- "
        "`wait(ms)` at 5000 ms and `wait_until(\"a.png\", ms)` at 20000 ms -- so a larger "
        "number silently becomes the cap and you get less time than you asked for; "
        "`wait_until(\"a.png\", 0)` is a parse error. A timeout is an error that stops the "
        "script.\n"
        "  * Reuse before you write, and `update_script` before you duplicate: `save_script` "
        "refuses a name that already exists, and the answer is never a second bundle under a "
        "new name.\n"
    ),
    "script_failure_workflow": (
        "A script that fails stops at the statement that failed, and that error comes "
        "back to you with what the matcher saw (`best=`, the `box` it preferred, "
        "`threshold`). That `threshold` is the bar that match was measured against: a "
        "circular template is matched through its alpha mask, which leaves it less to "
        "go on than a square one, so the two are held to different bars. Find out which "
        "side is wrong before changing anything:\n"
        "\n"
        "1. `view_template(bundle, name)` -- what it was looking for (the name is in "
        "the error's path; `read_script` lists them).\n"
        "2. `view_crop(box={\"x\":...,\"y\":...,\"w\":...,\"h\":...})` with the error's "
        "`box`, or `view_crop_circle(circle={\"cx\":...,\"cy\":...,\"r\":...})` with "
        "its `circle` -- what is there now, at the template's own scale. `screenshot` "
        "when you need the whole screen instead.\n"
        "3. Compare both against the failing step's `//` comment: the same control "
        "looking different (a state you never waited for, a label that changes "
        "between runs), or nothing answering it at all (the step before it did not "
        "land, or this is not the screen you thought).\n"
        "4. Fix the cause once and run it again: `update_script` with that template "
        "re-cut when the cut is wrong, or a `wait_until` / a longer `ms` when the "
        "screen simply was not ready. Running the same script again unchanged is not "
        "a fix.\n"
        "5. Still failing after that: say what you now think the screen is doing, "
        "instead of changing another template."
    ),
}


def _read_gold_script() -> str:
    """The worked example, read from the library rather than copied here.

    One example in one file, edited in one place -- `ccat_help` reads `CCAT.md`
    the same way. It sits as a bundle under `ccat_library/`, which also makes it
    readable from the app's own editor. A missing file is not fatal, since the
    rules above stand on their own, so an empty string is the answer and the
    block is left out.
    """
    path = (
        Path(__file__).resolve().parents[3] / "ccat_library" / "gold.ccat"
    )
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


_example = _read_gold_script()
if _example:
    RULES["example_script"] = (
        "A whole script that follows every rule above: reuse through `run`, an anchor "
        "after each action, each `log` on its anchor, and every `ms` sized from the "
        "screenshots. Copy this shape:\n\n" + _example
    )

# Each rule becomes a `##` heading plus its body; PERSONA stays one plain block.
DEFAULT_SYSTEM_PROMPT = PERSONA + "\n\n" + "\n\n".join(
    f"## {name}\n\n{body}" for name, body in RULES.items()
)
DEFAULT_MAX_TOKENS = 1024
# The line compaction triggers at, not the size afterwards: `keep` is a quarter
# of it. The model in use has a 1,048,576-token window, verified against the
# provider, so this sits at about a fifth of it.
DEFAULT_CONTEXT_TOKENS = 200000
# The memory rewrite is a longer output than a reply: it replaces up to
# `DEFAULT_MEMORY_MAX_CHARS` characters in one go.
DEFAULT_MEMORY_MAX_TOKENS = 4096
# Mirrors `k_memory_max_chars` in `campcat/sources/cpp/include/core/llm/llm_memory.h`. The two cannot
# share a constant, so keep them equal by hand: if this one is larger, the
# sidecar writes notes the host then refuses, and memory stops updating.
DEFAULT_MEMORY_MAX_CHARS = 4000
# Every knob, including which variable holds the key and which holds the base
# URL. Both are passed to the client explicitly, so neither name has to be one an
# integration already knows: `QWEN_API_KEY` is ours, not ChatOpenAI's. A blank
# `*_BASE_URL` falls back to the default in the same entry.
#
# `extra_body` is the provider's own thinking switch: Qwen3.7 and DeepSeek V4
# both think by default, and sending one's shape to the other is not a no-op.
PROVIDERS = {
    "deepseek": {
        "key_env": "DEEPSEEK_API_KEY",
        "base_url_env": "DEEPSEEK_BASE_URL",
        "base_url": "https://api.deepseek.com",
        "model": "deepseek:deepseek-flash",
        "extra_body": {"thinking": {"type": "disabled"}},
    },
    "qwen": {
        # The `openai:` prefix is what tells init_chat_model which client to
        # build. This base URL is the Token Plan endpoint, not the pay-as-you-go
        # `dashscope.aliyuncs.com/compatible-mode/v1`: a plan key (`sk-sp-`) is
        # refused there with a 401, and the two are not interchangeable.
        "key_env": "QWEN_API_KEY",
        "base_url_env": "QWEN_BASE_URL",
        "base_url": "https://token-plan.maas.qianwenaiapi.com/compatible-mode/v1",
        "model": "openai:qwen3.8-flash",
        "extra_body": {"enable_thinking": False},
    },
}
DEFAULT_PROVIDER = "deepseek"
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


# Which provider the current request wants, set by main.py from the C++ host's
# settings. Empty means the request did not say -- a bare `python3 main.py` run
# -- and the environment, then the default, decides.
_SELECTED = ""


def set_provider(name: str) -> None:
    """Remember the provider this request asks for; empty clears the choice."""
    global _SELECTED
    _SELECTED = name.strip()


def _provider() -> dict:
    """The selected provider's knobs. Raises on a name that is not one of them.

    Loud rather than silently falling back: a mistyped name would otherwise bill
    DeepSeek while the user believes they are on Qwen.
    """
    name = _SELECTED or _setting("CAMPCAT_LLM_PROVIDER", DEFAULT_PROVIDER)
    if name not in PROVIDERS:
        raise RuntimeError(
            f"provider {name!r} is not one of {', '.join(sorted(PROVIDERS))}"
        )
    return PROVIDERS[name]


_CCAT_CHEATSHEET = (
    "The .ccat language. Call `ccat_help` for the full reference; the parts "
    "that are easy to get wrong:\n"
    "  * Comments are `//`; semicolons are optional. There are no expressions and no "
    "runtime variables: `defs` values are substituted while parsing.\n"
    "  * `tap_at(x, y)` / `swipe_at(...)` take coordinates normalized to [0,1] against "
    "the screenshot size, not pixels; out of range is an error.\n"
    "  * Actions: tap(\"a.png\"), tap_at(x, y), tap_offset(\"a.png\", dx, dy), "
    "swipe(\"a.png\", \"b.png\"), wait(ms), wait_until(\"a.png\", ms) (a timeout is an "
    "error), log(\"msg\"), home, run(\"other.ccat\").\n"
    "  * Control flow: if (has(\"a.png\")) { } else { }, elif for a chain, loop(n) "
    "{ }, do { } while (has(\"a.png\")) (stops hard at 50000 iterations), break, "
    "return. `if` / `elif` / `while` also take `has(\"a.png\")`, which answers "
    "whether that template is on screen right now, or one of the statements that "
    "reports a result (tap, tap_offset, swipe, tap_at, swipe_at, wait_until, "
    "run), which then runs and picks the branch: a failure is false, while a bare "
    "tap still fails the script. `return` / `return true` answer true, "
    "`return false` answers false, a script with no return answers true, and "
    "`run(...)` hands that answer back.\n"
    "  * A failing statement stops the script and comes back to you as an error. "
    "Statements that need a PNG (`tap`, `tap_offset`, `swipe`, `wait_until`) or a `.ccat` "
    "(`run`) fail when the file is missing; a `has(...)` whose template is missing is "
    "just false.\n"
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
    """Build a chat model for the selected provider.

    Key and base URL are passed explicitly, so neither has to be a name the
    integration already reads: `QWEN_API_KEY` is ours. A missing key is refused
    here, in a sentence that names the variable.

    `base_url` is the one spelling both clients accept. `api_base` is a
    ChatDeepSeek-only alias that ChatOpenAI drops without complaining, which
    would send Qwen's key to OpenAI's own endpoint instead.
    """
    provider = _provider()
    api_key = _env(provider["key_env"])
    if not api_key:
        raise RuntimeError(f"{provider['key_env']} is not set (put it in .env)")
    return init_chat_model(
        name,
        api_key=api_key,
        base_url=_setting(provider["base_url_env"], provider["base_url"]),
        max_tokens=max_tokens,
        extra_body=provider["extra_body"],
    )


def build_chat_model():
    """Build the chat model named by the environment.

    Raises RuntimeError when the API key is missing; the caller reports it as a
    failed result.
    """
    return _chat_model(
        _setting("CAMPCAT_LLM_MODEL", _provider()["model"]),
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
                 _setting("CAMPCAT_LLM_MODEL", _provider()["model"])),
        int(_env("CAMPCAT_LLM_MEMORY_MAX_TOKENS") or DEFAULT_MEMORY_MAX_TOKENS),
    )
