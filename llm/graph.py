"""LangGraph flow for the one-shot screenshot describe request.

Three nodes: precheck -> invoke -> finalize. A conditional edge sends an
invalid request straight to the end. This is deliberately linear today; it is
the extension point for a tool loop later.
"""

from __future__ import annotations

from typing import TypedDict

from langchain_core.messages import HumanMessage, SystemMessage
from langgraph.graph import END, StateGraph

import models

# The sidecar only ever receives PNG, so the mime type is fixed here. Add a
# protocol field if the C++ side ever sends another format.
MIME = "image/png"
USER_PROMPT = "Describe this screenshot."


class DescribeState(TypedDict, total=False):
    image_b64: str
    text: str
    error: str


def _content_to_text(content: object) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, str):
                parts.append(block)
            elif isinstance(block, dict) and block.get("type") == "text":
                parts.append(str(block.get("text", "")))
        return "".join(parts)
    return str(content)


def precheck(state: DescribeState) -> DescribeState:
    if not state.get("image_b64"):
        return {"error": "empty image"}
    return {}


def invoke(state: DescribeState) -> DescribeState:
    try:
        model = models.build_chat_model()
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as a result
        return {"error": str(exc)}

    data_url = f"data:{MIME};base64,{state.get('image_b64', '')}"
    content = [
        {"type": "text", "text": USER_PROMPT},
        {"type": "image_url", "image_url": {"url": data_url}},
    ]
    messages = [
        SystemMessage(content=models.system_prompt()),
        HumanMessage(content=content),
    ]

    try:
        response = model.invoke(messages)
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as a result
        return {"error": str(exc)}

    return {"text": _content_to_text(getattr(response, "content", response))}


def finalize(state: DescribeState) -> DescribeState:
    return state


def route_after_precheck(state: DescribeState) -> str:
    return "end" if state.get("error") else "invoke"


def build_graph():
    graph = StateGraph(DescribeState)
    graph.add_node("precheck", precheck)
    graph.add_node("invoke", invoke)
    graph.add_node("finalize", finalize)
    graph.set_entry_point("precheck")
    graph.add_conditional_edges(
        "precheck", route_after_precheck, {"invoke": "invoke", "end": END}
    )
    graph.add_edge("invoke", "finalize")
    graph.add_edge("finalize", END)
    return graph.compile()


_GRAPH = None


def run_describe(state: DescribeState) -> DescribeState:
    """Run the compiled graph, reusing it across requests."""
    global _GRAPH
    if _GRAPH is None:
        _GRAPH = build_graph()
    return _GRAPH.invoke(state)
