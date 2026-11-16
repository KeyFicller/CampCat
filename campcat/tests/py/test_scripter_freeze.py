"""A handoff may not change bundles that already exist, unless it is a fix."""

import contextlib
import pytest

import agent.region_review as region_review
import graph
import runner
import observability
import session
import tools
import transport
import tool_policy
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
        ("save_script", {"name": "old"}, "old"),
        ("update_script", {"name": "old"}, "old"),
        ("delete_script", {"name": "old"}, "old"),
        ("rename_script", {"name": "old", "new_name": "fresh"}, "old"),
        ("rename_script", {"name": "fresh", "new_name": "old"}, "old"),
        ("save_template", {"bundle": "old", "name": "a.png"}, "old"),
        ("reuse_template", {"bundle": "old", "from_bundle": "source"}, "old"),
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
        return runner.ToolResult("ok")

    monkeypatch.setattr(runner, "host_result", fake)
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


def test_a_handoff_is_capped_below_the_turn(monkeypatch):
    monkeypatch.setattr(
        tools, "call_cpp", lambda *a, **k: {"ok": True, "text": "none yet"})
    seen = {}

    def run(compiled, state, callbacks=None):
        seen["cap"] = state["max_model_rounds"]
        return {"reply": "saved x"}

    monkeypatch.setattr(graph, "run_subgraph", run)
    assert scripter.run_scripter({"goal": "签到"}, [], 1000).content == "saved x"
    assert seen["cap"] == scripter.HANDOFF_MAX_ROUNDS
    assert scripter.run_scripter({"goal": "签到"}, [], 12).content == "saved x"
    assert seen["cap"] == 12


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
    assert scripter.run_scripter({"goal": "签到"}, []).content == "done"
    assert opened == [("author-script", "goal: 签到" + session.ASK_IN_CHINESE)]
    assert seen[0][2] is None


def test_a_failed_listing_does_not_start_the_scripter(monkeypatch):
    def fail_list(name, args):
        assert name == "list_scripts"
        return {"ok": False, "error": "down"}

    monkeypatch.setattr(tools, "call_cpp", fail_list)
    started = []
    monkeypatch.setattr(graph, "run_subgraph", lambda *a, **k: started.append(1))
    assert scripter.run_scripter({"goal": "签到"}, []).content == "error: down"
    assert started == []

    seen = []

    def record(name, args):
        seen.append(name)
        return {"ok": True, "text": "none yet"}

    monkeypatch.setattr(tools, "call_cpp", record)
    monkeypatch.setattr(graph, "run_subgraph", lambda *a, **k: {"reply": "saved x"})
    assert scripter.run_scripter({"goal": "签到", "fix": True}, []).content == "saved x"
    assert seen == ["list_scripts"]


def test_a_handoff_drops_tap_boxes_once_it_starts(monkeypatch):
    region_review._tap_boxes[(5, "k")] = {"x": 1}
    monkeypatch.setattr(
        tools, "call_cpp", lambda *a, **k: {"ok": False, "error": "down"})
    assert scripter.run_scripter({"goal": "签到"}, []).content == "error: down"
    assert region_review._tap_boxes[(5, "k")] == {"x": 1}

    def run(*_args, **_kwargs):
        assert region_review._tap_boxes[(5, "k")] == {"x": 1}
        return {"reply": "done"}

    monkeypatch.setattr(
        tools, "call_cpp", lambda *a, **k: {"ok": True, "text": "none yet"})
    monkeypatch.setattr(graph, "run_subgraph", run)
    assert scripter.run_scripter({"goal": "签到"}, []).content == "done"
    assert region_review._tap_boxes == {}


