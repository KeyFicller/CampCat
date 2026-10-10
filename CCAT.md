# CampCat (`.ccat`) language

Written for a model to read, not for people: the `ccat_help` tool returns this file verbatim.

Scripts drive an Android device over ADB: screenshot, template match, tap / swipe / wait.

- **A failing statement stops the script**; that error is what you get back. Nothing catches it.
- **No expressions, no variables.** Numbers only where a statement takes one; `defs` values are substituted while
  parsing.

## Syntax at a glance

Every statement that names a PNG takes its own fresh screenshot. `//` comments to end of line, no block comments;
`;` between statements is optional. Everything is parsed before anything runs, so a syntax error never reaches the
device.

A PNG path or a message may be a quoted string, a string `def`, or a bare word (letters, digits, `_`, `.`, `-`), so
`tap(ok.png)` is legal but spaces need quotes. Absolute paths are used as is; relative ones resolve against the
directory of the `.ccat` file being run. Strings are `"..."` with `\n`, `\t`, `\\`, `\"`; any other `\x` is `x`.
Scripts you save live in a bundle, `config/scripts/llm/<name>/main.ccat` with their templates beside it; run one from
another script with `run("<name>/main.ccat")`.

**`<name>` is a skill.** The first line of `main.ccat` is its one-line description when it starts with `//`:
`// taps the login button and waits for the feed`. `list_scripts` shows every bundle with its description; read one
before writing a new skill, and build on what is there with `run()` rather than copying its statements. Change an
existing skill with `update_script`, which replaces the script and its template set together.

Counts and timeouts are non-negative integer literals: `wait(0)` and `loop(0)` are fine, `wait_until("a.png", 0)` and
`retry(0)` are rejected while parsing. No arithmetic: `wait(1000 + 500)` is a syntax error.

```ccat
defs { claim = "claim.png" }
if (claim) { tap(claim) } else { log("no claim") }
```

## Statements

| Statement | Does | Fails when |
|---|---|---|
| `tap("a.png")` | match, tap the center | not found; adb failure |
| `tap_at(x, y)` | tap normalized coords | `x` / `y` outside `[0,1]` |
| `tap_offset("a.png", dx, dy)` | tap center + `(dx, dy)` x screen | point lands off screen |
| `swipe("a.png", "b.png")` | one screenshot, center to center | either not found |
| `swipe_at(x1, y1, x2, y2)` | swipe normalized endpoints | a coordinate outside `[0,1]` |
| `wait(ms)` | sleep | nothing |
| `wait_until("a.png", ms)` | poll until matched | timeout |
| `log("msg")` | one log line | nothing |
| `home` | HOME + force-stop recents | adb failure |
| `run("other.ccat")` | another script, relative to this file | unreadable; cyclic; deeper than 16; callee error |

**A named file that is missing fails the statement**, which is why `tap`, `tap_offset`, `swipe`, `wait_until` and
`run` need their files to exist first. `if` is the exception (below).

`update_script` checks exactly this list: every template a `tap`, `tap_offset`, `swipe` or `wait_until` names must be
in the declared template set, and it refuses the update naming the ones that are not. `if` conditions and `run()`
targets are not checked; absolute paths are not bundle files, so they are not either.

After a tap or swipe the script waits `max(tap_delay_ms, action_gap_ms)` (config, 1000 ms), so you rarely need `wait`
after an action.

## Control flow

`if ("a.png") { ... } else { ... }` takes one screenshot, then runs a branch. **A missing template is not an error
here: the condition is simply false.** `else` is optional.

`loop(n) { ... }` runs the body exactly `n` times; `0` succeeds.

`retry(n) { ... }` runs the body **until it succeeds**, 50 ms apart: success exits early, `break` counts as success,
and if every attempt fails the last error is what the script gets.

`do { ... } while ("a.png")` runs the body first, then repeats while the template is visible, giving up with an error
after **50000** iterations, so a polling loop needs another way out.

`break` leaves the innermost `loop` / `do` / `retry`; elsewhere it is an error (`break outside loop`). `return` ends
the whole script successfully, including from inside a `retry`. An error stops the script wherever it happens, except
inside a `retry`, which uses it only to decide whether to try again.

## defs

Optional. **At most one block, the first statement of its file**; a duplicate name is an error. Values: number,
string, `true`, `false`: `defs { n = 3  btn = "ok.png"  fast = true }`.

`defs` is parse-time substitution (the AST never sees it), so a string def can stand wherever a PNG path or a message
is expected. `if (fast)` on a bool def is folded into the chosen branch while parsing; a **number** def in an `if` is
a syntax error.

## Coordinates & timing

Normalized `[0,1]` against the current screenshot, never pixels: `tap_at(0.5, 0.5)` is the center. Each axis maps to
`lround(n x (dim - 1))`, and a value outside `[0,1]` is an error rather than a clamp. `tap_offset` scales `dx` / `dy`
by the full screen and errors if the result is off screen, so keep offsets small and signed. The match threshold and
the swipe duration come from config; a script cannot change them.

## `$Debug`

`$Debug On` / `$Debug Off` (`on` / `off` too) toggles the per-match debug dumps regardless of the `match_debug`
config value, which only sets the starting state. Dumps land in the configured `match_debug_dir`.
