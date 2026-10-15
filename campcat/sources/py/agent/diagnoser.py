"""Why a template failed to match: the template, the best crop, the screen.

One model call, no tools. The result is text the scripter acts on. Pictures
stay in this call; only the screenshot it takes is registered as a frame.
"""

from __future__ import annotations

import re
from typing import TypedDict

from langchain_core.messages import HumanMessage, SystemMessage
from langgraph.graph import END, StateGraph

import models
import session
import tools

DIAGNOSE_TEMPLATE_NAME = "diagnose_template"

DIAGNOSE_TEMPLATE = {
    "type": "function",
    "function": {
        "name": DIAGNOSE_TEMPLATE_NAME,
        "description": (
            "Decide why a template failed to match, from the template, the "
            "screen at the matcher's best box, and the whole screen. Returns "
            "crop_issue, wrong_screen, or inconclusive, and the shot to re-cut "
            "from. Pass the run_script error verbatim."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "bundle": {
                    "type": "string",
                    "description": "Bundle the template lives in.",
                },
                "template": {
                    "type": "string",
                    "description": "Template name; .png may be omitted.",
                },
                "error": {
                    "type": "string",
                    "description": (
                        "The run_script failure verbatim. It must contain "
                        "best=… box=x,y,w,h threshold=…."
                    ),
                },
                "expect": {
                    "type": "string",
                    "description": (
                        "What that step expected to see, usually its // comment."
                    ),
                },
            },
            "required": ["bundle", "template", "error"],
        },
    },
}

JUDGE_PROMPT = (
    "A script failed because a template did not match. You see three pictures: "
    "the template it looked for, the screen at the place the matcher liked best "
    "(same scale as the template), and the whole screen. Decide which side is "
    "wrong and answer in exactly this form:\n"
    "verdict: crop_issue | wrong_screen | inconclusive\n"
    "reason: what you see in the template and on the screen, in one or two sentences\n"
    "fix: one sentence on what to change\n"
    "crop_issue: the control in the template is on this screen, but the template "
    "also holds background or a neighbouring element, or its edges are off. "
    "wrong_screen: the control is not on this screen at all. inconclusive: you "
    "cannot tell. A circular template's corners are transparent and are not background."
)

_MATCH = re.compile(
    r"\(best=([\d.]+) box=(-?\d+),(-?\d+),(\d+),(\d+) threshold=([\d.]+)\)")
_VERDICT_LINE = re.compile(
    r"verdict:\s*(crop_issue|wrong_screen|inconclusive)\s*$")


class DiagnoseState(TypedDict, total=False):
    bundle: str
    template: str
    error: str
    expect: str
    match: dict
    shot: int
    screen_b64: str
    crop_b64: str
    template_b64: str
    verdict: str
    reason: str
    fix: str


def parse_match(error: str, template: str) -> dict | None:
    """The verdict that follows this template's name, or None."""
    name = template if template.endswith(".png") else template + ".png"
    at = error.find(name)
    if at < 0:
        return None
    found = _MATCH.search(error, at)
    if found is None:
        return None
    best, x, y, w, h, threshold = found.groups()
    return {
        "best": float(best),
        "box": {"x": int(x), "y": int(y), "w": int(w), "h": int(h)},
        "threshold": float(threshold),
    }


def parse_diagnosis(text: str) -> tuple[str, str, str]:
    """`(verdict, reason, fix)`. A bad first line is inconclusive, text kept."""
    lines = text.splitlines()
    first = lines[0].strip() if lines else ""
    found = _VERDICT_LINE.match(first)
    if found is None:
        return "inconclusive", text, ""
    reason = ""
    fix = ""
    for line in lines[1:]:
        if line.startswith("reason:"):
            reason = line[len("reason:"):].strip()
        elif line.startswith("fix:"):
            fix = line[len("fix:"):].strip()
    return found.group(1), reason, fix


def _host_fail(reply: dict, shot: int = 0) -> dict:
    return {
        "verdict": "inconclusive",
        "shot": shot,
        "reason": str(reply.get("error") or "host call failed"),
        "fix": "",
    }


def diagnose_parse(state: DiagnoseState) -> dict:
    match = parse_match(state.get("error") or "", state.get("template") or "")
    if match is None:
        return {
            "verdict": "inconclusive",
            "shot": 0,
            "reason": "the error has no match verdict for this template",
            "fix": "",
        }
    return {"match": match, "shot": 0}


