"""Check a region against its own crop before the host acts on it.

One model call per round, with no tools: the crop at full size, and the area
around it. A region the check is not sure about gets one more look, asked for
the strongest alternative it can see. Every attempt is only a candidate: the
last call sees each one's cut and the surroundings it sits in, picks the best,
and a pick that cannot be read keeps the first proposal. A check that cannot
run lets the original call through.
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

# One 0..1 scale for the judge's own confidence and any scoring model wired in
# later: below this, another attempt is worth trying.
MIN_SCORE = 0.6

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

_PRESSURE = (
    "Assume the region is wrong until you have looked for the fault and not "
    "found it. You did not draw this: a first proposal carries margin, and a "
    "correction that already ran can overshoot the other way. Name the one "
    "edge -- left, right, top, bottom, or the shape itself -- most likely to "
    "be off, and answer ok only if you would ship this exact crop unchanged "
    "to every later run. A crop you sign off on is reused long after this "
    "check is gone, so a fault you saw and let pass is the expensive one. "
    "Adjusting is cheap and is compared, not trusted: a worse try is dropped, "
    "so there is no reason to accept a crop you would not use yourself. The "
    "confidence number is not a vote either: a low one buys one more look and "
    "counts against nothing, while a high one just ends the check."
)

REVIEW_PROMPT = (
    "You check one region another agent proposed, before it is used. You see "
    "the region cut out at its own size, and a second picture with the region "
    "drawn on it: the area around it at full resolution, or the whole frame "
    "scaled down, as the text says. Answer in exactly this form, nothing else:\n"
    "verdict: ok | adjust | reject\n"
    "confidence: 0.0-1.0   (how sure you are this region is right; 0 = guessing)\n"
    "box: {\"x\":..,\"y\":..,\"w\":..,\"h\":..}   (only with adjust; a circle "
    "region writes circle: {\"cx\":..,\"cy\":..,\"r\":..}; a diamond region "
    "writes diamond: {\"cx\":..,\"cy\":..,\"rx\":..,\"ry\":..})\n"
    "reason: one sentence\n"
    "Use adjust for a small correction of the edges, in the frame's pixels. "
    "Use reject when the region is on the wrong element or the element is not "
    "in this frame.\n" + _PRESSURE
)

_ALTERNATIVE = (
    "You were not sure last time. Look again and name the strongest "
    "alternative as `adjust`, switching to circle, diamond or box when the "
    "control could be another shape. A try may be worse: it is only a "
    "candidate, and the best of them is picked afterwards."
)

PICK_PROMPT = (
    "You see every attempt for one region, in order: the first proposal and "
    "any try after it. For each, its cut, then the same region drawn in its "
    "surroundings. The numbers only name the attempts: a later one is not "
    "better and not worse than the first. Pick the cut that best matches the "
    "expectation: the control it names, tight around it, nothing extra, and "
    "not cut off. Answer in exactly this form:\n"
    "pick: <the number>\n"
    "reason: one sentence"
)

# Shared by the one-crop check and the pick, so the two cannot drift.
_TEMPLATE_RULES = (
    "Apply these rules in order and stop at the first that decides. Color and "
    "how much a crop looks like a button do not count.\n"
    "1. Text boundary. When the control is text -- a label, a menu row, a "
    "button caption -- the glyphs are the control: the edges sit on the first "
    "and the last stroke, and it is a box around exactly those pixels. Not the "
    "button, pill or panel they sit on, and no 留白 for legibility either.\n"
    "2. Shape. A round control (圆) keeps only a circle {cx,cy,r}. A "
    "diamond control (菱形) keeps only a diamond {cx,cy,rx,ry}. A "
    "rectangular control keeps only a box {x,y,w,h}. While a right shape "
    "remains, drop the wrong one. A box around a round control is the wrong "
    "shape. A diamond much wider than it is tall is a box, not a diamond.\n"
    "3. Clipping. Drop a crop that cuts the control off while a complete one "
    "remains. A circle is judged by its rim, not the corners outside the disc. "
    "A diamond is judged by its four vertices, not the corners of its box.\n"
    "4. Edges. A crop whose edge sits on the control beats one with background "
    "between the edge and the control (留白). While a flush crop remains, drop "
    "the ones with that gap.\n"
    "5. If none sit on the edge, keep the one with the least 留白.\n"
    "6. If more than one remain, keep the lowest number, the first proposal."
)

TEMPLATE_PICK_PROMPT = (
    "You see every attempt for one template, in order: the first proposal and "
    "any try after it. For each, its cut, then the same region drawn in its "
    "surroundings. The numbers only name the attempts: a later one is not "
    "better and not worse than the first.\n"
    + _TEMPLATE_RULES +
    "\nJudge rule 3 (cutting the control off) on the surroundings picture, not "
    "on the cut alone."
    "\nAnswer in exactly this form:\n"
    "pick: <the number>\n"
    "reason: one sentence naming which rule decided it"
)

REVIEW_CRITERIA = {
    "tap": "The region is to be tapped: it must sit on the element the expectation names.",
    "template": (
        "The region becomes a template.\n" + _TEMPLATE_RULES + "\n"
        "On this one crop, answer ok only when its shape is right and its edge "
        "already sits on the control -- on the glyphs, when the control is text. "
        "Otherwise adjust to that boundary, and "
        "switch circle, diamond and box when the shape is wrong. Reject only when the "
        "region is on the wrong element or the element is not in this frame."
    ),
}


def _review_system(kind: str) -> str:
    if kind == "template":
        extra = (
            "For a template, an adjust may change the shape: write circle: for "
            "a round control, diamond: for a diamond, and box: for a rectangular one."
        )
    else:
        extra = (
            "The box, circle or diamond key must be the shape of the region you were given."
        )
    return REVIEW_PROMPT + "\n" + extra


_HUMAN_REGION = re.compile(
    r"call (\w+) again with this corrected region (\{.*\})")

# The region a person redrew, as (tool, {"box": ...} or {"circle": ...}).
# The next call that repeats it is theirs, so it is not checked again.
_human: tuple | None = None

# A checked region does not converge across calls: the next look moves the box
# the previous one just accepted. One acceptance is final for the turn; a
# rejection may be retried once, and the second rejection ends it.
_STOP = (" Do not view_crop or save this region again this turn "
         "unless the tool returned an error.")
_KEPT = "already checked this turn; kept the previous region." + _STOP
_accepted: dict[str, dict] = {}
_reject_count: dict[str, int] = {}
_reject_reason: dict[str, str] = {}


def begin_turn() -> None:
    """Drop this turn's checks. A new request may look at the screen again."""
    global _human
    _human = None
    _accepted.clear()
    _reject_count.clear()
    _reject_reason.clear()


