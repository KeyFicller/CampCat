"""What one handoff taught, written down for the next one.

Two halves, kept apart on purpose. The counts are arithmetic: a repeated
`view_shot`, a failed call, a read for every write is a number, and asking a
model to report it invites it to invent one. Only the sentence about where the
work went in circles needs a model.

Nothing here outlives the process: the notes sit in `session` and are read by
the next handoff's prompt. The host's own consolidation still owns the disk.
"""

from __future__ import annotations

import json
import os
import sys

from langchain_core.messages import HumanMessage, SystemMessage

import models
import observability
import session

# Longest a note may be. It is prepended to the scripter's prompt, so it is
# sent on every round of every later handoff until it is replaced.
MAX_CHARS = 1200

# Longest the trajectory may be before it goes to the model. A busy handoff
# makes 70-plus calls; the tail is what explains the outcome, so the head goes.
MAX_TRAJECTORY = 6000

INSTRUCTIONS = (
    "你要为一个写 Android 设备脚本的助手总结刚结束的一次交接。"
    "给你的是这次交接的轨迹：它调了哪些工具、结果如何，以及它最后的答复。\n"
    "\n"
    "只写这次交接里能看出来的东西，每条一行，最多四条：\n"
    "- 哪一步明显绕了弯，下次该怎么做\n"
    "- 哪个界面或控件的定位反复没过，下次先从哪儿下手\n"
    "- 哪个 bundle、模板或命名被重试过，下次一次写对的要点\n"
    "\n"
    "不要复述数字（已经有人算好了）、不要写当前界面、不要写用户偏好，"
    "不要写已经存进 bundle 的流程。没有值得写的就什么都不输出。\n"
    "\n"
    "只输出这些行，别的什么都不要：没有开场白、没有代码围栏、没有标题，"
    f"总长不超过 {MAX_CHARS} 个字符。"
)

FRAME = (
    "下面是你在前几次交接之后给自己留的笔记，最近的在最上面。用得上的时候就用，"
    "和这份 brief 冲突的行直接丢掉。\n\n"
)


def metrics(calls: list[dict], operator_rounds: int) -> dict:
    """The numbers, from the calls this handoff made.

    `calls` is `[{"name", "args", "failed"}]` in the order they ran. A repeat is
    the same tool with the same arguments twice: for `view_shot` and
    `save_anchor` that is the same request twice, which is the shape of the
    loop this feature exists to catch.
    """
    counts: dict[str, int] = {}
    hints: dict[str, str] = {}
    failed = writes = reads = 0
    for call in calls:
        name = str(call.get("name") or "")
        args = call.get("args") or {}
        # ponytail: the whole argument set is the key, so a tap repeated with
        # different wording for `expect` is not counted. Upgrade to a per-tool
        # key only if repeats go unreported in a smoke run.
        key = name + json.dumps(args, sort_keys=True, ensure_ascii=False)[:200]
        counts[key] = counts.get(key, 0) + 1
        hints.setdefault(key, _args_hint(args))
        if call.get("failed"):
            failed += 1
        if name.startswith(("save_", "update_", "create_")):
            writes += 1
        elif name.startswith(("view_", "read_", "list_")) or name == "screenshot":
            reads += 1
    # The hint rides along because two calls to the same tool with different
    # arguments are two repeats, and the line then named the same tool twice.
    top = [(key.split("{", 1)[0] + hints[key], n)
           for key, n in counts.items() if n > 1]
    return {
        "calls": len(calls), "failed": failed,
        "writes": writes, "reads": reads, "operator_rounds": operator_rounds,
        "repeat": sorted(top, key=lambda pair: -pair[1])[:3],
    }


def _args_hint(args: dict) -> str:
    """Up to two short scalar arguments as `(k=v k=v)`, so two repeats read apart.

    Long ones are left out on purpose: `golden` and `source` carry whole bodies,
    and the point here is to tell two repeats of the same tool apart, not to
    repeat what the tool did.
    """
    parts = [f"{k}={v}" for k, v in args.items()
             if isinstance(v, (str, int, float)) and not isinstance(v, bool)
             and len(str(v)) <= 24 and "\n" not in str(v)][:2]
    return ("(" + " ".join(parts) + ")") if parts else ""


