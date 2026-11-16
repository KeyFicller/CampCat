"""The log as the graph really fills it.

The other cases call `log_*` directly; this one runs a whole turn through
`graph.stream_turn` so the three write points -- `build_messages`, `run_tools`,
`commit` -- are the ones under test.
"""

import graph
import models
import session
import tools
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage

TAP_TOOL = {
    "type": "function",
    "function": {
        "name": "tap",
        "description": "tap a template",
        "parameters": {"type": "object", "properties": {}},
    },
}


def test_a_turn_through_the_graph_lands_in_the_log(monkeypatch, scripted,
                                                   tmp_path):
    session.reset()
    path = str(tmp_path / "turns.jsonl")
    # What `main.py` does before running the graph.
    session.attach_log(path)

    def fake_call_cpp(name, args):
        return {"ok": True, "text": "tapped"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    # Built once: `invoke` asks for a model every round, and a fresh list each
    # time would hand back the tool call forever.
    model = scripted([
        AIMessage(content="", tool_calls=[{
            "name": "tap", "args": {"x": 1, "y": 2, "w": 3, "h": 4},
            "id": "c1"}]),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)

    out = {}
    list(graph.stream_turn(
        {"text": "tap it", "tools": [TAP_TOOL], "memory": ""}, out))

    # A restart: memory gone, the file still there.
    session._MESSAGES.clear()
    session._LOG.close()
    session._LOG = None
    session._LOG_PATH = ""
    session.attach_log(path)

    kept = session.history()
    assert [type(m) for m in kept] == [
        HumanMessage, AIMessage, ToolMessage, AIMessage]
    assert kept[0].content.startswith("tap it")
    assert [(c["name"], c["args"]) for c in kept[1].tool_calls] == [
        ("tap", {"x": 1, "y": 2, "w": 3, "h": 4})]
    assert kept[2].content == "tapped"
    assert kept[3].content == "Done."
    assert session.size() == 1
