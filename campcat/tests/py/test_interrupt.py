"""A new user message stops the tool loop and is kept in the history.

`call_cpp` is faked. Raising `TurnInterrupted` is what the host's `interrupt`
line does inside the real bridge.
"""

import graph
import models
import session
import tools
from conftest import ScriptedModel
from langchain_core.messages import AIMessage, ToolMessage

TAP = {
    "type": "function",
    "function": {
        "name": "tap",
        "description": "tap",
        "parameters": {"type": "object", "properties": {}},
    },
}

SHOT = {
    "type": "function",
    "function": {
        "name": "screenshot",
        "description": "screenshot",
        "parameters": {"type": "object", "properties": {}},
    },
}

AUTHOR = {
    "type": "function",
    "function": {
        "name": "author_script",
        "description": "write a script",
        "parameters": {"type": "object", "properties": {}},
    },
}


def _counting(responses):
    """One model, with a count of `invoke` calls.

    The list wraps around once spent, so the count is what shows the turn
    stopped: a later "Done." would otherwise come back as another answer.
    """
    calls = {"n": 0}

    class CountingModel(ScriptedModel):
        def invoke(self, model_input, config=None, **kwargs):
            calls["n"] += 1
            return super().invoke(model_input, config=config, **kwargs)

    return CountingModel(responses=responses), calls


def test_second_tool_stops_the_turn(monkeypatch):
    session.reset()
    seen = []

    def fake_call_cpp(name, args):
        if seen:
            raise tools.TurnInterrupted()
        seen.append(name)
        return {"ok": True, "text": "tapped"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    model, invokes = _counting([
        AIMessage(content="tapping", tool_calls=[
            {"name": "tap", "args": {}, "id": "c1"},
            {"name": "tap", "args": {}, "id": "c2"},
        ]),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)

    out = {}
    text = "".join(
        chunk
        for is_answer, chunk in graph.stream_turn(
            {"text": "tap twice", "tools": [TAP], "memory": ""}, out
        )
    )

    assert seen == ["tap"]
    assert "Done." not in text
    assert out.get("interrupted") is True
    assert invokes["n"] == 1
    kept = [m.content for m in session.history() if isinstance(m, ToolMessage)]
    assert kept == ["tapped", tools.INTERRUPTED_TEXT]


def test_scripter_interrupt_stops_the_operator(monkeypatch):
    session.reset()

    def fake_call_cpp(name, args):
        # The handoff lists bundles before the scripter model runs. The
        # interrupt under test is the scripter's own tool, not that listing.
        if name == "list_scripts":
            return {"ok": True, "text": "none yet"}
        raise tools.TurnInterrupted()

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    model, invokes = _counting([
        AIMessage(content="", tool_calls=[{
            "name": "author_script",
            "args": {"goal": "open the panel"},
            "id": "c1",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "screenshot",
            "args": {},
            "id": "s1",
        }]),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)

    out = {}
    list(graph.stream_turn(
        {"text": "write it", "tools": [AUTHOR, SHOT], "memory": ""}, out
    ))

    assert out.get("interrupted") is True
    assert not out.get("error")
    # Operator once, scripter once. "Done." would be a third call.
    assert invokes["n"] == 2
    kept = [m.content for m in session.history() if isinstance(m, ToolMessage)]
    assert kept == [tools.INTERRUPTED_TEXT]
    scripter_kept = [
        m.content for m in session.scripter_history() if isinstance(m, ToolMessage)
    ]
    assert scripter_kept == [tools.INTERRUPTED_TEXT]
