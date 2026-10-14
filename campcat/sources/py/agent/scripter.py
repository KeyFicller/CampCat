"""The scripter: bundle writing, and the handoff subgraph.

Imported by `graph.py`. This module does not import `graph` at load time;
the subgraph nodes do, once `graph` has finished loading.
"""

from __future__ import annotations

from pathlib import Path

from langchain_core.messages import HumanMessage, SystemMessage, ToolMessage
from langgraph.config import get_config
from langgraph.graph import END, StateGraph

import agent.memorier as memorier
import session


# Only the scripter sees these. Anything the host sends that is not listed here
# goes to the operator, so a new C++ tool lands with today's behaviour.
SCRIPTER_ONLY = frozenset({
    "read_script", "ccat_help",
    "save_script", "update_script", "delete_script", "rename_script",
    "save_template_box", "save_template_circle", "view_template",
})
# Offered to both: the operator runs saved skills and looks at the screen; the
# scripter verifies what it wrote and cuts templates from fresh frames.
SHARED = frozenset({
    "list_scripts", "run_script", "screenshot", "view_shot",
    "view_crop_box", "view_crop_circle",
})


def _name(tool: dict) -> str | None:
    try:
        name = tool["function"]["name"]
    except (KeyError, TypeError):
        return None
    return name if isinstance(name, str) else None


def scripter_tools(host_tools: list) -> list:
    """Host tools the scripter is allowed to call, in the host's order."""
    offered = SCRIPTER_ONLY | SHARED
    return [tool for tool in host_tools if _name(tool) in offered]


def brief_of(args: dict) -> str:
    """The scripter's one user message. Empty optional fields are left out."""
    lines = [f"goal: {args.get('goal', '')}"]
    bundle = args.get("bundle") or ""
    if bundle:
        lines.append(f"bundle: {bundle}")
    shots = args.get("shots")
    if isinstance(shots, list) and shots:
        lines.append("shots: " + " ".join(str(shot) for shot in shots))
    last_error = args.get("last_error") or ""
    if last_error:
        lines.append(f"last_error: {last_error}")
    return "\n".join(lines)

# How a reply is written, one entry per part of the rule. Fed as Markdown with
# the key as a heading, so a part can be rewritten or dropped on its own.
RULES = {
    "script_workflow": (
        "Writing or changing a script is this sequence, and you do not skip a step:\n"
        "\n"
        "1. `list_scripts` first. If a bundle already does the job, run it with "
        "`run(\"<name>/main.ccat\")`; if one is close, `read_script` it and build on it. "
        "Write a new script only when neither holds.\n"
        "2. `screenshot`, so you can see the screen you are scripting.\n"
        "3. Decide each step's anchor: the image that only appears when that step worked, "
        "taken from a fixed UI control -- a button, a tab, a label -- never from free-form "
        "text, which drifts between runs and makes the check meaningless. Write what that "
        "step expects beside it as a `//` comment (`wait_until(\"claim_taken.png\", 8000)"
        "   // the button turns grey once the claim lands`): a failing run then has an "
        "expectation to compare the screen against, and the approval reads.\n"
        "4. `save_script` to create the bundle. It writes main.ccat and runs nothing; a name "
        "that already exists is refused.\n"
        "5. `save_template_box`, once for every PNG the script names. Screenshot right before "
        "each cut -- it can only match as well as the picture it came from -- and crop tight "
        "around the part that stays put. `save_template_box(bundle, name, shot, box)` takes the "
        "box; for a round element use `save_template_circle(bundle, name, shot, circle)` "
        "instead, which writes the disc's alpha and leaves every pixel outside it out of "
        "the match.\n"
        "6. Run the bundle: `run_script` with the source `run(\"<name>/main.ccat\")`. Pass "
        "that `run(...)` call and not the script's own text -- a bare `run_script` resolves "
        "template names against the script root, so the PNGs you just saved into the bundle "
        "are invisible to it.\n"
        "7. Change what you have with `update_script(name, source, templates)`: the source "
        "and the whole template set in one step, under one approval. Each entry is "
        "`{\"name\":...,\"cut\":{\"x\":...,\"y\":...,\"w\":...,\"h\":...},\"shot\":...}` "
        "for a box or the same with `{\"cx\":...,\"cy\":...,\"r\":...}` for a circle, and "
        "`shot` omitted means the most recent screenshot. `save_script` refuses an "
        "existing name, and the answer is never a second bundle under a new name."
    ),
    "script_rules": (
        "  * A failing statement stops the script. That error is what you get back -- not a "
        "screen that looks wrong.\n"
        "  * Anchor every action. After a tap, swipe or `run(...)`, assert the screen it "
        "should have produced with `wait_until(\"expected.png\", ms)`. A step that did not "
        "land then fails there, naming the file, instead of the next statement acting on a "
        "stale screen. Save the anchor's template with the script.\n"
        "  * Ground every `log(...)` in an anchor: never print \"task complete\" on its own. "
        "A `log(...)` that declares a state sits right after the `wait_until` or `if` on a "
        "template that confirms that state.\n"
        "  * Size that `ms` from the screenshots, not from a guess. Each caption says when it "
        "was taken and, within a turn, how long passed since the one before; a change that "
        "took +3.2s gets about twice that. `wait_until` stops as soon as the anchor appears, "
        "so a generous number costs nothing. Both waits are capped, not rejected -- "
        "`wait(ms)` at 5000 ms and `wait_until(\"a.png\", ms)` at 20000 ms -- so a larger "
        "number silently becomes the cap and you get less time than you asked for; "
        "`wait_until(\"a.png\", 0)` is a parse error. A timeout is an error that stops the "
        "script.\n"
        "  * Reuse before you write, and `update_script` before you duplicate: `save_script` "
        "refuses a name that already exists, and the answer is never a second bundle under a "
        "new name.\n"
    ),
    "script_failure_workflow": (
        "A script that fails stops at the statement that failed, and that error comes "
        "back to you with what the matcher saw (`best=`, the `box` it preferred, "
        "`threshold`). That `threshold` is the bar that match was measured against: a "
        "circular template is matched through its alpha mask, which leaves it less to "
        "go on than a square one, so the two are held to different bars. Find out which "
        "side is wrong before changing anything:\n"
        "\n"
        "1. `view_template(bundle, name)` -- what it was looking for (the name is in "
        "the error's path; `read_script` lists them).\n"
        "2. `view_crop_box(box={\"x\":...,\"y\":...,\"w\":...,\"h\":...})` with the error's "
        "`box`, or `view_crop_circle(circle={\"cx\":...,\"cy\":...,\"r\":...})` with "
        "its `circle` -- what is there now, at the template's own scale. `screenshot` "
        "when you need the whole screen instead.\n"
        "3. Compare both against the failing step's `//` comment: the same control "
        "looking different (a state you never waited for, a label that changes "
        "between runs), or nothing answering it at all (the step before it did not "
        "land, or this is not the screen you thought).\n"
        "4. Fix the cause once and run it again: `update_script` with that template "
        "re-cut when the cut is wrong, or a `wait_until` / a longer `ms` when the "
        "screen simply was not ready. Running the same script again unchanged is not "
        "a fix.\n"
        "5. Still failing after that: say what you now think the screen is doing, "
        "instead of changing another template."
    ),
}


