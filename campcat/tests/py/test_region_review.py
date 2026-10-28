"""A region is checked against its crop before the host acts on it."""

import base64
import io
import threading
import time

import agent.region_review as region_review
import graph
import models
import session
import tools
import transport
from conftest import ScriptedModel
from langchain_core.messages import AIMessage, ToolMessage
from PIL import Image

_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP8z4AdMOEQH6QSAM1B"
    "AQ/oQeJvAAAAAElFTkSuQmCC"
)

BOX = {"x": 1, "y": 2, "w": 3, "h": 4}


def test_a_diamond_is_not_read_as_a_circle():
    region = {"cx": 50, "cy": 40, "rx": 10, "ry": 6}
    assert region_review._shape_of(region) == "diamond"
    assert region_review._bounds(region, "diamond") == (40, 34, 21, 13)
    assert region_review._inside(region, "diamond", (100, 80))
    assert not region_review._inside(
        {"cx": 90, "cy": 40, "rx": 10, "ry": 5}, "diamond", (100, 80))
    verdict, got, _, shape = region_review.parse_review(
        'verdict: adjust\n'
        'diamond: {"cx":50,"cy":40,"rx":8,"ry":5}\n'
        "reason: tight",
        "diamond",
        kind="template",
    )
    assert verdict == "adjust"
    assert shape == "diamond"
    assert got == {"cx": 50, "cy": 40, "rx": 8, "ry": 5}


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
        if tool == "tight_crop" and shots.get("crop_error"):
            return {"ok": False, "error": shots["crop_error"]}
        image = shots.get(tool, _PNG)
        if tool in ("tight_crop", "mark"):
            return {"ok": True, "text": "ok", "image_b64": image}
        return {"ok": True, "text": "ok"}

    monkeypatch.setattr(tools, "call_cpp", fake_call_cpp)
    inner = ScriptedModel(responses=responses)
    model = _Seeing(inner, fail_at)
    monkeypatch.setattr(models, "build_chat_model", lambda **_: model)
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
    assert [c[0] for c in calls] == ["tight_crop", "mark", "tap"]
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
    text = _tap_text()[0]
    assert "(自查 1.00；已经改用 box 2,2 3x3。" in text
    assert "这一轮里不要再 view_crop 或重新保存这个区域" in text


def test_two_adjusts_then_the_pick_chooses(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: a'),
        _say('verdict: adjust\nbox: {"x":3,"y":3,"w":2,"h":2}\nreason: b'),
        _say("pick: 2\nreason: the second one"),
        _say("Done."),
    ], None)
    assert len(_reviews(model)) == 3
    # Two looks while correcting, then one crop per candidate for the pick.
    assert [c[0] for c in calls].count("tight_crop") == 5
    assert calls[-1][1]["region"] == {"x": 2, "y": 2, "w": 3, "h": 3}


def test_an_unreadable_pick_keeps_the_first_proposal(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tight'),
        _say('verdict: adjust\nbox: {"x":3,"y":3,"w":2,"h":2}\nreason: tighter'),
        _say("nope"),
        _say("Done."),
    ], None)
    assert calls[-1][1]["region"] == BOX
    assert "3 个候选中保留第一个" in _tap_text()[0]


def test_the_second_look_reuses_the_numbers_the_first_one_gave(monkeypatch):
    # Re-measuring the same crop is what turned one correction into three: the
    # second reading disagreed with the first at the same pixels. The numbers it
    # settled on go into the next look instead.
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: bottom cut'),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None)
    texts = [review.content[0]["text"] for review in _reviews(model)]
    assert "上一轮：（这一轮）" in texts[0]
    assert '上一轮：已经改成 {"x":2,"y":2,"w":3,"h":3}，理由是：bottom cut' in texts[1]
    assert calls[-1][1]["region"] == {"x": 2, "y": 2, "w": 3, "h": 3}


def test_an_ok_after_a_correction_does_not_pick(monkeypatch):
    # The last verdict says the box is usable; another pass over the same
    # candidates could only agree with it, or talk itself out of it.
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tight'),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None)
    assert len(_reviews(model)) == 2
    assert not any(text.startswith("你看到同一个") for text in _systems(model))
    # Two looks, and no crop per candidate for a pick that never ran.
    assert [c[0] for c in calls].count("tight_crop") == 2
    assert calls[-1][1]["region"] == {"x": 2, "y": 2, "w": 3, "h": 3}


def test_reject_never_reaches_the_host(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: reject\nreason: wrong element"),
        _say("Done."),
    ], None)
    assert "tap" not in [c[0] for c in calls]
    assert _tap_text() == ["error: 自查否掉了这个区域：wrong element"]


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
    assert [c[0] for c in calls] == ["tight_crop", "mark", "tap", "tap"]
    assert calls[-1][1]["region"] == corrected


