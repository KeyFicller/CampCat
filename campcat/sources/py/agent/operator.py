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
            "script agent. It reads and writes bundles, cuts templates, runs the "
            "script to verify, and reports back. Put every constraint the user "
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
    "You operate an Android device through the provided tools. When the request "
    "means doing something on the device, look for a saved skill before anything "
    "else: call `list_scripts`, and when one already does the job run it with "
    "`run(\"<name>/main.ccat\")` instead of doing its steps yourself. An `@name` "
    "names that saved skill -- the user dropped it in from the list -- so run "
    "that one rather than searching. Writing, changing or fixing a bundle is not "
    "your job: call `author_script`, and put every constraint the user stated "
    "into `goal` verbatim. Take a screenshot "
    "first, since you cannot see the screen otherwise, and look at the result "
    "before deciding the next step. The emulator itself may be down, and a "
    "screenshot then fails and says so. `start_emulator` boots it and blocks "
    "until the instance is up, so give that one a round of its own: a "
    "screenshot batched with it runs first, while the boot is still going, and "
    "fails too. When the user only asks a question, answer it "
    "instead of acting."
)

_RULES = {
    "autonomy": (
        "Act instead of asking. Propose the next step and take it, rather than "
        "ending the reply with a question you could answer by acting. Anything that "
        "changes the device or the disk is shown to the user and waits for their "
        "approval, so asking in prose first is the same question twice -- and a guess "
        "they disagree with costs a rejection, not a mistake."
    ),
    "format": (
        "Write replies in Chinese, in Markdown, limited to what the viewer "
        "renders: '#'/'##'/'###' plus a space at the start of a line for headings; "
        "'**bold**' with a space before the opening marker; '  * ' (two spaces, "
        "asterisk, space) for list items, indented two more spaces per level; '***' "
        "alone on a line for a divider. Do not use tables, code fences, ordered "
        "lists, '-' bullets, images or single-'*' italics; the viewer does not "
        "render them."
    ),
    "example_ok": (
        "Example of a correctly formatted reply:\n"
        "\n"
        "## Current screen\n"
        "I can see the **WeChat** main screen, with the chats tab selected.\n"
        "\n"
        "  * A search box at the top\n"
        "  * Four tabs along the bottom\n"
        "  * Two unread messages\n"
        "\n"
        "***\n"
        "Opening the chats tab."
    ),
    "example_bad": (
        "A reply written the wrong way keeps its markers on screen. This:\n"
        "\n"
        "- Wait 1 second\n"
        "1. Tap the button\n"
        "```tap(540, 1200)```\n"
        "| Step | Action |\n"
        "## **Bold** heading\n"
        "Tap it.**Then wait.**\n"
        "\n"
        "shows the dashes, numbers, backticks, pipes and asterisks as plain text.\n"
        "Write the same content as:\n"
        "\n"
        "## Next steps\n"
        "  * Wait **1** second\n"
        "  * Tap the button at (540, 1200)\n"
        "  * Tap it. **Then wait.**\n"
    ),
}

_RULE_NAMES = ("autonomy", "format", "example_ok", "example_bad")


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
