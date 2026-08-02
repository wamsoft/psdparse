"""Tests for the Tier-1 reference metadata exposed in 0.3.0:

layer hierarchy (parent_index), layer mask, blending ranges, and the
document-level guides / slices / layer comps / color table accessors.
"""
import psdparse


# --- layer hierarchy -------------------------------------------------------

def test_parent_index_valid(psd_group):
    """Every parent_index is either -1 (top level) or points at a FOLDER."""
    layers = psd_group.layers
    tops = children = 0
    for i, l in enumerate(layers):
        pi = l.parent_index
        assert pi == -1 or 0 <= pi < len(layers), (i, pi)
        if pi == -1:
            tops += 1
        else:
            children += 1
            assert layers[pi].layer_type == psdparse.LayerType.FOLDER, \
                f"parent of layer {i} (index {pi}) is not a FOLDER"
    assert tops > 0
    assert children > 0  # the group sample is nested


def test_tree_reconstruction(psd_group):
    """parent_index lets us rebuild children lists that partition the layers."""
    layers = psd_group.layers
    children = {i: [] for i in range(len(layers))}
    roots = []
    for i, l in enumerate(layers):
        (roots if l.parent_index == -1 else children[l.parent_index]).append(i)
    total = len(roots) + sum(len(v) for v in children.values())
    assert total == len(layers)


# --- layer mask ------------------------------------------------------------

def test_mask_none_when_absent(psd_group):
    # Most layers in the sample have no mask -> None.
    assert any(l.mask is None for l in psd_group.layers)


def test_mask_dict_shape(psd_mask):
    masked = [l for l in psd_mask.layers if l.mask is not None]
    assert len(masked) >= 1
    m = masked[0].mask
    # geometry is self-consistent
    assert m["width"] == m["right"] - m["left"]
    assert m["height"] == m["bottom"] - m["top"]
    assert 0 <= m["default_color"] <= 255
    for k in ("relative", "disabled", "inverted", "from_render", "has_parameters"):
        assert isinstance(m[k], bool)
    # synthesized fixture: 40x32 mask at (left=10, top=8), not disabled
    assert (m["width"], m["height"]) == (40, 32)
    assert m["disabled"] is False


# --- blending ranges -------------------------------------------------------

def test_blending_ranges_shape(psd_group):
    ranged = [l for l in psd_group.layers if l.blending_ranges is not None]
    assert len(ranged) > 0
    br = ranged[0].blending_ranges
    assert len(br["gray"]) == 2
    for pair in br["channels"]:
        assert len(pair) == 2


# --- document resources ----------------------------------------------------

def test_guides_accessor(psd_group):
    g = psd_group.guides
    if g is None:
        return  # sample may lack a grid/guides resource
    assert "horizontal_grid" in g and "vertical_grid" in g
    for gd in g["guides"]:
        assert gd["direction"] in ("vertical", "horizontal")


def test_slices_accessor(psd_group):
    s = psd_group.slices
    if s is None:
        return
    assert "bounding" in s and "slices" in s
    for it in s["slices"]:
        assert it["right"] >= it["left"]
        assert it["bottom"] >= it["top"]
        assert len(it["color"]) == 4


def test_layer_comps_is_list(psd_group):
    comps = psd_group.layer_comps
    assert isinstance(comps, list)
    for c in comps:
        assert "id" in c and "name" in c


def test_color_table_none_for_rgb(psd_group):
    # The group samples are RGB, so there is no indexed palette.
    assert psd_group.header.mode == psdparse.COLOR_MODE_RGB
    assert psd_group.color_table is None
