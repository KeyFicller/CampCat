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
import subagent
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
    "一句话：这个区域里应该是什么，例如「领取按钮，不带它下面的面板」。"
    "调用真正执行之前，裁切会先拿它对照检查。"
)

_PRESSURE = (
    "先假定这个区域是错的，直到你找过毛病却没找到为止。这不是你画的："
    "第一次的提议都带边距，已经修过一次的又可能改过了头。指出最可能出错的那"
    "一条边——左、右、上、下，或者形状本身——只有当这个裁切你会原样交给之后"
    "每一次运行使用时，才回答 ok。你签字放过的裁切，在这次检查消失很久之后"
    "还在被用，所以看见了却放过才是贵的那个。调整很便宜，而且只参与比较、"
    "不被信任：差的一次会被丢掉，所以你没有任何理由接受一个自己都不用的裁切。"
    "置信度也不是投票：低的只是多买一次复查，不扣任何分；高的只是让检查提前"
    "结束。"
)

REVIEW_PROMPT = (
    "你在检查别的 agent 提议的一个区域，它还没被使用。你看到两张图：这张区域"
    "按自身尺寸裁下来的样子，以及把该区域画上去的第二张图——按文字说明，"
    "可能是它周围的原分辨率邻域，也可能是整帧缩小后的画面。严格按下面的格式"
    "回答，不要写别的：\n"
    "verdict: ok | adjust | reject\n"
    "confidence: 0.0-1.0   （你对这个区域正确的把握有多大；0 = 纯猜）\n"
    "box: {\"x\":..,\"y\":..,\"w\":..,\"h\":..}   （只在 adjust 时写；圆形区域写 "
    "circle: {\"cx\":..,\"cy\":..,\"r\":..}；菱形区域写 "
    "diamond: {\"cx\":..,\"cy\":..,\"rx\":..,\"ry\":..}）\n"
    "reason: 一句话\n"
    "verdict、confidence、reason、box、circle、diamond 这几个键名，以及 "
    "ok / adjust / reject，都要照抄英文原样，不要翻译。\n"
    "边只做小修正时用 adjust，坐标用这一帧的像素。区域落在错误的元素上、"
    "或者这个元素根本不在这帧里，用 reject。\n" + _PRESSURE
)

_ALTERNATIVE = (
    "上一次你没把握。再看一遍，把最强的另一种可能写成 `adjust`，"
    "控件也可能是别的形状时，就在 circle、diamond、box 之间换。"
    "这一次可能更差：它只是一个候选，之后会在其中挑最好的。"
)

PICK_PROMPT = (
    "你看到同一个区域的所有尝试，按顺序排列：第一次的提议，以及之后的每一次。"
    "每一个都先给裁切图，再给把该区域画在周围环境里的图。序号只是用来标识"
    "哪一次：靠后的既不比第一次好，也不比它差。"
    "挑出最符合期望的那一个裁切：它指名的控件，贴着它、不多余、也没被切掉。"
    "严格按下面的格式回答：\n"
    "pick: <序号>\n"
    "reason: 一句话\n"
    "pick 和 reason 这两个键名照抄英文，不要翻译。"
)

# Shared by the one-crop check and the pick, so the two cannot drift.
_TEMPLATE_RULES = (
    "按顺序套用下面的规则，遇到第一条能定的就停。颜色、以及一个裁切看起来"
    "多像一个按钮，都不算数。\n"
    "1. 文字边界。控件是文字时——一个标签、一行菜单、一个按钮文案——字形就是"
    "控件：边落在第一笔和最后一笔上，是一个恰好框住那些像素的方块。"
    "不是字底下的按钮、胶囊或面板，也不为了可读性留白。\n"
    "2. 形状。圆形控件（圆）只留 circle {cx,cy,r}。菱形控件（菱形）只留 "
    "diamond {cx,cy,rx,ry}。矩形控件只留 box {x,y,w,h}。只要还有形状正确的，"
    "就把错的丢掉。圆形控件外面套方块是错的形状。宽得远超高的菱形，是方块，"
    "不是菱形。\n"
    "3. 切边。只要还有完整的一个，就把把控件切掉的那些丢掉。圆看的是它的"
    "圆弧，不是圆盘外的四个角。菱形看的是它的四个顶点，不是它外接方块的角。\n"
    "4. 贴边。边正好压在控件上的裁切，胜过硬边与控件之间夹着背景（留白）的"
    "那个。只要还有贴边的，就把带那道缝的丢掉。\n"
    "5. 一个都不贴边时，留留白最少的那一个。\n"
    "6. 还剩多个时，留序号最小的那个，也就是第一次的提议。"
)

TEMPLATE_PICK_PROMPT = (
    "你看到同一个模板的所有尝试，按顺序排列：第一次的提议，以及之后的每一次。"
    "每一个都先给裁切图，再给把该区域画在周围环境里的图。序号只是用来标识"
    "哪一次：靠后的既不比第一次好，也不比它差。\n"
    + _TEMPLATE_RULES +
    "\n第 3 条（把控件切掉）要看周围环境那张图判断，不能只看裁切本身。"
    "\n严格按下面的格式回答：\n"
    "pick: <序号>\n"
    "reason: 一句话，说明是哪一条规则定的\n"
    "pick 和 reason 这两个键名照抄英文，不要翻译。"
)

