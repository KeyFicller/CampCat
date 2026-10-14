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
Scripts you save live in a bundle, `ccat_library/<name>/main.ccat` with their templates beside it; run one with
`run("<name>/main.ccat")`. A `run` path resolves like any other relative one -- against the calling script's own
directory -- so a script that is itself saved in a bundle reaches another one as `run("../<name>/main.ccat")`:
without the `../`, `<name>/main.ccat` means a directory inside its own bundle.

**`<name>` is a skill.** The first line of `main.ccat` is its one-line description when it starts with `//`:
`// taps the login button and waits for the feed`. `list_scripts` shows every bundle with its description; read one
before writing a new skill, and build on what is there with `run()` rather than copying its statements. Change an
existing skill with `update_script`, which replaces the script and its template set together.

Counts and timeouts are non-negative integer literals: `wait(0)` and `loop(0)` are fine, `wait_until("a.png", 0)` is
rejected while parsing. No arithmetic: `wait(1000 + 500)` is a syntax error.

Waits are **capped, not rejected**: `wait(ms)` runs at most `5000` ms and `wait_until("a.png", ms)` at most `20000` ms,
so a larger number silently becomes the cap rather than an error.

```ccat
defs { claim = "claim.png" }
if (has(claim)) { tap(claim) } else { log("no claim") }
```

## Statements

| Statement | Does | Fails when |
|---|---|---|
| `tap("a.png")` | match, tap the center | not found; adb failure |
| `tap_at(x, y)` | tap normalized coords | `x` / `y` outside `[0,1]` |
| `tap_offset("a.png", dx, dy)` | tap center + `(dx, dy)` x screen | point lands off screen |
| `swipe("a.png", "b.png")` | one screenshot, center to center | either not found |
| `swipe_at(x1, y1, x2, y2)` | swipe normalized endpoints | a coordinate outside `[0,1]` |
| `wait(ms)` | sleep, at most 5000 ms | nothing |
| `wait_until("a.png", ms)` | poll until matched, timeout at most 20000 ms | timeout |
| `log("msg")` | one log line | nothing |
| `home` | HOME + force-stop recents | adb failure |
| `run("other.ccat")` | another script, relative to this file | unreadable; cyclic; deeper than 16; callee error |
| `goto("g:a")` | walk graph `g` to anchor `a` | graph invalid; `a` is not a place to stay; still not there after replan and one recover; adb failure |
| `at("g:a")` | condition only: one screenshot, true when that anchor matches | not a statement; an invalid graph is an error, not `false` |

**A named file that is missing fails the statement**, which is why `tap`, `tap_offset`, `swipe`, `wait_until` and
`run` need their files to exist first. `has(...)` is the exception (below): a template that is not there is exactly
what it reports.

Any statement in the table above that reports a result can also stand as an `if` / `elif` / `while` condition, where
a failure is `false` instead of stopping the script. See **Conditions that ask the screen**.

`update_script` checks exactly this list: every template a `tap`, `tap_offset`, `swipe` or `wait_until` names must be
in the declared template set, and it refuses the update naming the ones that are not. A condition counts the same way
-- `if (tap("x.png"))` really taps, so its template has to be declared -- but `has(...)` does not, because a template
that is not there is the `false` it reports. `run()` targets are not checked; absolute paths are not bundle files, so
they are not either.

After a tap or swipe the script waits `max(tap_delay_ms, action_gap_ms)` (config, 1000 ms), so you rarely need `wait`
after an action.

## Anchoring a step

After an action, assert the screen it should have produced with `wait_until`. The anchor
is the image that only appears when the action worked:

```ccat
tap(login.png)
wait_until("feed.png", 5000)   // anchor: the feed only appears if the tap landed
```

`wait_until` polls every 50 ms until the template matches. If it never does, the statement
fails with `wait_until: timeout after <ms> ms for <file>`, and a failing statement stops
the script, so the error names the step that did not land instead of letting the rest of
the script run against a stale screen. Anchor after `run()` too: a callee that succeeds
can still leave the screen somewhere you did not expect.

