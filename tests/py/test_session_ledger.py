"""Skeleton for the ledger half of the sidecar gate.

`session` holds the one conversation the sidecar serves, so every case resets it
first. More cases to come: `screenshot #N`, `save_template` with and without an
explicit `shot`, the host-summary branch, and the 80-character cut.
"""

import session


def test_ledger_line_names_the_frame(monkeypatch):
    session.reset()
    # Sentinel: monkeypatch.setattr happily creates a name that does not exist,
    # so without this a rename upstream would turn into a quiet wrong assertion.
    assert hasattr(session, "_LATEST_NUM")
    monkeypatch.setattr(session, "_LATEST_NUM", 6)

    assert session.ledger_line(
        "tap", {"x": 2407, "y": 142, "w": 77, "h": 99}, {}
    ) == "tap x=2407 y=142 w=77 h=99 on #6"


def test_append_turn_folds_the_ledger_into_the_stored_reply():
    session.reset()
    session.append_turn("tap it", "answer", ["screenshot #6"])

    stored = session.history()[-1].content
    # Order is what this case guards, so it asserts leading and trailing rather
    # than membership: a plain `in` would pass with the ledger either side.
    assert stored.startswith("### Actions In Current Turn")
    assert stored.endswith("answer")
    assert "- screenshot #6" in stored
