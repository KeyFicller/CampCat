"""`mcp_client`: the LangGraph loop and what it does with a script's question.

The transport is four headers and a JSON body, and the server rejects a wrong one
loudly, so it is not what breaks here. Two things are: an answer has to reach the
server as `inputResponses.elicit` on a retry of the same call, and the tool
results have to reach the model in the one order LangChain accepts.
"""

import mcp_client
from conftest import ScriptedModel
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage

_PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8AAAwAB/AF+pA7d"


def _asked(message="Is it?"):
    return {
        "resultType": "input_required",
        "inputRequests": {"elicit": {"params": {"message": message}}},
    }


def _completed(text="ran"):
    return {"content": [{"type": "text", "text": text}], "isError": False}


def _screen():
    return {"content": [{"type": "image", "mimeType": "image/png", "data": _PNG}]}


def _replay(monkeypatch, by_name):
    """Each `tools/call` answered from `by_name[name]`, one reply per call."""
    calls = []

    def post(method, params, name=None):
        calls.append((name, params))
        if method == "tools/list":
            return {"tools": []}
        return by_name[name].pop(0)

    monkeypatch.setattr(mcp_client, "post", post)
    return calls


def _model(monkeypatch, words):
    """A stand-in model whose answers are `words`, one per invoke."""
    said = iter(words)
    monkeypatch.setattr(
        mcp_client.models,
        "build_chat_model",
        lambda: _OneWord(next(said)),
    )


class _OneWord:
    """Answers every invoke with the one word it was built with."""

    def __init__(self, word):
        self.word = word

    def invoke(self, messages):
        return AIMessage(content=self.word)


def test_a_yes_is_carried_as_the_answer(monkeypatch):
    calls = _replay(
        monkeypatch, {"run_bundle": [_asked(), _completed()], "screenshot": [_screen()]}
    )
    _model(monkeypatch, ["yes"])

    text, _ = mcp_client.run_bundle("b")
    assert text == "ran"
    retry = calls[-1][1]["inputResponses"]["elicit"]
    assert retry == {"action": "accept", "content": {"answer": True}}


def test_a_no_is_carried_as_the_answer(monkeypatch):
    calls = _replay(
        monkeypatch, {"run_bundle": [_asked(), _completed()], "screenshot": [_screen()]}
    )
    _model(monkeypatch, ["No."])

    mcp_client.run_bundle("b")
    retry = calls[-1][1]["inputResponses"]["elicit"]
    assert retry["content"]["answer"] is False


def test_an_unsure_answer_cancels_the_run(monkeypatch):
    calls = _replay(
        monkeypatch,
        {
            "run_bundle": [_asked(), _completed("ask: cancelled")],
            "screenshot": [_screen()],
        },
    )
    _model(monkeypatch, ["unsure"])

    mcp_client.run_bundle("b")
    assert calls[-1][1]["inputResponses"]["elicit"] == {"action": "cancel"}


def test_two_questions_take_two_answers(monkeypatch):
    calls = _replay(
        monkeypatch,
        {
            "run_bundle": [_asked("First?"), _asked("Second?"), _completed()],
            "screenshot": [_screen(), _screen()],
        },
    )
    _model(monkeypatch, ["yes", "no"])

    mcp_client.run_bundle("b")
    assert calls[-1][1]["inputResponses"]["elicit"]["content"]["answer"] is False


def test_a_tool_result_precedes_its_picture(monkeypatch):
    """LangChain refuses anything wedged between tool calls and their results."""
    _replay(monkeypatch, {"screenshot": [_screen()]})
    call = {"name": "screenshot", "args": {}, "id": "1", "type": "tool_call"}
    out = mcp_client.act({"messages": [AIMessage(content="", tool_calls=[call])]})

    kinds = [type(message) for message in out["messages"]]
    assert kinds[0] is ToolMessage
    assert all(kind is HumanMessage for kind in kinds[1:])


def test_a_failed_tool_is_reported_as_its_result(monkeypatch):
    _replay(
        monkeypatch,
        {"read_graph": [{"content": [{"type": "text", "text": "no graph"}], "isError": True}]},
    )
    call = {"name": "read_graph", "args": {"name": "x"}, "id": "1", "type": "tool_call"}
    out = mcp_client.act({"messages": [AIMessage(content="", tool_calls=[call])]})

    assert out["messages"][0].content == "error: no graph"


def test_the_loop_ends_when_nothing_is_asked_for():
    assert mcp_client.route({"messages": [AIMessage(content="done")]}) == "__end__"


def test_the_model_is_not_offered_screenshot(monkeypatch):
    """A picture is the client's business: `_answer` pulls one when a script asks."""
    bound = []

    def post(method, params, name=None):
        assert method == "tools/list"
        return {
            "tools": [
                {"name": "locate", "description": "", "inputSchema": {}},
                {"name": "screenshot", "description": "", "inputSchema": {}},
            ]
        }

    class _Recording:
        def bind_tools(self, tools, **kwargs):
            bound.append(tools)
            return self

        def invoke(self, messages):
            return AIMessage(content="ok")

    monkeypatch.setattr(mcp_client, "post", post)
    monkeypatch.setattr(mcp_client.models, "build_chat_model", lambda: _Recording())
    mcp_client.agent({"messages": [HumanMessage(content="hi")]})

    assert [tool["function"]["name"] for tool in bound[0]] == ["locate"]


def test_a_second_turn_sees_the_first(monkeypatch):
    """The checkpointer, not a list of our own, is what makes it a conversation."""
    _replay(monkeypatch, {})
    model = ScriptedModel(responses=[AIMessage("first"), AIMessage("second")])
    monkeypatch.setattr(mcp_client.models, "build_chat_model", lambda: model)

    graph = mcp_client.build_graph()
    config = {"configurable": {"thread_id": "t"}}
    graph.invoke({"messages": [HumanMessage(content="one")]}, config)
    state = graph.invoke({"messages": [HumanMessage(content="two")]}, config)

    assert [message.content for message in state["messages"]] == [
        "one",
        "first",
        "two",
        "second",
    ]