`wait_until` is not `has`. `if (has("a.png"))` takes one screenshot and treats a missing template
as false, which is how you branch on the screen you are on. `wait_until` waits for the
screen you expect, and fails rather than falling through.

## Control flow

`if (has("a.png")) { ... } else { ... }` takes one screenshot, then runs a branch. **A missing template is not an
error here: the condition is simply false.** `else` is optional.

`elif` is `else if`: a chain is tried in order and stops at the first condition that holds, so write the most
specific one first and put `else` last. `else if` still spells the same thing.

`loop(n) { ... }` runs the body exactly `n` times; `0` succeeds.

To wait for a template to show up, use `wait_until("a.png", 2000)`: it looks every 50 ms and **times out with an
error**, so a screen that never arrives stops the script. For anything else, poll with a loop -- but note that
running out of attempts is **not** an error:

```ccat
loop(5) { if (tap_at(0.5, 0.5)) { break } wait(50) }
```

`do { ... } while (has("a.png"))` runs the body first, then repeats while the condition holds, giving up with an
error after **50000** iterations. It takes every condition `if` does -- see **Conditions that ask the screen** below.

`break` leaves the innermost `loop` or `do`/`while`; elsewhere it is an error (`break outside loop`). `return` ends
the whole script and carries its result: `return` and `return true` both answer `true`, `return false` answers
`false`, and a boolean `def` may stand in for either. A script that runs to the end with no `return` answers `true`,
so **answering false is something you say, not something that happens** -- an error is still an error, never a
`false`.

`run("other.ccat")` hands that answer back, which is how one script asks another a question:

```ccat
if (run("logged_in.ccat")) { tap("feed.png") } else { tap("login.png") }
```

A bare `run` of a script that answered `false` is not a failure: the caller carries on, and the answer is only
visible where you test it with `if`. A callee's `return` ends the callee, never the caller. An error stops the script
wherever it happens.

## Conditions that ask the screen

Two kinds of statement may stand as an `if` / `elif` / `while` condition.

`has("a.png")` answers whether that template is on screen **right now** -- one screenshot, `true` or `false`, never
an error. `at("g:a")` is the same kind of condition for a graph anchor (see **Graphs**); an invalid graph is an
error, not `false`:

```ccat
if (has("a.png")) { log("here") } else { log("not here") }
```

Any statement in the table above that can report a result also works: `tap`, `tap_offset`, `swipe`, `tap_at`,
`swipe_at`, `wait_until`, `run`, `goto`. It runs, and its result picks the branch: a failure is `false`, so it does not stop
the script.

```ccat
if (tap("claim.png")) { log("collected") } else { log("no claim button") }
if (wait_until("feed.png", 8000)) { tap("post.png") }
```

A chain reads as "whichever of these is on screen first", because an action that finds nothing changes nothing:

```ccat
if (tap("claim.png")) { log("claimed") }
elif (tap("sign_in.png")) { log("signed in") }
elif (has("already.png")) { log("already claimed") }
else { log("neither is here") }
```

Nothing else. `log`, `wait` and `home` cannot be conditions: the first two always succeed, and `home` only fails when
the device itself is gone, which is not a screen state. `break`, `return`, `loop`, `do` are control flow rather than
answers. A template is always tested through `has(...)` -- never as a string on its own, so a condition never leaves
you guessing whether it is a template or a boolean.

**A bare `tap("x.png")` still fails the script.** Being written as a condition is what turns a failure into a
`false`. Nothing is written to the log for you: if you want a record of which branch was taken, `log` it yourself in
the branch. One screenshot, not two: `if (tap("x.png")) { ... }` captures once, where
`if (has("x.png")) { tap("x.png") }` captures for the test and again for the tap.

A condition's action is checked like a bare one: `update_script` needs the `x.png` of `if (tap("x.png"))` in the
bundle. A `has(...)` template is **not** checked -- "that template is not here" is a `false`, not an error, so a typo
in `has` shows up as the else branch.

`do ... while` takes the same conditions and repeats **while** one holds -- the usual way to page through a list
until the next button is gone:

