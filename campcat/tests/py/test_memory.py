"""Long-term notes: each agent folds only its own history."""

import json

import pytest

import agent.memorier as memorier
import agent.operator as operator
import agent.scripter as scripter
import graph
import models
import main
import session
from langchain_core.messages import AIMessage

_OPS = '{"ops":[{"op":"add","section":"偏好","text":"用中文"}]}'


def test_operator_transcript_skips_scripter_and_scripter_skips_operator(monkeypatch):
    session.reset()
    session.append_turn("operator said hello", [AIMessage(content="ok")])
    session.append_scripter_turn("goal: x", [AIMessage(content="scripter secret handoff")])
    seen = []

    class _Model:
        def invoke(self, messages, config=None):
            seen.append(messages[-1].content)
            return AIMessage(content=_OPS)

    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    assert json.loads(memorier.memorize("", "operator"))["ops"]
    assert json.loads(memorier.memorize("", "scripter"))["ops"]
    assert "operator said hello" in seen[0]
    assert "scripter secret handoff" not in seen[0]
    assert "scripter secret handoff" in seen[1]
    assert "operator said hello" not in seen[1]


def test_nothing_new_does_not_call_the_model(monkeypatch):
    session.reset()
    monkeypatch.setattr(models, "build_memory_model", lambda: (_ for _ in ()).throw(AssertionError("called")))
    # Still a receipt: the host records that these turns are folded, so they are
    # not paid for twice.
    for agent in ("operator", "scripter"):
        payload = json.loads(memorier.memorize("notes", agent))
        assert payload["ops"] == []
        assert payload["to"] == ""


def test_the_pass_reports_which_turns_it_folded(monkeypatch):
    session.reset()
    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    session.append_turn("first", [AIMessage(content="ok")])
    session.append_turn("second", [AIMessage(content="ok")])
    payload = json.loads(memorier.memorize("", "operator"))
    marks = [m.id for m in session._MESSAGES if str(m.id).startswith("turn:")]
    assert payload["from"] == marks[0]
    assert payload["to"] == marks[-1]


@pytest.mark.parametrize("agent", ["operator", "scripter"])
def test_proposals_repeat_until_host_commits_their_turn_mark(monkeypatch, agent):
    session.reset()
    append = session.append_turn if agent == "operator" else session.append_scripter_turn
    append("first", [AIMessage(content="first reply")])
    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())

    first = json.loads(memorier.memorize("", agent))
    repeated = json.loads(memorier.memorize("", agent))
    history = session.history() if agent == "operator" else session.scripter_history()
    assert first == repeated
    assert first["from"] == first["to"] == history[0].id

    append("second", [AIMessage(content="second reply")])
    response = list(main.handle_request({
        "type": "memorize", "id": "memory", "agent": agent,
        "memory_upto": first["to"],
    }))[-1]
    assert response["ok"]
    second = json.loads(response["text"])
    history = session.history() if agent == "operator" else session.scripter_history()
    assert second["from"] == second["to"] == history[2].id


def test_compaction_cannot_skip_uncommitted_turns(monkeypatch):
    session.reset()
    session.append_turn("committed", [AIMessage(content="old reply")])
    session.append_turn("still pending", [AIMessage(content="pending reply")])
    session.set_memory_upto(session.history()[0].id)

    class Cutter:
        def before_model(self, state, runtime):
            return {"messages": state["messages"][2:]}

    monkeypatch.setattr(memorier, "_middleware", lambda: Cutter())
    memorier.compact()
    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    proposal = json.loads(memorier.memorize("", "operator"))
    assert proposal["ops"]
    assert proposal["to"] == session.history()[0].id


@pytest.mark.parametrize("agent", ["operator", "scripter"])
def test_previous_conversation_receipt_cannot_skip_a_new_turn(monkeypatch, agent):
    session.reset()
    append = session.append_turn if agent == "operator" else session.append_scripter_turn
    append("old conversation", [])
    old_mark = session.history(agent)[0].id
    session.reset()
    append("new conversation", [])
    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())

    result = list(main.handle_request({
        "type": "memorize", "id": "memory", "agent": agent, "memory_upto": old_mark,
    }))[-1]
    proposal = json.loads(result["text"])
    assert proposal["ops"]
    assert proposal["to"] == session.history(agent)[0].id
    assert proposal["to"] != old_mark


