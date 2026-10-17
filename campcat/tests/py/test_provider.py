"""Which provider the sidecar talks to, and what it sends that provider.

No network: `init_chat_model` is replaced, so the cases check the wiring rather
than any real call. Every case clears the overrides it does not set, because a
developer running with them exported would otherwise change the defaults these
assert.
"""

import pytest

import models

CLEARED = ("CAMPCAT_LLM_PROVIDER", "CAMPCAT_LLM_MODEL",
           "DEEPSEEK_BASE_URL", "QWEN_BASE_URL", "CAMPCAT_LLM_MEMORY_MODEL",
           # Cleared too: a developer with real keys exported must not change
           # what a missing-key case observes.
           "DEEPSEEK_API_KEY", "QWEN_API_KEY")


@pytest.fixture(autouse=True)
def _no_selected_provider():
    """The selected provider is module state, so it outlives the case that set it.

    Cleared before and after: a case that never picks one must not inherit the
    one the case before it picked, in either direction.
    """
    models.set_provider("")
    yield
    models.set_provider("")


def _clear(monkeypatch):
    for name in CLEARED:
        monkeypatch.delenv(name, raising=False)


def _capture(monkeypatch):
    """Record what `_chat_model` would have built, without building it."""
    seen = {}

    def fake(name, **kwargs):
        seen["name"] = name
        seen.update(kwargs)
        return "model"

    monkeypatch.setattr(models, "init_chat_model", fake)
    return seen


def test_deepseek_is_the_default_and_keeps_its_wire_format(monkeypatch):
    _clear(monkeypatch)
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-deepseek")
    seen = _capture(monkeypatch)

    models.build_chat_model()

    assert seen["name"] == "deepseek:deepseek-flash"
    assert seen["base_url"] == "https://api.deepseek.com"
    assert seen["api_key"] == "sk-deepseek"
    assert seen["extra_body"] == {"thinking": {"type": "enabled"}}


def test_the_request_picks_qwen_and_its_thinking_parameter(monkeypatch):
    _clear(monkeypatch)
    models.set_provider("qwen")
    monkeypatch.setenv("QWEN_API_KEY", "sk-qwen")
    seen = _capture(monkeypatch)

    models.build_chat_model()

    assert seen["name"] == "openai:qwen3.8-max"
    assert seen["base_url"] == models.PROVIDERS["qwen"]["base_url"]
    # Its own name, not OPENAI_API_KEY: the key is passed in, so the integration
    # never looks one up.
    assert seen["api_key"] == "sk-qwen"
    # Not DeepSeek's shape: Qwen3.7 thinks by default too, and its own switch is
    # the only one it understands.
    assert seen["extra_body"] == {"enable_thinking": True}


def test_a_base_url_env_var_overrides_the_default(monkeypatch):
    _clear(monkeypatch)
    monkeypatch.setenv("QWEN_API_KEY", "sk-qwen")
    monkeypatch.setenv("QWEN_BASE_URL", "https://example.invalid/v1")
    models.set_provider("qwen")
    seen = _capture(monkeypatch)

    models.build_chat_model()

    assert seen["base_url"] == "https://example.invalid/v1"


def test_a_missing_key_names_the_providers_variable(monkeypatch):
    """Our own sentence, not a library's: hence the explicit `api_key`."""
    _clear(monkeypatch)
    models.set_provider("qwen")
    monkeypatch.delenv("QWEN_API_KEY", raising=False)

    with pytest.raises(RuntimeError, match="QWEN_API_KEY"):
        models.build_chat_model()


def test_the_request_wins_over_the_environment(monkeypatch):
    """The host sends a name every turn, so the file cannot override the UI."""
    _clear(monkeypatch)
    monkeypatch.setenv("CAMPCAT_LLM_PROVIDER", "deepseek")
    monkeypatch.setenv("QWEN_API_KEY", "sk-qwen")
    models.set_provider("qwen")
    seen = _capture(monkeypatch)

    models.build_chat_model()

    assert seen["name"] == "openai:qwen3.8-max"


def test_an_empty_request_falls_back_to_the_environment(monkeypatch):
    """What a bare `python3 main.py` sends: nothing, so `.env` decides."""
    _clear(monkeypatch)
    monkeypatch.setenv("CAMPCAT_LLM_PROVIDER", "qwen")
    monkeypatch.setenv("QWEN_API_KEY", "sk-qwen")
    seen = _capture(monkeypatch)

    models.build_chat_model()

    assert seen["name"] == "openai:qwen3.8-max"


def test_an_unknown_provider_is_refused_rather_than_falling_back(monkeypatch):
    _clear(monkeypatch)
    models.set_provider("gpt")

    with pytest.raises(RuntimeError, match="qwen"):
        models._provider()


def test_an_explicit_model_still_overrides_the_preset(monkeypatch):
    """The preset is a default, not a lock: the older knobs keep working."""
    _clear(monkeypatch)
    models.set_provider("qwen")
    monkeypatch.setenv("QWEN_API_KEY", "sk-qwen")
    monkeypatch.setenv("CAMPCAT_LLM_MODEL", "openai:qwen3.7-max")
    seen = _capture(monkeypatch)

    models.build_chat_model()

    assert seen["name"] == "openai:qwen3.7-max"


def test_key_and_base_url_reach_the_real_client(monkeypatch):
    """Built for real -- no request goes out -- because the question is whether
    the client takes the keywords at all.

    `api_base` is a ChatDeepSeek alias that ChatOpenAI ignores without
    complaining, so naming it there would send Qwen's key to OpenAI's endpoint.
    Only a real construction catches that; a captured kwarg would not. The key
    is checked the same way, on the attribute both clients expose.
    """
    for provider in ("deepseek", "qwen"):
        _clear(monkeypatch)
        preset = models.PROVIDERS[provider]
        monkeypatch.setenv(preset["key_env"], "sk-test")
        models.set_provider(provider)

        model = models.build_chat_model()

        assert model.openai_api_base == preset["base_url"]
        assert model.openai_api_key.get_secret_value() == "sk-test"
