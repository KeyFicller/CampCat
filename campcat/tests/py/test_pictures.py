"""Pictures in the message list: screenshots and small crops are counted apart.

A crop must not push the screenshot it was cut from out of the request, and
crops are cleared in a batch so a new one does not rewrite the prefix the
provider caches.
"""

import base64
import copy
import io

import session
from PIL import Image


def _png(w, h):
    buf = io.BytesIO()
    Image.new("RGB", (w, h), (200, 10, 10)).save(buf, "PNG")
    return base64.b64encode(buf.getvalue()).decode()


def _urls(message):
    if not isinstance(message.content, list):
        return []
    return [
        part["image_url"]["url"]
        for part in message.content
        if isinstance(part, dict) and part.get("type") == "image_url"
    ]


def _crop(image, n=0):
    return session.picture_of("view_crop", image, {"region": {"n": n}})


def setup_function():
    session.reset()


def test_crops_do_not_push_the_screenshot_out():
    big, small = _png(2000, 1000), _png(40, 40)
    shot = session.screenshot_frame(1, big)
    messages = [shot] + [_crop(small, n) for n in range(8)]
    before = [_urls(m) for m in messages]

    session.demote_pictures(messages)

    assert [_urls(m) for m in messages] == before


def test_crops_are_cleared_in_one_batch():
    big, small = _png(2000, 1000), _png(40, 40)
    messages = [session.screenshot_frame(1, big)] + [
        _crop(small, n) for n in range(9)
    ]

    session.demote_pictures(messages)

    crops = messages[1:]
    assert all(_urls(m) for m in crops[-4:])
    for old in crops[:5]:
        assert not _urls(old)
        text = session.content_text(old.content)
        assert "make the same call again" in text
        assert 'view_crop({"region":' in text


def test_new_crop_under_the_high_mark_rewrites_nothing():
    big, small = _png(2000, 1000), _png(40, 40)
    messages = [session.screenshot_frame(1, big)] + [
        _crop(small, n) for n in range(5)
    ]
    session.demote_pictures(messages)
    kept = copy.deepcopy(messages)

    messages.append(_crop(small, 5))
    session.demote_pictures(messages)

    assert messages[:-1] == kept


def test_template_and_crop_in_one_batch_both_stay():
    big, small = _png(2000, 1000), _png(40, 40)
    messages = [
        session.picture_of("view_template", small, {"bundle": "b", "name": "n"}),
        _crop(small),
        session.screenshot_frame(1, big),
    ]

    session.demote_pictures(messages)

    assert all(_urls(m) for m in messages)


def test_large_crop_counts_as_a_frame():
    wide = _png(1200, 100)
    crop = session.picture_of("view_crop", wide, {"region": {"n": 0}})
    assert not (isinstance(crop.id, str) and crop.id.startswith("crop:"))
    messages = [crop, session.screenshot_frame(1, _png(2000, 1000))]

    session.demote_pictures(messages)

    assert not _urls(messages[0])
    text = session.content_text(messages[0].content)
    assert "make the same call again" in text


def test_mark_still_drops_when_not_newest_frame():
    messages = [
        session.picture_of("mark", _png(2000, 1000)),
        session.screenshot_frame(1, _png(2000, 1000)),
    ]

    session.demote_pictures(messages)

    text = session.content_text(messages[0].content)
    assert not _urls(messages[0])
    assert "make the same call again" in text


def test_thumbnail_and_crop_helpers(monkeypatch):
    big, small = _png(2000, 1000), _png(40, 40)

    thumb = session.thumbnail_png(big)
    assert session.png_size(thumb) == (800, 400)
    assert session.thumbnail_png(small) == small
    assert session.png_size(session.crop_png(big, (10, 20, 30, 40))) == (30, 40)

    monkeypatch.setattr(session, "Image", None)
    assert session.thumbnail_png(big) is None
    assert session.crop_png(big, (10, 20, 30, 40)) is None
