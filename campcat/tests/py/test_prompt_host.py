"""System prompts may come from Langfuse. The in-repo text is the fallback.

A missing client is the normal offline path and stays quiet. A client that
errors, or a prompt that is missing or unusable, warns once and returns the
same text the process would have sent before this existed.
"""

import agent.diagnoser as diagnoser
import agent.memorier as memorier
import agent.operator as operator
import agent.reflector as reflector
import agent.region_review as region_review
import agent.scripter as scripter
import agent.solo as solo
import consult
import models
import observability
import prompt_host
import session
from langchain_core.messages import AIMessage, SystemMessage

# Captured before the autouse fixture replaces `reflect` with a no-op.
_real_reflect = reflector.reflect

_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP8z4AdMOEQH6QSAM1B"
    "AQ/oQeJvAAAAAElFTkSuQmCC"
)


class _Client:
    """A stand-in for the Langfuse client. Records each get_prompt."""

    def __init__(self, prompts, error=None):
        self.prompts = prompts
        self.error = error
        self.calls = []

    def get_prompt(self, name, **kwargs):
        self.calls.append((name, kwargs))
        if self.error is not None:
            raise self.error
        if name not in self.prompts:
            raise KeyError(name)
        return _Prompt(self.prompts[name])


class _Prompt:
    def __init__(self, body):
        self.prompt = body


def _install(monkeypatch, prompts, error=None):
    client = _Client(prompts, error)
    monkeypatch.setattr(prompt_host, "_langfuse", lambda: client)
    prompt_host.reset()
    return client


def _quiet(monkeypatch):
    monkeypatch.setattr(prompt_host, "_langfuse", lambda: None)
    prompt_host.reset()


def test_a_hosted_prompt_replaces_the_fallback(monkeypatch):
    client = _install(monkeypatch, {"campcat/operator": "HOSTED"})

    assert prompt_host.text("campcat/operator", "LOCAL") == "HOSTED"
    assert client.calls[0][0] == "campcat/operator"
    assert client.calls[0][1]["label"] == "production"
    assert client.calls[0][1]["type"] == "text"
    assert client.calls[0][1]["max_retries"] == 0
    assert client.calls[0][1]["fetch_timeout_seconds"] > 0


def test_a_fetch_error_falls_back(monkeypatch, capsys):
    _install(monkeypatch, {}, error=RuntimeError("down"))

    assert prompt_host.text("campcat/operator", "LOCAL") == "LOCAL"
    assert prompt_host.text("campcat/operator", "LOCAL") == "LOCAL"

    err = capsys.readouterr().err
    assert err.count("campcat/operator") == 1
    assert "down" in err
    assert "using in-repo text" in err


def test_an_empty_prompt_falls_back(monkeypatch, capsys):
    _install(monkeypatch, {"campcat/operator": "  \n"})

    assert prompt_host.text("campcat/operator", "LOCAL") == "LOCAL"

    err = capsys.readouterr().err
    assert "campcat/operator" in err
    assert "using in-repo text" in err


def test_a_chat_prompt_falls_back(monkeypatch, capsys):
    # Prompt Management can store a chat prompt under the same name. The
    # sidecar only has a slot for one system string, so a list is a miss.
    _install(monkeypatch, {"campcat/operator": [{"role": "system", "content": "x"}]})

    assert prompt_host.text("campcat/operator", "LOCAL") == "LOCAL"
    assert "using in-repo text" in capsys.readouterr().err


def test_no_client_stays_quiet(monkeypatch, capsys):
    _quiet(monkeypatch)

    assert prompt_host.text("campcat/operator", "LOCAL") == "LOCAL"
    assert capsys.readouterr().err == ""


def test_mustache_and_single_braces_render(monkeypatch):
    _install(monkeypatch, {
        "mustache": "cap {{entry_chars}} / {{ max_ops }}",
        "braces": "cap {entry_chars} / {max_ops}",
    })

    assert prompt_host.text("mustache", "nope", entry_chars=200, max_ops=40) == "cap 200 / 40"
    assert prompt_host.text("braces", "nope", entry_chars=200, max_ops=40) == "cap 200 / 40"


def test_json_braces_in_the_template_stay(monkeypatch):
    _quiet(monkeypatch)
    fallback = 'limit {entry_chars} {"ops":[]}'

    assert prompt_host.text("campcat/memory-operator", fallback, entry_chars=200) == (
        'limit 200 {"ops":[]}'
    )


def test_a_prompt_is_fetched_once(monkeypatch):
    client = _install(monkeypatch, {"campcat/operator": "HOSTED {{extra}}"})

    assert prompt_host.text("campcat/operator", "LOCAL", extra="one") == "HOSTED one"
    assert prompt_host.text("campcat/operator", "LOCAL", extra="two") == "HOSTED two"
    assert len(client.calls) == 1


