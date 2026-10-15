"""Tool tables for the operator and the scripter.

The host sends every tool. These tests pin which names each agent is offered
and how an `author_script` brief is spelled.
"""

import agent.operator as operator
import agent.scripter as scripter

HOST_NAMES = (
    "screenshot view_shot view_template view_crop_box view_crop_circle mark "
    "mark_circle tap tap_circle start_emulator run_script save_script "
    "save_template_box save_template_circle ccat_help list_scripts read_script "
    "update_script read_graph save_anchor delete_script "
    "rename_script"
).split()

# Host order, not the grouped order in the spec table: `run_script` is
# registered before `list_scripts`.
OPERATOR_NAMES = (
    "screenshot view_shot view_crop_box view_crop_circle mark mark_circle tap "
    "tap_circle start_emulator run_script list_scripts author_script"
).split()


def _tool(name):
    return {"type": "function", "function": {"name": name, "parameters": {}}}


def _names(tools):
    return [t["function"]["name"] for t in tools if "function" in t]


def test_operator_and_scripter_split_the_host_table():
    host = [_tool(name) for name in HOST_NAMES]
    operator_table = operator.operator_tools(host)
    scripter_table = scripter.scripter_tools(host)

    assert _names(operator_table) == OPERATOR_NAMES
    assert "view_template" not in _names(operator_table)
    assert len(scripter_table) == 18
    assert _names(scripter_table) == [
        name for name in HOST_NAMES
        if name in scripter.SCRIPTER_ONLY | scripter.SHARED
    ] + ["diagnose_template"]
    assert set(_names(scripter_table)) == (
        scripter.SCRIPTER_ONLY | scripter.SHARED | {"diagnose_template"})
    assert "diagnose_template" not in _names(operator_table)
    covered = (set(_names(operator_table)) | set(_names(scripter_table))) - {
        "author_script", "diagnose_template"}
    assert covered == set(HOST_NAMES)


def test_unknown_tool_stays_with_the_operator():
    host = [_tool("future_tool")]
    assert _names(operator.operator_tools(host)) == ["future_tool", "author_script"]
    assert _names(scripter.scripter_tools(host)) == ["diagnose_template"]


def test_nameless_tool_stays_with_the_operator():
    nameless = {"type": "function"}
    operator_table = operator.operator_tools([nameless])
    assert operator_table[0] is nameless
    assert _names(operator_table) == ["author_script"]
    assert _names(scripter.scripter_tools([nameless])) == ["diagnose_template"]


def test_brief_keeps_only_the_fields_that_were_given():
    full = scripter.brief_of({
        "goal": "签到",
        "bundle": "daily",
        "shots": [3, 4],
        "last_error": "best=0.61",
    })
    assert full == "goal: 签到\nbundle: daily\nshots: 3 4\nlast_error: best=0.61"
    assert scripter.brief_of({"goal": "签到"}) == "goal: 签到"
    assert scripter.brief_of({
        "goal": "签到", "bundle": "", "shots": [], "last_error": "",
    }) == "goal: 签到"
    assert scripter.brief_of({"goal": "签到", "shots": "3"}) == "goal: 签到"
