"""`mcp_client`: the LangGraph loop and what it does with a script's question.

The wire is the MCP SDK's now, so what is left to check here is ours: an answer
has to reach the server as an `ElicitResult`, and the tool results have to reach
the model in the one order LangChain accepts.
"""

import pytest
from conftest import ScriptedModel
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage
from mcp import types

import mcp_client

_PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8AAAwAB/AF+pA7d"


class _Client:
    """Stands in for `mcp.Client`: records calls, answers from `by_name`."""

    def __init__(self, by_name=None, tools=()):
        self.by_name = {
            name: list(replies) for name, replies in (by_name or {}).items()
        }
        self.tools = list(tools)
        self.calls = []

    async def list_tools(self):
        return types.ListToolsResult(tools=self.tools)

    async def call_tool(self, name, arguments=None):
        self.calls.append((name, arguments))
        return self.by_name[name].pop(0)


class _Context:
    """What `answer` reaches through the request context."""

    def __init__(self, client):
        self.session = client


class _OneWord:
    """Answers every model round with the one word it was built with."""

    def __init__(self, word):
        self.word = word

    async def ainvoke(self, messages):
        return AIMessage(content=self.word)


def _asked(message="Is it?"):
    return types.ElicitRequestFormParams(
        message=message, requested_schema={"type": "object"}
    )


def _completed(text="ran"):
    return types.CallToolResult(content=[types.TextContent(text=text)])


def _screen():
    return types.CallToolResult(
        content=[types.ImageContent(data=_PNG, mime_type="image/png")]
    )


def _model(monkeypatch, *words):
    said = iter(words)
    monkeypatch.setattr(
        mcp_client.models, "build_chat_model", lambda: _OneWord(next(said))
    )


@pytest.mark.anyio
async def test_a_yes_is_carried_as_the_answer(monkeypatch):
    client = _Client({"screenshot": [_screen()]})
    _model(monkeypatch, "yes")

    result = await mcp_client.answer(_Context(client), _asked())

    assert result.action == "accept"
    assert result.content == {"answer": True}


@pytest.mark.anyio
async def test_a_no_is_carried_as_the_answer(monkeypatch):
    client = _Client({"screenshot": [_screen()]})
    _model(monkeypatch, "No.")

    result = await mcp_client.answer(_Context(client), _asked())

    assert result.content == {"answer": False}


@pytest.mark.anyio
async def test_an_unsure_answer_cancels_the_run(monkeypatch):
    client = _Client({"screenshot": [_screen()]})
    _model(monkeypatch, "unsure")

    result = await mcp_client.answer(_Context(client), _asked())

    assert result.action == "cancel"


@pytest.mark.anyio
async def test_a_tool_result_precedes_its_picture(monkeypatch):
    """LangChain refuses anything wedged between tool calls and their results."""
    client = _Client({"screenshot": [_screen()]})
    call = {"name": "screenshot", "args": {}, "id": "1", "type": "tool_call"}

    state = {"messages": [AIMessage(content="", tool_calls=[call])]}
    out = await mcp_client.act(client, state)

    kinds = [type(message) for message in out["messages"]]
    assert kinds[0] is ToolMessage
    assert all(kind is HumanMessage for kind in kinds[1:])


@pytest.mark.anyio
async def test_a_failed_tool_is_reported_as_its_result(monkeypatch):
    client = _Client(
        {
            "read_graph": [
                types.CallToolResult(
                    content=[types.TextContent(text="no graph")], is_error=True
                )
            ]
        }
    )
    call = {"name": "read_graph", "args": {"name": "x"}, "id": "1", "type": "tool_call"}

    state = {"messages": [AIMessage(content="", tool_calls=[call])]}
    out = await mcp_client.act(client, state)

    assert out["messages"][0].content == "error: no graph"


def test_the_loop_ends_when_nothing_is_asked_for():
    assert mcp_client.route({"messages": [AIMessage(content="done")]}) == "__end__"


@pytest.mark.anyio
async def test_the_model_is_not_offered_screenshot():
    """A picture is the client's business: `answer` pulls one when a script asks."""
    client = _Client(
        tools=[
            types.Tool(name="locate", input_schema={}),
            types.Tool(name="screenshot", input_schema={}),
        ]
    )

    offered = await mcp_client.tool_specs(client)

    assert [tool["function"]["name"] for tool in offered] == ["locate"]


@pytest.mark.anyio
async def test_a_result_without_a_picture_says_so():
    pictures = mcp_client._pictures([types.TextContent(text="map")])

    assert pictures[0].content[0]["text"] == "no picture came back"


@pytest.mark.anyio
async def test_a_second_turn_sees_the_first(monkeypatch):
    """The checkpointer, not a list of our own, is what makes it a conversation."""
    model = ScriptedModel(responses=[AIMessage("first"), AIMessage("second")])
    monkeypatch.setattr(mcp_client.models, "build_chat_model", lambda: model)

    graph = mcp_client.build_graph(_Client(), [])
    config = {"configurable": {"thread_id": "t"}}
    await graph.ainvoke({"messages": [HumanMessage(content="one")]}, config)
    state = await graph.ainvoke({"messages": [HumanMessage(content="two")]}, config)

    assert [message.content for message in state["messages"]] == [
        "one",
        "first",
        "two",
        "second",
    ]
