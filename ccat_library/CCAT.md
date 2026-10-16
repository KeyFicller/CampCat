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
`tap(ok.png)` is legal but spaces need quotes. A relative path resolves against the directory of the
`.ccat` file being run. Strings are `"..."` with `\n`, `\t`, `\\`, `\"`; any other `\x` is `x`.
Scripts you save live in a bundle, `ccat_library/<name>/main.ccat` with their templates beside it; run one with
`run("<name>/main.ccat")`. A `run` path resolves like any other relative one -- against the calling script's own
directory -- so a script that is itself saved in a bundle reaches another one as `run("../<name>/main.ccat")`:
without the `../`, `<name>/main.ccat` means a directory inside its own bundle.
A PNG path is a file name in that same bundle (`tap("a.png")`). One with `/` or `..` is
refused when the script is saved; cut the picture into this bundle instead.

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
| `goto("g:a")` | walk graph `g` to anchor `a` | graph invalid; `a` is not a page; an anchor script fails or two match; three failed hops; adb failure |
| `at("g:a")` | condition only: one screenshot, true when the screen locates as `a` | not a statement; an invalid graph, a failing anchor script or two matching anchors is an error, not `false` |

**A named file that is missing fails the statement**, which is why `tap`, `tap_offset`, `swipe`, `wait_until` and
`run` need their files to exist first. `has(...)` is the exception (below): a template that is not there is exactly
what it reports.

Any statement in the table above that reports a result can also stand as an `if` / `elif` / `while` condition, where
a failure is `false` instead of stopping the script. See **Conditions that ask the screen**.

`update_script` checks exactly this list: every template a `tap`, `tap_offset`, `swipe` or `wait_until` names must be
in the declared template set, and it refuses the update naming the ones that are not. A condition counts the same way
-- `if (tap("x.png"))` really taps, so its template has to be declared -- but `has(...)` does not, because a template
that is not there is the `false` it reports. An anchor's (see **Graphs**) does count: a `has` it can never match
means it never recognises its screen. `run()` targets are not checked; absolute paths are not bundle files, so
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

A graph `G` is the screens of one game, kept as ordinary bundles. Names are `[a-z0-9_]+`.

| Bundle | Is | Recognised by |
|---|---|---|
| `G.entry` | the start anchor | named `entry`, holds `golden.md` |
| `G.<anchor>` | an anchor: one screen | holds `golden.md` |
| `G.<a>-<b>` | the edge from `a` to `b` | both `a` and `b` are anchors of `G` |
| `G.<other>` | a plain task or skill | none of the above |

`G` is a graph when `G.entry/golden.md` exists. A bundle named `G.<a>-<b>` whose ends are both
anchors is always an edge, so do not give a task that name. A bundle named just `G` is not
needed; when there is one it is a plain bundle, for example the game's default task.

### Anchors

`golden.md` is front matter, then a description of the screen:

```text
---
kind: page
---
Main city map: avatar and the task book top left, a row of resources along the top,
five buttons along the bottom. Buildings, resource numbers and chat change.
```

- `kind` is `page` (the default, a screen you can stay on), `transient` (a loading or
  transition screen: `goto` waits it out, it is never a destination) or `popup` (it can
  cover any screen: `goto` closes it first).
- A `popup` also needs `close: <bundle>`, the bundle that closes it.
- The description says what is fixed on the screen and what changes. It is stored for
  locating by meaning later; nothing reads it yet.

The anchor's `main.ccat` only judges whether its screen is showing:

```ccat
if (has("neizheng.png")) { return true }
return false
```

It may use `defs`, `has`, `if` / `elif` / `else`, `return`, `log` and comments, and needs at least
one `return` -- with none it would answer true on every screen. Every anchor runs on every
locate, so actions, waits, `run`, `loop`, `do`, `goto` and `at` are refused. Its templates
live in its own bundle. A `has` whose only job is `return false` does not split one
chrome into two anchors. Another mode of that screen stays the same anchor; the work
that mode does is an action.

### Edges

An edge's `main.ccat` performs the move from `a` to `b`. It may branch -- a cold start and a
warm start take different paths -- but it never checks its own ends, because the engine
does, and it must not call `goto` (`goto inside a graph script`). A move slower than 20 s
waits for itself: `wait_until` on a template of `b`, or `loop` + `wait_until` for a long load.

### Actions

An action's bundle is `{graph}.{anchor}.{action}` (for example `stzb.kucang.claim`, tagged
`[action of stzb on kucang]`). It is work that starts and ends on the same screen. The script
may leave that screen briefly. It must not call `goto` or `at`. Templates live in the action
bundle. `run("{graph}.{anchor}.{action}/main.ccat")` checks both ends the way an edge does,
once, and does not retry: not on the anchor fails without running, back on it is success,
another page or a 20000 ms timeout is a failure. Spending or claiming goes in an action.

### Locating

A locate takes one screenshot and runs every anchor script against that one frame:

1. `popup` anchors first, in name order; the first that answers true is the result.
2. Then every `page` and `transient` anchor. None true is unknown, one is the result, two
   or more is an error naming them (`ambiguous: a; b`).
3. An anchor script that fails is an error naming that anchor.

### `goto`, `at` and `run` of an edge

`goto("G:a")` locates, then:

- a popup runs its `close` bundle and locates again (at most 10 popups);
- loading or unknown waits, looking every 200 ms for up to 20 s;
- the target is success.

Otherwise it runs the first hop of the shortest path (fewest hops; ties go to the edge
name), then looks every 200 ms for 20000 ms: the far end finishes the hop, a popup is
closed, another page replans without counting a failure, and still on the start or
unknown at the timeout is a failure. A failing edge script, a timeout and no path each
count one failure; the third is the error, listing every hop. `if (goto("G:a"))` is false
when it cannot get there; a dead device is still an error.

`at("G:a")` is one locate, as a condition: true when the result is `a`. An invalid graph
or an ambiguous locate is an error, not `false`.

`run("G.a-b/main.ccat")` is one checked hop: it locates (closing popups), fails with
`run G.a-b: not at a (at <x>)` without running anything when the screen is not `a`, runs the
edge, then waits 20000 ms for `b`. A timeout or another page is a failure that names the
last screen seen. It does not replan or retry. Inside `goto` the edge body runs without
this second check.

### Checks

Every `goto` / `at` loads the graph and refuses an invalid one, naming every problem:
`G.entry` exists and is a `page`; every `golden.md` parses, and a popup's `close` bundle
parses; every anchor script parses and only judges; every `has` template is in its bundle;
every edge joins two `page` anchors, parses and has no `goto`; every `page` anchor is
reachable from `entry`. Saving an anchor or an edge reports the same list.

### Rules for a graph that stays reliable

1. One screen, one anchor. Tell two screens apart by a template of a control only one of
   them shows. Every PNG in the bundle is drawn on that anchor in the graph editor, so do
   not save a picture of the other screen just to `return false`. Never a lower threshold.
2. An edge must be safe to run again. Spending or claiming goes in an action.
3. Do not `tap_at` when a `tap` missed. Let it fail.
4. In a task, do not `loop` + `has` + `wait` for a screen: `goto` it. Only an edge waits
   for itself.
5. A task is `goto` to the page, `run` the action, then a check of the result.

## `$Debug`

`$Debug On` / `$Debug Off` (`on` / `off` too) toggles the per-match debug dumps regardless of the `match_debug`
config value, which only sets the starting state. Dumps land in the configured `match_debug_dir`.
