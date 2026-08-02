"""Tests for layer renaming (E3, 0.7.0):

Renaming edits the extra-data block, which is re-serialized on save() from
fields (Pascal name + luni), while the layer mask / blending ranges are copied
through verbatim from their preserved raw bytes. Unmodified layers keep the
exact-bytes path, so round-trip identity is unaffected.
"""
import hashlib

import psdparse


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_rename_via_property(psd_group, tmp_path):
    psd_group.layers[7].name_unicode = "改名テスト😀"
    q = _reload(psd_group, tmp_path)
    assert q.layers[7].name_unicode == "改名テスト😀"


def test_rename_via_method(psd_group, tmp_path):
    psd_group.set_layer_name(8, "method-name")
    q = _reload(psd_group, tmp_path)
    assert q.layers[8].name_unicode == "method-name"


def test_rename_leaves_pixels_and_neighbours(psd_group, tmp_path):
    h6 = hashlib.md5(psd_group.layer_image(6, "image")).hexdigest()
    h8 = hashlib.md5(psd_group.layer_image(8, "image")).hexdigest()
    psd_group.layers[7].name_unicode = "only-7"
    q = _reload(psd_group, tmp_path)
    assert hashlib.md5(q.layer_image(6, "image")).hexdigest() == h6
    assert hashlib.md5(q.layer_image(8, "image")).hexdigest() == h8


def test_rename_preserves_mask(psd_maskparams, tmp_path):
    # Renaming a masked layer must not disturb the mask (kept as raw bytes).
    before = dict(psd_maskparams.layers[0].mask)
    psd_maskparams.layers[0].name_unicode = "renamed-mask"
    q = _reload(psd_maskparams, tmp_path)
    m = q.layers[0].mask
    assert q.layers[0].name_unicode == "renamed-mask"
    assert (m["top"], m["left"], m["bottom"], m["right"]) == \
           (before["top"], before["left"], before["bottom"], before["right"])
    assert m["user_density"] == before["user_density"]
    assert abs(m["user_feather"] - before["user_feather"]) < 1e-9
    assert (m["real"] is None) == (before["real"] is None)


def test_rename_out_of_range_raises(psd_group):
    import pytest
    with pytest.raises(IndexError):
        psd_group.set_layer_name(9999, "x")


def test_renamed_layer_valid_for_psd_tools(psd_group, tmp_path):
    import pytest
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    idx = next(i for i, l in enumerate(psd_group.layers)
               if l.layer_type == psdparse.LayerType.NORMAL)
    psd_group.set_layer_name(idx, "pt-renamed😀")
    dst = tmp_path / "ren.psd"
    assert psd_group.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    assert "pt-renamed😀" in [l.name for l in pt.descendants()]