class ReviewState(TypedDict, total=False):
    kind: str
    shape: str
    region: dict
    shot: int | None
    expect: str
    label: str
    rounds: int
    confidence: float
    tried_alt: bool
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


def parse_review(text: str, shape: str, *, kind: str = "tap"
                 ) -> tuple[str, dict | None, str, str]:
    """`(verdict, region, reason, shape)`. Anything off the form is `skip`.

    A template adjust may name either shape; the key on that line is the new
    one. A tap must repeat the shape it was given.
    """
    lines = [line.strip() for line in text.strip().splitlines() if line.strip()]
    if not lines or not lines[0].startswith("verdict:"):
        return "skip", None, text.strip(), shape
    verdict = lines[0].split(":", 1)[1].strip()
    if verdict not in ("ok", "adjust", "reject"):
        return "skip", None, text.strip(), shape
    region = None
    reason = ""
    for line in lines[1:]:
        if line.startswith("reason:"):
            reason = line.split(":", 1)[1].strip()
        elif line.startswith(("box:", "circle:", "diamond:")):
            key, raw = line.split(":", 1)
            if kind != "template" and key != shape:
                return "skip", None, text.strip(), shape
            try:
                region = json.loads(raw)
            except json.JSONDecodeError:
                return "skip", None, text.strip(), shape
            if not isinstance(region, dict):
                return "skip", None, text.strip(), shape
            shape = key
    if verdict == "adjust" and not isinstance(region, dict):
        return "skip", None, text.strip(), shape
    return verdict, region, reason, shape


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
    if "rx" in region:
        return "diamond"
    return "circle" if "cx" in region else "box"


def _bounds(region: dict, shape: str) -> tuple[int, int, int, int]:
    if shape == "circle":
        # The disc plus the one-pixel rim, the same box the matcher crops.
        return (region["cx"] - region["r"], region["cy"] - region["r"],
                2 * region["r"] + 1, 2 * region["r"] + 1)
    if shape == "diamond":
        return (region["cx"] - region["rx"], region["cy"] - region["ry"],
                2 * region["rx"] + 1, 2 * region["ry"] + 1)
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
        if shape == "diamond":
            rx, ry = region["rx"], region["ry"]
            cx, cy = region["cx"], region["cy"]
            return rx > 0 and ry > 0 and cx - rx >= 0 and cy - ry >= 0 \
                and cx + rx < width and cy + ry < height
        radius, cx, cy = region["r"], region["cx"], region["cy"]
        return radius > 0 and cx - radius >= 0 and cy - radius >= 0 \
            and cx + radius < width and cy + radius < height
    except (KeyError, TypeError):
        return False


