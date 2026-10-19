#!/usr/bin/env python3
"""A chat agent over CampCat's MCP face, as one small LangGraph.

    START -> agent -> act -> agent -> ... -> END

`agent` is the model with the server's own tools bound; `act` runs whatever it
asked for over `POST /mcp`. The message list lives in a checkpointer, so a second
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

import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Annotated, Any, TypedDict

from langchain_core.messages import (
    BaseMessage,
    HumanMessage,
    SystemMessage,
    ToolMessage,
)
from langgraph.checkpoint.memory import MemorySaver
from langgraph.graph import END, START, StateGraph
from langgraph.graph.message import add_messages

import models

URL = os.environ.get("CAMPCAT_MCP_URL", "http://127.0.0.1:8787/mcp")
# Must match the kit's `kProtocolVersion`; it bumps with CppReflectKit.
PROTOCOL = "2026-07-28"
# The server is started by hand or by a launch config, so the first request may
# arrive before it is listening.
WAIT_SECONDS = 60.0
# Loopback only, so the system proxy is never right here: a Clash-style proxy
# answers a local port it cannot reach with 502, which reads as the server
# rejecting the request.
_OPEN = urllib.request.build_opener(urllib.request.ProxyHandler({}))

# Kept off the model's tool list: a picture is ~1000 tokens and turns "where am
# I" into a look-around. `_answer` pulls one over HTTP itself when a script asks
# a question, so the tool stays on the wire; only the model cannot reach for it.
HIDDEN_FROM_MODEL = {"screenshot"}

# One question, one picture, one word -- the same bar the sidecar's `consult`
# holds a script's `ask` to. A wrong guess picks a wrong branch.
ASK_SYSTEM = (
    "You answer one yes/no question about an Android screenshot. Look only at "
    "the picture. Reply with exactly one word: yes, no, or unsure. Answer "
    "unsure when the picture does not show enough to decide; never guess."
)


def post(method: str, params: dict, name: str | None = None) -> dict:
    """Sends one JSON-RPC request to the server and returns its `result`."""
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "Mcp-Method": method,
        "MCP-Protocol-Version": PROTOCOL,
    }
    if name is not None:
        # The server refuses a body whose name disagrees with this header.
        headers["Mcp-Name"] = name
    body = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
        "params": {
            **params,
            "_meta": {
                "io.modelcontextprotocol/protocolVersion": PROTOCOL,
                # Without this the server tells the script nobody can answer
                # its `ask`, rather than asking us.
                "io.modelcontextprotocol/clientCapabilities": {
                    "elicitation": {"form": {}}
                },
            },
        },
    }
    deadline = time.monotonic() + WAIT_SECONDS
    told = False
    while True:
        request = urllib.request.Request(
            URL, data=json.dumps(body).encode(), headers=headers
        )
        try:
            # A bundle runs as long as its script does, so one request spans it.
            with _OPEN.open(request, timeout=600) as reply:
                document = json.loads(reply.read())
            break
        except urllib.error.HTTPError as error:
            detail = json.loads(error.read() or b"{}").get("error", {})
            raise SystemExit(f"{detail.get('code')}: {detail.get('message')}") from None
        except urllib.error.URLError as error:
            if time.monotonic() >= deadline:
                raise SystemExit(f"{URL}: {error.reason}") from None
            if not told:
                print(f"waiting for {URL}", flush=True)
                told = True
            time.sleep(0.2)
    if "error" in document:
        error = document["error"]
        raise RuntimeError(f"{error.get('code')}: {error.get('message')}")
    return document["result"]


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


def _blocks(result: dict) -> list[dict]:
    return [block for block in result.get("content", []) if isinstance(block, dict)]


def _written(blocks: list[dict]) -> str:
    written = []
    for block in blocks:
        if block.get("type") == "text":
            written.append(block.get("text", ""))
        else:
            written.append(f"[{block.get('type')} {block.get('mimeType', '')}]")
    return "\n".join(written)


def _pictures(blocks: list[dict]) -> list[BaseMessage]:
    """Image blocks as picture messages, which follow every tool result.

    LangChain refuses a request that puts anything between an assistant's tool
    calls and their results, so these are appended once the whole batch is in.
    """
    pictures: list[BaseMessage] = []
    for block in blocks:
        if block.get("type") != "image" or not block.get("data"):
            pictures.append(
                HumanMessage(content=[{"type": "text", "text": "no picture came back"}])
            )
            continue
        pictures.append(
            HumanMessage(
                content=[
                    {"type": "text", "text": "the screen as it is now"},
                    {
                        "type": "image_url",
                        "image_url": {
                            "url": "data:"
                            + block.get("mimeType", "image/png")
                            + ";base64,"
                            + block["data"]
                        },
                    },
                ]
            )
        )
    return pictures


def _answer(question: str) -> bool | None:
    """The model's yes/no about the screen now; None when it cannot tell."""
    screen = post("tools/call", {"name": "screenshot", "arguments": {}}, "screenshot")
    content: list[dict] = [{"type": "text", "text": question}]
    for block in _blocks(screen):
        if block.get("type") == "image" and block.get("data"):
            content.insert(
                0,
                {
                    "type": "image_url",
                    "image_url": {
                        "url": "data:"
                        + block.get("mimeType", "image/png")
                        + ";base64,"
                        + block["data"]
                    },
                },
            )
    reply = models.build_chat_model().invoke(
        [SystemMessage(content=ASK_SYSTEM), HumanMessage(content=content)]
    )
    word = message_text(reply.content).strip().strip(".").lower()
    return {"yes": True, "no": False}.get(word)


