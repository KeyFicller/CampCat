"""The joint test: a LangChain fake model drives the real graph.

No network and no host -- `tools.call_cpp` is monkeypatched -- so this covers
`graph.py` and `session.py` end to end with only the model and the device
faked out.
"""

import graph
import models
import session
import tools
from langchain_core.language_models.fake_chat_models import (
    FakeMessagesListChatModel,
)
from langchain_core.messages import AIMessage

# Shape only: ScriptedModel ignores what it is bound to. The schemas the host
# really sends are reflection-generated on the C++ side.
TAP_TOOL = {
    "type": "function",
    "function": {
        "name": "tap",
        "description": "tap a template",
        "parameters": {"type": "object", "properties": {}},
    },
}


def test_tool_round_then_answer(monkeypatch, scripted):
    session.reset()
    calls = []

    def fake_call_cpp(name, args):
        calls.append((name, args))
        return {"ok": True, "text": "tapped"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    # One model for the whole turn: `invoke` calls `build_chat_model` once per
    # round, and `FakeMessagesListChatModel` walks its list by an internal index,
    # so a fresh model per round would hand back the tool call forever.
    model = scripted([
        AIMessage(content="", tool_calls=[{
            "name": "tap",
            "args": {"x": 2407, "y": 142, "w": 77, "h": 99},
            "id": "c1",
        }]),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda: model)

    out = {}
    text = "".join(
        chunk
        for is_answer, chunk in graph.stream_turn(
            {"text": "tap it", "tools": [TAP_TOOL], "memory": ""}, out
        )
    )

    assert [c[0] for c in calls] == ["tap"]              # the tool ran once
    assert "Done." in text                               # the second round answered
    assert not out.get("error")
    assert "tap x=2407 y=142 w=77 h=99" in session.history()[-1].content


def test_stock_fake_model_cannot_be_bound(monkeypatch):
    """Why `ScriptedModel` exists, kept as a guard on that reasoning.

    If a future langchain_core implements `bind_tools` on the fake, this test
    goes red -- and then the shim in conftest can go too.
    """
    session.reset()
    monkeypatch.setattr(models, "build_chat_model", lambda: FakeMessagesListChatModel(
        responses=[AIMessage(content="Done.")],
    ))

    out = {}
    list(graph.stream_turn({"text": "hi", "tools": [TAP_TOOL], "memory": ""}, out))

    # The key, not its value: the fake raises `NotImplementedError()` with no
    # message, so `invoke` reports the empty string and the turn still fails.
    assert "error" in out
