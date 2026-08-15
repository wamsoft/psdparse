"""Tests for text placement / flow box editing (0.9.0 bindings):

text_transform / set_text_transform read and write the TySh affine transform,
move_text_layer translates the transform *and* the layer (plus mask) rectangle
so the baked raster travels with the text, and text_bounds / set_text_bounds
edit the descriptor's flow box in the transform's local coordinates.
"""
import pytest

import psdparse


def _text_layer(p):
    idx = next((i for i, l in enumerate(p.layers) if l.text is not None), None)
    if idx is None:
        pytest.skip("sample has no text layer")
    return idx


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def _rect(layer):
    return (layer.left, layer.top, layer.right, layer.bottom)


def _mask_rect(layer):
    m = layer.mask
    return None if m is None else (m["left"], m["top"], m["right"], m["bottom"])


def test_text_transform_matches_layer_text(psd_textbox):
    i = _text_layer(psd_textbox)
    assert list(psd_textbox.text_transform(i)) == list(psd_textbox.layers[i].text["transform"])


def test_set_text_transform(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    psd_textbox.set_text_transform(i, (1.0, 0.0, 0.0, 1.0, 111.0, 222.0))
    q = _reload(psd_textbox, tmp_path)
    assert q.text_transform(i) == pytest.approx((1.0, 0.0, 0.0, 1.0, 111.0, 222.0))
    assert q.layers[i].text["transform"][4:] == pytest.approx([111.0, 222.0])


def test_set_text_transform_bad_length_raises(psd_textbox):
    i = _text_layer(psd_textbox)
    with pytest.raises(ValueError):
        psd_textbox.set_text_transform(i, (1.0, 0.0, 0.0, 1.0))


def test_move_text_layer_shifts_transform_and_rect(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    tf = psd_textbox.text_transform(i)
    rect = _rect(psd_textbox.layers[i])

    psd_textbox.move_text_layer(i, 40, -15)
    q = _reload(psd_textbox, tmp_path)

    assert q.text_transform(i)[4] == pytest.approx(tf[4] + 40)
    assert q.text_transform(i)[5] == pytest.approx(tf[5] - 15)
    assert _rect(q.layers[i]) == (rect[0] + 40, rect[1] - 15, rect[2] + 40, rect[3] - 15)


def test_move_text_layer_moves_mask_too(psd_textmask, tmp_path):
    i = next((k for k, l in enumerate(psd_textmask.layers)
              if l.text is not None and l.mask is not None), None)
    if i is None:
        pytest.skip("sample has no masked text layer")
    rect = _rect(psd_textmask.layers[i])
    mask = _mask_rect(psd_textmask.layers[i])

    psd_textmask.move_text_layer(i, 40, -15)
    q = _reload(psd_textmask, tmp_path, "movedmask.psd")

    assert _rect(q.layers[i]) == (rect[0] + 40, rect[1] - 15, rect[2] + 40, rect[3] - 15)
    assert _mask_rect(q.layers[i]) == (mask[0] + 40, mask[1] - 15, mask[2] + 40, mask[3] - 15)


def test_move_text_layer_round_trip(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    tf = psd_textbox.text_transform(i)
    rect = _rect(psd_textbox.layers[i])

    psd_textbox.move_text_layer(i, 25, 30)
    psd_textbox.move_text_layer(i, -25, -30)
    q = _reload(psd_textbox, tmp_path)

    assert q.text_transform(i) == pytest.approx(tf)
    assert _rect(q.layers[i]) == rect


def test_text_bounds_round_trip(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    psd_textbox.set_text_bounds(i, 0.0, 0.0, 300.0, 200.0)
    q = _reload(psd_textbox, tmp_path)
    assert q.text_bounds(i) == pytest.approx((0.0, 0.0, 300.0, 200.0))


def test_text_bounds_keeps_text(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    text_before = psd_textbox.layers[i].text["text"]
    l, t, r, b = psd_textbox.text_bounds(i)
    psd_textbox.set_text_bounds(i, l, t, r + 50, b + 50)
    q = _reload(psd_textbox, tmp_path)
    assert q.layers[i].text["text"] == text_before
    assert q.text_bounds(i)[2] == pytest.approx(r + 50)


def test_placement_non_text_raises(psd_textbox):
    idx = next((i for i, l in enumerate(psd_textbox.layers) if l.text is None), None)
    if idx is None:
        pytest.skip("all layers are text")
    with pytest.raises(RuntimeError):
        psd_textbox.text_transform(idx)
    with pytest.raises(RuntimeError):
        psd_textbox.text_bounds(idx)
    with pytest.raises(RuntimeError):
        psd_textbox.move_text_layer(idx, 1, 1)


def test_placement_out_of_range_raises(psd_textbox):
    with pytest.raises(IndexError):
        psd_textbox.text_transform(999)
    with pytest.raises(IndexError):
        psd_textbox.text_bounds(999)


def test_placement_valid_for_psd_tools(psd_textbox, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    i = _text_layer(psd_textbox)
    psd_textbox.move_text_layer(i, 30, 20)
    psd_textbox.set_text_bounds(i, 0.0, 0.0, 260.0, 180.0)
    dst = tmp_path / "placed.psd"
    assert psd_textbox.save(str(dst))
    PSDImage.open(str(dst)).composite()      # full decode must not raise
