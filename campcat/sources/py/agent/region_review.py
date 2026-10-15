"""Check a region against its own crop before the host acts on it.

One model call per round, with no tools: the crop at full size, and the area
around it. A correction is kept, not trusted: when there is more than one,
a last call sees every crop and picks one. Corrections drift, so a pick that
cannot be read keeps the first proposal. A check that cannot run lets the
original call through.
"""

from __future__ import annotations

import json
import re
from typing import TypedDict

from langchain_core.messages import HumanMessage, SystemMessage
from langgraph.graph import END, StateGraph

import models
import session
import tools

MAX_ROUNDS = 3

# kind, and the shape a single region takes. update_script and save_anchor have
# one region per template entry, read off that entry's cut.
REGION_TOOLS = {
    "tap": ("tap", "region"),
    "save_template": ("template", "region"),
    "update_script": ("template", None),
    "save_anchor": ("template", None),
}

EXPECT_DOC = (
    "One sentence: what this region should contain, e.g. \"the claim button, "
    "without the panel behind it\". The crop is checked against it before the "
    "call runs."
)

REVIEW_PROMPT = (
    "You check one region another agent proposed, before it is used. You see "
    "the region cut out at its own size, and a second picture with the region "
    "drawn on it: the area around it at full resolution, or the whole frame "
    "scaled down, as the text says. Answer in exactly this form, nothing else:\n"
    "verdict: ok | adjust | reject\n"
    "box: {\"x\":..,\"y\":..,\"w\":..,\"h\":..}   (only with adjust; a circle "
    "region writes circle: {\"cx\":..,\"cy\":..,\"r\":..})\n"
    "reason: one sentence\n"
    "Use adjust for a small correction of the edges, in the frame's pixels. "
    "Use reject when the region is on the wrong element or the element is not "
    "in this frame."
)

PICK_PROMPT = (
    "You see every crop considered for one region, in order. 1 is the first "
    "proposal; each later one is a correction of the one before it, and "
    "corrections drift, so a later crop is not a better one. Pick the crop "
    "that best matches the expectation: the control it names, tight around "
    "it, nothing extra, and not cut off. Answer in exactly this form:\n"
    "pick: <the number>\n"
    "reason: one sentence"
)

REVIEW_CRITERIA = {
    "tap": "The region is to be tapped: it must sit on the element the expectation names.",
    "template": (
        "The region becomes a template: it must hold the stable control the "
        "expectation names and nothing behind it -- no background, no "
        "neighbouring element -- and must not cut the control off. For a "
        "circle, the corners outside the disc are ignored."
    ),
}

_HUMAN_REGION = re.compile(
    r"call (\w+) again with this corrected region (\{.*\})")

# The region a person redrew, as (tool, {"box": ...} or {"circle": ...}).
# The next call that repeats it is theirs, so it is not checked again.
_human: tuple | None = None


class ReviewState(TypedDict, total=False):
    kind: str
    shape: str
    region: dict
    shot: int | None
    expect: str
    label: str
    rounds: int
    crop_b64: str
    context_b64: str
    context_note: str
    frame_size: tuple
    verdict: str
    reason: str
    candidates: list


def neighbourhood(bounds: tuple[int, int, int, int],
                  frame: tuple[int, int]) -> tuple[int, int, int, int]:
    """The box around `bounds`, one of its own sides out, and at least 64px.

    Clipped to the frame. A region on the edge simply has no context that side.
    """
    x, y, w, h = bounds
    width, height = frame
    dx, dy = max(w, 64), max(h, 64)
    x0, y0 = max(0, x - dx), max(0, y - dy)
    x1, y1 = min(width, x + w + dx), min(height, y + h + dy)
    return x0, y0, x1 - x0, y1 - y0


def with_expect(offered: list) -> list:
    """A copy of `offered` whose region tools require `expect`.

    A schema that is not the shape this adds to is left as it is. The input
    list is not modified.
    """
    import copy
    out = copy.deepcopy(offered)
    for tool in out:
        try:
            name = tool["function"]["name"]
        except (KeyError, TypeError):
            continue
        if name not in REGION_TOOLS:
            continue
        try:
            params = tool["function"]["parameters"]
            if name in ("update_script", "save_anchor"):
                items = params["properties"]["templates"]["items"]
                items.setdefault("properties", {})["expect"] = {
                    "type": "string", "description": EXPECT_DOC}
                required = items.setdefault("required", [])
            else:
                params.setdefault("properties", {})["expect"] = {
                    "type": "string", "description": EXPECT_DOC}
                required = params.setdefault("required", [])
        except (KeyError, TypeError):
            continue
        if "expect" not in required:
            required.append("expect")
    return out


