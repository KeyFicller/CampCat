"""Fixtures for the sidecar's tests.

`campcat/sources/py/` goes on sys.path because the sidecar's modules import each
other by bare name (`import models`). A test has to reach the same module object
the code under test will look up, or monkeypatching it does nothing.
"""

import os
import pathlib
import sys

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[2] / "sources" / "py")
)

# `observability` turns tracing on as soon as these two names are in the
# environment, and a contributor who has them set would then run every case with
# a live Langfuse callback attached -- the graph state is serialized for upload,
# which is slow, reaches the network, and makes the result depend on the
# developer's credentials. Popped at import, before any test module loads, so
# every case exercises the no-credentials path the module documents.
os.environ.pop("LANGFUSE_PUBLIC_KEY", None)
os.environ.pop("LANGFUSE_SECRET_KEY", None)

import pytest
from langchain_core.language_models.fake_chat_models import (
    FakeMessagesListChatModel,
)


class ScriptedModel(FakeMessagesListChatModel):
    """A fake model the graph will accept.

    LangChain's fake raises NotImplementedError from `bind_tools`, and
    `graph.invoke` binds as soon as tools are offered -- `invoke` catches that
    and returns it as `{"error": ...}`, so the turn dies with an empty message.
    Returning self keeps the tool path running; the schemas go unvalidated,
    which is the C++ side's job anyway.
    """

    def bind_tools(self, tools, **kwargs):
        return self


@pytest.fixture
def scripted():
    """scripted([AIMessage, ...]) -> a model that hands them back in order.

    Note the list wraps around once spent (LangChain's own behaviour), so a case
    that expects a turn to end has to include the final plain answer, and has to
    build the model once rather than per round -- `graph.invoke` calls
    `build_chat_model` every round, and a fresh model per round restarts the list.
    """

    def _build(responses):
        return ScriptedModel(responses=responses)

    return _build
