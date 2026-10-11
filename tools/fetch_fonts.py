#!/usr/bin/env python3
"""Fetch the upstream UI fonts and prepare the copies the app ships with.

Run once; the outputs under `assets/fonts/` are committed and the build never
touches the network. Sources are pinned to a commit so a rerun is reproducible.

    python3 -m venv .venv-fonts
    .venv-fonts/bin/pip install --index-url https://pypi.org/simple fonttools
    .venv-fonts/bin/python tools/fetch_fonts.py            # generate (skips if current)
    .venv-fonts/bin/python tools/fetch_fonts.py --check    # verify only

`--index-url` matters: the machine's global pip config points at a mirror that
does not carry fontTools.

Why the fonts are prepared rather than shipped as downloaded: upstream ships
only variable fonts, and ImGui's rasterizer ignores variation axes, so every
weight would draw as Regular. The instances have to be baked out here.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

from fontTools.subset import Options, Subsetter
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

# Pinned so a rerun gets the same bytes; `main` moves. Bump deliberately, with
# the OFL texts re-checked, when a newer release is wanted.
GOOGLE_FONTS_COMMIT = "bd8f81ddb5c74d5c8897b36ad88b440266245103"

FONT_DIR = Path(__file__).resolve().parent.parent / "assets" / "fonts"
OFL_DIR = FONT_DIR / "OFL"
README = FONT_DIR / "README.md"

SOURCES = {
    "arimo": "ofl/arimo/Arimo[wght].ttf",
    "mono": "ofl/jetbrainsmono/JetBrainsMono[wght].ttf",
    "cjk": "ofl/notosanssc/NotoSansSC[wght].ttf",
}

# Kept verbatim, never retyped: OFL requires the notice and licence to travel
# with the binary, and a hand-copied licence text is how that goes wrong.
OFL_SOURCES = {
    "Arimo-OFL.txt": "ofl/arimo/OFL.txt",
    "JetBrainsMono-OFL.txt": "ofl/jetbrainsmono/OFL.txt",
    "NotoSansSC-OFL.txt": "ofl/notosanssc/OFL.txt",
}

# output name -> (source key, wght instance, subset?)
OUTPUTS: dict[str, tuple[str, int, bool]] = {
    "Arimo-Regular.ttf": ("arimo", 400, False),
    "Arimo-Bold.ttf": ("arimo", 700, False),
    "JetBrainsMono-Regular.ttf": ("mono", 400, False),
    "NotoSansSC-Common.ttf": ("cjk", 400, True),
}

README_ROW = re.compile(
    r"^\|\s*`(?P<name>[^`]+)`\s*\|\s*(?P<bytes>\d+)\s*\|\s*`(?P<sha>[0-9a-f]{64})`\s*\|"
)


def cdn(path: str) -> str:
    return f"https://cdn.jsdelivr.net/gh/google/fonts@{GOOGLE_FONTS_COMMIT}/{path}"


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "campcat-fetch-fonts"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        if resp.status != 200:
            raise RuntimeError(f"HTTP {resp.status} for {url}")
        return resp.read()


def gb2312_hanzi() -> set[int]:
    """GB2312 level 1 (3755) + level 2 (3008) = 6763 hanzi.

    Enumerated rather than shipped as a word list: reproducible, and nothing to
    keep in sync. A hanzi outside this set draws as a box, so widening the
    coverage means reaching for a bigger character set, not a longer constant.
    """
    out: set[int] = set()
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ord(ch))
    return out


def cjk_codepoints() -> list[int]:
    """What the merged CJK face has to cover.

    Mirrors the ranges the app asks ImGui for in `src/app/ui/fonts.cpp`, so the
    subset and the request cannot drift apart.
    """
    cps: set[int] = set()
    cps |= set(range(0x20, 0x7F))  # ASCII, in case a glyph is missing upstream
    cps |= set(range(0x2190, 0x2200))  # arrows: -> <- => draw as nothing without
    cps |= set(range(0x3000, 0x3040))  # CJK symbols and punctuation
    cps |= set(range(0xFF00, 0xFFF0))  # fullwidth forms
    cps |= gb2312_hanzi()
    return sorted(cps)


def instantiate(data: bytes, wght: int) -> bytes:
    """Bake every axis out, not just `wght`.

    Pinning one axis leaves the others live, so the result would still be a
    variable font and the weight would still be ignored.
    """
    font = TTFont(io.BytesIO(data))
    location = {axis.axisTag: axis.defaultValue for axis in font["fvar"].axes}
    location["wght"] = wght
    instantiateVariableFont(font, location, inplace=True, updateFontNames=True)
    assert "fvar" not in font, "font is still variable after instancing"
    return save(font)


def subset(data: bytes, unicodes: list[int]) -> bytes:
    font = TTFont(io.BytesIO(data))
    options = Options()
    # ImGui rasterizes through stb_truetype, which does no shaping: GSUB/GPOS
    # are unreachable weight.
    options.layout_features = []
    options.name_IDs = ["*"]
    options.notdef_outline = True
    subsetter = Subsetter(options=options)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    return save(font)


def save(font: TTFont) -> bytes:
    """Serialize with the source's `head.modified`, not the current time.

    fontTools stamps the save time by default, which makes the output differ on
    every run: `--check` would call a freshly generated tree stale, and each
    regeneration would show up as a 320 KB binary diff for no real change.
    """
    out = io.BytesIO()
    # `head` stamps the save time unless this is off; it is a constructor knob,
    # not a `save()` one.
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build() -> tuple[dict[str, bytes], dict[str, bytes]]:
    upstream = {key: fetch(cdn(path)) for key, path in SOURCES.items()}
    licences = {name: fetch(cdn(path)) for name, path in OFL_SOURCES.items()}

    cps = cjk_codepoints()
    outputs: dict[str, bytes] = {}
    for name, (key, wght, do_subset) in OUTPUTS.items():
        data = instantiate(upstream[key], wght)
        if do_subset:
            data = subset(data, cps)
        outputs[name] = data

    for name in OFL_SOURCES:
        (OFL_DIR / name).write_bytes(licences[name])
    return outputs, licences


def copyright_line(text: bytes) -> str:
    for line in text.decode("utf-8", "replace").splitlines():
        if line.strip():
            return line.strip()
    return "(no notice found)"


def write_readme(outputs: dict[str, bytes], licences: dict[str, bytes]) -> None:
    rows = "\n".join(
        f"| `{name}` | {len(data)} | `{sha256(data)}` |"
        for name, data in sorted(outputs.items())
    )
    notices = "\n".join(
        f"- {name.removesuffix('-OFL.txt')} -- {copyright_line(licences[name])}"
        for name in OFL_SOURCES
    )
    README.write_text(
        f"""# Bundled UI fonts

