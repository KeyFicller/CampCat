"""Reasoning streams to the display and stays out of the conversation.

Qwen and DeepSeek both send `reasoning_content`, but `ChatOpenAI` drops it, so
the survivor is the subclass in `models.py`. From there it becomes a delta of
its own kind and reaches the UI as a thinking step.
"""

import graph
import models
import session
import transport
from langchain_core.messages import AIMessageChunk, HumanMessage


def _chunk(text="", reasoning=""):
    extra = {"reasoning_content": reasoning} if reasoning else {}
    return AIMessageChunk(content=text, additional_kwargs=extra)


def test_reasoning_text_reads_the_field_both_providers_send():
    assert session.reasoning_text(_chunk(reasoning="the button is")) == \
        "the button is"


def test_a_plain_chunk_has_no_reasoning():
    assert session.reasoning_text(_chunk("hello")) == ""


def test_chat_openai_keeps_the_reasoning_it_would_drop():
    model = models._ReasoningChatOpenAI(model="qwen3.7-max", api_key="k",
                                        base_url="http://localhost:1")
    raw = {"choices": [{"delta": {"reasoning_content": "look first"},
                        "index": 0}]}

    out = model._convert_chunk_to_generation_chunk(raw, AIMessageChunk, None)

    assert out.message.additional_kwargs["reasoning_content"] == "look first"


def test_chat_openai_still_converts_an_answer_chunk():
    model = models._ReasoningChatOpenAI(model="qwen3.7-max", api_key="k",
                                        base_url="http://localhost:1")
    raw = {"choices": [{"delta": {"content": "hi"}, "index": 0}]}

    out = model._convert_chunk_to_generation_chunk(raw, AIMessageChunk, None)

    assert out.message.content == "hi"
    assert "reasoning_content" not in out.message.additional_kwargs


class _Streaming:
    """A chat model stand-in that streams the given chunks."""

    def __init__(self, *chunks):
        self._chunks = chunks

    def stream(self, _messages):
        yield from self._chunks


def test_streamed_does_not_carry_reasoning_into_the_next_turn(monkeypatch):
    # Some providers reject the field they sent on the way back, and the next
    # turn has no use for it: it belongs to this call's display only.
    monkeypatch.setattr(transport, "stop_requested", lambda: False)
    replies = [_chunk(reasoning="think"), _chunk("answer")]

    reply = models.streamed(_Streaming(*replies), [HumanMessage(content="q")])

    assert reply.content == "answer"
    assert "reasoning_content" not in reply.additional_kwargs


def test_stream_turn_yields_reasoning_before_the_answer(monkeypatch):
    class _FakeGraph:
        def stream(self, _state, **_kwargs):
            for chunk in (
                AIMessageChunk(content="", additional_kwargs={
                    "reasoning_content": "first "}),
                AIMessageChunk(content="", additional_kwargs={
                    "reasoning_content": "second"}),
                _chunk("done"),
            ):
                yield "messages", (chunk, {"langgraph_node": "invoke"})

        def invoke(self, _state, config=None):
            return {}

    monkeypatch.setattr(graph, "_GRAPH", _FakeGraph())
    out_state = {}

    got = list(graph.stream_turn({}, out_state))

    assert got == [(False, "first "), (False, "second"), (True, "done")]
