"""The note a handoff leaves for the next one: the numbers, then the sentence.

The conftest silences `reflect` for every case; the ones that want the real
thing call `_real_reflect`, the function captured before that patch went on.
"""

import agent.reflector as reflector
import models
import session
from langchain_core.messages import AIMessage

_real_reflect = reflector.reflect


def _call(name, args=None, failed=False):
    return {"name": name, "args": args or {}, "failed": failed}


def test_a_repeated_call_is_counted_once_per_argument_set():
    m = reflector.metrics([
        _call("view_shot", {"shot": 15}),
        _call("view_shot", {"shot": 15}),
        _call("view_shot", {"shot": 15}),
        _call("view_shot", {"shot": 16}),
    ], operator_rounds=3)

    assert m["repeat"] == [("view_shot(shot=15)", 3)]
    assert m["calls"] == 4


def test_a_different_argument_is_not_a_repeat():
    m = reflector.metrics([
        _call("tap", {"x": 1, "y": 2}),
        _call("tap", {"x": 3, "y": 4}),
    ], operator_rounds=0)

    assert m["repeat"] == []


def test_failures_and_reads_and_writes_are_separate():
    m = reflector.metrics([
        _call("view_shot", failed=True),
        _call("save_anchor"),
        _call("update_bundle"),
        _call("list_scripts"),
    ], operator_rounds=5)

    assert (m["failed"], m["writes"], m["reads"]) == (1, 2, 2)
    assert m["operator_rounds"] == 5


def test_the_summary_names_the_numbers():
    line = reflector.summary(reflector.metrics([
        _call("view_shot", {"shot": 15}),
        _call("view_shot", {"shot": 15}),
    ], operator_rounds=7))

    assert "2 次调用" in line
    assert "view_shot(shot=15) 重复 2 次" in line
    assert "7 轮" in line


def test_two_repeats_of_the_same_tool_read_apart():
    """Both had repeated twice, and the line named `save_anchor` twice over."""
    line = reflector.summary(reflector.metrics([
        _call("save_anchor", {"anchor": "home"}),
        _call("save_anchor", {"anchor": "home"}),
        _call("save_anchor", {"anchor": "entry"}),
        _call("save_anchor", {"anchor": "entry"}),
    ], operator_rounds=0))

    assert "save_anchor(anchor=home) 重复 2 次" in line
    assert "save_anchor(anchor=entry) 重复 2 次" in line


def test_a_long_argument_is_left_out_of_the_hint():
    # `golden` carries a whole body; repeating it would put the body in the line.
    line = reflector.summary(reflector.metrics([
        _call("save_anchor", {"anchor": "home", "golden": "---\nkind: page\n---\n" * 9}),
        _call("save_anchor", {"anchor": "home", "golden": "---\nkind: page\n---\n" * 9}),
    ], operator_rounds=0))

    assert "save_anchor(anchor=home) 重复 2 次" in line
    assert "kind:" not in line


def test_a_handoff_that_called_nothing_gets_no_note(monkeypatch):
    monkeypatch.setattr(models, "build_chat_model",
                        lambda **_: (_ for _ in ()).throw(AssertionError))

    assert _real_reflect([], 0, "done") == ("", "")


def test_the_head_is_the_numbers_and_the_body_is_the_lesson(
        monkeypatch, scripted):
    monkeypatch.setattr(models, "build_chat_model", lambda **_: scripted(
        [AIMessage(content="# chat\n我多看了两次同一帧。")]))

    head, body = _real_reflect([_call("view_shot", {"shot": 15}),
                                _call("view_shot", {"shot": 15})], 2, "done")

    assert head.startswith("这次交接：2 次调用")
    assert "我多看了两次同一帧。" in body
    assert "2 次调用" not in body


def test_the_body_is_capped(monkeypatch, scripted):
    monkeypatch.setattr(models, "build_chat_model", lambda **_: scripted(
        [AIMessage(content="很长" * 5000)]))

    _, body = _real_reflect([_call("view_shot")], 0, "done")

    assert len(body) == reflector.MAX_CHARS


def test_a_model_that_raises_leaves_no_note(monkeypatch):
    def broken(**_):
        raise RuntimeError("no endpoint")
    monkeypatch.setattr(models, "build_chat_model", broken)

    assert _real_reflect([_call("view_shot")], 0, "done") == ("", "")


def test_an_empty_ledger_leaves_the_prompt_byte_for_byte_alone():
    assert reflector.ledger_block([]) == ""
    assert reflector.ledger_block(["", "  \n"]) == ""


def test_a_ledger_is_framed_and_kept_newest_first():
    block = reflector.ledger_block(["后一条。", "前一条。"])

    assert block.startswith("\n\n下面是你")
    assert block.index("后一条。") < block.index("前一条。")


def test_the_trajectory_keeps_the_tail_when_it_is_too_long():
    many = [_call("tap", {"note": "x" * 200}) for _ in range(60)]

    text = reflector._trajectory(many)

    assert len(text) == reflector.MAX_TRAJECTORY
    assert text.endswith("-> ok")


def test_two_handoffs_both_stay_in_the_ledger():
    session.reset()
    session.add_lessons("早的那条。")
    session.add_lessons("晚的那条。")

    assert session.lessons() == ["晚的那条。", "早的那条。"]


def test_the_oldest_note_is_dropped_whole():
    """Half a lesson reads as an instruction, so it goes whole."""
    session.reset()
    for tag in ("A", "B", "C"):
        session.add_lessons(tag * 1000)

    kept = session.lessons()

    assert len(kept) == 1
    assert kept[0] == "C" * 1000


def test_a_note_over_the_cap_on_its_own_is_cut():
    session.reset()
    session.add_lessons("D" * 5000)

    assert len(session.lessons()) == 1
    assert len(session.lessons()[0]) == session.LEDGER_MAX_CHARS


def test_empties_leave_the_ledger_alone():
    session.reset()
    session.add_lessons("留着。")
    session.add_lessons("   ")

    assert session.lessons() == ["留着。"]


def test_a_directive_says_it_is_not_a_request():
    """The operator reads tool results as instruction, so this one must say
    which side it is on -- otherwise it treats the lesson as the user talking."""
    text = reflector.directive("这次交接：2 次调用。", "- 先看清再裁。")

    assert "不是用户的新要求" in text
    assert "这次交接：2 次调用。" in text
    assert "先看清再裁。" in text


def test_no_body_leaves_the_operator_result_alone():
    assert reflector.directive("这次交接：2 次调用。", "") == ""
    assert reflector.directive("", "  \n") == ""

