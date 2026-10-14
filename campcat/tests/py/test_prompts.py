"""The two prompts. The operator never sees the script language; the scripter does."""

import agent.operator as operator
import agent.scripter as scripter


def test_operator_prompt_drops_the_script_language():
    text = operator.operator_prompt()
    assert "author_script" in text
    assert "verbatim" in text
    assert "wait_until" not in text
    assert "read_script" not in text


def test_scripter_prompt_keeps_the_cheatsheet():
    text = scripter.scripter_prompt()
    assert "wait_until" in text
    assert "saved" in text


def test_prompt_override_covers_only_the_operator(monkeypatch):
    untouched = scripter.scripter_prompt()
    monkeypatch.setenv("CAMPCAT_LLM_SYSTEM_PROMPT", "OVERRIDE")
    assert operator.operator_prompt() == "OVERRIDE"
    assert scripter.scripter_prompt() == untouched
    assert "OVERRIDE" not in scripter.scripter_prompt()
