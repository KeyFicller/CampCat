"""The operator: device actions, and the handoff into the scripter.

The host sends one flat tool list. Filtering happens here, so C++ never learns
that two agents exist.
"""

from __future__ import annotations

from typing import Annotated

from langchain_core.tools import tool
from pydantic import Field

import agent.scripter as scripter
import models

# Not in the C++ registry. `run_tools` intercepts this name and runs the
# scripter; this body is not called. Defaults keep every argument but `goal`
# out of the schema's `required`.
@tool
def author_script(
    goal: Annotated[str, Field(description=(
        "What the script must do, in the user's words plus "
        "anything you learned from the screen."))],
    bundle: Annotated[str, Field(description=(
        "Bundle to create or change; omit to let the script agent pick."))] = "",
    shots: Annotated[list[int], Field(description=(
        "Screenshot numbers that show the screens involved."))] = [],
    last_error: Annotated[str, Field(description=(
        "Verbatim error from the last run_script, when fixing a failure."))] = "",
    fix: Annotated[bool, Field(description=(
        "True only when the user asked to change a bundle that already "
        "exists. Omit it for a new script, a new screen, or a new edge."))] = False,
) -> str:
    """Hand the job of writing, fixing or managing a .ccat bundle to the
    script agent, including adding screens and moves to a game's graph.
    It reads and writes bundles, cuts templates, runs the script to
    verify, and reports back. Put every constraint the user stated into
    `goal` verbatim: the script agent sees nothing else of this
    conversation. Without `fix: true` or `last_error`, it cannot change
    a bundle that already exists.
    """
    raise RuntimeError("author_script is dispatched by run_tools")


AUTHOR_SCRIPT_NAME = author_script.name

# The operator drives the device and hands bundle work off. It does not see
# `read_script` or the language reference; those belong to the scripter.
OPERATOR_PERSONA = (
    "You operate an Android device through the provided tools. For a device "
    "request, call `list_scripts` first; when a saved skill already does the "
    "job, run it with `run(\"<name>/main.ccat\")` instead of doing its steps. "
    "An `@name` is that skill -- run it, do not search. Writing, changing or "
    "fixing a bundle is `author_script`: put every constraint the user stated "
    "into `goal` verbatim. Pass `fix: true` only when the user asked to change "
    "a bundle that already exists, and `last_error` when the last `run_script` "
    "failed. Any other handoff must not change an existing bundle. "
    "A game tagged `[graph <graph>, ...]` has a map of its screens. "
    "Call `read_graph` with that graph name before a `goto`: it lists "
    "every anchor and every edge. To reach one of those anchors, "
    "`run_script` with `goto(\"<graph>:<anchor>\")` using an anchor id "
    "from that list. To reach a screen it does not have, `author_script` "
    "with a goal that says from which screen to which. "
    "Screenshot first (you cannot see the screen "
    "otherwise) and look at the result before the next step. A flow that must "
    "start from a known screen begins with `go_home`: it returns to the "
    "launcher and closes every recent app. A down emulator "
    "makes the screenshot fail and say so. `start_emulator` boots it and "
    "blocks until the instance is up, so call it alone: a screenshot in the "
    "same round runs first, while the boot is still going, and fails too. "
    "When the user only asks a question, answer it."
)

_RULES = {
    "autonomy": (
        "Act instead of asking. Take the next step rather than ending on a "
        "question you could settle by acting. Changes to the device or the "
        "disk already wait for approval, so a prose question first asks twice; "
        "a guess they reject costs a rejection, not a mistake."
    ),
    "format": (
        "Write replies in Chinese. The viewer renders only: '#'/'##'/'###' "
        "plus a space at the start of a line; '**bold**' with a space before "
        "the opening marker; list items as '  * ' (two spaces, asterisk, "
        "space), indented two more spaces per level; '***' alone on a line "
        "as a divider. Tables, code fences, ordered lists, '-' bullets, "
        "images and single-'*' italics show their markers as text.\n"
        "\n"
        "## Current screen\n"
        "I can see the **WeChat** main screen, with the chats tab selected.\n"
        "\n"
        "  * A search box at the top\n"
        "    * Unread on the first row\n"
        "  * Tap it. **Then wait.**\n"
        "\n"
        "***\n"
        "Opening the chats tab."
    ),
}

_RULE_NAMES = ("autonomy", "format")


def _rules(names: tuple[str, ...]) -> str:
    """One `## name` block per rule."""
    return "\n\n".join(f"## {name}\n\n{_RULES[name]}" for name in names)


def _name(tool: dict) -> str | None:
    try:
        name = tool["function"]["name"]
    except (KeyError, TypeError):
        return None
    return name if isinstance(name, str) else None


def operator_tools(host_tools: list, allow_handoff: bool = True) -> list:
    """Host tools the operator may call, plus the handoff tool.

    `allow_handoff` false withholds the handoff, so the operator is the only
    agent this turn: what the host wants while it records training samples.
    """
    hidden = scripter.SCRIPTER_ONLY | scripter.HIDDEN
    kept = [tool for tool in host_tools if _name(tool) not in hidden]
    if not allow_handoff:
        return kept
    return kept + [author_script]


def operator_prompt(allow_handoff: bool = True) -> str:
    """Who the operator is. An env override replaces the whole thing, cheatsheet included."""
    override = models._env("CAMPCAT_LLM_SYSTEM_PROMPT")
    if override:
        return override
    prompt = OPERATOR_PERSONA + "\n\n" + _rules(_RULE_NAMES)
    if not allow_handoff:
        # Said out loud: the persona above names the handoff, and a model that
        # is offered no such tool will otherwise spend a round calling it.
        prompt += ("\n\n## solo\n\n`author_script` is unavailable right now. "
                   "Do the device work yourself with the tools you have.")
    return prompt
