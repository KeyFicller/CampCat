"""A region is checked against its crop before the host acts on it."""

import base64
import io

import agent.region_review as region_review
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

BOX = {"x": 1, "y": 2, "w": 3, "h": 4}


def _png(w, h):
    buf = io.BytesIO()
    Image.new("RGB", (w, h), (20, 20, 20)).save(buf, "PNG")
    return base64.b64encode(buf.getvalue()).decode()


def _tool(name):
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": name,
            "parameters": {
                "type": "object",
                "properties": {"box": {"type": "object"}},
                "required": ["box"],
            },
        },
    }


class _Seeing:
    """Records every invoke, and can fail the review round on purpose."""

    def __init__(self, inner, fail_at=None):
        self.inner = inner
        self.fail_at = fail_at
        self.seen = []
        self.n = 0

    def bind_tools(self, tools_, **kwargs):
        self.inner.bind_tools(tools_, **kwargs)
        return self

    def invoke(self, messages, *args, **kwargs):
        self.n += 1
        self.seen.append(messages)
        if self.fail_at == self.n:
            raise RuntimeError("down")
        return self.inner.invoke(messages, *args, **kwargs)


def _reviews(model):
    """The self-check calls: a user message whose content is a list of pictures."""
    found = []
    for messages in model.seen:
        last = messages[-1]
        if isinstance(getattr(last, "content", None), list):
            found.append(last)
    return found


def _image_size(part):
    b64 = part["image_url"]["url"].split(",", 1)[1]
    return session.png_size(b64)


def setup_function():
    session.reset()
    region_review._human = None


def _drive(monkeypatch, responses, args, name="tap", host=None, images=None,
           fail_at=None, interrupt_on=None):
    calls = []
    shots = dict(images or {})

    def fake_call_cpp(tool, tool_args):
        if tool == interrupt_on:
            raise tools.TurnInterrupted()
        calls.append((tool, tool_args))
        if tool == "tap" and tool_args.get("region", {}).get("x") == 1 and host:
            return host
        if tool == "view_crop" and shots.get("crop_error"):
            return {"ok": False, "error": shots["crop_error"]}
        image = shots.get(tool, _PNG)
        if tool in ("view_crop", "mark"):
            return {"ok": True, "text": "ok", "image_b64": image}
        return {"ok": True, "text": "ok"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    inner = ScriptedModel(responses=responses)
    model = _Seeing(inner, fail_at)
    monkeypatch.setattr(models, "build_chat_model", lambda: model)
    out = {}
    "".join(
        chunk for is_answer, chunk in graph.stream_turn(
            {"text": "go", "tools": [_tool(name)], "memory": ""}, out)
    )
    return calls, model, out


def _say(text):
    return AIMessage(content=text)


def _call(name, args, call_id="c"):
    return AIMessage(content="", tool_calls=[{"name": name, "args": args, "id": call_id}])


def _tap_text():
    return [m.content for m in session.history() if isinstance(m, ToolMessage)]


def test_ok_sends_the_original_region_without_expect(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None)
    assert [c[0] for c in calls] == ["view_crop", "mark", "tap"]
    assert calls[2][1] == {"region": BOX}
    assert "expect" not in calls[2][1]


def test_adjust_then_ok_sends_the_new_region_and_says_so(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tight'),
        _say("verdict: ok\nreason: fine"),
        _say("pick: 2\nreason: tighter"),
        _say("Done."),
    ], None)
    assert calls[-1] == ("tap", {"region": {"x": 2, "y": 2, "w": 3, "h": 3}})
    assert "self-check moved box 1,2 3x4 to 2,2 3x3" in _tap_text()[0]


def test_three_adjusts_then_the_pick_chooses(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: a'),
        _say('verdict: adjust\nbox: {"x":3,"y":3,"w":2,"h":2}\nreason: b'),
        _say('verdict: adjust\nbox: {"x":4,"y":4,"w":2,"h":2}\nreason: c'),
        _say("pick: 2\nreason: the second one"),
        _say("Done."),
    ], None)
    assert len(_reviews(model)) == 4
    # Three looks while correcting, then one crop per candidate for the pick.
    assert [c[0] for c in calls].count("view_crop") == 7
    assert calls[-1][1]["region"] == {"x": 2, "y": 2, "w": 3, "h": 3}


def test_an_unreadable_pick_keeps_the_first_proposal(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tight'),
        _say("verdict: ok\nreason: fine"),
        _say("nope"),
        _say("Done."),
    ], None)
    assert calls[-1][1]["region"] == BOX
    assert "kept the first of 2" in _tap_text()[0]


def test_reject_never_reaches_the_host(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: reject\nreason: wrong element"),
        _say("Done."),
    ], None)
    assert "tap" not in [c[0] for c in calls]
    assert _tap_text() == ["error: self-check rejected this region: wrong element"]


def test_human_region_is_not_reviewed(monkeypatch):
    corrected = {"x": 5, "y": 5, "w": 5, "h": 5}
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}, "c1"),
        _say("verdict: ok\nreason: fine"),
        _call("tap", {"region": corrected, "expect": "btn"}, "c2"),
        _say("Done."),
    ], None, host={
        "ok": True,
        "text": 'tapped; call tap again with this corrected region '
                '{"shot":1,"region":{"x":5,"y":5,"w":5,"h":5}}',
    })
    assert [c[0] for c in calls] == ["view_crop", "mark", "tap", "tap"]
    assert calls[-1][1]["region"] == corrected


