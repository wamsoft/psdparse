"""Tests for the tree view over the flat layer list.

`PSDFile.layers` stays canonical — it is PSD's own flat order, and both drawing
order and the editing API depend on it. `roots` / `LayerInfo.children` are a
derived read-only view that hides PSD's folder encoding (the FOLDER layer above
its contents plus a `</Layer group>` divider below them).
"""
import pytest

import psdparse


def _bgra(w, h, r, g, b, a=255):
    return bytes([b, g, r, a]) * (w * h)


def _nested():
    """bg / [outer: a, [inner: b]] / top"""
    w, h = 32, 24
    p = psdparse.PSDFile()
    p.create_blank(w, h)
    p.add_layer("bg", 0, 0, _bgra(w, h, 9, 9, 9), w, h)
    p.add_layer("a", 0, 0, _bgra(w, h, 255, 0, 0), w, h)
    p.add_layer("b", 0, 0, _bgra(w, h, 0, 255, 0), w, h)
    p.add_folder("inner", 2, 1)
    p.add_folder("outer", 1, 4)
    p.add_layer("top", 0, 0, _bgra(w, h, 0, 0, 255), w, h)
    return p


def _names(p, idxs):
    return [p.layers[i].name_unicode for i in idxs]


def test_flat_list_keeps_psd_encoding():
    p = _nested()
    assert [l.name_unicode for l in p.layers] == [
        "bg", "</Layer group>", "a", "</Layer group>", "b", "inner", "outer", "top"]


def test_roots_skip_group_internals():
    p = _nested()
    assert _names(p, p.roots) == ["bg", "outer", "top"]


def test_children_are_nested():
    p = _nested()
    outer = p.roots[1]
    assert _names(p, p.layers[outer].children) == ["a", "inner"]
    inner = p.layers[outer].children[1]
    assert _names(p, p.layers[inner].children) == ["b"]


def test_dividers_never_appear_as_children():
    p = _nested()
    for i in range(len(p.layers)):
        for c in p.layers[i].children:
            assert p.layers[c].layer_type != psdparse.LayerType.HIDDEN


def test_is_group():
    p = _nested()
    groups = [l.name_unicode for l in p.layers if l.is_group]
    assert groups == ["inner", "outer"]


def test_leaf_has_no_children():
    p = _nested()
    assert p.layers[p.roots[0]].children == []


def test_children_method_matches_property():
    p = _nested()
    for i in range(len(p.layers)):
        assert p.children(i) == p.layers[i].children
    assert p.children(-1) == p.roots


def test_tree_covers_every_content_layer():
    """Walking the tree must reach every layer except the dividers."""
    p = _nested()
    seen = []

    def walk(i):
        seen.append(i)
        for c in p.layers[i].children:
            walk(c)

    for r in p.roots:
        walk(r)
    expected = [i for i, l in enumerate(p.layers)
                if l.layer_type != psdparse.LayerType.HIDDEN]
    assert sorted(seen) == expected


def test_tree_survives_roundtrip(tmp_path):
    p = _nested()
    dst = tmp_path / "tree.psd"
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    assert _names(q, q.roots) == ["bg", "outer", "top"]
    outer = q.roots[1]
    assert _names(q, q.layers[outer].children) == ["a", "inner"]


def test_tree_refreshed_after_structural_edit():
    p = _nested()
    p.add_layer("extra", 0, 0, _bgra(32, 24, 1, 2, 3), 32, 24)
    assert _names(p, p.roots) == ["bg", "outer", "top", "extra"]
