"""The event log: what a killed process leaves, and what the next one reads.

Every case starts from `reset()`, because `session` holds the one conversation
the sidecar serves. A "restart" is module state cleared by hand: `reset()` would
delete the very file under test.
"""

import json

import agent.memorier as memorier
import session
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage


def _attach(tmp_path) -> str:
    session.reset()
    path = str(tmp_path / "turns.jsonl")
    session.attach_log(path)
    return path


def _forget() -> None:
    """What a new process starts with: nothing in memory, the file on disk."""
    session._MESSAGES.clear()
    if session._LOG is not None:
        session._LOG.close()
    session._LOG = None
    session._LOG_PATH = ""
    session._LOG_MARK = None
    session._TURN_SEQ = 0
    session._MEMORIZED_UPTO = 0


def _restart(path: str) -> int:
    """What a new process sees: nothing in memory, the file still on disk."""
    _forget()
    return session.attach_log(path)


def _calls(*specs) -> AIMessage:
    return AIMessage(content="", tool_calls=[
        {"name": name, "args": args, "id": call_id}
        for name, args, call_id in specs])


def _turn(path: str, text: str) -> str:
    """Start a turn the way `build_messages` does."""
    mark = session.new_mark("turn")
    session.log_turn(text, mark)
    return mark


def test_a_finished_turn_survives_a_restart(tmp_path):
    path = _attach(tmp_path)
    mark = _turn(path, "tap it")
    call = _calls(("tap", {"x": 1}, "c1"))
    result = ToolMessage(content="tapped", tool_call_id="c1")
    answer = AIMessage(content="done")
    session.log_intent(call)
    session.log_round([HumanMessage(content="tap it", id=mark), call, result,
                       answer])
    session.log_end("")

    _restart(path)

    kept = session.history()
    assert [type(m) for m in kept] == [
        HumanMessage, AIMessage, ToolMessage, AIMessage]
    assert kept[0].content == "tap it"
    assert kept[0].id == mark
    # The call itself, not a line describing it.
    assert [(c["name"], c["args"]) for c in kept[1].tool_calls] == [
        ("tap", {"x": 1})]
    assert kept[2].content == "tapped"
    assert kept[3].content == "done"
    assert session.size() == 1


def test_a_crash_after_a_batch_keeps_the_turn(tmp_path):
    path = _attach(tmp_path)
    mark = _turn(path, "tap it")
    call = _calls(("tap", {"x": 1}, "c1"))
    result = ToolMessage(content="tapped", tool_call_id="c1")
    session.log_intent(call)
    session.log_round([HumanMessage(content="tap it", id=mark), call, result])

    _restart(path)  # no `end`: the process died here

    kept = session.history()
    assert kept[2].content == "tapped"
    # The batch ran, so the device may have moved even though the turn stops.
    assert kept[-1].content == session.LOG_INTERRUPTED


def test_a_crash_mid_batch_fills_every_call(tmp_path):
    path = _attach(tmp_path)
    _turn(path, "two taps")
    call = _calls(("tap", {"x": 1}, "c1"), ("tap", {"x": 2}, "c2"))
    session.log_intent(call)

    _restart(path)  # died before any result came back

    kept = session.history()
    assert [type(m) for m in kept] == [
        HumanMessage, AIMessage, ToolMessage, ToolMessage, AIMessage]
    # Every call answered: one without a result is a 400 on the next request.
    assert [m.tool_call_id for m in kept[2:4]] == ["c1", "c2"]
    assert all(m.content == session.LOG_INTERRUPTED for m in kept[2:5])
    assert session.LOG_INTERRUPTED in kept[-1].content


def test_the_placeholder_survives_pairing(tmp_path):
    # The reason `intent` gets placeholders at all: without them `_paired`
    # pops the assistant message -- the only evidence the device may have
    # moved -- and says nothing.
    path = _attach(tmp_path)
    _turn(path, "two taps")
    session.log_intent(
        _calls(("tap", {"x": 1}, "c1"), ("tap", {"x": 2}, "c2")))

    _restart(path)

    paired = memorier._paired(session.history())
    assert any(getattr(m, "tool_calls", None) for m in paired)
    assert session.LOG_INTERRUPTED in paired[-1].content


def test_a_crash_before_any_call_keeps_the_instruction(tmp_path):
    path = _attach(tmp_path)
    _turn(path, "把每日领奖跑通")

    _restart(path)

    kept = session.history()
    assert [type(m) for m in kept] == [HumanMessage, AIMessage]
    # What the user asked for is not lost -- only the fact that nothing ran.
    assert kept[0].content == "把每日领奖跑通"
    assert kept[-1].content == session.LOG_NOT_STARTED


def test_an_unfinished_turn_does_not_leave_a_400(tmp_path):
    # `commit` drops a trailing unanswered call before recording a failed turn.
    # Replaying must reach the same history, or the note lands after a dangling
    # call and no later repair reaches it.
    path = _attach(tmp_path)
    mark = _turn(path, "tap it")
    session.log_round([HumanMessage(content="tap it", id=mark),
                       _calls(("tap", {"x": 1}, "c1"))])
    session.log_end("上一轮因 连接中断 中断")

    _restart(path)

    kept = session.history()
    assert [type(m) for m in kept] == [HumanMessage, AIMessage]
    assert kept[-1].content == "上一轮因 连接中断 中断"
    assert not any(getattr(m, "tool_calls", None) for m in kept)


