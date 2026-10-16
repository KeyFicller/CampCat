"""Tool tables for the operator and the scripter.

The host sends every tool. These tests pin which names each agent is offered
and how an `author_script` brief is spelled.
"""

import agent.operator as operator
import agent.scripter as scripter

HOST_NAMES = (
    "screenshot view_shot view_template view_crop mark tap start_emulator "
    "run_script save_script save_template ccat_help list_scripts read_script "
    "update_script read_graph save_anchor delete_script rename_script"
).split()

# Host order, not the grouped order in the spec table: `run_script` is
# registered before `list_scripts`. `mark` is registered and offered to neither.
OPERATOR_NAMES = (
    "screenshot view_shot tap start_emulator run_script list_scripts "
    "author_script"
).split()


def _tool(name):
    return {"type": "function", "function": {"name": name, "parameters": {}}}


def _names(tools):
    names = []
    for tool in tools:
        if isinstance(tool, dict) and "function" in tool:
            names.append(tool["function"]["name"])
        elif getattr(tool, "name", None):
            names.append(tool.name)
    return names


def test_operator_and_scripter_split_the_host_table():
    host = [_tool(name) for name in HOST_NAMES]
    operator_table = operator.operator_tools(host)
    scripter_table = scripter.scripter_tools(host)

    assert _names(operator_table) == OPERATOR_NAMES
    assert "view_template" not in _names(operator_table)
    assert len(scripter_table) == 16
    assert _names(scripter_table) == [
        name for name in HOST_NAMES
        if name in scripter.SCRIPTER_ONLY | scripter.SHARED
    ] + ["diagnose_template"]
    assert set(_names(scripter_table)) == (
        scripter.SCRIPTER_ONLY | scripter.SHARED | {"diagnose_template"})
    assert "diagnose_template" not in _names(operator_table)
    covered = (set(_names(operator_table)) | set(_names(scripter_table))) - {
        "author_script", "diagnose_template"}
    assert covered == set(HOST_NAMES) - scripter.HIDDEN
    assert "mark" not in _names(operator_table)
    assert "mark" not in _names(scripter_table)


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


def test_author_script_schema_comes_from_the_tool():
    from langchain_core.utils.function_calling import convert_to_openai_tool

    fn = convert_to_openai_tool(operator.author_script)["function"]
    params = fn["parameters"]
    assert fn["name"] == "author_script"
    assert "fix: true" in fn["description"]
    assert params["required"] == ["goal"]
    assert list(params["properties"]) == [
        "goal", "bundle", "shots", "last_error", "fix"]
    assert params["properties"]["shots"]["items"]["type"] == "integer"
    assert params["properties"]["fix"]["type"] == "boolean"
    assert "omit" in params["properties"]["bundle"]["description"]


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
    assert scripter.brief_of({"goal": "签到", "fix": True}) == "goal: 签到\nfix: true"
    assert scripter.brief_of({"goal": "签到", "fix": "true"}) == "goal: 签到"
    assert scripter.brief_of({"goal": "签到", "fix": False}) == "goal: 签到"
