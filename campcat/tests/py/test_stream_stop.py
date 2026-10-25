"""A stop from the host reaches a model call that is already on the wire.

The reader thread is what makes this possible: `model.stream` is the only place
the request path blocks for long, and nothing there reads stdin.

`_pump` is driven directly from an `io.StringIO` rather than started as a
thread, so each case is deterministic; `_QUEUE` and `_STOP_IDS` are drained
first, since the module keeps them for the life of the process.
"""

import io

import graph
import models
import pytest
import session
import tools
import transport
from langchain_core.messages import AIMessage, HumanMessage, SystemMessage

TAP = {
    "type": "function",
    "function": {
        "name": "tap",
        "description": "tap",
        "parameters": {"type": "object", "properties": {}},
    },
}


@pytest.fixture(autouse=True)
def _clean_transport():
    """Start every case with an empty queue, no recorded stops, no rid."""
    transport._PENDING.clear()
    transport._STOP_IDS.clear()
    transport.set_tool_wait(False)
    transport._CLOSED = False
    while not transport._QUEUE.empty():
        transport._QUEUE.get_nowait()
    transport.set_current_rid(None)
    yield
    transport._PENDING.clear()
    transport._STOP_IDS.clear()
    transport.set_tool_wait(False)
    transport._CLOSED = False
    while not transport._QUEUE.empty():
        transport._QUEUE.get_nowait()


def _pump(text):
    transport._pump(io.StringIO(text))


def test_a_stop_is_recorded_and_kept_from_the_request_loop():
    transport.set_current_rid(7)
    _pump('{"type":"interrupt","id":7}\n{"type":"tool_result","call_id":"x"}\n')

    assert transport.stop_requested()
    # Only the tool result reaches the request loop: an interrupt answered as
    # a request would also reset the provider selection.
    assert transport.read_message()["call_id"] == "x"


def test_a_stop_reaches_a_waiting_tool_call():
    transport.set_current_rid(7)
    transport.set_tool_wait(True)
    _pump('{"type":"interrupt","id":7}\n')

    msg = transport.read_message()
    assert msg["type"] == "interrupt"


def test_a_stop_for_another_request_is_not_this_one():
    transport.set_current_rid(8)
    _pump('{"type":"interrupt","id":7}\n')

    assert not transport.stop_requested()


def test_closed_stdin_ends_the_loop():
    _pump("")

    assert transport.read_message() is None
    assert transport.read_message() is None


def test_streamed_merges_the_chunks():
    class TwoChunks:
        def stream(self, messages):
            yield AIMessage(content="ab")

    reply = models.streamed(TwoChunks(), [HumanMessage(content="hi")])
    assert reply.content == "ab"


def test_streamed_stops_in_the_middle_of_a_reply(monkeypatch):
    pulled = []

    class Slow:
        def stream(self, messages):
            for piece in ("one ", "two ", "three"):
                pulled.append(piece)
                yield AIMessage(content=piece)

    stops = iter([False, False, True])
    monkeypatch.setattr(transport, "stop_requested", lambda: next(stops, True))

    with pytest.raises(transport.TurnInterrupted):
        models.streamed(Slow(), [HumanMessage(content="hi")])
    # The third chunk was never pulled: that is the saving.
    assert pulled == ["one ", "two "]


def test_streamed_asks_for_chinese_thinking_at_the_end_of_the_system_prompt():
    seen = {}

    class Recorder:
        def stream(self, messages):
            seen["messages"] = messages
            yield AIMessage(content="ok")

    original = [SystemMessage(content="你是操作员。"), HumanMessage(content="hi")]
    models.streamed(Recorder(), original)

    assert seen["messages"][0].content.endswith(session.THINK_IN_CHINESE)
    # The caller's list is the conversation: it must not keep the note.
    assert original[0].content == "你是操作员。"


def test_streamed_still_works_for_a_model_that_cannot_stream():
    class OnlyInvoke:
        def invoke(self, messages):
            return AIMessage(content="done")

    reply = models.streamed(OnlyInvoke(), [HumanMessage(content="hi")])
    assert reply.content == "done"


def test_a_push_back_interrupt_does_not_reach_the_request_loop():
    transport.set_current_rid(7)
    transport.push_back({"type": "interrupt", "id": 7})
    transport._QUEUE.put({"type": "ping", "id": 8})

    assert transport.read_message()["type"] == "ping"


def test_a_stop_while_generating_ends_the_turn_without_an_error(monkeypatch):
    session.reset()

    class Stopped:
        def bind_tools(self, tools, **kwargs):
            return self

        def stream(self, messages):
            yield AIMessage(content="thinking out loud")

    monkeypatch.setattr(models, "build_chat_model", lambda **_: Stopped())
    # First check passes (the turn starts), then the host's stop lands.
    stops = iter([False, True])
    monkeypatch.setattr(transport, "stop_requested", lambda: next(stops, True))

    out = {}
    text = "".join(
        chunk
        for is_answer, chunk in graph.stream_turn(
            {"text": "tap it", "tools": [TAP], "memory": ""}, out
        )
    )

    assert out.get("interrupted") is True
    assert not out.get("error")
    assert "thinking out loud" not in text
    # Recorded like any other turn, with no reply: the next request stays valid.
    assert out.get("turns") == 1


def test_tools_re_exports_the_transport_exception():
    assert tools.TurnInterrupted is transport.TurnInterrupted
