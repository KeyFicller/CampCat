"""A handoff may not change bundles that already exist, unless it is a fix."""

import contextlib

import graph
import observability
import session
import tools
from langchain_core.messages import AIMessage, ToolMessage

import agent.scripter as scripter


def test_only_a_real_fix_flag_or_an_error_unlocks():
    assert scripter.handoff_may_change({}) is False
    assert scripter.handoff_may_change({"fix": True}) is True
    assert scripter.handoff_may_change({"fix": "true"}) is False
    assert scripter.handoff_may_change({"last_error": " best=0.4 "}) is True
    assert scripter.handoff_may_change({"last_error": "  "}) is False


def test_listing_names_are_the_text_before_the_separator():
    assert scripter.bundle_names_of("none yet") == set()
    assert scripter.bundle_names_of(
        "g.a -- 页 [anchor of g, page]\nold -- keep") == {"g.a", "old"}


def test_frozen_bundles_are_refused():
    frozen = {"old", "g.a"}
    cases = [
        ("update_script", {"name": "old"}, "old"),
        ("delete_script", {"name": "old"}, "old"),
        ("rename_script", {"name": "old", "new_name": "fresh"}, "old"),
        ("save_template", {"bundle": "old", "name": "a.png"}, "old"),
        ("save_anchor", {"graph": "g", "anchor": "a"}, "g.a"),
    ]
    for tool, args, target in cases:
        assert scripter.refuse_existing(tool, args, frozen) == (
            f"error: {target} 已存在；这次交接不能改它。要改它得让交接带上 fix: true，"
            "请把这一句写进你的总结。")


def test_a_new_name_and_a_missing_field_pass():
    assert scripter.refuse_existing("update_script", {"name": "fresh"}, {"old"}) is None
    assert scripter.refuse_existing("update_script", {}, {"old"}) is None
    assert scripter.refuse_existing("run_script", {"name": "old"}, {"old"}) is None


def _update(name):
    return {"messages": [AIMessage(content="", tool_calls=[{
        "name": "update_script", "args": {"name": name}, "id": "c",
    }])], "tool_rounds": 0}


def _result_text(out):
    return [m.content for m in out["messages"] if isinstance(m, ToolMessage)]


def test_the_gate_stops_the_host(monkeypatch):
    calls = []

    def fake(name, args):
        calls.append(name)
        return "ok", []

    monkeypatch.setattr(graph, "_host_result", fake)
    token = scripter.arm_freeze(frozenset({"old"}))
    try:
        out = scripter._apply_scripter_tools(_update("old"))
    finally:
        scripter.disarm_freeze(token)
    assert calls == []
    assert _result_text(out) == [
        "error: old 已存在；这次交接不能改它。要改它得让交接带上 fix: true，"
        "请把这一句写进你的总结。"]

    token = scripter.arm_freeze(None)
    try:
        scripter._apply_scripter_tools(_update("old"))
    finally:
        scripter.disarm_freeze(token)
    assert calls == ["update_script"]


def test_a_handoff_opens_its_own_span(monkeypatch):
    opened = []

    @contextlib.contextmanager
    def fake_span(name, input_text=""):
        opened.append((name, input_text))
        yield [], None

    monkeypatch.setattr(observability, "span", fake_span)
    monkeypatch.setattr(
        tools, "call_cpp", lambda *a, **k: {"ok": True, "text": "none yet"})
    seen = []

    def run(*args, **kwargs):
        seen.append(args)
        return {"reply": "done"}

    monkeypatch.setattr(graph, "run_subgraph", run)
    assert scripter.run_scripter({"goal": "签到"}, []) == "done"
    assert opened == [("author-script", "goal: 签到" + session.ASK_IN_CHINESE)]
    assert seen[0][2] is None


def test_a_failed_listing_does_not_start_the_scripter(monkeypatch):
    def fail_list(name, args):
        assert name == "list_scripts"
        return {"ok": False, "error": "down"}

    monkeypatch.setattr(tools, "call_cpp", fail_list)
    started = []
    monkeypatch.setattr(graph, "run_subgraph", lambda *a, **k: started.append(1))
    assert scripter.run_scripter({"goal": "签到"}, []) == "error: down"
    assert started == []

    seen = []

    def record(name, args):
        seen.append(name)
        return {"ok": True, "text": "none yet"}

    monkeypatch.setattr(tools, "call_cpp", record)
    monkeypatch.setattr(graph, "run_subgraph", lambda *a, **k: {"reply": "saved x"})
    assert scripter.run_scripter({"goal": "签到", "fix": True}, []) == "saved x"
    assert seen == []
