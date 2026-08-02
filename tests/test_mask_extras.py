"""Tests for the 0.6.0 mask/label extras:

- layer mask density/feather parameters and the (fixed) real-mask section,
- the 'lclr' layer-panel color label (layer.sheet_color),
- document-level global layer mask info.

The maskparams.psd fixture was authored with psd-tools so the expected values
(bbox, density=128, feather=2.5, real rect 0..16, lclr=5) are known exactly.
"""
import psdparse


def test_mask_parameters(psd_maskparams):
    m = psd_maskparams.layers[0].mask
    assert m is not None
    assert (m["top"], m["left"], m["bottom"], m["right"]) == (1, 2, 13, 14)
    assert m["has_parameters"] is True
    assert m["user_density"] == 128
    assert abs(m["user_feather"] - 2.5) < 1e-9
    # only user density/feather were authored; vector ones stay absent
    assert m["vector_density"] is None
    assert m["vector_feather"] is None


def test_mask_real_section(psd_maskparams):
    # Regression: the real/user-mask section used to be decoded one byte off.
    m = psd_maskparams.layers[0].mask
    real = m["real"]
    assert real is not None
    assert (real["top"], real["left"], real["bottom"], real["right"]) == (0, 0, 16, 16)
    assert real["background"] == 0


def test_plain_mask_has_no_parameters(psd_mask):
    # masktest.psd carries a simple 20-byte mask: no params, no real section.
    m = psd_mask.layers[0].mask
    assert m is not None
    assert m["has_parameters"] is False
    assert m["user_density"] is None and m["user_feather"] is None
    assert m["real"] is None


def test_sheet_color(psd_maskparams):
    sc = psd_maskparams.layers[0].sheet_color
    assert sc == {"index": 5, "name": "blue"}


def test_sheet_color_none_when_zero(psd_group):
    # config.psd layers carry lclr = 0 (explicit "no color").
    sc = psd_group.layers[0].sheet_color
    assert sc is None or sc["index"] == 0


def test_global_layer_mask_shape(psd_group):
    # Most PSDs have an empty global layer mask block -> None. When present it
    # is a dict with the documented keys.
    g = psd_group.global_layer_mask
    if g is None:
        return
    assert set(g) >= {"overlay_color_space", "color", "opacity", "kind"}
    assert len(g["color"]) == 4
