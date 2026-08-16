"""Tests for PSDFile.add_folder — building layer groups from scratch.

A PSD group is two marker layers around the contents: a `</Layer group>`
divider (LayerType.HIDDEN) below and the folder layer (LayerType.FOLDER)
above, both with an empty rect. add_folder inserts that pair.
"""
import pytest

import psdparse


def _bgra(w, h, r, g, b, a=255):
    return bytes([b, g, r, a]) * (w * h)


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def _build(w=64, h=48):
    p = psdparse.PSDFile()
    p.create_blank(w, h)
    p.add_layer("bg", 0, 0, _bgra(w, h, 200, 200, 200), w, h)
    p.add_layer("child1", 0, 0, _bgra(w, h, 255, 0, 0), w, h)
    p.add_layer("child2", 0, 0, _bgra(w, h, 0, 255, 0), w, h, "mul ")
    return p


def test_add_folder_inserts_marker_pair():
    p = _build()
    f = p.add_folder("group", 1, 2)
    # divider goes below the contents, folder layer above them
    assert f == 4
    assert len(p.layers) == 5
    assert p.layers[1].layer_type == psdparse.LayerType.HIDDEN
    assert p.layers[1].name_unicode == "</Layer group>"
    assert p.layers[4].layer_type == psdparse.LayerType.FOLDER
    assert p.layers[4].name_unicode == "group"
    # markers carry no pixels
    for i in (1, 4):
        assert (p.layers[i].left, p.layers[i].top,
                p.layers[i].right, p.layers[i].bottom) == (0, 0, 0, 0)


def test_add_folder_relinks_parents():
    p = _build()
    f = p.add_folder("group", 1, 2)
    assert [l.parent_index for l in p.layers] == [-1, f, f, f, -1]


def test_add_folder_roundtrips(tmp_path):
    p = _build()
    p.add_folder("グループ", 1, 2, closed=True, blend_mode="pass")
    q = _reload(p, tmp_path)
    assert len(q.layers) == 5
    assert q.layers[1].layer_type == psdparse.LayerType.HIDDEN
    assert q.layers[4].layer_type == psdparse.LayerType.FOLDER
    assert q.layers[4].name_unicode == "グループ"
    assert [l.parent_index for l in q.layers] == [-1, 4, 4, 4, -1]
    # contents survive untouched
    assert q.layer_image(2, "image") == _bgra(64, 48, 255, 0, 0)
    assert q.layer_image(3, "image") == _bgra(64, 48, 0, 255, 0)


def test_add_folder_keeps_blend_mode(tmp_path):
    p = _build()
    p.add_folder("multiply group", 1, 2, blend_mode="mul ")
    q = _reload(p, tmp_path)
    assert q.layers[4].blend_mode == psdparse.BlendMode.MULTIPLY


def test_add_folder_nesting(tmp_path):
    p = _build()
    p.add_folder("inner", 2, 1)          # wrap child2 alone -> indices 2..4
    p.add_folder("outer", 1, 4)          # wrap child1 + the inner group
    q = _reload(p, tmp_path)
    names = [l.name_unicode for l in q.layers]
    types = [str(l.layer_type).rsplit(".", 1)[-1] for l in q.layers]
    assert names[-1] == "outer" and types[-1] == "FOLDER"
    assert "inner" in names
    # every layer inside the outer group resolves to a parent
    outer = names.index("outer")
    assert all(l.parent_index >= 0 for l in q.layers[1:outer])


def test_add_folder_empty(tmp_path):
    p = _build()
    f = p.add_folder("empty", 3, 0)
    assert f == 4
    q = _reload(p, tmp_path)
    assert q.layers[3].layer_type == psdparse.LayerType.HIDDEN
    assert q.layers[4].layer_type == psdparse.LayerType.FOLDER


def test_add_folder_rejects_bad_range():
    p = _build()
    with pytest.raises(RuntimeError):
        p.add_folder("bad", 2, 99)
    with pytest.raises(RuntimeError):
        p.add_folder("bad", -1, 1)


def test_folder_valid_for_psd_tools(tmp_path):
    """Cross-check the group structure with an independent PSD reader."""
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    p = _build()
    p.add_folder("group", 1, 2)
    dst = tmp_path / "folder.psd"
    assert p.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    groups = [l for l in pt.descendants() if l.is_group()]
    assert [g.name for g in groups] == ["group"]
    assert sorted(l.name for l in groups[0]) == ["child1", "child2"]
