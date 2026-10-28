"""The operator hands a bundle off to the scripter, and only the result comes back.

One scripted model serves both agents: its replies are consumed in the order
the graph asks for them, operator round, then the scripter's rounds, then the
operator's next round.
"""

import graph
import models
import session
import tools
from langchain_core.messages import AIMessage, ToolMessage

# 8x8 red PNG. A fake payload makes the model's message normalizer hang.
_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP8z4AdMOEQH6QSAM1B"
    "AQ/oQeJvAAAAAElFTkSuQmCC"
)


def _call(name, args=None, call_id="c"):
    return {"name": name, "args": args or {}, "id": call_id}


def _drive(monkeypatch, scripted, responses):
    session.reset()
    calls = []

    def fake_call_cpp(name, args):
        calls.append(name)
        return {"ok": True, "text": name + " ok"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    model = scripted(responses)
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    out = {}
    text = "".join(
        chunk
        for is_answer, chunk in graph.stream_turn(
            {"text": "write it", "tools": [], "memory": ""}, out)
    )
    return calls, text, out


def _tool_messages():
    return [m for m in session.history() if isinstance(m, ToolMessage)]


def test_handoff_runs_scripter_tools_and_returns_text(monkeypatch, scripted):
    calls, text, out = _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="", tool_calls=[_call("save_script", {}, "s")]),
        AIMessage(content="", tool_calls=[_call("run_script", {}, "r")]),
        AIMessage(content="saved the bundle"),
        AIMessage(content="Done."),
    ])

    assert calls == ["list_scripts", "save_script", "run_script"]
    assert "Done." in text
    assert "saved the bundle" not in text
    assert not out.get("error")
    assert [c["name"] for m in session.history() if isinstance(m, AIMessage)
            for c in (m.tool_calls or [])] == ["author_script"]
    assert [m.content for m in _tool_messages()] == ["saved the bundle"]
    assert "goal: 签到" in " ".join(
        session.content_text(m.content) for m in session.scripter_history())


def test_handoff_carries_the_operator_steps(monkeypatch, scripted):
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {
                "goal": "签到",
                "steps": "- tap 政策: box 10,20 30x40"}, "a")]),
        AIMessage(content="saved the bundle"),
        AIMessage(content="Done."),
    ])

    seen = " ".join(
        session.content_text(m.content) for m in session.scripter_history())
    assert "- tap 政策: box 10,20 30x40" in seen


def test_handoff_steps_arrive_as_a_table(monkeypatch, scripted):
    """A row-shaped handoff, end to end: the tool schema reaches the scripter."""
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {
                "goal": "签到",
                "steps": [{"shot": 4, "screen": "任务页", "did": "点「签到」",
                           "region": "box 640,1295 160x100",
                           "expect": "白色「签到」两字", "result": "按钮变灰"}],
            }, "a")]),
        AIMessage(content="saved the bundle"),
        AIMessage(content="Done."),
    ])

    seen = " ".join(
        session.content_text(m.content) for m in session.scripter_history())
    assert ("1. #4 | 任务页 | 点「签到」 | box 640,1295 160x100 | "
            "白色「签到」两字 | 按钮变灰") in seen


def test_same_batch_keeps_tool_message_order(monkeypatch, scripted):
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("tap", {}, "t"),
            _call("author_script", {"goal": "签到"}, "a"),
        ]),
        AIMessage(content="saved x"),
        AIMessage(content="Done."),
    ])

    kept = _tool_messages()
    assert [m.tool_call_id for m in kept] == ["t", "a"]
    assert [m.content for m in kept] == ["tap ok", "saved x"]