def _read_gold_script() -> str:
    """The worked example, read from the library rather than copied here.

    One example in one file, edited in one place -- `ccat_help` reads `CCAT.md`
    the same way. It sits as a bundle under `ccat_library/`, which also makes it
    readable from the app's own editor. A missing file is not fatal, since the
    rules above stand on their own, so an empty string is the answer and the
    block is left out.
    """
    path = (
        Path(__file__).resolve().parents[4] / "ccat_library" / "gold.ccat"
    )
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


_example = _read_gold_script()
if _example:
    RULES["example_script"] = (
        "A whole script that follows every rule above: reuse through `run`, an anchor "
        "after each action, each `log` on its anchor, and every `ms` sized from the "
        "screenshots. Copy this shape:\n\n" + _example
    )

_CCAT_CHEATSHEET = (
    "The .ccat language. Call `ccat_help` for the full reference; the parts "
    "that are easy to get wrong:\n"
    "  * Comments are `//`; semicolons are optional. There are no expressions and no "
    "runtime variables: `defs` values are substituted while parsing.\n"
    "  * `tap_at(x, y)` / `swipe_at(...)` take coordinates normalized to [0,1] against "
    "the screenshot size, not pixels; out of range is an error.\n"
    "  * Actions: tap(\"a.png\"), tap_at(x, y), tap_offset(\"a.png\", dx, dy), "
    "swipe(\"a.png\", \"b.png\"), wait(ms), wait_until(\"a.png\", ms) (a timeout is an "
    "error), log(\"msg\"), home, run(\"other.ccat\").\n"
    "  * Control flow: if (has(\"a.png\")) { } else { }, elif for a chain, loop(n) "
    "{ }, do { } while (has(\"a.png\")) (stops hard at 50000 iterations), break, "
    "return. `if` / `elif` / `while` also take `has(\"a.png\")`, which answers "
    "whether that template is on screen right now, or one of the statements that "
    "reports a result (tap, tap_offset, swipe, tap_at, swipe_at, wait_until, "
    "run), which then runs and picks the branch: a failure is false, while a bare "
    "tap still fails the script. `return` / `return true` answer true, "
    "`return false` answers false, a script with no return answers true, and "
    "`run(...)` hands that answer back.\n"
    "  * A failing statement stops the script and comes back to you as an error. "
    "Statements that need a PNG (`tap`, `tap_offset`, `swipe`, `wait_until`) or a `.ccat` "
    "(`run`) fail when the file is missing; a `has(...)` whose template is missing is "
    "just false.\n"
)