def test_a_half_written_last_line_is_dropped(tmp_path):
    session.reset()
    path = str(tmp_path / "turns.jsonl")
    good = [
        {"v": 1, "kind": "turn", "id": "turn:1", "text": "one", "at": 1.0},
        {"v": 1, "kind": "end", "note": ""},
        {"v": 1, "kind": "turn", "id": "turn:2", "text": "two", "at": 2.0},
        {"v": 1, "kind": "end", "note": ""},
    ]
    half = json.dumps({"v": 1, "kind": "turn", "id": "turn:3", "text": "thr"})
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(json.dumps(r) for r in good) + "\n" + half)

    session.attach_log(path)

    assert [m.content for m in session.history() if isinstance(m, HumanMessage)] \
        == ["one", "two"]
    # The tail is gone from the file too, so the next append is a whole line.
    with open(path, encoding="utf-8") as f:
        assert f.read().endswith("\n")


def test_restoring_no_longer_claims_the_notes_cover_everything(tmp_path):
    """The cursor says what a *pass* folded, not what a restart found.

    Anything written after the last pass was restored but never folded, and
    assuming otherwise dropped those turns permanently.
    """
    path = _attach(tmp_path)
    mark = _turn(path, "one")
    session.log_round([HumanMessage(content="one", id=mark), AIMessage(content="ok")])
    session.log_end("")

    _forget()
    session._MEMORIZED_UPTO = 99
    session._SCRIPTER_MEMORIZED_UPTO = 99
    session.attach_log(path)

    assert session._MEMORIZED_UPTO == 0
    # The scripter's history is not in this log, so its cursor starts over.
    assert session._SCRIPTER_MEMORIZED_UPTO == 0


def test_a_pass_mark_moves_the_cursor_past_that_turn(tmp_path):
    path = _attach(tmp_path)
    first = _turn(path, "one")
    session.log_round([HumanMessage(content="one", id=first), AIMessage(content="ok")])
    session.log_end("")
    second = _turn(path, "two")
    session.log_round([HumanMessage(content="two", id=second), AIMessage(content="ok")])
    session.log_end("")

    _restart(path)
    assert session._MEMORIZED_UPTO == 0

    session.set_memory_upto(first)
    # That turn's own reply was folded with it, so the cursor sits at the next
    # turn's start rather than right after the mark.
    assert str(session._MESSAGES[session._MEMORIZED_UPTO].id) == second

    session.set_memory_upto(second)
    assert session._MEMORIZED_UPTO == len(session._MESSAGES)


def test_an_unknown_pass_mark_leaves_the_cursor_alone(tmp_path):
    # The host's ledger and this history disagree. Folding everything again
    # costs one pass; silently skipping costs the note.
    path = _attach(tmp_path)
    mark = _turn(path, "one")
    session.log_round([HumanMessage(content="one", id=mark), AIMessage(content="ok")])
    session.log_end("")
    _restart(path)

    session.set_memory_upto("turn:999")
    assert session._MEMORIZED_UPTO == 0
    session.set_memory_upto("")
    assert session._MEMORIZED_UPTO == 0


def test_replaying_twice_gives_the_same_history(tmp_path):
    path = _attach(tmp_path)
    mark = _turn(path, "tap it")
    call = _calls(("tap", {"x": 1}, "c1"))
    session.log_intent(call)
    session.log_round([HumanMessage(content="tap it", id=mark), call])
    # No `end`: the turn gets a placeholder on every replay.

    _restart(path)
    once = [(type(m), m.content) for m in session.history()]
    restored = _restart(path)
    twice = [(type(m), m.content) for m in session.history()]

    assert once == twice
    assert len(session.history()) == len(once)
    # Reported once, so the host can say so in a window that started empty.
    assert restored == 1
    assert session.attach_log(path) == 0  # and never twice for one process


def test_reset_removes_the_file(tmp_path):
    path = _attach(tmp_path)
    _turn(path, "one")
    session.log_end("")

    session.reset()
    assert not (tmp_path / "turns.jsonl").exists()

    session.attach_log(path)
    assert session.history() == []


def test_old_turns_are_trimmed(tmp_path):
    path = _attach(tmp_path)
    for n in range(session.LOG_KEEP_TURNS + 5):
        mark = _turn(path, f"turn {n}")
        session.log_round([HumanMessage(content=f"turn {n}", id=mark)])
        session.log_end("")

    _restart(path)

    kept = [m.content for m in session.history() if isinstance(m, HumanMessage)]
    assert len(kept) == session.LOG_KEEP_TURNS
    assert kept[-1] == f"turn {session.LOG_KEEP_TURNS + 4}"
    # Trimmed on disk, not just in memory: the file cannot grow without bound.
    with open(path, encoding="utf-8") as f:
        ids = [json.loads(line)["id"] for line in f
               if json.loads(line)["kind"] == "turn"]
    assert len(ids) == session.LOG_KEEP_TURNS
