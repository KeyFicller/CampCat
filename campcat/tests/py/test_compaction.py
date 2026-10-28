"""History that has been summarized, and the slice that ends a turn.

Two ways a request went out with a `tool` result that nothing asked for. The
provider rejects the whole request with a 400 naming the role, which reads as a
broken tool: one smoke run lost sixteen handoffs in a row to it.
"""

import agent.memorier as memorier
import graph
import session
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage


def _ask(call_id: str) -> AIMessage:
    return AIMessage(content="", tool_calls=[
        {"name": "screenshot", "args": {}, "id": call_id}])


def test_a_result_whose_question_was_cut_is_dropped():
    kept = memorier._without_orphan_tools([
        HumanMessage(content="summary of the earlier handoffs"),
        ToolMessage(content="ok", tool_call_id="a"),
        AIMessage(content="done"),
    ])

    assert [type(m) for m in kept] == [HumanMessage, AIMessage]


def test_a_result_with_its_question_above_it_stays():
    kept = memorier._without_orphan_tools([
        _ask("a"),
        ToolMessage(content="ok", tool_call_id="a"),
        _ask("b"),
        ToolMessage(content="ok", tool_call_id="b"),
    ])

    assert len(kept) == 4


def test_a_cut_between_two_batches_keeps_the_second_batch_whole():
    # The cut landed after `a`'s result, so `b`'s question and result survive
    # together -- and `a`'s question went with the summary.
    kept = memorier._without_orphan_tools([
        ToolMessage(content="ok", tool_call_id="a"),
        _ask("b"),
        ToolMessage(content="ok", tool_call_id="b"),
    ])

    assert len(kept) == 2
    assert isinstance(kept[0], AIMessage)
    assert isinstance(kept[1], ToolMessage)


def test_compaction_leaves_no_orphan_in_either_history(monkeypatch):
    class Cutter:
        """Compaction as the middleware does it: an old head replaced by a
        summary, cut wherever the boundary happens to fall."""

        def before_model(self, state, runtime):
            return {"messages": [HumanMessage(content="summary")] + state["messages"][2:]}

    monkeypatch.setattr(memorier, "_middleware", lambda: Cutter())
    monkeypatch.setattr(memorier, "_scripter_middleware", lambda: Cutter())
    session.reset()
    session._MESSAGES.extend([_ask("a"), ToolMessage(content="ok", tool_call_id="a"),
                              _ask("b"), ToolMessage(content="ok", tool_call_id="b")])
    session._SCRIPTER_MESSAGES.extend([
        _ask("c"), ToolMessage(content="ok", tool_call_id="c")])

    memorier.compact()
    memorier.compact_scripter()

    for messages in (session._MESSAGES, session._SCRIPTER_MESSAGES):
        assert not isinstance(messages[0], ToolMessage)
        asked = {call["id"] for m in messages
                 for call in (getattr(m, "tool_calls", None) or [])}
        assert all(m.tool_call_id in asked
                   for m in messages if isinstance(m, ToolMessage))


def test_the_turn_boundary_is_found_by_id_not_by_index():
    """`demote_pictures` drops stale frames as a turn goes on, so an index taken
    when the request was built points into a shorter list. Slicing there cut an
    assistant turn out and kept its tool results."""
    session.reset()
    session.start_turn()
    session.append_turn("旧的一轮", [AIMessage(content="ok"),
                                  HumanMessage(content="[图片未附上 #1]")])
    built = graph.build_messages({"text": "去设置页"})
    messages = list(built["messages"])
    del messages[1]  # what demote_pictures does to a stale frame
    messages += [_ask("a"), ToolMessage(content="ok", tool_call_id="a")]

    own = session.own_since(messages, built["turn_mark"])

    assert [type(m) for m in own] == [AIMessage, ToolMessage]
