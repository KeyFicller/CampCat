"""`consult`: one yes/no question about one screenshot, answered by the model.

Strict parsing is the point -- a wrong yes or no picks a wrong script branch --
and so is leaving the conversation alone: the call must not touch `session`.
"""

import pytest

import consult
import models
from conftest import ScriptedModel
from langchain_core.messages import AIMessage, HumanMessage, SystemMessage

_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP8z4AdMOEQH6QSAM1B"
    "AQ/oQeJvAAAAAElFTkSuQmCC"
)


class _Seen:
    """Records every invoke, so a case can read what the model was handed."""

    def __init__(self, inner):
        self.inner = inner
        self.seen = []

    def invoke(self, messages, *args, **kwargs):
        self.seen.append(messages)
        return self.inner.invoke(messages, *args, **kwargs)


def _model(monkeypatch, responses):
    seen = _Seen(ScriptedModel(responses=responses))
    monkeypatch.setattr(models, "build_chat_model", lambda **_: seen)
    return seen


def _request(question="Is there a claim button?", image=_PNG):
    return {"type": "consult", "id": 7, "question": question,
            "image_b64": image}


@pytest.mark.parametrize("text,want", [
    ("yes", "yes"),
    (" YES\n", "yes"),
    ("no.", "no"),
    ("No", "no"),
    ("unsure", ""),
    ("maybe", ""),
    ("", ""),
])
def test_only_a_yes_or_no_counts_as_an_answer(text, want):
    assert consult.parse_answer(text) == want


def test_a_yes_comes_back_with_the_hosts_id(monkeypatch):
    seen = _model(monkeypatch, [AIMessage(content="yes")])
    out = consult.answer(_request())

    assert out == {"type": "consult_result", "id": 7, "ok": True,
                   "answer": "yes", "error": ""}
    # Two messages, no history: the question and the picture.
    assert len(seen.seen) == 1
    system, human = seen.seen[0]
    assert isinstance(system, SystemMessage)
    assert isinstance(human, HumanMessage)
    assert human.content[0] == {"type": "text",
                                "text": "Is there a claim button?"}
    assert human.content[1]["type"] == "image_url"


def test_an_unsure_answer_travels_as_an_empty_one(monkeypatch):
    # `ok` says the call ran; the empty answer is what stops the script, and the
    # host owns that wording.
    _model(monkeypatch, [AIMessage(content="unsure")])
    out = consult.answer(_request())
    assert out["ok"] is True
    assert out["answer"] == ""


def test_an_empty_question_or_picture_never_reaches_the_model(monkeypatch):
    seen = _model(monkeypatch, [AIMessage(content="yes")])
    for req in (_request(question="  "), _request(image="")):
        out = consult.answer(req)
        assert out["ok"] is False
        assert out["error"] == "问题或图片为空"
    assert seen.seen == []


def test_a_failing_model_is_an_error_not_a_crash(monkeypatch):
    class _Broken:
        def invoke(self, *_args, **_kwargs):
            raise RuntimeError("no api key")

    monkeypatch.setattr(models, "build_chat_model", lambda **_: _Broken())
    out = consult.answer(_request())
    assert out["ok"] is False
    assert out["error"] == "no api key"
