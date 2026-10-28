"""Host calls from more than one thread.

Template checks run on a pool, so two `call_cpp` can be in flight at once. The
host answers them one at a time and a waiter throws away a result that is not
its own -- which turned a second call arriving early into two waiters that never
woke up, and a turn that ended only on the host's timeout.
"""

import threading
import time

import tools
import transport
from collections import deque


def _fake_host(monkeypatch):
    """A host that answers the newest waiting call, and nothing else.

    Newest first is the whole point: the older waiter is handed a result that is
    not its own, drops it, and has nothing left to wake it.
    """
    lock = threading.Lock()
    waiting: list[str] = []
    answers: deque = deque()

    def emit(message: dict) -> None:
        with lock:
            waiting.append(message["call_id"])

    def host() -> None:
        while True:
            with lock:
                call_id = waiting.pop() if waiting else None
            if call_id is None:
                time.sleep(0.002)
                continue
            # Long enough for a second call to arrive while this one is running.
            time.sleep(0.05)
            with lock:
                answers.append({"type": "tool_result", "call_id": call_id,
                                "ok": True, "text": "ok"})

    def read_message():
        while True:
            with lock:
                if answers:
                    return answers.popleft()
            time.sleep(0.002)

    threading.Thread(target=host, daemon=True).start()
    monkeypatch.setattr(transport, "emit", emit)
    monkeypatch.setattr(transport, "read_message", read_message)
    monkeypatch.setattr(transport, "get_current_rid", lambda: "r")


def test_two_host_calls_in_flight_at_once_both_come_back(monkeypatch):
    _fake_host(monkeypatch)
    done: list[dict] = []

    def call() -> None:
        done.append(tools.call_cpp("tight_crop", {}))

    threads = [threading.Thread(target=call, daemon=True) for _ in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=3)

    assert len(done) == 2
    assert all(result.get("ok") for result in done)