@pytest.mark.parametrize("repair", [{"fix": True}, {"last_error": "matcher failed"}])
@pytest.mark.parametrize("bundle", ["target", ""])
def test_repairs_only_unlock_the_explicit_bundle_and_send_host_scope(monkeypatch, repair, bundle):
    runner.reset_fail_counts()
    emitted = []
    monkeypatch.setattr(transport, "emit", emitted.append)
    monkeypatch.setattr(tools, "_wait_for_result", lambda call_id: {
        "ok": True, "text": "target -- first\nother -- second"
        if call_id.startswith("list_scripts-") else "ok"})

    def run(*args, **kwargs):
        state = {"messages": [AIMessage(content="", tool_calls=[
            {"name": "update_script", "args": {"name": "target"}, "id": "target"},
            {"name": "update_script", "args": {"name": "other"}, "id": "other"},
            {"name": "save_script", "args": {"name": "fresh"}, "id": "fresh"},
        ])]}
        out = scripter._apply_scripter_tools(state)
        results = _result_text(out)
        assert results[0].startswith("error:") is (not bool(bundle))
        assert results[1].startswith("error:")
        assert results[2] == "ok"
        return {"reply": "done"}

    monkeypatch.setattr(graph, "run_subgraph", run)
    assert scripter.run_scripter({"goal": "repair", "bundle": bundle, **repair}, []).content == "done"
    writes = [message for message in emitted
              if message.get("type") == "tool_call" and message["name"] != "list_scripts"]
    assert [message["args"]["name"] for message in writes] == (
        ["target", "fresh"] if bundle else ["fresh"])
    assert all(message["write_scope"] == (["target"] if bundle else [])
               for message in writes)

    # Scope must end with the handoff, including failed handoffs.
    tools.call_cpp("update_script", {"name": "outer"})
    assert emitted[-1]["write_scope"] is None


def test_failed_handoff_is_structured_and_recorded_as_failure():
    runner.reset_fail_counts()
    out = scripter._apply_scripter_tools({"messages": [AIMessage(content="", tool_calls=[{
        "name": "Handoff", "args": {"status": "failed", "summary": "无法完成"}, "id": "done",
    }])]})
    result = next(message for message in out["messages"] if isinstance(message, ToolMessage))
    assert result.content == "failed\n无法完成"
    assert result.status == "error"
    assert result.artifact["error_code"] == "handoff_failed"
    assert scripter.calls()[-1]["failed"]
    assert out["failure_code"] == "handoff_failed"


def test_write_scope_survives_real_graph_node_execution(monkeypatch, scripted):
    session.reset()
    emitted = []
    monkeypatch.setattr(transport, "emit", emitted.append)
    monkeypatch.setattr(tools, "_wait_for_result", lambda call_id: {
        "ok": True, "text": "target -- first\nother -- second"
        if call_id.startswith("list_scripts-") else "updated"})
    model = scripted([
        AIMessage(content="", tool_calls=[
            {"name": "update_script", "args": {"name": "target"}, "id": "a"},
            {"name": "update_script", "args": {"name": "other"}, "id": "b"},
        ]),
        AIMessage(content="", tool_calls=[{
            "name": "Handoff", "args": {"status": "updated"}, "id": "done",
        }]),
    ])
    monkeypatch.setattr(runner.models, "build_chat_model", lambda **kwargs: model)
    result = scripter.run_scripter({"goal": "repair", "bundle": "target", "fix": True}, [])
    assert result.content == "updated"
    assert not result.failed
    calls = [message for message in emitted if message.get("type") == "tool_call"]
    assert [(message["name"], message["write_scope"]) for message in calls] == [
        ("list_scripts", ["target"]), ("update_script", ["target"])]


