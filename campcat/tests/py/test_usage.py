"""Session counters the status bar reads. No network, no host."""

import graph
import session
import tools
import transport


class _Reply:
    """A stand-in for the model response. AIMessage refuses a partial usage."""

    def __init__(self, usage: dict | None) -> None:
        self.usage_metadata = usage


def test_input_tokens_set_context_and_a_later_gap_does_not():
    session.reset()
    session.note_llm(_Reply({"input_tokens": 42}), context=True)
    session.note_llm(_Reply({"total_tokens": 99}), context=True)
    assert session.usage_fields() == {"llm_calls": 2, "tool_calls": 0, "context": 42}


def test_non_conversation_call_does_not_replace_context():
    session.reset()
    session.note_llm(_Reply({"input_tokens": 42}), context=True)
    session.note_llm(_Reply({"input_tokens": 7}), context=False)
    assert session.usage_fields()["context"] == 42
    assert session.usage_fields()["llm_calls"] == 2


def test_a_missing_response_does_not_count():
    session.reset()
    session.note_llm(None, context=True)
    assert session.usage_fields() == {"llm_calls": 0, "tool_calls": 0}


def test_bool_is_not_an_input_token_count():
    session.reset()
    session.note_llm(_Reply({"input_tokens": True}), context=True)
    assert "context" not in session.usage_fields()
    assert session.usage_fields()["llm_calls"] == 1


def test_reset_drops_context():
    session.reset()
    session.note_llm(_Reply({"input_tokens": 42}), context=True)
    session.note_tool()
    session.reset()
    assert session.usage_fields() == {"llm_calls": 0, "tool_calls": 0}


def test_rounds_of_ignores_a_bool_and_uses_the_default():
    assert graph.rounds_of(True) == graph.MAX_MODEL_ROUNDS
    assert graph.rounds_of(20) == 20
    assert graph.rounds_of(None) == graph.MAX_MODEL_ROUNDS


def test_a_model_return_counts_and_records_input_tokens():
    """This LangChain ends a chat run with on_llm_end, not on_chat_model_end."""
    from langchain_core.language_models.chat_models import BaseChatModel
    from langchain_core.messages import AIMessage, HumanMessage
    from langchain_core.outputs import ChatGeneration, ChatResult

    import models

    class _OneShot(BaseChatModel):
        @property
        def _llm_type(self) -> str:
            return "one-shot"

        def _generate(self, messages, stop=None, run_manager=None, **kwargs):
            msg = AIMessage(
                content="ok",
                usage_metadata={
                    "input_tokens": 42,
                    "output_tokens": 3,
                    "total_tokens": 45,
                },
            )
            return ChatResult(generations=[ChatGeneration(message=msg)])

    session.reset()
    model = _OneShot()
    model.callbacks = [models._UsageHandler(context=True)]
    model.invoke([HumanMessage(content="hi")])
    assert session.usage_fields() == {"llm_calls": 1, "tool_calls": 0, "context": 42}


def test_call_cpp_counts_a_tool(monkeypatch):
    session.reset()
    transport.set_current_rid(1)
    emitted: list[dict] = []
    monkeypatch.setattr(transport, "emit", emitted.append)

    def reply():
        return {"type": "tool_result", "call_id": emitted[0]["call_id"],
                "ok": True, "text": "ok"}

    monkeypatch.setattr(transport, "read_message", reply)
    tools.call_cpp("tap", {})
    assert session.usage_fields()["tool_calls"] == 1
    assert emitted[0]["llm_calls"] == 0
    assert emitted[0]["tool_calls"] == 1
