"""The sample viewer: does it find the samples and say what a human changed."""

import json

from PIL import Image

import view_samples

BOX = {"shape": "box", "x": 10, "y": 10, "w": 40, "h": 20}


def sample(root, provider, name, given, human):
    d = root / provider / name
    d.mkdir(parents=True)
    Image.new("RGB", (200, 100), (20, 20, 20)).save(d / "frame.png")
    (d / "meta.json").write_text(json.dumps({"given": given, "human": human}))


def test_render_groups_every_sample_by_provider(tmp_path):
    sample(tmp_path, "qwen", "1-1", BOX, dict(BOX, x=14))
    sample(tmp_path, "deepseek", "2-1", BOX, BOX)

    page = view_samples.render(tmp_path).read_text(encoding="utf-8")

    assert "qwen" in page and "deepseek" in page
    assert "1-1" in page and "2-1" in page
    assert "centre +4,+0" in page
    assert "unchanged" in page
    assert (tmp_path / "viewer" / "qwen" / "1-1.png").is_file()


def test_a_shape_change_is_named(tmp_path):
    sample(tmp_path, "qwen", "1-1", BOX,
           {"shape": "circle", "cx": 30, "cy": 20, "r": 12})

    page = view_samples.render(tmp_path).read_text(encoding="utf-8")

    assert "box -&gt; circle" in page


def test_an_unreadable_sample_is_skipped_whole(tmp_path):
    sample(tmp_path, "qwen", "1-1", BOX, BOX)
    (tmp_path / "qwen" / "1-1" / "meta.json").write_text("{}")

    assert "1-1" not in view_samples.render(tmp_path).read_text(
        encoding="utf-8")