def parse_review(text: str, shape: str) -> tuple[str, dict | None, str]:
    """`(verdict, region, reason)`. Anything off the form is `skip`."""
    lines = [line.strip() for line in text.strip().splitlines() if line.strip()]
    if not lines or not lines[0].startswith("verdict:"):
        return "skip", None, text.strip()
    verdict = lines[0].split(":", 1)[1].strip()
    if verdict not in ("ok", "adjust", "reject"):
        return "skip", None, text.strip()
    region = None
    reason = ""
    for line in lines[1:]:
        if line.startswith("reason:"):
            reason = line.split(":", 1)[1].strip()
        elif line.startswith("box:") or line.startswith("circle:"):
            key, raw = line.split(":", 1)
            if key != shape:
                return "skip", None, text.strip()
            try:
                region = json.loads(raw)
            except json.JSONDecodeError:
                return "skip", None, text.strip()
    if verdict == "adjust" and not isinstance(region, dict):
        return "skip", None, text.strip()
    return verdict, region, reason


def parse_pick(text: str, count: int) -> int | None:
    """The 1-based crop a pick names, or None when the line is missing or off."""
    for line in text.strip().splitlines():
        line = line.strip()
        if not line.lower().startswith("pick:"):
            continue
        try:
            number = int(line.split(":", 1)[1].strip())
        except ValueError:
            return None
        return number if 1 <= number <= count else None
    return None


def remember_human_region(content: str) -> None:
    """Remember a region a person redrew, from the host's refusal text."""
    global _human
    if not isinstance(content, str):
        return
    found = _HUMAN_REGION.search(content)
    if not found:
        return
    try:
        payload = json.loads(found.group(2))
    except json.JSONDecodeError:
        return
    region = payload.get("region")
    if isinstance(region, dict):
        _human = (found.group(1), {"region": region})


def _strip(args: dict) -> dict:
    out = {key: value for key, value in args.items() if key != "expect"}
    entries = out.get("templates")
    if isinstance(entries, list):
        out["templates"] = [
            {key: value for key, value in entry.items() if key != "expect"}
            if isinstance(entry, dict) else entry
            for entry in entries
        ]
    return out


def _shape_of(region: dict) -> str:
    """Which shape a `region` argument is, from the fields it carries."""
    return "circle" if "cx" in region else "box"


def _bounds(region: dict, shape: str) -> tuple[int, int, int, int]:
    if shape == "circle":
        # The disc plus the one-pixel rim, the same box the matcher crops.
        return (region["cx"] - region["r"], region["cy"] - region["r"],
                2 * region["r"] + 1, 2 * region["r"] + 1)
    return region["x"], region["y"], region["w"], region["h"]


def _inside(region: dict, shape: str, frame: tuple) -> bool:
    if not frame or not isinstance(region, dict):
        return False
    width, height = frame
    try:
        if shape == "box":
            x, y, w, h = (region["x"], region["y"], region["w"], region["h"])
            return x >= 0 and y >= 0 and w > 0 and h > 0 \
                and x + w <= width and y + h <= height
        radius, cx, cy = region["r"], region["cx"], region["cy"]
        return radius > 0 and cx - radius >= 0 and cy - radius >= 0 \
            and cx + radius < width and cy + radius < height
    except (KeyError, TypeError):
        return False


def _moved(shape: str, before: dict, after: dict, reason: str) -> str:
    if before == after:
        return ""
    if shape == "box":
        return (f" (self-check moved box {before['x']},{before['y']} "
                f"{before['w']}x{before['h']} to {after['x']},{after['y']} "
                f"{after['w']}x{after['h']}: {reason})")
    return (f" (self-check moved circle r={before['r']} at "
            f"{before['cx']},{before['cy']} to r={after['r']} at "
            f"{after['cx']},{after['cy']}: {reason})")


def _picture(image: str) -> dict:
    return {"type": "image_url",
            "image_url": {"url": f"data:{session.MIME};base64,{image}"}}