@pytest.mark.parametrize("creation,args,bundle", [
    ("save_script", {"name": "fresh", "source": "return true"}, "fresh"),
    ("save_anchor", {"graph": "g", "anchor": "fresh"}, "g.fresh"),
])
@pytest.mark.parametrize("created", [True, False])
@pytest.mark.parametrize("repair", [True, False])
def test_new_bundle_authority_lasts_only_for_its_creating_handoff(
        monkeypatch, scripted, creation, args, bundle, created, repair):
    session.reset()
    emitted = []
    existing = {"repair-target"}
    monkeypatch.setattr(transport, "emit", emitted.append)

    def host(call_id):
        call = next(message for message in reversed(emitted) if message.get("type") == "tool_call")
        assert call["call_id"] == call_id
        name, arguments = call["name"], call["args"]
        if name == "list_scripts":
            return {"ok": True, "text": "\n".join(existing)}
        if any(target in existing and target not in call["write_scope"]
               for target in tool_policy.write_targets(name, arguments)):
            return {"ok": False, "error": "write outside scope", "error_code": "write_scope"}
        if name == creation:
            if not created:
                return {"ok": False, "error": "storage failure", "error_code": "storage_error"}
            existing.add(bundle)
        if name == "save_template" and bundle not in existing:
            return {"ok": False, "error": "missing bundle", "error_code": "invalid_args"}
        return {"ok": True, "text": "ok"}

    monkeypatch.setattr(tools, "_wait_for_result", host)
    model = scripted([
        AIMessage(content="", tool_calls=[{"name": creation, "args": args, "id": "create"}]),
        AIMessage(content="", tool_calls=[{
            "name": "save_template", "args": {"bundle": bundle, "name": "button.png"}, "id": "template",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "Handoff", "args": {"status": "saved" if created else "failed"}, "id": "done",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "save_template", "args": {"bundle": bundle, "name": "other.png"}, "id": "later",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "Handoff", "args": {"status": "failed"}, "id": "stop",
        }]),
    ])
    monkeypatch.setattr(runner.models, "build_chat_model", lambda **kwargs: model)
    repair_args = {"bundle": "repair-target", "fix": True} if repair else {}
    scripter.run_scripter({"goal": "create", **repair_args}, [])
    template_result = next(message for message in session.scripter_history()
                           if isinstance(message, ToolMessage) and message.tool_call_id == "template")
    assert template_result.status == ("success" if created else "error")
    calls = [message for message in emitted if message.get("type") == "tool_call"]
    assert calls[-1]["name"] == "save_template"
    allowed = (["repair-target"] if repair else []) + ([bundle] if created else [])
    assert calls[-1]["write_scope"] == sorted(allowed)

    if created:
        scripter.run_scripter({"goal": "another handoff"}, [])
        later = next(message for message in session.scripter_history()
                     if isinstance(message, ToolMessage) and message.tool_call_id == "later")
        assert later.status == "error"
        calls = [message for message in emitted if message.get("type") == "tool_call"]
        assert calls[-1]["name"] == "list_scripts"
        assert calls[-1]["write_scope"] == []


@pytest.mark.parametrize("renamed", [True, False])
def test_successful_rename_keeps_authority_at_its_new_name(monkeypatch, scripted, renamed):
    session.reset()
    emitted = []
    existing = set()
    monkeypatch.setattr(transport, "emit", emitted.append)

    def host(call_id):
        call = next(message for message in reversed(emitted) if message.get("type") == "tool_call")
        name = call["name"]
        if name == "list_scripts":
            return {"ok": True, "text": "none yet"}
        if any(target in existing and target not in call["write_scope"]
               for target in tool_policy.write_targets(name, call["args"])):
            return {"ok": False, "error": "outside scope", "error_code": "write_scope"}
        if name == "save_script":
            existing.add("fresh")
        elif name == "rename_script":
            if not renamed:
                return {"ok": False, "error": "rename failed", "error_code": "storage_error"}
            existing.remove("fresh")
            existing.add("renamed")
        elif name == "save_template" and "renamed" not in existing:
            return {"ok": False, "error": "missing bundle", "error_code": "invalid_args"}
        return {"ok": True, "text": "ok"}

    monkeypatch.setattr(tools, "_wait_for_result", host)
    model = scripted([
        AIMessage(content="", tool_calls=[{
            "name": "save_script", "args": {"name": "fresh"}, "id": "create",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "rename_script", "args": {"name": "fresh", "new_name": "renamed"}, "id": "rename",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "save_template", "args": {"bundle": "renamed", "name": "button.png"}, "id": "template",
        }]),
        AIMessage(content="", tool_calls=[{
            "name": "Handoff", "args": {"status": "saved" if renamed else "failed"}, "id": "done",
        }]),
    ])
    monkeypatch.setattr(runner.models, "build_chat_model", lambda **kwargs: model)
    scripter.run_scripter({"goal": "create and rename"}, [])
    result = next(message for message in session.scripter_history()
                  if isinstance(message, ToolMessage) and message.tool_call_id == "template")
    assert result.status == ("success" if renamed else "error")
    calls = [message for message in emitted if message.get("type") == "tool_call"]
    assert calls[-1]["write_scope"] == (["fresh", "renamed"] if renamed else ["fresh"])
