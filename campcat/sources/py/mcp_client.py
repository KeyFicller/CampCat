#!/usr/bin/env python3
"""A chat agent over CampCat's MCP face, as one small LangGraph.

    START -> agent -> act -> agent -> ... -> END

`agent` is the model with the server's own tools bound; `act` runs whatever it
asked for through the official MCP client, which also carries a script's
questions back to `answer`. The message list lives in a checkpointer, so a second
line typed at the prompt continues the same conversation.

Nothing here restates the library: the tool schemas come from `tools/list` and
the system prompt is the server's `instructions`, so a change on the server side
reaches this client without an edit.

Usage:
  ./build/campcat --mcp-http 8787 config.json &
  python3 campcat/sources/py/mcp_client.py

CAMPCAT_MCP_URL names another endpoint; the model and its key come from `.env`
through `models.py`, the same ones the sidecar uses.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import time
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Annotated, Any, TypedDict

import httpx2
from langchain_core.messages import (
    BaseMessage,
    HumanMessage,
    SystemMessage,
    ToolMessage,
)
from langgraph.checkpoint.memory import MemorySaver
from langgraph.graph import END, START, StateGraph
from langgraph.graph.message import add_messages
from mcp import Client, types
from mcp.client import ClientRequestContext
from mcp.client.streamable_http import streamable_http_client

import models

URL = os.environ.get("CAMPCAT_MCP_URL", "http://127.0.0.1:8787/mcp")
# The server is started by hand or by a launch config, so the first connect may
# arrive before it is listening.
WAIT_SECONDS = 60.0

# Kept off the model's tool list: a picture is ~1000 tokens and turns "where am
# I" into a look-around. `answer` pulls one when a script asks a question, so the
# tool stays on the wire; only the model cannot reach for it.
HIDDEN_FROM_MODEL = {"screenshot"}

# A bundle runs as long as its script does, so one request spans it. httpx's
# own default is a 5-second read, and a timeout there surfaces as a bare
# `CancelledError` from the transport's task group, not as `ReadTimeout`.
TIMEOUT = httpx2.Timeout(30.0, read=600.0)

# One question, one picture, one word -- the same bar the sidecar's `consult`
# holds a script's `ask` to. A wrong guess picks a wrong branch.
ASK_SYSTEM = (
    "You answer one yes/no question about an Android screenshot. Look only at "
    "the picture. Reply with exactly one word: yes, no, or unsure. Answer "
    "unsure when the picture does not show enough to decide; never guess."
)


def _refused(error: BaseException) -> bool:
    """A port nobody is listening on yet, however the transport wrapped it.

    The SDK raises from inside a task group, so the useful exception arrives
    inside an `ExceptionGroup` rather than on its own.
    """
    if isinstance(error, (httpx2.ConnectError, httpx2.ConnectTimeout)):
        return True
    return isinstance(error, BaseExceptionGroup) and all(
        _refused(inner) for inner in error.exceptions
    )


def _cause(error: BaseException) -> BaseException:
    """The first leaf of an `ExceptionGroup`, which is the one worth printing."""
    while isinstance(error, BaseExceptionGroup) and error.exceptions:
        error = error.exceptions[0]
    return error


@contextlib.asynccontextmanager
async def connect() -> AsyncIterator[Client]:
    """A client for the server, waiting for it to start listening."""
    deadline = time.monotonic() + WAIT_SECONDS
    told = False
    while True:
        try:
            # Loopback only, so the system proxy is never right here: a
            # Clash-style proxy answers a local port it cannot reach with 502.
            async with httpx2.AsyncClient(timeout=TIMEOUT, trust_env=False) as http:
                transport = streamable_http_client(URL, http_client=http)
                async with Client(transport, elicitation_callback=answer) as client:
                    yield client
                    return
        except Exception as error:  # noqa: BLE001 - only a refused port is retried
            if not _refused(error):
                raise
            if time.monotonic() >= deadline:
                raise SystemExit(f"{URL}: {_cause(error)}") from None
            if not told:
                print(f"waiting for {URL}", flush=True)
                told = True
            await asyncio.sleep(0.2)


def message_text(content: Any) -> str:
    """The words of a message whose content is either a string or blocks."""
    if isinstance(content, str):
        return content
    written = []
    for block in content or []:
        if isinstance(block, dict) and block.get("type") == "text":
            written.append(block.get("text", ""))
        elif isinstance(block, dict) and block.get("type") == "image_url":
            written.append("[picture]")
    return "\n".join(written)


def _written(blocks: list[types.ContentBlock]) -> str:
    written = []
    for block in blocks:
        if block.type == "text":
            written.append(block.text)
        else:
            written.append(f"[{block.type} {getattr(block, 'mime_type', '')}]")
    return "\n".join(written)


def _picture(block: types.ImageContent) -> dict:
    """One image block as the message content LangChain sends to the model."""
    return {
        "type": "image_url",
        "image_url": {
            "url": f"data:{block.mime_type};base64,{block.data}",
        },
    }


def _pictures(blocks: list[types.ContentBlock]) -> list[BaseMessage]:
    """Image blocks as picture messages, which follow every tool result.

    LangChain refuses a request that puts anything between an assistant's tool
    calls and their results, so these are appended once the whole batch is in.
    """
    pictures: list[BaseMessage] = []
    for block in blocks:
        if block.type != "image":
            pictures.append(
                HumanMessage(content=[{"type": "text", "text": "no picture came back"}])
            )
            continue
        pictures.append(
            HumanMessage(
                content=[
                    {"type": "text", "text": "the screen as it is now"},
                    _picture(block),
                ]
            )
        )
    return pictures


async def answer(
    context: ClientRequestContext, params: types.ElicitRequestParams
) -> types.ElicitResult:
    """The model's yes/no about the screen now; a cancel when it cannot tell."""
    screen = await context.session.call_tool("screenshot", {})
    content: list[dict] = [{"type": "text", "text": params.message}]
    for block in screen.content:
        if block.type == "image":
            content.insert(0, _picture(block))
    reply = await models.build_chat_model().ainvoke(
        [SystemMessage(content=ASK_SYSTEM), HumanMessage(content=content)]
    )
    word = message_text(reply.content).strip().strip(".").lower()
    if word not in ("yes", "no"):
        # A guess picks a branch the user may not want; a cancelled run says the
        # screen did not show enough, which is what happened.
        print(f"  ask: {params.message} -> unsure, cancelling", flush=True)
        return types.ElicitResult(action="cancel")
    print(f"  ask: {params.message} -> {word}", flush=True)
    return types.ElicitResult(action="accept", content={"answer": word == "yes"})