def review_render(state: ReviewState) -> dict:
    shape = state["shape"]
    region = state["region"]
    extra = {} if state.get("shot") is None else {"shot": state["shot"]}
    crop = tools.call_cpp("view_crop", {"region": region, **extra})
    if not crop.get("ok"):
        return {"verdict": "skip", "reason": str(crop.get("error", ""))}
    marked = tools.call_cpp(
        "mark", {"label": state.get("label") or "", "region": region, **extra})
    if not marked.get("ok"):
        return {"verdict": "skip", "reason": str(marked.get("error", ""))}
    marked_b64 = marked.get("image_b64") or ""
    frame = session.png_size(marked_b64)
    context = marked_b64
    if not frame:
        note = "second picture: the whole frame"
    else:
        box = neighbourhood(_bounds(region, shape), frame)
        cut = session.crop_png(marked_b64, box)
        if cut:
            context = cut
            note = (f"second picture: neighbourhood {box[0]},{box[1]} "
                    f"{box[2]}x{box[3]} of the {frame[0]}x{frame[1]} frame, "
                    f"at full resolution; give coordinates in frame pixels")
        else:
            note = f"second picture: the whole {frame[0]}x{frame[1]} frame"
    return {"crop_b64": crop.get("image_b64") or "", "context_b64": context,
            "context_note": note, "frame_size": frame or ()}


def review_judge(state: ReviewState) -> dict:
    rounds = state.get("rounds", 0) + 1
    text = (
        f"{REVIEW_CRITERIA[state['kind']]}\n"
        f"expectation: {state.get('expect') or '(none given)'}\n"
        f"region: {json.dumps(state['region'], separators=(',', ':'))}\n"
        f"{state.get('context_note', '')}"
    )
    try:
        model = models.build_chat_model()
        response = model.invoke([
            SystemMessage(content=REVIEW_PROMPT),
            HumanMessage(content=[
                {"type": "text", "text": text},
                _picture(state.get("crop_b64") or ""),
                _picture(state.get("context_b64") or ""),
            ]),
        ])
    except tools.TurnInterrupted:
        raise
    except Exception as exc:  # noqa: BLE001 - a failed check lets the call through
        return {"verdict": "skip", "reason": str(exc), "rounds": rounds}
    verdict, region, reason = parse_review(
        session.content_text(getattr(response, "content", response)), state["shape"])
    out: dict = {"verdict": verdict, "reason": reason, "rounds": rounds}
    if verdict == "adjust" and _inside(region, state["shape"], state.get("frame_size")):
        out["region"] = region
        out["candidates"] = list(state.get("candidates") or []) + [
            {"region": region, "note": reason}]
    elif verdict == "adjust":
        # Off the frame, or no box: the previous region stands, and the loop stops.
        out["verdict"] = "ok"
    return out


def _after_render(state: ReviewState) -> str:
    # Only a render that could not fetch the pictures stops here. A verdict
    # left over from the previous round is not this render's answer.
    return "end" if state.get("verdict") == "skip" else "judge"


def _after_judge(state: ReviewState) -> str:
    if state.get("verdict") == "adjust" and state.get("rounds", 0) < MAX_ROUNDS:
        return "render"
    # A correction is a candidate, not the answer: the last call picks among
    # them. One candidate means nothing moved, so there is nothing to pick.
    if (state.get("verdict") in ("ok", "adjust")
            and len(state.get("candidates") or []) > 1):
        return "pick"
    return "end"


def review_pick(state: ReviewState) -> dict:
    """The crop that best matches the expectation, from every one considered."""
    candidates = state.get("candidates") or []
    first = candidates[0]["region"]
    extra = {} if state.get("shot") is None else {"shot": state["shot"]}
    lines = [
        REVIEW_CRITERIA[state["kind"]],
        f"expectation: {state.get('expect') or '(none given)'}",
    ]
    pictures = []
    for index, candidate in enumerate(candidates, 1):
        crop = tools.call_cpp("view_crop", {"region": candidate["region"], **extra})
        if not crop.get("ok"):
            return {"region": first, "verdict": "ok",
                    "reason": str(crop.get("error", "")) or "could not compare"}
        pictures.append(_picture(crop.get("image_b64") or ""))
        lines.append(
            f"{index}. {json.dumps(candidate['region'], separators=(',', ':'))}"
            f" — {candidate.get('note') or ''}")
    lines.append("The pictures follow in that order.")
    try:
        model = models.build_chat_model()
        response = model.invoke([
            SystemMessage(content=PICK_PROMPT),
            HumanMessage(content=[
                {"type": "text", "text": "\n".join(lines)}, *pictures]),
        ])
    except tools.TurnInterrupted:
        raise
    except Exception as exc:  # noqa: BLE001 - an unreadable pick keeps the proposal
        return {"region": first, "verdict": "ok", "reason": str(exc)}
    text = session.content_text(getattr(response, "content", response))
    number = parse_pick(text, len(candidates))
    if number is None:
        return {"region": first, "verdict": "ok",
                "reason": "unreadable pick; kept the first proposal"}
    reason = ""
    for line in text.splitlines():
        if line.strip().lower().startswith("reason:"):
            reason = line.split(":", 1)[1].strip()
    return {"region": candidates[number - 1]["region"], "verdict": "ok",
            "reason": reason}