```ccat
do { tap("swipe_left.png") } while (has("next_page.png"))
```

`while (true)` repeats until a `break` in the body, so it is how you write a loop that ends on a condition of your
own; it still errors if it reaches 50000 iterations. `while (false)` runs the body once. Nothing is logged for you
here either.

## defs

Optional. **At most one block, the first statement of its file**; a duplicate name is an error. Values: number,
string, `true`, `false`: `defs { n = 3  btn = "ok.png"  fast = true }`.

`defs` is parse-time substitution (the AST never sees it), so a string def can stand wherever a PNG path or a message
is expected. `if (fast)` on a bool def is folded into the chosen branch while parsing; a **number** def in an `if` is
a syntax error, and a **string** def has to be spelled `has(name)` -- a bare `if (name)` would not say whether it
means a template or a boolean.

## Coordinates & timing

Normalized `[0,1]` against the current screenshot, never pixels: `tap_at(0.5, 0.5)` is the center. Each axis maps to
`lround(n x (dim - 1))`, and a value outside `[0,1]` is an error rather than a clamp. `tap_offset` scales `dx` / `dy`
by the full screen and errors if the result is off screen, so keep offsets small and signed. The match threshold and
the swipe duration come from config; a script cannot change them.

## Graphs

A graph is a bundle that holds `graph.json` next to `main.ccat`. Its name is the bundle
name. Anchors are screens. Edges are other bundles that only perform the action between
two anchors; `goto` checks the screen before and after, so the edge does not.

```json
{
  "root": "desktop",
  "recover": "shuaitu.recover",
  "anchors": {
    "desktop": { "must": ["desktop_folder.png"] },
    "main_city": { "must": ["recruit_button.png"] }
  },
  "edges": [
    { "from": "desktop", "to": "main_city", "script": "shuaitu.desktop-main", "timeout_ms": 60000 }
  ],
  "interrupts": [
    { "anchor": "notice", "script": "shuaitu.close-notice" }
  ]
}
```

- `must` is the templates that all have to match. `must_not` is the ones that must not.
  An entry is a file name, or `{"png", "roi"}` with `roi` normalized `[x, y, w, h]`.
- `transient: true` is a loading screen. `goto` waits it out. It is not a destination.
- An interrupt anchor is a popup. `goto` closes it before planning. It is not a destination.
- `timeout_ms` is how long to wait for the edge's `to` after the edge script returns.
  Default 20000, at most 600000. This is not the `wait_until` cap.
- `recover` runs once, after three failed attempts, then `goto` tries again. No `recover`
  means the third failure is the error.
- Every stable anchor must be reachable from `root`. A bad `graph.json` names every
  problem and does not run.

`goto("g:a")` takes one screenshot, decides where it is, and runs the next edge on the
shortest path. Landing on a different anchor replans without counting a failure. Three
failures, then one recover. The error lists the hops. `if (goto("g:a"))` is false when
it cannot get there; a dead device is still an error. `at("g:a")` is the same locate,
as a condition, and does not walk.

An edge, a popup script and the recover script must not call `goto`. That is
`goto inside a graph script`.

Rules for a graph that stays reliable:

1. One edge, one bundle, named `<graph>.<from>-<to>`. Actions only. No "where am I".
2. An edge must be safe to run again. Spending or claiming goes in the task, after `goto`.
3. Do not `tap_at` when a `tap` missed. Let it fail.
4. Do not `loop` + `has` + `wait` for a screen. Wait with the edge's `timeout_ms`, and
   mark a loading screen `transient`.
5. A task is `goto` to the start, the action, then a result anchor (`wait_until` or `at`).
6. Two anchors that match the same screenshot need a `must_not` or a `roi`.
7. `run` the recover bundle, then `goto` each anchor, to recheck the whole graph.

## `$Debug`

`$Debug On` / `$Debug Off` (`on` / `off` too) toggles the per-match debug dumps regardless of the `match_debug`
config value, which only sets the starting state. Dumps land in the configured `match_debug_dir`.