SCRIPTER_PERSONA = (
    "You write and fix .ccat bundles for the operator. You never talk to the "
    "user; your reply is a tool result. The first line is exactly one of "
    "`saved`, `updated` or `failed`, then one to three sentences on what you "
    "did and how the run ended. Do the work with the tools before you answer: "
    "read and write the bundle, cut every template, run it, and fix it from the "
    "error. The brief is the whole conversation you can see. A constraint that "
    "is not in it does not exist."
)

SCRIPTER_RULE_NAMES = (
    "script_workflow", "script_rules", "script_failure_workflow", "example_script",
)


def _rules(names: tuple[str, ...]) -> str:
    """One `## name` block per rule, skipping a name the gold example did not add."""
    return "\n\n".join(
        f"## {name}\n\n{RULES[name]}" for name in names if name in RULES)


def scripter_prompt() -> str:
    """Who the scripter is, plus the language reference the operator no longer carries."""
    return SCRIPTER_PERSONA + "\n\n" + _rules(SCRIPTER_RULE_NAMES) + "\n\n" + _CCAT_CHEATSHEET


def _no_summary(messages: list) -> str:
    """What the operator hears when the scripter stops without a sentence."""
    last = ""
    for message in messages:
        if isinstance(message, ToolMessage):
            last = session.content_text(message.content)
    if not last:
        return "failed: script agent gave no summary"
    return "failed: script agent gave no summary\n" + last


def run_scripter(args: dict, host_tools: list) -> str:
    """Run the scripter to the end and return the text the operator should see.

    A failure here is a tool result, not a failed operator turn.
    """
    goal = str(args.get("goal") or "").strip()
    if not goal:
        return "error: empty goal"
    state = {
        "text": brief_of(args),
        "tools": scripter_tools(host_tools),
        "tool_rounds": 0,
    }
    try:
        # Callbacks only. The parent's whole config joins the two Pregel runs,
        # and a picture coming back then stalls the parent's stream.
        try:
            callbacks = get_config().get("callbacks")
        except Exception:  # noqa: BLE001 - outside a graph there is no config
            callbacks = None
        if callbacks:
            out = _scripter_graph().invoke(state, {"callbacks": callbacks})
        else:
            out = _scripter_graph().invoke(state)
    except Exception as exc:  # noqa: BLE001 - the operator hears it as a tool error
        return "error: " + str(exc)
    if out.get("error"):
        return "error: " + str(out["error"])
    reply = (out.get("reply") or "").strip()
    if not reply:
        own = (out.get("messages") or [])[out.get("input_len") or 0:]
        return _no_summary(own)
    return reply


def _scripter_build(state: dict) -> dict:
    """The scripter's request: its own prompt and history, plus the brief."""
    memorier.compact_scripter()
    messages: list[object] = [SystemMessage(content=scripter_prompt())]
    messages.extend(session.scripter_history())
    frame = session.latest_frame()
    if frame is not None:
        messages.append(frame)
    messages.append(HumanMessage(content=state.get("text", "")))
    # One past the brief, matching `build_messages`: `append_scripter_turn`
    # writes the brief itself, so the slice must not include it again.
    return {"messages": messages, "input_len": len(messages)}


def _scripter_invoke(state: dict) -> dict:
    """One scripter round. Only this round's text is kept.

    The operator accumulates text because the UI streams every round. The
    handoff result is the last round alone: an announcement before a tool
    call must not prefix `saved`, and must not hide an empty ending.
    """
    import graph
    fresh = dict(state)
    fresh["reply"] = ""
    return graph._invoke(fresh, state.get("tools") or [])


def _apply_scripter_tools(state: dict) -> dict:
    import graph
    messages = graph._run_host_tools(list(state.get("messages") or []))
    return {"messages": messages, "tool_rounds": state.get("tool_rounds", 0) + 1}


def _scripter_commit(state: dict) -> dict:
    """Keep the handoff only when the scripter's model call succeeded."""
    if state.get("error"):
        return {}
    session.append_scripter_turn(
        state.get("text", ""),
        list(state.get("messages") or [])[state.get("input_len") or 0:])
    return {}


def build_scripter_graph():
    import graph
    compiled = StateGraph(graph.TurnState)
    compiled.add_node("scripter_build", _scripter_build)
    compiled.add_node("scripter_invoke", _scripter_invoke)
    compiled.add_node("scripter_tools", _apply_scripter_tools)
    compiled.add_node("scripter_commit", _scripter_commit)
    compiled.set_entry_point("scripter_build")
    compiled.add_edge("scripter_build", "scripter_invoke")
    compiled.add_conditional_edges(
        "scripter_invoke",
        graph.route_after_invoke,
        {"run_tools": "scripter_tools", "commit": "scripter_commit"},
    )
    compiled.add_edge("scripter_tools", "scripter_invoke")
    compiled.add_edge("scripter_commit", END)
    return compiled.compile()


_SCRIPTER = None


def _scripter_graph():
    global _SCRIPTER
    if _SCRIPTER is None:
        _SCRIPTER = build_scripter_graph()
    return _SCRIPTER