def test_a_new_session_fetches_again(monkeypatch):
    client = _install(monkeypatch, {"campcat/operator": "HOSTED"})

    prompt_host.text("campcat/operator", "LOCAL")
    observability.new_session()
    prompt_host.text("campcat/operator", "LOCAL")

    assert len(client.calls) == 2


def test_the_label_comes_from_the_env(monkeypatch):
    client = _install(monkeypatch, {"campcat/operator": "STAGED"})
    monkeypatch.setenv("CAMPCAT_LANGFUSE_PROMPT_LABEL", "staging")

    assert prompt_host.text("campcat/operator", "LOCAL") == "STAGED"
    assert client.calls[0][1]["label"] == "staging"


def test_a_blank_label_means_production(monkeypatch):
    client = _install(monkeypatch, {"campcat/operator": "HOSTED"})
    monkeypatch.setenv("CAMPCAT_LANGFUSE_PROMPT_LABEL", "  ")

    prompt_host.text("campcat/operator", "LOCAL")

    assert client.calls[0][1]["label"] == "production"


def test_operator_prompt_loads_its_langfuse_name(monkeypatch):
    monkeypatch.delenv("CAMPCAT_LLM_SYSTEM_PROMPT", raising=False)
    client = _install(monkeypatch, {prompt_host.OPERATOR: "HOSTED-OPERATOR"})

    assert operator.operator_prompt() == "HOSTED-OPERATOR"
    assert client.calls[0][0] == prompt_host.OPERATOR


def test_the_env_override_still_skips_langfuse(monkeypatch):
    client = _install(monkeypatch, {prompt_host.OPERATOR: "HOSTED"})
    monkeypatch.setenv("CAMPCAT_LLM_SYSTEM_PROMPT", "OVERRIDE")

    assert operator.operator_prompt() == "OVERRIDE"
    assert solo.prompt() == "OVERRIDE"
    assert client.calls == []


def test_solo_and_scripter_load_their_own_names(monkeypatch):
    monkeypatch.delenv("CAMPCAT_LLM_SYSTEM_PROMPT", raising=False)
    client = _install(monkeypatch, {
        prompt_host.SOLO: "HOSTED-SOLO",
        prompt_host.SCRIPTER: "HOSTED-SCRIPTER",
    })

    assert solo.prompt() == "HOSTED-SOLO"
    assert scripter.scripter_prompt() == "HOSTED-SCRIPTER"
    assert [name for name, _ in client.calls] == [
        prompt_host.SOLO, prompt_host.SCRIPTER]


def test_region_review_fills_extra_from_one_fetch(monkeypatch):
    client = _install(monkeypatch, {prompt_host.REGION_REVIEW: "HEAD\n{{extra}}"})

    template = region_review._review_system("template")
    tap = region_review._review_system("tap")

    assert template.startswith("HEAD\n")
    assert "模板的 adjust" in template
    assert "只填你收到的这个区域" in tap
    assert "模板的 adjust" not in tap
    assert len(client.calls) == 1
    assert client.calls[0][0] == prompt_host.REGION_REVIEW


def test_region_pick_uses_the_prompt_for_its_kind(monkeypatch):
    seen = []

    def streamed(model, messages, sink):
        seen.append(messages[0].content)
        return AIMessage(content='{"pick":1,"reason":"ok"}')

    monkeypatch.setattr(region_review, "_scores", lambda *a, **k: None)
    monkeypatch.setattr(
        region_review, "_views",
        lambda *a, **k: ("cut", "context", None, None, ""))
    monkeypatch.setattr(models, "build_chat_model", lambda **_: object())
    monkeypatch.setattr(models, "bind_schema", lambda model, schema: model)
    monkeypatch.setattr(models, "streamed", streamed)
    _install(monkeypatch, {
        prompt_host.REGION_PICK: "PICK-HOSTED",
        prompt_host.REGION_TEMPLATE_PICK: "TEMPLATE-PICK-HOSTED",
    })
    region = {"x": 1, "y": 2, "w": 3, "h": 4}
    state = {"candidates": [{"region": region}], "kind": "tap", "shot": 1}

    region_review.review_pick(state)
    state["kind"] = "template"
    region_review.review_pick(state)

    assert seen == ["PICK-HOSTED", "TEMPLATE-PICK-HOSTED"]


