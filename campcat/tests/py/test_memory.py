"""Long-term notes: each agent folds only its own history."""

import agent.memorier as memorier
import agent.operator as operator
import agent.scripter as scripter
import graph
import models
import session
from langchain_core.messages import AIMessage


def test_operator_transcript_skips_scripter_and_scripter_skips_operator(monkeypatch):
    session.reset()
    session.append_turn("operator said hello", [AIMessage(content="ok")])
    session.append_scripter_turn("goal: x", [AIMessage(content="scripter secret handoff")])
    seen = []

    class _Model:
        def invoke(self, messages, config=None):
            seen.append(messages[-1].content)
            return AIMessage(content="## kept")

    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    assert memorier.memorize("", "operator") == "## kept"
    assert memorier.memorize("", "scripter") == "## kept"
    assert "operator said hello" in seen[0]
    assert "scripter secret handoff" not in seen[0]
    assert "scripter secret handoff" in seen[1]
    assert "operator said hello" not in seen[1]


def test_nothing_new_does_not_call_the_model(monkeypatch):
    session.reset()
    monkeypatch.setattr(models, "build_memory_model", lambda: (_ for _ in ()).throw(AssertionError("called")))
    assert memorier.memorize("notes", "operator") == "notes"
    assert memorier.memorize("notes", "scripter") == "notes"


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
    assert "## 偏好" in memorier.MEMORY_INSTRUCTIONS
    assert "## 叫法" in memorier.MEMORY_INSTRUCTIONS
    assert "Device & skills" not in memorier.MEMORY_INSTRUCTIONS
    assert "密码" in memorier.MEMORY_INSTRUCTIONS
    assert "## 坑" in memorier.SCRIPTER_MEMORY_INSTRUCTIONS
    assert "bundle" in memorier.SCRIPTER_MEMORY_INSTRUCTIONS
    assert memorier.scripter_memory_block("  ") == ""
    assert "你在之前会话里留下的坑" in memorier.scripter_memory_block("sign_in.png lies")


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
                  "input_len": built["input_len"]})
    assert session.history()[0].content == "看看现在什么界面" + session.ASK_IN_CHINESE
