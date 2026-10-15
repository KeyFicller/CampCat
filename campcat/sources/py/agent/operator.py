"""The operator: device actions, and the handoff into the scripter.

The host sends one flat tool list. Filtering happens here, so C++ never learns
that two agents exist.
"""

from __future__ import annotations

import agent.scripter as scripter
import models

AUTHOR_SCRIPT_NAME = "author_script"

# Not in the C++ registry. `run_tools` intercepts this name and runs the
# scripter instead of forwarding it to the host.
AUTHOR_SCRIPT = {
    "type": "function",
    "function": {
        "name": "author_script",
        "description": (
            "Hand the job of writing, fixing or managing a .ccat bundle to the "
            "script agent, including adding screens and moves to a game's graph. "
            "It reads and writes bundles, cuts templates, runs the script to "
            "verify, and reports back. Put every constraint the user "
            "stated into `goal` verbatim: the script agent sees nothing else of "
            "this conversation."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "goal": {
                    "type": "string",
                    "description": (
                        "What the script must do, in the user's words plus "
                        "anything you learned from the screen."
                    ),
                },
                "bundle": {
                    "type": "string",
                    "description": (
                        "Bundle to create or change; omit to let the script "
                        "agent pick."
                    ),
                },
                "shots": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": (
                        "Screenshot numbers that show the screens involved."
                    ),
                },
                "last_error": {
                    "type": "string",
                    "description": (
                        "Verbatim error from the last run_script, when fixing "
                        "a failure."
                    ),
                },
            },
            "required": ["goal"],
        },
    },
}

# The operator drives the device and hands bundle work off. It does not see
# `read_script` or the language reference; those belong to the scripter.
OPERATOR_PERSONA = (
    "You operate an Android device through the provided tools. For a device "
    "request, call `list_scripts` first; when a saved skill already does the "
    "job, run it with `run(\"<name>/main.ccat\")` instead of doing its steps. "
    "An `@name` is that skill -- run it, do not search. Writing, changing or "
    "fixing a bundle is `author_script`: put every constraint the user stated "
    "into `goal` verbatim. A game tagged `[graph <graph>, ...]` has a map of "
    "its screens: to reach one of its anchors, `run_script` with "
    "`goto(\"<graph>:<anchor>\")`; to reach a screen it does not have, "
    "`author_script` with a goal that says from which screen to which. "
    "Screenshot first (you cannot see the screen "
    "otherwise) and look at the result before the next step. A down emulator "
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


def operator_tools(host_tools: list) -> list:
    """Host tools minus the scripter-only ones, plus the handoff tool."""
    kept = [tool for tool in host_tools if _name(tool) not in scripter.SCRIPTER_ONLY]
    return kept + [AUTHOR_SCRIPT]


def operator_prompt() -> str:
    """Who the operator is. An env override replaces the whole thing, cheatsheet included."""
    override = models._env("CAMPCAT_LLM_SYSTEM_PROMPT")
    if override:
        return override
    return OPERATOR_PERSONA + "\n\n" + _rules(_RULE_NAMES)
