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


# --- テキストレイヤ + マスク -------------------------------------------------
# 多言語版を作るときは「テキストレイヤを言語ぶん複製し、書き出し範囲をマスクで
# 切る」形になりやすい。複製や本文の書き換えでマスクが落ちると、書き出しの
# たびに手でマスクを付け直すことになるので、ここで押さえておく。

def _mask_channel(layer):
    for c in layer.channels:
        if c.id == -2:
            return c
    return None


def test_textmask_sample_has_masked_text_layers(psd_textmask):
    for i in (1, 2):
        assert psd_textmask.layers[i].text, "layer %d should be a text layer" % i
        assert psd_textmask.layers[i].mask, "layer %d should carry a mask" % i


def test_duplicate_keeps_mask(psd_textmask, tmp_path):
    src = psd_textmask.layers[1]
    before = dict(src.mask)
    before_len = _mask_channel(src).length

    ni = psd_textmask.duplicate_layer(1)
    assert ni >= 0
    q = _reload(psd_textmask, tmp_path, "dup.psd")

    copy = q.layers[ni]
    assert copy.text, "the duplicate should still be a text layer"
    m = copy.mask
    assert m, "the duplicate lost its mask"
    for k in ("top", "left", "bottom", "right", "default_color"):
        assert m[k] == before[k], "mask %s changed on duplicate" % k
    assert _mask_channel(copy).length == before_len, "mask pixels changed on duplicate"


def test_set_text_keeps_mask(psd_textmask, tmp_path):
    before = dict(psd_textmask.layers[1].mask)
    before_len = _mask_channel(psd_textmask.layers[1]).length

    psd_textmask.set_text(1, "translated")
    q = _reload(psd_textmask, tmp_path, "txt.psd")

    m = q.layers[1].mask
    assert m, "editing the text dropped the mask"
    for k in ("top", "left", "bottom", "right"):
        assert m[k] == before[k]
    assert _mask_channel(q.layers[1]).length == before_len
