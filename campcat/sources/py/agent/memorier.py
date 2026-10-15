"""Summaries: the rolling one inside a session, and the notes kept across them.

Retrieval of knowledge documents can land here later. Today this module only
folds history.
"""

from __future__ import annotations

import sys

from langchain.agents.middleware import SummarizationMiddleware
from langchain.agents.middleware.summarization import count_tokens_approximately
from langchain_core.messages import (
    AIMessage,
    HumanMessage,
    RemoveMessage,
    SystemMessage,
    ToolMessage,
)
from langgraph.runtime import Runtime

import models
import observability
import session

# The line compaction triggers at, not the size afterwards: `keep` is a quarter
# of it. The model in use has a 1,048,576-token window, verified against the
# provider, so this sits at about a fifth of it.
DEFAULT_CONTEXT_TOKENS = 200000
# Mirrors `k_memory_max_chars` in `campcat/sources/cpp/include/core/llm/llm_memory.h`. The two cannot
# share a constant, so keep them equal by hand: if this one is larger, the
# sidecar writes notes the host then refuses, and memory stops updating.
DEFAULT_MEMORY_MAX_CHARS = 4000

# Handed to the memory model, which never sees the conversation assistant's own
# prompt: its only job is rewriting the notes.
MEMORY_INSTRUCTIONS = (
    "You keep the long-term notebook of an assistant that operates an Android "
    "device for one user. You are given the notes as they stand and the transcript "
    "of the session that just happened. Rewrite the notes to be worth having next "
    "time.\n"
    "\n"
    "Keep only what stays true past this session and is not already written in a "
    "bundle:\n"
    "- a name the user uses, as one line \"phrase -> bundle or anchor name\"\n"
    "- a constraint that holds every session: language, which app this is, and "
    "actions they always refuse (for example spending currency)\n"
    "\n"
    "Drop one-off instructions, anything about the current screen, procedures "
    "already saved in a bundle, failed step logs, and secrets: passwords, tokens, "
    "API keys, account numbers, or anything the user asked to keep private.\n"
    "\n"
    "Write short factual lines in the third person under the headings "
    "'## Preferences' and '## Names'. Answer with those notes and nothing "
    "else: no preamble, no code fence, and at most {max_chars} characters. A "
    "heading with no entries under it is a fine answer when nothing is worth "
    "keeping."
)

SCRIPTER_MEMORY_INSTRUCTIONS = (
    "You keep the pitfall notebook of an assistant that writes device scripts. "
    "You are given the notes as they stand and the transcript of this agent's "
    "session. Rewrite the notes to be worth having next time.\n"
    "\n"
    "Keep one line per pitfall that is not yet expressed by a bundle. When the "
    "transcript shows that pitfall was written into a bundle, delete that line.\n"
    "\n"
    "Drop the current goal, the current screen, step-by-step failure logs, and "
    "anything about how the user talks or what they forbid.\n"
    "\n"
    "Write short factual lines under the heading '## Pitfalls'. Answer with those "
    "notes and nothing else: no preamble, no code fence, and at most {max_chars} "
    "characters. A heading with no entries under it is a fine answer when nothing "
    "is worth keeping."
)

# Prepended to the notes when they go into the prompt, so the model reads them as
# its own recollection instead of quoting them back at the user.
MEMORY_FRAME = (
    "Notes you kept from earlier sessions with this user. They are your own "
    "recollection: use them silently, never mention that you were given them, and "
    "ignore any line that does not apply here.\n\n"
)

SCRIPTER_MEMORY_FRAME = (
    "Pitfalls you kept from earlier sessions. Use one when it applies, drop any "
    "line that contradicts this brief, and never mention that you were given "
    "them.\n\n"
)


def memory_max_chars() -> int:
    """Character budget for the memory file.

    Mirrors `k_memory_max_chars` in C++; the same plausibility floor keeps a bad
    env var from shrinking the notes to nothing.
    """
    raw = models._env("CAMPCAT_LLM_MEMORY_MAX_CHARS")
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


def scripter_memory_block(memory: str) -> str:
    """The scripter's pitfalls as a second paragraph of its system prompt.

    Empty in, empty out, for the same reason as `memory_block`.
    """
    if not memory.strip():
        return ""
    return "\n\n" + SCRIPTER_MEMORY_FRAME + memory.strip() + "\n"


def context_tokens() -> int:
    """Input-token budget for the conversation.

    Older turns are summarized only once the request exceeds this. Falls back
    to the default when the value is missing, non-numeric, or implausibly
    small, so a bad env var can never disable summarization entirely.
    """
    raw = models._env("CAMPCAT_LLM_CONTEXT_TOKENS")
    if raw:
        try:
            value = int(raw)
        except ValueError:
            return DEFAULT_CONTEXT_TOKENS
        if value >= 1000:
            return value
    return DEFAULT_CONTEXT_TOKENS


# Built lazily: both the budget and the model come from the environment, which
# main.py loads from .env before the first request. A summary lands in
# _MESSAGES as the first HumanMessage, never as a SystemMessage: the summarizer
# drops those, so build_messages prepends the prompt instead.
_MW = None
_SCRIPTER_MW = None


