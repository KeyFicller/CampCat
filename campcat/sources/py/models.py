"""Model defaults, `.env` loading, and LangChain chat-model construction.

All model configuration lives here. Which provider to build arrives with each
request -- the C++ host sends the name its settings selected -- and `.env`
holds the keys.
"""

from __future__ import annotations

import os
from pathlib import Path

from langchain.chat_models import init_chat_model
from langchain_core.callbacks import BaseCallbackHandler
from langchain_openai import ChatOpenAI

import session
import transport


# Room for the answer plus the reasoning that precedes it: DeepSeek bills its
# chain of thought against `max_tokens`, so a tight cap spends the whole budget
# thinking and returns no content at all. Qwen's plan endpoint caps only the
# visible answer, so this is slack there and a floor here.
DEFAULT_MAX_TOKENS = 8192 * 16
# The memory rewrite is a longer output than a reply: it replaces up to
# `memorier.DEFAULT_MEMORY_MAX_CHARS` characters in one go, and it thinks
# first like every other call.
DEFAULT_MEMORY_MAX_TOKENS = 8192 * 16
# Every knob, including which variable holds the key and which holds the base
# URL. Both are passed to the client explicitly, so neither name has to be one an
# integration already knows: `QWEN_API_KEY` is ours, not ChatOpenAI's. A blank
# `*_BASE_URL` falls back to the default in the same entry.
#
# `extra_body` is the provider's own thinking switch: Qwen3.7 and DeepSeek V4
# both think by default, and sending one's shape to the other is not a no-op.
# Both are asked to think: the regions and scripts here are decided from a
# screenshot, where a fast guess is the common failure.
PROVIDERS = {
    "deepseek": {
        "key_env": "DEEPSEEK_API_KEY",
        "base_url_env": "DEEPSEEK_BASE_URL",
        "base_url": "https://api.deepseek.com",
        "model": "deepseek:deepseek-flash",
        "extra_body": {"thinking": {"type": "enabled"}},
        "thinking_off": {"thinking": {"type": "disabled"}},
    },
    "qwen": {
        # The `openai:` prefix is what tells init_chat_model which client to
        # build. This base URL is the Token Plan endpoint, not the pay-as-you-go
        # `dashscope.aliyuncs.com/compatible-mode/v1`: a plan key (`sk-sp-`) is
        # refused there with a 401, and the two are not interchangeable.
        "key_env": "QWEN_API_KEY",
        "base_url_env": "QWEN_BASE_URL",
        "base_url": "https://token-plan.maas.qianwenaiapi.com/compatible-mode/v1",
        "model": "openai:qwen3.8-max",
        "extra_body": {"enable_thinking": True},
        "thinking_off": {"enable_thinking": False},
    },
}
DEFAULT_PROVIDER = "deepseek"
# Measured: a 1080x2400 emulator screenshot costs ~985 input tokens. The
# summarization counter never sees the image bytes, so it needs this constant.
TOKENS_PER_IMAGE = 950


