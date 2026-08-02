"""Tests for mask pixel / geometry editing (mask revision, 0.7.0):

- set_layer_pixels must NOT drop an existing mask channel (regression),
- set_layer_mask_pixels sets the mask grayscale + rectangle (geometry),
  creating a mask when the layer had none.
"""
import pytest

import psdparse


def _bgra(w, h, r, g, b, a=255):
    return bytes([b, g, r, a]) * (w * h)


def _gray(w, h):
    return bytes([(x * 12) & 0xFF for _ in range(h) for x in range(w)])


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def _channel_ids(layer):
    return [c.id for c in layer.channels]


def test_set_layer_pixels_preserves_mask(psd_maskparams, tmp_path):
    # Regression: replacing color pixels must keep the mask channel (-2).
    assert -2 in _channel_ids(psd_maskparams.layers[0])
    psd_maskparams.set_layer_pixels(0, _bgra(16, 16, 10, 20, 30), 16, 16)
    q = _reload(psd_maskparams, tmp_path)
    assert -2 in _channel_ids(q.layers[0])
    assert q.layers[0].mask is not None


def test_set_mask_pixels_geometry(psd_group, tmp_path):
    # config.psd layers have no mask; adding one sets the rectangle.
    w, h = 20, 10
    psd_group.set_layer_mask_pixels(3, _gray(w, h), 3, 5, w, h)
    assert -2 in _channel_ids(psd_group.layers[3])
    q = _reload(psd_group, tmp_path)
    m = q.layers[3].mask
    assert m is not None
    assert (m["top"], m["left"], m["bottom"], m["right"]) == (3, 5, 13, 25)
    assert (m["width"], m["height"]) == (w, h)


def test_set_mask_pixels_roundtrip_on_matching_layer(tmp_path):
    # Build a layer whose size == mask size so layer_image('mask') is contiguous.
    p = psdparse.PSDFile()
    p.create_blank(40, 30)
    w, h = 40, 30
    p.add_layer("m", 0, 0, _bgra(w, h, 100, 150, 200), w, h)
    gray = _gray(w, h)
    p.set_layer_mask_pixels(0, gray, 0, 0, w, h)
    q = _reload(p, tmp_path)
    md = q.layer_image(0, "mask")            # BGRA, gray replicated into RGB
    got = bytes(md[i * 4 + 2] for i in range(w * h))   # R channel
    assert got == gray


def test_set_mask_pixels_bad_size_raises(psd_group):
    with pytest.raises(ValueError):
        psd_group.set_layer_mask_pixels(3, b"\x00" * 5, 0, 0, 20, 10)


def test_mask_pixels_valid_for_psd_tools(psd_group, tmp_path):
    np = pytest.importorskip("numpy")
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    w, h = 20, 10
    gray = _gray(w, h)
    psd_group.set_layer_mask_pixels(3, gray, 3, 5, w, h)
    dst = tmp_path / "mpx.psd"
    assert psd_group.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    rec = pt._record.layer_and_mask_information.layer_info.layer_records[3]
    api = next(l for l in pt.descendants() if l._record is rec)
    arr = np.array(api.mask.topil())
    assert arr.shape == (h, w)
    assert np.array_equal(arr, np.frombuffer(gray, dtype=np.uint8).reshape(h, w))