REVIEW_CRITERIA = {
    "tap": "这个区域是要被点的：它必须落在期望里点名的那个元素上。",
    "template": (
        "这个区域会变成一个模板。\n" + _TEMPLATE_RULES + "\n"
        "就这一张裁切来看，只有形状对、边也已经压在控件上时（控件是文字时，"
        "压在字形上）才回答 ok。否则 adjust 到那个边界，形状不对就在 circle、"
        "diamond、box 之间换。只有区域落在错误的元素上、或者元素根本不在这帧里，"
        "才用 reject。"
    ),
}


def _review_system(kind: str) -> str:
    if kind == "template":
        extra = (
            "模板的 adjust 可以换形状：圆形控件写 circle:，菱形控件写 diamond:，"
            "矩形控件写 box:。"
        )
    else:
        extra = (
            "box、circle、diamond 这三个键里，用你收到的这个区域本身的形状。"
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
_STOP = (" 这一轮里不要再 view_crop 或重新保存这个区域，"
         "除非工具返回了错误。")
_KEPT = "这一轮已经检查过了，沿用上一次的区域。" + _STOP
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
    return f"已经改用 {where}。{_STOP}"


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
    return f"自查{tag}{'；' + body if body else ''}"


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
        return cut, marked_b64, "第二张图：整帧", (), ""
    box = neighbourhood(_bounds(region, shape), frame)
    context = session.crop_png(marked_b64, box)
    if not context:
        return (cut, marked_b64,
                f"第二张图：整帧 {frame[0]}x{frame[1]}",
                frame, "")
    note = (f"第二张图：{frame[0]}x{frame[1]} 这一帧里 "
            f"{box[0]},{box[1]} 处 {box[2]}x{box[3]} 的邻域，原分辨率；"
            f"坐标用帧像素")
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
        f"期望：{state.get('expect') or '（未给出）'}\n"
        f"区域：{json.dumps(state['region'], separators=(',', ':'))}\n"
        f"{state.get('context_note', '')}"
    ) + session.ASK_IN_CHINESE
    alt = _needs_alt(state)
    if alt:
        text += "\n" + _ALTERNATIVE
    try:
        model = models.build_chat_model()
        response = models.streamed(model, [
            SystemMessage(content=_review_system(state["kind"])),
            HumanMessage(content=[
                {"type": "text", "text": text},
                _picture(state.get("crop_b64") or ""),
                _picture(state.get("context_b64") or ""),
            ]),
        ], subagent.chunk_sink)
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
                "reason": f"打分 {scores[best]:.2f}"}
    lines = [
        REVIEW_CRITERIA[state["kind"]],
        f"期望：{state.get('expect') or '（未给出）'}",
    ]
    pictures = []
    for index, candidate in enumerate(candidates, 1):
        region = candidate["region"]
        cut, context, _, _, error = _views(
            region, _shape_of(region), shot, f"候选 {index}")
        if error:
            return {"region": first, "verdict": "ok",
                    "reason": error or "无法比较"}
        pictures += [_picture(cut), _picture(context)]
        lines.append(
            f"候选 {index}：{json.dumps(region, separators=(',', ':'))}")
    lines.append("上面的图按这个顺序排：每个候选先给裁切图，"
                 "再给同一区域在周围环境里的样子。")
    lines.append(session.ASK_IN_CHINESE.strip())
    try:
        model = models.build_chat_model()
        prompt = (TEMPLATE_PICK_PROMPT if state.get("kind") == "template"
                  else PICK_PROMPT)
        response = models.streamed(model, [
            SystemMessage(content=prompt),
            HumanMessage(content=[
                {"type": "text", "text": "\n".join(lines)}, *pictures]),
        ], subagent.chunk_sink)
    except tools.TurnInterrupted:
        raise
    except Exception as exc:  # noqa: BLE001 - an unreadable pick keeps the proposal
        return {"region": first, "verdict": "ok", "reason": str(exc)}
    text = session.content_text(getattr(response, "content", response))
    number = parse_pick(text, len(candidates))
    if number is None:
        return {"region": first, "verdict": "ok",
                "reason": "读不懂这个选择，保留了第一个提议"}
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
    out = {}
    # Reported for the operator's own calls too: the check is a model loop of
    # its own, and the bubble only says how it ended.
    with subagent.working("region", label or expect) as run:
        out = graph.run_subgraph(_graph(), {
            "kind": kind, "shape": shape, "region": region, "shot": shot,
            "expect": expect, "label": label or expect, "rounds": 0,
            "candidates": [{"region": dict(region), "note": "proposal"}],
        })
        run.settle(out.get("verdict") or "checked")
    verdict = out.get("verdict")
    if verdict == "reject":
        reason = out.get("reason") or "已拒绝"
        if key:
            _reject_count[key] = _reject_count.get(key, 0) + 1
            if _reject_count[key] >= 2:
                reason += _STOP
            _reject_reason[key] = reason
        return region, "", reason
    if verdict == "skip":
        return region, f"自查跳过：{out.get('reason') or '读不懂'}", ""
    final = out.get("region") or region
    if key:
        _accepted[key] = dict(final)
    body = _moved(shape, region, final)
    if not body and len(out.get("candidates") or []) > 1:
        body = (f"{len(out['candidates'])} 个候选中保留第一个："
                f"{out.get('reason') or ''}。{_STOP}")
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
    return out, "；".join(notes), ""


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
