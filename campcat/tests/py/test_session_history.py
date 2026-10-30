"""The turn's own messages, as the history keeps them.

`session` holds the one conversation the sidecar serves, so every case resets it
first.
"""

import agent.memorier as memorier
import agent.scripter as scripter
import graph
import models
import session
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage


def test_append_turn_keeps_the_turn_as_it_happened():
    session.reset()
    session.append_turn("tap it", [
        AIMessage(content="", tool_calls=[
            {"name": "tap", "args": {"x": 1}, "id": "c1"}]),
        ToolMessage(content="tapped", tool_call_id="c1"),
        AIMessage(content="answer"),
    ])

    kept = session.history()
    assert [type(m) for m in kept] == [
        HumanMessage, AIMessage, ToolMessage, AIMessage]
    # The call itself, not a line describing it.
    assert [(c["name"], c["args"]) for c in kept[1].tool_calls] == [
        ("tap", {"x": 1})]
    # The real result, not an 80-character cut of it.
    assert kept[2].content == "tapped"
    # The reply verbatim: no ledger prefix.
    assert kept[-1].content == "answer"
    assert session.size() == 1


def test_size_counts_turns_not_the_rounds_between_them():
    session.reset()
    for n in range(3):
        session.append_turn(f"turn {n}", [
            AIMessage(content="", tool_calls=[
                {"name": "tap", "args": {}, "id": f"c{n}"}]),
            ToolMessage(content=f"result {n}", tool_call_id=f"c{n}"),
            AIMessage(content=f"done {n}"),
        ])

    # Four messages per turn: the opener, the call, the result, the answer.
    assert len(session.history()) == 12
    assert session.size() == 3


def _texts(messages):
    return [session.content_text(m.content) for m in messages]


def test_scripter_history_is_separate():
    session.reset()
    session.append_turn("hi", [AIMessage(content="op")])
    session.append_scripter_turn("goal: 签到", [AIMessage(content="saved it")])

    assert "saved it" not in _texts(session.history())
    assert "hi" not in _texts(session.scripter_history())
    assert session.size() == 1


def test_reset_clears_scripter_history():
    session.reset()
    session.append_turn("hi", [AIMessage(content="op")])
    session.append_scripter_turn("goal: 签到", [AIMessage(content="saved it")])
    session.reset()

    assert session.history() == []
    assert session.scripter_history() == []

    memorier._MW = object()
    memorier._SCRIPTER_MW = object()
    session.reset()
    assert memorier._MW is None
    assert memorier._SCRIPTER_MW is None


def test_memorize_ignores_scripter_history(monkeypatch):
    session.reset()
    session.append_turn("operator said hello", [AIMessage(content="ok")])
    session.append_scripter_turn(
        "goal: x", [AIMessage(content="scripter secret handoff")])
    seen = {}

    class _Model:
        def invoke(self, messages, config=None):
            seen["text"] = messages[-1].content
            return AIMessage(content="## Preferences\nkept")

    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    notes = memorier.memorize("", "operator")
    assert notes == "## Preferences\nkept"
    assert "operator said hello" in seen["text"]
    assert "scripter secret handoff" not in seen["text"]


def test_a_failed_turn_keeps_the_rounds_that_finished():
    session.reset()
    mark = session.new_mark("turn")
    graph.commit({
        "text": "go",
        "turn_mark": mark,
        "error": "provider 500",
        "messages": [
            HumanMessage(content="go", id=mark),
            AIMessage(content="", tool_calls=[
                {"name": "tap", "args": {}, "id": "c1"}]),
            ToolMessage(content="tapped", tool_call_id="c1"),
            AIMessage(content="", tool_calls=[
                {"name": "screenshot", "args": {}, "id": "c2"}]),
        ],
    })

    kept = session.history()
    assert [type(m) for m in kept] == [
        HumanMessage, AIMessage, ToolMessage, AIMessage]
    assert kept[2].content == "tapped"
    assert kept[2].tool_call_id == kept[1].tool_calls[0]["id"] == "c1"
    assert "provider 500" in kept[-1].content
    assert "c2" not in [
        c["id"] for m in kept for c in (getattr(m, "tool_calls", None) or [])]


def test_a_failure_before_any_round_records_nothing():
    session.reset()
    mark = session.new_mark("turn")
    graph.commit({
        "text": "go",
        "turn_mark": mark,
        "error": "400",
        "messages": [HumanMessage(content="go", id=mark)],
    })

    assert session.history() == []


def test_a_request_drops_an_orphan_tool_result():
    session.reset()
    session.append_turn("hi", [
        ToolMessage(content="stray", tool_call_id="nope"),
        AIMessage(content="ok"),
    ])
    built = graph.build_messages({"text": "next", "memory": ""})

    assert "stray" not in [
        session.content_text(m.content) for m in built["messages"]]

    session.reset()
    session.append_scripter_turn("old", [
        ToolMessage(content="stray", tool_call_id="nope"),
    ])
    built = scripter._scripter_build({"text": "brief"})

    assert "stray" not in [
        session.content_text(m.content) for m in built["messages"]]


def test_append_scripter_turn_drops_image_bytes():
    session.reset()
    session.append_scripter_turn("goal: 签到", [HumanMessage(content=[
        {"type": "text", "text": "Screenshot #3 (100x100 pixels)."},
        {"type": "image_url", "image_url": {"url": "data:image/png;base64,AAAA"}},
    ])])

    kept = session.scripter_history()[-1]
    assert not any(isinstance(part, dict) and part.get("type") == "image_url"
                   for part in kept.content)
    assert "Screenshot #3" in session.content_text(kept.content)