def test_bad_output_or_model_error_lets_the_call_through(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("looks good"),
        _say("Done."),
    ], None)
    assert calls[-1][1] == {"region": BOX}
    assert "(自查跳过：" in _tap_text()[0]

    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("Done."),
    ], None, fail_at=2)
    assert calls[-1][1] == {"region": BOX}
    assert "(自查跳过：" in _tap_text()[-1]


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


def test_the_operator_own_check_reports_a_window(monkeypatch):
    """No handoff in this turn: the check still says what it is doing."""
    sent = []
    monkeypatch.setattr(transport, "emit", sent.append)
    _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None)
    events = [(m["event"], m["agent"]) for m in sent if m["type"] == "subagent"]
    assert events[0] == ("start", "region")
    assert events[-1] == ("end", "region")
    assert sent[0]["text"] == "btn"


def test_tap_review_sends_the_neighbourhood(monkeypatch):
    big = _png(1000, 800)
    _, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": {"x": 400, "y": 300, "w": 100, "h": 50}, "expect": "btn"}),
        _say("verdict: ok\nreason: fine"),
        _say("Done."),
    ], None, images={"mark": big})
    review = _reviews(model)[0]
    assert _image_size(review.content[2]) == (300, 178)
    assert "1000x800 这一帧里 300,236 处 300x178 的邻域" in review.content[0]["text"]


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
    tap_views = [args for name, args in calls if name in ("tight_crop", "mark")][:2]
    assert all("shot" not in args for args in tap_views)
    template_views = [args for name, args in calls if name in ("tight_crop", "mark")][2:]
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
    # Label-keyed, not turn-keyed: the two cuts are checked at once now, so which
    # scripted reply lands on which template would be a race.
    def run(_compiled, state):
        if state["label"] == "second.png":
            return {"verdict": "reject", "reason": "loose"}
        return {"verdict": "ok", "region": dict(state["region"]),
                "candidates": [{"region": dict(state["region"]), "note": "p"}]}

    monkeypatch.setattr(graph, "run_subgraph", run)
    calls, _, _ = _drive(monkeypatch, [
        _call("update_script", {"name": "b", "source": "log(\"a\")", "templates": [
            {"name": "first.png", "cut": {"x": 1, "y": 1, "w": 2, "h": 2}, "expect": "a"},
            {"name": "second.png", "cut": {"x": 1, "y": 1, "w": 2, "h": 2}, "expect": "b"},
        ]}),
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


def _scripted_review(monkeypatch):
    seen = []

    def run(_compiled, state):
        seen.append(state["label"])
        return {"verdict": "ok", "region": dict(state["region"]),
                "candidates": [{"region": dict(state["region"]), "note": "proposal"}]}

    monkeypatch.setattr(graph, "run_subgraph", run)
    return seen


def test_a_checked_template_keeps_its_cut(monkeypatch):
    seen = _scripted_review(monkeypatch)
    region_review.review("save_anchor", {"golden": "v1", "templates": [
        {"name": "dock.png", "cut": dict(BOX), "expect": "the dock"},
    ]})
    out, note, rejected = region_review.review("save_anchor", {
        "golden": "v2",
        "templates": [
            {"name": "dock.png", "cut": {"x": 9, "y": 9, "w": 2, "h": 2},
             "expect": "the dock"},
            {"name": "icon.png", "cut": {"x": 4, "y": 4, "w": 1, "h": 1},
             "expect": "the icon"},
        ],
    })
    assert rejected == ""
    assert out["golden"] == "v2"
    cuts = {entry["name"]: entry["cut"] for entry in out["templates"]}
    assert cuts["dock.png"] == BOX
    assert cuts["icon.png"] == {"x": 4, "y": 4, "w": 1, "h": 1}
    assert sorted(seen) == ["dock.png", "icon.png"]
    assert "这一轮已经检查过了" in note
    # Which box it reused: without it the model cannot tell a stale answer from
    # a correct one, and the operator spent a turn suspecting a bug.
    assert "沿用的是 box 1,2 3x4" in note


def test_templates_in_one_call_are_checked_together(monkeypatch):
    # Serial, three templates in one `save_anchor` were three model round trips
    # back to back -- about 15 s each, a quarter of a 20-minute run. They share
    # nothing, so they run at once; the lock inside `tools` keeps the host's
    # replies from being swallowed.
    live = {"now": 0, "most": 0}
    lock = threading.Lock()

    def run(_compiled, state):
        with lock:
            live["now"] += 1
            live["most"] = max(live["most"], live["now"])
        time.sleep(0.05)
        with lock:
            live["now"] -= 1
        return {"verdict": "ok", "region": dict(state["region"]),
                "candidates": [{"region": dict(state["region"]), "note": "p"}]}

    monkeypatch.setattr(graph, "run_subgraph", run)
    out, _, rejected = region_review.review("save_anchor", {
        "golden": "v",
        "templates": [{"name": f"{n}.png", "cut": dict(BOX)}
                      for n in ("a", "b", "c")],
    })
    assert rejected == ""
    assert live["most"] > 1
    assert [entry["name"] for entry in out["templates"]] == [
        "a.png", "b.png", "c.png"]


def test_a_second_rejection_is_the_last_review(monkeypatch):
    seen = []

    def run(_compiled, state):
        seen.append(state["label"])
        return {"verdict": "reject", "reason": "wrong element"}

    monkeypatch.setattr(graph, "run_subgraph", run)
    args = {"region": dict(BOX), "expect": "btn"}
    _, _, first = region_review.review("tap", args)
    assert first == "wrong element"
    _, _, second = region_review.review("tap", {
        "region": {"x": 9, "y": 9, "w": 2, "h": 2}, "expect": "btn"})
    assert "这一轮里不要再 view_crop" in second
    _, _, third = region_review.review("tap", {
        "region": {"x": 8, "y": 8, "w": 2, "h": 2}, "expect": "btn"})
    assert third == second
    assert seen == ["btn", "btn"]


def test_interrupt_inside_review_stops_the_turn(monkeypatch):
    calls, _, out = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("Done."),
    ], None, interrupt_on="tight_crop")
    assert "tap" not in [c[0] for c in calls]
    assert _tap_text() == [tools.INTERRUPTED_TEXT]
    assert not out.get("error")


