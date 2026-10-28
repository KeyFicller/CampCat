"""Pictures in the message list: screenshots and small crops are counted apart.

A crop must not push the screenshot it was cut from out of the request, and
crops are cleared in a batch so a new one does not rewrite the prefix the
provider caches.
"""

import base64
import copy
import io

import session
from langchain_core.messages import AIMessage, ToolMessage
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

    # Still there, at full size: a crop added around it must not evict it.
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
        assert "把同一个调用再发一次" in text
        assert 'view_crop {"region":' in text


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
    assert "把同一个调用再发一次" in text


def test_a_scaled_frame_says_what_to_multiply_by():
    # The multiplier is the point: reading a box off a scaled picture and
    # rescaling it by hand was the largest block of thinking in a smoke run,
    # and every reading of the same frame came out slightly different.
    frame = session.screenshot_frame(7, _png(2000, 1000))
    messages = [
        frame, session.screenshot_frame(8, _png(2000, 1000)),
    ]
    session.demote_pictures(messages)
    text = session.content_text(messages[0].content)
    assert "坐标仍是帧像素 2000x1000" in text
    assert "×2.50" in text            # 2000 / 800, computed, not written down
    assert "view_shot(7)" in text


def test_a_full_size_frame_says_it_is_full_size():
    # A frame that is not scaled said nothing at all, so the model could not
    # tell whether it was reading the real pixels or a thumbnail.
    frame = session.screenshot_frame(9, _png(400, 200))
    assert "就是原尺寸" in session.content_text(frame.content)
    assert "×" not in session.content_text(frame.content)


def test_a_batch_of_look_backs_keeps_its_pictures():
    # Four at once is what the scripter actually asked for. The oldest used to
    # arrive already dropped -- its caption then said to call view_shot(n) again,
    # which is why one frame was asked for seven times in a single handoff.
    frame = _png(2000, 1000)
    messages = [session.recall_frame(n, frame) for n in range(1, 5)]

    session.demote_pictures(messages)

    assert all(_urls(m) for m in messages)


def test_look_backs_do_not_evict_the_live_screen():
    # Demotion runs every round, so a look-back that slipped into the screenshot
    # window would push out the very frame a tap's box is measured on.
    shot = session.screenshot_frame(9, _png(2000, 1000))
    messages = [shot] + [session.recall_frame(n, _png(2000, 1000))
                         for n in range(1, 7)]

    session.demote_pictures(messages)
    session.demote_pictures(messages)

    assert _urls(messages[0])
    assert session.png_size(_urls(messages[0])[0].partition(",")[2]) == (2000, 1000)


def test_asking_for_the_same_frame_twice_keeps_only_the_new_one():
    # The stale copy kept the "call view_shot(N) again" note, which the model
    # read as its own failed call and answered by asking again: one run made 42
    # such calls for 10 frames.
    frame = _png(2000, 1000)
    messages = [session.recall_frame(3, frame) for _ in range(3)]

    session.demote_pictures(messages)

    assert len(messages) == 1
    assert _urls(messages[0])
    assert "坐标仍是帧像素 2000x1000" in session.content_text(messages[0].content)
    assert "图片未附上" not in session.content_text(messages[0].content)


def test_an_old_screenshot_copy_does_not_survive_its_look_back():
    # The stale copy's note says to call view_shot(1) again, and it sits above
    # the fresh picture of that same frame. One run's scripter read it as its
    # own failed call and asked five more times.
    frame = _png(2000, 1000)
    messages = [session.screenshot_frame(n, frame) for n in (1, 2, 3, 4)]
    messages.append(session.recall_frame(1, frame))

    session.demote_pictures(messages)

    assert sum("#1" in session.content_text(m.content) for m in messages) == 1


def test_an_assistant_turn_that_names_a_frame_is_not_a_copy_of_it():
    # It carries `tool_calls`, so dropping it left its tool result with nothing
    # above it and the provider rejected the whole request (400, "Messages with
    # role 'tool' must be a response to a preceding message with 'tool_calls'").
    frame = _png(2000, 1000)
    messages = [
        session.recall_frame(15, frame),
        AIMessage(content="看到 #15 是设置页",
                  tool_calls=[{"name": "view_shot", "args": {"shot": 15},
                               "id": "c"}]),
        ToolMessage(content="ok", tool_call_id="c"),
        session.recall_frame(15, frame),
    ]

    session.demote_pictures(messages)

    assert any(isinstance(m, AIMessage) for m in messages)
    assert any(isinstance(m, ToolMessage) for m in messages)


def test_mark_still_drops_when_not_newest_frame():
    messages = [
        session.picture_of("mark", _png(2000, 1000)),
        session.screenshot_frame(1, _png(2000, 1000)),
    ]

    session.demote_pictures(messages)

    text = session.content_text(messages[0].content)
    assert not _urls(messages[0])
    assert "把同一个调用再发一次" in text


def test_thumbnail_and_crop_helpers(monkeypatch):
    big, small = _png(2000, 1000), _png(40, 40)

    thumb = session.thumbnail_png(big)
    assert session.png_size(thumb) == (800, 400)
    assert session.thumbnail_png(small) == small
    assert session.png_size(session.crop_png(big, (10, 20, 30, 40))) == (30, 40)

    monkeypatch.setattr(session, "Image", None)
    assert session.thumbnail_png(big) is None
    assert session.crop_png(big, (10, 20, 30, 40)) is None