Generated by `tools/fetch_fonts.py`; do not edit by hand. Regenerate with:

    .venv-fonts/bin/python tools/fetch_fonts.py --force

## Sources

All from [google/fonts](https://github.com/google/fonts) at commit
`{GOOGLE_FONTS_COMMIT}`, pulled through jsDelivr.

| output | upstream | wght | subset |
|---|---|---|---|
| `Arimo-Regular.ttf` | `{SOURCES["arimo"]}` | 400 | no |
| `Arimo-Bold.ttf` | same | 700 | no |
| `JetBrainsMono-Regular.ttf` | `{SOURCES["mono"]}` | 400 | no |
| `NotoSansSC-Common.ttf` | `{SOURCES["cjk"]}` | 400 | GB2312 1+2, 6763 hanzi |

Variable fonts are instantiated because ImGui ignores variation axes: shipped
as-is, every weight would draw as Regular. The CJK face is additionally subset:
instancing alone still leaves 10.1 MB, while the subset is 2.1 MB.

The 20px title reuses `Arimo-Bold.ttf` and merges no CJK, as it always has.

## Checksums

`--check` compares the files on disk against this table.

| file | bytes | sha256 |
|---|---|---|
{rows}

## Licence

All three are SIL Open Font License 1.1; full texts in `OFL/`.

{notices}

OFL-1.1 permits use, modification, subsetting and redistribution inside an
application. It requires the notice and licence to travel with the binaries
(hence `OFL/`), and that modified copies not carry a Reserved Font Name -- the
outputs here are renamed, and none contains `Source`.

The proprietary faces this replaced (Arial, Menlo, Hiragino Sans GB, PingFang,
Arial Unicode) are not redistributable and are deliberately not in this tree.
""",
        encoding="utf-8",
    )


def generate(force: bool) -> int:
    if not force and check() == 0:
        print("already up to date (use --force to regenerate)")
        return 0

    FONT_DIR.mkdir(parents=True, exist_ok=True)
    OFL_DIR.mkdir(parents=True, exist_ok=True)
    outputs, licences = build()
    for name, data in sorted(outputs.items()):
        (FONT_DIR / name).write_bytes(data)
    write_readme(outputs, licences)

    for name, data in sorted(outputs.items()):
        print(f"  {name:26} {len(data):>9} B")
    return 0


def expected_files() -> dict[str, tuple[int, str]] | None:
    if not README.is_file():
        print("missing assets/fonts/README.md", file=sys.stderr)
        return None
    found = {
        m.group("name"): (int(m.group("bytes")), m.group("sha"))
        for m in (README_ROW.match(line) for line in README.read_text().splitlines())
        if m
    }
    if not found:
        print("no checksum table found in README.md", file=sys.stderr)
        return None
    return found


def check() -> int:
    expected = expected_files()
    if expected is None:
        return 1

    bad = 0
    for name, (size, digest) in sorted(expected.items()):
        path = FONT_DIR / name
        if not path.is_file():
            print(f"  MISSING  {name}")
            bad += 1
            continue
        data = path.read_bytes()
        if len(data) != size or sha256(data) != digest:
            print(f"  STALE    {name} ({len(data)} B)")
            bad += 1
    for name in OFL_SOURCES:
        if not (OFL_DIR / name).is_file():
            print(f"  MISSING  OFL/{name}")
            bad += 1

    if bad:
        print(f"{bad} problem(s); regenerate with --force", file=sys.stderr)
        return 1
    print(f"ok: {len(expected)} fonts + {len(OFL_SOURCES)} licences verified")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Fetch and prepare the bundled UI fonts."
    )
    ap.add_argument("--check", action="store_true", help="verify only, change nothing")
    ap.add_argument("--force", action="store_true", help="regenerate regardless")
    args = ap.parse_args()

    try:
        if args.check:
            return check()
        return generate(args.force)
    except (urllib.error.URLError, RuntimeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