CIRCLE = {"cx": 3, "cy": 3, "r": 2}


def _systems(model):
    found = []
    for messages in model.seen:
        content = getattr(messages[0], "content", "")
        if isinstance(content, str):
            found.append(content)
    return found


def test_tap_adjust_to_a_circle_is_skipped(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\ncircle: {"cx":3,"cy":3,"r":2}\nreason: round'),
        _say("Done."),
    ], None)
    assert [c for c in calls if c[0] == "tap"] == [("tap", {"region": BOX})]


def test_template_adjust_to_a_circle_is_what_the_host_gets(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("save_template", {"region": dict(BOX), "expect": "round button"}),
        _say('verdict: adjust\ncircle: {"cx":3,"cy":3,"r":2}\nreason: round control'),
        _say("verdict: ok\nreason: rim sits on it"),
        _say("pick: 2\nreason: rule 1"),
        _say("Done."),
    ], None, name="save_template")
    crops = [c[1]["region"] for c in calls if c[0] == "tight_crop"]
    assert crops[1] == CIRCLE
    assert [c for c in calls if c[0] == "save_template"] == [
        ("save_template", {"region": CIRCLE})]
    assert any("circle r=2" in text for text in _tap_text())


def test_template_pick_follows_the_crop_rules(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("save_template", {"region": dict(BOX), "expect": "round button"}),
        _say('verdict: adjust\ncircle: {"cx":3,"cy":3,"r":2}\nreason: round control'),
        _say('verdict: adjust\ncircle: {"cx":3,"cy":3,"r":3}\nreason: rim'),
        _say("pick: 2\nreason: rule 1, the circle"),
        _say("Done."),
    ], None, name="save_template")
    picks = [text for text in _systems(model) if text.startswith("你看到同一个")]
    assert picks
    assert "圆" in picks[-1]
    assert "留白" in picks[-1]
    assert "best matches" not in picks[-1]
    assert "correction of the one before" not in picks[-1]
    assert [c for c in calls if c[0] == "save_template"][-1][1]["region"] == CIRCLE


def test_unreadable_template_pick_keeps_the_first_cut(monkeypatch):
    calls, _, _ = _drive(monkeypatch, [
        _call("save_template", {"region": dict(BOX), "expect": "round button"}),
        _say('verdict: adjust\ncircle: {"cx":3,"cy":3,"r":2}\nreason: round control'),
        _say('verdict: adjust\ncircle: {"cx":3,"cy":3,"r":3}\nreason: rim'),
        _say("nope"),
        _say("Done."),
    ], None, name="save_template")
    assert [c for c in calls if c[0] == "save_template"] == [
        ("save_template", {"region": BOX})]
    assert any("3 个候选中保留第一个" in text for text in _tap_text())


def test_a_low_confidence_ok_gets_one_more_look(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nconfidence: 0.2\nreason: maybe"),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\n'
             "confidence: 0.7\nreason: tighter"),
        _say("verdict: ok\nconfidence: 0.9\nreason: fine"),
        _say("pick: 1\nreason: the first"),
        _say("Done."),
    ], None)
    texts = [review.content[0]["text"] for review in _reviews(model)]
    assert sum("上一次你没把握" in text for text in texts) == 1
    assert calls[-1][1]["region"] == BOX


