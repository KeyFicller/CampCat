"""What the history keeps, and how compaction prices itself.

`session` holds the one conversation the sidecar serves, so every case resets it
first. No network: the compaction case drives the real middleware with the
`scripted` fake as its summary model.
"""

import models
import session
from langchain.agents.middleware import SummarizationMiddleware
from langchain_core.messages import (
    AIMessage,
    HumanMessage,
    ToolMessage,
)


def _turn(n: int) -> list:
    """One turn that called a tool and answered."""
    return [
        AIMessage(content="", tool_calls=[
            {"name": "tap", "args": {}, "id": f"c{n}"}]),
        ToolMessage(content=f"result {n}", tool_call_id=f"c{n}"),
        AIMessage(content=f"done {n}"),
    ]


def test_a_long_tool_result_survives_uncut():
    """The point of the change: the ledger's 80-character cut is gone."""
    session.reset()
    body = "x" * 5000
    session.append_turn("read it", [
        AIMessage(content="", tool_calls=[
            {"name": "read_script", "args": {"name": "a"}, "id": "c1"}]),
        ToolMessage(content=body, tool_call_id="c1"),
    ])

    assert session.history()[2].content == body


def test_a_picture_keeps_its_caption_and_loses_its_bytes():
    session.reset()
    session.append_turn("look", [
        HumanMessage(content=session._shot_content(
            "Screenshot you took: #6.", "A" * 4000)),
    ])

    kept = session.history()[1]
    # The frame number lives in the caption, which is why the caption stays.
    assert "#6" in kept.content[0]["text"]
    assert not any(p.get("type") == "image_url" for p in kept.content)


def test_tokens_anchors_on_what_the_provider_charged():
    charged = AIMessage("x", usage_metadata={
        "input_tokens": 100, "output_tokens": 20, "total_tokens": 120})

    # Nothing after it: the anchor is the whole answer, nothing estimated.
    assert session._tokens([charged]) == 120
    # Only the tail is approximated.
    tail = [HumanMessage("hi")]
    assert session._tokens([charged, *tail]) == 120 + session._tokens(tail)


def test_tokens_falls_back_before_any_usage_is_known():
    assert session._tokens([HumanMessage("hi")]) > 0


def test_the_trigger_line_default_clears_the_model_window(monkeypatch):
    monkeypatch.delenv("CAMPCAT_LLM_CONTEXT_TOKENS", raising=False)
    assert models.context_tokens() == 200000


def test_compaction_lowers_the_turn_count_and_keeps_calls_paired(scripted):
    session.reset()
    for n in range(3):
        session.append_turn(f"turn {n}", _turn(n))
    assert session.size() == 3

    session._MW = SummarizationMiddleware(
        model=scripted([AIMessage(content="earlier turns, summarized")]),
        trigger=("messages", 4),
        keep=("messages", 1),
    )
    session.compact()

    assert session.size() < 3
    # Whatever survived still answers every call it kept: a `tool_calls` block
    # with no result is what makes the provider reject the whole request.
    kept = session.history()
    answered = {m.tool_call_id for m in kept if isinstance(m, ToolMessage)}
    assert all(c["id"] in answered
               for m in kept if isinstance(m, AIMessage)
               for c in (m.tool_calls or []))