def diagnose_gather(state: DiagnoseState) -> dict:
    match = state["match"]
    shot_reply = tools.call_cpp("screenshot", {})
    if not shot_reply.get("ok"):
        return _host_fail(shot_reply)
    image = shot_reply.get("image_b64") or ""
    shot = shot_reply.get("shot")
    shot_n = shot if isinstance(shot, int) and not isinstance(shot, bool) else 0
    session.screenshot_frame(shot, image)
    crop = tools.call_cpp("view_crop", {"region": match["box"]})
    if not crop.get("ok"):
        return _host_fail(crop, shot_n)
    template = tools.call_cpp("view_template", {
        "bundle": state.get("bundle") or "",
        "name": state.get("template") or "",
    })
    if not template.get("ok"):
        return _host_fail(template, shot_n)
    return {
        "shot": shot_n,
        "screen_b64": image,
        "crop_b64": crop.get("image_b64") or "",
        "template_b64": template.get("image_b64") or "",
    }


def diagnose_judge(state: DiagnoseState) -> dict:
    match = state["match"]
    expect = state.get("expect") or "(none given)"
    text = f"best={match['best']} threshold={match['threshold']}\nexpectation: {expect}"
    screen = session.thumbnail_png(state.get("screen_b64") or "")
    if screen is None:
        screen = state.get("screen_b64") or ""
    content: list = [{"type": "text", "text": text}]
    for image in (state.get("template_b64") or "", state.get("crop_b64") or "", screen):
        content.append({
            "type": "image_url",
            "image_url": {"url": f"data:{session.MIME};base64,{image}"},
        })
    try:
        reply = models.build_chat_model().invoke([
            SystemMessage(content=JUDGE_PROMPT),
            HumanMessage(content=content),
        ])
    except tools.TurnInterrupted:
        raise
    except Exception as exc:  # noqa: BLE001 - reported as inconclusive, not a failed turn
        return {
            "verdict": "inconclusive",
            "reason": "model error: " + str(exc),
            "fix": "",
        }
    raw = reply.content if isinstance(reply.content, str) else str(reply.content)
    return {"reason": raw}


def diagnose_finish(state: DiagnoseState) -> dict:
    if state.get("verdict"):
        return {}
    verdict, reason, fix = parse_diagnosis(state.get("reason") or "")
    return {"verdict": verdict, "reason": reason, "fix": fix}


def _after_parse(state: DiagnoseState) -> str:
    return "diagnose_finish" if state.get("verdict") else "diagnose_gather"


def _after_gather(state: DiagnoseState) -> str:
    return "diagnose_finish" if state.get("verdict") else "diagnose_judge"


_GRAPH = None


def _graph():
    global _GRAPH
    if _GRAPH is None:
        compiled = StateGraph(DiagnoseState)
        compiled.add_node("diagnose_parse", diagnose_parse)
        compiled.add_node("diagnose_gather", diagnose_gather)
        compiled.add_node("diagnose_judge", diagnose_judge)
        compiled.add_node("diagnose_finish", diagnose_finish)
        compiled.set_entry_point("diagnose_parse")
        compiled.add_conditional_edges(
            "diagnose_parse", _after_parse,
            {"diagnose_gather": "diagnose_gather", "diagnose_finish": "diagnose_finish"},
        )
        compiled.add_conditional_edges(
            "diagnose_gather", _after_gather,
            {"diagnose_judge": "diagnose_judge", "diagnose_finish": "diagnose_finish"},
        )
        compiled.add_edge("diagnose_judge", "diagnose_finish")
        compiled.add_edge("diagnose_finish", END)
        _GRAPH = compiled.compile()
    return _GRAPH


def _report(state: dict) -> str:
    return (
        f"verdict: {state.get('verdict') or 'inconclusive'}\n"
        f"shot: {state.get('shot') or 0}\n"
        f"reason: {state.get('reason') or ''}\n"
        f"fix: {state.get('fix') or ''}"
    )


def run_diagnoser(args: dict) -> str:
    """The four-line report. A user interrupt propagates."""
    import graph
    out = graph.run_subgraph(_graph(), {
        "bundle": str(args.get("bundle") or ""),
        "template": str(args.get("template") or ""),
        "error": str(args.get("error") or ""),
        "expect": str(args.get("expect") or ""),
    })
    return _report(out)
