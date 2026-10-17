"""Browse what the region sampler collected.

    python3 campcat/sources/py/view_samples.py [--root DIR] [--open]

Each `<root>/<provider>/<sample>/` written by `record_region_sample` holds a
`frame.png` and a `meta.json` naming two regions. This draws both over the
frame -- the model's `given` in blue, the human's `human` in red -- and writes
`<root>/viewer/index.html` plus one thumbnail per sample. Read only: no sample
is touched, so it is safe to point at a library still being collected.
"""

import argparse
import html
import json
import sys
import webbrowser
from pathlib import Path

from PIL import Image, ImageDraw

GIVEN = (64, 156, 255)
HUMAN = (255, 64, 96)
THUMB = (700, 700)
# `ponytail:` thumbnails are named after their sample, so a sample deleted from
# disk leaves its picture behind in `viewer/`. Clear that directory by hand if
# the stale ones get in the way.
_CSS = """
body{background:#16161a;color:#d8d8e0;font:14px/1.5 -apple-system,sans-serif;
     margin:24px}
h1{font-size:18px;font-weight:600;margin:0 0 4px}
h2{font-size:15px;margin:28px 0 10px}
h2 span{color:#8a8a96;font-weight:400}
.legend{margin:0 0 20px;color:#8a8a96}
.given{color:#409cff}.human{color:#ff4060}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));
      gap:14px}
figure{margin:0;background:#1e1e24;border-radius:8px;overflow:hidden}
img{display:block;width:100%;height:auto}
figcaption{padding:8px 10px;font-size:12px;color:#9a9aa6}
figcaption b{color:#d8d8e0;font-weight:600}
"""


def _centre(region):
    if region["shape"] == "box":
        return region["x"] + region["w"] / 2, region["y"] + region["h"] / 2
    return region["cx"], region["cy"]


def delta_text(given, human):
    """What the human changed, in the sample's own pixels."""
    parts = []
    if given["shape"] != human["shape"]:
        parts.append(f"{given['shape']} -> {human['shape']}")
    gx, gy = _centre(given)
    hx, hy = _centre(human)
    if hx != gx or hy != gy:
        parts.append(f"centre {hx - gx:+.0f},{hy - gy:+.0f}")
    for key in ("w", "h", "r", "rx", "ry"):
        if key in given and key in human and human[key] != given[key]:
            parts.append(f"{key} {human[key] - given[key]:+.0f}")
    return "   ".join(parts) or "unchanged"


def _draw(draw, region, colour, width):
    shape = region["shape"]
    if shape == "box":
        x, y, w, h = region["x"], region["y"], region["w"], region["h"]
        draw.rectangle([x, y, x + w, y + h], outline=colour, width=width)
    elif shape == "circle":
        cx, cy, r = region["cx"], region["cy"], region["r"]
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=colour,
                     width=width)
    else:
        cx, cy, rx, ry = region["cx"], region["cy"], region["rx"], region["ry"]
        draw.polygon([(cx, cy - ry), (cx + rx, cy), (cx, cy + ry),
                      (cx - rx, cy)], outline=colour, width=width)


def _samples(provider):
    # Folder names start with the write time, so the newest sorts first.
    dirs = (d for d in provider.iterdir() if d.is_dir())
    return sorted(dirs, key=lambda d: d.name, reverse=True)


def render(root):
    """Write the page and the thumbnails; return the page's path."""
    root = Path(root)
    out = root / "viewer"
    groups = []
    for provider in sorted(p for p in root.iterdir()
                           if p.is_dir() and p.name != "viewer"):
        cards = []
        for sample in _samples(provider):
            meta_path = sample / "meta.json"
            frame_path = sample / "frame.png"
            if not (meta_path.is_file() and frame_path.is_file()):
                continue
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                frame = Image.open(frame_path).convert("RGB")
                _draw(ImageDraw.Draw(frame), meta["given"], GIVEN,
                      max(2, round(frame.width / 400)))
                _draw(ImageDraw.Draw(frame), meta["human"], HUMAN,
                      max(2, round(frame.width / 400)))
            except (KeyError, OSError, ValueError) as ex:
                print(f"[skip] {sample}: {ex}", file=sys.stderr)
                continue
            frame.thumbnail(THUMB)
            png = out / provider.name / f"{sample.name}.png"
            png.parent.mkdir(parents=True, exist_ok=True)
            frame.save(png)
            rel = f"{provider.name}/{sample.name}.png"
            cards.append((sample.name, rel, delta_text(meta["given"],
                                                       meta["human"])))
        if cards:
            groups.append((provider.name, cards))

    body = []
    for provider, cards in groups:
        body.append(f"<h2>{html.escape(provider)} <span>{len(cards)}</span>"
                    "</h2><div class=grid>")
        for name, rel, delta in cards:
            body.append(
                f"<figure><img loading=lazy src=\"{html.escape(rel, True)}\">"
                f"<figcaption><b>{html.escape(name)}</b><br>"
                f"{html.escape(delta)}</figcaption></figure>")
        body.append("</div>")

    total = sum(len(cards) for _, cards in groups)
    out.mkdir(parents=True, exist_ok=True)
    page = out / "index.html"
    page.write_text(
        "<!doctype html><meta charset=\"utf-8\">"
        "<title>region samples</title>"
        f"<style>{_CSS}</style>"
        f"<h1>region samples &mdash; {total}</h1>"
        "<p class=legend><span class=given>model proposal</span> / "
        "<span class=human>human</span>. The line under each picture is the "
        "human's change from the proposal, in screen pixels.</p>"
        + "\n".join(body),
        encoding="utf-8")
    return page


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parents[3] / "training_data",
        help="where the samples live (default: the repo's training_data)")
    parser.add_argument("--open", action="store_true",
                        help="open the page in the browser")
    args = parser.parse_args()
    page = render(args.root)
    print(page)
    if args.open:
        webbrowser.open(page.as_uri())


if __name__ == "__main__":
    main()