def _moved(shape: str, before: dict, after: dict) -> str:
    if before == after:
        return ""
    # The sent crop may have changed shape, so the note follows `after`.
    shape = _shape_of(after)
    if shape == "box":
        where = (f"box {after['x']},{after['y']} {after['w']}x{after['h']}")
    elif shape == "diamond":
        where = (f"diamond rx={after['rx']} ry={after['ry']} "
                 f"at {after['cx']},{after['cy']}")
    else:
        where = (f"circle r={after['r']} at {after['cx']},{after['cy']}")
    return f"already used {where}.{_STOP}"


_NUMBER = re.compile(r"[-+]?\d*\.?\d+")


def _confidence(text: str) -> float:
    """The judge's own 0..1 score, clamped; 1.0 when it did not give one."""
    for line in text.splitlines():
        if line.strip().lower().startswith("confidence:"):
            found = _NUMBER.search(line.split(":", 1)[1])
            if found is None:
                return 1.0
            return min(1.0, max(0.0, float(found.group())))
    return 1.0


def _needs_alt(state: ReviewState) -> bool:
    """True when the last verdict accepted a region it was unsure about.

    The judge that takes the extra look marks `tried_alt`, so one check gets at
    most one of them.
    """
    return (state.get("verdict") == "ok"
            and state.get("confidence", 1.0) < MIN_SCORE
            and not state.get("tried_alt")
            and state.get("rounds", 0) < MAX_ROUNDS)


def _self_check(state: ReviewState, body: str) -> str:
    """The check's score, and what it did about it, for the history.

    The score is what the judge said and is also what a scoring model would
    say, so a low one in the log explains a second look. The brackets around it
    are the caller's: the same line goes into the model's result and into the
    conversation.
    """
    score = state.get("confidence")
    tag = "" if score is None else f" {score:.2f}"
    return f"self-check{tag}{'; ' + body if body else ''}"


def _picture(image: str) -> dict:
    return {"type": "image_url",
            "image_url": {"url": f"data:{session.MIME};base64,{image}"}}


def _views(region: dict, shape: str, shot, label: str
           ) -> tuple[str, str, str, tuple, str]:
    """`(cut, context, note, frame, error)` for one region.

    The cut is what would be used; the context is the same region drawn on the
    frame around it, so a boundary that takes the control with it is visible.
    `error` is the host's refusal, with the first three empty then.
    """
    extra = {} if shot is None else {"shot": shot}
    crop = tools.call_cpp("tight_crop", {"region": region, **extra})
    if not crop.get("ok"):
        return "", "", "", (), str(crop.get("error", ""))
    marked = tools.call_cpp("mark", {"label": label, "region": region, **extra})
    if not marked.get("ok"):
        return "", "", "", (), str(marked.get("error", ""))
    cut = crop.get("image_b64") or ""
    marked_b64 = marked.get("image_b64") or ""
    frame = session.png_size(marked_b64)
    if not frame:
        return cut, marked_b64, "second picture: the whole frame", (), ""
    box = neighbourhood(_bounds(region, shape), frame)
    context = session.crop_png(marked_b64, box)
    if not context:
        return (cut, marked_b64,
                f"second picture: the whole {frame[0]}x{frame[1]} frame",
                frame, "")
    note = (f"second picture: neighbourhood {box[0]},{box[1]} "
            f"{box[2]}x{box[3]} of the {frame[0]}x{frame[1]} frame, "
            f"at full resolution; give coordinates in frame pixels")
    return cut, context, note, frame, ""


def review_render(state: ReviewState) -> dict:
    cut, context, note, frame, error = _views(
        state["region"], state["shape"], state.get("shot"),
        state.get("label") or "")
    if error:
        return {"verdict": "skip", "reason": error}
    return {"crop_b64": cut, "context_b64": context, "context_note": note,
            "frame_size": frame}