def summary(m: dict) -> str:
    """The numbers as one line, for the model to read as fact.

    "这次"而不是"本次": the same line also reaches the operator who handed this
    off, for whom the handoff it describes is already over.
    """
    if not m["calls"]:
        return ""
    line = (f"这次交接：{m['calls']} 次调用"
            f"（写 {m['writes']}、读 {m['reads']}、失败 {m['failed']}）；"
            f"operator 交接前用了 {m['operator_rounds']} 轮。")
    if m["repeat"]:
        same = "，".join(f"{name} 重复 {n} 次" for name, n in m["repeat"])
        line += "同一调用重复：" + same + "。"
    return line


def reflect(calls: list[dict], operator_rounds: int,
            reply: str) -> tuple[str, str]:
    """(head, body). The head is counted here; the body is what the model saw.

    "" for both when there is nothing to say: no calls, the model failed, or it
    wrote nothing. Never raises: a missed note must not turn a finished handoff
    into a failed tool call.

    The two halves go to different readers -- the body is the lesson the next
    handoff's scripter reads, the head is the number only the operator can act
    on -- so they are not joined here.
    """
    if not calls:
        return "", ""
    m = metrics(calls, operator_rounds)
    head = summary(m)
    try:
        model = models.build_chat_model()
        with observability.turn("reflect") as callbacks:
            answer = model.invoke([
                SystemMessage(content=INSTRUCTIONS),
                HumanMessage(content=(
                    head + "\n\n## 轨迹\n" + _trajectory(calls) +
                    "\n\n## 它最后的答复\n" + (reply or "（没有）")
                )),
            ], config={"callbacks": callbacks})
    except Exception as exc:  # noqa: BLE001 - a missed note, never fatal
        print(f"[sidecar] reflection skipped: {exc}", file=sys.stderr)
        return "", ""
    text = session.strip_tool_markup(
        session.content_text(getattr(answer, "content", answer))).strip()
    if not text:
        return "", ""
    body = text[:MAX_CHARS]
    if os.environ.get("CAMPCAT_LLM_TRACE"):
        note = head + "\n" + body
        print("[reflect] " + note.replace("\n", "\n[reflect] "),
              file=sys.stderr)
    return head, body


def _trajectory(calls: list[dict]) -> str:
    """The calls as one line each, tail first when the whole thing is too long."""
    lines = []
    for call in calls:
        args = json.dumps(call.get("args") or {}, ensure_ascii=False)[:120]
        mark = "失败" if call.get("failed") else "ok"
        lines.append(f"{call.get('name')} {args} -> {mark}")
    joined = "\n".join(lines)
    return joined if len(joined) <= MAX_TRAJECTORY else joined[-MAX_TRAJECTORY:]


def ledger_block(notes: list[str]) -> str:
    """The ledger as a paragraph of the scripter's prompt, newest first.

    Empty in, empty out: with no notes the prompt is byte-identical to what it
    was before this feature existed, which keeps the provider's cache warm.
    """
    kept = [note.strip() for note in notes if note.strip()]
    if not kept:
        return ""
    return "\n\n" + FRAME + "\n".join(kept) + "\n"


def directive(head: str, body: str) -> str:
    """The lesson appended to the `author_script` result, for the operator.

    It rides on the tool result rather than the prompt because the operator's
    prompt is built once per user turn, and a smoke run is one user turn: a
    note there would arrive before the first handoff and never be updated. Said
    as a lesson, not a request, or the operator takes it for the user talking.
    """
    if not body.strip():
        return ""
    parts = [body.strip()]
    if head:
        parts.insert(0, head)
    return ("\n\n上一次交接的教训（用来调整你下一次交接，不是用户的新要求）\n"
            + "\n".join(parts))
