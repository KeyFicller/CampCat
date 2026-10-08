"""LangGraph flow for the sidecar's conversation.

Owns the conversation as a real message list. Older turns are summarized once
the request approaches `models.context_tokens()`, so the whole conversation
stays available to the model instead of being truncated by a fixed window.
"""

from __future__ import annotations

import functools
import sys
from typing import Iterator, TypedDict

from langchain.agents.middleware import SummarizationMiddleware
from langchain.agents.middleware.summarization import count_tokens_approximately
from langchain_core.messages import (
    AIMessage,
    HumanMessage,
    RemoveMessage,
    SystemMessage,
)
from langgraph.graph import END, StateGraph
from langgraph.runtime import Runtime

import models

# The sidecar only ever receives PNG, so the mime type is fixed here. Add a
# protocol field if the C++ side ever sends another format.
MIME = "image/png"
DEFAULT_PROMPT = "Describe this screenshot."


class DescribeState(TypedDict, total=False):
    image_b64: str  # request: this turn's screenshot, may be empty
    text: str       # request: this turn's text, may be empty
    messages: list  # built by build_messages
    reply: str      # response: model output
    turns: int      # response: turns retained in the conversation
    error: str


# Only the NDJSON read loop touches this, so it needs no lock.
_MESSAGES: list = []

# Built lazily: both the budget and the model come from the environment, which
# main.py loads from llm/.env before the first request. A summary lands in
# _MESSAGES as the first HumanMessage, never as a SystemMessage: the summarizer
# drops those, so build_messages prepends the prompt instead.
_MW = None


def reset_history() -> None:
    """Drop every turn. The next request starts a fresh conversation."""
    global _MW
    _MESSAGES.clear()
    _MW = None


def history_size() -> int:
    """Turns currently kept verbatim; a summary does not count as a turn."""
    return sum(1 for m in _MESSAGES if isinstance(m, AIMessage))


def _middleware():
    """Summarizes older turns once the request approaches the budget.

    `keep` is a quarter of the budget: it has to comfortably exceed one image
    (~950 tokens), or the verbatim window could retain no screenshots at all.
    """
    global _MW
    if _MW is None:
        counter = functools.partial(
            count_tokens_approximately, tokens_per_image=models.TOKENS_PER_IMAGE
        )
        budget = models.context_tokens()
        _MW = SummarizationMiddleware(
            model=models.build_chat_model(),
            trigger=("tokens", budget),
            keep=("tokens", budget // 4),
            token_counter=counter,
        )
    return _MW


def _content_to_text(content: object) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, str):
                parts.append(block)
            elif isinstance(block, dict) and block.get("type") == "text":
                parts.append(str(block.get("text", "")))
        return "".join(parts)
    return str(content)


def _turn_content(image_b64: str, text: str) -> object:
    """One turn's human content: a plain string when text-only, else blocks."""
    if not image_b64:
        return text
    blocks: list[dict] = []
    if text:
        blocks.append({"type": "text", "text": text})
    blocks.append(
        {"type": "image_url", "image_url": {"url": f"data:{MIME};base64,{image_b64}"}}
    )
    return blocks


def precheck(state: DescribeState) -> dict:
    """A turn needs a screenshot, text, or both; neither is an empty request.

    Whitespace-only text counts as empty, so a stray space in the input box
    never turns into a paid round trip.
    """
    text = (state.get("text") or "").strip()
    if not state.get("image_b64") and not text:
        return {"error": "empty request"}
    return {"text": text}


def compact(state: DescribeState) -> dict:
    """Fold the oldest turns into a rolling summary once past the budget.

    Summarizing rewrites the request prefix, which invalidates the provider's
    prompt cache from that point on, so the summarizer's own hysteresis is what
    keeps this rare. A failure here is skipped, never fatal: the request is
    still far below the model's real limit, so it goes out unsummarized.
    """
    if not _MESSAGES:
        return {}
    try:
        out = _middleware().before_model({"messages": list(_MESSAGES)}, Runtime())
    except Exception as exc:  # noqa: BLE001 - summarizing must not lose history
        print(f"[sidecar] summarization skipped: {exc}", file=sys.stderr)
        return {}
    if not out or "messages" not in out:
        return {}
    # Drop the RemoveMessage sentinel: it signals a reducer this list has none of.
    _MESSAGES[:] = [m for m in out["messages"] if not isinstance(m, RemoveMessage)]
    return {}


def build_messages(state: DescribeState) -> dict:
    """Assemble the model input from the conversation plus the current turn."""
    messages: list[object] = [SystemMessage(content=models.system_prompt())]
    messages.extend(_MESSAGES)
    messages.append(
        HumanMessage(
            content=_turn_content(
                state.get("image_b64", ""), state.get("text") or DEFAULT_PROMPT
            )
        )
    )
    return {"messages": messages}


def invoke(state: DescribeState) -> dict:
    try:
        model = models.build_chat_model()
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as a result
        return {"error": str(exc)}

    try:
        response = model.invoke(state.get("messages") or [])
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as a result
        return {"error": str(exc)}

    return {"reply": _content_to_text(getattr(response, "content", response))}


def commit(state: DescribeState) -> dict:
    """Record the turn on success.

    A failed turn is deliberately not recorded, so retrying cannot pollute the
    context with a broken exchange. Nothing is trimmed here: `compact` owns
    that, and only once the budget is reached.
    """
    if state.get("error"):
        return {"turns": history_size()}

    _MESSAGES.append(
        HumanMessage(
            content=_turn_content(state.get("image_b64", ""), state.get("text", ""))
        )
    )
    _MESSAGES.append(AIMessage(content=state.get("reply", "")))
    return {"turns": history_size()}


def route_after_precheck(state: DescribeState) -> str:
    return "end" if state.get("error") else "compact"


def build_graph():
    graph = StateGraph(DescribeState)
    graph.add_node("precheck", precheck)
    graph.add_node("compact", compact)
    graph.add_node("build_messages", build_messages)
    graph.add_node("invoke", invoke)
    graph.add_node("commit", commit)
    graph.set_entry_point("precheck")
    graph.add_conditional_edges(
        "precheck", route_after_precheck, {"compact": "compact", "end": END}
    )
    graph.add_edge("compact", "build_messages")
    graph.add_edge("build_messages", "invoke")
    graph.add_edge("invoke", "commit")
    graph.add_edge("commit", END)
    return graph.compile()


_GRAPH = None


def stream_describe(state: DescribeState,
                    out_state: dict) -> Iterator[tuple[bool, str]]:
    """Run the graph, yielding (is_answer, text) and copying the final state.

    (True, delta) is answer text to display; (False, "") is a reasoning
    heartbeat, meaning the model is thinking and has no answer yet. The final
    state lands in `out_state` for `error` and `turns`, which only exist once
    the run is over.
    """
    global _GRAPH
    if _GRAPH is None:
        _GRAPH = build_graph()
    for mode, payload in _GRAPH.stream(state, stream_mode=["messages", "values"]):
        if mode == "values":
            out_state.update(payload)
            continue
        chunk, meta = payload
        # Both the input echo (build_messages) and the summarizer (compact)
        # stream chunks too; only invoke is the answer.
        if meta.get("langgraph_node") != "invoke":
            continue
        text = _content_to_text(getattr(chunk, "content", ""))
        yield (bool(text), text)
