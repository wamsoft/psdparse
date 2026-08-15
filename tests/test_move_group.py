"""Tests for folder-aware layer moves (0.9.0 bindings):

group_span reports the flat-list block a folder occupies, move_layer_sibling
swaps a layer (or a whole folder) with its next sibling at the same level, and
move_layer_range relocates an arbitrary block. parent_index is re-linked after
every structural edit.
"""
import pytest

import psdparse


def _names(p):
    return [l.name_unicode for l in p.layers]


def _parents(p):
    return [l.parent_index for l in p.layers]


def _first_folder(p):
    idx = next((i for i, l in enumerate(p.layers)
                if l.layer_type == psdparse.LayerType.FOLDER), None)
    if idx is None:
        pytest.skip("sample has no layer folders")
    return idx


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_group_span_leaf_is_itself(psd_group):
    i = next(k for k, l in enumerate(psd_group.layers)
             if l.layer_type == psdparse.LayerType.NORMAL)
    assert psd_group.group_span(i) == (i, 1)


def test_group_span_folder_covers_divider_and_contents(psd_group):
    f = _first_folder(psd_group)
    start, count = psd_group.group_span(f)
    assert start + count - 1 == f                     # the folder closes the block
    assert psd_group.layers[start].layer_type == psdparse.LayerType.HIDDEN
    # everything between the divider and the folder belongs to it
    for i in range(start + 1, f):
        assert psd_group.layers[i].parent_index == f or psd_group.layers[i].parent_index > start


def test_group_span_out_of_range_raises(psd_group):
    with pytest.raises(IndexError):
        psd_group.group_span(999)


def test_move_layer_sibling_round_trip(psd_group):
    before = _names(psd_group)
    f = _first_folder(psd_group)

    new_index = psd_group.move_layer_sibling(f, up=True)
    assert new_index is not None
    assert psd_group.layers[new_index].name_unicode == before[f]
    assert _names(psd_group) != before

    assert psd_group.move_layer_sibling(new_index, up=False) == f
    assert _names(psd_group) == before


def test_move_layer_sibling_keeps_folder_contents(psd_group):
    f = _first_folder(psd_group)
    start, count = psd_group.group_span(f)
    contents = _names(psd_group)[start:start + count]

    new_index = psd_group.move_layer_sibling(f, up=True)
    if new_index is None:
        pytest.skip("folder is already at the top of its level")
    new_start, new_count = psd_group.group_span(new_index)
    assert new_count == count
    assert _names(psd_group)[new_start:new_start + new_count] == contents


def test_move_layer_sibling_stays_in_its_folder(psd_group):
    """A child never leaves its folder, and its parent is unchanged."""
    f = _first_folder(psd_group)
    child = next((i for i, l in enumerate(psd_group.layers) if l.parent_index == f), None)
    if child is None:
        pytest.skip("folder has no children")

    parent_before = psd_group.layers[child].parent_index
    moved = 0
    while True:
        new_index = psd_group.move_layer_sibling(child, up=True)
        if new_index is None:            # hit the top of this level
            break
        assert psd_group.layers[new_index].parent_index == parent_before
        child = new_index
        moved += 1
        if moved > 50:
            pytest.fail("move_layer_sibling never reports the end of the level")


def test_move_layer_sibling_reports_none_at_the_edges(psd_group):
    before = _names(psd_group)
    assert psd_group.move_layer_sibling(len(psd_group.layers) - 1, up=True) is None
    assert psd_group.move_layer_sibling(0, up=False) is None
    assert _names(psd_group) == before


def test_move_layer_sibling_out_of_range_raises(psd_group):
    with pytest.raises(IndexError):
        psd_group.move_layer_sibling(999)


def test_move_layer_range_moves_the_block(psd_group):
    f = _first_folder(psd_group)
    start, count = psd_group.group_span(f)
    block = _names(psd_group)[start:start + count]

    psd_group.move_layer_range(start, count, 0)
    assert _names(psd_group)[:count] == block


def test_move_layer_range_bad_args_raise(psd_group):
    with pytest.raises(IndexError):
        psd_group.move_layer_range(0, 0, 5)
    with pytest.raises(IndexError):
        psd_group.move_layer_range(0, len(psd_group.layers) + 1, 1)


def test_parent_index_relinked_after_structural_edits(psd_group):
    """parent_index used to keep the values computed at load time."""
    f = _first_folder(psd_group)
    start, count = psd_group.group_span(f)

    psd_group.move_layer_range(start, count, 0)
    # the folder now sits at index count-1 with its children below it
    folder = count - 1
    assert psd_group.layers[folder].layer_type == psdparse.LayerType.FOLDER
    for i in range(1, folder):
        assert psd_group.layers[i].parent_index == folder
    assert psd_group.layers[folder].parent_index == -1


def test_hierarchy_survives_save_reload(psd_group, tmp_path):
    f = _first_folder(psd_group)
    if psd_group.move_layer_sibling(f, up=True) is None:
        pytest.skip("folder is already at the top of its level")

    parents = _parents(psd_group)
    names = _names(psd_group)
    q = _reload(psd_group, tmp_path, "moved.psd")
    assert _names(q) == names
    assert _parents(q) == parents


def test_moved_folder_valid_for_psd_tools(psd_group, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    f = _first_folder(psd_group)
    if psd_group.move_layer_sibling(f, up=True) is None:
        pytest.skip("folder is already at the top of its level")
    dst = tmp_path / "group.psd"
    assert psd_group.save(str(dst))
    PSDImage.open(str(dst)).composite()      # full decode must not raise
