"""The two prompts. The operator never sees the script language; the scripter does."""

import agent.operator as operator
import agent.scripter as scripter


def test_operator_prompt_drops_the_script_language():
    text = operator.operator_prompt()
    assert "author_script" in text
    assert "go_home" in text
    assert "verbatim" in text
    assert "goto(" in text
    assert "wait_until" not in text
    assert "read_script" not in text


def test_scripter_prompt_keeps_the_cheatsheet():
    text = scripter.scripter_prompt()
    assert "wait_until" in text
    assert "saved" in text
    # The graph half: the workflow that builds one, and the language it uses.
    assert "save_anchor" in text
    assert "golden.md" in text
    assert "not an anchor id" in text
    assert "do not save a picture of another screen" in text
    assert 'has("other.png")' not in text
    assert "goto(" in text
    assert "update_graph" not in text
    assert "save_golden" not in text
    assert "graph.json" not in text
    # Building the graph comes before scripting on it.
    assert text.index("## graph_workflow") < text.index("## script_workflow")


def test_scripter_prompt_crops_an_element_once():
    text = scripter.scripter_prompt()
    assert "view_crop` once" in text
    assert "tighten the same box" in text


def test_scripter_prompt_uses_an_action_for_same_screen_work():
    text = scripter.scripter_prompt()
    assert "{graph}.{anchor}.{action}" in text
    assert "[action of stzb on kucang]" in text
    assert "same screen" in text
    assert "same chrome" in text
    assert "does not split the anchor" in text
    assert text.index("{graph}.{anchor}.{action}") < text.index("## script_workflow")


def test_prompts_freeze_existing_bundles():
    assert "fix: true" in operator.operator_prompt()
    text = scripter.scripter_prompt()
    assert "fix: true" in text
    assert "last_error" in text
    assert "update_script` before you duplicate" not in text


def test_prompt_override_covers_only_the_operator(monkeypatch):
    untouched = scripter.scripter_prompt()
    monkeypatch.setenv("CAMPCAT_LLM_SYSTEM_PROMPT", "OVERRIDE")
    assert operator.operator_prompt() == "OVERRIDE"
    assert scripter.scripter_prompt() == untouched
    assert "OVERRIDE" not in scripter.scripter_prompt()