def test_memory_instructions_fill_the_same_placeholders(monkeypatch):
    session.reset()
    session.append_turn("hi", [AIMessage(content="ok")])
    seen = []

    class _Model:
        def invoke(self, messages, config=None):
            seen.append(messages[0].content)
            return AIMessage(content='{"ops":[]}')

    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    _quiet(monkeypatch)

    memorier.memorize("old", "operator")

    text = seen[0]
    assert "{entry_chars}" not in text
    assert "{max_ops}" not in text
    assert str(memorier.ENTRY_MAX_CHARS) in text
    assert str(memorier.MAX_OPS_PER_PASS) in text
    assert '"ops"' in text
    assert "偏好" in text


def test_memory_instructions_fill_a_hosted_mustache(monkeypatch):
    session.reset()
    session.append_turn("hi", [AIMessage(content="ok")])
    seen = []

    class _Model:
        def invoke(self, messages, config=None):
            seen.append(messages[0].content)
            return AIMessage(content='{"ops":[]}')

    monkeypatch.setattr(models, "build_memory_model", lambda: _Model())
    _install(monkeypatch, {
        prompt_host.MEMORY_OPERATOR: "cap {{entry_chars}} / {{max_ops}}",
    })

    memorier.memorize("old", "operator")

    assert seen == [
        f"cap {memorier.ENTRY_MAX_CHARS} / {memorier.MAX_OPS_PER_PASS}"]


def test_memory_frames_can_be_hosted(monkeypatch):
    _install(monkeypatch, {
        prompt_host.MEMORY_FRAME: "FRAME\n\n",
        prompt_host.SCRIPTER_MEMORY_FRAME: "SCRIPTER-FRAME\n\n",
    })

    assert memorier.memory_block("note") == "\n\nFRAME\n\nnote\n"
    assert memorier.scripter_memory_block("note") == "\n\nSCRIPTER-FRAME\n\nnote\n"
    assert memorier.memory_block("  ") == ""


def test_reflector_fills_max_chars(monkeypatch, scripted):
    seen = []
    inner = scripted([AIMessage(content="note")])

    class _Wrap:
        def bind_tools(self, tools, **kwargs):
            return self

        def invoke(self, messages, config=None):
            seen.append(messages[0].content)
            return inner.invoke(messages, config=config)

    monkeypatch.setattr(models, "build_chat_model", lambda **_: _Wrap())
    _install(monkeypatch, {prompt_host.REFLECTOR: "limit {{max_chars}}"})

    _real_reflect([{"name": "view_shot", "args": {}, "failed": True}], 0, "done")

    assert seen == [f"limit {reflector.MAX_CHARS}"]


def test_reflector_fallback_still_states_the_cap(monkeypatch, scripted):
    seen = []
    inner = scripted([AIMessage(content="note")])

    class _Wrap:
        def bind_tools(self, tools, **kwargs):
            return self

        def invoke(self, messages, config=None):
            seen.append(messages[0].content)
            return inner.invoke(messages, config=config)

    monkeypatch.setattr(models, "build_chat_model", lambda **_: _Wrap())
    _quiet(monkeypatch)

    _real_reflect([{"name": "view_shot", "args": {}, "failed": True}], 0, "done")

    assert "{max_chars}" not in seen[0]
    assert str(reflector.MAX_CHARS) in seen[0]
    assert "notes" in seen[0]


def test_consult_uses_the_hosted_prompt(monkeypatch):
    seen = []

    class _Model:
        def invoke(self, messages, *args, **kwargs):
            seen.append(messages)
            return AIMessage(content="yes")

    monkeypatch.setattr(models, "build_chat_model", lambda **_: _Model())
    _install(monkeypatch, {prompt_host.CONSULT: "HOSTED-CONSULT"})

    consult.answer({
        "id": 7, "question": "Is there a claim button?", "image_b64": _PNG})

    assert isinstance(seen[0][0], SystemMessage)
    assert seen[0][0].content == "HOSTED-CONSULT"


def test_diagnose_uses_the_hosted_prompt(monkeypatch):
    seen = []

    def streamed(model, messages, sink):
        seen.append(messages[0].content)
        return AIMessage(content=(
            '{"verdict":"inconclusive","reason":"x","fix":""}'))

    monkeypatch.setattr(models, "build_chat_model", lambda **_: object())
    monkeypatch.setattr(models, "bind_schema", lambda model, schema: model)
    monkeypatch.setattr(models, "streamed", streamed)
    _install(monkeypatch, {prompt_host.DIAGNOSE: "HOSTED-DIAGNOSE"})

    diagnoser.diagnose_judge({
        "match": {"best": 0.2, "threshold": 0.8, "box": {}},
        "expect": "button",
        "screen_b64": "",
        "crop_b64": "",
        "template_b64": "",
    })

    assert seen == ["HOSTED-DIAGNOSE"]
