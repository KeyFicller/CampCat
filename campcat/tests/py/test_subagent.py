"""What the subagent window is fed, and where it stays quiet.

The operator's own turn must produce nothing here: its text already streams
through the parent graph, and its tool calls are already in its bubble.
"""

import pytest
from langchain_core.messages import AIMessage, AIMessageChunk, HumanMessage

import models
import subagent
import tools
import transport


def _events(sent):
    return [m for m in sent if m.get("type") == "subagent"]


def test_an_operator_tool_call_stays_quiet(monkeypatch):
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    subagent.event("tool", text="**tap** {}")
    subagent.chunk_sink(True, "hello")

    assert sent == []


def test_a_run_opens_and_closes(monkeypatch):
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    with subagent.working("scripter", "add an action") as run:
        run.settle("wrote stzb.map.claim")
        assert subagent.current.get() == "scripter"
    assert subagent.current.get() is None

    events = _events(sent)
    assert [m["event"] for m in events] == ["start", "end"]
    assert events[0]["agent"] == "scripter"
    assert events[0]["text"] == "add an action"
    assert events[1]["ok"] is True
    assert events[1]["text"] == "wrote stzb.map.claim"


def test_a_failure_still_closes_the_run(monkeypatch):
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    with pytest.raises(RuntimeError):
        with subagent.working("diagnoser", "stzb.map/city.png"):
            raise RuntimeError("model said no")

    end = _events(sent)[-1]
    assert end["event"] == "end"
    assert end["ok"] is False
    assert "model said no" in end["text"]
    assert subagent.current.get() is None


def test_nesting_restores_the_outer_agent(monkeypatch):
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    with subagent.working("scripter", "s"):
        with subagent.working("region", "r"):
            assert subagent.current.get() == "region"
        assert subagent.current.get() == "scripter"
    assert subagent.current.get() is None

    pairs = [(m["event"], m["agent"]) for m in _events(sent)]
    assert pairs[:2] == [("start", "scripter"), ("start", "region")]
    assert pairs[-1] == ("end", "scripter")


def test_deltas_are_text_or_thinking(monkeypatch):
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    with subagent.working("scripter"):
        subagent.chunk_sink(True, "let me ")
        subagent.chunk_sink(False, "hmm")

    assert [m["event"] for m in _events(sent)] == [
        "start", "text", "thinking", "end"]


def test_tool_calls_are_reported_inside_a_run(monkeypatch):
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    def fake_read():
        call = [m for m in sent if m["type"] == "tool_call"][-1]
        return {"type": "tool_result", "call_id": call["call_id"],
                "ok": False, "error": "no such bundle"}

    monkeypatch.setattr(transport, "read_message", fake_read)
    transport.set_current_rid(3)
    with subagent.working("scripter", "goal"):
        tools.call_cpp("save_script", {"name": "x"})

    events = _events(sent)
    tool = [m for m in events if m["event"] == "tool"][0]
    assert "save_script" in tool["text"]
    assert '"name": "x"' in tool["text"]
    end = [m for m in events if m["event"] == "tool_end"][0]
    assert end["ok"] is False
    assert "no such bundle" in end["text"]


def test_streamed_reports_every_delta():
    class Two:
        def stream(self, messages):
            yield AIMessageChunk(content="ab")
            yield AIMessageChunk(content="cd")

    seen = []
    models.streamed(Two(), [HumanMessage(content="hi")],
                    lambda is_answer, delta: seen.append((is_answer, delta)))

    assert seen == [(True, "ab"), (True, "cd")]


def test_the_trace_writes_whole_lines_tagged_with_who_thought_them(
        monkeypatch, capsys):
    # A smoke run has no window: its log is the only place the reasoning can be
    # read, and a line split across two deltas has to come out as one line.
    class Split:
        def stream(self, messages):
            yield AIMessageChunk(content="",
                                 additional_kwargs={"reasoning_content": "想一"})
            yield AIMessageChunk(content="",
                                 additional_kwargs={"reasoning_content": "想\n"})
            yield AIMessageChunk(content="开跑")

    monkeypatch.setenv("CAMPCAT_LLM_TRACE", "1")
    with subagent.working("scripter", "goal"):
        models.streamed(Split(), [HumanMessage(content="hi")])

    err = capsys.readouterr().err
    assert "[think][scripter] 想一想" in err
    assert "[say][scripter] 开跑" in err


def test_the_trace_prints_the_calls_a_reply_asked_for(monkeypatch, capsys):
    # A loop is the same call repeated; the narration alone never says which
    # call it kept making.
    class Calls:
        def stream(self, messages):
            yield AIMessageChunk(content="",
                                 tool_call_chunks=[{"name": "tap",
                                                    "args": '{"x": 1, "y": 2}',
                                                    "index": 0, "id": "a"}])

    monkeypatch.setenv("CAMPCAT_LLM_TRACE", "1")
    models.streamed(Calls(), [HumanMessage(content="hi")])

    err = capsys.readouterr().err
    assert '[call][operator] tap {"x": 1, "y": 2}' in err


def test_the_trace_keeps_reasoning_out_of_the_answer_when_no_newline_ends_it(
        monkeypatch, capsys):
    # The stream turns from reasoning to answer without a newline between them.
    class Turn:
        def stream(self, messages):
            yield AIMessageChunk(content="",
                                 additional_kwargs={"reasoning_content": "先想"})
            yield AIMessageChunk(content="再答")

    monkeypatch.setenv("CAMPCAT_LLM_TRACE", "1")
    models.streamed(Turn(), [HumanMessage(content="hi")])

    err = capsys.readouterr().err
    assert "[think][operator] 先想\n" in err and "先想再答" not in err


def test_a_line_is_flat_and_bounded():
    assert subagent.line("a\n  b") == "a b"
    assert len(subagent.line("x" * 500)) == subagent.LIMIT


def test_a_handoff_reports_the_scripter(monkeypatch, scripted):
    """The real tool bridge, so the events are the ones a run would send."""
    import graph
    import session

    session.reset()
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)

    def fake_read():
        call = [m for m in sent if m["type"] == "tool_call"][-1]
        return {"type": "tool_result", "call_id": call["call_id"],
                "ok": True, "text": "ok"}

    monkeypatch.setattr(transport, "read_message", fake_read)
    # Built once: `build_chat_model` is called every round, and a fresh model
    # per round restarts the script, which never reaches the last answer.
    model = scripted([
        AIMessage(content="", tool_calls=[{
            "name": "author_script", "args": {"goal": "签到"}, "id": "a"}]),
        AIMessage(content="", tool_calls=[{
            "name": "save_script", "args": {}, "id": "s"}]),
        AIMessage(content="saved the bundle"),
        AIMessage(content="Done."),
    ])
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    out = {}
    list(graph.stream_turn({"text": "write it", "tools": [], "memory": ""}, out))

    assert not out.get("error")
    events = _events(sent)
    pairs = [(m["event"], m["agent"]) for m in events]
    assert pairs[0] == ("start", "scripter")
    assert ("tool", "scripter") in pairs
    assert ("tool_end", "scripter") in pairs
    assert pairs[-1] == ("end", "scripter")
    assert "save_script" in " ".join(m.get("text", "") for m in events)
    assert subagent.current.get() is None