def load_env_file(path: Path) -> int:
    """Load `KEY=VALUE` lines into os.environ, without clobbering real env vars.

    The shell wins over the file, so `CAMPCAT_LLM_MODEL=... python3 main.py`
    behaves as expected. Returns the number of variables applied.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return 0

    applied = 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip().strip("'\"")
        if not key or key in os.environ:
            continue
        os.environ[key] = value
        applied += 1
    return applied


def _env(name: str) -> str:
    return os.environ.get(name, "").strip()


def _setting(name: str, default: str) -> str:
    return _env(name) or default


# Which provider the current request wants, set by main.py from the C++ host's
# settings. Empty means the request did not say -- a bare `python3 main.py` run
# -- and the environment, then the default, decides.
_SELECTED = ""


def set_provider(name: str) -> None:
    """Remember the provider this request asks for; empty clears the choice."""
    global _SELECTED
    _SELECTED = name.strip()


def _provider() -> dict:
    """The selected provider's knobs. Raises on a name that is not one of them.

    Loud rather than silently falling back: a mistyped name would otherwise bill
    DeepSeek while the user believes they are on Qwen.
    """
    name = _SELECTED or _setting("CAMPCAT_LLM_PROVIDER", DEFAULT_PROVIDER)
    if name not in PROVIDERS:
        raise RuntimeError(
            f"provider {name!r} is not one of {', '.join(sorted(PROVIDERS))}"
        )
    return PROVIDERS[name]





class _UsageHandler(BaseCallbackHandler):
    """Counts one completion when the model call returns.

    Hung on the model itself. The summarizer calls `with_retry().invoke`,
    which still runs this model's callbacks; a proxy around `invoke` would
    not see that call. This LangChain ends that run with `on_llm_end` and an
    `LLMResult`; it never calls `on_chat_model_end`.
    """

    def __init__(self, *, context: bool) -> None:
        super().__init__()
        self._context = context

    def on_llm_end(self, response, **kwargs) -> None:
        msg = None
        gens = getattr(response, "generations", None) or []
        if gens and gens[0]:
            msg = getattr(gens[0][0], "message", None)
        session.note_llm(msg, context=self._context)


class _ReasoningChatOpenAI(ChatOpenAI):
    """Keeps the provider's reasoning deltas, which `ChatOpenAI` drops.

    `reasoning_content` is not part of the OpenAI schema, so the base class
    strips it on purpose. Qwen sends it, and during a call that thinks for a
    minute it is the only sign of progress.
    """

    def _convert_chunk_to_generation_chunk(self, chunk, default_chunk_class,
                                           base_generation_info):
        out = super()._convert_chunk_to_generation_chunk(
            chunk, default_chunk_class, base_generation_info)
        choices = chunk.get("choices") or []
        if out is not None and choices:
            text = (choices[0].get("delta") or {}).get("reasoning_content")
            if text:
                out.message.additional_kwargs["reasoning_content"] = text
        return out


def _chat_model(name: str, max_tokens: int, *, context: bool = False):
    """Build a chat model for the selected provider.

    Key and base URL are passed explicitly, so neither has to be a name the
    integration already reads: `QWEN_API_KEY` is ours. A missing key is refused
    here, in a sentence that names the variable.

    `base_url` is the one spelling both clients accept. `api_base` is a
    ChatDeepSeek-only alias that ChatOpenAI drops without complaining, which
    would send Qwen's key to OpenAI's own endpoint instead.
    """
    provider = _provider()
    api_key = _env(provider["key_env"])
    if not api_key:
        raise RuntimeError(f"{provider['key_env']} is not set (put it in .env)")
    # `CAMPCAT_LLM_THINKING=off` asks for a faster, dumber run: a smoke test
    # cares about the tool calls, and nobody is reading the reasoning.
    quiet = _env("CAMPCAT_LLM_THINKING").lower() in {"off", "0", "false"}
    base_url = _setting(provider["base_url_env"], provider["base_url"])
    extra_body = provider["thinking_off"] if quiet else provider["extra_body"]
    prefix, _, bare = name.partition(":")
    if prefix == "openai":
        # Built here rather than through `init_chat_model` so the reasoning
        # deltas survive; the prefix only told `init_chat_model` which client.
        model = _ReasoningChatOpenAI(model=bare, api_key=api_key,
                                     base_url=base_url, max_tokens=max_tokens,
                                     extra_body=extra_body)
    else:
        model = init_chat_model(
            name,
            api_key=api_key,
            base_url=base_url,
            max_tokens=max_tokens,
            extra_body=extra_body,
        )
    # The provider tests stand in a non-model for init_chat_model. A real chat
    # model has this attribute; without it there is nothing to count.
    if hasattr(model, "callbacks"):
        model.callbacks = [_UsageHandler(context=context)]
    return model


def build_chat_model(*, context: bool = False):
    """Build the chat model named by the environment.

    `context` is true only for a conversation request: that response's
    `input_tokens` is the status bar's context length. Other callers leave it
    false so their own prompts do not overwrite it.

    Raises RuntimeError when the API key is missing; the caller reports it as a
    failed result.
    """
    return _chat_model(
        _setting("CAMPCAT_LLM_MODEL", _provider()["model"]),
        int(_env("CAMPCAT_LLM_MAX_TOKENS") or DEFAULT_MAX_TOKENS),
        context=context,
    )


def streamed(model, messages):
    """One model call, abandoned as soon as the host stops the turn.

    `invoke` blocks until the provider has finished thinking, and with a
    reasoning model that is tens of seconds during which a stop could only be
    noticed afterwards. Streaming gives the loop a point to look between
    chunks, so a stop costs one chunk instead of one reply.

    A model that cannot stream still works: LangChain's own `stream` falls back
    to a single `invoke` and yields one chunk, so there is simply nothing to
    stop early in that case.
    """
    if transport.stop_requested():
        raise transport.TurnInterrupted()
    if not hasattr(model, "stream"):
        # Only a stand-in handed in by a test has no `stream` at all; a real
        # LangChain model always has one and falls back to a single `invoke`
        # inside it.
        return model.invoke(messages)
    chunks = []
    for chunk in model.stream(messages):
        if transport.stop_requested():
            raise transport.TurnInterrupted()
        chunks.append(chunk)
    if not chunks:
        raise RuntimeError("model stream ended without a reply")
    reply = chunks[0]
    for chunk in chunks[1:]:
        reply = reply + chunk
    # Reasoning belongs to this call's display, not to the conversation: some
    # providers reject it on the way back, and the next turn does not need it.
    reply.additional_kwargs.pop("reasoning_content", None)
    return reply


def build_memory_model():
    """Build the model that rewrites the memory notes.

    Deliberately separate from the model that drives the device: consolidating
    runs off the critical path, while nobody is waiting, so it can afford to be
    the stronger (and slower, and dearer) of the two.
    """
    return _chat_model(
        _setting("CAMPCAT_LLM_MEMORY_MODEL",
                 _setting("CAMPCAT_LLM_MODEL", _provider()["model"])),
        int(_env("CAMPCAT_LLM_MEMORY_MAX_TOKENS") or DEFAULT_MEMORY_MAX_TOKENS),
    )