def test_a_reply_that_is_not_ops_nothing_is_written(monkeypatch):
    session.reset()
    session.append_turn("hi", [AIMessage(content="ok")])

    class _Proud:
        def invoke(self, messages, config=None):
            return AIMessage(content="## 偏好\n- 用中文回答")

    monkeypatch.setattr(models, "build_memory_model", lambda: _Proud())
    assert memorier.memorize("old", "operator") is None
    # The cursor stays put, so the next pass tries this turn again.
    assert session._MEMORIZED_UPTO == 0


def test_parse_ops_keeps_only_the_shape(monkeypatch):
    assert memorier._parse_ops("") is None
    assert memorier._parse_ops("nope") is None
    assert memorier._parse_ops("[]") is None
    assert memorier._parse_ops('{"ops":{}}') is None
    # A malformed container cannot be turned into a successful empty receipt.
    assert memorier._parse_ops('{"ops":[1,{"op":"add"}]}') is None
    # Object policy stays with the host, including operations it may reject.
    assert memorier._parse_ops('{"ops":[{"op":"unknown"}]}') == {"ops": [{"op": "unknown"}]}


def test_empty_reply_and_unknown_agent_leave_cursors(monkeypatch):
    session.reset()
    session.append_turn("hi", [AIMessage(content="ok")])
    session.append_scripter_turn("goal", [AIMessage(content="did")])

    class _Empty:
        def invoke(self, messages, config=None):
            return AIMessage(content="  ")

    monkeypatch.setattr(models, "build_memory_model", lambda: _Empty())
    assert memorier.memorize("old", "operator") is None
    assert memorier.memorize("old", "nope") is None
    assert session._MEMORIZED_UPTO == 0
    assert session._SCRIPTER_MEMORIZED_UPTO == 0


def test_instructions_name_the_sections():
    assert "偏好" in memorier.MEMORY_INSTRUCTIONS
    assert "叫法" in memorier.MEMORY_INSTRUCTIONS
    assert "Device & skills" not in memorier.MEMORY_INSTRUCTIONS
    assert "密码" in memorier.MEMORY_INSTRUCTIONS
    assert '"ops"' in memorier.MEMORY_INSTRUCTIONS
    assert "钉" in memorier.MEMORY_INSTRUCTIONS
    assert "坑" in memorier.SCRIPTER_MEMORY_INSTRUCTIONS
    assert "风格" in memorier.SCRIPTER_MEMORY_INSTRUCTIONS
    assert "bundle" in memorier.SCRIPTER_MEMORY_INSTRUCTIONS
    assert "{entry_chars}" in memorier.MEMORY_INSTRUCTIONS
    assert memorier.scripter_memory_block("  ") == ""
    assert "你在之前会话里留下的坑" in memorier.scripter_memory_block("sign_in.png lies")


class _Model:
    """A memory model that always proposes one fact."""

    def invoke(self, messages, config=None):
        return AIMessage(content=_OPS)


def test_empty_notes_leave_both_prompts_byte_identical():
    session.reset()
    built = graph.build_messages({"text": "hi", "memory": ""})
    assert built["messages"][0].content == operator.operator_prompt()
    built_s = scripter._scripter_build({"text": "brief"})
    assert built_s["messages"][0].content == scripter.scripter_prompt()


def test_each_prompt_sees_only_its_own_notes():
    session.reset()
    session.set_scripter_memory("sign_in.png lies")
    op = graph.build_messages({"text": "hi", "memory": "日常 -> app.daily_claim"})
    sc = scripter._scripter_build({"text": "brief"})
    assert "日常 -> app.daily_claim" in op["messages"][0].content
    assert "sign_in.png lies" not in op["messages"][0].content
    assert "sign_in.png lies" in sc["messages"][0].content
    assert "日常 -> app.daily_claim" not in sc["messages"][0].content


def test_the_chinese_note_is_stored_with_the_turn():
    """In history too, so the next request's prefix matches what was sent."""
    session.reset()
    state = graph.precheck({"text": "看看现在什么界面"})
    assert state["text"] == "看看现在什么界面" + session.ASK_IN_CHINESE
    built = graph.build_messages({**state, "memory": ""})
    assert built["messages"][-1].content == state["text"]
    graph.commit({**state, "messages": built["messages"],
                  "turn_mark": built["turn_mark"]})
    assert session.history()[0].content == "看看现在什么界面" + session.ASK_IN_CHINESE