_GRAPH = None


def _graph():
    global _GRAPH
    if _GRAPH is None:
        compiled = StateGraph(ReviewState)
        compiled.add_node("review_render", review_render)
        compiled.add_node("review_judge", review_judge)
        compiled.add_node("review_pick", review_pick)
        compiled.set_entry_point("review_render")
        compiled.add_conditional_edges(
            "review_render", _after_render, {"judge": "review_judge", "end": END})
        compiled.add_conditional_edges(
            "review_judge", _after_judge,
            {"render": "review_render", "pick": "review_pick", "end": END})
        compiled.add_edge("review_pick", END)
        _GRAPH = compiled.compile()
    return _GRAPH


def _one(kind: str, shape: str, region: dict, shot, expect: str,
         label: str) -> tuple[dict, str, str]:
    """`(region to send, note, rejection)`. A rejection ends the call."""
    import graph
    out = graph.run_subgraph(_graph(), {
        "kind": kind, "shape": shape, "region": region, "shot": shot,
        "expect": expect, "label": label or expect, "rounds": 0,
        "candidates": [{"region": dict(region), "note": "proposal"}],
    })
    verdict = out.get("verdict")
    if verdict == "reject":
        return region, "", out.get("reason") or "rejected"
    if verdict == "skip":
        return region, f" (self-check skipped: {out.get('reason') or 'unreadable'})", ""
    final = out.get("region") or region
    note = _moved(shape, region, final, out.get("reason") or "")
    if not note and len(out.get("candidates") or []) > 1:
        note = (f" (self-check kept the first of {len(out['candidates'])}: "
                f"{out.get('reason') or ''})")
    return final, note, ""


def _human_repeat(name: str, args: dict) -> bool:
    """True when this call is the person sending back the region they redrew."""
    global _human
    if _human is None or _human[0] != name:
        return False
    saved = _human[1]
    same = args.get("region") == saved.get("region")
    if not same:
        return False
    _human = None
    return True


def _review_update(args: dict) -> tuple[dict, str, str]:
    notes = []
    templates = []
    for entry in args.get("templates") or []:
        if not isinstance(entry, dict) or not isinstance(entry.get("cut"), dict):
            templates.append(
                {key: value for key, value in entry.items() if key != "expect"}
                if isinstance(entry, dict) else entry)
            continue
        cut = entry["cut"]
        shape = "circle" if "cx" in cut else "box"
        final, note, rejected = _one(
            "template", shape, cut, entry.get("shot"),
            str(entry.get("expect") or ""), str(entry.get("name") or ""))
        if rejected:
            name = entry.get("name") or ""
            return args, "", f"{name}: {rejected}" if name else rejected
        kept = {key: value for key, value in entry.items() if key != "expect"}
        kept["cut"] = final
        templates.append(kept)
        if note:
            notes.append(note)
    out = _strip(args)
    out["templates"] = templates
    return out, "".join(notes), ""


def review(name: str, args: dict) -> tuple[dict, str, str]:
    """`(args to send, note to append, rejection)`.

    A rejection is sent back instead of the call. No region, or a region a
    person just redrew, goes out with `expect` removed and nothing else done.
    """
    spec = REGION_TOOLS.get(name)
    if spec is None:
        return _strip(args), "", ""
    if _human_repeat(name, args):
        return _strip(args), "", ""
    if name in ("update_script", "save_anchor"):
        return _review_update(args)
    _kind, key = spec
    region = args.get(key) if isinstance(args.get(key), dict) else None
    if region is None:
        return _strip(args), "", ""
    final, note, rejected = _one(
        _kind, _shape_of(region), region,
        None if _kind == "tap" else args.get("shot"),
        str(args.get("expect") or ""), str(args.get("expect") or name))
    if rejected:
        return args, "", rejected
    out = _strip(args)
    out[key] = final
    return out, note, ""