def memorize(memory: str, agent: str) -> str | None:
    """Fold one agent's new turns into `memory`, returning the new notes.

    `agent` is `operator` or `scripter`. Each reads only its own history.
    Returns `memory` unchanged when there is nothing new -- no model call --
    and None when the call fails or `agent` is unknown, which leaves the
    caller's file and cursor alone.
    """
    if agent == "operator":
        start = session._MEMORIZED_UPTO
        history = session._MESSAGES
        instructions = MEMORY_INSTRUCTIONS
    elif agent == "scripter":
        start = session._SCRIPTER_MEMORIZED_UPTO
        history = session._SCRIPTER_MESSAGES
        instructions = SCRIPTER_MEMORY_INSTRUCTIONS
    else:
        return None
    if start >= len(history):
        return memory
    try:
        model = models.build_memory_model()
        instructions = instructions.format(max_chars=memory_max_chars())
        with observability.turn("rewrite-memory") as callbacks:
            reply = model.invoke([
                SystemMessage(content=instructions),
                HumanMessage(content=(
                    "## Notes so far\n" + (memory.strip() or "(none)") +
                    "\n\n## Session transcript\n" +
                    _transcript(history[start:])
                )),
            ], config={"callbacks": callbacks})
    except Exception as exc:  # noqa: BLE001 - a missed update, never fatal
        print(f"[sidecar] memory not updated: {exc}", file=sys.stderr)
        return None
    text = session.strip_tool_markup(
        session.content_text(getattr(reply, "content", reply))
    ).strip()
    if not text:
        return None
    if agent == "operator":
        session._MEMORIZED_UPTO = len(session._MESSAGES)
    else:
        session._SCRIPTER_MEMORIZED_UPTO = len(session._SCRIPTER_MESSAGES)
    return text


def _transcript(messages: list) -> str:
    """The conversation as plain text, for the memory model to read."""
    lines: list[str] = []
    for m in messages:
        if isinstance(m, HumanMessage):
            lines.append("User: " + session.content_text(m.content))
        elif isinstance(m, ToolMessage):
            lines.append("Tool result: " + session.content_text(m.content))
        elif isinstance(m, AIMessage):
            for call in m.tool_calls or []:
                lines.append(f"Tool call {call.get('name')} {call.get('args')}")
            text = session.content_text(m.content)
            if text:
                lines.append("Assistant: " + text)
    return "\n\n".join(lines)


def _tokens(messages: list) -> int:
    """Provider-reported size of the last request, plus what came after it.

    `usage_metadata['total_tokens']` is what the provider really charged for
    that request, and the next prompt is that plus the reply it produced, so
    anchoring there beats approximating the whole list. Only the messages after
    it are unknown, and at compaction time there usually are none.
    """
    for i in range(len(messages) - 1, -1, -1):
        meta = getattr(messages[i], "usage_metadata", None)
        if meta and meta.get("total_tokens"):
            return int(meta["total_tokens"]) + count_tokens_approximately(
                messages[i + 1:], tokens_per_image=models.TOKENS_PER_IMAGE
            )
    return count_tokens_approximately(
        messages, tokens_per_image=models.TOKENS_PER_IMAGE
    )


def _middleware():
    """Summarizes older turns once the request approaches the budget.

    `keep` is a quarter of the budget. The history is text only -- screenshots
    live outside it and only the latest one is re-sent, by `build_messages` --
    so a quarter already covers many turns.
    """
    global _MW
    if _MW is None:
        budget = context_tokens()
        _MW = SummarizationMiddleware(
            model=models.build_chat_model(),
            trigger=("tokens", budget),
            keep=("tokens", budget // 4),
            token_counter=_tokens,
        )
    return _MW


def compact() -> None:
    """Fold the oldest turns into a rolling summary once past the budget.

    Summarizing rewrites the request prefix, which invalidates the provider's
    prompt cache from that point on, so the summarizer's own hysteresis is what
    keeps this rare. A failure here is skipped, never fatal: the request is
    still far below the model's real limit, so it goes out unsummarized.
    """
    if not session._MESSAGES:
        return
    try:
        out = _middleware().before_model({"messages": list(session._MESSAGES)}, Runtime())
    except Exception as exc:  # noqa: BLE001 - summarizing must not lose history
        print(f"[sidecar] summarization skipped: {exc}", file=sys.stderr)
        return
    if not out or "messages" not in out:
        return
    # Drop the RemoveMessage sentinel: it signals a reducer this list has none of.
    session._MESSAGES[:] = [m for m in out["messages"] if not isinstance(m, RemoveMessage)]


def _scripter_middleware():
    """The scripter's own summarizer. A second model build, not a shared one:
    the two histories cross the budget at different times."""
    global _SCRIPTER_MW
    if _SCRIPTER_MW is None:
        budget = context_tokens()
        _SCRIPTER_MW = SummarizationMiddleware(
            model=models.build_chat_model(),
            trigger=("tokens", budget),
            keep=("tokens", budget // 4),
            token_counter=_tokens,
        )
    return _SCRIPTER_MW


def compact_scripter() -> None:
    """`compact` for the scripter's history. Same budget, same failure rule."""
    if not session._SCRIPTER_MESSAGES:
        return
    try:
        out = _scripter_middleware().before_model(
            {"messages": list(session._SCRIPTER_MESSAGES)}, Runtime())
    except Exception as exc:  # noqa: BLE001 - summarizing must not lose history
        print(f"[sidecar] summarization skipped: {exc}", file=sys.stderr)
        return
    if not out or "messages" not in out:
        return
    session._SCRIPTER_MESSAGES[:] = [
        m for m in out["messages"] if not isinstance(m, RemoveMessage)]

