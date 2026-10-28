"""A failed template match is diagnosed from the template and the screen."""

import base64
import io

import agent.diagnoser as diagnoser
import graph
import models
import session
import tools
from conftest import ScriptedModel
from langchain_core.messages import AIMessage, ToolMessage
from PIL import Image

_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP8z4AdMOEQH6QSAM1B"
    "AQ/oQeJvAAAAAElFTkSuQmCC"
)
_TEMPLATE = _PNG
_CROP = (
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAEElEQVR4nGP8z8Dwn4EIwAig"
    "AAX2Af8dX0LuAAAAAElFTkSuQmCC"
)


def _png(w, h):
    buf = io.BytesIO()
    Image.new("RGB", (w, h), (30, 30, 30)).save(buf, "PNG")
    return base64.b64encode(buf.getvalue()).decode()


class _Seeing:
    def __init__(self, inner, fail=False):
        self.inner = inner
        self.fail = fail
        self.seen = []

    def bind_tools(self, tools_, **kwargs):
        self.inner.bind_tools(tools_, **kwargs)
        return self

    def invoke(self, messages, *args, **kwargs):
        self.seen.append(messages)
        if self.fail:
            raise RuntimeError("down")
        return self.inner.invoke(messages, *args, **kwargs)


def _images(message):
    return [
        part["image_url"]["url"]
        for part in message.content
        if isinstance(part, dict) and part.get("type") == "image_url"
    ]


def setup_function():
    session.reset()


def _run(monkeypatch, error, template="btn", bundle="b", screen=None,
         verdict="verdict: crop_issue\nreason: r\nfix: f", fail=False,
         template_error=None, interrupt_on=None):
    calls = []
    big = screen if screen is not None else _PNG

    def fake(name, args):
        if name == interrupt_on:
            raise tools.TurnInterrupted()
        calls.append((name, args))
        if name == "screenshot":
            return {"ok": True, "shot": 7, "text": "ok", "image_b64": big}
        if name == "view_template" and template_error:
            return {"ok": False, "error": template_error}
        image = _TEMPLATE if name == "view_template" else _CROP
        return {"ok": True, "text": "ok", "image_b64": image}

    monkeypatch.setattr(tools, "call_cpp", fake)
    model = _Seeing(ScriptedModel(responses=[AIMessage(content=verdict)]), fail)
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    text = diagnoser.run_diagnoser({
        "bundle": bundle, "template": template, "error": error, "expect": "the button",
    })
    return calls, model, text


def test_crop_issue_path_gathers_three_pictures_and_reports_the_shot(monkeypatch):
    big = _png(2000, 1000)
    calls, model, text = _run(
        monkeypatch,
        "tap: image not found /lib/b/btn.png (best=0.612 box=10,20,30,40 threshold=0.800)",
        screen=big,
    )
    assert calls == [
        ("screenshot", {}),
        ("tight_crop", {"region": {"x": 10, "y": 20, "w": 30, "h": 40}}),
        ("view_template", {"bundle": "b", "name": "btn"}),
    ]
    images = _images(model.seen[-1][-1])
    assert len(images) == 3
    assert images[0].endswith(_TEMPLATE)
    assert images[1].endswith(_CROP)
    assert session.png_size(images[2].split(",", 1)[1]) == (800, 400)
    assert text.startswith("verdict: crop_issue\nshot: 7\nreason: r\nfix: f")


def test_swipe_takes_the_box_after_its_template():
    found = diagnoser.parse_match(
        "swipe: not found on the current screen: a.png (best=0.1 box=1,1,1,1 "
        "threshold=0.8) b.png (best=0.2 box=2,2,2,2 threshold=0.8)",
        "b",
    )
    assert found["box"] == {"x": 2, "y": 2, "w": 2, "h": 2}