def test_bad_output_or_model_error_lets_the_call_through(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("looks good"),
        _say("Done."),
    ], None)
    assert calls[-1][1] == {"region": BOX}
    assert "(self-check skipped:" in _tap_text()[0]

    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("Done."),
    ], None, fail_at=2)
    assert calls[-1][1] == {"region": BOX}
    assert "(self-check skipped:" in _tap_text()[-1]


def test_adjust_off_the_frame_keeps_the_previous_region(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":6,"y":6,"w":5,"h":5}\nreason: big'),
        _say("Done."),
    ], None)
    assert calls[-1][1]["region"] == BOX

    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\ncircle: {"cx":1,"cy":1,"r":1}\nreason: round'),
        _say("Done."),
    ], None)
    assert calls[-1][1]["region"] == BOX


def test_host_error_during_render_lets_the_call_through_without_expect(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("Done."),
    ], None, images={"crop_error": "no screenshot yet"})
    assert calls[-1] == ("tap", {"region": BOX})
    assert "expect" not in calls[-1][1]
    assert _reviews(model) == []


def test_call_without_a_region_is_forwarded_untouched(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {}),
        _say("Done."),
    ], None)
    assert calls == [("tap", {})]
    assert _reviews(model) == []


def test_tap_review_sends_the_neighbourhood(monkeypatch):
    big = _png(1000, 800)
    _, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": {"x": 400, "y": 300, "w": 100, "h": 50}, "expect": "btn"}),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None, images={"mark": big})
    review = _reviews(model)[0]
    assert _image_size(review.content[2]) == (300, 178)
    assert "neighbourhood 300,236 300x178 of the 1000x800 frame" in review.content[0]["text"]


def test_neighbourhood_is_clipped_at_the_frame_edge():
    assert region_review.neighbourhood((0, 0, 10, 10), (100, 80)) == (0, 0, 74, 74)
    assert region_review.neighbourhood((90, 70, 10, 10), (100, 80)) == (26, 6, 74, 74)


def test_save_anchor_reviews_each_template_cut(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("save_anchor", {
            "graph": "g", "anchor": "a", "golden": "---\n---\ncity",
            "source": "return false", "templates": [
                {"name": "a.png", "cut": dict(BOX), "expect": "the city tab"},
            ]}),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None, name="save_anchor")
    assert len(_reviews(model)) == 1
    assert calls[-1] == ("save_anchor", {
        "graph": "g", "anchor": "a", "golden": "---\n---\ncity",
        "source": "return false", "templates": [{"name": "a.png", "cut": dict(BOX)}]})


def test_template_review_uses_its_shot(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}, "t"),
        _say("verdict: ok\nreason: fine"),
        _call("save_template", {
            "bundle": "b", "name": "n", "shot": 2, "region": dict(BOX), "expect": "x",
        }, "s"),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None, name="save_template")
    tap_views = [args for name, args in calls if name in ("view_crop", "mark")][:2]
    assert all("shot" not in args for args in tap_views)
    template_views = [args for name, args in calls if name in ("view_crop", "mark")][2:]
    assert [args["shot"] for args in template_views] == [2, 2]


def test_review_images_stay_out_of_the_caller_history(monkeypatch):
    _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None)
    for message in session.history():
        content = message.content
        if isinstance(content, list):
            assert not any(
                isinstance(part, dict) and part.get("type") == "image_url"
                for part in content)


def test_update_script_rejects_whole_call_and_names_template(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("update_script", {"name": "b", "source": "log(\"a\")", "templates": [
            {"name": "first.png", "cut": {"x": 1, "y": 1, "w": 2, "h": 2}, "expect": "a"},
            {"name": "second.png", "cut": {"x": 1, "y": 1, "w": 2, "h": 2}, "expect": "b"},
        ]}),
        _say("verdict: ok\nreason: fine"),
        _say("verdict: reject\nreason: loose"),
        _say("Done."),
    ], None, name="update_script")
    assert "update_script" not in [c[0] for c in calls]
    assert "second.png" in _tap_text()[0]


def test_update_script_entry_without_cut_is_kept_and_stripped(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("update_script", {"name": "b", "source": "log(\"a\")", "templates": [
            {"name": "keep.png", "expect": "old"},
        ]}),
        _say("Done."),
    ], None, name="update_script")
    assert _reviews(model) == []
    sent = calls[-1][1]["templates"]
    assert sent == [{"name": "keep.png"}]


def test_with_expect_marks_only_region_tools():
    tap = _tool("tap")
    update = {
        "type": "function",
        "function": {
            "name": "update_script",
            "parameters": {
                "type": "object",
                "properties": {
                    "templates": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {"name": {"type": "string"}},
                            "required": ["name"],
                        },
                    },
                },
            },
        },
    }
    shot = _tool("screenshot")
    original = [tap, update, shot]
    out = region_review.with_expect(original)
    assert "expect" in out[0]["function"]["parameters"]["required"]
    items = out[1]["function"]["parameters"]["properties"]["templates"]["items"]
    assert "expect" in items["required"]
    assert out[2] == shot
    assert "expect" not in tap["function"]["parameters"]["required"]
    assert "expect" not in update["function"]["parameters"]["properties"]["templates"]["items"]["required"]


def test_interrupt_inside_review_stops_the_turn(monkeypatch):
    calls, _, out = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("Done."),
    ], None, interrupt_on="view_crop")
    assert "tap" not in [c[0] for c in calls]
    assert _tap_text() == [tools.INTERRUPTED_TEXT]
    assert not out.get("error")
