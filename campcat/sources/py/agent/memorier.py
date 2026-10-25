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
    "你替一个操作 Android 设备的助手维护长期笔记，服务对象只有一位用户。"
    "给你的内容是这个助手现有的笔记，以及刚结束那一轮会话的记录。"
    "把笔记重写成下一次真正用得上的样子。\n"
    "\n"
    "只留经过这一轮之后仍然成立、而且是 bundle 里还没写下的内容：\n"
    "- 用户用的某个叫法，一行写成「说法 -> bundle 或 anchor 名」\n"
    "- 每一轮都成立的约束：用什么语言、这是哪个 app、以及用户总是拒绝的操作"
    "（例如花掉货币）\n"
    "\n"
    "丢掉一次性的指令、关于当前界面的任何内容、已经存进 bundle 的流程、"
    "失败步骤的日志，以及机密：密码、token、API key、账号，"
    "或者用户要求保密的任何东西。\n"
    "\n"
    "在「## 偏好」和「## 叫法」两个标题下，用第三人称写简短的事实条目。"
    "只输出这些笔记，别的什么都不要：没有开场白、没有代码围栏，"
    "总长不超过 {max_chars} 个字符。没有任何值得留下的东西时，"
    "只留一个空标题也是可以的。"
)

SCRIPTER_MEMORY_INSTRUCTIONS = (
    "你替一个写设备脚本的助手维护踩坑笔记。给你的内容是这个助手现有的笔记，"
    "以及它这一轮会话的记录。把笔记重写成下一次真正用得上的样子。\n"
    "\n"
    "每条还没被某个 bundle 表达出来的坑留一行。记录显示某个坑已经写进 bundle "
    "了，就删掉那一行。\n"
    "\n"
    "丢掉当前目标、当前界面、逐步的失败日志，以及任何关于用户怎么说话、"
    "禁止了什么的内容。\n"
    "\n"
    "在「## 坑」这个标题下写简短的事实条目。只输出这些笔记，别的什么都不要："
    "没有开场白、没有代码围栏，总长不超过 {max_chars} 个字符。"
    "没有任何值得留下的东西时，只留一个空标题也是可以的。"
)

# Prepended to the notes when they go into the prompt, so the model reads them as
# its own recollection instead of quoting them back at the user.
MEMORY_FRAME = (
    "下面是你在之前和这位用户的会话里留下的笔记。它们是你自己的回忆："
    "默默用它们，绝不要提你收到过这些内容，和这里无关的行直接忽略。\n\n"
)

SCRIPTER_MEMORY_FRAME = (
    "下面是你在之前会话里留下的坑。用得上的时候就用，和这份 brief 冲突的行"
    "直接丢掉，绝不要提你收到过它们。\n\n"
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
                    "## 现有笔记\n" + (memory.strip() or "（无）") +
                    "\n\n## 本轮记录\n" +
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
            lines.append("用户：" + session.content_text(m.content))
        elif isinstance(m, ToolMessage):
            lines.append("工具结果：" + session.content_text(m.content))
        elif isinstance(m, AIMessage):
            for call in m.tool_calls or []:
                lines.append(f"工具调用 {call.get('name')} {call.get('args')}")
            text = session.content_text(m.content)
            if text:
                lines.append("助手：" + text)
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