def test_scripter_model_error_stays_in_the_tool_result(monkeypatch, scripted):
    session.reset()
    made = {"n": 0}

    def build(**_):
        made["n"] += 1
        if made["n"] == 1:
            return scripted([AIMessage(content="", tool_calls=[
                _call("author_script", {"goal": "签到"}, "a")])])
        if made["n"] == 2:
            raise RuntimeError("boom")
        return scripted([AIMessage(content="Done.")])

    monkeypatch.setattr(tools, "call_cpp", lambda name, args: {"ok": True, "text": "ok"})
    monkeypatch.setattr(models, "build_chat_model", build)
    out = {}
    list(graph.stream_turn({"text": "write it", "tools": [], "memory": ""}, out))

    assert "error" not in out
    assert _tool_messages()[0].content.startswith("error:")
    assert "boom" in _tool_messages()[0].content
    assert session.scripter_history() == []


def test_operator_only_refuses_everything_but_screenshot_and_tap(monkeypatch, scripted):
    # Not offered this turn, so these are the model calling from the prompt
    # alone. Refused rather than run: neither may start the scripter or drive
    # the device behind the sample.
    session.reset()
    calls = []

    def fake_call_cpp(name, args):
        calls.append(name)
        return {"ok": True, "text": name + " ok"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    model = scripted([
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a"),
            _call("run_script", {}, "r"),
        ]),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    out = {}
    list(graph.stream_turn(
        {"text": "write it", "tools": [], "memory": "", "operator_only": True},
        out))

    assert calls == []
    assert session.scripter_history() == []
    assert [m.content for m in _tool_messages()] == [
        "error: 这一轮只能用 go_home, screenshot, start_emulator, tap；"
        "不能用 author_script",
        "error: 这一轮只能用 go_home, screenshot, start_emulator, tap；"
        "不能用 run_script",
    ]


def test_operator_only_still_runs_what_it_keeps(monkeypatch, scripted):
    session.reset()
    calls = []

    def fake_call_cpp(name, args):
        calls.append(name)
        return {"ok": True, "text": name + " ok"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    model = scripted([
        AIMessage(content="", tool_calls=[
            _call("start_emulator", {}, "e"),
            _call("go_home", {}, "h"),
            _call("screenshot", {}, "s"),
            _call("tap", {}, "t"),
        ]),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    out = {}
    list(graph.stream_turn(
        {"text": "look and tap", "tools": [], "memory": "", "operator_only": True},
        out))

    assert calls == ["start_emulator", "go_home", "screenshot", "tap"]


def test_second_handoff_sees_the_first(monkeypatch, scripted):
    model = scripted([
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "one"}, "a1")]),
        AIMessage(content="saved one"),
        AIMessage(content="Done."),
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "two"}, "a2")]),
        AIMessage(content="updated one"),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(tools, "call_cpp", lambda name, args: {"ok": True, "text": "ok"})
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    session.reset()
    for _ in range(2):
        list(graph.stream_turn({"text": "write it", "tools": [], "memory": ""}, {}))

    kept = " ".join(
        session.content_text(m.content) for m in session.scripter_history())
    assert "saved one" in kept
    assert "goal: two" in kept


def test_empty_goal_skips_the_model(monkeypatch, scripted):
    made = {"n": 0}
    model = scripted([
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "  "}, "a")]),
        AIMessage(content="Done."),
    ])

    def build(**_):
        made["n"] += 1
        return model

    monkeypatch.setattr(tools, "call_cpp", lambda name, args: {"ok": True, "text": "ok"})
    monkeypatch.setattr(models, "build_chat_model", build)
    session.reset()
    list(graph.stream_turn({"text": "write it", "tools": [], "memory": ""}, {}))

    assert made["n"] == 2
    assert _tool_messages()[0].content == "error: goal 为空"


def test_scripter_reply_strips_dsml(monkeypatch, scripted):
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="<x DSML>call</x DSML>\n\nsaved ok"),
        AIMessage(content="Done."),
    ])

    assert _tool_messages()[0].content == "saved ok"


