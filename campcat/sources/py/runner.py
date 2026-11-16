"""Shared model invocation and structured tool execution for both agents."""

from __future__ import annotations

import json
from dataclasses import dataclass, field

from langchain_core.messages import HumanMessage, ToolMessage

import agent.region_review as region_review
import models
import session
import tools

MAX_MODEL_ROUNDS = 1000
_FAIL_LIMIT = 2
FUSE_TEXT = "error: 同一调用已连续失败 3 次，停止重试，如实报告"
# A module stack survives LangGraph's copied execution contexts; a handoff
# gets its own streaks and restores the operator's when it returns.
_FAIL_STACK: list[dict] = [{}]
NO_TOOLS_NOTE = (
    "这次请求的工具额度已经用完。现在只用纯文本回答，"
    "不要再输出任何工具调用的标记。"
)


@dataclass
class ToolResult:
    content: object
    pictures: list = field(default_factory=list)
    error_code: str = ""

    @property
    def failed(self) -> bool:
        return bool(self.error_code)


def invoke(state: dict, offered: list, on_chunk=None) -> dict:
    """One model turn against `offered`.

    The schemas are passed to bind_tools with `expect` added on region tools;
    the check strips it before the host sees the call. The operator and the
    scripter share this; each passes its own table and its own `on_chunk` -- the
    operator's stream is the parent graph's, a subagent's goes to its window.
    """
    offered = region_review.with_expect(offered)
    cap = state.get("max_model_rounds", MAX_MODEL_ROUNDS)
    exhausted = bool(offered) and state.get("tool_rounds", 0) >= cap
    try:
        model = models.build_chat_model(context=True)
        # Past the round cap the tools are withdrawn, so the model has to answer
        # in text instead of the request ending on an empty tool call.
        if offered and not exhausted:
            model = model.bind_tools(offered)
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as a result
        return {"error": str(exc) or type(exc).__name__}

    model_input = list(state.get("messages") or [])
    if exhausted:
        # Withdrawing the tools silently is what produced raw markup in the
        # answer, so the withdrawal gets said out loud.
        model_input.append(HumanMessage(content=NO_TOOLS_NOTE))

    try:
        response = models.streamed(model, model_input, on_chunk)
    except tools.TurnInterrupted:
        # Not an error: the host has the next message already. The turn commits
        # what finished and stops, like a stop inside the tool batch.
        return {"interrupted": True}
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as a result
        return {"error": str(exc) or type(exc).__name__}

    messages = list(state.get("messages") or [])
    messages.append(response)
    text = session.strip_tool_markup(
        session.content_text(getattr(response, "content", response)))
    if not text and exhausted:
        # Tools are gone and the model said nothing usable, so say that rather
        # than showing an empty bubble the user would read as a hang. An empty
        # text on a normal round is just a round that only called tools.
        text = "这次请求的工具额度已经用完，而模型没有给出文字回答。"
    # Accumulated across rounds: the UI streams every round's text, so keeping
    # only the last one would make the reply shrink when the turn completes.
    # A model that announces a tool call and then answers is the common case.
    prior = state.get("reply", "")
    if prior and text:
        reply = prior.rstrip() + "\n\n" + text
    else:
        reply = prior + text
    return {"messages": messages, "reply": reply}


def host_result(name: str, args: dict) -> ToolResult:
    """One host tool. Pictures stay out of the result list; the caller appends them."""
    args, note, rejected = region_review.review(name, args)
    if rejected:
        return ToolResult("error: 自查否掉了这个区域：" + rejected, error_code="region_rejected")
    # Said before the call because the approval gate, which comes first, is
    # where the score helps: the host's own note on this call cannot carry it.
    if note:
        tools.note(note)
    reply = tools.call_cpp(name, args)
    error_code = "" if reply.get("ok") else str(reply.get("error_code") or "host_error")
    if reply.get("ok"):
        content: object = reply.get("text") or "ok"
    else:
        content = "error: " + str(reply.get("error", ""))
    if note and isinstance(content, str):
        content += " (" + note + ")"
    image = reply.get("image_b64") or ""
    if isinstance(content, str):
        region_review.remember_human_region(content)
    if not image:
        return ToolResult(content, error_code=error_code)
    if name == "screenshot":
        frame = session.screenshot_frame(reply.get("shot"), image)
        return ToolResult(content, [frame] if frame is not None else [], error_code)
    if name == "view_shot":
        return ToolResult(content, [session.recall_frame(args.get("shot"), image)], error_code)
    return ToolResult(content, [session.picture_of(name, image, args)], error_code)


def _interrupted_from(calls: list, start: int) -> list:
    """Tool results for `calls[start:]`: the one in flight, and the ones not sent."""
    return [
        ToolMessage(content=tools.INTERRUPTED_TEXT, tool_call_id=call.get("id", ""),
                    status="error", artifact={"error_code": "interrupted"})
        for call in calls[start:]
    ]


def reset_fail_counts() -> None:
    """Drop every streak. One operator turn starts clean."""
    _FAIL_STACK[:] = [{}]


def begin_fail_counts() -> None:
    """A fresh streak for one handoff, so it does not inherit the operator's."""
    _FAIL_STACK.append({})


def end_fail_counts() -> None:
    if len(_FAIL_STACK) > 1:
        _FAIL_STACK.pop()


def _fail_counts() -> dict:
    return _FAIL_STACK[-1]


def _fail_key(call: dict) -> str:
    name = str(call.get("name") or "")
    args = json.dumps(call.get("args") or {}, sort_keys=True,
                      ensure_ascii=False, default=str)
    return name + "\0" + args


def run_calls(calls: list, run_one) -> tuple[list, list, bool]:
    """Run `calls` until one is interrupted.

    `run_one(call)` returns a ToolResult or raises TurnInterrupted.
    An interrupt fills that call and every call after it, and stops the batch.
    The same call with the same failure category twice is not run a third time.
    """
    results: list = []
    pictures: list = []
    counts = _fail_counts()
    for index, call in enumerate(calls):
        key = _fail_key(call)
        seen = counts.get(key)
        if seen is not None and seen[1] >= _FAIL_LIMIT:
            outcome = ToolResult(FUSE_TEXT, error_code="retry_exhausted")
        else:
            try:
                outcome = run_one(call)
            except tools.TurnInterrupted:
                results.extend(_interrupted_from(calls, index))
                return results, pictures, True
            if outcome.failed:
                code = outcome.error_code
                counts[key] = (code, seen[1] + 1) if seen and seen[0] == code else (code, 1)
            else:
                counts.pop(key, None)
        results.append(ToolMessage(
            content=outcome.content, tool_call_id=call.get("id", ""),
            status="error" if outcome.failed else "success",
            artifact={"error_code": outcome.error_code} if outcome.failed else None))
        pictures.extend(outcome.pictures)
    return results, pictures, False