def test_a_sure_ok_stops_at_once(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nconfidence: 0.9\nreason: fine"),
        _say("Done."),
    ], None)
    assert len(_reviews(model)) == 1
    assert calls[-1] == ("tap", {"region": BOX})


def test_the_score_shows_up_in_the_history(monkeypatch):
    _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nconfidence: 0.35\nreason: maybe"),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\n'
             "confidence: 0.8\nreason: tighter"),
        _say("pick: 1\nreason: the first"),
        _say("Done."),
    ], None)
    # The last verdict is the one reported, in the same bracket as what the
    # check did about it.
    assert "(自查 0.80；2 个候选中保留第一个：the first。" in _tap_text()[0]


def test_a_skipped_check_keeps_its_own_wording(monkeypatch):
    _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("looks good"),
        _say("Done."),
    ], None)
    text = _tap_text()[0]
    assert "(自查跳过：" in text
    assert "(自查 0." not in text


def test_the_score_is_shown_before_the_tool_runs(monkeypatch):
    notes = []
    monkeypatch.setattr(tools, "note", notes.append)
    calls, _, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: ok\nconfidence: 0.4\nreason: maybe"),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\n'
             "confidence: 0.9\nreason: tighter"),
        _say("pick: 1\nreason: the first"),
        _say("Done."),
    ], None)
    # Once, before the host call: the approval gate reads the conversation, not
    # the tool result, and the result only exists after it is answered.
    assert notes == ["自查 0.90；2 个候选中保留第一个：the first。"
                     " 这一轮里不要再 view_crop 或重新保存这个区域，"
                     "除非工具返回了错误。"]
    assert calls[-1][0] == "tap"


def test_a_rejected_region_says_nothing_about_its_score(monkeypatch):
    notes = []
    monkeypatch.setattr(tools, "note", notes.append)
    _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say("verdict: reject\nconfidence: 0.1\nreason: wrong element"),
        _say("Done."),
    ], None)
    assert notes == []


def test_the_pick_judges_each_attempt_on_its_own(monkeypatch):
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tightened'),
        _say('verdict: adjust\nbox: {"x":3,"y":3,"w":3,"h":3}\nreason: wider'),
        _say("pick: 1\nreason: the first"),
        _say("Done."),
    ], None)
    picks = [text for text in _systems(model) if text.startswith("你看到同一个")]
    assert picks and "既不比第一次好，也不比它差" in picks[-1]
    assert "周围环境" in picks[-1]
    # One text and two pictures per candidate; the judge's reason would only
    # make the later attempt sound better.
    wide = [review for review in _reviews(model) if len(review.content) > 3]
    assert len(wide) == 1
    assert len(wide[0].content) == 1 + 2 * 3
    assert "tightened" not in wide[0].content[0]["text"]
    assert "候选 1" in wide[0].content[0]["text"]
    assert [c[0] for c in calls].count("tight_crop") == 2 + 3
    assert [c[0] for c in calls].count("mark") == 2 + 3


def test_a_wired_scorer_picks_the_highest(monkeypatch):
    scores = {1: 0.3, 2: 0.8}
    monkeypatch.setattr(
        region_review, "score_region",
        lambda region, shape, shot: scores[region["x"]])
    calls, model, _ = _drive(monkeypatch, [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tighter'),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\n'
             "confidence: 0.8\nreason: still tight"),
        _say("Done."),
    ], None)
    assert calls[-1] == ("tap", {"region": {"x": 2, "y": 2, "w": 3, "h": 3}})
    assert [c[0] for c in calls].count("tight_crop") == 2
    assert [review for review in _reviews(model) if len(review.content) > 3] == []
    assert "(自查 0.80；" in _tap_text()[0]


def test_a_half_scored_or_broken_scorer_falls_back_to_the_model(monkeypatch):
    def half(region, shape, shot):
        return 0.5 if region["x"] == 1 else None

    script = [
        _call("tap", {"region": dict(BOX), "expect": "btn"}),
        _say('verdict: adjust\nbox: {"x":2,"y":2,"w":3,"h":3}\nreason: tighter'),
        _say('verdict: adjust\nbox: {"x":3,"y":3,"w":3,"h":3}\nreason: wider'),
        _say("pick: 1\nreason: the first"),
        _say("Done."),
    ]
    monkeypatch.setattr(region_review, "score_region", half)
    calls, model, _ = _drive(monkeypatch, list(script), None)
    assert calls[-1][1]["region"] == BOX
    assert [review for review in _reviews(model) if len(review.content) > 3]

    def broken(region, shape, shot):
        raise RuntimeError("no model")

    monkeypatch.setattr(region_review, "score_region", broken)
    calls, model, _ = _drive(monkeypatch, list(script), None)
    assert calls[-1][1]["region"] == BOX
    assert [review for review in _reviews(model) if len(review.content) > 3]
