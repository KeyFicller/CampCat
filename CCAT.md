# CampCat (`.ccat`) language

Scripts drive Android emulator automation: template match → tap / swipe / wait.
PNG paths are always relative to the **folder of the current `.ccat` file**.

Comments: `//` to end of line. Semicolons are optional.

## `defs` (optional, top of file only)

Parse-time constants for **this** script only. Expanded while parsing; not kept at runtime.

```ccat
defs {
  n = 5
  x = 0.5
  btn = "ok.png"
  enabled = true
}
```

Values: number, string, `true` / `false`. Names are identifiers.
`if (enabled)` with a bool def is folded at parse time.

## Actions

| Statement | Meaning |
|-----------|---------|
| `tap("btn.png")` | One screencap → match → tap center |
| `tap_at(x, y)` | Tap normalized coords in `[0,1]` vs screenshot size |
| `tap_offset("btn.png", dx, dy)` | Match center + screen-normalized offset (signed) |
| `swipe("a.png", "b.png")` | One screencap; swipe between match centers |
| `swipe_at(x1, y1, x2, y2)` | Swipe normalized endpoints |
| `wait(ms)` | Sleep milliseconds |
| `wait_until("dlg.png", ms)` | Poll until template appears or timeout |
| `log("msg")` | Write a line to the shell log |
| `home` | HOME + force-stop recent non-home packages |
| `run("other.ccat")` | Run another script relative to this file’s directory (`..` allowed; depth/cycle limited) |

## Control flow

```ccat
if ("popup.png") {
  tap("ok.png")
} else {
  log("no popup")
}

retry(3) {
  tap("claim.png")
}

loop(5) {
  tap("next.png")
  break          // leave innermost loop / do-while / retry
}

do {
  tap("close.png")
} while ("close.png")

return           // end this script successfully
```

## Debug

```ccat
$Debug On        // dump match overlays (when config enables match_debug)
$Debug Off
```

## Minimal example

```ccat
defs {
  gap = 800
}

wait_until("home.png", 15000)
tap("daily.png")
wait(gap)
if ("claim.png") {
  tap("claim.png")
}
return
```

Point the shell’s script path at your `.ccat` under `config/` (typically `scripts/…`). Put template PNGs next to that file.