def test_the_finished_handoff_leaves_a_note_for_the_next_one(monkeypatch, scripted):
    import agent.reflector as reflector
    seen = []

    def spy(calls, operator_rounds, reply):
        seen.append(([c["name"] for c in calls], operator_rounds, reply))
        return "这次交接：1 次调用。", "下一手先读 bundle。"

    monkeypatch.setattr(reflector, "reflect", spy)
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="", tool_calls=[_call("save_script", {}, "s")]),
        AIMessage(content="saved ok"),
        AIMessage(content="Done."),
    ])

    assert seen == [(["save_script"], 0, "saved ok")]
    assert session.lessons() == ["下一手先读 bundle。"]
    # The numbers go back on this result instead: the operator's prompt is
    # built once per user turn, so a note there would never be updated.
    result = _tool_messages()[0].content
    assert result.startswith("saved ok")
    assert "这次交接：1 次调用。" in result
    assert "下一手先读 bundle。" in result


def test_the_note_reaches_the_next_scripter_prompt(monkeypatch, scripted):
    import agent.reflector as reflector
    import agent.scripter as scripter

    monkeypatch.setattr(reflector, "reflect",
                        lambda *a: ("", "别再看同一帧。"))
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="saved ok"),
        AIMessage(content="Done."),
    ])

    built = scripter._scripter_build({"text": "brief"})

    assert "别再看同一帧。" in built["messages"][0].content


def test_scripter_silent_after_tools(monkeypatch, scripted):
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="", tool_calls=[_call("save_script", {}, "s")]),
        AIMessage(content=""),
        AIMessage(content="Done."),
    ])

    assert _tool_messages()[0].content == (
        "failed: 写脚本的 agent 没有给出总结\nsave_script ok")


def test_scripter_author_script_goes_to_host(monkeypatch, scripted):
    calls, _, _ = _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "again"}, "b")]),
        AIMessage(content="failed no"),
        AIMessage(content="Done."),
    ])

    assert calls == ["list_scripts", "author_script"]


def test_scripter_final_text_drops_the_announcement(monkeypatch, scripted):
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="Let me read the bundle first.", tool_calls=[
            _call("save_script", {}, "s")]),
        AIMessage(content="saved\nWrote daily."),
        AIMessage(content="Done."),
    ])

    assert _tool_messages()[0].content == "saved\nWrote daily."


def test_an_announcement_does_not_hide_a_missing_summary(monkeypatch, scripted):
    _drive(monkeypatch, scripted, [
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="Saving now.", tool_calls=[
            _call("save_script", {}, "s")]),
        AIMessage(content=""),
        AIMessage(content="Done."),
    ])

    assert _tool_messages()[0].content == (
        "failed: 写脚本的 agent 没有给出总结\nsave_script ok")


def test_no_summary_ignores_an_earlier_handoff(monkeypatch, scripted):
    model = scripted([
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "one"}, "a1")]),
        AIMessage(content="", tool_calls=[_call("run_script", {}, "r")]),
        AIMessage(content="saved one"),
        AIMessage(content="Done."),
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "two"}, "a2")]),
        AIMessage(content=""),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(tools, "call_cpp",
                        lambda name, args: {"ok": True, "text": name + " ok"})
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    session.reset()
    list(graph.stream_turn({"text": "write it", "tools": [], "memory": ""}, {}))
    list(graph.stream_turn({"text": "again", "tools": [], "memory": ""}, {}))

    assert _tool_messages()[-1].content == "failed: 写脚本的 agent 没有给出总结"


def test_operator_turn_keeps_the_scripter_screenshot(monkeypatch, scripted):
    session.reset()

    def fake_call_cpp(name, args):
        if name == "screenshot":
            return {"ok": True, "text": "shot", "image_b64": _PNG, "shot": 2}
        return {"ok": True, "text": name + " ok"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    model = scripted([
        AIMessage(content="", tool_calls=[
            _call("author_script", {"goal": "签到"}, "a")]),
        AIMessage(content="", tool_calls=[_call("screenshot", {}, "s")]),
        AIMessage(content="saved it"),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    list(graph.stream_turn({"text": "write it", "tools": [], "memory": ""}, {}))

    kept = " ".join(session.content_text(m.content) for m in session.history())
    assert "#2" in kept
