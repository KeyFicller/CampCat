"""Answer one yes/no question about a picture, for a `.ccat` `ask()`.

Deliberately not a conversation: two messages, no tools, no history. A script
that stops to ask may be running from the Run button while a turn is in flight,
and answering it must not touch `session`, `graph`, or the turn's transcript.
"""

from __future__ import annotations

from typing import Any

from langchain_core.messages import HumanMessage, SystemMessage

import models
import protocol
import session

# The model sees one screenshot and one question. "unsure" is the way out: a
# wrong yes or no picks a wrong branch, and the script would rather stop.
_SYSTEM = (
    "你回答关于一张 Android 截图的一个是/否问题。只看图片。"
    "只回一个词，照抄英文原样：yes、no 或 unsure。"
    "图片里显示的信息不足以判断时，回答 unsure；永远不要猜。"
)


def parse_answer(text: str) -> str:
    """'yes' / 'no' / '' -- strict, because a guess picks a branch."""
    word = (text or "").strip().strip(".").lower()
    return word if word in ("yes", "no") else ""


def answer(req: dict) -> dict:
    """The `consult_result` for one `consult` request."""
    rid = req.get("id")
    question = str(req.get("question") or "").strip()
    image = req.get("image_b64") or ""
    if not question or not image:
        return protocol.build_consult_result(
            rid, False, error="问题或图片为空")
    try:
        reply: Any = models.build_chat_model().invoke([
            SystemMessage(content=_SYSTEM),
            HumanMessage(content=session._shot_content(question, image)),
        ])
    except Exception as exc:  # noqa: BLE001 - the script stops, the loop lives
        return protocol.build_consult_result(rid, False, error=str(exc))
    # `ok` means the call ran; an answer that is neither word travels as an
    # empty one and the host reports it as a script error.
    return protocol.build_consult_result(
        rid, True, parse_answer(session.content_text(reply.content)))