def run_bundle(name: str) -> tuple[str, list[dict]]:
    """Plays a bundle, answering its questions from the screen until it ends."""
    carry: dict = {}
    while True:
        result = post(
            "tools/call",
            {"name": "run_bundle", "arguments": {"name": name}, **carry},
            "run_bundle",
        )
        blocks = _blocks(result)
        if result.get("resultType") != "input_required":
            return _written(blocks), blocks
        question = result["inputRequests"]["elicit"]["params"]["message"]
        answer = _answer(question)
        if answer is None:
            # A guess picks a branch the user may not want; a cancelled run says
            # the screen did not show enough, which is what happened.
            print(f"  ask: {question} -> unsure, cancelling", flush=True)
            carry = {"inputResponses": {"elicit": {"action": "cancel"}}}
        else:
            print(f"  ask: {question} -> {'yes' if answer else 'no'}", flush=True)
            carry = {
                "inputResponses": {
                    "elicit": {"action": "accept", "content": {"answer": answer}}
                }
            }


def execute(name: str, args: dict) -> tuple[str, list[dict]]:
    """One tool call on the server. A failure comes back as the tool's result."""
    if name == "run_bundle":
        return run_bundle(str(args.get("name", "")))
    result = post("tools/call", {"name": name, "arguments": args}, name)
    blocks = _blocks(result)
    text = _written(blocks)
    if result.get("isError"):
        text = "error: " + text
    return text, blocks


class Chat(TypedDict, total=False):
    messages: Annotated[list, add_messages]


def agent(state: Chat) -> dict:
    """One model round, with the server's tools on offer."""
    schemas = post("tools/list", {})["tools"]
    offered = [
        {
            "type": "function",
            "function": {
                "name": tool["name"],
                "description": tool.get("description", ""),
                "parameters": tool.get("inputSchema") or {"type": "object"},
            },
        }
        for tool in schemas
        if tool["name"] not in HIDDEN_FROM_MODEL
    ]
    model = models.build_chat_model()
    if offered:
        model = model.bind_tools(offered)
    return {"messages": [model.invoke(state["messages"])]}


def act(state: Chat) -> dict:
    """Every tool call on the last message, then the pictures they returned."""
    calls = getattr(state["messages"][-1], "tool_calls", None) or []
    results: list[BaseMessage] = []
    pictures: list[BaseMessage] = []
    for call in calls:
        name = call.get("name", "")
        args = call.get("args") or {}
        print(f"  tool: {name} {json.dumps(args, ensure_ascii=False)}", flush=True)
        try:
            text, blocks = execute(name, args)
        except Exception as exc:  # noqa: BLE001 - the model gets to correct it
            text, blocks = f"error: {exc}", []
        results.append(ToolMessage(content=text, tool_call_id=call.get("id", "")))
        pictures.extend(_pictures(blocks))
    return {"messages": results + pictures}


def route(state: Chat) -> str:
    return "act" if getattr(state["messages"][-1], "tool_calls", None) else END


def build_graph():
    graph = StateGraph(Chat)
    graph.add_node("agent", agent)
    graph.add_node("act", act)
    graph.add_edge(START, "agent")
    graph.add_conditional_edges("agent", route, {"act": "act", END: END})
    graph.add_edge("act", "agent")
    return graph.compile(checkpointer=MemorySaver())


def system_prompt() -> str:
    """The server's own guide, so this client never has to restate it."""
    return post("server/discover", {}).get("instructions", "")


def main() -> int:
    models.load_env_file(Path(__file__).resolve().parents[3] / ".env")
    graph = build_graph()
    config = {"configurable": {"thread_id": "cli"}}
    graph.update_state(
        config, {"messages": [SystemMessage(content=system_prompt())]}
    )
    print("CampCat MCP agent. 'exit' or Ctrl-D leaves.")
    while True:
        try:
            line = input("\n> ").strip()
        except EOFError:
            print()
            return 0
        if line in ("exit", "quit"):
            return 0
        if not line:
            continue
        state = graph.invoke({"messages": [HumanMessage(content=line)]}, config)
        print("\n" + (message_text(state["messages"][-1].content) or "(no answer)"))


if __name__ == "__main__":
    raise SystemExit(main())
