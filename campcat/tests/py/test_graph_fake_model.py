"""The joint test: a LangChain fake model drives the real graph.

No network and no host -- `tools.call_cpp` is monkeypatched -- so this covers
`graph.py` and `session.py` end to end with only the model and the device
faked out.
"""

import graph
import runner
import models
import session
import tools
from langchain_core.language_models.fake_chat_models import (
    FakeMessagesListChatModel,
)
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage

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
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)

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
    # The turn is kept as it happened: the model's own call and the host's own
    # result, not a written-up line about them.
    kept = session.history()
    assert [c["name"] for m in kept if isinstance(m, AIMessage)
            for c in (m.tool_calls or [])] == ["tap"]
    assert [m.content for m in kept if isinstance(m, ToolMessage)] == ["tapped"]


def test_stock_fake_model_cannot_be_bound(monkeypatch):
    """Why `ScriptedModel` exists, kept as a guard on that reasoning.

    If a future langchain_core implements `bind_tools` on the fake, this test
    goes red -- and then the shim in conftest can go too.
    """
    session.reset()
    monkeypatch.setattr(models, "build_chat_model", lambda **_: FakeMessagesListChatModel(
        responses=[AIMessage(content="Done.")],
    ))

    out = {}
    list(graph.stream_turn({"text": "hi", "tools": [TAP_TOOL], "memory": ""}, out))

    # Empty exception text must still produce a failed host response.
    assert "error" in out
    assert out["error"] == "NotImplementedError"
    import main
    response = list(main.handle_request({"type": "turn", "id": "empty-error",
                                         "text": "hi", "tools": [TAP_TOOL]}))[-1]
    assert response["ok"] is False
    assert response["error"] == "NotImplementedError"


def test_the_third_identical_failure_is_refused():
    runner.reset_fail_counts()
    ran = []

    def run_one(call):
        ran.append(call["id"])
        return runner.ToolResult("error: adb busy", error_code="device_busy")

    call = {"name": "go_home", "args": {}, "id": "a"}
    for n in ("1", "2", "3"):
        results, _, interrupted = runner.run_calls(
            [dict(call, id=n)], run_one)
        assert not interrupted
    assert ran == ["1", "2"]
    assert results[0].content == runner.FUSE_TEXT
    # A different argument is a different call.
    runner.run_calls(
        [{"name": "go_home", "args": {"why": "other"}, "id": "4"}], run_one)
    assert ran == ["1", "2", "4"]


def test_a_success_resets_the_failure_streak():
    runner.reset_fail_counts()
    replies = iter(["error: adb busy", "ok", "error: adb busy", "error: adb busy"])
    ran = []

    def run_one(call):
        ran.append(call["id"])
        text = next(replies)
        code = "device_busy" if text == "error: adb busy" else ("invalid_args" if text.startswith("error:") else "")
        return runner.ToolResult(text, error_code=code)

    call = {"name": "go_home", "args": {}, "id": "a"}
    for n in ("1", "2", "3", "4", "5"):
        runner.run_calls([dict(call, id=n)], run_one)
    assert ran == ["1", "2", "3", "4"]


def test_a_different_error_does_not_trip_the_fuse():
    runner.reset_fail_counts()
    replies = iter(["error: adb busy", "error: box out of range", "error: box out of range"])
    ran = []

    def run_one(call):
        ran.append(1)
        text = next(replies)
        code = "device_busy" if text == "error: adb busy" else ("invalid_args" if text.startswith("error:") else "")
        return runner.ToolResult(text, error_code=code)

    call = {"name": "tap", "args": {"x": 1}, "id": "a"}
    for n in ("1", "2", "3"):
        runner.run_calls([dict(call, id=n)], run_one)
    assert ran == [1, 1, 1]


def test_failure_category_survives_wording_changes_and_trips_the_fuse(monkeypatch):
    runner.reset_fail_counts()
    replies = iter([
        {"ok": False, "error": "first explanation", "error_code": "device_busy"},
        {"ok": False, "error": "a completely different explanation", "error_code": "device_busy"},
    ])
    ran = []

    def call_cpp(name, args):
        ran.append(name)
        return next(replies)

    monkeypatch.setattr(tools, "call_cpp", call_cpp)
    call = {"name": "go_home", "args": {}, "id": "a"}
    for n in range(3):
        results, _, _ = runner.run_calls(
            [dict(call, id=str(n))],
            lambda item: runner.host_result(item["name"], item["args"]))
        assert results[0].status == "error"
        assert results[0].artifact["error_code"] == (
            "retry_exhausted" if n == 2 else "device_busy")
    assert ran == ["go_home", "go_home"]


def test_distinct_failure_categories_with_identical_text_do_not_share_a_streak(monkeypatch):
    runner.reset_fail_counts()
    replies = iter([
        {"ok": False, "error": "same visible explanation", "error_code": "device_busy"},
        {"ok": False, "error": "same visible explanation", "error_code": "invalid_args"},
        {"ok": False, "error": "same visible explanation", "error_code": "invalid_args"},
    ])
    ran = []

    def call_cpp(name, args):
        ran.append(name)
        return next(replies)

    monkeypatch.setattr(tools, "call_cpp", call_cpp)
    call = {"name": "go_home", "args": {}, "id": "a"}
    for n in range(3):
        runner.run_calls([dict(call, id=str(n))],
                         lambda item: runner.host_result(item["name"], item["args"]))
    assert ran == ["go_home", "go_home", "go_home"]


def test_successful_tool_text_starting_with_error_is_still_success(monkeypatch):
    runner.reset_fail_counts()
    ran = []

    def call_cpp(name, args):
        ran.append(name)
        return {"ok": True, "text": "error: is a literal in this script"}

    monkeypatch.setattr(tools, "call_cpp", call_cpp)
    call = {"name": "read_script", "args": {"name": "bundle"}, "id": "a"}
    for n in range(3):
        results, _, _ = runner.run_calls(
            [dict(call, id=str(n))],
            lambda item: runner.host_result(item["name"], item["args"]))
        assert results[0].status == "success"
        assert results[0].content == "error: is a literal in this script"
    assert len(ran) == 3


def test_a_subgraph_does_not_inherit_the_parent_callback(monkeypatch, scripted):
    from langchain_core.callbacks import BaseCallbackHandler
    from langchain_core.runnables.config import var_child_runnable_config
    from langgraph.graph import END, StateGraph

    class Count(BaseCallbackHandler):
        def __init__(self):
            super().__init__()
            self.n = 0

        def on_llm_end(self, *args, **kwargs):
            self.n += 1

    parent, child = Count(), Count()
    monkeypatch.setattr(
        models, "build_chat_model", lambda **_: scripted([AIMessage(content="ok")]))

    def node(state):
        models.build_chat_model().invoke([HumanMessage(content="hi")])
        return {}

    compiled = StateGraph(dict)
    compiled.add_node("n", node)
    compiled.set_entry_point("n")
    compiled.add_edge("n", END)
    token = var_child_runnable_config.set({"callbacks": [parent]})
    try:
        graph.run_subgraph(compiled.compile(), {}, [child])
    finally:
        var_child_runnable_config.reset(token)

    assert child.n == 1
    assert parent.n == 0
