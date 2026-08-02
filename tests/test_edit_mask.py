"""Tests for editing extra-data values (E3 remainder, 0.7.0):

- layer mask values: disabled / density / feather / default_color
  (the mask rectangle and pixels are unchanged; the block is re-serialized),
- fill opacity (the 'iOpa' block).

maskparams.psd carries a real+parameters mask, so edits must preserve the
untouched parts (rectangle, real section).
"""
import pytest

import psdparse


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_set_mask_disabled(psd_maskparams, tmp_path):
    psd_maskparams.set_layer_mask(0, disabled=True)
    q = _reload(psd_maskparams, tmp_path)
    assert q.layers[0].mask["disabled"] is True


def test_set_mask_density_feather(psd_maskparams, tmp_path):
    psd_maskparams.set_layer_mask(0, density=100, feather=5.5)
    q = _reload(psd_maskparams, tmp_path)
    m = q.layers[0].mask
    assert m["user_density"] == 100
    assert abs(m["user_feather"] - 5.5) < 1e-9


def test_set_mask_default_color(psd_maskparams, tmp_path):
    psd_maskparams.set_layer_mask(0, default_color=200)
    q = _reload(psd_maskparams, tmp_path)
    assert q.layers[0].mask["default_color"] == 200


def test_mask_edit_preserves_geometry_and_real(psd_maskparams, tmp_path):
    before = dict(psd_maskparams.layers[0].mask)
    psd_maskparams.set_layer_mask(0, density=42)
    q = _reload(psd_maskparams, tmp_path)
    m = q.layers[0].mask
    assert (m["top"], m["left"], m["bottom"], m["right"]) == \
           (before["top"], before["left"], before["bottom"], before["right"])
    # real section is preserved verbatim
    assert (m["real"] is None) == (before["real"] is None)
    if before["real"] is not None:
        assert (m["real"]["top"], m["real"]["left"], m["real"]["bottom"], m["real"]["right"]) == \
               (before["real"]["top"], before["real"]["left"],
                before["real"]["bottom"], before["real"]["right"])


def test_set_mask_on_maskless_layer_raises(psd_group):
    # config.psd layers have no masks.
    with pytest.raises(RuntimeError):
        psd_group.set_layer_mask(0, disabled=True)


def test_set_mask_no_args_raises(psd_maskparams):
    with pytest.raises(ValueError):
        psd_maskparams.set_layer_mask(0)


def test_mask_edit_valid_for_psd_tools(psd_maskparams, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    psd_maskparams.set_layer_mask(0, disabled=True, density=100, feather=5.5, default_color=200)
    dst = tmp_path / "m.psd"
    assert psd_maskparams.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    md = pt._record.layer_and_mask_information.layer_info.layer_records[0].mask_data
    assert md.flags.mask_disabled is True
    assert md.parameters.user_mask_density == 100
    assert abs(md.parameters.user_mask_feather - 5.5) < 1e-9
    assert md.background_color == 200


def test_fill_opacity_writable(psd_group, tmp_path):
    psd_group.layers[7].fill_opacity = 111
    q = _reload(psd_group, tmp_path)
    assert q.layers[7].fill_opacity == 111


def test_fill_opacity_valid_for_psd_tools(psd_group, tmp_path):
    pytest.importorskip("psd_tools")
    from psd_tools import PSDImage
    from psd_tools.constants import Tag
    psd_group.layers[7].fill_opacity = 88
    dst = tmp_path / "fo.psd"
    assert psd_group.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    rec = pt._record.layer_and_mask_information.layer_info.layer_records[7]
    assert rec.tagged_blocks.get_data(Tag.BLEND_FILL_OPACITY) == 88
