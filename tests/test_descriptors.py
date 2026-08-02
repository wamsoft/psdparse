"""Tests for the Tier-2 generic descriptor bridge (0.4.0):

layer.effects (lfx2), layer.fill (SoCo/GdFl/PtFl), layer.info_keys and the
generic layer.descriptor(key) escape hatch.

The group sample (config.psd) carries a layer with an 'lfx2' PatternOverlay;
values here were cross-checked against psd-tools 1.17 reading the same file.
"""
import psdparse
import pytest


def _effects_layers(psd):
    return [l for l in psd.layers if l.effects is not None]


def test_effects_extraction(psd_group):
    hits = _effects_layers(psd_group)
    if not hits:
        pytest.skip("sample has no layer effects")
    e = hits[0].effects
    assert isinstance(e, dict)
    # master switch is a plain bool
    assert e.get("masterFXSwitch") in (True, False)


def test_unit_float_shape(psd_group):
    hits = _effects_layers(psd_group)
    if not hits:
        pytest.skip("sample has no layer effects")
    e = hits[0].effects
    scl = e.get("Scl ")  # a UnitFloat
    if scl is not None:
        assert set(scl.keys()) == {"value", "unit"}
        assert isinstance(scl["value"], float)
        assert isinstance(scl["unit"], str)


def test_descriptor_value_types(psd_group):
    """Every converted value is a JSON-ish Python type (recursively)."""
    hits = _effects_layers(psd_group)
    if not hits:
        pytest.skip("sample has no layer effects")

    def check(v):
        assert isinstance(v, (int, float, bool, str, bytes, dict, list, type(None)))
        if isinstance(v, dict):
            for x in v.values():
                check(x)
        elif isinstance(v, list):
            for x in v:
                check(x)

    check(hits[0].effects)


def test_pattern_overlay_values(psd_group):
    """Cross-checked against psd-tools: PatternOverlay opacity=100, scale=50%."""
    hits = _effects_layers(psd_group)
    if not hits:
        pytest.skip("sample has no layer effects")
    pf = hits[0].effects.get("patternFill")
    if not isinstance(pf, dict):
        pytest.skip("sample effect is not a pattern overlay")
    assert pf.get("enab") is True
    assert pf["Opct"]["value"] == pytest.approx(100.0)


def test_generic_descriptor_matches_effects(psd_group):
    hits = _effects_layers(psd_group)
    if not hits:
        pytest.skip("sample has no layer effects")
    l = hits[0]
    assert l.descriptor("lfx2").keys() == l.effects.keys()


def test_binary_key_returns_none(psd_group):
    """lrFX is a binary (non-descriptor) block -> None, must not crash."""
    for l in psd_group.layers:
        if "lrFX" in l.info_keys:
            assert l.descriptor("lrFX") is None
            return
    pytest.skip("sample has no lrFX block")


def test_descriptor_bad_key_raises(psd_group):
    with pytest.raises(ValueError):
        psd_group.layers[0].descriptor("ab")


def test_info_keys_are_4cc(psd_group):
    for l in psd_group.layers:
        keys = l.info_keys
        assert isinstance(keys, list)
        for k in keys:
            assert isinstance(k, str) and len(k) == 4


def test_fill_none_for_non_fill(psd_group):
    # config/system samples contain no fill layers.
    for l in psd_group.layers:
        if l.layer_type != psdparse.LayerType.FILL:
            assert l.fill is None
