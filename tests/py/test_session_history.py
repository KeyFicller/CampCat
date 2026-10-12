"""The turn's own messages, as the history keeps them.

`session` holds the one conversation the sidecar serves, so every case resets it
first.
"""

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