def test_no_verdict_in_error_calls_nothing(monkeypatch):
    calls, model, text = _run(monkeypatch, "script failed")
    assert text.startswith("verdict: inconclusive\nshot: 0\n")
    assert calls == []
    assert model.seen == []

    calls, model, text = _run(monkeypatch, "tap: image not found /lib/b/btn.png")
    assert text.startswith("verdict: inconclusive\nshot: 0\n")
    assert calls == []
    assert model.seen == []


def test_host_error_is_inconclusive_without_model(monkeypatch):
    calls, model, text = _run(
        monkeypatch,
        "tap: image not found /lib/b/btn.png (best=0.612 box=10,20,30,40 threshold=0.800)",
        template_error="missing",
    )
    assert text.startswith("verdict: inconclusive\n")
    assert "missing" in text
    assert model.seen == []


def test_bad_model_output_is_inconclusive_with_raw_text(monkeypatch):
    _, _, text = _run(
        monkeypatch,
        "tap: image not found /lib/b/btn.png (best=0.612 box=10,20,30,40 threshold=0.800)",
        verdict="maybe the crop",
    )
    assert text.startswith("verdict: inconclusive\n")
    assert "maybe the crop" in text


def test_model_error_is_inconclusive(monkeypatch):
    _, _, text = _run(
        monkeypatch,
        "tap: image not found /lib/b/btn.png (best=0.612 box=10,20,30,40 threshold=0.800)",
        fail=True,
    )
    assert "模型出错：" in text.split("reason: ", 1)[1]


def test_scripter_intercepts_diagnose_and_keeps_host_clean(monkeypatch):
    calls = []

    def fake(name, args):
        calls.append(name)
        image = _png(20, 20) if name == "screenshot" else _PNG
        return {"ok": True, "shot": 7, "text": "ok", "image_b64": image}

    monkeypatch.setattr(tools, "call_cpp", fake)
    model = _Seeing(ScriptedModel(responses=[
        AIMessage(content="", tool_calls=[{
            "name": "author_script", "args": {"goal": "签到"}, "id": "a"}]),
        AIMessage(content="", tool_calls=[{
            "name": "diagnose_template",
            "args": {
                "bundle": "b", "template": "btn",
                "error": "tap: image not found b/btn.png (best=0.6 box=1,1,2,2 threshold=0.8)",
            },
            "id": "d",
        }]),
        AIMessage(content="verdict: crop_issue\nreason: r\nfix: f"),
        AIMessage(content="failed: the cut is loose"),
        AIMessage(content="Done."),
    ]))
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    "".join(chunk for _, chunk in graph.stream_turn(
        {"text": "fix it", "tools": [], "memory": ""}, {}))
    assert "diagnose_template" not in calls
    texts = [m.content for m in session.scripter_history()
             if isinstance(m, ToolMessage)]
    assert texts[0].startswith("verdict:")


def test_interrupt_in_gather_stops_the_outer_turn(monkeypatch):
    def fake(name, args):
        if name == "tight_crop":
            raise tools.TurnInterrupted()
        return {"ok": True, "shot": 7, "text": "ok", "image_b64": _PNG}

    monkeypatch.setattr(tools, "call_cpp", fake)
    model = _Seeing(ScriptedModel(responses=[
        AIMessage(content="", tool_calls=[{
            "name": "author_script", "args": {"goal": "签到"}, "id": "a"}]),
        AIMessage(content="", tool_calls=[{
            "name": "diagnose_template",
            "args": {
                "bundle": "b", "template": "btn",
                "error": "tap: image not found b/btn.png (best=0.6 box=1,1,2,2 threshold=0.8)",
            },
            "id": "d",
        }]),
    ]))
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
    out = {}
    "".join(chunk for _, chunk in graph.stream_turn(
        {"text": "fix it", "tools": [], "memory": ""}, out))
    texts = [m.content for m in session.scripter_history()
             if isinstance(m, ToolMessage)]
    assert texts == [tools.INTERRUPTED_TEXT]
    assert not out.get("error")