async def tool_specs(client: Client) -> list[dict]:
    """The server's tools as LangChain function schemas, minus the screenshot."""
    listed = await client.list_tools()
    return [
        {
            "type": "function",
            "function": {
                "name": tool.name,
                "description": tool.description or "",
                "parameters": tool.input_schema or {"type": "object"},
            },
        }
        for tool in listed.tools
        if tool.name not in HIDDEN_FROM_MODEL
    ]


async def execute(
    client: Client, name: str, args: dict
) -> tuple[str, list[types.ContentBlock]]:
    """One tool call on the server. A failure comes back as the tool's result."""
    result = await client.call_tool(name, args)
    text = _written(result.content)
    if result.is_error:
        text = "error: " + text
    return text, result.content


class Chat(TypedDict, total=False):
    messages: Annotated[list, add_messages]


async def act(client: Client, state: Chat) -> dict:
    """Every tool call on the last message, then the pictures they returned."""
    calls = getattr(state["messages"][-1], "tool_calls", None) or []
    results: list[BaseMessage] = []
    pictures: list[BaseMessage] = []
    for call in calls:
        name = call.get("name", "")
        args = call.get("args") or {}
        print(f"  tool: {name} {json.dumps(args, ensure_ascii=False)}", flush=True)
        try:
            text, blocks = await execute(client, name, args)
        except Exception as exc:  # noqa: BLE001 - the model gets to correct it
            text, blocks = f"error: {exc}", []
        results.append(ToolMessage(content=text, tool_call_id=call.get("id", "")))
        pictures.extend(_pictures(blocks))
    return {"messages": results + pictures}


def route(state: Chat) -> str:
    return "act" if getattr(state["messages"][-1], "tool_calls", None) else END


def build_graph(client: Client, offered: list[dict]):
    async def agent(state: Chat) -> dict:
        """One model round, with the server's tools on offer."""
        model = models.build_chat_model()
        if offered:
            model = model.bind_tools(offered)
        return {"messages": [await model.ainvoke(state["messages"])]}

    async def act_node(state: Chat) -> dict:
        return await act(client, state)

    graph = StateGraph(Chat)
    graph.add_node("agent", agent)
    graph.add_node("act", act_node)
    graph.add_edge(START, "agent")
    graph.add_conditional_edges("agent", route, {"act": "act", END: END})
    graph.add_edge("act", "agent")
    return graph.compile(checkpointer=MemorySaver())


async def main() -> int:
    models.load_env_file(Path(__file__).resolve().parents[3] / ".env")
    async with connect() as client:
        graph = build_graph(client, await tool_specs(client))
        config = {"configurable": {"thread_id": "cli"}}
        # The server's own guide, so this client never has to restate it.
        graph.update_state(
            config, {"messages": [SystemMessage(content=client.instructions or "")]}
        )
        print("CampCat MCP agent. 'exit' or Ctrl-D leaves.")
        while True:
            try:
                # Off the loop: the MCP session keeps running while a human types.
                line = (await asyncio.to_thread(input, "\n> ")).strip()
            except EOFError:
                print()
                return 0
            if line in ("exit", "quit"):
                return 0
            if not line:
                continue
            state = await graph.ainvoke(
                {"messages": [HumanMessage(content=line)]}, config
            )
            print("\n" + (message_text(state["messages"][-1].content) or "(no answer)"))


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