def review_judge(state: ReviewState) -> dict:
    rounds = state.get("rounds", 0) + 1
    text = (
        f"{REVIEW_CRITERIA[state['kind']]}\n"
        f"expectation: {state.get('expect') or '(none given)'}\n"
        f"region: {json.dumps(state['region'], separators=(',', ':'))}\n"
        f"{state.get('context_note', '')}"
    )
    alt = _needs_alt(state)
    if alt:
        text += "\n" + _ALTERNATIVE
    try:
        model = models.build_chat_model()
        response = model.invoke([
            SystemMessage(content=_review_system(state["kind"])),
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
    answer = session.content_text(getattr(response, "content", response))
    verdict, region, reason, shape = parse_review(
        answer, state["shape"], kind=state["kind"])
    out: dict = {"verdict": verdict, "reason": reason, "rounds": rounds,
                 "confidence": _confidence(answer)}
    if alt:
        out["tried_alt"] = True
    if verdict == "adjust" and _inside(region, shape, state.get("frame_size")):
        out["region"] = region
        out["shape"] = shape
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
    if _needs_alt(state):
        return "render"
    # A correction is a candidate, not the answer: the last call picks among
    # them. One candidate means nothing moved, so there is nothing to pick.
    if (state.get("verdict") in ("ok", "adjust")
            and len(state.get("candidates") or []) > 1):
        return "pick"
    return "end"


def score_region(region: dict, shape: str, shot) -> float | None:
    """0..1 for one candidate, the same scale as the judge's own confidence.

    None until a scoring model is wired in: put it behind this function, and
    the pick uses its numbers instead of asking the model.
    """
    return None


def _scores(candidates: list, shot) -> list[float] | None:
    """Every candidate's score, or None when any is missing or raised.

    A partial set is not comparable -- the scoring model only knows boxes, and
    a circle beside them has no number -- so the model picks instead.
    """
    scores = []
    for candidate in candidates:
        region = candidate["region"]
        try:
            score = score_region(region, _shape_of(region), shot)
        except Exception:  # noqa: BLE001 - a broken scorer must not block the call
            return None
        if score is None:
            return None
        scores.append(float(score))
    return scores


def review_pick(state: ReviewState) -> dict:
    """The best attempt, by score when a model is wired, else by the model.

    A later attempt is no better than the first, so every candidate is shown
    the same way -- its cut, then the surroundings where a boundary that took
    the control with it shows -- and the text carries no reason to favour one.
    """
    candidates = state.get("candidates") or []
    first = candidates[0]["region"]
    shot = state.get("shot")
    scores = _scores(candidates, shot)
    if scores is not None:
        # Ties keep the earliest attempt, which is the original proposal.
        best = max(range(len(candidates)), key=lambda i: (scores[i], -i))
        # The number the pick actually used is the one the history should
        # show, in place of the judge's own guess.
        return {"region": candidates[best]["region"], "verdict": "ok",
                "confidence": scores[best],
                "reason": f"scored {scores[best]:.2f}"}
    lines = [
        REVIEW_CRITERIA[state["kind"]],
        f"expectation: {state.get('expect') or '(none given)'}",
    ]
    pictures = []
    for index, candidate in enumerate(candidates, 1):
        region = candidate["region"]
        cut, context, _, _, error = _views(
            region, _shape_of(region), shot, f"candidate {index}")
        if error:
            return {"region": first, "verdict": "ok",
                    "reason": error or "could not compare"}
        pictures += [_picture(cut), _picture(context)]
        lines.append(
            f"candidate {index}: {json.dumps(region, separators=(',', ':'))}")
    lines.append("The pictures follow that order: each candidate's cut, then "
                 "the same region in its surroundings.")
    try:
        model = models.build_chat_model()
        prompt = (TEMPLATE_PICK_PROMPT if state.get("kind") == "template"
                  else PICK_PROMPT)
        response = model.invoke([
            SystemMessage(content=prompt),
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
    key = (label or "").strip()
    if key and key in _accepted:
        return dict(_accepted[key]), _KEPT, ""
    if key and _reject_count.get(key, 0) >= 2:
        return region, "", _reject_reason[key]
    import graph
    out = graph.run_subgraph(_graph(), {
        "kind": kind, "shape": shape, "region": region, "shot": shot,
        "expect": expect, "label": label or expect, "rounds": 0,
        "candidates": [{"region": dict(region), "note": "proposal"}],
    })
    verdict = out.get("verdict")
    if verdict == "reject":
        reason = out.get("reason") or "rejected"
        if key:
            _reject_count[key] = _reject_count.get(key, 0) + 1
            if _reject_count[key] >= 2:
                reason += _STOP
            _reject_reason[key] = reason
        return region, "", reason
    if verdict == "skip":
        return region, f"self-check skipped: {out.get('reason') or 'unreadable'}", ""
    final = out.get("region") or region
    if key:
        _accepted[key] = dict(final)
    body = _moved(shape, region, final)
    if not body and len(out.get("candidates") or []) > 1:
        body = (f"kept the first of {len(out['candidates'])}: "
                f"{out.get('reason') or ''}.{_STOP}")
    return final, _self_check(out, body), ""


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
        shape = _shape_of(cut)
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
    return out, "; ".join(notes), ""


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
